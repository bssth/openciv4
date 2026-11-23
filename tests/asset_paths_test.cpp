#undef NDEBUG
#include "core/AssetResolver.h"
#include <cassert>
#include <chrono>
#include <fstream>

namespace fs = std::filesystem;

static void write(const fs::path& file, const std::string& text)
{
	fs::create_directories(file.parent_path());
	std::ofstream(file, std::ios::binary) << text;
}

int main()
{
	const auto temporary = fs::temp_directory_path() / ("oc4-asset-paths-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	const struct Cleanup { fs::path path; ~Cleanup() { fs::remove_all(path); } } cleanup{ temporary };
	const auto root = temporary / fs::path(u8"Civ4 caf\u00e9");
	// Synthetic contents: these names exercise paths, not the game's file formats.
	write(root / "Assets/Art/MiXeD.DDS", "base");
	write(root / "Warlords/Assets/XML/Test.XML", "warlords");
	write(root / "Beyond the Sword/Assets/Art/Mixed.dds", "bts");
	write(root / "Beyond the Sword/Assets/Python/CvMixed.py", "synthetic");
	write(root / "Mod/Assets/aRT/MIXED.dds", "mod");
	{
		auto assets = oc4::AssetResolver::forInstall(root);
		const auto art = assets.locate("ASSETS\\ART\\mixed.dds");
		assert(art && fs::equivalent(art->file, root / "Beyond the Sword/Assets/Art/Mixed.dds"));
		const auto data = assets.read(*art);
		assert(std::string(reinterpret_cast<const char*>(data.data()), data.size()) == "bts");
		const auto xml = assets.locate("xml/test.xml");
		assert(xml && fs::equivalent(xml->file, root / "Warlords/Assets/XML/Test.XML"));
		assert(assets.list(".DDS").size() == 1);
		const auto scripts = assets.list(".py");
		assert(scripts.size() == 1 && fs::equivalent(scripts.front().file, root / "Beyond the Sword/Assets/Python/CvMixed.py"));
		assert(!assets.locate("art/missing.dds"));
		assets.addLayer(root / "MissingOptionalLayer");
		assert(!assets.locate("art/missing.dds"));
	}
	{
		auto assets = oc4::AssetResolver::forInstall(root, root / "Mod/Assets");
		const auto art = assets.locate("art/mixed.dds");
		assert(art && fs::equivalent(art->file, root / "Mod/Assets/aRT/MIXED.dds"));
		const auto listed = assets.list(".dds");
		assert(listed.size() == 1 && fs::equivalent(listed.front().file, art->file));
	}
}
