#include "engine.hpp"

#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/crypto/md5.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cmath>
#include <iterator>
#include <mutex>
#include <thread>

namespace duckdb {
namespace rvbbit {

namespace {

string TrimCopy(string value) {
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
		value.erase(value.begin());
	}
	while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
		value.pop_back();
	}
	return value;
}

string LowerCopy(string value) {
	for (auto &ch : value) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return value;
}

bool ContainsLower(const string &haystack, const string &needle) {
	if (needle.empty()) {
		return false;
	}
	return LowerCopy(haystack).find(LowerCopy(needle)) != string::npos;
}

string SettingString(ClientContext &context, const char *name, const string &fallback) {
	Value value;
	if (!context.TryGetCurrentSetting(Identifier(name), value) || value.IsNull()) {
		return fallback;
	}
	return value.ToString();
}

int32_t SettingInt(ClientContext &context, const char *name, int32_t fallback) {
	Value value;
	if (!context.TryGetCurrentSetting(Identifier(name), value) || value.IsNull()) {
		return fallback;
	}
	return value.GetValue<int32_t>();
}

bool SettingBool(ClientContext &context, const char *name, bool fallback) {
	Value value;
	if (!context.TryGetCurrentSetting(Identifier(name), value) || value.IsNull()) {
		return fallback;
	}
	return value.GetValue<bool>();
}

string TrimSlash(string url) {
	while (!url.empty() && url.back() == '/') {
		url.pop_back();
	}
	return url;
}

struct LogEntry {
	int64_t seq;
	string kind;
	string url;
	int32_t status;
	string body;
};

struct NoulOverride {
	string state_substr;
	string instruction_substr;
	double score;
};

struct CacheEntry {
	string key;
	string payload;
};

class Engine {
public:
	static Engine &Get() {
		static Engine engine;
		return engine;
	}

	std::mutex mu;
	std::condition_variable cv;
	int in_flight = 0;
	int limit = 8;

	int64_t hits = 0;
	int64_t misses = 0;
	int64_t l2_hits = 0;
	int64_t http_calls = 0;
	int32_t cache_capacity = 10000;
	vector<CacheEntry> lru;

	vector<LogEntry> log;
	int64_t log_seq = 0;

	int32_t fail_remaining = 0;
	int32_t fail_status = 500;
	vector<NoulOverride> noul_overrides;

	unordered_map<string, string> trees;

	unique_ptr<DuckDB> cache_db;
	unique_ptr<Connection> cache_con;
	string cache_path;

	bool grammar_registered = false;

	// The nested cache database shares this process with the DuckDB that loaded
	// the extension. Destroying it from this singleton's destructor runs after
	// DuckDB's own teardown and corrupts the heap, which fails the test runner
	// even when every assertion passed. The OS reclaims the leak at exit.
	~Engine() {
		cache_con.release();
		cache_db.release();
	}

	void Acquire(ClientContext &context) {
		std::unique_lock<std::mutex> lock(mu);
		while (in_flight >= limit) {
			if (context.IsInterrupted()) {
				throw InterruptException();
			}
			cv.wait_for(lock, std::chrono::milliseconds(50));
		}
		in_flight++;
	}

	void Release() {
		std::lock_guard<std::mutex> lock(mu);
		if (in_flight > 0) {
			in_flight--;
		}
		cv.notify_one();
	}

	void SetLimit(int32_t value) {
		std::lock_guard<std::mutex> lock(mu);
		limit = std::max(1, static_cast<int>(value));
	}

	void NoteHttp(const string &kind, const string &url, int status, const string &body) {
		std::lock_guard<std::mutex> lock(mu);
		http_calls++;
		LogEntry entry;
		entry.seq = ++log_seq;
		entry.kind = kind;
		entry.url = url;
		entry.status = status;
		entry.body = body.size() > 4000 ? body.substr(0, 4000) : body;
		log.push_back(std::move(entry));
		if (log.size() > 128) {
			log.erase(log.begin(), log.begin() + static_cast<idx_t>(log.size() - 128));
		}
	}

	bool LookupL1(const string &key, string &payload) {
		for (idx_t i = 0; i < lru.size(); i++) {
			if (lru[i].key == key) {
				payload = lru[i].payload;
				auto entry = std::move(lru[i]);
				lru.erase(lru.begin() + static_cast<idx_t>(i));
				lru.push_back(std::move(entry));
				hits++;
				return true;
			}
		}
		misses++;
		return false;
	}

