// Checks reading XML text as the original engine reads it (apps/oc4core/XmlDecode.h): every byte of
// a file without a byte order mark is Windows-1252, whatever the declaration says; a file with a
// UTF-8 byte order mark is UTF-8; and the way back to Windows-1252 narrow strings.

#include "XmlDecode.h"

#include <cstdio>
#include <cstring>
#include <string>

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

	std::span<const unsigned char> bytes(const char* s, size_t n)
	{
		return { reinterpret_cast<const unsigned char*>(s), n };
	}

	std::span<const unsigned char> bytes(const char* s)
	{
		return bytes(s, std::strlen(s));
	}

	// Decodes the whole of a UTF-8 text; out gets the characters, the result is how many.
	size_t decodeAll(const std::string& s, wchar_t* out)
	{
		const auto* data = reinterpret_cast<const unsigned char*>(s.data());
		size_t pos = 0, n = 0;
		while (pos < s.size())
			out[n++] = decodeXmlChar(data, s.size(), pos);
		return n;
	}

	size_t decodeAll(const char* s, wchar_t* out)
	{
		return decodeAll(std::string(s), out);
	}
}

int main()
{
	wchar_t out[64];

	// The original engine: every byte is Windows-1252, the declaration plays no part.
	check(windows1252ToUtf8(bytes("<a>ci\xF3n</a>")) == "<a>ci\xC3\xB3n</a>", "o acute");
	check(windows1252ToUtf8(bytes("\x80\x96\x9F")) == "\xE2\x82\xAC\xE2\x80\x93\xC5\xB8", "euro sign, en dash, Y diaeresis");
	check(windows1252ToUtf8(bytes("\x81\x8D\x8F\x90\x9D")) == "\xC2\x81\xC2\x8D\xC2\x8F\xC2\x90\xC2\x9D",
		"bytes Windows-1252 leaves undefined keep their value");
	// A UTF-8 file without a byte order mark reads as the original reads it: two characters.
	check(windows1252ToUtf8(bytes("Regul\xC3\xA4r")) == "Regul\xC3\x83\xC2\xA4r", "UTF-8 a diaeresis as two characters");
	check(windows1252ToUtf8(bytes("a\0b", 3)) == std::string("a\0b", 3), "a NUL byte stays one byte");

	// Every byte decodes to its Windows-1252 character through the parser's UTF-8 reader.
	bool all = true;
	for (int b = 1; b < 256; ++b)
	{
		const char c = static_cast<char>(b);
		const std::string text = windows1252ToUtf8(bytes(&c, 1));
		all = all && decodeAll(text, out) == 1 && out[0] == windows1252ToWide(static_cast<unsigned char>(b));
	}
	check(all, "each byte is one Windows-1252 character");

#ifndef _WIN32
	// Off Windows the document is Windows-1252 (on Windows it is the thread's ANSI code page).
	check(xmlDocumentText(bytes("<?xml version=\"1.0\" encoding=\"UTF-8\"?><a>\xC3\xA4</a>")) ==
			"<?xml version=\"1.0\" encoding=\"UTF-8\"?><a>\xC3\x83\xC2\xA4</a>",
		"declared UTF-8 still reads as Windows-1252");
	check(xmlDocumentText(bytes("<a>\x96</a>")) == "<a>\xE2\x80\x93</a>", "undeclared Windows-1252");
#endif
	check(xmlDocumentText(bytes("")).empty(), "empty file");

	// A UTF-8 byte order mark: the rest is UTF-8, which the original could not load at all.
	check(xmlDocumentText(bytes("\xEF\xBB\xBF<a>\xC3\xA4</a>")) == "<a>\xC3\xA4</a>", "byte order mark");
	check(decodeAll("\xC3\xB3", out) == 1 && out[0] == 0xF3, "two-byte UTF-8");
	check(decodeAll("\xE2\x82\xAC", out) == 1 && out[0] == 0x20AC, "three-byte UTF-8");
	check(decodeAll("\xF0\x9F\x98\x80", out) == 1 && out[0] == L'?', "four-byte UTF-8 beyond 16 bits");
	// After a byte order mark, a byte that does not start well-formed UTF-8 is Windows-1252 and
	// does not take what follows with it.
	check(decodeAll("ci\xF3n</", out) == 6 && out[2] == 0xF3 && out[3] == L'n' && out[4] == L'<', "lead byte alone");
	check(decodeAll("a \x96 b", out) == 5 && out[2] == 0x2013 && out[3] == L' ', "stray continuation byte");
	check(decodeAll("\xC0\x80", out) == 2 && out[0] == 0xC0 && out[1] == 0x20AC, "overlong two-byte form");
	check(decodeAll("\xED\xA0\x80", out) == 3 && out[0] == 0xED && out[1] == 0xA0, "surrogate");
	check(decodeAll("\xE2\x82<", out) == 3 && out[0] == 0xE2 && out[1] == 0x201A && out[2] == L'<', "sequence cut short");

	// Back to the DLL's narrow strings.
	bool roundTrip = true;
	for (int b = 1; b < 256; ++b)
		roundTrip = roundTrip && wideToWindows1252(windows1252ToWide(static_cast<unsigned char>(b))) == static_cast<char>(b);
	check(roundTrip, "Windows-1252 round trip");
	check(wideToWindows1252(0x4E2D) == '?', "character without a Windows-1252 byte");
	if constexpr (sizeof(wchar_t) > 2)
	{
		uint32_t big = 0x120AC;
		check(wideToWindows1252(static_cast<wchar_t>(big)) == '?', "euro sign plus 0x10000");
	}
#ifndef _WIN32
	check(narrowXmlText(L"Ci\x00F3n \x2013 \x20AC") == "Ci\xF3n \x96 \x80", "narrow value");
#endif

	if (failures == 0)
		std::printf("xml decode: all passed\n");
	return failures == 0 ? 0 : 1;
}
