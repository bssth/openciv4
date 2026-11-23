#pragma once

// The game's button art for the overlay: a DDS of its own, or a cell of one of the interface's
// button atlases (",<file>,<atlas>,<column>,<row>", 64 pixel cells counted from 1), decoded
// once and kept as premultiplied BGRA with a key of its own for Overlay::image.
// "opaque:<art>" is the art's colours without its alpha (a civilization's flag picture,
// whose alpha is no mask). "font:<code>" is a glyph of the game font (res/Fonts/GameFont.tga)
// by the character the DLL gave it.

#include "core/AssetResolver.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <vector>

namespace oc4::render
{
	struct Color;

	class ButtonArt
	{
	public:
		struct Icon
		{
			std::vector<uint8_t> bgra; // premultiplied
			uint32_t width = 0, height = 0;
			uint64_t key = 0;
		};

		explicit ButtonArt(const AssetResolver& assets) : m_assets(assets) {}

		// nullptr when there is none.
		const Icon* icon(const std::string& art);
		// A flag decal's alpha blends the player's two colours; white flags use their own RGB.
		const Icon* flag(const std::string& art, const Color& primary, const Color& secondary, bool whiteFlag);
		// The DLL's groups of the font's icons (CvGameTextMgr::getFontSymbols): their
		// characters and how many rows of 25 cells each group takes in the font.
		struct FontGroup
		{
			std::vector<int> codes;
			int rows = 0;
		};
		void setFontLayout(std::vector<FontGroup> groups);

	private:
		struct Picture
		{
			std::vector<uint8_t> bgra;
			uint32_t width = 0, height = 0;
		};
		const Picture* picture(const std::string& file);
		// The game font's cell of the character: false without one.
		bool fontGlyph(int code, Icon& icon);
		const Picture* readFont();

		// The font's icon cells, by character: x0, y0, x1, y1 in its picture.
		std::map<int, std::array<uint32_t, 4>> m_glyphs;
		std::vector<FontGroup> m_fontLayout;
		bool m_glyphsRead = false;

		const AssetResolver& m_assets;
		std::map<std::string, Icon> m_icons;
		std::map<std::tuple<std::string, uint32_t, uint32_t, bool>, Icon> m_flags;
		std::map<std::string, Picture> m_pictures; // decoded DDS files (the atlases)
		uint64_t m_next = 0;
	};
}
