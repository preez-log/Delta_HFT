# Delta_HFT

A low-latency crypto trading engine built in C++.

## TL;DR
* **Dev Time:** 30 days.
* **Focus:** Software-level micro-optimization (Bare-metal).
* **Result:** System executed as designed. Live trading yielded no profit due to cross-cloud network latency.

## Architecture & Optimizations
* **Hardware Profiling:** __rdtsc intrinsic for sub-nanosecond timestamping.
* **Concurrency:** Lock-free queues with alignas(64) padding to prevent L1 cache false sharing.
* **CPU Management:** Core pinning to isolate the hot path from OS context switching.
* **Math Operations:** SIMD AVX2 (_mm256_fmadd_pd) for vectorized signal processing.
* **Memory:** Zero-allocation on the hot path.

## Post-Mortem
The engine was tested in the Binance ETH Futures market.
* **Setup:** Engine deployed on GCP Tokyo (asia-northeast1); Binance matching engine located in AWS Tokyo (ap-northeast-1).
* **Latency:** **~5ms** cross-cloud network latency observed.
* **Competitors:** Institutional market makers utilize AWS Colocation (same Availability Zone) with <0.1ms latency.
* **Conclusion:** Software-level nanosecond optimizations cannot overcome the **3ms** physical network distance between data centers and the structural disadvantage of retail taker fees.

Open-sourced for architectural reference.



## Dependencies
* C++20 Compiler (GCC/Clang)
* [simdjson](https://github.com/simdjson/simdjson) (For AVX2 accelerated JSON parsing)

## Build
No CMake Just the raw compiler command:
```bash
g++ -O3 -march=native -std=c++17 -pthread main.cpp simdjson.cpp -o delta_hft -lssl -lcrypto
