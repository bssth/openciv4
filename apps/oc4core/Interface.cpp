#include "Interface.h"

#include "CvGameCoreDLL.h"
#include "CvCity.h"
#include "CvDLLButtonPopup.h"
#include "CvDLLWidgetData.h"
#include "CvMap.h"
#include "CvPlayerAI.h"
#include "CvPlot.h"
#include "CvPopupInfo.h"
#include "CvPopupReturn.h"
#include "CvSelectionGroup.h"
#include "CvTalkingHeadMessage.h"
#include "CvUnit.h"

#include "render/MapSnapshot.h"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include <pybind11/pybind11.h>
namespace py = pybind11;

namespace oc4::host
{
	namespace
	{
		CvUnit* unitOf(const IDInfo& info)
		{
			if (info.eOwner < 0 || info.eOwner >= GC.getMAX_PLAYERS())
				return nullptr;
			return CvPlayerAI::getPlayerNonInl(info.eOwner).getUnit(info.iID);
		}

		// A 4 by 4 block of BGRA8 pixels as DXT1: the far corners of the colours' box as its two
		// colours (four-colour mode), each pixel the nearest of the four.
		void encodeBc1(const uint32_t px[16], unsigned char out[8])
		{
			int lo[3] = { 255, 255, 255 }, hi[3] = { 0, 0, 0 };
			for (int i = 0; i < 16; ++i)
				for (int c = 0; c < 3; ++c)
				{
					const int v = int((px[i] >> (8 * c)) & 255);
					lo[c] = std::min(lo[c], v), hi[c] = std::max(hi[c], v);
				}
			const auto to565 = [](const int bgr[3]) { return uint16_t(((bgr[2] >> 3) << 11) | ((bgr[1] >> 2) << 5) | (bgr[0] >> 3)); };
			uint16_t c0 = to565(hi), c1 = to565(lo);
			if (c0 < c1)
				std::swap(c0, c1);
			const auto expand = [](uint16_t c, int out[3]) {
				out[0] = (c & 31) * 255 / 31, out[1] = ((c >> 5) & 63) * 255 / 63, out[2] = (c >> 11) * 255 / 31;
			};
			int pal[4][3];
			expand(c0, pal[0]);
			expand(c1, pal[1]);
			for (int c = 0; c < 3; ++c)
				pal[2][c] = (2 * pal[0][c] + pal[1][c]) / 3, pal[3][c] = (pal[0][c] + 2 * pal[1][c]) / 3;
			uint32_t bits = 0;
			if (c0 != c1) // (equal, it is three-colour mode: every pixel the first)
				for (int i = 0; i < 16; ++i)
				{
					int best = 0, bestDistance = INT_MAX;
					for (int k = 0; k < 4; ++k)
					{
						int d = 0;
						for (int c = 0; c < 3; ++c)
						{
							const int e = int((px[i] >> (8 * c)) & 255) - pal[k][c];
							d += e * e;
						}
						if (d < bestDistance)
							best = k, bestDistance = d;
					}
					bits |= uint32_t(best) << (2 * i);
				}
			out[0] = uint8_t(c0), out[1] = uint8_t(c0 >> 8), out[2] = uint8_t(c1), out[3] = uint8_t(c1 >> 8);
			for (int i = 0; i < 4; ++i)
				out[4 + i] = uint8_t(bits >> (8 * i));
		}
	}

	unsigned char* Interface::getMinimapBaseTexture() const
	{
		const int size = std::max(GC.getDefineINT("MINIMAP_RENDER_SIZE"), 4) & ~3;
		m_minimapBase.assign(size_t(size) * size_t(size) / 2, 0);
		CvMap& map = GC.getMap();
		const int w = map.getGridWidth(), h = map.getGridHeight();
		if (w <= 0 || h <= 0)
			return m_minimapBase.data();
		// Each plot's colour, as the window's minimap has it without owners or fog.
		std::vector<uint32_t> plots(size_t(w) * size_t(h), 0xFF000000);
		for (int i = 0; i < map.numPlots(); ++i)
		{
			const CvPlot* plot = map.plotByIndex(i);
			if (!plot || plot->getX() < 0 || plot->getY() < 0 || plot->getX() >= w || plot->getY() >= h)
				continue;
			const int t = plot->getTerrainType();
			uint32_t c = t >= 0 && size_t(t) < minimapTerrainColors.size() ? minimapTerrainColors[size_t(t)] : plot->isWater() ? 0xFF8C5A28 : 0xFF808080;
			if (plot->isPeak())
				c = 0xFFBFBFBF;
			else if (plot->isHills())
				c = 0xFF000000 | (((c >> 16 & 255) * 4 / 5) << 16) | (((c >> 8 & 255) * 4 / 5) << 8) | ((c & 255) * 4 / 5);
			plots[size_t(plot->getX()) + size_t(plot->getY()) * size_t(w)] = c;
		}
		// The texture's rows from the north, in blocks of four by four.
		const int blocks = size / 4;
		for (int by = 0; by < blocks; ++by)
			for (int bx = 0; bx < blocks; ++bx)
			{
				uint32_t px[16];
				for (int j = 0; j < 4; ++j)
					for (int i = 0; i < 4; ++i)
					{
						const int x = (bx * 4 + i) * w / size, y = h - 1 - (by * 4 + j) * h / size;
						px[j * 4 + i] = plots[size_t(x) + size_t(y) * size_t(w)];
					}
				encodeBc1(px, &m_minimapBase[(size_t(by) * size_t(blocks) + size_t(bx)) * 8]);
			}
		return m_minimapBase.data();
	}

	void Interface::setDiploQueue(CvDiploParameters* pDiploParams, PlayerTypes ePlayer)
	{
		// (the active player's own contact: at once; another's waits in the player's queue)
		if (pDiploParams && ePlayer != NO_PLAYER && diplomacyAsked && ePlayer == GC.getGame().getActivePlayer())
			diplomacyAsked(pDiploParams, ePlayer);
		else if (pDiploParams && ePlayer != NO_PLAYER)
			CvPlayerAI::getPlayerNonInl(ePlayer).addDiplomacy(pDiploParams);
	}

