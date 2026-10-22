// Natives 3-4 of Guwange's CPU round (2026-10-22): the arcade board's video, drawn in software by default.xex on the
// emulation thread. The console profile (rex_profile (22).log, 40-48 fps): sub_82196338 (the three tile layers) 45.7 %
// and sub_821966D8 (the sprites) 14.5 % of that thread. Both are transliterations of the guest code (tools/ppcfn.py
// listings), with the same order of visits, so the priority buffer, the 16-bit frame buffer and every other byte they
// write come out the same; what changes is how: the board's registers, the screen size and the buffers in host
// registers instead of the guest stack, host pointers instead of a guest address per access, and a transparent row of
// a 16x16 tile skipped with two 8-byte loads.
//
// The board's memory (this+748, "mem" below) as both functions read it:
//   mem + 0x400000 (+0x4000)   sprite list, two halves of 788 entries of 16 bytes (the vtable call picks one)
//   mem + 0x500000 + L*1MB     tile layer L: 32x32 entries of 4 bytes (attribute byte, tile code u16 at +2), then
//                              its line scroll table (u16 per line, 4 bytes apart) at +0x1000
//   mem + 0x900000 + L*1MB     layer L's registers: scroll x u16 at +0, scroll y u16 at +2 (0x4000 = line scroll),
//                              byte at +5 (0x10 = layer off)
//   mem + 0xC00000 / 0xC08000  sprite / tile palettes: 64 colours of 256 big-endian u16 entries
// this+44 = the frame buffer's pitch (pixels), this+68 = the frame buffer (u16), this+72 = the priority buffer (one
// byte per pixel), this+36/40 = sprite graphics size / base, this+12+8L / this+16+8L = layer L's graphics size / base.

#include "cpu/guwange_native.h"

#include <rex/frame_latency.h>
#include <rex/hook.h>

extern "C" REX_FUNC(__imp__sub_82196338);
extern "C" REX_FUNC(__imp__sub_821966D8);

