//===-- CexCachingSolver.cpp ----------------------------------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Solver/Solver.h"

#include "klee/ADT/MapOfSets.h"
#include "klee/Expr/Assignment.h"
#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Expr/ExprUtil.h"
#include "klee/Expr/ExprVisitor.h"
#include "klee/Support/OptionCategories.h"
#include "klee/Statistics/TimerStatIncrementer.h"
#include "klee/Solver/SolverImpl.h"
#include "klee/Solver/SolverStats.h"
#include "klee/Support/ErrorHandling.h"

#include "llvm/Support/CommandLine.h"

#include <array>
#include <memory>
#include <utility>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <bitset>
#include <cctype>
#include <cstdlib>
#include <chrono>
#include <vector>
#include <random>

#include <cstdint>
#include <cstring>
#include <openssl/sha.h>

#if __has_include(<filesystem>)
#  include <filesystem>
   namespace klee_fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
#  include <experimental/filesystem>
   namespace klee_fs = std::experimental::filesystem;
#else
#  define KLEE_NO_STD_FILESYSTEM 1
#endif

#include <zstd.h>

using namespace klee;
using namespace llvm;

namespace {
cl::opt<bool> DebugCexCacheCheckBinding(
    "debug-cex-cache-check-binding", cl::init(false),
    cl::desc("Debug the correctness of the counterexample "
             "cache assignments (default=false)"),
    cl::cat(SolvingCat));

cl::opt<bool>
    CexCacheTryAll("cex-cache-try-all", cl::init(false),
                   cl::desc("Try substituting all counterexamples before "
                            "asking the SMT solver (default=false)"),
                   cl::cat(SolvingCat));

cl::opt<bool>
    CexCacheSuperSet("cex-cache-superset", cl::init(false),
                     cl::desc("Try substituting SAT superset counterexample "
                              "before asking the SMT solver (default=false)"),
                     cl::cat(SolvingCat));

cl::opt<bool>
    DebugCexCacheSymbolCandidates("cex-cache-debug-symbol-candidates", cl::init(false),
                    cl::desc("Log number of disk-cache entries that share the same symbol set with a query"),
                    cl::cat(SolvingCat));

cl::opt<unsigned>
    CexCacheSameSymbolProbeLimit("cex-cache-same-symbol-probe-limit", cl::init(0),
      cl::desc("Max number of same-symbol SAT candidates to probe per miss (0 = unlimited)"),
      cl::cat(SolvingCat));

cl::opt<bool>
    CexCacheSameSymbolProbe("cex-cache-same-symbol-probe", cl::init(true),
      cl::desc("Probe SAT entries with the same symbol set after exact-key miss"),
      cl::cat(SolvingCat));
} // namespace

///

typedef std::set< ref<Expr> > KeyType;

struct AssignmentLessThan {
  bool operator()(const Assignment *a, const Assignment *b) const {
    return a->bindings < b->bindings;
  }
};


class CexCachingSolver : public SolverImpl {
  typedef std::set<Assignment*, AssignmentLessThan> assignmentsTable_ty;

  std::unique_ptr<Solver> solver;

  bool diskEnabled = true;
  bool diskVerbose = true;
  bool diskLogEnabled = false; // hit.count / solve_time.log

  inline void initDiskEnabled() {
    const char* e = std::getenv("KLEE_CEXCACHINGSOLVER_DISK");
    diskEnabled = (e && *e && std::strcmp(e,"0")!=0);
    const char* v = std::getenv("KLEE_CEXCACHINGSOLVER_VERBOSE");
    diskVerbose = (v && *v && std::strcmp(v,"0")!=0);
    const char* l = std::getenv("KLEE_CEXCACHINGSOLVER_LOG");
    diskLogEnabled = (l && *l && std::strcmp(l,"0")!=0);
  }
  inline bool isDiskEnabled() const noexcept { return diskEnabled; }
  inline bool isDiskLogEnabled() const noexcept { return diskEnabled && diskLogEnabled; }
  
  MapOfSets<ref<Expr>, Assignment*> cache;
  assignmentsTable_ty assignmentsTable;

  struct Arr32Hash {
    size_t operator()(const std::array<uint8_t,32>& a) const noexcept {
      uint64_t h = 1469598103934665603ull;              // FNV-1a 64-bit
      for (uint8_t b : a) { h ^= b; h *= 1099511628211ull; }
      return (size_t)h;
    }
  };
  
  struct DiskEntry {
    int8_t sat = 0;
    std::string constraints;
    std::string symbolsCSV;
    std::string symKey;
    std::string value;
  };
  struct Arr32Eq {
    bool operator()(const std::array<uint8_t,32>&a,const std::array<uint8_t,32>&b) const noexcept { return std::memcmp(a.data(), b.data(), 32)==0; }
  };
  struct OffsetInfo {
    uint64_t off = 0;
    uint8_t  ver = 0;
    int8_t   sat = 0;
  };
  static constexpr uint64_t kOverlayOff = ~uint64_t(0);
  struct CandRef {
    std::array<uint8_t,32> key{};
    uint64_t off = 0;
  };

  std::unordered_map<std::array<uint8_t,32>, OffsetInfo, Arr32Hash, Arr32Eq> diskIndex;
  std::unordered_map<std::array<uint8_t,32>, DiskEntry,  Arr32Hash, Arr32Eq> diskOverlay;
  std::unordered_set<std::array<uint8_t,32>, Arr32Hash, Arr32Eq> diskLoadedKeys;
  std::vector<std::pair<std::array<uint8_t,32>, DiskEntry>> diskAppendLog; 
  std::string cacheFilePath;
  bool diskCacheDirty = false;
  unsigned diskCacheHits = 0;

  std::unordered_map<std::array<uint8_t,32>, uint64_t, Arr32Hash> diskHitPerKeyLoaded;
  std::unordered_map<std::array<uint8_t,32>, uint64_t, Arr32Hash> diskHitPerKeyCreated;
  std::string diskHitCountPath;

  std::unordered_set<std::array<uint8_t,32>, Arr32Hash, Arr32Eq> diskKeyBlacklist;
  std::string blacklistPath;

  static inline void splitCSV(const std::string& s, std::vector<std::string>& out);
  static inline bool sameSymbolSet(const std::vector<std::string>& a,
                                   const std::vector<std::string>& b);
  static std::string canonicalSymKey(const std::string& csv);

  void collectDiskCandidatesSameSymbols(const std::vector<const Array*>& objects,
                                        std::vector<CandRef>& outRefs) const;

  void rankSameSymbolCandidates(const Query& query,
                                const std::vector<const Array*>& objects,
                                std::vector<CandRef>& candRefs) const;

  struct ExprNode {
    bool isAtom = true;
    std::string atom;
    std::string op;
    std::vector<ExprNode> args;
  };

  using ByteSet = std::bitset<256>;

  struct QueryValueProfile {
    std::unordered_set<unsigned> idxSet;
    std::unordered_map<unsigned, ByteSet> valMap;
    bool valid = false;
  };

  static void skipSpaces(const std::string& s, size_t& pos);
  static std::string parseToken(const std::string& s, size_t& pos);
  static bool parseExprNode(const std::string& s, size_t& pos, ExprNode& out);
  static bool parseConstraintList(const std::string& s, std::vector<ExprNode>& out);
  static bool isIntegerAtom(const std::string& s);
  static std::string reverseUnsignedOp(const std::string& op);
  static std::string reverseSignedOp(const std::string& op);
  static std::string negateUnsignedOp(const std::string& op);
  static std::string negateSignedOp(const std::string& op);
  static bool buildAtomicProfile(const std::string& op, unsigned idx, int64_t c, QueryValueProfile& out);
  static bool atomToInt64(const std::string& s, int64_t& out);
  static const ExprNode* unwrapCasts(const ExprNode* n);
  static bool extractReadIndex(const ExprNode& n, unsigned& idx);
  static bool extractConstValue(const ExprNode& n, int64_t& value);
  static ByteSet fullByteSet();
  static ByteSet singletonByteSet(unsigned v);
  static ByteSet rangeByteSetSigned(const std::string& op, int64_t c);
  static ByteSet rangeByteSetUnsigned(const std::string& op, int64_t c);
  static QueryValueProfile mergeAnd(const QueryValueProfile& a, const QueryValueProfile& b);
  static QueryValueProfile mergeOr(const QueryValueProfile& a, const QueryValueProfile& b);
  static QueryValueProfile negateProfile(const QueryValueProfile& p);
  static bool profileFromAtomicNegation(const ExprNode& n, QueryValueProfile& out);
  static QueryValueProfile profileFromExpr(const ExprNode& n);
  static QueryValueProfile buildQueryValueProfile(const std::string& constraintsText);
  static double computeProfileSimilarity(const QueryValueProfile& q, const QueryValueProfile& c);
  void handleBadDiskSatEntry(const std::array<uint8_t,32>& key, const DiskEntry& ent);

