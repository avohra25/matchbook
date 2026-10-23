// Minimal single-header test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }
// Each test binary includes this header exactly once; it provides main().
#pragma once

#include <cstdio>
#include <functional>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace th {

struct Case {
    const char* name;
    std::function<void()> fn;
};
inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}
struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};
struct Failure {};

template <class T>
std::string show(const T& v) {
    if constexpr (requires(std::ostream& os) { os << v; }) {
        std::ostringstream s;
        s << v;
        return s.str();
    } else if constexpr (requires { v.has_value(); *v; }) {
        return v ? "some(" + show(*v) + ")" : "none";
    } else if constexpr (requires { v.begin(); v.end(); v.size(); }) {
        std::string out = "[";
        for (const auto& x : v) out += (out.size() > 1 ? ", " : "") + show(x);
        return out + "]";
    } else {
        return "<unprintable>";
    }
}

inline void fail(const char* file, int line, const std::string& msg) {
    std::fprintf(stderr, "  %s:%d: %s\n", file, line, msg.c_str());
    throw Failure{};
}

}  // namespace th

#define TH_CAT2(a, b) a##b
#define TH_CAT(a, b) TH_CAT2(a, b)
#define TEST(name)                                                  \
    static void name();                                             \
    static const th::Registrar TH_CAT(th_reg_, name){#name, name};  \
    static void name()

#define CHECK(cond) \
    do { if (!(cond)) th::fail(__FILE__, __LINE__, "CHECK(" #cond ") failed"); } while (0)

#define CHECK_EQ(a, b)                                                                         \
    do {                                                                                       \
        const auto& th_a = (a);                                                                \
        const auto& th_b = (b);                                                                \
        if (!(th_a == th_b))                                                                   \
            th::fail(__FILE__, __LINE__,                                                       \
                     "CHECK_EQ(" #a ", " #b ")\n    left:  " + th::show(th_a) +                \
                         "\n    right: " + th::show(th_b));                                    \
    } while (0)

#define CHECK_THROWS(expr)                                                         \
    do {                                                                           \
        bool th_threw = false;                                                     \
        try { (void)(expr); } catch (...) { th_threw = true; }                     \
        if (!th_threw) th::fail(__FILE__, __LINE__, "expected exception: " #expr); \
    } while (0)

int main() {
    int failed = 0;
    for (const auto& c : th::registry()) {
        try {
            c.fn();
            std::printf("ok   %s\n", c.name);
        } catch (const th::Failure&) {
            ++failed;
            std::printf("FAIL %s\n", c.name);
        } catch (const std::exception& e) {
            ++failed;
            std::printf("FAIL %s (exception: %s)\n", c.name, e.what());
        }
    }
    std::printf("%zu tests, %d failed\n", th::registry().size(), failed);
    return failed == 0 ? 0 : 1;
}
