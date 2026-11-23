#include "render/Font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_ADVANCES_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H
#include FT_TRUETYPE_TAGS_H

#include <algorithm>
#include <bit>
#include <cstring>
#include <cstdlib>
#include <cwctype>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>

namespace oc4::render
{
	namespace
	{
		namespace fs = std::filesystem;

		// Asked in turn for a family missing from the system, then for a character the face lacks.
		constexpr const wchar_t* kDefaults[] = { L"Segoe UI", L"DejaVu Sans", L"Liberation Sans", L"Noto Sans", L"Arial", L"Helvetica" };
		// For a missing serif family (the game's Sylfaen when neither the game's own fonts nor
		// Wine's are found): serifs with a large x-height first, as Sylfaen's.
		constexpr const wchar_t* kSerifFamilies[] = { L"sylfaen", L"georgia", L"times new roman", L"times", L"cambria", L"constantia",
			L"palatino linotype", L"book antiqua", L"garamond" };
		constexpr const wchar_t* kSerifDefaults[] = { L"Sylfaen", L"Georgia", L"Noto Serif", L"DejaVu Serif", L"Liberation Serif",
			L"Times New Roman", L"Times" };
		constexpr const wchar_t* kFallbacks[] = { L"Segoe UI", L"Segoe UI Symbol", L"DejaVu Sans", L"Noto Sans", L"Noto Sans Symbols",
			L"Noto Sans Symbols 2", L"Arial", L"Microsoft YaHei", L"Noto Sans CJK SC", L"Malgun Gothic", L"Meiryo", L"Nirmala UI",
			L"Arial Unicode MS", L"Apple Symbols", L"Hiragino Sans", L"PingFang SC", L"Apple SD Gothic Neo" }; // (the last: macOS's)

		std::wstring lower(std::wstring s)
		{
			for (wchar_t& c : s)
				c = wchar_t(std::towlower(c));
			return s;
		}

		std::wstring widenAscii(const char* s)
		{
			std::wstring w;
			for (; s && *s; ++s)
				w += wchar_t(static_cast<unsigned char>(*s));
			return w;
		}

		std::vector<fs::path> fontFolders()
		{
			std::vector<fs::path> out;
			if (const char* extra = std::getenv("OC4_FONTS"))
				out.emplace_back(extra);
			const auto env = [](const char* name) { const char* v = std::getenv(name); return std::string(v ? v : ""); };
#ifdef _WIN32
			out.push_back(fs::path(env("WINDIR").empty() ? "C:\\Windows" : env("WINDIR")) / "Fonts");
			if (!env("LOCALAPPDATA").empty())
				out.push_back(fs::path(env("LOCALAPPDATA")) / "Microsoft" / "Windows" / "Fonts");
#elif defined(__APPLE__)
			out.insert(out.end(), { "/System/Library/Fonts", "/Library/Fonts" });
			if (!env("HOME").empty())
				out.push_back(fs::path(env("HOME")) / "Library" / "Fonts");
#else
			out.insert(out.end(), { "/usr/share/fonts", "/usr/local/share/fonts" });
			if (!env("HOME").empty())
				out.insert(out.end(), { fs::path(env("HOME")) / ".local" / "share" / "fonts", fs::path(env("HOME")) / ".fonts" });
#endif
			return out;
		}

