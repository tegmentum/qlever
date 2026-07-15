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
// Host callbacks (v0.5): `execute-query` is now wired up via HTTP loopback
// to the same qlever-server that hosts the outer query. Rationale: deep
// in-process re-entry into QLever's query engine from inside a running
// query is hard — the outer query owns the EvaluationContext, per-worker
// task groups, and index-scan iterators, and none of those are thread-safe
// to share with a fresh nested query. HTTP loopback sidesteps that by
// letting qlever-server treat the nested query as an ordinary external
// SPARQL client: a fresh scheduling slot, a fresh evaluator context, and
// no attempt to share mid-flight state. The cost is one localhost socket
// per callback (~fractional ms round-trip on loopback), plus the hard
// requirement that qlever-server run with `--num-simultaneous-queries`
// >= 2. With -j 1 the callback thread would wait forever, because the
// nested SPARQL request would sit in the queue behind the outer query
// that is currently blocked inside the callback. `callback-depth` remains
// wired to a per-thread counter — bumped/decremented around each host
// re-entry so a guest that introspects mid-recursion sees a truthful
// depth value.
//
// Host callbacks (v0.7 add): `execute-update` uses the same HTTP-loopback
// dispatch as `execute-query` — the update text is POSTed to qlever-
// server with `Content-Type: application/sparql-update` (the same wire
// shape a Web-facing SPARQL 1.1 UPDATE client sends). This lets guests
// that mutate the store (wf_pipeline steps of kind sparql_update;
// wf_infer's CLEAR + INSERT DATA loop) run without in-process re-entry
// into the query engine's transaction machinery. QLever's own /update
// handler applies changes to DeltaTriples atomically, so subsequent
// execute-query callbacks in the same wasm invocation observe the write
// through the standard permutation-scan path. QLever guards update with
// an access-token check — the loopback caller must supply the same token
// qlever-server was started with, sourced from QLEVER_LOOPBACK_ACCESS_TOKEN
// (see below).
//
// Configuration:
//   QLEVER_LOOPBACK_PORT — the TCP port qlever-server listens on. If unset
//                          at callback time, `execute-query` and
//                          `execute-update` both return err<string>,
//                          matching pre-v0.5 behaviour. This keeps guests
//                          that never call the host callbacks
//                          (`to_upper`, `debug_callback_depth`, ...)
//                          working without any deployment change.
//
//   QLEVER_LOOPBACK_ACCESS_TOKEN — access token forwarded as the
//                          `?access-token=…` query parameter on
//                          `execute-update` roundtrips. Must match
//                          whatever qlever-server was started with via
//                          `--access-token`. Optional if the server was
//                          started with `--no-access-check`; required
//                          otherwise (Server.cpp visitUpdate calls
//                          requireValidAccessToken()). Not used on the
//                          read-only `execute-query` path.
//
// prepare-query / run-prepared / follow-predicate are still stubbed in
// the Rust runtime; guests that reach for those (wf_tree, wf_tree_rows,
// adjacency_tree) will surface a clean err<string>. Wiring those is a
// follow-up on the runtime side.

#include "engine/sparqlExpressions/WasmRuntime.h"

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#ifdef QLEVER_ENABLE_WF
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <sstream>
#include <vector>

#include <nlohmann/json.hpp>
#include <qlever_wf_runtime.h>
#endif

namespace sparqlExpression::wf {

#ifdef QLEVER_ENABLE_WF

namespace {

// Per-thread callback-depth counter. Multi-threaded query workers each
// see their own counter, matching the Jena implementation's semantics.
// Bumped/decremented around any host re-entrancy — including the
// HTTP-loopback wrapper below, so a guest that introspects while
// recursing sees 1 rather than 0.
thread_local int gCallbackDepth = 0;

extern "C" int wfCallbackDepth(void* /*user_data*/) { return gCallbackDepth; }

// ---- SPARQL VALUES injection --------------------------------------------

// Escape a lexical form for embedding inside a SPARQL "..."-quoted literal.
// Spec-minimum coverage: backslash, double-quote, and CR/LF/TAB — these are
// the only characters that break a "..."-form. Everything else (including
// UTF-8 multi-byte) is passed through verbatim.
std::string escapeSparqlLiteral(const std::string& in) {
  std::string out;
  out.reserve(in.size() + 8);
  for (char c : in) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"':  out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:   out += c; break;
    }
  }
  return out;
}

// Serialize one WIT-shape binding `value` node as a SPARQL 1.1 term literal.
// bnodes cannot appear in VALUES per the spec, so we reject them — callers
// bubble the error up to a clean err<string> on the guest side.
std::string serializeBindingForSparql(const nlohmann::json& v) {
  if (auto it = v.find("iri"); it != v.end() && it->is_string()) {
    return "<" + it->get<std::string>() + ">";
  }
  if (auto it = v.find("literal"); it != v.end() && it->is_object()) {
    const auto& lit = *it;
    const std::string label = lit.value("label", "");
    std::string out;
    out.reserve(label.size() + 16);
    out += '"';
    out += escapeSparqlLiteral(label);
    out += '"';
    // Language tag beats datatype per SPARQL grammar. Elide xsd:string —
    // it's the canonical default and re-emitting it triggers redundant
    // ^^<xsd:string> suffixes in QLever's parser output.
    auto langIt = lit.find("lang");
    if (langIt != lit.end() && langIt->is_string() &&
        !langIt->get<std::string>().empty()) {
      out += '@';
      out += langIt->get<std::string>();
    } else if (auto dtIt = lit.find("datatype");
               dtIt != lit.end() && dtIt->is_string()) {
      const std::string dt = dtIt->get<std::string>();
      if (!dt.empty() && dt != "http://www.w3.org/2001/XMLSchema#string") {
        out += "^^<";
        out += dt;
        out += ">";
      }
    }
    return out;
  }
  throw std::runtime_error(
      "execute-query: only IRI and literal binding values are supported in "
      "the injected VALUES clause (bnodes are not permitted by SPARQL)");
}

