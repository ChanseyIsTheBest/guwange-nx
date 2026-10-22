// Guwange: profile collection of the PGO-instrumented Switch build (GUWANGE_PGO=generate), see guwange_pgo.cpp.
// In every other build these do nothing.

#pragma once

namespace guwange::pgo {

// Picks this session's file (pgo/guwange_pgo_<N>.stream next to the executable) and starts the 2-minute dumps.
void Start();
// Writes every count so far to this session's file (the whole session each time, replacing the previous dump).
void DumpNow();
// The last dump, then the dump thread ends.
void Stop();

}  // namespace guwange::pgo
