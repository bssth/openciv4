// Flag alpha selects player colours; custom white flags keep RGB even where alpha is zero.
#include "render/ButtonArt.h"
#include "render/MapSnapshot.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>

using namespace oc4;
using namespace oc4::render;

int main()
{
	const auto dir = std::filesystem::temp_directory_path() / ("oc4_flag_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(dir);
	const unsigned char tga[] = {
		0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 3, 0, 1, 0, 32, 0x28,
		255, 255, 255, 0, 255, 255, 255, 255, 128, 128, 128, 128
	};
	{
		std::ofstream file(dir / "flag.tga", std::ios::binary);
		file.write(reinterpret_cast<const char*>(tga), sizeof(tga));
	}
	int failures = 0;
	const auto check = [&](bool ok, const char* what) {
		if (!ok) { std::printf("FAILED: %s\n", what); ++failures; }
	};
	{
		AssetResolver assets;
		assets.addLayer(dir);
		ButtonArt art(assets);
		const Color red{ 1, 0, 0 }, blue{ 0, 0, 1 };
		const auto* flag = art.flag("flag.tga", red, blue, false);
		check(flag && flag->width == 3 && flag->height == 1, "decal loaded");
		if (flag)
		{
			check(flag->bgra[2] == 255 && flag->bgra[0] == 0 && flag->bgra[3] == 255, "zero alpha uses primary colour, opaque cloth");
			check(flag->bgra[4] == 255 && flag->bgra[6] == 0 && flag->bgra[7] == 255, "full alpha uses secondary colour");
			check(flag->bgra[8] == 64 && flag->bgra[10] == 64 && flag->bgra[11] == 255, "partial mask blends colours and retains texture shading");
			check(art.flag("FLAG.TGA", red, blue, false) == flag, "same decal and colours reuse the cached image");
			const auto* recoloured = art.flag("flag.tga", blue, red, false);
			check(recoloured && recoloured->key != flag->key && recoloured->bgra[0] == 255, "same decal can use another player's colours");
		}
		const auto* white = art.flag("flag.tga", red, blue, true);
		check(white && white->bgra[0] == 255 && white->bgra[2] == 255 && white->bgra[3] == 255 && white->bgra[8] == 128,
			"white flag keeps its original RGB and ignores alpha");
		const auto* ordinary = art.icon("flag.tga");
		check(ordinary && ordinary->bgra[3] == 0, "flag composition does not alter ordinary image transparency");
		check(flag && ordinary && flag->key != ordinary->key, "flags and UI icons have separate texture keys");
		check(!art.flag("missing.tga", red, blue, false), "missing decal permits a fallback");
	}
	std::filesystem::remove(dir / "flag.tga");
	std::filesystem::remove(dir);
	if (!failures) std::puts("flag_art_test: all passed");
	return failures ? 1 : 0;
}
