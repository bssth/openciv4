#include "render/Overlay.h"

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <numbers>

namespace oc4::render
{
	namespace
	{
		constexpr int kAtlasSize = 2048;
		// (D2D's grayscale text contrast: the coverage of thin stems raised)
		constexpr float kTextContrast = 1.0f;

		int32_t pixel(float v) { return int32_t(std::floor(v + 0.5f)); }

		uint32_t pack(float r, float g, float b, float a)
		{
			const auto byte = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255 + 0.5f); };
			return byte(r) | byte(g) << 8 | byte(b) << 16 | byte(a) << 24;
		}

		// The points of a rounded rectangle, clockwise from the top left corner's start.
		std::vector<std::array<float, 2>> roundedRect(float x0, float y0, float x1, float y1, float radius)
		{
			radius = std::min({ radius, (x1 - x0) / 2, (y1 - y0) / 2 });
			if (radius <= 0)
				return { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
			const int steps = std::clamp(int(radius / 1.5f) + 2, 2, 12);
			const float cx[4]{ x0 + radius, x1 - radius, x1 - radius, x0 + radius }, cy[4]{ y0 + radius, y0 + radius, y1 - radius, y1 - radius };
			std::vector<std::array<float, 2>> p;
			for (int c = 0; c < 4; ++c)
				for (int k = 0; k <= steps; ++k)
				{
					const float a = std::numbers::pi_v<float> * (1 + 0.5f * float(c) + 0.5f * float(k) / float(steps));
					p.push_back({ cx[c] + radius * std::cos(a), cy[c] + radius * std::sin(a) });
				}
			return p;
		}

		std::vector<std::array<float, 2>> ellipsePoints(float x, float y, float rx, float ry)
		{
			const int steps = std::clamp(int(std::max(rx, ry) * 1.5f), 12, 96);
			std::vector<std::array<float, 2>> p;
			for (int k = 0; k < steps; ++k)
			{
				const float a = 2 * std::numbers::pi_v<float> * float(k) / float(steps);
				p.push_back({ x + rx * std::cos(a), y + ry * std::sin(a) });
			}
			return p;
		}

		uint32_t packColour(int r, int g, int b, int a)
		{
			return uint32_t(std::clamp(r, 0, 255)) << 24 | uint32_t(std::clamp(g, 0, 255)) << 16 | uint32_t(std::clamp(b, 0, 255)) << 8 |
				uint32_t(std::clamp(a, 0, 255));
		}

		// The characters of marked up text and the runs of their styles.
		std::wstring parseMarkup(std::wstring_view s, float size, bool bold, const std::array<float, 5>& sizes, std::vector<TextSpan>& spans,
			std::vector<TextInline>& inlines, std::vector<std::string>& art)
		{
			struct Look
			{
				float size;
				bool bold, italic, underline;
			};
			std::vector<Look> looks{ { size, bold, false, false } };
			std::vector<uint32_t> colours{ 0 }; // 0: the text's own
			std::wstring out;
			const auto push = [&](auto change) {
				Look l = looks.back();
				change(l);
				looks.push_back(l);
			};
			const auto pop = [&](auto& stack) {
				if (stack.size() > 1)
					stack.pop_back();
			};
			for (size_t i = 0; i < s.size(); ++i)
			{
				if (s[i] == L'<')
					if (const size_t close = s.find(L'>', i + 1); close != std::wstring_view::npos)
					{
						// (in any case: some screens upper-case their text with its markup, "<COLOR=...>")
						std::wstring lowered(s.substr(i + 1, close - i - 1));
						for (wchar_t& c : lowered)
							c = wchar_t(std::towlower(c));
						const std::wstring_view tag = lowered;
						bool known = true;
						if (tag.starts_with(L"font=") && tag.size() > 5 && tag[5] >= L'0' && tag[5] <= L'4')
						{
							const float px = sizes[size_t(tag[5] - L'0')];
							const bool b = tag.find(L'b', 6) != std::wstring_view::npos, it = tag.find(L'i', 6) != std::wstring_view::npos;
							push([&](Look& l) {
								l.size = px;
								l.bold = b;
								l.italic = it;
							});
						}
						// (a bare <font> closes one too: the replay screen's title ends so)
						else if (tag == L"/font" || tag == L"font" || tag == L"/b" || tag == L"/i" || tag == L"/u")
							pop(looks);
						else if (tag == L"b")
							push([](Look& l) { l.bold = true; });
						else if (tag == L"i")
							push([](Look& l) { l.italic = true; });
						else if (tag == L"u")
							push([](Look& l) { l.underline = true; });
						else if (tag.starts_with(L"color="))
						{
							int c[4]{ 255, 255, 255, 255 }, n = 0;
							for (size_t k = 6; k < tag.size() && n < 4;)
							{
								int v = 0;
								bool digits = false;
								for (; k < tag.size() && std::iswdigit(tag[k]); ++k, digits = true)
									v = v * 10 + (tag[k] - L'0');
								if (digits)
									c[n++] = v;
								while (k < tag.size() && !std::iswdigit(tag[k]))
									++k;
							}
							colours.push_back(packColour(c[0], c[1], c[2], c[3]));
						}
						else if (tag.starts_with(L"img=") && tag.size() > 4)
						{
							// "img=Art/Interface/Buttons/x.dds size=16" ("img=font:8483": the game
							// font's icon, a little taller than the text)
							const size_t end = std::min(tag.find(L' ', 4), tag.size());
							std::string name;
							for (const wchar_t c : tag.substr(4, end - 4))
								name += c < 128 ? char(c) : '?';
							const Look& l = looks.back();
							float h = l.size * 1.25f, w = h;
							if (const size_t at = tag.find(L"size="); at != std::wstring_view::npos)
								w = h = std::max(float(std::wcstol(lowered.c_str() + at + 5, nullptr, 10)) * l.size / 12, 4.0f);
							else if (name.starts_with("font:"))
								w = h * 21 / 20; // (the font's cells)
							spans.push_back({ out.size(), 1, TextStyle{ l.size, l.bold, l.italic, l.underline, 0 } });
							inlines.push_back({ out.size(), w, h });
							art.push_back(std::move(name));
							out += wchar_t(0xFFFC);
							i = close;
							continue;
						}
						else if (tag == L"/img")
							; // (an inline picture ends)
						else if (tag == L"/color" || tag == L"/link")
							pop(colours);
						else if (tag.starts_with(L"link="))
							colours.push_back(packColour(175, 210, 245, 255)); // (the theme's link colour)
						else if (!tag.starts_with(L"shadow") && !tag.starts_with(L"/shadow") && !tag.starts_with(L"outline") &&
							!tag.starts_with(L"/outline"))
							known = false;
						if (known)
						{
							i = close;
							continue;
						}
					}
				const Look& l = looks.back();
				const uint32_t c = colours.back();
				if (const TextStyle* last = spans.empty() ? nullptr : &spans.back().style;
					!last || last->size != l.size || last->bold != l.bold || last->italic != l.italic || last->underline != l.underline || last->rgba != c)
					spans.push_back({ out.size(), 0, TextStyle{ l.size, l.bold, l.italic, l.underline, c } });
				// (the markup's escapes: "&lt;F&gt;" is "<F>")
				wchar_t ch = s[i];
				if (ch == L'&')
					for (const auto& [name, value] : { std::pair{ L"&lt;", L'<' }, { L"&gt;", L'>' }, { L"&amp;", L'&' }, { L"&quot;", L'"' }, { L"&apos;", L'\'' } })
						if (s.substr(i).starts_with(name))
						{
							ch = value;
							i += std::wcslen(name) - 1;
							break;
						}
				out += ch;
				++spans.back().length;
			}
			return out;
		}
	}

	bool Overlay::begin()
	{
		m_drawing = true;
		return true;
	}

	void Overlay::end()
	{
		flush();
		m_drawing = false;
		m_clips.clear();
		// Text and pictures not drawn for a while go.
		if (++m_frame % 120 == 0)
		{
			std::erase_if(m_layouts, [&](const auto& e) { return m_frame - e.second.used > 240; });
			std::erase_if(m_bitmaps, [&](const auto& e) { return e.first != 0 && m_frame - e.second.used > 600; });
		}
	}

	void Overlay::flush()
	{
		if (!m_draws.empty())
			m_renderer.drawOverlay(m_vertices, m_draws);
		m_vertices.clear();
		m_draws.clear();
		++m_batch;
	}

	void Overlay::use(const GpuImage* image)
	{
		const std::array<int32_t, 4> clip = m_clips.empty() ? std::array<int32_t, 4>{ 0, 0, int32_t(m_renderer.width()), int32_t(m_renderer.height()) }
															: m_clips.back();
		if (!m_draws.empty())
		{
			const OverlayDraw& d = m_draws.back();
			if (d.image == image && std::equal(clip.begin(), clip.end(), d.clip))
				return;
		}
		OverlayDraw d;
		d.image = image;
		d.first = uint32_t(m_vertices.size());
		std::copy(clip.begin(), clip.end(), d.clip);
		m_draws.push_back(d);
	}

	void Overlay::vertex(float x, float y, uint32_t color, float u, float v) { pixelVertex(x * m_scale, y * m_scale, color, u, v); }

	void Overlay::pixelVertex(float x, float y, uint32_t color, float u, float v)
	{
		m_vertices.push_back({ x, y, u, v, color });
		++m_draws.back().count;
	}

	uint32_t Overlay::premultiplied(Color c, float alpha) const { return pack(c.r * alpha, c.g * alpha, c.b * alpha, alpha); }

	namespace
	{
		uint32_t scaled(uint32_t color, float k)
		{
			uint32_t out = 0;
			for (int shift = 0; shift < 32; shift += 8)
				out |= uint32_t(float((color >> shift) & 255) * k + 0.5f) << shift;
			return out;
		}

		// Each point's way out of the shape (by a unit across its edges): the edges' normals mitred.
		std::vector<std::array<float, 2>> outward(const std::vector<std::array<float, 2>>& p)
		{
			const size_t n = p.size();
			float area = 0;
			for (size_t i = 0; i < n; ++i)
				area += p[i][0] * p[(i + 1) % n][1] - p[(i + 1) % n][0] * p[i][1];
			const float side = area >= 0 ? 1.0f : -1.0f;
			std::vector<std::array<float, 2>> edge(n), out(n);
			for (size_t i = 0; i < n; ++i)
			{
				const float dx = p[(i + 1) % n][0] - p[i][0], dy = p[(i + 1) % n][1] - p[i][1], len = std::hypot(dx, dy);
				edge[i] = len > 1e-6f ? std::array<float, 2>{ side * dy / len, -side * dx / len } : std::array<float, 2>{ 0, 0 };
			}
			for (size_t i = 0; i < n; ++i)
			{
				const auto& a = edge[(i + n - 1) % n];
				const auto& b = edge[i];
				float mx = a[0] + b[0], my = a[1] + b[1];
				const float len = std::hypot(mx, my);
				if (len < 1e-4f)
					mx = b[0], my = b[1];
				else
					mx /= len, my /= len;
				const float c = std::max(0.25f, mx * b[0] + my * b[1]);
				out[i] = { mx / c, my / c };
			}
			return out;
		}
	}

	void Overlay::fillConvex(const std::vector<std::array<float, 2>>& p, uint32_t color)
	{
		const size_t n = p.size();
		if (n < 3)
			return;
		use(nullptr);
		const auto o = outward(p);
		const float soft = 0.5f / m_scale; // (the soft edge: a pixel wide at any scale)
		const auto in = [&](size_t i) { return std::array<float, 2>{ p[i][0] - o[i][0] * soft, p[i][1] - o[i][1] * soft }; };
		const auto out = [&](size_t i) { return std::array<float, 2>{ p[i][0] + o[i][0] * soft, p[i][1] + o[i][1] * soft }; };
		for (size_t i = 1; i + 1 < n; ++i)
		{
			vertex(in(0)[0], in(0)[1], color);
			vertex(in(i)[0], in(i)[1], color);
			vertex(in(i + 1)[0], in(i + 1)[1], color);
		}
		for (size_t i = 0; i < n; ++i)
		{
			const size_t j = (i + 1) % n;
			vertex(in(i)[0], in(i)[1], color);
			vertex(in(j)[0], in(j)[1], color);
			vertex(out(j)[0], out(j)[1], 0);
			vertex(in(i)[0], in(i)[1], color);
			vertex(out(j)[0], out(j)[1], 0);
			vertex(out(i)[0], out(i)[1], 0);
		}
	}

	void Overlay::strokeClosed(const std::vector<std::array<float, 2>>& p, float width, uint32_t color)
	{
		const size_t n = p.size();
		if (n < 2 || width <= 0)
			return;
		use(nullptr);
		const auto o = outward(p);
		const float pixels = width * m_scale, soft = 0.5f / m_scale;
		const uint32_t peak = pixels < 1 ? scaled(color, pixels) : color;
		const float edge = width / 2 + soft, solid = std::max(width / 2 - soft, 0.0f);
		const auto strip = [&](float t0, uint32_t c0, float t1, uint32_t c1) {
			for (size_t i = 0; i < n; ++i)
			{
				const size_t j = (i + 1) % n;
				const float ax = p[i][0] + o[i][0] * t0, ay = p[i][1] + o[i][1] * t0, bx = p[j][0] + o[j][0] * t0, by = p[j][1] + o[j][1] * t0;
				const float cx = p[j][0] + o[j][0] * t1, cy = p[j][1] + o[j][1] * t1, dx = p[i][0] + o[i][0] * t1, dy = p[i][1] + o[i][1] * t1;
				vertex(ax, ay, c0), vertex(bx, by, c0), vertex(cx, cy, c1);
				vertex(ax, ay, c0), vertex(cx, cy, c1), vertex(dx, dy, c1);
			}
		};
		strip(edge, 0, solid, peak);
		if (solid > 0)
			strip(solid, peak, -solid, peak);
		strip(-solid, peak, -edge, 0);
	}

	void Overlay::setFonts(std::wstring family, const std::array<float, 5>& sizes)
	{
		if (family == m_family && sizes == m_sizes)
			return;
		m_family = std::move(family);
		m_sizes = sizes;
		m_layouts.clear();
	}

	void Overlay::rect(float x0, float y0, float x1, float y1, Color color, float alpha, float radius)
	{
		if (m_drawing && x1 > x0 && y1 > y0)
			fillConvex(roundedRect(x0, y0, x1, y1, radius), premultiplied(color, alpha));
	}

	void Overlay::frame(float x0, float y0, float x1, float y1, Color color, float alpha, float radius, float lineWidth)
	{
		if (m_drawing && x1 >= x0 && y1 >= y0)
			strokeClosed(roundedRect(x0, y0, x1, y1, radius), lineWidth, premultiplied(color, alpha));
	}

	void Overlay::circle(float x, float y, float radius, Color color, float alpha)
	{
		if (m_drawing && radius > 0)
			fillConvex(ellipsePoints(x, y, radius, radius), premultiplied(color, alpha));
	}

	void Overlay::ellipse(float x, float y, float rx, float ry, Color color, float alpha, float lineWidth)
	{
		if (m_drawing && rx > 0 && ry > 0)
			strokeClosed(ellipsePoints(x, y, rx, ry), lineWidth, premultiplied(color, alpha));
	}

	void Overlay::line(float x0, float y0, float x1, float y1, Color color, float alpha, float lineWidth)
	{
		const float len = std::hypot(x1 - x0, y1 - y0);
		if (!m_drawing || len < 1e-4f || lineWidth <= 0)
			return;
		use(nullptr);
		const float nx = -(y1 - y0) / len, ny = (x1 - x0) / len;
		const float pixels = lineWidth * m_scale, soft = 0.5f / m_scale;
		const uint32_t c = premultiplied(color, alpha), peak = pixels < 1 ? scaled(c, pixels) : c;
		const float edge = lineWidth / 2 + soft, solid = std::max(lineWidth / 2 - soft, 0.0f);
		const auto strip = [&](float t0, uint32_t c0, float t1, uint32_t c1) {
			vertex(x0 + nx * t0, y0 + ny * t0, c0), vertex(x1 + nx * t0, y1 + ny * t0, c0), vertex(x1 + nx * t1, y1 + ny * t1, c1);
			vertex(x0 + nx * t0, y0 + ny * t0, c0), vertex(x1 + nx * t1, y1 + ny * t1, c1), vertex(x0 + nx * t1, y0 + ny * t1, c1);
		};
		strip(edge, 0, solid, peak);
		if (solid > 0)
			strip(solid, peak, -solid, peak);
		strip(-solid, peak, -edge, 0);
	}

	void Overlay::quad(const float x[4], const float y[4], Color color, float alpha)
	{
		if (m_drawing)
			fillConvex({ { x[0], y[0] }, { x[1], y[1] }, { x[2], y[2] }, { x[3], y[3] } }, premultiplied(color, alpha));
	}

	void Overlay::image(uint64_t key, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h, uint64_t version, float x0, float y0, float x1, float y1,
		float alpha)
	{
		imagePart(key, bgra, w, h, version, x0, y0, x1, y1, 0, 0, float(w), float(h), alpha);
	}

	const GpuImage* Overlay::upload(uint64_t key, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h, uint64_t version)
	{
		Bitmap& b = m_bitmaps[key];
		// (a picture the draws not yet made show is changed only after them)
		if (!b.image || b.width != w || b.height != h)
		{
			if (b.batch == m_batch)
				flush();
			b.image = m_renderer.createImage(w, h, false, bgra.data());
			b.width = w;
			b.height = h;
			b.version = version;
		}
		else if (b.version != version)
		{
			if (b.batch == m_batch)
				flush();
			m_renderer.updateImage(*b.image, 0, 0, w, h, bgra.data());
			b.version = version;
		}
		if (!b.image)
			return nullptr;
		b.used = m_frame;
		b.batch = m_batch;
		return b.image.get();
	}

	void Overlay::imagePart(uint64_t key, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h, uint64_t version, float x0, float y0, float x1,
		float y1, float sx0, float sy0, float sx1, float sy1, float alpha)
	{
		if (!m_drawing || w == 0 || h == 0 || bgra.size() < size_t(w) * h * 4 || x1 <= x0 || y1 <= y0 || sx1 <= sx0 || sy1 <= sy0)
			return;
		const GpuImage* image = upload(key, bgra, w, h, version);
		if (!image)
			return;
		use(image);
		const float u0 = sx0 / float(w), v0 = sy0 / float(h), u1 = sx1 / float(w), v1 = sy1 / float(h);
		const uint32_t c = pack(alpha, alpha, alpha, alpha);
		vertex(x0, y0, c, u0, v0), vertex(x1, y0, c, u1, v0), vertex(x1, y1, c, u1, v1);
		vertex(x0, y0, c, u0, v0), vertex(x1, y1, c, u1, v1), vertex(x0, y1, c, u0, v1);
	}

	void Overlay::imageMesh(uint64_t key, const std::vector<uint8_t>& bgra, uint32_t w, uint32_t h, const std::vector<std::array<float, 2>>& points,
		int n, float sx0, float sy0, float sx1, float sy1, Color tint, float alpha)
	{
		if (!m_drawing || w == 0 || h == 0 || bgra.size() < size_t(w) * h * 4 || n < 1 || points.size() < size_t(n + 1) * size_t(n + 1))
			return;
		const GpuImage* image = upload(key, bgra, w, h, 0);
		if (!image)
			return;
		use(image);
		const uint32_t c = premultiplied(tint, alpha);
		const auto at = [&](int i, int j) {
			const auto& p = points[size_t(j) * size_t(n + 1) + size_t(i)];
			const float t = float(i) / float(n), u = float(j) / float(n);
			vertex(p[0], p[1], c, (sx0 + (sx1 - sx0) * t) / float(w), (sy0 + (sy1 - sy0) * u) / float(h));
		};
		for (int j = 0; j < n; ++j)
			for (int i = 0; i < n; ++i)
			{
				at(i, j), at(i + 1, j), at(i + 1, j + 1);
				at(i, j), at(i + 1, j + 1), at(i, j + 1);
			}
	}

	void Overlay::bitmap(const GpuImage* image, float x0, float y0, float x1, float y1, float alpha)
	{
		if (!m_drawing || !image || x1 <= x0 || y1 <= y0)
			return;
		use(image);
		const uint32_t c = pack(alpha, alpha, alpha, alpha);
		vertex(x0, y0, c, 0, 0), vertex(x1, y0, c, 1, 0), vertex(x1, y1, c, 1, 1);
		vertex(x0, y0, c, 0, 0), vertex(x1, y1, c, 1, 1), vertex(x0, y1, c, 0, 1);
	}

	void Overlay::drawInline(const std::string& art, float x, float y, float width, float height)
	{
		if (!m_art || !m_drawing)
			return;
		if (const ButtonArt::Icon* ic = m_art->icon(art))
		{
			// (kept in its proportions inside its place)
			float w = width, h = height;
			if (ic->width * height > ic->height * width)
				h = width * float(ic->height) / float(ic->width);
			else
				w = height * float(ic->width) / float(ic->height);
			const float x0 = x + (width - w) / 2, y0 = y + (height - h) / 2;
			image(ic->key, ic->bgra, ic->width, ic->height, 0, x0, y0, x0 + w, y0 + h, m_inlineAlpha);
		}
	}

	void Overlay::clip(float x0, float y0, float x1, float y1)
	{
		if (!m_drawing)
			return;
		std::array<int32_t, 4> c{ pixel(x0 * m_scale), pixel(y0 * m_scale), pixel(x1 * m_scale), pixel(y1 * m_scale) };
		if (!m_clips.empty())
		{
			const auto& outer = m_clips.back();
			c = { std::max(c[0], outer[0]), std::max(c[1], outer[1]), std::min(c[2], outer[2]), std::min(c[3], outer[3]) };
		}
		m_clips.push_back(c);
	}

	void Overlay::unclip()
	{
		if (!m_clips.empty())
			m_clips.pop_back();
	}

	const Overlay::GlyphSlot* Overlay::glyph(int face, uint16_t index, float size, int quarter)
	{
		const auto key = std::tuple{ face, index, int(std::lround(size * 4)), quarter };
		if (const auto it = m_glyphs.find(key); it != m_glyphs.end())
			return &it->second;
		Fonts::Bitmap b = m_fonts.rasterize(face, index, size, float(quarter) / 4);
		GlyphSlot slot{ b.left, b.top, b.width, b.height };
		if (b.width > 0 && b.width < kAtlasSize && b.height < kAtlasSize)
		{
			if (!m_atlas)
			{
				const std::vector<uint8_t> zero(size_t(kAtlasSize) * kAtlasSize, 0);
				m_atlas = m_renderer.createImage(kAtlasSize, kAtlasSize, true, zero.data());
			}
			if (m_shelfX + b.width + 1 > kAtlasSize)
			{
				m_shelfY += m_shelfHeight + 1;
				m_shelfX = m_shelfHeight = 0;
			}
			if (m_shelfY + b.height + 1 > kAtlasSize)
			{
				// (full: drawn what uses it, then filled again from empty)
				flush();
				m_glyphs.clear();
				m_shelfX = m_shelfY = m_shelfHeight = 0;
			}
			for (uint8_t& a : b.alpha)
			{
				const float v = float(a) / 255;
				a = uint8_t(v * (kTextContrast + 1) / (v * kTextContrast + 1) * 255 + 0.5f);
			}
			if (m_atlas)
				m_renderer.updateImage(*m_atlas, uint32_t(m_shelfX), uint32_t(m_shelfY), uint32_t(b.width), uint32_t(b.height), b.alpha.data());
			slot.x = m_shelfX, slot.y = m_shelfY;
			m_shelfX += b.width + 1;
			m_shelfHeight = std::max(m_shelfHeight, b.height);
		}
		else
			slot.width = slot.height = 0;
		return &(m_glyphs[key] = slot);
	}

	const Overlay::Layout* Overlay::layout(std::wstring_view text, float size, bool bold, float maxWidth, bool markup)
	{
		const auto key = std::tuple{ std::wstring(text), int(std::lround(size * 4)), bold, int(std::lround(maxWidth)), markup };
		auto [it, added] = m_layouts.try_emplace(key);
		Layout& l = it->second;
		if (added)
		{
			std::vector<TextSpan> spans;
			if (markup)
			{
				const std::wstring plain = parseMarkup(text, size, bold, m_sizes, spans, l.inlines, l.art);
				l.laid = layOut(m_fonts, m_family, plain, spans, l.inlines, maxWidth);
			}
			else
			{
				spans.push_back({ 0, text.size(), TextStyle{ size, bold } });
				l.laid = layOut(m_fonts, m_family, text, spans, {}, maxWidth);
			}
		}
		l.used = m_frame;
		return &l;
	}

	void Overlay::drawGlyphs(const Layout& l, float x, float y, Color color, float alpha, bool ownColours)
	{
		const uint32_t base = premultiplied(color, alpha);
		const auto colourOf = [&](uint32_t rgba) {
			if (!ownColours || rgba == 0)
				return base;
			const float a = float(rgba & 255) / 255 * alpha;
			return pack(float(rgba >> 24) / 255 * a, float((rgba >> 16) & 255) / 255 * a, float((rgba >> 8) & 255) / 255 * a, a);
		};
		for (const LaidText::Glyph& g : l.laid.glyphs)
		{
			const float gx = (x + g.x) * m_scale;
			float ix = std::floor(gx);
			int quarter = int((gx - ix) * 4 + 0.5f);
			if (quarter == 4)
				quarter = 0, ix += 1;
			const GlyphSlot* s = glyph(g.face, g.index, g.size * m_scale, quarter);
			if (!s->width)
				continue;
			use(m_atlas.get());
			const float x0 = ix + float(s->left), y0 = std::floor((y + g.baseline) * m_scale + 0.5f) + float(s->top), x1 = x0 + float(s->width), y1 = y0 + float(s->height);
			const float k = 1.0f / kAtlasSize, u0 = float(s->x) * k, v0 = float(s->y) * k, u1 = float(s->x + s->width) * k, v1 = float(s->y + s->height) * k;
			const uint32_t c = colourOf(g.rgba);
			pixelVertex(x0, y0, c, u0, v0), pixelVertex(x1, y0, c, u1, v0), pixelVertex(x1, y1, c, u1, v1);
			pixelVertex(x0, y0, c, u0, v0), pixelVertex(x1, y1, c, u1, v1), pixelVertex(x0, y1, c, u0, v1);
		}
		for (const LaidText::Underline& u : l.laid.underlines)
			fillConvex(roundedRect(x + u.x0, y + u.y, x + u.x1, y + u.y + u.thickness, 0), colourOf(u.rgba));
	}

	void Overlay::drawLayout(const Layout& l, float x, float y, Color color, float alpha, bool shadow)
	{
		if (shadow)
			drawGlyphs(l, x + 1, y + 1, m_shadow, alpha * m_shadowAlpha, false);
		drawGlyphs(l, x, y, color, alpha, true);
		m_inlineAlpha = alpha;
		for (const LaidText::Picture& p : l.laid.pictures)
			drawInline(l.art[p.inlineIndex], x + p.x, y + p.y, l.inlines[p.inlineIndex].width, l.inlines[p.inlineIndex].height);
	}

	float Overlay::text(std::wstring_view text, float x, float y, float size, Color color, float alpha, bool bold, Align align, bool shadow)
	{
		if (!m_drawing || text.empty())
			return 0;
		const Layout* l = layout(text, size, bold, 4096, false);
		const float w = l->laid.width;
		if (align == Align::Center)
			x -= w / 2;
		else if (align == Align::Right)
			x -= w;
		drawLayout(*l, x, y, color, alpha, shadow);
		return w;
	}

	float Overlay::width(std::wstring_view text, float size, bool bold) { return text.empty() ? 0 : layout(text, size, bold, 4096, false)->laid.width; }

	float Overlay::textBlock(std::wstring_view text, float x, float y, float maxWidth, float size, Color color, float alpha, bool bold)
	{
		if (!m_drawing || text.empty())
			return 0;
		const Layout* l = layout(text, size, bold, std::max(maxWidth, 1.0f), false);
		drawLayout(*l, x, y, color, alpha, true);
		return l->laid.height;
	}

	float Overlay::blockHeight(std::wstring_view text, float maxWidth, float size, bool bold)
	{
		return text.empty() ? 0 : layout(text, size, bold, std::max(maxWidth, 1.0f), false)->laid.height;
	}

	float Overlay::richText(std::wstring_view markup, float x, float y, float size, bool bold, Color color, float alpha, Align align, bool shadow)
	{
		if (!m_drawing || markup.empty())
			return 0;
		const Layout* l = layout(markup, size, bold, 4096, true);
		const float w = l->laid.width;
		if (align == Align::Center)
			x -= w / 2;
		else if (align == Align::Right)
			x -= w;
		drawLayout(*l, x, y, color, alpha, shadow);
		return w;
	}

	float Overlay::richWidth(std::wstring_view markup, float size, bool bold) { return markup.empty() ? 0 : layout(markup, size, bold, 4096, true)->laid.width; }

	float Overlay::richBlock(std::wstring_view markup, float x, float y, float maxWidth, float size, bool bold, Color color, float alpha)
	{
		if (!m_drawing || markup.empty())
			return 0;
		const Layout* l = layout(markup, size, bold, std::max(maxWidth, 1.0f), true);
		drawLayout(*l, x, y, color, alpha, true);
		return l->laid.height;
	}

	float Overlay::richHeight(std::wstring_view markup, float maxWidth, float size, bool bold)
	{
		return markup.empty() ? 0 : layout(markup, size, bold, std::max(maxWidth, 1.0f), true)->laid.height;
	}
}
