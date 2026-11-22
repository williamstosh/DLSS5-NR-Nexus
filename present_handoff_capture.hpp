/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#if defined(NR_NEXUS_DEV_CONTROL)

#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "report_bundle.hpp"
#include "screenshot.hpp"
#include "submission_tracker.hpp"

namespace renodx::addons::dlss5::present_handoff_capture {

enum class Producer : uint8_t { ORDINARY, XEFG };

struct Frame {
  Producer producer = Producer::ORDINARY;
  uint64_t output = 0;
  uint64_t context = 0;
  uint64_t epoch = 0;
  uint64_t app_frame = 0;
  bool real_guides = false;
  bool source_confirmed = false;
  uint64_t output_generation = 0;
  uint64_t call = 0;
  uint64_t source_sequence = 0;
};

struct Color {
  uint64_t source = 0;
  bool reset = false;
  uint32_t model_calls = 0;
  uint64_t reset_epoch = 0;
  uint64_t source_generation = 0;
  bool output_ready = false;
  bool group_complete = false;
  bool delivery_recorded = false;
};

namespace internal {

using Microsoft::WRL::ComPtr;
using Json = nlohmann::ordered_json;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

inline constexpr uint32_t MAX_FRAMES = 96;
inline constexpr uint32_t ROI_WIDTH = 128;
inline constexpr uint32_t ROI_HEIGHT = 64;
inline constexpr auto MAX_WINDOW = 10s;
inline constexpr auto MAX_TRIGGER_WAIT = 60s;
inline constexpr auto MAX_DRAIN = 5s;

enum class SampleState : uint8_t {
  RECORDING, CANCELLED, UNAVAILABLE, SUBMITTED, SIGNAL_LOST,
  COMPLETE, MAP_FAILED, DEVICE_REMOVED,
};

struct Sample {
  Frame frame;
  Color color;
  std::shared_ptr<submission::RecordingStatus> recording;
  ComPtr<ID3D12Resource> readback;
  ComPtr<ID3D12Fence> fence;
  D3D12_BOX region{};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 final_offset = 0;
  uint32_t row_bytes = 0;
  uint32_t subresource = 0;
  uintptr_t backbuffer = 0;
  uintptr_t queue_identity = 0;
  uintptr_t fence_identity = 0;
  uint64_t fence_value = 0;
  uint64_t completed_value = 0;
  Clock::time_point began;
  Clock::time_point submitted;
  Clock::time_point completed;
  SampleState state = SampleState::RECORDING;
  const char* reason = "recording";
  bool original_recorded = false;
  bool final_recorded = false;
  bool executed = false;
  bool signaled = false;
  std::vector<char> original;
  std::vector<char> final;
};

struct Batch {
  std::shared_mutex mutex;
  std::atomic_bool accepting{false};
  bool worker_active = false;
  bool wait_for_guides = false;
  bool stop_requested = false;
  const char* status = "idle";
  const char* reason = "none";
  std::string capture_id;
  uint32_t attempted = 0;
  uint32_t written = 0;
  uint32_t waiting_skipped = 0;
  std::optional<Frame> first_waiting_skip;
  Clock::time_point started;
  Clock::time_point deadline;
  std::array<Sample, MAX_FRAMES> samples;
};

inline Batch batch;
inline std::atomic_bool shutdown_requested{false};

// Both copies borrow the producer's BB on its existing private recording.
// Restore the caller's exact state so the remaining recording is unchanged.
inline void RecordCopy(const Sample& sample, ID3D12GraphicsCommandList* list,
                       ID3D12Resource* bb, D3D12_RESOURCE_STATES state, UINT64 offset) noexcept {
  D3D12_RESOURCE_BARRIER barrier{.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION,
      .Transition = {bb, sample.subresource, state, D3D12_RESOURCE_STATE_COPY_SOURCE}};
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    list->ResourceBarrier(1, &barrier);
  }
  auto footprint = sample.footprint;
  footprint.Offset = offset;
  const D3D12_TEXTURE_COPY_LOCATION destination{.pResource = sample.readback.Get(),
      .Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, .PlacedFootprint = footprint};
  const D3D12_TEXTURE_COPY_LOCATION source{.pResource = bb,
      .Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, .SubresourceIndex = sample.subresource};
  list->CopyTextureRegion(&destination, 0, 0, 0, &source, &sample.region);
  if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) {
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
  }
}

