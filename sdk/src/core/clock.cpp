/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2019 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <atomic>
#include <deque>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

#include <rex/chrono/clock.h>
#include <rex/cvar.h>
#include <rex/math.h>
#include <rex/sys_counters.h>

#if !defined(__SWITCH__)
// The runtime census (sys_counters.h): off the Switch, what REX_SYS_COUNT / REX_SYS_CENSUS count into.
namespace rex::syscount {
static std::atomic<uint64_t> g_census[kCensusCount]{};

namespace {
struct ExportEntry {
  const char* name;
  std::atomic<uint64_t> count{0};
};
std::mutex g_export_mutex;
std::deque<ExportEntry>& ExportEntries() {
  static std::deque<ExportEntry> entries;  // a deque: the counters never move
  return entries;
}
}  // namespace

std::atomic<uint64_t>* ExportCounter(const char* name) {
  std::lock_guard<std::mutex> lock(g_export_mutex);
  ExportEntry& entry = ExportEntries().emplace_back();
  entry.name = name;
  return &entry.count;
}

size_t ExportCensus(const char** names, uint64_t* counts, size_t capacity) {
  std::vector<std::pair<uint64_t, const char*>> all;
  {
    std::lock_guard<std::mutex> lock(g_export_mutex);
    for (const ExportEntry& entry : ExportEntries()) {
      const uint64_t n = entry.count.load(std::memory_order_relaxed);
      if (n) all.emplace_back(n, entry.name);
    }
  }
  std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
  const size_t n = std::min(capacity, all.size());
  for (size_t i = 0; i < n; ++i) {
    names[i] = all[i].second;
    counts[i] = all[i].first;
  }
  return n;
}
void Census(unsigned id, uint64_t n) {
  if (id < kCensusCount)
    g_census[id].store(g_census[id].load(std::memory_order_relaxed) + n, std::memory_order_relaxed);
}
uint64_t CensusRead(unsigned id) {
  return id < kCensusCount ? g_census[id].load(std::memory_order_relaxed) : 0;
}

namespace {
struct Site {
  std::atomic<uint64_t> address{0}, count{0};
};
constexpr size_t kSites = size_t(1) << 16;
Site g_sites[kSites];
std::atomic<bool> g_sites_on{false};
}  // namespace

void CensusSitesEnable(bool on) {
  g_sites_on.store(on, std::memory_order_relaxed);
}

#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
void CensusSite(unsigned id) {
  Census(id, 1);
  if (!g_sites_on.load(std::memory_order_relaxed)) return;
#if defined(__clang__) || defined(__GNUC__)
  const uint64_t a = reinterpret_cast<uint64_t>(__builtin_return_address(0));
#else
  const uint64_t a = 1;
#endif
  size_t i = size_t((a * 0x9E3779B97F4A7C15ull) >> 48);
  for (size_t probe = 0; probe < 64; ++probe, i = (i + 1) & (kSites - 1)) {
    uint64_t have = g_sites[i].address.load(std::memory_order_relaxed);
    if (have == 0 && g_sites[i].address.compare_exchange_strong(have, a, std::memory_order_relaxed)) have = a;
    if (have == a) {
      g_sites[i].count.store(g_sites[i].count.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
      return;
    }
  }
}

size_t CensusSites(uint64_t* addresses, uint64_t* counts, size_t capacity) {
  size_t n = 0;
  for (size_t i = 0; i < kSites; ++i) {
    const uint64_t c = g_sites[i].count.load(std::memory_order_relaxed);
    if (!c) continue;
    const uint64_t a = g_sites[i].address.load(std::memory_order_relaxed);
    // Insertion into the list sorted by count, largest first.
    size_t at = n < capacity ? n : capacity;
    while (at > 0 && counts[at - 1] < c) --at;
    if (at >= capacity) continue;
    const size_t last = n < capacity ? n : capacity - 1;
    for (size_t k = last; k > at; --k) {
      addresses[k] = addresses[k - 1];
      counts[k] = counts[k - 1];
    }
    addresses[at] = a;
    counts[at] = c;
    if (n < capacity) ++n;
  }
  return n;
}
}  // namespace rex::syscount
#endif

REXCVAR_DEFINE_BOOL(clock_no_scaling, false, "Clock",
                    "Disable clock scaling (inverted: false = scaling enabled)");

REXCVAR_DEFINE_BOOL(clock_source_raw, false, "Clock", "Use raw clock source without scaling");

REXCVAR_DEFINE_BOOL(clock_nonblocking, true, "Clock",
                    "UpdateGuestClock never blocks on the clock mutex: a caller that finds another thread in the "
                    "middle of an update returns that thread's published count (equivalent to waiting for it "
                    "and reading the same value, without the wait). false = the original blocking path");

REXCVAR_DEFINE_BOOL(clock_lockfree, true, "Clock",
                    "UpdateGuestClock without the clock mutex: the guest count is computed from an epoch (the host "
                    "and guest counts when the tick ratio last changed) and published with an atomic maximum, so it "
                    "never runs backwards across threads. With a 1:1 ratio (the default) the counts are the mutex "
                    "path's; with another ratio this path no longer loses each call's rounding. false = the mutex "
                    "path (clock_nonblocking)");

namespace rex::chrono {

// Time scalar applied to all time operations.
double guest_time_scalar_ = 1.0;
// Tick frequency of guest.
uint64_t guest_tick_frequency_ = Clock::host_tick_frequency_platform();
// Base FILETIME of the guest system from app start.
uint64_t guest_system_time_base_ = Clock::QueryHostSystemTime();
// Combined time and frequency ratio between host and guest.
// Split in numerator (first) and denominator (second).
// Computed by RecomputeGuestTickScalar.
std::pair<uint64_t, uint64_t> guest_tick_ratio_ = std::make_pair(1, 1);

// Native guest ticks. Atomic so that a contended reader can take it without the mutex.
std::atomic<uint64_t> last_guest_tick_count_{0};
// Last sampled host tick count.
uint64_t last_host_tick_count_ = Clock::QueryHostTickCount();
// Mutex to ensure last_host_tick_count_ and last_guest_tick_count_ are in sync
std::mutex tick_mutex_;

namespace {
// clock_lockfree (guwange-nx's CPU round, 2026-10-22: the console profile had Guwange's drawing thread polling the
// timebase 930,000 times a second, a mutex try-lock and unlock each, ~11 % of that thread): the guest count as a
// function of the host count since the epoch, the counts when the ratio last changed. An epoch is never changed once
// published (a new one replaces it; the old ones are kept, a few bytes per ratio change).
struct Epoch {
  uint64_t host;
  uint64_t guest;
  uint64_t num;
  uint64_t den;
};
std::atomic<const Epoch*> g_epoch{nullptr};

uint64_t EpochCount(const Epoch& e, uint64_t host) {
  const uint64_t d = host > e.host ? host - e.host : 0;
  if (e.num == e.den) {
    return e.guest + d;
  }
  if (d <= std::numeric_limits<uint64_t>::max() / e.num) {
    return e.guest + d * e.num / e.den;
  }
  // The same floor without 128-bit arithmetic (no __udivti3 with clang on Windows): d = q * den + r, so
  // d * num / den = q * num + r * num / den (r < den; the ratio is a reduced fraction of two clock frequencies).
  const uint64_t q = d / e.den, r = d % e.den;
  return e.guest + q * e.num + r * e.num / e.den;
}

// The first epoch: the start of the clock (last_host_tick_count_'s first value, guest count 0), as the mutex path.
const Epoch* CurrentEpoch() {
  const Epoch* e = g_epoch.load(std::memory_order_acquire);
  if (e) [[likely]] {
    return e;
  }
  std::lock_guard<std::mutex> lock(tick_mutex_);
  e = g_epoch.load(std::memory_order_acquire);
  if (!e) {
    e = new Epoch{last_host_tick_count_, 0, guest_tick_ratio_.first, guest_tick_ratio_.second};
    g_epoch.store(e, std::memory_order_release);
  }
  return e;
}

uint64_t UpdateGuestClockLockFree(uint64_t host_tick_count) {
  const uint64_t count = EpochCount(*CurrentEpoch(), host_tick_count);
  uint64_t last = last_guest_tick_count_.load(std::memory_order_relaxed);
  while (count > last) {
    if (last_guest_tick_count_.compare_exchange_weak(last, count, std::memory_order_release,
                                                     std::memory_order_relaxed)) {
      return count;
    }
  }
  return last;  // another thread published a later count (it read the host clock after this one did)
}
}  // namespace

void RecomputeGuestTickScalar() {
  // Create a rational number with numerator (first) and denominator (second)
  auto frac = std::make_pair(guest_tick_frequency_, Clock::QueryHostTickFrequency());
  // Doing it this way ensures we don't mess up our frequency scaling and
  // precisely controls the precision the guest_time_scalar_ can have.
  if (guest_time_scalar_ > 1.0) {
    frac.first *= static_cast<uint64_t>(guest_time_scalar_ * 10.0);
    frac.second *= 10;
  } else {
    frac.first *= 10;
    frac.second *= static_cast<uint64_t>(10.0 / guest_time_scalar_);
  }
  // Keep this a rational calculation and reduce the fraction
  reduce_fraction(frac);

  // The lock-free path's epoch moves to now, at the count the old ratio reached (before the lock: CurrentEpoch may
  // take it to make the first epoch).
  const Epoch* old_epoch = CurrentEpoch();
  std::lock_guard<std::mutex> lock(tick_mutex_);
  guest_tick_ratio_ = frac;
  const uint64_t host = Clock::QueryHostTickCount();
  const uint64_t guest =
      std::max(EpochCount(*old_epoch, host), last_guest_tick_count_.load(std::memory_order_acquire));
  g_epoch.store(new Epoch{host, guest, frac.first, frac.second}, std::memory_order_release);
}

// Update the guest timer for all threads.
// Return a copy of the value so locking is reduced.
uint64_t UpdateGuestClock() {
  uint64_t host_tick_count = Clock::QueryHostTickCount();

  if (REXCVAR_GET(clock_no_scaling)) {
    // Nothing to update, calculate on the fly
    return host_tick_count * guest_tick_ratio_.first / guest_tick_ratio_.second;
  }

  REX_SYS_COUNT(rex::syscount::kClockCall, 1);
  if (REXCVAR_GET(clock_lockfree)) [[likely]] {
    return UpdateGuestClockLockFree(host_tick_count);
  }
  std::unique_lock<std::mutex> lock(tick_mutex_, std::defer_lock);
  if (lock.try_lock()) {
    // Translate host tick count to guest tick count.
    uint64_t host_tick_delta =
        host_tick_count > last_host_tick_count_ ? host_tick_count - last_host_tick_count_ : 0;
    last_host_tick_count_ = host_tick_count;
    uint64_t guest_tick_delta =
        host_tick_delta * guest_tick_ratio_.first / guest_tick_ratio_.second;
    const uint64_t guest_tick_count =
        last_guest_tick_count_.load(std::memory_order_relaxed) + guest_tick_delta;
    last_guest_tick_count_.store(guest_tick_count, std::memory_order_release);
    return guest_tick_count;
  } else {
    REX_SYS_COUNT(rex::syscount::kClockBlocked, 1);
    if (REXCVAR_GET(clock_nonblocking)) {
      // Another thread is inside the update (a few instructions): its count is the one it is about to
      // publish and that is what waiting for the lock used to return.
      return last_guest_tick_count_.load(std::memory_order_acquire);
    }
    // Wait until another thread has finished updating the clock.
    lock.lock();
    return last_guest_tick_count_.load(std::memory_order_acquire);
  }
}

// Offset of the current guest system file time relative to the guest base time.
inline uint64_t QueryGuestSystemTimeOffset() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime() - guest_system_time_base_;
  }

