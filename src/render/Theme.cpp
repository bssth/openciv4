#include "render/Theme.h"

#include "render/Overlay.h"
#include "core/FileUtil.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <unordered_map>

namespace oc4::render
{
	namespace fs = std::filesystem;

	namespace
	{
		// ---------------------------------------------------------------- values

		struct Value;
		using RawLayer = std::map<std::string, Value>;

		struct Value
		{
			enum class Kind : uint8_t
			{
				None, Number, String, Name, Call, Skin, Index
			};
			Kind kind = Kind::None;
			double number = 0;
			std::string text;               // a string, a name, a call's function, an index's style
			std::vector<Value> args;        // a call's arguments; an index's property (one name)
			std::vector<RawLayer> layers;   // a skin's
		};

		struct Style
		{
			std::unordered_map<std::string, Value> props;
			std::vector<std::pair<std::string, std::string>> maps; // a namespace and the style its properties come from
		};

		// ---------------------------------------------------------------- tokens

		struct Token
		{
			enum class Kind : uint8_t
			{
				End, Name, String, Number, Punct
			};
			Kind kind = Kind::End;
			std::string text;
			double number = 0;
		};

		bool nameChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.'; }

		std::vector<Token> tokenize(const std::string& s)
		{
			std::vector<Token> out;
			const size_t n = s.size();
			for (size_t i = 0; i < n;)
			{
				const char c = s[i];
				if (std::isspace(static_cast<unsigned char>(c)))
					++i;
				else if (c == '/' && i + 1 < n && s[i + 1] == '/')
					while (i < n && s[i] != '\n')
						++i;
				else if (c == '/' && i + 1 < n && s[i + 1] == '*')
				{
					const size_t end = s.find("*/", i + 2);
					i = end == std::string::npos ? n : end + 2;
				}
				else if (c == '"')
				{
					size_t j = i + 1;
					while (j < n && s[j] != '"')
						++j;
					out.push_back({ Token::Kind::String, s.substr(i + 1, j - i - 1) });
					i = j + 1;
				}
				else if (std::isdigit(static_cast<unsigned char>(c)) ||
					((c == '-' || c == '+' || c == '.') && i + 1 < n && std::isdigit(static_cast<unsigned char>(s[i + 1]))))
				{
					char* end = nullptr;
					Token t{ Token::Kind::Number };
					t.number = std::strtod(s.c_str() + i, &end);
					i = std::max(size_t(end - s.c_str()), i + 1);
					out.push_back(std::move(t));
				}
				else if (nameChar(c))
				{
					size_t j = i;
					while (j < n && nameChar(s[j]))
						++j;
					out.push_back({ Token::Kind::Name, s.substr(i, j - i) });
					i = j;
				}
				else
				{
					out.push_back({ Token::Kind::Punct, std::string(1, c) });
					++i;
				}
			}
			out.push_back({});
			return out;
		}

		struct Reader
		{
			std::vector<Token> tokens;
			size_t at = 0;

			const Token& peek(size_t k = 0) const { return tokens[std::min(at + k, tokens.size() - 1)]; }
			Token next()
			{
				Token t = peek();
				if (at < tokens.size() - 1)
					++at;
				return t;
			}
			bool punct(char c, size_t k = 0) const { return peek(k).kind == Token::Kind::Punct && peek(k).text[0] == c; }
			bool take(char c)
			{
				if (!punct(c))
					return false;
				next();
				return true;
			}
			bool end() const { return peek().kind == Token::Kind::End; }
		};

		// A property's full name: ".Name" continues the scope's.
		std::string fullName(const std::string& scope, const std::string& name)
		{
			if (name.empty() || name[0] != '.')
				return name;
			std::string rest = name.substr(1);
			std::ranges::replace(rest, '.', '_');
			return scope.empty() ? rest : scope + "_" + rest;
		}

