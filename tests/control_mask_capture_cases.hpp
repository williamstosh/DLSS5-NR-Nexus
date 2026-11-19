/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>

#include <control_mask_capture.hpp>

// Include after the CPU host's Check(bool, const char*) declaration.
namespace control_mask_capture_cases {
namespace cap = renodx::addons::dlss5::control_mask_capture;
namespace timers = renodx::addons::dlss5::gpu_timers;
namespace report = renodx::addons::dlss5::report;

inline uint32_t wakes = 0;
inline bool Wake() {
  ++wakes;
  // A callback may acquire the capture leaf lock: callers must already have released it.
  (void)cap::GetSummary();
  return true;
}

inline int Run() {
  struct EvidenceDirectory {
    std::filesystem::path path;
    bool owned = false;
  } temp;
  temp.path = std::filesystem::current_path()
      / ("nr-control-mask-cpu-" + std::to_string(GetCurrentProcessId()) + "-"
         + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  temp.owned = std::filesystem::create_directory(temp.path);
  Check(temp.owned, "capture creates a new synthetic temp directory");

  const report::Redaction redaction{.profile = "C:\\Users\\CaptureTest", .user = "CaptureTest"};
  cap::Context context{
      .requested_model = 7, .stage = 1, .mask_enabled = true, .mask_mode = 2,
      .mask_blend = 0.75f, .mask_r = 0.1f, .mask_g = 0.2f, .mask_b = 0.3f,
      .auto_mask = true, .style = 0.4f, .intensity = 0.5f, .local_tone = 0.6f,
      .sent_structure = 0.7f, .sent_skin = 0.8f, .width = 1920, .height = 1080,
      .source_generation_available = true, .source_generation = 91, .api_returned = true, .ngx_accepted = true};
  std::strcpy(context.runtime_version, "C:\\Users\\CaptureTest");
  const auto valid = [](uint64_t sequence) {
    return timers::Stats{
        .total_us = 110, .own_us = 10, .eval_us = 100, .encode_us = 3, .resolve_us = 7,
        .width = 1920, .height = 1080, .valid = true, .state = timers::SampleState::VALID,
        .sequence = sequence, .measured_ns = 123456, .samples = 1};
  };
  const auto read_json = [](const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return nlohmann::ordered_json::parse(input);
  };
  const auto terminal_for = [&](uint64_t id) {
    for (const auto& entry : std::filesystem::directory_iterator(temp.path)) {
      if (entry.path().filename().string().find(".incomplete.json") != std::string::npos) continue;
      if (entry.path().extension() == ".json" && read_json(entry.path()).value("capture_id", uint64_t{0}) == id) {
        return entry.path();
      }
    }
    return std::filesystem::path{};
  };
  const auto flush = [&] {
    cap::ProcessPending(temp.path, &redaction);
    Check(!cap::WorkPending(), "capture worker drains pending work");
  };
  const auto expect_written = [&] {
    flush();
    Check(cap::GetSummary().export_state == cap::ExportState::WRITTEN, "capture terminal export is confirmed");
    Check(!cap::CapturePending() && !cap::Enabled(), "capture is settled after terminal export");
    Check(!terminal_for(cap::GetSummary().capture_id).empty(), "capture terminal file exists");
  };
  // The test owns the singleton without a worker. Failed sessions deliberately require explicit disposal;
  // reset only test state so multiple independent failure fixtures can run in one process.
  const auto dispose_failed_fixture = [] {
    auto& s = cap::internal::state;
    std::unique_lock lock(s.mutex);
    Check(!s.worker_busy && s.summary.export_state == cap::ExportState::FAILED, "failed fixture is quiescent");
    s.summary = {};
    s.rows.clear();
  };

  Check(!cap::Enabled() && !cap::CapturePending() && !cap::WorkPending(), "capture defaults off");
  Check(cap::Reserve(context, 1, 0) == 0, "disabled capture reserves nothing");
  Check(cap::Configure(), "zero target keeps capture disabled");
  Check(!cap::Enabled(), "default configure remains off");
  cap::wake_worker = &Wake;
  Check(!cap::Configure(1, 0, 0) && !cap::Configure(1, 1, 1)
            && !cap::Configure(1, 1, 0, 0) && !cap::Configure(1, 1, 0, 8193), "capture bounds invalid inputs");

  Check(cap::Configure(1, 2, 0.1), "capture configures warmup");
  auto wrong_stage = context;
  wrong_stage.stage = 0;
  Check(cap::Reserve(wrong_stage, 1, 0) == 0 && !cap::CapturePending(), "Render cannot start the window");
  wrong_stage.stage = 3;
  Check(cap::Reserve(wrong_stage, 1, 0) == 0 && !cap::CapturePending(), "unknown stage cannot start the window");
  wakes = 0;
  Check(cap::Reserve(context, 1, 0) == 0, "first eligible evaluate starts warmup without a sample");
  Check(wakes == 1 && cap::WorkPending(), "start wakes the existing worker exactly once");
  Check(!cap::Configure(2, 2, 0), "armed session rejects busy reconfiguration");
  flush();
  Check(cap::GetSummary().marker_written && cap::GetSummary().export_state == cap::ExportState::MARKER_WRITTEN,
        "start writes incomplete receipt before completion");
  Check(cap::Reserve(context, 2, 50'000'000) == 0 && wakes == 1, "warmup causes no repeated wake or sample");
  const auto frozen_token = cap::Reserve(context, 3, 100'000'000);
  Check(frozen_token != 0 && cap::Reserve(context, 4, 100'000'001) == 0, "target bounds in-flight samples");
  auto changed = context;
  changed.mask_r = 0.9f;
  changed.requested_model = 8;
  changed.source_generation_available = false;
  // The caller can mutate its original snapshot after recording; the captured copy is independent.
  context.mask_mode = 9;
  auto pending = valid(3);
  pending.state = timers::SampleState::PENDING;
  cap::Observe(pending, frozen_token);
  Check(cap::GetSummary().in_flight == 1 && cap::GetSummary().valid == 0, "PENDING is not a completion");
  cap::Observe(valid(3), frozen_token);
  cap::Observe(valid(3), frozen_token);
  Check(cap::GetSummary().valid == 1 && cap::GetSummary().recorded == 1 && wakes == 2,
        "valid result is counted once and closes at exact target");
  expect_written();
  const auto frozen_output = read_json(terminal_for(cap::GetSummary().capture_id));
  const auto& event = frozen_output["events"][0];
  Check(event["context"]["mask_mode"] == 2 && event["context"]["requested_model"] == 7,
        "recorded context is frozen, never sampled at completion");
  Check(event["context"]["source_generation"] == 91 && !event.contains("game_frame"),
        "available source generation is exported without a synthetic game frame");
  Check(event["timing_us"]["total"] == 110 && event["samples"] == 1,
        "raw timing units and single-sample count are retained");
  Check(event["context"]["stage"] == "Upscaled", "Upscaled stage identity is retained");
  Check(frozen_output.dump().find("CaptureTest") == std::string::npos, "final JSON crosses RedactJson boundary");

  auto present_context = context;
  present_context.stage = static_cast<uint32_t>(renodx::addons::dlss5::stage_plan::Stage::PRESENT);
  for (uint32_t pass_count : {1u, 2u}) {
    Check(cap::Configure(1, 20, 6), "Present configures the real capture window and warmup");
    wakes = 0;
    cap::Tick(30'000'000'000);
    Check(!cap::CapturePending() && !cap::GetSummary().active && wakes == 0,
          "configuration and idle ticks do not start the Present clock");
    Check(cap::Reserve(present_context, 100, 30'000'000'000) == 0, "first Present starts warmup");
    Check(cap::GetSummary().started_ns == 30'000'000'000 && wakes == 1, "Present starts its clock at first evaluate");
    flush();
    Check(cap::GetSummary().marker_written && terminal_for(cap::GetSummary().capture_id).empty(),
          "Present writes a marker before any valid sample or terminal");
    Check(cap::Reserve(present_context, 101, 35'999'999'999) == 0, "Present excludes the full six-second warmup");
    const auto present_token = cap::Reserve(present_context, 102, 36'000'000'000);
    Check(present_token != 0 && cap::Reserve(present_context, 102, 36'000'000'001) == 0,
          "Present timer sequence reserves once across same-stage passes");
    auto present_stats = valid(102);
    for (uint32_t pass = 0; pass < pass_count; ++pass) {
      present_stats.passes[pass].id = pass;
      present_stats.passes[pass].state = timers::SampleState::VALID;
    }
    cap::Observe(present_stats, present_token);
    Check(cap::GetSummary().recorded == 1 && cap::GetSummary().valid == 1
              && std::string(cap::GetSummary().reason) == "target_reached" && wakes == 2,
          "Present token observes one completed segment and reaches the unchanged target");
    expect_written();
    const auto present_output = read_json(terminal_for(cap::GetSummary().capture_id));
    Check(present_output["events"].size() == 1 && present_output["events"][0]["context"]["stage"] == "Present"
              && present_output["events"][0]["passes"].size() == pass_count
              && present_output["summary"]["warmup_seconds"] == 6
              && present_output["summary"]["window_seconds"] == 20,
          "Present terminal preserves actual stage, per-pass timings and requested window");
  }

  auto xefg_context = present_context;
  xefg_context.xefg_service = true;
  xefg_context.application_frame = 120;
  xefg_context.guide_serial = 121;
  xefg_context.stream_epoch = 7;
  xefg_context.service_entry_ns = 9'000'000'000;
  xefg_context.service_interval_ms = 1000. / 30.;
  xefg_context.service_interval_available = true;
  xefg_context.api_returned = false;
  xefg_context.ngx_accepted = false;
  xefg_context.reuse = true;
  Check(cap::Configure(1, 1, 0, 8192, true), "dedicated XeFG capture reuses the bounded worker");
  Check(cap::Reserve(present_context, 1, 0) == 0 && !cap::CapturePending(),
        "ordinary core evaluates cannot consume the dedicated Service capture window");
  const auto failed_service_token = cap::Reserve(xefg_context, 1, 0);
  cap::Observe(valid(1), failed_service_token);
  Check(cap::GetSummary().rejected == 1 && cap::GetSummary().valid == 0,
        "GPU completion without a complete Service output remains rejected");
  xefg_context.output_ready = true;
  const auto service_token = cap::Reserve(xefg_context, 2, 1);
  auto service_stats = valid(2);
  service_stats.service_total_us = 160;
  service_stats.staging_us = 20;
  service_stats.writeback_us = 30;
  cap::Observe(service_stats, service_token);
  expect_written();
  const auto service_output = read_json(terminal_for(cap::GetSummary().capture_id));
  const auto& service_event = service_output["events"][1];
  Check(service_output["scope"] == "xefg_single_backbuffer_service"
            && service_event["xefg"]["application_frame"] == 120
            && service_event["xefg"]["guide_serial"] == 121
            && service_event["xefg"]["service_entry_ns"] == 9'000'000'000
            && service_event["xefg"]["service_interval_available"] == true
            && !service_event["context"].contains("style") && !service_event["context"].contains("mask_rgb"),
        "Service export retains its application identity and CPU entry interval scope");
  Check(service_event["xefg"]["model_evaluates"] == 0 && service_event["xefg"]["reuse"] == true
            && !service_event["context"].contains("ngx_accepted")
            && service_event["timing_us"]["total"] == 110 && service_event["timing_us"]["service_total"] == 160
            && service_event["timing_us"]["staging"] == 20 && service_event["timing_us"]["writeback"] == 30,
        "completed reuse exports full Service cost without inventing an NR call or NGX acceptance");

  auto inline_context = changed;
  inline_context.inline_edit_reuse = true;
  inline_context.source_generation_available = true;
  inline_context.source_generation = 2;
  inline_context.source_handle = 17;
  inline_context.source_sequence = 100;
  inline_context.output_ready = true;
  inline_context.refresh = true;
  inline_context.model_evaluates = 1;
  inline_context.api_returned = true;
  inline_context.ngx_accepted = true;
  Check(cap::Configure(3, 1, 0), "inline comparison uses the existing bounded capture");
  for (uint32_t frame = 0; frame < 3; ++frame) {
    inline_context.source_sequence = 100 + frame;
    inline_context.reuse = frame == 1;
    inline_context.refresh = !inline_context.reuse;
    inline_context.model_evaluates = (inline_context.reuse ? 0u : 1u);
    inline_context.api_returned = inline_context.ngx_accepted = !inline_context.reuse;
    const auto inline_token = cap::Reserve(inline_context, 200 + frame, frame);
    Check(inline_token != 0 && cap::Reserve(changed, 300 + frame, frame) == 0,
        "one capture cannot mix model-only rows into its inline source-chain scope");
    cap::Observe(valid(200 + frame), inline_token);
  }
  expect_written();
  const auto inline_output = read_json(terminal_for(cap::GetSummary().capture_id));
  const auto& reused_row = inline_output["events"][1];
  Check(inline_output["scope"] == "native_upscaled_inline_chain"
            && inline_output["summary"]["actual_valid"] == 3
            && inline_output["events"][0]["inline_chain"]["source_sequence"] == 100
            && reused_row["inline_chain"]["source_sequence"] == 101
            && inline_output["events"][2]["inline_chain"]["source_sequence"] == 102,
        "refresh/reuse/refresh all export one completed source-chain row in order");
  Check(reused_row["inline_chain"]["reuse"] && reused_row["inline_chain"]["model_evaluates"] == 0
            && !reused_row["context"]["api_returned"].get<bool>()
            && !reused_row["context"]["ngx_accepted"].get<bool>()
            && reused_row["timing_us"]["total"] == 110
            && reused_row["inline_chain"]["source_handle"] == 17
            && reused_row["context"]["source_generation"] == 2
            && inline_output["eval_scope"].get<std::string>().find("cache apply") != std::string::npos,
        "a completed cache-only write keeps actual false model API fields and a nonempty full-chain time");

  Check(cap::Configure(2, 1, 0), "inline invalid-result fixture configures");
  inline_context.refresh = false;
  inline_context.reuse = true;
  inline_context.api_returned = inline_context.ngx_accepted = false;
  inline_context.model_evaluates = 1;
  auto inline_token = cap::Reserve(inline_context, 210, 0);
  cap::Observe(valid(210), inline_token);
  inline_context.model_evaluates = 0;
  inline_token = cap::Reserve(inline_context, 211, 1);
  auto inline_replayed = valid(211);
  inline_replayed.state = timers::SampleState::REPLAYED;
  cap::Observe(inline_replayed, inline_token);
  inline_context.output_ready = false;
  inline_token = cap::Reserve(inline_context, 212, 2);
  cap::Observe(valid(212), inline_token);
  Check(cap::GetSummary().valid == 0 && cap::GetSummary().rejected == 3,
      "invented reuse calls, replayed GPU work and failed output cannot become completed inline samples");
  inline_context.output_ready = true;
  inline_context.stage = static_cast<uint32_t>(renodx::addons::dlss5::stage_plan::Stage::PRESENT);
  Check(cap::Reserve(inline_context, 213, 3) == 0, "native inline scope does not accept a Present row");
  inline_context.stage = static_cast<uint32_t>(renodx::addons::dlss5::stage_plan::Stage::UPSCALED);
  cap::Stop("inline_negative_cases_complete");
  expect_written();

  Check(cap::Configure(2, 1, 0), "capture can configure after successful export");
  auto token = cap::Reserve(changed, 10, 0);
  auto rejected = valid(10);
  rejected.state = timers::SampleState::REPLAYED;
  cap::Observe(rejected, token);
  token = cap::Reserve(changed, 11, 1);
  rejected = valid(11);
  rejected.samples = 2;
  cap::Observe(rejected, token);
  auto not_accepted = changed;
  not_accepted.ngx_accepted = false;
  token = cap::Reserve(not_accepted, 12, 2);
  cap::Observe(valid(12), token);
  not_accepted.ngx_accepted = true;
  not_accepted.api_returned = false;
  token = cap::Reserve(not_accepted, 13, 3);
  cap::Observe(valid(13), token);
  token = cap::Reserve(changed, 0, 4);
  cap::Observe(valid(0), token);
  token = cap::Reserve(changed, 14, 5);
  rejected = valid(14);
  rejected.state = timers::SampleState::EXPIRED;
  cap::Observe(rejected, token);
  Check(cap::GetSummary().rejected == 5 && cap::GetSummary().timeout == 1
            && cap::GetSummary().dropped == 1 && cap::GetSummary().valid == 0,
        "replay, aggregation, NGX failure, exception and missing timer cannot become valid");
  token = cap::Reserve(changed, 15, 6);
  cap::Observe(valid(15), token);
  Check(cap::Reserve(changed, 15, 7) == 0, "duplicate timer reservation is ignored");
  const auto pending_stop = cap::Reserve(changed, 16, 8);
  cap::Tick(1'000'000'000);
  const auto timed_out = cap::GetSummary();
  Check(timed_out.valid == 1 && timed_out.timeout == 2 && timed_out.window_timed_out && timed_out.pending_stop == 1
            && timed_out.ended_ns == 1'000'000'000,
        "window expires with actual valid count and explicit pending-at-stop");
  cap::Observe(valid(16), pending_stop);
  Check(cap::GetSummary().valid == 1, "late completion after timeout is ignored");
  expect_written();
  const auto timeout_output = read_json(terminal_for(cap::GetSummary().capture_id));
  Check(timeout_output["events"].back()["status"] == "pending_at_stop"
            && !timeout_output["events"].back().contains("timing_us"), "pending-at-stop has no fabricated timings");
  Check(!timeout_output["events"][0].contains("timing_us"), "rejected result has no timings");
  Check(!timeout_output["events"][0]["context"].contains("source_generation"),
        "unavailable generation is omitted rather than represented as a measured zero");
  Check(timeout_output.contains("eval_scope") && timeout_output.contains("count_scope")
            && timeout_output.contains("time_basis") && timeout_output["summary"]["ended_ns"] == 1'000'000'000,
        "export explains eval, exclusion and CPU time scopes");

  Check(cap::Configure(4097, 1, 0, 2) && cap::GetSummary().target == 4096, "target clamps to bounded 4096");
  token = cap::Reserve(changed, 20, 0);
  rejected = valid(20);
  rejected.valid = false;
  cap::Observe(rejected, token);
  token = cap::Reserve(changed, 21, 1);
  cap::Observe(valid(21), token);
  Check(cap::GetSummary().overflow == 1 && std::string(cap::GetSummary().reason) == "buffer_full",
        "bounded event capacity stops window explicitly");
  expect_written();

  Check(cap::Configure(2, 1, 0, 1), "small capacity fixture configures");
  token = cap::Reserve(changed, 22, 0);
  Check(cap::Reserve(changed, 23, 1) == 0 && cap::GetSummary().pending_stop == 1,
        "full in-flight buffer stops with pending-at-stop");
  cap::Observe(valid(22), token);
  expect_written();

  Check(cap::Configure(2, 1, 0), "stop fixture configures");
  token = cap::Reserve(changed, 30, 0);
  cap::Stop("shutdown");
  cap::Stop("duplicate_stop");
  Check(cap::GetSummary().pending_stop == 1 && std::string(cap::GetSummary().reason) == "shutdown",
        "shutdown preserves first reason and unresolved record");
  expect_written();
  Check(cap::Configure(1, 1, 0), "new window configures without replaying history");
  const auto newer_token = cap::Reserve(changed, 31, 0);
  cap::Observe(valid(30), token);
  Check(cap::GetSummary().valid == 0 && newer_token != token, "old session token cannot match a new session");
  cap::Observe(valid(999), newer_token);
  Check(cap::GetSummary().rejected == 1, "wrong sequence cannot borrow another completed sample");
  const auto final_token = cap::Reserve(changed, 32, 1);
  cap::Observe(valid(32), final_token);
  expect_written();

  Check(cap::Configure(2, 1, 0), "worker shutdown fixture configures");
  token = cap::Reserve(changed, 33, 0);
  const auto saved_wake = cap::wake_worker;
  cap::wake_worker = nullptr;
  cap::Stop("shutdown", /*notify_worker=*/false);
  Check(cap::WorkPending() && cap::GetSummary().pending_stop == 1
            && cap::GetSummary().export_state != cap::ExportState::FAILED,
        "a running worker stops without trying to wake its unavailable shutdown entry");
  expect_written();
  cap::wake_worker = saved_wake;

  Check(cap::Configure(1, 1, 0), "collision fixture configures");
  token = cap::Reserve(changed, 40, 0);
  flush();
  const auto marker_path = cap::internal::state.marker_path;
  const auto terminal_path = cap::internal::state.terminal_path;
  {
    std::ofstream collision(terminal_path);
    collision << "historical-data";
  }
  cap::Observe(valid(40), token);
  flush();
  Check(cap::GetSummary().export_state == cap::ExportState::FAILED && std::filesystem::exists(marker_path),
        "existing terminal refuses overwrite and retains incomplete receipt");
  {
    std::ifstream collision(terminal_path);
    std::string text{std::istreambuf_iterator<char>(collision), std::istreambuf_iterator<char>()};
    Check(text == "historical-data", "collision never truncates historical data");
  }
  Check(!cap::Configure(1, 1, 0), "failed export cannot silently overwrite the session");
  dispose_failed_fixture();

  Check(cap::Configure(1, 1, 0), "write failure fixture configures");
  token = cap::Reserve(changed, 50, 0);
  const auto not_directory = temp.path / "not-a-directory";
  {
    std::ofstream file(not_directory);
    file << "synthetic";
  }
  cap::ProcessPending(not_directory, &redaction);
  Check(cap::GetSummary().export_state == cap::ExportState::FAILED && !cap::WorkPending()
            && !cap::Enabled(), "write failure is explicit and does not claim an export");
  dispose_failed_fixture();

  cap::wake_worker = nullptr;
  Check(cap::Configure(1, 1, 0), "worker-unavailable fixture configures");
  (void)cap::Reserve(changed, 60, 0);
  Check(cap::GetSummary().export_state == cap::ExportState::FAILED
            && std::string(cap::GetSummary().error) == "worker_unavailable", "missing worker fails explicitly without IO");
  dispose_failed_fixture();
  Check(cap::Configure(), "test restores disabled state");
  return 0;
}
}  // namespace control_mask_capture_cases
