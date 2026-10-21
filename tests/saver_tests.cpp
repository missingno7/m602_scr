// Deterministic checks of the port.  The main one replays the traces in
// tests/dosref, recorded from the reconstructed SAVER.PAS compiled by Turbo
// Pascal 6.01 (see tests/dosref/gen_reference.py), and requires the same
// RandSeed, positions, directions, Cycle, StepY and screen contents after
// every frame.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "biostime.h"
#include "renderer.h"
#include "saver.h"
#ifdef _WIN32
#include "winmachine.h"
using RealMachine = WinMachine;
#else
#include <ctime>
#include "posixmachine.h"
using RealMachine = PosixMachine;
#endif

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// res:1570:1726 NextRand, register by register: RandSeed * 08088405h + 1.
uint32_t NextRandAsm(uint32_t seed) {
    uint16_t ax = uint16_t(seed), bx = uint16_t(seed >> 16), cx = ax, dx;
    uint32_t p = uint32_t(ax) * 0x8405u;  // mul word ptr cs:[175C]
    ax = uint16_t(p);
    dx = uint16_t(p >> 16);
    cx = uint16_t(cx << 3);                                            // shl cx,1 x3
    cx = uint16_t((cx & 0x00FF) | (((cx >> 8) + cx) & 0xFF) << 8);     // add ch,cl
    dx = uint16_t(dx + cx);
    dx = uint16_t(dx + bx);
    bx = uint16_t(bx << 2);                                            // shl bx,1 x2
    dx = uint16_t(dx + bx);
    dx = uint16_t((dx & 0x00FF) | (((dx >> 8) + bx) & 0xFF) << 8);     // add dh,bl
    bx = uint16_t(bx << 5);
    dx = uint16_t((dx & 0x00FF) | (((dx >> 8) + bx) & 0xFF) << 8);     // add dh,bl
    uint32_t sum = uint32_t(ax) + 1;                                   // add ax,1 / adc dx,0
    ax = uint16_t(sum);
    dx = uint16_t(dx + (sum >> 16));
    return uint32_t(dx) << 16 | ax;
}

void TestPrng() {
    uint32_t s = 0x12345678;
    for (int i = 0; i < 100000; ++i) {
        uint32_t seed = s;
        s = s * 2654435761u + 0x9E3779B9u;
        CHECK(NextRandAsm(seed) == NextRandSeed(seed));
        if (NextRandAsm(seed) != NextRandSeed(seed)) break;
    }
    CHECK(NextRandSeed(0) == 1);
    CHECK(NextRandSeed(1) == 0x08088406);
}

void TestTwoDigits() {
    CHECK(TwoDigits(7, 2) == "07");
    CHECK(TwoDigits(21, 2) == "21");
    CHECK(TwoDigits(0, 2) == "00");
    CHECK(TwoDigits(123, 2) == "23");
}

// The screen a frame must leave: blanks, then the three boxes in order.
std::vector<uint16_t> Composite(const SaverLocals& l, int width, const std::string& date, const std::string& time) {
    std::vector<uint16_t> screen(size_t(width) * 25, 0x0720);
    const std::string rows[5] = {
        "\xDA\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xBF", "",
        "\xB3    o    \xB3", "\xB3    O    \xB3",
        "\xC0\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xC4\xD9"};
    const std::string labels[4] = {"", "Software602", "\xB3  " + date + " \xB3", "\xB3  " + time + "  \xB3"};
    for (int i = 1; i <= 3; ++i)
        for (int j = 0; j < 5; ++j) {
            const std::string& text = j == 1 ? labels[i] : rows[j];
            for (int k = 0; k < 11; ++k)
                screen[size_t(l.NextY[i] + j) * width + l.NextX[i] + k] = uint16_t(0x0700 | uint8_t(text[k]));
        }
    return screen;
}

// Answers DOS and BIOS calls the way tests/dosref/MOVEREF.PAS does.
struct TraceMachine : Machine {
    int width = 80, frames = 0, calls = 0, mismatches = 0;
    uint16_t fakeCX = 0, fakeDX = 0, fakeMD = 0x0A15;
    int32_t ticks = 0;
    std::vector<std::string> expected;
    std::string name;

