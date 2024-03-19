#include "serialise.hh"
#include "error.hh"
#include "fmt.hh"
#include "generator.hh"
#include "libexpr/pos-table.hh"
#include "ref.hh"
#include "types.hh"
#include "util.hh"

#include <concepts>
#include <initializer_list>
#include <limits.h>
#include <gtest/gtest.h>

#include <numeric>
#include <stdexcept>

namespace nix {

TEST(ChainSource, single)
{
    ChainSource s{StringSource{"test"}};
    ASSERT_EQ(s.drain(), "test");
}

TEST(ChainSource, multiple)
{
    ChainSource s{StringSource{"1"}, StringSource{""}, StringSource{"3"}};
    ASSERT_EQ(s.drain(), "13");
}

TEST(ChainSource, chunk)
{
    std::string buf(2, ' ');
    ChainSource s{StringSource{"111"}, StringSource{""}, StringSource{"333"}};

    s(buf.data(), buf.size());
    ASSERT_EQ(buf, "11");
    s(buf.data(), buf.size());
    ASSERT_EQ(buf, "13");
    s(buf.data(), buf.size());
    ASSERT_EQ(buf, "33");
    ASSERT_THROW(s(buf.data(), buf.size()), EndOfFile);
}

TEST(ChainSource, move)
{
    std::string buf(2, ' ');
    ChainSource s1{StringSource{"111"}, StringSource{""}, StringSource{"333"}};

    s1(buf.data(), buf.size());
    ASSERT_EQ(buf, "11");

    ChainSource s2 = std::move(s1);
    ASSERT_THROW(s1(buf.data(), buf.size()), EndOfFile);
    s2(buf.data(), buf.size());
    ASSERT_EQ(buf, "13");

    s1 = std::move(s2);
    ASSERT_THROW(s2(buf.data(), buf.size()), EndOfFile);
    s1(buf.data(), buf.size());
    ASSERT_EQ(buf, "33");
}

static std::string simpleToWire(const auto & val)
{
    std::string result;
    auto g = [&] () -> WireFormatGenerator { co_yield val; }();
    while (g) {
        auto bit = g();
        result.append(bit.data(), bit.size());
    }
    return result;
}

TEST(WireFormatGenerator, uint64_t)
{
    auto s = simpleToWire(42);
    ASSERT_EQ(s, std::string({42, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(WireFormatGenerator, string_view)
{
    auto s = simpleToWire("");
    // clang-format off
    ASSERT_EQ(
        s,
        std::string({
            // length
            0, 0, 0, 0, 0, 0, 0, 0,
            // data (omitted)
        })
    );
    // clang-format on

    s = simpleToWire("test");
    // clang-format off
    ASSERT_EQ(
        s,
        std::string({
            // length
            4, 0, 0, 0, 0, 0, 0, 0,
            // data
            't', 'e', 's', 't',
            // padding
            0, 0, 0, 0,
        })
    );
    // clang-format on

    s = simpleToWire("longer string");
    // clang-format off
    ASSERT_EQ(
        s,
        std::string({
            // length
            13, 0, 0, 0, 0, 0, 0, 0,
            // data
            'l', 'o', 'n', 'g', 'e', 'r', ' ', 's', 't', 'r', 'i', 'n', 'g',
            // padding
            0, 0, 0,
        })
    );
    // clang-format on
}

TEST(WireFormatGenerator, StringSet)
{
    auto s = simpleToWire(StringSet{});
    // clang-format off
    ASSERT_EQ(
        s,
        std::string({
            // length
            0, 0, 0, 0, 0, 0, 0, 0,
            // data (omitted)
        })
    );
    // clang-format on

    s = simpleToWire(StringSet{"a", ""});
    // clang-format off
    ASSERT_EQ(
        s,
        std::string({
            // length
            2, 0, 0, 0, 0, 0, 0, 0,
            // data ""
            0, 0, 0, 0, 0, 0, 0, 0,
            // data "a"
            1, 0, 0, 0, 0, 0, 0, 0, 'a', 0, 0, 0, 0, 0, 0, 0,
        })
    );
    // clang-format on
}

TEST(WireFormatGenerator, Strings)
{
    auto s = simpleToWire(Strings{});
    // clang-format off
    ASSERT_EQ(
        s,
        std::string({
            // length
            0, 0, 0, 0, 0, 0, 0, 0,
            // data (omitted)
        })
    );
    // clang-format on

    s = simpleToWire(Strings{"a", ""});
    // clang-format off
    ASSERT_EQ(
        s,
        std::string({
            // length
            2, 0, 0, 0, 0, 0, 0, 0,
            // data "a"
            1, 0, 0, 0, 0, 0, 0, 0, 'a', 0, 0, 0, 0, 0, 0, 0,
            // data ""
            0, 0, 0, 0, 0, 0, 0, 0,
        })
    );
    // clang-format on
}

TEST(WireFormatGenerator, Error)
{
    PosTable pt;
    auto o = pt.addOrigin(Pos::String{make_ref<std::string>("test")}, 4);

    auto s = simpleToWire(Error{{
        .level = lvlInfo,
        .msg = HintFmt("foo"),
        .pos = pt[pt.add(o, 1)],
        .traces = {{.pos = pt[pt.add(o, 2)], .hint = HintFmt("b %1%", "foo")}},
    }});
    // NOTE position of the error and all traces are ignored
    // by the wire format
    // clang-format off
    ASSERT_EQ(
        s,
        std::string({
            5, 0, 0, 0, 0, 0, 0, 0, 'E', 'r', 'r', 'o', 'r', 0, 0, 0,
            3, 0, 0, 0, 0, 0, 0, 0,
            5, 0, 0, 0, 0, 0, 0, 0, 'E', 'r', 'r', 'o', 'r', 0, 0, 0,
            3, 0, 0, 0, 0, 0, 0, 0, 'f', 'o', 'o', 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0,
            1, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0,
            16, 0, 0, 0, 0, 0, 0, 0,
            'b', ' ', '\x1b', '[', '3', '5', ';', '1', 'm', 'f', 'o', 'o', '\x1b', '[', '0', 'm',
        })
    );
    // clang-format on
}

TEST(FullFormatter, foo)
{
    auto gen = []() -> Generator<std::span<const char>, SerializingTransform> {
        std::set<std::string> foo{"a", "longer string", ""};
        co_yield 42;
        co_yield foo;
        co_yield std::string_view("test");
        co_yield 7;
    }();

    std::vector<char> full;
    while (gen) {
        auto s = gen();
        full.insert(full.end(), s.begin(), s.end());
    }

    ASSERT_EQ(
        full,
        (std::vector<char>{
            // clang-format off
            // 32
            42, 0, 0, 0, 0, 0, 0, 0,
            // foo
            3, 0, 0, 0, 0, 0, 0, 0,
            /// ""
            0, 0, 0, 0, 0, 0, 0, 0,
            /// a
            1, 0, 0, 0, 0, 0, 0, 0,
            'a', 0, 0, 0, 0, 0, 0, 0,
            /// longer string
            13, 0, 0, 0, 0, 0, 0, 0,
            'l', 'o', 'n', 'g', 'e', 'r', ' ', 's', 't', 'r', 'i', 'n', 'g', 0, 0, 0,
            // foo done
            // test
            4, 0, 0, 0, 0, 0, 0, 0,
            't', 'e', 's', 't', 0, 0, 0, 0,
            // 7
            7, 0, 0, 0, 0, 0, 0, 0,
            //clang-format on
            }));
}

}
