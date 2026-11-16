/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace renodx::addons::dlss5::runtime_image {

using LoadFn = HMODULE (*)(const wchar_t*, DWORD*);

struct PrivateImage {
  HMODULE module = nullptr;
  std::filesystem::path source_path;
  std::filesystem::path path;
  std::filesystem::path directory;
  DWORD error = 0;
  DWORD exception = 0;
  std::string stage;
  bool ready = false;
};

// The module reference is deliberately not released here: an NR DLL that only
// ran DllMain can already own worker threads. Its caller owns retirement too.
inline PrivateImage LoadPrivate(const std::filesystem::path& source,
                                const std::filesystem::path& cache_root,
                                HMODULE shared_module, LoadFn load) {
  PrivateImage image;
  std::error_code error;
  image.source_path = std::filesystem::absolute(source, error);
  if (error || source.empty() || cache_root.empty() || load == nullptr) {
    image.stage = "source";
    image.error = (error ? static_cast<DWORD>(error.value()) : ERROR_INVALID_PARAMETER);
    return image;
  }
  const auto root = std::filesystem::absolute(cache_root, error);
  if (!error) std::filesystem::create_directories(root, error);
  if (error) {
    image.stage = "cache";
    image.error = static_cast<DWORD>(error.value());
    return image;
  }
  static std::atomic_uint64_t sequence{0};
  bool created = false;
  for (uint32_t attempt = 0; attempt < 32; ++attempt) {
    image.directory = root / (L"nr-nexus-native-" + std::to_wstring(GetCurrentProcessId())
        + L"-" + std::to_wstring(GetTickCount64()) + L"-"
        + std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed)));
    created = std::filesystem::create_directory(image.directory, error);
    if (created || error) break;
  }
  if (!created) {
    image.stage = "directory";
    image.error = (error ? static_cast<DWORD>(error.value()) : ERROR_ALREADY_EXISTS);
    image.directory.clear();
    return image;
  }
  image.path = image.directory / L"nvngx_dlssnr.dll";
  if (!CopyFileW(image.source_path.c_str(), image.path.c_str(), TRUE)) {
    image.stage = "copy";
    image.error = GetLastError();
    return image;
  }
  image.module = load(image.path.c_str(), &image.exception);
  if (image.module == nullptr) {
    image.stage = "load";
    image.error = (image.exception != 0 ? image.exception : GetLastError());
    return image;
  }
  if (image.module == shared_module) {
    image.stage = "shared_module";
    image.error = ERROR_INVALID_HANDLE;
    return image;
  }
  std::wstring loaded_path(32768, L'\0');
  const DWORD length = GetModuleFileNameW(image.module, loaded_path.data(), static_cast<DWORD>(loaded_path.size()));
  if (length == 0 || length >= loaded_path.size()) {
    image.stage = "module_path";
    image.error = (length == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER);
    return image;
  }
  loaded_path.resize(length);
  if (!std::filesystem::equivalent(image.path, loaded_path, error) || error) {
    image.stage = "module_path";
    image.error = (error ? static_cast<DWORD>(error.value()) : ERROR_INVALID_DATA);
    return image;
  }
  image.ready = true;
  return image;
}

// Only the exact file and empty directory created above are removed. The caller
// must first prove that this image is no longer mapped or otherwise referenced.
inline bool RemoveFiles(const PrivateImage& image) {
  if (image.directory.empty()) return image.path.empty();
  if (!image.directory.filename().wstring().starts_with(L"nr-nexus-native-")
      || (!image.path.empty() && image.path != image.directory / L"nvngx_dlssnr.dll")) return false;
  std::error_code error;
  if (!image.path.empty()) {
    const DWORD attributes = GetFileAttributesW(image.path.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_READONLY) != 0) {
      if (!SetFileAttributesW(image.path.c_str(), attributes & ~FILE_ATTRIBUTE_READONLY)) return false;
    }
    std::filesystem::remove(image.path, error);
    if (error) return false;
  }
  std::filesystem::remove(image.directory, error);
  return !error;
}

}  // namespace renodx::addons::dlss5::runtime_image
