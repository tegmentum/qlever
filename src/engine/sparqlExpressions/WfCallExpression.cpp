// Copyright 2026 tegmentum
//
// wf:call SPARQL filter function for QLever.
//
// v0.1 SCOPE: this cut proves the parser wire-up, the expression dispatch,
// and the result-shape marshalling. The actual wasmtime invocation is
// stubbed to return a fixed marker literal for now — real wasm execution
// lands in a follow-up once the C API version handshake is stable.
//
// The full design (URL-keyed module cache, malloc/free/evaluate ABI,
// SPARQL-JSON in-memory marshalling) lives in git history.

#include "engine/sparqlExpressions/WfCallExpression.h"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "engine/sparqlExpressions/LiteralExpression.h"
#include "engine/sparqlExpressions/SparqlExpressionValueGetters.h"
#include "global/Constants.h"
#include "parser/LiteralOrIri.h"

namespace sparqlExpression {
namespace {

class WfCallExpression : public SparqlExpression {
  std::vector<Ptr> args_;

 public:
  explicit WfCallExpression(std::vector<Ptr> args) : args_(std::move(args)) {}

  ExpressionResult evaluate(EvaluationContext* /*context*/) const override {
    // Marker payload — swap for real wasmtime invocation once the C-API
    // handshake settles. Returning IdOrLocalVocabEntry with an undefined
    // Id sidesteps LiteralOrIri construction ambiguities in this file
    // while we finalize the invocation path.
    return IdOrLocalVocabEntry{Id::makeUndefined()};
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