// Splice a single-row VALUES pattern into the top-level WHERE of `sparql`.
//
// The guest supplies a self-contained SPARQL query string that we cannot
// know the shape of without a full parser. As a pragmatic middle ground
// we assume the query is a SELECT/CONSTRUCT/ASK/DESCRIBE with a `{`-delimited
// body somewhere after any PREFIX/BASE prologue. That's what all of our
// current guest crates emit and it's what SPARQL 1.1's grammar demands for
// anything with a WHERE — so we scan forward to the first `{` after the
// prologue and inject `VALUES (...) { (...) }` immediately after it. The
// injected variables get joined into the pattern before projection, giving
// Jena-style initial-binding semantics even for variables the outer SELECT
// doesn't project (e.g. wf_tree's `?this`).
std::string prependValues(const std::string& sparql,
                          const nlohmann::json& bindings_arr) {
  if (bindings_arr.empty()) return sparql;

  std::vector<std::string> vars;
  std::vector<std::string> terms;
  vars.reserve(bindings_arr.size());
  terms.reserve(bindings_arr.size());
  for (const auto& b : bindings_arr) {
    const auto nameIt = b.find("name");
    const auto valueIt = b.find("value");
    if (nameIt == b.end() || !nameIt->is_string() || valueIt == b.end()) {
      throw std::runtime_error(
          "execute-query: binding missing name/value fields");
    }
    vars.push_back("?" + nameIt->get<std::string>());
    terms.push_back(serializeBindingForSparql(*valueIt));
  }

  std::string values = "VALUES (";
  for (size_t i = 0; i < vars.size(); ++i) {
    if (i) values += ' ';
    values += vars[i];
  }
  values += ") { (";
  for (size_t i = 0; i < terms.size(); ++i) {
    if (i) values += ' ';
    values += terms[i];
  }
  values += ") }\n";

  // Find the first `{` that opens the query body. Any `{` inside PREFIX/BASE
  // decls would break this heuristic, but SPARQL grammar disallows `{` in
  // those, so a bare scan is safe.
  const size_t brace_pos = sparql.find('{');
  if (brace_pos == std::string::npos) {
    // Malformed query — best-effort: hand the guest string through
    // unchanged so qlever-server can reject it with a real parser error
    // rather than us mangling it.
    return sparql;
  }
  std::string patched;
  patched.reserve(sparql.size() + values.size() + 4);
  patched.append(sparql, 0, brace_pos + 1);
  patched += '\n';
  patched += values;
  patched.append(sparql, brace_pos + 1, std::string::npos);
  return patched;
}

// ---- HTTP loopback -------------------------------------------------------

// Minimal HTTP/1.1 POST client. Kept intentionally self-contained rather
// than sharing QLever's beast-based `sendHttpOrHttpsRequest`, because:
//   - the beast stack's header ordering constraint (util/http/beast.h must
//     precede any <boost/...> include) is fragile to compose with the
//     wasmtime-generated headers we already include here,
//   - the beast path expects a SharedCancellationHandle we have no
//     meaningful sink for from a wasm host callback,
//   - loopback HTTP over an already-negotiated TCP connection needs
//     essentially none of beast's features (no TLS, no chunked encoding
//     on the request side, no redirects, one round trip).
// If future callbacks need those features, swap this out.
std::string httpPost(const std::string& host, int port,
                     const std::string& path,
                     const std::string& body,
                     const std::string& content_type,
                     const std::string& accept) {
  int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    throw std::runtime_error(std::string{"socket: "} + std::strerror(errno));
  }

  struct sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    ::close(fd);
    throw std::runtime_error("inet_pton failed for " + host);
  }
  if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    ::close(fd);
    throw std::runtime_error(std::string{"connect: "} + std::strerror(errno));
  }

  // HTTP/1.0 forces identity Transfer-Encoding — HTTP/1.1 clients get
  // chunked responses from qlever-server, and we'd rather skip the chunk
  // decoder in this shim.
  std::ostringstream req;
  req << "POST " << path << " HTTP/1.0\r\n"
      << "Host: " << host << ":" << port << "\r\n"
      << "User-Agent: qlever-wf-loopback/0.6\r\n"
      << "Content-Type: " << content_type << "\r\n"
      << "Accept: " << accept << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Connection: close\r\n"
      << "\r\n"
      << body;
  const std::string req_str = req.str();

  size_t written = 0;
  while (written < req_str.size()) {
    ssize_t n = ::send(fd, req_str.data() + written,
                       req_str.size() - written, 0);
    if (n <= 0) {
      ::close(fd);
      throw std::runtime_error(std::string{"send: "} + std::strerror(errno));
    }
    written += static_cast<size_t>(n);
  }

  // Half-close the write side so qlever-server can detect end-of-body
  // even if it doesn't honour Content-Length (belt-and-braces; qlever
  // does honour it, but the SPARQL protocol allows either).
  ::shutdown(fd, SHUT_WR);

  std::string resp;
  char buf[8192];
  for (;;) {
    ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    if (n < 0) {
      ::close(fd);
      throw std::runtime_error(std::string{"recv: "} + std::strerror(errno));
    }
    if (n == 0) break;
    resp.append(buf, static_cast<size_t>(n));
  }
  ::close(fd);

  const size_t header_end = resp.find("\r\n\r\n");
  if (header_end == std::string::npos) {
    throw std::runtime_error("http response missing header terminator");
  }
  if (resp.size() < 12 || resp.compare(0, 5, "HTTP/") != 0) {
    throw std::runtime_error("http response missing status line");
  }
  const int status = std::atoi(resp.data() + 9);
  const std::string body_out = resp.substr(header_end + 4);
  if (status < 200 || status >= 300) {
    throw std::runtime_error("http status " + std::to_string(status) + ": " +
                             body_out.substr(0, 512));
  }
  return body_out;
}