	void StoreL1(const string &key, const string &payload) {
		for (auto &entry : lru) {
			if (entry.key == key) {
				entry.payload = payload;
				return;
			}
		}
		if (cache_capacity > 0 && static_cast<int32_t>(lru.size()) >= cache_capacity) {
			lru.erase(lru.begin());
		}
		lru.push_back(CacheEntry {key, payload});
	}
};

string Endpoint(const ProviderConfig &provider, const string &kind) {
	if (kind == "chat") {
		return provider.base_url + "/api/v1/chat/completions";
	}
	if (kind == "embed") {
		return provider.base_url + "/api/v1/embeddings";
	}
	return provider.base_url + "/api/alpha/decisions";
}

void EnsureCache(Engine &engine, const string &path) {
	if (path.empty()) {
		return;
	}
	if (engine.cache_con && engine.cache_path == path) {
		return;
	}
	engine.cache_con.reset();
	engine.cache_db.reset();
	engine.cache_path = path;
	engine.cache_db = make_uniq<DuckDB>(path);
	engine.cache_con = make_uniq<Connection>(*engine.cache_db);
	auto created = engine.cache_con->Query(
	    "CREATE TABLE IF NOT EXISTS judgments(cache_key VARCHAR PRIMARY KEY, payload VARCHAR NOT NULL);"
	    "CREATE TABLE IF NOT EXISTS trees(name VARCHAR PRIMARY KEY, tree_json VARCHAR NOT NULL);");
	if (created->HasError()) {
		throw InvalidInputException("rvbbit persistent cache: " + created->GetError());
	}
}

string QueryOne(Connection &con, const string &sql, bool &found) {
	found = false;
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException("rvbbit persistent cache: " + result->GetError());
	}
	auto chunk = result->Fetch();
	if (!chunk || chunk->size() == 0) {
		return "";
	}
	auto value = chunk->data[0].GetValue(0);
	if (value.IsNull()) {
		return "";
	}
	found = true;
	return value.ToString();
}

void ExecCache(Connection &con, const string &sql) {
	auto result = con.Query(sql);
	if (result->HasError()) {
		throw InvalidInputException("rvbbit persistent cache: " + result->GetError());
	}
}

string SqlQuote(const string &value) {
	string out = "'";
	for (auto ch : value) {
		if (ch == '\'') {
			out += "''";
		} else {
			out.push_back(ch);
		}
	}
	out.push_back('\'');
	return out;
}

vector<string> Words(const string &text) {
	string lower = LowerCopy(text);
	vector<string> words;
	string current;
	for (auto ch : lower) {
		if (std::isalnum(static_cast<unsigned char>(ch))) {
			current.push_back(ch);
		} else if (!current.empty()) {
			if (current.size() >= 4) {
				words.push_back(current);
			}
			current.clear();
		}
	}
	if (current.size() >= 4) {
		words.push_back(current);
	}
	std::sort(words.begin(), words.end());
	words.erase(std::unique(words.begin(), words.end()), words.end());
	return words;
}

bool Overlaps(const string &left, const string &right) {
	auto a = Words(left);
	auto b = Words(right);
	vector<string> both;
	std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(both));
	return !both.empty();
}

string FocusInstruction(string instructions) {
	string boundary = EVIDENCE_BOUNDARY;
	if (instructions.rfind(boundary, 0) == 0) {
		instructions = instructions.substr(boundary.size());
	}
	auto marker = instructions.rfind("Question: ");
	if (marker != string::npos) {
		instructions = instructions.substr(marker + 10);
	}
	return instructions;
}

string StateText(const Json &state) {
	if (state.IsString()) {
		return state.str;
	}
	return state.TextBlob();
}

