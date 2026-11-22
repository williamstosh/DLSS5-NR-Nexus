/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>

// Including output_framerate_registration_cases.hpp here would form a cycle:
// its registration suite needs nr_edit_reuse_present_cases::Swapchain, and
// that suite uses this GPU suite's CopySr. The executable defines the shared
// registrar and dispatcher once through that later registration suite.
namespace {
void RegisterOutputSampling();
void UnregisterOutputSampling();
}  // namespace

namespace output_framerate_registration_cases {
template <reshade::addon_event Event, typename... Args>
void Dispatch(Args... args);
}  // namespace output_framerate_registration_cases

namespace nr_edit_reuse_gpu_cases {
namespace adaptive = renodx::addons::dlss5::adaptive_resolution;
namespace rate = renodx::addons::dlss5::output_framerate;
namespace sdk = renodx::addons::dlss5::fg::native_options;
namespace timers = renodx::addons::dlss5::gpu_timers;
using output_framerate_registration_cases::Dispatch;

// ReShade faces for an ordinary output backed by the same real WARP texture
// that the game SR substitute and NR write. No compositor or SDK is needed.
struct AdaptivePresentDevice final : nr::test::vk::DeviceApiStubs {
  ID3D12Device* native = nullptr;
  uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(native); }
  reshade::api::device_api get_api() const override { return reshade::api::device_api::d3d12; }
};

struct AdaptivePresentQueue final : reshade::api::command_queue {
  AdaptivePresentDevice* device = nullptr;
  ID3D12CommandQueue* native = nullptr;
  uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(native); }
  reshade::api::device* get_device() override { return device; }
  reshade::api::command_queue_type get_type() const override { return reshade::api::command_queue_type::graphics; }
  void get_private_data(const uint8_t[16], uint64_t*) const override {}
  void set_private_data(const uint8_t[16], uint64_t) override {}
  void wait_idle() const override {}
  void flush_immediate_command_list() const override {}
  reshade::api::command_list* get_immediate_command_list() override { return nullptr; }
  void begin_debug_event(const char*, const float[4]) override {}
  void end_debug_event() override {}
  void insert_debug_marker(const char*, const float[4]) override {}
  bool wait(reshade::api::fence, uint64_t) override { return false; }
  bool signal(reshade::api::fence, uint64_t) override { return false; }
  uint64_t get_timestamp_frequency() const override { return 0; }
};

struct AdaptivePresentOutput final : reshade::api::swapchain {
  AdaptivePresentDevice device;
  AdaptivePresentQueue queue;
  ID3D12Resource* buffer;
  IDXGISwapChain* native_swapchain = nullptr;

  AdaptivePresentOutput(const Gpu& gpu, ID3D12Resource* buffer, IDXGISwapChain* native_swapchain = nullptr)
      : buffer(buffer), native_swapchain(native_swapchain) {
    device.native = gpu.device.Get();
    queue.device = &device;
    queue.native = gpu.queue.Get();
    Dispatch<reshade::addon_event::init_swapchain>(this, false);
    Check(static_cast<bool>(Identity()), "registered ordinary output owns the actual SR writeback texture");
  }
  ~AdaptivePresentOutput() {
    Dispatch<reshade::addon_event::destroy_swapchain>(this, false);
  }
  rate::OutputIdentity Identity() {
    return rate::MatchPresentOutput(reinterpret_cast<uintptr_t>(this), get_back_buffer(0).handle);
  }
  void Present() {
    Dispatch<reshade::addon_event::present>(&queue, this, nullptr, nullptr, 0u, nullptr);
  }
  uint64_t get_native() const override { return reinterpret_cast<uintptr_t>(native_swapchain); }
  void get_private_data(const uint8_t[16], uint64_t*) const override {}
  void set_private_data(const uint8_t[16], uint64_t) override {}
  reshade::api::device* get_device() override { return &device; }
  void* get_hwnd() const override { return nullptr; }
  reshade::api::resource get_back_buffer(uint32_t) override { return {reinterpret_cast<uintptr_t>(buffer)}; }
  uint32_t get_back_buffer_count() const override { return 1; }
  uint32_t get_current_back_buffer_index() const override { return 0; }
  bool check_color_space_support(reshade::api::color_space space) const override {
    return space == reshade::api::color_space::scrgb;
  }
  reshade::api::color_space get_color_space() const override { return reshade::api::color_space::scrgb; }
};