		std::string lower(std::string s)
		{
			for (char& c : s)
				c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		// ---------------------------------------------------------------- TGA pictures

		// Uncompressed or RLE true colour TGA, 24 or 32 bits, into premultiplied BGRA top-down.
		bool readTga(const std::vector<char>& data, std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height)
		{
			if (data.size() < 18)
				return false;
			const auto u8 = [&](size_t i) { return uint8_t(data[i]); };
			const uint8_t idLength = u8(0), colourMap = u8(1), type = u8(2);
			const uint32_t mapLength = u8(5) | (u8(6) << 8), mapBits = u8(7);
			width = u8(12) | (u8(13) << 8);
			height = u8(14) | (u8(15) << 8);
			const uint32_t bits = u8(16);
			const bool topDown = (u8(17) & 0x20) != 0;
			if ((type != 2 && type != 10) || (bits != 24 && bits != 32) || width == 0 || height == 0)
				return false;
			size_t p = 18 + idLength + (colourMap ? size_t(mapLength) * ((mapBits + 7) / 8) : 0);
			const size_t bpp = bits / 8, pixels = size_t(width) * height;
			std::vector<uint8_t> raw(pixels * 4);
			const auto pixel = [&](size_t i, size_t from) {
				raw[i * 4 + 0] = u8(from + 0);
				raw[i * 4 + 1] = u8(from + 1);
				raw[i * 4 + 2] = u8(from + 2);
				raw[i * 4 + 3] = bpp == 4 ? u8(from + 3) : 255;
			};
			if (type == 2)
			{
				if (data.size() < p + pixels * bpp)
					return false;
				for (size_t i = 0; i < pixels; ++i)
					pixel(i, p + i * bpp);
			}
			else
				for (size_t i = 0; i < pixels && p < data.size();)
				{
					const uint8_t head = u8(p++);
					const size_t count = (head & 0x7F) + 1;
					if (head & 0x80)
					{
						if (p + bpp > data.size())
							return false;
						for (size_t k = 0; k < count && i < pixels; ++k)
							pixel(i++, p);
						p += bpp;
					}
					else
						for (size_t k = 0; k < count && i < pixels; ++k, p += bpp)
						{
							if (p + bpp > data.size())
								return false;
							pixel(i++, p);
						}
				}
			bgra.resize(pixels * 4);
			for (uint32_t y = 0; y < height; ++y)
			{
				const uint8_t* src = &raw[size_t(topDown ? y : height - 1 - y) * width * 4];
				uint8_t* dst = &bgra[size_t(y) * width * 4];
				for (uint32_t x = 0; x < width; ++x, src += 4, dst += 4)
				{
					const uint32_t a = src[3];
					dst[0] = uint8_t(src[0] * a / 255);
					dst[1] = uint8_t(src[1] * a / 255);
					dst[2] = uint8_t(src[2] * a / 255);
					dst[3] = uint8_t(a);
				}
			}
			return true;
		}

		// The enums the scripts pass for styles, by their names, as the theme has them.
		const std::map<std::string, std::string, std::less<>>& enumStyles()
		{
			static const std::map<std::string, std::string, std::less<>> m = {
				{ "PANEL_STYLE_STANDARD", "GFC_Control_StandardPanel_Style" }, { "PANEL_STYLE_SOLID", "GFC_Control_SolidPanel_Style" },
				{ "PANEL_STYLE_EMPTY", "GFC_Control_EmptyPanel_Style" }, { "PANEL_STYLE_FLAT", "GFC_Control_FlatPanel_Style" },
				{ "PANEL_STYLE_IN", "GFC_Control_InPanel_Style" }, { "PANEL_STYLE_OUT", "GFC_Control_OutPanel_Style" },
				{ "PANEL_STYLE_EXTERNAL", "GFC_Control_ExternalPanel_Style" }, { "PANEL_STYLE_DEFAULT", "GFC_Control_DefaultPanel_Style" },
				{ "PANEL_STYLE_UNITSTAT", "Panel_Game_HudStat_Style" }, { "PANEL_STYLE_BLUELARGE", "Panel_BlueLarge_Style" },
				{ "PANEL_STYLE_BLUE50", "Panel_Blue50_Style" }, { "PANEL_STYLE_TOPBAR", "Panel_TopBar_Style" },
				{ "PANEL_STYLE_BOTTOMBAR", "Panel_BottomBar_Style" }, { "PANEL_STYLE_TECH", "Panel_Tech_Style" },
				{ "PANEL_STYLE_GAMEHUD_LEFT", "Panel_Game_HudBL_Style" }, { "PANEL_STYLE_GAMEHUD_RIGHT", "Panel_Game_HudBR_Style" },
				{ "PANEL_STYLE_GAMEHUD_CENTER", "Panel_Game_HudBC_Style" }, { "PANEL_STYLE_GAMEHUD_STATS", "Panel_Game_HudStat_Style" },
				{ "PANEL_STYLE_GAME_MAP", "Panel_Game_HudMap_Style" }, { "PANEL_STYLE_GAME_TOPBAR", "Panel_Game_HudTC_Style" },
				{ "PANEL_STYLE_HUD_HELP", "Panel_HudHelp_Style" }, { "PANEL_STYLE_CITY_LEFT", "Panel_City_Left_Style" },
				{ "PANEL_STYLE_CITY_RIGHT", "Panel_City_Right_Style" }, { "PANEL_STYLE_CITY_TOP", "Panel_City_Top_Style" },
				{ "PANEL_STYLE_CITY_TANSHADE", "Panel_City_TanShade_Style" }, { "PANEL_STYLE_CITY_INFO", "Panel_City_Info_Style" },
				{ "PANEL_STYLE_CITY_TANTL", "Panel_City_TanTL_Style" }, { "PANEL_STYLE_CITY_TANTR", "Panel_City_TanTR_Style" },
				{ "PANEL_STYLE_CITY_COLUMNL", "Panel_City_ColumnL_Style" }, { "PANEL_STYLE_CITY_COLUMNC", "Panel_City_ColumnC_Style" },
				{ "PANEL_STYLE_CITY_COLUMNR", "Panel_City_ColumnR_Style" }, { "PANEL_STYLE_CITY_TITLE", "Panel_City_Title_Style" },
				{ "PANEL_STYLE_DAWN", "Panel_Dawn_Style" }, { "PANEL_STYLE_DAWNTOP", "Panel_DawnTop_Style" },
				{ "PANEL_STYLE_DAWNBOTTOM", "Panel_DawnBottom_Style" }, { "PANEL_STYLE_MAIN", "Panel_Main_Style" },
				{ "PANEL_STYLE_MAIN_BLACK25", "Panel_Black25_Style" }, { "PANEL_STYLE_MAIN_BLACK50", "Panel_Black50_Style" },
				{ "PANEL_STYLE_MAIN_WHITE", "Panel_WhitePanel_Style" }, { "PANEL_STYLE_MAIN_WHITETAB", "Panel_WhiteTab_Style" },
				{ "PANEL_STYLE_MAIN_TAN", "Panel_TanPanel_Style" }, { "PANEL_STYLE_MAIN_TAN15", "Panel_Tan15_Style" },
				{ "PANEL_STYLE_MAIN_TANL", "Panel_TanL_Style" }, { "PANEL_STYLE_MAIN_TANR", "Panel_TanR_Style" },
				{ "PANEL_STYLE_MAIN_TANT", "Panel_TanT_Style" }, { "PANEL_STYLE_MAIN_TANB", "Panel_TanB_Style" },
				{ "PANEL_STYLE_MAIN_BOTTOMBAR", "Panel_BottomBar_Style" }, { "PANEL_STYLE_MAIN_SELECT", "Panel_Select_Style" },
				{ "BUTTON_STYLE_STANDARD", "GFC_Control_StandardButton_Style" }, { "BUTTON_STYLE_ETCHED", "GFC_Control_EtchedButton_Style" },
				{ "BUTTON_STYLE_FLAT", "GFC_Control_FlatButton_Style" }, { "BUTTON_STYLE_IMAGE", "GFC_Control_ImageButton_Style" },
				{ "BUTTON_STYLE_LABEL", "GFC_Control_LabelButton_Style" }, { "BUTTON_STYLE_LINK", "GFC_Control_LinkButton_Style" },
				{ "BUTTON_STYLE_SQUARE", "GFC_Control_SquareButton_Style" }, { "BUTTON_STYLE_TOOL", "GFC_Control_ToolButton_Style" },
				{ "BUTTON_STYLE_DEFAULT", "GFC_Control_DefaultButton_Style" },
				{ "BUTTON_STYLE_CITY_B02TL", "Button_CityBtn02_Style" }, { "BUTTON_STYLE_CITY_B02TR", "Button_CityBtn03_Style" },
				{ "BUTTON_STYLE_CITY_B02BL", "Button_CityBtn04_Style" }, { "BUTTON_STYLE_CITY_B02BR", "Button_CityBtn05_Style" },
				{ "BUTTON_STYLE_CITY_B03TL", "Button_CityBtn06_Style" }, { "BUTTON_STYLE_CITY_B03TC", "Button_CityBtn07_Style" },
				{ "BUTTON_STYLE_CITY_B03TR", "Button_CityBtn08_Style" }, { "BUTTON_STYLE_CITY_B03BL", "Button_CityBtn09_Style" },
				{ "BUTTON_STYLE_CITY_B03BC", "Button_CityBtn10_Style" }, { "BUTTON_STYLE_CITY_B03BR", "Button_CityBtn11_Style" },
				{ "BUTTON_STYLE_CITY_PLUS", "Button_Plus_Style" }, { "BUTTON_STYLE_CITY_MINUS", "Button_Minus_Style" },
				{ "BUTTON_STYLE_ARROW_LEFT", "Button_CityArrowL_Style" }, { "BUTTON_STYLE_ARROW_RIGHT", "Button_CityArrowR_Style" },
			};
			return m;
		}

		constexpr const char* kStates[] = { "Default", "Rollover", "Press", "Disable", "Toggle", "ToggleRollover", "TogglePress", "ToggleDisable" };
	}

