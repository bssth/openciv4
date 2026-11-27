// Checks a table's look as the theme gives it (Theme::table): the rows' skins as
// Civ4Theme_Table.thm and Civ4Theme_Custom.thm lay them out (the standard table's rows blank,
// every other one selection.tga tinted AltCell, a selected one tinted Select; a custom style
// mapped to it adding a rollover and its own room round a cell's content), the cell height
// through GFC_Control_Size_ListItem, and the standard table for a style the theme has not.

#include "render/Theme.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
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

	// A 2 x 2 opaque white 32-bit TGA (selection.tga is white: the tint is the colour).
	void writeTga(const fs::path& file)
	{
		std::vector<unsigned char> d(18, 0);
		d[2] = 2, d[12] = 2, d[14] = 2, d[16] = 32, d[17] = 0x28;
		for (int i = 0; i < 4; ++i)
			d.insert(d.end(), { 255, 255, 255, 255 });
		std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), std::streamsize(d.size()));
	}

	// The first pixel of a skin's single layer (BGRA, premultiplied), or null.
	const unsigned char* pixel(const Theme::Skin& skin)
	{
		return skin.size() == 1 && skin[0].image ? &skin[0].image.picture->bgra[0] : nullptr;
	}
}

int main()
{
	const fs::path dir = fs::current_path() / "theme_table_test";
	fs::remove_all(dir);
	fs::create_directories(dir / "Assets");
	fs::create_directories(dir / "Resource");
	writeTga(dir / "Resource/selection.tga");
	std::ofstream(dir / "Resource/Civ4.thm") << R"(
with GFC_Control
{
	with .Size
	{
		.ListItem = 24;
	}
}
skin SF_CtrlTheme_Civ4_Control_Skin_Blank
{
	layer
	{
	}
}
with GFC_Control_Table
{
	GFC_Control_StandardTable_Style
	{
		with .Size
		{
			.CellContentMargin = GRectMargin(4,2,4,2);
			.CellHeight = GFC_Control_Size_ListItem;
		}
		with .Flag
		{
			.NoInnerFill = 0;
		}
		with GFC_CtrlTheme_Bitmap_Control_Table_Skin
		{
			with .Inner
			{
				.Default
				{
					layer {
						LayerFlags		= GFlags(GCtrlBitmapSkin, GFC_BITMAPSKIN_CENTER_SCALE);
						DestCombine		= GColorCombine(BlendSource,Disable);
						Image			= GBitmap("selection.tga", HVGrid, 1, 1, 0, 0);
						Margin			= GRectMargin(0,0,0,0);
					}
				}
			}
			with .Row
			{
				.Default = SF_CtrlTheme_Civ4_Control_Skin_Blank;
				.Rollover = .Default;
				.Select
				{
					layer {
						LayerFlags		= GFlags(GCtrlBitmapSkin, GFC_BITMAPSKIN_CENTER_BILINEAR);
						DestCombine		= GColorCombine(BlendSource,Disable);
						FactorCombine	= GColorCombine(Modulate,Modulate);
						FactorColor		= GColor(128,168,255,89);
						Image			= GBitmap("selection.tga", HVGrid, 1, 1, 0, 0);
						Margin			= GRectMargin(0,0,0,0);
					}
				}
				.Alt_Default
				{
					layer {
						LayerFlags		= GFlags(GCtrlBitmapSkin, GFC_BITMAPSKIN_CENTER_BILINEAR);
						DestCombine		= GColorCombine(BlendSource,Disable);
						FactorCombine	= GColorCombine(Modulate,Modulate);
						FactorColor		= GColor(199,178,153,32);
						Image			= GBitmap("selection.tga", HVGrid, 1, 1, 0, 0);
						Margin			= GRectMargin(0,0,0,0);
					}
				}
				.Alt_Rollover = .Alt_Default;
			}
		}
	}

	style SF_CtrlTheme_Civ4_Control_Table_StandardCiv_Style
	{
		.Size_CellContentMargin = GRectMargin(4,1,4,2);
		with GFC_CtrlTheme_Bitmap_Control_Table_Skin
		{
			with .Row
			{
				.Rollover
				{
					layer {
						LayerFlags		= GFlags(GCtrlBitmapSkin, GFC_BITMAPSKIN_CENTER_BILINEAR);
						DestCombine		= GColorCombine(BlendSource,Disable);
						FactorCombine	= GColorCombine(Modulate,Modulate);
						FactorColor		= GColor(255,229,121,64);
						Image			= GBitmap("selection.tga", HVGrid, 1, 1, 0, 0);
						Margin			= GRectMargin(0,0,0,0);
					}
				}
				.Alt_Rollover = .Rollover;
			}
		}
		map GFC_CtrlTheme_Bitmap_Control_Table = GFC_Control_StandardTable_Style;
		map GFC_Control = GFC_Control_StandardTable_Style;
	}

	style SF_CtrlTheme_Civ4_Control_Table_Empty_Style
	{
		.Flag_NoInnerFill = GFlags(GTableCtrl, GFC_CF_TABLE_NOINNERFILL);
		map GFC_CtrlTheme_Bitmap_Control_Table = GFC_Control_StandardTable_Style;
		map GFC_Control = GFC_Control_StandardTable_Style;
	}

	style SF_CtrlTheme_Civ4_Control_Table_MainMenu_Style
	{
		.Size_CellHeight = 32;
		map GFC_CtrlTheme_Bitmap_Control_Table = GFC_Control_StandardTable_Style;
		map GFC_Control = GFC_Control_StandardTable_Style;
	}
}
)";

	using R = Theme::Table::Row;
	Theme theme;
	check(theme.load({ dir / "Assets" }), "theme loads");
	for (const std::string& p : theme.problems())
		std::printf("  theme problem: %s\n", p.c_str());

	const Theme::Table* standard = theme.table("TABLE_STYLE_STANDARD");
	check(standard != nullptr, "the standard table (TABLE_STYLE_STANDARD)");
	if (standard)
	{
		check(standard->cellHeight == 24, "cell height GFC_Control_Size_ListItem");
		check(standard->content[1] == 2 && standard->content[3] == 2, "content margin 2 above and below");
		check(standard->row[0][R::Default].empty(), "a row is blank");
		check(standard->row[0][R::Rollover].empty(), "no rollover in the standard table");
		const unsigned char* alt = pixel(standard->row[1][R::Default]);
		check(alt && alt[3] == 32 && alt[2] == 199 * 32 / 255, "every other row selection.tga in AltCell (premultiplied)");
		check(pixel(standard->row[1][R::Rollover]) != nullptr, "an alt row under the mouse keeps its look");
		const unsigned char* select = pixel(standard->row[0][R::Select]);
		check(select && select[3] == 89 && select[0] == 255 * 89 / 255, "a selected row selection.tga in Select");
		check(standard->inner.size() == 1, "the standard table's background");
	}
	const Theme::Table* empty = theme.table("TABLE_STYLE_EMPTY");
	check(empty && empty->inner.empty(), "no background with Flag_NoInnerFill (TABLE_STYLE_EMPTY)");

	const Theme::Table* civ = theme.table("Table_StandardCiv_Style");
	check(civ != nullptr && civ != standard, "BUG's Table_StandardCiv_Style");
	if (civ)
	{
		check(civ->cellHeight == 24, "its cell height the standard table's");
		check(civ->content[1] == 1 && civ->content[3] == 2, "its own content margin (1 above, 2 below)");
		const unsigned char* over = pixel(civ->row[0][R::Rollover]);
		check(over && over[3] == 64, "a row under the mouse selection.tga in Rollover");
		const unsigned char* altOver = pixel(civ->row[1][R::Rollover]);
		check(altOver && altOver[3] == 64, "an alt row under the mouse too (Alt_Rollover = .Rollover)");
		check(pixel(civ->row[1][R::Default]) != nullptr, "its alt rows the standard table's");
	}

	const Theme::Table* menu = theme.table("Table_MainMenu_Style");
	check(menu && menu->cellHeight == 32, "a style's own cell height");

	const Theme::Table* unknown = theme.table("NoSuchTable_Style");
	check(unknown && unknown->cellHeight == 24 && pixel(unknown->row[1][R::Default]) != nullptr, "an unknown style: the standard table");

	fs::remove_all(dir);
	if (failures == 0)
		std::printf("theme_table: all checks passed\n");
	return failures == 0 ? 0 : 1;
}
