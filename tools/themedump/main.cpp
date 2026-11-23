// themedump: reads the game's interface theme (Resource/Civ4.thm) as the map window does and
// prints what styles come to: a panel's skins, a button's skin and image in each state, the
// fonts, and the problems met.
//   themedump --civ4 <Civ4 root> [--mod <mod folder>] [style | style/property | /skin ...]

#include "core/AssetResolver.h"
#include "render/Theme.h"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace oc4;
using namespace oc4::render;

namespace
{
	void printSkin(const char* what, const Theme::Skin& skin)
	{
		for (const Theme::Layer& l : skin)
		{
			const auto bitmap = [](const Theme::Bitmap& b) {
				return b ? b.picture->path + " (" + std::to_string(int(b.x0)) + "," + std::to_string(int(b.y0)) + ")-(" + std::to_string(int(b.x1)) + "," +
						std::to_string(int(b.y1)) + ")"
						 : std::string("-");
			};
			static const char* kMiddle[] = { "stretch", "fixed", "empty", "fill" };
			std::printf("    %s: %s  margin %g,%g,%g,%g  indent %g,%g,%g,%g  middle %s%s%s\n", what, bitmap(l.image).c_str(), l.margin[0], l.margin[1],
				l.margin[2], l.margin[3], l.indent[0], l.indent[1], l.indent[2], l.indent[3], kMiddle[int(l.middle)], l.sides ? "" : ", no sides",
				l.center ? (", center " + bitmap(l.center)).c_str() : "");
		}
	}
}

int main(int argc, char** argv)
{
	fs::path civ4, mod;
	std::vector<std::string> names;
	for (int i = 1; i < argc; ++i)
	{
		const std::string a = argv[i];
		if (a == "--civ4" && i + 1 < argc)
			civ4 = argv[++i];
		else if (a == "--mod" && i + 1 < argc)
			mod = argv[++i];
		else
			names.push_back(a);
	}
	if (civ4.empty())
	{
		std::fprintf(stderr, "themedump --civ4 <Civ4 root> [--mod <mod folder>] [style ...]\n");
		return 2;
	}
	const AssetResolver assets = AssetResolver::forInstall(civ4, mod.empty() ? std::nullopt : std::optional<fs::path>(mod / "Assets"));
	Theme theme;
	const bool ok = theme.load(assets.layerRoots());
	std::printf("theme %s: %s\n", ok ? "read" : "not read", theme.file().string().c_str());
	for (int size = 1; size <= 4; ++size)
	{
		const Theme::Font f = theme.font(size), b = theme.font(size, true);
		std::printf("  font %d: %ls %g, bold %ls %g\n", size, f.family.c_str(), f.size, b.family.c_str(), b.size);
	}
	for (const std::string& n : names)
	{
		if (const Theme::Panel* p = theme.panel(n))
		{
			std::printf("%s: panel\n", n.c_str());
			printSkin("inner", p->inner);
			printSkin("border", p->border);
		}
		if (const Theme::Button* b = theme.button(n))
		{
			static const char* kStates[] = { "default", "rollover", "press", "disable", "toggle", "toggle rollover", "toggle press", "toggle disable" };
			std::printf("%s: button\n", n.c_str());
			for (size_t i = 0; i < size_t(Theme::State::Count); ++i)
			{
				printSkin((std::string(kStates[i]) + " skin").c_str(), b->skin[i]);
				if (b->image[i])
					std::printf("    %s image: %s (%d,%d)-(%d,%d)\n", kStates[i], b->image[i].picture->path.c_str(), int(b->image[i].x0), int(b->image[i].y0),
						int(b->image[i].x1), int(b->image[i].y1));
			}
		}
		if (n.find("able") != std::string::npos && n.find('/') == std::string::npos)
			if (const Theme::Table* t = theme.table(n))
			{
				// (a table style: its row's least height, the room round a cell's content, its rows)
				std::printf("%s: table, cell height %g, content margin %g,%g,%g,%g\n", n.c_str(), t->cellHeight, t->content[0], t->content[1], t->content[2],
					t->content[3]);
				printSkin("inner", t->inner);
				static const char* kRows[] = { "row", "row rollover", "row select", "row select rollover" };
				for (size_t alt = 0; alt < 2; ++alt)
					for (size_t i = 0; i < Theme::Table::RowCount; ++i)
						printSkin((std::string(alt ? "alt " : "") + kRows[i]).c_str(), t->row[alt][i]);
			}
		if (const size_t at = n.find('/'); at != std::string::npos)
		{
			// "style/property": that skin (an empty style: the theme's own skin of that name)
			std::printf("%s: skin\n", n.c_str());
			printSkin("layer", theme.skin(n.substr(0, at), n.substr(at + 1)));
		}
		else if (!theme.panel(n) && !theme.button(n))
			std::printf("%s: none\n", n.c_str());
	}
	std::printf("%zu problems\n", theme.problems().size());
	for (const std::string& p : theme.problems())
		std::printf("  %s\n", p.c_str());
	return ok ? 0 : 1;
}