	bool Theme::Button::empty() const
	{
		return std::ranges::all_of(skin, [](const Skin& s) { return s.empty(); }) && std::ranges::none_of(image, [](const Bitmap& b) { return bool(b); }) &&
			std::ranges::all_of(mark, [](const Skin& s) { return s.empty(); });
	}

	struct Theme::Impl
	{
		std::vector<fs::path> roots;  // the folders whose Resource the theme's pictures come from, highest first
		fs::path base;                // the folder the theme is in (its Resource's parent): includes start there
		fs::path resources;           // its resource_path (from the file that says it): the pictures
		std::unordered_map<std::string, Style> styles;
		Style global;
		std::unordered_map<std::string, const Value*> anywhereCache;
		std::map<std::string, std::unique_ptr<Picture>> pictures; // by path and tint (null: missing)
		std::map<std::string, std::unique_ptr<Panel>, std::less<>> panels;   // (null: the theme has none)
		std::map<std::string, std::unique_ptr<Button>, std::less<>> buttons;
		std::map<std::string, std::unique_ptr<Progress>, std::less<>> progresses;
		std::vector<std::string> problems;
		std::set<std::string> told;
		bool loaded = false;
		uint64_t nextKey = 0;

		void problem(const std::string& what)
		{
			if (told.insert(what).second && problems.size() < 200)
				problems.push_back(what);
		}

		// ------------------------------------------------ reading

		void parseFile(const fs::path& file, int depth)
		{
			std::ifstream in(file, std::ios::binary);
			if (!in || depth > 8)
			{
				problem("theme file " + file.string() + " not read");
				return;
			}
			std::stringstream ss;
			ss << in.rdbuf();
			Reader r{ tokenize(ss.str()) };
			while (!r.end())
			{
				parseStatements(r, "", nullptr, file.parent_path(), depth);
				if (!r.end())
					r.next(); // (a stray '}')
			}
		}

		// A mod's theme names its files from the game folder, "Mods\<its folder>\Assets\...":
		// that is the mod's own folder, wherever it is and whatever it is called (CoM's work
		// copy says "Mods\Chronicles of Mankind 1.3\", its author's folder). The rest of the
		// path, under each layer's root (the mod's first).
		std::optional<fs::path> modPath(const std::string& name, bool directory) const
		{
			const fs::path p = fs::path(name).lexically_normal();
			auto it = p.begin();
			if (it == p.end() || lower(utf8Path(*it)) != "mods" || ++it == p.end() || ++it == p.end())
				return std::nullopt;
			fs::path rest;
			for (; it != p.end(); ++it)
				rest /= *it;
			for (const fs::path& root : roots)
				if (std::error_code ec; directory ? fs::is_directory(root / rest, ec) : fs::is_regular_file(root / rest, ec))
					return root / rest;
			return std::nullopt;
		}

