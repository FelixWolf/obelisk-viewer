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
