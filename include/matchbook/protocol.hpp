#pragma once

#include "matchbook/engine.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace mb {

struct CancelCmd {
    OrderId id = 0;
    bool operator==(const CancelCmd&) const = default;
};
struct ModifyCmd {
    OrderId id = 0;
    Price price = 0;
    Qty qty = 0;
    bool operator==(const ModifyCmd&) const = default;
};
struct BookCmd {
    std::size_t levels = 5;
    bool operator==(const BookCmd&) const = default;
};

using Command = std::variant<NewOrder, CancelCmd, ModifyCmd, BookCmd>;

class ParseError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Parses one line of the text protocol:
///
///   buy|sell <id> <qty> <price|market> [gtc|ioc|fok] [post] [stop=<px>] [trader=<n>]
///   cancel <id>
///   modify <id> <price> <qty>
///   book [levels]
///
/// Everything after '#' is a comment. Returns nullopt for blank lines and
/// throws ParseError for malformed input. Market orders default to IOC.
std::optional<Command> parse_command(std::string_view line);

/// Renders the top `levels` of each side, asks above bids, best prices in the middle.
std::string format_book(const Book& book, std::size_t levels);

/// Applies a command and returns the human-readable output lines.
std::vector<std::string> run_command(Engine& engine, const Command& cmd);

std::optional<StpMode> parse_stp_mode(std::string_view s);

}  // namespace mb
