#include "matchbook/protocol.hpp"

#include <charconv>
#include <iomanip>
#include <sstream>

namespace mb {

namespace {

std::vector<std::string_view> split(std::string_view line) {
    std::vector<std::string_view> out;
    std::size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
        std::size_t j = i;
        while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
        if (j > i) out.push_back(line.substr(i, j - i));
        i = j;
    }
    return out;
}

template <class T>
T number(std::string_view token, std::string_view what) {
    T value{};
    auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), value);
    if (ec != std::errc{} || ptr != token.data() + token.size())
        throw ParseError("invalid " + std::string(what) + " '" + std::string(token) + "'");
    return value;
}

void expect_args(const std::vector<std::string_view>& t, std::size_t min, std::size_t max, const char* usage) {
    if (t.size() < min || t.size() > max) throw ParseError(std::string("usage: ") + usage);
}

NewOrder parse_order(const std::vector<std::string_view>& t) {
    constexpr const char* usage =
        "buy|sell <id> <qty> <price|market> [gtc|ioc|fok] [post] [stop=<px>] [trader=<n>]";
    expect_args(t, 4, 8, usage);
    NewOrder o;
    o.side = t[0] == "buy" ? Side::Buy : Side::Sell;
    o.id = number<OrderId>(t[1], "order id");
    o.qty = number<Qty>(t[2], "quantity");
    if (t[3] == "market" || t[3] == "mkt") {
        o.type = OrderType::Market;
        o.tif = TimeInForce::IOC;
    } else {
        o.price = number<Price>(t[3], "price");
    }
    for (std::size_t i = 4; i < t.size(); ++i) {
        std::string_view opt = t[i];
        if (opt == "gtc") o.tif = TimeInForce::GTC;
        else if (opt == "ioc") o.tif = TimeInForce::IOC;
        else if (opt == "fok") o.tif = TimeInForce::FOK;
        else if (opt == "post") o.post_only = true;
        else if (opt.starts_with("stop=")) o.stop = number<Price>(opt.substr(5), "stop price");
        else if (opt.starts_with("trader=")) o.trader = number<TraderId>(opt.substr(7), "trader id");
        else throw ParseError("unknown option '" + std::string(opt) + "'");
    }
    return o;
}

}  // namespace

std::optional<Command> parse_command(std::string_view line) {
    if (auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    auto t = split(line);
    if (t.empty()) return std::nullopt;
    const std::string_view verb = t[0];
    if (verb == "buy" || verb == "sell") return parse_order(t);
    if (verb == "cancel") {
        expect_args(t, 2, 2, "cancel <id>");
        return CancelCmd{number<OrderId>(t[1], "order id")};
    }
    if (verb == "modify") {
        expect_args(t, 4, 4, "modify <id> <price> <qty>");
        return ModifyCmd{number<OrderId>(t[1], "order id"), number<Price>(t[2], "price"), number<Qty>(t[3], "quantity")};
    }
    if (verb == "book") {
        expect_args(t, 1, 2, "book [levels]");
        return BookCmd{t.size() == 2 ? number<std::size_t>(t[1], "level count") : 5};
    }
    throw ParseError("unknown command '" + std::string(verb) + "'");
}

std::string format_book(const Book& book, std::size_t levels) {
    std::ostringstream os;
    auto row = [&](const char* label, const LevelView& l) {
        os << label << std::setw(10) << l.price << "  x " << std::setw(8) << l.qty << "  (" << l.orders << ")\n";
    };
    auto asks = book.depth(Side::Sell, levels);
    for (auto it = asks.rbegin(); it != asks.rend(); ++it) row("  ask ", *it);
    os << "  ----------------------------------\n";
    for (const auto& l : book.depth(Side::Buy, levels)) row("  bid ", l);
    return os.str();
}

std::vector<std::string> run_command(Engine& engine, const Command& cmd) {
    std::vector<Event> events;
    if (auto* o = std::get_if<NewOrder>(&cmd)) events = engine.submit(*o);
    else if (auto* c = std::get_if<CancelCmd>(&cmd)) events = engine.cancel(c->id);
    else if (auto* m = std::get_if<ModifyCmd>(&cmd)) events = engine.modify(m->id, m->price, m->qty);
    else {
        std::vector<std::string> lines;
        std::istringstream in(format_book(engine.book(), std::get<BookCmd>(cmd).levels));
        for (std::string l; std::getline(in, l);) lines.push_back(l);
        return lines;
    }
    std::vector<std::string> lines;
    lines.reserve(events.size());
    for (const auto& e : events) lines.push_back(to_string(e));
    return lines;
}

std::optional<StpMode> parse_stp_mode(std::string_view s) {
    if (s == "none") return StpMode::None;
    if (s == "cancel-taker") return StpMode::CancelTaker;
    if (s == "cancel-maker") return StpMode::CancelMaker;
    if (s == "cancel-both") return StpMode::CancelBoth;
    return std::nullopt;
}

}  // namespace mb
