#pragma once
#include "../network/BinanceTrade.h"
#include "../core/Logger.h"
#include "../core/PrecisionClock.h"
#include "Types.h"
#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstring>
#include <charconv>

inline bool is_ent_prefix(const char* cid) {
    // "ent_" -> 0x5f746e65
    return *reinterpret_cast<const uint32_t*>(cid) == 0x5f746e65; 
}

inline bool is_ext_prefix(const char* cid) {
    // "ext_" -> 0x5f747865
    return *reinterpret_cast<const uint32_t*>(cid) == 0x5f747865;
}

inline bool is_status_filled(const char* status) {
    // "FILL" -> 0x4C4C4946
    return *reinterpret_cast<const uint32_t*>(status) == 0x4C4C4946;
}

inline bool is_status_partial(const char* status) {
    // "PART" -> 0x54524150
    return *reinterpret_cast<const uint32_t*>(status) == 0x54524150;
}

inline bool is_status_canceled(const char* status) {
    // "CANC" -> 0x434E4143
    return *reinterpret_cast<const uint32_t*>(status) == 0x434E4143;
}

inline bool is_status_rejected(const char* status) {
    // "REJE" -> 0x454A4552
    return *reinterpret_cast<const uint32_t*>(status) == 0x454A4552;
}

inline bool is_status_expired(const char* status) {
    // "EXPI" -> 0x49505845
    return *reinterpret_cast<const uint32_t*>(status) == 0x49505845;
}

enum class PosState { 
    NONE = 0, 
    LONG, 
    SHORT, 
    PENDING_ENTRY, 
    PENDING_EXIT, 
    PENDING_EMERGENCY_CANCEL,
    PENDING_EMERGENCY_EXIT,
    PENDING_CANCEL
};

struct RiskParams {
    double qty;          
    double stop_loss;    
    double trail_start;  
    double trail_drop;   
};

class OrderManager {
private:
    BinanceTrade* trade_ws_; 
    std::string symbol_;
    RiskParams risk_;

    PosState state_ = PosState::NONE;
    double entry_price_ = 0.0;
    double inv_entry_price_ = 0.0;
    double pending_price_ = 0.0;
    bool is_long_ = false;
    double peak_price_ = 0.0; 
    double position_qty_ = 0.0;
    
    double last_req_time_ = 0;
    double entry_time_ = 0;
    double pending_start_time_ = 0; 
    double last_sync_req_time_ = 0;
    
    // API Rate Limit 방어용 변수
    double rate_limit_sec_ = 0.0;
    int current_sec_api_calls_ = 0;
    const int MAX_CALLS_PER_SEC = 20;
    
    // [Zero-Allocation] 스택 버퍼 기반 Client ID
    long long exit_mod_count_ = 0;
    long long entry_mod_count_ = 0;
    char current_exit_cid_[32] = {0};
    char current_entry_cid_[32] = {0};
    
    char exit_prefix_[32] = {0};
    char entry_prefix_[32] = {0};
    size_t exit_prefix_len_ = 0;
    size_t entry_prefix_len_ = 0;

    inline void set_next_exit_cid(char* target) {
        std::memcpy(target, exit_prefix_, exit_prefix_len_);
        auto res = std::to_chars(target + exit_prefix_len_, target + 31, ++exit_mod_count_);
        *res.ptr = '\0';
    }
    
    inline void set_next_entry_cid(char* target) {
        std::memcpy(target, entry_prefix_, entry_prefix_len_);
        auto res = std::to_chars(target + entry_prefix_len_, target + 31, ++entry_mod_count_);
        *res.ptr = '\0';
    }

public:
    OrderManager(BinanceTrade* ws, const std::string& sym, RiskParams r)
        : trade_ws_(ws), symbol_(sym), risk_(r) {
            snprintf(exit_prefix_, sizeof(exit_prefix_), "ext_%s_", symbol_.c_str());
            exit_prefix_len_ = std::strlen(exit_prefix_);
        
            snprintf(entry_prefix_, sizeof(entry_prefix_), "ent_%s_", symbol_.c_str());
            entry_prefix_len_ = std::strlen(entry_prefix_);
        }
        
    //  장세가 바뀔 때마다 익절/손절 파라미터 실시간 교체
    void update_risk_params(double sl_pct, double take_profit_pct) {
        risk_.stop_loss = sl_pct;
        risk_.trail_start = take_profit_pct; 
    }

