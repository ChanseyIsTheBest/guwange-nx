// Guwange: the on/off key of every enhancement and the two masters (guwange-nx, 2026-10-21: futari-nx's
// futari_enhancements.cpp with the rows of the keys the Guwange build has: the SDK's and Renut's renderer's; Futari's
// native renderer keys are not built here). The renderer's banjo::EnhancementActive (renut_engine/futari_renut_shim.cpp)
// reads the masters through futari::enhancements::GroupOn.
// On/off keys for every enhancement (see futari_enhancements.h).
//
// The user's rule (2026-10-06): every enhancement, CPU and non-CPU, can be switched off on its own in futari.toml, and
// each group has a master switch, so any change can be A/B-tested on the console without a new build. Build-time
// changes that cannot be switched at run time (codegen, compiler flags, LTO/PGO) are listed in docs/futari-notes.md
// with the build option that makes the A/B pair instead.
//
// Most of the table is the native renderer's and the SDK's own performance work (masseffect-nx / nfsmw-nx / banjo-nx,
// each already a cvar with a self-check where it needs one); masseffect-nx's docs/optimization-paths.md has the console
// measurements of each (e.g. deferred recording: ring thread 62 -> 40 %). The ones whose code default is off are turned
// on by futari_switch.toml / app/configs/futari_cpu_on.toml, as masseffect-nx's best profile does.
//
// Each row: the cvar, its group, what it does, the value it has when the enhancement is ON and the value that turns it
// OFF (a diagnostic that costs time is an enhancement whose ON value is "false"). Values go through the cvar registry
// by name (works for the SDK's cvars too, which live in another DLL on the PC).
//
// To add an enhancement: one row in kTable (its cvar must exist), and its ON value in the Switch configs if the code
// default is OFF.

#include "futari_enhancements.h"

#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/logging.h>

#if REX_PLATFORM_SWITCH
#include <rex/ui/switch_apm.h>
#endif