		// The pair adjustments of a font's GPOS 'kern' feature (PairPos lookups), as DirectWrite kerns.
		class GposKerning
		{
		public:
			explicit GposKerning(std::vector<uint8_t> table) : m_t(std::move(table))
			{
				if (m_t.size() < 10)
					return;
				const size_t features = u16(6), lookups = u16(8);
				std::vector<uint16_t> kern;
				for (size_t i = 0, n = u16(features); i < n; ++i)
				{
					const size_t record = features + 2 + i * 6;
					if (std::memcmp(at(record), "kern", 4) != 0)
						continue;
					const size_t feature = features + u16(record + 4);
					for (size_t k = 0, c = u16(feature + 2); k < c; ++k)
						kern.push_back(u16(feature + 4 + k * 2));
				}
				std::ranges::sort(kern);
				for (const uint16_t l : kern)
				{
					if (l >= u16(lookups))
						continue;
					const size_t lookup = lookups + u16(lookups + 2 + size_t(l) * 2);
					const uint16_t type = u16(lookup);
					for (size_t s = 0, n = u16(lookup + 4); s < n; ++s)
					{
						size_t sub = lookup + u16(lookup + 6 + s * 2);
						if (type == 9 && u16(sub + 2) == 2)
							sub += u32(sub + 4);
						else if (type != 2)
							continue;
						m_subtables.push_back(sub);
					}
				}
			}

			bool empty() const { return m_subtables.empty(); }

			// The first glyph's advance change in font units, from the first subtable that covers it.
			int adjustment(uint16_t left, uint16_t right) const
			{
				for (const size_t sub : m_subtables)
				{
					const int index = coverage(sub + u16(sub + 2), left);
					if (index < 0)
						continue;
					const uint16_t format1 = u16(sub + 4), format2 = u16(sub + 6);
					const size_t size1 = valueSize(format1), size2 = valueSize(format2);
					if (u16(sub) == 1)
					{
						const size_t set = sub + u16(sub + 10 + size_t(index) * 2);
						const size_t record = 2 + size1 + size2;
						for (size_t p = 0, n = u16(set); p < n; ++p)
							if (u16(set + 2 + p * record) == right)
								return xAdvance(set + 4 + p * record, format1);
						continue;
					}
					if (u16(sub) == 2)
					{
						const size_t class1 = classOf(sub + u16(sub + 8), left), class2 = classOf(sub + u16(sub + 10), right);
						const size_t count1 = u16(sub + 12), count2 = u16(sub + 14);
						if (class1 >= count1 || class2 >= count2)
							return 0;
						return xAdvance(sub + 16 + (class1 * count2 + class2) * (size1 + size2), format1);
					}
				}
				return 0;
			}

		private:
			const uint8_t* at(size_t o) const { return o + 4 <= m_t.size() ? &m_t[o] : nullptr; }
			uint16_t u16(size_t o) const { return o + 2 <= m_t.size() ? uint16_t(m_t[o] << 8 | m_t[o + 1]) : 0; }
			uint32_t u32(size_t o) const { return uint32_t(u16(o)) << 16 | u16(o + 2); }
			static size_t valueSize(uint16_t format) { return size_t(std::popcount(uint16_t(format & 0xFF))) * 2; }
			int xAdvance(size_t value, uint16_t format) const
			{
				if (!(format & 4))
					return 0;
				return int16_t(u16(value + size_t(std::popcount(uint16_t(format & 3))) * 2));
			}
			int coverage(size_t c, uint16_t glyph) const
			{
				if (u16(c) == 1)
				{
					for (size_t i = 0, n = u16(c + 2); i < n; ++i)
						if (u16(c + 4 + i * 2) == glyph)
							return int(i);
				}
				else if (u16(c) == 2)
					for (size_t i = 0, n = u16(c + 2); i < n; ++i)
					{
						const size_t r = c + 4 + i * 6;
						if (glyph >= u16(r) && glyph <= u16(r + 2))
							return int(u16(r + 4)) + (glyph - u16(r));
					}
				return -1;
			}
			size_t classOf(size_t c, uint16_t glyph) const
			{
				if (u16(c) == 1)
				{
					const uint16_t start = u16(c + 2);
					return glyph >= start && size_t(glyph - start) < u16(c + 4) ? u16(c + 6 + size_t(glyph - start) * 2) : 0;
				}
				if (u16(c) == 2)
					for (size_t i = 0, n = u16(c + 2); i < n; ++i)
					{
						const size_t r = c + 4 + i * 6;
						if (glyph >= u16(r) && glyph <= u16(r + 2))
							return u16(r + 4);
					}
				return 0;
			}

