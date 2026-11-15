#include "MapCapture.h"

#include "GameFont.h"
#include "Interface.h"
#include "Xml.h"

#include "CvGameCoreDLL.h"
#include "CvArtFileMgr.h"
#include "CvCity.h"
#include "CvGameAI.h"
#include "CvDLLWidgetData.h"
#include "CvGameTextMgr.h"
#include "CvPopupInfo.h"
#include "CvGlobals.h"
#include "CvInfos.h"
#include "CvMap.h"
#include "CvPlayerAI.h"
#include "CvPlot.h"
#include "CvSelectionGroup.h"
#include "CvUnit.h"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <list>

namespace oc4::host
{
	namespace
	{
		CvPlayerAI& player(int i) { return CvPlayerAI::getPlayerNonInl(static_cast<PlayerTypes>(i)); }

		// A worker at work: the animation its build's entity event plays (CvBuildInfo's
		// EntityEvent, its first animation path), as the first of the path's categories; -1 none.
		int workAnimation(const CvUnit& unit)
		{
			const BuildTypes build = unit.getBuildType();
			if (build == NO_BUILD)
				return -1;
			const int event = GC.getBuildInfo(build).getEntityEvent();
			if (event < 0)
				return -1;
			const CvEntityEventInfo& info = GC.getEntityEventInfo(static_cast<EntityEventTypes>(event));
			switch (info.getAnimationPathCount() > 0 ? info.getAnimationPathType(0) : ANIMATIONPATH_NONE)
			{
			case ANIMATIONPATH_IRRIGATE: return 50;
			case ANIMATIONPATH_BUILD: return 53;
			case ANIMATIONPATH_MINE: return 56;
			case ANIMATIONPATH_CHOP: return 59;
			case ANIMATIONPATH_SHOVEL: return 62;
			case ANIMATIONPATH_RAILROAD: return 65;
			default: return 53;
			}
		}

		std::string utf8(const wchar_t* w)
		{
			if (!w || !*w)
				return {};
			const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
			std::string s(size_t(std::max(n, 1)) - 1, '\0');
			WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
			return s;
		}

		std::string text(const char* s) { return s ? s : ""; }

		render::Color colorOf(int playerColor, bool primary)
		{
			if (playerColor < 0 || playerColor >= GC.getNumPlayerColorInfos())
				return {};
			CvPlayerColorInfo& info = GC.getPlayerColorInfo(static_cast<PlayerColorTypes>(playerColor));
			const int type = primary ? info.getColorTypePrimary() : info.getColorTypeSecondary();
			if (type < 0 || type >= GC.getNumColorInfos())
				return {};
			const NiColorA& c = GC.getColorInfo(static_cast<ColorTypes>(type)).getColor();
			return { c.r, c.g, c.b };
		}

		// A variety's piece for a connection mask, if it has one. The SDK's lookup falls back
		// to the first piece, and a variety does not tell how many it has: one without pieces
		// faults there, so probe under SEH.
		const CvArtInfoFeature::FeatureArtPiece* pieceFor(const CvArtInfoFeature::FeatureVariety& variety, int mask)
		{
#ifdef _WIN32
			__try
			{
				const CvArtInfoFeature::FeatureArtPiece& piece = variety.getFeatureArtPieceFromConnectionMask(mask);
				return piece.getConnectionMask() == mask ? &piece : nullptr;
			}
			__except (EXCEPTION_EXECUTE_HANDLER)
			{
				return nullptr;
			}
#else
			if (variety.getNumFeatureArtPieces() == 0)
				return nullptr;
			const CvArtInfoFeature::FeatureArtPiece& piece = variety.getFeatureArtPieceFromConnectionMask(mask);
			return piece.getConnectionMask() == mask ? &piece : nullptr;
#endif
		}

		// "N NE" as DirectionTypes bits; "-" is none.
		int directionMask(const char* text)
		{
			static const char* const kNames[] = { "N", "NE", "E", "SE", "S", "SW", "W", "NW" };
			int mask = 0;
			std::string word;
			for (const char* c = text ? text : "";; ++c)
				if (*c == ' ' || *c == '\t' || *c == '\0')
				{
					for (int d = 0; d < 8; ++d)
						if (_stricmp(word.c_str(), kNames[d]) == 0)
							mask |= 1 << d;
					word.clear();
					if (!*c)
						break;
				}
				else
					word += *c;
			return mask;
		}

		std::vector<int> numbers(const char* text)
		{
			std::vector<int> values;
			for (const char* c = text ? text : ""; *c;)
			{
				char* end = nullptr;
				const long v = std::strtol(c, &end, 10);
				if (end == c)
				{
					++c;
					continue;
				}
				values.push_back(int(v));
				c = end;
			}
			return values;
		}

		float defineOr(const char* name, float fallback)
		{
			const float v = GC.getDefineFLOAT(name);
			return v > 0 ? v : fallback;
		}

