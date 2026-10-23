// Differential and invariant fuzzing.
//
// 1. A deliberately naive reference book (a flat vector scanned on every
//    match) is driven with the same random commands as the real engine, and
//    every event and the full depth must agree after each command.
// 2. Stops and self-trade prevention are fuzzed against invariants: the book
//    never crosses, level totals match their orders, and every order's
//    quantity is fully accounted for as fills, cancels, or what is still live.
#include "matchbook/engine.hpp"

#include "harness.hpp"

#include <algorithm>
#include <map>
#include <random>

using namespace mb;

namespace {

class ReferenceBook {
public:
    std::vector<Event> submit(const NewOrder& o) {
        std::vector<Event> out;
        auto it = find(o.id);
        if (it != orders_.end()) return {ev::Rejected{o.id, RejectReason::DuplicateId}};
        if (o.qty <= 0) return {ev::Rejected{o.id, RejectReason::InvalidQuantity}};
        if (o.type == OrderType::Limit && o.price <= 0) return {ev::Rejected{o.id, RejectReason::InvalidPrice}};
        if (o.post_only && (o.type == OrderType::Market || o.tif != TimeInForce::GTC))
            return {ev::Rejected{o.id, RejectReason::InvalidCombination}};
        if (o.post_only) {
            auto m = best_maker(o.side);
            if (m != orders_.end() && crosses(o, m->price)) return {ev::Rejected{o.id, RejectReason::PostOnlyWouldCross}};
        }
        out.push_back(ev::Accepted{o.id});
        execute(o, out);
        return out;
    }

    std::vector<Event> cancel(OrderId id) {
        auto it = find(id);
        if (it == orders_.end()) return {ev::Rejected{id, RejectReason::UnknownOrder}};
        std::vector<Event> out{ev::Cancelled{id, it->remaining, CancelReason::Requested}};
        orders_.erase(it);
        return out;
    }

    std::vector<Event> modify(OrderId id, Price px, Qty qty) {
        auto it = find(id);
        if (it == orders_.end()) return {ev::Rejected{id, RejectReason::UnknownOrder}};
        if (qty <= 0) return {ev::Rejected{id, RejectReason::InvalidQuantity}};
        if (px <= 0) return {ev::Rejected{id, RejectReason::InvalidPrice}};
        if (px == it->price && qty <= it->remaining) {
            it->remaining = qty;
            return {ev::Modified{id, px, qty, true}};
        }
        Resting r = *it;
        orders_.erase(it);
        std::vector<Event> out{ev::Modified{id, px, qty, false}};
        execute(NewOrder{.id = id, .trader = r.trader, .side = r.side, .price = px, .qty = qty}, out);
        return out;
    }

    std::vector<LevelView> depth(Side side) const {
        std::map<Price, LevelView> levels;
        for (const auto& r : orders_)
            if (r.side == side) {
                auto& l = levels[r.price];
                l.price = r.price;
                l.qty += r.remaining;
                ++l.orders;
            }
        std::vector<LevelView> out;
        for (auto& [p, l] : levels) out.push_back(l);
        if (side == Side::Buy) std::reverse(out.begin(), out.end());
        return out;
    }

private:
    static bool crosses(const NewOrder& o, Price maker_px) {
        if (o.type == OrderType::Market) return true;
        return o.side == Side::Buy ? maker_px <= o.price : maker_px >= o.price;
    }

    std::vector<Resting>::iterator find(OrderId id) {
        return std::find_if(orders_.begin(), orders_.end(), [&](const Resting& r) { return r.id == id; });
    }

    std::vector<Resting>::iterator best_maker(Side taker) {
        auto best = orders_.end();
        for (auto it = orders_.begin(); it != orders_.end(); ++it) {
            if (it->side == taker) continue;
            if (best == orders_.end()) { best = it; continue; }
            bool better = taker == Side::Buy ? it->price < best->price : it->price > best->price;
            if (better || (it->price == best->price && it->seq < best->seq)) best = it;
        }
        return best;
    }

    void execute(const NewOrder& o, std::vector<Event>& out) {
        const std::uint64_t seq = ++seq_;
        if (o.tif == TimeInForce::FOK) {
            Qty avail = 0;
            for (const auto& r : orders_)
                if (r.side != o.side && crosses(o, r.price)) avail += r.remaining;
            if (avail < o.qty) {
                out.push_back(ev::Cancelled{o.id, o.qty, CancelReason::Unfilled});
                return;
            }
        }
        Qty left = o.qty;
        while (left > 0) {
            auto m = best_maker(o.side);
            if (m == orders_.end() || !crosses(o, m->price)) break;
            Qty fill = std::min(left, m->remaining);
            out.push_back(ev::Trade{o.id, m->id, o.trader, m->trader, o.side, m->price, fill});
            left -= fill;
            m->remaining -= fill;
            if (m->remaining == 0) orders_.erase(m);
        }
        if (left == 0) return;
        if (o.type == OrderType::Limit && o.tif == TimeInForce::GTC) {
            orders_.push_back(Resting{o.id, o.trader, o.side, o.price, left, seq});
            out.push_back(ev::Rested{o.id, o.side, o.price, left});
        } else {
            out.push_back(ev::Cancelled{o.id, left, CancelReason::Unfilled});
        }
    }

