#pragma once

// Scrolling the map with the mouse at the window's edge, as the original does; an option, on by
// default (the options' Game tab, profile.ini edgeScroll; --no-edge-scroll turns it off for a run).

namespace oc4::render
{
	// The way the mouse at (px, py) in a w x h window pans the map: x -1 left / 1 right, y 1 up
	// (forward) / -1 down; 0 0 away from the edges, outside the window or with the option off.
	struct EdgeScrollMove
	{
		int x = 0, y = 0;
	};

	constexpr int kEdgeScrollBand = 3; // pixels from the edge

	inline EdgeScrollMove edgeScrollMove(bool on, int px, int py, int w, int h)
	{
		EdgeScrollMove m;
		if (!on || px < 0 || py < 0 || px >= w || py >= h)
			return m;
		if (py < kEdgeScrollBand)
			m.y = 1;
		else if (py >= h - kEdgeScrollBand)
			m.y = -1;
		if (px < kEdgeScrollBand)
			m.x = -1;
		else if (px >= w - kEdgeScrollBand)
			m.x = 1;
		return m;
	}

	// The option: the profile's (on unless the player turned it off), or off for the session when
	// forced (--no-edge-scroll, scripted runs) until the player chooses in the options.
	class EdgeScrollOption
	{
	public:
		bool on() const { return !m_forcedOff && m_on; }
		void set(bool on) { m_on = on; }
		void forceOff() { m_forcedOff = true; }
		void choose(bool on) { m_forcedOff = false; m_on = on; }

	private:
		bool m_on = true, m_forcedOff = false;
	};
}
