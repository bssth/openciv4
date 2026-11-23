#pragma once

#include <cmath>
#include <cstdint>

// The mouse's double click on the map, as forgiving as the original's (BtS under Wine, measured:
// a second press up to ~480 ms after the first opens one's city, 550 ms
// does not, as Windows' 500 ms). The time is the presses' own (the system's event times), not
// when a frame gets round to them, so a slow frame after the first click cannot split a double
// click. The original lets the mouse move 1 point between the presses; this lets it move a few
// (a trackpad's taps wander): the second click of a double click is taken on what the first was
// on anyway (MapWindow), so nothing moving under the mouse in between matters.

namespace oc4::render
{
	constexpr uint64_t kDoubleClickNs = 500'000'000; // between the two presses
	constexpr float kDoubleClickRadius = 8;          // points, each way

	class DoubleClick
	{
	public:
		// A press of the button at that time (nanoseconds) and place (points): 2 for the second of
		// a double click, else 1. As Windows', a third press starts again (no triple click: it
		// would close the city screen the double click opened).
		int press(uint64_t ns, float x, float y)
		{
			const bool second = m_count == 1 && ns >= m_at && ns - m_at <= kDoubleClickNs &&
				std::fabs(x - m_x) <= kDoubleClickRadius && std::fabs(y - m_y) <= kDoubleClickRadius;
			m_count = second ? 2 : 1;
			m_at = ns, m_x = x, m_y = y;
			return m_count;
		}
		// Another button pressed between: no double click across it.
		void reset() { m_count = 0; }

	private:
		int m_count = 0;
		uint64_t m_at = 0;
		float m_x = 0, m_y = 0;
	};
}