			std::vector<uint8_t> m_t;
			std::vector<size_t> m_subtables;
		};
	}

	struct Fonts::Impl
	{
		FT_Library library = nullptr;
		struct File
		{
			fs::path path;
			FT_Long index = 0;
			bool bold = false, italic = false;
		};
		std::multimap<std::wstring, File> files; // by family, lower case
		bool scanned = false;
		struct Face
		{
			FT_Face face = nullptr;
			Metrics metrics;
			float unitsPerEm = 1;
			bool bold = false, italic = false;          // as asked
			bool emboldened = false, oblique = false; // (simulated)
			std::unordered_map<char32_t, Glyph> glyphs;
			std::unordered_map<uint32_t, float> pairs;
			std::unique_ptr<GposKerning> gpos; // (made when first asked)
			std::vector<int> fallbacks;
			bool fallbacksMade = false;
			float sizeSet = 0;
		};
		std::vector<Face> faces;
		std::map<std::tuple<std::wstring, bool, bool>, int> byName;

		std::vector<fs::path> extra; // (addFolders')

		void scan()
		{
			scanned = true;
			for (const fs::path& folder : fontFolders())
				scan(folder);
			for (const fs::path& folder : extra)
				scan(folder);
		}

		void scan(const fs::path& folder)
		{
			std::error_code ec;
			for (fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec))
			{
				const std::wstring ext = lower(it->path().extension().wstring());
				if (ec || (ext != L".ttf" && ext != L".otf" && ext != L".ttc"))
					continue;
				FT_Face f = nullptr;
				if (FT_New_Face(library, it->path().string().c_str(), -1, &f) != 0)
					continue;
				const FT_Long count = f->num_faces;
				FT_Done_Face(f);
				for (FT_Long i = 0; i < count; ++i)
				{
					if (FT_New_Face(library, it->path().string().c_str(), i, &f) != 0)
						continue;
					if (f->family_name)
						files.emplace(lower(widenAscii(f->family_name)),
							File{ it->path(), i, (f->style_flags & FT_STYLE_FLAG_BOLD) != 0, (f->style_flags & FT_STYLE_FLAG_ITALIC) != 0 });
					FT_Done_Face(f);
				}
			}
		}

