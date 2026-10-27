#pragma once
#include <cstdint>
#include <string>

namespace NxMatrix::Util {

// Milliseconds since the Unix epoch, from the system clock.
std::int64_t NowMs();

// Parses "2026-10-26T12:34:56.789Z" (Matrix timestamps). Returns 0 when unparseable.
std::int64_t EpochMsFromIso8601(const std::string &Iso8601);

std::string FormatClock(std::int64_t EpochMs);    // "14:35"
std::string FormatDayLabel(std::int64_t EpochMs); // "Today" / "Yesterday" / "26/10/2026"
std::string FormatDateTime(std::int64_t EpochMs); // "26/10/2026 14:35"
std::string FormatAgo(std::int64_t DeltaMs);      // "5m ago"

} // namespace NxMatrix::Util