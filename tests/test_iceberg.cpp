#include "matchbook/engine.hpp"

#include "harness.hpp"

using namespace mb;

namespace {
NewOrder limit(OrderId id, TraderId trader, Side side, Price px, Qty q, Qty display = 0) {
    return NewOrder{.id = id, .trader = trader, .side = side, .type = OrderType::Limit, .price = px, .qty = q,
                    .display = display};
}
template <class T>
std::vector<T> only(const std::vector<Event>& events) {
    std::vector<T> out;
    for (const auto& e : events)
        if (auto* p = std::get_if<T>(&e)) out.push_back(*p);
    return out;
}
}  // namespace

TEST(iceberg_rests_with_only_its_display_visible) {
    Engine e;
    auto events = e.submit(limit(1, 1, Side::Sell, 100, 25, 10));
    CHECK_EQ(events, (std::vector<Event>{ev::Accepted{1}, ev::Rested{1, Side::Sell, 100, 10, 15}}));
    CHECK_EQ(e.book().volume_at(Side::Sell, 100), 10);
    CHECK_EQ(e.book().depth(Side::Sell, 1), (std::vector<LevelView>{{100, 10, 1}}));
    CHECK_EQ(e.book().find(1)->hidden, 15);
}

TEST(display_at_least_qty_is_a_plain_order) {
    Engine e;
    auto events = e.submit(limit(1, 1, Side::Buy, 100, 5, 8));
    CHECK_EQ(only<ev::Rested>(events), (std::vector<ev::Rested>{{1, Side::Buy, 100, 5, 0}}));
}

TEST(invalid_iceberg_combinations_are_rejected) {
    Engine e;
    CHECK_EQ(e.submit(limit(1, 1, Side::Buy, 100, 5, -1)),
             (std::vector<Event>{ev::Rejected{1, RejectReason::InvalidQuantity}}));
    NewOrder ioc = limit(2, 1, Side::Buy, 100, 5, 2);
    ioc.tif = TimeInForce::IOC;
    CHECK_EQ(e.submit(ioc), (std::vector<Event>{ev::Rejected{2, RejectReason::InvalidCombination}}));
    NewOrder mkt{.id = 3, .side = Side::Buy, .type = OrderType::Market, .qty = 5, .tif = TimeInForce::IOC, .display = 2};
    CHECK_EQ(e.submit(mkt), (std::vector<Event>{ev::Rejected{3, RejectReason::InvalidCombination}}));
}

TEST(replenished_slice_loses_time_priority) {
    Engine e;
    e.submit(limit(1, 1, Side::Sell, 100, 9, 3));  // iceberg, 3 shown
    e.submit(limit(2, 2, Side::Sell, 100, 4));     // plain order behind it
    auto events = e.submit(limit(3, 3, Side::Buy, 100, 5));
    CHECK_EQ(events, (std::vector<Event>{
                         ev::Accepted{3},
                         ev::Trade{3, 1, 3, 1, Side::Buy, 100, 3},
                         ev::Replenished{1, 3, 3},
                         ev::Trade{3, 2, 3, 2, Side::Buy, 100, 2},
                     }));
    // Order 2 is now ahead of the iceberg's new slice.
    CHECK_EQ(e.book().front(Side::Sell)->id, 2u);
    CHECK_EQ(e.book().volume_at(Side::Sell, 100), 5);
}

TEST(lone_iceberg_trades_slice_by_slice) {
    Engine e;
    e.submit(limit(1, 1, Side::Sell, 100, 10, 4));
    auto events = e.submit(limit(2, 2, Side::Buy, 100, 10));
    std::vector<Qty> fills;
    for (auto& t : only<ev::Trade>(events)) fills.push_back(t.qty);
    CHECK_EQ(fills, (std::vector<Qty>{4, 4, 2}));
    CHECK_EQ(only<ev::Replenished>(events), (std::vector<ev::Replenished>{{1, 4, 2}, {1, 2, 0}}));
    CHECK(e.book().empty());
}

