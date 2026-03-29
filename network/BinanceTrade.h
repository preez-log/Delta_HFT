#pragma once
#include "SocketBase.h"
#include <cstdio>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <atomic>
#include <openssl/hmac.h> 
#include <netinet/tcp.h>
#include <utility>
#include <map>
#include "../trade/Types.h"
#include "../core/LockFreeQueue.h"
#include "../core/Config.h"
#include "../simdjson.h"

// [OrderSocket Class]
// 하이브리드 모드에서도 User Data Stream(체결 알림)을 위해 필요함
class OrderSocket : public SocketBase<OrderSocket> {
private:
    simdjson::ondemand::parser parser_;

public:
    OrderSocket() : SocketBase<OrderSocket>("ws-fapi.binance.com", 443) {}

    bool is_connected() const { return connected; }

    void on_message(char* data, size_t len) {
        // [디버그] 응답 메시지 확인
        if (len > 0) {
            std::string msg(data, len);
            // Ping 메시지가 아닐 때만 로그 출력 (로그 폭주 방지)
            //if (msg.find("ping") == std::string::npos) {
            //    LOG(">>> [OrderWS-RES] %s", msg.c_str());
            //}
        }
    }
};

// [BinanceTrade Class - Hybrid Mode]
class BinanceTrade : public SocketBase<BinanceTrade> {
private:
    const Config& config_;
    std::string api_key_;
    std::string secret_key_;
    LockFreeQueue<OrderUpdate>* queue_;
    simdjson::ondemand::parser parser_;
    
    std::string symbol_;
    std::string listen_key_;
    
    std::thread keepalive_thread_;
    std::atomic<bool> keepalive_running_{false};
    
    SSL_CTX* rest_ctx_ = nullptr;
    SSL* rest_ssl_ = nullptr;
    int rest_sock_ = -1;
    
    // User Data Stream용 소켓 (체결 확인용)
    OrderSocket* order_ws_ = nullptr;
    std::thread order_read_thread_;
    std::atomic<bool> order_ws_running_{false};
    std::atomic<long long> ws_req_id_{0};
    
    std::mutex reconnect_mutex_;
    std::mutex http_mutex_;

public:
    BinanceTrade(const Config& cfg, LockFreeQueue<OrderUpdate>* q, const std::string& sym)
        : SocketBase<BinanceTrade>("fstream.binance.com", 443), config_(cfg),
          api_key_(cfg.api_key), secret_key_(cfg.secret_key), queue_(q), symbol_(sym) {
            rest_ctx_ = SSL_CTX_new(TLS_client_method());
            order_ws_ = new OrderSocket();
        }
    
    ~BinanceTrade() {
        keepalive_running_ = false;
        order_ws_running_ = false;
        
        if (keepalive_thread_.joinable()) keepalive_thread_.join();
        if (order_read_thread_.joinable()) order_read_thread_.join();
        
        close_rest_session();
        if (rest_ctx_) SSL_CTX_free(rest_ctx_);
        if (order_ws_) delete order_ws_;
    }

    bool connect() {
        // 1. ListenKey 발급 (HTTP)
        if (listen_key_.empty()) {
            if (!fetch_listen_key()) {
                LOG(">>> [trade] Failed to get ListenKey!");
                return false;
            }
        }
        
        // 2. User Data Stream 연결 (체결 확인용 필수)
        std::string path = "/ws/" + listen_key_;
        if (!SocketBase::connect(path)) return false;

        // 3. Keep-alive 스레드
        if (!keepalive_running_.load()) {
            keepalive_running_ = true;
            keepalive_thread_ = std::thread(&BinanceTrade::keepalive_loop, this);
        }
        
        // 4.Order WS 연결 유지
        if (!order_ws_->connect("/ws-fapi/v1")) {
            LOG(">>> [Warning] Order WS Connect Failed (But HTTP Order is OK).");
        } else if (!order_ws_running_.load()) {
            order_ws_running_ = true;
            order_read_thread_ = std::thread([this]() {
                while (order_ws_running_) {
                     // 연결 끊겨있으면 재연결 시도
                     if (!order_ws_->is_connected()) {
                         LOG(">>> [system] Order WS Disconnected! Reconnecting...");
                         std::this_thread::sleep_for(std::chrono::seconds(5));
                         
                         if (order_ws_->connect("/ws-fapi/v1")) {
                             LOG(">>> [system] Order WS Reconnected!");
                         } else {
                             // 재연결 실패 시 잠시 대기 후 재시도
                             std::this_thread::sleep_for(std::chrono::seconds(5));
                             continue; 
                         }
                     }

                     order_ws_->poll();
                     std::this_thread::sleep_for(std::chrono::microseconds(10));
                     
                     // poll 이후에도 끊겼는지 체크
                     if (!order_ws_->is_connected()) {
                         std::this_thread::sleep_for(std::chrono::milliseconds(100));
                     }
                }
            });
        }
        
        LOG(">>> [trade] Hybrid Mode Active (Web Socket)");
        return true;
    }
    