Json MockDecision(const Json &request) {
	auto model = request.Find("model");
	string requested = model && model->IsString() ? model->str : DEFAULT_DECISION_MODEL;
	auto state = request.Find("state");
	auto questions = request.Find("questions");
	if (!state || !questions || !questions->IsObject()) {
		throw std::runtime_error("mock decision request is missing state or questions");
	}
	string state_text = StateText(*state);
	Json answers = Json::Object();
	auto &engine = Engine::Get();
	for (auto &entry : questions->obj) {
		auto &question = entry.second;
		auto type = question.Find("type");
		auto instructions = question.Find("instructions");
		if (!type || !type->IsString() || !instructions || !instructions->IsString()) {
			throw std::runtime_error("mock decision question is malformed");
		}
		string focus = FocusInstruction(instructions->str);
		if (type->str == "noul") {
			double score = Overlaps(state_text, focus) ? 0.91 : 0.12;
			std::lock_guard<std::mutex> lock(engine.mu);
			for (auto &rule : engine.noul_overrides) {
				if (ContainsLower(state_text, rule.state_substr) && ContainsLower(focus, rule.instruction_substr)) {
					score = rule.score;
					break;
				}
			}
			Json answer = Json::Object();
			answer.Set("type", Json::String("noul"));
			answer.Set("noul", Json::Number(score));
			answers.Set(entry.first, std::move(answer));
		} else if (type->str == "choice") {
			auto criteria = question.Find("criteria");
			if (!criteria || !criteria->IsObject() || criteria->obj.size() < 2) {
				throw std::runtime_error("mock choice question needs criteria");
			}
			idx_t best = criteria->obj.size() - 1;
			bool any = false;
			for (idx_t i = 0; i < criteria->obj.size(); i++) {
				string blob = criteria->obj[i].first + " " +
				              (criteria->obj[i].second.IsString() ? criteria->obj[i].second.str : "");
				if (Overlaps(state_text, blob)) {
					best = i;
					any = true;
					break;
				}
			}
			if (!any) {
				for (idx_t i = 0; i < criteria->obj.size(); i++) {
					auto label = LowerCopy(criteria->obj[i].first);
					if (label == "other" || label == "unclear") {
						best = i;
						break;
					}
				}
			}
			double winner = any ? 0.8 : 0.7;
			double rest = criteria->obj.size() == 1 ? 0 : (1.0 - winner) / static_cast<double>(criteria->obj.size() - 1);
			Json probs = Json::Object();
			for (idx_t i = 0; i < criteria->obj.size(); i++) {
				probs.Set(criteria->obj[i].first, Json::Number(i == best ? winner : rest));
			}
			Json answer = Json::Object();
			answer.Set("type", Json::String("choice"));
			answer.Set("choice", Json::String(criteria->obj[best].first));
			answer.Set("probabilities", std::move(probs));
			answers.Set(entry.first, std::move(answer));
		} else if (type->str == "score") {
			auto criteria = question.Find("criteria");
			if (!criteria || !criteria->IsArray() || criteria->arr.size() < 2) {
				throw std::runtime_error("mock score question needs a criteria array");
			}
			idx_t best = criteria->arr.size() / 2;
			bool any = false;
			for (idx_t i = 0; i < criteria->arr.size(); i++) {
				if (criteria->arr[i].IsString() && Overlaps(state_text, criteria->arr[i].str)) {
					best = i;
					any = true;
					break;
				}
			}
			Json legend = Json::Object();
			Json probs = Json::Object();
			double winner = any ? 0.8 : 0.55;
			double rest = criteria->arr.size() == 1 ? 0 : (1.0 - winner) / static_cast<double>(criteria->arr.size() - 1);
			for (idx_t i = 0; i < criteria->arr.size(); i++) {
				string key = std::to_string(i);
				legend.Set(key, criteria->arr[i].IsString() ? Json::String(criteria->arr[i].str) : Json::String(""));
				probs.Set(key, Json::Number(i == best ? winner : rest));
			}
			Json answer = Json::Object();
			answer.Set("type", Json::String("score"));
			answer.Set("score", Json::Number(static_cast<double>(best)));
			answer.Set("legend", std::move(legend));
			answer.Set("probabilities", std::move(probs));
			answer.Set("confidence", Json::Number(any ? 0.8 : 0.4));
			answers.Set(entry.first, std::move(answer));
		} else if (type->str == "box") {
			bool hit = Overlaps(state_text, focus);
			double top = hit ? 0.2 : 0.55;
			Json box = Json::Object();
			box.Set("top", Json::Number(top));
			box.Set("bottom", Json::Number(std::min(1.0, top + 0.3)));
			box.Set("left", Json::Number(0.15));
			box.Set("right", Json::Number(0.7));
			Json answer = Json::Object();
			answer.Set("type", Json::String("box"));
			answer.Set("box", std::move(box));
			answer.Set("confidence", Json::Number(hit ? 0.82 : 0.31));
			answers.Set(entry.first, std::move(answer));
		} else {
			throw std::runtime_error("mock does not implement question type " + type->str);
		}
	}
	Json response = Json::Object();
	response.Set("model", Json::String(requested + "-mock"));
	response.Set("answers", std::move(answers));
	Json usage = Json::Object();
	usage.Set("cost", Json::Number(0));
	usage.Set("input_tokens", Json::Number(1));
	usage.Set("output_tokens", Json::Number(1));
	usage.Set("total_tokens", Json::Number(2));
	response.Set("usage", std::move(usage));
	return response;
}

string Between(const string &text, const string &start, const string &end) {
	auto at = text.find(start);
	if (at == string::npos) {
		return "";
	}
	at += start.size();
	auto stop = text.find(end, at);
	if (stop == string::npos) {
		return TrimCopy(text.substr(at));
	}
	return TrimCopy(text.substr(at, stop - at));
}

