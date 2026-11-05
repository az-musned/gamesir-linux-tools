// Minimal test harness (no external dependency).
#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace t {

struct Case { const char* name; std::function<void()> fn; };
inline std::vector<Case>& registry() { static std::vector<Case> r; return r; }
inline int& failures() { static int f = 0; return f; }
inline bool& caseFailed() { static bool f = false; return f; }

struct Reg { Reg(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };

inline void fail(const char* file, int line, const std::string& msg) {
    std::printf("    FAIL %s:%d: %s\n", file, line, msg.c_str());
    failures()++;
    caseFailed() = true;
}

}  // namespace t

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)
#define TEST(name)                                                              \
    static void TEST_CAT(test_fn_, __LINE__)();                                 \
    static t::Reg TEST_CAT(test_reg_, __LINE__)(name, TEST_CAT(test_fn_, __LINE__)); \
    static void TEST_CAT(test_fn_, __LINE__)()

#define CHECK(c) do { if (!(c)) t::fail(__FILE__, __LINE__, "CHECK(" #c ")"); } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (!(_a == _b)) \
    t::fail(__FILE__, __LINE__, std::string(#a " == " #b "  (") + std::to_string((long long)_a) + " vs " + std::to_string((long long)_b) + ")"); } while (0)
#define CHECK_NEAR(a, b, tol) do { double _a = (double)(a), _b = (double)(b); if (!(_a >= _b - (tol) && _a <= _b + (tol))) \
    t::fail(__FILE__, __LINE__, std::string(#a " ~= " #b "  (") + std::to_string(_a) + " vs " + std::to_string(_b) + ")"); } while (0)
