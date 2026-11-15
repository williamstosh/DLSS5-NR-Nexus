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
          && !status["state"]["experiments"]["bridge_adaptive_experiment"].get<bool>()
          && !status["identity"]["experiments"]["bridge_adaptive_experiment"].get<bool>()
          && !status["state"]["experiments"]["edit_reuse_adaptive_experiment"].get<bool>()
          && !status["identity"]["experiments"]["edit_reuse_adaptive_experiment"].get<bool>()
          && status["identity"]["experiments"]["edit_reuse_adaptive_evidence"] == "requested_enabled_not_activation"
          && status["state"]["experiments"]["edit_reuse_adaptive_evidence"] == "requested_enabled_not_activation"
          && status["state"]["experiments"]["bridge_adaptive_evidence"] == "requested_enabled_not_activation"
          && status["state"]["experiments"]["evidence"] == "requested_mode_not_activation"
          && status["state"]["adaptive_caches"].is_array(),
      "the experiment defaults to OFF and reports its request separately from execution");
  namespace rate = renodx::addons::dlss5::output_framerate;
  namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
  {
    const auto* source_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xD3C044});
    auto [source, inserted] = runtime::features.try_emplace(source_handle);
    check(inserted, "the CPU sampling observation owns a distinct synthetic source");
    source->second.sr_sequence = 44;
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
  check(set("NREditReuse", true)["reason"] == "invalid_setting", "the existing x2 INI setting stays outside the DEV whitelist");
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
  for (const auto* operation : {"bridge_adaptive_experiment", "edit_reuse_adaptive_experiment"}) {
    const bool bridge = std::string_view(operation) == "bridge_adaptive_experiment";
    auto& requested_flag = (bridge ? runtime::bridge_adaptive_experiment : runtime::edit_reuse_adaptive_experiment);
    const auto* evidence_key = (bridge ? "bridge_adaptive_evidence" : "edit_reuse_adaptive_evidence");
    const auto boolean_experiment = [&](Json enabled) {
      return control::Execute({{"id", std::string(operation) + "-" + std::to_string(++serial)},
          {"op", operation}, {"enabled", std::move(enabled)}});
    };
    const auto boolean_identity = control::Identity();
    const auto boolean_settings = runtime::profile_ui::Snapshot(true);
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
            && boolean_enabled["state"]["experiments"][evidence_key] == "requested_enabled_not_activation",
        "Boolean enable publishes only the independent requested flag and its confirmation identity");
    check(control::Execute(boolean_request) == boolean_enabled, "same-id Boolean enable returns the original receipt");
    status = control::Execute({{"id", "query-" + std::string(operation)}, {"op", "status"}, {"query_id", boolean_request["id"]}});
    check(status["receipt"] == boolean_enabled, "lost Boolean enable receipts use the existing status lookup");
    auto reused_boolean = boolean_request;
    reused_boolean["enabled"] = false;
    check(control::Execute(reused_boolean)["reason"] == "request_id_reused", "reused Boolean ids cannot change the requested flag");
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
            && configuration_writes == experiment_writes && runtime::configuration_generation == experiment_generation
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
  Json receipt{{"scope", "CPU production configuration and status adapters with synthetic ReShade cache; no GPU, NGX or game"},
      {"checks", checks}, {"configuration_writes", configuration_writes}, {"final", control::Snapshot()}};
  std::ofstream("dev-control-runtime.json") << receipt.dump(2);
  configuration_fixture::use_cache = false;
  configuration_fixture::cache.clear();
  std::printf("PASS development control production configuration cases; checks=%u\n", checks);
}