string MockChatContent(const Json &request) {
	string user;
	auto messages = request.Find("messages");
	if (messages && messages->IsArray()) {
		for (auto &message : messages->arr) {
			auto role = message.Find("role");
			auto content = message.Find("content");
			if (role && role->IsString() && role->str == "user" && content && content->IsString()) {
				user = content->str;
			}
		}
	}
	if (user.find("CRITERION:") != string::npos) {
		auto criterion = Between(user, "CRITERION:", "TEXT:");
		auto text = Between(user, "TEXT:", "Does TEXT");
		return Overlaps(text, criterion) ? "YES" : "NO";
	}
	if (user.find("TOPIC:") != string::npos) {
		auto topic = Between(user, "TOPIC:", "TEXT:");
		auto text = Between(user, "TEXT:", "Relevance");
		return Overlaps(text, topic) ? "0.86" : "0.07";
	}
	if (user.find("WHAT:") != string::npos) {
		auto text = Between(user, "TEXT:", "WHAT:");
		string digits;
		for (auto ch : text) {
			if (std::isdigit(static_cast<unsigned char>(ch))) {
				digits.push_back(ch);
			} else if (!digits.empty()) {
				break;
			}
		}
		return digits.empty() ? "NULL" : digits;
	}
	if (user.find("CATEGORIES:") != string::npos) {
		auto categories = Between(user, "CATEGORIES:", "TEXT:");
		auto text = Between(user, "TEXT:", "Best category");
		if (text.empty()) {
			text = Between(user, "TEXT:", "The single");
		}
		string chosen;
		string current;
		auto consider = [&](const string &label) {
			auto trimmed = TrimCopy(label);
			if (trimmed.empty()) {
				return;
			}
			if (chosen.empty()) {
				chosen = trimmed;
			}
			if (Overlaps(text, trimmed) || ContainsLower(text, trimmed)) {
				chosen = trimmed;
			}
		};
		for (auto ch : categories) {
			if (ch == ',') {
				consider(current);
				current.clear();
			} else {
				current.push_back(ch);
			}
		}
		consider(current);
		return chosen.empty() ? "other" : chosen;
	}
	if (user.find("One-sentence summary:") != string::npos || user.find("Summary:") != string::npos) {
		auto text = Between(user, "TEXT:", user.find("Summary:") != string::npos ? "Summary:" : "One-sentence");
		if (text.size() > 80) {
			text = text.substr(0, 80);
		}
		return "Summary of: " + text;
	}
	if (user.find("Does A contradict") != string::npos || user.find("Does A support") != string::npos ||
	    user.find("Does A imply") != string::npos) {
		auto a = Between(user, "A:", "B:");
		auto b = Between(user, "B:", "Does A");
		return Overlaps(a, b) ? "YES" : "NO";
	}
	if (user.find("\n\nYear:") != string::npos) {
		auto text = Between(user, "TEXT:", "Year:");
		auto year_of = [](const string &blob) {
			for (size_t i = 0; i + 4 <= blob.size(); i++) {
				if (std::isdigit(static_cast<unsigned char>(blob[i])) &&
				    std::isdigit(static_cast<unsigned char>(blob[i + 1])) &&
				    std::isdigit(static_cast<unsigned char>(blob[i + 2])) &&
				    std::isdigit(static_cast<unsigned char>(blob[i + 3])) &&
				    (i == 0 || !std::isdigit(static_cast<unsigned char>(blob[i - 1])))) {
					return blob.substr(i, 4);
				}
			}
			for (size_t i = 0; i + 2 <= blob.size(); i++) {
				if (std::isdigit(static_cast<unsigned char>(blob[i])) &&
				    std::isdigit(static_cast<unsigned char>(blob[i + 1])) &&
				    (i == 0 || !std::isdigit(static_cast<unsigned char>(blob[i - 1]))) &&
				    (i + 2 == blob.size() || !std::isdigit(static_cast<unsigned char>(blob[i + 2])))) {
					int yy = (blob[i] - '0') * 10 + (blob[i + 1] - '0');
					return std::to_string(yy >= 30 ? 1900 + yy : 2000 + yy);
				}
			}
			return string("unknown");
		};
		// The first attempt is deliberately invalid so the retry plan runs.
		if (user.find("not a valid year") == string::npos && user.find("previous answer") == string::npos) {
			return "nope";
		}
		return year_of(text);
	}
	if (user.find("Redacted text:") != string::npos) {
		auto text = Between(user, "TEXT:", "Redacted");
		string out;
		for (size_t i = 0; i < text.size();) {
			auto at = text.find('@', i);
			if (at == string::npos || at == 0) {
				out.append(text, i, string::npos);
				break;
			}
			size_t start = at;
			while (start > 0 && text[start - 1] != ' ') {
				start--;
			}
			size_t end = at + 1;
			while (end < text.size() && text[end] != ' ') {
				end++;
			}
			out.append(text, i, start - i);
			out += "[EMAIL]";
			i = end;
		}
		return out;
	}
	if (user.find("\n\nHeadline:") != string::npos) {
		auto text = Between(user, "TEXT:", "Headline:");
		if (text.size() > 48) {
			text = text.substr(0, 48);
		}
		return "Headline: " + text;
	}
	if (user.find("CANDIDATE ANSWERS:") != string::npos || user.find("ONLY the number") != string::npos) {
		return "1";
	}
	if (user.find("\n\nSentiment:") != string::npos) {
		auto text = LowerCopy(Between(user, "TEXT:", "Sentiment:"));
		if (text.find("cancel") != string::npos || text.find("angry") != string::npos ||
		    text.find("terrible") != string::npos) {
			return "negative";
		}
		if (text.find("thanks") != string::npos || text.find("love") != string::npos) {
			return "positive";
		}
		return "neutral";
	}
	if (user.find("JSON array") != string::npos) {
		auto lower = LowerCopy(user);
		string items;
		auto add = [&](const char *word, const char *label) {
			if (lower.find(word) == string::npos) {
				return;
			}
			if (!items.empty()) {
				items += ",";
			}
			items += "\"";
			items += label;
			items += "\"";
		};
		add("cancel", "Cancellation");
		add("billing", "Billing");
		add("shipping", "Shipping");
		if (items.empty()) {
			return "[\"General\"]";
		}
		return "[" + items + "]";
	}
	auto text = Between(user, "TEXT:", "INSTRUCTION:");
	if (text.empty()) {
		text = user;
	}
	if (text.size() > 80) {
		text = text.substr(0, 80);
	}
	return "answer: " + text;
}

Json MockChat(const Json &request) {
	auto model = request.Find("model");
	string requested = model && model->IsString() ? model->str : DEFAULT_CHAT_MODEL;
	Json message = Json::Object();
	message.Set("role", Json::String("assistant"));
	message.Set("content", Json::String(MockChatContent(request)));
	Json choice = Json::Object();
	choice.Set("message", std::move(message));
	Json choices = Json::Array();
	choices.arr.push_back(std::move(choice));
	Json response = Json::Object();
	response.Set("model", Json::String(requested));
	response.Set("choices", std::move(choices));
	return response;
}