		render::FeatureArt::Tiling tilingOf(TileArtTypes type)
		{
			switch (type)
			{
			case TILE_ART_TYPE_TREES: return render::FeatureArt::Tiling::Trees;
			case TILE_ART_TYPE_HALF_TILING: return render::FeatureArt::Tiling::HalfTiling;
			case TILE_ART_TYPE_PLOT_TILING: return render::FeatureArt::Tiling::PlotTiling;
			default: return render::FeatureArt::Tiling::None;
			}
		}
	}

	void MapCapture::captureArt()
	{
		render::MapSnapshot& s = m_snapshot;
		s.artGeneration++;

		s.terrains.assign(size_t(std::max(GC.getNumTerrainInfos(), 0)), {});
		for (int t = 0; t < GC.getNumTerrainInfos(); ++t)
		{
			render::TerrainArt& art = s.terrains[size_t(t)];
			CvArtInfoTerrain* info = ARTFILEMGR.getTerrainArtInfo(GC.getTerrainInfo(static_cast<TerrainTypes>(t)).getArtDefineTag());
			if (!info)
				continue;
			art.blend = text(info->getBaseTexture());
			art.grid = text(info->getGridTexture());
			art.detail = text(info->getDetailTexture());
			art.layerOrder = info->getLayerOrder();
			for (int mask = 1; mask < 16; ++mask)
				for (const auto& [slot, rotation] : info->getBlendList(mask))
					art.slots[size_t(mask)].push_back({ slot, rotation });
		}
		s.hillTerrain = GC.getInfoTypeForString("TERRAIN_HILL", true);
		s.peakTerrain = GC.getInfoTypeForString("TERRAIN_PEAK", true);

		s.features.assign(size_t(std::max(GC.getNumFeatureInfos(), 0)), {});
		for (int f = 0; f < GC.getNumFeatureInfos(); ++f)
		{
			const CvArtInfoFeature* info = GC.getFeatureInfo(static_cast<FeatureTypes>(f)).getArtInfo();
			if (!info)
				continue;
			render::FeatureArt& art = s.features[size_t(f)];
			art.tiling = tilingOf(info->getTileArtType());
			art.scale = info->getScale();
			for (int v = 0; v < info->getNumVarieties(); ++v)
			{
				auto& pieces = art.varieties.emplace_back();
				for (int mask = 0; mask < 256; ++mask)
					if (const auto* piece = pieceFor(info->getVariety(v), mask))
					{
						render::FeatureArt::Piece& p = pieces.emplace_back();
						p.connections = mask;
						for (int m = 0; m < piece->getNumArtModels(); ++m)
							p.models.push_back({ std::string(piece->getArtModel(m).getModelFile()), int(piece->getArtModel(m).getRotation()) });
					}
			}
		}
		// Improvements and resources stand in their plot, scaled as the game does.
		const float improvementScale = defineOr("IMPROVEMENT_SCALE", 1), bonusScale = defineOr("BONUS_SCALE", 1);
		s.improvements.assign(size_t(std::max(GC.getNumImprovementInfos(), 0)), -1);
		for (int i = 0; i < GC.getNumImprovementInfos(); ++i)
			if (const CvArtInfoImprovement* art = GC.getImprovementInfo(static_cast<ImprovementTypes>(i)).getArtInfo())
				s.improvements[size_t(i)] = modelIndex(art, art->getNIF(), art->getKFM(), art->getScale() * improvementScale);
		s.bonuses.assign(size_t(std::max(GC.getNumBonusInfos(), 0)), -1);
		for (int b = 0; b < GC.getNumBonusInfos(); ++b)
			if (const CvArtInfoBonus* art = GC.getBonusInfo(static_cast<BonusTypes>(b)).getArtInfo())
				s.bonuses[size_t(b)] = modelIndex(art, art->getNIF(), art->getKFM(), art->getScale() * bonusScale);
		s.bonusButtons.assign(size_t(std::max(GC.getNumBonusInfos(), 0)), std::string());
		for (int b = 0; b < GC.getNumBonusInfos(); ++b)
			if (const char* button = GC.getBonusInfo(static_cast<BonusTypes>(b)).getButton())
				s.bonusButtons[size_t(b)] = button;

		s.routes.clear();
		for (int r = 0; r < GC.getNumRouteModelInfos(); ++r)
		{
			CvRouteModelInfo& info = GC.getRouteModelInfo(r);
			s.routes.push_back({ int(info.getRouteType()), text(info.getModelFile()), directionMask(info.getConnectString()), numbers(info.getRotateString()) });
		}
		loadCityLSystem();
		loadAttachables(s);

		s.rivers.clear();
		for (int r = 0; r < GC.getNumRiverModelInfos(); ++r)
		{
			CvRiverModelInfo& info = GC.getRiverModelInfo(r);
			render::RiverArt art{ text(info.getModelFile()), directionMask(info.getConnectString()), render::RiverArt::Delta::None,
				numbers(info.getRotateString()) };
			const std::string delta = text(info.getDeltaString());
			if (delta == "DELTA_STRAIGHT")
				art.delta = render::RiverArt::Delta::Straight;
			else if (delta == "DELTA_CLOCKWISE")
				art.delta = render::RiverArt::Delta::Clockwise;
			else if (delta == "DELTA_COUNTER_CLOCKWISE")
				art.delta = render::RiverArt::Delta::CounterClockwise;
			s.rivers.push_back(std::move(art));
		}
		m_haveArt = true;
	}

