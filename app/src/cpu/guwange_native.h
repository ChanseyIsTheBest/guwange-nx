// Native replacements of hot guest functions: the shared framework (futari-nx's futari_native.h, after
// UnleashedRecomp-NX's native_hot_patches / native_verify pattern), for Guwange's CPU round of 2026-10-22 (the console
// profile's hottest functions; the user: "optimise the hottest 35 functions").
//
// A replacement is a strong definition of the recompiled sub_X (REX_HOOK_RAW): it overrides the generated weak alias,
// and the recompiled body stays callable as __imp__sub_X. The body of a replacement is always
//
//   REX_HOOK_RAW(sub_82244530) {
//     static guwange::native::Site site{"guwange_native_memcpy", "memcpy", 0x82244530, 290, 0x...};
//     switch (guwange::native::ModeOf(site, base)) {
//       case guwange::native::kOriginal: return __imp__sub_82244530(ctx, base);
//       case guwange::native::kNative:   return NativeMemcpy(ctx, base);
//       case guwange::native::kVerify:   return guwange::native::Verify(site, ctx, base, {ranges},
//                                                                       __imp__sub_82244530, NativeMemcpy);
//     }
//   }
//
// Rules for a native (what makes it safe to turn on):
//  - Code check: it runs only if its key is on (and the CPU master, guwange_enhancements.cpp) AND the guest code it was
//    written from is what is loaded at its address: FNV-1a of the instruction words.
//  - Result: the same guest memory contents and the same ABI-visible registers as the recompiled function: r1, r2,
//    r13-r31, f14-f31, v14-v31 unchanged unless the original changes them, r3/r4 and f1 (return values) equal. Volatile
//    registers (r0, r4-r12, f0, f2-f13, ctr, xer, cr) are the caller's to lose; with kExact the native must match
//    those too. Stores below the stack pointer (the red zone) may be skipped.
//  - Verify: with guwange_cpu_verify (guwange_native_verify_every = N: every Nth call of each native) the recompiled
//    function and the native run from the same state; their memory in the declared ranges and their registers are
//    compared and the ORIGINAL's results are kept. Every check and difference goes into the "[verify] natives" line.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

namespace guwange::native {

// Host pointer of a guest address: the generated code's REX_RAW_ADDR (the PC keeps physical memory 0x1000 higher).
inline uint8_t* Host(uint8_t* base, uint32_t address) {
#if defined(_WIN32)
  return base + address + (address >= 0xE0000000u ? 0x1000u : 0u);
#else
  return base + address;
#endif
}

// A guest range [address, address + size) that one host pointer covers (no crossing into the physical views).
inline bool Contiguous(uint32_t address, uint32_t size) {
  const uint64_t end = uint64_t(address) + size;
  return end <= 0x100000000ull && (address >= 0xE0000000u || end <= 0xE0000000u);
}

// Big-endian guest loads and stores through a host pointer.
inline uint32_t Be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
inline uint16_t Be16(const uint8_t* p) {
  uint16_t v;
  std::memcpy(&v, p, 2);
  return __builtin_bswap16(v);
}
inline void PutBe32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}
inline void PutBe16(uint8_t* p, uint16_t v) {
  v = __builtin_bswap16(v);
  std::memcpy(p, &v, 2);
}

// FNV-1a 64 over the guest instruction words as stored (big-endian bytes).
inline uint64_t CodeHash(const uint8_t* base, uint32_t address, uint32_t words) {
  uint64_t h = 0xcbf29ce484222325ull;
  const uint8_t* p = Host(const_cast<uint8_t*>(base), address);
  for (uint32_t i = 0; i < words * 4; ++i) {
    h = (h ^ p[i]) * 0x100000001b3ull;
  }
  return h;
}

enum Mode : uint8_t { kOriginal = 0, kNative = 1, kVerify = 2 };
enum class Compare : uint8_t { kAbi, kExact };

struct Site {
  const char* key;    // cvar that turns it on (a row of guwange_enhancements.cpp)
  const char* what;   // log name
  uint32_t address;   // guest entry point
  uint32_t words;     // instructions covered by the code check
  uint64_t hash;      // CodeHash of those instructions when the native was written
  Compare compare = Compare::kAbi;
  std::atomic<int8_t> mode{-1};  // -1 = not decided yet
  std::atomic<uint32_t> calls{0};
  uint64_t verify_every = 1;
};