  bool trySatisfyFromSameSymbolCandidates(const KeyType& key,
                                          const std::vector<const Array*>& objects,
                                          const std::vector<CandRef>& candRefs,
                                          Assignment *&result);

  std::unordered_map<std::string, std::vector<CandRef>> symIndex;
  static constexpr uint8_t kFlagRaw  = 0x00;
  static constexpr uint8_t kFlagZstd = 0x01;

  static inline uint64_t to_le64(uint64_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return __builtin_bswap64(v);
#endif
  }
  static inline uint32_t to_le32(uint32_t v) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return v;
#else
    return __builtin_bswap32(v);
#endif
  }
  static inline uint64_t from_le64(uint64_t v){ return to_le64(v); }
  static inline uint32_t from_le32(uint32_t v){ return to_le32(v); }

  bool readDiskEntryAt(uint64_t off, DiskEntry& out) const;

  static inline std::string joinPath(const std::string& base, const char* name) {
    if (base.empty()) return std::string(name);
    if (base.back()=='/') return base + name;
    return base + "/" + name;
  }

  static inline std::string maybeCompress(const std::string& s, uint8_t& outFlags) {
    outFlags = kFlagZstd;
    size_t cap = ZSTD_compressBound(s.size());
    std::string out; out.resize(cap);
    size_t n = ZSTD_compress(out.data(), cap, s.data(), s.size(), /*level=*/3);
    if (ZSTD_isError(n) || n==0 || n >= s.size()) { outFlags = kFlagRaw; return s; }
    out.resize(n); return out;
  }
  static inline bool maybeDecompress(const std::string& in, uint8_t flags, std::string& out) {
    if (!(flags & kFlagZstd)) { out = in; return true; }
    unsigned long long sz = ZSTD_getFrameContentSize(in.data(), in.size());
    if (sz == ZSTD_CONTENTSIZE_ERROR) return false;
    if (sz == ZSTD_CONTENTSIZE_UNKNOWN) {
      size_t cap = in.size()*4 + 64;
      std::string buf(cap, '\0');
      size_t n = ZSTD_decompress(buf.data(), buf.size(), in.data(), in.size());
      if (ZSTD_isError(n)) return false;
      buf.resize(n); out.swap(buf); return true;
    }
    out.resize((size_t)sz);
    size_t n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    return !ZSTD_isError(n);
  }

  static inline std::string toHex32(const std::array<uint8_t,32>& h) {
    static const char* kHex = "0123456789abcdef";
    std::string s; s.resize(64);
    for (size_t i=0;i<32;++i) {
      s[2*i]   = kHex[(h[i] >> 4) & 0xF];
      s[2*i+1] = kHex[h[i] & 0xF];
    }
    return s;
  }

  static inline bool parseHex32(const std::string& hex, std::array<uint8_t,32>& out) {
    auto hexval = [](char c)->int{
      if ('0'<=c && c<='9') return c-'0';
      if ('a'<=c && c<='f') return c-'a'+10;
      if ('A'<=c && c<='F') return c-'A'+10;
      return -1;
    };
    if (hex.size() != 64) return false;
    for (size_t i=0;i<32;++i) {
      int hi = hexval(hex[2*i]);
      int lo = hexval(hex[2*i+1]);
      if (hi<0 || lo<0) return false;
      out[i] = static_cast<uint8_t>((hi<<4) | lo);
    }
    return true;
  }

  void loadExistingHitCounts(std::unordered_map<std::array<uint8_t,32>, uint64_t, Arr32Hash>& acc) {
    if (diskHitCountPath.empty()) return;
    std::ifstream fin(diskHitCountPath);
    if (!fin.good()) return;
    std::string line;
    auto hexval = [](char c)->int{
      if ('0'<=c && c<='9') return c-'0';
      if ('a'<=c && c<='f') return c-'a'+10;
      if ('A'<=c && c<='F') return c-'A'+10;
      return -1;
    };
    while (std::getline(fin, line)) {
      if (line.size() < 66) continue;
      size_t sp = line.find(' ');
      if (sp == std::string::npos) continue;
      std::string hex = line.substr(0, sp);
      std::string cnt = line.substr(sp+1);
      if (hex.size() != 64) continue;
      std::array<uint8_t,32> h{};
      bool ok = true;
      for (size_t i=0;i<32;++i) {
        int hi = hexval(hex[2*i]);
        int lo = hexval(hex[2*i+1]);
        if (hi<0 || lo<0) { ok=false; break; }
        h[i] = static_cast<uint8_t>((hi<<4) | lo);
      }
      if (!ok) continue;
      char* endp=nullptr;
      unsigned long long v = std::strtoull(cnt.c_str(), &endp, 10);
      if (endp==cnt.c_str()) continue;
      acc[h] += static_cast<uint64_t>(v);
    }
  }

  void loadBlacklist() {
    diskKeyBlacklist.clear();
    if (blacklistPath.empty())
      return;
    std::ifstream fin(blacklistPath);
    if (!fin.good()) return;
    std::string line;
    while (std::getline(fin, line)) {
      while (!line.empty() && (line.back()=='\r' || line.back()=='\n' || line.back()==' ' || line.back()=='\t')) line.pop_back();
      std::array<uint8_t,32> h{};
      if (parseHex32(line, h)) diskKeyBlacklist.insert(h);
    }
  }

  void saveHitCountsAtomically(const std::unordered_map<std::array<uint8_t,32>, uint64_t, Arr32Hash>& acc) {
    if (diskHitCountPath.empty()) return;
    const std::string tmpPath = diskHitCountPath + ".tmp";
    {
      std::ofstream fout(tmpPath, std::ios::out | std::ios::trunc);
      if (!fout.good()) return;
      for (const auto& kv : acc) {
        fout << toHex32(kv.first) << ' ' << kv.second << '\n';
      }
      fout.flush();
      if (!fout.good()) return;
    }
#ifndef KLEE_NO_STD_FILESYSTEM
    std::error_code ec;
    klee_fs::rename(tmpPath, diskHitCountPath, ec);
    if (ec) {
      std::ofstream fout(diskHitCountPath, std::ios::out | std::ios::trunc);
      if (!fout.good()) return;
      for (const auto& kv : acc) fout << toHex32(kv.first) << ' ' << kv.second << '\n';
      std::error_code ec2; klee_fs::remove(tmpPath, ec2);
    }
#else
    std::ofstream fout(diskHitCountPath, std::ios::out | std::ios::trunc);
    if (!fout.good()) return;
    for (const auto& kv : acc) fout << toHex32(kv.first) << ' ' << kv.second << '\n';
#endif
  }

  inline void recordDiskEntryHit(const std::array<uint8_t,32>& key, bool cameFromDiskFile) {
    if (cameFromDiskFile)
      ++diskHitPerKeyLoaded[key];
    else
      ++diskHitPerKeyCreated[key];
    if (solveTimeLoggedKeys.insert(key).second) {
      queryTimeLog.emplace_back(key, 0);
    }
  }

  void dumpDiskCache();
  void logDiskHit();
  bool diskCacheLookupCounted(const std::array<uint8_t,32>& key,
                              DiskEntry& out,
                              bool& cameFromDiskFile);
  void diskCacheInsertOrUpdateCounted(const std::array<uint8_t,32>& key,
                                      const DiskEntry& entry);
  uint64_t opHash = 0;
  uint64_t opLookup = 0;
  uint64_t opCompare = 0;
  uint64_t timeHashUs    = 0;
  uint64_t timeLookupUs  = 0;
  uint64_t timeCompareUs = 0;

  std::string queryTimePath;
  std::vector<std::pair<std::array<uint8_t,32>, uint64_t>> queryTimeLog;
  std::unordered_set<std::array<uint8_t,32>, Arr32Hash, Arr32Eq> solveTimeLoggedKeys;
  inline void recordSolveTime(const std::array<uint8_t,32>& key, uint64_t micros) {
    queryTimeLog.emplace_back(key, micros);
    solveTimeLoggedKeys.insert(key);
  }

  inline bool countingEnabled() const noexcept {
    return isDiskEnabled();
  }

  static inline void sha256_compute(const uint8_t* data, size_t len, uint8_t out32[32]) {
    SHA256(data, len, out32);
  }
  static inline std::array<uint8_t,32> sha256_of_string(const std::string& s) {
    std::array<uint8_t,32> out{};
    sha256_compute(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out.data());
    return out;
  }

  inline std::array<uint8_t,32> makeHashedKeyCounted(const Query& query, std::string* outText=nullptr) {
    auto t0 = std::chrono::steady_clock::now();
    const std::string diskKeyText = makeDiskKeyFromQuery(query);
    if (outText) *outText = diskKeyText;
    if (countingEnabled()) ++opHash;
    auto out = sha256_of_string(diskKeyText);
    auto t1 = std::chrono::steady_clock::now();
    timeHashUs += (uint64_t)
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    return out;
  }

  static std::string oneLine(std::string s) {
    std::string out; out.reserve(s.size());
    bool inWS=false;
    for (char ch: s) {
      if (ch==' '||ch=='\t'||ch=='\r'||ch=='\n') {
        if (!inWS) { out.push_back(' '); inWS=true; }
      } else { out.push_back(ch); inWS=false; }
    }
    while (!out.empty() && out.front()==' ') out.erase(out.begin());
    while (!out.empty() && out.back ()==' ') out.pop_back();
    return out;
  }

  static inline std::string joinComma(const std::vector<std::string>& v) {
    std::ostringstream oss;
    for (size_t i=0;i<v.size();++i) { if (i) oss << ','; oss << v[i]; }
    return oss.str();
  }

  static inline void extractSymbolsFromObjects(const std::vector<const Array*>& objects,
                                               std::vector<std::string>& out) {
    out.clear(); out.reserve(objects.size());
    std::unordered_set<std::string> seen;
    for (const Array* A : objects) if (seen.insert(A->name).second) out.push_back(A->name);
  }

  std::string makeDiskKeyFromQuery(const Query& query) const {
    std::vector<std::string> parts;
    parts.reserve(query.constraints.size()+1);
    for (auto &c : query.constraints) {
      std::string cs; llvm::raw_string_ostream os(cs); c->print(os); os.flush();
      parts.push_back(oneLine(std::move(cs)));
    }
    ref<Expr> neg = Expr::createIsZero(query.expr);
    if (!(isa<ConstantExpr>(neg) && cast<ConstantExpr>(neg)->isFalse())) {
      std::string ns; llvm::raw_string_ostream os(ns); neg->print(os); os.flush();
      parts.push_back(oneLine(std::move(ns)));
    }
    std::sort(parts.begin(), parts.end());
    std::string key = "C:[";
    for (size_t i=0;i<parts.size();++i) {
      if (i) key += "; ";
      key += parts[i];
    }
    key += "]";
    return key;
  }

  static std::string hexFromBytes(const std::vector<unsigned char>& v) {
    static const char* h="0123456789abcdef";
    std::string out; out.resize(v.size()*2);
    for (size_t i=0;i<v.size();++i){ unsigned c=v[i]; out[2*i]=h[(c>>4)&0xF]; out[2*i+1]=h[c&0xF]; }
    return out;
  }

  static bool bytesFromHex(const std::string& hex, std::vector<unsigned char>& out) {
    auto val=[](char ch)->int{
      if ('0'<=ch&&ch<='9') return ch-'0';
      if ('a'<=ch&&ch<='f') return ch-'a'+10;
      if ('A'<=ch&&ch<='F') return ch-'A'+10;
      return -1;
    };
    if (hex.size()%2) return false;
    out.clear(); out.reserve(hex.size()/2);
    for (size_t i=0;i<hex.size();i+=2){
      int hi=val(hex[i]), lo=val(hex[i+1]);
      if (hi<0||lo<0) return false;
      out.push_back(static_cast<unsigned char>((hi<<4)|lo));
    }
    return true;
  }

  static std::string serializeAssignment(const Assignment* a) {
    std::string out;
    bool first=true;
    for (auto &kv : a->bindings) {
      const Array* arr = kv.first;
      const auto& bytes = kv.second;
      if (!first) out.push_back(',');
      first=false;
      out += arr->name;
      out.push_back(':');
      out += std::to_string(arr->size);
      out.push_back(':');
      out += hexFromBytes(bytes);
    }
    if (out.empty()) out = "EMPTY";
    return out;
  }

  static bool deserializeAssignment(const std::string& payload,
                                    const std::vector<const Array*>& objects,
                                    std::unique_ptr<Assignment>& outA) {
    std::unordered_map<std::string,std::pair<unsigned,std::vector<unsigned char>>> entries;
    if (payload!="EMPTY") {
      size_t pos=0;
      while (pos<payload.size()) {
        size_t comma = payload.find(',', pos);
        std::string item = payload.substr(pos, comma==std::string::npos ? std::string::npos : comma-pos);
        pos = (comma==std::string::npos) ? payload.size() : comma+1;
        if (item.empty()) continue;
        size_t c1=item.find(':'); if (c1==std::string::npos) continue;
        size_t c2=item.find(':', c1+1); if (c2==std::string::npos) continue;
        std::string name = item.substr(0,c1);
        unsigned size = (unsigned)std::strtoul(item.substr(c1+1, c2-(c1+1)).c_str(), nullptr, 10);
        std::string hex = item.substr(c2+1);
        std::vector<unsigned char> bytes;
        if (!bytesFromHex(hex, bytes)) continue;
        entries[name] = {size, std::move(bytes)};
      }
    }
    
    std::vector<std::vector<unsigned char>> values(objects.size());
    for (size_t i=0;i<objects.size();++i) {
      const Array* A = objects[i];
      auto it = entries.find(A->name);
      if (it==entries.end()) {
        values[i] = std::vector<unsigned char>(A->size, 0);
      } else {
        auto& bytes = it->second.second;
        values[i] = std::vector<unsigned char>(A->size, 0);
        size_t n = std::min<size_t>(A->size, bytes.size());
        std::copy(bytes.begin(), bytes.begin()+n, values[i].begin());
      }
    }
    outA.reset(new Assignment(objects, values));
    return true;
  }

  static constexpr uint8_t kDiskRecVersion3 = 3;
  static inline void writeU32(std::ofstream& f, uint32_t v) {
    unsigned char b[4] = {
      (unsigned char)(v & 0xFF),
      (unsigned char)((v>>8) & 0xFF),
      (unsigned char)((v>>16)& 0xFF),
      (unsigned char)((v>>24)& 0xFF)
    };
    f.write((const char*)b, 4);
  }

  static inline bool readU32(std::ifstream& f, uint32_t& v) {
    unsigned char b[4];
    if (!f.read((char*)b,4)) return false;
    v = (uint32_t)b[0] | ((uint32_t)b[1]<<8) | ((uint32_t)b[2]<<16) | ((uint32_t)b[3]<<24);
    return true;
  }

  static inline void writeOneRecordV3(std::ofstream& fout,
                                      const std::array<uint8_t,32>& key,
                                      const DiskEntry& e) {
    const char ver_c = (char)kDiskRecVersion3;
    const char sat_c = (char)e.sat;
    fout.write(&ver_c,1);
    fout.write(&sat_c,1);
    fout.write((const char*)key.data(), 32);
    writeU32(fout, (uint32_t)e.constraints.size());
    if (!e.constraints.empty())
      fout.write(e.constraints.data(), (std::streamsize)e.constraints.size());
    writeU32(fout, (uint32_t)e.symbolsCSV.size());
    if (!e.symbolsCSV.empty())
      fout.write(e.symbolsCSV.data(), (std::streamsize)e.symbolsCSV.size());
    uint8_t vFlags = kFlagRaw;
    std::string vPayload = e.value;
    if (e.sat && !e.value.empty()) {
      vPayload = maybeCompress(e.value, vFlags);
    }
    const char fg_c = (char)vFlags;
    fout.write(&fg_c,1);
    writeU32(fout, (uint32_t)vPayload.size());
    if (!vPayload.empty())
      fout.write(vPayload.data(), (std::streamsize)vPayload.size());
  }
  static inline bool readOneRecordV3(std::ifstream& fin,
                                     std::array<uint8_t,32>& key,
                                     DiskEntry& out) {
    char sc[1];
    if (!fin.read(sc,1)) return false;
    out.sat = (int8_t)sc[0];
    if (!fin.read((char*)key.data(), 32)) return false;
    uint32_t cLen=0, sLen=0, vLen=0; uint8_t vFlags=0;
    if (!readU32(fin, cLen)) return false;
    out.constraints.clear();
    if (cLen) {
      out.constraints.resize(cLen);
      if (!fin.read(&out.constraints[0], (std::streamsize)cLen)) return false;
    }
    if (!readU32(fin, sLen)) return false;
    out.symbolsCSV.clear();
    if (sLen) {
      out.symbolsCSV.resize(sLen);
      if (!fin.read(&out.symbolsCSV[0], (std::streamsize)sLen)) return false;
    }
    char fg_c[1]; if (!fin.read(fg_c,1)) return false;
    vFlags = (uint8_t)fg_c[0];
    if (!readU32(fin, vLen)) return false;
    std::string vIn; vIn.clear();
    if (vLen) {
      vIn.resize(vLen);
      if (!fin.read(&vIn[0], (std::streamsize)vLen)) return false;
    }
    if (out.sat==0) { out.value.clear(); return true; }
    if (vFlags & kFlagZstd) {
      std::string dec;
      if (!maybeDecompress(vIn, vFlags, dec)) out.value = std::move(vIn);
      else out.value = std::move(dec);
    } else {
      out.value = std::move(vIn);
    }
    return true;
  }

    void loadDiskCache() {
      diskIndex.clear(); diskOverlay.clear(); symIndex.clear(); diskLoadedKeys.clear(); diskAppendLog.clear(); diskCacheDirty=false;
      const char* dir = std::getenv("KLEE_CACHE_DIR");
      if (dir && *dir) {
        std::ostringstream oss; oss<<dir; if (oss.str().back()!='/') oss<<'/'; oss<<"cex_cache.bin";
        cacheFilePath = oss.str();
      } else {
        cacheFilePath = "klee_cex_cache.bin";
      }
      std::string base = ".";
      auto pos = cacheFilePath.find_last_of('/');
      if (pos != std::string::npos) base = cacheFilePath.substr(0,pos);
      blacklistPath = joinPath(base, "blacklist.txt");
  #ifndef KLEE_NO_STD_FILESYSTEM
      { std::error_code ec; klee_fs::create_directories(base, ec); }
  #endif
      loadBlacklist();
      {
        std::ifstream fin(cacheFilePath, std::ios::in | std::ios::binary);
        if (fin.good()) {
          fin.seekg(0, std::ios::beg);
          while (true) {
            std::streampos pos = fin.tellg();
            char ver_c;
            if (!fin.read(&ver_c, 1)) break;
            const uint8_t ver = (uint8_t)ver_c;
            fin.seekg(pos, std::ios::beg);

            if (ver == kDiskRecVersion3) {
              char tmp;
              if (!fin.read(&tmp,1)) break;
              const uint64_t off = (uint64_t)pos;
              DiskEntry ent;
              std::array<uint8_t,32> key;
              if (!readOneRecordV3(fin, key, ent)) break;
              if (diskKeyBlacklist.find(key) != diskKeyBlacklist.end()) continue;
              diskIndex[key] = OffsetInfo{off, kDiskRecVersion3, ent.sat};
              diskLoadedKeys.insert(key);
              if (!ent.symbolsCSV.empty()) {
                const std::string sk = canonicalSymKey(ent.symbolsCSV);
                if (!sk.empty()) symIndex[sk].push_back(CandRef{key, off});
              }
              continue;
            }
            break;
          }
        }
      }
    }

  bool searchForAssignment(KeyType &key, 
                           Assignment *&result);
  
  bool lookupAssignment(const Query& query, KeyType &key, Assignment *&result);

  bool lookupAssignment(const Query& query, Assignment *&result) {
    KeyType key;
    return lookupAssignment(query, key, result);
  }

  bool getAssignment(const Query& query, Assignment *&result);
  
