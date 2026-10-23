#include "matchbook/engine.hpp"

#include <algorithm>
#include <sstream>

namespace mb {

std::string to_string(RejectReason r) {
    switch (r) {
        case RejectReason::DuplicateId: return "duplicate-id";
        case RejectReason::InvalidQuantity: return "invalid-quantity";
        case RejectReason::InvalidPrice: return "invalid-price";
        case RejectReason::InvalidCombination: return "invalid-combination";
        case RejectReason::PostOnlyWouldCross: return "post-only-would-cross";
        case RejectReason::UnknownOrder: return "unknown-order";
    }
    return "?";
}

std::string to_string(CancelReason r) {
    switch (r) {
        case CancelReason::Requested: return "requested";
        case CancelReason::Unfilled: return "unfilled";
        case CancelReason::SelfTrade: return "self-trade";
    }
    return "?";
}

namespace {
template <class... Fs>
struct overloaded : Fs... {
    using Fs::operator()...;
};
}  // namespace

std::string to_string(const Event& e) {
    std::ostringstream os;
    std::visit(overloaded{
                   [&](const ev::Accepted& a) { os << "ACCEPTED id=" << a.id; },
                   [&](const ev::Rejected& r) { os << "REJECTED id=" << r.id << " reason=" << to_string(r.reason); },
                   [&](const ev::Trade& t) {
                       os << "TRADE taker=" << t.taker << " maker=" << t.maker << " side=" << t.taker_side
                          << " px=" << t.price << " qty=" << t.qty;
                   },
                   [&](const ev::Rested& r) {
                       os << "RESTED id=" << r.id << " side=" << r.side << " px=" << r.price << " qty=" << r.qty;
                   },
                   [&](const ev::Cancelled& c) {
                       os << "CANCELLED id=" << c.id << " remaining=" << c.remaining
                          << " reason=" << to_string(c.reason);
                   },
                   [&](const ev::Modified& m) {
                       os << "MODIFIED id=" << m.id << " px=" << m.price << " qty=" << m.qty
                          << (m.kept_priority ? " priority=kept" : " priority=lost");
                   },
                   [&](const ev::StopParked& s) { os << "STOP_PARKED id=" << s.id << " stop=" << s.stop; },
                   [&](const ev::StopTriggered& s) { os << "STOP_TRIGGERED id=" << s.id << " last=" << s.last; },
               },
               e);
    return os.str();
}

std::ostream& operator<<(std::ostream& os, const Event& e) { return os << to_string(e); }

namespace {
bool crosses(Side taker, Price limit, Price maker_price) {
    return taker == Side::Buy ? maker_price <= limit : maker_price >= limit;
}
bool stop_hit(Side side, Price stop, Price last) {
    return side == Side::Buy ? last >= stop : last <= stop;
}
}  // namespace

bool Engine::would_cross(Side side, Price limit) const {
    auto best = book_.best(opposite(side));
    return best && crosses(side, limit, *best);
}

bool Engine::fok_fillable(const NewOrder& o) const {
    Qty available = 0;
    book_.walk(opposite(o.side), [&](const Resting& r) {
        if (o.type == OrderType::Limit && !crosses(o.side, o.price, r.price)) return false;
        if (stp_ != StpMode::None && r.trader == o.trader) {
            // CancelMaker skips the order; the other modes end matching here.
            return stp_ == StpMode::CancelMaker;
        }
        available += r.remaining;
        return available < o.qty;
    });
    return available >= o.qty;
}

std::vector<Event> Engine::submit(const NewOrder& o) {
    out_.clear();
    auto reject = [&](RejectReason why) {
        out_.push_back(ev::Rejected{o.id, why});
        return std::move(out_);
    };
    if (live(o.id)) return reject(RejectReason::DuplicateId);
    if (o.qty <= 0) return reject(RejectReason::InvalidQuantity);
    if (o.type == OrderType::Limit && o.price <= 0) return reject(RejectReason::InvalidPrice);
    if (o.stop && *o.stop <= 0) return reject(RejectReason::InvalidPrice);
    if (o.post_only && (o.type == OrderType::Market || o.tif != TimeInForce::GTC || o.stop))
        return reject(RejectReason::InvalidCombination);
    if (o.post_only && would_cross(o.side, o.price)) return reject(RejectReason::PostOnlyWouldCross);

    out_.push_back(ev::Accepted{o.id});
    const std::uint64_t seq = ++seq_;
    if (o.stop && !(last_ && stop_hit(o.side, *o.stop, *last_))) {
        auto& stops = o.side == Side::Buy ? buy_stops_ : sell_stops_;
        StopKey key{*o.stop, seq};
        stops.emplace(key, o);
        stop_index_.emplace(o.id, StopLoc{o.side, key});
        out_.push_back(ev::StopParked{o.id, *o.stop});
        return std::move(out_);
    }
    if (o.stop) out_.push_back(ev::StopTriggered{o.id, *last_});
    execute(o, seq);
    trigger_stops();
    return std::move(out_);
}

void Engine::execute(const NewOrder& o, std::uint64_t seq) {
    if (o.tif == TimeInForce::FOK && !fok_fillable(o)) {
        out_.push_back(ev::Cancelled{o.id, o.qty, CancelReason::Unfilled});
        return;
    }
    Qty remaining = o.qty;
    bool self_trade_kill = false;
    const Side maker_side = opposite(o.side);

    while (remaining > 0) {
        const Resting* maker = book_.front(maker_side);
        if (maker == nullptr) break;
        if (o.type == OrderType::Limit && !crosses(o.side, o.price, maker->price)) break;

        if (stp_ != StpMode::None && maker->trader == o.trader) {
            if (stp_ == StpMode::CancelMaker || stp_ == StpMode::CancelBoth) {
                out_.push_back(ev::Cancelled{maker->id, maker->remaining, CancelReason::SelfTrade});
                book_.erase(maker->id);
            }
            if (stp_ == StpMode::CancelTaker || stp_ == StpMode::CancelBoth) {
                self_trade_kill = true;
                break;
            }
            continue;
        }

        const Qty fill = std::min(remaining, maker->remaining);
        const Resting m = *maker;  // copy: erase below invalidates the pointer
        out_.push_back(ev::Trade{o.id, m.id, o.trader, m.trader, o.side, m.price, fill});
        last_ = m.price;
        remaining -= fill;
        if (fill == m.remaining) book_.erase(m.id);
        else book_.set_remaining(m.id, m.remaining - fill);
    }

    if (remaining == 0) return;
    if (self_trade_kill) {
        out_.push_back(ev::Cancelled{o.id, remaining, CancelReason::SelfTrade});
    } else if (o.type == OrderType::Limit && o.tif == TimeInForce::GTC) {
        book_.insert(Resting{o.id, o.trader, o.side, o.price, remaining, seq});
        out_.push_back(ev::Rested{o.id, o.side, o.price, remaining});
    } else {
        out_.push_back(ev::Cancelled{o.id, remaining, CancelReason::Unfilled});
    }
}

void Engine::trigger_stops() {
    // Each triggered stop can trade and move the last price, which can trigger
    // more stops, so keep going until nothing fires. When stops on both sides
    // are eligible, the one that was submitted first goes first.
    while (last_) {
        const Price last = *last_;
        const bool buy_ready = !buy_stops_.empty() && buy_stops_.begin()->first.first <= last;
        const bool sell_ready = !sell_stops_.empty() && sell_stops_.rbegin()->first.first >= last;
        if (!buy_ready && !sell_ready) return;

        bool take_buy = buy_ready;
        if (buy_ready && sell_ready)
            take_buy = buy_stops_.begin()->first.second < sell_stops_.rbegin()->first.second;

        NewOrder order;
        if (take_buy) {
            auto it = buy_stops_.begin();
            order = it->second;
            buy_stops_.erase(it);
        } else {
            auto it = std::prev(sell_stops_.end());
            order = it->second;
            sell_stops_.erase(it);
        }
        stop_index_.erase(order.id);
        out_.push_back(ev::StopTriggered{order.id, last});
        execute(order, ++seq_);  // time priority starts at activation
    }
}

std::vector<Event> Engine::cancel(OrderId id) {
    out_.clear();
    if (const Resting* r = book_.find(id)) {
        out_.push_back(ev::Cancelled{id, r->remaining, CancelReason::Requested});
        book_.erase(id);
    } else if (auto it = stop_index_.find(id); it != stop_index_.end()) {
        auto& stops = it->second.side == Side::Buy ? buy_stops_ : sell_stops_;
        auto node = stops.find(it->second.key);
        out_.push_back(ev::Cancelled{id, node->second.qty, CancelReason::Requested});
        stops.erase(node);
        stop_index_.erase(it);
    } else {
        out_.push_back(ev::Rejected{id, RejectReason::UnknownOrder});
    }
    return std::move(out_);
}

std::vector<Event> Engine::modify(OrderId id, Price new_price, Qty new_qty) {
    out_.clear();
    const Resting* found = book_.find(id);
    if (found == nullptr) {
        out_.push_back(ev::Rejected{id, RejectReason::UnknownOrder});
        return std::move(out_);
    }
    if (new_qty <= 0 || new_price <= 0) {
        out_.push_back(ev::Rejected{id, new_qty <= 0 ? RejectReason::InvalidQuantity : RejectReason::InvalidPrice});
        return std::move(out_);
    }
    const Resting r = *found;
    if (new_price == r.price && new_qty <= r.remaining) {
        book_.set_remaining(id, new_qty);
        out_.push_back(ev::Modified{id, new_price, new_qty, true});
        return std::move(out_);
    }
    book_.erase(id);
    out_.push_back(ev::Modified{id, new_price, new_qty, false});
    NewOrder replacement{.id = id, .trader = r.trader, .side = r.side, .type = OrderType::Limit,
                         .price = new_price, .qty = new_qty};
    execute(replacement, ++seq_);
    trigger_stops();
    return std::move(out_);
}

}  // namespace mb
