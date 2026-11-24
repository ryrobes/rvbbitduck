#include "engine.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

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

Json ParseJsonOrThrow(const string &text, const string &what) {
	try {
		return Json::Parse(text);
	} catch (const std::exception &ex) {
		throw InvalidInputException(what + ": " + ex.what());
	}
}

Json EncodeState(const string &raw) {
	if (Trim(raw).empty()) {
		throw InvalidInputException("state must not be empty");
	}
	if (Trim(raw).size() > 32768) {
		throw InvalidInputException("state exceeds 32768 bytes; split the evidence explicitly");
	}
	try {
		auto parsed = Json::Parse(Trim(raw));
		if (parsed.IsNull()) {
			throw InvalidInputException("state must not be empty");
		}
		if (parsed.Dump().size() > 32768) {
			throw InvalidInputException("state exceeds 32768 bytes; split the evidence explicitly");
		}
		return parsed;
	} catch (const InvalidInputException &) {
		throw;
	} catch (const std::exception &) {
		if (raw.size() > 32768) {
			throw InvalidInputException("state exceeds 32768 bytes; split the evidence explicitly");
		}
		return Json::String(raw);
	}
}

string WithBoundary(const string &instructions, const string &type) {
	if (type == "box") {
		return instructions;
	}
	string boundary = EVIDENCE_BOUNDARY;
	if (instructions.rfind(boundary, 0) == 0) {
		return instructions;
	}
	return boundary + instructions;
}

void PrefixQuestions(Json &questions) {
	if (!questions.IsObject() || questions.obj.empty()) {
		throw InvalidInputException("questions must be a non-empty object");
	}
	if (questions.obj.size() > 200) {
		throw InvalidInputException("at most 200 questions");
	}
	for (auto &entry : questions.obj) {
		if (entry.first.empty() || Trim(entry.first).empty() || entry.first.size() > 80) {
			throw InvalidInputException("question names must contain 1 to 80 characters");
		}
		auto type = entry.second.Find("type");
		auto instructions = entry.second.Find("instructions");
		if (!type || !type->IsString() || !instructions || !instructions->IsString()) {
			throw InvalidInputException("each question needs a type and instructions");
		}
		auto text = Trim(instructions->str);
		if (text.empty() || text.size() > 2000) {
			throw InvalidInputException("instructions must contain 1 to 2000 characters");
		}
		entry.second.Set("instructions", Json::String(WithBoundary(text, type->str)));
		if (type->str == "choice" || type->str == "score") {
			auto criteria = entry.second.Find("criteria");
			if (!criteria || !criteria->IsObject() || criteria->obj.size() < 2 || criteria->obj.size() > 16) {
				throw InvalidInputException(type->str + " questions need 2 to 16 criteria");
			}
		} else if (type->str != "noul" && type->str != "box") {
			throw InvalidInputException("unsupported question type " + type->str);
		}
	}
}

Json TruthQuestions(const string &predicate) {
	auto text = Trim(predicate);
	if (text.empty() || text.size() > 2000) {
		throw InvalidInputException("predicate must contain 1 to 2000 characters");
	}
	Json question = Json::Object();
	question.Set("type", Json::String("noul"));
	question.Set("instructions", Json::String(text));
	Json questions = Json::Object();
	questions.Set("result", std::move(question));
	return questions;
}

Json ChoiceQuestionsFromLabels(const vector<string> &labels) {
	if (labels.size() < 2 || labels.size() > 16) {
		throw InvalidInputException("provide 2 to 16 distinct non-empty category labels");
	}
	Json criteria = Json::Object();
	for (auto &label : labels) {
		auto text = Trim(label);
		if (text.empty() || text.size() > 80) {
			throw InvalidInputException("provide 2 to 16 distinct non-empty category labels");
		}
		if (criteria.Has(text)) {
			throw InvalidInputException("provide 2 to 16 distinct non-empty category labels");
		}
		criteria.Set(text, Json::String(text));
	}
	Json question = Json::Object();
	question.Set("type", Json::String("choice"));
	question.Set("instructions", Json::String("Choose the category that best describes the supplied state."));
	question.Set("criteria", std::move(criteria));
	Json questions = Json::Object();
	questions.Set("result", std::move(question));
	return questions;
}

Json ChoiceQuestionsFromObject(const Json &categories) {
	if (!categories.IsObject()) {
		throw InvalidInputException("categories must be an object mapping labels to descriptions");
	}
	if (categories.obj.size() < 2 || categories.obj.size() > 16) {
		throw InvalidInputException(
		    "provide 2 to 16 named categories, labels 1..80 characters and descriptions 1..2000 characters");
	}
	for (auto &entry : categories.obj) {
		if (Trim(entry.first).empty() || entry.first.size() > 80 || !entry.second.IsString() ||
		    Trim(entry.second.str).empty() || entry.second.str.size() > 2000) {
			throw InvalidInputException(
			    "provide 2 to 16 named categories, labels 1..80 characters and descriptions 1..2000 characters");
		}
	}
	Json question = Json::Object();
	question.Set("type", Json::String("choice"));
	question.Set("instructions", Json::String("Choose the category that best describes the supplied state."));
	question.Set("criteria", categories);
	Json questions = Json::Object();
	questions.Set("result", std::move(question));
	return questions;
}

string DecisionBody(Json state, Json questions, const string &model) {
	PrefixQuestions(questions);
	Json body = Json::Object();
	body.Set("model", Json::String(model));
	body.Set("state", std::move(state));
	body.Set("questions", std::move(questions));
	return body.Dump();
}

bool FiniteProbability(const Json *value) {
	return value && value->IsNumber() && std::isfinite(value->num) && value->num >= 0 && value->num <= 1;
}

