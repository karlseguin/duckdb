#include "duckdb/parser/statement/set_statement.hpp"

namespace duckdb {

SetStatement::SetStatement(std::string name_p, Value value_p)
    : SQLStatement(StatementType::SET_STATEMENT), name(name_p), value(value_p) {};

unique_ptr<SQLStatement> SetStatement::Copy() const {
	return make_unique<SetStatement>(name, value);
}

} // namespace duckdb
