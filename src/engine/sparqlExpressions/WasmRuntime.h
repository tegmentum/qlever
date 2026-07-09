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

class WfRuntime {
 public:
  static WfRuntime& instance();

  // Invoke `evaluate` on the module at `wasmUrl`, passing `jsonPayload`
  // (UTF-8, will be NUL-terminated on the guest side) as the input.
  // Returns the guest's NUL-terminated reply verbatim.
  //
  // Throws std::runtime_error on any transport or wasm-side failure.
  std::string callEvaluate(std::string_view wasmUrl,
                           std::string_view jsonPayload);

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