TEST(aggressive_iceberg_trades_its_full_size_then_rests_a_slice) {
    Engine e;
    e.submit(limit(1, 1, Side::Sell, 100, 6));
    auto events = e.submit(limit(2, 2, Side::Buy, 100, 20, 5));
    auto trades = only<ev::Trade>(events);
    CHECK_EQ(trades.size(), 1u);
    CHECK_EQ(trades[0].qty, 6);
    CHECK_EQ(only<ev::Rested>(events), (std::vector<ev::Rested>{{2, Side::Buy, 100, 5, 9}}));
}

TEST(fok_counts_hidden_reserve_as_available) {
    Engine e;
    e.submit(limit(1, 1, Side::Sell, 100, 12, 2));
    NewOrder fok = limit(2, 2, Side::Buy, 100, 12);
    fok.tif = TimeInForce::FOK;
    auto events = e.submit(fok);
    Qty total = 0;
    for (auto& t : only<ev::Trade>(events)) total += t.qty;
    CHECK_EQ(total, 12);
    CHECK(e.book().empty());
}

TEST(cancel_reports_visible_plus_hidden) {
    Engine e;
    e.submit(limit(1, 1, Side::Buy, 100, 20, 5));
    CHECK_EQ(e.cancel(1), (std::vector<Event>{ev::Cancelled{1, 20, CancelReason::Requested}}));
}

TEST(modify_shrinks_hidden_first_and_keeps_priority) {
    Engine e;
    e.submit(limit(1, 1, Side::Buy, 100, 20, 5));
    e.submit(limit(2, 2, Side::Buy, 100, 3));
    CHECK_EQ(e.modify(1, 100, 8), (std::vector<Event>{ev::Modified{1, 100, 8, true}}));
    CHECK_EQ(e.book().find(1)->remaining, 5);
    CHECK_EQ(e.book().find(1)->hidden, 3);
    CHECK_EQ(e.book().front(Side::Buy)->id, 1u);
    e.modify(1, 100, 2);  // now the visible slice itself shrinks
    CHECK_EQ(e.book().find(1)->remaining, 2);
    CHECK_EQ(e.book().find(1)->hidden, 0);
}

TEST(repriced_iceberg_keeps_its_display_size) {
    Engine e;
    e.submit(limit(1, 1, Side::Buy, 100, 20, 5));
    auto events = e.modify(1, 101, 30);
    CHECK_EQ(only<ev::Rested>(events), (std::vector<ev::Rested>{{1, Side::Buy, 101, 5, 25}}));
}

TEST(self_trade_cancel_maker_removes_the_whole_iceberg) {
    Engine e(StpMode::CancelMaker);
    e.submit(limit(1, 7, Side::Sell, 100, 30, 5));
    e.submit(limit(2, 8, Side::Sell, 100, 4));
    auto events = e.submit(limit(3, 7, Side::Buy, 100, 4));
    CHECK_EQ(only<ev::Cancelled>(events), (std::vector<ev::Cancelled>{{1, 30, CancelReason::SelfTrade}}));
    CHECK_EQ(only<ev::Trade>(events).size(), 1u);
}

TEST(iceberg_stop_limit_rests_as_an_iceberg_when_triggered) {
    Engine e;
    e.submit(limit(1, 1, Side::Sell, 100, 1));
    NewOrder sl = limit(2, 2, Side::Buy, 99, 12, 4);
    sl.stop = 100;
    e.submit(sl);
    auto events = e.submit(limit(3, 3, Side::Buy, 100, 1));
    CHECK_EQ(only<ev::Rested>(events), (std::vector<ev::Rested>{{2, Side::Buy, 99, 4, 8}}));
}

TEST(event_text_includes_hidden_and_replenishment) {
    CHECK_EQ(to_string(Event{ev::Rested{1, Side::Sell, 100, 5, 15}}), std::string("RESTED id=1 side=sell px=100 qty=5 hidden=15"));
    CHECK_EQ(to_string(Event{ev::Rested{1, Side::Sell, 100, 5}}), std::string("RESTED id=1 side=sell px=100 qty=5"));
    CHECK_EQ(to_string(Event{ev::Replenished{1, 5, 10}}), std::string("REPLENISHED id=1 qty=5 hidden=10"));
}
