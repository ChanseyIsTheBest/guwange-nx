// Guwange: a diagnostic watch on guest memory words (development aid).
//
// guwange_diag_watch = "0x887B444C,0x887B4444+1630b" logs, every guwange_diag_watch_ms, the big-endian word at each
// address (a "+<offset>" reads through the pointer at the address first; a trailing "b" reads one byte). Nothing runs
// when the key is empty.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/frame_latency.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

REXCVAR_DEFINE_STRING(guwange_diag_watch, "", "Guwange/Diagnostics",
                      "Guest words to log periodically: comma-separated 0xADDR[+OFFSET][b] (+OFFSET reads through the "
                      "pointer at ADDR; b = one byte)");
REXCVAR_DEFINE_INT32(guwange_diag_watch_ms, 500, "Guwange/Diagnostics", "Period of guwange_diag_watch (ms)");
REXCVAR_DEFINE_INT32(guwange_diag_latency_trace_s, 0, "Guwange/Diagnostics",
                     "Diagnostic: this many seconds after the start, log the order and times of the next 12 frames' "
                     "events (controller reads, Swaps, presents, vblanks, the arcade frame's video); 0 = off");
REXCVAR_DEFINE_STRING(guwange_diag_around, "", "Guwange/Diagnostics",
                      "0xADDR:BEFORE:LENGTH: also log LENGTH bytes of guest memory from (the pointer at ADDR) - BEFORE");

namespace guwange::diag {
namespace {

struct Watch {
  uint32_t address = 0;
  bool through = false;
  uint32_t offset = 0;
  bool byte = false;
  std::string text;
};

std::thread g_thread;
std::thread g_trace_thread;
std::atomic<bool> g_stop{false};

uint32_t Load32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

// Whether the guest page at the address is committed (a game core's data is mapped only once the core is loaded).
template <typename M>
bool Committed(M* memory, uint32_t address) {
  auto* heap = memory->LookupHeap(address);
  rex::memory::HeapAllocationInfo info{};
  return heap && heap->QueryRegionInfo(address, &info) && (info.state & rex::memory::kMemoryAllocationCommit);
}

}  // namespace

void Start(rex::Runtime* runtime) {
  if (const int32_t at = REXCVAR_GET(guwange_diag_latency_trace_s); at > 0 && !g_trace_thread.joinable()) {
    g_trace_thread = std::thread([at] {
      for (int i = 0; i < at * 20 && !g_stop.load(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
      rex::latency::StartTrace(12);
      for (int i = 0; i < 400 && !g_stop.load(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        const std::string trace = rex::latency::TakeTrace();
        if (trace.empty()) continue;
        size_t from = 0;
        while (from < trace.size()) {
          const size_t end = trace.find('\n', from);
          REXLOG_INFO("[trace] {}", trace.substr(from, end == std::string::npos ? std::string::npos : end - from));
          if (end == std::string::npos) break;
          from = end + 1;
        }
        break;
      }
    });
  }
  const std::string spec = REXCVAR_GET(guwange_diag_watch);
  if (spec.empty() || !runtime || !runtime->memory() || g_thread.joinable()) {
    return;
  }
  std::vector<Watch> watches;
  size_t at = 0;
  while (at < spec.size()) {
    size_t end = spec.find(',', at);
    std::string item = spec.substr(at, end == std::string::npos ? std::string::npos : end - at);
    at = end == std::string::npos ? spec.size() : end + 1;
    if (item.empty()) continue;
    Watch w;
    w.text = item;
    if (item.back() == 'b') {
      w.byte = true;
      item.pop_back();
    }
    const size_t plus = item.find('+');
    w.address = uint32_t(std::stoul(item.substr(0, plus), nullptr, 0));
    if (plus != std::string::npos) {
      w.through = true;
      w.offset = uint32_t(std::stoul(item.substr(plus + 1), nullptr, 0));
    }
    watches.push_back(w);
  }
  uint32_t around = 0, before = 0, length = 0;
  if (const std::string a = REXCVAR_GET(guwange_diag_around); !a.empty()) {
    const size_t c1 = a.find(':'), c2 = a.find(':', c1 + 1);
    around = uint32_t(std::stoul(a.substr(0, c1), nullptr, 0));
    before = uint32_t(std::stoul(a.substr(c1 + 1, c2 - c1 - 1), nullptr, 0));
    length = uint32_t(std::stoul(a.substr(c2 + 1), nullptr, 0));
  }
  auto* memory = runtime->memory();
  const int period = std::max(10, int(REXCVAR_GET(guwange_diag_watch_ms)));
  g_thread = std::thread([watches, memory, period, around, before, length] {
    while (!g_stop.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(period));
      std::string line;
      for (const auto& w : watches) {
        uint32_t address = w.address;
        if (!Committed(memory, w.address)) {
          line += fmt::format(" {}=--------", w.text);
          continue;
        }
        if (w.through) {
          address = Load32(memory->TranslateVirtual<const uint8_t*>(w.address)) + w.offset;
        }
        uint32_t value = 0;
        if (address && Committed(memory, address)) {
          const auto* p = memory->TranslateVirtual<const uint8_t*>(address);
          value = w.byte ? uint32_t(p[0]) : Load32(p);
        }
        line += fmt::format(" {}={:08X}", w.text, value);
      }
      REXLOG_INFO("[diag]{}", line);
      if (around && Committed(memory, around)) {
        const uint32_t p = Load32(memory->TranslateVirtual<const uint8_t*>(around));
        std::string dump;
        for (uint32_t a = (p - before) & ~3u; a < p - before + length; a += 4) {
          if (!Committed(memory, a)) break;
          dump += fmt::format("{}{:08X}", (a == (p & ~3u)) ? " >" : " ", Load32(memory->TranslateVirtual<const uint8_t*>(a)));
        }
        REXLOG_INFO("[diag] around {:08X} (from {:08X}):{}", p, (p - before) & ~3u, dump);
      }
    }
  });
}

void Stop() {
  g_stop.store(true);
  if (g_thread.joinable()) {
    g_thread.join();
  }
  if (g_trace_thread.joinable()) {
    g_trace_thread.join();
  }
}

}  // namespace guwange::diag
