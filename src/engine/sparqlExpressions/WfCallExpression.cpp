// Copyright 2026 tegmentum
//
// wf:call SPARQL filter function for QLever.
//
// v0.1: the first argument must be a constant IRI or string literal
// naming the wasm module (file:// or http(s)://). Remaining arguments
// are currently ignored on the wire — the guest sees an empty JSON
// payload — while the parameter-marshalling design is finalised.
// The guest's `evaluate` reply is parsed with a minimal SPARQL-JSON
// extractor: the first `"value":"..."` occurrence is lifted verbatim
// as the return literal (xsd:string).
//
// Component Model wasm is out of scope for v0.1; the module-mode ABI
// mirrors the Stardog `webfunctions.engine.mode=module` path.

#include "engine/sparqlExpressions/WfCallExpression.h"

#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "engine/sparqlExpressions/LiteralExpression.h"
#include "engine/sparqlExpressions/SparqlExpressionValueGetters.h"
#include "global/Constants.h"
#include "parser/LiteralOrIri.h"
#include "rdfTypes/Literal.h"

#ifdef QLEVER_ENABLE_WF
#include "engine/sparqlExpressions/WasmRuntime.h"
#endif

namespace sparqlExpression {
namespace {

using LiteralOrIri = ad_utility::triple_component::LiteralOrIri;
using Literal = ad_utility::triple_component::Literal;

class WfCallExpression : public SparqlExpression {
  std::vector<Ptr> args_;

 public:
  explicit WfCallExpression(std::vector<Ptr> args) : args_(std::move(args)) {}

  ExpressionResult evaluate(
      [[maybe_unused]] EvaluationContext* context) const override {
#ifndef QLEVER_ENABLE_WF
    // Build was configured without wasm support. Rather than throw at
    // query-plan time, we return UNDEF so callers can still parse and
    // plan queries containing wf:call — they just won't get results.
    return IdOrLocalVocabEntry{Id::makeUndefined()};
#else
    if (args_.empty()) {
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }

    // v0.1 requires the first arg to evaluate to a constant IRI or
    // string literal — variables are rejected as UNDEF for now.
    ExpressionResult firstEval = args_[0]->evaluate(context);
    std::string wasmUrl;
    bool haveUrl = false;
    std::visit(
        [&](auto&& val) {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, IdOrLocalVocabEntry>) {
            if (std::holds_alternative<LocalVocabEntry>(val)) {
              const auto& lve = std::get<LocalVocabEntry>(val);
              if (lve.isIri() || lve.isLiteral()) {
                wasmUrl = std::string(asStringViewUnsafe(lve.getContent()));
                haveUrl = true;
              }
            }
          }
        },
        firstEval);

    if (!haveUrl) {
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }

    std::string reply;
    try {
      // v0.1 wire format: empty JSON payload. Argument marshalling into
      // SPARQL-JSON bindings lands with the parameter-passing pass.
      reply = wf::WfRuntime::instance().callEvaluate(wasmUrl, "{}");
    } catch (const std::exception&) {
      // Swallow to UNDEF for now — the alternative is to bubble an
      // exception through the plan, which would abort the whole query.
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }

    // Naive extractor: find `"value":"..."` and lift the inner string.
    // Full SPARQL-JSON parsing lands with the results-marshalling pass.
    std::string_view r{reply};
    const std::string_view needle = "\"value\":\"";
    auto pos = r.find(needle);
    if (pos == std::string_view::npos) {
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }
    pos += needle.size();
    auto endPos = r.find('"', pos);
    if (endPos == std::string_view::npos) {
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }
    std::string extracted(r.substr(pos, endPos - pos));

    // Wrap the extracted string as an xsd:string literal in the local
    // vocab, matching the pattern used by CONCAT et al. in
    // StringExpressions.cpp.
    auto lit = Literal::literalWithNormalizedContent(
        asNormalizedStringViewUnsafe(extracted));
    return IdOrLocalVocabEntry{
        LocalVocabEntry{std::move(lit), context->getLocalVocabContext()}};
#endif
  }

  std::span<SparqlExpression::Ptr> childrenImpl() override {
    return {args_.data(), args_.size()};
  }

  std::string getCacheKey(
      const VariableToColumnMap& varColMap) const override {
    std::string k = "wf:call(";
    for (const auto& a : args_) k += a->getCacheKey(varColMap) + ',';
    k += ")";
    return k;
  }

  bool isDeterministic() const override { return false; }
};

}  // namespace

SparqlExpression::Ptr makeWfCallExpression(std::vector<SparqlExpression::Ptr> args) {
  return std::make_unique<WfCallExpression>(std::move(args));
}

}  // namespace sparqlExpression
