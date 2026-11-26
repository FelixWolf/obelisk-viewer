#include "MediaKeys.hpp"

namespace viewer
{

namespace
{

// By what the key is (SDL's keycode).
uint32_t byKey(SDL_Keycode key)
{
    if(key >= SDLK_A && key <= SDLK_Z)
    {
        return 'A' + (key - SDLK_A);
    }
    if(key >= SDLK_0 && key <= SDLK_9)
    {
        return '0' + (key - SDLK_0);
    }
    if(key >= SDLK_F1 && key <= SDLK_F12)
    {
        return 0x70 + (key - SDLK_F1);
    }
    if(key >= SDLK_F13 && key <= SDLK_F24)
    {
        return 0x7C + (key - SDLK_F13);
    }
    switch(key)
    {
    case SDLK_BACKSPACE:
        return 0x08;
    case SDLK_TAB:
        return 0x09;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return 0x0D;
    case SDLK_LSHIFT:
    case SDLK_RSHIFT:
        return 0x10;
    case SDLK_LCTRL:
    case SDLK_RCTRL:
        return 0x11;
    case SDLK_LALT:
    case SDLK_RALT:
        return 0x12;
    case SDLK_PAUSE:
        return 0x13;
    case SDLK_CAPSLOCK:
        return 0x14;
    case SDLK_ESCAPE:
        return 0x1B;
    case SDLK_SPACE:
        return 0x20;
    case SDLK_PAGEUP:
        return 0x21;
    case SDLK_PAGEDOWN:
        return 0x22;
    case SDLK_END:
        return 0x23;
    case SDLK_HOME:
        return 0x24;
    case SDLK_LEFT:
        return 0x25;
    case SDLK_UP:
        return 0x26;
    case SDLK_RIGHT:
        return 0x27;
    case SDLK_DOWN:
        return 0x28;
    case SDLK_PRINTSCREEN:
        return 0x2C;
    case SDLK_INSERT:
        return 0x2D;
    case SDLK_DELETE:
        return 0x2E;
    case SDLK_LGUI:
        return 0x5B;
    case SDLK_RGUI:
        return 0x5C;
    case SDLK_APPLICATION:
        return 0x5D;
    case SDLK_KP_0:
    case SDLK_KP_1:
    case SDLK_KP_2:
    case SDLK_KP_3:
    case SDLK_KP_4:
    case SDLK_KP_5:
    case SDLK_KP_6:
    case SDLK_KP_7:
    case SDLK_KP_8:
    case SDLK_KP_9:
        return 0x60 + (key == SDLK_KP_0 ? 0 : 1 + (key - SDLK_KP_1));
    case SDLK_KP_MULTIPLY:
        return 0x6A;
    case SDLK_KP_PLUS:
        return 0x6B;
    case SDLK_KP_MINUS:
        return 0x6D;
    case SDLK_KP_PERIOD:
        return 0x6E;
    case SDLK_KP_DIVIDE:
        return 0x6F;
    case SDLK_NUMLOCKCLEAR:
        return 0x90;
    case SDLK_SCROLLLOCK:
        return 0x91;
    default:
        return 0;
    }
}

// Punctuation, by where it is (Windows' OEM keys, as on a US keyboard).
uint32_t byPlace(SDL_Scancode scancode)
{
    switch(scancode)
    {
    case SDL_SCANCODE_SEMICOLON:
        return 0xBA;
    case SDL_SCANCODE_EQUALS:
        return 0xBB;
    case SDL_SCANCODE_COMMA:
        return 0xBC;
    case SDL_SCANCODE_MINUS:
        return 0xBD;
    case SDL_SCANCODE_PERIOD:
        return 0xBE;
    case SDL_SCANCODE_SLASH:
        return 0xBF;
    case SDL_SCANCODE_GRAVE:
        return 0xC0;
    case SDL_SCANCODE_LEFTBRACKET:
        return 0xDB;
    case SDL_SCANCODE_BACKSLASH:
        return 0xDC;
    case SDL_SCANCODE_RIGHTBRACKET:
        return 0xDD;
    case SDL_SCANCODE_APOSTROPHE:
        return 0xDE;
    case SDL_SCANCODE_NONUSBACKSLASH:
        return 0xE2;
    default:
        return 0;
    }
}

} // namespace

std::optional<media::Key> mediaKey(SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mod,
                                   bool down, bool repeat)
{
    uint32_t code = byKey(key);
    if(code == 0)
    {
        code = byPlace(scancode);
    }
    if(code == 0)
    {
        return std::nullopt;
    }
    using namespace media::key_flags;
    uint32_t flags = 0;
    flags |= (mod & SDL_KMOD_SHIFT) ? kShift : 0;
    flags |= (mod & SDL_KMOD_CTRL) ? kControl : 0;
    flags |= (mod & SDL_KMOD_ALT) ? kAlt : 0;
    flags |= (mod & SDL_KMOD_GUI) ? kMeta : 0;
    flags |= (mod & SDL_KMOD_CAPS) ? kCapsLock : 0;
    flags |= (mod & SDL_KMOD_NUM) ? kNumLock : 0;
    flags |= (key >= SDLK_KP_DIVIDE && key <= SDLK_KP_PERIOD) ? kKeypad : 0;
    flags |= repeat ? kRepeat : 0;
    return media::Key{0, down ? media::KeyKind::Down : media::KeyKind::Up, code, flags, 0};
}

} // namespace viewer