		int make(const std::wstring& family, bool bold, bool italic, bool orDefault)
		{
			const auto key = std::tuple{ family, bold, italic };
			if (const auto it = byName.find(key); it != byName.end())
				return it->second;
			if (!scanned)
				scan();
			const auto [first, last] = files.equal_range(lower(family));
			const File* best = nullptr;
			int bestScore = -1;
			for (auto it = first; it != last; ++it)
			{
				const int score = (it->second.bold == bold ? 2 : 0) + (it->second.italic == italic ? 1 : 0);
				if (score > bestScore)
					best = &it->second, bestScore = score;
			}
			if (!best)
			{
				if (!orDefault)
					return -1;
				const std::wstring name = lower(family);
				if (std::ranges::any_of(kSerifFamilies, [&](const wchar_t* f) { return name == f; }))
					for (const wchar_t* other : kSerifDefaults)
						if (lower(other) != name)
							if (const int i = make(other, bold, italic, false); i >= 0)
								return byName[key] = i;
				for (const wchar_t* other : kDefaults)
					if (other != family)
						if (const int i = make(other, bold, italic, false); i >= 0)
							return byName[key] = i;
				throw std::runtime_error("no font");
			}
			Face face;
			if (FT_New_Face(library, best->path.string().c_str(), best->index, &face.face) != 0)
				return -1;
			FT_Select_Charmap(face.face, FT_ENCODING_UNICODE);
			face.unitsPerEm = float(face.face->units_per_EM);
			face.bold = bold, face.italic = italic;
			face.emboldened = bold && !best->bold;
			face.oblique = italic && !best->italic;
			// (as DirectWrite measures a face: its typographic metrics when it asks for them, else the Windows ones)
			const float u = face.unitsPerEm;
			const auto* os2 = static_cast<const TT_OS2*>(FT_Get_Sfnt_Table(face.face, FT_SFNT_OS2));
			const auto* hhea = static_cast<const TT_HoriHeader*>(FT_Get_Sfnt_Table(face.face, FT_SFNT_HHEA));
			if (os2 && os2->version != 0xFFFF && (os2->fsSelection & 128))
				face.metrics = { float(os2->sTypoAscender) / u, float(-os2->sTypoDescender) / u, float(os2->sTypoLineGap) / u };
			else if (os2 && os2->version != 0xFFFF)
			{
				const float gap = hhea ? float(hhea->Line_Gap) - float(os2->usWinAscent + os2->usWinDescent - (hhea->Ascender - hhea->Descender)) : 0;
				face.metrics = { float(os2->usWinAscent) / u, float(os2->usWinDescent) / u, std::max(gap, 0.0f) / u };
			}
			else
				face.metrics = { float(face.face->ascender) / u, float(-face.face->descender) / u,
					float(face.face->height - face.face->ascender + face.face->descender) / u };
			face.metrics.underlinePosition = float(-face.face->underline_position) / u;
			face.metrics.underlineThickness = float(face.face->underline_thickness) / u;
			if (FT_ULong length = 0; FT_Load_Sfnt_Table(face.face, FT_MAKE_TAG('V', 'D', 'M', 'X'), 0, nullptr, &length) == 0 && length > 0)
			{
				std::vector<uint8_t> vdmx(length);
				if (FT_Load_Sfnt_Table(face.face, FT_MAKE_TAG('V', 'D', 'M', 'X'), 0, vdmx.data(), &length) == 0)
					face.metrics.pixelLines = readVdmx(vdmx.data(), vdmx.size());
			}
			faces.push_back(std::move(face));
			return byName[key] = int(faces.size() - 1);
		}

		bool find(int face, char32_t c, Glyph& g)
		{
			Face& f = faces[size_t(face)];
			const FT_UInt index = FT_Get_Char_Index(f.face, FT_ULong(c));
			if (index == 0)
				return false;
			g = { face, uint16_t(index), advance(f, index, c) };
			return true;
		}

