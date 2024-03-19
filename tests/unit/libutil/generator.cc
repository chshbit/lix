#include "generator.hh"

#include <concepts>
#include <cstdint>
#include <gtest/gtest.h>

namespace nix {

TEST(Generator, yields)
{
    auto g = []() -> Generator<int> {
        co_yield 1;
        co_yield 2;
    }();

    ASSERT_TRUE(bool(g));
    ASSERT_EQ(g(), 1);
    ASSERT_EQ(g(), 2);
    ASSERT_FALSE(bool(g));
}

TEST(Generator, nests)
{
    auto g = []() -> Generator<int> {
        co_yield 1;
        co_yield []() -> Generator<int> {
            co_yield 9;
            co_yield []() -> Generator<int> {
                co_yield 99;
                co_yield 100;
            }();
        }();

        auto g2 = []() -> Generator<int> {
            co_yield []() -> Generator<int> {
                co_yield 2000;
                co_yield 2001;
            }();
            co_yield 1001;
        }();

        co_yield g2();
        co_yield std::move(g2);
        co_yield 2;
    }();

    ASSERT_TRUE(bool(g));
    ASSERT_EQ(g(), 1);
    ASSERT_EQ(g(), 9);
    ASSERT_EQ(g(), 99);
    ASSERT_EQ(g(), 100);
    ASSERT_EQ(g(), 2000);
    ASSERT_EQ(g(), 2001);
    ASSERT_EQ(g(), 1001);
    ASSERT_EQ(g(), 2);
    ASSERT_FALSE(bool(g));
}

TEST(Generator, nestsExceptions)
{
    auto g = []() -> Generator<int> {
        co_yield 1;
        co_yield []() -> Generator<int> {
            co_yield 9;
            throw 1;
            co_yield 10;
        }();
        co_yield 2;
    }();

    ASSERT_TRUE(bool(g));
    ASSERT_EQ(g(), 1);
    ASSERT_EQ(g(), 9);
    ASSERT_THROW(g(), int);
}

TEST(Generator, exception)
{
    {
        auto g = []() -> Generator<int> {
            throw 1;
            co_return;
        }();

        ASSERT_THROW(void(bool(g)), int);
    }
    {
        auto g = []() -> Generator<int> {
            throw 1;
            co_return;
        }();

        ASSERT_THROW(g(), int);
    }
}

namespace {
struct Transform
{
    int state = 0;

    std::pair<uint32_t, int> operator()(std::integral auto x)
    {
        return {x, state++};
    }

    Generator<std::pair<uint32_t, int>, Transform> operator()(const char *)
    {
        co_yield 9;
        co_yield 19;
    }

    Generator<std::pair<uint32_t, int>, Transform> operator()(Generator<int> && inner)
    {
        return [](auto g) mutable -> Generator<std::pair<uint32_t, int>, Transform> {
            while (g) {
                co_yield g();
            }
        }(std::move(inner));
    }
};
}

TEST(Generator, transform)
{
    auto g = []() -> Generator<std::pair<uint32_t, int>, Transform> {
        co_yield int32_t(-1);
        co_yield "";
        std::cerr << "1\n";
        co_yield []() -> Generator<int> { co_yield 7; }();
        co_yield 20;
    }();

    ASSERT_EQ(g(), (std::pair<unsigned, int>{4294967295, 0}));
    ASSERT_EQ(g(), (std::pair<unsigned, int>{9, 0}));
    ASSERT_EQ(g(), (std::pair<unsigned, int>{19, 1}));
    ASSERT_EQ(g(), (std::pair<unsigned, int>{7, 0}));
    ASSERT_EQ(g(), (std::pair<unsigned, int>{20, 1}));
}

}
