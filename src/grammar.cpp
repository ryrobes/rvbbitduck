#include "engine.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/grammar_extension.hpp"
#include "duckdb/parser/peg/transformer/parse_result.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"

#include <cctype>

namespace duckdb {
namespace rvbbit {

namespace {

string Lower(string value) {
	for (auto &ch : value) {
		ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
	}
	return value;
}

ParseResult &Unwrap(ParseResult &result) {
	if (result.type == ParseResultType::CHOICE) {
		return Unwrap(result.Cast<ChoiceParseResult>().GetResult());
	}
	if (result.type == ParseResultType::LIST) {
		auto &list = result.Cast<ListParseResult>();
		if (list.GetChildren().size() == 1) {
			return Unwrap(list.GetChild(0));
		}
	}
	return result;
}

[[noreturn]] void BadShape(ParseResult &result, const string &rule) {
	throw ParserException("rvbbit grammar could not transform %s:\n%s", rule, result.ToString());
}

unique_ptr<ParsedExpression> AsExpression(PEGTransformer &transformer, ParseResult &result) {
	auto &inner = Unwrap(result);
	if (inner.type == ParseResultType::STRING) {
		return inner.Cast<StringLiteralParseResult>().ToExpression();
	}
	if (inner.type == ParseResultType::NUMBER) {
		return ConstantExpression::Number(inner.Cast<NumberParseResult>().number);
	}
	return transformer.Transform<unique_ptr<ParsedExpression>>(inner);
}

string AsIdentifier(ParseResult &result) {
	auto &inner = Unwrap(result);
	if (inner.type == ParseResultType::IDENTIFIER) {
		return inner.Cast<IdentifierParseResult>().identifier.GetIdentifierName();
	}
	if (inner.type == ParseResultType::KEYWORD) {
		return inner.Cast<KeywordParseResult>().keyword;
	}
	BadShape(result, "identifier");
}

unique_ptr<ParsedExpression> Call(const string &name, vector<unique_ptr<ParsedExpression>> args) {
	return make_uniq<FunctionExpression>(QualifiedName(Identifier(), Identifier("rvbbit"), Identifier(name)),
	                                     std::move(args));
}

unique_ptr<ParsedExpression> LeafCall(unique_ptr<ParsedExpression> label) {
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(label));
	return Call("leaf", std::move(args));
}

unique_ptr<ParsedExpression> BranchCall(unique_ptr<ParsedExpression> question, unique_ptr<ParsedExpression> yes,
                                        unique_ptr<ParsedExpression> no, unique_ptr<ParsedExpression> threshold) {
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(question));
	args.push_back(std::move(yes));
	args.push_back(std::move(no));
	args.push_back(std::move(threshold));
	return Call("branch", std::move(args));
}

unique_ptr<ParsedExpression> ThresholdFrom(PEGTransformer &transformer, OptionalParseResult &optional) {
	if (!optional.HasResult()) {
		return ConstantExpression::Number("0.5");
	}
	auto &list = Unwrap(optional.GetResult()).Cast<ListParseResult>();
	if (list.GetChildren().size() < 2) {
		BadShape(optional.GetResult(), "ABOVE");
	}
	return AsExpression(transformer, list.GetChild(1));
}

unique_ptr<ParsedExpression> TreeFromNode(PEGTransformer &transformer, ParseResult &result);

unique_ptr<ParsedExpression> TreeFromBranch(PEGTransformer &transformer, ParseResult &result) {
	auto &list = Unwrap(result).Cast<ListParseResult>();
	if (list.GetChildren().size() < 7) {
		BadShape(result, "BRANCH");
	}
	if (Lower(AsIdentifier(list.GetChild(0))) != "branch") {
		throw ParserException("decision tree branch must start with BRANCH");
	}
	auto question = AsExpression(transformer, list.GetChild(1));
	auto &above = list.Child<OptionalParseResult>(2);
	auto yes = TreeFromNode(transformer, list.GetChild(4));
	auto no = TreeFromNode(transformer, list.GetChild(6));
	return BranchCall(std::move(question), std::move(yes), std::move(no), ThresholdFrom(transformer, above));
}

unique_ptr<ParsedExpression> TreeFromNode(PEGTransformer &transformer, ParseResult &result) {
	auto &inner = Unwrap(result);
	if (inner.type == ParseResultType::STRING) {
		return LeafCall(inner.Cast<StringLiteralParseResult>().ToExpression());
	}
	if (inner.type == ParseResultType::LIST) {
		auto &list = inner.Cast<ListParseResult>();
		if (!list.GetChildren().empty() && Unwrap(list.GetChild(0)).type == ParseResultType::IDENTIFIER) {
			return TreeFromBranch(transformer, inner);
		}
	}
	return AsExpression(transformer, inner);
}

arena_ptr<TransformResultValue> FinalizeAtom(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	if (list.GetChildren().empty()) {
		BadShape(parse_result, "atom");
	}
	auto expr = AsExpression(transformer, list.GetChild(0));
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(expr));
}

