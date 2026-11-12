// Checks the gamepad's mapping (src/render/PadMap.h): what each button does on the map, over a
// screen and with the D-pad's choice made; the sticks' dead zone and curve; the pointer kept on
// the screen and the map panned at its edge; a held direction's repeats.

#include "render/PadMap.h"

#include <cmath>
#include <cstdio>

using namespace oc4::render;

namespace
{
	int failures = 0;

	void check(bool ok, const char* what)
	{
		if (!ok)
		{
			std::printf("FAILED: %s\n", what);
			++failures;
		}
	}

	bool near(float a, float b) { return std::fabs(a - b) < 1e-3f; }
	bool isKey(PadAction a, unsigned k) { return a.kind == PadAction::Kind::Key && a.key == k; }
}

int main()
{
	using Kind = PadAction::Kind;
	const PadContext map{ false, true }, screen{ false, false }, chosen{ true, false };
	const PadContext bareMap{ false, true, false }, bareScreen{ false, false, false }, bareChosen{ true, false, false };

	// The buttons.
	check(padAction(PadButton::A, map).kind == Kind::MouseLeft && padAction(PadButton::A, screen).kind == Kind::MouseLeft, "A: the left button");
	check(padAction(PadButton::A, chosen).kind == Kind::Choose, "A with the D-pad's choice: uses it");
	check(padAction(PadButton::B, map).kind == Kind::MouseRight, "B on the map: the right button");
	check(isKey(padAction(PadButton::B, screen), key::Escape) && isKey(padAction(PadButton::B, chosen), key::Escape), "B off the map: Escape");
	check(isKey(padAction(PadButton::X, map), key::Return), "X: Return (end turn)");
	check(isKey(padAction(PadButton::Y, map), key::Space), "Y: Space (skip)");
	check(isKey(padAction(PadButton::L, map), key::Comma) && isKey(padAction(PadButton::R, map), key::Period), "L and R: the unit before, the next");
	check(isKey(padAction(PadButton::Plus, map), key::Escape), "Plus: Escape");
	check(padAction(PadButton::Minus, screen).kind == Kind::Menu, "Minus: the advisors' list");
	check(padAction(PadButton::Minus, PadContext{ false, false, true, true }).kind == Kind::Keyboard, "Minus in an edit box: the software keyboard");

	// Without a pointer.
	check(padAction(PadButton::A, bareMap).kind == Kind::MouseLeft, "no pointer, A on the map: a click at the reticle");
	{
		const PadAction a = padAction(PadButton::A, bareScreen);
		check(a.kind == Kind::Move && a.direction == NavDirection::Down, "no pointer, A off the map: the choice starts");
	}
	check(padAction(PadButton::A, bareChosen).kind == Kind::Choose, "no pointer, A with a choice: uses it");
	check(padAction(PadButton::B, bareMap).kind == Kind::TapOrHold, "no pointer, B on the map: a press lets the selection go, held it is the right button");
	check(isKey(padAction(PadButton::B, bareScreen), key::Escape), "no pointer, B off the map: Escape");
	check(padNextMode(PadMode::Pointer) == PadMode::Reticle && padNextMode(PadMode::Reticle) == PadMode::Tile &&
		padNextMode(PadMode::Tile) == PadMode::Unit && padNextMode(PadMode::Unit) == PadMode::Cursor &&
		padNextMode(PadMode::Cursor) == PadMode::Pointer, "the modes go round");

	// With the plot cursor (Civilization VI's way).
	{
		const PadContext onMap{ false, true, false, false, true }, offMap{ false, false, false, false, true };
		const PadContext choosing{ true, true, false, false, true }, typing{ false, false, false, true, true };
		check(padAction(PadButton::A, onMap).kind == Kind::PlotAct, "plot cursor, A on the map: select or go");
		check(padAction(PadButton::A, choosing).kind == Kind::Choose, "plot cursor, A with a choice: uses it");
		check(padAction(PadButton::A, offMap).kind == Kind::Move, "plot cursor, A off the map: the choice starts");
		check(padAction(PadButton::B, onMap).kind == Kind::TapOrHold && isKey(padAction(PadButton::B, offMap), key::Escape), "plot cursor: B as without a pointer");
		const PadAction l = padAction(PadButton::L, onMap), r = padAction(PadButton::R, onMap);
		check(isKey(padAction(PadButton::Y, onMap), key::Space), "plot cursor, Y: Space (skip), as elsewhere");
		check(l.kind == Kind::Menu && l.menu == 1 && r.kind == Kind::Menu && r.menu == 2, "plot cursor, L and R: their lists");
		check(isKey(padAction(PadButton::Minus, onMap), key::Home), "plot cursor, Minus: the next city");
		check(padAction(PadButton::Minus, typing).kind == Kind::Keyboard, "plot cursor, Minus in an edit box: the keyboard still");
		check(isKey(padAction(PadButton::X, onMap), key::Return), "plot cursor, X: Return");
		const PadAction menu = padAction(PadButton::Minus, bareMap);
		check(menu.kind == Kind::Menu && menu.menu == 0, "the other ways' Minus: the whole list");
	}
	check(padStickRMode(PadMode::Cursor, std::nullopt) == PadMode::Pointer, "the right stick pressed with the plot cursor: the pointer");
	check(padStickRMode(PadMode::Pointer, PadMode::Cursor) == PadMode::Cursor && padStickRMode(PadMode::Pointer, PadMode::Tile) == PadMode::Tile,
		"pressed with the pointer: back to the way without one");
	check(!padStickRMode(PadMode::Pointer, std::nullopt) && !padStickRMode(PadMode::Reticle, PadMode::Reticle), "no way to go back to, or another way: the stick's own key");
	check(kPadMenus[1].count == 6 && kPadMenus[2].count == 6 && kPadMenus[2].entries[5].key == 0, "L's and R's lists; the controls in R's");
	check(padCursorScroll(640, 300, 100, 80, 1180, 560) == std::pair{ 0, 0 }, "the cursor inside the map's part: the map stays");
	check(padCursorScroll(99, 300, 100, 80, 1180, 560) == std::pair{ -1, 0 } && padCursorScroll(1180, 300, 100, 80, 1180, 560) == std::pair{ 1, 0 },
		"off the left or right: the map goes west or east");
	check(padCursorScroll(640, 79, 100, 80, 1180, 560) == std::pair{ 0, 1 } && padCursorScroll(640, 560, 100, 80, 1180, 560) == std::pair{ 0, -1 },
		"off the top or bottom: north or south");
	check(padCursorScroll(50, 600, 100, 80, 1180, 560) == std::pair{ -1, -1 }, "off a corner: both");

	// A stick's eight ways.
	check(padEightWay(0, 0) == 0 && padEightWay(15000, 0) == 0 && padEightWay(13000, 13000) == 0, "not pushed out: none");
	check(padEightWay(32767, 0) == 6 && padEightWay(-32768, 0) == 4, "right 6, left 4");
	check(padEightWay(0, -32768) == 8 && padEightWay(0, 32767) == 2, "up 8, down 2");
	check(padEightWay(25000, -25000) == 9 && padEightWay(-25000, -25000) == 7, "up right 9, up left 7");
	check(padEightWay(25000, 25000) == 3 && padEightWay(-25000, 25000) == 1, "down right 3, down left 1");
	check(padEightWay(32767, -9000) == 6 && padEightWay(32767, -18000) == 9, "a little off a side is still that side; more is the corner");
	check(padDigitStep(8) == std::pair{ 0, 1 } && padDigitStep(2) == std::pair{ 0, -1 } && padDigitStep(4) == std::pair{ -1, 0 } &&
		padDigitStep(6) == std::pair{ 1, 0 }, "the sides' steps (north is up)");
	check(padDigitStep(7) == std::pair{ -1, 1 } && padDigitStep(3) == std::pair{ 1, -1 } && padDigitStep(5) == std::pair{ 0, 0 } &&
		padDigitStep(0) == std::pair{ 0, 0 }, "the corners' steps; none for 5 or no digit");

	// A flick.
	{
		PadFlick f;
		check(padFlickSteps(f, 0, 0.016f, false) == 0, "at rest: nothing");
		check(padFlickSteps(f, 8, 0.016f, false) == 1, "pushed out: one step");
		check(padFlickSteps(f, 8, 1.0f, false) == 0, "held, no repeat: no more");
		check(padFlickSteps(f, 9, 0.016f, false) == 0, "swung round to the next way: no step");
		check(padFlickSteps(f, 0, 0.016f, false) == 0 && padFlickSteps(f, 9, 0.016f, false) == 1, "let go and pushed again: a step");
		PadFlick r;
		check(padFlickSteps(r, 6, 0.016f, true) == 1 && padFlickSteps(r, 6, 0.2f, true) == 0, "repeating: the first, then the delay");
		check(padFlickSteps(r, 6, 0.15f, true) == 1 && padFlickSteps(r, 6, 0.1f, true) == 1, "then one each interval");
		check(padFlickSteps(r, 3, 0.016f, true) == 0 && padFlickSteps(r, 3, 0.2f, true) == 0 && padFlickSteps(r, 3, 0.15f, true) == 1,
			"another way while held: its own delay first");
	}

	// The list.
	check(kPadMenuCount == 12 && kPadMenu[0].key == key::F1 && kPadMenu[kPadMenuCount - 1].key == 0, "the list: the advisors, the controls last");
	check(isKey(padAction(PadButton::StickL, map), 'C') && isKey(padAction(PadButton::StickR, map), key::Home), "the sticks pressed: centre, next city");
	{
		const PadAction up = padAction(PadButton::Up, map), right = padAction(PadButton::Right, chosen);
		check(up.kind == Kind::Move && up.direction == NavDirection::Up, "D-pad up moves the choice up");
		check(right.kind == Kind::Move && right.direction == NavDirection::Right, "D-pad right moves the choice right");
	}

	// The sticks.
	{
		const PadVector rest = padStick(0, 0), drift = padStick(5000, -3000), edge = padStick(int(32767 * 0.2f), 0);
		check(rest.x == 0 && rest.y == 0, "at rest: nothing");
		check(drift.x == 0 && drift.y == 0, "inside the dead zone: nothing");
		check(edge.x == 0 && edge.y == 0, "at the dead zone's edge: nothing");
		const PadVector full = padStick(32767, 0), back = padStick(-32768, 0), down = padStick(0, 32767);
		check(near(full.x, 1) && full.y == 0, "full right: 1");
		check(near(back.x, -1), "full left: -1 (not beyond)");
		check(near(down.y, 1) && down.x == 0, "full down: y 1");
		const PadVector half = padStick(int(32767 * 0.6f), 0);
		check(near(half.x, 0.25f), "half way past the dead zone: a quarter (squared)");
		const PadVector corner = padStick(32767, 32767);
		check(near(std::sqrt(corner.x * corner.x + corner.y * corner.y), 1), "a corner: no faster than a side");
		const PadVector diagonal = padStick(20000, -20000);
		check(diagonal.x > 0 && near(diagonal.x, -diagonal.y), "a diagonal keeps its direction");
	}

	// The pointer.
	{
		const PadCursorStep s = padCursorStep(100, 100, { 1, 0 }, 0.1f, 1280, 720, 900);
		check(near(s.x, 190) && near(s.y, 100) && s.edge.x == 0 && s.edge.y == 0, "moves at its speed");
		const PadCursorStep still = padCursorStep(100, 100, {}, 0.1f, 1280, 720);
		check(still.x == 100 && still.y == 100, "no push: still");
		const PadCursorStep right = padCursorStep(1275, 300, { 1, 0.5f }, 0.1f, 1280, 720, 900);
		check(right.x == 1279 && near(right.y, 345) && right.edge.x == 1 && right.edge.y == 0, "stops at the right edge and pans that way only");
		const PadCursorStep corner = padCursorStep(0, 0, { -0.5f, -1 }, 0.016f, 1280, 720);
		check(corner.x == 0 && corner.y == 0 && corner.edge.x == -0.5f && corner.edge.y == -1, "in the corner: pans both ways, by the push");
		const PadCursorStep away = padCursorStep(0, 719, { 1, -1 }, 0.016f, 1280, 720);
		check(away.edge.x == 0 && away.edge.y == 0 && away.x > 0 && away.y < 719, "leaving the edge: no pan");
		const PadCursorStep empty = padCursorStep(5, 5, { 1, 1 }, 0.1f, 0, 0);
		check(empty.x == 0 && empty.y == 0, "no screen: stays at its origin");
	}

	// A held direction.
	check(padRepeats(0, 0.39f) == 0, "held less than the delay: no repeat");
	check(padRepeats(0.39f, 0.41f) == 1, "the delay passed: one");
	check(padRepeats(0.41f, 0.45f) == 0, "between repeats: none");
	check(padRepeats(0.41f, 0.53f) == 1, "the interval passed: another");
	check(padRepeats(0, 0.95f) == 5, "a long frame: all that were due");
	check(padRepeats(1.0f, 1.0f) == 0, "no time: none");

	check(!padTriggerHeld(0) && !padTriggerHeld(16000) && padTriggerHeld(32767), "a trigger is held past half way");

	// the mouse's cursor and the gamepad's pointer: one at a time
	PadDevice device;
	check(device.mouseCursorShown(), "no gamepad used yet: the mouse's cursor");
	device.onPad();
	check(!device.mouseCursorShown(), "the gamepad used: the mouse's cursor hidden");
	device.onMouse();
	check(device.mouseCursorShown(), "the mouse moved: its cursor back");
	check(!padStickUsed(0) && !padStickUsed(9000) && padStickUsed(-30000) && padStickUsed(30000), "a stick is used well off its rest");

	if (failures)
		return 1;
	std::printf("pad map: ok\n");
	return 0;
}