inline void StopLocked(const char* reason) noexcept {
  if (!batch.accepting.exchange(false, std::memory_order_acq_rel)) return;
  batch.status = "draining";
  batch.reason = reason;
}

// Cancellation retires only an irrevocably discarded recording. Execute with
// no healthy first Signal retains its buffers until the device is removed.
inline void ResolveSample(Sample* sample, bool collect_pixels) {
  if (!sample->readback || sample->state == SampleState::RECORDING) return;
  if (sample->recording && !sample->recording->closed.load()) return;
  if (sample->state == SampleState::CANCELLED && sample->recording && sample->recording->submitted.load()) {
    sample->state = SampleState::SIGNAL_LOST;
    sample->executed = true;
    sample->reason = "execution_receipt_missing";
  }
  if (sample->state == SampleState::SIGNAL_LOST) {
    ComPtr<ID3D12Device> device;
    if (FAILED(sample->readback->GetDevice(IID_PPV_ARGS(&device))) || SUCCEEDED(device->GetDeviceRemovedReason())) {
      return;
    }
  } else if (sample->state == SampleState::SUBMITTED) {
    sample->completed_value = sample->fence->GetCompletedValue();
    if (sample->completed_value < sample->fence_value) return;
    if (sample->completed_value == UINT64_MAX) {
      sample->state = SampleState::DEVICE_REMOVED;
      sample->reason = "device_removed";
    } else {
      sample->completed = Clock::now();
      sample->state = SampleState::COMPLETE;
      sample->reason = (sample->final_recorded ? "pair_complete" : "final_not_recorded");
      if (collect_pixels) {
        const auto width = sample->row_bytes;
        const auto height = sample->footprint.Footprint.Height;
        const auto pitch = sample->footprint.Footprint.RowPitch;
        sample->original.resize(size_t{width} * height);
        if (sample->final_recorded) {
          sample->final.resize(size_t{width} * height);
        }
        const D3D12_RANGE bytes{0, static_cast<SIZE_T>(sample->final_recorded
              ? sample->final_offset + UINT64{pitch} * height : UINT64{pitch} * height)};
        void* mapped = nullptr;
        if (FAILED(sample->readback->Map(0, &bytes, &mapped)) || mapped == nullptr) {
          sample->original.clear();
          sample->final.clear();
          sample->state = SampleState::MAP_FAILED;
          sample->reason = "readback_map_failed";
        } else {
          const auto* data = static_cast<const char*>(mapped);
          for (uint32_t row = 0; row < height; ++row) {
            std::memcpy(sample->original.data() + size_t{row} * width, data + size_t{row} * pitch, width);
            if (sample->final_recorded) {
              std::memcpy(sample->final.data() + size_t{row} * width,
                          data + sample->final_offset + size_t{row} * pitch, width);
            }
          }
          const D3D12_RANGE no_write{};
          sample->readback->Unmap(0, &no_write);
        }
      }
    }
  }
  sample->readback.Reset();
  sample->fence.Reset();
}

}  // namespace internal

class Capture {
 public:
  Capture() = default;
  Capture(const Capture&) = delete;
  Capture& operator=(const Capture&) = delete;
  ~Capture() {
    if (sample_ == nullptr) return;
    std::unique_lock lock(internal::batch.mutex);
    sample_->state = internal::SampleState::CANCELLED;
    sample_->reason = "not_executed";
  }

  [[nodiscard]] bool HasSample() const noexcept { return sample_ != nullptr; }

