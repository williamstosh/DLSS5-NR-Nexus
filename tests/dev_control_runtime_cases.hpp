/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included in the existing real-ImGui fixture. ReShade interfaces and execution
// observations are CPU substitutes, with no GPU allocation or model execution.
#define NR_NEXUS_TEST_OUTPUT_RUNTIME_ONLY
#include "adaptive_ui_cases.hpp"
#undef NR_NEXUS_TEST_OUTPUT_RUNTIME_ONLY
{
  OutputRuntimeFixture output_runtime;
  check(static_cast<bool>(output_runtime.output), "DEV diagnostics resolve their registered runtime backbuffer");
  namespace control = runtime::dev_control;
  namespace xefg = renodx::addons::dlss5::fg::xefg;
  using Json = control::Json;
  configuration_fixture::use_cache = true;
  configuration_fixture::cache.clear();
  runtime::LoadConfiguration();
  runtime::enabled = true;
  runtime::hooks_enabled = true;
  runtime::shutting_down = false;
  runtime::d3d12_present_seen = true;
  runtime::direct_load_state = runtime::DirectLoadState::Ready;
  runtime::direct_runtime_requirements.dual_models = true;
  runtime::native_fg_declaration = runtime::NativeFgDeclaration::kUnknown;
  runtime::native_fg_owns = false;
  runtime::auto_fg_fallback_active = false;
  runtime::experimental_present_chain = false;
  runtime::experimental_native_prefg_resolution = false;
  runtime::stage_source_rr = false;
  runtime::present_unusable = false;
  runtime::present_restore_configuration.global_writable = true;
  runtime::slider_pending = nullptr;
  runtime::stage_edit::Cancel();
  runtime::resolution_edit::Cancel();
  xefg::internal::stopped = false;
  xefg::internal::owns_stream = true;
  xefg::internal::requested = runtime::fg_service::Requested;
  runtime::kSetPresentBlockHudless.Store(1.f);
  runtime::last_input_width = 2560;
  runtime::last_input_height = 1440;
  runtime::last_output_width = 3840;
  runtime::last_output_height = 2160;
  for (auto& observed : runtime::stage_telemetry) {
    observed = {};
    observed.width = 3840;
    observed.height = 2160;
  }
  for (const runtime::SettingGroup& group : runtime::kSettingSections) {
    for (const auto* setting : group) setting->Persist();
  }
  auto fixture = runtime::profile_ui::Snapshot(true);
  fixture.update({{"NRHookPoint", 0u}, {"NRPasses", 2u}, {"NRPass2Stage", 2u},
      {"NRPreset", 1u}, {"NRPass2Preset", 2u}, {"NRFollowInputRes", false},
      {"NRResolutionScale", 1.f}, {"NRPass2ResolutionMode", 2u}, {"NRPass2ResolutionScale", 1.f}});
  std::string error;
  check(runtime::profile_ui::ApplySnapshot(fixture, &error), "control fixture uses verified production configuration application");
  unsigned serial = 0;
  const auto set = [&](const char* key, Json value, const Json& confirmation = nullptr) {
    Json request{{"id", "control-" + std::to_string(++serial)}, {"op", "set"}, {"key", key}, {"value", std::move(value)}};
    if (!confirmation.is_null()) request["confirm"] = confirmation;
    return control::Execute(request);
  };
  const auto confirm = [&](const char* key, Json value) {
    auto reply = set(key, value);
    if (reply["status"] == "confirmation_required") {
      reply = set(key, value, {{"identity", reply["identity"]}, {"changes", reply["changes"]}});
    }
    return reply;
  };
  auto status = control::Execute({{"id", "status-1"}, {"op", "status"}});
  check(status["ok"] && status["state"]["available"] && status["state"]["pid"] == GetCurrentProcessId(),
      "status captures configuration without Home or a device");
  check(status["state"]["process_created"].is_string() && status["identity"].is_object(),
      "status identifies the actual process lifetime and configuration");
  check(!status["state"]["stages"][1]["completion"]["current"].get<bool>(), "settings and a valid plan do not invent GPU proof");
  runtime::stage_telemetry[1].last_evaluate = {.sequence = 123, .observed_ns = runtime::SteadyNowNs(),
      .result = NVSDK_NGX_Result_Success, .api_returned = true};
  status = control::Execute({{"id", "status-api"}, {"op", "status"}});
  check(status["state"]["stages"][1]["last_evaluate"]["api_returned"]
          && !status["state"]["stages"][1]["completion"]["current"].get<bool>(),
      "successful Evaluate API observation remains separate from GPU completion");
  check(status["state"]["experiments"]["adaptive_feature_mode"] == 0
          && status["identity"]["experiments"]["adaptive_feature_mode"] == 0
          && !status["state"]["experiments"]["bridge_adaptive_experiment"].get<bool>()
          && !status["identity"]["experiments"]["bridge_adaptive_experiment"].get<bool>()
          && !status["state"]["experiments"]["edit_reuse_adaptive_experiment"].get<bool>()
          && !status["identity"]["experiments"]["edit_reuse_adaptive_experiment"].get<bool>()
          && !status["state"]["experiments"]["edit_reuse_fg_experiment"].get<bool>()
          && !status["identity"]["experiments"]["edit_reuse_fg_experiment"].get<bool>()
          && status["identity"]["experiments"]["edit_reuse_adaptive_evidence"] == "requested_enabled_not_activation"
          && status["state"]["experiments"]["edit_reuse_adaptive_evidence"] == "requested_enabled_not_activation"
          && status["state"]["experiments"]["bridge_adaptive_evidence"] == "requested_enabled_not_activation"
          && status["state"]["experiments"]["evidence"] == "requested_mode_not_activation"
          && status["state"]["adaptive_caches"].is_array(),
      "the experiment defaults to OFF and reports its request separately from execution");
  check(status["state"]["edit_reuse"]["xefg_off_sources"].is_array()
          && status["state"]["edit_reuse"]["xefg_off_sources"].empty(),
      "status reports an empty XeFG Off source list when this CPU fixture has no recorded D3D12 sources");
  check(status["state"]["edit_reuse"]["xefg_device_observation"].is_null(),
      "the disabled experiment leaves independent SDK device diagnostics null");
  check(status["state"]["edit_reuse"]["xefg_off_events"].is_null(),
      "the disabled experiment leaves bounded Off event diagnostics null");
  check(status["state"]["edit_reuse"]["sources"].is_array()
          && status["state"]["edit_reuse"]["sources"].empty(),
      "an empty runtime exposes no invented per-source edit cache");
  check(!status["state"]["edit_reuse"].contains("native_evaluate"),
      "the disabled FG experiment leaves the existing status envelope free of gateway diagnostics");
  {
    struct RestoreSdkContexts {
      decltype(xefg::internal::state.contexts) contexts;
      bool experiment = runtime::edit_reuse_adaptive_experiment.exchange(true);
      bool stopped = xefg::internal::stopped.exchange(false);
      bool sdk_enabled = xefg::internal::sdk_detours_enabled.exchange(true);
      RestoreSdkContexts() {
        std::unique_lock lock(xefg::internal::state.mutex);
        contexts.swap(xefg::internal::state.contexts);
      }
      ~RestoreSdkContexts() {
        {
          std::unique_lock lock(xefg::internal::state.mutex);
          contexts.swap(xefg::internal::state.contexts);
        }
        runtime::edit_reuse_adaptive_experiment = experiment;
        xefg::internal::stopped = stopped;
        xefg::internal::sdk_detours_enabled = sdk_enabled;
      }
    } restore;
    constexpr uintptr_t SDK_HANDLE = 0xD3C100;
    constexpr uintptr_t SDK_PROXY = 0xD3C200;
    constexpr uintptr_t DEVICE_IDENTITY = 0xD3C300;
    auto context = std::make_shared<xefg::internal::Context>();
    context->proxy_identity = SDK_PROXY;
    context->epoch = 9;
    context->bridge_init_kind = "InitDesc";
    context->bridge_context_ready = context->bridge_proxy_ready = true;
    context->bridge_proxy_observation = {.object_present = true, .swapchain_result = S_OK,
        .device11_result = S_OK, .device12_result = S_OK, .returned_device_identity = DEVICE_IDENTITY,
        .device11_interface_result = E_NOINTERFACE, .device12_interface_result = S_OK,
        .device12_interface_identity = DEVICE_IDENTITY};
    context->bridge_proxy_observation.returned_device_request = 11;
    {
      std::unique_lock lock(xefg::internal::state.mutex);
      xefg::internal::state.contexts.emplace(reinterpret_cast<void*>(SDK_HANDLE), context);
    }
    check(runtime::create_contracts.empty(), "SDK device diagnostics are tested without any recorded source contract");
    const auto observed = control::Snapshot();
    const auto& device = observed["edit_reuse"]["xefg_device_observation"];
    check(observed["edit_reuse"]["xefg_off_sources"].empty() && device.is_object() && device.size() == 10
            && device["sdk_context"] == SDK_HANDLE && device["sdk_proxy"] == SDK_PROXY
            && device["context_identity"] == reinterpret_cast<uintptr_t>(context.get())
            && device["context_epoch"] == 9 && device["init_kind"] == "InitDesc"
            && device["query_reason"] == "disable_unobserved",
        "a unique SDK context exposes its device metadata independently of missing SR source records");
    const auto& proxy_device = device["proxy_device_observation"];
    check(proxy_device["object_present"] && proxy_device["device11_result"] == S_OK
            && proxy_device["device12_result"] == S_OK && proxy_device["returned_device_request"] == 11
            && proxy_device["returned_device_identity"] == DEVICE_IDENTITY
            && proxy_device["device11_interface_result"] == E_NOINTERFACE
            && proxy_device["device12_interface_result"] == S_OK && proxy_device["device11_interface_identity"] == 0
            && proxy_device["device12_interface_identity"] == DEVICE_IDENTITY,
        "source-independent status preserves the queried getter, returned-device identity and interface capabilities");
    check(!device.contains("available") && !device.contains("uses_source_link") && !device.contains("source")
            && !device.contains("source_swapchain") && !xefg::QueryBridgeOff({}).available
            && context->bridge_source_swapchain == 0 && context->bridge_link_generation == 0
            && runtime::create_contracts.empty(),
        "context metadata exports neither source authority nor an Off receipt and cannot create a source link");
    {
      std::unique_lock lock(xefg::internal::state.mutex);
      for (uint32_t index = 0; index < 32; ++index) {
        context->bridge_source_swapchain = SDK_PROXY + 1;
        context->bridge_source_generation = 2;
        context->bridge_link_source = {.handle = 0xD3C400, .generation = 3, .device12 = DEVICE_IDENTITY};
        xefg::internal::RevokeBridgeSourceLinkLocked(context.get(), "source_swapchain_removed");
      }
    }
    const auto traced = control::Snapshot();
    const auto& trace = traced["edit_reuse"]["xefg_off_events"];
    const auto& event = trace["events"].back();
    check(trace["events"].size() == 32 && trace["last_sequence"] == event["sequence"]
            && event["kind"] == "link_revoked" && event["observed_ns"].get<int64_t>() > 0
            && event["thread_id"] == GetCurrentThreadId()
            && event["context_identity"] == reinterpret_cast<uintptr_t>(context.get())
            && event["source_swapchain"] == SDK_PROXY + 1 && event["source_swapchain_generation"] == 2
            && event["source"]["handle"] == 0xD3C400 && event["source"]["generation"] == 3
            && event["source"]["device12"] == DEVICE_IDENTITY
            && event["reason"] == "source_swapchain_removed" && !event["off_observed"].get<bool>()
            && event["source_entry"]["method"] == "unobserved"
            && event["source_entry"]["return_address"] == 0,
        "bounded Off diagnostics retain the revoked identity, event ordering and explicit missing Present origin");
    check(!xefg::QueryBridgeOff({}).available && context->bridge_source_swapchain == 0
            && control::Snapshot()["edit_reuse"]["xefg_off_events"] == trace,
        "reading Off events neither consumes them nor grants a receipt or restores the source link");
    check(control::Execute({{"id", "off-events-bound"}, {"op", "status"}}).dump().size() < 64 * 1024,
        "a full bounded Off event window fits the unchanged control-pipe frame limit with the real status envelope");
    runtime::edit_reuse_adaptive_experiment = false;
    check(control::Snapshot()["edit_reuse"]["xefg_device_observation"].is_null(),
        "disabling the experiment hides device diagnostics even while a captured SDK context remains");
    check(control::Snapshot()["edit_reuse"]["xefg_off_events"].is_null(),
        "disabling the experiment hides retained Off events without clearing their sequence");
    runtime::edit_reuse_adaptive_experiment = true;
    auto second_context = std::make_shared<xefg::internal::Context>();
    {
      std::unique_lock lock(xefg::internal::state.mutex);
      xefg::internal::state.contexts.emplace(reinterpret_cast<void*>(SDK_HANDLE + 1), second_context);
    }
    const auto ambiguous = control::Snapshot()["edit_reuse"]["xefg_device_observation"];
    check(ambiguous["query_reason"] == "multiple_contexts" && ambiguous["sdk_context"] == 0
            && ambiguous["sdk_proxy"] == 0 && ambiguous["context_identity"] == 0,
        "multiple SDK contexts report ambiguity without guessing a device observation owner");
    {
      std::unique_lock lock(xefg::internal::state.mutex);
      xefg::internal::state.contexts.clear();
    }
    const auto missing = control::Snapshot()["edit_reuse"]["xefg_device_observation"];
    check(missing["query_reason"] == "context_unobserved" && missing["sdk_context"] == 0
            && missing["sdk_proxy"] == 0 && missing["context_identity"] == 0,
        "missing SDK contexts retain an explicit reason and zero identities");
  }
  namespace rate = renodx::addons::dlss5::output_framerate;
  namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
  {
    const auto* source_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xD3C044});
    auto [source, inserted] = runtime::features.try_emplace(source_handle);
    check(inserted, "the CPU sampling observation owns a distinct synthetic source");
    source->second.sr_sequence = 44;
    source->second.source_generation = 3;
    source->second.source_type_confirmed = true;
    auto& edit = source->second.slots[0].edit_reuse;
    edit = std::make_shared<renodx::addons::dlss5::edit_reuse::State>();
    edit->width = edit->output_width = 16;
    edit->height = edit->output_height = 12;
    edit->anchor_generation = 7;
    edit->source_sequence = 44;
    edit->reuse_count = 8;
    edit->valid = edit->reused = true;
    const auto edited = control::Snapshot()["edit_reuse"]["sources"];
    check(edited.size() == 1 && edited[0]["source_handle"] == reinterpret_cast<uintptr_t>(source_handle)
            && edited[0]["source_generation"] == 3 && edited[0]["source_frame"] == 44
            && edited[0]["source_type_confirmed"] && edited[0]["pass"] == 1
            && edited[0]["anchor_generation"] == 7 && edited[0]["source_sequence"] == 44
            && edited[0]["reuse_count"] == 8 && edited[0]["valid"] && edited[0]["reused"]
            && edited[0]["working_size"] == Json::array({16, 12})
            && edited[0]["output_size"] == Json::array({16, 12})
            && edited[0]["evidence"] == "recorded_edit_cache_state_not_gpu_completion",
        "per-source cache status preserves source and anchor generations without claiming GPU completion");
    edit.reset();
    check(control::Snapshot()["edit_reuse"]["sources"].empty(),
        "retiring an edit cache removes it from the current status snapshot");
    source->second.adaptive_cache = std::make_shared<runtime::AdaptiveFeatureCache>();
    source->second.adaptive_cache->count = 1;
    const auto cache_status = [&]() {
      const auto state = control::Snapshot();
      for (const auto& cache : state["adaptive_caches"]) {
        if (cache["source_handle"] == reinterpret_cast<uintptr_t>(source_handle)) return cache;
      }
      return Json(nullptr);
    };
    check(cache_status()["source_frame"] == 44 && cache_status()["sampling"].is_null() && cache_status()["trial"].is_null(),
        "an existing cache reports its source clock without inventing a missing Session");
    source->second.adaptive_session = std::make_shared<adaptive::Session>();
    auto& session = *source->second.adaptive_session;
    session.paired_sampling = true;
    session.sampling_generation = 7;
    session.trial = true;
    session.pending_pair.frame = 44;
    session.pending_pair.total_us = 8000.;
    auto cache = cache_status();
    check(cache["trial"] && cache["sampling"]["paired_sampling"] && cache["sampling"]["sampling_generation"] == 7
            && cache["sampling"]["gpu_count"] == 0 && cache["sampling"]["latest_recorded_ns"].is_null()
            && !cache["sampling"]["statistics_valid"].get<bool>() && cache["sampling"]["median_us"].is_null()
            && cache["sampling"]["mad_us"].is_null(),
        "a synthetic half pair is not a completed controller sample or a valid cost window");
    // These CPU observations test serialization, not GPU completion or actual pair production.
    const auto recorded_ns = runtime::SteadyNowNs();
    session.gpu_us = {1900., 2100., 2000., 1800., 2200.};
    for (size_t index = 0; index < session.gpu_recorded_ns.size(); ++index) {
      session.gpu_recorded_ns[index] = recorded_ns - adaptive::WARMUP_NS
          + static_cast<int64_t>(index) * adaptive::SAMPLE_INTERVAL_NS;
    }
    session.trial = false;
    session.gpu_count = session.gpu_us.size() - 1;
    cache = cache_status();
    check(!cache["trial"].get<bool>() && !cache["sampling"]["statistics_valid"].get<bool>()
            && cache["sampling"]["median_us"].is_null(),
        "an incomplete five-sample window publishes no valid median");
    session.gpu_count = session.gpu_us.size();
    cache = cache_status();
    check(cache["sampling"]["statistics_valid"] && cache["sampling"]["gpu_count"] == 5
            && cache["sampling"]["latest_recorded_ns"] == recorded_ns
            && cache["sampling"]["median_us"] == 2000. && cache["sampling"]["mad_us"] == 100.
            && cache["sampling"]["evidence"] == "controller_per_source_gpu_chain",
        "status uses the actual Session statistics and identifies the per-source GPU-chain scope");
    for (auto& timestamp : session.gpu_recorded_ns) {
      timestamp -= adaptive::SAMPLE_MAX_AGE_NS + 1;
    }
    cache = cache_status();
    check(!cache["sampling"]["statistics_valid"].get<bool>() && cache["sampling"]["median_us"].is_null()
            && cache["sampling"]["mad_us"].is_null() && !cache["sampling"]["latest_recorded_ns"].is_null(),
        "expired statistics retain their last observation time but expose no current cost");
    const auto saved_counters = std::tuple{runtime::edit_reuse_enabled.load(), runtime::edit_reuse_refreshes.load(),
        runtime::edit_reuse_reuses.load(), runtime::edit_reuse_fallbacks.load()};
    runtime::edit_reuse_enabled = true;
    runtime::edit_reuse_refreshes = 3;
    runtime::edit_reuse_reuses = 5;
    runtime::edit_reuse_fallbacks = 7;
    status = control::Snapshot();
    check(status["edit_reuse"]["requested"] && status["edit_reuse"]["refreshes"] == 3
            && status["edit_reuse"]["reuses"] == 5 && status["edit_reuse"]["fallbacks"] == 7
            && status["edit_reuse"]["evidence"] == "process_counters_not_activation_or_gpu_completion"
            && !status["experiments"]["edit_reuse_adaptive_experiment"].get<bool>()
            && !status["adaptive_caches"].empty() && !cache["slots"][0]["ready"].get<bool>(),
        "process counters and the NREditReuse request do not assert experiment activation or a ready tier");
    runtime::edit_reuse_enabled = std::get<0>(saved_counters);
    runtime::edit_reuse_refreshes = std::get<1>(saved_counters);
    runtime::edit_reuse_reuses = std::get<2>(saved_counters);
    runtime::edit_reuse_fallbacks = std::get<3>(saved_counters);
    runtime::features.erase(source);
  }
  status = control::Snapshot();
  check(!status["output_framerate"]["available"].get<bool>() && status["output_framerate"]["fps"].is_null()
          && status["output_framerate"]["fps_mad"].is_null() && status["output_framerate"]["observed_multiplier"].is_null()
          && status["output_framerate"]["identity"] == 0 && !status["output_framerate"]["fg_off"].get<bool>(),
      "an unavailable output source cannot publish valid frame rate or observed multiplier values");
  const auto sampled_ns = runtime::SteadyNowNs();
  constexpr uint64_t output_context = 0xCEF0;
  const auto sdk_output = rate::RegisterXeFGOutput(0xCEF1, output_runtime.device.get_native(), output_context,
      reinterpret_cast<uintptr_t>(&output_runtime));
  rate::ObserveXeFGOutputLink(sdk_output, reinterpret_cast<uintptr_t>(&output_runtime),
      reinterpret_cast<uintptr_t>(&output_runtime));
  rate::ObserveXeFGMetadata(sdk_output, 4, 4);
  check(runtime::MatchRuntimeOutput(&output_runtime) == sdk_output
          && rate::Read(sdk_output, sampled_ns).output == sdk_output,
      "DEV output ownership follows the runtime backbuffer and observed SDK link");
  {
    std::unique_lock lock(rate::internal::state.mutex);
    auto& stream = rate::internal::state.xefg.at(sdk_output.view).stream;
    stream.previous_ns = sampled_ns;
    stream.identity = output_context;
    stream.snapshot = {.available = true, .reason = rate::Reason::kREADY, .source = rate::Source::XEFG_STATUS,
        .source_generation = 9, .configuration_generation = 11, .window_serial = 4,
        .window_begin_ns = sampled_ns - 2'000'000'000, .window_end_ns = sampled_ns,
        .fps = 144., .fps_mad = 2., .requested_multiplier = 4, .observed_multiplier = 3.6};
  }
  constexpr uintptr_t secondary_swapchain = 0xCEF2;
  const std::array<uintptr_t, 1> secondary_buffers{0xCEF3};
  rate::RegisterPresentOutput(secondary_swapchain, output_runtime.device.get_native(), 0, secondary_buffers);
  const auto secondary_output = rate::FindPresentOutput(output_runtime.device.get_native(), secondary_buffers[0]);
  for (int tick = 0; tick <= 240; ++tick) {
    rate::ObservePresent(secondary_output, sampled_ns - 2'400'000'000LL + tick * 10'000'000LL);
  }
  check(rate::Read(secondary_output, sampled_ns).available
          && rate::Read(sampled_ns).reason == rate::Reason::kAMBIGUOUS,
      "two valid outputs require the diagnostics consumer's exact runtime owner");
  const auto output_plan = runtime::RequestedStagePlan();
  check(output_plan.valid && output_plan.pass_count != 0, "the diagnostic output fixture has an effective stage");
  runtime::PublishAdaptiveStatus({.reason = adaptive::Reason::OBSERVING,
      .generation = 7, .output_fps = 144., .gpu_ms = 2., .prepared_levels = 3,
      .preparation_cpu_ms = 5., .preparation_vram_delta_bytes = 4096, .peak_process_vram_bytes = 8192,
      .display_output_generation = sdk_output.generation, .display_source_generation = 3,
      .display_stage = static_cast<uint32_t>(output_plan.effective[0])});
  status = control::Snapshot();
  check(status["output_framerate"]["available"] && status["output_framerate"]["source"] == rate::SourceText(rate::Source::XEFG_STATUS)
          && status["output_framerate"]["source_generation"] == 9 && status["output_framerate"]["configuration_generation"] == 11
          && status["output_framerate"]["identity"] == output_context && !status["output_framerate"]["fg_off"].get<bool>()
          && status["output_framerate"]["window_serial"] == 4 && status["output_framerate"]["window_end_ns"] == sampled_ns
          && status["output_framerate"]["fps"] == 144. && status["output_framerate"]["fps_mad"] == 2.
          && status["output_framerate"]["requested_multiplier"] == 4 && status["output_framerate"]["observed_multiplier"] == 3.6,
      "status carries the synthetic SDK observation identity, complete window and separate multiplier values");
  check(status["adaptive"]["output_fps"] == 144. && status["adaptive"]["gpu_ms"] == 2.
          && status["adaptive"]["measurement_scope"] == "last_controller_observation"
          && status["adaptive"]["preparation_cpu_ms"] == 5. && status["adaptive"]["preparation_vram_delta_bytes"] == 4096
          && status["adaptive"]["peak_process_vram_bytes"] == 8192,
      "controller observations and preparation counters remain distinct from current GPU completion proof");
  {
    namespace timers = renodx::addons::dlss5::gpu_timers;
    const auto group = output_plan.At(output_plan.effective[0]);
    const auto stage = static_cast<size_t>(group.stage);
    const runtime::AdaptiveCompletionKey selected{sdk_output.generation, 3};
    const runtime::AdaptiveCompletionKey other_output{secondary_output.generation, 3};
    const runtime::AdaptiveCompletionKey other_source{sdk_output.generation, 4};
    renodx::addons::dlss5::RuntimeLock lock(runtime::runtime_mutex);
    const auto saved_telemetry = runtime::stage_telemetry;
    const auto saved_epochs = runtime::comparison_stable_epochs;
    const auto saved_stable_after = runtime::comparison_stable_after_ns;
    const auto saved_sequence = timers::internal::next_sequence;
    timers::Stats proof{.valid = true, .state = timers::SampleState::VALID,
        .sequence = ++timers::internal::next_sequence, .measured_ns = runtime::SteadyNowNs()};
    proof.recorded_ns = proof.measured_ns;
    const std::array<const char*, 3> paths{"preSR", "after", "present"};
    std::strcpy(proof.path, paths[stage]);
    runtime::comparison_ui::CaptureSettings(&proof.identity);
    proof.identity.device_session = timers::device_session;
    proof.identity.stable_epoch = runtime::comparison_stable_epochs[stage];
    proof.identity.display_output_generation = sdk_output.generation;
    proof.identity.source_generation = 3;
    runtime::stage_telemetry[stage].last_evaluate = {.source_generation = 3};
    for (uint32_t pass = 0; pass < group.count; ++pass) {
      proof.passes[pass] = {.id = group.begin + pass, .state = timers::SampleState::VALID};
    }
    const auto current = [&] {
      return runtime::CurrentCompletedGroup(proof, group, runtime::SteadyNowNs(), nullptr, &selected);
    };
    const auto evaluate = [&](const runtime::AdaptiveCompletionKey& key, NVSDK_NGX_Result result) {
      const auto& [output, source] = key;
      auto session = std::make_shared<adaptive::Session>();
      session->status.display_output_generation = output;
      session->status.display_source_generation = source;
      session->status.display_stage = static_cast<uint32_t>(stage);
      const adaptive::Frame frame{.session = session};
      const runtime::AdaptiveFrameScope scope(&frame);
      runtime::FeatureState feature;
      feature.source_generation = source;
      runtime::ObserveNrEvaluation(group.stage, nullptr, feature, {}, group.begin, result, output_plan);
    };
    check(current(), "a completion receipt matches the selected output, source and Stage");
    const auto checked_ns = runtime::SteadyNowNs();
    check(runtime::CurrentGpuSample(proof, checked_ns, &proof.identity), "the matching receipt is also current for GPU cost");
    using ComparisonIdentity = renodx::addons::dlss5::gpu_comparison::Identity;
    for (const auto field : {&ComparisonIdentity::fg, &ComparisonIdentity::route, &ComparisonIdentity::settings_hash,
             &ComparisonIdentity::resolution_hash, &ComparisonIdentity::config_epoch}) {
      auto comparison_identity = proof.identity;
      ++(comparison_identity.*field);
      check(!runtime::CurrentGpuSample(proof, checked_ns, &comparison_identity)
                && runtime::CurrentCompletedGroup(proof, group, checked_ns, &comparison_identity, &selected),
            "a changed comparison identity invalidates GPU cost while the selected scope keeps its completed dimensions");
    }
    ++proof.identity.configuration_generation;
    check(!current(), "a different recreation generation invalidates the selected scope's completed dimensions");
    --proof.identity.configuration_generation;
    proof.passes[0].state = timers::SampleState::PENDING;
    check(!current(), "the selected scope needs completion proof for every Pass in its Stage");
    proof.passes[0].state = timers::SampleState::VALID;
    evaluate(other_output, NVSDK_NGX_Result_FAIL_UnableToInitializeFeature);
    evaluate(other_source, NVSDK_NGX_Result_FAIL_UnableToInitializeFeature);
    check(current(), "a later failed Evaluate in another output or source preserves the selected receipt");
    evaluate(selected, NVSDK_NGX_Result_FAIL_UnableToInitializeFeature);
    evaluate(other_source, NVSDK_NGX_Result_Success);
    check(!current(), "a later successful Evaluate from another source cannot restore a receipt invalidated by its own failure");
    proof.sequence = ++timers::internal::next_sequence;
    proof.recorded_ns = proof.measured_ns = runtime::SteadyNowNs();
    check(current(), "a fresh completion sequence restores proof for the selected scope");
    for (const auto& key : {selected, other_output, other_source}) {
      runtime::adaptive_completion_scopes[stage].erase(key);
    }
    runtime::stage_telemetry = saved_telemetry;
    runtime::comparison_stable_epochs = saved_epochs;
    runtime::comparison_stable_after_ns = saved_stable_after;
    timers::internal::next_sequence = saved_sequence;
  }
  {
    std::unique_lock lock(rate::internal::state.mutex);
    auto& snapshot = rate::internal::state.xefg.at(sdk_output.view).stream.snapshot;
    snapshot.fg_off = true;
    snapshot.requested_multiplier = 0;
    snapshot.observed_multiplier = 1.;
  }
  status = control::Snapshot();
  check(status["output_framerate"]["identity"] == output_context && status["output_framerate"]["fg_off"]
          && status["output_framerate"]["observed_multiplier"] == 1.
          && status["edit_reuse"]["xefg_off_sources"].empty(),
      "an observed SDK Off flag retains its provider identity without inventing a linked source");
  {
    std::unique_lock lock(rate::internal::state.mutex);
    rate::internal::state.xefg.at(sdk_output.view).stream.previous_ns = sampled_ns - rate::Stream::STALE_NS - 1;
  }
  status = control::Snapshot();
  check(!status["output_framerate"]["available"].get<bool>() && status["output_framerate"]["fps"].is_null()
          && status["output_framerate"]["fps_mad"].is_null() && status["adaptive"]["output_fps"].is_null()
          && status["output_framerate"]["identity"] == output_context && !status["output_framerate"]["fg_off"].get<bool>(),
      "stale SDK observations keep their source metadata but do not expose a valid current FPS");
  rate::DestroyPresentOutput(secondary_swapchain);
  rate::ReleaseXeFGOutput(sdk_output);
  check(set("EnableHooks", 0)["reason"] == "invalid_setting", "startup hook policy stays outside the control whitelist");
  check(set("NREditReuse", 1)["reason"] == "invalid_value", "x2 requires a JSON boolean");
  {
    const bool previous = runtime::edit_reuse_enabled.load();
    const auto reset_epoch = runtime::history_reset_epoch.load();
    const auto restore_epoch = runtime::present_restore_configuration.epoch;
    const auto toggle = set("NREditReuse", !previous);
    check(toggle["ok"] && toggle["changes"].size() == 1
            && toggle["state"]["settings"]["NREditReuse"] == !previous
            && runtime::edit_reuse_enabled.load() == !previous
            && runtime::history_reset_epoch.load() == reset_epoch
            && runtime::present_restore_configuration.epoch == restore_epoch,
        "the live x2 toggle commits only the existing request");
    check(set("NREditReuse", previous)["ok"] && runtime::edit_reuse_enabled.load() == previous
            && runtime::history_reset_epoch.load() == reset_epoch
            && runtime::present_restore_configuration.epoch == restore_epoch,
        "the same operation restores the initial x2 request and preserves both epochs");
  }
  check(set("NRHookPoint", 3)["reason"] == "invalid_value", "invalid hook is rejected before mutation");
  check(set("NeuralUplift", 1)["reason"] == "invalid_value", "booleans require JSON booleans");
  check(set("NRPasses", 0)["reason"] == "invalid_value", "zero pass count is rejected");
  check(control::Execute({{"id", "bad\nlog"}, {"op", "status"}})["reason"] == "invalid_request_id",
      "request ids cannot inject log lines");
  check(control::Execute({{"id", "unknown-op"}, {"op", "reset"}})["reason"] == "invalid_operation",
      "the development interface has no generic arbitrary command dispatch");
  {
    const auto* source = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xD3C090});
    auto [entry, inserted] = runtime::features.try_emplace(source);
    check(inserted, "the reset fixture owns a distinct synthetic source without creating a device");
    auto& feature = entry->second;
    feature.slots[0].handle = reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t{0xD3C091});
    feature.slots[0].failed = true;
    feature.slots[0].fail_count = 3;
    feature.render_session = std::make_shared<runtime::FeatureState>();
    feature.render_session->slots[0].handle = reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t{0xD3C092});
    runtime::codec_pipeline.preparation_failed = true;
    runtime::direct_runtime_failed = true;
    runtime::processed_outputs[nullptr] = {source};
    runtime::processed_render_outputs[nullptr] = {source};
    runtime::streamline_viewports.try_emplace(90);
    const auto before = runtime::configuration_generation.load();
    const auto epoch = runtime::history_reset_epoch.load();
    const auto resets = runtime::history_reset_counts[static_cast<size_t>(runtime::HistoryResetSource::kRecreate)].load();
    const auto retired = runtime::retired_nr_features.size();
    const bool retirement_pending = runtime::nr_retirement_pending.load();
    const auto writes = configuration_writes;
    const auto settings = control::Snapshot()["settings"];
    const Json request{{"id", "reset-on"}, {"op", "reset_nr"}};
    const auto accepted = control::Execute(request);
    check(accepted["ok"] && accepted["status"] == "accepted" && accepted["reason"] == "nr_reset_requested"
            && accepted["evidence"] == "reset_requested_not_model_or_gpu_completion"
            && accepted["configuration_generation_before"] == before
            && accepted["configuration_generation_after"] == before + 1,
        "the dedicated reset acknowledges one production recreation request without claiming model or GPU completion");
    check(runtime::configuration_generation == before + 1 && runtime::history_reset_epoch == epoch + 1
            && runtime::history_reset_counts[static_cast<size_t>(runtime::HistoryResetSource::kRecreate)] == resets + 1
            && !runtime::codec_pipeline.preparation_failed && !runtime::direct_runtime_failed
            && !feature.slots[0].failed && feature.slots[0].fail_count == 0,
        "the real RecreateFeatures advances history once and clears the codec, runtime and slot failure latches");
    check(feature.slots[0].handle == nullptr && feature.render_session == nullptr
            && runtime::retired_nr_features.size() == retired + 2
            && runtime::retired_nr_features[retired].handle == reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t{0xD3C092})
            && runtime::retired_nr_features[retired + 1].handle == reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t{0xD3C091})
            && runtime::processed_outputs.empty() && runtime::processed_render_outputs.empty()
            && runtime::streamline_viewports.empty(),
        "the real reset retires both render and main slots and clears the processed-output history");
    check(configuration_writes == writes && accepted["state"]["settings"] == settings
            && accepted["state"]["configuration_generation"] == before + 1,
        "reset preserves every setting and does not write the configuration cache");
    check(control::Execute(request) == accepted && runtime::configuration_generation == before + 1
            && runtime::history_reset_epoch == epoch + 1 && runtime::retired_nr_features.size() == retired + 2,
        "same-id reset replay returns its original receipt without another real recreation");
    const auto queried = control::Execute({{"id", "reset-query"}, {"op", "status"}, {"query_id", "reset-on"}});
    check(queried["receipt"] == accepted, "a lost reset receipt remains queryable by its original id");
    auto invalid = request;
    invalid["value"] = true;
    check(control::Execute(invalid)["reason"] == "request_id_reused", "a reset id cannot be reused with different content");
    for (const char* field : {"key", "value", "confirm", "query_id", "action", "unexpected"}) {
      invalid = {{"id", "reset-extra-" + std::string(field)}, {"op", "reset_nr"}, {field, true}};
      const auto rejected = control::Execute(invalid);
      check(!rejected["ok"].get<bool>() && rejected["reason"] == "invalid_reset_request"
              && rejected.contains("configuration_generation_before") && rejected.contains("configuration_generation_after")
              && rejected["configuration_generation_before"] == before + 1
              && rejected["configuration_generation_after"] == before + 1,
          "reset rejects all additional request fields and reports the unchanged generation");
    }
    runtime::slider_pending = &runtime::kSetAdaptiveTargetFps;
    const auto busy = control::Execute({{"id", "reset-busy"}, {"op", "reset_nr"}});
    check(!busy["ok"].get<bool>() && busy["reason"] == "configuration_busy"
            && busy.contains("configuration_generation_before") && busy.contains("configuration_generation_after")
            && busy["configuration_generation_before"] == before + 1
            && busy["configuration_generation_after"] == before + 1,
        "an existing UI configuration transaction retains ownership over reset");
    runtime::slider_pending = nullptr;
    native_window_fixture::foreground_process = 0;
    const auto unfocused = control::Execute({{"id", "reset-unfocused"}, {"op", "reset_nr"}});
    check(!unfocused["ok"].get<bool>() && unfocused["reason"] == "not_foreground"
            && unfocused.contains("configuration_generation_before") && unfocused.contains("configuration_generation_after")
            && unfocused["configuration_generation_before"] == before + 1
            && unfocused["configuration_generation_after"] == before + 1,
        "reset rejects a missing or foreign foreground process without moving the real foreground");
    native_window_fixture::foreground_process = GetCurrentProcessId();
    check(control::Execute({{"id", 42}, {"op", "reset_nr"}})["reason"] == "invalid_request_id"
            && runtime::configuration_generation == before + 1 && runtime::history_reset_epoch == epoch + 1
            && runtime::retired_nr_features.size() == retired + 2 && configuration_writes == writes,
        "rejected reset requests cannot advance history, retire slots or write configuration");
    runtime::enabled = false;
    const auto off = control::Execute({{"id", "reset-off"}, {"op", "reset_nr"}});
    check(off["ok"] && off["status"] == "accepted" && !runtime::enabled
            && !off["state"]["settings"]["NeuralUplift"].get<bool>()
            && runtime::configuration_generation == before + 2 && runtime::history_reset_epoch == epoch + 2
            && runtime::history_reset_counts[static_cast<size_t>(runtime::HistoryResetSource::kRecreate)] == resets + 2
            && configuration_writes == writes,
        "reset while NR is off still uses the UI recreation contract once without enabling NR");
    runtime::enabled = true;
    runtime::features.erase(source);
    runtime::retired_nr_features.resize(retired);
    runtime::nr_retirement_pending = retirement_pending;
  }
  const auto unchanged_generation = runtime::configuration_generation.load();
  check(set("NRHookPoint", 0u)["status"] == "unchanged" && runtime::configuration_generation == unchanged_generation,
      "repeating the current stage is an acknowledged no-op without recreation");
  auto reply = set("NRHookPoint", 1u);
  check(reply["ok"] && runtime::hook_point == 1 && runtime::configuration_generation == unchanged_generation + 1,
      "Render commits and recreates through the production stage transaction");
  check(set("NRHookPoint", 0u)["ok"] && runtime::hook_point == 0, "stage returns to Upscaled with Home closed");
  const auto before_preview = configuration_writes;
  reply = set("NRHookPoint", 2u);
  check(reply["status"] == "confirmation_required" && runtime::hook_point == 0
          && configuration_writes == before_preview && !runtime::stage_edit::pending,
      "collateral stage changes return a concrete proposal without retaining a UI modal or writing configuration");
  check(reply["changes"].contains("NRPass2Stage"), "Present proposal includes the repaired second pass");
  const Json stale{{"identity", reply["identity"]}, {"changes", reply["changes"]}};
  check(set("NRAdaptiveTargetFPS", 108.f)["ok"], "adaptive target uses the registered field rule and setter");
  check(set("NRHookPoint", 2u, stale)["reason"] == "configuration_changed" && runtime::hook_point == 0,
      "a stale confirmation cannot overwrite an intervening configuration edit");
  reply = confirm("NRHookPoint", 2u);
  check(reply["ok"] && runtime::hook_point == 2 && runtime::pass_stage[0] == 3
          && reply["state"]["plan"]["valid"], "confirmed Present commits the complete production proposal");
  check(set("NRPasses", 1u)["ok"] && runtime::stack_passes == 1, "pass retirement uses the production recreation path");
  check(set("NRPass2Stage", 2u)["reason"] == "inactive_pass", "inactive pass edits cannot be silently skipped by preset scope");
  check(confirm("NRHookPoint", 0u)["ok"], "single-pass root returns to Upscaled");
  check(confirm("NRPasses", 2u)["ok"] && runtime::stack_passes == 2 && runtime::RequestedStagePlan().valid,
      "multi-pass can be restored after a single-pass stage change");
  check(set("NRAdaptiveEnabled", true)["ok"] && runtime::nr_adaptive_enabled,
      "adaptive preference is committed independently of controller readiness");
  check(set("NRAdaptiveMinScale", .67f)["ok"], "adaptive minimum uses production validation");
  check(set("NRAdaptiveLevelCount", 15u)["ok"] && runtime::nr_adaptive_selected_levels == 15u,
      "selected level count uses the registered setting without activating an experimental mode");
  check(!set("NRAdaptiveLevelCount", 16u)["ok"].get<bool>(), "unsupported level count is rejected");
  const auto saved_release_mode = runtime::disable_release_mode.load();
  const auto saved_release_proof = runtime::disabled_resources_released.load();
  const auto before_release_writes = configuration_writes;
  runtime::disabled_resources_released = false;
  reply = set("NRDisableRelease", 1u);
  const bool release_policy_applied = reply["ok"] && runtime::disable_release_mode == 1u
      && reply["state"]["settings"]["NRDisableRelease"] == 1u;
  if (!release_policy_applied) std::fprintf(stderr, "DEV_RELEASE_REPLY %s\n", reply.dump().c_str());
  check(release_policy_applied,
      "DEV can request the existing OFF release policy through production configuration");
  check(configuration_writes > before_release_writes,
      "OFF release policy is saved through the production configuration transaction");
  check(!runtime::disabled_resources_released.load(), "release policy acceptance does not manufacture completion proof");
  check(set("NRDisableRelease", 2u)["reason"] == "invalid_value" && runtime::disable_release_mode == 1u,
      "OFF release policy keeps the registered value range");
  check(set("NRDisableRelease", saved_release_mode)["ok"] && runtime::disable_release_mode == saved_release_mode,
      "DEV can restore the original OFF resource retention policy");
  const auto workset_policy = runtime::workset_free_policy.load();
  check(set("NRWorksetFreePolicy", 0u)["reason"] == "invalid_setting"
          && runtime::workset_free_policy == workset_policy,
      "allowing the OFF release policy does not expose other compatibility settings");
  runtime::disabled_resources_released = saved_release_proof;
  const auto before_preparation_mode = control::Identity();
  const auto mode_configuration_generation = runtime::configuration_generation.load();
  reply = set("NRAdaptiveLimitedResidency", true);
  check(reply["ok"] && runtime::nr_adaptive_limited_residency.load()
          && reply["state"]["settings"]["NRAdaptiveLimitedResidency"] == true
          && runtime::nr_adaptive_selected_levels == 15u
          && control::Identity()["config_epoch"] != before_preparation_mode["config_epoch"]
          && control::Identity()["settings_hash"] != before_preparation_mode["settings_hash"]
          && runtime::configuration_generation == mode_configuration_generation,
      "preparation mode uses the registered transaction and identity without recreating compatible features or reducing levels");
  check(!set("NRAdaptiveLimitedResidency", 2u)["ok"].get<bool>(), "preparation mode rejects a non-boolean interface value");
  check(set("NRAdaptiveLimitedResidency", false)["ok"] && !runtime::nr_adaptive_limited_residency.load(),
      "the live interface can restore full preparation through the same committed setting");
  check(set("NRControlMaskEnabled", true)["ok"], "region mask activation uses the production setting");
  check(set("NRPass2ControlMaskFollowPass1", false)["ok"], "a pass can select its existing independent mask settings");
  reply = set("NRPass2ControlMaskBlue", .25f);
  check(reply["ok"] && reply["state"]["resolved_masks"][1]["rgb"][2] == .25f
          && reply["state"]["resolved_masks"][1]["evidence"] == "resolved_configuration_not_execution",
      "resolved per-pass mask values retain their configuration-only evidence label");
  check(set("NRAdaptiveEnabled", false)["ok"], "adaptive disable preserves the saved manual limit");
  check(confirm("NRHookPoint", 0u)["ok"], "the fixture returns to Upscaled before failure injection");
  configuration_fixture::dropped_key = "NRHookPoint";
  reply = set("NRHookPoint", 1u);
  check(!reply["ok"].get<bool>() && !reply["message"].get<std::string>().empty() && runtime::hook_point == 0,
      "cache-write rejection preserves the real transaction error and previous runtime values");
  const auto rejected_id = reply["id"];
  status = control::Execute({{"id", "query-rejection"}, {"op", "status"}, {"query_id", rejected_id}});
  check(status["receipt"] == reply, "a lost mutation response can be queried by its original request id");
  configuration_fixture::dropped_key.clear();
  const Json unique{{"id", "idempotent"}, {"op", "set"}, {"key", "NRHookPoint"}, {"value", 1u}};
  const auto first = control::Execute(unique);
  const auto generation = runtime::configuration_generation.load();
  check(first["ok"] && control::Execute(unique) == first && runtime::configuration_generation == generation,
      "same-id replay returns its cached receipt without repeating recreation");
  auto conflicting = unique;
  conflicting["value"] = 0u;
  check(control::Execute(conflicting)["reason"] == "request_id_reused", "reusing an id with different content is rejected");
  runtime::slider_pending = &runtime::kSetAdaptiveTargetFps;
  check(set("NRAdaptiveTargetFPS", 100.f)["reason"] == "configuration_busy", "an active UI edit retains ownership of its transaction");
  runtime::slider_pending = nullptr;
  const auto before_off = runtime::configuration_generation.load();
  reply = set("NeuralUplift", false);
  check(reply["ok"] && !runtime::enabled && !reply["state"]["settings"]["NeuralUplift"].get<bool>(),
      "OFF cleanup commits through verified production configuration and returns the actual current value");
  check(runtime::configuration_generation == before_off, "OFF does not create features to claim success");
  check(set("NeuralUplift", true)["ok"] && runtime::enabled && runtime::configuration_generation == before_off + 1,
      "NR re-enable retains production failure-latch reset and recreation");
  check(set("NeuralUplift", false)["ok"], "the offline fixture finishes with NR disabled");
  const auto experiment = [&](Json mode) {
    return control::Execute({{"id", "experiment-" + std::to_string(++serial)},
        {"op", "adaptive_feature_experiment"}, {"mode", std::move(mode)}});
  };
  const auto experiment_writes = configuration_writes;
  const auto experiment_generation = runtime::configuration_generation.load();
  const auto experiment_features = runtime::nr_features_created;
  check(experiment(true)["reason"] == "invalid_mode" && experiment(2.0)["reason"] == "invalid_mode"
          && experiment(-1)["reason"] == "invalid_mode" && experiment(4)["reason"] == "invalid_mode",
      "experimental mode accepts only the integer values zero through three");
  const bool released_before = runtime::disabled_resources_released.load();
  runtime::disabled_resources_released = false;
  runtime::enabled = true;
  runtime::slider_pending = &runtime::kSetAdaptiveTargetFps;
  check(experiment(2)["reason"] == "configuration_busy", "another configuration edit owns the experiment admission boundary");
  runtime::slider_pending = nullptr;
  check(runtime::adaptive_feature_mode.load() == runtime::AdaptiveFeatureMode::OFF,
      "rejected experiment requests leave the mode disabled");
  const Json experiment_request{{"id", "experiment-idempotent"}, {"op", "adaptive_feature_experiment"}, {"mode", 2}};
  const auto experimental = control::Execute(experiment_request);
  check(experimental["ok"] && experimental["state"]["available"]
          && experimental["state"]["experiments"]["adaptive_feature_mode"] == 2
          && experimental["state"]["settings"][runtime::kSetAdaptiveResourceStrategy.key] == 2
          && runtime::adaptive_feature_mode.load() == runtime::AdaptiveFeatureMode::UPPER_CAPACITY,
      "the legacy experiment command commits the registered setting while NR is running and resources remain allocated");
  check(control::Execute(experiment_request) == experimental,
      "same-id experimental replay returns its original receipt without another mutation");
  status = control::Execute({{"id", "query-experiment"}, {"op", "status"}, {"query_id", "experiment-idempotent"}});
  check(status["receipt"] == experimental, "the dedicated experiment operation uses the existing receipt lookup");
  auto reused_experiment = experiment_request;
  reused_experiment["mode"] = 1;
  check(control::Execute(reused_experiment)["reason"] == "request_id_reused", "a reused experimental id cannot change its requested mode");
  check(experiment(1)["ok"] && runtime::adaptive_feature_mode.load() == runtime::AdaptiveFeatureMode::RETAIN
          && experiment(3)["ok"] && runtime::adaptive_feature_mode.load() == runtime::AdaptiveFeatureMode::UPPER_CAPACITY_ON_DEMAND
          && experiment(0)["ok"] && runtime::adaptive_feature_mode.load() == runtime::AdaptiveFeatureMode::OFF
          && experiment(0)["status"] == "unchanged",
      "the dedicated operation supports RETAIN and ON_DEMAND and returns to OFF without activation claims");
  check(configuration_writes > experiment_writes && runtime::configuration_generation == experiment_generation
          && runtime::nr_features_created == experiment_features,
      "legacy experiment requests persist the official setting without immediately recreating features");
  runtime::enabled = false;
  runtime::disabled_resources_released = released_before;
  for (const auto* operation : {"bridge_adaptive_experiment", "edit_reuse_adaptive_experiment", "edit_reuse_fg_experiment"}) {
    const bool bridge = std::string_view(operation) == "bridge_adaptive_experiment";
    const bool fg = std::string_view(operation) == "edit_reuse_fg_experiment";
    auto& requested_flag = (bridge ? runtime::bridge_adaptive_experiment
                                  : (fg ? runtime::edit_reuse_fg_experiment : runtime::edit_reuse_adaptive_experiment));
    const auto* evidence_key = (bridge ? "bridge_adaptive_evidence"
                                      : (fg ? "edit_reuse_fg_evidence" : "edit_reuse_adaptive_evidence"));
    const auto boolean_experiment = [&](Json enabled) {
      return control::Execute({{"id", std::string(operation) + "-" + std::to_string(++serial)},
          {"op", operation}, {"enabled", std::move(enabled)}});
    };
    const auto boolean_identity = control::Identity();
    const auto boolean_settings = runtime::profile_ui::Snapshot(true);
    const auto boolean_writes = configuration_writes;
    const auto boolean_adaptive = runtime::CopyAdaptiveStatus();
    const auto boolean_sources = runtime::features.size();
    const auto boolean_evaluates = runtime::SourceSuccessfulEvaluations();
    const auto boolean_counters = std::tuple{runtime::edit_reuse_enabled.load(), runtime::edit_reuse_refreshes.load(),
        runtime::edit_reuse_reuses.load(), runtime::edit_reuse_fallbacks.load()};
    const auto boolean_fg = std::tuple{runtime::native_fg_public_off.load(), runtime::native_fg_declaration.load(),
        runtime::native_fg_owns.load(), runtime::native_fg_options_generation.load()};
    check(boolean_experiment(1)["reason"] == "invalid_enabled" && boolean_experiment(0)["reason"] == "invalid_enabled"
            && boolean_experiment(1.0)["reason"] == "invalid_enabled" && boolean_experiment("true")["reason"] == "invalid_enabled"
            && boolean_experiment(nullptr)["reason"] == "invalid_enabled"
            && control::Execute({{"id", std::string(operation) + "-missing"}, {"op", operation}})["reason"] == "invalid_enabled",
        "Boolean experiments require an explicit Boolean instead of a legacy mode or setting value");
    // No GPU exists in this CPU fixture; only its release-proof input is mocked.
    runtime::disabled_resources_released = false;
    check(boolean_experiment(true)["reason"] == "resources_not_released"
            && boolean_experiment(false)["reason"] == "resources_not_released"
            && !runtime::disabled_resources_released.load(),
        "neither Boolean experiment direction accepts NR OFF without the existing release proof");
    runtime::disabled_resources_released = true;
    runtime::enabled = true;
    check(boolean_experiment(true)["reason"] == "nr_running" && boolean_experiment(false)["reason"] == "nr_running",
        "both Boolean experiment directions reject a running NR chain even with a stale release flag");
    runtime::enabled = false;
    runtime::slider_pending = &runtime::kSetAdaptiveTargetFps;
    check(boolean_experiment(true)["reason"] == "configuration_busy"
            && boolean_experiment(false)["reason"] == "configuration_busy",
        "Boolean requests respect the existing UI transaction owner in both directions");
    runtime::slider_pending = nullptr;
    {
      std::atomic_bool held{false};
      std::jthread owner([&](std::stop_token stop) {
        renodx::addons::dlss5::RuntimeLock lock(runtime::runtime_mutex);
        held.store(true, std::memory_order_release);
        while (!stop.stop_requested()) {
          Sleep(1);
        }
      });
      const auto deadline = runtime::SteadyNowNs() + 1'000'000'000LL;
      while (!held.load(std::memory_order_acquire) && runtime::SteadyNowNs() < deadline) {
        Sleep(1);
      }
      check(held.load(std::memory_order_acquire), "the CPU fixture owns the actual runtime mutex on another thread");
      check(boolean_experiment(true)["reason"] == "runtime_busy" && boolean_experiment(false)["reason"] == "runtime_busy",
          "Boolean experiment mutation never waits on another runtime owner");
    }
    check(!requested_flag.load(), "rejected Boolean requests cannot enable the experiment");
    const Json boolean_request{{"id", std::string(operation) + "-idempotent"}, {"op", operation}, {"enabled", true}};
    const auto boolean_enabled = control::Execute(boolean_request);
    auto enabled_identity = boolean_identity;
    enabled_identity["experiments"][operation] = true;
    check(boolean_enabled["ok"] && boolean_enabled["state"]["available"]
            && boolean_enabled["identity"] == enabled_identity && requested_flag.load()
            && boolean_enabled["state"]["edit_reuse"]["xefg_off_sources"].is_array()
            && boolean_enabled["state"]["edit_reuse"]["xefg_off_sources"].empty()
            && boolean_enabled["state"]["experiments"][evidence_key] == "requested_enabled_not_activation",
        "Boolean enable publishes only the independent requested flag and its confirmation identity");
    check(control::Execute(boolean_request) == boolean_enabled, "same-id Boolean enable returns the original receipt");
    if (fg) {
      const auto observed = boolean_enabled["state"]["edit_reuse"]["fg_observation"];
      check(observed["state"] == "armed" && observed["source_frames"] == 0 && observed["events"].empty()
              && observed["first_source_prefix_complete"] == false,
          "FG experiment arms bounded observation without inventing a source frame or complete prefix");
    }
    status = control::Execute({{"id", "query-" + std::string(operation)}, {"op", "status"}, {"query_id", boolean_request["id"]}});
    check(status["receipt"] == boolean_enabled, "lost Boolean enable receipts use the existing status lookup");
    auto reused_boolean = boolean_request;
    reused_boolean["enabled"] = false;
    check(control::Execute(reused_boolean)["reason"] == "request_id_reused", "reused Boolean ids cannot change the requested flag");
    if (fg) {
      check(boolean_experiment(true)["status"] == "unchanged"
              && control::Snapshot()["edit_reuse"]["fg_observation"]["request_generation"]
                  == boolean_enabled["state"]["edit_reuse"]["fg_observation"]["request_generation"],
          "unchanged FG enable preserves the observation request generation");
    }
    check(set("NRAdaptiveTargetFPS", runtime::nr_adaptive_target_fps.load(),
              {{"identity", boolean_identity}, {"changes", {{"NRAdaptiveTargetFPS", runtime::nr_adaptive_target_fps.load()}}}})["reason"]
            == "configuration_changed",
        "a pre-experiment confirmation cannot commit after the Boolean identity changes");
    runtime::enabled = true;
    check(boolean_experiment(false)["reason"] == "nr_running" && requested_flag.load(),
        "disabling an enabled Boolean experiment also requires NR OFF");
    runtime::enabled = false;
    runtime::disabled_resources_released = false;
    check(boolean_experiment(false)["reason"] == "resources_not_released" && requested_flag.load(),
        "disabling an enabled Boolean experiment never bypasses resource retirement");
    runtime::disabled_resources_released = true;
    check(boolean_experiment(false)["ok"] && !requested_flag.load()
            && boolean_experiment(true)["ok"] && requested_flag.load()
            && boolean_experiment(false)["ok"] && boolean_experiment(false)["status"] == "unchanged",
        "Boolean requested state can turn off, reopen and return to its default without changing ordinary settings");
    const auto after_boolean = runtime::CopyAdaptiveStatus();
    check(control::Identity() == boolean_identity && runtime::profile_ui::Snapshot(true) == boolean_settings
            && configuration_writes == boolean_writes && runtime::configuration_generation == experiment_generation
            && runtime::nr_features_created == experiment_features && runtime::features.size() == boolean_sources
            && runtime::SourceSuccessfulEvaluations() == boolean_evaluates
            && boolean_counters == std::tuple{runtime::edit_reuse_enabled.load(), runtime::edit_reuse_refreshes.load(),
                runtime::edit_reuse_reuses.load(), runtime::edit_reuse_fallbacks.load()}
            && runtime::disabled_resources_released.load(),
        "Boolean requests neither persist settings, evaluate/create sources/models nor change x2 counters or release proof");
    check(boolean_fg == std::tuple{runtime::native_fg_public_off.load(), runtime::native_fg_declaration.load(),
              runtime::native_fg_owns.load(), runtime::native_fg_options_generation.load()}
            && after_boolean.generation == boolean_adaptive.generation && after_boolean.reason == boolean_adaptive.reason
            && after_boolean.requested == boolean_adaptive.requested && after_boolean.applied == boolean_adaptive.applied,
        "request acceptance cannot manufacture FG Off, an eligible source or an actual applied tier");
    runtime::disabled_resources_released = released_before;
  }