    bool auto_detect_precision(const char* target_symbol, int& out_price_prec, int& out_qty_prec) {
        LOG(">>> [System] Fetching Exchange Info for %s...", target_symbol);
        
        // 1. 거래소 전체 스펙 조회 (HTTP GET)
        std::string response = send_http_and_get_body("GET", "/fapi/v1/exchangeInfo", "");
        if (response.empty()) return false;

        try {
            simdjson::ondemand::parser parser;
            simdjson::padded_string json_data(response);
            simdjson::ondemand::document doc;
            
            if (parser.iterate(json_data).get(doc)) return false;
            
            // 2. symbols 배열 순회하며 내 코인 찾기
            simdjson::ondemand::array symbols;
            if (doc["symbols"].get_array().get(symbols)) return false;

            std::string upper_target = target_symbol;
            std::transform(upper_target.begin(), upper_target.end(), upper_target.begin(), ::toupper);

            for (auto sym_obj : symbols) {
                std::string_view sym_name;
                if (!sym_obj["symbol"].get(sym_name)) {
                    if (sym_name == upper_target) {
                        // pricePrecision과 quantityPrecision 추출
                        out_price_prec = (int)sym_obj["pricePrecision"].get_int64();
                        out_qty_prec = (int)sym_obj["quantityPrecision"].get_int64();
                        
                        LOG(">>> [System] %s Auto-Configured! Price Decimals: %d, Qty Decimals: %d", 
                            upper_target.c_str(), out_price_prec, out_qty_prec);
                        return true;
                    }
                }
            }
        } catch (...) {
            LOG(">>> [Error] Failed to parse exchangeInfo!");
            return false;
        }
        
        LOG(">>> [Error] Symbol %s not found in exchangeInfo!", target_symbol);
        return false;
    }
    
    // ---------- [HYBRID ORDER FUNCTIONS (HTTP REST API)] ----------

    // 1. 지정가 주문 (HTTP)
    bool send_limit_order(const char* symbol, const char* side, double qty, double price, bool post_only) {
        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::stringstream ss_price;
        ss_price << std::fixed << std::setprecision(config_.exchange.price_precision) << price;
        std::string price_str = ss_price.str();

        // Query String 구성
        std::stringstream query_ss;
        query_ss << "apiKey=" << api_key_
                 << "&price=" << price_str
                 << "&quantity=" << qty
                 << "&side=" << side
                 << "&symbol=" << symbol
                 << "&timeInForce=" << (post_only ? "GTX" : "GTC")
                 << "&timestamp=" << timestamp
                 << "&type=LIMIT";

        std::string query = query_ss.str();
        std::string signature = hmac_sha256(secret_key_, query);
        std::string full_query = query + "&signature=" + signature;

        // [HTTP 전송] 응답을 즉시 받아 확인
        std::string response = send_http_and_get_body("POST", "/fapi/v1/order", full_query);
        
        // 응답 검증
        if (response.empty()) return false;
        
        // 성공 시 orderId가 포함됨
        if (response.find("\"orderId\"") != std::string::npos) {
            // LOG(">>> [Order-HTTP] Limit Order Placed: %s", price_str.c_str());
            return true;
        } else {
            // 실패 시 에러 로그 출력
            LOG(">>> [Order-WS] Limit FAILED: %s", response.c_str());
            return false;
        }
    }
    