public:
  CexCachingSolver(std::unique_ptr<Solver> solver)
      : solver(std::move(solver)) {
    initDiskEnabled();
    if (isDiskEnabled()) loadDiskCache();
  }
  ~CexCachingSolver();
  
  bool computeTruth(const Query&, bool &isValid);
  bool computeValidity(const Query&, Solver::Validity &result);
  bool computeValue(const Query&, ref<Expr> &result);
  bool computeInitialValues(const Query&,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution);
  SolverRunStatus getOperationStatusCode();
  std::string getConstraintLog(const Query& query) override;
  void setCoreSolverTimeout(time::Span timeout);
};

///

struct NullAssignment {
  bool operator()(Assignment *a) const { return !a; }
};

struct NonNullAssignment {
  bool operator()(Assignment *a) const { return a!=0; }
};

struct NullOrSatisfyingAssignment {
  KeyType &key;
  
  NullOrSatisfyingAssignment(KeyType &_key) : key(_key) {}

  bool operator()(Assignment *a) const { 
    return !a || a->satisfies(key.begin(), key.end()); 
  }
};

/// searchForAssignment - Look for a cached solution for a query.
///
/// \param key - The query to look up.
/// \param result [out] - The cached result, if the lookup is successful. This is
/// either a satisfying assignment (for a satisfiable query), or 0 (for an
/// unsatisfiable query).
/// \return - True if a cached result was found.
bool CexCachingSolver::searchForAssignment(KeyType &key, Assignment *&result) {
  Assignment * const *lookup = cache.lookup(key);
  if (lookup) {
    result = *lookup;
    return true;
  }

  if (CexCacheTryAll) {
    // Look for a satisfying assignment for a superset, which is trivially an
    // assignment for any subset.
    Assignment **lookup = 0;
    if (CexCacheSuperSet)
      lookup = cache.findSuperset(key, NonNullAssignment());

    // Otherwise, look for a subset which is unsatisfiable, see below.
    if (!lookup) 
      lookup = cache.findSubset(key, NullAssignment());

    // If either lookup succeeded, then we have a cached solution.
    if (lookup) {
      result = *lookup;
      return true;
    }

    // Otherwise, iterate through the set of current assignments to see if one
    // of them satisfies the query.
    for (assignmentsTable_ty::iterator it = assignmentsTable.begin(), 
           ie = assignmentsTable.end(); it != ie; ++it) {
      Assignment *a = *it;
      if (a->satisfies(key.begin(), key.end())) {
        result = a;
        return true;
      }
    }
  } else {
    // FIXME: Which order? one is sure to be better.

    // Look for a satisfying assignment for a superset, which is trivially an
    // assignment for any subset.
    Assignment **lookup = 0;
    if (CexCacheSuperSet)
      lookup = cache.findSuperset(key, NonNullAssignment());

    // Otherwise, look for a subset which is unsatisfiable -- if the subset is
    // unsatisfiable then no additional constraints can produce a valid
    // assignment. While searching subsets, we also explicitly the solutions for
    // satisfiable subsets to see if they solve the current query and return
    // them if so. This is cheap and frequently succeeds.
    if (!lookup) 
      lookup = cache.findSubset(key, NullOrSatisfyingAssignment(key));

    // If either lookup succeeded, then we have a cached solution.
    if (lookup) {
      result = *lookup;
      return true;
    }
  }
  
  return false;
}

