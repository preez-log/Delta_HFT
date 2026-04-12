#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <algorithm>
#include <iomanip>
#include <cstring>
#include "trade/Types.h" // TickData 구조체

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

// SIMD 기반 대용량 JSONL(로그) 파서 (기존과 동일 - 극강의 포인터 파싱 유지!)
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
                    char* q_ptr = p_end + 3;
                    char* q_end = strchr(q_ptr, '"');
                    tick.best_bid_qty = fast_atof(q_ptr, q_end - q_ptr);
                }

                char* a_ptr = strstr(e_ptr, "\"a\":[[\"");
                if (a_ptr) {
                    a_ptr += 7;
                    char* p_end = strchr(a_ptr, '"');
                    tick.best_ask = fast_atof(a_ptr, p_end - a_ptr);
                    char* q_ptr = p_end + 3;
                    char* q_end = strchr(q_ptr, '"');
                    tick.best_ask_qty = fast_atof(q_ptr, q_end - q_ptr);
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

    std::cout << ">>> [Backtester] Successfully loaded " << ticks.size() << " ticks in milliseconds!\n";
    return ticks;
}

// 1분(60초) 단위의 시장 데이터 블록
struct RegimeBucket {
    double price_diff_pct;
    double net_delta;
    double total_vol;
};

int main(int argc, char* argv[]) {
    std::string filename = (argc > 1) ? argv[1] : "eth_data_merge.log";
    std::cout << ">>> Booting Market Regime Profiler...\n";
    
    std::vector<TickData> history = load_history(filename);
    if (history.empty()) {
        std::cout << ">>> [Error] No data loaded.\n";
        return 1;
    }

    std::vector<RegimeBucket> buckets;
    
    double window_start = history[0].timestamp;
    double min_price = history[0].price;
    double max_price = history[0].price;
    double taker_buy = 0.0;
    double taker_sell = 0.0;

    // 1분 단위로 틱 데이터를 썰어서 통계 수집
    for (const auto& tick : history) {
        if (tick.price > max_price) max_price = tick.price;
        if (tick.price < min_price) min_price = tick.price;
        
        if (tick.volume > 0.0001) {
            if (tick.is_buy) taker_buy += (tick.price * tick.volume);
            else             taker_sell += (tick.price * tick.volume);
        }

        // 60초가 지나면 버킷에 담고 초기화
        if (tick.timestamp - window_start >= 60.0) {
            RegimeBucket b;
            b.price_diff_pct = (max_price - min_price) / min_price * 100.0;
            b.total_vol = taker_buy + taker_sell;
            b.net_delta = std::abs(taker_buy - taker_sell);
            buckets.push_back(b);

            // 다음 1분 준비
            window_start = tick.timestamp;
            min_price = tick.price;
            max_price = tick.price;
            taker_buy = 0.0;
            taker_sell = 0.0;
        }
    }

    if (buckets.empty()) return 0;

    std::cout << ">>> Analyzed " << buckets.size() << " minutes of market data.\n\n";

    // 1. 변동성(Volatility) 통계 정렬
    std::vector<double> vols;
    for (const auto& b : buckets) vols.push_back(b.price_diff_pct);
    std::sort(vols.begin(), vols.end());

    // 2. 델타(Net Delta) 통계 정렬
    std::vector<double> deltas;
    for (const auto& b : buckets) deltas.push_back(b.net_delta);
    std::sort(deltas.begin(), deltas.end());

    std::vector<double> ratios;
    for (const auto& b : buckets) {
        if (b.total_vol > 0)
            ratios.push_back(b.net_delta / b.total_vol);
    }
    std::sort(ratios.begin(), ratios.end());

    auto get_pctile = [](const std::vector<double>& arr, double pct) {
        size_t idx = (size_t)((arr.size() - 1) * (pct / 100.0));
        return arr[idx];
    };

    std::cout << "================ [ ETHUSDC 1-Min Regime Profiler ] ================\n";
    std::cout << "[ Price Volatility (trending_vol_pct) ]\n";
    printf(" - Top 50%% (Average/Choppy) : %.4f %%\n", get_pctile(vols, 50.0));
    printf(" - Top 20%% (Start Trending) : %.4f %%\n", get_pctile(vols, 80.0));
    printf(" - Top  5%% (Strong Trend)   : %.4f %%\n", get_pctile(vols, 95.0));
    printf(" - Top  1%% (Toxic/Extreme)  : %.4f %%\n", get_pctile(vols, 99.0));
    printf(" - Top 0.1%%(Madness)        : %.4f %%\n\n", get_pctile(vols, 99.9));

    std::cout << "[ Net Delta (toxic_net_delta) ]\n";
    printf(" - Top 50%% (Average/Choppy) : %.0f $\n", get_pctile(deltas, 50.0));
    printf(" - Top 20%% (Start Trending) : %.0f $\n", get_pctile(deltas, 80.0));
    printf(" - Top  5%% (Strong Trend)   : %.0f $\n", get_pctile(deltas, 95.0));
    printf(" - Top  1%% (Toxic/Extreme)  : %.0f $\n", get_pctile(deltas, 99.0));
    printf(" - Top 0.1%%(Madness)        : %.0f $\n\n", get_pctile(deltas, 99.9));
    
    printf("[ Delta Ratio (trending_delta_ratio) ]\n");
    printf(" - Top 50%% : %.4f\n", get_pctile(ratios, 50.0));
    printf(" - Top 20%% : %.4f\n", get_pctile(ratios, 80.0));
    printf(" - Top  5%% : %.4f\n", get_pctile(ratios, 95.0));
    printf(" - Top  1%% : %.4f\n", get_pctile(ratios, 99.0));
    std::cout << "===================================================================\n";

    return 0;
}