		void include(const fs::path& from, const std::string& name, int depth)
		{
			for (const fs::path& dir : { from, base })
				if (std::error_code ec; fs::is_regular_file(dir / name, ec))
				{
					parseFile(dir / name, depth + 1);
					return;
				}
			if (const auto file = modPath(name, false))
			{
				parseFile(*file, depth + 1);
				return;
			}
			problem("theme file " + name + " not found");
		}

		Value parseValue(Reader& r, const std::string& scope)
		{
			Value v;
			const Token t = r.next();
			if (t.kind == Token::Kind::String)
			{
				v.kind = Value::Kind::String;
				v.text = t.text;
			}
			else if (t.kind == Token::Kind::Number)
			{
				v.kind = Value::Kind::Number;
				v.number = t.number;
			}
			else if (t.kind == Token::Kind::Name)
			{
				v.text = fullName(scope, t.text);
				if (r.take('('))
				{
					v.kind = Value::Kind::Call;
					v.text = t.text;
					while (!r.end() && !r.take(')'))
					{
						v.args.push_back(parseValue(r, scope));
						r.take(',');
					}
				}
				else if (r.take('['))
				{
					v.kind = Value::Kind::Index;
					v.args.push_back(parseValue(r, scope));
					r.take(']');
				}
				else
					v.kind = Value::Kind::Name;
			}
			return v;
		}

		Value parseSkin(Reader& r, const std::string& scope)
		{
			Value v;
			v.kind = Value::Kind::Skin;
			if (!r.take('{'))
				return v;
			while (!r.end() && !r.take('}'))
			{
				if (r.peek().kind == Token::Kind::Name && r.peek().text == "layer" && r.punct('{', 1))
				{
					r.next();
					r.next();
					RawLayer layer;
					while (!r.end() && !r.take('}'))
					{
						const Token key = r.next();
						if (key.kind == Token::Kind::Name && r.take('='))
						{
							layer[key.text] = parseValue(r, scope);
							r.take(';');
						}
					}
					v.layers.push_back(std::move(layer));
				}
				else
					r.next();
			}
			return v;
		}

		void parseStatements(Reader& r, const std::string& scope, Style* style, const fs::path& dir, int depth)
		{
			const auto store = [&](const std::string& name, Value v) { (style ? style->props : global.props)[name] = std::move(v); };
			while (!r.end() && !r.punct('}'))
			{
				if (r.take(';'))
					continue;
				if (r.peek().kind != Token::Kind::Name)
				{
					problem("theme: unexpected '" + r.next().text + "'");
					continue;
				}
				const std::string word = r.next().text;
				if (word == "include" || word == "resource_path")
				{
					const Token file = r.next();
					r.take(';');
					if (word == "include")
						include(dir, file.text, depth);
					else if (const auto mod = modPath(file.text, true); mod && !fs::is_directory(dir / file.text))
						resources = mod->lexically_normal();
					else
						resources = (dir / file.text).lexically_normal();
				}
				else if (word == "with" || word == "style")
				{
					const std::string name = fullName(scope, r.next().text);
					if (r.take('{'))
					{
						if (word == "with")
							parseStatements(r, name, style, dir, depth);
						else
							parseStatements(r, scope, &styles[name], dir, depth);
						r.take('}');
					}
				}
				else if (word == "skin")
				{
					const std::string name = fullName(scope, r.next().text);
					store(name, parseSkin(r, scope));
				}
				else if (word == "map")
				{
					const std::string from = fullName(scope, r.next().text);
					r.take('=');
					const std::string to = fullName(scope, r.next().text);
					r.take(';');
					if (style)
						style->maps.push_back({ from, to });
				}
				else
				{
					std::string name = word;
					if (r.peek().kind == Token::Kind::Name && r.punct('=', 1)) // (a typed one: GColor .AltText = ...)
						name = r.next().text;
					if (r.take('='))
					{
						Value v = parseValue(r, scope);
						if (r.take(':')) // (": GFC_Control_Color" after a value)
							r.next();
						r.take(';');
						store(fullName(scope, name), std::move(v));
					}
					else if (r.punct('{'))
					{
						// A skin (its layers), else a style of that name.
						if ((r.peek(1).kind == Token::Kind::Name && r.peek(1).text == "layer") || r.punct('}', 1))
							store(fullName(scope, name), parseSkin(r, scope));
						else
						{
							r.next();
							parseStatements(r, scope, &styles[fullName(scope, name)], dir, depth);
							r.take('}');
						}
					}
					else
						problem("theme: unexpected " + name);
				}
			}
		}

		// ------------------------------------------------ looking up

		const Style* findStyle(const std::string& name) const
		{
			const auto it = styles.find(name);
			return it == styles.end() ? nullptr : &it->second;
		}

		const Value* lookup(const Style* s, const std::string& name, int depth = 0) const
		{
			if (!s || depth > 24)
				return nullptr;
			if (const auto it = s->props.find(name); it != s->props.end())
				return &it->second;
			for (const auto& [ns, target] : s->maps)
				if (name.compare(0, ns.size(), ns) == 0 && (name.size() == ns.size() || name[ns.size()] == '_'))
					if (const Value* v = lookup(findStyle(target), name, depth + 1))
						return v;
			return nullptr;
		}

		const Value* anywhere(const std::string& name)
		{
			if (const auto it = anywhereCache.find(name); it != anywhereCache.end())
				return it->second;
			const Value* found = nullptr;
			if (const auto it = global.props.find(name); it != global.props.end())
				found = &it->second;
			for (auto i = styles.begin(); !found && i != styles.end(); ++i)
				if (const auto it = i->second.props.find(name); it != i->second.props.end())
					found = &it->second;
			anywhereCache[name] = found;
			return found;
		}

