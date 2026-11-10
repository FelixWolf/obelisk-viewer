#include "viewer/core/Md5.hpp"

#include <doctest/doctest.h>

#include <string>

using viewer::md5Hex;

TEST_CASE("MD5 matches the RFC 1321 test suite")
{
    CHECK(md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(md5Hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(md5Hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(md5Hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
          "d174ab98d277d9f5a5611c2c9f419d9f");
    CHECK(md5Hex("1234567890123456789012345678901234567890123456789012345678901234567890123456"
                 "7890") == "57edf4a22be3c955ac49da2e2107b67a");
}

TEST_CASE("MD5 handles every padding boundary")
{
    // 55, 56 and 64 bytes straddle the one-or-two padding block cutoff.
    CHECK(md5Hex(std::string(55, 'x')) == "04364420e25c512fd958a70738aa8f72");
    CHECK(md5Hex(std::string(56, 'x')) == "668a72d5ba17f08e62dabcafad6db14b");
    CHECK(md5Hex(std::string(64, 'x')) == "c1bb4f81d892b2d57947682aeb252456");
}
