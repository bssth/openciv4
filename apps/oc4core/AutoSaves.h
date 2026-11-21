#pragma once
// The game's autosaves as the original makes them (CivilizationIV.ini's AutoSaveInterval and
// MaxAutoSaves; the options' OpenCiv4 tab): the game's first turn as AutoSave_Initial_<year>, then
// the start of every interval's turn as AutoSave_<year> (BC-4000, AD-1250), into
// Saves\single\auto, the oldest autosaves there beyond the number kept deleted. Or openciv4's own
// way: each game its own files, the kept ones in turn.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oc4::host
{
	struct AutoSaveSettings
	{
		int interval = 4, kept = 5; // (the ini's defaults; interval 0: no autosaves)
		bool openciv4 = false;      // openciv4's own (My Games\openciv4\Saves\auto, every 25th turn kept)
	};

	// Whether the game saves at this turn's start (the game's first: its initial save).
	inline bool autoSaveDue(const AutoSaveSettings& how, int turn, bool initial)
	{
		return how.interval > 0 && (initial || turn % how.interval == 0);
	}

	// The original's file name: AutoSave_[Initial_]BC-4000.CivBeyondSwordSave.
	inline std::wstring autoSaveName(bool initial, int year)
	{
		return std::format(L"AutoSave_{}{}-{}.CivBeyondSwordSave", initial ? L"Initial_" : L"", year < 0 ? L"BC" : L"AD", std::abs(year));
	}

	// openciv4's own: <stem>_<slot>, the kept ones in turn.
	inline std::wstring openciv4AutoSaveName(const std::wstring& stem, const AutoSaveSettings& how, int turn)
	{
		return std::format(L"{}_{}.CivBeyondSwordSave", stem, turn / std::max(1, how.interval) % std::max(1, how.kept));
	}

	// The autosaves in the folder beyond the newest kept ones (the oldest first to go).
	inline std::vector<std::filesystem::path> autoSavesBeyond(const std::filesystem::path& folder, int kept)
	{
		std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> saves;
		std::error_code ec;
		for (const auto& e : std::filesystem::directory_iterator(folder, ec))
			if (e.is_regular_file() && e.path().filename().wstring().starts_with(L"AutoSave"))
				saves.push_back({ e.last_write_time(), e.path() });
		std::ranges::sort(saves, std::greater{});
		std::vector<std::filesystem::path> out;
		for (size_t i = size_t(std::max(1, kept)); i < saves.size(); ++i)
			out.push_back(saves[i].second);
		return out;
	}
}
