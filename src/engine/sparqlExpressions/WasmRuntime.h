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
  // Populate the runtime's fulltext-index registry from a JSON file.
  // Expected shape mirrors oxigraph-wf's fulltext_registry: top-level
  // `{ "indexes": [...] }` with per-entry name/mode/backend_url/opts.
  // Consumed by the (not-yet-ported) filter-fold rewrite pass; safe to
  // leave unset when no fulltext parity cases are configured.
  void loadFulltextRegistryFromJson(std::string_view jsonPath);
  // Populate the runtime's document-store registry from a JSON file.
  // Expected shape mirrors the sibling oxigraph-wf document_registry:
  // top-level `{ "documents": [...] }` with per-entry
  // name/mode/guest_url/search_backend/storage_backend and the managed-
  // mode sweep_interval_secs/revision_retention pair. Consumed by
  // explicit `SERVICE ?svc` dispatch (no filter-fold — wf_document
  // reuses wf_fulltext's, see wf-document.md §11) and the periodic
  // Manticore mirror sweep. Safe to leave unset when no wf_document
  // parity cases are configured. v0.2 companion to
  // `loadFulltextRegistryFromJson`.
  void loadDocumentRegistryFromJson(std::string_view jsonPath);
  // Populate the runtime's federation-source registry from a JSON file.
  // Expected shape mirrors the sibling Rust
  // `federation_registry::FederationRegistry`: top-level
  // `{ "sources": [...] }` with per-entry name / type
  // (sparql|wf-search|wf-fetch|wf-document|http-sparql) / endpoint /
  // optional predicates / optional probe_ttl_secs. Consumed by the
  // `wf_federation_rewrite` pass (v0.1: static source selection,
  // filter pushdown, uniform-cost lexicographic reorder over BGPs).
  // Safe to leave unset when no federation parity cases are
  // configured. See `wf-conformance/docs/design/wf-federation.md`
  // §03, §04.
  void loadFederationRegistryFromJson(std::string_view jsonPath);
  void setWfFetchUrl(std::string_view url);

  // Store the wf_canonicalize.wasm URL + SQLite sink URL used by the
  // /admin/canonicalize-sweep endpoint. Both may be empty (in which case
  // hitting the endpoint returns a 400 that names the missing
  // configuration). Populated once at server boot from ServerMain's
  // --wf-canonicalize-wasm-url / --wf-alias-db + --wf-alias-table.
  void setCanonicalizeConfig(std::string_view wasmUrl,
                             std::string_view sinkUrl);
  // Getters used by Server::process's /admin/canonicalize-sweep handler to
  // decide whether the endpoint has been configured before it invokes the
  // wasm (an unconfigured call would otherwise reach the Rust runtime and
  // fail there with a less helpful message).
  const std::string& canonicalizeWasmUrl() const;
  const std::string& canonicalizeSinkUrl() const;

  // Invoke wf_canonicalize.wasm against the current fulltext + document
  // registries. Mirrors oxigraph-wf's /admin/canonicalize-sweep behaviour
  // — the sweep endpoint on both engines hands the guest the same config
  // JSON. Returns the guest's raw JSON reply (a single-row binding-set
  // with count fields the caller can wrap into an HTTP response body).
  // Throws std::runtime_error on any wasm-side or transport failure.
  std::string runCanonicalizeSweep(std::string_view wasmUrl,
                                   std::string_view sinkUrl,
                                   bool fullScan);

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
