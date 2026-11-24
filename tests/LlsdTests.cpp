#include "TestUtil.hpp"

#include "viewer/llsd/Serialization.hpp"

#include <doctest/doctest.h>

using namespace viewer::llsd;

namespace
{

Binary bytes(std::string_view s)
{
    return Binary(s.begin(), s.end());
}

Value parseOrFail(std::string_view xml)
{
    auto result = parse(xml);
    INFO((result ? std::string() : result.error()));
    REQUIRE(result.has_value());
    return std::move(*result);
}

} // namespace

TEST_CASE("LLSD+XML: pymetaverse sample document")
{
    const auto file = viewer::test::readTestFile("llsd_sample.xml");
    const Value root = parseOrFail(asText(file));
    REQUIRE(root.isMap());

    CHECK(root["undef"][0].isUndefined());

    const Value& booleans = root["boolean"];
    REQUIRE(booleans.size() == 5);
    CHECK(booleans[0].asBoolean());
    CHECK(booleans[1].asBoolean());
    CHECK_FALSE(booleans[2].asBoolean());
    CHECK_FALSE(booleans[3].asBoolean());
    CHECK(booleans[4].type() == Value::Type::Boolean);
    CHECK_FALSE(booleans[4].asBoolean());

    CHECK(root["integer"][0].asInteger() == 289343);
    CHECK(root["integer"][1].asInteger() == -3);
    CHECK(root["integer"][2].type() == Value::Type::Integer);
    CHECK(root["integer"][2].asInteger() == 0);

    CHECK(root["real"][0].asReal() == doctest::Approx(-0.28334));
    CHECK(root["real"][1].asReal() == doctest::Approx(2983287453.3848387));
    CHECK(root["real"][2].asReal() == 0.0);

    CHECK(root["uuid"][0].asUuid().toString() == "d7f4aeca-88f1-42a1-b385-b9db18abb255");
    CHECK(root["uuid"][1].type() == Value::Type::Uuid);
    CHECK(root["uuid"][1].asUuid().isNull());

    CHECK(root["string"][0].asString() == "The quick brown fox jumped over the lazy dog.");
    CHECK(root["string"][2].type() == Value::Type::String);
    CHECK(root["string"][2].asString().empty());

    const Value& binary = root["binary"];
    REQUIRE(binary.size() == 5);
    CHECK((binary[0].asBinary() == bytes("random")));
    CHECK((binary[1].asBinary() == bytes("the quick brown fox")));
    CHECK((binary[2].asBinary() == bytes("jumped over the")));
    CHECK((binary[3].asBinary() == bytes("lazy dog")));
    CHECK(binary[4].asBinary().empty());

    CHECK(formatDate(root["date"][0].asDate()) == "2006-02-01T14:29:53.430000Z");
    CHECK(root["date"][1].asDate() == Date{});

    CHECK(root["uri"][0].asUri().value == "http://sim956.agni.lindenlab.com:12035/runtime/agents");
    CHECK(root["uri"][1].type() == Value::Type::Uri);
}

TEST_CASE("LLSD+XML: write then read gives the same value")
{
    Value original;
    original["name"] = "Test Resident";
    original["agent_id"] = *viewer::Uuid::parse("d7f4aeca-88f1-42a1-b385-b9db18abb255");
    original["count"] = 42;
    original["negative"] = -7;
    original["ratio"] = 0.1;
    original["enabled"] = true;
    original["disabled"] = false;
    original["empty"] = "";
    original["markup"] = "<tag> & \"quotes\" 'n' stuff";
    original["spaces"] = "   ";
    original["blob"] = Binary{0, 1, 2, 254, 255};
    original["seen"] = *parseDate("2024-05-06T07:08:09.123456Z");
    original["link"] = Uri{"https://example.com/a?b=c&d=e"};
    original["nothing"] = Value();
    original["list"].append(1);
    original["list"].append("two");
    original["list"].append(Array{});
    original["nested"]["deeper"]["deepest"] = 3.5;

    for(bool compact : {true, false})
    {
        for(bool pretty : {false, true})
        {
            CAPTURE(compact);
            CAPTURE(pretty);
            const std::string xml = toXml(original, {.compact = compact, .pretty = pretty});
            const Value roundTripped = parseOrFail(xml);
            CHECK(roundTripped == original);
        }
    }
}

