#pragma once

// What the map view draws, as plain data: the host fills it from the game core (only
// through what the DLL exports) and the view never sees the game's classes. The art
// part changes with the game's content, the rest every turn.
//
// Plain C++20 with the standard library only: the host includes it with the SDK's
// headers and settings.

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace oc4::render
{
	struct Color
	{
		float r = 1, g = 1, b = 1;
	};

	// CvArtInfoTerrain. The blend texture is an atlas of square tiles, 4 per row: slot 1
	// is the top left one. A plot's corners are drawn from the tile chosen by the mask of
	// corners covered by this terrain (bit 0 top left, 1 top right, 2 bottom right, 3 bottom
	// left); mask 15 is the plain terrain.
	struct TerrainArt
	{
		std::string blend, grid, detail; // asset paths
		int layerOrder = 0;
		bool water = false;
		std::array<std::vector<std::pair<int, int>>, 16> slots; // per mask: (1-based slot, degrees clockwise)
	};

	// CvArtInfoFeature: per variety the model pieces by the mask of neighbours (DirectionTypes
	// bits) that have the same feature.
	struct FeatureArt
	{
		enum class Tiling : uint8_t
		{
			None, Trees, HalfTiling, PlotTiling
		};
		struct Piece
		{
			int connections = 0;
			std::vector<std::pair<std::string, int>> models; // (NIF path, RotationTypes)
		};
		Tiling tiling = Tiling::None;
		float scale = 1;
		std::vector<std::vector<Piece>> varieties;
	};

	// CvRouteModelInfo: a piece of a route for a plot whose neighbours in `connections`
	// (DirectionTypes bits) have routes, turned by one of `rotations` (degrees clockwise).
	struct RouteArt
	{
		int route = -1;
		std::string nif;
		int connections = 0;
		std::vector<int> rotations;
	};

	// CvRiverModelInfo: a piece of river at a plot corner whose edges in `connections`
	// (DirectionTypes bits of N, E, S, W) have river, turned by one of `rotations`. Deltas
	// end a river at the coast.
	struct RiverArt
	{
		enum class Delta : uint8_t
		{
			None, Straight, Clockwise, CounterClockwise
		};
		std::string nif;
		int connections = 0;
		Delta delta = Delta::None;
		std::vector<int> rotations;
	};

	// A model with its animations (CvArtInfoAsset and its scalable subclasses).
	struct ModelArt
	{
		std::string nif, kfm;
		float scale = 1;
		// Units: the blob shadow under a node (CvArtInfoUnit::getShadowNIF and the like).
		std::string shadow, shadowNode;
		float shadowScale = 1;
		bool ranged = false; // units: CvArtInfoUnit::getActAsRanged (their fortify is the ranged one)
		bool operator==(const ModelArt&) const = default;
	};

	// A generic house of the city LSystem (CIV4CityLSystem.xml): one node of a NIF that
	// holds a set of them ("1x1_01"), covering width x height lots of the city's layout.
	struct CityHouseArt
	{
		std::string nif, node;
		int width = 1, height = 1;
	};

	// Walls built around a city (the LSystem's WallBuilder): posts, and wall pieces 1, 2 and
	// 4 posts' distances long ("%1" in the name is 01, 02, 04), a gate.
	struct CityWallView
	{
		std::string post, wall, gate;
	};

	// A building the city shows (CvCity::getVisibleBuildings) with the lots its art takes.
	struct CityBuildingView
	{
		int model = -1; // into MapSnapshot::models
		int width = 1, height = 1;
		bool water = false; // harbours, lighthouses and the like: on the shore, not on lots
		float scale = 1;    // the LSystem's for it
	};

	struct PlayerView
	{
		Color primary, secondary;
		bool alive = false;
		std::string name, civilization;
		int score = 0, cities = 0;
		bool barbarian = false;
	};

	struct PlotView
	{
		int16_t terrain = -1, feature = -1, owner = -1;
		int16_t improvement = -1, bonus = -1, route = -1; // (the resource as the active team knows it)
		uint8_t yields[3]{}; // food, production, commerce (YieldTypes) of a plot the active team has seen
		uint8_t featureVariety = 0;
		bool water = false, hills = false, peak = false;
		bool revealed = true, visible = true; // to the active team
		bool riverSouth = false, riverEast = false; // a river along the plot's south / east edge
	};

	struct UnitView
	{
		int id = -1; // the owner's unit id: the view follows a unit from plot to plot
		int x = 0, y = 0;
		int owner = -1;
		int shownOwner = -1; // whose it looks to the active team (getVisualOwner: a hidden nationality's, the barbarians')
		int direction = 4; // DirectionTypes the unit faces (south)
		std::vector<int> models; // one per figure of the formation, into MapSnapshot::models
		int count = 1;           // units on the plot
		float health = 1;
		std::string button; // the shown unit's button art (the unit layer's icons)
		bool selected = false;   // the player has selected units there
		// ... and how the head of them can move (the selection ring's dots): 2 it has not moved
		// yet, 1 it has but can go on, 0 it cannot; -1 another player's.
		int moves = -1;
		bool resting = false;    // fortified, asleep, on sentry or healing: it fortifies, then stays so
		int battle = -1;         // fighting: into MapSnapshot::battles
		bool attacking = false;  // ... as the attacker
		// Working (a worker building): the first of its animation categories (ANIMCAT_*_BEGIN:
		// irrigate 50, build 53, mine 56, chop 59, shovel 62, railroad 65; then the work, its
		// pause); -1 none.
		int work = -1;
		// A settler that founded a city (it is gone from the game): seconds since, as captured;
		// it plays its founding (ANIMCAT_FOUND) once.
		float founded = -1;
		// An action it shows (a mission's animation): how it acts (1 pillaging: its melee
		// strikes; 2 a ranged or an air strike, bombarding: its ranged strike; 3 captured: it
		// heals), seconds since it began, as captured, how long it lasts, the plot it aims at
		// and the effect there (a CIV4EffectInfos type; none: empty).
		int action = -1;
		float actionSince = 0, actionLength = 0;
		int actionX = -1, actionY = -1;
		std::string actionEffect;
		bool air = false; // a plane: its missions are flown to their plot and back
	};

	// A battle the game shows (its CvBattleDefinition): the game holds it for `slices` of its
	// updates, kBattleSliceSeconds each while it is on: setup (the attacker steps up), six a
	// round (both sides strike; those killed in it die), then its end.
	constexpr float kBattleSliceSeconds = 0.15f;
	constexpr int kBattleSetupSlices = 4, kBattleRoundSlices = 6;
	struct BattleRoundView
	{
		bool ranged = false;
		int killed[2]{}, alive[2]{}; // attacker, defender: figures killed in it, alive after it
	};
	struct BattleView
	{
		uint64_t serial = 0;  // each battle its own
		float elapsed = 0;    // seconds since it began, when captured
		int slices = 0;
		int attackerX = 0, attackerY = 0, defenderX = 0, defenderY = 0;
		std::vector<BattleRoundView> rounds;
		int collateral = 0; // the defender's plot's other units hit too (collateral damage): blasts there
	};

	struct CityView
	{
		int x = 0, y = 0;
		int owner = -1;
		int population = 0;
		bool capital = false;
		std::string name;       // as the game's city bar shows it (with turns to grow)
		std::string production; // what it builds (turns), if the active player may see it: markup
		float foodStored = -1, foodRate = 0, productionStored = -1, productionRate = 0; // bar fractions, -1: not shown
		// The game's colours of the population's circle and number (getCityBillboardSizeIconColors) and of the bars.
		Color sizeDot, sizeText, foodBar{ 0.35f, 0.85f, 0.25f }, productionBar{ 0.35f, 0.55f, 0.95f };
		// What the original lays out on the city's lots: its visible buildings, highest
		// priority first, and as many generic houses from its era's and style's set.
		std::vector<CityBuildingView> buildings;
		std::vector<CityWallView> walls;
		int generics = 0;
		int houseSet = -1; // into MapSnapshot::houseSets
	};

	struct MapSnapshot
	{
		// Content (art); `artGeneration` changes when it does.
		uint64_t artGeneration = 0;
		std::vector<TerrainArt> terrains;  // by TerrainTypes
		int hillTerrain = -1, peakTerrain = -1; // graphical terrains drawn over hills and peaks
		std::vector<FeatureArt> features; // by FeatureTypes
		std::vector<int> improvements, bonuses; // by ImprovementTypes / BonusTypes: into models, -1 none
		std::vector<std::string> bonusButtons;  // by BonusTypes: their button art (the resource layer's icons)
		std::vector<RouteArt> routes;
		std::vector<RiverArt> rivers;
		std::vector<ModelArt> models;      // referenced by units, cities, improvements and bonuses
		// The effects models carry on their nodes (a camp's fire, a mine's smoke: CIV4AttachableInfos,
		// "ATTACH:<node>:<type>" in a NIF's user properties): by type, their NIF and scale.
		struct Attachable
		{
			std::string type, nif;
			float scale = 1;
		};
		std::vector<Attachable> attachables;
		// The Great Wall (a building that is an area border obstacle): along the edges of the
		// plots of its owner on its city's landmass that face land of others, its segments
		// (ART_GREAT_WALL_SEGMENT) and posts at the corners (ART_GREAT_WALL_POST).
		struct WallEdge
		{
			int x = 0, y = 0;
			uint8_t side = 0; // 0 north, 1 east, 2 south, 3 west
		};
		std::vector<WallEdge> greatWall;
		Attachable greatWallSegment, greatWallPost;
		// The effects (CIV4EffectInfos) the figures' animations set off by type ("Effect:<node>:<type>").
		std::vector<Attachable> effects;
		// The city layout: generic houses, the sets of them per era and art style (indices into
		// cityHouses), the layout's size in plots and its turn (CIV4DetailManager).
		std::vector<CityHouseArt> cityHouses;
		std::vector<std::vector<int>> houseSets;
		float cityLayoutSize = 0.72f, cityTwist = -0.7853982f;

		// The map. World units as the game's (PLOT_SIZE per plot); heights of hills and peaks
		// in plots (CvLandscapeInfo).
		int width = 0, height = 0;
		bool wrapX = false, wrapY = false;
		// The team's map is centred (CvGame::shouldCenterMinimap: Calendar, Stonehenge). Until it is,
		// the minimap shows what the team has seen in its middle, at its own scale: it does not tell
		// how big the world is nor where in it the team is.
		bool minimapCentered = true;
		float plotSize = 120, hillScale = 0.13f, peakScale = 0.44f;
		std::vector<PlotView> plots; // x + y * width
		std::vector<PlayerView> players;
		std::vector<UnitView> units;
		std::vector<BattleView> battles;
		std::vector<CityView> cities;
		int turn = 0;
		std::string date;

		const PlotView* plot(int x, int y) const
		{
			if (wrapX)
				x = (x % width + width) % width;
			if (wrapY)
				y = (y % height + height) % height;
			if (x < 0 || y < 0 || x >= width || y >= height)
				return nullptr;
			return &plots[size_t(x) + size_t(y) * size_t(width)];
		}
	};
}
