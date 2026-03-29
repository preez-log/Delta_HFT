#pragma once
#include <vector>
#include <cmath>
#include "../trade/Types.h"
#include "../core/Config.h"

enum class MarketRegime {
    UNKNOWN,   
    CHOPPY,    
    TRENDING,  
    TOXIC      
};

// [복원력 추정기]
class RollingOUEstimator {
private:
    struct DataPoint { double x, y; };
    size_t window_size_;
    size_t total_count_ = 0;
    size_t index_ = 0;
    double sum_x_ = 0, sum_y_ = 0, sum_xx_ = 0, sum_xy_ = 0;
    std::vector<DataPoint> history_;
    double last_price_ = 0;

public:
    RollingOUEstimator(size_t window_size) : window_size_(window_size) {
        history_.assign(window_size_, {0.0, 0.0}); 
    }

    double update(double current_price) {
        if (last_price_ == 0) {
            last_price_ = current_price;
            return 1.0; 
        }

        double x = last_price_;
        double y = current_price;
        last_price_ = current_price;

        if (total_count_ >= window_size_) {
            const DataPoint& old = history_[index_];
            sum_x_ -= old.x; sum_y_ -= old.y; 
            sum_xx_ -= old.x * old.x; sum_xy_ -= old.x * old.y;
        }

        sum_x_ += x; sum_y_ += y; sum_xx_ += x * x; sum_xy_ += x * y;
        history_[index_] = {x, y}; 
        total_count_++;
        
        // 롤 오버
        index_++;
        if (index_ >= window_size_) index_ = 0;

        size_t n = (total_count_ < window_size_) ? total_count_ : window_size_;
        if (n < 100) return 1.0; 

        double denominator = (n * sum_xx_) - (sum_x_ * sum_x_);
        if (denominator == 0.0) return 1.0;

        double inv_denom = 1.0 / denominator; 
        return ((n * sum_xy_) - (sum_x_ * sum_y_)) * inv_denom;
    }
};

// 실시간 레짐 판독기 - Division-Free & Config-Driven
class RegimeDetector {
private:
    RollingOUEstimator ou_estimator_;
    RegimeConfig conf_;
    
    double min_price_1m_ = 999999.0;
    double max_price_1m_ = 0.0;
    double taker_buy_vol_ = 0.0;
    double taker_sell_vol_ = 0.0;
    double last_reset_time_ = 0.0;
    
    MarketRegime current_regime_ = MarketRegime::UNKNOWN;

public:
    RegimeDetector(const RegimeConfig& config) 
        : ou_estimator_(config.ou_window), conf_(config) {}

    MarketRegime update(const TickData& tick, double now) {
        if (last_reset_time_ == 0.0) last_reset_time_ = now;
        
        double b_coeff = ou_estimator_.update(tick.price);
        if (tick.price > max_price_1m_) max_price_1m_ = tick.price;
        if (tick.price < min_price_1m_) min_price_1m_ = tick.price;
        
        if (tick.volume > 0.0001) { 
            if (tick.is_buy) taker_buy_vol_ += (tick.price * tick.volume);
            else             taker_sell_vol_ += (tick.price * tick.volume);
        }

        // 레짐 판독기 윈도우 사이즈
        if (now - last_reset_time_ >= conf_.reset_interval_sec) {
            double price_diff = max_price_1m_ - min_price_1m_;
            double total_vol = taker_buy_vol_ + taker_sell_vol_;
            double net_delta = std::abs(taker_buy_vol_ - taker_sell_vol_);

            MarketRegime new_regime = MarketRegime::CHOPPY; // 기본값

            // 1. TOXIC (광기장)
            if (price_diff >= min_price_1m_ * (conf_.toxic_vol_pct / 100.0) || net_delta > conf_.toxic_net_delta) {
                new_regime = MarketRegime::TOXIC;
            }
            // 2. TRENDING (추세장)
            else if (price_diff >= min_price_1m_ * (conf_.trending_vol_pct / 100.0) && 
                     net_delta > total_vol * conf_.trending_delta_ratio && 
                     b_coeff > conf_.trending_ou_thresh) {
                new_regime = MarketRegime::TRENDING;
            }

            if (new_regime != current_regime_) {
                const char* regime_names[] = {"UNKNOWN", "CHOPPY", "TRENDING", "TOXIC"};
                LOG(">>> [REGIME SHIFT] %s -> %s", regime_names[(int)current_regime_], regime_names[(int)new_regime]);
            }
            
            current_regime_ = new_regime;

            min_price_1m_ = tick.price; max_price_1m_ = tick.price;
            taker_buy_vol_ = 0; taker_sell_vol_ = 0;
            last_reset_time_ = now;
        }
        
        return current_regime_;
    }
    
    MarketRegime get_regime() const { return current_regime_; }
};