bool Retryable(const FetchResult &result) {
	return result.status == 429 || result.status == 408 || (result.status >= 500 && result.status <= 599) ||
	       result.status == 0;
}

//! 401, 403, and every other 4xx except the retryable ones (408, 429).
bool RejectedStatus(int status) {
	return status >= 400 && status <= 499 && status != 408 && status != 429;
}

void RejectClientError(const FetchResult &result) {
	if (!RejectedStatus(result.status)) {
		return;
	}
	string detail = "request was rejected";
	if (result.status == 401) {
		detail = "invalid or missing API key";
	} else if (result.status == 403) {
		detail = "forbidden";
	} else if (result.status == 400) {
		detail = "invalid request";
	}
	throw InvalidInputException("rvbbit: OpenRouter rejected the request (HTTP " + std::to_string(result.status) +
	                            "): " + detail);
}

int RetryDelayMs(const FetchResult &result, int32_t base_ms, int attempt) {
	auto marker = result.error.find("retry-after=");
	if (marker != string::npos) {
		try {
			int seconds = std::stoi(result.error.substr(marker + 12));
			if (seconds > 0) {
				return std::min(seconds, 5) * 1000;
			}
		} catch (const std::exception &) {
		}
	}
	int delay = base_ms;
	for (int i = 1; i < attempt; i++) {
		delay = std::min(delay * 2, 2000);
	}
	return std::max(delay, 0);
}

vector<double> HashVector(const string &text, int dim) {
	vector<double> values(static_cast<size_t>(dim), 0.0);
	auto words = Words(text);
	if (words.empty()) {
		words.push_back(LowerCopy(text));
	}
	for (auto &word : words) {
		uint32_t hash = 2166136261u;
		for (unsigned char ch : word) {
			hash ^= ch;
			hash *= 16777619u;
		}
		for (int i = 0; i < dim; i++) {
			uint32_t mixed = hash + static_cast<uint32_t>(i) * 374761393u;
			double sign = (mixed & 1u) ? 1.0 : -1.0;
			values[static_cast<size_t>(i)] += sign * (0.25 + static_cast<double>(mixed % 1000) / 4000.0);
		}
	}
	double norm = 0;
	for (auto value : values) {
		norm += value * value;
	}
	norm = std::sqrt(norm);
	if (norm == 0) {
		values[0] = 1;
		return values;
	}
	for (auto &value : values) {
		value /= norm;
	}
	return values;
}

Json MockEmbeddings(const Json &request) {
	auto input = request.Find("input");
	if (!input || !input->IsArray() || input->arr.empty()) {
		throw std::runtime_error("embeddings input must be a non-empty array");
	}
	int dim = 1024;
	if (auto dimensions = request.Find("dimensions"); dimensions && dimensions->IsNumber()) {
		dim = static_cast<int>(dimensions->num);
	}
	if (dim < 2) {
		dim = 2;
	}
	if (dim > 4096) {
		dim = 4096;
	}
	Json data = Json::Array();
	for (idx_t i = 0; i < input->arr.size(); i++) {
		if (!input->arr[i].IsString()) {
			throw std::runtime_error("embeddings input must be strings");
		}
		auto values = HashVector(input->arr[i].str, dim);
		Json embedding = Json::Array();
		for (auto value : values) {
			embedding.arr.push_back(Json::Number(value));
		}
		Json item = Json::Object();
		item.Set("object", Json::String("embedding"));
		item.Set("index", Json::Number(static_cast<double>(i)));
		item.Set("embedding", std::move(embedding));
		data.arr.push_back(std::move(item));
	}
	Json response = Json::Object();
	response.Set("object", Json::String("list"));
	auto model = request.Find("model");
	response.Set("model", model && model->IsString() ? Json::String(model->str) : Json::String("mock"));
	response.Set("data", std::move(data));
	return response;
}

FetchResult MockPost(Engine &engine, const string &kind, const string &body) {
	int status = 200;
	{
		std::lock_guard<std::mutex> lock(engine.mu);
		if (engine.fail_remaining > 0) {
			engine.fail_remaining--;
			status = engine.fail_status;
		}
	}
	FetchResult result;
	result.status = status;
	if (status == 200) {
		try {
			auto request = Json::Parse(body);
			if (kind == "chat") {
				result.body = MockChat(request).Dump();
			} else if (kind == "embed") {
				result.body = MockEmbeddings(request).Dump();
			} else {
				result.body = MockDecision(request).Dump();
			}
			result.ok = true;
		} catch (const std::exception &ex) {
			result.status = 500;
			result.ok = false;
			result.error = ex.what();
		}
	} else {
		result.ok = false;
		result.error = "HTTP status " + std::to_string(status);
	}
	return result;
}

