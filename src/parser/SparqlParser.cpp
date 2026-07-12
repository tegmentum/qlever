// Copyright 2022, University of Freiburg,
//                 Chair of Algorithms and Data Structures.
// Author: Julian Mundhahs (mundhahj@informatik.uni-freiburg.de)

#include "parser/SparqlParser.h"

#include "engine/sparqlExpressions/WasmRuntime.h"
#include "parser/SparqlParserHelpers.h"

using AntlrParser = SparqlAutomaticParser;

using BnodeMgr = ad_utility::BlankNodeManager*;

namespace {

// Run the qlever-wf-runtime rewrite passes over the raw SPARQL text.
// A hard passthrough when QLEVER_ENABLE_WF is off (the runtime facade
// keeps the same signature so we don't need conditional compilation
// here). On rewrite failure we let the exception propagate — the
// runtime only fails on genuinely malformed SPARQL, and the parser
// would produce a comparable error anyway; surfacing the runtime's
// message first is more actionable because it points at the rewrite
// pass that broke.
//
// Returns the rewritten SPARQL text plus the canonical->alias reverse
// map JSON as a struct so the caller can stash the alias JSON on the
// resulting `ParsedQuery` for the result serialiser to consume later.
sparqlExpression::wf::RewriteResult applyWfRewrite(std::string query) {
  if (!sparqlExpression::wf::WfRuntime::isEnabled()) {
    // The runtime facade would return `query` unchanged anyway when
    // QLEVER_ENABLE_WF is off, but skipping the call entirely avoids
    // constructing the runtime singleton on the disabled path.
    return sparqlExpression::wf::RewriteResult{std::move(query),
                                               std::string{}};
  }
  return sparqlExpression::wf::WfRuntime::instance().rewriteQuery(query);
}

}  // namespace

namespace {
// _____________________________________________________________________________
// Parse the given string as the given clause. If the datasets are not empty,
// then they are fixed during the parsing and cannot be changed by the SPARQL.
template <typename ContextType>
auto parseOperation(BnodeMgr bnodeMgr,
                    const EncodedIriManager* encodedIriManager,
                    ContextType* (SparqlAutomaticParser::*f)(void),
                    std::string operation,
                    const std::vector<DatasetClause>& datasets) {
  using S = std::string;
  // The second argument is the `PrefixMap` for QLever's internal IRIs.
  // The third argument are the datasets from outside the query, which override
  // any datasets in the query.
  sparqlParserHelpers::ParserAndVisitor p{
      bnodeMgr,
      encodedIriManager,
      std::move(operation),
      {{S{QLEVER_INTERNAL_PREFIX_NAME}, S{QLEVER_INTERNAL_PREFIX_IRI}}},
      datasets.empty()
          ? std::nullopt
          : std::optional(parsedQuery::DatasetClauses::fromClauses(datasets))};
  auto resultOfParseAndRemainingText = p.parseTypesafe(f);
  // The query rule ends with <EOF> so the parse always has to consume the whole
  // input. If this is not the case a ParseException should have been thrown at
  // an earlier point.
  AD_CONTRACT_CHECK(resultOfParseAndRemainingText.remainingText_.empty());
  return std::move(resultOfParseAndRemainingText.resultOfParse_);
}
}  // namespace

// _____________________________________________________________________________
ParsedQuery SparqlParser::parseQuery(
    const EncodedIriManager* encodedIriManager, std::string query,
    const std::vector<DatasetClause>& datasets) {
  ad_utility::BlankNodeManager bnodeMgr;
  // Preprocess hook: run the wf runtime's rewrite passes over the raw
  // SPARQL text before it hits the ANTLR parser. Every SPARQL entry
  // point in QLever eventually calls SparqlParser::parseQuery / parseUpdate,
  // so this is the one central seam that catches Server.cpp, Qlever.cpp,
  // MaterializedViews.cpp, and GraphStoreProtocol.cpp in a single place.
  // The hook is a hard passthrough when QLEVER_ENABLE_WF is off, and a
  // near-identity pass when the runtime has no registered aliases.
  //
  // The alias JSON (canonical->alias reverse map) travels with the
  // parsed query on `wfAliasesJson_` so the result serialiser can relabel
  // IRI-typed cells back to whatever alias the client wrote. This
  // supersedes the earlier thread_local staging cell — the alias data
  // is now request-scoped rather than thread-scoped.
  auto rewrite = applyWfRewrite(std::move(query));
  auto res = parseOperation(&bnodeMgr, encodedIriManager, &AntlrParser::query,
                            std::move(rewrite.rewritten), datasets);
  res.wfAliasesJson_ = std::move(rewrite.aliasesJson);
  // Queries never contain blank nodes in the body since they are always turned
  // into internal variables.
  AD_CORRECTNESS_CHECK(bnodeMgr.numBlocksUsed() == 0);
  return res;
}

// _____________________________________________________________________________
std::vector<ParsedQuery> SparqlParser::parseUpdate(
    BnodeMgr bnodeMgr, const EncodedIriManager* encodedIriManager,
    std::string update, const std::vector<DatasetClause>& datasets) {
  // Updates are intentionally not rewritten: qlever-wf-runtime's rewrite
  // passes are query-shaped (SELECT/CONSTRUCT/ASK/DESCRIBE) and would
  // reject or mangle SPARQL Update text. The parser sees updates
  // exactly as sent by the client. If/when Update rewriting lands in
  // the runtime, wire it here symmetrically to parseQuery.
  return parseOperation(bnodeMgr, encodedIriManager, &AntlrParser::update,
                        std::move(update), datasets);
}
