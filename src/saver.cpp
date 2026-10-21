// Mechanical port of m602_recon/src/SAVER.PAS.  Statement order, integer
// widths and signedness follow the Pascal source and the oracle code; the
// Pascal is quoted in comments where the C++ is not an obvious one-to-one
// transcription.  DOS/BIOS/hardware accesses go through Machine.
#include "saver.h"

#include <cassert>

namespace {

// Initialized data from the M602 data segment (Kamenicky / CP437 bytes).
// DS:539C BoxLines: array[0..4] of string[11]
const char* const BoxLines[5] = {
    "\xDA\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xBF",  // top border
    "Software602",                                    // bare label, no side borders
    "\xB3    o    \xB3",
    "\xB3    O    \xB3",
    "\xC0\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xD9",  // bottom border
};
// DS:53D8 InitialDY, DS:53DE InitialDX: array[1..3] of Integer
const int16_t InitialDY[4] = {0, -1, 1, -1};
const int16_t InitialDX[4] = {0, -1, 0, 1};

// res:1126:0131 display text conversion.  It translates bytes 80h..FFh
// through the active code-page table(s); ASCII is never changed and the six
// box-drawing codes the saver uses (B3 C4 DA BF C0 D9) are identity under
// the default KeybCS setting, so for every string this program converts the
// result equals the input.  (Only the table at DS:6E38, selected by a
// non-default "Cestina" setting, would remap the box characters.)
std::string ToDisplayText(const std::string& s) {
    return s;
}

// Signed LongInt helpers (resident 012C / 016A).
int32_t Maximum(int32_t a, int32_t b) { return a > b ? a : b; }
int32_t Minimum(int32_t a, int32_t b) { return a < b ? a : b; }

// Drivers.ClearScreen (res:0FD9:04A8): INT 10h AX=0600h, BH=07h over
// (0,0)-(ScreenWidth-1,ScreenHeight-1) fills the screen with blanks in
// attribute 07h; the following INT 10h AH=02h cursor home is not modelled.
void ClearScreen(TextScreen& s) {
    for (unsigned i = 0; i < unsigned(s.ScreenWidth) * s.ScreenHeight; ++i)
        s.mem[i] = 0x0720;
}

struct Frame : SaverLocals {
    Machine& m;
    explicit Frame(Machine& machine) : SaverLocals{}, m(machine) {}

    // procedure DrawString(X,Y: Byte; Text: string; Color: Byte)   ovl:61:0000
    //   Destination := ScreenBuffer + (Y*ScreenWidth+X)*2, then
    //   mov ah,Color / mov cl,[si] / @@next: lodsb; stosw; loop @@next
    void DrawString(uint8_t X, uint8_t Y, const std::string& Text, uint8_t Color) {
        TextScreen& s = m.screen;
        uint16_t di = uint16_t((Y * s.ScreenWidth + X) * 2);  // 16-bit offset, ScreenBuffer.Offset = 0
        // LOOP with CX = 0 would repeat 65536 times; every string here is non-empty.
        assert(!Text.empty() && Text.size() <= 255);
        uint16_t cx = uint16_t(Text.size());
        size_t si = 0;
        do {
            uint16_t ax = uint16_t(Color << 8 | uint8_t(Text[si++]));
            if (di < TextScreen::kWords * 2)  // B800:8000 and up is not text memory
                s.mem[di / 2] = ax;
            di = uint16_t(di + 2);
        } while (--cx != 0);
    }

    // procedure Pause(Milliseconds: Word)   ovl:61:0073
    //   FirstTick := Ticks;
    //   while (Milliseconds > (Ticks-FirstTick)*55) and (Ticks-FirstTick >= 0) do
    //     asm int $28 end
    // LongInt arithmetic, signed comparisons, B- short-circuit.  The second
    // condition ends the wait at once when the BIOS count wraps at midnight.
    void Pause(uint16_t Milliseconds) {
        int32_t FirstTick = m.BiosTicks();
        auto elapsed = [&] { return int32_t(uint32_t(m.BiosTicks()) - uint32_t(FirstTick)); };
        while (int32_t(Milliseconds) > int32_t(uint32_t(elapsed()) * 55u) && elapsed() >= 0)
            m.Int28();
    }

