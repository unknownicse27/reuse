//===-- Z3Solver.cpp -------------------------------------------*- C++ -*-====//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Config/config.h"
#include "klee/Support/ErrorHandling.h"
#include "klee/Support/FileHandling.h"
#include "klee/Support/OptionCategories.h"

#include <csignal>
#include <cctype>
#include <cstdlib>
#include <cstring>

#ifdef ENABLE_Z3

#include "Z3Solver.h"
#include "Z3Builder.h"

#include "klee/Expr/Constraints.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Solver/Solver.h"
#include "klee/Solver/SolverImpl.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <chrono>
#include <memory>
#include <map>
#include <unordered_map>
#include <cstdint>
#include <cstdio>
#include <vector>
#include <algorithm>
#include <unordered_set>
#include <string>
#include <sstream>

static std::string normalizeSExpr(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  bool inSpace = false;

  for (char c : s) {
    if (std::isspace(static_cast<unsigned char>(c))) {
      if (!inSpace) {
        out.push_back(' ');
        inSpace = true;
      }
    } else {
      out.push_back(c);
      inSpace = false;
    }
  }
  if (!out.empty() && out.front() == ' ')
    out.erase(out.begin());
  if (!out.empty() && out.back() == ' ')
    out.pop_back();
  return out;
}

namespace {
static inline uint64_t rotr64(uint64_t w, unsigned c) {
  return (w >> c) | (w << (64 - c));
}

static const uint64_t blake2b_iv[8] = {
  0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL,
  0x3c6ef372fe94f82bULL, 0xa54ff53a5f1d36f1ULL,
  0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
  0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL
};

static const uint8_t blake2b_sigma[12][16] = {
  { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15},
  {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3},
  {11, 8,12, 0, 5, 2,15,13,10,14, 3, 6, 7, 1, 9, 4},
  { 7, 9, 3, 1,13,12,11,14, 2, 6, 5,10, 4, 0,15, 8},
  { 9, 0, 5, 7, 2, 4,10,15,14, 1,11,12, 6, 8, 3,13},
  { 2,12, 6,10, 0,11, 8, 3, 4,13, 7, 5,15,14, 1, 9},
  {12, 5, 1,15,14,13, 4,10, 0, 7, 6, 3, 9, 2, 8,11},
  {13,11, 7,14,12, 1, 3, 9, 5, 0,15, 4, 8, 6, 2,10},
  { 6,15,14, 9,11, 3, 0, 8,12, 2,13, 7, 1, 4,10, 5},
  {10, 2, 8, 4, 7, 6, 1, 5,15,11, 9,14, 3,12,13, 0},
  { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15},
  {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3}
};

static inline uint64_t load64_le(const uint8_t *p) {
  return ((uint64_t)p[0]) |
         ((uint64_t)p[1] << 8) |
         ((uint64_t)p[2] << 16) |
         ((uint64_t)p[3] << 24) |
         ((uint64_t)p[4] << 32) |
         ((uint64_t)p[5] << 40) |
         ((uint64_t)p[6] << 48) |
         ((uint64_t)p[7] << 56);
}

static inline void store64_le(uint8_t *p, uint64_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
  p[4] = (uint8_t)(v >> 32);
  p[5] = (uint8_t)(v >> 40);
  p[6] = (uint8_t)(v >> 48);
  p[7] = (uint8_t)(v >> 56);
}

static inline void G(uint64_t &a, uint64_t &b, uint64_t &c, uint64_t &d,
                     uint64_t x, uint64_t y) {
  a = a + b + x;
  d = rotr64(d ^ a, 32);
  c = c + d;
  b = rotr64(b ^ c, 24);
  a = a + b + y;
  d = rotr64(d ^ a, 16);
  c = c + d;
  b = rotr64(b ^ c, 63);
}

static void blake2b_compress(uint64_t h[8], const uint8_t block[128],
                             uint64_t t0, uint64_t t1, bool last) {
  uint64_t m[16];
  for (int i = 0; i < 16; ++i) {
    m[i] = load64_le(block + i * 8);
  }

  uint64_t v[16];
  for (int i = 0; i < 8; ++i) v[i] = h[i];
  for (int i = 0; i < 8; ++i) v[i + 8] = blake2b_iv[i];
  v[12] ^= t0;
  v[13] ^= t1;
  if (last) v[14] = ~v[14];
  for (int r = 0; r < 12; ++r) {
    const uint8_t *s = blake2b_sigma[r];
    G(v[0], v[4], v[8], v[12], m[s[0]],  m[s[1]]);
    G(v[1], v[5], v[9], v[13], m[s[2]],  m[s[3]]);
    G(v[2], v[6], v[10],v[14], m[s[4]],  m[s[5]]);
    G(v[3], v[7], v[11],v[15], m[s[6]],  m[s[7]]);
    G(v[0], v[5], v[10],v[15], m[s[8]],  m[s[9]]);
    G(v[1], v[6], v[11],v[12], m[s[10]], m[s[11]]);
    G(v[2], v[7], v[8], v[13], m[s[12]], m[s[13]]);
    G(v[3], v[4], v[9], v[14], m[s[14]], m[s[15]]);
  }
  for (int i = 0; i < 8; ++i) {
    h[i] ^= v[i] ^ v[i + 8];
  }
}

static uint64_t blake2b64_id(const std::string &s) {
  uint64_t h[8];
  for (int i = 0; i < 8; ++i) h[i] = blake2b_iv[i];
  h[0] ^= 0x01010000ULL ^ 8ULL;

  uint8_t block[128];
  std::memset(block, 0, sizeof(block));

  uint64_t t0 = 0, t1 = 0;
  const uint8_t *data = (const uint8_t*)s.data();
  size_t len = s.size();

  while (len > 128) {
    std::memcpy(block, data, 128);
    t0 += 128;
    if (t0 < 128) t1++;
    blake2b_compress(h, block, t0, t1, false);
    data += 128;
    len -= 128;
  }
  std::memset(block, 0, sizeof(block));
  if (len) std::memcpy(block, data, len);
  t0 += (uint64_t)len;
  if (t0 < (uint64_t)len) t1++;
  blake2b_compress(h, block, t0, t1, true);
  uint8_t out[8];
  store64_le(out, h[0]);
  uint64_t be = 0;
  for (int i = 0; i < 8; ++i) {
    be = (be << 8) | (uint64_t)out[i];
  }
  return be;
}

static bool parseJsonHexIdList(const std::string &line, std::vector<uint64_t> &out) {
  out.clear();
  std::string s = line;
  auto ltrim = [](std::string &x) {
    size_t i = 0;
    while (i < x.size() && std::isspace((unsigned char)x[i])) ++i;
    x.erase(0, i);
  };
  auto rtrim = [](std::string &x) {
    size_t i = x.size();
    while (i > 0 && std::isspace((unsigned char)x[i-1])) --i;
    x.erase(i);
  };
  ltrim(s); rtrim(s);
  if (s.size() < 2 || s.front() != '[' || s.back() != ']') return false;
  if (s == "[]") return true;
  size_t i = 1;
  while (i < s.size()) {
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    if (i >= s.size() || s[i] == ']') break;
    if (s[i] != '"') return false;
    ++i;
    size_t start = i;
    while (i < s.size() && s[i] != '"') ++i;
    if (i >= s.size()) return false;
    std::string tok = s.substr(start, i - start);
    ++i;
    if (tok.rfind("0x", 0) == 0 || tok.rfind("0X", 0) == 0) tok = tok.substr(2);
    if (tok.empty()) return false;
    uint64_t v = 0;
    for (char c : tok) {
      unsigned char uc = (unsigned char)c;
      if (!std::isxdigit(uc)) return false;
      v <<= 4;
      if (c >= '0' && c <= '9') v |= (uint64_t)(c - '0');
      else if (c >= 'a' && c <= 'f') v |= (uint64_t)(10 + (c - 'a'));
      else if (c >= 'A' && c <= 'F') v |= (uint64_t)(10 + (c - 'A'));
    }
    out.push_back(v);
    while (i < s.size() && std::isspace((unsigned char)s[i])) ++i;
    if (i < s.size() && s[i] == ',') { ++i; continue; }
    if (i < s.size() && s[i] == ']') break;
  }
  return true;
}
} // namespace

