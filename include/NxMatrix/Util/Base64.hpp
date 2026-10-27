#pragma once
#include "NxMatrix/Types.hpp"

#include <string>

namespace NxMatrix::Util {

// Standard base64 with padding (HTTP bodies, JSON fields).
std::string Base64Encode(const ByteArray &Data);

// URL-safe base64 without padding (Matrix keys, mxc media IDs).
std::string Base64EncodeUrlSafe(const ByteArray &Data);

// Accepts both alphabets, with or without padding. Returns an empty buffer on invalid input.
ByteArray Base64Decode(const std::string &Text);

} // namespace NxMatrix::Util