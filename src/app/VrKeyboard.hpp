#pragma once

// The VR keyboard: a panel of keys under a line of text, floating where
// it's opened, typed on with pointers (both controllers' rays, or the
// mouse). Enter hands the text on (to local chat) and closes it.

#include "viewer/render/Scene.hpp"

#include <glm/glm.hpp>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace viewer
{

class VrKeyboard
{
public:
    using Submit = std::function<void(const std::string& text)>;
    static constexpr int kPointers = 2;

    void setSubmit(Submit submit) { submit_ = std::move(submit); }

    void open(const glm::mat4& frame);
    void close();
    bool isOpen() const { return open_; }
    void moveTo(const glm::mat4& frame) { frame_ = frame; }
    const glm::mat4& frame() const { return frame_; }

    // A pointer's ray: where it meets the keyboard, if it does (and the
    // key it's on is lit).
    std::optional<glm::vec3> point(int pointer, const glm::vec3& origin,
                                   const glm::vec3& direction);
    void clearPoint(int pointer);
    // The pointer's button: the key it's on is pressed.
    bool press(int pointer);

    const std::string& text() const { return text_; }
    bool shifted() const { return shift_; }

    // The keys (as laid out) and pressing one by its index (tests).
    struct Key
    {
        enum class Kind
        {
            Character,
            Shift,
            Backspace,
            Space,
            Enter,
            Close,
        };
        Kind kind = Kind::Character;
        std::string lower; // what a character key types, and shifted
        std::string upper;
        glm::vec2 topLeft{0.0f}; // panel pixels
        glm::vec2 extent{0.0f};
    };
    const std::vector<Key>& keys() const;
    void pressKey(size_t index);

    render::Panel panel() const;

private:
    std::optional<size_t> keyAt(glm::vec2 pixel) const;

    Submit submit_;
    bool open_ = false;
    glm::mat4 frame_{1.0f};
    std::string text_;
    bool shift_ = false;
    std::array<std::optional<size_t>, kPointers> hovered_{};
};

} // namespace viewer
