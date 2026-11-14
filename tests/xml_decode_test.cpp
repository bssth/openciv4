// Checks reading one character of XML text (apps/oc4core/XmlDecode.h): UTF-8 as before,
// Windows-1252 bytes in a document read as UTF-8 read as Windows-1252 without eating what follows,
// and the way back to Windows-1252 narrow strings.

#include "XmlDecode.h"

#include <cstdio>
#include <cstring>

using namespace oc4::host;

namespace
{
	int failures = 0;

	void check(bool ok, const char* what)
	{
		if (!ok)
		{
			std::printf("FAILED: %s\n", what);
			++failures;
		}
	}

	// Decodes the whole of s; out gets the characters, the result is how many.
	size_t decodeAll(const char* s, bool utf8, wchar_t* out)
	{
		const auto* data = reinterpret_cast<const unsigned char*>(s);
		const size_t size = std::strlen(s);
		size_t pos = 0, n = 0;
		while (pos < size)
			out[n++] = decodeXmlChar(data, size, pos, utf8);
		return n;
	}
}

int main()
{
	wchar_t out[32];

	check(decodeAll("a<", true, out) == 2 && out[0] == L'a' && out[1] == L'<', "ASCII");
	check(decodeAll("\xC3\xB3", true, out) == 1 && out[0] == 0xF3, "two-byte UTF-8");
	check(decodeAll("\xE2\x82\xAC", true, out) == 1 && out[0] == 0x20AC, "three-byte UTF-8");
	check(decodeAll("\xF0\x9F\x98\x80", true, out) == 1 && out[0] == L'?', "four-byte UTF-8 beyond 16 bits");

	// Windows-1252 o acute before an end tag: the end tag is still read.
	check(decodeAll("ci\xF3n</", true, out) == 6 && out[2] == 0xF3 && out[3] == L'n' && out[4] == L'<' && out[5] == L'/',
		"lead byte without continuation bytes");
	// Windows-1252 en dash: a continuation byte with no lead byte.
	check(decodeAll("a \x96 b", true, out) == 5 && out[2] == 0x2013 && out[3] == L' ', "stray continuation byte");
	check(decodeAll("\x80\x9F", true, out) == 2 && out[0] == 0x20AC && out[1] == 0x0178, "euro sign and Y diaeresis");
	check(decodeAll("\x81\x8D\x8F\x90\x9D", true, out) == 5 && out[0] == 0x81 && out[1] == 0x8D && out[2] == 0x8F &&
		out[3] == 0x90 && out[4] == 0x9D, "bytes Windows-1252 leaves undefined");
	check(decodeAll("a\xC3", true, out) == 2 && out[1] == 0xC3, "lead byte at the end");
	check(decodeAll("\xE2\x82<", true, out) == 3 && out[0] == 0xE2 && out[1] == 0x201A && out[2] == L'<',
		"sequence cut short");

	// Not well-formed UTF-8: each byte is Windows-1252.
	check(decodeAll("\xC0\x80", true, out) == 2 && out[0] == 0xC0 && out[1] == 0x20AC, "overlong two-byte form");
	check(decodeAll("\xE0\x80\x80", true, out) == 3 && out[0] == 0xE0 && out[1] == 0x20AC, "overlong three-byte form");
	check(decodeAll("\xED\xA0\x80", true, out) == 3 && out[0] == 0xED && out[1] == 0xA0, "surrogate");
	check(decodeAll("\xF4\x90\x80\x80", true, out) == 4 && out[0] == 0xF4 && out[1] == 0x90, "past U+10FFFF");
	check(decodeAll("\xF5\x80", true, out) == 2 && out[0] == 0xF5, "lead byte F5");
	check(decodeAll("\xE0\xA0\x80", true, out) == 1 && out[0] == 0x800, "smallest three-byte form");
	check(decodeAll("\xED\x9F\xBF", true, out) == 1 && out[0] == 0xD7FF, "last character before the surrogates");

	// A document declared ISO-8859-1 reads each byte as the character of that value, as before.
	check(decodeAll("\xC3\xB3", false, out) == 2 && out[0] == 0xC3 && out[1] == 0xB3, "ISO-8859-1 document");
	check(decodeAll("\x96", false, out) == 1 && out[0] == 0x96, "ISO-8859-1 document, byte 0x96");

	// Back to the DLL's Windows-1252: every byte comes back from both readings of it.
	bool roundTrip = true;
	for (int b = 1; b < 256; ++b)
		roundTrip = roundTrip && wideToWindows1252(windows1252ToWide(static_cast<unsigned char>(b))) == static_cast<char>(b) &&
			wideToWindows1252(static_cast<wchar_t>(b)) == static_cast<char>(b);
	check(roundTrip, "Windows-1252 round trip");
	check(wideToWindows1252(0x4E2D) == '?', "character without a Windows-1252 byte");
	if constexpr (sizeof(wchar_t) > 2)
	{
		uint32_t big = 0x120AC;
		check(wideToWindows1252(static_cast<wchar_t>(big)) == '?', "euro sign plus 0x10000");
	}

	if (failures == 0)
		std::printf("xml decode: all passed\n");
	return failures == 0 ? 0 : 1;
}