// Totals of every native's verify checks for the "[verify] natives" line (guwange_native.cpp), and the one log line
// per site saying what it runs.
void ReportVerify(const char* what, uint64_t checks, uint64_t differences);
void ReportDecided(const char* what, uint32_t address, int mode, uint64_t found_hash, uint64_t expected_hash);
// "memcpy 1234/0, ..." (checks/differences per native, since the last call) and the differences in all.
std::string VerifySummary(uint64_t& differences);

inline bool FlagOn(const char* key) {
  const std::string value = rex::cvar::GetFlagByName(key);
  return value == "true" || value == "1";
}

// The first call of a site: key, code check, verify mode (the module's code is loaded then); the result is kept.
// Out of line: only ModeOf's two-compare fast path below sits in every replacement.
[[gnu::noinline]] inline Mode DecideMode(Site& site, uint8_t* base) {
  int8_t mode = site.mode.load(std::memory_order_relaxed);
  if (mode < 0) {
    const uint64_t found = CodeHash(base, site.address, site.words);
    if (!FlagOn(site.key)) {
      mode = kOriginal;
    } else if (found != site.hash) {
      mode = kOriginal;
      ReportDecided(site.what, site.address, -1, found, site.hash);
    } else {
      mode = FlagOn("guwange_cpu_verify") ? kVerify : kNative;
      const std::string every = rex::cvar::GetFlagByName("guwange_native_verify_every");
      site.verify_every = std::max<uint64_t>(1, std::strtoull(every.c_str(), nullptr, 10));
    }
    site.mode.store(mode, std::memory_order_relaxed);
    if (mode != kOriginal) {
      ReportDecided(site.what, site.address, mode, found, site.hash);
    }
  }
  if (mode == kVerify && (site.calls.fetch_add(1, std::memory_order_relaxed) % site.verify_every) != 0) {
    return kNative;
  }
  return Mode(mode);
}

// What this call of the site runs: one load and a compare once decided (native or original); the first call and
// every call in verify mode go through DecideMode.
inline Mode ModeOf(Site& site, uint8_t* base) {
  const int8_t mode = site.mode.load(std::memory_order_relaxed);
  if (mode == kNative || mode == kOriginal) [[likely]] {
    return Mode(mode);
  }
  return DecideMode(site, base);
}

struct Range {
  uint32_t address;
  uint32_t size;
};

namespace detail {

inline void Gather(uint8_t* base, std::initializer_list<Range> ranges, std::vector<uint8_t>& out) {
  out.clear();
  for (const Range& r : ranges) {
    const uint8_t* p = Host(base, r.address);
    out.insert(out.end(), p, p + r.size);
  }
}

inline void Scatter(uint8_t* base, std::initializer_list<Range> ranges, const std::vector<uint8_t>& in) {
  size_t at = 0;
  for (const Range& r : ranges) {
    std::memcpy(Host(base, r.address), in.data() + at, r.size);
    at += r.size;
  }
}

inline bool SameRegisters(const PPCContext& a, const PPCContext& b, Compare compare, const char*& first) {
#define GUWANGE_SAME(reg)                        \
  if (a.reg.u64 != b.reg.u64) {                  \
    first = #reg;                                \
    return false;                                \
  }
  GUWANGE_SAME(r1) GUWANGE_SAME(r2) GUWANGE_SAME(r3) GUWANGE_SAME(r13) GUWANGE_SAME(r14) GUWANGE_SAME(r15)
  GUWANGE_SAME(r16) GUWANGE_SAME(r17) GUWANGE_SAME(r18) GUWANGE_SAME(r19) GUWANGE_SAME(r20) GUWANGE_SAME(r21)
  GUWANGE_SAME(r22) GUWANGE_SAME(r23) GUWANGE_SAME(r24) GUWANGE_SAME(r25) GUWANGE_SAME(r26) GUWANGE_SAME(r27)
  GUWANGE_SAME(r28) GUWANGE_SAME(r29) GUWANGE_SAME(r30) GUWANGE_SAME(r31) GUWANGE_SAME(f1) GUWANGE_SAME(f14)
  GUWANGE_SAME(f15) GUWANGE_SAME(f16) GUWANGE_SAME(f17) GUWANGE_SAME(f18) GUWANGE_SAME(f19) GUWANGE_SAME(f20)
  GUWANGE_SAME(f21) GUWANGE_SAME(f22) GUWANGE_SAME(f23) GUWANGE_SAME(f24) GUWANGE_SAME(f25) GUWANGE_SAME(f26)
  GUWANGE_SAME(f27) GUWANGE_SAME(f28) GUWANGE_SAME(f29) GUWANGE_SAME(f30) GUWANGE_SAME(f31)
  if (compare == Compare::kExact) {
    GUWANGE_SAME(r0) GUWANGE_SAME(r4) GUWANGE_SAME(r5) GUWANGE_SAME(r6) GUWANGE_SAME(r7) GUWANGE_SAME(r8)
    GUWANGE_SAME(r9) GUWANGE_SAME(r10) GUWANGE_SAME(r11) GUWANGE_SAME(r12) GUWANGE_SAME(f0) GUWANGE_SAME(f2)
    GUWANGE_SAME(f3) GUWANGE_SAME(f4) GUWANGE_SAME(f5) GUWANGE_SAME(f6) GUWANGE_SAME(f7) GUWANGE_SAME(f8)
    GUWANGE_SAME(f9) GUWANGE_SAME(f10) GUWANGE_SAME(f11) GUWANGE_SAME(f12) GUWANGE_SAME(f13)
  }
#undef GUWANGE_SAME
  if (std::memcmp(&a.v14, &b.v14, sizeof(PPCVRegister) * 18) != 0) {
    first = "v14-v31";
    return false;
  }
  return true;
}

}  // namespace detail