// ---- N-Triples response parsing (CONSTRUCT fallback) -------------------

// Sniff a response body: true iff it looks like an N-Triples payload
// rather than a SPARQL-Results JSON object.
//
// QLever's /query endpoint serves CONSTRUCT results as raw N-Triples
// (turtle-format) text regardless of the Accept header the client sent —
// see ExportQueryExecutionTrees::constructQueryResultToStream: `qleverJson`
// / `sparqlJson` / `sparqlXml` / `octetStream` are all rejected for
// CONSTRUCT with an explicit AD_THROW, so the only branch the runtime
// takes is the turtle serializer, which emits fully-expanded N-Triples
// lines (`<s> <p> <o> .`) because CONSTRUCT rewriting is deliberately
// deferred. That means the `Content-Type: application/sparql-results+json`
// we ask for is a hint the server ignores for graph shapes, and the shim
// has to figure the shape out from the body itself.
//
// A SPARQL-Results JSON body starts with `{`; an N-Triples body starts
// with `<` (IRI subject), `_` (blank-node subject), or `#` (comment).
// Empty bodies fall through to the JSON path so an empty result still
// surfaces its json parse error rather than silently producing zero rows.
bool looksLikeNTriples(const std::string& body) {
  for (char c : body) {
    if (c == ' ' || c == '\t' || c == '\r' || c == '\n') continue;
    return c == '<' || c == '_' || c == '#';
  }
  return false;
}

// Unescape one N-Triples "..."-quoted literal lexical form.
// Handles the escape set from the N-Triples 1.1 grammar (ECHAR + UCHAR):
// \\, \", \t, \n, \r, \b, \f, \uXXXX, \UXXXXXXXX. Unknown escapes fall
// through so a malformed input at least round-trips something visible.
std::string unescapeNTriplesLiteral(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    char c = in[i];
    if (c != '\\' || i + 1 >= in.size()) {
      out += c;
      continue;
    }
    char n = in[i + 1];
    switch (n) {
      case '\\': out += '\\'; ++i; break;
      case '"':  out += '"';  ++i; break;
      case '\'': out += '\''; ++i; break;
      case 't':  out += '\t'; ++i; break;
      case 'n':  out += '\n'; ++i; break;
      case 'r':  out += '\r'; ++i; break;
      case 'b':  out += '\b'; ++i; break;
      case 'f':  out += '\f'; ++i; break;
      case '/':  out += '/';  ++i; break;
      case 'u':
      case 'U': {
        const size_t hexLen = (n == 'u') ? 4 : 8;
        if (i + 1 + hexLen >= in.size()) {
          out += c;
          continue;
        }
        uint32_t cp = 0;
        bool ok = true;
        for (size_t j = 0; j < hexLen; ++j) {
          char h = in[i + 2 + j];
          uint32_t d;
          if (h >= '0' && h <= '9') d = static_cast<uint32_t>(h - '0');
          else if (h >= 'a' && h <= 'f') d = 10u + static_cast<uint32_t>(h - 'a');
          else if (h >= 'A' && h <= 'F') d = 10u + static_cast<uint32_t>(h - 'A');
          else { ok = false; break; }
          cp = (cp << 4) | d;
        }
        if (!ok) {
          out += c;
          continue;
        }
        // Encode the code point as UTF-8.
        if (cp < 0x80u) {
          out += static_cast<char>(cp);
        } else if (cp < 0x800u) {
          out += static_cast<char>(0xC0u | (cp >> 6));
          out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else if (cp < 0x10000u) {
          out += static_cast<char>(0xE0u | (cp >> 12));
          out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
          out += static_cast<char>(0x80u | (cp & 0x3Fu));
        } else {
          out += static_cast<char>(0xF0u | (cp >> 18));
          out += static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
          out += static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
          out += static_cast<char>(0x80u | (cp & 0x3Fu));
        }
        i += 1 + hexLen;
        break;
      }
      default:
        out += c;
        break;
    }
  }
  return out;
}