TEST_CASE("LLSD+XML: compact output uses empty elements for defaults")
{
    Value v;
    v["i"] = 0;
    v["s"] = "";
    v["b"] = false;
    const std::string xml = toXml(v);
    CHECK(xml.starts_with("<?xml"));
    CHECK(xml.find("<integer/>") != std::string::npos);
    CHECK(xml.find("<string/>") != std::string::npos);
    CHECK(xml.find("<boolean/>") != std::string::npos);
}

TEST_CASE("LLSD+XML: malformed documents are rejected with a message")
{
    CHECK_FALSE(parse("").has_value());
    CHECK_FALSE(parse("not llsd at all").has_value());
    CHECK_FALSE(fromXml("<llsd><integer>12abc</integer></llsd>").has_value());
    CHECK_FALSE(fromXml("<llsd><boolean>maybe</boolean></llsd>").has_value());
    CHECK_FALSE(fromXml("<llsd><uuid>not-a-uuid</uuid></llsd>").has_value());
    CHECK_FALSE(fromXml("<llsd><map><key>a</key></map></llsd>").has_value());
    CHECK_FALSE(fromXml("<llsd><map><string>a</string></map></llsd>").has_value());
    CHECK_FALSE(fromXml("<llsd><binary encoding=\"rot13\">abc</binary></llsd>").has_value());
    CHECK_FALSE(fromXml("<llsd><date>yesterday</date></llsd>").has_value());
    CHECK_FALSE(fromXml("<notllsd/>").has_value());
    CHECK_FALSE(fromXml("<llsd><integer>1</llsd>").has_value());

    const auto error = fromXml("<llsd>\n<array>\n<blah/>\n</array></llsd>");
    REQUIRE_FALSE(error.has_value());
    CHECK(error.error().find("line 3") != std::string::npos);

    CHECK(parse("<?xml version=\"1.0\"?><llsd/>").value().isUndefined());
}

TEST_CASE("LLSD format detection")
{
    CHECK(detectFormat("<?xml version=\"1.0\"?><llsd/>") == Format::Xml);
    CHECK(detectFormat("  <llsd><undef/></llsd>") == Format::Xml);
    CHECK(detectFormat("<?llsd/notation?>\n{}") == Format::Notation);
    CHECK(detectFormat("<?LLSD/Binary?>\n") == Format::Binary);
    CHECK_FALSE(detectFormat("{'a':1}").has_value());
    CHECK_FALSE(parse("<?llsd/binary?>\n").has_value()); // header without data
}

TEST_CASE("LLSD value conversions")
{
    CHECK(Value("42").asInteger() == 42);
    CHECK(Value("3.9").asInteger() == 3);
    CHECK(Value(3.9).asInteger() == 3);
    CHECK(Value(1e20).asInteger() == 2147483647);
    CHECK(Value(true).asInteger() == 1);
    CHECK(Value(7).asReal() == 7.0);
    CHECK(Value(7).asString() == "7");
    CHECK(Value(true).asString() == "true");
    CHECK(Value(false).asString().empty());
    CHECK(Value("").asBoolean() == false);
    CHECK(Value("x").asBoolean() == true);
    CHECK(Value("d7f4aeca-88f1-42a1-b385-b9db18abb255").asUuid().toString() ==
          "d7f4aeca-88f1-42a1-b385-b9db18abb255");
    CHECK(Value(0xFFFFFFFFu).asInteger() == -1); // U32 keeps its bit pattern
    CHECK(static_cast<uint32_t>(
              fromXml("<llsd><integer>3000000000</integer></llsd>").value().asInteger()) ==
          3000000000u);
    CHECK(fromXml("<llsd><integer>99999999999</integer></llsd>").value().asInteger() == 2147483647);

    Value missing;
    CHECK(std::as_const(missing)["nope"]["deeper"].isUndefined()); // const: no insertion
    CHECK(std::as_const(missing)[5].isUndefined());
    CHECK(missing.size() == 0);
}

TEST_CASE("LLSD maps compare without regard to order")
{
    Map a{{"x", 1}, {"y", 2}};
    Map b{{"y", 2}, {"x", 1}};
    CHECK(a == b);
    b.set("x", 3);
    CHECK_FALSE(a == b);
    CHECK(a.erase("x"));
    CHECK_FALSE(a.contains("x"));
}

