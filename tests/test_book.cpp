#include "matchbook/book.hpp"

#include "harness.hpp"

using namespace mb;

namespace {
Resting bid(OrderId id, Price px, Qty q, std::uint64_t seq = 0) { return {id, 1, Side::Buy, px, q, seq}; }
Resting ask(OrderId id, Price px, Qty q, std::uint64_t seq = 0) { return {id, 1, Side::Sell, px, q, seq}; }
}  // namespace

TEST(empty_book_has_no_prices) {
    Book b;
    CHECK(b.empty());
    CHECK_EQ(b.best_bid(), std::optional<Price>{});
    CHECK_EQ(b.best_ask(), std::optional<Price>{});
    CHECK(b.front(Side::Buy) == nullptr);
    CHECK(b.depth(Side::Sell, 5).empty());
}

TEST(best_prices_track_highest_bid_and_lowest_ask) {
    Book b;
    b.insert(bid(1, 99, 10));
    b.insert(bid(2, 101, 5));
    b.insert(bid(3, 100, 7));
    b.insert(ask(4, 105, 3));
    b.insert(ask(5, 103, 4));
    CHECK_EQ(b.best_bid(), std::optional<Price>{101});
    CHECK_EQ(b.best_ask(), std::optional<Price>{103});
    CHECK_EQ(b.front(Side::Buy)->id, 2u);
    CHECK_EQ(b.front(Side::Sell)->id, 5u);
    CHECK_EQ(b.level_count(Side::Buy), 3u);
}

TEST(same_price_orders_queue_fifo) {
    Book b;
    b.insert(bid(1, 100, 10));
    b.insert(bid(2, 100, 20));
    b.insert(bid(3, 100, 30));
    CHECK_EQ(b.front(Side::Buy)->id, 1u);
    CHECK(b.erase(1));
    CHECK_EQ(b.front(Side::Buy)->id, 2u);
    CHECK_EQ(b.volume_at(Side::Buy, 100), 50);
}

TEST(depth_aggregates_levels_from_best_outward) {
    Book b;
    b.insert(ask(1, 102, 5));
    b.insert(ask(2, 101, 1));
    b.insert(ask(3, 101, 2));
    b.insert(ask(4, 104, 9));
    auto d = b.depth(Side::Sell, 2);
    CHECK_EQ(d.size(), 2u);
    CHECK_EQ(d[0], (LevelView{101, 3, 2}));
    CHECK_EQ(d[1], (LevelView{102, 5, 1}));
    auto bids = b.depth(Side::Buy, 10);
    CHECK(bids.empty());
}

TEST(erase_removes_empty_levels_and_unknown_ids_return_false) {
    Book b;
    b.insert(bid(1, 100, 10));
    CHECK(b.erase(1));
    CHECK(!b.erase(1));
    CHECK(!b.erase(42));
    CHECK_EQ(b.level_count(Side::Buy), 0u);
    CHECK(b.empty());
}

TEST(set_remaining_keeps_queue_position_and_updates_totals) {
    Book b;
    b.insert(bid(1, 100, 10));
    b.insert(bid(2, 100, 10));
    b.set_remaining(1, 4);
    CHECK_EQ(b.front(Side::Buy)->id, 1u);
    CHECK_EQ(b.find(1)->remaining, 4);
    CHECK_EQ(b.volume_at(Side::Buy, 100), 14);
    CHECK_THROWS(b.set_remaining(1, 0));
    CHECK_THROWS(b.set_remaining(99, 5));
}

TEST(insert_rejects_duplicates_and_empty_orders) {
    Book b;
    b.insert(bid(1, 100, 10));
    CHECK_THROWS(b.insert(ask(1, 105, 1)));
    CHECK_THROWS(b.insert(ask(2, 105, 0)));
    CHECK_EQ(b.order_count(), 1u);
}

TEST(walk_visits_in_price_time_priority_and_can_stop_early) {
    Book b;
    b.insert(bid(1, 100, 1));
    b.insert(bid(2, 101, 1));
    b.insert(bid(3, 100, 1));
    b.insert(bid(4, 99, 1));
    std::vector<OrderId> seen;
    b.walk(Side::Buy, [&](const Resting& r) { seen.push_back(r.id); return true; });
    CHECK_EQ(seen, (std::vector<OrderId>{2, 1, 3, 4}));
    seen.clear();
    b.walk(Side::Buy, [&](const Resting& r) { seen.push_back(r.id); return seen.size() < 2; });
    CHECK_EQ(seen, (std::vector<OrderId>{2, 1}));
}
