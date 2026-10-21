// M602 screen saver: mechanical port of m602_recon/src/SAVER.PAS
// (oracle overlay 61, ScreenSaver at ovl:61:06D8) and the pieces of the
// Turbo Pascal 6.01 RTL, Turbo Vision Drivers and M602 helpers it calls.
//
// Everything the original took from DOS, the BIOS or the video hardware is
// reached through Machine, one emulated PC: its text video memory, the BIOS
// tick counter, DOS date/time, INT 28h and the application's GetEvent.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>

// Colour text video memory, segment B800h.  A word per cell: character in
// the low byte, attribute in the high byte, row-major with ScreenWidth
// cells per row, as Turbo Vision's Drivers.ScreenBuffer addresses it.
struct TextScreen {
    static constexpr unsigned kWords = 0x4000;  // B800:0000..B800:7FFF (32 KB)
    uint16_t mem[kWords] = {};
    uint8_t ScreenWidth = 80;   // Drivers.ScreenWidth  (Byte)
    uint8_t ScreenHeight = 25;  // Drivers.ScreenHeight (Byte)
    // Not in the original: the renderer reads mem from another thread.
    std::mutex lock;
};

// ScreenSaver's local variables (SAVER.PAS), kept in one record so that
// tests can observe them at the same point the DOS reference harness does.
// Arrays are 1-based like the Pascal array[1..3]; element 0 is unused.
struct SaverLocals {
    int16_t NextY[4], NextX[4], OldY[4], OldX[4], DY[4], DX[4];
    int16_t I;
    uint8_t StepY;  // Boolean: nonzero = True
    int16_t MaxY, MaxX;
    uint16_t BoxHeight;
    uint16_t Cycle;
};

class Machine {
public:
    virtual ~Machine() = default;

    TextScreen screen;
    uint32_t RandSeed = 0;  // System.RandSeed

    // StepY and Cycle are never assigned before use in the original; they
    // hold whatever the stack contained.  These are the values this port
    // assumes were there (see docs/FIDELITY.md).
    uint8_t InitialStepY = 0;  // False
    uint16_t InitialCycle = 1;

    virtual int32_t BiosTicks() = 0;  // MemL[0:$46C], BIOS timer tick count
    virtual void Int28() = 0;         // INT 28h issued while Pause polls the ticks
    // INT 21h AH=2Ah / AH=2Ch, as returned by Dos.GetDate / Dos.GetTime.
    virtual void GetDate(uint16_t& year, uint16_t& month, uint16_t& day, uint16_t& dayOfWeek) = 0;
    virtual void GetTime(uint16_t& hour, uint16_t& minute, uint16_t& second, uint16_t& sec100) = 0;
    // Application^.GetEvent(Event); returns Event.What (0 = evNothing).
    virtual uint16_t GetEvent(const SaverLocals& locals) = 0;
    // Called after each Draw so the display can be refreshed.  Not in the
    // original, where the VGA scanned video memory continuously.
    virtual void ScreenChanged() {}
};

// ovl:61:06D8 ScreenSaver.  Returns when GetEvent reports an event.
void ScreenSaver(Machine& m);

// Turbo Pascal 6.01 System unit (res:1570:16D7 Random, 1726 NextRand, 175E Randomize).
void Randomize(Machine& m);
uint16_t Random(Machine& m, uint16_t range);
uint32_t NextRandSeed(uint32_t seed);

// res:119C:0201: Width decimal digits of N, zero-padded.
std::string TwoDigits(int16_t n, int16_t width);
