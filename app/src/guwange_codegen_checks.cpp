// The PC side of the post-codegen steps (tools/guwange_post_codegen.py; futari-nx's futari_codegen_checks.cpp and the
// check half of futari_native_core.cpp, for Guwange's five modules). The Switch build has no code at any of these
// places: the macros of the generated pch are the bare operation there. Off the Switch each keeps the old operation
// next to the new one, counts itself and calls a Rex*Mismatch function when its proof was wrong:
//   - tools/ro_constants.py, tools/ro_constants_cfg.py: a load from read-only data folded to its constant still
//     loads (g_rex_ro_checked; RexRoMismatch when the memory does not hold the constant);
//   - tools/indirect_calls.py: a promoted indirect call (g_rex_promoted_calls; RexPromotedMismatch when the dispatch
//     table's entry for the guarded target is not the function called);
//   - tools/flush_mode_flow.py: a flush-mode check proven redundant keeps its test (g_rex_flush_flow_checked;
//     RexFlushFlowMismatch when the mode had to change after all);
//   - tools/r1_local.py: after a call the r1 mirror is compared with ctx.r1 (REX_R1_CHECK: g_rex_r1_checked,
//     RexR1Mismatch, which also reloads the mirror);
//   - tools/lr_preserve.py: an epilogue write-back proven to store what ctx.lr holds compares the two
//     (REX_LR_WRITE_SAME_CHECK: g_rex_lr_write_checked, RexLrWriteMismatch);
//   - the census of indirect calls (guwange_diag_indirect_census = a file): per module and call site how often it ran
//     and where to, the input of tools/indirect_promotions.py;
//   - guwange_diag_rdata_check = a folder with the generator's image dumps (out): the read-only data of every loaded
//     module is compared with its dump, import records aside (folding assumes exactly that, also for the loads no PC
//     session reaches);
//   - tools/fp_sign_ops.py is exact while the game never selects round up or round down: the "[census] FPSCR
//     rounding mode" line counts the guest's FPSCR writes by mode.
// So every PC run tests what it executes: every 10 s a "[verify] codegen checks" line counts the checks and the
// differences (must be 0). A game core is a DLL on the PC: this file is compiled into each core too (GUWANGE_CORE_NAME
// set), with the core's own counters; its differences go to the executable's exports below, and the executable reads
// the core's counts through GuwangeCoreCheckCounts.
#include <atomic>
#include <cstdint>

#if defined(_WIN32)
#define GUWANGE_CHECKS_EXPORT extern "C" __declspec(dllexport)
#else
#define GUWANGE_CHECKS_EXPORT extern "C" __attribute__((visibility("default")))
#endif

#if defined(GUWANGE_CORE_NAME)

// ---- A game core (a DLL on the PC; on the Switch the cores are linked into the NRO and have no checks) ----
#if !defined(__SWITCH__)
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

extern "C" {
volatile unsigned long long g_rex_flush_flow_checked = 0;  // plain lossy counts: "at least this many"
volatile unsigned long long g_rex_ro_checked = 0;
volatile unsigned long long g_rex_promoted_calls = 0;
volatile unsigned long long g_rex_r1_checked = 0;
volatile unsigned long long g_rex_lr_write_checked = 0;
// On until the first tagged call has asked the executable (the DLL may be loaded before the config is read).
unsigned char g_rex_indirect_census = 1;
}

namespace {
// The executable's report: GuwangeCodegenMismatch(module, kind, a, b) (kinds as in the executable's half below).
void Report(int kind, unsigned long long a, unsigned long long b) {
#if defined(_WIN32)
  static const auto report = reinterpret_cast<void (*)(const char*, int, unsigned long long, unsigned long long)>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "GuwangeCodegenMismatch")));
  if (report) report(GUWANGE_CORE_NAME, kind, a, b);
#else
  (void)kind, (void)a, (void)b;
#endif
}
}  // namespace

