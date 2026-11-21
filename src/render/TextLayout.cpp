#include "render/TextLayout.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace oc4::render
{
	namespace
	{
		bool hardBreak(char32_t c) { return c == U'\n' || c == U'\r' || c == 0x2028 || c == 0x2029 || c == 0x85; }
		bool space(char32_t c) { return c == U' ' || c == U'\t' || c == 0x3000; }
		bool ideograph(char32_t c) { return (c >= 0x2E80 && c <= 0x9FFF) || (c >= 0xAC00 && c <= 0xD7AF) || (c >= 0xF900 && c <= 0xFAFF); }

		struct Item
		{
			char32_t c = 0;
			const TextStyle* style = nullptr;
			int picture = -1; // into the inlines
			Fonts::Glyph glyph;
			float advance = 0, ascent = 0, descent = 0, gap = 0;
			float underlinePosition = 0, underlineThickness = 0;
		};
	}

	LaidText layOut(Fonts& fonts, const std::wstring& family, std::wstring_view text, const std::vector<TextSpan>& spans,
		const std::vector<TextInline>& inlines, float maxWidth)
	{
		static const TextStyle kDefault;
		std::map<std::pair<bool, bool>, int> faces;
		const auto faceOf = [&](const TextStyle& s) {
			const auto [it, added] = faces.try_emplace({ s.bold, s.italic }, 0);
			if (added)
				it->second = fonts.face(family, s.bold, s.italic);
			return it->second;
		};
		const auto measure = [&](Item& it, int face) {
			const Fonts::Metrics& m = fonts.metrics(face);
			const float size = it.style->size;
			// (in whole pixels, as Windows' text metrics give them to the original's interface: the face's
			// own heights for the size (its VDMX table), else rounded up. Sylfaen at 16 px is 17 + 5 = 22 px a
			// line, not 21.07, as the Corporation Advisor's city lists measure in Wine; at 14 px 14 + 4 = 18)
			const int pixels = int(std::lround(size));
			if (std::abs(size - float(pixels)) < 0.01f && size_t(pixels) < m.pixelLines.size() && m.pixelLines[size_t(pixels)].first > 0)
				it.ascent = m.pixelLines[size_t(pixels)].first, it.descent = m.pixelLines[size_t(pixels)].second;
			else
				it.ascent = std::ceil(m.ascent * size - 0.01f), it.descent = std::ceil(m.descent * size - 0.01f);
			it.gap = std::round(m.lineGap * size);
			it.underlinePosition = m.underlinePosition * size, it.underlineThickness = std::max(1.0f, m.underlineThickness * size);
		};

		std::vector<Item> items;
		items.reserve(text.size());
		size_t span = 0;
		for (size_t i = 0; i < text.size(); ++i)
		{
			Item it;
			const size_t at = i;
			it.c = char32_t(text[i]);
			if constexpr (sizeof(wchar_t) == 2)
				if (it.c >= 0xD800 && it.c < 0xDC00 && i + 1 < text.size() && text[i + 1] >= 0xDC00 && text[i + 1] < 0xE000)
					it.c = 0x10000 + ((it.c - 0xD800) << 10) + (char32_t(text[++i]) - 0xDC00);
			if (it.c == U'\r' && i + 1 < text.size() && text[i + 1] == L'\n')
				++i;
			while (span + 1 < spans.size() && spans[span + 1].start <= at)
				++span;
			it.style = !spans.empty() && at >= spans[span].start ? &spans[span].style : !spans.empty() ? &spans[0].style : &kDefault;
			const int face = faceOf(*it.style);
			const auto picture = std::ranges::find(inlines, at, &TextInline::at);
			if (picture != inlines.end())
			{
				it.picture = int(picture - inlines.begin());
				it.advance = picture->width;
				it.ascent = picture->height * 0.82f, it.descent = picture->height * 0.18f;
			}
			else
			{
				measure(it, face);
				if (!hardBreak(it.c) && it.c != U'\t')
				{
					it.glyph = fonts.glyph(face, it.c);
					it.advance = it.glyph.advance * it.style->size;
					if (it.glyph.face != face)
					{
						const float underline = it.underlinePosition, thickness = it.underlineThickness;
						measure(it, it.glyph.face);
						it.underlinePosition = underline, it.underlineThickness = thickness;
					}
				}
			}
			// (a kerning pair with the glyph before it, in the same face and style)
			if (!items.empty() && it.picture < 0 && items.back().picture < 0 && items.back().style == it.style && items.back().glyph.face == it.glyph.face &&
				it.glyph.index && items.back().glyph.index)
				items.back().advance += fonts.kerning(it.glyph.face, items.back().glyph.index, it.glyph.index) * it.style->size;
			items.push_back(it);
		}

		LaidText out;
		const auto advanceAt = [](const Item& it, float x) {
			if (it.c != U'\t' || it.picture >= 0)
				return it.advance;
			const float stop = 4 * it.style->size;
			return (std::floor(x / stop + 1e-4f) + 1) * stop - x;
		};
		float top = 0;
		const auto emitLine = [&](size_t from, size_t to, const Item* metricsOf) {
			float ascent = 0, descent = 0, gap = 0;
			for (size_t k = from; k < to; ++k)
				if (!hardBreak(items[k].c) || to - from == 1)
				{
					ascent = std::max(ascent, items[k].ascent);
					descent = std::max(descent, items[k].descent);
					gap = std::max(gap, items[k].gap);
				}
			if (from == to && metricsOf)
				ascent = metricsOf->ascent, descent = metricsOf->descent, gap = metricsOf->gap;
			const float baseline = top + ascent;
			float x = 0;
			for (size_t k = from; k < to; ++k)
			{
				const Item& it = items[k];
				if (hardBreak(it.c))
					continue;
				const float advance = advanceAt(it, x);
				if (it.picture >= 0)
					out.pictures.push_back({ size_t(it.picture), x, baseline - inlines[size_t(it.picture)].height * 0.82f });
				else if (!space(it.c))
					out.glyphs.push_back({ it.glyph.face, it.glyph.index, x, baseline, it.style->size, it.style->rgba });
				if (it.style->underline)
				{
					if (!out.underlines.empty() && k > from && items[k - 1].style == it.style && out.underlines.back().x1 == x)
						out.underlines.back().x1 = x + advance;
					else
						out.underlines.push_back({ x, x + advance, baseline + it.underlinePosition, it.underlineThickness, it.style->rgba });
				}
				x += advance;
			}
			out.width = std::max(out.width, x);
			top += ascent + descent + gap;
		};

		size_t i = 0;
		while (i < items.size())
		{
			const size_t from = i;
			size_t next = items.size(), breakAt = 0;
			float x = 0;
			for (size_t j = from; j < items.size(); ++j)
			{
				const Item& it = items[j];
				if (hardBreak(it.c))
				{
					next = j + 1;
					break;
				}
				if (ideograph(it.c) && j > from)
					breakAt = j;
				const float advance = advanceAt(it, x);
				if (!space(it.c) && j > from && x + advance > maxWidth + 0.01f)
				{
					next = breakAt > from ? breakAt : j;
					break;
				}
				x += advance;
				if (space(it.c) || ideograph(it.c) || (it.c == U'-' && j + 1 < items.size() && !space(items[j + 1].c)))
					breakAt = j + 1;
			}
			emitLine(from, next, nullptr);
			i = next;
		}
		if (items.empty())
		{
			Item it;
			it.style = !spans.empty() ? &spans[0].style : &kDefault;
			measure(it, faceOf(*it.style));
			emitLine(0, 0, &it);
		}
		else if (hardBreak(items.back().c))
			emitLine(items.size(), items.size(), &items.back());
		out.height = top;
		return out;
	}
}
