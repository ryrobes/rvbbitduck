#include "engine.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <unordered_map>

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

//! Calliope prefixes nomic, e5, and bge before the cache key. Other models,
//! and an empty mode, keep the raw text.
string ApplyModePrefix(const string &text, const string &mode, const string &model) {
	auto kind = Lower(Trim(mode));
	auto name = Lower(model);
	const char *prefix = nullptr;
	if (kind == "query" && name.find("nomic") != string::npos) {
		prefix = "search_query: ";
	} else if ((kind == "document" || kind == "doc") && name.find("nomic") != string::npos) {
		prefix = "search_document: ";
	} else if (kind == "query" && name.find("e5") != string::npos) {
		prefix = "query: ";
	} else if ((kind == "document" || kind == "doc") && name.find("e5") != string::npos) {
		prefix = "passage: ";
	} else if (kind == "query" && name.find("bge") != string::npos) {
		prefix = "Represent this sentence for searching relevant passages: ";
	}
	if (!prefix) {
		return text;
	}
	return string(prefix) + text;
}

string EmbedCacheKey(const string &model, int32_t dim, const string &text) {
	return Md5Hex("embed\n" + model + "\n" + std::to_string(dim) + "\n" + text);
}

vector<double> ParseVector(const string &payload) {
	auto parsed = Json::Parse(payload);
	if (!parsed.IsArray() || parsed.arr.empty()) {
		throw InvalidInputException("cached embedding is not a vector");
	}
	vector<double> values;
	values.reserve(parsed.arr.size());
	for (auto &item : parsed.arr) {
		if (!item.IsNumber() || !std::isfinite(item.num)) {
			throw InvalidInputException("cached embedding is not a vector");
		}
		values.push_back(item.num);
	}
	return values;
}

string DumpVector(const vector<double> &values) {
	Json array = Json::Array();
	for (auto value : values) {
		array.arr.push_back(Json::Number(value));
	}
	return array.Dump();
}

double Cosine(const vector<double> &left, const vector<double> &right) {
	if (left.empty() || left.size() != right.size()) {
		return 0;
	}
	double dot = 0;
	double left_norm = 0;
	double right_norm = 0;
	for (idx_t i = 0; i < left.size(); i++) {
		dot += left[i] * right[i];
		left_norm += left[i] * left[i];
		right_norm += right[i] * right[i];
	}
	if (left_norm == 0 || right_norm == 0) {
		return 0;
	}
	return dot / (std::sqrt(left_norm) * std::sqrt(right_norm));
}

Value VectorValue(const vector<double> &values) {
	vector<Value> children;
	children.reserve(values.size());
	for (auto value : values) {
		children.push_back(Value::FLOAT(static_cast<float>(value)));
	}
	return Value::LIST(LogicalType::FLOAT, std::move(children));
}

vector<string> TextsOf(const Value &value, const char *what) {
	if (value.IsNull()) {
		throw InvalidInputException(string(what) + " must be a list of text");
	}
	auto children = ListValue::GetChildren(value);
	vector<string> texts;
	texts.reserve(children.size());
	for (auto &child : children) {
		if (child.IsNull()) {
			continue;
		}
		texts.push_back(StringValue::Get(child));
	}
	if (texts.size() > 2000) {
		throw InvalidInputException(string(what) + " accepts at most 2000 texts; filter the list in SQL first");
	}
	return texts;
}