extern "C" {
void RexFlushFlowMismatch(int on) noexcept { Report(0, unsigned(on), 0); }
void RexRoMismatch(unsigned address) noexcept { Report(1, address, 0); }
void RexPromotedMismatch(unsigned target) noexcept { Report(2, target, 0); }
void RexR1Mismatch(unsigned long long expected, unsigned long long got) noexcept { Report(3, expected, got); }
void RexLrWriteMismatch(unsigned long long held, unsigned long long written) noexcept { Report(4, held, written); }
void RexIndirectCensus(unsigned function, unsigned site, unsigned target) noexcept {
#if defined(_WIN32)
  static const auto on = reinterpret_cast<bool (*)()>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "GuwangeIndirectCensusOn")));
  static const auto census = reinterpret_cast<void (*)(const char*, unsigned, unsigned, unsigned)>(
      reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(nullptr), "GuwangeCoreIndirectCensus")));
  if (!on || !census || !on()) {
    g_rex_indirect_census = 0;  // the census is off: no further calls from this core
    return;
  }
  census(GUWANGE_CORE_NAME, function, site, target);
#else
  (void)function, (void)site, (void)target;
  g_rex_indirect_census = 0;
#endif
}
}

// The core's counts for the executable's report: flush-mode flow, read-only constants, promoted calls, r1, lr.
GUWANGE_CHECKS_EXPORT void GuwangeCoreCheckCounts(unsigned long long* out) {
  out[0] = g_rex_flush_flow_checked;
  out[1] = g_rex_ro_checked;
  out[2] = g_rex_promoted_calls;
  out[3] = g_rex_r1_checked;
  out[4] = g_rex_lr_write_checked;
}
#endif  // !__SWITCH__

#else  // GUWANGE_CORE_NAME

// ---- The executable ----
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/sys_counters.h>

REXCVAR_DEFINE_STRING(guwange_diag_indirect_census, "", "Guwange/Diagnostics",
                      "Diagnostic (PC): a file that gets, every 10 s, the census of the game's indirect calls (per "
                      "module and call site: calls and targets). tools/indirect_promotions.py makes "
                      "app/indirect_promotions/<module>.txt from it. Empty = no census")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_STRING(guwange_diag_rdata_check, "", "Guwange/Diagnostics",
                      "Diagnostic (PC): the folder of the code generator's image dumps (out). Every 10 s the "
                      "read-only data of default.xex and of the loaded game core is compared with its dump (import "
                      "records aside). Empty = no comparison")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace guwange::codegen_checks {
void Start();
}
namespace guwange::native {
std::string VerifySummary(uint64_t& differences);  // src/cpu/guwange_native.cpp
}

#if !REX_PLATFORM_SWITCH

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xmemory.h>

extern "C" {
volatile unsigned long long g_rex_flush_flow_checked = 0;  // plain lossy counts: "at least this many"
volatile unsigned long long g_rex_ro_checked = 0;
volatile unsigned long long g_rex_promoted_calls = 0;
volatile unsigned long long g_rex_r1_checked = 0;
volatile unsigned long long g_rex_lr_write_checked = 0;
unsigned char g_rex_indirect_census = 0;
}

