#pragma once

// PC sampling profiler (cvar guwange_pc_profile_s); see guwange_pc_profiler.cpp. Does nothing on the Switch, where the
// SDK's sampler (sdk/src/ui/switch_perf.cpp) does the same job.
namespace guwange::pc_profiler {

void Start();
void Stop();

}  // namespace guwange::pc_profiler
