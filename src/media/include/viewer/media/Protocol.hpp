#pragma once

// What the viewer and the media host (its own program, src/media/host) say
// to each other, over the host's standard input and output. Each message is
// its length (4 bytes, little-endian: the type and what follows), its type
// (1 byte), then its fields in order: integers little-endian, floats as
// their bits, strings as a length then UTF-8. Frames don't go this way:
// each tab's pixels are in shared memory (SharedMemory.hpp), and a Frame
// message says which slot is ready.
//
// Nothing here knows CEF: another engine could stand behind the same
// messages.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace viewer::media
{

// Both sides check it (Init, Ready); a host that speaks another isn't used.
inline constexpr uint32_t kProtocolVersion = 2;
// Longer than this is taken to be garbage: the stream is given up.
inline constexpr uint32_t kMaxMessage = 16u << 20;

using TabId = uint32_t;

// --- The viewer's -------------------------------------------------------------

// First, once: how the host starts its engine.
struct Init
{
    uint32_t version = kProtocolVersion;
    std::string root; // the engine's own folder (profiles, cache, its log)
    bool sandbox = true;
    bool allowData = false; // data: pages too (tests only)
};
// A tab: a page drawn at width x height into `memory` (SharedMemory's name,
// made by the viewer for this size: frameMemorySize). `profile` names its
// cookies and storage (several tabs may share one).
struct Open
{
    TabId tab = 0;
    std::string profile;
    std::string url;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string memory;
    uint32_t frameRate = 30;
};
struct Navigate
{
    TabId tab = 0;
    std::string url;
};
// Another size, in new memory (the old is the viewer's to drop once it's
// closed: Resized says so).
struct Resize
{
    TabId tab = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string memory;
};
struct Close
{
    TabId tab = 0;
};
enum class MouseKind : uint8_t
{
    Move,
    Down,
    Up,
    Leave,
};
enum class MouseButton : uint8_t
{
    Left,
    Middle,
    Right,
};
// In the page's pixels, from its top left.
struct Mouse
{
    TabId tab = 0;
    MouseKind kind = MouseKind::Move;
    int32_t x = 0;
    int32_t y = 0;
    MouseButton button = MouseButton::Left;
    uint8_t clicks = 1;
};
// The wheel at a point: 120 a notch (Windows' wheel delta); dx > 0 scrolls
// right, dy > 0 up.
struct Wheel
{
    TabId tab = 0;
    int32_t x = 0;
    int32_t y = 0;
    int32_t dx = 0; // pixels
    int32_t dy = 0;
};
// Hidden: not drawn (audio still plays). The rate it's drawn at.
struct Visibility
{
    TabId tab = 0;
    bool hidden = false;
    uint32_t frameRate = 30;
};
// The viewer is done with a frame's slot.
struct Release
{
    TabId tab = 0;
    uint32_t slot = 0;
};
// Close everything and exit (the host also exits when its input ends).
struct Quit
{
};

// The keyboard: the page that has it (others lose it).
struct Focus
{
    TabId tab = 0;
    bool focused = false;
};
// Modifier keys held, and what else a key is (Key::modifiers).
namespace key_flags
{
inline constexpr uint32_t kShift = 1;
inline constexpr uint32_t kControl = 2;
inline constexpr uint32_t kAlt = 4;
inline constexpr uint32_t kMeta = 8;
inline constexpr uint32_t kKeypad = 16; // on the number pad
inline constexpr uint32_t kRepeat = 32; // held down, repeating
inline constexpr uint32_t kCapsLock = 64;
inline constexpr uint32_t kNumLock = 128;
} // namespace key_flags
enum class KeyKind : uint8_t
{
    Down,
    Up,
    // What a key types, for those whose meaning is in it (Enter, Tab): text
    // goes as Text.
    Char,
};
// A key, as Windows names them (virtual-key codes: what pages see as
// keyCode), whatever the platform.
struct Key
{
    TabId tab = 0;
    KeyKind kind = KeyKind::Down;
    uint32_t keyCode = 0;
    uint32_t modifiers = 0;
    uint32_t character = 0; // Char: the UTF-16 unit typed
};
// Text typed (after the keyboard's layout and any input method), into
// whatever on the page has the keyboard.
struct Text
{
    TabId tab = 0;
    std::string text;
};

// --- The host's ---------------------------------------------------------------

struct Ready
{
    uint32_t version = kProtocolVersion;
    std::string engine; // its name and version, for the log
};
// The engine wouldn't start; the host exits after this.
struct Failed
{
    std::string reason;
};
// A frame is in this slot of the tab's memory, `width` x `height`.
struct Frame
{
    TabId tab = 0;
    uint32_t slot = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};
// The tab's memory of before a Resize is no longer used.
struct Resized
{
    TabId tab = 0;
};
// The page now shown: on going anywhere (a link, a redirect, a script) the
// viewer checks it, and may close the tab.
struct Address
{
    TabId tab = 0;
    std::string url;
};
struct Title
{
    TabId tab = 0;
    std::string title;
};
struct Loading
{
    TabId tab = 0;
    bool loading = false;
};
struct AudioStart
{
    TabId tab = 0;
    uint32_t sampleRate = 0;
    uint32_t channels = 2;
};
// Interleaved samples, `channels` of them a frame.
struct Audio
{
    TabId tab = 0;
    std::vector<float> samples;
};
struct AudioStop
{
    TabId tab = 0;
};
// Gone, as asked (Close) or by itself.
struct Closed
{
    TabId tab = 0;
};
// The page's process died; the tab stays, showing nothing new.
struct Crashed
{
    TabId tab = 0;
};
// Whether something on the page that's typed into has the keyboard (a
// text field): an on-screen keyboard is wanted (a headset's).
struct Editing
{
    TabId tab = 0;
    bool editing = false;
};
// What's selected on the page now (for copying: the host's clipboard isn't
// the system's). Long selections are cut at kMaxSelection.
struct Selection
{
    TabId tab = 0;
    std::string text;
};
inline constexpr size_t kMaxSelection = 1u << 20;

// (New ones at the end: a message's type is its place here.)
using Message =
    std::variant<Init, Open, Navigate, Resize, Close, Mouse, Wheel, Visibility, Release, Quit,
                 Ready, Failed, Frame, Resized, Address, Title, Loading, AudioStart, Audio,
                 AudioStop, Closed, Crashed, Focus, Key, Text, Editing, Selection>;

// A message as it's sent: length, type, fields.
std::vector<uint8_t> encode(const Message& message);
// The type and fields (after the length); nothing if they don't make one.
std::optional<Message> decode(std::span<const uint8_t> body);

// Messages out of a stream of bytes, however they arrive.
class Parser
{
public:
    void feed(std::span<const uint8_t> bytes);
    // The next whole message; nothing until one is in. Garbage (one that
    // doesn't decode, or is too long) makes broken() true.
    std::optional<Message> next();
    bool broken() const { return broken_; }

private:
    std::vector<uint8_t> buffer_;
    size_t at_ = 0;
    bool broken_ = false;
};

// A tab's shared memory: a header, then two frames' slots of BGRA pixels,
// rows from the top.
inline constexpr size_t kFrameHeader = 64;
inline constexpr uint32_t kFrameSlots = 2;
inline constexpr uint32_t kMaxFrameSide = 4096;
constexpr size_t frameSlotSize(uint32_t width, uint32_t height)
{
    return size_t{width} * height * 4;
}
constexpr size_t frameMemorySize(uint32_t width, uint32_t height)
{
    return kFrameHeader + kFrameSlots * frameSlotSize(width, height);
}
constexpr size_t frameSlotOffset(uint32_t slot, uint32_t width, uint32_t height)
{
    return kFrameHeader + slot * frameSlotSize(width, height);
}

} // namespace viewer::media