namespace {
llvm::cl::opt<std::string> Z3LogInteractionFile(
    "debug-z3-log-api-interaction", llvm::cl::init(""),
    llvm::cl::desc("Log API interaction with Z3 to the specified path"),
    llvm::cl::cat(klee::SolvingCat));

llvm::cl::opt<std::string> Z3QueryDumpFile(
    "debug-z3-dump-queries", llvm::cl::init(""),
    llvm::cl::desc("Dump Z3's representation of the query to the specified path"),
    llvm::cl::cat(klee::SolvingCat));

llvm::cl::opt<bool> Z3ValidateModels(
    "debug-z3-validate-models", llvm::cl::init(false),
    llvm::cl::desc("When generating Z3 models validate these against the query"),
    llvm::cl::cat(klee::SolvingCat));

llvm::cl::opt<unsigned>
    Z3VerbosityLevel("debug-z3-verbosity", llvm::cl::init(0),
                     llvm::cl::desc("Z3 verbosity level (default=0)"),
                     llvm::cl::cat(klee::SolvingCat));
}

#include "llvm/Support/ErrorHandling.h"

namespace klee {

class Z3SolverImpl : public SolverImpl {
private:
  std::unique_ptr<Z3Builder> builder;
  time::Span timeout;
  SolverRunStatus runStatusCode;
  std::unique_ptr<llvm::raw_fd_ostream> dumpedQueriesFile;
  ::Z3_params solverParameters;
  ::Z3_symbol timeoutParamStrSymbol;

  std::vector<std::unordered_set<uint64_t>> cachedUnsatCores;
  std::vector<unsigned> coreHitCounts;
  std::string unsatCoreHitLogPath;

  void loadUnsatCoreCache();
  int getImpliedCachedCoreIndex(::Z3_solver theSolver);

  bool internalRunSolver(const Query &,
                         const std::vector<const Array *> *objects,
                         std::vector<std::vector<unsigned char> > *values,
                         bool &hasSolution);
  bool validateZ3Model(::Z3_solver &theSolver, ::Z3_model &theModel);

public:
  Z3SolverImpl();
  ~Z3SolverImpl();

  std::string getConstraintLog(const Query &) override;
  void setCoreSolverTimeout(time::Span _timeout) {
    timeout = _timeout;

    auto timeoutInMilliSeconds = static_cast<unsigned>((timeout.toMicroseconds() / 1000));
    if (!timeoutInMilliSeconds)
      timeoutInMilliSeconds = UINT_MAX;
    Z3_params_set_uint(builder->ctx, solverParameters, timeoutParamStrSymbol,
                       timeoutInMilliSeconds);
  }