arena_ptr<TransformResultValue> FinalizeRelation(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	if (list.GetChildren().size() < 3) {
		BadShape(parse_result, "relation");
	}
	auto word = Lower(AsIdentifier(list.GetChild(1)));
	if (word != "contradicts" && word != "supports" && word != "implies") {
		throw ParserException("expected CONTRADICTS, SUPPORTS, or IMPLIES, got '%s'", word);
	}
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(AsExpression(transformer, list.GetChild(0)));
	args.push_back(AsExpression(transformer, list.GetChild(2)));
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(Call(word, std::move(args)));
}

arena_ptr<TransformResultValue> FinalizeMeans(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	if (list.GetChildren().size() < 4) {
		BadShape(parse_result, "MEANS");
	}
	auto word = Lower(AsIdentifier(list.GetChild(1)));
	auto left = AsExpression(transformer, list.GetChild(0));
	auto right = AsExpression(transformer, list.GetChild(2));
	auto &above = list.Child<OptionalParseResult>(3);
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(left));
	args.push_back(std::move(right));
	if (word == "means" && above.HasResult()) {
		args.push_back(ThresholdFrom(transformer, above));
		return transformer.MakeResult<unique_ptr<ParsedExpression>>(Call("truth", std::move(args)));
	}
	if (word == "means") {
		return transformer.MakeResult<unique_ptr<ParsedExpression>>(Call("means", std::move(args)));
	}
	if (word == "about" || word == "near") {
		auto scored = Call(word == "near" ? "similarity" : "about", std::move(args));
		if (!above.HasResult()) {
			return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(scored));
		}
		auto threshold = ThresholdFrom(transformer, above);
		return transformer.MakeResult<unique_ptr<ParsedExpression>>(make_uniq<ComparisonExpression>(
		    ExpressionType::COMPARE_GREATERTHAN, std::move(scored), std::move(threshold)));
	}
	if (word == "contradicts" || word == "supports" || word == "implies") {
		if (above.HasResult()) {
			throw ParserException("%s does not take ABOVE", word);
		}
		return transformer.MakeResult<unique_ptr<ParsedExpression>>(Call(word, std::move(args)));
	}
	throw ParserException("expected MEANS, ABOUT, NEAR, CONTRADICTS, SUPPORTS, or IMPLIES, got '%s'", word);
}

