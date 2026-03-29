#pragma once
#include "../trade/Types.h"
#include "Math.h"
#include <array>

// ---------------------------------------------------------------
// OFI (Order Flow Imbalance) 롤링 합산기
// 매 틱마다 호가창 변화량을 누적해서 실제 주문 흐름 방향을 측정
// OBI와 달리 "지금 실제로 누가 사고 있는지"를 동적으로 포착
// ---------------------------------------------------------------
class RollingOFI {
private:
    static constexpr size_t MAX_WINDOW = 2000;
    alignas(64) double buffer_[MAX_WINDOW] = {0.0};

    size_t window_size_;
    size_t index_ = 0;
    size_t count_ = 0;
    double ofi_sum_ = 0.0;

    double prev_bid_qty_ = 0.0;
    double prev_ask_qty_ = 0.0;
    bool initialized_ = false;

public:
    RollingOFI(size_t window_size) {
        window_size_ = (window_size > MAX_WINDOW) ? MAX_WINDOW : window_size;
    }

    // 매 틱마다 호출 → 현재 OFI 합산값 반환
    double update(double bid_qty, double ask_qty) {
        if (!initialized_) {
            prev_bid_qty_ = bid_qty;
            prev_ask_qty_ = ask_qty;
            initialized_ = true;
            return 0.0;
        }

        // bid/ask 잔량 변화량 계산
        double bid_delta = bid_qty - prev_bid_qty_;
        double ask_delta = ask_qty - prev_ask_qty_;

        // OFI = 매수 변화 - 매도 변화
        // 양수: 매수 압력 증가, 음수: 매도 압력 증가
        double ofi = bid_delta - ask_delta;

        prev_bid_qty_ = bid_qty;
        prev_ask_qty_ = ask_qty;

        // 롤링 합산 업데이트
        double old_val = (count_ < window_size_) ? 0.0 : buffer_[index_];
        buffer_[index_] = ofi;
        ofi_sum_ += ofi - old_val;

        index_++;
        if (index_ >= window_size_) index_ = 0;
        if (count_ < window_size_) count_++;

        return ofi_sum_;
    }

    double get_sum() const { return ofi_sum_; }
    bool is_ready() const { return count_ >= window_size_ / 2; }
};

class EmaOUEstimator {
private:
    double alpha_;
    double ema_x_ = 0, ema_y_ = 0, ema_xx_ = 0, ema_xy_ = 0;
    double last_price_ = 0;
    bool initialized_ = false;

public:
    EmaOUEstimator(size_t window_size) {
        alpha_ = 2.0 / (window_size + 1.0);
    }

    double update(double current_price) {
        if (!initialized_) {
            last_price_ = current_price;
            ema_x_ = current_price; ema_y_ = current_price;
            ema_xx_ = current_price * current_price;
            ema_xy_ = current_price * current_price;
            initialized_ = true;
            return 1.0;
        }

        double x = last_price_;
        double y = current_price;

        ema_x_ = alpha_ * x + (1.0 - alpha_) * ema_x_;
        ema_y_ = alpha_ * y + (1.0 - alpha_) * ema_y_;
        ema_xx_ = alpha_ * (x * x) + (1.0 - alpha_) * ema_xx_;
        ema_xy_ = alpha_ * (x * y) + (1.0 - alpha_) * ema_xy_;

        last_price_ = current_price;

        double var_x = ema_xx_ - (ema_x_ * ema_x_);
        double cov_xy = ema_xy_ - (ema_x_ * ema_y_);

        if (var_x <= 1e-9) return 1.0; 
        return cov_xy / var_x;
    }
};

class RollingZEngine {
private:
    RollingZScore z_engine_;
    EmaOUEstimator ou_engine_;
    RollingOFI ofi_engine_;  // [OFI 추가]
    double signal_cooldown_ = 0.0;
    double cooldown_duration_;

public:
    RollingZEngine(size_t window_size, size_t ou_window, double cooldown_duration,
                   size_t ofi_window = 100)
        : z_engine_(window_size), ou_engine_(ou_window),
          ofi_engine_(ofi_window), cooldown_duration_(cooldown_duration) {}

    int update(const TickData& tick, double z_threshold, double obi_threshold,
               double ou_thresh, double now) {

        if (tick.price <= 0.0001) return 0;

        double z_score  = z_engine_.update(tick.price);
        double b_coeff  = ou_engine_.update(tick.price);

        // OFI 업데이트 (bookTicker 틱에만 의미있는 값)
        double ofi_sum  = ofi_engine_.update(tick.best_bid_qty, tick.best_ask_qty);

        if (now < signal_cooldown_) return 0;

        double total_qty = tick.best_bid_qty + tick.best_ask_qty;

        if (b_coeff <= ou_thresh && total_qty > 0.0001 && ofi_engine_.is_ready()) {

            double delta_qty     = tick.best_bid_qty - tick.best_ask_qty;
            double required_delta = obi_threshold * total_qty;

            // [OFI 필터] 실제 주문 흐름이 신호 방향과 일치할 때만 진입
            // 롱: OBI 매수벽 + OFI 양수 (실제 매수 주문 유입 중)
            // 숏: OBI 매도벽 + OFI 음수 (실제 매도 주문 유입 중)
            if (z_score < -z_threshold && delta_qty > required_delta && ofi_sum > 0) {
                signal_cooldown_ = now + cooldown_duration_;
                return 1;
            }
            else if (z_score > z_threshold && delta_qty < -required_delta && ofi_sum < 0) {
                signal_cooldown_ = now + cooldown_duration_;
                return -1;
            }
        }
        return 0;
    }
};