    // 1. 지정가 주문 (socket)
    bool send_limit_order_ws(const char* symbol, const char* side, double qty, double price, bool post_only) {
        if (!order_ws_ || !order_ws_->is_connected()) return false;

        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        // 1. 파라미터 맵 구성 (자동으로 Key 기준 알파벳 정렬됨 -> 서명 순서 보장)
        std::map<std::string, std::string> params;
        
        std::stringstream ss_price, ss_qty;
        ss_price << std::fixed << std::setprecision(config_.exchange.price_precision) << price;
        ss_qty << std::fixed << std::setprecision(4) << qty; // 수량 정밀도 체크 필요

        params["apiKey"] = api_key_;
        params["price"] = ss_price.str();
        params["quantity"] = ss_qty.str();
        params["side"] = side;
        params["symbol"] = symbol;
        params["timeInForce"] = post_only ? "GTX" : "GTC";
        params["timestamp"] = std::to_string(timestamp);
        params["type"] = "LIMIT";

        // 2. Query String 생성 (서명용)
        std::string query_str;
        for (const auto& p : params) {
            if (!query_str.empty()) query_str += "&";
            query_str += p.first + "=" + p.second;
        }

        // 3. 서명 생성
        std::string signature = hmac_sha256(secret_key_, query_str);
        params["signature"] = signature;

        // 4. 최종 JSON 생성 (simdjson은 파싱용이라 빌더는 수동으로 문자열 조합이 빠름)
        std::stringstream json_ss;
        json_ss << "{"
                << "\"id\":\"" << "ws_order_" << ++ws_req_id_ << "\","
                << "\"method\":\"order.place\","
                << "\"params\":{";
        
        bool first = true;
        for (const auto& p : params) {
            if (!first) json_ss << ",";
            json_ss << "\"" << p.first << "\":\"" << p.second << "\"";
            first = false;
        }
        json_ss << "}}";

        // 5. 전송 (Non-blocking)
        order_ws_->send(json_ss.str());
        
        // 로그는 최소화 (속도 위해)
        // LOG(">>> [WS-Order] Sent: %s", side); 
        return true; 
    }
    
    // 2. 시장가 주문 (HTTP)
    bool send_market_order(const char* symbol, const char* side, double qty, bool reduce_only) {
        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::stringstream ss_qty;
        ss_qty << std::fixed << std::setprecision(config_.exchange.qty_precision) << qty;
        std::string qty_str = ss_qty.str();

        std::stringstream query_ss;
        query_ss << "apiKey=" << api_key_ << "&quantity=" << qty_str;
        if (reduce_only) query_ss << "&reduceOnly=true";
        query_ss << "&side=" << side << "&symbol=" << symbol << "&timestamp=" << timestamp << "&type=MARKET";

        std::string query = query_ss.str();
        std::string signature = hmac_sha256(secret_key_, query);
        std::string full_query = query + "&signature=" + signature;

        std::string response = send_http_and_get_body("POST", "/fapi/v1/order", full_query);
        
        if (response.find("\"orderId\"") != std::string::npos) {
            LOG(">>> [Order-WS] Market Order Placed!");
            return true;
        } else {
            LOG(">>> [Order-WS] Market FAILED: %s", response.c_str());
            return false;
        }
    }
    