namespace {

constexpr int kKinds = 5;
const char* const kKindNames[kKinds] = {"flush-mode flow", "read-only constants", "promoted indirect calls",
                                        "r1 mirror", "link-register write-backs"};
std::atomic<uint64_t> g_differences[kKinds];

void Mismatch(const char* module, int kind, unsigned long long a, unsigned long long b) {
  if (kind < 0 || kind >= kKinds) return;
  if (g_differences[kind].fetch_add(1, std::memory_order_relaxed) >= 16) return;
  switch (kind) {
    case 0:
      REXLOG_ERROR("[verify] DIFFERENCE: flush-mode flow ({}): a check proven redundant had to switch the mode {}",
                   module, a ? "on" : "off");
      break;
    case 1:
      REXLOG_ERROR("[verify] DIFFERENCE: read-only constants ({}): guest memory at {:08X} does not hold the constant "
                   "the generated code uses", module, a);
      break;
    case 2:
      REXLOG_ERROR("[verify] DIFFERENCE: promoted indirect call ({}): the dispatch table's entry for {:08X} is not the "
                   "function the generated code calls", module, a);
      break;
    case 3:
      REXLOG_ERROR("[verify] DIFFERENCE: r1 mirror ({}): after a call the mirror held {:016X}, ctx.r1 {:016X} (a "
                   "callee assumed to return r1 unchanged did not; the mirror is reloaded)", module, a, b);
      break;
    case 4:
      REXLOG_ERROR("[verify] DIFFERENCE: link-register write-back ({}): ctx.lr held {:016X}, the epilogue writes "
                   "{:016X} (a site proven to write the value ctx.lr holds)", module, a, b);
      break;
  }
}

// One call site of the census: module, function address, ordinal in the function. Up to four targets are told apart;
// the rest is "other".
struct Site {
  uint64_t total = 0;
  uint32_t target[4] = {};
  uint64_t count[4] = {};
  uint64_t other = 0;
};
struct Key {
  const char* module;  // a string literal of the reporting module: compared by content
  uint32_t function;
  uint32_t site;
  bool operator==(const Key& o) const {
    return function == o.function && site == o.site && std::strcmp(module, o.module) == 0;
  }
};
struct KeyHash {
  size_t operator()(const Key& k) const {
    uint64_t h = (uint64_t(k.function) << 32 | k.site) * 0x9E3779B97F4A7C15ull;
    for (const char* c = k.module; *c; ++c) h = (h ^ uint8_t(*c)) * 0x100000001B3ull;
    return size_t(h);
  }
};
constexpr size_t kStripes = 64;
struct Stripe {
  std::mutex mutex;
  std::unordered_map<Key, Site, KeyHash> sites;
};
Stripe g_stripes[kStripes];

void Census(const char* module, unsigned function, unsigned site, unsigned target) {
  const Key key{module, function, site};
  Stripe& stripe = g_stripes[KeyHash()(key) >> 58];
  std::lock_guard<std::mutex> lock(stripe.mutex);
  Site& s = stripe.sites[key];
  ++s.total;
  for (int i = 0; i < 4; ++i) {
    if (s.target[i] == target || s.count[i] == 0) {
      s.target[i] = target;
      ++s.count[i];
      return;
    }
  }
  ++s.other;
}

void WriteCensus(int seconds) {
  struct Row {
    Key key;
    Site site;
  };
  std::vector<Row> rows;
  for (Stripe& stripe : g_stripes) {
    std::lock_guard<std::mutex> lock(stripe.mutex);
    for (const auto& [key, site] : stripe.sites) rows.push_back({key, site});
  }
  std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
    if (a.site.total != b.site.total) return a.site.total > b.site.total;
    if (int c = std::strcmp(a.key.module, b.key.module)) return c < 0;
    return a.key.function != b.key.function ? a.key.function < b.key.function : a.key.site < b.key.site;
  });
  uint64_t calls = 0, single = 0, single_calls = 0, top_calls = 0;
  for (const Row& r : rows) {
    calls += r.site.total;
    const uint64_t top = *std::max_element(r.site.count, r.site.count + 4);
    top_calls += top;
    if (top == r.site.total) {
      ++single;
      single_calls += r.site.total;
    }
  }
  static uint64_t last_calls = 0;
  REXLOG_INFO("[census] indirect calls: {:.0f} per second; {} sites seen so far with {} calls, {} sites ({} calls) "
              "always went to one target, {:.1f} % of all calls went to their site's most frequent target",
              double(calls - last_calls) / double(std::max(seconds, 1)), rows.size(), calls, single, single_calls,
              calls ? 100.0 * double(top_calls) / double(calls) : 0.0);
  last_calls = calls;
  const std::string path = REXCVAR_GET(guwange_diag_indirect_census);
  const std::string temp = path + ".tmp";
  if (FILE* f = std::fopen(temp.c_str(), "w")) {
    std::fprintf(f, "# guwange-nx indirect call census (cumulative): <module> <function> <site> <calls> "
                    "<target>:<calls>... other:<calls>\n");
    for (const Row& r : rows) {
      std::fprintf(f, "%s %08X %u %llu", r.key.module, r.key.function, r.key.site,
                   static_cast<unsigned long long>(r.site.total));
      for (int i = 0; i < 4; ++i) {
        if (r.site.count[i]) {
          std::fprintf(f, " %08X:%llu", r.site.target[i], static_cast<unsigned long long>(r.site.count[i]));
        }
      }
      std::fprintf(f, " other:%llu\n", static_cast<unsigned long long>(r.site.other));
    }
    std::fclose(f);
    std::remove(path.c_str());
    std::rename(temp.c_str(), path.c_str());
  } else {
    REXLOG_WARN("[census] indirect calls: cannot write {}", temp);
  }
}

