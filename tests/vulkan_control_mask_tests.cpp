#define VK_NO_PROTOTYPES
#define DEBUG_LEVEL_0
#include <windows.h>
#include <vulkan/vulkan.h>
#include <dlssnr.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include "vulkan_test_device.hpp"

extern "C" __declspec(dllexport) bool ReShadeRegisterAddon(void*, uint32_t) {
  return true;
}
extern "C" __declspec(dllexport) void ReShadeUnregisterAddon(void*) {}
extern "C" __declspec(dllexport) void ReShadeLogMessage(void*, int, const char*) {}

namespace addon = renodx::addons::dlss5::internal;
namespace capability = renodx::addons::dlss5::capability;
namespace stage_plan = renodx::addons::dlss5::stage_plan;
namespace vk_test = nr::test::vk;

namespace {
struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize size = 0;
};

Buffer CreateHostBuffer(vk_test::VulkanContext& context, VkDeviceSize size,
                        VkBufferUsageFlags usage) {
  VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  info.size = size;
  info.usage = usage;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  Buffer result{.size = size};
  vk_test::Require(context.DeviceProc<PFN_vkCreateBuffer>("vkCreateBuffer")(
                       context.device, &info, nullptr, &result.buffer), "vkCreateBuffer");
  VkMemoryRequirements requirements{};
  context.DeviceProc<PFN_vkGetBufferMemoryRequirements>("vkGetBufferMemoryRequirements")(
      context.device, result.buffer, &requirements);
  VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocation.allocationSize = requirements.size;
  allocation.memoryTypeIndex = context.MemoryType(
      requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  vk_test::Require(context.DeviceProc<PFN_vkAllocateMemory>("vkAllocateMemory")(
                       context.device, &allocation, nullptr, &result.memory), "vkAllocateMemory(buffer)");
  vk_test::Require(context.DeviceProc<PFN_vkBindBufferMemory>("vkBindBufferMemory")(
                       context.device, result.buffer, result.memory, 0), "vkBindBufferMemory");
  return result;
}

void DestroyBuffer(vk_test::VulkanContext& context, Buffer* buffer) {
  if (buffer->buffer != VK_NULL_HANDLE) {
    context.DeviceProc<PFN_vkDestroyBuffer>("vkDestroyBuffer")(context.device, buffer->buffer, nullptr);
    buffer->buffer = VK_NULL_HANDLE;
  }
  if (buffer->memory != VK_NULL_HANDLE) {
    context.DeviceProc<PFN_vkFreeMemory>("vkFreeMemory")(context.device, buffer->memory, nullptr);
    buffer->memory = VK_NULL_HANDLE;
  }
}

void Begin(vk_test::VulkanContext& context) {
  if (addon::vk::real_reset_command_buffer == nullptr) {
    addon::vk::real_reset_command_buffer =
        context.DeviceProc<PFN_vkResetCommandBuffer>("vkResetCommandBuffer");
  }
  if (addon::vk::real_begin_command_buffer == nullptr) {
    addon::vk::real_begin_command_buffer =
        context.DeviceProc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer");
  }
  vk_test::Require(
      addon::vk::HookedResetCommandBuffer(context.command_buffer, 0),
      "vkResetCommandBuffer");
  VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vk_test::Require(
      addon::vk::HookedBeginCommandBuffer(context.command_buffer, &info),
      "vkBeginCommandBuffer");
}

void SubmitRecordedAndWait(vk_test::VulkanContext& context);

void SubmitAndWait(vk_test::VulkanContext& context) {
  vk_test::Require(context.DeviceProc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(
                       context.command_buffer), "vkEndCommandBuffer");
  SubmitRecordedAndWait(context);
}

void SubmitRecordedAndWait(vk_test::VulkanContext& context) {
  VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  VkFence fence = VK_NULL_HANDLE;
  vk_test::Require(context.DeviceProc<PFN_vkCreateFence>("vkCreateFence")(
                       context.device, &fence_info, nullptr, &fence), "vkCreateFence");
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &context.command_buffer;
  const VkResult submitted = context.DeviceProc<PFN_vkQueueSubmit>("vkQueueSubmit")(
      context.queue, 1, &submit, fence);
  if (submitted == VK_SUCCESS) {
    vk_test::Require(context.DeviceProc<PFN_vkWaitForFences>("vkWaitForFences")(
                         context.device, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences");
  }
  context.DeviceProc<PFN_vkDestroyFence>("vkDestroyFence")(context.device, fence, nullptr);
  vk_test::Require(submitted, "vkQueueSubmit");
}

NVSDK_NGX_Resource_VK MakeImageResource(VkImage image, VkImageView view, uint32_t width,
                                        uint32_t height, VkFormat format, bool read_write) {
  NVSDK_NGX_Resource_VK result{};
  result.Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
  result.ReadWrite = read_write;
  result.Resource.ImageViewInfo = {view, image, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
                                   format, width, height};
  return result;
}

void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}

#if defined(NR_NEXUS_TEST_NATIVE_NGX)
addon::vk::EvaluateFn native_real_evaluate = nullptr;
NVSDK_NGX_Resource_VK native_observed_mask{};
bool native_observed_mask_bound = false;
uint32_t native_evaluate_count = 0;
NVSDK_NGX_Result native_observed_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;

NVSDK_NGX_Result NVSDK_CONV ObserveNativeEvaluate(
    VkCommandBuffer command_buffer, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
  Check(addon::get_module_filename_iat == nullptr,
        "public SDK Evaluate does not install the caller identity IAT bridge");
  void* value = nullptr;
  ++native_evaluate_count;
  native_observed_mask_bound = false;
  if (parameters != nullptr
      && NVSDK_NGX_SUCCEED(const_cast<NVSDK_NGX_Parameter*>(parameters)->Get(
          "DLSSNR.ControlMask", &value)) && value != nullptr) {
    // ProcessNr owns this descriptor on its stack and unbinds it after Evaluate.
    native_observed_mask = *static_cast<NVSDK_NGX_Resource_VK*>(value);
    native_observed_mask_bound = true;
  }
  native_observed_result = native_real_evaluate(command_buffer, handle, parameters, callback);
  Check(addon::get_module_filename_iat == nullptr,
        "public SDK Evaluate leaves the caller identity IAT untouched");
  return native_observed_result;
}

Buffer UploadImage(vk_test::VulkanContext& context, vk_test::CommandList& command_list,
                   reshade::api::resource image, VkImage native_image,
                   const void* data, VkDeviceSize size, uint32_t width, uint32_t height) {
  Buffer upload = CreateHostBuffer(context, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  void* mapped = nullptr;
  vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                       context.device, upload.memory, 0, upload.size, 0, &mapped), "map upload");
  std::memcpy(mapped, data, static_cast<size_t>(size));
  context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, upload.memory);
  const reshade::api::resource resources[] = {image};
  const reshade::api::resource_usage before[] = {reshade::api::resource_usage::undefined};
  const reshade::api::resource_usage copy_dest[] = {reshade::api::resource_usage::copy_dest};
  command_list.barrier(1, resources, before, copy_dest);
  const VkBufferImageCopy region{0, 0, 0,
      {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, 0, 0, 0, {width, height, 1}};
  context.DeviceProc<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(
      context.command_buffer, upload.buffer, native_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      1, &region);
  const reshade::api::resource_usage shader_resource[] = {
      reshade::api::resource_usage::shader_resource};
  command_list.barrier(1, resources, copy_dest, shader_resource);
  return upload;
}

void RunNativeNgx(const wchar_t* project_id_path) {
  vk_test::VulkanContext context;
  vk_test::Device device(&context);
  vk_test::CommandList command_list(&context, &device);
  reshade::api::resource color{};
  reshade::api::resource output{};
  reshade::api::resource depth{};
  reshade::api::resource motion{};
  reshade::api::resource_view color_view{};
  reshade::api::resource_view output_view{};
  reshade::api::resource_view depth_view{};
  reshade::api::resource_view motion_view{};
  std::vector<Buffer> uploads;
  Buffer mask_readback{};
  bool public_sdk_initialized = false;
  bool cleanup_done = false;
  const auto cleanup = [&] {
    if (cleanup_done) return;
    cleanup_done = true;
    if (addon::vk::runtime.device == context.device) {
      std::lock_guard lock(addon::runtime_mutex);
      addon::vk::ShutdownNr(context.device);
      public_sdk_initialized = false;
    }
    if (public_sdk_initialized) {
      NVSDK_NGX_VULKAN_Shutdown1(context.device);
      public_sdk_initialized = false;
    }
    if (addon::vk::game_api_device.load(std::memory_order_relaxed) == &device) {
      std::lock_guard lock(addon::runtime_mutex);
      addon::vk::OnDestroyGameDevice();
    }
    addon::vk::OnDestroyDeviceContext(&device);
    for (Buffer& buffer : uploads) DestroyBuffer(context, &buffer);
    DestroyBuffer(context, &mask_readback);
    device.destroy_resource_view(motion_view);
    device.destroy_resource_view(depth_view);
    device.destroy_resource_view(output_view);
    device.destroy_resource_view(color_view);
    device.destroy_resource(motion);
    device.destroy_resource(depth);
    device.destroy_resource(output);
    device.destroy_resource(color);
  };

  try {
    Check(addon::get_module_filename_iat == nullptr,
          "public SDK test starts without a caller identity IAT bridge");
    addon::addon_module = GetModuleHandleW(nullptr);
    addon::hooks_enabled = false;
    Begin(context);
    addon::vk::OnResetCommandList(&command_list);
    addon::vk::game_instance.store(context.instance, std::memory_order_relaxed);
    addon::vk::game_physical_device.store(context.physical, std::memory_order_relaxed);
    addon::vk::game_init_device.store(context.device, std::memory_order_relaxed);
    {
      std::lock_guard lock(addon::vk::context_mutex);
      addon::vk::device_contexts[context.device] = {context.instance, context.physical};
    }
    {
      std::lock_guard lock(addon::vk::context_mutex);
      addon::vk::api_devices[context.device] = &device;
    }
    addon::hooks_enabled = true;
    addon::enabled = true;
    addon::control_mask_enabled = true;
    addon::control_mask_mode = addon::kControlMaskDepthFar;
    addon::control_mask_depth_near = 0.7f;
    addon::control_mask_depth_far = 0.9f;
    addon::control_mask_blend_mode = 0;
    addon::control_mask_red = 0.75f;
    addon::control_mask_green = 0.25f;
    addon::control_mask_blue = 0.5f;
    addon::control_mask_motion_enabled = false;
    addon::control_mask_motion_start = 2.f;
    addon::control_mask_motion_end = 14.f;
    addon::motion_scale_x_multiplier = 1.f;
    addon::motion_scale_y_multiplier = 1.f;
    addon::stack_passes = 1;
    addon::hook_point = addon::kHookUpscaled;
    addon::preset = 1;
    addon::intensity = 1.f;
    addon::transfer_strength = 1.f;
    addon::color_strength = 1.f;
    addon::look_mode = false;
    addon::detail_stability = 1;
    addon::PrimeNgxLoaderSymbols(nullptr);
    const auto& provider = addon::NgxLoaderView();
    Check(provider.nr_ready && !provider.nr_failed
              && provider.nr_requirements.file_version == "310.9.16.0",
          "read-only loader metadata identifies the selected 310.9.16.0 Runtime");
    Check(addon::get_module_filename_iat == nullptr,
          "priming Runtime metadata does not install a caller identity bridge");
    std::ifstream project_id_file(project_id_path);
    std::string project_id;
    Check(project_id_file.is_open() && std::getline(project_id_file, project_id)
              && project_id.size() == 36,
          "read the existing isolated NGX ProjectID");
    const std::wstring runtime_directory = addon::AddonDirectory().wstring();
    const wchar_t* runtime_paths[] = {runtime_directory.c_str()};
    NVSDK_NGX_FeatureCommonInfo common_info{};
    common_info.PathListInfo = {runtime_paths, 1};
    const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    const auto get_instance_proc = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        GetProcAddress(loader, "vkGetInstanceProcAddr"));
    const auto get_device_proc = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        GetProcAddress(loader, "vkGetDeviceProcAddr"));
    Check(get_instance_proc != nullptr && get_device_proc != nullptr,
          "public Vulkan SDK Init receives the existing loader proc addresses");
    const auto init_result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        project_id.c_str(), NVSDK_NGX_ENGINE_TYPE_CUSTOM, "NR-Nexus-ControlMask-Vulkan-offline-1",
        runtime_directory.c_str(), context.instance, context.physical, context.device,
        get_instance_proc, get_device_proc, &common_info);
    std::cout << "PUBLIC_SDK Vulkan Init_with_ProjectID result=0x" << std::hex
              << static_cast<uint32_t>(init_result) << std::dec << std::endl;
    public_sdk_initialized = NVSDK_NGX_SUCCEED(init_result);
    Check(public_sdk_initialized, "public Vulkan SDK/Core ProjectID Init succeeds");
    Check(addon::get_module_filename_iat == nullptr,
          "public Vulkan SDK Init leaves the caller identity IAT untouched");
    // Adopt the SDK-initialized device so EnsureRuntime takes its existing-device
    // fast path. Create/Evaluate/Release/Shutdown remain public SDK/Core calls.
    addon::vk::runtime = {
        .create = &NVSDK_NGX_VULKAN_CreateFeature1,
        .evaluate = &NVSDK_NGX_VULKAN_EvaluateFeature,
        .release = &NVSDK_NGX_VULKAN_ReleaseFeature,
        .shutdown = &NVSDK_NGX_VULKAN_Shutdown1,
        .device = context.device,
    };
    addon::direct_runtime_requirements = provider.nr_requirements;
    addon::direct_runtime_sha256 = provider.nr_sha256;

    constexpr uint32_t width = 960;
    constexpr uint32_t height = 540;
    constexpr uint32_t depth_base_x = 4;
    constexpr uint32_t depth_base_y = 6;
    constexpr uint32_t depth_width = width + depth_base_x;
    constexpr uint32_t depth_height = height + depth_base_y;
    constexpr uint32_t motion_base_x = 2;
    constexpr uint32_t motion_base_y = 4;
    constexpr uint32_t motion_width = width + motion_base_x;
    constexpr uint32_t motion_height = height + motion_base_y;
    const auto usages = reshade::api::resource_usage::shader_resource
        | reshade::api::resource_usage::unordered_access
        | reshade::api::resource_usage::copy_dest
        | reshade::api::resource_usage::copy_source;
    Check(device.create_resource({width, height, 1, 1, reshade::api::format::r16g16b16a16_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &color),
          "native color image");
    Check(device.create_resource({width, height, 1, 1, reshade::api::format::r16g16b16a16_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &output),
          "native output image");
    Check(device.create_resource({depth_width, depth_height, 1, 1, reshade::api::format::r32_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &depth),
          "native depth image");
    Check(device.create_resource({motion_width, motion_height, 1, 1, reshade::api::format::r16g16_float, 1,
                                  reshade::api::memory_heap::gpu_only, usages},
                                 nullptr, reshade::api::resource_usage::undefined, &motion),
          "native motion image");
    Check(device.create_resource_view(color, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float),
                                     &color_view), "native color view");
    Check(device.create_resource_view(output, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r16g16b16a16_float),
                                     &output_view), "native output view");
    Check(device.create_resource_view(depth, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r32_float),
                                     &depth_view), "native depth view");
    Check(device.create_resource_view(motion, reshade::api::resource_usage::undefined,
                                     reshade::api::resource_view_desc(reshade::api::format::r16g16_float),
                                     &motion_view), "native motion view");

    std::vector<uint16_t> color_data(width * height * 4, 0x3c00);
    std::vector<uint16_t> output_data(width * height * 4, 0);
    std::vector<float> depth_data(depth_width * depth_height);
    for (uint32_t y = 0; y < depth_height; ++y) {
      const uint32_t relative_y = y >= depth_base_y ? y - depth_base_y : UINT32_MAX;
      const float value = relative_y < 32 ? 0.5f : (relative_y < 56 ? 0.8f : 1.0f);
      for (uint32_t x = 0; x < depth_width; ++x) depth_data[y * depth_width + x] = value;
    }
    std::vector<uint16_t> motion_data(motion_width * motion_height * 2, 0);
    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        const size_t index = ((y + motion_base_y) * motion_width + x + motion_base_x) * 2;
        // FP16 (4, 0) and (0, 32), scaled by (-2, 0.5), are 8 and 16 pixels/frame.
        motion_data[index] = (x < width / 2 ? 0x4400 : 0);
        motion_data[index + 1] = (x < width / 2 ? 0 : 0x5000);
      }
    }
    Begin(context);
    addon::hooks_enabled = false;
    addon::vk::OnResetCommandList(&command_list);
    addon::hooks_enabled = true;
    uploads.push_back(UploadImage(context, command_list, color,
                                  vk_test::NativeHandle<VkImage>(color.handle), color_data.data(),
                                  color_data.size() * sizeof(uint16_t), width, height));
    uploads.push_back(UploadImage(context, command_list, output,
                                  vk_test::NativeHandle<VkImage>(output.handle), output_data.data(),
                                  output_data.size() * sizeof(uint16_t), width, height));
    uploads.push_back(UploadImage(context, command_list, depth,
                                  vk_test::NativeHandle<VkImage>(depth.handle), depth_data.data(),
                                  depth_data.size() * sizeof(float), depth_width, depth_height));
    uploads.push_back(UploadImage(context, command_list, motion,
                                  vk_test::NativeHandle<VkImage>(motion.handle), motion_data.data(),
                                  motion_data.size() * sizeof(uint16_t), motion_width, motion_height));

    const auto color_vk = addon::vk::ImageResource(color_view, color, width, height, false,
                                                   VK_FORMAT_R16G16B16A16_SFLOAT);
    const auto output_vk = addon::vk::ImageResource(output_view, output, width, height, true,
                                                    VK_FORMAT_R16G16B16A16_SFLOAT);
    const auto depth_vk = addon::vk::ImageResource(depth_view, depth, depth_width, depth_height, false,
                                                   VK_FORMAT_R32_SFLOAT);
    const auto motion_vk = addon::vk::ImageResource(motion_view, motion, motion_width, motion_height, false,
                                                    VK_FORMAT_R16G16_SFLOAT);
    renodx::addons::dlss5::bridge::OwnedParameters parameters;
    addon::SetNrCreateParameters(&parameters, width, height, width, height, 0, false, 1);
    parameters.Set(NVSDK_NGX_Parameter_Color, const_cast<NVSDK_NGX_Resource_VK*>(&color_vk));
    parameters.Set(NVSDK_NGX_Parameter_Output, const_cast<NVSDK_NGX_Resource_VK*>(&output_vk));
    parameters.Set(NVSDK_NGX_Parameter_Depth, const_cast<NVSDK_NGX_Resource_VK*>(&depth_vk));
    parameters.Set(NVSDK_NGX_Parameter_MotionVectors, const_cast<NVSDK_NGX_Resource_VK*>(&motion_vk));
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, width);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, height);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_X, depth_base_x);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_Depth_Subrect_Base_Y, depth_base_y);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, motion_base_x);
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_Y, motion_base_y);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_X, -2.f);
    parameters.Set(NVSDK_NGX_Parameter_MV_Scale_Y, 0.5f);
    const auto* owner = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t{0xC0A71003});
    addon::vk::Feature feature{};
    feature.id = addon::kFeatureDlss;
    feature.device = &device;
    addon::vk::features.emplace(owner, std::move(feature));

    const auto first = addon::vk::ProcessNr(context.command_buffer, owner, &parameters,
                                            addon::vk::UpscaledExecutionInput(false));
    std::cout << "PUBLIC_SDK production Feature18 Create result=0x" << std::hex
              << addon::last_result.load() << std::dec
              << " decline=" << static_cast<int>(first) << std::endl;
    Check(addon::get_module_filename_iat == nullptr,
          "production Vulkan Create bypasses the snippet identity bridge");
    Check(first == renodx::addons::dlss5::NrDeclineReason::kSlotWarmupMaturation,
          "production Vulkan Feature 18 create reaches warmup maturation");
    SubmitAndWait(context);

    mask_readback = CreateHostBuffer(context, width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    std::vector<uint8_t> disabled_mask;
    native_evaluate_count = 0;
    for (uint32_t frame = 0; frame < 3; ++frame) {
      const bool motion_enabled = frame == 1;
      addon::control_mask_motion_enabled = motion_enabled;
      Begin(context);
      addon::hooks_enabled = false;
      addon::vk::OnResetCommandList(&command_list);
      addon::hooks_enabled = true;
      const reshade::api::resource_usage color_before[] = {reshade::api::resource_usage::shader_resource};
      const reshade::api::resource_usage color_copy[] = {reshade::api::resource_usage::copy_source};
      const reshade::api::resource_usage output_before[] = {reshade::api::resource_usage::shader_resource};
      const reshade::api::resource_usage output_copy[] = {reshade::api::resource_usage::copy_dest};
      const reshade::api::resource_usage output_write[] = {reshade::api::resource_usage::unordered_access};
      command_list.barrier(1, &color, color_before, color_copy);
      command_list.barrier(1, &output, output_before, output_copy);
      command_list.copy_texture_region(color, 0, nullptr, output, 0, nullptr);
      command_list.barrier(1, &color, color_copy, color_before);
      command_list.barrier(1, &output, output_copy, output_write);

      native_real_evaluate = addon::vk::runtime.evaluate;
      Check(native_real_evaluate != nullptr, "production Vulkan Runtime exposes Feature 18 Evaluate");
      native_observed_mask_bound = false;
      native_observed_result = NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
      addon::vk::runtime.evaluate = ObserveNativeEvaluate;
      const auto second = addon::vk::ProcessNr(context.command_buffer, owner, &parameters,
                                              addon::vk::UpscaledExecutionInput(false));
      addon::vk::runtime.evaluate = native_real_evaluate;
      std::cout << "PUBLIC_SDK production Feature18 Evaluate frame=" << frame
                << " result=0x" << std::hex << static_cast<uint32_t>(native_observed_result)
                << std::dec << " decline=" << static_cast<int>(second) << std::endl;
      Check(second == renodx::addons::dlss5::NrDeclineReason::kCount,
            "production Vulkan Feature 18 Evaluate completes with ControlMask enabled");
      Check(NVSDK_NGX_SUCCEED(native_observed_result),
            "real Vulkan Runtime Feature 18 Evaluate returns success");
      Check(native_evaluate_count == frame + 1,
            "each production Vulkan frame performs exactly one real Feature 18 Evaluate");
      const auto& workset = addon::vk::features.at(owner).worksets.front();
      Check(native_observed_mask_bound,
            "real Vulkan Runtime Evaluate receives a ControlMask resource");
      Check(native_observed_mask.Resource.ImageViewInfo.Image
                == vk_test::NativeHandle<VkImage>(workset.images[addon::vk::kControlMask].handle),
            "real Vulkan Runtime receives the generated production mask image");

      const reshade::api::resource mask_resource = workset.images[addon::vk::kControlMask];
      const reshade::api::resource_usage mask_before[] = {reshade::api::resource_usage::unordered_access};
      const reshade::api::resource_usage mask_copy[] = {reshade::api::resource_usage::copy_source};
      command_list.barrier(1, &mask_resource, mask_before, mask_copy);
      const VkBufferImageCopy mask_region{0, 0, 0,
          {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, 0, 0, 0, {width, height, 1}};
      context.DeviceProc<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
          context.command_buffer, vk_test::NativeHandle<VkImage>(mask_resource.handle),
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, mask_readback.buffer, 1, &mask_region);
      // Restore layouts so the completed recording can also be replayed as-is.
      command_list.barrier(1, &mask_resource, mask_copy, mask_before);
      command_list.barrier(1, &output, output_write, output_before);
      SubmitAndWait(context);
      void* mapped = nullptr;
      vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                           context.device, mask_readback.memory, 0, mask_readback.size, 0, &mapped),
                       "map native production mask");
      const auto* pixels = static_cast<const uint8_t*>(mapped);
      for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
          const size_t index = (y * width + x) * 4;
          // At 8 pixels/frame motion keep is 0.5; at 16 it is 0. Depth keep is 1/0.5/0.
          int expected_red = (y < 32 ? 191 : (y < 56 ? 96 : 0));
          if (motion_enabled) {
            expected_red = (x < width / 2 && y < 56 ? 96 : 0);
          }
          Check(std::abs(static_cast<int>(pixels[index]) - expected_red) <= 1,
                "native production motion/depth R uses parsed subrect/scales and strongest protection");
          Check(pixels[index + 1] == 64
                    && std::abs(static_cast<int>(pixels[index + 2]) - 128) <= 1
                    && pixels[index + 3] == 255,
                "native production motion attenuation preserves configured G/B and opaque A");
        }
      }
      if (frame == 0) {
        disabled_mask.assign(pixels, pixels + mask_readback.size);
      } else if (frame == 2) {
        Check(std::memcmp(disabled_mask.data(), pixels, disabled_mask.size()) == 0,
              "disabling motion attenuation exactly restores the depth/manual mask with unchanged nonzero motion");
      }
      context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, mask_readback.memory);
      if (frame == 0) {
        SubmitRecordedAndWait(context);
      }
      Check(!addon::vk::RecordedUseComplete(workset.use_id),
            "completed production Vulkan recording remains live while replayable");
      Begin(context);
      addon::vk::OnResetCommandList(&command_list);
      Check(addon::vk::RecordedUseComplete(workset.use_id),
            "production reset retires the replayed workset use");
      std::cout << "NATIVE_GPU+NATIVE_NGX motion frame=" << frame
                << " enabled=" << motion_enabled << " scale=(-2,0.5) subrect=(2,4,960,540)"
                   " R/G/B/A readback passed\n";
    }
    cleanup();
    Check(addon::get_module_filename_iat == nullptr,
          "public Vulkan SDK cleanup leaves the caller identity IAT untouched");
    std::cout << "PASS NATIVE_GPU+NATIVE_NGX: production Vulkan Feature 18 Create/Evaluate consumed"
                 " depth/motion ControlMask through OFF/ON/OFF, replayed the completed command buffer,"
                 " and completed reset/RGBA readback\n";
  } catch (...) {
    cleanup();
    throw;
  }
}