  auto guest_tick_count = UpdateGuestClock();

  uint64_t numerator = 10000000;  // 100ns/10MHz resolution
  uint64_t denominator = guest_tick_frequency_;
  reduce_fraction(numerator, denominator);

  return guest_tick_count * numerator / denominator;
}

uint64_t Clock::QueryHostTickFrequency() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_frequency_raw();
  }
#endif
  return host_tick_frequency_platform();
}
uint64_t Clock::QueryHostTickCount() {
#if REX_CLOCK_RAW_AVAILABLE
  if (REXCVAR_GET(clock_source_raw)) {
    return host_tick_count_raw();
  }
#endif
  return host_tick_count_platform();
}

double Clock::guest_time_scalar() {
  return guest_time_scalar_;
}

void Clock::set_guest_time_scalar(double scalar) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  guest_time_scalar_ = scalar;
  RecomputeGuestTickScalar();
}

std::pair<uint64_t, uint64_t> Clock::guest_tick_ratio() {
  std::lock_guard<std::mutex> lock(tick_mutex_);
  return guest_tick_ratio_;
}

uint64_t Clock::guest_tick_frequency() {
  return guest_tick_frequency_;
}

void Clock::set_guest_tick_frequency(uint64_t frequency) {
  guest_tick_frequency_ = frequency;
  RecomputeGuestTickScalar();
}

