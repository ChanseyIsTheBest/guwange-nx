// Windows: fatal host exceptions written down with the module, offset and the faulting thread's stack, so the crash can
// be symbolized afterwards: llvm-symbolizer --obj=guwange.exe --relative-address 0x<offset> (futari-nx's
// futari_crash_log.cpp, guwange-nx 2026-10-22):
//   - illegal instruction, divide by zero, stack overflow ...: logged when raised (the SDK does not report them);
//   - any exception nothing handled (an access violation the SDK's own handlers declined, during teardown say): the
//     unhandled-exception filter, which writes logs/crash.txt next to the executable with plain stdio (the logger may
//     already be shut down) and the stack unwound frame by frame.

#if defined(_WIN32)

#include <windows.h>

#include <cstdio>
#include <cstring>

#include <rex/logging.h>
#include <rex/logging/api.h>

namespace {

void ModuleOf(uintptr_t pc, char (&path)[MAX_PATH], uintptr_t& base) {
  HMODULE module = nullptr;
  std::strcpy(path, "?");
  base = 0;
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCSTR>(pc), &module) && module) {
    GetModuleFileNameA(module, path, MAX_PATH);
    base = reinterpret_cast<uintptr_t>(module);
  }
}

const char* BaseName(const char* path) {
  const char* name = std::strrchr(path, '\\');
  return name ? name + 1 : path;
}

LONG CALLBACK LogFatalException(EXCEPTION_POINTERS* info) {
  const DWORD code = info->ExceptionRecord->ExceptionCode;
  switch (code) {
    case EXCEPTION_ILLEGAL_INSTRUCTION:
    case EXCEPTION_PRIV_INSTRUCTION:
    case EXCEPTION_INT_DIVIDE_BY_ZERO:
    case EXCEPTION_STACK_OVERFLOW:
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
    case STATUS_STACK_BUFFER_OVERRUN:
      break;
    default:
      return EXCEPTION_CONTINUE_SEARCH;
  }
  const auto pc = reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress);
  char path[MAX_PATH];
  uintptr_t base;
  ModuleOf(pc, path, base);
  REXLOG_CRITICAL("[crash] host exception {:08X} at {:016X} ({}+0x{:X}) on thread {}", uint32_t(code), pc,
                  BaseName(path), pc - base, GetCurrentThreadId());
  rex::FlushLogging();
  return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI WriteUnhandledException(EXCEPTION_POINTERS* info) {
  char exe[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, exe, MAX_PATH);
  if (char* slash = std::strrchr(exe, '\\'))
    std::strcpy(slash + 1, "logs\\crash.txt");
  std::FILE* f = std::fopen(exe, "a");
  if (!f)
    return EXCEPTION_CONTINUE_SEARCH;
  const EXCEPTION_RECORD& r = *info->ExceptionRecord;
  std::fprintf(f, "unhandled exception %08lX at %p on thread %lu", r.ExceptionCode, r.ExceptionAddress,
               GetCurrentThreadId());
  if (r.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r.NumberParameters >= 2)
    std::fprintf(f, " (%s of %p)", r.ExceptionInformation[0] ? "write" : "read",
                 reinterpret_cast<void*>(r.ExceptionInformation[1]));
  std::fprintf(f, "\n");
  CONTEXT context = *info->ContextRecord;
  for (int frame = 0; frame < 32 && context.Rip; ++frame) {
    char path[MAX_PATH];
    uintptr_t base;
    ModuleOf(uintptr_t(context.Rip), path, base);
    std::fprintf(f, "  #%02d %s+0x%llX\n", frame, BaseName(path), (unsigned long long)(context.Rip - base));
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &image_base, nullptr);
    if (!entry) {  // a leaf function: the return address is at the stack pointer
      context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
      context.Rsp += 8;
      continue;
    }
    PVOID handler_data = nullptr;
    DWORD64 establisher = 0;
    RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, context.Rip, entry, &context, &handler_data, &establisher,
                     nullptr);
  }
  std::fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;
}

// Installed before main; they only write things down and never handle anything.
const PVOID g_handler = AddVectoredExceptionHandler(1, LogFatalException);
const LPTOP_LEVEL_EXCEPTION_FILTER g_previous_filter = SetUnhandledExceptionFilter(WriteUnhandledException);

}  // namespace

#endif
