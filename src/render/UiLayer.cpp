#include "render/UiLayer.h"
#include <cstdio>

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>

namespace oc4::render
{
	// The end of turn message's beat, as the original's (measured in Wine at 120 frames a second):
	// every 1.5 s it rises over 0.5 s, holds 0.5 s, sinks over 0.5 s. 0 at its lowest, 1 where
	// the hold starts and ends (1.5 in its middle: callers clamp).
	static float endTurnBeat(double clock)
	{
		const double t = std::fmod(std::max(clock, 0.0), 1.5) / 0.5;
		return float(std::min(t, 3 - t));
	}

	namespace
	{
		using Kind = MapWindow::UiItem::Kind;

		std::wstring widen(std::string_view s)
		{
			if (s.empty())
				return {};
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
			std::wstring w(size_t(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
			return w;
		}

		constexpr Color kPanel{ 0.05f, 0.06f, 0.09f }, kEdge{ 0.78f, 0.64f, 0.36f }, kText{ 0.93f, 0.93f, 0.9f }, kHot{ 1, 0.9f, 0.6f };
		// (a table header's text: the original's, dark brown on the parchment)
		constexpr Color kHeaderText{ 65 / 255.f, 40 / 255.f, 38 / 255.f };

		// The markup cut to the width with ".." after what is left of its text, as the theme's
		// buttons crop (GFC_CF_BUTTON_CROP_DOTTED); its tags stay whole.
		std::wstring fitWithDots(Overlay& o, const std::wstring& markup, float width, float size, std::wstring_view dots = L"..")
		{
			if (width <= 0)
				return {};
			if (o.richWidth(markup, size, false) <= width)
				return markup;
			std::vector<size_t> ends; // after each character of text
			for (size_t i = 0; i < markup.size(); ++i)
			{
				if (markup[i] == L'<')
					if (const size_t close = markup.find(L'>', i); close != std::wstring::npos)
					{
						i = close;
						continue;
					}
				ends.push_back(i + 1);
			}
			// (one character, an icon, is drawn whole: BUG's religion marks in 25 px columns)
			if (ends.size() <= 1)
				return markup;
			for (size_t k = ends.size(); k-- > 1;)
			{
				const std::wstring cut = markup.substr(0, ends[k - 1]) + std::wstring(dots);
				if (o.richWidth(cut, size, false) <= width)
					return cut;
			}
			return ends.empty() ? std::wstring() : markup.substr(0, ends[0]) + std::wstring(dots);
		}

		// The built-in panel styles that draw nothing (PANEL_STYLE_EMPTY, PANEL_STYLE_EXTERNAL).
		bool emptyPanel(const std::string& style)
		{
			return style == "PANEL_STYLE_EMPTY" || style == "PANEL_STYLE_EXTERNAL" || style.find("Empty") != std::string::npos;
		}

		bool interactiveKind(Kind k)
		{
			return k == Kind::ImageButton || k == Kind::Button || k == Kind::CheckBox || k == Kind::Cell || k == Kind::Dropdown || k == Kind::EndTurnButton;
		}

		// Until the theme is read: a styled button without art shows the style's own word.
		std::wstring styleWord(const std::string& style)
		{
			// ButtonStyles: the city screen's plus and minus, the arrows; the others none.
			if (style.starts_with("BUTTON_STYLE_") || (!style.empty() && std::isdigit(static_cast<unsigned char>(style[0]))))
				return style.ends_with("PLUS") ? L"+" : style.ends_with("MINUS") ? L"-" : style.ends_with("LEFT") ? L"<" : style.ends_with("RIGHT") ? L">" : L"";
			std::string s = style;
			for (const char* cut : { "Button_", "_Style", "HUDAdvisor", "HUDGlobe", "HUDJump", "HUD", "City" })
				if (const size_t at = s.find(cut); at != std::string::npos)
					s.erase(at, std::strlen(cut));
			return s.empty() ? std::wstring() : std::wstring(1, wchar_t(std::toupper(static_cast<unsigned char>(s[0]))));
		}
	}

	void UiLayer::set(std::vector<MapWindow::UiItem> items)
	{
		m_items = std::move(items);
		if (!m_fontsRead && m_theme.loaded())
		{
			m_fontsRead = true;
			for (int i = 0; i < 5; ++i)
				m_sizes[size_t(i)] = m_theme.font(i).size;
			m_tooltip = m_theme.skin("GFC_Control_StandardToolTip_Style", "GFC_CtrlTheme_Bitmap_Control_ToolTip_Skin_Inner_Default");
			m_listBorder = m_theme.skin("", "SF_CtrlTheme_Civ4_Control_MultiList_ItemBorder_Skin");
			m_listBorderGlow = m_theme.skin("", "SF_CtrlTheme_Civ4_Control_MultiList_ItemBorderGlow_Skin");
			m_listFill = m_theme.skin("", "SF_CtrlTheme_Civ4_Control_MultiList_ItemFill_Skin");
			m_listFillGlow = m_theme.skin("", "SF_CtrlTheme_Civ4_Control_MultiList_ItemFillGlow_Skin");
			const auto tinted = [&](const Theme::Skin& skin, Color c) {
				Theme::Skin out = skin;
				for (Theme::Layer& l : out)
					l.image = m_theme.modulate(l.image, c);
				return out;
			};
			// (pressed: the fill in a blue half over it; pulsing: a little blue inside, the ring from a
			// deep to a light blue, a blue halo round it; Wine's colours)
			m_listPress = tinted(m_listFill, { 0.27f, 0.26f, 0.77f });
			// (disabled: everything inside the ring and the ring itself half way to black, as Wine draws
			// a worker's farm and railroad it cannot build; not the theme's ItemDisable, a white wash)
			m_listDisable = tinted(m_listFill, { 0, 0, 0 });
			m_pulseInside = tinted(m_listFill, { 0, 0.35f, 1 });
			m_pulseDeep = tinted(m_listBorder, { 0.13f, 0.45f, 1 });
			m_pulseLight = tinted(m_listBorder, { 0.01f, 0.59f, 1 });
			m_pulseHalo = m_listBorderGlow;
			for (Theme::Layer& l : m_pulseHalo)
				l.image = m_theme.glow(l.image, { 0, 0.31f, 1 });
			if (const Theme::Panel* p = m_theme.panel("PANEL_STYLE_HUD_HELP"))
				m_hudHelp = p->inner;
			constexpr std::string_view combo = "GFC_Control_StandardComboBox_Style", button = "SF_CtrlTheme_Civ4_Control_ComboBox_Standard_PopupButton_Style";
			m_comboBox = { m_theme.skin(combo, "GFC_CtrlTheme_Bitmap_Control_ComboBox_Skin_ViewItem_Default"),
				m_theme.skin(combo, "GFC_CtrlTheme_Bitmap_Control_ComboBox_Skin_ViewItem_Rollover") };
			m_comboButton = { m_theme.skin(button, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Default"),
				m_theme.skin(button, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Rollover") };
			m_dropInner = m_theme.skin("SF_CtrlTheme_Civ4_Control_ComboBox_Standard_PopupBoxTable_Style", "GFC_CtrlTheme_Bitmap_Control_Table_Skin_Inner_Default");
			m_dropBorder = m_theme.skin("SF_CtrlTheme_Civ4_Control_ComboBox_Standard_PopupBoxTableBorder_Style",
				"GFC_CtrlTheme_Bitmap_Control_Border_Skin_Full_Default");
			// (its sides' fill left out: the list's inner skin is drawn under the whole border here)
			std::erase_if(m_dropBorder, [](const Theme::Layer& l) { return l.sidesFill && !l.image; });
			m_dropSelect = m_theme.skin(combo, "GFC_CtrlTheme_Bitmap_Control_ComboBox_Skin_PopupItem_Select");
			// The scroll bars: the standard one (GFC_Control_StandardScroll_Style) and the pull down
			// list's (the combo box's), each with its arrows, shaft and grip.
			constexpr std::string_view bs = "GFC_CtrlTheme_Bitmap_Control_Button_Skin_";
			const auto barSkin = [&](std::string_view style, std::string_view state) { return m_theme.skin(style, std::string(bs) + std::string(state)); };
			constexpr std::array<std::string_view, 3> states{ "Default", "Rollover", "Press" };
			for (size_t look = 0; look < 2; ++look)
			{
				BarLook& b = m_bar[look];
				const std::string prefix = look == 0 ? "SF_CtrlTheme_Civ4_Control_Scroll_" : "SF_CtrlTheme_Civ4_Control_ComboBox_Standard_Scroll_";
				const std::string_view scroll = look == 0 ? "GFC_Control_StandardScroll_Style" : "SF_CtrlTheme_Civ4_Control_ComboBox_Standard_Scroll_Style";
				for (size_t d = 0; d < 2; ++d)
				{
					const std::string shaft = std::string("GFC_CtrlTheme_Bitmap_Control_Scroll_Skin_") + (d == 0 ? "VShaft_" : "HShaft_");
					b.shaft[d] = { m_theme.skin(scroll, shaft + "Default"), m_theme.skin(scroll, shaft + "Press") };
					const std::string grip = prefix + (d == 0 ? "VGrip" : "HGrip") + (look == 0 ? "Button_Style" : "_Style");
					for (size_t k = 0; k < 3; ++k)
						b.grip[d][k] = barSkin(grip, states[k]);
				}
				constexpr std::array<std::string_view, 4> arrows{ "UpButton", "DownButton", "LeftButton", "RightButton" };
				for (size_t a = 0; a < 4; ++a)
					for (size_t k = 0; k < 3; ++k)
						b.arrow[a][k] = barSkin(prefix + std::string(arrows[a]) + "_Style", states[k]);
			}
			constexpr std::string_view slider = "GFC_Control_StandardSlider_Style";
			for (size_t d = 0; d < 2; ++d)
			{
				const std::string track = std::string("GFC_CtrlTheme_Bitmap_Control_Slider_Skin_") + (d == 0 ? "VTrack" : "HTrack");
				m_sliderTrack[d] = { m_theme.skin(slider, track + "_Inner_Default"), m_theme.skin(slider, track + "_Inner_Disable") };
				m_sliderFill[d] = { m_theme.skin(slider, track + "_Fill_Default"), m_theme.skin(slider, track + "_Fill_Disable") };
				const std::string_view grip = d == 0 ? "SF_CtrlTheme_Civ4_Control_Slider_VGripButton_Normal_Style" : "SF_CtrlTheme_Civ4_Control_Slider_HGripButton_Normal_Style";
				m_sliderGrip[d] = { m_theme.skin(grip, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Default"),
					m_theme.skin(grip, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Rollover"), m_theme.skin(grip, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Press"),
					m_theme.skin(grip, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Disable") };
			}
		}
		// (the screens are made again whenever something on them changes, often between a press
		// and its release: the item pressed is the same one in the new list, and what is under the
		// mouse is looked for again)
		if (m_pressed >= 0)
		{
			const Item was = std::move(m_pressedItem);
			m_pressed = -1;
			for (size_t i = 0; i < m_items.size(); ++i)
				if (const Item& it = m_items[i]; it.kind == was.kind && it.x0 == was.x0 && it.y0 == was.y0 && it.x1 == was.x1 && it.y1 == was.y1 &&
					it.texture == was.texture && it.style == was.style)
				{
					m_pressed = int(i);
					m_pressedItem = it;
					break;
				}
		}
		if (m_barClick)
		{
			if (m_pressed >= 0)
				m_barClick->item = m_pressed;
			else
				m_barClick.reset();
		}
		m_hover = m_mouseX >= 0 ? hit(m_mouseX, m_mouseY) : m_hover < int(m_items.size()) ? m_hover : -1;
		// (the keyboard's choice: the same item, or what is now nearest it)
		if (m_keyHover)
		{
			const NavRect was{ std::min(m_keyItem.x0, m_keyItem.x1), m_keyItem.y0, std::max(m_keyItem.x0, m_keyItem.x1), m_keyItem.y1 };
			int nearest = -1;
			float best = 0;
			for (const int i : reachable())
			{
				const Item& it = m_items[size_t(i)];
				const float dx = (it.x0 + it.x1) / 2 - was.cx(), dy = (it.y0 + it.y1) / 2 - was.cy(), d = dx * dx + dy * dy;
				if (nearest < 0 || d < best)
					nearest = i, best = d;
			}
			m_hover = nearest;
			m_keyHover = nearest >= 0;
			if (m_keyHover)
				m_keyItem = m_items[size_t(nearest)];
		}
	}

	std::vector<int> UiLayer::reachable() const
	{
		std::vector<int> out;
		for (size_t i = 0; i < m_items.size(); ++i)
			if (const Item& it = m_items[i]; it.interactive && std::abs(it.x1 - it.x0) >= 1 && it.y1 - it.y0 >= 1 &&
				(m_keyWithin ? (it.x0 + it.x1) / 2 >= m_keyWithin->x0 && (it.x0 + it.x1) / 2 < m_keyWithin->x1 && (it.y0 + it.y1) / 2 >= m_keyWithin->y0 &&
						(it.y0 + it.y1) / 2 < m_keyWithin->y1 : !it.mainInterface) &&
				hit(int((it.x0 + it.x1) / 2), int((it.y0 + it.y1) / 2)) == int(i))
				out.push_back(int(i));
		return out;
	}

	bool UiLayer::moveChoice(NavDirection direction, bool wrap, const NavRect* within)
	{
		// (what was chosen stays the start if it is in the new place too)
		m_keyWithin = within ? std::optional(*within) : std::nullopt;
		const std::vector<int> items = reachable();
		std::vector<NavRect> rects;
		int from = -1;
		for (const int i : items)
		{
			const Item& it = m_items[size_t(i)];
			if (m_keyHover && i == m_hover)
				from = int(rects.size());
			rects.push_back({ std::min(it.x0, it.x1), it.y0, std::max(it.x0, it.x1), it.y1 });
		}
		const int next = wrap ? navWrap(rects, from, direction) : navNearest(rects, from, direction);
		if (next < 0)
			return false;
		m_hover = items[size_t(next)];
		m_keyHover = true;
		m_keyItem = m_items[size_t(m_hover)];
		m_mouseX = m_mouseY = -1;
		return true;
	}

	int UiLayer::useKeyChoice()
	{
		const int used = m_hover;
		const Item chosen = m_items[size_t(used)];
		if (chosen.style == "__option")
		{
			const Item* box = nullptr;
			for (const Item& it : m_items)
				if (it.kind == Kind::Dropdown && it.style != "__option" && it.x0 == chosen.x0 && it.y0 < chosen.y0 && (!box || it.y0 > box->y0))
					box = &it;
			if (box)
				m_keyItem = *box;
		}
		return used;
	}

	std::pair<int, int> UiLayer::keyChoicePlace() const
	{
		if (!keyChoice())
			return { -1, -1 };
		const Item& it = m_items[size_t(m_hover)];
		return { int((it.x0 + it.x1) / 2), int((it.y0 + it.y1) / 2) };
	}

	bool UiLayer::inside(const Item& item, int x, int y) const
	{
		const float fx = float(x), fy = float(y);
		if (item.clipped && (fx < item.clip[0] || fx >= item.clip[2] || fy < item.clip[1] || fy >= item.clip[3]))
			return false;
		return fx >= std::min(item.x0, item.x1) && fx < std::max(item.x0, item.x1) && fy >= item.y0 && fy < item.y1;
	}

	int UiLayer::hit(int x, int y) const
	{
		for (size_t i = m_items.size(); i-- > 0;)
			if (const Item& it = m_items[i]; inside(it, x, y))
			{
				if (it.interactive)
					return int(i);
				if (it.blocks)
					return -1; // (a screen over the main interface: nothing under it is clicked)
			}
		return -1;
	}

	bool UiLayer::onMainMinimap(int x, int y) const
	{
		for (size_t i = m_items.size(); i-- > 0;)
			if (const Item& it = m_items[i]; inside(it, x, y))
			{
				if (it.kind == Kind::Minimap)
					return it.style != "__screenmap";
				if (it.interactive || it.blocks)
					return false;
			}
		return false;
	}

	bool UiLayer::solid(int x, int y) const { return solid(x, y, true); }

	bool UiLayer::solid(int x, int y, bool mainInterface) const
	{
		for (const Item& it : m_items)
		{
			if (!inside(it, x, y) || (!mainInterface && it.mainInterface))
				continue;
			if (it.blocks)
				return true; // (a screen over the map)
			switch (it.kind)
			{
			case Kind::Panel:
				if (const Theme::Panel* p = themedPanel(it))
				{
					if (Theme::covers(p->inner, it.x0, it.y0, it.x1, it.y1, float(x), float(y)) ||
						Theme::covers(p->border, it.x0, it.y0, it.x1, it.y1, float(x), float(y)))
						return true;
				}
				else if (!emptyPanel(it.style))
					return true;
				break;
			case Kind::Label:
			case Kind::Text:
			case Kind::EndTurnText:
			case Kind::Line:
			case Kind::Separator:
				if (it.interactive)
					return true;
				break;
			default:
				return true;
			}
		}
		return false;
	}

	void UiLayer::hover(int x, int y)
	{
		m_mouseX = x;
		m_mouseY = y;
		m_hover = hit(x, y);
		m_keyHover = false;
	}

	float UiLayer::fontSize(int font) const
	{
		// (as the game draws its labels: the game's and the small font the theme's size 2, the
		// title and the menus' 3)
		return m_sizes[font == 1 || font == 2 ? 2 : 3];
	}

	void UiLayer::drawPlotHelp(Overlay& o, const std::wstring& text)
	{
		drawHelp(o, text);
	}

	void UiLayer::drawHelp(Overlay& o, const std::wstring& text)
	{
		// The main interface's: the theme's HUD help panel (help_panel.tga in HelpBG, 35 %), the
		// map seen through it, the text 4 in from its sides (as in the original: BUG + BULL in
		// Wine); the city screen's, framed: the theme's tooltip.
		const bool see = !m_helpFramed && !m_hudHelp.empty();
		const float inX = see ? 4.0f : 10.0f, inY = 5;
		const float sw = o.screenWidth(), sh = o.screenHeight();
		// A help taller than the room it grows into gets wider, then is kept on the screen.
		const float room = std::max(m_helpDown ? sh - m_helpY : m_helpY, 100.0f);
		const float widest = std::max(m_helpRight ? sw - m_helpX : m_helpX, m_helpWidth);
		// (a tooltip: as wide as its longest line, up to its width)
		float w = m_helpFit ? std::min(std::ceil(o.richWidth(text, m_sizes[2], false)) + 2 * inX + 2, m_helpWidth) : m_helpWidth;
		float h = o.richHeight(text, w - 2 * inX, m_sizes[2], false) + 2 * inY;
		while (h > room && w < widest)
		{
			w = std::min(w * 1.5f, widest);
			h = o.richHeight(text, w - 2 * inX, m_sizes[2], false) + 2 * inY;
		}
		float x = std::clamp(m_helpRight ? m_helpX : m_helpX - w, 0.0f, std::max(sw - w, 0.0f));
		float y = std::clamp(m_helpDown ? m_helpY : m_helpY - h, 0.0f, std::max(sh - h, 0.0f));
		if (m_helpFloating && m_hover >= 0 && size_t(m_hover) < m_items.size())
		{
			// A floating help area (the city screen's, setHelpTextArea's bFloating): the help beside
			// the widget under the mouse, as the original (BUG + BULL in Wine, Roma's screen at
			// 1600x900): its right edge at the widget's left (on its right when there is no room),
			// its top at the widget's top, or its bottom at the widget's bottom when it would run off
			// the screen below (the hurry buttons); as wide as its widest line, wrapped at 240.
			const Item& it = m_items[size_t(m_hover)];
			const float ix0 = std::min(it.x0, it.x1), ix1 = std::max(it.x0, it.x1);
			constexpr float kWrap = 240;
			w = std::min(std::ceil(o.richWidth(text, m_sizes[2], false)) + 2 * inX + 2, kWrap + 2 * inX);
			h = o.richHeight(text, w - 2 * inX, m_sizes[2], false) + 2 * inY;
			x = ix0 - w >= 0 ? ix0 - w : std::min(ix1, std::max(sw - w, 0.0f));
			y = it.y0 + h <= sh ? it.y0 : it.y1 - h;
			y = std::clamp(y, 0.0f, std::max(sh - h, 0.0f));
		}
		if (see)
			Theme::draw(o, m_hudHelp, x, y, x + w, y + h);
		else if (!m_tooltip.empty())
			Theme::draw(o, m_tooltip, x, y, x + w, y + h);
		else
		{
			o.rect(x, y, x + w, y + h, kPanel, 0.93f, 5);
			o.frame(x, y, x + w, y + h, kEdge, 0.6f, 5, 1);
		}
		o.richBlock(text, x + inX, y + inY, w - 2 * inX, m_sizes[2], false, kText);
	}

	const Theme::Panel* UiLayer::themedPanel(const Item& it) const
	{
		return m_theme.loaded() && !it.style.empty() ? m_theme.panel(it.style) : nullptr;
	}

	bool UiLayer::themedButton(Overlay& o, const Item& it, int index, bool hot, bool toggled)
	{
		const Theme::Button* b = m_theme.loaded() && !it.style.empty() ? m_theme.button(it.style) : nullptr;
		if (!b)
			return false;
		using S = Theme::State;
		const bool pressed = hot && index == m_pressed;
		const S state = !it.enabled ? (toggled ? S::ToggleDisable : S::Disable)
			: pressed ? (toggled ? S::TogglePress : S::Press)
			: hot ? (toggled ? S::ToggleRollover : S::Rollover)
			: (toggled ? S::Toggle : S::Default);
		const S base = toggled ? S::Toggle : S::Default;
		// (a state the style leaves out looks as its plain one)
		const Theme::Skin& skin = !b->skin[size_t(state)].empty() ? b->skin[size_t(state)] : !b->skin[size_t(base)].empty() ? b->skin[size_t(base)] : b->skin[0];
		Theme::draw(o, skin, it.x0, it.y0, it.x1, it.y1, it.alpha);
		const Theme::Bitmap& image = b->image[size_t(state)] ? b->image[size_t(state)] : b->image[size_t(base)] ? b->image[size_t(base)] : b->image[0];
		if (image)
		{
			// At its size in the middle, smaller if the button is.
			const float w = it.x1 - it.x0, h = it.y1 - it.y0;
			const float scale = std::min({ 1.0f, w / std::max(image.width(), 1.0f), h / std::max(image.height(), 1.0f) });
			const float iw = image.width() * scale, ih = image.height() * scale;
			const float ix = std::round(it.x0 + (w - iw) / 2), iy = std::round(it.y0 + (h - ih) / 2);
			Theme::draw(o, image, ix, iy, ix + iw, iy + ih, it.alpha);
		}
		if (const Theme::Skin& mark = !b->mark[size_t(state)].empty() ? b->mark[size_t(state)] : b->mark[size_t(base)]; !mark.empty())
			Theme::draw(o, mark, it.x0, it.y0, it.x1, it.y1, it.alpha);
		return true;
	}

	void UiLayer::press(int x, int y, bool right)
	{
		m_pressed = hit(x, y);
		m_pressedRight = right;
		if (m_pressed >= 0)
			m_pressedItem = m_items[size_t(m_pressed)];
	}

	std::optional<MapWindow::UiClick> UiLayer::release(int x, int y, bool shift, bool ctrl, bool alt)
	{
		const int pressed = std::exchange(m_pressed, -1);
		if (pressed < 0 && hit(x, y) < 0)
			return MapWindow::UiClick{ -1, m_pressedRight, shift, ctrl, alt }; // (on no widget: an open list closes)
		if (pressed < 0 || hit(x, y) != pressed || !m_items[size_t(pressed)].enabled)
			return std::nullopt;
		return MapWindow::UiClick{ pressed, m_pressedRight, shift, ctrl, alt };
	}

	UiLayer::BarParts UiLayer::barParts(const Item& it)
	{
		// (the standard bar's arrows are as long as it is wide, 16, its grip at least 16; the
		// pull down list's arrows 22, its grip at least 7: as the original draws them)
		BarParts p;
		p.down = it.y1 - it.y0 >= it.x1 - it.x0;
		p.a0 = p.down ? it.y0 : it.x0;
		p.a1 = p.down ? it.y1 : it.x1;
		const bool combo = it.style == "__combo";
		const float length = p.a1 - p.a0, minGrip = combo ? 7.0f : 16.0f;
		const float arrow = std::clamp((length - minGrip) / 2, 0.0f, combo ? 22.0f : (p.down ? it.x1 - it.x0 : it.y1 - it.y0));
		p.t0 = p.a0 + arrow;
		p.t1 = p.a1 - arrow;
		const float track = p.t1 - p.t0, shown = std::clamp(it.barPercent, 0.0f, 1.0f);
		const float grip = std::min(track, std::max(minGrip, track * shown));
		const float at = shown < 1 ? std::clamp(it.barStart / (1 - shown), 0.0f, 1.0f) : 0.0f;
		p.g0 = p.t0 + (track - grip) * at;
		p.g1 = p.g0 + grip;
		return p;
	}

	UiLayer::BarPart UiLayer::barPartAt(const Item& it, int x, int y) const
	{
		if (!inside(it, x, y))
			return BarPart::None;
		const BarParts p = barParts(it);
		const float a = float(p.down ? y : x);
		return a < p.t0 ? BarPart::Back : a >= p.t1 ? BarPart::Forward : a < p.g0 ? BarPart::PageBack : a >= p.g1 ? BarPart::PageForward : BarPart::Grip;
	}

	void UiLayer::pressBar(int x, int y)
	{
		m_barPart = BarPart::None;
		m_barClick.reset();
		if (m_pressed < 0 || m_pressedRight || m_items[size_t(m_pressed)].kind != Kind::Scrollbar || !m_items[size_t(m_pressed)].enabled)
			return;
		const Item& it = m_items[size_t(m_pressed)];
		m_barPart = barPartAt(it, x, y);
		if (m_barPart == BarPart::Grip)
		{
			const BarParts p = barParts(it);
			m_barGrab = float(p.down ? y : x) - p.g0;
			return;
		}
		m_barNext = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
		barStep(x, y);
	}

	void UiLayer::barStep(int x, int y)
	{
		// (an arrow steps while the mouse is on it; the shaft pages until the grip is under the mouse)
		if (barPartAt(m_items[size_t(m_pressed)], x, y) != m_barPart)
			return;
		m_barClick = MapWindow::UiClick{ m_pressed };
		m_barClick->barStep = m_barPart == BarPart::Back ? -1 : m_barPart == BarPart::Forward ? 1 : m_barPart == BarPart::PageBack ? -2 : 2;
	}

	void UiLayer::holdBar(int x, int y)
	{
		if (!barHeld() || m_pressed < 0)
			return;
		const Item& it = m_items[size_t(m_pressed)];
		if (m_barPart == BarPart::Grip)
		{
			// (the grip's start where the mouse has it, as a part of the way it can go, as a part of
			// all that is scrolled)
			const BarParts p = barParts(it);
			const float room = (p.t1 - p.t0) - (p.g1 - p.g0), shown = std::clamp(it.barPercent, 0.0f, 1.0f);
			const float at = room > 0 ? std::clamp((float(p.down ? y : x) - m_barGrab - p.t0) / room, 0.0f, 1.0f) : 0.0f;
			const float to = at * (1 - shown);
			if (std::abs(to - std::clamp(it.barStart, 0.0f, 1.0f)) < 1e-4f)
				return;
			m_barClick = MapWindow::UiClick{ m_pressed };
			m_barClick->barTo = to;
			return;
		}
		const auto now = std::chrono::steady_clock::now();
		if (now < m_barNext)
			return;
		m_barNext = now + std::chrono::milliseconds(90);
		barStep(x, y);
	}

	void UiLayer::setHelp(const std::string& text, float x, float y, float width, bool expandRight, bool expandDown, bool framed, bool fit, bool floating)
	{
		m_helpFramed = framed;
		m_helpFit = fit;
		m_helpFloating = floating;
		m_help = widen(text);
		m_helpX = x;
		m_helpY = y;
		m_helpWidth = std::max(200.0f, width);
		m_helpRight = expandRight;
		m_helpDown = expandDown;
	}

	void UiLayer::drawListButton(Overlay& o, const Item& it, bool hot, bool pressed, double clock)
	{
		// As the original draws a multi list's button (measured in Wine, BUG + BULL): its
		// picture whole, the ring over its edge in the HUDBorder colour; under the mouse the fill
		// glow added (+57 of 255: its inside, the ring, and a soft edge just outside); pressed, a
		// blue fill half over that; disabled, half way to black (its own picture, the one
		// disableMultiListButton gives).
		const float x0 = it.x0, y0 = it.y0, x1 = it.x1, y1 = it.y1, a = it.alpha;
		if (it.pulse)
		{
			// The action recommended: a blue glow that beats every 0.6 s, the ring going from a deep to
			// a light blue, a blue halo just outside it and a little blue inside (Wine, 120 fps film).
			const float beat = 0.5f - 0.5f * std::cos(float(clock) * 2 * std::numbers::pi_v<float> / 0.6f);
			Theme::draw(o, m_pulseInside, x0, y0, x1, y1, 0.25f * beat * a);
			if (const Theme::Skin& h = m_pulseHalo; !h.empty())
			{
				// (strong: at its peak the halo covers what is under it where the glow is two thirds white)
				const float k = 0.75f + 0.75f * beat;
				Theme::draw(o, h, x0, y0, x1, y1, std::min(k, 1.0f) * a);
				if (k > 1)
					Theme::draw(o, h, x0, y0, x1, y1, (k - 1) * a);
			}
			Theme::draw(o, m_pulseDeep, x0, y0, x1, y1, a);
			Theme::draw(o, m_pulseLight, x0, y0, x1, y1, beat * a);
		}
		else
			Theme::draw(o, m_listBorder, x0, y0, x1, y1, a);
		if (!it.enabled)
			Theme::draw(o, m_listDisable, x0, y0, x1, y1, 0.5f * a);
		if (hot)
			Theme::draw(o, m_listFillGlow, x0, y0, x1, y1, 0.224f * a);
		if (pressed)
			Theme::draw(o, m_listPress, x0, y0, x1, y1, 0.5f * a);
	}

	void UiLayer::draw(Overlay& o, const std::function<void(float, float, float, float, bool)>& minimap, double clock,
		const std::function<bool(const Item&)>& leaderhead, const std::function<void()>& underScreens)
	{
		const float sw = o.screenWidth(), sh = o.screenHeight();
		bool screensReached = false;
		const Item* map = nullptr;
		for (const Item& it : m_items)
			if (it.kind == Kind::Minimap)
				map = &it;
		for (size_t i = 0; i < m_items.size(); ++i)
		{
			Item& it = m_items[i];
			if (!screensReached && !it.mainInterface)
			{
				screensReached = true;
				if (underScreens)
					underScreens();
			}
			const bool hot = int(i) == m_hover && it.enabled;
			const float x0 = it.x0, y0 = it.y0, x1 = it.x1, y1 = it.y1, w = x1 - x0, h = y1 - y0;
			const float alpha = it.alpha * (it.enabled ? 1.0f : 0.45f);
			if (it.clipped)
				o.clip(it.clip[0], it.clip[1], it.clip[2], it.clip[3]);
			const auto icon = [&](const std::string& art, float ix0, float iy0, float ix1, float iy1, float a) {
				if (const ButtonArt::Icon* ic = m_art.icon(art))
				{
					o.image(ic->key, ic->bgra, ic->width, ic->height, 1, ix0, iy0, ix1, iy1, a);
					return true;
				}
				return false;
			};
			// (a text's box is on the side of its anchor that its justification says)
			const auto label = [&](float y, float size, bool bold, Color c) {
				const std::wstring text = widen(it.text);
				if (it.flags & 4)
					o.richText(text, (x0 + x1) / 2, y, size, bold, c, alpha, Overlay::Align::Center);
				else if (it.flags & 2)
					o.richText(text, x1, y, size, bold, c, alpha, Overlay::Align::Right);
				else
					o.richText(text, x0, y, size, bold, c, alpha);
			};
			switch (it.kind)
			{
			case Kind::Panel:
				if (w <= 0 || h <= 0)
					break;
				if (it.style == "__droplist")
				{
					// An open pull down's list: the theme's, or a dark box with a rim.
					if (!m_dropInner.empty() || !m_dropBorder.empty())
					{
						Theme::draw(o, m_dropInner, x0, y0, x1, y1);
						Theme::draw(o, m_dropBorder, x0, y0, x1, y1);
					}
					else
					{
						o.rect(x0, y0, x1, y1, { 0.08f, 0.1f, 0.15f }, 0.95f, 2);
						o.frame(x0, y0, x1, y1, kEdge, 0.8f, 2, 1);
					}
					break;
				}
				if (it.style.starts_with('@'))
				{
					// (a skin of the theme by its style and property, "@style|property": the engine's own
					// controls the scripts do not draw, as the WorldBuilder's tool window and its tabs)
					auto at = m_namedSkins.find(it.style);
					if (at == m_namedSkins.end() && m_theme.loaded())
					{
						const size_t bar = it.style.find('|');
						at = m_namedSkins.emplace(it.style, bar == std::string::npos ? Theme::Skin{} :
							m_theme.skin(std::string_view(it.style).substr(1, bar - 1), std::string_view(it.style).substr(bar + 1))).first;
					}
					if (at != m_namedSkins.end())
						Theme::draw(o, at->second, x0, y0, x1, y1, it.alpha);
				}
				else if (const Theme::Panel* p = themedPanel(it))
				{
					// (in the script's colour, setPanelColor, where the theme tints it by its own)
					Theme::draw(o, it.hasColor ? m_theme.recoloured(p->inner, it.color) : p->inner, x0, y0, x1, y1, it.alpha);
					Theme::draw(o, it.hasColor ? m_theme.recoloured(p->border, it.color) : p->border, x0, y0, x1, y1, it.alpha);
				}
				else if (!emptyPanel(it.style))
				{
					// (one over much of the screen is a frame around the map, as the city screen's top: only its edge)
					if (w * h < 0.3f * sw * sh)
						o.rect(x0, y0, x1, y1, it.hasColor ? it.color : kPanel, 0.8f * alpha, 4);
					o.frame(x0, y0, x1, y1, kEdge, 0.45f * alpha, 4, 1);
				}
				break;
			case Kind::Image:
			case Kind::Graphic:
				icon(it.texture, x0, y0, x1, y1, alpha);
				break;
			case Kind::ImageButton:
				if (it.listItem && !m_listBorder.empty() && icon(it.texture, x0, y0, x1, y1, it.alpha))
					drawListButton(o, it, hot, hot && int(i) == m_pressed, clock);
				else if (icon(it.texture, x0, y0, x1, y1, alpha))
				{
					// (a pulsing button: what the unit is recommended to do; the original's has a blue rim
					// that glows round it, a little brighter and dimmer a second a beat, the picture tinted blue)
					if (it.pulse)
					{
						const float beat = 0.5f - 0.5f * std::cos(float(clock) * 2 * std::numbers::pi_v<float>);
						constexpr Color kGlow{ 0.2f, 0.55f, 1.0f };
						o.rect(x0, y0, x1, y1, kGlow, 0.18f * alpha, 3);
						o.frame(x0 - 2, y0 - 2, x1 + 2, y1 + 2, kGlow, (0.25f + 0.15f * beat) * alpha, 5, 3);
						o.frame(x0, y0, x1, y1, { 0.45f, 0.75f, 1.0f }, (0.75f + 0.25f * beat) * alpha, 3, 2);
					}
					// (a screen's picture button has no look of its own under the mouse in the
					// original: the civics screen's big civic buttons; the main interface's keep theirs)
					if (hot && it.mainInterface)
						o.frame(x0, y0, x1, y1, kHot, 1, 3, 2);
				}
				else if (!themedButton(o, it, int(i), hot, it.state) && w > 0)
				{
					o.rect(x0, y0, x1, y1, kPanel, 0.85f * alpha, 4);
					o.frame(x0, y0, x1, y1, hot ? kHot : kEdge, 0.8f * alpha, 4, 1);
					o.text(styleWord(it.style), (x0 + x1) / 2, y0 + h / 2 - 8, 13, kText, alpha, true, Overlay::Align::Center);
				}
				break;
			case Kind::Button:
			case Kind::Dropdown:
			case Kind::Edit:
			{
				const bool themed = it.kind == Kind::Button && themedButton(o, it, int(i), hot, it.state);
				// (a pull down in the theme's look: its box, and its button at the right)
				const bool combo = it.kind == Kind::Dropdown && !m_comboBox[0].empty() && w > 30;
				if (combo)
				{
					const float bw = std::min(25.0f, w / 3);
					Theme::draw(o, m_comboBox[hot && !m_comboBox[1].empty() ? 1 : 0], x0, y0, x1 - bw, y1, alpha);
					Theme::draw(o, m_comboButton[hot && !m_comboButton[1].empty() ? 1 : 0], x1 - bw, y0, x1, y1, alpha);
				}
				else if (!themed)
				{
					// (a disabled edit box greyed: its box and text fainter)
					const float dim = it.kind == Kind::Edit && !it.enabled ? 0.45f : 1.0f;
					o.rect(x0, y0, x1, y1, hot ? Color{ 0.22f, 0.27f, 0.36f } : Color{ 0.12f, 0.14f, 0.2f }, 0.9f * alpha * dim, 4);
					o.frame(x0, y0, x1, y1, hot ? kHot : kEdge, 0.8f * alpha * dim, 4, 1);
				}
				float tx = x0 + 6;
				if (!it.texture.empty() && icon(it.texture, x0 + 2, y0 + 2, x0 + h - 2, y1 - 2, alpha))
					tx = x0 + h + 2;
				const std::wstring text = widen(it.text);
				const float size = m_sizes[2], ty = y0 + (h - o.richHeight(text, 4096, size, false)) / 2;
				// (a themed button's label in its colours: dark on stone, light shadow)
				const Theme::Button* look = themed && m_theme.loaded() ? m_theme.button(it.style) : nullptr;
				const size_t st = !it.enabled ? 3 : hot ? 1 : 0;
				Color colour = it.interactive && hot && !look ? kHot : kText;
				float a = it.kind == Kind::Edit && !it.enabled ? 0.45f * alpha : alpha;
				if (look && look->coloured)
				{
					colour = look->text[st];
					a = look->textAlpha[st];
					o.setShadow(look->shadow[st], look->shadowAlpha[st]);
				}
				if (it.kind == Kind::Button && it.texture.empty())
					o.richText(text, (x0 + x1) / 2, ty, size, false, colour, a, Overlay::Align::Center);
				else if (it.kind == Kind::Edit)
				{
					// (an edit box's text kept inside it: a long path is cut at its right edge)
					o.clip(it.clipped ? std::max(x0, it.clip[0]) : x0, it.clipped ? std::max(y0, it.clip[1]) : y0,
						it.clipped ? std::min(x1 - 3, it.clip[2]) : x1 - 3, it.clipped ? std::min(y1, it.clip[3]) : y1);
					o.richText(text, tx, ty, size, false, colour, a);
					o.unclip(); // (back to its own clip, if it has one: the clips are a stack)
				}
				else
					o.richText(text, tx, ty, size, false, colour, a);
				o.resetShadow();
				if (it.kind == Kind::Dropdown && !combo)
					o.text(L"\x25BE", x1 - 14, y0 + h / 2 - 9, 13, kText, alpha);
				if (!themed && w > 0 && it.text.empty() && it.texture.empty())
					o.text(styleWord(it.style), (x0 + x1) / 2, y0 + h / 2 - 8, 12, kText, alpha, true, Overlay::Align::Center);
				break;
			}
			case Kind::CheckBox:
				if (icon(it.texture, x0, y0, x1, y1, alpha))
				{
					// (a picture with its own highlight, as the civics screen's: that highlight says
					// it is on, and nothing more is drawn, under the mouse either, as the original)
					if (!it.texture2.empty())
					{
						if (it.state)
							icon(it.texture2, x0, y0, x1, y1, alpha);
						break;
					}
				}
				else if (themedButton(o, it, int(i), hot, it.state))
					break; // (its toggled look says it is on)
				else
				{
					o.rect(x0, y0, x1, y1, it.state ? Color{ 0.35f, 0.3f, 0.15f } : kPanel, 0.85f * alpha, 4);
					o.frame(x0, y0, x1, y1, kEdge, 0.8f * alpha, 4, 1);
					o.text(styleWord(it.style), (x0 + x1) / 2, y0 + h / 2 - 8, 12, kText, alpha, true, Overlay::Align::Center);
				}
				if (it.state)
					o.frame(x0, y0, x1, y1, kHot, 0.9f, 3, 2);
				else if (hot)
					o.frame(x0, y0, x1, y1, kHot, 0.7f, 3, 1.5f);
				break;
			case Kind::Label:
			case Kind::Text:
				// (a text, CyGInterfaceScreen.setText, is in the theme's normal font, size 3 and not bold,
				// whatever font it names, as the original draws it and as determineWidth measures it; a
				// label in the one it names)
				// (only a text lights up under the mouse; a label keeps its colour, its help aside)
				label(y0, it.kind == Kind::Text ? m_sizes[3] : fontSize(it.font), it.kind != Kind::Text && fontBold(it.font),
					it.kind == Kind::Text && it.interactive && hot ? kHot : kText);
				break;
			case Kind::EndTurnText:
				if (!it.text.empty())
					// (blinking with the end turn button's beat: gone around its darkest)
					o.richText(widen(it.text), sw / 2, y0 + 6, m_sizes[3], false, { 0.85f, 0.85f, 0.8f },
						std::clamp((endTurnBeat(clock) - 0.25f) / 0.75f, 0.0f, 1.0f), Overlay::Align::Center);
				break;
			case Kind::Multiline:
				o.richBlock(widen(it.text), x0, y0, std::max(20.0f, w), fontSize(it.font), fontBold(it.font), kText, alpha);
				break;
			case Kind::Bar:
			{
				// A stacked bar's part: the theme's track under the first, each part's fill (the theme's,
				// in the part's colour) over its share.
				const float fx0 = x0 + w * std::clamp(it.barStart, 0.0f, 1.0f), fx1 = x0 + w * std::clamp(it.barStart + it.barPercent, 0.0f, 1.0f);
				const Color colour = it.hasColor ? it.color : Color{ 0.5f, 0.5f, 0.5f };
				if (const Theme::Progress* p = m_theme.loaded() ? m_theme.progress(it.style) : nullptr)
				{
					if (it.barStart <= 0)
					{
						Theme::draw(o, p->outer, x0, y0, x1, y1);
						Theme::draw(o, p->track, x0, y0, x1, y1);
					}
					if (it.barPercent > 0 && p->fill.image && fx1 > fx0)
					{
						Theme::Layer fill = p->fill;
						fill.image = m_theme.modulate(fill.image, colour, it.alpha);
						o.clip(fx0, y0, fx1, y1);
						Theme::draw(o, Theme::Skin{ fill }, x0, y0, x1, y1);
						o.unclip();
					}
					if (it.barLast)
						Theme::draw(o, p->rim, x0, y0, x1, y1); // (the rim and its gloss over all the parts)
					break;
				}
				if (it.barStart <= 0)
					o.rect(x0, y0, x1, y1, { 0, 0, 0 }, 0.55f * alpha, 2);
				if (it.barPercent > 0)
					o.rect(fx0, y0, fx1, y1, colour, it.alpha, 1);
				break;
			}
			case Kind::Header:
			{
				// A table's column header: the theme's header button (Civ4Theme_Table.thm's
				// Header_Table_HButtonStyle: the parchment of bg_header.tga, header_h.tga's edges
				// between the columns), its text in the button's colours, in the middle of its height.
				const Theme::Table* t = m_theme.loaded() ? m_theme.table(it.style) : nullptr;
				const Theme::Button* b = t ? t->header : nullptr;
				const std::wstring text = widen(it.text);
				const size_t state = hot ? size_t(Theme::State::Rollover) : size_t(Theme::State::Default);
				if (b && !b->skin[state].empty())
					Theme::draw(o, b->skin[state], x0, y0, x1, y1, alpha);
				else
					o.rect(x0, y0, x1, y1, { 0.1f, 0.11f, 0.15f }, 0.9f, 0);
				const Color colour = b && b->coloured ? b->text[state] : b ? kHeaderText : Color{ 1, 0.9f, 0.65f };
				const float ty = y0 + (h - o.richHeight(text, 4096, m_sizes[2], false)) / 2;
				// (the column the table is sorted by: its arrow at the right, the text left of it, cut
				// with dots where it does not fit: Wine's "P.. ^" over BUG's POP)
				const int sorted = (it.flags & 0x100) ? 0 : (it.flags & 0x200) ? 1 : -1;
				if (sorted >= 0 && t && t->sortArrow[sorted])
				{
					const Theme::Bitmap& arrow = t->sortArrow[sorted];
					const float aw = arrow.width(), ah = arrow.height();
					const float ax = std::round(x1 - 4 - aw), ay = std::round(y0 + (h - ah) / 2);
					Theme::draw(o, arrow, ax, ay, ax + aw, ay + ah, alpha);
					o.richText(fitWithDots(o, text, ax - 2 - (x0 + 4), m_sizes[2]), x0 + 4, ty, m_sizes[2], false, colour, alpha, Overlay::Align::Left);
					break;
				}
				o.richText(text, (it.flags & 2) ? x1 - 4 : x0 + 4, ty, m_sizes[2], false, colour, alpha,
					(it.flags & 2) ? Overlay::Align::Right : Overlay::Align::Left);
				break;
			}
			case Kind::Cell:
			{
				if (it.style == "__option")
				{
					// An item of an open pull down's list: the theme's band under the mouse (the
					// chosen one's text gold).
					if (hot)
					{
						if (!m_dropSelect.empty())
							Theme::draw(o, m_dropSelect, x0 + 2, y0, x1 - 2, y1);
						else
							o.rect(x0 + 2, y0, x1 - 2, y1, { 0.25f, 0.3f, 0.42f }, 0.7f, 0);
					}
					const std::wstring text = widen(it.text);
					o.richText(text, x0 + 8, y0 + (h - o.richHeight(text, 4096, m_sizes[2], false)) / 2, m_sizes[2], false, it.state ? kHot : kText, alpha);
					break;
				}
				// (its row under it has the look: alternate, under the mouse, selected)
				float tx = x0 + 4;
				if (!it.texture.empty() && icon(it.texture, x0 + 2, y0 + 1, x0 + h - 1, y1 - 1, alpha))
					tx = x0 + h + 2;
				const bool right = (it.flags & 2) != 0, centre = (it.flags & 4) != 0;
				// (in its font, size 2 a table's, 3 a list box's; in the middle of the room the style
				// leaves round a cell's content; cut with "..." where its column ends, as the original's:
				// BUG's page editor's "GREATPEOPLE_TUR...")
				const float size = m_sizes[size_t(std::clamp(it.font, 0, 4))];
				const std::wstring text = fitWithDots(o, widen(it.text), (right || centre ? x1 - x0 - 8 : x1 - 4 - tx), size, L"...");
				float above = 0, below = 0;
				if (const Theme::Table* t = m_theme.loaded() ? m_theme.table(it.style) : nullptr)
					above = t->content[1], below = t->content[3];
				const float ty = y0 + above + (h - above - below - o.richHeight(text, 4096, size, false)) / 2;
				o.richText(text, right ? x1 - 4 : centre ? (x0 + x1) / 2 : tx, ty, size, false, kText, alpha,
					right ? Overlay::Align::Right : centre ? Overlay::Align::Center : Overlay::Align::Left);
				break;
			}
			case Kind::Row:
			{
				// A table's or list box's row: the theme's skin for it (Civ4Theme_Table.thm's .Row):
				// every other one Alt_, under the mouse Rollover (the whole row, whichever of its
				// cells is), selected Select.
				if (it.flags & 2)
				{
					// (the table's own background, under its rows: the theme's inner skin)
					if (const Theme::Table* t = m_theme.loaded() ? m_theme.table(it.style) : nullptr)
						Theme::draw(o, t->inner, x0, y0, x1, y1, alpha);
					break;
				}
				bool over = inside(it, m_mouseX, m_mouseY);
				for (size_t j = i + 1; over && j < m_items.size(); ++j)
					if (const Item& above = m_items[j]; (above.interactive || above.blocks) && inside(above, m_mouseX, m_mouseY))
					{
						over = above.kind == Kind::Cell && above.y0 == y0 && above.y1 == y1 && above.x0 >= x0 && above.x1 <= x1;
						break;
					}
				const bool alt = (it.flags & 1) != 0;
				if (const Theme::Table* t = m_theme.loaded() ? m_theme.table(it.style) : nullptr)
				{
					using R = Theme::Table::Row;
					Theme::draw(o, t->row[alt][it.state ? (over ? R::SelectRollover : R::Select) : over ? R::Rollover : R::Default], x0, y0, x1, y1, alpha);
				}
				else if (it.state || over)
					o.rect(x0, y0, x1, y1, { 0.25f, 0.3f, 0.42f }, it.state ? 0.7f : 0.4f, 0);
				break;
			}
			case Kind::Line:
				// (a pixel wide, as the original's addLineGFC: the foreign advisor's relations; a table's grid)
				o.line(x0, y0, x1, y1, it.hasColor ? it.color : kEdge, it.alpha, 1);
				break;
			case Kind::Separator:
				// (nothing: the theme's GSeparator is transparent with no inner fill,
				// Civ4Theme_Separator.thm; it only takes its room in the flow)
				break;
			case Kind::Slider:
			{
				// The theme's slider (GFC_Control_StandardSlider_Style): a track 12 thick in the middle
				// of its box, its fill from the grip to the side the style names ("down": the bottom
				// or the right, "up": the top or the left), and the grip, 28 by 14, where it is set.
				const bool down = it.style.starts_with('v'), fillEnd = it.style.find("down") != std::string::npos,
					fillStart = it.style.find("up") != std::string::npos;
				const size_t d = down ? 0 : 1, off = it.enabled ? 0 : 1;
				constexpr float thick = 12, gripLong = 28, gripShort = 14;
				const float length = down ? h : w, at = std::clamp(it.barStart, 0.0f, 1.0f);
				const float centre = (down ? y0 : x0) + gripShort / 2 + at * std::max(0.0f, length - gripShort);
				const float tx0 = down ? (x0 + x1 - thick) / 2 : x0, ty0 = down ? y0 : (y0 + y1 - thick) / 2;
				const float tx1 = down ? tx0 + thick : x1, ty1 = down ? y1 : ty0 + thick;
				if (!m_sliderTrack[d][off].empty())
				{
					Theme::draw(o, m_sliderTrack[d][off], tx0, ty0, tx1, ty1, it.alpha);
					if (fillEnd || fillStart)
					{
						const float f0 = fillEnd ? centre : (down ? ty0 : tx0), f1 = fillEnd ? (down ? ty1 : tx1) : centre;
						if (f1 > f0)
							Theme::draw(o, m_sliderFill[d][off], down ? tx0 : f0, down ? f0 : ty0, down ? tx1 : f1, down ? f1 : ty1, it.alpha);
					}
				}
				else
					o.rect(tx0, ty0, tx1, ty1, kEdge, 0.6f * alpha, 2);
				// (the theme's disabled grip is an empty cell: a disabled one is the plain grip, dimmed)
				const size_t state = !it.enabled ? 0 : int(i) == m_pressed ? 2 : hot ? 1 : 0;
				const float gx0 = down ? (x0 + x1 - gripLong) / 2 : centre - gripShort / 2, gy0 = down ? centre - gripShort / 2 : (y0 + y1 - gripLong) / 2;
				const float gx1 = gx0 + (down ? gripLong : gripShort), gy1 = gy0 + (down ? gripShort : gripLong);
				if (const Theme::Skin& g = !m_sliderGrip[d][state].empty() ? m_sliderGrip[d][state] : m_sliderGrip[d][0]; !g.empty())
					Theme::draw(o, g, gx0, gy0, gx1, gy1, alpha);
				else
					o.rect(gx0, gy0, gx1, gy1, hot ? kHot : kText, alpha, 2);
				break;
			}
			case Kind::Minimap:
				if (minimap)
					minimap(x0, y0, x1, y1, it.style == "__screenmap");
				break;
			case Kind::Movie:
				if (leaderhead)
					leaderhead(it);
				break;
			case Kind::Leaderhead:
			case Kind::Model:
				if (!leaderhead || !leaderhead(it))
				{
					// (its button, as large as fits, in the middle)
					const float s = std::min({ w, h, 192.0f }), cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
					icon(it.leaderButton, cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2, alpha);
				}
				break;
			case Kind::Scrollbar:
			{
				// A scroll bar on the edge of what scrolls: its shaft, an arrow at each end, and its
				// grip as long as the part shown and where it is (the theme's look, or a dark track
				// with a light grip); pressed, held or under the mouse as the mouse has them.
				const BarParts p = barParts(it);
				const size_t d = p.down ? 0 : 1;
				const BarLook& look = m_bar[it.style == "__combo" ? 1 : 0];
				const bool held = int(i) == m_pressed && barHeld();
				const BarPart under = hot && !barHeld() ? barPartAt(it, m_mouseX, m_mouseY) : BarPart::None;
				const auto along = [&](float a, float b, float& rx0, float& ry0, float& rx1, float& ry1) {
					rx0 = p.down ? x0 : a, ry0 = p.down ? a : y0, rx1 = p.down ? x1 : b, ry1 = p.down ? b : y1;
				};
				const auto state = [&](BarPart part) { return held && m_barPart == part ? 2 : under == part ? 1 : 0; };
				const auto skin = [](const auto& states, int k) -> const Theme::Skin& {
					return !states[size_t(k)].empty() ? states[size_t(k)] : states[0];
				};
				float rx0, ry0, rx1, ry1;
				along(p.t0, p.t1, rx0, ry0, rx1, ry1);
				const bool paging = held && (m_barPart == BarPart::PageBack || m_barPart == BarPart::PageForward);
				if (!look.shaft[d][0].empty())
					Theme::draw(o, skin(look.shaft[d], paging ? 1 : 0), rx0, ry0, rx1, ry1);
				else
					o.rect(x0, y0, x1, y1, { 0.05f, 0.06f, 0.09f }, 0.6f, 3);
				for (int end = 0; end < 2 && p.t0 > p.a0; ++end)
				{
					const BarPart part = end == 0 ? BarPart::Back : BarPart::Forward;
					const auto& arrow = look.arrow[d * 2 + size_t(end)];
					if (end == 0)
						along(p.a0, p.t0, rx0, ry0, rx1, ry1);
					else
						along(p.t1, p.a1, rx0, ry0, rx1, ry1);
					if (!arrow[0].empty())
						Theme::draw(o, skin(arrow, state(part)), rx0, ry0, rx1, ry1);
				}
				along(p.g0, p.g1, rx0, ry0, rx1, ry1);
				if (!look.grip[d][0].empty())
					Theme::draw(o, skin(look.grip[d], state(BarPart::Grip)), rx0, ry0, rx1, ry1);
				else
					o.rect(rx0 + 2, ry0 + 2, rx1 - 2, ry1 - 2, state(BarPart::Grip) ? kHot : kEdge, 0.8f, 3);
				break;
			}
			case Kind::Flag:
			{
				// A player's flag, as the original's by the minimap (68 by 250, most below the
				// screen): its era's flag model waving, drawn by the window; where it cannot be, a
				// banner from a crossbar on its pole, in the player's colour with the
				// civilization's flag picture over it.
				if (leaderhead && leaderhead(it))
					break;
				const float s = w / 68.0f, px = x0 + w / 2;
				const Color wood{ 0.33f, 0.2f, 0.1f }, brass{ 0.76f, 0.64f, 0.4f }, shade{ 0, 0, 0 };
				o.rect(px - 1.5f * s, y0 + 14 * s, px + 1.5f * s, y1, wood, alpha, 0);
				o.rect(x0 + 3 * s, y0 + 29 * s, x1 - 3 * s, y0 + 33 * s, wood, alpha, 1);
				o.rect(px - 7 * s, y0 + 27.5f * s, px + 7 * s, y0 + 34.5f * s, brass, alpha, 3);
				const float bx0 = x0 + 6 * s, bx1 = x1 - 5 * s, by0 = y0 + 33 * s, by1 = y0 + 110 * s;
				o.rect(bx0, by0, bx1, by1, it.hasColor ? it.color : Color{ 0.5f, 0.5f, 0.5f }, alpha, 0);
				icon(it.texture, bx0, by0, bx1, by1, alpha);
				// (its folds: shade at its edges)
				o.rect(bx0, by0, bx0 + 2 * s, by1, shade, 0.3f * alpha, 0);
				o.rect(bx1 - 2 * s, by0, bx1, by1, shade, 0.3f * alpha, 0);
				o.rect(bx0, by1 - 2 * s, bx1, by1, shade, 0.2f * alpha, 0);
				if (hot)
					o.frame(bx0, by0, bx1, by1, kHot, 0.8f, 0, 1.5f);
				break;
			}
			case Kind::EndTurnButton:
			{
				// The engine's own button where the script puts it: the style's brass ring, and over it
				// the theme's glass (SF_CtrlTheme_Civ4_Control_Button_EndTurn_Skin, at its size in the
				// middle) taking the state's colour, added less half (AddSigned): EndTurnDot_Green while
				// units can still move, steady; EndTurnDot_Red once the turn is over, sinking to
				// EndTurnDot_DarkRed and back with the end of turn message's beat.
				const float cx = w > 0 ? (x0 + x1) / 2 : map ? map->x0 - 22 : sw - 240, cy = h > 0 ? (y0 + y1) / 2 : map ? map->y0 + 18 : sh - 140;
				const float r = w > 0 ? std::min(w, h) / 2 - 1 : 15;
				const bool green = it.text == "Green";
				Color colour = m_theme.colour("SF_CtrlTheme_Civ4_Control_Color_EndTurnDot_Green", { 0, 0.75f, 0 });
				if (!green)
				{
					const Color red = m_theme.colour("SF_CtrlTheme_Civ4_Control_Color_EndTurnDot_Red", { 0.75f, 0, 0 });
					const Color dark = m_theme.colour("SF_CtrlTheme_Civ4_Control_Color_EndTurnDot_DarkRed", { 0.39f, 0, 0 });
					const float k = std::min(endTurnBeat(clock), 1.0f);
					colour = { dark.r + (red.r - dark.r) * k, dark.g + (red.g - dark.g) * k, dark.b + (red.b - dark.b) * k };
				}
				const Theme::Button* frame = m_theme.loaded() && !it.style.empty() ? m_theme.button(it.style) : nullptr;
				Theme::Skin glass = m_theme.loaded() ? m_theme.skin("", "SF_CtrlTheme_Civ4_Control_Button_EndTurn_Skin") : Theme::Skin{};
				if (frame && !frame->skin[0].empty() && !glass.empty())
				{
					Theme::draw(o, frame->skin[hot && !frame->skin[1].empty() ? 1 : 0], cx - r, cy - r, cx + r, cy + r, it.alpha);
					for (Theme::Layer& l : glass)
						l.image = m_theme.addSigned(l.image, colour);
					Theme::draw(o, glass, cx - r, cy - r, cx + r, cy + r, it.alpha);
				}
				else
				{
					o.circle(cx, cy, r + 2, { 0.05f, 0.05f, 0.05f }, 0.9f);
					o.circle(cx, cy, r, colour, 1);
					o.ellipse(cx, cy, r, r, hot ? kHot : kEdge, 1, hot ? 2.0f : 1.2f);
				}
				break;
			}
			}
			if (it.clipped)
				o.unclip();
		}
		if (!screensReached && underScreens)
			underScreens(); // (no screen: over the main interface, under its help)
		// The help for what is under the mouse, in the help area (growing up from it unless it
		// expands down).
		// (what the keyboard chose: the mouse's highlight alone is too faint to follow)
		if (keyChoice())
		{
			const Item& it = m_items[size_t(m_hover)];
			const float x0 = std::min(it.x0, it.x1), x1 = std::max(it.x0, it.x1);
			o.frame(x0 - 2, it.y0 - 2, x1 + 2, it.y1 + 2, kHot, 1, 3, 2);
		}
		// (a tooltip the screens placed themselves, the World Builder's: where they put it)
		if (!m_help.empty() && !m_helpFit && m_hover >= 0 && size_t(m_hover) < m_items.size() && !m_items[size_t(m_hover)].mainInterface)
		{
			// A screen's widget (not the main interface's, which has its help area): the help in the
			// theme's tooltip beside it, as the original: its top at the widget's, its right edge at
			// the widget's left (on its right when there is no room), wrapped at 220 and as wide as
			// its widest line (measured in Wine, BUG's Finance Advisor at 1600x900).
			const Item& it = m_items[size_t(m_hover)];
			constexpr float kWrap = 220, kPad = 10;
			const float size = m_sizes[2];
			if (m_besideFor != m_help)
			{
				m_besideFor = m_help;
				// (its lines wrapped word by word at kWrap; the widest of them)
				float widest = 0;
				size_t start = 0;
				while (start <= m_help.size())
				{
					const size_t end = std::min(m_help.find(L'\n', start), m_help.size());
					std::wstring line;
					size_t word = start;
					while (word < end)
					{
						const size_t space = std::min(m_help.find(L' ', word), end);
						const std::wstring next = (line.empty() ? L"" : line + L" ") + m_help.substr(word, space - word);
						if (!line.empty() && o.richWidth(next, size, false) > kWrap)
						{
							widest = std::max(widest, o.richWidth(line, size, false));
							line = m_help.substr(word, space - word);
						}
						else
							line = next;
						word = space + 1;
					}
					widest = std::max(widest, o.richWidth(line, size, false));
					start = end + 1;
				}
				m_besideWidth = std::clamp(std::ceil(widest) + 1, 20.0f, kWrap);
			}
			const float w = m_besideWidth + 2 * kPad, h = o.richHeight(m_help, m_besideWidth, size, false) + 10;
			const float left = std::min(it.x0, it.x1), right = std::max(it.x0, it.x1);
			const float x = left - w >= 0 ? left - w : std::min(right, std::max(sw - w, 0.0f));
			const float y = std::clamp(it.y0, 0.0f, std::max(sh - h, 0.0f));
			if (!m_tooltip.empty())
				Theme::draw(o, m_tooltip, x, y, x + w, y + h);
			else
			{
				o.rect(x, y, x + w, y + h, kPanel, 0.93f, 5);
				o.frame(x, y, x + w, y + h, kEdge, 0.6f, 5, 1);
			}
			o.richBlock(m_help, x + kPad, y + 5, m_besideWidth, size, false, kText);
		}
		else if (!m_help.empty())
			drawHelp(o, m_help);
	}
}
