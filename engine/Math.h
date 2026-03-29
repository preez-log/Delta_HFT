#pragma once
#include <immintrin.h>
#include <cmath>

class RollingZScore {
private:
    static constexpr size_t MAX_CAPACITY = 50000; // 스택/BSS 영역 고정 할당
    
    // AVX/SIMD 최적화를 위한 64바이트 메모리 정렬
    alignas(64) double buffer_[MAX_CAPACITY] = {0.0};
    
    size_t window_size_;
    size_t index_ = 0;
    size_t count_ = 0;
    
    // O(1) 업데이트용 누적합
    double sum_ = 0.0;
    double sum_sq_ = 0.0;

public:
    RollingZScore(size_t window_size) {
        window_size_ = (window_size > MAX_CAPACITY) ? MAX_CAPACITY : window_size;
    }
    
    inline double update(double val) {
        double old_val = (count_ < window_size_) ? 0.0 : buffer_[index_];
        
        buffer_[index_] = val;
        
        sum_ += (val - old_val);
        sum_sq_ += (val * val) - (old_val * old_val);
        
        index_++;
        if (index_ >= window_size_) index_ = 0; 
        
        if (count_ < window_size_) count_++;

        // 누적 오차 보정
        if (count_ == window_size_ && index_ == 0) {
            double exact_sum = 0.0;
            double exact_sum_sq = 0.0;
            
            for(size_t i = 0; i < window_size_; ++i) {
                exact_sum += buffer_[i];
                exact_sum_sq += buffer_[i] * buffer_[i];
            }
            sum_ = exact_sum;
            sum_sq_ = exact_sum_sq;
        }

        if (count_ < window_size_) return 0.0; 

        double inv_count = 1.0 / static_cast<double>(count_);
        double mean = sum_ * inv_count;
        double variance = (sum_sq_ * inv_count) - (mean * mean);
        
        if (variance <= 0.00000001) return 0.0; 
        
        return (val - mean) / std::sqrt(variance);
    }
};

// [32-Byte Aligned State]
// CPU 캐시 라인(64byte)의 절반을 딱 차지함.
// p: Position(가격), v: Velocity(속도), a: Acceleration(가속도), pad: Padding
struct alignas(32) PhysicsState {
    double p;
    double v;
    double a;
    double pad; // AVX2 256bit 레지스터(4 doubles)를 채우기 위한 패딩
};

struct alignas(32) PhysicsParams {
    double k_p; // Gain P
    double k_v; // Gain V
    double k_a; // Gain A
    double pad;
};

class AVXMath {
private:
    // 마찰 계수 (속도 0.9, 가속도 0.6)
    // _mm256_set_pd는 역순으로 들어감 (pad, a, v, p)
    const __m256d FRICTION = _mm256_set_pd(0.0, 0.6, 0.9, 1.0);
    
    // 가속도 클램핑 한계 (±50000.0)
    const double MAX_ACCEL = 50000.0;

public:
    // [Core Engine] 상태 업데이트
    // 1. 예측 -> 2. 잔차 계산 -> 3. 보정(Kalman Update) -> 4. 마찰 적용
    // 모든 과정이 인라인(inline)으로 처리됨.
    inline void update_physics(PhysicsState& state, const PhysicsParams& gains, double measured_price) {
        // 1. Load Data to Registers (메모리 -> CPU)
        __m256d y_state = _mm256_load_pd(&state.p);       // [p, v, a, pad]
        __m256d y_gains = _mm256_load_pd(&gains.k_p);     // [kp, kv, ka, pad]

        // 2. Predict (물리 법칙 적용)
        // P_new = P + V + 0.5A
        // V_new = V + A
        // A_new = A
        // 이 부분은 행렬 연산보다 스칼라가 빠를 수 있으나, 데이터 로 locality를 위해 여기서 처리
        double pred_p = state.p + state.v + 0.5 * state.a;
        double pred_v = state.v + state.a;
        double pred_a = state.a;

        // 예측값을 벡터로 만듦 (이 부분은 컴파일러가 최적화함)
        __m256d y_pred = _mm256_set_pd(0.0, pred_a, pred_v, pred_p);

        // 3. Calculate Residual (잔차 = 관측값 - 예측값)
        double residual = measured_price - pred_p;
        __m256d y_res = _mm256_set1_pd(residual); // [res, res, res, res]

        // 4. Update (Fused Multiply-Add)
        // State = Pred + (Gains * Residual)
        // 이 연산이 한 사이클에 수행됨
        __m256d y_next = _mm256_fmadd_pd(y_gains, y_res, y_pred);

        // 5. Apply Friction (마찰력 적용)
        // State = State * Friction
        y_next = _mm256_mul_pd(y_next, FRICTION);

        // 6. Store Data (CPU -> 메모리)
        _mm256_store_pd(&state.p, y_next);

        // 7. Clamp Acceleration (안전장치)
        // 벡터 연산보다 스칼라 분기가 쌀 수 있음 (Branch Prediction이 잘 됨)
        if (state.a > MAX_ACCEL) state.a = MAX_ACCEL;
        else if (state.a < -MAX_ACCEL) state.a = -MAX_ACCEL;
    }
};