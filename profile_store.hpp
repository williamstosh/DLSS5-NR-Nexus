/*
 * Copyright (C) 2026
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "preset_fields.hpp"
#include "preset_store.hpp"

#pragma comment(lib, "bcrypt.lib")

namespace renodx::addons::dlss5::profiles {

using Json = presets::Json;
using Validator = std::function<bool(std::string_view, const Json&)>;

inline constexpr std::array<std::string_view, 6> SLOT_IDS = {
    "official_lite", "official_balanced", "official_max", "custom_slot_1", "custom_slot_2", "custom_slot_3"};

struct Slot {
  std::string name;
  bool populated = false;
  std::uint32_t template_version = 1;
  Json overrides = Json::object();
  Json snapshot = Json::object();
};

struct State {
  std::uint64_t revision = 0;
  std::string owner;
  std::string selected;
  Json last_applied = Json::object();
  std::array<Slot, 6> slots;
};

inline State EmptyState(std::string_view owner) {
  State state{.owner = std::string(owner)};
  for (std::size_t index = 0; index < state.slots.size(); ++index) {
    state.slots[index].populated = index < 3;
  }
  return state;
}

namespace internal {

using presets::internal::Fail;

inline bool ValidateSettings(const Json& settings, const Validator& validator, std::string* error) {
  if (!validator) return Fail(error, "A Slot settings validator is required.");
  if (!settings.is_object() || settings.size() > presets::MAX_SETTINGS) {
    return Fail(error, "Slot settings must be an object with at most 512 entries.");
  }
  for (const auto& [key, value] : settings.items()) {
    if (key.empty() || key.size() > 4096
        || !(value.is_boolean() || value.is_number() || value.is_string())
        || (value.is_number_float() && !std::isfinite(value.get<double>()))
        || (value.is_string() && value.get_ref<const std::string&>().size() > 128)) {
      return Fail(error, "Invalid Slot setting key or scalar value.");
    }
    if (!validator(key, value)) return Fail(error, "Unknown or invalid Slot setting: " + key);
  }
  return true;
}

// The exact key sets are also used on generated documents before publishing.
inline bool Decode(const Json& root, const Validator& validator, State* output, std::string* error) {
  if (!root.is_object() || root.size() != 7 || !root.contains("format") || !root.contains("schema_version")
      || !root.contains("revision") || !root.contains("owner") || !root.contains("selected")
      || !root.contains("last_applied") || !root.contains("slots")) {
    return Fail(error, "Invalid profiles manifest fields.");
  }
  if (root["format"] != "nr-nexus-profiles" || !root["schema_version"].is_number_unsigned()
      || root["schema_version"] != 1) return Fail(error, "Unsupported profiles format or schema version.");
  if (!root["revision"].is_number_unsigned() || !root["owner"].is_string() || !root["selected"].is_string()) {
    return Fail(error, "Invalid profiles revision, owner or selection.");
  }
  State state{.revision = root["revision"].get<std::uint64_t>(),
              .owner = root["owner"].get<std::string>(), .selected = root["selected"].get<std::string>(),
              .last_applied = root["last_applied"]};
  if (state.owner.empty() || state.owner.size() > 128 || state.owner.find('\0') != std::string::npos
      || (!state.selected.empty() && std::find(SLOT_IDS.begin(), SLOT_IDS.end(), state.selected) == SLOT_IDS.end())) {
    return Fail(error, "Invalid profiles owner or Slot ID.");
  }
  if (!root["slots"].is_object() || root["slots"].size() != SLOT_IDS.size()) {
    return Fail(error, "Profiles must contain exactly the six fixed Slot IDs.");
  }
  if (!ValidateSettings(state.last_applied, validator, error)) return false;
  for (std::size_t index = 0; index < SLOT_IDS.size(); ++index) {
    const auto id = SLOT_IDS[index];
    if (!root["slots"].contains(id)) return Fail(error, "Missing fixed Slot ID.");
    const auto& entry = root["slots"][id];
    if (!entry.is_object() || entry.size() != 5 || !entry.contains("name") || !entry.contains("populated")
        || !entry.contains("template_version") || !entry.contains("overrides") || !entry.contains("snapshot")
        || !entry["name"].is_string() || !entry["populated"].is_boolean()
        || !entry["template_version"].is_number_unsigned()) return Fail(error, "Invalid Slot fields or types.");
    const auto version = entry["template_version"].get<std::uint64_t>();
    const auto& name = entry["name"].get_ref<const std::string&>();
    if (version == 0 || version > (std::numeric_limits<std::uint32_t>::max)()
        || name.size() > 128 || name.find('\0') != std::string::npos) return Fail(error, "Invalid Slot name or template version.");
    auto& slot = state.slots[index];
    slot = Slot{.name = name, .populated = entry["populated"].get<bool>(),
                .template_version = static_cast<std::uint32_t>(version),
                .overrides = entry["overrides"], .snapshot = entry["snapshot"]};
    if (!ValidateSettings(slot.overrides, validator, error) || !ValidateSettings(slot.snapshot, validator, error)) return false;
    if (index < 3 && !slot.populated) return Fail(error, "Official Slots must be populated.");
  }
  if (!state.selected.empty()) {
    const auto index = static_cast<std::size_t>(std::find(SLOT_IDS.begin(), SLOT_IDS.end(), state.selected) - SLOT_IDS.begin());
    if (!state.slots[index].populated) return Fail(error, "Selected Slot is empty.");
  }
  *output = std::move(state);
  if (error != nullptr) {
    error->clear();
  }
  return true;
}

inline Json Encode(const State& state) {
  Json slots = Json::object();
  for (std::size_t index = 0; index < SLOT_IDS.size(); ++index) {
    const auto& slot = state.slots[index];
    slots[SLOT_IDS[index]] = {{"name", slot.name}, {"populated", slot.populated},
                            {"template_version", slot.template_version}, {"overrides", slot.overrides}, {"snapshot", slot.snapshot}};
  }
  return {{"format", "nr-nexus-profiles"}, {"schema_version", 1u}, {"revision", state.revision},
          {"owner", state.owner}, {"selected", state.selected}, {"last_applied", state.last_applied},
          {"slots", std::move(slots)}};
}

inline auto ResourceStrategyFields(Json* manifest) {
  std::array<Json*, 1 + 2 * SLOT_IDS.size()> fields{&(*manifest)["last_applied"]};
  for (std::size_t index = 0; index < SLOT_IDS.size(); ++index) {
    auto& slot = (*manifest)["slots"][SLOT_IDS[index]];
    fields[1 + 2 * index] = &slot["snapshot"];
    fields[2 + 2 * index] = &slot["overrides"];
  }
  return fields;
}

inline std::filesystem::path ResourceStrategyPath(const std::filesystem::path& manifest_path) {
  auto path = manifest_path;
  path += L".resource-strategy.json";
  return path;
}

inline std::string ManifestDigest(std::string_view manifest) {
  std::array<unsigned char, 32> hash{};
  if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
          reinterpret_cast<unsigned char*>(const_cast<char*>(manifest.data())), static_cast<ULONG>(manifest.size()), hash.data(),
          static_cast<ULONG>(hash.size())) < 0) return {};
  constexpr std::string_view digits = "0123456789abcdef";
  std::string digest;
  digest.reserve(hash.size() * 2);
  for (const auto value : hash) {
    digest += digits[value >> 4];
    digest += digits[value & 15];
  }
  return digest;
}

}  // namespace internal

inline bool StoragePath(std::string_view game_id, std::filesystem::path* output, std::string* error = nullptr) {
  if (output == nullptr) return internal::Fail(error, "Missing profiles path output.");
  if (game_id.size() != 32 || game_id.find_first_not_of("0123456789abcdefABCDEF") != std::string_view::npos) {
    return internal::Fail(error, "Game ID must contain exactly 32 hexadecimal digits.");
  }
  try {
    const DWORD required = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (required == 0 || required > 32768) return internal::Fail(error, "LOCALAPPDATA is unavailable or invalid.");
    std::wstring local(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", local.data(), required);
    if (length == 0 || length >= required) return internal::Fail(error, "LOCALAPPDATA changed or is invalid.");
    local.resize(length);
    std::string canonical_id(game_id);
    for (auto& digit : canonical_id) {
      if (digit >= 'A' && digit <= 'F') {
        digit += 'a' - 'A';
      }
    }
    const auto directory = std::filesystem::path(local) / NR_NEXUS_NAME / L"Profiles" / canonical_id;
    std::vector<presets::internal::Handle> ancestors;
    if (!presets::internal::OpenDirectory(directory, true, &ancestors, error)) return false;
    *output = directory / L"slots.json";
    if (error != nullptr) {
      error->clear();
    }
    return true;
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot resolve profiles path.");
  }
}

inline bool Read(const std::filesystem::path& path, const Validator& validator, State* output, std::string* error = nullptr) {
  if (output == nullptr) return internal::Fail(error, "Missing profiles output.");
  try {
    std::string manifest;
    Json root;
    if (!presets::internal::ReadTextFile(path, &manifest, error) || !presets::ParseJson(manifest, &root, error)) return false;
    State decoded;
    if (!internal::Decode(root, validator, &decoded, error)) return false;
    Json extension;
    if (presets::ReadJsonFile(internal::ResourceStrategyPath(path), &extension) && extension.is_object()) {
      const auto entry = extension.find(internal::ManifestDigest(manifest));
      if (entry != extension.end() && entry->is_array()) {
        const auto fields = internal::ResourceStrategyFields(&root);
        if (entry->size() == fields.size() && std::all_of(entry->begin(), entry->end(), [&](const Json& value) {
              return value.is_null() || validator(presets::ADAPTIVE_RESOURCE_STRATEGY_KEY, value);
            })) {
          for (std::size_t index = 0; index < fields.size(); ++index) {
            if (!(*entry)[index].is_null()) {
              (*fields[index])[presets::ADAPTIVE_RESOURCE_STRATEGY_KEY] = (*entry)[index];
            }
          }
          State extended;
          if (internal::Decode(root, validator, &extended, nullptr)) {
            decoded = std::move(extended);
          }
        }
      }
    }
    *output = std::move(decoded);
    return true;
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot read profiles or validate settings.");
  }
}

inline bool Write(const std::filesystem::path& path, State* state, std::uint64_t expected_revision,
                  const Validator& validator, std::string* error = nullptr) {
  if (state == nullptr) return internal::Fail(error, "Missing profiles state.");
  try {
    if (!presets::internal::IsComponent(path.filename().native())) return internal::Fail(error, "Unsafe profiles filename.");
    if (state->revision != expected_revision || expected_revision == (std::numeric_limits<std::uint64_t>::max)()) {
      return internal::Fail(error, "Profiles revision conflict or overflow.");
    }
    State checked;
    if (!internal::Decode(internal::Encode(*state), validator, &checked, error)) return false;
    std::vector<presets::internal::Handle> ancestors;
    if (!presets::internal::OpenDirectory(path.parent_path(), false, &ancestors, error)) return false;
    auto lock_path = path;
    lock_path += L".lock";
    // Keep the lock file: deleting it would allow another writer to lock a different object.
    presets::internal::Handle lock(CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                              OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (lock.value == INVALID_HANDLE_VALUE) return presets::internal::WindowsError(error, "Profiles lock unavailable (busy or unsafe)");
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(lock.value, &information)) return presets::internal::WindowsError(error, "Cannot inspect profiles lock");
    if ((information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) != 0
        || information.nNumberOfLinks != 1 || GetFileType(lock.value) != FILE_TYPE_DISK) {
      return internal::Fail(error, "Profiles lock must be a regular file without reparse points or hard links.");
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    const bool exists = attributes != INVALID_FILE_ATTRIBUTES;
    std::string existing_manifest;
    if (exists) {
      Json current_manifest;
      State current;
      if (!presets::internal::ReadTextFile(path, &existing_manifest, error)
          || !presets::ParseJson(existing_manifest, &current_manifest, error)
          || !internal::Decode(current_manifest, validator, &current, error)) return false;
      if (current.revision != expected_revision) return internal::Fail(error, "Profiles revision conflict; reload before saving.");
      if (current.owner != state->owner) return internal::Fail(error, "Profiles owner mismatch.");
    } else {
      if (GetLastError() != ERROR_FILE_NOT_FOUND) return presets::internal::WindowsError(error, "Cannot inspect profiles destination");
      if (expected_revision != 0) return internal::Fail(error, "Profiles revision conflict; destination is missing.");
    }
    checked.revision = expected_revision + 1;
    auto manifest = internal::Encode(checked);
    Json values = Json::array();
    bool has_resource_strategy = false;
    for (auto* fields : internal::ResourceStrategyFields(&manifest)) {
      const auto strategy = fields->find(presets::ADAPTIVE_RESOURCE_STRATEGY_KEY);
      if (strategy == fields->end()) {
        values.push_back(nullptr);
      } else {
        values.push_back(*strategy);
        fields->erase(strategy);
        has_resource_strategy = true;
      }
    }
    // v0.6.0 emits LF. Use Windows line endings so its save cannot match an
    // extension from an interrupted write for the same revision and fields.
    const auto text = manifest.dump(2) + "\r\n";
    if (has_resource_strategy) {
      const auto digest = internal::ManifestDigest(text);
      if (digest.empty()) return internal::Fail(error, "Cannot identify profiles for the resource strategy extension.");
      const auto extension_path = internal::ResourceStrategyPath(path);
      Json extension = Json::object();
      Json previous;
      if (exists && presets::ReadJsonFile(extension_path, &previous) && previous.is_object()) {
        const auto current_digest = internal::ManifestDigest(existing_manifest);
        if (const auto current = previous.find(current_digest); current != previous.end()) {
          extension[current_digest] = *current;
        }
      }
      extension[digest] = std::move(values);
      // Publish both revisions before replacing slots.json. An interrupted save
      // still opens the old manifest and its matching extension.
      if (!presets::WriteJsonFile(extension_path, extension.dump() + "\n", true,
              [&](const Json& root, std::string*) { return root == extension; }, error)) return false;
    }
    if (!presets::WriteJsonFile(path, text, exists,
                               [&](const Json& root, std::string* message) {
                                 State parsed;
                                 return internal::Decode(root, validator, &parsed, message);
                               }, error)) return false;
    state->revision = checked.revision;
    return true;
  } catch (const std::exception&) {
    return internal::Fail(error, "Cannot write profiles or validate settings.");
  }
}

}  // namespace renodx::addons::dlss5::profiles
