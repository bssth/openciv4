#pragma once

// The game's popups over the map: a window in the theme's look (its frame and glass, its
// standard buttons) with the popup's header, texts, pictures and buttons (their art cut from
// the interface's button atlases), radio buttons and checkboxes, a scrolling list when it is
// long, and an OK button when the popup has one.
// The help for the item under the mouse comes from the host, next to the panel.

#include "render/ButtonArt.h"
#include "render/MapWindow.h"
#include "render/Overlay.h"
#include "render/Theme.h"

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace oc4::render
{
	class PopupPanel
	{
	public:
		PopupPanel(ButtonArt& art, Theme& theme) : m_art(art), m_theme(theme) {}

		void set(const MapWindow::PopupView* view);
		bool active() const { return m_view.has_value(); }
		bool contains(int x, int y) const { return active() && m_panel.contains(float(x), float(y)); }

		// Input in window pixels. While it is up the panel takes every click and key; the
		// wheel only over it. The arrows go through what can be chosen, and Return does it.
		void hover(int x, int y);
		bool click(int x, int y);
		bool wheel(int x, int y, int delta);
		bool key(unsigned virtualKey);
		// A character typed (WM_CHAR): into the edit box that has the focus.
		bool character(wchar_t c);
		std::optional<MapWindow::PopupAnswer> takeAnswer() { return std::exchange(m_answer, std::nullopt); }

		void draw(Overlay& overlay, const std::function<std::string(int item)>& help);
		// The keys have chosen something (Tab started it): the arrows are the popup's, not the map's.
		bool keyboardChoice() const { return m_keyboard; }
		// An edit box has the focus: typing goes into it.
		bool editing() const { return active() && m_focus >= 0; }

	private:
		struct Rect
		{
			float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
			bool contains(float x, float y) const { return x >= x0 && x < x1 && y >= y0 && y < y1; }
		};
		void answer(int item);
		void choose(int item); // as a click on it
		void moveChoice(int delta);

		ButtonArt& m_art;
		Theme& m_theme;
		bool m_skinsRead = false;
		Theme::Skin m_glass, m_frame, m_tooltip; // the theme's window and tooltip (empty: our own look)
		const Theme::Button* m_button = nullptr; // its standard button
		float m_headerSize = 22, m_textSize = 16, m_helpSize = 14; // its fonts
		std::optional<MapWindow::PopupView> m_view;
		std::vector<bool> m_checked; // per item: the radio button chosen, the checkbox ticked
		std::vector<std::wstring> m_edits; // per item: an edit box's text
		int m_focus = -1;                  // the edit box typed into
		std::optional<MapWindow::PopupAnswer> m_answer;
		bool m_answered = false;
		float m_scroll = 0, m_scrollMax = 0;
		Rect m_panel, m_list, m_ok;
		std::vector<Rect> m_items; // where each item was drawn (empty: not shown)
		int m_hover = -1, m_helpItem = -2;
		bool m_hoverOk = false;
		bool m_keyboard = false; // what m_hover says was chosen with the keys, not the mouse
		int m_scrollTo = -1;     // the item to bring into the list at the next draw
		std::wstring m_help;
		int m_mouseX = -1, m_mouseY = -1;
	};
}
