#include "render/PopupPanel.h"
#include "render/Input.h"
#include "render/KeyNav.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace oc4::render
{
	namespace
	{
		using Kind = MapWindow::PopupView::Item::Kind;

		std::wstring widen(std::string_view s)
		{
			if (s.empty())
				return {};
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
			std::wstring w(size_t(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
			return w;
		}

		// (as the game's popups: their width, margins, the choices' pictures, the OK button's height)
		constexpr float kWidth = 400, kPadX = 21, kPadTop = 18, kPadBottom = 20, kIcon = 28, kOk = 24;
		constexpr float kStoneLine = 25, kStoneGap = 6, kStoneInset = 9; // (a choice's stone button)
		// A choice with a picture is the theme's label button instead (GFC_Control_LabelButton_Style:
		// no stone; Wine at 1600 x 900 and 1280 x 1000 alike): its picture 30 square 2 in, the text 4
		// after it, one every 36 (the picture, the button's margin 2 round it, the item space 2), the
		// content 1 lower while pressed. And a popup is never higher than the original's: what is
		// longer scrolls, its title too (Wine's rim 77-553 at both sizes, its light line at 550: our
		// frame's is 7 over its bottom, so 480), wrapped short of the scroll bar.
		constexpr float kLabelIcon = 30, kLabelRow = 36, kLabelInset = 2, kLabelGap = 4, kMaxHeight = 480, kScrollRoom = 20;
		// The game menu (Window_NoTitleBar_Style): 248 wide, where the other popups are (Wine at
		// 1600 x 900 and 1280 x 1000 alike: its rim 1334-1582 and 1014-1262, from 77); its separator
		// only a space (no line: the first button 10 under the title).
		constexpr float kMenuWidth = 248, kMenuSeparator = 7;
		constexpr Color kFocus{ 0.45f, 0.62f, 0.95f };                  // (the focus rim on the first)
		constexpr Color kPanel{ 0.05f, 0.06f, 0.09f }, kEdge{ 0.78f, 0.64f, 0.36f }, kText{ 0.97f, 0.97f, 0.97f }, kHot{ 1, 0.83f, 0.49f };
		constexpr Color kButton{ 0.13f, 0.16f, 0.22f }, kButtonHover{ 0.24f, 0.31f, 0.44f }, kEditFill{ 0.09f, 0.11f, 0.2f }, kEditRim{ 0.62f, 0.72f, 0.9f };

		constexpr Color kKeyChoice{ 1, 0.9f, 0.6f };

		bool chooses(Kind k) { return k == Kind::Button || k == Kind::Radio || k == Kind::Check || k == Kind::Edit; }

		std::string narrow(const std::wstring& w)
		{
			if (w.empty())
				return {};
			const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
			std::string s(size_t(n), '\0');
			WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
			return s;
		}
	}

	void PopupPanel::set(const MapWindow::PopupView* view)
	{
		if (!view)
		{
			m_view.reset();
			m_items.clear();
			m_hover = -1;
			return;
		}
		if (m_view && m_view->serial == view->serial)
			return;
		m_view = *view;
		m_checked.assign(view->items.size(), false);
		m_edits.assign(view->items.size(), std::wstring());
		m_focus = -1;
		for (size_t i = 0; i < view->items.size(); ++i)
		{
			m_checked[i] = view->items[i].checked;
			if (view->items[i].kind == Kind::Edit)
			{
				m_edits[i] = widen(view->items[i].text);
				if (m_focus < 0)
					m_focus = int(i); // (the first one has the focus)
			}
		}
		m_items.clear();
		m_answered = false;
		m_scroll = 0;
		m_hover = -1;
		m_helpItem = -2;
		m_hoverOk = false;
		m_keyboard = false;
		m_scrollTo = -1;
		m_pressed = -1;
		// (the focus starts on the first choice, as the original's: the gold one)
		m_selected = -1;
		for (size_t i = 0; i < view->items.size() && m_selected < 0; ++i)
			if (view->items[i].kind == Kind::Button)
				m_selected = int(i);
	}

	bool PopupPanel::label(int item) const
	{
		return m_view && item >= 0 && size_t(item) < m_view->items.size() && m_view->items[size_t(item)].kind == Kind::Button &&
			!m_view->items[size_t(item)].icon.empty() && m_label != nullptr && m_label->coloured;
	}

	void PopupPanel::hover(int x, int y)
	{
		m_mouseX = x;
		m_mouseY = y;
		m_hover = -1;
		m_hoverOk = false;
		m_keyboard = false;
		if (!m_view)
			return;
		const float fx = float(x), fy = float(y);
		m_hoverOk = m_view->okButton && m_ok.contains(fx, fy);
		if (!m_list.contains(fx, fy))
			return;
		for (size_t i = 0; i < m_items.size(); ++i)
			if (m_items[i].contains(fx, fy) && chooses(m_view->items[i].kind))
				m_hover = int(i);
		// (a choice under the mouse takes the focus, and keeps it when the mouse leaves)
		if (m_hover >= 0 && m_view->items[size_t(m_hover)].kind == Kind::Button)
			m_selected = m_hover;
	}

	void PopupPanel::press(int x, int y)
	{
		hover(x, y);
		m_pressed = m_hover;
	}

	void PopupPanel::release()
	{
		m_pressed = -1;
	}

	bool PopupPanel::click(int x, int y)
	{
		if (!m_view)
			return false;
		hover(x, y);
		const int pressed = std::exchange(m_pressed, -1);
		if (m_answered)
			return true;
		if (pressed >= 0 && pressed != m_hover) // (let go off the choice pressed: nothing, as the original's)
			return true;
		if (m_hoverOk)
			answer(-1);
		else if (m_hover >= 0)
			choose(m_hover);
		return true;
	}

	void PopupPanel::choose(int index)
	{
		const auto& item = m_view->items[size_t(index)];
		if (item.kind == Kind::Button)
			answer(index);
		else if (item.kind == Kind::Check)
			m_checked[size_t(index)] = !m_checked[size_t(index)];
		else if (item.kind == Kind::Edit)
			m_focus = index;
		else if (item.kind == Kind::Radio)
			for (size_t i = 0; i < m_view->items.size(); ++i)
				if (m_view->items[i].kind == Kind::Radio && m_view->items[i].group == item.group)
					m_checked[i] = int(i) == index;
	}

	void PopupPanel::moveChoice(int delta)
	{
		// The stops: what can be chosen, then the OK button.
		constexpr int kOkStop = -2;
		std::vector<int> stops;
		for (size_t i = 0; i < m_view->items.size(); ++i)
			if (chooses(m_view->items[i].kind))
				stops.push_back(int(i));
		if (m_view->okButton)
			stops.push_back(kOkStop);
		// (the keys go on from the focus, the gold choice, when it is a label button)
		const int current = m_keyboard ? (m_hoverOk ? kOkStop : m_hover) : label(m_selected) ? m_selected : -1;
		const auto at = std::ranges::find(stops, current);
		const int next = navStep(int(stops.size()), m_keyboard && at != stops.end() ? int(at - stops.begin()) : -1, delta);
		if (next < 0)
			return;
		const int stop = stops[size_t(next)];
		m_keyboard = true;
		m_hoverOk = stop == kOkStop;
		m_hover = m_hoverOk ? -1 : stop;
		if (m_hover >= 0 && m_view->items[size_t(m_hover)].kind == Kind::Button)
			m_selected = m_hover;
		m_scrollTo = m_hover;
		if (m_hover >= 0 && m_view->items[size_t(m_hover)].kind == Kind::Edit)
			m_focus = m_hover;
	}

	bool PopupPanel::wheel(int x, int y, int delta, bool anywhere)
	{
		if (!active() || (!anywhere && !contains(x, y)))
			return false;
		m_scroll = std::clamp(m_scroll - float(delta) / 120 * 3 * 36, 0.0f, m_scrollMax);
		hover(x, y);
		return true;
	}

	bool PopupPanel::key(unsigned virtualKey)
	{
		if (!m_view)
			return false;
		if (m_answered)
			return true;
		const bool chosenWithKeys = m_keyboard && m_hover >= 0 && m_view->items[size_t(m_hover)].kind != Kind::Edit;
		if (virtualKey == key::Escape && !m_view->escape)
			return true; // (the original's questions stay: what to build, research, a new civic)
		if (virtualKey == key::Escape)
		{
			m_answer = MapWindow::PopupAnswer{ .dismissed = true };
			m_answered = true;
		}
		else if (virtualKey == key::Up || virtualKey == key::Left)
			moveChoice(-1);
		else if (virtualKey == key::Down || virtualKey == key::Right || virtualKey == key::Tab)
			moveChoice(1);
		else if (virtualKey == key::Return)
		{
			if (chosenWithKeys)
				choose(m_hover);
			else if (label(m_selected)) // (the gold choice, as the original's)
				choose(m_selected);
			else if (m_view->okButton)
				answer(-1);
		}
		else if (virtualKey == key::Space && chosenWithKeys && m_focus < 0) // (a space is text in an edit box)
			choose(m_hover);
		else if (virtualKey == key::Space && label(m_selected) && m_focus < 0) // (the gold choice, as Return)
			choose(m_selected);
		return true;
	}

	bool PopupPanel::character(wchar_t c)
	{
		if (!m_view || m_answered || m_focus < 0 || size_t(m_focus) >= m_edits.size())
			return false;
		std::wstring& text = m_edits[size_t(m_focus)];
		if (c == L'\b')
		{
			if (!text.empty())
				text.pop_back();
		}
		else if (c >= L' ' && c != 0x7F && text.size() < 256)
			text += c;
		return true;
	}

	void PopupPanel::answer(int item)
	{
		MapWindow::PopupAnswer a;
		a.item = item;
		for (size_t i = 0; i < m_view->items.size(); ++i)
		{
			if (m_view->items[i].kind == Kind::Edit)
				a.edits.push_back({ int(i), narrow(m_edits[i]) });
			else if (m_checked[i])
				(m_view->items[i].kind == Kind::Radio ? a.radio : a.checked).push_back(int(i));
		}
		m_answer = std::move(a);
		m_answered = true;
	}

	void PopupPanel::draw(Overlay& o, const std::function<std::string(int)>& help)
	{
		if (!m_view)
			return;
		if (!m_skinsRead && m_theme.loaded())
		{
			// The theme's window without a title bar, as the game's popups are; its standard button
			// (the OK); its tooltip; its fonts (the header's size 4, the rest 3, the help 2).
			m_skinsRead = true;
			m_glass = m_theme.skin("GFC_Control_DefaultWindow_Style", "GFC_CtrlTheme_Bitmap_Control_Window_Skin_Inner_Default");
			m_frame = m_theme.skin("SF_CtrlTheme_Civ4_Control_Window_NoTitleBar_InFrameBorder_Normal_Style", "GFC_CtrlTheme_Bitmap_Control_Border_Skin_Full_Default");
			m_tooltip = m_theme.skin("GFC_Control_StandardToolTip_Style", "GFC_CtrlTheme_Bitmap_Control_ToolTip_Skin_Inner_Default");
			m_button = m_theme.button("BUTTON_STYLE_STANDARD");
			m_label = m_theme.button("BUTTON_STYLE_LABEL");
			m_shaft = m_theme.skin("GFC_Control_StandardScroll_Style", "GFC_CtrlTheme_Bitmap_Control_Scroll_Skin_VShaft_Default");
			m_gripSkin = m_theme.skin("SF_CtrlTheme_Civ4_Control_Scroll_VGripButton_Style", "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Default");
			m_headerSize = m_theme.font(4).size;
			m_textSize = m_theme.font(3).size;
			m_helpSize = m_theme.font(2).size;
		}
		const auto& items = m_view->items;
		const float sw = o.screenWidth(), sh = o.screenHeight();
		// As the game's: its width, at the right under the top bar (in the middle when narrow). The
		// game menu too, narrower.
		const bool menu = m_view->menu;
		const float w = std::min(menu ? kMenuWidth : kWidth, sw - 40), inner = w - 2 * kPadX;
		const bool right = sw >= 2.5f * w;
		const float x0 = right ? sw - 18 - w : (sw - w) / 2; // (Wine at 1600 x 900: its rim 1182-1582 from 77; 1280 wide, the game menu's at 1262)
		const std::wstring header = widen(m_view->header);
		std::vector<std::wstring> texts(items.size());
		std::vector<float> heights(items.size());
		const bool stone = m_button && !m_button->skin[0].empty();
		float content = 0;
		for (size_t i = 0; i < items.size(); ++i)
		{
			texts[i] = widen(items[i].text);
			const bool icon = items[i].kind == Kind::Button && m_art.icon(items[i].icon);
			switch (items[i].kind)
			{
			case Kind::Text: heights[i] = o.richHeight(texts[i], inner, m_textSize, false) + 10; break;
			case Kind::Button:
				if (label(int(i)))
				{
					heights[i] = std::max(kLabelRow, o.richHeight(texts[i], inner - kLabelInset - kLabelIcon - kLabelGap, m_textSize, false) + 2 * kLabelInset + 2);
					break;
				}
				// (on the theme's stone button, as the original's: 25 high for a line, the text's
				// height and 3 for more (two lines of the game font: 47), 6 apart, the text 9 in;
				// measured in Wine at 1600 x 900)
				heights[i] = stone ? std::max(kIcon * (icon ? 1.0f : 0.0f), std::max(kStoneLine, o.richHeight(texts[i], inner - 2 * kStoneInset - (icon ? kIcon + 5 : 0), m_textSize, false) + 3)) + kStoneGap
					: std::max(kIcon, o.richHeight(texts[i], inner - (icon ? kIcon + 5 : 0), m_textSize, false)) + 8;
				break;
			case Kind::Radio:
			case Kind::Check: heights[i] = std::max(26.0f, o.richHeight(texts[i], inner - 30, m_textSize, false) + 8); break;
			case Kind::Separator: heights[i] = menu ? kMenuSeparator : 12; break;
			case Kind::Image: heights[i] = items[i].height > 0 ? items[i].height + 4 : 72; break; // (at the size asked, 4 down)
			case Kind::Edit: heights[i] = 34; break;
			}
			content += heights[i];
		}
		float headerWidth = inner;
		float headerHeight = header.empty() ? 0.0f : o.richHeight(header, headerWidth, m_headerSize, false) + 10;
		const float okHeight = m_view->okButton ? kOk + 10 : 0.0f;
		float y0 = right ? 77.0f : 30.0f;
		// (the original leaves 19 under a last stone button to the rim: 3 more than the margin)
		const float tail = stone && !items.empty() && items.back().kind == Kind::Button && !m_view->okButton ? 3.0f : 0.0f;
		// (no higher than the original's 477, nor than the screen: what does not fit scrolls, the
		// title with the rest)
		const float room = std::min(kMaxHeight, sh - y0 - 20) - okHeight - kPadTop - kPadBottom - tail;
		const bool titleScrolls = headerHeight + content > room;
		if (titleScrolls && !header.empty())
		{
			headerWidth = inner - kScrollRoom;
			headerHeight = o.richHeight(header, headerWidth, m_headerSize, false) + 10;
		}
		const float scrolled = content + (titleScrolls ? headerHeight : 0.0f);
		const float listHeight = std::max(0.0f, std::min(scrolled, room - (titleScrolls ? 0.0f : headerHeight)));
		const float h = kPadTop + (titleScrolls ? 0.0f : headerHeight) + listHeight + okHeight + kPadBottom + tail;
		if (!right)
			y0 = std::max(30.0f, (sh - h) / 2);
		m_panel = { x0, y0, x0 + w, y0 + h };
		if (!m_glass.empty() || !m_frame.empty())
		{
			// (the glass inside the border's margin, GFC_Control_Border_Size_Margin of the window
			// without a title bar: the border's own sides, filled with the window's colour, round it;
			// the glass's light top then follows on from the border's, as the original's)
			Theme::draw(o, m_glass, x0 + 12, y0 + 13, x0 + w - 12, y0 + h - 15);
			Theme::draw(o, m_frame, x0, y0, x0 + w, y0 + h);
		}
		else
		{
			o.rect(x0, y0, x0 + w, y0 + h, kPanel, 0.95f, 8);
			o.frame(x0, y0, x0 + w, y0 + h, kEdge, 0.9f, 8, 1.5f);
		}

		float y = y0 + kPadTop;
		if (!header.empty() && !titleScrolls)
		{
			o.richBlock(header, x0 + kPadX, y, inner, m_headerSize, false, kText);
			y += headerHeight;
		}
		m_list = { x0 + kPadX - 4, y, x0 + w - kPadX + 4, y + listHeight };
		m_scrollMax = std::max(0.0f, scrolled - listHeight);
		if (m_scrollTo >= 0 && size_t(m_scrollTo) < items.size())
		{
			float top = titleScrolls ? headerHeight : 0.0f;
			for (int i = 0; i < m_scrollTo; ++i)
				top += heights[size_t(i)];
			if (top < m_scroll)
				m_scroll = top;
			else if (top + heights[size_t(m_scrollTo)] > m_scroll + listHeight)
				m_scroll = top + heights[size_t(m_scrollTo)] - listHeight;
		}
		m_scrollTo = -1;
		m_scroll = std::clamp(m_scroll, 0.0f, m_scrollMax);
		o.clip(m_list.x0, m_list.y0, m_list.x1, m_list.y1);
		m_items.assign(items.size(), Rect{});
		int firstChoice = -1;
		for (size_t i = 0; i < items.size() && firstChoice < 0; ++i)
			if (items[i].kind == Kind::Button)
				firstChoice = int(i);
		float iy = y - m_scroll;
		if (titleScrolls)
		{
			if (!header.empty())
				o.richBlock(header, x0 + kPadX, iy, headerWidth, m_headerSize, false, kText);
			iy += headerHeight;
		}
		const float rightEdge = x0 + w - kPadX - (m_scrollMax > 0 ? 10 : 0);
		for (size_t i = 0; i < items.size(); ++i)
		{
			const auto& item = items[i];
			const float ih = heights[i];
			const Rect r{ x0 + kPadX, iy, rightEdge, iy + ih };
			iy += ih;
			if (r.y1 <= m_list.y0 || r.y0 >= m_list.y1)
				continue;
			m_items[i] = { r.x0, std::max(r.y0, m_list.y0), r.x1, std::min(r.y1, m_list.y1) };
			const bool hot = int(i) == m_hover;
			switch (item.kind)
			{
			case Kind::Text: o.richBlock(texts[i], r.x0, r.y0 + 4, r.x1 - r.x0, m_textSize, false, kText); break;
			case Kind::Button:
			{
				if (label(int(i)))
				{
					// A choice with a picture, as the original's: the theme's label button, no stone.
					// Its picture, and its text white; gold with the focus (the first, then the last
					// under the mouse or the keys), RollText while pressed, the content then 1 lower.
					const bool pressed = int(i) == m_pressed && hot;
					const size_t st = pressed ? size_t(Theme::State::Press) : int(i) == m_selected ? size_t(Theme::State::Rollover) : size_t(Theme::State::Default);
					const float down = pressed ? 1.0f : 0.0f;
					float tx = r.x0 + kLabelInset;
					if (const ButtonArt::Icon* ic = m_art.icon(item.icon))
					{
						const float iy0 = std::round(r.y0 + (ih - kLabelIcon) / 2) - 2 + down; // (Wine: its top 9 over the text's capitals)
						o.image(ic->key, ic->bgra, ic->width, ic->height, 1, tx, iy0, tx + kLabelIcon, iy0 + kLabelIcon);
					}
					tx += kLabelIcon + kLabelGap;
					const float tw = r.x1 - tx, th = o.richHeight(texts[i], tw, m_textSize, false);
					o.setShadow(m_label->shadow[st], m_label->shadowAlpha[st]);
					o.richBlock(texts[i], tx, std::round(r.y0 + (ih - th) / 2) + down, tw, m_textSize, false, m_label->text[st], m_label->textAlpha[st]);
					o.resetShadow();
					break;
				}
				if (stone)
				{
					// A choice: the theme's stone button across the window (lit under the mouse), its
					// picture and its text dark on it; the first has the focus rim until the keys choose.
					// (the skin is drawn over the whole place: its own indent leaves the gap, 3 above
					// and below; the stone then shows 1.5 inside the window's text margin)
					const float by0 = r.y0, by1 = r.y1;
					const size_t st = hot && !m_button->skin[1].empty() ? 1 : 0;
					Theme::draw(o, m_button->skin[st], r.x0 - 2.5f, by0, x0 + w - kPadX + 3.5f, by1);
					if (!m_keyboard && int(i) == firstChoice)
						o.frame(r.x0 - 1.5f, by0 + 3, x0 + w - kPadX + 1.5f, by1 - 3, kFocus, 0.9f, 4, 1.5f);
					float tx = r.x0 + kStoneInset;
					if (const ButtonArt::Icon* ic = m_art.icon(item.icon))
					{
						const float iy0 = std::round(by0 + (by1 - by0 - kIcon) / 2);
						o.image(ic->key, ic->bgra, ic->width, ic->height, 1, tx, iy0, tx + kIcon, iy0 + kIcon);
						tx += kIcon + 5;
					}
					float tw = x0 + w - kPadX - kStoneInset - tx;
					if (item.centred) // (DLL_FONT_CENTER_JUSTIFY: the game menu's)
					{
						const float natural = std::min(o.richWidth(texts[i], m_textSize, false), tw);
						tx += std::round((tw - natural) / 2);
						tw = natural + 1;
					}
					const float th = o.richHeight(texts[i], tw, m_textSize, false);
					const bool coloured = m_button->coloured;
					if (coloured)
						o.setShadow(m_button->shadow[st], m_button->shadowAlpha[st]);
					o.richBlock(texts[i], tx, std::round(by0 + (by1 - by0 - th) / 2), tw, m_textSize, false, coloured ? m_button->text[st] : kText,
						coloured ? m_button->textAlpha[st] : 1);
					o.resetShadow();
					break;
				}
				// Without the theme: its picture and its text, gold under the mouse (its own colours stay).
				float tx = r.x0;
				if (const ButtonArt::Icon* ic = m_art.icon(item.icon))
				{
					const float iy0 = std::round(r.y0 + (ih - kIcon) / 2);
					o.image(ic->key, ic->bgra, ic->width, ic->height, 1, r.x0, iy0, r.x0 + kIcon, iy0 + kIcon);
					tx = r.x0 + kIcon + 5;
				}
				const float th = o.richHeight(texts[i], r.x1 - tx, m_textSize, false);
				o.richBlock(texts[i], tx, std::round(r.y0 + (ih - th) / 2), r.x1 - tx, m_textSize, false, hot ? kHot : kText);
				break;
			}
			case Kind::Radio:
			case Kind::Check:
			{
				const float cx = r.x0 + 9, cy = r.y0 + 13;
				if (item.kind == Kind::Radio)
				{
					o.ellipse(cx, cy, 7, 7, kEdge, 0.9f, 1.5f);
					if (m_checked[i])
						o.circle(cx, cy, 4, kHot, 1);
				}
				else
				{
					o.frame(cx - 7, cy - 7, cx + 7, cy + 7, kEdge, 0.9f, 2, 1.5f);
					if (m_checked[i])
						o.rect(cx - 4, cy - 4, cx + 4, cy + 4, kHot, 1, 1);
				}
				o.richBlock(texts[i], r.x0 + 26, r.y0 + 4, r.x1 - r.x0 - 30, m_textSize, false, hot ? kHot : kText);
				break;
			}
			case Kind::Separator:
				if (!menu)
					o.line(r.x0, r.y0 + 6, r.x1, r.y0 + 6, kText, 0.3f);
				break;
			case Kind::Edit:
			{
				// A box to type into, as the game's: dark, a light rim; the one with the focus shows where the text goes.
				const bool focus = int(i) == m_focus;
				const float by0 = r.y0 + 4, by1 = by0 + 25;
				o.rect(r.x0, by0, r.x1, by1, kEditFill, 0.95f, 3);
				o.frame(r.x0, by0, r.x1, by1, kEditRim, focus ? 0.95f : 0.6f, 3, 1);
				o.text(m_edits[i] + (focus ? L"|" : L""), r.x0 + 6, by0 + (25 - m_textSize * 1.3f) / 2, m_textSize, kText);
				break;
			}
			case Kind::Image:
				if (const ButtonArt::Icon* ic = m_art.icon(item.icon))
				{
					// (at the size the game asks, as the original: the game menu's title 192 x 48; else 64
					// high, as wide as its proportions have it)
					const float cx = (r.x0 + r.x1) / 2;
					const float ih = item.height > 0 ? item.height : 64;
					const float hw = std::min(item.width > 0 ? item.width / 2 : ih / 2 * float(ic->width) / float(std::max(ic->height, 1u)), (r.x1 - r.x0) / 2);
					o.image(ic->key, ic->bgra, ic->width, ic->height, 1, std::round(cx - hw), r.y0 + 4, std::round(cx - hw) + 2 * hw, r.y0 + 4 + ih);
				}
				break;
			}
		}
		if (m_keyboard && m_hover >= 0 && size_t(m_hover) < m_items.size() && !label(m_hover)) // (a label button shows it gold)
		{
			const Rect& r = m_items[size_t(m_hover)];
			o.frame(r.x0 - 2, r.y0 + 1, r.x1 + 2, r.y1 - 1, kKeyChoice, 1, 3, 2);
		}
		o.unclip();
		if (m_scrollMax > 0)
		{
			// Where the list is: the theme's scroll bar, as the original's (14 wide, 20 from the
			// window's right edge, from 21 under its top to 21 over its bottom when the title scrolls
			// too); else a thin grip.
			const float track = m_list.y1 - m_list.y0, thumb = std::max(24.0f, track * listHeight / scrolled);
			const float ty = m_list.y0 + (track - thumb) * (m_scroll / m_scrollMax);
			if (!m_shaft.empty() && !m_gripSkin.empty())
			{
				const float bx0 = x0 + w - 34, bx1 = bx0 + 14, by0 = titleScrolls ? y0 + 21 : m_list.y0, by1 = titleScrolls ? y0 + h - 21 : m_list.y1;
				const float gy = by0 + (by1 - by0 - thumb) * (m_scroll / m_scrollMax);
				Theme::draw(o, m_shaft, bx0, by0, bx1, by1);
				Theme::draw(o, m_gripSkin, bx0, gy, bx1, gy + thumb);
			}
			else
				o.rect(x0 + w - kPadX - 4, ty, x0 + w - kPadX + 1, ty + thumb, kEdge, 0.6f, 2);
		}
		m_ok = {};
		if (m_view->okButton)
		{
			// The OK: the theme's stone button across the window, dark text on it.
			const float bx = x0 + kPadX, bx1 = x0 + w - kPadX, by = y0 + h - kPadBottom - kOk;
			m_ok = { bx, by, bx1, by + kOk };
			const Theme::Skin* skin = m_button ? &m_button->skin[m_hoverOk && !m_button->skin[1].empty() ? 1 : 0] : nullptr;
			if (skin && !skin->empty())
				Theme::draw(o, *skin, bx, by, bx1, by + kOk);
			else
			{
				o.rect(bx, by, bx1, by + kOk, m_hoverOk ? kButtonHover : kButton, 0.95f, 5);
				o.frame(bx, by, bx1, by + kOk, kEdge, 0.8f, 5, 1);
			}
			const size_t st = m_hoverOk ? 1 : 0;
			const bool coloured = m_button && m_button->coloured;
			if (coloured)
				o.setShadow(m_button->shadow[st], m_button->shadowAlpha[st]);
			o.richText(L"OK", (bx + bx1) / 2, by + (kOk - m_textSize * 1.3f) / 2, m_textSize, false, coloured ? m_button->text[st] : kText,
				coloured ? m_button->textAlpha[st] : 1, Overlay::Align::Center);
			o.resetShadow();
		}

		// The help for what is under the mouse, in the theme's tooltip beside the window.
		if (m_hover != m_helpItem)
		{
			m_helpItem = m_hover;
			m_help = m_hover >= 0 && help ? widen(help(m_hover)) : std::wstring();
		}
		if (!m_help.empty() && m_hover >= 0 && size_t(m_hover) < m_items.size())
		{
			const float tw = 260, th = o.richHeight(m_help, tw - 20, m_helpSize, false) + 10;
			float tx = x0 - tw + 16;
			if (tx < 8)
				tx = std::min(x0 + w - 16, sw - tw - 8);
			const float ty = std::clamp(m_items[size_t(m_hover)].y0 - 10, 8.0f, std::max(8.0f, sh - th - 8));
			if (!m_tooltip.empty())
				Theme::draw(o, m_tooltip, tx, ty, tx + tw, ty + th);
			else
			{
				o.rect(tx, ty, tx + tw, ty + th, kPanel, 0.95f, 6);
				o.frame(tx, ty, tx + tw, ty + th, kEdge, 0.6f, 6, 1);
			}
			o.richBlock(m_help, tx + 10, ty + 5, tw - 20, m_helpSize, false, kText);
		}
	}
}