  bool computeTruth(const Query &, bool &isValid);
  bool computeValue(const Query &, ref<Expr> &result);
  bool computeInitialValues(const Query &,
                            const std::vector<const Array *> &objects,
                            std::vector<std::vector<unsigned char> > &values,
                            bool &hasSolution);
  SolverRunStatus
  handleSolverResponse(::Z3_solver theSolver, ::Z3_lbool satisfiable,
                       const std::vector<const Array *> *objects,
                       std::vector<std::vector<unsigned char> > *values,
                       bool &hasSolution);
  SolverRunStatus getOperationStatusCode();
};

Z3SolverImpl::Z3SolverImpl()
    : builder(new Z3Builder(false, Z3LogInteractionFile.size() > 0
              ? Z3LogInteractionFile.c_str()
              : NULL)),
      runStatusCode(SOLVER_RUN_STATUS_FAILURE) {
  assert(builder && "unable to create Z3Builder");
  solverParameters = Z3_mk_params(builder->ctx);
  Z3_params_inc_ref(builder->ctx, solverParameters);
  timeoutParamStrSymbol = Z3_mk_string_symbol(builder->ctx, "timeout");
  setCoreSolverTimeout(timeout);

  if (!Z3QueryDumpFile.empty()) {
    std::string error;
    dumpedQueriesFile = klee_open_output_file(Z3QueryDumpFile, error);
    if (!dumpedQueriesFile) {
      klee_error("Error creating file for dumping Z3 queries: %s",
                 error.c_str());
    }
    klee_message("Dumping Z3 queries to \"%s\"", Z3QueryDumpFile.c_str());
  }

  if (Z3VerbosityLevel > 0) {
    std::string underlyingString;
    llvm::raw_string_ostream ss(underlyingString);
    ss << Z3VerbosityLevel;
    ss.flush();
    Z3_global_param_set("verbose", underlyingString.c_str());
  }

  {
    const char *outDirEnv = std::getenv("KLEE_OUTPUT_DIR");
    std::string outDir = outDirEnv && *outDirEnv
                           ? std::string(outDirEnv)
                           : std::string("klee-last");
    if (!outDir.empty() && outDir.back() == '/')
      outDir.pop_back();

    unsatCoreHitLogPath = outDir + "/unsat_core_hit.log";
    llvm::errs() << "[Z3Solver] UNSAT core hit log path: "
                 << unsatCoreHitLogPath << "\n";
  }
  loadUnsatCoreCache();
  coreHitCounts.assign(cachedUnsatCores.size(), 0);
}

static std::vector<std::string> tokenizeSExpr(const std::string &text) {
  std::vector<std::string> tokens;
  tokens.reserve(text.size() / 2);
  size_t i = 0, n = text.size();
  while (i < n) {
    char c = text[i];
    if (std::isspace(static_cast<unsigned char>(c))) {
      ++i;
      continue;
    }
    if (c == '(' || c == ')') {
      tokens.emplace_back(1, c);
      ++i;
      continue;
    }
    size_t j = i;
    while (j < n &&
           !std::isspace(static_cast<unsigned char>(text[j])) &&
           text[j] != '(' && text[j] != ')') {
      ++j;
    }
    tokens.push_back(text.substr(i, j - i));
    i = j;
  }
  return tokens;
}

struct SNode {
  bool isAtom = true;
  std::string atom;
  std::vector<SNode> list;

  static SNode Atom(std::string s) {
    SNode n;
    n.isAtom = true;
    n.atom = std::move(s);
    return n;
  }
  static SNode List(std::vector<SNode> xs) {
    SNode n;
    n.isAtom = false;
    n.list = std::move(xs);
    return n;
  }
};

static bool parseSExprAt(const std::vector<std::string> &toks, size_t &i, SNode &out) {
  if (i >= toks.size())
    return false;

  if (toks[i] != "(") {
    out = SNode::Atom(toks[i++]);
    return true;
  }


  ++i;
  std::vector<SNode> elems;
  while (i < toks.size() && toks[i] != ")") {
    SNode child;
    if (!parseSExprAt(toks, i, child))
      return false;
    elems.push_back(std::move(child));
  }
  if (i >= toks.size() || toks[i] != ")")
    return false;
  ++i;
  out = SNode::List(std::move(elems));
  return true;
}

static bool parseSExpr(const std::string &s, SNode &out) {
  auto toks = tokenizeSExpr(s);
  size_t i = 0;
  if (!parseSExprAt(toks, i, out))
    return false;
  if (i != toks.size())
    return false;
  return true;
}

static void dumpSExpr(const SNode &n, std::string &out) {
  if (n.isAtom) {
    out += n.atom;
    return;
  }
  out.push_back('(');
  for (size_t k = 0; k < n.list.size(); ++k) {
    if (k) out.push_back(' ');
    dumpSExpr(n.list[k], out);
  }
  out.push_back(')');
}

static std::string dumpSExpr(const SNode &n) {
  std::string out;
  dumpSExpr(n, out);
  return out;
}

static bool isAtom(const SNode &n, const char *s) {
  return n.isAtom && n.atom == s;
}

static std::string normalizeForPythonCoreMatch(const std::string &term);
static void collectTopLevelAndTerms(const SNode &node, std::vector<SNode> &out) {
  if (!node.isAtom && !node.list.empty() && isAtom(node.list[0], "and")) {
    for (size_t i = 1; i < node.list.size(); ++i) {
      out.push_back(node.list[i]);
    }
    return;
  }
  out.push_back(node);
}

static std::string normalizeNodeForPythonCoreMatch(const SNode &node) {
  std::string s = dumpSExpr(node);
  s = normalizeSExpr(s);
  return normalizeForPythonCoreMatch(s);
}


static bool isAtomPrefixDigits(const SNode &n, const char *prefix) {
  if (!n.isAtom) return false;
  const std::string &a = n.atom;
  size_t plen = std::strlen(prefix);
  if (a.size() <= plen) return false;
  if (a.compare(0, plen, prefix) != 0) return false;
  for (size_t i = plen; i < a.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(a[i])))
      return false;
  }
  return true;
}

