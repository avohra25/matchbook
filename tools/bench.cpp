// matchbook-bench: synthetic order flow around a drifting mid price.
// Reports throughput and per-command latency percentiles.
#include "matchbook/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>
#include <string>
#include <vector>

namespace {

struct Op {
    enum Kind { Submit, Cancel, Modify } kind;
    mb::NewOrder order;
    mb::OrderId id;
    mb::Price price;
    mb::Qty qty;
};

std::vector<Op> generate(std::size_t n, std::uint64_t seed, int iceberg_pct) {
    std::mt19937_64 rng(seed);
    auto uni = [&](long lo, long hi) { return std::uniform_int_distribution<long>(lo, hi)(rng); };
    std::vector<Op> ops;
    ops.reserve(n);
    std::vector<mb::OrderId> recent;  // candidates for cancel/modify (may already be gone)
    mb::Price mid = 10'000;
    mb::OrderId next = 1;
    for (std::size_t i = 0; i < n; ++i) {
        if (i % 64 == 0) mid += uni(-2, 2);
        long r = uni(0, 99);
        if (r < 25 && !recent.empty()) {
            std::size_t k = static_cast<std::size_t>(uni(0, static_cast<long>(recent.size()) - 1));
            ops.push_back({Op::Cancel, {}, recent[k], 0, 0});
            recent[k] = recent.back();
            recent.pop_back();
        } else if (r < 30 && !recent.empty()) {
            mb::OrderId id = recent[static_cast<std::size_t>(uni(0, static_cast<long>(recent.size()) - 1))];
            ops.push_back({Op::Modify, {}, id, mid + uni(-20, 20), uni(1, 100)});
        } else {
            mb::NewOrder o;
            o.id = next++;
            o.trader = static_cast<mb::TraderId>(uni(1, 500));
            o.side = uni(0, 1) ? mb::Side::Buy : mb::Side::Sell;
            o.qty = uni(1, 100);
            if (r < 38) {
                o.type = mb::OrderType::Market;
                o.tif = mb::TimeInForce::IOC;
            } else {
                // Mostly passive orders a few ticks behind the mid, some aggressive.
                long offset = uni(-3, 25);
                o.price = o.side == mb::Side::Buy ? mid - offset : mid + offset;
                if (uni(0, 99) < iceberg_pct) {
                    o.qty = uni(100, 1000);
                    o.display = uni(10, 50);
                }
                recent.push_back(o.id);
            }
            ops.push_back({Op::Submit, o, 0, 0, 0});
        }
    }
    return ops;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t n = 1'000'000;
    std::uint64_t seed = 42;
    int iceberg_pct = 0;
    bool market_data = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--ops=", 0) == 0) n = std::strtoull(a.c_str() + 6, nullptr, 10);
        else if (a.rfind("--seed=", 0) == 0) seed = std::strtoull(a.c_str() + 7, nullptr, 10);
        else if (a.rfind("--icebergs=", 0) == 0) iceberg_pct = std::clamp(std::atoi(a.c_str() + 11), 0, 100);
        else if (a == "--md") market_data = true;
        else {
            std::fprintf(stderr, "usage: matchbook-bench [--ops=N] [--seed=S] [--icebergs=PCT] [--md]\n");
            return 2;
        }
    }

    const auto ops = generate(n, seed, iceberg_pct);
    mb::Engine engine;
    engine.enable_market_data(market_data);
    std::size_t updates = 0;
    std::vector<std::uint32_t> latency_ns(ops.size());
    std::size_t trades = 0;
    using clock = std::chrono::steady_clock;

    const auto start = clock::now();
    for (std::size_t i = 0; i < ops.size(); ++i) {
        const Op& op = ops[i];
        const auto t0 = clock::now();
        std::vector<mb::Event> events = op.kind == Op::Submit   ? engine.submit(op.order)
                                        : op.kind == Op::Cancel ? engine.cancel(op.id)
                                                                : engine.modify(op.id, op.price, op.qty);
        if (market_data) updates += engine.take_level_updates().size();
        const auto t1 = clock::now();
        latency_ns[i] = static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        for (const auto& e : events) trades += std::holds_alternative<mb::ev::Trade>(e);
    }
    const double secs = std::chrono::duration<double>(clock::now() - start).count();

    std::sort(latency_ns.begin(), latency_ns.end());
    auto pct = [&](double p) {
        return latency_ns[std::min(latency_ns.size() - 1, static_cast<std::size_t>(p * static_cast<double>(latency_ns.size())))];
    };
    std::printf("commands       %zu\n", ops.size());
    std::printf("trades         %zu\n", trades);
    if (market_data) std::printf("L2 updates     %zu\n", updates);
    std::printf("resting orders %zu (%zu bid levels, %zu ask levels)\n", engine.book().order_count(),
                engine.book().level_count(mb::Side::Buy), engine.book().level_count(mb::Side::Sell));
    std::printf("throughput     %.2f M commands/s\n", static_cast<double>(ops.size()) / secs / 1e6);
    std::printf("latency ns     p50 %u  p99 %u  p99.9 %u  max %u\n", pct(0.50), pct(0.99), pct(0.999),
                latency_ns.back());
    return 0;
}