uint64_t Clock::guest_system_time_base() {
  return guest_system_time_base_;
}

void Clock::set_guest_system_time_base(uint64_t time_base) {
  guest_system_time_base_ = time_base;
}

uint64_t Clock::QueryGuestTickCount() {
  auto guest_tick_count = UpdateGuestClock();
  return guest_tick_count;
}

uint64_t Clock::QueryGuestSystemTime() {
  if (REXCVAR_GET(clock_no_scaling)) {
    return Clock::QueryHostSystemTime();
  }

  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  return guest_system_time_base_ + guest_system_time_offset;
}

uint32_t Clock::QueryGuestUptimeMillis() {
  return static_cast<uint32_t>(std::min<uint64_t>(QueryGuestSystemTimeOffset() / 10000,
                                                  std::numeric_limits<uint32_t>::max()));
}

void Clock::SetGuestSystemTime(uint64_t system_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    // Time is fixed to host time.
    return;
  }

  // Query the filetime offset to calculate a new base time.
  auto guest_system_time_offset = QueryGuestSystemTimeOffset();
  guest_system_time_base_ = system_time - guest_system_time_offset;
}

uint32_t Clock::ScaleGuestDurationMillis(uint32_t guest_ms) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return guest_ms;
  }

  constexpr uint64_t max = std::numeric_limits<uint32_t>::max();

  if (guest_ms >= max) {
    return max;
  } else if (!guest_ms) {
    return 0;
  }
  uint64_t scaled_ms =
      static_cast<uint64_t>((static_cast<uint64_t>(guest_ms) * guest_time_scalar_));
  return static_cast<uint32_t>(std::min(scaled_ms, max));
}

