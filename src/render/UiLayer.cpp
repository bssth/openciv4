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
			constexpr std::string_view combo = "GFC_Control_StandardComboBox_Style", button = "SF_CtrlTheme_Civ4_Control_ComboBox_Standard_PopupButton_Style";
			m_comboBox = { m_theme.skin(combo, "GFC_CtrlTheme_Bitmap_Control_ComboBox_Skin_ViewItem_Default"),
				m_theme.skin(combo, "GFC_CtrlTheme_Bitmap_Control_ComboBox_Skin_ViewItem_Rollover") };
			m_comboButton = { m_theme.skin(button, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Default"),
				m_theme.skin(button, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Rollover") };
			m_dropInner = m_theme.skin("SF_CtrlTheme_Civ4_Control_ComboBox_Standard_PopupBoxTable_Style", "GFC_CtrlTheme_Bitmap_Control_Table_Skin_Inner_Default");
			m_dropBorder = m_theme.skin("SF_CtrlTheme_Civ4_Control_ComboBox_Standard_PopupBoxTableBorder_Style",
				"GFC_CtrlTheme_Bitmap_Control_Border_Skin_Full_Default");
			m_dropSelect = m_theme.skin(combo, "GFC_CtrlTheme_Bitmap_Control_ComboBox_Skin_PopupItem_Select");
			constexpr std::string_view scroll = "GFC_Control_StandardScroll_Style";
			m_shaft = { m_theme.skin(scroll, "GFC_CtrlTheme_Bitmap_Control_Scroll_Skin_VShaft_Default"),
				m_theme.skin(scroll, "GFC_CtrlTheme_Bitmap_Control_Scroll_Skin_HShaft_Default") };
			for (size_t d = 0; d < 2; ++d)
			{
				const std::string_view grip = d == 0 ? "SF_CtrlTheme_Civ4_Control_Scroll_VGripButton_Style" : "SF_CtrlTheme_Civ4_Control_Scroll_HGripButton_Style";
				m_grip[d] = { m_theme.skin(grip, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Default"),
					m_theme.skin(grip, "GFC_CtrlTheme_Bitmap_Control_Button_Skin_Rollover") };
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

	bool UiLayer::solid(int x, int y) const
	{
		for (const Item& it : m_items)
		{
			if (!inside(it, x, y))
				continue;
			if (it.blocks)
				return true; // (a screen over the map)
			switch (it.kind)
			{
			case Kind::Panel:
				if (const Theme::Panel* p = themedPanel(it))
				{
					if (!p->inner.empty() || !p->border.empty())
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
		const float w = m_helpWidth, h = o.richHeight(text, w - 10, m_sizes[2], false);
		const float x = (m_helpRight ? m_helpX : m_helpX - w) + 5;
		const float y = std::clamp(m_helpDown ? m_helpY : m_helpY - h - 4, 0.0f, std::max(o.screenHeight() - h, 0.0f));
		o.richBlock(text, x, y, w - 10, m_sizes[2], false, kText);
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
		if (pressed < 0 || hit(x, y) != pressed || !m_items[size_t(pressed)].enabled)
			return std::nullopt;
		return MapWindow::UiClick{ pressed, m_pressedRight, shift, ctrl, alt };
	}

	void UiLayer::setHelp(const std::string& text, float x, float y, float width, bool expandRight, bool expandDown)
	{
		m_help = widen(text);
		m_helpX = x;
		m_helpY = y;
		m_helpWidth = std::max(200.0f, width);
		m_helpRight = expandRight;
		m_helpDown = expandDown;
	}

	void UiLayer::draw(Overlay& o, const std::function<void(float, float, float, float, bool)>& minimap, double clock,
		const std::function<bool(const Item&)>& leaderhead)
	{
		const float sw = o.screenWidth(), sh = o.screenHeight();
		const Item* map = nullptr;
		for (const Item& it : m_items)
			if (it.kind == Kind::Minimap)
				map = &it;
		for (size_t i = 0; i < m_items.size(); ++i)
		{
			Item& it = m_items[i];
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
					Theme::draw(o, p->inner, x0, y0, x1, y1, it.alpha);
					Theme::draw(o, p->border, x0, y0, x1, y1, it.alpha);
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
				if (icon(it.texture, x0, y0, x1, y1, alpha))
				{
					if (hot)
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
					o.rect(x0, y0, x1, y1, hot ? Color{ 0.22f, 0.27f, 0.36f } : Color{ 0.12f, 0.14f, 0.2f }, 0.9f * alpha, 4);
					o.frame(x0, y0, x1, y1, hot ? kHot : kEdge, 0.8f * alpha, 4, 1);
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
				float a = alpha;
				if (look && look->coloured)
				{
					colour = look->text[st];
					a = look->textAlpha[st];
					o.setShadow(look->shadow[st], look->shadowAlpha[st]);
				}
				if (it.kind == Kind::Button && it.texture.empty())
					o.richText(text, (x0 + x1) / 2, ty, size, false, colour, a, Overlay::Align::Center);
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
					if (it.state && !it.texture2.empty())
						icon(it.texture2, x0, y0, x1, y1, alpha);
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
				// (a text, CyGInterfaceScreen.setText, is in the theme's normal font, size 3, whatever
				// font it names; a label in the one it names)
				label(y0, it.kind == Kind::Text ? m_sizes[3] : fontSize(it.font), fontBold(it.font), it.interactive && hot ? kHot : kText);
				break;
			case Kind::EndTurnText:
				if (!it.text.empty())
					o.richText(widen(it.text), sw / 2, y0 + 6, m_sizes[3], false, { 0.85f, 0.85f, 0.8f }, 0.6f + 0.4f * float(std::sin(clock * 3)), Overlay::Align::Center);
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
				o.rect(x0, y0, x1, y1, { 0.1f, 0.11f, 0.15f }, 0.9f, 0);
				o.richText(widen(it.text), (it.flags & 2) ? x1 - 4 : x0 + 4, y0 + 2, m_sizes[1], true, { 1, 0.9f, 0.65f }, 1,
					(it.flags & 2) ? Overlay::Align::Right : Overlay::Align::Left);
				break;
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
				if (it.state || hot)
					o.rect(x0, y0, x1, y1, { 0.25f, 0.3f, 0.42f }, it.state ? 0.7f : 0.4f, 0);
				float tx = x0 + 4;
				if (!it.texture.empty() && icon(it.texture, x0 + 2, y0 + 1, x0 + h - 1, y1 - 1, alpha))
					tx = x0 + h + 2;
				const bool right = (it.flags & 2) != 0, centre = (it.flags & 4) != 0;
				const std::wstring text = widen(it.text);
				o.richText(text, right ? x1 - 4 : centre ? (x0 + x1) / 2 : tx, y0 + (h - o.richHeight(text, 4096, m_sizes[2], false)) / 2, m_sizes[2], false,
					kText, alpha, right ? Overlay::Align::Right : centre ? Overlay::Align::Center : Overlay::Align::Left);
				break;
			}
			case Kind::Line:
				o.line(x0, y0, x1, y1, it.hasColor ? it.color : kEdge, it.alpha, 2);
				break;
			case Kind::Separator:
				o.line(x0, y0 + h / 2, x1, y0 + h / 2, kEdge, 0.5f);
				break;
			case Kind::Slider:
				o.rect(x0, y0 + h / 2 - 2, x1, y0 + h / 2 + 2, kEdge, 0.6f, 2);
				break;
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
				// A scroll bar on the edge of what scrolls: its shaft, and its grip as long as the part
				// shown and where it is (the theme's look, or a dark track with a light grip).
				const bool down = h >= w;
				const size_t d = down ? 0 : 1;
				const float length = down ? h : w, grip = std::max(20.0f, length * std::clamp(it.barPercent, 0.0f, 1.0f));
				const float at = std::clamp(it.barStart, 0.0f, 1.0f) * length, from = std::min(at, length - grip);
				const float gx0 = down ? x0 : x0 + from, gy0 = down ? y0 + from : y0, gx1 = down ? x1 : gx0 + grip, gy1 = down ? gy0 + grip : y1;
				if (!m_shaft[d].empty())
					Theme::draw(o, m_shaft[d], x0, y0, x1, y1);
				else
					o.rect(x0, y0, x1, y1, { 0.05f, 0.06f, 0.09f }, 0.6f, 3);
				if (!m_grip[d][0].empty())
					Theme::draw(o, m_grip[d][hot && !m_grip[d][1].empty() ? 1 : 0], gx0, gy0, gx1, gy1);
				else
					o.rect(gx0 + 2, gy0 + 2, gx1 - 2, gy1 - 2, hot ? kHot : kEdge, 0.8f, 3);
				break;
			}
			case Kind::Flag:
			{
				// A player's flag, as the original's by the minimap (68 by 250, most below the
				// screen): a banner from a crossbar on its pole, in the player's colour with the
				// civilization's flag picture over it.
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
				// The engine's own button where the script puts it: green when the turn is done.
				const float cx = w > 0 ? (x0 + x1) / 2 : map ? map->x0 - 22 : sw - 240, cy = h > 0 ? (y0 + y1) / 2 : map ? map->y0 + 18 : sh - 140;
				const float r = w > 0 ? std::min(w, h) / 2 - 1 : 15;
				const bool green = it.text == "Green";
				const float pulse = green ? 0.75f + 0.25f * float(std::sin(clock * 5)) : 1.0f;
				// The theme's glass takes the colour (EndTurnDot_Green / Red: added to it, less half),
				// the green one pulsing.
				const Color colour = green ? Color{ 0, std::round(0.75f * pulse * 16) / 16, 0 } : Color{ 0.75f, 0, 0 };
				const Theme::Button* frame = m_theme.loaded() && !it.style.empty() ? m_theme.button(it.style) : nullptr;
				if (frame && !frame->skin[0].empty())
				{
					Theme::Skin glass = frame->skin[hot && !frame->skin[1].empty() ? 1 : 0];
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
		// The help for what is under the mouse, in the help area (growing up from it unless it
		// expands down).
		// (what the keyboard chose: the mouse's highlight alone is too faint to follow)
		if (keyChoice())
		{
			const Item& it = m_items[size_t(m_hover)];
			const float x0 = std::min(it.x0, it.x1), x1 = std::max(it.x0, it.x1);
			o.frame(x0 - 2, it.y0 - 2, x1 + 2, it.y1 + 2, kHot, 1, 3, 2);
		}
		if (!m_help.empty())
		{
			// A help taller than the room it grows into gets wider, then is kept on the screen.
			const float room = std::max(m_helpDown ? sh - m_helpY : m_helpY, 100.0f);
			const float widest = std::max(m_helpRight ? sw - m_helpX : m_helpX, m_helpWidth);
			float w = m_helpWidth, h = o.richHeight(m_help, w - 20, m_sizes[2], false) + 10;
			while (h > room && w < widest)
			{
				w = std::min(w * 1.5f, widest);
				h = o.richHeight(m_help, w - 20, m_sizes[2], false) + 10;
			}
			const float x = m_helpRight ? m_helpX : m_helpX - w;
			const float y = std::clamp(m_helpDown ? m_helpY : m_helpY - h, 0.0f, std::max(sh - h, 0.0f));
			// (in the theme's tooltip)
			if (!m_tooltip.empty())
				Theme::draw(o, m_tooltip, x, y, x + w, y + h);
			else
			{
				o.rect(x, y, x + w, y + h, kPanel, 0.93f, 5);
				o.frame(x, y, x + w, y + h, kEdge, 0.6f, 5, 1);
			}
			o.richBlock(m_help, x + 10, y + 5, w - 20, m_sizes[2], false, kText);
		}
	}
}
