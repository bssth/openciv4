#pragma once

// Playing with a gamepad: the left stick moves a pointer drawn over the
// interface, which is the mouse to everything under it; the right stick pans the map (and scrolls
// lists); the buttons are the mouse's two, the game's commonest keys, and the D-pad chooses among a
// popup's or a screen's widgets as the arrows do after Tab.
// Buttons by their letters (MapWindow's padButtonOfSdl): A is the controller's confirm button, B its back.
//
// Three more ways need no pointer (both sticks pressed together goes round them): a reticle in the
// middle of the screen that the map moves under; the same with the map stepping plot by plot; and
// the units alone, the camera only looking on. In these a flick of a stick moves the selected unit
// one plot (the game's numpad keys), the D-pad walks the unit's action buttons, and Minus lists the
// advisors.
//
// A fifth way is Civilization VI's on a console: the left stick steps a plot cursor over a map that
// stays still (it follows only at the screen's edge), the right stick is the camera, A selects what is
// under the cursor or sends the selected unit there (the way there shown all the while), the D-pad's
// up and down reach the plot's other units, L and R bring up lists of screens, Minus goes to the next
// city, and the right stick pressed goes to the pointer and back.
// Without a pointer B only pressed lets the selection go; held, it is the right button (the units go
// there when it is let go).

#include "render/Input.h"
#include "render/KeyNav.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

namespace oc4::render
{
	enum class PadButton
	{
		A, B, X, Y, L, R, Plus, Minus, StickL, StickR, Up, Down, Left, Right
	};

	enum class PadMode
	{
		Pointer, // the left stick moves a pointer
		Reticle, // the left stick moves the map under the screen's middle
		Tile,    // the same, a plot at a time
		Unit,    // the left stick moves the selected unit; the right one the map
		Cursor,  // the left stick steps a plot cursor; the right one the map
	};

	inline PadMode padNextMode(PadMode m) { return m == PadMode::Cursor ? PadMode::Pointer : PadMode(int(m) + 1); }

	inline const wchar_t* padModeName(PadMode m)
	{
		switch (m)
		{
		case PadMode::Pointer: return L"Pointer";
		case PadMode::Reticle: return L"Centre reticle";
		case PadMode::Tile: return L"Plot by plot";
		case PadMode::Unit: return L"Units only";
		default: return L"Plot cursor";
		}
	}

	struct PadContext
	{
		bool choice = false;  // the D-pad has chosen a widget (a popup's or a screen's)
		bool overMap = false; // the pointer (the screen's middle without one) is on the map, nothing over it there and no popup up
		bool pointer = true;  // PadMode::Pointer
		bool editing = false; // a popup's edit box has the focus
		bool cursor = false;  // PadMode::Cursor
	};

	struct PadAction
	{
		enum class Kind
		{
			None,
			MouseLeft,  // held as the left button, at the pointer
			MouseRight, // held as the right button (on the map: the way there shown, let go the units go)
			Key,        // a press of `key`
			Choose,     // use what the D-pad chose
			Move,       // the D-pad's choice moves `direction`
			Keyboard,   // the software keyboard, for the edit box that has the focus
			Menu,       // a list of screens: kPadMenus[menu]
			PlotAct,    // at the plot cursor: one's own unit or city there selected, else the selected unit sent there
			TapOrHold,  // let go soon: nothing selected any more; held: MouseRight from then on
		};
		Kind kind = Kind::None;
		unsigned key = 0;
		NavDirection direction = NavDirection::Up;
		int menu = 0;
	};

