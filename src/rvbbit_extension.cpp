#define DUCKDB_EXTENSION_MAIN

#include "rvbbit_extension.hpp"

#include "engine.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

namespace duckdb {
namespace rvbbit {

void RegisterDecisionFunctions(ExtensionLoader &loader);
void RegisterLlmFunctions(ExtensionLoader &loader);
void RegisterGrammar(ExtensionLoader &loader);

void SetNull(Vector &result, idx_t row) {
	result.SetValue(row, Value(result.GetType()));
}

bool ArgString(const Vector &vec, idx_t row, string &out) {
	auto value = vec.GetValue(row);
	if (value.IsNull()) {
		return false;
	}
	out = StringValue::Get(value);
	return true;
}

ScalarFunction VolatileFun(const string &name, std::initializer_list<FunctionParameter> params, LogicalType ret,
                           scalar_function_t func) {
	ScalarFunction fun(Identifier(name), params, std::move(ret), func);
	fun.SetVolatile();
	fun.SetFallible();
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

unique_ptr<BaseSecret> CreateOpenRouterSecret(ClientContext &, CreateSecretInput &input) {
	vector<string> scope = input.scope.empty() ? vector<string> {"openrouter"} : input.scope;
	auto secret = make_uniq<KeyValueSecret>(std::move(scope), input.type, input.provider, input.name);
	secret->TrySetValue("key", input);
	secret->TrySetValue("api_key", input);
	secret->TrySetValue("base_url", input);
	secret->TrySetValue("decision_model", input);
	secret->TrySetValue("chat_model", input);
	auto key = secret->TryGetValue(Identifier("key"), false);
	if (key.IsNull()) {
		auto api_key = secret->TryGetValue(Identifier("api_key"), false);
		if (!api_key.IsNull()) {
			secret->secret_map[Identifier("key")] = api_key;
		}
	}
	secret->redact_keys.insert(Identifier("key"));
	secret->redact_keys.insert(Identifier("api_key"));
	return std::move(secret);
}

void RegisterSecret(ExtensionLoader &loader) {
	SecretType secret_type;
	secret_type.name = "openrouter";
	secret_type.deserializer = KeyValueSecret::Deserialize<KeyValueSecret>;
	secret_type.default_provider = "config";
	loader.RegisterSecretType(secret_type);

	CreateSecretFunction function;
	function.secret_type = "openrouter";
	function.provider = "config";
	function.function = CreateOpenRouterSecret;
	function.named_parameters["key"] = LogicalType::VARCHAR;
	function.named_parameters["api_key"] = LogicalType::VARCHAR;
	function.named_parameters["base_url"] = LogicalType::VARCHAR;
	function.named_parameters["decision_model"] = LogicalType::VARCHAR;
	function.named_parameters["chat_model"] = LogicalType::VARCHAR;
	loader.RegisterFunction(function);
}

void VersionFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		result.SetValue(row, Value(EXTENSION_VERSION));
	}
}

void KeySourceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto source = ResolveProvider(context, LoadSettings(context)).key_source;
	for (idx_t row = 0; row < args.size(); row++) {
		result.SetValue(row, Value(source));
	}
}

void BoolSettingFunction(DataChunk &args, ExpressionState &state, Vector &result, bool (*probe)(ClientContext &)) {
	auto value = probe(state.GetContext());
	for (idx_t row = 0; row < args.size(); row++) {
		result.SetValue(row, Value::BOOLEAN(value));
	}
}

void CachePurgeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	CachePurge(state.GetContext());
	for (idx_t row = 0; row < args.size(); row++) {
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

void CacheClearFunction(DataChunk &args, ExpressionState &, Vector &result) {
	CacheClearMemory();
	for (idx_t row = 0; row < args.size(); row++) {
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

void HttpResetFunction(DataChunk &args, ExpressionState &, Vector &result) {
	HttpLogReset();
	for (idx_t row = 0; row < args.size(); row++) {
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

void MockResetFunction(DataChunk &args, ExpressionState &, Vector &result) {
	MockReset();
	for (idx_t row = 0; row < args.size(); row++) {
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

void MockFailFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		if (args.data[0].GetValue(row).IsNull() || args.data[1].GetValue(row).IsNull()) {
			SetNull(result, row);
			continue;
		}
		MockFailNext(args.data[0].GetValue(row).GetValue<int32_t>(), args.data[1].GetValue(row).GetValue<int32_t>());
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

void MockNoulFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		string state_text;
		string predicate;
		if (!ArgString(args.data[0], row, state_text) || !ArgString(args.data[1], row, predicate) ||
		    args.data[2].GetValue(row).IsNull()) {
			SetNull(result, row);
			continue;
		}
		MockNoul(std::move(state_text), std::move(predicate), args.data[2].GetValue(row).GetValue<double>());
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

struct LogState : public GlobalTableFunctionState {
	vector<HttpLogRow> rows;
	idx_t offset = 0;
};

unique_ptr<FunctionData> HttpLogBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                     vector<Identifier> &names) {
	names = {Identifier("seq"), Identifier("kind"), Identifier("url"), Identifier("status"), Identifier("body")};
	return_types = {LogicalType::BIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::INTEGER,
	                LogicalType::VARCHAR};
	return make_uniq<TableFunctionData>();
}

unique_ptr<GlobalTableFunctionState> HttpLogInit(ClientContext &, TableFunctionInitInput &) {
	auto state = make_uniq<LogState>();
	state->rows = HttpLogSnapshot();
	return std::move(state);
}

void HttpLogScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<LogState>();
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.offset++];
		output.data[0].SetValue(count, Value::BIGINT(row.seq));
		output.data[1].SetValue(count, Value(row.kind));
		output.data[2].SetValue(count, Value(row.url));
		output.data[3].SetValue(count, Value::INTEGER(row.status));
		output.data[4].SetValue(count, Value(row.body));
		count++;
	}
		output.SetCardinalityUnsafe(count);
}

struct StatsState : public GlobalTableFunctionState {
	bool done = false;
};

unique_ptr<FunctionData> StatsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                   vector<Identifier> &names) {
	names = {Identifier("hits"), Identifier("misses"), Identifier("l2_hits"), Identifier("entries"),
	         Identifier("http_calls")};
	return_types = {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT,
	                LogicalType::BIGINT};
	return make_uniq<TableFunctionData>();
}

unique_ptr<GlobalTableFunctionState> StatsInit(ClientContext &, TableFunctionInitInput &) {
	return make_uniq<StatsState>();
}

void StatsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<StatsState>();
	if (state.done) {
		output.SetCardinalityUnsafe(0);
		return;
	}
	int64_t hits = 0;
	int64_t misses = 0;
	int64_t l2_hits = 0;
	int64_t entries = 0;
	int64_t http_calls = 0;
	CacheStats(hits, misses, l2_hits, entries, http_calls);
	output.data[0].SetValue(0, Value::BIGINT(hits));
	output.data[1].SetValue(0, Value::BIGINT(misses));
	output.data[2].SetValue(0, Value::BIGINT(l2_hits));
	output.data[3].SetValue(0, Value::BIGINT(entries));
	output.data[4].SetValue(0, Value::BIGINT(http_calls));
	output.SetCardinalityUnsafe(1);
	state.done = true;
}

void RegisterSupport(ExtensionLoader &loader) {
	loader.RegisterFunction(VolatileFun("version", {}, LogicalType::VARCHAR, VersionFunction));
	loader.RegisterFunction(VolatileFun("key_source", {}, LogicalType::VARCHAR, KeySourceFunction));
	loader.RegisterFunction(VolatileFun("secret_configured", {}, LogicalType::BOOLEAN,
	                                    [](DataChunk &args, ExpressionState &state, Vector &result) {
		                                    BoolSettingFunction(args, state, result, [](ClientContext &context) {
			                                    return ResolveProvider(context, LoadSettings(context)).key_source !=
			                                           "none";
		                                    });
	                                    }));
	loader.RegisterFunction(VolatileFun("grammar_registered", {}, LogicalType::BOOLEAN,
	                                    [](DataChunk &args, ExpressionState &state, Vector &result) {
		                                    BoolSettingFunction(args, state, result,
		                                                        [](ClientContext &) { return GrammarRegistered(); });
	                                    }));
	loader.RegisterFunction(VolatileFun("cache_purge", {}, LogicalType::BOOLEAN, CachePurgeFunction));
	loader.RegisterFunction(VolatileFun("cache_clear_memory", {}, LogicalType::BOOLEAN, CacheClearFunction));
	loader.RegisterFunction(VolatileFun("http_log_reset", {}, LogicalType::BOOLEAN, HttpResetFunction));
	loader.RegisterFunction(VolatileFun("mock_reset", {}, LogicalType::BOOLEAN, MockResetFunction));
	loader.RegisterFunction(VolatileFun(
	    "mock_fail_next",
	    {FunctionParameter("count", LogicalType::INTEGER), FunctionParameter("status", LogicalType::INTEGER)},
	    LogicalType::BOOLEAN, MockFailFunction));
	loader.RegisterFunction(VolatileFun(
	    "mock_noul",
	    {FunctionParameter("state_substr", LogicalType::VARCHAR),
	     FunctionParameter("instruction_substr", LogicalType::VARCHAR),
	     FunctionParameter("score", LogicalType::DOUBLE)},
	    LogicalType::BOOLEAN, MockNoulFunction));

	TableFunction http_log(Identifier("http_log"), {}, HttpLogScan, HttpLogBind, HttpLogInit);
	loader.RegisterFunction(http_log);
	TableFunction cache_stats(Identifier("cache_stats"), {}, StatsScan, StatsBind, StatsInit);
	loader.RegisterFunction(cache_stats);
}

} // namespace rvbbit

