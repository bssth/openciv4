// oc4core: the x64 engine for the Civ4 BtS game DLL.
//
//   oc4core [--civ4 <root>] [--mod <Mods\\Name> | <mod folder>] [--python-home <dir>] [--log-dir <dir>]
//           [--turns N [--map Continents] [--size SMALL] [--players N] [--seed N]]
//           [--load <file.CivBeyondSwordSave>] [--save <file.CivBeyondSwordSave> [--save-at TURN]]
//           [--profile] [--profile-out <stacks.folded>] [--rand-log] [--exec <script.py>] [--exec-first <script.py>]
//           [--menu | --view | --play [--screenshot <file.png>] [--view-size WxH] [--ui-scale PERCENT] [--show-fps] [--wasd-pan] [--auto-end-turn] [--no-edge-scroll] [--look X,Y[,distance in plots]] [--keys <window keys>]
//            [--view-seconds S]]
//           [--host PORT [--humans N] | --join HOST:PORT]
//
// --log-dir enables the DLL's logs (xml.log, ...), like LoggingEnabled = 1 in CivilizationIV.ini;
// --rand-log adds every synchronised random number to MessageControl.log there.
//
// --view shows the game's map in a window; the AI plays when Space is pressed (N: one turn),
// --turns N of them right away. With --screenshot it plays --turns first, renders the map
// offscreen into the file and exits. --play lets you play the active player's side: click
// to select, right click to go there; the unit's actions are buttons along the bottom and
// the game's own hotkeys (B found a city, Space skip, Enter next unit / end of the turn,
// Shift+Enter end it anyway, ...); the game's popups ask what to build and research. The
// interface is the game's own (CvMainInterface.py and its city screen: a click on your city
// opens it, Esc closes it), or with --own-interface the window's simple one.
// --keys are typed in on your turns then (letters and Space as keys, '^', '@' and '+' before
// one hold Ctrl, Alt and Shift, '>' ends the turn, '~' is Esc, "(x,y)" right clicks that plot,
// "[x,y]" left clicks it, "%u", "%d", "%l", "%r", "%e" and "%t" type an arrow, Return or Tab into the window, "<x,y>" clicks that pixel of the window (a figure there: its unit), "#n" picks the n-th button of the popup up (OK if it has none), a
// popup nothing answers gets its first, 'text' goes into its next edit box, "{Name}" or
// "{Name/part}" clicks the interface's widget so named, "^{Name}" with Ctrl held, "?{Name}" rests
// the mouse on it); with
// --screenshot and --view-seconds that game is played offscreen. OC4_PLAY_LOG=1 prints the
// popups as they come, the actions the keys run and the widgets clicked; OC4_UI_DUMP=<file>
// writes the interface's widgets there when they change.
//
// --menu (and no options at all) opens the window on a temporary main menu: a new game, with
// the leader, map script, world and difficulty to choose, or a saved one, played as with
// --play then (its leading "{Name}" --keys click the menu's widgets: MenuNew, MenuLaunch, ...;
// "{MenuLeader}{MenuLeader/3}" opens a pull down and picks its 4th item).
//
// Starts Python, publishes the DLL to it, initialises CvGlobals and loads all XML the
// way the original engine does. With --turns it then starts a new game on a map
// script and lets the AI play N turns.

#include "AutoEndTurn.h"
#include "CrashHandler.h"
#include "EngineClasses.h"
#include "GameCoreImports.h"
#include "GameFont.h"
#include "GameSession.h"
#include "Net.h"
#include "MapCapture.h"
#include "Profiler.h"
#include "PythonHost.h"
#include "SaveFile.h"
#include "Screens.h"
#include "TurnWorker.h"
#include "Unimplemented.h"
#include "Utility.h"
#include "Xml.h"

#include "core/AssetResolver.h"
#include "core/FileUtil.h"
#include "render/Input.h"
#include "render/MapWindow.h"

#include "CvGameCoreDLL.h"
#include "CvArtFileMgr.h"
#include "CvGameAI.h"
#include "CvPopupInfo.h"
#include "CvGameTextMgr.h"
#include "CvGlobals.h"
#include "CvInitCore.h"
#include "CvMap.h"
#include "CvSelectionGroup.h"
#include "FInputDevice.h"
#include "CvPlayerAI.h"
#include "CvPlot.h"
#include "CvUnit.h"
#include "CvXMLLoadUtility.h"
#include "CvDLLFAStarIFaceBase.h"
#include "FAStarNode.h"

#include <pybind11/pybind11.h>

#include <windows.h>
#include <fstream>
#include <shlobj.h>
#ifndef _WIN32
#include <spawn.h>
extern char** environ;
#endif

#include <cctype>
#include <chrono>
#include <format>
#include <cstdio>
#include <cwctype>
#include <filesystem>
#include <functional>
#include <optional>
#include <set>
#include <unordered_map>
#include <string>

namespace fs = std::filesystem;
using namespace oc4;

namespace
{
	fs::path documentsFolder();

	// What the program runs as next, when a game leaves for another (the game menu's load,
	// its main menu): these options with the install's and the mod's of this run.
	std::optional<std::vector<std::wstring>> g_relaunch;

	struct Options
	{
		fs::path startDir = fs::current_path(); // (the engine then works in the Beyond the Sword folder)
		fs::path civ4Root;
		std::optional<fs::path> mod;
		fs::path pythonHome = OC4_DEFAULT_PYTHON_HOME;
		fs::path logDir; // DLL logs (xml.log, ...) are written only when set
		int turns = 0; // > 0: start a new game and let the AI play this many turns
		fs::path probeSave; // development: report how a save's parts line up
		fs::path load; // continue a saved game instead of starting a new one
		fs::path save; // save the game when done (or at --save-at, during autoplay)
		int saveAt = -1;
		fs::path exec; // development: run a Python file at the end (after the game, if any)
		fs::path execFirst; // ... or once the game is loaded or started (to set a test up)
		bool randLog = false; // log every synchronised random number to MessageControl.log (needs --log-dir)
		bool profile = false; // sample the autoplay and print where the time goes
		fs::path profileOut;  // also write folded stacks for flame graphs
		bool menu = false;    // the main menu first, then the game chosen there is played
		std::string menuPage; // ... opened on that page ("load")
		std::vector<std::wstring> kept; // the options a relaunch keeps: the install's, the mod's, Python's, the logs', the window's
		bool view = false;    // the map in a window
		bool play = false;    // ... where the active player gives the orders
		bool ownInterface = false; // ... with the host's interface instead of the game's
		fs::path screenshot;  // ... or rendered into a file
		unsigned viewWidth = 1600, viewHeight = 900;
		bool showFps = false; // the frame rate in the corner from the start
		bool wasdPan = false; // W A S D pan the map from the start (whatever the profile says)
		bool autoEndTurn = false; // idle turns end by themselves from the start (the same)
		bool noEdgeScroll = false; // the mouse at the window's edge does not scroll the map (the same)
		int uiScale = -1; // the interface's, in percent, 0 the window's density (-1: the profile's)
		int lookX = -1, lookY = -1;
		float lookDistance = 9;
		double viewSeconds = 0; // development: close the window after this long
		std::string viewKeys;   // window keys pressed at the start (G grid, V fog of war, ...)
		int helpX = -1, helpY = -1; // development: print the plot help of this plot
		host::NewGameSettings game;
		// Multiplayer: host on this port, for this many humans in all; or join a host ("name:port").
		int hostPort = 0;
		int humans = 2;
		std::string join;
	};

	fs::path exeFolder()
	{
		wchar_t path[MAX_PATH]{};
		GetModuleFileNameW(nullptr, path, MAX_PATH);
		return fs::path(path).parent_path();
	}