    // procedure Draw   ovl:61:0121
    void Draw() {
#pragma warning(suppress : 4458)  // Draw declares its own I, as in the Pascal
        int16_t I, J;
        uint16_t Year, MonthOrMinute, Day, Hour, Second, WeekdayOrFraction;
        // Erase pass: only the trailing column / row each box leaves behind.
        for (I = 1; I <= 3; ++I) {
            switch (NextX[I] - OldX[I]) {
            case 1:
                for (J = 0; J <= int16_t(BoxHeight); ++J)
                    DrawString(uint8_t(OldX[I]), uint8_t(OldY[I] + J), " ", 7);
                break;
            case -1:
                for (J = 0; J <= int16_t(BoxHeight); ++J)
                    DrawString(uint8_t(OldX[I] + 11 - 1), uint8_t(OldY[I] + J), " ", 7);
                break;
            }
            switch (NextY[I] - OldY[I]) {
            case 1:
                DrawString(uint8_t(OldX[I]), uint8_t(OldY[I]), ToDisplayText("           "), 7);
                break;
            case -1:
                DrawString(uint8_t(OldX[I]), uint8_t(OldY[I] + BoxHeight), ToDisplayText("           "), 7);
                break;
            }
        }
        // Draw pass: every box completely, in order 1, 2, 3 (3 ends up on top).
        for (I = 1; I <= 3; ++I) {
            uint8_t x = uint8_t(NextX[I]);
            for (J = 0; J <= int16_t(BoxHeight); ++J) {
                uint8_t y = uint8_t(NextY[I] + J);
                if (J == 1) {
                    switch (I) {
                    case 1:
                        DrawString(x, y, ToDisplayText(BoxLines[J]), 7);
                        break;
                    case 2:
                        m.GetDate(Year, MonthOrMinute, Day, WeekdayOrFraction);
                        DrawString(x, y, ToDisplayText("\xB3  " + TwoDigits(Day, 2) + "." +
                                                       TwoDigits(MonthOrMinute, 2) + ". \xB3"), 7);
                        break;
                    case 3:
                        m.GetTime(Hour, MonthOrMinute, Second, WeekdayOrFraction);
                        DrawString(x, y, ToDisplayText("\xB3  " + TwoDigits(Hour, 2) + ":" +
                                                       TwoDigits(MonthOrMinute, 2) + "  \xB3"), 7);
                        break;
                    }
                } else if (BoxHeight > 4) {  // 9-row boxes, ScreenHeight > 40 only
                    switch (J) {
                    case 2: case 3: case 5: case 6:
                        DrawString(x, y, ToDisplayText("\xB3         \xB3"), 7);
                        break;
                    case 4: DrawString(x, y, ToDisplayText(BoxLines[2]), 7); break;
                    case 7: DrawString(x, y, ToDisplayText(BoxLines[J - 4]), 7); break;
                    case 8: DrawString(x, y, ToDisplayText(BoxLines[4]), 7); break;
                    case 0: DrawString(x, y, ToDisplayText(BoxLines[J]), 7); break;
                    }
                } else {
                    DrawString(x, y, ToDisplayText(BoxLines[J]), 7);
                }
            }
            OldX[I] = NextX[I];
            OldY[I] = NextY[I];
        }
    }
};

}  // namespace

uint32_t NextRandSeed(uint32_t seed) {
    return seed * 134775813u + 1u;  // 08088405h, mod 2^32
}

// procedure Randomize: INT 21h AH=2Ch; RandSeed := DX shl 16 + CX
// (CH hour, CL minute, DH second, DL hundredths).
void Randomize(Machine& m) {
    uint16_t hour, minute, second, sec100;
    m.GetTime(hour, minute, second, sec100);
    uint16_t cx = uint16_t(hour << 8 | (minute & 0xFF));
    uint16_t dx = uint16_t(second << 8 | (sec100 & 0xFF));
    m.RandSeed = uint32_t(dx) << 16 | cx;
}