    std::vector<Resting> orders_;
    std::uint64_t seq_ = 0;
};

NewOrder random_order(std::mt19937_64& rng, OrderId id, bool allow_stops) {
    auto pick = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(rng); };
    NewOrder o;
    o.id = id;
    o.trader = static_cast<TraderId>(pick(1, 4));
    o.side = pick(0, 1) ? Side::Buy : Side::Sell;
    o.qty = pick(0, 20) == 0 ? 0 : pick(1, 20);
    if (pick(0, 9) == 0) {
        o.type = OrderType::Market;
        o.tif = pick(0, 3) == 0 ? TimeInForce::FOK : TimeInForce::IOC;
    } else {
        o.price = pick(95, 105);
        int t = pick(0, 9);
        o.tif = t == 0 ? TimeInForce::IOC : t == 1 ? TimeInForce::FOK : TimeInForce::GTC;
        o.post_only = pick(0, 9) == 0;
    }
    if (allow_stops && pick(0, 5) == 0) {
        o.stop = pick(95, 105);
        o.post_only = false;
    }
    return o;
}

void check_book_invariants(const Book& book) {
    if (book.best_bid() && book.best_ask()) CHECK(*book.best_bid() < *book.best_ask());
    for (Side s : {Side::Buy, Side::Sell}) {
        std::size_t orders = 0;
        for (const auto& level : book.depth(s, 1000)) {
            Qty sum = 0;
            std::size_t n = 0;
            book.walk(s, [&](const Resting& r) {
                if (r.price == level.price) { sum += r.remaining; ++n; }
                return true;
            });
            CHECK_EQ(sum, level.qty);
            CHECK_EQ(n, level.orders);
            orders += n;
        }
        (void)orders;
    }
}

}  // namespace

TEST(engine_matches_reference_model_on_random_command_streams) {
    for (std::uint64_t seed = 1; seed <= 300; ++seed) {
        std::mt19937_64 rng(seed);
        Engine engine;
        ReferenceBook ref;
        for (int step = 0; step < 400; ++step) {
            int op = std::uniform_int_distribution<int>(0, 9)(rng);
            OrderId id = std::uniform_int_distribution<OrderId>(1, 80)(rng);
            std::vector<Event> got, want;
            if (op < 7) {
                NewOrder o = random_order(rng, id, false);
                got = engine.submit(o);
                want = ref.submit(o);
            } else if (op < 9) {
                got = engine.cancel(id);
                want = ref.cancel(id);
            } else {
                Price px = std::uniform_int_distribution<Price>(95, 105)(rng);
                Qty q = std::uniform_int_distribution<Qty>(0, 25)(rng);
                got = engine.modify(id, px, q);
                want = ref.modify(id, px, q);
            }
            if (got != want) {
                std::fprintf(stderr, "  seed %llu step %d diverged\n", static_cast<unsigned long long>(seed), step);
            }
            CHECK_EQ(got, want);
            CHECK_EQ(engine.book().depth(Side::Buy, 1000), ref.depth(Side::Buy));
            CHECK_EQ(engine.book().depth(Side::Sell, 1000), ref.depth(Side::Sell));
        }
    }
}

TEST(stops_and_self_trade_prevention_preserve_invariants) {
    const StpMode modes[] = {StpMode::None, StpMode::CancelTaker, StpMode::CancelMaker, StpMode::CancelBoth};
    for (std::uint64_t seed = 1; seed <= 200; ++seed) {
        std::mt19937_64 rng(seed * 7919);
        Engine engine(modes[seed % 4]);
        std::map<OrderId, Qty> submitted, filled, cancelled;
        OrderId next_id = 1;
        for (int step = 0; step < 400; ++step) {
            std::vector<Event> events;
            if (std::uniform_int_distribution<int>(0, 5)(rng) == 0 && next_id > 1) {
                events = engine.cancel(std::uniform_int_distribution<OrderId>(1, next_id - 1)(rng));
            } else {
                NewOrder o = random_order(rng, next_id++, true);
                events = engine.submit(o);
                if (!events.empty() && std::holds_alternative<ev::Accepted>(events.front())) submitted[o.id] = o.qty;
            }
            for (const auto& e : events) {
                if (auto* t = std::get_if<ev::Trade>(&e)) {
                    CHECK(t->qty > 0);
                    if (engine.stp_mode() != StpMode::None) CHECK(t->taker_trader != t->maker_trader);
                    filled[t->taker] += t->qty;
                    filled[t->maker] += t->qty;
                } else if (auto* c = std::get_if<ev::Cancelled>(&e)) {
                    cancelled[c->id] += c->remaining;
                }
            }
            check_book_invariants(engine.book());
        }
        // Conservation: every accepted unit was filled, cancelled, or is still live.
        std::size_t live_stops = 0;
        for (const auto& [id, qty] : submitted) {
            Qty live = 0;
            if (const Resting* r = engine.book().find(id)) live = r->remaining;
            Qty accounted = filled[id] + cancelled[id] + live;
            if (accounted != qty) {
                ++live_stops;  // only a still-parked stop may be unaccounted, and then wholly
                CHECK_EQ(filled[id] + cancelled[id] + live, Qty{0});
            }
        }
        CHECK_EQ(live_stops, engine.pending_stops());
    }
}
