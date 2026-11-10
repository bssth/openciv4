#pragma once

#include "audio/Audio.h"

// A window with the map view: the camera (looking north as in the game; drag or arrow keys
// to move, wheel to zoom, right drag or Q/E (Ctrl+Q/E while playing) to turn), display
// toggles and the game controls the host polls. Plain C++20 interface: the host includes it
// with the SDK's headers.
//
// Keys: Space runs or pauses the AI, N plays one turn; with Ctrl (or alone while watching):
// G grid, B borders, F features, I improvements, resources and roads, U units, P unit
// flags, C cities, L city banners, V the active player's view (fog of war), S saves a
// screenshot; Home the whole map, Esc closes. The minimap (lower right) moves the camera
// where it is clicked. While playing, clicks on plots and the other keys go to the host:
// the view's toggles are then only Ctrl+G, F, I, U, V and P (the game has hotkeys on the
// others: Ctrl+S saves, Ctrl+B bares the map, Home is the next city) and Ctrl+Shift+S the
// screenshot.

#include "core/AssetResolver.h"
#include "render/ButtonArt.h"
#include "render/MapSnapshot.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace oc4::render
{
	class MapWindow
	{
	public:
		// offscreen: no window, for screenshots.
		MapWindow(const AssetResolver& assets, uint32_t width, uint32_t height, bool offscreen);
		~MapWindow();

		// Processes window messages; false once the window is closed.
		bool pump();
		// The first map frames the camera on the active player's capital (focusX/Y) or the middle.
		void setMap(const MapSnapshot& snapshot, int focusX = -1, int focusY = -1);
		void lookAt(int x, int y, float distanceInPlots = 9);
		// Moves the camera over the plot, keeping its distance.
		void centerOn(int x, int y);
		// The interface's size: its units (UiScale.h), which the screens, the mouse and the
		// click positions are in.
		uint32_t width() const;
		uint32_t height() const;
		// A menu or a screen is up over the map: Tab starts choosing on it with the keys, then the
		// arrows choose instead of moving the map and Return and Space use what they chose. With
		// no map behind it (the main menu) the arrows choose at once.
		void setUiTakesKeys(bool takes, bool mapBehind = true);
		// The keyboard's choice ends (a talk's answers changed: Return is the talk's again).
		void clearKeyChoice();
		void pressKey(unsigned key); // (as if typed)
		// W A S D pan the map as the arrows do (the letters alone, no menu up; WasdPan.h).
		// Ctrl+Shift+W turns it off (only while on: BtS has the key too): takeWasdPanToggle has what it was changed to since last asked.
		void setWasdPan(bool on);
		bool wasdPan() const;
		std::optional<bool> takeWasdPanToggle();
		// The frame rate in the corner (Ctrl+Shift+F changes it).
		void setFrameRateShown(bool shown);
		// The interface's scale in percent, 0 for the window's density.
		void setUiScale(int percent);
		struct UiDisplay
		{
			uint32_t width = 0, height = 0; // pixels
			float density = 1;
		};
		UiDisplay uiDisplay() const;
		// Renders a frame (dt seconds after the last) and shows it.
		void frame(float dt);
		void saveScreenshot(const std::filesystem::path& png);
		// Where the S key saves its screenshots (default: the working folder).
		void setScreenshotFolder(const std::filesystem::path& folder);

		// Set by the user: the AI keeps playing; one more turn is asked for.
		bool running() const;
		void setRunning(bool running);
		bool takeTurnRequest();
		// Shown in the title next to the plot under the cursor.
		void setStatus(const std::string& text);
		// Playing: clicks on plots (left without moving the mouse, or right) and the keys (but
		// the view's: Ctrl with its letters, Home) are the host's to take.
		struct PlotClick
		{
			int x = -1, y = -1;
			bool right = false, shift = false, ctrl = false, alt = false;
			bool onFigure = false; // on a unit's figure (the plot is the unit's)
			bool either = false;   // (the gamepad's plot cursor: `right` is the host's to say, see PadAction::Kind::PlotAct)
			bool clear = false;    // (the gamepad's B: no plot's click, the selection let go)
		};
		void setPlaying(bool playing);
		std::vector<PlotClick> takeClicks();
		struct KeyPress
		{
			unsigned key = 0; // virtual-key code
			bool shift = false, ctrl = false, alt = false;
		};
		std::vector<KeyPress> takeKeys();

		// The selected units' actions (the game's action buttons): their icons along the bottom,
		// dimmed when they cannot be done now. A click on one is the host's to take; the help for
		// the one under the mouse comes from it.
		struct ActionButton
		{
			std::string icon; // button art, as for popups
			bool enabled = true;
			bool operator==(const ActionButton&) const = default;
		};
		void setActions(const std::vector<ActionButton>& actions);
		std::optional<int> takeActionClick();
		void setActionHelp(std::function<std::string(int action)> help);
		// Shown in the lower left corner (the selected unit), lines.
		void setInfo(const std::string& text);
		// The game's messages to the player, at the top in the middle as the original shows them:
		// lines with the screens' markup (UTF-8), oldest first, each with its opacity.
		void setMessages(std::vector<std::pair<std::string, float>> messages);

		// A popup to answer (the game's own: what to build, what to research, ...), over the
		// map; while it is up, clicks and keys go to it.
		struct PopupView
		{
			struct Item
			{
				enum class Kind : uint8_t
				{
					Text, Button, Radio, Check, Separator, Image, Edit
				};
				Kind kind = Kind::Text;
				std::string text; // UTF-8, lines (an edit box: what it starts with)
				std::string icon; // a DDS, or ",<file>,<atlas>,<column>,<row>" (64 pixel cells)
				int group = 0;    // radio buttons and checkboxes
				bool checked = false;
			};
			uint64_t serial = 0; // another popup when it changes
			std::string header;
			std::vector<Item> items;
			bool okButton = false;
			bool centered = false; // in the middle of the window (the game menu's style), not at the right
		};
		void setPopup(const PopupView* popup); // nullptr: none
		struct PopupAnswer
		{
			int item = -1;            // the button clicked; -1 OK
			std::vector<int> radio;   // the radio buttons chosen (one per group)
			std::vector<int> checked; // the checkboxes ticked
			std::vector<std::pair<int, std::string>> edits; // each edit box's text (UTF-8)
			bool dismissed = false;   // closed with Esc
		};
		std::optional<PopupAnswer> takePopupAnswer();
		// The help for the popup item under the mouse (lines; the game's help for its widget).
		void setPopupHelp(std::function<std::string(int item)> help);
		// The mouse wheel at (x, y) over the screens: true if something there scrolled (the map
		// does not zoom under the screens either way).
		void setUiWheel(std::function<bool(int x, int y, int delta)> wheel);
		// The game runs on another thread now: the window asks nothing of it (no help for the
		// plot, the buttons or the popup, no scrolling of the screens) until it is idle again.
		void setBusy(bool busy);
		// Between two of the game's updates: the help for what the mouse is over, asked now.
		void askHelp();
		// The mouse at the window's edge scrolls the map (off for scripted runs).
		void setEdgeScroll(bool on);
		// Each terrain's colour on the minimap (BGRA8), by terrain type; empty before the first map.
		std::vector<uint32_t> terrainColors() const;
		// The sounds the units' animations call for (an audio script's tag, where: plot units).
		void setSoundHandler(std::function<void(const std::string& tag, float x, float y)> handler);
		// A movie's sound started (its voice) and stopped when the movie goes.
		void setMovieSound(std::function<uint64_t(std::shared_ptr<const audio::Pcm>)> play, std::function<void(uint64_t)> stop);
		// The movies (UiItem::Movie, by their texture) shown to their end since last asked.
		std::vector<std::string> takeMoviesDone();
		// Where the camera looks (plot units: plot x spans [x, x + 1)) and from how far (plots).
		struct Look
		{
			float x = 0, y = 0, distance = 0;
		};
		Look look() const;

		// The game's Python screens (the main interface, the city screen, ...): what their
		// widgets draw, back to front, in window pixels. While there are any they are the
		// interface: the window's own status, scores and action buttons give way and the
		// minimap goes where they put it.
		struct UiItem
		{
			enum class Kind : uint8_t
			{
				Panel, Image, ImageButton, Button, CheckBox, Label, Text, Multiline, Bar, Line, Header, Cell, Dropdown, EndTurnText,
				EndTurnButton, Minimap, Graphic, Separator, Edit, Slider, Flag, Scrollbar, Leaderhead, Model, Movie
			};
			Kind kind = Kind::Panel;
			float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			bool clipped = false;
			float clip[4]{};
			std::string style;  // the theme's style, or a PanelStyles / ButtonStyles number
			std::string text;   // UTF-8, with the screens' markup (<font=2>, <color=...>)
			std::string texture, texture2;
			int font = 1;       // FontTypes
			int flags = 1;      // 1 left, 2 right, 4 centred (x0 is the anchor)
			bool state = false, enabled = true;
			bool interactive = false; // takes clicks and has help
			bool mainInterface = false; // the main interface's: under a menu the keys do not reach it
			bool blocks = false;      // a screen's own panel or picture over the main interface: what is under it takes no clicks
			bool hasColor = false;
			Color color{};
			float alpha = 1;
			float barStart = 0, barPercent = 0; // (a scroll bar's: where its grip starts, how long it is, as parts of it)
			bool barLast = false; // a stacked bar's last part (its rim goes over them all)
			// A leader's head: its art (texture the NIF, texture2 the KFM, and the background
			// KFM), its button where the art cannot be drawn, its attitude (AttitudeTypes), the
			// gesture last asked of it (LeaderheadAction) and how many have been asked.
			std::string leaderBackground, leaderButton;
			int attitude = -1, action = -1, actionSerial = 0;
			// A model on its own (Model: texture the NIF, texture2 the KFM, its button in
			// leaderButton): turned so many degrees (tilted, about its up axis), so large, animated.
			float modelTilt = 0, modelTurn = 0, modelScale = 1;
			bool modelAnimated = true;
			// ... or its KFM's clips as layers ("camera=<node>;<event code>:<e|l|o>;...", see
			// ModelView): the spaceship screen's launch pad.
			std::string modelLayers;
		};
		void setUi(std::vector<UiItem> items);
		// A screen's own minimap (a UiItem::Minimap of style "__screenmap": the military advisor's,
		// the replay's): the map's terrain (none for a replay of another map), the owners' colours
		// over it that strong (0: none), then the plots the screen coloured (RGBA8 by plot, x + y *
		// width, y north; alpha 0 none) and those it flashes (plot, RGBA).
		struct ScreenMinimap
		{
			int width = 0, height = 0;
			bool terrain = true;
			bool wholeMap = false; // unexplored plots too (a replay's)
			float territory = 0;
			std::vector<uint32_t> colors;
			struct Flash
			{
				int x = 0, y = 0;
				uint32_t color = 0;
			};
			std::vector<Flash> flashes;
			uint64_t version = 0; // another one when it changes
		};
		void setScreenMinimap(ScreenMinimap minimap);

		// What the game's controls show of the map (Ctrl+Y the plots' yields, Ctrl+R the
		// resources' icons, the unit icons, Ctrl+B the bare map, Ctrl+T the grid); the grid and
		// the bare map switch the view's own toggles when they change. The yields' symbols are
		// the game font's (food, production, commerce: markup).
		struct Layers
		{
			bool yields = false, resources = false, units = false, bare = false, grid = false;
			bool operator==(const Layers&) const = default;
		};
		void setLayers(Layers layers);
		// The game's graphic options the window follows (CIV4GraphicOptionInfos).
		struct Graphics
		{
			bool healthBars = false, frozen = false, effects = true, fullscreen = false;
		};
		void setGraphics(Graphics graphics);
		// Full screen or windowed (the profile's option; the menu's too, not only a game's).
		void setFullscreen(bool on);
		// The globe view: the camera back over the whole map (on), where it was (off).
		void setGlobeView(bool up);
		void setYieldSymbols(std::array<std::string, 3> symbols);

		// The plots the game marks (in the window's coordinates): a circle on a plot, or the
		// outline round each set of plots of one layer and colour (a city's radius, a ranged
		// unit's reach); and the signs and landmarks, their captions over their plots.
		struct PlotMarks
		{
			struct Mark
			{
				int x = 0, y = 0, layer = 0;
				Color color{ 1, 1, 1 };
				float alpha = 1;
				bool border = false;
				bool fill = false; // the whole plot coloured (a globe layer's)
			};
			struct Sign
			{
				int x = 0, y = 0;
				std::string caption; // UTF-8
				Color color{ 1, 1, 1 };
			};
			std::vector<Mark> marks;
			std::vector<Sign> signs;
		};
		void setPlotMarks(PlotMarks marks);

		// Playing, the right button held (Ctrl+right drag still turns the camera): the plot under
		// the mouse, where the selected units would go when it is let go (a right click there).
		std::optional<std::pair<int, int>> pathTarget() const;
		// The way there, drawn while the button is held: the plots from the units' to it, each with
		// the turn the units are there in (1 this turn); reachable false: they cannot go there.
		struct PathView
		{
			struct Point
			{
				int x = 0, y = 0, turn = 1;
			};
			std::vector<Point> points;
			bool reachable = true;
		};
		void setPath(PathView path);
		// An effect the game sets off there (MapView::triggerEffect).
		void triggerEffect(const std::string& nif, int x, int y, float fx, float fy, float degrees, float scale);
		bool uiActive() const;
		struct UiClick
		{
			int item = -1;
			bool right = false, shift = false, ctrl = false, alt = false;
		};
		std::vector<UiClick> takeUiClicks();
		// How wide a text with the screens' markup is, in the theme's font (size 2 outside its
		// <font> tags), as the scripts measure theirs.
		// In a widget's font (FontTypes; -1 the theme's size 2, as the scripts measure).
		int textWidth(const std::string& markup, int font = -1) const;
		// How high a text with the screens' markup is when it wraps at a width, in a widget's
		// font (FontTypes), as the screens draw their multi-line texts.
		int textHeight(const std::string& markup, int width, int font) const;
		// The interactive item under the mouse, -1 none.
		int uiHovered() const;
		// As if the mouse rested on that item (a scripted game's look at its help).
		void hoverUi(int item);
		// Where the camera is (for the play log): its target in plots, its distance in plots, its pitch.
		std::string cameraText() const;
		// Where the mouse is, in window pixels ((-1, -1) before it came in).
		std::pair<int, int> mousePosition() const;
		// The help for what is under the mouse, in the screens' help area.
		void setUiHelp(const std::string& text, float x, float y, float width, bool expandRight, bool expandDown);

		// The text shown when the mouse rests on a plot (lines; the game's plot help).
		void setPlotHelp(std::function<std::string(int x, int y)> help);
		// As if these keys were pressed (letters as above), e.g. for screenshots.
		void pressKeys(const std::string& keys);
		// The click a press at that pixel of the window would make on the map (a scripted game).
		std::optional<PlotClick> clickAt(int x, int y, bool right) const;
		// A left click at that pixel as the mouse makes it (scripted tests): on a popup, the
		// screens, the action bar or the map, whichever takes it there.
		void mouseClick(int x, int y);
		// The mouse's cursor over the window: a cursor file (.cur, .ani: the game's res/Cursors);
		// none, the system's arrow.
		void setCursorFile(const std::filesystem::path& file);
		// Where the game font's icons lie, by the DLL's groups (ButtonArt::setFontLayout).
		void setGameFontLayout(std::vector<ButtonArt::FontGroup> groups);
		// The plot under the cursor, (-1, -1) if none.
		std::pair<int, int> hoveredPlot() const;

		// Art that could not be loaded, once per asset.
		const std::vector<std::string>& problems() const;

		struct Impl; // opaque (the window procedure reaches it)

	private:
		std::unique_ptr<Impl> m;
	};
}