    // 2. 시장가 주문 (socket)
    bool send_market_order_ws(const char* symbol, const char* side, double qty, bool reduce_only) {
         if (!order_ws_ || !order_ws_->is_connected()) return false;

        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::map<std::string, std::string> params;
        std::stringstream ss_qty;
        ss_qty << std::fixed << std::setprecision(config_.exchange.qty_precision) << qty;

        params["apiKey"] = api_key_;
        params["quantity"] = ss_qty.str();
        if (reduce_only) params["reduceOnly"] = "true";
        params["side"] = side;
        params["symbol"] = symbol;
        params["timestamp"] = std::to_string(timestamp);
        params["type"] = "MARKET";

        std::string query_str;
        for (const auto& p : params) {
            if (!query_str.empty()) query_str += "&";
            query_str += p.first + "=" + p.second;
        }

        std::string signature = hmac_sha256(secret_key_, query_str);
        params["signature"] = signature;

        std::stringstream json_ss;
        json_ss << "{"
                << "\"id\":\"" << "ws_mkt_" << ++ws_req_id_ << "\","
                << "\"method\":\"order.place\","
                << "\"params\":{";
        
        bool first = true;
        for (const auto& p : params) {
            if (!first) json_ss << ",";
            json_ss << "\"" << p.first << "\":\"" << p.second << "\"";
            first = false;
        }
        json_ss << "}}";

        order_ws_->send(json_ss.str());
        return true;
    }

    // 3. 주문 취소 ALL (HTTP)
    bool cancel_all_orders(const char* symbol) {
        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();

        std::stringstream query_ss;
        query_ss << "apiKey=" << api_key_ << "&symbol=" << symbol << "&timestamp=" << timestamp;

        std::string query = query_ss.str();
        std::string signature = hmac_sha256(secret_key_, query);
        std::string full_query = query + "&signature=" + signature;

        // DELETE 요청
        std::string response = send_http_and_get_body("DELETE", "/fapi/v1/allOpenOrders", full_query);
        
        // 취소는 성공하든 실패하든(주문이 없어서 실패 등) true 리턴해서 로직 진행
        return true; 
    }
    
    // 3. 주문 취소 ALL (SOCKET)
    bool cancel_all_orders_ws(const char* symbol) {
        if (!order_ws_ || !order_ws_->is_connected()) return false;
        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        // 1. 서명(Signature)을 위한 파라미터 구성 (알파벳 순서: apiKey, symbol, timestamp)
        char query[256];
        snprintf(query, sizeof(query),
            "apiKey=%s&symbol=%s&timestamp=%lld",
            api_key_.c_str(), symbol, timestamp);

        char signature[65];
        hmac_sha256_fast(secret_key_.c_str(), query, signature);

        // 2. 전송할 JSON 페이로드 구성
        char json[512];
        snprintf(json, sizeof(json),
            "{\"id\":\"ws_ccl_all_%lld\",\"method\":\"allOpenOrders\",\"params\":{"
            "\"apiKey\":\"%s\",\"symbol\":\"%s\",\"timestamp\":\"%lld\",\"signature\":\"%s\"}}",
            (long long)++ws_req_id_, api_key_.c_str(), symbol, timestamp, signature);

        order_ws_->send(json);

        // 디버깅/추적을 위한 로그
        LOG(">>> [WS] Sent Cancel All Orders for %s", symbol);
        return true;
    }

    // 3. 주문 취소 (SOCKET)
    bool cancel_order_ws(const char* symbol, const char* orig_cid) {
        if (!order_ws_ || !order_ws_->is_connected()) return false;
        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        char query[256];
        snprintf(query, sizeof(query), 
            "apiKey=%s&origClientOrderId=%s&symbol=%s&timestamp=%lld",
            api_key_.c_str(), orig_cid, symbol, timestamp);

        char signature[65];
        hmac_sha256_fast(secret_key_.c_str(), query, signature);

        char json[512];
        snprintf(json, sizeof(json),
            "{\"id\":\"ws_ccl_%lld\",\"method\":\"order.cancel\",\"params\":{"
            "\"apiKey\":\"%s\",\"origClientOrderId\":\"%s\",\"symbol\":\"%s\",\"timestamp\":\"%lld\",\"signature\":\"%s\"}}",
            (long long)++ws_req_id_, api_key_.c_str(), orig_cid, symbol, timestamp, signature);

        order_ws_->send(json);
        return true;
    }

