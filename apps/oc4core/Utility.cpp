#include "Utility.h"

#include "DataStream.h"
#include "GameFont.h"
#include "Net.h"
#ifdef __linux__
#include "Pitboss.h"
#endif
#include "core/FileUtil.h"

#include "CvDeal.h"
#include "CvGameAI.h"
#include "CvGlobals.h"
#include "CvMap.h"
#include "CvPlot.h"
#include "CvInfos.h"
#include "CvInitCore.h"
#include "CvMessageData.h"
#include "CvPlayerAI.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <set>

namespace oc4::host
{
	bool Utility::isConnected(int player)
	{
#ifdef __linux__
		return m_pitboss && pb::connected(player);
#else
		return false;
#endif
	}
	int Utility::getAssignedNetworkID(int player) { return isConnected(player) ? player : -1; }
	Utility::Utility(const AssetResolver& assets, PythonHost& python, Xml& xml, std::filesystem::path logDir)
		: m_assets(assets), m_python(python), m_xml(xml), m_logDir(std::move(logDir))
	{
		if (!m_logDir.empty())
			std::filesystem::create_directories(m_logDir);
		m_text.setTagResolver(resolveTextTag);
		m_interface.diplomacyWaiting = [this] { return m_diplomacy.active() || m_diplomacy.waiting(); };
		m_interface.diplomacyAsked = [this](CvDiploParameters* params, PlayerTypes p) { m_diplomacy.ask(params, p); };
		m_interface.sounds = &m_sounds;
	}

	void Engine::toggleGlobeview()
	{
		globeUp = !globeUp;
		++globeVersion;
		// (the whole interface again: its buttons are the globe's, or the map's)
		if (CvDLLInterfaceIFaceBase* ui = gDLL->getInterfaceIFace())
			for (int bit = 0; bit < NUM_INTERFACE_DIRTY_BITS; ++bit)
				ui->setDirty(static_cast<InterfaceDirtyBits>(bit), true);
	}

	void Engine::addLandmark(CvPlot* plot, const wchar* caption)
	{
		if (plot)
			addSign({ plot->getX(), plot->getY(), -1, caption ? caption : L"", false });
	}

	void Utility::Do3DSound(int iScriptId, NiPoint3 vPosition)
	{
		std::optional<Sounds::Place> place;
		if (Interface::soundPlace(vPosition, place))
			m_sounds.play(AudioScripts::Script3D, iScriptId, place);
	}

	CvDLLUtilityIFaceBase& engineMemoryAfterShutdown()
	{
		static EngineMemory* const memory = new EngineMemory; // outlives the game core's DLL_PROCESS_DETACH
		return *memory;
	}

	void Utility::setModFolder(const std::string& folder)
	{
		m_modFolder = folder;
		m_modName = std::filesystem::path(folder).parent_path().filename().string();
	}

	void Engine::TriggerEffect(int iEffect, NiPoint3 pt3Point, float rotation)
	{
		if (iEffect < 0)
			return;
		// (the point as CvMap::plotXToPointX makes it: the map's centre at 0)
		float width = 0, height = 0;
		GetLandscapeGameDimensions(width, height);
		const float size = float(GC.getPLOT_SIZE());
		if (size <= 0)
			return;
		const float px = (pt3Point.x + width / 2) / size, py = (pt3Point.y + height / 2) / size;
		std::lock_guard lock(marksMutex);
		if (effects.size() < 64) // (none drawing: no more kept)
			effects.push_back({ iEffect, int(std::floor(px)), int(std::floor(py)), px - std::floor(px), py - std::floor(py), rotation });
	}

	void Engine::GetLandscapeDimensions(float& fWidth, float& fHeight) { GetLandscapeGameDimensions(fWidth, fHeight); }

	void Engine::GetLandscapeGameDimensions(float& fWidth, float& fHeight)
	{
		CvMap& map = GC.getMap();
		const float size = float(GC.getPLOT_SIZE());
		fWidth = float(map.getGridWidth()) * size;
		fHeight = float(map.getGridHeight()) * size;
	}

	void Utility::chooseGlobeLayer(bool layer, int index)
	{
		Engine& e = engine();
		if (layer)
		{
			std::vector<CvGlobeLayerData> layers;
			GC.getGame().getGlobeLayers(layers);
			int current = e.globeLayer;
			if (current < 0)
				for (size_t i = 0; i < layers.size(); ++i)
					if ((e.resourceLayer && layers[i].m_eType == GLOBE_LAYER_RESOURCE) || (e.unitLayer && layers[i].m_eType == GLOBE_LAYER_UNIT))
						current = int(i);
			const int chosen = index == current ? -1 : index;
			e.globeLayer = chosen;
			e.globeOption = 0;
			const GlobeLayerTypes type = chosen >= 0 && size_t(chosen) < layers.size() ? layers[size_t(chosen)].m_eType : GLOBE_LAYER_STRATEGY;
			e.resourceLayer = type == GLOBE_LAYER_RESOURCE;
			e.unitLayer = type == GLOBE_LAYER_UNIT;
		}
		else
			e.globeOption = index;
		++e.globeVersion;
		if (CvDLLInterfaceIFaceBase* ui = gDLL->getInterfaceIFace())
		{
			ui->setDirty(GlobeInfo_DIRTY_BIT, true);
			ui->setDirty(GlobeLayer_DIRTY_BIT, true);
		}
	}

