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
               },
               e);
    return os.str();
}

std::ostream& operator<<(std::ostream& os, const Event& e) { return os << to_string(e); }

namespace {
bool crosses(Side taker, Price limit, Price maker_price) {
    return taker == Side::Buy ? maker_price <= limit : maker_price >= limit;
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
    if (o.post_only && (o.type == OrderType::Market || o.tif != TimeInForce::GTC))
        return reject(RejectReason::InvalidCombination);
    if (o.post_only && would_cross(o.side, o.price)) return reject(RejectReason::PostOnlyWouldCross);

    out_.push_back(ev::Accepted{o.id});
    execute(o, ++seq_);
    return std::move(out_);
}

void Engine::execute(const NewOrder& o, std::uint64_t seq) {
    if (o.tif == TimeInForce::FOK && !fok_fillable(o)) {
        out_.push_back(ev::Cancelled{o.id, o.qty, CancelReason::Unfilled});
        return;
    }
    Qty remaining = o.qty;
    const Side maker_side = opposite(o.side);

    while (remaining > 0) {
        const Resting* maker = book_.front(maker_side);
        if (maker == nullptr) break;
        if (o.type == OrderType::Limit && !crosses(o.side, o.price, maker->price)) break;

        const Qty fill = std::min(remaining, maker->remaining);
        const Resting m = *maker;  // copy: erase below invalidates the pointer
        out_.push_back(ev::Trade{o.id, m.id, o.trader, m.trader, o.side, m.price, fill});
        last_ = m.price;
        remaining -= fill;
        if (fill == m.remaining) book_.erase(m.id);
        else book_.set_remaining(m.id, m.remaining - fill);
    }

    if (remaining == 0) return;
    if (o.type == OrderType::Limit && o.tif == TimeInForce::GTC) {
        book_.insert(Resting{o.id, o.trader, o.side, o.price, remaining, seq});
        out_.push_back(ev::Rested{o.id, o.side, o.price, remaining});
    } else {
        out_.push_back(ev::Cancelled{o.id, remaining, CancelReason::Unfilled});
    }
}

std::vector<Event> Engine::cancel(OrderId id) {
    out_.clear();
    if (const Resting* r = book_.find(id)) {
        out_.push_back(ev::Cancelled{id, r->remaining, CancelReason::Requested});
        book_.erase(id);
    } else {
        out_.push_back(ev::Rejected{id, RejectReason::UnknownOrder});
    }
    return std::move(out_);
}

}  // namespace mb
