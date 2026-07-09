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

# tegmentum WebAssembly filter function. v0.1 lands the parser wire-up
# and dispatch stub; the wasmtime invocation is not yet linked (option
# retained so a follow-up can turn it on without touching this file).
option(QLEVER_ENABLE_WF "Enable tegmentum wf:call wasm invocation" OFF)
if (QLEVER_ENABLE_WF)
    find_library(WASMTIME_LIB wasmtime REQUIRED)
    find_library(CURL_LIB curl REQUIRED)
    target_compile_definitions(sparqlExpressions PRIVATE QLEVER_ENABLE_WF=1)
    target_link_libraries(sparqlExpressions ${WASMTIME_LIB} ${CURL_LIB})
    message(STATUS "wf:call wasm exec enabled — linking ${WASMTIME_LIB} and ${CURL_LIB}")
endif()

if (NOT REDUCED_FEATURE_SET_FOR_CPP17)
    target_link_libraries(sparqlExpressions Boost::url)
endif()
