#include "GameSession.h"

#include "Net.h"
#ifdef __linux__
#include "Pitboss.h"
#endif
#include "core/FileUtil.h"

#include "DataStream.h"
#include "PythonHost.h"
#include "SaveFile.h"
#include "Utility.h"

#include "CvGameCoreDLL.h"
#include "CvEventReporter.h"
#include "CvGameAI.h"
#include "CvGlobals.h"
#include "CvInfos.h"
#include "CvInitCore.h"
#include "CvMap.h"
#include "CvMapGenerator.h"
#include "CvPlayerAI.h"
#include "CvTeamAI.h"

#include <pybind11/pybind11.h>

#include <windows.h>

#include <algorithm>
#include <format>
#include <fstream>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace oc4::host
{
	namespace
	{
		// CPU time of the calling thread: steadier than wall time on a busy machine.
		double threadCpuSeconds()
		{
#ifdef _WIN32
			FILETIME created, exited, kernel, user;
			GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
			const auto ticks = [](FILETIME t) { return (ULONGLONG(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
			return double(ticks(kernel) + ticks(user)) * 1e-7;
#else
			timespec t{};
			clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
			return double(t.tv_sec) + double(t.tv_nsec) * 1e-9;
#endif
		}

		CvPlayerAI& player(int i) { return CvPlayerAI::getPlayerNonInl(static_cast<PlayerTypes>(i)); }
		CvTeamAI& team(int i) { return CvTeamAI::getTeamNonInl(static_cast<TeamTypes>(i)); }

		// Player and team counts come from the DLL: mods change them (AND2 has 50 civilizations),
		// the SDK headers the host is built with have the original 18.
		int maxPlayers() { return GC.getMAX_PLAYERS(); }
		int maxCivPlayers() { return GC.getMAX_CIV_PLAYERS(); }
		int maxTeams() { return GC.getMAX_TEAMS(); }
		PlayerTypes barbarianPlayer() { return static_cast<PlayerTypes>(GC.getBARBARIAN_PLAYER()); }
		TeamTypes barbarianTeam() { return static_cast<TeamTypes>(GC.getBARBARIAN_TEAM()); }

		int infoType(const std::string& name)
		{
			const int type = GC.getInfoTypeForString(name.c_str());
			if (type < 0)
				throw std::runtime_error("unknown type " + name);
			return type;
		}

		// A setting the loaded game may not have under the engine's default name (Realism Invictus has
		// no GAMESPEED_NORMAL): then the game's own standard one from its GlobalDefines, the setting the
		// original's setup screen starts from. A name given explicitly must exist.
		int infoTypeOrStandard(const std::string& name, const std::string& engineDefault, const char* standardDefine)
		{
			if (name != engineDefault || GC.getInfoTypeForString(name.c_str()) >= 0)
				return infoType(name);
			// (the XML loader has turned the define's type name into its index, as STANDARD_TURNTIMER)
			const int standard = GC.getDefineINT(standardDefine);
			if (standard < 0)
				throw std::runtime_error("unknown type " + name + " and no " + standardDefine);
			std::printf("  %s instead of %s (the game has no %s)\n", standardDefine, name.c_str(), name.c_str());
			return standard;
		}

		// Small deterministic generator for the staging room choices (civs, leaders).
		struct SetupRandom
		{
			unsigned long long state;
			explicit SetupRandom(unsigned int seed) : state(seed * 0x9E3779B97F4A7C15ull + 1) {}
			int next(int n)
			{
				state ^= state << 13;
				state ^= state >> 7;
				state ^= state << 17;
				return static_cast<int>(state % static_cast<unsigned long long>(n));
			}
		};
	}

	void GameSession::initEngine()
	{
		// The pathfinders belong to the engine; the DLL configures them in CvMap::setup.
		CvDLLFAStarIFaceBase& fa = *m_utility.getFAStarIFace();
		GC.setPathFinder(fa.create());
		GC.setInterfacePathFinder(fa.create());
		GC.setStepFinder(fa.create());
		GC.setRouteFinder(fa.create());
		GC.setBorderFinder(fa.create());
		GC.setAreaFinder(fa.create());
		GC.setPlotGroupFinder(fa.create());

		// Python's "Init" event: loads CvEventInterface and with it the event manager and screens.
		CvEventReporter::getInstance().init();
	}

	void GameSession::setupInitCore(const NewGameSettings& s)
	{
		CvInitCore& ic = GC.getInitCore();
		ic.init(s.pitboss ? GAMEMODE_PITBOSS : GAMEMODE_NORMAL);
		ic.setType(s.pitboss ? GAME_MP_NEW : GAME_SP_NEW);
		if (s.pitboss) ic.setMPOption(MPOPTION_SIMULTANEOUS_TURNS, true);
		// The "loaded" init core describes the game being played too: a new one (AND2 asks it
		// whether the game is new before it offers to recalculate an old save's modifiers).
		GC.getLoadedInitCore().setType(s.pitboss ? GAME_MP_NEW : GAME_SP_NEW);

		// Python's own random module from the game's seed: mods' scripts draw from it (CoM's
		// natural wonders), seeded by the clock it would make each game of a seed another.
		m_python.run(std::format("import random\nrandom.seed({})\n", s.seed));
		m_python.setMapScriptModule(s.mapScript);
		ic.setMapScriptName(CvWString(s.mapScript)); // reads the script's custom options
		ic.setWorldSize(static_cast<WorldSizeTypes>(infoType(s.worldSize)));
		ic.setClimate(static_cast<ClimateTypes>(infoTypeOrStandard(s.climate, NewGameSettings{}.climate, "STANDARD_CLIMATE")));
		ic.setSeaLevel(static_cast<SeaLevelTypes>(infoTypeOrStandard(s.seaLevel, NewGameSettings{}.seaLevel, "STANDARD_SEALEVEL")));
		ic.setEra(static_cast<EraTypes>(infoTypeOrStandard(s.era, NewGameSettings{}.era, "STANDARD_ERA")));
		ic.setGameSpeed(static_cast<GameSpeedTypes>(infoTypeOrStandard(s.gameSpeed, NewGameSettings{}.gameSpeed, "STANDARD_GAMESPEED")));
		ic.setTurnTimer(static_cast<TurnTimerTypes>(GC.getDefineINT("STANDARD_TURNTIMER")));
		ic.setCalendar(static_cast<CalendarTypes>(GC.getDefineINT("STANDARD_CALENDAR")));

		const int numVictories = GC.getNumVictoryInfos();
		std::unique_ptr<bool[]> victories(new bool[size_t(numVictories)]);
		std::fill_n(victories.get(), numVictories, true);
		ic.setVictories(numVictories, victories.get());

		ic.setSyncRandSeed(s.seed);
		ic.setMapRandSeed(s.seed * 7919u + 13u);

		// Players: 0 is the (watching) human, the rest AI with distinct playable civs.
		SetupRandom rng(s.seed);
		const int barbarianCiv = GC.getDefineINT("BARBARIAN_CIVILIZATION");
		std::vector<int> civs;
		for (int c = 0; c < GC.getNumCivilizationInfos(); ++c)
			if (c != barbarianCiv && GC.getCivilizationInfo(static_cast<CivilizationTypes>(c)).isAIPlayable())
				civs.push_back(c);
		for (size_t i = civs.size(); i > 1; --i)
			std::swap(civs[i - 1], civs[size_t(rng.next(int(i)))]);
		// (a civilization chosen: the human's, the others as they came)
		const int chosenCiv = s.civilization.empty() ? -1 : infoType(s.civilization);
		const int chosenLeader = s.leader.empty() ? -1 : infoType(s.leader);
		if (chosenCiv >= 0)
		{
			std::erase(civs, chosenCiv);
			civs.insert(civs.begin(), chosenCiv);
		}

		const WorldSizeTypes worldSize = ic.getWorldSize();
		int numPlayers = s.players > 0 ? s.players : GC.getWorldInfo(worldSize).getDefaultPlayers();
		numPlayers = std::clamp(numPlayers, 2, std::min<int>(maxCivPlayers(), int(civs.size())));
		if (s.pitboss && std::ranges::any_of(s.humans, [&](int p) { return p < 0 || p >= numPlayers; }))
			throw std::runtime_error("human seat outside configured player count");

		const HandicapTypes humanHandicap = static_cast<HandicapTypes>(infoTypeOrStandard(s.handicap, NewGameSettings{}.handicap, "STANDARD_HANDICAP"));
		const HandicapTypes aiHandicap = static_cast<HandicapTypes>(GC.getDefineINT("AI_HANDICAP"));
		for (int p = 0; p < numPlayers; ++p)
		{
			const auto civ = static_cast<CivilizationTypes>(civs[size_t(p)]);
			const CvCivilizationInfo& civInfo = GC.getCivilizationInfo(civ);
			std::vector<int> leaders;
			for (int l = 0; l < GC.getNumLeaderHeadInfos(); ++l)
				if (civInfo.isLeaders(l))
					leaders.push_back(l);
			const PlayerTypes id = static_cast<PlayerTypes>(p);
			const bool human = s.pitboss ? std::ranges::find(s.humans, p) != s.humans.end() : p == 0;
			ic.setSlotStatus(id, human ? SS_TAKEN : SS_COMPUTER);
			ic.setCiv(id, civ);
			const bool chosen = p == 0 && chosenLeader >= 0 && std::ranges::find(leaders, chosenLeader) != leaders.end();
			ic.setLeader(id, static_cast<LeaderHeadTypes>(chosen ? chosenLeader : leaders.empty() ? 0 : leaders[size_t(rng.next(int(leaders.size())))]));
			ic.setTeam(id, static_cast<TeamTypes>(p));
			ic.setHandicap(id, human ? humanHandicap : aiHandicap);
			ic.setColor(id, static_cast<PlayerColorTypes>(civInfo.getDefaultPlayerColor()));
			ic.setPlayableCiv(id, civInfo.isPlayable());
		}

		const auto barbCiv = static_cast<CivilizationTypes>(barbarianCiv);
		ic.setSlotStatus(barbarianPlayer(), SS_COMPUTER);
		ic.setCiv(barbarianPlayer(), barbCiv);
		ic.setLeader(barbarianPlayer(), static_cast<LeaderHeadTypes>(GC.getDefineINT("BARBARIAN_LEADER")));
		ic.setTeam(barbarianPlayer(), barbarianTeam());
		ic.setHandicap(barbarianPlayer(), static_cast<HandicapTypes>(GC.getDefineINT("BARBARIAN_HANDICAP")));
		ic.setColor(barbarianPlayer(), static_cast<PlayerColorTypes>(GC.getCivilizationInfo(barbCiv).getDefaultPlayerColor()));

		ic.setActivePlayer(static_cast<PlayerTypes>(s.pitboss ? s.humans.front() : 0));
	}

	namespace py = pybind11;

	void GameSession::setupScenarioInitCore(const NewGameSettings& s)
	{
		CvInitCore& ic = GC.getInitCore();
		ic.init(GAMEMODE_NORMAL);
		ic.setType(GAME_SP_SCENARIO);
		GC.getLoadedInitCore().setType(GAME_SP_SCENARIO);
		py::module_ wb = py::module_::import("CvWBInterface");
		if (wb.attr("readDesc")(py::make_tuple(s.mapScript)).cast<int>() < 0)
			throw std::runtime_error("cannot read the scenario " + s.mapScript);

		// The game: CvWBInterface.getGameData's tuple, in its order.
		const py::tuple g = wb.attr("getGameData")();
		size_t k = 0;
		const auto next = [&] { return g[k++].cast<int>(); };
		ic.setWorldSize(static_cast<WorldSizeTypes>(next()));
		ic.setClimate(static_cast<ClimateTypes>(next()));
		ic.setSeaLevel(static_cast<SeaLevelTypes>(next()));
		ic.setEra(static_cast<EraTypes>(next()));
		ic.setGameSpeed(static_cast<GameSpeedTypes>(next()));
		ic.setCalendar(static_cast<CalendarTypes>(next()));
		for (int o = 0; o < GC.getNumGameOptionInfos(); ++o)
			ic.setOption(static_cast<GameOptionTypes>(o), false);
		for (int n = next(); n > 0; --n)
			if (const int o = next(); o >= 0)
				ic.setOption(static_cast<GameOptionTypes>(o), true);
		for (int n = next(); n > 0; --n)
			if (const int o = next(); o >= 0)
				ic.setMPOption(static_cast<MultiplayerOptionTypes>(o), true);
		for (int n = next(); n > 0; --n)
			if (const int o = next(); o >= 0)
				ic.setForceControl(static_cast<ForceControlTypes>(o), true);
		std::unique_ptr<bool[]> victories(new bool[size_t(GC.getNumVictoryInfos())]());
		for (int n = next(); n > 0; --n)
			if (const int v = next(); v >= 0 && v < GC.getNumVictoryInfos())
				victories[size_t(v)] = true;
		ic.setVictories(GC.getNumVictoryInfos(), victories.get());
		ic.setGameTurn(next());
		ic.setMaxTurns(next());
		ic.setMaxCityElimination(next());
		ic.setNumAdvancedStartPoints(next());
		ic.setTargetScore(next());
		ic.setTurnTimer(static_cast<TurnTimerTypes>(GC.getDefineINT("STANDARD_TURNTIMER")));
		ic.setSyncRandSeed(s.seed);
		ic.setMapRandSeed(s.seed * 7919u + 13u);
		// The scenario is the map script: the game core asks getWBMapScript.
		ic.setMapScriptName(CvWString(s.mapScript.c_str()));

		// The players: nine values each, then their five texts.
		const py::tuple p = wb.attr("getPlayerData")();
		const py::tuple d = wb.attr("getPlayerDesc")();
		const int wanted = s.civilization.empty() ? -1 : GC.getInfoTypeForString(s.civilization.c_str());
		// (the human: the one of the civilization asked for, else the first playable)
		int human = -1;
		for (int pass = 0; pass < 2 && human < 0; ++pass)
			for (int i = 0; i < maxCivPlayers() && size_t(i * 9 + 8) < p.size() && human < 0; ++i)
			{
				const int civ = p[size_t(i * 9)].cast<int>(), leader = p[size_t(i * 9 + 2)].cast<int>();
				if (civ >= 0 && leader >= 0 && (pass == 0 ? civ == wanted : p[size_t(i * 9 + 1)].cast<bool>()))
					human = i;
			}
		for (int i = 0; i < maxCivPlayers() && size_t(i * 9 + 8) < p.size(); ++i)
		{
			const PlayerTypes id = static_cast<PlayerTypes>(i);
			const int civ = p[size_t(i * 9)].cast<int>(), leader = p[size_t(i * 9 + 2)].cast<int>();
			if (civ < 0 || leader < 0)
			{
				ic.setSlotStatus(id, SS_CLOSED);
				continue;
			}
			ic.setSlotStatus(id, i == human ? SS_TAKEN : SS_COMPUTER);
			ic.setCiv(id, static_cast<CivilizationTypes>(civ));
			ic.setPlayableCiv(id, p[size_t(i * 9 + 1)].cast<bool>());
			ic.setLeader(id, static_cast<LeaderHeadTypes>(leader));
			const int handicap = p[size_t(i * 9 + 3)].cast<int>();
			ic.setHandicap(id, static_cast<HandicapTypes>(handicap >= 0 ? handicap : GC.getDefineINT("AI_HANDICAP")));
			ic.setTeam(id, static_cast<TeamTypes>(p[size_t(i * 9 + 4)].cast<int>()));
			const int color = p[size_t(i * 9 + 5)].cast<int>();
			ic.setColor(id, static_cast<PlayerColorTypes>(color >= 0 ? color : GC.getCivilizationInfo(static_cast<CivilizationTypes>(civ)).getDefaultPlayerColor()));
			if (const int art = p[size_t(i * 9 + 6)].cast<int>(); art >= 0)
				ic.setArtStyle(id, static_cast<ArtStyleTypes>(art));
			ic.setMinorNationCiv(id, p[size_t(i * 9 + 7)].cast<bool>());
			ic.setWhiteFlag(id, p[size_t(i * 9 + 8)].cast<bool>());
			const auto text = [&](size_t j) {
				const py::object o = d[size_t(i) * 5 + j];
				if (PyUnicode_Check(o.ptr()))
					return CvWString(o.cast<std::wstring>().c_str());
				return CvWString(CvString(py::str(o).cast<std::string>().c_str()));
			};
			if (size_t(i) * 5 + 4 < d.size())
			{
				if (const CvWString t = text(0); !t.empty())
					ic.setCivDescription(id, t);
				if (const CvWString t = text(1); !t.empty())
					ic.setCivShortDesc(id, t);
				if (const CvWString t = text(2); !t.empty())
					ic.setLeaderName(id, t);
				if (const CvWString t = text(3); !t.empty())
					ic.setCivAdjective(id, t);
				if (const CvWString t = text(4); !t.empty())
					ic.setFlagDecal(id, t);
			}
		}
		const auto barbCiv = static_cast<CivilizationTypes>(GC.getDefineINT("BARBARIAN_CIVILIZATION"));
		ic.setSlotStatus(barbarianPlayer(), SS_COMPUTER);
		ic.setCiv(barbarianPlayer(), barbCiv);
		ic.setLeader(barbarianPlayer(), static_cast<LeaderHeadTypes>(GC.getDefineINT("BARBARIAN_LEADER")));
		ic.setTeam(barbarianPlayer(), barbarianTeam());
		ic.setHandicap(barbarianPlayer(), static_cast<HandicapTypes>(GC.getDefineINT("BARBARIAN_HANDICAP")));
		ic.setColor(barbarianPlayer(), static_cast<PlayerColorTypes>(GC.getCivilizationInfo(barbCiv).getDefaultPlayerColor()));
		ic.setActivePlayer(static_cast<PlayerTypes>(std::max(human, 0)));
	}

	void GameSession::startScenario(const NewGameSettings& settings)
	{
		m_utility.setGameInitializing(true);
		m_utility.setProfileOptionsFromPlayer(NO_PLAYER);
		m_python.run(std::format("import random\nrandom.seed({})\n", settings.seed));
		setupScenarioInitCore(settings);
		CvInitCore& ic = GC.getInitCore();
		CvGame& game = GC.getGame();
		py::module_ wb = py::module_::import("CvWBInterface");
		const py::object map = wb.attr("WBDesc").attr("mapDesc");

		game.init(ic.getHandicap(ic.getActivePlayer()));
		CvMapInitData data(map.attr("iGridW").cast<int>(), map.attr("iGridH").cast<int>(), map.attr("iTopLatitude").cast<int>(),
			map.attr("iBottomLatitude").cast<int>(), map.attr("bWrapX").cast<int>() != 0, map.attr("bWrapY").cast<int>() != 0);
		GC.getMap().init(&data);
		for (int t = 0; t < maxTeams(); ++t)
			team(t).init(static_cast<TeamTypes>(t));
		for (int p = 0; p < maxPlayers(); ++p)
			player(p).init(static_cast<PlayerTypes>(p));
		game.initDiplomacy();

		// As the original engine plays a scenario: its map, its starting plots, the era's
		// free things (CvGame::setInitialItems: units where none are), then its own.
		if (wb.attr("applyMapDesc")().cast<int>() < 0)
			throw std::runtime_error("cannot apply the scenario's map");
		wb.attr("getAssignedStartingPlots")();
		game.setInitialItems();
		wb.attr("applyInitialItems")();
		game.initScoreCalculation();
		game.setFinalInitialized(true);
		game.initEvents();
		m_python.callFunction("CvAppInterface", "preGameStart", nullptr);
		CvEventReporter::getInstance().gameStart();

		m_utility.setGameInitializing(false);
		m_utility.setGameActive(true);
		activateFirstTurn();
	}

	void GameSession::startNewGame(const NewGameSettings& settings)
	{
		if (m_utility.isDescFileName(settings.mapScript.c_str()))
		{
			startScenario(settings);
			return;
		}
		m_utility.setGameInitializing(true);
		m_utility.setProfileOptionsFromPlayer(NO_PLAYER);
		setupInitCore(settings);
		CvInitCore& ic = GC.getInitCore();
		CvGame& game = GC.getGame();

		game.init(ic.getHandicap(ic.getActivePlayer()));
		GC.getMap().init(); // grid size, latitudes and wrapping from the map script
		for (int t = 0; t < maxTeams(); ++t)
			team(t).init(static_cast<TeamTypes>(t));
		for (int p = 0; p < maxPlayers(); ++p)
			player(p).init(static_cast<PlayerTypes>(p));
		game.initDiplomacy();

		CvMapGenerator& generator = CvMapGenerator::GetInstance();
		generator.generateRandomMap(); // plot types and terrain from the map script
		generator.addGameElements();   // rivers, lakes, features, bonuses, goody huts

		game.setInitialItems(); // free techs, starting plots, starting units
		game.initScoreCalculation();
		game.setFinalInitialized(true);
		game.initEvents();
		// As the original engine: CvAppInterface.preGameStart(), then the "GameStart" event (BUG
		// initialises mods' event handlers on "PreGameStart").
		m_python.callFunction("CvAppInterface", "preGameStart", nullptr);
		CvEventReporter::getInstance().gameStart();

		m_utility.setGameInitializing(false);
		m_utility.setGameActive(true);
		activateFirstTurn();
	}

	void GameSession::activateFirstTurn()
	{
		CvGame& game = GC.getGame();
		if (game.isMPOption(MPOPTION_SIMULTANEOUS_TURNS))
		{
			for (int p = 0; p < maxPlayers(); ++p)
				if (player(p).isAlive())
					player(p).setTurnActive(true);
			return;
		}
		for (int p = 0; p < maxPlayers(); ++p)
			if (player(p).isAlive())
			{
				player(p).setTurnActive(true);
				return;
			}
	}

	void GameSession::autoPlay(int turns, const std::function<void(int turn)>& onTurn)
	{
		beginAutoPlay(turns);
		while (!autoPlayDone())
			if (stepAutoPlay() && onTurn)
				onTurn(GC.getGame().getGameTurn());
		endAutoPlay();
	}

	// A game core may want to know when the engine's interface, not a game update, runs (in a
	// multiplayer game what it caches then is this game's alone): oc4_setInterfaceTime, when it has one.
	void setInterfaceTime(bool interface)
	{
		using Fn = void (*)(bool);
		static const Fn fn = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleA(OC4_GAME_CORE_DLL), "oc4_setInterfaceTime"));
		if (fn)
			fn(interface);
	}

	void GameSession::update()
	{
		// A multiplayer game: the host's next frame, when it has come (its orders, then one
		// update, as in every other game), else nothing yet.
		if (Net& net = Net::get(); net.active())
		{
			Net::Frame frame;
			if (!net.takeFrame(frame))
				return;
			setInterfaceTime(false);
#ifdef __linux__
			if (net.pitboss())
			{
				pb::connections(frame.connected);
				GC.getInitCore().setMPOption(MPOPTION_TURN_TIMER, frame.pitbossTicks >= 0);
				GC.getGame().setCutoffSlice(GC.getGame().getTurnSlice() + (frame.pitbossTicks == 0 ? -1 : frame.pitbossTicks));
			}
#endif
			net.setInFrame(true);
			for (const auto& command : frame.commands)
				m_utility.runCommand(command);
			m_utility.pumpMessages();
			GC.getGame().update();
			net.setInFrame(false);
			setInterfaceTime(true);
			const bool check = frame.number % Net::kChecksumEvery == 0;
			net.executed(frame.number, (check || net.pitboss()) ? GC.getGame().calculateSyncChecksum() : 0);
			// (development, to find where the games of one out of sync part: OC4_MP_LOG=<file> gets
			// each frame's orders and each check's checksum, OC4_MP_CHECK_SCRIPT=<file.py> runs at
			// each check, OC4_MP_FRAME telling it which)
			if (static const char* path = std::getenv("OC4_MP_LOG"); path && (check || !frame.commands.empty()))
				if (FILE* f = std::fopen(path, "a"))
				{
					std::fprintf(f, "frame %u turn %d slice %d", frame.number, GC.getGame().getGameTurn(), GC.getGame().getTurnSlice());
					if (check)
						std::fprintf(f, " checksum %08X", unsigned(GC.getGame().calculateSyncChecksum()));
					for (const auto& command : frame.commands)
						std::fprintf(f, " [kind %d, %zu bytes]", int(command.empty() ? 0 : uint8_t(command[0])), command.size());
					std::fprintf(f, "\n");
					std::fclose(f);
				}
			if (static const char* script = std::getenv("OC4_MP_CHECK_SCRIPT"); script && check)
			{
				_putenv_s("OC4_MP_FRAME", std::to_string(frame.number).c_str());
				const auto bytes = readWholeFile(script);
				m_python.run(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
			}
			// (OC4_MP_STATE_DIR=<folder>: the game's state, as a save has it, at each check, part by part)
			if (static const char* dir = std::getenv("OC4_MP_STATE_DIR"); dir && check)
			{
				const auto write = [&](const char* part, const auto& fn) {
					MemoryStream s;
					fn(s);
					std::ofstream(std::filesystem::path(dir) / std::format("f{:05}_{}.bin", frame.number, part), std::ios::binary)
						.write(reinterpret_cast<const char*>(s.data().data()), std::streamsize(s.data().size()));
				};
				write("game", [](MemoryStream& s) { GC.getGame().write(&s); });
				write("map", [](MemoryStream& s) { GC.getMap().write(&s); });
				write("teams", [&](MemoryStream& s) {
					for (int t = 0; t < maxTeams(); ++t)
						team(t).write(&s);
				});
				for (int p = 0; p < maxPlayers(); ++p)
					write(std::format("player{:02}", p).c_str(), [&](MemoryStream& s) { player(p).write(&s); });
			}
			return;
		}
		m_utility.pumpMessages();
		GC.getGame().update();
	}

	void GameSession::setupPitboss(const std::vector<int>& humans, int seat, bool server)
	{
		m_utility.setPitboss(server, seat);
		GC.getInitCore().setMode(GAMEMODE_PITBOSS);
		setupMultiplayer(humans, seat);
	}

	std::string GameSession::pitbossPythonState() const
	{
		return py::repr(py::module_::import("random").attr("getstate")()).cast<std::string>();
	}

	void GameSession::restorePitbossPythonState(const std::string& state)
	{
		// literal_eval accepts the RNG's tuple of numbers, without executing Python from the wire.
		auto tuple = py::module_::import("ast").attr("literal_eval")(py::bytes(state));
		py::module_::import("random").attr("setstate")(tuple);
	}

	void GameSession::setupMultiplayer(const std::vector<int>& humans, int seat)
	{
		CvInitCore& ic = GC.getInitCore();
		ic.setType(GAME_MP_LOAD);
		ic.setMPOption(MPOPTION_SIMULTANEOUS_TURNS, true);
		for (int p = 0; p < maxCivPlayers(); ++p)
		{
			const bool human = std::ranges::find(humans, p) != humans.end();
			const SlotStatus status = ic.getSlotStatus(static_cast<PlayerTypes>(p));
			if (human)
				ic.setSlotStatus(static_cast<PlayerTypes>(p), SS_TAKEN);
			else if (status == SS_TAKEN || status == SS_OPEN)
				ic.setSlotStatus(static_cast<PlayerTypes>(p), SS_COMPUTER);
			player(p).updateHuman();
		}
		GC.getGame().setActivePlayer(static_cast<PlayerTypes>(seat));
		if (seat >= 0) m_utility.setProfileOptionsFromPlayer(static_cast<PlayerTypes>(seat));
		// (the mods' scripts draw from Python's random module too: the same numbers in every game)
		m_python.run(std::format("import random\nrandom.seed({})\n", ic.getSyncRandSeed() ^ unsigned(GC.getGame().getGameTurn())));
		setInterfaceTime(true); // (from now on, between frames)
	}

	void GameSession::beginAutoPlay(int turns)
	{
		CvGame& game = GC.getGame();
		m_autoPlay = {};
		m_autoPlay.turn = game.getGameTurn();
		m_autoPlay.endTurn = m_autoPlay.turn + turns;

		// Nobody gives orders: other humans (hotseat saves, whose open slots count as
		// human) become AI players.
		CvInitCore& ic = GC.getInitCore();
		for (int p = 0; p < maxCivPlayers(); ++p)
			if (p != ic.getActivePlayer() && player(p).isHuman())
				ic.setSlotStatus(static_cast<PlayerTypes>(p), SS_COMPUTER);

		// Through Python, as autoplay scripts do: mods that let each player autoplay (AND2
		// and its relatives) changed the DLL function to take the player first, and the
		// Python binding follows the DLL's own signature.
		const std::string active = std::to_string(ic.getActivePlayer()), count = std::to_string(turns);
		if (!m_python.run("import CvPythonExtensions\n"
						  "_game = CvPythonExtensions.CyGame()\n"
						  "try:\n"
						  "    _game.setAIAutoPlay(" + count + ")\n"
						  "except TypeError:\n"
						  "    _game.setAIAutoPlay(" + active + ", " + count + ")\n"
						  "del _game\n"))
			throw std::runtime_error("CyGame.setAIAutoPlay failed");

		std::printf("  turn  alive cities   pop  units  checksum     ms\n");
		m_autoPlay.turnStart = std::chrono::steady_clock::now();
		m_autoPlay.cpuStart = threadCpuSeconds();
	}

	bool GameSession::autoPlayDone() const
	{
		CvGame& game = GC.getGame();
		return m_autoPlay.stalled || game.getGameTurn() >= m_autoPlay.endTurn || game.getGameState() == GAMESTATE_OVER;
	}

	bool GameSession::stepAutoPlay()
	{
		CvGame& game = GC.getGame();
		m_utility.pumpMessages();
		game.update();
		++m_autoPlay.updates;
		if (game.getGameTurn() == m_autoPlay.turn)
		{
			if (++m_autoPlay.sinceTurn == 100000)
			{
				std::printf("  stalled: turn %d does not end; active player %d\n", m_autoPlay.turn, game.getActivePlayer());
				for (int p = 0; p < maxPlayers(); ++p)
					if (player(p).isAlive() && player(p).isTurnActive())
						std::printf("    player %d has the turn: human %d, units %d, cities %d\n", p, player(p).isHuman(), player(p).getNumUnits(),
							player(p).getNumCities());
				m_autoPlay.stalled = true;
			}
			return false;
		}
		m_autoPlay.sinceTurn = 0;
		const auto now = std::chrono::steady_clock::now();
		int cities = 0, units = 0, alive = 0, pop = 0;
		for (int p = 0; p < maxPlayers(); ++p)
		{
			CvPlayerAI& pl = player(p);
			if (!pl.isAlive())
				continue;
			++alive;
			cities += pl.getNumCities();
			units += pl.getNumUnits();
			pop += pl.getTotalPopulation();
		}
		std::printf("  %4d  %5d %6d %5d %6d  %08X %6.0f\n", game.getGameTurn(), alive, cities, pop, units,
			static_cast<unsigned>(game.calculateSyncChecksum()), std::chrono::duration<double, std::milli>(now - m_autoPlay.turnStart).count());
		m_autoPlay.turnStart = now;
		m_autoPlay.turn = game.getGameTurn();
		return true;
	}

	void GameSession::endAutoPlay()
	{
		std::printf("  (%lld game updates, %.2fs CPU)\n", m_autoPlay.updates, threadCpuSeconds() - m_autoPlay.cpuStart);
	}

	void GameSession::saveGame(const std::filesystem::path& path)
	{
		CvEventReporter::getInstance().preSave(); // Python "OnPreSave"

		SaveFile save;
		save.modFolder = m_modFolder;
		MemoryStream init;
		GC.getInitCore().write(&init);
		save.initCore = std::move(init.data());

		// The original engine's order: game, map, teams, players, statistics.
		MemoryStream data;
		unsigned int parts[5];
		GC.getGame().write(&data);
		parts[0] = data.GetPosition();
		GC.getMap().write(&data);
		parts[1] = data.GetPosition();
		for (int t = 0; t < maxTeams(); ++t)
			team(t).write(&data);
		parts[2] = data.GetPosition();
		for (int p = 0; p < maxPlayers(); ++p)
			player(p).write(&data);
		parts[3] = data.GetPosition();
		CvEventReporter::getInstance().writeStatistics(&data);
		parts[4] = data.GetPosition();
		save.gameData = std::move(data.data());
		// (compare with --probe-save when a game core reads its saves back differently)
		std::printf("  save: init core %zu, game %u, map %u, teams %u, players %u, statistics %u bytes\n", save.initCore.size(), parts[0],
			parts[1] - parts[0], parts[2] - parts[1], parts[3] - parts[2], parts[4] - parts[3]);

		// Script data: CvAppInterface.onSave() returns a pickle string.
		std::vector<byte> pickled;
		if (m_python.callFunction("CvAppInterface", "onSave", nullptr, &pickled))
			save.pythonData.assign(reinterpret_cast<const char*>(pickled.data()), pickled.size());

		save.write(path);
	}

	namespace
	{
		// A mod's DLL has its own data layout; the original engine restarts into the mod,
		// the host can only ask for the right --mod.
		void checkMod(const std::filesystem::path& path, const SaveFile& save, const std::string& modFolder)
		{
			if (_stricmp(save.modFolder.c_str(), modFolder.c_str()) != 0)
				throw std::runtime_error(path.filename().string() + " belongs to " +
					(save.modFolder.empty() ? std::string("the unmodded game") : "mod \"" + save.modFolder + "\"") + ", running " +
					(modFolder.empty() ? std::string("without a mod") : "mod \"" + modFolder + "\""));
		}
	}

	void GameSession::checkSave(const std::filesystem::path& path) const { checkMod(path, SaveFile::read(path), m_modFolder); }

	void GameSession::loadGame(const std::filesystem::path& path)
	{
		const SaveFile save = SaveFile::read(path);
		checkMod(path, save, m_modFolder);

		m_utility.setGameInitializing(true);
		readInitCore(save);
		CvInitCore& ic = GC.getInitCore();
		switch (ic.getType())
		{
		case GAME_SP_NEW:
		case GAME_SP_SCENARIO: ic.setType(GAME_SP_LOAD); break;
		case GAME_MP_NEW:
		case GAME_MP_SCENARIO: ic.setType(GAME_MP_LOAD); break;
		case GAME_HOTSEAT_NEW:
		case GAME_HOTSEAT_SCENARIO: ic.setType(GAME_HOTSEAT_LOAD); break;
		case GAME_PBEM_NEW:
		case GAME_PBEM_SCENARIO: ic.setType(GAME_PBEM_LOAD); break;
		default: break;
		}
		m_python.setMapScriptModule(std::string(CvString(ic.getMapScriptName()).GetCString()));

		MemoryStream data(save.gameData);
		GC.getGame().read(&data);
		GC.getMap().read(&data);
		for (int t = 0; t < maxTeams(); ++t)
			team(t).read(&data);
		for (int p = 0; p < maxPlayers(); ++p)
			player(p).read(&data);
		CvEventReporter::getInstance().readStatistics(&data);
		if (data.overrun() || data.GetSizeLeft() != 0)
			throw std::runtime_error(path.filename().string() + ": game data does not match this game core (wrong mod or DLL?)");

		// CvGame::read picks the active player of single player games; for hotseat and
		// other multiplayer types the engine does: the human whose turn it is. The saved
		// options of the human at the seat stand in for the user profile (the DLL sends
		// the profile's options to the new active player).
		if (GC.getGame().getActivePlayer() == NO_PLAYER)
		{
			int chosen = NO_PLAYER;
			for (int p = 0; p < maxCivPlayers() && chosen == NO_PLAYER; ++p)
				if (player(p).isHuman() && player(p).isTurnActive())
					chosen = p;
			for (int p = 0; p < maxCivPlayers() && chosen == NO_PLAYER; ++p)
				if (player(p).isHuman())
					chosen = p;
			if (chosen != NO_PLAYER)
			{
				m_utility.setProfileOptionsFromPlayer(static_cast<PlayerTypes>(chosen));
				GC.getGame().setActivePlayer(static_cast<PlayerTypes>(chosen));
			}
		}
		else
			m_utility.setProfileOptionsFromPlayer(GC.getGame().getActivePlayer());

		// Script data back to Python: CvAppInterface.onLoad((pickle,)).
		void* args[] = { PyString_FromStringAndSize(save.pythonData.data(), Py_ssize_t(save.pythonData.size())) };
		m_python.callFunction("CvAppInterface", "onLoad", m_python.MakeFunctionArgs(args, 1));
		m_python.callFunction("CvAppInterface", "preGameStart", nullptr); // after "OnLoad", as the original engine

		m_utility.setGameInitializing(false);
		m_utility.setGameActive(true);
	}

	void GameSession::readInitCore(const SaveFile& save)
	{
		// The original engine reads a save's staging room into the "loaded" CvInitCore (the
		// load screen shows it) as well as the game's own; mods read the rest of their saves
		// according to the loaded one (AND2: the DLL revision that wrote it).
		GC.getLoadedInitCore().init(NO_GAMEMODE); // (first use: allocates its arrays)
		for (CvInitCore* ic : { &GC.getLoadedInitCore(), &GC.getInitCore() })
		{
			MemoryStream init(save.initCore);
			ic->read(&init);
			if (init.overrun() || init.GetSizeLeft() != 0)
				throw std::runtime_error("the save's game setup does not match this game core (wrong mod or DLL?)");
		}
	}

	void GameSession::probeSave(const std::filesystem::path& path)
	{
		const SaveFile save = SaveFile::read(path);
		std::printf("save %s: version %d, mod \"%s\", init core %zu bytes, game data %zu bytes, python %zu bytes\n",
			path.filename().string().c_str(), save.version, save.modFolder.c_str(), save.initCore.size(), save.gameData.size(),
			save.pythonData.size());

		readInitCore(save);
		std::printf("  init core: read all %zu bytes\n", save.initCore.size());

		MemoryStream data(save.gameData);
		const auto step = [&](const char* what, const auto& fn) {
			const unsigned before = data.GetPosition();
			fn();
			std::printf("  %-10s %8u bytes, now at %8u%s\n", what, data.GetPosition() - before, data.GetPosition(),
				data.overrun() ? " OVERRUN" : "");
		};
		step("game", [&] { GC.getGame().read(&data); });
		step("map", [&] { GC.getMap().read(&data); });
		step("teams", [&] {
			for (int t = 0; t < maxTeams(); ++t)
				team(t).read(&data);
		});
		step("players", [&] {
			for (int p = 0; p < maxPlayers(); ++p)
				player(p).read(&data);
		});
		std::printf("  %u bytes left\n", data.GetSizeLeft());
		{
			const unsigned at = data.GetPosition();
			step("statistics", [&] { CvEventReporter::getInstance().readStatistics(&data); });
			std::printf("  %u bytes left\n", data.GetSizeLeft());
			data.SetPosition(at);
		}

		std::printf("  next ints:");
		const unsigned at = data.GetPosition();
		for (int i = 0; i < 24; ++i)
		{
			int v = 0;
			data.Read(&v);
			std::printf(" %d", v);
		}
		std::printf("\n");
		data.SetPosition(at);
	}
}
