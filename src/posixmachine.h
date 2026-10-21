// The emulated PC on macOS (any POSIX system): BIOS ticks from the monotonic
// clock, DOS date/time from the local clock.  Input is handled by the system,
// which stops the saver; GetEvent only reports that.
#pragma once

#include <sys/time.h>

#include <atomic>
#include <chrono>
#include <ctime>
#include <thread>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <pthread.h>
#endif

#include "biostime.h"
#include "saver.h"

class PosixMachine : public Machine {
public:
    // speedPercent scales the emulated timer (see WinMachine).
    explicit PosixMachine(int speedPercent = 100) : speed_(uint64_t(speedPercent)) {
        start_ = std::chrono::steady_clock::now();
        timeval tv;
        gettimeofday(&tv, nullptr);
        tm t;
        localtime_r(&tv.tv_sec, &t);
        pit0_ = biostime::PitClocksFromMilliseconds(((t.tm_hour * 60ull + t.tm_min) * 60 + t.tm_sec) * 1000 +
                                                    uint64_t(tv.tv_usec) / 1000);
    }

    std::atomic<bool> quit{false};
    std::atomic<bool> framePending{false};

    int32_t BiosTicks() override { return biostime::TickCount(PitNow()); }

    // INT 28h in Pause's polling loop: sleep until the next tick boundary.
    void Int28() override {
        uint64_t next = (PitNow() / biostime::kClocksPerTick + 1) * biostime::kClocksPerTick;
        uint64_t realClocks = ((next - pit0_) * 100 + speed_ - 1) / speed_;
        auto target = start_ + std::chrono::nanoseconds(biostime::CountsFromPitClocks(realClocks, 1000000000));
#ifdef __APPLE__
        // Plain sleeps get timer-coalescing leeway on macOS and wake late;
        // mach_wait_until on a user-interactive thread does not.
        static thread_local bool interactive = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0) == 0;
        (void)interactive;
        auto remaining = target - std::chrono::steady_clock::now();
        if (remaining <= remaining.zero()) return;
        mach_timebase_info_data_t tb;
        mach_timebase_info(&tb);
        uint64_t ns = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(remaining).count());
        mach_wait_until(mach_absolute_time() + ns * tb.denom / tb.numer);
#else
        std::this_thread::sleep_until(target);
#endif
    }

    void GetDate(uint16_t& year, uint16_t& month, uint16_t& day, uint16_t& dayOfWeek) override {
        tm t = Now(nullptr);
        year = uint16_t(t.tm_year + 1900);
        month = uint16_t(t.tm_mon + 1);
        day = uint16_t(t.tm_mday);
        dayOfWeek = uint16_t(t.tm_wday);  // 0 = Sunday, as DOS
    }
    void GetTime(uint16_t& hour, uint16_t& minute, uint16_t& second, uint16_t& sec100) override {
        int usec;
        tm t = Now(&usec);
        hour = uint16_t(t.tm_hour);
        minute = uint16_t(t.tm_min);
        second = uint16_t(t.tm_sec);
        sec100 = uint16_t(usec / 10000);
    }

    uint16_t GetEvent(const SaverLocals&) override { return quit ? evCommand : 0; }
    void ScreenChanged() override { framePending = true; }

private:
    static tm Now(int* usec) {
        timeval tv;
        gettimeofday(&tv, nullptr);
        tm t;
        localtime_r(&tv.tv_sec, &t);
        if (usec) *usec = int(tv.tv_usec);
        return t;
    }
    uint64_t PitNow() const {
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start_);
        return pit0_ + biostime::PitClocksFromCounts(uint64_t(ns.count()), 1000000000) * speed_ / 100;
    }

    uint64_t speed_;
    std::chrono::steady_clock::time_point start_;
    uint64_t pit0_;
};