struct AdaptivePresentRegistration {
  AdaptivePresentRegistration() { RegisterOutputSampling(); }
  ~AdaptivePresentRegistration() { UnregisterOutputSampling(); }
};

void SetAdaptiveOutputMode(AdaptivePresentOutput* output, uint32_t viewport, sl::DLSSGMode mode = sl::DLSSGMode::eOff) {
  funnel_accounting_tests::Restore substitutions;
  substitutions.Value(&native::real_streamline_set_tag);
  substitutions.Value(&sdk::internal::state.real);
  substitutions.Atomic(&sdk::internal::stopped);
  native::real_streamline_set_tag = +[](const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*) {
    return sl::Result::eOk;
  };
  sdk::internal::state.real = +[](const sl::ViewportHandle&, const sl::DLSSGOptions&) { return sl::Result::eOk; };
  sdk::internal::stopped = false;
  sl::Resource buffer(sl::ResourceType::eTex2d, output->buffer);
  const sl::ResourceTag tag(&buffer, sl::kBufferTypeBackbuffer, sl::ResourceLifecycle::eValidUntilPresent);
  const sl::ViewportHandle handle(viewport);
  sl::DLSSGOptions options;
  options.mode = mode;
  Check(native::HookedStreamlineSetTag(handle, &tag, 1, nullptr) == sl::Result::eOk
            && sdk::internal::HookedSetOptions(handle, options) == sl::Result::eOk
            && rate::NativeMode(output->Identity()) == static_cast<uint32_t>(mode),
        "successful SDK tag and Options bind the declaration to this registered output");
}

struct AdaptiveGpuSubmission {
  Gpu* gpu;

  void ResetList() const {
    Check(SUCCEEDED(gpu->allocator->Reset()) && SUCCEEDED(gpu->list->Reset(gpu->allocator.Get(), nullptr)),
          "reset the closed adaptive source recording");
  }

