/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

// Included in the existing real-ImGui fixture; only host configuration and
// execution observations are synthetic. No device, model or game is created.
{
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
          && status["state"]["experiments"]["evidence"] == "requested_mode_not_activation"
          && status["state"]["adaptive_caches"].is_array(),
      "the experiment defaults to OFF and reports its request separately from execution");
  namespace rate = renodx::addons::dlss5::output_framerate;
  namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
  rate::Stream saved_output;
  const auto saved_adaptive = runtime::CopyAdaptiveStatus();
  {
    std::unique_lock lock(rate::internal::state.mutex);
    saved_output = rate::internal::state.stream;
    rate::internal::state.stream = {};
  }
  status = control::Snapshot();
  check(!status["output_framerate"]["available"].get<bool>() && status["output_framerate"]["fps"].is_null()
          && status["output_framerate"]["fps_mad"].is_null() && status["output_framerate"]["observed_multiplier"].is_null(),
      "an unavailable output source cannot publish valid frame rate or observed multiplier values");
  const auto sampled_ns = runtime::SteadyNowNs();
  {
    std::unique_lock lock(rate::internal::state.mutex);
    auto& stream = rate::internal::state.stream;
    stream.previous_ns = sampled_ns;
    stream.snapshot = {.available = true, .reason = rate::Reason::kREADY, .source = rate::Source::XEFG_STATUS,
        .source_generation = 9, .configuration_generation = 11, .window_serial = 4,
        .window_begin_ns = sampled_ns - 2'000'000'000, .window_end_ns = sampled_ns,
        .fps = 144., .fps_mad = 2., .requested_multiplier = 4, .observed_multiplier = 3.6};
  }
  runtime::PublishAdaptiveStatus({.reason = adaptive::Reason::OBSERVING,
      .generation = 7, .output_fps = 144., .gpu_ms = 2., .prepared_levels = 3,
      .preparation_cpu_ms = 5., .preparation_vram_delta_bytes = 4096, .peak_process_vram_bytes = 8192});
  status = control::Snapshot();
  check(status["output_framerate"]["available"] && status["output_framerate"]["source"] == rate::SourceText(rate::Source::XEFG_STATUS)
          && status["output_framerate"]["source_generation"] == 9 && status["output_framerate"]["configuration_generation"] == 11
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
    std::unique_lock lock(rate::internal::state.mutex);
    rate::internal::state.stream.previous_ns = sampled_ns - rate::Stream::STALE_NS - 1;
  }
  status = control::Snapshot();
  check(!status["output_framerate"]["available"].get<bool>() && status["output_framerate"]["fps"].is_null()
          && status["output_framerate"]["fps_mad"].is_null() && status["adaptive"]["output_fps"].is_null(),
      "stale SDK observations keep their source metadata but do not expose a valid current FPS");
  {
    std::unique_lock lock(rate::internal::state.mutex);
    rate::internal::state.stream = saved_output;
  }
  runtime::PublishAdaptiveStatus(saved_adaptive);
  check(set("EnableHooks", 0)["reason"] == "invalid_setting", "startup hook policy stays outside the control whitelist");
  check(set("NRHookPoint", 3)["reason"] == "invalid_value", "invalid hook is rejected before mutation");
  check(set("NeuralUplift", 1)["reason"] == "invalid_value", "booleans require JSON booleans");
  check(set("NRPasses", 0)["reason"] == "invalid_value", "zero pass count is rejected");
  check(control::Execute({{"id", "bad\nlog"}, {"op", "status"}})["reason"] == "invalid_request_id",
      "request ids cannot inject log lines");
  check(control::Execute({{"id", "unknown-op"}, {"op", "reset"}})["reason"] == "invalid_operation",
      "the development interface has no generic arbitrary command dispatch");
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
  check(experiment(2)["reason"] == "resources_not_released", "NR OFF alone does not prove its old resources are released");
  // This fixture has no GPU resources; the release proof is explicitly mocked.
  runtime::disabled_resources_released = true;
  runtime::enabled = true;
  check(experiment(2)["reason"] == "nr_running", "a running NR chain cannot change the experimental mode");
  runtime::enabled = false;
  runtime::slider_pending = &runtime::kSetAdaptiveTargetFps;
  check(experiment(2)["reason"] == "configuration_busy", "another configuration edit owns the experiment admission boundary");
  runtime::slider_pending = nullptr;
  check(runtime::adaptive_feature_mode.load() == runtime::AdaptiveFeatureMode::OFF,
      "rejected experiment requests leave the mode disabled");
  const Json experiment_request{{"id", "experiment-idempotent"}, {"op", "adaptive_feature_experiment"}, {"mode", 2}};
  const auto experimental = control::Execute(experiment_request);
  check(experimental["ok"] && experimental["state"]["available"]
          && experimental["state"]["experiments"]["adaptive_feature_mode"] == 2
          && runtime::adaptive_feature_mode.load() == runtime::AdaptiveFeatureMode::UPPER_CAPACITY,
      "an admitted experiment request reads back after releasing the nonrecursive runtime lock");
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
  check(configuration_writes == experiment_writes && runtime::configuration_generation == experiment_generation
          && runtime::nr_features_created == experiment_features,
      "experiment requests write no INI settings, recreate no features and leave ordinary configuration generation unchanged");
  runtime::disabled_resources_released = released_before;
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
  Json receipt{{"scope", "CPU production configuration and status adapters with synthetic ReShade cache; no GPU, NGX or game"},
      {"checks", checks}, {"configuration_writes", configuration_writes}, {"final", control::Snapshot()}};
  std::ofstream("dev-control-runtime.json") << receipt.dump(2);
  configuration_fixture::use_cache = false;
  configuration_fixture::cache.clear();
  std::printf("PASS development control production configuration cases; checks=%u\n", checks);
}