    // 1초 단위로 호출 횟수를 검사
    inline bool check_and_add_rate_limit(double now) {
        if (now - rate_limit_sec_ >= 1.0) {
            current_sec_api_calls_ = 0;
            rate_limit_sec_ = now;
        }
        if (current_sec_api_calls_ >= MAX_CALLS_PER_SEC) return false; 
        current_sec_api_calls_++;
        return true;
    }

    // GHOST FIX: 웹소켓 응답 지연/누락 방어 로직
    void update_tick(const TickData& tick, double now) {
        if (tick.best_bid <= 0.0001 || tick.best_ask <= 0.0001) return;
        
        bool trigger_sync = false;
        if (state_ == PosState::PENDING_ENTRY || state_ == PosState::PENDING_CANCEL || 
            state_ == PosState::PENDING_EMERGENCY_CANCEL || state_ == PosState::PENDING_EMERGENCY_EXIT) {
            // 비상 상태
            if (now - last_req_time_ > 1.5) trigger_sync = true;
        } else if (state_ == PosState::PENDING_EXIT) {
            // 익절 대기(PENDING_EXIT)
            if (now - last_req_time_ > 10.0) trigger_sync = true; 
        }

        // 스팸 방지 쿨다운 3초
        if (trigger_sync && (now - last_sync_req_time_ > 3.0)) {
            if (state_ != PosState::PENDING_EXIT) { 
                LOG(">>> [GHOST FIX] Websocket timeout! Forcing REST API Sync.");
            }
            trade_ws_->sync_position_task(); 
            last_sync_req_time_ = now;
        }
        
	      // 소프트 스탑(지정가 손절) 호가 추적
        if (state_ == PosState::PENDING_EMERGENCY_EXIT) {
            // 보수적 PNL 계산 (즉시 던질 수 있는 호가 기준)
            double current_val = is_long_ ? tick.best_bid : tick.best_ask;
            double pnl = is_long_ ? (current_val - entry_price_) * inv_entry_price_ 
                                  : (entry_price_ - current_val) * inv_entry_price_;

            // [하드 스탑] 추격 중이라도 하드 스탑 한계선(-100%)을 돌파하면시장가 투척
            if (pnl <= -risk_.stop_loss) {
                LOG(">>> [EMERGENCY] Soft Stop Failed! Hard Stop Hit (PNL: %.4f%%). MARKET EXIT!", pnl * 100.0);
                if (current_exit_cid_[0] != '\0') trade_ws_->cancel_order_ws(symbol_.c_str(), current_exit_cid_);
                double exit_qty = (position_qty_ > 0.001) ? position_qty_ : risk_.qty;
                trade_ws_->send_market_order_ws(symbol_.c_str(), is_long_ ? "SELL" : "BUY", exit_qty, true); 
                state_ = PosState::PENDING_EMERGENCY_CANCEL; 
                last_req_time_ = now;
                return;
            }

            // 안전장치: 추격 시작한 지 2초(2.0s)가 넘었는데 안 팔리면 시장가로
            if (now - entry_time_ > 2.0) { 
                LOG(">>> [EMERGENCY] Soft Stop Timeout (2.0s)! Falling back to MARKET EXIT!");
                if (current_exit_cid_[0] != '\0') trade_ws_->cancel_order_ws(symbol_.c_str(), current_exit_cid_);
                double exit_qty = (position_qty_ > 0.001) ? position_qty_ : risk_.qty;
                trade_ws_->send_market_order(symbol_.c_str(), is_long_ ? "SELL" : "BUY", exit_qty, true);
                state_ = PosState::PENDING_EMERGENCY_CANCEL; 
                last_req_time_ = now;
                return;
            }

            // 0.1초(100ms)마다 내 주문을 최우선 호가로 밀어 올림/내림
            if (now - last_req_time_ > 0.1) {
                double target_price = is_long_ ? tick.best_ask : tick.best_bid;
                double exit_qty = (position_qty_ > 0.001) ? position_qty_ : risk_.qty;
                char next_cid[32];
                set_next_exit_cid(next_cid);

                if (current_exit_cid_[0] != '\0') {
                    trade_ws_->modify_limit_order_ws(symbol_.c_str(), is_long_ ? "SELL" : "BUY",
                                                    exit_qty, target_price, current_exit_cid_, next_cid);
                } else {
                    trade_ws_->send_limit_order_ws_cid(symbol_.c_str(), is_long_ ? "SELL" : "BUY",
                                                      exit_qty, target_price, true, next_cid);
                }
                strcpy(current_exit_cid_, next_cid);
                last_req_time_ = now;
            }
            return;
        }

        // 진입 대기 중 (1호가를 쫓아감)
        if (state_ == PosState::PENDING_ENTRY) {
            if (now - pending_start_time_ > 0.3) { 
                LOG(">>> [System] Entry Order Timeout (300ms). Canceling...");
                if (current_entry_cid_[0] != '\0') {
                    if (!trade_ws_->cancel_order_ws(symbol_.c_str(), current_entry_cid_)) {
                        trade_ws_->cancel_all_orders(symbol_.c_str());
                    }
                }
                state_ = PosState::PENDING_CANCEL; 
                last_req_time_ = now;
                return;
            }

            double target_price = is_long_ ? tick.best_bid : tick.best_ask;
            double price_diff_abs = std::abs(target_price - pending_price_);
            
            // [FIX 3] 이탈 임계값 0.05% → 0.15% (단기 노이즈 버티기)
            if (price_diff_abs > pending_price_ * 0.0015) { 
                LOG(">>> [System] Price ran away. Abandoning Entry.");
                if (current_entry_cid_[0] != '\0') trade_ws_->cancel_order_ws(symbol_.c_str(), current_entry_cid_);
                state_ = PosState::NONE;
                return;
            }
            
            // [FIX 6] 진입 추격은 GTC (modify_entry_order_ws) 사용
            if (pending_price_ != target_price && (now - last_req_time_ > 0.2)) {
                if (!check_and_add_rate_limit(now)) return; 
                char next_cid[32];
                set_next_entry_cid(next_cid);
                trade_ws_->modify_entry_order_ws(symbol_.c_str(), is_long_ ? "BUY" : "SELL", 
                                                risk_.qty, target_price, current_entry_cid_, next_cid);
                strcpy(current_entry_cid_, next_cid);
                pending_price_ = target_price;
                last_req_time_ = now;
            }
            return;
        }
        
        // REAL POS 응답 대기
        if (state_ == PosState::PENDING_CANCEL || state_ == PosState::PENDING_EMERGENCY_CANCEL) {
            if (now - last_req_time_ > 5.0) {
                LOG(">>> [FATAL] Exchange not responding for 5s. State unlocked to prevent deadlock.");
                state_ = PosState::NONE;
                last_req_time_ = now;
            }
            return;
        }
        
        // 포지션 보유 상태 (메인 감시 로직)
        if (state_ == PosState::LONG || state_ == PosState::SHORT || state_ == PosState::PENDING_EXIT || state_ == PosState::PENDING_EMERGENCY_CANCEL) {
            
            if (is_long_) peak_price_ = std::max(peak_price_, tick.price);
            else          peak_price_ = std::min(peak_price_, tick.price);

            // 보수적 PNL 계산 (즉시 시장가로 팔 수 있는 Best Bid/Ask 기준)
            double current_val = is_long_ ? tick.best_bid : tick.best_ask;
            double pnl = is_long_ ? (current_val - entry_price_) * inv_entry_price_ 
                                  : (entry_price_ - current_val) * inv_entry_price_;
            
            bool trail_hit = false;
            // 트레일링 스탑 조건
            if (is_long_ && peak_price_ >= entry_price_ * (1.0 + risk_.trail_start) && tick.price <= peak_price_ * (1.0 - risk_.trail_drop)) trail_hit = true;
            if (!is_long_ && peak_price_ <= entry_price_ * (1.0 - risk_.trail_start) && tick.price >= peak_price_ * (1.0 + risk_.trail_drop)) trail_hit = true;
            
            double exit_qty = (position_qty_ > 0.001) ? position_qty_ : risk_.qty;
            
            // --- 하드 스탑 (Hard Stop: -100%) ---
            if (pnl <= -risk_.stop_loss) {
                if (state_ != PosState::PENDING_EMERGENCY_CANCEL) { 
                    LOG(">>> [EMERGENCY] Hard Stop Hit! PNL: %.4f%%. Firing MARKET EXIT!", pnl * 100.0);
                    if (current_exit_cid_[0] != '\0') trade_ws_->cancel_order_ws(symbol_.c_str(), current_exit_cid_);
                    trade_ws_->send_market_order(symbol_.c_str(), is_long_ ? "SELL" : "BUY", exit_qty, true);
                    state_ = PosState::PENDING_EMERGENCY_CANCEL; 
                    last_req_time_ = now;
                }
                return;
            }

            // --- 소프트 스탑 (Soft Stop: -90%) ---
            // [FIX 4] 소프트스탑 90% → 75% (정상 변동성 구간 오발동 방지)
            double soft_stop_limit = risk_.stop_loss * 0.75;
            if (pnl <= -soft_stop_limit) {
                if (state_ == PosState::LONG || state_ == PosState::SHORT || state_ == PosState::PENDING_EXIT) {
                    LOG(">>> [Warning] Soft Stop Triggered! PNL: %.4f%%. Starting Maker Chase.", pnl * 100.0);
                    if (current_exit_cid_[0] != '\0') trade_ws_->cancel_order_ws(symbol_.c_str(), current_exit_cid_);
                    state_ = PosState::PENDING_EMERGENCY_EXIT; 
                    current_exit_cid_[0] = '\0';
                    last_req_time_ = now - 0.15;
                    entry_time_ = now; 
                    return;
                }
            }

            // --- 지정가(Maker) 추적 로직 (일반 익절 & 트레일링) ---
            
            // 취소 대기 중일 때는 일반 지정가 로직을 무시
            if (state_ == PosState::PENDING_EMERGENCY_CANCEL) return;

            double target_price;
            if (trail_hit) { 
                target_price = is_long_ ? tick.best_ask : tick.best_bid;
            } else { 
                double min_profit_margin = entry_price_ * risk_.trail_start; 
                target_price = is_long_ ? std::max(tick.best_ask, entry_price_ + min_profit_margin) 
                                        : std::min(tick.best_bid, entry_price_ - min_profit_margin);
            }

            // 최초 지정가 주문 제출
            if (state_ == PosState::LONG || state_ == PosState::SHORT) {
                if (now - last_req_time_ < 0.5) return;
                state_ = PosState::PENDING_EXIT;
                last_req_time_ = now;
                pending_price_ = target_price;
                set_next_exit_cid(current_exit_cid_);
                trade_ws_->send_limit_order_ws_cid(symbol_.c_str(), is_long_ ? "SELL" : "BUY", 
                                                  exit_qty, target_price, true, current_exit_cid_);
                return;
            }

            // 호가 추적
            if (state_ == PosState::PENDING_EXIT) {
                if (pending_price_ != target_price && (now - last_req_time_ > 0.2)) {
                    if (!check_and_add_rate_limit(now)) return; 
                    char next_cid[32];
                    set_next_exit_cid(next_cid);
                    trade_ws_->modify_limit_order_ws(symbol_.c_str(), is_long_ ? "SELL" : "BUY", 
                                                    exit_qty, target_price, current_exit_cid_, next_cid);
                    strcpy(current_exit_cid_, next_cid);
                    pending_price_ = target_price;
                    last_req_time_ = now;
                    LOG(">>> [Maker] Chasing Exit Price -> %.2f", target_price);
                }
            }
        }
    }