string NormalizeDecision(const string &request_body, const string &response_body, string &error) {
	Json request;
	Json response;
	try {
		request = Json::Parse(request_body);
		response = Json::Parse(response_body);
	} catch (const std::exception &) {
		error = "invalid decision response";
		return "";
	}
	auto requested = request.Find("model");
	auto questions = request.Find("questions");
	auto answers = response.Find("answers");
	auto actual = response.Find("model");
	if (!requested || !requested->IsString() || !questions || !questions->IsObject() || !answers || !answers->IsObject() ||
	    !actual || !actual->IsString()) {
		error = "invalid decision response";
		return "";
	}
	if (!(actual->str == requested->str || actual->str.rfind(requested->str + "-", 0) == 0)) {
		error = "unexpected provider model";
		return "";
	}
	if (answers->obj.size() != questions->obj.size()) {
		error = "answer keys differ from questions";
		return "";
	}
	Json normalized_answers = Json::Object();
	for (auto &entry : questions->obj) {
		auto answer = answers->Find(entry.first);
		auto qtype = entry.second.Find("type");
		if (!answer || !answer->IsObject() || !qtype || !qtype->IsString()) {
			error = "answer keys differ from questions";
			return "";
		}
		auto atype = answer->Find("type");
		if (!atype || !atype->IsString() || atype->str != qtype->str) {
			error = "wrong answer type";
			return "";
		}
		auto confidence = answer->Find("confidence");
		if (confidence && !confidence->IsNull() && !FiniteProbability(confidence)) {
			error = "invalid confidence";
			return "";
		}
		Json kept = Json::Object();
		kept.Set("type", Json::String(qtype->str));
		if (qtype->str == "noul") {
			auto noul = answer->Find("noul");
			if (!FiniteProbability(noul)) {
				error = "invalid boolean score";
				return "";
			}
			kept.Set("noul", Json::Number(noul->num));
		} else if (qtype->str == "choice") {
			auto criteria = entry.second.Find("criteria");
			auto choice = answer->Find("choice");
			auto probs = answer->Find("probabilities");
			if (!criteria || !choice || !choice->IsString() || !criteria->Has(choice->str) || !probs || !probs->IsObject()) {
				error = "unknown choice";
				return "";
			}
			if (probs->obj.size() != criteria->obj.size()) {
				error = "missing choice probabilities";
				return "";
			}
			double sum = 0;
			for (auto &criterion : criteria->obj) {
				auto score = probs->Find(criterion.first);
				if (!FiniteProbability(score)) {
					error = "invalid choice probabilities";
					return "";
				}
				sum += score->num;
			}
			if (std::fabs(sum - 1.0) > 0.02) {
				error = "invalid choice probabilities";
				return "";
			}
			kept.Set("choice", Json::String(choice->str));
			kept.Set("probabilities", *probs);
			if (confidence && !confidence->IsNull()) {
				kept.Set("confidence", Json::Number(confidence->num));
			}
		} else if (qtype->str == "score") {
			auto criteria = entry.second.Find("criteria");
			auto score = answer->Find("score");
			if (!criteria || !score || !score->IsString() || !criteria->Has(score->str)) {
				error = "invalid score answer";
				return "";
			}
			kept.Set("score", Json::String(score->str));
		} else if (qtype->str == "box") {
			auto box = answer->Find("box");
			if (!box || !box->IsObject()) {
				error = "invalid box";
				return "";
			}
			double edges[4];
			const char *names[] = {"top", "bottom", "left", "right"};
			for (int i = 0; i < 4; i++) {
				auto edge = box->Find(names[i]);
				if (!FiniteProbability(edge)) {
					error = "invalid box edge";
					return "";
				}
				edges[i] = edge->num;
			}
			if (edges[0] > edges[1] || edges[2] > edges[3]) {
				error = "invalid box";
				return "";
			}
			Json kept_box = Json::Object();
			for (int i = 0; i < 4; i++) {
				kept_box.Set(names[i], Json::Number(edges[i]));
			}
			kept.Set("box", std::move(kept_box));
			if (confidence && !confidence->IsNull()) {
				kept.Set("confidence", Json::Number(confidence->num));
			}
		} else {
			error = "unsupported question type";
			return "";
		}
		normalized_answers.Set(entry.first, std::move(kept));
	}
	Json usage = Json::Object();
	if (auto raw_usage = response.Find("usage"); raw_usage && raw_usage->IsObject()) {
		for (auto &key : {"cost", "input_tokens", "output_tokens", "total_tokens"}) {
			if (auto value = raw_usage->Find(key); value && value->IsNumber() && std::isfinite(value->num) && value->num >= 0) {
				usage.Set(key, Json::Number(value->num));
			}
		}
	}
	Json normalized = Json::Object();
	normalized.Set("model", Json::String(actual->str));
	normalized.Set("requested_model", Json::String(requested->str));
	normalized.Set("contract", Json::String(DECISION_CONTRACT));
	normalized.Set("answers", std::move(normalized_answers));
	normalized.Set("usage", std::move(usage));
	error.clear();
	return normalized.Dump();
}

string DecisionCacheKey(const string &body) {
	return Md5Hex(string("decision\n") + body);
}

//! Several questions about one state become one /api/alpha/decisions call.
//! OpenRouter accepts many named questions. A rejected fused call is retried
//! as the original single-question requests.
unordered_map<string, string> ResolveDecisions(ClientContext &context, const vector<string> &bodies) {
	unordered_map<string, string> resolved;
	vector<string> missing;
	for (auto &body : bodies) {
		if (resolved.find(body) != resolved.end() ||
		    std::find(missing.begin(), missing.end(), body) != missing.end()) {
			continue;
		}
		string cached;
		if (CacheGet(context, DecisionCacheKey(body), cached)) {
			resolved[body] = std::move(cached);
		} else {
			missing.push_back(body);
		}
	}
	if (missing.empty()) {
		return resolved;
	}

	struct Piece {
		string body;
		string question_name;
		string fused_name;
		Json question;
	};
	struct Group {
		string model;
		Json state;
		vector<Piece> pieces;
	};
	vector<Group> groups;
	vector<string> unparsed;
	for (auto &body : missing) {
		Json request;
		try {
			request = Json::Parse(body);
		} catch (const std::exception &) {
			unparsed.push_back(body);
			continue;
		}
		auto model = request.Find("model");
		auto state = request.Find("state");
		auto questions = request.Find("questions");
		if (!model || !model->IsString() || !state || !questions || !questions->IsObject() || questions->obj.empty()) {
			unparsed.push_back(body);
			continue;
		}
		string state_key = state->Dump();
		Group *group = nullptr;
		for (auto &candidate : groups) {
			if (candidate.model == model->str && candidate.state.Dump() == state_key && candidate.pieces.size() < 8) {
				group = &candidate;
				break;
			}
		}
		if (!group) {
			groups.push_back(Group {});
			group = &groups.back();
			group->model = model->str;
			group->state = *state;
		}
		for (auto &entry : questions->obj) {
			Piece piece;
			piece.body = body;
			piece.question_name = entry.first;
			piece.question = entry.second;
			piece.fused_name = "q" + std::to_string(group->pieces.size());
			group->pieces.push_back(std::move(piece));
		}
	}

	auto store = [&](const string &body, const string &normalized) {
		CachePut(context, DecisionCacheKey(body), normalized);
		resolved[body] = normalized;
	};
	auto individually = [&](const vector<string> &alone) {
		if (alone.empty()) {
			return;
		}
		auto fetched = FetchPost(context, "decision", alone);
		for (idx_t i = 0; i < alone.size(); i++) {
			if (!fetched[i].ok) {
				continue;
			}
			string error;
			auto normalized = NormalizeDecision(alone[i], fetched[i].body, error);
			if (!normalized.empty()) {
				store(alone[i], normalized);
			}
		}
	};
	individually(unparsed);

	for (auto &group : groups) {
		vector<string> originals;
		for (auto &piece : group.pieces) {
			if (std::find(originals.begin(), originals.end(), piece.body) == originals.end()) {
				originals.push_back(piece.body);
			}
		}
		if (group.pieces.size() < 2) {
			individually(originals);
			continue;
		}
		Json questions = Json::Object();
		for (auto &piece : group.pieces) {
			questions.Set(piece.fused_name, piece.question);
		}
		Json fused = Json::Object();
		fused.Set("model", Json::String(group.model));
		fused.Set("state", group.state);
		fused.Set("questions", std::move(questions));
		auto fused_body = fused.Dump();
		auto fetched = FetchPost(context, "decision", {fused_body});
		string error;
		string normalized = fetched[0].ok ? NormalizeDecision(fused_body, fetched[0].body, error) : "";
		Json parsed;
		bool fused_ok = false;
		if (!normalized.empty()) {
			try {
				parsed = Json::Parse(normalized);
				fused_ok = parsed.Find("answers") != nullptr;
			} catch (const std::exception &) {
				fused_ok = false;
			}
		}
		if (!fused_ok) {
			individually(originals);
			continue;
		}
		auto answers = parsed.Find("answers");
		for (auto &body : originals) {
			Json kept = Json::Object();
			for (auto &piece : group.pieces) {
				if (piece.body != body) {
					continue;
				}
				auto answer = answers->Find(piece.fused_name);
				if (!answer) {
					kept = Json::Object();
					break;
				}
				kept.Set(piece.question_name, *answer);
			}
			if (kept.obj.empty()) {
				continue;
			}
			Json one = Json::Object();
			if (auto model = parsed.Find("model")) {
				one.Set("model", *model);
			}
			if (auto requested = parsed.Find("requested_model")) {
				one.Set("requested_model", *requested);
			}
			one.Set("contract", Json::String(DECISION_CONTRACT));
			one.Set("answers", std::move(kept));
			if (auto usage = parsed.Find("usage")) {
				one.Set("usage", *usage);
			}
			store(body, one.Dump());
		}
	}
	return resolved;
}

