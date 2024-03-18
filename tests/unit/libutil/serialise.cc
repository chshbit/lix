#include "serialise.hh"
#include "types.hh"

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

}
