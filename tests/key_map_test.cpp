// The keys by their place and the option Mac keys (render/KeyMap.h).
#include "render/KeyMap.h"

#include <cstdio>

using namespace oc4::render;

static int failures = 0;
#define CHECK(x)                                                         \
	do                                                                   \
	{                                                                    \
		if (!(x))                                                        \
		{                                                                \
			std::printf("%s:%d: FAILED %s\n", __FILE__, __LINE__, #x); \
			++failures;                                                  \
		}                                                                \
	} while (0)

int main()
{
	// By place: the key right of the full stop is the slash whatever it prints ('-' on a Danish
	// layout), the key left of Return (ISO) the backslash; letters' places are not punctuation.
	CHECK(keyOfPlace(56) == key::Slash);
	CHECK(keyOfPlace(49) == key::Backslash);
	CHECK(keyOfPlace(50) == key::Backslash);
	CHECK(keyOfPlace(45) == key::Minus);
	CHECK(keyOfPlace(46) == key::Plus);
	CHECK(keyOfPlace(54) == key::Comma);
	CHECK(keyOfPlace(4) == 0); // (A)
	CHECK(keyOfPlace(30) == 0); // (1)

	const Chord f11{ key::F1 + 10 };
	// Off: nothing changes, Cmd is nothing.
	CHECK(macKey({ 'G', true, false, false }, true, false) == (Chord{ 'G', true, false, false }));
	CHECK(macKey({ key::Left, false, false, true }, false, false) == (Chord{ key::Left, false, false, true }));
	CHECK(macKey({ 'H', false, false, false }, true, false) == (Chord{ 'H' }));

	// On: Cmd is Ctrl.
	CHECK(macKey({ key::Left }, true, true) == (Chord{ key::Left, false, true, false }));
	CHECK(macKey({ 'S' }, true, true) == (Chord{ 'S', false, true, false }));
	CHECK(macKey({ 'S', false, true, false }, false, true) == (Chord{ 'S', false, true, false })); // (Ctrl too)
	CHECK(macKey({ 'S', false, false, true }, true, true) == (Chord{ 'S', false, true, true }));   // (Cmd+Option+S: Ctrl+Alt+S)
	// ... but Cmd+Q, H, M are macOS's; Ctrl+H is the game's.
	CHECK(!macKey({ 'Q' }, true, true));
	CHECK(!macKey({ 'H' }, true, true));
	CHECK(!macKey({ 'M' }, true, true));
	CHECK(macKey({ 'H', false, true, false }, false, true) == (Chord{ 'H', false, true, false }));
	// F11: Cmd+Shift+G (or Ctrl+Shift+G).
	CHECK(macKey({ 'G', true, false, false }, true, true) == f11);
	CHECK(macKey({ 'G', true, true, false }, false, true) == f11);
	CHECK(macKey({ 'G' }, true, true) == (Chord{ 'G', false, true, false })); // (Go To Type stays)
	CHECK(macKey({ 'G', true, false, false }, false, true) == (Chord{ 'G', true, false, false }));
	// Insert: Cmd+I.
	CHECK(macKey({ 'I' }, true, true) == (Chord{ key::Insert }));
	CHECK(macKey({ 'I' }, false, true) == (Chord{ 'I' })); // (I alone: Farm, Air Patrol)
	// Option+arrows: the numpad's 8 2 4 6.
	CHECK(macKey({ key::Up, false, false, true }, false, true) == (Chord{ key::Numpad0 + 8 }));
	CHECK(macKey({ key::Down, false, false, true }, false, true) == (Chord{ key::Numpad0 + 2 }));
	CHECK(macKey({ key::Left, false, false, true }, false, true) == (Chord{ key::Numpad0 + 4 }));
	CHECK(macKey({ key::Right, false, false, true }, false, true) == (Chord{ key::Numpad0 + 6 }));
	CHECK(macKey({ key::Left, true, false, true }, false, true) == (Chord{ key::Left, true, false, true })); // (with Shift: not)
	CHECK(macKey({ key::Left, false, false, false }, false, true) == (Chord{ key::Left }));

	if (failures)
		std::printf("%d failed\n", failures);
	else
		std::printf("key map: all passed\n");
	return failures ? 1 : 0;
}