	// The players' orders in a multiplayer game: a command each, in the host's frames (Net), run
	// by every game alike; in a game of one, at once.
	namespace
	{
		enum class Command : uint8_t
		{
			Message = 1,   // a game core message (CvMessageData)
			DiploEvent,    // a talk's event
			Deal,          // a deal the talk made
			KillDeal,      // a deal cancelled
			PlayerOption,  // one of the player's options
			MakeAI,        // a player who left: the AI plays on
		};

		struct CommandWriter
		{
			Net::Bytes b;
			CommandWriter(Command kind, int player)
			{
				b.push_back(std::byte(kind));
				i32(player);
			}
			void i32(int v)
			{
				for (int i = 0; i < 4; ++i)
					b.push_back(std::byte((unsigned(v) >> (8 * i)) & 0xFF));
			}
			void trades(const CLinkList<TradeData>* list)
			{
				int n = 0;
				for (CLLNode<TradeData>* t = list ? list->head() : nullptr; t; t = list->next(t))
					++n;
				i32(n);
				for (CLLNode<TradeData>* t = list ? list->head() : nullptr; t; t = list->next(t))
				{
					i32(t->m_data.m_eItemType);
					i32(t->m_data.m_iData);
					i32(t->m_data.m_bOffering ? 1 : 0);
					i32(t->m_data.m_bHidden ? 1 : 0);
				}
			}
		};

		struct CommandReader
		{
			const Net::Bytes& b;
			size_t at = 1;
			int i32()
			{
				unsigned v = 0;
				for (int i = 0; i < 4 && at < b.size(); ++i, ++at)
					v |= unsigned(b[at]) << (8 * i);
				return int(v);
			}
			void trades(CLinkList<TradeData>& list)
			{
				const int n = i32();
				for (int i = 0; i < n && at < b.size(); ++i)
				{
					TradeData t;
					t.m_eItemType = TradeableItems(i32());
					t.m_iData = i32();
					t.m_bOffering = i32() != 0;
					t.m_bHidden = i32() != 0;
					list.insertAtEnd(t);
				}
			}
		};

		// (in a multiplayer game every message goes to the host, as the original engine sends them:
		// one the game core sends while a frame runs is this game's player's too, an auto move,
		// the end of the turn, which the other games do not send)
		bool networked() { return Net::get().active(); }
	}

	void Utility::sendDiploEvent(PlayerTypes eWhoTradingWith, DiploEventTypes eDiploEvent, int iData1, int iData2)
	{
		const PlayerTypes active = GC.getGame().getActivePlayer();
		if (active == NO_PLAYER)
			return;
		if (networked())
		{
			CommandWriter w(Command::DiploEvent, active);
			for (const int v : { int(eWhoTradingWith), int(eDiploEvent), iData1, iData2 })
				w.i32(v);
			Net::get().submit(std::move(w.b));
			return;
		}
		CvPlayerAI::getPlayerNonInl(active).handleDiploEvent(eDiploEvent, eWhoTradingWith, iData1, iData2);
	}

	void Utility::sendImplementDealMessage(PlayerTypes eOtherWho, CLinkList<TradeData>* pOurList, CLinkList<TradeData>* pTheirList)
	{
		const PlayerTypes active = GC.getGame().getActivePlayer();
		if (active == NO_PLAYER)
			return;
		if (networked())
		{
			CommandWriter w(Command::Deal, active);
			w.i32(eOtherWho);
			w.trades(pOurList);
			w.trades(pTheirList);
			Net::get().submit(std::move(w.b));
			return;
		}
		GC.getGame().implementDeal(active, eOtherWho, pOurList, pTheirList);
	}

	void Utility::sendKillDeal(int iDealID, bool)
	{
		if (networked())
		{
			CommandWriter w(Command::KillDeal, GC.getGame().getActivePlayer());
			w.i32(iDealID);
			Net::get().submit(std::move(w.b));
			return;
		}
		if (CvDeal* deal = GC.getGame().getDeal(iDealID))
			deal->kill();
	}

