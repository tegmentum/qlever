// Copyright 2026 tegmentum
//
// wf:call SPARQL filter function for QLever.
//
// v0.2: extends the v0.1 skeleton so args_[1..] are marshalled onto the
// wire and the guest's binding-sets reply is projected back to a real
// SPARQL literal or IRI. Variables (non-constant expressions) still
// resolve to UNDEF — a proper batched evaluation would need per-row
// invocation, which is a follow-up.
//
// Wire format follows what qlever-wf-runtime already emits from its
// component-mode `serialize_binding_sets` helper (and expects on the
// arg side, once wired). The C++ side is the authoritative reference
// for the arg direction because the runtime currently ignores the
// payload — sending well-formed JSON now means no churn when the
// runtime starts consuming it.
//
//   value := {"iri": "..."}
//          | {"bnode": "..."}
//          | {"literal": {"label": "...", "datatype": "...",
//                         "lang": null | "..."}}
//   args   := [value, value, ...]
//   reply  := {"vars": ["x", ...],
//              "rows": [[{"name": "x", "value": <value>}, ...], ...]}

#include "engine/sparqlExpressions/WfCallExpression.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "engine/sparqlExpressions/LiteralExpression.h"
#include "engine/sparqlExpressions/SparqlExpressionValueGetters.h"
#include "global/Constants.h"
#include "index/ExportIds.h"
#include "parser/LiteralOrIri.h"
#include "rdfTypes/Iri.h"
#include "rdfTypes/Literal.h"
#include "util/json.h"

#ifdef QLEVER_ENABLE_WF
#include "engine/sparqlExpressions/WasmRuntime.h"
#endif