// function Random(Range: Word): Word: NextRand, then (RandSeed shr 16) mod Range
// (XCHG/DIV/XCHG on the high word; Range = 0 returns 0).
uint16_t Random(Machine& m, uint16_t range) {
    m.RandSeed = NextRandSeed(m.RandSeed);
    uint16_t hi = uint16_t(m.RandSeed >> 16);
    return range ? uint16_t(hi % range) : 0;
}

// res:119C:0201: Result[0] := Width; while Width > 0 do
//   begin Result[Width] := Chr(N mod 10 + 48); N := N div 10; Dec(Width) end
// (signed IDIV, so the digits come from truncating division).
std::string TwoDigits(int16_t n, int16_t width) {
    std::string s(uint8_t(width), '\0');
    while (width > 0) {
        s[width - 1] = char(n % 10 + 48);
        n = int16_t(n / 10);
        --width;
    }
    return s;
}

// procedure ScreenSaver(Application: PView; var StartupEvents: Word)   ovl:61:06D8
void ScreenSaver(Machine& m) {
    Frame f(m);
    TextScreen& s = m.screen;
    auto& [NextY, NextX, OldY, OldX, DY, DX, I, StepY, MaxY, MaxX, BoxHeight, Cycle] =
        static_cast<SaverLocals&>(f);
    // var StartupEvents: the caller (customised TProgram.GetEvent) passes 0.
    uint16_t StartupEvents = 0;

    StepY = m.InitialStepY;  // uninitialized locals in the original
    Cycle = m.InitialCycle;

    if (s.ScreenHeight > 40) BoxHeight = 8; else BoxHeight = 4;
    // SavedShift := ShiftState  -- see the exit test below.
    // INT 10h AH=03h saves the cursor position; not modelled (no cursor drawn).
    {
        std::lock_guard<std::mutex> g(s.lock);
        ClearScreen(s);
    }
    // HiddenCursor := ScreenHeight shl 8; INT 10h AH=02h parks the cursor on
    // the row below the screen, i.e. invisible.  The renderer draws no cursor.
    m.ScreenChanged();
    Randomize(m);
    MaxX = int16_t(s.ScreenWidth - 11);
    MaxY = int16_t(s.ScreenHeight - (BoxHeight + 1));
    for (I = 1; I <= 3; ++I) {
        OldX[I] = 30;  // fixed start column, not centred
        OldY[I] = int16_t((s.ScreenHeight - BoxHeight) >> 1);
        DX[I] = InitialDX[I];
        DY[I] = InitialDY[I];
        NextX[I] = OldX[I];
        NextY[I] = OldY[I];
    }
    for (;;) {
        // asm int $28 end -- DOS idle call; returns at once, nothing observable.
        for (I = 1; I <= 3; ++I) {
            NextX[I] = int16_t(NextX[I] + DX[I]);
            if (StepY) {  // one flag shared by all three boxes
                NextY[I] = int16_t(NextY[I] + DY[I]);
                StepY = 0;
            } else {
                StepY = 1;
            }
            if (NextX[I] > MaxX || NextX[I] < 0 || NextY[I] < 0 || NextY[I] > MaxY || Cycle % 5 == 0) {
                Cycle = 1;
                do {
                    DX[I] = int16_t(Random(m, 3) - 1);
                    DY[I] = int16_t(Random(m, 3) - 1);
                } while (!(DX[I] != 0 || DY[I] != 0));
            }
            NextX[I] = int16_t(Maximum(Minimum(NextX[I], MaxX), 0));
            NextY[I] = int16_t(Maximum(Minimum(NextY[I], MaxY), 0));
        }
        ++Cycle;
        {
            std::lock_guard<std::mutex> g(s.lock);
            f.Draw();
        }
        m.ScreenChanged();
        // First frame only: ten events are fetched and discarded.
        while (StartupEvents < 10) {
            // asm int $28 end
            ++StartupEvents;
            m.GetEvent(f);
        }
        // if (Event.What<>0) or (CheckModem and ModemWake) or (ShiftState<>SavedShift)
        //   then goto Finished;
        // The modem wake does not exist here, and every keyboard shift-state
        // change also arrives as a key event (see winmain.cpp).
        if (m.GetEvent(f) != 0) break;
        f.Pause(100);
    }
    // Finished: INT 10h AH=02h restores the saved cursor position; not modelled.
}
