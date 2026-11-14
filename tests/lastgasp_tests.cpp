/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#define RESHADE_ADDON 1
#include <lastgasp.hpp>

#include <malloc.h>

#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace reporter = renodx::addons::dlss5::lastgasp;
unsigned checks = 0;
LONG previous_calls = 0;
LONG mirror_calls = 0;
LONG dump_calls = 0;
LONG real_overflow = 0;
DWORD observed_code = 0;
EXCEPTION_POINTERS* observed_info = nullptr;
LONG reentry_result = EXCEPTION_EXECUTE_HANDLER;
bool mirror_after_file = false;
volatile unsigned stack_sink = 0;

void Check(bool passed, const char* message) {
  ++checks;
  if (!passed) throw std::runtime_error(message);
}

LONG WINAPI Previous(EXCEPTION_POINTERS* info) {
  InterlockedIncrement(&previous_calls);
  observed_info = info;
  observed_code = info->ExceptionRecord->ExceptionCode;
  return EXCEPTION_EXECUTE_HANDLER;
}

LONG WINAPI ReenterPrevious(EXCEPTION_POINTERS* info) {
  InterlockedIncrement(&previous_calls);
  reentry_result = reporter::CrashFilter(info);
  return EXCEPTION_EXECUTE_HANDLER;
}

BOOL WINAPI ObserveDump(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                       PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                       PMINIDUMP_CALLBACK_INFORMATION) {
  InterlockedIncrement(&dump_calls);
  return FALSE;
}

void ResetFixture() {
  reporter::filter_entered = false;
  reporter::previous_filter = Previous;
  reporter::dumps_on = false;
  reporter::dumped_crash = false;
  reporter::reshade_log_ready = true;
  previous_calls = mirror_calls = dump_calls = 0;
  observed_info = nullptr;
  observed_code = 0;
  mirror_after_file = false;
  LARGE_INTEGER start{};
  Check(SetFilePointerEx(reporter::crash_file, start, nullptr, FILE_BEGIN)
            && SetEndOfFile(reporter::crash_file), "reset isolated crash file");
}

LONGLONG LogSize() {
  LARGE_INTEGER size{};
  Check(GetFileSizeEx(reporter::crash_file, &size) != FALSE, "read isolated crash file size");
  return size.QuadPart;
}

void TestSyntheticOverflow() {
  ResetFixture();
  reporter::dumps_on = true;
  EXCEPTION_RECORD record{.ExceptionCode = EXCEPTION_STACK_OVERFLOW};
  EXCEPTION_POINTERS info{&record, nullptr};
  Check(reporter::CrashFilter(&info) == EXCEPTION_EXECUTE_HANDLER
            && previous_calls == 1 && observed_info == &info && observed_code == EXCEPTION_STACK_OVERFLOW,
        "stack overflow forwards the original pointers and previous return exactly once");
  Check(LogSize() == 0 && mirror_calls == 0 && dump_calls == 0,
        "stack overflow bypasses crash formatting, file, mirror and dump");
}

void TestOrdinaryAndReentry() {
  ResetFixture();
  reporter::addon_version = "lastgasp-test";
  EXCEPTION_RECORD record{.ExceptionCode = EXCEPTION_ACCESS_VIOLATION};
  record.ExceptionAddress = GetModuleHandleW(nullptr);
  record.NumberParameters = 2;
  record.ExceptionInformation[1] = 0x12345678;
  EXCEPTION_POINTERS info{&record, nullptr};
  Check(reporter::CrashFilter(&info) == EXCEPTION_EXECUTE_HANDLER && previous_calls == 1 && observed_info == &info,
        "ordinary exception preserves the previous filter result and pointers");
  Check(LogSize() > 0 && mirror_calls == 1 && mirror_after_file && dump_calls == 0,
        "ordinary crash writes its file before the existing log mirror");
  char line[2048]{};
  LARGE_INTEGER start{};
  DWORD bytes = 0;
  Check(SetFilePointerEx(reporter::crash_file, start, nullptr, FILE_BEGIN)
            && ReadFile(reporter::crash_file, line, sizeof(line) - 1, &bytes, nullptr), "read ordinary crash line");
  Check(std::strstr(line, "[crash] lastgasp-test") != nullptr
            && std::strstr(line, "code=0xC0000005 (access-violation) READ at 0x12345678") != nullptr,
        "ordinary crash retains its fault classification and address");
  Check(reporter::CrashFilter(&info) == EXCEPTION_CONTINUE_SEARCH && previous_calls == 1 && mirror_calls == 1,
        "reentered filter neither records nor chains again");
  ResetFixture();
  record.ExceptionCode = EXCEPTION_STACK_OVERFLOW;
  reporter::previous_filter = nullptr;
  Check(reporter::CrashFilter(&info) == EXCEPTION_CONTINUE_SEARCH && LogSize() == 0,
        "stack overflow without a previous filter continues to host/WER without formatting");
  ResetFixture();
  reporter::previous_filter = ReenterPrevious;
  Check(reporter::CrashFilter(&info) == EXCEPTION_EXECUTE_HANDLER
            && previous_calls == 1 && reentry_result == EXCEPTION_CONTINUE_SEARCH && LogSize() == 0,
        "a previous filter calling back cannot create a filter cycle");
}

