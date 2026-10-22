// Guwange: the ReXGlue application object (paths, renderer choice, analysis aids). futari-nx's FutariApp.
//
// Renderers: Renut's Direct3D-level renderer (futari_renderer = "renut", the default; deko3d on the Switch, D3D12 or
// Vulkan on the PC), or the SDK's emulated Xenos GPU (futari_renderer = "default": the PC reference, PM4 ring and EDRAM
// model; not on the Switch, which links no Vulkan driver).

#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <system_error>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>

#include "futari_enhancements.h"
#include "guwange_capture.h"
#include "guwange_input_script.h"
#include "guwange_pc_profiler.h"
#include "guwange_pgo.h"
// Renut's Direct3D-level renderer (cmake/guwange_renut_gpu.cmake).
#if FUTARI_RENUT_RENDERER
#include <rex/frame_latency.h>
#include <rex/ui/immediate_drawer.h>
#include "renut_engine/gpu/banjo_renut_capture.h"
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "renut_engine/futari_renut_vblank.h"
#endif
#if defined(GUWANGE_BUILTIN_CONFIG)
#include "guwange_builtin_config.h"  // generated from app/guwange_switch_defaults.toml and guwange_switch.toml (CMake)
#endif

namespace guwange::diag {
void Start(rex::Runtime* runtime);
void Stop();
}  // namespace guwange::diag
namespace guwange::codegen_checks {
void Start();  // src/guwange_codegen_checks.cpp
}  // namespace guwange::codegen_checks
namespace guwange::late_input {
void Install();  // src/guwange_late_input.cpp
}  // namespace guwange::late_input

class GuwangeApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<GuwangeApp>(new GuwangeApp(ctx, "guwange", PPCImageConfig));
  }

  // The Switch build's fixed settings (app/guwange_switch_defaults.toml, compiled in): applied before guwange.toml,
  // which holds only the player's options (and may still override any of these).
  std::string_view BuiltInConfig() const override {
#if defined(GUWANGE_BUILTIN_CONFIG)
    return kGuwangeBuiltInConfig;
#else
    return {};
#endif
  }
  // Without a guwange.toml next to the NRO, the player's default one is written there (app/guwange_switch.toml).
  std::string_view DefaultConfigFile() const override {
#if defined(GUWANGE_BUILTIN_CONFIG)
    return kGuwangePlayerConfig;
#else
    return {};
#endif
  }

  void OnPostInitLogging() override {
    // The runtime defaults to no GPU plugin: select ReXGlue's Xenos implementation (used with futari_renderer =
    // "default"), keeping an explicit command-line or config-file choice.
    if (!rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
#if FUTARI_RENUT_RENDERER
    // Renut's renderer is Guwange's renderer (the emulated GPU stays the PC reference: futari_renderer = "default").
    if (!rex::cvar::HasNonDefaultValue("futari_renderer")) {
      rex::cvar::SetFlagByName("futari_renderer", "renut");
    }
#if REX_PLATFORM_SWITCH
    if (!rex::cvar::HasNonDefaultValue("renut_gpu_backend")) {
      rex::cvar::SetFlagByName("renut_gpu_backend", "deko3d");
    }
#endif
#endif
    // Guwange binds its textures through fetch constants whose type field is 0 (dword 0 = 0x10000000, 0x0A000000...),
    // not 2: the emulated GPU skips such textures by default and the whole picture stays black (first boots,
    // 2026-10-21). gpu_allow_invalid_fetch_constants = true is in guwange.toml: the GPU plugin registers that key only
    // after this point, so setting it here has no effect.
    //
    // The XBLA licence: bit 0 of the mask is the purchased game (license_mask 0, the SDK's default, is the trial:
    // the main menu offers "Unlock Full Game"). An explicit setting still wins.
    if (!rex::cvar::HasNonDefaultValue("license_mask")) {
      rex::cvar::SetFlagByName("license_mask", "1");
    }
    // Enhancement masters (guwange_cpu_enhancements / guwange_noncpu_enhancements) and the "[enhancements]" log line,
    // before anything reads the keys.
    futari::enhancements::Apply();
    // The PC self-checks of the post-codegen steps (src/guwange_codegen_checks.cpp): the "[verify] codegen checks"
    // line every 10 s. Nothing on the Switch.
    guwange::codegen_checks::Start();
    // Input lag (futari_late_input, src/guwange_late_input.cpp): the frame's controller read as late as it allows.
    guwange::late_input::Install();
  }

  // Without --game_data_root, use a game_root/ folder next to the executable (on the Switch:
  // sdmc:/switch/guwange-nx/game_root), and on a PC build tree the repository's assets/game_root.
  // An explicit path always wins.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;
    }
    std::error_code ec;
#if REX_PLATFORM_SWITCH
    // A loader that gives the NRO no path (Ryujinx's homebrew loader): the release's own folder, then the SD's root.
    for (const char* fixed : {"/switch/guwange-nx/game_root", "/game_root"}) {
      if (std::filesystem::is_regular_file(std::filesystem::path(fixed) / "default.xex", ec)) {
        const auto exe = rex::filesystem::GetExecutableFolder();
        if (exe.empty() || !std::filesystem::is_regular_file(exe / "game_root" / "default.xex", ec)) {
          paths.game_data_root = fixed;
          return;
        }
        break;
      }
    }