arena_ptr<TransformResultValue> FinalizeSemanticCase(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	if (list.GetChildren().size() < 6) {
		BadShape(parse_result, "SEMANTIC CASE");
	}
	auto state = AsExpression(transformer, list.GetChild(3));
	auto &whens = list.Child<RepeatParseResult>(4);
	unique_ptr<ParsedExpression> otherwise = ConstantExpression::String("unmatched");
	auto &else_clause = list.Child<OptionalParseResult>(5);
	if (else_clause.HasResult()) {
		auto &else_list = Unwrap(else_clause.GetResult()).Cast<ListParseResult>();
		otherwise = AsExpression(transformer, else_list.GetChild(else_list.GetChildren().size() - 1));
	}
	unique_ptr<ParsedExpression> node = LeafCall(std::move(otherwise));
	auto children = whens.GetChildren();
	for (idx_t i = children.size(); i-- > 0;) {
		auto &when_list = Unwrap(children[i].get()).Cast<ListParseResult>();
		if (when_list.GetChildren().size() < 5) {
			BadShape(children[i].get(), "WHEN");
		}
		auto predicate = AsExpression(transformer, when_list.GetChild(1));
		auto &above = when_list.Child<OptionalParseResult>(2);
		auto label = AsExpression(transformer, when_list.GetChild(4));
		node = BranchCall(std::move(predicate), LeafCall(std::move(label)), std::move(node),
		                  ThresholdFrom(transformer, above));
	}
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(state));
	args.push_back(std::move(node));
	// CASE yields the label, the same way rvbbit.semantic_case does. The struct
	// is still available from rvbbit.decision_tree.
	vector<unique_ptr<ParsedExpression>> extract_args;
	extract_args.push_back(Call("decision_tree", std::move(args)));
	extract_args.push_back(ConstantExpression::String("label"));
	auto extracted = make_uniq<FunctionExpression>(Identifier("struct_extract"), std::move(extract_args));
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(extracted));
}

arena_ptr<TransformResultValue> FinalizeDecide(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	if (list.GetChildren().size() < 4) {
		BadShape(parse_result, "DECIDE");
	}
	auto name = AsIdentifier(list.GetChild(1));
	auto state = AsExpression(transformer, list.GetChild(3));
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(ConstantExpression::String(name));
	args.push_back(std::move(state));
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(Call("run_tree", std::move(args)));
}

arena_ptr<TransformResultValue> FinalizeTreeNode(PEGTransformer &transformer, ParseResult &parse_result) {
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(TreeFromNode(transformer, parse_result));
}

arena_ptr<TransformResultValue> FinalizeCreateTree(PEGTransformer &transformer, ParseResult &parse_result) {
	auto &list = parse_result.Cast<ListParseResult>();
	if (list.GetChildren().size() < 6) {
		BadShape(parse_result, "CREATE DECISION TREE");
	}
	auto name = AsIdentifier(list.GetChild(3));
	auto tree = TreeFromNode(transformer, list.GetChild(5));
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(ConstantExpression::String(name));
	args.push_back(std::move(tree));
	auto select = make_uniq<SelectStatement>();
	auto node = make_uniq<SelectNode>();
	node->select_list.push_back(Call("save_tree", std::move(args)));
	node->from_table = make_uniq<EmptyTableRef>();
	select->node = std::move(node);
	return transformer.MakeResult<unique_ptr<SQLStatement>>(std::move(select));
}

arena_ptr<TransformProcess> Start(PEGTransformer &transformer, ParseResult &parse_result,
                                  transform_finalize_function_t finalize) {
	return transformer.MakeProcess<FinalizeTransformProcess>(transformer, parse_result, finalize);
}

class RvbbitGrammarExtension : public GrammarExtension {
public:
	RvbbitGrammarExtension()
	    : GrammarExtension("rvbbit", "Semantic predicates, SEMANTIC CASE, and CREATE DECISION TREE. Opt-in via "
	                                 "active_grammar_extensions. Every form desugars to rvbbit.* functions.") {
	}