TEST_CASE("LLSD mutable access creates containers")
{
    Value v;
    v["a"]["b"] = 1;
    v["list"][2] = "third";
    CHECK(v.size() == 2);
    CHECK(v["a"]["b"].asInteger() == 1);
    CHECK(v["list"].size() == 3);
    CHECK(v["list"][0].isUndefined());
}

TEST_CASE("LLSD binary: round trip and known bytes")
{
    Value v;
    v["int"] = -5;
    v["real"] = 2.5;
    v["text"] = "hello";
    v["id"] = *viewer::Uuid::parse("d7f4aeca-88f1-42a1-b385-b9db18abb255");
    v["flag"] = true;
    v["blob"] = Binary{1, 2, 3};
    v["link"] = Uri{"https://example.com"};
    v["when"] = *parseDate("2024-05-06T07:08:09Z");
    v["list"].append(1);
    v["list"].append(Value());
    const auto bytes = toBinary(v);
    size_t consumed = 0;
    const auto back = fromBinary(bytes, &consumed);
    REQUIRE(back.has_value());
    CHECK(*back == v);
    CHECK(consumed == bytes.size());

    // Integers are big-endian; map keys are 'k' + length + bytes.
    const std::vector<uint8_t> known = {'{', 0,   0,   0, 1, 'k', 0, 0,  0,
                                        1,   'a', 'i', 0, 0, 1,   0, '}'};
    const auto parsed = fromBinary(known);
    REQUIRE(parsed.has_value());
    CHECK((*parsed)["a"].asInteger() == 256);

    // Through parse() with the header line.
    std::string withHeader = "<?llsd/binary?>\n";
    withHeader.append(bytes.begin(), bytes.end());
    CHECK(parse(withHeader).value() == v);

    // Truncated and garbage data fail cleanly.
    CHECK_FALSE(fromBinary(std::span(bytes).first(bytes.size() / 2)).has_value());
    CHECK_FALSE(fromBinary(std::vector<uint8_t>{'z'}).has_value());
    CHECK_FALSE(fromBinary(std::vector<uint8_t>{'[', 0xFF, 0xFF, 0xFF, 0xFF}).has_value());
}

TEST_CASE("LLSD notation")
{
    using namespace viewer::llsd;
    // The shape of a GLTF material override (GenericStreamingMessage 0x4175).
    const auto v = fromNotation(
        "{'id':i1234,'te':[i0,i3],'od':[{'bc':[r1,r0.5,r0.25,r1],'am':i2,'ds':1,"
        "'tex':[u11111111-2222-3333-4444-555555555555,!]},{'mf':r0.75,\"rf\":r0.125e1}]}");
    REQUIRE(v.has_value());
    CHECK((*v)["id"].asInteger() == 1234);
    CHECK((*v)["te"].size() == 2);
    CHECK((*v)["te"][1].asInteger() == 3);
    const Value& first = (*v)["od"][0];
    CHECK(first["bc"][1].asReal() == 0.5);
    CHECK(first["am"].type() == Value::Type::Integer);
    CHECK(first["ds"].asBoolean());
    CHECK(first["tex"][0].asUuid().toString() == "11111111-2222-3333-4444-555555555555");
    CHECK(first["tex"][1].isUndefined());
    CHECK((*v)["od"][1]["rf"].asReal() == 1.25);

    // Strings: both quotes, escapes, sized; binary; booleans spelled out.
    const auto s = fromNotation(
        R"N([ "a\"b\x41\n", 'c', s(3)"x"y", b64"AQID", b(2)"zz", true, F, l"http://x", ! ])N");
    REQUIRE(s.has_value());
    CHECK((*s)[0].asString() == "a\"bA\n");
    CHECK((*s)[1].asString() == "c");
    CHECK((*s)[2].asString() == "x\"y");
    CHECK((*s)[3].asBinary() == Binary{1, 2, 3});
    CHECK((*s)[4].asBinary() == Binary{'z', 'z'});
    CHECK((*s)[5].asBoolean());
    CHECK_FALSE((*s)[6].asBoolean());
    CHECK((*s)[7].type() == Value::Type::Uri);
    CHECK((*s)[8].isUndefined());

    // parse() recognizes the header.
    CHECK(parse("<?llsd/notation?>\n{'a':i1}")->operator[]("a").asInteger() == 1);
    CHECK_FALSE(fromNotation("{'a':").has_value());
    CHECK_FALSE(fromNotation("[i1, ").has_value());
}

TEST_CASE("JSON into LLSD")
{
    using namespace viewer::llsd;
    const auto v = fromJson(R"({"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0.5,0,1],
        "metallicFactor":0}, "alphaMode":"MASK", "doubleSided":true}],
        "images":[{"uri":"11111111-2222-3333-4444-555555555555"}], "n":null, "big":3000000000,
        "esc":"é😀\t"})");
    REQUIRE(v.has_value());
    const Value& m = (*v)["materials"][0];
    CHECK(m["pbrMetallicRoughness"]["baseColorFactor"][1].asReal() == 0.5);
    CHECK(m["pbrMetallicRoughness"]["metallicFactor"].type() == Value::Type::Integer);
    CHECK(m["alphaMode"].asString() == "MASK");
    CHECK(m["doubleSided"].asBoolean());
    CHECK((*v)["images"][0]["uri"].asString() == "11111111-2222-3333-4444-555555555555");
    CHECK((*v)["n"].isUndefined());
    CHECK((*v)["big"].type() == Value::Type::Real);
    CHECK((*v)["esc"].asString() == "\xc3\xa9\xf0\x9f\x98\x80\t");
    CHECK_FALSE(fromJson("{\"a\":1,}").has_value());
    CHECK_FALSE(fromJson("[1 2]").has_value());
    CHECK_FALSE(fromJson("{} x").has_value());
}

