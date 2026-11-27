#include "engine.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <regex>

namespace duckdb {
namespace rvbbit {

namespace {

string Trim(string value) {
	auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

string Lower(string value) {
	for (auto &ch : value) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return value;
}

struct Operator {
	string name;
	string shape = "scalar";
	vector<string> arg_names;
	string return_type = "text";
	string model = DEFAULT_CHAT_MODEL;
	string system_prompt;
	string user_prompt;
	string parser = "strip";
	int32_t max_tokens = 256;
	bool has_temperature = false;
	double temperature = 0;
	string description;
	string infix_word;
	string steps;
	string retry;
	string wards;
	string takes;
};

struct Registry {
	std::mutex mu;
	vector<Operator> ops;
	bool seeded = false;
};

Registry &Catalog() {
	static Registry registry;
	return registry;
}

Operator *FindUnlocked(const string &name) {
	for (auto &op : Catalog().ops) {
		if (Lower(op.name) == Lower(name)) {
			return &op;
		}
	}
	return nullptr;
}

void Upsert(Operator op) {
	auto &registry = Catalog();
	std::lock_guard<std::mutex> lock(registry.mu);
	if (auto *existing = FindUnlocked(op.name)) {
		*existing = std::move(op);
	} else {
		registry.ops.push_back(std::move(op));
	}
}

Operator CopyOf(const string &name) {
	auto &registry = Catalog();
	std::lock_guard<std::mutex> lock(registry.mu);
	auto *found = FindUnlocked(name);
	if (!found) {
		throw InvalidInputException("rvbbit: unknown operator " + name);
	}
	return *found;
}

string Render(string templ, const Json &inputs, const Json *steps, const string &output, int attempt) {
	auto lookup = [&](const string &path) -> string {
		if (path == "output") {
			return output;
		}
		if (path == "attempt") {
			return std::to_string(attempt);
		}
		if (path.rfind("inputs.", 0) == 0) {
			auto key = path.substr(7);
			auto value = inputs.Find(key);
			return value && value->IsString() ? value->str : (value ? value->Dump() : "");
		}
		if (path.rfind("steps.", 0) == 0 && steps) {
			auto rest = path.substr(6);
			auto dot = rest.find('.');
			auto step = dot == string::npos ? rest : rest.substr(0, dot);
			auto field = dot == string::npos ? string("output") : rest.substr(dot + 1);
			auto node = steps->Find(step);
			if (!node) {
				return "";
			}
			auto value = node->Find(field);
			return value && value->IsString() ? value->str : (value ? value->Dump() : "");
		}
		auto direct = inputs.Find(path);
		return direct && direct->IsString() ? direct->str : (direct ? direct->Dump() : "");
	};
	string out;
	for (size_t i = 0; i < templ.size();) {
		if (templ[i] == '{' && i + 1 < templ.size() && templ[i + 1] == '{') {
			auto end = templ.find("}}", i + 2);
			if (end == string::npos) {
				out.push_back(templ[i++]);
				continue;
			}
			out += lookup(Trim(templ.substr(i + 2, end - (i + 2))));
			i = end + 2;
		} else {
			out.push_back(templ[i++]);
		}
	}
	return out;
}

string ChatContentOf(const string &response, string &error) {
	try {
		auto json = Json::Parse(response);
		auto choices = json.Find("choices");
		if (!choices || !choices->IsArray() || choices->arr.empty()) {
			error = "chat response has no choices";
			return "";
		}
		auto message = choices->arr[0].Find("message");
		auto content = message ? message->Find("content") : nullptr;
		if (!content || content->IsNull() || !content->IsString() || Trim(content->str).empty()) {
			error = "chat response content was empty";
			return "";
		}
		error.clear();
		return Trim(content->str);
	} catch (const std::exception &ex) {
		error = ex.what();
		return "";
	}
}

string ChatBody(const Operator &op, const string &model, const string &system, const string &user,
                 const string &reasoning) {
	Json messages = Json::Array();
	Json system_message = Json::Object();
	system_message.Set("role", Json::String("system"));
	system_message.Set("content", Json::String(system));
	Json user_message = Json::Object();
	user_message.Set("role", Json::String("user"));
	user_message.Set("content", Json::String(user));
	messages.arr.push_back(std::move(system_message));
	messages.arr.push_back(std::move(user_message));
	Json body = Json::Object();
	body.Set("model", Json::String(model.empty() ? op.model : model));
	body.Set("messages", std::move(messages));
	body.Set("temperature", Json::Number(op.has_temperature ? op.temperature : 0));
	body.Set("max_tokens", Json::Number(op.max_tokens));
	SetReasoning(body, reasoning);
	return body.Dump();
}

string OperatorModel(ClientContext &context, const Operator &op) {
	auto provider = ResolveProvider(context, LoadSettings(context));
	if (provider.chat_model_from_user && !provider.chat_model.empty()) {
		return provider.chat_model;
	}
	return op.model.empty() ? DEFAULT_CHAT_MODEL : op.model;
}

struct Work {
	string output;
	string error;
	bool ok = false;
};

Work Chat(ClientContext &context, const Operator &op, const string &model, const string &system, const string &user) {
	auto settings = LoadSettings(context);
	auto request = ChatBody(op, model, system, user, settings.reasoning);
	int attempts = std::max(1, settings.max_retries + 1);
	Work work;
	for (int attempt = 1; attempt <= attempts; attempt++) {
		auto fetched = FetchPost(context, "chat", {request});
		if (fetched.empty() || !fetched[0].ok) {
			work.error = fetched.empty() ? "chat failed" : fetched[0].error;
			work.ok = false;
			return work;
		}
		string error;
		work.output = ChatContentOf(fetched[0].body, error);
		if (!error.empty()) {
			work.error = error;
			work.ok = false;
			if (error != "chat response content was empty" || attempt == attempts) {
				if (error == "chat response content was empty") {
					throw InvalidInputException(
					    "chat response content was empty. rvbbit_reasoning defaults to disabled so the token budget is not spent on reasoning.");
				}
				return work;
			}
			continue;
		}
		work.ok = true;
		work.error.clear();
		return work;
	}
	return work;
}

string ValidateOneOf(const string &value, const string &allowed, const string &fallback) {
	string current;
	auto match = Lower(Trim(value));
	auto consider = [&](const string &label) {
		auto trimmed = Trim(label);
		if (!trimmed.empty() && Lower(trimmed) == match) {
			current = trimmed;
		}
	};
	string piece;
	for (auto ch : allowed) {
		if (ch == ',') {
			consider(piece);
			piece.clear();
		} else {
			piece.push_back(ch);
		}
	}
	consider(piece);
	return current.empty() ? (fallback.empty() ? "unknown" : fallback) : current;
}

Work RunSteps(ClientContext &context, const Operator &op, const Json &inputs, const string &feedback,
              const string &model) {
	Json steps = Json::Parse(op.steps);
	Json done = Json::Object();
	Work last;
	if (!steps.IsArray()) {
		last.error = "operator steps must be a JSON array";
		return last;
	}
	for (auto &step : steps.arr) {
		auto kind = step.Find("kind");
		auto name = step.Find("name");
		string step_name = name && name->IsString() ? name->str : "step";
		if (!kind || !kind->IsString()) {
			last.error = "operator step is missing a kind";
			return last;
		}
		if (kind->str == "llm") {
			auto system = step.Find("system");
			auto user = step.Find("user");
			auto max_tokens = step.Find("max_tokens");
			Operator step_op = op;
			if (max_tokens && max_tokens->IsNumber()) {
				step_op.max_tokens = static_cast<int32_t>(max_tokens->num);
			}
			auto user_text = Render(user && user->IsString() ? user->str : "", inputs, &done, last.output, 1);
			if (!feedback.empty()) {
				user_text += "\n\n" + feedback;
			}
			last = Chat(context, step_op, model.empty() ? op.model : model, system && system->IsString() ? system->str : "",
			            user_text);
			if (!last.ok) {
				return last;
			}
		} else if (kind->str == "code") {
			auto fn = step.Find("fn");
			if (!fn || !fn->IsString() || fn->str != "validate_one_of") {
				last.error = "unsupported code step";
				return last;
			}
			auto spec = step.Find("inputs");
			auto render_field = [&](const char *key) {
				auto field = spec ? spec->Find(key) : nullptr;
				return Render(field && field->IsString() ? field->str : "", inputs, &done, last.output, 1);
			};
			last.output = ValidateOneOf(render_field("value"), render_field("allowed"), render_field("default"));
			last.ok = true;
			last.error.clear();
		} else {
			last.error = "operator step kind '" + kind->str + "' is not executed in this version";
			return last;
		}
		Json recorded = Json::Object();
		recorded.Set("output", Json::String(last.output));
		done.Set(step_name, std::move(recorded));
	}
	return last;
}

Work RunBody(ClientContext &context, const Operator &op, const Json &inputs, const string &model, const string &feedback) {
	if (!op.steps.empty()) {
		return RunSteps(context, op, inputs, feedback, model);
	}
	auto user = Render(op.user_prompt, inputs, nullptr, "", 1);
	if (!feedback.empty()) {
		user += "\n\n" + feedback;
	}
	Operator call = op;
	return Chat(context, call, model.empty() ? op.model : model, op.system_prompt, user);
}

bool SqlClause(const string &clause, const string &output, const Json &inputs, string &error) {
	std::smatch match;
	static const std::regex regex_clause(
	    R"(^(?:btrim\s*\(\s*\$output\s*\)|\$output)\s*(!~|~)\s*'([^']*)'\s*$)", std::regex::icase);
	if (std::regex_match(clause, match, regex_clause)) {
		bool negated = match[1].str() == "!~";
		try {
			std::regex pattern(match[2].str());
			bool found = std::regex_search(Trim(output), pattern);
			return negated ? !found : found;
		} catch (const std::regex_error &ex) {
			error = ex.what();
			return true;
		}
	}
	static const std::regex eq_clause(R"(^\$output\s*(<>|=)\s*'([^']*)'\s*$)", std::regex::icase);
	if (std::regex_match(clause, match, eq_clause)) {
		bool equal = Trim(output) == match[2].str();
		return match[1].str() == "=" ? equal : !equal;
	}
	static const std::regex len_clause(
	    R"(^length\s*\(\s*\$inputs\s*->>\s*'([^']+)'\s*\)\s*(>=|<=|>|<|=)\s*([0-9]+)\s*$)", std::regex::icase);
	if (std::regex_match(clause, match, len_clause)) {
		auto field = inputs.Find(match[1].str());
		auto text = field && field->IsString() ? field->str : string();
		auto length = static_cast<int>(text.size());
		auto bound = std::stoi(match[3].str());
		auto cmp = match[2].str();
		if (cmp == ">") {
			return length > bound;
		}
		if (cmp == ">=") {
			return length >= bound;
		}
		if (cmp == "<") {
			return length < bound;
		}
		if (cmp == "<=") {
			return length <= bound;
		}
		return length == bound;
	}
	error = "unsupported validator expression";
	return true;
}

bool EvalValidator(const Json &validator, const string &output, const Json &inputs) {
	string expr;
	if (validator.IsString()) {
		// A function validator needs a SQL function. A missing one passes, matching Calliope.
		return true;
	}
	auto sql = validator.Find("sql");
	if (!sql || !sql->IsString()) {
		return true;
	}
	string pending;
	bool all = true;
	auto flush = [&](const string &piece) {
		string error;
		bool passed = SqlClause(Trim(piece), output, inputs, error);
		if (!error.empty()) {
			passed = true;
		}
		all = all && passed;
	};
	string rest = sql->str;
	while (!rest.empty()) {
		auto at = Lower(rest).find(" and ");
		if (at == string::npos) {
			flush(rest);
			break;
		}
		flush(rest.substr(0, at));
		rest = rest.substr(at + 5);
	}
	return all;
}

Json ValidatorOf(const string &json, const char *field) {
	if (json.empty()) {
		return Json::Null();
	}
	try {
		auto parsed = Json::Parse(json);
		if (field) {
			auto child = parsed.Find(field);
			return child ? *child : Json::Null();
		}
		return parsed;
	} catch (const std::exception &) {
		return Json::Null();
	}
}

bool Wards(const string &json, const char *slot, const string &output, const Json &inputs, bool &blocking) {
	blocking = false;
	auto parsed = json.empty() ? Json::Null() : Json::Parse(json);
	auto list = parsed.Find(slot);
	if (!list || !list->IsArray()) {
		return true;
	}
	for (auto &ward : list->arr) {
		auto validator = ward.Find("validator");
		auto mode = ward.Find("mode");
		bool advisory = mode && mode->IsString() && Lower(mode->str) == "advisory";
		bool passed = validator ? EvalValidator(*validator, output, inputs) : true;
		if (!passed && !advisory) {
			blocking = true;
			return false;
		}
	}
	return true;
}

Work ExecuteAttempt(ClientContext &context, const Operator &op, const Json &inputs, const string &feedback,
                     const string &resolved_model) {
	if (op.takes.empty()) {
		return RunBody(context, op, inputs, resolved_model, feedback);
	}
	Json plan;
	try {
		plan = Json::Parse(op.takes);
	} catch (const std::exception &) {
		return RunBody(context, op, inputs, resolved_model, feedback);
	}
	int factor = 1;
	if (auto value = plan.Find("factor"); value && value->IsNumber()) {
		factor = std::max(1, std::min(12, static_cast<int>(value->num)));
	}
	vector<string> models;
	if (auto value = plan.Find("models"); value && value->IsArray()) {
		for (auto &model : value->arr) {
			if (model.IsString()) {
				models.push_back(model.str);
			}
		}
	}
	vector<Work> takes;
	for (int i = 0; i < factor; i++) {
		string model = models.empty() ? resolved_model : models[static_cast<size_t>(i) % models.size()];
		takes.push_back(RunBody(context, op, inputs, model, feedback));
	}
	vector<idx_t> alive;
	for (idx_t i = 0; i < takes.size(); i++) {
		if (takes[i].ok) {
			alive.push_back(i);
		}
	}
	if (alive.empty()) {
		return takes.empty() ? Work {} : takes[0];
	}
	string reduce = "vote";
	if (auto value = plan.Find("reduce"); value && value->IsString()) {
		reduce = Lower(value->str);
	}
	if (reduce == "first_valid") {
		return takes[alive[0]];
	}
	if (reduce == "evaluator") {
		string instructions = "You are selecting the single best answer to the task.";
		if (auto evaluator = plan.Find("evaluator")) {
			if (auto text = evaluator->Find("instructions"); text && text->IsString()) {
				instructions = text->str;
			}
		}
		string user = "TASK INPUTS:\n" + inputs.Dump() + "\n\nCANDIDATE ANSWERS:\n";
		for (idx_t n = 0; n < alive.size(); n++) {
			user += "[" + std::to_string(n + 1) + "] " + Trim(takes[alive[n]].output) + "\n";
		}
		user += "\nReply with ONLY the number (1-" + std::to_string(alive.size()) + ") of the best candidate.";
		auto judged = Chat(context, op, "", instructions, user);
		idx_t pick = 0;
		if (judged.ok) {
			string digits;
			for (auto ch : judged.output) {
				if (std::isdigit(static_cast<unsigned char>(ch))) {
					digits.push_back(ch);
				}
			}
			if (!digits.empty()) {
				auto number = std::stoi(digits);
				if (number >= 1 && static_cast<size_t>(number) <= alive.size()) {
					pick = static_cast<idx_t>(number - 1);
				}
			}
		}
		return takes[alive[pick]];
	}
	// vote, and consensus falls back to vote unless every take is a JSON array
	if (reduce == "consensus") {
		vector<vector<string>> parsed;
		for (auto index : alive) {
			try {
				auto json = Json::Parse(Trim(takes[index].output));
				if (!json.IsArray()) {
					continue;
				}
				vector<string> items;
				for (auto &item : json.arr) {
					items.push_back(Lower(Trim(item.IsString() ? item.str : item.Dump())));
				}
				parsed.push_back(std::move(items));
			} catch (const std::exception &) {
			}
		}
		if (!parsed.empty()) {
			int need = static_cast<int>(parsed.size() / 2 + 1);
			if (auto min_agree = plan.Find("min_agree"); min_agree && min_agree->IsNumber()) {
				need = std::max(1, static_cast<int>(min_agree->num));
			}
			vector<string> kept;
			vector<string> seen;
			for (auto &take : parsed) {
				for (auto &item : take) {
					if (std::find(seen.begin(), seen.end(), item) != seen.end()) {
						continue;
					}
					int count = 0;
					for (auto &other : parsed) {
						if (std::find(other.begin(), other.end(), item) != other.end()) {
							count++;
						}
					}
					if (count >= need) {
						kept.push_back(item);
						seen.push_back(item);
					}
				}
			}
			Work merged = takes[alive[0]];
			string array = "[";
			for (idx_t i = 0; i < kept.size(); i++) {
				if (i) {
					array += ",";
				}
				array += "\"" + kept[i] + "\"";
			}
			array += "]";
			merged.output = array;
			return merged;
		}
	}
	std::map<string, int> counts;
	for (auto index : alive) {
		counts[Trim(takes[index].output)]++;
	}
	idx_t best = alive[0];
	int best_count = -1;
	for (auto index : alive) {
		int count = counts[Trim(takes[index].output)];
		if (count > best_count) {
			best_count = count;
			best = index;
		}
	}
	return takes[best];
}

Work ApplyRetry(ClientContext &context, const Operator &op, const Json &inputs, Work first) {
	if (op.retry.empty() || !first.ok) {
		return first;
	}
	Json plan;
	try {
		plan = Json::Parse(op.retry);
	} catch (const std::exception &) {
		return first;
	}
	auto until = plan.Find("until");
	if (!until) {
		return first;
	}
	int max_attempts = 3;
	if (auto value = plan.Find("max_attempts"); value && value->IsNumber()) {
		max_attempts = std::max(1, static_cast<int>(value->num));
	}
	auto instructions = plan.Find("instructions");
	Work result = std::move(first);
	for (int attempt = 1; attempt <= max_attempts; attempt++) {
		if (EvalValidator(*until, result.output, inputs) || attempt == max_attempts) {
			return result;
		}
		string feedback;
		if (instructions && instructions->IsString()) {
			feedback = Render(instructions->str, inputs, nullptr, result.output, attempt + 1);
		}
		result = ExecuteAttempt(context, op, inputs, feedback, OperatorModel(context, op));
		if (!result.ok) {
			return result;
		}
	}
	return result;
}

bool Typed(const Operator &op, const Work &work, Vector &result, idx_t row) {
	if (!work.ok || !work.error.empty()) {
		result.SetValue(row, Value(result.GetType()));
		return false;
	}
	if (op.shape == "dimension") {
		try {
			auto json = Json::Parse(Trim(work.output));
			vector<Value> values;
			if (json.IsArray()) {
				for (auto &item : json.arr) {
					values.push_back(Value(item.IsString() ? item.str : item.Dump()));
				}
			}
			result.SetValue(row, Value::LIST(LogicalType::VARCHAR, std::move(values)));
			return true;
		} catch (const std::exception &) {
			result.SetValue(row, Value(result.GetType()));
			return false;
		}
	}
	if (op.parser == "yes_no" || op.return_type == "bool") {
		auto lower = Lower(Trim(work.output));
		if (lower == "yes" || lower == "true") {
			result.SetValue(row, Value::BOOLEAN(true));
			return true;
		}
		if (lower == "no" || lower == "false") {
			result.SetValue(row, Value::BOOLEAN(false));
			return true;
		}
		result.SetValue(row, Value(result.GetType()));
		return false;
	}
	if (op.parser == "score_0_1" || op.return_type == "float8") {
		try {
			size_t used = 0;
			double score = std::stod(Trim(work.output), &used);
			if (used == 0 || !std::isfinite(score) || score < 0 || score > 1) {
				result.SetValue(row, Value(result.GetType()));
				return false;
			}
			result.SetValue(row, Value::DOUBLE(score));
			return true;
		} catch (const std::exception &) {
			result.SetValue(row, Value(result.GetType()));
			return false;
		}
	}
	if (Trim(work.output) == "NULL") {
		result.SetValue(row, Value(result.GetType()));
		return false;
	}
	result.SetValue(row, Value(work.output));
	return true;
}

string FinalCacheKey(const Operator &op, const Json &inputs, const string &model) {
	return Md5Hex("operator\n" + op.name + "\n" + model + "\n" + op.system_prompt + "\n" + op.user_prompt + "\n" +
	              op.parser + "\n" + op.steps + "\n" + op.retry + "\n" + op.wards + "\n" + op.takes + "\n" +
	              inputs.Dump());
}

void ExecuteOne(ClientContext &context, const Operator &op, const Json &inputs, Vector &result, idx_t row) {
	if (op.shape == "aggregate" || op.shape == "rowset" || op.shape == "query") {
		throw InvalidInputException("rvbbit: operator '" + op.name + "' has shape " + op.shape +
		                            ", which is registered but not executed in this version");
	}
	bool blocking = false;
	if (!op.wards.empty() && !Wards(op.wards, "pre", "", inputs, blocking) && blocking) {
		throw InvalidInputException("input rejected by a blocking pre-ward");
	}
	auto model = OperatorModel(context, op);
	string cached;
	auto key = FinalCacheKey(op, inputs, model);
	if (CacheGet(context, key, cached)) {
		RecordCacheHit(context, "chat", model);
		Work hit;
		hit.ok = true;
		hit.output = cached;
		Typed(op, hit, result, row);
		return;
	}
	auto work = ExecuteAttempt(context, op, inputs, "", model);
	work = ApplyRetry(context, op, inputs, std::move(work));
	if (!op.wards.empty()) {
		if (!Wards(op.wards, "post", work.output, inputs, blocking) && blocking) {
			work.ok = false;
			work.error = "output rejected by a blocking post-ward";
		}
	}
	if (work.ok && work.error.empty()) {
		CachePut(context, key, work.output);
	}
	Typed(op, work, result, row);
}

Json InputsFrom(const Operator &op, DataChunk &args, idx_t row, bool &any_null) {
	Json inputs = Json::Object();
	any_null = false;
	for (idx_t col = 0; col < op.arg_names.size() && col < args.ColumnCount(); col++) {
		auto value = args.data[col].GetValue(row);
		if (value.IsNull()) {
			any_null = true;
			return inputs;
		}
		inputs.Set(op.arg_names[col], Json::String(StringValue::Get(value)));
	}
	return inputs;
}

void FinishOperator(ClientContext &context, const Operator &op, const Json &inputs, const string &key, Work work,
                    Vector &result, idx_t row) {
	bool blocking = false;
	if (!op.wards.empty()) {
		if (!Wards(op.wards, "post", work.output, inputs, blocking) && blocking) {
			work.ok = false;
			work.error = "output rejected by a blocking post-ward";
		}
	}
	if (work.ok && work.error.empty()) {
		CachePut(context, key, work.output);
	}
	Typed(op, work, result, row);
}

void RunNamed(const string &name, DataChunk &args, ExpressionState &state, Vector &result) {
	auto op = CopyOf(name);
	auto &context = state.GetContext();
	if (op.shape == "aggregate" || op.shape == "rowset" || op.shape == "query") {
		throw InvalidInputException("rvbbit: operator '" + op.name + "' has shape " + op.shape +
		                            ", which is registered but not executed in this version");
	}
	// Retry, takes, and multi-step operators still run per row. Their HTTP calls
	// go through FetchPost, which dedupes identical bodies. A plain catalog call
	// (sentiment, means, about, ...) is one batched POST for the chunk.
	if (!op.steps.empty() || !op.takes.empty() || !op.retry.empty()) {
		for (idx_t row = 0; row < args.size(); row++) {
			bool any_null = false;
			auto inputs = InputsFrom(op, args, row, any_null);
			if (any_null) {
				result.SetValue(row, Value(result.GetType()));
				continue;
			}
			ExecuteOne(context, op, inputs, result, row);
		}
		return;
	}
	auto model = OperatorModel(context, op);
	auto reasoning = LoadSettings(context).reasoning;
	struct Item {
		idx_t row;
		Json inputs;
		string body;
		string key;
	};
	vector<Item> pending;
	for (idx_t row = 0; row < args.size(); row++) {
		bool any_null = false;
		auto inputs = InputsFrom(op, args, row, any_null);
		if (any_null) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		bool blocking = false;
		if (!op.wards.empty() && !Wards(op.wards, "pre", "", inputs, blocking) && blocking) {
			throw InvalidInputException("input rejected by a blocking pre-ward");
		}
		auto key = FinalCacheKey(op, inputs, model);
		string cached;
		if (CacheGet(context, key, cached)) {
			RecordCacheHit(context, "chat", model);
			Work hit;
			hit.ok = true;
			hit.output = cached;
			Typed(op, hit, result, row);
			continue;
		}
		auto user = Render(op.user_prompt, inputs, nullptr, "", 1);
		pending.push_back(Item {row, std::move(inputs), ChatBody(op, model, op.system_prompt, user, reasoning), key});
	}
	if (pending.empty()) {
		return;
	}
	vector<string> bodies;
	bodies.reserve(pending.size());
	for (auto &item : pending) {
		bodies.push_back(item.body);
	}
	auto fetched = FetchPost(context, "chat", bodies);
	vector<idx_t> retry;
	for (idx_t i = 0; i < pending.size(); i++) {
		Work work;
		if (i >= fetched.size() || !fetched[i].ok) {
			work.error = i >= fetched.size() ? "chat failed" : fetched[i].error;
			FinishOperator(context, op, pending[i].inputs, pending[i].key, work, result, pending[i].row);
			continue;
		}
		string error;
		work.output = ChatContentOf(fetched[i].body, error);
		if (!error.empty()) {
			retry.push_back(i);
			continue;
		}
		work.ok = true;
		FinishOperator(context, op, pending[i].inputs, pending[i].key, std::move(work), result, pending[i].row);
	}
	if (retry.empty()) {
		return;
	}
	vector<string> again;
	for (auto index : retry) {
		again.push_back(pending[index].body);
	}
	auto second = FetchPost(context, "chat", again);
	for (idx_t n = 0; n < retry.size(); n++) {
		auto &item = pending[retry[n]];
		Work work;
		string error;
		if (n >= second.size() || !second[n].ok) {
			work.error = n >= second.size() ? "chat failed" : second[n].error;
		} else {
			work.output = ChatContentOf(second[n].body, error);
		}
		if (!error.empty() || work.output.empty()) {
			throw InvalidInputException(
			    "chat response content was empty. rvbbit_reasoning defaults to disabled so the token budget is not spent on reasoning.");
		}
		work.ok = true;
		FinishOperator(context, op, item.inputs, item.key, std::move(work), result, item.row);
	}
}

#define RVBBIT_OP(fn, opname)                                                                                          \
	void fn(DataChunk &args, ExpressionState &state, Vector &result) {                                                 \
		RunNamed(opname, args, state, result);                                                                         \
	}

RVBBIT_OP(MeansOp, "means")
RVBBIT_OP(AboutOp, "about")
RVBBIT_OP(SummarizeOp, "summarize")
RVBBIT_OP(ClassifyOp, "classify")
RVBBIT_OP(ExtractOp, "extract")
RVBBIT_OP(CondenseOp, "condense")
RVBBIT_OP(SentimentOp, "sentiment")
RVBBIT_OP(ContradictsOp, "contradicts")
RVBBIT_OP(SupportsOp, "supports")
RVBBIT_OP(ImpliesOp, "implies")
RVBBIT_OP(CleanYearOp, "clean_year")
RVBBIT_OP(RedactOp, "redact")
RVBBIT_OP(HeadlineOp, "headline")
RVBBIT_OP(SafeClassifyOp, "safe_classify")
RVBBIT_OP(ThemesOp, "themes")
RVBBIT_OP(TagsOp, "tags")
RVBBIT_OP(AspectsOp, "aspects")
RVBBIT_OP(EntitiesOp, "entities")
RVBBIT_OP(KeyphrasesOp, "keyphrases")

#undef RVBBIT_OP

void ExecFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	for (idx_t row = 0; row < args.size(); row++) {
		auto name_value = args.data[0].GetValue(row);
		if (name_value.IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		auto op = CopyOf(StringValue::Get(name_value));
		Json inputs = Json::Object();
		bool any_null = false;
		for (idx_t col = 1; col < args.ColumnCount() && col - 1 < op.arg_names.size(); col++) {
			auto value = args.data[col].GetValue(row);
			if (value.IsNull()) {
				any_null = true;
				break;
			}
			inputs.Set(op.arg_names[col - 1], Json::String(StringValue::Get(value)));
		}
		if (any_null) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		ExecuteOne(context, op, inputs, result, row);
	}
}

bool InstallMacro(ClientContext &context, const Operator &op) {
	if (op.arg_names.empty() || op.arg_names.size() > 3) {
		return false;
	}
	if (op.shape != "scalar" && op.shape != "dimension") {
		return false;
	}
	for (auto ch : op.name) {
		if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') {
			return false;
		}
	}
	string signature;
	string call = "rvbbit.exec('" + op.name + "'";
	for (auto &arg : op.arg_names) {
		if (!signature.empty()) {
			signature += ", ";
		}
		signature += arg;
		call += ", " + arg;
	}
	call += ")";
	// The rvbbit schema is the extension's system catalog, which rejects CREATE
	// MACRO. Custom operators land in main and are called unqualified.
	auto sql = "CREATE OR REPLACE MACRO main." + op.name + "(" + signature + ") AS " + call;
	try {
		Connection connection(*context.db);
		auto created = connection.Query(sql);
		return created && !created->HasError();
	} catch (const std::exception &) {
		return false;
	}
}

void CreateOperatorFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	for (idx_t row = 0; row < args.size(); row++) {
		if (args.data[0].GetValue(row).IsNull() || args.data[1].GetValue(row).IsNull() ||
		    args.data[2].GetValue(row).IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		Operator op;
		op.name = Trim(StringValue::Get(args.data[0].GetValue(row)));
		auto names = args.data[1].GetValue(row);
		for (auto &child : ListValue::GetChildren(names)) {
			op.arg_names.push_back(StringValue::Get(child));
		}
		op.return_type = Lower(Trim(StringValue::Get(args.data[2].GetValue(row))));
		op.system_prompt = args.data[3].GetValue(row).IsNull() ? "" : StringValue::Get(args.data[3].GetValue(row));
		op.user_prompt = args.data[4].GetValue(row).IsNull() ? "" : StringValue::Get(args.data[4].GetValue(row));
		if (args.ColumnCount() > 5 && !args.data[5].GetValue(row).IsNull()) {
			op.shape = Lower(StringValue::Get(args.data[5].GetValue(row)));
		}
		if (args.ColumnCount() > 6 && !args.data[6].GetValue(row).IsNull()) {
			op.model = StringValue::Get(args.data[6].GetValue(row));
		}
		if (args.ColumnCount() > 7 && !args.data[7].GetValue(row).IsNull()) {
			op.parser = Lower(StringValue::Get(args.data[7].GetValue(row)));
		} else if (op.return_type == "bool") {
			op.parser = "yes_no";
		} else if (op.return_type == "float8") {
			op.parser = "score_0_1";
		} else {
			op.parser = "strip";
		}
		if (args.ColumnCount() > 8 && !args.data[8].GetValue(row).IsNull()) {
			op.max_tokens = args.data[8].GetValue(row).GetValue<int32_t>();
		}
		if (args.ColumnCount() > 9 && !args.data[9].GetValue(row).IsNull()) {
			op.description = StringValue::Get(args.data[9].GetValue(row));
		}
		if (args.ColumnCount() > 10 && !args.data[10].GetValue(row).IsNull()) {
			op.steps = StringValue::Get(args.data[10].GetValue(row));
		}
		if (args.ColumnCount() > 11 && !args.data[11].GetValue(row).IsNull()) {
			op.infix_word = StringValue::Get(args.data[11].GetValue(row));
		}
		if (op.name.empty() || op.arg_names.empty()) {
			throw InvalidInputException("rvbbit.create_operator needs a name and argument names");
		}
		if (op.return_type != "bool" && op.return_type != "text" && op.return_type != "float8" &&
		    op.return_type != "jsonb") {
			throw InvalidInputException("rvbbit.create_operator return_type must be bool, text, float8, or jsonb");
		}
		Upsert(op);
		bool macro = InstallMacro(context, op);
		result.SetValue(row, Value(macro ? string("macro") : string("exec")));
	}
}

void SetJsonField(const string &name, const string &json, void (*apply)(Operator &, const string &)) {
	auto &registry = Catalog();
	std::lock_guard<std::mutex> lock(registry.mu);
	auto *op = FindUnlocked(name);
	if (!op) {
		throw InvalidInputException("rvbbit: unknown operator " + name);
	}
	if (!json.empty()) {
		auto parsed = Json::Parse(json);
		if (!parsed.IsObject() && !parsed.IsNull()) {
			throw InvalidInputException("operator config must be a JSON object");
		}
	}
	apply(*op, json);
}

void SetRetryFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		if (args.data[0].GetValue(row).IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		string name = StringValue::Get(args.data[0].GetValue(row));
		string json = args.data[1].GetValue(row).IsNull() ? "" : StringValue::Get(args.data[1].GetValue(row));
		if (!json.empty() && Json::Parse(json).Find("until") == nullptr) {
			throw InvalidInputException("rvbbit.set_operator_retry: retry_config needs an \"until\" validator");
		}
		SetJsonField(name, json, [](Operator &op, const string &value) { op.retry = value; });
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

void SetWardsFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		if (args.data[0].GetValue(row).IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		string name = StringValue::Get(args.data[0].GetValue(row));
		string json = args.data[1].GetValue(row).IsNull() ? "" : StringValue::Get(args.data[1].GetValue(row));
		SetJsonField(name, json, [](Operator &op, const string &value) { op.wards = value; });
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

void SetTakesFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		if (args.data[0].GetValue(row).IsNull()) {
			result.SetValue(row, Value(result.GetType()));
			continue;
		}
		string name = StringValue::Get(args.data[0].GetValue(row));
		string json = args.data[1].GetValue(row).IsNull() ? "" : StringValue::Get(args.data[1].GetValue(row));
		if (!json.empty()) {
			auto parsed = Json::Parse(json);
			if (!parsed.Find("factor") && !parsed.Find("nodes")) {
				throw InvalidInputException(
				    "rvbbit.set_operator_takes: takes_config needs a \"factor\" or \"nodes\"");
			}
		}
		SetJsonField(name, json, [](Operator &op, const string &value) { op.takes = value; });
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

struct OpsState : public GlobalTableFunctionState {
	vector<Operator> rows;
	idx_t offset = 0;
};

unique_ptr<FunctionData> OpsBind(ClientContext &, TableFunctionBindInput &, vector<LogicalType> &return_types,
                                 vector<Identifier> &names) {
	names = {Identifier("name"),     Identifier("shape"),      Identifier("return_type"), Identifier("arg_names"),
	         Identifier("model"),    Identifier("parser"),     Identifier("infix_word"),  Identifier("description"),
	         Identifier("has_retry"), Identifier("has_wards"), Identifier("has_takes"),   Identifier("has_steps")};
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::BOOLEAN,
	                LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN};
	return make_uniq<TableFunctionData>();
}

unique_ptr<GlobalTableFunctionState> OpsInit(ClientContext &, TableFunctionInitInput &) {
	auto state = make_uniq<OpsState>();
	std::lock_guard<std::mutex> lock(Catalog().mu);
	state->rows = Catalog().ops;
	return std::move(state);
}

void OpsScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<OpsState>();
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &op = state.rows[state.offset++];
		string args;
		for (auto &arg : op.arg_names) {
			if (!args.empty()) {
				args += ",";
			}
			args += arg;
		}
		output.data[0].SetValue(count, Value(op.name));
		output.data[1].SetValue(count, Value(op.shape));
		output.data[2].SetValue(count, Value(op.return_type));
		output.data[3].SetValue(count, Value(args));
		output.data[4].SetValue(count, Value(op.model));
		output.data[5].SetValue(count, Value(op.parser));
		output.data[6].SetValue(count, Value(op.infix_word));
		output.data[7].SetValue(count, Value(op.description));
		output.data[8].SetValue(count, Value::BOOLEAN(!op.retry.empty()));
		output.data[9].SetValue(count, Value::BOOLEAN(!op.wards.empty()));
		output.data[10].SetValue(count, Value::BOOLEAN(!op.takes.empty()));
		output.data[11].SetValue(count, Value::BOOLEAN(!op.steps.empty()));
		count++;
	}
	output.SetCardinalityUnsafe(count);
}

ScalarFunction Fun(const string &name, std::initializer_list<FunctionParameter> params, LogicalType ret,
                   scalar_function_t func) {
	ScalarFunction function(Identifier(name), params, std::move(ret), func);
	function.SetVolatile();
	function.SetFallible();
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

void Add(Operator op) {
	Upsert(std::move(op));
}

void Seed() {
	{
		std::lock_guard<std::mutex> lock(Catalog().mu);
		if (Catalog().seeded) {
			return;
		}
		Catalog().seeded = true;
	}
	auto text = [](const char *name, const char *args, const char *system, const char *user, const char *parser,
	               int tokens, const char *description, const char *infix) {
		Operator op;
		op.name = name;
		op.return_type = std::string(parser) == "yes_no" ? "bool" : (std::string(parser) == "score_0_1" ? "float8" : "text");
		string rest = args;
		string piece;
		for (auto ch : rest) {
			if (ch == ',') {
				op.arg_names.push_back(piece);
				piece.clear();
			} else {
				piece.push_back(ch);
			}
		}
		if (!piece.empty()) {
			op.arg_names.push_back(piece);
		}
		op.system_prompt = system;
		op.user_prompt = user;
		op.parser = parser;
		op.max_tokens = tokens;
		op.description = description;
		op.infix_word = infix ? infix : "";
		Add(std::move(op));
	};
	text("means", "text,criterion",
	     "You are a precise binary classifier. Given a TEXT and a CRITERION, decide whether the TEXT expresses the "
	     "CRITERION. Respond with ONLY one word — either YES or NO — and nothing else.",
	     "CRITERION: {{ criterion }}\n\nTEXT: {{ text }}\n\nDoes TEXT express CRITERION?", "yes_no", 16,
	     "Binary semantic match: does TEXT express CRITERION?", "MEANS");
	text("about", "text,topic",
	     "You are a precise relevance scorer. Rate how relevant the TEXT is to the TOPIC on a scale from 0.0 (not "
	     "relevant at all) to 1.0 (entirely about it). Respond with ONLY the number — for example \"0.73\" — and "
	     "nothing else.",
	     "TOPIC: {{ topic }}\n\nTEXT: {{ text }}\n\nRelevance score (0.0 to 1.0):", "score_0_1", 16,
	     "Relevance score in [0,1] of TEXT to TOPIC.", "ABOUT");
	text("summarize", "text",
	     "You are a concise summarizer. Summarize the given TEXT in ONE short sentence. Output ONLY the sentence — no "
	     "preamble, no quotes, no markdown.",
	     "TEXT: {{ text }}\n\nOne-sentence summary:", "strip", 120, "One-sentence summary of TEXT.", nullptr);
	text("classify", "text,categories",
	     "You are a strict classifier. Given a TEXT and a comma-separated list of CATEGORIES, return ONLY the single "
	     "category name that best matches the TEXT. Use the exact spelling from the list. No explanation, no quotes, "
	     "just the category name.",
	     "CATEGORIES: {{ categories }}\n\nTEXT: {{ text }}\n\nBest category:", "strip", 320,
	     "Classify text into ONE of the comma-separated CATEGORIES.", nullptr);
	text("extract", "text,what",
	     "You are a precise information extractor. Given a TEXT and a description WHAT of the value to find, return "
	     "ONLY the literal value from the text. If the value is not present, return exactly: NULL. No explanation, no "
	     "quotes, no surrounding text.",
	     "TEXT: {{ text }}\n\nWHAT: {{ what }}\n\nExtracted value:", "strip", 640,
	     "Extract a specific value WHAT from TEXT (or NULL).", nullptr);
	text("condense", "text",
	     "You are a concise summarizer. Return a 1-3 sentence summary of the TEXT. Preserve the most important facts, "
	     "names, and numbers. Use plain prose — no bullet points, no preamble like \"Here is a summary\".",
	     "TEXT: {{ text }}\n\nSummary:", "strip", 200, "Condense TEXT into a 1-3 sentence summary.", nullptr);
	text("sentiment", "text",
	     "You are a sentiment classifier. Return ONLY one of: positive, negative, neutral, mixed. Use lowercase. No "
	     "explanation, no period, just the label.",
	     "TEXT: {{ text }}\n\nSentiment:", "strip", 16, "Sentiment label: positive, negative, neutral, or mixed.",
	     nullptr);
	text("contradicts", "a,b",
	     "You are a strict logical relation classifier. Given two statements A and B, decide whether A directly "
	     "CONTRADICTS B. They contradict if both cannot be true at the same time. Respond ONLY with YES or NO.",
	     "A: {{ a }}\n\nB: {{ b }}\n\nDoes A contradict B?", "yes_no", 16, "Does statement A contradict statement B?",
	     "CONTRADICTS");
	text("supports", "a,b",
	     "You are a strict logical relation classifier. Given two statements A and B, decide whether A provides direct "
	     "evidence SUPPORTING B. Respond ONLY with YES or NO.",
	     "A: {{ a }}\n\nB: {{ b }}\n\nDoes A support B?", "yes_no", 16, "Does statement A support statement B?",
	     "SUPPORTS");
	text("implies", "a,b",
	     "You are a strict logical relation classifier. Given two statements A and B, decide whether A logically "
	     "IMPLIES B (if A is true, B must also be true). Respond ONLY with YES or NO.",
	     "A: {{ a }}\n\nB: {{ b }}\n\nDoes A imply B?", "yes_no", 16, "Does statement A logically imply statement B?",
	     "IMPLIES");
	text("clean_year", "text",
	     "You extract the calendar year an event took place from messy text. Respond with ONLY a 4-digit year such as "
	     "1997. Expand two-digit years (97 becomes 1997, 05 becomes 2005). If the text states no year at all, respond "
	     "with exactly: unknown",
	     "TEXT: {{ text }}\n\nYear:", "strip", 120, "Extract a clean 4-digit year from messy text (retry-validated).",
	     nullptr);
	{
		auto op = CopyOf("clean_year");
		op.has_temperature = true;
		op.temperature = 0;
		op.retry = "{\"until\":{\"sql\":\"btrim($output) ~ '^((1[6-9]|20)[0-9]{2}|unknown)$'\"},\"max_attempts\":3,"
		           "\"instructions\":\"Your previous answer was not a valid year. Respond with ONLY a 4-digit year "
		           "such as 1997, or exactly the word: unknown\"}";
		Add(std::move(op));
	}
	text("redact", "text",
	     "You remove personally identifying information from text. Replace each person name with [NAME], email "
	     "address with [EMAIL], phone number with [PHONE], street address with [ADDRESS], and government id number "
	     "with [ID]. Leave place names such as cities, counties and states intact. Return ONLY the rewritten text, "
	     "preserving all other wording.",
	     "TEXT: {{ text }}\n\nRedacted text:", "strip", 1024,
	     "Strip PII from text; post-ward rejects output that still contains an email.", nullptr);
	{
		auto op = CopyOf("redact");
		op.has_temperature = true;
		op.temperature = 0;
		op.wards = "{\"post\":[{\"validator\":{\"sql\":\"$output !~ "
		           "'[A-Za-z0-9._%+-]+@[A-Za-z0-9._-]+'\"},\"mode\":\"blocking\"}]}";
		Add(std::move(op));
	}
	text("headline", "text",
	     "You write one short, punchy headline that captures the single most striking thing in the TEXT. Under 12 "
	     "words. No quotation marks, no trailing period. Return ONLY the headline.",
	     "TEXT: {{ text }}\n\nHeadline:", "strip", 320,
	     "Generate a punchy headline; 3 takes, an LLM evaluator picks the best.", nullptr);
	{
		auto op = CopyOf("headline");
		op.has_temperature = true;
		op.temperature = 0.8;
		op.takes = "{\"factor\":3,\"reduce\":\"evaluator\",\"evaluator\":{\"instructions\":\"Pick the headline that "
		           "is the most vivid and specific while staying accurate to the text. Reply with only its number.\"}}";
		Add(std::move(op));
	}
	{
		Operator op;
		op.name = "safe_classify";
		op.arg_names = {"text", "categories"};
		op.return_type = "text";
		op.parser = "strip";
		op.max_tokens = 160;
		op.system_prompt = "unused — overridden per step";
		op.user_prompt = "unused — overridden per step";
		op.description = "Classify TEXT into one of CATEGORIES. The code step clamps the answer to the allowed list.";
		op.steps = "[{\"name\":\"classify\",\"kind\":\"llm\",\"system\":\"You are a precise text classifier. Reply "
		           "with exactly one of the provided categories, copied verbatim. No quotes, no explanation.\","
		           "\"user\":\"CATEGORIES: {{ inputs.categories }}\\n\\nTEXT: {{ inputs.text }}\\n\\nThe single best "
		           "category from the list above:\",\"max_tokens\":16},{\"name\":\"validate\",\"kind\":\"code\","
		           "\"fn\":\"validate_one_of\",\"inputs\":{\"value\":\"{{ steps.classify.output }}\","
		           "\"allowed\":\"{{ inputs.categories }}\",\"default\":\"unknown\"}}]";
		Add(std::move(op));
	}
	{
		Operator op;
		op.name = "classify_collection";
		op.shape = "aggregate";
		op.arg_names = {"text", "categories"};
		op.return_type = "text";
		op.parser = "strip";
		op.max_tokens = 240;
		op.system_prompt = "Classify this collection of texts into ONE of the provided categories. Consider the "
		                   "overall theme. Return ONLY the category name, exactly as provided.";
		op.user_prompt = "TEXTS:\n{{ text }}\n\nCATEGORIES: {{ categories }}\n\nWhich category best describes this "
		                 "collection as a whole?";
		op.description = "AGGREGATE: one label for a group of texts. Use with GROUP BY.";
		Add(std::move(op));
	}
	{
		Operator op;
		op.name = "summarize_collection";
		op.shape = "aggregate";
		op.arg_names = {"text"};
		op.return_type = "text";
		op.parser = "strip";
		op.max_tokens = 240;
		op.system_prompt = "You summarize a collection of texts in ONE short sentence. Output ONLY the sentence.";
		op.user_prompt = "TEXTS:\n{{ text }}\n\nSummary:";
		op.description = "AGGREGATE: one sentence for a group of texts. Use with GROUP BY.";
		Add(std::move(op));
	}
	{
		Operator op;
		op.name = "consensus";
		op.shape = "aggregate";
		op.arg_names = {"text"};
		op.return_type = "text";
		op.parser = "strip";
		op.max_tokens = 160;
		op.system_prompt = "You state the single consensus of a collection of texts in one short sentence. "
		                   "Output ONLY that sentence.";
		op.user_prompt = "TEXTS:\n{{ text }}\n\nCONSENSUS:";
		op.description = "AGGREGATE: one consensus sentence for a group of texts. Use with GROUP BY.";
		Add(std::move(op));
	}
	auto dimension = [](const char *name, const char *system, const char *user, const char *description) {
		Operator op;
		op.name = name;
		op.shape = "dimension";
		op.arg_names = {"text"};
		op.return_type = "text";
		op.parser = "strip";
		op.max_tokens = 128;
		op.system_prompt = system;
		op.user_prompt = user;
		op.description = description;
		Add(std::move(op));
	};
	dimension("themes",
	          "You are a literary theme analyst. Output ONLY a JSON array of 1 to 5 short Title Case theme labels, "
	          "most central first. Themes are underlying ideas, not literal topics. If the text is too thin, return [].",
	          "TEXT:\n{{ text }}\n\nReturn ONLY a JSON array of canonical theme labels (1-5, Title Case, most central "
	          "first).",
	          "Dimension: one text to a list of canonical themes.");
	dimension("tags",
	          "You are a precise content tagger. Output ONLY a raw JSON array of 3 to 5 short Title Case topical tags. "
	          "No leading hash, no prose. Most defining tag first.",
	          "Read the text and return its filing tags as a JSON array of short Title Case keywords. Output ONLY the "
	          "JSON array.\n\nTEXT:\n{{ text }}\n\nTAGS (JSON array):",
	          "Dimension: one text to a list of filing tags.");
	dimension("aspects",
	          "You identify the concrete features a review discusses. Output ONLY a JSON array of 1 to 5 Title Case "
	          "aspect labels such as Battery Life or Price. Return [] when there is no aspect.",
	          "Identify the canonical aspects discussed in the text below.\n\nTEXT:\n{{ text }}\n\nAspects (JSON array "
	          "of 1-5 short Title-Case labels, ONLY the raw array):",
	          "Dimension: one review to a list of aspects.");
	dimension("entities",
	          "You extract salient named entities: people, organizations, places, and products. Output ONLY a JSON "
	          "array of canonical names, at most 5, most salient first. Return [] when there are none.",
	          "TEXT:\n{{ text }}\n\nSalient named entities, canonicalized — JSON array only:",
	          "Dimension: one text to a list of named entities.");
	dimension("keyphrases",
	          "You extract salient key phrases: 1-5 Title Case noun phrases. Output ONLY a JSON array. Return [] when "
	          "the text has no subject.",
	          "Extract the most salient key phrases describing what this text is about. Return ONLY a JSON array of "
	          "1-5 Title Case strings, most central first.\n\nTEXT:\n{{ text }}\n\nkey phrases (JSON array):",
	          "Dimension: one text to a list of key phrases.");
}

} // namespace

vector<string> EvaluateOperators(ClientContext &context, vector<OperatorCall> calls) {
	vector<string> outputs(calls.size());
	auto reasoning = LoadSettings(context).reasoning;
	struct Pending {
		idx_t index;
		string key;
		string body;
	};
	vector<Pending> pending;
	for (idx_t i = 0; i < calls.size(); i++) {
		auto op = CopyOf(calls[i].name);
		auto model = OperatorModel(context, op);
		auto key = FinalCacheKey(op, calls[i].inputs, model);
		string cached;
		if (CacheGet(context, key, cached)) {
			RecordCacheHit(context, "chat", model);
			outputs[i] = std::move(cached);
			continue;
		}
		auto user = Render(op.user_prompt, calls[i].inputs, nullptr, "", 1);
		pending.push_back(Pending {i, std::move(key), ChatBody(op, model, op.system_prompt, user, reasoning)});
	}
	if (pending.empty()) {
		return outputs;
	}
	auto finish = [&](const vector<FetchResult> &fetched) {
		vector<Pending> again;
		for (idx_t n = 0; n < pending.size(); n++) {
			if (n >= fetched.size() || !fetched[n].ok) {
				throw InvalidInputException(n >= fetched.size() ? "aggregate operator failed" : fetched[n].error);
			}
			string error;
			auto output = ChatContentOf(fetched[n].body, error);
			if (!error.empty() || output.empty()) {
				again.push_back(std::move(pending[n]));
				continue;
			}
			CachePut(context, pending[n].key, output);
			outputs[pending[n].index] = std::move(output);
		}
		pending = std::move(again);
	};
	vector<string> bodies;
	bodies.reserve(pending.size());
	for (auto &item : pending) {
		bodies.push_back(item.body);
	}
	finish(FetchPost(context, "chat", bodies));
	if (!pending.empty()) {
		bodies.clear();
		for (auto &item : pending) {
			bodies.push_back(item.body);
		}
		finish(FetchPost(context, "chat", bodies));
	}
	if (!pending.empty()) {
		throw InvalidInputException(
		    "chat response content was empty. rvbbit_reasoning defaults to disabled so the token budget is not spent on reasoning.");
	}
	return outputs;
}

void RegisterAggregateOperators(ExtensionLoader &loader);

void RegisterOperatorFunctions(ExtensionLoader &loader) {
	Seed();
	auto list = LogicalType::LIST(LogicalType::VARCHAR);
	loader.RegisterFunction(Fun(
	    "means",
	    {FunctionParameter("text", LogicalType::VARCHAR), FunctionParameter("criterion", LogicalType::VARCHAR)},
	    LogicalType::BOOLEAN, MeansOp));
	loader.RegisterFunction(Fun(
	    "about",
	    {FunctionParameter("text", LogicalType::VARCHAR), FunctionParameter("topic", LogicalType::VARCHAR)},
	    LogicalType::DOUBLE, AboutOp));
	loader.RegisterFunction(
	    Fun("summarize", {FunctionParameter("text", LogicalType::VARCHAR)}, LogicalType::VARCHAR, SummarizeOp));
	loader.RegisterFunction(Fun(
	    "classify",
	    {FunctionParameter("text", LogicalType::VARCHAR), FunctionParameter("categories", LogicalType::VARCHAR)},
	    LogicalType::VARCHAR, ClassifyOp));
	loader.RegisterFunction(Fun(
	    "extract",
	    {FunctionParameter("text", LogicalType::VARCHAR), FunctionParameter("what", LogicalType::VARCHAR)},
	    LogicalType::VARCHAR, ExtractOp));
	loader.RegisterFunction(
	    Fun("condense", {FunctionParameter("text", LogicalType::VARCHAR)}, LogicalType::VARCHAR, CondenseOp));
	loader.RegisterFunction(
	    Fun("sentiment", {FunctionParameter("text", LogicalType::VARCHAR)}, LogicalType::VARCHAR, SentimentOp));
	loader.RegisterFunction(Fun(
	    "contradicts", {FunctionParameter("a", LogicalType::VARCHAR), FunctionParameter("b", LogicalType::VARCHAR)},
	    LogicalType::BOOLEAN, ContradictsOp));
	loader.RegisterFunction(Fun(
	    "supports", {FunctionParameter("a", LogicalType::VARCHAR), FunctionParameter("b", LogicalType::VARCHAR)},
	    LogicalType::BOOLEAN, SupportsOp));
	loader.RegisterFunction(Fun(
	    "implies", {FunctionParameter("a", LogicalType::VARCHAR), FunctionParameter("b", LogicalType::VARCHAR)},
	    LogicalType::BOOLEAN, ImpliesOp));
	loader.RegisterFunction(
	    Fun("clean_year", {FunctionParameter("text", LogicalType::VARCHAR)}, LogicalType::VARCHAR, CleanYearOp));
	loader.RegisterFunction(
	    Fun("redact", {FunctionParameter("text", LogicalType::VARCHAR)}, LogicalType::VARCHAR, RedactOp));
	loader.RegisterFunction(
	    Fun("headline", {FunctionParameter("text", LogicalType::VARCHAR)}, LogicalType::VARCHAR, HeadlineOp));
	loader.RegisterFunction(Fun(
	    "safe_classify",
	    {FunctionParameter("text", LogicalType::VARCHAR), FunctionParameter("categories", LogicalType::VARCHAR)},
	    LogicalType::VARCHAR, SafeClassifyOp));
	loader.RegisterFunction(Fun("themes", {FunctionParameter("text", LogicalType::VARCHAR)}, list, ThemesOp));
	loader.RegisterFunction(Fun("tags", {FunctionParameter("text", LogicalType::VARCHAR)}, list, TagsOp));
	loader.RegisterFunction(Fun("aspects", {FunctionParameter("text", LogicalType::VARCHAR)}, list, AspectsOp));
	loader.RegisterFunction(Fun("entities", {FunctionParameter("text", LogicalType::VARCHAR)}, list, EntitiesOp));
	loader.RegisterFunction(Fun("keyphrases", {FunctionParameter("text", LogicalType::VARCHAR)}, list, KeyphrasesOp));
	RegisterAggregateOperators(loader);

	ScalarFunctionSet exec_set("exec");
	exec_set.AddFunction(Fun(
	    "exec", {FunctionParameter("name", LogicalType::VARCHAR), FunctionParameter("a", LogicalType::VARCHAR)},
	    LogicalType::VARCHAR, ExecFunction));
	exec_set.AddFunction(Fun(
	    "exec",
	    {FunctionParameter("name", LogicalType::VARCHAR), FunctionParameter("a", LogicalType::VARCHAR),
	     FunctionParameter("b", LogicalType::VARCHAR)},
	    LogicalType::VARCHAR, ExecFunction));
	exec_set.AddFunction(Fun(
	    "exec",
	    {FunctionParameter("name", LogicalType::VARCHAR), FunctionParameter("a", LogicalType::VARCHAR),
	     FunctionParameter("b", LogicalType::VARCHAR), FunctionParameter("c", LogicalType::VARCHAR)},
	    LogicalType::VARCHAR, ExecFunction));
	loader.RegisterFunction(exec_set);

	loader.RegisterFunction(Fun(
	    "create_operator",
	    {FunctionParameter("name", LogicalType::VARCHAR), FunctionParameter("arg_names", list),
	     FunctionParameter("return_type", LogicalType::VARCHAR),
	     FunctionParameter("system_prompt", LogicalType::VARCHAR, Value("")),
	     FunctionParameter("user_prompt", LogicalType::VARCHAR, Value("")),
	     FunctionParameter("shape", LogicalType::VARCHAR, Value("scalar")),
	     FunctionParameter("model", LogicalType::VARCHAR, Value(DEFAULT_CHAT_MODEL)),
	     FunctionParameter("parser", LogicalType::VARCHAR, Value(LogicalType::VARCHAR)),
	     FunctionParameter("max_tokens", LogicalType::INTEGER, Value::INTEGER(256)),
	     FunctionParameter("description", LogicalType::VARCHAR, Value("")),
	     FunctionParameter("steps", LogicalType::VARCHAR, Value(LogicalType::VARCHAR)),
	     FunctionParameter("infix_word", LogicalType::VARCHAR, Value(""))},
	    LogicalType::VARCHAR, CreateOperatorFunction));
	loader.RegisterFunction(Fun(
	    "set_operator_retry",
	    {FunctionParameter("name", LogicalType::VARCHAR),
	     FunctionParameter("retry_config", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))},
	    LogicalType::BOOLEAN, SetRetryFunction));
	loader.RegisterFunction(Fun(
	    "set_operator_wards",
	    {FunctionParameter("name", LogicalType::VARCHAR),
	     FunctionParameter("wards_config", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))},
	    LogicalType::BOOLEAN, SetWardsFunction));
	loader.RegisterFunction(Fun(
	    "set_operator_takes",
	    {FunctionParameter("name", LogicalType::VARCHAR),
	     FunctionParameter("takes_config", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))},
	    LogicalType::BOOLEAN, SetTakesFunction));
	TableFunction operators(Identifier("operators"), {}, OpsScan, OpsBind, OpsInit);
	loader.RegisterFunction(operators);
}

} // namespace rvbbit
} // namespace duckdb
