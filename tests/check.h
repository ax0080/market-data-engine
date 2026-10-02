#pragma once
// Minimal self-registering test harness (no external dependency).

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace check {

struct Test {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Test>& registry() {
    static std::vector<Test> r;
    return r;
}
inline int& failures() {
    static int f = 0;
    return f;
}

struct Register {
    Register(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

inline int run_all() {
    for (const Test& t : registry()) {
        const int before = failures();
        t.fn();
        std::printf("%s %s\n", failures() == before ? "[pass]" : "[FAIL]", t.name);
    }
    std::printf("%zu tests, %d failed checks\n", registry().size(), failures());
    return failures() == 0 ? 0 : 1;
}

}  // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)
#define TEST(name)                                                                    \
    static void name();                                                               \
    static check::Register CHECK_CAT(reg_, name)(#name, name);                        \
    static void name()
#define CHECK(cond)                                                                   \
    do {                                                                              \
        if (!(cond)) {                                                                \
            ++check::failures();                                                      \
            std::printf("  %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);    \
        }                                                                             \
    } while (0)
#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        const auto va_ = (a);                                                         \
        const auto vb_ = (b);                                                         \
        if (!(va_ == vb_)) {                                                          \
            ++check::failures();                                                      \
            std::printf("  %s:%d: %s == %s failed (%lld vs %lld)\n", __FILE__, __LINE__, #a, #b, \
                        static_cast<long long>(va_), static_cast<long long>(vb_));    \
        }                                                                             \
    } while (0)