		const Value* property(const Style* s, const std::string& name)
		{
			if (const Value* v = lookup(s, name))
				return v;
			const auto it = global.props.find(name);
			return it == global.props.end() ? nullptr : &it->second;
		}

		// What a name stands for: the style's property, the theme's, or the one of any style that
		// has it (the common styles define the colours); a word that is none (HVGrid) is itself.
		const Value* resolve(const Style* s, const Value* v, int depth = 0)
		{
			for (; v && depth < 24; ++depth)
			{
				if (v->kind == Value::Kind::Index && !v->args.empty())
				{
					const Style* other = findStyle(v->text);
					s = other;
					v = property(other, v->args[0].text);
					continue;
				}
				if (v->kind != Value::Kind::Name)
					return v;
				const Value* next = property(s, v->text);
				if (!next)
					next = anywhere(v->text);
				if (!next || next == v)
					return v;
				v = next;
			}
			return v;
		}

		static double number(const Value& call, size_t i, double fallback = 0)
		{
			return i < call.args.size() && call.args[i].kind == Value::Kind::Number ? call.args[i].number : fallback;
		}

		bool colour(const Style* s, const Value* v, Color& c, float& alpha)
		{
			v = resolve(s, v);
			if (!v || v->kind != Value::Kind::Call || v->text != "GColor")
				return false;
			if (!v->args.empty() && v->args[0].kind == Value::Kind::Name)
			{
				const std::string& n = v->args[0].text;
				c = n == "White" ? Color{ 1, 1, 1 } : n == "Yellow" ? Color{ 1, 1, 0 } : Color{ 0, 0, 0 };
				alpha = 1;
				return true;
			}
			c = { float(number(*v, 0) / 255), float(number(*v, 1) / 255), float(number(*v, 2) / 255) };
			alpha = float(number(*v, 3, 255) / 255);
			return true;
		}

		void margins(const Style* s, const Value* v, float out[4])
		{
			v = resolve(s, v);
			if (!v || v->kind != Value::Kind::Call || v->text != "GRectMargin")
				return;
			if (v->args.size() == 1)
				out[0] = out[1] = out[2] = out[3] = float(number(*v, 0));
			else
				for (size_t i = 0; i < 4; ++i)
					out[i] = float(number(*v, i));
		}

		const Picture* picture(const std::string& path, const Color* tint, float tintAlpha)
		{
			std::string key = lower(path);
			if (tint)
				key += "|" + std::to_string(int(tint->r * 255)) + "," + std::to_string(int(tint->g * 255)) + "," + std::to_string(int(tint->b * 255)) +
					"," + std::to_string(int(tintAlpha * 255));
			if (const auto it = pictures.find(key); it != pictures.end())
				return it->second.get();
			std::unique_ptr<Picture> p;
			if (tint)
			{
				if (const Picture* plain = picture(path, nullptr, 1))
				{
					p = std::make_unique<Picture>(*plain);
					for (size_t i = 0; i + 3 < p->bgra.size(); i += 4)
					{
						p->bgra[i + 0] = uint8_t(p->bgra[i + 0] * tint->b * tintAlpha);
						p->bgra[i + 1] = uint8_t(p->bgra[i + 1] * tint->g * tintAlpha);
						p->bgra[i + 2] = uint8_t(p->bgra[i + 2] * tint->r * tintAlpha);
						p->bgra[i + 3] = uint8_t(p->bgra[i + 3] * tintAlpha);
					}
				}
			}
			else
			{
				std::string rel = path;
				std::ranges::replace(rel, '\\', '/');
				std::vector<fs::path> where{ resources / rel };
				for (const fs::path& root : roots)
					where.push_back(root / "Resource" / rel);
				for (const fs::path& file : where)
				{
					std::ifstream in(file, std::ios::binary);
					if (!in)
						continue;
					const std::vector<char> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
					auto pic = std::make_unique<Picture>();
					if (readTga(data, pic->bgra, pic->width, pic->height))
						p = std::move(pic);
					else
						problem("theme picture " + path + " not readable");
					break;
				}
				if (!p && !told.contains("theme picture " + path + " not readable"))
					problem("theme picture " + path + " not found");
			}
			if (p)
			{
				p->key = (uint64_t(0x7E) << 56) | ++nextKey;
				p->path = path;
			}
			return (pictures[key] = std::move(p)).get();
		}

		// The bitmap added to what is under it (the Add combine: a button's rollover glow, black
		// where it adds nothing): its colours kept and its alpha cleared, which the overlay's
		// premultiplied blending (one, one less the alpha) then adds.
		Bitmap additive(const Bitmap& b)
		{
			if (!b)
				return b;
			auto& slot = pictures["|add|" + std::to_string(b.picture->key)]; // (by its own key: a tinted one is another picture)
			if (!slot)
			{
				slot = std::make_unique<Picture>(*b.picture);
				for (size_t i = 3; i < slot->bgra.size(); i += 4)
					slot->bgra[i] = 0;
				slot->key = (uint64_t(0x7E) << 56) | ++nextKey;
			}
			Bitmap out = b;
			out.picture = slot.get();
			return out;
		}