// Read by the SDK's Switch profiler (sdk/src/ui/switch_perf.cpp), which cannot include the cvar header.
REXCVAR_DEFINE_BOOL(switch_profile_stacks, false, "Guwange",
                    "Switch profiler: sample the pc of every busy thread at 1 kHz (the same as an empty "
                    "logs/rex/stacks_profile.flag); for profiling configs")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Read by the same profiler (sa2-nx's switch_profile_report): false = no logs/rex/rex_profile.log and no sampling (the
// default and the release's built-in settings; the test configs with the profiler turn it on); the overlay's FPS and
// resolution keep going.
REXCVAR_DEFINE_BOOL(switch_profile_report, false, "Guwange",
                    "Switch profiler: write logs/rex/rex_profile.log every 10 s (false = no profile file, no sampling)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// Read by the same profiler (sdk/src/ui/switch_perf.cpp; guwange-nx's CPU round, 2026-10-22): the SDK's runtime
// counters (critical sections, clock calls, waits, ...) are atomic adds on every call, and the console profile had
// them at 4.5 % of the drawing thread. Only the report reads them; the overlay's frame counter always counts.
REXCVAR_DEFINE_BOOL(switch_perf_counters_report_only, true, "Guwange",
                    "Switch: the SDK's runtime counters count only while a profile report is written "
                    "(switch_profile_report); false = always")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(guwange_cpu_enhancements, true, "Guwange",
                    "Master switch of the CPU enhancements: false turns every one of them off (see the "
                    "\"[enhancements]\" log line for the list)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(guwange_noncpu_enhancements, true, "Guwange",
                    "Master switch of the non-CPU enhancements (GPU, memory, I/O): false turns every one of them off")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Verify configuration (app/configs/futari_cpu_verify.toml): the same CPU enhancements, with every self-check of the
// renderer's fast paths comparing on EVERY use instead of the first futari_native_verify_n (2048) /
// futari_native_fast_copy_verify (256). Each comparison runs the original path too, so it is slower: for a test
// session, not for play. Every 10 s the log has a "[verify] checks done: ... | differences: N" line; N must be 0 (a
// difference also logs DIFFERENCE / MISMATCH and turns that one fast path off for the rest of the session).
REXCVAR_DEFINE_BOOL(guwange_cpu_verify, false, "Guwange",
                    "Verify mode: every fast path's self-check compares with the original path on every use; "
                    "\"[verify]\" log lines count the checks and the differences (must be 0). Slower: test only")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// Non-CPU: handheld GPU clock 384 -> 460.8 MHz with the memory clock unchanged (apm configuration 0x92220008, the
// pair commercial games use; UnleashedRecomp-NX and MarathonRecomp-NX ship it on). The SDK's switch_apm sets it back
// if the memory clock moves. Docked: nothing is touched.
REXCVAR_DEFINE_BOOL(guwange_handheld_gpu_boost, true, "Guwange",
                    "Handheld: GPU 460.8 MHz instead of 384 (memory clock unchanged); docked: no effect")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// sasrt-nx's CPU round of 2026-10-12 (through sa2-nx): build-time work with a run-time side. Compare-and-swap (the
// game's interlocked operations, tools/guest_cas.py, and the SDK's lock words, rex/thread/atomic.h) without the legacy
// builtin's trailing `dmb ish` on the A57; off puts the barrier back at run time (sdk/src/core/atomic_posix.cpp).
REXCVAR_DEFINE_BOOL(guwange_cpu_guest_cas_no_barrier, true, "Guwange",
                    "Guest compare-and-swap without the trailing full barrier on the A57 (false = the barrier back)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
#if defined(__aarch64__)
extern "C" unsigned char g_rex_atomic_legacy_barrier;
#endif


namespace futari::enhancements {
namespace {

struct Entry {
  const char* key;
  Group group;
  const char* what;
  const char* on;   // value when the enhancement is active
  const char* off;  // value that turns it off
  // Experimental: never turned on for a key the config leaves out, and written OFF by the generated configs (it must be
  // set to its ON value explicitly to try it).
  bool explicit_only = false;
};
constexpr bool kExplicitOnly = true;

constexpr Group kCpu = Group::kCpu;
constexpr Group kGpu = Group::kNonCpu;

// clang-format off
const Entry kTable[] = {
    // ---- CPU: the renderer's ring thread (masseffect-nx best profile; its optimization-paths.md section 3) --------
    // Deferred recording stays explicit-only (2026-10-07): lockstep captures (vblank_per_swap, frame hashes) found it
    // NOT pixel-identical in Futari. In two runs the Mode Select banner's fade differed on frames 3180-3184 (up to 65
    // levels, run-varying) from five runs without it, which all agreed. On the PC it also made the ring slower (its
    // drains wait for the worker). Its console A/B config: configs/futari_cpu_on_deferred.toml.
    // 2026-10-07 (sa2-nx's Switch settings): 2 = the A57's hardware CRC32 pair instead of XXH3 for the texture
    // recheck / vertex / index fingerprints (XXH3 was 5.8 % of the console ring in build E); builds without +crc (the
    // PC) use 1.
    // sa2-nx since its round 3: the upload buffer (vertices, indices, constants) in CPU-cached memory, flushed before
    // each submit (console: 5,251 MB/s written against 2,506 uncached). Switch only: on a discrete PC GPU the cached
    // type is system memory (banjo-nx: its ring 2.5-3x slower per draw there).
#if REX_PLATFORM_SWITCH
#endif
    {"futari_vblank_sleep_exact", kCpu, "vblank thread sleeps to the exact vblank", "true", "false"},
    // ---- CPU: SDK (guest kernel / threads), masseffect-nx section 4 ---------------------------------------------
    {"thread_wait_blocking", kCpu, "SDK: waits parked on a wake point, not 1 ms polls", "true", "false"},
    {"clock_nonblocking", kCpu, "SDK: guest clock update never blocks", "true", "false"},
    {"clock_lockfree", kCpu, "SDK: guest clock without the clock mutex (epoch + atomic maximum; no lost rounding at the Switch's 125/48 tick ratio)", "true", "false"},
    // ---- CPU: natives of the console profile's hottest guest functions (src/cpu; 2026-10-22) ---------------------
    {"guwange_native_memcpy", kCpu, "native: default.xex's XDK memcpy (sub_82244530) as host memcpy", "true", "false"},
    {"guwange_native_memset", kCpu, "native: default.xex's XDK memset (sub_82243BF0) as host memset", "true", "false"},
    {"guwange_native_tilemap", kCpu, "native: the arcade board's tile layers (sub_82196338, 46 % of the emulation thread)", "true", "false"},
    {"guwange_native_sprites", kCpu, "native: the arcade board's sprites (sub_821966D8, 15 % of the emulation thread)", "true", "false"},
    {"banjo_renut_neon_texels", kCpu, "Renut (ARM): 16-bit texel conversions by table lookups, 8 texels per NEON step (Guwange's 1-5-5-5 frame buffer)", "true", "false"},
    {"banjo_renut_fused_texel_rows", kCpu, "Renut: a linear 16-bit texture row read, swapped and converted in one pass (no row buffer copy)", "true", "false"},
    {"banjo_renut_native_16bit_textures", kCpu, "Renut (deko3d): linear 16-bit textures (Guwange's frame buffer) in their own 5-5-5-1 / 5-6-5 / 4-4-4-4 format: half the upload and cache clean, no conversion", "true", "false"},
    {"futari_late_input", kCpu, "Input lag: the emulation thread's controller read as late as the frame allows (Guwange's version of futari-nx's late input)", "true", "false"},
    {"switch_perf_counters_report_only", kCpu, "SDK (Switch): runtime counters only while a profile report is written; one counter per cache line", "true", "false"},
    {"global_lock_spin", kCpu, "SDK: bounded spin on the object-table lock", "48", "0"},
    // ---- CPU: native replacements of hot guest functions (src/native; code-checked, verified by futari_cpu_verify) --
    {"futari_native_hot_821536C0", kCpu, "native: localized audio mix sub_821536C0 (mixer thread)", "true", "false"},
    {"futari_native_hot_821528D0", kCpu, "native: localized mixer sub_821528D0 (mixer thread)", "true", "false"},
    {"futari_native_hot_8212BE20", kCpu, "native: localized resampler sub_8212BE20 (mixer thread)", "true", "false"},
    {"futari_native_hot_821268F8", kCpu, "native: localized mixer sub_821268F8 (mixer thread)", "true", "false"},
    // ---- CPU: the codegen rounds of 1.0.1 (sasrt-nx / sa2-nx, 2026-10-12): the run-time keys. The build-time steps
    // have their switches in app/CMakeLists.txt (FUTARI_SWITCH_*, FUTARI_GEN_*) and app/perf_overrides.toml.
    {"guwange_cpu_guest_cas_no_barrier", kCpu, "game code + SDK: compare-and-swap without the trailing dmb ish (A57)", "true", "false"},
    {"kernel_critsec_pcr_owner", kCpu, "SDK: critical sections take the owner thread from the guest's PCR, no host thread lookup", "true", "false"},
#if REX_PLATFORM_SWITCH
    // threading_posix.cpp, which only the Switch build compiles: the key does not exist on the PC.
    {"thread_signal_skip_idle", kCpu, "SDK: no condition-variable notify (a syscall) for an event or semaphore nobody waits on", "true", "false"},
#endif
    // ---- non-CPU: GPU work and memory ---------------------------------------------------------------------------
    {"guwange_handheld_gpu_boost", kGpu, "handheld GPU 460.8 MHz", "true", "false"},
    // Futari's click fix: Guwange does not cut its sounds on frame edges, so the release has it off (the user,
    // 2026-10-22: "we don't need it for this game"); each key still turns it on.
    {"audio_switch_declick", kGpu, "audio: a sound the game cuts or starts mid-wave on a frame edge is faded instead of clicking (Switch)", "true", "false", kExplicitOnly},
    {"audio_switch_declick_smooth", kGpu, "audio: that fade over 5.3 ms on a raised cosine, not 1.3 ms linear (crackles)", "true", "false", kExplicitOnly},
    {"audio_switch_declick_predict", kGpu, "audio: steps hidden in busy sound caught too (against the slope)", "true", "false", kExplicitOnly},
    // Input lag (2026-10-16; docs/futari-notes.md, "Input lag"): each its own key, the probe's "[latency]" line says
    // what they do.
    {"futari_late_input", kGpu, "input lag: the game reads its controllers as late as its frame allows (adaptive)", "true", "false"},
#if REX_PLATFORM_SWITCH
    {"futari_vblank_display_sync", kGpu, "input lag / judder: the game's vertical blank follows the display's refresh", "true", "false"},
    {"futari_vblank_phase_auto", kGpu, "input lag: the game's vertical blank as late after the refresh as the image still makes the next one", "true", "false"},
    {"futari_renut_vblank_core", kGpu, "Renut renderer: the vblank thread at the render thread's priority on the background core (banjo-nx's display thread), not behind the game thread", "2", "-1"},
#endif
    // futari_gpu_marks_categories is not a row: it is half of the profiler (with switch_profile_stacks), on in every
    // config but the verify one, and no enhancement master may switch it (the user's request, 2026-10-06).
    // ---- Renut's renderer (futari_renderer = "renut"; banjo-nx's keys, src/renut_engine/gpu) -------------------------
    // 2026-10-20: its optimisations under the two masters as well (they read their own keys, which a master off sets
    // to OFF here). Not rows: its fixes (half-pixel offset, point radii, stencil words, the texture header swizzle,
    // alpha to mask...), its settings, and banjo-nx's game-specific ones (the fur and light shader lists, fur shells,
    // its display thread, store cards: no codegen pass in Futari). Its late input acts only with futari_late_input off.
    // CPU: the render thread.
    {"banjo_deko3d_push_constants", kCpu, "Renut: float and shared constants pushed into fixed uniform buffers (render-thread proposals 2, 5)", "true", "false"},
    {"banjo_deko3d_ring_cleaner", kCpu, "Renut: the upload ring's cache clean on the background core (render-thread proposal 23)", "true", "false"},
    {"banjo_renut_capture_pool", kCpu, "Renut: queued draws' captures from a pool", "true", "false"},
    {"banjo_renut_snapshot_deltas", kCpu, "Renut (sa2-nx): a draw keeps only the device chunks and constant groups changed since the previous one (whole device before)", "true", "false"},
    {"futari_renut_deferred_chain", kCpu, "Renut: Begin*Vertices draws (88 % of Futari's) deltas of the snapshot chain too, not whole copies", "true", "false"},
    {"banjo_renut_snapshot_deltas_constants", kCpu, "Renut (sa2-nx): the chain's draws push only the constant groups that changed", "true", "false"},
    {"banjo_renut_snapshot_deltas_narrow", kCpu, "Renut (sa2-nx): only the device chunks the render thread reads compared", "true", "false"},
    {"banjo_renut_neon_compare", kCpu, "Renut (sa2-nx): 64-byte lines compared with NEON (4 loads, XOR, OR, one max)", "true", "false"},
    {"banjo_renut_capture_batches", kCpu, "Renut (sa2-nx): the capture pool taken and given back 32 at a time (no lock per draw)", "true", "false"},
    {"banjo_renut_callbacks_fast", kCpu, "Renut (sa2-nx): the hooks' callback and pending-vertex checks return at once when there is nothing", "true", "false"},
    {"banjo_renut_prefetch_next", kCpu, "Renut (sa2-nx): the next queued draw's capture prefetched", "true", "false"},
    {"banjo_renut_done_notify_waiters", kCpu, "Renut (sa2-nx): the render thread's done signal only with a waiter", "true", "false"},
    {"banjo_renut_batch_clock", kCpu, "Renut (sa2-nx): one clock read per batch of render work", "true", "false"},
    {"banjo_renut_chain_reuse", kCpu, "Renut (sa2-nx): a draw reuses the previous draw's key, targets, viewport, textures and pipeline when nothing they come from changed", "true", "false"},
    {"banjo_renut_chain_reuse_replays", kCpu, "Renut: the chain's reuse for recorded command-buffer replays too", "true", "false"},
    {"banjo_renut_inline_line_compare", kCpu, "Renut (sa2-nx): shared constant lines compared inline (no memcmp calls)", "true", "false"},
    {"banjo_renut_fetch_slots_skip", kCpu, "Renut (sa2-nx): texture and fetch-slot lines known equal not compared", "true", "false"},
    {"banjo_renut_shared_partial_clear", kCpu, "Renut (sa2-nx): only the part of the shared constants after the texture words cleared", "true", "false"},
    {"banjo_renut_fetch_data_unread", kCpu, "Renut (sa2-nx): no fetch-data upload for draws that read none (the line stops changing)", "true", "false"},
    {"banjo_renut_stream_mask", kCpu, "Renut (sa2-nx): vertex streams walked by a mask", "true", "false"},
    {"banjo_renut_texture_defaults_copy", kCpu, "Renut (sa2-nx): default texture words as one block copy", "true", "false"},
    {"banjo_renut_lean_diagnostics", kCpu, "Renut (sa2-nx): per-draw diagnostic counts only while their lines are logged", "true", "false"},
    {"banjo_renut_core_layout", kCpu, "Renut (Switch, sa2-nx): the drawing thread alone on core 0, the other guest threads on cores 1-2", "true", "false"},
    {"guwange_core_layout_drawing", kCpu, "Switch: Guwange's drawing thread alone on core 0 (its main thread only sleeps), the emulation and the rest on cores 1-2", "true", "false"},
    {"banjo_renut_wake_batch", kCpu, "Renut (sa2-nx): the sleeping render thread woken every 16 queued draws, not every draw", "16", "1"},
    {"banjo_renut_cpu_memos", kCpu, "Renut: the render thread's per-draw lookups memoised (targets, textures, streams, memexport)", "true", "false"},
    {"banjo_renut_pipeline_memo", kCpu, "Renut: a memo of the pipeline each key gave", "true", "false"},
    {"banjo_renut_shared_dedupe", kCpu, "Renut: SharedConstants equal to the previous draw's not uploaded again", "true", "false"},
    {"banjo_renut_queue_notify_idle", kCpu, "Renut: queued work wakes the render thread only while it waits", "true", "false"},
    {"banjo_renut_record_worker", kCpu, "Renut: command recording split across two threads", "true", "false"},
    {"banjo_renut_pmull_hash", kCpu, "Renut (ARM): content hashes with PMULL folding", "true", "false"},
    {"banjo_renut_neon_swap", kCpu, "Renut (ARM): vertex byte swaps 16 bytes per NEON reverse", "true", "false"},
    {"banjo_renut_upload_cached", kCpu, "Renut (Switch): the upload ring in CPU-cached memory", "true", "false"},
    {"banjo_ring_core", kCpu, "Renut (Switch): the render thread pinned to core 1", "1", "-1"},
    {"banjo_background_compile_core", kCpu, "Renut (Switch): pipeline prewarm and specialisation threads pinned to core 2", "2", "-1"},
    {"banjo_renut_record_worker_core", kCpu, "Renut (Switch): the recording worker pinned to core 0", "0", "-1"},
    {"masseffect_native_nvk_emission", kCpu, "Renut on NVK: each draw command written in one go", "true", "false"},
    {"masseffect_native_nvk_cbufs", kCpu, "Renut on NVK: only the cbuf slots that can get dirty looked at", "true", "false"},
    {"masseffect_native_nvk_dynamic", kCpu, "Renut on NVK: only the dirty dynamic state groups emitted", "true", "false"},
    {"masseffect_native_nvk_preload", kCpu, "Renut on NVK: PRFM hints for the pipeline and shaders", "true", "false"},
    {"masseffect_native_set4_differences", kCpu, "Renut on NVK: descriptor set 4 by differences", "true", "false"},
    // Non-CPU: GPU work, memory, input latency.
    {"banjo_deko3d_alpha_spec", kGpu, "Renut deko3d: the alpha test's function in the pipeline (constant 903)", "true", "false"},
    {"banjo_deko3d_bound_fetch_spec", kGpu, "Renut deko3d: the bound fetches' declaration types in the pipeline (constants 920-935)", "true", "false"},
    {"banjo_deko3d_tess_spec", kGpu, "Renut deko3d: no tessellation code in pipelines without it (constant 904)", "true", "false"},
    {"banjo_deko3d_early_hoist", kGpu, "Renut deko3d: the early alpha test's texture fetch hoisted", "true", "false"},
    {"banjo_deko3d_early_z", kGpu, "Renut deko3d: depth tested before a discarding shader that writes no depth (constant 909)", "true", "false"},
    {"banjo_deko3d_txq_constants", kGpu, "Renut deko3d: texture sizes from a buffer, not a query", "true", "false"},
    {"banjo_deko3d_vs_outputs", kGpu, "Renut deko3d: vertex shaders write only what the pixel shader reads", "true", "false"},
    {"banjo_deko3d_uam_prmt", kGpu, "Renut deko3d: uam's byte swaps as PRMT", "true", "false"},
    {"banjo_deko3d_uam_gvn", kGpu, "Renut deko3d: uam's global value numbering", "true", "false"},
    {"banjo_deko3d_uam_uniform_branches", kGpu, "Renut deko3d: uam's uniform branches without reconvergence", "true", "false"},
    {"banjo_deko3d_zcull", kGpu, "Renut deko3d: hierarchical depth culling", "true", "false"},
    {"banjo_deko3d_depth_compression", kGpu, "Renut deko3d: compressed depth targets", "true", "false"},
    {"banjo_deko3d_depth24", kGpu, "Renut deko3d: D24S8 targets as 24-bit depth", "true", "false"},
    {"banjo_deko3d_depth_handover", kGpu, "Renut deko3d: a whole depth resolve handed over, not drawn", "true", "false"},
    {"banjo_deko3d_draw_resolves", kGpu, "Renut deko3d: colour resolves drawn on the 3D engine", "true", "false"},
    {"banjo_deko3d_barrier_lean", kGpu, "Renut deko3d: write-only transitions keep the texture cache", "true", "false"},
    {"banjo_deko3d_barrier_merge", kGpu, "Renut deko3d: back-to-back barriers merged", "true", "false"},
    {"banjo_gpu_depth_only", kGpu, "Renut: depth-only pipelines for colourless draws", "true", "false"},
    {"banjo_gpu_depth_words_view", kGpu, "Renut: a depth resolve's words read through its own texture (fewer copies)", "true", "false"},
    {"banjo_gpu_output_trim", kGpu, "Renut: colour outputs a pipeline never writes trimmed", "true", "false"},
    {"banjo_gpu_bool_spec", kGpu, "Renut: bool constants as specialisation constants", "true", "false"},
    {"banjo_gpu_tess_grid", kGpu, "Renut: tessellated patches as a precomputed grid", "true", "false"},
    {"banjo_gpu_robustness_off", kGpu, "Renut (Vulkan): pipelines without buffer and image robustness", "true", "false"},
    {"banjo_gpu_nvk_draw_contract", kGpu, "Renut on NVK: NVK's per-draw contract", "true", "false"},
    {"banjo_gpu_blend_skip", kGpu, "Renut: pixels whose blend changes nothing discarded", "true", "false"},
    {"banjo_renut_precise_barriers", kGpu, "Renut (Vulkan): barriers' access masks from the layouts", "true", "false"},
    {"banjo_renut_push_point_only", kGpu, "Renut (Vulkan): the push constant read only by point sprites", "true", "false"},
    {"banjo_renut_resolve_skip", kGpu, "Renut: a resolve of an unchanged source into the same texture not done again", "true", "false"},
    {"banjo_renut_depth_words_lazy", kGpu, "Renut: a depth resolve's words written only when read", "true", "false"},
#if REX_PLATFORM_SWITCH  // (off by default on the PC)
    {"banjo_renut_memory_policy", kGpu, "Renut: memory limits sized for the console", "true", "false"},
#endif
    {"banjo_renut_pipeline_cache", kGpu, "Renut (Vulkan): the driver's pipelines kept on disk", "true", "false"},
    {"banjo_renut_cache_save_quiet", kGpu, "Renut: the pipeline cache saved once no pipeline was built for a while", "true", "false"},
    {"banjo_renut_linked_stages", kGpu, "Renut on NVK (Switch): each pipeline's stages compiled together", "true", "false"},
    {"banjo_deko3d_kick_submit", kGpu, "Renut deko3d, input lag: command lists handed to the GPU at the game's kicks", "true", "false"},
    {"banjo_deko3d_eager_kicks", kGpu, "Renut deko3d, input lag: lists of 100 draws (6 a frame), each submitted at its own kick", "true", "false"},
    {"banjo_late_input", kGpu, "Renut, input lag: the controllers read as late as the render thread's slack allows (only with futari_late_input off)", "true", "false"},
    {"futari_renut_double_buffer", kGpu, "Renut, input lag: a 2-image swapchain (one image less queued before the display)", "true", "false"},
    // banjo_deko3d_latency_trim is not a row: off by default as banjo-nx's until a console measures it (a row would put
    // it on in the built-in defaults); its own check honours the non-CPU master (banjo_renut_tuning.cpp).
};
// clang-format on

bool Active(const Entry& e) {
  const std::string value = rex::cvar::GetFlagByName(e.key);
  return !value.empty() && value != e.off;
}

}  // namespace

bool GroupOn(Group group) {
  return group == Group::kCpu ? REXCVAR_GET(guwange_cpu_enhancements) : REXCVAR_GET(guwange_noncpu_enhancements);
}

void Apply() {
  std::string active, inactive;
  size_t active_count = 0;
  size_t defaulted_on = 0;
  for (const Entry& e : kTable) {
    if (!GroupOn(e.group) && Active(e)) {
      rex::cvar::SetFlagByName(e.key, e.off);
    }
    // A key the config does not mention means ON (its group master decides): a futari.toml from an older build,
    // which lacks the newer keys, used to leave them at their code defaults, mostly OFF. That is what a console run
    // on 2026-10-06 showed: 21 CPU enhancements off, today's fixes among them. A key set in the config, the
    // environment or on the command line is left as it is.
    // Deferred recording is experimental here: with futari_native_output_no_wait (on by default) the PC stalled
    // ~100 ms on GPU waits around copies, 23 fps instead of 60 (2026-10-06); either one alone is fine.
    if (GroupOn(e.group) && !Active(e) && !e.explicit_only &&
        rex::cvar::GetFlagSource(e.key) == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName(e.key, e.on);
      ++defaulted_on;
    }
    const bool on = Active(e);
    std::string& list = on ? active : inactive;
    if (!list.empty()) {
      list += ", ";
    }
    list += e.key;
    active_count += on;
  }
#if defined(__aarch64__)
  // guwange_cpu_guest_cas_no_barrier: off (or the CPU master off) puts the legacy `dmb ish` back.
  g_rex_atomic_legacy_barrier = REXCVAR_GET(guwange_cpu_guest_cas_no_barrier) ? 0 : 1;
#endif
#if REX_PLATFORM_SWITCH
  if (REXCVAR_GET(guwange_handheld_gpu_boost)) {
    RexSwitchApmRequestGpuMhz(460);
    RexSwitchApmApply();
  }
#endif
  REXLOG_INFO("[enhancements] masters: CPU {}, non-CPU {} | {} of {} active ({} not in the config, turned on): {} | "
              "off: {}",
              REXCVAR_GET(guwange_cpu_enhancements) ? "on" : "OFF",
              REXCVAR_GET(guwange_noncpu_enhancements) ? "on" : "OFF", active_count, std::size(kTable), defaulted_on,
              active.empty() ? "none" : active, inactive.empty() ? "none" : inactive);
}

}  // namespace futari::enhancements
