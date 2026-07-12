add_library(sparqlExpressions
        SparqlExpressionValueGetters.cpp
        SparqlExpressionGenerators.cpp
        SetOfIntervals.cpp
        SparqlExpressionPimpl.cpp
        SampleExpression.cpp
        RelationalExpressions.cpp
        AggregateExpression.cpp
        StdevExpression.cpp
        RegexExpression.cpp
        NumericUnaryExpressions.cpp
        NumericBinaryExpressions.cpp
        DateExpressions.cpp
        StringExpressions.cpp
        IsSomethingExpressions.cpp
        ConditionalExpressions.cpp
        SparqlExpressionTypes.cpp
        SparqlExpression.cpp
        ConvertToDtypeConstructor.cpp
        RdfTermExpressions.cpp
        LangExpression.cpp
        CountStarExpression.cpp
        PrefilterExpressionIndex.cpp
        GeoExpression.cpp
        BlankNodeExpression.cpp
        GroupConcatExpression.cpp
        WfCallExpression.cpp
        WasmRuntime.cpp)

qlever_target_link_libraries(sparqlExpressions util index)

# tegmentum WebAssembly filter function. The wasm runtime lives in a Rust
# crate (qlever-wf-runtime) that we link as a static archive; that crate
# hosts the component-model support (wasmtime's C API component-model
# surface is impractically verbose, so we route through the Rust API).
#
# Expected inputs (set by the Dockerfile.wf builder stage):
#   QLEVER_WF_RUNTIME_INCLUDE_DIR — dir containing qlever_wf_runtime.h
#   QLEVER_WF_RUNTIME_LIB        — path to libqlever_wf_runtime.a
option(QLEVER_ENABLE_WF "Enable tegmentum wf:call wasm invocation" OFF)
if (QLEVER_ENABLE_WF)
    if (NOT QLEVER_WF_RUNTIME_INCLUDE_DIR OR NOT QLEVER_WF_RUNTIME_LIB)
        message(FATAL_ERROR
            "QLEVER_ENABLE_WF=ON requires QLEVER_WF_RUNTIME_INCLUDE_DIR and "
            "QLEVER_WF_RUNTIME_LIB to be set (path to header dir and static "
            "archive of the qlever-wf-runtime crate).")
    endif()
    # PUBLIC: the definition also propagates to every downstream target that
    # links sparqlExpressions (parser, engine, qlever-server, ...). Necessary
    # because WfRuntime::isEnabled() is a constexpr inline in the header —
    # its value is baked into every translation unit that includes the
    # header, and the SparqlParser rewrite hook keys off it.
    target_compile_definitions(sparqlExpressions PUBLIC QLEVER_ENABLE_WF=1)
    target_include_directories(sparqlExpressions PRIVATE
        ${QLEVER_WF_RUNTIME_INCLUDE_DIR})
    # Static archive: wasmtime + cranelift + rustls are compiled in
    # transitively. The Rust std runtime references dl/pthread/m and the
    # rustls/ring crates want libgcc symbols; on Linux we surface those
    # explicitly so the final qlever-server link succeeds. On macOS the
    # equivalents come from the SDK, so the guard is Linux-only.
    target_link_libraries(sparqlExpressions ${QLEVER_WF_RUNTIME_LIB})
    if (CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(sparqlExpressions dl pthread m)
    endif()
    message(STATUS "wf:call wasm exec enabled — linking ${QLEVER_WF_RUNTIME_LIB}")
endif()

if (NOT REDUCED_FEATURE_SET_FOR_CPP17)
    target_link_libraries(sparqlExpressions Boost::url)
endif()