	// What a button does when pressed.
	inline PadAction padAction(PadButton b, const PadContext& c)
	{
		using Kind = PadAction::Kind;
		switch (b)
		{
		case PadButton::A:
			if (c.choice)
				return { Kind::Choose };
			// (without a pointer there is nothing to click off the map: the D-pad's choice starts)
			if (c.cursor && c.overMap)
				return { Kind::PlotAct };
			return c.pointer || c.overMap ? PadAction{ Kind::MouseLeft } : PadAction{ Kind::Move, 0, NavDirection::Down };
		// (off the map there is little for the right button, and B is "back" on a console)
		case PadButton::B: return c.overMap ? PadAction{ c.pointer ? Kind::MouseRight : Kind::TapOrHold } : PadAction{ Kind::Key, key::Escape };
		case PadButton::X: return { Kind::Key, key::Return };    // end the turn, or the next unit waiting; a popup's OK
		case PadButton::Y: return { Kind::Key, key::Space };     // the unit skips its turn
		// (with the plot cursor: two lists of screens, as Civilization VI's L and R)
		case PadButton::L: return c.cursor ? PadAction{ Kind::Menu, 0, NavDirection::Up, 1 } : PadAction{ Kind::Key, key::Comma };  // the unit before
		case PadButton::R: return c.cursor ? PadAction{ Kind::Menu, 0, NavDirection::Up, 2 } : PadAction{ Kind::Key, key::Period }; // the next unit
		case PadButton::Plus: return { Kind::Key, key::Escape }; // the game's menu; out of a screen
		case PadButton::Minus: // (the plot cursor has L and R for the lists: the next city)
			return c.editing ? PadAction{ Kind::Keyboard } : c.cursor ? PadAction{ Kind::Key, key::Home } : PadAction{ Kind::Menu };
		case PadButton::StickL: return { Kind::Key, 'C' };       // look at the selected unit
		case PadButton::StickR: return { Kind::Key, key::Home }; // the next city
		case PadButton::Up: return { Kind::Move, 0, NavDirection::Up };
		case PadButton::Down: return { Kind::Move, 0, NavDirection::Down };
		case PadButton::Left: return { Kind::Move, 0, NavDirection::Left };
		case PadButton::Right: return { Kind::Move, 0, NavDirection::Right };
		}
		return {};
	}

	struct PadVector
	{
		float x = 0, y = 0;
	};

	// A stick's two axes (-32768..32767) as a push of 0..1 that way: nothing inside the dead zone
	// (a worn Joy-Con rests off centre), then from 0 at its edge, squared so small pushes are fine ones.
	inline PadVector padStick(int rawX, int rawY, float deadZone = 0.2f)
	{
		const float x = std::clamp(float(rawX) / 32767, -1.0f, 1.0f), y = std::clamp(float(rawY) / 32767, -1.0f, 1.0f);
		const float length = std::min(1.0f, std::sqrt(x * x + y * y));
		if (length <= deadZone)
			return {};
		const float push = (length - deadZone) / (1 - deadZone);
		const float scale = push * push / std::sqrt(x * x + y * y);
		return { x * scale, y * scale };
	}

	struct PadCursorStep
	{
		float x = 0, y = 0; // where the pointer is now
		PadVector edge;     // pushed on against the screen's edge: the map pans that way
	};

	// The pointer after `dt` seconds of the stick's push, kept on a screen of width by height.
	inline PadCursorStep padCursorStep(float x, float y, PadVector stick, float dt, float width, float height, float speed = 900)
	{
		PadCursorStep s;
		const float nx = x + stick.x * speed * dt, ny = y + stick.y * speed * dt;
		s.x = std::clamp(nx, 0.0f, std::max(0.0f, width - 1));
		s.y = std::clamp(ny, 0.0f, std::max(0.0f, height - 1));
		if (nx != s.x)
			s.edge.x = stick.x;
		if (ny != s.y)
			s.edge.y = stick.y;
		return s;
	}

	// A held D-pad direction repeats: after `delay` seconds, then every `interval`. How many
	// repeats fall in the frame that takes the hold from `before` to `after` seconds.
	inline int padRepeats(float before, float after, float delay = 0.4f, float interval = 0.12f)
	{
		const auto count = [&](float t) { return t < delay ? 0 : 1 + int((t - delay) / interval); };
		return std::max(0, count(after) - count(before));
	}

	// The way a stick is pushed as the numpad's digit that way (8 up, 6 right, 2 down, 4 left, the
	// corners between); 0 until it is pushed well out.
	inline int padEightWay(int rawX, int rawY, float threshold = 0.6f)
	{
		const float x = float(rawX) / 32767, y = float(rawY) / 32767;
		if (x * x + y * y < threshold * threshold)
			return 0;
		constexpr int kDigits[8] = { 6, 3, 2, 1, 4, 7, 8, 9 }; // (clockwise from the right; y is down)
		constexpr float kPi = 3.14159265f;
		const int sector = int(std::floor((std::atan2(y, x) + kPi / 8) / (kPi / 4)));
		return kDigits[(sector % 8 + 8) % 8];
	}

	// The plots a numpad digit moves by (east, north).
	inline std::pair<int, int> padDigitStep(int digit)
	{
		if (digit < 1 || digit > 9)
			return { 0, 0 };
		return { (digit - 1) % 3 - 1, (digit - 1) / 3 - 1 };
	}

	// A stick flicked: a step when it is first pushed out (not when it only swings round to another
	// way, which a thumb does without meaning to), and with `repeat` more while it is held there.
	struct PadFlick
	{
		int digit = 0;
		float held = 0;
	};