//! One embeddings POST per chunk of unique misses. Cache key includes the
//! mode prefix, model, and dimension, matching Calliope's embed_one.
vector<vector<double>> EmbedTexts(ClientContext &context, const vector<string> &texts, const string &mode) {
	auto settings = LoadSettings(context);
	auto provider = ResolveProvider(context, settings);
	if (provider.embed_dim < 2 || provider.embed_dim > 4096) {
		throw InvalidInputException("rvbbit_embed_dim must be between 2 and 4096");
	}
	vector<string> prepared(texts.size());
	vector<vector<double>> vectors(texts.size());
	vector<char> ready(texts.size(), 0);
	vector<string> missing;
	unordered_map<string, vector<idx_t>> rows_for;
	for (idx_t row = 0; row < texts.size(); row++) {
		if (Trim(texts[row]).empty()) {
			throw InvalidInputException("text to embed must not be empty");
		}
		prepared[row] = ApplyModePrefix(texts[row], mode, provider.embed_model);
		string cached;
		if (CacheGet(context, EmbedCacheKey(provider.embed_model, provider.embed_dim, prepared[row]), cached)) {
			RecordCacheHit(context, "embed", provider.embed_model);
			vectors[row] = ParseVector(cached);
			ready[row] = 1;
			continue;
		}
		if (rows_for.find(prepared[row]) == rows_for.end()) {
			missing.push_back(prepared[row]);
		}
		rows_for[prepared[row]].push_back(row);
	}
	constexpr idx_t kBatch = 64;
	for (idx_t start = 0; start < missing.size(); start += kBatch) {
		auto end = std::min(missing.size(), start + kBatch);
		Json input = Json::Array();
		for (idx_t i = start; i < end; i++) {
			input.arr.push_back(Json::String(missing[i]));
		}
		Json body = Json::Object();
		body.Set("model", Json::String(provider.embed_model));
		body.Set("input", std::move(input));
		body.Set("dimensions", Json::Number(provider.embed_dim));
		auto fetched = FetchPost(context, "embed", {body.Dump()});
		if (fetched.empty() || !fetched[0].ok) {
			string message = fetched.empty() ? "embedding request failed" : fetched[0].error;
			throw InvalidInputException("rvbbit.embed: " + message);
		}
		Json response;
		try {
			response = Json::Parse(fetched[0].body);
		} catch (const std::exception &ex) {
			throw InvalidInputException(string("rvbbit.embed: ") + ex.what());
		}
		auto data = response.Find("data");
		if (!data || !data->IsArray()) {
			throw InvalidInputException("rvbbit.embed: response has no data array");
		}
		vector<char> seen(end - start, 0);
		for (idx_t item_index = 0; item_index < data->arr.size(); item_index++) {
			auto &item = data->arr[item_index];
			if (!item.IsObject()) {
				throw InvalidInputException("rvbbit.embed: embedding item is not an object");
			}
			idx_t slot = item_index;
			if (auto index = item.Find("index"); index && index->IsNumber()) {
				slot = static_cast<idx_t>(index->num);
			}
			if (slot >= end - start) {
				throw InvalidInputException("rvbbit.embed: embedding index is out of range");
			}
			auto embedding = item.Find("embedding");
			if (!embedding || !embedding->IsArray() || embedding->arr.size() != static_cast<size_t>(provider.embed_dim)) {
				throw InvalidInputException("rvbbit.embed: embedding length does not match rvbbit_embed_dim");
			}
			vector<double> values;
			values.reserve(embedding->arr.size());
			for (auto &number : embedding->arr) {
				if (!number.IsNumber() || !std::isfinite(number.num)) {
					throw InvalidInputException("rvbbit.embed: embedding contains a non-numeric value");
				}
				values.push_back(number.num);
			}
			auto &text = missing[start + slot];
			CachePut(context, EmbedCacheKey(provider.embed_model, provider.embed_dim, text), DumpVector(values));
			for (auto row : rows_for[text]) {
				vectors[row] = values;
				ready[row] = 1;
			}
			seen[slot] = 1;
		}
		for (auto flag : seen) {
			if (!flag) {
				throw InvalidInputException("rvbbit.embed: response omitted an input");
			}
		}
	}
	for (auto flag : ready) {
		if (!flag) {
			throw InvalidInputException("rvbbit.embed: a text was not embedded");
		}
	}
	return vectors;
}

void EmbedFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	vector<string> texts(args.size());
	vector<string> modes(args.size(), "");
	vector<char> live(args.size(), 0);
	for (idx_t row = 0; row < args.size(); row++) {
		if (!ArgString(args.data[0], row, texts[row])) {
			SetNull(result, row);
			continue;
		}
		if (args.ColumnCount() > 1) {
			ArgString(args.data[1], row, modes[row]);
		}
		live[row] = 1;
	}
	//! Mode prefixes change the cache key, so each distinct mode is its own batch.
	unordered_map<string, vector<idx_t>> by_mode;
	for (idx_t row = 0; row < args.size(); row++) {
		if (live[row]) {
			by_mode[modes[row]].push_back(row);
		}
	}
	for (auto &entry : by_mode) {
		vector<string> batch;
		batch.reserve(entry.second.size());
		for (auto row : entry.second) {
			batch.push_back(texts[row]);
		}
		auto vectors = EmbedTexts(context, batch, entry.first);
		for (idx_t i = 0; i < entry.second.size(); i++) {
			result.SetValue(entry.second[i], VectorValue(vectors[i]));
		}
	}
}

void SimilarityFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	vector<string> texts;
	vector<idx_t> left_index(args.size(), static_cast<idx_t>(-1));
	vector<idx_t> right_index(args.size(), static_cast<idx_t>(-1));
	unordered_map<string, idx_t> position;
	auto intern = [&](const string &text) {
		auto found = position.find(text);
		if (found != position.end()) {
			return found->second;
		}
		auto index = texts.size();
		texts.push_back(text);
		position[text] = index;
		return index;
	};
	for (idx_t row = 0; row < args.size(); row++) {
		string left;
		string right;
		if (!ArgString(args.data[0], row, left) || !ArgString(args.data[1], row, right)) {
			SetNull(result, row);
			continue;
		}
		left_index[row] = intern(left);
		right_index[row] = intern(right);
	}
	auto vectors = texts.empty() ? vector<vector<double>> {} : EmbedTexts(context, texts, "");
	for (idx_t row = 0; row < args.size(); row++) {
		if (left_index[row] == static_cast<idx_t>(-1)) {
			continue;
		}
		result.SetValue(row, Value::DOUBLE(Cosine(vectors[left_index[row]], vectors[right_index[row]])));
	}
}

void NearestLabelFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	for (idx_t row = 0; row < args.size(); row++) {
		string text;
		string otherwise_label = "";
		double min_score = 0;
		if (!ArgString(args.data[0], row, text)) {
			SetNull(result, row);
			continue;
		}
		if (args.data[1].GetValue(row).IsNull() || args.data[2].GetValue(row).IsNull()) {
			SetNull(result, row);
			continue;
		}
		auto conditions_value = args.data[1].GetValue(row);
		auto results_value = args.data[2].GetValue(row);
		auto conditions = TextsOf(conditions_value, "conditions");
		auto labels = TextsOf(results_value, "results");
		if (args.ColumnCount() > 3 && !args.data[3].GetValue(row).IsNull()) {
			otherwise_label = StringValue::Get(args.data[3].GetValue(row));
		}
		if (args.ColumnCount() > 4 && !args.data[4].GetValue(row).IsNull()) {
			min_score = args.data[4].GetValue(row).GetValue<double>();
		}
		if (conditions.size() != labels.size()) {
			throw InvalidInputException("nearest_label conditions and results must be the same length");
		}
		if (conditions.empty() || Trim(text).empty()) {
			result.SetValue(row, Value(otherwise_label));
			continue;
		}
		vector<string> needed;
		needed.push_back(text);
		needed.insert(needed.end(), conditions.begin(), conditions.end());
		auto vectors = EmbedTexts(context, needed, "");
		idx_t best = 0;
		double best_score = -2;
		for (idx_t i = 0; i < conditions.size(); i++) {
			auto score = Cosine(vectors[0], vectors[i + 1]);
			if (score > best_score) {
				best_score = score;
				best = i;
			}
		}
		if (best_score < min_score) {
			result.SetValue(row, Value(otherwise_label));
		} else {
			result.SetValue(row, Value(labels[best]));
		}
	}
}

struct RowsData : FunctionData {
	vector<vector<Value>> rows;
	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<RowsData>();
		copy->rows = rows;
		return std::move(copy);
	}
	bool Equals(const FunctionData &) const override {
		return false;
	}
};

struct RowState : GlobalTableFunctionState {
	vector<vector<Value>> rows;
	idx_t offset = 0;
};

