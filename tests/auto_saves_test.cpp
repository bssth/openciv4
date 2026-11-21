// Checks the autosaves' rules (apps/oc4core/AutoSaves.h), as the original makes them with
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
	check(autoSaveName(false, 1250) == L"AutoSave_AD-1250.CivBeyondSwordSave", "an AD year");

	// openciv4's own: the kept ones in turn (every turn, 10 kept: as before the option).
	check(openciv4AutoSaveName(L"AutoSave_Rome_1", { 1, 10, true }, 23) == L"AutoSave_Rome_1_3.CivBeyondSwordSave", "openciv4's, slot 3");
	check(openciv4AutoSaveName(L"AutoSave_Rome_1", { 4, 5, true }, 24) == L"AutoSave_Rome_1_1.CivBeyondSwordSave", "openciv4's, every 4th, 5 kept");

	// The oldest beyond the kept go; other files stay.
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
	check(beyond.size() == 2, "two beyond the two kept");
	check(beyond.size() == 2 && beyond[0].filename() == names[1] && beyond[1].filename() == names[0], "the oldest autosaves, not the player's");
	check(autoSavesBeyond(folder, 5).empty(), "none beyond five");
	fs::remove_all(folder);

	if (failures)
	{
		std::printf("auto_saves_test: %d failed\n", failures);
		return 1;
	}
	std::printf("auto_saves_test: all passed\n");
	return 0;
}
