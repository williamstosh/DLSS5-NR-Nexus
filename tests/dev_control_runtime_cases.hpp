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
  Json receipt{{"scope", "CPU production configuration and status adapters with synthetic ReShade cache; no GPU, NGX or game"},
      {"checks", checks}, {"configuration_writes", configuration_writes}, {"final", control::Snapshot()}};
  std::ofstream("dev-control-runtime.json") << receipt.dump(2);
  configuration_fixture::use_cache = false;
  configuration_fixture::cache.clear();
  std::printf("PASS development control production configuration cases; checks=%u\n", checks);
}