void RowScan(ClientContext &, TableFunctionInput &input, DataChunk &output) {
	auto &state = input.global_state->Cast<RowState>();
	idx_t count = 0;
	while (state.offset < state.rows.size() && count < STANDARD_VECTOR_SIZE) {
		auto &row = state.rows[state.offset++];
		for (idx_t column = 0; column < row.size() && column < output.ColumnCount(); column++) {
			output.data[column].SetValue(count, row[column]);
		}
		count++;
	}
	output.SetCardinalityUnsafe(count);
}

unique_ptr<GlobalTableFunctionState> RowInit(ClientContext &, TableFunctionInitInput &input) {
	auto state = make_uniq<RowState>();
	if (input.bind_data) {
		state->rows = input.bind_data->Cast<RowsData>().rows;
	}
	return std::move(state);
}

unique_ptr<FunctionData> KnnBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                 vector<Identifier> &names) {
	names = {Identifier("value"), Identifier("score")};
	return_types = {LogicalType::VARCHAR, LogicalType::DOUBLE};
	auto data = make_uniq<RowsData>();
	if (input.inputs.size() < 3 || input.inputs[0].IsNull() || input.inputs[1].IsNull() || input.inputs[2].IsNull()) {
		return std::move(data);
	}
	auto query = StringValue::Get(input.inputs[0]);
	auto corpus = TextsOf(input.inputs[1], "knn_text");
	auto k = input.inputs[2].GetValue<int32_t>();
	if (k <= 0 || Trim(query).empty() || corpus.empty()) {
		return std::move(data);
	}
	vector<string> needed;
	needed.push_back(query);
	needed.insert(needed.end(), corpus.begin(), corpus.end());
	auto vectors = EmbedTexts(context, needed, "");
	vector<std::pair<double, string>> scored;
	scored.reserve(corpus.size());
	for (idx_t i = 0; i < corpus.size(); i++) {
		scored.push_back({Cosine(vectors[0], vectors[i + 1]), corpus[i]});
	}
	std::sort(scored.begin(), scored.end(), [](const std::pair<double, string> &left, const std::pair<double, string> &right) {
		if (left.first != right.first) {
			return left.first > right.first;
		}
		return left.second < right.second;
	});
	if (static_cast<idx_t>(k) < scored.size()) {
		scored.resize(static_cast<size_t>(k));
	}
	for (auto &item : scored) {
		data->rows.push_back({Value(item.second), Value::DOUBLE(item.first)});
	}
	return std::move(data);
}

vector<int> KMeans(const vector<vector<double>> &vectors, int k) {
	auto n = static_cast<int>(vectors.size());
	auto dim = static_cast<int>(vectors[0].size());
	vector<int> assign(static_cast<size_t>(n), 0);
	vector<vector<double>> centroids(static_cast<size_t>(k));
	for (int cluster = 0; cluster < k; cluster++) {
		centroids[static_cast<size_t>(cluster)] = vectors[static_cast<size_t>((cluster * n) / k)];
	}
	for (int iter = 0; iter < 8; iter++) {
		bool changed = false;
		for (int row = 0; row < n; row++) {
			int best = 0;
			double best_score = -2;
			for (int cluster = 0; cluster < k; cluster++) {
				auto score = Cosine(vectors[static_cast<size_t>(row)], centroids[static_cast<size_t>(cluster)]);
				if (score > best_score) {
					best_score = score;
					best = cluster;
				}
			}
			if (assign[static_cast<size_t>(row)] != best) {
				assign[static_cast<size_t>(row)] = best;
				changed = true;
			}
		}
		vector<vector<double>> sums(static_cast<size_t>(k), vector<double>(static_cast<size_t>(dim), 0));
		vector<int> counts(static_cast<size_t>(k), 0);
		for (int row = 0; row < n; row++) {
			auto cluster = assign[static_cast<size_t>(row)];
			counts[static_cast<size_t>(cluster)]++;
			for (int axis = 0; axis < dim; axis++) {
				sums[static_cast<size_t>(cluster)][static_cast<size_t>(axis)] +=
				    vectors[static_cast<size_t>(row)][static_cast<size_t>(axis)];
			}
		}
		for (int cluster = 0; cluster < k; cluster++) {
			if (!counts[static_cast<size_t>(cluster)]) {
				continue;
			}
			for (int axis = 0; axis < dim; axis++) {
				centroids[static_cast<size_t>(cluster)][static_cast<size_t>(axis)] =
				    sums[static_cast<size_t>(cluster)][static_cast<size_t>(axis)] /
				    counts[static_cast<size_t>(cluster)];
			}
		}
		if (!changed && iter > 0) {
			break;
		}
	}
	return assign;
}