FetchResult PostOne(ClientContext &context, const RuntimeSettings &settings, const ProviderConfig &provider,
                    const string &kind, const string &body) {
	auto &engine = Engine::Get();
	string url = Endpoint(provider, kind);
	if (settings.transport != "mock") {
		throw InternalException("live HTTP is posted by LivePostMany");
	}
	FetchResult last;
	int attempts = std::max(0, settings.max_retries) + 1;
	for (int attempt = 1; attempt <= attempts; attempt++) {
		if (context.IsInterrupted()) {
			throw InterruptException();
		}
		FetchResult result = MockPost(engine, kind, body);
		engine.NoteHttp(kind, url, result.status, body);
		last = result;
		if (result.ok || !Retryable(result) || attempt == attempts) {
			return result;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(RetryDelayMs(result, settings.retry_base_ms, attempt)));
	}
	return last;
}

} // namespace

string Md5Hex(const string &data) {
	MD5Context context;
	context.Add(data);
	return context.FinishHex();
}

RuntimeSettings LoadSettings(ClientContext &context) {
	RuntimeSettings settings;
	settings.transport = LowerCopy(SettingString(context, "rvbbit_transport", "live"));
	settings.max_concurrency = SettingInt(context, "rvbbit_max_concurrency", 8);
	settings.cache_path = SettingString(context, "rvbbit_cache_path", "");
	settings.base_url = SettingString(context, "rvbbit_base_url", "");
	settings.decision_model = SettingString(context, "rvbbit_decision_model", "");
	settings.chat_model = SettingString(context, "rvbbit_chat_model", "");
	settings.embed_model = SettingString(context, "rvbbit_embed_model", "");
	settings.embed_dim = SettingInt(context, "rvbbit_embed_dim", 0);
	settings.cache_size = SettingInt(context, "rvbbit_cache_size", 10000);
	settings.max_retries = SettingInt(context, "rvbbit_max_retries", 3);
	settings.retry_base_ms = SettingInt(context, "rvbbit_retry_base_ms", 200);
	settings.timeout_sec = SettingInt(context, "rvbbit_timeout_sec", 60);
	settings.tls_verify = SettingBool(context, "rvbbit_tls_verify", true);
	settings.reasoning = LowerCopy(SettingString(context, "rvbbit_reasoning", "disabled"));
	settings.ignore_env = SettingBool(context, "rvbbit_ignore_env", false);
	if (settings.transport != "live" && settings.transport != "mock") {
		throw InvalidInputException("rvbbit_transport must be 'live' or 'mock'");
	}
	if (settings.max_concurrency < 1) {
		settings.max_concurrency = 1;
	}
	if (settings.cache_size < 0) {
		settings.cache_size = 0;
	}
	if (settings.embed_dim < 0) {
		settings.embed_dim = 0;
	}
	if (settings.embed_dim > 4096) {
		settings.embed_dim = 4096;
	}
	if (settings.timeout_sec < 1) {
		settings.timeout_sec = 1;
	}
	if (settings.reasoning != "disabled" && settings.reasoning != "none" && settings.reasoning != "minimal" &&
	    settings.reasoning != "low" && settings.reasoning != "medium" && settings.reasoning != "high" &&
	    settings.reasoning != "enabled") {
		throw InvalidInputException(
		    "rvbbit_reasoning must be disabled, none, minimal, low, medium, high, or enabled");
	}
	return settings;
}

ProviderConfig ResolveProvider(ClientContext &context, const RuntimeSettings &settings) {
	ProviderConfig config;
	config.base_url = DEFAULT_BASE_URL;
	config.decision_model = DEFAULT_DECISION_MODEL;
	config.chat_model = DEFAULT_CHAT_MODEL;
	config.embed_model = DEFAULT_EMBED_MODEL;
	config.embed_dim = DEFAULT_EMBED_DIM;
	config.key_source = "none";
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	auto match = SecretManager::Get(context).LookupSecret(transaction, "openrouter", "openrouter");
	if (match.HasMatch()) {
		auto &secret = dynamic_cast<const KeyValueSecret &>(match.GetSecret());
		auto key = secret.TryGetValue(Identifier("key"), false);
		if (key.IsNull()) {
			key = secret.TryGetValue(Identifier("api_key"), false);
		}
		if (!key.IsNull()) {
			config.api_key = TrimCopy(key.ToString());
			config.key_source = "secret";
		}
		auto base = secret.TryGetValue(Identifier("base_url"), false);
		if (!base.IsNull() && !TrimCopy(base.ToString()).empty()) {
			config.base_url = TrimSlash(TrimCopy(base.ToString()));
		}
		auto decision = secret.TryGetValue(Identifier("decision_model"), false);
		if (!decision.IsNull() && !TrimCopy(decision.ToString()).empty()) {
			config.decision_model = TrimCopy(decision.ToString());
		}
		auto chat = secret.TryGetValue(Identifier("chat_model"), false);
		if (!chat.IsNull() && !TrimCopy(chat.ToString()).empty()) {
			config.chat_model = TrimCopy(chat.ToString());
			config.chat_model_from_user = true;
		}
		auto embed = secret.TryGetValue(Identifier("embed_model"), false);
		if (!embed.IsNull() && !TrimCopy(embed.ToString()).empty()) {
			config.embed_model = TrimCopy(embed.ToString());
		}
		auto embed_dim = secret.TryGetValue(Identifier("embed_dim"), false);
		if (!embed_dim.IsNull()) {
			config.embed_dim = embed_dim.GetValue<int32_t>();
		}
	}
	if (config.api_key.empty() && !settings.ignore_env) {
		auto env = std::getenv("OPENROUTER_API_KEY");
		if (env && *env) {
			config.api_key = TrimCopy(env);
			config.key_source = "env";
		}
	}
	if (!settings.base_url.empty()) {
		config.base_url = TrimSlash(settings.base_url);
	}
	if (!settings.decision_model.empty()) {
		config.decision_model = settings.decision_model;
	}
	if (!settings.chat_model.empty()) {
		config.chat_model = settings.chat_model;
		config.chat_model_from_user = true;
	}
	if (!settings.embed_model.empty()) {
		config.embed_model = settings.embed_model;
	}
	if (settings.embed_dim >= 2) {
		config.embed_dim = settings.embed_dim;
	}
	if (config.embed_dim < 2 || config.embed_dim > 4096) {
		config.embed_dim = DEFAULT_EMBED_DIM;
	}
	config.embed_model = CanonicalEmbedModel(config.embed_model);
	return config;
}

