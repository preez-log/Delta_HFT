#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <sstream>
#include <iomanip>
#include <cmath>
#include "simdjson.h"
#include <cstring>
#include <algorithm>
#include <thread>
#include <mutex>
#include <atomic>

#include "trade/Types.h"
#include "engine/EngineRouter.h"
#include "core/Config.h"
#include "engine/Regime.h"

struct SearchTask {
    Config config;
    char param_desc[64];
};

struct SimResult {
    char param_desc[64];
    double sl;
    double tp;
    double win;
    int trades;
    int wins;
    double win_rate;
    double pnl;
};

// ============================================================
// [진단용] CHOPPY 레짐의 b_coeff 분포를 수집하는 구조체
//
// b_coeff(AR1 계수)의 의미:
//   < 0.995      : 강한 평균회귀 → RollingZEngine이 잘 작동해야 함
//   0.995~0.999  : 약한 평균회귀
//   0.999~1.0    : 거의 랜덤워크
//   > 1.0        : 추세적 → 평균회귀 전략에 최악
//
// 진단 결과 해석:
//   CHOPPY 틱의 b_coeff가 대부분 1.0 근처
//     → RegimeDetector가 너무 관대하게 CHOPPY를 찍는 것이 문제
//   b_coeff는 낮은데도 승률이 48~49%
//     → z_score+OBI+OFI 신호 조합 자체의 예측력이 문제
// ============================================================
struct ChoppyDiagnostics {
    long long bucket_strong_mr   = 0;  // b < 0.995
    long long bucket_weak_mr     = 0;  // 0.995 ~ 0.999
    long long bucket_random_walk = 0;  // 0.999 ~ 1.0
    long long bucket_trending    = 0;  // > 1.0
    long long total_choppy_ticks = 0;

    double    signal_b_coeff_sum = 0.0;
    long long signal_count       = 0;

    // UNKNOWN/CHOPPY/TRENDING/TOXIC 순서
    long long regime_ticks[4]    = {0, 0, 0, 0};

    void add_choppy_tick(double b) {
        total_choppy_ticks++;
        regime_ticks[1]++;
        if      (b < 0.995)  bucket_strong_mr++;
        else if (b < 0.999)  bucket_weak_mr++;
        else if (b <= 1.0)   bucket_random_walk++;
        else                 bucket_trending++;
    }

    void add_signal(double b) {
        signal_b_coeff_sum += b;
        signal_count++;
    }

    void print() const {
        long long total = 0;
        for (int i = 0; i < 4; ++i) total += regime_ticks[i];

        printf("\n========== [CHOPPY Diagnostics] ==========\n");

        printf("\n[1] Regime Distribution (total %lld ticks)\n", total);
        const char* names[] = {"UNKNOWN", "CHOPPY", "TRENDING", "TOXIC"};
        for (int i = 0; i < 4; ++i) {
            double pct = total > 0 ? regime_ticks[i] * 100.0 / total : 0.0;
            printf("    %-10s : %10lld ticks (%5.1f%%)\n", names[i], regime_ticks[i], pct);
        }

        printf("\n[2] b_coeff Distribution within CHOPPY ticks (%lld total)\n", total_choppy_ticks);
        if (total_choppy_ticks > 0) {
            auto pct = [&](long long n) { return n * 100.0 / total_choppy_ticks; };
            printf("    b < 0.995   (강한 평균회귀) : %10lld  (%5.1f%%)\n", bucket_strong_mr,   pct(bucket_strong_mr));
            printf("    0.995~0.999 (약한 평균회귀) : %10lld  (%5.1f%%)\n", bucket_weak_mr,     pct(bucket_weak_mr));
            printf("    0.999~1.0   (랜덤워크)      : %10lld  (%5.1f%%)\n", bucket_random_walk, pct(bucket_random_walk));
            printf("    b > 1.0     (추세적)         : %10lld  (%5.1f%%)\n", bucket_trending,    pct(bucket_trending));
        }

        printf("\n[3] Signal Quality\n");
        if (signal_count > 0) {
            double avg_b = signal_b_coeff_sum / signal_count;
            printf("    신호 발생 횟수         : %lld\n", signal_count);
            printf("    신호 시점 평균 b_coeff : %.6f\n", avg_b);
            if (avg_b < 0.997)
                printf("    → 판정: 신호 시점 b_coeff 양호. 신호 로직(z+OBI+OFI) 자체를 의심할 것.\n");
            else
                printf("    → 판정: 신호 시점에 b_coeff가 이미 높음. 레짐 판독 오염이 주범.\n");
        } else {
            printf("    신호 없음\n");
        }

        printf("\n[4] 종합 진단\n");
        if (total_choppy_ticks > 0) {
            double bad_pct = (bucket_random_walk + bucket_trending) * 100.0 / total_choppy_ticks;
            if (bad_pct > 60.0) {
                printf("    ★ CHOPPY 구간의 %.1f%%가 랜덤워크 이상.\n", bad_pct);
                printf("    → RegimeDetector가 너무 관대하게 CHOPPY를 찍고 있음.\n");
                printf("    → 해결책: CHOPPY 분류에 b_coeff < threshold 조건 추가.\n");
            } else {
                printf("    ★ CHOPPY 구간의 b_coeff는 대체로 낮음 (평균회귀 조건 충족).\n");
                printf("    → 레짐 판독은 양호. z_score+OBI+OFI 신호 로직 자체를 개선해야 함.\n");
            }
        }
        printf("==========================================\n\n");
    }
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
    std::cout << ">>> [Backtester] Loading historical data from: " << filename << "...\n";

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