static bool parseHex(const std::string &tok, uint64_t &val) {
  if (tok.size() < 3) return false;
  if (!(tok[0] == '#' && (tok[1] == 'x' || tok[1] == 'X'))) return false;
  val = 0;
  for (size_t i = 2; i < tok.size(); ++i) {
    unsigned char c = static_cast<unsigned char>(tok[i]);
    if (!std::isxdigit(c)) return false;
    val <<= 4;
    if (c >= '0' && c <= '9') val |= (c - '0');
    else if (c >= 'a' && c <= 'f') val |= (10 + (c - 'a'));
    else if (c >= 'A' && c <= 'F') val |= (10 + (c - 'A'));
  }
  return true;
}

static std::string stripDoubleNot(const std::string &s);

static SNode rewriteEqFalseToNot(const SNode &n) {
  if (n.isAtom) return n;
  std::vector<SNode> ch;
  ch.reserve(n.list.size());
  for (auto &c : n.list) ch.push_back(rewriteEqFalseToNot(c));
  SNode nn = SNode::List(std::move(ch));

  if (nn.list.size() == 3 &&
      isAtom(nn.list[0], "=") &&
      (isAtom(nn.list[1], "false") || isAtom(nn.list[2], "false"))) {
    const SNode &x = isAtom(nn.list[1], "false") ? nn.list[2] : nn.list[1];
    return SNode::List({SNode::Atom("not"), x});
  }
  return nn;
}

static SNode rewriteBangNamed(const SNode &n) {
  if (n.isAtom) return n;
  std::vector<SNode> ch;
  ch.reserve(n.list.size());
  for (auto &c : n.list) ch.push_back(rewriteBangNamed(c));
  SNode nn = SNode::List(std::move(ch));

  if (nn.list.size() >= 4 && isAtom(nn.list[0], "!") &&
      nn.list[2].isAtom && nn.list[2].atom == ":named") {
    return nn.list[1];
  }
  return nn;
}

static SNode rewriteSelectArrayToSYMBOL(const SNode &n) {
  if (n.isAtom) return n;
  std::vector<SNode> ch;
  ch.reserve(n.list.size());
  for (auto &c : n.list) ch.push_back(rewriteSelectArrayToSYMBOL(c));
  SNode nn = SNode::List(std::move(ch));

  if (nn.list.size() >= 3 && isAtom(nn.list[0], "select") && nn.list[1].isAtom) {
    nn.list[1].atom = "SYMBOL";
  }
  return nn;
}

static SNode rewriteLetBinders(const SNode &n,
                               std::vector<std::unordered_map<std::string,std::string>> &envStack) {
  if (n.isAtom) {
    if (isAtomPrefixDigits(n, "a!")) {
      for (auto it = envStack.rbegin(); it != envStack.rend(); ++it) {
        auto f = it->find(n.atom);
        if (f != it->end())
          return SNode::Atom(f->second);
      }
    }
    return n;
  }

  if (!n.list.empty() && isAtom(n.list[0], "let") && n.list.size() == 3) {
    const SNode &binds = n.list[1];
    const SNode &body  = n.list[2];

    std::unordered_map<std::string,std::string> local;
    int nextId = 0;

    if (!binds.isAtom) {
      for (auto &b : binds.list) {
        if (!b.isAtom && b.list.size() == 2 && b.list[0].isAtom) {
          const std::string &v = b.list[0].atom;
          if (v.rfind("a!", 0) == 0) {
            bool ok = true;
            for (size_t i = 2; i < v.size(); ++i) {
              if (!std::isdigit(static_cast<unsigned char>(v[i]))) { ok = false; break; }
            }
            if (ok) {
              local[v] = "a!" + std::to_string(nextId++);
            }
          }
        }
      }
    }

    envStack.push_back(std::move(local));

    SNode newBinds = binds;
    if (!binds.isAtom) {
      std::vector<SNode> nb;
      nb.reserve(binds.list.size());
      for (auto &b : binds.list) {
        if (!b.isAtom && b.list.size() == 2) {
          SNode v = rewriteLetBinders(b.list[0], envStack);
          SNode e = rewriteLetBinders(b.list[1], envStack);
          nb.push_back(SNode::List({v, e}));
        } else {
          nb.push_back(rewriteLetBinders(b, envStack));
        }
      }
      newBinds = SNode::List(std::move(nb));
    }

    SNode newBody = rewriteLetBinders(body, envStack);
    envStack.pop_back();

    return SNode::List({SNode::Atom("let"), newBinds, newBody});
  }

  std::vector<SNode> ch;
  ch.reserve(n.list.size());
  for (auto &c : n.list) ch.push_back(rewriteLetBinders(c, envStack));
  return SNode::List(std::move(ch));
}

static void collectSelectHexIndices(const SNode &n, std::vector<uint64_t> &vals) {
  if (n.isAtom) return;
  if (n.list.size() == 3 && isAtom(n.list[0], "select") &&
      isAtom(n.list[1], "SYMBOL") && n.list[2].isAtom) {
    uint64_t v;
    if (parseHex(n.list[2].atom, v))
      vals.push_back(v);
  }
  for (auto &c : n.list) collectSelectHexIndices(c, vals);
}

