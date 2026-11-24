#pragma once
#include <cstddef>
#include <cstdint>

namespace dsr {

// Installs SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT/SIGTRAP handlers that write a crash report (signal,
// fault address, registers, and addresses translated to "main+0x..." offsets) to the log file and
// logcat, then hand the signal to the previous handler (Android's debuggerd).
void InstallCrashHandler();

// Register a guest module so crash addresses inside it are printed as module offsets
// (matching the addresses in Ghidra/IDA when the ELF from tools/mkelf.py is loaded at base 0).
void RegisterCrashModule(const char* name, uintptr_t base, size_t size);

// A file the crash handler writes if the process crashes before `until_ns` (CLOCK_MONOTONIC): `text` into `path`
// (both copied; text up to 4 KiB). The cheats use it to know, at the next start, which cheats were on when the game
// crashed soon after they started. ClearCrashNote: nothing is written.
void SetCrashNote(const char* path, const char* text, size_t len, int64_t until_ns);
void ClearCrashNote();
int64_t MonotonicNs();

}  // namespace dsr