// guwange_diag_rdata_check: the .rdata section of one loaded module against the generator's dump of its image.
uint32_t Le32(const std::vector<uint8_t>& d, size_t at) {
  return at + 4 <= d.size() ? uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 |
                                  uint32_t(d[at + 3]) << 24
                            : 0;
}

// Returns the bytes compared; `different` gets the differing ones.
uint64_t CheckReadOnlyData(rex::system::UserModule* module, const std::string& folder, uint64_t& different) {
  if (!module || !module->xex_module()) return 0;
  std::string stem = module->name();
  if (const size_t dot = stem.find_last_of('.'); dot != std::string::npos) stem.resize(dot);
  const std::string path = folder + "/" + stem + "_image.bin";
  std::vector<uint8_t> dump;
  if (FILE* f = std::fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    dump.resize(size > 0 ? size_t(size) : 0);
    if (!dump.empty() && std::fread(dump.data(), 1, dump.size(), f) != dump.size()) dump.clear();
    std::fclose(f);
  }
  if (dump.size() < 0x400) {
    static std::atomic<uint32_t> warned{0};
    if (warned.fetch_add(1) < 4) REXLOG_WARN("[verify] read-only data: no image dump {}", path);
    return 0;
  }
  // The words the loader rewrites: the module's import records.
  std::vector<uint32_t> records;
  for (const auto& library : *module->xex_module()->import_libraries()) {
    for (const auto& import : library.imports) {
      if (import.value_address) records.push_back(import.value_address & ~3u);
      if (import.thunk_address) records.push_back(import.thunk_address & ~3u);
    }
  }
  std::sort(records.begin(), records.end());
  const size_t pe = Le32(dump, 0x3C);
  const uint32_t sections = Le32(dump, pe + 6) & 0xFFFF, optional = Le32(dump, pe + 20) & 0xFFFF;
  const uint32_t base = Le32(dump, pe + 24 + 28);
  const size_t table = pe + 24 + optional;
  auto* memory = rex::system::kernel_state()->memory();
  uint64_t compared = 0, here = 0;
  uint32_t first = 0;
  for (uint32_t i = 0; i < sections; ++i) {
    const size_t at = table + 40 * i;
    if (at + 40 > dump.size() || std::memcmp(&dump[at], ".rdata\0\0", 8) != 0) continue;
    const uint32_t size = Le32(dump, at + 8), va = Le32(dump, at + 12);
    if (uint64_t(va) + size > dump.size()) continue;
    const uint8_t* live = memory->TranslateVirtual<const uint8_t*>(base + va);
    for (uint32_t o = 0; o < size; ++o) {
      if (live[o] == dump[va + o]) continue;
      const uint32_t address = base + va + o;
      if (std::binary_search(records.begin(), records.end(), address & ~3u)) continue;
      if (!here++) first = address;
    }
    compared += size;
  }
  if (here) {
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1) < 8) {
      REXLOG_ERROR("[verify] DIFFERENCE: read-only data of {}: {} of {} bytes differ from the image dump {} (first "
                   "at {:08X}): a folded constant there would be wrong on the console", module->name(), here,
                   compared, path, first);
    }
  }
  different += here;
  return compared;
}

