/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <windows.h>
#include <nlohmann/json.hpp>

#include "gpu_timers.hpp"
#include "report_bundle.hpp"
#include "stage_plan.hpp"

namespace renodx::addons::dlss5::control_mask_capture {

struct Context {
  char runtime_version[32] = {};
  uint32_t requested_model = 0;
  uint32_t stage = 1;  // stage_plan::Stage::UPSCALED
  bool mask_enabled = false;
  uint32_t mask_mode = 0;
  float mask_blend = 0;
  float mask_r = 0;
  float mask_g = 0;
  float mask_b = 0;
  bool auto_mask = false;
  float style = 0;
  float intensity = 0;
  float local_tone = 0;
  float sent_structure = 0;
  float sent_skin = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool source_generation_available = false;
  uint64_t source_generation = 0;
  // One successful Upscaled source write, including a cache-only write.
  bool inline_edit_reuse = false;
  uint64_t source_handle = 0;
  uint64_t source_sequence = 0;
  uint32_t reset_reasons = 0;
  bool api_returned = false;
  bool ngx_accepted = false;
  bool xefg_service = false;
  uint64_t application_frame = 0;
  uint64_t guide_serial = 0;
  uint64_t stream_epoch = 0;
  int64_t service_entry_ns = 0;
  double service_interval_ms = 0;
  bool service_interval_available = false;
  uint32_t model_evaluates = 0;
  bool refresh = false;
  bool reuse = false;
  bool output_ready = false;
};

enum class ExportState : uint8_t { NONE, PENDING, MARKER_WRITTEN, WRITTEN, FAILED };

struct Summary {
  uint64_t capture_id = 0;
  uint32_t target = 0;
  uint32_t capacity = 0;
  uint32_t recorded = 0;
  uint32_t valid = 0;
  uint32_t rejected = 0;
  uint32_t timeout = 0;
  uint32_t dropped = 0;
  uint32_t overflow = 0;
  uint32_t pending_stop = 0;
  uint32_t in_flight = 0;
  double window_seconds = 0;
  double warmup_seconds = 0;
  int64_t started_ns = 0;
  int64_t ended_ns = 0;
  bool window_timed_out = false;
  bool active = false;
  bool marker_written = false;
  ExportState export_state = ExportState::NONE;
  char reason[96] = {};
  char error[96] = {};
  bool xefg_service = false;
  bool inline_edit_reuse = false;
};

// Assigned before capture configuration; callback lifetime belongs to the existing screenshot worker.
inline bool (*wake_worker)() = nullptr;

namespace internal {
enum class RowState : uint8_t { PENDING, VALID, REJECTED, DROPPED, PENDING_AT_STOP };
struct Row {
  Context context;
  uint64_t token = 0;
  uint64_t timer_sequence = 0;
  int64_t recorded_ns = 0;
  RowState state = RowState::PENDING;
  gpu_timers::SampleState gpu_state = gpu_timers::SampleState::PENDING;
  gpu_timers::Stats stats;
};
struct State {
  std::shared_mutex mutex;
  Summary summary;
  std::vector<Row> rows;
  uint64_t next_token = 0;
  uint64_t next_capture = 0;
  uint64_t max_sequence = 0;
  int64_t window_ns = 0;
  int64_t warmup_ns = 0;
  bool armed = false;
  bool worker_busy = false;
  bool marker_needed = false;
  bool terminal_needed = false;
  std::filesystem::path marker_path;
  std::filesystem::path terminal_path;
};
static State state;
static std::atomic_bool enabled{false};
static std::atomic_bool capture_pending{false};
static std::atomic_bool work_pending{false};

template <size_t N>
inline void CopyText(char (&destination)[N], const char* source) {
  std::snprintf(destination, N, "%s", (source == nullptr ? "unspecified" : source));
}

// Caller owns the leaf lock. No callback, filesystem access or outer lock is acquired here.
inline void Finish(const char* reason, int64_t now_ns = gpu_timers::NowNs()) {
  auto& s = state;
  if (!s.summary.active) return;
  s.summary.active = false;
  s.summary.ended_ns = now_ns;
  s.armed = false;
  enabled.store(false, std::memory_order_relaxed);
  CopyText(s.summary.reason, reason);
  for (auto& row : s.rows) {
    if (row.state != RowState::PENDING) continue;
    row.state = RowState::PENDING_AT_STOP;
    ++s.summary.pending_stop;
  }
  s.summary.in_flight = 0;
  s.terminal_needed = true;
  work_pending.store(true, std::memory_order_release);
}

inline void Fail(const char* error) {
  auto& s = state;
  Finish("export_failed");
  s.armed = false;
  enabled.store(false, std::memory_order_relaxed);
  s.summary.export_state = ExportState::FAILED;
  CopyText(s.summary.error, error);
  s.marker_needed = false;
  s.terminal_needed = false;
  work_pending.store(false, std::memory_order_release);
  capture_pending.store(false, std::memory_order_release);
}

inline void Wake() {
  if (wake_worker == nullptr || !wake_worker()) {
    std::unique_lock lock(state.mutex);
    Fail("worker_unavailable");
  }
}

// CREATE_NEW protects both old exports and partial writes, including across processes.
inline bool WriteExclusive(const std::filesystem::path& path, const std::string& text) {
  struct File {
    HANDLE handle = INVALID_HANDLE_VALUE;
    ~File() {
      if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
      }
    }
  } file{CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr)};
  if (file.handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  if (text.size() > MAXDWORD
      || !WriteFile(file.handle, text.data(), static_cast<DWORD>(text.size()), &written, nullptr)
      || written != text.size() || !FlushFileBuffers(file.handle)) return false;
  const bool closed = CloseHandle(file.handle) != FALSE;
  file.handle = INVALID_HANDLE_VALUE;
  return closed;
}
}  // namespace internal