REXCVAR_DEFINE_BOOL(guwange_native_tilemap, true, "Guwange/Enhancements",
                    "Native tile layers of the arcade board's video (sub_82196338, the emulation thread's hottest "
                    "function), checked against the guest code")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(guwange_native_sprites, true, "Guwange/Enhancements",
                    "Native sprites of the arcade board's video (sub_821966D8), checked against the guest code")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace {

using namespace guwange::native;

constexpr uint32_t kLinesAddress = 0x8268D6EC;  // visible lines (int32)
constexpr uint32_t kWidthAddress = 0x8268A3DC;  // visible columns (int32)
constexpr uint32_t kRowTable = 0x82C138D8;      // 512 u32: per screen row of the layer, the frame buffer offset
constexpr uint32_t kBoardSize = 0xC10000;       // the part of the board's memory the two functions read

struct Screen {
  uint32_t self, mem, dest, prio;
  int32_t pitch, lines, width;
  uint8_t *dest_h, *prio_h, *mem_h;
};

// The screen and buffers, or false when something is outside what the natives assume (then the guest code runs:
// a pitch of 0 traps there, sizes no game screen has, buffers crossing into the physical views).
bool LoadScreen(PPCContext& ctx, uint8_t* base, Screen& s) {
  s.self = ctx.r3.u32;
  if (!Contiguous(s.self, 752)) return false;
  const uint8_t* self = Host(base, s.self);
  s.mem = Be32(self + 748);
  s.dest = Be32(self + 68);
  s.pitch = int32_t(Be32(self + 44));
  s.prio = Be32(self + 72);
  s.lines = int32_t(Be32(Host(base, kLinesAddress)));
  s.width = int32_t(Be32(Host(base, kWidthAddress)));
  if (s.pitch <= 0 || s.pitch > 4096 || s.lines <= 0 || s.lines > 1024 || s.width <= 0 || s.width > 512) return false;
  // Every pixel index either function writes is below pitch * lines + 1024 (a row offset below pitch * lines plus a
  // column below 1024).
  const uint32_t pixels = uint32_t(s.pitch) * uint32_t(s.lines) + 1024;
  if (!Contiguous(s.mem, kBoardSize) || !Contiguous(s.dest, 2 * pixels) || !Contiguous(s.prio, pixels)) return false;
  s.dest_h = Host(base, s.dest);
  s.prio_h = Host(base, s.prio);
  s.mem_h = Host(base, s.mem);
  return true;
}

uint32_t Pixels(const Screen& s) { return uint32_t(s.pitch) * uint32_t(s.lines) + 1024; }

// ---- sub_82196338: tile layers 2, 1, 0 --------------------------------------------------------------------------
constexpr uint32_t kTilemapWords = 231;
constexpr uint64_t kTilemapHash = 0x63D872FB1337C0A1ull;

void Tilemap(PPCContext& __restrict ctx, uint8_t* base) {
  rex::latency::TraceEvent("emulation thread: tile layers (the arcade frame's video, last)");
  Screen s;
  if (!LoadScreen(ctx, base, s)) {
    __imp__sub_82196338(ctx, base);
    return;
  }
  const uint8_t* self = Host(base, s.self);
  // Each layer's graphics in one host range (never seen otherwise; checked before anything is written, so the guest
  // code can take the whole call over).
  for (int layer = 0; layer < 3; ++layer) {
    if (!Contiguous(Be32(self + 16 + 8 * layer), Be32(self + 12 + 8 * layer))) {
      __imp__sub_82196338(ctx, base);
      return;
    }
  }
  uint8_t* rows = Host(base, kRowTable);
  const uint8_t* pal_h = s.mem_h + 0xC08000;
  const int32_t screen_end = int32_t(uint32_t(s.pitch) * uint32_t(s.lines));
  uint64_t r3 = ctx.r3.u64;  // what the guest code leaves in r3 (its last tile's priority level, see below)
  for (int layer = 2; layer >= 0; --layer) {
    const uint32_t lo = uint32_t(layer) << 20;
    const uint8_t* regs = s.mem_h + lo + 0x900000;
    if (regs[5] & 0x10) continue;  // layer off
    const uint32_t sy = Be16(regs + 2), sx = Be16(regs + 0);
    const uint8_t* vram = s.mem_h + lo + 0x500000;
    const int32_t xoff = (406 - layer) - int32_t(sx & 0x1FF);
    const int32_t yoff = 495 - int32_t(sy & 0x1FF);
    const bool line_scroll = (sy & 0x4000) != 0;
    if (line_scroll) {
      // The guest's memset(rows, 0xFF, 2048) (it returns r3 = rows), then line k + 1 at the row its table names, and
      // every row still unset when its turn comes gets line 511 (no line on screen).
      std::memset(rows, 0xFF, 2048);
      r3 = uint64_t(int64_t(int32_t(kRowTable)));  // built with lis: sign-extended in the 64-bit register
      const uint8_t* table = vram + 0x1000 + 2;
      for (int k = 0; k < 512; ++k) {
        const int32_t line = (k + 1) & 511;
        if (line < s.lines) {
          PutBe32(rows + 4 * (Be16(table + 4 * k) & 0x1FF), uint32_t(line) * uint32_t(s.pitch));
        }
        if (Be32(rows + 4 * k) == 0xFFFFFFFFu) PutBe32(rows + 4 * k, uint32_t(s.pitch) * 511u);
      }
    } else {
      for (int i = 0; i < 512; ++i) {
        PutBe32(rows + 4 * i, uint32_t((i + yoff + 512) & 511) * uint32_t(s.pitch));
      }
    }
    const uint32_t limit = Be32(self + 12 + 8 * layer);
    const uint32_t gfx = Be32(self + 16 + 8 * layer);
    const uint8_t* gfx_h = Host(base, gfx);
    for (int row = 31; row >= 0; --row) {
      const int32_t y0 = row * 16;
      for (int col = 31; col >= 0; --col) {
        const int32_t x0 = (xoff + 512 + 16 * col) & 511;
        if (!line_scroll) {
          // The guest's cull of a tile by its top-left pixel (as it is: a tile straddling the edge can go).
          if (x0 % 496 >= s.width) continue;
          const uint32_t line = Be32(rows + 4 * y0) / uint32_t(s.pitch);
          if (int32_t(line % 496) >= s.lines) continue;
        }
        const uint8_t* entry = vram + 4 * (row * 32 + col);
        const uint32_t code = Be16(entry + 2);
        if (code == 0 || code == 0xFFFF) continue;
        const uint32_t attr = entry[0];
        const int32_t level = int32_t(attr >> 6) + 2;
        r3 = uint64_t(level);
        const uint32_t offset = code << 8;
        if (offset + 256 >= limit) continue;
        const uint8_t* tile = gfx_h + offset;
        const uint8_t* tpal = pal_h + ((attr & 0x3F) << 9);
        // The tile's 16 columns on screen, in order (they wrap at 512).
        const bool inside = x0 + 15 < s.width;  // no column off the right edge, none wrapping
        for (int r = 0; r < 16; ++r) {
          const int32_t line_base = int32_t(Be32(rows + 4 * ((y0 + r) & 511)));
          if (line_base >= screen_end) continue;
          const uint8_t* src = tile + 16 * r;
          uint64_t a, b;
          std::memcpy(&a, src, 8);
          std::memcpy(&b, src + 8, 8);
          if ((a | b) == 0) continue;  // a transparent row: nothing to draw
          for (int px = 0; px < 16; ++px) {
            const uint32_t pix = src[px];
            if (!pix) continue;
            int32_t x = x0 + px;
            if (!inside) {
              x &= 0x1FF;
              if (x >= s.width) continue;
            }
            const uint32_t idx = uint32_t(line_base) + uint32_t(x);
            uint8_t& p = s.prio_h[idx];
            const uint32_t pv = p;
            if (int32_t(pv & 0xF) < level || ((pv & 0x80) && int32_t((pv >> 4) & 7) < level)) {
              PutBe16(s.dest_h + 2 * idx, uint16_t(Be16(tpal + 2 * pix) | 0x8000));
              p = uint8_t(level);
            }
          }
        }
      }
    }
  }
  ctx.r3.u64 = r3;
}

// ---- sub_821966D8: the 788 sprites ----------------------------------------------------------------------------------
constexpr uint32_t kSpritesWords = 175;
constexpr uint64_t kSpritesHash = 0x249FC657F5AB0DA8ull;

// After the guest's virtual call (which picks the list half; r3 = its result): the sprites from entry 787 down to 0.
// The sprite graphics are one host range (SpriteGraphicsOk).
void SpritesAfterCall(PPCContext& __restrict ctx, uint8_t* base, const Screen& s) {
  uint8_t* self = Host(base, s.self);
  const uint32_t sgfx_limit = Be32(self + 36);
  const uint8_t* sgfx_h = Host(base, Be32(self + 40));
  const uint8_t* spal_h = s.mem_h + 0xC00000;
  const int32_t bank = int8_t(ctx.r3.u8);
  const uint32_t half = uint32_t((bank + 1) & 1) << 14;
  const uint8_t* list = s.mem_h + half + 0x400000;
  const int32_t screen_end = int32_t(uint32_t(s.pitch) * uint32_t(s.lines));
  uint32_t tiles = 0;
  uint64_t r3 = ctx.r3.u64;
  for (int k = 787; k >= 0; --k) {
    const uint8_t* e = list + 16 * k;
    const int32_t attr = int8_t(e[5]);
    const uint32_t w = e[12], h = e[13];
    int32_t x = int32_t(int16_t(Be16(e + 0))) / 64;
    const int32_t flip_x = (attr >> 3) & 1;
    const int32_t flip_y = (attr >> 2) & 1;
    const int32_t zx = int32_t(Be16(e + 8) & 0x3FFF);
    const int32_t zy = int32_t(Be16(e + 10) & 0x3FFF);
    r3 = uint64_t(uint32_t(zx));
    const int32_t w16 = int32_t(w << 4), h16 = int32_t(h << 4);
    tiles += h * w;
    if (flip_x) x += w16 - ((w16 * zy) >> 8);  // (zoom y: as the guest code has it)
    int32_t y = int32_t(int16_t(Be16(e + 2))) / 64;
    if (flip_y) y += h16 - ((h16 * zy) >> 8);
    const int32_t x_span = w16 * zx;
    if (((x_span >> 8) + x) < 0 || x >= s.width) continue;
    const int32_t y_span = h16 * zy;
    if (((y_span >> 8) + y) < 0 || y >= s.lines) continue;
    const uint32_t code = (uint32_t(Be16(e + 6)) | ((uint32_t(attr) & 3) << 16)) & 0x3FFFF;
    if (code == 0) continue;
    const int32_t level = ((attr >> 4) & 3) + 1;
    const uint32_t offset = code << 8;
    if (uint32_t(h16 * w16) + offset >= sgfx_limit) continue;
    const uint8_t* tpal = spal_h + ((uint32_t(e[4]) << 9) & 0x7E00);
    int32_t x_from, x_to, x_step, x_base = x;
    if (!flip_x) {
      x_from = 0, x_to = x_span, x_step = zx;
    } else {
      x_from = x_span, x_to = 0, x_step = -zx;
      x_base = x - 1;
      r3 = uint64_t(-int64_t(r3));
    }
    int32_t ry, ry_to, ry_step, y_base;
    if (!flip_y) {
      ry = 0, ry_to = y_span, ry_step = zy, y_base = y;
    } else {
      ry = y_span, ry_to = 0, ry_step = -zy, y_base = y - 1;
    }
    const uint8_t* src_row = sgfx_h + offset;
    for (; ry != ry_to; ry += ry_step, src_row += w16) {
      const int32_t line = ((ry >> 8) + y_base) & 0x3FF;
      const int32_t line_base = int32_t(uint32_t(line) * uint32_t(s.pitch));
      if (line_base >= screen_end || x_from == x_to) continue;
      const uint8_t* src = src_row;
      for (int32_t rx = x_from; rx != x_to; rx += x_step, ++src) {
        const uint32_t pix = *src;
        if (!pix) continue;
        const int32_t xx = ((rx >> 8) + x_base) & 0x3FF;
        if (xx >= s.width) continue;
        const uint32_t idx = uint32_t(line_base) + uint32_t(xx);
        uint8_t& p = s.prio_h[idx];
        const uint32_t pv = p;
        if (pv == 0) {
          PutBe16(s.dest_h + 2 * idx, uint16_t(Be16(tpal + 2 * pix) | 0x8000));
          p = uint8_t(level);
        } else if (int32_t(pv & 0xF) < level) {
          // A sprite behind what is there: the level goes in, the old one moves up (the tile layers read both).
          p = uint8_t((pv << 4) | 0x80 | uint32_t(level));
        }
      }
    }
  }
  PutBe32(self + 736, Be32(self + 736) + tiles);
  ctx.r3.u64 = r3;
}

// Whether the sprite graphics are contiguous (else the guest code runs; decided before the guest's virtual call, so
// that the call is made once either way).
bool SpriteGraphicsOk(uint8_t* base, const Screen& s) {
  const uint8_t* self = Host(base, s.self);
  return Contiguous(Be32(self + 40), Be32(self + 36));
}

void Sprites(PPCContext& __restrict ctx, uint8_t* base) {
  rex::latency::TraceEvent("emulation thread: sprites (the arcade frame's video, first)");
  Screen s;
  if (!LoadScreen(ctx, base, s) || !SpriteGraphicsOk(base, s)) {
    __imp__sub_821966D8(ctx, base);
    return;
  }
  // The guest's first statement: (*(this+744))->vtable[27](), whose result picks the half of the sprite list.
  const uint32_t object = Be32(Host(base, s.self) + 744);
  ctx.r3.u64 = object;
  const uint32_t target = Be32(Host(base, Be32(Host(base, object))) + 108);
  rex::runtime::ResolveIndirectFunction(target)(ctx, base);
  SpritesAfterCall(ctx, base, s);
}

}  // namespace

