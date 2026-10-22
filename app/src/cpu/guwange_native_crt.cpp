// Natives 1-2 of Guwange's CPU round (2026-10-22): default.xex's XDK memcpy (sub_82244530) and memset (sub_82243BF0),
// 2.3 % and 2.2 % of the emulation thread on the console (the arcade board's frame buffers and sprite lists). Byte for
// byte the routines futari-nx replaced (the same instruction hashes; its futari_native_crt.h): for ranges that do not
// overlap the copy is exactly a host memcpy, the fill exactly a host memset of r4's low byte.

#include "cpu/guwange_native.h"

#include <rex/hook.h>

extern "C" REX_FUNC(__imp__sub_82244530);
extern "C" REX_FUNC(__imp__sub_82243BF0);

REXCVAR_DEFINE_BOOL(guwange_native_memcpy, true, "Guwange/Enhancements",
                    "Native XDK memcpy in default.xex (host memcpy; overlapping copies keep the recompiled code)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(guwange_native_memset, true, "Guwange/Enhancements",
                    "Native XDK memset in default.xex (host memset of r4's low byte)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

using namespace guwange::native;

// memcpy(r3 = destination, r4 = source, r5 = size) -> r3 = destination. The XDK's copy (290 instructions) is a pure
// forward copy (byte/word/doubleword/128-byte loops with dcbt/dcbtst hints, no dcbz) that saves r3 in the red zone and
// reloads it. Overlapping ranges (undefined for memcpy, but the forward copy gives a defined pattern) and ranges
// crossing into the physical views run the recompiled copy.
constexpr uint32_t kMemcpyWords = 290;
constexpr uint64_t kMemcpyHash = 0xEE0EAEB1E0584297ull;

void Memcpy(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, size = ctx.r5.u32;
  const bool apart = uint64_t(dst) + size <= src || uint64_t(src) + size <= dst;
  if (!apart || !Contiguous(dst, size) || !Contiguous(src, size)) {
    __imp__sub_82244530(ctx, base);
    return;
  }
  std::memcpy(Host(base, dst), Host(base, src), size);
}

// memset(r3 = destination, r4 = value (its low byte), r5 = size) -> r3 = destination unchanged. The XDK's fill (40
// instructions): every byte of [dst, dst + n) is written once with the low byte of r4, nothing outside it, nothing
// read. ctr = n + 1 is 64-bit, so a size with its upper half set runs the original, as do ranges crossing into the
// physical views.
constexpr uint32_t kMemsetWords = 40;
constexpr uint64_t kMemsetHash = 0x54D201DC8A21DABAull;

void Memset(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, size = ctx.r5.u32;
  if ((ctx.r5.u64 >> 32) || !Contiguous(dst, size)) {
    __imp__sub_82243BF0(ctx, base);
    return;
  }
  if (size) std::memset(Host(base, dst), int(ctx.r4.u32 & 0xFF), size);
}

}  // namespace

REX_HOOK_RAW(sub_82244530) {
  static Site site{"guwange_native_memcpy", "memcpy", 0x82244530, kMemcpyWords, kMemcpyHash};
  switch (ModeOf(site, base)) {
    case kOriginal:
      return __imp__sub_82244530(ctx, base);
    case kNative:
      return Memcpy(ctx, base);
    case kVerify:
      if (!Contiguous(ctx.r3.u32, ctx.r5.u32)) {
        return __imp__sub_82244530(ctx, base);
      }
      return Verify(site, ctx, base, {{ctx.r3.u32, ctx.r5.u32}}, __imp__sub_82244530, Memcpy);
  }
}

REX_HOOK_RAW(sub_82243BF0) {
  static Site site{"guwange_native_memset", "memset", 0x82243BF0, kMemsetWords, kMemsetHash};
  switch (ModeOf(site, base)) {
    case kOriginal:
      return __imp__sub_82243BF0(ctx, base);
    case kNative:
      return Memset(ctx, base);
    case kVerify:
      if ((ctx.r5.u64 >> 32) || !Contiguous(ctx.r3.u32, ctx.r5.u32)) {
        return __imp__sub_82243BF0(ctx, base);
      }
      return Verify(site, ctx, base, {{ctx.r3.u32, ctx.r5.u32}}, __imp__sub_82243BF0, Memset);
  }
}