#if defined(NR_NEXUS_DEV_CONTROL)
namespace benchmark {

enum class Delivery : uint8_t { kOff, kPassthrough, kPartial, kPerFrame, kRefresh, kReuse, kFallbackRefresh };
enum class Stage : uint8_t { kUnobserved, kUpscaled, kRender };

struct Pass {
  uint32_t model = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t calls = 0;
  uint32_t accepted = 0;
  uint32_t reset_reasons = 0;
  uint32_t detail_stability = 0;
  bool look = false;
  bool detail_temporal = false;
  bool refresh_capture_fused = false;
  bool auto_mask = false;
  bool external_mask = false;
  float intensity = 0;
  float color_strength = 0;
  float transfer_strength = 0;
};

struct Frame {
  uint64_t token = 0;
  uintptr_t source_handle = 0;
  uint64_t source_generation = 0;
  uint64_t source_sequence = 0;
  int64_t sr_success_ns = 0;
  uint32_t phase = 0;
  uint32_t sr_result = 0;
  uint32_t input_width = 0;
  uint32_t input_height = 0;
  uint32_t output_width = 0;
  uint32_t output_height = 0;
  uint32_t output_format = 0;
  uint32_t motion_width = 0;
  uint32_t motion_height = 0;
  uint32_t passes_recorded = 0;
  uint32_t passes_requested = 0;
  bool nr_enabled = false;
  bool reuse_requested = false;
  bool expected_reuse_failed = false;
  bool original_selection_failed = false;
  Delivery delivery = Delivery::kPassthrough;
  Stage stage = Stage::kUnobserved;
  bool pre_sr_color_bound = false;
  bool original_color_restored = false;
  bool original_selection_recorded = false;
  bool nr_writeback_recorded = false;
  const char* fallback_reason = nullptr;  // Static diagnostic text.
  std::array<Pass, gpu_timers::kTimerMaxPasses> passes{};
  std::shared_ptr<submission::RecordingStatus> recording;
};

struct Row {
  Frame frame;
  gpu_timers::Stats timing;
  bool wrapper_finalized = false;
};

struct Source {
  uintptr_t handle;
  uint64_t generation;
  uint64_t attempts = 0;
  uint64_t failures = 0;
};

using Json = nlohmann::json;
struct State {
  std::shared_mutex mutex;
  uint64_t capture_id = 0;
  uint64_t last_token = 0;
  uint64_t first_token = 0;
  int64_t started_ns = 0;
  int64_t deadline_ns = 0;
  int64_t ended_ns = 0;
  int64_t drain_until_ns = 0;
  uint32_t capacity = 0;
  uint32_t frame_count = 0;
  std::vector<Source> sources;
  std::vector<Row> rows;
  Json phases = Json::array();
  std::array<int64_t, 16> phase_starts{};
  Json ended_environment;
  const char* status = "idle";
  const char* reason = "";
  std::string filename;
};

inline State state;
inline std::atomic_bool active{false};
inline std::atomic_bool write_pending{false};
inline std::atomic_bool draining{false};
inline std::atomic_uint64_t native_fg_calls{0};
inline std::atomic_uint64_t native_fg_successes{0};
inline thread_local Frame* current = nullptr;

inline Json Status() {
  std::shared_lock lock(state.mutex);
  return {{"capture_id", state.capture_id}, {"status", state.status}, {"reason", state.reason},
      {"filename", state.filename}, {"frames", state.frame_count}, {"started_ns", state.started_ns},
      {"ended_ns", state.ended_ns}, {"phase_index", state.phases.empty() ? 0 : state.phases.size() - 1},
      {"phase_begin_ns", state.phases.empty() ? 0 : state.phase_starts[state.phases.size() - 1]}};
}

inline bool Arm(uint32_t capacity, uint32_t seconds, Json environment, Json settings) {
  std::unique_lock lock(state.mutex);
  if (active.load() || draining.load() || write_pending.load() || std::string_view(state.status) == "writing") return false;
  state.rows.clear();
  state.rows.reserve(capacity);
  state.sources.clear();
  state.sources.reserve(8);
  state.started_ns = gpu_timers::NowNs();
  state.deadline_ns = state.started_ns + int64_t{seconds} * 1'000'000'000;
  state.phases = Json::array({{{"label", "unmarked"}, {"begin_ns", state.started_ns},
      {"settings", std::move(settings)}, {"fg", std::move(environment)}}});
  state.phase_starts[0] = state.started_ns;
  state.ended_environment = nullptr;
  state.ended_ns = 0;
  state.capacity = capacity;
  state.frame_count = 0;
  state.first_token = state.last_token + 1;
  state.filename = "sr-benchmark-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++state.capture_id) + ".json";
  state.reason = "";
  state.status = "recording";
  active.store(true);
  return true;
}

inline bool Mark(std::string label, Json environment, Json settings) {
  std::unique_lock lock(state.mutex);
  if (!active.load() || state.phases.size() >= state.phase_starts.size()) return false;
  const auto begin_ns = gpu_timers::NowNs();
  state.phase_starts[state.phases.size()] = begin_ns;
  state.phases.push_back({{"label", std::move(label)}, {"begin_ns", begin_ns},
      {"settings", std::move(settings)}, {"fg", std::move(environment)}});
  return true;
}

inline bool Stop(const char* reason, Json environment = nullptr, bool immediate = false) {
  std::unique_lock lock(state.mutex);
  if (!active.load() && !(immediate && draining.load())) return false;
  if (active.exchange(false)) {
    state.ended_ns = gpu_timers::NowNs();
    state.reason = reason;
    state.ended_environment = std::move(environment);
  }
  state.drain_until_ns = state.ended_ns + (immediate ? 0 : 2'000'000'000LL);
  state.status = "draining";
  draining.store(true);
  return true;
}

inline void Tick(int64_t now_ns, bool notify_worker = true) {
  if (active.load()) {
    bool timed_out = false;
    {
      std::shared_lock lock(state.mutex);
      timed_out = now_ns >= state.deadline_ns;
    }
    if (timed_out) Stop("window_timeout");
  }
  if (!draining.load()) return;
  {
    std::unique_lock lock(state.mutex);
    if (!draining.load() || now_ns < state.drain_until_ns) return;
    draining.store(false);
    state.status = "writing";
    write_pending.store(true);
  }
  if (notify_worker && (wake_worker == nullptr || !wake_worker())) {
    std::unique_lock lock(state.mutex);
    if (write_pending.exchange(false)) {
      state.status = "failed";
      state.reason = "benchmark_worker_unavailable";
    }
  }
}

// An inner NGX module may record NR before the owning wrapper sees the SR
// result. It can reserve a token, but only the owner can timestamp the row.
struct Scope;
inline thread_local Scope* current_scope = nullptr;

struct Scope {
  const uintptr_t source_handle;
  std::pair<uint64_t, uint64_t> recording_identity;
  Frame frame;
  Frame* previous = current;
  Scope* previous_scope = current_scope;

  Scope(uintptr_t handle, ID3D12CommandList* list)
      : source_handle(handle), recording_identity(submission::RecordingIdentity(list)) {
    current = nullptr;
    current_scope = this;
  }
  ~Scope() {
    current = previous;
    current_scope = previous_scope;
    if (frame.token == 0 || frame.sr_success_ns == 0) return;
    std::unique_lock lock(state.mutex);
    if (frame.token >= state.first_token && frame.token - state.first_token < state.rows.size()) {
      auto& row = state.rows[frame.token - state.first_token];
      row.frame = std::move(frame);
      row.wrapper_finalized = true;
    }
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

  bool MatchesRecording(ID3D12CommandList* list) const {
    return recording_identity.first != 0 && recording_identity == submission::RecordingIdentity(list);
  }

  // The Render timer is recorded before the real SR call can submit the list.
  Frame* ReserveRender(Frame observation, ID3D12CommandList* list) noexcept {
    try {
      if (observation.source_handle != source_handle || !MatchesRecording(list)) return nullptr;
      std::unique_lock lock(state.mutex);
      if (!active.load() || frame.token != 0) return nullptr;
      if (ReserveLocked(std::move(observation), list, true)) return &frame;
    } catch (...) {
      // A failed diagnostic allocation must not interrupt the game evaluate.
    }
    return nullptr;
  }

  void Observe(Frame observation, bool owner, bool success, ID3D12CommandList* list, bool track_queue) {
    if (!owner && (!success || frame.stage == Stage::kRender)) return;
    if (success && (!MatchesRecording(list)
                    || (frame.token != 0 && frame.source_generation != observation.source_generation))) {
      if (!owner) return;
      // The owner's original SR still succeeded. Any evidence from the
      // superseded recording stays provisional and is excluded from export.
      frame = {};
      current = nullptr;
      recording_identity = submission::RecordingIdentity(list);
    }
    std::unique_lock lock(state.mutex);
    if (observation.sr_success_ns < state.started_ns
        || (!active.load() && !(draining.load() && observation.sr_success_ns < state.ended_ns))) return;
    if (frame.token != 0 && (frame.token < state.first_token || frame.token - state.first_token >= state.rows.size())) return;
    if (owner) {
      auto source = std::find_if(state.sources.begin(), state.sources.end(), [&](const Source& candidate) {
        return candidate.handle == observation.source_handle && candidate.generation == observation.source_generation;
      });
      if (source == state.sources.end()) {
        if (state.sources.size() == 64) {
          lock.unlock();
          Stop("source_capacity_reached");
          return;
        }
        source = state.sources.insert(state.sources.end(), {observation.source_handle, observation.source_generation});
      }
      observation.source_sequence = ++source->attempts;
      if (!success) {
        ++source->failures;
        return;
      }
    }
    if (frame.token == 0 && !ReserveLocked(observation, list, track_queue)) {
      lock.unlock();
      Stop("frame_capacity_reached");
      return;
    }
    if (!owner) return;
    frame.sr_success_ns = observation.sr_success_ns;
    frame.sr_result = observation.sr_result;
    frame.source_sequence = observation.source_sequence;
    frame.phase = static_cast<uint32_t>(state.phases.size() - 1);
    while (frame.phase != 0 && frame.sr_success_ns < state.phase_starts[frame.phase]) {
      --frame.phase;
    }
    frame.input_width = observation.input_width;
    frame.input_height = observation.input_height;
    frame.output_width = observation.output_width;
    frame.output_height = observation.output_height;
    frame.output_format = observation.output_format;
    if (frame.stage == Stage::kRender && frame.delivery != Delivery::kPassthrough
        && (!frame.pre_sr_color_bound || !frame.original_color_restored)) {
      frame.delivery = Delivery::kPassthrough;
      frame.fallback_reason = (frame.pre_sr_color_bound ? "pre_sr_original_color_not_restored"
                                                       : "pre_sr_color_not_bound");
    }
    state.rows[frame.token - state.first_token].frame = frame;
    ++state.frame_count;
  }

 private:
  bool ReserveLocked(Frame observation, ID3D12CommandList* list, bool track_queue) {
    if (state.rows.size() == state.capacity) return false;
    if (track_queue) {
      observation.recording = submission::ObserveRecording(list);
      submission::TrackUse(list, observation.recording.get());
    }
    state.rows.emplace_back();
    frame = std::move(observation);
    frame.token = ++state.last_token;
    frame.sr_success_ns = 0;
    current = &frame;
    return true;
  }
};

inline void ObserveTiming(const gpu_timers::Stats& stats, uint64_t token) {
  std::unique_lock lock(state.mutex);
  if (token >= state.first_token && token - state.first_token < state.rows.size()) {
    state.rows[token - state.first_token].timing = stats;
  }
}

inline void Export(const std::filesystem::path& directory, const report::Redaction* redaction) noexcept {
  if (!write_pending.exchange(false)) return;
  const char* failure = "benchmark_export_failed";
  try {
    std::vector<Row> rows;
    std::vector<Source> sources;
    Json document;
    std::string filename;
    {
      std::unique_lock lock(state.mutex);
      rows.swap(state.rows);
      sources.swap(state.sources);
      filename = state.filename;
      document = {{"schema", "sr_benchmark_v1"}, {"status", "terminal"}, {"pid", GetCurrentProcessId()},
          {"capture_id", state.capture_id}, {"started_ns", state.started_ns}, {"ended_ns", state.ended_ns},
          {"time_basis", "steady_clock_ns; source timestamps are taken immediately after the outer NGX return"},
          {"gpu_scope", "NR start through Upscaled writeback or Render stand-in copy; source_gpu proves SR recording completion"},
          {"reason", state.reason}, {"phases", state.phases}, {"fg_at_stop", state.ended_environment}};
    }
    document["sources"] = Json::array();
    for (const auto& source : sources) {
      document["sources"].push_back({{"source_handle", source.handle}, {"source_generation", source.generation},
          {"attempts", source.attempts}, {"failures", source.failures}});
    }
    document["frames"] = Json::array();
    constexpr const char* deliveries[] = {"off", "passthrough", "partial", "per_frame", "refresh", "reuse", "fallback_refresh"};
    for (const auto& row : rows) {
      const auto& f = row.frame;
      if (f.sr_success_ns == 0) continue;
      const auto& t = row.timing;
      const char* completion = "unavailable";
      if (f.recording != nullptr) {
        std::lock_guard lock(submission::tracker_mutex);
        if (f.recording->completed_healthy) {
          completion = "complete";
        } else if (f.recording->closed.load() && !f.recording->submitted.load()) {
          completion = "discarded";
        } else {
          completion = "unproven";
        }
      }
      Json timing = {{"state", gpu_timers::StateText(t.state)}, {"sequence", t.sequence}, {"valid", t.valid},
          {"path", t.path[0] == '\0' ? Json(nullptr) : Json(t.path)}, {"recorded_ns", t.recorded_ns},
          {"samples", t.samples}, {"observed_ns", t.measured_ns}, {"total_us", t.valid ? Json(t.total_us) : Json(nullptr)},
          {"commit_us", t.valid ? Json(t.commit_us) : Json(nullptr)}, {"copies_us", t.valid ? Json(t.copies_us) : Json(nullptr)},
          {"group_edit_reused", t.group_edit_reused}};
      Json passes = Json::array();
      for (uint32_t i = 0; i < f.passes_recorded; ++i) {
        const auto& p = f.passes[i];
        const auto& g = t.passes[i];
        const bool timed = t.valid && g.state == gpu_timers::SampleState::VALID;
        passes.push_back({{"pass", i + 1}, {"model", p.model}, {"working_size", {p.width, p.height}},
            {"calls", p.calls}, {"accepted", p.accepted}, {"reset_reasons", p.reset_reasons},
            {"look", p.look}, {"detail_stability", p.detail_stability}, {"detail_temporal", p.detail_temporal},
            {"refresh_capture_fused", p.refresh_capture_fused},
            {"auto_mask", p.auto_mask}, {"external_mask", p.external_mask}, {"intensity", p.intensity},
            {"color_strength", p.color_strength}, {"transfer_strength", p.transfer_strength},
            {"model_values_source", p.calls != 0 ? "current_call" : (f.delivery == Delivery::kReuse ? "reuse_anchor" : "none")},
            {"gpu", timed ? Json{{"encode_us", g.encode_us}, {"eval_us", g.eval_us}, {"resolve_us", g.resolve_us},
                                 {"look_us", g.look_us}, {"total_us", g.total_us}} : Json(nullptr)}});
      }
      constexpr const char* stages[] = {nullptr, "upscaled", "render"};
      document["frames"].push_back({{"source_handle", f.source_handle}, {"source_generation", f.source_generation},
          {"source_sequence", f.source_sequence}, {"sr_success_ns", f.sr_success_ns}, {"sr_result", f.sr_result},
          {"phase_index", f.phase}, {"phase", document["phases"][f.phase]["label"]},
          {"sr_input_size", {f.input_width, f.input_height}}, {"sr_output_size", {f.output_width, f.output_height}},
          {"sr_output_format", f.output_format}, {"motion_size", {f.motion_width, f.motion_height}},
          {"nr_enabled", f.nr_enabled}, {"reuse_requested", f.reuse_requested},
          {"nr_writeback_recorded", f.nr_writeback_recorded},
          {"nr_stage", f.stage == Stage::kUnobserved ? Json(nullptr) : Json(stages[static_cast<size_t>(f.stage)])},
          {"pre_sr", f.stage == Stage::kRender ? Json{{"color_bound", f.pre_sr_color_bound},
              {"original_color_restored", f.original_color_restored},
              {"original_selection_recorded", f.original_selection_recorded}} : Json(nullptr)},
          {"delivery", row.wrapper_finalized ? Json(deliveries[static_cast<size_t>(f.delivery)]) : Json(nullptr)},
          {"fallback_reason", f.fallback_reason == nullptr ? Json(nullptr) : Json(f.fallback_reason)},
          {"passes_requested", f.passes_requested}, {"passes", std::move(passes)},
          {"recording", f.recording == nullptr ? Json(nullptr) : Json{f.recording->list, f.recording->generation}},
          {"source_gpu", completion}, {"nr_gpu", std::move(timing)}});
    }
    std::filesystem::create_directories(directory);
    const auto scrub = (redaction == nullptr ? report::SystemRedaction() : *redaction);
    std::string text;
    failure = "benchmark_redaction_failed";
    if (!report::RedactJson(document.dump(), scrub, &text)) throw 0;
    failure = "benchmark_write_failed_or_exists";
    if (!internal::WriteExclusive(directory / filename, text)) throw 0;
    std::unique_lock lock(state.mutex);
    state.status = "written";
  } catch (...) {
    std::unique_lock lock(state.mutex);
    state.status = "failed";
    state.reason = failure;
  }
}

}  // namespace benchmark
#endif

inline bool Enabled() { return internal::enabled.load(std::memory_order_relaxed); }
inline bool CapturePending() { return internal::capture_pending.load(std::memory_order_acquire); }
inline bool WorkPending() {
#if defined(NR_NEXUS_DEV_CONTROL)
  if (benchmark::write_pending.load()) return true;
#endif
  return internal::work_pending.load(std::memory_order_acquire);
}

inline Summary GetSummary() {
  std::shared_lock lock(internal::state.mutex);
  return internal::state.summary;
}

// Busy sessions (including a failed export) cannot be overwritten by reconfiguration.
inline bool Configure(uint32_t target = 0, double seconds = 30, double warmup = 0,
                      uint32_t capacity = 8192, bool xefg_service = false) {
  auto& s = internal::state;
  std::unique_lock lock(s.mutex);
  if (s.armed || s.summary.active || s.worker_busy || s.marker_needed || s.terminal_needed
      || s.summary.export_state == ExportState::FAILED) return false;
  if (!std::isfinite(seconds) || !std::isfinite(warmup) || seconds <= 0 || seconds > 3600
      || warmup < 0 || warmup >= seconds || capacity == 0 || capacity > 8192) return false;
  s.summary = Summary{
      .target = (std::min)(target, 4096u), .capacity = capacity,
      .window_seconds = seconds, .warmup_seconds = warmup, .xefg_service = xefg_service};
  s.rows.clear();
  s.max_sequence = 0;
  s.marker_path.clear();
  s.terminal_path.clear();
  s.window_ns = static_cast<int64_t>(seconds * 1e9);
  s.warmup_ns = static_cast<int64_t>(warmup * 1e9);
  s.armed = target != 0;
  internal::enabled.store(s.armed, std::memory_order_relaxed);
  return true;
}

inline void Stop(const char* reason = "stopped", bool notify_worker = true) {
#if defined(NR_NEXUS_DEV_CONTROL)
  if (benchmark::Stop(reason, nullptr, true)) benchmark::Tick(gpu_timers::NowNs(), notify_worker);
#endif
  bool wake = false;
  {
    auto& s = internal::state;
    std::unique_lock lock(s.mutex);
    s.armed = false;
    internal::enabled.store(false, std::memory_order_relaxed);
    wake = s.summary.active;
    internal::Finish(reason);
  }
  if (wake && notify_worker) {
    internal::Wake();
  }
}

inline void Tick(int64_t now_ns) {
  if (!Enabled()) return;
  bool wake = false;
  {
    auto& s = internal::state;
    std::unique_lock lock(s.mutex);
    if (s.summary.active && now_ns >= s.summary.started_ns
        && static_cast<uint64_t>(now_ns) - static_cast<uint64_t>(s.summary.started_ns)
            >= static_cast<uint64_t>(s.window_ns)) {
      ++s.summary.timeout;
      s.summary.window_timed_out = true;
      internal::Finish("window_timeout", now_ns);
      wake = true;
    }
  }
  if (wake) {
    internal::Wake();
  }
}

inline uint64_t Reserve(const Context& context, uint64_t timer_sequence, int64_t now_ns) {
  if (!Enabled() || (context.stage != static_cast<uint32_t>(stage_plan::Stage::UPSCALED)
                     && context.stage != static_cast<uint32_t>(stage_plan::Stage::PRESENT))) return 0;
  if (context.inline_edit_reuse && context.stage != static_cast<uint32_t>(stage_plan::Stage::UPSCALED)) return 0;
  Tick(now_ns);
  uint64_t token = 0;
  bool wake = false;
  {
    auto& s = internal::state;
    std::unique_lock lock(s.mutex);
    if (!s.armed || context.xefg_service != s.summary.xefg_service
        || (context.inline_edit_reuse && context.xefg_service)) return 0;
    if (s.summary.active && context.inline_edit_reuse != s.summary.inline_edit_reuse) return 0;
    if (!s.summary.active) {
      try {
        s.rows.reserve(s.summary.capacity);
      } catch (...) {
        internal::Fail("allocation_failed");
        return 0;
      }
      s.summary.capture_id = ++s.next_capture;
      s.summary.inline_edit_reuse = context.inline_edit_reuse;
      s.summary.started_ns = now_ns;
      s.summary.active = true;
      s.summary.export_state = ExportState::PENDING;
      s.marker_needed = true;
      internal::capture_pending.store(true, std::memory_order_release);
      internal::work_pending.store(true, std::memory_order_release);
      wake = true;
    }
    const bool warmed = now_ns >= s.summary.started_ns
        && static_cast<uint64_t>(now_ns) - static_cast<uint64_t>(s.summary.started_ns)
            >= static_cast<uint64_t>(s.warmup_ns);
    if (warmed && s.rows.size() >= s.summary.capacity) {
      ++s.summary.overflow;
      internal::Finish("buffer_full", now_ns);
      wake = true;
    } else if (warmed && s.summary.valid + s.summary.in_flight < s.summary.target
               && (timer_sequence == 0 || timer_sequence > s.max_sequence)) {
      token = ++s.next_token;
      auto frozen = context;
      frozen.runtime_version[sizeof(frozen.runtime_version) - 1] = '\0';
      s.rows.push_back(internal::Row{
          .context = frozen, .token = token, .timer_sequence = timer_sequence, .recorded_ns = now_ns});
      ++s.summary.recorded;
      ++s.summary.in_flight;
      if (timer_sequence != 0) {
        s.max_sequence = timer_sequence;
      }
    }
  }
  if (wake) {
    internal::Wake();
  }
  return token;
}

inline void Observe(const gpu_timers::Stats& stats, uint64_t token) {
  if (token == 0 || stats.state == gpu_timers::SampleState::PENDING) return;
  bool wake = false;
  {
    auto& s = internal::state;
    std::unique_lock lock(s.mutex);
    if (!s.summary.active) return;
    // Tokens are contiguous within the bounded row vector; old-session tokens cannot index it.
    if (s.rows.empty() || token < s.rows.front().token || token - s.rows.front().token >= s.rows.size()) return;
    auto& row = s.rows[static_cast<size_t>(token - s.rows.front().token)];
    if (row.token != token || row.state != internal::RowState::PENDING) return;
    row.gpu_state = stats.state;
    --s.summary.in_flight;
    const auto& context = row.context;
    bool output_valid = context.api_returned && context.ngx_accepted;
    if (context.xefg_service) {
      output_valid = context.output_ready;
    }
    if (context.inline_edit_reuse) {
      const bool calls_match = (context.reuse
          ? context.model_evaluates == 0 && !context.api_returned && !context.ngx_accepted
          : context.model_evaluates == 1 && context.api_returned && context.ngx_accepted);
      output_valid = context.output_ready && context.source_generation_available
          && context.source_handle != 0 && context.source_generation != 0 && context.source_sequence != 0
          && context.refresh != context.reuse && calls_match;
    }
    if (row.timer_sequence == 0) {
      row.state = internal::RowState::DROPPED;
      ++s.summary.dropped;
    } else if (stats.valid && stats.state == gpu_timers::SampleState::VALID && stats.samples == 1
                && stats.sequence == row.timer_sequence
                && output_valid) {
      row.state = internal::RowState::VALID;
      row.stats = stats;
      row.stats.queue = nullptr;
      ++s.summary.valid;
    } else {
      row.state = internal::RowState::REJECTED;
      ++s.summary.rejected;
      if (stats.state == gpu_timers::SampleState::EXPIRED) {
        ++s.summary.timeout;
      }
    }
    if (s.summary.valid == s.summary.target) {
      internal::Finish("target_reached");
    } else if (s.rows.size() == s.summary.capacity && s.summary.in_flight == 0) {
      ++s.summary.overflow;
      internal::Finish("buffer_full");
    }
    // Results are retained in preallocated memory until the terminal job. No per-frame disk IO.
    wake = s.terminal_needed;
  }
  if (wake) {
    internal::Wake();
  }
}

// Existing screenshot worker owns module lifetime and calls this outside its job mutex.
// The optional redaction is for synthetic CPU tests; production uses SystemRedaction().
inline void ProcessPending(const std::filesystem::path& directory, const report::Redaction* redaction = nullptr) noexcept {
#if defined(NR_NEXUS_DEV_CONTROL)
  benchmark::Export(directory, redaction);
#endif
  using Json = nlohmann::ordered_json;
  auto& s = internal::state;
  Summary summary;
  std::vector<internal::Row> rows;
  std::filesystem::path marker_path;
  std::filesystem::path terminal_path;
  bool marker = false;
  bool terminal = false;
  bool claimed = false;
  const char* error = "export_exception";
  try {
    {
      std::unique_lock lock(s.mutex);
      if (s.worker_busy || !internal::work_pending.load(std::memory_order_acquire)) return;
      s.worker_busy = true;
      claimed = true;
      summary = s.summary;
      marker = std::exchange(s.marker_needed, false);
      terminal = std::exchange(s.terminal_needed, false);
      if (terminal) {
        rows = std::move(s.rows);
      }
      marker_path = s.marker_path;
      terminal_path = s.terminal_path;
      internal::work_pending.store(false, std::memory_order_release);
    }
    const auto scrub = (redaction == nullptr ? report::SystemRedaction() : *redaction);
    if (marker) {
      const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
      const auto stem = "control-mask-" + std::to_string(GetCurrentProcessId()) + "-"
          + std::to_string(stamp) + "-" + std::to_string(summary.capture_id);
      marker_path = directory / (stem + ".incomplete.json");
      terminal_path = directory / (stem + ".json");
      std::string text;
      error = "marker_redaction_failed";
      if (!report::RedactJson(Json{{"schema", "control_mask_capture_v1"}, {"status", "incomplete"},
                                  {"capture_id", summary.capture_id}, {"target", summary.target},
                                  {"started_ns", summary.started_ns}}.dump(), scrub, &text)) throw 0;
      error = "marker_write_failed_or_exists";
      std::filesystem::create_directories(directory);
      if (!internal::WriteExclusive(marker_path, text)) throw 0;
      {
        std::unique_lock lock(s.mutex);
        s.marker_path = marker_path;
        s.terminal_path = terminal_path;
        s.summary.marker_written = true;
        s.summary.export_state = ExportState::MARKER_WRITTEN;
      }
    }
    if (terminal) {
      Json events = Json::array();
      for (const auto& row : rows) {
        const auto& c = row.context;
        const char* row_state = "pending_at_stop";
        switch (row.state) {
          case internal::RowState::VALID: row_state = "valid"; break;
          case internal::RowState::REJECTED: row_state = "rejected"; break;
          case internal::RowState::DROPPED: row_state = "dropped"; break;
          case internal::RowState::PENDING: row_state = "pending"; break;
          case internal::RowState::PENDING_AT_STOP: break;
        }
        Json event{{"token", row.token}, {"timer_sequence", row.timer_sequence}, {"recorded_ns", row.recorded_ns},
                   {"status", row_state}, {"gpu_state", gpu_timers::StateText(row.gpu_state)},
                   {"context", {{"runtime_version", c.runtime_version}, {"requested_model", c.requested_model},
                                {"stage", stage_plan::Name(static_cast<stage_plan::Stage>(c.stage))},
                                {"mask_enabled", c.mask_enabled}, {"mask_mode", c.mask_mode},
                                {"mask_blend", c.mask_blend}, {"mask_rgb", {c.mask_r, c.mask_g, c.mask_b}},
                                {"auto_mask", c.auto_mask}, {"style", c.style}, {"intensity", c.intensity},
                                {"local_tone", c.local_tone}, {"sent_structure", c.sent_structure}, {"sent_skin", c.sent_skin},
                                {"width", c.width}, {"height", c.height}, {"api_returned", c.api_returned},
                                {"ngx_accepted", c.ngx_accepted}, {"source_generation_available", c.source_generation_available}}}};
        if (c.source_generation_available) {
          event["context"]["source_generation"] = c.source_generation;
        }
        if (c.inline_edit_reuse) {
          event["inline_chain"] = {{"source_handle", c.source_handle}, {"source_sequence", c.source_sequence},
                                   {"reset_reasons", c.reset_reasons}, {"model_evaluates", c.model_evaluates},
                                   {"refresh", c.refresh}, {"reuse", c.reuse}, {"output_ready", c.output_ready}};
        }
        if (c.xefg_service) {
          event["context"] = {{"runtime_version", c.runtime_version}, {"requested_model", c.requested_model},
                              {"stage", stage_plan::Name(static_cast<stage_plan::Stage>(c.stage))},
                              {"width", c.width}, {"height", c.height}};
          event["xefg"] = {{"application_frame", c.application_frame}, {"guide_serial", c.guide_serial},
                           {"stream_epoch", c.stream_epoch}, {"service_entry_ns", c.service_entry_ns},
                           {"service_interval_available", c.service_interval_available},
                           {"model_evaluates", c.model_evaluates}, {"refresh", c.refresh},
                           {"reuse", c.reuse}, {"output_ready", c.output_ready}};
          if (c.service_interval_available) {
            event["xefg"]["service_interval_ms"] = c.service_interval_ms;
          }
        }
        if (row.state == internal::RowState::VALID) {
          const auto& t = row.stats;
          event["timing_us"] = {{"total", t.total_us}, {"own", t.own_us}, {"eval", t.eval_us},
                                {"encode", t.encode_us}, {"resolve", t.resolve_us}, {"look", t.look_us},
                                {"linearize", t.linearize_us}, {"commit", t.commit_us}, {"copies", t.copies_us}};
          event["samples"] = t.samples;
          if (c.xefg_service) {
            event["timing_us"]["service_total"] = t.service_total_us;
            event["timing_us"]["staging"] = t.staging_us;
            event["timing_us"]["writeback"] = t.writeback_us;
          }
          event["measured_ns"] = t.measured_ns;
          event["passes"] = Json::array();
          for (const auto& pass : t.passes) {
            if (pass.id == UINT32_MAX) continue;
            event["passes"].push_back({{"id", pass.id}, {"requested_model", pass.model},
                                       {"width", pass.width}, {"height", pass.height},
                                       {"output_width", pass.output_width}, {"output_height", pass.output_height},
                                       {"state", gpu_timers::StateText(pass.state)}, {"encode_us", pass.encode_us},
                                       {"eval_us", pass.eval_us}, {"resolve_us", pass.resolve_us},
                                       {"look_us", pass.look_us}, {"look_measured", pass.look_measured}, {"total_us", pass.total_us}});
          }
        }
        events.push_back(std::move(event));
      }
      Json document{{"schema", "control_mask_capture_v1"}, {"status", "terminal"}, {"capture_id", summary.capture_id},
                    {"time_basis", "started_ns/recorded_ns are CPU recording times; ended_ns/measured_ns are CPU stop/observation times on steady_clock, not absolute GPU timestamps"},
                     {"eval_scope", (summary.inline_edit_reuse
                         ? "eval span includes cache apply/composed motion or NR Evaluate; actual calls are inline_chain.model_evaluates"
                         : summary.xefg_service
                         ? "core eval span includes cache apply/composed motion or NR Evaluate; actual calls are xefg.model_evaluates"
                         : "mask_generation+NR_Evaluate; not an independent mask duration")},
                    {"count_scope", "Disabled, warmup, duplicate-reservation and target-drain evaluates are excluded from event counts. timeout counts EXPIRED rejections plus window expiry; window_timed_out identifies the latter. Terminal means export ended, not necessarily target reached."},
                    {"summary", {{"target", summary.target}, {"actual_valid", summary.valid}, {"rejected", summary.rejected},
                                 {"timeout", summary.timeout}, {"dropped", summary.dropped}, {"overflow", summary.overflow},
                                 {"pending_stop", summary.pending_stop}, {"recorded", summary.recorded},
                                 {"capacity", summary.capacity}, {"window_seconds", summary.window_seconds},
                                 {"warmup_seconds", summary.warmup_seconds}, {"reason", summary.reason},
                                 {"started_ns", summary.started_ns}, {"ended_ns", summary.ended_ns},
                                 {"window_timed_out", summary.window_timed_out}}},
                    {"events", std::move(events)}};
      if (summary.xefg_service) {
        document["scope"] = "xefg_single_backbuffer_service";
        document["service_timing_scope"] = "GPU service_total spans current-color staging through ProcessInline and final private-color writeback; total/eval retain their core-chain meanings; staging/writeback are the outer intervals";
        document["service_interval_scope"] = "CPU monotonic Service entry intervals between consecutive application frames in this stream; not display/generated-frame intervals or Service call durations";
      }
      if (summary.inline_edit_reuse) {
        document["scope"] = "native_upscaled_inline_chain";
        document["chain_timing_scope"] = "GPU total spans copy-in through the successful full source writeback, for refresh and reuse; model API fields remain false when no model was invoked";
        document["pairing_scope"] = "Pair only adjacent source_sequence values from the same source_handle/source_generation with refresh then reuse and zero reset_reasons; these are source-chain GPU costs, not output or scanout frame times";
      }
      std::string text;
      error = "terminal_redaction_failed";
      if (!report::RedactJson(document.dump(), scrub, &text)) throw 0;
      error = "terminal_write_failed_or_exists";
      if (terminal_path.empty() || !internal::WriteExclusive(terminal_path, text)) throw 0;
      // The start receipt remains as provenance. Only the separate terminal file proves a completed export.
      std::unique_lock lock(s.mutex);
      s.summary.export_state = ExportState::WRITTEN;
      internal::capture_pending.store(false, std::memory_order_release);
    }
    std::unique_lock lock(s.mutex);
    s.worker_busy = false;
  } catch (...) {
    if (!claimed) return;
    std::unique_lock lock(s.mutex);
    s.worker_busy = false;
    if (terminal) {
      s.rows = std::move(rows);
    }
    internal::Fail(error);
  }
}

}  // namespace renodx::addons::dlss5::control_mask_capture