	bool Interface::selected(const CvUnit* pUnit) const
	{
		for (CLLNode<IDInfo>* n = m_selection.head(); n; n = m_selection.next(n))
			if (n->m_data.eOwner == pUnit->getOwner() && n->m_data.iID == pUnit->getID())
				return true;
		return false;
	}

	CvUnit* Interface::getHeadSelectedUnit()
	{
		// Units that died since are dropped on the way.
		while (CLLNode<IDInfo>* n = m_selection.head())
		{
			if (CvUnit* u = unitOf(n->m_data))
				return u;
			m_selection.deleteNode(n);
		}
		return nullptr;
	}

	CvUnit* Interface::getSelectionUnit(int iIndex)
	{
		int i = 0;
		for (CLLNode<IDInfo>* n = m_selection.head(); n; n = m_selection.next(n), ++i)
			if (i == iIndex)
				return unitOf(n->m_data);
		return nullptr;
	}

	CvPlot* Interface::getSelectionPlot()
	{
		CvUnit* head = getHeadSelectedUnit();
		return head ? head->plot() : nullptr;
	}

	bool Interface::soundPlace(const NiPoint3& point, std::optional<Sounds::Place>& place)
	{
		place.reset();
		if (point.x == -1.0f && point.y == -1.0f)
			return true; // (no place)
		CvMap& map = GC.getMap();
		const float width = float(map.getGridWidth()) * float(GC.getPLOT_SIZE()), height = float(map.getGridHeight()) * float(GC.getPLOT_SIZE());
		if (map.getGridWidth() <= 0 || map.getGridHeight() <= 0 || width <= 0 || height <= 0)
			return true;
		// (CvMap::plotXToPointX: the plot's middle, the landscape centred on 0)
		const Sounds::Place p{ (point.x + width / 2) * float(map.getGridWidth()) / width, (point.y + height / 2) * float(map.getGridHeight()) / height };
		if (const CvPlot* plot = map.plot(int(std::floor(p.x)), int(std::floor(p.y))))
			if (const TeamTypes team = GC.getGame().getActiveTeam(); team != NO_TEAM && !plot->isVisible(team, false))
				return false;
		place = p;
		return true;
	}

	void Interface::playGeneralSound(LPCTSTR pszSound, NiPoint3 vPos)
	{
		std::optional<Sounds::Place> place;
		if (sounds && pszSound && *pszSound && soundPlace(vPos, place))
			sounds->play(pszSound, place);
	}

	void Interface::playGeneralSound(int iSoundId, int iSoundType, NiPoint3 vPos)
	{
		std::optional<Sounds::Place> place;
		if (sounds && soundPlace(vPos, place))
			sounds->play(iSoundType == AUDIOTAG_3DSCRIPT ? AudioScripts::Script3D : AudioScripts::Script2D, iSoundId, place);
	}

	// A message to a player, as the original engine takes it: into the player's messages (the
	// turn log, saved with the game; not one only to be shown), and for the active player on
	// the screen for its length in seconds (EVENT_MESSAGE_TIME if none), its sound played.
	void Interface::addMessage(PlayerTypes ePlayer, bool /*bForce*/, int iLength, CvWString szString, LPCTSTR pszSound,
		InterfaceMessageTypes eType, LPCSTR pszIcon, ColorTypes eFlashColor, int iFlashX, int iFlashY, bool bShowOffScreenArrows,
		bool bShowOnScreenArrows)
	{
		if (ePlayer < 0 || ePlayer >= GC.getMAX_PLAYERS())
			return;
		if (eType != MESSAGE_TYPE_DISPLAY_ONLY)
			CvPlayerAI::getPlayerNonInl(ePlayer).addMessage(CvTalkingHeadMessage(GC.getGame().getGameTurn(), iLength, szString.c_str(), pszSound, eType,
				pszIcon, eFlashColor, iFlashX, iFlashY, bShowOffScreenArrows, bShowOnScreenArrows));
		if (ePlayer != GC.getGame().getActivePlayer())
			return;
		show(szString.c_str(), iLength);
		if (sounds && pszSound && *pszSound)
			sounds->play(pszSound);
	}

	void Interface::addCombatMessage(PlayerTypes ePlayer, CvWString szString)
	{
		if (ePlayer >= 0 && ePlayer < GC.getMAX_PLAYERS())
			CvPlayerAI::getPlayerNonInl(ePlayer).addMessage(CvTalkingHeadMessage(GC.getGame().getGameTurn(), GC.getEVENT_MESSAGE_TIME(), szString.c_str(),
				nullptr, MESSAGE_TYPE_COMBAT_MESSAGE));
	}

	void Interface::addQuestMessage(PlayerTypes ePlayer, CvWString szString, int /*iQuestId*/)
	{
		if (ePlayer >= 0 && ePlayer < GC.getMAX_PLAYERS())
			CvPlayerAI::getPlayerNonInl(ePlayer).addMessage(CvTalkingHeadMessage(GC.getGame().getGameTurn(), GC.getEVENT_MESSAGE_TIME(), szString.c_str(),
				nullptr, MESSAGE_TYPE_QUEST));
	}

	// (the messages of the last turn again, when a game is loaded: CvPlayer::showMissedMessages)
	void Interface::showMessage(CvTalkingHeadMessage& msg) { show(msg.getDescription(), msg.getLength()); }

	void Interface::clearEventMessages()
	{
		std::lock_guard lock(m_shownMutex);
		m_shown.clear();
	}

