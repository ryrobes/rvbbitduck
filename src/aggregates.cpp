#include "engine.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

#include <algorithm>

namespace duckdb {
namespace rvbbit {

namespace {

constexpr idx_t kMaxDistinct = 48;
constexpr idx_t kChunk = 12;
constexpr idx_t kMaxChars = 400;

struct CollectionState {
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
	return sizeof(CollectionState);
}

void InitState(AggregateStateInput &, data_ptr_t *states, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		new (states[i]) CollectionState();
	}
}

void DestroyState(Vector &state, AggregateInputData &, idx_t count) {
	auto data = FlatVector::GetData<CollectionState *>(state);
	for (idx_t i = 0; i < count; i++) {
		if (data[i]) {
			data[i]->~CollectionState();
		}
	}
}

void AddText(CollectionState &state, string text) {
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
	inputs[0].ToUnifiedFormat(count, text_format);
	state.ToUnifiedFormat(count, state_format);
	auto text_data = UnifiedVectorFormat::GetData<string_t>(text_format);
	auto state_data = UnifiedVectorFormat::GetData<CollectionState *>(state_format);
	UnifiedVectorFormat category_format;
	const string_t *category_data = nullptr;
	if (input_count > 1) {
		inputs[1].ToUnifiedFormat(count, category_format);
		category_data = UnifiedVectorFormat::GetData<string_t>(category_format);
	}
	for (idx_t row = 0; row < count; row++) {
		auto group = state_data[state_format.sel->get_index(row)];
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
	source.ToUnifiedFormat(count, from_format);
	combined.ToUnifiedFormat(count, into_format);
	auto from = UnifiedVectorFormat::GetData<CollectionState *>(from_format);
	auto into = UnifiedVectorFormat::GetData<CollectionState *>(into_format);
	for (idx_t row = 0; row < count; row++) {
		auto src = from[from_format.sel->get_index(row)];
		auto dst = into[into_format.sel->get_index(row)];
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

string Reduce(ClientContext &context, const string &op, const vector<string> &texts, const string &categories) {
	if (texts.empty()) {
		return "";
	}
	if (texts.size() <= kChunk) {
		Json inputs = Json::Object();
		inputs.Set("text", Json::String(JoinTexts(texts)));
		if (!categories.empty()) {
			inputs.Set("categories", Json::String(categories));
		}
		auto output = EvaluateOperator(context, op, inputs);
		if (texts.size() == kMaxDistinct) {
			// The cap is reported in the docs; the model still sees the kept rows.
		}
		(void)texts;
		return output;
	}
	vector<string> parts;
	for (idx_t start = 0; start < texts.size(); start += kChunk) {
		auto end = std::min(texts.size(), start + kChunk);
		vector<string> chunk(texts.begin() + static_cast<ptrdiff_t>(start), texts.begin() + static_cast<ptrdiff_t>(end));
		auto partial = Reduce(context, op, chunk, categories);
		if (!partial.empty()) {
			parts.push_back(std::move(partial));
		}
	}
	return Reduce(context, op, parts, categories);
}

unique_ptr<FunctionData> Bind(BindAggregateFunctionInput &input) {
	auto data = make_uniq<AggBind>();
	data->context = &input.GetClientContext();
	return std::move(data);
}

void FinalizeText(Vector &state, AggregateFinalizeInputData &input, Vector &result, idx_t count, idx_t offset,
                  const char *op, bool needs_categories) {
	if (!input.bind_data) {
		throw InvalidInputException("rvbbit aggregate is missing its bind data");
	}
	auto &bound = input.bind_data->Cast<AggBind>();
	state.Flatten();
	auto groups = FlatVector::GetData<CollectionState *>(state);
	for (idx_t row = 0; row < count; row++) {
		auto &group = *groups[row];
		if (group.texts.empty()) {
			result.SetValue(offset + row, Value(result.GetType()));
			continue;
		}
		if (needs_categories && group.categories.empty()) {
			throw InvalidInputException("classify_collection needs a category list");
		}
		auto output = Reduce(*bound.context, op, group.texts, group.categories);
		result.SetValue(offset + row, Value(output));
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
	state.Flatten();
	auto groups = FlatVector::GetData<CollectionState *>(state);
	for (idx_t row = 0; row < count; row++) {
		auto &group = *groups[row];
		if (group.texts.empty()) {
			result.SetValue(offset + row, Value(result.GetType()));
			continue;
		}
		auto output = Reduce(*bound.context, "themes", group.texts, "");
		vector<Value> labels;
		try {
			auto parsed = Json::Parse(output);
			if (parsed.IsArray()) {
				for (auto &item : parsed.arr) {
					if (item.IsString() && !item.str.empty()) {
						labels.push_back(Value(item.str));
					}
				}
			}
		} catch (const std::exception &) {
		}
		if (labels.empty() && !output.empty()) {
			labels.push_back(Value(output));
		}
		result.SetValue(offset + row, Value::LIST(LogicalType::VARCHAR, std::move(labels)));
	}
}

AggregateFunction MakeAggregate(const char *name, vector<LogicalType> args, LogicalType ret, aggregate_finalize_t finalize) {
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
	loader.RegisterFunction(MakeAggregate("consensus", {LogicalType::VARCHAR}, LogicalType::VARCHAR, FinalizeConsensus));
	loader.RegisterFunction(MakeAggregate("collection_themes", {LogicalType::VARCHAR},
	                                      LogicalType::LIST(LogicalType::VARCHAR), FinalizeThemes));
}

} // namespace rvbbit
} // namespace duckdb