    int32_t BiosTicks() override { return ticks; }
    void Int28() override { ++ticks; }
    void GetDate(uint16_t& y, uint16_t& m, uint16_t& d, uint16_t& w) override {
        y = 1992;
        m = fakeMD >> 8;
        d = fakeMD & 0xFF;
        w = 3;
    }
    void GetTime(uint16_t& h, uint16_t& m, uint16_t& s, uint16_t& f) override {
        h = fakeCX >> 8;
        m = fakeCX & 0xFF;
        s = fakeDX >> 8;
        f = fakeDX & 0xFF;
    }
    void SetFakeClock(int frame) {
        uint16_t minute = uint16_t((frame / 7) % 60), hour = uint16_t((frame / 420 + 22) % 24);
        uint16_t day = uint16_t((frame / 97) % 31 + 1), month = uint16_t((frame / 1000) % 12 + 1);
        fakeCX = uint16_t(hour << 8 | minute);
        fakeMD = uint16_t(month << 8 | day);
    }
    uint16_t GetEvent(const SaverLocals& l) override {
        if (++calls <= 10) return 0;
        int frame = calls - 10;
        uint32_t hash = 0;
        for (int i = 0; i < width * 25; ++i) hash = hash * 31 + screen.mem[i];
        char line[256];
        std::snprintf(line, sizeof line, "%d %08X %d %d %d %d %d %d %d %d %d %d %d %d %u %u %08X", frame,
                      RandSeed, l.NextX[1], l.NextX[2], l.NextX[3], l.NextY[1], l.NextY[2], l.NextY[3], l.DX[1],
                      l.DX[2], l.DX[3], l.DY[1], l.DY[2], l.DY[3], l.Cycle, l.StepY, hash);
        if (size_t(frame) > expected.size() || expected[frame - 1] != line) {
            if (mismatches++ < 3)
                std::printf("FAIL %s frame %d\n  got      %s\n  expected %s\n", name.c_str(), frame, line,
                            size_t(frame) <= expected.size() ? expected[frame - 1].c_str() : "(none)");
        }
        // Independent check of the erase/draw logic: no stale cells, no gaps.
        std::string date = TwoDigits(fakeMD & 0xFF, 2) + "." + TwoDigits(fakeMD >> 8, 2) + ".";
        std::string time = TwoDigits(fakeCX >> 8, 2) + ":" + TwoDigits(fakeCX & 0xFF, 2);
        std::vector<uint16_t> want = Composite(l, width, date, time);
        if (std::memcmp(want.data(), screen.mem, want.size() * 2) != 0 && mismatches++ < 3)
            std::printf("FAIL %s frame %d: screen differs from the composite of the three boxes\n", name.c_str(),
                        frame);
        SetFakeClock(frame);
        return frame >= frames ? 0x0010 : 0;
    }
};

void TestTrace(const char* name) {
    std::ifstream in(std::string(TRACE_DIR) + "/" + name + ".trace");
    CHECK(in.good());
    if (!in) return;
    std::string header;
    std::getline(in, header);
    int params[8];
    std::istringstream hs(header.substr(1));
    for (int& p : params) hs >> p;
    TraceMachine m;
    m.name = name;
    m.width = params[0];
    m.frames = params[1];
    m.InitialStepY = uint8_t(params[2]);
    m.InitialCycle = uint16_t(params[3]);
    m.fakeCX = uint16_t(params[4] << 8 | params[5]);
    m.fakeDX = uint16_t(params[6] << 8 | params[7]);
    m.screen.ScreenWidth = uint8_t(m.width);
    m.screen.ScreenHeight = 25;
    for (std::string line; std::getline(in, line);)
        if (!line.empty()) m.expected.push_back(line);
    ScreenSaver(m);
    CHECK(m.calls - 10 == m.frames);
    CHECK(m.mismatches == 0);
    std::printf("trace %-20s %d frames, %d mismatches\n", name, m.calls - 10, m.mismatches);
}

