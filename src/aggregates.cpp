#include "engine.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>

namespace duckdb {
namespace rvbbit {

namespace {

constexpr idx_t kMaxDistinct = 48;
constexpr idx_t kChunk = 12;
constexpr idx_t kMaxChars = 400;

//! Heap data. The aggregate slot itself stores only this pointer, because DuckDB
//! memcpy's aggregate states when it moves rows.
struct CollectionData {
	vector<string> texts;
	string categories;
	idx_t dropped = 0;
};

struct AggBind : FunctionData {
	ClientContext *context = nullptr;
	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<AggBind>();
		copy->context = context;
		return std::move(copy);
	}
	bool Equals(const FunctionData &) const override {
		return true;
	}
};

idx_t StateSize(AggregateStateInput &) {
	return sizeof(CollectionData *);
}

void InitState(AggregateStateInput &, data_ptr_t *states, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		auto slot = reinterpret_cast<CollectionData **>(states[i]);
		*slot = new CollectionData();
	}
}

void DestroyState(Vector &state, AggregateInputData &, idx_t count) {
	UnifiedVectorFormat format;
	state.ToUnifiedFormat(format);
	auto slots = const_cast<CollectionData ***>(UnifiedVectorFormat::GetData<CollectionData **>(format));
	for (idx_t i = 0; i < count; i++) {
		auto slot = slots[format.sel->get_index(i)];
		if (!slot || !*slot) {
			continue;
		}
		delete *slot;
		*slot = nullptr;
	}
}

CollectionData *SlotAt(UnifiedVectorFormat &format, idx_t row) {
	auto slots = UnifiedVectorFormat::GetData<CollectionData **>(format);
	auto slot = slots[format.sel->get_index(row)];
	return slot ? *slot : nullptr;
}

void AddText(CollectionData &state, string text) {
	if (text.empty()) {
		return;
	}
	if (text.size() > kMaxChars) {
		text.resize(kMaxChars);
	}
	for (auto &existing : state.texts) {
		if (existing == text) {
			return;
		}
	}
	if (state.texts.size() >= kMaxDistinct) {
		state.dropped++;
		return;
	}
	state.texts.push_back(std::move(text));
}

void Update(Vector inputs[], AggregateInputData &, idx_t input_count, Vector &state, idx_t count) {
	UnifiedVectorFormat text_format;
	UnifiedVectorFormat state_format;
	inputs[0].ToUnifiedFormat(text_format);
	state.ToUnifiedFormat(state_format);
	auto text_data = UnifiedVectorFormat::GetData<string_t>(text_format);
	UnifiedVectorFormat category_format;
	const string_t *category_data = nullptr;
	if (input_count > 1) {
		inputs[1].ToUnifiedFormat(category_format);
		category_data = UnifiedVectorFormat::GetData<string_t>(category_format);
	}
	for (idx_t row = 0; row < count; row++) {
		auto group = SlotAt(state_format, row);
		if (!group) {
			continue;
		}
		auto text_index = text_format.sel->get_index(row);
		if (text_format.validity.RowIsValid(text_index)) {
			AddText(*group, text_data[text_index].GetString());
		}
		if (category_data && group->categories.empty()) {
			auto category_index = category_format.sel->get_index(row);
			if (category_format.validity.RowIsValid(category_index)) {
				group->categories = category_data[category_index].GetString();
			}
		}
	}
}

void Combine(Vector &source, Vector &combined, AggregateInputData &, idx_t count) {
	UnifiedVectorFormat from_format;
	UnifiedVectorFormat into_format;
	source.ToUnifiedFormat(from_format);
	combined.ToUnifiedFormat(into_format);
	for (idx_t row = 0; row < count; row++) {
		auto src = SlotAt(from_format, row);
		auto dst = SlotAt(into_format, row);
		if (!src || !dst || src == dst) {
			continue;
		}
		for (auto &text : src->texts) {
			AddText(*dst, text);
		}
		dst->dropped += src->dropped;
		if (dst->categories.empty()) {
			dst->categories = src->categories;
		}
	}
}

string JoinTexts(const vector<string> &texts) {
	string joined;
	for (auto &text : texts) {
		if (!joined.empty()) {
			joined += "\n";
		}
		joined += "- ";
		joined += text;
	}
	return joined;
}

struct Piece {
	string op;
	vector<string> texts;
	string categories;
	bool skip = false;
};

//! Judge every piece. Chunks from every group share one chat POST per round.
vector<string> JudgeAll(ClientContext &context, vector<Piece> pieces) {
	vector<string> answers(pieces.size());
	vector<char> done(pieces.size(), 0);
	for (idx_t i = 0; i < pieces.size(); i++) {
		if (pieces[i].skip || pieces[i].texts.empty()) {
			done[i] = 1;
		}
	}
	while (true) {
		vector<OperatorCall> calls;
		vector<idx_t> owner;
		bool pending = false;
		for (idx_t i = 0; i < pieces.size(); i++) {
			if (done[i]) {
				continue;
			}
			pending = true;
			auto &texts = pieces[i].texts;
			idx_t parts = (texts.size() + kChunk - 1) / kChunk;
			for (idx_t part = 0; part < parts; part++) {
				auto start = part * kChunk;
				auto end = std::min(texts.size(), start + kChunk);
				vector<string> chunk(texts.begin() + static_cast<ptrdiff_t>(start),
				                     texts.begin() + static_cast<ptrdiff_t>(end));
				OperatorCall call;
				call.name = pieces[i].op;
				call.inputs = Json::Object();
				call.inputs.Set("text", Json::String(JoinTexts(chunk)));
				if (!pieces[i].categories.empty()) {
					call.inputs.Set("categories", Json::String(pieces[i].categories));
				}
				calls.push_back(std::move(call));
				owner.push_back(i);
			}
		}
		if (!pending) {
			break;
		}
		auto outputs = EvaluateOperators(context, std::move(calls));
		vector<vector<string>> next(pieces.size());
		for (idx_t n = 0; n < outputs.size(); n++) {
			if (!outputs[n].empty()) {
				next[owner[n]].push_back(std::move(outputs[n]));
			}
		}
		for (idx_t i = 0; i < pieces.size(); i++) {
			if (done[i]) {
				continue;
			}
			if (pieces[i].texts.size() <= kChunk) {
				answers[i] = next[i].empty() ? string() : next[i][0];
				done[i] = 1;
				continue;
			}
			pieces[i].texts = std::move(next[i]);
			if (pieces[i].texts.empty()) {
				done[i] = 1;
			}
		}
	}
	return answers;
}

unique_ptr<FunctionData> Bind(BindAggregateFunctionInput &input) {
	auto data = make_uniq<AggBind>();
	data->context = &input.GetClientContext();
	return std::move(data);
}

vector<CollectionData *> GroupsOf(Vector &state, idx_t count) {
	UnifiedVectorFormat format;
	state.ToUnifiedFormat(format);
	vector<CollectionData *> groups(count);
	for (idx_t row = 0; row < count; row++) {
		groups[row] = SlotAt(format, row);
	}
	return groups;
}

void FinalizeText(Vector &state, AggregateFinalizeInputData &input, Vector &result, idx_t count, idx_t offset,
                  const char *op, bool needs_categories) {
	if (!input.bind_data) {
		throw InvalidInputException("rvbbit aggregate is missing its bind data");
	}
	auto &bound = input.bind_data->Cast<AggBind>();
	auto groups = GroupsOf(state, count);
	vector<Piece> pieces(count);
	for (idx_t row = 0; row < count; row++) {
		auto group = groups[row];
		if (!group || group->texts.empty()) {
			pieces[row].skip = true;
			continue;
		}
		if (needs_categories && group->categories.empty()) {
			throw InvalidInputException("classify_collection needs a category list");
		}
		pieces[row].op = op;
		pieces[row].texts = group->texts;
		pieces[row].categories = group->categories;
	}
	auto answers = JudgeAll(*bound.context, std::move(pieces));
	for (idx_t row = 0; row < count; row++) {
		if (!groups[row] || groups[row]->texts.empty()) {
			result.SetValue(offset + row, Value(result.GetType()));
			continue;
		}
		result.SetValue(offset + row, Value(answers[row]));
	}
}

void FinalizeClassify(Vector &state, AggregateFinalizeInputData &input, Vector &result, idx_t count, idx_t offset) {
	FinalizeText(state, input, result, count, offset, "classify_collection", true);
}

void FinalizeSummary(Vector &state, AggregateFinalizeInputData &input, Vector &result, idx_t count, idx_t offset) {
	FinalizeText(state, input, result, count, offset, "summarize_collection", false);
}

void FinalizeConsensus(Vector &state, AggregateFinalizeInputData &input, Vector &result, idx_t count, idx_t offset) {
	FinalizeText(state, input, result, count, offset, "consensus", false);
}

void FinalizeThemes(Vector &state, AggregateFinalizeInputData &input, Vector &result, idx_t count, idx_t offset) {
	if (!input.bind_data) {
		throw InvalidInputException("rvbbit aggregate is missing its bind data");
	}
	auto &bound = input.bind_data->Cast<AggBind>();
	auto groups = GroupsOf(state, count);
	vector<Piece> pieces(count);
	for (idx_t row = 0; row < count; row++) {
		auto group = groups[row];
		if (!group || group->texts.empty()) {
			pieces[row].skip = true;
			continue;
		}
		pieces[row].op = "themes";
		pieces[row].texts = group->texts;
	}
	auto answers = JudgeAll(*bound.context, std::move(pieces));
	for (idx_t row = 0; row < count; row++) {
		if (!groups[row] || groups[row]->texts.empty()) {
			result.SetValue(offset + row, Value(result.GetType()));
			continue;
		}
		vector<Value> labels;
		try {
			auto parsed = Json::Parse(answers[row]);
			if (parsed.IsArray()) {
				for (auto &item : parsed.arr) {
					if (item.IsString() && !item.str.empty()) {
						labels.push_back(Value(item.str));
					}
				}
			}
		} catch (const std::exception &) {
		}
		if (labels.empty() && !answers[row].empty()) {
			labels.push_back(Value(answers[row]));
		}
		result.SetValue(offset + row, Value::LIST(LogicalType::VARCHAR, std::move(labels)));
	}
}

AggregateFunction MakeAggregate(const char *name, vector<LogicalType> args, LogicalType ret,
                                aggregate_finalize_t finalize) {
	AggregateFunction function(Identifier(name), std::move(args), std::move(ret), StateSize, InitState, Update, Combine,
	                           finalize, FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, Bind, DestroyState);
	function.SetVolatile();
	function.SetFallible();
	return function;
}

} // namespace

void RegisterAggregateOperators(ExtensionLoader &loader) {
	loader.RegisterFunction(MakeAggregate("classify_collection", {LogicalType::VARCHAR, LogicalType::VARCHAR},
	                                      LogicalType::VARCHAR, FinalizeClassify));
	loader.RegisterFunction(
	    MakeAggregate("summarize_collection", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FinalizeSummary));
	loader.RegisterFunction(
	    MakeAggregate("consensus", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FinalizeConsensus));
	loader.RegisterFunction(MakeAggregate("collection_themes", {LogicalType::VARCHAR},
	                                      LogicalType::LIST(LogicalType::VARCHAR), FinalizeThemes));
}

} // namespace rvbbit
} // namespace duckdb