unique_ptr<FunctionData> TopicsBind(ClientContext &context, TableFunctionBindInput &input,
                                    vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {Identifier("cluster_id"), Identifier("size"), Identifier("exemplar")};
	return_types = {LogicalType::INTEGER, LogicalType::BIGINT, LogicalType::VARCHAR};
	auto data = make_uniq<RowsData>();
	if (input.inputs.size() < 2 || input.inputs[0].IsNull() || input.inputs[1].IsNull()) {
		return std::move(data);
	}
	auto texts = TextsOf(input.inputs[0], "topics");
	auto k = input.inputs[1].GetValue<int32_t>();
	if (texts.empty() || k <= 0) {
		return std::move(data);
	}
	unordered_map<string, int64_t> counts;
	vector<string> distinct;
	for (auto &text : texts) {
		if (!counts[text]++) {
			distinct.push_back(text);
		}
	}
	auto k_eff = std::min(k, static_cast<int32_t>(distinct.size()));
	k_eff = std::min(k_eff, static_cast<int32_t>(16));
	auto vectors = EmbedTexts(context, distinct, "");
	auto assign = KMeans(vectors, k_eff);
	struct Cluster {
		int64_t size = 0;
		vector<double> sum;
		idx_t exemplar = 0;
		double best = -2;
	};
	vector<Cluster> clusters(static_cast<size_t>(k_eff));
	for (int cluster = 0; cluster < k_eff; cluster++) {
		clusters[static_cast<size_t>(cluster)].sum.assign(vectors[0].size(), 0);
	}
	for (idx_t row = 0; row < distinct.size(); row++) {
		auto &cluster = clusters[static_cast<size_t>(assign[row])];
		auto copies = counts[distinct[row]];
		cluster.size += copies;
		for (idx_t axis = 0; axis < vectors[row].size(); axis++) {
			cluster.sum[axis] += vectors[row][axis] * static_cast<double>(copies);
		}
	}
	for (auto &cluster : clusters) {
		if (!cluster.size) {
			continue;
		}
		for (auto &axis : cluster.sum) {
			axis /= static_cast<double>(cluster.size);
		}
	}
	for (idx_t row = 0; row < distinct.size(); row++) {
		auto &cluster = clusters[static_cast<size_t>(assign[row])];
		auto score = Cosine(cluster.sum, vectors[row]);
		if (score > cluster.best) {
			cluster.best = score;
			cluster.exemplar = row;
		}
	}
	vector<idx_t> order;
	for (idx_t cluster = 0; cluster < clusters.size(); cluster++) {
		if (clusters[cluster].size) {
			order.push_back(cluster);
		}
	}
	std::sort(order.begin(), order.end(), [&](idx_t left, idx_t right) {
		if (clusters[left].size != clusters[right].size) {
			return clusters[left].size > clusters[right].size;
		}
		return distinct[clusters[left].exemplar] < distinct[clusters[right].exemplar];
	});
	for (auto cluster : order) {
		data->rows.push_back({Value::INTEGER(static_cast<int32_t>(cluster)), Value::BIGINT(clusters[cluster].size),
		                      Value(distinct[clusters[cluster].exemplar])});
	}
	return std::move(data);
}

idx_t FindRoot(vector<idx_t> &parent, idx_t node) {
	while (parent[node] != node) {
		parent[node] = parent[parent[node]];
		node = parent[node];
	}
	return node;
}