    // Client Order ID를 지정하여 지정가 주문을 넣는 함수
    bool send_limit_order_ws_cid(const char* symbol, const char* side, double qty, double price, bool post_only, const char* cid) {
        if (!order_ws_ || !order_ws_->is_connected()) return false;
        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();

        // 1. 파라미터는 반드시 알파벳 순서대로 하드코딩! (속도 극대화)
        char query[512];
        snprintf(query, sizeof(query), 
            "apiKey=%s&newClientOrderId=%s&price=%.*f&quantity=%.*f&side=%s&symbol=%s&timeInForce=%s&timestamp=%lld&type=LIMIT",
            api_key_.c_str(), cid, config_.exchange.price_precision, price, config_.exchange.qty_precision, qty, side, symbol, (post_only ? "GTX" : "GTC"), timestamp);

        char signature[65];
        hmac_sha256_fast(secret_key_.c_str(), query, signature);

        char json[1024];
        snprintf(json, sizeof(json),
            "{\"id\":\"ws_ord_%lld\",\"method\":\"order.place\",\"params\":{"
            "\"apiKey\":\"%s\",\"newClientOrderId\":\"%s\",\"price\":\"%.*f\",\"quantity\":\"%.*f\","
            "\"side\":\"%s\",\"symbol\":\"%s\",\"timeInForce\":\"%s\",\"timestamp\":\"%lld\",\"type\":\"LIMIT\",\"signature\":\"%s\"}}",
            (long long)++ws_req_id_, api_key_.c_str(), cid, config_.exchange.price_precision, price, config_.exchange.qty_precision, qty, side, symbol, 
                        (post_only ? "GTX" : "GTC"), timestamp, signature);

        order_ws_->send(json);
        return true; 
    }
    
    // Cancel & Replace - 청산용 (post_only=true, GTX)
    bool modify_limit_order_ws(const char* symbol, const char* side, double qty, double price, const char* orig_cid, const char* new_cid) {
        if (!order_ws_ || !order_ws_->is_connected()) return false;
        cancel_order_ws(symbol, orig_cid);
        send_limit_order_ws_cid(symbol, side, qty, price, true, new_cid); // GTX: maker 보장
        return true; 
    }

    // [FIX 5] Cancel & Replace - 진입용 (post_only=false, GTC)
    // GTX는 즉시 체결될 것 같으면 EXPIRED 처리 → 진입 CANCELED 폭탄의 원인
    bool modify_entry_order_ws(const char* symbol, const char* side, double qty, double price, const char* orig_cid, const char* new_cid) {
        if (!order_ws_ || !order_ws_->is_connected()) return false;
        cancel_order_ws(symbol, orig_cid);
        send_limit_order_ws_cid(symbol, side, qty, price, false, new_cid); // GTC: 체결 우선
        return true; 
    }

    // ---------- [Common Logic (HTTP / Parsing)] ----------