// Frame timing as seen through BIOS ticks: Pause(100) waits for the second
// tick edge after it starts, so after the first frame every frame is exactly
// two ticks (131072 PIT clocks, 109.85 ms) apart.
struct ClockMachine : Machine {
    uint64_t pit = 0;
    std::vector<uint64_t> frameTimes;
    int frames = 0, calls = 0;
    int32_t BiosTicks() override { return biostime::TickCount(pit); }
    void Int28() override { pit = (pit / biostime::kClocksPerTick + 1) * biostime::kClocksPerTick; }
    void GetDate(uint16_t& y, uint16_t& m, uint16_t& d, uint16_t& w) override { y = 1992, m = 10, d = 21, w = 3; }
    void GetTime(uint16_t& h, uint16_t& m, uint16_t& s, uint16_t& f) override { h = 12, m = 0, s = 0, f = 0; }
    uint16_t GetEvent(const SaverLocals&) override {
        if (++calls <= 10) return 0;
        frameTimes.push_back(pit);
        return int(frameTimes.size()) >= frames ? 0x0010 : 0;
    }
};

void TestTiming() {
    ClockMachine m;
    m.pit = 123456789;  // arbitrary tick phase
    m.frames = 50;
    ScreenSaver(m);
    CHECK(m.frameTimes.size() == 50);
    CHECK(m.frameTimes[1] == (m.frameTimes[0] / 65536 + 2) * 65536);
    for (size_t i = 2; i < m.frameTimes.size(); ++i) CHECK(m.frameTimes[i] - m.frameTimes[i - 1] == 131072);

    // Across midnight the tick count drops to 0 and Pause returns early.
    ClockMachine w;
    w.pit = uint64_t(biostime::kTicksPerDay - 1) * 65536 + 1000;
    w.frames = 4;
    ScreenSaver(w);
    CHECK(w.frameTimes[1] == uint64_t(biostime::kTicksPerDay) * 65536);  // one tick, not two
    CHECK(w.frameTimes[2] - w.frameTimes[1] == 131072);

    // Monotonic-clock conversions used by the Windows machine.
    const uint64_t freq = 10000000;
    for (uint64_t clocks : {0ull, 1ull, 65535ull, 65536ull, 1193181ull, 1193182ull, 123456789012ull}) {
        uint64_t counts = biostime::CountsFromPitClocks(clocks, freq);
        CHECK(biostime::PitClocksFromCounts(counts, freq) >= clocks);
        CHECK(counts == 0 || biostime::PitClocksFromCounts(counts - 1, freq) < clocks);
    }
    CHECK(biostime::TickCount(biostime::PitClocksFromMilliseconds(3600000)) == 65543);  // 1 h = 65543 ticks
}

// The platform machine in real time: frames two BIOS ticks apart without
// busy-waiting.
struct TimedMachine : RealMachine {
#ifdef _WIN32
    explicit TimedMachine(int speed) : RealMachine(nullptr, speed) {}
#else
    explicit TimedMachine(int speed) : RealMachine(speed) {}
#endif
    std::vector<double> times;
    int calls = 0;
    uint16_t GetEvent(const SaverLocals&) override {
        if (++calls <= 10) return 0;
        times.push_back(std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count());
        return times.size() >= 26 ? evKeyDown : 0;
    }
    void ScreenChanged() override {}
};

double ThreadCpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    auto s = [](FILETIME t) { return (uint64_t(t.dwHighDateTime) << 32 | t.dwLowDateTime) / 1e7; };
    return s(k) + s(u);
#else
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + ts.tv_nsec / 1e9;
#endif
}

void TestRealTime(int speed) {
    TimedMachine m(speed);
    double cpu0 = ThreadCpuSeconds();
    ScreenSaver(m);
    double cpu = ThreadCpuSeconds() - cpu0;
    const double frame = 2 * 65536.0 / 1193182 * 100 / speed;  // 109.85 ms at 100 %
    double worst = 0;
    for (size_t i = 2; i < m.times.size(); ++i)
        worst = std::max(worst, std::abs(m.times[i] - m.times[i - 1] - frame));
    double wall = m.times.back() - m.times.front();
    double mean = (m.times.back() - m.times[1]) / double(m.times.size() - 2);
    std::printf("real time %d %%: mean frame %.3f ms (expected %.3f), worst deviation %.2f ms, CPU %.1f%%\n",
                speed, mean * 1000, frame * 1000, worst * 1000, 100 * cpu / wall);
    CHECK(std::abs(mean - frame) < 0.001);
    CHECK(worst < 0.02);  // generous for virtual machines
    CHECK(cpu / wall < 0.05);
}

