#pragma once

#include <cstdint>

namespace oc4::render
{
	// Keys by the codes Windows gives them (letters and digits their capitals' and digits' ASCII), on every platform.
	namespace key
	{
		constexpr unsigned Back = 0x08, Tab = 0x09, Return = 0x0D, Shift = 0x10, Control = 0x11, Menu = 0x12, Escape = 0x1B, Space = 0x20;
		constexpr unsigned Prior = 0x21, Next = 0x22, End = 0x23, Home = 0x24, Left = 0x25, Up = 0x26, Right = 0x27, Down = 0x28;
		constexpr unsigned Insert = 0x2D, Delete = 0x2E, Numpad0 = 0x60, Numpad9 = 0x69, F1 = 0x70, F12 = 0x7B;
		constexpr unsigned Semicolon = 0xBA, Plus = 0xBB, Comma = 0xBC, Minus = 0xBD, Period = 0xBE, Slash = 0xBF, Grave = 0xC0;
		constexpr unsigned LBracket = 0xDB, Backslash = 0xDC, RBracket = 0xDD, Apostrophe = 0xDE;
	}

	struct Modifiers
	{
		bool shift = false, ctrl = false, alt = false;
	};

	Modifiers heldModifiers();
	bool keyHeld(unsigned key);
	bool mouseHeld(bool left);
	extern bool padHeld[2]; // (a gamepad's buttons held as the mouse's: left, right; PadMap.h)

	// The key's code above of an SDL keycode; 0 none.
	unsigned keyOfSdl(uint32_t keycode);
}