		float advance(const Face& f, FT_UInt index, char32_t c)
		{
			FT_Fixed a = 0;
			FT_Get_Advance(f.face, index, FT_LOAD_NO_SCALE, &a);
			// (as DirectWrite simulates bold: each glyph but the blank ones an em / 50 wider)
			const bool blank = c == U' ' || c == 0xA0 || c == 0x3000 || c == U'\t';
			return float(a) / f.unitsPerEm + (f.emboldened && !blank ? 0.02f : 0.0f);
		}
	};

	Fonts::Fonts() : m(std::make_unique<Impl>())
	{
		if (FT_Init_FreeType(&m->library) != 0)
			throw std::runtime_error("FreeType is unavailable");
	}

	Fonts::~Fonts()
	{
		for (auto& f : m->faces)
			FT_Done_Face(f.face);
		FT_Done_FreeType(m->library);
	}

	void Fonts::addFolders(const std::vector<std::filesystem::path>& folders)
	{
		bool added = false;
		for (const fs::path& folder : folders)
		{
			std::error_code ec;
			if (!fs::is_directory(folder, ec) || std::ranges::find(m->extra, folder) != m->extra.end())
				continue;
			m->extra.push_back(folder);
			if (m->scanned)
				m->scan(folder);
			added = true;
		}
		if (added)
			m->byName.clear(); // (a family found missing, or a stand-in's, may be there now; the faces made stay)
	}

	int Fonts::face(const std::wstring& family, bool bold, bool italic) { return m->make(family, bold, italic, true); }

	const Fonts::Metrics& Fonts::metrics(int face) const { return m->faces[size_t(face)].metrics; }

	Fonts::Glyph Fonts::glyph(int face, char32_t c)
	{
		if (const auto it = m->faces[size_t(face)].glyphs.find(c); it != m->faces[size_t(face)].glyphs.end())
			return it->second;
		Glyph g;
		if (!m->find(face, c, g))
		{
			if (!m->faces[size_t(face)].fallbacksMade)
			{
				const bool bold = m->faces[size_t(face)].bold, italic = m->faces[size_t(face)].italic;
				std::vector<int> fallbacks;
				for (const wchar_t* family : kFallbacks)
					if (const int f = m->make(family, bold, italic, false); f >= 0 && f != face)
						fallbacks.push_back(f);
				m->faces[size_t(face)].fallbacks = std::move(fallbacks);
				m->faces[size_t(face)].fallbacksMade = true;
			}
			bool found = false;
			for (const int f : m->faces[size_t(face)].fallbacks)
				if ((found = m->find(f, c, g)))
					break;
			if (!found)
				g = { face, 0, m->advance(m->faces[size_t(face)], 0, c) };
		}
		return m->faces[size_t(face)].glyphs[c] = g;
	}

	float Fonts::kerning(int face, uint16_t left, uint16_t right)
	{
		Impl::Face& f = m->faces[size_t(face)];
		if (!f.gpos)
		{
			FT_ULong length = 0;
			std::vector<uint8_t> table;
			if (FT_Load_Sfnt_Table(f.face, TTAG_GPOS, 0, nullptr, &length) == 0 && length > 0)
			{
				table.resize(length);
				FT_Load_Sfnt_Table(f.face, TTAG_GPOS, 0, table.data(), &length);
			}
			f.gpos = std::make_unique<GposKerning>(std::move(table));
		}
		if (f.gpos->empty() && !FT_HAS_KERNING(f.face))
			return 0;
		const uint32_t key = uint32_t(left) << 16 | right;
		if (const auto it = f.pairs.find(key); it != f.pairs.end())
			return it->second;
		float units = 0;
		if (!f.gpos->empty())
			units = float(f.gpos->adjustment(left, right));
		else
		{
			FT_Vector delta{};
			FT_Get_Kerning(f.face, left, right, FT_KERNING_UNSCALED, &delta);
			units = float(delta.x);
		}
		return f.pairs[key] = units / f.unitsPerEm;
	}

	Fonts::Bitmap Fonts::rasterize(int face, uint16_t glyph, float size, float subpixelX)
	{
		Impl::Face& f = m->faces[size_t(face)];
		Bitmap b;
		if (f.sizeSet != size)
		{
			FT_Set_Char_Size(f.face, 0, FT_F26Dot6(size * 64 + 0.5f), 72, 72);
			f.sizeSet = size;
		}
		if (FT_Load_Glyph(f.face, glyph, FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_LIGHT) != 0)
			return b;
		FT_GlyphSlot slot = f.face->glyph;
		if (f.emboldened)
			FT_GlyphSlot_Embolden(slot);
		if (f.oblique)
			FT_GlyphSlot_Oblique(slot);
		if (slot->format == FT_GLYPH_FORMAT_OUTLINE)
			FT_Outline_Translate(&slot->outline, FT_Pos(subpixelX * 64), 0);
		if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0 || slot->bitmap.width == 0 || slot->bitmap.rows == 0)
			return b;
		b.left = slot->bitmap_left, b.top = -slot->bitmap_top, b.width = int(slot->bitmap.width), b.height = int(slot->bitmap.rows);
		b.alpha.resize(size_t(b.width) * size_t(b.height));
		for (int y = 0; y < b.height; ++y)
			std::copy_n(slot->bitmap.buffer + y * slot->bitmap.pitch, b.width, b.alpha.begin() + ptrdiff_t(y) * b.width);
		return b;
	}
}
