/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)
#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace native_fg_frame_cases {
namespace options = renodx::addons::dlss5::fg::native_options;
namespace source = options::frame_source;

struct FakeFrameToken : sl::FrameToken {
  uint32_t value;
  explicit FakeFrameToken(uint32_t frame) : value(frame) {}
  operator uint32_t() const override { return value; }
};

static std::atomic_uint32_t mock_calls{0};
static std::atomic<const sl::FrameToken*> mock_token{nullptr};
static sl::PCLMarker mock_marker = sl::PCLMarker::eSimulationStart;
static sl::Result mock_result = sl::Result::eOk;
static HANDLE mock_entered = nullptr;
static HANDLE mock_release = nullptr;
static uint32_t blocked_frame = 0;
static thread_local bool reentering = false;

inline sl::Result MockMarker(sl::PCLMarker marker, const sl::FrameToken& frame) {
  ++mock_calls;
  mock_token.store(&frame);
  // Arguments are inspected only in the single-thread forwarding case.
  if (static_cast<uint32_t>(frame) == 0) mock_marker = marker;
  if (static_cast<uint32_t>(frame) == 100 && !reentering) {
    reentering = true;
    const FakeFrameToken nested(101);
    source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, nested);
    reentering = false;
  }
  if (mock_entered != nullptr && static_cast<uint32_t>(frame) == blocked_frame) {
    SetEvent(mock_entered);
    if (WaitForSingleObject(mock_release, 2000) != WAIT_OBJECT_0) return sl::Result::eErrorInvalidState;
  }
  return mock_result;
}

struct Events {
  HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  ~Events() {
    if (release != nullptr) {
      SetEvent(release);
      CloseHandle(release);
    }
    if (entered != nullptr) CloseHandle(entered);
  }
};

