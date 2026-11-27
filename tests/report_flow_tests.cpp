/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#define DEBUG_LEVEL_0

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <dlssnr.hpp>

#define NR_NEXUS_TEST_OUTPUT_RUNTIME_ONLY
#include "adaptive_ui_cases.hpp"
#undef NR_NEXUS_TEST_OUTPUT_RUNTIME_ONLY
#include "d3d12_test_gpu.hpp"

namespace runtime = renodx::addons::dlss5::internal;
namespace flow = runtime::issue_report;
namespace capture = renodx::addons::dlss5::screenshot;
namespace report = renodx::addons::dlss5::report;
using Json = nlohmann::json;

namespace {
std::filesystem::path base_directory;
unsigned checks = 0;
bool copy_result = true;
INT_PTR browser_result = 33;
unsigned copy_calls = 0;
unsigned browser_calls = 0;
std::string copied_text;
std::wstring browser_url;

void Require(bool condition, const char* message) {
  ++checks;
  if (!condition) throw std::runtime_error(message);
}

void Write(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary);
  out.write(text.data(), static_cast<std::streamsize>(text.size()));
  Require(out.good(), "fixture write");
}

std::string Read(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  Require(in.good(), "fixture read");
  return {std::istreambuf_iterator<char>(in), {}};
}

void CheckPrivacy(std::string_view text) {
  for (const auto* secret : {"SYNTHETIC123", "query-value", "opaque-secret", "private-header"}) {
    Require(text.find(secret) == std::string_view::npos, "export contains synthetic secret");
  }
  const auto privacy = report::SystemRedaction();
  if (!privacy.profile.empty()) {
    Require(text.find(privacy.profile) == std::string_view::npos, "export contains profile path");
  }
}

bool FakeCopyDraft(std::string_view text) {
  ++copy_calls;
  copied_text = text;
  return copy_result;
}

HINSTANCE WINAPI FakeOpenBrowser(HWND window, LPCWSTR operation, LPCWSTR file,
                                LPCWSTR parameters, LPCWSTR directory, INT show) {
  ++browser_calls;
  Require(window == nullptr && parameters == nullptr && directory == nullptr, "browser receives URL without shell arguments");
  Require(operation != nullptr && std::wstring_view(operation) == L"open" && show == SW_SHOWNORMAL,
          "browser open boundary contract");
  Require(file != nullptr, "browser has URL");
  browser_url = file;
  return reinterpret_cast<HINSTANCE>(browser_result);
}

void Begin() {
  Require(runtime::BeginIssueReport("fixture"), "begin report");
  flow::test_reason = "fixture";  // Disable Explorer/browser; no automatic present callback is registered.
  flow::test_timeout_ms = 200;
  flow::host_setting = "none";
  flow::description = "中文 issue ghp_SYNTHETIC123";
  flow::reproduction = "First step\nAuthorization: Bearer private-header";
  flow::ticks = (1u << 9) - 1;
}

void Collect(const std::string& mode) {
  flow::capture_requested = mode == "written" || mode == "timeout" || mode == "no-nr-frame" || mode == "cancel-capture";
  flow::step = flow::Step::kCapturing;
  const auto request_before = capture::internal::request_id.load();
  std::thread publisher;
  if (mode == "timeout" || mode == "written") {
    publisher = std::thread([mode] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
      while (!capture::IsArmed() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
      if (!capture::IsArmed()) return;
      const auto serial = capture::internal::capture_serial.fetch_add(1) + 1;
      if (mode == "written") {
        capture::internal::PublishFinished({
            .serial = serial,
            .outcome = "written",
            .pre_image = base_directory / "Saved" / "pre.png",
            .post_image = base_directory / "Saved" / "post.png",
            .meta = base_directory / "Saved" / "capture.meta.txt",
        });
      }
    });
  }
  if (mode == "cancel-capture") {
    flow::cancel = true;
  }
  try {
    runtime::RunReportCaptureJob();
  } catch (...) {
    if (publisher.joinable()) publisher.join();
    throw;
  }
  if (publisher.joinable()) publisher.join();
  capture::Disarm();
  if (mode == "cancel-capture") {
    Require(flow::step == flow::Step::kClosed, "capture cancelled");
    return;
  }
  Require(flow::step == flow::Step::kReview, "collection enters review");
  Require(flow::stage.parent_path().filename() == ".cache", "staging isolated in .cache");
  const std::string expected = (mode == "written" ? "written" : mode == "timeout" ? "timeout"
                                               : mode == "no-nr-frame" ? "no_nr_frame" : "skipped");
  Require(flow::capture_result == expected, "capture outcome");
  if (!flow::capture_requested) {
    Require(capture::internal::request_id == request_before, "no screenshot option never arms capture");
  }
  const auto summary = Json::parse(flow::summary);
  Require(summary["diagnostics"]["extension_version"] == 2, "diagnostics extension version 2");
  for (const auto* field : {"nr", "native_fg", "xefg", "colour", "performance", "permissions"}) {
    Require(summary["diagnostics"].contains(field), "diagnostic extension field");
  }
  CheckPrivacy(flow::summary);
  for (const auto& item : flow::items) {
    if (!item.file.empty() && !item.image) {
      CheckPrivacy(Read(item.file));
    }
  }
}