		Bitmap bitmap(const Style* s, const Value* v, const Color* tint = nullptr, float tintAlpha = 1)
		{
			v = resolve(s, v);
			if (!v || v->kind != Value::Kind::Call || v->text != "GBitmap" || v->args.empty() || v->args[0].kind != Value::Kind::String)
				return {};
			const Picture* p = picture(v->args[0].text, tint, tintAlpha);
			if (!p)
				return {};
			int cols = 1, rows = 1, col = 0, row = 0;
			const std::string mode = v->args.size() > 1 && v->args[1].kind == Value::Kind::Name ? v->args[1].text : "";
			if (mode == "HVGrid")
			{
				cols = int(number(*v, 2, 1));
				rows = int(number(*v, 3, 1));
				col = int(number(*v, 4));
				row = int(number(*v, 5));
			}
			else if (mode == "Vertical")
			{
				rows = int(number(*v, 2, 1));
				row = int(number(*v, 3));
			}
			else if (mode == "Horizontal")
			{
				cols = int(number(*v, 2, 1));
				col = int(number(*v, 3));
			}
			cols = std::max(cols, 1);
			rows = std::max(rows, 1);
			const float cw = float(p->width / uint32_t(cols)), ch = float(p->height / uint32_t(rows));
			Bitmap b{ p, cw * float(col), ch * float(row), cw * float(col + 1), ch * float(row + 1) };
			if (b.x1 > float(p->width) || b.y1 > float(p->height))
				return {};
			return b;
		}

		void apply(const Style* s, const RawLayer& raw, Layer& l)
		{
			const auto has = [&](const char* key) { return raw.find(key) != raw.end() ? &raw.at(key) : nullptr; };
			Color tint{ 1, 1, 1 };
			float tintAlpha = 1;
			bool tinted = false;
			if (const Value* f = has("FactorColor"))
				if (const Value* combine = resolve(s, has("FactorCombine"));
					combine && combine->kind == Value::Kind::Call && !combine->args.empty() && combine->args[0].text == "Modulate")
					tinted = colour(s, f, tint, tintAlpha);
			if (const Value* v = has("Image"))
				l.image = bitmap(s, v, tinted ? &tint : nullptr, tintAlpha);
			if (const Value* v = has("Center"))
				l.center = bitmap(s, v, tinted ? &tint : nullptr, tintAlpha);
			if (const Value* combine = resolve(s, has("DestCombine"));
				combine && combine->kind == Value::Kind::Call && !combine->args.empty() && combine->args[0].text == "Add")
			{
				l.image = additive(l.image);
				l.center = additive(l.center);
			}
			if (const Value* v = has("Margin"))
				margins(s, v, l.margin);
			if (const Value* v = has("Indent"))
				margins(s, v, l.indent);
			if (const Value* v = has("CenterColor"))
				colour(s, v, l.fill, l.fillAlpha);
			static const char* const kSections[] = { "LeftFlags", "TopFlags", "RightFlags", "BottomFlags" };
			for (int i = 0; i < 4; ++i)
				if (const Value* v = resolve(s, has(kSections[i])); v && v->kind == Value::Kind::Call)
					for (const Value& f : v->args)
						if (f.text == "GFC_BITMAPSKIN_SECTION_EMPTY")
							l.section[i] = false;
			if (const Value* v = resolve(s, has("LayerFlags")); v && v->kind == Value::Kind::Call)
				for (const Value& f : v->args)
				{
					if (f.text == "GFC_BITMAPSKIN_CENTER_FIXED")
						l.middle = Layer::Middle::Fixed;
					else if (f.text == "GFC_BITMAPSKIN_CENTER_EMPTY")
						l.middle = Layer::Middle::Empty;
					else if (f.text == "GFC_BITMAPSKIN_CENTER_FILL")
						l.middle = Layer::Middle::Fill;
					else if (f.text == "GFC_BITMAPSKIN_SIDES_NOTVISIBLE")
						l.sides = false;
					else if (f.text == "GFC_BITMAPSKIN_NOTVISIBLE")
						l.image = l.center = {};
				}
		}

		// A skin's layers: one that inherits takes the other skin's, and the one after it (with no
		// flags of its own) changes their last.
		Skin skin(const Style* s, const Value* v, int depth = 0)
		{
			Skin out;
			v = resolve(s, v);
			if (!v || v->kind != Value::Kind::Skin || depth > 8)
				return out;
			bool inherited = false;
			for (const RawLayer& raw : v->layers)
			{
				if (const auto it = raw.find("Inherit"); it != raw.end())
				{
					Skin from = skin(s, &it->second, depth + 1);
					inherited = !from.empty();
					out.insert(out.end(), from.begin(), from.end());
				}
				else if (inherited && !raw.contains("LayerFlags"))
					apply(s, raw, out.back());
				else
				{
					apply(s, raw, out.emplace_back());
					inherited = false;
				}
			}
			std::erase_if(out, [](const Layer& l) { return !l.image && !l.center && l.middle != Layer::Middle::Fill; });
			return out;
		}

		const Style* styleFor(std::string_view name)
		{
			std::string n(name);
			if (const auto it = enumStyles().find(n); it != enumStyles().end())
				n = it->second;
			if (const Style* s = findStyle(n))
				return s;
			return findStyle("SF_CtrlTheme_Civ4_Control_" + n);
		}
	};

	Theme::Theme() : m(std::make_unique<Impl>()) {}
	Theme::~Theme() = default;

	bool Theme::load(const std::vector<fs::path>& roots)
	{
		// The highest layer's Resource/Civ4.thm: beside its Assets folder (Beyond the Sword's), or in
		// it, a mod's (AND2 has Assets/Modules/Interface/Resource).
		m->roots.clear();
		for (const fs::path& r : roots)
			m->roots.push_back(r.parent_path());
		fs::path file;
		for (size_t i = 0; i < roots.size() && file.empty(); ++i)
		{
			std::error_code ec;
			if (fs::is_regular_file(m->roots[i] / "Resource/Civ4.thm", ec))
				file = m->roots[i] / "Resource/Civ4.thm";
			else
				for (fs::recursive_directory_iterator it(roots[i], fs::directory_options::skip_permission_denied, ec), end; it != end && file.empty();
					 it.increment(ec))
				{
					if (it.depth() > 3)
						it.disable_recursion_pending();
					else if (it->is_regular_file(ec) && lower(utf8Path(it->path().filename())) == "civ4.thm" &&
						lower(utf8Path(it->path().parent_path().filename())) == "resource")
						file = it->path();
				}
		}
		if (!file.empty())
		{
			m->base = file.parent_path().parent_path();
			m->resources = file.parent_path();
			m->parseFile(file, 0);
			m->loaded = !m->styles.empty();
		}
		if (!m->loaded)
			m->problem("no interface theme (Resource/Civ4.thm)");
		return m->loaded;
	}

