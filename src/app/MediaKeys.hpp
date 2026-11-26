#pragma once

// The keyboard's keys as pages know them: SDL's keys to Windows'
// virtual-key codes (what a page sees as keyCode, and what Chromium is told
// on every platform), and the modifiers held. Letters and digits by what
// the layout makes them (as Windows' do); punctuation by where it is, as on
// a US keyboard (Windows' OEM keys). Text itself goes separately (SDL's
// text input: the layout's and any input method's), so a key here only says
// which key it was.

#include "viewer/media/Protocol.hpp"

#include <SDL3/SDL_keyboard.h>

#include <optional>

namespace viewer
{

// Nothing for keys pages have no code for.
std::optional<media::Key> mediaKey(SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mod,
                                   bool down, bool repeat);

} // namespace viewer
