#pragma once

// The game's interface theme as the original engine reads it: Resource/Civ4.thm and
// the files it includes (Scaleform GFC theme scripts). They define styles by name
// ("Panel_Game_HudStat_Style", "Button_HUDAdvisorDomestic_Style", ...) whose properties are
// named by their scopes (with GFC_Control_Panel { .Flag_NoInnerFill } is
// GFC_Control_Panel_Flag_NoInnerFill), inherit through `map`, and draw with skins: layers of
// a cell of a TGA picture cut into nine by a margin, placed by an indent, stretched, kept at
// its size or left empty in the middle, tinted by a colour. The fonts are there too (Sylfaen
// at four sizes).
//
// What the screens need from it: a panel's inner skin and its border's, a button's
// background skin and image in each state, the fonts. Pictures come from the layers'
// Resource folders (a mod's first).

#include "render/MapSnapshot.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oc4::render
{
	class Overlay;

	class Theme
	{
	public:
		// A TGA picture, premultiplied BGRA (tinted copies are pictures of their own).
		struct Picture
		{
			uint64_t key = 0; // for Overlay::image
			std::vector<uint8_t> bgra;
			uint32_t width = 0, height = 0;
			std::string path; // as the theme names it
		};
		// A part of a picture.
		struct Bitmap
		{
			const Picture* picture = nullptr;
			float x0 = 0, y0 = 0, x1 = 0, y1 = 0; // in its pixels
			explicit operator bool() const { return picture != nullptr; }
			float width() const { return x1 - x0; }
			float height() const { return y1 - y0; }
		};
		struct Layer
		{
			enum class Middle : uint8_t
			{
				Stretch, Fixed, Empty, Fill
			};
			Bitmap image;          // cut into nine by the margin
			Bitmap center;         // drawn at its size in the middle (a checkbox's mark)
			float margin[4]{};     // left, top, right, bottom: the image's corners and sides
			float indent[4]{};     // the layer's place inside the control: left, top, right, bottom
			Middle middle = Middle::Stretch;
			bool sides = true;     // the edges are drawn
			bool section[4]{ true, true, true, true }; // its left, top, right, bottom columns / rows drawn
			Color fill{};          // Middle::Fill
			float fillAlpha = 0;
		};
		using Skin = std::vector<Layer>;
		struct Panel
		{
			Skin inner, border;
		};
		enum class State : uint8_t
		{
			Default, Rollover, Press, Disable, Toggle, ToggleRollover, TogglePress, ToggleDisable, Count
		};
		struct Button
		{
			std::array<Skin, size_t(State::Count)> skin;    // its background
			std::array<Bitmap, size_t(State::Count)> image; // its picture (an image button's)
			std::array<Skin, size_t(State::Count)> mark;    // a check box's or radio button's mark, over it
			// Its label's colour and shadow in each state (a stone button's are dark on light).
			bool coloured = false;
			std::array<Color, size_t(State::Count)> text{}, shadow{};
			std::array<float, size_t(State::Count)> textAlpha{}, shadowAlpha{};
			bool empty() const;
		};
		struct Font
		{
			std::wstring family = L"Sylfaen";
			float size = 14;
			bool bold = false, italic = false;
		};

		Theme();
		~Theme();

		// roots: the layers' Assets folders, highest first (Resource/Civ4.thm beside one, or in it).
		bool load(const std::vector<std::filesystem::path>& roots);
		bool loaded() const;
		// The folder whose Resource it came from.
		std::filesystem::path file() const;
		// Problems met while reading (unknown syntax, missing pictures), once each.
		const std::vector<std::string>& problems() const;

		// A style as the scripts name it ("Panel_City_Left_Style"), or by the name of the
		// PanelStyles / ButtonStyles value they pass (PANEL_STYLE_EMPTY); nullptr when the theme
		// has none.
		const Panel* panel(std::string_view style);
		const Button* button(std::string_view style);
		// Any skin of a style by its property ("GFC_CtrlTheme_Bitmap_Control_Window_Skin_Inner_Default"),
		// or a skin of the theme's own by its name (style empty); empty if there is none.
		Skin skin(std::string_view style, std::string_view property);
		// A progress bar's look (the stacked bars' too): its track, and the fill a bar's colour
		// tints (style: a progress style; the standard one when the theme has none such).
		struct Progress
		{
			Skin outer, track; // (HTrack, HInner)
			Layer fill;        // (HFillLeft's)
			Skin rim;          // over it all (HBorder)
		};
		const Progress* progress(std::string_view style);

		// The bitmap with the colour added, less half (the AddSigned combine): how the end of turn
		// button's glass takes its red or green.
		Bitmap addSigned(const Bitmap& bitmap, Color colour);
		// The bitmap multiplied by the colour (the Modulate combine): a bar's fill in its colour.
		Bitmap modulate(const Bitmap& bitmap, Color colour, float alpha = 1);
		// Font size 1..4 (the markup's <font=n>); the theme's, else the nearest it has.
		Font font(int size, bool bold = false, bool italic = false) const;

		// Draws a skin over the rectangle (each layer at its indent).
		static void draw(Overlay& overlay, const Skin& skin, float x0, float y0, float x1, float y1, float alpha = 1);
		// A bitmap stretched over the rectangle.
		static void draw(Overlay& overlay, const Bitmap& bitmap, float x0, float y0, float x1, float y1, float alpha = 1);

		struct Impl;

	private:
		std::unique_ptr<Impl> m;
	};
}
