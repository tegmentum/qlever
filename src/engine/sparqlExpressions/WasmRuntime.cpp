// Copyright 2026 tegmentum
//
// wf:call host runtime — wasmtime C API glue.
//
// The entire translation unit is guarded on QLEVER_ENABLE_WF. When the
// build option is off, this file compiles down to an empty singleton
// whose `callEvaluate` throws — the expression code should never reach
// it in that configuration because WfCallExpression itself short-circuits.

#include "engine/sparqlExpressions/WasmRuntime.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifdef QLEVER_ENABLE_WF
// The wasmtime v27 C API tarball ships wasm.h + wasmtime.h at the include
// root, with per-subsystem headers under `include/wasmtime/*.h`. Include
// the flat headers — the umbrella `wasmtime.h` pulls the rest in.
#include <wasm.h>
#include <wasmtime.h>

#include <cerrno>
#include <cstdio>
#include <fstream>
#include <sstream>

#include <curl/curl.h>
#endif

namespace sparqlExpression::wf {

#ifdef QLEVER_ENABLE_WF

namespace {

// Small helper: format a wasmtime_error into a std::runtime_error message
// and free the error handle.
[[noreturn]] void throwWasmError(const char* what, wasmtime_error_t* err) {
  wasm_byte_vec_t msg;
  wasm_byte_vec_new_empty(&msg);
  wasmtime_error_message(err, &msg);
  std::string body(msg.data ? msg.data : "(no message)",
                   msg.data ? msg.size : 0);
  wasm_byte_vec_delete(&msg);
  wasmtime_error_delete(err);
  throw std::runtime_error(std::string{what} + ": " + body);
}

// Same treatment for trap handles.
[[noreturn]] void throwWasmTrap(const char* what, wasm_trap_t* trap) {
  wasm_byte_vec_t msg;
  wasm_byte_vec_new_empty(&msg);
  wasm_trap_message(trap, &msg);
  std::string body(msg.data ? msg.data : "(no message)",
                   msg.data ? msg.size : 0);
  wasm_byte_vec_delete(&msg);
  wasm_trap_delete(trap);
  throw std::runtime_error(std::string{what} + ": trap: " + body);
}

// libcurl WRITEFUNCTION appending to a std::string.
size_t curlAppend(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* out = static_cast<std::string*>(userdata);
  out->append(ptr, size * nmemb);
  return size * nmemb;
}

// Fetch the raw wasm bytes for a URL. Supports file://, http://, https://.
// A bare path is treated as a filesystem path for convenience.
std::vector<uint8_t> fetchBytes(std::string_view url) {
  std::string u(url);

  auto readFile = [&](const std::string& path) -> std::vector<uint8_t> {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
      throw std::runtime_error("wf:call: cannot open " + path);
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    const auto& s = ss.str();
    return std::vector<uint8_t>(s.begin(), s.end());
  };

  if (u.rfind("file://", 0) == 0) {
    return readFile(u.substr(7));
  }
  if (u.rfind("http://", 0) == 0 || u.rfind("https://", 0) == 0) {
    CURL* curl = curl_easy_init();
    if (!curl) {
      throw std::runtime_error("wf:call: curl init failed");
    }
    std::string body;
    curl_easy_setopt(curl, CURLOPT_URL, u.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curlAppend);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    CURLcode rc = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
      throw std::runtime_error(std::string{"wf:call: curl error: "} +
                               curl_easy_strerror(rc));
    }
    if (http >= 400) {
      throw std::runtime_error("wf:call: HTTP " + std::to_string(http) +
                               " fetching " + u);
    }
    return std::vector<uint8_t>(body.begin(), body.end());
  }
  // Bare path or unknown scheme — try as a filesystem path.
  return readFile(u);
}

}  // namespace

struct WfRuntime::Impl {
  wasm_engine_t* engine_ = nullptr;
  std::mutex cacheMu_;
  // Compiled modules (URL -> module handle). wasmtime modules are documented
  // as thread-safe once compiled.
  std::unordered_map<std::string, wasmtime_module_t*> moduleCache_;
  // Retain the source bytes for eviction diagnostics and so downstream
  // callers can reuse them without a second fetch.
  std::unordered_map<std::string, std::vector<uint8_t>> bytesCache_;

  Impl() {
    wasm_config_t* cfg = wasm_config_new();
    // Fuel is disabled by default; leaving it off for v0.1 to keep the
    // stage-1 stub trivial. The knob is intentionally wired here so
    // stage 2 can flip it on with a one-line change.
    wasmtime_config_consume_fuel_set(cfg, false);
    wasmtime_config_max_wasm_stack_set(cfg, 512 * 1024);
    engine_ = wasm_engine_new_with_config(cfg);
    if (!engine_) {
      throw std::runtime_error("wf:call: wasm_engine_new_with_config failed");
    }
  }

  ~Impl() {
    for (auto& [url, mod] : moduleCache_) {
      if (mod) wasmtime_module_delete(mod);
    }
    if (engine_) wasm_engine_delete(engine_);
  }

