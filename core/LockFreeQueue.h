#pragma once
#include <atomic>
#include <cstddef>
#include <iostream>
#include <immintrin.h>

// [CPU Cache Line Size = 64 bytes]
#define CACHE_LINE_SIZE 64

template<typename T, size_t Capacity = 4096>
class LockFreeQueue {
    static_assert((Capacity != 0) && ((Capacity & (Capacity - 1)) == 0), 
                  "Capacity must be a power of 2");
private:
    struct Element {
        T data;
    };
    // 힙 할당 방지를 위해 스택/데이터 영역 배열 사용
    Element buffer_[Capacity];

    // [Producer가 쓰는 변수]
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> head_{0};
    size_t cached_tail_{0};
    
    // [Consumer가 쓰는 변수]
    alignas(CACHE_LINE_SIZE) std::atomic<size_t> tail_{0};
    size_t cached_head_{0};

public:
    // Producer: 데이터 넣기 (Wait-Free)
    bool push(const T& item) {
        const size_t current_head = head_.load(std::memory_order_relaxed);
        const size_t next_head = (current_head + 1) & (Capacity - 1);

        // 내 캐시 메모리에 있는 tail이 가리키는 곳까지는안전
        if (next_head == cached_tail_) {
            // 진짜로 꽉 찬 것 같을 때만 상대방 캐시 라인(tail_)을 훔쳐봄
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next_head == cached_tail_) return false; 
        }

        buffer_[current_head].data = item;
        // 데이터 기록 완료 후 head 업데이트
        head_.store(next_head, std::memory_order_release);
        return true;
    }

    // Consumer: 데이터 빼기 (Wait-Free)
    bool pop(T& item) {
        const size_t current_tail = tail_.load(std::memory_order_relaxed);

        // 내 캐시 메모리에 있는 head까지만 읽음
        if (current_tail == cached_head_) {
            // 진짜 비어있는 것 같을 때만 상대방 캐시 라인(head_)을 훔쳐봄
            cached_head_ = head_.load(std::memory_order_acquire);
            if (current_tail == cached_head_) return false; 
        }

        item = buffer_[current_tail].data;

        // 데이터 읽기 완료 후 tail 업데이트 (Release: Producer에게 보장)
        tail_.store((current_tail + 1) & (Capacity - 1), std::memory_order_release);
        return true;
    }

    bool empty() const {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }
};