/// lookupAssignment - Lookup a cached result for the given \arg query.
///
/// \param query - The query to lookup.
/// \param key [out] - On return, the key constructed for the query.
/// \param result [out] - The cached result, if the lookup is successful. This is
/// either a satisfying assignment (for a satisfiable query), or 0 (for an
/// unsatisfiable query).
/// \return True if a cached result was found.
bool CexCachingSolver::lookupAssignment(const Query &query, 
                                        KeyType &key,
                                        Assignment *&result) {
  key = KeyType(query.constraints.begin(), query.constraints.end());
  ref<Expr> neg = Expr::createIsZero(query.expr);
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(neg)) {
    if (CE->isFalse()) {
      result = (Assignment*) 0;
      ++stats::queryCexCacheHits;
      return true;
    }
  } else {
    key.insert(neg);
  }

  bool found = searchForAssignment(key, result);
  if (found) {
    ++stats::queryCexCacheHits;
    if (isDiskLogEnabled()) {
      const auto diskKey = makeHashedKeyCounted(query);
      recordDiskEntryHit(diskKey, false);
    }
    return true;
  } else {
    const auto diskKey = makeHashedKeyCounted(query);
    DiskEntry dent;
    bool cameFromDisk = false;
    if (diskKeyBlacklist.find(diskKey) != diskKeyBlacklist.end()) {
      ++stats::queryCexCacheMisses;
      return false;
    }
    if (diskCacheLookupCounted(diskKey, dent, cameFromDisk)) {
      std::vector<const Array*> objects;
      findSymbolicObjects(key.begin(), key.end(), objects);
      std::unique_ptr<Assignment> rebuilt;
      if (!dent.sat) {
        result = (Assignment*)0;
        cache.insert(key, result);
        if (cameFromDisk) logDiskHit();
        if (isDiskLogEnabled()) recordDiskEntryHit(diskKey, cameFromDisk);
        ++stats::queryCexCacheHits;
        return true;
      } else if (deserializeAssignment(dent.value, objects, rebuilt) && rebuilt) {
        if (!rebuilt->satisfies(key.begin(), key.end())) {
          handleBadDiskSatEntry(diskKey, dent);
        } else {
          Assignment* binding = rebuilt.release();
          auto ins = assignmentsTable.insert(binding);
          if (!ins.second) { delete binding; binding = *ins.first; }
          cache.insert(key, binding);
          result = binding;
          if (cameFromDisk) logDiskHit();
          if (isDiskLogEnabled()) recordDiskEntryHit(diskKey, cameFromDisk);
          ++stats::queryCexCacheHits;
          return true;
        }
      }
    }

    {
      std::vector<const Array*> objectsProbe;
      findSymbolicObjects(key.begin(), key.end(), objectsProbe);
      std::vector<CandRef> sameSymRefs;
      collectDiskCandidatesSameSymbols(objectsProbe, sameSymRefs);
      if (DebugCexCacheSymbolCandidates) {
        llvm::errs() << "[CexCachingSolver] same-symbol candidates: "
                     << sameSymRefs.size() << "\n";
      }

      {
        static std::mt19937 rng(std::random_device{}());
        std::shuffle(sameSymRefs.begin(), sameSymRefs.end(), rng);
      }

      if (CexCacheSameSymbolProbe && !sameSymRefs.empty()) {
        Assignment* satFromCands = nullptr;
        if (trySatisfyFromSameSymbolCandidates(key, objectsProbe, sameSymRefs, satFromCands)) {
          result = satFromCands;
          return true;
        }
      }
    }
    ++stats::queryCexCacheMisses;
  }
  return found;
}

