/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Composed after the production configuration and monitor adapters. All calls
// belong to the selected ReShade frame callback, including with Home closed.
namespace dev_control {
using Json = presets::Json;
inline Json last_request;
inline Json last_receipt;

inline bool Allowed(std::string_view key) {
  constexpr std::string_view base[] = {
      "NeuralUplift", "NRDisableRelease", "NRHookPoint", "NRPasses", "NRFollowInputRes", "NRResolutionScale",
      "NRAdaptiveEnabled", "NRAdaptiveTargetFPS", "NRAdaptiveMinScale", "NRAdaptiveLevelCount", "NRAdaptiveLimitedResidency",
      "NRControlMaskEnabled",
      "NRControlMaskMode", "NRControlMaskBlendMode", "NRControlMaskRed", "NRControlMaskGreen",
      "NRControlMaskBlue", "NRControlMaskDepthNear", "NRControlMaskDepthFar",
      "NRControlMaskMotionEnabled", "NRControlMaskMotionStart", "NRControlMaskMotionEnd"};
  if (std::find(std::begin(base), std::end(base), key) != std::end(base)) return true;
  if (key.size() < 8 || !key.starts_with("NRPass") || key[6] < '2' || key[6] > '4') return false;
  constexpr std::string_view suffixes[] = {
      "Stage", "ResolutionMode", "ResolutionScale", "ControlMaskFollowPass1", "ControlMaskMode",
      "ControlMaskBlendMode", "ControlMaskRed", "ControlMaskGreen", "ControlMaskBlue",
      "ControlMaskDepthNear", "ControlMaskDepthFar", "ControlMaskMotionEnabled",
      "ControlMaskMotionStart", "ControlMaskMotionEnd"};
  return std::find(std::begin(suffixes), std::end(suffixes), key.substr(7)) != std::end(suffixes);
}

inline Json Identity() {
  gpu_comparison::Identity identity;
  comparison_ui::CaptureSettings(&identity);
  return {{"configuration_generation", identity.configuration_generation}, {"config_epoch", identity.config_epoch},
      {"settings_hash", identity.settings_hash}, {"resolution_hash", identity.resolution_hash},
      {"route", identity.route}, {"fg", identity.fg},
      {"experiments", {{"adaptive_feature_mode", static_cast<uint32_t>(adaptive_feature_mode.load())},
          {"bridge_adaptive_experiment", bridge_adaptive_experiment.load()},
          {"edit_reuse_adaptive_experiment", edit_reuse_adaptive_experiment.load()},
          {"edit_reuse_fg_experiment", edit_reuse_fg_experiment.load()},
          {"edit_reuse_adaptive_evidence", "requested_enabled_not_activation"}}}};
}

inline Json ProcessIdentityState() {
  FILETIME created{}, exited{}, kernel{}, user{};
  const bool process_known = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE;
  return {{"pid", GetCurrentProcessId()},
      {"process_created", process_known ? Json(std::to_string((uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime)) : Json(nullptr)}};
}

inline Json Snapshot() {
  auto state = ProcessIdentityState();
  state.update({{"version", kAddonVersion}, {"available", false}, {"reason", "runtime_busy"}});
  const auto roi = screenshot::GetInlineRestoreStatus();
  state["inline_restore_roi"] = {{"capture_id", roi.capture_id}, {"status", roi.status},
      {"reason", roi.reason}, {"serial", roi.serial}};
  RuntimeTryLock lock(runtime_mutex);
  if (!lock.owns_lock()) return state;
  const auto identity = Identity();
  const auto now = SteadyNowNs();
  Json settings = Json::object();
  for (const auto& group : kSettingSections) {
    for (const auto* setting : group) {
      if (Allowed(setting->key)) settings[setting->key] = preset_ui::ReadSetting(setting->key);
    }
  }
  for (const auto& setting : preset_ui::kExtraSettings) {
    if (Allowed(setting.key)) settings[setting.key] = preset_ui::ReadSetting(setting.key);
  }
  const auto plan = RequestedStagePlan();
  Json passes = Json::array();
  Json masks = Json::array();
  for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
    passes.push_back({{"pass", pass + 1}, {"requested", stage_plan::Name(plan.requested[pass])},
        {"effective", plan.valid ? Json(stage_plan::Name(plan.effective[pass])) : Json(nullptr)}});
    const auto mask = ResolveControlMaskParameters(pass);
    masks.push_back({{"pass", pass + 1}, {"mode", mask.mode}, {"blend_mode", mask.blend_mode},
        {"rgb", mask.rgb}, {"depth_near", mask.depth_near}, {"depth_far", mask.depth_far},
        {"motion_enabled", mask.motion_enabled}, {"motion_start", mask.motion_start}, {"motion_end", mask.motion_end},
        {"evidence", "resolved_configuration_not_execution"}});
  }
  Json stages = Json::array();
  for (size_t index = 0; index < stage_telemetry.size(); ++index) {
    const auto& stats = stage_telemetry[index];
    const auto& observed = stats.last_evaluate;
    Json completion{{"current", false}};
    const gpu_timers::Stats* latest = nullptr;
    if (plan.valid) {
      for (uint32_t group = 0; group < plan.group_count; ++group) {
        if (static_cast<size_t>(plan.groups[group].stage) != index) continue;
        for (const auto* sample : {&gpu_timers::stage_stats[index], &gpu_timers::execution_stats[index], &gpu_timers::completed_stats[index]}) {
          if (CurrentCompletedGroup(*sample, plan.groups[group], now)
              && (latest == nullptr || latest->sequence < sample->sequence)) latest = sample;
        }
      }
    }
    if (latest != nullptr) {
      Json completed_passes = Json::array();
      for (const auto& pass : latest->passes) {
        if (pass.id >= kMaxNrPasses || pass.state != gpu_timers::SampleState::VALID) continue;
        completed_passes.push_back({{"pass", pass.id + 1}, {"width", pass.width}, {"height", pass.height},
            {"model_request", pass.model}, {"internal_model", nullptr}});
      }
      completion = {{"current", true}, {"sequence", latest->sequence}, {"recorded_ns", latest->recorded_ns},
          {"completed_ns", latest->measured_ns}, {"width", latest->width}, {"height", latest->height},
          {"passes", std::move(completed_passes)}};
    }
    stages.push_back({{"stage", stage_plan::Name(static_cast<stage_plan::Stage>(index))},
        {"groups", stats.groups}, {"evaluates", stats.evaluates}, {"accepted", stats.accepted},
        {"last_evaluate", {{"sequence", observed.sequence}, {"observed_ns", observed.observed_ns},
            {"api_returned", observed.api_returned}, {"result", static_cast<uint32_t>(observed.result)},
            {"pass", observed.pass + 1}, {"requested", stage_plan::Name(observed.requested)},
            {"planned", stage_plan::Name(observed.planned)}, {"gpu_complete", false}}},
        {"completion", std::move(completion)}});
  }
  const auto adaptive = CopyAdaptiveStatus();
  Json sizes = Json::object();
  for (const auto& [name, values] : {std::pair{"upper", &adaptive.upper}, {"requested", &adaptive.requested}, {"applied", &adaptive.applied}}) {
    sizes[name] = Json::array();
    for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
      sizes[name].push_back({{"pass", pass + 1}, {"width", (*values)[pass].width}, {"height", (*values)[pass].height}});
    }
  }
  const auto output = output_framerate::Read(now);
  const auto fg_environment = ReadEditReuseFgEnvironmentLocked();
  Json native_evaluate = nullptr;
  if (edit_reuse_enabled.load() && edit_reuse_fg_experiment.load()) {
    const auto observed = fg::native_options::frame_source::Snapshot().evaluations;
    native_evaluate = {{"feature_order", {sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureDLSS_G, "other"}},
        {"feature_calls", {observed.features[0].enters, observed.features[1].enters,
            observed.features[2].enters, observed.features[3].enters}},
        {"ngx_fg_calls", observed.ngx_fg_calls}, {"ngx_fg_under_dlssg", observed.ngx_fg_under_dlssg},
        {"evidence", "cpu_calls_only"}};
  }
  edit_fg_observation::Snapshot fg_observed;
  edit_fg_observation::CopySnapshot(&fg_observed);
  const auto resource_identity = [](const edit_fg_observation::ResourceIdentity& resource) {
    return Json{{"raw", resource.raw}, {"canonical", resource.canonical},
        {"canonical_available", resource.canonical != 0}};
  };
  const auto recording_identity = [](const edit_fg_observation::RecordingIdentity& recording) {
    return Json{{"raw", recording.raw}, {"list_id", recording.list_id}, {"generation", recording.generation},
        {"identity_available", recording.list_id != 0 && recording.generation != 0}};
  };
  const auto source_observation = [&](const edit_fg_observation::SrCopyback& source) {
    return Json{{"handle", source.source_handle}, {"generation", source.source_generation},
        {"sequence", source.sr_sequence}, {"reused", source.reused},
        {"output", resource_identity(source.output)}, {"size", {source.width, source.height}}, {"format", source.format},
        {"recording", recording_identity(source.recording)},
        {"environment_provider", source.environment_provider}, {"environment_context", source.environment_context},
        {"environment_generation", source.environment_generation}};
  };
  const char* fg_observation_state = "disabled";
  switch (fg_observed.state) {
    case edit_fg_observation::State::DISABLED: break;
    case edit_fg_observation::State::ARMED: fg_observation_state = "armed"; break;
    case edit_fg_observation::State::CAPTURING: fg_observation_state = "capturing"; break;
    case edit_fg_observation::State::STOPPED: fg_observation_state = "stopped"; break;
  }
  Json fg_observation_events = Json::array();
  for (uint32_t index = 0; index < fg_observed.count; ++index) {
#if defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
    // The dedicated output probe needs its SDK receipts, not a second full
    // metadata trace. Keep a labeled prefix within the unchanged pipe limit.
    if (index == 16) break;
#endif
    const auto& observed = fg_observed.events[index];
    Json event{{"sequence", observed.sequence}, {"observed_ns", observed.observed_ns},
        {"thread_id", observed.thread_id}};
    switch (observed.kind) {
      case edit_fg_observation::Kind::SR_COPYBACK:
        event["kind"] = "sr_copyback_recorded";
        event["source"] = source_observation(observed.sr);
        break;
      case edit_fg_observation::Kind::NATIVE_FG_ENTRY: {
        const auto& native = observed.native_fg;
        event["kind"] = "native_fg_entry";
        event["call_id"] = observed.call_id;
        event["native"] = {{"handle", native.handle}, {"environment_generation", native.generation},
            {"slot", native.slot}, {"c_export", native.c_export},
            {"recording", recording_identity(native.recording)},
            {"backbuffer", resource_identity(native.backbuffer)}, {"hudless", resource_identity(native.hudless)},
            {"ui", resource_identity(native.ui)}, {"ui_alpha", resource_identity(native.ui_alpha)},
            {"ui_mode", native.has_ui_mode ? Json(native.ui_mode) : Json(nullptr)},
            {"frame_id", native.has_frame_id ? Json(native.frame_id) : Json(nullptr)}};
        break;
      }
      case edit_fg_observation::Kind::XEFG_TAG_ENTRY: {
        const auto& tag = observed.xefg_tag;
        event["kind"] = "xefg_tag_entry";
        event["call_id"] = observed.call_id;
        event["tag"] = {{"sdk_context", tag.sdk_context}, {"sdk_slot", tag.sdk_slot},
            {"context_identity", tag.context_identity},
            {"epoch", tag.epoch}, {"generation", tag.generation}, {"present_id", tag.present_id},
            {"type", tag.type}, {"validity", tag.validity}, {"resource", resource_identity(tag.resource)},
            {"recording", recording_identity(tag.recording)}, {"base", {tag.base_x, tag.base_y}},
            {"size", {tag.width, tag.height}}, {"resource_state", tag.resource_state},
            {"ui_mode", tag.ui_mode}, {"ui_composition", tag.ui_composition}};
        break;
      }
      case edit_fg_observation::Kind::XEFG_PRESENT_ENTRY: {
        const auto& present = observed.xefg_present;
        event["kind"] = "xefg_present_entry";
        event["call_id"] = observed.call_id;
        event["present"] = {{"sdk_context", present.sdk_context}, {"context_identity", present.context_identity},
            {"epoch", present.epoch}, {"generation", present.generation},
            {"present_id", present.has_present_id ? Json(present.present_id) : Json(nullptr)},
            {"repeated", present.repeated}, {"proxy", resource_identity(present.proxy)},
            {"backbuffer", resource_identity(present.backbuffer)}, {"method", present.method},
            {"interval", present.interval}, {"flags", present.flags}};
        break;
      }
      case edit_fg_observation::Kind::NATIVE_FG_RETURN:
      case edit_fg_observation::Kind::XEFG_TAG_RETURN:
      case edit_fg_observation::Kind::XEFG_PRESENT_RETURN:
        switch (observed.kind) {
          case edit_fg_observation::Kind::NATIVE_FG_RETURN: event["kind"] = "native_fg_return"; break;
          case edit_fg_observation::Kind::XEFG_TAG_RETURN: event["kind"] = "xefg_tag_return"; break;
          default: event["kind"] = "xefg_present_return"; break;
        }
        event["call_id"] = observed.call_id;
        event["api_result"] = observed.api_result;
        event["api_return_ns"] = observed.api_return_ns;
        if (observed.kind == edit_fg_observation::Kind::XEFG_PRESENT_RETURN) {
          const auto& status = observed.present_status;
          const bool valid = status.queried && status.api_result == 0;
          event["sdk_present_status"] = {{"queried", status.queried},
              {"api_result", status.queried ? Json(status.api_result) : Json(nullptr)},
              {"frames_presented", valid ? Json(status.frames_presented) : Json(nullptr)},
              {"frame_gen_result", valid ? Json(status.frame_gen_result) : Json(nullptr)},
              {"enabled", valid ? Json(status.enabled) : Json(nullptr)}};
        }
        break;
    }
    fg_observation_events.push_back(std::move(event));
  }
  const auto device_observation = [](const fg::xefg::BridgeDeviceObservation& observed) {
    return Json{{"object_present", observed.object_present}, {"swapchain_unwrapped", observed.swapchain_unwrapped},
        {"swapchain_result", observed.swapchain_result}, {"device11_result", observed.device11_result},
        {"device12_result", observed.device12_result}, {"returned_device_request", observed.returned_device_request},
        {"returned_device_identity", observed.returned_device_identity},
        {"device11_interface_result", observed.device11_interface_result},
        {"device12_interface_result", observed.device12_interface_result},
        {"device11_interface_identity", observed.device11_interface_identity},
        {"device12_interface_identity", observed.device12_interface_identity}};
  };
  Json edit_off_sources = Json::array();
  Json edit_device_observation = nullptr;
  Json edit_off_events = nullptr;
  if (edit_reuse_adaptive_experiment.load()) {
    const auto trace = fg::xefg::CopyBridgeEvents();
    Json events = Json::array();
    for (uint32_t index = 0; index < trace.count; ++index) {
      const auto& event = trace.events[index];
      const char* kind = "unobserved";
      switch (event.kind) {
        case fg::xefg::BridgeEventKind::ENABLE_BEGIN: kind = "enable_begin"; break;
        case fg::xefg::BridgeEventKind::ENABLE_END: kind = "enable_end"; break;
        case fg::xefg::BridgeEventKind::LINK_REVOKED: kind = "link_revoked"; break;
        case fg::xefg::BridgeEventKind::LINK_OBSERVED: kind = "link_observed"; break;
      }
      const char* method = "unobserved";
      switch (event.source_entry.method) {
        case fg::xefg::BridgePresentMethod::UNOBSERVED: break;
        case fg::xefg::BridgePresentMethod::PRESENT: method = "Present"; break;
        case fg::xefg::BridgePresentMethod::PRESENT1: method = "Present1"; break;
      }
      events.push_back({{"kind", kind}, {"sequence", event.sequence}, {"observed_ns", event.observed_ns},
          {"sdk_return_ns", event.sdk_return_ns}, {"thread_id", event.thread_id},
          {"context_identity", event.context_identity}, {"declaration", event.declaration}, {"epoch", event.epoch},
          {"link_generation", event.link_generation}, {"source_swapchain", event.source_swapchain},
          {"source_swapchain_generation", event.source_swapchain_generation},
          {"source", {{"handle", event.source.handle}, {"generation", event.source.generation},
              {"device11", event.source.device11}, {"device12", event.source.device12}}},
          {"reason", event.reason}, {"enabled_request", event.enabled_request}, {"sdk_status", event.sdk_status},
          {"off_observed", event.off_observed},
          {"source_entry", {{"method", method}, {"return_address", event.source_entry.return_address},
              {"thread_id", event.source_entry.thread_id}, {"proxy_entry_depth", event.source_entry.proxy_entry_depth},
              {"interval", event.source_entry.interval}, {"flags", event.source_entry.flags}}}});
    }
    edit_off_events = {{"last_sequence", trace.last_sequence}, {"overwritten", trace.overwritten},
        {"events", std::move(events)}};
    const auto observed = fg::xefg::QueryBridgeOff({});
    edit_device_observation = {{"query_reason", observed.reason}, {"sdk_context", observed.context},
        {"sdk_proxy", observed.proxy}, {"context_identity", observed.context_identity},
        {"context_epoch", observed.epoch}, {"init_kind", observed.init_kind},
        {"application_device11", observed.application_device11}, {"proxy_device11", observed.proxy_device11},
        {"application_device_observation", device_observation(observed.application)},
        {"proxy_device_observation", device_observation(observed.proxy_device)}};
    for (const auto& [handle, source] : create_contracts) {
      if (source.source_device12_identity == nullptr) continue;
      const auto off = EditReuseXefgOffLocked(handle);
      edit_off_sources.push_back({{"source_handle", reinterpret_cast<uintptr_t>(handle)},
          {"source_generation", source.source_generation},
          {"device12", reinterpret_cast<uintptr_t>(source.source_device12_identity.Get())},
          {"available", off.available}, {"reason", off.reason}, {"source_link_reason", off.source_link_reason},
          {"context_identity", off.context_identity}, {"context_epoch", off.epoch},
          {"sdk_context", off.context}, {"sdk_proxy", off.proxy}, {"init_kind", off.init_kind},
          {"application_device11", off.application_device11}, {"proxy_device11", off.proxy_device11},
          {"application_device_observation", device_observation(off.application)},
          {"proxy_device_observation", device_observation(off.proxy_device)},
          {"source_link_generation", off.source_link_generation}, {"uses_source_link", off.uses_source_link},
          {"source_swapchain", off.source_swapchain}, {"source_swapchain_generation", off.source_swapchain_generation},
          {"source_present_thread_id", off.source_present_thread_id},
          {"source_present_proxy_entry_depth", off.source_present_proxy_entry_depth},
          {"provider_matches", off.MatchesOutput(output)}});
    }
  }
  Json edit_sources = Json::array();
  Json caches = Json::array();
  for (const auto& [handle, feature] : features) {
    for (uint32_t pass = 0; pass < kMaxNrPasses; ++pass) {
      const auto& edit = feature.slots[pass].edit_reuse;
      if (edit == nullptr) continue;
      edit_sources.push_back({{"source_handle", reinterpret_cast<uintptr_t>(handle)},
          {"source_generation", feature.source_generation}, {"source_frame", feature.sr_sequence},
          {"source_type_confirmed", feature.source_type_confirmed},
          {"source_feature", static_cast<uint32_t>(feature.source_feature)}, {"pass", pass + 1},
          {"working_size", {edit->width, edit->height}}, {"output_size", {edit->output_width, edit->output_height}},
          {"anchor_generation", edit->anchor_generation}, {"source_sequence", edit->source_sequence},
          {"reuse_count", edit->reuse_count}, {"valid", edit->valid}, {"reused", edit->reused},
          {"evidence", "recorded_edit_cache_state_not_gpu_completion"}});
    }
    const auto& cache = feature.adaptive_cache;
    if (cache == nullptr) continue;
    Json slots = Json::array();
    for (size_t index = 0; index < cache->count; ++index) {
      const auto& tier = cache->tiers[index];
      const auto& current = (index == cache->active ? feature : tier.state);
      Json models = Json::array();
      for (uint32_t pass = 0; pass < kMaxNrPasses; ++pass) {
        const auto& model = current.slots[pass];
        if (model.handle == nullptr) continue;
        models.push_back({{"pass", pass + 1}, {"handle", reinterpret_cast<uintptr_t>(model.handle)},
            {"parameters", reinterpret_cast<uintptr_t>(model.parameters)},
            {"create_input", {model.create_input_width, model.create_input_height}},
            {"create_output", {model.create_output_width, model.create_output_height}}});
      }
      slots.push_back({{"slot", index}, {"active", index == cache->active}, {"ready", tier.ready},
          {"logical_size", {current.input_width, current.input_height}}, {"models", std::move(models)}});
    }
    Json sampling = nullptr;
    if (feature.adaptive_session != nullptr) {
      const auto& session = *feature.adaptive_session;
      double median = 0., mad = 0.;
      const bool statistics_valid = session.GpuStatistics(now, &median, &mad);
      sampling = {{"paired_sampling", session.paired_sampling}, {"sampling_generation", session.sampling_generation},
          {"gpu_count", session.gpu_count},
          {"latest_recorded_ns", session.gpu_count != 0 ? Json(session.gpu_recorded_ns[session.gpu_count - 1]) : Json(nullptr)},
          {"statistics_valid", statistics_valid}, {"median_us", statistics_valid ? Json(median) : Json(nullptr)},
          {"mad_us", statistics_valid ? Json(mad) : Json(nullptr)}, {"evidence", "controller_per_source_gpu_chain"}};
    }
    caches.push_back({{"source_handle", reinterpret_cast<uintptr_t>(handle)}, {"physical_slot_count", cache->count},
        {"active_slot", cache->active}, {"source_frame", feature.sr_sequence},
        {"trial", feature.adaptive_session != nullptr ? Json(feature.adaptive_session->trial) : Json(nullptr)},
        {"sampling", std::move(sampling)}, {"slots", std::move(slots)}});
  }
  if (identity != Identity()) {
    state["reason"] = "configuration_changed";
    return state;
  }
  state.update({{"available", true}, {"reason", "observed"}, {"sampled_ns", now}, {"identity", identity},
      {"settings", std::move(settings)}, {"configuration_generation", identity["configuration_generation"]},
      {"config_epoch", identity["config_epoch"]}, {"presents", presents_observed.load()},
      {"successful_source_frames", SourceSuccessfulEvaluations()},
      {"nr_features_created", nr_features_created},
      {"edit_reuse", {{"requested", edit_reuse_enabled.load()}, {"refreshes", edit_reuse_refreshes.load()},
          {"reuses", edit_reuse_reuses.load()}, {"fallbacks", edit_reuse_fallbacks.load()},
          {"sources", std::move(edit_sources)},
          {"fg_environment", {{"available", fg_environment.available}, {"reason", fg_environment.reason},
              {"provider", fg_environment.provider}, {"context", fg_environment.context},
              {"generation", fg_environment.generation},
              {"evidence", "fg_environment_metadata_not_source_handoff"}}},
          {"fg_observation", {{"enabled", fg_observed.enabled}, {"state", fg_observation_state},
              {"reason", fg_observed.stop_reason}, {"request_generation", fg_observed.request_generation},
              {"source_frames", fg_observed.source_frames}, {"event_count", fg_observed.count},
              {"overflow", fg_observed.overflow},
              {"source", source_observation(fg_observed.source)}, {"events", std::move(fg_observation_events)},
              {"evidence", "cpu_call_and_recording_observations_not_gpu_completion_or_pixel_handoff"},
              {"resource_identity_scope", "IUnknown_after_known_ReShade_unwrap;Streamline_faces_remain_distinct"},
              {"first_source_prefix_complete", false}}},
          {"xefg_off_sources", std::move(edit_off_sources)},
          {"xefg_device_observation", std::move(edit_device_observation)},
          {"xefg_off_events", std::move(edit_off_events)},
          {"evidence", "process_counters_not_activation_or_gpu_completion"}}},
      {"experiments", {{"adaptive_feature_mode", identity["experiments"]["adaptive_feature_mode"]},
          {"bridge_adaptive_experiment", identity["experiments"]["bridge_adaptive_experiment"]},
          {"bridge_adaptive_evidence", "requested_enabled_not_activation"},
          {"edit_reuse_adaptive_experiment", identity["experiments"]["edit_reuse_adaptive_experiment"]},
          {"edit_reuse_adaptive_evidence", identity["experiments"]["edit_reuse_adaptive_evidence"]},
          {"edit_reuse_fg_experiment", identity["experiments"]["edit_reuse_fg_experiment"]},
          {"edit_reuse_fg_evidence", "requested_enabled_not_activation"},
          {"evidence", "requested_mode_not_activation"}}},
      {"adaptive_caches", std::move(caches)},
      {"output_framerate", {{"available", output.available}, {"reason", output_framerate::ReasonText(output.reason)},
          {"source", output_framerate::SourceText(output.source)}, {"source_generation", output.source_generation},
          {"identity", output.identity}, {"fg_off", output.fg_off},
          {"configuration_generation", output.configuration_generation}, {"window_serial", output.window_serial},
          {"window_begin_ns", output.window_begin_ns}, {"window_end_ns", output.window_end_ns},
          {"fps", output.available ? Json(output.fps) : Json(nullptr)},
          {"fps_mad", output.available ? Json(output.fps_mad) : Json(nullptr)},
          {"requested_multiplier", output.requested_multiplier != 0 ? Json(output.requested_multiplier) : Json(nullptr)},
          {"observed_multiplier", output.available && output.observed_multiplier > 0. ? Json(output.observed_multiplier) : Json(nullptr)}}},
      {"disabled_resources_released", disabled_resources_released.load()}, {"overlay_open", hud_overlay_open.load()},
      {"plan", {{"valid", plan.valid}, {"reason", stage_plan::ReasonText(plan.reason)}, {"passes", std::move(passes)}}},
      {"stages", std::move(stages)}, {"resolved_masks", std::move(masks)},
      {"adaptive", {{"reason", adaptive_resolution::ReasonText(adaptive.reason)}, {"generation", adaptive.generation},
          {"coefficient", adaptive.coefficient}, {"ready_levels", adaptive.ready_levels},
          {"prepared_levels", adaptive.prepared_levels}, {"upper", sizes["upper"]},
          {"requested", sizes["requested"]}, {"applied", sizes["applied"]},
          {"output_fps", output.available && adaptive.output_fps > 0. ? Json(adaptive.output_fps) : Json(nullptr)},
          {"gpu_ms", adaptive.gpu_ms > 0. ? Json(adaptive.gpu_ms) : Json(nullptr)},
          {"measurement_scope", "last_controller_observation"},
          {"preparation_cpu_ms", adaptive.preparation_cpu_ms},
          {"preparation_vram_delta_bytes", adaptive.preparation_vram_delta_bytes},
          {"peak_process_vram_bytes", adaptive.peak_process_vram_bytes}}}});
  if (!native_evaluate.is_null()) {
    state["edit_reuse"]["native_evaluate"] = std::move(native_evaluate);
  }
#if defined(NR_NEXUS_TEST_FG_OUTPUT_PROBE)
  const auto probe = fg_output_probe::CopySnapshot();
  const auto& native_probe = probe.native;
  Json xefg_probes = Json::array();
  for (const auto& context : probe.xefg) {
    if (!context.observed) continue;
    xefg_probes.push_back({{"sequence", context.observed_sequence}, {"registered", context.registered},
        {"stopped", context.stopped}, {"reason", context.reason}, {"sdk_slot", context.sdk_slot},
        {"module_base", context.module_base}, {"sdk_context", context.sdk_context},
        {"context_identity", context.context_identity}, {"init_epoch", context.init_epoch},
        {"init_generation", context.init_generation}, {"current_epoch", context.current_epoch},
        {"current_generation", context.current_generation}, {"debug_export", context.debug_export},
        {"init_result", context.init_result_available ? Json(context.init_result) : Json(nullptr)},
        {"show_result", context.show_called ? Json(context.show_result) : Json(nullptr)},
        {"tag_result", context.tag_called ? Json(context.tag_result) : Json(nullptr)},
        {"debug_applied", context.debug_applied},
        {"active", context.registered && !context.stopped && context.debug_applied
            && fg_environment.available && fg_environment.provider == 2
            && fg_environment.context == context.context_identity},
        {"destroy_result", context.destroy_called ? Json(context.destroy_result) : Json(nullptr)}});
  }
  const auto& marker = fg_output_marker_window;
  state["edit_reuse"]["fg_observation"]["serialized_event_limit"] = 16;
  state["edit_reuse"]["fg_output_probe"] = {
      {"dedicated_validation_build", true}, {"evidence", "sdk_display_configuration_and_marker_recording_not_output_pixels"},
      {"sequence", probe.sequence}, {"overflow", probe.overflow}, {"xefg", std::move(xefg_probes)},
      {"native", {{"observed", native_probe.observed}, {"active", native_probe.active},
          {"sequence", native_probe.observed_sequence}, {"success_generation", native_probe.after_success_generation},
          {"reason", native_probe.reason}, {"stopped", native_probe.stopped},
          {"version_supported", native_probe.version_supported},
          {"viewport", native_probe.observed ? Json(native_probe.viewport) : Json(nullptr)},
          {"struct_version", native_probe.observed ? Json(native_probe.struct_version) : Json(nullptr)},
          {"mode", native_probe.version_supported ? Json(native_probe.mode) : Json(nullptr)},
          {"num_frames", native_probe.version_supported ? Json(native_probe.num_frames) : Json(nullptr)},
          {"original_flags", native_probe.version_supported ? Json(native_probe.original_flags) : Json(nullptr)},
          {"forwarded_flags", native_probe.version_supported ? Json(native_probe.forwarded_flags) : Json(nullptr)},
          {"flag_forwarded", native_probe.flag_forwarded},
          {"api_result", native_probe.observed ? Json(native_probe.result) : Json(nullptr)}}},
      {"marker", {{"request_generation", marker.request_generation}, {"enabled", marker.enabled},
          {"bound", marker.bound}, {"stopped", marker.stopped}, {"reason", marker.reason},
          {"frame_limit", marker.MAX_FRAMES}, {"recorded", marker.recorded},
          {"refreshes", marker.refreshes}, {"reuses", marker.reuses},
          {"first_sequence", marker.first_sequence}, {"last_sequence", marker.last_sequence},
          {"last_recorded_ns", marker.last_recorded_ns}, {"source", marker.binding.source},
          {"source_generation", marker.binding.source_generation}, {"provider", marker.binding.provider},
          {"context", marker.binding.context}, {"environment_generation", marker.binding.environment_generation},
          {"roi_position", "source_width/4,source_height/3"}, {"roi_size", {128, 64}},
          {"encoding", "scRGB_FP16_or_PQ_RGB10A2_after_commit_alpha_preserved"}}}};
#endif
  return state;
}