// Runs the recompiled function and the native from the same state (registers + the declared memory ranges), compares,
// and leaves the recompiled function's results in place. The ranges must cover every guest byte either one writes.
// The buffers belong to this call: a recompiled function can call another native's site, whose Verify then runs
// inside this one (the tile layers' memset). Shared per-thread buffers (futari-nx's form, where no native calls
// another) had the inner call overwrite the outer one's saved state: false differences, 2026-10-22.
template <typename Original, typename Native>
void Verify(Site& site, PPCContext& ctx, uint8_t* base, std::initializer_list<Range> ranges, Original&& original,
            Native&& native) {
  std::vector<uint8_t> before, after_original, after_native;
  detail::Gather(base, ranges, before);
  // PPCContext is aligned to 64 bytes; heap copies would need aligned new: copies live on this stack frame.
  PPCContext start = ctx;
  original(ctx, base);
  PPCContext result = ctx;
  detail::Gather(base, ranges, after_original);
  detail::Scatter(base, ranges, before);
  ctx = start;
  native(ctx, base);
  detail::Gather(base, ranges, after_native);
  const char* first = "memory";
  const bool same = after_native == after_original && detail::SameRegisters(result, ctx, site.compare, first);
  detail::Scatter(base, ranges, after_original);
  ctx = result;
  if (!same) {
    size_t at = 0;
    if (after_native.size() == after_original.size()) {
      while (at < after_native.size() && after_native[at] == after_original[at]) {
        ++at;
      }
    }
    // Where: which declared range, the guest address, and the bytes there (original / native), with the number of
    // differing bytes in all.
    std::string where;
    if (first[0] == 'm' && at < after_native.size()) {
      size_t range_start = 0, index = 0;
      uint32_t address = 0;
      for (const Range& r : ranges) {
        if (at < range_start + r.size) {
          address = r.address + uint32_t(at - range_start);
          break;
        }
        range_start += r.size;
        ++index;
      }
      size_t count = 0;
      for (size_t i = 0; i < after_native.size(); ++i) count += after_native[i] != after_original[i];
      std::string o, n;
      for (size_t i = at & ~size_t(3); i < std::min(after_native.size(), (at & ~size_t(3)) + 8); ++i) {
        o += fmt::format("{:02X}", after_original[i]);
        n += fmt::format("{:02X}", after_native[i]);
      }
      where = fmt::format(" byte {} = range {} address {:08X}: original {} native {}; {} bytes differ", at, index,
                          address, o, n, count);
    }
    REXLOG_ERROR("[native] {} {:08X}: verify DIFFERENCE (first: {}{}); r3 {:08X} r4 {:08X} r5 {:08X} in", site.what,
                 site.address, first, where, start.r3.u32, start.r4.u32, start.r5.u32);
  }
  ReportVerify(site.what, 1, same ? 0 : 1);
}

}  // namespace guwange::native
