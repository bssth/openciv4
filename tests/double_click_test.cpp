// Checks the map's double click (src/render/DoubleClick.h): two presses within 500 ms and a few
// points are a double click, a third starts again, and a later, farther or interrupted one is not.

#include "render/DoubleClick.h"

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

	constexpr uint64_t ms = 1'000'000;
}

int main()
{
	{
		DoubleClick d;
		check(d.press(1000 * ms, 100, 100) == 1, "first press: a click");
		check(d.press(1200 * ms, 100, 100) == 2, "200 ms later, same place: a double click");
		check(d.press(1300 * ms, 100, 100) == 1, "a third press: a click again");
		check(d.press(1400 * ms, 100, 100) == 2, "and a fourth the second of a new double click");
	}
	{
		DoubleClick d;
		d.press(0, 50, 50);
		check(d.press(480 * ms, 50, 50) == 2, "480 ms (the original opened the city): double");
		d.press(10'000 * ms, 50, 50);
		check(d.press(10'000 * ms + 500 * ms, 50, 50) == 2, "500 ms: double");
		d.press(20'000 * ms, 50, 50);
		check(d.press(20'000 * ms + 550 * ms, 50, 50) == 1, "550 ms (the original did not): single");
	}
	{
		DoubleClick d;
		d.press(0, 50, 50);
		check(d.press(200 * ms, 58, 42) == 2, "8 points each way: double");
		d.press(5000 * ms, 50, 50);
		check(d.press(5200 * ms, 59, 50) == 1, "9 points away: single");
		d.press(9000 * ms, 50, 50);
		check(d.press(9200 * ms, 50, 108) == 1, "from a city's plot to its bar, 58 points (the original: single)");
	}
	{
		DoubleClick d;
		d.press(0, 50, 50);
		d.reset();
		check(d.press(200 * ms, 50, 50) == 1, "the other button pressed between: single");
	}
	{
		DoubleClick d;
		d.press(1000 * ms, 50, 50);
		check(d.press(900 * ms, 50, 50) == 1, "a time before the last press: single");
	}
	if (failures == 0)
		std::printf("double click: all passed\n");
	return failures == 0 ? 0 : 1;
}
