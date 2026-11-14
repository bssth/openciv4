#pragma once

// Draws a game map from MapSnapshots: the terrain textured from the terrain art's blend
// atlases as the original engine does (at every point between four plot centres the
// terrains of those plots, lowest layer first, each from the atlas tile for the corners
// it covers), hills and peaks as height plus their overlay art, feature models (forests,
// jungles, ice) chosen by their neighbours, improvements, resources and roads (laid on
// the terrain, pieces by the neighbours they lead to), the cities' houses and the units,
// animated.

#include "render/MapSnapshot.h"
#include "render/Overlay.h"
#include "render/Renderer.h"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oc4::render
{
	class MapView
	{
	public:
		struct Options
		{
			bool grid = false, borders = true, features = true, improvements = true, cities = true, units = true, banners = true;
			bool fog = false;  // as the active team sees the map
			bool flags = true; // the units' flags
			bool healthBars = true, frozen = false, effects = true; // (the game's graphic options: health under the flags, idle figures still, no particles)
		};

		MapView(Renderer& renderer, const AssetResolver& assets);
		~MapView();

		// Takes over a new state of the game; rebuilds what changed.
		void setMap(const MapSnapshot& snapshot);
		bool empty() const;
		const MapSnapshot& map() const;

		void update(float dt);
		// An effect set off (its NIF: particles, and what animates in it) at plot (x + fx, y + fy),
		// turned that many degrees and scaled: shown for its time, then its particles die out.
		void triggerEffect(const std::string& nif, int x, int y, float fx, float fy, float degrees, float scale);
		// Between Renderer::beginFrame and endFrame.
		void draw();
		// The cities' banners and the units' flags, after endFrame.
		void drawOverlay(Overlay& overlay);

		// The map at one pixel per plot, north up (BGRA8): terrain, owners, cities; `version`
		// changes with it.
		const std::vector<uint8_t>& minimap(uint64_t& version) const;
		// The plots the minimap is moved by until the team's map is centred (MapSnapshot::minimapCentered):
		// its column x shows plot column x + first, its row y plot row y + second (around the map where it wraps).
		std::pair<int, int> minimapShift() const;
		// Until the team's map is centred (with the fog of war on): the plots it has seen, x0, y0, x1, y1
		// inclusive (on an axis the map wraps on, the whole map). The camera keeps near them rather than
		// on the map, so that where the map ends does not show.
		std::optional<std::array<int, 4>> seenBounds() const;
		// The same without the cities, the owners' colours over the terrain that strong (0: the
		// terrain alone), as the screens' own minimaps start from (the military advisor's).
		// unexplored: black with the fog of war on, unless it shows the whole map (a replay's).
		std::vector<uint8_t> territoryMinimap(float alpha, bool wholeMap = false) const;
		// Each terrain's colour there (BGRA8, the average of its plain tile), by terrain type.
		const std::vector<uint32_t>& terrainColors() const;

		Options options;

		// World space: plot (x, y) spans [x, x + 1) * plotSize in X and Y, Z is up.
		float plotSize() const;
		Vec3 worldSize() const;
		float groundHeight(float x, float y) const;
		std::optional<std::pair<int, int>> plotAt(Vec3 world) const;
		// The plot of the unit whose figure the ray from a to b meets first, as last drawn.
		std::optional<std::pair<int, int>> figureAt(Vec3 a, Vec3 b) const;

		// Art that could not be loaded, once per asset.
		const std::vector<std::string>& problems() const;

		// The sounds the units' animations call for as they play (their clips' text keys:
		// an audio script's tag), where (plot units).
		void setSoundHandler(std::function<void(const std::string& tag, float x, float y)> handler);

	private:
		struct Impl;
		std::unique_ptr<Impl> m;
	};
}