// Parse a single N-Triples term (IRI, blank node, or literal) beginning
// at `pos` in `line`. On success, `pos` is advanced past the term and
// `out` is populated as a WIT-shape value object. Throws on malformed
// input so the surrounding callback can bubble the error up as a clean
// err<string> to the guest rather than materialising a wrong triple.
void parseNTriplesTerm(const std::string& line, size_t& pos,
                       nlohmann::json& out) {
  while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) ++pos;
  if (pos >= line.size()) {
    throw std::runtime_error("N-Triples parse: unexpected end of term");
  }
  const char c = line[pos];
  if (c == '<') {
    const size_t end = line.find('>', pos + 1);
    if (end == std::string::npos) {
      throw std::runtime_error("N-Triples parse: unterminated IRI");
    }
    out["iri"] = line.substr(pos + 1, end - pos - 1);
    pos = end + 1;
  } else if (c == '_' && pos + 1 < line.size() && line[pos + 1] == ':') {
    size_t start = pos + 2;
    size_t end = start;
    while (end < line.size() && line[end] != ' ' && line[end] != '\t' &&
           line[end] != '.') {
      ++end;
    }
    out["bnode"] = line.substr(start, end - start);
    pos = end;
  } else if (c == '"') {
    size_t start = pos + 1;
    size_t end = start;
    while (end < line.size()) {
      if (line[end] == '\\' && end + 1 < line.size()) {
        end += 2;
        continue;
      }
      if (line[end] == '"') break;
      ++end;
    }
    if (end >= line.size()) {
      throw std::runtime_error("N-Triples parse: unterminated literal");
    }
    const std::string lex =
        unescapeNTriplesLiteral(line.substr(start, end - start));
    pos = end + 1;
    nlohmann::json lit = nlohmann::json::object();
    lit["label"] = lex;
    if (pos < line.size() && line[pos] == '@') {
      size_t lstart = pos + 1;
      size_t lend = lstart;
      while (lend < line.size() && line[lend] != ' ' && line[lend] != '\t' &&
             line[lend] != '.') {
        ++lend;
      }
      lit["lang"] = line.substr(lstart, lend - lstart);
      lit["datatype"] =
          "http://www.w3.org/1999/02/22-rdf-syntax-ns#langString";
      pos = lend;
    } else if (pos + 1 < line.size() && line[pos] == '^' &&
               line[pos + 1] == '^') {
      pos += 2;
      while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
        ++pos;
      }
      if (pos >= line.size() || line[pos] != '<') {
        throw std::runtime_error(
            "N-Triples parse: expected <datatype-iri> after ^^");
      }
      const size_t dend = line.find('>', pos + 1);
      if (dend == std::string::npos) {
        throw std::runtime_error(
            "N-Triples parse: unterminated datatype IRI");
      }
      lit["datatype"] = line.substr(pos + 1, dend - pos - 1);
      lit["lang"] = nullptr;
      pos = dend + 1;
    } else {
      lit["datatype"] = "http://www.w3.org/2001/XMLSchema#string";
      lit["lang"] = nullptr;
    }
    out["literal"] = lit;
  } else {
    throw std::runtime_error(
        std::string{"N-Triples parse: unexpected character '"} + c + "'");
  }
}

// Convert an N-Triples response body into the WIT-shape binding-sets JSON
// the guest expects. Vars are `s`/`p`/`o` (one row per triple) to mirror
// oxigraph-wf's QueryResults::Graph shape (~/git/oxigraph-wf/src/host.rs
// l.1025-1051) and the CONSTRUCT-shape dispatch the Jena/RDF4J adapters
// emit — guests such as wf_infer's `bulk_insert` (~/git/tegmentum-
// webfunctions/crates/wf_infer/src/lib.rs l.337-357) key off exactly
// those three names, so cross-engine parity requires this envelope.
nlohmann::json convertNTriplesToWit(const std::string& body) {
  nlohmann::json out = nlohmann::json::object();
  out["vars"] = nlohmann::json::array({"s", "p", "o"});
  nlohmann::json rows = nlohmann::json::array();

  size_t i = 0;
  while (i < body.size()) {
    while (i < body.size() && (body[i] == ' ' || body[i] == '\t' ||
                                body[i] == '\r' || body[i] == '\n')) {
      ++i;
    }
    if (i >= body.size()) break;
    if (body[i] == '#') {
      while (i < body.size() && body[i] != '\n') ++i;
      continue;
    }
    size_t line_end = body.find('\n', i);
    if (line_end == std::string::npos) line_end = body.size();
    std::string line = body.substr(i, line_end - i);
    i = line_end;

    while (!line.empty() && (line.back() == ' ' || line.back() == '\t' ||
                              line.back() == '\r')) {
      line.pop_back();
    }
    if (line.empty()) continue;
    if (line.back() != '.') {
      throw std::runtime_error(
          "N-Triples parse: line does not end with '.': " +
          line.substr(0, 120));
    }
    line.pop_back();
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
      line.pop_back();
    }

    size_t pos = 0;
    nlohmann::json s = nlohmann::json::object();
    nlohmann::json p = nlohmann::json::object();
    nlohmann::json o = nlohmann::json::object();
    parseNTriplesTerm(line, pos, s);
    parseNTriplesTerm(line, pos, p);
    parseNTriplesTerm(line, pos, o);
    // Anything after the object (before the stripped `.`) is unexpected;
    // silently ignore trailing whitespace but reject stray tokens so a
    // subject-with-embedded-space bug doesn't drop rows silently.
    while (pos < line.size() && (line[pos] == ' ' || line[pos] == '\t')) {
      ++pos;
    }
    if (pos != line.size()) {
      throw std::runtime_error(
          "N-Triples parse: trailing content after object: " +
          line.substr(pos, 80));
    }

    nlohmann::json row = nlohmann::json::array();
    nlohmann::json bindS = nlohmann::json::object();
    bindS["name"] = "s";
    bindS["value"] = s;
    row.push_back(bindS);
    nlohmann::json bindP = nlohmann::json::object();
    bindP["name"] = "p";
    bindP["value"] = p;
    row.push_back(bindP);
    nlohmann::json bindO = nlohmann::json::object();
    bindO["name"] = "o";
    bindO["value"] = o;
    row.push_back(bindO);
    rows.push_back(row);
  }
  out["rows"] = rows;
  return out;
}

// ---- SPARQL 1.1 Results JSON → WIT binding-sets JSON --------------------