void Save(const std::string& mode) {
  std::vector<std::filesystem::path> old_zips;
  const auto reports = base_directory / "DLSS5 Reports";
  if (mode == "retention") {
    for (int index = 0; index < 6; ++index) {
      const auto path = reports / ("renodx-dlss5-report-20000101-00000" + std::to_string(index) + ".zip");
      Write(path, "user saved archive");
      old_zips.push_back(path);
    }
  }
  Json excluded = Json::array();
  Json included = Json::array({"report.json"});
  for (auto& item : flow::items) {
    if (mode == "written" && item.image && item.entry.find("NR_ON") != std::string::npos) {
      item.include = true;
    }
    if (mode == "staged-edit" && !item.file.empty() && !item.image) {
      Write(item.file, "Edited after review: ghp_SYNTHETIC123\ntoken=opaque-secret\n"
                       "Authorization: Bearer private-header\n?token=query-value&fps=60\n"
                       "{\"credentials\":{\"value\":\"opaque-secret\"}}\n");
    }
    if (mode == "selection" && item.entry == "logs/ReShade.ini") {
      item.include = false;
    }
    if (!item.include) {
      excluded.push_back(item.entry);
    } else if (!item.required && item.include) {
      included.push_back(item.entry);
    }
  }
  if (mode == "selection") {
    Require(!excluded.empty(), "selection fixture has optional settings attachment");
  }
  if (mode == "json-privacy") {
    auto summary = Json::parse(flow::summary);
    summary["future_extension"] = {
        {"access_token", "opaque-secret"},
        {"message", "ghp_SYNTHETIC123 ?token=query-value&width=1920"},
        {"profile_path", report::SystemRedaction().profile + "\\test.txt"},
        {"nested", Json::array({true, 42, "中文"})},
    };
    flow::summary = summary.dump();
  }
  if (mode == "pack-failure") {
    flow::items.push_back({.label = "Missing fixture", .entry = "missing.txt",
                           .file = base_directory / "absent.txt", .bytes = 7});
  }
  flow::upload = false;
  flow::step = flow::Step::kPacking;
  const auto staging = flow::stage;
  runtime::RunReportSendJob();
  Require(flow::step == flow::Step::kDone, "send job completes");
  Require(!std::filesystem::exists(staging), "private staging removed");
  Require(flow::link.empty(), "local save has no uploaded link");
  Require(Read(base_directory / "Saved" / "pre.png") == "synthetic-original-pre", "original Saved capture retained");
  if (mode == "pack-failure") {
    Require(flow::zip.empty() && !flow::error.empty(), "packing failure surfaced without saved ZIP");
    return;
  }
  Require(!flow::zip.empty() && std::filesystem::is_regular_file(flow::zip), "local ZIP created without upload");
  for (const auto& path : old_zips) {
    Require(Read(path) == "user saved archive", "more than three old ZIPs retained byte for byte");
  }
  Write(base_directory / "archive-manifest.json", Json({
      {"archive", report::Utf8(flow::zip.wstring())}, {"included", included}, {"excluded", excluded},
      {"future_extension", mode == "json-privacy"},
  }).dump(2));
  runtime::CloseIssueReport();
  Require(std::filesystem::exists(reports), "closing done retains reports");
}

