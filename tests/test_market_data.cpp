// Incremental level-2 market data: unit cases, then a replica check. A copy of
// the book's depth built only from the update stream must match the real book
// after every command, across random streams with icebergs, stops, cancels,
// modifies and every self-trade prevention mode.
#include "matchbook/engine.hpp"

#include "harness.hpp"

#include <algorithm>
#include <map>
#include <random>

using namespace mb;

namespace {
NewOrder limit(OrderId id, TraderId trader, Side side, Price px, Qty q, Qty display = 0) {
    return NewOrder{.id = id, .trader = trader, .side = side, .type = OrderType::Limit, .price = px, .qty = q,
                    .display = display};
}
LevelUpdate bid(Price px, Qty q, std::size_t n) { return {Side::Buy, px, q, n}; }
LevelUpdate ask(Price px, Qty q, std::size_t n) { return {Side::Sell, px, q, n}; }
}  // namespace

TEST(feed_is_off_by_default) {
    Engine e;
    CHECK(!e.market_data_enabled());
    e.submit(limit(1, 1, Side::Buy, 100, 5));
    CHECK(e.take_level_updates().empty());
}

TEST(new_levels_and_trades_produce_updates) {
    Engine e;
    e.enable_market_data();
    e.submit(limit(1, 1, Side::Buy, 100, 5));
    CHECK_EQ(e.take_level_updates(), (std::vector<LevelUpdate>{bid(100, 5, 1)}));
    e.submit(limit(2, 2, Side::Buy, 100, 3));
    e.submit(limit(3, 3, Side::Sell, 102, 4));
    CHECK_EQ(e.take_level_updates(), (std::vector<LevelUpdate>{bid(100, 8, 2), ask(102, 4, 1)}));
    e.submit(limit(4, 4, Side::Sell, 100, 6));  // fills order 1, 1 lot of order 2
    CHECK_EQ(e.take_level_updates(), (std::vector<LevelUpdate>{bid(100, 2, 1)}));
    e.cancel(2);
    auto gone = e.take_level_updates();
    CHECK_EQ(gone, (std::vector<LevelUpdate>{bid(100, 0, 0)}));
    CHECK(gone[0].deleted());
}

TEST(sweep_reports_each_level_once_in_touch_order) {
    Engine e;
    e.enable_market_data();
    e.submit(limit(1, 1, Side::Sell, 101, 2));
    e.submit(limit(2, 1, Side::Sell, 102, 2));
    e.submit(limit(3, 1, Side::Sell, 103, 2));
    e.take_level_updates();
    e.submit(limit(4, 2, Side::Buy, 103, 5));
    CHECK_EQ(e.take_level_updates(), (std::vector<LevelUpdate>{ask(101, 0, 0), ask(102, 0, 0), ask(103, 1, 1)}));
}

TEST(unchanged_levels_are_suppressed) {
    Engine e;
    e.enable_market_data();
    e.submit(limit(1, 1, Side::Sell, 100, 6, 3));  // iceberg showing 3
    e.take_level_updates();
    // Filling exactly one slice replenishes 3 more at the same level: same
    // visible size, same order count, so nothing is published.
    e.submit(limit(2, 2, Side::Buy, 100, 3));
    CHECK(e.take_level_updates().empty());
    // A reduce that only shrinks the hidden reserve is invisible too.
    e.modify(1, 100, 1);
    auto upd = e.take_level_updates();
    CHECK_EQ(upd, (std::vector<LevelUpdate>{ask(100, 1, 1)}));
}

TEST(modify_to_new_price_moves_the_level) {
    Engine e;
    e.enable_market_data();
    e.submit(limit(1, 1, Side::Buy, 100, 5));
    e.take_level_updates();
    e.modify(1, 101, 5);
    CHECK_EQ(e.take_level_updates(), (std::vector<LevelUpdate>{bid(100, 0, 0), bid(101, 5, 1)}));
}

TEST(disabling_clears_pending_updates) {
    Engine e;
    e.enable_market_data();
    e.submit(limit(1, 1, Side::Buy, 100, 5));
    e.enable_market_data(false);
    CHECK(e.take_level_updates().empty());
}

TEST(update_text_format) {
    CHECK_EQ(to_string(bid(99, 6, 2)), std::string("L2 bid px=99 qty=6 orders=2"));
    CHECK_EQ(to_string(ask(101, 0, 0)), std::string("L2 ask px=101 deleted"));
}

TEST(replica_built_from_updates_matches_the_book) {
    const StpMode modes[] = {StpMode::None, StpMode::CancelTaker, StpMode::CancelMaker, StpMode::CancelBoth};
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        std::mt19937_64 rng(seed * 104729);
        auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
        Engine engine(modes[seed % 4]);
        engine.enable_market_data();
        std::map<Price, LevelView> replica[2];
        for (int step = 0; step < 400; ++step) {
            const OrderId id = static_cast<OrderId>(pick(1, 60));
            const int op = pick(0, 9);
            if (op < 7) {
                NewOrder o = limit(id, static_cast<TraderId>(pick(1, 4)), pick(0, 1) ? Side::Buy : Side::Sell,
                                   pick(95, 105), pick(1, 20));
                if (pick(0, 9) == 0) {
                    o.type = OrderType::Market;
                    o.tif = TimeInForce::IOC;
                } else if (pick(0, 3) == 0) {
                    o.display = pick(1, 5);
                }
                if (pick(0, 7) == 0) o.stop = pick(95, 105);
                engine.submit(o);
            } else if (op < 9) {
                engine.cancel(id);
            } else {
                engine.modify(id, pick(95, 105), pick(1, 20));
            }
            for (const LevelUpdate& u : engine.take_level_updates()) {
                auto& side = replica[u.side == Side::Buy ? 0 : 1];
                auto it = side.find(u.price);
                // Every update must say something new.
                const LevelView old = it == side.end() ? LevelView{u.price, 0, 0} : it->second;
                CHECK(!(old == LevelView{u.price, u.qty, u.orders}));
                if (u.deleted()) {
                    CHECK(it != side.end());
                    CHECK_EQ(u.orders, 0u);
                    side.erase(u.price);
                } else {
                    CHECK(u.orders > 0);
                    side[u.price] = {u.price, u.qty, u.orders};
                }
            }
            for (Side s : {Side::Buy, Side::Sell}) {
                std::vector<LevelView> want;
                for (const auto& [px, l] : replica[s == Side::Buy ? 0 : 1]) want.push_back(l);
                if (s == Side::Buy) std::reverse(want.begin(), want.end());
                CHECK_EQ(engine.book().depth(s, 1000), want);
            }
        }
    }
}