  void Begin(ID3D12GraphicsCommandList* list, ID3D12Resource* bb, uint32_t subresource,
             D3D12_RESOURCE_STATES state, const Frame& frame, std::optional<RECT> bounds = {}) noexcept {
    using namespace internal;
    if (sample_ != nullptr || !batch.accepting.load(std::memory_order_acquire)) return;
    std::unique_lock lock(batch.mutex);
    if (!batch.accepting.load(std::memory_order_relaxed)) return;
    const auto now = Clock::now();
    if (now >= batch.deadline) {
      StopLocked(batch.started == Clock::time_point{} ? "trigger_timeout" : "window_elapsed");
      return;
    }
    if (batch.started == Clock::time_point{}) {
      if (batch.wait_for_guides && !(frame.real_guides && frame.source_confirmed)) {
        ++batch.waiting_skipped;
        if (!batch.first_waiting_skip) batch.first_waiting_skip = frame;
        return;
      }
      batch.started = now;
      batch.deadline = now + MAX_WINDOW;
      batch.status = "capturing";
      batch.reason = "window_started";
    }
    auto& sample = batch.samples[batch.attempted++];
    if (batch.attempted == MAX_FRAMES) StopLocked("frame_limit");
    sample.frame = frame;
    sample.began = now;
    sample.subresource = subresource;
    sample.state = SampleState::UNAVAILABLE;
    sample.reason = "missing_backbuffer_or_list";
    if (list == nullptr || bb == nullptr) return;
    try {
      sample.backbuffer = reinterpret_cast<uintptr_t>(renodx::utils::directx::NativeIdentity(bb));
      // Observe the owner's existing generation without making the diagnostic
      // readback a production use that can block write permission or adoption.
      sample.recording = submission::ObserveRecording(list);
      const auto desc = bb->GetDesc();
      sample.footprint.Footprint.Format = desc.Format;
      uint32_t bytes_per_pixel = 4;
      switch (desc.Format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: bytes_per_pixel = 8; break;
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: break;
        default: sample.reason = "unsupported_backbuffer_format"; return;
      }
      sample.reason = "unsupported_backbuffer_subresource";
      if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1
          || desc.MipLevels == 0 || subresource >= uint32_t{desc.MipLevels} * desc.DepthOrArraySize) return;
      const auto mip = subresource % desc.MipLevels;
      const auto full_width = static_cast<LONG>(std::max(UINT64{1}, desc.Width >> mip));
      const auto full_height = static_cast<LONG>(std::max(1U, desc.Height >> mip));
      const RECT rect = bounds.value_or(RECT{0, 0, full_width, full_height});
      sample.reason = "invalid_backbuffer_region";
      if (rect.left < 0 || rect.top < 0 || rect.right > full_width || rect.bottom > full_height
          || rect.right <= rect.left || rect.bottom <= rect.top) return;
      const uint32_t width = std::min(ROI_WIDTH, static_cast<uint32_t>(rect.right - rect.left));
      const uint32_t height = std::min(ROI_HEIGHT, static_cast<uint32_t>(rect.bottom - rect.top));
      const uint32_t left = (static_cast<uint32_t>(rect.left) + (rect.right - rect.left - width) / 2);
      const uint32_t top = (static_cast<uint32_t>(rect.top) + (rect.bottom - rect.top - height) / 2);
      sample.region = {left, top, 0, left + width, top + height, 1};
      sample.row_bytes = width * bytes_per_pixel;
      const auto align = [](UINT64 size, UINT alignment) { return (size + alignment - 1) / alignment * alignment; };
      const auto pitch = static_cast<uint32_t>(align(sample.row_bytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT));
      sample.footprint.Footprint = {desc.Format, width, height, 1, pitch};
      sample.final_offset = align(UINT64{pitch} * height, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
      ComPtr<ID3D12Device> device;
      sample.reason = "readback_allocation_failed";
      if (FAILED(list->GetDevice(IID_PPV_ARGS(&device))) || !screenshot::internal::MakeReadbackBuffer(
              device.Get(), sample.final_offset + UINT64{pitch} * height, sample.readback.GetAddressOf(), false)) return;
      sample.state = SampleState::RECORDING;
      sample.reason = "recording";
      sample_ = &sample;
      list_ = list;
      bb_ = bb;
      RecordCopy(sample, list_, bb_, state, 0);
      sample.original_recorded = true;
    } catch (...) {
      sample.reason = "capture_recording_failed";
    }
  }