// The game cores: module file name (the kernel's module name) and the PC DLL that holds its code.
struct Core {
  const char* module;
  const wchar_t* dll;
};
constexpr Core kCores[] = {
    {"Guwange01_x360.dll", L"guwange_Guwange01_x360.dll"},
    {"Guwange01_coop_x360.dll", L"guwange_Guwange01_coop_x360.dll"},
    {"Guwange_arrange_x360.dll", L"guwange_Guwange_arrange_x360.dll"},
    {"Guwange_blue_x360.dll", L"guwange_Guwange_blue_x360.dll"},
};

// The checks done so far (cumulative, every module): the executable's counters plus each loaded core's. A core that
// was unloaded takes its counts with it; the report only adds what grew.
void Counts(uint64_t out[kKinds]) {
  out[0] = g_rex_flush_flow_checked;
  out[1] = g_rex_ro_checked;
  out[2] = g_rex_promoted_calls;
  out[3] = g_rex_r1_checked;
  out[4] = g_rex_lr_write_checked;
#if defined(_WIN32)
  for (const Core& core : kCores) {
    HMODULE dll = nullptr;
    if (!GetModuleHandleExW(0, core.dll, &dll) || !dll) continue;  // holds the DLL while its counts are read
    if (auto counts = reinterpret_cast<void (*)(unsigned long long*)>(
            reinterpret_cast<void*>(GetProcAddress(dll, "GuwangeCoreCheckCounts")))) {
      unsigned long long c[kKinds] = {};
      counts(c);
      for (int k = 0; k < kKinds; ++k) out[k] += c[k];
    }
    FreeLibrary(dll);
  }
#endif
}

void Report(int seconds) {
  static uint64_t last[kKinds] = {};
  static uint64_t last_differences = 0, last_pcr = 0, last_pcr_wrong = 0;
  uint64_t now[kKinds];
  Counts(now);
  std::string checks;
  uint64_t differences = 0;
  for (int k = 0; k < kKinds; ++k) {
    differences += g_differences[k].load(std::memory_order_relaxed);
    if (now[k] > last[k]) {
      checks += fmt::format("{}{} {}", checks.empty() ? "" : ", ", kKindNames[k], now[k] - last[k]);
    }
    last[k] = std::max(last[k], now[k]);
  }
  // The SDK's kernel_critsec_pcr_owner: every critical section call compares the owner id it took from the PCR with
  // the looked-up thread's.
  const uint64_t pcr = rex::syscount::CensusRead(rex::syscount::kCensusCritSecPcrOwner);
  const uint64_t pcr_wrong = rex::syscount::CensusRead(rex::syscount::kCensusCritSecOwnerMismatch);
  if (pcr > last_pcr) {
    checks += fmt::format("{}critical section owners {}", checks.empty() ? "" : ", ", pcr - last_pcr);
  }
  if (pcr_wrong > last_pcr_wrong) {
    REXLOG_ERROR("[verify] DIFFERENCE: critical section owners from the PCR: {} of {} were not the current thread",
                 pcr_wrong, pcr);
  }
  differences += pcr_wrong;
  last_pcr = pcr;
  last_pcr_wrong = pcr_wrong;
  if (const std::string folder = REXCVAR_GET(guwange_diag_rdata_check); !folder.empty()) {
    auto* kernel = rex::system::kernel_state();
    uint64_t different = 0, compared = CheckReadOnlyData(kernel->GetExecutableModule().get(), folder, different);
    for (const Core& core : kCores) {
      if (auto module = kernel->GetModule(core.module, true)) {
        compared += CheckReadOnlyData(dynamic_cast<rex::system::UserModule*>(module.get()), folder, different);
      }
    }
    if (compared) checks += fmt::format("{}read-only data bytes {}", checks.empty() ? "" : ", ", compared);
    differences += different;
  }
  if (!checks.empty() || differences != last_differences) {
    REXLOG_INFO("[verify] codegen checks ({} s): {} | differences: {}", seconds, checks.empty() ? "none" : checks,
                differences);
  }
  last_differences = differences;
  {
    // The natives' verify comparisons (src/cpu/guwange_native.h; only with guwange_cpu_verify).
    static uint64_t last_native_differences = 0;
    uint64_t native_differences = 0;
    const std::string natives = guwange::native::VerifySummary(native_differences);
    if (!natives.empty() || native_differences != last_native_differences) {
      REXLOG_INFO("[verify] natives (checks/differences, {} s): {} | differences: {}", seconds,
                  natives.empty() ? "none" : natives, native_differences);
    }
    last_native_differences = native_differences;
  }
  {
    // tools/fp_sign_ops.py needs round-up / round-down never to be selected (cumulative, every module).
    using namespace rex::syscount;
    const uint64_t up_down = CensusRead(kCensusRoundUpDown);
    static uint64_t logged_up_down = 0;
    static int since = 0;
    since += seconds;
    if (since >= 60 || up_down != logged_up_down) {
      since = 0;
      if (up_down) {
        REXLOG_ERROR("[census] FPSCR rounding mode written by the guest (cumulative): to nearest {}, toward zero {}, "
                     "up or down {} (DIFFERENCE: tools/fp_sign_ops.py assumes never)",
                     CensusRead(kCensusRoundNearest), CensusRead(kCensusRoundZero), up_down);
      } else {
        REXLOG_INFO("[census] FPSCR rounding mode written by the guest (cumulative): to nearest {}, toward zero {}, "
                    "up or down 0", CensusRead(kCensusRoundNearest), CensusRead(kCensusRoundZero));
      }
      logged_up_down = up_down;
    }
  }
  if (g_rex_indirect_census) WriteCensus(seconds);
}

}  // namespace

