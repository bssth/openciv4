#include "core/FileUtil.h"

#include <fstream>
#include <stdexcept>

namespace oc4
{
	std::vector<std::byte> readWholeFile(const std::filesystem::path& path)
	{
		std::ifstream in(path, std::ios::binary | std::ios::ate);
		if (!in)
			throw std::runtime_error("cannot open " + path.string());
		const auto size = static_cast<size_t>(in.tellg());
		std::vector<std::byte> data(size);
		in.seekg(0);
		in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(size));
		if (!in)
			throw std::runtime_error("cannot read " + path.string());
		return data;
	}

	std::string toLowerAscii(std::string s)
	{
		for (char& c : s)
			if (c >= 'A' && c <= 'Z')
				c = static_cast<char>(c - 'A' + 'a');
		return s;
	}
	std::string utf8Path(const std::filesystem::path& path)
	{
		const std::u8string s = path.u8string();
		return std::string(s.begin(), s.end());
	}

	std::filesystem::path pathFromUtf8(const std::string& text)
	{
		return std::filesystem::path(std::u8string(text.begin(), text.end()));
	}

	std::string utf8GenericPath(const std::filesystem::path& path)
	{
		const std::u8string s = path.generic_u8string();
		return std::string(s.begin(), s.end());
	}
}
