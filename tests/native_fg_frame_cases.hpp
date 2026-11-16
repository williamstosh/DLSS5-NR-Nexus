/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)
#include <atomic>
#include <cstring>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace native_fg_frame_cases {
namespace options = renodx::addons::dlss5::fg::native_options;
namespace source = options::frame_source;
namespace runtime = renodx::addons::dlss5::internal;
namespace submission = renodx::addons::dlss5::submission;

struct FakeFrameToken : sl::FrameToken {
  uint32_t value;
  mutable uint32_t conversions = 0;
  explicit FakeFrameToken(uint32_t frame) : value(frame) {}
  operator uint32_t() const override {
    ++conversions;
    return value;
  }
};

static std::atomic_uint32_t mock_calls{0};
static std::atomic<const sl::FrameToken*> mock_token{nullptr};
static sl::PCLMarker mock_marker = sl::PCLMarker::eSimulationStart;
static sl::Result mock_result = sl::Result::eOk;
static HANDLE mock_entered = nullptr;
static HANDLE mock_release = nullptr;
static uint32_t blocked_frame = 0;
static thread_local bool reentering = false;
static std::function<sl::Result(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**,
    uint32_t, sl::CommandBuffer*)> evaluate_handler;
static uint64_t premise_identity = 0;
static uint32_t premise_reads = 0, premise_writes = 0, premise_forwards = 0;
static uintptr_t premise_forward_list = 0;
static uint32_t premise_forward_count = 0;
static const void* premise_forward_array = nullptr;
inline HRESULT STDMETHODCALLTYPE MockPremiseGetData(ID3D12CommandList*, REFGUID, UINT* size, void* data) {
  ++premise_reads;
  if (premise_identity == 0) return E_FAIL;
  if (size == nullptr || data == nullptr || *size != sizeof(premise_identity)) return E_INVALIDARG;
  std::memcpy(data, &premise_identity, sizeof(premise_identity));
  return S_OK;
}
inline HRESULT STDMETHODCALLTYPE MockPremiseSetData(ID3D12CommandList*, REFGUID, UINT, const void*) {
  ++premise_writes;
  return E_FAIL;
}
inline void STDMETHODCALLTYPE MockPremiseBarrier(ID3D12GraphicsCommandList* list, UINT count, const D3D12_RESOURCE_BARRIER* barriers) {
  ++premise_forwards;
  premise_forward_list = reinterpret_cast<uintptr_t>(list);
  premise_forward_count = count;
  premise_forward_array = barriers;
}
inline void STDMETHODCALLTYPE MockPremiseEnhanced(ID3D12GraphicsCommandList7* list, UINT32 count, const D3D12_BARRIER_GROUP* groups) {
  ++premise_forwards;
  premise_forward_list = reinterpret_cast<uintptr_t>(list);
  premise_forward_count = count;
  premise_forward_array = groups;
}

