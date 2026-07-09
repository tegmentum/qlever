// Copyright 2026 tegmentum
//
// wf:call host runtime — thin FFI shim over the qlever-wf-runtime Rust crate.
//
// The original implementation drove the wasmtime C API directly. That worked
// for core modules but the component-model surface in the C API is
// dramatically more verbose than the Rust API, so the component-model
// support (needed for wf_tree.wasm / adjacency_tree.wasm) lives in a Rust
// crate we link as a static library. See ~/git/qlever-wf-runtime/ for the
// crate; qlever_wf_runtime.h for the C ABI.
//
// This translation unit is guarded on QLEVER_ENABLE_WF: when off, the
// runtime falls back to a throwing stub so the option remains flippable
// without touching call sites.

#include "engine/sparqlExpressions/WasmRuntime.h"

#include <stdexcept>
#include <string>

#ifdef QLEVER_ENABLE_WF
#include <qlever_wf_runtime.h>
#endif

namespace sparqlExpression::wf {

#ifdef QLEVER_ENABLE_WF

struct WfRuntime::Impl {
  // The Rust crate hides all synchronization behind this opaque handle;
  // concurrent wf_runtime_invoke calls are safe.
  ::WfRuntime* handle_ = nullptr;

  Impl() {
    handle_ = ::wf_runtime_new();
    if (!handle_) {
      throw std::runtime_error("wf:call: wf_runtime_new returned NULL (OOM?)");
    }
  }

  ~Impl() { ::wf_runtime_free(handle_); }

  std::string invoke(const std::string& url, const std::string& args_json) {
    char* err = nullptr;
    const char* out =
        ::wf_runtime_invoke(handle_, url.c_str(), args_json.c_str(), &err);
    if (!out) {
      // Contract: on failure `out` is NULL and `err` is a heap string we own.
      std::string msg =
          err ? std::string("wf:call: ") + err : std::string("wf:call: unknown error");
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
    std::string result{out};
    ::wf_runtime_free_string(out);
    return result;
  }
};

WfRuntime::WfRuntime() : impl_(new Impl()) {}
WfRuntime::~WfRuntime() { delete impl_; }

WfRuntime& WfRuntime::instance() {
  static WfRuntime r;
  return r;
}

std::string WfRuntime::callEvaluate(std::string_view wasmUrl,
                                    std::string_view jsonPayload) {
  // The Rust runtime accepts args as a JSON array; Phase 1 passes []
  // — argument marshalling from QLever's Value types is Phase 2.
  const std::string args_json = jsonPayload.empty()
                                    ? std::string{"[]"}
                                    : std::string{jsonPayload};
  return impl_->invoke(std::string(wasmUrl), args_json);
}

#else  // !QLEVER_ENABLE_WF

struct WfRuntime::Impl {};
WfRuntime::WfRuntime() : impl_(nullptr) {}
WfRuntime::~WfRuntime() = default;

WfRuntime& WfRuntime::instance() {
  static WfRuntime r;
  return r;
}

std::string WfRuntime::callEvaluate(std::string_view, std::string_view) {
  throw std::runtime_error(
      "wf:call: this QLever build was compiled without QLEVER_ENABLE_WF; "
      "rebuild with -DQLEVER_ENABLE_WF=ON to enable wasm execution.");
}

#endif

}  // namespace sparqlExpression::wf
