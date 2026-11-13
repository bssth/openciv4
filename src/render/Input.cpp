#include "render/Input.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>

#include <utility>

namespace oc4::render
{
	namespace
	{
		constexpr std::pair<SDL_Keycode, unsigned> kNamed[] = {
			{ SDLK_BACKSPACE, key::Back },
			{ SDLK_TAB, key::Tab },
			{ SDLK_RETURN, key::Return },
			{ SDLK_KP_ENTER, key::Return },
			{ SDLK_LSHIFT, key::Shift },
			{ SDLK_RSHIFT, key::Shift },
			{ SDLK_LCTRL, key::Control },
			{ SDLK_RCTRL, key::Control },
			{ SDLK_LALT, key::Menu },
			{ SDLK_RALT, key::Menu },
			{ SDLK_ESCAPE, key::Escape },
			{ SDLK_SPACE, key::Space },
			{ SDLK_PAGEUP, key::Prior },
			{ SDLK_PAGEDOWN, key::Next },
			{ SDLK_END, key::End },
			{ SDLK_HOME, key::Home },
			{ SDLK_LEFT, key::Left },
			{ SDLK_UP, key::Up },
			{ SDLK_RIGHT, key::Right },
			{ SDLK_DOWN, key::Down },
			{ SDLK_INSERT, key::Insert },
			{ SDLK_DELETE, key::Delete },
			{ SDLK_KP_0, key::Numpad0 },
			{ SDLK_KP_1, key::Numpad0 + 1 },
			{ SDLK_KP_2, key::Numpad0 + 2 },
			{ SDLK_KP_3, key::Numpad0 + 3 },
			{ SDLK_KP_4, key::Numpad0 + 4 },
			{ SDLK_KP_5, key::Numpad0 + 5 },
			{ SDLK_KP_6, key::Numpad0 + 6 },
			{ SDLK_KP_7, key::Numpad0 + 7 },
			{ SDLK_KP_8, key::Numpad0 + 8 },
			{ SDLK_KP_9, key::Numpad0 + 9 },
			{ SDLK_SEMICOLON, key::Semicolon },
			{ SDLK_EQUALS, key::Plus },
			{ SDLK_COMMA, key::Comma },
			{ SDLK_MINUS, key::Minus },
			{ SDLK_PERIOD, key::Period },
			{ SDLK_SLASH, key::Slash },
			{ SDLK_GRAVE, key::Grave },
			{ SDLK_LEFTBRACKET, key::LBracket },
			{ SDLK_BACKSLASH, key::Backslash },
			{ SDLK_RIGHTBRACKET, key::RBracket },
			{ SDLK_APOSTROPHE, key::Apostrophe },
		};

		SDL_Keycode sdlOfKey(unsigned code)
		{
			if (code >= 'A' && code <= 'Z')
				return SDLK_A + (code - 'A');
			if (code >= '0' && code <= '9')
				return SDLK_0 + (code - '0');
			if (code >= key::F1 && code <= key::F12)
				return SDLK_F1 + (code - key::F1);
			for (const auto& [sdl, k] : kNamed)
				if (k == code)
					return sdl;
			return SDLK_UNKNOWN;
		}
	}

	unsigned keyOfSdl(uint32_t keycode)
	{
		if (keycode >= SDLK_A && keycode <= SDLK_Z)
			return 'A' + (keycode - SDLK_A);
		if (keycode >= SDLK_0 && keycode <= SDLK_9)
			return '0' + (keycode - SDLK_0);
		if (keycode >= SDLK_F1 && keycode <= SDLK_F12)
			return key::F1 + (keycode - SDLK_F1);
		for (const auto& [sdl, k] : kNamed)
			if (sdl == keycode)
				return k;
		return 0;
	}

	Modifiers heldModifiers()
	{
		const SDL_Keymod mod = SDL_GetModState();
		return { (mod & SDL_KMOD_SHIFT) != 0, (mod & SDL_KMOD_CTRL) != 0, (mod & SDL_KMOD_ALT) != 0 };
	}

	bool keyHeld(unsigned code)
	{
		const Modifiers m = heldModifiers();
		if (code == key::Shift || code == key::Control || code == key::Menu)
			return code == key::Shift ? m.shift : code == key::Control ? m.ctrl : m.alt;
		const SDL_Keycode sdl = sdlOfKey(code);
		if (sdl == SDLK_UNKNOWN)
			return false;
		int count = 0;
		const bool* state = SDL_GetKeyboardState(&count);
		const SDL_Scancode sc = SDL_GetScancodeFromKey(sdl, nullptr);
		return sc != SDL_SCANCODE_UNKNOWN && int(sc) < count && state[sc];
	}

	bool padHeld[2] = {}; // (MapWindow's onPadButton)

	bool mouseHeld(bool left)
	{
		if (padHeld[left ? 0 : 1])
			return true;
		return (SDL_GetMouseState(nullptr, nullptr) & (left ? SDL_BUTTON_LMASK : SDL_BUTTON_RMASK)) != 0;
	}
}