static SNode rewriteSelectHexIndicesToIDX(const SNode &n,
                                         const std::map<uint64_t,int> &v2idx) {
  if (n.isAtom) return n;
  std::vector<SNode> ch;
  ch.reserve(n.list.size());
  for (auto &c : n.list) ch.push_back(rewriteSelectHexIndicesToIDX(c, v2idx));
  SNode nn = SNode::List(std::move(ch));

  if (nn.list.size() == 3 && isAtom(nn.list[0], "select") &&
      isAtom(nn.list[1], "SYMBOL") && nn.list[2].isAtom) {
    uint64_t v;
    if (parseHex(nn.list[2].atom, v)) {
      auto it = v2idx.find(v);
      if (it != v2idx.end()) {
        nn.list[2].atom = "IDX" + std::to_string(it->second);
      }
    }
  }
  return nn;
}

static void collectAllHexConsts(const SNode &n, std::vector<uint64_t> &vals) {
  if (n.isAtom) {
    uint64_t v;
    if (parseHex(n.atom, v))
      vals.push_back(v);
    return;
  }
  for (auto &c : n.list) collectAllHexConsts(c, vals);
}

static SNode rewriteHexConstsToVAL0Delta(const SNode &n, uint64_t base,
                                        const std::unordered_map<uint64_t,std::string> &mapv) {
  if (n.isAtom) {
    uint64_t v;
    if (parseHex(n.atom, v)) {
      auto it = mapv.find(v);
      if (it != mapv.end())
        return SNode::Atom(it->second);
      if (v == base) return SNode::Atom("VAL0");
      uint64_t d = v - base;
      std::string s = "VAL0+0x";
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%llx", (unsigned long long)d);
      s += buf;
      return SNode::Atom(s);
    }
    return n;
  }
  std::vector<SNode> ch;
  ch.reserve(n.list.size());
  for (auto &c : n.list) ch.push_back(rewriteHexConstsToVAL0Delta(c, base, mapv));
  return SNode::List(std::move(ch));
}

static std::string normalizeForPythonCoreMatch(const std::string &term) {
  return normalizeSExpr(term);
}

static std::string stripDoubleNot(const std::string &s) {
  const std::string prefix = "(not (not ";
  if (s.compare(0, prefix.size(), prefix) != 0)
    return s;
  if (s.size() < prefix.size() + 2)
    return s;
  if (s[s.size() - 1] != ')' || s[s.size() - 2] != ')')
    return s;
  return s.substr(prefix.size(), s.size() - prefix.size() - 2);
}


static std::vector<std::string> extractAssertFormulasFromQuery(const std::string &z3Query) {
  std::vector<std::string> result;
  const std::string keyword = "(assert";
  std::size_t pos = 0;

  while (true) {
    std::size_t assertPos = z3Query.find(keyword, pos);
    if (assertPos == std::string::npos)
      break;

    std::size_t open = assertPos;
    int depth = 0;
    std::size_t i = open;
    for (; i < z3Query.size(); ++i) {
      char c = z3Query[i];
      if (c == '(') {
        ++depth;
      } else if (c == ')') {
        --depth;
        if (depth == 0) {
          break;
        }
      }
    }
    if (depth != 0) {
      break;
    }
    std::size_t close = i;
    std::string assertExpr = z3Query.substr(open, close - open + 1);
    std::size_t kwPos = assertExpr.find(keyword);
    if (kwPos == std::string::npos) {
      pos = close + 1;
      continue;
    }
    std::size_t bodyStart = kwPos + keyword.size();
    while (bodyStart < assertExpr.size() &&
           std::isspace(static_cast<unsigned char>(assertExpr[bodyStart]))) {
      ++bodyStart;
    }
    std::size_t bodyEnd = assertExpr.size();
    if (bodyEnd > bodyStart && assertExpr[bodyEnd - 1] == ')')
      --bodyEnd;
    while (bodyEnd > bodyStart &&
           std::isspace(static_cast<unsigned char>(assertExpr[bodyEnd - 1]))) {
      --bodyEnd;
    }

    if (bodyEnd > bodyStart) {
      std::string term = assertExpr.substr(bodyStart, bodyEnd - bodyStart);
      std::string norm;
      norm.reserve(term.size());
      bool inSpace = false;
      for (char c : term) {
        if (std::isspace(static_cast<unsigned char>(c))) {
          if (!inSpace) {
            norm.push_back(' ');
            inSpace = true;
          }
        } else {
          norm.push_back(c);
          inSpace = false;
        }
      }
      if (!norm.empty() && norm.front() == ' ')
        norm.erase(norm.begin());
      if (!norm.empty() && norm.back() == ' ')
        norm.pop_back();

      SNode ast;
      if (parseSExpr(norm, ast)) {
        std::vector<SNode> pieces;
        pieces.reserve(4);
        collectTopLevelAndTerms(ast, pieces);
        for (const auto &p : pieces) {
          std::string one = normalizeNodeForPythonCoreMatch(p);
          if (!one.empty())
            result.push_back(std::move(one));
        }
      } else {
        norm = normalizeForPythonCoreMatch(norm);
        result.push_back(std::move(norm));
      }
    }

    pos = close + 1;
  }

  return result;
}