extern "C" {
void RexFlushFlowMismatch(int on) noexcept { Mismatch("default", 0, unsigned(on), 0); }
void RexRoMismatch(unsigned address) noexcept { Mismatch("default", 1, address, 0); }
void RexPromotedMismatch(unsigned target) noexcept { Mismatch("default", 2, target, 0); }
void RexR1Mismatch(unsigned long long expected, unsigned long long got) noexcept {
  Mismatch("default", 3, expected, got);
}
void RexLrWriteMismatch(unsigned long long held, unsigned long long written) noexcept {
  Mismatch("default", 4, held, written);
}
void RexIndirectCensus(unsigned function, unsigned site, unsigned target) noexcept {
  Census("default", function, site, target);
}
}

// For the game cores (the GUWANGE_CORE_NAME half above).
GUWANGE_CHECKS_EXPORT void GuwangeCodegenMismatch(const char* module, int kind, unsigned long long a,
                                                  unsigned long long b) {
  Mismatch(module, kind, a, b);
}
GUWANGE_CHECKS_EXPORT bool GuwangeIndirectCensusOn() { return g_rex_indirect_census != 0; }
GUWANGE_CHECKS_EXPORT void GuwangeCoreIndirectCensus(const char* module, unsigned function, unsigned site,
                                                     unsigned target) {
  Census(module, function, site, target);
}

namespace guwange::codegen_checks {

// After the config is read (src/guwange_app.h): the census switch, and the report every 10 s for the whole session
// (a detached thread; the process ends with std::_Exit / the libnx exit, never by joining it).
void Start() {
  g_rex_indirect_census = REXCVAR_GET(guwange_diag_indirect_census).empty() ? 0 : 1;
  if (g_rex_indirect_census) {
    REXLOG_INFO("[census] indirect calls: every tagged call site reports its target; the table goes to {} every 10 s",
                REXCVAR_GET(guwange_diag_indirect_census));
  }
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(10));
      Report(10);
    }
  }).detach();
}

}  // namespace guwange::codegen_checks

#else  // the Switch build: nothing to check, nothing to count

namespace guwange::codegen_checks {
void Start() {}
}  // namespace guwange::codegen_checks

#endif  // !REX_PLATFORM_SWITCH
#endif  // GUWANGE_CORE_NAME
