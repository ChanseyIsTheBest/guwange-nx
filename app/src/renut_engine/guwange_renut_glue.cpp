// guwange-nx (2026-10-21): what Renut's renderer (futari-nx's copy, app/src/renut_engine) takes from futari-nx's app,
// which Guwange does not build (futari-nx's native renderer src/me and its natives src/native): the vblank keys of its
// native system (me_native_system.cpp, the same names and defaults: futari_renut_vblank.cpp and display_sync.cpp read
// them), its late input key (Futari's own late input hooks Futari's pad read; Guwange has none, so it is off and
// Renut's render-thread late input, banjo_late_input, is the one that applies) and the codegen self-check report
// (futari-nx's post-codegen checks, which Guwange's codegen does not run).

#include <cstdint>

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(futari_vblank_sleep_exact, true, "Guwange/Renut",
                    "The vblank thread sleeps until the exact next vblank instead of 1 ms steps")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_vblank_display_sync, true, "Guwange/Renut",
                    "Switch: the game's vertical blank follows the display's own refresh (vi vsync event) instead "
                    "of a 60 Hz timer of its own (input lag, judder); false = the timer")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(futari_vblank_delay_us, 0, "Guwange/Renut",
                     "Switch, with futari_vblank_display_sync and futari_vblank_phase_auto off: the game's vertical "
                     "blank comes this many microseconds after the display's refresh (0 = at the refresh)")
    .range(0, 12000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_vblank_phase_auto, true, "Guwange/Renut",
                    "Switch, with futari_vblank_display_sync: find the latest guest vblank after the display's refresh "
                    "at which the image is still finished for the next refresh (less input lag); false = "
                    "futari_vblank_delay_us")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(futari_vblank_phase_max_us, 9000, "Guwange/Renut",
                     "futari_vblank_phase_auto: the latest it may put the guest's vblank after the refresh")
    .range(0, 14000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(futari_late_input, true, "Guwange/Renut",
                    "Input lag: the emulation thread's first controller read of each frame waits by what the frame can "
                    "spare, so the arcade frame starts later with newer input (src/guwange_late_input.cpp, futari-nx's "
                    "idea for Guwange's frame); false = right after the Swap, as the game does")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace futari::native {
// futari-nx's post-codegen self-checks report (futari_codegen_checks.cpp): Guwange's codegen runs none of those steps.
void CodegenChecksReport(int /*seconds*/) {}
}  // namespace futari::native