int64_t Clock::ScaleGuestDurationFileTime(int64_t guest_file_time) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return static_cast<uint64_t>(guest_file_time);
  }

  if (!guest_file_time) {
    return 0;
  } else if (guest_file_time > 0) {
    // Absolute time.
    uint64_t guest_time = Clock::QueryGuestSystemTime();
    int64_t relative_time = guest_file_time - static_cast<int64_t>(guest_time);
    int64_t scaled_time = static_cast<int64_t>(relative_time * guest_time_scalar_);
    return static_cast<int64_t>(guest_time) + scaled_time;
  } else {
    // Relative time.
    uint64_t scaled_file_time =
        static_cast<uint64_t>((static_cast<uint64_t>(guest_file_time) * guest_time_scalar_));
    // TODO(benvanik): check for overflow?
    return scaled_file_time;
  }
}

void Clock::ScaleGuestDurationTimeval(int32_t* tv_sec, int32_t* tv_usec) {
  if (REXCVAR_GET(clock_no_scaling)) {
    return;
  }

  uint64_t scaled_sec = static_cast<uint64_t>(static_cast<uint64_t>(*tv_sec) * guest_time_scalar_);
  uint64_t scaled_usec =
      static_cast<uint64_t>(static_cast<uint64_t>(*tv_usec) * guest_time_scalar_);
  if (scaled_usec > std::numeric_limits<uint32_t>::max()) {
    uint64_t overflow_sec = scaled_usec / 1000000;
    scaled_usec -= overflow_sec * 1000000;
    scaled_sec += overflow_sec;
  }
  *tv_sec = int32_t(scaled_sec);
  *tv_usec = int32_t(scaled_usec);
}

}  // namespace rex::chrono
