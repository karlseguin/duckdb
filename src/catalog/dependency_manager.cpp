#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/catalog/mapping_value.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/enums/catalog_type.hpp"
#include "duckdb/catalog/catalog_entry/dependency_set_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/dependency_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/duck_schema_entry.hpp"
#include "duckdb/catalog/mapping_value.hpp"

namespace duckdb {

DependencyManager::DependencyManager(DuckCatalog &catalog) : catalog(catalog), connections(catalog) {
}

static string GetSchema(CatalogEntry &entry) {
	if (entry.type == CatalogType::SCHEMA_ENTRY) {
		return entry.name;
	}
	return entry.ParentSchema().name;
}

string DependencyManager::MangleName(CatalogType type, const string &schema, const string &name) {
	auto null_byte = string(1, '\0');
	return CatalogTypeToString(type) + null_byte + schema + null_byte + name;
}

string DependencyManager::MangleName(CatalogEntry &entry) {
	CatalogType type = CatalogType::INVALID;
	string schema;
	string name;

	if (entry.type == CatalogType::DEPENDENCY_ENTRY) {
		auto &dependency_entry = entry.Cast<DependencyCatalogEntry>();
		return dependency_entry.MangledName();
	} else if (entry.type == CatalogType::DEPENDENCY_SET) {
		auto &dependency_set = entry.Cast<DependencySetCatalogEntry>();
		return dependency_set.MangledName();
	} else {
		type = entry.type;
		schema = GetSchema(entry);
		name = entry.name;
	}
	D_ASSERT(type != CatalogType::INVALID);
	return MangleName(type, schema, name);
}

optional_ptr<DependencySetCatalogEntry> DependencyManager::GetDependencySet(CatalogEntry &object) {
	D_ASSERT(object.type != CatalogType::DEPENDENCY_SET);
	auto name = MangleName(object);
	auto mapping = connections.GetLatestMapping(name);
	if (!mapping) {
		return nullptr;
	}
	auto it = connections.entries.find(mapping->index.GetIndex());
	if (it == connections.entries.end()) {
		return nullptr;
	}
	auto &entry_value = it->second;
	auto &dependency_set_entry = entry_value.Entry();

	D_ASSERT(dependency_set_entry.type == CatalogType::DEPENDENCY_SET);
	return dynamic_cast<DependencySetCatalogEntry *>(&dependency_set_entry);
}

optional_ptr<DependencySetCatalogEntry> DependencyManager::GetDependencySet(CatalogTransaction transaction,
                                                                            CatalogEntry &object) {
	auto name = MangleName(object);
	auto connection_p = connections.GetEntry(transaction, name);
	if (!connection_p) {
		return nullptr;
	}
	D_ASSERT(connection_p->type == CatalogType::DEPENDENCY_SET);
	return dynamic_cast<DependencySetCatalogEntry *>(connection_p.get());
}

DependencySetCatalogEntry &DependencyManager::GetOrCreateDependencySet(CatalogTransaction transaction,
                                                                       CatalogEntry &object) {
	auto name = MangleName(object);
	auto connection_p = connections.GetEntry(transaction, name);
	if (!connection_p) {
		auto new_connection = make_uniq<DependencySetCatalogEntry>(catalog, name);
		if (catalog.IsTemporaryCatalog()) {
			new_connection->temporary = true;
		}
		auto &connection = *new_connection;
		DependencyList empty_dependencies;
		auto res = connections.CreateEntry(transaction, name, std::move(new_connection), empty_dependencies);
		(void)res;
		D_ASSERT(res);
		return connection;
	}
	D_ASSERT(connection_p->type == CatalogType::DEPENDENCY_SET);
	return connection_p->Cast<DependencySetCatalogEntry>();
}

bool DependencyManager::IsSystemEntry(CatalogEntry &entry) const {
	if (entry.internal) {
		return true;
	}

	switch (entry.type) {
	case CatalogType::DEPENDENCY_ENTRY:
	case CatalogType::DEPENDENCY_SET:
	case CatalogType::DATABASE_ENTRY:
		return true;
	default:
		return false;
	}
}

void DependencyManager::AddObject(CatalogTransaction transaction, CatalogEntry &object, DependencyList &dependencies) {
	if (IsSystemEntry(object)) {
		// Don't do anything for this
		return;
	}

	// check for each object in the sources if they were not deleted yet
	for (auto &dep : dependencies.set) {
		auto &dependency = dep.get();
		if (&dependency.ParentCatalog() != &object.ParentCatalog()) {
			throw DependencyException(
			    "Error adding dependency for object \"%s\" - dependency \"%s\" is in catalog "
			    "\"%s\", which does not match the catalog \"%s\".\nCross catalog dependencies are not supported.",
			    object.name, dependency.name, dependency.ParentCatalog().GetName(), object.ParentCatalog().GetName());
		}
		if (!dependency.set) {
			throw InternalException("Dependency has no set");
		}
		auto catalog_entry = dependency.set->GetEntryInternal(transaction, dependency.name, nullptr);
		if (!catalog_entry) {
			throw InternalException("Dependency has already been deleted?");
		}
	}

	// indexes do not require CASCADE to be dropped, they are simply always dropped along with the table
	auto dependency_type = object.type == CatalogType::INDEX_ENTRY ? DependencyType::DEPENDENCY_AUTOMATIC
	                                                               : DependencyType::DEPENDENCY_REGULAR;
	// add the object to the dependents_map of each object that it depends on
	for (auto &dependency : dependencies.set) {
		// NEW
		auto &dependency_connections = GetOrCreateDependencySet(transaction, dependency);
		dependency_connections.AddDependent(transaction, object, dependency_type);
	}
	// create the dependents map for this object: it starts out empty

	auto &object_connections = GetOrCreateDependencySet(transaction, object);
	object_connections.AddDependencies(transaction, dependencies);
}

static bool CascadeDrop(bool cascade, DependencyType dependency_type) {
	if (cascade) {
		return true;
	}
	if (dependency_type == DependencyType::DEPENDENCY_AUTOMATIC) {
		// These dependencies are automatically dropped implicitly
		return true;
	}
	if (dependency_type == DependencyType::DEPENDENCY_OWNS) {
		// The object has explicit ownership over the dependency
		return true;
	}
	return false;
}

void DependencyManager::UnmangleName(const string &mangled, CatalogType &type, string &schema, string &name) {
	auto parts = StringUtil::Split(mangled, std::string("\0", 1));
	D_ASSERT(parts.size() == 3);
	type = CatalogTypeFromString(parts[0]);
	schema = std::move(parts[1]);
	name = std::move(parts[2]);
}

void GetLookupProperties(CatalogEntry &entry, string &schema, string &name, CatalogType &type) {
	if (entry.type == CatalogType::DEPENDENCY_ENTRY) {
		auto &dependency_entry = entry.Cast<DependencyCatalogEntry>();

		schema = dependency_entry.EntrySchema();
		name = dependency_entry.EntryName();
		type = dependency_entry.EntryType();
	} else if (entry.type == CatalogType::DEPENDENCY_SET) {
		auto &dependency_set = entry.Cast<DependencySetCatalogEntry>();

		auto &mangled_name = dependency_set.MangledName();
		DependencyManager::UnmangleName(mangled_name, type, schema, name);
	} else {
		throw InternalException("Unrecognized CatalogType in 'GetLookupProperties'");
	}
}

DependencyManager::LookupResult::LookupResult(optional_ptr<CatalogEntry> entry) : entry(entry) {
}
DependencyManager::LookupResult::LookupResult(optional_ptr<CatalogSet> set, optional_ptr<MappingValue> mapping_value,
                                              optional_ptr<CatalogEntry> entry)
    : set(set), mapping_value(mapping_value), entry(entry) {
}

// Always performs the callback, it's up to the callback to determine what to do based on the lookup result
DependencyManager::LookupResult DependencyManager::LookupEntry(CatalogTransaction transaction,
                                                               CatalogEntry &dependency) {
	string schema;
	string name;
	CatalogType type;
	GetLookupProperties(dependency, schema, name, type);

	// Lookup the schema
	EntryIndex index;
	// Use 'GetEntryInternal' because we don't care if the schema is deleted
	catalog.schemas->GetEntryInternal(transaction, schema, &index);
	D_ASSERT(index.IsValid());
	auto &schema_entry = index.GetEntry();
	if (type == CatalogType::SCHEMA_ENTRY) {
		// This is a schema entry, perform the callback only providing the schema
		auto entry = dynamic_cast<CatalogEntry *>(&schema_entry);
		return LookupResult(entry);
	}
	auto &duck_schema_entry = schema_entry.Cast<DuckSchemaEntry>();

	// Lookup the catalog set
	auto &catalog_set = duck_schema_entry.GetCatalogSet(type);

	// Get the mapping from name -> index
	auto mapping_value = catalog_set.GetMapping(transaction, name, /* get_latest = */ true);
	if (!mapping_value) {
		return LookupResult(&catalog_set, nullptr, nullptr);
	}
	// Use the index to find the actual entry
	auto entry = catalog_set.GetEntryInternal(transaction, mapping_value->index);
	return LookupResult(&catalog_set, mapping_value, entry);
}

void DependencyManager::CleanupDependencies(CatalogTransaction transaction, CatalogEntry &object) {
	auto connections_p = GetDependencySet(transaction, object);
	D_ASSERT(connections_p);
	auto &connections = *connections_p;

	// Collect the dependencies
	catalog_entry_set_t dependencies_to_remove;
	connections.Dependencies().Scan([&](CatalogEntry &other) { dependencies_to_remove.insert(other); });
	// Also collect the dependents
	catalog_entry_set_t dependents_to_remove;
	connections.Dependents().Scan([&](CatalogEntry &other) { dependents_to_remove.insert(other); });

	// Remove the dependency entries
	for (auto &dependency : dependencies_to_remove) {
		auto other_connections_p = GetDependencySet(transaction, dependency);
		auto &other_connections = *other_connections_p;

		other_connections.RemoveDependent(transaction, connections);
		connections.RemoveDependency(transaction, dependency);
	}
	// Remove the dependent entries
	for (auto &dependent : dependents_to_remove) {
		auto other_connections_p = GetDependencySet(transaction, dependent);
		auto &other_connections = *other_connections_p;

		other_connections.RemoveDependency(transaction, connections);
		connections.RemoveDependent(transaction, dependent);
	}
}

void DependencyManager::DropObject(CatalogTransaction transaction, CatalogEntry &object, bool cascade) {
	if (IsSystemEntry(object)) {
		// Don't do anything for this
		return;
	}

	// Check if there are any dependencies registered on this object
	auto object_connections_p = GetDependencySet(transaction, object);
	if (!object_connections_p) {
		return;
	}
	auto &object_connections = *object_connections_p;

	// Check if there are any entries that block the DROP because they still depend on the object
	auto &dependents = object_connections.Dependents();
	vector<LookupResult> to_drop;
	dependents.Scan([&](CatalogEntry &other) {
		D_ASSERT(other.type == CatalogType::DEPENDENCY_ENTRY);
		auto &other_entry = other.Cast<DependencyCatalogEntry>();
		auto other_connections_p = GetDependencySet(transaction, other);
		if (!other_connections_p) {
			// Already deleted
			return;
		}
		auto &other_connections = *other_connections_p;
		D_ASSERT(other_connections.HasDependencyOn(object, other_entry.Type()));

		// It makes no sense to have a schema depend on anything
		D_ASSERT(other_entry.EntryType() != CatalogType::SCHEMA_ENTRY);
		auto lookup = LookupEntry(transaction, other_entry);
		if (!lookup.entry) {
			return;
		}

		if (!CascadeDrop(cascade, other_entry.Type())) {
			// no cascade and there are objects that depend on this object: throw error
			throw DependencyException("Cannot drop entry \"%s\" because there are entries that "
			                          "depend on it. Use DROP...CASCADE to drop all dependents.",
			                          object.name);
		}
		to_drop.emplace_back(std::move(lookup));
	});

	for (auto &lookup : to_drop) {
		auto set = lookup.set;
		auto entry = lookup.entry;
		set->DropEntry(transaction, entry->name, cascade);
	}

	CleanupDependencies(transaction, object);
}

void DependencyManager::AlterObject(CatalogTransaction transaction, CatalogEntry &old_obj, CatalogEntry &new_obj) {
	if (IsSystemEntry(new_obj)) {
		D_ASSERT(IsSystemEntry(old_obj));
		// Don't do anything for this
		return;
	}

	auto old_connections_p = GetDependencySet(transaction, old_obj);
	if (!old_connections_p) {
		// Nothing depends on this object and this object doesn't depend on anything either
		return;
	}
	auto &old_connections = *old_connections_p;

	dependency_set_t preserved_dependents;
	auto &dependents = old_connections.Dependents();
	dependents.Scan([&](CatalogEntry &other) {
		D_ASSERT(other.type == CatalogType::DEPENDENCY_ENTRY);
		auto &other_entry = other.Cast<DependencyCatalogEntry>();
		auto other_connections_p = GetDependencySet(transaction, other);
		if (!other_connections_p) {
			// Already deleted
			return;
		}
		D_ASSERT(other_connections_p->HasDependencyOn(old_obj, other_entry.Type()));

		// It makes no sense to have a schema depend on anything
		D_ASSERT(other_entry.EntryType() != CatalogType::SCHEMA_ENTRY);

		auto lookup = LookupEntry(transaction, other_entry);

		if (!lookup.entry) {
			return;
		}
		auto entry = lookup.entry;
		if (other_entry.Type() == DependencyType::DEPENDENCY_OWNS) {
			preserved_dependents.insert(Dependency(*entry, other_entry.Type()));
			return;
		}
		// conflict: attempting to alter this object but the dependent object still exists
		// no cascade and there are objects that depend on this object: throw error
		throw DependencyException("Cannot alter entry \"%s\" because there are entries that "
		                          "depend on it.",
		                          old_obj.name);
	});

	// Keep old dependencies
	auto &old_dependencies = old_connections.Dependencies();
	dependency_set_t dependency_list;
	old_dependencies.Scan([&](CatalogEntry &other) {
		D_ASSERT(other.type == CatalogType::DEPENDENCY_ENTRY);
		auto &other_entry = other.Cast<DependencyCatalogEntry>();

		auto lookup = LookupEntry(transaction, other_entry);
		if (!lookup.entry) {
			return;
		}
		auto entry = lookup.entry;
		dependency_list.insert(Dependency(*entry, other_entry.Type()));
	});

	// FIXME: we should update dependencies in the future
	// some alters could cause dependencies to change (imagine types of table columns)
	// or DEFAULT depending on a sequence
	if (old_obj.name != new_obj.name) {
		CleanupDependencies(transaction, old_obj);
	}

	for (auto &dep : dependency_list) {
		auto &other = dep.entry.get();
		auto other_connections = GetDependencySet(transaction, other);
		// Register that the new version of this object still has this dependency.
		// FIXME: what should the dependency type be???
		other_connections->AddDependent(transaction, new_obj, DependencyType::DEPENDENCY_REGULAR);
	}

	// Add the dependencies to the new object
	auto &connections = GetOrCreateDependencySet(transaction, new_obj);
	for (auto &dep : preserved_dependents) {
		auto &entry = dep.entry.get();
		// Create a regular dependency on 'entry', so the drop of 'entry' is blocked by the object
		dependency_list.insert(Dependency(entry, DependencyType::DEPENDENCY_REGULAR));
	}
	connections.AddDependencies(transaction, dependency_list);

	// Add the dependents that did not block the Alter
	connections.AddDependents(transaction, preserved_dependents);

	for (auto &dependency : preserved_dependents) {
		auto &entry = dependency.entry.get();
		auto dependency_connections = GetDependencySet(transaction, entry);
		D_ASSERT(dependency_connections);

		dependency_connections->AddDependent(transaction, new_obj, DependencyType::DEPENDENCY_OWNED_BY);
	}
}

void DependencyManager::Scan(ClientContext &context,
                             const std::function<void(CatalogEntry &, CatalogEntry &, DependencyType)> &callback) {
	lock_guard<mutex> write_lock(catalog.GetWriteLock());
	auto transaction = catalog.GetCatalogTransaction(context);

	// Scan all the dependency sets
	catalog_entry_set_t entries;
	connections.Scan([&](CatalogEntry &set) {
		auto lookup = LookupEntry(transaction, set);
		D_ASSERT(lookup.entry);
		auto &entry = *lookup.entry;
		entries.insert(entry);
	});

	for (auto &entry : entries) {
		auto set = GetDependencySet(transaction, entry);
		auto &dependents = set->Dependents();
		// Scan all the dependents of the entry
		dependents.Scan([&](CatalogEntry &dependent) {
			auto &dependency_entry = dependent.Cast<DependencyCatalogEntry>();
			auto lookup = LookupEntry(transaction, dependent);
			if (!lookup.entry) {
				return;
			}
			auto &dependent_entry = *lookup.entry;
			callback(entry, dependent_entry, dependency_entry.Type());
		});
	}
}

void DependencyManager::AddOwnership(CatalogTransaction transaction, CatalogEntry &owner, CatalogEntry &entry) {
	D_ASSERT(!IsSystemEntry(entry));
	D_ASSERT(!IsSystemEntry(owner));

	// lock the catalog for writing
	lock_guard<mutex> write_lock(catalog.GetWriteLock());

	// If the owner is already owned by something else, throw an error
	auto &owner_connections = GetOrCreateDependencySet(transaction, owner);
	auto &owner_dependents = owner_connections.Dependents();
	owner_dependents.Scan([&](CatalogEntry &dependent) {
		auto &dependent_entry = dependent.Cast<DependencyCatalogEntry>();
		if (dependent_entry.Type() == DependencyType::DEPENDENCY_OWNED_BY) {
			throw DependencyException(owner.name + " already owned by " + dependent_entry.name);
		}
	});

	// If the entry is already owned, throw an error
	auto &entry_connections = GetOrCreateDependencySet(transaction, entry);
	auto &entry_dependents = entry_connections.Dependents();
	entry_dependents.Scan([&](CatalogEntry &dependent) {
		auto &dependent_entry = dependent.Cast<DependencyCatalogEntry>();
		auto dependency_type = dependent_entry.Type();

		auto lookup = LookupEntry(transaction, dependent);
		if (!lookup.entry) {
			return;
		}
		auto &dep = *lookup.entry;

		// if the entry is already owned, throw error
		if (&dep != &owner) {
			throw DependencyException(entry.name + " already depends on " + dep.name);
		}

		// if the entry owns the owner, throw error
		if (&dep == &owner && dependency_type == DependencyType::DEPENDENCY_OWNS) {
			throw DependencyException(entry.name + " already owns " + owner.name +
			                          ". Cannot have circular dependencies");
		}
	});
	entry_connections.AddDependent(transaction, owner, DependencyType::DEPENDENCY_OWNED_BY);
	// We use an automatic dependency because if the Owner gets deleted, then the owned objects are also deleted
	owner_connections.AddDependency(transaction, entry);

	owner_connections.AddDependent(transaction, entry, DependencyType::DEPENDENCY_OWNS);
	// We explicitly don't complete this link the other way, so we don't have recursive dependencies
	// If we would 'entry_connection.AddDependency(owner)' then we would try to delete 'owner'
	// when 'entry' gets deleted, but this delete can only be initiated by 'owner'
}

} // namespace duckdb