    double last_best_bid = 0.0, last_best_ask = 0.0;
    double last_best_bid_qty = 0.0, last_best_ask_qty = 0.0;
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
                    if (p_end) {
                        tick.best_bid = fast_atof(b_ptr, p_end - b_ptr);
                        char* q_ptr = p_end + 3;
                        char* q_end = strchr(q_ptr, '"');
                        if (q_end) tick.best_bid_qty = fast_atof(q_ptr, q_end - q_ptr);
                    }
                }
                char* a_ptr = strstr(e_ptr, "\"a\":[[\"");
                if (a_ptr) {
                    a_ptr += 7;
                    char* p_end = strchr(a_ptr, '"');
                    if (p_end) {
                        tick.best_ask = fast_atof(a_ptr, p_end - a_ptr);
                        char* q_ptr = p_end + 3;
                        char* q_end = strchr(q_ptr, '"');
                        if (q_end) tick.best_ask_qty = fast_atof(q_ptr, q_end - q_ptr);
                    }
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
                    if (p_end) tick.price = fast_atof(p_ptr, p_end - p_ptr);
                }
                char* q_ptr = strstr(e_ptr, "\"q\":\"");
                if (q_ptr) {
                    q_ptr += 5;
                    char* q_end = strchr(q_ptr, '"');
                    if (q_end) tick.volume = fast_atof(q_ptr, q_end - q_ptr);
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
    std::cout << ">>> [Backtester] Successfully loaded " << ticks.size() << " ticks\n";
    return ticks;
}

class BacktestSimulator {
private:
    int state_ = 0;
    int pending_signal_ = 0;
    double entry_price_ = 0.0, inv_entry_price_ = 0.0;
    double entry_time_ = 0.0, pending_start_time_ = 0.0, pending_price_ = 0.0;
    double position_qty_ = 0.0, total_pnl_ = 0.0, max_pnl_pct_ = 0.0;
    int win_count_ = 0, loss_count_ = 0, trade_count_ = 0, timeout_count_ = 0;

    const double maker_fee_ = 0.0002, taker_fee_ = 0.0004, slippage_rate_ = 0.0001;
    const double ENTRY_TIMEOUT = 0.3, PRICE_ESCAPE_THRESH = 0.0015;

public:
    void enter(int signal, const TickData& tick, double qty) {
        if (state_ != 0) return;
        state_ = 2; pending_signal_ = signal;
        pending_start_time_ = tick.timestamp; position_qty_ = qty; max_pnl_pct_ = 0.0;
        pending_price_ = (signal == 1) ? tick.best_ask : tick.best_bid;
    }

