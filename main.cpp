#include <iostream>
#include <thread>
#include <atomic>
#include <csignal>
#include <cctype>
#include <algorithm>
#include <immintrin.h>
#include <pthread.h>
#include <sys/inotify.h>
#include <sys/select.h>
#include <unistd.h>

#include "core/Config.h"
#include "core/Logger.h"
#include "core/LockFreeQueue.h"
#include "core/PrecisionClock.h"
#include "network/BinanceMarket.h"
#include "network/BinanceTrade.h"
#include "engine/EngineRouter.h"
#include "engine/Regime.h"
#include "trade/OrderManager.h"

std::atomic<bool> g_running{true};
std::atomic<bool> g_shutting_down{false};
std::atomic<Config*> g_active_config{nullptr};

void signal_handler(int signum) {
    if (signum == SIGINT || signum == SIGTERM) {
        if (!g_shutting_down.load(std::memory_order_relaxed)) {
            // 첫 번째 시그널: 셧다운 플래그 ON
            g_shutting_down.store(true, std::memory_order_relaxed);
        } else {
            // 두 번 연속 시그널 강제 종료
            g_running = false;
        }
    }
}

void pin_thread_to_core(int core_id) {
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    CPU_SET(core_id, &cpuset);
    pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
}

int main(int argc, char* argv[]) {
    
    PrecisionClock::Calibrate();

    // 출력 버퍼 비우기
    setvbuf(stdout, NULL, _IONBF, 0); 
    
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    Config* initial_config = new Config();
    if (!initial_config->load("config.json")) return 1;
    g_active_config.store(initial_config, std::memory_order_release);

    std::string symbol = (argc > 1) ? argv[1] : initial_config->exchange.symbol;
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c){ return std::tolower(c); });
    
    LOG(">>> [system] Booting DeltaBot (Taskset Mode) | Target: %s", symbol.c_str());
    initial_config->print_summary();

    LockFreeQueue<TickData> tick_queue;
    LockFreeQueue<OrderUpdate> order_queue;

    BinanceMarket market_ws(&tick_queue, symbol);
    BinanceTrade trade_ws(*initial_config, &order_queue, symbol);
    trade_ws.auto_detect_precision(symbol.c_str(), initial_config->exchange.price_precision, initial_config->exchange.qty_precision);
    
    // 레짐 판독기
    RegimeDetector regime_detector(initial_config->regime); 
    
    // 엔진 라우터
    EngineRouter engine_router(*initial_config);
    
    OrderManager order_manager(&trade_ws, symbol, 
        {initial_config->risk.qty, initial_config->strategy.choppy.stop_loss, initial_config->risk.trail_start, initial_config->risk.trail_drop});

    // 네트워크 연결
    if (!market_ws.connect()) { LOG(">>> [error] Market WS Failed"); return 1; }
    if (!trade_ws.connect()) { LOG(">>> [error] Trade WS Failed"); return 1; }

    LOG(">>> [system] Network Online. Spawning Threads...");
    
    // 0번코어 핫스왑 감시
    std::thread c_thread([&]() {
        pin_thread_to_core(0); // OS 및 시스템 I/O 전담 코어
        int fd = inotify_init();
        if (fd < 0) { LOG(">>> [Config] inotify_init failed!"); return; }
        
        // 디렉토리 자체를 감시하여 vim/nano의 파일 교체(Rename/Move)에 완벽 대응
        int wd = inotify_add_watch(fd, ".", IN_CLOSE_WRITE | IN_MOVED_TO);
        char buffer[1024];

        LOG(">>> [System] Hot-Swap Watchdog Armed on Core 0");

        while (g_running) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(fd, &set);
            struct timeval timeout = {1, 0}; // 1초 타임아웃 (우아한 종료를 위함)
            
            // 파일 변경 이벤트 블로킹 대기 (CPU 점유율 0%)
            int ret = select(fd + 1, &set, NULL, NULL, &timeout);
            if (ret > 0 && FD_ISSET(fd, &set)) {
                int length = read(fd, buffer, 1024);
                if (length < 0) continue;

                bool config_changed = false;
                for (int i = 0; i < length; ) {
                    struct inotify_event* event = (struct inotify_event*)&buffer[i];
                    if (event->len > 0 && std::string(event->name) == "config.json") {
                        config_changed = true;
                    }
                    i += sizeof(struct inotify_event) + event->len;
                }

                if (config_changed) {
                    LOG(">>> [Config] config.json change detected! Hot-Swapping...");
                    std::this_thread::sleep_for(std::chrono::milliseconds(200)); // 파일 쓰기 완료 대기

                    Config* new_config = new Config();
                    if (new_config->load("config.json")) {
                        // ★ [핵심] 단 한 번의 어셈블리 명령(lock cmpxchg)으로 포인터 스왑!
                        Config* old_config = g_active_config.exchange(new_config, std::memory_order_release);
                        new_config->print_summary();
                        
                        // Poor man's RCU: 메인 루프가 예전 포인터 사용을 마칠 때까지 1초 대기 후 메모리 해제
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                        delete old_config;
                        LOG(">>> [Config] Hot-Swap Complete. Old config memory reclaimed.");
                    } else {
                        LOG(">>> [Config] Failed to parse new config! Reverting to old config.");
                        delete new_config;
                    }
                }
            }
        }
        close(fd);
    });

    // 스레드 생성 (1, 2, 3번 코어)
    std::thread m_thread([&]() {
        market_ws.subscribe();
        pin_thread_to_core(1);
        while (g_running) {
            market_ws.poll();
            _mm_pause();
        }
    });

    std::thread t_thread([&]() {
        pin_thread_to_core(2);
        while (g_running) {
            trade_ws.poll();
            _mm_pause();
        }
    });

    pin_thread_to_core(3);
    
    LOG(">>> [system] Engine Live. Monitoring Cores 1");

    // Main Engine Loop
    TickData tick;
    OrderUpdate order_up;
    
    double last_signal_time = 0.0;
    double min_price_1m = 999999.0;
    double max_price_1m = 0.0;
    double taker_buy_vol_1m = 0.0;
    double taker_sell_vol_1m = 0.0;
    double last_reset_time = 0.0;
    double circuit_breaker_until = 0.0;
    bool engine_armed = false;
    
    // 글로벌 킬스위치 변수
    double global_ref_price = 0.0;
    double global_ref_time = 0.0;
    
    // 셧다운 관리 변수
    bool is_shutting_down = false;
    double shutdown_start_time = 0.0;
    
    TickData last_known_tick;
    memset(&last_known_tick, 0, sizeof(TickData));
    
    // 레짐 판독 UNKNOWN으로 시작
    MarketRegime last_regime = MarketRegime::UNKNOWN;

    LOG(">>> [system] Fetching current position from exchange...");
    trade_ws.cancel_all_orders(symbol.c_str());
    PrecisionClock::WaitUntil(PrecisionClock::Now() + 0.5);
    trade_ws.sync_position_task();

    LOG(">>> [system] Engine Loop Started. Waiting for data...");

    while (g_running) {
        bool busy = false;
        double now = PrecisionClock::Now();
        Config* current_config = g_active_config.load(std::memory_order_acquire);
        
        // 셧다운 모드 진입 감지
        if (g_shutting_down.load(std::memory_order_acquire) && !is_shutting_down) {
            is_shutting_down = true;
            shutdown_start_time = now;
            LOG(">>> [System] SIGTERM/SIGINT Received! Initiating Graceful Shutdown...");
            engine_armed = false; // 신규 진입 차단
        }
        
        // 시세 처리
        int tick_count = 0;
        while (tick_count < 100 && tick_queue.pop(tick)) {
            tick_count++;
            last_known_tick = tick;
            
            now = PrecisionClock::Now();
            if (last_reset_time == 0.0) last_reset_time = now;

            // [글로벌 킬스위치]
            if (global_ref_price == 0.0 || (now - global_ref_time >= current_config->risk.global_kill_window)) { 
                global_ref_price = tick.price;
                global_ref_time = now;
            }
            if (std::abs(tick.price - global_ref_price) / global_ref_price * 100.0 >= current_config->risk.global_kill_pct) {
                LOG(">>> [FATAL] %.1f%% PRICE SHOCK DETECTED! GLOBAL KILL SWITCH ACTIVATED!", current_config->risk.global_kill_pct);
                order_manager.force_exit("GLOBAL_KILL_SWITCH", now);
                circuit_breaker_until = now + current_config->risk.global_kill_cooldown; 
                engine_armed = false;
                global_ref_price = tick.price; 
            }
            
            // 셧다운 중 신규 진입 차단
            if (is_shutting_down) {
                engine_armed = false; 
            }
            
            // 현재 레짐(장세) 판독
            MarketRegime current_regime = regime_detector.update(tick, now);
            engine_router.switch_regime(current_regime);
            
            // TOXIC 장세 진입 시 진입 금지
            if (current_regime == MarketRegime::TOXIC && last_regime != MarketRegime::TOXIC) {
                LOG(">>> [SYSTEM] TOXIC Regime Detected! Unleashing Hawkes Engine...");
                engine_armed = true;
            }
            last_regime = current_regime;
            
            // 1분 변동성 수집
            if (tick.price > max_price_1m) max_price_1m = tick.price;
            if (tick.price < min_price_1m) min_price_1m = tick.price;
            if (tick.volume > 0.0001) { 
                if (tick.is_buy) taker_buy_vol_1m += (tick.price * tick.volume);
                else             taker_sell_vol_1m += (tick.price * tick.volume);
            }
            
            // 1분 변동성 단기 서킷 브레이커
            if (now - last_reset_time >= current_config->regime.reset_interval_sec) { 
                double volatility_pct = (max_price_1m - min_price_1m) / min_price_1m * 100.0;
                double net_delta = taker_buy_vol_1m - taker_sell_vol_1m;
                
                if (volatility_pct >= current_config->risk.circuit_breaker_vol_pct) {
                    LOG(">>> [EMERGENCY] Market is CRAZY (Vol: %.2f%%)! Circuit Breaker Activated!", volatility_pct);
                    order_manager.force_exit("CIRCUIT_BREAKER", now); 
                    circuit_breaker_until = now + current_config->risk.circuit_breaker_cooldown;         
                    engine_armed = false;
                }
                // 최소 진입 변동성 (어설픈 장세에서는 진입 방지)
                else if (now >= circuit_breaker_until) {
                    if (volatility_pct >= 0.10 || std::abs(net_delta) >= 100000.0) engine_armed = true;
                    else engine_armed = false;
                }
                
                min_price_1m = tick.price; max_price_1m = tick.price;
                taker_buy_vol_1m = 0; taker_sell_vol_1m = 0;
                last_reset_time = now;
            }
        
            int final_signal = 0;

            if (engine_armed && now >= circuit_breaker_until) {
                // 라우터가 현재 레짐에 맞는 엔진 선택
                final_signal = engine_router.update(tick, *current_config, now);
                
                // 장세에 맞게 익절/손절 파라미터 교체
                if (current_regime == MarketRegime::CHOPPY) {
                    order_manager.update_risk_params(current_config->strategy.choppy.stop_loss, current_config->strategy.choppy.take_profit); 
                } else if (current_regime == MarketRegime::TRENDING) {
                    order_manager.update_risk_params(current_config->strategy.trending.stop_loss, current_config->strategy.trending.take_profit);
                } else if (current_regime == MarketRegime::TOXIC) {
                    order_manager.update_risk_params(current_config->strategy.toxic.stop_loss, current_config->strategy.toxic.take_profit);
                }
            } else {
                // 무장 해제 상태라도, 백그라운드 업데이트
                engine_router.update(tick, *current_config, now);
            }
            
            // 주문 관리 업데이트
            order_manager.update_tick(tick, now);
            
            if (now < circuit_breaker_until) {
                final_signal = 0; 
            }
            
            if (final_signal != 0 && engine_armed) {
                // API 제한 초과 방지
                if (now - last_signal_time < current_config->risk.order_cooldown) {
                    final_signal = 0; 
                } else {
                    if (order_manager.try_enter(final_signal, tick, now)) {
                        last_signal_time = now;
                        engine_armed = false;
                        busy = true;
                    }
                }
            }
        }
        
        // 거래소 웹소켓 멈춤 감지용 (워치독)
        if (tick_count == 0 && last_known_tick.price > 0.0001) {
            order_manager.update_tick(last_known_tick, now);
        }
        
        // 주문 응답 처리 (체결, 취소 등)
        while (order_queue.pop(order_up)) {
            now = PrecisionClock::Now();
            order_manager.on_order_update(order_up, now);
            busy = true;
        }
        
        // 셧다운 처리
        if (is_shutting_down) {
            double elapsed_sec = now - shutdown_start_time;
            double current_pos = order_manager.get_position(); 
            
            if (std::abs(current_pos) > 0.0001) { 
                if (elapsed_sec < 10.0) {
                    // [10초 이내] 지정가(Maker)로 탈출 
                    order_manager.try_instance_exit(now); 
                } else {
                    // [10초 초과] 시장가(Taker)로탈출
                    order_manager.force_maket_sell(now);
                }
            } else {
                // 포지션이 0(Flat)이 된 것을 확인하면 루프 탈출
                g_running = false;
            }
        }
        
        // 휴식
        if (!busy) {
            _mm_pause();
        }
    }

    if (c_thread.joinable()) c_thread.join();
    if (m_thread.joinable()) m_thread.join();
    if (t_thread.joinable()) t_thread.join();
    
    return 0;
}
