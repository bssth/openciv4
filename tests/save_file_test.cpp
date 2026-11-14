#undef NDEBUG
#include "SaveFile.h"
#include <cassert>
#include <chrono>
#include <fstream>
#include <cstdint>
#include <stdexcept>

namespace fs = std::filesystem;
using oc4::host::SaveFile;

static std::vector<std::byte> bytes(const std::string& text)
{
	return { reinterpret_cast<const std::byte*>(text.data()), reinterpret_cast<const std::byte*>(text.data() + text.size()) };
}

int main()
{
	const auto temporary = fs::temp_directory_path() / ("oc4-save-test-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	fs::create_directories(temporary);
	const struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{ temporary };
	const auto file = temporary / fs::u8path("Synthetic caf\u00e9.CivBeyondSwordSave");
	SaveFile original;
	original.modFolder = "Mods\\Synthetic\\";
	original.initCore = { std::byte{0}, std::byte{255}, std::byte{42} };
	original.pythonData = std::string("\0\xffsynthetic", 11);
	const auto check = [&](const std::string& data, const std::string& md5) {
		original.gameData = bytes(data);
		original.write(file);
		const SaveFile loaded = SaveFile::read(file);
		assert(loaded.version == SaveFile::kVersion);
		assert(loaded.modFolder == original.modFolder && loaded.initCore == original.initCore);
		assert(loaded.gameData == original.gameData && loaded.pythonData == original.pythonData);
		assert(loaded.md5 == md5);
		assert(!fs::exists(fs::path(file).concat(".tmp")));
	};
	check("", "d41d8cd98f00b204e9800998ecf8427e");
	check("abc", "900150983cd24fb0d6963f7d28e17f72");
	check(std::string(200000, 'x'), "4b98146705d4b0b98b758a78ff6fb73f");
	std::string large(200000, '\0');
	uint32_t state = 1;
	for (char& c : large) { state = state * 1664525u + 1013904223u; c = char(state >> 24); }
	check(large, "c4ac56b386a4c628aeeac2212bd0d897");
	assert(fs::file_size(file) > 131072); // incompressible data crosses the container's chunk boundaries
	std::ofstream(file, std::ios::binary | std::ios::trunc) << "bad";
	bool rejected = false;
	try { (void)SaveFile::read(file); }
	catch (const std::runtime_error&) { rejected = true; }
	assert(rejected);
}
