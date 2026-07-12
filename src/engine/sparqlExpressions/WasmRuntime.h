// Copyright 2026 tegmentum
//
// wf:call WebAssembly host runtime for QLever.
//
// Owns a process-wide wasmtime engine and a URL-keyed cache of compiled
// modules. Instantiated once per QLever process; stores are cheap enough
// to spin up per invocation (and wasmtime stores are documented as not
// thread-safe, so we do not share them).
//
// The public surface is intentionally narrow: fetch a wasm module by URL,
// compile it (cached), instantiate against a fresh store, marshal a
// JSON payload through malloc/free/evaluate, return the JSON reply.

#ifndef QLEVER_SRC_ENGINE_SPARQLEXPRESSIONS_WASMRUNTIME_H_
#define QLEVER_SRC_ENGINE_SPARQLEXPRESSIONS_WASMRUNTIME_H_

#include <string>
#include <string_view>

namespace sparqlExpression::wf {

// Result of a rewrite pass. `aliasesJson` may be empty when the runtime
// has no active alias registrations; callers must treat empty as "no
// aliases" rather than an error.
struct RewriteResult {
  std::string rewritten;
  std::string aliasesJson;
};

class WfRuntime {
 public:
  static WfRuntime& instance();

  // Compile-time knob: `true` iff QLEVER_ENABLE_WF was defined at build
  // time. Callers that must degrade gracefully (the SparqlParser preprocess
  // hook, the SERVICE dispatch's `wf-invoke:` branch) key off this so a
  // build without the Rust runtime linked in still parses queries
  // untouched.
  static constexpr bool isEnabled() {
#ifdef QLEVER_ENABLE_WF
    return true;
#else
    return false;
#endif
  }

  // Invoke `evaluate` on the module at `wasmUrl`, passing `jsonPayload`
  // (UTF-8, will be NUL-terminated on the guest side) as the input.
  // Returns the guest's NUL-terminated reply verbatim.
  //
  // Throws std::runtime_error on any transport or wasm-side failure.
  std::string callEvaluate(std::string_view wasmUrl,
                           std::string_view jsonPayload);

  // Run the runtime's rewrite passes over the raw SPARQL text. Returns
  // the rewritten SPARQL and (optionally) a JSON `{canonical: alias, ...}`
  // reverse map for output-path re-aliasing. Throws on runtime failure.
  //
  // When QLEVER_ENABLE_WF is off this returns `{query, ""}` unchanged.
  RewriteResult rewriteQuery(std::string_view query);

  // Invoke a `wf:partial(...)` that the rewrite pass previously folded
  // into a `wf-invoke:<id>` SERVICE IRI. `idHex` is the hex substring
  // after `wf-invoke:`. Returns the guest's binding-sets JSON. Throws
  // on runtime failure.
  //
  // Called from Service::computeResultImpl when the resolved SERVICE
  // IRI uses the `wf-invoke:` scheme.
  std::string invokeById(std::string_view idHex);

  // Configuration loaders. Each populates one of the four rewrite-state
  // registries so the corresponding rewrite pass stops being a cheap
  // identity. Intended to be called once during server boot before
  // `Server::run()` starts serving queries.
  //
  // Each throws std::runtime_error on any failure — a bad db path,
  // malformed JSON, or a build compiled without QLEVER_ENABLE_WF. The
  // server main is expected to catch and exit non-zero rather than boot
  // with a half-configured runtime.
  void loadAliasMapFromSqlite(std::string_view dbPath,
                              std::string_view table);
  void loadShapeRegistryFromSqlite(std::string_view dbPath,
                                   std::string_view table);
  void loadConversionRegistryFromJson(std::string_view jsonPath);
  void setWfFetchUrl(std::string_view url);

  WfRuntime(const WfRuntime&) = delete;
  WfRuntime& operator=(const WfRuntime&) = delete;

 private:
  WfRuntime();
  ~WfRuntime();
  struct Impl;
  Impl* impl_;
};

}  // namespace sparqlExpression::wf

#endif  // QLEVER_SRC_ENGINE_SPARQLEXPRESSIONS_WASMRUNTIME_H_