static void dumpCurrentSetOnly(
    const std::string &logPath,
    const std::unordered_set<uint64_t> &current) {
  if (logPath.empty())
    return;

  std::ofstream out(logPath, std::ios::app);
  if (!out)
    return;

  out << "=== CURRENT SET ===\n";
  out << "size=" << current.size() << "\n";

  std::vector<uint64_t> list;
  list.reserve(current.size());
  for (auto v : current) list.push_back(v);
  std::sort(list.begin(), list.end());
  for (auto v : list) {
    out << std::hex << v << std::dec << "\n";
  }
  out << "=== END CURRENT SET ===\n\n";
  out.flush();
}


void Z3SolverImpl::loadUnsatCoreCache() {
  const char *solverDirEnv = std::getenv("KLEE_CACHE_DIR");
  if (!solverDirEnv) {
    klee_warning("KLEE_CACHE_DIR not set, skipping unsat core cache load");
    return;
  }

  std::string path = std::string(solverDirEnv);
  if (!path.empty() && path.back() == '/')
    path.pop_back();
  path += "/unsat_cores.txt";

  std::ifstream in(path);
  if (!in) {
    klee_warning("Failed to open unsat core cache file '%s'", path.c_str());
    return;
  }

  std::string line;
  while (std::getline(in, line)) {
    std::vector<uint64_t> ids;
    if (parseJsonHexIdList(line, ids)) {
      std::unordered_set<uint64_t> core;
      core.reserve(ids.size() * 2 + 1);
      for (auto v : ids) core.insert(v);
      if (!core.empty())
        cachedUnsatCores.push_back(std::move(core));
      continue;
    }

    auto first = line.find('[');
    auto last = line.rfind(']');
    if (first == std::string::npos || last == std::string::npos || last <= first)
      continue;
    std::string inner = line.substr(first + 1, last - first - 1);
    std::unordered_set<uint64_t> core;
    size_t pos = 0;
    while (true) {
      size_t s = inner.find('\'', pos);
      if (s == std::string::npos) break;
      size_t e = inner.find('\'', s + 1);
      if (e == std::string::npos) break;
      std::string elem = inner.substr(s + 1, e - (s + 1));
      elem = normalizeForPythonCoreMatch(elem);
      if (!elem.empty()) {
        uint64_t id = blake2b64_id(elem);
        core.insert(id);
      }
      pos = e + 1;
    }
    if (!core.empty())
      cachedUnsatCores.push_back(std::move(core));
  }

  klee_message("Loaded %zu unsat cores from '%s'",
               cachedUnsatCores.size(), path.c_str());
}

Z3SolverImpl::~Z3SolverImpl() {
  if (!unsatCoreHitLogPath.empty() && !coreHitCounts.empty()) {
    std::ofstream out(unsatCoreHitLogPath, std::ios::app);
    if (!out) {
      klee_warning("Failed to open UNSAT core hit log '%s'",
                   unsatCoreHitLogPath.c_str());
    } else {
      out << "=== UNSAT core hit summary ===\n";
      for (size_t i = 0; i < coreHitCounts.size(); ++i) {
        out << "UNSAT core #" << i << " hits: "
            << coreHitCounts[i] << "\n";
        for (const auto &expr : cachedUnsatCores[i]) {
          out << "  " << std::hex << expr << std::dec << "\n";
        }
        out << "\n";
      }
      out.flush();
    }
  }

  Z3_params_dec_ref(builder->ctx, solverParameters);
}

Z3Solver::Z3Solver() : Solver(std::make_unique<Z3SolverImpl>()) {}


int Z3SolverImpl::getImpliedCachedCoreIndex(::Z3_solver theSolver) {
  if (cachedUnsatCores.empty())
    return -1;

  std::string qStr = Z3_solver_to_string(builder->ctx, theSolver);
  std::vector<std::string> terms = extractAssertFormulasFromQuery(qStr);

  std::unordered_set<uint64_t> current;
  current.reserve(terms.size() * 2 + 1);
  for (auto &t : terms) {
    uint64_t id = blake2b64_id(t);
    current.insert(id);
  }

  for (size_t idx = 0; idx < cachedUnsatCores.size(); ++idx) {
    const auto &core = cachedUnsatCores[idx];
    bool included = true;
    for (const auto &cid : core) {
      if (current.find(cid) == current.end()) {
        included = false;
        break;
      }
    }
    if (included) {
      return static_cast<int>(idx);
    }
  }

  return -1;
}

std::string Z3Solver::getConstraintLog(const Query &query) {
  return impl->getConstraintLog(query);
}

void Z3Solver::setCoreSolverTimeout(time::Span timeout) {
  impl->setCoreSolverTimeout(timeout);
}

std::string Z3SolverImpl::getConstraintLog(const Query &query) {
  std::vector<Z3ASTHandle> assumptions;
  Z3Builder temp_builder(false, NULL);
  ConstantArrayFinder constant_arrays_in_query;
  for (auto const &constraint : query.constraints) {
    assumptions.push_back(temp_builder.construct(constraint));
    constant_arrays_in_query.visit(constraint);
  }

  Z3ASTHandle formula = Z3ASTHandle(
      Z3_mk_not(temp_builder.ctx, temp_builder.construct(query.expr)),
      temp_builder.ctx);
  constant_arrays_in_query.visit(query.expr);

  for (auto const &constant_array : constant_arrays_in_query.results) {
    assert(temp_builder.constant_array_assertions.count(constant_array) == 1 &&
           "Constant array found in query, but not handled by Z3Builder");
    for (auto const &arrayIndexValueExpr :
         temp_builder.constant_array_assertions[constant_array]) {
      assumptions.push_back(arrayIndexValueExpr);
    }
  }

  std::vector<::Z3_ast> raw_assumptions{assumptions.cbegin(),
                                        assumptions.cend()};
  ::Z3_string result = Z3_benchmark_to_smtlib_string(
      temp_builder.ctx,
      "Emited by klee::Z3SolverImpl::getConstraintLog()",
      "",
      "unknown",
      "",
      raw_assumptions.size(),
      raw_assumptions.size() ? raw_assumptions.data() : nullptr,
      formula);
  raw_assumptions.clear();
  assumptions.clear();
  formula = Z3ASTHandle(NULL, temp_builder.ctx);

  return {result};
}

