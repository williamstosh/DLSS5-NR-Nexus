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
          {"edit_reuse_adaptive_evidence", "requested_enabled_not_activation"}}}};
}

inline Json Snapshot() {
  FILETIME created{}, exited{}, kernel{}, user{};
  const bool process_known = GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE;
  Json state{{"pid", GetCurrentProcessId()},
      {"process_created", process_known ? Json(std::to_string((uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime)) : Json(nullptr)},
      {"version", kAddonVersion}, {"available", false}, {"reason", "runtime_busy"}};
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
  Json caches = Json::array();
  for (const auto& [handle, feature] : features) {
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
          {"evidence", "process_counters_not_activation_or_gpu_completion"}}},
      {"experiments", {{"adaptive_feature_mode", identity["experiments"]["adaptive_feature_mode"]},
          {"bridge_adaptive_experiment", identity["experiments"]["bridge_adaptive_experiment"]},
          {"bridge_adaptive_evidence", "requested_enabled_not_activation"},
          {"edit_reuse_adaptive_experiment", identity["experiments"]["edit_reuse_adaptive_experiment"]},
          {"edit_reuse_adaptive_evidence", identity["experiments"]["edit_reuse_adaptive_evidence"]},
          {"evidence", "requested_mode_not_activation"}}},
      {"adaptive_caches", std::move(caches)},
      {"output_framerate", {{"available", output.available}, {"reason", output_framerate::ReasonText(output.reason)},
          {"source", output_framerate::SourceText(output.source)}, {"source_generation", output.source_generation},
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
  if (*operation == "status") {
    auto state = Snapshot();
    return {{"id", *id}, {"ok", true}, {"status", "observed"}, {"state", state},
        {"identity", state.value("identity", Json(nullptr))},
        {"receipt", request.contains("query_id") && last_request.is_object()
                && request["query_id"] == last_request["id"] ? last_receipt : Json(nullptr)}};
  }
  if (*operation != "set" && *operation != "adaptive_feature_experiment"
      && *operation != "bridge_adaptive_experiment" && *operation != "edit_reuse_adaptive_experiment"
      && *operation != "inline_restore_roi") {
    return {{"id", *id}, {"ok", false}, {"status", "rejected"}, {"reason", "invalid_operation"}};
  }
  if (last_request.is_object() && last_request["id"] == *id) {
    if (request == last_request) return last_receipt;
    return {{"id", *id}, {"ok", false}, {"status", "rejected"}, {"reason", "request_id_reused"}};
  }
  Json response{{"id", *id}, {"ok", false}, {"status", "rejected"}, {"reason", "invalid_setting"},
      {"message", ""}, {"changes", Json::object()}};
  const auto finish = [&]() {
    response["state"] = Snapshot();
    response["identity"] = response["state"].value("identity", Json(nullptr));
    last_request = request;
    last_receipt = response;
    Log(reshade::log::level::info, NR_NEXUS_NAME " development control: id=" + id->get<std::string>()
        + " status=" + response["status"].get<std::string>() + " reason=" + response["reason"].get<std::string>());
    return response;
  };
  const bool configuration_busy = shutting_down.load() || slider_pending != nullptr || stage_edit::pending || resolution_edit::pending
      || stage_edit_request.setting != nullptr || !resolution_edit_request.empty() || profile_ui::Busy()
      || profile_ui::pending_action != profile_ui::Action::NONE || preset_ui::manager_geometry.was_open;
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
  const bool boolean_experiment = bridge_experiment || *operation == "edit_reuse_adaptive_experiment";
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
          auto& requested_flag = (bridge_experiment ? bridge_adaptive_experiment : edit_reuse_adaptive_experiment);
          const bool requested = value->get<bool>();
          unchanged = requested_flag.load() == requested;
          requested_flag.store(requested);
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
