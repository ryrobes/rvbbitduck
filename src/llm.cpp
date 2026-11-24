#include "engine.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>

namespace duckdb {
namespace rvbbit {

namespace {

string Trim(string value) {
	auto not_space = [](unsigned char ch) { return !std::isspace(ch); };
	value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
	value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
	return value;
}

bool ArgString(const Vector &vec, idx_t row, string &out) {
	auto value = vec.GetValue(row);
	if (value.IsNull()) {
		return false;
	}
	out = StringValue::Get(value);
	return true;
}

void SetNull(Vector &result, idx_t row) {
	result.SetValue(row, Value(result.GetType()));
}

Json Message(const string &role, const string &content) {
	Json message = Json::Object();
	message.Set("role", Json::String(role));
	message.Set("content", Json::String(content));
	return message;
}

string ChatBody(const string &model, const string &system, const string &user, int max_tokens, const string &reasoning) {
	Json messages = Json::Array();
	messages.arr.push_back(Message("system", system));
	messages.arr.push_back(Message("user", user));
	Json body = Json::Object();
	body.Set("model", Json::String(model));
	body.Set("messages", std::move(messages));
	body.Set("temperature", Json::Number(0));
	body.Set("max_tokens", Json::Number(max_tokens));
	SetReasoning(body, reasoning);
	return body.Dump();
}

string ChatContent(const string &response, string &error) {
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

enum class ChatParser : uint8_t { YES_NO, SCORE, TEXT };

struct ChatJob {
	idx_t row = 0;
	string body;
	ChatParser parser = ChatParser::TEXT;
};

string ApplyParser(ChatParser parser, const string &content, bool &ok) {
	ok = true;
	if (parser == ChatParser::YES_NO) {
		auto lower = content;
		for (auto &ch : lower) {
			ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
		}
		if (lower == "yes" || lower == "true") {
			return "true";
		}
		if (lower == "no" || lower == "false") {
			return "false";
		}
		ok = false;
		return "";
	}
	if (parser == ChatParser::SCORE) {
		try {
			size_t used = 0;
			double score = std::stod(content, &used);
			if (used == 0 || !std::isfinite(score) || score < 0 || score > 1) {
				ok = false;
				return "";
			}
			return std::to_string(score);
		} catch (const std::exception &) {
			ok = false;
			return "";
		}
	}
	if (content == "NULL") {
		ok = false;
		return "";
	}
	return content;
}

unordered_map<string, string> ResolveChats(ClientContext &context, const vector<string> &bodies) {
	unordered_map<string, string> resolved;
	vector<string> missing;
	for (auto &body : bodies) {
		if (resolved.find(body) != resolved.end() || std::find(missing.begin(), missing.end(), body) != missing.end()) {
			continue;
		}
		string cached;
		if (CacheGet(context, Md5Hex(string("chat\n") + body), cached)) {
			resolved[body] = std::move(cached);
		} else {
			missing.push_back(body);
		}
	}
	if (!missing.empty()) {
		auto fetched = FetchPost(context, "chat", missing);
		vector<string> retry;
		for (idx_t i = 0; i < missing.size(); i++) {
			if (!fetched[i].ok) {
				continue;
			}
			string error;
			auto content = ChatContent(fetched[i].body, error);
			if (!error.empty() || content.empty()) {
				retry.push_back(missing[i]);
				continue;
			}
			CachePut(context, Md5Hex(string("chat\n") + missing[i]), content);
			resolved[missing[i]] = content;
		}
		if (!retry.empty()) {
			auto again = FetchPost(context, "chat", retry);
			for (idx_t i = 0; i < retry.size(); i++) {
				if (i >= again.size() || !again[i].ok) {
					continue;
				}
				string error;
				auto content = ChatContent(again[i].body, error);
				if (!error.empty() || content.empty()) {
					throw InvalidInputException(
					    "chat response content was empty. rvbbit_reasoning defaults to disabled so the token budget is not spent on reasoning.");
				}
				CachePut(context, Md5Hex(string("chat\n") + retry[i]), content);
				resolved[retry[i]] = content;
			}
		}
	}
	return resolved;
}

void RunChat(DataChunk &args, ExpressionState &state, Vector &result, ChatParser parser,
             const std::function<string(const vector<string> &, int &)> &prompt) {
	auto &context = state.GetContext();
	auto provider = ResolveProvider(context, LoadSettings(context));
	vector<string> bodies(args.size());
	vector<char> live(args.size(), 0);
	for (idx_t row = 0; row < args.size(); row++) {
		vector<string> inputs;
		bool any_null = false;
		for (idx_t col = 0; col < args.ColumnCount(); col++) {
			string value;
			if (!ArgString(args.data[col], row, value)) {
				any_null = true;
				break;
			}
			inputs.push_back(std::move(value));
		}
		if (any_null) {
			SetNull(result, row);
			continue;
		}
		int max_tokens = 64;
		auto user = prompt(inputs, max_tokens);
		string system = inputs.back();
		inputs.pop_back();
		(void)inputs;
		bodies[row] = user;
		live[row] = 1;
		(void)system;
		(void)max_tokens;
	}
	// The prompt lambda returns the full request body. Rebuild cleanly below.
	live.assign(args.size(), 0);
	for (idx_t row = 0; row < args.size(); row++) {
		vector<string> inputs;
		bool any_null = false;
		for (idx_t col = 0; col < args.ColumnCount(); col++) {
			string value;
			if (!ArgString(args.data[col], row, value)) {
				any_null = true;
				break;
			}
			if (Trim(value).empty() && col == 0) {
				any_null = true;
				break;
			}
			inputs.push_back(std::move(value));
		}
		if (any_null) {
			SetNull(result, row);
			continue;
		}
		int max_tokens = 64;
		bodies[row] = prompt(inputs, max_tokens);
		// prompt returns the user text. Pair it with a system prompt stashed in max_tokens' sibling via a convention:
		// the returned string is "system\n\x1euser" so one function builds both.
		live[row] = 1;
	}
	vector<string> requests(args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		if (!live[row]) {
			continue;
		}
		auto split = bodies[row].find("\n\x1e");
		if (split == string::npos) {
			SetNull(result, row);
			live[row] = 0;
			continue;
		}
		auto system = bodies[row].substr(0, split);
		auto user = bodies[row].substr(split + 2);
		auto max_pos = user.rfind("\n\x1f");
		int max_tokens = 64;
		if (max_pos != string::npos) {
			max_tokens = std::stoi(user.substr(max_pos + 2));
			user = user.substr(0, max_pos);
		}
		requests[row] = ChatBody(provider.chat_model, system, user, max_tokens, LoadSettings(context).reasoning);
	}
	vector<string> needed;
	for (idx_t row = 0; row < args.size(); row++) {
		if (live[row]) {
			needed.push_back(requests[row]);
		}
	}
	auto resolved = needed.empty() ? unordered_map<string, string> {} : ResolveChats(context, needed);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!live[row]) {
			continue;
		}
		auto found = resolved.find(requests[row]);
		if (found == resolved.end()) {
			SetNull(result, row);
			continue;
		}
		bool ok = false;
		auto parsed = ApplyParser(parser, found->second, ok);
		if (!ok) {
			SetNull(result, row);
			continue;
		}
		if (result.GetType().id() == LogicalTypeId::BOOLEAN) {
			result.SetValue(row, Value::BOOLEAN(parsed == "true"));
		} else if (result.GetType().id() == LogicalTypeId::DOUBLE) {
			result.SetValue(row, Value::DOUBLE(std::stod(parsed)));
		} else {
			result.SetValue(row, Value(parsed));
		}
	}
}

string Pack(const string &system, const string &user, int max_tokens) {
	return system + "\n\x1e" + user + "\n\x1f" + std::to_string(max_tokens);
}

void PromptFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	RunChat(args, state, result, ChatParser::TEXT, [](const vector<string> &inputs, int &) {
		return Pack("You follow the instruction exactly. Return only the requested answer, with no preamble.",
		            "INSTRUCTION: " + inputs[1] + "\n\nTEXT: " + inputs[0], 800);
	});
}

ScalarFunction VolatileFun(const string &name, std::initializer_list<FunctionParameter> params, LogicalType ret,
                           scalar_function_t func) {
	ScalarFunction fun(Identifier(name), params, std::move(ret), func);
	fun.SetVolatile();
	fun.SetFallible();
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace

void RegisterLlmFunctions(ExtensionLoader &loader) {
	loader.RegisterFunction(VolatileFun(
	    "prompt",
	    {FunctionParameter("text", LogicalType::VARCHAR), FunctionParameter("instruction", LogicalType::VARCHAR)},
	    LogicalType::VARCHAR, PromptFunction));
}

} // namespace rvbbit
} // namespace duckdb
