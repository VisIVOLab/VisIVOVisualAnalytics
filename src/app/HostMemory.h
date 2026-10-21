#pragma once

#include <cstddef>
#include <cstdint>

/// Physical memory of this machine in bytes, or 0 when it cannot be read.
/// A single place for the three platform calls, so a feature that is about to
/// hold a multi-GB volume can say beforehand whether the machine has room.
std::uint64_t hostPhysicalMemoryBytes();