	void Interface::show(const std::wstring& text, int seconds)
	{
		if (text.empty())
			return;
		if (seconds <= 0)
			seconds = GC.getEVENT_MESSAGE_TIME();
		std::lock_guard lock(m_shownMutex);
		const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(seconds, 1));
		// (the same one again while it is up, as a mod's weather has it plot after plot: one line,
		// counted, moved to the newest)
		int count = 1;
		if (const auto same = std::ranges::find(m_shown, text, &Shown::text); same != m_shown.end())
		{
			count = same->count + 1;
			m_shown.erase(same);
		}
		m_shown.push_back({ text, until, count });
		while (m_shown.size() > 12) // (the oldest go first when many come at once)
			m_shown.pop_front();
	}

	std::vector<Interface::ShownMessage> Interface::shownMessages() const
	{
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard lock(m_shownMutex);
		std::vector<ShownMessage> out;
		for (const Shown& s : m_shown)
			if (const float left = std::chrono::duration<float>(s.until - now).count(); left > 0)
				out.push_back({ s.count > 1 ? s.text + L" (x" + std::to_wstring(s.count) + L")" : s.text, std::min(1.0f, left) }); // (the last second fades it)
		return out;
	}

	// A unit of the active player picked, or sent somewhere: its sound for it (the art's, else
	// its people's voice, as CvSelectionGroup::playActionSound picks an order's), where it is.
	void Interface::unitSound(const CvUnit* pUnit, bool order)
	{
		if (!sounds || !pUnit || pUnit->getOwner() != GC.getGame().getActivePlayer())
			return;
		const CvPlayer& owner = CvPlayerAI::getPlayerNonInl(pUnit->getOwner());
		const CvArtInfoUnit* art = pUnit->getArtInfo(0, owner.getCurrentEra());
		int script = art ? (order ? art->getActionSoundScriptId() : art->getSelectionSoundScriptId()) : -1;
		if (script < 0 && owner.getCivilizationType() != NO_CIVILIZATION)
		{
			const CvCivilizationInfo& civ = GC.getCivilizationInfo(owner.getCivilizationType());
			script = order ? civ.getActionSoundScriptId() : civ.getSelectionSoundScriptId();
		}
		sounds->play(AudioScripts::Script3D, script, Sounds::Place{ float(pUnit->getX()) + 0.5f, float(pUnit->getY()) + 0.5f });
	}

	void Interface::insertIntoSelectionList(CvUnit* pUnit, bool bClear, bool bToggle, bool bGroup, bool bSound, bool /*bMinimalChange*/)
	{
		if (bSound && (bClear || m_selection.getLength() == 0)) // (once for a group, inserted unit by unit)
			unitSound(pUnit, false);
		if (bClear)
			m_selection.clear();
		if (!pUnit)
			return;
		const auto insert = [&](CvUnit* u) {
			if (!selected(u))
				m_selection.insertAtEnd(u->getIDInfo());
			else if (bToggle)
				removeFromSelectionList(u);
		};
		if (bGroup && pUnit->getGroup())
		{
			CvSelectionGroup* group = pUnit->getGroup();
			for (CLLNode<IDInfo>* n = group->headUnitNode(); n; n = group->nextUnitNode(n))
				if (CvUnit* u = unitOf(n->m_data))
					insert(u);
		}
		else
			insert(pUnit);
		m_lastSelected = pUnit;
		selectionChanged();
	}

	// The game decides: a unit of another group brings its whole group, alt and ctrl the units on the plot that can still move.
	void Interface::selectUnit(CvUnit* pUnit, bool bClear, bool bToggle, bool bSound)
	{
		if (pUnit)
			GC.getGame().selectUnit(pUnit, bClear, bToggle, bSound);
	}

	void Interface::selectGroup(CvUnit* pUnit, bool bShift, bool bCtrl, bool bAlt)
	{
		if (pUnit)
			GC.getGame().selectGroup(pUnit, bShift, bCtrl, bAlt);
	}

	void Interface::selectAll(CvPlot* pPlot)
	{
		m_selection.clear();
		for (int i = 0; pPlot && i < pPlot->getNumUnits(); ++i)
			if (CvUnit* u = pPlot->getUnitByIndex(i); u && u->getOwner() == GC.getGame().getActivePlayer())
				insertIntoSelectionList(u, false, false, false, false, false);
	}

	bool Interface::removeFromSelectionList(CvUnit* pUnit)
	{
		for (CLLNode<IDInfo>* n = m_selection.head(); n; n = m_selection.next(n))
			if (n->m_data.eOwner == pUnit->getOwner() && n->m_data.iID == pUnit->getID())
			{
				m_selection.deleteNode(n);
				selectionChanged();
				return true;
			}
		return false;
	}

	// Whether the selected units are exactly the head unit's group (no need to join them).
	bool Interface::mirrorsSelectionGroup()
	{
		CvUnit* head = getHeadSelectedUnit();
		CvSelectionGroup* group = head ? head->getGroup() : nullptr;
		if (!group || group->getNumUnits() != m_selection.getLength())
			return false;
		for (CLLNode<IDInfo>* n = group->headUnitNode(); n; n = group->nextUnitNode(n))
			if (CvUnit* u = unitOf(n->m_data); !u || !selected(u))
				return false;
		return true;
	}

	CvSelectionGroup* Interface::getSelectionList()
	{
		CvUnit* head = getHeadSelectedUnit();
		return head ? head->getGroup() : nullptr;
	}

	bool Interface::canSelectionListFound()
	{
		CvUnit* head = getHeadSelectedUnit();
		return head && head->getGroup() && head->getGroup()->canStartMission(MISSION_FOUND, -1, -1, head->plot(), true);
	}

	void Entities::createUnitEntity(CvUnit* pUnit)
	{
		pUnit->setEntity(reinterpret_cast<CvEntity*>(pUnit));
	}

	bool Entities::IsSelected(const CvEntity* pEntity) const
	{
		return pEntity && m_selection.isSelected(reinterpret_cast<const CvUnit*>(pEntity));
	}

	std::unordered_map<const void*, MissionTypes> Entities::s_missions;

	void Entities::destroyEntity(CvEntity*& pEntity, bool)
	{
		s_missions.erase(pEntity);
		pEntity = nullptr;
	}

	namespace
	{
		std::mutex g_foundingMutex;
		std::vector<Entities::Founding> g_foundings;
	}

	namespace
	{
		// The units whose damage changed lately (MISSION_DAMAGE), when: a battle's collateral damage.
		std::mutex g_damageMutex;
		std::unordered_map<const CvUnit*, std::chrono::steady_clock::time_point> g_damaged;
	}

	void Interface::setWorldBuilder(bool bTurnOn)
	{
		if (bTurnOn == m_worldBuilder)
			return;
		m_worldBuilder = bTurnOn;
		if (bTurnOn)
		{
			m_showBeforeWorldBuilder = m_show;
			m_show = INTERFACE_HIDE_ALL;
		}
		else
			m_show = m_showBeforeWorldBuilder;
		m_dirty.set();
		try
		{
			// (the tool palettes start afresh each time, as the executable makes them anew)
			auto wb = py::module_::import("oc4wb");
			if (bTurnOn)
				wb.attr("reset")();
			py::module_::import("CvScreensInterface").attr(bTurnOn ? "showWorldBuilderScreen" : "hideWorldBuilderScreen")();
			if (!bTurnOn)
				wb.attr("reset")();
		}
		catch (py::error_already_set& e)
		{
			std::fprintf(stderr, "[oc4core] world builder: %s\n", e.what());
		}
	}

	CvPlot* Interface::getMouseOverPlot() { return mouseX >= 0 ? GC.getMap().plot(mouseX, mouseY) : nullptr; }

	void Entities::NotifyEntity(CvUnitEntity* pEntity, MissionTypes eMission)
	{
		if (!pEntity)
			return;
		s_missions[pEntity] = eMission;
		if (eMission == MISSION_DAMAGE)
		{
			std::lock_guard lock(g_damageMutex);
			g_damaged[reinterpret_cast<const CvUnit*>(pEntity)] = std::chrono::steady_clock::now();
		}
		if (eMission == MISSION_FOUND)
		{
			// (its figures as the capture lays a formation out: each art as often as required)
			const CvUnit& unit = *reinterpret_cast<const CvUnit*>(pEntity);
			Founding f;
			f.x = unit.getX(), f.y = unit.getY(), f.owner = unit.getOwner(), f.direction = int(unit.getFacingDirection(false));
			const EraTypes era = CvPlayerAI::getPlayerNonInl(unit.getOwner()).getCurrentEra();
			const int figures = std::clamp(unit.getGroupSize(), 1, 12), definitions = unit.getGroupDefinitions();
			std::vector<int> art;
			for (int d = 0; d < definitions; ++d)
				for (int r = 0; r < unit.getUnitGroupRequired(d) && int(art.size()) < figures; ++r)
					art.push_back(d);
			for (int k = int(art.size()); definitions > 0 && k < figures; ++k)
				art.push_back(k % definitions);
			for (const int d : art)
				if (const CvArtInfoUnit* info = unit.getArtInfo(d, era))
					f.figures.push_back(info);
			f.at = std::chrono::steady_clock::now();
			std::lock_guard lock(g_foundingMutex);
			g_foundings.push_back(std::move(f));
		}
	}

	std::vector<Entities::Founding> Entities::foundings()
	{
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard lock(g_foundingMutex);
		std::erase_if(g_foundings, [&](const Founding& f) { return now - f.at > std::chrono::seconds(6); });
		return g_foundings;
	}

	namespace
	{
		std::mutex g_battleMutex;
		std::vector<Entities::Battle> g_battles;
		uint64_t g_battleSerial = 0;
		std::chrono::steady_clock::time_point g_lastSlice{};

		std::chrono::steady_clock::time_point battleEnd(const Entities::Battle& b)
		{
			return b.start + std::chrono::milliseconds(int(1000 * render::kBattleSliceSeconds * float(b.slices)) + 500);
		}
	}

	namespace
	{
		std::mutex g_actionMutex;
		std::vector<Entities::Action> g_actions;
	}

	std::vector<Entities::Action> Entities::actions()
	{
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard lock(g_actionMutex);
		std::erase_if(g_actions, [&](const Action& a) { return std::chrono::duration<float>(now - a.start).count() > a.seconds + 1; });
		return g_actions;
	}

	void Entities::AddMission(const CvMissionDefinition* pDefinition)
	{
		if (!pDefinition)
			return;
		if (const MissionTypes m = pDefinition->getMissionType(); m != MISSION_BEGIN_COMBAT)
		{
			// (the missions the units act out)
			if (m != MISSION_PILLAGE && m != MISSION_BOMBARD && m != MISSION_RANGE_ATTACK && m != MISSION_AIRSTRIKE && m != MISSION_AIRBOMB &&
				m != MISSION_PARADROP && m != MISSION_RECON && m != MISSION_NUKE && m != MISSION_CAPTURED)
				return;
			const CvUnit* unit = pDefinition->getUnit(BATTLE_UNIT_ATTACKER);
			if (!unit)
				return;
			Action a;
			a.unit = unit->getIDInfo();
			a.mission = m == MISSION_PILLAGE ? 1 : m == MISSION_CAPTURED ? 3 : 2; // (the way it acts: render::UnitView::action)
			const CvPlot* target = pDefinition->getPlot();
			a.targetX = target ? target->getX() : unit->getX();
			a.targetY = target ? target->getY() : unit->getY();
			if (const int event = GC.getMissionInfo(m).getEntityEvent(); event >= 0 && event < GC.getNumEntityEventInfos())
				if (const EffectTypes e = GC.getEntityEventInfo(EntityEventTypes(event)).getEffectType(0); e >= 0 && e < GC.getNumEffectInfos())
					a.effect = GC.getEffectInfo(e).getType();
			a.seconds = std::clamp(pDefinition->getMissionTime() * render::kBattleSliceSeconds, 0.6f, 6.0f);
			if (unit->getDomainType() == DOMAIN_AIR)
				a.seconds = std::max(a.seconds, 3.0f); // (flown there and back: render::MapView's flight)
			a.start = std::chrono::steady_clock::now();
			if (std::getenv("OC4_PLAY_LOG"))
				std::printf("  action %d of unit %d: %.2fs at %d,%d%s%s\n", a.mission, a.unit.iID, a.seconds, a.targetX, a.targetY, a.effect.empty() ? "" : ", ", a.effect.c_str());
			std::lock_guard lock(g_actionMutex);
			g_actions.push_back(std::move(a));
			return;
		}
		const auto& def = *static_cast<const CvBattleDefinition*>(pDefinition);
		const CvUnit* attacker = def.getUnit(BATTLE_UNIT_ATTACKER);
		const CvUnit* defender = def.getUnit(BATTLE_UNIT_DEFENDER);
		if (!attacker || !defender)
			return;
		Battle b;
		b.attacker = attacker->getIDInfo(), b.defender = defender->getIDInfo();
		b.attackerX = attacker->getX(), b.attackerY = attacker->getY(), b.defenderX = defender->getX(), b.defenderY = defender->getY();
		// (CvUnit::collateralCombat damaged others there just now: each told its entity so)
		if (const CvPlot* at = defender->plot())
		{
			const auto now = std::chrono::steady_clock::now();
			std::lock_guard lock(g_damageMutex);
			for (int i = 0; i < at->getNumUnits(); ++i)
				if (const CvUnit* u = at->getUnitByIndex(i); u && u != defender)
					if (const auto it = g_damaged.find(u); it != g_damaged.end() && now - it->second < std::chrono::milliseconds(50))
						++b.collateral;
			g_damaged.clear();
		}
		for (int i = 0; i < def.getNumBattleRounds(); ++i)
		{
			const CvBattleRound& r = def.getBattleRound(i);
			Battle::Round& round = b.rounds.emplace_back();
			round.ranged = r.isRangedRound();
			for (int side = 0; side < BATTLE_UNIT_COUNT; ++side)
			{
				round.killed[side] = r.getNumKilled(BattleUnitTypes(side));
				round.alive[side] = r.getNumAlive(BattleUnitTypes(side));
			}
		}
		// (its length in the DLL's updates: the combat timer it set, as the mission time says at
		// Utility::getSecsPerTurn's 1 second a turn)
		b.slices = std::max(1, int(std::lround(def.getMissionTime() / 1.0f)));
		b.start = std::chrono::steady_clock::now();
		std::lock_guard lock(g_battleMutex);
		b.serial = ++g_battleSerial;
		g_battles.push_back(std::move(b));
		if (std::getenv("OC4_PLAY_LOG"))
			std::printf("  battle %llu: %zu rounds, %d slices\n", static_cast<unsigned long long>(g_battleSerial), g_battles.back().rounds.size(),
				g_battles.back().slices);
	}

	std::vector<Entities::Battle> Entities::battles()
	{
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard lock(g_battleMutex);
		std::erase_if(g_battles, [&](const Battle& b) { return now > battleEnd(b); });
		return g_battles;
	}

	bool Entities::battleWait()
	{
		const auto now = std::chrono::steady_clock::now();
		std::lock_guard lock(g_battleMutex);
		std::erase_if(g_battles, [&](const Battle& b) { return now > battleEnd(b); });
		if (g_battles.empty())
			return false;
		if (now - g_lastSlice < std::chrono::milliseconds(int(1000 * render::kBattleSliceSeconds)))
			return true;
		g_lastSlice = now;
		return false;
	}

	bool Entities::resting(const CvUnit& unit)
	{
		if (!unit.isWaiting())
			return false;
		const auto it = s_missions.find(&unit);
		if (it == s_missions.end())
			return true;
		return it->second == MISSION_FORTIFY || it->second == MISSION_SLEEP || it->second == MISSION_SENTRY || it->second == MISSION_HEAL;
	}

	// ---------------------------------------------------------------- cities

	namespace
	{
		CvCity* cityOf(const IDInfo& info)
		{
			if (info.eOwner < 0 || info.eOwner >= GC.getMAX_PLAYERS())
				return nullptr;
			return CvPlayerAI::getPlayerNonInl(info.eOwner).getCity(info.iID);
		}
	}

	void Interface::selectionChanged()
	{
		for (const InterfaceDirtyBits bit : { SelectionButtons_DIRTY_BIT, InfoPane_DIRTY_BIT, PlotListButtons_DIRTY_BIT, CitizenButtons_DIRTY_BIT,
				 CityScreen_DIRTY_BIT, SelectionCamera_DIRTY_BIT, Help_DIRTY_BIT, MiscButtons_DIRTY_BIT })
			m_dirty[bit] = true;
	}

	void Interface::cityScreenChanged(bool wasUp)
	{
		if (wasUp && !isCityScreenUp())
		{
			for (const IDInfo& id : m_citiesShown)
				if (CvCity* c = cityOf(id); c && c->getOwner() == id.eOwner)
					c->updateSelectedCity(true);
			m_citiesShown.clear();
		}
		if (isCityScreenUp() != wasUp)
			for (const InterfaceDirtyBits bit : { PercentButtons_DIRTY_BIT, ResearchButtons_DIRTY_BIT, GameData_DIRTY_BIT, Score_DIRTY_BIT })
				m_dirty[bit] = true;
	}

	void Interface::selectCity(CvCity* pNewValue, bool /*bTestProduction*/)
	{
		const bool wasUp = isCityScreenUp();
		if (wasUp)
			m_citiesShown.push_back(getHeadSelectedCity()->getIDInfo());
		m_cities.clear();
		if (pNewValue)
		{
			m_cities.insertAtEnd(pNewValue->getIDInfo());
			m_cityScreen = true;
			lookAtX = pNewValue->getX();
			lookAtY = pNewValue->getY();
		}
		selectionChanged();
		cityScreenChanged(wasUp);
	}

	void Interface::addSelectedCity(CvCity* pNewValue, bool bToggle)
	{
		if (!pNewValue)
			return;
		for (CLLNode<IDInfo>* n = m_cities.head(); n; n = m_cities.next(n))
			if (n->m_data.eOwner == pNewValue->getOwner() && n->m_data.iID == pNewValue->getID())
			{
				if (bToggle)
					m_cities.deleteNode(n);
				selectionChanged();
				return;
			}
		m_cities.insertAtEnd(pNewValue->getIDInfo());
		selectionChanged();
	}

	void Interface::clearSelectedCities()
	{
		const bool wasUp = isCityScreenUp();
		if (wasUp)
			m_citiesShown.push_back(getHeadSelectedCity()->getIDInfo());
		m_cities.clear();
		m_cityScreen = false;
		selectionChanged();
		cityScreenChanged(wasUp);
	}

	bool Interface::isCitySelected(CvCity* pCity)
	{
		for (CLLNode<IDInfo>* n = m_cities.head(); pCity && n; n = m_cities.next(n))
			if (n->m_data.eOwner == pCity->getOwner() && n->m_data.iID == pCity->getID())
				return true;
		return false;
	}

	CvCity* Interface::getHeadSelectedCity()
	{
		// Cities lost since are dropped on the way.
		while (CLLNode<IDInfo>* n = m_cities.head())
		{
			if (CvCity* c = cityOf(n->m_data); c && c->getOwner() == n->m_data.eOwner)
				return c;
			m_cities.deleteNode(n);
		}
		m_cityScreen = false;
		return nullptr;
	}

	void Interface::lookAtCityOffset(int iCity)
	{
		const PlayerTypes active = GC.getGame().getActivePlayer();
		if (active == NO_PLAYER || m_quietFocus)
			return;
		if (const CvCity* city = CvPlayerAI::getPlayerNonInl(active).getCity(iCity))
		{
			lookAtX = city->getX();
			lookAtY = city->getY();
		}
	}

	bool Interface::canHandleAction(int iAction, CvPlot* pPlot, bool bTestVisible)
	{
		return iAction >= 0 && iAction < GC.getNumActionInfos() && GC.getGame().canHandleAction(iAction, pPlot, bTestVisible);
	}

	bool Interface::canDoInterfaceMode(InterfaceModeTypes eInterfaceMode, CvSelectionGroup* pSelectionGroup)
	{
		return eInterfaceMode != NO_INTERFACEMODE && pSelectionGroup && pSelectionGroup->canDoInterfaceMode(eInterfaceMode);
	}

	// ---------------------------------------------------------------- popups

	void Interface::addPopup(CvPopupInfo* pInfo, PlayerTypes ePlayer, bool bImmediate, bool bFront)
	{
		if (!pInfo)
			return;
		const PlayerTypes player = ePlayer == NO_PLAYER ? GC.getGame().getActivePlayer() : ePlayer;
		if (std::getenv("OC4_PLAY_LOG"))
			std::printf("  popup %d queued for player %d\n", int(pInfo->getButtonPopupType()), int(player));
		// The player's list is what a save keeps of the popups not yet shown.
		if (!bImmediate)
		{
			CvPlayerAI::getPlayerNonInl(player).addPopup(pInfo, bFront);
			return;
		}
		m_queued.push_front({ pInfo, player, nullptr, true });
	}

	bool Interface::isPopupQueued()
	{
		const PlayerTypes active = GC.getGame().getActivePlayer();
		return !m_queued.empty() || (active != NO_PLAYER && !CvPlayerAI::getPlayerNonInl(active).getPopups().empty());
	}

	void Interface::queuePopup(Popup popup, bool front)
	{
		Queued q{ nullptr, popup.player == NO_PLAYER ? GC.getGame().getActivePlayer() : popup.player, std::make_unique<Popup>(std::move(popup)) };
		q.ready->player = q.player;
		if (std::getenv("OC4_PLAY_LOG"))
			std::printf("  script popup %d queued for player %d\n", q.ready->python, int(q.player));
		if (front)
			m_queued.push_front(std::move(q));
		else
			m_queued.push_back(std::move(q));
	}

	void Interface::getDisplayedButtonPopups(CvPopupQueue& infos)
	{
		// (the game's menu, and its "are you sure", are not part of the game: a save made from it
		// does not bring it back)
		if (m_popup && m_popup->info && m_popup->info->getButtonPopupType() != BUTTONPOPUP_MAIN_MENU &&
			m_popup->info->getButtonPopupType() != BUTTONPOPUP_CONFIRM_MENU)
			infos.push_back(m_popup->info);
	}

	void Interface::clearQueuedPopups()
	{
		for (const Queued& q : m_queued)
			delete q.info;
		m_queued.clear();
	}

	void Interface::popupSetHeaderString(CvPopup* pPopup, CvWString szText, uint)
	{
		if (pPopup)
			popupOf(pPopup)->header = szText;
	}

	void Interface::popupSetBodyString(CvPopup* pPopup, CvWString szText, uint, char*, CvWString szHelpText)
	{
		if (pPopup)
			popupOf(pPopup)->items.push_back({ .kind = Popup::Item::Kind::Text, .text = szText, .help = szHelpText });
	}

	void Interface::popupLaunch(CvPopup* pPopup, bool bCreateOkButton, PopupStates, int)
	{
		if (!pPopup)
			return;
		popupOf(pPopup)->launched = true;
		popupOf(pPopup)->okButton = bCreateOkButton;
	}

	void Interface::popupAddDDS(CvPopup* pPopup, const char* szIconFilename, int, int, CvWString szHelpText)
	{
		if (pPopup && szIconFilename)
			popupOf(pPopup)->items.push_back({ .kind = Popup::Item::Kind::Image, .help = szHelpText, .icon = szIconFilename });
	}

	void Interface::popupAddSeparator(CvPopup* pPopup, int)
	{
		if (pPopup)
			popupOf(pPopup)->items.push_back({ .kind = Popup::Item::Kind::Separator });
	}

	void Interface::popupCreateEditBox(CvPopup* pPopup, CvWString szDefaultString, WidgetTypes, CvWString szHelpText, int iGroup, PopupControlLayout,
		unsigned int, unsigned int)
	{
		if (pPopup)
			popupOf(pPopup)->items.push_back({ .kind = Popup::Item::Kind::Edit, .text = szDefaultString, .help = szHelpText, .group = iGroup });
	}

	void Interface::popupAddGenericButton(CvPopup* pPopup, CvWString szText, const char* szIcon, int iButtonId, WidgetTypes eWidgetType, int iData1,
		int iData2, bool bOption, PopupControlLayout, unsigned int)
	{
		if (!pPopup)
			return;
		Popup::Item b{ .kind = Popup::Item::Kind::Button, .text = szText, .icon = szIcon ? szIcon : "", .id = iButtonId };
		b.widget.m_iData1 = iData1;
		b.widget.m_iData2 = iData2;
		b.widget.m_bOption = bOption;
		b.widget.m_eWidgetType = eWidgetType;
		popupOf(pPopup)->items.push_back(std::move(b));
	}

	void Interface::popupCreateRadioButtons(CvPopup* pPopup, int iNumButtons, int iGroup, WidgetTypes, PopupControlLayout)
	{
		for (int i = 0; pPopup && i < iNumButtons; ++i)
			popupOf(pPopup)->items.push_back({ .kind = Popup::Item::Kind::Radio, .id = i, .group = iGroup, .checked = i == 0 });
	}

	void Interface::popupSetRadioButtonText(CvPopup* pPopup, int iRadioButtonID, CvWString szText, int iGroup, CvWString szHelpText)
	{
		if (pPopup)
			for (Popup::Item& item : popupOf(pPopup)->items)
				if (item.kind == Popup::Item::Kind::Radio && item.group == iGroup && item.id == iRadioButtonID)
				{
					item.text = szText;
					item.help = szHelpText;
				}
	}

	void Interface::popupCreateCheckBoxes(CvPopup* pPopup, int iNumBoxes, int iGroup, WidgetTypes, PopupControlLayout)
	{
		for (int i = 0; pPopup && i < iNumBoxes; ++i)
			popupOf(pPopup)->items.push_back({ .kind = Popup::Item::Kind::Check, .id = i, .group = iGroup });
	}

	void Interface::popupSetCheckBoxText(CvPopup* pPopup, int iCheckBoxID, CvWString szText, int iGroup, CvWString szHelpText)
	{
		if (pPopup)
			for (Popup::Item& item : popupOf(pPopup)->items)
				if (item.kind == Popup::Item::Kind::Check && item.group == iGroup && item.id == iCheckBoxID)
				{
					item.text = szText;
					item.help = szHelpText;
				}
	}

	void Interface::popupSetCheckBoxState(CvPopup* pPopup, int iCheckBoxID, bool bChecked, int iGroup)
	{
		if (pPopup)
			for (Popup::Item& item : popupOf(pPopup)->items)
				if (item.kind == Popup::Item::Kind::Check && item.group == iGroup && item.id == iCheckBoxID)
					item.checked = bChecked;
	}

	void Interface::popupSetAsCancelled(CvPopup* pPopup)
	{
		if (pPopup)
			popupOf(pPopup)->cancelled = true;
	}

	bool Interface::popupIsDying(CvPopup* pPopup) { return pPopup && popupOf(pPopup)->cancelled; }

	void Interface::closePopup()
	{
		if (m_popup)
			delete m_popup->info;
		m_popup.reset();
	}

	namespace
	{
		// Popups of the multiplayer game and its administration: not here (the game menu, its
		// confirmations, the game's details and the scripts' screens are).
		bool launchable(ButtonPopupTypes type)
		{
			switch (type)
			{
			case BUTTONPOPUP_ADMIN:
			case BUTTONPOPUP_ADMIN_PASSWORD:
			case BUTTONPOPUP_DIPLOMACY:
			case BUTTONPOPUP_ADDBUDDY:
			case BUTTONPOPUP_FORCED_DISCONNECT:
			case BUTTONPOPUP_PITBOSS_DISCONNECT:
			case BUTTONPOPUP_KICKED:
				return false;
			default:
				return type >= 0; // (mods add their own; the DLL refuses what it does not know)
			}
		}
	}

	void Interface::updatePopups(PlayerTypes active)
	{
		if (active == NO_PLAYER)
			return;
		CvDLLButtonPopup& popups = CvDLLButtonPopup::getInstance();
		if (m_popup && !m_popup->info)
			return; // (a script's: nothing to check)
		if (m_popup)
		{
			// Still wanted (OnFocus cancels a choice made meanwhile), without moving the camera again.
			m_quietFocus = true;
			if (sounds)
				sounds->setQuiet(true);
			popups.OnFocus(reinterpret_cast<CvPopup*>(m_popup.get()), *m_popup->info);
			if (sounds)
				sounds->setQuiet(false);
			m_quietFocus = false;
			if (m_popup->cancelled)
				closePopup();
			return;
		}
		auto next = std::ranges::find_if(m_queued, [&](const Queued& q) { return q.player == active; });
		if (next == m_queued.end())
		{
			// (the city screen holds the next city's question until it is left, as the original's)
			if (isCityScreenUp())
				return;
			CvPopupInfo* info = CvPlayerAI::getPlayerNonInl(active).popFrontPopup();
			// (a save made from the game's menu before the fix above has it among the player's)
			while (info && (info->getButtonPopupType() == BUTTONPOPUP_MAIN_MENU || info->getButtonPopupType() == BUTTONPOPUP_CONFIRM_MENU))
			{
				delete info;
				info = CvPlayerAI::getPlayerNonInl(active).popFrontPopup();
			}
			if (!info)
				return;
			m_queued.push_front({ info, active });
			next = m_queued.begin();
		}
		// (one wonder's movie after the other, each once the last is closed)
		if (next->info && next->info->getButtonPopupType() == BUTTONPOPUP_PYTHON_SCREEN && screenUp && screenUp())
			return;
		if (next->ready)
		{
			// A script's popup, as it was filled.
			m_popup = std::move(next->ready);
			m_popup->serial = ++m_popupSerial;
			m_queued.erase(next);
			if (std::getenv("OC4_PLAY_LOG"))
				std::printf("  script popup %d: shown, %zu items\n", m_popup->python, m_popup->items.size());
			return;
		}
		m_popup = std::make_unique<Popup>();
		m_popup->info = next->info;
		m_popup->player = next->player;
		m_popup->serial = ++m_popupSerial;
		const bool added = next->added;
		m_queued.erase(next);
		CvPopup* const popup = reinterpret_cast<CvPopup*>(m_popup.get());
		const ButtonPopupTypes type = m_popup->info->getButtonPopupType();
		// (the espionage popups carry the turn slice they were made in and launch only in it, as
		// the original's do; the game has run on since)
		if (added && (type == BUTTONPOPUP_DOESPIONAGE || type == BUTTONPOPUP_DOESPIONAGE_TARGET))
			m_popup->info->setFlags(GC.getGame().getTurnSlice());
		const bool launched = launchable(type) && popups.launchButtonPopup(popup, *m_popup->info) && m_popup->launched;
		if (std::getenv("OC4_PLAY_LOG"))
			std::printf("  popup %d: %s, %zu items\n", int(type), launched ? "launched" : "not shown", m_popup->items.size());
		if (!launched)
		{
			closePopup(); // (the DLL found nothing to ask)
			return;
		}
		popups.OnFocus(popup, *m_popup->info);
		if (m_popup->cancelled)
			closePopup();
	}

	void Interface::answerPopup(int item, const std::vector<int>& radioItems, const std::vector<int>& checkedItems,
		const std::vector<std::pair<int, std::wstring>>& edits)
	{
		if (!m_popup)
			return;
		using Kind = Popup::Item::Kind;
		Popup& p = *m_popup;
		const auto at = [&](int i, Kind kind) { return i >= 0 && size_t(i) < p.items.size() && p.items[size_t(i)].kind == kind ? &p.items[size_t(i)] : nullptr; };
		PopupAnswer a; // (OK: button 0)
		if (const Popup::Item* clicked = at(item, Kind::Button))
		{
			a.button = clicked->id;
			// The button's own action first (train this, research that), as a widget does.
			if (clicked->widget.m_eWidgetType != WIDGET_GENERAL)
			{
				CvWidgetDataStruct widget = clicked->widget;
				CvDLLWidgetData::getInstance().executeAction(widget);
			}
		}
		// Every group answers: the radio button or entry chosen (the first unless another is), the
		// checkboxes as a bitfield, the edit box's text.
		for (const Popup::Item& it : p.items)
		{
			if (it.kind == Kind::Radio && (it.checked || !a.chosen.contains({ it.control, it.group })))
				a.chosen[{ it.control, it.group }] = it.id; // (the one marked chosen, else the first)
			else if (it.kind == Kind::Check && !a.checked.contains(it.group))
				a.checked[it.group] = 0;
			else if (it.kind == Kind::Edit && !a.edits.contains(it.group))
				a.edits[it.group] = it.text;
		}
		for (const int i : radioItems)
			if (const Popup::Item* it = at(i, Kind::Radio))
				a.chosen[{ it->control, it->group }] = it->id;
		for (const int i : checkedItems)
			if (const Popup::Item* it = at(i, Kind::Check))
				a.checked[it->group] |= 1 << it->id;
		for (const auto& [i, text] : edits)
			if (const Popup::Item* it = at(i, Kind::Edit))
				a.edits[it->group] = text;
		if (!p.info)
		{
			if (onPythonAnswer)
				onPythonAnswer(p, a);
			closePopup();
			return;
		}
		PopupReturn answer;
		answer.setButtonClicked(a.button);
		for (const auto& [key, id] : a.chosen)
			switch (key.first)
			{
			case Popup::Item::Control::RadioButtons: answer.setSelectedRadioButton(id, key.second); break;
			case Popup::Item::Control::PullDown: answer.setSelectedPulldownValue(id, key.second); break;
			case Popup::Item::Control::ListBox: answer.setSelectedListBoxValue(id, key.second); break;
			}
		for (const auto& [group, bits] : a.checked)
			answer.setCheckboxBitfield(bits, group);
		for (const auto& [group, text] : a.edits)
			answer.setEditBoxString(CvWString(text), group);
		CvDLLButtonPopup::getInstance().OnOkClicked(reinterpret_cast<CvPopup*>(&p), &answer, *p.info);
		closePopup();
	}

	void Interface::dismissPopup() { closePopup(); }

	CvUnit* Interface::getInterfacePlotUnit(const CvPlot* pPlot, int iIndex)
	{
		const TeamTypes team = GC.getGame().getActiveTeam();
		for (int i = 0; pPlot && i < pPlot->getNumUnits(); ++i)
			if (CvUnit* u = pPlot->getUnitByIndex(i); u && !u->isInvisible(team, false) && iIndex-- == 0)
				return u;
		return nullptr;
	}

	int Interface::getPlotListOffset()
	{
		const int perRow = std::max(1, plotListButtonsPerRow ? plotListButtonsPerRow() : 1), rows = std::max(1, GC.getMAX_PLOT_LIST_ROWS());
		if (isCityScreenUp())
			return (rows - 1) * perRow;
		int visible = 0;
		while (getInterfacePlotUnit(getSelectionPlot(), visible))
			++visible;
		const int used = std::min(rows, (visible + perRow - 1) / perRow);
		return (rows - used) * perRow;
	}

	void Interface::lookAt(NiPoint3 pt3Target, CameraLookAtTypes, NiPoint3)
	{
		// (a point of the map, as CvPlot::getPoint gives it: the plot under it)
		CvMap& map = GC.getMap();
		const int x = map.pointXToPlotX(pt3Target.x), y = map.pointYToPlotY(pt3Target.y);
		if (x >= 0 && y >= 0 && x < map.getGridWidth() && y < map.getGridHeight())
		{
			lookAtX = x;
			lookAtY = y;
		}
	}

	void Interface::lookAtSelectionPlot(bool /*bRelease*/)
	{
		if (CvPlot* p = getSelectionPlot())
		{
			lookAtX = p->getX();
			lookAtY = p->getY();
		}
	}
}
