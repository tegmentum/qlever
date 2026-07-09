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
//
// Host callbacks (v0.3): we now construct the underlying runtime with a
// WfCallbacks table so a component-model guest can invoke
// stardog:webfunction/host@0.3.2's imports. `callback-depth` is wired to a
// thread-local counter maintained here — it survives across re-entrancy
// and, crucially, is per-thread so parallel query workers don't step on
// each other. `execute-query` is intentionally left NULL for now: safely
// re-entering QLever's query engine from inside a running query needs
// deeper plumbing than a shim can honestly provide (task-group state,
// index-scan iterators, and evaluator context all need to be threaded
// through). Wiring the ABI slot but leaving the callback NULL lets guests
// that never invoke execute-query run cleanly today, and lets a follow-up
// change flip the switch without touching the ABI.

#include "engine/sparqlExpressions/WasmRuntime.h"

#include <stdexcept>
#include <string>

#ifdef QLEVER_ENABLE_WF
#include <qlever_wf_runtime.h>
#endif

namespace sparqlExpression::wf {

#ifdef QLEVER_ENABLE_WF

namespace {

// Per-thread callback-depth counter. Multi-threaded query workers each
// see their own counter, matching the Jena implementation's semantics.
// Bumped/decremented around any host re-entrancy the runtime may drive.
thread_local int gCallbackDepth = 0;

extern "C" int wfCallbackDepth(void* /*user_data*/) { return gCallbackDepth; }

}  // namespace

struct WfRuntime::Impl {
  // The Rust crate hides all synchronization behind this opaque handle;
  // concurrent wf_runtime_invoke calls are safe.
  ::WfRuntime* handle_ = nullptr;

  Impl() {
    // Populate a callback table before constructing the runtime. We only
    // wire callback_depth today; execute_query stays NULL so the guest
    // sees a clean err<string> "not wired by embedder" if it reaches for
    // it. See the file-level comment for the rationale.
    ::WfCallbacks callbacks{};
    callbacks.user_data = nullptr;
    callbacks.execute_query = nullptr;
    callbacks.callback_depth = &wfCallbackDepth;
    callbacks.reserved_prepare_query = nullptr;
    callbacks.reserved_run_prepared = nullptr;

    handle_ = ::wf_runtime_new_with_callbacks(&callbacks);
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
