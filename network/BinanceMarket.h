#pragma once
#include "SocketBase.h"
#include "../trade/Types.h"
#include "../core/LockFreeQueue.h"
#include "../simdjson.h"

// [초고속 파싱] 바이낸스 전용 Zero-overhead 문자열 -> double 변환기
inline double fast_atof(const char* p, size_t len) {
    int64_t val = 0;
    size_t i = 0;
    
    // 정수
    while (i < len && p[i] >= '0' && p[i] <= '9') {
        val = val * 10 + (p[i] - '0');
        i++;
    }
    
    // 소수
    if (i < len && p[i] == '.') {
        i++;
        int64_t frac = 1;
        while (i < len && p[i] >= '0' && p[i] <= '9') {
            val = val * 10 + (p[i] - '0');
            frac *= 10;
            i++;
        }
        // 최종적으로 한 번만 부동소수점 나눗셈 수행
        return static_cast<double>(val) / static_cast<double>(frac);
    }
    
    return static_cast<double>(val);
}

class BinanceMarket : public SocketBase<BinanceMarket> {
private:
    LockFreeQueue<TickData>* queue_;
    std::string symbol_;
    simdjson::ondemand::parser parser_; 
    
    double last_best_bid_ = 0.0;
    double last_best_ask_ = 0.0;
    double last_best_bid_qty_ = 0.0; 
    double last_best_ask_qty_ = 0.0;
    
    // [Zero-Allocation] 패딩 처리를 위한 내부 고정 버퍼 (힙 할당 방지)
    char padded_buffer_[4096]; 

public:
    BinanceMarket(LockFreeQueue<TickData>* q, const std::string& sym)
        : SocketBase<BinanceMarket>("fstream.binance.com", 443), queue_(q), symbol_(sym) {}

    void subscribe() {
        if (!connected) return;
        std::string sub_msg = "{\"method\":\"SUBSCRIBE\",\"params\":[\"" 
                            + symbol_ + "@bookTicker\",\"" 
                            + symbol_ + "@aggTrade\"],\"id\":1}";
        send(sub_msg);
        LOG(">>> [Market] Subscribed to %s @ bookTicker & aggTrade", symbol_.c_str());
    }

    void on_message(char* data, size_t len) {
        if (len + simdjson::SIMDJSON_PADDING > sizeof(padded_buffer_)) return; // 버퍼 초과 방지

        // 수신된 데이터를 미리 할당된 고정 스택 버퍼에 복사 후 패딩 영역 0 초기화
        std::memcpy(padded_buffer_, data, len);
        std::memset(padded_buffer_ + len, 0, simdjson::SIMDJSON_PADDING);

        // 메모리 할당 없이 view만 생성하여 파싱
        simdjson::padded_string_view json_view(padded_buffer_, len, len + simdjson::SIMDJSON_PADDING);
        simdjson::ondemand::document doc;
        
        if (parser_.iterate(json_view).get(doc)) return;

        std::string_view e_type;
        if (doc["e"].get(e_type)) return;
        
        TickData tick;
        memset(&tick, 0, sizeof(TickData));
        
        if (e_type == "bookTicker") {
            // --- 1. 호가창 업데이트 ---
            std::string_view b_str, a_str, B_str, A_str;
            if (!doc["b"].get(b_str) && !doc["a"].get(a_str) && 
                !doc["B"].get(B_str) && !doc["A"].get(A_str)) {
            
                tick.best_bid = fast_atof(b_str.data(), b_str.length());
                tick.best_ask = fast_atof(a_str.data(), a_str.length());
                tick.best_bid_qty = fast_atof(B_str.data(), B_str.length());
                tick.best_ask_qty = fast_atof(A_str.data(), A_str.length());
                
                last_best_bid_ = tick.best_bid;
                last_best_ask_ = tick.best_ask;
                last_best_bid_qty_ = tick.best_bid_qty; 
                last_best_ask_qty_ = tick.best_ask_qty;
            
                tick.price = (tick.best_bid + tick.best_ask) * 0.5; // Mid-price
                tick.volume = 0; // 호가 데이터이므로 체결량은 0
            
                queue_->push(tick);
            }
        } 
        else if (e_type == "aggTrade") {
            // --- 2. 실제 체결 업데이트 ---
            std::string_view p_str, q_str;
            bool is_market_maker; // m 필드: true면 매도자가 Market Maker (즉, 매수 Taker 체결)
        
            if (!doc["p"].get(p_str) && !doc["q"].get(q_str) && !doc["m"].get(is_market_maker)) {
                tick.price = fast_atof(p_str.data(), p_str.length());
                tick.volume = fast_atof(q_str.data(), q_str.length());
                
                tick.best_bid = last_best_bid_;
                tick.best_ask = last_best_ask_;
                tick.best_bid_qty = last_best_bid_qty_;
                tick.best_ask_qty = last_best_ask_qty_;
            
                // 바이낸스 'm' 필드: Taker가 Buy면 m=false, Taker가 Sell이면 m=true
                tick.is_buy = !is_market_maker; 
            
                queue_->push(tick);
            }
        }
    }
};