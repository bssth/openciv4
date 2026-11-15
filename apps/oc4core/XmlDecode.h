#pragma once
// The text of an XML document as the original engine reads it (Xml.cpp), and the way back to the
// DLL's narrow strings.
//
// The original engine reads the whole file, turns it into UTF-16 with
// MultiByteToWideChar(CP_THREAD_ACP, 0, ...) and hands that to MSXML's IXMLDOMDocument::loadXML. The
// encoding a declaration names therefore plays no part: on a Western Windows every byte is
// Windows-1252, whether the file says ISO-8859-1, UTF-8 or nothing, and a UTF-8 file's accented
// letters come out as two characters each. Values the DLL asks for as char* or std::string go back
// through WideCharToMultiByte(CP_THREAD_ACP, 0, ...).
//
// Here the document becomes UTF-8 before it is parsed: on Windows through the same two calls, on
// other systems with Windows-1252. A file that starts with a UTF-8 byte order mark, which the
// original cannot load at all, is read as UTF-8, with any byte that does not start a well-formed
// sequence read as Windows-1252.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <windows.h>
#endif

namespace oc4::host
{
	namespace detail
	{
		// Windows-1252 0x80 to 0x9F. The five bytes it leaves undefined keep their own value, as
		// MultiByteToWideChar gives them.
		inline constexpr char16_t kWindows1252High[32] = {
			0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
			0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0x008D, 0x017D, 0x008F,
			0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
			0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
		};

		inline void appendUtf8(std::string& out, uint32_t cp)
		{
			if (cp < 0x80)
				out.push_back(static_cast<char>(cp));
			else if (cp < 0x800)
			{
				out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
			else if (cp < 0x10000)
			{
				out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
			else
			{
				out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
				out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
			}
		}
	}

	// The character a Windows-1252 byte stands for.
	inline wchar_t windows1252ToWide(unsigned char c)
	{
		return c >= 0x80 && c < 0xA0 ? static_cast<wchar_t>(detail::kWindows1252High[c - 0x80]) : static_cast<wchar_t>(c);
	}

	// The Windows-1252 byte for a character, or '?' when it has none. U+0080 to U+009F keep their
	// value, as in Windows' best fit table for code page 1252.
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

	// The document as UTF-8, read the way the original engine reads it (see the top of this file).
	inline std::string windows1252ToUtf8(std::span<const unsigned char> bytes)
	{
		std::string out;
		out.reserve(bytes.size() + bytes.size() / 8);
		for (unsigned char c : bytes)
			detail::appendUtf8(out, static_cast<uint32_t>(windows1252ToWide(c)));
		return out;
	}

	inline std::string xmlDocumentText(std::span<const unsigned char> bytes)
	{
		if (bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
			return std::string(reinterpret_cast<const char*>(bytes.data()) + 3, bytes.size() - 3);
#ifdef _WIN32
		if (bytes.empty())
			return {};
		const auto* src = reinterpret_cast<const char*>(bytes.data());
		const int srcLen = static_cast<int>(bytes.size());
		const int wideLen = MultiByteToWideChar(CP_THREAD_ACP, 0, src, srcLen, nullptr, 0);
		std::wstring wide(static_cast<size_t>(wideLen), L'\0');
		MultiByteToWideChar(CP_THREAD_ACP, 0, src, srcLen, wide.data(), wideLen);
		const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, nullptr, 0, nullptr, nullptr);
		std::string out(static_cast<size_t>(utf8Len), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.data(), wideLen, out.data(), utf8Len, nullptr, nullptr);
		return out;
#else
		return windows1252ToUtf8(bytes);
#endif
	}

	// A value as the DLL's narrow strings hold it.
	inline std::string narrowXmlText(std::wstring_view w)
	{
#ifdef _WIN32
		if (w.empty())
			return {};
		const int len = WideCharToMultiByte(CP_THREAD_ACP, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
		std::string s(static_cast<size_t>(len), '\0');
		WideCharToMultiByte(CP_THREAD_ACP, 0, w.data(), static_cast<int>(w.size()), s.data(), len, nullptr, nullptr);
		return s;
#else
		std::string s;
		s.reserve(w.size());
		for (wchar_t c : w)
			s.push_back(wideToWindows1252(c));
		return s;
#endif
	}

	// Decodes the UTF-8 character at data[pos] and moves pos past it. A byte that does not start a
	// well-formed sequence (only possible after a byte order mark) is read as Windows-1252.
	inline wchar_t decodeXmlChar(const unsigned char* data, size_t size, size_t& pos)
	{
		const unsigned char c = data[pos++];
		if (c < 0x80)
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