  void RecordFinal(D3D12_RESOURCE_STATES state, const Color& color) noexcept {
    if (sample_ == nullptr) return;
    std::unique_lock lock(internal::batch.mutex);
    if (sample_->final_recorded) return;
    internal::RecordCopy(*sample_, list_, bb_, state, sample_->final_offset);
    sample_->final_recorded = true;
    sample_->color = color;
  }

  // Called once after the real Execute and its existing private-pool Signal.
  // The diagnostic never submits a second Signal or substitutes a later value.
  void Submitted(ID3D12CommandQueue* queue, ID3D12Fence* fence, uint64_t value, bool signaled) noexcept {
    using namespace internal;
    if (sample_ == nullptr) return;
    std::unique_lock lock(batch.mutex);
    sample_->executed = true;
    sample_->submitted = Clock::now();
    sample_->queue_identity = reinterpret_cast<uintptr_t>(queue);
    sample_->fence_identity = reinterpret_cast<uintptr_t>(fence);
    sample_->fence_value = value;
    sample_->signaled = signaled && fence != nullptr && value != 0;
    if (sample_->signaled) {
      sample_->fence = fence;
      sample_->state = SampleState::SUBMITTED;
      sample_->reason = "fence_pending";
    } else {
      sample_->state = SampleState::SIGNAL_LOST;
      sample_->reason = "signal_unconfirmed";
    }
    sample_ = nullptr;
  }

 private:
  internal::Sample* sample_ = nullptr;
  ID3D12GraphicsCommandList* list_ = nullptr;
  ID3D12Resource* bb_ = nullptr;
};

