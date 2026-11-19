/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Included after the ordinary Present x2 fixture types.
namespace nr_edit_reuse_present_diagnostic_cases {
namespace shot = renodx::addons::dlss5::screenshot;

inline void Run(const std::filesystem::path& artifact_dir, Gpu* gpu,
                nr_edit_reuse_present_cases::Swapchain* output, uint32_t size,
                const auto& record, const auto& submit, const auto& finish,
                const auto& swap, const auto& reopen, ID3D12Resource* motion, ID3D12Resource* depth) {
  std::filesystem::create_directories(artifact_dir);
  Check(std::filesystem::equivalent(renodx::utils::path::GetReShadeBasePath(), artifact_dir),
        "Present x2 diagnostic uses the test-local screenshot directory");
  Check(native::WantsPresentBaseFrames() && native::present_base_capture_active.load()
            && native::queue_tracking_active.load(),
        "Present x2 diagnostic runs with its real native observers and submission tracking active");

  // Vary both axes and motion components so stale, neutral, and shifted copies differ.
  using Motion = std::array<float, 2>;
  const size_t pixel_count = static_cast<size_t>(size) * size;
  std::vector<Motion> motion_pixels(pixel_count);
  std::vector<float> depth_pixels(pixel_count);
  for (uint32_t y = 0; y < size; ++y) {
    for (uint32_t x = 0; x < size; ++x) {
      const size_t index = static_cast<size_t>(y) * size + x;
      motion_pixels[index] = {float(x + 1) / 32.f, -float(y + 1) / 64.f};
      depth_pixels[index] = .125f + static_cast<float>(index) / static_cast<float>(2 * pixel_count);
    }
  }
  const auto upload = [&](ID3D12Resource* texture, const auto& pixels) {
    auto transfer = gpu->CreateTextureTransfer(texture);
    const auto pixel_bytes = sizeof(pixels.front());
    gpu->Transition(texture, NATIVE_INPUT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
    gpu->UploadTexture(texture, transfer, reinterpret_cast<const uint8_t*>(pixels.data()),
                       pixel_count * pixel_bytes, size * pixel_bytes);
    gpu->Transition(texture, D3D12_RESOURCE_STATE_COPY_DEST, NATIVE_INPUT_STATE);
  };
  upload(motion, motion_pixels);
  upload(depth, depth_pixels);

  shot::source_mode = 2;
  shot::stage_planes_enabled = true;
  Check(shot::RequestCapture() && shot::WantsEvaluatedBuffer(), "arm evaluated-buffer stage capture");
  record(true);
  submit();
  const uint64_t submitted = native::present_ring.submitted;
  const uint32_t calls = model_calls;
  const auto& stream = native::present_streams.at(output);
  const uint64_t outputs = stream.nr_presents;
  const auto on_present = [&] {
    native::OnPresent(&output->queue, output, nullptr, nullptr, 0, nullptr);
  };
  // The process runner bounds the synchronous callback even if a native hook deadlocks.
  std::cerr << "PRESENT_X2_DIAGNOSTIC callback entered" << std::endl;
  on_present();
  std::cerr << "PRESENT_X2_DIAGNOSTIC callback returned" << std::endl;
  finish();
  Check(native::present_ring.submitted == submitted + 1 && stream.nr_presents == outputs + 1
            && model_calls == calls + 1,
        "diagnostic Present x2 refresh submits and delivers one real base frame");

  struct Guide {
    const char* name;
    const char* parameter;
    DXGI_FORMAT format;
    ID3D12Resource* source;
  };
  const Guide guides[]{
      {"guide_depth", NVSDK_NGX_Parameter_Depth, DXGI_FORMAT_R32_FLOAT, depth},
      {"guide_motion", NVSDK_NGX_Parameter_MotionVectors, DXGI_FORMAT_R32G32_FLOAT, motion},
  };
  uint64_t serial = 0;
  {
    std::scoped_lock lock(shot::internal::state_mutex);
    const auto& capture = shot::internal::pending;
    Check(capture && capture->model_diagnostic && capture->stage_planes && capture->pre_recorded && capture->ready
              && !capture->aborted && capture->width == size && capture->height == size
              && capture->model_recording_list == native::present_ring.slots[(submitted + 1) % native::kPresentRing].list,
          "pending model pair belongs to the submitted Present ring recording and full model grid");
    serial = capture->serial;
    for (const auto& guide : guides) {
      const auto* clone = native::GetD3D12Resource(&stream.parameters, guide.parameter);
      const auto plane = std::find_if(capture->planes.begin(), capture->planes.end(),
                                      [&](const auto& candidate) { return candidate.name == guide.name; });
      Check(clone != nullptr && clone != guide.source && plane != capture->planes.end()
                && plane->source_identity == reinterpret_cast<uintptr_t>(clone)
                && plane->required && plane->recorded && plane->error.empty() && plane->readback != nullptr
                && plane->source_width == size && plane->source_height == size && plane->width == size && plane->height == size
                && !plane->region_copy && plane->source_format == guide.format && plane->format == guide.format
                && shot::internal::DiagnosticFootprintValid(*plane),
            guide.name);
    }
  }

  // Real subsequent Presents reuse the exact ring slot, retiring the completed
  // recording. The next lifecycle tick can then hand its readbacks to the worker.
  for (uint32_t frame = 0; frame <= native::kPresentRing && shot::HasPending(); ++frame) {
    swap();
    reopen();
    record(false);
    submit();
    on_present();
    finish();
  }
  Check(!shot::HasPending(), "Present x2 diagnostic leaves pending after exact ring retirement");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (shot::internal::stage_busy.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  Check(!shot::internal::stage_busy.load(), "Present x2 diagnostic worker completes within 10 seconds");
  shot::internal::FinishedCapture result;
  {
    std::scoped_lock lock(shot::internal::finished_mutex);
    result = shot::internal::finished;
  }
  Check(result.serial == serial && result.model_diagnostic && std::string_view(result.outcome) == "written",
        "screenshot worker successfully exports the captured Present model pair");
  Check(std::filesystem::equivalent(result.meta.parent_path(), artifact_dir / L"DLSS5 Screenshots")
            && std::filesystem::is_regular_file(result.pre_image) && std::filesystem::is_regular_file(result.post_image),
        "production screenshot worker writes the pair and sidecar under the test artifact directory");

  std::ifstream meta_file(result.meta);
  const std::string meta{std::istreambuf_iterator<char>(meta_file), std::istreambuf_iterator<char>()};
  Check(meta.find("\ncompletion_proven=1\n") != std::string::npos
            && meta.find("\nrequired_planes_complete=1\n") != std::string::npos
            && meta.find("\nguide_real=1\n") != std::string::npos,
        "Present diagnostic sidecar proves exact completion and real guides");
  for (const auto& guide : guides) {
    const std::string prefix = "\nplane." + std::string(guide.name) + '.';
    Check(meta.find(prefix + "copy_submission_completed=1\n") != std::string::npos
              && meta.find(prefix + "output_success=1\n") != std::string::npos,
          guide.name);
    auto plane_path = result.meta.parent_path() / result.meta.stem().stem();
    plane_path += std::string("_") + guide.name + ".f32";
    using Rgba = std::array<float, 4>;
    std::vector<Rgba> pixels(pixel_count);
    std::ifstream plane_file(plane_path, std::ios::binary);
    plane_file.read(reinterpret_cast<char*>(pixels.data()), static_cast<std::streamsize>(pixels.size() * sizeof(Rgba)));
    Check(plane_file && plane_file.peek() == std::ifstream::traits_type::eof(), guide.name);
    for (size_t index = 0; index < pixel_count; ++index) {
      const Rgba expected = (guide.source == depth ? Rgba{depth_pixels[index], 0.f, 0.f, 1.f}
                                                   : Rgba{motion_pixels[index][0], motion_pixels[index][1], 0.f, 1.f});
      Check(pixels[index] == expected, guide.name);
    }
  }
  std::cout << "PASS ordinary Present x2 diagnostic WARP+MOCK: exact model pair and full " << size << 'x' << size
            << " depth/motion plane exports\n";
}
}  // namespace nr_edit_reuse_present_diagnostic_cases