	int MapCapture::modelIndex(const void* art, const char* nif, const char* kfm, float scale, const char* shadow, const char* shadowNode, float shadowScale)
	{
		if (const auto it = m_modelByArt.find(art); it != m_modelByArt.end())
			return it->second;
		render::ModelArt model{ text(nif), text(kfm), scale, text(shadow), text(shadowNode), shadowScale };
		const std::string key = model.nif + '|' + model.kfm + '|' + std::to_string(scale) + '|' + model.shadow + '|' + model.shadowNode;
		auto [it, added] = m_modelByPath.try_emplace(key, int(m_snapshot.models.size()));
		if (added)
			m_snapshot.models.push_back(std::move(model));
		m_modelByArt.emplace(art, it->second);
		return it->second;
	}

	// CIV4AttachableInfos.xml, the engine's own too (the DLL loads none of it).
	void MapCapture::loadAttachables(render::MapSnapshot& s)
	{
		s.attachables.clear();
		s.effects.clear();
		loadArtList(s.attachables, "xml/misc/civ4attachableinfos.xml", "AttachableInfo");
		loadArtList(s.effects, "xml/misc/civ4effectinfos.xml", "EffectInfo");
		std::vector<render::MapSnapshot::Attachable> misc;
		loadArtList(misc, "xml/art/civ4artdefines_misc.xml", "MiscArtInfo");
		for (const auto& a : misc)
			if (a.type == "ART_GREAT_WALL_SEGMENT")
				s.greatWallSegment = a;
			else if (a.type == "ART_GREAT_WALL_POST")
				s.greatWallPost = a;
	}

	void MapCapture::loadArtList(std::vector<render::MapSnapshot::Attachable>& out, const char* file, const char* element)
	{
		const auto loc = m_assets.locate(file);
		if (!loc)
			return;
		std::string error;
		const auto doc = parseXml(m_assets.read(*loc), error);
		if (!doc)
			return;
		const auto narrow = [](const std::wstring& w) { return std::string(w.begin(), w.end()); };
		std::vector<const XmlNode*> stack{ doc.get() };
		while (!stack.empty())
		{
			const XmlNode* n = stack.back();
			stack.pop_back();
			for (const auto& c : n->children)
				stack.push_back(c.get());
			if (n->type != XmlNode::Type::Element || n->name != element)
				continue;
			const XmlNode* type = n->firstChildElement("Type");
			const XmlNode* path = n->firstChildElement("Path");
			if (!type || !path)
				continue;
			const XmlNode* scale = n->firstChildElement("fScale");
			out.push_back({ narrow(type->text), narrow(path->text), scale ? std::wcstof(scale->text.c_str(), nullptr) : 1.0f });
		}
	}

	// The engine reads CIV4CityLSystem.xml itself (the DLL never sees it): here only its
	// generic houses, "generic:<set>.nif::<node>" art references in the leaf nodes, with the
	// eras and art styles each is for.
	void MapCapture::loadCityLSystem()
	{
		const auto loc = m_assets.locate("xml/buildings/civ4citylsystem.xml");
		if (!loc)
			return;
		std::string error;
		const auto doc = parseXml(m_assets.read(*loc), error);
		if (!doc)
		{
			std::printf("  map view: CIV4CityLSystem.xml: %s\n", error.c_str());
			return;
		}
		const auto narrow = [](const std::wstring& w) { return std::string(w.begin(), w.end()); };
		const auto list = [&](const std::wstring& w) {
			std::vector<std::string> out;
			std::string item;
			for (const wchar_t c : w + L",")
				if (c == L',' || c == L' ')
				{
					if (!item.empty())
						out.push_back(item);
					item.clear();
				}
				else
					item += char(c);
			return out;
		};
		std::vector<const XmlNode*> stack{ doc.get() };
		while (!stack.empty())
		{
			const XmlNode* n = stack.back();
			stack.pop_back();
			for (const auto& c : n->children)
				stack.push_back(c.get());
			if (n->type != XmlNode::Type::Element || n->name != "LNode")
				continue;
			int width = 1, height = 1;
			if (const XmlNode* w = n->firstChildElement("Width"))
				width = std::max(1, int(std::wcstof(w->text.c_str(), nullptr)));
			if (const XmlNode* h = n->firstChildElement("Height"))
				height = std::max(1, int(std::wcstof(h->text.c_str(), nullptr)));
			for (const auto& ref : n->children)
			{
				const std::wstring* name = ref->type == XmlNode::Type::Element && ref->name == "ArtRef" ? ref->attribute("Name") : nullptr;
				if (name && name->rfind(L"building:", 0) == 0)
				{
					// Buildings laid out by a builder of the engine's: here the walls.
					std::map<std::string, std::string> scalars;
					for (const auto& a : ref->children)
						if (const std::wstring* cls = a->type == XmlNode::Type::Element && a->name == "Attribute" ? a->attribute("Class") : nullptr)
							if (*cls == L"Scalar")
							{
								const std::string v = narrow(a->text);
								if (const size_t colon = v.find(':'); colon != std::string::npos)
									scalars[v.substr(0, colon)] = v.substr(colon + 1);
							}
					const std::string building = narrow(name->substr(9));
					if (scalars["szBuilder"] == "WallBuilder")
						m_wallBuilders[building] = { scalars["szPostArtFile"], scalars["szWallArtFile"] };
					else if (scalars["szSpecialLayout"] == "Water" || scalars["szSpecialLayout"] == "Coast")
					{
						float scale = 1;
						if (const XmlNode* s = ref->firstChildElement("Scale"))
							scale = std::wcstof(s->text.c_str(), nullptr);
						m_shoreBuildings.try_emplace(building, scale > 0 ? scale : 1.0f);
					}
					continue;
				}
				if (!name || name->rfind(L"generic:", 0) != 0)
					continue;
				const std::string value = narrow(name->substr(8));
				const size_t sep = value.find("::");
				if (sep == std::string::npos)
					continue;
				LSystemHouse house;
				house.art = int(m_snapshot.cityHouses.size());
				m_snapshot.cityHouses.push_back({ "Art/Structures/Cities/" + value.substr(0, sep), value.substr(sep + 2), width, height });
				for (const auto& a : ref->children)
					if (const std::wstring* cls = a->type == XmlNode::Type::Element && a->name == "Attribute" ? a->attribute("Class") : nullptr)
					{
						if (*cls == L"Era")
							house.eras = list(a->text);
						else if (*cls == L"ArtStyle")
							house.styles = list(a->text);
					}
				m_lsystemHouses.push_back(std::move(house));
			}
		}
	}

