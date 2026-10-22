// Guwange's late input (futari-nx's futari_late_input, for Guwange's frame; the user, 2026-10-22).
//
// A latency trace of the PC build (guwange_diag_latency_trace_s) shows the frame: the guest vblank, the drawing
// thread's Swap a few microseconds later, and right after it the emulation thread's controller reads (sub_82217868,
// XInputGetState for both players); then the arcade frame (the 68000 program, then the board's video: sprites and
// tile layers), the frame buffer texture's unlock and the frame's 21 draws, about 4.8 ms after the reads; then nothing
// until the next vblank, 16.7 ms after the last. The input of a frame is therefore about a whole frame old when it is
// shown, and could be read that much later.
//
// The frame's first controller read after each Swap (the SDK's rex::input::SetFrameReadDelayFunction) waits by what
// the frame can spare: the time to the next vblank less the longest work of the last 64 frames (read to last draw)
// and a margin (guwange_late_input_margin_us), at most guwange_late_input_max_us. The emulation thread starts its
// frame with that read, so the whole arcade frame moves later with newer input and still has its draws queued before
// the vblank the Swap waits for. A frame that ran long (or a loading screen) counts as a whole period: no delay until it
// has left the window; no delay either before 8 frames are measured, or when the read is not in the first half of the
// period after a vblank (a frame running late). Swap, vblank and the frame's own work are unchanged; only when the
// controllers are read moves.
//
// Renut's banjo_late_input (render_thread.cpp) stands aside while this is on: it delays only when the game waits for
// the render thread, which Guwange never does ("late input: no delay").

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>

REXCVAR_DECLARE(bool, futari_late_input);  // src/renut_engine/guwange_renut_glue.cpp (the shared key name)

REXCVAR_DEFINE_INT32(guwange_late_input_margin_us, 3000, "Guwange/Enhancements",
                     "futari_late_input: microseconds kept free before the vertical blank beyond the longest recent "
                     "frame (larger = fewer missed frames, less gain)")
    .range(500, 16000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(guwange_late_input_max_us, 12000, "Guwange/Enhancements",
                     "futari_late_input: the longest delay of the frame's controller read, in microseconds")
    .range(0, 15000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace guwange::late_input {
namespace {

uint64_t NowNs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

std::atomic<bool> g_on{false};
std::atomic<uint64_t> g_vblank_ns{0};        // the last guest vblank
std::atomic<uint64_t> g_period_ns{16666667};  // the vblank period, measured
std::atomic<uint64_t> g_last_draw_ns{0};      // the last draw call (the drawing thread)
uint64_t g_read_ns = 0;                       // when the current frame read its controllers (emulation thread only)
constexpr uint32_t kWindow = 64;
uint64_t g_work_ns[kWindow] = {};  // the last frames' work, read to last draw (emulation thread only)
uint32_t g_work_next = 0;
uint32_t g_work_count = 0;
std::atomic<uint64_t> g_delay_sum_us{0}, g_delays{0}, g_frames{0}, g_work_max_report_ns{0};

uint64_t WorkMax() {
  uint64_t m = 0;
  for (uint32_t i = 0; i < g_work_count; ++i) m = std::max(m, g_work_ns[i]);
  return m;
}

// The frame's first controller read after a Swap, on the emulation thread.
uint32_t FrameReadDelay() {
  const uint64_t now = NowNs();
  const uint64_t period = g_period_ns.load(std::memory_order_relaxed);
  // The frame before: its work, from its read to its last draw (all of it before this Swap). None drawn (a loading
  // screen, a frame that skipped its video) or longer than a period: a whole period.
  if (g_read_ns) {
    const uint64_t draw = g_last_draw_ns.load(std::memory_order_acquire);
    const uint64_t work = draw > g_read_ns ? std::min(draw - g_read_ns, period) : period;
    g_work_ns[g_work_next] = work;
    g_work_next = (g_work_next + 1) % kWindow;
    g_work_count = std::min(g_work_count + 1, kWindow);
    g_work_max_report_ns.store(std::max(g_work_max_report_ns.load(std::memory_order_relaxed), work),
                               std::memory_order_relaxed);
  }
  static const uint64_t margin = uint64_t(REXCVAR_GET(guwange_late_input_margin_us)) * 1000;
  static const uint64_t most = uint64_t(REXCVAR_GET(guwange_late_input_max_us)) * 1000;
  const uint64_t vblank = g_vblank_ns.load(std::memory_order_acquire);
  uint64_t delay = 0;
  if (vblank && g_work_count >= 8 && now >= vblank && now - vblank < period / 2) {
    const uint64_t next = vblank + period;
    const uint64_t need = WorkMax() + margin;
    if (next > now + need) delay = std::min(next - now - need, most);
  }
  g_read_ns = now + delay;
  g_frames.fetch_add(1, std::memory_order_relaxed);
  if (delay) {
    g_delay_sum_us.fetch_add(delay / 1000, std::memory_order_relaxed);
    g_delays.fetch_add(1, std::memory_order_relaxed);
  }
  return uint32_t(delay / 1000);
}

}  // namespace

// The guest vblank (src/renut_engine/futari_renut_vblank.cpp, the vblank thread): its time and period.
void NoteVblank() {
  if (!g_on.load(std::memory_order_relaxed)) return;
  const uint64_t now = NowNs();
  const uint64_t last = g_vblank_ns.exchange(now, std::memory_order_acq_rel);
  if (last && now > last) {
    const uint64_t interval = now - last;
    // One missed vblank (a stall) is not the period; a smoothed period of the plausible intervals.
    if (interval > 10000000 && interval < 25000000) {
      const uint64_t old = g_period_ns.load(std::memory_order_relaxed);
      g_period_ns.store((old * 15 + interval) / 16, std::memory_order_relaxed);
    }
  }
}

// A draw call of the game (the drawing thread: Renut's draw hooks).
void NoteDraw() {
  if (!g_on.load(std::memory_order_relaxed)) return;
  g_last_draw_ns.store(NowNs(), std::memory_order_release);
}

// After the configuration is read (src/guwange_app.h).
void Install() {
  if (!REXCVAR_GET(futari_late_input)) return;
  g_on.store(true, std::memory_order_relaxed);
  rex::input::SetFrameReadDelayFunction(&FrameReadDelay);
  REXLOG_INFO("[latency] late input: the emulation thread's first controller read of each frame waits by what the "
              "frame can spare (margin {} us, at most {} us; futari_late_input)",
              REXCVAR_GET(guwange_late_input_margin_us), REXCVAR_GET(guwange_late_input_max_us));
}

// For the "[latency]" line (render_thread.cpp's LateInputReport).
std::string Report() {
  const uint64_t frames = g_frames.exchange(0, std::memory_order_relaxed);
  const uint64_t delays = g_delays.exchange(0, std::memory_order_relaxed);
  const uint64_t total = g_delay_sum_us.exchange(0, std::memory_order_relaxed);
  const uint64_t work = g_work_max_report_ns.exchange(0, std::memory_order_relaxed);
  if (!frames) return "late input: no frames";
  return fmt::format("late input: the controller read delayed {:.1f} ms on average over {} frames ({} delayed; "
                     "longest frame read -> last draw {:.1f} ms)",
                     double(total) / 1000.0 / double(frames), frames, delays, double(work) / 1e6);
}

}  // namespace guwange::late_input
