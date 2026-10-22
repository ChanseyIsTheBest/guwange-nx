// Guwange: fixes the generated code needs from the application.

#include <chrono>
#include <thread>

#include <rex/system/xthread.h>

// tools/spin_forever.py: a guest loop that can never end (Guwange's main spins this way at 0x821E9814 to keep the
// title alive while the game runs on its other threads). On the Xbox 360 it spins a hardware thread for good; here the
// thread sleeps instead, which is the same as far as the game can tell, and keeps a core free.
//
// It wakes every 50 ms to look for the title's termination (2026-10-22): "Return to Game Library" -> Yes ends the title
// (XamLoaderLaunchTitle -> KernelState::TerminateTitle), whose guest threads leave at their next kernel call
// (XThread::CheckTitleTermination); the app quits once the main thread is gone. A sleep that never checked kept the
// main thread, and so the whole game, alive: the console and the PC both hung there.
[[noreturn]] void rex_spin_forever() {
  for (;;) {
    rex::system::XThread::CheckTitleTermination();  // does not return once the title ends
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
}