	std::filesystem::path Theme::file() const { return m->base; }

	bool Theme::loaded() const { return m->loaded; }
	const std::vector<std::string>& Theme::problems() const { return m->problems; }

	const Theme::Panel* Theme::panel(std::string_view style)
	{
		if (const auto it = m->panels.find(style); it != m->panels.end())
			return it->second.get();
		std::unique_ptr<Panel> p;
		if (const Style* s = m->loaded ? m->styleFor(style) : nullptr)
		{
			p = std::make_unique<Panel>();
			p->inner = m->skin(s, m->property(s, "GFC_CtrlTheme_Bitmap_Control_Panel_Skin_Inner_Default"));
			if (const Value* b = m->resolve(s, m->property(s, "GFC_Control_Panel_StyleId_Border"));
				b && b->kind == Value::Kind::Call && b->text == "GPropertyId" && !b->args.empty())
				if (const Style* border = m->findStyle(b->args[0].text))
					p->border = m->skin(border, m->property(border, "GFC_CtrlTheme_Bitmap_Control_Border_Skin_Full_Default"));
		}
		return (m->panels[std::string(style)] = std::move(p)).get();
	}

	const Theme::Button* Theme::button(std::string_view style)
	{
		if (const auto it = m->buttons.find(style); it != m->buttons.end())
			return it->second.get();
		std::unique_ptr<Button> b;
		if (const Style* s = m->loaded ? m->styleFor(style) : nullptr)
		{
			b = std::make_unique<Button>();
			for (size_t i = 0; i < size_t(State::Count); ++i)
			{
				b->skin[i] = m->skin(s, m->property(s, std::string("GFC_CtrlTheme_Bitmap_Control_Button_Skin_") + kStates[i]));
				b->image[i] = m->bitmap(s, m->property(s, std::string("GFC_Control_Button_Image_") + kStates[i]));
				b->mark[i] = m->skin(s, m->property(s, std::string("GFC_CtrlTheme_Bitmap_Control_Button_Skin_Image_") + kStates[i]));
				const bool text = m->colour(s, m->property(s, std::string("GFC_Control_Button_Color_Text_") + kStates[i]), b->text[i], b->textAlpha[i]);
				const bool shadow = m->colour(s, m->property(s, std::string("GFC_Control_Button_Color_TextShadow_") + kStates[i]), b->shadow[i], b->shadowAlpha[i]);
				b->coloured = b->coloured || (text && shadow);
			}
			// (one with nothing to draw, a label button, is still the theme's: it draws its text only)
		}
		return (m->buttons[std::string(style)] = std::move(b)).get();
	}

	Theme::Skin Theme::skin(std::string_view style, std::string_view property)
	{
		if (!m->loaded)
			return {};
		const Style* s = style.empty() ? nullptr : m->styleFor(style);
		if (!style.empty() && !s)
			return {};
		return m->skin(s, s ? m->property(s, std::string(property)) : m->anywhere(std::string(property)));
	}

	const Theme::Progress* Theme::progress(std::string_view style)
	{
		const std::string key = "progress|" + std::string(style);
		if (const auto it = m->progresses.find(key); it != m->progresses.end())
			return it->second.get();
		std::unique_ptr<Progress> p;
		const Style* s = m->loaded ? m->styleFor(style) : nullptr;
		if (s && !m->property(s, "GFC_CtrlTheme_Bitmap_Control_Progress_Skin_HInner_Default"))
			s = nullptr;
		if (!s && m->loaded)
			s = m->findStyle("GFC_Control_StandardProgress_Style");
		if (s)
		{
			p = std::make_unique<Progress>();
			p->outer = m->skin(s, m->property(s, "GFC_CtrlTheme_Bitmap_Control_Progress_Skin_HTrack_Default"));
			p->track = m->skin(s, m->property(s, "GFC_CtrlTheme_Bitmap_Control_Progress_Skin_HInner_Default"));
			p->rim = m->skin(s, m->property(s, "GFC_CtrlTheme_Bitmap_Control_Progress_Skin_HBorder_Default"));
			const Skin fill = m->skin(s, m->property(s, "GFC_CtrlTheme_Bitmap_Control_Progress_Skin_HFillLeft_Default"));
			if (!fill.empty())
				p->fill = fill.back(); // (the layer over the track)
			if (p->track.empty() && !p->fill.image)
				p.reset();
		}
		return (m->progresses[key] = std::move(p)).get();
	}

	Theme::Bitmap Theme::modulate(const Bitmap& b, Color c, float alpha)
	{
		if (!b)
			return b;
		// (a few levels of each: the bars' colours do not change often)
		const auto level = [](float v) { return std::round(std::clamp(v, 0.0f, 1.0f) * 64) / 64; };
		const Color q{ level(c.r), level(c.g), level(c.b) };
		Bitmap out = b;
		if (const Picture* p = m->picture(b.picture->path, &q, level(alpha)))
			out.picture = p;
		return out;
	}