bool CexCachingSolver::getAssignment(const Query& query, Assignment *&result) {
  KeyType key;
  if (lookupAssignment(query, key, result))
    return true;

  std::vector<const Array*> objects;
  findSymbolicObjects(key.begin(), key.end(), objects);

  std::vector< std::vector<unsigned char> > values;
  bool hasSolution;
  auto t0 = std::chrono::steady_clock::now();
  if (!solver->impl->computeInitialValues(query, objects, values,
                                          hasSolution))
    return false;
  
  auto t1 = std::chrono::steady_clock::now();
  uint64_t usSolve = (uint64_t)
      std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  Assignment *binding;
  if (hasSolution) {
    binding = new Assignment(objects, values);
    std::pair<assignmentsTable_ty::iterator, bool>
      res = assignmentsTable.insert(binding);
    if (!res.second) {
      delete binding;
      binding = *res.first;
    }
    
    if (DebugCexCacheCheckBinding)
      if (!binding->satisfies(key.begin(), key.end())) {
        query.dump();
        binding->dump();
        klee_error("Generated assignment doesn't match query");
      }
  } else {
    binding = (Assignment*) 0;
  }
  
  result = binding;
  cache.insert(key, binding);

  std::string keyText;
  const auto diskKey = makeHashedKeyCounted(query, &keyText);
  DiskEntry ent;
  ent.sat = (binding!=nullptr) ? 1 : 0;
  ent.constraints = keyText;
  std::vector<std::string> syms;
  extractSymbolsFromObjects(objects, syms);
  ent.symbolsCSV = joinComma(syms);
  ent.symKey = canonicalSymKey(ent.symbolsCSV);
  ent.value = ent.sat ? serializeAssignment(binding) : std::string();

  if (diskKeyBlacklist.find(diskKey) != diskKeyBlacklist.end()) {
    llvm::errs() << "[Increase] Skipping disk-cache (blacklisted): "
                  << toHex32(diskKey) << "\n";
  } else {
    diskCacheInsertOrUpdateCounted(diskKey, ent);
  }
  recordSolveTime(diskKey, usSolve);
  return true;
}

///

CexCachingSolver::~CexCachingSolver() {
  if (isDiskEnabled()) dumpDiskCache();
  cache.clear();

  const char* outDirEnv = std::getenv("KLEE_OUTPUT_DIR");
  std::string outDir = outDirEnv && *outDirEnv ? std::string(outDirEnv)
                                               : std::string("klee-last");

  std::ostringstream resultPath;
  resultPath << outDir << "/cex_cache_result.txt";
  diskHitCountPath = outDir + std::string("/cex_cache_hit.count");
  queryTimePath    = outDir + std::string("/cex_cache_solve_time.log");
  llvm::errs() << "[CexCachingSolver] Writing disk cache result to: "
               << resultPath.str() << "\n";

  std::ofstream fout(resultPath.str(), std::ios::out | std::ios::trunc);
  if (fout.good()) {
    fout << "=== CexCachingSolver Disk Cache Summary ===" << std::endl;
    fout << "File: " << cacheFilePath << std::endl;
    fout << "Disk cache hits: " << diskCacheHits << std::endl;
    fout << "Total cached entries: " << (diskIndex.size() + diskOverlay.size()) << std::endl;
    fout << "Record format: [ver=3][sat i8][hash32]"
            "[cLen u32][constraints][sLen u32][symbolsCSV]"
            "[vFlags u8][vLen u32][value]"
          << std::endl;
    fout << "Loaded entries (from disk): " << diskLoadedKeys.size() << std::endl;
    fout << "Created this run          : " << diskOverlay.size() << std::endl;
    fout << "Op(Hash)    : " << opHash << std::endl;
    fout << "Op(Lookup)  : " << opLookup << std::endl;
    fout << "Op(Compare) : " << opCompare << std::endl;
    fout << "Time(Hash) us    : " << timeHashUs << std::endl;
    fout << "Time(Lookup) us  : " << timeLookupUs << std::endl;
    fout << "Time(Compare) us : " << timeCompareUs << std::endl;
    fout.close();
  }

  if (isDiskLogEnabled()) {
    std::unordered_map<std::array<uint8_t,32>, uint64_t, Arr32Hash> acc;
    loadExistingHitCounts(acc);

    for (const auto &kv : acc) {
      if (solveTimeLoggedKeys.insert(kv.first).second) {
        queryTimeLog.emplace_back(kv.first, 0);
      }
    }
    if (!queryTimeLog.empty() && !queryTimePath.empty()) {
      std::ofstream qf(queryTimePath, std::ios::out | std::ios::app);
      if (qf.good()) {
        for (const auto &kv2 : queryTimeLog) {
          qf << toHex32(kv2.first) << ' ' << kv2.second << '\n';
        }
        qf.flush();
      }
    }
    for (const auto& kv : diskHitPerKeyLoaded)
      acc[kv.first] += kv.second;
    for (const auto& kv : diskHitPerKeyCreated)
      acc[kv.first] += kv.second;
    saveHitCountsAtomically(acc);
  }

  for (assignmentsTable_ty::iterator it = assignmentsTable.begin(),
        ie = assignmentsTable.end(); it != ie; ++it)
    delete *it;
}

bool CexCachingSolver::computeValidity(const Query& query,
                                       Solver::Validity &result) {
  TimerStatIncrementer t(stats::cexCacheTime);
  Assignment *a;
  if (!getAssignment(query.withFalse(), a))
    return false;
  if (!a) {
    llvm::errs() << "[CexCachingSolver][WARN] computeValidity(): "
                 << "no assignment for query.withFalse() (constraints UNSAT or bad cache hit). "
                 << "Falling back to core solver.\n";
    return solver->impl->computeValidity(query, result);
  }
  ref<Expr> q = a->evaluate(query.expr);
  assert(isa<ConstantExpr>(q) && 
         "assignment evaluation did not result in constant");

  if (cast<ConstantExpr>(q)->isTrue()) {
    if (!getAssignment(query, a))
      return false;
    result = !a ? Solver::True : Solver::Unknown;
  } else {
    if (!getAssignment(query.negateExpr(), a))
      return false;
    result = !a ? Solver::False : Solver::Unknown;
  }
  
  return true;
}

bool CexCachingSolver::computeTruth(const Query& query,
                                    bool &isValid) {
  TimerStatIncrementer t(stats::cexCacheTime);

  Assignment *a;
  if (!getAssignment(query, a))
    return false;

  isValid = !a;

  return true;
}

bool CexCachingSolver::computeValue(const Query& query,
                                    ref<Expr> &result) {
  TimerStatIncrementer t(stats::cexCacheTime);

  Assignment *a;
  if (!getAssignment(query.withFalse(), a))
    return false;
  if (!a) {
    llvm::errs() << "[CexCachingSolver][WARN] computeValue(): "
                 << "no assignment for query.withFalse() (constraints UNSAT or bad cache hit). "
                 << "Falling back to core solver.\n";
    return solver->impl->computeValue(query, result);
  }
  result = a->evaluate(query.expr);  
  assert(isa<ConstantExpr>(result) && 
         "assignment evaluation did not result in constant");
  return true;
}

bool 
CexCachingSolver::computeInitialValues(const Query& query,
                                       const std::vector<const Array*> 
                                         &objects,
                                       std::vector< std::vector<unsigned char> >
                                         &values,
                                       bool &hasSolution) {
  TimerStatIncrementer t(stats::cexCacheTime);
  Assignment *a;
  if (!getAssignment(query, a))
    return false;
  hasSolution = !!a;
  
  if (!a)
    return true;

  // FIXME: We should use smarter assignment for result so we don't
  // need redundant copy.
  values = std::vector< std::vector<unsigned char> >(objects.size());
  for (unsigned i=0; i < objects.size(); ++i) {
    const Array *os = objects[i];
    Assignment::bindings_ty::iterator it = a->bindings.find(os);
    
    if (it == a->bindings.end()) {
      values[i] = std::vector<unsigned char>(os->size, 0);
    } else {
      values[i] = it->second;
    }
  }
  
  return true;
}

SolverImpl::SolverRunStatus CexCachingSolver::getOperationStatusCode() {
  return solver->impl->getOperationStatusCode();
}

std::string CexCachingSolver::getConstraintLog(const Query& query) {
  return solver->impl->getConstraintLog(query);
}

void CexCachingSolver::setCoreSolverTimeout(time::Span timeout) {
  solver->impl->setCoreSolverTimeout(timeout);
}

void CexCachingSolver::handleBadDiskSatEntry(const std::array<uint8_t,32>& key,
                                            const DiskEntry& /*ent*/) {
  llvm::errs()
      << "[CexCachingSolver][WARN] Disk SAT assignment does NOT satisfy current key. "
      << "Treat as miss. key=" << toHex32(key) << "\n";

  diskIndex.erase(key);
  diskOverlay.erase(key);

  if (diskKeyBlacklist.insert(key).second && !blacklistPath.empty()) {
    std::ofstream bl(blacklistPath, std::ios::out | std::ios::app);
    if (bl.good()) { bl << toHex32(key) << "\n"; bl.flush(); }
  }
}

