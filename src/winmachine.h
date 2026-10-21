// The emulated PC behind each saver window: BIOS tick count from the
// monotonic clock, DOS date/time from the local clock, input events from
// the window.  See biostime.h for the tick model.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>

#include "biostime.h"
#include "saver.h"

constexpr UINT WM_APP_FRAME = WM_APP + 1;  // saver thread drew a frame

class WinMachine : public Machine {
public:
    // speedPercent scales the emulated timer: at 200 % the BIOS ticks come
    // twice as often, so the unchanged Pause(100) gives twice the frame rate.
    explicit WinMachine(HWND hwnd, int speedPercent = 100) : hwnd_(hwnd), speed_(uint64_t(speedPercent)) {
        LARGE_INTEGER f, now;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&now);
        freq_ = uint64_t(f.QuadPart);
        qpc0_ = uint64_t(now.QuadPart);
        // BIOS tick phase: PIT clocks since local midnight at qpc0_.
        SYSTEMTIME t;
        GetLocalTime(&t);
        pit0_ = biostime::PitClocksFromMilliseconds(
            ((t.wHour * 60ull + t.wMinute) * 60 + t.wSecond) * 1000 + t.wMilliseconds);
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer_) timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }
    ~WinMachine() override { CloseHandle(timer_); }

    std::atomic<int> pendingEvents{0};
    std::atomic<bool> quit{false};
    std::atomic<bool> framePosted{false};

    // MemL[0:$46C]: the IRQ 0 count, from the monotonic clock.
    int32_t BiosTicks() override { return biostime::TickCount(PitNow()); }

    // INT 28h in Pause's polling loop.  DOS returns at once and the loop
    // spins until the tick count changes; since nothing else can change
    // between two IRQ 0 ticks, sleep until the next tick boundary instead.
    void Int28() override {
        uint64_t next = (PitNow() / biostime::kClocksPerTick + 1) * biostime::kClocksPerTick;
        uint64_t realClocks = ((next - pit0_) * 100 + speed_ - 1) / speed_;
        uint64_t target = qpc0_ + biostime::CountsFromPitClocks(realClocks, freq_);
        int64_t counts = int64_t(target - Qpc());
        if (counts <= 0) return;
        LARGE_INTEGER due;
        due.QuadPart = -int64_t((uint64_t(counts) * 10000000 + freq_ - 1) / freq_);  // 100 ns, relative
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer_, INFINITE);
    }

    void GetDate(uint16_t& year, uint16_t& month, uint16_t& day, uint16_t& dayOfWeek) override {
        SYSTEMTIME t;
        GetLocalTime(&t);
        year = t.wYear;
        month = t.wMonth;
        day = t.wDay;
        dayOfWeek = t.wDayOfWeek;  // 0 = Sunday, as DOS
    }
    void GetTime(uint16_t& hour, uint16_t& minute, uint16_t& second, uint16_t& sec100) override {
        SYSTEMTIME t;
        GetLocalTime(&t);
        hour = t.wHour;
        minute = t.wMinute;
        second = t.wSecond;
        sec100 = t.wMilliseconds / 10;
    }

    // Application^.GetEvent: one queued input event per call.
    uint16_t GetEvent(const SaverLocals&) override {
        if (quit) return evCommand;
        int n = pendingEvents.load();
        while (n > 0 && !pendingEvents.compare_exchange_weak(n, n - 1)) {}
        return n > 0 ? evKeyDown : 0;
    }

    void ScreenChanged() override {
        if (!framePosted.exchange(true)) PostMessageW(hwnd_, WM_APP_FRAME, 0, 0);
    }

private:
    uint64_t Qpc() const {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        return uint64_t(now.QuadPart);
    }
    uint64_t PitNow() const {
        return pit0_ + biostime::PitClocksFromCounts(Qpc() - qpc0_, freq_) * speed_ / 100;
    }

    HWND hwnd_;
    uint64_t speed_;
    HANDLE timer_;
    uint64_t freq_, qpc0_, pit0_;
};