	Theme::Bitmap Theme::addSigned(const Bitmap& b, Color c)
	{
		if (!b)
			return b;
		const int r = std::clamp(int(c.r * 255), 0, 255), g = std::clamp(int(c.g * 255), 0, 255), bl = std::clamp(int(c.b * 255), 0, 255);
		const std::string key = lower(b.picture->path) + "|+" + std::to_string(r) + "," + std::to_string(g) + "," + std::to_string(bl);
		auto& slot = m->pictures[key];
		if (!slot)
			if (const Picture* plain = m->picture(b.picture->path, nullptr, 1))
			{
				slot = std::make_unique<Picture>(*plain);
				const int add[3] = { bl - 128, g - 128, r - 128 }; // (BGRA)
				for (size_t i = 0; i + 3 < slot->bgra.size(); i += 4)
					if (const int a = slot->bgra[i + 3]; a > 0)
						for (int k = 0; k < 3; ++k)
						{
							const int straight = slot->bgra[i + k] * 255 / a; // (premultiplied: undo, add, again)
							slot->bgra[i + k] = uint8_t(std::clamp(straight + add[k], 0, 255) * a / 255);
						}
				slot->key = (uint64_t(0x7E) << 56) | ++m->nextKey;
			}
		Bitmap out = b;
		if (slot)
			out.picture = slot.get();
		return out;
	}

	Theme::Font Theme::font(int size, bool bold, bool italic) const
	{
		static const float kSizes[] = { 10, 12, 14, 16, 22 }; // (the theme's, when it says nothing)
		size = std::clamp(size, 0, 4);
		Font f;
		f.size = kSizes[size];
		f.bold = bold;
		f.italic = italic;
		const char* kind = bold ? "Bold" : italic ? "Italic" : "Normal";
		for (const std::string name : { "SF_CtrlTheme_Civ4_Control_Font_Size" + std::to_string(size) + "_" + kind,
				 "SF_CtrlTheme_Civ4_Control_Font_Size" + std::to_string(size) + "_Normal" })
			if (const Value* v = m->anywhere(name); v && v->kind == Value::Kind::Call && v->text == "GFont" && v->args.size() >= 3)
			{
				if (v->args[0].kind == Value::Kind::String)
					f.family.assign(v->args[0].text.begin(), v->args[0].text.end());
				f.size = float(Impl::number(*v, 2, f.size));
				break;
			}
		return f;
	}

	void Theme::draw(Overlay& o, const Bitmap& b, float x0, float y0, float x1, float y1, float alpha)
	{
		if (b)
			o.imagePart(b.picture->key, b.picture->bgra, b.picture->width, b.picture->height, 1, x0, y0, x1, y1, b.x0, b.y0, b.x1, b.y1, alpha);
	}

	void Theme::draw(Overlay& o, const Skin& skin, float rx0, float ry0, float rx1, float ry1, float alpha)
	{
		for (const Layer& l : skin)
		{
			const float x0 = rx0 + l.indent[0], y0 = ry0 + l.indent[1], x1 = rx1 - l.indent[2], y1 = ry1 - l.indent[3];
			if (x1 <= x0 || y1 <= y0)
				continue;
			if (l.middle == Layer::Middle::Fill && l.fillAlpha > 0)
				o.rect(x0, y0, x1, y1, l.fill, l.fillAlpha * alpha);
			if (const Bitmap& b = l.image)
			{
				if (l.middle == Layer::Middle::Fixed && l.margin[0] + l.margin[1] + l.margin[2] + l.margin[3] == 0)
				{
					// At its size, in the middle.
					const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
					draw(o, b, std::round(cx - b.width() / 2), std::round(cy - b.height() / 2), std::round(cx - b.width() / 2) + b.width(),
						std::round(cy - b.height() / 2) + b.height(), alpha);
				}
				else
				{
					// Nine pieces: the corners at their size, the sides and the middle stretched.
					float ml = l.margin[0], mt = l.margin[1], mr = l.margin[2], mb = l.margin[3];
					const float w = x1 - x0, h = y1 - y0;
					if (ml + mr > w)
						ml = mr = w / 2;
					if (mt + mb > h)
						mt = mb = h / 2;
					const float sx[4] = { b.x0, b.x0 + l.margin[0], b.x1 - l.margin[2], b.x1 };
					const float sy[4] = { b.y0, b.y0 + l.margin[1], b.y1 - l.margin[3], b.y1 };
					const float dx[4] = { x0, x0 + ml, x1 - mr, x1 };
					const float dy[4] = { y0, y0 + mt, y1 - mb, y1 };
					for (int j = 0; j < 3; ++j)
						for (int i = 0; i < 3; ++i)
						{
							const bool middle = i == 1 && j == 1, side = (i == 1) != (j == 1);
							if ((middle && l.middle == Layer::Middle::Empty) || (side && !l.sides) || (!middle && !side && !l.sides))
								continue;
							if ((i == 0 && !l.section[0]) || (j == 0 && !l.section[1]) || (i == 2 && !l.section[2]) || (j == 2 && !l.section[3]))
								continue;
							if (dx[i + 1] > dx[i] && dy[j + 1] > dy[j] && sx[i + 1] > sx[i] && sy[j + 1] > sy[j])
								o.imagePart(b.picture->key, b.picture->bgra, b.picture->width, b.picture->height, 1, dx[i], dy[j], dx[i + 1], dy[j + 1], sx[i],
									sy[j], sx[i + 1], sy[j + 1], alpha);
						}
				}
			}
			if (const Bitmap& c = l.center)
			{
				const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
				draw(o, c, std::round(cx - c.width() / 2), std::round(cy - c.height() / 2), std::round(cx - c.width() / 2) + c.width(),
					std::round(cy - c.height() / 2) + c.height(), alpha);
			}
		}
	}
}