bool NoulScore(const string &normalized, double &score) {
	auto parsed = Json::Parse(normalized);
	auto answers = parsed.Find("answers");
	auto answer = answers ? answers->Find("result") : nullptr;
	auto noul = answer ? answer->Find("noul") : nullptr;
	if (!noul || !noul->IsNumber()) {
		return false;
	}
	score = noul->num;
	return true;
}

bool ChoiceLabel(const string &normalized, string &label) {
	auto parsed = Json::Parse(normalized);
	auto answers = parsed.Find("answers");
	auto answer = answers ? answers->Find("result") : nullptr;
	auto choice = answer ? answer->Find("choice") : nullptr;
	if (!choice || !choice->IsString()) {
		return false;
	}
	label = choice->str;
	return true;
}

LogicalType TreeResultType() {
	child_list_t<LogicalType> children;
	children.emplace_back(Identifier("version"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("tree_id"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("status"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("label"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("steps"), LogicalType::INTEGER);
	children.emplace_back(Identifier("path"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("error"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("stopped_at"), LogicalType::VARCHAR);
	children.emplace_back(Identifier("missing_inputs"), LogicalType::VARCHAR);
	return LogicalType::STRUCT(std::move(children));
}

Value TreeValue(const string &tree_id, const string &status, const string &label, bool has_label, int32_t steps,
                const string &path, const string &error, const string &stopped_at, const string &missing) {
	vector<Value> values;
	values.push_back(Value(TREE_VERSION));
	values.push_back(Value(tree_id));
	values.push_back(Value(status));
	values.push_back(has_label ? Value(label) : Value(LogicalType::VARCHAR));
	values.push_back(Value::INTEGER(steps));
	values.push_back(Value(path));
	values.push_back(error.empty() ? Value(LogicalType::VARCHAR) : Value(error));
	values.push_back(stopped_at.empty() ? Value(LogicalType::VARCHAR) : Value(stopped_at));
	values.push_back(missing.empty() ? Value(LogicalType::VARCHAR) : Value(missing));
	return Value::STRUCT(TreeResultType(), std::move(values));
}

int ValidateNode(const Json &node, int depth);

void ValidateInputs(const Json &node) {
	auto inputs = node.Find("inputs");
	if (!inputs) {
		return;
	}
	if (!inputs->IsArray() || inputs->arr.empty() || inputs->arr.size() > 16) {
		throw InvalidInputException("decision tree: inputs must contain 1..16 distinct field names of 1..80 characters");
	}
	std::set<string> seen;
	for (auto &value : inputs->arr) {
		if (!value.IsString() || Trim(value.str).empty() || value.str.size() > 80 || !seen.insert(value.str).second) {
			throw InvalidInputException(
			    "decision tree: inputs must contain 1..16 distinct field names of 1..80 characters");
		}
	}
}

void RejectExtra(const Json &node, const std::set<string> &allowed) {
	for (auto &entry : node.obj) {
		if (!allowed.count(entry.first)) {
			throw InvalidInputException("decision tree: unsupported node field");
		}
	}
}

int ValidateNode(const Json &node, int depth) {
	if (!node.IsObject() || depth > 16) {
		throw InvalidInputException("decision tree: expected a node object, maximum depth is 16");
	}
	auto kind = node.Find("kind");
	if (!kind || !kind->IsString()) {
		throw InvalidInputException("decision tree: unknown node kind");
	}
	int total = 1;
	if (kind->str == "leaf") {
		RejectExtra(node, {"kind", "label"});
		auto label = node.Find("label");
		if (!label || !label->IsString() || Trim(label->str).empty() || label->str.size() > 200) {
			throw InvalidInputException("decision tree: leaf label must contain 1..200 characters");
		}
	} else if (kind->str == "branch") {
		RejectExtra(node, {"kind", "question", "yes", "no", "threshold", "inputs"});
		auto threshold = node.Find("threshold");
		if (!threshold || !threshold->IsNumber() || !std::isfinite(threshold->num) || threshold->num < 0 ||
		    threshold->num > 1) {
			throw InvalidInputException("decision tree: threshold must be between 0 and 1");
		}
		auto yes = node.Find("yes");
		auto no = node.Find("no");
		if (!yes || !no) {
			throw InvalidInputException("decision tree: expected a node object, maximum depth is 16");
		}
		total += ValidateNode(*yes, depth + 1);
		total += ValidateNode(*no, depth + 1);
		ValidateInputs(node);
	} else if (kind->str == "choice") {
		RejectExtra(node, {"kind", "question", "choices", "routes", "inputs"});
		auto choices = node.Find("choices");
		auto routes = node.Find("routes");
		if (!choices || !routes || !routes->IsObject()) {
			throw InvalidInputException("decision tree: choice needs definitions and routes");
		}
		ChoiceQuestionsFromObject(*choices);
		if (choices->obj.size() != routes->obj.size()) {
			throw InvalidInputException("decision tree: every choice must have exactly one matching route");
		}
		for (auto &choice : choices->obj) {
			if (!routes->Has(choice.first)) {
				throw InvalidInputException("decision tree: every choice must have exactly one matching route");
			}
		}
		for (auto &route : routes->obj) {
			total += ValidateNode(route.second, depth + 1);
			if (total > 255) {
				throw InvalidInputException("decision tree: maximum 255 nodes");
			}
		}
		ValidateInputs(node);
	} else {
		throw InvalidInputException("decision tree: unknown node kind");
	}
	if (kind->str != "leaf") {
		auto question = node.Find("question");
		if (!question || !question->IsString() || Trim(question->str).empty() || question->str.size() > 1800) {
			throw InvalidInputException("decision tree: question must contain 1..1800 characters");
		}
	}
	if (total > 255) {
		throw InvalidInputException("decision tree: maximum 255 nodes");
	}
	return total;
}

Json CheckedTree(const string &text) {
	if (text.size() > 262144) {
		throw InvalidInputException("decision tree: maximum 256 KiB tree");
	}
	auto tree = ParseJsonOrThrow(text, "decision tree");
	ValidateNode(tree, 0);
	if (tree.Dump().size() > 262144) {
		throw InvalidInputException("decision tree: maximum 256 KiB tree");
	}
	return tree;
}

struct Cursor {
	bool active = false;
	Json evidence;
	Json node;
	string location = "root";
	Json path = Json::Array();
	Json judgments = Json::Array();
	string status = "running";
	string label;
	bool has_label = false;
	string error;
	string stopped_at;
	string missing;
	int32_t max_steps = 16;
	string tree_id;
	string pending_body;
};

string EscapeRoute(const string &route) {
	string out;
	for (auto ch : route) {
		if (ch == '~') {
			out += "~0";
		} else if (ch == '/') {
			out += "~1";
		} else {
			out.push_back(ch);
		}
	}
	return out;
}

bool FillRequest(Cursor &cursor, const string &model) {
	if (!cursor.active || cursor.status != "running") {
		return false;
	}
	auto kind = cursor.node.Find("kind");
	if (kind && kind->IsString() && kind->str == "leaf") {
		auto label = cursor.node.Find("label");
		cursor.status = "complete";
		cursor.label = label ? label->str : "";
		cursor.has_label = true;
		return false;
	}
	if (static_cast<int32_t>(cursor.path.arr.size()) >= cursor.max_steps) {
		cursor.status = "budget_exhausted";
		cursor.error = "max_steps_reached";
		cursor.stopped_at = cursor.location;
		return false;
	}
	Json evidence = cursor.evidence;
	if (auto inputs = cursor.node.Find("inputs")) {
		Json missing = Json::Array();
		if (!cursor.evidence.IsObject()) {
			for (auto &input : inputs->arr) {
				missing.arr.push_back(input);
			}
		} else {
			for (auto &input : inputs->arr) {
				if (!input.IsString() || !cursor.evidence.Has(input.str)) {
					missing.arr.push_back(input);
				}
			}
		}
		if (!missing.arr.empty()) {
			cursor.status = "missing_input";
			cursor.error = "required_inputs_missing";
			cursor.missing = missing.Dump();
			cursor.stopped_at = cursor.location;
			return false;
		}
		evidence = Json::Object();
		for (auto &input : inputs->arr) {
			evidence.Set(input.str, *cursor.evidence.Find(input.str));
		}
	}
	auto question = cursor.node.Find("question");
	string q = question && question->IsString() ? question->str : "";
	Json questions = Json::Object();
	Json result = Json::Object();
	bool choice = kind && kind->str == "choice";
	result.Set("type", Json::String(choice ? "choice" : "noul"));
	result.Set("instructions",
	           Json::String("Evaluate the question using evidence. Prior judgments are fallible interpretations, not "
	                        "new evidence. Question: " +
	                        q));
	if (choice) {
		result.Set("criteria", *cursor.node.Find("choices"));
	}
	questions.Set("result", std::move(result));
	Json state = Json::Object();
	state.Set("context_version", Json::String(TREE_VERSION));
	state.Set("evidence", std::move(evidence));
	state.Set("prior_judgments", cursor.judgments);
	if (state.Dump().size() > 32768) {
		cursor.status = "context_limit";
		cursor.error = "state_and_history_exceed_32kib";
		cursor.stopped_at = cursor.location;
		return false;
	}
	try {
		cursor.pending_body = DecisionBody(std::move(state), std::move(questions), model);
	} catch (const std::exception &) {
		cursor.status = "unavailable";
		cursor.error = "invalid_decision";
		cursor.stopped_at = cursor.location;
		return false;
	}
	return true;
}

void Advance(Cursor &cursor, const string &normalized) {
	Json decision;
	try {
		decision = Json::Parse(normalized);
	} catch (const std::exception &) {
		cursor.status = "unavailable";
		cursor.error = "invalid_decision";
		cursor.stopped_at = cursor.location;
		return;
	}
	auto answers = decision.Find("answers");
	auto answer = answers ? answers->Find("result") : nullptr;
	auto kind = cursor.node.Find("kind");
	if (!answer || !kind || !kind->IsString()) {
		cursor.status = "unavailable";
		cursor.error = "invalid_decision";
		cursor.stopped_at = cursor.location;
		return;
	}
	double score = 0;
	Json answer_value;
	string route;
	const Json *next = nullptr;
	if (kind->str == "branch") {
		auto noul = answer->Find("noul");
		auto atype = answer->Find("type");
		auto threshold = cursor.node.Find("threshold");
		if (!atype || atype->str != "noul" || !noul || !noul->IsNumber() || !threshold) {
			cursor.status = "unavailable";
			cursor.error = "invalid_decision";
			cursor.stopped_at = cursor.location;
			return;
		}
		score = noul->num;
		bool yes = score > threshold->num;
		answer_value = Json::Bool(yes);
		route = yes ? "yes" : "no";
		next = cursor.node.Find(route);
	} else {
		auto atype = answer->Find("type");
		auto choice = answer->Find("choice");
		auto probs = answer->Find("probabilities");
		if (!atype || atype->str != "choice" || !choice || !choice->IsString() || !cursor.node.Find("routes") ||
		    !cursor.node.Find("routes")->Has(choice->str) || !probs) {
			cursor.status = "unavailable";
			cursor.error = "invalid_decision";
			cursor.stopped_at = cursor.location;
			return;
		}
		auto prob = probs->Find(choice->str);
		if (!prob || !prob->IsNumber()) {
			cursor.status = "unavailable";
			cursor.error = "invalid_decision";
			cursor.stopped_at = cursor.location;
			return;
		}
		score = prob->num;
		route = choice->str;
		answer_value = Json::String(route);
		next = cursor.node.Find("routes")->Find(route);
	}
	if (!std::isfinite(score) || score < 0 || score > 1 || !next) {
		cursor.status = "unavailable";
		cursor.error = "invalid_decision";
		cursor.stopped_at = cursor.location;
		return;
	}
	auto question = cursor.node.Find("question");
	Json judgment = Json::Object();
	judgment.Set("question", Json::String(question ? question->str : ""));
	judgment.Set("answer", answer_value);
	judgment.Set("score", Json::Number(score));
	if (kind->str == "branch") {
		judgment.Set("threshold", *cursor.node.Find("threshold"));
	} else {
		judgment.Set("choices", *cursor.node.Find("choices"));
	}
	Json trace = judgment;
	trace.Set("node", Json::String(cursor.location));
	trace.Set("route", Json::String(route));
	if (auto model = decision.Find("model")) {
		trace.Set("model", *model);
	}
	if (auto contract = decision.Find("contract")) {
		trace.Set("contract", *contract);
	}
	if (cursor.node.Has("inputs")) {
		trace.Set("inputs", *cursor.node.Find("inputs"));
	}
	cursor.path.arr.push_back(std::move(trace));
	cursor.judgments.arr.push_back(std::move(judgment));
	cursor.location += "/" + EscapeRoute(route);
	cursor.node = *next;
}

void RunTrees(ClientContext &context, vector<Cursor> &cursors) {
	auto settings = LoadSettings(context);
	auto provider = ResolveProvider(context, settings);
	for (int wave = 0; wave < 16; wave++) {
		vector<string> bodies;
		vector<idx_t> waiting;
		for (idx_t i = 0; i < cursors.size(); i++) {
			if (FillRequest(cursors[i], provider.decision_model)) {
				bodies.push_back(cursors[i].pending_body);
				waiting.push_back(i);
			}
		}
		if (bodies.empty()) {
			return;
		}
		auto resolved = ResolveDecisions(context, bodies);
		for (idx_t i = 0; i < waiting.size(); i++) {
			auto &cursor = cursors[waiting[i]];
			auto found = resolved.find(cursor.pending_body);
			if (found == resolved.end()) {
				cursor.status = "unavailable";
				cursor.error = "decision_unavailable";
				cursor.stopped_at = cursor.location;
				continue;
			}
			Advance(cursor, found->second);
		}
	}
	for (auto &cursor : cursors) {
		if (cursor.active && cursor.status == "running") {
			cursor.status = "budget_exhausted";
			cursor.error = "max_steps_reached";
			cursor.stopped_at = cursor.location;
		}
	}
}

Value CursorValue(const Cursor &cursor) {
	return TreeValue(cursor.tree_id, cursor.status, cursor.label, cursor.has_label,
	                 static_cast<int32_t>(cursor.path.arr.size()), cursor.path.Dump(), cursor.error, cursor.stopped_at,
	                 cursor.missing);
}

void TruthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto settings = LoadSettings(context);
	auto provider = ResolveProvider(context, settings);
	bool scored = result.GetType().id() == LogicalTypeId::DOUBLE;
	vector<string> bodies(args.size());
	vector<char> live(args.size(), 0);
	vector<double> thresholds(args.size(), 0.5);
	for (idx_t row = 0; row < args.size(); row++) {
		string raw_state;
		string predicate;
		if (!ArgString(args.data[0], row, raw_state) || !ArgString(args.data[1], row, predicate)) {
			SetNull(result, row);
			continue;
		}
		if (args.ColumnCount() > 2 && !args.data[2].GetValue(row).IsNull()) {
			thresholds[row] = args.data[2].GetValue(row).GetValue<double>();
			if (!std::isfinite(thresholds[row]) || thresholds[row] < 0 || thresholds[row] > 1) {
				throw InvalidInputException("threshold must be a finite number between 0 and 1");
			}
		}
		try {
			bodies[row] = DecisionBody(EncodeState(raw_state), TruthQuestions(predicate), provider.decision_model);
			live[row] = 1;
		} catch (const InvalidInputException &) {
			throw;
		}
	}
	vector<string> needed;
	for (idx_t row = 0; row < args.size(); row++) {
		if (live[row]) {
			needed.push_back(bodies[row]);
		}
	}
	auto resolved = needed.empty() ? unordered_map<string, string> {} : ResolveDecisions(context, needed);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!live[row]) {
			continue;
		}
		auto found = resolved.find(bodies[row]);
		double score = 0;
		if (found == resolved.end() || !NoulScore(found->second, score)) {
			SetNull(result, row);
			continue;
		}
		if (scored) {
			result.SetValue(row, Value::DOUBLE(score));
		} else {
			result.SetValue(row, Value::BOOLEAN(score > thresholds[row]));
		}
	}
}

void ChooseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto provider = ResolveProvider(context, LoadSettings(context));
	bool categories_are_list = args.data[1].GetType().id() == LogicalTypeId::LIST;
	vector<string> bodies(args.size());
	vector<char> live(args.size(), 0);
	for (idx_t row = 0; row < args.size(); row++) {
		string raw_state;
		if (!ArgString(args.data[0], row, raw_state) || args.data[1].GetValue(row).IsNull()) {
			SetNull(result, row);
			continue;
		}
		try {
			Json questions;
			if (categories_are_list) {
				auto list_value = args.data[1].GetValue(row);
				vector<string> labels;
				for (auto &child : ListValue::GetChildren(list_value)) {
					if (child.IsNull()) {
						throw InvalidInputException("provide 2 to 16 distinct non-empty category labels");
					}
					labels.push_back(StringValue::Get(child));
				}
				questions = ChoiceQuestionsFromLabels(labels);
			} else {
				string text;
				if (!ArgString(args.data[1], row, text)) {
					SetNull(result, row);
					continue;
				}
				questions = ChoiceQuestionsFromObject(ParseJsonOrThrow(text, "categories"));
			}
			bodies[row] = DecisionBody(EncodeState(raw_state), std::move(questions), provider.decision_model);
			live[row] = 1;
		} catch (const InvalidInputException &) {
			throw;
		}
	}
	vector<string> needed;
	for (idx_t row = 0; row < args.size(); row++) {
		if (live[row]) {
			needed.push_back(bodies[row]);
		}
	}
	auto resolved = needed.empty() ? unordered_map<string, string> {} : ResolveDecisions(context, needed);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!live[row]) {
			continue;
		}
		auto found = resolved.find(bodies[row]);
		string label;
		if (found == resolved.end() || !ChoiceLabel(found->second, label)) {
			SetNull(result, row);
		} else {
			result.SetValue(row, Value(label));
		}
	}
}

void DecideFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto provider = ResolveProvider(context, LoadSettings(context));
	vector<string> bodies(args.size());
	vector<char> live(args.size(), 0);
	for (idx_t row = 0; row < args.size(); row++) {
		string raw_state;
		string questions_text;
		if (!ArgString(args.data[0], row, raw_state) || !ArgString(args.data[1], row, questions_text)) {
			SetNull(result, row);
			continue;
		}
		string model = provider.decision_model;
		if (args.ColumnCount() > 2) {
			string override_model;
			if (ArgString(args.data[2], row, override_model) && !Trim(override_model).empty()) {
				model = Trim(override_model);
			}
		}
		try {
			bodies[row] = DecisionBody(EncodeState(raw_state), ParseJsonOrThrow(questions_text, "questions"), model);
			live[row] = 1;
		} catch (const InvalidInputException &) {
			throw;
		}
	}
	vector<string> needed;
	for (idx_t row = 0; row < args.size(); row++) {
		if (live[row]) {
			needed.push_back(bodies[row]);
		}
	}
	auto resolved = needed.empty() ? unordered_map<string, string> {} : ResolveDecisions(context, needed);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!live[row]) {
			continue;
		}
		auto found = resolved.find(bodies[row]);
		if (found == resolved.end()) {
			SetNull(result, row);
		} else {
			result.SetValue(row, Value(found->second));
		}
	}
}

void TruthQuestionFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		string predicate;
		if (!ArgString(args.data[0], row, predicate)) {
			SetNull(result, row);
			continue;
		}
		result.SetValue(row, Value(TruthQuestions(predicate).Dump()));
	}
}

void ChoiceQuestionFunction(DataChunk &args, ExpressionState &, Vector &result) {
	bool as_list = args.data[0].GetType().id() == LogicalTypeId::LIST;
	for (idx_t row = 0; row < args.size(); row++) {
		if (args.data[0].GetValue(row).IsNull()) {
			SetNull(result, row);
			continue;
		}
		if (as_list) {
			auto list_value = args.data[0].GetValue(row);
			vector<string> labels;
			for (auto &child : ListValue::GetChildren(list_value)) {
				if (child.IsNull()) {
					throw InvalidInputException("provide 2 to 16 distinct non-empty category labels");
				}
				labels.push_back(StringValue::Get(child));
			}
			result.SetValue(row, Value(ChoiceQuestionsFromLabels(labels).Dump()));
		} else {
			string text = StringValue::Get(args.data[0].GetValue(row));
			result.SetValue(row, Value(ChoiceQuestionsFromObject(ParseJsonOrThrow(text, "categories")).Dump()));
		}
	}
}

Json LeafNode(const string &label) {
	Json node = Json::Object();
	node.Set("kind", Json::String("leaf"));
	node.Set("label", Json::String(label));
	ValidateNode(node, 0);
	return node;
}

void LeafFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		string label;
		if (!ArgString(args.data[0], row, label)) {
			SetNull(result, row);
			continue;
		}
		result.SetValue(row, Value(LeafNode(label).Dump()));
	}
}

vector<string> OptionalInputs(const Vector &vec, idx_t row) {
	auto value = vec.GetValue(row);
	vector<string> inputs;
	if (value.IsNull()) {
		return inputs;
	}
	for (auto &child : ListValue::GetChildren(value)) {
		if (child.IsNull()) {
			throw InvalidInputException("decision tree: inputs must contain 1..16 distinct field names of 1..80 characters");
		}
		inputs.push_back(StringValue::Get(child));
	}
	return inputs;
}

void BranchFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		string question;
		string yes;
		string no;
		if (!ArgString(args.data[0], row, question) || !ArgString(args.data[1], row, yes) ||
		    !ArgString(args.data[2], row, no)) {
			SetNull(result, row);
			continue;
		}
		double threshold = 0.5;
		if (args.ColumnCount() > 3 && !args.data[3].GetValue(row).IsNull()) {
			threshold = args.data[3].GetValue(row).GetValue<double>();
		}
		Json node = Json::Object();
		node.Set("kind", Json::String("branch"));
		node.Set("question", Json::String(Trim(question)));
		node.Set("yes", ParseJsonOrThrow(yes, "decision tree"));
		node.Set("no", ParseJsonOrThrow(no, "decision tree"));
		node.Set("threshold", Json::Number(threshold));
		if (args.ColumnCount() > 4) {
			auto inputs = OptionalInputs(args.data[4], row);
			if (!inputs.empty()) {
				Json list = Json::Array();
				for (auto &input : inputs) {
					list.arr.push_back(Json::String(input));
				}
				node.Set("inputs", std::move(list));
			}
		}
		ValidateNode(node, 0);
		result.SetValue(row, Value(node.Dump()));
	}
}

void ChoiceBranchFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		string question;
		string choices;
		string routes;
		if (!ArgString(args.data[0], row, question) || !ArgString(args.data[1], row, choices) ||
		    !ArgString(args.data[2], row, routes)) {
			SetNull(result, row);
			continue;
		}
		Json node = Json::Object();
		node.Set("kind", Json::String("choice"));
		node.Set("question", Json::String(Trim(question)));
		node.Set("choices", ParseJsonOrThrow(choices, "decision tree"));
		node.Set("routes", ParseJsonOrThrow(routes, "decision tree"));
		if (args.ColumnCount() > 3) {
			auto inputs = OptionalInputs(args.data[3], row);
			if (!inputs.empty()) {
				Json list = Json::Array();
				for (auto &input : inputs) {
					list.arr.push_back(Json::String(input));
				}
				node.Set("inputs", std::move(list));
			}
		}
		ValidateNode(node, 0);
		result.SetValue(row, Value(node.Dump()));
	}
}

int32_t MaxStepsOf(DataChunk &args, idx_t row, idx_t column) {
	if (args.ColumnCount() <= column || args.data[column].GetValue(row).IsNull()) {
		return 16;
	}
	auto value = args.data[column].GetValue(row).GetValue<int32_t>();
	if (value < 1 || value > 16) {
		throw InvalidInputException("decision tree: max_steps must be 1..16");
	}
	return value;
}

void DecisionTreeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	vector<Cursor> cursors(args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		string raw_state;
		string tree_text;
		if (!ArgString(args.data[0], row, raw_state) || !ArgString(args.data[1], row, tree_text)) {
			continue;
		}
		cursors[row].active = true;
		cursors[row].evidence = EncodeState(raw_state);
		cursors[row].node = CheckedTree(tree_text);
		cursors[row].tree_id = Md5Hex(cursors[row].node.Dump());
		cursors[row].max_steps = MaxStepsOf(args, row, 2);
	}
	RunTrees(context, cursors);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!cursors[row].active) {
			SetNull(result, row);
		} else {
			result.SetValue(row, CursorValue(cursors[row]));
		}
	}
}

void DecisionTreeBatchFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	for (idx_t row = 0; row < args.size(); row++) {
		if (args.data[0].GetValue(row).IsNull() || args.data[1].GetValue(row).IsNull()) {
			SetNull(result, row);
			continue;
		}
		auto states = args.data[0].GetValue(row);
		string tree_text = StringValue::Get(args.data[1].GetValue(row));
		auto tree = CheckedTree(tree_text);
		auto tree_id = Md5Hex(tree.Dump());
		int32_t max_steps = MaxStepsOf(args, row, 2);
		vector<Cursor> cursors;
		for (auto &child : ListValue::GetChildren(states)) {
			Cursor cursor;
			cursor.max_steps = max_steps;
			cursor.tree_id = tree_id;
			cursor.node = tree;
			if (!child.IsNull()) {
				cursor.active = true;
				cursor.evidence = EncodeState(StringValue::Get(child));
			}
			cursors.push_back(std::move(cursor));
		}
		if (cursors.size() > 256) {
			throw InvalidInputException("decision tree batch: maximum 256 states / 8 MiB");
		}
		RunTrees(context, cursors);
		vector<Value> values;
		for (auto &cursor : cursors) {
			if (!cursor.active) {
				values.emplace_back(TreeResultType());
			} else {
				values.push_back(CursorValue(cursor));
			}
		}
		result.SetValue(row, Value::LIST(TreeResultType(), std::move(values)));
	}
}

Json SemanticTree(const vector<string> &predicates, const vector<string> &labels, const vector<double> &thresholds,
                  const string &otherwise) {
	if (predicates.empty() || predicates.size() != labels.size()) {
		throw InvalidInputException("semantic_case needs the same number of predicates and labels");
	}
	Json node = LeafNode(otherwise.empty() ? "unmatched" : otherwise);
	for (idx_t i = predicates.size(); i-- > 0;) {
		double threshold = i < thresholds.size() ? thresholds[i] : 0.5;
		Json branch = Json::Object();
		branch.Set("kind", Json::String("branch"));
		branch.Set("question", Json::String(Trim(predicates[i])));
		branch.Set("yes", LeafNode(labels[i]));
		branch.Set("no", std::move(node));
		branch.Set("threshold", Json::Number(threshold));
		ValidateNode(branch, 0);
		node = std::move(branch);
	}
	return node;
}

void SemanticCaseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	bool with_thresholds = args.ColumnCount() == 5;
	vector<Cursor> cursors(args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		string raw_state;
		if (!ArgString(args.data[0], row, raw_state) || args.data[1].GetValue(row).IsNull() ||
		    args.data[2].GetValue(row).IsNull()) {
			continue;
		}
		auto predicate_list = args.data[1].GetValue(row);
		auto label_list = args.data[2].GetValue(row);
		vector<string> predicates;
		vector<string> labels;
		vector<double> thresholds;
		for (auto &child : ListValue::GetChildren(predicate_list)) {
			predicates.push_back(child.IsNull() ? "" : StringValue::Get(child));
		}
		for (auto &child : ListValue::GetChildren(label_list)) {
			labels.push_back(child.IsNull() ? "" : StringValue::Get(child));
		}
		string otherwise = "unmatched";
		if (with_thresholds) {
			auto threshold_list = args.data[3].GetValue(row);
			if (!threshold_list.IsNull()) {
				for (auto &child : ListValue::GetChildren(threshold_list)) {
					thresholds.push_back(child.IsNull() ? 0.5 : child.GetValue<double>());
				}
			}
			if (!args.data[4].GetValue(row).IsNull()) {
				otherwise = StringValue::Get(args.data[4].GetValue(row));
			}
		} else if (!args.data[3].GetValue(row).IsNull()) {
			otherwise = StringValue::Get(args.data[3].GetValue(row));
		}
		auto tree = SemanticTree(predicates, labels, thresholds, otherwise);
		cursors[row].active = true;
		cursors[row].evidence = EncodeState(raw_state);
		cursors[row].node = std::move(tree);
		cursors[row].tree_id = Md5Hex(cursors[row].node.Dump());
	}
	RunTrees(context, cursors);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!cursors[row].active) {
			SetNull(result, row);
		} else if (cursors[row].status == "complete") {
			result.SetValue(row, Value(cursors[row].label));
		} else {
			SetNull(result, row);
		}
	}
}

void SaveTreeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	for (idx_t row = 0; row < args.size(); row++) {
		string name;
		string tree;
		if (!ArgString(args.data[0], row, name) || !ArgString(args.data[1], row, tree)) {
			SetNull(result, row);
			continue;
		}
		if (Trim(name).empty()) {
			throw InvalidInputException("decision tree name must not be empty");
		}
		auto checked = CheckedTree(tree);
		SaveTree(context, Trim(name), checked.Dump());
		result.SetValue(row, Value::BOOLEAN(true));
	}
}

void TreeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	for (idx_t row = 0; row < args.size(); row++) {
		string name;
		if (!ArgString(args.data[0], row, name)) {
			SetNull(result, row);
			continue;
		}
		string tree;
		if (!LoadTree(context, Trim(name), tree)) {
			throw InvalidInputException("decision tree '" + Trim(name) + "' does not exist");
		}
		result.SetValue(row, Value(tree));
	}
}

void RunTreeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	vector<Cursor> cursors(args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		string name;
		string raw_state;
		if (!ArgString(args.data[0], row, name) || !ArgString(args.data[1], row, raw_state)) {
			continue;
		}
		string tree_text;
		if (!LoadTree(context, Trim(name), tree_text)) {
			throw InvalidInputException("decision tree '" + Trim(name) + "' does not exist");
		}
		cursors[row].active = true;
		cursors[row].evidence = EncodeState(raw_state);
		cursors[row].node = CheckedTree(tree_text);
		cursors[row].tree_id = Md5Hex(cursors[row].node.Dump());
		cursors[row].max_steps = MaxStepsOf(args, row, 2);
	}
	RunTrees(context, cursors);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!cursors[row].active) {
			SetNull(result, row);
		} else {
			result.SetValue(row, CursorValue(cursors[row]));
		}
	}
}