// Convert a parsed SPARQL 1.1 Results JSON object into the WIT-shape
// binding-sets JSON the Rust runtime expects on the callback return. The
// mapping preserves the input variable order from the "head" so the guest
// sees columns in the same order the query projected them.
nlohmann::json convertSparqlResultsToWit(const nlohmann::json& sparql) {
  nlohmann::json out = nlohmann::json::object();

  nlohmann::json vars = nlohmann::json::array();
  if (sparql.contains("head") && sparql["head"].contains("vars") &&
      sparql["head"]["vars"].is_array()) {
    for (const auto& v : sparql["head"]["vars"]) vars.push_back(v);
  }
  out["vars"] = vars;

  nlohmann::json rows = nlohmann::json::array();
  if (sparql.contains("results") && sparql["results"].contains("bindings") &&
      sparql["results"]["bindings"].is_array()) {
    for (const auto& row_obj : sparql["results"]["bindings"]) {
      nlohmann::json row = nlohmann::json::array();
      for (const auto& v : vars) {
        const std::string name = v.get<std::string>();
        if (!row_obj.contains(name)) continue;
        const auto& term = row_obj[name];
        const std::string type = term.value("type", "literal");
        nlohmann::json value = nlohmann::json::object();
        if (type == "uri") {
          value["iri"] = term.value("value", "");
        } else if (type == "bnode") {
          value["bnode"] = term.value("value", "");
        } else {
          // literal | typed-literal — same-shape output in either case.
          nlohmann::json lit = nlohmann::json::object();
          lit["label"] = term.value("value", "");
          const std::string dt = term.value("datatype", "");
          const std::string lang = term.value("xml:lang", "");
          if (!lang.empty()) {
            // rdf:langString is the canonical datatype for lang-tagged
            // literals; encode both fields so the guest doesn't have to
            // special-case it.
            lit["datatype"] =
                "http://www.w3.org/1999/02/22-rdf-syntax-ns#langString";
            lit["lang"] = lang;
          } else {
            lit["datatype"] = dt.empty()
                ? std::string("http://www.w3.org/2001/XMLSchema#string")
                : dt;
            lit["lang"] = nullptr;
          }
          value["literal"] = lit;
        }
        nlohmann::json binding = nlohmann::json::object();
        binding["name"] = name;
        binding["value"] = value;
        row.push_back(binding);
      }
      rows.push_back(row);
    }
  }
  out["rows"] = rows;
  return out;
}

// Duplicate a std::string into a malloc'd buffer so the Rust runtime can
// free it with `free()`, matching the ABI contract in qlever_wf_runtime.h.
char* mallocDup(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (!p) return nullptr;
  std::memcpy(p, s.data(), s.size());
  p[s.size()] = '\0';
  return p;
}

// C ABI callback. Returns a malloc'd JSON string on success and writes NULL
// to *err_out; on failure returns NULL and writes a malloc'd error message
// to *err_out. Matches the wf_execute_query_cb typedef in qlever_wf_runtime.h.
extern "C" char* wfExecuteQueryCallback(void* /*user_data*/,
                                        const char* sparql_c,
                                        const char* bindings_json_c,
                                        int max_rows,
                                        char** err_out) {
  try {
    // Configuration lives in the environment rather than the callback's
    // user_data so it can be flipped without rebuilding qlever-server.
    // The whoever-starts-qlever-server sets QLEVER_LOOPBACK_PORT to match
    // whatever `-p N` they passed on the CLI.
    const char* port_env = std::getenv("QLEVER_LOOPBACK_PORT");
    if (!port_env || !*port_env) {
      *err_out = mallocDup(
          "execute-query: QLEVER_LOOPBACK_PORT unset — cannot loop back to "
          "qlever-server. Start the server with `qlever-server --port N` and "
          "set QLEVER_LOOPBACK_PORT=N in the same environment.");
      return nullptr;
    }
    const int port = std::atoi(port_env);
    if (port <= 0 || port > 65535) {
      *err_out = mallocDup("execute-query: QLEVER_LOOPBACK_PORT out of range");
      return nullptr;
    }

    std::string sparql = sparql_c ? std::string(sparql_c) : std::string{};
    if (bindings_json_c && *bindings_json_c) {
      nlohmann::json bindings = nlohmann::json::parse(bindings_json_c);
      if (bindings.is_array() && !bindings.empty()) {
        sparql = prependValues(sparql, bindings);
      }
    }

    // Bump the depth counter around the outbound roundtrip so a guest
    // that introspects mid-recursion sees a truthful value. The Rust
    // runtime also bumps its own fallback counter; we're the source of
    // truth per the callback_depth registration, so we keep both in sync.
    ++gCallbackDepth;
    std::string body;
    try {
      body = httpPost("127.0.0.1", port, "/query", sparql,
                      "application/sparql-query",
                      "application/sparql-results+json");
    } catch (...) {
      --gCallbackDepth;
      throw;
    }
    --gCallbackDepth;

    // QLever's /query returns CONSTRUCT results as N-Triples text no
    // matter what Accept header we sent (see `looksLikeNTriples` above),
    // so sniff the body shape and dispatch. SELECT/ASK still take the
    // SPARQL-Results JSON path; CONSTRUCT/DESCRIBE take the s/p/o
    // binding-sets envelope that matches oxigraph-wf's QueryResults::Graph
    // shape and the Jena/RDF4J CONSTRUCT dispatch.
    nlohmann::json wit;
    if (looksLikeNTriples(body)) {
      wit = convertNTriplesToWit(body);
    } else {
      nlohmann::json parsed = nlohmann::json::parse(body);
      wit = convertSparqlResultsToWit(parsed);
    }

    // ABI comment on max_rows: -1 means no cap; positive value is a hard
    // row cap. Applied post-fetch because SPARQL LIMIT would change the
    // semantics of the outer query if the guest already added its own.
    if (max_rows >= 0 && wit["rows"].is_array() &&
        wit["rows"].size() > static_cast<size_t>(max_rows)) {
      wit["rows"].get_ref<nlohmann::json::array_t&>().resize(
          static_cast<size_t>(max_rows));
    }

    const std::string out = wit.dump();
    char* buf = mallocDup(out);
    if (!buf) {
      *err_out = mallocDup("execute-query: malloc failed");
      return nullptr;
    }
    return buf;
  } catch (const std::exception& e) {
    *err_out = mallocDup(std::string{"execute-query: "} + e.what());
    return nullptr;
  } catch (...) {
    *err_out = mallocDup("execute-query: unknown error");
    return nullptr;
  }
}