namespace internal {

struct BatchRequest {
  std::filesystem::path run_directory;
  std::string capture_id;
  bool start_on_real_guides = false;
  HMODULE module = nullptr;
};

inline bool GuardStopped(const std::filesystem::path& run) {
  return std::filesystem::exists(run / L"capture-cancel.json")
      || std::filesystem::exists(run / L"guard-complete.json");
}

inline std::filesystem::path ValidateRun(const BatchRequest& request) {
  const auto& run = request.run_directory;
  auto project = run.parent_path();
  auto container = run.filename();
  while (project.has_relative_path() && project.filename() != L"RenoDX-DLSSNR-Nexus") {
    container = project.filename();
    project = project.parent_path();
  }
  if (project.filename() != L"RenoDX-DLSSNR-Nexus" || (container != L"tmp" && container != L"artifacts")
      || _wcsicmp(std::filesystem::canonical(run).c_str(), run.c_str()) != 0 || GuardStopped(run)) return {};
  const auto tmp = project / L"tmp";
  if (_wcsicmp(std::filesystem::canonical(tmp).c_str(), tmp.c_str()) != 0) return {};
  std::ifstream guard_file(run / L"protected-files.json");
  const auto guard = Json::parse(guard_file, nullptr, false);
  if (!guard.is_array() || guard.empty()) return {};
  std::ifstream owner_file(run / L"owned-process.json");
  const auto owner = Json::parse(owner_file, nullptr, false);
  FILETIME created{}, exited{}, kernel{}, user{};
  if (!owner.is_object() || owner.value("pid", 0u) != GetCurrentProcessId()
      || !GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return {};
  const uint64_t creation = (uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime;
  if (!owner.contains("created") || owner["created"] != creation) return {};
  const auto output = tmp / ("present-handoff-" + request.capture_id);
  if (std::filesystem::exists(output)) return {};
  return output;
}

inline Json FrameJson(const Frame& frame) {
  return {{"producer", frame.producer == Producer::XEFG ? "xefg" : "ordinary"},
      {"output", frame.output}, {"output_generation", frame.output_generation},
      {"context", frame.context}, {"epoch", frame.epoch}, {"app_frame", frame.app_frame}, {"call", frame.call},
      {"source_sequence", frame.source_sequence},
      {"real_guides", frame.real_guides}, {"source_confirmed", frame.source_confirmed}};
}

inline bool ExportBatch(const std::filesystem::path& output, const std::string& capture_id) {
  using Pixels = std::array<std::vector<char>, 2>;
  std::array<Pixels, MAX_FRAMES> pixels;
  constexpr std::array files{"original.raw", "final.raw"};
  constexpr std::array keys{"original", "final"};
  std::array<size_t, 2> offsets{};
  Json manifest{{"schema", "present_handoff_capture_v1"}, {"capture_id", capture_id},
      {"pid", GetCurrentProcessId()}, {"coverage", "private_present_recording_entries"},
      {"frames", Json::array()}};
  uint32_t written = 0;
  uint32_t attempted = 0;
  {
    std::unique_lock lock(batch.mutex);
    manifest["stop_reason"] = batch.reason;
    manifest["start_on_real_guides"] = batch.wait_for_guides;
    manifest["waiting_skipped"] = batch.waiting_skipped;
    manifest["first_waiting_skip"] = batch.first_waiting_skip ? FrameJson(*batch.first_waiting_skip) : Json(nullptr);
    attempted = batch.attempted;
    const auto ns = [](Clock::time_point time) {
      return std::chrono::duration_cast<std::chrono::nanoseconds>(time.time_since_epoch()).count();
    };
    for (uint32_t i = 0; i < attempted; ++i) {
      auto& sample = batch.samples[i];
      auto frame = FrameJson(sample.frame);
      const auto& footprint = sample.footprint.Footprint;
      const bool pair = !sample.original.empty() && !sample.final.empty();
      if (pair) ++written;
      frame.update(Json{{"sample", i + 1}, {"list", sample.recording ? sample.recording->list : 0},
          {"generation", sample.recording ? sample.recording->generation : 0},
          {"backbuffer", sample.backbuffer}, {"subresource", sample.subresource}, {"format_dxgi", footprint.Format},
          {"roi", {sample.region.left, sample.region.top, footprint.Width, footprint.Height}},
          {"row_bytes", sample.row_bytes}, {"source", sample.color.source}, {"source_generation", sample.color.source_generation},
          {"reset", sample.color.reset}, {"reset_epoch", sample.color.reset_epoch}, {"model_calls", sample.color.model_calls},
          {"output_ready", sample.color.output_ready}, {"group_complete", sample.color.group_complete},
          {"delivery_recorded", sample.color.delivery_recorded},
          {"began_ns", ns(sample.began)}, {"submitted_ns", ns(sample.submitted)}, {"completion_observed_ns", ns(sample.completed)},
          {"original_recorded", sample.original_recorded}, {"final_recorded", sample.final_recorded},
          {"executed", sample.executed}, {"signaled", sample.signaled}, {"fence_completed", sample.completed != Clock::time_point{}},
          {"queue", sample.queue_identity}, {"fence", sample.fence_identity}, {"fence_value", sample.fence_value},
          {"completed_value", sample.completed_value}, {"pair_written", pair},
          {"outcome", sample.state == SampleState::RECORDING ? "recording_pending_at_drain_deadline" : sample.reason}});
      pixels[i] = {std::move(sample.original), std::move(sample.final)};
      for (size_t p = 0; p < files.size(); ++p) {
        const auto size = pixels[i][p].size();
        frame[keys[p]] = size != 0 ? Json{{"file", files[p]}, {"offset", offsets[p]}, {"bytes", size}} : Json(nullptr);
        offsets[p] += size;
      }
      manifest["frames"].push_back(std::move(frame));
    }
  }
  manifest["pairs_written"] = written;
  std::string redacted;
  if (!report::RedactJson(manifest.dump(2), report::SystemRedaction(), &redacted)) return false;
  if (_wcsicmp(std::filesystem::canonical(output.parent_path()).c_str(), output.parent_path().c_str()) != 0
      || !std::filesystem::create_directory(output)) return false;
  struct PartialExport {
    const std::filesystem::path& directory;
    bool committed = false;
    ~PartialExport() {
      if (committed) return;
      std::error_code error;
      for (const auto* name : {L"original.raw", L"final.raw", L"capture.json"}) {
        std::filesystem::remove(directory / name, error);
      }
      std::filesystem::remove(directory, error);
    }
  } partial{output};
  for (size_t p = 0; p < files.size(); ++p) {
    if (offsets[p] == 0) continue;
    std::ofstream data(output / files[p], std::ios::binary | std::ios::trunc);
    for (uint32_t i = 0; i < attempted; ++i) {
      if (!pixels[i][p].empty()) {
        data.write(pixels[i][p].data(), pixels[i][p].size());
      }
    }
    data.close();
    if (data.fail()) return false;
  }
  std::ofstream metadata(output / L"capture.json", std::ios::binary | std::ios::trunc);
  metadata << redacted;
  metadata.close();
  if (metadata.fail()) return false;
  partial.committed = true;
  std::unique_lock lock(batch.mutex);
  batch.written = written;
  return true;
}

inline DWORD WINAPI WorkerMain(void* argument) noexcept {
  HMODULE module = nullptr;
  {
    const std::unique_ptr<BatchRequest> request(static_cast<BatchRequest*>(argument));
    module = request->module;
    try {
      const auto output = ValidateRun(*request);
      if (output.empty()) throw std::runtime_error("invalid_protected_run");
      {
        std::unique_lock lock(batch.mutex);
        batch.wait_for_guides = request->start_on_real_guides;
        const auto now = Clock::now();
        batch.deadline = now + (batch.wait_for_guides ? MAX_TRIGGER_WAIT : MAX_WINDOW);
        if (!batch.wait_for_guides) batch.started = now;
        batch.status = (batch.wait_for_guides ? "waiting_for_real_guides" : "capturing");
        batch.reason = (batch.wait_for_guides ? "waiting_for_confirmed_source" : "window_started");
        batch.accepting.store(true, std::memory_order_release);
        if (batch.stop_requested) StopLocked("explicit_stop");
      }
      while (batch.accepting.load(std::memory_order_acquire)) {
        const bool cancelled = GuardStopped(request->run_directory);
        {
          std::unique_lock lock(batch.mutex);
          if (shutdown_requested.load(std::memory_order_acquire) || cancelled) {
            StopLocked(cancelled ? "guard_stopped" : "shutdown");
          } else if (Clock::now() >= batch.deadline) {
            StopLocked(batch.started == Clock::time_point{} ? "trigger_timeout" : "window_elapsed");
          }
        }
        if (batch.accepting.load(std::memory_order_relaxed)) Sleep(50);
      }
      const auto drain_deadline = Clock::now() + MAX_DRAIN;
      for (;;) {
        bool pending = false;
        {
          std::unique_lock lock(batch.mutex);
          for (uint32_t i = 0; i < batch.attempted; ++i) {
            auto& sample = batch.samples[i];
            ResolveSample(&sample, true);
            pending |= (sample.state == SampleState::RECORDING || sample.state == SampleState::SUBMITTED
                || (sample.state == SampleState::CANCELLED && sample.readback));
          }
        }
        if (!pending || Clock::now() >= drain_deadline) break;
        Sleep(20);
      }
      const bool exported = ExportBatch(output, request->capture_id);
      std::unique_lock lock(batch.mutex);
      batch.status = (exported ? (batch.written != 0 && batch.written == batch.attempted ? "written" : "incomplete")
                               : "failed");
      if (!exported) batch.reason = "export_failed";
    } catch (...) {
      std::unique_lock lock(batch.mutex);
      StopLocked("capture_worker_failed");
      batch.reason = (batch.status == std::string_view("validating") ? "invalid_protected_run" : "capture_worker_failed");
      batch.status = "failed";
    }
    // Reporting deadlines stop CPU work on the batch, not the GPU's lease.
    // A lost production Signal is never made healthy by a later fence value.
    for (;;) {
      bool retained = false;
      {
        std::unique_lock lock(batch.mutex);
        for (uint32_t i = 0; i < batch.attempted; ++i) {
          auto& sample = batch.samples[i];
          ResolveSample(&sample, false);
          retained |= (sample.readback != nullptr);
        }
        if (!retained) {
          batch.worker_active = false;
          break;
        }
      }
      Sleep(250);
    }
  }
  FreeLibraryAndExitThread(module, 0);
}

}  // namespace internal

inline const char* Arm(const std::filesystem::path& run_directory, const std::string& capture_id,
                       bool start_on_real_guides) {
  using namespace internal;
  if (!run_directory.is_absolute() || run_directory != run_directory.lexically_normal()
      || run_directory.root_name().native().size() != 2
      || capture_id.empty() || capture_id.size() > 80
      || !std::all_of(capture_id.begin(), capture_id.end(), [](char c) {
           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
         })) return "invalid_request";
  auto request = std::make_unique<BatchRequest>(BatchRequest{
      .run_directory = run_directory, .capture_id = capture_id, .start_on_real_guides = start_on_real_guides});
  auto status_id = capture_id;
  std::unique_lock lock(batch.mutex);
  if (shutdown_requested.load(std::memory_order_acquire)) return "shutting_down";
  if (batch.worker_active) return "capture_busy";
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                          reinterpret_cast<LPCWSTR>(WorkerMain), &request->module)) return "worker_unavailable";
  batch.samples = {};
  batch.capture_id = std::move(status_id);
  batch.worker_active = true;
  batch.stop_requested = false;
  batch.wait_for_guides = start_on_real_guides;
  batch.first_waiting_skip.reset();
  batch.started = {};
  batch.attempted = batch.written = batch.waiting_skipped = 0;
  batch.status = "validating";
  batch.reason = "protected_run_validation";
  auto* raw_request = request.release();
  if (const auto thread = CreateThread(nullptr, 0, WorkerMain, raw_request, 0, nullptr)) {
    CloseHandle(thread);
    return "queued";
  }
  request.reset(raw_request);
  FreeLibrary(request->module);
  batch.worker_active = false;
  batch.status = "failed";
  batch.reason = "worker_unavailable";
  return batch.reason;
}

inline bool Stop(std::string_view capture_id) {
  std::unique_lock lock(internal::batch.mutex);
  if (!internal::batch.worker_active || internal::batch.capture_id != capture_id) return false;
  internal::batch.stop_requested = true;
  internal::StopLocked("explicit_stop");
  return true;
}

inline internal::Json Status() {
  std::shared_lock lock(internal::batch.mutex);
  const auto& batch = internal::batch;
  return {{"capture_id", batch.capture_id}, {"status", batch.status}, {"reason", batch.reason},
      {"accepting", batch.accepting.load(std::memory_order_relaxed)}, {"drained", !batch.worker_active},
      {"attempted", batch.attempted}, {"pairs_written", batch.written}, {"waiting_skipped", batch.waiting_skipped},
      {"start_on_real_guides", batch.wait_for_guides}};
}

inline void SignalShutdown() noexcept {
  internal::shutdown_requested.store(true, std::memory_order_release);
}

}  // namespace renodx::addons::dlss5::present_handoff_capture

#endif
