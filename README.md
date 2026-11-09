# matchbook

A single-instrument **limit order book and matching engine** in C++20. It implements the rules a real exchange matcher has to get right (price-time priority, the common order types, stop orders, iceberg orders, cancel/replace and self-trade prevention) as a small, deterministic, dependency-free library with an incremental market-data feed, a replay CLI and a benchmark.

The engine is checked by a differential fuzz test: every command in 300 random streams is run through both the engine and a deliberately naive reference book, and their events and full market depth must match exactly after every step.

## Features

- **Price-time priority**: best price first, then first-come-first-served within a price. Trades always print at the resting (maker) order's price.
- **Order types**: limit and market, with time-in-force `GTC`, `IOC` and `FOK` (all-or-nothing, pre-checked so it never partially fills).
- **Post-only** orders, rejected instead of taking liquidity.
- **Stop and stop-limit orders** that wait off-book until the last trade price reaches the stop, including cascades where one triggered stop fires the next.
- **Cancel/replace (`modify`)**: reducing size at the same price keeps queue position; changing the price or increasing size re-queues the order and can trade immediately.
- **Iceberg orders** (`display`): only a slice of the order is shown; each time the slice fills, the next one comes out of the hidden reserve and joins the back of the queue. The whole order still trades as a taker, and FOK checks count hidden quantity.
- **Level-2 market data**: an opt-in incremental feed of net price-level changes per command (visible quantity and order count only), suitable for keeping a downstream depth copy in sync.
- **Self-trade prevention** with `cancel-taker`, `cancel-maker` or `cancel-both` modes, which FOK checks also respect.
- **Event stream** output: every call returns `Accepted`, `Rejected`, `Trade`, `Rested`, `Cancelled`, `Modified`, `StopParked`, `StopTriggered` and `Replenished` events in order.
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
buy|sell <id> <qty> <price|market> [gtc|ioc|fok] [post] [stop=<px>] [display=<n>] [trader=<n>]
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

### Icebergs and market data

`display=<n>` makes a GTC limit order an iceberg, and `--md` prints the level-2 updates each command caused, after its events (`examples/iceberg.txt` walks through it):

```sh
$ ./build/matchbook --echo --md examples/iceberg.txt
> sell 1 30 101 display=5 trader=1     # shows 5 of 30
ACCEPTED id=1
RESTED id=1 side=sell px=101 qty=5 hidden=25
L2 ask px=101 qty=5 orders=1
> sell 2 4  101 trader=2               # plain order queued behind it
ACCEPTED id=2
RESTED id=2 side=sell px=101 qty=4
L2 ask px=101 qty=9 orders=2
...
> buy  10 7 101 trader=9               # 5 from the iceberg, which replenishes behind
ACCEPTED id=10
TRADE taker=10 maker=1 side=buy px=101 qty=5
REPLENISHED id=1 qty=5 hidden=20
TRADE taker=10 maker=2 side=buy px=101 qty=2
L2 ask px=101 qty=7 orders=2
```

A level that disappears is reported as `L2 ask px=101 deleted`. A command that leaves a level's visible size and order count where they started (for example an iceberg replenishing in place, or a reduce that only shrinks a hidden reserve) publishes nothing for it.

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

Icebergs and the market-data feed:

```cpp
engine.enable_market_data();
engine.submit({.id = 3, .trader = 9, .side = mb::Side::Buy, .price = 100, .qty = 500, .display = 50});
for (const mb::LevelUpdate& u : engine.take_level_updates())
    std::cout << u << '\n';   // L2 bid px=100 qty=50 orders=1
```

`take_level_updates()` returns net changes since the previous call, in the order the levels were first touched. Applying them in order to a copy of the depth keeps it identical to `engine.book().depth(...)`.

## Design notes

- **Book layout.** Each side is a `std::map<Price, Level>`, and each level is a `std::list` FIFO with a running total. An `unordered_map` from order id to list iterator makes cancel and in-place reduce O(1) after the id lookup; adding at a new price is O(log levels). The `Book` is a passive data structure with no matching logic, so it is tested on its own.
- **Determinism.** The engine has no clocks, threads or randomness. Time priority comes from a sequence counter, so a command log replays to the identical event stream. That property is what makes the replay CLI and the differential fuzzer possible.
- **Stops.** Parked stops are kept off-book, ordered by `(stop price, sequence)`. After any trade the engine repeatedly fires every stop the last price has reached (buy stops when `last >= stop`, sell stops when `last <= stop`), oldest first when both sides are eligible, until nothing more triggers. A triggered order gets a fresh sequence number, so its time priority starts when it activates.
- **Icebergs.** A resting iceberg keeps its visible slice in `remaining` and the reserve in `hidden`; book levels and depth only ever count the visible part. When a slice fills, the order is re-queued at the back of its level with a fresh sequence number and a new slice, which is how CME and most venues treat a refill. A taker that keeps going will meet it again only after the orders that were behind it. Cancel/replace treats the quantity as the total and takes reductions out of the hidden reserve first, so a shrink keeps queue priority.
- **Market data.** Tracking is off by default, so the matching path costs one branch when nobody listens. When on, the book records each touched level's state before its first change in the current command and diffs against it on `take_level_updates()`, so the feed reports net changes rather than every intermediate step of a sweep.
- **FOK.** Fill-or-kill walks the opposite side without mutating it to confirm enough crossable, non-self quantity exists before matching, so it never needs to roll back a partial fill.
- **Order ids** must be unique among live orders (resting or parked). Ids of finished orders can be reused.

## Testing

| Suite | What it covers |
|---|---|
| `test_book` | levels, FIFO, depth, O(1) cancel and reduce, priority walk |
| `test_matching` | limit/market/IOC/FOK/post-only matching and validation |
| `test_advanced` | stops and cascades, cancel/replace priority rules, all STP modes |
| `test_iceberg` | slices and replenishment priority, aggressive icebergs, FOK with hidden reserve, cancel/modify accounting, STP, iceberg stop-limits |
| `test_market_data` | update contents and ordering, suppression of no-op changes, and a replica rebuilt only from updates that must match the real depth after every command (200 seeds x 400 commands, all STP modes) |
| `test_protocol` | text protocol parsing, errors, golden session output |
| `test_fuzz` | differential fuzzing against a naive reference book, icebergs included (300 seeds x 400 commands), plus invariant fuzzing of stops and STP: never-crossed book, level totals, and per-order quantity conservation |

Run the whole suite under both GCC and Clang, with and without sanitizers, before changing the engine.

## Benchmark

`matchbook-bench` replays a synthetic flow (passive and aggressive limits around a drifting mid, market orders, cancels and modifies) and times each command:

```sh
cmake -S . -B build-rel -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j
./build-rel/matchbook-bench --ops=1000000
./build-rel/matchbook-bench --ops=1000000 --icebergs=20 --md   # 20% of passive orders as icebergs, feed on
```

On the 8-core Linux box this was developed on, a 1M-command run measured about 4 million commands per second, with a median latency around 150 ns and p99 under 1 µs. Your numbers will differ by machine.

## License

MIT, see [LICENSE](LICENSE).
