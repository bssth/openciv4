#pragma once
// Ending idle turns, an option (Ctrl+Shift+E, the options' Game tab, --auto-end-turn): the game
// core ends a turn by itself only when a unit moved in it (CvGame::updateTestEndTurn, and only
// with "Wait at End of Turn" off); a turn where nothing was done waits at its "press Enter".
// With this on, that wait ends the turn too, once it has lasted a while with nothing in the way
// (a popup, a talk, a screen, the city screen; not in hot seat or by email, as the original).
// A key or a click while it waits keeps the turn: the player is looking at something.

#include <chrono>
#include <optional>

namespace oc4::host
{
	struct AutoEndTurnView
	{
		bool on = false;
		bool turnActive = false;     // the player's turn
		bool endTurnMessage = false; // the game core's "press Enter": nothing waits for orders
		bool blocked = false;        // a popup, a talk, a screen or the city screen up; hot seat, by email
		int turn = 0;                // the game's turn
		unsigned inputs = 0;         // the window's count of the player's keys and clicks
	};

	class AutoEndTurn
	{
	public:
		using Clock = std::chrono::steady_clock;
		static constexpr std::chrono::milliseconds delay{ 2000 };

		// True when the turn is to end now (once a turn).
		bool update(const AutoEndTurnView& v, Clock::time_point now)
		{
			if (v.turn != m_turn)
			{
				m_turn = v.turn;
				m_held = false;
				m_since.reset();
			}
			// (a key or click while the turn is ready to end: the player stays on it)
			if (v.inputs != m_inputs)
			{
				m_inputs = v.inputs;
				if (v.turnActive && v.endTurnMessage)
					m_held = true;
			}
			if (!v.on || !v.turnActive || !v.endTurnMessage || v.blocked || m_held)
			{
				m_since.reset();
				return false;
			}
			if (!m_since)
			{
				m_since = now;
				return false;
			}
			if (now - *m_since < delay)
				return false;
			m_held = true; // (sent: not again this turn)
			return true;
		}

		bool held() const { return m_held; }

	private:
		int m_turn = -1;
		unsigned m_inputs = 0;
		bool m_held = false;
		std::optional<Clock::time_point> m_since;
	};
}
