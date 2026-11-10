// Checks edge scrolling (src/render/EdgeScroll.h): the 3-pixel band at each edge pans the map,
// nothing does with the option off, and --no-edge-scroll holds until the player chooses.

#include "render/EdgeScroll.h"

#include <cstdio>
#include <utility>

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

	bool is(EdgeScrollMove m, int x, int y) { return m.x == x && m.y == y; }
}

int main()
{
	constexpr int w = 1600, h = 900;
	check(is(edgeScrollMove(true, 800, 450, w, h), 0, 0), "middle: still");
	check(is(edgeScrollMove(true, 800, 0, w, h), 0, 1), "top: forward");
	check(is(edgeScrollMove(true, 800, 2, w, h), 0, 1), "top band's last row");
	check(is(edgeScrollMove(true, 800, 3, w, h), 0, 0), "just inside the top band: still");
	check(is(edgeScrollMove(true, 800, h - 1, w, h), 0, -1), "bottom: back");
	check(is(edgeScrollMove(true, 800, h - 4, w, h), 0, 0), "just inside the bottom band: still");
	check(is(edgeScrollMove(true, 0, 450, w, h), -1, 0), "left");
	check(is(edgeScrollMove(true, w - 1, 450, w, h), 1, 0), "right");
	check(is(edgeScrollMove(true, 0, 0, w, h), -1, 1), "corner: both");
	check(is(edgeScrollMove(true, -1, 450, w, h), 0, 0) && is(edgeScrollMove(true, 800, h, w, h), 0, 0), "outside the window: still");

	// Off: no edge moves the map.
	for (const auto [x, y] : { std::pair{ 0, 0 }, std::pair{ 800, 0 }, std::pair{ w - 1, h - 1 }, std::pair{ 0, 450 } })
		check(is(edgeScrollMove(false, x, y, w, h), 0, 0), "off: edges are still");

	{
		EdgeScrollOption o;
		check(o.on(), "on by default");
		o.set(false);
		check(!o.on(), "profile off");
		o.set(true);
		o.forceOff();
		check(!o.on(), "forced off over the profile's on");
		o.set(true); // (the profile loads after the flag)
		check(!o.on(), "forced off survives the profile loading");
		o.choose(true);
		check(o.on(), "the player's choice ends the force");
		o.choose(false);
		check(!o.on(), "the player turns it off");
	}

	if (failures)
		return 1;
	std::printf("edge scroll: all passed\n");
	return 0;
}
