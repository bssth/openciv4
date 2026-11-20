#include "GreatWall.h"
#include "Utility.h"
#include "CvGameCoreDLL.h"
#include "CvCity.h"
#include "CvGlobals.h"
#include "CvMap.h"
#include "CvPlot.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace oc4::host
{
	GreatWall traceGreatWall(const CvCity& city)
	{
		GreatWall wall;
		wall.cityX = city.getX();
		wall.cityY = city.getY();
		CvMap& map = GC.getMap();
		const CvPlot* cityPlot = city.plot();
		const PlayerTypes owner = city.getOwner();
		if (!cityPlot || owner == NO_PLAYER || cityPlot->getOwner() != owner)
			return wall;

		// The owner's territory joined to the city's plot, side by side (not corner to corner).
		std::vector<char> inside(size_t(map.numPlots()), 0);
		const auto index = [&](int x, int y) -> int {
			const CvPlot* p = map.plot(x, y); // (wraps)
			return p ? p->getY() * map.getGridWidth() + p->getX() : -1;
		};
		const auto in = [&](int x, int y) {
			const int i = index(x, y);
			return i >= 0 && inside[size_t(i)] != 0;
		};
		static constexpr int dx[4] = { 0, 1, 0, -1 }, dy[4] = { 1, 0, -1, 0 }; // north, east, south, west
		std::vector<std::array<int, 2>> todo{ { wall.cityX, wall.cityY } };
		inside[size_t(index(wall.cityX, wall.cityY))] = 1;
		while (!todo.empty())
		{
			const auto [x, y] = todo.back();
			todo.pop_back();
			for (int d = 0; d < 4; ++d)
			{
				const CvPlot* n = map.plot(x + dx[d], y + dy[d]);
				if (!n || n->getOwner() != owner)
					continue;
				const int i = index(x + dx[d], y + dy[d]);
				if (inside[size_t(i)])
					continue;
				inside[size_t(i)] = 1;
				todo.push_back({ x + dx[d], y + dy[d] });
			}
		}

		// Its edge from the plot south of the city where the territory ends: that plot's south
		// side, heading west, the territory on the right. At each corner the edge turns right
		// where it can, so plots that only touch at a corner stay apart.
		int sy = wall.cityY;
		for (int n = 0; n < map.getGridHeight() && in(wall.cityX, sy - 1); ++n)
			--sy;
		// (the plots ahead of a corner, right and left, by heading)
		const auto ahead = [](int px, int py, int d, bool right) -> std::array<int, 2> {
			switch (d)
			{
			case 0: return right ? std::array{ px, py } : std::array{ px - 1, py };
			case 1: return right ? std::array{ px, py - 1 } : std::array{ px, py };
			case 2: return right ? std::array{ px - 1, py - 1 } : std::array{ px, py - 1 };
			default: return right ? std::array{ px - 1, py } : std::array{ px - 1, py - 1 };
			}
		};
		struct Edge
		{
			std::array<int, 2> from, to;
			bool walled = false;
		};
		std::vector<Edge> edges;
		const int area = cityPlot->getAreaIdForGreatWall();
		const auto landOfArea = [&](int x, int y) {
			const CvPlot* p = map.plot(x, y);
			return p && !p->isWater() && p->getAreaIdForGreatWall() == area;
		};
		const std::array<int, 2> start{ wall.cityX + 1, sy };
		std::array<int, 2> at = start;
		int d = 3;
		const size_t limit = size_t(map.numPlots()) * 4 + 4;
		do
		{
			const std::array<int, 2> to{ at[0] + dx[d], at[1] + dy[d] };
			// (the edge's plots: the territory's on the right, the other on the left)
			const auto r = ahead(at[0], at[1], d, true), l = ahead(at[0], at[1], d, false);
			edges.push_back({ at, to, landOfArea(r[0], r[1]) && landOfArea(l[0], l[1]) });
			at = to;
			const auto fr = ahead(at[0], at[1], d, true), fl = ahead(at[0], at[1], d, false);
			if (!in(fr[0], fr[1]))
				d = (d + 1) % 4;
			else if (in(fl[0], fl[1]))
				d = (d + 3) % 4;
		} while ((at != start || d != 3) && edges.size() < limit);

		// A closed loop starts where the first edge ends; pieces start after a gap.
		size_t first = 0;
		bool all = true;
		for (size_t i = 0; i < edges.size(); ++i)
			if (!edges[i].walled)
			{
				all = false;
				first = (i + 1) % edges.size();
			}
		if (all)
		{
			GreatWallChain& c = wall.chains.emplace_back();
			c.closed = true;
			for (size_t i = 0; i < edges.size(); ++i)
				c.corners.push_back(edges[i].to);
			return wall;
		}
		GreatWallChain* piece = nullptr;
		for (size_t k = 0; k < edges.size(); ++k)
		{
			const Edge& e = edges[(first + k) % edges.size()];
			if (!e.walled)
			{
				piece = nullptr;
				continue;
			}
			if (!piece)
			{
				piece = &wall.chains.emplace_back();
				piece->corners.push_back(e.from);
			}
			piece->corners.push_back(e.to);
		}
		return wall;
	}

	namespace
	{
		constexpr int kVersion = 1;
		void put(std::vector<std::byte>& out, int v)
		{
			const auto* b = reinterpret_cast<const std::byte*>(&v);
			out.insert(out.end(), b, b + 4);
		}
	}

	std::vector<std::byte> writeGreatWalls(const std::vector<GreatWall>& walls)
	{
		std::vector<std::byte> out;
		put(out, kVersion);
		put(out, int(walls.size()));
		for (const GreatWall& w : walls)
		{
			put(out, w.cityX);
			put(out, w.cityY);
			put(out, int(w.chains.size()));
			for (const GreatWallChain& c : w.chains)
			{
				put(out, c.closed ? 1 : 0);
				put(out, int(c.corners.size()));
				for (const auto& p : c.corners)
				{
					put(out, p[0]);
					put(out, p[1]);
				}
			}
		}
		return out;
	}

	std::vector<GreatWall> readGreatWalls(const std::vector<std::byte>& data)
	{
		size_t at = 0;
		const auto get = [&]() {
			if (at + 4 > data.size())
				throw std::runtime_error("the save's Great Wall is cut short");
			int v;
			std::memcpy(&v, data.data() + at, 4);
			at += 4;
			return v;
		};
		const auto count = [&]() {
			const int n = get();
			if (n < 0 || size_t(n) > data.size())
				throw std::runtime_error("the save's Great Wall is damaged");
			return size_t(n);
		};
		std::vector<GreatWall> walls;
		if (data.empty() || get() != kVersion)
			return walls;
		walls.resize(count());
		for (GreatWall& w : walls)
		{
			w.cityX = get();
			w.cityY = get();
			w.chains.resize(count());
			for (GreatWallChain& c : w.chains)
			{
				c.closed = get() != 0;
				c.corners.resize(count());
				for (auto& p : c.corners)
				{
					p[0] = get();
					p[1] = get();
				}
			}
		}
		return walls;
	}
}