	void Utility::sendMessageData(CvMessageData* pData)
	{
		if (!pData)
			return;
		if (networked())
		{
			CommandWriter w(Command::Message, GC.getGame().getActivePlayer());
			w.i32(pData->getType());
			MemoryStream stream;
			pData->PutInBuffer(&stream);
			w.b.insert(w.b.end(), stream.data().begin(), stream.data().end());
			delete pData;
			Net::get().submit(std::move(w.b));
			return;
		}
		m_messages.push_back(pData);
	}

	void Utility::sendMakeAI(PlayerTypes ePlayer)
	{
		CommandWriter w(Command::MakeAI, ePlayer);
		Net::get().submit(std::move(w.b));
	}

	void Utility::runCommand(const std::vector<std::byte>& command)
	{
		if (command.empty())
			return;
		CommandReader r{ command };
		const PlayerTypes player = PlayerTypes(r.i32());
		switch (Command(uint8_t(command[0])))
		{
		case Command::Message:
			if (CvMessageData* m = CvMessageData::createMessage(GameMessageTypes(r.i32())))
			{
				MemoryStream stream(std::vector<std::byte>(command.begin() + ptrdiff_t(r.at), command.end()));
				m->SetFromBuffer(&stream);
				m->Execute();
				delete m;
			}
			break;
		case Command::DiploEvent:
		{
			const int who = r.i32(), event = r.i32(), d1 = r.i32(), d2 = r.i32();
			if (player >= 0 && player < GC.getMAX_PLAYERS())
				CvPlayerAI::getPlayerNonInl(player).handleDiploEvent(DiploEventTypes(event), PlayerTypes(who), d1, d2);
			break;
		}
		case Command::Deal:
		{
			const PlayerTypes other = PlayerTypes(r.i32());
			CLinkList<TradeData> ours, theirs;
			r.trades(ours);
			r.trades(theirs);
			if (player >= 0 && player < GC.getMAX_PLAYERS())
				GC.getGame().implementDeal(player, other, &ours, &theirs);
			break;
		}
		case Command::KillDeal:
			if (CvDeal* deal = GC.getGame().getDeal(r.i32()))
				deal->kill();
			break;
		case Command::PlayerOption:
		{
			const int option = r.i32(), value = r.i32();
			if (player >= 0 && player < GC.getMAX_PLAYERS())
				CvPlayerAI::getPlayerNonInl(player).setOption(PlayerOptionTypes(option), value != 0);
			break;
		}
		case Command::MakeAI:
			if (player >= 0 && player < GC.getMAX_CIV_PLAYERS() && player != GC.getGame().getActivePlayer())
			{
				GC.getInitCore().setSlotStatus(player, SS_COMPUTER);
				CvPlayerAI::getPlayerNonInl(player).updateHuman();
			}
			break;
		}
	}

	void Utility::pumpMessages()
	{
		std::vector<CvMessageData*> batch;
		batch.swap(m_messages);
		for (CvMessageData* m : batch)
		{
			m->Execute();
			delete m;
		}
	}

	void Utility::sendPlayerOption(PlayerOptionTypes eOption, bool bValue)
	{
		const PlayerTypes active = GC.getInitCore().getActivePlayer();
		if (active == NO_PLAYER)
			return;
		// (in a multiplayer game every game learns each player's options from that player,
		// whenever the game core sends them: in a frame too, where it is this game's own)
		if (Net::get().active())
		{
			CommandWriter w(Command::PlayerOption, active);
			w.i32(eOption);
			w.i32(bValue ? 1 : 0);
			Net::get().submit(std::move(w.b));
			return;
		}
		CvPlayerAI::getPlayerNonInl(active).setOption(eOption, bValue);
	}

	bool Utility::getPlayerOption(PlayerOptionTypes ePlayerOption)
	{
		if (size_t(ePlayerOption) < m_profileOptions.size())
			return m_profileOptions[size_t(ePlayerOption)] != 0;
		return GC.getPlayerOptionInfo(ePlayerOption).getDefault();
	}

	bool Utility::getGraphicOption(GraphicOptionTypes eGraphicOption)
	{
		if (eGraphicOption >= 0 && size_t(eGraphicOption) < m_graphicOptions.size())
			if (const int8_t o = m_graphicOptions[size_t(eGraphicOption)])
				return o == 2;
		return eGraphicOption >= 0 && eGraphicOption < GC.getNumGraphicOptions() && GC.getGraphicOptionInfo(eGraphicOption).getDefault();
	}

	void Utility::setProfileOptionsFromPlayer(PlayerTypes ePlayer)
	{
		m_profileOptions.clear();
		if (ePlayer == NO_PLAYER)
			return;
		const CvPlayerAI& player = CvPlayerAI::getPlayerNonInl(ePlayer);
		for (int i = 0; i < GC.getNumPlayerOptionInfos(); ++i) // (mods may have more than the SDK's enum)
			m_profileOptions.push_back(player.isOption(static_cast<PlayerOptionTypes>(i)) ? 1 : 0);
	}