void TestSelectedReportDiagnostics() {
  namespace addon = renodx::addons::dlss5;
  namespace rate = addon::output_framerate;
  namespace timers = addon::gpu_timers;
  OutputRuntimeFixture runtime_a;
  const auto a = runtime_a.output;
  constexpr std::array<uintptr_t, 1> buffers_b{0xB100};
  rate::RegisterPresentOutput(0xB000, a.device, 0xB010, buffers_b);
  const auto b = rate::MatchPresentOutput(0xB000, buffers_b[0]);
  runtime::enabled = true;
  runtime::stack_passes = 1;
  runtime::hook_point = 2;
  timers::comparison_settings = runtime::comparison_ui::CaptureSettings;
  const int64_t now = runtime::SteadyNowNs();
  const int64_t start = now - 2'000'000'000;
  constexpr auto present = addon::stage_plan::Stage::PRESENT;
  constexpr auto upscaled = addon::stage_plan::Stage::UPSCALED;
  constexpr size_t present_index = static_cast<size_t>(present);
  constexpr size_t upscaled_index = static_cast<size_t>(upscaled);
  const addon::stage_plan::Plan plan{.pass_count = 1, .effective = {present}, .groups = {addon::stage_plan::Group{.stage = present, .count = 1}}, .group_count = 1};
  addon::gpu_comparison::Identity settings;
  runtime::comparison_ui::CaptureSettings(&settings);
  const auto make_sample = [&](const rate::OutputIdentity& output, uint64_t sequence,
                               uint32_t us, const char* path, uint32_t pass) {
    timers::Stats sample;
    sample.total_us = us;
    sample.sequence = sequence;
    sample.valid = true;
    sample.samples = 1;
    sample.state = timers::SampleState::VALID;
    sample.recorded_ns = now - 2;
    sample.measured_ns = now - 1;
    sample.identity = settings;
    sample.identity.device_session = timers::device_session;
    sample.identity.display_output_generation = output.generation;
    sample.identity.stable_epoch = runtime::comparison_stable_epochs[timers::StageIndex(path)];
    sample.passes[0] = {.id = pass, .width = 1920, .height = 1080, .output_width = 3840, .output_height = 2160, .eval_us = us / 2, .total_us = us, .state = timers::SampleState::VALID};
    strcpy_s(sample.path, path);
    return sample;
  };
  const auto render = [](const runtime::ReportOutputObservation& frozen) {
    Json diagnostics;
    runtime::AppendSelectedReportDiagnostics(diagnostics, frozen);
    return diagnostics;
  };
  addon::ui::CardInputs session;
  session.verdict.state = addon::NrState::kEngaged;
  session.verdict.injected = session.verdict.eligible = 120;
  session.active_features = 1;
  session.fg_submitted_frames = 7;
  {
    const addon::RuntimeLock lock(runtime::runtime_mutex);
    for (int64_t i = 0; i <= 200; ++i) {
      rate::ObservePresent(b, start + i * 10'000'000);
    }
    timers::stage_stats[present_index] = make_sample(b, 22, 9000, "present", 0);
    runtime::nr_cost_gpu_ms = 99.f;
    const auto pending = runtime::CopyReportDiagnostics(a, plan, now, settings, session);
    const auto card = runtime::SelectedReportCardJson(pending);
    Require(card["id"] == addon::ui::CardSlug(addon::ui::CardId::kStarting)
                && card["basis"] == "selected_output_at_request" && !card["present_before_fg"].get<bool>(),
            "A pending primary status cannot borrow process B engagement or FG delivery");
    const auto other = render(pending.output);
    Require(other["performance"]["gpu_chain_ms"]["value"].is_null()
                && other["performance"]["displayed_fps"]["value"].is_null()
                && other["colour"]["effective_domain"].is_null(),
            "A missing scoped diagnostics stay unset despite B timing and process colour/cost");
    const auto unknown = runtime::CopyReportDiagnostics({}, plan, now, settings, session);
    Require(render(unknown.output)["selected_output"] == "unknown"
                && runtime::SelectedReportCardJson(unknown)["basis"] == "selected_output_unknown",
            "unknown runtime does not acquire selected-output facts");
    timers::stage_stats[present_index] = make_sample(a, 11, 2500, "present", 0);
    for (int64_t i = 0; i <= 200; ++i) {
      rate::ObservePresent(a, start + i * 10'000'000);
    }
    auto mixed = plan;
    mixed.pass_count = 2;
    mixed.effective[1] = upscaled;
    mixed.groups[1] = {.stage = upscaled, .begin = 1, .count = 1};
    mixed.group_count = 2;
    timers::stage_stats[upscaled_index] = make_sample(a, 33, 1700, "postSR", 1);
    const auto groups = render(runtime::CopyReportDiagnostics(a, mixed, now, settings, session).output)["performance"];
    Require(groups["gpu_chain_ms"]["status"] == "per_stage_only"
                && groups["gpu_chain_ms"]["value"].is_null()
                && groups["gpu_groups_us"][0]["total_us"] == 2500
                && groups["gpu_groups_us"][0]["pass_begin"] == 1
                && groups["gpu_groups_us"][1]["total_us"] == 1700
                && groups["gpu_groups_us"][1]["pass_begin"] == 2,
            "independent stage measurements retain Pass and microseconds, never a fictitious sum");
    const auto frozen = runtime::CopyReportDiagnostics(a, plan, now, settings, session);
    const auto support = runtime::BuildSupportReport(frozen);
    Require(support.find("gpu_group: stage=Present passes=1-1 us=2500 sample=11") != std::string::npos
                && support.find("log_lines_basis: process session diagnostics") != std::string::npos,
            "text support report uses selected measurements and labels process log observations");
  }
  // The actual Collect action freezes before starting its real worker. Hold its
  // existing screenshot wait, replace the selected output, then release the wait.
  flow::typed_capture = true;
  flow::test_timeout_ms = 2000;
  runtime::ContinueIssueReport(true, &runtime_a);
  Require(flow::worker_active, "Collect action launched the production worker");
  const auto armed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!capture::IsArmed() && flow::worker_active && std::chrono::steady_clock::now() < armed_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Require(capture::IsArmed(), "request is waiting for its capture before worker serialization");
  {
    const addon::RuntimeLock lock(runtime::runtime_mutex);
    runtime::monitor_runtime = nullptr;
    rate::DestroyPresentOutput(a.swapchain);
    timers::stage_stats[present_index] = make_sample(b, 44, 9000, "present", 0);
    Require(!runtime::CaptureReportDiagnostics(&runtime_a).output.selected,
            "new request to destroyed output is unknown, not the reused pointer or B");
    runtime_a.buffers[0] = {buffers_b[0]};
    runtime::monitor_runtime = &runtime_a;
    Require(runtime::MatchRuntimeOutput(&runtime_a) == b,
            "the same live runtime now selects B without reviving destroyed output A");
  }
  flow::skip_capture = true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (flow::worker_active && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Require(!flow::worker_active && flow::step == flow::Step::kReview, "production worker finished without upload");
  const auto summary = Json::parse(flow::summary);
  const auto& performance = summary["diagnostics"]["performance"];
  Require(performance["gpu_chain_ms"]["value"] == 2.5
              && performance["gpu_groups_us"][0]["sample_sequence"] == 11
              && performance["displayed_fps"]["status"] == "application_present_estimate"
              && performance["displayed_fps"]["value"] == 100,
          "worker keeps request A timing and FPS after A destruction and a different B sample");
  Require(summary["diagnostics"]["colour"]["effective_domain"].is_null(),
          "no output-colour ownership cannot be filled from the global colour cache");
  Require(summary["card"]["basis"] == "selected_output_at_request", "JSON primary card preserves request scope");
  for (const auto& item : flow::items) {
    if (item.entry == "support-report.txt") {
      const auto support = Read(item.file);
      Require(support.find("us=2500 sample=11") != std::string::npos
                  && support.find("us=9000 sample=44") == std::string::npos,
              "worker text retains the same frozen A sample as JSON");
    }
  }
  CheckPrivacy(flow::summary);
  Save("no-capture");
}

void TestSelectedXefgReportDiagnostics() {
  namespace addon = renodx::addons::dlss5;
  namespace rate = addon::output_framerate;
  namespace fg = addon::fg;
  namespace xefg = fg::xefg;
  namespace adapter = xefg::internal;
  OutputRuntimeFixture request;
  const auto a_owner = request.output;
  constexpr std::array<uintptr_t, 1> buffers_b{0xB100};
  rate::RegisterPresentOutput(0xB000, a_owner.device, 0xB010, buffers_b);
  const auto b_owner = rate::FindPresentOwner(a_owner.device, buffers_b[0]);
  d3d12_test::Gpu gpu;
  const auto proxy_a = gpu.Swapchain();
  const auto proxy_b = gpu.Swapchain();

  // ReShade output matching and the SDK return are substituted boundaries.
  // Use real WARP backbuffers and the actual Begin/End status publication.
  const auto register_context = [&](IDXGISwapChain3* proxy, uintptr_t application) {
    const auto context = std::make_shared<adapter::Context>();
    context->queue = gpu.queue;
    context->device = gpu.device;
    context->device_identity = renodx::utils::directx::NativeIdentity(gpu.device.Get());
    context->proxy_identity = adapter::Identity(proxy);
    context->application_swapchain_identity = application;
    context->enabled = true;
    std::unique_lock lock(adapter::state.mutex);
    adapter::state.contexts.emplace(context.get(), context);
    adapter::RegisterOutput(context.get());
    return context;
  };
  const auto a = register_context(proxy_a.Get(), a_owner.swapchain);
  const auto b = register_context(proxy_b.Get(), 0xB010);
  runtime::enabled = true;
  runtime::d3d12_swapchain_seen = true;
  runtime::stack_passes = 1;
  runtime::hook_point = runtime::kHookPresent;
  runtime::monitor_status_output = {.owner = b_owner};
  runtime::intercepted_evaluations = 17;
  runtime::successful_evaluations = 5;
  runtime::submitted_injections = 5;
  adapter::requested = +[]() noexcept { return true; };
  Require(runtime::MatchRuntimeOutput(&request) == a->output_identity, "report runtime resolves to XeFG A, not the monitor default B");

  adapter::service = +[](const fg::FrameInput&) noexcept { return fg::Result::SUBMITTED; };
  auto b_frame = adapter::BeginPresent(proxy_b.Get(), 0);
  Require(b_frame.service_eligible && b_frame.service_result == fg::Result::SUBMITTED, "B reaches the production SDK return classification");
  adapter::EndPresent(&b_frame, E_FAIL);
  const auto default_status = runtime::ReadSelectedXefgStatus();
  Require(default_status && default_status->output == b->output_identity
              && default_status->DisplayOutcome() == fg::Result::FAILED,
          "default monitor B holds a true SDK failure");
  const auto a_pending = runtime::CaptureReportDiagnostics(&request);
  Require(a_pending.output.selected && a_pending.card.verdict.IsFgPending()
              && runtime::SelectedReportCardJson(a_pending)["id"] == "starting",
          "first owned A without a result stays grey even when default B failed");

  adapter::service = +[](const fg::FrameInput&) noexcept { return fg::Result::NOT_SERVED; };
  auto a_frame = adapter::BeginPresent(proxy_a.Get(), 0);
  Require(a_frame.service_eligible && a_frame.service_result == fg::Result::NOT_SERVED, "A has made its first real service attempt");
  adapter::EndPresent(&a_frame, S_OK);
  const auto a_refusal = xefg::ReadStatus(a->output_identity);
  Require(a_refusal && a_refusal->outcome == fg::Result::NOT_SERVED, "the actual A publication is a terminal refusal");

  flow::typed_capture = true;
  flow::test_timeout_ms = 2000;
  runtime::ContinueIssueReport(true, &request);
  const auto armed_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (!capture::IsArmed() && flow::worker_active && std::chrono::steady_clock::now() < armed_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Require(capture::IsArmed(), "worker holds a frozen A request while waiting for its screenshot");
  runtime::intercepted_evaluations = 29;
  runtime::successful_evaluations = 9;
  runtime::submitted_injections = 9;
  const auto a_output = a->output_identity;
  {
    std::unique_lock lock(adapter::state.mutex);
    adapter::ReleaseFrames(a.get());
    adapter::state.contexts.erase(a.get());
  }
  rate::DestroyPresentOutput(a_owner.swapchain);
  request.buffers[0] = {buffers_b[0]};
  Require(!rate::IsCurrentOutput(a_output) && runtime::MatchRuntimeOutput(&request) == b->output_identity,
          "request runtime now resolves to B after destroying A");
  const auto b_failed = runtime::CaptureReportDiagnostics(&request);
  Require(b_failed.card.verdict.reason != nullptr
              && std::string_view(b_failed.card.verdict.reason) == "fg_handoff_failed"
              && runtime::SelectedReportCardJson(b_failed)["tone"] == "warn",
          "a new B report retains the actual SDK failure as yellow, not the previous output's rejection");
  const auto old_b = b->output_identity;
  {
    std::unique_lock lock(adapter::state.mutex);
    adapter::ReleaseFrames(b.get());
    adapter::RegisterOutput(b.get());
  }
  const auto b_pending = runtime::CaptureReportDiagnostics(&request);
  Require(old_b != b->output_identity && b_pending.card.verdict.IsFgPending()
              && runtime::SelectedReportCardJson(b_pending)["tone"] == "idle",
          "a new B generation cannot inherit B's old yellow");
  flow::skip_capture = true;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (flow::worker_active && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  Require(!flow::worker_active && flow::step == flow::Step::kReview, "frozen A report completes its real worker");
  const auto summary = Json::parse(flow::summary);
  Require(summary["card"]["basis"] == "selected_output_at_request" && summary["card"]["id"] == "not_running"
              && summary["card"]["tone"] == "warn" && !summary["card"]["present_before_fg"].get<bool>(),
          "first real A refusal stays yellow without a fabricated delivery in the exported report");
  const auto& process = summary["diagnostics"]["process_status"];
  Require(process["basis"] == "session_diagnostics_not_selected_output"
              && process["injected"] == 5 && process["eligible"] == 17,
          "process diagnostics keep the request's independent session counters, not A's single-frame result or later counts");
  const auto support = std::ranges::find_if(flow::items, [](const auto& item) { return item.entry == "support-report.txt"; });
  Require(support != flow::items.end(), "worker writes the text report for the same snapshot");
  const auto support_text = Read(support->file);
  Require(support_text.find("card: Neural Rendering did not serve this frame") != std::string::npos
              && support_text.find("card: Neural Rendering submission failed") == std::string::npos,
          "support text retains A's exact refusal, never B's different failure or fresh state");
  CheckPrivacy(flow::summary);
  Save("no-capture");
  {
    std::unique_lock lock(adapter::state.mutex);
    adapter::ReleaseFrames(b.get());
    adapter::state.contexts.erase(b.get());
  }
  adapter::service = nullptr;
  adapter::requested = nullptr;
  runtime::monitor_status_output = {};
}

void TestIssueJob(const std::string& mode) {
  flow::test_reason.clear();
  std::strcpy(flow::typed, "中文 title ghp_SYNTHETIC123");
  std::strcpy(flow::typed_steps, "step one\ntoken=opaque-secret");
  flow::typed_capture = false;
  const auto request_before = capture::internal::request_id.load();
  runtime::ContinueIssueReport(false);
  Require(flow::step == flow::Step::kDone && !flow::worker_active, "skip diagnostics synchronous transition");
  Require(flow::summary.empty() && flow::public_summary.empty(), "skip diagnostics has no report data");
  Require(flow::stage.empty() && flow::zip.empty(), "skip diagnostics creates no files");
  Require(!flow::capture_requested && capture::internal::request_id == request_before, "skip diagnostics does not capture");
  Require(!flow::public_summary_consent && !flow::public_link_consent, "public consent defaults off");
  flow::test_reason = "fixture";
  flow::public_summary = "PRIVATE_DIAGNOSTIC_SENTINEL";
  flow::link = "https://example.invalid/PRIVATE_LINK_SENTINEL";
  flow::feature_request = mode == "issue-feature";
  flow::force_issue_fallback = mode == "issue-fallback";
  flow::public_summary_consent = mode == "issue-consent";
  flow::public_link_consent = mode == "issue-consent";
  flow::step = flow::Step::kOpeningIssue;
  runtime::RunReportIssueJob();
  Require(flow::step == flow::Step::kDone, "issue worker completes");
  Require(flow::issue_notice == "test_browser_suppressed", "test suppresses clipboard/browser");
  Require(!flow::issue_body.empty(), "issue worker prepares Markdown");
  CheckPrivacy(flow::issue_body);
  const bool consent = mode == "issue-consent";
  Require((flow::issue_body.find("PRIVATE") != std::string::npos) == consent, "public content requires separate consent");
  Require(flow::zip.empty() && flow::stage.empty(), "issue-only draft needs no ZIP or capture");
  runtime::CloseIssueReport();
}

void TestIssueBoundaries(const std::string& mode) {
  // Keep an actual production-written ZIP while injecting only the two OS
  // boundaries. The test reason continues to suppress all default OS actions.
  Save(mode);
  const auto zip = flow::zip;
  const auto zip_bytes = Read(zip);
  Require(flow::copy_draft == runtime::CopyReportText && flow::open_browser == ShellExecuteW,
          "production boundaries default to real platform functions");
  copy_result = mode != "boundary-clipboard-failure" && mode != "boundary-both-failure";
  browser_result = (mode == "boundary-browser-failure" || mode == "boundary-both-failure" ? 31 : 33);
  flow::force_issue_fallback = mode == "boundary-manual-fallback";
  flow::copy_draft = FakeCopyDraft;
  flow::open_browser = FakeOpenBrowser;
  flow::step = flow::Step::kOpeningIssue;
  runtime::RunReportIssueJob();
  flow::copy_draft = runtime::CopyReportText;
  flow::open_browser = ShellExecuteW;
  Require(copy_calls == 1 && browser_calls == 1, "both injected boundaries called once");
  Require(flow::step == flow::Step::kDone, "platform failure completes with manual recovery");
  const std::string expected = (browser_result <= 32 ? "browser_unavailable" : !copy_result ? "clipboard_unavailable"
      : flow::force_issue_fallback ? "manual_fallback" : "draft_opened");
  Require(flow::issue_notice == expected, "platform result selects visible notice");
  Require(!copied_text.empty() && flow::issue_body == copied_text, "full Markdown retained even when clipboard fails");
  CheckPrivacy(copied_text);
  Require(browser_url.starts_with(L"https://github.com/williamstosh/RenoDX-DLSSNR-Nexus/issues/new"),
          "browser failure still targets official repository");
  Require(browser_url.size() <= report::issue::MAX_URL_BYTES, "browser URL respects shell limit");
  if (flow::force_issue_fallback) {
    Require(report::Utf8(browser_url) == report::issue::NEW_ISSUE_URL, "manual fallback opens ordinary new issue");
  }
  Require(flow::zip == zip && Read(zip) == zip_bytes, "all platform outcomes preserve saved ZIP byte for byte");
  runtime::CloseIssueReport();
  Require(Read(zip) == zip_bytes, "closing platform error retains saved ZIP");
}
}  // namespace

extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char* message) {
  std::cout << "production: " << message << '\n';
}

// Only the ReShade host boundary is replaced; collectors, state machine,
// redaction, ZIP writer and diagnostics execute their production implementation.
extern "C" __declspec(dllexport) bool ReShadeGetBasePath(char* path, size_t* size) {
  const auto base = base_directory.string();
  if (!path) {
    *size = base.size() + 1;
  } else if (*size != 0) {
    const auto copied = std::min(*size - 1, base.size());
    std::memcpy(path, base.data(), copied);
    path[copied] = '\0';
    *size = copied;
  }
  return true;
}

extern "C" __declspec(dllexport) bool ReShadeGetConfigValue(
    void*, reshade::api::effect_runtime*, const char*, const char*, char*, size_t* size) {
  *size = 0;
  return false;
}

extern "C" __declspec(dllexport) void ReShadeSetConfigValue(
    void*, reshade::api::effect_runtime*, const char*, const char*, const char*) {}

int main(int argc, char** argv) {
  try {
    Require(argc == 2, "usage: report_flow_tests CASE (private, empty cwd required)");
    const std::string mode = argv[1];
    base_directory = std::filesystem::current_path();
    Require(std::filesystem::is_empty(base_directory), "flow fixture requires empty private cwd");
    reshade::internal::get_reshade_module_handle(GetModuleHandleW(nullptr));
    std::filesystem::create_directory(base_directory / "Saved");
    Write(base_directory / "Saved" / "pre.png", "synthetic-original-pre");
    Write(base_directory / "Saved" / "post.png", "synthetic-original-post");
    Write(base_directory / "Saved" / "capture.meta.txt", "token=opaque-secret\nwidth=1920");
    Write(base_directory / "ReShade.log", "ReShade version 'fixture'\nAuthorization: Bearer private-header\nghp_SYNTHETIC123");
    Write(base_directory / "ReShade.ini", "[Synthetic]\naccess_token=opaque-secret\nUrl=https://example.invalid/?token=query-value\n");
    Begin();
    if (mode == "cancel-describe") {
      runtime::CloseIssueReport();
      Require(flow::step == flow::Step::kClosed, "cancel description");
    } else if (mode == "repeat") {
      Require(!runtime::BeginIssueReport("duplicate"), "repeat begin refused");
      flow::worker_active = true;
      runtime::CloseIssueReport();
      Require(flow::step == flow::Step::kDescribe, "close cannot race active worker");
      flow::step = flow::Step::kReview;
      runtime::SendIssueReport(false);
      Require(flow::step == flow::Step::kReview, "repeat send refused while worker active");
      flow::worker_active = false;
      flow::step = flow::Step::kDescribe;
      runtime::CloseIssueReport();
      Require(runtime::BeginIssueReport("again"), "begin after close");
      runtime::CloseIssueReport();
    } else if (mode == "selected-output") {
      TestSelectedReportDiagnostics();
    } else if (mode == "selected-xefg-output") {
      TestSelectedXefgReportDiagnostics();
    } else if (mode == "test-environment") {
      Require(_putenv_s("RENODX_NR_TEST_REPORT", "fixture") == 0, "set test reason environment");
      for (const auto* url : {"https://example.invalid/upload", "http://127.0.0.1:1@evil.invalid/upload",
                              "http://localhost:1@evil.invalid/upload", "http://127.0.0.1.evil.invalid:1/upload", ""}) {
        Require(_putenv_s("RENODX_NR_TEST_REPORT_URL", url) == 0, "set synthetic test URL");
        Require(runtime::InitIssueReportTest(), "test environment recognized");
        report::ParsedReportUrl parsed;
        Require(report::ParseReportUrl(flow::test_url, &parsed) && parsed.loopback,
                "remote or absent test URL replaced by exact loopback refusal");
        Require(!flow::test_reason.empty(), "test browser suppression remains active");
      }
      Require(_putenv_s("RENODX_NR_TEST_REPORT", "") == 0, "remove test reason");
      Require(_putenv_s("RENODX_NR_TEST_REPORT_URL", "") == 0, "remove test URL");
      runtime::CloseIssueReport();
    } else if (mode.starts_with("issue-")) {
      TestIssueJob(mode);
    } else if (mode == "collect-failure") {
      Write(base_directory / "DLSS5 Reports", "block directory creation");
      flow::step = flow::Step::kCollecting;
      bool threw = false;
      try {
        runtime::RunReportCaptureJob();
      } catch (const std::exception&) {
        threw = true;
      }
      Require(threw && flow::step != flow::Step::kReview, "collection error propagated");
    } else {
      Collect(mode);
      if (mode == "cancel-capture") {
        Require(flow::zip.empty(), "cancel creates no archive");
      } else if (mode.starts_with("boundary-")) {
        TestIssueBoundaries(mode);
      } else if (mode == "cancel-review") {
        const auto staging = flow::stage;
        runtime::CloseIssueReport();
        // Review cancellation uses the production cleanup thread. ReShade is
        // the only substituted boundary; module references use real Win32 APIs.
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (flow::worker_active && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(!flow::worker_active, "cleanup worker completes within bound");
        Require(flow::step == flow::Step::kClosed && !std::filesystem::exists(staging), "cancel review removes staging");
        Require(std::filesystem::exists(base_directory / "Saved" / "pre.png"), "cancel retains source capture");
      } else if (mode == "upload-consent") {
        Require(!flow::upload_consent, "upload consent defaults off");
        flow::test_reason.clear();
        runtime::SendIssueReport(true);
        Require(flow::step == flow::Step::kReview && !flow::worker_active && !flow::upload,
                "upload without consent refused before worker/network");
        flow::test_reason = "fixture";
        flow::step = flow::Step::kDone;
        runtime::CloseIssueReport();
      } else if (mode == "test-empty-url") {
        flow::test_url.clear();
        // Keep configured hosts disabled even if a future regression removes
        // the production test-mode override; this fixture must stay local.
        flow::host_setting = "none";
        runtime::SendIssueReport(true);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (flow::worker_active && std::chrono::steady_clock::now() < deadline) {
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        Require(!flow::worker_active && flow::step == flow::Step::kDone, "test upload worker completed");
        report::ParsedReportUrl parsed;
        Require(report::ParseReportUrl(flow::test_url, &parsed) && parsed.loopback,
                "empty test override replaced by loopback refusal, never configured host");
        Require(flow::link.empty() && !flow::error.empty(), "refused upload has no link and visible failure");
        Require(!flow::zip.empty() && std::filesystem::exists(flow::zip), "failed test upload retains ZIP");
        runtime::CloseIssueReport();
      } else {
        Save(mode);
      }
    }
    std::cout << checks << " checks, 0 failures\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
