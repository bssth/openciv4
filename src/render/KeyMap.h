#pragma once

// How a key pressed on this keyboard becomes one of the game's keys: the punctuation keys by their
// place on the keyboard, and OpenCiv4's option Mac keys (the keys macOS takes first, or a Mac
// keyboard has not, moved to ones it sends). No SDL here: the tests include it on its own.

#include "render/Input.h"

#include <optional>

namespace oc4::render
{
	// The punctuation keys by their place (SDL's scancodes, the USB keyboard usages): the key right
	// of the full stop is the game's KB_SLASH whatever the layout prints on it, as the game's
	// keyboard codes are (FInputDevice.h's KB_ list follows DirectInput's DIK_ order, and
	// DirectInput's keyboard data are the keys' scan codes, not the characters; Microsoft's
	// DirectInput documentation, "Keyboard Device"). So on a Danish layout '-' cycles the workers
	// and the key left of Return (' and *) is the last unit. Letters and digits stay by what they
	// type. 0 for any other place.
	constexpr unsigned keyOfPlace(int scancode)
	{
		switch (scancode)
		{
		case 45: return key::Minus;      // SDL_SCANCODE_MINUS
		case 46: return key::Plus;       // SDL_SCANCODE_EQUALS
		case 47: return key::LBracket;   // SDL_SCANCODE_LEFTBRACKET
		case 48: return key::RBracket;   // SDL_SCANCODE_RIGHTBRACKET
		case 49: return key::Backslash;  // SDL_SCANCODE_BACKSLASH
		case 50: return key::Backslash;  // SDL_SCANCODE_NONUSHASH (ISO keyboards: the same place)
		case 51: return key::Semicolon;  // SDL_SCANCODE_SEMICOLON
		case 52: return key::Apostrophe; // SDL_SCANCODE_APOSTROPHE
		case 53: return key::Grave;      // SDL_SCANCODE_GRAVE
		case 54: return key::Comma;      // SDL_SCANCODE_COMMA
		case 55: return key::Period;     // SDL_SCANCODE_PERIOD
		case 56: return key::Slash;      // SDL_SCANCODE_SLASH
		default: return 0;
		}
	}

	// A key with the modifiers it is pressed with, as the game sees it.
	struct Chord
	{
		unsigned key = 0;
		bool shift = false, ctrl = false, alt = false;
		bool operator==(const Chord&) const = default;
	};

	// The option Mac keys (on by default on macOS; the Mac keys tab of the options lists them):
	// - Cmd is Ctrl too (Cmd+Left/Right turn the camera: macOS takes Ctrl+Left/Right to switch
	//   Spaces), but Cmd+Q, Cmd+H and Cmd+M stay macOS's (Ctrl+H still selects the healthy units);
	// - Cmd+Shift+G is F11, the globe view (macOS takes F11 for Show Desktop, with Fn too);
	// - Cmd+I is Insert, Select City (no Insert on a Mac keyboard);
	// - Option+arrows are the numpad's 8, 2, 4, 6: the selected units move a plot that way.
	// nullopt: the key is macOS's, not the game's. Off: Cmd is nothing, as before.
	inline std::optional<Chord> macKey(const Chord& c, bool cmd, bool on)
	{
		if (!on)
			return c;
		if (cmd && !c.ctrl && (c.key == 'Q' || c.key == 'H' || c.key == 'M'))
			return std::nullopt;
		const bool ctrl = c.ctrl || cmd;
		if (ctrl && c.shift && !c.alt && c.key == 'G')
			return Chord{ key::F1 + 10 };
		if (ctrl && !c.shift && !c.alt && c.key == 'I')
			return Chord{ key::Insert };
		if (c.alt && !c.shift && !ctrl)
			switch (c.key)
			{
			case key::Up: return Chord{ key::Numpad0 + 8 };
			case key::Down: return Chord{ key::Numpad0 + 2 };
			case key::Left: return Chord{ key::Numpad0 + 4 };
			case key::Right: return Chord{ key::Numpad0 + 6 };
			default: break;
			}
		return Chord{ c.key, c.shift, ctrl, c.alt };
	}
}