__declspec(noinline) void ExhaustStack(unsigned depth) {
  volatile unsigned char page[1024]{};
  page[depth & 1023] = static_cast<unsigned char>(depth);
  if (depth != 0xFFFFFFFFu) {
    ExhaustStack(depth + 1);
  }
  stack_sink = page[depth & 1023];
}

DWORD WINAPI OverflowThread(void*) {
  __try {
    ExhaustStack(0);
    return ERROR_INVALID_FUNCTION;
  } __except (reporter::CrashFilter(GetExceptionInformation())) {
    if (_resetstkoflw() == 0) return ERROR_BAD_STACK;
    InterlockedIncrement(&real_overflow);
    return ERROR_SUCCESS;
  }
}

void TestRealOverflow() {
  ResetFixture();
  reporter::dumps_on = true;
  HANDLE thread = CreateThread(nullptr, 64 * 1024, OverflowThread, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
  Check(thread != nullptr, "create isolated 64KiB stack test thread");
  const DWORD wait = WaitForSingleObject(thread, 10000);
  DWORD exit_code = ERROR_INVALID_FUNCTION;
  const BOOL got_exit = GetExitCodeThread(thread, &exit_code);
  CloseHandle(thread);
  Check(wait == WAIT_OBJECT_0 && got_exit && exit_code == ERROR_SUCCESS && real_overflow == 1,
        "real stack overflow passes through the production thin entry and restores the thread stack");
  Check(previous_calls == 1 && observed_code == EXCEPTION_STACK_OVERFLOW
            && LogSize() == 0 && mirror_calls == 0 && dump_calls == 0,
        "real low-stack handling forwards once without entering ordinary diagnostics");
  std::cout << "evidence=CPU real_stack_overflow=1 reserve_bytes=65536 resetstkoflw=passed no_GPU=1\n";
}
}  // namespace

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {
  InterlockedIncrement(&mirror_calls);
  LARGE_INTEGER size{};
  mirror_after_file = GetFileSizeEx(reporter::crash_file, &size) && size.QuadPart > 0;
}

int main(int argc, char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
  reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
  reporter::crash_file = CreateFileW(L"synthetic-crash.log", GENERIC_READ | GENERIC_WRITE,
      FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  try {
    Check(reporter::crash_file != INVALID_HANDLE_VALUE, "open synthetic crash file in test cwd");
    reporter::write_minidump = ObserveDump;
    TestSyntheticOverflow();
    if (argc == 1 || std::strcmp(argv[1], "--synthetic-stack") != 0) {
      TestOrdinaryAndReentry();
      TestRealOverflow();
    }
    CloseHandle(reporter::crash_file);
    reporter::crash_file = nullptr;
    std::cout << "PASS lastgasp CPU checks=" << checks << " failures=0\n";
    return 0;
  } catch (const std::exception& error) {
    if (reporter::crash_file != INVALID_HANDLE_VALUE) CloseHandle(reporter::crash_file);
    reporter::crash_file = nullptr;
    std::cerr << "FAIL lastgasp: " << error.what() << '\n';
    return 1;
  }
}
