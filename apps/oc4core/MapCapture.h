#pragma once

// The running game as the map view sees it (render::MapSnapshot), read through what the
// DLL exports, as the original engine reads it for drawing: the terrain, feature and unit
// art from the art infos, the map's plots, units and cities, the players' colours.

#include "core/AssetResolver.h"
#include "render/MapSnapshot.h"
#include "render/MapWindow.h"
#include "Interface.h"

#include <map>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oc4::host
{
	class MapCapture
	{
	public:
		explicit MapCapture(const AssetResolver& assets) : m_assets(assets) {}

		// Everything observable, ignoring what the active player has revealed; with the
		// interface, which units are selected.
		const render::MapSnapshot& capture(const class Interface* selection = nullptr);
		// The game's description of the selected units, for the info panel.
		std::string selectionInfo(class Interface& selection) const;
		// The popup up, for the window: its items as the window shows them (itemOf: the popup's
		// item for each), and the game's help for one of them (its widget's).
		render::MapWindow::PopupView popupView(const Interface::Popup& popup, std::vector<int>& itemOf) const;
		std::string popupHelp(const Interface::Popup& popup, int item) const;
		// The game's help for one of its widgets (an action button: WIDGET_ACTION, the action).
		std::string widgetHelp(int widget, int data1, int data2, bool option = false) const;
		// The active player's capital, or (-1, -1).
		std::pair<int, int> focus() const;
		// "Turn 57" and the like, for the window title.
		std::string caption() const;
		// What the game says about a plot when the mouse rests on it (its units, then the plot;
		// the odds of an attack there when it is the goto plot).
		std::string plotHelp(int x, int y) const;

	private:
		void captureArt();
		int modelIndex(const void* art, const char* nif, const char* kfm, float scale, const char* shadow = nullptr, const char* shadowNode = nullptr,
			float shadowScale = 1);
		void loadCityLSystem();
		void loadFlagScale();
		void loadAttachables(render::MapSnapshot& s);
	public:
		// The cities with the Great Wall (their plots, the map's): the wall along their owners'
		// borders on their landmasses.
		void setGreatWallCities(std::vector<std::pair<int, int>> cities) { m_greatWallCities = std::move(cities); }
	private:
		std::vector<std::pair<int, int>> m_greatWallCities;
		void loadArtList(std::vector<render::MapSnapshot::Attachable>& out, const char* file, const char* element);
		int houseSet(int era, int artStyle);

		// A generic house of the city LSystem with the eras and art styles it is drawn for.
		struct LSystemHouse
		{
			int art = -1; // into the snapshot's cityHouses
			std::vector<std::string> eras, styles;
		};

		// The LSystem's walls (szBuilder:WallBuilder) by LSystem name: post and wall files
		// (empty: the generic walls of the city's era).
		std::map<std::string, std::pair<std::string, std::string>> m_wallBuilders;
		// Buildings of the LSystem's water and coast layouts, by LSystem name: their scale.
		std::map<std::string, float> m_shoreBuildings;

		const AssetResolver& m_assets;
		std::vector<LSystemHouse> m_lsystemHouses;
		std::map<std::pair<int, int>, int> m_houseSets; // (era, art style) -> set
		render::MapSnapshot m_snapshot;
		bool m_haveArt = false;
		// The cities as last asked for, by owner and id: asked again when the DLL marks them
		// dirty (info: the bar, layout: the buildings), each turn, and a few each capture.
		struct KnownCity
		{
			const void* city = nullptr;
			int turn = -1, activePlayer = -1;
			size_t seen = 0; // the capture that last found it
			render::CityView view;
		};
		std::unordered_map<uint64_t, KnownCity> m_cities;
		size_t m_captures = 0;
		std::unordered_map<const void*, int> m_modelByArt;
		std::unordered_map<std::string, int> m_modelByPath;
	};
}