	// The houses a city of this era and art style draws from, as the LSystem's attributes say.
	int MapCapture::houseSet(int era, int artStyle)
	{
		if (const auto it = m_houseSets.find({ era, artStyle }); it != m_houseSets.end())
			return it->second;
		const std::string eraName = era >= 0 && era < GC.getNumEraInfos() ? text(GC.getEraInfo(static_cast<EraTypes>(era)).getType()) : "";
		const std::string styleName = artStyle >= 0 && artStyle < GC.getNumArtStyleTypes() ? std::string(GC.getArtStyleTypes(static_cast<ArtStyleTypes>(artStyle))) : "";
		const auto allows = [](const std::vector<std::string>& values, const std::string& value, const char* all) {
			return values.empty() || std::ranges::find(values, value) != values.end() || std::ranges::find(values, std::string(all)) != values.end();
		};
		std::vector<int> set;
		for (const bool anyStyle : { false, true }) // no house for the style: any of the era's
		{
			for (const LSystemHouse& h : m_lsystemHouses)
				if (allows(h.eras, eraName, "ERA_ALL") && (anyStyle || allows(h.styles, styleName, "ARTSTYLE_ALL")))
					set.push_back(h.art);
			if (!set.empty())
				break;
		}
		const int index = int(m_snapshot.houseSets.size());
		m_snapshot.houseSets.push_back(std::move(set));
		m_houseSets.emplace(std::pair{ era, artStyle }, index);
		return index;
	}