    bool try_enter(int signal, const TickData& tick, double now) {
        if (state_ != PosState::NONE) return false; 
        if (now - last_req_time_ < 0.5) return false;

        last_req_time_ = now;
        pending_start_time_ = now;
        state_ = PosState::PENDING_ENTRY;
        is_long_ = (signal == 1);

        // [FIX 1] 진입가를 반대 호가로 설정 (즉시 체결 가능성 극대화)
        // 롱: best_ask (매도 1호가) = 즉시 살 수 있는 가격
        // 숏: best_bid (매수 1호가) = 즉시 팔 수 있는 가격
        double maker_price = is_long_ ? tick.best_ask : tick.best_bid; 
        pending_price_ = maker_price;
        
        set_next_entry_cid(current_entry_cid_);
        const char* dir = is_long_ ? "BUY" : "SELL"; 

        bool sent = trade_ws_->send_limit_order_ws_cid(symbol_.c_str(), dir, risk_.qty, maker_price, false, current_entry_cid_);
        
        if (sent) return true;
        
        state_ = PosState::NONE;
        return false;
    }

    void on_order_update(const OrderUpdate& up, double now) {
        if (strcmp(up.symbol, symbol_.c_str()) != 0) return;
        
        if (strcmp(up.status, "REAL_POS") == 0) {
            double current_pos_qty = std::abs(up.real_position_amt);
            
            if (current_pos_qty > 0.001) { 
                // 익절 대기 중(PENDING_EXIT)이거나, 손절중(PENDING_EMERGENCY_EXIT)일 때
                if ((state_ == PosState::PENDING_EXIT || state_ == PosState::PENDING_EMERGENCY_EXIT) 
                    && std::abs(current_pos_qty - position_qty_) < 0.001) {
                    last_req_time_ = now; 
                    return; 
                }

                // 그 외의 어중간한 상태 초기화
                if (state_ != PosState::LONG && state_ != PosState::SHORT && 
                    state_ != PosState::PENDING_EXIT && state_ != PosState::PENDING_EMERGENCY_EXIT &&
                    state_ != PosState::PENDING_EMERGENCY_CANCEL) { 
                    
                    state_ = (up.real_position_amt > 0) ? PosState::LONG : PosState::SHORT;
                    is_long_ = (up.real_position_amt > 0);
                    position_qty_ = current_pos_qty;
                    entry_price_ = up.avg_price > 0 ? up.avg_price : entry_price_; 
                    
                    // [캐싱] REAL_POS 복구 시 역수 계산
                    if (entry_price_ > 0.0) inv_entry_price_ = 1.0 / entry_price_;
                    
                    peak_price_ = entry_price_; 
                    trade_ws_->cancel_all_orders(symbol_.c_str()); 
                    last_req_time_ = now; 
                }
            } else {
                if (state_ != PosState::NONE && state_ != PosState::PENDING_ENTRY) {
                    state_ = PosState::NONE;
                    trade_ws_->cancel_all_orders(symbol_.c_str());
                    last_req_time_ = now;
                }
            }
            return;
        }
        
        if (strcmp(up.status, "NEW") == 0) return;
        bool is_filled = is_status_filled(up.status) || is_status_partial(up.status);
        bool is_entry_order = is_ent_prefix(up.client_order_id);
        bool is_exit_order  = is_ext_prefix(up.client_order_id);
        
        if (is_status_canceled(up.status) || is_status_expired(up.status) || is_status_rejected(up.status)) {
        
            // 하드 스탑(시장가) 주문 자체가 거절
            if (state_ == PosState::PENDING_EMERGENCY_CANCEL) {
                if (is_status_rejected(up.status) || is_status_expired(up.status)) {
                    state_ = is_long_ ? PosState::LONG : PosState::SHORT;
                }
                return;
            }
        
            // 소프트 스탑 중 GTX(Post-Only) 함정에 빠졌을 때
            if (is_exit_order && state_ == PosState::PENDING_EMERGENCY_EXIT) {
                if (strcmp(up.client_order_id, current_exit_cid_) == 0) {
                    if (is_status_expired(up.status) || is_status_rejected(up.status)) {
                        double exit_qty = (position_qty_ > 0.001) ? position_qty_ : risk_.qty;
                        trade_ws_->send_market_order_ws(symbol_.c_str(), is_long_ ? "SELL" : "BUY", exit_qty, true);
                        state_ = PosState::PENDING_EMERGENCY_CANCEL;
                        last_req_time_ = now;
                    }
                }
                return;
            }
            if (is_entry_order && (state_ == PosState::PENDING_ENTRY || state_ == PosState::PENDING_CANCEL)) {
                if (strcmp(up.client_order_id, current_entry_cid_) == 0) state_ = PosState::NONE; 
            } 
            else if (is_exit_order && state_ == PosState::PENDING_EXIT) {
                if (strcmp(up.client_order_id, current_exit_cid_) == 0) state_ = is_long_ ? PosState::LONG : PosState::SHORT;
            }
            return;
        }

        if (is_filled) {
            
            // 시장가 손절
            if ((state_ == PosState::PENDING_EMERGENCY_EXIT || state_ == PosState::PENDING_EMERGENCY_CANCEL) && 
                 is_status_filled(up.status)) {
                state_ = PosState::NONE;
                position_qty_ = 0;
                return;
            }
            if (is_entry_order) {
                // 진입 주문 체결 (부분체결이든 완전체결이든 지속적으로 LONG/SHORT 유지 및 평단가 갱신)
                if (is_status_partial(up.status)) {
                    trade_ws_->cancel_order_ws(symbol_.c_str(), current_entry_cid_);
                    last_req_time_ = now; 
                }
                
                entry_price_ = up.avg_price;
                if (entry_price_ > 0.0) inv_entry_price_ = 1.0 / entry_price_;
                
                peak_price_ = entry_price_; 
                position_qty_ = up.filled_qty;
                
                state_ = (strcmp(up.side, "BUY") == 0) ? PosState::LONG : PosState::SHORT;
                is_long_ = (state_ == PosState::LONG);
                LOG(">>> [Order] Entry %s @ %.2f (Qty: %.4f)", up.status, entry_price_, position_qty_);
            }
            else if (is_exit_order) {
                // 청산(익절/손절) 주문 체결
                if (is_status_filled(up.status)) {
                    state_ = PosState::NONE;
                    LOG(">>> [Order] Position Fully Closed @ %.2f", up.avg_price);
                } else {
                    LOG(">>> [Order] Position Partially Closed @ %.2f", up.avg_price);
                    last_req_time_ = now;
                }
            }
        }
    }
    