	CvWString Utility::getText(CvWString szIDTag, ...)
	{
		va_list args;
		va_start(args, szIDTag);
		CvWString out(m_text.get(szIDTag, args));
		va_end(args);
		return out;
	}

	CvWString Utility::getObjectText(CvWString szIDTag, uint uiForm, bool)
	{
		return CvWString(m_text.form(szIDTag, uiForm)); // (a grammatical form: "Warrior:Warriors")
	}

	void Utility::addText(const TCHAR* szIDTag, const wchar* szString, const wchar* szGender, const wchar* szPlural)
	{
		if (szIDTag && *szIDTag)
			m_text.add(szIDTag, szString, szGender, szPlural);
	}

	uint Utility::getNumForms(CvWString szIDTag) { return uint(m_text.forms(szIDTag)); }

	namespace
	{
		// A wildcard pattern ('*' any run, '?' one character) against a key; '*' crosses folders
		// only when deep is.
		bool wildcard(std::string_view p, std::string_view s, bool deep)
		{
			if (p.empty())
				return s.empty();
			if (p.front() == '*')
			{
				for (size_t i = 0; i <= s.size(); ++i)
				{
					if (wildcard(p.substr(1), s.substr(i), deep))
						return true;
					if (i < s.size() && s[i] == '/' && !deep)
						return false;
				}
				return false;
			}
			if (s.empty() || (p.front() != '?' && p.front() != s.front()))
				return false;
			return wildcard(p.substr(1), s.substr(1), deep);
		}
	}

	void Utility::enumerateFiles(std::vector<CvString>& files, const char* szPattern)
	{
		// "xml\\text\\*.xml" -> every matching asset key across all layers (highest layer wins).
		// ('*' does not cross folders, but under modules\: a mod's modules are folders of folders,
		// AND2's civilization packs keep their texts three deep; "modules\\*_CIV4GameText*.xml")
		const std::string pattern = makeAssetKey(szPattern ? szPattern : "");
		const size_t star = pattern.find_first_of("*?");
		const std::string prefix = pattern.substr(0, star);
		const size_t dot = pattern.rfind('.');
		const std::string ext = dot == std::string::npos || pattern.find_first_of("*?", dot) != std::string::npos ? std::string() : pattern.substr(dot);
		const bool deep = prefix.starts_with("modules/");
		std::set<std::string> seen;
		for (const auto& loc : m_assets.list(ext))
		{
			const std::string& k = loc.key;
			if (!k.starts_with(prefix) || !wildcard(pattern, k, deep))
				continue;
			if (seen.insert(k).second)
				files.push_back(CvString(k));
		}
	}

	bool Utility::isDescFileName(const char* szFileName)
	{
		const std::string f = toLowerAscii(szFileName ? szFileName : "");
		return f.ends_with(".civbeyondswordwbsave") || f.ends_with(".civwarlordswbsave") || f.ends_with(".civ4worldbuildersave");
	}

	void Utility::logMsg(const TCHAR* pLogFileName, const TCHAR* pBuf, bool bWriteToConsole, bool bTimeStamp)
	{
		if (bWriteToConsole)
			std::fputs(pBuf ? pBuf : "", stdout);
		if (m_logDir.empty()) // like LoggingEnabled = 0 in the original ini
			return;
		const std::string name = pLogFileName && *pLogFileName ? pLogFileName : "oc4core.log";
		auto& out = m_logs[name];
		if (!out.is_open())
			out.open(m_logDir / name, std::ios::out | std::ios::trunc);
		if (bTimeStamp)
		{
			const std::time_t t = std::time(nullptr);
			char stamp[32];
			std::strftime(stamp, sizeof stamp, "[%H:%M:%S] ", std::localtime(&t));
			out << stamp;
		}
		out << (pBuf ? pBuf : "");
		if (pBuf && !std::string_view(pBuf).ends_with('\n'))
			out << '\n';
		out.flush();
	}

	void Utility::logRaw(const std::string& fileName, std::string_view text)
	{
		if (m_logDir.empty())
			return;
		auto& out = m_logs[fileName];
		if (!out.is_open())
			out.open(m_logDir / fileName, std::ios::out | std::ios::trunc | std::ios::binary);
		out.write(text.data(), std::streamsize(text.size()));
		out.flush();
	}

	void Utility::messageControlLog(char* s) { logMsg("MessageControl.log", s, false, true); }

	void Utility::NiTextOut(const TCHAR* szText) { logMsg("NiTextOut.log", szText, false, true); }

	void Utility::MessageBox(const TCHAR* szText, const TCHAR* szCaption)
	{
		std::fprintf(stderr, "[oc4core] %s: %s\n", szCaption ? szCaption : "Message", szText ? szText : "");
	}
}