bool Z3SolverImpl::computeTruth(const Query &query, bool &isValid) {
  bool hasSolution = false;
  bool status = internalRunSolver(query, NULL, NULL, hasSolution);
  isValid = !hasSolution;
  return status;
}

bool Z3SolverImpl::computeValue(const Query &query, ref<Expr> &result) {
  std::vector<const Array *> objects;
  std::vector<std::vector<unsigned char> > values;
  bool hasSolution;

  findSymbolicObjects(query.expr, objects);
  if (!computeInitialValues(query.withFalse(), objects, values, hasSolution))
    return false;
  if (!hasSolution) {
    klee_warning_once(0, "Z3SolverImpl::computeValue() called on UNSAT/UNKNOWN query; returning 0 to avoid crash");
    result = ConstantExpr::alloc(0, query.expr->getWidth());
    return true;
  }

  Assignment a(objects, values);
  result = a.evaluate(query.expr);

  return true;
}

bool Z3SolverImpl::computeInitialValues(
    const Query &query, const std::vector<const Array *> &objects,
    std::vector<std::vector<unsigned char> > &values, bool &hasSolution) {
  return internalRunSolver(query, &objects, &values, hasSolution);
}

bool Z3SolverImpl::internalRunSolver(
    const Query &query, const std::vector<const Array *> *objects,
    std::vector<std::vector<unsigned char> > *values, bool &hasSolution) {

  TimerStatIncrementer t(stats::queryTime);
  Z3_solver theSolver = Z3_mk_solver(builder->ctx);
  Z3_solver_inc_ref(builder->ctx, theSolver);
  Z3_solver_set_params(builder->ctx, theSolver, solverParameters);

  runStatusCode = SOLVER_RUN_STATUS_FAILURE;

  ConstantArrayFinder constant_arrays_in_query;
  for (auto const &constraint : query.constraints) {
    Z3_solver_assert(builder->ctx, theSolver, builder->construct(constraint));
    constant_arrays_in_query.visit(constraint);
  }
  ++stats::solverQueries;
  if (objects)
    ++stats::queryCounterexamples;

  Z3ASTHandle z3QueryExpr =
      Z3ASTHandle(builder->construct(query.expr), builder->ctx);
  constant_arrays_in_query.visit(query.expr);

  for (auto const &constant_array : constant_arrays_in_query.results) {
    assert(builder->constant_array_assertions.count(constant_array) == 1 &&
           "Constant array found in query, but not handled by Z3Builder");
    for (auto const &arrayIndexValueExpr :
         builder->constant_array_assertions[constant_array]) {
      Z3_solver_assert(builder->ctx, theSolver, arrayIndexValueExpr);
    }
  }
  Z3_solver_assert(
      builder->ctx, theSolver,
      Z3ASTHandle(Z3_mk_not(builder->ctx, z3QueryExpr), builder->ctx));
  int hitIndex = getImpliedCachedCoreIndex(theSolver);
  if (hitIndex >= 0) {
    hasSolution = false;
    runStatusCode = SOLVER_RUN_STATUS_SUCCESS_UNSOLVABLE;

    if (static_cast<size_t>(hitIndex) < coreHitCounts.size())
      coreHitCounts[hitIndex]++;
    {
      std::ofstream out(unsatCoreHitLogPath, std::ios::app);
      if (out) {
        out << "[HIT] core_index=" << hitIndex
            << " current_size=" << query.constraints.size() + 1
            << "\n\n";
      }
    }
    ++stats::queriesValid;

    Z3_solver_dec_ref(builder->ctx, theSolver);
    builder->clearConstructCache();
    return true;
  }

  if (dumpedQueriesFile) {
    *dumpedQueriesFile << "; start Z3 query\n";
    *dumpedQueriesFile << Z3_solver_to_string(builder->ctx, theSolver);
    *dumpedQueriesFile << "(check-sat)\n";
    *dumpedQueriesFile << "(reset)\n";
    *dumpedQueriesFile << "; end Z3 query\n\n";
    dumpedQueriesFile->flush();
  }

  ::Z3_lbool satisfiable = Z3_solver_check(builder->ctx, theSolver);
  runStatusCode = handleSolverResponse(theSolver, satisfiable, objects, values,
                                       hasSolution);

  Z3_solver_dec_ref(builder->ctx, theSolver);
  builder->clearConstructCache();

  if (runStatusCode == SolverImpl::SOLVER_RUN_STATUS_SUCCESS_SOLVABLE ||
      runStatusCode == SolverImpl::SOLVER_RUN_STATUS_SUCCESS_UNSOLVABLE) {
    if (hasSolution) {
      ++stats::queriesInvalid;
    } else {
      ++stats::queriesValid;
    }
    return true;
  }
  if (runStatusCode == SolverImpl::SOLVER_RUN_STATUS_INTERRUPTED) {
    raise(SIGINT);
  }
  return false;
}