vector<FetchResult> FetchPost(ClientContext &context, const string &kind, vector<string> bodies) {
	auto settings = LoadSettings(context);
	auto provider = ResolveProvider(context, settings);
	auto &engine = Engine::Get();
	engine.SetLimit(settings.max_concurrency);
	{
		std::lock_guard<std::mutex> lock(engine.mu);
		engine.cache_capacity = settings.cache_size;
	}
	vector<string> unique;
	for (auto &body : bodies) {
		if (std::find(unique.begin(), unique.end(), body) == unique.end()) {
			unique.push_back(body);
		}
	}
	vector<FetchResult> unique_results(unique.size());
	if (!unique.empty() && settings.transport == "live" && provider.api_key.empty()) {
		throw InvalidInputException(
		    "OpenRouter API key is not configured. CREATE SECRET (TYPE openrouter, KEY '...') or set OPENROUTER_API_KEY.");
	}
	if (!unique.empty() && settings.transport != "mock") {
		LivePostOptions options;
		options.concurrency = settings.max_concurrency;
		options.timeout_sec = settings.timeout_sec;
		options.max_retries = settings.max_retries;
		options.retry_base_ms = settings.retry_base_ms;
		options.tls_verify = settings.tls_verify;
		unique_results = LivePostMany(context, kind, Endpoint(provider, kind), unique, provider.api_key, options);
		for (auto &result : unique_results) {
			RejectClientError(result);
		}
		vector<FetchResult> results;
		results.reserve(bodies.size());
		for (auto &body : bodies) {
			auto found = std::find(unique.begin(), unique.end(), body);
			results.push_back(unique_results[static_cast<idx_t>(found - unique.begin())]);
		}
		return results;
	}
	std::atomic<idx_t> next {0};
	std::mutex error_mu;
	string error;
	bool interrupted = false;
	int workers = std::min(settings.max_concurrency, static_cast<int32_t>(unique.size()));
	if (workers < 1) {
		workers = 1;
	}
	vector<std::thread> threads;
	for (int i = 0; i < workers; i++) {
		threads.emplace_back([&] {
			while (true) {
				if (context.IsInterrupted()) {
					std::lock_guard<std::mutex> lock(error_mu);
					interrupted = true;
					return;
				}
				idx_t job = next.fetch_add(1);
				if (job >= unique.size()) {
					return;
				}
				try {
					unique_results[job] = PostOne(context, settings, provider, kind, unique[job]);
				} catch (const InterruptException &) {
					std::lock_guard<std::mutex> lock(error_mu);
					interrupted = true;
					return;
				} catch (const std::exception &ex) {
					std::lock_guard<std::mutex> lock(error_mu);
					if (error.empty()) {
						error = ex.what();
					}
					return;
				}
			}
		});
	}
	for (auto &thread : threads) {
		thread.join();
	}
	if (interrupted || context.IsInterrupted()) {
		throw InterruptException();
	}
	if (!error.empty()) {
		throw InvalidInputException(error);
	}
	vector<FetchResult> results;
	results.reserve(bodies.size());
	for (auto &body : bodies) {
		auto found = std::find(unique.begin(), unique.end(), body);
		results.push_back(unique_results[static_cast<idx_t>(found - unique.begin())]);
	}
	for (auto &result : results) {
		RejectClientError(result);
	}
	return results;
}

bool CacheGet(ClientContext &context, const string &key, string &payload_out) {
	auto settings = LoadSettings(context);
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	engine.cache_capacity = settings.cache_size;
	if (engine.LookupL1(key, payload_out)) {
		return true;
	}
	if (settings.cache_path.empty()) {
		return false;
	}
	EnsureCache(engine, settings.cache_path);
	bool found = false;
	auto payload = QueryOne(*engine.cache_con, "SELECT payload FROM judgments WHERE cache_key = " + SqlQuote(key), found);
	if (!found) {
		return false;
	}
	engine.l2_hits++;
	engine.hits++;
	engine.misses--;
	engine.StoreL1(key, payload);
	payload_out = std::move(payload);
	return true;
}