    bool ensure_connection() {
        if (rest_ssl_ && rest_sock_ != -1) return true;
        rest_sock_ = socket(AF_INET, SOCK_STREAM, 0);
        if (rest_sock_ < 0) return false;
        int flag = 1;
        setsockopt(rest_sock_, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
        struct hostent* host = gethostbyname("fapi.binance.com");
        if (!host) { close(rest_sock_); return false; }
        struct sockaddr_in addr;
        addr.sin_family = AF_INET;
        addr.sin_port = htons(443);
        addr.sin_addr = *((struct in_addr*)host->h_addr);
        if (::connect(rest_sock_, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
            close_rest_session(); return false;
        }
        rest_ssl_ = SSL_new(rest_ctx_);
        SSL_set_fd(rest_ssl_, rest_sock_);
        SSL_set_tlsext_host_name(rest_ssl_, "fapi.binance.com");
        if (SSL_connect(rest_ssl_) != 1) {
            close_rest_session(); return false;
        }
        return true;
    }

    void close_rest_session() {
        if (rest_ssl_) { SSL_free(rest_ssl_); rest_ssl_ = nullptr; }
        if (rest_sock_ != -1) { close(rest_sock_); rest_sock_ = -1; }
    }

    std::string send_http_and_get_body(const std::string& method, const std::string& endpoint, const std::string& query) {
    
        std::lock_guard<std::mutex> lock(http_mutex_);
        
        for (int attempt = 0; attempt < 2; ++attempt) {
            if (!ensure_connection()) {
                close_rest_session();
                if (attempt == 0) continue;
                return "";
            }
            std::stringstream req;
            
            req << method << " " << endpoint << (query.empty() ? "" : "?") << query << " HTTP/1.1\r\n"
                << "Host: fapi.binance.com\r\n"
                << "X-MBX-APIKEY: " << api_key_ << "\r\n"
                << "Content-Length: 0\r\n"
                << "Connection: keep-alive\r\n\r\n";

            std::string req_str = req.str();
            if (SSL_write(rest_ssl_, req_str.c_str(), req_str.length()) <= 0) {
                close_rest_session();
                if (attempt == 0) continue;
                return "";
            }
            std::string raw_response;
            char buf[4096];
            int content_length = -1;
            bool header_parsed = false;
            size_t body_start_pos = std::string::npos;
            bool read_started = false;
            while (true) {
                int bytes = SSL_read(rest_ssl_, buf, sizeof(buf) - 1);
                if (bytes <= 0) {
                    if (!read_started && attempt == 0) {
                        close_rest_session();
                        break;
                    }
                    break;
                }
                read_started = true;
                buf[bytes] = 0;
                raw_response.append(buf, bytes);
                if (!header_parsed) {
                    body_start_pos = raw_response.find("\r\n\r\n");
                    if (body_start_pos != std::string::npos) {
                        header_parsed = true;
                        body_start_pos += 4;
                        size_t cl_pos = raw_response.find("Content-Length: ");
                        if (cl_pos == std::string::npos) cl_pos = raw_response.find("content-length: ");
                        if (cl_pos != std::string::npos) {
                            size_t end_line = raw_response.find("\r\n", cl_pos);
                            std::string cl_str = raw_response.substr(cl_pos + 16, end_line - (cl_pos + 16));
                            try { content_length = std::stoi(cl_str); } catch(...) { content_length = -1; }
                        }
                    }
                }
                if (header_parsed) {
                    if (content_length != -1) {
                        if (raw_response.size() >= body_start_pos + content_length) goto success;
                    } else {
                        char last = raw_response.back();
                        if (last == '}' || last == ']') goto success;
                    }
                }
            }
            if (!read_started && attempt == 0) continue;
            success:
            if (body_start_pos != std::string::npos && raw_response.size() > body_start_pos) {
                return raw_response.substr(body_start_pos);
            }
            close_rest_session();
        }
        return "";
    }

    bool send_http_generic(const std::string& method, const std::string& endpoint, const std::string& query, bool is_listen_key) {
         std::string body = send_http_and_get_body(method, endpoint, query);
         if (body.empty()) return false;
         if (is_listen_key && method == "POST") {
             simdjson::ondemand::parser local_parser;
             simdjson::padded_string json_body(body);
             simdjson::ondemand::document doc;
             std::string_view key_view;
             if (!local_parser.iterate(json_body).get(doc) && !doc["listenKey"].get(key_view)) {
                 listen_key_ = std::string(key_view);
                 return true;
             }
             
             // 디버그
             LOG(">>> [Error] ListenKey Failed! Binance Reply: %s", body.c_str());
             return false;
         }
         return true;
    }
    
    bool send_http_post(const std::string& endpoint, const std::string& query, bool is_listen_key) {
        return send_http_generic("POST", endpoint, query, is_listen_key);
    }
    bool send_http_put(const std::string& endpoint) {
        return send_http_generic("PUT", endpoint, "", false);
    }
    bool fetch_listen_key() { return send_http_post("/fapi/v1/listenKey", "", true); }
    bool extend_listen_key() { return send_http_put("/fapi/v1/listenKey"); }

    void on_message(char* data, size_t len) {
        simdjson::padded_string json(data, len);
        simdjson::ondemand::document doc;
        
        if (parser_.iterate(json).get(doc)) return;

        std::string_view event_type;
        if (doc["e"].get(event_type)) return;
        
        if (event_type == "ACCOUNT_UPDATE") {
            simdjson::ondemand::object a;
            if (doc["a"].get(a)) return;
            
            simdjson::ondemand::array positions;
            if (a["P"].get_array().get(positions)) return;

            for (auto position : positions) {
                std::string_view sym;
                if (!position["s"].get(sym)) {
                    std::string sym_lower(sym);
                    std::transform(sym_lower.begin(), sym_lower.end(), sym_lower.begin(), ::tolower);
                    
                    if (sym_lower == symbol_) {
                        std::string_view pa_str, ep_str;
                        if (!position["pa"].get(pa_str) && !position["ep"].get(ep_str)) {
                            OrderUpdate sync_msg;
                            memset(&sync_msg, 0, sizeof(OrderUpdate));
                        
                            strncpy(sync_msg.symbol, symbol_.c_str(), sizeof(sync_msg.symbol) - 1);
                            strcpy(sync_msg.status, "REAL_POS"); // 특수 식별자
                        
                            sync_msg.real_position_amt = std::stod(std::string(pa_str));
                            sync_msg.avg_price = std::stod(std::string(ep_str));
                            sync_msg.is_real_trade = false;
                        
                            queue_->push(sync_msg);
                            // LOG(">>> [WS] Real Position Synced: %.4f @ %.2f", sync_msg.real_position_amt, sync_msg.avg_price);
                        }
                        break; 
                    }
                }
            }
            return;
        }

        if (event_type == "ORDER_TRADE_UPDATE") {
            simdjson::ondemand::object o;
            if (doc["o"].get(o)) return;
            
            OrderUpdate up;
            memset(&up, 0, sizeof(OrderUpdate));
            bool found_status = false;
            std::string_view status_view;

            for (auto field : o) {
                std::string_view key;
                if (field.unescaped_key().get(key)) continue;

                if (key == "s") { 
                    std::string_view val; 
                    if (!field.value().get(val)) {
                        size_t copy_len = std::min(val.length(), sizeof(up.symbol) - 1);
                        for(size_t i=0; i<copy_len; ++i) up.symbol[i] = std::tolower(val[i]);
                        up.symbol[copy_len] = '\0';
                    }
                } else if (key == "c") {
                    std::string_view val; 
                    if (!field.value().get(val)) {
                        size_t copy_len = std::min(val.length(), sizeof(up.client_order_id) - 1);
                        strncpy(up.client_order_id, val.data(), copy_len); 
                        up.client_order_id[copy_len] = '\0';
                    }
                } else if (key == "S") { 
                    std::string_view val; 
                    if (!field.value().get(val)) {
                        size_t copy_len = std::min(val.length(), sizeof(up.side) - 1);
                        strncpy(up.side, val.data(), copy_len); up.side[copy_len] = '\0';
                    }
                } else if (key == "X") { 
                    if (!field.value().get(status_view)) {
                        found_status = true;
                        size_t copy_len = std::min(status_view.length(), sizeof(up.status) - 1);
                        strncpy(up.status, status_view.data(), copy_len); up.status[copy_len] = '\0';
                    }
                } else if (key == "ap") { 
                    std::string_view val; 
                    if (!field.value().get(val)) up.avg_price = std::strtod(val.data(), nullptr);
                } else if (key == "z") { 
                    std::string_view val; 
                    if (!field.value().get(val)) up.filled_qty = std::strtod(val.data(), nullptr);
                }
            }
            
            if (found_status) {
                if (status_view == "FILLED" || status_view == "PARTIALLY_FILLED" || 
                    status_view == "NEW" || status_view == "CANCELED" || 
                    status_view == "EXPIRED" || status_view == "REJECTED") {
                    
                    up.is_real_trade = true;
                    queue_->push(up);
                    
                    if (status_view != "NEW") { 
                        LOG_TRADE("exec", "symbol=\"%s\" side=\"%s\" qty=%.4f price=%.2f status=\"%s\"", 
                                  up.symbol, up.side, up.filled_qty, up.avg_price, up.status);
                    }
                }
            }
        }
    }
    
    void sync_position_task() {
        std::pair<double, double> pos_info = fetch_position_info(symbol_.c_str());
        OrderUpdate sync_msg;
        memset(&sync_msg, 0, sizeof(OrderUpdate)); 
        
        strcpy(sync_msg.symbol, symbol_.c_str());
        strcpy(sync_msg.status, "REAL_POS");
        sync_msg.real_position_amt = pos_info.first;   
        sync_msg.avg_price = pos_info.second;  
        sync_msg.is_real_trade = false; 
        queue_->push(sync_msg);
    }

    std::pair<double, double> fetch_position_info(const char* symbol) {
        long long timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::stringstream ss;
        ss << "symbol=" << symbol << "&timestamp=" << timestamp;
        std::string query = ss.str();
        std::string signature = hmac_sha256(secret_key_, query);
        std::string full_query = query + "&signature=" + signature;
        std::string json_body = send_http_and_get_body("GET", "/fapi/v2/positionRisk", full_query);

        if (json_body.empty()) return {0.0, 0.0};
        double qty = 0.0;
        double entry_price = 0.0;
        try {
            simdjson::ondemand::parser local_parser;
            simdjson::padded_string json_data(json_body);
            simdjson::ondemand::document doc;
            simdjson::ondemand::array arr;
            if (!local_parser.iterate(json_data).get(doc)) {
                if (!doc.get_array().get(arr)) {
                    for (auto item : arr) {
                        std::string_view amt_str, entry_str;
                        auto err1 = item["positionAmt"].get(amt_str);
                        auto err2 = item["entryPrice"].get(entry_str);
                        if (!err1 && !err2) {
                            qty = std::strtod(amt_str.data(), nullptr);
                            entry_price = std::strtod(entry_str.data(), nullptr);
                            break; 
                        }
                    }
                }
            }
        }
        catch (...) { return {0.0, 0.0}; }
        return {qty, entry_price};
    }

    inline void hmac_sha256_fast(const char* key, const char* data, char* out_hex) {
        unsigned char digest[EVP_MAX_MD_SIZE];
        unsigned int len = 0;
        HMAC(EVP_sha256(), key, strlen(key), (const unsigned char*)data, strlen(data), digest, &len);
        static const char hex_chars[] = "0123456789abcdef";
        for (unsigned int i = 0; i < len; ++i) {
            out_hex[i * 2]     = hex_chars[(digest[i] >> 4) & 0x0F];
            out_hex[i * 2 + 1] = hex_chars[digest[i] & 0x0F];
        }
        out_hex[len * 2] = '\0';
    }
    
    std::string hmac_sha256(const std::string& key, const std::string& data) {
        unsigned char* digest;
        unsigned int len = 0;
        digest = HMAC(EVP_sha256(), key.c_str(), key.length(), 
                      (unsigned char*)data.c_str(), data.length(), NULL, &len);
        std::stringstream ss;
        for(unsigned int i = 0; i < len; i++) {
            ss << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
        }
        return ss.str();
    }
    
    void keepalive_loop() {
        auto last_gcp_shield_time = std::chrono::steady_clock::now();
        auto last_put_time = std::chrono::steady_clock::now();
        auto last_sync_time = std::chrono::steady_clock::now();
        LOG(">>> [system] ListenKey Keep-alive Thread Started (40min interval)");
        
        while (keepalive_running_) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::minutes>(now - last_gcp_shield_time).count() >= 5) {
                this->send_ping(""); 
                if (order_ws_ && order_ws_->is_connected()) {
                    order_ws_->send_ping();
                }
                last_gcp_shield_time = now;
            }
            if (std::chrono::duration_cast<std::chrono::minutes>(now - last_put_time).count() >= 40) {
                extend_listen_key();
                LOG_INFO("sys", "Binance Life: listenKey extended.");
                last_put_time = now;
            }
            if (std::chrono::duration_cast<std::chrono::minutes>(now - last_sync_time).count() >= 1) {
                sync_position_task();
                last_sync_time = now;
            }
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
};
