//===-- CachingSolver.cpp - Caching expression solver ---------------------===//
//
//                     The KLEE Symbolic Virtual Machine
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "klee/Solver/Solver.h"

#include "klee/Expr/Constraints.h"
#include "klee/Expr/Expr.h"
#include "klee/Solver/IncompleteSolver.h"
#include "klee/Solver/SolverImpl.h"
#include "klee/Solver/SolverStats.h"

#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>
#include <unordered_set>

#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <sstream>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <openssl/sha.h>

#include <zstd.h>

#if __has_include(<filesystem>)
#  include <filesystem>
   namespace klee_fs = std::filesystem;
#elif __has_include(<experimental/filesystem>)
#  include <experimental/filesystem>
   namespace klee_fs = std::experimental::filesystem;
#else
#  define KLEE_NO_STD_FILESYSTEM 1
#endif

using namespace klee;

class CachingSolver : public SolverImpl {
private:
  bool diskEnabled = false;
  bool diskVerbose = false;
  ref<Expr> canonicalizeQuery(ref<Expr> originalQuery,
                              bool &negationUsed);

  void cacheInsert(const Query& query,
                   IncompleteSolver::PartialValidity result);

  bool cacheLookup(const Query& query,
                   IncompleteSolver::PartialValidity &result);

  struct CacheEntry {
    CacheEntry(const ConstraintSet &c, ref<Expr> q)
        : constraints(c), query(q) {}

    CacheEntry(const CacheEntry &ce)
      : constraints(ce.constraints), query(ce.query) {}

    ConstraintSet constraints;
    ref<Expr> query;

    bool operator==(const CacheEntry &b) const {
      return constraints==b.constraints && *query.get()==*b.query.get();
    }
  };

  struct CacheEntryHash {
    unsigned operator()(const CacheEntry &ce) const {
      unsigned result = ce.query->hash();
      for (auto const &constraint : ce.constraints) {
        result ^= constraint->hash();
      }
      return result;
    }
  };

  typedef std::unordered_map<CacheEntry, IncompleteSolver::PartialValidity,
                             CacheEntryHash>
      cache_map;

  std::unique_ptr<Solver> solver;
  cache_map cache;

  inline void initDiskEnabled() {
    const char* e = std::getenv("KLEE_CACHINGSOLVER_DISK");
    diskEnabled = (e && *e && std::strcmp(e, "0") != 0);
    const char* v = std::getenv("KLEE_CACHINGSOLVER_VERBOSE");
    diskVerbose = (v && *v && std::strcmp(v, "0") != 0);
  }
  inline bool isDiskEnabled() const { return diskEnabled; }
  inline void resetDiskStateWhenDisabled() {
    diskCache.clear();
    diskLoadedKeys.clear();
    diskAppendLog.clear();
    dictIndex.clear();
    diskFilePresent = false;
  }

  inline bool maybeMakeHashedKey(const ConstraintSet &constraints,
                                 const ref<Expr> &canonicalQuery,
                                 std::array<uint8_t,32> &outKey,
                                 std::string* outTextKey) {
    if (!isDiskEnabled()) return false;
    outKey = makeHashedKeyCounted(constraints, canonicalQuery, outTextKey);
    return true;
  }

  struct Arr32Hash {
    size_t operator()(const std::array<uint8_t,32>& a) const noexcept {
      uint64_t h = 1469598103934665603ull;
      for (uint8_t b : a) { h ^= b; h *= 1099511628211ull; }
      return static_cast<size_t>(h);
    }
  };
  struct DiskVal {
    IncompleteSolver::PartialValidity val;
    uint8_t flags;
  };
  std::unordered_map<std::array<uint8_t,32>, DiskVal, Arr32Hash> diskCache;
  std::string cacheFilePath;
  bool diskCacheDirty = false;
  std::unordered_set<std::array<uint8_t,32>, Arr32Hash> diskLoadedKeys;

  std::vector<std::pair<std::array<uint8_t,32>, DiskVal>> diskAppendLog;

  unsigned diskCacheHits = 0;

  uint64_t queryIndexCounter = 0;
  uint64_t currentQueryIndex = 0;
  std::unordered_map<std::array<uint8_t,32>, uint64_t, Arr32Hash> diskLastHitQueryIndex;
  std::string diskLastHitPath;
  inline void updateLastHitQueryIndex(const std::array<uint8_t,32>& key) {
    diskLastHitQueryIndex[key] = currentQueryIndex;
  }

  uint64_t opHash = 0;
  uint64_t opLookup = 0;
  uint64_t opCompare = 0;
  bool diskFilePresent = false;

  uint64_t runTotalQueries = 0;
  uint64_t runHitQueries   = 0;
  uint64_t runMemHits      = 0;
  uint64_t runDiskHits     = 0;