bool CexCachingSolver::trySatisfyFromSameSymbolCandidates(
    const KeyType& key,
    const std::vector<const Array*>& objects,
    const std::vector<CandRef>& candRefs,
    Assignment *&result) {
  unsigned probed = 0;
  for (const auto& c : candRefs) {
    if (CexCacheSameSymbolProbeLimit && probed >= CexCacheSameSymbolProbeLimit) break;
    if (diskKeyBlacklist.find(c.key) != diskKeyBlacklist.end())
      continue;

    DiskEntry ent;
    if (c.off == kOverlayOff) {
      auto itO = diskOverlay.find(c.key);
      if (itO == diskOverlay.end()) continue;
      ent = itO->second;
    } else {
      if (!readDiskEntryAt(c.off, ent)) continue;
    }

    if (!ent.sat) continue;

    std::unique_ptr<Assignment> rebuilt;
    if (!deserializeAssignment(ent.value, objects, rebuilt) || !rebuilt) continue;
    ++probed;

    if (rebuilt->satisfies(key.begin(), key.end())) {
      Assignment* binding = rebuilt.release();
      auto ins = assignmentsTable.insert(binding);
      if (!ins.second) { delete binding; binding = *ins.first; }
      result = binding;
      {
        KeyType kcopy(key.begin(), key.end());
        cache.insert(kcopy, binding);
      }
      const bool cameFromDisk = (c.off != kOverlayOff);
      if (cameFromDisk) logDiskHit();
      if (isDiskLogEnabled()) recordDiskEntryHit(c.key, cameFromDisk);
      ++stats::queryCexCacheHits;
      return true;
    }
  }
  return false;
}

void CexCachingSolver::dumpDiskCache() {
  if (!diskCacheDirty || cacheFilePath.empty())
    return;

  std::ofstream fout(cacheFilePath, std::ios::out | std::ios::app | std::ios::binary);
  if (!fout.good())
    return;

  std::unordered_set<std::string> seenSATCombos;
  if (CexCacheSameSymbolProbe) {
    seenSATCombos.reserve(diskLoadedKeys.size() + diskOverlay.size());
    for (const auto &k : diskLoadedKeys) {
      if (diskKeyBlacklist.find(k) != diskKeyBlacklist.end()) continue;
      auto itI = diskIndex.find(k);
      if (itI == diskIndex.end()) continue;
      DiskEntry ent;
      if (!readDiskEntryAt(itI->second.off, ent)) continue;
      if (!ent.sat) continue;
      if (ent.symbolsCSV.empty()) continue;
      const std::string symKey = canonicalSymKey(ent.symbolsCSV);
      if (symKey.empty()) continue;
      seenSATCombos.insert(symKey + "|" + ent.value);
    }
    for (const auto &kv : diskOverlay) {
      const DiskEntry &ent = kv.second;
      if (!ent.sat) continue;
      if (ent.symbolsCSV.empty()) continue;
      const std::string symKey = canonicalSymKey(ent.symbolsCSV);
      if (symKey.empty()) continue;
      seenSATCombos.insert(symKey + "|" + ent.value);
    }
  }

  for (auto &kv : diskAppendLog) {
    const auto &key   = kv.first;
    const auto &entry = kv.second;

    bool skip = false;
    if (CexCacheSameSymbolProbe && entry.sat && !entry.symbolsCSV.empty()) {
      const std::string symKey = canonicalSymKey(entry.symbolsCSV);
      if (!symKey.empty()) {
        const std::string combo = symKey + "|" + entry.value;
        if (!seenSATCombos.insert(combo).second) {
          skip = true;
        }
      }
    }
    if (skip)
      continue;
    writeOneRecordV3(fout, key, entry);
  }

  fout.flush();
  fout.close();
  diskAppendLog.clear();
  diskCacheDirty = false;
}

void CexCachingSolver::logDiskHit() {
  ++diskCacheHits;
  if (diskVerbose) {
    llvm::errs() << "[CexCachingSolver] Disk cache hit #" << diskCacheHits << "\n";
  }
}

bool CexCachingSolver::diskCacheLookupCounted(const std::array<uint8_t,32>& key,
                                              DiskEntry& out,
                                              bool& cameFromDiskFile) {
  auto tLookup0 = std::chrono::steady_clock::now();
  uint64_t localCompareUs = 0;
  const bool doCount = countingEnabled();
  if (doCount) ++opLookup;
  if (diskKeyBlacklist.find(key) != diskKeyBlacklist.end()) {
    auto tLookup1 = std::chrono::steady_clock::now();
    timeLookupUs += (uint64_t)
      std::chrono::duration_cast<std::chrono::microseconds>(tLookup1 - tLookup0).count();
    return false;
  }

  auto itO = diskOverlay.find(key);
  if (itO != diskOverlay.end()) {
    out = itO->second;
    cameFromDiskFile = false;
    auto tLookup1 = std::chrono::steady_clock::now();
    timeLookupUs += (uint64_t)
      std::chrono::duration_cast<std::chrono::microseconds>(tLookup1 - tLookup0).count();
    return true;
  }

  auto tCmp0 = std::chrono::steady_clock::now();
  auto itI = diskIndex.find(key);
  if (doCount && itI != diskIndex.end() && diskLoadedKeys.find(key) != diskLoadedKeys.end())
    ++opCompare;
  auto tCmp1 = std::chrono::steady_clock::now();
  uint64_t cmpUs = (uint64_t)
    std::chrono::duration_cast<std::chrono::microseconds>(tCmp1 - tCmp0).count();
  timeCompareUs += cmpUs;
  localCompareUs += cmpUs;

  if (itI == diskIndex.end()) {
    auto tLookup1 = std::chrono::steady_clock::now();
    uint64_t totalUs = (uint64_t)
      std::chrono::duration_cast<std::chrono::microseconds>(tLookup1 - tLookup0).count();
    timeLookupUs += (totalUs >= localCompareUs) ? (totalUs - localCompareUs) : 0;
    return false;
  }
  cameFromDiskFile = true;
  bool ok = readDiskEntryAt(itI->second.off, out);
  auto tLookup1 = std::chrono::steady_clock::now();
  uint64_t totalUs = (uint64_t)
    std::chrono::duration_cast<std::chrono::microseconds>(tLookup1 - tLookup0).count();
  timeLookupUs += (totalUs >= localCompareUs) ? (totalUs - localCompareUs) : 0;
  return ok;
}

void CexCachingSolver::diskCacheInsertOrUpdateCounted(const std::array<uint8_t,32>& key,
                                                      const DiskEntry& entry) {
  if (diskKeyBlacklist.find(key) != diskKeyBlacklist.end())
    return;

  diskOverlay[key] = entry;
  diskAppendLog.emplace_back(key, entry);
  diskCacheDirty = true;

  if (CexCacheSameSymbolProbe && !entry.symKey.empty()) {
    symIndex[entry.symKey].push_back(CandRef{key, kOverlayOff});
  }
}

void CexCachingSolver::splitCSV(const std::string& s, std::vector<std::string>& out) {
  out.clear();
  size_t pos = 0;
  while (pos < s.size()) {
    size_t comma = s.find(',', pos);
    std::string tok = s.substr(pos, comma == std::string::npos ? std::string::npos : (comma - pos));
    size_t l = 0, r = tok.size();
    while (l < r && (tok[l] == ' ' || tok[l] == '\t')) ++l;
    while (r > l && (tok[r-1] == ' ' || tok[r-1] == '\t')) --r;
    if (r > l) out.emplace_back(tok.substr(l, r - l));
    if (comma == std::string::npos) break;
    pos = comma + 1;
  }
}

bool CexCachingSolver::sameSymbolSet(const std::vector<std::string>& a,
                                     const std::vector<std::string>& b) {
  if (a.size() != b.size()) return false;
  std::unordered_set<std::string> sa(a.begin(), a.end());
  for (const auto& x : b) {
    auto it = sa.find(x);
    if (it == sa.end()) return false;
    sa.erase(it);
  }
  return sa.empty();
}

std::string CexCachingSolver::canonicalSymKey(const std::string& csv) {
  std::vector<std::string> syms;
  splitCSV(csv, syms);
  if (syms.empty())
    return std::string();
  std::sort(syms.begin(), syms.end());
  return joinComma(syms);
}

void CexCachingSolver::collectDiskCandidatesSameSymbols(const std::vector<const Array*>& objects,
                                                        std::vector<CandRef>& outRefs) const {
  outRefs.clear();
  std::vector<std::string> qSyms;
  extractSymbolsFromObjects(objects, qSyms);
  if (qSyms.empty()) return;
  std::string qSymKey = joinComma(qSyms);
  qSymKey = canonicalSymKey(qSymKey);
  if (qSymKey.empty()) return;

  auto it = symIndex.find(qSymKey);
  if (it == symIndex.end()) return;
  outRefs = it->second;
}

