// Copyright 2011, University of Freiburg,
// Chair of Algorithms and Data Structures.
//   2011-2017 Björn Buchhold (buchhold@informatik.uni-freiburg.de)
//   2018-     Johannes Kalmbach (kalmbach@informatik.uni-freiburg.de)
//
// Copyright 2025, Bayerische Motoren Werke Aktiengesellschaft (BMW AG)

#include <boost/program_options.hpp>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "CompilationInfo.h"
#include "engine/Server.h"
#include "engine/sparqlExpressions/WasmRuntime.h"
#include "global/Constants.h"
#include "global/RuntimeParameters.h"
#include "libqlever/Qlever.h"
#include "util/MemorySize/MemorySize.h"
#include "util/ParseableDuration.h"
#include "util/ProgramOptionsHelpers.h"
#include "util/ReadableNumberFacet.h"

using std::size_t;
using std::string;

namespace po = boost::program_options;

// Main function.
int main(int argc, char** argv) {
  // TODO<joka921> This is a hack, because the unit tests currently don't work
  // with the strip-columns feature.
  setRuntimeParameter<&RuntimeParameters::stripColumns_>(true);
  // Copy the git hash and datetime of compilation (which require relinking)
  // to make them accessible to other parts of the code
  qlever::version::copyVersionInfo();
  setlocale(LC_CTYPE, "");

  std::locale loc;
  ad_utility::ReadableNumberFacet facet(1);
  std::locale locWithNumberGrouping(loc, &facet);
  ad_utility::Log::imbue(locWithNumberGrouping);

  // Init variables that may or may not be
  // filled / set depending on the options.
  using ad_utility::NonNegative;

  qlever::EngineConfig config;
  std::string accessToken;
  bool noAccessCheck = false;
  unsigned short port;
  NonNegative numSimultaneousQueries = 1;
  bool noMetricsLog = false;

  // wf:call configuration — populated below via optional CLI flags, then
  // pushed into the singleton WfRuntime after po::notify but before the
  // server starts serving queries. All four flags are optional; when none
  // are supplied the runtime boots with empty registries and the rewrite
  // passes stay cheap identities (the pre-flags behaviour).
  std::string wfAliasDb;
  std::string wfAliasTable = "aliases";
  std::string wfShapeDb;
  std::string wfShapeTable = "shapes";
  std::string wfConversionRules;
  std::string wfFetchUrl;

  ad_utility::ParameterToProgramOptionFactory optionFactory{
      &globalRuntimeParameters};

  po::options_description options("Options for qlever-server");
  auto add = [&options](auto&&... args) {
    options.add_options()(AD_FWD(args)...);
  };
  add("help,h", "Produce this help message.");
  add("version,v", "Print version information.");
  // TODO<joka921> Can we output the "required" automatically?
  add("index-basename,i", po::value<std::string>(&config.baseName_)->required(),
      "The basename of the index files (required).");
  add("port,p", po::value<unsigned short>(&port)->required(),
      "The port on which HTTP requests are served (required).");
  add("access-token,a", po::value<std::string>(&accessToken)->default_value(""),
      "Access token for restricted API calls (default: no access).");
  add("no-access-check,n",
      po::bool_switch(&noAccessCheck)->default_value(false),
      "If set to true, no access-token check is performed for restricted API "
      "calls (default: false).");
  add("num-simultaneous-queries,j",
      po::value<NonNegative>(&numSimultaneousQueries)->default_value(1),
      "The number of queries that can be processed simultaneously.");
  add("memory-max-size,m",
      po::value<ad_utility::MemorySize>()
          ->default_value(DEFAULT_MEM_FOR_QUERIES)
          ->notifier([&config](auto v) { config.memoryLimit_ = v; }),
      "Limit on the total amount of memory that can be used for "
      "query processing and caching. If exceeded, query will return with "
      "an error, but the engine will not crash.");
  add("cache-max-size,c",
      optionFactory.getProgramOption<&RuntimeParameters::cacheMaxSize_>(),
      "Maximum memory size for all cache entries (pinned and "
      "not pinned). Note that the cache is part of the total memory "
      "limited by --memory-max-size.");
  add("cache-max-size-single-entry,e",
      optionFactory
          .getProgramOption<&RuntimeParameters::cacheMaxSizeSingleEntry_>(),
      "Maximum size for a single cache entry. That is, "
      "results larger than this will not be cached unless pinned.");
  add("cache-max-size-lazy-result,E",
      optionFactory
          .getProgramOption<&RuntimeParameters::cacheMaxSizeLazyResult_>(),
      "Maximum size up to which lazy results will be cached by aggregating "
      "partial results. Caching does cause significant overhead for this "
      "case.");
  add("cache-max-num-entries,k",
      optionFactory.getProgramOption<&RuntimeParameters::cacheMaxNumEntries_>(),
      "Maximum number of entries in the cache. If exceeded, remove "
      "least-recently used non-pinned entries from the cache. Note that "
      "this condition and the size limit specified via --cache-max-size "
      "both have to hold (logical AND).");
  add("no-patterns,P", po::bool_switch(&config.noPatterns_),
      "Disable the use of patterns. If disabled, the special predicate "
      "`ql:has-predicate` is not available.");
  add("no-metrics-log", po::bool_switch(&noMetricsLog),
      "Disable the per-query metrics log. By default a JSONL log of query "
      "start/end events is written next to the index files "
      "(`<index-basename>.metrics-log.jsonl`).");
  add("text,t", po::bool_switch(&config.loadTextIndex_),
      "Also load the text index. The text index must have been built before "
      "using `qlever-index` with options `-d` and `- w`.");
  add("only-pso-and-pos-permutations,o",
      po::bool_switch(&config.onlyPsoAndPos_),
      "Only load the PSO and POS permutations. This disables queries with "
      "predicate variables.");
  add("default-query-timeout,s",
      optionFactory
          .getProgramOption<&RuntimeParameters::defaultQueryTimeout_>(),
      "Set the default timeout in seconds after which queries are cancelled"
      "automatically.");
  add("service-max-value-rows,S",
      optionFactory
          .getProgramOption<&RuntimeParameters::serviceMaxValueRows_>(),
      "The maximal number of result rows to be passed to a SERVICE operation "
      "as a VALUES clause to optimize its computation.");
  add("throw-on-unbound-variables",
      optionFactory
          .getProgramOption<&RuntimeParameters::throwOnUnboundVariables_>(),
      "If set to true, the queries that use GROUP BY, BIND, or ORDER BY with "
      "variables that are unbound in the query throw an exception. These "
      "queries technically are allowed by the SPARQL standard, but typically "
      "are the result of typos and unintended by the user");
  add("request-body-limit",
      optionFactory.getProgramOption<&RuntimeParameters::requestBodyLimit_>(),
      "Set the maximum size for the body of requests the server will process. "
      "Set to zero to disable the limit.");
  add("cache-service-results",
      optionFactory
          .getProgramOption<&RuntimeParameters::cacheServiceResults_>(),
      "SERVICE is not cached because we have to assume that any remote "
      "endpoint might change at any point in time. If you control the "
      "endpoints, you can override this setting. This will disable the sibling "
      "optimization where VALUES are dynamically pushed into `SERVICE`.");
  add("persist-updates", po::bool_switch(&config.persistUpdates_),
      "If set, then SPARQL UPDATES will be persisted on disk. Otherwise they "
      "will be lost when the engine is stopped");
  add("syntax-test-mode",
      optionFactory.getProgramOption<&RuntimeParameters::syntaxTestMode_>(),
      "Make several query patterns that are syntactially valid, but otherwise "
      "erroneous silently into empty results (e.g. LOAD or SERVICE requests to "
      "nonexisting endpoints). This mode should only be used for running the "
      "syntax tests from the W3C SPARQL 1.1 test suite.");
  add("enable-prefilter-on-index-scans",
      optionFactory
          .getProgramOption<&RuntimeParameters::enablePrefilterOnIndexScans_>(),
      "If set to false, the prefilter procedures for FILTER expressions are "
      "disabled.");
  add("spatial-join-max-num-threads",
      optionFactory
          .getProgramOption<&RuntimeParameters::spatialJoinMaxNumThreads_>(),
      "The maximum number of threads to be used for spatial join processing. "
      "If this option is set to `0`, the number of CPU threads will be used.");
  add("spatial-join-prefilter-max-size",
      optionFactory
          .getProgramOption<&RuntimeParameters::spatialJoinPrefilterMaxSize_>(),
      "The maximum size in square coordinates of the aggregated bounding box "
      "of the smaller join partner in a spatial join, such that prefiltering "
      "will be employed. To disable prefiltering for non-point geometries, set "
      "this option to 0.");
  add("materialized-view-writer-memory",
      optionFactory.getProgramOption<
          &RuntimeParameters::materializedViewWriterMemory_>(),
      "Memory limit for sorting rows during the writing of materialized "
      "views.");
  add("preload-materialized-views,l",
      po::value<std::vector<std::string>>(&config.preloadMaterializedViews_)
          ->multitoken(),
      "The names of materialized views to be loaded automatically on server "
      "start (this option takes an arbitrary number of arguments).");
  add("enable-materialized-view-query-rewrite",
      optionFactory.getProgramOption<
          &RuntimeParameters::enableMaterializedViewQueryRewrite_>(),
      "If set to true, loaded materialized views will be considered as "
      "alternative query plans for certain supported query patterns.");
  add("service-allowed-iri-prefixes",
      optionFactory
          .getProgramOption<&RuntimeParameters::serviceAllowedIriPrefixes_>()
          ->multitoken(),
      "IRI prefixes that are allowed as SERVICE endpoints (this option takes "
      "an arbitrary number of arguments). If none are given (the default), all "
      "IRIs are allowed. If given, SERVICE requests to IRIs not matching any "
      "prefix are rejected. To disable all federated queries, set this option "
      "to an invalid IRI prefix like `-`. Magic services (for example spatial "
      "search or materialized views) are never affected.");
  add("log-level",
      optionFactory.getProgramOption<&RuntimeParameters::logLevel_>(),
      "Runtime log level: FATAL, ERROR, WARN, INFO, DEBUG, TIMING, or TRACE. "
      "Default is INFO. The compile-time level (CMake -DLOGLEVEL=...) applies "
      "as an upper bound — messages above it are never emitted regardless of "
      "this setting.");
  // wf:call configuration loaders. All optional; the runtime boots with
  // empty registries when nothing is supplied. On any load failure the
  // server exits non-zero rather than serving with a half-configured
  // rewrite state — a silently misconfigured shape/alias/conversion
  // registry would produce mis-routed queries at runtime that would be
  // far harder to diagnose than a startup-time abort.
  add("wf-alias-db", po::value<std::string>(&wfAliasDb)->default_value(""),
      "Path to a SQLite database containing the wf:call alias map. If set, "
      "the runtime loads the `aliases` table (or the table named by "
      "--wf-alias-table) at boot so alias→canonical rewrites take effect.");
  add("wf-alias-table",
      po::value<std::string>(&wfAliasTable)->default_value("aliases"),
      "Table name inside --wf-alias-db (default: `aliases`).");
  add("wf-shape-db", po::value<std::string>(&wfShapeDb)->default_value(""),
      "Path to a SQLite database containing the wf:call shape registry. If "
      "set, the runtime loads the `shapes` table (or the table named by "
      "--wf-shape-table) at boot so BGP→wf_fetch shape rewrites take effect. "
      "Requires --wf-fetch-url to have effect.");
  add("wf-shape-table",
      po::value<std::string>(&wfShapeTable)->default_value("shapes"),
      "Table name inside --wf-shape-db (default: `shapes`).");
  add("wf-conversion-rules",
      po::value<std::string>(&wfConversionRules)->default_value(""),
      "Path to a JSON file of conversion rules. Format: "
      "`{\"rules\":[{\"source_predicate\":..,\"target_predicate\":..,"
      "\"expression\":..}, ...]}`. Populates the runtime's conversion "
      "registry at boot so `GRAPH <urn:wf:conversion:*>` rewrites take "
      "effect.");
  add("wf-fetch-url", po::value<std::string>(&wfFetchUrl)->default_value(""),
      "URL of the wf_fetch wasm module used by the shape rewrite pass. "
      "Empty (the default) disables shape rewriting even when "
      "--wf-shape-db is populated.");
  add("construct-deduplication",
      optionFactory
          .getProgramOption<&RuntimeParameters::constructDeduplication_>(),
      R"("Controls deduplication of triples in CONSTRUCT query results. "
      "\"none\" (default): no deduplication, every triple is emitted. "
      "\"global\": a triple is emitted at most once across the entire result. "
      "\"batchwise:N\" (positive integer N): deduplicate against the N most "
      "recently seen unique triples per template triple (bounded memory, "
      "partial deduplication).")");
  po::variables_map optionsMap;

  try {
    po::store(po::parse_command_line(argc, argv, options), optionsMap);
    if (optionsMap.count("help")) {
      std::cout << options << '\n';
      return EXIT_SUCCESS;
    }
    if (optionsMap.count("version")) {
      std::cout << argv[0] << " " << qlever::version::ProjectVersion << '\n';
      return EXIT_SUCCESS;
    }
    po::notify(optionsMap);
  } catch (const std::exception& e) {
    std::cerr << "Error in command-line argument: " << e.what() << '\n';
    std::cerr << options << '\n';
    return EXIT_FAILURE;
  }

  AD_LOG_INFO << EMPH_ON << "QLever server " << qlever::version::ProjectVersion
              << ", compiled on " << qlever::version::DatetimeOfCompilation
              << " using git hash " << qlever::version::GitShortHash << EMPH_OFF
              << std::endl;

  // Push wf:call configuration into the runtime BEFORE the server accepts
  // its first request. Any load failure is fatal — a half-configured
  // rewrite state would silently produce wrong query plans.
  try {
    auto& wfRuntime = sparqlExpression::wf::WfRuntime::instance();
    if (!wfAliasDb.empty()) {
      wfRuntime.loadAliasMapFromSqlite(wfAliasDb, wfAliasTable);
      AD_LOG_INFO << "wf: loaded alias map from " << wfAliasDb << " (table `"
                  << wfAliasTable << "`)" << std::endl;
    }
    if (!wfShapeDb.empty()) {
      wfRuntime.loadShapeRegistryFromSqlite(wfShapeDb, wfShapeTable);
      AD_LOG_INFO << "wf: loaded shape registry from " << wfShapeDb
                  << " (table `" << wfShapeTable << "`)" << std::endl;
    }
    if (!wfConversionRules.empty()) {
      wfRuntime.loadConversionRegistryFromJson(wfConversionRules);
      AD_LOG_INFO << "wf: loaded conversion rules from " << wfConversionRules
                  << std::endl;
    }
    if (!wfFetchUrl.empty()) {
      wfRuntime.setWfFetchUrl(wfFetchUrl);
      AD_LOG_INFO << "wf: shape rewrite will dispatch to " << wfFetchUrl
                  << std::endl;
    }
  } catch (const std::exception& e) {
    AD_LOG_ERROR << "wf: configuration failed: " << e.what() << std::endl;
    return 1;
  }

  try {
    Server server(port, numSimultaneousQueries, std::move(accessToken), config,
                  noAccessCheck);
    // Per-query jsonl metrics log, written next to the index files. On by
    // default; `--no-metrics-log` opts out.
    if (!noMetricsLog) {
      server.configureQueryEventLog(config.baseName_ + ".metrics-log.jsonl");
    }
    server.run();
  } catch (const std::exception& e) {
    // Reached if opening the metrics log fails; server.run() otherwise handles
    // its own exceptions.
    AD_LOG_ERROR << e.what() << std::endl;
    return 1;
  }
  // This should also never be reached as the server threads are not supposed
  // to terminate.
  return 2;
}
