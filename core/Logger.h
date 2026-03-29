#pragma once
#include <iostream>
#include <thread>
#include <atomic>
#include <cstdarg>
#include <cstring>
#include <chrono>
#include <iomanip>
#include <immintrin.h>

// CPU 캐시 라인 패딩 (False Sharing 방지)
#define CACHE_LINE 64

// 고정 크기의 링 버퍼
struct alignas(CACHE_LINE) LogEntry {
    std::atomic<bool> ready{false}; // Consumer에게 읽기 권한 위임
    char level[8];
    char tag[16];
    uint64_t epoch_us;
    char payload[400];
};

class AsyncLogger {
private:
    static constexpr size_t Q_SIZE = 8192; // 반드시 2의 제곱수여야 함
    static constexpr size_t Q_MASK = Q_SIZE - 1;
    
    LogEntry ring_[Q_SIZE];
    
    // 생산자(Core 1, 2, 3)용 인덱스
    alignas(CACHE_LINE) std::atomic<size_t> write_idx_{0};
    char pad1[CACHE_LINE - sizeof(std::atomic<size_t>)];
    
    // 소비자(Core 0)용 인덱스
    alignas(CACHE_LINE) size_t read_idx_{0};
    char pad2[CACHE_LINE - sizeof(size_t)];
    
    std::atomic<bool> running_{true};
    std::thread worker_;
public:
    static AsyncLogger& instance() {
        static AsyncLogger logger;
        return logger;
    }

    AsyncLogger() {
        worker_ = std::thread([this]() {
            while (running_) {
                size_t idx = read_idx_ & Q_MASK;
                LogEntry& entry = ring_[idx];

                // 데이터가 준비되었는지 확인 (Acquire)
                if (entry.ready.load(std::memory_order_acquire)) {
                    
                    // 1. 미뤄뒀던 시간 포맷팅을 백그라운드 스레드(Core 0)에서 일괄 처리
                    std::time_t now_c = entry.epoch_us / 1000000;
                    int micros = entry.epoch_us % 1000000;
                    struct tm tstruct;
                    localtime_r(&now_c, &tstruct);
                    
                    char time_buf[32];
                    std::strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tstruct);

                    // 2. 최종 출력
                    std::cout << "time=\"" << time_buf << "." 
                              << std::setfill('0') << std::setw(6) << micros 
                              << "\" level=\"" << entry.level 
                              << "\" log_type=\"" << entry.tag 
                              << "\" " << entry.payload << "\n";

                    // 3. 처리가 끝났으므로 슬롯 반환 (Release)
                    entry.ready.store(false, std::memory_order_release);
                    read_idx_++;
                } else {
                    // 비어있을 때는 Core 0의 점유율을 낮추기 위해 미세 대기
                    if (!running_) break;
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }
        });
    }

    ~AsyncLogger() {
        running_ = false;
        if (worker_.joinable()) worker_.join();
    }

    // [Wait-Free] 코어 1, 2, 3이 호출하는 핫 패스
    void log_kv(const char* level, const char* tag, const char* fmt, ...) {
        // 1. 원자적 덧셈으로 내 자리(Index) 확보 (O(1) 연산, 뮤텍스 없음!)
        size_t idx = write_idx_.fetch_add(1, std::memory_order_relaxed) & Q_MASK;
        LogEntry& entry = ring_[idx];

        // 2. 혹시라도 큐가 꽉 차서 한 바퀴 돌았다면 대기 (백프레셔)
        while (entry.ready.load(std::memory_order_acquire)) {
            _mm_pause();
        }

        // 3. 시간 포맷팅 생략! 타임스탬프 숫자만 캡처하고 도망감
        auto now = std::chrono::system_clock::now();
        entry.epoch_us = std::chrono::time_point_cast<std::chrono::microseconds>(now).time_since_epoch().count();

        // 4. 메타 데이터 복사
        std::strncpy(entry.level, level, sizeof(entry.level) - 1);
        std::strncpy(entry.tag, tag, sizeof(entry.tag) - 1);

        // 5. 메시지 페이로드 포맷팅
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(entry.payload, sizeof(entry.payload), fmt, args);
        va_end(args);

        // 6. 소비자에게 "내 슬롯 읽어라"고 승인 (Release)
        entry.ready.store(true, std::memory_order_release);
    }
    
    void log(const char* fmt, ...) {
        char temp_buf[400];
        va_list args;
        va_start(args, fmt);
        std::vsnprintf(temp_buf, sizeof(temp_buf), fmt, args);
        va_end(args);
        
        log_kv("info", "sys", "%s", temp_buf);
    }
};

// 매크로 정의 (사용 편의성)
#define LOG_INFO(tag, fmt, ...)  AsyncLogger::instance().log_kv("info", tag, fmt, ##__VA_ARGS__)
#define LOG_TRADE(tag, fmt, ...) AsyncLogger::instance().log_kv("trade", tag, fmt, ##__VA_ARGS__)
#define LOG_ERROR(tag, fmt, ...) AsyncLogger::instance().log_kv("error", tag, fmt, ##__VA_ARGS__)
#define LOG(fmt, ...) AsyncLogger::instance().log(fmt, ##__VA_ARGS__)