TEST_CASE("LLSD headers with spaces")
{
    using namespace viewer::llsd;
    Value v;
    v["a"] = 7;
    const auto binary = toBinary(v);
    std::string text = "<? LLSD/Binary ?>\n";
    text.append(binary.begin(), binary.end());
    const auto parsed = parse(text);
    REQUIRE(parsed.has_value());
    CHECK((*parsed)["a"].asInteger() == 7);
    CHECK(detectFormat("<? llsd/notation ?>\n{}") == Format::Notation);
    // Settings assets: notation after its header.
    const auto sky = parse("<? LLSD/Notation ?>\n{'type':'sky','gamma':r1.5}");
    REQUIRE(sky.has_value());
    CHECK((*sky)["type"].asString() == "sky");
    CHECK((*sky)["gamma"].asReal() == doctest::Approx(1.5));
}

TEST_CASE("LLSD notation: written as it's read back")
{
    Map inner{{"ok", true}, {"n", 0}};
    Value v(Map{
        {"undef", Value()},
        {"bool", false},
        {"int", -42},
        {"real", 0.25},
        {"string", "it's \\ \"quoted\"\nand tabbed\t\x01 ünï"},
        {"uuid", viewer::Uuid::parse("d7f4aeca-88f1-42a1-b385-b9db18abb255").value()},
        {"date", parseDate("2006-02-01T14:29:53.430000Z").value()},
        {"uri", Uri{"https://example.com/a\"b"}},
        {"binary", bytes("hello\0world")},
        {"array", Array{1, "two", Array{}, Map{}}},
        {"map", inner},
    });
    for(const bool pretty : {false, true})
    {
        const std::string text = toNotation(v, {.pretty = pretty, .header = pretty});
        INFO(text);
        const auto back = pretty ? parse(text) : fromNotation(text);
        REQUIRE(back.has_value());
        CHECK(*back == v);
    }
}

TEST_CASE("LLSD notation: compact and pretty forms")
{
    const Value v(Map{{"a", 1}, {"b", Array{true, "x"}}, {"c", Map{}}});
    CHECK(toNotation(v) == "{'a':i1,'b':[true,'x'],'c':{}}");
    CHECK(toNotation(v, {.pretty = true, .header = true}) == "<?llsd/notation?>\n"
                                                             "{\n"
                                                             "    'a': i1,\n"
                                                             "    'b': [\n"
                                                             "        true,\n"
                                                             "        'x'\n"
                                                             "    ],\n"
                                                             "    'c': {}\n"
                                                             "}\n");
}