  void Complete(timers::Segment* segment, uint32_t cost_us) const {
    Check(SUCCEEDED(gpu->list->Close()), "close the adaptive source recording");
    ID3D12CommandList* lists[]{gpu->list.Get()};
    gpu->queue->ExecuteCommandLists(1, lists);
    gpu->submission_pending = true;
    Check(SUCCEEDED(gpu->queue->Signal(gpu->fence.Get(), ++gpu->serial)), "signal adaptive WARP completion");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Check(event != nullptr, "create the adaptive completion event");
    const auto armed = gpu->fence->SetEventOnCompletion(gpu->serial, event);
    const auto wait = (SUCCEEDED(armed) ? WaitForSingleObject(event, 10000) : WAIT_FAILED);
    CloseHandle(event);
    Check(wait == WAIT_OBJECT_0 && SUCCEEDED(gpu->device->GetDeviceRemovedReason()), "wait for healthy adaptive GPU completion");
    gpu->submission_pending = false;
    native::ResolveAdaptiveBorrowedDeliveries();
    ResetList();
    native::DrainRetiredResources(true);

    // Retain the real query coverage, exact queue receipt and delivery test.
    // Scale only the completed timestamp span to make the comparative costs repeatable.
    if (segment != nullptr && segment->measure && cost_us == 0) {
      const auto sequence = segment->sequence;
      const auto delivery = segment->adaptive.delivery;
      // Preserve the independent completion receipt while withholding timestamps.
      segment->measure = false;
      timers::Poll(true, false);
      const auto& completed = timers::completed_stats[1];
      Check(delivery != nullptr && *delivery == adaptive::DeliveryReceipt::DELIVERED
                && completed.valid && completed.sequence == sequence && completed.total_us == 0,
            "proof-only completion preserves delivery and records no timestamp cost");
    } else if (segment != nullptr && segment->measure) {
      const auto measured_sequence = segment->sequence;
      const auto* actual = timers::internal::mapped;
      Check(actual != nullptr && actual[segment->base + timers::kSlotFrameStart] != timers::kUnwritten,
            "measured source resolved the real WARP timestamps");
      const uint64_t first = actual[segment->base + timers::kSlotFrameStart];
      const uint64_t last = actual[segment->base + timers::kSlotCopyBackEnd];
      UINT64 frequency = 0;
      Check(last > first && SUCCEEDED(gpu->queue->GetTimestampFrequency(&frequency)) && frequency != 0,
            "the exact WARP queue has a positive complete timestamp interval");
      std::array<uint64_t, timers::kHeapSlots> controlled;
      std::copy_n(actual, controlled.size(), controlled.begin());
      const double ticks = static_cast<double>(frequency) * cost_us / 1'000'000.;
      for (uint32_t slot = timers::kSlotFrameStart; slot <= timers::kSlotCopyBackEnd; ++slot) {
        if ((segment->written & (uint64_t{1} << slot)) == 0) continue;
        const double fraction = static_cast<double>(actual[segment->base + slot] - first) / (last - first);
        controlled[segment->base + slot] = first + static_cast<uint64_t>(std::llround(ticks * fraction));
      }
      funnel_accounting_tests::Restore timestamp_source;
      timestamp_source.Value(&timers::internal::mapped);
      timers::internal::mapped = controlled.data();
      const auto delivery = segment->adaptive.delivery;
      const auto stable = segment->adaptive_stable;
      const auto sampling_generation = segment->adaptive.sampling_generation;
      const auto generation = segment->adaptive.generation;
      const auto frame = segment->adaptive.application_frame;
      const auto proof = segment->proof;
      const auto proof_completed = (segment->proof_fence.empty() ? 0 : segment->proof_fence->GetCompletedValue());
      timers::Poll(true, false);
      const auto& stage = timers::stage_stats[1];
      const auto& raw = timers::stats;
      // The display averages and throttles stage_stats. The completion ledger
      // reports the same full-chain sample that went to the controller.
      const auto& completed = timers::completed_stats[1];
      const bool published = completed.valid && completed.sequence == measured_sequence && completed.total_us == cost_us
          && delivery != nullptr && *delivery == adaptive::DeliveryReceipt::DELIVERED;
      if (!published) {
        std::cerr << "Adaptive timing stage=" << stage.valid << '/' << stage.sequence << '/' << stage.total_us
                  << " state=" << timers::StateText(stage.state) << " raw=" << raw.valid << '/' << raw.sequence << '/' << raw.total_us
                  << " completed=" << completed.valid << '/' << completed.sequence << '/' << completed.total_us
                  << " expected=" << measured_sequence << '/' << cost_us
                  << " delivery=" << (delivery == nullptr ? -1 : static_cast<int>(*delivery)) << " stable=" << stable
                  << " generation=" << generation << " sampling_generation=" << sampling_generation << " frame=" << frame
                  << " proof=" << timers::StateText(proof) << " proof_completed=" << proof_completed
                  << " fixture_fence=" << gpu->fence->GetCompletedValue() << '/' << gpu->serial
                  << " timestamp_frequency=" << frequency << " controlled_ticks=" << std::llround(ticks) << '\n';
      }
      Check(published,
            "the production publisher reports the controlled cost after exact WARP delivery and completion");
    } else {
      timers::Poll(true, false);
    }
    gpu->uploads.clear();
    gpu->retained_resources.clear();
  }
};
}  // namespace nr_edit_reuse_gpu_cases