void CachePut(ClientContext &context, const string &key, const string &payload) {
	auto settings = LoadSettings(context);
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	engine.cache_capacity = settings.cache_size;
	engine.StoreL1(key, payload);
	if (settings.cache_path.empty()) {
		return;
	}
	EnsureCache(engine, settings.cache_path);
	auto prepared = engine.cache_con->Prepare("INSERT OR REPLACE INTO judgments(cache_key, payload) VALUES (?, ?)");
	if (prepared->HasError()) {
		throw InvalidInputException("rvbbit persistent cache: " + prepared->GetError());
	}
	vector<Value> values {Value(key), Value(payload)};
	auto inserted = prepared->Execute(values);
	if (inserted->HasError()) {
		throw InvalidInputException("rvbbit persistent cache: " + inserted->GetError());
	}
}

void CacheClearMemory() {
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	engine.lru.clear();
}

void CachePurge(ClientContext &context) {
	auto settings = LoadSettings(context);
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	engine.lru.clear();
	engine.hits = 0;
	engine.misses = 0;
	engine.l2_hits = 0;
	if (settings.cache_path.empty()) {
		return;
	}
	EnsureCache(engine, settings.cache_path);
	ExecCache(*engine.cache_con, "DELETE FROM judgments");
}

void CacheStats(int64_t &hits, int64_t &misses, int64_t &l2_hits, int64_t &entries, int64_t &http_calls) {
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	hits = engine.hits;
	misses = engine.misses;
	l2_hits = engine.l2_hits;
	entries = static_cast<int64_t>(engine.lru.size());
	http_calls = engine.http_calls;
}

vector<HttpLogRow> HttpLogSnapshot() {
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	vector<HttpLogRow> rows;
	for (auto &entry : engine.log) {
		HttpLogRow row;
		row.seq = entry.seq;
		row.kind = entry.kind;
		row.url = entry.url;
		row.status = entry.status;
		row.body = entry.body;
		rows.push_back(std::move(row));
	}
	return rows;
}

void HttpLogReset() {
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	engine.log.clear();
	engine.http_calls = 0;
}

void MockReset() {
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	engine.fail_remaining = 0;
	engine.fail_status = 500;
	engine.noul_overrides.clear();
}

void MockFailNext(int32_t count, int32_t status) {
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	engine.fail_remaining = std::max(0, count);
	engine.fail_status = status;
}

void MockNoul(string state_substr, string instruction_substr, double score) {
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	engine.noul_overrides.push_back(NoulOverride {std::move(state_substr), std::move(instruction_substr), score});
}

bool SaveTree(ClientContext &context, const string &name, const string &tree_json) {
	auto settings = LoadSettings(context);
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	engine.trees[name] = tree_json;
	if (!settings.cache_path.empty()) {
		EnsureCache(engine, settings.cache_path);
		auto prepared = engine.cache_con->Prepare("INSERT OR REPLACE INTO trees(name, tree_json) VALUES (?, ?)");
		if (prepared->HasError()) {
			throw InvalidInputException(prepared->GetError());
		}
		vector<Value> values {Value(name), Value(tree_json)};
		auto inserted = prepared->Execute(values);
		if (inserted->HasError()) {
			throw InvalidInputException(inserted->GetError());
		}
	}
	return true;
}

bool LoadTree(ClientContext &context, const string &name, string &tree_json_out) {
	auto settings = LoadSettings(context);
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	auto found = engine.trees.find(name);
	if (found != engine.trees.end()) {
		tree_json_out = found->second;
		return true;
	}
	if (settings.cache_path.empty()) {
		return false;
	}
	EnsureCache(engine, settings.cache_path);
	bool hit = false;
	auto payload = QueryOne(*engine.cache_con, "SELECT tree_json FROM trees WHERE name = " + SqlQuote(name), hit);
	if (!hit) {
		return false;
	}
	engine.trees[name] = payload;
	tree_json_out = std::move(payload);
	return true;
}

bool DropTree(ClientContext &context, const string &name) {
	auto settings = LoadSettings(context);
	auto &engine = Engine::Get();
	std::lock_guard<std::mutex> lock(engine.mu);
	auto erased = engine.trees.erase(name) > 0;
	if (!settings.cache_path.empty()) {
		EnsureCache(engine, settings.cache_path);
		ExecCache(*engine.cache_con, "DELETE FROM trees WHERE name = " + SqlQuote(name));
		erased = true;
	}
	return erased;
}

bool GrammarRegistered() {
	return Engine::Get().grammar_registered;
}

void SetGrammarRegistered(bool value) {
	Engine::Get().grammar_registered = value;
}

void RecordHttp(const string &kind, const string &url, int status, const string &body) {
	Engine::Get().NoteHttp(kind, url, status, body);
}

void SetReasoning(Json &body, const string &mode) {
	auto kind = LowerCopy(mode);
	Json reasoning = Json::Object();
	if (kind.empty() || kind == "disabled") {
		reasoning.Set("enabled", Json::Bool(false));
	} else if (kind == "enabled") {
		reasoning.Set("enabled", Json::Bool(true));
	} else {
		reasoning.Set("effort", Json::String(kind));
	}
	body.Set("reasoning", std::move(reasoning));
}

string CanonicalEmbedModel(string model) {
	auto lower = LowerCopy(model);
	if (lower == "voyageai/voyage-4" || lower == "voyage-4") {
		return DEFAULT_EMBED_MODEL;
	}
	return model;
}

} // namespace rvbbit
} // namespace duckdb
