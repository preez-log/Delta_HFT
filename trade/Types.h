#pragma once
#include <cstdint>

// [16 bytes aligned]
struct TickData {
    double timestamp;
    double price;
    double volume;
    
    double best_bid;   // 매수 1호가
    double best_ask;   // 매도 1호가
    
    double best_bid_qty;
    double best_ask_qty;
    
    bool is_buy;
};

// 주문 업데이트 정보
struct OrderUpdate {
    char symbol[12]; // std::string 제거 (고정 배열)
    char side[8];    // "BUY" or "SELL"
    char status[32]; // "FILLED", "NEW"...
    char client_order_id[32];
    double avg_price;
    double filled_qty;
    bool is_real_trade;
    double real_position_amt = 0.0;
};