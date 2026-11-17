#include "VrKeyboard.hpp"

#include "PanelGeometry.hpp"

#include <algorithm>

namespace viewer
{

namespace
{

// The layout, in panel pixels: the text line, then rows of keys.
constexpr float kKey = 64.0f; // a key's pitch
constexpr float kGap = 6.0f;  // between keys
constexpr float kMargin = 14.0f;
constexpr float kTextHeight = 64.0f;
constexpr int kColumns = 12;
constexpr float kWidth = kMargin * 2.0f + kKey * kColumns;
constexpr float kHeight = kMargin * 3.0f + kTextHeight + kKey * 5.0f;
constexpr float kMetersPerPixel = 0.0006f; // about 45 cm across
constexpr float kKeyCorner = 8.0f;
constexpr float kKeyPixels = 26.0f;
constexpr float kTextPixels = 26.0f;

const glm::vec4 kBackground{0.05f, 0.06f, 0.08f, 0.85f};
const glm::vec4 kKeyColor{0.16f, 0.18f, 0.24f, 0.95f};
const glm::vec4 kActionKeyColor{0.12f, 0.14f, 0.19f, 0.95f};
const glm::vec4 kHoverColor{0.22f, 0.45f, 0.85f, 1.0f};
const glm::vec4 kOnColor{0.30f, 0.38f, 0.55f, 1.0f};
const glm::vec4 kFieldColor{0.0f, 0.0f, 0.0f, 0.6f};
const glm::vec4 kTextColor{1.0f, 1.0f, 1.0f, 1.0f};

std::vector<VrKeyboard::Key> layOut()
{
    using Kind = VrKeyboard::Key::Kind;
    std::vector<VrKeyboard::Key> keys;
    const float top = kMargin * 2.0f + kTextHeight;
    // A row's keys, `indent` keys in; characters as lower/upper pairs.
    auto row = [&](int index, float indent, std::string_view lower, std::string_view upper)
    {
        for(size_t i = 0; i < lower.size(); i++)
        {
            VrKeyboard::Key k;
            k.lower = std::string(1, lower[i]);
            k.upper = std::string(1, upper[i]);
            k.topLeft = {kMargin + (indent + static_cast<float>(i)) * kKey,
                         top + static_cast<float>(index) * kKey};
            k.extent = glm::vec2(kKey - kGap);
            keys.push_back(k);
        }
    };
    auto special = [&](Kind kind, std::string label, int index, float column, float width)
    {
        VrKeyboard::Key k;
        k.kind = kind;
        k.lower = k.upper = std::move(label);
        k.topLeft = {kMargin + column * kKey, top + static_cast<float>(index) * kKey};
        k.extent = {width * kKey - kGap, kKey - kGap};
        keys.push_back(k);
    };
    row(0, 0.0f, "1234567890-", "!@#$%^&*()_");
    row(1, 0.0f, "qwertyuiop/", "QWERTYUIOP?");
    row(2, 0.25f, "asdfghjkl;'", "ASDFGHJKL:\"");
    special(Kind::Shift, "Shift", 3, 0.0f, 1.5f);
    row(3, 1.5f, "zxcvbnm,.", "ZXCVBNM<>");
    special(Kind::Backspace, "Del", 3, 10.5f, 1.5f);
    special(Kind::Close, "Close", 4, 0.0f, 2.0f);
    special(Kind::Space, "", 4, 2.0f, 6.0f);
    special(Kind::Enter, "Send", 4, 8.0f, 4.0f);
    return keys;
}

} // namespace

const std::vector<VrKeyboard::Key>& VrKeyboard::keys() const
{
    static const std::vector<Key> kKeys = layOut();
    return kKeys;
}

void VrKeyboard::open(const glm::mat4& frame)
{
    frame_ = frame;
    open_ = true;
    hovered_ = {};
}

void VrKeyboard::close()
{
    open_ = false;
    hovered_ = {};
}

std::optional<size_t> VrKeyboard::keyAt(glm::vec2 pixel) const
{
    const std::vector<Key>& all = keys();
    for(size_t i = 0; i < all.size(); i++)
    {
        const Key& k = all[i];
        // The gaps count as the key before them: no dead strips.
        const glm::vec2 end = k.topLeft + k.extent + glm::vec2(kGap);
        if(pixel.x >= k.topLeft.x && pixel.y >= k.topLeft.y && pixel.x < end.x && pixel.y < end.y)
        {
            return i;
        }
    }
    return std::nullopt;
}

std::optional<glm::vec3> VrKeyboard::point(int pointer, const glm::vec3& origin,
                                           const glm::vec3& direction)
{
    if(!open_ || pointer < 0 || pointer >= kPointers)
    {
        return std::nullopt;
    }
    const auto hit = panelHit(frame_, {kWidth, kHeight}, kMetersPerPixel, origin, direction);
    hovered_[static_cast<size_t>(pointer)] = hit ? keyAt(hit->pixel) : std::nullopt;
    return hit ? std::optional(hit->position) : std::nullopt;
}

void VrKeyboard::clearPoint(int pointer)
{
    if(pointer >= 0 && pointer < kPointers)
    {
        hovered_[static_cast<size_t>(pointer)].reset();
    }
}

bool VrKeyboard::press(int pointer)
{
    if(!open_ || pointer < 0 || pointer >= kPointers || !hovered_[static_cast<size_t>(pointer)])
    {
        return false;
    }
    pressKey(*hovered_[static_cast<size_t>(pointer)]);
    return true;
}

void VrKeyboard::pressKey(size_t index)
{
    const std::vector<Key>& all = keys();
    if(index >= all.size())
    {
        return;
    }
    const Key& k = all[index];
    switch(k.kind)
    {
    case Key::Kind::Character:
        text_ += shift_ ? k.upper : k.lower;
        shift_ = false; // for one key
        break;
    case Key::Kind::Shift:
        shift_ = !shift_;
        break;
    case Key::Kind::Backspace:
        // A whole UTF-8 character (its continuation bytes too).
        while(!text_.empty())
        {
            const auto byte = static_cast<unsigned char>(text_.back());
            text_.pop_back();
            if((byte & 0xC0) != 0x80)
            {
                break;
            }
        }
        break;
    case Key::Kind::Space:
        text_ += ' ';
        break;
    case Key::Kind::Enter:
        if(!text_.empty() && submit_)
        {
            submit_(text_);
        }
        text_.clear();
        shift_ = false;
        close();
        break;
    case Key::Kind::Close:
        close();
        break;
    }
}

render::Panel VrKeyboard::panel() const
{
    render::Panel p;
    p.frame = frame_;
    p.size = {kWidth, kHeight};
    p.metersPerPixel = kMetersPerPixel;
    p.boxes.push_back({glm::vec2(0.0f), p.size, kBackground, 16.0f});
    // The text so far (its end, if it's long) and a caret.
    p.boxes.push_back(
        {glm::vec2(kMargin), {kWidth - kMargin * 2.0f, kTextHeight}, kFieldColor, 8.0f});
    constexpr size_t kShown = 40;
    std::string shown = text_.size() > kShown ? "..." + text_.substr(text_.size() - kShown) : text_;
    p.texts.push_back({shown + "_",
                       {kWidth * 0.5f, kMargin + kTextHeight * 0.5f},
                       kTextPixels,
                       kTextColor,
                       kWidth - kMargin * 4.0f});
    const std::vector<Key>& all = keys();
    for(size_t i = 0; i < all.size(); i++)
    {
        const Key& k = all[i];
        const bool hot =
            std::find(hovered_.begin(), hovered_.end(), std::optional(i)) != hovered_.end();
        glm::vec4 color = k.kind == Key::Kind::Character ? kKeyColor : kActionKeyColor;
        if(k.kind == Key::Kind::Shift && shift_)
        {
            color = kOnColor;
        }
        p.boxes.push_back({k.topLeft, k.extent, hot ? kHoverColor : color, kKeyCorner});
        p.texts.push_back({shift_ ? k.upper : k.lower, k.topLeft + k.extent * 0.5f, kKeyPixels,
                           kTextColor, 0.0f});
    }
    return p;
}

} // namespace viewer
