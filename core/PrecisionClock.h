#pragma once
#include <chrono>
#include <thread>
#include <iostream>
#include <iomanip>
#include <sstream>

#include <time.h>        // clock_gettime (CLOCK_MONOTONIC_RAW)
#include <x86intrin.h>   // __rdtsc, _mm_lfence, _mm_pause

class PrecisionClock {
private:
    static inline uint64_t cpu_frequency_ = 0;
    static inline double period_ = 0.0;
    static inline uint64_t start_tick_ = 0;

public:
    using TimePoint = uint64_t;

    static void Calibrate() {
        struct timespec ts_start, ts_end;

        // OS의 하드웨어 타이머(Raw)를 이용해 시작 시간 측정
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_start);
        uint64_t tsc_start = __rdtsc();

        // 100ms 샘플링 대기
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        // 종료 시간 측정
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts_end);
        uint64_t tsc_end = __rdtsc();

        // 경과 시간 계산 (초 단위)
        double elapsed_sec = (ts_end.tv_sec - ts_start.tv_sec) + 
                             (ts_end.tv_nsec - ts_start.tv_nsec) / 1e9;
        uint64_t tsc_delta = tsc_end - tsc_start;

        // CPU 실제 주파수 계산
        cpu_frequency_ = static_cast<uint64_t>(tsc_delta / elapsed_sec);
        period_ = 1.0 / static_cast<double>(cpu_frequency_);
        start_tick_ = __rdtsc();

        std::cout << "[PrecisionClock] Linux Calibrated. CPU Freq: " 
                  << cpu_frequency_ / 1'000'000.0 << " MHz\n";
        std::cout << "[PrecisionClock] Resolution: " << period_ * 1e9 << " ns\n";
    }

    static inline double Now() {
        return GetTimeSeconds();
    }

    static inline TimePoint NowTicks() {
        //_mm_lfence(); // 파이프라인 비순차 실행 방지 HFT에선 실행속도가 정밀성보다 중요
        return __rdtsc();
    }

    static inline double GetTimeSeconds() {
        uint64_t now = NowTicks();
        if (now < start_tick_) return 0.0;
        return TicksToSeconds(now - start_tick_);
    }

    static std::string GetDateString() {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);

        std::stringstream ss;
        struct tm buf;
        localtime_r(&in_time_t, &buf); 

        ss << std::put_time(&buf, "%y%m%d-%H%M%S");
        return ss.str();
    }

    static inline uint64_t SecondsToTicks(double seconds) {
        if (seconds < 0.0) return 0;
        return static_cast<uint64_t>(seconds * static_cast<double>(cpu_frequency_));
    }

    static inline double TicksToSeconds(uint64_t ticks) {
        return static_cast<double>(ticks) * period_;
    }

    static void WaitUntil(double target_time) {
        double now = GetTimeSeconds();
        while (now < target_time) {
            double remaining = target_time - now;

            if (remaining > 0.002) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            else {
                _mm_pause(); // Spin lock 최적화
            }
            now = GetTimeSeconds();
        }
    }
};