#endif  // NR_NEXUS_TEST_NATIVE_NGX

void RunProductionMaskDispatchMock() {
  vk_test::VulkanContext context;
  vk_test::Device device(&context);
  vk_test::CommandList command_list(&context, &device);
  constexpr uint32_t width = 16;
  constexpr uint32_t height = 16;
  constexpr uint32_t depth_base_x = 3;
  constexpr uint32_t depth_base_y = 6;
  constexpr uint32_t depth_width = 8;
  constexpr uint32_t depth_height = 8;

  reshade::api::resource depth{};
  reshade::api::resource motion{};
  reshade::api::resource mask{};
  reshade::api::resource_view depth_view{};
  reshade::api::resource_view motion_view{};
  reshade::api::resource_view mask_view{};
  const auto usages = reshade::api::resource_usage::shader_resource
                      | reshade::api::resource_usage::unordered_access
                      | reshade::api::resource_usage::copy_dest
                      | reshade::api::resource_usage::copy_source;
  Check(device.create_resource({16, 16, 1, 1, reshade::api::format::r32_float, 1,
                                reshade::api::memory_heap::gpu_only, usages},
                               nullptr, reshade::api::resource_usage::undefined, &depth),
        "create depth image");
  Check(device.create_resource({width, height, 1, 1, reshade::api::format::r8g8b8a8_unorm, 1,
                                reshade::api::memory_heap::gpu_only, usages},
                               nullptr, reshade::api::resource_usage::undefined, &mask),
        "create RGBA8 mask image");
  Check(device.create_resource({16, 16, 1, 1, reshade::api::format::r32g32_float, 1,
                                reshade::api::memory_heap::gpu_only, usages},
                               nullptr, reshade::api::resource_usage::undefined, &motion),
        "create motion image");
  Check(device.create_resource_view(depth, reshade::api::resource_usage::undefined,
                                    reshade::api::resource_view_desc(reshade::api::format::r32_float),
                                    &depth_view),
        "create depth image view");
  Check(device.create_resource_view(motion, reshade::api::resource_usage::undefined,
                                    reshade::api::resource_view_desc(reshade::api::format::r32g32_float),
                                    &motion_view),
        "create motion image view");
  Check(device.create_resource_view(mask, reshade::api::resource_usage::undefined,
                                    reshade::api::resource_view_desc(reshade::api::format::r8g8b8a8_unorm),
                                    &mask_view),
        "create mask image view");
  Check(addon::vk::EnsureControlMaskPipeline(&device), "create production ControlMask pipeline");

  std::array<float, 16 * 16> depths{};
  for (uint32_t y = 0; y < depth_height; ++y) {
    for (uint32_t x = 0; x < depth_width; ++x) {
      depths[(depth_base_y + y) * 16 + depth_base_x + x] =
          x < 2 ? 0.5f : (x < 5 ? 0.8f : 1.0f);
    }
  }
  Buffer upload = CreateHostBuffer(context, sizeof(depths), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  Buffer readback = CreateHostBuffer(context, width * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  void* mapped = nullptr;
  vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                       context.device, upload.memory, 0, upload.size, 0, &mapped),
                   "map depth upload");
  std::memcpy(mapped, depths.data(), sizeof(depths));
  context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, upload.memory);

  Begin(context);
  addon::vk::OnResetCommandList(&command_list);
  Check(addon::vk::command_lists.at(context.command_buffer).list == &command_list,
        "the host's first recording event maps the actual native command buffer");
  Check(renodx::addons::dlss5::detour_guard::Classify(addon::vk::recording_hook_records[0]) == renodx::addons::dlss5::detour_guard::Head::kOurs
            && renodx::addons::dlss5::detour_guard::Classify(addon::vk::recording_hook_records[1]) == renodx::addons::dlss5::detour_guard::Head::kOurs,
        "native Begin and Reset entries actually forward through the production observers");
  const reshade::api::resource depth_resource[] = {depth};
  const reshade::api::resource_usage depth_before[] = {reshade::api::resource_usage::undefined};
  const reshade::api::resource_usage depth_copy[] = {reshade::api::resource_usage::copy_dest};
  command_list.barrier(1, depth_resource, depth_before, depth_copy);
  const VkBufferImageCopy upload_region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, 0, 0, 0, {16, 16, 1}};
  context.DeviceProc<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(
      context.command_buffer, upload.buffer, vk_test::NativeHandle<VkImage>(depth.handle),
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);
  const reshade::api::resource_usage depth_after[] = {reshade::api::resource_usage::shader_resource};
  command_list.barrier(1, depth_resource, depth_copy, depth_after);
  std::array<float, 16 * 16 * 2> vectors{};
  for (uint32_t y = 0; y < depth_height; ++y) {
    for (uint32_t x = 0; x < depth_width; ++x) {
      vectors[((depth_base_y + y) * 16 + depth_base_x + x) * 2] =
          (x == 0 ? 16.f : (x == 2 ? 9.f : (x == 3 ? std::numeric_limits<float>::quiet_NaN() : 0.f)));
    }
  }
  auto motion_upload = CreateHostBuffer(context, sizeof(vectors), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                       context.device, motion_upload.memory, 0, motion_upload.size, 0, &mapped),
                   "map motion upload");
  std::memcpy(mapped, vectors.data(), sizeof(vectors));
  context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, motion_upload.memory);
  const reshade::api::resource motion_resource[] = {motion};
  command_list.barrier(1, motion_resource, depth_before, depth_copy);
  context.DeviceProc<PFN_vkCmdCopyBufferToImage>("vkCmdCopyBufferToImage")(
      context.command_buffer, motion_upload.buffer, vk_test::NativeHandle<VkImage>(motion.handle),
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &upload_region);
  command_list.barrier(1, motion_resource, depth_copy, depth_after);
  const reshade::api::resource mask_resource[] = {mask};
  const reshade::api::resource_usage mask_before[] = {reshade::api::resource_usage::undefined};
  const reshade::api::resource_usage mask_write[] = {reshade::api::resource_usage::unordered_access};
  command_list.barrier(1, mask_resource, mask_before, mask_write);

  const auto depth_vk = MakeImageResource(vk_test::NativeHandle<VkImage>(depth.handle),
                                          vk_test::NativeHandle<VkImageView>(depth_view.handle), 16, 16, VK_FORMAT_R32_SFLOAT, false);
  const addon::vk::ControlMaskConstants constants{
      width, height, addon::kControlMaskDepthFar, width / 2, height / 2,
      0.75f, 0.25f, 0.5f, depth_base_x, depth_base_y, depth_width, depth_height,
      0, 0.7f, 0.9f, 0};
  const auto motion_vk = MakeImageResource(vk_test::NativeHandle<VkImage>(motion.handle),
                                           vk_test::NativeHandle<VkImageView>(motion_view.handle), 16, 16, VK_FORMAT_R32G32_SFLOAT, false);
  addon::vk::RecordControlMaskDispatch(&command_list, &depth_vk, &motion_vk, depth_view, motion_view, mask_view, constants);
  const reshade::api::resource_usage mask_read[] = {reshade::api::resource_usage::shader_resource};
  command_list.barrier(1, mask_resource, mask_write, mask_read);
  const reshade::api::resource_usage mask_copy[] = {reshade::api::resource_usage::copy_source};
  command_list.barrier(1, mask_resource, mask_read, mask_copy);
  const VkBufferImageCopy read_region{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, 0, 0, 0, {width, height, 1}};
  context.DeviceProc<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
      context.command_buffer, vk_test::NativeHandle<VkImage>(mask.handle),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &read_region);

  renodx::addons::dlss5::bridge::OwnedParameters parameters;
  addon::vk::SetControlMaskParameters(&parameters, nullptr, 0, 0, 0);
  auto mask_resource_vk = addon::vk::ImageResource(mask_view, mask, width, height, false,
                                                   VK_FORMAT_R8G8B8A8_UNORM);
  addon::vk::SetControlMaskParameters(&parameters, &mask_resource_vk, width, height, 3);
  void* bound = nullptr;
  Check(NVSDK_NGX_SUCCEED(parameters.Get("DLSSNR.ControlMask", &bound)) && bound == &mask_resource_vk,
        "production parameter block binds generated Vulkan image to Feature 18 Evaluate");
  Check(addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectWidth") == width
            && addon::GetUInt(&parameters, "DLSSNR.ControlMaskSubrectHeight") == height
            && addon::GetUInt(&parameters, "DLSSNR.MaskBlendMode") == 3,
        "production parameter block carries full mask subrect and blend mode");

  constexpr uint64_t use_id = 0xC0A71001;
  addon::vk::TrackRecordedUse(&command_list, use_id);
  Check(!addon::vk::RecordedUseComplete(use_id), "recorded mask use remains live before GPU submission");
  SubmitAndWait(context);
  Check(!addon::vk::RecordedUseComplete(use_id),
        "completed Vulkan submission remains replayable until command-buffer reset");

  vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                       context.device, readback.memory, 0, readback.size, 0, &mapped),
                   "map mask readback");
  const auto* pixels = static_cast<const uint8_t*>(mapped);
  const auto channel = [&](uint32_t x, uint32_t y, uint32_t c) {
    return pixels[(y * width + x) * 4 + c];
  };
  Check(channel(0, 0, 0) >= 190 && channel(0, 0, 0) <= 192,
        "production Vulkan shader preserves configured near-depth red weight");
  Check(channel(6, 0, 0) > 90 && channel(6, 0, 0) < 100,
        "production Vulkan shader produces the soft depth-far transition");
  Check(channel(14, 0, 0) == 0, "production Vulkan shader fades far-depth red weight to zero");
  if (channel(6, 0, 1) != 64 || channel(6, 0, 2) < 127 || channel(6, 0, 2) > 128
      || channel(6, 0, 3) != 255) {
    throw std::runtime_error("production Vulkan mask G/B/A readback was "
                             + std::to_string(channel(6, 0, 1)) + "/" + std::to_string(channel(6, 0, 2))
                             + "/" + std::to_string(channel(6, 0, 3)));
  }
  context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, readback.memory);

  SubmitRecordedAndWait(context);
  Check(!addon::vk::RecordedUseComplete(use_id),
        "replayed production Vulkan mask use remains live until reset");
  Begin(context);
  addon::vk::OnResetCommandList(&command_list);
  Check(addon::vk::RecordedUseComplete(use_id),
        "production reset retirement releases completed mask use");
  for (const bool combine_depth : {false, true}) {
    const reshade::api::resource_usage previous_mask[] = {reshade::api::resource_usage::copy_source};
    command_list.barrier(1, mask_resource, previous_mask, mask_write);
    auto motion_constants = constants;
    motion_constants.mode = (combine_depth ? addon::kControlMaskDepthFar : addon::kControlMaskRgb);
    motion_constants.motion_enabled = 1;
    motion_constants.motion_base_x = depth_base_x;
    motion_constants.motion_base_y = depth_base_y;
    motion_constants.motion_width = depth_width;
    motion_constants.motion_height = depth_height;
    motion_constants.motion_scale_x = -1.f;
    motion_constants.motion_scale_y = 1.f;
    motion_constants.motion_start = 2.f;
    motion_constants.motion_end = 16.f;
    addon::vk::RecordControlMaskDispatch(&command_list, &depth_vk, &motion_vk,
                                         depth_view, motion_view, mask_view, motion_constants);
    command_list.barrier(1, mask_resource, mask_write, mask_copy);
    context.DeviceProc<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        context.command_buffer, vk_test::NativeHandle<VkImage>(mask.handle),
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &read_region);
    SubmitAndWait(context);
    vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                         context.device, readback.memory, 0, readback.size, 0, &mapped),
                     "map motion mask readback");
    const auto* motion_pixels = static_cast<const uint8_t*>(mapped);
    Check(motion_pixels[0] == 0 && std::abs(static_cast<int>(motion_pixels[2 * 4]) - 191) <= 1
              && std::abs(static_cast<int>(motion_pixels[4 * 4]) - 96) <= 1
              && std::abs(static_cast<int>(motion_pixels[6 * 4]) - (combine_depth ? 96 : 191)) <= 1,
          "Vulkan motion subrect, negative scale, midpoint and invalid-vector fallback match the completed shader output");
    Check(motion_pixels[1] == 64 && std::abs(static_cast<int>(motion_pixels[2]) - 128) <= 1
              && motion_pixels[3] == 255 && (!combine_depth || motion_pixels[14 * 4] == 0),
          "Vulkan combines strongest R protection while preserving G/B/A");
    context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, readback.memory);
    Begin(context);
    addon::vk::OnResetCommandList(&command_list);
  }

  constexpr uint64_t discarded_use = 0xC0A71002;
  Begin(context);
  addon::vk::OnResetCommandList(&command_list);
  addon::vk::TrackRecordedUse(&command_list, discarded_use);
  Check(!addon::vk::RecordedUseComplete(discarded_use), "unsubmitted recording retains its mask use");
  Begin(context);
  addon::vk::OnResetCommandList(&command_list);
  Check(addon::vk::RecordedUseComplete(discarded_use), "discarded recording retires mask use on reset");

  const auto* source =
      reinterpret_cast<NVSDK_NGX_Handle*>(uintptr_t{0xC0A71003});
  constexpr uint64_t retired_source_use = 0xC0A71004;
  {
    auto& feature = addon::vk::features[source];
    feature.device = &device;
    feature.id = addon::kFeatureDlss;
    feature.worksets.push_back({});
    feature.worksets.back().images[addon::vk::kControlMask] = mask;
    feature.worksets.back().views[addon::vk::kControlMask] = mask_view;
    feature.worksets.back().use_id = retired_source_use;
  }
  const reshade::api::resource_usage previous_mask[] = {
      reshade::api::resource_usage::copy_source};
  command_list.barrier(1, mask_resource, previous_mask, mask_write);
  addon::vk::TrackRecordedUse(&command_list, retired_source_use);
  addon::vk::RecordControlMaskDispatch(&command_list, &depth_vk, &motion_vk,
                                       depth_view, motion_view, mask_view,
                                       constants);
  command_list.barrier(1, mask_resource, mask_write, mask_copy);
  context.DeviceProc<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
      context.command_buffer, vk_test::NativeHandle<VkImage>(mask.handle),
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.buffer, 1, &read_region);
  addon::vk::slot_real[0].release = reinterpret_cast<void*>(
      +[](NVSDK_NGX_Handle*) { return NVSDK_NGX_Result_Success; });
  Check(addon::vk::HookedRelease<0>(const_cast<NVSDK_NGX_Handle*>(source)) == NVSDK_NGX_Result_Success && !addon::vk::features.contains(source) && addon::vk::released_features.size() == 1 && device.NativeImage(mask) != VK_NULL_HANDLE,
        "source Release keeps the actual replayable Vulkan mask image alive");
  SubmitAndWait(context);
  SubmitRecordedAndWait(context);
  vk_test::Require(
      context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
          context.device, readback.memory, 0, readback.size, 0, &mapped),
      "map retired source replay");
  const auto* retired_pixels = static_cast<const uint8_t*>(mapped);
  Check(retired_pixels[0] == 191 && retired_pixels[1] == 64 && std::abs(static_cast<int>(retired_pixels[2]) - 128) <= 1 && retired_pixels[3] == 255,
        "the released source's old executable recording still produces its "
        "actual shader pixels");
  context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device,
                                                         readback.memory);
  Check(addon::vk::released_features.size() == 1 && !addon::vk::RecordedUseComplete(retired_source_use),
        "completed replay is still executable and cannot free the retired "
        "source");
  vk_test::Require(context.DeviceProc<PFN_vkResetCommandBuffer>("vkResetCommandBuffer")(
                       context.command_buffer, 0),
                   "native entry vkResetCommandBuffer");
  VkCommandBufferBeginInfo next_info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vk_test::Require(context.DeviceProc<PFN_vkBeginCommandBuffer>("vkBeginCommandBuffer")(
                       context.command_buffer, &next_info),
                   "native entry vkBeginCommandBuffer");
  Check(addon::vk::released_features.empty() && addon::vk::RecordedUseComplete(retired_source_use) && device.NativeImage(mask) == VK_NULL_HANDLE,
        "a successful real native reset retires the released source without "
        "another Evaluate or Present");
  mask = {};
  mask_view = {};
  std::cout << "VULKAN_SOURCE_REPLAY evidence=NATIVE_GPU+MOCK source_closed=1 "
               "completed_replay=retained"
               " rgba=actual native_entry=detoured native_reset=success retired=drained\n";

  DestroyBuffer(context, &upload);
  DestroyBuffer(context, &motion_upload);
  DestroyBuffer(context, &readback);
  device.destroy_resource_view(mask_view);
  device.destroy_resource_view(depth_view);
  device.destroy_resource_view(motion_view);
  device.destroy_resource(mask);
  device.destroy_resource(depth);
  device.destroy_resource(motion);
  device.destroy_pipeline(addon::vk::control_mask_program);
  device.destroy_pipeline_layout(addon::vk::control_mask_layout);
  addon::vk::control_mask_program = {};
  addon::vk::control_mask_layout = {};
  std::cout << "PASS NATIVE_GPU+MOCK: production Vulkan ControlMask pipeline, depth subrect,"
               " Evaluate binding, submission replay, and reset/discard retirement\n";
}

