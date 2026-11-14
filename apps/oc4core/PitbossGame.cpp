#include "PitbossGame.h"

#include "CvGameCoreDLL.h"

#include "CvCity.h"
#include "CvDeal.h"
#include "CvGameAI.h"
#include "CvGlobals.h"
#include "CvInfos.h"
#include "CvInitCore.h"
#include "CvMap.h"
#include "CvMessageData.h"
#include "CvPlayerAI.h"
#include "CvUnit.h"
#include "DataStream.h"
#include "GameSession.h"
#include "Utility.h"
#include "core/AssetResolver.h"
#include "core/FileUtil.h"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <limits>
#include <memory>
#include <openssl/evp.h>
#include <stdexcept>

namespace oc4::host
{
namespace
{
int integer(const pb::Bytes &b, size_t at)
{
	if (at + 4 > b.size())
		throw std::runtime_error("short command");
	int v;
	std::memcpy(&v, b.data() + at, 4);
	return v;
}
bool validateMessage(int seat, const pb::Bytes &data)
{
	if (data.size() < 13 || data[0] != std::byte{1} || integer(data, 1) != seat)
		return false;
	auto type = GameMessageTypes(integer(data, 5));
	pb::Bytes p(data.begin() + 9, data.end());
	auto i = [&](size_t at) { return integer(p, at); };
	auto range = [&](int n, int count) { return n >= 0 && n < count; };
	auto city = [&] { return i(4) >= 0 && CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).getCity(i(4)) != nullptr; };
	auto unit = [&] { return i(4) >= 0 && CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).getUnit(i(4)) != nullptr; };
	auto boolean = [&](size_t at) { return at < p.size() && uint8_t(p[at]) <= 1; };
	auto team = [&](int t) { return range(t, GC.getMAX_TEAMS()); };
	auto xy = [&](int x, int y) { return range(x, GC.getMap().getGridWidth()) && range(y, GC.getMap().getGridHeight()); };
	if (type != GAMEMESSAGE_ADVANCED_START_ACTION && i(0) != seat)
		return false;
	switch (type)
	{
	case GAMEMESSAGE_EXTENDED_GAME:
	case GAMEMESSAGE_AUTO_MOVES:
	case GAMEMESSAGE_TURN_COMPLETE:
		return p.size() == 4;
	case GAMEMESSAGE_PUSH_ORDER:
	{
		if (p.size() != 19 || !city() || !boolean(16) || !boolean(17) || !boolean(18))
			return false;
		int n = i(12);
		switch (OrderTypes(i(8)))
		{
		case ORDER_TRAIN:
			return range(n, GC.getNumUnitInfos());
		case ORDER_CONSTRUCT:
			return range(n, GC.getNumBuildingInfos());
		case ORDER_CREATE:
			return range(n, GC.getNumProjectInfos());
		case ORDER_MAINTAIN:
			return range(n, GC.getNumProcessInfos());
		default:
			return false;
		}
	}
	case GAMEMESSAGE_POP_ORDER:
		return p.size() == 12 && city() && i(8) >= 0 && i(8) < 1000;
	case GAMEMESSAGE_DO_TASK:
		if (p.size() != 24 || !city() || !boolean(20) || !boolean(21) || !boolean(22) || !boolean(23))
			return false;
		switch (TaskTypes(i(8)))
		{
		case TASK_GIFT:
			return range(i(12), GC.getMAX_CIV_PLAYERS()) && i(12) != seat;
		case TASK_SET_EMPHASIZE:
			return range(i(12), GC.getNumEmphasizeInfos());
		case TASK_CHANGE_SPECIALIST:
			return range(i(12), GC.getNumSpecialistInfos()) && i(16) >= -100 && i(16) <= 100;
		case TASK_CHANGE_WORKING_PLOT:
		case TASK_CLEAR_WORKING_OVERRIDE:
			return range(i(12), NUM_CITY_PLOTS);
		case TASK_HURRY:
			return range(i(12), GC.getNumHurryInfos());
		case TASK_RALLY_PLOT:
			return xy(i(12), i(16));
		case TASK_RAZE:
		case TASK_DISBAND:
		case TASK_LIBERATE:
		case TASK_SET_AUTOMATED_CITIZENS:
		case TASK_SET_AUTOMATED_PRODUCTION:
		case TASK_CONSCRIPT:
		case TASK_CLEAR_ORDERS:
		case TASK_CLEAR_RALLY_PLOT:
			return true;
		default:
			return false;
		}
	case GAMEMESSAGE_UPDATE_CIVICS:
		if (p.size() != size_t(4 + 4 * GC.getNumCivicOptionInfos()))
			return false;
		for (int c = 0; c < GC.getNumCivicOptionInfos(); ++c)
		{
			int civic = i(4 + c * 4);
			if (!range(civic, GC.getNumCivicInfos()) || GC.getCivicInfo(CivicTypes(civic)).getCivicOptionType() != c)
				return false;
		}
		return true;
	case GAMEMESSAGE_RESEARCH:
		return p.size() == 13 && i(4) >= -1 && i(4) <= 64 && boolean(8) && ((i(4) <= 0 && i(9) == -1) || range(i(9), GC.getNumTechInfos()));
	case GAMEMESSAGE_ESPIONAGE_CHANGE:
		return p.size() == 12 && team(i(4)) && i(8) >= -99 && i(8) <= 99;
	case GAMEMESSAGE_ADVANCED_START_ACTION:
		return false; // no advanced-start setup in v1
	case GAMEMESSAGE_MOD_NET_MESSAGE:
		return false; // no untyped Python execution messages
	case GAMEMESSAGE_CONVERT:
		return p.size() == 8 && (i(4) == -1 || range(i(4), GC.getNumReligionInfos()));
	case GAMEMESSAGE_EMPIRE_SPLIT:
		return p.size() == 8 && i(4) >= 0 && GC.getMap().getArea(i(4));
	case GAMEMESSAGE_FOUND_RELIGION:
		return p.size() == 12 && range(i(4), GC.getNumReligionInfos()) && range(i(8), GC.getNumReligionInfos());
	case GAMEMESSAGE_LAUNCH_SPACESHIP:
		return p.size() == 8 && range(i(4), GC.getNumVictoryInfos());
	case GAMEMESSAGE_EVENT_TRIGGERED:
		return p.size() == 12 && range(i(4), GC.getNumEventInfos()) && i(8) >= 0 &&
			   CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).getEventTriggered(i(8));
	case GAMEMESSAGE_JOIN_GROUP:
		return p.size() == 12 && unit() && (i(8) == -1 || CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).getUnit(i(8)));
	case GAMEMESSAGE_PUSH_MISSION:
		if (p.size() != 25 || !unit() || i(20) < 0 || i(20) >= (1 << 20) || !boolean(24))
			return false;
		switch (MissionTypes(i(8)))
		{
		case MISSION_MOVE_TO:
		case MISSION_ROUTE_TO:
		case MISSION_AIRLIFT:
		case MISSION_NUKE:
		case MISSION_RECON:
		case MISSION_PARADROP:
		case MISSION_AIRBOMB:
		case MISSION_RANGE_ATTACK:
			return xy(i(12), i(16));
		case MISSION_MOVE_TO_UNIT:
			return range(i(12), GC.getMAX_PLAYERS()) && CvPlayerAI::getPlayerNonInl(PlayerTypes(i(12))).getUnit(i(16));
		case MISSION_SPREAD:
			return range(i(12), GC.getNumReligionInfos());
		case MISSION_SPREAD_CORPORATION:
			return range(i(12), GC.getNumCorporationInfos());
		case MISSION_JOIN:
			return range(i(12), GC.getNumSpecialistInfos());
		case MISSION_CONSTRUCT:
			return range(i(12), GC.getNumBuildingInfos());
		case MISSION_BUILD:
			return range(i(12), GC.getNumBuildInfos());
		case MISSION_LEAD:
			return CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).getUnit(i(12));
		case MISSION_ESPIONAGE:
		{
			if (i(12) == -1)
				return true;
			if (!range(i(12), GC.getNumEspionageMissionInfos()) || i(16) < -1)
				return false;
			if (i(16) == -1)
				return true;
			auto &mission = GC.getEspionageMissionInfo(EspionageMissionTypes(i(12)));
			if (mission.getBuyTechCostFactor() > 0)
				return range(i(16), GC.getNumTechInfos());
			if (mission.getSwitchCivicCostFactor() > 0)
				return range(i(16), GC.getNumCivicInfos());
			if (mission.getSwitchReligionCostFactor() > 0)
				return range(i(16), GC.getNumReligionInfos());
			if (mission.getDestroyProjectCostFactor() > 0)
				return range(i(16), GC.getNumProjectInfos());
			if (mission.getDestroyBuildingCostFactor() > 0)
				return range(i(16), GC.getNumBuildingInfos());
			return i(16) < 1000000; // remaining payloads are looked up as object IDs, not array indices
		}
		case MISSION_SKIP:
		case MISSION_SLEEP:
		case MISSION_FORTIFY:
		case MISSION_PLUNDER:
		case MISSION_AIRPATROL:
		case MISSION_SEAPATROL:
		case MISSION_HEAL:
		case MISSION_SENTRY:
		case MISSION_BOMBARD:
		case MISSION_PILLAGE:
		case MISSION_SABOTAGE:
		case MISSION_DESTROY:
		case MISSION_STEAL_PLANS:
		case MISSION_FOUND:
		case MISSION_DISCOVER:
		case MISSION_HURRY:
		case MISSION_TRADE:
		case MISSION_GREAT_WORK:
		case MISSION_INFILTRATE:
		case MISSION_GOLDEN_AGE:
		case MISSION_DIE_ANIMATION:
			return true;
		default:
			return false;
		}
	case GAMEMESSAGE_AUTO_MISSION:
		return p.size() == 8 && unit();
	case GAMEMESSAGE_DO_COMMAND:
		if (p.size() != 21 || !unit() || !boolean(20))
			return false;
		switch (CommandTypes(i(8)))
		{
		case COMMAND_PROMOTION:
			return range(i(12), GC.getNumPromotionInfos()) &&
				   (i(16) == -1 || CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).getUnit(i(16)));
		case COMMAND_UPGRADE:
			return range(i(12), GC.getNumUnitInfos());
		case COMMAND_AUTOMATE:
			return range(i(12), GC.getNumAutomateInfos());
		case COMMAND_LOAD_UNIT:
			return range(i(12), GC.getMAX_PLAYERS()) && CvPlayerAI::getPlayerNonInl(PlayerTypes(i(12))).getUnit(i(16));
		case COMMAND_HOTKEY:
			return i(12) >= -1 && i(12) <= 9;
		case COMMAND_WAKE:
		case COMMAND_CANCEL:
		case COMMAND_CANCEL_ALL:
		case COMMAND_STOP_AUTOMATION:
		case COMMAND_DELETE:
		case COMMAND_GIFT:
		case COMMAND_LOAD:
		case COMMAND_UNLOAD:
		case COMMAND_UNLOAD_ALL:
			return true;
		default:
			return false;
		}
	case GAMEMESSAGE_PERCENT_CHANGE:
		return p.size() == 12 && i(4) >= -100 && i(4) <= 100 && range(i(8), NUM_COMMERCE_TYPES);
	case GAMEMESSAGE_CHANGE_VASSAL:
		return p.size() == 10 && team(i(4)) && i(4) != CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).getTeam() && boolean(8) && boolean(9);
	case GAMEMESSAGE_CHOOSE_ELECTION:
	{
		if (p.size() != 12)
			return false;
		auto vote = GC.getGame().getVoteSelection(i(8));
		return vote && (i(4) == -1 || (i(4) >= 0 && size_t(i(4)) < vote->aVoteOptions.size())) &&
			   GC.getGame().getSecretaryGeneral(vote->eVoteSource) == CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).getTeam();
	}
	case GAMEMESSAGE_DIPLO_VOTE:
		return p.size() == 12 && GC.getGame().getVoteTriggered(i(4)) && i(8) >= PLAYER_VOTE_NEVER && i(8) < GC.getMAX_CIV_TEAMS();
	case GAMEMESSAGE_CHANGE_WAR:
		return p.size() == 9 && team(i(4)) && boolean(8);
	case GAMEMESSAGE_PING:
		return p.size() == 12 && range(i(4), GC.getMap().getGridWidth()) && range(i(8), GC.getMap().getGridHeight());
	default:
		return false;
	}
}
struct GameWorld final : pb::World
{
	GameSession &session;
	Utility &utility;
	GameWorld(GameSession &s, Utility &u) : session(s), utility(u) {}
	void create(const pb::Config &c) override
	{
		NewGameSettings s;
		s.pitboss = true;
		s.humans = c.humans;
		s.mapScript = pb::value(c.values, "game.map", "Continents");
		s.worldSize = pb::value(c.values, "game.size", "WORLDSIZE_SMALL");
		s.seed = std::stoul(pb::value(c.values, "game.seed", "1"));
		s.players = std::stoi(pb::value(c.values, "game.players", "0"));
		s.gameSpeed = pb::value(c.values, "game.speed", s.gameSpeed);
		s.handicap = pb::value(c.values, "game.handicap", s.handicap);
		session.startNewGame(s);
		session.setupPitboss(c.humans, -1, true);
	}
	void load(const std::filesystem::path &p) override
	{
		session.loadGame(p);
		std::vector<int> humans;
		for (int i = 0; i < GC.getMAX_CIV_PLAYERS(); ++i)
			if (CvPlayerAI::getPlayerNonInl(PlayerTypes(i)).isHuman())
				humans.push_back(i);
		session.setupPitboss(humans, -1, true);
	}
	void save(const std::filesystem::path &p) override { session.saveGame(p); }
	int turn() const override { return GC.getGame().getGameTurn(); }
	uint32_t checksum() const override { return uint32_t(GC.getGame().calculateSyncChecksum()); }
	bool validate(int seat, const pb::Bytes &b) const override
	{
		try
		{
			if (seat < 0 || seat >= GC.getMAX_CIV_PLAYERS() || !CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).isHuman() ||
				!CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).isAlive() || b.size() < 5 || integer(b, 1) != seat)
				return false;
			int kind = uint8_t(b[0]);
			if (kind == 1)
				return validateMessage(seat, b);
			if (kind == 5)
				return b.size() == 13 && integer(b, 5) >= 0 && integer(b, 5) < NUM_PLAYEROPTION_TYPES &&
					   (integer(b, 9) == 0 || integer(b, 9) == 1);
			if (kind == 2)
			{
				if (b.size() != 21 || integer(b, 5) < 0 || integer(b, 5) >= GC.getMAX_CIV_PLAYERS() || integer(b, 5) == seat ||
					integer(b, 9) < 0 || integer(b, 9) >= NUM_DIPLOEVENT_TYPES)
					return false;
				const int d = integer(b, 13), e = integer(b, 9);
				if (e == DIPLOEVENT_JOIN_WAR || e == DIPLOEVENT_STOP_TRADING)
					return d >= 0 && d < GC.getMAX_CIV_TEAMS();
				if (e == DIPLOEVENT_RESEARCH_TECH)
					return d >= 0 && d < GC.getNumTechInfos();
				if (e == DIPLOEVENT_TARGET_CITY)
					return d >= 0 && d < GC.getMAX_CIV_PLAYERS() && CvPlayerAI::getPlayerNonInl(PlayerTypes(d)).getCity(integer(b, 17));
				if (e == DIPLOEVENT_REVOLUTION)
					return GC.getLeaderHeadInfo(CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).getPersonalityType()).getFavoriteCivic() !=
						   NO_CIVIC;
				return true;
			}
			if (kind == 4)
			{
				if (b.size() != 9)
					return false;
				auto deal = GC.getGame().getDeal(integer(b, 5));
				return deal && (deal->getFirstPlayer() == seat || deal->getSecondPlayer() == seat);
			}
			if (kind == 3)
			{
				if (b.size() < 17 || integer(b, 5) < 0 || integer(b, 5) >= GC.getMAX_CIV_PLAYERS() || integer(b, 5) == seat)
					return false;
				size_t at = 9;
				for (int list = 0; list < 2; ++list)
				{
					int n = integer(b, at);
					at += 4;
					if (n < 0 || n > 64)
						return false;
					for (int i = 0; i < n; ++i)
					{
						const int item = integer(b, at), data = integer(b, at + 4);
						if (data < 0 || (integer(b, at + 8) != 0 && integer(b, at + 8) != 1))
							return false;
						int count = 1;
						switch (TradeableItems(item))
						{
						case TRADE_TECHNOLOGIES:
							count = GC.getNumTechInfos();
							break;
						case TRADE_RESOURCES:
							count = GC.getNumBonusInfos();
							break;
						case TRADE_CIVIC:
							count = GC.getNumCivicInfos();
							break;
						case TRADE_RELIGION:
							count = GC.getNumReligionInfos();
							break;
						case TRADE_PEACE:
						case TRADE_WAR:
						case TRADE_EMBARGO:
							count = GC.getMAX_CIV_TEAMS();
							break;
						case TRADE_CITIES:
							if (!CvPlayerAI::getPlayerNonInl(PlayerTypes(list == 0 ? seat : integer(b, 5))).getCity(data))
								return false;
							count = std::numeric_limits<int>::max();
							break;
						case TRADE_GOLD:
						case TRADE_GOLD_PER_TURN:
							count = 100000000;
							break;
						case TRADE_MAPS:
						case TRADE_VASSAL:
						case TRADE_SURRENDER:
						case TRADE_OPEN_BORDERS:
						case TRADE_DEFENSIVE_PACT:
						case TRADE_PERMANENT_ALLIANCE:
						case TRADE_PEACE_TREATY:
							break;
						default:
							return false;
						}
						if (data >= count)
							return false;
						at += 12;
					}
				}
				return at == b.size();
			}
			return false;
		}
		catch (...)
		{
			return false;
		}
	}
	void step(const pb::Frame &f) override
	{
		pb::connections(f.connected);
		auto &game = GC.getGame();
		GC.getInitCore().setMPOption(MPOPTION_TURN_TIMER, f.remainingTicks >= 0);
		game.setCutoffSlice(game.getTurnSlice() + (f.remainingTicks == 0 ? -1 : f.remainingTicks));
		for (auto &o : f.orders)
			utility.runCommand(o.command);
		session.update();
	}
	void replaceAI(int seat) override
	{
		GC.getInitCore().setSlotStatus(PlayerTypes(seat), SS_COMPUTER);
		CvPlayerAI::getPlayerNonInl(PlayerTypes(seat)).updateHuman();
	}
	std::string runtimeState() const override { return session.pitbossPythonState(); }
	void runtimeState(const std::string &state) override { session.restorePitbossPythonState(state); }
};
} // namespace
void runPitboss(const pb::Config &c, GameSession &s, Utility &u)
{
	GameWorld world(s, u);
	pb::serve(c, world);
}
std::string pitbossIdentity(const AssetResolver &assets, const std::vector<std::filesystem::path> &mapDirs)
{
	std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(EVP_MD_CTX_new(), EVP_MD_CTX_free);
	if (!ctx || !EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr))
		throw std::runtime_error("ruleset hashing failed");
	auto add = [&](const void *p, size_t n)
	{
		uint64_t len = n;
		EVP_DigestUpdate(ctx.get(), &len, sizeof len);
		EVP_DigestUpdate(ctx.get(), p, n);
	};
	const std::string core = OC4_PITBOSS_CORE_REV;
	add(core.data(), core.size());
	for (auto ext : {".xml", ".py"})
		for (auto &asset : assets.list(ext))
		{
			auto data = assets.read(asset);
			add(asset.key.data(), asset.key.size());
			add(data.data(), data.size());
		}
	for (auto &dir : mapDirs)
	{
		std::error_code ec;
		std::vector<std::filesystem::path> files;
		for (auto &e : std::filesystem::directory_iterator(dir, ec))
			if (e.is_regular_file() && toLowerAscii(utf8Path(e.path().extension())) == ".py")
				files.push_back(e.path());
		std::sort(files.begin(), files.end());
		for (auto &file : files)
		{
			auto key = utf8Path(file.filename());
			auto data = readWholeFile(file);
			add(key.data(), key.size());
			add(data.data(), data.size());
		}
	}
	unsigned char hash[32];
	unsigned n = 0;
	EVP_DigestFinal_ex(ctx.get(), hash, &n);
	std::string out = "openciv4-pitboss-v1:";
	for (unsigned i = 0; i < n; ++i)
	{
		const char *h = "0123456789abcdef";
		out += h[hash[i] >> 4];
		out += h[hash[i] & 15];
	}
	return out;
}
} // namespace oc4::host