    void update_risk(RiskParams new_risk) { risk_ = new_risk; }

    void force_exit(const char* reason, double now) {
    
        // PENDING_ENTRY 도중 강제 취소
        if (state_ == PosState::PENDING_ENTRY || state_ == PosState::PENDING_CANCEL) {
            LOG(">>> [EMERGENCY] %s! Aborting Pending Entry Order!", reason);
            if (current_entry_cid_[0] != '\0') trade_ws_->cancel_order_ws(symbol_.c_str(), current_entry_cid_);
            trade_ws_->cancel_all_orders(symbol_.c_str());
            state_ = PosState::PENDING_CANCEL; 
            last_req_time_ = now;
            return;
        }
        
        // 포지션이 있거나, 익절 대기 중
        if (state_ == PosState::LONG || state_ == PosState::SHORT || state_ == PosState::PENDING_EXIT) {
            LOG(">>> [Order] Force Exit (%s). Initiating Limit Chase!", reason);
            trade_ws_->cancel_all_orders(symbol_.c_str());
            state_ = PosState::PENDING_EMERGENCY_EXIT;
            current_exit_cid_[0] = '\0';
            last_req_time_ = now - 0.15; // next tick에 즉시 Limit Chase가 발동
            entry_time_ = now;
        }
    
    }
    
