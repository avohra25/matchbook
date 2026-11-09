// matchbook: replay a text order script (file or stdin) through the engine.
#include "matchbook/protocol.hpp"

#include <fstream>
#include <iostream>
#include <string>

namespace {
void usage() {
    std::cerr << "usage: matchbook [--stp=none|cancel-taker|cancel-maker|cancel-both] [--echo] [--md] [script]\n"
                 "Reads commands from the script (or stdin) and prints engine events.\n"
                 "  --echo  print each command before its output\n"
                 "  --md    after each command, print the level-2 market-data updates it caused\n";
}
}  // namespace

int main(int argc, char** argv) {
    mb::StpMode stp = mb::StpMode::None;
    bool echo = false;
    bool market_data = false;
    std::string path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage();
            return 0;
        } else if (arg == "--echo") {
            echo = true;
        } else if (arg == "--md") {
            market_data = true;
        } else if (arg.rfind("--stp=", 0) == 0) {
            auto mode = mb::parse_stp_mode(arg.substr(6));
            if (!mode) {
                usage();
                return 2;
            }
            stp = *mode;
        } else if (path.empty() && arg[0] != '-') {
            path = arg;
        } else {
            usage();
            return 2;
        }
    }

    std::ifstream file;
    if (!path.empty()) {
        file.open(path);
        if (!file) {
            std::cerr << "matchbook: cannot open " << path << "\n";
            return 2;
        }
    }
    std::istream& in = path.empty() ? std::cin : file;

    mb::Engine engine(stp);
    engine.enable_market_data(market_data);
    int errors = 0;
    std::size_t line_no = 0;
    for (std::string line; std::getline(in, line);) {
        ++line_no;
        try {
            auto cmd = mb::parse_command(line);
            if (!cmd) continue;
            if (echo) std::cout << "> " << line << "\n";
            for (const auto& out : mb::run_command(engine, *cmd)) std::cout << out << "\n";
            if (market_data)
                for (const auto& u : engine.take_level_updates()) std::cout << u << "\n";
        } catch (const mb::ParseError& e) {
            ++errors;
            std::cerr << "line " << line_no << ": " << e.what() << "\n";
        }
    }
    return errors == 0 ? 0 : 1;
}
