// Checks the command line's forms of a new game's setup (apps/oc4core/NewGameSettings.h):
// --option, --victories, --map-option and --slot, with and without their type prefixes.

#include "NewGameSettings.h"

#include <cstdio>
#include <stdexcept>

using namespace oc4::host;
using namespace oc4::host::setup;

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

	template <class F>
	void throws(F f, const char* what)
	{
		try
		{
			f();
		}
		catch (const std::runtime_error&)
		{
			return;
		}
		check(false, what);
	}
}

int main()
{
	check(parseOption("GAMEOPTION_NO_EVENTS") == std::pair<std::string, bool>{ "GAMEOPTION_NO_EVENTS", true }, "an option by its name is on");
	check(parseOption("no_goody_huts") == std::pair<std::string, bool>{ "GAMEOPTION_NO_GOODY_HUTS", true }, "the prefix and case may differ");
	check(parseOption("NO_EVENTS=0") == std::pair<std::string, bool>{ "GAMEOPTION_NO_EVENTS", false }, "=0 turns it off");
	check(parseOption("NO_EVENTS=1").second, "=1 turns it on");
	throws([] { parseOption("NO_EVENTS=yes"); }, "an option's value is 0 or 1");

	check(parseVictories("VICTORY_CONQUEST,space_race") == std::vector<std::string>{ "VICTORY_CONQUEST", "VICTORY_SPACE_RACE" }, "victories, with and without prefix");
	check(parseVictories("none") == std::vector<std::string>{ "NONE" }, "no victories at all");
	throws([] { parseVictories(","); }, "an empty victory list");

	check(parseMapOption("0=2") == std::pair{ 0, 2 }, "map option 0 to value 2");
	throws([] { parseMapOption("1"); }, "a map option without value");
	throws([] { parseMapOption("a=1"); }, "a map option that is no number");
	throws([] { parseMapOption("1=-1"); }, "a negative map option value");

	{
		const auto [slot, set] = parseSlot("3=closed");
		check(slot == 3 && set.status == SlotSetting::Status::Closed && set.civilization.empty() && set.team == -1, "a closed slot");
	}
	{
		const auto [slot, set] = parseSlot("2=AI:rome::1");
		check(slot == 2 && set.status == SlotSetting::Status::AI, "an AI slot");
		check(set.civilization == "CIVILIZATION_ROME" && set.leader.empty() && set.team == 1, "its civ, a random leader, team 1");
	}
	{
		const auto [slot, set] = parseSlot("0=human:CIVILIZATION_MALI:LEADER_MANSA_MUSA");
		check(slot == 0 && set.status == SlotSetting::Status::Human && set.leader == "LEADER_MANSA_MUSA", "the human's slot");
	}
	{
		const auto [slot, set] = parseSlot("5=ai");
		check(slot == 5 && set.civilization.empty() && set.leader.empty() && set.team == -1, "an AI slot, all random");
	}
	throws([] { parseSlot("3"); }, "a slot without status");
	throws([] { parseSlot("3=open"); }, "an unknown status");
	throws([] { parseSlot("3=closed:ROME"); }, "a closed slot with a civilization");
	throws([] { parseSlot("3=ai:a:b:1:x"); }, "too many parts");
	throws([] { parseSlot("3=ai:::x"); }, "a team that is no number");

	if (failures == 0)
		std::printf("new_game_settings: all passed\n");
	return failures == 0 ? 0 : 1;
}
