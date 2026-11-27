#include "render/Font.h"

#include <dwrite_2.h>
#include <wrl/client.h>

#include <map>
#include <stdexcept>
#include <tuple>
#include <unordered_map>

namespace oc4::render
{
	using Microsoft::WRL::ComPtr;

	namespace
	{
		// Asked in turn for a character the face lacks (the stars and arrows of the game's text, other scripts).
		constexpr const wchar_t* kFallbacks[] = { L"Segoe UI", L"Segoe UI Symbol", L"Arial", L"Microsoft YaHei", L"Malgun Gothic", L"Meiryo",
			L"Nirmala UI", L"Segoe UI Emoji" };
	}

	struct Fonts::Impl
	{
		ComPtr<IDWriteFactory2> factory;
		ComPtr<IDWriteFontCollection> system;
		ComPtr<IDWriteRenderingParams> params;
		struct Face
		{
			ComPtr<IDWriteFont> font;
			ComPtr<IDWriteFontFace> face;
			Metrics metrics;
			float unitsPerEm = 1;
			bool bold = false, italic = false;
			std::unordered_map<char32_t, Glyph> glyphs;
			std::vector<int> fallbacks; // (made when first needed)
			bool fallbacksMade = false;
			ComPtr<IDWriteFontFace1> kerned; // (null: no kerning pairs)
			std::unordered_map<uint32_t, float> pairs;
		};
		std::vector<Face> faces;
		std::map<std::tuple<std::wstring, bool, bool>, int> byName;

		int make(const std::wstring& family, bool bold, bool italic, bool orDefault)
		{
			const auto key = std::tuple{ family, bold, italic };
			if (const auto it = byName.find(key); it != byName.end())
				return it->second;
			UINT32 index = 0;
			BOOL exists = FALSE;
			ComPtr<IDWriteFontFamily> f;
			ComPtr<IDWriteFont> font;
			if (FAILED(system->FindFamilyName(family.c_str(), &index, &exists)) || !exists || FAILED(system->GetFontFamily(index, &f)) ||
				FAILED(f->GetFirstMatchingFont(bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
					italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, &font)))
			{
				if (!orDefault)
					return -1;
				for (const wchar_t* other : { L"Segoe UI", L"Arial" })
					if (other != family)
						if (const int i = make(other, bold, italic, false); i >= 0)
							return byName[key] = i;
				throw std::runtime_error("no font");
			}
			Face face;
			face.font = font;
			if (FAILED(font->CreateFontFace(&face.face)))
				return -1;
			DWRITE_FONT_METRICS fm{};
			face.face->GetMetrics(&fm);
			face.unitsPerEm = float(fm.designUnitsPerEm);
			const float u = face.unitsPerEm;
			face.metrics = { float(fm.ascent) / u, float(fm.descent) / u, float(fm.lineGap) / u, float(-fm.underlinePosition) / u,
				float(fm.underlineThickness) / u };
			{
				const void* table = nullptr;
				UINT32 length = 0;
				void* context = nullptr;
				BOOL has = FALSE;
				if (SUCCEEDED(face.face->TryGetFontTable(DWRITE_MAKE_OPENTYPE_TAG('V', 'D', 'M', 'X'), &table, &length, &context, &has)) && has)
				{
					face.metrics.pixelLines = readVdmx(static_cast<const uint8_t*>(table), length);
					face.face->ReleaseFontTable(context);
				}
			}
			face.bold = bold, face.italic = italic;
			if (SUCCEEDED(face.face.As(&face.kerned)) && !face.kerned->HasKerningPairs())
				face.kerned.Reset();
			faces.push_back(std::move(face));
			return byName[key] = int(faces.size() - 1);
		}

		bool find(int face, char32_t c, Glyph& g)
		{
			Face& f = faces[size_t(face)];
			UINT32 cp = c;
			UINT16 index = 0;
			if (FAILED(f.face->GetGlyphIndices(&cp, 1, &index)) || index == 0)
				return false;
			DWRITE_GLYPH_METRICS gm{};
			f.face->GetDesignGlyphMetrics(&index, 1, &gm, FALSE);
			g = { face, index, float(gm.advanceWidth) / f.unitsPerEm };
			return true;
		}
	};

