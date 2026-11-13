// Checks reading one character of XML text (apps/oc4core/XmlDecode.h): UTF-8 as before, and
// Windows-1252 bytes in a document read as UTF-8 kept as they stand without eating what follows.

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
	check(decodeAll("a \x96 b", true, out) == 5 && out[2] == 0x96 && out[3] == L' ', "stray continuation byte");
	check(decodeAll("a\xC3", true, out) == 2 && out[1] == 0xC3, "lead byte at the end");
	check(decodeAll("\xE2\x82<", true, out) == 3 && out[0] == 0xE2 && out[1] == 0x82 && out[2] == L'<',
		"sequence cut short");

	check(decodeAll("\xC3\xB3", false, out) == 2 && out[0] == 0xC3 && out[1] == 0xB3, "ISO-8859-1 document");

	if (failures == 0)
		std::printf("xml decode: all passed\n");
	return failures == 0 ? 0 : 1;
}
