#pragma once
// One character of an XML document's text (Xml.cpp). A document whose declaration names no
// encoding is UTF-8, but some Civ4 files that name none are Windows-1252. A byte that does not
// start a well-formed UTF-8 sequence is therefore taken as it stands, as in an ISO-8859-1
// document, and the markup after it is still read.

#include <cstddef>
#include <cstdint>

namespace oc4::host
{
	// Decodes the character at data[pos] and moves pos past it.
	inline wchar_t decodeXmlChar(const unsigned char* data, size_t size, size_t& pos, bool utf8)
	{
		const unsigned char c = data[pos++];
		if (!utf8 || c < 0x80)
			return c;
		const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
		if (extra == 0 || pos + extra > size)
			return c;
		for (int i = 0; i < extra; ++i)
			if ((data[pos + i] & 0xC0) != 0x80)
				return c;
		uint32_t cp = c & (0x3F >> extra);
		for (int i = 0; i < extra; ++i)
			cp = (cp << 6) | (data[pos++] & 0x3F);
		return cp > 0xFFFF ? L'?' : static_cast<wchar_t>(cp);
	}
}