namespace sparqlExpression {
namespace {

using LiteralOrIri = ad_utility::triple_component::LiteralOrIri;
using Literal = ad_utility::triple_component::Literal;
using Iri = ad_utility::triple_component::Iri;

// Result-side conversion signal: a "match failed" from the reply
// parser (e.g. no rows, malformed JSON) collapses to UNDEF at the
// call site rather than aborting the whole query.
struct WfCallFailed {};

// ---- arg marshalling ------------------------------------------------------

// Convert a resolved LiteralOrIri into the WIT `value`-variant JSON shape.
// Kept in one place so args (out) and results (in) speak the same dialect.
nlohmann::json literalOrIriToJsonValue(const LiteralOrIri& term) {
  nlohmann::json out = nlohmann::json::object();
  if (term.isIri()) {
    // `getContent()` strips angle brackets; the WIT value carries the
    // bracket-free IRI string, matching Rust's `Val::Variant("iri", ...)`.
    out["iri"] = std::string{asStringViewUnsafe(term.getIri().getContent())};
    return out;
  }
  // Literal: the WIT record splits label/datatype/lang so the guest
  // side can round-trip without re-parsing the QLever-internal quoted
  // form (which embeds datatype/lang as a suffix on the storage).
  const auto& lit = term.getLiteral();
  nlohmann::json inner = nlohmann::json::object();
  inner["label"] = std::string{asStringViewUnsafe(lit.getContent())};
  if (lit.hasLanguageTag()) {
    // A langString literal is xsd:string-shaped over the wire, but the
    // WIT `datatype` field is required, so we emit RDF's canonical
    // langString type — the same choice the Rust runtime made on the
    // reply side (see `write_value` -> literal branch).
    inner["datatype"] = std::string{RDF_LANGTAG_STRING};
    inner["lang"] = std::string{asStringViewUnsafe(lit.getLanguageTag())};
  } else if (lit.hasDatatype()) {
    inner["datatype"] = std::string{asStringViewUnsafe(lit.getDatatype())};
    inner["lang"] = nullptr;
  } else {
    // Plain literal — QLever's `hasDatatype()` returns false for
    // `xsd:string`, so we synthesise it here to keep the WIT record
    // total (the WIT `datatype` field is required, not optional).
    inner["datatype"] = std::string{XSD_STRING};
    inner["lang"] = nullptr;
  }
  out["literal"] = std::move(inner);
  return out;
}

// Try to lift one child expression's scalar constant result into a WIT
// value JSON node. Returns nullopt for non-constant / UNDEF results,
// which the caller propagates as UNDEF for the whole wf:call — matching
// Oxigraph's semantics (any bad arg fails the call, not just the arg).
std::optional<nlohmann::json> childArgToJson(
    const SparqlExpression& child, EvaluationContext* context) {
  ExpressionResult res = child.evaluate(context);
  std::optional<nlohmann::json> out;
  std::visit(
      [&](auto&& val) {
        using T = std::decay_t<decltype(val)>;
        // Extract a single constant from any of the four QLever
        // ExpressionResult shapes that can carry a broadcast constant.
        // A scalar constant may arrive as either IdOrLocalVocabEntry
        // (vocab-carrying) or a bare ValueId (encoded numeric / small
        // vocab id). Both shapes also appear wrapped in a
        // VectorWithMemoryLimit that broadcasts across the row set —
        // for BIND-context we peel off the first element. Anything else
        // (Variable, SetOfIntervals) is a non-constant we can't marshal
        // in v0.2.
        std::optional<LocalVocabEntry> lveHolder;
        std::optional<ValueId> idHolder;
        if constexpr (std::is_same_v<T, IdOrLocalVocabEntry>) {
          if (std::holds_alternative<LocalVocabEntry>(val)) {
            lveHolder = std::get<LocalVocabEntry>(val);
          } else {
            idHolder = std::get<ValueId>(val);
          }
        } else if constexpr (std::is_same_v<
                                 T,
                                 VectorWithMemoryLimit<IdOrLocalVocabEntry>>) {
          if (!val.empty()) {
            const auto& first = val.front();
            if (std::holds_alternative<LocalVocabEntry>(first)) {
              lveHolder = std::get<LocalVocabEntry>(first);
            } else {
              idHolder = std::get<ValueId>(first);
            }
          }
        } else if constexpr (std::is_same_v<T, ValueId>) {
          idHolder = val;
        } else if constexpr (std::is_same_v<
                                 T, VectorWithMemoryLimit<ValueId>>) {
          if (!val.empty()) idHolder = val.front();
        }

        if (lveHolder.has_value()) {
          out = literalOrIriToJsonValue(lveHolder->asLiteralOrIri());
          return;
        }
        if (!idHolder.has_value() || idHolder->isUndefined()) {
          return;  // out stays nullopt
        }
        // Encoded numeric/date/small-vocab-id — resolve via the exportIds
        // helper so we get a proper LiteralOrIri regardless of storage.
        auto lit = ql::exportIds::idToLiteralOrIri(
            context->_qec.getIndex(), *idHolder, context->_localVocab);
        if (lit.has_value()) {
          out = literalOrIriToJsonValue(lit.value());
        }
      },
      res);
  return out;
}

// ---- result parsing -------------------------------------------------------

// Turn a parsed WIT `value` JSON node into an IdOrLocalVocabEntry sitting
// on the current context's LocalVocab. WfCallFailed sentinels propagate
// upward so the outermost handler collapses to UNDEF once.
std::variant<IdOrLocalVocabEntry, WfCallFailed> jsonValueToLocalVocabEntry(
    const nlohmann::json& valueNode, EvaluationContext* context) {
  if (!valueNode.is_object()) {
    return WfCallFailed{};
  }
  if (auto it = valueNode.find("iri");
      it != valueNode.end() && it->is_string()) {
    // fromIriref expects <...>-wrapped input, since it stores the
    // bracket form for round-tripping through the string representation.
    std::string wrapped = "<" + it->get<std::string>() + ">";
    return IdOrLocalVocabEntry{
        LocalVocabEntry::fromIriref(wrapped, context->getLocalVocabContext())};
  }
  if (auto it = valueNode.find("bnode");
      it != valueNode.end() && it->is_string()) {
    // Blank nodes are stored as internal-prefix IRIs so downstream
    // operators (JOIN, DISTINCT, ...) don't have to special-case them.
    // Same convention as BlankNodeExpression::makeBlankNode.
    std::string wrapped = absl::StrCat(
        QLEVER_INTERNAL_BLANK_NODE_IRI_PREFIX, it->get<std::string>(), ">");
    return IdOrLocalVocabEntry{LocalVocabEntry::fromStringRepresentation(
        std::move(wrapped), context->getLocalVocabContext())};
  }
  auto litIt = valueNode.find("literal");
  if (litIt == valueNode.end() || !litIt->is_object()) {
    return WfCallFailed{};
  }
  const auto& lit = *litIt;
  auto labelIt = lit.find("label");
  if (labelIt == lit.end() || !labelIt->is_string()) {
    return WfCallFailed{};
  }
  auto datatypeIt = lit.find("datatype");
  auto langIt = lit.find("lang");
  const std::string label = labelIt->get<std::string>();
  // Descriptor priority: language tag beats datatype; only fall back
  // to bare literal when both are absent or empty. Datatype xsd:string
  // is dropped because Literal::addDatatype trims it anyway — passing
  // it in creates a redundant `^^<xsd:string>` suffix.
  std::optional<std::variant<Iri, std::string>> descriptor;
  const bool hasLang =
      langIt != lit.end() && langIt->is_string() && !langIt->get<std::string>().empty();
  if (hasLang) {
    descriptor = langIt->get<std::string>();
  } else if (datatypeIt != lit.end() && datatypeIt->is_string()) {
    const std::string dt = datatypeIt->get<std::string>();
    if (!dt.empty() && dt != XSD_STRING) {
      descriptor = Iri::fromIrirefWithoutBrackets(dt);
    }
  }
  auto normalised = asNormalizedStringViewUnsafe(label);
  auto literal =
      Literal::literalWithNormalizedContent(normalised, std::move(descriptor));
  return IdOrLocalVocabEntry{
      LocalVocabEntry{std::move(literal), context->getLocalVocabContext()}};
}

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