SolverImpl::SolverRunStatus Z3SolverImpl::handleSolverResponse(
    ::Z3_solver theSolver, ::Z3_lbool satisfiable,
    const std::vector<const Array *> *objects,
    std::vector<std::vector<unsigned char> > *values, bool &hasSolution) {
  switch (satisfiable) {
  case Z3_L_TRUE: {
    hasSolution = true;
    if (!objects) {
      assert(values == NULL);
      return SolverImpl::SOLVER_RUN_STATUS_SUCCESS_SOLVABLE;
    }
    assert(values && "values cannot be nullptr");
    ::Z3_model theModel = Z3_solver_get_model(builder->ctx, theSolver);
    assert(theModel && "Failed to retrieve model");
    Z3_model_inc_ref(builder->ctx, theModel);
    values->reserve(objects->size());
    for (std::vector<const Array *>::const_iterator it = objects->begin(),
                                                    ie = objects->end();
         it != ie; ++it) {
      const Array *array = *it;
      std::vector<unsigned char> data;

      data.reserve(array->size);
      for (unsigned offset = 0; offset < array->size; offset++) {
        ::Z3_ast arrayElementExpr;
        Z3ASTHandle initial_read = builder->getInitialRead(array, offset);

        __attribute__((unused))
        bool successfulEval =
            Z3_model_eval(builder->ctx, theModel, initial_read, true, &arrayElementExpr);
        assert(successfulEval && "Failed to evaluate model");
        Z3_inc_ref(builder->ctx, arrayElementExpr);
        assert(Z3_get_ast_kind(builder->ctx, arrayElementExpr) ==
                   Z3_NUMERAL_AST &&
               "Evaluated expression has wrong sort");

        int arrayElementValue = 0;
        __attribute__((unused))
        bool successGet = Z3_get_numeral_int(builder->ctx, arrayElementExpr,
                                             &arrayElementValue);
        assert(successGet && "failed to get value back");
        assert(arrayElementValue >= 0 && arrayElementValue <= 255 &&
               "Integer from model is out of range");
        data.push_back(arrayElementValue);
        Z3_dec_ref(builder->ctx, arrayElementExpr);
      }
      values->push_back(data);
    }

    if (Z3ValidateModels) {
      bool success = validateZ3Model(theSolver, theModel);
      if (!success)
        abort();
    }

    Z3_model_dec_ref(builder->ctx, theModel);
    return SolverImpl::SOLVER_RUN_STATUS_SUCCESS_SOLVABLE;
  }
  case Z3_L_FALSE:
    hasSolution = false;
    return SolverImpl::SOLVER_RUN_STATUS_SUCCESS_UNSOLVABLE;
  case Z3_L_UNDEF: {
    ::Z3_string reason =
        ::Z3_solver_get_reason_unknown(builder->ctx, theSolver);
    if (strcmp(reason, "timeout") == 0 || strcmp(reason, "canceled") == 0 ||
        strcmp(reason, "(resource limits reached)") == 0) {
      return SolverImpl::SOLVER_RUN_STATUS_TIMEOUT;
    }
    if (strcmp(reason, "unknown") == 0) {
      return SolverImpl::SOLVER_RUN_STATUS_FAILURE;
    }
    if (strcmp(reason, "interrupted from keyboard") == 0) {
      return SolverImpl::SOLVER_RUN_STATUS_INTERRUPTED;
    }
    klee_warning("Unexpected solver failure. Reason is \"%s,\"\n", reason);
    abort();
  }
  default:
    llvm_unreachable("unhandled Z3 result");
  }
}

bool Z3SolverImpl::validateZ3Model(::Z3_solver &theSolver, ::Z3_model &theModel) {
  bool success = true;
  ::Z3_ast_vector constraints =
      Z3_solver_get_assertions(builder->ctx, theSolver);
  Z3_ast_vector_inc_ref(builder->ctx, constraints);

  unsigned size = Z3_ast_vector_size(builder->ctx, constraints);

  for (unsigned index = 0; index < size; ++index) {
    Z3ASTHandle constraint = Z3ASTHandle(
        Z3_ast_vector_get(builder->ctx, constraints, index), builder->ctx);

    ::Z3_ast rawEvaluatedExpr;
    __attribute__((unused))
    bool successfulEval =
        Z3_model_eval(builder->ctx, theModel, constraint, true, &rawEvaluatedExpr);
    assert(successfulEval && "Failed to evaluate model");

    Z3ASTHandle evaluatedExpr(rawEvaluatedExpr, builder->ctx);
    Z3SortHandle sort =
        Z3SortHandle(Z3_get_sort(builder->ctx, evaluatedExpr), builder->ctx);
    assert(Z3_get_sort_kind(builder->ctx, sort) == Z3_BOOL_SORT &&
           "Evaluated expression has wrong sort");

    Z3_lbool evaluatedValue =
        Z3_get_bool_value(builder->ctx, evaluatedExpr);
    if (evaluatedValue != Z3_L_TRUE) {
      llvm::errs() << "Validating model failed:\n"
                   << "The expression:\n";
      constraint.dump();
      llvm::errs() << "evaluated to \n";
      evaluatedExpr.dump();
      llvm::errs() << "But should be true\n";
      success = false;
    }
  }

  if (!success) {
    llvm::errs() << "Solver state:\n" << Z3_solver_to_string(builder->ctx, theSolver) << "\n";
    llvm::errs() << "Model:\n" << Z3_model_to_string(builder->ctx, theModel) << "\n";
  }

  Z3_ast_vector_dec_ref(builder->ctx, constraints);
  return success;
}

SolverImpl::SolverRunStatus Z3SolverImpl::getOperationStatusCode() {
  return runStatusCode;
}
}
#endif // ENABLE_Z3