	vector<GrammarChange> GetChanges() const override {
		vector<GrammarChange> changes;
		for (auto word : {"'SEMANTIC'", "'ABOVE'", "'DECISION'", "'TREE'", "'DECIDE'"}) {
			changes.push_back(GrammarChange::AddChoice("UnreservedKeyword", word));
		}
		changes.push_back(GrammarChange::AddRule(
		    "RvbbitAtom <- ColumnReference / LiteralExpression / FunctionExpression / ParensExpression / "
		    "StructExpression / CaseExpression",
		    [](PEGTransformer &transformer, ParseResult &result) {
			    return Start(transformer, result, FinalizeAtom);
		    }));
		changes.push_back(GrammarChange::AddRule(
		    "RvbbitMeansExpr <- RvbbitAtom Identifier StringLiteral RvbbitAboveOpt?",
		    [](PEGTransformer &transformer, ParseResult &result) {
			    return Start(transformer, result, FinalizeMeans);
		    }));
		changes.push_back(GrammarChange::AddRule("RvbbitAboveOpt <- 'ABOVE' NumberLiteral"));
		changes.push_back(GrammarChange::AddRule(
		    "RvbbitSemanticCase <- 'SEMANTIC' 'CASE' 'ON' Expression RvbbitWhen+ RvbbitElse? 'END'",
		    [](PEGTransformer &transformer, ParseResult &result) {
			    return Start(transformer, result, FinalizeSemanticCase);
		    }));
		changes.push_back(GrammarChange::AddRule(
		    "RvbbitWhen <- 'WHEN' StringLiteral RvbbitAboveOpt? 'THEN' Expression"));
		changes.push_back(GrammarChange::AddRule("RvbbitElse <- 'ELSE' Expression"));
		changes.push_back(GrammarChange::AddRule(
		    "RvbbitDecideExpr <- 'DECIDE' Identifier 'FOR' Expression",
		    [](PEGTransformer &transformer, ParseResult &result) {
			    return Start(transformer, result, FinalizeDecide);
		    }));
		changes.push_back(GrammarChange::AddRule(
		    "RvbbitTreeNode <- RvbbitBranchNode / StringLiteral",
		    [](PEGTransformer &transformer, ParseResult &result) {
			    return Start(transformer, result, FinalizeTreeNode);
		    }));
		changes.push_back(GrammarChange::AddRule(
		    "RvbbitBranchNode <- Identifier StringLiteral RvbbitAboveOpt? 'YES' RvbbitTreeNode 'NO' RvbbitTreeNode"));
		changes.push_back(GrammarChange::AddRule(
		    "RvbbitCreateDecisionTree <- 'CREATE' 'DECISION' 'TREE' Identifier 'AS' RvbbitTreeNode",
		    [](PEGTransformer &transformer, ParseResult &result) {
			    return Start(transformer, result, FinalizeCreateTree);
		    }));
		changes.push_back(GrammarChange::AddRule(
		    "RvbbitRelationExpr <- RvbbitAtom Identifier RvbbitAtom",
		    [](PEGTransformer &transformer, ParseResult &result) {
			    return Start(transformer, result, FinalizeRelation);
		    }));
		// Relation is prepended first so MEANS/ABOUT (which consume ABOVE) are tried before it.
		changes.push_back(GrammarChange::PrependChoice("SingleExpression", "RvbbitRelationExpr"));
		changes.push_back(GrammarChange::PrependChoice("SingleExpression", "RvbbitMeansExpr"));
		changes.push_back(GrammarChange::PrependChoice("SingleExpression", "RvbbitSemanticCase"));
		changes.push_back(GrammarChange::PrependChoice("SingleExpression", "RvbbitDecideExpr"));
		// Try this before CREATE TABLE so a failed CreateStatement does not have to backtrack
		// out of a committed CREATE token.
		changes.push_back(GrammarChange::PrependChoice("Statement", "RvbbitCreateDecisionTree"));
		return changes;
	}
};

} // namespace

void RegisterGrammar(ExtensionLoader &loader) {
	GrammarExtension::Register(loader.GetDatabaseInstance(), make_shared_ptr<RvbbitGrammarExtension>());
	SetGrammarRegistered(true);
}

} // namespace rvbbit
} // namespace duckdb
