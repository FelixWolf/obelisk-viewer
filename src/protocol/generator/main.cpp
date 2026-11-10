// viewer_msggen: generates C++ message structs from message_template.msg.
//
//   viewer_msggen <message_template.msg> <out.hpp> <out.cpp>
//
// For every message it emits a struct in viewer::protocol::msg with:
//   - static constexpr MessageInfo kInfo (number, name, flags)
//   - one nested struct per block type, named <Block>Block
//   - one member per block: the struct (Single), std::array (Multiple N) or
//     std::vector (Variable, at most 255 entries)
//   - encode(MessageWriter&) and decode(MessageReader&)
// Members keep the template's names so they match the protocol docs. A block
// named like its message gets a trailing underscore, since C++ doesn't allow
// a member with its class's name.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <format>
#include <fstream>
#include <map>
#include <print>
#include <sstream>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace
{

// --- Template parsing: tokens and nested { } lists --------------------------

struct Node
{
    std::string word; // set for leaves
    std::vector<Node> children;
    bool isList = false;
    int line = 0;
};

std::vector<Node> parseNodes(const std::string& text)
{
    std::vector<std::vector<Node>> stack(1);
    std::vector<int> openLines;
    std::string word;
    int line = 1;
    auto flush = [&]
    {
        if(!word.empty())
        {
            stack.back().push_back(Node{word, {}, false, line});
            word.clear();
        }
    };
    for(size_t i = 0; i < text.size(); i++)
    {
        const char c = text[i];
        if(c == '/' && i + 1 < text.size() && text[i + 1] == '/')
        {
            flush();
            while(i < text.size() && text[i] != '\n')
            {
                i++;
            }
            line++;
            continue;
        }
        if(c == '{')
        {
            flush();
            stack.emplace_back();
            openLines.push_back(line);
        }
        else if(c == '}')
        {
            flush();
            if(stack.size() < 2)
            {
                throw std::runtime_error(std::format("line {}: unmatched }}", line));
            }
            Node list{{}, std::move(stack.back()), true, openLines.back()};
            stack.pop_back();
            openLines.pop_back();
            stack.back().push_back(std::move(list));
        }
        else if(c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            flush();
            if(c == '\n')
            {
                line++;
            }
        }
        else
        {
            word.push_back(c);
        }
    }
    flush();
    if(stack.size() != 1)
    {
        throw std::runtime_error("unterminated {");
    }
    return std::move(stack.front());
}

// --- Model ------------------------------------------------------------------

struct Field
{
    std::string name;
    std::string type; // template type name
    int size = 0;     // Fixed / Variable only
};

enum class Quantity
{
    Single,
    Multiple,
    Variable,
};

struct Block
{
    std::string name;
    Quantity quantity = Quantity::Single;
    int count = 1; // Multiple only
    std::vector<Field> fields;
};

struct Message
{
    std::string name;
    std::string frequency; // High, Medium, Low, Fixed
    uint32_t id = 0;       // fully qualified
    bool trusted = false;
    bool zerocoded = false;
    std::string deprecation = "None";
    std::vector<Block> blocks;
};

[[noreturn]] void fail(const Node& node, const std::string& message)
{
    throw std::runtime_error(std::format("line {}: {}", node.line, message));
}

std::vector<const Node*> leaves(const Node& list)
{
    std::vector<const Node*> out;
    for(const auto& child : list.children)
    {
        if(!child.isList)
        {
            out.push_back(&child);
        }
    }
    return out;
}

std::vector<const Node*> lists(const Node& list)
{
    std::vector<const Node*> out;
    for(const auto& child : list.children)
    {
        if(child.isList)
        {
            out.push_back(&child);
        }
    }
    return out;
}

uint32_t parseNumber(const Node& node, const std::string& text)
{
    try
    {
        size_t used = 0;
        const unsigned long long value = std::stoull(text, &used, 0);
        if(used != text.size() || value > 0xFFFFFFFFull)
        {
            fail(node, "bad number " + text);
        }
        return static_cast<uint32_t>(value);
    }
    catch(const std::logic_error&)
    {
        fail(node, "bad number " + text);
    }
}

Message parseMessage(const Node& node)
{
    const auto head = leaves(node);
    if(head.size() < 5)
    {
        fail(node, "message header needs name, frequency, number, trust and encoding");
    }
    Message m;
    m.name = head[0]->word;
    m.frequency = head[1]->word;
    const uint32_t number = parseNumber(*head[2], head[2]->word);
    if(m.frequency == "High")
    {
        if(number > 0xFE)
        {
            fail(node, "High message number out of range");
        }
        m.id = number;
    }
    else if(m.frequency == "Medium")
    {
        if(number > 0xFE)
        {
            fail(node, "Medium message number out of range");
        }
        m.id = 0xFF00u | number;
    }
    else if(m.frequency == "Low")
    {
        if(number > 0xFFFF)
        {
            fail(node, "Low message number out of range");
        }
        m.id = 0xFFFF0000u | number;
    }
    else if(m.frequency == "Fixed")
    {
        m.id = number;
    }
    else
    {
        fail(node, "unknown frequency " + m.frequency);
    }

    if(head[3]->word != "Trusted" && head[3]->word != "NotTrusted")
    {
        fail(node, "unknown trust " + head[3]->word);
    }
    m.trusted = head[3]->word == "Trusted";
    if(head[4]->word != "Zerocoded" && head[4]->word != "Unencoded")
    {
        fail(node, "unknown encoding " + head[4]->word);
    }
    m.zerocoded = head[4]->word == "Zerocoded";
    if(head.size() > 5)
    {
        const std::string& d = head[5]->word;
        if(d == "UDPDeprecated")
        {
            m.deprecation = "UdpDeprecated";
        }
        else if(d == "UDPBlackListed")
        {
            m.deprecation = "UdpBlacklisted";
        }
        else if(d == "Deprecated")
        {
            m.deprecation = "Deprecated";
        }
        else
        {
            fail(node, "unknown deprecation " + d);
        }
    }

    for(const Node* blockNode : lists(node))
    {
        const auto bhead = leaves(*blockNode);
        if(bhead.size() < 2)
        {
            fail(*blockNode, "block needs a name and quantity");
        }
        Block b;
        b.name = bhead[0]->word;
        const std::string& quantity = bhead[1]->word;
        if(quantity == "Single")
        {
            b.quantity = Quantity::Single;
        }
        else if(quantity == "Variable")
        {
            b.quantity = Quantity::Variable;
        }
        else if(quantity == "Multiple")
        {
            if(bhead.size() < 3)
            {
                fail(*blockNode, "Multiple block needs a count");
            }
            b.quantity = Quantity::Multiple;
            b.count = static_cast<int>(parseNumber(*bhead[2], bhead[2]->word));
        }
        else
        {
            fail(*blockNode, "unknown quantity " + quantity);
        }

        for(const Node* fieldNode : lists(*blockNode))
        {
            const auto fhead = leaves(*fieldNode);
            if(fhead.size() < 2)
            {
                fail(*fieldNode, "field needs a name and type");
            }
            Field f{fhead[0]->word, fhead[1]->word, 0};
            if(f.type == "Fixed" || f.type == "Variable")
            {
                if(fhead.size() < 3)
                {
                    fail(*fieldNode, f.type + " field needs a size");
                }
                f.size = static_cast<int>(parseNumber(*fhead[2], fhead[2]->word));
                if(f.type == "Variable" && f.size != 1 && f.size != 2)
                {
                    fail(*fieldNode, "Variable size must be 1 or 2");
                }
            }
            b.fields.push_back(std::move(f));
        }
        m.blocks.push_back(std::move(b));
    }
    return m;
}

std::vector<Message> parseTemplate(const std::string& text)
{
    const auto nodes = parseNodes(text);
    if(nodes.size() < 2 || nodes[0].isList || nodes[0].word != "version")
    {
        throw std::runtime_error("template must start with 'version'");
    }
    if(nodes[1].word != "2.0")
    {
        throw std::runtime_error("only template version 2.0 is supported");
    }
    std::vector<Message> messages;
    std::map<uint32_t, std::string> ids;
    for(size_t i = 2; i < nodes.size(); i++)
    {
        if(!nodes[i].isList)
        {
            fail(nodes[i], "unexpected " + nodes[i].word);
        }
        Message m = parseMessage(nodes[i]);
        if(auto [it, inserted] = ids.emplace(m.id, m.name); !inserted)
        {
            fail(nodes[i], std::format("{} reuses the number of {}", m.name, it->second));
        }
        messages.push_back(std::move(m));
    }
    return messages;
}

// --- Code generation --------------------------------------------------------

struct TypeInfo
{
    std::string cppType;
    std::string method; // MessageWriter / MessageReader method
    std::string init;   // default member initializer
};

TypeInfo typeInfo(const Field& f)
{
    static const std::map<std::string, TypeInfo> simple = {
        {"U8", {"uint8_t", "u8", " = 0"}},
        {"U16", {"uint16_t", "u16", " = 0"}},
        {"U32", {"uint32_t", "u32", " = 0"}},
        {"U64", {"uint64_t", "u64", " = 0"}},
        {"S8", {"int8_t", "s8", " = 0"}},
        {"S16", {"int16_t", "s16", " = 0"}},
        {"S32", {"int32_t", "s32", " = 0"}},
        {"S64", {"int64_t", "s64", " = 0"}},
        {"F32", {"float", "f32", " = 0"}},
        {"F64", {"double", "f64", " = 0"}},
        {"BOOL", {"bool", "boolean", " = false"}},
        {"LLUUID", {"Uuid", "uuid", "{}"}},
        {"LLVector3", {"Vector3", "vector3", "{}"}},
        {"LLVector3d", {"Vector3d", "vector3d", "{}"}},
        {"LLVector4", {"Vector4", "vector4", "{}"}},
        {"LLQuaternion", {"Quaternion", "quaternion", "{}"}},
        {"IPADDR", {"uint32_t", "ipAddress", " = 0"}},
        {"IPPORT", {"uint16_t", "ipPort", " = 0"}},
    };
    if(f.type == "Fixed")
    {
        return {std::format("std::array<uint8_t, {}>", f.size), "fixed", "{}"};
    }
    if(f.type == "Variable")
    {
        return {"std::vector<uint8_t>", f.size == 1 ? "variable1" : "variable2", "{}"};
    }
    const auto it = simple.find(f.type);
    if(it == simple.end())
    {
        throw std::runtime_error("unknown field type " + f.type);
    }
    return it->second;
}

std::string memberName(const Message& m, const Block& b)
{
    return b.name == m.name ? b.name + "_" : b.name;
}

std::string infoInitializer(const Message& m)
{
    return std::format("MessageInfo{{0x{:08X}u, \"{}\", Frequency::{}, {}, {}, Deprecation::{}}}",
                       m.id, m.name, m.frequency, m.trusted ? "true" : "false",
                       m.zerocoded ? "true" : "false", m.deprecation);
}

std::string generateHeader(const std::vector<Message>& messages)
{
    std::ostringstream out;
    out << "// Generated from message_template.msg by viewer_msggen. Do not edit.\n"
           "#pragma once\n\n"
           "#include \"viewer/protocol/MessageTypes.hpp\"\n\n"
           "#include <array>\n#include <cstdint>\n#include <span>\n#include <vector>\n\n"
           "namespace viewer::protocol::msg\n{\n";
    for(const Message& m : messages)
    {
        out << "\nstruct " << m.name << "\n{\n";
        out << "    static constexpr MessageInfo kInfo = " << infoInitializer(m) << ";\n";
        for(const Block& b : m.blocks)
        {
            out << "\n    struct " << b.name << "Block\n    {\n";
            for(const Field& f : b.fields)
            {
                const TypeInfo t = typeInfo(f);
                out << "        " << t.cppType << " " << f.name << t.init << ";\n";
            }
            out << "    };\n";
        }
        if(!m.blocks.empty())
        {
            out << "\n";
        }
        for(const Block& b : m.blocks)
        {
            const std::string type = b.name + "Block";
            switch(b.quantity)
            {
            case Quantity::Single:
                out << "    " << type << " " << memberName(m, b) << "{};\n";
                break;
            case Quantity::Multiple:
                out << "    std::array<" << type << ", " << b.count << "> " << memberName(m, b)
                    << "{};\n";
                break;
            case Quantity::Variable:
                out << "    std::vector<" << type << "> " << memberName(m, b)
                    << "{}; // at most 255\n";
                break;
            }
        }
        out << "\n    void encode(MessageWriter& w) const;\n"
               "    bool decode(MessageReader& r);\n};\n";
    }
    out << "\n} // namespace viewer::protocol::msg\n\n"
           "namespace viewer::protocol\n{\n\n"
           "// Every message in the template, sorted by id.\n"
           "std::span<const MessageInfo> allMessages();\n"
           "const MessageInfo* findMessage(MessageId id);\n"
           "const MessageInfo* findMessage(std::string_view name);\n\n"
           "} // namespace viewer::protocol\n";
    return out.str();
}

void emitFieldIo(std::ostringstream& out, const Field& f, const std::string& object,
                 const std::string& indent, bool encode)
{
    const TypeInfo t = typeInfo(f);
    if(encode)
    {
        out << indent << "w." << t.method << "(" << object << f.name << ");\n";
    }
    else if(f.type == "Fixed")
    {
        out << indent << object << f.name << " = r.fixed<" << f.size << ">();\n";
    }
    else
    {
        out << indent << object << f.name << " = r." << t.method << "();\n";
    }
}

std::string generateSource(const std::vector<Message>& messages)
{
    std::ostringstream out;
    out << "// Generated from message_template.msg by viewer_msggen. Do not edit.\n"
           "#include \"viewer/protocol/Messages.hpp\"\n\n"
           "#include <algorithm>\n\n"
           "namespace viewer::protocol::msg\n{\n";
    for(const Message& m : messages)
    {
        // encode
        out << "\nvoid " << m.name << "::encode(MessageWriter& w) const\n{\n";
        if(m.blocks.empty())
        {
            out << "    (void)w;\n";
        }
        for(const Block& b : m.blocks)
        {
            const std::string member = memberName(m, b);
            if(b.quantity == Quantity::Single)
            {
                for(const Field& f : b.fields)
                {
                    emitFieldIo(out, f, member + ".", "    ", true);
                }
                continue;
            }
            if(b.quantity == Quantity::Variable)
            {
                out << "    const size_t " << member << "Count = std::min<size_t>(" << member
                    << ".size(), 255);\n";
                out << "    w.u8(static_cast<uint8_t>(" << member << "Count));\n";
                out << "    for(size_t i = 0; i < " << member << "Count; i++)\n    {\n";
            }
            else
            {
                out << "    for(size_t i = 0; i < " << b.count << "; i++)\n    {\n";
            }
            for(const Field& f : b.fields)
            {
                emitFieldIo(out, f, member + "[i].", "        ", true);
            }
            out << "    }\n";
        }
        out << "}\n";

        // decode
        out << "\nbool " << m.name << "::decode(MessageReader& r)\n{\n";
        for(const Block& b : m.blocks)
        {
            const std::string member = memberName(m, b);
            if(b.quantity == Quantity::Single)
            {
                for(const Field& f : b.fields)
                {
                    emitFieldIo(out, f, member + ".", "    ", false);
                }
                continue;
            }
            if(b.quantity == Quantity::Variable)
            {
                // A truncated message may end before a variable block's count;
                // the official reader treats that as zero entries.
                out << "    " << member << ".resize(r.remaining() > 0 ? r.u8() : 0);\n";
                out << "    for(auto& block : " << member << ")\n    {\n";
            }
            else
            {
                out << "    for(auto& block : " << member << ")\n    {\n";
            }
            for(const Field& f : b.fields)
            {
                emitFieldIo(out, f, "block.", "        ", false);
            }
            out << "        if(r.failed())\n        {\n            return false;\n        }\n";
            out << "    }\n";
        }
        out << "    return !r.failed();\n}\n";
    }
    out << "\n} // namespace viewer::protocol::msg\n\n"
           "namespace viewer::protocol\n{\n\n"
           "namespace\n{\n\n";

    std::vector<const Message*> byId;
    for(const Message& m : messages)
    {
        byId.push_back(&m);
    }
    std::ranges::sort(byId, {}, &Message::id);
    out << "constexpr MessageInfo kMessages[] = {\n";
    for(const Message* m : byId)
    {
        out << "    msg::" << m->name << "::kInfo,\n";
    }
    out << "};\n\n} // namespace\n\n"
           "std::span<const MessageInfo> allMessages()\n{\n    return kMessages;\n}\n\n"
           "const MessageInfo* findMessage(MessageId id)\n{\n"
           "    const auto it = std::ranges::lower_bound(kMessages, id, {}, &MessageInfo::id);\n"
           "    return it != std::end(kMessages) && it->id == id ? &*it : nullptr;\n}\n\n"
           "const MessageInfo* findMessage(std::string_view name)\n{\n"
           "    const auto it = std::ranges::find(kMessages, name, &MessageInfo::name);\n"
           "    return it != std::end(kMessages) ? &*it : nullptr;\n}\n\n"
           "} // namespace viewer::protocol\n";
    return out.str();
}

// Only rewrite when the content changed, so dependents don't rebuild.
void writeIfChanged(const std::string& path, const std::string& content)
{
    {
        std::ifstream in(path, std::ios::binary);
        if(in)
        {
            std::ostringstream existing;
            existing << in.rdbuf();
            if(existing.str() == content)
            {
                return;
            }
        }
    }
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    if(!out)
    {
        throw std::runtime_error("couldn't write " + path);
    }
}

} // namespace

int main(int argc, char** argv)
{
    if(argc != 4)
    {
        std::println(stderr, "usage: viewer_msggen <message_template.msg> <out.hpp> <out.cpp>");
        return 2;
    }
    try
    {
        std::ifstream in(argv[1], std::ios::binary);
        if(!in)
        {
            throw std::runtime_error(std::format("couldn't open {}", argv[1]));
        }
        std::ostringstream text;
        text << in.rdbuf();
        const auto messages = parseTemplate(text.str());
        writeIfChanged(argv[2], generateHeader(messages));
        writeIfChanged(argv[3], generateSource(messages));
    }
    catch(const std::exception& e)
    {
        std::println(stderr, "{}: {}", argv[1], e.what());
        return 1;
    }
    return 0;
}
