#pragma once

// Text, pictures and simple shapes over a rendered frame: triangles with soft edges, the glyphs
// from an atlas, drawn by the renderer when the overlay ends. Everything here is in the
// interface's units, setScale of the frame's pixels each (see UiScale.h); text is rasterized at
// the pixels' size and stays sharp.

#include "render/ButtonArt.h"
#include "render/Font.h"
#include "render/MapSnapshot.h"
#include "render/Renderer.h"
#include "render/TextLayout.h"

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include <string_view>
#include <tuple>

namespace oc4::render
{
	class Overlay
	{
	public:
		enum class Align : uint8_t
		{
			Left, Center, Right
		};

		explicit Overlay(Renderer& renderer) : m_renderer(renderer) {}

		// Between Renderer::endFrame and present (or readback).
		bool begin();
		void end();

		void rect(float x0, float y0, float x1, float y1, Color color, float alpha, float radius = 0);
		void frame(float x0, float y0, float x1, float y1, Color color, float alpha, float radius = 0, float width = 1);
		void circle(float x, float y, float radius, Color color, float alpha);
		void ellipse(float x, float y, float rx, float ry, Color color, float alpha, float width = 1);
		void line(float x0, float y0, float x1, float y1, Color color, float alpha, float width = 1);
		// A filled four-cornered shape (its corners in order).
		void quad(const float x[4], const float y[4], Color color, float alpha);
		// BGRA8 pixels (premultiplied alpha), stretched over the rectangle; `version` tells
		// when they changed (the bitmap is kept until then). Each picture has its own `key`.
		void image(uint64_t key, const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint64_t version, float x0, float y0, float x1,
			float y1, float alpha = 1);
		void image(const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint64_t version, float x0, float y0, float x1, float y1,
			float alpha = 1)
		{
			image(0, bgra, width, height, version, x0, y0, x1, y1, alpha);
		}
		// A picture the renderer drew (Renderer::targetImage), stretched over the rectangle.
		void bitmap(const GpuImage* image, float x0, float y0, float x1, float y1, float alpha = 1);
		// Part of the picture (sx0..sy1 in its pixels) over the rectangle.
		void imagePart(uint64_t key, const std::vector<uint8_t>& bgra, uint32_t width, uint32_t height, uint64_t version, float x0, float y0, float x1,
			float y1, float sx0, float sy0, float sx1, float sy1, float alpha = 1);
		// y is the top of the text; returns its width.
		float text(std::wstring_view text, float x, float y, float size, Color color, float alpha = 1, bool bold = false, Align align = Align::Left,
			bool shadow = true);
		float width(std::wstring_view text, float size, bool bold = false);
		// Wrapped at maxWidth, its top left at x, y; returns its height.
		float textBlock(std::wstring_view text, float x, float y, float maxWidth, float size, Color color, float alpha = 1, bool bold = false);
		float blockHeight(std::wstring_view text, float maxWidth, float size, bool bold = false);
		// Text with the game screens' markup: <font=2>, <font=4b> (size 0..4, bold, italic) and
		// </font>, <color=r,g,b,a> and </color>, <b>, <i>, <u>, <link=...> (a link's colour); other
		// text in angle brackets stays. size and bold are the text's own outside any <font>.
		float richText(std::wstring_view markup, float x, float y, float size, bool bold, Color color, float alpha = 1, Align align = Align::Left,
			bool shadow = true);
		float richWidth(std::wstring_view markup, float size, bool bold);
		// Wrapped at maxWidth; returns its height.
		float richBlock(std::wstring_view markup, float x, float y, float maxWidth, float size, bool bold, Color color, float alpha = 1);
		float richHeight(std::wstring_view markup, float maxWidth, float size, bool bold);
		// The pictures inside marked up text (<img=art size=16>, the game font's icons as
		// <img=font:code>): drawn from the art, none without it.
		void setArt(ButtonArt* art) { m_art = art; }
		// (an inline picture drawn: by its art, the top left and size of its place)
		void drawInline(const std::string& art, float x, float y, float width, float height);