#include "nr_edit_fg_observation_cases.hpp"
  {
    const Json request{{"id", "fg-observation-receipt"}, {"op", "edit_reuse_fg_experiment"}, {"enabled", false}};
    const auto receipt = control::Execute(request);
    const auto reply = control::Execute({{"id", "fg-observation-receipt-query"}, {"op", "status"},
        {"query_id", request["id"]}});
    const auto& summary = receipt["state"]["edit_reuse"]["fg_observation"];
    constexpr size_t serialized_events =
#if defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
        16;
#else
        64;
#endif
    check(summary["event_count"] == 64 && summary["events"].empty() && summary["events_in_status_only"] == true
            && reply["state"]["edit_reuse"]["fg_observation"]["events"].size() == serialized_events,
        "mutation receipts keep the trace summary while status follows the build's documented event limit");
    check(reply["receipt"] == receipt && control::Execute(request) == receipt
            && reply.dump().size() < renodx::addons::dlss5::dev_control_pipe::internal::MAX_FRAME_BYTES,
        "a full observation plus its idempotent cached receipt fits the existing pipe frame limit");
    std::printf("FG metadata status plus receipt bytes=%zu\n", reply.dump().size());
    namespace source = renodx::addons::dlss5::fg::native_options::frame_source;
    struct RestoreEvaluateObservation {
      source::EvaluateEvents evaluations;
      bool edit_reuse = runtime::edit_reuse_enabled.exchange(true);
      bool experiment = runtime::edit_reuse_fg_experiment.exchange(true);
      Json request = control::last_request;
      Json receipt = control::last_receipt;
      RestoreEvaluateObservation() {
        std::unique_lock lock(source::state.mutex);
        evaluations = source::state.metadata.evaluations;
        auto& observed = source::state.metadata.evaluations;
        for (auto& feature : observed.features) feature.enters = UINT64_MAX;
        observed.ngx_fg_calls = observed.ngx_fg_under_dlssg = UINT64_MAX;
      }
      ~RestoreEvaluateObservation() {
        std::unique_lock lock(source::state.mutex);
        source::state.metadata.evaluations = evaluations;
        runtime::edit_reuse_enabled = edit_reuse;
        runtime::edit_reuse_fg_experiment = experiment;
        control::last_request = request;
        control::last_receipt = receipt;
      }
    } restore;
    const Json max_request{{"id", "native-evaluate-summary-receipt"},
        {"op", "edit_reuse_fg_experiment"}, {"enabled", true}};
    const auto max_receipt = control::Execute(max_request);
    const auto maximum = control::Execute({{"id", "native-evaluate-summary-query"}, {"op", "status"},
        {"query_id", max_request["id"]}});
    const auto& native_evaluate = maximum["state"]["edit_reuse"]["native_evaluate"];
    check(native_evaluate["feature_order"] == Json::array({sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureDLSS_G, "other"})
            && native_evaluate["feature_calls"] == Json::array({UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX})
            && native_evaluate["ngx_fg_calls"] == UINT64_MAX && native_evaluate["ngx_fg_under_dlssg"] == UINT64_MAX
            && native_evaluate["evidence"] == "cpu_calls_only",
        "gateway status reports fixed feature buckets and CPU call counters without truncating uint64 values");
    check(!max_receipt["state"]["edit_reuse"].contains("native_evaluate")
            && maximum["receipt"] == max_receipt && control::Execute(max_request) == max_receipt
            && maximum["state"]["edit_reuse"]["fg_observation"]["events"].size() == serialized_events
            && maximum.dump().size() < renodx::addons::dlss5::dev_control_pipe::internal::MAX_FRAME_BYTES,
        "maximum gateway counters, the complete existing trace and its original receipt fit the unchanged pipe limit");
    std::printf("Native gateway summary bytes=%zu; full status plus receipt bytes=%zu\n",
        native_evaluate.dump().size(), maximum.dump().size());
    const auto saved_marker = runtime::fg_output_marker_window;
    runtime::fg_output_marker_window = {.binding = {.source = UINTPTR_MAX, .source_generation = UINT64_MAX,
        .provider = UINT64_MAX, .context = UINTPTR_MAX, .environment_generation = UINT64_MAX},
        .request_generation = UINT64_MAX, .recorded = UINT32_MAX, .refreshes = UINT32_MAX, .reuses = UINT32_MAX,
        .first_sequence = UINT64_MAX, .last_sequence = UINT64_MAX, .armed_ns = INT64_MAX};
    const auto marker_projection = control::Execute({{"id", "marker-budget"}, {"op", "fg_output_marker"}, {"action", "status"}});
    runtime::fg_output_marker_window = saved_marker;
    auto marker_combined = maximum;
    marker_combined["receipt"] = marker_projection;
    check(marker_projection.dump().size() < 2048
            && marker_combined.dump().size() < renodx::addons::dlss5::dev_control_pipe::internal::MAX_FRAME_BYTES
            && marker_combined["state"]["edit_reuse"]["fg_observation"]["events"].size() == serialized_events,
        "maximum marker scalars and the complete existing status trace fit without duplicating or truncating events");
    runtime::edit_reuse_fg_experiment = false;
    check(!control::Snapshot()["edit_reuse"].contains("native_evaluate"),
        "disabling the experiment removes the additional diagnostics without clearing retained observations");
  }
  {
    namespace screenshot = renodx::addons::dlss5::screenshot;
    const auto before_identity = control::Identity();
    const auto before_history = runtime::history_reset_epoch.load();
    const auto before_writes = configuration_writes;
    const auto before_route = runtime::hook_point.load();
    auto project = std::filesystem::current_path();
    while (project.filename() != L"RenoDX-DLSSNR-Nexus" && project.has_parent_path()
           && project.parent_path() != project) project = project.parent_path();
    check(project.filename() == L"RenoDX-DLSSNR-Nexus", "ROI control fixture resolves the authorized project");
    const auto output = project / L"tmp" / L"mafia-same-frame-roi" / L"dev-control-contract";
    const auto worker_started = screenshot::internal::worker_started.exchange(true);
    const Json request{{"id", "roi-idempotent"}, {"op", "inline_restore_roi"},
        {"output_directory", output.string()}};
    const auto armed = control::Execute(request);
    const auto request_id = screenshot::internal::inline_restore_request_id.load();
    check(armed["ok"] && armed["status"] == "armed"
            && armed["state"]["inline_restore_roi"]["status"] == "armed",
        "dedicated ROI operation arms without a device or configuration edits");
    check(control::Execute(request) == armed && screenshot::internal::inline_restore_request_id == request_id,
        "same-id ROI replay returns the original receipt without rearming");
    status = control::Execute({{"id", "roi-status"}, {"op", "status"}, {"query_id", "roi-idempotent"}});
    check(status["receipt"] == armed && status["state"]["inline_restore_roi"]["status"] == "armed",
        "ROI progress and cached receipt are observable through status");
    check(control::Execute({{"id", "roi-busy"}, {"op", "inline_restore_roi"},
              {"output_directory", output.string()}})["reason"] == "capture_busy",
        "a different ROI request cannot replace the armed capture");
    check(control::Identity() == before_identity && runtime::history_reset_epoch == before_history
            && runtime::hook_point == before_route && configuration_writes == before_writes,
        "ROI arm, status and retries write no settings, reset no history and change no route");
    screenshot::AbortPending();
    check(!screenshot::internal::inline_restore_active
            && control::Snapshot()["inline_restore_roi"]["status"] == "failed",
        "aborting an unclaimed ROI request releases its slot and records failure");
    screenshot::internal::worker_started = worker_started;
  }
  {
    // Synthetic IAT ownership with a real Win32 protection failure; no SDK or Shutdown is invoked.
    struct CallerIdentityFixture {
      void** previous_slot = runtime::get_module_filename_iat;
      void* previous_original = runtime::original_get_module_filename;
      decltype(runtime::real_get_module_filename) previous_real = runtime::real_get_module_filename;
      void* page = nullptr;
      ~CallerIdentityFixture() {
        runtime::get_module_filename_iat = previous_slot;
        runtime::original_get_module_filename = previous_original;
        runtime::real_get_module_filename = previous_real;
        if (page != nullptr) {
          VirtualFree(page, 0, MEM_RELEASE);
        }
      }
    } identity_fixture;
    SYSTEM_INFO system_info{};
    GetSystemInfo(&system_info);
    identity_fixture.page = VirtualAlloc(nullptr, system_info.dwPageSize, MEM_RESERVE, PAGE_NOACCESS);
    check(identity_fixture.page != nullptr, "reserve an uncommitted synthetic IAT page without creating a device");
    auto** synthetic_slot = static_cast<void**>(identity_fixture.page);
    auto* original = reinterpret_cast<void*>(GetModuleFileNameW);
    auto* stub = reinterpret_cast<void*>(runtime::CallerGetModuleFileNameW);
    runtime::get_module_filename_iat = synthetic_slot;
    runtime::original_get_module_filename = original;
    runtime::real_get_module_filename = GetModuleFileNameW;
    check(!runtime::RestoreCallerIdentity() && runtime::get_module_filename_iat == synthetic_slot
            && runtime::original_get_module_filename == original && runtime::real_get_module_filename == GetModuleFileNameW,
        "real VirtualProtect failure on an uncommitted page retains all three caller identity references");
    const auto kernel_module = GetModuleHandleW(L"kernel32.dll");
    check(kernel_module != nullptr && kernel_module != runtime::addon_module, "the fallback probe names a real non-addon module");
    std::array<wchar_t, MAX_PATH> expected{}, actual{};
    const auto length = GetModuleFileNameW(kernel_module, expected.data(), static_cast<DWORD>(expected.size()));
    check(length > 0 && length < expected.size()
            && runtime::CallerGetModuleFileNameW(kernel_module, actual.data(), static_cast<DWORD>(actual.size())) == length
            && std::equal(expected.begin(), expected.begin() + length, actual.begin()),
        "failed restoration leaves the real GetModuleFileNameW fallback callable");
    check(VirtualAlloc(identity_fixture.page, system_info.dwPageSize, MEM_COMMIT, PAGE_READWRITE) == identity_fixture.page,
        "commit the same synthetic IAT page before retrying restoration");
    *synthetic_slot = stub;
    DWORD previous_protection = 0;
    check(VirtualProtect(identity_fixture.page, system_info.dwPageSize, PAGE_READONLY, &previous_protection) != FALSE,
        "the committed synthetic IAT has its original read-only protection");
    check(runtime::InstallCallerIdentity() && *synthetic_slot == stub
            && runtime::original_get_module_filename == original && runtime::real_get_module_filename == GetModuleFileNameW,
        "reinstalling the tracked patch cannot wrap its own stub as the original function");
    check(runtime::RestoreCallerIdentity() && *synthetic_slot == original
            && runtime::get_module_filename_iat == nullptr && runtime::original_get_module_filename == nullptr
            && runtime::real_get_module_filename == nullptr,
        "a successful retry restores the original slot before clearing caller identity tracking");
    MEMORY_BASIC_INFORMATION memory{};
    check(VirtualQuery(identity_fixture.page, &memory, sizeof(memory)) == sizeof(memory) && memory.Protect == PAGE_READONLY,
        "successful restoration preserves the IAT page's read-only protection");
    check(runtime::RestoreCallerIdentity() && *synthetic_slot == original,
        "restoring an already restored caller identity is an idempotent success");
  }
