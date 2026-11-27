#pragma once

// The game's Python screens over the map: draws the items the host gives (panels, pictures,
// buttons, texts, bars, tables, the minimap's place), finds what is under the mouse and
// turns presses on interactive items into clicks for the host. Panels and buttons look as
// the game's theme has their styles; one it does not know has a plain look of its own.

#include "render/ButtonArt.h"
#include "render/KeyNav.h"
#include "render/MapWindow.h"
#include "render/Overlay.h"
#include "render/Theme.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oc4::render
{
	class UiLayer
	{
	public:
		UiLayer(ButtonArt& art, Theme& theme) : m_art(art), m_theme(theme) {}

		void set(std::vector<MapWindow::UiItem> items);
		bool active() const { return !m_items.empty(); }
		const std::vector<MapWindow::UiItem>& items() const { return m_items; }

		// The interactive item under the mouse, -1 none.
		int hit(int x, int y) const;
		// Whether the mouse is on something of the screens (a click there is theirs, not the map's).
		bool solid(int x, int y) const;
		// The same, the main interface left out: a screen of its own (the original's popups leave
		// the main interface no clicks, the screens over it theirs).
		bool solid(int x, int y, bool mainInterface) const;
		// Whether the main interface's minimap is what the mouse is on (no button nor screen over it).
		bool onMainMinimap(int x, int y) const;
		void hover(int x, int y);
		int hovered() const { return m_hover; }
		void hoverItem(int item) // (as if the mouse were on it)
		{
			m_hover = item >= 0 && size_t(item) < m_items.size() ? item : -1;
			m_mouseX = m_mouseY = -1;
		}
		// The keyboard's choice, where the mouse is not: the nearest thing that can be used that
		// way (round the far side with `wrap`); false if there is nothing. Moving the mouse ends it.
		// (`within`: only among what has its middle there, the main interface's widgets too: the
		// gamepad's walk along the unit's action buttons)
		bool moveChoice(NavDirection direction, bool wrap, const NavRect* within = nullptr);
		bool keyChoice() const { return m_keyHover && m_hover >= 0; }
		void clearKeyChoice() { m_keyHover = false; }
		// What it chose, about to be used: an open pull down's item hands the choice back to its box.
		int useKeyChoice();
		// The middle of what the keyboard has chosen.
		std::pair<int, int> keyChoicePlace() const;
		// A press and its release on the same item make a click.
		void press(int x, int y, bool right);
		// The item pressed and not let go yet (found again when the items are made again), -1 none.
		int pressed() const { return m_pressed; }
		std::optional<MapWindow::UiClick> release(int x, int y, bool shift, bool ctrl, bool alt);
		// A scroll bar pressed with the left button (after press): its arrows step a line and its
		// shaft a page, again and again while held (the theme's 250 ms, then every 90 ms; the
		// shaft until the grip reaches the mouse); its grip follows the mouse, wherever it goes,
		// until the button is let go. holdBar: the mouse moved or time passed.
		// What that asks of the screens is taken with takeBarClick (the latest: it stays with the
		// bar when the screens are made again before it is taken).
		void pressBar(int x, int y);
		void holdBar(int x, int y);
		void releaseBar() { m_barPart = BarPart::None; }
		bool barHeld() const { return m_barPart != BarPart::None; }
		std::optional<MapWindow::UiClick> takeBarClick() { return std::exchange(m_barClick, std::nullopt); }

		// framed: the help area has art of its own (the city screen's), in the theme's tooltip;
		// else the main interface's see-through box (PANEL_STYLE_HUD_HELP).
		// fit: as wide as its text, up to width (a tooltip at the widget, the World Builder's).
		void setHelp(const std::string& text, float x, float y, float width, bool expandRight, bool expandDown, bool framed = false, bool fit = false, bool floating = false);
		// The size of a widget's font (FontTypes) as the screens draw it.
		float textSize(int font) const { return fontSize(font); }
		// The plot's help where the screens show theirs, in the same box (the game's look).
		void drawPlotHelp(Overlay& overlay, const std::wstring& text);
		// minimap(x0, y0, x1, y1, screen) draws the map's minimap there (screen: a screen's own, the
		// military advisor's); leaderhead(item) a leader's head
		// (false: it has none, its button shows).
		void draw(Overlay& overlay, const std::function<void(float, float, float, float, bool)>& minimap, double clock,
			const std::function<bool(const MapWindow::UiItem&)>& leaderhead = {},
			const std::function<void()>& underScreens = {}); // (drawn over the main interface, under the screens)

	private:
		using Item = MapWindow::UiItem;
		enum class BarPart { None, Back, Forward, PageBack, PageForward, Grip };
		// A scroll bar's parts along it: its ends, its shaft's (between its arrows) and its grip's.
		struct BarParts
		{
			bool down = true; // (up and down; else across)
			float a0 = 0, t0 = 0, t1 = 0, a1 = 0, g0 = 0, g1 = 0;
		};
		static BarParts barParts(const Item& it);
		BarPart barPartAt(const Item& it, int x, int y) const;
		void barStep(int x, int y);
		bool inside(const Item& item, int x, int y) const;
		// What can be reached now: nothing clipped away or covered by a screen on top.
		std::vector<int> reachable() const;
		// The theme's look for the item's style, in its state (toggled: a checkbox that is on);
		// false if the theme has no such button.
		bool themedButton(Overlay& o, const Item& it, int index, bool hot, bool toggled);
		const Theme::Panel* themedPanel(const Item& it) const;

		// A widget's FontTypes (TITLE, GAME, SMALL, MENU, MENU_HIGHLIGHT) as the theme's sizes.
		float fontSize(int font) const;
		static bool fontBold(int font) { return font == 0 || font == 4; }

		ButtonArt& m_art;
		Theme& m_theme;
		std::map<std::string, Theme::Skin, std::less<>> m_namedSkins; // panels styled "@style|property": that skin of the theme
		bool m_fontsRead = false;
		Theme::Skin m_tooltip; // the theme's (the framed help's box)
		Theme::Skin m_hudHelp; // the main interface's help box (PANEL_STYLE_HUD_HELP: see-through)
		// A multi list's button (the unit's actions, the city's builds): the theme's MultiList_Item
		// skins over its picture: its ring (HUDBorder), the glow under the mouse (added), its fill
		// when pressed, darker when disabled, and the glow round a pulsing one.
		Theme::Skin m_listBorder, m_listBorderGlow, m_listFill, m_listFillGlow, m_listDisable;
		Theme::Skin m_listPress, m_pulseInside, m_pulseDeep, m_pulseLight, m_pulseHalo;
		void drawListButton(Overlay& o, const Item& it, bool hot, bool pressed, double clock);
		// The theme's pull down (GFC_Control_StandardComboBox_Style): its box and its button, plain
		// and under the mouse; its open list's box, frame and chosen item.
		std::array<Theme::Skin, 2> m_comboBox, m_comboButton;
		Theme::Skin m_dropInner, m_dropBorder, m_dropSelect;
		// The theme's scroll bars (GFC_Control_StandardScroll_Style): the shaft, up and down and
		// across, and the grip, plain and under the mouse.
		// The theme's sliders (GFC_Control_StandardSlider_Style), up and down [0] and across [1]:
		// the track, the track with its fill, plain and disabled, and the grip plain, under the
		// mouse, pressed and disabled.
		std::array<std::array<Theme::Skin, 2>, 2> m_sliderTrack, m_sliderFill;
		std::array<std::array<Theme::Skin, 4>, 2> m_sliderGrip;
		// Its arrows (up, down, left, right) plain, under the mouse and pressed. The open pull
		// down's list has the combo box's own (SF_CtrlTheme_Civ4_Control_ComboBox_Standard_Scroll_Style):
		// look 1. Shaft: plain and pressed, up and across; grip: plain, under the mouse and pressed.
		struct BarLook
		{
			std::array<std::array<Theme::Skin, 2>, 2> shaft;
			std::array<std::array<Theme::Skin, 3>, 2> grip;
			std::array<std::array<Theme::Skin, 3>, 4> arrow;
		};
		std::array<BarLook, 2> m_bar;
		BarPart m_barPart = BarPart::None;
		float m_barGrab = 0; // (where along the grip it was taken)
		std::optional<MapWindow::UiClick> m_barClick;
		std::chrono::steady_clock::time_point m_barNext; // (the next step while an arrow or the shaft is held)
		std::array<float, 5> m_sizes{ 10, 12, 14, 16, 22 }; // the markup's <font=0..4> (the theme's)
		std::vector<Item> m_items;
		int m_hover = -1, m_pressed = -1;
		bool m_keyHover = false; // m_hover is the keyboard's
		std::optional<NavRect> m_keyWithin; // (the keys choose only in there, and from the main interface too)
		Item m_keyItem;          // (to find it again when the items are made again)
		Item m_pressedItem;          // (to find it again when the items are made again)
		int m_mouseX = -1, m_mouseY = -1;
		bool m_pressedRight = false;
		std::wstring m_help;
		float m_helpX = 0, m_helpY = 0, m_helpWidth = 350;
		bool m_helpRight = true, m_helpDown = false, m_helpFramed = false, m_helpFit = false;
		bool m_helpFloating = false; // (the help area floats: the help goes beside the widget under the mouse)
		// A help in the help area: its box, then its text.
		void drawHelp(Overlay& o, const std::wstring& text);
		std::wstring m_besideFor; // (the help m_besideWidth was measured for)
		float m_besideWidth = 0;
	};
}
