#include "render/ButtonArt.h"

#include "core/FileUtil.h"
#include "gfx/DdsImage.h"
#include "render/MapSnapshot.h"

#include <algorithm>
#include <cstdlib>
#include <cmath>
#include <filesystem>

namespace oc4::render
{
	namespace
	{
		constexpr uint64_t kIconKeys = 1ull << 40; // overlay picture keys (0 is the minimap)
		constexpr uint64_t kFlagKeys = 2ull << 40; // the map's flag art has its own ButtonArt cache
		constexpr uint32_t kAtlasCell = 64;
	}

	std::atomic<uint64_t> ButtonArt::s_next{ 0 };

	const ButtonArt::Picture* ButtonArt::picture(const std::string& file)
	{
		const std::string key = makeAssetKey(file);
		if (const auto it = m_pictures.find(key); it != m_pictures.end())
			return it->second.width ? &it->second : nullptr;
		Picture& p = m_pictures[key];
		try
		{
			std::vector<std::byte> bytes;
			if (const auto loc = m_assets.locate(key))
				bytes = m_assets.read(*loc);
			// (one of the theme's, "resource/temp/civ4_title_small.dds": in the Resource beside a
			// layer's Assets, as the game menu's title)
			else if (key.starts_with("resource/"))
				for (const auto& root : m_assets.layerRoots())
					if (std::error_code ec; std::filesystem::is_regular_file(root.parent_path() / file, ec))
					{
						bytes = readWholeFile(root.parent_path() / file);
						break;
					}
			if (!bytes.empty())
			{
				const gfx::Image image = gfx::loadTexture(bytes); // (DDS, or the font's TGA)
				p.bgra = gfx::decodeBgra(image);
				p.width = image.width;
				p.height = image.height;
			}
		}
		catch (const std::exception&)
		{
			p = {};
		}
		return p.width ? &p : nullptr;
	}

	const ButtonArt::Icon* ButtonArt::flag(const std::string& art, const Color& primary, const Color& secondary, bool whiteFlag)
	{
		const auto pack = [](const Color& c) {
			const auto channel = [](float v) { return uint32_t(std::lround(std::clamp(v, 0.0f, 1.0f) * 255)); };
			return channel(c.b) | channel(c.g) << 8 | channel(c.r) << 16;
		};
		const uint32_t p = pack(primary), s = pack(secondary);
		const auto key = std::tuple{ makeAssetKey(art), p, s, whiteFlag };
		if (const auto it = m_flags.find(key); it != m_flags.end())
			return it->second.width ? &it->second : nullptr;
		Icon& icon = m_flags[key];
		const Picture* source = picture(art);
		if (!source)
			return nullptr;
		icon.width = source->width;
		icon.height = source->height;
		icon.bgra = source->bgra;
		for (size_t i = 0; i + 3 < icon.bgra.size(); i += 4)
		{
			const unsigned a = icon.bgra[i + 3];
			if (!whiteFlag)
				for (unsigned c = 0; c < 3; ++c)
				{
					const unsigned colour = (((p >> (c * 8)) & 255) * (255 - a) + ((s >> (c * 8)) & 255) * a + 127) / 255;
					icon.bgra[i + c] = uint8_t((unsigned(icon.bgra[i + c]) * colour + 127) / 255);
				}
			icon.bgra[i + 3] = 255; // the decal's alpha is a colour mask, not cloth transparency
		}
		icon.key = kFlagKeys + ++s_next;
		return &icon;
	}

	void ButtonArt::setFontLayout(std::vector<FontGroup> groups)
	{
		m_fontLayout = std::move(groups);
		m_glyphsRead = false;
		m_glyphs.clear();
	}

	// The game font: rows of cells split by magenta lines and columns (the letters' rows
	// first, then the icons' rows, as tall as the last one). The icon cells are counted on
	// across the rows (mods' rows are longer than 25); each of the DLL's groups takes its
	// rows of 25 of them, its characters by their distance from the group's first code.
	const ButtonArt::Picture* ButtonArt::readFont()
	{
		constexpr int kFirstCode = 8483, kRow = 25;
		const Picture* font = picture("res/fonts/gamefont.tga");
		if (!font)
			return nullptr;
		const uint32_t w = font->width, h = font->height;
		const auto marker = [&](uint32_t x, uint32_t y) {
			const uint8_t* p = font->bgra.data() + (size_t(y) * w + x) * 4;
			return p[0] == 255 && p[1] == 0 && p[2] == 255;
		};
		if (!m_glyphsRead)
		{
			m_glyphsRead = true;
			std::vector<std::pair<uint32_t, uint32_t>> bands; // rows of cells: y0, y1
			for (uint32_t y = 0, start = 0; y <= h; ++y)
			{
				bool line = y == h;
				for (uint32_t x = 0; !line && x < w && marker(x, y); ++x)
					line = x + 1 == w;
				if (line)
				{
					if (y > start)
						bands.push_back({ start, y });
					start = y + 1;
				}
			}
			if (bands.empty())
				return font;
			const uint32_t iconHeight = bands.back().second - bands.back().first;
			size_t first = 0;
			while (first < bands.size() && bands[first].second - bands[first].first != iconHeight)
				++first;
			std::vector<std::array<uint32_t, 4>> cells;
			for (size_t b = first; b < bands.size(); ++b)
			{
				const auto [y0, y1] = bands[b];
				for (uint32_t x = 0, start = 0; x <= w; ++x)
				{
					// (a column of markers in most of the row; a glyph's own pixels may cover some)
					uint32_t marks = 0;
					for (uint32_t y = y0; x < w && y < y1; ++y)
						marks += marker(x, y);
					if (x == w || marks * 5 >= (y1 - y0) * 4)
					{
						if (x > start)
							cells.push_back({ start, y0, x, y1 });
						start = x + 1;
					}
				}
			}
			size_t groupStart = 0;
			for (const FontGroup& group : m_fontLayout)
			{
				if (!group.codes.empty())
				{
					// (the DLL starts every group but the first at a multiple of 25; a mod's bonuses
					// are numbered by their FontButtonIndex, the first one need not be 0)
					const int base = std::max(kFirstCode, *std::ranges::min_element(group.codes) / kRow * kRow);
					for (const int code : group.codes)
						if (const size_t cell = groupStart + size_t(code - base); cell < cells.size())
							m_glyphs[code] = cells[cell];
				}
				groupStart += size_t(group.rows) * kRow;
			}
		}
		return font;
	}

