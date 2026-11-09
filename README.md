# matchbook

A single-instrument **limit order book and matching engine** in C++20. It implements the rules a real exchange matcher has to get right (price-time priority, the common order types, stop orders, cancel/replace and self-trade prevention) as a small, deterministic, dependency-free library with a replay CLI and a benchmark.

The engine is checked by a differential fuzz test: every command in 300 random streams is run through both the engine and a deliberately naive reference book, and their events and full market depth must match exactly after every step.

## Features

- **Price-time priority**: best price first, then first-come-first-served within a price. Trades always print at the resting (maker) order's price.
- **Order types**: limit and market, with time-in-force `GTC`, `IOC` and `FOK` (all-or-nothing, pre-checked so it never partially fills).
- **Post-only** orders, rejected instead of taking liquidity.
- **Stop and stop-limit orders** that wait off-book until the last trade price reaches the stop, including cascades where one triggered stop fires the next.
- **Cancel/replace (`modify`)**: reducing size at the same price keeps queue position; changing the price or increasing size re-queues the order and can trade immediately.
- **Self-trade prevention** with `cancel-taker`, `cancel-maker` or `cancel-both` modes, which FOK checks also respect.
- **Event stream** output: every call returns `Accepted`, `Rejected`, `Trade`, `Rested`, `Cancelled`, `Modified`, `StopParked` and `StopTriggered` events in order.
- Integer tick prices throughout, so there's no floating point in the matching path.

## Build and test

Requires CMake 3.20+ and a C++20 compiler (GCC 12+ or Clang 15+). There are no other dependencies.

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure

# with AddressSanitizer + UBSan
cmake -S . -B build-asan -DMATCHBOOK_SANITIZE=ON && cmake --build build-asan -j && ctest --test-dir build-asan
```

## The CLI

`matchbook` replays a text script (or stdin) and prints the engine's events.

```
buy|sell <id> <qty> <price|market> [gtc|ioc|fok] [post] [stop=<px>] [trader=<n>]
cancel <id>
modify <id> <price> <qty>
book [levels]
```

```sh
$ ./build/matchbook --echo examples/session.txt
...
> buy  10 3 market stop=102 trader=9   # parks until something trades at >= 102
ACCEPTED id=10
STOP_PARKED id=10 stop=102
> buy  11 12 102 trader=6               # sweeps 101 and 2 @ 102, fires the stop
ACCEPTED id=11
TRADE taker=11 maker=1 side=buy px=101 qty=10
TRADE taker=11 maker=2 side=buy px=102 qty=2
STOP_TRIGGERED id=10 last=102
TRADE taker=10 maker=2 side=buy px=102 qty=3
> book
  ask        104  x        8  (1)
  ----------------------------------
  bid         99  x        6  (1)
  bid         98  x        4  (1)
```

Use `--stp=cancel-taker|cancel-maker|cancel-both` to turn on self-trade prevention.

## Using the library

```cpp
#include "matchbook/engine.hpp"

mb::Engine engine(mb::StpMode::CancelTaker);
engine.submit({.id = 1, .trader = 7, .side = mb::Side::Sell, .price = 101, .qty = 10});
for (const mb::Event& e : engine.submit({.id = 2, .trader = 8, .side = mb::Side::Buy,
                                         .type = mb::OrderType::Market, .qty = 4,
                                         .tif = mb::TimeInForce::IOC}))
    std::cout << e << '\n';   // ACCEPTED id=2 / TRADE taker=2 maker=1 side=buy px=101 qty=4
```

## Design notes

- **Book layout.** Each side is a `std::map<Price, Level>`, and each level is a `std::list` FIFO with a running total. An `unordered_map` from order id to list iterator makes cancel and in-place reduce O(1) after the id lookup; adding at a new price is O(log levels). The `Book` is a passive data structure with no matching logic, so it is tested on its own.
- **Determinism.** The engine has no clocks, threads or randomness. Time priority comes from a sequence counter, so a command log replays to the identical event stream. That property is what makes the replay CLI and the differential fuzzer possible.
- **Stops.** Parked stops are kept off-book, ordered by `(stop price, sequence)`. After any trade the engine repeatedly fires every stop the last price has reached (buy stops when `last >= stop`, sell stops when `last <= stop`), oldest first when both sides are eligible, until nothing more triggers. A triggered order gets a fresh sequence number, so its time priority starts when it activates.
- **FOK.** Fill-or-kill walks the opposite side without mutating it to confirm enough crossable, non-self quantity exists before matching, so it never needs to roll back a partial fill.
- **Order ids** must be unique among live orders (resting or parked). Ids of finished orders can be reused.

## Testing

| Suite | What it covers |
|---|---|
| `test_book` | levels, FIFO, depth, O(1) cancel and reduce, priority walk |
| `test_matching` | limit/market/IOC/FOK/post-only matching and validation |
| `test_advanced` | stops and cascades, cancel/replace priority rules, all STP modes |
| `test_protocol` | text protocol parsing, errors, golden session output |
| `test_fuzz` | differential fuzzing against a naive reference book (300 seeds x 400 commands), plus invariant fuzzing of stops and STP: never-crossed book, level totals, and per-order quantity conservation |

Run the whole suite under both GCC and Clang, with and without sanitizers, before changing the engine.

## Benchmark

`matchbook-bench` replays a synthetic flow (passive and aggressive limits around a drifting mid, market orders, cancels and modifies) and times each command:

```sh
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j
./build-rel/matchbook-bench --ops=1000000
```

On the 8-core Linux box this was developed on, a 1M-command run measured about 4 million commands per second, with a median latency around 150 ns and p99 under 1 µs. Your numbers will differ by machine.

## License

MIT, see [LICENSE](LICENSE).