  std::string runHitSummaryPath;
  std::string queryTimePath;
  std::vector<std::pair<std::array<uint8_t,32>, uint64_t>> queryTimeLog;
  inline void recordSolveTime(const std::array<uint8_t,32>& key, uint64_t micros) {
    queryTimeLog.emplace_back(key, micros);
  }

  std::unordered_map<std::array<uint8_t,32>, uint64_t, Arr32Hash> diskHitPerKeyLoaded;
  std::unordered_map<std::array<uint8_t,32>, uint64_t, Arr32Hash> diskHitPerKeyCreated;
  std::unordered_map<CacheEntry, std::pair<std::array<uint8_t,32>, bool>, CacheEntryHash> memEntryToDiskKey;

  std::unordered_set<std::array<uint8_t,32>, Arr32Hash> blacklistKeys;
  std::string blacklistPath;

  static inline void sha256_compute(const uint8_t* data, size_t len, uint8_t out32[32]) {
    SHA256(data, len, out32);
  }

  static inline std::array<uint8_t,32> sha256_of_string(const std::string& s) {
    std::array<uint8_t,32> out{};
    sha256_compute(reinterpret_cast<const uint8_t*>(s.data()), s.size(), out.data());
    return out;
  }

  inline std::array<uint8_t,32>
  makeHashedKeyCounted(const ConstraintSet &constraints,
                       const ref<Expr> &canonicalQuery,
                       std::string* outTextKey) {
    std::string local;
    const std::string& textKey = (outTextKey
                                  ? (*outTextKey = makeDiskKey(constraints, canonicalQuery), *outTextKey)
                                  : (local = makeDiskKey(constraints, canonicalQuery), local));
    if (isDiskEnabled()) ++opHash;
    return sha256_of_string(textKey);
  }

  std::string makeDiskKey(const ConstraintSet &constraints,
                          const ref<Expr> &canonicalQuery) const {
    auto oneLine = [](std::string s) {
      std::string out;
      out.reserve(s.size());
      bool inWS = false;
      for (char ch : s) {
        if (ch == '\r' || ch == '\n' || ch == '\t' || ch == ' ') {
          if (!inWS) { out.push_back(' '); inWS = true; }
        } else {
          out.push_back(ch); inWS = false;
        }
      }
      while (!out.empty() && out.front()==' ') out.erase(out.begin());
      while (!out.empty() && out.back()==' ')  out.pop_back();
      return out;
    };
    
    std::vector<std::string> printed;
    printed.reserve(constraints.size());
    for (auto const &c : constraints) {
      std::string cs;
      llvm::raw_string_ostream cso(cs);
      c->print(cso);
      cso.flush();
      printed.push_back(oneLine(std::move(cs)));
    }
    std::sort(printed.begin(), printed.end());
    std::string key = "C:[";
    for (size_t i=0;i<printed.size();++i) {
      if (i) key += "; ";
      key += printed[i];
    }
    key += "] Q:[";
    std::string qs;
    llvm::raw_string_ostream qso(qs);
    canonicalQuery->print(qso);
    qso.flush();
    key += oneLine(qs);
    key += "]";
    return key;
  }

  static constexpr uint8_t kDiskRecVersion  = 2;
  static constexpr uint8_t kDiskRecVersion1 = 1;

  std::string dictBlobPath;
  std::string dictIdxPath;
  std::string diskHitCountPath;
  struct DictEntry { uint64_t off=0; uint32_t len=0; uint8_t flags=0; };

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
  static inline uint64_t from_le64(uint64_t v) { return to_le64(v); }
  static inline uint32_t from_le32(uint32_t v) { return to_le32(v); }

  std::unordered_map<std::array<uint8_t,32>, DictEntry, Arr32Hash> dictIndex;
  
  static constexpr uint8_t kFlagRaw  = 0x00;
  static constexpr uint8_t kFlagZstd = 0x01;

  
  static inline std::string maybeCompress(const std::string& s, uint8_t& outFlags) {
  outFlags = kFlagZstd;
  size_t cap = ZSTD_compressBound(s.size());
  std::string out;
  out.resize(cap);
  size_t n = ZSTD_compress(out.data(), cap, s.data(), s.size(), 3);
  if (ZSTD_isError(n) || n == 0 || n >= s.size()) {
    outFlags = kFlagRaw;
    return s;
  }
  out.resize(n);
  return out;
  }
  static inline bool maybeDecompress(const std::string& in, uint8_t flags, std::string& out) {
  if (flags & kFlagZstd) {
    unsigned long long sz = ZSTD_getFrameContentSize(in.data(), in.size());
    if (sz == ZSTD_CONTENTSIZE_ERROR) return false;
    if (sz == ZSTD_CONTENTSIZE_UNKNOWN) {
      size_t cap = in.size() * 4 + 64;
      std::string buf(cap, '\0');
      size_t n = ZSTD_decompress(buf.data(), buf.size(), in.data(), in.size());
      if (ZSTD_isError(n)) return false;
      buf.resize(n);
      out.swap(buf);
      return true;
    }
    out.resize(static_cast<size_t>(sz));
    size_t n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
    return !ZSTD_isError(n);
  }
    out = in;
    return true;
  }