void CexCachingSolver::skipSpaces(const std::string& s, size_t& pos) {
  while (pos < s.size() && std::isspace((unsigned char)s[pos])) ++pos;
}

std::string CexCachingSolver::parseToken(const std::string& s, size_t& pos) {
  skipSpaces(s, pos);
  size_t start = pos;
  while (pos < s.size() &&
         !std::isspace((unsigned char)s[pos]) &&
         s[pos] != '(' && s[pos] != ')')
    ++pos;
  return s.substr(start, pos - start);
}

bool CexCachingSolver::parseExprNode(const std::string& s, size_t& pos, ExprNode& out) {
  skipSpaces(s, pos);
  if (pos >= s.size()) return false;

  if (s[pos] != '(') {
    out = ExprNode();
    out.isAtom = true;
    out.atom = parseToken(s, pos);
    return !out.atom.empty();
  }

  ++pos;
  skipSpaces(s, pos);

  out = ExprNode();
  out.isAtom = false;
  out.op = parseToken(s, pos);
  if (out.op.empty()) return false;

  while (true) {
    skipSpaces(s, pos);
    if (pos >= s.size()) return false;
    if (s[pos] == ')') {
      ++pos;
      break;
    }
    ExprNode child;
    if (!parseExprNode(s, pos, child)) return false;
    out.args.push_back(std::move(child));
  }
  return true;
}

bool CexCachingSolver::parseConstraintList(const std::string& s, std::vector<ExprNode>& out) {
  out.clear();
  const std::string prefix = "C:[";
  if (s.compare(0, prefix.size(), prefix) != 0 || s.empty() || s.back() != ']')
    return false;

  const std::string body = s.substr(prefix.size(), s.size() - prefix.size() - 1);
  size_t pos = 0;
  while (pos < body.size()) {
    skipSpaces(body, pos);
    if (pos >= body.size()) break;
    ExprNode n;
    if (!parseExprNode(body, pos, n)) return false;
    out.push_back(std::move(n));
    skipSpaces(body, pos);
    if (pos + 1 < body.size() && body[pos] == ';' && body[pos + 1] == ' ')
      pos += 2;
  }
  return true;
}

bool CexCachingSolver::isIntegerAtom(const std::string& s) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-') ? 1 : 0;
  if (i >= s.size()) return false;
  for (; i < s.size(); ++i) {
    if (!std::isdigit((unsigned char)s[i])) return false;
  }
  return true;
}

std::string CexCachingSolver::reverseUnsignedOp(const std::string& op) {
  if (op == "Ult") return "Ugt";
  if (op == "Ule") return "Uge";
  return op;
}

std::string CexCachingSolver::reverseSignedOp(const std::string& op) {
  if (op == "Slt") return "Sgt";
  if (op == "Sle") return "Sge";
  return op;
}

std::string CexCachingSolver::negateUnsignedOp(const std::string& op) {
  if (op == "Eq")  return "Une";
  if (op == "Ult") return "Uge";
  if (op == "Ule") return "Ugt";
  if (op == "Ugt") return "Ule";
  if (op == "Uge") return "Ult";
  if (op == "Une") return "Eq";
  return std::string();
}

std::string CexCachingSolver::negateSignedOp(const std::string& op) {
  if (op == "Eq")  return "Sne";
  if (op == "Slt") return "Sge";
  if (op == "Sle") return "Sgt";
  if (op == "Sgt") return "Sle";
  if (op == "Sge") return "Slt";
  if (op == "Sne") return "Eq";
  return std::string();
}

bool CexCachingSolver::atomToInt64(const std::string& s, int64_t& out) {
  if (!isIntegerAtom(s)) return false;
  char* endp = nullptr;
  long long v = std::strtoll(s.c_str(), &endp, 10);
  if (endp == s.c_str()) return false;
  out = (int64_t)v;
  return true;
}

const CexCachingSolver::ExprNode*
CexCachingSolver::unwrapCasts(const ExprNode* n) {
  while (n && !n->isAtom &&
         (n->op == "ZExt" || n->op == "SExt") &&
         !n->args.empty()) {
    n = &n->args[0];
  }
  return n;
}

bool CexCachingSolver::extractReadIndex(const ExprNode& n, unsigned& idx) {
  const ExprNode* p = unwrapCasts(&n);
  if (!p || p->isAtom) return false;
  if (p->op != "Read" && p->op != "ReadLSB") return false;

  for (const auto& a : p->args) {
    if (a.isAtom) {
      int64_t v = 0;
      if (atomToInt64(a.atom, v) && v >= 0 && v <= 1000000) {
        idx = (unsigned)v;
        return true;
      }
    }
  }
  return false;
}

bool CexCachingSolver::extractConstValue(const ExprNode& n, int64_t& value) {
  const ExprNode* p = unwrapCasts(&n);
  if (!p) return false;
  if (p->isAtom) return atomToInt64(p->atom, value);

  if ((p->op == "Add" || p->op == "Sub") && p->args.size() == 2) {
    int64_t lhs = 0, rhs = 0;
    if (extractConstValue(p->args[0], lhs) && extractConstValue(p->args[1], rhs)) {
      value = (p->op == "Add") ? (lhs + rhs) : (lhs - rhs);
      return true;
    }
  }
  return false;
}

CexCachingSolver::ByteSet CexCachingSolver::fullByteSet() {
  ByteSet bs;
  bs.set();
  return bs;
}

CexCachingSolver::ByteSet CexCachingSolver::singletonByteSet(unsigned v) {
  ByteSet bs;
  if (v < 256) bs.set(v);
  return bs;
}

CexCachingSolver::ByteSet
CexCachingSolver::rangeByteSetUnsigned(const std::string& op, int64_t c) {
  ByteSet bs;
  int64_t lo = 0, hi = 255;
  if (op == "Eq") {
    lo = hi = c;
    if (hi < 0 || lo > 255) return bs;
    lo = std::max<int64_t>(0, lo);
    hi = std::min<int64_t>(255, hi);
    for (int64_t v = lo; v <= hi; ++v) bs.set((size_t)v);
    return bs;
  }
  if (op == "Une") {
    bs = fullByteSet();
    if (0 <= c && c <= 255) bs.reset((size_t)c);
    return bs;
  }
  if (op == "Ult") {
    hi = c - 1;
  } else if (op == "Ule") {
    hi = c;
  } else if (op == "Ugt") {
    lo = c + 1;
  } else if (op == "Uge") {
    lo = c;
  } else {
    return bs;
  }

  if (hi < 0 || lo > 255 || lo > hi) return bs;
  lo = std::max<int64_t>(0, lo);
  hi = std::min<int64_t>(255, hi);
  for (int64_t v = lo; v <= hi; ++v) bs.set((size_t)v);
  return bs;
}

CexCachingSolver::ByteSet
CexCachingSolver::rangeByteSetSigned(const std::string& op, int64_t c) {
  ByteSet bs;
  if (op == "Sne") {
    bs = fullByteSet();
    if (-128 <= c && c <= 127) {
      int8_t sv = (int8_t)c;
      bs.reset((size_t)(uint8_t)sv);
    }
    return bs;
  }
  for (int v = 0; v <= 255; ++v) {
    int8_t sv = (int8_t)(uint8_t)v;
    bool ok = false;
    if (op == "Eq") {
      ok = (sv == c);
    } else if (op == "Slt") {
      ok = (sv < c);
    } else if (op == "Sle") {
      ok = (sv <= c);
    } else if (op == "Sgt") {
      ok = (sv > c);
    } else if (op == "Sge") {
      ok = (sv >= c);
    }
    if (ok) bs.set((size_t)v);
  }
  return bs;
}

CexCachingSolver::QueryValueProfile
CexCachingSolver::mergeAnd(const QueryValueProfile& a, const QueryValueProfile& b) {
  if (!a.valid) return b;
  if (!b.valid) return a;

  QueryValueProfile out;
  out.valid = true;
  out.idxSet = a.idxSet;
  out.idxSet.insert(b.idxSet.begin(), b.idxSet.end());

  for (unsigned idx : out.idxSet) {
    auto ita = a.valMap.find(idx);
    auto itb = b.valMap.find(idx);
    if (ita != a.valMap.end() && itb != b.valMap.end())
      out.valMap[idx] = (ita->second & itb->second);
    else if (ita != a.valMap.end())
      out.valMap[idx] = ita->second;
    else if (itb != b.valMap.end())
      out.valMap[idx] = itb->second;
  }
  return out;
}