	inline int padFlickSteps(PadFlick& f, int digit, float dt, bool repeat, float delay = 0.3f, float interval = 0.1f)
	{
		if (digit == 0)
		{
			f = {};
			return 0;
		}
		if (f.digit == 0)
		{
			f = { digit, 0 };
			return 1;
		}
		if (digit != f.digit)
		{
			f = { digit, 0 };
			return 0;
		}
		const float before = f.held;
		f.held += dt;
		return repeat ? padRepeats(before, f.held, delay, interval) : 0;
	}

	// The list Minus brings up: the advisors by their keys, and the way of playing.
	struct PadMenuEntry
	{
		const wchar_t* label;
		unsigned key; // 0: the next PadMode
	};

	inline constexpr PadMenuEntry kPadMenu[] = {
		{ L"Domestic advisor", key::F1 },      { L"Finance advisor", key::F1 + 1 },  { L"Civics", key::F1 + 2 },
		{ L"Foreign advisor", key::F1 + 3 },   { L"Military advisor", key::F1 + 4 }, { L"Technology", key::F1 + 5 },
		{ L"Religion", key::F1 + 6 },          { L"Victory", key::F1 + 7 },          { L"Info", key::F1 + 8 },
		{ L"Civilopedia", key::F12 },          { L"Game menu", key::Escape },        { L"Controls", 0 },
	};
	inline constexpr int kPadMenuCount = int(sizeof(kPadMenu) / sizeof(kPadMenu[0]));

	// With the plot cursor, L's list (one's own empire) and R's (the world, and the game).
	inline constexpr PadMenuEntry kPadMenuEmpire[] = {
		{ L"Technology", key::F1 + 5 },       { L"Civics", key::F1 + 2 },         { L"Religion", key::F1 + 6 },
		{ L"Domestic advisor", key::F1 },     { L"Finance advisor", key::F1 + 1 }, { L"Military advisor", key::F1 + 4 },
	};
	inline constexpr PadMenuEntry kPadMenuWorld[] = {
		{ L"Foreign advisor", key::F1 + 3 }, { L"Victory", key::F1 + 7 },   { L"Info", key::F1 + 8 },
		{ L"Civilopedia", key::F12 },        { L"Game menu", key::Escape }, { L"Controls", 0 },
	};

	struct PadMenu
	{
		const PadMenuEntry* entries;
		int count;
	};
	inline constexpr PadMenu kPadMenus[] = {
		{ kPadMenu, kPadMenuCount },
		{ kPadMenuEmpire, int(sizeof(kPadMenuEmpire) / sizeof(kPadMenuEmpire[0])) },
		{ kPadMenuWorld, int(sizeof(kPadMenuWorld) / sizeof(kPadMenuWorld[0])) },
	};

	// The plot cursor on the screen at x,y, the map's part of the screen x0,y0..x1,y1: the way the map
	// must move (in plots, east and north) to bring it back in; 0,0 while it is inside.
	inline std::pair<int, int> padCursorScroll(float x, float y, float x0, float y0, float x1, float y1)
	{
		return { x < x0 ? -1 : x >= x1 ? 1 : 0, y < y0 ? 1 : y >= y1 ? -1 : 0 };
	}

	// B held this long (seconds) is the right button, not a press (PadAction::Kind::TapOrHold).
	inline constexpr float kPadHold = 0.3f;

	// The right stick pressed: from the plot cursor to the pointer, and from the pointer back to the
	// way without one last played (none yet: nothing, the stick's own key).
	inline std::optional<PadMode> padStickRMode(PadMode now, std::optional<PadMode> lastBare)
	{
		if (now == PadMode::Cursor)
			return PadMode::Pointer;
		if (now == PadMode::Pointer && lastBare)
			return lastBare;
		return std::nullopt;
	}

	// A trigger's axis (0..32767) is held.
	inline bool padTriggerHeld(int raw) { return raw > 32767 / 2; }

	// Whose turn it is to show a pointer: the system's mouse cursor until the gamepad is used (a
	// button, a stick well off its rest), and again as soon as the mouse moves. Without this both
	// showed at once, the mouse's and the one drawn for the gamepad.
	struct PadDevice
	{
		bool padLast = false;
		void onPad() { padLast = true; }
		void onMouse() { padLast = false; }
		bool mouseCursorShown() const { return !padLast; }
	};
	inline bool padStickUsed(int raw) { return raw > 32767 / 2 || raw < -32767 / 2; }
}
