#pragma once
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

namespace NxMatrix::Util {

// Matrix canonical JSON (spec "Canonical JSON"): keys sorted, no insignificant
// whitespace. Required before signing JSON objects with ed25519.
std::string CanonicalJson(const nlohmann::json &Value);

// Tolerant readers: missing keys or wrong types fall back to the given default.
std::string JsonGetString(const nlohmann::json &Object, const std::string &Key, const std::string &Fallback = {});
std::int64_t JsonGetInt(const nlohmann::json &Object, const std::string &Key, std::int64_t Fallback = 0);
bool JsonGetBool(const nlohmann::json &Object, const std::string &Key, bool Fallback = false);

} // namespace NxMatrix::Util