#if defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
#include "nr_fg_output_probe_cases.hpp"
#endif
  {
    runtime::FgOutputMarkerWindow marker;
    const runtime::FgOutputMarkerWindow::Binding source{
        .source = 11, .source_generation = 2, .provider = 2, .context = 33, .environment_generation = 4};
    constexpr int64_t started = 1'000;
    check(!marker.Admit(source, true, started), "output marker is disabled in an unarmed normal DEV build");
    marker.Arm(started);
    for (uint32_t frame = 0; frame < marker.MAX_FRAMES; ++frame) {
      check(marker.Admit(source, true, started + frame), "bounded marker admits the same source and FG environment without SDK debug state");
      marker.Recorded((frame & 1) != 0, frame + 100, started + frame);
    }
    check(marker.stopped && marker.recorded == 128 && marker.refreshes == 64 && marker.reuses == 64
            && marker.first_sequence == 100 && marker.last_sequence == 227 && !marker.Admit(source, true, started + 128),
        "output marker stops at 128 source writes with distinct refresh and reuse counters");
    marker.Arm(started);
    check(marker.recorded == 0 && marker.Admit(source, true, started), "a new explicit arm request starts a fresh marker window");
    auto successor = source;
    ++successor.source_generation;
    check(!marker.Admit(successor, true, started) && marker.stopped && !marker.Admit(source, true, started),
        "source generation changes permanently stop the current window");
    marker.Arm(started);
    check(marker.Admit(source, true, started), "new request admits its first source");
    ++successor.context;
    check(!marker.Admit(successor, true, started), "a different FG context cannot inherit the source marker window");
    marker.Arm(started);
    check(marker.Admit(source, true, started) && !marker.Admit(source, false, started) && marker.stopped,
        "loss of the actual FG environment stops a bound window without SDK debug dependencies");
    marker.Arm(started);
    check(!marker.Admit(source, true, started + marker.MAX_DURATION_NS) && marker.stopped && !marker.enabled
            && std::string_view(marker.reason) == "time_limit" && marker.recorded == 0,
        "a request expires at three seconds even when it has not written its first source frame");
    marker.Arm(started);
    marker.StopSource(source.source, source.source_generation, "source_model_failed");
    check(marker.stopped && !marker.bound && !marker.Admit(source, true, started),
        "a source failure before the first write cancels the short marker request");
    marker.Arm(started);
    check(marker.Admit(source, true, started), "failure test binds the actual source");
    marker.StopSource(source.source + 1, source.source_generation, "unrelated_failure");
    check(marker.Admit(source, true, started), "a failure in another source does not stop the bound source");
    marker.StopSource(source.source, source.source_generation, "source_model_failed");
    check(marker.stopped && !marker.Admit(source, true, started),
        "a bound source failure cannot resume its marker after the model later recovers");
  }
  {
    namespace screenshot = renodx::addons::dlss5::screenshot;
    struct RestoreMarkerControl {
      runtime::FgOutputMarkerWindow marker = runtime::fg_output_marker_window;
      decltype(runtime::created_feature_ids) handles;
      Json request = control::last_request, receipt = control::last_receipt;
      bool enabled = runtime::enabled.exchange(true), reuse = runtime::edit_reuse_enabled.exchange(true);
      bool experiment = runtime::edit_reuse_fg_experiment.exchange(true);
      bool public_off = runtime::native_fg_public_off.exchange(false);
      bool xefg_sdk = xefg::internal::sdk_detours_enabled.exchange(false);
      bool xefg_owns = xefg::internal::owns_stream.exchange(false);
      uint32_t hook = runtime::hook_point.exchange(runtime::kHookUpscaled);
      runtime::NativeFgDeclaration declaration = runtime::native_fg_declaration.exchange(runtime::NativeFgDeclaration::kOn);
      RestoreMarkerControl() {
        handles.swap(runtime::created_feature_ids);
        runtime::created_feature_ids.emplace(reinterpret_cast<const NVSDK_NGX_Handle*>(0xF60011), NVSDK_NGX_Feature_FrameGeneration);
        runtime::fg_output_marker_window = {};
      }
      ~RestoreMarkerControl() {
        handles.swap(runtime::created_feature_ids);
        runtime::fg_output_marker_window = marker;
        control::last_request = request;
        control::last_receipt = receipt;
        runtime::enabled = enabled;
        runtime::edit_reuse_enabled = reuse;
        runtime::edit_reuse_fg_experiment = experiment;
        runtime::native_fg_public_off = public_off;
        xefg::internal::sdk_detours_enabled = xefg_sdk;
        xefg::internal::owns_stream = xefg_owns;
        runtime::hook_point = hook;
        runtime::native_fg_declaration = declaration;
      }
    } restore_marker_control;
    const auto writes_before_marker = configuration_writes;
    const auto process_identity = control::ProcessIdentityState();
    auto progress = control::Execute({{"id", "marker-default"}, {"op", "fg_output_marker"}, {"action", "status"}});
    check(progress["ok"] && !progress["marker"]["enabled"].get<bool>()
            && progress["state"] == process_identity && process_identity.size() == 2
            && process_identity["pid"] == GetCurrentProcessId() && process_identity["process_created"].is_string(),
        "normal DEV exposes an independent compact marker query with default OFF");
    const Json arm{{"id", "marker-arm"}, {"op", "fg_output_marker"}, {"action", "arm"}};
    const auto armed = control::Execute(arm);
    check(armed["ok"] && armed["status"] == "armed" && armed["marker"]["duration_ms"] == 3000
            && armed["marker"]["frame_limit"] == 128 && armed["state"] == process_identity,
        "the explicit marker operation arms a running native Upscaled experiment without stopping NR or touching SDK display modes");
    runtime::fg_output_marker_window.Recorded(false, 123, runtime::SteadyNowNs());
    check(control::Execute(arm) == armed && runtime::fg_output_marker_window.recorded == 1,
        "same-id marker replay returns the original receipt without restarting the finite window");
    progress = control::Execute({{"id", "marker-progress"}, {"op", "fg_output_marker"}, {"action", "status"}});
    check(progress["marker"]["recorded"] == 1 && progress["marker"]["refreshes"] == 1
            && control::last_receipt == armed && progress.dump().size() < 2048,
        "marker progress is live, compact and does not replace the cached mutation receipt");
    const auto reused_id = control::Execute({{"id", "marker-arm"}, {"op", "fg_output_marker"}, {"action", "stop"}});
    check(!reused_id["ok"].get<bool>() && reused_id["reason"] == "request_id_reused" && reused_id["state"] == process_identity,
        "a reused marker mutation id cannot change its original action");
    const auto stopped = control::Execute({{"id", "marker-stop"}, {"op", "fg_output_marker"}, {"action", "stop"}});
    check(stopped["ok"] && !runtime::fg_output_marker_window.enabled && runtime::fg_output_marker_window.stopped
            && stopped["state"] == process_identity,
        "an explicit stop cancels the marker while NR remains running");
    runtime::hook_point = runtime::kHookPresent;
    const auto blocked = control::Execute({{"id", "marker-native-present"}, {"op", "fg_output_marker"}, {"action", "arm"}});
    check(!blocked["ok"].get<bool>() && blocked["reason"] == "marker_route_unavailable"
            && !runtime::fg_output_marker_window.enabled && blocked["state"] == process_identity,
        "native Present does not gain marker admission from the diagnostic operation");
    runtime::hook_point = runtime::kHookUpscaled;
    runtime::enabled = false;
    const auto inactive = control::Execute({{"id", "marker-nr-off"}, {"op", "fg_output_marker"}, {"action", "arm"}});
    check(inactive["reason"] == "nr_x2_not_running" && inactive["state"] == process_identity,
        "an inactive NR chain cannot arm the marker");
    const auto invalid_action = control::Execute({{"id", "marker-invalid"}, {"op", "fg_output_marker"}, {"action", "invalid"}});
    check(!invalid_action["ok"].get<bool>() && invalid_action["reason"] == "invalid_marker_action"
            && invalid_action["state"] == process_identity,
        "a rejected marker action retains the client's minimal process identity contract");
    const auto consumer_status = control::Execute({{"id", "consumer-status"}, {"op", "fg_consumer_capture"}, {"action", "status"}});
    check(consumer_status["ok"] && consumer_status["capture"].is_object()
            && consumer_status["state"] == process_identity && consumer_status.dump().size() < 2048,
        "consumer progress reuses the same compact native process identity envelope");
    Json consumer{{"id", "consumer-bad-roi"}, {"op", "fg_consumer_capture"}, {"action", "arm"},
        {"output_directory", "D:/nr-consumer-cpu"}, {"capture_id", "cpu-consumer"},
        {"roi", {{"x", -1}, {"y", 0}, {"width", 128}, {"height", 64}}}};
    const auto bad_roi = control::Execute(consumer);
    check(!bad_roi["ok"].get<bool>() && bad_roi["reason"] == "invalid_consumer_request"
            && bad_roi["state"] == process_identity && !screenshot::WantsFgConsumerCapture(),
        "malformed ROI coordinates cannot arm a consumer request or start GPU work");
    runtime::enabled = true;
    runtime::hook_point = runtime::kHookPresent;
    consumer["id"] = "consumer-native-present";
    consumer["roi"]["x"] = 0;
    const auto consumer_blocked = control::Execute(consumer);
    check(!consumer_blocked["ok"].get<bool>() && consumer_blocked["reason"] == "native_consumer_route_unavailable"
            && consumer_blocked["state"] == process_identity && !screenshot::WantsFgConsumerCapture(),
        "the consumer operation does not admit native Present or claim a source frame");
    check(configuration_writes == writes_before_marker
            && !control::Snapshot()["edit_reuse"].contains("fg_output_marker")
            && !control::Snapshot()["edit_reuse"].contains("fg_consumer_capture"),
        "marker requests do not persist settings or enlarge the ordinary status body");
  }
  Json receipt{{"scope", "CPU production configuration and status adapters with synthetic ReShade cache; no GPU, NGX or game"},
      {"checks", checks}, {"configuration_writes", configuration_writes}, {"final", control::Snapshot()}};
  std::ofstream("dev-control-runtime.json") << receipt.dump(2);
  configuration_fixture::use_cache = false;
  configuration_fixture::cache.clear();
  std::printf("PASS development control production configuration cases; checks=%u\n", checks);
}
