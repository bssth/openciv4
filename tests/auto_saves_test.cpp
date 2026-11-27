// Checks the autosaves' rules (apps/oc4core/AutoSaves.h), as the running original showed them with
// CivilizationIV.ini's AutoSaveInterval and MaxAutoSaves: when, the file names, and which old
// ones go; and openciv4's own names.

#include "AutoSaves.h"

#include <cstdio>
#include <fstream>

using namespace oc4::host;
namespace fs = std::filesystem;

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
}

int main()
{
	// When: the game's first turn, then every interval's; none with 0.
	const AutoSaveSettings ini; // (4, 5)
	check(autoSaveDue(ini, 0, true), "the game's first turn");
	check(!autoSaveDue(ini, 1, false) && !autoSaveDue(ini, 3, false) && autoSaveDue(ini, 4, false) && autoSaveDue(ini, 8, false), "every 4th turn");
	check(autoSaveDue({ 1, 10 }, 7, false), "every turn");
	check(!autoSaveDue({ 0, 5 }, 0, true) && !autoSaveDue({ 0, 5 }, 4, false), "interval 0: none, the first neither");

	// The names: the year as the original writes it.
	check(autoSaveName(true, -4000) == L"AutoSave_Initial_BC-4000.CivBeyondSwordSave", "the initial save's name");
	check(autoSaveName(false, -3840) == L"AutoSave_BC-3840.CivBeyondSwordSave", "a BC year");
	check(autoSaveName(false, 1250) == L"AutoSave_AD-1250.CivBeyondSwordSave", "an AD year (not seen: written as a BC one)");

	// openciv4's own: the kept ones in turn (every turn, 10 kept).
	check(openciv4AutoSaveName(L"AutoSave_Rome_1", { 1, 10, true }, 23) == L"AutoSave_Rome_1_3.CivBeyondSwordSave", "openciv4's, slot 3");
	check(openciv4AutoSaveName(L"AutoSave_Rome_1", { 4, 5, true }, 24) == L"AutoSave_Rome_1_1.CivBeyondSwordSave", "openciv4's, every 4th, 5 kept");

	// openciv4's own kept for good: one save in each 25 turns, whatever the interval.
	check(openciv4KeepsForGood({ 1, 10, true }, 25) && !openciv4KeepsForGood({ 1, 10, true }, 24) && !openciv4KeepsForGood({ 1, 10, true }, 26),
		"every turn: turn 25 kept");
	check(openciv4KeepsForGood({ 5, 5, true }, 50) && !openciv4KeepsForGood({ 5, 5, true }, 45) && !openciv4KeepsForGood({ 5, 5, true }, 55),
		"every 5th: turn 50 kept");
	check(openciv4KeepsForGood({ 4, 5, true }, 28) && !openciv4KeepsForGood({ 4, 5, true }, 24) && !openciv4KeepsForGood({ 4, 5, true }, 32)
			&& openciv4KeepsForGood({ 4, 5, true }, 52) && openciv4KeepsForGood({ 4, 5, true }, 100),
		"every 4th: 28, 52, 76, 100 kept (the first after each 25th)");
	check(!openciv4KeepsForGood({ 4, 5, true }, 0), "not the game's first turn");

	// The oldest AutoSave_<year> beyond the kept go; the initial save and other files stay (as seen:
	// with 3 kept, the 4th AutoSave_<year> takes the oldest, AutoSave_Initial_ stays).
	const fs::path folder = fs::temp_directory_path() / "oc4_auto_saves_test";
	fs::remove_all(folder);
	fs::create_directories(folder);
	const auto now = fs::file_time_type::clock::now();
	const wchar_t* names[] = { L"AutoSave_Initial_BC-4000.CivBeyondSwordSave", L"AutoSave_BC-3840.CivBeyondSwordSave",
		L"AutoSave_BC-3680.CivBeyondSwordSave", L"AutoSave_BC-3520.CivBeyondSwordSave" };
	for (int i = 0; i < 4; ++i)
	{
		std::ofstream(folder / names[i]) << i;
		fs::last_write_time(folder / names[i], now - std::chrono::hours(4 - i));
	}
	std::ofstream(folder / L"Mine.CivBeyondSwordSave") << 9;
	fs::last_write_time(folder / L"Mine.CivBeyondSwordSave", now - std::chrono::hours(9));
	const std::vector<fs::path> beyond = autoSavesBeyond(folder, 2);
	check(beyond.size() == 1 && beyond[0].filename() == names[1], "the oldest AutoSave_<year> beyond the two kept");
	check(autoSavesBeyond(folder, 3).empty(), "three kept: none to go, the initial save not counted");
	check(autoSavesBeyond(folder, 1).size() == 2, "one kept: the two older go, not the initial one nor the player's");
	fs::remove_all(folder);

	if (failures)
	{
		std::printf("auto_saves_test: %d failed\n", failures);
		return 1;
	}
	std::printf("auto_saves_test: all passed\n");
	return 0;
}