    // 스팟 인스턴스 셧다운용
    void try_instance_exit(double now) {
        // 진입 대기 중이면 취소하고 봇 즉시 정지
        if (state_ == PosState::PENDING_ENTRY || state_ == PosState::PENDING_CANCEL) {
            LOG(">>> [Shutdown] Aborting Pending Entry Order!");
            trade_ws_->cancel_all_orders(symbol_.c_str());
            state_ = PosState::NONE;
            return;
        }

        // 포지션이 있는데 아직 탈출 모드가 아니면, 비상 탈출 모드로 진입
        if (state_ == PosState::LONG || state_ == PosState::SHORT || state_ == PosState::PENDING_EXIT) {
            LOG(">>> [Shutdown] Initiating Graceful Limit Chase! (Will panic sell in 2.0s if unfilled)");
            trade_ws_->cancel_all_orders(symbol_.c_str());
            state_ = PosState::PENDING_EMERGENCY_EXIT;
            current_exit_cid_[0] = '\0';
            last_req_time_ = now - 0.15;
            entry_time_ = now;
        }
        
    }

    // 안전장치 10초후 강제 청산
    void force_maket_sell(double now) {
        if (state_ != PosState::NONE && state_ != PosState::PENDING_EMERGENCY_CANCEL) {
            LOG(">>> [Shutdown] 10s Timeout Hit! Forcing MARKET SELL.");
            if (current_exit_cid_[0] != '\0') trade_ws_->cancel_order_ws(symbol_.c_str(), current_exit_cid_);
            double exit_qty = (position_qty_ > 0.001) ? position_qty_ : risk_.qty;
            trade_ws_->send_market_order_ws(symbol_.c_str(), is_long_ ? "SELL" : "BUY", exit_qty, true);
            state_ = PosState::PENDING_EMERGENCY_CANCEL;
            last_req_time_ = now;
        }
    }
    
    double get_position() const {
        return position_qty_;
    }
};
