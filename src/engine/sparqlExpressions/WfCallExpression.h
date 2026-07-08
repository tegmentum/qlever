// Copyright 2026 tegmentum
//
// Licensed under the same terms as QLever.
//
// wf:call SPARQL filter function — invokes a WebAssembly module fetched
// from a URL and returns its result as a plain literal.
//
// Signature:
//   wf:call(<wasm-url>, arg1, arg2, ...) -> literal
//
// The wasm module is expected to export the tegmentum module-mode ABI:
//   * `malloc(size: i32) -> i32`
//   * `free(ptr: i32, size: i32)`
//   * `evaluate(input_ptr: i32) -> i32`  — reads a UTF-8 SPARQL-JSON
//     document at `input_ptr` (null-terminated), returns a pointer to a
//     null-terminated SPARQL-JSON document holding the result. First row's
//     `value_0` binding becomes the return literal.
//
// Component Model wasm is NOT supported by this initial QLever build;
// components need host callback plumbing which lives in the JVM plugins.

#ifndef QLEVER_SRC_ENGINE_SPARQLEXPRESSIONS_WFCALLEXPRESSION_H_
#define QLEVER_SRC_ENGINE_SPARQLEXPRESSIONS_WFCALLEXPRESSION_H_

#include <memory>
#include <vector>

#include "engine/sparqlExpressions/SparqlExpression.h"

namespace sparqlExpression {

// Free-function factory used by SparqlQleverVisitor. Takes ownership of the
// argument expressions.
SparqlExpression::Ptr makeWfCallExpression(std::vector<SparqlExpression::Ptr> args);

}  // namespace sparqlExpression

#endif  // QLEVER_SRC_ENGINE_SPARQLEXPRESSIONS_WFCALLEXPRESSION_H_
