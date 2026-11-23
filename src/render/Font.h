#pragma once

// The system's fonts: faces by family and style, their glyphs (with other faces standing in for
// the characters a face lacks), metrics and coverage bitmaps. DirectWrite on Windows (Font.cpp),
// FreeType elsewhere (FontFreeType.cpp).

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace oc4::render
{
	class Fonts
	{
	public:
		Fonts();
		~Fonts();
		Fonts(const Fonts&) = delete;
		Fonts& operator=(const Fonts&) = delete;

		// More folders whose fonts count as installed: the game's own (it ships Sylfaen in
		// Resource/Civ4), a Wine prefix's Windows/Fonts. Families asked for before and missing
		// are looked for again.
		void addFolders(const std::vector<std::filesystem::path>& folders);

		// A face of the family (or a stand-in when it is missing: a serif one for a serif family);
		// faces are numbered from 0.
		int face(const std::wstring& family, bool bold, bool italic);

		// In ems (times the size in pixels).
		struct Metrics
		{
			float ascent = 0.8f, descent = 0.2f, lineGap = 0, underlinePosition = 0.1f, underlineThickness = 0.05f;
			// The face's own line heights in whole pixels by size (its VDMX table: yMax and -yMin at that
			// many pixels an em, for a 1:1 screen), as Windows' text metrics give them; {0, 0} where it has none.
			std::vector<std::pair<int16_t, int16_t>> pixelLines;
		};
		const Metrics& metrics(int face) const;

		struct Glyph
		{
			int face = 0;
			uint16_t index = 0;
			float advance = 0; // in ems
		};
		// The character's glyph in the face, else in the first fallback face that has it (else the face's missing glyph).
		Glyph glyph(int face, char32_t c);
		// The change of the advance between two glyphs of a face (its kerning pairs), in ems.
		float kerning(int face, uint16_t left, uint16_t right);

		// Coverage (0..255) of a glyph drawn at size pixels, its origin at (subpixelX, 0); left, top: the bitmap's offset from the origin.
		struct Bitmap
		{
			int left = 0, top = 0, width = 0, height = 0;
			std::vector<uint8_t> alpha;
		};
		Bitmap rasterize(int face, uint16_t glyph, float size, float subpixelX);

		// A face's VDMX table (the OpenType "VDMX" bytes) as Metrics::pixelLines.
		static std::vector<std::pair<int16_t, int16_t>> readVdmx(const uint8_t* data, size_t size);

	private:
		struct Impl;
		std::unique_ptr<Impl> m;
	};
}
