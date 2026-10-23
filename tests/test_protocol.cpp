#include "matchbook/protocol.hpp"

#include "harness.hpp"

using namespace mb;

TEST(parses_limit_order_with_options) {
    auto cmd = parse_command("buy 7 25 10050 ioc trader=3   # comment");
    CHECK(cmd.has_value());
    auto& o = std::get<NewOrder>(*cmd);
    CHECK_EQ(o.id, 7u);
    CHECK_EQ(o.qty, 25);
    CHECK_EQ(o.price, 10050);
    CHECK(o.side == Side::Buy);
    CHECK(o.tif == TimeInForce::IOC);
    CHECK_EQ(o.trader, 3u);
    CHECK(!o.stop);
}

TEST(parses_market_and_stop_orders) {
    auto m = std::get<NewOrder>(*parse_command("sell 1 5 market stop=90"));
    CHECK(m.type == OrderType::Market);
    CHECK(m.tif == TimeInForce::IOC);
    CHECK_EQ(m.stop, std::optional<Price>{90});
    auto p = std::get<NewOrder>(*parse_command("\tsell 2 5 101 post"));
    CHECK(p.post_only);
}

TEST(parses_cancel_modify_and_book) {
    CHECK(std::get<CancelCmd>(*parse_command("cancel 4")) == CancelCmd{4});
    CHECK(std::get<ModifyCmd>(*parse_command("modify 4 99 10")) == (ModifyCmd{4, 99, 10}));
    CHECK(std::get<BookCmd>(*parse_command("book")) == BookCmd{5});
    CHECK(std::get<BookCmd>(*parse_command("book 2")) == BookCmd{2});
}

TEST(blank_and_comment_lines_are_skipped) {
    CHECK(!parse_command(""));
    CHECK(!parse_command("   "));
    CHECK(!parse_command("# just a comment"));
}

TEST(malformed_lines_throw_parse_error) {
    CHECK_THROWS(parse_command("buy 1 10"));
    CHECK_THROWS(parse_command("buy x 10 100"));
    CHECK_THROWS(parse_command("buy 1 10 10.5"));
    CHECK_THROWS(parse_command("sell 1 10 100 sometimes"));
    CHECK_THROWS(parse_command("cancel"));
    CHECK_THROWS(parse_command("explode 1"));
}

TEST(stp_mode_names_round_trip) {
    CHECK(parse_stp_mode("cancel-maker") == StpMode::CancelMaker);
    CHECK(!parse_stp_mode("bogus"));
}

TEST(scripted_session_produces_expected_output) {
    Engine e;
    std::vector<std::string> out;
    for (const char* line : {"sell 1 10 101 trader=1", "buy 2 3 market stop=101 trader=2", "buy 3 4 101 trader=3"})
        for (auto& l : run_command(e, *parse_command(line))) out.push_back(l);
    CHECK_EQ(out, (std::vector<std::string>{
                      "ACCEPTED id=1",
                      "RESTED id=1 side=sell px=101 qty=10",
                      "ACCEPTED id=2",
                      "STOP_PARKED id=2 stop=101",
                      "ACCEPTED id=3",
                      "TRADE taker=3 maker=1 side=buy px=101 qty=4",
                      "STOP_TRIGGERED id=2 last=101",
                      "TRADE taker=2 maker=1 side=buy px=101 qty=3",
                  }));
    auto book = run_command(e, BookCmd{1});
    CHECK_EQ(book.size(), 2u);
    CHECK(book[0].find("101") != std::string::npos);
    CHECK(book[0].find("3") != std::string::npos);
}
