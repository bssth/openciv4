#pragma once
// The game's autosaves as the original makes them (CivilizationIV.ini's AutoSaveInterval and
// MaxAutoSaves; the options' OpenCiv4 tab), as seen in the running original under Wine
// (interval 2, 3 kept, Saves\single\auto listed at each of 16 turns): the game's first
// turn as AutoSave_Initial_BC-4000, then at the start of every interval's turn (2, 4, 6, ...)
// AutoSave_<year> (AutoSave_BC-3920); when one more than the number kept is written, the oldest
// AutoSave_<year> goes. The initial save doesn't count and stays. Interval 0: none, as the ini's
// comment says. Or openciv4's own way: each game its own files, the kept ones in turn.

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <functional>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oc4::host
{
	struct AutoSaveSettings
	{
		int interval = 4, kept = 5; // (the ini's defaults; interval 0: no autosaves)
		bool openciv4 = false;      // openciv4's own (My Games\openciv4\Saves\auto, one in 25 turns kept)
	};

	// The settings as the options' script sets them and the game's thread reads them.
	class SharedAutoSaveSettings
	{
	public:
		void set(const AutoSaveSettings& how)
		{
			std::lock_guard lock(m_mutex);
			m_how = how;
		}
		AutoSaveSettings get() const
		{
			std::lock_guard lock(m_mutex);
			return m_how;
		}

	private:
		mutable std::mutex m_mutex;
		AutoSaveSettings m_how;
	};

	// Whether the game saves at this turn's start (the game's first: its initial save).
	inline bool autoSaveDue(const AutoSaveSettings& how, int turn, bool initial)
	{
		return how.interval > 0 && (initial || turn % how.interval == 0);
	}

	// The original's file name: AutoSave_[Initial_]BC-4000.CivBeyondSwordSave. (Only BC years were
	// seen; an AD year is written the same way, AD-1250, as our guess.)
	inline std::wstring autoSaveName(bool initial, int year)
	{
		return std::format(L"AutoSave_{}{}-{}.CivBeyondSwordSave", initial ? L"Initial_" : L"", year < 0 ? L"BC" : L"AD", std::abs(year));
	}

	// openciv4's own: <stem>_<slot>, the kept ones in turn.
	inline std::wstring openciv4AutoSaveName(const std::wstring& stem, const AutoSaveSettings& how, int turn)
	{
		return std::format(L"{}_{}.CivBeyondSwordSave", stem, turn / std::max(1, how.interval) % std::max(1, how.kept));
	}

	// openciv4's own: whether this turn's save is also kept for good, the first save in each 25 turns
	// (turn 25, 50, ... when the interval divides it; else the first save after each).
	inline bool openciv4KeepsForGood(const AutoSaveSettings& how, int turn)
	{
		const int every = std::max(1, how.interval);
		return turn > 0 && turn / 25 != (turn - every) / 25;
	}

	// The AutoSave_<year> saves in the folder beyond the newest kept ones (the oldest first to go);
	// not the initial save, nor any other file.
	inline std::vector<std::filesystem::path> autoSavesBeyond(const std::filesystem::path& folder, int kept)
	{
		std::vector<std::pair<std::filesystem::file_time_type, std::filesystem::path>> saves;
		std::error_code ec;
		for (const auto& e : std::filesystem::directory_iterator(folder, ec))
			if (const std::wstring name = e.path().filename().wstring();
				e.is_regular_file() && name.starts_with(L"AutoSave_") && !name.starts_with(L"AutoSave_Initial_"))
				saves.push_back({ e.last_write_time(), e.path() });
		std::ranges::sort(saves, std::greater{});
		std::vector<std::filesystem::path> out;
		for (size_t i = size_t(std::max(1, kept)); i < saves.size(); ++i)
			out.push_back(saves[i].second);
		return out;
	}
}
