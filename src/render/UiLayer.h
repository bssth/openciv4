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
		std::optional<MapWindow::UiClick> release(int x, int y, bool shift, bool ctrl, bool alt);

		void setHelp(const std::string& text, float x, float y, float width, bool expandRight, bool expandDown);
		// The size of a widget's font (FontTypes) as the screens draw it.
		float textSize(int font) const { return fontSize(font); }
		// The plot's help where the screens show theirs, over the map without a box (the game's look).
		void drawPlotHelp(Overlay& overlay, const std::wstring& text);
		// minimap(x0, y0, x1, y1, screen) draws the map's minimap there (screen: a screen's own, the
		// military advisor's); leaderhead(item) a leader's head
		// (false: it has none, its button shows).
		void draw(Overlay& overlay, const std::function<void(float, float, float, float, bool)>& minimap, double clock,
			const std::function<bool(const MapWindow::UiItem&)>& leaderhead = {});

	private:
		using Item = MapWindow::UiItem;
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
		Theme::Skin m_tooltip; // the theme's (the help's box)
		// The theme's pull down (GFC_Control_StandardComboBox_Style): its box and its button, plain
		// and under the mouse; its open list's box, frame and chosen item.
		std::array<Theme::Skin, 2> m_comboBox, m_comboButton;
		Theme::Skin m_dropInner, m_dropBorder, m_dropSelect;
		// The theme's scroll bars (GFC_Control_StandardScroll_Style): the shaft, up and down and
		// across, and the grip, plain and under the mouse.
		std::array<Theme::Skin, 2> m_shaft;
		std::array<std::array<Theme::Skin, 2>, 2> m_grip;
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
		bool m_helpRight = true, m_helpDown = false;
	};
}
