#include "serialise.hh"
#include "error.hh"
#include "fmt.hh"
#include "libexpr/pos-table.hh"
#include "ref.hh"
#include "types.hh"
#include "util.hh"

#include <limits.h>
#include <gtest/gtest.h>

#include <numeric>

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

TEST(Sink, uint64_t)
{
    StringSink s;
    s << 42;
    ASSERT_EQ(s.s, std::string({42, 0, 0, 0, 0, 0, 0, 0}));
}

TEST(Sink, string_view)
{
    StringSink s;
    s << "";
    // clang-format off
    ASSERT_EQ(
        s.s,
        std::string({
            // length
            0, 0, 0, 0, 0, 0, 0, 0,
            // data (omitted)
        })
    );
    // clang-format on

    s = {};
    s << "test";
    // clang-format off
    ASSERT_EQ(
        s.s,
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

    s = {};
    s << "longer string";
    // clang-format off
    ASSERT_EQ(
        s.s,
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

TEST(Sink, StringSet)
{
    StringSink s;
    s << StringSet{};
    // clang-format off
    ASSERT_EQ(
        s.s,
        std::string({
            // length
            0, 0, 0, 0, 0, 0, 0, 0,
            // data (omitted)
        })
    );
    // clang-format on

    s = {};
    s << StringSet{"a", ""};
    // clang-format off
    ASSERT_EQ(
        s.s,
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

TEST(Sink, Strings)
{
    StringSink s;
    s << Strings{};
    // clang-format off
    ASSERT_EQ(
        s.s,
        std::string({
            // length
            0, 0, 0, 0, 0, 0, 0, 0,
            // data (omitted)
        })
    );
    // clang-format on

    s = {};
    s << Strings{"a", ""};
    // clang-format off
    ASSERT_EQ(
        s.s,
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

TEST(Sink, Error)
{
    PosTable pt;
    auto o = pt.addOrigin(Pos::String{make_ref<std::string>("test")}, 4);

    StringSink s;
    s << Error{{
        .level = lvlInfo,
        .msg = HintFmt("foo"),
        .pos = pt[pt.add(o, 1)],
        .traces = {{.pos = pt[pt.add(o, 2)], .hint = HintFmt("b %1%", "foo")}},
    }};
    // NOTE position of the error and all traces are ignored
    // by the wire format
    // clang-format off
    ASSERT_EQ(
        s.s,
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

}
