#include "viewer/core/Md5.hpp"
#include "viewer/core/Sha256.hpp"

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

TEST_CASE("SHA-256 matches FIPS 180-4's examples")
{
    using viewer::sha256;
    using viewer::toHex;
    CHECK(toHex(sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(toHex(sha256("abc")) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(toHex(sha256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")) ==
          "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    // A million a's: many blocks.
    CHECK(toHex(sha256(std::string(1'000'000, 'a'))) ==
          "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

TEST_CASE("SHA-256 handles every padding boundary")
{
    using viewer::sha256;
    using viewer::toHex;
    CHECK(toHex(sha256(std::string(55, 'a'))) ==
          "9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318");
    CHECK(toHex(sha256(std::string(56, 'a'))) ==
          "b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a");
    CHECK(toHex(sha256(std::string(64, 'a'))) ==
          "ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb");
}
