// Checks where the game's fonts are looked for (Theme::fontFolders): the original ships Sylfaen in
// Resource/Civ4 (and Resource/Fonts, Assets/res/Fonts), and a game installed in a Wine prefix has
// the Windows fonts it ran with in drive_c/windows/Fonts. A fake install is laid out here.

#include "render/Theme.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <vector>

using namespace oc4::render;
namespace fs = std::filesystem;

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
}

int main()
{
	const fs::path tmp = fs::temp_directory_path() / "oc4_game_fonts_test";
	fs::remove_all(tmp);
	const fs::path wineFonts = tmp / "drive_c" / "windows" / "Fonts";
	const fs::path civ4 = tmp / "drive_c" / "Program Files" / "Civ4";
	const fs::path bts = civ4 / "Beyond the Sword";
	for (const fs::path& p : { wineFonts, civ4 / "Assets" / "res" / "Fonts", civ4 / "Resource" / "Civ4", civ4 / "Resource" / "Fonts",
			 bts / "Assets", bts / "Resource" / "Themes" })
		fs::create_directories(p);

	Theme theme;
	theme.load({ bts / "Assets", civ4 / "Assets" }); // (no Civ4.thm: the folders are known all the same)
	const std::vector<fs::path> folders = theme.fontFolders();
	const auto at = [&](const fs::path& p) { return std::ranges::find(folders, p) - folders.begin(); };
	const long n = long(folders.size());
	check(n == 4, "four folders (the missing ones left out, the prefix's once)");
	check(at(civ4 / "Resource" / "Civ4") < n, "the theme's Resource/Civ4 (sylfaen*.ttf)");
	check(at(civ4 / "Resource" / "Fonts") < n, "Resource/Fonts");
	check(at(civ4 / "Assets" / "res" / "Fonts") < n, "Assets/res/Fonts");
	check(at(wineFonts) == n - 1, "the Wine prefix's windows/Fonts, last");

	// Not in a Wine prefix: only the game's own.
	fs::rename(tmp / "drive_c", tmp / "c");
	Theme plain;
	plain.load({ tmp / "c" / "Program Files" / "Civ4" / "Assets" });
	check(plain.fontFolders().size() == 3, "no prefix: the game's three folders");

	fs::remove_all(tmp);
	if (failures == 0)
		std::printf("game fonts: ok\n");
	return failures == 0 ? 0 : 1;
}