	Fonts::Fonts() : m(std::make_unique<Impl>())
	{
		if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2), reinterpret_cast<IUnknown**>(m->factory.GetAddressOf()))) ||
			FAILED(m->factory->GetSystemFontCollection(&m->system)))
			throw std::runtime_error("DirectWrite is unavailable");
		m->factory->CreateRenderingParams(&m->params);
	}

	Fonts::~Fonts() = default;

	// (Windows has the game's fonts installed: Sylfaen ships with it)
	void Fonts::addFolders(const std::vector<std::filesystem::path>&) {}

	int Fonts::face(const std::wstring& family, bool bold, bool italic) { return m->make(family, bold, italic, true); }

	const Fonts::Metrics& Fonts::metrics(int face) const { return m->faces[size_t(face)].metrics; }

	Fonts::Glyph Fonts::glyph(int face, char32_t c)
	{
		if (const auto it = m->faces[size_t(face)].glyphs.find(c); it != m->faces[size_t(face)].glyphs.end())
			return it->second;
		Glyph g;
		if (!m->find(face, c, g))
		{
			if (!m->faces[size_t(face)].fallbacksMade)
			{
				const bool bold = m->faces[size_t(face)].bold, italic = m->faces[size_t(face)].italic;
				std::vector<int> fallbacks;
				for (const wchar_t* family : kFallbacks)
					if (const int f = m->make(family, bold, italic, false); f >= 0 && f != face)
						fallbacks.push_back(f);
				m->faces[size_t(face)].fallbacks = std::move(fallbacks);
				m->faces[size_t(face)].fallbacksMade = true;
			}
			bool found = false;
			for (const int f : m->faces[size_t(face)].fallbacks)
				if ((found = m->find(f, c, g)))
					break;
			if (!found)
			{
				Impl::Face& f = m->faces[size_t(face)];
				UINT16 missing = 0;
				DWRITE_GLYPH_METRICS gm{};
				f.face->GetDesignGlyphMetrics(&missing, 1, &gm, FALSE);
				g = { face, 0, float(gm.advanceWidth) / f.unitsPerEm };
			}
		}
		return m->faces[size_t(face)].glyphs[c] = g;
	}

	float Fonts::kerning(int face, uint16_t left, uint16_t right)
	{
		Impl::Face& f = m->faces[size_t(face)];
		if (!f.kerned)
			return 0;
		const uint32_t key = uint32_t(left) << 16 | right;
		if (const auto it = f.pairs.find(key); it != f.pairs.end())
			return it->second;
		const UINT16 glyphs[2]{ left, right };
		INT32 adjustments[2]{};
		f.kerned->GetKerningPairAdjustments(2, glyphs, adjustments);
		return f.pairs[key] = float(adjustments[0]) / f.unitsPerEm;
	}

	Fonts::Bitmap Fonts::rasterize(int face, uint16_t glyph, float size, float subpixelX)
	{
		Impl::Face& f = m->faces[size_t(face)];
		const UINT16 index = glyph;
		const FLOAT advance = 0;
		const DWRITE_GLYPH_RUN run{ f.face.Get(), size, 1, &index, &advance, nullptr, FALSE, 0 };
		DWRITE_RENDERING_MODE mode = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
		f.face->GetRecommendedRenderingMode(size, 1.0f, DWRITE_MEASURING_MODE_NATURAL, m->params.Get(), &mode);
		if (mode == DWRITE_RENDERING_MODE_OUTLINE || mode == DWRITE_RENDERING_MODE_DEFAULT)
			mode = DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC;
		ComPtr<IDWriteGlyphRunAnalysis> analysis;
		Bitmap b;
		RECT r{};
		if (FAILED(m->factory->CreateGlyphRunAnalysis(&run, nullptr, mode, DWRITE_MEASURING_MODE_NATURAL, DWRITE_GRID_FIT_MODE_DEFAULT,
				DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE, subpixelX, 0, &analysis)) ||
			FAILED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &r)) || r.right <= r.left || r.bottom <= r.top)
			return b;
		b.left = r.left, b.top = r.top, b.width = r.right - r.left, b.height = r.bottom - r.top;
		b.alpha.resize(size_t(b.width) * size_t(b.height));
		if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &r, b.alpha.data(), UINT32(b.alpha.size()))))
			return {};
		return b;
	}
}
