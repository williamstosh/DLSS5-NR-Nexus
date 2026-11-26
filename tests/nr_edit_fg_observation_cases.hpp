/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included by the existing DEV control runtime fixture. All identities and SDK
// returns below are CPU substitutes; no device, GPU work or game is created.
{
  namespace observation = renodx::addons::dlss5::edit_fg_observation;
  observation::Snapshot observed;
  const auto read = [&]() -> const observation::Snapshot& {
    observation::CopySnapshot(&observed);
    return observed;
  };
  const observation::SrCopyback source{.source_handle = 0x1234, .source_generation = 71,
      .sr_sequence = 100, .output = {.raw = 0x4567, .canonical = 0x4567},
      .width = 1920, .height = 1080, .format = DXGI_FORMAT_R16G16B16A16_FLOAT,
      .recording = {.raw = 0x6789, .list_id = 31, .generation = 13},
      .environment_provider = 2, .environment_context = 0x9876, .environment_generation = 83};
  {
    const bool was_enabled = runtime::enabled.exchange(true);
    const auto production = [&] {
      return std::tuple{control::Identity(), runtime::profile_ui::Snapshot(true),
          configuration_writes, runtime::nr_features_created, runtime::SourceSuccessfulEvaluations(),
          runtime::history_reset_epoch.load(), runtime::native_fg_owns.load(),
          runtime::edit_reuse_refreshes.load(), runtime::edit_reuse_reuses.load(), runtime::edit_reuse_fallbacks.load(),
          runtime::CopyAdaptiveStatus().generation};
    };
    const auto before = production();
    const Json arm{{"id", "fg-execution-arm"}, {"op", "fg_execution_observation"}, {"action", "arm"}};
    const auto receipt = control::Execute(arm);
    const auto request_generation = observation::CaptureGeneration(true);
    check(receipt["ok"] == true && receipt["request_generation"] == request_generation && request_generation != 0
            && !receipt["state"].contains("edit_reuse") && read().state == observation::State::ARMED
            && observation::CaptureGeneration() == 0 && observation::CaptureSrCopyback(false) == 0,
        "arm works with NR On and awaits a successful SR return without caching the trace");
    observation::ObserveSourceFrame(source.source_handle, source.source_generation, source.sr_sequence - 1);
    observation::RecordSrCopyback(request_generation, source);
    check(read().state == observation::State::ARMED && observed.count == 0,
        "an Evaluate entry and an unbound copyback cannot start an on-demand trace");
    observation::ObserveSourceSuccess(source.source_handle, source.source_generation, source.sr_sequence);
    const auto status = control::Execute({{"id", "fg-execution-read"}, {"op", "fg_execution_observation"}, {"action", "status"}});
    const auto& success_trace = status["state"]["edit_reuse"]["fg_observation"];
    check(success_trace["event_count"] == 1 && success_trace["source_copyback_observed"] == false
            && success_trace["events"][0]["kind"] == "source_success"
            && control::Execute(arm) == receipt && read().count == 1,
        "successful SR alone starts a Present trace and duplicate arm returns its cached receipt");
    auto later_copyback = source;
    ++later_copyback.sr_sequence;
    observation::ObserveSourceSuccess(source.source_handle, source.source_generation, later_copyback.sr_sequence);
    observation::RecordSrCopyback(request_generation, later_copyback);
    check(read().state == observation::State::CAPTURING && observed.count == 3 && observed.source_frames == 2
            && observed.source_copyback_observed && observed.source.environment_context == source.environment_context
            && observed.source.sr_sequence == later_copyback.sr_sequence
            && observed.events[2].kind == observation::Kind::SR_COPYBACK,
        "the first same-source writeback fills the snapshot without changing the successful SR frame count");
    for (uint64_t sequence = 102; sequence <= 104; ++sequence) {
      observation::ObserveSourceSuccess(source.source_handle, source.source_generation, sequence);
    }
    check(read().state == observation::State::STOPPED && observed.source_frames == 4 && observed.count == 5,
        "on-demand observation stops at the fifth success even if later SR frames never write back");
    const auto rearm = control::Execute({{"id", "fg-execution-rearm"}, {"op", "fg_execution_observation"}, {"action", "arm"}});
    check(rearm["ok"] == true && read().request_generation > request_generation && observed.count == 0
            && observation::Record(request_generation, {.kind = observation::Kind::XEFG_CONSTANTS}).call_id == 0,
        "a new arm resets only the observer and rejects results from the previous request");
    const Json stop{{"id", "fg-execution-stop"}, {"op", "fg_execution_observation"}, {"action", "stop"}};
    const auto stopped = control::Execute(stop);
    check(stopped["ok"] == true && control::Execute(stop) == stopped && read().state == observation::State::STOPPED,
        "stop is idempotent and seals the current request");
    const auto invalid = control::Execute({{"id", "fg-execution-invalid"}, {"op", "fg_execution_observation"}, {"action", 1}});
    check(invalid["ok"] == false && invalid["reason"] == "invalid_fg_observation_action"
            && production() == before && runtime::enabled.load(),
        "observer commands leave production settings, ownership, counters, history and adaptive decisions intact");
    runtime::enabled = was_enabled;
  }
  observation::SetEnabled(false);
  observation::SetEnabled(true);
  const auto generation = observation::CaptureGeneration(true);
  check(generation != 0 && observation::CaptureGeneration() == 0,
      "FG observation arms without admitting boot SDK tags before successful SR copyback");
  check(observation::Record(generation, {.kind = observation::Kind::XEFG_TAG_ENTRY}).call_id == 0
          && read().count == 0,
      "armed FG observation does not spend its event budget on pre-NR tags");
  observation::StopSource(0);
  observation::StopEnvironment(1, 0);
  observation::StopEnvironment(2, 0);
  observation::ObserveSourceFrame(0, 0, 1);
  check(read().state == observation::State::ARMED,
      "NR Off maintenance without a captured source or environment preserves the armed window");
  check(observation::CaptureSrCopyback(false) == 0 && observation::CaptureSrCopyback(true) == generation,
      "legacy observation preserves its existing SR eligibility");
  observation::RecordSrCopyback(generation, source);
  observation::SetEnabled(true);
  read();
  check(observed.state == observation::State::CAPTURING && observed.source_frames == 1 && observed.count == 1
          && observed.request_generation == generation && observed.events[0].sr.recording.generation == 13,
      "first successful SR copyback starts the window and unchanged true cannot reset its request or recording");
  const auto native = observation::Record(generation,
      {.kind = observation::Kind::NATIVE_FG_ENTRY,
          .native_fg = {.handle = 0x7788, .generation = 19, .slot = 1, .c_export = true,
              .recording = {.raw = 0x6789, .list_id = 31, .generation = 13},
              .backbuffer = {.raw = 0x4567, .canonical = 0x4567}}});
  const auto returned_ns = observation::NowNs();
  observation::RecordResult(native, -17, returned_ns);
  read();
  check(observed.count == 3 && observed.events[1].kind == observation::Kind::NATIVE_FG_ENTRY
          && observed.events[2].kind == observation::Kind::NATIVE_FG_RETURN
          && observed.events[1].call_id == observed.events[2].call_id
          && observed.events[2].api_result == -17 && observed.events[2].api_return_ns == returned_ns
          && observed.events[2].observed_ns >= returned_ns,
      "native entry and failed return retain call identity and distinct SDK-return and snapshot timestamps");
  {
    auto& adapter = xefg::internal::modules[0];
    const auto previous_resource = adapter.real[3];
    const auto previous_stopped = xefg::internal::stopped.exchange(true);
    adapter.real[3] = reinterpret_cast<void*>(+[](void*, ID3D12CommandList*, uint32_t,
        const xefg::abi::Resource*) -> int32_t { return -23; });
    const xefg::abi::Resource region{.type = 4, .validity = 0,
        .base = {0, 0}, .size = {1920, 1080}, .resource = nullptr};
    const auto result = xefg::internal::SdkHooks<0>::Resource(reinterpret_cast<void*>(0x123456),
        reinterpret_cast<ID3D12CommandList*>(1), 47, &region);
    adapter.real[3] = previous_resource;
    xefg::internal::stopped.store(previous_stopped);
    read();
    const auto& tag = observed.events[3].xefg_tag;
    check(result == -23 && observed.count == 5 && tag.sdk_slot == 0 && tag.type == 4 && tag.validity == 0
            && tag.recording.raw == 1 && tag.recording.reshade_base == 0
            && tag.recording.list_id == 0 && tag.recording.generation == 0
            && tag.resource.raw == 0 && tag.resource.canonical == 0
            && observed.events[4].api_result == -23,
        "actual XeFG Resource wrapper records region list=1/resource=0 without QI, recording invention or SDK-success inference");
  }
  {
    auto& adapter = xefg::internal::modules[0];
    const auto previous_constants = adapter.real[4];
    const auto previous_stopped = xefg::internal::stopped.exchange(true);
    auto context = std::make_shared<xefg::internal::Context>();
    context->epoch = 38;
    struct ConstantCall {
      void* actual_handle = nullptr;
      uint32_t id = 0;
      const xefg::abi::Constants* data = nullptr;
      uint32_t count = 0;
    } call;
    void* handle = &call;
    xefg::internal::state.contexts.emplace(handle, context);
    adapter.real[4] = reinterpret_cast<void*>(+[](void* handle, uint32_t id, const xefg::abi::Constants* data) -> int32_t {
      auto* call = static_cast<ConstantCall*>(handle);
      call->actual_handle = handle;
      call->id = id;
      call->data = data;
      ++call->count;
      return id == 47 ? -29 : 0;
    });
    const auto before = read().count;
    const xefg::abi::Constants zero{};
    const xefg::abi::Constants nonzero{.frame_time_ms = 21.25f};
    const auto failed_result = xefg::internal::SdkHooks<0>::Constants(handle, 47, &zero);
    check(failed_result == -29 && call.count == 1 && call.actual_handle == handle && call.id == 47 && call.data == &zero,
        "observed failed Constants forwards the exact argument once and preserves the SDK result");
    const auto success_result = xefg::internal::SdkHooks<0>::Constants(handle, 48, &nonzero);
    check(success_result == 0 && call.count == 2 && call.actual_handle == handle && call.id == 48 && call.data == &nonzero,
        "observed successful Constants forwards the exact argument once");
    const auto null_result = xefg::internal::SdkHooks<0>::Constants(handle, 49, nullptr);
    xefg::internal::state.contexts.erase(handle);
    adapter.real[4] = previous_constants;
    xefg::internal::stopped.store(previous_stopped);
    read();
    const auto& failed = observed.events[before];
    const auto& success = observed.events[before + 1];
    const auto& missing = observed.events[before + 2];
    const auto events = control::Snapshot()["edit_reuse"]["fg_observation"]["events"];
    check(observed.count == before + 3 && failed.kind == observation::Kind::XEFG_CONSTANTS
            && failed.xefg_constants.sdk_context == reinterpret_cast<uintptr_t>(handle)
            && failed.xefg_constants.context_identity == reinterpret_cast<uintptr_t>(context.get())
            && failed.xefg_constants.epoch == 38 && failed.xefg_constants.present_id == 47
            && failed.xefg_constants.has_frame_time && failed.xefg_constants.frame_time_ms == 0.f
            && failed.api_result == -29 && failed.api_return_ns > 0 && failed.observed_ns >= failed.api_return_ns
            && success.xefg_constants.present_id == 48 && success.xefg_constants.frame_time_ms == 21.25f
            && success.api_result == 0 && null_result == 0 && call.count == 3 && call.data == nullptr
            && !missing.xefg_constants.has_frame_time && events[before]["constants"]["frame_time_ms"] == 0.f
            && events[before + 2]["constants"]["frame_time_ms"].is_null(),
        "Constants diagnostics preserve the pre-call context and ID, real returns, zero and missing frame time");
  }
  {
    namespace tracker = renodx::addons::dlss5::submission;
    struct RecordingList final : ID3D12CommandList {
      RecordingList* base = nullptr;
      uint64_t identity = 0;
      uint32_t native_queries = 0;
      uint32_t canonical_queries = 0;
      uint32_t references = 1;
      uint32_t private_writes = 0;

      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        *result = nullptr;
        if (iid == __uuidof(renodx::utils::directx::ReShadeRetrieveBaseInterface)) {
          ++native_queries;
          if (base == nullptr) return E_NOINTERFACE;
          *result = base;
          base->AddRef();
          return S_OK;
        }
        if (iid == IID_IUnknown) ++canonical_queries;
        return E_NOINTERFACE;
      }
      ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
      ULONG STDMETHODCALLTYPE Release() override { return --references; }
      HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* size, void* data) override {
        if (guid != tracker::kListIdentityGuid || identity == 0 || *size != sizeof(identity)) return E_FAIL;
        *static_cast<uint64_t*>(data) = identity;
        return S_OK;
      }
      HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override {
        ++private_writes;
        return E_NOTIMPL;
      }
      HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
      HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
      HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
      D3D12_COMMAND_LIST_TYPE STDMETHODCALLTYPE GetType() override { return D3D12_COMMAND_LIST_TYPE_DIRECT; }
    } proxy, native_list, foreign;
    struct ReplayFence final : ID3D12Fence {
      uint32_t references = 1;
      uint32_t completion_reads = 0;
      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
      ULONG STDMETHODCALLTYPE AddRef() override { return ++references; }
      ULONG STDMETHODCALLTYPE Release() override { return --references; }
      HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
      HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
      HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
      HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return E_NOTIMPL; }
      HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
      UINT64 STDMETHODCALLTYPE GetCompletedValue() override {
        ++completion_reads;
        return 0;
      }
      HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE) override { return E_NOTIMPL; }
      HRESULT STDMETHODCALLTYPE Signal(UINT64) override { return E_NOTIMPL; }
    } replay_fence;
    proxy.base = &native_list;
    auto witnessed_source = source;
    constexpr uint64_t identity = 0xF6123431;
    uint64_t previous_execute = 0;
    {
      std::lock_guard lock(tracker::tracker_mutex);
      const auto [tracked, inserted] = tracker::tracked_lists.try_emplace(identity);
      check(inserted, "queue observation fixture owns its synthetic recording identity");
      auto& list = tracked->second;
      list.current = source.recording.generation;
      auto& recording = list.generations[list.current];
      recording.uses.push_back(&native_list);
      previous_execute = tracker::next_execute;
    }
    proxy.identity = native_list.identity = identity;
    witnessed_source.recording = observation::ObserveRecording(&proxy);
    const auto unknown = observation::ObserveRecording(&foreign);
    check(witnessed_source.recording.raw == reinterpret_cast<uintptr_t>(&proxy)
            && witnessed_source.recording.reshade_base == reinterpret_cast<uintptr_t>(&native_list)
            && witnessed_source.recording.list_id == identity && witnessed_source.recording.generation == 13
            && proxy.native_queries == 1 && proxy.canonical_queries == 0 && native_list.references == 1
            && unknown.raw == reinterpret_cast<uintptr_t>(&foreign) && unknown.reshade_base == 0
            && unknown.list_id == 0 && unknown.generation == 0 && foreign.private_writes == 0,
        "recording captures only the typed base returned by the known ReShade unwrap and leaves foreign identities unknown");
    observation::RecordSrCopyback(generation, witnessed_source);
    auto* queue = reinterpret_cast<ID3D12CommandQueue*>(0x7878);
    ID3D12CommandList* lists[]{&foreign, &native_list};
    const auto pending_first = tracker::BeginSubmission(2, lists);
    const auto first = observation::CaptureQueueSubmissions(generation, queue, 2, lists, pending_first);
    auto wrong_generation = pending_first;
    if (!wrong_generation.empty()) ++wrong_generation.front().generation;
    const auto missing = observation::CaptureQueueSubmissions(generation, queue, 2, lists, wrong_generation);
    const auto pending_replay = tracker::BeginSubmission(1, lists + 1);
    const auto replay = observation::CaptureQueueSubmissions(generation, queue, 1, lists + 1, pending_replay);
    const auto before = read().count;
    std::thread replay_thread([&] {
      tracker::CompleteSubmission(pending_replay, &replay_fence, 9);
      observation::RecordQueueSubmissions(generation, replay,
          {.execute_entry_ns = 102, .execute_return_ns = 103, .forward_calls = 1,
              .signal_fence = reinterpret_cast<uintptr_t>(&replay_fence), .signal_value = 9, .signal_succeeded = true});
    });
    replay_thread.join();
    tracker::CompleteSubmission(pending_first, nullptr, 0);
    observation::RecordQueueSubmissions(generation, first,
        {.execute_entry_ns = 100, .execute_return_ns = 104, .forward_calls = 1, .signal_fence = 0x9000, .signal_value = 8});
    observation::RecordQueueSubmissions(generation + 1, first, {});
    {
      std::lock_guard lock(tracker::tracker_mutex);
      tracker::identity_by_pointer.erase(&native_list);
      tracker::tracked_lists.erase(identity);
      tracker::next_execute = previous_execute;
    }
    read();
    const auto& replay_event = observed.events[before];
    const auto& first_event = observed.events[before + 1];
    const auto events = control::Snapshot()["edit_reuse"]["fg_observation"]["events"];
    check(first.size() == 1 && replay.size() == 1 && missing.empty() && observed.count == before + 2
            && first_event.kind == observation::Kind::SR_QUEUE_SUBMISSION && first_event.thread_id != replay_event.thread_id
            && first_event.queue_submission.queue == reinterpret_cast<uintptr_t>(queue)
            && first_event.queue_submission.list == reinterpret_cast<uintptr_t>(&native_list)
            && first_event.queue_submission.batch_index == 1 && first_event.queue_submission.submission_index == 0
            && first_event.queue_submission.first_submission && !replay_event.queue_submission.first_submission
            && replay_event.queue_submission.submission_index == 1
            && events[before - 1]["source"]["recording"]["reshade_base"] == reinterpret_cast<uintptr_t>(&native_list)
            && events[before + 1]["submission"]["execute_return_ns"] == 104
            && events[before + 1]["submission"]["signal_succeeded"] == false
            && events[before + 1]["submission"]["signal_fence"] == 0x9000
            && events[before + 1]["submission"]["signal_value"] == 8
            && events[before]["submission"]["signal_succeeded"] == true
            && replay_fence.completion_reads == 0 && replay_fence.references == 1,
        "queue diagnostics match exact list generations and distinguish first Signal failure from a replay that records first");
  }
  {
    const auto before_count = read().count;
    const auto record_owned = [&](const renodx::addons::dlss5::SourceStageScope& scope) {
      if (scope.owner) {
        observation::Record(generation, {.kind = observation::Kind::NATIVE_FG_ENTRY});
      }
    };
    {
      const renodx::addons::dlss5::SourceStageScope outer(reinterpret_cast<void*>(0x7788), true);
      record_owned(outer);
      {
        const renodx::addons::dlss5::SourceStageScope cpp_mirror(reinterpret_cast<void*>(0x7788), true);
        record_owned(cpp_mirror);
      }
      {
        const renodx::addons::dlss5::SourceStageScope distinct_fg(reinterpret_cast<void*>(0x7799), true);
        record_owned(distinct_fg);
      }
    }
    {
      const renodx::addons::dlss5::SourceStageScope c_outer(reinterpret_cast<void*>(0x7788), true);
      record_owned(c_outer);
      const renodx::addons::dlss5::SourceStageScope c_to_cpp(reinterpret_cast<void*>(0x7788), false);
      record_owned(c_to_cpp);
    }
    check(read().count == before_count + 3,
        "source ownership records one C++ mirror chain, preserves distinct nested FG, and records only the outer C-to-C++ entry");
  }
  observation::StopEnvironment(1, 0);
  check(read().state == observation::State::CAPTURING,
      "native input environment changes cannot cancel an observed XeFG output window");
  for (uint64_t sequence = 101; sequence <= 103; ++sequence) {
    observation::ObserveSourceFrame(source.source_handle, source.source_generation, sequence);
  }
  const auto fourth_tail = observation::Record(generation, {.kind = observation::Kind::XEFG_PRESENT_ENTRY});
  observation::RecordResult(fourth_tail, S_OK, observation::NowNs(),
      {.queried = true, .api_result = -31});
  const auto fourth_count = read().count;
  observation::ObserveSourceFrame(source.source_handle, source.source_generation, 104);
  read();
  check(observed.source_frames == 4 && observed.count == fourth_count
          && observed.state == observation::State::STOPPED
          && std::string_view(observed.stop_reason) == "source_frame_limit",
      "fourth-source tail is observed and fifth real SR entry seals the window before its FG work");
  observation::SetEnabled(true);
  check(read().state == observation::State::STOPPED
          && read().request_generation == generation,
      "a sealed observation never retries or rearms while the experiment request remains true");
  observation::SetEnabled(false);
  check(read().count == fourth_count,
      "experiment Off preserves the completed observation snapshot");
  observation::SetEnabled(true);
  const auto fresh_generation = observation::CaptureGeneration(true);
  observation::RecordSrCopyback(fresh_generation, source);
  observation::RecordResult(native, 0, observation::NowNs());
  check(fresh_generation > generation && read().count == 1,
      "a fresh request cannot accept an old in-flight SDK result");
  observation::ObserveSourceFrame(source.source_handle, source.source_generation + 1, 101);
  check(read().state == observation::State::STOPPED,
      "a changed real SR generation stops the bound observation");
  observation::SetEnabled(false);
  observation::SetEnabled(true);
  observation::RecordSrCopyback(observation::CaptureGeneration(true), source);
  observation::StopSource(source.source_handle + 1);
  check(read().state == observation::State::CAPTURING,
      "an unrelated source release does not cancel the bound observation");
  observation::StopSource(source.source_handle);
  check(read().state == observation::State::STOPPED,
      "releasing the captured source stops its observation without retaining the source object");
  observation::SetEnabled(false);
  observation::SetEnabled(true);
  observation::RecordSrCopyback(observation::CaptureGeneration(true), source);
  observation::StopEnvironment(2, source.environment_context + 1);
  check(read().state == observation::State::CAPTURING,
      "an unrelated XeFG context change does not cancel the bound observation");
  observation::StopEnvironment(2, source.environment_context);
  check(read().state == observation::State::STOPPED,
      "a captured XeFG context generation change seals its observation");
  observation::SetEnabled(false);
  observation::SetEnabled(true);
  const auto full_generation = observation::CaptureGeneration(true);
  observation::RecordSrCopyback(full_generation, source);
  for (uint32_t index = 1; index < 64; ++index) {
    observation::Record(full_generation,
        {.kind = observation::Kind::XEFG_TAG_ENTRY,
            .xefg_tag = {.sdk_slot = UINT32_MAX, .sdk_context = UINTPTR_MAX,
                .context_identity = UINTPTR_MAX, .epoch = UINT64_MAX, .generation = UINT64_MAX,
                .present_id = UINT32_MAX, .type = INT32_MAX, .validity = INT32_MAX,
                .resource = {.raw = UINTPTR_MAX, .canonical = UINTPTR_MAX},
                .recording = {.raw = UINTPTR_MAX, .list_id = UINT64_MAX, .generation = UINT64_MAX},
                .base_x = UINT32_MAX, .base_y = UINT32_MAX, .width = UINT32_MAX, .height = UINT32_MAX,
                .resource_state = UINT32_MAX, .ui_mode = UINT32_MAX, .ui_composition = true}});
  }
  auto full_status = control::Snapshot();
  const auto tag_bytes = full_status.dump().size();
  check(read().count == 64
          && tag_bytes < renodx::addons::dlss5::dev_control_pipe::internal::MAX_FRAME_BYTES,
      "a full 64-event maximal Tag snapshot serializes with the actual DEV status below the unchanged pipe frame limit");
  observation::Record(full_generation, {.kind = observation::Kind::XEFG_TAG_ENTRY});
  read();
  bool sequence_valid = true;
  for (uint32_t index = 0; index < observed.count; ++index) {
    sequence_valid &= observed.events[index].sequence == index + 1
        && observed.events[index].observed_ns != 0 && observed.events[index].thread_id != 0;
  }
  check(observed.count == 64 && observed.overflow == 1 && sequence_valid
          && observed.state == observation::State::STOPPED && std::string_view(observed.stop_reason) == "event_limit",
      "fixed event capacity stops once with explicit overflow and retains the complete ordered prefix");
  const auto repeated_read = read();
  check(repeated_read.count == observed.count && repeated_read.events[63].sequence == 64,
      "status copies do not consume observation events");
  const auto fill_native = [&](uintptr_t reshade_base) {
    observation::SetEnabled(false);
    observation::SetEnabled(true);
    const auto native_generation = observation::CaptureGeneration(true);
    observation::RecordSrCopyback(native_generation, source);
    for (uint32_t index = 1; index < 64; ++index) {
      observation::Record(native_generation,
        {.kind = observation::Kind::NATIVE_FG_ENTRY,
            .native_fg = {.handle = UINTPTR_MAX, .generation = UINT64_MAX, .slot = UINT32_MAX, .c_export = true,
                .recording = {.raw = UINTPTR_MAX, .reshade_base = reshade_base, .list_id = UINT64_MAX, .generation = UINT64_MAX},
                .backbuffer = {.raw = UINTPTR_MAX, .canonical = UINTPTR_MAX},
                .hudless = {.raw = UINTPTR_MAX, .canonical = UINTPTR_MAX},
                .ui = {.raw = UINTPTR_MAX, .canonical = UINTPTR_MAX},
                .ui_alpha = {.raw = UINTPTR_MAX, .canonical = UINTPTR_MAX},
                .ui_mode = UINT32_MAX, .has_ui_mode = true, .frame_id = UINT64_MAX, .has_frame_id = true}});
    }
  };
  fill_native(UINTPTR_MAX);
  const auto maximal = control::Execute({{"id", "fg-observation-full"}, {"op", "fg_execution_observation"}, {"action", "status"}});
  const auto& maximal_events = maximal["state"]["edit_reuse"]["fg_observation"]["events"];
  constexpr auto frame_limit = renodx::addons::dlss5::dev_control_pipe::internal::MAX_FRAME_BYTES;
  const auto maximal_bytes = maximal.dump().size();
  check(read().count == 64 && !maximal_events.empty()
          && maximal_events[1]["native"]["recording"]["reshade_base"] == UINTPTR_MAX
          && maximal_bytes < frame_limit,
      "the maximum full DEV status keeps unwrapped list addresses below the pipe frame limit");
  {
    const Json padded_receipt{{"padding", std::string(frame_limit - maximal_bytes + 256, 'x')}};
    auto saved_request = std::exchange(control::last_request, Json{{"id", "fg-large-receipt"}});
    auto saved_receipt = std::exchange(control::last_receipt, padded_receipt);
    const auto queried = control::Execute({{"id", "fg-receipt-query"}, {"op", "status"}, {"query_id", "fg-large-receipt"}});
    control::last_request = std::move(saved_request);
    control::last_receipt = std::move(saved_receipt);
    const auto& trace = queried["state"]["edit_reuse"]["fg_observation"];
    check(queried["receipt"] == padded_receipt && trace["event_count"] == 64 && trace["events"].empty()
            && trace["events_in_status_without_receipt"] == true && queried.dump().size() < frame_limit
            && read().count == 64,
        "an oversized combined query preserves its cached receipt and leaves the full trace available through plain status");
    std::printf("FG execution observation status bytes: full=%zu oversized_query_reduced=%zu limit=%u\n",
        maximal_bytes, queried.dump().size(), frame_limit);
  }
  fill_native(0);
  const auto native_bytes = control::Snapshot().dump().size();
  check(read().count == 64
          && native_bytes < renodx::addons::dlss5::dev_control_pipe::internal::MAX_FRAME_BYTES,
      "a full 64-event maximal Native snapshot fits the actual DEV status within the unchanged pipe frame limit");
  std::printf("FG metadata snapshot bytes: maximal_tag=%zu maximal_native=%zu limit=%u\n",
      tag_bytes, native_bytes, renodx::addons::dlss5::dev_control_pipe::internal::MAX_FRAME_BYTES);
  observation::SetEnabled(false);
}