unique_ptr<FunctionData> DedupeBind(ClientContext &context, TableFunctionBindInput &input,
                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	names = {Identifier("group_id"), Identifier("representative"), Identifier("size"), Identifier("members")};
	return_types = {LogicalType::INTEGER, LogicalType::VARCHAR, LogicalType::BIGINT,
	                LogicalType::LIST(LogicalType::VARCHAR)};
	auto data = make_uniq<RowsData>();
	if (input.inputs.empty() || input.inputs[0].IsNull()) {
		return std::move(data);
	}
	auto texts = TextsOf(input.inputs[0], "dedupe_groups");
	double threshold = 0.7;
	if (input.inputs.size() > 1 && !input.inputs[1].IsNull()) {
		threshold = input.inputs[1].GetValue<double>();
	}
	if (texts.empty()) {
		return std::move(data);
	}
	unordered_map<string, int64_t> counts;
	vector<string> distinct;
	for (auto &text : texts) {
		if (!counts[text]++) {
			distinct.push_back(text);
		}
	}
	auto vectors = EmbedTexts(context, distinct, "");
	vector<idx_t> parent(distinct.size());
	for (idx_t i = 0; i < parent.size(); i++) {
		parent[i] = i;
	}
	for (idx_t i = 0; i < distinct.size(); i++) {
		for (idx_t j = i + 1; j < distinct.size(); j++) {
			if (Cosine(vectors[i], vectors[j]) >= threshold) {
				parent[FindRoot(parent, i)] = FindRoot(parent, j);
			}
		}
	}
	unordered_map<idx_t, vector<idx_t>> groups;
	for (idx_t i = 0; i < distinct.size(); i++) {
		groups[FindRoot(parent, i)].push_back(i);
	}
	struct Group {
		int64_t size = 0;
		string representative;
		vector<string> members;
	};
	vector<Group> rows;
	for (auto &entry : groups) {
		Group group;
		for (auto index : entry.second) {
			group.size += counts[distinct[index]];
			group.members.push_back(distinct[index]);
			if (group.representative.empty() || distinct[index].size() > group.representative.size() ||
			    (distinct[index].size() == group.representative.size() && distinct[index] < group.representative)) {
				group.representative = distinct[index];
			}
		}
		std::sort(group.members.begin(), group.members.end());
		rows.push_back(std::move(group));
	}
	std::sort(rows.begin(), rows.end(), [](const Group &left, const Group &right) {
		if (left.size != right.size) {
			return left.size > right.size;
		}
		return left.representative < right.representative;
	});
	int32_t group_id = 0;
	for (auto &group : rows) {
		vector<Value> members;
		for (auto &member : group.members) {
			members.push_back(Value(member));
		}
		data->rows.push_back({Value::INTEGER(group_id++), Value(group.representative), Value::BIGINT(group.size),
		                      Value::LIST(LogicalType::VARCHAR, std::move(members))});
	}
	return std::move(data);
}

