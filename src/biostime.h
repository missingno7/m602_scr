// BIOS timer tick emulation (replaces the IRQ 0 tick count at 0040:006C).
//
// The PC's 8253/8254 PIT runs at 1193182 Hz and raises IRQ 0 every 65536
// clocks (about 18.2065 Hz, 54.925 ms).  The BIOS increments the doubleword
// at 0:046C on each interrupt and resets it to 0 when it reaches 1800B0h
// ticks (one day, 86399.9 s).  Here the tick count is derived from a
// monotonic clock: an initial PIT phase taken from the local time of day,
// plus the elapsed QueryPerformanceCounter time converted to PIT clocks.
#pragma once

#include <cstdint>

namespace biostime {

constexpr uint64_t kPitHz = 1193182;
constexpr uint64_t kClocksPerTick = 65536;
constexpr uint32_t kTicksPerDay = 0x1800B0;

// Whole PIT clocks in `counts` ticks of a counter running at `freq` Hz
// (floor; split to avoid 64-bit overflow).
inline uint64_t PitClocksFromCounts(uint64_t counts, uint64_t freq) {
    return counts / freq * kPitHz + counts % freq * kPitHz / freq;
}

// Smallest counter value whose PitClocksFromCounts is >= `clocks` (ceil).
inline uint64_t CountsFromPitClocks(uint64_t clocks, uint64_t freq) {
    return clocks / kPitHz * freq + (clocks % kPitHz * freq + kPitHz - 1) / kPitHz;
}

// PIT clocks since midnight for a local time of day in milliseconds.
inline uint64_t PitClocksFromMilliseconds(uint64_t ms) {
    return ms * kPitHz / 1000;
}

// Value of MemL[0:$46C] for an absolute PIT clock count since a midnight.
inline int32_t TickCount(uint64_t pitClocks) {
    return int32_t(pitClocks / kClocksPerTick % kTicksPerDay);
}

}  // namespace biostime