#endif
    const auto folder = rex::filesystem::GetExecutableFolder();
    if (folder.empty()) {
      return;
    }
    for (auto candidate = folder; !candidate.empty(); candidate = candidate.parent_path()) {
      for (const auto& sub : {std::filesystem::path("game_root"), std::filesystem::path("assets") / "game_root"}) {
        if (std::filesystem::is_regular_file(candidate / sub / "default.xex", ec)) {
          paths.game_data_root = candidate / sub;
          return;
        }
      }
      if (candidate == candidate.parent_path()) {
        break;
      }
    }
  }

  // Renut's renderer: no SDK graphics system and no GPU plugin (the SDK's detached mode).
  void OnPreSetup(rex::RuntimeConfig& config) override {
#if FUTARI_RENUT_RENDERER
    if (banjo::renut_gpu::Active()) {
      renut::gpu::ConfigureRuntime(config);
      REXLOG_INFO("graphics: Renut's Direct3D-level renderer (plume; no PM4 ring, no EDRAM model)");
      return;
    }
#endif
    (void)config;
  }

  void OnPostSetup() override {
    // Unattended runs: a scripted pad (guwange_input_script) to get through the title screen and menus.
    guwange::input_script::Install(runtime());
#if FUTARI_RENUT_RENDERER
    // Renut's renderer has no GPU: the guest's vertical blank comes from a thread of its own (futari_renut_vblank.h).
    if (banjo::renut_gpu::Active()) futari::renut_vblank::Start();
    // Renut's renderer has no SDK presenter: its own Present reads the frame back (banjo_renut_capture.h).
    if (banjo::renut_gpu::Active()) {
      guwange::capture::Start([](rex::ui::RawImage& image) { return renut::gpu::CaptureFrame(image, 100); },
                              [] { return rex::latency::SwapCount(); });
    } else
#endif
    // Video recording of the game image for testing (guwange_video_s = N seconds after start; 0 = off). Taken from
    // the presenter's guest output, so it works minimized and nothing can cover it.
    guwange::capture::Start([this]() -> rex::ui::Presenter* {
      const auto* rt = runtime();
      const auto* graphics = rt ? rt->graphics_system() : nullptr;
      return graphics ? graphics->presenter() : nullptr;
    });
    // PC: function-level CPU profile (guwange_pc_profile_s = N seconds per report; 0 = off).
    guwange::pc_profiler::Start();
    // Development: guest words logged periodically (guwange_diag_watch).
    guwange::diag::Start(runtime());
    // The PGO-instrumented build (GUWANGE_PGO=generate) saves its counts to pgo/ every 2 minutes (nothing otherwise),
    // and before the hard exit the title's end takes (Return to Game Library: no teardown runs).
    guwange::pgo::Start();
    rex::ReXApp::SetBeforeHardExit(&guwange::pgo::Stop);
  }

  // The PGO-instrumented build saves its counts when the game loses focus (HOME menu): it may be closed from there.
  void OnWindowFocusChanged(bool focused) override {
    if (!focused) guwange::pgo::DumpNow();
  }

  void OnShutdown() override {
    guwange::pgo::Stop();
    guwange::diag::Stop();
    guwange::pc_profiler::Stop();
    guwange::capture::Stop();
#if FUTARI_RENUT_RENDERER
    if (banjo::renut_gpu::Active()) {
      futari::renut_vblank::Stop();
      renut::gpu::ShutdownRenderer();
    }
#endif
  }

#if FUTARI_RENUT_RENDERER
  // Renut's renderer draws the SDK's ImGui through its own plume drawer, and resizes its swapchain itself.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (banjo::renut_gpu::Active()) return renut::gpu::CreateOverlayDrawer();
    return rex::ReXApp::OnCreateImmediateDrawer();
  }
  void OnWindowPixelSizeChanged(uint32_t pixel_width, uint32_t pixel_height) override {
    rex::ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
    if (banjo::renut_gpu::Active()) renut::gpu::OnWindowResized();
  }
  // The window exists here (UI thread, before the game starts): Renut's renderer comes up first.
  std::optional<rex::PathConfig> OnFinalizePaths(const rex::PathConfig& defaults,
                                                 std::function<void(rex::PathConfig)> resume) override {
    if (banjo::renut_gpu::Active()) {
      renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); });
    }
    return rex::ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }
#endif

  // Analysis aid: with GUWANGE_DUMP_IMAGE=<file>, write the loaded guest image (image_base..image_base+image_size)
  // so code pointers stored in data can be scanned for. Does nothing unless the variable is set.
  void OnPostLoadXexImage() override {
    if (!runtime() || !runtime()->memory()) {
      return;
    }
    const char* path = std::getenv("GUWANGE_DUMP_IMAGE");
    if (!path) {
      return;
    }
    const uint8_t* image = runtime()->memory()->TranslateVirtual<const uint8_t*>(PPCImageConfig.image_base);
    if (std::FILE* f = std::fopen(path, "wb")) {
      std::fwrite(image, 1, PPCImageConfig.image_size, f);
      std::fclose(f);
      REXLOG_INFO("guest image written to {}", path);
    }
  }
};
