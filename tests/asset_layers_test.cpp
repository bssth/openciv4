// Checks the order AssetResolver::listByLayer gives files across layers (src/core/AssetResolver.h):
// the lowest layer first, each layer's in key order, a file a higher layer replaces listed once,
// at its higher layer. Civ4 reads xml/text in this order and keeps a key's last text, so
// "civ4gametext_bts.xml" (BtS) must come after "civ4gametextinfos.xml" (the base game), though
// its name sorts first.

#include "core/AssetResolver.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using oc4::AssetResolver;

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

	void touch(const fs::path& p)
	{
		fs::create_directories(p.parent_path());
		std::ofstream(p) << "<x/>";
	}
}

int main()
{
	const fs::path root = fs::temp_directory_path() / "oc4_asset_layers_test";
	fs::remove_all(root);
	const fs::path mod = root / "Mod/Assets", bts = root / "Beyond the Sword/Assets", base = root / "Assets";
	touch(base / "XML/Text/CIV4GameTextInfos.xml");
	touch(base / "XML/Text/CIV4GameText_Help.xml");
	touch(bts / "XML/Text/CIV4GameText_BTS.xml");
	touch(bts / "XML/Text/CIV4GameText_Events_BTS.xml");
	touch(mod / "XML/Text/BUG_CIV4GameText.xml");
	touch(mod / "XML/Text/CIV4GameText_Events_BTS.xml"); // replaces BtS's file of that name

	AssetResolver assets;
	assets.addLayer(mod); // highest first, as forInstall adds them
	assets.addLayer(bts);
	assets.addLayer(base);

	std::vector<std::string> keys;
	std::vector<size_t> layers;
	for (const auto& loc : assets.listByLayer(".xml"))
	{
		keys.push_back(loc.key);
		layers.push_back(loc.layer);
	}
	const std::vector<std::string> want{
		"xml/text/civ4gametext_help.xml", "xml/text/civ4gametextinfos.xml", // the base game
		"xml/text/civ4gametext_bts.xml", // BtS
		"xml/text/bug_civ4gametext.xml", "xml/text/civ4gametext_events_bts.xml", // the mod
	};
	check(keys == want, "lowest layer first, each layer in key order");
	check(layers == std::vector<size_t>{ 2, 2, 1, 0, 0 }, "a file a higher layer replaces is listed once, at that layer");
	if (keys != want)
		for (const auto& k : keys)
			std::printf("  %s\n", k.c_str());

	// list() keeps its plain key order.
	std::vector<std::string> plain;
	for (const auto& loc : assets.list(".xml"))
		plain.push_back(loc.key);
	check(plain.size() == 5 && plain.front() == "xml/text/bug_civ4gametext.xml" && plain.back() == "xml/text/civ4gametextinfos.xml", "list() in key order");

	fs::remove_all(root);
	if (failures == 0)
		std::printf("asset_layers: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
