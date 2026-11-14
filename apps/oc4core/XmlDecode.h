#pragma once
// One character of an XML document's text (Xml.cpp), and the way back to the DLL's narrow strings.
//
// A document whose declaration names no encoding is UTF-8, but some Civ4 files that name none are
// Windows-1252. A byte that does not start a well-formed UTF-8 sequence is therefore read as
// Windows-1252 (0x96 is an en dash, 0x80 the euro sign), and the markup after it is still read.
// The five bytes Windows-1252 leaves undefined keep their own value, as in Microsoft's best fit
// table. A document that names another encoding (Civ4's name ISO-8859-1) is read as before, each
// byte as the character of that value.

#include <cstddef>
#include <cstdint>

namespace oc4::host
{
	namespace detail
	{
		// Windows-1252 0x80 to 0x9F.
		inline constexpr char16_t kWindows1252High[32] = {
			0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
			0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
			0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
			0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
		};
	}

	// The character a Windows-1252 byte stands for.
	inline wchar_t windows1252ToWide(unsigned char c)
	{
		return c >= 0x80 && c < 0xA0 ? static_cast<wchar_t>(detail::kWindows1252High[c - 0x80]) : static_cast<wchar_t>(c);
	}

	// The Windows-1252 byte for a character, or '?' when it has none. U+0080 to U+009F, which an
	// ISO-8859-1 document reads its bytes 0x80 to 0x9F as, keep their value (best fit).
	inline char wideToWindows1252(wchar_t w)
	{
		const uint32_t u = static_cast<uint32_t>(w);
		if (u < 0x100)
			return static_cast<char>(u);
		for (int i = 0; i < 32; ++i)
			if (static_cast<uint32_t>(detail::kWindows1252High[i]) == u)
				return static_cast<char>(0x80 + i);
		return '?';
	}

	// Decodes the character at data[pos] and moves pos past it.
	inline wchar_t decodeXmlChar(const unsigned char* data, size_t size, size_t& pos, bool utf8)
	{
		const unsigned char c = data[pos++];
		if (!utf8 || c < 0x80)
			return c;
		// Lead bytes of well-formed UTF-8 (C0, C1 and F5 to FF never are), and the range of the
		// first continuation byte that rules out overlong forms, surrogates and code points past
		// U+10FFFF.
		size_t extra = 0;
		unsigned char lo = 0x80, hi = 0xBF;
		if (c >= 0xC2 && c <= 0xDF)
			extra = 1;
		else if (c >= 0xE0 && c <= 0xEF)
		{
			extra = 2;
			if (c == 0xE0)
				lo = 0xA0;
			else if (c == 0xED)
				hi = 0x9F;
		}
		else if (c >= 0xF0 && c <= 0xF4)
		{
			extra = 3;
			if (c == 0xF0)
				lo = 0x90;
			else if (c == 0xF4)
				hi = 0x8F;
		}
		if (extra == 0 || pos + extra > size || data[pos] < lo || data[pos] > hi)
			return windows1252ToWide(c);
		for (size_t i = 1; i < extra; ++i)
			if ((data[pos + i] & 0xC0) != 0x80)
				return windows1252ToWide(c);
		uint32_t cp = c & (0x3F >> extra);
		for (size_t i = 0; i < extra; ++i)
			cp = (cp << 6) | (data[pos++] & 0x3F);
		return cp > 0xFFFF ? L'?' : static_cast<wchar_t>(cp);
	}
}
