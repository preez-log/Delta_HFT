#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <iomanip>
#include <cmath>
#include <cstring>
#include <algorithm>

// 기존 엔진 헤더들
#include "trade/Types.h"
#include "engine/EngineRouter.h"
#include "core/Config.h"
#include "engine/Regime.h" 

// =========================================================================
// [1] 비행 기록 장치 (Black Box)
// =========================================================================
struct TradeRecord {
    double entry_time;
    double exit_time;
    int signal;          
    double entry_price;
    double exit_price;
    double pnl;          
    double cum_pnl;      
    double duration;     
    bool is_market_exit; 
    MarketRegime regime; 
};

inline double fast_atof(const char* p, size_t len) {
    int64_t val = 0; size_t i = 0;
    while (i < len && p[i] >= '0' && p[i] <= '9') { val = val * 10 + (p[i] - '0'); i++; }
    if (i < len && p[i] == '.') {
        i++; int64_t frac = 1;
        while (i < len && p[i] >= '0' && p[i] <= '9') { val = val * 10 + (p[i] - '0'); frac *= 10; i++; }
        return static_cast<double>(val) / static_cast<double>(frac);
    }
    return static_cast<double>(val);
}

std::vector<TickData> load_history(const std::string& filename) {
    std::vector<TickData> ticks;
    std::cout << ">>> [Analyzer] Loading historical data from: " << filename << "...\n";

    std::ifstream file(filename, std::ios::ate | std::ios::binary);
    if (!file.is_open()) {
        std::cerr << ">>> [Error] Failed to load file: " << filename << "\n";
        return ticks;
    }
    
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::string buffer(size + 1, '\0'); 
    file.read(&buffer[0], size);
    
    ticks.reserve(5000000);

    double last_best_bid = 0.0;
    double last_best_ask = 0.0;
    double last_best_bid_qty = 0.0;
    double last_best_ask_qty = 0.0;

    char* ptr = &buffer[0];
    char* end = ptr + size;

    while (ptr < end) {
        char* next_line = (char*)memchr(ptr, '\n', end - ptr);
        if (!next_line) next_line = end;
        *next_line = '\0';

        char* e_ptr = strstr(ptr, "\"e\":\"");
        if (e_ptr) {
            e_ptr += 5; 
            TickData tick;
            std::memset(&tick, 0, sizeof(TickData));

            if (strncmp(e_ptr, "depthUpdate", 11) == 0) {
                char* time_ptr = strstr(e_ptr, "\"E\":");
                if (time_ptr) tick.timestamp = fast_atof(time_ptr + 4, 13) / 1000.0;

                char* b_ptr = strstr(e_ptr, "\"b\":[[\"");
                if (b_ptr) {
                    b_ptr += 7;
                    char* p_end = strchr(b_ptr, '"');
                    tick.best_bid = fast_atof(b_ptr, p_end - b_ptr);
                    // [FIX] best_bid_qty 파싱 추가
                    char* q_ptr = p_end + 3;
                    char* q_end = strchr(q_ptr, '"');
                    if (q_end) tick.best_bid_qty = fast_atof(q_ptr, q_end - q_ptr);
                }

                char* a_ptr = strstr(e_ptr, "\"a\":[[\"");
                if (a_ptr) {
                    a_ptr += 7;
                    char* p_end = strchr(a_ptr, '"');
                    tick.best_ask = fast_atof(a_ptr, p_end - a_ptr);
                    // [FIX] best_ask_qty 파싱 추가
                    char* q_ptr = p_end + 3;
                    char* q_end = strchr(q_ptr, '"');
                    if (q_end) tick.best_ask_qty = fast_atof(q_ptr, q_end - q_ptr);
                }

                if (tick.best_bid > 0.001 && tick.best_ask > 0.001) {
                    last_best_bid = tick.best_bid; last_best_ask = tick.best_ask;
                    last_best_bid_qty = tick.best_bid_qty; last_best_ask_qty = tick.best_ask_qty;
                    tick.price = (tick.best_bid + tick.best_ask) * 0.5;
                    tick.volume = 0;
                    ticks.push_back(tick);
                }
            }
            else if (strncmp(e_ptr, "aggTrade", 8) == 0) {
                char* time_ptr = strstr(e_ptr, "\"E\":");
                if (time_ptr) tick.timestamp = fast_atof(time_ptr + 4, 13) / 1000.0;

                char* p_ptr = strstr(e_ptr, "\"p\":\"");
                if (p_ptr) {
                    p_ptr += 5;
                    char* p_end = strchr(p_ptr, '"');
                    tick.price = fast_atof(p_ptr, p_end - p_ptr);
                }

                char* q_ptr = strstr(e_ptr, "\"q\":\"");
                if (q_ptr) {
                    q_ptr += 5;
                    char* q_end = strchr(q_ptr, '"');
                    tick.volume = fast_atof(q_ptr, q_end - q_ptr);
                }

                char* m_ptr = strstr(e_ptr, "\"m\":");
                if (m_ptr) tick.is_buy = (m_ptr[4] == 'f'); 

                tick.best_bid = last_best_bid; tick.best_ask = last_best_ask;
                tick.best_bid_qty = last_best_bid_qty; tick.best_ask_qty = last_best_ask_qty;
                ticks.push_back(tick);
            }
        }
        ptr = next_line + 1;
    }

    std::cout << ">>> [Analyzer] Successfully loaded " << ticks.size() << " ticks in milliseconds!\n";
    return ticks;
}

