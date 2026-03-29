#pragma once
#include <tuple>
#include "RollingZEngine.h"
#include "KinematicEngine.h"
#include "HawkesEngine.h"
#include "Regime.h"
#include "../core/Logger.h"

class EngineRouter {
private:
    // 모든 엔진 객체를 메모리에 살려둠
    std::tuple<RollingZEngine, KinematicEngine, HawkesEngine> engines_;
    MarketRegime current_regime_;

public:
    // 생성자에서 모든 엔진을 초기화 
    EngineRouter(const Config& config) 
        : current_regime_(MarketRegime::UNKNOWN),
          engines_(
              RollingZEngine(config.strategy.choppy.window, config.regime.ou_window,
                             config.strategy.signal_cooldown,
                             (size_t)config.strategy.choppy.ofi_window), // [OFI 추가]
              KinematicEngine(config.strategy.trending.k_p, config.strategy.trending.k_v, config.strategy.trending.k_a, config.strategy.signal_cooldown),
              HawkesEngine(config.strategy.toxic.alpha, config.strategy.toxic.beta, config.strategy.toxic.energy_thresh, config.strategy.toxic.signal_cooldown) 
          ) 
    {}

    // 레짐 스위처
    void switch_regime(MarketRegime new_regime) {
        if (current_regime_ == new_regime) return;
        current_regime_ = new_regime;
        //LOG(">>> [Router] Market Regime Switched -> %d", static_cast<int>(new_regime));
    }

    // 메인 루프에서 호출 업데이트 함수
    int update(const TickData& tick, const Config& config, double now) {
        
        // [백그라운드 웜업] 현재 레짐과 상관없이 모든 엔진의 버퍼에 틱 유지
        int sig_choppy = 0;
        // 횡보 장 (횡보시 업데이트)
        if (tick.volume < 0.0001) { 
            sig_choppy = std::get<0>(engines_).update(tick, config.strategy.choppy.z_score, config.strategy.choppy.obi, config.strategy.choppy.ou_thresh, now);
        }
        
        // 추세 장 (가속도 업데이트)
        int sig_trending = std::get<1>(engines_).update(tick, config.strategy.trending.accel, config.strategy.trending.obi, 0.0, now);
        
        // TOXIC 장 (호크스 업데이트)
        int sig_toxic = std::get<2>(engines_).update(tick, now);

        // [시그널 라우팅] 현재 레짐(장세)에 해당하는 엔진의 시그널만 리턴
        switch (current_regime_) {
            case MarketRegime::CHOPPY:   return sig_choppy;
            case MarketRegime::TRENDING: return sig_trending;
            case MarketRegime::TOXIC:    return sig_toxic;
            default:                     return 0;
        }
    }
};