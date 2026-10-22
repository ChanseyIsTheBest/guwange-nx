// Native replacements: the executable's side of the framework (guwange_native.h): the verify totals for the
// "[verify] natives" line (src/guwange_codegen_checks.cpp writes it every 10 s on the PC) and the per-site decision
// log.

#include "cpu/guwange_native.h"

#include <mutex>

REXCVAR_DEFINE_INT32(guwange_native_verify_every, 1, "Guwange",
                     "With guwange_cpu_verify: compare each native replacement with the recompiled function on every "
                     "Nth call (1 = every call)")
    .range(1, 1000000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace guwange::native {
namespace {

struct Totals {
  std::string what;
  uint64_t checks = 0;
  uint64_t differences = 0;
  uint64_t reported_checks = 0;
};

std::mutex g_mutex;
std::vector<Totals> g_totals;

}  // namespace

void ReportVerify(const char* what, uint64_t checks, uint64_t differences) {
  std::lock_guard lock(g_mutex);
  for (Totals& t : g_totals) {
    if (t.what == what) {
      t.checks += checks;
      t.differences += differences;
      return;
    }
  }
  g_totals.push_back({what, checks, differences});
}

void ReportDecided(const char* what, uint32_t address, int mode, uint64_t found_hash, uint64_t expected_hash) {
  if (mode < 0) {
    REXLOG_WARN("[native] {} at {:08X}: guest code differs (hash {:016X}, written for {:016X}): recompiled code runs",
                what, address, found_hash, expected_hash);
  } else {
    REXLOG_INFO("[native] {} at {:08X}: {}", what, address, mode == kVerify ? "native, verified against the original"
                                                                            : "native");
  }
}

std::string VerifySummary(uint64_t& differences) {
  std::lock_guard lock(g_mutex);
  std::string out;
  differences = 0;
  for (Totals& t : g_totals) {
    differences += t.differences;
    if (t.checks == t.reported_checks) continue;
    out += fmt::format("{}{} {}/{}", out.empty() ? "" : ", ", t.what, t.checks - t.reported_checks, t.differences);
    t.reported_checks = t.checks;
  }
  return out;
}

}  // namespace guwange::native
