#include "json_common.hpp"

namespace duckdb {

static void CheckPath(const Value &path_val, string &path, size_t &len) {
	string error;
	Value path_str_val;
	if (!path_val.TryCastAs(LogicalType::VARCHAR, path_str_val, &error)) {
		throw InvalidInputException("Cannot cast JSON path to VARCHAR");
	}
	auto path_str = path_str_val.GetValueUnsafe<string_t>();
	len = path_str.GetSize();
	auto ptr = path_str.GetDataUnsafe();
	// Empty strings and invalid $ paths yield an error
	if (len == 0 || (*ptr == '$' && !JSONCommon::ValidPathDollar(ptr, len))) {
		throw Exception("JSON path error");
	}
	// Copy over string to the bind data
	if (*ptr == '/' || *ptr == '$') {
		path = string(ptr, len);
	} else {
		path = "/" + string(ptr, len);
		len++;
	}
}

JSONReadFunctionData::JSONReadFunctionData(bool constant, string path_p, idx_t len)
    : constant(constant), path(move(path_p)), ptr(path.c_str()), len(len) {
}

unique_ptr<FunctionData> JSONReadFunctionData::Copy() {
	return make_unique<JSONReadFunctionData>(constant, path, len);
}

unique_ptr<FunctionData> JSONReadFunctionData::Bind(ClientContext &context, ScalarFunction &bound_function,
                                                    vector<unique_ptr<Expression>> &arguments) {
	D_ASSERT(bound_function.arguments.size() == 2);
	bool constant = false;
	string path = "";
	size_t len = 0;
	if (arguments[1]->return_type.id() != LogicalTypeId::SQLNULL && arguments[1]->IsFoldable()) {
		constant = true;
		const auto path_val = ExpressionExecutor::EvaluateScalar(*arguments[1]);
		CheckPath(path_val, path, len);
	}
	return make_unique<JSONReadFunctionData>(constant, move(path), len);
}

JSONReadManyFunctionData::JSONReadManyFunctionData(vector<string> paths_p, vector<size_t> lens_p)
    : paths(move(paths_p)), lens(move(lens_p)) {
	for (const auto &path : paths) {
		ptrs.push_back(path.c_str());
	}
}

unique_ptr<FunctionData> JSONReadManyFunctionData::Copy() {
	return make_unique<JSONReadManyFunctionData>(paths, lens);
}

unique_ptr<FunctionData> JSONReadManyFunctionData::Bind(ClientContext &context, ScalarFunction &bound_function,
                                                        vector<unique_ptr<Expression>> &arguments) {
	D_ASSERT(bound_function.arguments.size() == 2);
	if (!arguments[1]->IsFoldable()) {
		throw InvalidInputException("List of paths must be constant");
	}
	if (arguments[1]->return_type.id() == LogicalTypeId::SQLNULL) {
		return make_unique<JSONReadManyFunctionData>(vector<string>(), vector<size_t>());
	}

	vector<string> paths;
	vector<size_t> lens;
	auto paths_val = ExpressionExecutor::EvaluateScalar(*arguments[1]);
	for (auto &path_val : ListValue::GetChildren(paths_val)) {
		paths.push_back("");
		lens.push_back(0);
		CheckPath(path_val, paths.back(), lens.back());
	}

	return make_unique<JSONReadManyFunctionData>(move(paths), move(lens));
}

bool JSONCommon::ValidPathDollar(const char *ptr, const idx_t &len) {
	const char *const end = ptr + len;
	// Skip past '$'
	ptr++;
	while (ptr != end) {
		const auto &c = *ptr++;
		if (c == '.') {
			// Object
			bool escaped = false;
			if (*ptr == '"') {
				// Skip past opening '"'
				ptr++;
				escaped = true;
			}
			auto key_len = ReadString(ptr, end, escaped);
			if (key_len == 0) {
				return false;
			}
			ptr += key_len;
			if (escaped) {
				// Skip past closing '"'
				ptr++;
			}
		} else if (c == '[') {
			// Array
			if (*ptr == '#') {
				// Index from back of array
				ptr++;
				if (*ptr == ']') {
					ptr++;
					continue;
				}
				if (*ptr != '-') {
					return false;
				}
				// Skip past '-'
				ptr++;
			}
			idx_t idx;
			auto idx_len = ReadIndex(ptr, end, idx);
			if (idx_len == 0) {
				return false;
			}
			ptr += idx_len;
			// Skip past closing ']'
			ptr++;
		} else {
			return false;
		}
	}
	return true;
}

} // namespace duckdb