	bool ButtonArt::fontGlyph(int code, Icon& icon)
	{
		const Picture* font = readFont();
		if (!font)
			return false;
		const uint32_t w = font->width;
		const auto it = m_glyphs.find(code);
		if (it == m_glyphs.end())
			return false;
		const auto [x0, y0, x1, y1] = it->second;
		icon.width = x1 - x0;
		icon.height = y1 - y0;
		icon.bgra.resize(size_t(icon.width) * icon.height * 4);
		for (uint32_t y = 0; y < icon.height; ++y)
			std::copy_n(font->bgra.begin() + ptrdiff_t((size_t(y0 + y) * w + x0) * 4), icon.width * 4,
				icon.bgra.begin() + ptrdiff_t(size_t(y) * icon.width * 4));
		bool any = false;
		for (size_t i = 3; i < icon.bgra.size(); i += 4)
			any |= icon.bgra[i] != 0;
		return any;
	}

	// Button art: a DDS of its own, or ",<file>,<atlas>,<column>,<row>": the atlas cell (64
	// pixels, counted from 1), the file only without one (mods leave placeholders there).
	const ButtonArt::Icon* ButtonArt::icon(const std::string& art)
	{
		if (art.empty())
			return nullptr;
		if (const auto it = m_icons.find(art); it != m_icons.end())
			return it->second.width ? &it->second : nullptr;
		Icon& icon = m_icons[art];
		if (art.starts_with("font:"))
		{
			if (!fontGlyph(std::atoi(art.c_str() + 5), icon))
				icon = {};
		}
		const bool opaque = art.starts_with("opaque:");
		const std::string source = icon.width ? std::string() : opaque ? art.substr(7) : art;
		std::vector<std::string> parts;
		for (size_t at = 0; at <= source.size();)
		{
			const size_t end = std::min(source.find(',', at), source.size());
			parts.push_back(source.substr(at, end - at));
			at = end + 1;
		}
		if (parts.size() >= 5)
			if (const Picture* atlas = picture(parts[2]))
			{
				const int column = std::atoi(parts[3].c_str()) - 1, row = std::atoi(parts[4].c_str()) - 1;
				const uint32_t x0 = uint32_t(std::max(column, 0)) * kAtlasCell, y0 = uint32_t(std::max(row, 0)) * kAtlasCell;
				if (x0 + kAtlasCell <= atlas->width && y0 + kAtlasCell <= atlas->height)
				{
					icon.width = icon.height = kAtlasCell;
					icon.bgra.resize(size_t(kAtlasCell) * kAtlasCell * 4);
					for (uint32_t y = 0; y < kAtlasCell; ++y)
						std::copy_n(atlas->bgra.begin() + ptrdiff_t((size_t(y0 + y) * atlas->width + x0) * 4), kAtlasCell * 4,
							icon.bgra.begin() + ptrdiff_t(size_t(y) * kAtlasCell * 4));
				}
			}
		const std::string& own = parts.size() > 1 && parts[0].empty() ? parts[1] : parts[0];
		if (!icon.width && !own.empty())
			if (const Picture* p = picture(own))
			{
				icon.bgra = p->bgra;
				icon.width = p->width;
				icon.height = p->height;
			}
		if (!icon.width)
			return nullptr;
		if (opaque)
			for (size_t i = 3; i < icon.bgra.size(); i += 4)
				icon.bgra[i] = 255;
		for (size_t i = 0; i + 3 < icon.bgra.size(); i += 4) // premultiplied, for Direct2D
			for (size_t c = 0; c < 3; ++c)
				icon.bgra[i + c] = uint8_t(icon.bgra[i + c] * icon.bgra[i + 3] / 255);
		icon.key = kIconKeys + ++s_next;
		return &icon;
	}
}