inline sl::Result MockEvaluate(sl::Feature feature, const sl::FrameToken& frame,
    const sl::BaseStructure** inputs, uint32_t num_inputs, sl::CommandBuffer* command_buffer) {
  return evaluate_handler(feature, frame, inputs, num_inputs, command_buffer);
}

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
    source::EvaluateBinding evaluate_binding = source::evaluate_binding;
    uint64_t ngx_call = source::ngx_call;
    uint32_t ngx_depth = source::ngx_depth;
    bool edit_reuse = runtime::edit_reuse_enabled.load();
    bool fg_experiment = runtime::edit_reuse_fg_experiment.load();
    bool shutting_down = runtime::shutting_down.load();
    decltype(runtime::real_streamline_evaluate) real_evaluate = runtime::real_streamline_evaluate;
    Restore() {
      std::unique_lock lock(source::state.mutex);
      metadata = source::state.metadata;
      source::state.metadata = {};
      source::sdk_binding = {};
      source::sdk_depth = 0;
      source::evaluate_binding = {};
      source::ngx_call = 0;
      source::ngx_depth = 0;
    }
    ~Restore() {
      std::unique_lock lock(source::state.mutex);
      source::state.metadata = metadata;
      source::sdk_binding = binding;
      source::sdk_depth = depth;
      source::evaluate_binding = evaluate_binding;
      source::ngx_call = ngx_call;
      source::ngx_depth = ngx_depth;
      runtime::edit_reuse_enabled = edit_reuse;
      runtime::edit_reuse_fg_experiment = fg_experiment;
      runtime::shutting_down = shutting_down;
      runtime::real_streamline_evaluate = real_evaluate;
      evaluate_handler = {};
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
      const auto before = source::Snapshot();
      const FakeFrameToken request_frame(0);
      const FakeFrameToken nested_frame(UINT32_MAX);
      const sl::BaseStructure* inputs[] = {reinterpret_cast<const sl::BaseStructure*>(0x6600)};
      auto* command_buffer = reinterpret_cast<sl::CommandBuffer*>(0x7700);
      uint32_t forwarded = 0;
      bool observe = false;
      uint64_t gateway_call = 0;
      source::NgxEvent delegated;
      source::ObservationSnapshot worker;
      runtime::real_streamline_evaluate = MockEvaluate;
      runtime::shutting_down = false;
      runtime::edit_reuse_enabled = true;
      runtime::edit_reuse_fg_experiment = false;
      evaluate_handler = [&](sl::Feature feature, const sl::FrameToken& frame,
                             const sl::BaseStructure** actual_inputs, uint32_t num_inputs,
                             sl::CommandBuffer* actual_command) {
        ++forwarded;
        check(actual_inputs == inputs && num_inputs == 1 && actual_command == command_buffer,
            "the real gateway forwards original input and opaque command-buffer addresses without querying them");
        check(&frame == (feature == sl::kFeatureDLSS_RR ? &nested_frame : &request_frame),
            "the real gateway passes the original borrowed FrameToken unchanged");
        if (!observe) return sl::Result::eErrorInvalidState;
        const auto entered = source::Snapshot();
        check(entered.current_evaluate.feature == feature && entered.current_evaluate.command_buffer == 0x7700
                && entered.current_evaluate.inputs == 1 && !options::CurrentFrame(output).available,
            "the request observation preserves API arguments without granting a source frame");
        if (feature == sl::kFeatureDLSS_RR) {
          check(entered.current_evaluate.frame == UINT32_MAX && entered.current_evaluate.depth == 2
                  && entered.current_evaluate.parent == gateway_call
                  && entered.current_evaluate.dlssg_call == gateway_call && entered.current_evaluate.dlssg_frame == 0,
              "a nested gateway preserves its own frame and the actual enclosing DLSS-G call");
          source::NgxCall ngx(true, true, 2, false, false, 0x3300, 0x4400);
          ngx.Finish(0xBAD00002);
          return sl::Result::eErrorInvalidState;
        }
        gateway_call = entered.current_evaluate.call;
        check(entered.current_evaluate.frame == 0 && entered.current_evaluate.depth == 1
                && entered.current_evaluate.parent == 0 && entered.current_evaluate.dlssg_call == gateway_call,
            "the SDK's public conversion preserves request frame zero rather than borrowing the latest Reflex token");
        {
          source::NgxCall ngx(true, true, 3, true, false, 0x3300, 0x4400);
          const auto outer_call = source::ngx_call;
          {
            source::NgxCall cpp(true, false, 4, false, true, 0x3300, 0x4400);
            delegated = source::Snapshot().evaluations.ngx_enter;
            check(delegated.parent == outer_call && delegated.depth == 2 && delegated.wrapper_delegate
                    && !delegated.c_export && delegated.gateway.call == gateway_call
                    && delegated.gateway.frame == 0,
                "the real C-to-C++ nesting relationship remains a call relationship, not another source frame");
            cpp.Finish(0xBAD00003);
          }
          check(source::ngx_call == outer_call && source::ngx_depth == 1
                  && source::Snapshot().evaluations.ngx_return.returned,
              "a nested NGX return restores the enclosing call and retains the actual API result");
          ngx.Finish(0xBAD00001);
        }
        check(source::ngx_call == 0 && source::ngx_depth == 0,
            "NGX observation ends before unrelated calls can inherit its parent");
        check(runtime::HookedStreamlineEvaluate(sl::kFeatureDLSS_RR, nested_frame, inputs, 1, command_buffer)
                    == sl::Result::eErrorInvalidState
                && source::Snapshot().current_evaluate.call == gateway_call,
            "a nested actual gateway returns its real error and restores its parent request");
        std::jthread consumer([&] {
          source::NgxCall ngx(true, true, 5, false, false, 0x3300, 0x4400);
          worker = source::Snapshot();
          ngx.Finish(0xBAD00004);
        });
        consumer.join();
        check(worker.current_evaluate.call == 0 && worker.evaluations.ngx_fg_enter.gateway.call == 0
                && worker.evaluations.ngx_fg_enter.gateway.dlssg_call == 0
                && worker.evaluations.active == 1 && worker.current_thread != entered.current_thread,
            "an asynchronous consumer observes active work but never inherits another thread's request frame");
        return sl::Result::eErrorInvalidState;
      };
      check(runtime::HookedStreamlineEvaluate(sl::kFeatureDLSS_G, request_frame, inputs, 1, command_buffer)
                  == sl::Result::eErrorInvalidState
              && forwarded == 1 && request_frame.conversions == 0
              && source::Snapshot().events.sequence == before.events.sequence,
          "the default-disabled observation forwards exactly once without reading a token or changing diagnostic state");
      runtime::edit_reuse_fg_experiment = true;
      observe = true;
      check(runtime::HookedStreamlineEvaluate(sl::kFeatureDLSS_G, request_frame, inputs, 1, command_buffer)
                  == sl::Result::eErrorInvalidState
              && forwarded == 3 && request_frame.conversions == 1 && nested_frame.conversions == 1,
          "the enabled actual gateway reads each SDK token only through its public numeric conversion");
      const auto returned = source::Snapshot();
      const auto& fg = returned.evaluations.features[source::EvaluateFeatureIndex(sl::kFeatureDLSS_G)];
      check(returned.current_evaluate.call == 0 && returned.evaluations.enters == 2
              && returned.evaluations.returns == 2 && returned.evaluations.active == 0
              && fg.enters == 1 && fg.returns == 1 && fg.ngx_calls == 2 && fg.ngx_fg_calls == 1
              && fg.returned.returned && fg.returned.result == static_cast<uint32_t>(sl::Result::eErrorInvalidState)
              && fg.returned.cpu.sequence > fg.enter.cpu.sequence
              && fg.returned.cpu.qpc >= fg.enter.cpu.qpc && fg.enter.cpu.thread == GetCurrentThreadId(),
          "feature counts and ordered request/return events describe CPU API calls with the real result");
      check(returned.evaluations.ngx_calls == 4 && returned.evaluations.ngx_fg_calls == 3
              && returned.evaluations.ngx_fg_nested == 2 && returned.evaluations.ngx_fg_under_dlssg == 2
              && delegated.call != 0 && returned.evaluations.ngx_fg_return.returned
              && returned.evaluations.ngx_fg_return.result == 0xBAD00004,
          "nested and asynchronous FG forwards remain distinguishable without counting C delegates as frames");
      check(returned.marker_epoch == before.marker_epoch && returned.generation == before.generation
              && returned.marker_published == before.marker_published && returned.marker_claimed == before.marker_claimed
              && returned.marker_frame == before.marker_frame && !options::CurrentFrame(output).available,
          "all new observations leave marker publication, epochs, claims and frame admission unchanged");
      evaluate_handler = [&](sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*) {
        ++forwarded;
        return sl::Result::eErrorInvalidState;
      };
      for (uint32_t index = 0; index < 12; ++index) {
        runtime::HookedStreamlineEvaluate(2000 + index, request_frame, inputs, 1, command_buffer);
      }
      const auto bounded = source::Snapshot();
      check(bounded.evaluations.features.size() == 4 && bounded.evaluations.features[3].enters == 12
              && bounded.evaluations.features[3].enter.request.feature == 2011
              && bounded.evaluations.features[2].enter.request.call == gateway_call,
          "arbitrary feature IDs share one bounded bucket and cannot evict the DLSS-G observation");
      runtime::shutting_down = true;
      const auto calls_before_drain = forwarded;
      check(runtime::HookedStreamlineEvaluate(sl::kFeatureDLSS_G, request_frame, inputs, 1, command_buffer)
                  == sl::Result::eErrorInvalidState
              && forwarded == calls_before_drain + 1,
          "the draining gateway continues forwarding the real call and result without changing shutdown behavior");
      runtime::shutting_down = false;
      runtime::real_streamline_evaluate = nullptr;
      const auto before_null = source::Snapshot();
      check(runtime::HookedStreamlineEvaluate(sl::kFeatureDLSS_G, request_frame, inputs, 1, command_buffer)
                  == sl::Result::eErrorInvalidParameter
              && forwarded == calls_before_drain + 1 && source::Snapshot().events.sequence == before_null.events.sequence,
          "a missing real API neither forwards nor invents an evaluate observation");
    }
    {
      namespace probe = runtime::native_tag_source;
      struct RestorePremise {
        probe::PremiseSnapshot snapshot;
        std::array<probe::PremiseEvent, 5> live;
        std::array<uint64_t, 2> calls;
        uint64_t sequence = 0, epoch = 0;
        bool enabled = runtime::enabled.load();
        bool edit_reuse = runtime::edit_reuse_enabled.load(), fg_experiment = runtime::edit_reuse_fg_experiment.load();
        bool owns_key = false;
        decltype(runtime::real_premise_barrier) legacy = runtime::real_premise_barrier;
        decltype(runtime::real_premise_enhanced) enhanced = runtime::real_premise_enhanced;
        RestorePremise() {
          std::unique_lock lock(probe::premise.mutex);
          snapshot = probe::premise.snapshot;
          live = probe::premise.live;
          calls = probe::premise.barrier_calls;
          sequence = probe::premise.sequence;
          epoch = probe::premise.epoch;
        }
        ~RestorePremise() {
          {
            std::unique_lock lock(probe::premise.mutex);
            probe::premise.snapshot = snapshot;
            probe::premise.live = live;
            probe::premise.barrier_calls = calls;
            probe::premise.sequence = sequence;
            probe::premise.epoch = epoch;
          }
          std::lock_guard lock(submission::tracker_mutex);
          if (owns_key) {
            submission::tracked_lists.erase(0xF0000022);
          }
          runtime::enabled = enabled;
          runtime::edit_reuse_enabled = edit_reuse;
          runtime::edit_reuse_fg_experiment = fg_experiment;
          runtime::real_premise_barrier = legacy;
          runtime::real_premise_enhanced = enhanced;
        }
      } restore_premise;
      probe::ResetPremise();
      std::array<void*, 5> vtable{};
      vtable[3] = reinterpret_cast<void*>(MockPremiseGetData);
      vtable[4] = reinterpret_cast<void*>(MockPremiseSetData);
      struct { void** vtable; } fake_list{vtable.data()};
      auto* list = reinterpret_cast<ID3D12GraphicsCommandList*>(&fake_list);
      auto* list7 = reinterpret_cast<ID3D12GraphicsCommandList7*>(&fake_list);
      auto* resource = reinterpret_cast<ID3D12Resource*>(0x4400);
      runtime::real_premise_barrier = MockPremiseBarrier;
      runtime::real_premise_enhanced = MockPremiseEnhanced;
      runtime::enabled = true;
      runtime::edit_reuse_enabled = true;
      runtime::edit_reuse_fg_experiment = false;
      premise_identity = premise_reads = premise_writes = premise_forwards = 0;
      D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
          .Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE,
          .Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
              D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE}};
      D3D12_TEXTURE_BARRIER texture{.SyncBefore = D3D12_BARRIER_SYNC_COPY, .SyncAfter = D3D12_BARRIER_SYNC_COMPUTE_SHADING,
          .AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST, .AccessAfter = D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
          .LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST, .LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE,
          .pResource = resource, .Subresources = {2, 3, 4, 5, 6, 7}, .Flags = D3D12_TEXTURE_BARRIER_FLAG_DISCARD};
      D3D12_BARRIER_GROUP group{.Type = D3D12_BARRIER_TYPE_TEXTURE, .NumBarriers = 1, .pTextureBarriers = &texture};
      const auto unknown = runtime::InstallNativePremiseBarrierHooks(list);
      runtime::HookedNativePremiseBarrier(list, 1, &barrier);
      check(premise_forwards == 1 && premise_forward_list == reinterpret_cast<uintptr_t>(list)
              && premise_forward_count == 1 && premise_forward_array == &barrier && premise_reads == 0,
          "the legacy native entry preserves its original face, count and array in one real call");
      runtime::HookedNativePremiseEnhanced(list7, 1, &group);
      check(!unknown.legacy && !runtime::premise_hooks_installed && premise_reads == 0 && premise_writes == 0
              && premise_forwards == 2 && premise_forward_list == reinterpret_cast<uintptr_t>(list7)
              && premise_forward_count == 1 && premise_forward_array == &group,
          "OFF installs nothing and both real native entry points forward the original face/array exactly once without metadata queries");
      runtime::edit_reuse_fg_experiment = true;
      const auto tracker_size = submission::tracked_lists.size();
      const auto next_identity = submission::next_list_identity.load();
      const auto tracked_uses = submission::HasEverTrackedUses();
      check(probe::ReadPremiseRecording(list).list == 0 && premise_writes == 0
              && submission::tracked_lists.size() == tracker_size && submission::next_list_identity.load() == next_identity,
          "a missing private identity remains unknown without SetPrivateData or tracker entry creation");
      premise_identity = 0xF0000022;
      check(probe::ReadPremiseRecording(list).generation == 0 && submission::tracked_lists.size() == tracker_size,
          "a readable identity alone does not manufacture a recording generation");
      {
        std::lock_guard lock(submission::tracker_mutex);
        check(!submission::tracked_lists.contains(premise_identity), "the CPU fixture owns its synthetic tracker key");
        submission::tracked_lists[premise_identity].current = 7;
        restore_premise.owns_key = true;
      }
      probe::PremiseSnapshot tag{.native = 0x4400, .identity = 0x4410, .viewport = 12, .frame = 0,
          .declared_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, .list_type = D3D12_COMMAND_LIST_TYPE_DIRECT,
          .hooks = {.legacy = true, .list7_acquired = true, .enhanced = true},
          .tag = {.list = reinterpret_cast<uintptr_t>(list), .recording = probe::ReadPremiseRecording(list)}};
      probe::FreezePremiseTag(tag);
      probe::ObservePremiseCopy(list, reinterpret_cast<ID3D12Resource*>(0x5500), resource, true);
      runtime::HookedNativePremiseBarrier(list, 1, &barrier);
      const auto ticket = probe::FreezePremiseTag(tag);
      auto frozen = probe::premise.snapshot;
      check(frozen.before[0].legacy.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES
              && frozen.before[0].legacy.Transition.StateBefore == D3D12_RESOURCE_STATE_COPY_DEST
              && static_cast<uint32_t>(frozen.before[0].legacy.Transition.StateAfter) == tag.declared_state
              && frozen.before[0].recording.list == premise_identity && frozen.before[0].recording.generation == 7
              && frozen.before[2].full && frozen.before[2].sequence < frozen.before[0].sequence
              && frozen.before[0].sequence < ticket,
          "actual legacy forwarding and an existing strong recording preserve full-write/transition/tag CPU order");
      barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY;
      barrier.Transition.Subresource = 3;
      runtime::HookedNativePremiseBarrier(list, 1, &barrier);
      runtime::HookedNativePremiseEnhanced(list7, 1, &group);
      check(probe::premise.snapshot.before[0].legacy.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE
              && probe::premise.snapshot.before[1].sequence == 0,
          "later partial/split/enhanced commands never rewrite the earlier tag-entry snapshot");
      probe::FreezePremiseTag(tag);
      frozen = probe::premise.snapshot;
      check(frozen.before[0].legacy.Flags == D3D12_RESOURCE_BARRIER_FLAG_BEGIN_ONLY
              && frozen.before[0].legacy.Transition.Subresource == 3
              && frozen.before[1].enhanced.Subresources.NumPlanes == 7
              && frozen.before[1].enhanced.LayoutAfter == texture.LayoutAfter
              && frozen.before[1].enhanced.SyncBefore == texture.SyncBefore
              && frozen.before[1].enhanced.AccessAfter == texture.AccessAfter && frozen.before[1].enhanced.Flags == texture.Flags,
          "split/subresource and enhanced layout/sync/access/range/flags remain exact raw evidence without state conversion");
      probe::FinishPremiseTag(ticket, true);
      check(!probe::premise.snapshot.returned, "a superseded tag return cannot update a later observation");
      probe::FinishPremiseTag(probe::premise.snapshot.tag.sequence, false);
      {
        std::lock_guard lock(submission::tracker_mutex);
        auto& tracked = submission::tracked_lists.at(premise_identity);
        tracked.current = 8;
        auto& submitted = tracked.generations[7];
        submitted.status = std::make_shared<submission::RecordingStatus>();
        submitted.status->submitted.store(true);
        submitted.submissions.push_back({.pending = false, .value = 9, .list_identity = premise_identity, .recording_generation = 7});
      }
      auto* queue = reinterpret_cast<ID3D12CommandQueue*>(0x6600);
      probe::ObservePremiseSubmit(queue, {{.list = premise_identity, .generation = 7}});
      check(probe::ReadPremiseRecording(list).generation == 8
              && probe::premise.snapshot.submitted_after_tag.recording.generation == 7
              && probe::premise.snapshot.submitted_after_tag.recording.value == 9,
          "a submit frozen at generation seven stays seven after current advances to eight");
      const auto submitted_sequence = probe::premise.snapshot.submitted_after_tag.sequence;
      probe::ObservePremiseSubmit(queue, {{.list = premise_identity, .generation = 8}});
      check(probe::premise.snapshot.submitted_after_tag.sequence == submitted_sequence,
          "a different captured recording cannot be reassigned to the tag's earlier generation");
      std::ostringstream evidence;
      probe::AppendPremiseObservation(evidence);
      check(evidence.str().find("gpu_completion=unqueried") != std::string::npos
              && evidence.str().find("enhanced_state_mapping=none") != std::string::npos
              && probe::premise.snapshot.submitted_after_tag.sequence > probe::premise.snapshot.tag.sequence
              && probe::premise.snapshot.returned && !probe::premise.snapshot.success,
          "actual submit/return observations stay separate from GPU completion and SDK success");
      probe::ObservePremiseCopy(list, reinterpret_cast<ID3D12Resource*>(0x5500), resource, false);
      probe::FreezePremiseTag(tag);
      check(!probe::premise.snapshot.before[2].full, "a Region write remains partial/unknown rather than an asserted full target write");
      barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
      barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      barrier.Aliasing = {resource, nullptr};
      runtime::HookedNativePremiseBarrier(list, 1, &barrier);
      probe::FreezePremiseTag(tag);
      check(probe::premise.snapshot.before[0].legacy.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING
              && probe::premise.snapshot.before[0].legacy.Aliasing.pResourceBefore == resource
              && probe::premise.snapshot.before[0].legacy.Aliasing.pResourceAfter == nullptr,
          "an alias boundary remains an alias event and cannot masquerade as a transition state");
      probe::ObservePremiseEvent({.kind = probe::PremiseKind::RESET, .list = tag.tag.list, .full = true});
      probe::FreezePremiseTag(tag);
      check(probe::premise.snapshot.before[0].sequence == 0 && probe::premise.snapshot.before[1].sequence == 0
              && probe::premise.snapshot.before[2].sequence == 0 && probe::premise.snapshot.before[3].sequence != 0,
          "Reset removes earlier recording facts instead of carrying state into a new recording");
      const auto epoch = probe::premise.snapshot.epoch;
      tag.native = 0x7700;
      probe::FreezePremiseTag(tag);
      check(probe::premise.snapshot.epoch > epoch && probe::premise.snapshot.before[3].sequence == 0,
          "a changed resource starts an independent observation epoch");
      probe::ObservePremiseEvent({.kind = probe::PremiseKind::LEGACY, .source = 0x4400, .legacy = barrier});
      probe::FreezePremiseTag(tag);
      check(probe::premise.snapshot.before[0].sequence == 0,
          "a delayed event for the previous resource cannot contaminate the new observation");
      probe::ForgetPremiseResource(tag.native);
      check(probe::PremiseNative() == 0 && premise_writes == 0 && submission::HasEverTrackedUses() == tracked_uses
              && submission::next_list_identity.load() == next_identity,
          "resource destruction clears borrowed facts and all observations leave production identity/use authority untouched");
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
