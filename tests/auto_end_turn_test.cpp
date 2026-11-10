// Checks ending idle turns (apps/oc4core/AutoEndTurn.h): off, nothing; on, a turn waiting at its
// "press Enter" ends after the delay, once, unless something is up or the player pressed or
// clicked while it waited.

#include "AutoEndTurn.h"

#include <cstdio>

using namespace oc4::host;
using namespace std::chrono_literals;

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

	const AutoEndTurn::Clock::time_point t0{};

	AutoEndTurnView waiting(int turn = 10, unsigned inputs = 0)
	{
		return { .on = true, .turnActive = true, .endTurnMessage = true, .blocked = false, .turn = turn, .inputs = inputs };
	}
}

int main()
{
	{
		AutoEndTurn a;
		AutoEndTurnView v = waiting();
		v.on = false;
		check(!a.update(v, t0) && !a.update(v, t0 + 10s), "off: never");
	}
	{
		AutoEndTurn a;
		check(!a.update(waiting(), t0), "on: not at once");
		check(!a.update(waiting(), t0 + AutoEndTurn::delay - 1ms), "on: not before the delay");
		check(a.update(waiting(), t0 + AutoEndTurn::delay), "on: at the delay");
		check(!a.update(waiting(), t0 + 10s), "on: once a turn");
		check(!a.update(waiting(11), t0 + 11s), "next turn: its own delay");
		check(a.update(waiting(11), t0 + 11s + AutoEndTurn::delay), "next turn: ends too");
	}
	{
		AutoEndTurn a;
		AutoEndTurnView v = waiting();
		v.endTurnMessage = false;
		check(!a.update(v, t0) && !a.update(v, t0 + 10s), "units wait for orders: never");
		v.endTurnMessage = true;
		v.turnActive = false;
		check(!a.update(v, t0 + 20s), "not the player's turn: never");
	}
	{
		AutoEndTurn a;
		AutoEndTurnView v = waiting();
		a.update(v, t0);
		v.blocked = true;
		check(!a.update(v, t0 + 5s), "a popup or screen up: never");
		v.blocked = false;
		check(!a.update(v, t0 + 6s), "closed: the delay starts again");
		check(a.update(v, t0 + 6s + AutoEndTurn::delay), "closed: then it ends");
	}
	{
		AutoEndTurn a;
		AutoEndTurnView v = waiting(10, 3);
		a.update(v, t0);
		v.inputs = 4;
		check(!a.update(v, t0 + 1s) && !a.update(v, t0 + 30s) && a.held(), "a key while it waits: the turn is kept");
		check(!a.update(waiting(11, 4), t0 + 31s) && a.update(waiting(11, 4), t0 + 31s + AutoEndTurn::delay), "kept for that turn only");
	}
	{
		AutoEndTurn a;
		AutoEndTurnView v = waiting(10, 5);
		v.endTurnMessage = false; // (moving the last unit: keys before the turn is ready)
		a.update(v, t0);
		v.inputs = 7;
		a.update(v, t0 + 1s);
		v.endTurnMessage = true;
		check(!a.update(v, t0 + 2s) && a.update(v, t0 + 2s + AutoEndTurn::delay), "keys before the wait do not keep it");
	}
	if (failures == 0)
		std::printf("auto_end_turn_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
