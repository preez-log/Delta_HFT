#pragma once
#include <string>
#include <iostream>
#include <cstdlib>
#include <sstream>
#include "../simdjson.h"

struct ExchangeConfig {
    std::string symbol;
    int price_precision;
    int qty_precision;
};

struct RiskConfig {
    double qty;
    double trail_start;
    double trail_drop;
    double circuit_breaker_vol_pct;
    double circuit_breaker_cooldown;
    double global_kill_pct;
    double global_kill_cooldown;
    double global_kill_window;
    double order_cooldown;
};

struct RegimeConfig {
    uint64_t ou_window;
    double reset_interval_sec;
    double toxic_vol_pct;
    double toxic_net_delta;
    double trending_vol_pct;
    double trending_delta_ratio;
    double trending_ou_thresh;
};

struct EngineParam {
    double window;
    double z_score;
    double obi;
    double ou_thresh;
    double stop_loss;
    double take_profit;
    double k_a;
    double k_p;
    double k_v;
    double accel;
    double alpha;
    double beta;
    double energy_thresh;
    double signal_cooldown;
    double ofi_window = 100.0;  // [OFI 추가] 기본값 100틱
};

struct StrategyConfig {
    double signal_cooldown;
    EngineParam choppy;
    EngineParam trending;
    EngineParam toxic;
};

struct Config {
    std::string api_key;
    std::string secret_key;
    
    ExchangeConfig exchange;
    RiskConfig risk;
    RegimeConfig regime;
    StrategyConfig strategy;

    bool load(const std::string& filename) {
        simdjson::ondemand::parser parser;
        simdjson::padded_string json;
        
        try {
            json = simdjson::padded_string::load(filename);
            simdjson::ondemand::document doc = parser.iterate(json);

            // Exchange
            auto ex = doc["exchange"];
            std::string_view sym;
            ex["symbol"].get(sym);
            exchange.symbol =               std::string(sym);
            exchange.price_precision =      static_cast<int>(int64_t(ex["price_precision"]));
            exchange.qty_precision =        static_cast<int>(int64_t(ex["qty_precision"]));

            // Risk
            auto r =                        doc["risk"];
            risk.qty =                      double(r["qty"]);
            risk.trail_start =              double(r["trail_start"]);
            risk.trail_drop =               double(r["trail_drop"]);
            risk.circuit_breaker_vol_pct =  double(r["circuit_breaker_vol_pct"]);
            risk.circuit_breaker_cooldown = double(r["circuit_breaker_cooldown"]);
            risk.global_kill_pct =          double(r["global_kill_pct"]);
            risk.global_kill_cooldown =     double(r["global_kill_cooldown"]);
            risk.global_kill_window =       double(r["global_kill_window"]);
            risk.order_cooldown =           double(r["order_cooldown"]);

            // Regime
            auto reg =                      doc["regime"];
            regime.ou_window =              uint64_t(reg["ou_window"]);
            regime.reset_interval_sec =     double(reg["reset_interval_sec"]);
            regime.toxic_vol_pct =          double(reg["toxic_vol_pct"]);
            regime.toxic_net_delta =        double(reg["toxic_net_delta"]);
            regime.trending_vol_pct =       double(reg["trending_vol_pct"]);
            regime.trending_delta_ratio =   double(reg["trending_delta_ratio"]);
            regime.trending_ou_thresh =     double(reg["trending_ou_thresh"]);

            // Strategy
            auto s =                        doc["strategy"];
            strategy.signal_cooldown =      double(s["signal_cooldown"]);
            
            auto chop =                     s["choppy"];
            strategy.choppy.window =        double(chop["window"]);
            strategy.choppy.z_score =       double(chop["z_score"]);
            strategy.choppy.obi =           double(chop["obi"]);
            strategy.choppy.ou_thresh =     double(chop["ou_thresh"]);
            strategy.choppy.stop_loss =     double(chop["stop_loss"]);
            strategy.choppy.take_profit =   double(chop["take_profit"]);
            strategy.choppy.ofi_window =    double(chop["ofi_window"]);

            auto trnd =                     s["trending"];
            strategy.trending.k_a =         double(trnd["k_a"]);
            strategy.trending.k_p =         double(trnd["k_p"]);
            strategy.trending.k_v =         double(trnd["k_v"]);
            strategy.trending.accel =       double(trnd["accel"]);
            strategy.trending.obi =         double(trnd["obi"]);
            strategy.trending.ou_thresh =   double(trnd["ou_thresh"]);
            strategy.trending.stop_loss =   double(trnd["stop_loss"]);
            strategy.trending.take_profit = double(trnd["take_profit"]);

            auto toxic =                     s["toxic"];
            strategy.toxic.alpha =           double(toxic["alpha"]);
            strategy.toxic.beta =            double(toxic["beta"]);
            strategy.toxic.energy_thresh =   double(toxic["energy_thresh"]);
            strategy.toxic.signal_cooldown = double(toxic["signal_cooldown"]);
            strategy.toxic.stop_loss =       double(toxic["stop_loss"]);
            strategy.toxic.take_profit =     double(toxic["take_profit"]);

            // API Keys
            const char* env_ak = std::getenv("BINANCE_ACCESS_KEY");
            const char* env_sk = std::getenv("BINANCE_SECRET_KEY");

            if (env_ak && env_sk) {
                api_key = std::string(env_ak);
                secret_key = std::string(env_sk);
            } else {
                std::string_view ak, sk;
                if (!doc["api_key"].get(ak) && !doc["secret_key"].get(sk)) {
                    api_key = std::string(ak);
                    secret_key = std::string(sk);
                } else {
                    std::cerr << ">>> [error] API Keys not found!" << std::endl;
                    return false;
                }
            }
            return true;
        } catch (const std::exception& e) {
            std::cerr << ">>> [config] Error parsing JSON: " << e.what() << std::endl;
            return false;
        }
    }

    void print_summary() const {
        char buf[1024];
        snprintf(buf, sizeof(buf), 
            ">>> [Config] %s | Qty: %.3f | Choppy(Z:%.1f OBI:%.1f) | Trend(Accel:%.4f k_a:%.4f OBI:%.1f) | Toxic(alpha:%.1f beta:%.1f energy:%.1f cooldown:%.1f) | Toxic Vol: %.2f%%",
            exchange.symbol.c_str(), risk.qty,
            strategy.choppy.z_score, strategy.choppy.obi,
            strategy.trending.accel, strategy.trending.k_a, strategy.trending.obi,
            strategy.toxic.alpha, strategy.toxic.beta, strategy.toxic.energy_thresh, strategy.toxic.signal_cooldown,
            regime.toxic_vol_pct);
        std::cout << buf << "\n";
    }
};