string Base64(const string &bytes) {
	static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	string out;
	out.reserve(((bytes.size() + 2) / 3) * 4);
	int value = 0;
	int bits = -6;
	for (unsigned char ch : bytes) {
		value = (value << 8) + ch;
		bits += 8;
		while (bits >= 0) {
			out.push_back(alphabet[(value >> bits) & 0x3F]);
			bits -= 6;
		}
	}
	if (bits > -6) {
		out.push_back(alphabet[((value << 8) >> (bits + 8)) & 0x3F]);
	}
	while (out.size() % 4) {
		out.push_back('=');
	}
	return out;
}

bool ImageDataUrl(const string &image) {
	return image.rfind("data:image/png;base64,", 0) == 0 || image.rfind("data:image/jpeg;base64,", 0) == 0;
}

string ImagePartJson(const string &url) {
	Json image_url = Json::Object();
	image_url.Set("url", Json::String(url));
	Json part = Json::Object();
	part.Set("type", Json::String("image_url"));
	part.Set("image_url", std::move(image_url));
	return part.Dump();
}

void ImagePartTextFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		string image;
		if (!ArgString(args.data[0], row, image)) {
			SetNull(result, row);
			continue;
		}
		if (!ImageDataUrl(image)) {
			throw InvalidInputException(
			    "image must be a base64 PNG or JPEG data URL (data:image/jpeg;base64,...)");
		}
		result.SetValue(row, Value(ImagePartJson(image)));
	}
}

void ImagePartBlobFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		auto value = args.data[0].GetValue(row);
		if (value.IsNull()) {
			SetNull(result, row);
			continue;
		}
		auto bytes = StringValue::Get(value);
		string kind;
		if (bytes.size() >= 8 && static_cast<unsigned char>(bytes[0]) == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' &&
		    bytes[3] == 'G') {
			kind = "image/png";
		} else if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF &&
		           static_cast<unsigned char>(bytes[1]) == 0xD8) {
			kind = "image/jpeg";
		} else {
			throw InvalidInputException("image bytes must be a PNG or JPEG file");
		}
		result.SetValue(row, Value(ImagePartJson("data:" + kind + ";base64," + Base64(bytes))));
	}
}

void BoxQuestionFunction(DataChunk &args, ExpressionState &, Vector &result) {
	for (idx_t row = 0; row < args.size(); row++) {
		string target;
		if (!ArgString(args.data[0], row, target)) {
			SetNull(result, row);
			continue;
		}
		target = Trim(target);
		if (target.empty() || target.size() > 2000) {
			throw InvalidInputException("target must contain 1 to 2000 characters");
		}
		Json question = Json::Object();
		question.Set("type", Json::String("box"));
		question.Set("instructions", Json::String(target));
		Json questions = Json::Object();
		questions.Set("result", std::move(question));
		result.SetValue(row, Value(questions.Dump()));
	}
}

LogicalType BoxResultType() {
	child_list_t<LogicalType> children;
	children.emplace_back(Identifier("top"), LogicalType::DOUBLE);
	children.emplace_back(Identifier("bottom"), LogicalType::DOUBLE);
	children.emplace_back(Identifier("left"), LogicalType::DOUBLE);
	children.emplace_back(Identifier("right"), LogicalType::DOUBLE);
	children.emplace_back(Identifier("confidence"), LogicalType::DOUBLE);
	return LogicalType::STRUCT(std::move(children));
}

void LocateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	vector<string> bodies(args.size());
	vector<char> live(args.size(), 0);
	for (idx_t row = 0; row < args.size(); row++) {
		string raw_state;
		string target;
		if (!ArgString(args.data[0], row, raw_state) || !ArgString(args.data[1], row, target)) {
			SetNull(result, row);
			continue;
		}
		target = Trim(target);
		if (target.empty() || target.size() > 2000) {
			throw InvalidInputException("target must contain 1 to 2000 characters");
		}
		string model = DEFAULT_BOX_MODEL;
		if (args.ColumnCount() > 2) {
			string override_model;
			if (ArgString(args.data[2], row, override_model) && !Trim(override_model).empty()) {
				model = Trim(override_model);
			}
		}
		Json question = Json::Object();
		question.Set("type", Json::String("box"));
		question.Set("instructions", Json::String(target));
		Json questions = Json::Object();
		questions.Set("result", std::move(question));
		bodies[row] = DecisionBody(EncodeState(raw_state), std::move(questions), model);
		live[row] = 1;
	}
	vector<string> needed;
	for (idx_t row = 0; row < args.size(); row++) {
		if (live[row]) {
			needed.push_back(bodies[row]);
		}
	}
	auto resolved = needed.empty() ? unordered_map<string, string> {} : ResolveDecisions(context, needed);
	auto type = BoxResultType();
	for (idx_t row = 0; row < args.size(); row++) {
		if (!live[row]) {
			continue;
		}
		auto found = resolved.find(bodies[row]);
		if (found == resolved.end()) {
			SetNull(result, row);
			continue;
		}
		auto parsed = Json::Parse(found->second);
		auto answers = parsed.Find("answers");
		auto answer = answers ? answers->Find("result") : nullptr;
		auto box = answer ? answer->Find("box") : nullptr;
		if (!box) {
			SetNull(result, row);
			continue;
		}
		vector<Value> values;
		for (auto name : {"top", "bottom", "left", "right"}) {
			values.push_back(Value::DOUBLE(box->Find(name)->num));
		}
		auto confidence = answer->Find("confidence");
		values.push_back(confidence && confidence->IsNumber() ? Value::DOUBLE(confidence->num) : Value(LogicalType::DOUBLE));
		result.SetValue(row, Value::STRUCT(type, std::move(values)));
	}
}

void TruthRankFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto provider = ResolveProvider(context, LoadSettings(context));
	vector<string> bodies(args.size());
	vector<char> live(args.size(), 0);
	for (idx_t row = 0; row < args.size(); row++) {
		string raw_state;
		string question;
		if (!ArgString(args.data[0], row, raw_state) || !ArgString(args.data[1], row, question) ||
		    args.data[2].GetValue(row).IsNull()) {
			SetNull(result, row);
			continue;
		}
		question = Trim(question);
		if (question.empty() || question.size() > 2000) {
			throw InvalidInputException("question must contain 1 to 2000 characters");
		}
		auto levels_value = args.data[2].GetValue(row);
		auto levels = ListValue::GetChildren(levels_value);
		if (levels.size() < 2 || levels.size() > 16) {
			throw InvalidInputException("truth_rank needs 2 to 16 levels");
		}
		Json criteria = Json::Object();
		for (auto &level : levels) {
			if (level.IsNull()) {
				throw InvalidInputException("truth_rank levels must be text");
			}
			auto label = Trim(StringValue::Get(level));
			if (label.empty() || criteria.Has(label)) {
				throw InvalidInputException("truth_rank levels must be distinct and non-empty");
			}
			criteria.Set(label, Json::String(label));
		}
		string model = provider.decision_model;
		if (args.ColumnCount() > 3) {
			string override_model;
			if (ArgString(args.data[3], row, override_model) && !Trim(override_model).empty()) {
				model = Trim(override_model);
			}
		}
		Json spec = Json::Object();
		spec.Set("type", Json::String("score"));
		spec.Set("instructions", Json::String(question));
		spec.Set("criteria", std::move(criteria));
		Json questions = Json::Object();
		questions.Set("result", std::move(spec));
		bodies[row] = DecisionBody(EncodeState(raw_state), std::move(questions), model);
		live[row] = 1;
	}
	vector<string> needed;
	for (idx_t row = 0; row < args.size(); row++) {
		if (live[row]) {
			needed.push_back(bodies[row]);
		}
	}
	auto resolved = needed.empty() ? unordered_map<string, string> {} : ResolveDecisions(context, needed);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!live[row]) {
			continue;
		}
		auto found = resolved.find(bodies[row]);
		if (found == resolved.end()) {
			SetNull(result, row);
			continue;
		}
		auto parsed = Json::Parse(found->second);
		auto answers = parsed.Find("answers");
		auto answer = answers ? answers->Find("result") : nullptr;
		auto score = answer ? answer->Find("score") : nullptr;
		if (!score || !score->IsString()) {
			SetNull(result, row);
		} else {
			result.SetValue(row, Value(score->str));
		}
	}
}