void TestGeometry() {
    CHECK(TextColumnsForDisplay(1600, 1200) == 80);  // 4:3
    CHECK(TextColumnsForDisplay(1024, 768) == 80);
    CHECK(TextColumnsForDisplay(1920, 1200) == 96);  // 16:10
    CHECK(TextColumnsForDisplay(1920, 1080) == 107);  // 16:9
    CHECK(TextColumnsForDisplay(3840, 2160) == 107);
    CHECK(TextColumnsForDisplay(2100, 900) == 140);  // 21:9
    CHECK(TextColumnsForDisplay(3440, 1440) == 143);  // "21:9" panels are 43:18
    CHECK(TextColumnsForDisplay(1280, 1024) == 75);  // 5:4

    Renderer r;
    r.Configure(1920, 1080, 107, 25, {});
    PixelRect c = r.Content();  // fitted with the original proportions: tiny letterbox
    CHECK(c.right - c.left == 1920 && c.bottom - c.top >= 1074 && c.bottom - c.top <= 1080);
    r.Configure(1920, 1080, 80, 25, {});
    c = r.Content();  // fixed 80x25: 4:3 area with side bars
    CHECK(c.right - c.left == 1440 && c.bottom - c.top == 1080 && c.left == 240);
}

void TestRenderer() {
    // 1440x1080 = the 720x400 raster at 4:3: two output columns per raster
    // column, 2.7 output rows per raster line.
    Renderer r;
    r.Configure(1440, 1080, 80, 25, {});
    std::vector<uint16_t> cells(80 * 25, 0x0720);
    cells[0] = 0x07C4;        // horizontal line: 9th column repeats the 8th
    cells[1] = 0x07B3;        // vertical line: 9th column blank
    cells[2] = 0x07DA;        // corner
    cells[3] = 0x07DB;        // full block
    cells[4] = 0x0700 | 'A';  // letter: 9th column blank
    r.Update(cells.data());
    auto px = [&](int rasterX, int y) { return r.Pixels()[y * 1440 + 2 * rasterX]; };
    int y = 20;  // inside raster line 7 of the first text row
    CHECK(px(0, y) == 0xAAAAAA && px(7, y) == 0xAAAAAA && px(8, y) == 0xAAAAAA);
    CHECK(px(9 + 3, y) == 0xAAAAAA && px(9 + 8, y) == 0);
    CHECK(px(18 + 8, y) == 0xAAAAAA);
    CHECK(px(27 + 8, y) == 0xAAAAAA);
    CHECK(px(36 + 8, y) == 0);

    // Colour and scanlines change only the presentation.
    std::vector<uint16_t> block(80 * 25, 0x07DB);
    RenderSettings green;
    green.color = 0x33FF66;
    r.Configure(1440, 1080, 80, 25, green);
    r.Update(block.data());
    CHECK(r.Pixels()[540 * 1440 + 720] == 0x33FF66);
    RenderSettings strong;
    strong.scanlines = 2;
    r.Configure(1440, 1080, 80, 25, strong);
    r.Update(block.data());
    uint32_t boundary = r.Pixels()[270 * 1440 + 720] & 0xFF;  // raster line 100 starts at row 270
    uint32_t centre = r.Pixels()[271 * 1440 + 720] & 0xFF;
    CHECK(boundary < centre && centre <= 0xAA);
}

}  // namespace

int main() {
    TestPrng();
    TestTwoDigits();
    for (const char* name : {"w80", "w107", "w140_stepy1_cycle0", "w41_stepy1_cycle3"}) TestTrace(name);
    TestTiming();
    TestRealTime(100);
    TestRealTime(250);
    TestGeometry();
    TestRenderer();
    std::printf(g_failures ? "%d FAILURE(S)\n" : "all tests passed\n", g_failures);
    return g_failures ? 1 : 0;
}