	// Where Beyond the Sword is installed, when nothing says: the folder the game's installer
	// records (Firaxis' registry key, INSTALLDIR its Beyond the Sword folder), else a Steam
	// library that has it. A Civ4 root holds Assets and "Beyond the Sword".
	fs::path findCiv4()
	{
		const auto isRoot = [](const fs::path& p) {
			std::error_code ec;
			return !p.empty() && fs::is_directory(p / "Assets", ec) && fs::is_directory(p / "Beyond the Sword", ec);
		};
		const auto readValue = [](HKEY root, const wchar_t* key, const wchar_t* value) -> std::wstring {
			wchar_t data[1024]{};
			DWORD size = sizeof(data);
			if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, data, &size) != ERROR_SUCCESS)
				return {};
			return data;
		};
		for (const wchar_t* key : { L"SOFTWARE\\WOW6432Node\\Firaxis Games\\Sid Meier's Civilization 4 - Beyond the Sword",
				 L"SOFTWARE\\Firaxis Games\\Sid Meier's Civilization 4 - Beyond the Sword" })
			if (const std::wstring dir = readValue(HKEY_LOCAL_MACHINE, key, L"INSTALLDIR"); !dir.empty())
			{
				fs::path p = fs::path(dir);
				if (!p.has_filename())
					p = p.parent_path();
				for (const fs::path& candidate : { p, p.parent_path() })
					if (isRoot(candidate))
						return candidate;
			}
		// Steam: its own folder, then the libraries libraryfolders.vdf lists ("path" "D:\\SteamLibrary").
		std::vector<fs::path> libraries;
		if (const std::wstring steam = readValue(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"); !steam.empty())
		{
			libraries.push_back(fs::path(steam));
			std::ifstream vdf(fs::path(steam) / "steamapps" / "libraryfolders.vdf");
			for (std::string line; std::getline(vdf, line);)
				if (const size_t key = line.find("\"path\""); key != std::string::npos)
				{
					const size_t a = line.find('"', key + 6), b = a == std::string::npos ? a : line.rfind('"');
					if (a != std::string::npos && b > a)
					{
						std::string path = line.substr(a + 1, b - a - 1), unescaped;
						for (size_t i = 0; i < path.size(); ++i)
							if (path[i] == '\\' && i + 1 < path.size() && path[i + 1] == '\\')
								unescaped += '\\', ++i;
							else
								unescaped += path[i];
						libraries.push_back(fs::u8path(unescaped));
					}
				}
		}
		for (const fs::path& library : libraries)
			if (const fs::path p = library / "steamapps" / "common" / "Sid Meier's Civilization IV Beyond the Sword"; isRoot(p))
				return p;
		return {};
	}

	Options parseArgs(int argc, wchar_t** argv)
	{
		Options o;
		if (wchar_t env[MAX_PATH]; GetEnvironmentVariableW(L"OC4_CIV4_DIR", env, MAX_PATH) > 0)
			o.civ4Root = env;
		// (a copy shipped with its Python: the standard library in "python" beside it)
		if (std::error_code ec; fs::exists(exeFolder() / "python" / "Lib" / "os.py", ec))
			o.pythonHome = exeFolder() / "python";
		for (int i = 1; i < argc; ++i)
		{
			const std::wstring a = argv[i];
			const auto next = [&]() -> fs::path {
				if (i + 1 >= argc)
					throw std::runtime_error("missing value for an option");
				return argv[++i];
			};
			if (a == L"--civ4" || a == L"--mod" || a == L"--python-home" || a == L"--log-dir" || a == L"--view-size" || a == L"--ui-scale")
				if (i + 1 < argc)
					o.kept.insert(o.kept.end(), { a, argv[i + 1] });
			if (a == L"--civ4")
				o.civ4Root = next();
			else if (a == L"--mod")
				o.mod = next();
			else if (a == L"--python-home")
				o.pythonHome = next();
			else if (a == L"--log-dir")
				o.logDir = next();
			else if (a == L"--menu-page")
				o.menuPage = next().string();
			else if (a == L"--turns")
				o.turns = std::stoi(next().wstring());
			else if (a == L"--map")
				o.game.mapScript = next().string();
			else if (a == L"--size")
				o.game.worldSize = "WORLDSIZE_" + next().string();
			else if (a == L"--players")
				o.game.players = std::stoi(next().wstring());
			else if (a == L"--load")
				o.load = next();
			else if (a == L"--host")
				o.hostPort = std::stoi(next().wstring());
			else if (a == L"--humans")
				o.humans = std::max(2, std::stoi(next().wstring()));
			else if (a == L"--join")
				o.join = next().string();
			else if (a == L"--save")
				o.save = next();
			else if (a == L"--exec-first")
				o.execFirst = next();
			else if (a == L"--exec")
				o.exec = next();
			else if (a == L"--rand-log")
				o.randLog = true;
			else if (a == L"--profile")
				o.profile = true;
			else if (a == L"--profile-out")
			{
				o.profile = true;
				o.profileOut = next();
			}
			else if (a == L"--save-at")
				o.saveAt = std::stoi(next().wstring());
			else if (a == L"--probe-save")
				o.probeSave = next();
			else if (a == L"--civ")
				o.game.civilization = next().string(); // (the human's: CIVILIZATION_..., a scenario's player of it)
			else if (a == L"--seed")
				o.game.seed = static_cast<unsigned>(std::stoul(next().wstring()));
			else if (a == L"--view")
				o.view = true;
			else if (a == L"--menu")
				o.menu = true;
			else if (a == L"--play")
				o.view = o.play = true;
			else if (a == L"--own-interface")
				o.ownInterface = true;
			else if (a == L"--screenshot")
			{
				o.view = true;
				o.screenshot = next();
			}
			else if (a == L"--view-size")
			{
				if (std::swscanf(next().wstring().c_str(), L"%ux%u", &o.viewWidth, &o.viewHeight) != 2)
					throw std::runtime_error("--view-size takes WxH");
			}
			else if (a == L"--show-fps")
				o.showFps = true;
			else if (a == L"--wasd-pan")
				o.wasdPan = true;
			else if (a == L"--auto-end-turn")
				o.autoEndTurn = true;
			else if (a == L"--no-edge-scroll")
				o.noEdgeScroll = true;
			else if (a == L"--ui-scale")
				o.uiScale = std::stoi(next().wstring());
			else if (a == L"--plot-help")
			{
				if (std::swscanf(next().wstring().c_str(), L"%d,%d", &o.helpX, &o.helpY) != 2)
					throw std::runtime_error("--plot-help takes X,Y");
			}
			else if (a == L"--keys")
				o.viewKeys = next().string();
			else if (a == L"--view-seconds")
				o.viewSeconds = std::stod(next().wstring());
			else if (a == L"--look")
			{
				if (std::swscanf(next().wstring().c_str(), L"%d,%d,%f", &o.lookX, &o.lookY, &o.lookDistance) < 2)
					throw std::runtime_error("--look takes X,Y[,distance]");
			}
			else
				throw std::runtime_error("unknown option");
		}
		if (argc <= 1)
			o.menu = true; // (started as a game is: its menu)
#ifndef _WIN32
		if (o.mod)
		{
			std::string native = utf8Path(*o.mod);
			std::replace(native.begin(), native.end(), '\\', '/');
			o.mod = fs::u8path(native);
		}
#endif
		if (o.civ4Root.empty())
			o.civ4Root = findCiv4();
		if (o.civ4Root.empty())
			throw std::runtime_error("Beyond the Sword not found: pass --civ4 <folder with Assets, Warlords, Beyond the Sword> or set OC4_CIV4_DIR");
		// The engine works in the Beyond the Sword folder: resolve command line paths first.
		for (fs::path* p : { &o.civ4Root, &o.pythonHome, &o.logDir, &o.probeSave, &o.load, &o.save, &o.exec, &o.execFirst, &o.profileOut, &o.screenshot })
			if (!p->empty())
				*p = fs::absolute(*p);
		return o;
	}

	bool stage(const char* name, const std::function<bool()>& fn)
	{
		const auto t0 = std::chrono::steady_clock::now();
		const bool ok = fn();
		const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
		std::printf("  %-34s %s %8.1f ms\n", name, ok ? "ok    " : "FAILED", ms);
		return ok;
	}

	// The game's code (FInputDevice::KB_..., as its hotkeys have it) of a Windows key; -1 none.
	unsigned scriptedKey(char c)
	{
		switch (c)
		{
		case 'u': return render::key::Up;
		case 'd': return render::key::Down;
		case 'l': return render::key::Left;
		case 'r': return render::key::Right;
		case 'e': return render::key::Return;
		case 't': return render::key::Tab;
		default: return 0;
		}
	}

	int gameKey(unsigned vk)
	{
		if (vk >= 'A' && vk <= 'Z')
			return FInputDevice::KB_A + int(vk - 'A');
		if (vk >= '0' && vk <= '9')
			return FInputDevice::KB_0 + int(vk - '0');
		if (vk >= render::key::F1 && vk <= render::key::F12)
			return FInputDevice::KB_F1 + int(vk - render::key::F1);
		if (vk >= render::key::Numpad0 && vk <= render::key::Numpad9)
			return FInputDevice::KB_NUMPAD0 + int(vk - render::key::Numpad0);
		switch (vk)
		{
		case render::key::Escape: return FInputDevice::KB_ESCAPE;
		case render::key::Return: return FInputDevice::KB_RETURN;
		case render::key::Space: return FInputDevice::KB_SPACE;
		case render::key::Tab: return FInputDevice::KB_TAB;
		case render::key::Back: return FInputDevice::KB_BACKSPACE;
		case render::key::Minus: return FInputDevice::KB_MINUS;
		case render::key::Plus: return FInputDevice::KB_EQUALS;
		case render::key::Comma: return FInputDevice::KB_COMMA;
		case render::key::Period: return FInputDevice::KB_PERIOD;
		case render::key::Semicolon: return FInputDevice::KB_SEMICOLON;
		case render::key::Slash: return FInputDevice::KB_SLASH;
		case render::key::Grave: return FInputDevice::KB_GRAVE;
		case render::key::LBracket: return FInputDevice::KB_LBRACKET;
		case render::key::Backslash: return FInputDevice::KB_BACKSLASH;
		case render::key::RBracket: return FInputDevice::KB_RBRACKET;
		case render::key::Apostrophe: return FInputDevice::KB_APOSTROPHE;
		case render::key::Insert: return FInputDevice::KB_INSERT;
		case render::key::Delete: return FInputDevice::KB_DELETE;
		case render::key::End: return FInputDevice::KB_END;
		case render::key::Prior: return FInputDevice::KB_PGUP;
		case render::key::Next: return FInputDevice::KB_PGDN;
		case render::key::Up: return FInputDevice::KB_UP;
		case render::key::Down: return FInputDevice::KB_DOWN;
		case render::key::Left: return FInputDevice::KB_LEFT;
		case render::key::Right: return FInputDevice::KB_RIGHT;
		default: return -1;
		}
	}

	// Of the game's controls, those the host can do: all but the multiplayer game's (chat,
	// pings, its administration) and the map's signs.
	bool playableControl(int control)
	{
		switch (control)
		{
		case CONTROL_PING:
		case CONTROL_SIGN:
		case CONTROL_CHAT_ALL:
		case CONTROL_CHAT_TEAM:
		case CONTROL_ADMIN_DETAILS:
		case CONTROL_DIPLOMACY:
		case CONTROL_ORTHO_CAMERA:
		case CONTROL_CYCLE_CAMERA_FLYING_MODES:
		case CONTROL_FLYING_CAMERA:
		case CONTROL_MOUSE_FLYING_CAMERA:
		case CONTROL_TOP_DOWN_CAMERA:
			return false;
		default:
			return true;
		}
	}

	// (the play log: the map as the engine sees it, a mod's viewport's coordinates, and what the
	// active team knows of it)
	void logMapKnown(const char* when)
	{
		CvMap& map = GC.getMap();
		const TeamTypes team = GC.getGame().getActiveTeam();
		int revealed = 0, placed = 0;
		for (int i = 0; i < map.numPlots(); ++i)
			if (CvPlot* plot = map.plotByIndex(i))
			{
				revealed += team != NO_TEAM && plot->isRevealed(team, false);
				placed += plot->getX() >= 0 && plot->getX() < map.getGridWidth() && plot->getY() >= 0 && plot->getY() < map.getGridHeight();
			}
		std::printf("  the map %s: %d plots of %dx%d, %d with their place on it, %d revealed to team %d\n", when, map.numPlots(), map.getGridWidth(),
			map.getGridHeight(), placed, revealed, int(team));
	}

	// The way the selected units would take to a plot of the window, as the original engine
	// draws it while the right button is held: the game core's interface pathfinder (the turn
	// each plot is reached in is its nodes' m_iData2). Its plots are the window's: a mod's
	// viewport callbacks look them up in GC.getMapExternal().
	render::MapWindow::PathView findPath(host::Interface& ui, std::pair<int, int> target)
	{
		render::MapWindow::PathView way;
		CvUnit* head = ui.getHeadSelectedUnit();
		CvSelectionGroup* group = ui.getSelectionList();
		const CvPlot* from = head ? head->plot() : nullptr;
		if (!from || !group || head->getOwner() != GC.getGame().getActivePlayer())
			return way;
		const int startX = from->getX(), startY = from->getY();
		const auto [destX, destY] = target;
		if (destX < 0 || (startX == destX && startY == destY))
			return way;
		FAStar* finder = &GC.getInterfacePathFinder();
		gDLL->getFAStarIFace()->SetData(finder, group);
		const bool found = head->getDomainType() != DOMAIN_AIR &&
			gDLL->getFAStarIFace()->GeneratePath(finder, startX, startY, destX, destY, false, MOVE_DECLARE_WAR, true);
		if (!found)
		{
			// (no way there: a red line straight to it)
			way.reachable = false;
			way.points = { { from->getX(), from->getY(), 1 }, { target.first, target.second, 1 } };
			return way;
		}
		for (const FAStarNode* n = gDLL->getFAStarIFace()->GetLastNode(finder); n; n = n->m_pParent)
			way.points.push_back({ n->m_iX, n->m_iY, n->m_iData2 });
		std::ranges::reverse(way.points);
		return way;
	}

	// Playing the active player's side: the window's clicks and keys become the orders the
	// original's interface gives: units selected and moved through the DLL's selection list,
	// the game's actions (CvActionInfo: missions, commands, builds, promotions, controls) from
	// their buttons and hotkeys through CvGame::handleAction, a plot for the interface mode one
	// asks for; the AI plays its turns in between.
	void runPlay(const Options& opt, render::MapWindow& window, host::MapCapture& capture, host::GameSession& session, host::Utility& utility)
	{
		using Clock = std::chrono::steady_clock;
		host::Interface& ui = utility.gameInterface();
		CvGame& game = GC.getGame();
		window.setPlaying(true);
		ui.minimapTerrainColors = window.terrainColors(); // (for the replays the saves keep; before the game's thread starts)
		// Sound in a game played by hand (OC4_SOUND: 1 in a scripted one too, mute to run it
		// unheard, 0 none); what plays is around the camera, the music the active player's era's.
		host::Sounds& sounds = utility.sounds();
		if (const char* s = std::getenv("OC4_SOUND"); s ? std::string_view(s) != "0" : opt.viewSeconds <= 0)
			sounds.start(s && std::string_view(s) == "mute");
		window.setSoundHandler([&sounds](const std::string& tag, float x, float y) { sounds.play(tag, host::Sounds::Place{ x, y }); });
		window.setMovieSound([&sounds](std::shared_ptr<const audio::Pcm> pcm) { return sounds.playMovie(std::move(pcm)); },
			[&sounds](uint64_t voice) { sounds.stopMovie(voice); });
		std::string era;
		// (before each frame: what is heard there, and the game's messages to the player)
		const auto hear = [&] {
			const auto look = window.look();
			sounds.update({ look.x, look.y, look.distance }, era);
			std::vector<std::pair<std::string, float>> lines;
			// (none over a talk: the leader's face is where they are; the turn log keeps them)
			if (!utility.diplomacy().active())
				for (const auto& m : ui.shownMessages())
					lines.push_back({ host::gameMarkup(m.text.c_str()), m.alpha });
			window.setMessages(std::move(lines));
			// (and what the game's controls show of the map)
			window.setLayers({ ui.showYields, utility.engine().resourceLayer, utility.engine().unitLayer, ui.bareMap, utility.engine().grid });
			const auto option = [&](const char* type) {
				const int i = GC.getInfoTypeForString(type, true);
				return i >= 0 && utility.getGraphicOption(static_cast<GraphicOptionTypes>(i));
			};
			window.setGraphics({ option("GRAPHICOPTION_HEALTH_BARS"), option("GRAPHICOPTION_FROZEN_ANIMATIONS"), !option("GRAPHICOPTION_EFFECTS_DISABLED"),
				utility.graphicOptionChosen(GC.getInfoTypeForString("GRAPHICOPTION_FULLSCREEN", true)) });
		};
		{
			std::array<std::string, 3> symbols;
			const wchar_t* const tags[3] = { L"ICON_FOOD", L"ICON_PRODUCTION", L"ICON_COMMERCE" };
			for (size_t i = 0; i < 3; ++i)
				if (std::wstring sym; host::resolveTextTag(tags[i], sym))
					symbols[i] = host::gameMarkup(sym.c_str());
			window.setYieldSymbols(symbols);
		}
		const bool playLog = std::getenv("OC4_PLAY_LOG") != nullptr;
		int loggedUnit = -1; // (the play log names each unit selected)
		ui.setCycleSelectionCounter(1); // (a loaded game too starts with a unit waiting for orders)
		if (std::getenv("OC4_PLAY_LOG"))
			logMapKnown("at the start");
		// The game's own interface (CvMainInterface.py and the screens), unless asked not to.
		host::Screens screens(utility);
		// The soundscape where the camera looks: a city's by it (its era's, its size's), else the
		// plot's feature's, improvement's or terrain's; none unexplored or from far up.
		const host::SoundscapePlaces scapes = sounds.started() ? host::soundscapePlaces(utility) : host::SoundscapePlaces{};
		if (playLog && sounds.started())
		{
			const auto count = [](const std::vector<int>& v) { return std::count_if(v.begin(), v.end(), [](int s) { return s >= 0; }); };
			std::printf("  soundscapes: %zd of %zu terrains, %zd of %zu features, %zd of %zu improvements, the first era's cities %d %d %d\n",
				count(scapes.terrain), scapes.terrain.size(), count(scapes.feature), scapes.feature.size(), count(scapes.improvement),
				scapes.improvement.size(), scapes.city.empty() ? -1 : scapes.city[0][0], scapes.city.empty() ? -1 : scapes.city[0][1],
				scapes.city.empty() ? -1 : scapes.city[0][2]);
		}
		const auto soundscapeHere = [&]() -> int {
			const auto look = window.look();
			CvMap& map = GC.getMap();
			const TeamTypes team = game.getActiveTeam();
			const CvPlot* plot = map.plot(int(std::floor(look.x)), int(std::floor(look.y)));
			if (look.distance > 25 || !plot || team == NO_TEAM || !plot->isRevealed(team, false))
				return -1;
			const auto of = [](const std::vector<int>& table, int i) { return i >= 0 && size_t(i) < table.size() ? table[size_t(i)] : -1; };
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					if (const CvPlot* p = map.plot(plot->getX() + dx, plot->getY() + dy); p && p->isRevealed(team, false))
						if (const CvCity* city = p->getPlotCity())
						{
							const EraTypes era = CvPlayerAI::getPlayerNonInl(city->getOwner()).getCurrentEra();
							if (era >= 0 && size_t(era) < scapes.city.size())
								if (const int s = of(scapes.city[size_t(era)], int(city->getCitySizeType())); s >= 0)
									return s;
						}
			if (const int s = of(scapes.feature, plot->getFeatureType()); s >= 0)
				return s;
			if (const int s = of(scapes.improvement, plot->getImprovementType()); s >= 0)
				return s;
			return of(scapes.terrain, plot->getTerrainType());
		};
		host::Screens::setTextWidth([&window](const std::string& markup, int font) { return window.textWidth(markup, font); });
		host::Screens::setTextHeight([&window](const std::string& markup, int width, int font) { return window.textHeight(markup, width, font); });
		window.setUiWheel([&screens](int x, int y, int delta) { return screens.wheel(x, y, delta); });
		struct ForgetWheel
		{
			render::MapWindow& window;
			~ForgetWheel() { window.setUiWheel({}); } // (the screens go first)
		} forgetWheel{ window };
		if (opt.uiScale >= 0)
			utility.forceUiScale(opt.uiScale);
		if (opt.showFps)
			window.setFrameRateShown(true);
		if (opt.wasdPan)
			utility.forceWasdPan();
		window.setWasdPan(utility.wasdPan());
		if (opt.autoEndTurn)
			utility.forceAutoEndTurn();
		window.setAutoEndTurn(utility.autoEndTurn());
		if (opt.noEdgeScroll || opt.viewSeconds > 0) // (scripted runs: never)
			utility.forceNoEdgeScroll();
		window.setEdgeScroll(utility.edgeScroll());
		window.setUiScale(utility.uiScale());
		if (!opt.ownInterface && !screens.start(int(window.width()), int(window.height())))
			std::printf("  the game's interface did not start: the host's own\n");
		int preparedTurn = -1;
		int askedResearch = -1; // the cities the player had when research was last asked for (no city, no research)
		uint64_t shownPopup = 0;
		render::MapWindow::PopupView popupView;
		std::vector<int> popupItems; // the popup's item for each one the window shows
		window.setPopupHelp([&](int i) {
			const host::Interface::Popup* p = ui.shownPopup();
			return p && i >= 0 && size_t(i) < popupItems.size() ? capture.popupHelp(*p, popupItems[size_t(i)]) : std::string();
		});
		auto last = Clock::now(), captured = last, ticked = last, typed = last;
		host::AutoEndTurn autoEndTurn; // (the option: idle turns end by themselves)
		const auto start = last;
		long long frames = 0;
		double longestFrame = 0; // seconds between two frames, the longest (a window that stood still)
		auto frameStart = last;
		// --keys are typed in while it is our turn, a few a second ('>' is Enter, '~' Esc, '^@+'
		// Ctrl, Alt, Shift with the next key, "(x,y)" a right click on that plot, "[x,y]" a left
		// click, "#n" the n-th button of the popup up, 'text' for its next edit box, "{Name}" a
		// click on the screens' widget so named, "{Name/part}" on its part): a scripted game.
		std::string script = opt.viewKeys;
		int waited = 0;
		std::pair<int, int> cityScreen{ -1, -1 }; // the city whose screen is up
		long long cityFrames = 0; // frames when the city screen last opened or closed
		auto cityToggled = start;
		std::vector<std::wstring> scriptEdits; // 'text' in the script: for the next popup's edit boxes

		std::vector<int> actionIds; // the action of each button the window shows
		std::optional<std::pair<int, int>> pathShown; // where the way the window shows leads
		std::pair<const void*, int> pathFrom{};        // (the unit it starts from and its plot: the gamepad's plot cursor shows it all the while)
		std::tuple<const void*, const void*, int, int> coloredFor{}; // what was selected when the plots were last coloured
		uint64_t marksShown = ~0ull; // the marks the window has
		uint64_t globeShown = 0;      // ... and the globe's state it shows (engine.globeVersion)
		int globeTurn = -1;
		std::vector<render::MapWindow::PlotMarks::Mark> globeMarks;
		uint64_t greatWallShown = 0; // the Great Wall the capture has (its cities)
		int shownCursor = -2; // the game's cursor the window has
		window.setActionHelp([&](int i) { return i >= 0 && size_t(i) < actionIds.size() ? capture.widgetHelp(WIDGET_ACTION, actionIds[size_t(i)], -1) : std::string(); });
		const auto held = render::keyHeld;
		const bool log = std::getenv("OC4_PLAY_LOG") != nullptr;
		const auto usable = [&](int action) {
			const int control = GC.getActionInfo(action).getControlType();
			return (control == NO_CONTROL || playableControl(control)) && game.canHandleAction(action);
		};
		// An action as the original's interface runs it, with the keys held (Shift queues a
		// mission, Alt gives it to the whole stack).
		const auto act = [&](int action, bool shift, bool ctrl, bool alt) {
			utility.setModifierKeys(shift, ctrl, alt);
			if (usable(action))
				game.handleAction(action);
			utility.setModifierKeys(false, false, false);
		};
		// A key: the action it is the hotkey of that can be done now (the highest priority of
		// them); Esc leaves the interface mode, the city screen or the screen on top.
		const auto press = [&](const render::MapWindow::KeyPress& k) {
			if (screens.diploKey(k.key))
				return;
			if (k.key == render::key::Escape)
			{
				if (ui.getInterfaceMode() != INTERFACEMODE_SELECTION)
					ui.setInterfaceMode(INTERFACEMODE_SELECTION);
				else if (screens.escape())
					;
				else if (ui.isCitySelection())
					ui.clearSelectedCities();
				// Nothing else to leave: the game's menu (the DLL's BUTTONPOPUP_MAIN_MENU), as the
				// original's Esc.
				else if (!ui.shownPopup() && !ui.isDiploOrPopupWaiting())
					ui.addPopup(new CvPopupInfo(BUTTONPOPUP_MAIN_MENU), game.getActivePlayer(), true, false);
				return;
			}
			const int code = gameKey(k.key);
			// The scripts see it first (BUG's shortcuts: Alt+M a reminder, ...); one they take goes no further.
			if (code >= 0)
			{
				const auto [mx, my] = window.mousePosition();
				const auto [plotX, plotY] = window.hoveredPlot();
				if (screens.keyEvent(code, k.shift, k.ctrl, k.alt, std::max(mx, 0), std::max(my, 0), plotX, plotY))
				{
					if (log)
						std::printf("  key %u: taken by the scripts\n", k.key);
					return;
				}
			}
			// The numpad's digits move the selected units a plot that way, as the original's do (its
			// engine's own keys, no action's hotkey; the gamepad's stick flicked sends them, PadMap.h).
			if (!k.shift && !k.ctrl && !k.alt && k.key > render::key::Numpad0 && k.key <= render::key::Numpad9 && k.key != render::key::Numpad0 + 5)
				if (CvUnit* unit = ui.getHeadSelectedUnit())
				{
					const int d = int(k.key - render::key::Numpad0) - 1;
					if (CvPlot* to = plotXY(unit->getX(), unit->getY(), d % 3 - 1, d / 3 - 1))
					{
						ui.unitSound(unit, true);
						game.selectionListMove(to, false, false, false);
					}
					return;
				}
			int best = -1, bestPriority = 0;
			utility.setModifierKeys(k.shift, k.ctrl, k.alt);
			for (int i = 0; code >= 0 && i < GC.getNumActionInfos(); ++i)
			{
				const CvActionInfo& a = GC.getActionInfo(i);
				const bool first = a.getHotKeyVal() == code && a.isShiftDown() == k.shift && a.isCtrlDown() == k.ctrl && a.isAltDown() == k.alt;
				const bool second = a.getHotKeyValAlt() == code && a.isShiftDownAlt() == k.shift && a.isCtrlDownAlt() == k.ctrl && a.isAltDownAlt() == k.alt;
				const int priority = first ? a.getHotKeyPriority() : a.getHotKeyPriorityAlt();
				if ((first || second) && (best < 0 || priority > bestPriority) && usable(i))
				{
					best = i;
					bestPriority = priority;
				}
			}
			utility.setModifierKeys(false, false, false);
			if (log)
				std::printf("  key %u%s%s%s: action %d %s\n", k.key, k.shift ? "+shift" : "", k.ctrl ? "+ctrl" : "", k.alt ? "+alt" : "", best,
					best >= 0 ? host::gameText(GC.getActionInfo(best).getDescription()).c_str() : "(none)");
			if (best >= 0)
				act(best, k.shift, k.ctrl, k.alt);
		};
		// The buttons: what the selected units can do (shown when the game would show it, dimmed
		// when it cannot be done now), in the game's order.
		const auto updateActions = [&](CvPlayerAI& player) {
			std::vector<std::pair<int, int>> order; // (order priority, action)
			const CvUnit* head = ui.getHeadSelectedUnit();
			if (player.isTurnActive() && !ui.shownPopup() && ui.getInterfaceMode() == INTERFACEMODE_SELECTION && head && head->getOwner() == player.getID())
				for (int i = 0; i < GC.getNumActionInfos(); ++i)
				{
					const CvActionInfo& a = GC.getActionInfo(i);
					if (a.isVisible() && a.getControlType() == NO_CONTROL && game.canHandleAction(i, nullptr, true))
						order.push_back({ a.getOrderPriority(), i });
				}
			std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
			std::vector<render::MapWindow::ActionButton> buttons;
			actionIds.clear();
			for (const auto& [priority, i] : order)
			{
				actionIds.push_back(i);
				const char* art = GC.getActionInfo(i).getButton();
				buttons.push_back({ art ? art : "", game.canHandleAction(i) });
			}
			window.setActions(buttons);
		};
		// The others' turns run on a thread of their own; meanwhile the window draws the map as
		// it was and stays live (the camera moves, the figures move), and nothing here touches
		// the game.
		host::TurnWorker worker;
		// A game played by hand is saved at each turn's start (Documents\My Games\openciv4\Saves\
		// auto, ten in turn), as the original does; a scripted one is not.
		if (opt.viewSeconds <= 0)
			utility.engine().onAutoSave = [&session, &game](bool) {
				PWSTR documents = nullptr;
				if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
					return;
				const fs::path folder = fs::path(documents) / L"My Games" / L"openciv4" / L"Saves" / L"auto";
				CoTaskMemFree(documents);
				// (each game its own: the active civilization and the game's map seed in the name;
				// every 25th turn kept for good)
				std::wstring civ;
				if (const PlayerTypes active = game.getActivePlayer(); active != NO_PLAYER)
					for (const wchar_t c : std::wstring(GC.getInitCore().getCivShortDesc(active).GetCString()))
						civ += std::iswalnum(c) ? c : L'_';
				const std::wstring stem = std::format(L"AutoSave_{}_{:08X}", civ, GC.getInitCore().getMapRandSeed());
				const fs::path path = folder / std::format(L"{}_{}.CivBeyondSwordSave", stem, game.getGameTurn() % 10);
				try
				{
					fs::create_directories(folder);
					session.saveGame(path);
					if (game.getGameTurn() % 25 == 0)
						fs::copy_file(path, folder / std::format(L"{}_turn{}.CivBeyondSwordSave", stem, game.getGameTurn()), fs::copy_options::overwrite_existing);
					std::printf("  autosaved turn %d: %s\n", game.getGameTurn(), path.string().c_str());
				}
				catch (const std::exception& e)
				{
					std::printf("  autosave failed: %s\n", e.what());
				}
			};
		struct ForgetAutoSave
		{
			host::Utility& utility;
			~ForgetAutoSave() { utility.engine().onAutoSave = {}; }
		} forgetAutoSave{ utility };
		// The game menu's and the controls' saves (Ctrl+S, Shift+F5) into the original's folders,
		// said in a message; loading another game, the main menu and the way out leave this
		// game: the program starts again for what comes next (Shift+F8: the quick save).
		bool leave = false;
		const fs::path saves = documentsFolder() / L"My Games" / L"Beyond the Sword" / L"Saves" / L"single";
		const auto saved = [&](const fs::path& path) {
			try
			{
				fs::create_directories(path.parent_path());
				session.saveGame(path);
				ui.addMessage(game.getActivePlayer(), true, -1, CvWString((L"Game saved: " + path.stem().wstring()).c_str()), "AS2D_GAMESAVED",
					MESSAGE_TYPE_DISPLAY_ONLY, nullptr, NO_COLOR, -1, -1, false, false);
				std::printf("  saved %s\n", utf8Path(path).c_str());
			}
			catch (const std::exception& e)
			{
				std::printf("  saving failed: %s\n", e.what());
			}
		};
		utility.session.save = [&](SaveGameTypes) {
			// (named as the original names one: the leader and the date)
			const CvPlayerAI& p = CvPlayerAI::getPlayerNonInl(game.getActivePlayer());
			CvWString date; // ("BC-4000", the game's date as saves name it)
			CvGameTextMgr::GetInstance().setYearStr(date, game.getGameTurn(), true, game.getCalendar(), GC.getDefineINT("START_YEAR"), game.getGameSpeedType());
			std::string name = host::gameText(p.getName()) + " " + host::gameText(date.c_str());
			for (char& c : name)
				if (c && std::strchr("\\/:*?\"<>|", c))
					c = '_';
			saved(saves / fs::path(std::u8string(name.begin(), name.end()) + u8".CivBeyondSwordSave"));
		};
		const fs::path quick = saves / L"quick" / L"QuickSave.CivBeyondSwordSave";
		utility.session.quickSave = [&] { saved(quick); };
		utility.session.quickLoad = [&] {
			if (std::error_code ec; fs::exists(quick, ec))
				g_relaunch = std::vector<std::wstring>{ L"--play", L"--load", quick.wstring() }, leave = true;
		};
		utility.session.load = [&] { g_relaunch = std::vector<std::wstring>{ L"--menu", L"--menu-page", L"load" }, leave = true; };
		utility.session.quit = [&] { leave = true; };
		ui.onExitToMainMenu = [&](const std::string& load) {
			g_relaunch = load.empty() ? std::vector<std::wstring>{ L"--menu" } : std::vector<std::wstring>{ L"--play", L"--load", fs::path(load).wstring() };
			leave = true;
		};
		struct ForgetSession
		{
			host::Utility& utility;
			~ForgetSession()
			{
				utility.session = {};
				utility.gameInterface().onExitToMainMenu = {};
			}
		} forgetSession{ utility };
		// (development: OC4_SHOT_EVERY=s with --screenshot saves a frame each s seconds beside it, <name>_<n>.png)
		const double shotEvery = std::getenv("OC4_SHOT_EVERY") && !opt.screenshot.empty() ? std::atof(std::getenv("OC4_SHOT_EVERY")) : 0;
		int shots = 0;
		while (!leave && window.pump() && (opt.viewSeconds <= 0 || std::chrono::duration<double>(Clock::now() - start).count() < opt.viewSeconds))
		{
			++frames;
			if (shotEvery > 0 && std::chrono::duration<double>(Clock::now() - start).count() >= shotEvery * (shots + 1))
			{
				fs::path shot = opt.screenshot;
				shot.replace_filename(opt.screenshot.stem().wstring() + L"_" + std::to_wstring(++shots) + L".png");
				window.saveScreenshot(shot);
			}
			{
				const auto now = Clock::now();
				longestFrame = std::max(longestFrame, std::chrono::duration<double>(now - frameStart).count());
				frameStart = now;
			}
			if (worker.running())
			{
				if (!worker.finish())
				{
					const auto now = Clock::now();
					hear();
					window.frame(std::min(std::chrono::duration<float>(now - last).count(), 0.1f));
					last = now;
					continue;
				}
				// (done: what they did, shown now)
				window.setBusy(false);
				window.askHelp();
				captured = {};
			}
			// Multiplayer: a player whose link broke is an AI player from the next frame on; a lost
			// host leaves this game to itself, the other humans played by the AI.
			if (host::Net& net = host::Net::get(); net.active())
			{
				if (net.isHost())
					for (const int seat : net.takeLostSeats())
						utility.sendMakeAI(static_cast<PlayerTypes>(seat));
				else if (net.status().ends_with("the host has left"))
				{
					net.stop();
					for (int p = 0; p < GC.getMAX_CIV_PLAYERS(); ++p)
						if (p != game.getActivePlayer() && GC.getInitCore().getSlotStatus(static_cast<PlayerTypes>(p)) == SS_TAKEN)
						{
							GC.getInitCore().setSlotStatus(static_cast<PlayerTypes>(p), SS_COMPUTER);
							CvPlayerAI::getPlayerNonInl(static_cast<PlayerTypes>(p)).updateHuman();
						}
					ui.addMessage(game.getActivePlayer(), true, 8, L"The host has left: the other players are played by the AI now.", nullptr,
						MESSAGE_TYPE_INFO, nullptr, NO_COLOR, -1, -1, false, false);
				}
			}
			bool changed = false;
			const PlayerTypes me = game.getActivePlayer();
			CvPlayerAI& player = CvPlayerAI::getPlayerNonInl(me);
			if (me != NO_PLAYER && player.getCurrentEra() != NO_ERA)
				era = GC.getEraInfo(player.getCurrentEra()).getType();
			if (player.isTurnActive())
			{
				if (preparedTurn != game.getGameTurn())
				{
					preparedTurn = game.getGameTurn();
					askedResearch = -1;
					std::printf("  your turn %d: %d cities, %d units\n", game.getGameTurn(), player.getNumCities(), player.getNumUnits());
					// (for scripted games: which of our units have an enemy next to them)
					if (playLog)
					{
						int loop = 0;
						for (CvUnit* u = player.firstUnit(&loop); u; u = player.nextUnit(&loop))
							for (int dy = -1; dy <= 1; ++dy)
								for (int dx = -1; dx <= 1; ++dx)
									if (const CvPlot* p = GC.getMap().plot(u->getX() + dx, u->getY() + dy); (dx || dy) && p && p->isVisibleEnemyUnit(me))
										std::printf("  unit %d at %d,%d has an enemy at %d,%d\n", u->getID(), u->getX(), u->getY(), p->getX(), p->getY());
					}
					changed = true;
				}
				// As the original's interface does a few times a second: the popups (the DLL's
				// questions, and what to research when nothing is: once a turn, and again after a new
				// city); once the units that moved are done, the next one that waits for orders (the
				// DLL holds it back with the cycle counter); then whether the turn is over (its "press
				// Enter", or ending it).
				if (const auto now = Clock::now(); now - ticked > std::chrono::milliseconds(150))
				{
					ticked = now;
					if (!ui.isDiploOrPopupWaiting() && player.getCurrentResearch() == NO_TECH &&
						std::exchange(askedResearch, player.getNumCities()) != player.getNumCities())
						player.chooseTech();
					ui.updatePopups(me);
					if (ui.getCycleSelectionCounter() > 0)
					{
						ui.changeCycleSelectionCounter(-1);
						if (ui.getCycleSelectionCounter() == 0)
							game.updateSelectionList();
					}
					if (!ui.isDiploOrPopupWaiting())
						game.updateTestEndTurn();
					// Ending idle turns (the option): the "press Enter" lasting, nothing up over the map.
					const host::AutoEndTurnView idle{
						.on = utility.autoEndTurn(),
						.turnActive = player.isTurnActive(),
						.endTurnMessage = ui.isEndTurnMessage(),
						.blocked = ui.isDiploOrPopupWaiting() || ui.shownPopup() || ui.isCitySelection() || screens.menuUp() || game.isHotSeat() || game.isPbem(),
						.turn = game.getGameTurn(),
						.inputs = window.inputCount(),
					};
					if (autoEndTurn.update(idle, now))
					{
						if (log)
							std::printf("  turn %d: idle, ended by itself\n", game.getGameTurn());
						// As Enter does it, through the action: CvGame::doControl is not exported.
						for (int i = 0; i < GC.getNumActionInfos(); ++i)
							if (GC.getActionInfo(i).getControlType() == CONTROL_ENDTURN)
							{
								act(i, false, false, false);
								break;
							}
					}
					changed = true;
				}
			}
			// The game over (won, lost: the player's turns are no more): its popups all the same,
			// the defeat or victory, the replay, one more turn or the main menu.
			else if (game.getGameState() == GAMESTATE_OVER || (me != NO_PLAYER && !player.isAlive()))
			{
				if (const auto now = Clock::now(); now - ticked > std::chrono::milliseconds(150))
				{
					ticked = now;
					ui.updatePopups(me);
					changed = true;
				}
			}
			// The game's cursor: the interface mode's (go to, air strike, ...), else its pointer.
			{
				int cursor = GC.getInfoTypeForString("CURSOR_DEFAULT");
				if (const InterfaceModeTypes mode = ui.getInterfaceMode(); mode != INTERFACEMODE_SELECTION && mode != NO_INTERFACEMODE)
					if (const int c = GC.getInterfaceModeInfo(mode).getCursorIndex(); c >= 0 && c < GC.getNumCursorInfos())
						cursor = c;
				if (cursor != shownCursor)
				{
					shownCursor = cursor;
					fs::path file;
					if (cursor >= 0 && cursor < GC.getNumCursorInfos())
						if (const char* path = GC.getCursorInfo(CursorTypes(cursor)).getPath(); path && *path)
							if (const auto loc = utility.assets().locate(path); loc && !loc->file.empty())
								file = loc->file;
					window.setCursorFile(file);
					if (log)
						std::printf("  cursor %d: %s\n", cursor, file.string().c_str());
				}
			}
			// The popup up and the player's answer to it.
			const host::Interface::Popup* shown = ui.shownPopup();
			if ((shown ? shown->serial : 0) != shownPopup)
			{
				shownPopup = shown ? shown->serial : 0;
				if (shown)
				{
					popupView = capture.popupView(*shown, popupItems);
					window.setPopup(&popupView);
				}
				else
					window.setPopup(nullptr);
			}
			if (auto answer = window.takePopupAnswer(); answer && shown)
			{
				const auto item = [&](int i) { return i >= 0 && size_t(i) < popupItems.size() ? popupItems[size_t(i)] : -1; };
				if (answer->dismissed)
					ui.dismissPopup();
				else
				{
					std::vector<int> radio, checked;
					for (const int i : answer->radio)
						radio.push_back(item(i));
					for (const int i : answer->checked)
						checked.push_back(item(i));
					std::vector<std::pair<int, std::wstring>> edits;
					for (const auto& [i, text] : answer->edits)
					{
						std::wstring w(text.size(), L'\0');
						w.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), w.data(), int(w.size()))));
						edits.push_back({ item(i), std::move(w) });
					}
					ui.answerPopup(item(answer->item), radio, checked, edits);
				}
				changed = true;
			}
			auto clicks = window.takeClicks();
			// (a scripted game answers what it does not expect with the popup's first button)
			if (!script.empty() && script.front() != '#' && script.front() != '\'' && script.front() != '%' && ui.shownPopup() && Clock::now() - typed > std::chrono::milliseconds(1500))
				script.insert(0, "#0");
			if (const auto now = Clock::now(); player.isTurnActive() && !script.empty() && now - typed > std::chrono::milliseconds(400) &&
				(script.front() == '#' || script.front() == '\'' || script.front() == '%' || !ui.shownPopup())) // (the rest waits for the popup's answer)
			{
				typed = now;
				render::MapWindow::PlotClick click;
				int button = -1;
				if (script.front() == '%' && script.size() > 1)
				{
					// "%u", "%d", "%l", "%r", "%e", "%t": an arrow, Return or Tab typed into the window, as a player's.
					if (const unsigned k = scriptedKey(script[1]))
						window.pressKey(k);
					script.erase(0, 2);
				}
				else if (script.front() == '\'')
				{
					// 'text': what the popup's next edit box gets when it is answered.
					const size_t end = script.find('\'', 1);
					const std::string text = script.substr(1, end == std::string::npos ? std::string::npos : end - 1);
					std::wstring w(text.size(), L'\0');
					w.resize(size_t(MultiByteToWideChar(CP_UTF8, 0, text.data(), int(text.size()), w.data(), int(w.size()))));
					scriptEdits.push_back(std::move(w));
					script.erase(0, end == std::string::npos ? script.size() : end + 1);
				}
				else if (script.front() == '#' && std::sscanf(script.c_str() + 1, "%d", &button) == 1)
				{
					// Answers the popup up (when there is one; else it waits, a few seconds at most):
					// its n-th button, or OK when it has no such button.
					const host::Interface::Popup* p = ui.shownPopup();
					if (p && shownPopup == p->serial)
					{
						std::vector<std::pair<int, std::wstring>> edits;
						for (size_t j = 0, k = 0; j < p->items.size() && k < scriptEdits.size(); ++j)
							if (p->items[j].kind == host::Interface::Popup::Item::Kind::Edit)
								edits.push_back({ int(j), scriptEdits[k++] });
						int chosen = -2;
						for (size_t i = 0; i < popupView.items.size(); ++i)
							if (popupView.items[i].kind == render::MapWindow::PopupView::Item::Kind::Button && button-- == 0)
							{
								std::printf("  popup \"%s\": %s\n", popupView.header.c_str(), popupView.items[i].text.c_str());
								chosen = popupItems[i];
								break;
							}
						if (chosen == -2 && p->okButton)
						{
							std::printf("  popup \"%s\": OK\n", popupView.header.c_str());
							chosen = -1;
						}
						if (chosen != -2)
						{
							ui.answerPopup(chosen, {}, {}, edits);
							changed = true;
						}
						scriptEdits.clear();
					}
					if (p || ++waited >= 8)
					{
						waited = 0;
						script.erase(0, std::min(script.find_first_not_of("0123456789", 1), script.size()));
					}
				}
				else if (const size_t close = script.find_first_of(")]"); (script.front() == '(' || script.front() == '[') && close != std::string::npos &&
					std::sscanf(script.c_str() + 1, "%d,%d", &click.x, &click.y) == 2)
				{
					click.right = script.front() == '(';
					clicks.push_back(click);
					script.erase(0, close + 1);
				}
				else if (int px = 0, py = 0; script.front() == '<' && script.find('>') != std::string::npos && std::sscanf(script.c_str() + 1, "%d,%d", &px, &py) == 2)
				{
					// "<x,y>": a left click at that pixel of the window, on what is there (a popup, the
					// screens, a unit's figure, a plot), as the mouse's.
					if (log)
						if (const auto c = window.clickAt(px, py, false))
							std::printf("  click at %d,%d: plot %d,%d%s\n", px, py, c->x, c->y, c->onFigure ? " (a unit's figure)" : "");
					window.mouseClick(px, py);
					script.erase(0, script.find('>') + 1);
				}
				else if (const size_t mods = script.find_first_not_of("^@+"), end = script.find('}');
						 mods != std::string::npos && (script[mods] == '{' || script.compare(mods, 2, "?{") == 0) && end != std::string::npos)
				{
					// "{Name}" or "{Name/part}": a click on the screens' widget so named ('^', '@' and '+'
					// before it hold Ctrl, Alt and Shift); "?{Name}" the mouse resting on it.
					const std::string held = script.substr(0, mods);
					script.erase(0, mods);
					const size_t close = end - mods;
					const bool hover = script.front() == '?';
					std::string name = script.substr(hover ? 2 : 1, close - (hover ? 2 : 1));
					int part = -1;
					if (const size_t slash = name.find('/'); slash != std::string::npos)
					{
						part = std::atoi(name.c_str() + slash + 1);
						name.resize(slash);
					}
					const bool shift = held.find('+') != std::string::npos, ctrl = held.find('^') != std::string::npos, alt = held.find('@') != std::string::npos;
					if (!(hover ? screens.hover(window, name, part) : screens.click(name, part, shift, ctrl, alt)))
						std::printf("  no widget %s/%d to %s\n", name.c_str(), part, hover ? "hover" : "click");
					script.erase(0, close + 1);
					changed = true;
				}
				else
				{
					// '>' ends the turn whatever is left (Shift+Enter), '~' is Esc, '.' waits; '^', '@'
					// and '+' before a key hold Ctrl, Alt and Shift.
					size_t n = 0;
					bool shift = false, ctrl = false, alt = false;
					for (; n < script.size() && (script[n] == '^' || script[n] == '@' || script[n] == '+'); ++n)
						(script[n] == '^' ? ctrl : script[n] == '@' ? alt : shift) = true;
					const char c = n < script.size() ? script[n] : '.';
					if (c == '>')
						press({ render::key::Return, true, false, false });
					else if (c == '~')
						press({ render::key::Escape, false, false, false });
					else if (std::isalnum(static_cast<unsigned char>(c)) || c == ' ')
						press({ unsigned(std::toupper(static_cast<unsigned char>(c))), shift, ctrl, alt });
					script.erase(0, std::min(n + 1, script.size()));
					changed = true;
				}
			}
			// The WorldBuilder: the mouse over the map is its screen's (where it is, its buttons held,
			// its clicks: CvScreensInterface's mouseOverPlot, leftMouseDown, rightMouseDown), and the
			// plot its brush is on outlined (WorldBuilderGetHighlightPlot); the game's clicks wait.
			window.setWorldBuilder(ui.worldBuilder());
			if (ui.worldBuilder())
			{
				namespace py = pybind11;
				try
				{
					py::module_ si = py::module_::import("CvScreensInterface");
					const int wbScreen = py::module_::import("CvScreenEnums").attr("WORLDBUILDER_SCREEN").cast<int>();
					const auto [hx, hy] = window.hoveredPlot();
					ui.mouseOverMap = hx >= 0 && window.uiHovered() < 0;
					ui.mouseLeft = ui.mouseOverMap && render::mouseHeld(true);
					ui.mouseRight = ui.mouseOverMap && render::mouseHeld(false);
					if (ui.mouseOverMap && (hx != ui.mouseX || hy != ui.mouseY || ui.mouseLeft || ui.mouseRight))
					{
						ui.mouseX = hx, ui.mouseY = hy;
						si.attr("mouseOverPlot")(py::make_tuple(wbScreen));
						changed = true;
					}
					for (const auto& click : clicks)
					{
						ui.mouseX = click.x, ui.mouseY = click.y;
						si.attr("mouseOverPlot")(py::make_tuple(wbScreen));
						if (click.right)
							si.attr("rightMouseDown")(py::make_tuple(wbScreen));
						else
							si.attr("leftMouseDown")(py::make_tuple(wbScreen, click.shift, click.ctrl, click.alt));
						changed = true;
					}
					clicks.clear();
					utility.engine().clear(AREA_BORDER_LAYER_HIGHLIGHT_PLOT, true);
					if (const py::object h = si.attr("WorldBuilderGetHighlightPlot")(py::make_tuple(wbScreen)); py::len(h) >= 2)
						utility.engine().mark({ h[py::int_(0)].cast<int>(), h[py::int_(1)].cast<int>(), 1, 1, 1, 0.9f, AREA_BORDER_LAYER_HIGHLIGHT_PLOT, true, false });
				}
				catch (py::error_already_set& e)
				{
					std::fprintf(stderr, "[oc4core] world builder: %s\n", e.what());
				}
			}
			for (auto click : clicks)
			{
				CvPlot* plot = GC.getMap().plot(click.x, click.y);
				if (!plot)
					continue;
				changed = true;
				// The gamepad's B pressed: out of an interface mode, else nothing selected any more.
				if (click.clear)
				{
					if (ui.getInterfaceMode() != INTERFACEMODE_SELECTION)
						ui.setInterfaceMode(INTERFACEMODE_SELECTION);
					else
						ui.clearSelectionList();
					continue;
				}
				// The gamepad's plot cursor: one's own units or city there are selected, as a left click;
				// anywhere else the selected units go there, as a right one (an interface mode's plot is a left click).
				if (click.either)
				{
					const CvUnit* head = ui.getHeadSelectedUnit();
					bool own = false;
					for (int i = 0; i < plot->getNumUnits() && !own; ++i)
						if (const CvUnit* u = plot->getUnitByIndex(i); u && u->getOwner() == me)
							own = true;
					if (const CvCity* city = plot->getPlotCity(); city && city->getOwner() == me)
						own = true;
					click.right = ui.getInterfaceMode() == INTERFACEMODE_SELECTION && head && !own;
				}
				// The interface mode wants a plot: its mission there (a right click leaves it).
				if (const InterfaceModeTypes mode = ui.getInterfaceMode(); mode != INTERFACEMODE_SELECTION)
				{
					CvSelectionGroup* group = ui.getSelectionList();
					if (log)
						std::printf("  %s at %d,%d: %s\n", GC.getInterfaceModeInfo(mode).getType(), plot->getX(), plot->getY(),
							click.right ? "cancelled" : group && group->canDoInterfaceModeAt(mode, plot) ? "done" : "not there");
					if (!click.right && group && group->canDoInterfaceModeAt(mode, plot))
						if (const int m = GC.getInterfaceModeInfo(mode).getMissionType(); m != NO_MISSION)
						{
							utility.setModifierKeys(click.shift, click.ctrl, click.alt);
							game.selectionListGameNetMessage(GAMEMESSAGE_PUSH_MISSION, m, plot->getX(), plot->getY(), 0, false, click.shift);
							utility.setModifierKeys(false, false, false);
						}
					if (click.right || (group && group->canDoInterfaceModeAt(mode, plot)))
						ui.setInterfaceMode(INTERFACEMODE_SELECTION);
					continue;
				}
				// A click on one's city: its screen (the units there are in the plot list then); on
				// one of the figures in it, as anywhere, its unit.
				if (!click.right && !click.onFigure && !opt.ownInterface && screens.active())
					if (CvCity* city = plot->getPlotCity(); city && city->getOwner() == me)
					{
						ui.selectCity(city, true);
						continue;
					}
				if (!click.right)
				{
					CvUnit* own = nullptr;
					for (int i = 0; i < plot->getNumUnits() && !own; ++i)
						if (CvUnit* u = plot->getUnitByIndex(i); u && u->getOwner() == me)
							own = u;
					if (own)
						ui.selectGroup(own, click.shift, click.ctrl, click.alt);
					else
						ui.clearSelectionList();
				}
				else if (ui.getHeadSelectedUnit())
				{
					utility.setModifierKeys(click.shift, click.ctrl, click.alt);
					ui.unitSound(ui.getHeadSelectedUnit(), true); // (the order's: moves have none of the DLL's)
					game.selectionListMove(plot, click.alt, click.shift, click.ctrl);
					utility.setModifierKeys(false, false, false);
				}
			}
			for (const auto& key : window.takeKeys())
			{
				if (player.isTurnActive() || utility.diplomacy().active())
					press(key);
				changed = true;
			}
			if (const auto a = window.takeActionClick(); a && size_t(*a) < actionIds.size())
			{
				act(actionIds[size_t(*a)], held(render::key::Shift), held(render::key::Control), held(render::key::Menu));
				changed = true;
			}
			// The right button held: the way the selected units would take there, as the original
			// shows it (the game core's interface pathfinder, the turns each plot is reached in).
			const CvUnit* pathHead = ui.getHeadSelectedUnit();
			const std::pair<const void*, int> from{ pathHead, pathHead ? pathHead->getX() * 4096 + pathHead->getY() : -1 };
			if (const auto target = player.isTurnActive() ? window.pathTarget() : std::nullopt; target != pathShown || (target && from != pathFrom))
			{
				pathShown = target;
				pathFrom = from;
				ui.setGotoPlot(target ? GC.getMap().plot(target->first, target->second) : nullptr);
				window.setPath(target ? findPath(ui, *target) : render::MapWindow::PathView{});
			}
			// The plots the game core colours for what is selected (the city's worked plots, the
			// recommended city sites, a ranged unit's reach, ...), when it says they changed, as
			// the original engine asks it for them (CvGame::updateColoredPlots).
			host::Engine& engine = utility.engine();
			const CvUnit* head = ui.getHeadSelectedUnit();
			const CvCity* headCity = ui.getHeadSelectedCity();
			const std::tuple<const void*, const void*, int, int> selected{ head, headCity, head ? head->getX() : -1, head ? head->getY() : -1 };
			if (ui.isDirty(ColoredPlots_DIRTY_BIT) || selected != coloredFor)
			{
				ui.setDirty(ColoredPlots_DIRTY_BIT, false);
				game.updateColoredPlots();
			}
			// The selected settler's city radius (the founding border), as the engine draws it.
			if (engine.foundingDirty.exchange(false) || selected != coloredFor)
			{
				engine.clear(AREA_BORDER_LAYER_FOUNDING_BORDER, true);
				bool founds = false;
				for (int i = 0; head && head->getOwner() == me && i < GC.getNumActionInfos() && !founds; ++i)
					founds = GC.getActionInfo(i).getMissionType() == MISSION_FOUND && game.canHandleAction(i, nullptr, true);
				if (founds)
					for (int dy = -2; dy <= 2; ++dy)
						for (int dx = -2; dx <= 2; ++dx)
							if (std::abs(dx) + std::abs(dy) < 4) // (the fat cross: no corners)
								if (const CvPlot* p = GC.getMap().plot(head->getX() + dx, head->getY() + dy))
									engine.mark({ p->getX(), p->getY(), 1, 1, 1, 0.55f, AREA_BORDER_LAYER_FOUNDING_BORDER, true, false });
			}
			coloredFor = selected;
			// The globe: its view, and its layer's plots coloured (the game core's colours for the
			// active player, again each turn).
			if (const uint64_t g = engine.globeVersion; g != globeShown || (engine.globeLayer >= 0 && game.getGameTurn() != globeTurn))
			{
				globeShown = g;
				globeTurn = game.getGameTurn();
				window.setGlobeView(engine.globeUp);
				globeMarks.clear();
				std::vector<CvGlobeLayerData> layers;
				game.getGlobeLayers(layers);
				if (const int l = engine.globeLayer; l >= 0 && size_t(l) < layers.size() && layers[size_t(l)].m_eType != GLOBE_LAYER_UNIT &&
					layers[size_t(l)].m_eType != GLOBE_LAYER_RESOURCE && me != NO_PLAYER)
				{
					std::vector<NiColorA> colors;
					std::vector<CvPlotIndicatorData> indicators;
					CvPlayerAI::getPlayerNonInl(me).getGlobeLayerColors(layers[size_t(l)].m_eType, engine.globeOption, colors, indicators);
					// (some layers give a plot several colours, the culture its owners' shares: mixed)
					CvMap& map = GC.getMap();
					const size_t per = std::max<size_t>(1, colors.size() / size_t(std::max(map.numPlots(), 1)));
					for (int i = 0; i < map.numPlots() && (size_t(i) + 1) * per <= colors.size(); ++i)
					{
						float r = 0, gr = 0, b = 0, a = 0, n = 0;
						for (size_t k = 0; k < per; ++k)
							if (const NiColorA& c = colors[size_t(i) * per + k]; c.a > 0.01f)
								r += c.r, gr += c.g, b += c.b, a = std::max(a, c.a), ++n;
						if (n > 0)
							if (const CvPlot* p = map.plotByIndex(i))
								globeMarks.push_back({ p->getX(), p->getY(), 1000, { r / n, gr / n, b / n }, std::min(a, 1.0f) * 0.55f, false, true });
					}
				}
				marksShown = ~0ull; // (shown with the others)
			}
			if (const uint64_t version = engine.marksVersion; version != marksShown)
			{
				marksShown = version;
				render::MapWindow::PlotMarks shownMarks;
				std::vector<host::Engine::PlotMark> marks;
				std::vector<host::Engine::Sign> signs;
				{
					std::lock_guard lock(engine.marksMutex);
					marks = engine.marks;
					signs = engine.signs;
				}
				for (const auto& m : marks)
				{
					const auto [x, y] = m.core ? host::windowPlot({ m.x, m.y }) : std::pair{ m.x, m.y };
					shownMarks.marks.push_back({ x, y, m.layer, { m.r, m.g, m.b }, m.a, m.border });
				}
				for (const auto& s : signs)
				{
					const auto [x, y] = s.core ? host::windowPlot({ s.x, s.y }) : std::pair{ s.x, s.y };
					shownMarks.signs.push_back({ x, y, host::gameMarkup(s.caption.c_str()), { 1, 0.95f, 0.75f } });
				}
				shownMarks.marks.insert(shownMarks.marks.end(), globeMarks.begin(), globeMarks.end());
				window.setPlotMarks(std::move(shownMarks));
			}
			// The effects the game core set off: at their plots, as they look.
			for (const auto& e : engine.takeEffects())
			{
				const auto [path, scale] = host::effectArt(e.type);
				if (path.empty())
					continue;
				// (at the window's plots already: the game core gives their places in its viewport's
				// coordinates, CvPlot::getPoint, as the map is captured)
				window.triggerEffect(path, e.x, e.y, e.fx, e.fy, e.rotation, scale);
			}
			// The game's screens: their input, then their update (and redraw) for this frame.
			screens.input(window);
			screens.frame(window, int(window.width()), int(window.height()));
			// The game: the orders sent (the others' turns after ours: on the worker, below).
			// (on the worker too: the update that ends our turn does the turn's work, half a
			// second and more in a big mod; one that takes longer than a frame leaves the window
			// live until it is done, and this frame's rest waits for it)
			const int turn = game.getGameTurn();
			// (a battle shown: the game goes on a battle slice at a time, as its animation)
			if (player.isTurnActive() && !host::Entities::battleWait() && !ui.worldBuilder())
			{
				worker.start([&session] { session.update(); });
				if (!worker.finishWithin(std::chrono::milliseconds(20)))
				{
					window.setBusy(true);
					continue;
				}
			}
			changed = changed || game.getGameTurn() != turn;
			if (ui.lookAtX >= 0)
			{
				// (a scripted game told where to look keeps looking there)
				if (opt.lookX < 0 || opt.viewSeconds <= 0)
					window.centerOn(ui.lookAtX, ui.lookAtY);
				ui.lookAtX = ui.lookAtY = -1;
			}
			// The city screen looks at the city from close by; closed, from the usual distance.
			if (const CvCity* city = ui.isCityScreenUp() ? ui.getHeadSelectedCity() : nullptr; (city != nullptr) != (cityScreen.first >= 0))
			{
				if (city)
					cityScreen = { city->getX(), city->getY() };
				window.lookAt(cityScreen.first, cityScreen.second, city ? 5.0f : 9.0f);
				if (!city)
					cityScreen = { -1, -1 };
				if (log)
				{
					const auto at = Clock::now();
					std::printf("  city screen %s after %lld frames in %.2f s\n", city ? "up" : "closed", frames - cityFrames, std::chrono::duration<double>(at - cityToggled).count());
					cityFrames = frames, cityToggled = at;
				}
			}
			const auto now = Clock::now();
				if (changed || now - captured > std::chrono::milliseconds(200))
				{
				if (const uint64_t v = engine.greatWallVersion; v != greatWallShown)
				{
					greatWallShown = v;
					std::vector<std::pair<int, int>> walls;
					for (const auto& c : host::greatWallCities())
						walls.push_back(host::windowPlot(c));
					capture.setGreatWallCities(std::move(walls));
				}
				window.setMap(capture.capture(&ui));
				if (sounds.started())
					sounds.setSoundscape(soundscapeHere());
				std::string state = game.getGameState() == GAMESTATE_OVER ? "the game is over" : !player.isAlive() ? "you have been defeated"
					: !player.isTurnActive() ? "the other civilizations move" : ui.isEndTurnMessage() ? "press Enter to end the turn" : "your turn";
				if (const InterfaceModeTypes mode = ui.getInterfaceMode(); mode != INTERFACEMODE_SELECTION && mode != NO_INTERFACEMODE)
					state = host::gameText(GC.getInterfaceModeInfo(mode).getDescription()) + ": click the plot (right click, Esc: cancel)";
				if (const host::Net& net = host::Net::get(); net.active())
					state += "  -  " + net.status();
				window.setStatus(capture.caption() + "  -  " + state);
				if (const CvUnit* unit = ui.getHeadSelectedUnit(); playLog && unit && unit->getID() != loggedUnit)
				{
					loggedUnit = unit->getID();
					std::printf("  selected unit %d (type %d) at %d,%d\n", unit->getID(), int(unit->getUnitType()), unit->getX(), unit->getY());
				}
				if (!screens.active())
				{
					window.setInfo(capture.selectionInfo(ui));
					updateActions(player);
				}
				captured = now;
			}
			hear();
			window.frame(std::min(std::chrono::duration<float>(now - last).count(), 0.1f));
			last = now;
			// Not our turn: the others move, a tenth of a second of the game at a time (or one of
			// its updates, however long), then the map is captured again.
			// (a leader talking to the player holds them until the talk is over, as the original's)
			if (!CvPlayerAI::getPlayerNonInl(game.getActivePlayer()).isTurnActive() && game.getGameState() != GAMESTATE_OVER &&
				!utility.diplomacy().active())
			{
				window.setBusy(true);
				worker.start([&session, &game] {
					const auto until = Clock::now() + std::chrono::milliseconds(100);
					do
						if (host::Entities::battleWait())
							Sleep(5);
						else
							session.update();
					while (!CvPlayerAI::getPlayerNonInl(game.getActivePlayer()).isTurnActive() && Clock::now() < until &&
						game.getGameState() != GAMESTATE_OVER);
				});
			}
		}
		// (the game is ours again before anything else looks at it)
		while (!worker.finish())
			Sleep(5);
		window.setBusy(false);
		if (host::Net& net = host::Net::get(); net.active())
		{
			std::printf("  %s, turn %d\n", net.status().c_str(), game.getGameTurn());
			net.stop();
		}
		const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
		std::printf("  map view: %lld frames in %.1fs (%.0f per second), the longest %.0f ms\n", frames, seconds, double(frames) / std::max(seconds, 0.001),
			longestFrame * 1000);
		if (log)
		{
			std::printf("  %s\n", window.cameraText().c_str());
			logMapKnown("at the end");
		}
	}

	std::string lowerAscii(std::string s)
	{
		for (char& c : s)
			c = char(std::tolower(static_cast<unsigned char>(c)));
		return s;
	}

	fs::path documentsFolder()
	{
		PWSTR documents = nullptr;
		if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &documents)))
			return {};
		const fs::path folder = documents;
		CoTaskMemFree(documents);
		return folder;
	}

	fs::path thisProgram()
	{
		wchar_t exe[MAX_PATH]{};
		GetModuleFileNameW(nullptr, exe, MAX_PATH);
		return exe;
	}

	// A mod played from the menu here before: its name as saves have it ("Mods\Name\"; empty: the
	// game without one), its --mod and the program that ran it (each has the game core of its mods).
	struct KnownMod
	{
		std::string folder;
		fs::path mod, exe;
	};

	const fs::path& knownModsFile()
	{
		static const fs::path file = documentsFolder() / L"My Games" / L"openciv4" / L"mods.txt";
		return file;
	}

	fs::path utf8ToPath(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

	std::vector<KnownMod> readKnownMods()
	{
		std::vector<KnownMod> mods;
		std::ifstream in(knownModsFile());
		for (std::string line; std::getline(in, line);)
		{
			const size_t a = line.find('\t'), b = a == std::string::npos ? a : line.find('\t', a + 1);
			if (b != std::string::npos)
				mods.push_back({ line.substr(0, a), utf8ToPath(line.substr(a + 1, b - a - 1)), utf8ToPath(line.substr(b + 1)) });
		}
		std::error_code ec;
		std::erase_if(mods, [&](const KnownMod& m) { return !fs::exists(m.exe, ec); });
		return mods;
	}

	void rememberMod(const KnownMod& mod)
	{
		auto mods = readKnownMods();
		std::erase_if(mods, [&](const KnownMod& m) { return _stricmp(m.folder.c_str(), mod.folder.c_str()) == 0; });
		mods.push_back(mod);
		std::error_code ec;
		fs::create_directories(knownModsFile().parent_path(), ec);
		std::ofstream out(knownModsFile(), std::ios::trunc);
		for (const KnownMod& m : mods)
			out << m.folder << '\t' << utf8Path(m.mod) << '\t' << utf8Path(m.exe) << '\n';
	}

	std::optional<KnownMod> findMod(const std::string& folder)
	{
		for (const KnownMod& m : readKnownMods())
			if (_stricmp(m.folder.c_str(), folder.c_str()) == 0)
				return m;
		return std::nullopt;
	}

	void startAgain(const fs::path& startDir, const fs::path& program, const std::vector<std::wstring>& args)
	{
#ifdef _WIN32
		const std::wstring exe = program.wstring();
		std::wstring line = L"\"" + exe + L"\"";
		for (const std::wstring& a : args)
			line += L" \"" + a + L"\"";
		STARTUPINFOW si{ sizeof(si) };
		PROCESS_INFORMATION pi{};
		std::wprintf(L"\nstarting again: %s\n", line.c_str());
		if (CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr, startDir.c_str(), &si, &pi))
		{
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
		}
#else
		std::error_code ec;
		fs::current_path(startDir, ec);
		std::vector<std::string> strings{ program.string() };
		for (const std::wstring& a : args)
			strings.push_back(fs::path(a).string());
		std::vector<char*> argv;
		for (std::string& a : strings)
			argv.push_back(a.data());
		argv.push_back(nullptr);
		pid_t child = 0;
		if (posix_spawn(&child, argv[0], nullptr, nullptr, argv.data(), environ) != 0)
			std::fprintf(stderr, "cannot start %s\n", argv[0]);
#endif
	}

	// The program of another mod (its install's, logs' and window's options kept).
	void startAgain(const Options& opt, const KnownMod& mod, const std::vector<std::wstring>& then)
	{
		std::vector<std::wstring> args;
		for (size_t i = 0; i + 1 < opt.kept.size(); i += 2)
			if (opt.kept[i] != L"--mod" && opt.kept[i] != L"--python-home")
				args.insert(args.end(), { opt.kept[i], opt.kept[i + 1] });
		if (!mod.mod.empty())
			args.insert(args.end(), { L"--mod", mod.mod.wstring() });
		args.insert(args.end(), then.begin(), then.end());
		startAgain(opt.startDir, mod.exe, args);
	}

	// Where a multiplayer game's start is kept (the host's save, the one a player got).
	fs::path multiplayerFolder()
	{
		const fs::path folder = documentsFolder() / L"My Games" / L"openciv4" / L"Saves" / L"multi";
		fs::create_directories(folder);
		return folder;
	}

	// The temporary main menu (python/oc4menu.py; the original's is the engine's own): the
	// game to play, before there is one. Its leading "{Name}" keys click its widgets, a few a
	// second; with --view-seconds it gives up after that long (and saves --screenshot).
	std::optional<host::MenuChoice> runMenu(const Options& opt, render::MapWindow& window, host::Utility& utility, const std::string& title,
		const std::vector<std::string>& mapScripts, const std::vector<std::string>& mods, std::string& keys, const std::string& message)
	{
		using Clock = std::chrono::steady_clock;
		window.setPlaying(true); // (the keys are the menu's)
		host::Screens screens(utility);
		host::Screens::setTextWidth([&window](const std::string& markup, int font) { return window.textWidth(markup, font); });
		host::Screens::setTextHeight([&window](const std::string& markup, int width, int font) { return window.textHeight(markup, width, font); });
		window.setUiWheel([&screens](int x, int y, int delta) { return screens.wheel(x, y, delta); });
		std::vector<std::pair<fs::path, std::string>> saveFolders;
		if (const fs::path documents = documentsFolder(); !documents.empty())
		{
			const fs::path bts = documents / L"My Games" / L"Beyond the Sword" / L"Saves";
			saveFolders = { { bts / L"single", "single" }, { bts / L"single" / L"auto", "auto" }, { bts / L"multi", "multi" },
				{ documents / L"My Games" / L"openciv4" / L"Saves" / L"auto", "openciv4 auto" } };
		}
		if (opt.uiScale >= 0)
			utility.forceUiScale(opt.uiScale);
		if (opt.showFps)
			window.setFrameRateShown(true);
		if (opt.wasdPan)
			utility.forceWasdPan();
		window.setWasdPan(utility.wasdPan());
		if (opt.autoEndTurn)
			utility.forceAutoEndTurn();
		window.setAutoEndTurn(utility.autoEndTurn());
		window.setUiScale(utility.uiScale());
		if (!screens.startMenu(int(window.width()), int(window.height()), title, mapScripts, saveFolders, mods, message, opt.menuPage))
			throw std::runtime_error("the main menu did not start");
		// Its music (the mod's, if it has its own), unless sound is off.
		host::Sounds& sounds = utility.sounds();
		if (const char* s = std::getenv("OC4_SOUND"); s ? std::string_view(s) != "0" : opt.viewSeconds <= 0)
			sounds.start(s && std::string_view(s) == "mute");
		sounds.setTalkMusic(-1, utility.audioScripts().index("AS2D_OPENING_MENU", AUDIOTAG_2DSCRIPT));
		std::optional<host::MenuChoice> choice;
		auto last = Clock::now(), typed = last;
		const auto start = last;
		while (!choice && window.pump())
		{
			const auto now = Clock::now();
			if (opt.viewSeconds > 0 && std::chrono::duration<double>(now - start).count() > opt.viewSeconds)
				break;
			screens.frame(window, int(window.width()), int(window.height()));
			screens.input(window);
			for (const auto& k : window.takeKeys())
				if (k.key == render::key::Escape)
					screens.escape();
			window.takeClicks(); // (no map under it)
			// (a scripted menu: "%d" and the like are keys, as in a game)
			if (keys.starts_with('%') && keys.size() > 1 && now - typed > std::chrono::milliseconds(400))
			{
				if (const unsigned k = scriptedKey(keys[1]))
					window.pressKey(k);
				keys.erase(0, 2);
				typed = now;
			}
			// (a scripted menu: "{Name}" clicks that widget)
			if (keys.starts_with('{') && now - typed > std::chrono::milliseconds(400))
				if (const size_t end = keys.find('}'); end != std::string::npos)
				{
					// ("{Name/part}": a part of it, as an item of an open pull down)
					std::string name = keys.substr(1, end - 1);
					int part = -1;
					if (const size_t slash = name.find('/'); slash != std::string::npos)
					{
						part = std::atoi(name.c_str() + slash + 1);
						name.resize(slash);
					}
					if (!screens.click(name, part))
						std::printf("  menu: no widget %s\n", name.c_str());
					keys.erase(0, end + 1);
					typed = now;
				}
			choice = screens.takeMenuChoice();
			// (full screen as in a game, when the profile says so; the options' box too)
			window.setFullscreen(utility.graphicOptionChosen(GC.getInfoTypeForString("GRAPHICOPTION_FULLSCREEN", true)));
			sounds.update({}, "");
			window.frame(std::min(std::chrono::duration<float>(now - last).count(), 0.1f));
			last = now;
		}
		if (!choice && !opt.screenshot.empty())
		{
			window.saveScreenshot(opt.screenshot);
			std::printf("  saved %s\n", opt.screenshot.string().c_str());
		}
		screens.closeMenu();
		window.setUi({});
		window.setUiWheel({});
		sounds.setTalkMusic(-1, -1);
		return choice;
	}

	// The map in a window while the AI plays, or rendered into a file.
	void runMapView(const Options& opt, const AssetResolver& assets, host::GameSession& session, host::Utility& utility, render::MapWindow* menuWindow = nullptr)
	{
		using Clock = std::chrono::steady_clock;
		const bool offscreen = !opt.screenshot.empty();
		if (offscreen && opt.play && opt.viewSeconds <= 0)
			throw std::runtime_error("--play with --screenshot needs --view-seconds");
		// (the menu's window goes on with the game chosen there)
		std::optional<render::MapWindow> ownWindow;
		render::MapWindow& window = menuWindow ? *menuWindow : ownWindow.emplace(assets, opt.viewWidth, opt.viewHeight, offscreen);
		{
			std::vector<render::ButtonArt::FontGroup> groups;
			for (const auto& [codes, rows] : host::fontSymbolGroups())
				groups.push_back({ codes, rows });
			window.setGameFontLayout(std::move(groups));
		}
		host::MapCapture capture(assets);
		const auto t0 = Clock::now();
		const render::MapSnapshot& first = capture.capture();
		const double captureMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
		const auto focus = opt.lookX >= 0 ? std::pair{ opt.lookX, opt.lookY } : capture.focus();
		const auto t1 = Clock::now();
		window.setMap(first, focus.first, focus.second);
		if (focus.first >= 0)
			window.lookAt(focus.first, focus.second, opt.lookDistance);
		std::printf("\nmap view: %dx%d plots of %.0f, %zu unit stacks, %zu cities, %zu models (read %.0f ms, built %.0f ms), looking at %d,%d\n",
			first.width, first.height, first.plotSize, first.units.size(), first.cities.size(), first.models.size(), captureMs,
			std::chrono::duration<double, std::milli>(Clock::now() - t1).count(), focus.first, focus.second);
		window.setStatus(capture.caption());
		if (!opt.play)
			window.pressKeys(opt.viewKeys);
		window.setPlotHelp([&](int x, int y) { return capture.plotHelp(x, y); });
		window.setScreenshotFolder(opt.startDir); // not into the game's folder
		if (opt.helpX >= 0)
			std::printf("plot %d,%d:\n%s\n", opt.helpX, opt.helpY, capture.plotHelp(opt.helpX, opt.helpY).c_str());

		if (opt.play)
		{
			// (offscreen: --keys played for --view-seconds, then the last frame saved)
			runPlay(opt, window, capture, session, utility);
			if (offscreen)
			{
				window.saveScreenshot(opt.screenshot);
				std::printf("  saved %s\n", opt.screenshot.string().c_str());
			}
		}
		else if (offscreen)
		{
			window.saveScreenshot(opt.screenshot);
			std::printf("  saved %s\n", opt.screenshot.string().c_str());
		}
		else
		{
			// The AI plays while Space is on, or up to the next turn after N.
			session.beginAutoPlay(opt.turns > 0 ? opt.turns : 100000);
			window.setRunning(opt.turns > 0);
			auto last = Clock::now(), captured = last;
			const auto start = last;
			int untilTurn = -1;
			long long frames = 0;
			while (window.pump() && (opt.viewSeconds <= 0 || std::chrono::duration<double>(Clock::now() - start).count() < opt.viewSeconds))
			{
				++frames;
				if (window.takeTurnRequest())
					untilTurn = GC.getGame().getGameTurn() + 1;
				bool changed = false;
				if ((window.running() || untilTurn >= 0) && !session.autoPlayDone())
				{
					const auto budget = Clock::now() + std::chrono::milliseconds(25);
					do
						if (session.stepAutoPlay() && untilTurn >= 0 && GC.getGame().getGameTurn() >= untilTurn)
						{
							untilTurn = -1;
							changed = true;
							break;
						}
					while (Clock::now() < budget && !session.autoPlayDone());
					// Units move during a turn: show them every now and then.
					changed = changed || Clock::now() - captured > std::chrono::milliseconds(200) || session.autoPlayDone();
				}
				const auto now = Clock::now();
				if (changed)
				{
					window.setMap(capture.capture());
					window.setStatus(capture.caption());
					captured = now;
				}
				window.frame(std::min(std::chrono::duration<float>(now - last).count(), 0.1f));
				last = now;
			}
			session.endAutoPlay();
			const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
			std::printf("  map view: %lld frames in %.1fs (%.0f per second)\n", frames, seconds, double(frames) / std::max(seconds, 0.001));
		}
		for (const auto& problem : window.problems())
			std::printf("  map view: %s\n", problem.c_str());
	}
}