  wasmtime_module_t* moduleFor(const std::string& url) {
    std::lock_guard<std::mutex> lk(cacheMu_);
    if (auto it = moduleCache_.find(url); it != moduleCache_.end()) {
      return it->second;
    }
    auto bytes = fetchBytes(url);
    wasmtime_module_t* mod = nullptr;
    wasmtime_error_t* err =
        wasmtime_module_new(engine_, bytes.data(), bytes.size(), &mod);
    if (err) throwWasmError("wf:call: wasmtime_module_new", err);
    moduleCache_[url] = mod;
    bytesCache_[url] = std::move(bytes);
    return mod;
  }

  std::string invoke(const std::string& url, std::string_view payload) {
    wasmtime_module_t* mod = moduleFor(url);

    // Stores are not thread-safe; fresh one per call.
    wasmtime_store_t* store = wasmtime_store_new(engine_, nullptr, nullptr);
    if (!store) {
      throw std::runtime_error("wf:call: wasmtime_store_new failed");
    }
    struct StoreGuard {
      wasmtime_store_t* s;
      ~StoreGuard() {
        if (s) wasmtime_store_delete(s);
      }
    } storeGuard{store};

    wasmtime_context_t* ctx = wasmtime_store_context(store);

    wasmtime_instance_t inst;
    wasm_trap_t* trap = nullptr;
    wasmtime_error_t* err =
        wasmtime_instance_new(ctx, mod, nullptr, 0, &inst, &trap);
    if (err) throwWasmError("wf:call: wasmtime_instance_new", err);
    if (trap) throwWasmTrap("wf:call: wasmtime_instance_new", trap);

    auto getExport = [&](const char* name, wasmtime_extern_t* out) {
      if (!wasmtime_instance_export_get(ctx, &inst, name, std::strlen(name),
                                        out)) {
        throw std::runtime_error(std::string{"wf:call: missing export '"} +
                                 name + "'");
      }
    };

    wasmtime_extern_t mallocExt, freeExt, evalExt, memExt;
    getExport("malloc", &mallocExt);
    getExport("free", &freeExt);
    getExport("evaluate", &evalExt);
    getExport("memory", &memExt);

    if (mallocExt.kind != WASMTIME_EXTERN_FUNC ||
        freeExt.kind != WASMTIME_EXTERN_FUNC ||
        evalExt.kind != WASMTIME_EXTERN_FUNC ||
        memExt.kind != WASMTIME_EXTERN_MEMORY) {
      throw std::runtime_error(
          "wf:call: malloc/free/evaluate/memory export kinds mismatch");
    }

    auto callI32 = [&](wasmtime_func_t& fn, std::vector<int32_t> ins,
                       bool wantRet) -> int32_t {
      std::vector<wasmtime_val_t> args(ins.size());
      for (size_t i = 0; i < ins.size(); ++i) {
        args[i].kind = WASMTIME_I32;
        args[i].of.i32 = ins[i];
      }
      wasmtime_val_t ret;
      wasm_trap_t* trap2 = nullptr;
      wasmtime_error_t* err2 = wasmtime_func_call(
          ctx, &fn, args.data(), args.size(), wantRet ? &ret : nullptr,
          wantRet ? 1 : 0, &trap2);
      if (err2) throwWasmError("wf:call: wasmtime_func_call", err2);
      if (trap2) throwWasmTrap("wf:call: wasmtime_func_call", trap2);
      if (wantRet) {
        if (ret.kind != WASMTIME_I32) {
          throw std::runtime_error(
              "wf:call: unexpected return kind (expected i32)");
        }
        return ret.of.i32;
      }
      return 0;
    };

    // Allocate len+1 bytes, copy payload, terminate with NUL.
    const int32_t payloadLen = static_cast<int32_t>(payload.size());
    const int32_t inLen = payloadLen + 1;
    int32_t inPtr = callI32(mallocExt.of.func, {inLen}, /*wantRet=*/true);

    uint8_t* mem = wasmtime_memory_data(ctx, &memExt.of.memory);
    size_t memSize = wasmtime_memory_data_size(ctx, &memExt.of.memory);
    if (inPtr < 0 || static_cast<size_t>(inPtr) + inLen > memSize) {
      throw std::runtime_error("wf:call: malloc returned out-of-bounds ptr");
    }
    std::memcpy(mem + inPtr, payload.data(), payloadLen);
    mem[inPtr + payloadLen] = 0;

    int32_t outPtr = callI32(evalExt.of.func, {inPtr}, /*wantRet=*/true);

    // Refresh — memory may have grown during evaluate().
    mem = wasmtime_memory_data(ctx, &memExt.of.memory);
    memSize = wasmtime_memory_data_size(ctx, &memExt.of.memory);
    if (outPtr < 0 || static_cast<size_t>(outPtr) >= memSize) {
      throw std::runtime_error("wf:call: evaluate returned out-of-bounds ptr");
    }
    // Guest owns the output buffer; we read until NUL.
    const uint8_t* start = mem + outPtr;
    const uint8_t* end = start;
    const uint8_t* memEnd = mem + memSize;
    while (end < memEnd && *end != 0) ++end;
    std::string result(reinterpret_cast<const char*>(start),
                       static_cast<size_t>(end - start));

    // Best-effort free of the input buffer. We deliberately do NOT free
    // the output — the guest owns its lifetime.
    (void)callI32(freeExt.of.func, {inPtr, inLen}, /*wantRet=*/false);

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
  return impl_->invoke(std::string(wasmUrl), jsonPayload);
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