    void process_pending(const TickData& tick) {
        if (state_ != 2) return;
        bool is_long  = (pending_signal_ == 1);
        double target = is_long ? tick.best_ask : tick.best_bid;
        if (std::abs(target - pending_price_) / pending_price_ > PRICE_ESCAPE_THRESH) { state_ = 0; return; }
        if (tick.timestamp - pending_start_time_ > ENTRY_TIMEOUT) { timeout_count_++; state_ = 0; return; }
        bool filled = is_long ? (tick.best_ask <= pending_price_ * 1.0001)
                              : (tick.best_bid >= pending_price_ * 0.9999);
        if (filled) {
            double slippage  = tick.price * slippage_rate_;
            entry_price_     = is_long ? (tick.best_ask + slippage) : (tick.best_bid - slippage);
            inv_entry_price_ = 1.0 / entry_price_;
            entry_time_      = tick.timestamp;
            state_           = pending_signal_;
            total_pnl_      -= entry_price_ * position_qty_ * maker_fee_;
        }
    }

    void exit(const TickData& tick, bool is_market = false) {
        if (state_ == 2) { state_ = 0; return; }
        if (state_ == 0) return;
        double fee_rate         = is_market ? taker_fee_ : maker_fee_;
        double slippage_penalty = tick.price * slippage_rate_ * (is_market ? 2.0 : 1.0);
        double exit_price = (state_ == 1)
            ? (is_market ? tick.best_bid : tick.best_ask) - slippage_penalty
            : (is_market ? tick.best_ask : tick.best_bid) + slippage_penalty;
        double raw_pnl = (state_ == 1) ? (exit_price - entry_price_) * position_qty_
                                       : (entry_price_ - exit_price) * position_qty_;
        double net_pnl = raw_pnl - exit_price * position_qty_ * fee_rate;
        total_pnl_ += net_pnl;
        trade_count_++;
        if (net_pnl > 0) win_count_++; else loss_count_++;
        state_ = 0; entry_price_ = 0.0;
    }

    void update_position(const TickData& tick, double sl, double tp, double trail_start, double trail_drop) {
        if (state_ == 2) { process_pending(tick); return; }
        if (state_ == 0) return;
        double cv  = (state_ == 1) ? tick.best_bid : tick.best_ask;
        double pnl = (state_ == 1) ? (cv - entry_price_) * inv_entry_price_
                                   : (entry_price_ - cv)  * inv_entry_price_;
        if (pnl > max_pnl_pct_) max_pnl_pct_ = pnl;
        if (max_pnl_pct_ >= trail_start && pnl <= max_pnl_pct_ - trail_drop) { exit(tick, true);  return; }
        if (pnl <= -(sl * 0.75) && pnl > -sl)                                 { exit(tick, true);  return; }
        if (pnl <= -sl)                                                         exit(tick, true);
        else if (pnl >= tp)                                                     exit(tick, false);
    }

    int    get_state()       const { return state_; }
    bool   is_flat()         const { return state_ == 0; }
    double get_total_pnl()         { return total_pnl_; }
    int    get_trade_count()       { return trade_count_; }
    int    get_win_count()         { return win_count_; }
    int    get_timeout_count()     { return timeout_count_; }
};

// ============================================================
// run_diagnostics: --diag 모드에서만 호출.
// 히스토리를 두 번 순회합니다.
//   1패스: 레짐 분포 + CHOPPY 구간 b_coeff 분포 수집
//   2패스: 신호 발생 시점의 b_coeff 수집
// 두 번 순회하는 이유는 EngineRouter의 signal_cooldown 내부 상태가
// RegimeDetector와 독립적이라 한 번에 합칠 수 없기 때문입니다.
// ============================================================
void run_diagnostics(const std::vector<TickData>& history, const Config& cfg) {
    ChoppyDiagnostics diag;

    constexpr double EXCLUDE_START = 1772236800.0;
    constexpr double EXCLUDE_END   = 1772496000.0;

    // --- 1패스: 레짐 분포 + b_coeff 분포 ---
    {
        RegimeDetector rd(cfg.regime);
        for (const auto& tick : history) {
            double now = tick.timestamp;
            if (now >= EXCLUDE_START && now < EXCLUDE_END) continue;
            MarketRegime regime = rd.update(tick, now);
            double b = rd.get_last_b_coeff();
            int idx = static_cast<int>(regime);
            if (idx >= 0 && idx < 4) diag.regime_ticks[idx]++;
            if (regime == MarketRegime::CHOPPY) diag.add_choppy_tick(b);
        }
    }

    // --- 2패스: 신호 발생 시점 b_coeff ---
    {
        RegimeDetector rd(cfg.regime);
        EngineRouter   er(cfg);
        MarketRegime   last_regime = MarketRegime::UNKNOWN;
        double circuit_breaker_until = 0.0;

        for (const auto& tick : history) {
            double now = tick.timestamp;
            if (now >= EXCLUDE_START && now < EXCLUDE_END) continue;

            MarketRegime regime = rd.update(tick, now);
            double b = rd.get_last_b_coeff();
            er.switch_regime(regime);

            if (regime != last_regime) {
                if (regime == MarketRegime::TOXIC)
                    circuit_breaker_until = now + cfg.risk.circuit_breaker_cooldown;
                last_regime = regime;
            }

            bool armed = (regime == MarketRegime::CHOPPY);
            if (now < circuit_breaker_until) { er.update(tick, cfg, now); continue; }

            int signal = er.update(tick, cfg, now);
            if (signal != 0 && armed) diag.add_signal(b);
        }
    }

    diag.print();
}