int wmain(int argc, wchar_t** argv)
{
	std::setvbuf(stdout, nullptr, _IONBF, 0); // keep progress visible if the DLL crashes
	host::installCrashHandler();
	try
	{
		const Options opt = [&] {
			Options o = parseArgs(argc, argv);
			// Started as a game is (no options) by a program that has only played mods: the last of them
			// (its game core reads no other).
			if (argc <= 1)
			{
				std::error_code ec;
				std::optional<KnownMod> last;
				bool unmodded = false;
				for (const KnownMod& m : readKnownMods())
					if (fs::equivalent(m.exe, thisProgram(), ec))
					{
						last = m;
						unmodded |= m.mod.empty();
					}
				if (last && !unmodded)
				{
					o.mod = last->mod;
					o.kept.insert(o.kept.end(), { L"--mod", last->mod.wstring() });
				}
			}
			return o;
		}();
		const fs::path btsDir = opt.civ4Root / "Beyond the Sword", exePath = btsDir / "Civ4BeyondSword.exe";

		// A mod is "Mods\Name" in the Beyond the Sword folder or, not installed, any folder by
		// its full path. Saves record "Mods\Name\" either way; the DLL gets the folder as the
		// original engine names it ("Mods\Name\", relative), or its full path.
		std::optional<fs::path> modDir;
		std::string modFolder, dllModFolder;
		if (opt.mod)
		{
			modDir = (btsDir / *opt.mod).lexically_normal().make_preferred(); // a full path replaces btsDir
			if (!modDir->has_filename())
				modDir = modDir->parent_path();
			const bool installed = opt.mod->is_relative();
			modFolder = installed ? opt.mod->lexically_normal().make_preferred().string() : "Mods\\" + modDir->filename().string();
			std::replace(modFolder.begin(), modFolder.end(), '/', '\\'); // original saves use Windows separators
			if (!modFolder.ends_with('\\'))
				modFolder += '\\';
			dllModFolder = installed ? modFolder : modDir->string() + '\\';
			// (without it a mod's game core reads the game's own XML, not its mod's, and fails)
			if (std::error_code ec; !fs::is_directory(*modDir / "Assets", ec))
				throw std::runtime_error("the mod is not there: " + modDir->string() + " (no Assets folder); pass --mod <its folder>");
		}
		// A save of another mod (or of none) played here before: the program of that mod loads it.
		const auto otherMod = [&](const fs::path& save) -> std::optional<KnownMod> {
			const std::string folder = host::SaveFile::read(save).modFolder;
			return _stricmp(folder.c_str(), modFolder.c_str()) == 0 ? std::nullopt : findMod(folder);
		};
		const auto loadElsewhere = [&](const fs::path& save, int hostPort, int humans) {
			const auto mod = otherMod(save);
			if (!mod)
				return false;
			std::vector<std::wstring> then{ L"--play", L"--load", save.wstring() };
			if (hostPort > 0)
				then.insert(then.end(), { L"--host", std::to_wstring(hostPort), L"--humans", std::to_wstring(humans) });
			startAgain(opt, *mod, then);
			return true;
		};
		if (!opt.load.empty() && opt.join.empty() && loadElsewhere(opt.load, opt.hostPort, opt.humans))
			return 0;
		const AssetResolver assets = AssetResolver::forInstall(opt.civ4Root, modDir ? std::optional(*modDir / "Assets") : std::nullopt);

		// The original engine's surroundings: it runs in the Beyond the Sword folder, where
		// mods (BUG's paths, AND2's XML) look for their files, and the game core is in the
		// Assets folder of its mod.
		if (!SetCurrentDirectoryW(btsDir.wstring().c_str()))
			throw std::runtime_error("cannot enter " + btsDir.string());
		const fs::path gameCore = host::loadGameCore(modDir ? std::optional(*modDir / "Assets") : std::nullopt);
		host::setGameCoreModulePaths(exePath, modDir.value_or(btsDir) / "Assets" / "CvGameCoreDLL.dll");

		host::Xml xml(assets);
		host::PythonHost python(assets);
		python.setExecutable(exePath);
		host::Utility utility(assets, python, xml, opt.logDir);
		utility.setModFolder(dllModFolder);
		// Before utility goes: the game core frees memory through its interface until it unloads.
		const struct KeepGameCoreMemory
		{
			~KeepGameCoreMemory() { GC.setDLLIFace(&host::engineMemoryAfterShutdown()); }
		} keepGameCoreMemory;

		const auto t0 = std::chrono::steady_clock::now();
		std::printf("oc4core: x64 host, %zu asset layers, game core %s\n", assets.layerCount(), gameCore.string().c_str());

		std::vector<fs::path> mapDirs;
		if (modDir)
			mapDirs.push_back(*modDir / "PublicMaps");
		for (const char* layer : { "Beyond the Sword/PublicMaps", "Warlords/PublicMaps", "PublicMaps" })
			mapDirs.push_back(opt.civ4Root / layer);
		for (const fs::path& dir : mapDirs)
			python.addMapScriptDir(dir);
		// First, as the original engine does: mods build their globals here (AND2's
		// setDLLIFace constructs them) and route the DLL's allocations through the engine.
		GC.setDLLIFace(&utility);
		stage("Python 2.7 + DLLPublishToPython", [&] {
			python.start(opt.pythonHome);
			host::registerEngineClasses(utility);
			// A mod outside the game's Mods folder: BUG looks for its folder (configs, INI files)
			// under Mods by name, as the replay gives no name this early; its CvModFolder module
			// gives it the mod's own folder instead (os.path.join keeps an absolute path).
			if (modDir && opt.mod->is_absolute())
			{
				std::string path;
				for (const char c : modDir->string())
					path += c == '\\' ? std::string("\\\\") : c == '\'' ? std::string("\\'") : std::string(1, c);
				python.run("import imp, sys\nm = imp.new_module('CvModFolder')\nm.modFolder = '" + path + "'\nsys.modules['CvModFolder'] = m\ndel m\n");
			}
			python.callFunction("CvAppInterface", "init", nullptr); // script output to the engine's logs
			return true;
		});
		std::printf("  (%zu game scripts indexed)\n", python.scriptCount());

		stage("CvGlobals::init", [&] {
			GC.init();
			ARTFILEMGR.Init(); // allocates the tag -> art info maps
			return true;
		});

		CvXMLLoadUtility loader;
		// The options first, as the original engine asks for them before anything else (its
		// options screen is there from the start): AND2's modular loading reads its module list
		// (MLF) there, and every load after it, the global defines too, takes the modules' files.
		stage("LoadPlayerOptions", [&] { return loader.LoadPlayerOptions(); });
		stage("LoadGraphicOptions", [&] { return loader.LoadGraphicOptions(); });
		stage("SetGlobalDefines", [&] { return loader.SetGlobalDefines(); });
		stage("SetGlobalTypes", [&] { return loader.SetGlobalTypes(); });
		stage("LoadBasicInfos", [&] { return loader.LoadBasicInfos(); });
		stage("LoadGlobalText", [&] { return loader.LoadGlobalText(); });
		stage("SetGlobalArtDefines", [&] {
			if (!loader.SetGlobalArtDefines())
				return false;
			ARTFILEMGR.buildArtFileInfoMaps(); // infos below resolve their art by tag
			return true;
		});
		stage("LoadPreMenuGlobals", [&] { return loader.LoadPreMenuGlobals(); });
		stage("SetPostGlobalsGlobalDefines", [&] { return loader.SetPostGlobalsGlobalDefines(); });
		stage("SetupGlobalLandscapeInfo", [&] { return loader.SetupGlobalLandscapeInfo(); });
		// The original engine resets the staging room between the menus: AND2 removes the
		// placeholder religions and corporations it adds for the game font there (cleanTGA),
		// before LoadPostMenuGlobals sums up the infos.
		GC.getInitCore().init(NO_GAMEMODE);
		stage("LoadPostMenuGlobals", [&] { return loader.LoadPostMenuGlobals(); });
		host::assignGameFont(); // as the original engine does when it loads the game font

		const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
		if (xml.filesLoaded() > 0)
			std::printf("\nloaded %zu XML files and %zu text entries in %.2fs\n", xml.filesLoaded(), utility.text().size(), seconds);
		else // the game core parses its XML itself (AND2's rapidxml)
			std::printf("\nloaded the XML (the game core's own parser) and %zu text entries in %.2fs\n", utility.text().size(), seconds);

		// The host only uses what the DLL exports (like the original engine), so
		// inspect the loaded data through the Python API the DLL published.
		std::printf("\nthrough CvPythonExtensions:\n");
		python.run(
			"import sys, CvPythonExtensions as cpe\n"
			"out = sys.__stdout__ # print itself goes to PythonDbg.log\n"
			"gc = cpe.CyGlobalContext()\n"
			"print >>out, '  units %d, buildings %d, techs %d, civilizations %d, leaders %d, promotions %d, terrains %d, events %d' % (\n"
			"    gc.getNumUnitInfos(), gc.getNumBuildingInfos(), gc.getNumTechInfos(), gc.getNumCivilizationInfos(),\n"
			"    gc.getNumLeaderHeadInfos(), gc.getNumPromotionInfos(), gc.getNumTerrainInfos(), gc.getNumEventInfos())\n"
			"u = gc.getUnitInfo(gc.getInfoTypeForString('UNIT_WARRIOR'))\n"
			"print >>out, '  UNIT_WARRIOR: %s, \"%s\", strength %d, cost %d' % (u.getType(), u.getDescription(), u.getCombat(), u.getProductionCost())\n"
			"t = gc.getTechInfo(gc.getInfoTypeForString('TECH_BRONZE_WORKING'))\n"
			"print >>out, '  TECH_BRONZE_WORKING: \"%s\", cost %d, era %s' % (t.getDescription(), t.getResearchCost(), gc.getEraInfo(t.getEra()).getType())\n"
			"print >>out, '  enums: %s = %d, %r' % (cpe.UnitAITypes.UNITAI_ATTACK, cpe.UnitAITypes.UNITAI_ATTACK, cpe.DomainTypes.DOMAIN_SEA)\n"
			"c = gc.getCivilizationInfo(gc.getInfoTypeForString('CIVILIZATION_AMERICA'))\n"
			"print >>out, '  audio script ids: America select %d, action %d' % (c.getSelectionSoundScriptId(), c.getActionSoundScriptId())\n"
			"print >>out, '  define MAX_CIV_PLAYERS = %d, START_YEAR = %d' % (gc.getMAX_CIV_PLAYERS(), gc.getDefineINT('START_YEAR'))\n");

		if (!opt.probeSave.empty())
		{
			host::GameSession session(utility, python);
			session.initEngine();
			session.probeSave(opt.probeSave);
		}

		if (opt.turns > 0 || !opt.load.empty() || opt.view || opt.menu || !opt.save.empty())
		{
			host::GameSession session(utility, python);
			session.setModFolder(modFolder);
			if (opt.randLog)
				GC.getLogging() = GC.getRandLogging() = true;
			stage("engine init (pathfinders, Init event)", [&] {
				session.initEngine();
				return true;
			});
			// The menu first: what it chooses is played as with --play, in its window.
			Options played = opt;
			std::optional<render::MapWindow> menuWindow;
			if (opt.menu)
			{
				menuWindow.emplace(assets, opt.viewWidth, opt.viewHeight, !opt.screenshot.empty());
				std::vector<std::string> maps;
				std::set<std::string> seen;
				for (const fs::path& dir : mapDirs)
				{
					std::error_code ec;
					for (const auto& e : fs::directory_iterator(dir, ec))
						if (e.is_regular_file() && lowerAscii(utf8Path(e.path().extension())) == ".py" && seen.insert(lowerAscii(utf8Path(e.path().stem()))).second)
							maps.push_back(utf8Path(e.path().stem()));
				}
				const std::string title = modDir ? utf8Path(modDir->filename()) : "Civilization IV: Beyond the Sword";
				rememberMod({ modFolder, opt.mod.value_or(fs::path()), thisProgram() });
				const std::vector<KnownMod> mods = readKnownMods();
				std::vector<std::string> otherMods;
				for (const KnownMod& m : mods)
					if (m.folder != modFolder)
						otherMods.push_back(m.folder);
				// (a save that cannot be played here: the menu again, saying why)
				std::string message;
				for (;;)
				{
					const auto choice = runMenu(opt, *menuWindow, utility, title, maps, otherMods, played.viewKeys, message);
					if (!choice || choice->kind == host::MenuChoice::Kind::Exit)
					{
						std::printf("\nmenu: %s\n", choice ? "exit" : "closed");
						return 0;
					}
					played.view = played.play = true;
					played.turns = 0;
					// (multiplayer: hosting it on the usual port, or joining one)
					played.hostPort = choice->host > 0 ? 2056 : 0;
					played.humans = std::max(2, choice->host);
					if (choice->kind == host::MenuChoice::Kind::Join)
					{
						played.join = choice->join;
						break;
					}
					if (choice->kind == host::MenuChoice::Kind::NewGame)
					{
						played.game = choice->game;
						break;
					}
					if (choice->kind == host::MenuChoice::Kind::Mod)
					{
						for (const KnownMod& m : mods)
							if (m.folder == choice->mod)
								startAgain(opt, m, { L"--menu" });
						return 0;
					}
					try
					{
						if (loadElsewhere(choice->save, played.hostPort, played.humans))
							return 0;
						session.checkSave(choice->save);
						played.load = choice->save;
						break;
					}
					catch (const std::exception& e)
					{
						message = e.what();
						std::printf("\nmenu: %s\n", message.c_str());
					}
				}
			}
			// Joining a multiplayer game: the host's save, loaded as any other.
			host::Net::Welcome joined;
			if (!played.join.empty())
			{
				std::printf("\nmultiplayer: joining %s\n", played.join.c_str());
				host::Net::get().join(played.join);
				joined = host::Net::get().awaitWelcome();
				const fs::path file = multiplayerFolder() / L"joined.CivBeyondSwordSave";
				std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(joined.save.data()), std::streamsize(joined.save.size()));
				std::printf("  multiplayer: seat %d, %zu humans, the game %zu bytes\n", joined.seat, joined.humans.size(), joined.save.size());
				played.load = file;
			}
			if (!played.load.empty())
			{
				std::printf("\nload: %s\n", utf8Path(played.load.filename()).c_str());
				stage("load game", [&] {
					session.loadGame(played.load);
					return true;
				});
			}
			else
			{
				std::printf("\nnew game: %s, %s, seed %u%s%s\n", played.game.mapScript.c_str(), played.game.worldSize.c_str(), played.game.seed,
					played.game.leader.empty() ? "" : ", ", played.game.leader.c_str());
				stage("start new game", [&] {
					session.startNewGame(played.game);
					return true;
				});
			}
			if (!opt.execFirst.empty())
			{
				const auto bytes = readWholeFile(opt.execFirst);
				python.run(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
			}
			// Hosting a multiplayer game: a new one is saved and started again from its save (all
			// the players then begin from the same file); a loaded one waits for the others, gives
			// them their seats and the file, and the frames begin.
			if (played.hostPort > 0 && played.load.empty())
			{
				const fs::path file = multiplayerFolder() / L"host-start.CivBeyondSwordSave";
				session.saveGame(file);
				g_relaunch = std::vector<std::wstring>{ L"--play", L"--host", std::to_wstring(played.hostPort), L"--humans",
					std::to_wstring(played.humans), L"--load", file.wstring() };
				played.view = false;
			}
			else if (played.hostPort > 0)
			{
				const PlayerTypes mine = GC.getGame().getActivePlayer();
				std::vector<int> humans{ int(mine) };
				for (int pass = 0; pass < 2; ++pass)
					for (int p = 0; p < GC.getMAX_CIV_PLAYERS() && int(humans.size()) < played.humans; ++p)
					{
						const CvPlayerAI& pl = CvPlayerAI::getPlayerNonInl(static_cast<PlayerTypes>(p));
						if (pl.isAlive() && !pl.isBarbarian() && std::ranges::find(humans, p) == humans.end() && (pass == 1 || pl.isHuman()))
							humans.push_back(p);
					}
				host::Net& net = host::Net::get();
				net.listen(static_cast<uint16_t>(played.hostPort));
				std::printf("\nmultiplayer: hosting on port %d for %zu humans (seats", played.hostPort, humans.size());
				for (const int h : humans)
					std::printf(" %d", h);
				std::printf(")\n");
				std::fflush(stdout);
				// (the window up and live meanwhile, saying who has come; closing it gives up)
				if (!menuWindow && played.view)
					menuWindow.emplace(assets, opt.viewWidth, opt.viewHeight, !opt.screenshot.empty());
				net.acceptPlayers(int(humans.size()) - 1, [&](int joined) {
					if (!menuWindow)
						return true;
					menuWindow->setStatus(std::format("multiplayer: waiting for the others on port {} ({} of {} here)", played.hostPort,
						joined + 1, humans.size()));
					menuWindow->frame(0.1f);
					return menuWindow->pump();
				});
				net.welcome(std::vector<int>(humans.begin() + 1, humans.end()), humans, readWholeFile(played.load));
				session.setupMultiplayer(humans, int(mine));
				net.start();
			}
			else if (!played.join.empty())
			{
				session.setupMultiplayer(joined.humans, joined.seat);
				host::Net::get().start();
			}
			// (in a window the AI plays them while the map shows)
			if (opt.turns > 0 && (!opt.view || !opt.screenshot.empty()))
			{
				std::printf("\nAI autoplay, %d turns:\n", opt.turns);
				host::SamplingProfiler profiler;
				if (opt.profile)
					profiler.start();
				const auto tPlay = std::chrono::steady_clock::now();
				session.autoPlay(opt.turns, [&](int turn) {
					if (turn == opt.saveAt && !opt.save.empty())
					{
						session.saveGame(opt.save);
						std::printf("  saved %s at turn %d\n", opt.save.filename().string().c_str(), turn);
					}
				});
				std::printf("  %.2fs\n", std::chrono::duration<double>(std::chrono::steady_clock::now() - tPlay).count());
				if (opt.profile)
				{
					profiler.stop();
					profiler.report(stdout, 40);
					if (!opt.profileOut.empty())
						profiler.writeFolded(opt.profileOut);
				}
			}
			if (played.view)
				runMapView(played, assets, session, utility, menuWindow ? &*menuWindow : nullptr);
			if (!opt.save.empty() && opt.saveAt < 0)
				stage("save game", [&] {
					session.saveGame(opt.save);
					return true;
				});
		}

		if (!opt.exec.empty())
		{
			const auto bytes = readWholeFile(opt.exec);
			python.run(std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
		}

		std::printf("\n");
		host::printUnimplementedSummary();
		// (another game, or the main menu: the program again, for it)
		if (g_relaunch)
		{
			std::vector<std::wstring> args = opt.kept;
			args.insert(args.end(), g_relaunch->begin(), g_relaunch->end());
			startAgain(opt.startDir, thisProgram(), args);
		}
		return 0;
	}
	catch (const std::exception& e)
	{
		std::fprintf(stderr, "oc4core: %s\n", e.what());
		return 1;
	}
}