namespace oc4::host
{
	void Engine::AddGreatWall(CvCity* city)
	{
		std::lock_guard lock(greatWallMutex);
		if (city)
			m_greatWallAsked.push_back({ { city->getX(), city->getY() }, true });
	}

	void Engine::RemoveGreatWall(CvCity* city)
	{
		std::lock_guard lock(greatWallMutex);
		if (city)
			m_greatWallAsked.push_back({ { city->getX(), city->getY() }, false });
	}

	void Engine::resetGreatWalls(std::vector<GreatWall> walls, bool fromWonders)
	{
		std::lock_guard lock(greatWallMutex);
		m_greatWalls = std::move(walls);
		m_greatWallAsked.clear();
		m_greatWallFromWonders = fromWonders;
		++greatWallVersion;
	}

	void Engine::layGreatWalls(const std::function<std::vector<std::pair<int, int>>()>& wonderCities)
	{
		std::vector<std::pair<std::array<int, 2>, bool>> asked;
		bool fromWonders = false;
		{
			std::lock_guard lock(greatWallMutex);
			asked.swap(m_greatWallAsked);
			fromWonders = m_greatWallFromWonders && wonderCities;
			if (fromWonders)
				m_greatWallFromWonders = false;
		}
		if (fromWonders)
			for (const auto& [x, y] : wonderCities())
				asked.push_back({ { x, y }, true });
		if (asked.empty())
			return;
		std::lock_guard lock(greatWallMutex);
		for (const auto& [at, built] : asked)
		{
			if (!built)
			{
				if (std::erase_if(m_greatWalls, [&](const GreatWall& w) { return w.cityX == at[0] && w.cityY == at[1]; }) > 0)
					++greatWallVersion;
				continue;
			}
			// (one wall: while it stands, the engine lays no other)
			const CvPlot* plot = GC.getMap().plot(at[0], at[1]);
			const CvCity* city = plot ? plot->getPlotCity() : nullptr;
			if (!m_greatWalls.empty() || !city)
				continue;
			m_greatWalls.push_back(traceGreatWall(*city));
			++greatWallVersion;
		}
	}
}
