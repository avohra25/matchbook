#pragma once

#include <cstdint>
#include <ostream>

namespace mb {

using OrderId = std::uint64_t;
using TraderId = std::uint32_t;
/// Prices are integer ticks; the engine never touches floating point.
using Price = std::int64_t;
using Qty = std::int64_t;

enum class Side : std::uint8_t { Buy, Sell };

constexpr Side opposite(Side s) noexcept { return s == Side::Buy ? Side::Sell : Side::Buy; }

inline std::ostream& operator<<(std::ostream& os, Side s) {
    return os << (s == Side::Buy ? "buy" : "sell");
}

}  // namespace mb