ScalarFunction VolatileFun(const string &name, std::initializer_list<FunctionParameter> params, LogicalType ret,
                           scalar_function_t func) {
	ScalarFunction fun(Identifier(name), params, std::move(ret), func);
	fun.SetVolatile();
	fun.SetFallible();
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

ScalarFunction StableFun(const string &name, std::initializer_list<FunctionParameter> params, LogicalType ret,
                         scalar_function_t func) {
	ScalarFunction fun(Identifier(name), params, std::move(ret), func);
	fun.SetFallible();
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace

void RegisterDecisionFunctions(ExtensionLoader &loader) {
	auto varchar_list = LogicalType::LIST(LogicalType::VARCHAR);
	auto double_list = LogicalType::LIST(LogicalType::DOUBLE);
	auto tree_type = TreeResultType();
	loader.RegisterFunction(VolatileFun(
	    "truth",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("predicate", LogicalType::VARCHAR),
	     FunctionParameter("threshold", LogicalType::DOUBLE, Value::DOUBLE(0.5))},
	    LogicalType::BOOLEAN, TruthFunction));
	loader.RegisterFunction(VolatileFun(
	    "truth_score",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("predicate", LogicalType::VARCHAR)},
	    LogicalType::DOUBLE, TruthFunction));
	ScalarFunctionSet choose("choose");
	choose.AddFunction(VolatileFun(
	    "choose",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("categories", varchar_list)},
	    LogicalType::VARCHAR, ChooseFunction));
	choose.AddFunction(VolatileFun(
	    "choose",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("categories", LogicalType::VARCHAR)},
	    LogicalType::VARCHAR, ChooseFunction));
	loader.RegisterFunction(choose);
	loader.RegisterFunction(VolatileFun(
	    "decide",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("questions", LogicalType::VARCHAR),
	     FunctionParameter("model", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))},
	    LogicalType::VARCHAR, DecideFunction));
	loader.RegisterFunction(
	    StableFun("truth_question", {FunctionParameter("predicate", LogicalType::VARCHAR)}, LogicalType::VARCHAR,
	              TruthQuestionFunction));
	ScalarFunctionSet choice_question("choice_question");
	choice_question.AddFunction(StableFun("choice_question", {FunctionParameter("categories", varchar_list)},
	                                      LogicalType::VARCHAR, ChoiceQuestionFunction));
	choice_question.AddFunction(StableFun("choice_question", {FunctionParameter("categories", LogicalType::VARCHAR)},
	                                      LogicalType::VARCHAR, ChoiceQuestionFunction));
	loader.RegisterFunction(choice_question);
	loader.RegisterFunction(
	    StableFun("leaf", {FunctionParameter("label", LogicalType::VARCHAR)}, LogicalType::VARCHAR, LeafFunction));
	loader.RegisterFunction(StableFun(
	    "branch",
	    {FunctionParameter("question", LogicalType::VARCHAR), FunctionParameter("yes", LogicalType::VARCHAR),
	     FunctionParameter("no", LogicalType::VARCHAR),
	     FunctionParameter("threshold", LogicalType::DOUBLE, Value::DOUBLE(0.5)),
	     FunctionParameter("inputs", varchar_list, Value(varchar_list))},
	    LogicalType::VARCHAR, BranchFunction));
	loader.RegisterFunction(StableFun(
	    "choice_branch",
	    {FunctionParameter("question", LogicalType::VARCHAR), FunctionParameter("choices", LogicalType::VARCHAR),
	     FunctionParameter("routes", LogicalType::VARCHAR), FunctionParameter("inputs", varchar_list, Value(varchar_list))},
	    LogicalType::VARCHAR, ChoiceBranchFunction));
	loader.RegisterFunction(VolatileFun(
	    "decision_tree",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("tree", LogicalType::VARCHAR),
	     FunctionParameter("max_steps", LogicalType::INTEGER, Value::INTEGER(16))},
	    tree_type, DecisionTreeFunction));
	loader.RegisterFunction(VolatileFun(
	    "decision_tree_batch",
	    {FunctionParameter("states", varchar_list), FunctionParameter("tree", LogicalType::VARCHAR),
	     FunctionParameter("max_steps", LogicalType::INTEGER, Value::INTEGER(16))},
	    LogicalType::LIST(tree_type), DecisionTreeBatchFunction));
	ScalarFunctionSet semantic_case("semantic_case");
	semantic_case.AddFunction(VolatileFun(
	    "semantic_case",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("predicates", varchar_list),
	     FunctionParameter("labels", varchar_list),
	     FunctionParameter("otherwise_label", LogicalType::VARCHAR, Value("unmatched"))},
	    LogicalType::VARCHAR, SemanticCaseFunction));
	semantic_case.AddFunction(VolatileFun(
	    "semantic_case",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("predicates", varchar_list),
	     FunctionParameter("labels", varchar_list), FunctionParameter("thresholds", double_list),
	     FunctionParameter("otherwise_label", LogicalType::VARCHAR, Value("unmatched"))},
	    LogicalType::VARCHAR, SemanticCaseFunction));
	loader.RegisterFunction(semantic_case);
	loader.RegisterFunction(VolatileFun(
	    "save_tree",
	    {FunctionParameter("name", LogicalType::VARCHAR), FunctionParameter("tree", LogicalType::VARCHAR)},
	    LogicalType::BOOLEAN, SaveTreeFunction));
	loader.RegisterFunction(
	    VolatileFun("tree", {FunctionParameter("name", LogicalType::VARCHAR)}, LogicalType::VARCHAR, TreeFunction));
	loader.RegisterFunction(VolatileFun(
	    "run_tree",
	    {FunctionParameter("name", LogicalType::VARCHAR), FunctionParameter("state", LogicalType::VARCHAR),
	     FunctionParameter("max_steps", LogicalType::INTEGER, Value::INTEGER(16))},
	    tree_type, RunTreeFunction));
	ScalarFunctionSet image_part("image_part");
	image_part.AddFunction(
	    StableFun("image_part", {FunctionParameter("image", LogicalType::VARCHAR)}, LogicalType::VARCHAR,
	              ImagePartTextFunction));
	image_part.AddFunction(
	    StableFun("image_part", {FunctionParameter("image", LogicalType::BLOB)}, LogicalType::VARCHAR,
	              ImagePartBlobFunction));
	loader.RegisterFunction(image_part);
	loader.RegisterFunction(StableFun("box_question", {FunctionParameter("target", LogicalType::VARCHAR)},
	                                  LogicalType::VARCHAR, BoxQuestionFunction));
	loader.RegisterFunction(VolatileFun(
	    "locate",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("target", LogicalType::VARCHAR),
	     FunctionParameter("model", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))},
	    BoxResultType(), LocateFunction));
	loader.RegisterFunction(VolatileFun(
	    "truth_rank",
	    {FunctionParameter("state", LogicalType::VARCHAR), FunctionParameter("question", LogicalType::VARCHAR),
	     FunctionParameter("levels", varchar_list),
	     FunctionParameter("model", LogicalType::VARCHAR, Value(LogicalType::VARCHAR))},
	    LogicalType::VARCHAR, TruthRankFunction));
}

} // namespace rvbbit
} // namespace duckdb
