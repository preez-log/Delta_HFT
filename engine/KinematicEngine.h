#pragma once
#include "Math.h"
#include "../trade/Types.h"
#include <array>
#include <numeric>

class KinematicEngine {
private:
    static const size_t WINDOW_SIZE = 50;
    
    // [Memory Layout]
    PhysicsState state_{0.0, 0.0, 0.0, 0.0};
    PhysicsParams params_;
    AVXMath math_;

    // Volume Ring Buffer
    std::array<double, WINDOW_SIZE> vol_buffer_;
    double vol_sum_ = 0.0;
    size_t head_ = 0;
    size_t count_ = 0;

    double signal_cooldown_ = 0.0;
    double cooldown_duration_ = 0.0;

public:
    // (k1, k2, k3) 
    KinematicEngine(double k1, double k2, double k3, double cooldown) 
        : cooldown_duration_(cooldown) {
        params_ = {k1, k2, k3, 0.0}; 
        vol_buffer_.fill(0.0);
    }

    // accel_thresh, obi_thresh, dummy, now
    int update(const TickData& tick, double accel_thresh, double obi_thresh, double dummy, double now) {
        if (tick.price <= 0.0001) return 0;

        // Volume Update
        double current_vol = tick.is_buy ? tick.volume : -tick.volume;
        vol_sum_ += current_vol - vol_buffer_[head_];
        vol_buffer_[head_] = current_vol;
        head_ = (head_ + 1) % WINDOW_SIZE;
        
        if (count_ < WINDOW_SIZE) count_++;

        if (count_ < WINDOW_SIZE) {
            state_.p = tick.price;
            return 0;
        }

        double prev_a = state_.a;

        // AVX2 Physics Update
        math_.update_physics(state_, params_, tick.price);

        if (now < signal_cooldown_) return 0;

        // Acceleration & Jerk
        double accel = state_.a;
        double jerk = accel - prev_a;

        double total_qty = tick.best_bid_qty + tick.best_ask_qty;
        double obi = (total_qty > 0) ? (tick.best_bid_qty - tick.best_ask_qty) / total_qty : 0.0;

        // [LONG Signal]
        if (vol_sum_ > 0 && accel > accel_thresh && obi > obi_thresh && jerk >= 0.0) {
            signal_cooldown_ = now + cooldown_duration_;
            return 1;
        }
        // [SHORT Signal] 
        else if (vol_sum_ < 0 && accel < -accel_thresh && obi < -obi_thresh && jerk <= 0.0) {
            signal_cooldown_ = now + cooldown_duration_;
            return -1;
        }

        return 0;
    }
};