unique_ptr<FunctionData> DiffBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                                  vector<Identifier> &names) {
	names = {Identifier("text"), Identifier("novelty")};
	return_types = {LogicalType::VARCHAR, LogicalType::DOUBLE};
	auto data = make_uniq<RowsData>();
	if (input.inputs.size() < 3 || input.inputs[0].IsNull() || input.inputs[2].IsNull()) {
		return std::move(data);
	}
	auto left = TextsOf(input.inputs[0], "diff");
	vector<string> right;
	if (!input.inputs[1].IsNull()) {
		right = TextsOf(input.inputs[1], "diff");
	}
	auto k = input.inputs[2].GetValue<int32_t>();
	if (k <= 0 || left.empty()) {
		return std::move(data);
	}
	vector<string> distinct_left;
	unordered_map<string, char> seen_left;
	for (auto &text : left) {
		if (!seen_left[text]++) {
			distinct_left.push_back(text);
		}
	}
	vector<string> distinct_right;
	unordered_map<string, char> seen_right;
	for (auto &text : right) {
		if (!seen_right[text]++) {
			distinct_right.push_back(text);
		}
	}
	vector<string> needed = distinct_left;
	needed.insert(needed.end(), distinct_right.begin(), distinct_right.end());
	auto vectors = needed.empty() ? vector<vector<double>> {} : EmbedTexts(context, needed, "");
	vector<std::pair<double, string>> scored;
	for (idx_t i = 0; i < distinct_left.size(); i++) {
		double max_sim = distinct_right.empty() ? 0 : -1;
		if (seen_right.find(distinct_left[i]) != seen_right.end()) {
			max_sim = 1;
		} else {
			for (idx_t j = 0; j < distinct_right.size(); j++) {
				max_sim = std::max(max_sim, Cosine(vectors[i], vectors[distinct_left.size() + j]));
			}
		}
		double novelty = distinct_right.empty() ? 1.0 : std::max(0.0, std::min(2.0, 1.0 - max_sim));
		scored.push_back({novelty, distinct_left[i]});
	}
	std::sort(scored.begin(), scored.end(), [](const std::pair<double, string> &a, const std::pair<double, string> &b) {
		if (a.first != b.first) {
			return a.first > b.first;
		}
		return a.second < b.second;
	});
	if (static_cast<idx_t>(k) < scored.size()) {
		scored.resize(static_cast<size_t>(k));
	}
	for (auto &item : scored) {
		data->rows.push_back({Value(item.second), Value::DOUBLE(item.first)});
	}
	return std::move(data);
}

ScalarFunction EmbedFun(const string &name, std::initializer_list<FunctionParameter> params, LogicalType ret,
                       scalar_function_t func) {
	ScalarFunction fun(Identifier(name), params, std::move(ret), func);
	fun.SetVolatile();
	fun.SetFallible();
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace

void RegisterEmbeddingFunctions(ExtensionLoader &loader) {
	auto float_list = LogicalType::LIST(LogicalType::FLOAT);
	auto varchar_list = LogicalType::LIST(LogicalType::VARCHAR);
	loader.RegisterFunction(EmbedFun(
	    "embed",
	    {FunctionParameter("text", LogicalType::VARCHAR), FunctionParameter("mode", LogicalType::VARCHAR, Value(""))},
	    float_list, EmbedFunction));
	loader.RegisterFunction(EmbedFun(
	    "similarity",
	    {FunctionParameter("left", LogicalType::VARCHAR), FunctionParameter("right", LogicalType::VARCHAR)},
	    LogicalType::DOUBLE, SimilarityFunction));
	loader.RegisterFunction(EmbedFun(
	    "nearest_label",
	    {FunctionParameter("text", LogicalType::VARCHAR), FunctionParameter("conditions", varchar_list),
	     FunctionParameter("results", varchar_list),
	     FunctionParameter("otherwise_label", LogicalType::VARCHAR, Value("")),
	     FunctionParameter("min_score", LogicalType::DOUBLE, Value::DOUBLE(0))},
	    LogicalType::VARCHAR, NearestLabelFunction));

	TableFunction knn(Identifier("knn_text"),
	                  {LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR), LogicalType::INTEGER}, RowScan,
	                  KnnBind, RowInit);
	loader.RegisterFunction(knn);
	TableFunction topics(Identifier("topics"), {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::INTEGER}, RowScan,
	                     TopicsBind, RowInit);
	loader.RegisterFunction(topics);
	TableFunctionSet dedupe(Identifier("dedupe_groups"));
	dedupe.AddFunction(TableFunction(Identifier("dedupe_groups"), {LogicalType::LIST(LogicalType::VARCHAR)}, RowScan,
	                                 DedupeBind, RowInit));
	dedupe.AddFunction(TableFunction(Identifier("dedupe_groups"),
	                                 {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::DOUBLE}, RowScan, DedupeBind,
	                                 RowInit));
	loader.RegisterFunction(dedupe);
	TableFunction diff(Identifier("diff"),
	                   {LogicalType::LIST(LogicalType::VARCHAR), LogicalType::LIST(LogicalType::VARCHAR),
	                    LogicalType::INTEGER},
	                   RowScan, DiffBind, RowInit);
	loader.RegisterFunction(diff);
}

} // namespace rvbbit
} // namespace duckdb
