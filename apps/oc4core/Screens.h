#pragma once

// The game's Python screens as the original engine runs them: the main interface comes up
// when a game starts (CvScreensInterface.showMainInterface), is updated every frame and
// redrawn when the DLL marks parts of it dirty (forceScreenUpdate / forceScreenRedraw);
// what the screens' widgets draw (python/oc4screens.py keeps them) goes to the window, and
// the input comes back: a click runs the widget's action in the DLL
// (CvDLLWidgetData::executeAction, as the original's widgets) and reaches the script
// (CvScreensInterface.handleInput); the help for the widget under the mouse is the DLL's
// (CvDLLWidgetData::parseHelp) or the script's.

#include "GameSession.h"
#include "render/MapWindow.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace oc4::host
{
	class Utility;

	// A plot of the window in the game core's coordinates and back: the same but for a mod's
	// viewport (AND2's), which only its scripts' CyMap translates (getMapXFromViewportX, ...).
	std::pair<int, int> gameCorePlot(std::pair<int, int> window);
	std::pair<int, int> windowPlot(std::pair<int, int> gameCore);
	// An effect's art (CvEffectInfo: its NIF and scale), through the scripts' view of the infos
	// (the mod's layout of them is its own). Cached; an unknown one: no path.
	std::pair<std::string, float> effectArt(int effect);
	// The cities with a building that is an area border obstacle (the Great Wall), their plots
	// in the game core's coordinates: through the scripts (the infos' layout is the mod's).
	std::vector<std::pair<int, int>> greatWallCities();

	// The oc4host functions CyInterface (oc4screens.py) is built on; before the scripts load.
	void registerScreenFunctions(Utility& utility);

	// Which soundscape (an index of the soundscape table; -1: none) each terrain, feature and
	// improvement type has, and each era's cities by size (small, medium, large): the types
	// by the scripts' infos, their soundscapes from the XML (the DLL keeps them unexported).
	// While the game is idle.
	struct SoundscapePlaces
	{
		std::vector<int> terrain, feature, improvement;
		std::vector<std::vector<int>> city;
	};
	SoundscapePlaces soundscapePlaces(Utility& utility);

	// What the main menu (python/oc4menu.py) was left with.
	struct MenuChoice
	{
		enum class Kind
		{
			NewGame, Load, Join, Mod, Exit
		};
		Kind kind = Kind::Exit;
		NewGameSettings game;
		std::filesystem::path save;
		int host = 0;     // a multiplayer game hosted for this many humans (port 2056), 0 none
		std::string join; // ... or the one at this address joined
		std::string mod;  // ... or this mod played instead ("Mods\Name\" as saves have it; empty: none)
	};

	class Screens
	{
	public:
		explicit Screens(Utility& utility) : m_utility(utility) {}

		// How wide a text is in the window (CyInterface.determineWidth answers with it).
		static void setTextWidth(std::function<int(const std::string& markup, int font)> width);
		// How high a text is when it wraps at a width, in a widget's font (FontTypes).
		static void setTextHeight(std::function<int(const std::string& markup, int width, int font)> height);
		// The main interface up; false if the scripts failed (the host keeps its own then).
		bool start(int width, int height);
		bool active() const { return m_active; }
		// The temporary main menu instead (the original's is the engine's own, no script's):
		// a title, the map scripts to choose from and the folders with saved games (each with a
		// label). Its choice, once made, from takeMenuChoice; closeMenu takes it down.
		bool startMenu(int width, int height, const std::string& title, const std::vector<std::string>& mapScripts,
			const std::vector<std::pair<std::filesystem::path, std::string>>& saveFolders, const std::vector<std::string>& mods,
			const std::string& message = {}, const std::string& page = {});
		std::optional<MenuChoice> takeMenuChoice();
		void closeMenu();
		// Every frame: the screens' update, their redraw when something is dirty, and what
		// they draw for the window when it changed.
		void frame(render::MapWindow& window, int width, int height);
		// The window's input on the screens' widgets.
		void input(render::MapWindow& window);
		// Esc: closes the screen on top that closes so (not the main interface); true if one did.
		bool escape();
		bool diploKey(unsigned key); // a key while a talk is up: the talk's (Enter, 1 to 9, Esc)
		// A left click on the widget so named (its part `part`, or the first), as from the
		// window (a scripted game); false if no such widget is shown.
		bool click(const std::string& name, int part = -1, bool shift = false, bool ctrl = false, bool alt = false);
		// The mouse on the widget so named, for its help (a scripted game); false if none is shown.
		bool hover(render::MapWindow& window, const std::string& name, int part = -1);
		// The mouse wheel at (x, y): scrolls the table, list or panel there; false if nothing
		// there scrolls.
		bool wheel(int x, int y, int delta);
		// A key (InputTypes) as the original engine gives it to the scripts first: the event
		// manager's kbdEvent, down and up, with the mouse and the plot under it (BUG's shortcuts
		// are there). True if they took it (it goes no further then).
		bool keyEvent(int key, bool shift, bool ctrl, bool alt, int mouseX, int mouseY, int plotX, int plotY);

		bool menuUp(); // the main menu, a talk or a screen over the map: the arrows are its

	private:
		struct Item
		{
			int screen = -1;
			std::string name;
			std::string kind; // as the display list has it ("cell", "dropdown", "option", ...)
			int part = -1;
			int widget = -1, data1 = -1, data2 = -1;
			bool option = false;
			bool checkbox = false;
			std::string movie; // (a movie's file)
		};
		void rebuild(render::MapWindow& window);
		void click(const Item& item, bool right, bool shift, bool ctrl, bool alt);
		// The script's handleInput for the item (a NotifyCode, its data).
		void scriptInput(const Item& item, int notify, int data, int column, bool right, bool shift, bool ctrl, bool alt);
		std::string help(const Item& item) const;

		Utility& m_utility;
		bool m_active = false;
		bool m_menu = false; // the main menu, not the game's screens
		int m_mainId = -1, m_failures = 0;
		long long m_version = -1;
		std::pair<int, int> m_laidOut{ -1, -1 }, m_seenSize{ -1, -1 }; // the size the screens were made for; the window's
		std::chrono::steady_clock::time_point m_sizeChanged;
		std::vector<Item> m_items;
		int m_hovered = -1;
		std::pair<int, int> m_mouse{ -1, -1 }; // where the mouse was at the clicks taken
		unsigned m_diploVersion = 0;           // the talk the diplomacy screen shows
		std::vector<std::pair<int, std::wstring>> m_talkAnswers; // the player's answers in it, last frame
		long long m_minimapVersion = 0;        // the screen minimap the window has
		std::chrono::steady_clock::time_point m_lastUpdate; // when the screens were last updated
		bool m_diploActive = false;
	};
}