void LoadInternal(ExtensionLoader &loader) {
	loader.SetDescription("rvbbit semantic SQL for DuckDB: OpenRouter decisions, lazy decision trees, and LLM "
	                      "operators. Grammar sugar is opt-in.");
	loader.UseDedicatedSchemaForExtension();
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(Identifier("rvbbit_transport"), "live or mock. mock serves SQLLogicTests without a key.",
	                          LogicalType::VARCHAR, Value("live"));
	config.AddExtensionOption(Identifier("rvbbit_max_concurrency"),
	                          "Process-wide cap on in-flight OpenRouter HTTP requests.", LogicalType::INTEGER,
	                          Value::INTEGER(8));
	config.AddExtensionOption(Identifier("rvbbit_cache_path"),
	                          "Optional DuckDB file used as the persistent judgment and named-tree cache.",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption(Identifier("rvbbit_base_url"),
	                          "OpenRouter origin, or another gateway that speaks the same paths. Empty uses the secret "
	                          "or https://openrouter.ai.",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption(Identifier("rvbbit_decision_model"), "Overrides the Jev model. Empty uses the secret or "
	                                                               "typesafe/jev-1.13.",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption(Identifier("rvbbit_chat_model"),
	                          "Overrides the chat model. Empty uses the secret or openai/gpt-6-luna.",
	                          LogicalType::VARCHAR, Value(""));
	config.AddExtensionOption(Identifier("rvbbit_cache_size"), "In-memory LRU capacity.", LogicalType::INTEGER,
	                          Value::INTEGER(10000));
	config.AddExtensionOption(Identifier("rvbbit_max_retries"), "Extra attempts after a 429 or 5xx response.",
	                          LogicalType::INTEGER, Value::INTEGER(3));
	config.AddExtensionOption(Identifier("rvbbit_retry_base_ms"), "First backoff delay. Later attempts double it.",
	                          LogicalType::INTEGER, Value::INTEGER(200));
	config.AddExtensionOption(Identifier("rvbbit_ignore_env"),
	                          "When true, OPENROUTER_API_KEY is not a fallback. Tests use this.", LogicalType::BOOLEAN,
	                          Value::BOOLEAN(false));

	rvbbit::RegisterSecret(loader);
	rvbbit::RegisterSupport(loader);
	rvbbit::RegisterDecisionFunctions(loader);
	rvbbit::RegisterLlmFunctions(loader);
	rvbbit::RegisterGrammar(loader);
}

void RvbbitExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string RvbbitExtension::Name() {
	return "rvbbit";
}

std::string RvbbitExtension::Version() const {
#ifdef EXT_VERSION_RVBBIT
	return EXT_VERSION_RVBBIT;
#else
	return rvbbit::EXTENSION_VERSION;
#endif
}

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(rvbbit, loader) {
	duckdb::LoadInternal(loader);
}

}
