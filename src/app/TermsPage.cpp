#include "TermsPage.hpp"

#include "viewer/session/Login.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <utility>

namespace viewer
{

namespace
{

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string lower(std::string_view s)
{
    std::string out(s);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

void appendUtf8(std::string& out, uint32_t c)
{
    if(c == 0xA0)
    {
        out += ' ';
    }
    else if(c < 0x80)
    {
        out += static_cast<char>(c);
    }
    else if(c < 0x800)
    {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else if(c < 0x10000)
    {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else
    {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// Entities decoded (those not known are left as written).
std::string decode(std::string_view s)
{
    static constexpr std::pair<std::string_view, uint32_t> kNamed[] = {
        {"amp", '&'},      {"lt", '<'},       {"gt", '>'},       {"quot", '"'},
        {"apos", '\''},    {"nbsp", 0xA0},    {"copy", 0xA9},    {"reg", 0xAE},
        {"sect", 0xA7},    {"ndash", 0x2013}, {"mdash", 0x2014}, {"lsquo", 0x2018},
        {"rsquo", 0x2019}, {"ldquo", 0x201C}, {"rdquo", 0x201D}, {"bull", 0x2022},
        {"trade", 0x2122}};
    std::string out;
    for(size_t i = 0; i < s.size(); i++)
    {
        const size_t end = s[i] == '&' ? s.find(';', i) : std::string_view::npos;
        if(end == std::string_view::npos || end - i > 10)
        {
            out += s[i];
            continue;
        }
        const std::string_view name = s.substr(i + 1, end - i - 1);
        uint32_t code = 0;
        if(name.starts_with('#'))
        {
            const bool hex = name.size() > 1 && (name[1] == 'x' || name[1] == 'X');
            const std::string_view digits = name.substr(hex ? 2 : 1);
            const auto [p, error] =
                std::from_chars(digits.data(), digits.data() + digits.size(), code, hex ? 16 : 10);
            if(error != std::errc{} || p != digits.data() + digits.size() || code > 0x10FFFF ||
               (code >= 0xD800 && code < 0xE000))
            {
                code = 0;
            }
        }
        else
        {
            for(const auto& [known, value] : kNamed)
            {
                if(name == known)
                {
                    code = value;
                }
            }
        }
        if(code == 0)
        {
            out += s[i];
            continue;
        }
        appendUtf8(out, code);
        i = end;
    }
    return out;
}

// A tag's attribute (`tag`: what's between its name and >).
std::string attribute(std::string_view tag, std::string_view wanted)
{
    size_t i = 0;
    while(i < tag.size())
    {
        while(i < tag.size() && (isSpace(tag[i]) || tag[i] == '/'))
        {
            i++;
        }
        const size_t start = i;
        while(i < tag.size() && !isSpace(tag[i]) && tag[i] != '=')
        {
            i++;
        }
        const std::string name = lower(tag.substr(start, i - start));
        while(i < tag.size() && isSpace(tag[i]))
        {
            i++;
        }
        std::string_view value;
        if(i < tag.size() && tag[i] == '=')
        {
            i++;
            while(i < tag.size() && isSpace(tag[i]))
            {
                i++;
            }
            if(i < tag.size() && (tag[i] == '"' || tag[i] == '\''))
            {
                const char quote = tag[i++];
                const size_t end = std::min(tag.find(quote, i), tag.size());
                value = tag.substr(i, end - i);
                i = std::min(end + 1, tag.size());
            }
            else
            {
                const size_t begin = i;
                while(i < tag.size() && !isSpace(tag[i]))
                {
                    i++;
                }
                value = tag.substr(begin, i - begin);
            }
        }
        if(name == wanted)
        {
            return decode(value);
        }
    }
    return {};
}

// Where a tag starting at `from` ends (its >, not one in quotes).
size_t tagEnd(std::string_view html, size_t from)
{
    char quote = 0;
    for(size_t i = from; i < html.size(); i++)
    {
        const char c = html[i];
        if(quote != 0)
        {
            quote = c == quote ? '\0' : quote;
        }
        else if(c == '"' || c == '\'')
        {
            quote = c;
        }
        else if(c == '>')
        {
            return i;
        }
    }
    return html.size();
}

class Builder
{
public:
    std::vector<TermsBlock> blocks;
    std::string link;
    std::string prefix; // a list item's number, before its text
    int bold = 0;
    int heading = 0;
    int item = 0;
    bool toTop = false; // in a link back up the page: nothing to read

    void text(std::string_view raw)
    {
        if(toTop)
        {
            return;
        }
        for(const char c : decode(raw))
        {
            if(isSpace(c))
            {
                space_ = space_ || !current_.spans.empty();
                continue;
            }
            if(current_.spans.empty())
            {
                current_.heading = heading > 0;
                current_.item = item > 0;
                if(!prefix.empty())
                {
                    current_.spans.push_back({std::exchange(prefix, {}), {}, bold > 0});
                }
            }
            if(current_.spans.empty() || current_.spans.back().link != link ||
               current_.spans.back().bold != (bold > 0))
            {
                // The space between goes with what's before.
                if(space_ && !current_.spans.empty())
                {
                    current_.spans.back().text += ' ';
                }
                space_ = false;
                current_.spans.push_back({{}, link, bold > 0});
            }
            else if(space_)
            {
                current_.spans.back().text += ' ';
            }
            space_ = false;
            current_.spans.back().text += c;
        }
    }

    void end()
    {
        if(!current_.spans.empty())
        {
            blocks.push_back(std::move(current_));
        }
        current_ = {};
        space_ = false;
    }

private:
    TermsBlock current_;
    bool space_ = false;
};

void step(int& depth, bool closing)
{
    depth = closing ? std::max(0, depth - 1) : depth + 1;
}

} // namespace

bool usesLindenTerms(std::string_view loginUri)
{
    return loginUri == session::kAgniLoginUri || loginUri == session::kAditiLoginUri;
}

std::vector<TermsBlock> parseTerms(std::string_view html)
{
    Builder b;
    std::vector<int> lists; // the next number of each list we're in; 0: bullets
    size_t i = 0;
    while(i < html.size())
    {
        if(html[i] != '<')
        {
            const size_t end = std::min(html.find('<', i), html.size());
            b.text(html.substr(i, end - i));
            i = end;
            continue;
        }
        if(html.substr(i).starts_with("<!--"))
        {
            const size_t end = html.find("-->", i);
            i = end == std::string_view::npos ? html.size() : end + 3;
            continue;
        }
        const bool closing = i + 1 < html.size() && html[i + 1] == '/';
        const size_t start = i + (closing ? 2 : 1);
        size_t nameEnd = start;
        while(nameEnd < html.size() && std::isalnum(static_cast<unsigned char>(html[nameEnd])))
        {
            nameEnd++;
        }
        const std::string name = lower(html.substr(start, nameEnd - start));
        if(name.empty() && !closing && (start >= html.size() || html[start] != '!'))
        {
            b.text("<"); // not a tag: text
            i++;
            continue;
        }
        const size_t end = tagEnd(html, nameEnd);
        const std::string_view rest = html.substr(nameEnd, end - nameEnd);
        i = std::min(end + 1, html.size());

        if(!closing && (name == "script" || name == "style" || name == "head" || name == "title"))
        {
            // Nothing in these is shown.
            const std::string close = "</" + name;
            const auto found =
                std::ranges::search(html.substr(i), close, [](unsigned char x, unsigned char y)
                                    { return std::tolower(x) == y; });
            const size_t at = static_cast<size_t>(found.begin() - html.substr(i).begin()) + i;
            i = std::min(tagEnd(html, at) + 1, html.size());
        }
        else if(name == "a")
        {
            const std::string href = closing ? std::string() : attribute(rest, "href");
            b.link = href.starts_with("https://") ? href : std::string();
            b.toTop = href == "#top";
        }
        else if(name == "b" || name == "strong")
        {
            step(b.bold, closing);
        }
        else if(name.size() == 2 && name[0] == 'h' && name[1] >= '1' && name[1] <= '6')
        {
            b.end();
            step(b.heading, closing);
        }
        else if(name == "ol" || name == "ul")
        {
            b.end();
            if(!closing)
            {
                lists.push_back(name == "ol" ? 1 : 0);
            }
            else if(!lists.empty())
            {
                lists.pop_back();
            }
        }
        else if(name == "li")
        {
            b.end();
            step(b.item, closing);
            b.prefix.clear();
            if(!closing)
            {
                const bool numbered = !lists.empty() && lists.back() > 0;
                b.prefix = numbered ? std::to_string(lists.back()++) + ". " : "\xE2\x80\xA2 ";
            }
        }
        else if(name == "p" || name == "div" || name == "br" || name == "tr" || name == "table" ||
                name == "section" || name == "article" || name == "body" || name == "blockquote")
        {
            b.end();
        }
    }
    b.end();
    return std::move(b.blocks);
}

bool plausibleTerms(const std::vector<TermsBlock>& blocks)
{
    size_t length = 0;
    for(const TermsBlock& block : blocks)
    {
        for(const TermsSpan& span : block.spans)
        {
            length += span.text.size();
        }
    }
    return length > 2000;
}

} // namespace viewer