inline int Run() {
  struct Restore {
    source::Metadata metadata;
    source::Binding binding = source::sdk_binding;
    uint32_t depth = source::sdk_depth;
    Restore() {
      std::unique_lock lock(source::state.mutex);
      metadata = source::state.metadata;
      source::state.metadata = {};
      source::sdk_binding = {};
      source::sdk_depth = 0;
    }
    ~Restore() {
      std::unique_lock lock(source::state.mutex);
      source::state.metadata = metadata;
      source::sdk_binding = binding;
      source::sdk_depth = depth;
      mock_entered = mock_release = nullptr;
      mock_token.store(nullptr);
    }
  } restore;
  uint32_t checks = 0;
  const auto check = [&](bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
  };
  constexpr uintptr_t output = 0x1100;
  constexpr uintptr_t other_output = 0x2200;
  mock_calls.store(0);
  mock_result = sl::Result::eOk;
  try {
    source::Activate();
    const auto generation = options::CurrentFrame(output).generation;
    check(!options::CurrentFrame(output).available, "an SDK scope cannot be invented before a real marker");
    const FakeFrameToken zero(0);
    check(source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, zero) == sl::Result::eOk
            && mock_calls.load() == 1 && mock_token.load() == &zero && mock_marker == sl::PCLMarker::ePresentStart,
        "the real setter receives the original marker and borrowed token unchanged exactly once");
    check(!options::CurrentFrame(output).available, "a successful marker alone is not an active SDK Present");
    uint64_t first_epoch = 0;
    {
      const source::SdkScope sdk(output);
      const auto frame = options::CurrentFrame(output);
      first_epoch = frame.epoch;
      check(frame.available && frame.frame == 0 && frame.generation == generation && frame.epoch != 0,
          "the current successful PresentStart grants frame zero with separate lifecycle and marker epochs");
      check(!options::CurrentFrame(other_output).available, "a different output cannot consume the scoped token");
      {
        const source::SdkScope nested(output);
        check(options::CurrentFrame(output).available && options::CurrentFrame(output).epoch == first_epoch,
            "same-output nested SDK calls keep one source token");
      }
      {
        const source::SdkScope nested(other_output);
        check(!options::CurrentFrame(output).available && !options::CurrentFrame(other_output).available,
            "nested foreign output cannot inherit or retarget the outer SDK token");
      }
      check(!options::CurrentFrame(output).available, "foreign nesting revokes the outer Begin even after its SDK scope resumes");
      std::atomic_bool foreign_reader{true};
      std::jthread reader([&] { foreign_reader.store(options::CurrentFrame(output).available); });
      reader.join();
      check(!foreign_reader.load(), "a worker cannot read another thread's SDK binding");
      source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentEnd, zero);
      check(!options::CurrentFrame(output).available, "PresentEnd revokes an entered SDK token immediately");
    }
    check(!options::CurrentFrame(output).available, "SDK return ends its thread-local authorization");

    const FakeFrameToken next(1);
    source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, next);
    {
      const source::SdkScope test_present(output, false);
      check(!options::CurrentFrame(output).available, "TEST or disabled SDK scopes cannot authorize a source frame");
    }
    {
      const source::SdkScope sdk(output);
      const auto frame = options::CurrentFrame(output);
      check(frame.available && frame.frame == 1 && frame.epoch > first_epoch && frame.generation == generation,
          "a fresh token changes marker epoch without changing cache-source lifecycle generation");
    }
    {
      const source::SdkScope repeated_sdk(output);
      check(!options::CurrentFrame(output).available, "one marker cannot be redeemed by a later SDK Present");
    }
    source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, next);
    {
      const source::SdkScope repeated_token(output);
      check(options::CurrentFrame(output).available && options::CurrentFrame(output).frame == 1,
          "a genuine repeated token remains that same source frame rather than an observer counter");
      mock_result = sl::Result::eErrorInvalidState;
      check(source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, next) == mock_result
              && !options::CurrentFrame(output).available,
          "a failed new writer preserves its real error and cannot inherit old success");
      mock_result = sl::Result::eOk;
    }
    source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, next);
    {
      const source::SdkScope sdk(output);
      source::ObserveMarker(MockMarker, sl::PCLMarker::eRenderSubmitEnd, next);
      check(!options::CurrentFrame(output).available, "the Unity alternate writer revokes but never grants this standard path");
    }
    {
      const source::SdkScope sdk(output);
      check(!options::CurrentFrame(output).available, "alternate marker success is not a PresentStart receipt");
    }

    const FakeFrameToken reentrant(100);
    source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, reentrant);
    {
      const source::SdkScope sdk(output);
      check(!options::CurrentFrame(output).available, "a reentrant real setter cannot publish either overlapping token");
    }
    for (const bool first_returns_first : {true, false}) {
      source::MarkerCall first(sl::PCLMarker::ePresentStart, 201);
      source::MarkerCall second(sl::PCLMarker::ePresentStart, 202);
      if (first_returns_first) {
        first.Finish(true);
        second.Finish(true);
      } else {
        second.Finish(true);
        first.Finish(true);
      }
      const source::SdkScope sdk(output);
      check(!options::CurrentFrame(output).available, "neither return ordering can repair an overlapped writer interval");
    }

    {
      Events events;
      check(events.entered != nullptr && events.release != nullptr, "concurrent writer events exist");
      mock_entered = events.entered;
      mock_release = events.release;
      blocked_frame = 301;
      std::jthread writer([&] {
        const FakeFrameToken frame(301);
        source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, frame);
      });
      check(WaitForSingleObject(events.entered, 2000) == WAIT_OBJECT_0, "the real writer enters without holding observer metadata");
      const FakeFrameToken later(302);
      source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, later);
      {
        const source::SdkScope sdk(output);
        check(!options::CurrentFrame(output).available, "another thread's in-flight writer prevents early publication");
      }
      SetEvent(events.release);
      writer.join();
      mock_entered = mock_release = nullptr;
      const source::SdkScope sdk(output);
      check(!options::CurrentFrame(output).available, "a late return cannot replace the SDK's unknown actual write order");
    }
    {
      Events events;
      check(events.entered != nullptr && events.release != nullptr, "cross-thread SDK events exist");
      std::atomic_bool owner_available{false};
      std::jthread owner([&] {
        const FakeFrameToken frame(401);
        source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, frame);
        SetEvent(events.entered);
        if (WaitForSingleObject(events.release, 2000) != WAIT_OBJECT_0) return;
        const source::SdkScope sdk(output);
        owner_available.store(options::CurrentFrame(output).available);
      });
      check(WaitForSingleObject(events.entered, 2000) == WAIT_OBJECT_0, "the other thread publishes its actual marker");
      {
        const source::SdkScope wrong_thread(output);
        check(!options::CurrentFrame(output).available, "same output and token do not authorize a different present thread");
      }
      SetEvent(events.release);
      owner.join();
      check(owner_available.load(), "a rejected foreign thread cannot steal the writer thread's fresh SDK scope");
    }
    {
      const auto before = source::Snapshot();
      const auto thread = GetCurrentThreadId();
      check(before.current_thread == thread && before.tls_depth == 0 && !before.binding_active
              && before.events.sdk_active == 0 && before.events.sdk_enters == before.events.sdk_exits,
          "a pre-Present snapshot distinguishes the caller thread with no current SDK scope");
      const FakeFrameToken frame(450);
      source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, frame);
      const auto marked = source::Snapshot();
      check(marked.events.marker_enter.sequence > before.events.sequence
              && marked.events.marker_return.sequence > marked.events.marker_enter.sequence
              && marked.events.marker_enter.thread == thread && marked.events.marker_return.thread == thread
              && marked.events.marker_enter.marker == static_cast<uint32_t>(sl::PCLMarker::ePresentStart)
              && marked.events.marker_return.frame == 450 && marked.events.marker_return.success
              && marked.events.marker_return.qpc >= marked.events.marker_enter.qpc && marked.marker_published,
          "marker entry and real return have distinct ordered CPU events, QPC ticks and actual token fields");
      uint64_t sdk_enter_sequence = 0;
      {
        const source::SdkScope sdk(output);
        const auto entered = source::Snapshot();
        sdk_enter_sequence = entered.events.sdk_enter.sequence;
        check(entered.events.sdk_enters == before.events.sdk_enters + 1
                && entered.events.sdk_exits == before.events.sdk_exits && entered.events.sdk_active == 1
                && entered.events.sdk_enter.thread == thread && entered.events.sdk_enter.output == output
                && entered.events.sdk_enter.sequence > marked.events.marker_return.sequence
                && entered.tls_depth == 1 && entered.current_output == output
                && entered.binding_active && entered.binding_available && entered.binding_frame == 450,
            "the current SDK entry is distinguishable from the preceding marker and later NGX caller");
        const auto admitted = options::CurrentFrame(output);
        source::ObservationSnapshot foreign;
        std::jthread reader([&] { foreign = source::Snapshot(); });
        reader.join();
        check(foreign.current_thread != thread && foreign.tls_depth == 0 && !foreign.binding_active
                && foreign.events.sdk_active == 1 && foreign.events.sdk_enter.thread == thread
                && foreign.events.sdk_enter.output == output,
            "another NGX thread sees global active SDK work without inheriting its thread-local binding");
        const auto after_read = options::CurrentFrame(output);
        check(after_read.available && admitted.frame == after_read.frame && admitted.epoch == after_read.epoch
                && source::Snapshot().events.sequence == entered.events.sequence,
            "read-only diagnostic snapshots neither publish an event nor change frame admission");
        {
          const source::SdkScope nested(output);
          const auto nested_snapshot = source::Snapshot();
          check(nested_snapshot.tls_depth == 2 && nested_snapshot.events.sdk_active == 2
                  && nested_snapshot.binding_available && options::CurrentFrame(output).available,
              "SDK diagnostic depth counts same-output nesting without changing its authorization");
        }
      }
      const auto returned = source::Snapshot();
      check(returned.events.sdk_enter.sequence > sdk_enter_sequence
              && returned.events.sdk_exit.sequence > returned.events.sdk_enter.sequence
              && returned.events.sdk_exit.thread == thread && returned.events.sdk_exit.output == output
              && returned.events.sdk_exit.qpc >= returned.events.sdk_enter.qpc
              && returned.events.sdk_active == 0 && returned.events.sdk_enters == returned.events.sdk_exits
              && returned.tls_depth == 0 && !returned.binding_active && !options::CurrentFrame(output).available,
          "post-SDK observations expose exit ordering while preserving the original outside-scope rejection");
    }
    {
      Events events;
      check(events.entered != nullptr && events.release != nullptr, "Stop writer events exist");
      mock_entered = events.entered;
      mock_release = events.release;
      blocked_frame = 501;
      std::jthread writer([&] {
        const FakeFrameToken frame(501);
        source::ObserveMarker(MockMarker, sl::PCLMarker::ePresentStart, frame);
      });
      check(WaitForSingleObject(events.entered, 2000) == WAIT_OBJECT_0, "Stop observes a real in-flight writer");
      source::Stop();
      SetEvent(events.release);
      writer.join();
      mock_entered = mock_release = nullptr;
      const source::SdkScope sdk(output);
      check(!options::CurrentFrame(output).available && options::CurrentFrame(output).generation != generation
              && std::strcmp(options::CurrentFrame(output).reason, "frame_source_stopped") == 0,
          "Stop changes source lifecycle and rejects late writer success");
      source::Activate();
      check(!options::CurrentFrame(output).available, "late installation cannot reactivate a stopped observer");
    }
    std::cout << "PASS native-frame-source CPU/MOCK checks=" << checks
              << "; GPU/NGX/IN_GAME=NotRun; no hooks installed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL native-frame-source: " << error.what() << '\n';
    return 1;
  }
}

}  // namespace native_fg_frame_cases
#endif