    // args_[0]: wasm URL. Must be a constant IRI or string literal;
    // variables are rejected as UNDEF for v0.2 for the same reason
    // as the args_[1..] path — batched per-row invocation is a follow-up.
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

    // args_[1..]: marshal each into a WIT value JSON node. A failure
    // to convert any one arg collapses the whole call to UNDEF — this
    // mirrors oxigraph-wf's `term_to_wit_value` error path, which
    // aborts the call rather than substituting placeholders that would
    // silently mis-attribute results.
    nlohmann::json argsArray = nlohmann::json::array();
    for (std::size_t i = 1; i < args_.size(); ++i) {
      auto j = childArgToJson(*args_[i], context);
      if (!j.has_value()) {
        return IdOrLocalVocabEntry{Id::makeUndefined()};
      }
      argsArray.push_back(std::move(j.value()));
    }
    const std::string payload = argsArray.dump();

    std::string reply;
    try {
      reply = wf::WfRuntime::instance().callEvaluate(wasmUrl, payload);
    } catch (const std::exception&) {
      // Swallow to UNDEF — surfacing the exception up the plan would
      // abort the whole query, which is worse than a per-cell miss.
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }

    nlohmann::json parsed;
    try {
      parsed = nlohmann::json::parse(reply);
    } catch (const nlohmann::json::exception&) {
      // Malformed reply — same reasoning as above, degrade to UNDEF
      // rather than surfacing an exception through the plan.
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }

    // Reply shape mirrors the WIT `binding-sets` record. Existing wf:call
    // callers project only the first row's first column (the classic
    // `BIND(wf:call(...))` shape); multi-row/multi-column projection is
    // handled by the SERVICE path, not this expression.
    if (!parsed.is_object()) {
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }
    auto rowsIt = parsed.find("rows");
    if (rowsIt == parsed.end() || !rowsIt->is_array() || rowsIt->empty()) {
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }
    const auto& firstRow = rowsIt->front();
    if (!firstRow.is_array() || firstRow.empty()) {
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }
    const auto& firstBinding = firstRow.front();
    auto valueIt = firstBinding.find("value");
    if (valueIt == firstBinding.end()) {
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }

    auto converted = jsonValueToLocalVocabEntry(*valueIt, context);
    if (std::holds_alternative<WfCallFailed>(converted)) {
      return IdOrLocalVocabEntry{Id::makeUndefined()};
    }
    return std::get<IdOrLocalVocabEntry>(std::move(converted));
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