// =========================================================================
// [2] 백테스트 시뮬레이터 (단일 통합본)
// =========================================================================
class BacktestSimulator {
private:
    int state_ = 0;
    double inv_entry_price_ = 0.0;
    double entry_price_ = 0.0;
    double entry_time_ = 0.0;
    double position_qty_ = 0.0;
    MarketRegime entry_regime_ = MarketRegime::UNKNOWN; 
    
    double total_pnl_ = 0.0;
    const double maker_fee_ = 0.0000;
    const double taker_fee_ = 0.0004;
    const double slippage_rate_ = 0.0001;
    
    double max_pnl_pct_ = 0.0;
    double active_sl_ = 0.0; 
    double active_tp_ = 0.0; 

public:
    std::vector<TradeRecord> trade_logs_; 

    void enter(int signal, const TickData& tick, double qty, MarketRegime regime, double sl, double tp) {
        if (state_ != 0) return;
        
        state_ = signal;
        entry_time_ = tick.timestamp; 
        entry_regime_ = regime; 
        max_pnl_pct_ = 0.0;
        
        active_sl_ = sl; 
        active_tp_ = tp; 
        
        double slippage_penalty = tick.price * slippage_rate_;
        if (signal == 1) entry_price_ = tick.best_bid + slippage_penalty;
        else             entry_price_ = tick.best_ask - slippage_penalty;
        
        inv_entry_price_ = 1.0 / entry_price_;
        position_qty_ = qty;
        total_pnl_ -= (entry_price_ * position_qty_ * maker_fee_);
    }

    void exit(const TickData& tick, bool is_market = false) {
        if (state_ == 0) return;

        double exit_price = 0.0;
        double fee_rate = is_market ? taker_fee_ : maker_fee_;
        double slippage_penalty = tick.price * slippage_rate_;
        if (is_market) slippage_penalty *= 2.0; 

        if (state_ == 1) exit_price = (is_market ? tick.best_bid : tick.best_ask) - slippage_penalty;
        else             exit_price = (is_market ? tick.best_ask : tick.best_bid) + slippage_penalty;

        double raw_pnl = (state_ == 1) ? (exit_price - entry_price_) * position_qty_ 
                                       : (entry_price_ - exit_price) * position_qty_;
        
        double fee = (exit_price * position_qty_ * fee_rate);
        double net_pnl = raw_pnl - fee;
        total_pnl_ += net_pnl;

        TradeRecord rec;
        rec.entry_time = entry_time_;
        rec.exit_time = tick.timestamp;
        rec.signal = state_;
        rec.entry_price = entry_price_;
        rec.exit_price = exit_price;
        rec.pnl = net_pnl;
        rec.cum_pnl = total_pnl_;
        rec.duration = tick.timestamp - entry_time_;
        rec.is_market_exit = is_market;
        rec.regime = entry_regime_; 
        trade_logs_.push_back(rec);

        state_ = 0;
        entry_price_ = 0.0;
    }