// C ABI callback for `execute-update`. Return contract per the Rust-side
// `wf_execute_update_cb`: NULL on success (WIT ok), non-NULL malloc'd
// error string on failure (which the Rust runtime free()s and wraps as
// err<string>). `err_out` is set to NULL on success and left untouched on
// failure — the returned string carries the failure message directly.
//
// Wire shape: POST /?access-token=<token> with body = update text and
// Content-Type: application/sparql-update. That's the wire that QLever's
// SparqlProtocol::parsePOST routes to visitUpdate (see qlever/src/engine/
// SparqlProtocol.cpp l.138). Any non-2xx response body is bubbled up as
// the failure message.
extern "C" char* wfExecuteUpdateCallback(void* /*user_data*/,
                                         const char* sparql_c,
                                         char** err_out) {
  try {
    const char* port_env = std::getenv("QLEVER_LOOPBACK_PORT");
    if (!port_env || !*port_env) {
      return mallocDup(
          "execute-update: QLEVER_LOOPBACK_PORT unset — cannot loop back to "
          "qlever-server. Start the server with `qlever-server --port N` and "
          "set QLEVER_LOOPBACK_PORT=N in the same environment.");
    }
    const int port = std::atoi(port_env);
    if (port <= 0 || port > 65535) {
      return mallocDup("execute-update: QLEVER_LOOPBACK_PORT out of range");
    }

    // Access token is required by qlever-server's visitUpdate path unless
    // the server was started with --no-access-check. Sourced from the
    // environment so it can be flipped without rebuilding qlever-server.
    // If unset, we still attempt the update — qlever-server will reject
    // with a "requires a valid access token" error we bubble up verbatim,
    // which is easier to diagnose than a silent misconfiguration.
    const char* token_env = std::getenv("QLEVER_LOOPBACK_ACCESS_TOKEN");
    std::string path = "/";
    if (token_env && *token_env) {
      // Very small subset of URL encoding — the token is a config-time
      // secret whose character set the deployer controls. We only guard
      // the couple of characters that would break the query-string parse.
      std::string enc;
      enc.reserve(std::strlen(token_env));
      for (const char* p = token_env; *p; ++p) {
        char c = *p;
        if (c == '&' || c == '=' || c == '#' || c == '%' || c == ' ' ||
            c == '?') {
          char buf[4];
          std::snprintf(buf, sizeof(buf), "%%%02X",
                        static_cast<unsigned char>(c));
          enc += buf;
        } else {
          enc += c;
        }
      }
      path = "/?access-token=" + enc;
    }

    const std::string sparql = sparql_c ? std::string(sparql_c) : std::string{};

    ++gCallbackDepth;
    std::string body;
    try {
      // qlever-server's parsePOST dispatches on Content-Type, so the path
      // itself doesn't need to be `/update` — the update-vs-query
      // discrimination happens on the header. Accept an empty body on
      // success (updates return "" or a small metadata JSON depending on
      // how the server was configured).
      body = httpPost("127.0.0.1", port, path, sparql,
                      "application/sparql-update",
                      "application/qlever-results+json");
    } catch (...) {
      --gCallbackDepth;
      throw;
    }
    --gCallbackDepth;

    // Success. The result body (if any) is metadata about the update; the
    // WIT return is `result<_, string>` so we drop the payload and just
    // signal ok by returning NULL.
    *err_out = nullptr;
    return nullptr;
  } catch (const std::exception& e) {
    return mallocDup(std::string{"execute-update: "} + e.what());
  } catch (...) {
    return mallocDup("execute-update: unknown error");
  }
}

}  // namespace

struct WfRuntime::Impl {
  // The Rust crate hides all synchronization behind this opaque handle;
  // concurrent wf_runtime_invoke calls are safe.
  ::WfRuntime* handle_ = nullptr;