REX_HOOK_RAW(sub_82196338) {
  static Site site{"guwange_native_tilemap", "tile layers", 0x82196338, kTilemapWords, kTilemapHash};
  switch (ModeOf(site, base)) {
    case kOriginal:
      return __imp__sub_82196338(ctx, base);
    case kNative:
      return Tilemap(ctx, base);
    case kVerify: {
      Screen s;
      if (!LoadScreen(ctx, base, s)) return __imp__sub_82196338(ctx, base);
      static std::atomic<uint32_t> described{0};
      if (described.fetch_add(1, std::memory_order_relaxed) < 3) {
        // What the verify lines need to place a difference: the screen and each layer's registers.
        std::string layers;
        for (int layer = 2; layer >= 0; --layer) {
          const uint8_t* regs = s.mem_h + (uint32_t(layer) << 20) + 0x900000;
          const uint8_t* self = Host(base, s.self);
          layers += fmt::format(" | layer {}: x {:04X} y {:04X} flags {:02X} gfx {:08X} limit {:08X}", layer,
                                Be16(regs), Be16(regs + 2), regs[5], Be32(self + 16 + 8 * layer),
                                Be32(self + 12 + 8 * layer));
        }
        REXLOG_INFO("[native] tile layers: this {:08X} board {:08X} frame buffer {:08X} priority {:08X} pitch {} "
                    "lines {} columns {}{}", s.self, s.mem, s.dest, s.prio, s.pitch, s.lines, s.width, layers);
      }
      return Verify(site, ctx, base, {{s.dest, 2 * Pixels(s)}, {s.prio, Pixels(s)}, {kRowTable, 2048}},
                    __imp__sub_82196338, Tilemap);
    }
  }
}

REX_HOOK_RAW(sub_821966D8) {
  static Site site{"guwange_native_sprites", "sprites", 0x821966D8, kSpritesWords, kSpritesHash};
  switch (ModeOf(site, base)) {
    case kOriginal:
      return __imp__sub_821966D8(ctx, base);
    case kNative:
      return Sprites(ctx, base);
    case kVerify: {
      Screen s;
      if (!LoadScreen(ctx, base, s)) return __imp__sub_821966D8(ctx, base);
      return Verify(site, ctx, base, {{s.dest, 2 * Pixels(s)}, {s.prio, Pixels(s)}, {s.self + 736, 4}},
                    __imp__sub_821966D8, Sprites);
    }
  }
}
