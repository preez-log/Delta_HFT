#pragma once
#include <cmath>
#include <algorithm>
#include "../core/Logger.h"
#include "../trade/Types.h"

class HawkesEngine {
private:
    double hawkes_energy_ = 0.0;
    double last_update_time_ = 0.0;
    double last_price_ = 0.0;
    
    // [호크스 파라미터]
    double alpha_;           // 충격 1회당 상승하는 에너지  
    double beta_;            // 감쇠율
    double energy_thresh_;   // 여진 매매가 가동되는 최소 에너지
    double signal_cooldown_;
    double last_signal_time_ = 0.0;

public:
    // json으로 초기화 
    HawkesEngine(double alpha, double beta, double threshold, double cooldown)
        : alpha_(alpha), beta_(beta), energy_thresh_(threshold), signal_cooldown_(cooldown) {}

    int update(const TickData& tick, double now) {
        if (last_update_time_ == 0.0) {
            last_update_time_ = now;
            last_price_ = tick.price;
            return 0;
        }

        double dt = now - last_update_time_;
        if (dt < 0) dt = 0;

        // [호크스 감쇠] 이전 에너지 시간차만큼 식혀줍니다.  
        hawkes_energy_ = hawkes_energy_ * std::exp(-beta_ * dt);

        // [충격 감지] 거래량이 크거나 가격이 튀면 에너지 펌핑
        double price_diff_pct = std::abs(tick.price - last_price_) / last_price_ * 100.0;
        if (price_diff_pct > 0.05 || tick.volume > 10.0) { 
            hawkes_energy_ += alpha_;
        }

        last_update_time_ = now;
        last_price_ = tick.price;

        if (now - last_signal_time_ < signal_cooldown_) return 0;

        // [여진 매매 로직] 에너지가 임계치를 넘을 때
        if (hawkes_energy_ > energy_thresh_) {
            
            // 스프레드(호가 간격) 확인: 평소보다 넓어졌을 때 유동성 진공 상태
            double spread = tick.best_ask - tick.best_bid;
            double spread_pct = spread / tick.price * 100.0;

            // 스프레드가 0.02% 이상 벌어졌을 때 (텅 빈 호가창)
            if (spread_pct > 0.02) { 
                double obi = (tick.best_bid_qty - tick.best_ask_qty) / (tick.best_bid_qty + tick.best_ask_qty + 1e-8);
                
                // OBI가 극단적일 때 그 방향으로 짧게
                if (obi > 0.8) {
                    // 매수벽이 압도적 (롱)
                    last_signal_time_ = now;
                    //LOG(">>> [Aftershock] Energy: %.2f | OBI: %.2f | SIGNAL: LONG", hawkes_energy_, obi);
                    return 1;
                } else if (obi < -0.8) {
                    // 매도벽이 압도적 (숏)
                    last_signal_time_ = now;
                    //LOG(">>> [Aftershock] Energy: %.2f | OBI: %.2f | SIGNAL: SHORT", hawkes_energy_, obi);
                    return -1;
                }
            }
        }
        
        return 0;
    }
};