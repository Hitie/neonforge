#pragma once
#include <functional>
#include <iostream>
#include <stdexcept>
#include <vector>
inline std::vector<std::pair<const char *, std::function<void()>>> &tests()
{
    static std::vector<std::pair<const char *, std::function<void()>>> x;
    return x;
}
struct RegisterTest
{
    RegisterTest(const char *n, std::function<void()> f) { tests().emplace_back(n, std::move(f)); }
};
#define CAT_(a, b) a##b
#define CAT(a, b) CAT_(a, b)
#define TEST(name)                                                                                 \
    void name();                                                                                   \
    static RegisterTest CAT(reg_, name)(#name, name);                                              \
    void name()
#define CHECK(expr)                                                                                \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
            throw std::runtime_error("check failed: " #expr);                                      \
    } while (false)
template <class E = std::exception, class F> void rejects(F f)
{
    bool caught = false;
    try
    {
        f();
    }
    catch (const E &)
    {
        caught = true;
    }
    CHECK(caught);
}
inline int run_tests()
{
    int failed = 0;
    for (auto &[name, fn] : tests())
    {
        try
        {
            fn();
            std::cout << "PASS " << name << '\n';
        }
        catch (const std::exception &e)
        {
            ++failed;
            std::cout << "FAIL " << name << ": " << e.what() << '\n';
        }
        catch (...)
        {
            ++failed;
            std::cout << "FAIL " << name << ": unknown exception\n";
        }
    }
    std::cout << tests().size() - failed << '/' << tests().size() << " passed\n";
    return failed ? 1 : 0;
}