// ============================================================
// run_simulation: 그리드 서치용 (기존 구조 유지)
// ============================================================
std::vector<SimResult> run_simulation(
    const std::vector<TickData>& history, 
    const Config& task_config, 
    const char* param_desc,
    const std::vector<std::pair<double, double>>& sl_tp_pairs, 
    MarketRegime target_regime) 
{
    RegimeDetector regime_detector(task_config.regime);
    EngineRouter   engine_router(task_config);
    size_t num_sims = sl_tp_pairs.size();
    std::vector<BacktestSimulator> sims(num_sims);

    double circuit_breaker_until = 0.0;
    bool   engine_armed = false;
    MarketRegime last_regime = MarketRegime::UNKNOWN;

    constexpr double EXCLUDE_START = 1772236800.0;
    constexpr double EXCLUDE_END   = 1772496000.0;

    for (const auto& tick : history) {
        double now = tick.timestamp;
        if (now >= EXCLUDE_START && now < EXCLUDE_END) continue;
        
        MarketRegime current_regime = regime_detector.update(tick, now);
        engine_router.switch_regime(current_regime);

        if (current_regime != last_regime) {
            if (current_regime == MarketRegime::TOXIC && target_regime != MarketRegime::TOXIC) {
                for (auto& sim : sims) if (sim.get_state() != 0) sim.exit(tick, true);
                circuit_breaker_until = now + task_config.risk.circuit_breaker_cooldown;
                engine_armed = false;
            }
            last_regime = current_regime;
        }

        engine_armed = (current_regime == MarketRegime::TOXIC)
            ? (target_regime == MarketRegime::TOXIC)
            : (current_regime == target_regime);

        for (size_t s = 0; s < num_sims; ++s)
            sims[s].update_position(tick, sl_tp_pairs[s].first, sl_tp_pairs[s].second,
                                    task_config.risk.trail_start, task_config.risk.trail_drop);

        if (now < circuit_breaker_until) { engine_router.update(tick, task_config, now); continue; }

        int signal = engine_router.update(tick, task_config, now);
        if (signal != 0 && engine_armed)
            for (size_t s = 0; s < num_sims; ++s)
                if (sims[s].is_flat()) sims[s].enter(signal, tick, task_config.risk.qty);
    }
    
    std::vector<SimResult> res_list;
    res_list.reserve(num_sims);
    for (size_t s = 0; s < num_sims; ++s) {
        SimResult res;
        std::strncpy(res.param_desc, param_desc, sizeof(res.param_desc) - 1);
        res.param_desc[sizeof(res.param_desc) - 1] = '\0'; 
        res.sl = sl_tp_pairs[s].first; res.tp = sl_tp_pairs[s].second;
        res.trades   = sims[s].get_trade_count();  
        res.wins     = sims[s].get_win_count();
        res.win_rate = res.trades > 0 ? (double)res.wins / res.trades * 100.0 : 0.0;
        res.pnl      = sims[s].get_total_pnl();
        res.win      = sims[s].get_timeout_count();
        res_list.push_back(res);
    }
    return res_list;
}