	const render::MapSnapshot& MapCapture::capture(const Interface* selection)
	{
		if (!m_haveArt)
			captureArt();
		render::MapSnapshot& s = m_snapshot;
		CvMap& map = GC.getMap();
		CvGame& game = GC.getGame();
		s.width = map.getGridWidth();
		s.height = map.getGridHeight();
		s.wrapX = map.isWrapX();
		s.wrapY = map.isWrapY();
		s.minimapCentered = game.getActivePlayer() == NO_PLAYER || game.shouldCenterMinimap();
		s.plotSize = GC.getPLOT_SIZE();
		s.cameraFov = defineOr("FIELD_OF_VIEW", 42);
		s.cameraMinDistance = defineOr("CAMERA_MIN_DISTANCE", 700);
		s.cameraStartDistance = defineOr("CAMERA_START_DISTANCE", 2200);
		s.cameraTopDistance = defineOr("CAMERA_SMALLEST_MAX_DISTANCE", 7000);
		s.cameraCityDistance = defineOr("CAMERA_CITY_ZOOM_IN_DISTANCE", 3500);
		// (the pitches are negative there: below the horizon)
		s.cameraLowerPitch = std::abs(GC.getDefineFLOAT("CAMERA_LOWER_PITCH")) > 0 ? std::abs(GC.getDefineFLOAT("CAMERA_LOWER_PITCH")) : 28;
		s.cameraUpperPitch = std::abs(GC.getDefineFLOAT("CAMERA_UPPER_PITCH")) > 0 ? std::abs(GC.getDefineFLOAT("CAMERA_UPPER_PITCH")) : 90;
		const int landscape = GC.getActiveLandscapeID();
		if (landscape >= 0 && landscape < GC.getNumLandscapeInfos())
		{
			CvLandscapeInfo& info = GC.getLandscapeInfo(landscape);
			s.hillScale = info.getHillScale();
			s.peakScale = info.getPeakScale();
		}
		s.turn = game.getGameTurn();
		CvWString date;
		GAMETEXT.setTimeStr(date, s.turn, false);
		s.date = gameText(date.c_str());

		s.players.assign(size_t(GC.getMAX_PLAYERS()), {});
		for (int p = 0; p < GC.getMAX_PLAYERS(); ++p)
		{
			CvPlayerAI& pl = player(p);
			render::PlayerView& v = s.players[size_t(p)];
			v.alive = pl.isAlive();
			if (!v.alive)
				continue;
			v.primary = colorOf(pl.getPlayerColor(), true);
			v.secondary = colorOf(pl.getPlayerColor(), false);
			v.name = utf8(pl.getName());
			v.civilization = utf8(pl.getCivilizationShortDescription());
			v.score = game.getPlayerScore(static_cast<PlayerTypes>(p));
			v.cities = pl.getNumCities();
			v.barbarian = p == GC.getBARBARIAN_PLAYER();
		}

		const TeamTypes active = game.getActiveTeam();
		const auto actions = Entities::actions();
		// The Great Wall's edges: its owner's plots on the landmass, against land not its own.
		s.greatWall.clear();
		for (const auto& [cx, cy] : m_greatWallCities)
		{
			const CvPlot* cityPlot = map.plot(cx, cy);
			if (!cityPlot || cityPlot->getOwner() == NO_PLAYER)
				continue;
			const PlayerTypes owner = cityPlot->getOwner();
			const int area = cityPlot->getAreaIdForGreatWall();
			for (int i = 0; i < map.numPlots(); ++i)
			{
				const CvPlot* p = map.plotByIndex(i);
				if (!p || p->getOwner() != owner || p->isWater() || p->getAreaIdForGreatWall() != area)
					continue;
				static constexpr int dx[4] = { 0, 1, 0, -1 }, dy[4] = { 1, 0, -1, 0 };
				for (uint8_t side = 0; side < 4; ++side)
					if (const CvPlot* n = map.plot(p->getX() + dx[side], p->getY() + dy[side]); n && !n->isWater() && n->getOwner() != owner)
						s.greatWall.push_back({ p->getX(), p->getY(), side });
			}
		}
		s.plots.assign(size_t(s.width) * size_t(s.height), {});
		s.units.clear();
		s.cities.clear();
		// The battles on: their units stand for their plots while they fight.
		const auto battles = Entities::battles();
		s.battles.clear();
		std::unordered_map<int, std::pair<IDInfo, std::pair<int, bool>>> fighting; // plot: unit, (battle, attacking)
		for (const Entities::Battle& b : battles)
		{
			render::BattleView& v = s.battles.emplace_back();
			v.serial = b.serial;
			v.elapsed = std::chrono::duration<float>(std::chrono::steady_clock::now() - b.start).count();
			v.slices = b.slices;
			v.collateral = b.collateral;
			v.attackerX = b.attackerX, v.attackerY = b.attackerY, v.defenderX = b.defenderX, v.defenderY = b.defenderY;
			for (const auto& r : b.rounds)
				v.rounds.push_back({ r.ranged, { r.killed[0], r.killed[1] }, { r.alive[0], r.alive[1] } });
			const int index = int(s.battles.size()) - 1;
			fighting[b.attackerX + b.attackerY * s.width] = { b.attacker, { index, true } };
			fighting[b.defenderX + b.defenderY * s.width] = { b.defender, { index, false } };
		}
		++m_captures;
		size_t cityIndex = 0;
		for (int i = 0; i < map.numPlots(); ++i)
		{
			CvPlot* plot = map.plotByIndex(i);
			if (!plot || plot->getX() < 0 || plot->getY() < 0 || plot->getX() >= s.width || plot->getY() >= s.height)
				continue;
			render::PlotView& v = s.plots[size_t(plot->getX()) + size_t(plot->getY()) * size_t(s.width)];
			v.terrain = int16_t(plot->getTerrainType());
			v.feature = int16_t(plot->getFeatureType());
			v.featureVariety = uint8_t(std::max(plot->getFeatureVariety(), 0));
			v.owner = int16_t(plot->getOwner());
			v.improvement = int16_t(plot->getImprovementType());
			// (a resource the active team cannot see yet, by its techs, is not there for it)
			v.bonus = int16_t(plot->getBonusType(active));
			v.route = int16_t(plot->getRouteType());
			v.water = plot->isWater();
			v.hills = plot->isHills();
			v.peak = plot->isPeak();
			v.riverSouth = plot->isNOfRiver();
			v.riverEast = plot->isWOfRiver();
			v.revealed = active == NO_TEAM || plot->isRevealed(active, false);
			v.visible = active == NO_TEAM || plot->isVisible(active, false);
			if (v.revealed)
				for (int y = 0; y < 3; ++y)
					v.yields[y] = uint8_t(std::clamp(plot->getYield(static_cast<YieldTypes>(y)), 0, 255));

			if (const int count = plot->getNumUnits(); count > 0)
			{
				// (the units the active team cannot see, spies and submarines of others, are not there for it)
				const auto seen = [&](const CvUnit* unit) { return unit && (active == NO_TEAM || !unit->isInvisible(active, false)); };
				int seenCount = 0;
				for (int u = 0; u < count; ++u)
					seenCount += seen(plot->getUnitByIndex(u)) ? 1 : 0;
				// The unit standing for the stack: one of the plot's own domain, if any.
				CvUnit* shown = nullptr;
				for (int u = 0; u < count && !shown; ++u)
				{
					CvUnit* unit = plot->getUnitByIndex(u);
					if (seen(unit) && (unit->getDomainType() == DOMAIN_SEA) == plot->isWater())
						shown = unit;
				}
				for (int u = 0; u < count && !shown; ++u)
					if (CvUnit* unit = plot->getUnitByIndex(u); seen(unit))
						shown = unit;
				const auto fight = fighting.find(plot->getX() + plot->getY() * s.width);
				if (fight != fighting.end())
					for (int u = 0; u < count; ++u)
						if (CvUnit* unit = plot->getUnitByIndex(u); unit && unit->getOwner() == fight->second.first.eOwner && unit->getID() == fight->second.first.iID)
							shown = unit;
				// (a plane flying a mission from there: it, while it flies)
				for (int u = 0; u < count; ++u)
					if (CvUnit* unit = plot->getUnitByIndex(u); seen(unit) && unit->getDomainType() == DOMAIN_AIR)
						for (const Entities::Action& a : actions)
							if (a.unit.eOwner == unit->getOwner() && a.unit.iID == unit->getID())
								shown = unit;
				if (shown && shown->getOwner() >= 0)
				{
					render::UnitView& u = s.units.emplace_back();
					u.id = shown->getID();
					if (fight != fighting.end() && shown->getOwner() == fight->second.first.eOwner && shown->getID() == fight->second.first.iID)
						u.battle = fight->second.second.first, u.attacking = fight->second.second.second;
					for (int k = 0; selection && k < count && !u.selected; ++k)
						if (const CvUnit* any = plot->getUnitByIndex(k))
							u.selected = selection->isSelected(any);
					if (u.selected)
						if (const CvUnit* head = const_cast<Interface*>(selection)->getHeadSelectedUnit();
							head && head->plot() == plot && head->getOwner() == GC.getGame().getActivePlayer())
							u.moves = !head->canMove() ? 0 : head->hasMoved() ? 1 : 2;
					u.x = plot->getX();
					u.y = plot->getY();
					u.owner = shown->getOwner();
					u.shownOwner = shown->getVisualOwner(GC.getGame().getActiveTeam());
					u.air = shown->getDomainType() == DOMAIN_AIR;
					u.direction = int(shown->getFacingDirection(false));
					u.resting = Entities::resting(*shown);
					u.work = workAnimation(*shown);
					for (const Entities::Action& a : actions)
						if (a.unit.eOwner == shown->getOwner() && a.unit.iID == shown->getID())
						{
							u.action = a.mission;
							u.actionSince = std::chrono::duration<float>(std::chrono::steady_clock::now() - a.start).count();
							u.actionLength = a.seconds;
							u.actionX = a.targetX, u.actionY = a.targetY;
							u.actionEffect = a.effect;
						}
					u.count = seenCount;
					u.health = shown->maxHitPoints() > 0 ? float(shown->currHitPoints()) / float(shown->maxHitPoints()) : 1.0f;
					if (const char* button = shown->getButton())
						u.button = button;
					// The formation's figures: each art definition as many times as the unit
					// requires it, the rest round the definitions.
					const EraTypes era = player(u.owner).getCurrentEra();
					// (one figure a unit with the single unit graphics option)
					const int figures = gDLL->getGraphicOption(GRAPHICOPTION_SINGLE_UNIT_GRAPHICS) ? 1 : std::clamp(shown->getGroupSize(), 1, 12),
							  definitions = shown->getGroupDefinitions();
					std::vector<int> art;
					for (int d = 0; d < definitions; ++d)
						for (int r = 0; r < shown->getUnitGroupRequired(d) && int(art.size()) < figures; ++r)
							art.push_back(d);
					for (int k = int(art.size()); definitions > 0 && k < figures; ++k)
						art.push_back(k % definitions);
					for (const int d : art)
						if (const CvArtInfoUnit* info = shown->getArtInfo(d, era))
						{
							u.models.push_back(modelIndex(info, info->getNIF(), info->getKFM(), info->getScale(), info->getShadowNIF(), info->getShadowAttachNode(),
								info->getShadowScale()));
							s.models[size_t(u.models.back())].ranged = info->getActAsRanged();
						}
				}
			}

			if (CvCity* city = plot->getPlotCity())
			{
				KnownCity& known = m_cities[uint64_t(uint32_t(city->getOwner())) << 32 | uint32_t(city->getID())];
				const bool due = known.city != city || known.activePlayer != game.getActivePlayer() || known.turn != s.turn ||
					cityIndex++ % 64 == m_captures % 64;
				known.city = city, known.turn = s.turn, known.activePlayer = game.getActivePlayer(), known.seen = m_captures;
				render::CityView& c = known.view;
				c.x = plot->getX();
				c.y = plot->getY();
				c.owner = city->getOwner();
				c.capital = c.owner >= 0 && player(c.owner).getCapitalCity() == city;
				// The city bar as the original engine builds it.
				if (due || city->isInfoDirty())
				{
					city->setInfoDirty(false);
					CvWStringBuffer label;
					GAMETEXT.buildCityBillboardCityNameString(label, city);
					c.name = gameText(label.getCString());
					label.clear();
					GAMETEXT.buildCityBillboardCitySizeString(label, city, NiColorA(1, 1, 1, 1));
					c.population = std::atoi(gameText(label.getCString()).c_str());
					label.clear();
					GAMETEXT.buildCityBillboardProductionString(label, city);
					c.production = gameMarkup(label.getCString());
					NiColorA dot, number;
					city->getCityBillboardSizeIconColors(dot, number);
					c.sizeDot = { dot.r, dot.g, dot.b }, c.sizeText = { number.r, number.g, number.b };
					std::vector<NiColorA> colours;
					GAMETEXT.getCityBillboardFoodbarColors(city, colours);
					if (colours.size() > INFOBAR_STORED)
						c.foodBar = { colours[INFOBAR_STORED].r, colours[INFOBAR_STORED].g, colours[INFOBAR_STORED].b };
					colours.clear();
					GAMETEXT.getCityBillboardProductionbarColors(city, colours);
					if (colours.size() > INFOBAR_STORED)
						c.productionBar = { colours[INFOBAR_STORED].r, colours[INFOBAR_STORED].g, colours[INFOBAR_STORED].b };
					std::vector<float> bar;
					if (city->getFoodBarPercentages(bar) && bar.size() > INFOBAR_RATE)
					{
						c.foodStored = bar[INFOBAR_STORED];
						c.foodRate = bar[INFOBAR_RATE];
					}
					bar.clear();
					if (city->getProductionBarPercentages(bar) && bar.size() > INFOBAR_RATE)
					{
						c.productionStored = bar[INFOBAR_STORED];
						c.productionRate = bar[INFOBAR_RATE];
					}
				}
				// The layout's content, as the original engine asks for it. Mods (AND2) answer only
				// with graphics up, which the original's are all along: the host keeps the flag
				// off otherwise, so that the DLL does not drive entities it has not got.
				if (due || city->isLayoutDirty())
				{
					city->setLayoutDirty(false);
					c.buildings.clear();
					c.walls.clear();
					std::list<BuildingTypes> visible;
					const bool graphics = GC.IsGraphicsInitialized();
					GC.SetGraphicsInitialized(true);
					city->getVisibleBuildings(visible, c.generics);
					GC.SetGraphicsInitialized(graphics);
					for (const BuildingTypes b : visible)
						if (const CvArtInfoBuilding* art = city->getBuildingArtInfo(b))
						{
							// "LSYSTEM_3x2" (and "_NO_ROTATE"): the lots it takes; walls, harbours and
							// the like have layouts of their own (not yet).
							int w = 0, h = 0;
							const std::string lsystem = text(art->getLSystemName());
							if (std::sscanf(lsystem.c_str(), "LSYSTEM_%dx%d", &w, &h) == 2 && w > 0 && h > 0)
								c.buildings.push_back({ modelIndex(art, art->getNIF(), art->getKFM(), art->getScale()), w, h });
							else if (const auto shore = m_shoreBuildings.find(lsystem); shore != m_shoreBuildings.end())
								c.buildings.push_back({ modelIndex(art, art->getNIF(), art->getKFM(), art->getScale()), 0, 0, true, shore->second });
							else if (const auto wall = m_wallBuilders.find(lsystem); wall != m_wallBuilders.end())
							{
								// The generic walls: of the ancient world until the middle ages.
								const int era = player(c.owner).getCurrentEra();
								const std::string kind = era >= 2 ? "med" : "an";
								const std::string folder = "Art/Structures/Buildings/Walls/";
								render::CityWallView walls{ wall->second.first, wall->second.second, {} };
								if (walls.post.empty())
									walls.post = folder + "post01_eu_" + kind + ".nif";
								if (walls.wall.empty())
								{
									walls.wall = folder + "wall%1_eu_" + kind + ".nif";
									walls.gate = folder + "gate01_eu_" + kind + ".nif";
								}
								c.walls.push_back(std::move(walls));
							}
						}
					c.houseSet = c.owner >= 0 ? houseSet(player(c.owner).getCurrentEra(), city->getArtStyleType()) : -1;
				}
				s.cities.push_back(c);
			}
		}
		std::erase_if(m_cities, [&](const auto& known) { return known.second.seen != m_captures; });
		// The settlers that just founded a city: gone from the game, they found it on the map.
		for (const Entities::Founding& f : Entities::foundings())
		{
			render::UnitView& u = s.units.emplace_back();
			u.id = -1;
			u.x = f.x, u.y = f.y, u.owner = u.shownOwner = f.owner, u.direction = f.direction;
			u.founded = std::chrono::duration<float>(std::chrono::steady_clock::now() - f.at).count();
			for (const CvArtInfoUnit* info : f.figures)
			{
				u.models.push_back(modelIndex(info, info->getNIF(), info->getKFM(), info->getScale(), info->getShadowNIF(), info->getShadowAttachNode(),
					info->getShadowScale()));
				s.models[size_t(u.models.back())].ranged = info->getActAsRanged();
			}
		}
		return s;
	}

