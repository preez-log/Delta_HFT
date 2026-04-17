# Delta HFT — Low-Latency Trading System Reference

![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)
![Language: C++17](https://img.shields.io/badge/Language-C%2B%2B17-blue.svg)
![Platform: Linux](https://img.shields.io/badge/Platform-Linux-lightgrey.svg)

A low-latency systems engineering reference applied to cryptocurrency markets.
Demonstrates multi-regime strategy routing, lock-free config hot-swap, and
SIMD-accelerated state estimation under microsecond constraints.

Built in 30 days as an architectural exercise. Open-sourced for educational
and reference purposes.

---

## Scope & Intent

This repository is a **systems engineering reference**, not a deployable
trading strategy. Strategy parameters (thresholds, regime classifier weights,
risk limits) are intentionally omitted from this public release.

Retail HFT on public cloud infrastructure faces a fundamental physical barrier:
institutional market makers operate in colocated facilities with sub-millisecond
latency to exchange matching engines, while public cloud deployments incur
~5ms cross-datacenter latency regardless of software optimization. This
project's value lies in its architecture, not its P&L.

---

## Architecture — 4-Core Pinned Design

The entire engine runs on four dedicated CPU cores with zero mutex overhead
on the hot path.

| Core | Thread            | Role                                                       |
|------|-------------------|------------------------------------------------------------|
| 0    | Config Watchdog   | `inotify` file watch → `atomic<Config*>` swap (RCU pattern)|
| 1    | Market Thread     | Binance WebSocket `bookTicker` + `aggTrade` ingestion      |
| 2    | Trade Thread      | Order fill / cancel WebSocket ingestion                    |
| 3    | Engine Loop       | Tick → regime detection → strategy → order management      |

---

## Technical Highlights

### Concurrency

- **Lock-Free Config Hot-Swap** : `inotify` detects `config.json` changes;
  `g_active_config.exchange(new_config)` replaces the entire config in a
  single atomic operation. Full parameter tuning without bot restart — no
  mutex on the hot path (poor man's RCU).

- **Zero-Allocation Hot Path** : Order IDs generated directly into stack
  buffers via `std::to_chars`. Prefix matching uses 4-byte integer comparison
  (`0x5f746e65 == "ent_"`) instead of `strcmp`. Order response parsing via
  `simdjson` SIMD JSON parser.

- **Cache-Line Padding** : `alignas(64)` on shared atomics to prevent L1
  false sharing across cores.

### Timing & Profiling

- **Hardware Timestamping** : `__rdtsc` intrinsic for sub-nanosecond
  latency measurement, cross-calibrated against `CLOCK_MONOTONIC_RAW`.

- **Core Pinning** : Each thread bound to a dedicated physical core via
  `pthread_setaffinity_np`, isolating the hot path from OS context switching.

### Strategy Routing (Structural Reference)

Three structurally distinct engines demonstrate different mathematical
approaches to market microstructure. The regime classifier routes ticks to
the appropriate engine every 60 seconds based on volatility, taker delta,
and OU β coefficient.

- **RollingZEngine (CHOPPY regime)** — Triple-filter mean reversion: rolling
  Z-score threshold + EMA OU coefficient gate + Order Flow Imbalance
  direction confirmation. `RollingZScore` uses O(1) incremental `sum` /
  `sum_sq` updates with periodic full recalculation to reset floating-point
  drift.

- **KinematicEngine (TRENDING regime)** — Price modeled as a physical system
  `[position, velocity, acceleration]`. `PhysicsState` updated via AVX2
  `_mm256_fmadd_pd` Kalman step executed in a single 256-bit register. Jerk
  condition (rate of change of acceleration) gates entry to accelerating
  trends only.

- **HawkesEngine (TOXIC regime)** — Self-exciting Hawkes Process models
  event clustering. `hawkes_energy` increments by `alpha` per event and
  decays as `exp(-beta*dt)`. Energy threshold breach triggers OBI-directional
  entry to capture post-shock aftershocks.

### Order Management

- **6-State FSM** : `NONE → PENDING_ENTRY → LONG/SHORT → PENDING_EXIT →
  PENDING_EMERGENCY`. Maker chase, two-tier stop loss (maker chase → market
  order fallback), trailing stop.

- **Ghost Fix** : REST API resync triggered on WebSocket silence detection
  to recover from missed order events.

- **Graceful Shutdown** : First SIGINT blocks new entries and attempts maker
  exit. Market order forced after 10s timeout. Second SIGINT triggers
  immediate termination.

---

## What This Project Demonstrates

- Hardware-level profiling and timing primitives (`__rdtsc`, core pinning,
  cache-line padding)
- Lock-free concurrency across multiple threads with atomic RCU patterns
- SIMD vectorization of non-trivial math (Kalman state update in a single
  AVX2 FMA instruction)
- Zero-allocation hot paths with compile-time optimized string handling
- Multi-strategy routing driven by online market regime classification
- State machine design for asynchronous order lifecycle under network
  failures

---

## Dependencies

- C++17 or later compiler (GCC / Clang)
- [simdjson](https://github.com/simdjson/simdjson) — AVX2-accelerated JSON parser
- OpenSSL — WebSocket TLS + HMAC signing

## Build

```bash
g++ -O3 -march=native -std=c++17 -pthread main.cpp simdjson.cpp -o delta_hft -lssl -lcrypto
```

---

## License

MIT License — see [LICENSE](LICENSE) file.

---

## Related Projects

- [Delta Cast](https://github.com/preez-log/delta-cast) — Kernel-level virtual ASIO driver
- Delta Engine — Custom D3D11 game engine (USPTO Patent Pending #19/641,687)