    void update_position(const TickData& tick, double trail_start, double trail_drop) {
        if (state_ == 0) return;
        
        double current_val = (state_ == 1) ? tick.best_bid : tick.best_ask;
        double pnl_pct = (state_ == 1) ? (current_val - entry_price_) * inv_entry_price_ 
                                       : (entry_price_ - current_val) * inv_entry_price_;

        if (pnl_pct > max_pnl_pct_) max_pnl_pct_ = pnl_pct;

        // [Trailing Stop] 
        if (max_pnl_pct_ >= trail_start) {
            double trailing_cut_line = max_pnl_pct_ - trail_drop;
            if (pnl_pct <= trailing_cut_line) {
                exit(tick, true); 
                return;
            }
        }

        // [Hard Stop / Take Profit] 
        if (pnl_pct <= -active_sl_) exit(tick, true); 
        else if (pnl_pct >= active_tp_) exit(tick, false);
    }
    
    int get_state() const { return state_; }
};

// =========================================================================
// [3] 레짐이 포함된 궁극의 아스키 아트 렌더러
// =========================================================================
void draw_ascii_pnl(const std::vector<TradeRecord>& logs) {
    if (logs.empty()) {
        std::cout << ">>> [Warning] No trades to visualize.\n";
        return;
    }

    int height = 20; 
    int width = 80;  

    double max_pnl = logs[0].cum_pnl, min_pnl = logs[0].cum_pnl;
    for (const auto& log : logs) {
        if (log.cum_pnl > max_pnl) max_pnl = log.cum_pnl;
        if (log.cum_pnl < min_pnl) min_pnl = log.cum_pnl;
    }
    if (min_pnl > 0) min_pnl = 0.0; 
    if (max_pnl < 0) max_pnl = 0.0;

    std::cout << "\n==================== [ DELTABOT ASCII PNL CHART ] ====================\n";
    
    int step = std::max(1, (int)logs.size() / width);

    for (int y = height; y >= 0; --y) {
        double current_level = min_pnl + (max_pnl - min_pnl) * ((double)y / height);
        printf(" %8.3f $ | ", current_level);
        
        for (size_t x = 0; x < logs.size(); x += step) {
            if (std::abs(current_level) < (max_pnl - min_pnl) / (height * 2.0)) {
                std::cout << (logs[x].cum_pnl >= current_level ? "\033[32m?\033[0m" : "-"); 
            } 
            else if (logs[x].cum_pnl >= current_level) {
                std::cout << "\033[36m?\033[0m"; // Cyan
            } 
            else std::cout << " ";
        }
        std::cout << "\n";
    }
    std::cout << "            +" << std::string(width, '=') << ">\n";

    std::cout << "  [Regime]  | ";
    for (size_t x = 0; x < logs.size(); x += step) {
        MarketRegime r = logs[x].regime;
        if (r == MarketRegime::CHOPPY)        std::cout << "\033[33mC\033[0m"; // 노란색 C (횡보)
        else if (r == MarketRegime::TRENDING) std::cout << "\033[35mT\033[0m"; // 보라색 T (추세)
        else if (r == MarketRegime::TOXIC)    std::cout << "\033[31mX\033[0m"; // 빨간색 X (독성)
        else                                  std::cout << "U";
    }
    std::cout << " (C:Choppy, T:Trending)\n\n";
}

