#include "viewer/core/Uuid.hpp"

#include <doctest/doctest.h>

#include <unordered_set>

using viewer::Uuid;

TEST_CASE("Uuid parses and formats canonical strings")
{
    const auto id = Uuid::parse("00F419C9-b54e-95c2-620c-3ac6bb1dbb4a");
    REQUIRE(id.has_value());
    CHECK(id->bytes[0] == 0x00);
    CHECK(id->bytes[1] == 0xF4);
    CHECK(id->bytes[15] == 0x4A);
    CHECK(id->toString() == "00f419c9-b54e-95c2-620c-3ac6bb1dbb4a");
    CHECK_FALSE(id->isNull());
    CHECK(Uuid{}.isNull());
}

TEST_CASE("Uuid rejects malformed strings")
{
    CHECK_FALSE(Uuid::parse("").has_value());
    CHECK_FALSE(Uuid::parse("00f419c9b54e95c2620c3ac6bb1dbb4a").has_value());
    CHECK_FALSE(Uuid::parse("00f419c9-b54e-95c2-620c-3ac6bb1dbb4g").has_value());
    CHECK_FALSE(Uuid::parse("00f419c9_b54e-95c2-620c-3ac6bb1dbb4a").has_value());
}

TEST_CASE("Uuid works as a hash key")
{
    std::unordered_set<Uuid> set;
    set.insert(*Uuid::parse("00f419c9-b54e-95c2-620c-3ac6bb1dbb4a"));
    set.insert(*Uuid::parse("00f419c9-b54e-95c2-620c-3ac6bb1dbb4a"));
    set.insert(*Uuid::parse("0147310a-ff0a-9c8d-bc23-14f628ecbabf"));
    CHECK(set.size() == 2);
}
