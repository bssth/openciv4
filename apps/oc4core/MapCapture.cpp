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
#include "CvInfoWater.h"
#include "CvMap.h"
#include "CvPlayerAI.h"
#include "CvPlot.h"
#include "CvSelectionGroup.h"
#include "CvUnit.h"

#include <windows.h>

#include <pybind11/pybind11.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <list>
#include <numbers>

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
		if (GC.getNumWaterPlaneInfos() > 0)
		{
			CvWaterPlaneInfo& info = GC.getWaterPlaneInfo(0);
			s.water.texture = text(info.getBaseTexture());
			s.water.scale = info.getTextureScaling();
			s.water.rateU = info.getTextureScrollRateU();
			s.water.rateV = info.getTextureScrollRateV();
			s.water.alpha = info.getMaterialAlpha();
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
		loadPlotLSystem();
		loadFlagScale();
		loadAttachables(s);

		s.rivers.clear();
		for (int r = 0; r < GC.getNumRiverModelInfos(); ++r)
		{
			CvRiverModelInfo& info = GC.getRiverModelInfo(r);
			render::RiverArt art{ text(info.getModelFile()), directionMask(info.getConnectString()), render::RiverArt::Delta::None,
				numbers(info.getRotateString()), text(info.getBorderFile()) };
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

	// Flags have their own zoom-dependent scale, separate from UNIT_SCALE; the health bars their
	// size and place. Mods may replace them.
	void MapCapture::loadFlagScale()
	{
		const auto loc = m_assets.locate("xml/misc/civ4detailmanager.xml");
		if (!loc)
			return;
		std::string error;
		const auto doc = parseXml(m_assets.read(*loc), error);
		if (!doc)
			return;
		const std::pair<const wchar_t*, std::vector<std::pair<float, float>>*> wanted[] = { { L"FLAG_SCALE", &m_snapshot.flagScale },
			{ L"UNIT_HEALTH_BILLBOARD_SCALE", &m_snapshot.healthScale }, { L"UNIT_HEALTH_BILLBOARD_Z", &m_snapshot.healthZ },
			{ L"UNIT_HEALTH_BILLBOARD_RADIUS", &m_snapshot.healthRadius } };
		for (const auto& node : doc->children)
			if (node->type == XmlNode::Type::Element)
				for (const auto& fader : node->children)
				{
					const XmlNode* name = fader->type == XmlNode::Type::Element && fader->name == "Fader" ? fader->firstChildElement("Name") : nullptr;
					const auto target = name ? std::ranges::find(wanted, name->text, [](const auto& w) { return std::wstring_view(w.first); }) : std::end(wanted);
					if (target == std::end(wanted))
						continue;
					const bool scale = target->second == &m_snapshot.flagScale || target->second == &m_snapshot.healthScale;
					std::vector<std::pair<float, float>> keys;
					for (const auto& key : fader->children)
						if (key->type == XmlNode::Type::Element && key->name == "Key")
						{
							float distance = 0, value = 0;
							if (std::swscanf(key->text.c_str(), L"%f , %f", &distance, &value) == 2 && distance >= 0 && (value > 0 || !scale))
								keys.emplace_back(distance, value);
						}
					if (!keys.empty())
					{
						std::ranges::sort(keys);
						*target->second = std::move(keys);
					}
				}
	}

	FlagArt flagArt(int player)
	{
		FlagArt f;
		if (player < 0 || player >= GC.getMAX_PLAYERS())
			return f;
		const CvPlayerAI& pl = CvPlayerAI::getPlayerNonInl(static_cast<PlayerTypes>(player));
		f.primary = colorOf(pl.getPlayerColor(), true);
		f.secondary = colorOf(pl.getPlayerColor(), false);
		f.decal = utf8(pl.getFlagDecal().c_str());
		f.white = pl.isWhiteFlag();
		const int era = pl.getCurrentEra(), medieval = GC.getInfoTypeForString("ERA_MEDIEVAL", true), industrial = GC.getInfoTypeForString("ERA_INDUSTRIAL", true);
		f.model = ARTFILEMGR.getMiscArtInfo(industrial >= 0 && era >= industrial ? "ART_UNITGROUP_FLAG_MODERN" :
			medieval >= 0 && era >= medieval ? "ART_UNITGROUP_FLAG_MEDIEVAL" : "ART_UNITGROUP_FLAG_ANCIENT");
		return f;
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
			else if (a.type == "ROUTE_BRIDGE")
				s.routeBridge = a;
			else if (a.type == "RAILROAD_BRIDGE")
				s.railroadBridge = a;
		s.railroadRoute = GC.getInfoTypeForString("ROUTE_RAILROAD", true);
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
			auto& a = out.emplace_back(render::MapSnapshot::Attachable{ narrow(type->text), narrow(path->text), scale ? std::wcstof(scale->text.c_str(), nullptr) : 1.0f });
			if (const XmlNode* projectile = n->firstChildElement("bIsProjectile"))
				a.projectile = projectile->text == L"1";
			if (const XmlNode* speed = n->firstChildElement("fSpeed"))
				a.speed = std::wcstof(speed->text.c_str(), nullptr);
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

	bool MapCapture::LAttributes::allows(const std::string& cls, const std::string& value, const char* all, const char* none) const
	{
		const auto it = lists.find(cls);
		if (it == lists.end())
			return true;
		const auto has = [&](const std::string& v) { return std::ranges::find(it->second, v) != it->second.end(); };
		if (value.empty())
			return has(none);
		return !has("!" + value) && (has(value) || has(all));
	}

	std::string MapCapture::LAttributes::scalar(const std::string& name) const
	{
		for (const auto& [n, v] : scalars)
			if (n == name)
				return v;
		return {};
	}

	// The engine reads CIV4PlotLSystem.xml itself (the DLL never sees it). It lays out what
	// stands on a plot from it: here what farms take, the farm's buildings and its crops.
	void MapCapture::loadPlotLSystem()
	{
		m_plotNodes.clear();
		m_plotProductions.clear();
		const auto loc = m_assets.locate("xml/buildings/civ4plotlsystem.xml");
		if (!loc)
			return;
		std::string error;
		const auto doc = parseXml(m_assets.read(*loc), error);
		if (!doc)
		{
			std::printf("  map view: CIV4PlotLSystem.xml: %s\n", error.c_str());
			return;
		}
		const auto narrow = [](const std::wstring& w) {
			std::string out;
			for (const wchar_t c : w)
				if (c != L' ' && c != L'\t' && c != L'\r' && c != L'\n')
					out += char(c);
			return out;
		};
		const auto number = [](const XmlNode* n) { return n ? std::wcstof(n->text.c_str(), nullptr) : 0.0f; };
		const auto attributes = [&](const XmlNode& n, LAttributes& out) {
			for (const auto& a : n.children)
				if (const std::wstring* cls = a->type == XmlNode::Type::Element && a->name == "Attribute" ? a->attribute("Class") : nullptr)
				{
					const std::string v = narrow(a->text);
					if (*cls == L"Scalar")
					{
						if (const size_t colon = v.find(':'); colon != std::string::npos)
							out.scalars.emplace_back(v.substr(0, colon), v.substr(colon + 1));
						continue;
					}
					auto& list = out.lists[narrow(*cls)];
					size_t start = 0;
					for (size_t comma; (comma = v.find(',', start)) != std::string::npos; start = comma + 1)
						list.push_back(v.substr(start, comma - start));
					list.push_back(v.substr(start));
				}
		};
		std::vector<const XmlNode*> stack{ doc.get() };
		while (!stack.empty())
		{
			const XmlNode* n = stack.back();
			stack.pop_back();
			for (auto c = n->children.rbegin(); c != n->children.rend(); ++c)
				stack.push_back(c->get());
			if (n->type != XmlNode::Type::Element)
				continue;
			const std::wstring* name = n->attribute("Name");
			if (n->name == "LNode" && name)
			{
				auto& art = m_plotNodes[narrow(*name)];
				for (const auto& r : n->children)
				{
					const std::wstring* refName = r->type == XmlNode::Type::Element && r->name == "ArtRef" ? r->attribute("Name") : nullptr;
					if (!refName)
						continue;
					LArtRef ref;
					ref.name = narrow(*refName);
					attributes(*r, ref);
					ref.nif = ref.scalar("NIF");
					if (const size_t sep = ref.nif.find("::"); sep != std::string::npos)
					{
						ref.node = ref.nif.substr(sep + 2);
						ref.nif.resize(sep);
					}
					ref.kfm = ref.scalar("KFM");
					if (const XmlNode* scale = r->firstChildElement("Scale"))
						ref.scale = number(scale);
					ref.rotate = number(r->firstChildElement("Rotate"));
					art.push_back(std::move(ref));
				}
			}
			else if (n->name == "LProduction")
			{
				LProduction p;
				if (const std::wstring* from = n->attribute("From"))
					p.from = narrow(*from);
				if (name)
					p.name = narrow(*name);
				attributes(*n, p);
				for (const auto& t : n->children)
				{
					const std::wstring* to = t->type == XmlNode::Type::Element && t->name == "To" ? t->attribute("Name") : nullptr;
					if (!to)
						continue;
					LProduction::To& out = p.to.emplace_back();
					out.node = narrow(*to);
					if (const XmlNode* tr = t->firstChildElement("Translate"))
						std::swscanf(tr->text.c_str(), L" %f , %f", &out.x, &out.y);
					out.rotate = number(t->firstChildElement("Rotate"));
				}
				m_plotProductions.push_back(std::move(p));
			}
		}
	}

	// A farm as CIV4PlotLSystem.xml lays it out: its farmhouse (Leaf_Farm_4x4's goal) and the
	// crops grown out from its 4x4 node; rice and wheat only the buildings, in a corner. -1: none
	// (not a farm, or nothing of it in the file: the improvement's own model is drawn).
	int MapCapture::plotLayout(const CvPlot& plot, int bonus, int shownImprovement, bool worked, int shownOwner)
	{
		// The improvement the player sees there (CvPlot::getVisibleImprovementState), as the
		// original lays out: on a worked plot without one, IMPROVEMENT_LAND_WORKED (a small
		// farmhouse) or IMPROVEMENT_WATER_WORKED (nets).
		const ImprovementTypes improvement = static_cast<ImprovementTypes>(shownImprovement);
		if (improvement == NO_IMPROVEMENT || m_plotNodes.empty())
			return -1;
		const std::string improvementName = text(GC.getImprovementInfo(improvement).getType());
		// (an unowned plot: as the player looking at it, as the original draws it; Wine's test
		// map has the industrial quarry and pasture outside any border for an industrial Rome)
		const PlayerTypes owner = shownOwner != NO_PLAYER ? static_cast<PlayerTypes>(shownOwner) : GC.getGame().getActivePlayer();
		const int era = owner != NO_PLAYER ? int(player(owner).getCurrentEra()) : int(GC.getGame().getCurrentEra());
		const int artStyle = owner != NO_PLAYER ? int(player(owner).getArtStyleType()) : -1;
		const bool irrigated = plot.isIrrigationAvailable();
		const uint32_t variant = (uint32_t(plot.getX()) * 73856093u) ^ (uint32_t(plot.getY()) * 19349663u);
		const uint64_t key = uint64_t(uint8_t(era)) | uint64_t(uint16_t(bonus + 1)) << 8 | uint64_t(worked) << 24 | uint64_t(irrigated) << 25 |
			uint64_t(variant % 64) << 26 | uint64_t(uint16_t(improvement)) << 32 | uint64_t(uint8_t(artStyle + 1)) << 48;
		if (const auto it = m_plotLayouts.find(key); it != m_plotLayouts.end())
			return it->second;

		const std::string eraName = era >= 0 && era < GC.getNumEraInfos() ? text(GC.getEraInfo(static_cast<EraTypes>(era)).getType()) : "";
		const std::string bonusName = bonus >= 0 && bonus < GC.getNumBonusInfos() ? text(GC.getBonusInfo(static_cast<BonusTypes>(bonus)).getType()) : "";
		const std::string styleName = artStyle >= 0 && artStyle < GC.getNumArtStyleTypes() ? std::string(GC.getArtStyleTypes(static_cast<ArtStyleTypes>(artStyle))) : "";
		const auto fits = [&](const LAttributes& a) {
			return a.allows("Era", eraName, "ERA_ALL", "NO_ERA") && a.allows("Bonus", bonusName, "BONUS_ALL", "NO_BONUS") &&
				a.allows("Improvement", improvementName, "IMPROVEMENT_ALL", "NO_IMPROVEMENT") &&
				(styleName.empty() || a.allows("ArtStyle", styleName, "ARTSTYLE_ALL", "NO_ARTSTYLE"));
		};
		uint32_t random = variant * 2654435761u + 1;
		const auto next = [&] { return (random = random * 1664525u + 1013904223u) >> 8; };
		const auto production = [&](const std::string& from, auto&& also) -> const LProduction* {
			std::vector<const LProduction*> found;
			for (const LProduction& p : m_plotProductions)
				if (p.from == from && fits(p) && also(p))
					found.push_back(&p);
			return found.empty() ? nullptr : found[next() % found.size()];
		};
		const auto artOf = [&](const std::string& node, auto&& also) -> const LArtRef* {
			const auto it = m_plotNodes.find(node);
			if (it == m_plotNodes.end())
				return nullptr;
			std::vector<const LArtRef*> found;
			for (const LArtRef& r : it->second)
				if (fits(r) && also(r))
					found.push_back(&r);
			return found.empty() ? nullptr : found[next() % found.size()];
		};
		const auto any = [](const auto&) { return true; };
		const auto placesFarm = [](const LProduction& p) {
			return std::ranges::any_of(p.to, [](const LProduction::To& t) { return t.node == "Farm_4x4"; });
		};

		std::vector<render::PlotPiece> pieces;
		const LArtRef* goal = artOf("Leaf_Farm_4x4", [&](const LArtRef& r) { return r.name == "goal:" + improvementName && !r.nif.empty(); });
		const LProduction* farm = production("Farm_4x4", any);
		if (goal && farm)
		{
			// A leaf of crops the root's productions came down to, in the plot's 12x12 cells,
			// and the 4x4 node it lies in (-1: none).
			struct Leaf
			{
				std::string node;
				float x = 0, y = 0, degrees = 0;
				int slot = -1;
			};
			struct Slot
			{
				float x = 0, y = 0, degrees = 0;
			};
			std::vector<Leaf> leaves;
			std::vector<Slot> slots;
			// Down from a node to its leaves of crops (the buildings' leaves, BLeaf_*, have no
			// farm in them; the farm's own production is the goal's alone).
			const auto expand = [&](auto&& self, const std::string& node, float x, float y, float degrees, int slot, int depth) -> void {
				if (node.rfind("GLeaf_", 0) == 0)
				{
					leaves.push_back({ node, x, y, degrees, slot });
					return;
				}
				if (depth > 8 || (node.rfind("Node_", 0) != 0 && node.rfind("GNode_", 0) != 0))
					return;
				if (node == "Node_4x4" && slot < 0)
				{
					slot = int(slots.size());
					slots.push_back({ x, y, degrees });
				}
				const LProduction* p = production(node, [&](const LProduction& q) {
					return q.scalar("bGoalOnly") != "1" && q.scalar("bExtraGoalOnly") != "1" && !placesFarm(q);
				});
				if (!p)
					return;
				const float rc = std::cos(degrees * std::numbers::pi_v<float> / 180), rs = std::sin(degrees * std::numbers::pi_v<float> / 180);
				for (const LProduction::To& t : p->to)
					self(self, t.node, x + t.x * rc - t.y * rs, y + t.x * rs + t.y * rc, degrees + t.rotate, slot, depth + 1);
			};

			// Where the farm's 4x4 node stands: a corner (rice, wheat), else one of the 4x4
			// nodes the root came down to, near the middle of the plot.
			float ox = 0, oy = 0, odeg = 0, twist = 0;
			std::vector<Leaf> crops;
			if (const LProduction* corner = production("Node_12x12", [&](const LProduction& p) {
					return p.lists.contains("Improvement") && p.to.size() == 1 && p.to[0].node == "Farm_4x4";
				}))
			{
				ox = corner->to[0].x;
				oy = corner->to[0].y;
			}
			else
			{
				twist = std::strtof(goal->scalar("fTwist").c_str(), nullptr);
				for (int attempt = 0; attempt < 8 && slots.empty(); ++attempt)
				{
					leaves.clear();
					expand(expand, "Node_12x12", 0, 0, 0, -1, 0);
				}
				int chosen = -1;
				float best = 1e9f;
				for (size_t i = 0; i < slots.size(); ++i)
					if (const float d = std::hypot(slots[i].x, slots[i].y) + float(next() % 1000) / 1000 * 3; d < best)
						best = d, chosen = int(i);
				if (chosen >= 0)
					std::tie(ox, oy, odeg) = std::tuple{ slots[size_t(chosen)].x, slots[size_t(chosen)].y, slots[size_t(chosen)].degrees };
				else
					std::tie(ox, oy) = std::pair{ -2.0f, -2.0f };
				std::erase_if(leaves, [&](const Leaf& l) { return l.slot == chosen && chosen >= 0; });
				// The farm's own crops, then the leaves nearest it (a little at random) until the
				// crops cover the ground the farmhouse asks for.
				if (production("Node_4x4", [&](const LProduction& p) {
						return placesFarm(p) && std::ranges::any_of(p.to, [](const LProduction::To& t) { return t.node == "GNode_4x4"; });
					}))
				{
					const size_t before = leaves.size();
					expand(expand, "GNode_4x4", ox, oy, odeg, -2, 0);
					crops.assign(leaves.begin() + std::ptrdiff_t(before), leaves.end());
					leaves.resize(before);
					std::vector<std::pair<float, const Leaf*>> nearest;
					for (const Leaf& l : leaves)
						nearest.push_back({ std::hypot(l.x - ox, l.y - oy) + float(next() % 1000) / 1000 * 2.5f, &l });
					std::ranges::sort(nearest, {}, &std::pair<float, const Leaf*>::first);
					const auto cells = [](const Leaf& l) {
						int w = 0, h = 0;
						std::sscanf(l.node.c_str() + 6, "%dx%d", &w, &h);
						return w * h;
					};
					int area = 0;
					for (const Leaf& l : crops)
						area += cells(l);
					const int wanted = std::atoi(goal->scalar("iCropAreaMultiplier").c_str()) * 4;
					for (const auto& [d, l] : nearest)
					{
						if (area >= wanted)
							break;
						crops.push_back(*l);
						area += cells(*l);
					}
				}
			}
			const float c = std::cos(twist), sn = std::sin(twist);
			const auto add = [&](const LArtRef& art, float scale, float x, float y, float degrees) {
				render::PlotPiece& piece = pieces.emplace_back();
				piece.model = modelIndex(&art, art.nif.c_str(), art.kfm.c_str(), scale);
				piece.node = art.node;
				piece.x = (x * c - y * sn) / 12;
				piece.y = (x * sn + y * c) / 12;
				piece.yaw = twist + degrees * std::numbers::pi_v<float> / 180;
			};
			// The crops of a leaf (GLeaf_*), worked or not, irrigated or not.
			for (const Leaf& l : crops)
			{
				const LArtRef* crop = artOf(l.node, [&](const LArtRef& r) {
					const std::string w = r.scalar("bImprovementWorkedOnly"), i = r.scalar("bIrrigatedOnly");
					return !r.nif.empty() && !r.node.empty() && r.lists.contains("Improvement") && (w.empty() || (w == "1") == worked) &&
						(i.empty() || (i == "1") == irrigated);
				});
				if (!crop)
					continue;
				// (a 3x2 leaf's crops are the file's 2x3 piece: turned)
				int w = 0, h = 0, aw = 0, ah = 0;
				std::sscanf(l.node.c_str() + 6, "%dx%d", &w, &h);
				std::sscanf(crop->node.c_str(), "%dx%d", &aw, &ah);
				add(*crop, crop->scale, l.x, l.y, l.degrees + (w != h && aw == h && ah == w ? 90.0f : 0.0f));
			}
			const float improvementScale = defineOr("IMPROVEMENT_SCALE", 1);
			std::vector<const LProduction::To*> spots;
			for (const LProduction::To& t : farm->to)
				if (t.node == "Leaf_Farm_4x4")
					add(*goal, goal->scale * improvementScale, ox + t.x, oy + t.y, goal->rotate);
				else
					spots.push_back(&t);
			for (const auto& [n, extra] : goal->scalars)
				if (n == "szExtra" && !spots.empty())
				{
					const LProduction::To* spot = spots[next() % spots.size()];
					if (const LArtRef* art = artOf(spot->node, [&](const LArtRef& r) { return r.name == extra && !r.nif.empty(); }))
						add(*art, art->scale * improvementScale, ox + spot->x, oy + spot->y, art->rotate);
					std::erase(spots, spot);
				}
		}
		else if (improvementName != "IMPROVEMENT_FARM")
			layOutImprovement(improvementName, improvement, pieces, fits, next, worked);
		const int index = pieces.empty() ? -1 : int(m_snapshot.plotLayouts.size());
		if (!pieces.empty())
			m_snapshot.plotLayouts.push_back(std::move(pieces));
		m_plotLayouts.emplace(key, index);
		return index;
	}

	// An improvement other than the farm as CIV4PlotLSystem.xml lays it out. The plot root puts
	// some goals straight on the plot (mine, quarry, pasture, windmill, fort:
	// Leaf_LandImprovement_4x4; whaling boats: Leaf_Water_Improvement_4x4), with the era's art
	// (no NIF in the goal: the art define's model). A cottage, hamlet, village and town are
	// houses of the era and art style over iCropArea cells round a lot near the plot's middle
	// (their goal, bPlace:0, has no model of its own). A plantation and a winery are their goal
	// on a leaf near the middle, the goal's szExtra beside it and its crops round it. Nothing
	// for an improvement the file has no goal for: its art define's model is drawn.
	void MapCapture::layOutImprovement(const std::string& name, int improvement, std::vector<render::PlotPiece>& pieces,
		const std::function<bool(const LAttributes&)>& fits, const std::function<uint32_t()>& next, bool worked)
	{
		const float improvementScale = defineOr("IMPROVEMENT_SCALE", 1);
		const std::string goalName = "goal:" + name;
		// (bImprovementWorkedOnly, bBonusWorkedOnly: worked or not)
		const auto workedFits = [&](const LArtRef& r) {
			for (const char* s : { "bImprovementWorkedOnly", "bBonusWorkedOnly" })
				if (const std::string v = r.scalar(s); !v.empty() && (v == "1") != worked)
					return false;
			return true;
		};
		const auto artOf = [&](const std::string& node, auto&& also) -> const LArtRef* {
			const auto it = m_plotNodes.find(node);
			if (it == m_plotNodes.end())
				return nullptr;
			std::vector<const LArtRef*> found;
			for (const LArtRef& r : it->second)
				if (fits(r) && workedFits(r) && also(r))
					found.push_back(&r);
			return found.empty() ? nullptr : found[next() % found.size()];
		};
		const auto twistOf = [](const LArtRef& r) { return std::strtof(r.scalar("fTwist").c_str(), nullptr); };
		// (Rotation: the art turned by one of these, in degrees)
		const auto turnOf = [&](const LArtRef& r) {
			const auto it = r.lists.find("Rotation");
			return it == r.lists.end() || it->second.empty() ? 0.0f : std::strtof(it->second[next() % it->second.size()].c_str(), nullptr);
		};
		constexpr float kDegrees = std::numbers::pi_v<float> / 180;
		// (fCenterCut: the trees round it go, in world units)
		const auto centerCut = [](const LArtRef& r) {
			const float plot = std::max(float(GC.getPLOT_SIZE()), 1.0f);
			return std::strtof(r.scalar("fCenterCut").c_str(), nullptr) / plot;
		};
		const auto whole = [&](const LArtRef& goal) {
			if (!goal.nif.empty())
				return modelIndex(&goal, goal.nif.c_str(), goal.kfm.c_str(), goal.scale * improvementScale);
			const CvArtInfoImprovement* art = GC.getImprovementInfo(static_cast<ImprovementTypes>(improvement)).getArtInfo();
			return art ? modelIndex(art, art->getNIF(), art->getKFM(), art->getScale() * improvementScale) : -1;
		};

		// Straight from the root, to a leaf with the goal: at once, or through nodes between
		// (the water's nets: PLOT_ROOT -> Water_Worked_4x4 -> Leaf_Water_Worked_4x4, turned -40).
		// Of the root's productions that get there, one (WaterWorkedProd: five places).
		struct Reached
		{
			const LArtRef* goal = nullptr;
			float x = 0, y = 0, degrees = 0;
		};
		std::vector<Reached> reached;
		const auto reach = [&](auto&& self, const std::string& node, float x, float y, float degrees, int depth) -> std::optional<Reached> {
			if (node.rfind("Leaf_", 0) == 0)
			{
				if (const LArtRef* goal = artOf(node, [&](const LArtRef& r) { return r.name == goalName; }))
					return Reached{ goal, x, y, degrees };
				return std::nullopt;
			}
			if (depth > 4)
				return std::nullopt;
			for (const LProduction& p : m_plotProductions)
				if (p.from == node && fits(p))
					for (const LProduction::To& t : p.to)
					{
						const float rc = std::cos(degrees * kDegrees), rs = std::sin(degrees * kDegrees);
						if (auto r = self(self, t.node, x + t.x * rc - t.y * rs, y + t.x * rs + t.y * rc, degrees + t.rotate, depth + 1))
							return r;
					}
			return std::nullopt;
		};
		for (const LProduction& p : m_plotProductions)
		{
			if (p.from != "PLOT_ROOT" || !p.lists.contains("Improvement") || !fits(p))
				continue;
			for (const LProduction::To& to : p.to)
				if (to.node.rfind("Node_", 0) != 0)
					if (auto r = reach(reach, to.node, to.x, to.y, to.rotate, 0))
						reached.push_back(*r);
		}
		if (!reached.empty())
		{
			const Reached& r = reached[next() % reached.size()];
			const LArtRef* goal = r.goal;
			render::PlotPiece& piece = pieces.emplace_back();
			piece.model = whole(*goal);
			piece.x = r.x / 12;
			piece.y = r.y / 12;
			// (a goal with an fTwist, the forts: turned by it and a quarter more, which puts
			// the tower on the left corner and the gate front right, as the original draws
			// them in every era)
			piece.yaw = twistOf(*goal) + (r.degrees + turnOf(*goal) + goal->rotate + (goal->scalar("fTwist").empty() ? 0.0f : 90.0f)) * kDegrees;
			piece.cut = centerCut(*goal);
			return;
		}

		// Its goal on a leaf of the plot's 12x12 cells.
		const LArtRef* goal = nullptr;
		std::string goalNode;
		for (const auto& [node, refs] : m_plotNodes)
			if (node.rfind("BLeaf_", 0) == 0)
				if (const LArtRef* g = artOf(node, [&](const LArtRef& r) { return r.name == goalName; }))
				{
					goal = g, goalNode = node;
					break;
				}
		if (!goal)
			return;
		struct Leaf
		{
			std::string node;
			float x = 0, y = 0, degrees = 0;
		};
		std::vector<Leaf> leaves;
		const auto expand = [&](auto&& self, const std::string& node, float x, float y, float degrees, int depth) -> void {
			if (node.rfind("BLeaf_", 0) == 0)
			{
				leaves.push_back({ node, x, y, degrees });
				return;
			}
			if (depth > 8 || node.rfind("Node_", 0) != 0)
				return;
			std::vector<const LProduction*> found;
			for (const LProduction& p : m_plotProductions)
				if (p.from == node && fits(p) && p.scalar("bGoalOnly") != "1" && p.scalar("bExtraGoalOnly") != "1")
					found.push_back(&p);
			if (found.empty())
				return;
			const LProduction& p = *found[next() % found.size()];
			const float rc = std::cos(degrees * kDegrees), rs = std::sin(degrees * kDegrees);
			for (const LProduction::To& t : p.to)
				self(self, t.node, x + t.x * rc - t.y * rs, y + t.x * rs + t.y * rc, degrees + t.rotate, depth + 1);
		};
		expand(expand, "Node_12x12", 0, 0, 0, 0);
		const auto cells = [](const std::string& node) {
			int w = 0, h = 0;
			std::sscanf(node.c_str() + 6, "%dx%d", &w, &h);
			return w * h;
		};
		// The goal's lot: a leaf of its size near the middle (none: the middle itself).
		float gx = 0, gy = 0, gdeg = 0;
		{
			int chosen = -1;
			float best = 1e9f;
			for (size_t i = 0; i < leaves.size(); ++i)
				if (leaves[i].node == goalNode)
					if (const float d = std::hypot(leaves[i].x, leaves[i].y) + float(next() % 1000) / 1000 * 3; d < best)
						best = d, chosen = int(i);
			if (chosen >= 0)
			{
				std::tie(gx, gy, gdeg) = std::tuple{ leaves[size_t(chosen)].x, leaves[size_t(chosen)].y, leaves[size_t(chosen)].degrees };
				if (goal->scalar("bPlace") == "0")
					leaves.insert(leaves.begin(), leaves[size_t(chosen)]); // (its lot is the first of the houses')
				leaves.erase(leaves.begin() + std::ptrdiff_t(chosen) + (goal->scalar("bPlace") == "0" ? 1 : 0));
			}
		}
		const float twist = twistOf(*goal), c = std::cos(twist), sn = std::sin(twist);
		const auto add = [&](const LArtRef& art, float x, float y, float degrees, const std::string& leaf) {
			render::PlotPiece& piece = pieces.emplace_back();
			// (bCutTrees: the trees on its leaf go)
			if (art.scalar("bCutTrees") == "1" && leaf.size() > 6)
			{
				int w = 0, h = 0;
				std::sscanf(leaf.c_str() + 6, "%dx%d", &w, &h);
				piece.cut = std::hypot(float(w), float(h)) / 2 / 12;
			}
			piece.cut = std::max(piece.cut, centerCut(art));
			piece.node = art.node;
			piece.model = art.node.empty() ? whole(art) : modelIndex(&art, art.nif.c_str(), "", art.scale);
			piece.standing = !art.node.empty();
			piece.x = (x * c - y * sn) / 12;
			piece.y = (x * sn + y * c) / 12;
			piece.yaw = twist + (degrees + turnOf(art) + art.rotate) * kDegrees;
		};
		if (goal->scalar("bPlace") != "0")
			add(*goal, gx, gy, gdeg, goalNode);
		else if (const float cut = centerCut(*goal); cut > 0)
		{
			// (no model: only the clearing round its lot)
			render::PlotPiece& piece = pieces.emplace_back();
			piece.x = (gx * c - gy * sn) / 12;
			piece.y = (gx * sn + gy * c) / 12;
			piece.cut = cut;
		}
		// The rest by their distance from the goal, a little at random.
		std::vector<std::pair<float, const Leaf*>> nearest;
		for (const Leaf& l : leaves)
			nearest.push_back({ std::hypot(l.x - gx, l.y - gy) + float(next() % 1000) / 1000 * 2.5f, &l });
		std::ranges::stable_sort(nearest, {}, &std::pair<float, const Leaf*>::first);
		std::vector<bool> used(nearest.size());
		// Its extra buildings (szExtra), each on the nearest leaf that has that art.
		for (const auto& [n, extra] : goal->scalars)
			if (n == "szExtra")
				for (size_t i = 0; i < nearest.size(); ++i)
					if (!used[i])
						if (const LArtRef* art = artOf(nearest[i].second->node, [&](const LArtRef& r) { return r.name == extra && !r.nif.empty(); }))
						{
							add(*art, nearest[i].second->x, nearest[i].second->y, nearest[i].second->degrees, nearest[i].second->node);
							used[i] = true;
							break;
						}
		// Houses or crops round it until they cover iCropArea cells.
		const bool houses = goal->scalar("bPlace") == "0";
		const int wanted = std::atoi(goal->scalar("iCropArea").c_str());
		int area = 0;
		for (size_t i = 0; i < nearest.size() && area < wanted; ++i)
		{
			if (used[i])
				continue;
			const Leaf& l = *nearest[i].second;
			const LArtRef* art = artOf(l.node, [&](const LArtRef& r) {
				return !r.nif.empty() && (houses ? r.name == "art:building" : r.name != goalName && r.scalar("bExtraGoalOnly") != "1" && r.lists.contains("Improvement"));
			});
			if (!art)
				continue;
			add(*art, l.x, l.y, l.degrees, l.node);
			area += cells(l.node);
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

	bool MapCapture::improvementTradesBonus(int improvement, int bonus)
	{
		const int improvements = GC.getNumImprovementInfos(), bonuses = GC.getNumBonusInfos();
		if (improvement < 0 || bonus < 0 || improvement >= improvements || bonus >= bonuses)
			return false;
		if (m_bonusTrade.size() != size_t(improvements) * size_t(bonuses))
		{
			// (CvImprovementInfo::isImprovementBonusTrade is the DLL's Python API, not its exported C++)
			namespace py = pybind11;
			m_bonusTrade.assign(size_t(improvements) * size_t(bonuses), 0);
			try
			{
				const py::object gc = py::module_::import("CvPythonExtensions").attr("CyGlobalContext")();
				for (int i = 0; i < improvements; ++i)
				{
					const py::object info = gc.attr("getImprovementInfo")(i);
					for (int b = 0; b < bonuses; ++b)
						m_bonusTrade[size_t(i) * size_t(bonuses) + size_t(b)] = int8_t(info.attr("isImprovementBonusTrade")(b).cast<bool>());
				}
			}
			catch (const py::error_already_set& e) { std::fprintf(stderr, "[oc4core] improvement trades: %s\n", e.what()); }
		}
		return m_bonusTrade[size_t(improvement) * size_t(bonuses) + size_t(bonus)] != 0;
	}

	// A plot's yields as the player sees them over it: CvPlot::calculateYield(eYield, bDisplay),
	// the original's plot symbols. Its owner, improvement and road as the active team last saw them
	// (under the fog, what was there then), the resource's share by what the owner's team knows (a
	// foreign mine on iron counts the iron's hammer before the player can see the iron; on an
	// unowned plot by what the player's team knows), a city's only once seen. Where all that is
	// the plot as it is, they are its own yields (CvPlot::getYield); the rest asked through Python
	// (calculateYield is the DLL's Python API, not its exported C++), again only when what they
	// come from changes. The WorldBuilder, and the debug mode, show the plot's own.
	void MapCapture::shownYields(const CvPlot& plot, int index, bool everything, int activeTeam, uint8_t (&yields)[3])
	{
		const TeamTypes active = static_cast<TeamTypes>(activeTeam);
		for (int y = 0; y < 3; ++y)
			yields[y] = uint8_t(std::clamp(plot.getYield(static_cast<YieldTypes>(y)), 0, 255));
		if (everything || active == NO_TEAM)
			return;
		const PlayerTypes owner = plot.getRevealedOwner(active, false);
		const ImprovementTypes improvement = plot.getRevealedImprovementType(active, false);
		const RouteTypes route = plot.getRevealedRouteType(active, false);
		const CvCity* city = plot.getPlotCity();
		const bool cityUnseen = city && !city->isRevealed(active, false);
		const bool differs = owner != plot.getOwner() || improvement != plot.getImprovementType() || route != plot.getRouteType() || cityUnseen ||
			(owner == NO_PLAYER && (plot.getBonusType() != NO_BONUS || improvement != NO_IMPROVEMENT || route != NO_ROUTE));
		if (!differs)
		{
			m_shownYields.erase(index);
			return;
		}
		const TeamTypes knowing = owner != NO_PLAYER ? player(owner).getTeam() : active;
		const std::array<int, 8> from{ int(owner), int(improvement), int(route), int(plot.getBonusType(knowing)),
			int(yields[0]) | int(yields[1]) << 8 | int(yields[2]) << 16, int(cityUnseen), int(GC.getGame().getActivePlayer()), GC.getGame().getGameTurn() };
		if (const auto it = m_shownYields.find(index); it != m_shownYields.end() && it->second.from == from)
		{
			std::copy(it->second.yields.begin(), it->second.yields.end(), yields);
			return;
		}
		namespace py = pybind11;
		ShownYields& shown = m_shownYields[index];
		shown.from = from;
		try
		{
			const py::object p = py::module_::import("CvPythonExtensions").attr("CyMap")().attr("plot")(plot.getX(), plot.getY());
			for (int y = 0; y < 3; ++y)
				yields[y] = uint8_t(std::clamp(p.attr("calculateYield")(y, true).cast<int>(), 0, 255));
		}
		catch (const py::error_already_set& e) { std::fprintf(stderr, "[oc4core] plot yields: %s\n", e.what()); }
		std::copy(yields, yields + 3, shown.yields.begin());
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
		s.flagOffset = { GC.getDefineFLOAT("FLAG_OFFSET_X"), GC.getDefineFLOAT("FLAG_OFFSET_Y") };
		const auto miscModel = [&](const char* tag) {
			const CvArtInfoMisc* art = ARTFILEMGR.getMiscArtInfo(tag);
			return art ? modelIndex(art, art->getNIF(), art->getKFM(), art->getScale()) : -1;
		};
		s.flagGlow = miscModel("ART_UNITGROUP_FLAG_GLOW");
		// (no art define names the flag's medallions: the engine's own files, as the original's)
		static const char medallionsArt = 0;
		s.flagMedallions = modelIndex(&medallionsArt, "Art/Units/Flag/Medallions.nif", "Art/Units/Flag/Medallions.kfm", 1);
		s.cameraTravelDistance = defineOr("CAMERA_MAX_TRAVEL_DISTANCE", 3000);
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
			const FlagArt flag = flagArt(p);
			v.primary = flag.primary;
			v.secondary = flag.secondary;
			v.flagDecal = flag.decal;
			v.whiteFlag = flag.white;
			v.flagModel = flag.model ? modelIndex(flag.model, flag.model->getNIF(), flag.model->getKFM(), flag.model->getScale()) : -1;
			v.name = utf8(pl.getName());
			v.civilization = utf8(pl.getCivilizationShortDescription());
			v.score = game.getPlayerScore(static_cast<PlayerTypes>(p));
			v.cities = pl.getNumCities();
			v.barbarian = p == GC.getBARBARIAN_PLAYER();
		}

		const TeamTypes active = game.getActiveTeam();
		// (the WorldBuilder shows the map as it is, not as the active team knows it)
		const bool everything = gDLL->GetWorldBuilderMode() || game.isDebugMode();
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
			v.surrender = b.surrender;
			v.firstStrikes[0] = b.firstStrikes[0], v.firstStrikes[1] = b.firstStrikes[1];
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
			// The plot as the active team knows it, as the original draws it (CvPlot's
			// getVisibleImprovementState and getVisibleBonusState, the engine's): its owner, improvement
			// and road as last seen (under the fog, what was there then); a resource only with the
			// tech that reveals it (iron without Iron Working is not there, in anyone's land), though
			// an improvement on it shows (a mine on flat land). The WorldBuilder (and the debug
			// mode) shows the plot as it is: every resource, improvement, road and border.
			{
				ImprovementTypes shown = NO_IMPROVEMENT;
				bool worked = false;
				plot->getVisibleImprovementState(shown, worked);
				if (everything)
				{
					// (on a worked plot without one, the worked look, as the original's)
					if (plot->getImprovementType() != NO_IMPROVEMENT || !worked)
						shown = plot->getImprovementType();
				}
				v.improvement = int16_t(shown);
				// (the improvement's KFM: On, a worked plot's (the mine's cart and smoke, ...); NotWorked)
				v.improvementState = uint8_t(shown == NO_IMPROVEMENT ? 0 : worked ? 3 : 2);
				v.owner = int16_t(everything || active == NO_TEAM ? plot->getOwner() : plot->getRevealedOwner(active, false));
				v.bonus = int16_t(everything || active == NO_TEAM ? plot->getBonusType() : plot->getBonusType(active));
				v.route = int16_t(everything || active == NO_TEAM ? plot->getRouteType() : plot->getRevealedRouteType(active, false));
				// (a resource drawn improved when the improvement on it trades it: the mine on iron;
				// worked when the plot is: CvPlot::getVisibleBonusState's)
				const bool improved = v.bonus >= 0 && shown != NO_IMPROVEMENT && improvementTradesBonus(shown, v.bonus);
				v.bonusState = uint8_t(!improved ? 1 : worked ? 3 : 2);
				v.layout = int16_t(plotLayout(*plot, v.bonus, shown, worked, v.owner));
			}
			v.water = plot->isWater();
			v.hills = plot->isHills();
			v.peak = plot->isPeak();
			v.riverSouth = plot->isNOfRiver();
			v.riverEast = plot->isWOfRiver();
			v.revealed = active == NO_TEAM || plot->isRevealed(active, false);
			v.visible = active == NO_TEAM || plot->isVisible(active, false);
			if (v.revealed)
				shownYields(*plot, i, everything, active, v.yields);

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
				// (one intercepting a plane: it, while it shoots)
				for (int u = 0; u < count; ++u)
					if (CvUnit* unit = plot->getUnitByIndex(u); seen(unit))
						for (const Entities::Action& a : actions)
							if (a.mission == 4 && a.unit.eOwner == unit->getOwner() && a.unit.iID == unit->getID())
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
					if (const auto movement = Entities::movement(shown->getIDInfo(), plot->getX(), plot->getY()))
					{
						for (const auto [x, y] : movement->plots)
							u.moveSteps.push_back({ x, y });
						u.moveSeconds = movement->seconds;
					}
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
					for (int k = 0; k < count && u.medallions.size() < 7; ++k)
						if (const CvUnit* any = plot->getUnitByIndex(k); seen(any))
							u.medallions.push_back(!selection || !selection->isSelected(any) || any->getOwner() != GC.getGame().getActivePlayer() ? 3
								: !any->canMove() ? 2 : any->hasMoved() ? 4 : 1);
					u.health = shown->maxHitPoints() > 0 ? float(shown->currHitPoints()) / float(shown->maxHitPoints()) : 1.0f;
					// (the health bar's length, as the engine asks the DLL for it: the unit's strength
					// now times CvUnit::getHealthBarModifier, HEALTH_BAR_WIDTH over twice the best land
					// unit's strength; none for a unit without strength, nor before there is a best
					// land unit: the modifier is then not finite. Only DllExport functions: the
					// Windows DLL exports nothing else.)
					if (const float modifier = shown->getHealthBarModifier(); std::isfinite(modifier))
						u.healthBar = (shown->getDomainType() == DOMAIN_AIR ? shown->airCurrCombatStrFloat(nullptr) : shown->currCombatStrFloat(nullptr, nullptr)) *
							modifier;
					u.promotionReady = shown->isPromotionReady();
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
							s.models[size_t(u.models.back())].battleDistance = info->getBattleDistance();
						}
					// Where they stand: the formation (CvUnitFormationInfo, CIV4FormationInfos.xml) of the
					// unit's FormationType for what it does (its entity event: a battle's, a worker's
					// build's, the mission a waiting unit was told; else idle), the first with as many
					// unit entries as it has figures; and where its general and its siege tower stand.
					{
						// (the events are infos: CIV4EntityEventInfos.xml)
						EntityEventTypes event = EntityEventTypes(GC.getInfoTypeForString("ENTITY_EVENT_IDLE", true));
						if (u.battle >= 0)
							event = EntityEventTypes(GC.getInfoTypeForString("ENTITY_EVENT_BEGIN_COMBAT", true));
						else if (const BuildTypes build = shown->getBuildType(); build != NO_BUILD && GC.getBuildInfo(build).getEntityEvent() >= 0)
							event = EntityEventTypes(GC.getBuildInfo(build).getEntityEvent());
						else if (const int told = Entities::restingEvent(*shown); told >= 0)
							event = EntityEventTypes(told);
						const char* type = shown->getFormationType();
						const auto spot = [](const CvUnitEntry& e) { return render::UnitView::Spot{ e.m_position.x, e.m_position.y, e.m_fFacingDirection }; };
						for (int k = 0; k < GC.getNumUnitFormationInfos() && u.spots.empty(); ++k)
						{
							const CvUnitFormationInfo& f = GC.getUnitFormationInfo(k);
							if (f.getNumUnitEntries() != int(u.models.size()) || !type || std::strcmp(f.getFormationType(), type) != 0 ||
								std::ranges::find(f.getEventTypes(), event) == f.getEventTypes().end())
								continue;
							for (int e = 0; e < f.getNumUnitEntries(); ++e)
								u.spots.push_back(spot(f.getUnitEntry(e)));
							if (f.getNumGreatUnitEntries() > 0)
								u.leaderSpot = spot(f.getGreatUnitEntry(0));
							if (f.getNumSiegeUnitEntries() > 0)
								u.towerSpot = spot(f.getSiegeUnitEntry(0));
						}
					}
					// (the great general's figure: its era's warlord, as CvUnitInfo::getArtInfo picks the
					// early, middle or late art; the tags are not exported, BtS names them)
					if (shown->getLeaderUnitType() != NO_UNIT)
					{
						const int eras = GC.getNumEraInfos();
						const char* tag = era > eras / 2 ? "ART_DEF_UNIT_WARLORD_MODERN" : era > eras / 4 ? "ART_DEF_UNIT_WARLORD_MEDIEVAL" : "ART_DEF_UNIT_WARLORD_ANCIENT";
						if (const CvArtInfoUnit* info = ARTFILEMGR.getUnitArtInfo(tag))
							u.leader = modelIndex(info, info->getNIF(), info->getKFM(), info->getScale(), info->getShadowNIF(), info->getShadowAttachNode(),
								info->getShadowScale());
					}
					if (u.battle >= 0 && u.attacking && Entities::siegeTower(*shown))
						if (const CvArtInfoUnit* info = ARTFILEMGR.getUnitArtInfo("ART_DEF_UNIT_SIEGE_TOWER"))
							u.siegeTower = modelIndex(info, info->getNIF(), info->getKFM(), info->getScale(), info->getShadowNIF(), info->getShadowAttachNode(),
								info->getShadowScale());
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
					// (what it builds only for the active team's cities, as the original's bar)
					const CvGame& g = GC.getGame();
					c.ownTeam = city->getTeam() == g.getActiveTeam() || g.isDebugMode();
					c.production.clear();
					c.productionArt.clear();
					if (c.ownTeam)
					{
						GAMETEXT.buildCityBillboardProductionString(label, city);
						c.production = gameMarkup(label.getCString());
						// The production symbol, as the engine asks for it: the button of what it builds,
						// else the game's "no production" button.
						if (const TCHAR* art = city->getCityBillboardProductionIcon())
							c.productionArt = art;
					}
					label.clear();
					GAMETEXT.buildCityBillboardIconString(label, city);
					c.icons = gameMarkup(label.getCString());
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