CexCachingSolver::QueryValueProfile
CexCachingSolver::mergeOr(const QueryValueProfile& a, const QueryValueProfile& b) {
  if (!a.valid) return b;
  if (!b.valid) return a;

  QueryValueProfile out;
  out.valid = true;
  out.idxSet = a.idxSet;
  out.idxSet.insert(b.idxSet.begin(), b.idxSet.end());

  for (unsigned idx : out.idxSet) {
    auto ita = a.valMap.find(idx);
    auto itb = b.valMap.find(idx);
    if (ita != a.valMap.end() && itb != b.valMap.end())
      out.valMap[idx] = (ita->second | itb->second);
    else if (ita != a.valMap.end())
      out.valMap[idx] = ita->second;
    else if (itb != b.valMap.end())
      out.valMap[idx] = itb->second;
  }
  return out;
}

CexCachingSolver::QueryValueProfile
CexCachingSolver::negateProfile(const QueryValueProfile& p) {
  if (!p.valid) return p;
  QueryValueProfile out = p;
  for (auto& kv : out.valMap)
    kv.second.flip();
  return out;
}

bool CexCachingSolver::buildAtomicProfile(const std::string& op, unsigned idx, int64_t c,
                                         QueryValueProfile& out) {
  ByteSet vals;
  if (op == "Eq" || op == "Ult" || op == "Ule" || op == "Ugt" || op == "Uge" || op == "Une") {
    vals = rangeByteSetUnsigned(op, c);
  } else if (op == "Slt" || op == "Sle" || op == "Sgt" || op == "Sge" || op == "Sne") {
    vals = rangeByteSetSigned(op, c);
  } else {
    return false;
  }

  out.valid = true;
  out.idxSet.insert(idx);
  out.valMap[idx] = vals;
  return true;
}

bool CexCachingSolver::profileFromAtomicNegation(const ExprNode& n, QueryValueProfile& out) {
  if (n.isAtom) return false;
  if (!(n.op == "Eq" || n.op == "Ult" || n.op == "Ule" || n.op == "Slt" || n.op == "Sle"))
    return false;
  if (n.args.size() != 2) return false;

  unsigned idx = 0;
  int64_t c = 0;
  if (extractReadIndex(n.args[0], idx) && extractConstValue(n.args[1], c)) {
    const std::string negOp =
        (n.op == "Eq" || n.op == "Ult" || n.op == "Ule")
            ? negateUnsignedOp(n.op)
            : negateSignedOp(n.op);
    if (negOp.empty()) return false;
    if (!buildAtomicProfile(negOp, idx, c, out))
      return false;
    return true;
  }

  if (extractReadIndex(n.args[1], idx) && extractConstValue(n.args[0], c)) {
    const std::string revOp =
        (n.op == "Eq" || n.op == "Ult" || n.op == "Ule")
            ? reverseUnsignedOp(n.op)
            : reverseSignedOp(n.op);
    const std::string negOp =
        (revOp == "Eq" || revOp == "Ult" || revOp == "Ule" || revOp == "Ugt" || revOp == "Uge")
            ? negateUnsignedOp(revOp)
            : negateSignedOp(revOp);
    if (negOp.empty()) return false;
    if (!buildAtomicProfile(negOp, idx, c, out))
      return false;
    return true;
  }
  return false;
}

CexCachingSolver::QueryValueProfile
CexCachingSolver::profileFromExpr(const ExprNode& n) {
  QueryValueProfile out;

  if (n.isAtom) return out;

  if (n.op == "And") {
    QueryValueProfile acc;
    for (const auto& a : n.args) {
      acc = mergeAnd(acc, profileFromExpr(a));
    }
    return acc;
  }

  if (n.op == "Or") {
    QueryValueProfile acc;
    for (const auto& a : n.args) {
      acc = mergeOr(acc, profileFromExpr(a));
    }
    return acc;
  }

  if (n.op == "Not" && n.args.size() == 1) {
    if (profileFromAtomicNegation(n.args[0], out))
      return out;
    return negateProfile(profileFromExpr(n.args[0]));
  }

  if ((n.op == "Eq" || n.op == "Ult" || n.op == "Ule" ||
       n.op == "Slt" || n.op == "Sle") &&
      n.args.size() == 2) {
    unsigned idx = 0;
    int64_t c = 0;
    const bool lhsRead = extractReadIndex(n.args[0], idx) && extractConstValue(n.args[1], c);
    const bool rhsRead = extractReadIndex(n.args[1], idx) && extractConstValue(n.args[0], c);
    if (!lhsRead && !rhsRead) return out;

    std::string effOp = n.op;
    if (rhsRead && !lhsRead) {
      if (n.op == "Ult" || n.op == "Ule")
        effOp = reverseUnsignedOp(n.op);
      else if (n.op == "Slt" || n.op == "Sle")
        effOp = reverseSignedOp(n.op);
    }
    if (!buildAtomicProfile(effOp, idx, c, out))
      return QueryValueProfile();
    return out;
  }

  return out;
}

CexCachingSolver::QueryValueProfile
CexCachingSolver::buildQueryValueProfile(const std::string& constraintsText) {
  QueryValueProfile prof;
  std::vector<ExprNode> nodes;
  if (!parseConstraintList(constraintsText, nodes))
    return prof;

  for (const auto& n : nodes) {
    prof = mergeAnd(prof, profileFromExpr(n));
  }
  if (!prof.valid) return QueryValueProfile();
  return prof;
}

double CexCachingSolver::computeProfileSimilarity(const QueryValueProfile& q,
                                                  const QueryValueProfile& c) {
  if (!q.valid || !c.valid) return -1e18;

  size_t idxInter = 0;
  for (unsigned idx : q.idxSet) {
    if (c.idxSet.find(idx) != c.idxSet.end()) ++idxInter;
  }
  const size_t idxUnion = q.idxSet.size() + c.idxSet.size() - idxInter;
  if (idxUnion == 0 || idxInter == 0) return -1e18;

  const double idxScore = (double)idxInter / (double)idxUnion;
  double valScoreSum = 0.0;
  size_t matchedIdxCount = 0;

  for (unsigned idx : q.idxSet) {
    auto itc = c.valMap.find(idx);
    if (itc == c.valMap.end()) continue;
    auto itq = q.valMap.find(idx);
    if (itq == q.valMap.end()) continue;

    const ByteSet inter = (itq->second & itc->second);
    const ByteSet uni   = (itq->second | itc->second);
    const ByteSet candMinusQ = (itc->second & (~itq->second));

    const double interCnt = (double)inter.count();
    const double uniCnt   = (double)uni.count();
    const double extraCnt = (double)candMinusQ.count();

    if (uniCnt > 0.0)
      valScoreSum += (interCnt - extraCnt) / uniCnt;
    ++matchedIdxCount;
  }
  if (matchedIdxCount == 0) return -1e18;
  return idxScore * (valScoreSum / (double)matchedIdxCount);
}

void CexCachingSolver::rankSameSymbolCandidates(
    const Query& query,
    const std::vector<const Array*>& objects,
    std::vector<CandRef>& candRefs) const {
  if (candRefs.size() <= 1)
    return;

  (void)objects;
  const QueryValueProfile queryProf = buildQueryValueProfile(makeDiskKeyFromQuery(query));
  if (!queryProf.valid)
    return;

  struct ScoredCand {
    CandRef ref;
    double score = -1e18;
  };

  std::vector<ScoredCand> scored;
  scored.reserve(candRefs.size());

  for (const auto& c : candRefs) {
    DiskEntry ent;
    bool ok = false;
    if (c.off == kOverlayOff) {
      auto itO = diskOverlay.find(c.key);
      if (itO != diskOverlay.end()) {
        ent = itO->second;
        ok = true;
      }
    } else {
      ok = readDiskEntryAt(c.off, ent);
    }
    if (!ok || !ent.sat || ent.constraints.empty())
      continue;

    const QueryValueProfile candProf = buildQueryValueProfile(ent.constraints);
    if (!candProf.valid)
      continue;
    
    ScoredCand sc;
    sc.ref = c;
    sc.score = computeProfileSimilarity(queryProf, candProf);
    scored.push_back(std::move(sc));
  }

  std::sort(scored.begin(), scored.end(),
            [](const ScoredCand& a, const ScoredCand& b) {
              return a.score > b.score;
            });

  if (scored.empty()) {
    candRefs.clear();
    return;
  }
  
  candRefs.clear();
  candRefs.reserve(scored.size());
  for (const auto& sc : scored)
    candRefs.push_back(sc.ref);
}

bool CexCachingSolver::readDiskEntryAt(uint64_t off, DiskEntry& out) const {
  std::ifstream fin(cacheFilePath, std::ios::binary);
  if (!fin.good()) return false;
  fin.seekg((std::streamoff)off, std::ios::beg);
  char ver_c;
  if (!fin.read(&ver_c,1)) return false;
  const uint8_t ver = (uint8_t)ver_c;

  if (ver != kDiskRecVersion3)
    return false;

  std::array<uint8_t,32> key{};
  DiskEntry ent;
  if (!readOneRecordV3(fin, key, ent)) return false;
  out = std::move(ent);
  return true;
}

///

std::unique_ptr<Solver>
klee::createCexCachingSolver(std::unique_ptr<Solver> solver) {
  return std::make_unique<Solver>(
      std::make_unique<CexCachingSolver>(std::move(solver)));
}