struct RecoveryEvaluate {
  const NVSDK_NGX_Handle* handle;
  int reset;
  bool operator==(const RecoveryEvaluate&) const = default;
};

struct RecoveryMock {
  vk_test::VulkanContext* context;
  vk_test::CommandList* command_list;
  VkImage game_output;
  VkBuffer readback;
  VkDeviceSize marker_offset;
  std::array<uint64_t, 7> handle_tokens{};
  std::array<NVSDK_NGX_Parameter*, 7> create_parameters{};
  const char* borrowed_create_key = nullptr;
  size_t creates = 0;
  std::vector<NVSDK_NGX_Handle*> releases;
  std::vector<RecoveryEvaluate> evaluates;
  const NVSDK_NGX_Handle* fail_handle = nullptr;
  capability::Size last_grid;

  NVSDK_NGX_Handle* Handle(size_t index) {
    return reinterpret_cast<NVSDK_NGX_Handle*>(&handle_tokens.at(index));
  }
};

constexpr uint32_t kFaultMarker = 0xBAD00002;
RecoveryMock* recovery_mock = nullptr;

NVSDK_NGX_Result NVSDK_CONV CreateRecoveryFeature(
    VkDevice device, VkCommandBuffer command_buffer, NVSDK_NGX_Feature feature,
    NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** handle) {
  auto& mock = *recovery_mock;
  if (device != mock.context->device || command_buffer != mock.context->command_buffer
      || feature != addon::kFeatureDlssNr || parameters == nullptr || mock.creates == mock.handle_tokens.size()) {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  for (const char* key : {"DLSSNR.Color", "DLSSNR.Output", "DLSSNR.MVec", "DLSSNR.Depth"}) {
    void* borrowed = nullptr;
    parameters->Get(key, &borrowed);
    if (borrowed != nullptr) {
      mock.borrowed_create_key = key;
    }
  }
  mock.create_parameters[mock.creates] = parameters;
  *handle = mock.Handle(mock.creates++);
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV EvaluateRecoveryFeature(
    VkCommandBuffer command_buffer, const NVSDK_NGX_Handle* handle,
    const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback) {
  auto& mock = *recovery_mock;
  void* output = nullptr;
  const_cast<NVSDK_NGX_Parameter*>(parameters)->Get("DLSSNR.Output", &output);
  const auto* neural = static_cast<const NVSDK_NGX_Resource_VK*>(output);
  if (command_buffer != mock.context->command_buffer || neural == nullptr || !neural->ReadWrite
      || neural->Resource.ImageViewInfo.Image == mock.game_output
      || std::find(mock.releases.begin(), mock.releases.end(), handle) != mock.releases.end()) {
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
  }
  mock.evaluates.push_back({handle, addon::GetInt(parameters, "DLSSNR.Reset", -1)});
  mock.last_grid = {.width = addon::GetUInt(parameters, "DLSSNR.InputWidth"),
                    .height = addon::GetUInt(parameters, "DLSSNR.InputHeight")};
  using reshade::api::resource_usage;
  const reshade::api::resource image{vk_test::ApiHandle(neural->Resource.ImageViewInfo.Image)};
  const reshade::api::resource_view view{vk_test::ApiHandle(neural->Resource.ImageViewInfo.ImageView)};
  const auto storage = resource_usage::unordered_access;
  const auto copy_dest = resource_usage::copy_dest;
  const float color[] = {handle == mock.Handle(2) ? 0.875f : 0.625f, 0.25f, 0.125f, 1.f};
  mock.command_list->barrier(1, &image, &storage, &copy_dest);
  mock.command_list->clear_unordered_access_view_float(view, color, 0, nullptr);
  mock.command_list->barrier(1, &image, &copy_dest, &storage);
  if (handle == mock.fail_handle) {
    mock.context->DeviceProc<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(
        command_buffer, mock.readback, mock.marker_offset, sizeof(kFaultMarker), kFaultMarker);
    return NVSDK_NGX_Result_FAIL_UnableToInitializeFeature;
  }
  return NVSDK_NGX_Result_Success;
}

NVSDK_NGX_Result NVSDK_CONV ReleaseRecoveryFeature(NVSDK_NGX_Handle* handle) {
  recovery_mock->releases.push_back(handle);
  return NVSDK_NGX_Result_Success;
}

void RunEvaluateRecovery(bool hdr_zero_prefix) {
  using namespace reshade::api;
  using renodx::addons::dlss5::NrDeclineReason;
  using renodx::addons::dlss5::RuntimeLock;
  vk_test::VulkanContext context;
  vk_test::Device device(&context);
  vk_test::CommandList command_list(&context, &device);
  vk_test::CommandList discard_list(&context, &device);
  const VkCommandBuffer submit_buffer = context.command_buffer;
  VkCommandBuffer discard_buffer = VK_NULL_HANDLE;
  VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  allocation.commandPool = context.command_pool;
  allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  allocation.commandBufferCount = 1;
  vk_test::Require(context.DeviceProc<PFN_vkAllocateCommandBuffers>("vkAllocateCommandBuffers")(
                       context.device, &allocation, &discard_buffer), "allocate discard command buffer");
  constexpr uint32_t width = 16;
  constexpr uint32_t height = 16;
  constexpr std::array<uint8_t, 4> sdr_color{64, 128, 192, 255};
  constexpr std::array<uint16_t, 4> hdr_color{0x3400, 0x3800, 0x3a00, 0x3c00};
  const size_t pixel_size = (hdr_zero_prefix ? sizeof(hdr_color) : sizeof(sdr_color));
  const size_t output_size = width * height * pixel_size;
  const auto undefined = resource_usage::undefined;
  const auto storage = resource_usage::unordered_access;
  const auto sampled = resource_usage::shader_resource;
  const auto copy_dest = resource_usage::copy_dest;
  const auto copy_source = resource_usage::copy_source;
  VkPhysicalDeviceProperties gpu{};
  context.InstanceProc<PFN_vkGetPhysicalDeviceProperties>("vkGetPhysicalDeviceProperties")(context.physical, &gpu);
  std::cout << "Vulkan Evaluate recovery GPU=" << gpu.deviceName << " api="
            << VK_API_VERSION_MAJOR(gpu.apiVersion) << '.' << VK_API_VERSION_MINOR(gpu.apiVersion)
            << '.' << VK_API_VERSION_PATCH(gpu.apiVersion) << " driver=0x" << std::hex
            << gpu.driverVersion << std::dec << " queue_family=" << context.queue_family
            << " output=16x16 " << (hdr_zero_prefix ? "RGBA16_FLOAT" : "RGBA8_UNORM")
            << " depth=R32F motion=RG32F NGX=mock\n";

  struct Image {
    resource image;
    resource_view view;
    NVSDK_NGX_Resource_VK ngx;
  };
  const auto make_image = [&](format pixel_format, VkFormat vk_format, bool output) {
    Image image{};
    Check(device.create_resource(
              {width, height, 1, 1, pixel_format, 1, memory_heap::gpu_only,
               storage | sampled | copy_source | copy_dest},
              nullptr, undefined, &image.image), "create Vulkan recovery image");
    Check(device.create_resource_view(image.image, undefined, resource_view_desc(pixel_format), &image.view),
          "create Vulkan recovery image view");
    image.ngx = addon::vk::ImageResource(image.view, image.image, width, height, output, vk_format);
    return image;
  };
  auto output = (hdr_zero_prefix
      ? make_image(format::r16g16b16a16_float, VK_FORMAT_R16G16B16A16_SFLOAT, true)
      : make_image(format::r8g8b8a8_unorm, VK_FORMAT_R8G8B8A8_UNORM, true));
  auto depth = make_image(format::r32_float, VK_FORMAT_R32_SFLOAT, false);
  auto motion = make_image(format::r32g32_float, VK_FORMAT_R32G32_SFLOAT, false);
  Buffer readback = CreateHostBuffer(context, output_size + sizeof(kFaultMarker), VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  void* mapped = nullptr;
  vk_test::Require(context.DeviceProc<PFN_vkMapMemory>("vkMapMemory")(
                       context.device, readback.memory, 0, readback.size, 0, &mapped), "map recovery readback");
  auto* pixels = static_cast<uint8_t*>(mapped);
  auto* marker = reinterpret_cast<uint32_t*>(pixels + output_size);
  RecoveryMock mock{.context = &context, .command_list = &command_list,
                    .game_output = output.ngx.Resource.ImageViewInfo.Image,
                    .readback = readback.buffer, .marker_offset = output_size};
  recovery_mock = &mock;

  addon::hooks_enabled = false;
  addon::enabled = true;
  addon::stack_passes = (hdr_zero_prefix ? 2 : 3);
  addon::chained_temporal_history = true;
  addon::workset_free_policy = 0;
  addon::control_mask_enabled = false;
  addon::look_mode = false;
  addon::detail_stability = 1;
  addon::vk::extension_state = 1;
  addon::vk::runtime = {
      .create = CreateRecoveryFeature, .evaluate = EvaluateRecoveryFeature,
      .release = ReleaseRecoveryFeature, .shutdown = [](VkDevice) { return NVSDK_NGX_Result_Success; },
      .device = context.device};
  renodx::addons::dlss5::bridge::OwnedParameters parameters;
  const auto source_feature = (hdr_zero_prefix ? addon::kFeatureDlss : addon::kFeatureDlssd);
  if (hdr_zero_prefix) {
    parameters.Set(NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, NVSDK_NGX_DLSS_Feature_Flags_IsHDR);
    addon::transfer_strength = 0.f;
    addon::color_strength = 0.f;
    addon::pass_transfer_strength[0] = 1.f;
    addon::pass_color_strength[0] = 1.f;
    addon::display_pedestal = 1;
  }
  NVSDK_NGX_Handle* owner = nullptr;
  const auto create_game_feature = [&] {
    return addon::vk::CreateBody(addon::vk::kCreate1, context.device, context.command_buffer,
        source_feature, &parameters, &owner, [&] {
          owner = reinterpret_cast<NVSDK_NGX_Handle*>(&context);
          return NVSDK_NGX_Result_Success;
        });
  };
  Check(create_game_feature() == NVSDK_NGX_Result_Success, "register the game source through the Vulkan create body");
  auto& feature = addon::vk::features.at(owner);
  const auto read_configuration = [&] {
    RuntimeLock lock(addon::runtime_mutex);
    return addon::vk::ReadConfigurationFactsLocked();
  };
  addon::vk::slot_real[0].evaluate = reinterpret_cast<void*>(+[](
      VkCommandBuffer, const NVSDK_NGX_Handle*, const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback) {
    return NVSDK_NGX_Result_Success;
  });
  addon::vk::slot_real[0].release = reinterpret_cast<void*>(+[](NVSDK_NGX_Handle*) {
    return NVSDK_NGX_Result_Success;
  });
  parameters.Set(NVSDK_NGX_Parameter_Output, &output.ngx);
  parameters.Set(NVSDK_NGX_Parameter_Depth, &depth.ngx);
  parameters.Set(NVSDK_NGX_Parameter_MotionVectors, &motion.ngx);
  parameters.Set(NVSDK_NGX_Parameter_Reset, 0);

  const auto begin_frame = [&] {
    Begin(context);
    addon::vk::OnResetCommandList(&command_list);
  };
  const auto cleanup = [&] {
    context.command_buffer = discard_buffer;
    Begin(context);
    addon::vk::ForgetCommandList(&discard_list);
    context.command_buffer = submit_buffer;
    begin_frame();
    addon::vk::ForgetCommandList(&command_list);
    {
      RuntimeLock lock(addon::runtime_mutex);
      addon::vk::ShutdownNr(context.device);
      addon::vk::OnDestroyGameDevice();
    }
    recovery_mock = nullptr;
    context.DeviceProc<PFN_vkUnmapMemory>("vkUnmapMemory")(context.device, readback.memory);
    DestroyBuffer(context, &readback);
    for (const auto* image : {&output, &depth, &motion}) {
      device.destroy_resource_view(image->view);
      device.destroy_resource(image->image);
    }
  };

  const void* const expected_pixel = (hdr_zero_prefix ? static_cast<const void*>(hdr_color.data()) : sdr_color.data());
  const float game_clear[] = {
      (hdr_zero_prefix ? 0.25f : sdr_color[0] / 255.f),
      (hdr_zero_prefix ? 0.5f : sdr_color[1] / 255.f),
      (hdr_zero_prefix ? 0.75f : sdr_color[2] / 255.f), 1.f};
  const auto unchanged_output = [&] {
    for (size_t offset = 0; offset < output_size; offset += pixel_size) {
      if (std::memcmp(pixels + offset, expected_pixel, pixel_size) != 0) return false;
    }
    return true;
  };
  const auto record_frame = [&](vk_test::CommandList* list, const NVSDK_NGX_Handle* failed, bool wrapped = false,
                                const capability::Input* frozen_input = nullptr) {
    mock.evaluates.clear();
    mock.fail_handle = failed;
    mock.command_list = list;
    *marker = 0;
    list->barrier(1, &output.image, &storage, &copy_dest);
    list->clear_unordered_access_view_float(output.view, game_clear, 0, nullptr);
    list->barrier(1, &output.image, &copy_dest, &storage);
    auto result = NrDeclineReason::kCount;
    if (wrapped) {
      Check(addon::vk::HookedEvaluate<0>(context.command_buffer, owner, &parameters, nullptr) == NVSDK_NGX_Result_Success,
            "production Vulkan wrapper returns the original SR result");
    } else {
      auto input = (frozen_input != nullptr ? *frozen_input
          : addon::vk::UpscaledExecutionInput(source_feature == addon::kFeatureDlssd));
      result = addon::vk::ProcessNr(context.command_buffer, owner, &parameters, std::move(input));
    }
    list->barrier(1, &output.image, &storage, &copy_source);
    const VkBufferImageCopy copy{0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {}, {width, height, 1}};
    context.DeviceProc<PFN_vkCmdCopyImageToBuffer>("vkCmdCopyImageToBuffer")(
        context.command_buffer, mock.game_output, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        readback.buffer, 1, &copy);
    list->barrier(1, &output.image, &copy_source, &storage);
    const VkMemoryBarrier host_read{VK_STRUCTURE_TYPE_MEMORY_BARRIER, nullptr,
                                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    context.DeviceProc<PFN_vkCmdPipelineBarrier>("vkCmdPipelineBarrier")(
        context.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &host_read, 0, nullptr, 0, nullptr);
    return result;
  };
  const auto submitted_frame = [&](const NVSDK_NGX_Handle* failed = nullptr, bool wrapped = false,
                                   const capability::Input* frozen_input = nullptr) {
    begin_frame();
    const auto result = record_frame(&command_list, failed, wrapped, frozen_input);
    SubmitAndWait(context);
    return result;
  };
  const auto expect_evaluates = [&](std::initializer_list<RecoveryEvaluate> expected, const char* message) {
    Check(std::equal(mock.evaluates.begin(), mock.evaluates.end(), expected.begin(), expected.end()), message);
  };

  try {
    begin_frame();
    command_list.barrier(1, &output.image, &undefined, &storage);
    const float guide_clear[] = {0.f, 0.f, 0.f, 0.f};
    for (const auto* guide : {&depth, &motion}) {
      command_list.barrier(1, &guide->image, &undefined, &copy_dest);
      command_list.clear_unordered_access_view_float(guide->view, guide_clear, 0, nullptr);
      command_list.barrier(1, &guide->image, &copy_dest, &sampled);
    }
    SubmitAndWait(context);

    auto* const first = mock.Handle(0);
    auto* const failed = mock.Handle(1);
    auto* const third = mock.Handle(2);
    Check(submitted_frame() == NrDeclineReason::kSlotWarmupMaturation, "first Vulkan slot has a warmup frame");
    if (hdr_zero_prefix) {
      const auto original_is_sampled = [&] {
        return feature.worksets.front().state[addon::vk::kWork0] == sampled;
      };
      Check(submitted_frame() == NrDeclineReason::kCount && mock.evaluates.empty(),
            "HDR zero-strength prefix copies while the second pass matures");
      Check(original_is_sampled(), "HDR reference returns to a sampled layout before pedestal reduce and commit");
      const auto mature = submitted_frame();
      std::cout << "HDR mature: decline=" << static_cast<int>(mature) << " evaluates=" << mock.evaluates.size()
                << " unchanged=" << unchanged_output() << '\n';
      Check(mature == NrDeclineReason::kCount, "HDR matures after its zero-strength prefix");
      expect_evaluates({{failed, 1}}, "only the HDR second pass evaluates");
      Check(submitted_frame(failed) == NrDeclineReason::kFailureBackoff && *marker == kFaultMarker && unchanged_output(),
            "failed HDR frame preserves the original FP16 pixels even after the copied prefix");
      Check(submitted_frame() == NrDeclineReason::kCount && mock.evaluates.empty() && original_is_sampled(),
            "HDR recovery prefix restores its sampled layout during the rebuild frame");
      Check(submitted_frame() == NrDeclineReason::kCount && !unchanged_output(), "HDR recovery resumes edited output");
      expect_evaluates({{third, 1}}, "the rebuilt HDR second pass receives Reset");
      cleanup();
      Check(addon::stage_source_api.load() == capability::Api::UNKNOWN && !addon::stage_source_rr.load(),
            "Vulkan shutdown and device destruction retract the current Vulkan source observation");
      std::cout << "PASS NATIVE_GPU+MOCK: HDR prefix layout, failure pixels and Reset after retry\n";
      return;
    }
    for (size_t passes = 1; passes <= 3; ++passes) {
      Check(submitted_frame() == NrDeclineReason::kCount && mock.evaluates.size() == passes,
            "Vulkan warms up all three passes one slot at a time");
    }
    Check(submitted_frame() == NrDeclineReason::kCount && mock.creates == 3 && !unchanged_output(),
          "mature three-pass Vulkan chain delivers an edited game image");
    expect_evaluates({{first, 0}, {failed, 0}, {third, 0}}, "mature Vulkan passes use their own history");
    const std::vector<uint8_t> normal_output(pixels, pixels + output_size);
    std::cout << "warmup: three mature slots, Reset=0/0/0, edited output read back\n";

    const auto failed_use = feature.nr_use[1];
    // This recording uses the same middle handle with already initialized scratch.
    context.command_buffer = discard_buffer;
    Begin(context);
    addon::vk::OnResetCommandList(&discard_list);
    Check(record_frame(&discard_list, nullptr) == NrDeclineReason::kCount, "record discard on fully warmed stack");
    expect_evaluates({{first, 0}, {failed, 0}, {third, 0}}, "discard recording references the same three handles");
    context.DeviceProc<PFN_vkCmdFillBuffer>("vkCmdFillBuffer")(
        discard_buffer, readback.buffer, output_size, sizeof(kFaultMarker), 0xD15CA4D);
    vk_test::Require(context.DeviceProc<PFN_vkEndCommandBuffer>("vkEndCommandBuffer")(discard_buffer),
                     "end unsubmitted discard recording");
    context.command_buffer = submit_buffer;

    const auto successful = addon::successful_evaluations.load();
    const auto passes_before_failure = addon::vk::nr_passes.load();
    Check(submitted_frame(failed) == NrDeclineReason::kFailureBackoff, "middle Evaluate returns failure");
    expect_evaluates({{first, 0}, {failed, 0}}, "failure records only the successful prefix and middle attempt");
    Check(addon::vk::nr_passes.load() == passes_before_failure + 1
              && addon::successful_evaluations.load() == successful && *marker == kFaultMarker && unchanged_output(),
          "failed middle GPU work completed and the game output stayed byte-identical");
    Check(!addon::vk::RecordedUseComplete(failed_use), "fenced failed slot still has a replayable recording");
    {
      RuntimeLock lock(addon::runtime_mutex);
      addon::vk::DrainRetiredFeature(&feature);
    }
    Check(mock.releases.empty(), "GPU completion alone does not release the failed handle");
    *marker = 0;
    SubmitRecordedAndWait(context);
    Check(*marker == kFaultMarker && unchanged_output() && !addon::vk::RecordedUseComplete(failed_use),
          "same recording replays failed GPU work and still preserves the game output");
    std::cout << "submitted and replayed: marker=0x" << std::hex << *marker << std::dec
              << " original output unchanged, no release\n";

    const bool middle_retired = feature.slots[1].failed && feature.slots[1].failed_ns > 0
        && feature.slots[1].handle == nullptr && feature.retired_nr.size() == 1
        && feature.retired_nr.front().handle == failed && feature.retired_nr.front().use_id == failed_use;
    const bool recovery_history = feature.slots[0].handle == first && !feature.slots[0].failed
        && !feature.slots[0].pending_reset && feature.slots[2].handle == third && !feature.slots[2].failed
        && feature.slots[2].pending_reset && (feature.slots[2].pending_reset_reasons & addon::kResetRecovery) != 0;
    std::cout << "failure state: middle_latched=" << feature.slots[1].failed
              << " middle_detached=" << (feature.slots[1].handle == nullptr)
              << " retired_handles=" << feature.retired_nr.size()
              << " suffix_pending_reset=" << feature.slots[2].pending_reset << '\n';

    *marker = 0;
    begin_frame();
    Check(!addon::vk::RecordedUseComplete(failed_use),
          "the separate unsubmitted recording still retains the same middle handle");
    const auto first_retry = record_frame(&command_list, nullptr);
    SubmitAndWait(context);
    Check(mock.releases.empty(), "retry cannot release the middle handle still referenced by the discard recording");
    begin_frame();
    context.command_buffer = discard_buffer;
    Begin(context);
    addon::vk::OnResetCommandList(&discard_list);
    context.command_buffer = submit_buffer;
    Check(*marker == 0 && addon::vk::RecordedUseComplete(failed_use) && mock.releases.empty(),
          "discard retires the last recording reference without executing its GPU work");
    std::cout << "discarded: same handle, unsubmitted marker stayed 0, recording references retired\n";

    Check(middle_retired,
          "middle Evaluate failure latches backoff and retires its handle against the recording");
    Check(recovery_history,
          "successful prefix keeps history and skipped suffix keeps its handle with recovery Reset pending");

    Check(first_retry == NrDeclineReason::kCount, "mature prefix delivers during first retry creation");
    expect_evaluates({{first, 0}}, "new middle slot matures without evaluating the suffix");
    auto* const retry = mock.Handle(3);
    Check(mock.creates == 4 && feature.slots[1].handle == retry, "only the failed slot is rebuilt on the first retry");
    if (mock.borrowed_create_key != nullptr) {
      throw std::runtime_error(std::string("Feature 18 Create receives stale ") + mock.borrowed_create_key);
    }
    Check(mock.create_parameters[3] != mock.create_parameters[1],
          "retry allocates a clean parameter block while the failed feature's block is still referenced");

    Check(submitted_frame(retry) == NrDeclineReason::kFailureBackoff && mock.releases == std::vector{failed}
              && *marker == kFaultMarker && unchanged_output(),
          "consecutive middle failure completes GPU work and preserves the game output");
    expect_evaluates({{first, 0}, {retry, 1}}, "fresh middle slot receives Reset on its first Evaluate");
    Check(submitted_frame() == NrDeclineReason::kCount && mock.creates == 4
              && mock.releases == std::vector{failed, retry} && feature.slots[1].failed,
          "consecutive failure backs off instead of recreating immediately");
    expect_evaluates({{first, 0}}, "only the mature prefix runs during backoff");
    Check(!unchanged_output(), "backoff frame delivers the mature prefix");
    const std::vector<uint8_t> prefix_output(pixels, pixels + output_size);

    // Advance the failed slot's clock without sleeping through its retry interval.
    feature.slots[1].failed_ns -= addon::kFeatureRetryMaxNs;
    Check(submitted_frame() == NrDeclineReason::kCount && mock.creates == 5,
          "expired backoff recreates the middle slot");
    Check(mock.borrowed_create_key == nullptr, "all Feature 18 creates use fresh parameters");
    expect_evaluates({{first, 0}}, "expired-backoff creation still waits for maturation");
    auto* const recovered = mock.Handle(4);
    const auto resumed = submitted_frame();
    const bool matches_mature = std::memcmp(pixels, normal_output.data(), output_size) == 0;
    const bool matches_prefix = std::memcmp(pixels, prefix_output.data(), output_size) == 0;
    std::cout << "resumed: decline=" << static_cast<int>(resumed) << " releases=" << mock.releases.size()
              << " evaluates=" << mock.evaluates.size() << " matches_mature=" << matches_mature
              << " matches_prefix=" << matches_prefix << " first_r=" << static_cast<unsigned>(pixels[0])
              << " mature_r=" << static_cast<unsigned>(normal_output[0])
              << " prefix_r=" << static_cast<unsigned>(prefix_output[0]) << '\n';
    Check(resumed == NrDeclineReason::kCount && mock.releases == std::vector{failed, retry}
              && matches_mature && !matches_prefix,
          "recovered Vulkan stack delivers the full three-pass image");
    expect_evaluates({{first, 0}, {recovered, 1}, {third, 1}},
                     "recovered suffix sends Reset to the runtime without resetting the prefix");
    Check(submitted_frame() == NrDeclineReason::kCount, "recovered Vulkan stack stays active");
    expect_evaluates({{first, 0}, {recovered, 0}, {third, 0}}, "recovery Reset is consumed once");
    std::cout << "recovery: prefix-only cooldown, middle-only rebuild with clean parameters, Reset=0/1/1 then 0/0/0,"
                 " full edited output restored\n";

    addon::stack_passes = 2;
    addon::hook_point = addon::kHookRender;
    addon::pass_stage[0] = 2u;
    addon::stage_source_api = capability::Api::DX12;
    addon::stage_source_rr = false;
    auto& mixed_declines = renodx::addons::dlss5::nr_decline_counts[static_cast<size_t>(NrDeclineReason::kStagePlanUnavailable)];
    const auto before_mixed = mixed_declines.load();
    submitted_frame(nullptr, true);
    auto observed = read_configuration();
    Check(addon::CurrentExecutionApi() == capability::Api::VULKAN && addon::stage_source_rr.load()
              && observed.source == owner && observed.ray_reconstruction,
          "a rejected mixed request still replaces a stale D3D12 source with the current Vulkan source");
    Check(mixed_declines.load() == before_mixed + 1 && observed.recorded_passes == 0
              && observed.geometry.reference_width == 0 && mock.evaluates.empty() && unchanged_output(),
          "the mixed Vulkan request declines once without NR recording or writeback");
    addon::pass_stage[0] = 0u;
    addon::pass_resolution_mode[1] = 2u;
    addon::hook_point = addon::kHookPresent;
    addon::nr_resolution_scale = .67f;
    const auto frozen = addon::vk::UpscaledExecutionInput(true);
    addon::stack_passes = 3;
    Check(submitted_frame(nullptr, false, &frozen) == NrDeclineReason::kCount
              && mock.evaluates.size() == 2 && read_configuration().group.count == 2,
          "an in-flight Vulkan recording uses the frozen group after live Pass settings change");
    addon::stack_passes = 2;
    addon::stage_source_api = capability::Api::DX12;
    addon::stage_source_rr = false;
    submitted_frame(nullptr, true);
    observed = read_configuration();
    Check(observed.source == owner && observed.source_generation == feature.ngx.source_generation
              && observed.source_generation != 0 && observed.ray_reconstruction
              && observed.runtime == capability::Runtime::AVAILABLE,
          "Vulkan configuration facts identify the current RR source and initialized runtime");
    Check(addon::CurrentExecutionApi() == capability::Api::VULKAN && addon::stage_source_rr.load(),
          "the actual Vulkan RR source replaces a prior D3D12 source observation");
    Check(observed.group.stage == stage_plan::Stage::UPSCALED && observed.group.begin == 0
              && observed.group.count == 2 && observed.recorded_passes == 2
              && observed.geometry.reference_width == width && observed.geometry.reference_height == height
              && mock.evaluates.size() == 2 && mock.last_grid == capability::Size{width, height}
              && addon::hook_point.load() == addon::kHookPresent && addon::nr_resolution_scale.load() == .67f,
          "saved Present and 67 percent use the inherited full two-pass grid without rewriting intent");

    addon::pass_resolution_mode[0] = 2u;
    submitted_frame(nullptr, true);
    Check(mock.evaluates.empty() && unchanged_output() && read_configuration().recorded_passes == 0,
          "the same mode on an active pass declines without recording NR");
    addon::pass_resolution_mode[0] = 0u;
    addon::nr_follow_input_res = true;
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 12u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 10u);
    for (unsigned frame = 0; frame < 3; ++frame) {
      submitted_frame(nullptr, true);
    }
    observed = read_configuration();
    Check(observed.geometry.reference_width == 12 && observed.geometry.reference_height == 10
              && observed.recorded_passes == 2 && mock.last_grid == capability::Size{12, 10}
              && addon::nr_follow_input_res.load(),
          "saved Follow Input keeps its value and records the new full output rectangle");
    const auto old_generation = observed.source_generation;
    begin_frame();
    Check(addon::vk::HookedRelease<0>(owner) == NVSDK_NGX_Result_Success, "release the observed Vulkan source");
    observed = read_configuration();
    Check(observed.source == nullptr && observed.source_generation == 0 && observed.recorded_passes == 0
              && observed.geometry.reference_width == 0 && observed.runtime == capability::Runtime::AVAILABLE,
          "release withdraws source facts while the device runtime stays initialized");
    Check(addon::stage_source_api.load() == capability::Api::UNKNOWN && !addon::stage_source_rr.load(),
          "releasing the current Vulkan source retracts its shared API and RR observation");
    parameters.Set(NVSDK_NGX_Parameter_OutWidth, 0u);
    parameters.Set(NVSDK_NGX_Parameter_OutHeight, 0u);
    Check(create_game_feature() == NVSDK_NGX_Result_Success, "reuse the released Vulkan handle in a new source lifetime");
    {
      RuntimeLock lock(addon::runtime_mutex);
      addon::vk::runtime.device = nullptr;
      addon::vk::runtime.failed = true;
    }
    submitted_frame(nullptr, true);
    observed = read_configuration();
    Check(observed.source == owner && observed.source_generation > old_generation && observed.recorded_passes == 0
              && observed.runtime == capability::Runtime::UNAVAILABLE && mock.evaluates.empty(),
          "a reused handle has a fresh source generation and a latched Vulkan Init failure stays visible");

    const auto* foreign_handle = reinterpret_cast<const NVSDK_NGX_Handle*>(&mock);
    const auto observe_foreign_rr = [&] {
      RuntimeLock lock(addon::runtime_mutex);
      auto& foreign = addon::create_contracts[foreign_handle];
      foreign.source_type_confirmed = true;
      foreign.source_generation = 1;
      addon::created_feature_ids[foreign_handle] = addon::kFeatureDlssd;
      addon::ObserveSourcePlan(foreign_handle, stage_plan::Build({}));
    };
    observe_foreign_rr();
    Check(addon::CurrentExecutionApi() == capability::Api::DX12 && addon::stage_source_rr.load(),
          "a confirmed foreign D3D12 source supersedes the Vulkan observation");
    begin_frame();
    Check(addon::vk::HookedRelease<0>(owner) == NVSDK_NGX_Result_Success, "release Vulkan after D3D12 took over");
    Check(read_configuration().source == nullptr && addon::CurrentExecutionApi() == capability::Api::DX12
              && addon::stage_source_rr.load(),
          "Vulkan release removes only Vulkan facts after another backend takes over");
    Check(create_game_feature() == NVSDK_NGX_Result_Success, "register a new Vulkan source on the same device");
    submitted_frame(nullptr, true);
    Check(read_configuration().source == owner && addon::CurrentExecutionApi() == capability::Api::VULKAN,
          "the newer Vulkan producer replaces the foreign observation again");
    observe_foreign_rr();
  } catch (...) {
    cleanup();
    throw;
  }
  cleanup();
  const auto destroyed = read_configuration();
  Check(destroyed.runtime == capability::Runtime::UNKNOWN && destroyed.source == nullptr
            && destroyed.recorded_passes == 0 && destroyed.geometry.reference_width == 0,
        "device destruction withdraws the source, initialization and geometry observations");
  Check(addon::CurrentExecutionApi() == capability::Api::DX12 && addon::stage_source_rr.load(),
        "Vulkan device destruction preserves the foreign D3D12 API and RR observation");
  std::cout << "PASS NATIVE_GPU+MOCK: Vulkan wrapper configuration facts and inherited Upscaled grid\n";
  std::cout << "PASS NATIVE_GPU+MOCK: Vulkan Evaluate failure recovery and unsubmitted discard\n";
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    addon::addon_module = GetModuleHandleW(nullptr);
    if (argc == 2 && std::wstring(argv[1]) == L"--evaluate-recovery") {
      RunEvaluateRecovery(false);
    } else if (argc == 2 && std::wstring(argv[1]) == L"--evaluate-recovery-hdr") {
      RunEvaluateRecovery(true);
    } else if (argc > 1 && std::wstring(argv[1]) == L"--native-ngx") {
#if defined(NR_NEXUS_TEST_NATIVE_NGX)
      Check(argc == 3, "--native-ngx requires an existing ProjectIDPath argument");
      RunNativeNgx(argv[2]);
#else
      throw std::runtime_error("--native-ngx requires NR_NEXUS_TEST_NATIVE_NGX and the public NGX SDK library");
#endif
    } else if (argc > 1 && std::wstring(argv[1]) == L"--native-ngx-evaluate-only") {
      throw std::runtime_error("legacy snippet probe removed; use --native-ngx <ProjectIDPath> for public SDK validation");
    } else {
      RunProductionMaskDispatchMock();
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL Vulkan ControlMask: " << error.what() << '\n';
    return 1;
  }
}
