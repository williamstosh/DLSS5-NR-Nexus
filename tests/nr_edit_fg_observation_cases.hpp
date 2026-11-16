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
  const observation::SrCopyback source{.source_handle = 0x1234, .source_generation = 71,
      .sr_sequence = 100, .output = {.raw = 0x4567, .canonical = 0x4567},
      .width = 1920, .height = 1080, .format = DXGI_FORMAT_R16G16B16A16_FLOAT,
      .recording = {.raw = 0x6789, .list_id = 31, .generation = 13},
      .environment_provider = 2, .environment_context = 0x9876, .environment_generation = 83};
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
            && tag.recording.raw == 1 && tag.recording.list_id == 0 && tag.recording.generation == 0
            && tag.resource.raw == 0 && tag.resource.canonical == 0
            && observed.events[4].api_result == -23,
        "actual XeFG Resource wrapper records region list=1/resource=0 without QI, recording invention or SDK-success inference");
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
  observation::SetEnabled(false);
  observation::SetEnabled(true);
  const auto native_generation = observation::CaptureGeneration(true);
  observation::RecordSrCopyback(native_generation, source);
  for (uint32_t index = 1; index < 64; ++index) {
    observation::Record(native_generation,
        {.kind = observation::Kind::NATIVE_FG_ENTRY,
            .native_fg = {.handle = UINTPTR_MAX, .generation = UINT64_MAX, .slot = UINT32_MAX, .c_export = true,
                .recording = {.raw = UINTPTR_MAX, .list_id = UINT64_MAX, .generation = UINT64_MAX},
                .backbuffer = {.raw = UINTPTR_MAX, .canonical = UINTPTR_MAX},
                .hudless = {.raw = UINTPTR_MAX, .canonical = UINTPTR_MAX},
                .ui = {.raw = UINTPTR_MAX, .canonical = UINTPTR_MAX},
                .ui_alpha = {.raw = UINTPTR_MAX, .canonical = UINTPTR_MAX},
                .ui_mode = UINT32_MAX, .has_ui_mode = true, .frame_id = UINT64_MAX, .has_frame_id = true}});
  }
  const auto native_bytes = control::Snapshot().dump().size();
  check(read().count == 64
          && native_bytes < renodx::addons::dlss5::dev_control_pipe::internal::MAX_FRAME_BYTES,
      "a full 64-event maximal Native snapshot fits the actual DEV status within the unchanged pipe frame limit");
  std::printf("FG metadata snapshot bytes: maximal_tag=%zu maximal_native=%zu limit=%u\n",
      tag_bytes, native_bytes, renodx::addons::dlss5::dev_control_pipe::internal::MAX_FRAME_BYTES);
  observation::SetEnabled(false);
}