	std::string MapCapture::plotHelp(int x, int y) const
	{
		CvPlot* plot = GC.getMap().plot(x, y);
		if (!plot)
			return {};
		CvWStringBuffer help;
		GAMETEXT.getPlotHelp(plot, nullptr, nullptr, false, help);
		return gameMarkup(help.getCString());
	}

	std::string MapCapture::selectionInfo(Interface& selection) const
	{
		CvUnit* head = selection.getHeadSelectedUnit();
		if (!head)
			return {};
		CvWStringBuffer help;
		GAMETEXT.setUnitHelp(help, head, false, false);
		std::string text = gameText(help.getCString());
		if (const int n = selection.getLengthSelectionList(); n > 1)
			text += "\n(" + std::to_string(n) + " units selected)";
		return text;
	}

	render::MapWindow::PopupView MapCapture::popupView(const Interface::Popup& popup, std::vector<int>& itemOf) const
	{
		using Item = Interface::Popup::Item;
		using View = render::MapWindow::PopupView;
		View v;
		v.serial = popup.serial;
		v.header = gameMarkup(popup.header.c_str());
		v.okButton = popup.okButton;
		v.centered = popup.style == "Window_NoTitleBar_Style";
		itemOf.clear();
		const ButtonPopupTypes type = popup.info ? popup.info->getButtonPopupType() : BUTTONPOPUP_TEXT;
		for (size_t i = 0; i < popup.items.size(); ++i)
		{
			const Item& it = popup.items[i];
			// In the research popup the buttons that are not a choice open the technology tree:
			// not yet. (The production popup's examining the city opens its screen.)
			const WidgetTypes w = it.widget.m_eWidgetType;
			const bool choice = type != BUTTONPOPUP_CHOOSETECH || w == WIDGET_RESEARCH;
			if (it.kind == Item::Kind::Button && !choice)
				continue;
			View::Item& out = v.items.emplace_back();
			out.kind = static_cast<View::Item::Kind>(it.kind);
			out.text = it.kind == Item::Kind::Edit ? gameText(it.text.c_str()) : gameMarkup(it.text.c_str()); // (the screens' markup but in an edit box)
			out.icon = it.icon;
			out.group = it.kind == Item::Kind::Radio ? int(it.control) * 1000 + it.group : it.group; // (a pulldown's group is not the radio buttons')
			out.checked = it.checked;
			itemOf.push_back(int(i));
		}
		return v;
	}