inline Json Execute(const Json& request) {
  const auto id = request.find("id");
  if (!request.is_object() || id == request.end() || !id->is_string()
      || id->get_ref<const std::string&>().empty() || id->get_ref<const std::string&>().size() > 96
      || !std::all_of(id->get_ref<const std::string&>().begin(), id->get_ref<const std::string&>().end(),
          [](unsigned char value) { return std::isalnum(value) || value == '-' || value == '_' || value == '.'; })) {
    return {{"ok", false}, {"status", "rejected"}, {"reason", "invalid_request_id"}};
  }
  const auto operation = request.find("op");
  if (operation == request.end() || !operation->is_string()) {
    return {{"id", *id}, {"ok", false}, {"status", "rejected"}, {"reason", "invalid_operation"}};
  }
  const bool marker_operation = *operation == "fg_output_marker";
  const bool consumer_operation = *operation == "fg_consumer_capture";
  const bool compact_operation = marker_operation || consumer_operation;
  const auto consumer_snapshot = []() {
    const auto capture = screenshot::CopyFgConsumerStatus();
    return Json{{"capture_id", capture.capture_id}, {"status", capture.status},
        {"reason", capture.reason}, {"serial", capture.serial}};
  };
  const auto marker_snapshot = []() {
    const auto& marker = fg_output_marker_window;
    const auto now = SteadyNowNs();
    return Json{{"request_generation", marker.request_generation}, {"enabled", marker.enabled},
        {"bound", marker.bound}, {"stopped", marker.stopped}, {"reason", marker.reason},
        {"expired", marker.enabled && (now < marker.armed_ns || now - marker.armed_ns >= marker.MAX_DURATION_NS)},
        {"frame_limit", marker.MAX_FRAMES}, {"duration_ms", marker.MAX_DURATION_NS / 1'000'000},
        {"recorded", marker.recorded}, {"refreshes", marker.refreshes}, {"reuses", marker.reuses},
        {"first_sequence", marker.first_sequence}, {"last_sequence", marker.last_sequence},
        {"source", marker.binding.source}, {"source_generation", marker.binding.source_generation},
        {"provider", marker.binding.provider}, {"context", marker.binding.context},
        {"environment_generation", marker.binding.environment_generation},
        {"evidence", "marker_recording_not_GPU_completion_or_FG_pixels"}};
  };
  if (marker_operation && request.value("action", Json(nullptr)) == "status") {
    RuntimeTryLock lock(runtime_mutex);
    if (!lock.owns_lock()) {
      return {{"id", *id}, {"ok", false}, {"status", "rejected"}, {"reason", "runtime_busy"},
          {"state", ProcessIdentityState()}};
    }
    return {{"id", *id}, {"ok", true}, {"status", "observed"}, {"marker", marker_snapshot()},
        {"state", ProcessIdentityState()}};
  }
  if (consumer_operation && request.value("action", Json(nullptr)) == "status") {
    return {{"id", *id}, {"ok", true}, {"status", "observed"}, {"capture", consumer_snapshot()},
        {"state", ProcessIdentityState()}};
  }
  if (*operation == "status") {
    auto state = Snapshot();
    return {{"id", *id}, {"ok", true}, {"status", "observed"}, {"state", state},
        {"identity", state.value("identity", Json(nullptr))},
        {"receipt", request.contains("query_id") && last_request.is_object()
                && request["query_id"] == last_request["id"] ? last_receipt : Json(nullptr)}};
  }
  if (*operation != "set" && *operation != "adaptive_feature_experiment"
      && *operation != "bridge_adaptive_experiment" && *operation != "edit_reuse_adaptive_experiment"
      && *operation != "edit_reuse_fg_experiment"
      && !compact_operation
      && *operation != "inline_restore_roi") {
    return {{"id", *id}, {"ok", false}, {"status", "rejected"}, {"reason", "invalid_operation"}};
  }
  if (last_request.is_object() && last_request["id"] == *id) {
    if (request == last_request) return last_receipt;
    if (compact_operation) {
      return {{"id", *id}, {"ok", false}, {"status", "rejected"}, {"reason", "request_id_reused"},
          {"state", ProcessIdentityState()}};
    }
    return {{"id", *id}, {"ok", false}, {"status", "rejected"}, {"reason", "request_id_reused"}};
  }
  Json response{{"id", *id}, {"ok", false}, {"status", "rejected"}, {"reason", "invalid_setting"},
      {"message", ""}, {"changes", Json::object()}};
  const auto finish = [&]() {
    if (compact_operation) {
      response["state"] = ProcessIdentityState();
    } else {
      response["state"] = Snapshot();
      if (response["state"].value("available", false)) {
        // Status can contain both the current state and this cached receipt.
        // Keep bounded events in the current status, rather than duplicating
        // them into every mutation receipt under the same 64 KiB pipe limit.
        auto& observed = response["state"]["edit_reuse"]["fg_observation"];
        observed["events"] = Json::array();
        observed["events_in_status_only"] = true;
        response["state"]["edit_reuse"].erase("native_evaluate");
      }
      response["identity"] = response["state"].value("identity", Json(nullptr));
    }
    last_request = request;
    last_receipt = response;
    Log(reshade::log::level::info, NR_NEXUS_NAME " development control: id=" + id->get<std::string>()
        + " status=" + response["status"].get<std::string>() + " reason=" + response["reason"].get<std::string>());
    return response;
  };
  const bool configuration_busy = shutting_down.load() || slider_pending != nullptr || stage_edit::pending || resolution_edit::pending
      || stage_edit_request.setting != nullptr || !resolution_edit_request.empty() || profile_ui::Busy()
      || profile_ui::pending_action != profile_ui::Action::NONE || preset_ui::manager_geometry.was_open;
  if (consumer_operation) {
    response["reason"] = "invalid_consumer_request";
    const auto directory = request.find("output_directory");
    const auto capture_id = request.find("capture_id");
    const auto roi = request.find("roi");
    if (request.value("action", Json(nullptr)) != "arm"
        || directory == request.end() || !directory->is_string()
        || capture_id == request.end() || !capture_id->is_string() || roi == request.end() || !roi->is_object()) return finish();
    for (const char* key : {"x", "y", "width", "height"}) {
      const auto value = roi->find(key);
      if (value == roi->end() || !value->is_number_integer() || *value < 0 || *value > UINT32_MAX) return finish();
    }
    if (configuration_busy) {
      response["reason"] = "configuration_busy";
      return finish();
    }
    bool route_ready = false;
    {
      RuntimeTryLock lock(runtime_mutex);
      if (!lock.owns_lock()) {
        response["reason"] = "runtime_busy";
      } else {
        const auto environment = ReadEditReuseFgEnvironmentLocked();
        route_ready = enabled.load() && hook_point.load() == kHookUpscaled
            && environment.available && environment.provider == 1;
        response["reason"] = "native_consumer_route_unavailable";
      }
    }
    if (route_ready) {
      try {
        const auto& utf8_directory = directory->get_ref<const std::string&>();
        std::string reason;
        const bool armed = screenshot::RequestFgConsumerCapture(
            std::filesystem::path(std::u8string(utf8_directory.begin(), utf8_directory.end())),
            capture_id->get_ref<const std::string&>(),
            {.x = (*roi)["x"].get<uint32_t>(), .y = (*roi)["y"].get<uint32_t>(),
                .width = (*roi)["width"].get<uint32_t>(), .height = (*roi)["height"].get<uint32_t>()}, &reason);
        response["ok"] = armed;
        response["status"] = (armed ? "armed" : "rejected");
        response["reason"] = reason;
      } catch (...) {
        response["reason"] = "invalid_consumer_request";
      }
    }
    response["capture"] = consumer_snapshot();
    return finish();
  }
  if (marker_operation) {
    const auto action = request.find("action");
    response["reason"] = "invalid_marker_action";
    if (action == request.end() || !action->is_string() || (*action != "arm" && *action != "stop")) return finish();
    {
      RuntimeTryLock lock(runtime_mutex);
      if (!lock.owns_lock()) {
        response["reason"] = "runtime_busy";
      } else {
        if (*action == "stop") {
          fg_output_marker_window.Stop("explicit_stop");
          response.update({{"ok", true}, {"status", "stopped"}, {"reason", "marker_stopped"}});
        } else if (configuration_busy) {
          response["reason"] = "configuration_busy";
        } else if (!enabled.load() || !edit_reuse_enabled.load()) {
          response["reason"] = "nr_x2_not_running";
        } else {
          const auto environment = ReadEditReuseFgEnvironmentLocked();
          const bool route = (environment.provider == 1 && hook_point.load() == kHookUpscaled
                  && edit_reuse_fg_experiment.load())
              || (environment.provider == 2 && hook_point.load() == kHookPresent);
          if (!environment.available || !route) {
            response["reason"] = "marker_route_unavailable";
          } else {
            fg_output_marker_window.Arm(SteadyNowNs());
            response.update({{"ok", true}, {"status", "armed"}, {"reason", "marker_requested"}});
          }
        }
        response["marker"] = marker_snapshot();
      }
    }
    return finish();
  }
  if (*operation == "inline_restore_roi") {
    response["reason"] = "invalid_output_directory";
    const auto directory = request.find("output_directory");
    if (directory == request.end() || !directory->is_string()) return finish();
    uint32_t tail_frames = 0;
    if (const auto count = request.find("tail_frames"); count != request.end()) {
      if (!count->is_number_integer() || *count < 2 || *count > screenshot::MAX_INLINE_TAIL_FRAMES) {
        response["reason"] = "invalid_tail_frame_count";
        return finish();
      }
      tail_frames = count->get<uint32_t>();
    }
    if (configuration_busy) {
      response["reason"] = "configuration_busy";
      return finish();
    }
    std::string reason;
    try {
      const auto& utf8_directory = directory->get_ref<const std::string&>();
      const bool armed = screenshot::RequestInlineRestoreRoi(
          std::filesystem::path(std::u8string(utf8_directory.begin(), utf8_directory.end())), &reason, tail_frames);
      response["ok"] = armed;
      response["status"] = (armed ? "armed" : "rejected");
      response["reason"] = reason;
    } catch (...) {
      response["reason"] = "invalid_output_directory";
    }
    return finish();
  }
  const bool bridge_experiment = *operation == "bridge_adaptive_experiment";
  const bool fg_experiment = *operation == "edit_reuse_fg_experiment";
  const bool boolean_experiment = bridge_experiment || fg_experiment || *operation == "edit_reuse_adaptive_experiment";
  if (*operation == "adaptive_feature_experiment" || boolean_experiment) {
    const auto value = request.find(boolean_experiment ? "enabled" : "mode");
    response["reason"] = (boolean_experiment ? "invalid_enabled" : "invalid_mode");
    if (boolean_experiment) {
      if (value == request.end() || !value->is_boolean()) return finish();
    } else if (value == request.end() || !value->is_number_integer() || *value < 0 || *value > 3) {
      return finish();
    }
    response["requested"] = {{boolean_experiment ? "enabled" : "mode", *value}};
    if (configuration_busy) {
      response["reason"] = "configuration_busy";
      return finish();
    }
    {
      RuntimeTryLock runtime_lock(runtime_mutex);
      if (!runtime_lock.owns_lock()) {
        response["reason"] = "runtime_busy";
      } else if (enabled.load()) {
        response["reason"] = "nr_running";
      } else if (!disabled_resources_released.load()) {
        response["reason"] = "resources_not_released";
      } else {
        bool unchanged = false;
        if (boolean_experiment) {
          auto* requested_flag = &edit_reuse_adaptive_experiment;
          if (bridge_experiment) {
            requested_flag = &bridge_adaptive_experiment;
          }
          if (fg_experiment) {
            requested_flag = &edit_reuse_fg_experiment;
          }
          const bool requested = value->get<bool>();
          unchanged = requested_flag->load() == requested;
          requested_flag->store(requested);
          if (fg_experiment && !unchanged) {
            edit_fg_observation::SetEnabled(requested);
            if (!requested) {
              fg_output_marker_window.Stop("fg_experiment_disabled");
            }
          }
        } else {
          const auto requested = static_cast<AdaptiveFeatureMode>(value->get<uint32_t>());
          unchanged = adaptive_feature_mode.load() == requested;
          adaptive_feature_mode.store(requested);
        }
        response["ok"] = true;
        response["status"] = unchanged ? "unchanged" : "applied";
        response["reason"] = "experiment_requested";
      }
    }
    return finish();
  }
  if (!request.contains("key") || !request["key"].is_string() || !request.contains("value")) return finish();
  const auto key = request["key"].get<std::string>();
  const auto& value = request["value"];
  response["requested"] = {{"key", key}, {"value", value}};
  const auto* setting = preset_ui::FindSetting(key);
  const auto* rule = presets::FindRule(key);
  if (!Allowed(key) || setting == nullptr || rule == nullptr) return finish();
  if (!preset_ui::ValidateValue(*rule, value)) {
    response["reason"] = "invalid_value";
    return finish();
  }
  if (configuration_busy) {
    response["reason"] = "configuration_busy";
    return finish();
  }
  if (key.starts_with("NRPass") && key != "NRPasses" && key.size() > 7
      && static_cast<uint32_t>(key[6] - '0') > stack_passes.load()) {
    response["reason"] = "inactive_pass";
    return finish();
  }
  const auto identity = Identity();
  const auto confirmation = request.find("confirm");
  if (confirmation != request.end() && (!confirmation->is_object()
          || !confirmation->contains("identity") || !confirmation->contains("changes")
          || (*confirmation)["identity"] != identity)) {
    response["reason"] = "configuration_changed";
    return finish();
  }
  Json changes{{key, value}};
  const bool unchanged = preset_ui::ReadSetting(key) == value;
  std::string error;
  bool prepared_stage = false;
  bool prepared_resolution = false;
  bool needs_confirmation = false;
  if (!unchanged && (IsStageSetting(*setting) || (setting == &kSetStackPasses && !ValidateConfigurationCapability(changes, nullptr)))) {
    prepared_stage = stage_edit::Prepare(*setting, value.get<float>());
    if (prepared_stage) {
      changes = stage_edit::changes;
      needs_confirmation = stage_edit::confirmation_required;
    } else {
      error = stage_edit::error;
      stage_edit::Cancel();
      if (setting != &kSetStackPasses) {
        response["reason"] = "configuration_unavailable";
        response["message"] = error.empty() ? FormatConfigurationCapabilityError(changes, QueryConfigurationCapability(changes)) : error;
        return finish();
      }
    }
  }
  if (!unchanged && !prepared_stage && (key == "NRPasses" || key == "NRFollowInputRes" || key == "NRResolutionScale"
          || key.ends_with("ResolutionMode") || key.ends_with("ResolutionScale"))) {
    prepared_resolution = resolution_edit::Prepare(changes);
    if (!prepared_resolution) {
      error = resolution_edit::error;
      resolution_edit::Cancel();
      response["reason"] = "configuration_unavailable";
      response["message"] = error.empty() ? FormatConfigurationCapabilityError(changes, QueryConfigurationCapability(changes)) : error;
      return finish();
    }
    changes = resolution_edit::changes;
    needs_confirmation = resolution_edit::confirmation_required;
  }
  response["changes"] = changes;
  bool applied = false;
  if ((needs_confirmation && confirmation == request.end())
      || (confirmation != request.end() && (*confirmation)["changes"] != changes)) {
    stage_edit::Cancel();
    resolution_edit::Cancel();
    response["reason"] = confirmation == request.end() ? "confirmation_required" : "proposal_changed";
    response["status"] = confirmation == request.end() ? "confirmation_required" : "rejected";
    return finish();
  }
  if (prepared_stage) {
    applied = stage_edit::Confirm();
    error = stage_edit::error;
    stage_edit::Cancel();
  } else if (prepared_resolution) {
    applied = resolution_edit::Confirm();
    error = resolution_edit::error;
    resolution_edit::Cancel();
  } else {
    applied = preset_ui::Apply({.metadata = Json::object(), .scope = presets::Scope::FULL, .settings = changes},
        key == "NRDisableRelease", false, &error);
  }
  response["ok"] = applied;
  response["status"] = applied ? (unchanged ? "unchanged" : "applied") : "rejected";
  response["reason"] = applied ? "configuration_committed" : "configuration_rejected";
  response["message"] = error;
  if (applied && key == "NeuralUplift") {
    NotifyNrOperation(enabled.load());
    EmitNrVerdict("development_control");
  }
  return finish();
}
}  // namespace dev_control
