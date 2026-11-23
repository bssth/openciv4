// Round-trips SaveFile through write and read: an empty engine section (OpenCiv4's own saves) and one
// laid out like the original's with a sign and a Great Wall line. A file cut short anywhere before the
// end of its MD5 string must throw.

#include "SaveFile.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

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

	void put(std::vector<std::byte>& d, int v)
	{
		const auto* b = reinterpret_cast<const std::byte*>(&v);
		d.insert(d.end(), b, b + 4);
	}
	void putF(std::vector<std::byte>& d, float v)
	{
		const auto* b = reinterpret_cast<const std::byte*>(&v);
		d.insert(d.end(), b, b + 4);
	}

	// The original's layout: signs, 5 zeros, line groups, the 17-byte tail.
	std::vector<std::byte> originalEngineSection()
	{
		std::vector<std::byte> d;
		put(d, 0);
		put(d, 1); // one sign
		put(d, 0);
		put(d, 12);
		put(d, 34);
		put(d, -1);
		const char16_t text[] = u"Rome 1234abcdef"; // hex-looking text, as UTF-16
		put(d, int(std::size(text) - 1));
		d.insert(d.end(), reinterpret_cast<const std::byte*>(text), reinterpret_cast<const std::byte*>(text) + 2 * (std::size(text) - 1));
		for (int i = 0; i < 5; ++i)
			put(d, 0);
		put(d, 1); // one group
		put(d, 0);
		put(d, 1); // one line
		put(d, 0);
		put(d, 2); // two points
		for (float f : { 10.f, 20.f, 1.f, 30.f, 40.f, 2.f })
			putF(d, f);
		d.push_back(std::byte{ 0 });
		put(d, 1); // one plot corner
		put(d, 0);
		put(d, 3);
		put(d, 4);
		put(d, 32); // a 32 that is not the MD5's length
		put(d, 7);
		d.push_back(std::byte{ 1 });
		put(d, 32);
		put(d, 0);
		put(d, 0);
		return d;
	}

	oc4::host::SaveFile sample(std::vector<std::byte> engine)
	{
		oc4::host::SaveFile s;
		s.modFolder = "Mods\\Test\\";
		s.initCore = { std::byte{ 1 }, std::byte{ 2 }, std::byte{ 3 } };
		for (int i = 0; i < 200000; ++i)
			s.gameData.push_back(std::byte(i * 7));
		s.pythonData = "(dp0\n.";
		s.engineData = std::move(engine);
		return s;
	}

	void roundTrip(const std::filesystem::path& file, const std::vector<std::byte>& engine, const char* what)
	{
		const auto in = sample(engine);
		in.write(file);
		try
		{
			const auto s = oc4::host::SaveFile::read(file);
			const std::vector<std::byte> expect = engine.empty() ? std::vector<std::byte>(33) : engine;
			std::printf("%s: engine %zu bytes, md5 %s\n", what, s.engineData.size(), s.md5.c_str());
			check(s.modFolder == in.modFolder, what);
			check(s.initCore == in.initCore, what);
			check(s.gameData == in.gameData, what);
			check(s.pythonData == in.pythonData, what);
			check(s.engineData == expect, what);
			check(s.md5.size() == 32, what);
		}
		catch (const std::exception& e)
		{
			std::printf("FAILED: %s: %s\n", what, e.what());
			++failures;
		}
	}

	// The file written with `engine`, cut `cut` bytes short of its end: read must throw.
	void truncated(const std::filesystem::path& file, const std::vector<std::byte>& engine, size_t cut, const char* what)
	{
		sample(engine).write(file);
		std::filesystem::resize_file(file, std::filesystem::file_size(file) - cut);
		try
		{
			oc4::host::SaveFile::read(file);
			std::printf("FAILED: %s (cut %zu bytes): read did not throw\n", what, cut);
			++failures;
		}
		catch (const std::runtime_error& e)
		{
			std::printf("%s (cut %zu bytes): %s\n", what, cut, e.what());
		}
	}
}

int main()
{
	const auto file = std::filesystem::temp_directory_path() / "oc4_save_file_test.CivBeyondSwordSave";
	roundTrip(file, {}, "empty engine section");
	roundTrip(file, originalEngineSection(), "original engine section");
	// The MD5 string is the last 36 bytes: int 32, 32 hex digits.
	for (const size_t cut : { size_t(1), size_t(20), size_t(32), size_t(36), size_t(37), size_t(50) })
	{
		truncated(file, {}, cut, "empty engine section, truncated");
		truncated(file, originalEngineSection(), cut, "original engine section, truncated");
	}
	std::filesystem::remove(file);
	if (failures == 0)
		std::printf("save_file_test: all passed\n");
	return failures == 0 ? 0 : 1;
}
