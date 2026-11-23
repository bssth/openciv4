// A face's VDMX table: the line heights Windows gives its text at each size in whole pixels.

#include "render/Font.h"

namespace oc4::render
{
	std::vector<std::pair<int16_t, int16_t>> Fonts::readVdmx(const uint8_t* data, size_t size)
	{
		std::vector<std::pair<int16_t, int16_t>> lines;
		const auto u16 = [&](size_t at) { return at + 2 <= size ? uint16_t(data[at] << 8 | data[at + 1]) : uint16_t(0); };
		if (!data || size < 6)
			return lines;
		const size_t ratios = u16(4);
		if (6 + 6 * ratios > size)
			return lines;
		// (the group for a 1:1 screen: a ratio of 0:0 is any, else x from yStart to yEnd)
		size_t group = 0;
		bool found = false;
		for (size_t i = 0; i < ratios && !found; ++i)
		{
			const uint8_t x = data[6 + 4 * i + 1], y0 = data[6 + 4 * i + 2], y1 = data[6 + 4 * i + 3];
			if ((x == 0 && y0 == 0 && y1 == 0) || (x >= y0 && x <= y1))
				group = u16(6 + 4 * ratios + 2 * i), found = true;
		}
		if (!found || group + 4 > size)
			return lines;
		const size_t records = u16(group);
		for (size_t i = 0; i < records; ++i)
		{
			const size_t at = group + 4 + 6 * i;
			if (at + 6 > size)
				break;
			const uint16_t pixels = u16(at);
			if (pixels >= lines.size())
				lines.resize(size_t(pixels) + 1);
			lines[pixels] = { int16_t(u16(at + 2)), int16_t(-int16_t(u16(at + 4))) };
		}
		return lines;
	}
}
