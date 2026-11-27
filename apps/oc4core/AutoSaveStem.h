#pragma once
// The start of OpenCiv4's own autosave names: AutoSave_<civilization>_<game>, so that each game
// keeps its own files (Documents\My Games\openciv4\Saves\auto).
//
// The civilization is the active player's short name as the game shows it
// (CvPlayer::getCivilizationShortDescription: the init core's name, else the civilization's own).
// The init core's name alone is empty for most games, and the init core's map seed is not in a
// save (CvInitCore::read/write), so after a load both were empty and every game shared
// AutoSave__00000000_<n>. The game is now told apart by its map: a hash of every plot's plot type
// (land, hills, peak, water), which a save keeps and which nothing in BtS changes during a game.

#include "CvGameCoreDLL.h"
#include "CvGameAI.h"
#include "CvGlobals.h"
#include "CvMap.h"
#include "CvPlayerAI.h"
#include "CvPlot.h"

#include <cstdint>
#include <cwctype>
#include <format>
#include <string>

namespace oc4::host
{
	// FNV-1a over the map's size and its plots' types.
	inline std::uint32_t mapLayoutHash(const CvMap& map)
	{
		std::uint32_t h = 2166136261u;
		const auto add = [&h](int v) {
			for (int i = 0; i < 4; ++i)
			{
				h ^= std::uint32_t(v >> (8 * i)) & 0xFFu;
				h *= 16777619u;
			}
		};
		add(map.getGridWidth());
		add(map.getGridHeight());
		// (the plot type from the exported isPeak/isHills/isWater, as PlotTypes numbers them:
		// CvPlot::getPlotType is not exported from the Windows game core)
		for (int i = 0; i < map.numPlots(); ++i)
			if (const CvPlot* plot = map.plotByIndex(i))
				add(plot->isPeak() ? int(PLOT_PEAK) : plot->isHills() ? int(PLOT_HILLS) : plot->isWater() ? int(PLOT_OCEAN) : int(PLOT_LAND));
		return h;
	}

	// AutoSave_<civilization>_<game>, the civilization's name with anything but letters and digits as _.
	inline std::wstring autoSaveStem(const CvGame& game)
	{
		std::wstring civ;
		if (const PlayerTypes active = game.getActivePlayer(); active != NO_PLAYER)
			for (const wchar_t* c = CvPlayerAI::getPlayerNonInl(active).getCivilizationShortDescription(); c && *c; ++c)
				civ += std::iswalnum(*c) ? *c : L'_';
		return std::format(L"AutoSave_{}_{:08X}", civ, mapLayoutHash(GC.getMap()));
	}
}