  Impl() {
    // Populate a callback table before constructing the runtime. As of
    // v0.5 execute-query is wired to the HTTP-loopback shim; if
    // QLEVER_LOOPBACK_PORT is unset at call time the shim itself returns
    // a clean err<string>, so guests that never call execute-query still
    // work with no deployment change.
    ::WfCallbacks callbacks{};
    callbacks.user_data = nullptr;
    callbacks.execute_query = &wfExecuteQueryCallback;
    callbacks.callback_depth = &wfCallbackDepth;
    callbacks.reserved_prepare_query = nullptr;
    callbacks.reserved_run_prepared = nullptr;
    callbacks.execute_update = &wfExecuteUpdateCallback;

    handle_ = ::wf_runtime_new_with_callbacks(&callbacks);
    if (!handle_) {
      throw std::runtime_error("wf:call: wf_runtime_new returned NULL (OOM?)");
    }

    // Emit a startup warning so a misconfigured deployment surfaces the
    // problem at server-boot time rather than the first wf:call. Guests
    // that never touch execute-query still work — the warning is
    // informational, not fatal.
    if (!std::getenv("QLEVER_LOOPBACK_PORT")) {
      std::cerr << "[wf] warning: QLEVER_LOOPBACK_PORT unset — wf:call "
                   "execute-query host import will return err<string>. "
                   "Guests that only use evaluate + callback-depth still work."
                << std::endl;
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

  // See WasmRuntime.h for the semantics; this is the FFI shim over
  // wf_runtime_rewrite_query. The Rust runtime owns the returned buffers.
  RewriteResult rewriteQuery(const std::string& query) {
    char* err = nullptr;
    char* aliases = nullptr;
    const char* out = ::wf_runtime_rewrite_query(handle_, query.c_str(),
                                                 &aliases, &err);
    if (!out) {
      std::string msg = err ? std::string("wf:rewrite: ") + err
                            : std::string("wf:rewrite: unknown error");
      if (err) ::wf_runtime_free_string(err);
      // The rewrite ABI documents `aliases_json_out` as always NULL on
      // failure, but harden against a future runtime that violates the
      // contract by freeing anyway.
      if (aliases) ::wf_runtime_free_string(aliases);
      throw std::runtime_error(msg);
    }
    RewriteResult res;
    res.rewritten = std::string{out};
    ::wf_runtime_free_string(out);
    if (aliases) {
      res.aliasesJson = std::string{aliases};
      ::wf_runtime_free_string(aliases);
    }
    return res;
  }

  std::string invokeById(const std::string& idHex) {
    char* err = nullptr;
    const char* out =
        ::wf_runtime_invoke_by_id(handle_, idHex.c_str(), &err);
    if (!out) {
      std::string msg = err ? std::string("wf-invoke: ") + err
                            : std::string("wf-invoke: unknown error");
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
    std::string result{out};
    ::wf_runtime_free_string(out);
    return result;
  }

  // Loaders — one thin FFI hop each. On non-zero return the Rust runtime
  // has populated `err` with a heap C string we own; wrap it in a
  // std::runtime_error the caller (ServerMain) can react to.
  void loadAliasMap(const std::string& dbPath, const std::string& table) {
    char* err = nullptr;
    const unsigned int rc = ::wf_runtime_load_alias_map_from_sqlite(
        handle_, dbPath.c_str(), table.c_str(), &err);
    if (rc != 0) {
      std::string msg = err ? std::string("wf: ") + err
                            : std::string("wf: load-alias-map failed (rc=") +
                                  std::to_string(rc) + ")";
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
  }

  void loadShapeRegistry(const std::string& dbPath, const std::string& table) {
    char* err = nullptr;
    const unsigned int rc = ::wf_runtime_load_shape_registry_from_sqlite(
        handle_, dbPath.c_str(), table.c_str(), &err);
    if (rc != 0) {
      std::string msg = err ? std::string("wf: ") + err
                            : std::string("wf: load-shape-registry failed (rc=") +
                                  std::to_string(rc) + ")";
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
  }

  void loadConversionRegistry(const std::string& jsonPath) {
    char* err = nullptr;
    const unsigned int rc = ::wf_runtime_load_conversion_registry_from_json(
        handle_, jsonPath.c_str(), &err);
    if (rc != 0) {
      std::string msg = err ? std::string("wf: ") + err
                            : std::string("wf: load-conversion-registry failed (rc=") +
                                  std::to_string(rc) + ")";
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
  }

  void loadFulltextRegistry(const std::string& jsonPath) {
    char* err = nullptr;
    const unsigned int rc = ::wf_runtime_load_fulltext_registry_from_json(
        handle_, jsonPath.c_str(), &err);
    if (rc != 0) {
      std::string msg = err ? std::string("wf: ") + err
                            : std::string("wf: load-fulltext-registry failed (rc=") +
                                  std::to_string(rc) + ")";
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
  }

  void loadDocumentRegistry(const std::string& jsonPath) {
    char* err = nullptr;
    const unsigned int rc = ::wf_runtime_load_document_registry_from_json(
        handle_, jsonPath.c_str(), &err);
    if (rc != 0) {
      std::string msg = err ? std::string("wf: ") + err
                            : std::string("wf: load-document-registry failed (rc=") +
                                  std::to_string(rc) + ")";
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
  }

  void loadFederationRegistry(const std::string& jsonPath) {
    char* err = nullptr;
    const unsigned int rc = ::wf_runtime_load_federation_registry_from_json(
        handle_, jsonPath.c_str(), &err);
    if (rc != 0) {
      std::string msg = err ? std::string("wf: ") + err
                            : std::string("wf: load-federation-registry failed (rc=") +
                                  std::to_string(rc) + ")";
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
  }

  void setFetchUrl(const std::string& url) {
    char* err = nullptr;
    const unsigned int rc =
        ::wf_runtime_set_wf_fetch_url(handle_, url.c_str(), &err);
    if (rc != 0) {
      std::string msg = err ? std::string("wf: ") + err
                            : std::string("wf: set-wf-fetch-url failed (rc=") +
                                  std::to_string(rc) + ")";
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
  }

  std::string runCanonicalizeSweep(const std::string& wasmUrl,
                                   const std::string& sinkUrl, bool fullScan) {
    char* err = nullptr;
    const char* out = ::wf_runtime_run_canonicalize_sweep(
        handle_, wasmUrl.c_str(), sinkUrl.c_str(), fullScan ? 1 : 0, &err);
    if (!out) {
      std::string msg = err ? std::string("wf-canonicalize: ") + err
                            : std::string("wf-canonicalize: unknown error");
      if (err) ::wf_runtime_free_string(err);
      throw std::runtime_error(msg);
    }
    std::string result{out};
    ::wf_runtime_free_string(out);
    return result;
  }

  // Canonicalize-sweep configuration — captured once at server boot from
  // ServerMain's CLI flags and consumed by Server::process's
  // /admin/canonicalize-sweep handler. Empty when the operator didn't
  // configure the sweep; the endpoint returns a 400 in that case.
  std::string canonicalizeWasmUrl_;
  std::string canonicalizeSinkUrl_;
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

RewriteResult WfRuntime::rewriteQuery(std::string_view query) {
  return impl_->rewriteQuery(std::string{query});
}

std::string WfRuntime::invokeById(std::string_view idHex) {
  return impl_->invokeById(std::string{idHex});
}

void WfRuntime::loadAliasMapFromSqlite(std::string_view dbPath,
                                       std::string_view table) {
  impl_->loadAliasMap(std::string{dbPath}, std::string{table});
}

void WfRuntime::loadShapeRegistryFromSqlite(std::string_view dbPath,
                                            std::string_view table) {
  impl_->loadShapeRegistry(std::string{dbPath}, std::string{table});
}

void WfRuntime::loadConversionRegistryFromJson(std::string_view jsonPath) {
  impl_->loadConversionRegistry(std::string{jsonPath});
}

void WfRuntime::loadFulltextRegistryFromJson(std::string_view jsonPath) {
  impl_->loadFulltextRegistry(std::string{jsonPath});
}

void WfRuntime::loadDocumentRegistryFromJson(std::string_view jsonPath) {
  impl_->loadDocumentRegistry(std::string{jsonPath});
}

void WfRuntime::loadFederationRegistryFromJson(std::string_view jsonPath) {
  impl_->loadFederationRegistry(std::string{jsonPath});
}

void WfRuntime::setWfFetchUrl(std::string_view url) {
  impl_->setFetchUrl(std::string{url});
}

void WfRuntime::setCanonicalizeConfig(std::string_view wasmUrl,
                                      std::string_view sinkUrl) {
  impl_->canonicalizeWasmUrl_.assign(wasmUrl.data(), wasmUrl.size());
  impl_->canonicalizeSinkUrl_.assign(sinkUrl.data(), sinkUrl.size());
}

const std::string& WfRuntime::canonicalizeWasmUrl() const {
  return impl_->canonicalizeWasmUrl_;
}

const std::string& WfRuntime::canonicalizeSinkUrl() const {
  return impl_->canonicalizeSinkUrl_;
}

std::string WfRuntime::runCanonicalizeSweep(std::string_view wasmUrl,
                                            std::string_view sinkUrl,
                                            bool fullScan) {
  return impl_->runCanonicalizeSweep(std::string{wasmUrl}, std::string{sinkUrl},
                                     fullScan);
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

RewriteResult WfRuntime::rewriteQuery(std::string_view query) {
  // Passthrough when the runtime is compiled out — hand the query text
  // back unchanged so the parser sees it exactly as the client sent it,
  // and hand back an empty alias map so the caller has nothing to
  // canonicalise on the output path. This mirrors how the caller would
  // behave if the runtime were linked in but had no active aliases.
  return RewriteResult{std::string{query}, std::string{}};
}

std::string WfRuntime::invokeById(std::string_view) {
  throw std::runtime_error(
      "wf-invoke: this QLever build was compiled without QLEVER_ENABLE_WF; "
      "SERVICE <wf-invoke:...> cannot be evaluated.");
}

// Loaders throw when the runtime is compiled out. Server main should
// treat any --wf-* flag as user error against a build without wf and
// exit non-zero rather than silently ignore the flag.
static void wfDisabledThrow(const char* flag) {
  throw std::runtime_error(
      std::string{"wf: --"} + flag +
      " requires this build to have been compiled with -DQLEVER_ENABLE_WF=ON");
}

void WfRuntime::loadAliasMapFromSqlite(std::string_view, std::string_view) {
  wfDisabledThrow("wf-alias-db");
}

void WfRuntime::loadShapeRegistryFromSqlite(std::string_view, std::string_view) {
  wfDisabledThrow("wf-shape-db");
}

void WfRuntime::loadConversionRegistryFromJson(std::string_view) {
  wfDisabledThrow("wf-conversion-rules");
}

void WfRuntime::loadFulltextRegistryFromJson(std::string_view) {
  wfDisabledThrow("wf-fulltext-config");
}

void WfRuntime::loadDocumentRegistryFromJson(std::string_view) {
  wfDisabledThrow("wf-document-config");
}

void WfRuntime::loadFederationRegistryFromJson(std::string_view) {
  wfDisabledThrow("wf-federation-config");
}

void WfRuntime::setWfFetchUrl(std::string_view) {
  wfDisabledThrow("wf-fetch-url");
}

void WfRuntime::setCanonicalizeConfig(std::string_view, std::string_view) {
  // Silently ignore in the disabled build so a --wf-canonicalize-wasm-url
  // supplied against a non-wf QLever doesn't crash the server at boot.
  // The endpoint itself will still 404 (no route registered) because the
  // ifdef guarding /admin/canonicalize-sweep in Server.cpp keys off
  // WfRuntime::isEnabled().
}

const std::string& WfRuntime::canonicalizeWasmUrl() const {
  static const std::string empty;
  return empty;
}

const std::string& WfRuntime::canonicalizeSinkUrl() const {
  static const std::string empty;
  return empty;
}

std::string WfRuntime::runCanonicalizeSweep(std::string_view, std::string_view,
                                            bool) {
  throw std::runtime_error(
      "wf-canonicalize: this QLever build was compiled without "
      "QLEVER_ENABLE_WF; the /admin/canonicalize-sweep endpoint is not "
      "available.");
}

#endif

}  // namespace sparqlExpression::wf