// =========================================================================
// [4] 메인 엔진 (라이브 봇 구조 완벽 동기화)
// =========================================================================
int main(int argc, char* argv[]) {
    std::string data_file = (argc > 1) ? argv[1] : "binance_data.log";
    std::string config_file = (argc > 2) ? argv[2] : "config.json";

    std::cout << ">>> Booting Live-Sim Digital Twin Analyzer...\n";
    
    // ★ 최적화 1: 수동 파싱 제거, 라이브 봇의 Config::load() 직접 사용!
    Config config;
    if (!config.load(config_file)) {
        std::cerr << ">>> [Error] Failed to load config.json!\n";
        return 1;
    }
    config.print_summary();

    std::vector<TickData> history = load_history(data_file);
    if (history.empty()) return 1;

    // ★ 최적화 2: 분리된 엔진 제거, 라이브 봇처럼 단일 EngineRouter 사용!
    RegimeDetector regime_detector(config.regime);
    EngineRouter engine_router(config);
    BacktestSimulator sim;

    double circuit_breaker_until = 0.0;
    bool engine_armed = false;
    MarketRegime last_regime = MarketRegime::UNKNOWN;

    double min_price_1m = 999999.0;
    double max_price_1m = 0.0;
    double taker_buy_vol_1m = 0.0;
    double taker_sell_vol_1m = 0.0;
    double last_reset_time = 0.0;

    int debug_regime_changes = 0;
    int debug_signals = 0;
    int debug_armed_ticks = 0;
    int debug_tick_count = 0;

    // 시뮬레이션 루프
    for (const auto& tick : history) {
        double now = tick.timestamp;
        debug_tick_count++;

        // [FIX 1] 첫 틱에서 reset 타이머 초기화
        if (last_reset_time == 0.0) last_reset_time = now;

        MarketRegime current_regime = regime_detector.update(tick, now);
        engine_router.switch_regime(current_regime);
        if (current_regime != last_regime) debug_regime_changes++;

        if (engine_armed) debug_armed_ticks++;

        // 1분 변동성 수집
        if (tick.price > max_price_1m) max_price_1m = tick.price;
        if (tick.price < min_price_1m) min_price_1m = tick.price;
        if (tick.volume > 0.0001) {
            if (tick.is_buy) taker_buy_vol_1m += (tick.price * tick.volume);
            else             taker_sell_vol_1m += (tick.price * tick.volume);
        }

        // 서킷브레이커
        if (now - last_reset_time >= config.regime.reset_interval_sec) {
            double volatility_pct = (max_price_1m - min_price_1m) / min_price_1m * 100.0;
            double net_delta = taker_buy_vol_1m - taker_sell_vol_1m;

            if (volatility_pct >= config.risk.circuit_breaker_vol_pct) {
                if (sim.get_state() != 0) sim.exit(tick, true);
                circuit_breaker_until = now + config.risk.circuit_breaker_cooldown;
                engine_armed = false;
            } else if (now >= circuit_breaker_until) {
                // [FIX 2] main.cpp와 동일: 최소 변동성만 있으면 무장
                if (volatility_pct >= 0.10 || std::abs(net_delta) >= 100000.0) engine_armed = true;
                else engine_armed = false;
            }

            min_price_1m = tick.price; max_price_1m = tick.price;
            taker_buy_vol_1m = 0; taker_sell_vol_1m = 0;
            last_reset_time = now;
        }

        // TOXIC 진입 시 Hawkes 엔진 무장
        if (current_regime == MarketRegime::TOXIC && last_regime != MarketRegime::TOXIC) {
            engine_armed = true;
        }
        // [FIX 3] CHOPPY/TRENDING 레짐 전환 시에도 무장 (main.cpp 동일)
        if ((current_regime == MarketRegime::CHOPPY || current_regime == MarketRegime::TRENDING)
            && last_regime == MarketRegime::UNKNOWN) {
            engine_armed = true;
        }
        last_regime = current_regime;

        if (sim.get_state() != 0) {
            sim.update_position(tick, config.risk.trail_start, config.risk.trail_drop);
        }

        if (now < circuit_breaker_until) {
            engine_router.update(tick, config, now);
            continue;
        }

        int signal = 0;
        if (engine_armed) {
            signal = engine_router.update(tick, config, now);
            if (signal != 0) debug_signals++;
        } else {
            engine_router.update(tick, config, now);
        }

        if (signal != 0 && engine_armed && sim.get_state() == 0) {
            double sl, tp;
            if (current_regime == MarketRegime::CHOPPY) {
                sl = config.strategy.choppy.stop_loss;
                tp = config.strategy.choppy.take_profit;
            } else if (current_regime == MarketRegime::TRENDING) {
                sl = config.strategy.trending.stop_loss;
                tp = config.strategy.trending.take_profit;
            } else { // TOXIC
                sl = config.strategy.toxic.stop_loss;
                tp = config.strategy.toxic.take_profit;
            }
            sim.enter(signal, tick, config.risk.qty, current_regime, sl, tp);
            engine_armed = false;
        }
    }

    std::cout << "\n>>> [DEBUG]\n";
    printf(" - Total ticks:      %d\n", debug_tick_count);
    printf(" - Regime changes:   %d\n", debug_regime_changes);
    printf(" - Armed ticks:      %d\n", debug_armed_ticks);
    printf(" - Signals fired:    %d\n", debug_signals);
    printf(" - Trades entered:   %zu\n\n", sim.trade_logs_.size());

    std::cout << "\n>>> Backtest Finished! Generating Ultimate ASCII Chart & Autopsy Report...\n";

    draw_ascii_pnl(sim.trade_logs_);
    
    // 장세별 성과 분리 계산
    double choppy_pnl = 0.0;
    double trending_pnl = 0.0;
    double toxic_pnl = 0.0;
    int choppy_trades = 0;
    int trending_trades = 0;
    int toxic_trades = 0;
    int choppy_wins = 0, trending_wins = 0, toxic_wins = 0;

    for(const auto& log : sim.trade_logs_) {
        if (log.regime == MarketRegime::CHOPPY) {
            choppy_pnl += log.pnl; choppy_trades++;
            if (log.pnl > 0) choppy_wins++;
        } else if (log.regime == MarketRegime::TRENDING) {
            trending_pnl += log.pnl; trending_trades++;
            if (log.pnl > 0) trending_wins++;
        } else if (log.regime == MarketRegime::TOXIC) {
            toxic_pnl += log.pnl; toxic_trades++;
            if (log.pnl > 0) toxic_wins++;
        }
    }

    std::cout << ">>> [PERFORMANCE BREAKDOWN]\n";
    printf(" - CHOPPY(C)   PnL: %8.4f $ | Trades: %3d | WinRate: %.1f%%\n",
           choppy_pnl, choppy_trades,
           choppy_trades > 0 ? (double)choppy_wins/choppy_trades*100.0 : 0.0);
    printf(" - TRENDING(T) PnL: %8.4f $ | Trades: %3d | WinRate: %.1f%%\n",
           trending_pnl, trending_trades,
           trending_trades > 0 ? (double)trending_wins/trending_trades*100.0 : 0.0);
    printf(" - TOXIC(X)    PnL: %8.4f $ | Trades: %3d | WinRate: %.1f%%\n",
           toxic_pnl, toxic_trades,
           toxic_trades > 0 ? (double)toxic_wins/toxic_trades*100.0 : 0.0);
    std::cout << "======================================================================\n";
    std::cout << ">>> [ULTIMATE TOTAL PnL]: " << (sim.trade_logs_.empty() ? 0.0 : sim.trade_logs_.back().cum_pnl) << " $\n\n";
    
    // ★ 최적화 3: BIG LOSS AUTOPSY REPORT (부검 리포트) 활성화!
    std::cout << "==================== [ BIG LOSS AUTOPSY REPORT ] ====================\n";
    int autopsy_count = 0;
    
    for (size_t i = 0; i < sim.trade_logs_.size(); ++i) {
        const auto& log = sim.trade_logs_[i];
        
        // 기준: -0.003$ 보다 큰 손실이 발생한 '치명적 손절' 색출 (qty에 따라 조절 가능)
        if (log.pnl < -0.003) { 
            const char* regime_str = (log.regime == MarketRegime::CHOPPY) ? "\033[33mCHOPPY  \033[0m" :
                                     (log.regime == MarketRegime::TRENDING) ? "\033[35mTRENDING\033[0m" :
                                     (log.regime == MarketRegime::TOXIC) ? "\033[31mTOXIC   \033[0m" : "UNKNOWN ";
            
            printf("[FATAL #%03zu] Time: %.1f | %s | %s | Dur: %5.1fs | Exit: %s | PnL: \033[31m%8.4f $\033[0m\n",
                   i + 1,
                   log.entry_time,
                   (log.signal == 1 ? "LONG " : "SHORT"),
                   regime_str,
                   log.duration,
                   (log.is_market_exit ? "MARKET(Slip)" : "LIMIT       "),
                   log.pnl);
            autopsy_count++;
        }
    }
    
    if (autopsy_count == 0) {
        std::cout << ">>> No fatal losses detected! Excellent Risk Management.\n";
    } else {
        std::cout << ">>> Total " << autopsy_count << " fatal trades found. Check the timestamps and regimes!\n";
    }
    std::cout << "=====================================================================\n";

    return 0;
}