int main(int argc, char* argv[]) {
    std::string filename = (argc > 1) ? argv[1] : "binance_data.log";
    std::cout << ">>> Booting HFT Backtester...\n";

    std::vector<TickData> history = load_history(filename);
    if (history.empty()) { std::cerr << ">>> [Fatal] No tick data loaded.\n"; return 1; }
    
    Config base_config;
    if (!base_config.load("config.json")) { std::cerr << ">>> [Error] config.json not found!\n"; return 1; }

    // ============================================================
    // [진단 모드] --diag 플래그로 실행 시 그리드 서치 없이 진단만 수행
    // 사용법: ./tester_wide eth_data_merge.log --diag
    //
    // config.json의 CHOPPY 파라미터(window, ou_thresh 등)를
    // 대표값으로 설정해두고 실행하면 됩니다.
    // ============================================================
    if (argc > 2 && std::string(argv[2]) == "--diag") {
        std::cout << "\n>>> [DIAG MODE] Running CHOPPY diagnostics...\n";
        run_diagnostics(history, base_config);
        return 0;
    }

    // 이하 그리드 서치
    std::cout << "\n>>> [SYSTEM] Igniting Batching Grid Search Engine...\n";
    unsigned int num_cores = std::thread::hardware_concurrency();
    if (num_cores == 0) num_cores = 4;
    std::cout << ">>> Engaging " << num_cores << " CPU Cores!\n";

    std::vector<std::pair<double, double>> sl_tp_pairs;
    for (double sl = 0.002; sl <= 0.006; sl += 0.001)
        for (double tp = 0.002; tp <= 0.006; tp += 0.001)
            sl_tp_pairs.push_back({sl, tp});

    std::vector<SearchTask> choppy_tasks;
    std::cout << "\n>>> [SYSTEM] Generating CHOPPY Wide Search Tasks...\n";
    for (double z = 0.5; z <= 2.0; z += 0.5) {
        for (double win = 300; win <= 800; win += 100) {
            for (double obi = 0.3; obi <= 0.7; obi += 0.1) {
                for (double ou = 0.996; ou <= 0.9990; ou += 0.001) {
                    for (double ofi_win = 700; ofi_win <= 700; ofi_win += 100) {
                        SearchTask task;
                        task.config = base_config;
                        task.config.strategy.choppy.z_score    = z;
                        task.config.strategy.choppy.window     = win;
                        task.config.strategy.choppy.obi        = obi;
                        task.config.strategy.choppy.ou_thresh  = ou;
                        task.config.strategy.choppy.ofi_window = ofi_win;
                        snprintf(task.param_desc, sizeof(task.param_desc),
                                 "C|Z:%.1f|W:%.0f|O:%.2f|U:%.4f|F:%.0f", z, win, obi, ou, ofi_win);
                        task.param_desc[63] = '\0';
                        choppy_tasks.push_back(task);
                    }
                }
            }
        }
    }

    std::vector<SearchTask> trending_tasks;
    std::cout << ">>> [SYSTEM] Generating TRENDING Wide Search Tasks...\n";
    for (double k_a = 0.0010; k_a <= 0.0040; k_a += 0.0003) {
        for (double accel = 0.0003; accel <= 0.0030; accel += 0.0003) {
            for (double obi = 0.3; obi <= 1.0; obi += 0.1) {
                SearchTask task;
                task.config = base_config;
                task.config.strategy.trending.k_a   = k_a;
                task.config.strategy.trending.accel = accel;
                task.config.strategy.trending.obi   = obi;
                snprintf(task.param_desc, sizeof(task.param_desc),
                         "T|kA:%.4f|Ac:%.4f|O:%.2f", k_a, accel, obi);
                task.param_desc[63] = '\0';
                trending_tasks.push_back(task);
            }
        }
    }

    std::vector<SearchTask> toxic_tasks;
    std::cout << ">>> [SYSTEM] Generating TOXIC Wide Search Tasks...\n";
    for (double alpha = 0.1; alpha <= 0.6; alpha += 0.1) {
        for (double beta = 0.5; beta <= 0.8; beta += 0.1) {
            for (double energy = 2.5; energy <= 3.5; energy += 0.1) {
                SearchTask task;
                task.config = base_config;
                task.config.strategy.toxic.alpha         = alpha;
                task.config.strategy.toxic.beta          = beta;
                task.config.strategy.toxic.energy_thresh = energy;
                snprintf(task.param_desc, sizeof(task.param_desc),
                         "X|Al:%.1f|Be:%.1f|En:%.1f", alpha, beta, energy);
                task.param_desc[63] = '\0';
                toxic_tasks.push_back(task);
            }
        }
    }

    std::cout << ">>> [SYSTEM] Tasks: CHOPPY=" << choppy_tasks.size()
              << " TRENDING=" << trending_tasks.size()
              << " TOXIC=" << toxic_tasks.size() << "\n";

    auto hft_sorter = [](const SimResult& a, const SimResult& b) {
        bool av = (a.trades >= 300 && a.pnl > 0), bv = (b.trades >= 300 && b.pnl > 0);
        if (av != bv) return av > bv;
        if (av) { if (std::abs(a.win_rate - b.win_rate) > 0.5) return a.win_rate > b.win_rate; return a.pnl > b.pnl; }
        return a.pnl > b.pnl;
    };

    struct RegimeRun { std::vector<SearchTask>* tasks; MarketRegime regime; const char* name; };
    std::vector<RegimeRun> runs = {
        { &choppy_tasks,   MarketRegime::CHOPPY,   "CHOPPY"   },
        { &trending_tasks, MarketRegime::TRENDING,  "TRENDING" },
        { &toxic_tasks,    MarketRegime::TOXIC,     "TOXIC"    },
    };

    std::vector<SimResult> all_results;
    for (auto& run : runs) {
        std::vector<SimResult> regime_results;
        std::mutex res_mutex;
        std::atomic<size_t> task_idx(0);
        size_t total = run.tasks->size();
        std::cout << "\n>>> [RUN] " << run.name << " (" << total << " tasks)...\n";

        auto worker = [&]() {
            std::vector<SimResult> local;
            local.reserve(1000);
            while (true) {
                size_t start = task_idx.fetch_add(10);
                if (start >= total) break;
                size_t end = std::min(start + 10, total);
                for (size_t i = start; i < end; ++i) {
                    auto res_list = run_simulation(history, (*run.tasks)[i].config,
                                                   (*run.tasks)[i].param_desc, sl_tp_pairs, run.regime);
                    for (auto& r : res_list) if (r.trades >= 10) local.push_back(r);
                }
                if (start % 50 < 10) { printf("\r>>> [%s] %zu / %zu", run.name, std::min(start, total), total); fflush(stdout); }
            }
            std::lock_guard<std::mutex> lock(res_mutex);
            regime_results.insert(regime_results.end(), local.begin(), local.end());
        };

        std::vector<std::thread> threads;
        for (unsigned int i = 0; i < num_cores; ++i) threads.emplace_back(worker);
        for (auto& t : threads) t.join();

        printf("\r>>> [%s] Done! Results: %zu\n", run.name, regime_results.size());
        std::sort(regime_results.begin(), regime_results.end(), hft_sorter);

        std::cout << "\n--- [" << run.name << " Top 10] ---\n";
        std::cout << "        Parameters                        |  SL   |  TP   | Trades | Wins | WinRate | Timeout | EV(USDT) |   Net PnL\n";
        std::cout << "------------------------------------------------------------------------------------------------------------------------\n";
        int show = std::min((int)regime_results.size(), 10);
        for (int i = 0; i < show; ++i) {
            const auto& r = regime_results[i];
            double ev = r.trades > 0 ? r.pnl / r.trades : 0.0;
            printf(" %-40s | %.3f | %.3f | %6d | %4d | %6.2f%% | %7.0f | %8.4f | %9.4f USDT\n",
                   r.param_desc, r.sl, r.tp, r.trades, r.wins, r.win_rate, r.win, ev, r.pnl);
        }
        all_results.insert(all_results.end(), regime_results.begin(), regime_results.end());
    }

    printf("\n>>> Total Results: %zu\n", all_results.size());
    std::sort(all_results.begin(), all_results.end(), hft_sorter);

    std::cout << "\n=================================== [Grid Search Results (HFT Optimized)] =========================================\n";
    std::cout << "        Strategy Parameters                |  SL   |  TP   | Trades | Wins | WinRate | Timeout | EV(USDT) |   Net PnL   \n";
    std::cout << "---------------------------------------------------------------------------------------------------------------------------\n";
    int display_count = std::min((int)all_results.size(), 30);
    for (int i = 0; i < display_count; ++i) {
        const auto& r = all_results[i];
        double ev = r.trades > 0 ? r.pnl / r.trades : 0.0;
        printf(" %-40s | %.3f | %.3f | %6d | %4d | %6.2f%% | %7.0f | %8.4f | %9.4f USDT \n",
               r.param_desc, r.sl, r.tp, r.trades, r.wins, r.win_rate, r.win, ev, r.pnl);
    }
    std::cout << "===========================================================================================================================\n";
    return 0;
}