	std::string MapCapture::popupHelp(const Interface::Popup& popup, int item) const
	{
		if (item < 0 || size_t(item) >= popup.items.size())
			return {};
		const Interface::Popup::Item& it = popup.items[size_t(item)];
		if (it.widget.m_eWidgetType == WIDGET_GENERAL)
			return gameMarkup(it.help.c_str());
		return widgetHelp(it.widget.m_eWidgetType, it.widget.m_iData1, it.widget.m_iData2, it.widget.m_bOption);
	}

	std::string MapCapture::widgetHelp(int widget, int data1, int data2, bool option) const
	{
		CvWStringBuffer help;
		CvWidgetDataStruct w{ data1, data2, option, static_cast<WidgetTypes>(widget) };
		CvDLLWidgetData::getInstance().parseHelp(help, w);
		return gameMarkup(help.getCString());
	}

	std::pair<int, int> MapCapture::focus() const
	{
		const PlayerTypes active = GC.getGame().getActivePlayer();
		if (active != NO_PLAYER)
		{
			if (const CvCity* capital = player(active).getCapitalCity())
				return { capital->getX(), capital->getY() };
			int iter = 0;
			if (const CvUnit* first = player(active).firstUnit(&iter)) // no city yet: the settlers
				return { first->getX(), first->getY() };
		}
		if (!m_snapshot.cities.empty()) // the active player has none: someone's
			return { m_snapshot.cities.front().x, m_snapshot.cities.front().y };
		return { -1, -1 };
	}

	std::string MapCapture::caption() const
	{
		int alive = 0;
		for (const auto& p : m_snapshot.players)
			alive += p.alive;
		return "turn " + std::to_string(m_snapshot.turn) + (m_snapshot.date.empty() ? "" : " (" + m_snapshot.date + ")") + ", " +
			std::to_string(m_snapshot.cities.size()) + " cities, " +
			std::to_string(m_snapshot.units.size()) + " unit stacks, " + std::to_string(alive) + " players";
	}
}