  static inline std::string joinPath(const std::string& base, const char* name) {
    if (base.empty()) return std::string(name);
    if (base.back()=='/') return base + name;
    return base + "/" + name;
  }
  void loadDictIndex() {
    dictIndex.clear();
    std::ifstream fx(dictIdxPath, std::ios::binary);
    if (!fx.good()) return;
    while (true) {
      std::array<uint8_t,32> h{}; DictEntry de{}; char flags=0;
      if (!fx.read(reinterpret_cast<char*>(h.data()), 32)) break;
      uint64_t offLE=0; uint32_t lenLE=0;
      if (!fx.read(reinterpret_cast<char*>(&offLE), sizeof(offLE))) break;
      if (!fx.read(reinterpret_cast<char*>(&lenLE), sizeof(lenLE))) break;
      de.off = from_le64(offLE);
      de.len = from_le32(lenLE);
      if (!fx.read(&flags, 1)) break;
      de.flags = static_cast<uint8_t>(flags);
      dictIndex.emplace(h, de);
    }
  }
  void ensureKeyTextPersisted(const std::array<uint8_t,32>& h, const std::string& keyText) {
    if (dictIndex.find(h) != dictIndex.end()) return;
    uint8_t flags = kFlagRaw;
    std::string payload = maybeCompress(keyText, flags);
    std::ofstream db(dictBlobPath, std::ios::binary | std::ios::app);
    std::ofstream di(dictIdxPath,  std::ios::binary | std::ios::app);
    if (!db.good() || !di.good()) return;
    db.seekp(0, std::ios::end);
    uint64_t off = static_cast<uint64_t>(db.tellp());
    uint32_t len = static_cast<uint32_t>(payload.size());
    db.write(payload.data(), payload.size());
    di.write(reinterpret_cast<const char*>(h.data()), 32);
    const uint64_t offLE = to_le64(off);
    const uint32_t lenLE = to_le32(len);
    di.write(reinterpret_cast<const char*>(&offLE), sizeof(offLE));
    di.write(reinterpret_cast<const char*>(&lenLE), sizeof(lenLE));
    di.write(reinterpret_cast<const char*>(&flags), 1);
    dictIndex.emplace(h, DictEntry{off, len, flags});
  }
  bool getOriginalKeyText(const std::array<uint8_t,32>& h, std::string& outText) {
    auto it = dictIndex.find(h);
    if (it == dictIndex.end()) return false;
    std::ifstream db(dictBlobPath, std::ios::binary);
    if (!db.good()) return false;
    std::string buf;
    buf.resize(it->second.len);
    db.seekg(static_cast<std::streampos>(it->second.off), std::ios::beg);
    if (!db.read(buf.data(), buf.size())) return false;
    return maybeDecompress(buf, it->second.flags, outText);
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

  static inline bool readOneRecord(std::ifstream& fin,
                                   uint8_t& ver, int8_t& vcode, uint8_t& flags,
                                   std::array<uint8_t,32>& keyOut) {
    char ver_c=0, vc=0;
    if (!fin.read(&ver_c, 1)) return false;
    if (!fin.read(&vc, 1))    return false;
    ver   = static_cast<uint8_t>(ver_c);
    vcode = static_cast<int8_t>(vc);
    if (ver == kDiskRecVersion) { // v2
      char fl=0; if (!fin.read(&fl, 1)) return false;
      flags = static_cast<uint8_t>(fl);
    } else {
      flags = 0;
    }
    if (!fin.read(reinterpret_cast<char*>(keyOut.data()), 32)) return false;
    return true;
  }

  static inline void writeOneRecord(std::ofstream& fout,
                                    int8_t vcode, uint8_t flags,
                                    const std::array<uint8_t,32>& key) {
    const char ver_c = static_cast<char>(kDiskRecVersion);
    const char vc    = static_cast<char>(vcode);
    fout.write(&ver_c, 1);
    fout.write(&vc, 1);
    const char fl    = static_cast<char>(flags);
    fout.write(&fl, 1);
    fout.write(reinterpret_cast<const char*>(key.data()), 32);
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

  void loadBlacklist(const std::string &dir) {
    blacklistKeys.clear();
    blacklistPath = joinPath(dir, "blacklist.txt");

    std::ifstream fin(blacklistPath);
    if (!fin.good())
      return;

    auto hexval = [](char c) -> int {
      if ('0' <= c && c <= '9') return c - '0';
      if ('a' <= c && c <= 'f') return c - 'a' + 10;
      if ('A' <= c && c <= 'F') return c - 'A' + 10;
      return -1;
    };

    std::string line;
    while (std::getline(fin, line)) {
      while (!line.empty() &&
             (line.back() == '\r' || line.back() == '\n' ||
              line.back() == ' '  || line.back() == '\t'))
        line.pop_back();
      size_t start = 0;
      while (start < line.size() &&
             (line[start] == ' ' || line[start] == '\t'))
        ++start;

      if (line.size() - start < 64)
        continue;
      std::string hex = line.substr(start, 64);
      if (hex.size() != 64)
        continue;

      std::array<uint8_t,32> h{};
      bool ok = true;
      for (size_t i = 0; i < 32; ++i) {
        int hi = hexval(hex[2*i]);
        int lo = hexval(hex[2*i+1]);
        if (hi < 0 || lo < 0) { ok = false; break; }
        h[i] = static_cast<uint8_t>((hi << 4) | lo);
      }
      if (ok) blacklistKeys.insert(h);
    }
  }

  void loadDiskCache() {
    if (!isDiskEnabled()) {
      resetDiskStateWhenDisabled();
      cacheFilePath.clear();
      return;
    }

    diskCache.clear();
    diskLoadedKeys.clear();
    diskAppendLog.clear();

    const char *solverDirEnv = std::getenv("KLEE_CACHE_DIR");
    if (solverDirEnv && *solverDirEnv) {
      std::ostringstream oss;
      oss << solverDirEnv;
      if (oss.str().back() != '/')
        oss << '/';
      oss << "cache.bin";
      cacheFilePath = oss.str();
    } else {
      cacheFilePath = "klee_cache_cachingsolver.bin";
    }

    {
      std::string dir = ".";
      const auto pos = cacheFilePath.find_last_of("/");
      if (pos != std::string::npos) dir = cacheFilePath.substr(0, pos);
#ifndef KLEE_NO_STD_FILESYSTEM
      {
        std::error_code ec;
        klee_fs::create_directories(dir, ec);
      }
#endif
      dictBlobPath = joinPath(dir, "cache.dict");
      dictIdxPath  = joinPath(dir, "cache.idx");
      loadBlacklist(dir);
    }

    std::ifstream fin(cacheFilePath, std::ios::in | std::ios::binary);
    if (!fin.good()) {
      diskFilePresent = false;
      loadDictIndex();
      return;
    }
    diskFilePresent = true;

    fin.seekg(0, std::ios::end);
    auto fsz = static_cast<std::streamoff>(fin.tellg());
    fin.seekg(0, std::ios::beg);
    if (fsz > 0) {
      size_t est = static_cast<size_t>(fsz / 35);
      if (est) {
        diskCache.reserve(est * 2);
        diskLoadedKeys.reserve(est);
      }
    }

    fin.seekg(0, std::ios::beg);
    uint8_t ver; int8_t vcode; uint8_t flags=0; std::array<uint8_t,32> karr;
    std::streampos start = fin.tellg();
    auto decodePV = [](int8_t c, IncompleteSolver::PartialValidity& pv)->bool{
      switch (c) {
        case (int8_t)IncompleteSolver::MustBeTrue:  pv = IncompleteSolver::MustBeTrue;  return true;
        case (int8_t)IncompleteSolver::MustBeFalse: pv = IncompleteSolver::MustBeFalse; return true;
        case (int8_t)IncompleteSolver::TrueOrFalse: pv = IncompleteSolver::TrueOrFalse; return true;
        case (int8_t)IncompleteSolver::MayBeTrue:   pv = IncompleteSolver::MayBeTrue;   return true;
        case (int8_t)IncompleteSolver::MayBeFalse:  pv = IncompleteSolver::MayBeFalse;  return true;
        default: return false;
      }
    };

    auto disableDiskCacheLoad = [&](const char *reason) {
      llvm::errs() << "[CachingSolver][WARN] " << reason
                   << " Disabling disk cache load.\n";
      diskEnabled = false;
      diskCache.clear();
      diskLoadedKeys.clear();
      diskAppendLog.clear();
      dictIndex.clear();
      diskFilePresent = false;
    };

    if (readOneRecord(fin, ver, vcode, flags, karr) &&
        (ver == kDiskRecVersion || ver == kDiskRecVersion1)) {
      IncompleteSolver::PartialValidity pv;
      if (decodePV(vcode, pv)) {
        if (blacklistKeys.find(karr) == blacklistKeys.end()) {
          diskCache[karr] = DiskVal{
            pv,
            static_cast<uint8_t>((ver == kDiskRecVersion) ? flags : 0u)
          };
          diskLoadedKeys.insert(karr);
        }
      }
      while (true) {
        std::streampos recPos = fin.tellg();
        if (!readOneRecord(fin, ver, vcode, flags, karr)) {
          if (!fin.eof()) {
            disableDiskCacheLoad("Malformed or truncated disk cache record.");
            return;
          }
          break;
        }
        if (!(ver == kDiskRecVersion || ver == kDiskRecVersion1)) {
          disableDiskCacheLoad("Unsupported disk cache record version.");
          return;
        }
        if (!decodePV(vcode, pv)) {
          disableDiskCacheLoad("Invalid PartialValidity code in disk cache.");
          return;
        }
        if (blacklistKeys.find(karr) != blacklistKeys.end())
          continue;
        diskCache[karr] = DiskVal{
          pv,
          static_cast<uint8_t>((ver == kDiskRecVersion) ? flags : 0u)
        };
        diskLoadedKeys.insert(karr);
      }
      loadDictIndex();
      return;
    }

    fin.clear();
    fin.seekg(start, std::ios::beg);
    std::string line;
    while (std::getline(fin, line)) {
      if (line.empty()) continue;
      size_t tab = line.find('\t');
      if (tab == std::string::npos) continue;
      std::string valStr = line.substr(0, tab);
      std::string keyText = line.substr(tab + 1);
      char *endptr = nullptr;
      long vi_long = std::strtol(valStr.c_str(), &endptr, 10);
      if (endptr == valStr.c_str() || *endptr != '\0') continue;
      int vi = static_cast<int>(vi_long);
      IncompleteSolver::PartialValidity pv;
      if      (vi == (int)IncompleteSolver::MustBeTrue)  pv = IncompleteSolver::MustBeTrue;
      else if (vi == (int)IncompleteSolver::MustBeFalse) pv = IncompleteSolver::MustBeFalse;
      else if (vi == (int)IncompleteSolver::TrueOrFalse) pv = IncompleteSolver::TrueOrFalse;
      else if (vi == (int)IncompleteSolver::MayBeTrue)   pv = IncompleteSolver::MayBeTrue;
      else if (vi == (int)IncompleteSolver::MayBeFalse)  pv = IncompleteSolver::MayBeFalse;
      else continue;
      auto arr = sha256_of_string(keyText);
      if (blacklistKeys.find(arr) != blacklistKeys.end())
        continue;
      diskCache[arr] = DiskVal{pv, 0u};
      diskLoadedKeys.insert(arr);
    }
    loadDictIndex();
  }

  void dumpDiskCache() {
    if (!isDiskEnabled()) {
      diskAppendLog.clear();
      diskCacheDirty = false;
      return;
    }
    if (!diskCacheDirty) return;
    if (cacheFilePath.empty()) return;

    std::ofstream fout(cacheFilePath, std::ios::out | std::ios::app | std::ios::binary);
    if (!fout.good()) return;

    for (const auto &kv : diskAppendLog) {
      const auto vcode = static_cast<int8_t>(kv.second.val);
      const auto fl    = static_cast<uint8_t>(kv.second.flags);
      writeOneRecord(fout, vcode, fl, kv.first);
    }
    fout.flush();
    fout.close();
    diskAppendLog.clear();
    diskCacheDirty = false;
  }

  bool diskCacheLookupCounted(const std::array<uint8_t,32>& key,
                              IncompleteSolver::PartialValidity& outVal,
                              uint8_t& outFlags,
                              bool& cameFromDiskFile) {
    if (diskFilePresent && isDiskEnabled()) ++opLookup;
    const size_t b = diskCache.bucket(key);
    for (auto it = diskCache.begin(b); it != diskCache.end(b); ++it) {
      if (diskFilePresent && isDiskEnabled()) ++opCompare;
      if (it->first == key) {
        outVal  = it->second.val;
        outFlags= it->second.flags;
        cameFromDiskFile = (diskLoadedKeys.find(key) != diskLoadedKeys.end());
        return true;
      }
    }
    return false;
  }

  void diskCacheInsertOrUpdateCounted(const std::array<uint8_t,32>& key,
                                      IncompleteSolver::PartialValidity val,
                                      uint8_t flags) {
    const size_t b = diskCache.bucket(key);
    bool found = false;
    for (auto it = diskCache.begin(b); it != diskCache.end(b); ++it) {
      if (diskFilePresent && isDiskEnabled()) ++opCompare;
      if (it->first == key) {
        if (it->second.val != val || it->second.flags != flags) {
          it->second = DiskVal{val, flags};
          diskAppendLog.emplace_back(key, DiskVal{val, flags});
          diskCacheDirty = true;
        }
        found = true;
        break;
      }
    }
    if (!found) {
      diskCache.emplace(key, DiskVal{val, flags});
      diskAppendLog.emplace_back(key, DiskVal{val, flags});
      diskCacheDirty = true;
    }
  }

public:
  CachingSolver(std::unique_ptr<Solver> solver) : solver(std::move(solver)) {
    initDiskEnabled();
    loadDiskCache();
  }

  ~CachingSolver() override {
    const char* outDirEnv = std::getenv("KLEE_OUTPUT_DIR");
    std::string outDir = outDirEnv && *outDirEnv ? std::string(outDirEnv) : std::string("klee-last");
    runHitSummaryPath= joinPath(outDir, "cache_result.log");
    if (!runHitSummaryPath.empty()) {
      std::ofstream sf(runHitSummaryPath, std::ios::out | std::ios::app);
      if (sf.good()) {
        using namespace std::chrono;
        const uint64_t epoch_ms =
          (uint64_t)duration_cast<milliseconds>(
            system_clock::now().time_since_epoch()).count();

        double hitRate = 0.0;
        if (runTotalQueries)
          hitRate = (double)runHitQueries /
                    (double)runTotalQueries;

        sf << epoch_ms << ' '
           << runTotalQueries << ' '
           << runHitQueries << ' '
           << runMemHits << ' '
           << runDiskHits << ' '
           << hitRate
           << '\n';
        sf.flush();
      }
    }

    if (!isDiskEnabled()) {
      return;
    }
    dumpDiskCache();

    std::ostringstream resultPath;
    #ifndef KLEE_NO_STD_FILESYSTEM
      { std::error_code ec; klee_fs::create_directories(outDir, ec); }
    #endif
    resultPath << outDir << "/disk_cache_result.txt";
    diskHitCountPath = joinPath(outDir, "disk_cache_hit.count");
    queryTimePath    = joinPath(outDir, "disk_cache_solve_time.log");
    diskLastHitPath  = joinPath(outDir, "disk_cache_last_hit.log");

    llvm::errs() << "[CachingSolver] Writing disk cache result to: "
                 << resultPath.str() << "\n";

    std::ofstream fout(resultPath.str(), std::ios::out | std::ios::trunc);
    if (fout.good()) {
      fout << "=== BranchCachingSolver Disk Cache Summary ===" << std::endl;
      fout << "File: " << cacheFilePath << std::endl;
      fout << "Record format: v1=[ver=1][i8 validity][hash32], v2=[ver=2][i8 validity][u8 flags][hash32]" << std::endl;
      fout << "Disk cache hits: " << diskCacheHits << std::endl;
      fout << "Total query entries: " << diskCache.size() << std::endl;
      fout << "Persistent file present: " << (diskFilePresent ? "yes" : "no") << std::endl;
      const size_t loaded = diskLoadedKeys.size();
      size_t created = 0;
      for (const auto& kv : diskCache) {
        if (diskLoadedKeys.find(kv.first) == diskLoadedKeys.end())
          ++created;
      }
      fout << "Loaded entries (from disk): " << loaded << std::endl;
      fout << "Created this run          : " << created << std::endl;
      fout << "Op(Hash)    : " << opHash << std::endl;
      fout << "Op(Lookup)  : " << opLookup << std::endl;
      fout << "Op(Compare) : " << opCompare << std::endl;
      fout << std::endl;
      fout.close();
    }

    if (!queryTimeLog.empty() && !queryTimePath.empty()) {
      std::ofstream qf(queryTimePath, std::ios::out | std::ios::app);
      if (qf.good()) {
        for (const auto& kv : queryTimeLog) {
          qf << toHex32(kv.first) << ' ' << kv.second << '\n';
        }
        qf.flush();
      }
    }
    
    std::unordered_map<std::array<uint8_t,32>, uint64_t, Arr32Hash> acc;
    loadExistingHitCounts(acc);
    for (const auto& kv : diskHitPerKeyLoaded)  acc[kv.first] += kv.second;
    for (const auto& kv : diskHitPerKeyCreated) acc[kv.first] += kv.second;
    saveHitCountsAtomically(acc);

    if (!diskLastHitPath.empty()) {
      std::ofstream lf(diskLastHitPath, std::ios::out | std::ios::trunc);
      if (lf.good()) {
        for (const auto& kv : diskLastHitQueryIndex) {
          lf << toHex32(kv.first) << ' ' << kv.second << '\n';
        }
        lf.flush();
      }
    }
  }

  void logDiskCacheHit() {
    ++diskCacheHits;
    if (diskVerbose) {
      llvm::errs() << "[CachingSolver] Disk cache hit #" << diskCacheHits << "\n";
    }
  }

  inline void recordDiskEntryHit(const std::array<uint8_t,32>& key, bool cameFromDiskFile) {
    if (!isDiskEnabled()) return;
    if (cameFromDiskFile) {
      ++diskHitPerKeyLoaded[key];
    } else {
      ++diskHitPerKeyCreated[key];
    }
    updateLastHitQueryIndex(key);
  }

  bool computeValidity(const Query&, Solver::Validity &result);
  bool computeTruth(const Query&, bool &isValid);
  bool computeValue(const Query& query, ref<Expr> &result) {
    ++stats::queryCacheMisses;
    return solver->impl->computeValue(query, result);
  }
  bool computeInitialValues(const Query& query,
                            const std::vector<const Array*> &objects,
                            std::vector< std::vector<unsigned char> > &values,
                            bool &hasSolution) {
    ++stats::queryCacheMisses;
    return solver->impl->computeInitialValues(query, objects, values, 
                                              hasSolution);
  }
  SolverRunStatus getOperationStatusCode();
  std::string getConstraintLog(const Query&) override;
  void setCoreSolverTimeout(time::Span timeout);
};

/** @returns the canonical version of the given query.  The reference
    negationUsed is set to true if the original query was negated in
    the canonicalization process. */
ref<Expr> CachingSolver::canonicalizeQuery(ref<Expr> originalQuery,
                                           bool &negationUsed) {
  ref<Expr> negatedQuery = Expr::createIsZero(originalQuery);

  // select the "smaller" query to the be canonical representation
  if (originalQuery.compare(negatedQuery) < 0) {
    negationUsed = false;
    return originalQuery;
  } else {
    negationUsed = true;
    return negatedQuery;
  }
}

/** @returns true on a cache hit, false of a cache miss.  Reference
    value result only valid on a cache hit. */
bool CachingSolver::cacheLookup(const Query& query,
                                IncompleteSolver::PartialValidity &result) {
  currentQueryIndex = ++queryIndexCounter;
  ++runTotalQueries;
  bool negationUsed;
  ref<Expr> canonicalQuery = canonicalizeQuery(query.expr, negationUsed);

  CacheEntry ce(query.constraints, canonicalQuery);
  cache_map::iterator it = cache.find(ce);
  
  if (it != cache.end()) {
    result = (negationUsed ?
              IncompleteSolver::negatePartialValidity(it->second) :
              it->second);
    ++runHitQueries;
    ++runMemHits;
    if (isDiskEnabled()) {
      auto dk = memEntryToDiskKey.find(ce);
      if (dk != memEntryToDiskKey.end()) {
        const auto& keyArr = dk->second.first;
        const bool cameFromDiskFile = dk->second.second;
        recordDiskEntryHit(keyArr, cameFromDiskFile);
      }
    }      
    return true;
  }

  {
    if (!isDiskEnabled()) {
      return false;
    }
    std::array<uint8_t,32> keyArr{};
    if (!maybeMakeHashedKey(query.constraints, canonicalQuery, keyArr, nullptr)) {
      return false;
    }
    IncompleteSolver::PartialValidity stored;
    uint8_t storedFlags = 0;
    bool cameFromDiskFile = false;
    if (diskCacheLookupCounted(keyArr, stored, storedFlags, cameFromDiskFile)) {
      result = (negationUsed ?
                IncompleteSolver::negatePartialValidity(stored) :
                stored);
      ++runHitQueries;
      ++runDiskHits;
      cache.insert(std::make_pair(ce, stored));
      memEntryToDiskKey.emplace(ce, std::make_pair(keyArr, cameFromDiskFile));
      if (cameFromDiskFile) {
        logDiskCacheHit();
      }
      recordDiskEntryHit(keyArr, cameFromDiskFile);
      return true;
    }
  }  
  return false;
}


void CachingSolver::cacheInsert(const Query& query,
                                IncompleteSolver::PartialValidity result) {
  bool negationUsed;
  ref<Expr> canonicalQuery = canonicalizeQuery(query.expr, negationUsed);

  CacheEntry ce(query.constraints, canonicalQuery);
  IncompleteSolver::PartialValidity cachedResult = 
    (negationUsed ? IncompleteSolver::negatePartialValidity(result) : result);
  
  cache.insert(std::make_pair(ce, cachedResult));
  if (!isDiskEnabled()) {
    return;
  }
  std::array<uint8_t,32> keyArr{};
  if (!maybeMakeHashedKey(query.constraints, canonicalQuery, keyArr, nullptr)) {
    return;
  }

  if (!blacklistKeys.empty() &&
      blacklistKeys.find(keyArr) != blacklistKeys.end()) {
    return;
  }

  uint8_t flags = 0u;
  if (negationUsed) flags |= 0x01;
  diskCacheInsertOrUpdateCounted(keyArr, cachedResult, flags);
  memEntryToDiskKey.try_emplace(ce, std::make_pair(keyArr, false));
}

bool CachingSolver::computeValidity(const Query& query,
                                    Solver::Validity &result) {
  IncompleteSolver::PartialValidity cachedResult;
  bool tmp, cacheHit = cacheLookup(query, cachedResult);
  
  if (cacheHit) {
    switch(cachedResult) {
    case IncompleteSolver::MustBeTrue:   
      result = Solver::True;
      ++stats::queryCacheHits;
      return true;
    case IncompleteSolver::MustBeFalse:  
      result = Solver::False;
      ++stats::queryCacheHits;
      return true;
    case IncompleteSolver::TrueOrFalse:  
      result = Solver::Unknown;
      ++stats::queryCacheHits;
      return true;
    case IncompleteSolver::MayBeTrue: {
      ++stats::queryCacheMisses;
      auto t0 = std::chrono::steady_clock::now();
      if (!solver->impl->computeTruth(query, tmp))
        return false;
      auto t1 = std::chrono::steady_clock::now();
      uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      if (isDiskEnabled()) {
        bool negUsedA = false;
        ref<Expr> canonA = canonicalizeQuery(query.expr, negUsedA);
        std::array<uint8_t,32> keyA{};
        if (maybeMakeHashedKey(query.constraints, canonA, keyA, nullptr))
          recordSolveTime(keyA, us);
      }
      if (tmp) {
        cacheInsert(query, IncompleteSolver::MustBeTrue);
        result = Solver::True;
        return true;
      } else {
        cacheInsert(query, IncompleteSolver::TrueOrFalse);
        result = Solver::Unknown;
        return true;
      }
    }
    case IncompleteSolver::MayBeFalse: {
      ++stats::queryCacheMisses;
      auto t0 = std::chrono::steady_clock::now();
      if (!solver->impl->computeTruth(query.negateExpr(), tmp))
        return false;
      auto t1 = std::chrono::steady_clock::now();
      uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      if (isDiskEnabled()) {
        bool negUsedB = false;
        ref<Expr> canonB = canonicalizeQuery(query.expr, negUsedB);
        std::array<uint8_t,32> keyB{};
        if (maybeMakeHashedKey(query.constraints, canonB, keyB, nullptr))
          recordSolveTime(keyB, us);
      }
      if (tmp) {
        cacheInsert(query, IncompleteSolver::MustBeFalse);
        result = Solver::False;
        return true;
      } else {
        cacheInsert(query, IncompleteSolver::TrueOrFalse);
        result = Solver::Unknown;
        return true;
      }
    }
    default: assert(0 && "unreachable");
    }
  }

  ++stats::queryCacheMisses;
  
  auto t0 = std::chrono::steady_clock::now();
  if (!solver->impl->computeValidity(query, result))
    return false;
  auto t1 = std::chrono::steady_clock::now();
  uint64_t usMiss = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

  switch (result) {
  case Solver::True: 
    cachedResult = IncompleteSolver::MustBeTrue; break;
  case Solver::False: 
    cachedResult = IncompleteSolver::MustBeFalse; break;
  default: 
    cachedResult = IncompleteSolver::TrueOrFalse; break;
  }
  
  cacheInsert(query, cachedResult);
  if (isDiskEnabled()) {
    bool negUsedM = false;
    ref<Expr> canonM = canonicalizeQuery(query.expr, negUsedM);
    std::array<uint8_t,32> keyM{};
    if (maybeMakeHashedKey(query.constraints, canonM, keyM, nullptr))
      recordSolveTime(keyM, usMiss);
  }
  return true;
}

bool CachingSolver::computeTruth(const Query& query,
                                 bool &isValid) {
  IncompleteSolver::PartialValidity cachedResult;
  bool cacheHit = cacheLookup(query, cachedResult);
  if (cacheHit && cachedResult != IncompleteSolver::MayBeTrue) {
    ++stats::queryCacheHits;
    isValid = (cachedResult == IncompleteSolver::MustBeTrue);
    return true;
  }

  ++stats::queryCacheMisses;

  auto t0 = std::chrono::steady_clock::now();
  if (!solver->impl->computeTruth(query, isValid))
    return false;
  auto t1 = std::chrono::steady_clock::now();
  uint64_t usTruth = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
  (void)usTruth;
  if (isValid) {
    cachedResult = IncompleteSolver::MustBeTrue;
  } else if (cacheHit) {
    assert(cachedResult == IncompleteSolver::MayBeTrue);
    cachedResult = IncompleteSolver::TrueOrFalse;
  } else {
    cachedResult = IncompleteSolver::MayBeFalse;
  }
  
  cacheInsert(query, cachedResult);
  if (isDiskEnabled()) {
    bool negUsedT = false;
    ref<Expr> canonT = canonicalizeQuery(query.expr, negUsedT);
    std::array<uint8_t,32> keyT{};
    if (maybeMakeHashedKey(query.constraints, canonT, keyT, nullptr))
      recordSolveTime(keyT, usTruth);
  }
  return true;
}

SolverImpl::SolverRunStatus CachingSolver::getOperationStatusCode() {
  return solver->impl->getOperationStatusCode();
}

std::string CachingSolver::getConstraintLog(const Query& query) {
  return solver->impl->getConstraintLog(query);
}

void CachingSolver::setCoreSolverTimeout(time::Span timeout) {
  solver->impl->setCoreSolverTimeout(timeout);
}

///

std::unique_ptr<Solver>
klee::createCachingSolver(std::unique_ptr<Solver> solver) {
  return std::make_unique<Solver>(
      std::make_unique<CachingSolver>(std::move(solver)));
}
