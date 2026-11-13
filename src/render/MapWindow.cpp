#include "render/MapWindow.h"
#include "render/FrameStats.h"
#include "render/GlobeBend.h"
#include "render/UiScale.h"
#include "render/WasdPan.h"

#include "render/Leaderhead.h"
#include "render/ModelView.h"
#include "render/MapView.h"
#include "render/Overlay.h"
#include "render/PngWriter.h"
#include "render/ButtonArt.h"
#include "render/PopupPanel.h"
#include "render/UiLayer.h"
#include "render/Renderer.h"
#include "render/Cursor.h"
#include "render/Input.h"
#include "media/Movie.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <windows.h>

#include <DirectXMath.h>

#include <algorithm>
#include <set>
#include <tuple>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <optional>
#include <stdexcept>
#include <utility>

using namespace DirectX;

namespace oc4::render
{
	namespace
	{
		std::wstring widen(std::string_view s)
		{
			if (s.empty())
				return {};
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
			std::wstring w(size_t(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
			return w;
		}
	}

	struct MapWindow::Impl
	{
		Impl(const AssetResolver& assets, uint32_t width, uint32_t height, bool offscreen)
			: art(assets), popup(art, theme), ui(art, theme), resolver(assets)
		{
			if (!offscreen)
			{
				// (the executable's first icon resource; as the game, scaled by the system on screens of more than 96 dpi)
				SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "1");
				SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "1");
				SDL_SetHint("SDL_WINDOWS_DPI_AWARENESS", "unaware"); // (a hint SDL reads but does not declare)
				if (!SDL_Init(SDL_INIT_VIDEO))
					throw std::runtime_error(std::string("cannot start SDL: ") + SDL_GetError());
				window = SDL_CreateWindow("openciv4", int(width), int(height), SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
				if (!window)
					throw std::runtime_error(std::string("cannot create the map window: ") + SDL_GetError());
				int w = 0, h = 0;
				SDL_GetWindowSizeInPixels(window, &w, &h);
				width = uint32_t(w), height = uint32_t(h);
				SDL_StartTextInput(window);
			}
			renderer = std::make_unique<Renderer>(window, width, height);
			view = std::make_unique<MapView>(*renderer, assets);
			overlay = std::make_unique<Overlay>(*renderer);
			updateUiScale();
			overlay->setArt(&art);
			// The game's interface theme: the screens' and the popups' look, the text's face.
			if (theme.load(assets.layerRoots()))
			{
				std::array<float, 5> sizes{};
				for (int i = 0; i < 5; ++i)
					sizes[size_t(i)] = theme.font(i).size;
				overlay->setFonts(theme.font(2).family, sizes);
			}
			camera.yaw = -std::numbers::pi_v<float> / 2; // from the south, looking north
			if (window)
				SDL_ShowWindow(window);
		}

		~Impl()
		{
			if (!window)
				return;
			for (auto& [file, c] : cursors)
				SDL_DestroyCursor(c);
			// (what draws on the window goes before it)
			heads.clear();
			models.clear();
			view.reset();
			overlay.reset();
			renderer.reset();
			SDL_DestroyWindow(window);
			SDL_QuitSubSystem(SDL_INIT_VIDEO);
		}

		SDL_Window* window = nullptr;
		std::unique_ptr<Renderer> renderer;
		std::unique_ptr<MapView> view;
		std::unique_ptr<Overlay> overlay;
		Camera camera;
		float tilt = 0; // the user's change of the zoom-dependent pitch
		bool open = true, running = false, turnRequest = false, framed = false;
		std::string status, title;
		int mouseX = -1, mouseY = -1; // (in the interface's units, as the screens are)
		int uiPercent = 0; // (0: the window's density)
		bool showFrameRate = false; // (Ctrl+Shift+F)
		bool uiTakesKeys = false;   // a menu or a screen over the map has the arrows (after Tab)
		bool uiMapBehind = true;    // (no map behind it, the main menu: the arrows are its at once)
		bool wasdPan = false;       // W A S D pan the map too (Ctrl+Shift+W, WasdPan.h)
		std::optional<bool> wasdToggled; // (Ctrl+Shift+W since asked: the profile's to keep)
		std::chrono::steady_clock::time_point wasdNoteUntil{}; // (the toggle's note shown till then)
		FrameStats frameStats;
		std::chrono::steady_clock::time_point lastFrame{};
		bool panning = false, orbiting = false, minimapDrag = false;
		bool playing = false;
		std::filesystem::path screenshotFolder;
		std::vector<MapWindow::PlotClick> clicks;
		std::vector<MapWindow::KeyPress> keys;
		std::vector<MapWindow::ActionButton> actions;
		std::vector<std::array<float, 4>> actionRects; // where each was drawn
		std::optional<int> actionClick;
		std::function<std::string(int)> actionHelp;
		int helpAction = -2;
		std::wstring actionHelpText;
		bool downOnActions = false;
		std::wstring info;
		std::vector<std::pair<std::wstring, float>> messages;
		MapWindow::ScreenMinimap screenMinimap;
		MapWindow::Layers layers, layersApplied;
		MapWindow::PlotMarks plotMarks;
		std::array<std::wstring, 3> yieldSymbols;
		bool pathing = false;     // the right button held while playing: the way to the plot under the mouse
		MapWindow::PathView path; // that way, as the host found it
		std::vector<uint8_t> screenPixels;
		std::pair<uint64_t, uint64_t> screenPixelsFor{ ~0ull, ~0ull };
		int downX = 0, downY = 0;
		bool downLeft = false;
		std::function<std::string(int, int)> plotHelp;
		ButtonArt art;
		Theme theme;
		PopupPanel popup;
		std::function<std::string(int)> popupHelp;
		bool busy = false; // the game runs on another thread: nothing asked of it
		bool edgeScroll = false;
		std::function<bool(int, int, int)> uiWheel; // the screens' (a table or list scrolled)
		bool downOnPopup = false;
		UiLayer ui;
		const AssetResolver& resolver;
		// The leaders' heads the screens show, by the art's NIF: the gesture they last did (its
		// count), the attitude they were given, their picture this frame.
		struct Head
		{
			std::unique_ptr<Leaderhead> head;
			int serial = 0, attitude = -1;
			const GpuImage* bitmap = nullptr;
			bool used = false;
		};
		std::map<std::string, Head> heads;
		// The models the screens show on their own, by art and how they are turned.
		struct Shown
		{
			std::unique_ptr<ModelView> view;
			const GpuImage* bitmap = nullptr;
			bool used = false;
		};
		std::map<std::string, Shown> models;
		// The movies the screens play, by file: from when, its sound's voice, whether it ended.
		struct Playing
		{
			std::unique_ptr<media::Movie> movie;
			std::optional<std::chrono::steady_clock::time_point> start;
			uint64_t voice = 0;
			bool used = false, done = false;
		};
		std::map<std::string, Playing> movies;
		std::function<uint64_t(std::shared_ptr<const audio::Pcm>)> playMovieSound;
		std::function<void(uint64_t)> stopMovieSound;
		std::vector<std::string> moviesDone;
		static std::string modelKey(const UiItem& it)
		{
			return std::format("{}|{}|{}|{}|{}|{}|{}", it.texture, it.texture2, it.modelTilt, it.modelTurn, it.modelScale, it.modelAnimated, it.modelLayers);
		}
		std::vector<MapWindow::UiClick> uiClicks;
		bool downOnUi = false;
		std::pair<int, int> helpPlot{ -1, -1 };
		bool helpCombat = false; // (plotHelpText was asked with the right button held: the odds of an attack there)
		std::wstring plotHelpText;
		// (the help of plots the mouse rested on: while the others move it cannot be asked for,
		// and the last one asked stands in, until it can)
		std::map<std::pair<int, int>, std::wstring> plotHelpSeen;
		std::filesystem::path cursorFile;           // (the game's cursor now)
		std::map<std::filesystem::path, SDL_Cursor*> cursors;
		double helpAsked = 0; // (when the help shown was asked for)
		bool fullscreen = false;
		bool globeView = false;
		Vec3 globeFrom;           // (the camera's before the globe view)
		float globeFromDistance = 0, globeFromTilt = 0;
		double restingSince = 0; // when the mouse last moved (seconds of the window's clock)
		double clock = 0;
		float minimapRect[4]{}; // where the minimap was drawn (x0, y0, x1, y1)
		Vec3 anchor;
		int lastX = 0, lastY = 0;
		bool worldBuilder = false;
		bool fogBeforeWorldBuilder = false;

		float plot() const { return view->plotSize(); }

		// The game's camera tilts towards straight down as it zooms out.
		void updatePitch()
		{
			const float t = std::clamp((camera.distance / plot() - 8) / 60, 0.0f, 1.0f);
			camera.pitch = std::clamp(0.85f + 0.55f * t + tilt, 0.2f, 1.55f);
		}

		XMMATRIX viewProjection() const
		{
			const Vec3 eye = camera.eye();
			const XMMATRIX v = XMMatrixLookAtRH(XMVectorSet(eye.x, eye.y, eye.z, 1), XMVectorSet(camera.target.x, camera.target.y, camera.target.z, 1),
				XMVectorSet(0, 0, 1, 0));
			const XMMATRIX p = XMMatrixPerspectiveFovRH(camera.fovY, float(renderer->width()) / float(renderer->height()),
				std::max(0.01f, camera.distance * 0.01f), camera.distance * 20 + 1000);
			return v * p;
		}

		// Where the cursor's ray meets the plane at height z.
		std::optional<Vec3> groundAt(int x, int y, float z = 0) const
		{
			const XMMATRIX inv = XMMatrixInverse(nullptr, viewProjection());
			const float nx = 2.0f * (float(x) + 0.5f) / overlay->screenWidth() - 1, ny = 1 - 2.0f * (float(y) + 0.5f) / overlay->screenHeight();
			const auto unproject = [&](float d) {
				XMFLOAT4 p;
				XMStoreFloat4(&p, XMVector4Transform(XMVectorSet(nx, ny, d, 1), inv));
				return Vec3{ p.x / p.w, p.y / p.w, p.z / p.w };
			};
			const Vec3 a = unproject(0), b = unproject(1);
			if (std::abs(b.z - a.z) < 1e-6f)
				return std::nullopt;
			const float t = (z - a.z) / (b.z - a.z);
			if (t < 0)
				return std::nullopt;
			return a + (b - a) * t;
		}

		void keepOnMap()
		{
			const Vec3 size = view->worldSize();
			if (size.x <= 0)
				return;
			if (view->map().wrapX)
				camera.target.x = std::fmod(std::fmod(camera.target.x, size.x) + size.x, size.x);
			else
				camera.target.x = std::clamp(camera.target.x, 0.0f, size.x);
			camera.target.y = std::clamp(camera.target.y, 0.0f, size.y);
			// A fixed focus plane: following the ground under the centre made the view rise and
			// fall while panning over hills and peaks. Land sits near 0 and peaks stay well below
			// the eye even at the closest zoom.
			camera.target.z = 0;
		}

		void lookAt(int x, int y, float distance = 9)
		{
			camera.target = { (float(x) + 0.5f) * plot(), (float(y) + 0.5f) * plot(), 0 };
			camera.distance = distance * plot();
			updatePitch();
			keepOnMap();
		}

		void showWholeMap()
		{
			const Vec3 size = view->worldSize();
			const float aspect = float(renderer->width()) / float(renderer->height());
			camera.target = { size.x / 2, size.y / 2, 0 };
			camera.distance = std::max(size.x / aspect, size.y) / (2 * std::tan(camera.fovY / 2)) * 1.05f;
			tilt = 0;
			updatePitch();
		}

		void updateTitle()
		{
			if (!window)
				return;
			std::string text = "openciv4";
			if (!status.empty())
				text += "  -  " + status;
			if (!playing)
				text += running ? "  -  AI playing (Space: pause)" : "  -  paused (Space: run, N: next turn)";
			if (const auto p = hoveredPlot(); p.first >= 0)
				text += std::format("  -  plot {}, {}", p.first, p.second);
			if (text != title)
			{
				title = text;
				SDL_SetWindowTitle(window, text.c_str());
			}
		}

		// The plot of the unit whose figure is under the pixel.
		std::optional<std::pair<int, int>> figureUnder(int x, int y) const
		{
			if (view->empty())
				return std::nullopt;
			const XMMATRIX inv = XMMatrixInverse(nullptr, viewProjection());
			const float nx = 2.0f * (float(x) + 0.5f) / overlay->screenWidth() - 1, ny = 1 - 2.0f * (float(y) + 0.5f) / overlay->screenHeight();
			const auto unproject = [&](float d) {
				XMFLOAT4 p;
				XMStoreFloat4(&p, XMVector4Transform(XMVectorSet(nx, ny, d, 1), inv));
				return Vec3{ p.x / p.w, p.y / p.w, p.z / p.w };
			};
			return view->figureAt(unproject(0), unproject(1));
		}

		std::pair<int, int> hoveredPlot() const { return mouseX < 0 ? std::pair{ -1, -1 } : plotUnder(mouseX, mouseY); }

		std::pair<int, int> plotUnder(int x, int y) const
		{
			if (view->empty())
				return { -1, -1 };
			if (const auto g = groundAt(x, y, camera.target.z))
				if (const auto p = view->plotAt(*g))
					return *p;
			return { -1, -1 };
		}

		static bool isArrow(unsigned k) { return k == key::Up || k == key::Down || k == key::Left || k == key::Right; }

		// The keyboard is choosing in a menu or a screen: Tab started it, or there is no map behind
		// to pan (the main menu). Moving the mouse, or the screen going, ends it.
		bool uiKeysChoose() const { return uiTakesKeys && (!uiMapBehind || ui.keyChoice()); }

		// A menu's or a screen's keys: Tab starts choosing, then the arrows choose and Return and
		// Space use what was chosen. Before that the arrows pan the map, as the original's do.
		bool uiKey(unsigned k, bool shift)
		{
			if (k == key::Tab)
			{
				const NavDirection d = shift ? NavDirection::Up : NavDirection::Down;
				if (!ui.moveChoice(d, false))
					ui.moveChoice(d, true);
				return true;
			}
			if (isArrow(k) && !uiKeysChoose())
				return false;
			if (k == key::Return || k == key::Space)
			{
				if (!ui.keyChoice())
					return false;
				uiClicks.push_back({ ui.useKeyChoice(), false, shift, false, false });
				return true;
			}
			const auto direction = [&]() -> std::optional<NavDirection> {
				switch (k)
				{
				case key::Up: return NavDirection::Up;
				case key::Down: return NavDirection::Down;
				case key::Left: return NavDirection::Left;
				case key::Right: return NavDirection::Right;
				default: return std::nullopt;
				}
			}();
			if (!direction)
				return false;
			if (ui.moveChoice(*direction, false))
				return true;
			// Nothing further that way: a list may have more to scroll to; else round to the far side.
			const bool vertical = *direction == NavDirection::Up || *direction == NavDirection::Down;
			const auto [x, y] = ui.keyChoicePlace();
			if (!(vertical && x >= 0 && !busy && uiWheel && uiWheel(x, y, *direction == NavDirection::Up ? 120 : -120)))
				ui.moveChoice(*direction, true);
			return true;
		}

		void onKey(unsigned k)
		{
			auto& o = view->options;
			const auto [shift, ctrl, alt] = heldModifiers();
			// A popup takes the keys (Esc closes it, Enter is its OK).
			if (popup.active() && !ctrl)
			{
				if (k == key::Back)
					popup.character(L'\b');
				// (until Tab starts the keyboard's choice the arrows pan the map under it, as the
				// original's do: handleHeldKeys)
				if (isArrow(k) && !popup.keyboardChoice())
					return;
				popup.key(k == key::Tab && shift ? key::Up : k);
				return;
			}
			// (the game has no Ctrl+Shift+F, and this works over its screens and popups too)
			if (ctrl && shift && !alt && k == 'F')
			{
				showFrameRate = !showFrameRate;
				return;
			}
			if (wasdToggle(k, shift, ctrl, alt, wasdPan))
			{
				wasdPan = !wasdPan;
				wasdToggled = wasdPan;
				wasdNoteUntil = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				return;
			}
			if (uiTakesKeys && !ctrl && !alt && uiKey(k, shift))
				return;
			// (W A S D panning: the press is the map's, not the game's hotkey)
			if (wasdPans(k, wasdState()))
				return;
			// Playing, the game takes the keys (its hotkeys, Esc too: Ctrl+S saves, Ctrl+B bares the
			// map, Home goes to the next city); the view's own toggles are on Ctrl with the letters
			// the game has none for, and Ctrl+Shift+S saves a screenshot.
			const bool viewKey = ctrl && !alt && (shift ? k == 'S' : std::string_view("GFIUVP").find(char(k)) != std::string_view::npos);
			if (playing && !viewKey)
			{
				if (k != key::Shift && k != key::Control && k != key::Menu)
					keys.push_back({ k, shift, ctrl, alt });
				return;
			}
			switch (k)
			{
			case key::Space: running = !running; break;
			case 'N': turnRequest = true; break;
			case 'G': o.grid = !o.grid; break;
			case 'B': o.borders = !o.borders; break;
			case 'F': o.features = !o.features; break;
			case 'I': o.improvements = !o.improvements; break;
			case 'U': o.units = !o.units; break;
			case 'C': o.cities = !o.cities; break;
			case 'L': o.banners = !o.banners; break;
			case 'V': o.fog = !o.fog; break;
			case 'P': o.flags = !o.flags; break;
			case key::Home: showWholeMap(); break;
			case 'S':
			{
				SYSTEMTIME t;
				GetLocalTime(&t);
				saveScreenshot(screenshotFolder /
					std::format("openciv4-{:04}{:02}{:02}-{:02}{:02}{:02}.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond));
				break;
			}
			case key::Escape: open = false; break;
			}
			updateTitle();
		}

		void onMouseMove(int x, int y)
		{
			if (x != mouseX || y != mouseY)
				restingSince = clock;
			mouseX = x;
			mouseY = y;
			popup.hover(x, y);
			ui.hover(x, y);
			if (minimapDrag)
				jumpFromMinimap(std::clamp(x, int(minimapRect[0]), int(minimapRect[2])), std::clamp(y, int(minimapRect[1]), int(minimapRect[3])));
			else if (panning)
			{
				if (const auto g = groundAt(x, y, anchor.z))
				{
					camera.target = camera.target + (anchor - *g);
					keepOnMap();
				}
			}
			else if (orbiting)
			{
				camera.yaw -= float(x - lastX) * 0.005f;
				tilt += float(y - lastY) * 0.005f;
				updatePitch();
			}
			lastX = x;
			lastY = y;
			updateTitle();
		}

		void onButton(bool left, bool down, int x, int y)
		{
			const Modifiers mods = heldModifiers();
			lastX = x;
			lastY = y;
			if (down)
			{
				downX = x;
				downY = y;
				downLeft = left;
			}
			// On a popup: its buttons (the map around it still pans).
			if (down && popup.contains(x, y))
			{
				downOnPopup = true;
				return;
			}
			// On the game's screens: theirs (a click on a widget, or nothing for the map).
			if (down && ui.active() && ui.solid(x, y)) // (a popup up too: both take clicks, as the original's)
			{
				downOnUi = true;
				ui.press(x, y, !left);
				return;
			}
			if (!down && std::exchange(downOnUi, false))
			{
				if (std::abs(x - downX) + std::abs(y - downY) < 5)
					if (const auto click = ui.release(x, y, mods.shift, mods.ctrl, mods.alt))
						uiClicks.push_back(*click);
				return;
			}
			// On the action bar: its buttons.
			if (down && left && actionAt(x, y) >= 0)
			{
				downOnActions = true;
				return;
			}
			if (!down && std::exchange(downOnActions, false))
			{
				if (const int a = actionAt(x, y); a >= 0 && std::abs(x - downX) + std::abs(y - downY) < 5)
					actionClick = a;
				return;
			}
			if (!down && std::exchange(downOnPopup, false))
			{
				if (downLeft && std::abs(x - downX) + std::abs(y - downY) < 5) // (button up messages do not say which)
					popup.click(x, y);
				return;
			}
			// The right button held while playing shows the way there; let go, the units go there.
			if (!down && std::exchange(pathing, false))
			{
				SDL_CaptureMouse(false);
				panning = orbiting = minimapDrag = false;
				mouseX = x;
				mouseY = y;
				if (!popup.active())
					if (const auto p = hoveredPlot(); p.first >= 0)
						clicks.push_back({ p.first, p.second, true, mods.shift, mods.ctrl, mods.alt, false });
				path = {};
				return;
			}
			if (!down && playing && !popup.active() && std::abs(x - downX) + std::abs(y - downY) < 5 && !onMinimap(x, y))
			{
				// A click, not a drag: the plot under it is the host's.
				mouseX = x;
				mouseY = y;
				const auto figure = figureUnder(x, y);
				if (const auto p = figure ? *figure : hoveredPlot(); p.first >= 0)
					clicks.push_back({ p.first, p.second, !downLeft, mods.shift, mods.ctrl, mods.alt, figure.has_value() });
			}
			if (down)
			{
				SDL_CaptureMouse(true);
				if (left && onMinimap(x, y))
				{
					minimapDrag = true;
					jumpFromMinimap(x, y);
				}
				else if (left)
				{
					const auto g = groundAt(x, y, camera.target.z);
					panning = g.has_value();
					if (g)
						anchor = *g;
				}
				else if (playing && !mods.ctrl)
					pathing = true;
				else
					orbiting = true;
			}
			else
			{
				SDL_CaptureMouse(false);
				panning = orbiting = minimapDrag = false;
			}
		}

		void onWheel(int delta)
		{
			if (popup.wheel(mouseX, mouseY, delta))
				return;
			// On the screens: what scrolls there, else nothing (the map only zooms where it shows).
			if (!popup.active() && ui.active() && ((!busy && uiWheel && uiWheel(mouseX, mouseY, delta)) || ui.solid(mouseX, mouseY)))
				return;
			const Vec3 size = view->worldSize();
			camera.distance = std::clamp(camera.distance * std::pow(0.85f, float(delta) / 120), 1.5f * plot(), std::max(size.x, size.y) * 1.5f + plot());
			updatePitch();
		}

		WasdState wasdState() const
		{
			const auto [shift, ctrl, alt] = heldModifiers();
			return { wasdPan, popup.active() || uiTakesKeys, shift, ctrl, alt, (SDL_GetModState() & SDL_KMOD_GUI) != 0 };
		}

		void handleHeldKeys(float dt)
		{
			if (!window || SDL_GetKeyboardFocus() != window)
				return;
			const auto held = keyHeld;
			const float speed = camera.distance * 0.9f * dt;
			const Vec3 forward{ -std::cos(camera.yaw), -std::sin(camera.yaw), 0 }, right{ -std::sin(camera.yaw), std::cos(camera.yaw), 0 };
			Vec3 move{};
			// (once the keyboard chooses in a menu or a popup, the arrows are its, not the map's)
			const bool arrows = !(popup.active() && popup.keyboardChoice()) && !uiKeysChoose();
			if (arrows && held(key::Up))
				move += forward;
			if (arrows && held(key::Down))
				move += forward * -1;
			if (arrows && held(key::Right))
				move += right;
			if (arrows && held(key::Left))
				move += right * -1;
			// W A S D as the arrows, when the option is on (and the letter alone).
			if (const WasdState wasd = wasdState(); wasdPans('W', wasd))
			{
				if (held('W'))
					move += forward;
				if (held('S'))
					move += forward * -1;
				if (held('D'))
					move += right;
				if (held('A'))
					move += right * -1;
			}
			// The mouse at the window's edge scrolls the map that way, as the original's does.
			if (float fx = 0, fy = 0; edgeScroll && !popup.active() && SDL_GetMouseFocus() == window)
			{
				SDL_GetMouseState(&fx, &fy);
				const float density = SDL_GetWindowPixelDensity(window);
				const int px = int(fx * density), py = int(fy * density), w = int(renderer->width()), h = int(renderer->height());
				constexpr int kEdge = 3;
				if (px >= 0 && py >= 0 && px < w && py < h)
				{
					if (py < kEdge)
						move += forward;
					else if (py >= h - kEdge)
						move += forward * -1;
					if (px < kEdge)
						move += right * -1;
					else if (px >= w - kEdge)
						move += right;
				}
			}
			if (move.x != 0 || move.y != 0)
			{
				camera.target = camera.target + move * speed;
				keepOnMap();
			}
			if (playing && !held(key::Control)) // (the letters are orders then)
				return;
			if (held('Q'))
				camera.yaw += dt * 1.2f;
			if (held('E'))
				camera.yaw -= dt * 1.2f;
		}

		// The heads of the screens' items: made for those new, gone with their items; the
		// attitude they are given, the gestures asked of them since.
		void syncHeads()
		{
			for (auto& [nif, h] : heads)
				h.used = false;
			for (const auto& it : ui.items())
			{
				if (it.kind != UiItem::Kind::Leaderhead || it.texture.empty())
					continue;
				Head& h = heads[it.texture];
				if (!h.head)
					h.head = std::make_unique<Leaderhead>(*renderer, resolver, it.texture, it.texture2, it.leaderBackground);
				h.used = true;
				if (it.attitude >= 0 && it.attitude != h.attitude)
				{
					h.attitude = it.attitude;
					h.head->setAttitude(it.attitude);
				}
				if (it.actionSerial != h.serial)
				{
					h.serial = it.actionSerial;
					h.head->perform(it.action);
				}
			}
			std::erase_if(heads, [](const auto& e) { return !e.second.used; });
			for (auto& [key, m] : models)
				m.used = false;
			for (const auto& it : ui.items())
			{
				if (it.kind != UiItem::Kind::Model || it.texture.empty())
					continue;
				Shown& m = models[modelKey(it)];
				if (!m.view)
					m.view = std::make_unique<ModelView>(*renderer, resolver, it.texture, it.texture2, it.modelTilt, it.modelTurn, it.modelScale, it.modelAnimated,
						it.modelLayers);
				m.used = true;
			}
			std::erase_if(models, [](const auto& e) { return !e.second.used; });
			for (auto& [file, p] : movies)
				p.used = false;
			for (const auto& it : ui.items())
			{
				if (it.kind != UiItem::Kind::Movie || it.texture.empty())
					continue;
				Playing& p = movies[it.texture];
				if (!p.movie)
				{
					const auto at = resolver.locate(it.texture);
					p.movie = std::make_unique<media::Movie>(at ? at->file : std::filesystem::path());
				}
				p.used = true;
			}
			std::erase_if(movies, [&](const auto& e) {
				if (e.second.used)
					return false;
				if (e.second.voice && stopMovieSound)
					stopMovieSound(e.second.voice);
				return true;
			});
		}

		// A movie: black until it can start, then the picture of the time since, as large as fits.
		void drawMovie(const UiItem& it)
		{
			overlay->rect(it.x0, it.y0, it.x1, it.y1, { 0, 0, 0 }, it.alpha);
			const auto at = movies.find(it.texture);
			if (at == movies.end())
				return;
			Playing& p = at->second;
			if (!p.done && p.movie->finished())
			{
				p.done = true;
				moviesDone.push_back(it.texture);
			}
			if (!p.movie->ready())
				return;
			const auto now = std::chrono::steady_clock::now();
			if (!p.start)
			{
				p.start = now;
				if (const auto sound = p.movie->sound(); sound && playMovieSound)
					p.voice = playMovieSound(sound);
			}
			const media::Movie::Frame* f = p.movie->frameAt(std::chrono::duration<double>(now - *p.start).count());
			if (!f || !f->width || !f->height)
				return;
			const float w = it.x1 - it.x0, h = it.y1 - it.y0, s = std::min(w / float(f->width), h / float(f->height));
			const float cx = (it.x0 + it.x1) / 2, cy = (it.y0 + it.y1) / 2, fw = float(f->width) * s / 2, fh = float(f->height) * s / 2;
			overlay->image(std::hash<std::string>{}(it.texture), f->bgra, f->width, f->height, f->number, cx - fw, cy - fh, cx + fw, cy + fh, it.alpha);
		}

		// Each head drawn into its picture, as large as its item.
		void renderHeads()
		{
			for (auto& [nif, h] : heads)
				h.bitmap = nullptr;
			for (const auto& it : ui.items())
				if (it.kind == UiItem::Kind::Leaderhead)
					if (const auto at = heads.find(it.texture); at != heads.end() && at->second.head->loaded() && !at->second.bitmap)
						at->second.bitmap = at->second.head->render(uint32_t(std::max(1.0f, (it.x1 - it.x0) * overlay->scale())), uint32_t(std::max(1.0f, (it.y1 - it.y0) * overlay->scale())));
			for (auto& [key, m] : models)
				m.bitmap = nullptr;
			for (const auto& it : ui.items())
				if (it.kind == UiItem::Kind::Model)
					if (const auto at = models.find(modelKey(it)); at != models.end() && at->second.view->loaded() && !at->second.bitmap)
						at->second.bitmap = at->second.view->render(uint32_t(std::max(1.0f, (it.x1 - it.x0) * overlay->scale())), uint32_t(std::max(1.0f, (it.y1 - it.y0) * overlay->scale())));
		}

		bool drawHead(const UiItem& it)
		{
			if (it.kind == UiItem::Kind::Movie)
			{
				drawMovie(it);
				return true;
			}
			if (it.kind == UiItem::Kind::Model)
			{
				const auto m = models.find(modelKey(it));
				if (m == models.end() || !m->second.bitmap)
					return false;
				overlay->bitmap(m->second.bitmap, it.x0, it.y0, it.x1, it.y1, it.alpha);
				return true;
			}
			const auto at = heads.find(it.texture);
			if (at == heads.end() || !at->second.bitmap)
				return false;
			overlay->bitmap(at->second.bitmap, it.x0, it.y0, it.x1, it.y1, it.alpha);
			return true;
		}

		// The map, the cities' banners and the status lines.
		void render()
		{
			const GlobeShape g = globeShape();
			renderer->setGlobe(g.x, g.y, g.radius, g.wrapWidth);
			renderer->beginFrame(camera, { 0.05f, 0.07f, 0.10f });
			view->draw();
			renderer->endFrame();
			syncHeads();
			renderHeads();
			if (!overlay->begin())
				return;
			view->drawOverlay(*overlay);
			drawMarks();
			drawLayers();
			drawPath();
			if (ui.active())
			{
				// The game's own interface: it has the status, the scores, the unit's buttons.
				minimapRect[0] = minimapRect[1] = minimapRect[2] = minimapRect[3] = 0;
				ui.draw(*overlay, [&](float x0, float y0, float x1, float y1, bool screen) {
					if (screen)
						drawScreenMinimap(x0, y0, x1, y1);
					else
						drawMinimap(x0, y0, x1, y1, false);
				}, clock,
					[&](const UiItem& it) { return drawHead(it); });
			}
			else
			{
				const std::wstring top = widen(status), state = playing ? std::wstring(L"click: select, right click: go, Enter: end turn")
					: running ? L"AI playing  (Space: pause)" : L"paused  (Space: run, N: next turn)";
				const float w = std::max(overlay->width(top, 15, true), overlay->width(state, 12)) + 24;
				overlay->rect(8, 8, 8 + w, 56, { 0.05f, 0.06f, 0.08f }, 0.7f, 6);
				overlay->text(top, 20, 13, 15, { 1, 0.92f, 0.7f }, 1, true);
				overlay->text(state, 20, 34, 12, { 0.85f, 0.88f, 0.95f });
				if (const auto p = hoveredPlot(); p.first >= 0)
					overlay->text(std::format(L"plot {}, {}", p.first, p.second), 12, overlay->screenHeight() - 26, 12, { 0.9f, 0.9f, 0.9f });
				drawScores();
				const float mw = 240, sw = overlay->screenWidth(), sh = overlay->screenHeight();
				drawMinimap(sw - 12 - mw, sh - 12 - mw, sw - 12, sh - 12, true);
				drawInfo();
				drawActions();
			}
			drawMessages();
			if (!popup.active())
				drawPlotHelp();
			popup.draw(*overlay, busy ? std::function<std::string(int)>{} : popupHelp);
			if (showFrameRate)
				drawFrameRate();
			if (std::chrono::steady_clock::now() < wasdNoteUntil)
				drawWasdNote();
			overlay->end();
		}

		// Under the interface's top left corner, over everything.
		void drawFrameRate()
		{
			const std::wstring text = frameStats.ready() ?
				std::format(L"{} FPS, worst {} ms", std::lround(frameStats.fps()), std::lround(frameStats.worstMs())) : std::wstring(L"Measuring...");
			const float size = 13, x = 8, y = 76, width = overlay->width(text, size, true);
			overlay->rect(x, y, x + width + 14, y + 23, { 0.03f, 0.04f, 0.06f }, 0.75f, 4);
			overlay->text(text, x + 7, y + 4, size, { 1, 1, 1 }, 1, true);
		}

		// Ctrl+Shift+W's answer, at the top in the middle for a moment.
		void drawWasdNote()
		{
			const std::wstring text = wasdPan ? L"W A S D pan the map (Ctrl+Shift+W: off)" : L"W A S D are the game's hotkeys again (the options' Game tab: pan)";
			const float size = 13, width = overlay->width(text, size, true), x = (overlay->screenWidth() - width) / 2 - 7, y = 76;
			overlay->rect(x, y, x + width + 14, y + 23, { 0.03f, 0.04f, 0.06f }, 0.75f, 4);
			overlay->text(text, x + 7, y + 4, size, { 1, 1, 1 }, 1, true);
		}

		// The globe view: the map round a sphere under the point looked at, once round its width
		// (radius 0: the flat map).
		GlobeShape globeShape() const
		{
			if (const Vec3 size = view->worldSize(); globeView && size.x > 0)
				return { camera.target.x, camera.target.y, size.x / (2 * 3.14159265f), view->map().wrapX ? size.x : 0 };
			return {};
		}

		// A point of the map's ground on the screen (the copy nearest the camera on a wrapping
		// map, or where the globe bends it); false behind the camera or round the globe's far side.
		bool groundToScreen(float wx, float wy, float lift, float& sx, float& sy) const
		{
			const GlobeShape g = globeShape();
			if (const Vec3 size = view->worldSize(); g.radius <= 0 && view->map().wrapX && size.x > 0)
				while (std::abs(wx - camera.target.x) > size.x / 2)
					wx += wx < camera.target.x ? size.x : -size.x;
			GlobePoint p{ wx, wy, view->groundHeight(wx, wy) + lift };
			if (g.radius > 0)
			{
				p = globeBend(g, p);
				const Vec3 eye = camera.eye();
				if (!globeFacing(g, p, { eye.x, eye.y, eye.z }))
					return false;
			}
			XMFLOAT4 c;
			XMStoreFloat4(&c, XMVector4Transform(XMVectorSet(p.x, p.y, p.z, 1), viewProjection()));
			if (c.w <= 0)
				return false;
			sx = (c.x / c.w * 0.5f + 0.5f) * overlay->screenWidth();
			sy = (0.5f - c.y / c.w * 0.5f) * overlay->screenHeight();
			return true;
		}

		// The marked plots: circles; the outline of each layer's and colour's set of plots, its
		// edges where the next plot is not of it; the signs' captions.
		void drawMarks()
		{
			const auto& map = view->map();
			if (map.width <= 0 || (plotMarks.marks.empty() && plotMarks.signs.empty()))
				return;
			const float S = plot();
			std::map<std::tuple<int, uint32_t>, std::set<std::pair<int, int>>> sets;
			const auto key = [](const MapWindow::PlotMarks::Mark& m) {
				return std::tuple{ m.layer, uint32_t(m.color.r * 255) | uint32_t(m.color.g * 255) << 8 | uint32_t(m.color.b * 255) << 16 };
			};
			// (the plots filled first, under the rest: a globe layer's colours)
			for (const auto& m : plotMarks.marks)
			{
				if (!m.fill)
					continue;
				float px[4], py[4];
				bool ok = true;
				static const float kCorners[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
				for (int k = 0; k < 4 && ok; ++k)
					ok = groundToScreen((float(m.x) + kCorners[k][0]) * S, (float(m.y) + kCorners[k][1]) * S, 2, px[k], py[k]);
				if (ok)
					overlay->quad(px, py, m.color, m.alpha);
			}
			for (const auto& m : plotMarks.marks)
				if (m.border)
					sets[key(m)].insert({ m.x, m.y });
			for (const auto& m : plotMarks.marks)
			{
				if (m.fill)
					continue;
				if (m.border)
				{
					const auto& set = sets[key(m)];
					// (the plot's edges: south, east, north, west, and the plot beyond each)
					static const int kEdges[4][6] = { { 0, 0, 1, 0, 0, -1 }, { 1, 0, 1, 1, 1, 0 }, { 1, 1, 0, 1, 0, 1 }, { 0, 1, 0, 0, -1, 0 } };
					for (const auto& e : kEdges)
					{
						int nx = m.x + e[4];
						if (map.wrapX)
							nx = (nx + map.width) % map.width;
						if (set.contains({ nx, m.y + e[5] }))
							continue;
						float ax = 0, ay = 0, bx = 0, by = 0;
						if (groundToScreen((float(m.x) + float(e[0])) * S, (float(m.y) + float(e[1])) * S, 3, ax, ay) &&
							groundToScreen((float(m.x) + float(e[2])) * S, (float(m.y) + float(e[3])) * S, 3, bx, by))
						{
							overlay->line(ax, ay, bx, by, { 0, 0, 0 }, 0.35f * m.alpha, 4);
							overlay->line(ax, ay, bx, by, m.color, std::min(1.0f, m.alpha + 0.2f), 2);
						}
					}
				}
				else
				{
					float cx = 0, cy = 0, ex = 0, ey = 0;
					if (groundToScreen((float(m.x) + 0.5f) * S, (float(m.y) + 0.5f) * S, 3, cx, cy) &&
						groundToScreen((float(m.x) + 0.85f) * S, (float(m.y) + 0.5f) * S, 3, ex, ey))
					{
						const float rx = std::abs(ex - cx), ry = rx * 0.6f;
						overlay->ellipse(cx, cy, rx, ry, { 0, 0, 0 }, 0.4f * m.alpha, 5);
						overlay->ellipse(cx, cy, rx, ry, m.color, std::min(1.0f, m.alpha + 0.2f), 2.5f);
					}
				}
			}
			for (const auto& s : plotMarks.signs)
			{
				float sx = 0, sy = 0;
				if (!groundToScreen((float(s.x) + 0.5f) * S, (float(s.y) + 0.5f) * S, 0.4f * S, sx, sy) || s.caption.empty())
					continue;
				const std::wstring text = widen(s.caption);
				const float w = overlay->width(text, 12, true) + 12;
				overlay->rect(sx - w / 2, sy - 10, sx + w / 2, sy + 10, { 0.05f, 0.06f, 0.08f }, 0.75f, 4);
				overlay->text(text, sx, sy - 8, 12, s.color, 1, true, Overlay::Align::Center);
			}
		}

		// The game's layers over the plots near the camera: the yields (as many symbols as the
		// plot gives, a row each), the resources' icons, the units' icons over their stacks.
		void drawLayers()
		{
			if (!layers.yields && !layers.resources && !layers.units)
				return;
			const auto& map = view->map();
			if (map.width <= 0 || camera.distance > 32 * plot())
				return;
			const XMMATRIX vp = viewProjection();
			const auto toScreen = [&](float wx, float wy, float lift, float& sx, float& sy) {
				if (const Vec3 size = view->worldSize(); map.wrapX && size.x > 0)
					while (std::abs(wx - camera.target.x) > size.x / 2)
						wx += wx < camera.target.x ? size.x : -size.x;
				XMFLOAT4 c;
				XMStoreFloat4(&c, XMVector4Transform(XMVectorSet(wx, wy, view->groundHeight(wx, wy) + lift, 1), vp));
				if (c.w <= 0)
					return false;
				sx = (c.x / c.w * 0.5f + 0.5f) * overlay->screenWidth();
				sy = (0.5f - c.y / c.w * 0.5f) * overlay->screenHeight();
				return sx > -40 && sy > -40 && sx < overlay->screenWidth() + 40 && sy < overlay->screenHeight() + 40;
			};
			const float zoom = std::clamp(12 * plot() / camera.distance, 0.6f, 1.4f);
			for (int y = 0; y < map.height; ++y)
				for (int x = 0; x < map.width; ++x)
				{
					const PlotView& p = map.plots[size_t(x) + size_t(y) * size_t(map.width)];
					if (view->options.fog && !p.revealed)
						continue;
					float sx = 0, sy = 0;
					if (!toScreen((float(x) + 0.5f) * plot(), (float(y) + 0.5f) * plot(), 0, sx, sy))
						continue;
					if (layers.resources && p.bonus >= 0 && size_t(p.bonus) < map.bonusButtons.size())
						if (const ButtonArt::Icon* ic = art.icon(map.bonusButtons[size_t(p.bonus)]))
						{
							const float r = 13 * zoom;
							overlay->rect(sx - r - 2, sy - r - 2 - 18 * zoom, sx + r + 2, sy + r + 2 - 18 * zoom, { 0, 0, 0 }, 0.55f, 4);
							overlay->image(ic->key, ic->bgra, ic->width, ic->height, 1, sx - r, sy - r - 18 * zoom, sx + r, sy + r - 18 * zoom);
						}
					if (layers.yields)
					{
						// (a row a yield, as many of its symbol as it gives: five and more as a number)
						const float size = 11 * zoom;
						float ry = sy - size * 0.5f * float(int(p.yields[0] > 0) + int(p.yields[1] > 0) + int(p.yields[2] > 0)) * 1.1f;
						for (int k = 0; k < 3; ++k)
						{
							const int n = p.yields[k];
							if (n <= 0 || yieldSymbols[size_t(k)].empty())
								continue;
							std::wstring row;
							if (n >= 5)
								row = std::to_wstring(n) + yieldSymbols[size_t(k)];
							else
								for (int i = 0; i < n; ++i)
									row += yieldSymbols[size_t(k)];
							// (in the yields' colours: the font's stand-ins have none of their own)
							static const Color kYield[3] = { { 0.55f, 0.95f, 0.35f }, { 0.75f, 0.8f, 0.95f }, { 1, 0.85f, 0.2f } };
							overlay->richText(row, sx, ry, size, true, kYield[k], 1, Overlay::Align::Center);
							ry += size * 1.1f;
						}
					}
				}
			if (layers.units)
				for (const UnitView& u : map.units)
				{
					if (u.button.empty() || (view->options.fog && !map.plots[size_t(u.x) + size_t(u.y) * size_t(map.width)].visible))
						continue;
					float sx = 0, sy = 0;
					if (!toScreen((float(u.x) + 0.5f) * plot(), (float(u.y) + 0.5f) * plot(), 0.5f * plot(), sx, sy))
						continue;
					if (const ButtonArt::Icon* ic = art.icon(u.button))
					{
						const float r = 12 * zoom;
						const Color c = u.shownOwner >= 0 && size_t(u.shownOwner) < map.players.size() ? map.players[size_t(u.shownOwner)].primary : Color{ 0.5f, 0.5f, 0.5f };
						overlay->rect(sx - r - 2, sy - r - 2, sx + r + 2, sy + r + 2, c, 0.9f, 3);
						overlay->image(ic->key, ic->bgra, ic->width, ic->height, 1, sx - r, sy - r, sx + r, sy + r);
					}
				}
		}

		// The way the right button shows: from plot to plot on the ground, the number of turns
		// where each turn's move ends and at its end; a red circle alone where they cannot go.
		void drawPath()
		{
			if (!pathing || path.points.empty())
				return;
			const XMMATRIX vp = viewProjection();
			const auto toScreen = [&](const MapWindow::PathView::Point& p, float& sx, float& sy) {
				float wx = (float(p.x) + 0.5f) * plot(), wy = (float(p.y) + 0.5f) * plot();
				// (on a wrapping map, the copy of the plot nearest the camera)
				if (const Vec3 size = view->worldSize(); view->map().wrapX && size.x > 0)
					while (std::abs(wx - camera.target.x) > size.x / 2)
						wx += wx < camera.target.x ? size.x : -size.x;
				XMFLOAT4 c;
				XMStoreFloat4(&c, XMVector4Transform(XMVectorSet(wx, wy, view->groundHeight(wx, wy) + 2, 1), vp));
				if (c.w <= 0)
					return false;
				sx = (c.x / c.w * 0.5f + 0.5f) * overlay->screenWidth();
				sy = (0.5f - c.y / c.w * 0.5f) * overlay->screenHeight();
				return true;
			};
			const Color way = path.reachable ? Color{ 0.95f, 0.95f, 0.85f } : Color{ 1, 0.3f, 0.25f };
			std::vector<std::pair<float, float>> at(path.points.size());
			std::vector<bool> shown(path.points.size());
			for (size_t i = 0; i < path.points.size(); ++i)
				shown[i] = toScreen(path.points[i], at[i].first, at[i].second);
			// (no way there: only a red circle where it would lead, as the original)
			for (size_t i = 1; path.reachable && i < at.size(); ++i)
				if (shown[i - 1] && shown[i])
				{
					overlay->line(at[i - 1].first, at[i - 1].second, at[i].first, at[i].second, { 0, 0, 0 }, 0.6f, 5);
					overlay->line(at[i - 1].first, at[i - 1].second, at[i].first, at[i].second, way, 0.95f, 2.5f);
				}
			for (size_t i = 1; i < at.size(); ++i)
			{
				const bool last = i + 1 == at.size(), turnEnds = last || path.points[i + 1].turn != path.points[i].turn;
				if (!shown[i] || !turnEnds)
					continue;
				const float r = last ? 11.0f : 9.0f;
				overlay->ellipse(at[i].first, at[i].second, r, r, { 0.05f, 0.06f, 0.1f }, 0.85f, r * 2);
				overlay->ellipse(at[i].first, at[i].second, r, r, way, 0.95f, 1.5f);
				if (path.reachable)
					overlay->text(std::to_wstring(path.points[i].turn), at[i].first, at[i].second - 8, 13, way, 1, true, Overlay::Align::Center);
			}
		}

		// The messages: centred lines under the top of the interface, the newest lowest.
		void drawMessages()
		{
			if (messages.empty())
				return;
			const float size = ui.textSize(2), line = std::round(size * 1.4f), cx = overlay->screenWidth() / 2;
			float y = ui.active() ? 74 : 70;
			for (const auto& [text, alpha] : messages)
			{
				overlay->richText(text, cx, y, size, false, { 0.93f, 0.93f, 0.9f }, alpha, Overlay::Align::Center);
				y += line;
			}
		}

		int actionAt(int x, int y) const
		{
			if (popup.active())
				return -1;
			for (size_t i = 0; i < actionRects.size(); ++i)
				if (const auto& r = actionRects[i]; float(x) >= r[0] && float(x) < r[2] && float(y) >= r[1] && float(y) < r[3])
					return int(i);
			return -1;
		}

		// The selected units' actions along the bottom, in rows from the bottom up; the help for
		// the one under the mouse above them.
		void drawActions()
		{
			actionRects.clear();
			if (actions.empty() || popup.active())
				return;
			const float size = 40, gap = 4, sw = overlay->screenWidth(), sh = overlay->screenHeight();
			const int perRow = std::max(1, int((sw - 2 * 340) / (size + gap)));
			const int rows = (int(actions.size()) + perRow - 1) / perRow;
			for (size_t i = 0; i < actions.size(); ++i)
			{
				const int row = int(i) / perRow, column = int(i) % perRow;
				const int inRow = std::min(perRow, int(actions.size()) - row * perRow);
				const float x0 = (sw - float(inRow) * (size + gap) + gap) / 2 + float(column) * (size + gap);
				const float y0 = sh - 12 - float(rows - row) * (size + gap);
				actionRects.push_back({ x0, y0, x0 + size, y0 + size });
				const bool hot = int(i) == actionAt(mouseX, mouseY);
				overlay->rect(x0 - 2, y0 - 2, x0 + size + 2, y0 + size + 2, { 0.05f, 0.06f, 0.09f }, 0.85f, 4);
				if (const ButtonArt::Icon* ic = art.icon(actions[i].icon))
					overlay->image(ic->key, ic->bgra, ic->width, ic->height, 1, x0, y0, x0 + size, y0 + size, actions[i].enabled ? 1.0f : 0.35f);
				overlay->frame(x0 - 2, y0 - 2, x0 + size + 2, y0 + size + 2, hot ? Color{ 1, 0.9f, 0.6f } : Color{ 0.45f, 0.4f, 0.3f }, hot ? 1.0f : 0.7f, 4,
					hot ? 2.0f : 1.0f);
			}
			const int hover = actionAt(mouseX, mouseY);
			if (!busy)
				askActionHelp();
			if (hover >= 0 && !actionHelpText.empty())
			{
				const float w = 380, h = overlay->richHeight(actionHelpText, w - 20, 13, false) + 16;
				const auto& r = actionRects[size_t(hover)];
				const float x = std::clamp(r[0] - w / 2, 8.0f, sw - w - 8), y = std::max(8.0f, actionRects.front()[1] - 10 - h);
				overlay->rect(x, y, x + w, y + h, { 0.05f, 0.06f, 0.09f }, 0.95f, 6);
				overlay->frame(x, y, x + w, y + h, { 0.78f, 0.64f, 0.36f }, 0.6f, 6, 1);
				overlay->richBlock(actionHelpText, x + 10, y + 8, w - 20, 13, false, { 0.92f, 0.93f, 0.95f });
			}
		}

		// The selected unit (or whatever the host shows) in the lower left corner.
		void drawInfo()
		{
			if (info.empty())
				return;
			std::vector<std::wstring> lines;
			for (size_t at = 0; at <= info.size();)
			{
				const size_t end = std::min(info.find(L'\n', at), info.size());
				lines.push_back(info.substr(at, end - at));
				at = end + 1;
			}
			const float size = 13, line = 17;
			float w = 0;
			for (const auto& l : lines)
				w = std::max(w, overlay->width(l, size, &l == &lines.front()));
			const float h = line * float(lines.size()) + 12, x = 10, y = overlay->screenHeight() - h - 40;
			overlay->rect(x, y, x + w + 20, y + h, { 0.04f, 0.05f, 0.07f }, 0.8f, 6);
			for (size_t i = 0; i < lines.size(); ++i)
				overlay->text(lines[i], x + 10, y + 6 + line * float(i), size, i == 0 ? Color{ 1, 0.92f, 0.7f } : Color{ 0.92f, 0.93f, 0.95f }, 1, i == 0);
		}

		// The game's help for the plot the mouse rests on: where the screens show their help (as the
		// game's, over the map without a box), else next to the mouse.
		void askActionHelp()
		{
			if (const int hover = actionAt(mouseX, mouseY); hover != helpAction)
			{
				helpAction = hover;
				actionHelpText = hover >= 0 && actionHelp ? widen(actionHelp(hover)) : std::wstring();
			}
		}

		bool plotHelpWanted(std::pair<int, int> plot)
		{
			return plotHelp && plot.first >= 0 && !panning && !orbiting && clock - restingSince >= 0.4 && !onMinimap(mouseX, mouseY) &&
				!(ui.active() && (ui.hovered() >= 0 || ui.solid(mouseX, mouseY)));
		}

		// (asked of the game between its updates; again now and then, as the units on it come and go)
		void askPlotHelp()
		{
			const auto plot = hoveredPlot();
			if (!plotHelpWanted(plot) || (plot == helpPlot && helpCombat == pathing && clock - helpAsked <= 1.0))
				return;
			helpPlot = plot;
			helpCombat = pathing;
			helpAsked = clock;
			plotHelpText = widen(plotHelp(plot.first, plot.second));
			if (pathing)
				return;
			if (plotHelpSeen.size() > 512)
				plotHelpSeen.clear();
			plotHelpSeen[plot] = plotHelpText;
		}

		void drawPlotHelp()
		{
			const auto plot = hoveredPlot();
			if (!plotHelpWanted(plot))
			{
				helpPlot = { -1, -1 };
				return;
			}
			if (!busy)
				askPlotHelp();
			else if (plot != helpPlot && !pathing)
			{
				if (const auto seen = plotHelpSeen.find(plot); seen != plotHelpSeen.end())
				{
					helpPlot = plot;
					helpCombat = false;
					plotHelpText = seen->second;
				}
			}
			if (plot != helpPlot || helpCombat != pathing)
				return;
			if (plotHelpText.empty())
				return;
			if (ui.active())
			{
				ui.drawPlotHelp(*overlay, plotHelpText);
				return;
			}
			const float size = 13, w = 340, h = overlay->richHeight(plotHelpText, w - 16, size, false) + 10;
			float x = float(mouseX) + 18, y = float(mouseY) + 18;
			x = std::min(x, overlay->screenWidth() - w - 8);
			y = std::min(y, overlay->screenHeight() - h - 8);
			overlay->rect(x, y, x + w, y + h, { 0.04f, 0.05f, 0.07f }, 0.85f, 5);
			overlay->frame(x, y, x + w, y + h, { 0.6f, 0.55f, 0.4f }, 0.8f, 5);
			overlay->richBlock(plotHelpText, x + 8, y + 5, w - 16, size, false, { 0.92f, 0.93f, 0.95f });
		}

		// The players by score, as the game's scoreboard.
		void drawScores()
		{
			if (view->empty())
				return;
			const auto& players = view->map().players;
			std::vector<size_t> order;
			for (size_t i = 0; i < players.size(); ++i)
				if (players[i].alive && players[i].cities > 0 && !players[i].barbarian)
					order.push_back(i);
			std::ranges::sort(order, std::greater<>(), [&](size_t i) { return players[i].score; });
			const float right = overlay->screenWidth() - 12, size = 12.5f, line = 17;
			float y = 10;
			float width = 0;
			for (const size_t i : order)
				width = std::max(width, overlay->width(widen(players[i].civilization), size, true) + 70);
			if (order.empty())
				return;
			overlay->rect(right - width - 12, y - 4, right + 4, y + line * float(order.size()) + 4, { 0.05f, 0.06f, 0.08f }, 0.65f, 6);
			for (const size_t i : order)
			{
				const PlayerView& p = players[i];
				overlay->rect(right - width - 4, y + 3, right - width + 8, y + 13, p.primary, 1, 2);
				overlay->text(widen(p.civilization), right - width + 14, y, size, { 1, 1, 1 }, 1, true);
				overlay->text(std::format(L"{}  ({})", p.score, p.cities), right, y, size, { 1, 0.92f, 0.7f }, 1, false, Overlay::Align::Right);
				y += line;
			}
		}

		// The minimap in the lower right corner, with what the camera sees on it.
		// The map in the box (keeping its shape, at the box's bottom right), with the ground the
		// camera sees outlined.
		void drawMinimap(float bx0, float by0, float bx1, float by1, bool background)
		{
			uint64_t version = 0;
			const auto& pixels = view->minimap(version);
			const auto& map = view->map();
			if (pixels.empty() || map.width <= 0 || bx1 <= bx0 || by1 <= by0)
				return;
			const float aspect = float(map.height) / float(map.width);
			const float w = std::min(bx1 - bx0, (by1 - by0) / aspect), h = w * aspect;
			const float x1 = bx1, y1 = by1, x0 = x1 - w, y0 = y1 - h;
			minimapRect[0] = x0, minimapRect[1] = y0, minimapRect[2] = x1, minimapRect[3] = y1;
			if (background)
				overlay->rect(x0 - 4, y0 - 4, x1 + 4, y1 + 4, { 0.05f, 0.06f, 0.08f }, 0.8f, 4);
			overlay->image(pixels, uint32_t(map.width), uint32_t(map.height), version, x0, y0, x1, y1);
			// The ground under the screen's corners.
			const Vec3 size = view->worldSize();
			const auto toMini = [&](Vec3 p) { return std::pair{ x0 + p.x / size.x * w, y1 - p.y / size.y * h }; };
			const int sw = int(overlay->screenWidth()), sh = int(overlay->screenHeight());
			const int sx[4] = { 0, sw - 1, sw - 1, 0 }, sy[4] = { 0, 0, sh - 1, sh - 1 };
			std::pair<float, float> c[4];
			for (int i = 0; i < 4; ++i)
			{
				const auto g = groundAt(sx[i], sy[i], camera.target.z);
				c[i] = toMini(g ? *g : camera.target);
			}
			for (int i = 0; i < 4; ++i)
				overlay->line(std::clamp(c[i].first, x0, x1), std::clamp(c[i].second, y0, y1), std::clamp(c[(i + 1) % 4].first, x0, x1),
					std::clamp(c[(i + 1) % 4].second, y0, y1), { 1, 1, 1 }, 0.9f, 1.2f);
		}

		// A screen's minimap, filling its place (its own map's proportions are the screen's).
		void drawScreenMinimap(float x0, float y0, float x1, float y1)
		{
			const ScreenMinimap& s = screenMinimap;
			const auto& map = view->map();
			const int W = s.width > 0 ? s.width : map.width, H = s.height > 0 ? s.height : map.height;
			if (W <= 0 || H <= 0 || x1 <= x0 || y1 <= y0)
				return;
			uint64_t mapVersion = 0;
			view->minimap(mapVersion);
			if (screenPixelsFor != std::pair{ s.version, mapVersion })
			{
				screenPixelsFor = { s.version, mapVersion };
				const bool sameMap = W == map.width && H == map.height;
				screenPixels = s.terrain && sameMap ? view->territoryMinimap(s.territory, s.wholeMap) : std::vector<uint8_t>();
				screenPixels.resize(size_t(W) * H * 4, 0);
				if (!s.terrain || !sameMap)
					for (size_t i = 3; i < screenPixels.size(); i += 4)
						screenPixels[i] = 255;
				for (size_t i = 0; i < s.colors.size() && i < size_t(W) * H; ++i)
					if (const uint32_t c = s.colors[i]; (c >> 24) != 0)
					{
						const size_t x = i % size_t(W), y = i / size_t(W);
						uint8_t* px = &screenPixels[(size_t(H - 1) - y) * size_t(W) * 4 + x * 4];
						const float a = float(c >> 24) / 255;
						const uint8_t r = uint8_t(c & 255), g = uint8_t((c >> 8) & 255), b = uint8_t((c >> 16) & 255);
						px[0] = uint8_t(px[0] + (b - px[0]) * a);
						px[1] = uint8_t(px[1] + (g - px[1]) * a);
						px[2] = uint8_t(px[2] + (r - px[2]) * a);
					}
			}
			overlay->rect(x0, y0, x1, y1, { 0, 0, 0 }, 1);
			overlay->image(1, screenPixels, uint32_t(W), uint32_t(H), (s.version << 24) ^ mapVersion, x0, y0, x1, y1);
			// (the flashing plots: a square that comes and goes, twice a second)
			const float pw = (x1 - x0) / float(W), ph = (y1 - y0) / float(H);
			const float on = 0.5f + 0.5f * std::sin(float(clock) * 6.283f * 1.5f);
			for (const auto& f : s.flashes)
			{
				const float fx = x0 + float(f.x) * pw, fy = y1 - float(f.y + 1) * ph;
				const Color c{ float(f.color & 255) / 255, float((f.color >> 8) & 255) / 255, float((f.color >> 16) & 255) / 255 };
				const float grow = std::max(2.0f, std::min(pw, ph));
				overlay->rect(fx - grow, fy - grow, fx + pw + grow, fy + ph + grow, c, on * 0.9f);
			}
		}

		bool onMinimap(int x, int y) const
		{
			return minimapRect[2] > minimapRect[0] && float(x) >= minimapRect[0] && float(x) <= minimapRect[2] && float(y) >= minimapRect[1] &&
				float(y) <= minimapRect[3];
		}

		void jumpFromMinimap(int x, int y)
		{
			const Vec3 size = view->worldSize();
			camera.target.x = (float(x) - minimapRect[0]) / (minimapRect[2] - minimapRect[0]) * size.x;
			camera.target.y = (minimapRect[3] - float(y)) / (minimapRect[3] - minimapRect[1]) * size.y;
			keepOnMap();
		}

		void frame(float dt)
		{
			// (the real time: dt is limited for the animations, and a stutter should show)
			const auto now = std::chrono::steady_clock::now();
			if (lastFrame != std::chrono::steady_clock::time_point{})
				frameStats.add(std::chrono::duration<double>(now - lastFrame).count());
			lastFrame = now;
			clock += dt;
			handleHeldKeys(dt);
			view->update(dt);
			for (auto& [nif, h] : heads)
				h.head->update(dt);
			for (auto& [key, m] : models)
				m.view->update(dt);
			render();
			renderer->present();
		}

		float windowDensity() const { return window ? SDL_GetWindowPixelDensity(window) : 1; }

		void updateUiScale() { overlay->setScale(uiScaleFor(uiPercent, windowDensity(), renderer->width(), renderer->height())); }

		void onEvent(const SDL_Event& e)
		{
			// (the window's points to the interface's units, which the mouse is in)
			const float density = SDL_GetWindowPixelDensity(window) / overlay->scale();
			const auto px = [&](float v) { return int(std::floor(v * density)); };
			switch (e.type)
			{
			case SDL_EVENT_KEY_DOWN:
				// (Alt+F4 closes, through SDL's close request)
				if (const unsigned k = keyOfSdl(e.key.key))
					onKey(k);
				break;
			case SDL_EVENT_TEXT_INPUT: // (typed into a popup's edit box)
				if (popup.active())
					for (const wchar_t c : widen(e.text.text))
						popup.character(c);
				break;
			case SDL_EVENT_MOUSE_MOTION: onMouseMove(px(e.motion.x), px(e.motion.y)); break;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
			{
				const bool down = e.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
				const int x = px(e.button.x), y = px(e.button.y);
				// (the middle button held turns the camera, as Ctrl with the right one)
				if (e.button.button == SDL_BUTTON_MIDDLE)
				{
					SDL_CaptureMouse(down);
					lastX = x, lastY = y;
					orbiting = down;
				}
				else if (e.button.button == SDL_BUTTON_LEFT || e.button.button == SDL_BUTTON_RIGHT)
					onButton(down && e.button.button == SDL_BUTTON_LEFT, down, x, y);
				break;
			}
			case SDL_EVENT_MOUSE_WHEEL:
				if (const int notches = e.wheel.integer_y * (e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1 : 1))
					onWheel(notches * 120);
				break;
			case SDL_EVENT_WINDOW_MOUSE_LEAVE: mouseX = -1; break;
			case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
				if (e.window.data1 > 0 && e.window.data2 > 0)
				{
					renderer->resize(uint32_t(e.window.data1), uint32_t(e.window.data2));
					updateUiScale();
				}
				break;
			case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			case SDL_EVENT_QUIT: open = false; break;
			}
		}

		void saveScreenshot(const std::filesystem::path& path)
		{
			render();
			uint32_t w = 0, h = 0;
			const auto pixels = renderer->readback(w, h);
			writePng(path, pixels, w, h);
		}
	};

	MapWindow::MapWindow(const AssetResolver& assets, uint32_t width, uint32_t height, bool offscreen)
		: m(std::make_unique<Impl>(assets, width, height, offscreen))
	{
	}

	MapWindow::~MapWindow() = default;

	bool MapWindow::pump()
	{
		for (SDL_Event e; m->window && SDL_PollEvent(&e);)
			m->onEvent(e);
		return m->open;
	}

	void MapWindow::setMap(const MapSnapshot& snapshot, int focusX, int focusY)
	{
		m->view->setMap(snapshot);
		if (!m->framed && !m->view->empty())
		{
			m->framed = true;
			if (focusX >= 0 && focusY >= 0)
				m->lookAt(focusX, focusY);
			else
				m->showWholeMap();
		}
	}

	void MapWindow::lookAt(int x, int y, float distanceInPlots) { m->lookAt(x, y, distanceInPlots); }
	void MapWindow::centerOn(int x, int y) { m->lookAt(x, y, m->camera.distance / m->plot()); }
	void MapWindow::frame(float dt) { m->frame(dt); }
	void MapWindow::saveScreenshot(const std::filesystem::path& png) { m->saveScreenshot(png); }
	void MapWindow::setScreenshotFolder(const std::filesystem::path& folder) { m->screenshotFolder = folder; }
	bool MapWindow::running() const { return m->running; }
	void MapWindow::setRunning(bool running)
	{
		m->running = running;
		m->updateTitle();
	}
	bool MapWindow::takeTurnRequest() { return std::exchange(m->turnRequest, false); }
	void MapWindow::setStatus(const std::string& text)
	{
		m->status = text;
		m->updateTitle();
	}
	std::pair<int, int> MapWindow::hoveredPlot() const { return m->hoveredPlot(); }
	void MapWindow::setPlotHelp(std::function<std::string(int x, int y)> help) { m->plotHelp = std::move(help); }
	void MapWindow::setPlaying(bool playing)
	{
		m->playing = playing;
		m->view->options.fog = playing; // you see what your team sees
		m->updateTitle();
	}
	std::vector<MapWindow::PlotClick> MapWindow::takeClicks() { return std::exchange(m->clicks, {}); }
	std::vector<MapWindow::KeyPress> MapWindow::takeKeys() { return std::exchange(m->keys, {}); }
	void MapWindow::setActions(const std::vector<ActionButton>& actions)
	{
		if (actions == m->actions)
			return;
		m->actions = actions;
		m->helpAction = -2; // (another unit's)
	}
	std::optional<int> MapWindow::takeActionClick() { return std::exchange(m->actionClick, std::nullopt); }
	void MapWindow::setActionHelp(std::function<std::string(int action)> help)
	{
		m->actionHelp = std::move(help);
		m->helpAction = -2;
	}
	void MapWindow::setInfo(const std::string& text) { m->info = widen(text); }
	void MapWindow::setScreenMinimap(ScreenMinimap minimap) { m->screenMinimap = std::move(minimap); }

	std::optional<std::pair<int, int>> MapWindow::pathTarget() const
	{
		// (OC4_PATH_AT=x,y: as if the button were held there, for a scripted game's screenshot)
		if (int px = 0, py = 0; std::getenv("OC4_PATH_AT") && std::sscanf(std::getenv("OC4_PATH_AT"), "%d,%d", &px, &py) == 2)
		{
			m->pathing = true;
			const auto p = m->plotUnder(px, py);
			return p.first >= 0 ? std::optional(p) : std::nullopt;
		}
		if (!m->pathing || m->popup.active())
			return std::nullopt;
		const auto p = m->hoveredPlot();
		return p.first >= 0 ? std::optional(p) : std::nullopt;
	}

	void MapWindow::setPath(PathView path) { m->path = std::move(path); }

	void MapWindow::triggerEffect(const std::string& nif, int x, int y, float fx, float fy, float degrees, float scale)
	{
		if (m->view)
			m->view->triggerEffect(nif, x, y, fx, fy, degrees, scale);
	}

	void MapWindow::setPlotMarks(PlotMarks marks) { m->plotMarks = std::move(marks); }

	void MapWindow::setLayers(Layers layers)
	{
		m->layers = layers;
		// (the grid and the bare map: the view's own toggles, when the game's change)
		if (layers.grid != m->layersApplied.grid)
			m->view->options.grid = layers.grid;
		if (layers.bare != m->layersApplied.bare)
			m->view->options.units = m->view->options.improvements = !layers.bare;
		m->layersApplied = layers;
	}

	void MapWindow::setGlobeView(bool up)
	{
		if (up == m->globeView)
			return;
		m->globeView = up;
		if (up)
		{
			m->globeFrom = m->camera.target;
			m->globeFromDistance = m->camera.distance;
			m->globeFromTilt = m->tilt;
			// (the globe from far enough to see it whole: its radius is the map's width once round)
			const Vec3 size = m->view->worldSize();
			m->camera.target = { m->camera.target.x, size.y / 2, 0 };
			m->camera.distance = size.x / (2 * 3.14159265f) * 3.4f;
			m->tilt = 0;
			m->updatePitch();
		}
		else
		{
			m->camera.target = m->globeFrom;
			m->camera.distance = m->globeFromDistance;
			m->tilt = m->globeFromTilt;
			m->updatePitch();
			m->keepOnMap();
		}
	}

	void MapWindow::setGraphics(Graphics g)
	{
		auto& o = m->view->options;
		o.healthBars = g.healthBars;
		o.frozen = g.frozen;
		o.effects = g.effects;
		setFullscreen(g.fullscreen);
	}

	void MapWindow::setFullscreen(bool on)
	{
		if (!m->window || on == m->fullscreen)
			return;
		m->fullscreen = on;
		// (borderless over the whole monitor the window is on)
		SDL_SetWindowFullscreen(m->window, on);
	}

	void MapWindow::setYieldSymbols(std::array<std::string, 3> symbols)
	{
		for (size_t i = 0; i < 3; ++i)
			m->yieldSymbols[i] = widen(symbols[i]);
	}

    void MapWindow::setWorldBuilder(bool on)
    {
		if (on == m->worldBuilder)
			return;
		m->worldBuilder = on;
		if (on)
		{
			m->fogBeforeWorldBuilder = m->view->options.fog;
			m->view->options.fog = false;
		}
		else
		{
			m->view->options.fog = m->fogBeforeWorldBuilder;
		}
    }

	void MapWindow::setGameFontLayout(std::vector<ButtonArt::FontGroup> groups) { m->art.setFontLayout(std::move(groups)); }

	void MapWindow::setCursorFile(const std::filesystem::path& file)
	{
		if (file == m->cursorFile)
			return;
		m->cursorFile = file;
		if (!m->window)
			return;
		auto [it, added] = m->cursors.try_emplace(file, nullptr);
		if (added && !file.empty())
			it->second = loadCursorFile(file);
		SDL_SetCursor(it->second ? it->second : SDL_GetDefaultCursor());
	}

	void MapWindow::mouseClick(int x, int y)
	{
		m->onMouseMove(x, y);
		m->onButton(true, true, x, y);
		m->onButton(false, false, x, y);
	}

	std::optional<MapWindow::PlotClick> MapWindow::clickAt(int x, int y, bool right) const
	{
		const auto figure = m->figureUnder(x, y);
		const auto p = figure ? *figure : m->plotUnder(x, y);
		if (p.first < 0)
			return std::nullopt;
		PlotClick c;
		c.x = p.first, c.y = p.second, c.right = right, c.onFigure = figure.has_value();
		return c;
	}
	void MapWindow::setMessages(std::vector<std::pair<std::string, float>> messages)
	{
		m->messages.clear();
		for (auto& [text, alpha] : messages)
			m->messages.push_back({ widen(text), alpha });
	}
	void MapWindow::setPopup(const PopupView* popup) { m->popup.set(popup); }
	std::optional<MapWindow::PopupAnswer> MapWindow::takePopupAnswer() { return m->popup.takeAnswer(); }
	void MapWindow::setBusy(bool busy) { m->busy = busy; }

	void MapWindow::askHelp()
	{
		m->askActionHelp();
		m->askPlotHelp();
	}
	void MapWindow::setEdgeScroll(bool on) { m->edgeScroll = on; }
	void MapWindow::setSoundHandler(std::function<void(const std::string& tag, float x, float y)> handler)
	{
		if (m->view)
			m->view->setSoundHandler(std::move(handler));
	}
	void MapWindow::setMovieSound(std::function<uint64_t(std::shared_ptr<const audio::Pcm>)> play, std::function<void(uint64_t)> stop)
	{
		m->playMovieSound = std::move(play);
		m->stopMovieSound = std::move(stop);
	}
	std::vector<std::string> MapWindow::takeMoviesDone() { return std::exchange(m->moviesDone, {}); }
	MapWindow::Look MapWindow::look() const
	{
		const float p = m->view ? m->view->plotSize() : 1;
		return { m->camera.target.x / p, m->camera.target.y / p, m->camera.distance / p };
	}
	std::vector<uint32_t> MapWindow::terrainColors() const { return m->view ? m->view->terrainColors() : std::vector<uint32_t>{}; }
	void MapWindow::setPopupHelp(std::function<std::string(int item)> help) { m->popupHelp = std::move(help); }
	void MapWindow::setUiWheel(std::function<bool(int x, int y, int delta)> wheel) { m->uiWheel = std::move(wheel); }
	void MapWindow::setUi(std::vector<UiItem> items) { m->ui.set(std::move(items)); }
	uint32_t MapWindow::width() const { return uint32_t(uiUnits(m->renderer->width(), m->overlay->scale())); }
	uint32_t MapWindow::height() const { return uint32_t(uiUnits(m->renderer->height(), m->overlay->scale())); }
	void MapWindow::setUiScale(int percent)
	{
		if (percent == m->uiPercent)
			return;
		m->uiPercent = percent;
		m->updateUiScale();
	}
	void MapWindow::setFrameRateShown(bool shown) { m->showFrameRate = shown; }
	void MapWindow::setUiTakesKeys(bool takes, bool mapBehind)
	{
		m->uiTakesKeys = takes;
		m->uiMapBehind = mapBehind;
		if (!takes)
			m->ui.clearKeyChoice(); // (the next screen starts with the arrows the map's again)
	}
	void MapWindow::clearKeyChoice() { m->ui.clearKeyChoice(); }
	void MapWindow::setWasdPan(bool on) { m->wasdPan = on; }
	bool MapWindow::wasdPan() const { return m->wasdPan; }
	std::optional<bool> MapWindow::takeWasdPanToggle() { return std::exchange(m->wasdToggled, std::nullopt); }
	void MapWindow::pressKey(unsigned key) { m->onKey(key); }
	MapWindow::UiDisplay MapWindow::uiDisplay() const { return { m->renderer->width(), m->renderer->height(), m->windowDensity() }; }
	bool MapWindow::uiActive() const { return m->ui.active(); }
	std::vector<MapWindow::UiClick> MapWindow::takeUiClicks() { return std::exchange(m->uiClicks, {}); }
	int MapWindow::uiHovered() const { return m->ui.hovered(); }
	void MapWindow::hoverUi(int item) { m->ui.hoverItem(item); }
	int MapWindow::textWidth(const std::string& markup, int font) const
	{
		const float size = font >= 0 ? m->ui.textSize(font) : m->theme.loaded() ? m->theme.font(2).size : 14;
		return int(std::ceil(m->overlay->richWidth(widen(markup), size, font == 0 || font == 4)));
	}
	int MapWindow::textHeight(const std::string& markup, int width, int font) const
	{
		return int(std::ceil(m->overlay->richHeight(widen(markup), float(std::max(20, width)), m->ui.textSize(font), font == 0 || font == 4)));
	}
	std::pair<int, int> MapWindow::mousePosition() const { return { m->mouseX, m->mouseY }; }
	std::string MapWindow::cameraText() const
	{
		const float p = m->plot();
		return std::format("camera at {:.1f},{:.1f} (height {:.0f}), {:.1f} plots away, pitch {:.2f}", m->camera.target.x / p, m->camera.target.y / p,
			m->camera.target.z, m->camera.distance / p, m->camera.pitch);
	}
	void MapWindow::setUiHelp(const std::string& text, float x, float y, float width, bool expandRight, bool expandDown)
	{
		m->ui.setHelp(text, x, y, width, expandRight, expandDown);
	}
	void MapWindow::pressKeys(const std::string& keys)
	{
		for (const char c : keys)
			if (c != 'S' && c != 's') // (not a screenshot of its own)
				m->onKey(unsigned(std::toupper(static_cast<unsigned char>(c))));
	}
	const std::vector<std::string>& MapWindow::problems() const { return m->view->problems(); }
}