		// The colour of the text's shadow (one pixel down and right) until changed again; black by default.
		void setShadow(Color colour, float alpha) { m_shadow = colour; m_shadowAlpha = alpha; }
		void resetShadow() { setShadow({ 0, 0, 0 }, 0.8f); }
		// The face for all text, and the pixel sizes of the markup's <font=0..4> (the theme's).
		void setFonts(std::wstring family, const std::array<float, 5>& sizes);
		// Folders whose fonts count as installed (the game's own, Wine's): Fonts::addFolders.
		void addFontFolders(const std::vector<std::filesystem::path>& folders);

		// Drawing only inside the rectangle until unclip.
		void clip(float x0, float y0, float x1, float y1);
		void unclip();

		// In the interface's units, as all the rest.
		float screenWidth() const { return float(m_renderer.width()) / m_scale; }
		float screenHeight() const { return float(m_renderer.height()) / m_scale; }
		// Pixels to a unit.
		void setScale(float scale) { m_scale = scale; }
		float scale() const { return m_scale; }

	private:
		// Marked up or plain text laid out, kept while it is drawn (the same names and numbers come every frame).
		struct Layout
		{
			LaidText laid;
			std::vector<TextInline> inlines;
			std::vector<std::string> art; // (the pictures' in it)
			uint64_t used = 0;
		};
		const Layout* layout(std::wstring_view text, float size, bool bold, float maxWidth, bool markup);
		void drawLayout(const Layout& l, float x, float y, Color color, float alpha, bool shadow);
		void drawGlyphs(const Layout& l, float x, float y, Color color, float alpha, bool ownColours);

		void use(const GpuImage* image);
		void vertex(float x, float y, uint32_t color, float u = 0, float v = 0);
		void pixelVertex(float x, float y, uint32_t color, float u, float v); // (x, y in pixels)
		void fillConvex(const std::vector<std::array<float, 2>>& points, uint32_t color);
		void strokeClosed(const std::vector<std::array<float, 2>>& points, float width, uint32_t color);
		void flush();
		uint32_t premultiplied(Color c, float alpha) const;

		struct Bitmap
		{
			std::shared_ptr<GpuImage> image;
			uint64_t version = ~0ull, used = 0, batch = ~0ull;
			uint32_t width = 0, height = 0;
		};
		// A glyph's place in the atlas and its bitmap's offset from the pen.
		struct GlyphSlot
		{
			int left = 0, top = 0, width = 0, height = 0, x = 0, y = 0;
		};
		const GlyphSlot* glyph(int face, uint16_t index, float size, int quarter);

		Renderer& m_renderer;
		Fonts m_fonts;
		bool m_drawing = false;
		float m_scale = 1;
		std::vector<OverlayVertex> m_vertices;
		std::vector<OverlayDraw> m_draws;
		std::vector<std::array<int32_t, 4>> m_clips;
		uint64_t m_batch = 0; // (the draws not yet given to the renderer)
		std::wstring m_family = L"Segoe UI";
		Color m_shadow{ 0, 0, 0 };
		float m_shadowAlpha = 0.8f;
		std::array<float, 5> m_sizes{ 10, 12, 14, 16, 22 };
		std::map<std::tuple<std::wstring, int, bool, int, bool>, Layout> m_layouts;
		uint64_t m_frame = 0;
		std::map<uint64_t, Bitmap> m_bitmaps;
		std::shared_ptr<GpuImage> m_atlas;
		std::map<std::tuple<int, uint16_t, int, int>, GlyphSlot> m_glyphs;
		int m_shelfX = 0, m_shelfY = 0, m_shelfHeight = 0;
		ButtonArt* m_art = nullptr;
		float m_inlineAlpha = 1; // (the text's being drawn)
	};
}
