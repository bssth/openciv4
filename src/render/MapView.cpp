#include "render/MapView.h"

#include "anim/Animation.h"
#include "core/FileUtil.h"
#include "gfx/DdsImage.h"
#include "nif/KfmFile.h"
#include "nif/NifFile.h"
#include "render/Particles.h"

#include <windows.h>

#include <algorithm>
#include <bit>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <numbers>
#include <queue>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace oc4::render
{
	namespace
	{
		constexpr uint32_t kTile = 128;  // atlas tiles are resampled to this size
		constexpr uint32_t kTileMips = 8; // 128 .. 1
		constexpr uint32_t kSlots = 32;  // atlas slots per terrain
		constexpr int kSub = 8;          // terrain quads per plot side
		constexpr size_t kMaxPlayers = 256;
		constexpr std::array<float, 4> kDefaultGridLine = { 0, 0.5f, 1.0f / 128, 0 }; // a terrain without grid art: as BtS's land
		constexpr size_t kGridTerrains = 64; // terrains with their own grid line (gridLines in the shader); more take the 64th's
		constexpr uint16_t kNoOverlay = 0xFFFF;
		// The culture borders' tiles (art/terrain/routes/rivers/noriverborder.dds and the rest):
		// 512 texels a side, cells of a plot (128) in four columns and three rows.
		constexpr int kBorderArtSize = 512;

		// DirectionTypes bits, as in the game's connection masks.
		constexpr int kNorthEast = 1 << 1, kSouthEast = 1 << 3, kSouthWest = 1 << 5, kNorthWest = 1 << 7;

		constexpr char kTerrainHlsl[] = R"hlsl(
cbuffer Terrain : register(b1, UNIFORMS)
{
	int2 mapSize;
	int wrapX;
	int wrapY;
	float plotSize;
	uint terrainCount;
	uint showGrid;
	uint showBorders;
	float3 worldOffset;
	float waterZ;
	uint showFog;
	uint3 terrainPad;
	float4 playerColors[256];
	// By terrain, its grid line as the original paints it into the terrain's grid texture
	// (<Grid> in CIV4ArtDefines_Terrain.xml): the ground under the line goes towards grey x by
	// y (land: black by half; coast and ocean: white), z the line's width in plots.
	float4 gridLines[64];
};

Texture2D plots : register(t0, space2);          // 16-bit unorm: terrain, overlay terrain (hills, peaks), owner + 1, flags
Texture2DArray atlas : register(t1, space2);     // 32 tiles per terrain
Texture2DArray decals : register(t2, space2);    // features painted on the ground (flood plains)
Texture2DArray details : register(t3, space2);   // the terrains' detail textures (grey around one half)
Texture2D cells : register(t4, space2);          // 16-bit unorm, two texels a tile: its layers lowest first (atlas slice | rotation << 14), 0xFFFF none
Texture2D borders : register(t5, space2);        // the culture borders' tiles, four texels a plot corner: owner + 1, cell, slice of borderArt, quarter turns (clockwise)
Texture2DArray borderArt : register(t6, space2); // the original's border tiles (noriverborder.dds, the rivers' BorderFiles...): their alpha
SamplerState atlasSampler : register(s1, space2);
SamplerState borderArtSampler : register(s6, space2);
SamplerState decalSampler : register(s2, space2);
SamplerState detailSampler : register(s3, space2);

struct VSIn
{
	float3 pos : TEXCOORD0;
	float3 nrm : TEXCOORD1;
};

struct PSIn
{
	float4 pos : SV_Position;
	float3 world : TEXCOORD0;
	float3 nrm : NORMAL;
	float3 drawn : TEXCOORD1; // where it is drawn (the copy, bent round the globe): the distance fog's
	float under : TEXCOORD2;  // the culture borders' pass: 1 where the ground is under the water's level
};

PSIn VSTerrain(VSIn i)
{
	PSIn o;
	o.drawn = globeBend(i.pos + worldOffset);
	o.pos = mul(float4(o.drawn, 1), viewProj);
	o.world = i.pos;
	o.nrm = i.nrm;
	o.under = 0;
	return o;
}

int2 wrapPlot(int2 p)
{
	// (in floats: an integer modulo is slow on some GPUs, Maxwell's among them; exact for map sizes)
	float2 size = float2(mapSize);
	int2 w = p - int2(size * floor((float2(p) + 0.5) / size));
	p.x = wrapX ? w.x : clamp(p.x, 0, mapSize.x - 1);
	p.y = wrapY ? w.y : clamp(p.y, 0, mapSize.y - 1);
	return p;
}

uint4 plotAt(int2 p) { return uint4(round(plots.Load(int3(wrapPlot(p), 0)) * 65535.0)); }

// An atlas slice turned by quarter turns clockwise.
float4 tile(uint slice, uint rotation, float2 uv, float2 dx, float2 dy)
{
	if (rotation == 1)
	{
		uv = float2(uv.y, 1 - uv.x);
		dx = float2(dx.y, -dx.x);
		dy = float2(dy.y, -dy.x);
	}
	else if (rotation == 2)
	{
		uv = 1 - uv;
		dx = -dx;
		dy = -dy;
	}
	else if (rotation == 3)
	{
		uv = float2(1 - uv.y, uv.x);
		dx = float2(-dx.y, dx.x);
		dy = float2(-dy.y, dy.x);
	}
	return atlas.SampleGrad(atlasSampler, float3(uv, slice), dx, dy);
}

// Smooth noise in [0, 1] over the map, `scale` cells a plot: around a wrapping map without a seam.
float valueNoise(float2 p, float scale)
{
	float2 q = p * scale;
	float2 c = floor(q), f = q - c;
	f = f * f * (3 - 2 * f);
	float2 period = float2(mapSize) * scale;
	float v[4];
	[unroll] for (int k = 0; k < 4; ++k)
	{
		float2 at = c + float2(k & 1, k >> 1);
		if (wrapX)
			at.x -= period.x * floor(at.x / period.x);
		if (wrapY)
			at.y -= period.y * floor(at.y / period.y);
		uint h = uint(at.x) * 0x9E3779B1u ^ uint(at.y) * 0x85EBCA77u;
		h ^= h >> 15;
		h *= 0x2C1B3C6Du;
		h ^= h >> 12;
		v[k] = float(h & 0xFFFF) / 65535.0;
	}
	return lerp(lerp(v[0], v[1], f.x), lerp(v[2], v[3], f.x), f.y);
}

// The ground's light, as measured in the original: the sun from the south-east (slopes facing the
// camera lit, the far side of a ridge dark), flat land a little lighter and bluer than its tiles,
// and how much a slope's light changes with its angle to the sun.
static const float3 kGroundSun = float3(-0.39, 0.59, -0.70);
static const float3 kGroundLight = float3(0.012, 0.024, 0.07);
static const float kSlopeLight = 0.6;

static const int2 kCorner[4] = { int2(0, 1), int2(1, 1), int2(1, 0), int2(0, 0) }; // top left, top right, bottom right, bottom left

// The culture borders, as the original draws them (CultureBorderShader.fx): the tiles of the plot
// corner nearest p, each an owner's, from a cell of its file (a plot square centred on the
// corner, north up unturned), turned back as its river piece is; of them the most opaque, in its
// owner's colour. As colour and alpha to lay over the ground.
float4 cultureBorder(float2 p)
{
	float2 v = floor(p + 0.5);
	float2 l = p - v; // from the corner, in plots: x east, y north
	int2 c = int2(v);
	int2 corners = mapSize + int2(wrapX ? 0 : 1, wrapY ? 0 : 1);
	int2 w = c - int2(float2(corners) * floor((float2(c) + 0.5) / float2(corners)));
	c.x = wrapX ? w.x : clamp(c.x, 0, corners.x - 1);
	c.y = wrapY ? w.y : clamp(c.y, 0, corners.y - 1);
	float best = 0;
	uint owner = 0;
	[unroll] for (int s = 0; s < 4; ++s)
	{
		uint4 t = uint4(round(borders.Load(int3(c.x * 4 + s, c.y, 0)) * 255.0));
		float2 q = l;
		// (the piece turned t.a quarter turns clockwise: the point as many counter-clockwise)
		[unroll] for (uint k = 0; k < 3; ++k)
			q = k < t.a ? float2(-q.y, q.x) : q;
		q = clamp(q + 0.5, 0.5 / 128, 1 - 0.5 / 128);
		float2 uv = (float2(t.g % 4, t.g / 4) + float2(q.x, 1 - q.y)) * 0.25;
		float a = t.r == 0 ? 0 : borderArt.SampleLevel(borderArtSampler, float3(uv, t.b), 0).r;
		if (a > best)
		{
			best = a;
			owner = t.r;
		}
	}
	if (owner == 0)
		return 0;
	float3 oc = playerColors[min(owner - 1, 255)].rgb;
	// (the line, the tiles' opaque middle, a shade darker, as measured before)
	float stroke = smoothstep(0.6, 0.95, best);
	return float4(oc * lerp(1, 0.85, stroke), best * lerp(1, 0.95, stroke));
}

// What the active team knows of the ground at p, blended between plot centres: 0 where it has
// not been, 0.55 where it does not see now, 1 where it does.
float fogAt(float2 p)
{
	float2 c = p - 0.5;
	int2 cell = int2(floor(c));
	float2 w = smoothstep(0, 1, c - cell);
	float2 known[4];
	[unroll] for (int k = 0; k < 4; ++k)
	{
		uint a = plotAt(cell + kCorner[k]).a;
		known[k] = float2((a & 4) ? 1 : 0, (a & 8) ? 1 : 0);
	}
	float2 k = lerp(lerp(known[3], known[2], w.x), lerp(known[0], known[1], w.x), w.y);
	return k.x * lerp(0.55, 1, k.y);
}

float4 PSTerrain(PSIn i) : SV_Target
{
	// The tile grid is offset by half a plot: each tile spans four plot centres.
	float2 p = i.world.xy / plotSize;
	float2 c = p - 0.5;
	int2 cell = int2(floor(c));
	float2 f = c - cell;
	float2 uv = float2(f.x, 1 - f.y); // atlas tiles have north up
	float2 dcx = ddx(c), dcy = ddy(c);
	float2 dx = float2(dcx.x, -dcx.y), dy = float2(dcy.x, -dcy.y);

	float2 known[4]; // revealed, visible
	uint decal[4];   // 1-based decal of the plot's feature, 0 none
	[unroll] for (int k = 0; k < 4; ++k)
	{
		uint4 d = plotAt(cell + kCorner[k]);
		known[k] = float2((d.a & 4) ? 1 : 0, (d.a & 8) ? 1 : 0);
		decal[k] = d.a >> 8;
	}

	// Nothing to blend where the team has been to none of the four plots: black, as the fog
	// below would make it.
	if (showFog && known[0].x + known[1].x + known[2].x + known[3].x == 0)
		return float4(0, 0, 0, 1);

	// The tile's layers, worked out when the plots change (updateCells): the lowest as a whole
	// tile, every higher one over it by its alpha, then the hills and peaks.
	int2 at = int2(clamp(cell.x + 1, 0, mapSize.x) * 2, clamp(cell.y + 1, 0, mapSize.y));
	uint4 lo = uint4(round(cells.Load(int3(at, 0)) * 65535.0));
	uint4 hi = uint4(round(cells.Load(int3(at + int2(1, 0), 0)) * 65535.0));
	uint entry[6] = { lo.r, lo.g, lo.b, lo.a, hi.r, hi.g };
	float3 color = 0;
	float3 lineColor = 0; // the ground as the grid texture has it under a grid line
	float lineWidth = 0;
	float3 detail = 0.5; // blended as the colours are
	float2 dp = p * 0.5; // detail repeats every 2 plots (the landscape's 4 plots a cell, texture scale 2)
	[loop] for (int step = 0; step < 6; ++step)
	{
		uint e = entry[step];
		if (e == 0xFFFF)
			break;
		uint slice = e & 0x3FFF;
		float4 s = tile(slice, e >> 14, uv, dx, dy);
		float3 ds = details.Sample(detailSampler, float3(dp, slice >> 5)).rgb;
		color = step == 0 ? s.rgb : lerp(color, s.rgb, s.a);
		detail = step == 0 ? ds : lerp(detail, ds, s.a);
		float4 gl = gridLines[min(slice >> 5, 63u)];
		float3 lc = lerp(s.rgb, gl.xxx, gl.y);
		lineColor = step == 0 ? lc : lerp(lineColor, lc, s.a);
		lineWidth = step == 0 ? gl.z : lerp(lineWidth, gl.z, s.a);
	}

	// The grid, as the original draws it: each terrain's blend atlas swapped for its grid twin,
	// the same tiles with a line along the plot edges (through the middle of each tile). So it
	// lies in the ground itself: over hills and down to the sea floor, under the water, rivers,
	// forests and improvements, lit and fogged as the ground is. The grid textures' mips are
	// drawn by hand: down to 16 texels a plot each draws the line again, as many texels wide
	// and as dark as the full texture does (so it stays a pixel or two wide, not fainter), and
	// the smaller ones have none. Sampled as they are: trilinear (isotropic), bilinear texels.
	if (showGrid)
	{
		float2 edge = abs(p - round(p)); // plots to the nearest edge
		float footprint = max(length(float2(dcx.x, dcy.x)), length(float2(dcx.y, dcy.y))); // plots a pixel
		float lod = max(log2(footprint * 128), 0); // the mip, 128 texels a plot at the top
		float level = floor(lod);
		float2 cover = 0;
		[unroll] for (int m = 0; m < 2; ++m)
		{
			float l = level + m;
			float texel = exp2(l) / 128;
			float width = lineWidth * exp2(l); // (as many texels as at the top)
			// a line of `width` filtered over a texel; none from 8 texels a plot down
			float2 c = clamp((width + texel) * 0.5 - edge, 0, min(width, texel)) / texel;
			cover += (l <= 3 ? c : 0) * (m == 0 ? 1 - (lod - level) : lod - level);
		}
		color = lerp(color, lineColor, saturate(cover.x + cover.y - cover.x * cover.y));
	}

	// The detail textures sharpen the ground up close (their mips fade to grey far away). Added
	// round their grey, as the original adds them: the ground keeps its tiles' colour.
	color += detail - 0.5;

	// Features painted on the ground (flood plains: grass on the plot's own ground): blended
	// between the plot centres that have them, in patches that grow together towards the
	// plot's edges with river and leave the ground between them away from it.
	uint d = max(max(decal[0], decal[1]), max(decal[2], decal[3]));
	if (d > 0)
	{
		float2 w = smoothstep(0, 1, f);
		float4 has = float4(decal[0] == d, decal[1] == d, decal[2] == d, decal[3] == d);
		float amount = lerp(lerp(has[3], has[2], w.x), lerp(has[0], has[1], w.x), w.y);
		// Distance to the nearest plot edge with river, in plots (a plot's flags 16 and 32: river
		// along its south and east edges).
		int2 own = int2(floor(p));
		float river = 2;
		[unroll] for (int k = 0; k < 9; ++k)
		{
			int2 q = own + int2(k % 3 - 1, k / 3 - 1);
			uint flags = plotAt(q).a;
			if (flags & 16)
				river = min(river, length(p - float2(clamp(p.x, q.x, q.x + 1), q.y)));
			if (flags & 32)
				river = min(river, length(p - float2(q.x + 1, clamp(p.y, q.y, q.y + 1))));
		}
		float nearRiver = 1 - smoothstep(0.05, 0.8, river);
		float4 tex = decals.Sample(decalSampler, float3(p * 2, d - 1)); // (tiled: wrapping)
		float patches = 0.5 * valueNoise(p, 3) + 0.3 * valueNoise(p, 7) + 0.2 * valueNoise(p, 17);
		float cover = amount * lerp(0.3, 1.0, nearRiver);
		color = lerp(color, tex.rgb + detail - 0.5, smoothstep(0.45, 0.65, cover * (0.35 + 0.95 * patches)));
	}

	float3 n = normalize(i.nrm);
	// Lit as the original lights the ground: its light is added, not multiplied, so flat land
	// keeps its tiles' own colour (grassland as bright as GrassBlend.dds) and slopes are lighter
	// or darker by how much more or less they face the sun than flat land does.
	float ndl = saturate(dot(n, -kGroundSun));
	color = saturate(color + kGroundLight + (ndl - saturate(-kGroundSun.z)) * kSlopeLight);

	float2 pw = fwidth(p);
	if (showFog)
	{
		// What the active team knows, blended between plot centres: black where it has not
		// been, dimmed where it does not see now.
		float2 w = smoothstep(0, 1, f);
		float2 k = lerp(lerp(known[3], known[2], w.x), lerp(known[0], known[1], w.x), w.y);
		color *= k.x * lerp(0.55, 1, k.y);
	}
	return float4(color * distanceFog(i.drawn), 1);
}

// The culture borders as their own layer, as the original's (CultureBorderShader.fx): not lit,
// darkened by the fog of war. In two passes (showBorders): 1, the ground above the water's level,
// right after the terrain, so that what stands on it blends over the border as over the ground
// (the original: the soft dirt under resources and improvements tinted, the rocks on it not);
// 2, at the water's level where the ground is under it, after the water, over it at full
// strength (the water does not dim them).
PSIn VSBorder(VSIn i)
{
	PSIn o;
	// (a little above the ground and towards the eye, so that it is never behind it nor behind the
	// pieces laid on it, drawn before it: rivers 0.8, roads 0.6, a farm's crops from 0.5 up, a plot
	// 180 world units; the model shader moves those 6 towards the eye. What is drawn after it
	// covers it whatever its depth: it writes none.)
	float3 pos = float3(i.pos.xy, max(i.pos.z, waterZ) + 0.012 * plotSize);
	o.drawn = globeBend(pos + worldOffset);
	float3 toEye = eyePos - o.drawn;
	o.pos = mul(float4(dot(toEye, toEye) > 100 ? o.drawn + normalize(toEye) * 8 : o.drawn, 1), viewProj);
	o.world = pos;
	o.nrm = i.nrm;
	o.under = i.pos.z < waterZ ? 1 : 0;
	return o;
}

float4 PSBorder(PSIn i) : SV_Target
{
	// (each pass its part: they meet where the ground crosses the water's level)
	clip(showBorders == 2 ? i.under - 0.5 : 0.5 - i.under);
	float2 p = i.world.xy / plotSize;
	float4 b = cultureBorder(p);
	clip(b.a - 1.0 / 255);
	if (showFog)
		b.rgb *= fogAt(p);
	return float4(b.rgb * distanceFog(i.drawn), b.a);
}
)hlsl";

		struct TerrainVertex
		{
			Vec3 position;
			Vec3 normal;
		};

		struct TerrainConstants
		{
			int32_t mapW, mapH, wrapX, wrapY;
			float plotSize;
			uint32_t terrainCount, showGrid, showBorders;
			float offset[3];
			float waterZ;
			uint32_t showFog, pad[3];
			float playerColors[kMaxPlayers][4];
			float gridLines[kGridTerrains][4];
		};

		// The water plane, as the original's Water.fx draws it: over the ground (the sea floor is
		// the coast and ocean terrains' own tiles), one texture (CIV4WaterPlaneInfos: Water.dds)
		// twice, scrolling, half and half; their alphas multiplied are added as sparkles; the whole
		// laid over the floor at the plane's material alpha, faded in from the shore. Unlit: no
		// reflections, no sun glint. Darkened by the fog of war as the ground is.
		constexpr char kWaterHlsl[] = R"hlsl(
cbuffer Water : register(b1, UNIFORMS)
{
	float4 waterUv1;   // u = dot(world.xy, xy), v = dot(world.xy, zw), world units
	float4 waterUv2;
	float4 waterScroll; // texture offsets: xy the first, zw the second
	float3 waterOffset; // the copy of the map drawn (wrapping)
	float waterAlpha;   // the plane's material alpha
	int2 waterMapSize;
	int waterWrapX;
	int waterWrapY;
	float waterPlotSize;
	uint waterShowFog;
	float waterShore;   // world units: how deep the ground is where the water's shore is
	float waterShoreWidth; // world units of depth over which it fades in there
};

static const float kWaterRedOffset = 14.0 / 255;

Texture2D waterTex : register(t0, space2);   // the water plane's base texture
Texture2D waterPlots : register(t1, space2); // as the terrain's plots: flags in alpha (4 revealed, 8 visible)
SamplerState waterSampler : register(s0, space2);

struct WaterIn
{
	float3 pos : TEXCOORD0;   // world, at the water's level
	float depth : TEXCOORD1;  // of the land/water levels under it, world units
	float bed : TEXCOORD2;    // how far a river's bed is carved below those levels there, world units
};

struct WaterOut
{
	float4 pos : SV_Position;
	float2 uv1 : TEXCOORD0;
	float2 uv2 : TEXCOORD1;
	float2 plot : TEXCOORD2;
	float depth : TEXCOORD3;
	float bed : TEXCOORD4;
	float3 drawn : TEXCOORD5; // (the distance fog's)
};

WaterOut VSWater(WaterIn i)
{
	WaterOut o;
	o.drawn = globeBend(i.pos + waterOffset);
	o.pos = mul(float4(o.drawn, 1), viewProj);
	o.uv1 = float2(dot(i.pos.xy, waterUv1.xy), dot(i.pos.xy, waterUv1.zw)) + waterScroll.xy;
	o.uv2 = float2(dot(i.pos.xy, waterUv2.xy), dot(i.pos.xy, waterUv2.zw)) + waterScroll.zw;
	o.plot = i.pos.xy / waterPlotSize;
	o.depth = i.depth;
	o.bed = i.bed;
	return o;
}

int2 waterWrap(int2 p)
{
	float2 size = float2(waterMapSize);
	int2 w = p - int2(size * floor((float2(p) + 0.5) / size));
	p.x = waterWrapX ? w.x : clamp(p.x, 0, waterMapSize.x - 1);
	p.y = waterWrapY ? w.y : clamp(p.y, 0, waterMapSize.y - 1);
	return p;
}

float2 waterKnown(int2 p)
{
	uint a = uint(round(waterPlots.Load(int3(waterWrap(p), 0)).a * 65535.0));
	return float2((a & 4) ? 1 : 0, (a & 8) ? 1 : 0);
}

// Smooth noise in [0, 1], `scale` cells a plot, without a seam where the map wraps.
float waterNoise(float2 p, float scale)
{
	float2 q = p * scale;
	float2 c = floor(q), f = q - c;
	f = f * f * (3 - 2 * f);
	float2 period = float2(waterMapSize) * scale;
	float v[4];
	[unroll] for (int k = 0; k < 4; ++k)
	{
		float2 at = c + float2(k & 1, k >> 1);
		if (waterWrapX)
			at.x -= period.x * floor(at.x / period.x);
		if (waterWrapY)
			at.y -= period.y * floor(at.y / period.y);
		uint h = uint(at.x) * 0x9E3779B1u ^ uint(at.y) * 0x85EBCA77u;
		h ^= h >> 15;
		h *= 0x2C1B3C6Du;
		h ^= h >> 12;
		v[k] = float(h & 0xFFFF) / 65535.0;
	}
	return lerp(lerp(v[0], v[1], f.x), lerp(v[2], v[3], f.x), f.y);
}

float4 PSWater(WaterOut i) : SV_Target
{
	// The shore: where the ground is that deep, a little in or out along it (the original's coast
	// fade is a ragged edge), the water ending over a short way.
	float wobble = 0.65 * waterNoise(i.plot, 1.5) + 0.35 * waterNoise(i.plot, 4) - 0.5;
	// (in a river's mouth the sea comes in gradually, over the river's own water)
	float mouth = saturate(i.bed / (0.25 * waterShore)) * waterShore;
	float coast = saturate((i.depth - waterShore * (1 + 0.2 * wobble) + mouth) / (waterShoreWidth + mouth));
	clip(coast - 1.0 / 255);
	float4 a = waterTex.Sample(waterSampler, i.uv1);
	float4 b = waterTex.Sample(waterSampler, i.uv2);
	// (less red than Water.dds gives: measured in the original over open sea, coast and lakes, the
	// water 14/255 less red at its 0.5 alpha, its sparkles as white as here; cause not found)
	float3 color = lerp(a.rgb, b.rgb, 0.5) - float3(kWaterRedOffset, 0, 0) + a.a * b.a * coast;
	if (waterShowFog)
	{
		// (as the terrain: blended between plot centres, black where never seen, dimmed where not seen now)
		float2 c = i.plot - 0.5;
		int2 cell = int2(floor(c));
		float2 w = smoothstep(0, 1, c - cell);
		float2 k = lerp(lerp(waterKnown(cell), waterKnown(cell + int2(1, 0)), w.x),
			lerp(waterKnown(cell + int2(0, 1)), waterKnown(cell + int2(1, 1)), w.x), w.y);
		color *= k.x * lerp(0.55, 1, k.y);
	}
	// (D3D fogs a pixel before blending it: the water darkens over a floor darkened as much)
	return float4(saturate(color) * distanceFog(i.drawn), waterAlpha * coast);
}
)hlsl";

		struct WaterVertex
		{
			Vec3 position;
			float depth, bed;
		};

		struct WaterConstants
		{
			float uv1[4], uv2[4], scroll[4];
			float offset[3];
			float alpha;
			int32_t mapW, mapH, wrapX, wrapY;
			float plotSize;
			uint32_t showFog;
			float shore, shoreWidth;
		};

		uint32_t mix(uint32_t h)
		{
			h ^= h >> 16;
			h *= 0x7FEB352Du;
			h ^= h >> 15;
			h *= 0x846CA68Bu;
			h ^= h >> 16;
			return h;
		}
		uint32_t hash(int x, int y, int salt) { return mix(uint32_t(x) * 0x9E3779B1u ^ mix(uint32_t(y) + 0x632BE5ABu * uint32_t(salt))); }
		float unitFloat(uint32_t h) { return float(h >> 8) / 16777216.0f; }

		float smoother(float t)
		{
			t = std::clamp(t, 0.0f, 1.0f);
			return t * t * t * (t * (t * 6 - 15) + 10);
		}
		float bump(float t)
		{
			if (t >= 1)
				return 0;
			const float u = 1 - t * t;
			return u * u;
		}

		Affine placement(Vec3 position, float yaw, float scale) { return { toMat33(axisAngle({ 0, 0, 1 }, yaw)), position, scale }; }

		// DirectionTypes bits turned clockwise by a multiple of 45 degrees.
		int turned(int connections, int degrees)
		{
			const int steps = ((degrees / 45) % 8 + 8) % 8;
			int out = 0;
			for (int d = 0; d < 8; ++d)
				if (connections & (1 << d))
					out |= 1 << ((d + steps) % 8);
			return out;
		}
		Affine translation(Vec3 t) { return { Mat33{}, t, 1 }; }

		std::string folderOf(const std::string& key) { return key.substr(0, key.rfind('/') + 1); }

		struct Frustum
		{
			std::array<std::array<float, 4>, 6> planes{};

			explicit Frustum(const float* m) // row-major, p_clip = M p
			{
				const auto row = [&](int r) { return std::array<float, 4>{ m[r * 4], m[r * 4 + 1], m[r * 4 + 2], m[r * 4 + 3] }; };
				const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
				for (int i = 0; i < 4; ++i)
				{
					planes[0][i] = r3[i] + r0[i];
					planes[1][i] = r3[i] - r0[i];
					planes[2][i] = r3[i] + r1[i];
					planes[3][i] = r3[i] - r1[i];
					planes[4][i] = r2[i];
					planes[5][i] = r3[i] - r2[i];
				}
			}

			bool all = false; // (the globe view: the flat map's places tell nothing)

			bool sphere(Vec3 c, float r) const
			{
				if (all)
					return true;
				for (const auto& p : planes)
					if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -r * std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]))
						return false;
				return true;
			}
		};
	}

	struct LoadedModel
	{
		bool ok = false;
		scene::Model model;
		std::shared_ptr<GpuImage> flagDecal; // runtime colours; outlives the GPU model's texture references
		std::unique_ptr<GpuModel> gpu;
		std::optional<anim::Sequence> idle, run; // event codes 1000, 1010
		// A flag's medallions (Medallions.kfm): medallion n (1..7) in state s (0 off, 1 can move,
		// 2 cannot move, 3 "fortify": white, 4 has moved but can go on) at event code 10 n + s.
		std::map<uint32_t, anim::Sequence> medallions;
		// A resource's look by its state (event codes 1, 2, 3: CvPlot::getVisibleBonusState):
		// not improved, improved, improved and worked; each shows its own nodes.
		std::optional<anim::Sequence> states[3];
		// Fortifying, then fortified: melee (1020, 1021) and ranged (1030, 1031), as the
		// animation categories ANIMCAT_*_FORTIFY and *_FORTIFIED of the first set.
		std::optional<anim::Sequence> fortify[2], fortified[2];
		// Fighting, by animation set (a, b, c; event code + 1000 a set) and kind (melee, ranged):
		// a strike (ANIMCAT_*_STRIKE 22, 32), a death (*_DIE 24, 33) and its fading (25, 34).
		std::optional<anim::Sequence> strike[3][2], die[3][2], dieFade[3][2];
		// Hurt in a round it survives (ANIMCAT_MELEE_HURT 23, by set; the ranged have none of
		// their own); healing (ANIMCAT_HEAL 4).
		std::optional<anim::Sequence> hurt[3][2], heal;
		// Giving itself up (ANIMCAT_SURRENDER: event code 1002, a worker's or a settler's), and a
		// unit's fading out (its XFadeOut, code 1: the material's alpha to 0).
		std::optional<anim::Sequence> surrender, fadeOut;
		std::optional<anim::Sequence> antiAir; // event code 1042: a SAM's anti-air strike (its missile)
		// Working, by work (irrigate, build, mine, chop, shovel, railroad: categories 50 + 3 a
		// work): its beginning, the work, a pause in it; founding a city (ANIMCAT_FOUND 40).
		std::optional<anim::Sequence> work[6][3], found;
		// Damaged (ANIMCAT_DAMAGE_0..3: event codes 500, 510, 520, 530, + 1000 a damage state
		// from 1): layers over the pose, each a part's (the hull's damage texture revealed, its
		// smoke shown, a tank's turret bent), by state.
		std::vector<anim::Sequence> damage[4];
		std::optional<anim::Sequence> embedded;  // the file's own controllers (flowing water, wheels)
		bool embeddedMoves = false;              // ... which move or hide nodes, not only materials
		std::vector<Affine> bindLocals, bindWorld;
		std::vector<uint8_t> bindVisible;
		// Its nodes laid on the terrain (scene::DrawItem::contour), if any: 1 per such node.
		std::vector<uint8_t> contourNodes;
		// The node the pieces hang below; per node the piece it belongs to (itself above).
		int spread = -1;
		// Its nodes' attachables ("ATTACH:<node>:<type>"), and those loaded.
		std::vector<std::pair<int, std::string>> attachTags;
		struct Attachment
		{
			LoadedModel* model = nullptr;
			int node = -1;
			float scale = 1;
		};
		std::vector<Attachment> attachments;
		bool attachResolved = false;
		// Where the model clears trees (bind pose, model space): centre and radius.
		std::vector<std::pair<Vec3, float>> treeCuts;
		// A river piece's water ("ContourGeometry RiverBase": a grid over the plot textured with
		// its bed and water): its triangles (model space, bind pose) with their texture
		// coordinates, and where that texture is water. The trees standing in it go.
		struct Water
		{
			std::vector<Vec3> corners; // three a triangle: x, y and (in z) nothing
			std::vector<std::array<float, 2>> uvs;
			std::shared_ptr<const std::vector<uint8_t>> alpha; // 2: water, 1: its bed, 0: neither
			uint32_t width = 0, height = 0;
		};
		std::optional<Water> water;
		std::vector<int> top;
		Bounds bounds;

		// The pieces with their own bounds, for house sets ("1x1_01", "2x1_03", ...).
		struct Part
		{
			int node = -1;
			Bounds bounds;
			std::vector<uint8_t> visible;
		};
		std::vector<Part> parts;
	};

	struct MapView::Impl
	{
		Impl(Renderer& r, const AssetResolver& a) : renderer(r), assets(a), flagArt(a) { createPipeline(); }
		~Impl()
		{
			for (const auto& [samples, g] : groundPipelines)
				for (SDL_GPUGraphicsPipeline* p : { g.terrain, g.water, g.border })
					SDL_ReleaseGPUGraphicsPipeline(renderer.device(), p);
			SDL_ReleaseGPUShader(renderer.device(), terrainVS);
			SDL_ReleaseGPUShader(renderer.device(), terrainPS);
			SDL_ReleaseGPUSampler(renderer.device(), sampler);
			SDL_ReleaseGPUSampler(renderer.device(), detailWrap);
			SDL_ReleaseGPUShader(renderer.device(), waterVS);
			SDL_ReleaseGPUShader(renderer.device(), waterPS);
			SDL_ReleaseGPUSampler(renderer.device(), waterSampler);
			SDL_ReleaseGPUShader(renderer.device(), borderVS);
			SDL_ReleaseGPUShader(renderer.device(), borderPS);
		}

		struct Instance
		{
			LoadedModel* model = nullptr;
			Affine place;
			std::vector<Affine> world;    // static instances: the final transforms
			std::vector<uint8_t> visible; // empty: the model's own
			std::vector<scene::Material> materials; // empty: the model's own (a forest piece's trees cut)
			Vec3 center;
			float radius = 0;
			Vec3 team = kDefaultTeamColor;
			bool animated = false;
			float phase = 0;
			// A unit's figure: placed every frame, as it grows with the camera's distance.
			bool figure = false;
			Vec3 anchor, offset;
			float yaw = 0, scale = 1;
			bool onGround = false; // heights over the terrain (roads)
			bool route = false;    // a road or railroad piece: trees beside it go (cutTrees)
			bool laid = false;     // a piece laid on the terrain (rivers, roads, a farm's crops): drawn before the culture borders
			int plot = -1;         // the plot it belongs to (fog of war)
			uint64_t unit = 0;     // a unit's figure: owner and id
			const anim::Sequence* sequence = nullptr; // animated: what it plays (none: its idle)
			bool resting = false, ranged = false; // fortified (and so on); its art's fortify is the ranged one
			// In a battle (into map.battles): its side, its place in the formation (the last
			// ones die first), and (the attacker) the step it takes towards the defender.
			int battle = -1;
			bool attacking = false;
			int figureIndex = 0;
			int role = 0; // 0 a figure of the formation, 1 its great general, 2 its siege tower
			Vec3 step;
			Vec3 soundAt; // where its sounds come from (plot units)
			int work = -1;       // at work: UnitView::work
			int damage = 0;      // its damage state: 0 none, 1..4 the model's damage layers
			bool air = false;    // a plane (UnitView::air)
			int actionX = -1, actionY = -1;
			int action = -1;     // acting (UnitView::action): since when (this view's clock), how long
			float actionStart = 0, actionLength = 0;
			float foundAt = -1;  // founding a city: since when (this view's clock)
			LoadedModel* shadow = nullptr; // a figure's blob shadow, under this node
			int shadowNode = -1;
			float shadowScale = 1;
		};

		Renderer& renderer;
		const AssetResolver& assets;
		ButtonArt flagArt;
		std::unordered_map<std::string, std::vector<std::string>> routeVariants; // routeFiles: by the XML's file
		MapSnapshot map;
		bool hasMap = false;
		uint64_t artGeneration = ~0ull;
		float time = 0;
		// The particles of the instances drawn (their emitters at work): by instance, kept while
		// it is drawn.
		struct Effect
		{
			Particles particles;
			float last = 0;
			uint32_t drawn = 0;
		};
		std::unordered_map<uint64_t, Effect> effects;
		// The effects set off (triggerEffect): until their time is up, then their particles' end.
		struct Burst
		{
			LoadedModel* model = nullptr;
			Affine place;
			float start = 0, until = 0, last = 0;
			Particles particles;
			// A projectile (an arrow, a rock, a bomb, a missile): it flies from `from` to `to` for
			// `flight` seconds (its run clip, if any), then plays its explosion (event code 1022),
			// whose text keys set off its effects and sounds.
			bool projectile = false;
			Vec3 from, to;
			float flight = 0, scale = 1, keysLast = -1;
		};
		std::vector<Burst> bursts;
		std::vector<std::function<void()>> pendingBursts; // (set off before the map came)
		std::unordered_map<uint64_t, uint32_t> effectsThisFrame;
		uint32_t effectFrame = 0;
		std::vector<std::string> problems;
		std::unordered_set<std::string> reported;

		// Terrain
		int gridW = 0, gridH = 0; // vertices per row and column
		std::vector<float> heights;
		std::vector<uint8_t> shape;     // per plot: water | hills << 1 | peak << 2
		std::vector<int32_t> featureKey; // per plot: feature and variety
		std::vector<uint64_t> decorKey;  // per plot: improvement, bonus, route
		std::shared_ptr<GpuBuffer> vb, ib;
		uint32_t indexCount = 0;
		// The water plane: over the tiles with water at a corner, at the water's level.
		std::shared_ptr<GpuBuffer> waterVb, waterIb;
		uint32_t waterIndexCount = 0;
		std::shared_ptr<GpuImage> waterTex;
		SDL_GPUShader* waterVS = nullptr;
		SDL_GPUShader* waterPS = nullptr;
		SDL_GPUSampler* waterSampler = nullptr;
		std::shared_ptr<GpuImage> plotTex, cellTex, atlasTex, groundTex, decalTex, detailTex, borderTex;
		// The culture borders: the original's tiles (borderArtTex, a slice each: no river, at sea,
		// then the river pieces' BorderFiles, by riverBorderSlice), the river piece at each plot
		// corner (cornerRiver: index into map.rivers and its turn in degrees; -1 none), and the
		// owners and water the corners' tiles (borderTex) were last worked out for.
		std::shared_ptr<GpuImage> borderArtTex;
		std::vector<int> riverBorderSlice;
		std::vector<std::pair<int16_t, int16_t>> cornerRiver;
		bool cornerRiverChanged = true;
		std::vector<int16_t> borderOwners;
		std::vector<uint8_t> borderWater;
		std::vector<uint32_t> terrainColors; // BGRA: the average of each terrain's plain tile
		std::vector<std::array<float, 4>> gridLines; // by terrain: grey, amount, width in plots (gridLines in the shader)
		std::vector<uint8_t> minimapPixels;
		uint64_t minimapVersion = 0;
		std::vector<uint16_t> featureDecal; // by FeatureTypes: 1-based slice of decalTex, 0 none
		SDL_GPUShader* terrainVS = nullptr;
		SDL_GPUShader* terrainPS = nullptr;
		SDL_GPUShader* borderVS = nullptr;
		SDL_GPUShader* borderPS = nullptr;
		SDL_GPUSampler* sampler = nullptr;
		SDL_GPUSampler* detailWrap = nullptr;

		// Models
		std::unordered_map<std::string, std::unique_ptr<LoadedModel>> models;
		struct WaterAlpha
		{
			std::shared_ptr<const std::vector<uint8_t>> alpha;
			uint32_t width = 0, height = 0;
		};
		std::unordered_map<std::string, WaterAlpha> waterAlpha; // by texture
		std::map<std::string, std::shared_ptr<const nif::File>> kfCache;
		std::vector<Instance> features, decor, cities, units;
		std::vector<std::pair<Vec3, float>> layoutCuts; // laid-out pieces' clearings: where, radius (world units)
		// Each city's pieces by its plot, kept while what it shows stays the same (cleared with the terrain or the art).
		struct CityLayout
		{
			std::string key;
			std::vector<Instance> pieces;
		};
		std::unordered_map<int, CityLayout> cityLayouts;

		// Units walking through the plots of their accepted move order.
		struct Motion
		{
			Vec3 from, to;
			float start = 0, duration = 0;
			float spread = 0; // the last figure arrives that much later (the original's run one by one)
			float orderStart = 0, orderSeconds = 0; // the whole order's walk this leg is part of
		};
		std::unordered_map<uint64_t, std::deque<Motion>> motions;
		std::unordered_map<uint64_t, Vec3> unitPositions; // the plot where each unit was last shown
		// Where each unit's figure was drawn last (round its middle), for clicks on them.
		struct FigureHit
		{
			Vec3 center;
			float radius = 0;
			int plot = -1;
		};
		std::vector<FigureHit> figureHits;
		bool ringsDrawn = false; // the selection rings are the game's (not the overlay's own)
		// The city bars' production symbols: a button cut round by the bar's mask, by its art.
		std::map<std::string, ButtonArt::Icon> cityBarSymbols;
		// Where the city bars were drawn last (in the overlay's units) and their cities' plots, in drawing order.
		struct CityBarPlace
		{
			float x0, y0, x1, y1;
			int x, y;
		};
		std::vector<CityBarPlace> cityBars;

		// The ring the game puts under the selected units (Art/Units/Selection Effect): its
		// circle turning (the default clip) and its dots over it as the head unit can move
		// (no dots, red, yellow, green: UnitView::moves + 1).
		static constexpr Vec3 kEnemyGlow{ 0.74f, 0.28f, 0.16f }; // (the ring's colour under an enemy: the original's red)
		struct SelectionRing
		{
			bool tried = false, ok = false;
			std::shared_ptr<const nif::File> file;
			scene::Model model;
			std::unique_ptr<GpuModel> gpu;
			anim::Player player;
			std::optional<size_t> dots[4];
			int shown = -3;
		} ring;

		void loadRing()
		{
			ring.tried = true;
			const std::string folder = "art/units/selection effect/";
			try
			{
				const auto loc = assets.locate(folder + "selection_ground.nif");
				if (!loc)
					throw std::runtime_error("not found");
				ring.file = std::make_shared<const nif::File>(nif::parseFile(assets.read(*loc)));
				ring.model = scene::buildModel(*ring.file);
				// (drawn where it is drawn, before the units, as its alpha property's "no sorter" has
				// it: it tests no depth, and the figures standing on it cover it, as in the original)
				// It tests depth and writes it where it shows (alpha test), a little above the
				// ground: the rivers' and roads' pieces, laid lower and drawn with the blended things
				// after it, cannot cover it then, as in the original, and what stands in front of it
				// (a hut, blended too) still covers it.
				for (auto& item : ring.model.items)
				{
					item.material.blendUnsorted = true;
					item.material.zTest = true;
					item.material.zWrite = true;
					item.material.alphaTest = true;
					item.material.alphaFunc = scene::Compare::Greater;
					item.material.alphaRef = 8;
				}
				ring.gpu = renderer.upload(ring.model, assets, *loc);
				std::vector<anim::Clip> clips;
				std::optional<nif::KfmFile> kfm;
				if (const auto k = assets.locate(folder + "selection_ground.kfm"))
				{
					kfm = nif::parseKfm(assets.read(*k));
					for (const auto& a : kfm->animations)
						if (const auto kf = assets.locate(folder + a.kfFile))
							if (anim::Sequence seq = anim::Sequence::bind(std::make_shared<const nif::File>(nif::parseFile(assets.read(*kf))), a.sequenceIndex,
									ring.model);
								seq.channelCount() > 0)
								clips.push_back({ toLowerAscii(a.kfFile), a.eventCode, std::move(seq) });
				}
				std::optional<size_t> turning;
				static const char* const kDots[4] = { "nodots", "allred", "allyelo", "allgreen" };
				for (size_t i = 0; i < clips.size(); ++i)
				{
					if (clips[i].label.find("default") != std::string::npos)
						turning = i;
					for (int d = 0; d < 4; ++d)
						if (clips[i].label.find(kDots[d]) != std::string::npos)
							ring.dots[d] = i;
				}
				ring.player = anim::Player(std::move(clips), std::move(kfm));
				if (turning)
					ring.player.play(*turning, false);
				ring.ok = ring.gpu != nullptr;
			}
			catch (const std::exception& e)
			{
				problem(folder + "selection_ground.nif: " + e.what());
			}
		}

		// Under each selected stack the game's ring (false: its art is missing).
		bool drawRings()
		{
			if (!ring.tried)
				loadRing();
			if (!ring.ok)
				return false;
			for (const UnitView& u : map.units)
			{
				// (none under a stack while it fights, as in the original; and under an enemy in the active
				// team's land, red and without dots: the enemy glow)
				if ((!u.selected && !u.enemy) || u.battle >= 0)
					continue;
				const bool glow = !u.selected;
				// (its dots: how the head unit can move)
				if (const int d = glow ? 0 : std::clamp(u.moves + 1, 0, 3); d != ring.shown && ring.dots[d])
				{
					ring.shown = d;
					ring.player.setLayers({ *ring.dots[d] });
				}
				Vec3 at{ (float(u.x) + 0.5f) * S(), (float(u.y) + 0.5f) * S(), 0 };
				// (none while its units walk there: the original's comes once they are in)
				const uint64_t key = uint64_t(uint32_t(u.owner)) << 32 | uint32_t(u.id);
				if (activeMotion(key))
					continue;
				at.x += wrapOffset(at.x);
				const anim::Pose pose = ring.player.evaluate(ring.model);
				const auto local = ring.model.worldTransforms(pose.locals);
				// (a little narrower than a plot: its circle is 154 across)
				const Affine place = placement({ at.x, at.y, 1.5f }, 0, S() / 154.0f * 0.95f);
				std::vector<Affine> world(local.size());
				for (size_t k = 0; k < local.size(); ++k)
					world[k] = place * local[k];
				std::vector<scene::Material> materials;
				for (const auto& item : ring.model.items)
				{
					materials.push_back(anim::animatedMaterial(item, pose));
					if (glow)
						for (nif::Color3* c : { &materials.back().diffuse, &materials.back().emissive, &materials.back().ambient })
							c->r *= kEnemyGlow.x, c->g *= kEnemyGlow.y, c->b *= kEnemyGlow.z;
				}
				renderer.draw(*ring.gpu, world, ring.model.visibility(pose.hidden), materials, kDefaultTeamColor, true);
			}
			return true;
		}

		// The plots the game colours in the circle style (PLOT_STYLE_CIRCLE: a worker's or a
		// settler's recommended plots, a rally point): as the original's, the circle cell of
		// Art/Terrain/PlotTextures/plottexture.dds (128 texels a plot; its ring 4 texels wide, 2 in
		// from the plot's edges) laid on the ground in the plot's colour, under what stands on it:
		// tested against the scene's depth and not writing it, so a figure on the plot hides it.
		std::vector<MapView::GroundCircle> groundCircles;
		scene::Model circleModel;
		std::unique_ptr<GpuModel> circleGpu;
		bool circleTried = false;

		void drawGroundCircles(const Frustum& frustum)
		{
			if (groundCircles.empty())
				return;
			if (!circleTried)
			{
				circleTried = true;
				circleModel.nodes.push_back({ "PlotCircle" });
				scene::DrawItem quad;
				quad.node = 0;
				// (the plot, on the terrain's own grid and triangles (kSub a side), so that it lies
				// exactly on the ground between its points too; the cell's picture across it)
				constexpr int n = kSub;
				constexpr float u0 = 640.0f / 1024, v0 = 256.0f / 512, du = 128.0f / 1024, dv = 128.0f / 512;
				for (int j = 0; j <= n; ++j)
					for (int i = 0; i <= n; ++i)
					{
						const float a = float(i) / n, b = float(j) / n;
						quad.vertices.push_back({ { a, b, 0 }, { 0, 0, 1 }, { u0 + a * du, v0 + (1 - b) * dv } });
					}
				for (int j = 0; j < n; ++j)
					for (int i = 0; i < n; ++i)
					{
						const uint32_t k = uint32_t(j * (n + 1) + i);
						quad.indices.insert(quad.indices.end(), { k, k + 1, k + n + 2, k, k + n + 2, k + n + 1 });
					}
				scene::Material& mat = quad.material;
				mat.baseTexture = "art/terrain/plottextures/plottexture.dds";
				mat.baseClamp = scene::TexClamp::ClampClamp;
				mat.lightingMode = 0;
				mat.alphaBlend = true;
				mat.zTest = true;
				// (its depth where it shows: the pieces laid lower on the ground, rivers and roads,
				// blended and drawn after it or before, cannot cover it; what stands on it can)
				mat.zWrite = true;
				mat.alphaTest = true;
				mat.alphaFunc = scene::Compare::Greater;
				mat.alphaRef = 8;
				mat.cull = scene::Cull::None;
				circleModel.items.push_back(std::move(quad));
				circleGpu = renderer.upload(circleModel, assets, AssetLocation{});
				if (!circleGpu)
					problem("art/terrain/plottextures/plottexture.dds: the plot circle");
			}
			if (!circleGpu)
				return;
			std::vector<scene::Material> material(1, circleModel.items[0].material);
			const uint8_t visible[] = { 1 };
			for (const auto& c : groundCircles)
			{
				if (c.x < 0 || c.y < 0 || c.x >= map.width || c.y >= map.height)
					continue;
				Vec3 middle{ (float(c.x) + 0.5f) * S(), (float(c.y) + 0.5f) * S(), 0 };
				middle.z = ground(middle.x, middle.y);
				const float dx = wrapOffset(middle.x);
				if (!frustum.sphere(middle + Vec3{ dx, 0, 0 }, S()))
					continue;
				// (above the pieces laid on the ground, rivers at 0.8 and roads at 0.6: the original
				// draws the circle over their water and banks)
				const Vec3 at{ float(c.x) * S() + dx, float(c.y) * S(), 1.5f };
				material[0].emissive = { c.color.r, c.color.g, c.color.b };
				material[0].alpha = std::clamp(c.alpha, 0.0f, 1.0f);
				const Affine world[] = { placement(at, 0, S()) };
				renderer.draw(*circleGpu, world, visible, material, kDefaultTeamColor, true);
			}
		}
		std::unordered_map<uint64_t, float> restingSince; // when each resting unit began to fortify
		std::unordered_map<uint64_t, float> battleStarts; // by BattleView::serial
		std::unordered_map<uint64_t, std::pair<int, float>> workSince; // each working unit's work, since when
		// A fighting figure's clip and where in it the last frame was: its text keys' sounds
		// sound as the clip passes them.
		std::unordered_map<uint64_t, std::pair<const anim::Sequence*, float>> clipTimes;
		std::function<void(const std::string&, float, float)> onSound;

		void problem(const std::string& what)
		{
			if (reported.insert(what).second)
				problems.push_back(what);
		}

		float S() const { return map.plotSize; }
		float worldW() const { return float(map.width) * S(); }
		float worldH() const { return float(map.height) * S(); }

		void createPipeline()
		{
			const std::string source = std::string(kFrameConstantsHlsl) + kTerrainHlsl;
			terrainVS = renderer.compile(source, "terrain.hlsl", "VSTerrain", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 2);
			terrainPS = renderer.compile(source, "terrain.hlsl", "PSTerrain", SDL_GPU_SHADERSTAGE_FRAGMENT, 7, 0, 2);
			const std::string waterSource = std::string(kFrameConstantsHlsl) + kWaterHlsl;
			waterVS = renderer.compile(waterSource, "water.hlsl", "VSWater", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 2);
			waterPS = renderer.compile(waterSource, "water.hlsl", "PSWater", SDL_GPU_SHADERSTAGE_FRAGMENT, 2, 0, 2);
			borderVS = renderer.compile(source, "terrain.hlsl", "VSBorder", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 2);
			borderPS = renderer.compile(source, "terrain.hlsl", "PSBorder", SDL_GPU_SHADERSTAGE_FRAGMENT, 7, 0, 2);
			groundPipelinesFor(1);
			makeTerrainSamplers();
		}

		// The ground's pipelines (the terrain, the water, the culture borders) for a pass of so many
		// samples (anti-aliasing), made when first needed.
		struct GroundPipelines
		{
			SDL_GPUGraphicsPipeline* terrain = nullptr;
			SDL_GPUGraphicsPipeline* water = nullptr;
			SDL_GPUGraphicsPipeline* border = nullptr;
		};
		std::map<uint32_t, GroundPipelines> groundPipelines;
		const GroundPipelines& groundPipelinesFor(uint32_t samples)
		{
			if (const auto it = groundPipelines.find(samples); it != groundPipelines.end())
				return it->second;
			SDL_GPUDevice* dev = renderer.device();
			GroundPipelines made;
			SDL_GPUGraphicsPipelineCreateInfo ci{};
			ci.vertex_shader = terrainVS;
			ci.fragment_shader = terrainPS;
			const SDL_GPUVertexBufferDescription vbd{ 0, sizeof(TerrainVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 };
			const SDL_GPUVertexAttribute attributes[] = { { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(TerrainVertex, position) },
				{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(TerrainVertex, normal) } };
			ci.vertex_input_state = { &vbd, 1, attributes, 2 };
			ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
			ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
			ci.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_BACK;
			ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE;
			ci.rasterizer_state.enable_depth_clip = true;
			ci.depth_stencil_state.enable_depth_test = ci.depth_stencil_state.enable_depth_write = true;
			ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
			SDL_GPUColorTargetDescription target{};
			target.format = renderer.colorFormat();
			ci.target_info.color_target_descriptions = &target;
			ci.target_info.num_color_targets = 1;
			ci.target_info.depth_stencil_format = renderer.depthFormat();
			ci.target_info.has_depth_stencil_target = true;
			ci.multisample_state.sample_count = Renderer::sampleCount(samples);
			if (!(made.terrain = SDL_CreateGPUGraphicsPipeline(dev, &ci)))
				throw std::runtime_error(std::string("terrain pipeline: ") + SDL_GetError());

			// The water: blended over the ground at its alpha, not writing depth. (Water.fx writes it,
			// but OpenCiv4's river beds dip below the water's level at the coast: the rivers' deltas
			// would end in a hard edge where the sea starts instead of running out into it.) Drawn
			// after the rivers' water (Renderer::drawGroundBlended), so a delta's water out at sea is
			// seen through the sea's.
			ci.vertex_shader = waterVS;
			ci.fragment_shader = waterPS;
			const SDL_GPUVertexBufferDescription wvbd{ 0, sizeof(WaterVertex), SDL_GPU_VERTEXINPUTRATE_VERTEX, 0 };
			const SDL_GPUVertexAttribute wattributes[] = { { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(WaterVertex, position) },
				{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(WaterVertex, depth) },
				{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT, offsetof(WaterVertex, bed) } };
			ci.vertex_input_state = { &wvbd, 1, wattributes, 3 };
			target.blend_state.enable_blend = true;
			target.blend_state.src_color_blendfactor = SDL_GPU_BLENDFACTOR_SRC_ALPHA;
			target.blend_state.dst_color_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			target.blend_state.color_blend_op = SDL_GPU_BLENDOP_ADD;
			target.blend_state.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ZERO;
			target.blend_state.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
			target.blend_state.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
			ci.depth_stencil_state.enable_depth_write = false;
			if (!(made.water = SDL_CreateGPUGraphicsPipeline(dev, &ci)))
				throw std::runtime_error(std::string("water pipeline: ") + SDL_GetError());

			// The culture borders: the ground's mesh again, after the water, blended over what is
			// drawn (as the water: not writing depth).
			ci.vertex_shader = borderVS;
			ci.fragment_shader = borderPS;
			ci.vertex_input_state = { &vbd, 1, attributes, 2 };
			if (!(made.border = SDL_CreateGPUGraphicsPipeline(dev, &ci)))
				throw std::runtime_error(std::string("border pipeline: ") + SDL_GetError());
			return groundPipelines[samples] = made;
		}

		// The ground's samplers, again when the renderer's lowest level changes (low resolution textures).
		uint32_t terrainSamplerGeneration = ~0u;
		void makeTerrainSamplers()
		{
			SDL_GPUDevice* dev = renderer.device();
			for (SDL_GPUSampler* s : { sampler, detailWrap, waterSampler })
				if (s)
					SDL_ReleaseGPUSampler(dev, s);
			SDL_GPUSamplerCreateInfo sd{};
			sd.min_filter = sd.mag_filter = SDL_GPU_FILTER_LINEAR;
			sd.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
			sd.address_mode_u = sd.address_mode_v = sd.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			sd.enable_anisotropy = true;
			sd.max_anisotropy = 8;
			sd.min_lod = renderer.minLod(); // (low resolution textures: from the half size level)
			sd.max_lod = 1000;
			sampler = SDL_CreateGPUSampler(dev, &sd);
			sd.address_mode_u = sd.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
			detailWrap = SDL_CreateGPUSampler(dev, &sd);
			// (Water.fx's sampler: linear, mipmapped, no anisotropy; the texture repeats)
			sd.enable_anisotropy = false;
			sd.max_anisotropy = 1;
			waterSampler = SDL_CreateGPUSampler(dev, &sd);
			if (!sampler || !detailWrap || !waterSampler)
				throw std::runtime_error(std::string("terrain samplers: ") + SDL_GetError());
			terrainSamplerGeneration = renderer.samplerGeneration();
		}

		// ------------------------------------------------------------ art

		// The original draws the grid by swapping each terrain's blend atlas for its grid texture:
		// the same tiles with a line painted through their middles, which is where the plot edges
		// run. Measured from the two (grey, amount, width in plots), so the shader draws that line:
		// the ground under it towards black or white (BtS: land half way to black, 1/128 of a plot
		// wide; coast half way and ocean 30% to white, 2/128 wide).
		std::array<float, 4> measureGridLine(const std::vector<uint8_t>& blend, uint32_t width, uint32_t height, const std::string& path)
		{
			try
			{
				const auto loc = assets.locate(makeAssetKey(path));
				if (!loc)
					throw std::runtime_error("not found");
				const gfx::Image image = gfx::loadDds(assets.read(*loc));
				if (image.width != width || image.height != height)
					throw std::runtime_error("not the size of the terrain's blend texture");
				const std::vector<uint8_t> grid = gfx::decodeBgra(image);
				// The tile columns that differ (summed over the whole texture), then the line's
				// colour against the ground's in them.
				const uint32_t tileSize = width / 4;
				if (tileSize == 0)
					return kDefaultGridLine;
				std::vector<double> diff(tileSize);
				for (uint32_t y = 0; y < height; ++y)
					for (uint32_t x = 0; x < width; ++x)
					{
						const size_t i = (size_t(y) * width + x) * 4;
						for (int c = 0; c < 3; ++c)
							diff[x % tileSize] += std::abs(int(grid[i + c]) - int(blend[i + c]));
					}
				const double perTexel = 3.0 * height * 4;
				double under = 0, line = 0;
				int columns = 0;
				for (uint32_t col = 0; col < tileSize; ++col)
				{
					if (diff[col] / perTexel < 8) // (DXT noise is ~1)
						continue;
					++columns;
					for (uint32_t y = 0; y < height; ++y)
						for (uint32_t x = col; x < width; x += tileSize)
						{
							const size_t i = (size_t(y) * width + x) * 4;
							for (int c = 0; c < 3; ++c)
								under += blend[i + c], line += grid[i + c];
						}
				}
				if (columns == 0 || under <= 0)
					return { 0, 0, 0, 0 }; // no line: the grid texture is the blend texture
				const float n = float(columns) * height * 4 * 3;
				const float u = float(under) / n / 255, l = float(line) / n / 255;
				const float grey = l < u ? 0.0f : 1.0f;
				const float amount = std::clamp(grey == 0 ? 1 - l / u : (l - u) / std::max(1 - u, 1e-3f), 0.0f, 1.0f);
				return { grey, amount, float(columns) / float(tileSize), 0 };
			}
			catch (const std::exception& e)
			{
				problem(std::format("terrain grid texture {}: {}", path, e.what()));
				return kDefaultGridLine;
			}
		}

		// Every terrain's blend atlas as 32 tiles of kTile pixels in one texture array, so that
		// tiles filter and mip without bleeding into each other.
		void buildAtlas()
		{
			const size_t count = map.terrains.size();
			atlasTex.reset();
			if (count == 0)
				return;
			std::vector<std::vector<uint8_t>> levels(count * kSlots * kTileMips);
			gridLines.assign(count, kDefaultGridLine);
			for (size_t t = 0; t < count; ++t)
			{
				std::vector<uint8_t> pixels;
				uint32_t width = 0, height = 0;
				const std::string& path = map.terrains[t].blend;
				try
				{
					const auto loc = assets.locate(makeAssetKey(path));
					if (!loc)
						throw std::runtime_error("not found");
					const gfx::Image image = gfx::loadDds(assets.read(*loc));
					pixels = gfx::decodeBgra(image);
					width = image.width;
					height = image.height;
				}
				catch (const std::exception& e)
				{
					if (!path.empty())
						problem(std::format("terrain texture {}: {}", path, e.what()));
				}
				if (!pixels.empty() && !map.terrains[t].grid.empty())
					gridLines[t] = measureGridLine(pixels, width, height, map.terrains[t].grid);
				const uint32_t tileSize = width / 4;
				const uint32_t tiles = tileSize ? 4 * (height / tileSize) : 0;
				for (uint32_t s = 0; s < kSlots; ++s)
				{
					std::vector<uint8_t> tile(kTile * kTile * 4, 128);
					if (tiles > 0)
					{
						const uint32_t src = s < tiles ? s : tiles - 1;
						const uint32_t ox = (src % 4) * tileSize, oy = (src / 4) * tileSize;
						// Box filter down (or nearest up) to kTile.
						const uint32_t step = std::max(1u, tileSize / kTile);
						for (uint32_t y = 0; y < kTile; ++y)
							for (uint32_t x = 0; x < kTile; ++x)
							{
								uint32_t sum[4]{};
								const uint32_t sx = ox + x * tileSize / kTile, sy = oy + y * tileSize / kTile;
								for (uint32_t j = 0; j < step; ++j)
									for (uint32_t i = 0; i < step; ++i)
									{
										const uint8_t* p = &pixels[(size_t(sy + j) * width + sx + i) * 4];
										for (int c = 0; c < 4; ++c)
											sum[c] += p[c];
									}
								for (int c = 0; c < 4; ++c)
									tile[(size_t(y) * kTile + x) * 4 + c] = uint8_t(sum[c] / (step * step));
							}
					}
					else
						for (size_t i = 3; i < tile.size(); i += 4)
							tile[i] = 255;
					const size_t base = (t * kSlots + s) * kTileMips;
					levels[base] = std::move(tile);
					for (uint32_t mip = 1, size = kTile / 2; mip < kTileMips; ++mip, size /= 2)
					{
						const auto& up = levels[base + mip - 1];
						std::vector<uint8_t> down(size_t(size) * size * 4);
						for (uint32_t y = 0; y < size; ++y)
							for (uint32_t x = 0; x < size; ++x)
								for (int c = 0; c < 4; ++c)
								{
									const size_t u = size * 2;
									const uint32_t v = up[((y * 2) * u + x * 2) * 4 + c] + up[((y * 2) * u + x * 2 + 1) * 4 + c] +
										up[((y * 2 + 1) * u + x * 2) * 4 + c] + up[((y * 2 + 1) * u + x * 2 + 1) * 4 + c];
									down[(size_t(y) * size + x) * 4 + c] = uint8_t((v + 2) / 4);
								}
						levels[base + mip] = std::move(down);
					}
				}
			}
			terrainColors.assign(count, 0xFF808080);
			for (size_t t = 0; t < count; ++t)
			{
				const auto& plain = map.terrains[t].slots[15];
				const size_t slot = plain.empty() ? 0 : size_t(std::clamp(plain.front().first - 1, 0, int(kSlots) - 1));
				const auto& px = levels[(t * kSlots + slot) * kTileMips + kTileMips - 1];
				std::memcpy(&terrainColors[t], px.data(), 4);
			}
			const std::vector<std::span<const uint8_t>> subresources(levels.begin(), levels.end());
			atlasTex = renderer.createTexture(kTile, kTile, uint32_t(count * kSlots), kTileMips, TextureFormat::BGRA8, subresources);
			buildDetails();

		}

		// The terrains' detail textures, one array slice of 256 pixels each (grey without one).
		// The water plane's texture (CIV4WaterPlaneInfos), with its mips: far away the sparkles in
		// its alpha average out, as the original's do.
		void buildWaterArt()
		{
			waterTex.reset();
			const auto loc = map.water.texture.empty() ? std::nullopt : assets.locate(makeAssetKey(map.water.texture));
			if (!loc)
			{
				if (!map.water.texture.empty())
					problem(std::format("{}: not found", map.water.texture));
				return;
			}
			try
			{
				const gfx::Image image = gfx::loadDds(assets.read(*loc));
				uint32_t w = image.width, h = image.height;
				std::vector<std::vector<uint8_t>> levels{ gfx::decodeBgra(image) };
				while (w > 1 || h > 1)
				{
					const uint32_t nw = std::max(w / 2, 1u), nh = std::max(h / 2, 1u);
					const auto& up = levels.back();
					std::vector<uint8_t> down(size_t(nw) * nh * 4);
					for (uint32_t y = 0; y < nh; ++y)
						for (uint32_t x = 0; x < nw; ++x)
							for (int c = 0; c < 4; ++c)
							{
								const uint32_t x0 = std::min(x * 2, w - 1), x1 = std::min(x * 2 + 1, w - 1);
								const uint32_t y0 = std::min(y * 2, h - 1), y1 = std::min(y * 2 + 1, h - 1);
								down[(size_t(y) * nw + x) * 4 + c] = uint8_t((up[(size_t(y0) * w + x0) * 4 + c] + up[(size_t(y0) * w + x1) * 4 + c] +
									up[(size_t(y1) * w + x0) * 4 + c] + up[(size_t(y1) * w + x1) * 4 + c] + 2) / 4);
							}
					levels.push_back(std::move(down));
					w = nw;
					h = nh;
				}
				const std::vector<std::span<const uint8_t>> subresources(levels.begin(), levels.end());
				waterTex = renderer.createTexture(image.width, image.height, 1, uint32_t(levels.size()), TextureFormat::BGRA8, subresources);
			}
			catch (const std::exception& e)
			{
				problem(std::format("{}: {}", map.water.texture, e.what()));
			}
		}

		void buildDetails()
		{
			detailTex.reset();
			constexpr uint32_t kSize = 256, kMips = 9;
			std::vector<std::vector<uint8_t>> levels;
			for (const TerrainArt& art : map.terrains)
			{
				std::vector<uint8_t> top(kSize * kSize * 4, 128);
				try
				{
					const auto loc = art.detail.empty() ? std::nullopt : assets.locate(makeAssetKey(art.detail));
					if (loc)
					{
						const gfx::Image image = gfx::loadDds(assets.read(*loc));
						const auto pixels = gfx::decodeBgra(image);
						for (uint32_t y = 0; y < kSize; ++y)
							for (uint32_t x = 0; x < kSize; ++x)
								std::memcpy(&top[(size_t(y) * kSize + x) * 4], &pixels[(size_t(y * image.height / kSize) * image.width + x * image.width / kSize) * 4], 4);
					}
				}
				catch (const std::exception& e)
				{
					problem(std::format("{}: {}", art.detail, e.what()));
				}
				levels.push_back(std::move(top));
				for (uint32_t mip = 1, size = kSize / 2; mip < kMips; ++mip, size /= 2)
				{
					const auto& up = levels.back();
					std::vector<uint8_t> down(size_t(size) * size * 4);
					for (uint32_t y = 0; y < size; ++y)
						for (uint32_t x = 0; x < size; ++x)
							for (int c = 0; c < 4; ++c)
							{
								const size_t u = size * 2;
								down[(size_t(y) * size + x) * 4 + c] = uint8_t((up[((y * 2) * u + x * 2) * 4 + c] + up[((y * 2) * u + x * 2 + 1) * 4 + c] +
									up[((y * 2 + 1) * u + x * 2) * 4 + c] + up[((y * 2 + 1) * u + x * 2 + 1) * 4 + c] + 2) / 4);
							}
					levels.push_back(std::move(down));
				}
			}
			if (levels.empty())
				return;
			const std::vector<std::span<const uint8_t>> subresources(levels.begin(), levels.end());
			detailTex = renderer.createTexture(kSize, kSize, uint32_t(map.terrains.size()), kMips, TextureFormat::BGRA8, subresources);
		}

		// Features whose art is a texture (flood plains) are painted on the ground: one array
		// slice of 256 pixels per texture.
		void buildDecals()
		{
			decalTex.reset();
			featureDecal.assign(map.features.size(), 0);
			std::vector<std::string> textures;
			for (size_t f = 0; f < map.features.size(); ++f)
				for (const auto& variety : map.features[f].varieties)
					for (const auto& piece : variety)
						for (const auto& [file, rotation] : piece.models)
							if (toLowerAscii(file).ends_with(".dds") && !featureDecal[f])
							{
								const auto it = std::ranges::find(textures, makeAssetKey(file));
								featureDecal[f] = uint16_t((it - textures.begin()) + 1);
								if (it == textures.end())
									textures.push_back(makeAssetKey(file));
							}
			constexpr uint32_t kSize = 256, kMips = 9;
			std::vector<std::vector<uint8_t>> levels;
			for (const auto& key : textures)
			{
				std::vector<uint8_t> top(kSize * kSize * 4, 0);
				try
				{
					const auto loc = assets.locate(key);
					if (!loc)
						throw std::runtime_error("not found");
					const gfx::Image image = gfx::loadDds(assets.read(*loc));
					const auto pixels = gfx::decodeBgra(image);
					for (uint32_t y = 0; y < kSize; ++y)
						for (uint32_t x = 0; x < kSize; ++x)
							std::memcpy(&top[(size_t(y) * kSize + x) * 4], &pixels[(size_t(y * image.height / kSize) * image.width + x * image.width / kSize) * 4], 4);
					// The colour is drawn without the alpha (that only rags the edge): where a mod's
					// texture is black under its clear parts (AND's flood plains), its mean colour.
					uint64_t sum[3] = {}, opaque = 0;
					for (size_t i = 0; i < top.size(); i += 4)
						if (top[i + 3] >= 200)
						{
							for (int c = 0; c < 3; ++c)
								sum[c] += top[i + c];
							++opaque;
						}
					if (opaque)
						for (size_t i = 0; i < top.size(); i += 4)
							for (int c = 0; c < 3; ++c)
								top[i + c] = uint8_t((top[i + c] * top[i + 3] + sum[c] / opaque * (255 - top[i + 3])) / 255);
				}
				catch (const std::exception& e)
				{
					problem(std::format("{}: {}", key, e.what()));
				}
				levels.push_back(std::move(top));
				for (uint32_t mip = 1, size = kSize / 2; mip < kMips; ++mip, size /= 2)
				{
					const auto& up = levels.back();
					std::vector<uint8_t> down(size_t(size) * size * 4);
					for (uint32_t y = 0; y < size; ++y)
						for (uint32_t x = 0; x < size; ++x)
							for (int c = 0; c < 4; ++c)
							{
								const size_t u = size * 2;
								down[(size_t(y) * size + x) * 4 + c] = uint8_t((up[((y * 2) * u + x * 2) * 4 + c] + up[((y * 2) * u + x * 2 + 1) * 4 + c] +
									up[((y * 2 + 1) * u + x * 2) * 4 + c] + up[((y * 2 + 1) * u + x * 2 + 1) * 4 + c] + 2) / 4);
							}
					levels.push_back(std::move(down));
				}
			}
			// (two slices at least: the shader samples an array, and one slice would make a plain texture)
			const size_t slices = std::max<size_t>(textures.size(), 2);
			while (levels.size() < slices * kMips)
				levels.emplace_back(size_t(kSize >> (levels.size() % kMips)) * (kSize >> (levels.size() % kMips)) * 4, 0);
			const std::vector<std::span<const uint8_t>> subresources(levels.begin(), levels.end());
			decalTex = renderer.createTexture(kSize, kSize, uint32_t(slices), kMips, TextureFormat::BGRA8, subresources);
		}

		void findWater(const nif::File& file, LoadedModel& lm, const AssetLocation& loc)
		{
			std::string name;
			for (size_t b = 0; b < file.blocks.size() && name.empty(); ++b)
				if (const auto* geom = file.get<nif::NiGeometry>(nif::Ref(b)))
					for (const nif::Ref e : geom->extraData)
						if (const auto* text = file.get<nif::NiStringExtraData>(e); text && toLowerAscii(text->value).contains("riverbase"))
							name = geom->name;
			const auto item = std::ranges::find_if(lm.model.items, [&](const scene::DrawItem& i) { return !name.empty() && i.nodeName == name; });
			if (item == lm.model.items.end() || item->material.baseTexture.empty())
				return;
			const auto texture = assets.locateTexture(loc, item->material.baseTexture);
			if (!texture)
				return;
			LoadedModel::Water w;
			auto& cached = waterAlpha[texture->key];
			if (!cached.alpha)
				try
				{
					const gfx::Image image = gfx::loadTexture(assets.read(*texture));
					const auto pixels = gfx::decodeBgra(image);
					auto alpha = std::make_shared<std::vector<uint8_t>>(size_t(image.width) * image.height);
					// (water: blue where it is drawn; the rest of what it draws is the river's bed of
					// sand, which a sandy ground shows to the edge of the grid)
					for (size_t i = 0; i < alpha->size(); ++i)
						(*alpha)[i] = pixels[i * 4 + 3] < 128 ? 0 : int(pixels[i * 4]) > int(pixels[i * 4 + 2]) + 16 ? 2 : 1;
					cached = { alpha, image.width, image.height };
				}
				catch (const std::exception& e)
				{
					problem(std::format("{}: {}", texture->key, e.what()));
					return;
				}
			w.alpha = cached.alpha;
			w.width = cached.width;
			w.height = cached.height;
			const scene::Model& m = lm.model;
			const auto world = m.worldTransforms(m.bindLocals());
			const Affine& at = item->node >= 0 && size_t(item->node) < world.size() ? world[size_t(item->node)] : Affine{};
			for (const uint32_t i : item->indices)
			{
				const scene::Vertex& v = item->vertices[i];
				w.corners.push_back(at.point(v.position));
				w.uvs.push_back({ v.uv.x, v.uv.y });
			}
			lm.water = std::move(w);
		}

		LoadedModel* model(const std::string& nif, const std::string& kfm, const ButtonArt::Icon* flag = nullptr, bool medallions = false)
		{
			if (nif.empty())
				return nullptr;
			const std::string key = makeAssetKey(nif);
			auto& slot = models[key + '|' + makeAssetKey(kfm) + (flag ? '|' + std::to_string(flag->key) : "") + (medallions ? "|m" : "")];
			if (slot)
				return slot->ok ? slot.get() : nullptr;
			slot = std::make_unique<LoadedModel>();
			LoadedModel& lm = *slot;
			try
			{
				const auto loc = assets.locate(key);
				if (!loc)
					throw std::runtime_error("not found");
				const auto file = std::make_shared<nif::File>(nif::parseFile(assets.read(*loc)));
				lm.model = scene::buildModel(*file);
				// (the engine's user properties: "ATTACH:<node>:<type>" lines, the effects on nodes)
				for (const auto& block : file->blocks)
					if (const auto* extra = dynamic_cast<const nif::NiStringExtraData*>(block.get()))
						for (size_t at = extra->value.find("ATTACH:"); at != std::string::npos; at = extra->value.find("ATTACH:", at + 7))
						{
							const size_t colon = extra->value.find(':', at + 7);
							if (colon == std::string::npos)
								break;
							size_t end = extra->value.find_first_of("\r\n \t", colon + 1);
							if (end == std::string::npos)
								end = extra->value.size();
							const std::string node = toLowerAscii(extra->value.substr(at + 7, colon - at - 7));
							const std::string type = extra->value.substr(colon + 1, end - colon - 1);
							for (size_t n = 0; n < lm.model.nodes.size(); ++n)
								if (toLowerAscii(lm.model.nodes[n].name) == node)
								{
									lm.attachTags.push_back({ int(n), type });
									break;
								}
						}
				if (anim::Sequence own = anim::Sequence::embedded(file, lm.model); own.channelCount() > 0)
				{
					lm.embeddedMoves = own.transformChannels() + own.visibilityChannels() > 0;
					lm.embedded = std::move(own);
				}
				// "TreeCut" spheres (resources, improvements, rivers) only tell the engine where
				// to clear trees: hidden, and remembered for buildFeatures.
				for (auto& node : lm.model.nodes)
				{
					const std::string name = toLowerAscii(node.name);
					if (name.starts_with("treecut") || name.starts_with("cuttree"))
						node.hidden = true;
				}
				findWater(*file, lm, *loc);
				if (flag)
				{
					lm.flagDecal = renderer.createMipmapped(flag->width, flag->height, flag->bgra);
				}
				lm.gpu = renderer.upload(lm.model, assets, *loc, lm.flagDecal.get());
				for (const auto& missing : lm.gpu->stats().missing)
					problem(std::format("{}: texture {}", nif, missing));
				lm.bindLocals = lm.model.bindLocals();
				lm.bindWorld = lm.model.worldTransforms(lm.bindLocals);
				lm.bindVisible = lm.model.visibility(lm.model.bindHidden());
				for (const auto& item : lm.model.items)
					if (item.contour && item.node >= 0)
					{
						lm.contourNodes.resize(lm.model.nodes.size(), 0);
						lm.contourNodes[size_t(item.node)] = 1;
					}
				lm.bounds = lm.model.bounds;
				for (size_t i = 0; i < lm.model.nodes.size(); ++i)
				{
					const std::string name = toLowerAscii(lm.model.nodes[i].name);
					if (!name.starts_with("treecut") && !name.starts_with("cuttree"))
						continue;
					Bounds b;
					for (const auto& item : lm.model.items)
						for (int n = item.node; n >= 0; n = lm.model.nodes[size_t(n)].parent)
							if (n == int(i))
							{
								for (const auto& v : item.vertices)
									b.add(lm.bindWorld[size_t(item.node)].point(v.position));
								break;
							}
					const Vec3 c = lm.bindWorld[i].translation;
					lm.treeCuts.push_back({ c, b.empty() ? 10.0f : std::max(b.max.x - b.min.x, b.max.y - b.min.y) / 2 });
				}
				// The model's pieces (trees, houses) hang below the first node with more than one
				// child: wrappers above it ("Scene Root" in another root) do not count.
				const auto& nodes = lm.model.nodes;
				std::vector<int> children(nodes.size(), 0);
				for (const auto& n : nodes)
					if (n.parent >= 0)
						++children[size_t(n.parent)];
				int spread = nodes.empty() ? -1 : 0;
				while (spread >= 0 && children[size_t(spread)] == 1)
				{
					const auto next = std::find_if(nodes.begin(), nodes.end(), [&](const scene::Node& n) { return n.parent == spread; });
					spread = int(next - nodes.begin());
				}
				lm.spread = spread;
				lm.top.resize(nodes.size());
				for (size_t i = 0; i < nodes.size(); ++i)
				{
					const int parent = nodes[i].parent;
					lm.top[i] = parent < 0 || int(i) <= spread || parent == spread ? int(i) : lm.top[size_t(parent)];
				}
				loadIdle(lm, *loc, kfm, flag ? 3276800u : 1000u, medallions);
				lm.ok = true;
			}
			catch (const std::exception& e)
			{
				problem(std::format("{}: {}", nif, e.what()));
			}
			return lm.ok ? &lm : nullptr;
		}

		// The idle animation (event 1000) from the art's KFM, or the one next to the model.
		void loadIdle(LoadedModel& lm, const AssetLocation& nifLocation, const std::string& kfm, uint32_t idleCode = 1000u, bool medallions = false)
		{
			std::string kfmKey = makeAssetKey(kfm);
			if (kfmKey.empty())
				kfmKey = nifLocation.key.substr(0, nifLocation.key.rfind('.')) + ".kfm";
			const auto loc = assets.locate(kfmKey);
			if (!loc)
				return;
			try
			{
				const nif::KfmFile file = nif::parseKfm(assets.read(*loc));
				std::vector<std::pair<uint32_t, std::optional<anim::Sequence>*>> wanted{ { idleCode, &lm.idle }, { 1010u, &lm.run }, { 1020u, &lm.fortify[0] },
					{ 1021u, &lm.fortified[0] }, { 1030u, &lm.fortify[1] }, { 1031u, &lm.fortified[1] } };
				for (uint32_t set = 0; set < 3; ++set)
					for (uint32_t kind = 0; kind < 2; ++kind)
					{
						const uint32_t base = 1000 * (set + 1);
						wanted.emplace_back(base + (kind ? 32 : 22), &lm.strike[set][kind]);
						wanted.emplace_back(base + (kind ? 33 : 24), &lm.die[set][kind]);
						wanted.emplace_back(base + (kind ? 34 : 25), &lm.dieFade[set][kind]);
						if (!kind)
							wanted.emplace_back(base + 23, &lm.hurt[set][kind]);
					}
				for (uint32_t w = 0; w < 6; ++w)
					for (uint32_t step = 0; step < 3; ++step)
						wanted.emplace_back(1050 + 3 * w + step, &lm.work[w][step]);
				wanted.emplace_back(1040u, &lm.found);
				for (uint32_t state = 0; state < 3; ++state)
					wanted.emplace_back(state + 1, &lm.states[state]);
				wanted.emplace_back(1004u, &lm.heal);
				std::vector<std::optional<anim::Sequence>> medallionStates(medallions ? 70 : 0);
				for (uint32_t code = 10; code < 10 + medallionStates.size(); ++code)
					if (code % 10 < 5)
						wanted.emplace_back(code, &medallionStates[code - 10]);
				wanted.emplace_back(1002u, &lm.surrender);
				wanted.emplace_back(1u, &lm.fadeOut);
				wanted.emplace_back(1042u, &lm.antiAir);
				std::vector<std::optional<anim::Sequence>> damaged(16);
				for (uint32_t state = 0; state < 4; ++state)
					for (uint32_t part = 0; part < 4; ++part)
						wanted.emplace_back(1000 * (state + 1) + 500 + 10 * part, &damaged[state * 4 + part]);
				for (const auto& [code, target] : wanted)
				{
					// (ships' and others' codes have more in their high bits: the code is the low 16)
					const nif::KfmAnimation* a = file.find(code);
					if (!a)
						for (const auto& any : file.animations)
							if ((any.eventCode & 0xFFFF) == code)
							{
								a = &any;
								break;
							}
					if (!a)
						continue;
					const std::string kfKey = makeAssetKey(folderOf(loc->key) + a->kfFile);
					auto& kf = kfCache[kfKey];
					if (!kf)
						if (const auto kfLoc = assets.locate(kfKey))
							kf = std::make_shared<nif::File>(nif::parseFile(assets.read(*kfLoc)));
					if (!kf)
						continue;
					anim::Sequence seq = anim::Sequence::bind(kf, a->sequenceIndex, lm.model);
					if (seq.channelCount() > 0)
						*target = std::move(seq);
				}
				for (size_t k = 0; k < damaged.size(); ++k)
					if (damaged[k])
						lm.damage[k / 4].push_back(std::move(*damaged[k]));
				for (size_t k = 0; k < medallionStates.size(); ++k)
					if (medallionStates[k])
						lm.medallions.emplace(uint32_t(k + 10), std::move(*medallionStates[k]));
			}
			catch (const std::exception& e)
			{
				problem(std::format("{}: {}", kfm.empty() ? kfmKey : kfm, e.what()));
			}
		}

		void buildParts(LoadedModel& lm)
		{
			if (!lm.parts.empty())
				return;
			const auto& nodes = lm.model.nodes;
			for (size_t i = 0; i < nodes.size(); ++i)
				if (nodes[i].parent >= 0 && nodes[i].parent == lm.spread)
				{
					LoadedModel::Part part;
					part.node = int(i);
					part.visible.assign(nodes.size(), 0);
					for (size_t n = 0; n < nodes.size(); ++n)
						part.visible[n] = (lm.top[n] == int(i) || int(n) <= lm.spread) && lm.bindVisible[n];
					for (const auto& item : lm.model.items)
						if (item.node >= 0 && lm.top[size_t(item.node)] == int(i))
							for (const auto& v : item.vertices)
								part.bounds.add(lm.bindWorld[size_t(item.node)].point(v.position));
					if (!part.bounds.empty())
						lm.parts.push_back(std::move(part));
				}
		}

		// ------------------------------------------------------------ terrain

		float baseLevel(int x, int y) const
		{
			const PlotView* p = map.plot(x, y);
			return p && p->water ? -0.05f * S() : 0.012f * S();
		}

		// Height of the ground in world units: land and water levels blended between plot
		// centres, hills and peaks as lumps around their plot.
		float terrainHeight(float wx, float wy) const
		{
			const float px = wx / S(), py = wy / S();
			const float cx = px - 0.5f, cy = py - 0.5f;
			const int ix = int(std::floor(cx)), iy = int(std::floor(cy));
			const float fx = smoother(cx - float(ix)), fy = smoother(cy - float(iy));
			const float b0 = baseLevel(ix, iy) + (baseLevel(ix + 1, iy) - baseLevel(ix, iy)) * fx;
			const float b1 = baseLevel(ix, iy + 1) + (baseLevel(ix + 1, iy + 1) - baseLevel(ix, iy + 1)) * fx;
			float h = b0 + (b1 - b0) * fy;

			const int ox = int(std::floor(px)), oy = int(std::floor(py));
			const float hill = map.hillScale * S(), peak = map.peakScale * S();
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					const PlotView* q = map.plot(ox + dx, oy + dy);
					if (!q || q->water || !(q->hills || q->peak))
						continue;
					const int hx = map.wrapX ? ((ox + dx) % map.width + map.width) % map.width : ox + dx;
					const int hy = map.wrapY ? ((oy + dy) % map.height + map.height) % map.height : oy + dy;
					const float qx = float(ox + dx) + 0.5f, qy = float(oy + dy) + 0.5f;
					for (int k = 0; k < 3; ++k)
					{
						const float jx = (unitFloat(hash(hx, hy, k)) - 0.5f) * (q->peak ? 0.2f : 0.4f);
						const float jy = (unitFloat(hash(hx, hy, k + 11)) - 0.5f) * (q->peak ? 0.2f : 0.4f);
						const float d = std::hypot(px - qx - jx, py - qy - jy);
						if (q->peak)
							h += peak * (k == 0 ? 1.0f : 0.3f) * std::pow(bump(d / (k == 0 ? 0.6f : 0.4f)), 1.4f);
						else
							h += hill * (k == 0 ? 0.75f : 0.45f) * bump(d / (k == 0 ? 0.48f : 0.33f));
					}
				}
			// River beds along the plot edges with river, as the original carves them.
			float bed = 0;
			const auto segment = [&](float ax, float ay, float bx, float by) {
				const float vx = bx - ax, vy = by - ay;
				const float t = std::clamp(((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy), 0.0f, 1.0f);
				const float d = std::hypot(px - ax - vx * t, py - ay - vy * t);
				bed = std::max(bed, 1 - smoother(d / 0.22f));
			};
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
					if (const PlotView* q = map.plot(ox + dx, oy + dy))
					{
						const float qx = float(ox + dx), qy = float(oy + dy);
						if (q->riverSouth)
							segment(qx, qy, qx + 1, qy);
						if (q->riverEast)
							segment(qx + 1, qy, qx + 1, qy + 1);
					}
			return h - bed * 0.025f * S();
		}

		void buildTerrain()
		{
			const int W = map.width, H = map.height;
			gridW = W * kSub + 1;
			gridH = H * kSub + 1;
			const float step = S() / kSub;
			heights.assign(size_t(gridW) * gridH, 0);
			for (int j = 0; j < gridH; ++j)
				for (int i = 0; i < gridW; ++i)
					heights[size_t(j) * gridW + i] = terrainHeight(float(i) * step, float(j) * step);

			std::vector<TerrainVertex> vertices(heights.size());
			const auto h = [&](int i, int j) {
				if (map.wrapX)
					i = (i % (gridW - 1) + (gridW - 1)) % (gridW - 1);
				else
					i = std::clamp(i, 0, gridW - 1);
				j = map.wrapY ? (j % (gridH - 1) + (gridH - 1)) % (gridH - 1) : std::clamp(j, 0, gridH - 1);
				return heights[size_t(j) * gridW + i];
			};
			for (int j = 0; j < gridH; ++j)
				for (int i = 0; i < gridW; ++i)
				{
					TerrainVertex& v = vertices[size_t(j) * gridW + i];
					v.position = { float(i) * step, float(j) * step, h(i, j) };
					v.normal = normalize(Vec3{ (h(i - 1, j) - h(i + 1, j)) / (2 * step), (h(i, j - 1) - h(i, j + 1)) / (2 * step), 1 });
				}
			std::vector<uint32_t> indices;
			indices.reserve(size_t(gridW - 1) * (gridH - 1) * 6);
			for (int j = 0; j + 1 < gridH; ++j)
				for (int i = 0; i + 1 < gridW; ++i)
				{
					const uint32_t a = uint32_t(j * gridW + i), b = a + 1, c = a + 1 + uint32_t(gridW), d = a + uint32_t(gridW);
					indices.insert(indices.end(), { a, b, c, a, c, d });
				}
			indexCount = uint32_t(indices.size());

			// The heights again as a texture: models laid on the ground (roads) follow them.
			const std::span<const uint8_t> heightBytes(reinterpret_cast<const uint8_t*>(heights.data()), heights.size() * 4);
			groundTex = renderer.createTexture(uint32_t(gridW), uint32_t(gridH), 1, 1, TextureFormat::R32Float, std::span(&heightBytes, 1));
			renderer.setGround(groundTex.get(), step, uint32_t(gridW), uint32_t(gridH), map.wrapX ? worldW() : 0);
			vb = renderer.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(), uint32_t(vertices.size() * sizeof(TerrainVertex)));
			ib = renderer.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(), uint32_t(indices.size() * 4));
		}

		// The water plane: a flat grid at the water's level over every tile (between four plot
		// centres) with water at a corner, kSub quads a plot side as the ground (the globe view
		// bends it); each vertex knows how deep the ground under it is, for the fade at the shore.
		void buildWater()
		{
			waterVb.reset();
			waterIb.reset();
			waterIndexCount = 0;
			const int W = map.width, H = map.height;
			const float step = S() / kSub;
			const auto water = [&](int x, int y) {
				const PlotView* p = map.plot(x, y);
				return p && p->water;
			};
			std::vector<WaterVertex> vertices;
			std::vector<uint32_t> indices;
			// (tiles from the south-west corner's plot centre: -1 .. W-1, the map's edge included)
			for (int ty = -1; ty < H; ++ty)
				for (int tx = -1; tx < W; ++tx)
				{
					if (!(water(tx, ty) || water(tx + 1, ty) || water(tx, ty + 1) || water(tx + 1, ty + 1)))
						continue;
					const float x0 = std::max((float(tx) + 0.5f) * S(), 0.0f), x1 = std::min((float(tx) + 1.5f) * S(), worldW());
					const float y0 = std::max((float(ty) + 0.5f) * S(), 0.0f), y1 = std::min((float(ty) + 1.5f) * S(), worldH());
					const int nx = std::max(1, int(std::lround((x1 - x0) / step))), ny = std::max(1, int(std::lround((y1 - y0) / step)));
					const uint32_t first = uint32_t(vertices.size());
					for (int j = 0; j <= ny; ++j)
						for (int i = 0; i <= nx; ++i)
						{
							const float x = x0 + (x1 - x0) * float(i) / float(nx), y = y0 + (y1 - y0) * float(j) / float(ny);
							const float level = levelHeight(x, y);
							vertices.push_back({ { x, y, kWaterLevel }, kWaterLevel - level, std::max(level - terrainHeight(x, y), 0.0f) });
						}
					for (int j = 0; j < ny; ++j)
						for (int i = 0; i < nx; ++i)
						{
							const uint32_t a = first + uint32_t(j * (nx + 1) + i), b = a + 1, c = b + uint32_t(nx + 1), d = a + uint32_t(nx + 1);
							indices.insert(indices.end(), { a, b, c, a, c, d });
						}
				}
			if (indices.empty())
				return;
			waterIndexCount = uint32_t(indices.size());
			waterVb = renderer.createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, vertices.data(), uint32_t(vertices.size() * sizeof(WaterVertex)));
			waterIb = renderer.createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, indices.data(), uint32_t(indices.size() * 4));
		}

		static constexpr float kWaterLevel = 0; // world units: the ground's level between land (above) and water plots

		// The land and water levels alone, blended between plot centres as the ground's are: how
		// deep the sea is from the shore out (hills, river beds and the like left out: the ground
		// there hides the water or is not sea).
		float levelHeight(float wx, float wy) const
		{
			const float cx = wx / S() - 0.5f, cy = wy / S() - 0.5f;
			const int ix = int(std::floor(cx)), iy = int(std::floor(cy));
			const float fx = smoother(cx - float(ix)), fy = smoother(cy - float(iy));
			const float b0 = baseLevel(ix, iy) + (baseLevel(ix + 1, iy) - baseLevel(ix, iy)) * fx;
			const float b1 = baseLevel(ix, iy + 1) + (baseLevel(ix + 1, iy + 1) - baseLevel(ix, iy + 1)) * fx;
			return b0 + (b1 - b0) * fy;
		}

		// How far the minimap is moved: by none once the team's map is centred, or with the fog of
		// war off; until then, so that what the team has seen is in its middle (on an axis the map
		// wraps on, the seen part as the shortest stretch around it).
		std::pair<int, int> shift() const
		{
			if (map.minimapCentered || !fog || map.width <= 0 || map.height <= 0)
				return { 0, 0 };
			const int W = map.width, H = map.height;
			std::vector<uint8_t> cols(size_t(W), 0), rows(size_t(H), 0);
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
					if (map.plots[size_t(x) + size_t(y) * W].revealed)
						cols[size_t(x)] = rows[size_t(y)] = 1;
			const auto along = [](const std::vector<uint8_t>& seen, bool wraps) {
				const int n = int(seen.size());
				int first = -1, last = -1;
				for (int i = 0; i < n; ++i)
					if (seen[size_t(i)])
						last = i, first = first < 0 ? i : first;
				if (first < 0)
					return 0;
				int start = first, length = last - first + 1;
				if (wraps)
				{
					// (the longest run not seen, around the end too: the seen part is the rest)
					int bestGap = 0, gapEnd = -1;
					for (int i = 0, run = 0; i < 2 * n; ++i)
					{
						run = seen[size_t(i % n)] ? 0 : run + 1;
						if (run > bestGap && run <= n)
							bestGap = run, gapEnd = i % n;
					}
					if (bestGap > 0 && bestGap < n)
						start = (gapEnd + 1) % n, length = n - bestGap;
				}
				const int middle = start + length / 2;
				return wraps ? ((middle - n / 2) % n + n) % n : middle - n / 2;
			};
			return { along(cols, map.wrapX), along(rows, map.wrapY) };
		}

		std::optional<std::array<int, 4>> computeSeen() const
		{
			if (map.minimapCentered || !fog || map.width <= 0 || map.height <= 0)
				return std::nullopt;
			std::array<int, 4> b{ map.width, map.height, -1, -1 };
			for (int y = 0; y < map.height; ++y)
				for (int x = 0; x < map.width; ++x)
					if (map.plots[size_t(x) + size_t(y) * size_t(map.width)].revealed)
						b = { std::min(b[0], x), std::min(b[1], y), std::max(b[2], x), std::max(b[3], y) };
			if (b[2] < 0)
				return std::nullopt;
			if (map.wrapX)
				b[0] = 0, b[2] = map.width - 1;
			if (map.wrapY)
				b[1] = 0, b[3] = map.height - 1;
			return b;
		}

		// (the camera's leash, worked out with the minimap: when the plots change)
		std::optional<std::array<int, 4>> seen;

		void updateMinimap()
		{
			seen = computeSeen();
			paintMinimap(minimapPixels, 0.45f, 0.25f, true);
			++minimapVersion;
		}

		// The map at a pixel per plot, north up (BGRA8): the terrain, the owners' colours over
		// it (that much on land, on water), the cities white; black what the active team has
		// not seen, with the fog of war on.
		void paintMinimap(std::vector<uint8_t>& pixels, float ownedLand, float ownedWater, bool cities, bool wholeMap = false) const
		{
			const bool fog = this->fog && !wholeMap;
			const int W = map.width, H = map.height;
			std::vector<uint8_t>& minimapPixels = pixels;
			minimapPixels.assign(size_t(W) * H * 4, 0);
			for (size_t i = 3; i < minimapPixels.size(); i += 4)
				minimapPixels[i] = 255; // (black where it was moved off the map)
			// (turned so that the start is in the middle, until the map is centred; not the whole map's)
			const auto [sx, sy] = wholeMap ? std::pair{ 0, 0 } : shift();
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					const PlotView& p = map.plots[size_t(x) + size_t(y) * W];
					const int ox = map.wrapX ? ((x - sx) % W + W) % W : x - sx, oy = map.wrapY ? ((y - sy) % H + H) % H : y - sy;
					if (ox < 0 || ox >= W || oy < 0 || oy >= H)
						continue;
					uint8_t* px = &minimapPixels[(size_t(H - 1 - oy) * W + ox) * 4];
					uint32_t c = p.terrain >= 0 && size_t(p.terrain) < terrainColors.size() ? terrainColors[size_t(p.terrain)] : 0xFF808080;
					float b = float(c & 255) / 255, g = float((c >> 8) & 255) / 255, r = float((c >> 16) & 255) / 255;
					if (p.peak)
						r = g = b = 0.75f;
					else if (p.hills)
						r *= 0.8f, g *= 0.8f, b *= 0.8f;
					if (p.owner >= 0 && size_t(p.owner) < map.players.size())
					{
						const Color& o = map.players[size_t(p.owner)].primary;
						const float k = p.water ? ownedWater : ownedLand;
						r += (o.r - r) * k, g += (o.g - g) * k, b += (o.b - b) * k;
					}
					if (fog && !p.revealed)
						r = g = b = 0;
					px[0] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255);
					px[1] = uint8_t(std::clamp(g, 0.0f, 1.0f) * 255);
					px[2] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255);
					px[3] = 255;
				}
			for (const CityView& c : map.cities)
			{
				if (!cities || (fog && !map.plots[size_t(c.x) + size_t(c.y) * W].revealed))
					continue;
				const int ox = map.wrapX ? ((c.x - sx) % W + W) % W : c.x - sx, oy = map.wrapY ? ((c.y - sy) % H + H) % H : c.y - sy;
				if (ox < 0 || ox >= W || oy < 0 || oy >= H)
					continue;
				uint8_t* px = &minimapPixels[(size_t(H - 1 - oy) * W + ox) * 4];
				px[0] = px[1] = px[2] = 255;
			}
		}

		void updatePlots()
		{
			const int W = map.width, H = map.height;
			std::vector<uint16_t> data(size_t(W) * H * 4);
			for (size_t i = 0; i < map.plots.size(); ++i)
			{
				const PlotView& p = map.plots[i];
				uint16_t overlay = kNoOverlay;
				if (!p.water && p.peak && map.peakTerrain >= 0)
					overlay = uint16_t(map.peakTerrain);
				else if (!p.water && p.hills && map.hillTerrain >= 0)
					overlay = uint16_t(map.hillTerrain);
				data[i * 4 + 0] = uint16_t(std::max<int>(p.terrain, 0));
				data[i * 4 + 1] = overlay;
				data[i * 4 + 2] = uint16_t(p.owner + 1);
				const uint16_t decal = p.feature >= 0 && size_t(p.feature) < featureDecal.size() ? featureDecal[size_t(p.feature)] : 0;
				data[i * 4 + 3] = uint16_t((p.water ? 1 : 0) | (p.riverSouth || p.riverEast ? 2 : 0) | (p.revealed ? 4 : 0) | (p.visible ? 8 : 0) |
					(p.riverSouth ? 16 : 0) | (p.riverEast ? 32 : 0) | decal << 8);
			}
			if (!plotTex || plotTex->width != uint32_t(W) || plotTex->height != uint32_t(H))
			{
				const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size() * 2);
				plotTex = renderer.createTexture(uint32_t(W), uint32_t(H), 1, 1, TextureFormat::RGBA16, std::span(&bytes, 1));
			}
			else
				renderer.updateTexture(*plotTex, 0, 0, 0, 0, uint32_t(W), uint32_t(H), data.data());
			updateCells(data);
		}

		// The culture borders' tiles, the original's (CIV4RiverModelInfos' BorderFile and the two
		// without a river), a slice each of borderArtTex: their alpha (they are white).
		void buildBorderArt()
		{
			std::vector<std::string> files{ "Art/Terrain/Routes/Rivers/NoRiverBorder.dds", "Art/Terrain/Routes/Rivers/NoRiverCurvedBorder.dds" };
			riverBorderSlice.assign(map.rivers.size(), -1);
			for (size_t r = 0; r < map.rivers.size(); ++r)
			{
				if (map.rivers[r].border.empty())
					continue;
				const std::string key = makeAssetKey(map.rivers[r].border);
				auto it = std::ranges::find_if(files, [&](const std::string& f) { return makeAssetKey(f) == key; });
				if (it == files.end())
				{
					files.push_back(map.rivers[r].border);
					it = files.end() - 1;
				}
				riverBorderSlice[r] = int(it - files.begin());
			}
			std::vector<std::vector<uint8_t>> slices;
			for (const std::string& f : files)
			{
				std::vector<uint8_t> alpha(size_t(kBorderArtSize) * kBorderArtSize, 0);
				if (const auto loc = assets.locate(makeAssetKey(f)))
					try
					{
						const gfx::Image image = gfx::loadTexture(assets.read(*loc));
						const auto pixels = gfx::decodeBgra(image);
						if (image.width != uint32_t(kBorderArtSize) || image.height != uint32_t(kBorderArtSize))
							problem(std::format("{}: {}x{}, not {} square", f, image.width, image.height, kBorderArtSize));
						else
							for (size_t i = 0; i < alpha.size(); ++i)
								alpha[i] = pixels[i * 4 + 3];
					}
					catch (const std::exception& e)
					{
						problem(std::format("{}: {}", f, e.what()));
					}
				else
					problem(std::format("{}: not found", f));
				slices.push_back(std::move(alpha));
			}
			const std::vector<std::span<const uint8_t>> subresources(slices.begin(), slices.end());
			borderArtTex = renderer.createTexture(kBorderArtSize, kBorderArtSize, uint32_t(slices.size()), 1, TextureFormat::R8, subresources);
		}

		// The culture borders, as the original draws them: at every plot corner, for each owner
		// of the four plots around it, the tile for which of them are its. The tiles are the
		// cells of a 512-texel file, a plot square centred on the corner: no river,
		// noriverborder.dds (the outer corners notched), where the owner's plots there are water
		// norivercurvedborder.dds, and where a river piece lies at the corner the piece's
		// BorderFile, turned as the piece is, whose line keeps to that piece's banks (the river's
		// water and any island in it nobody's). borderTex holds them, four texels a corner (BGRA:
		// owner + 1, cell, slice of borderArtTex, quarter turns clockwise); the border shader
		// draws, of a corner's tiles, the most opaque, as CultureBorderShader.fx does.
		void updateBorders()
		{
			const int W = map.width, H = map.height;
			const int CW = map.wrapX ? W : W + 1, CH = map.wrapY ? H : H + 1;
			std::vector<int16_t> owners(map.plots.size());
			std::vector<uint8_t> water(map.plots.size());
			for (size_t i = 0; i < owners.size(); ++i)
				owners[i] = map.plots[i].owner, water[i] = map.plots[i].water;
			if (!borderArtTex)
				buildBorderArt();
			if (!cornerRiverChanged && borderTex && borderTex->width == uint32_t(CW * 4) && borderTex->height == uint32_t(CH) && owners == borderOwners &&
				water == borderWater)
				return;
			cornerRiverChanged = false;
			borderOwners = owners;
			borderWater = water;

			const auto plotIndex = [&](int px, int py) -> int {
				if (map.wrapX)
					px = (px % W + W) % W;
				if (map.wrapY)
					py = (py % H + H) % H;
				return px < 0 || py < 0 || px >= W || py >= H ? -1 : px + py * W;
			};
			// The cells (row * 4 + column) by which of the four plots round the corner are the
			// owner's, as the tiles have them unturned, north up: bits north-west 1, north-east 2,
			// south-east 4, south-west 8. Measured from where each cell's wash is. None for all
			// four or none; two diagonal plots take a cell each.
			static constexpr uint8_t kNone = 255;
			static constexpr uint8_t kCell[16] = { kNone, 8, 9, 0, 3, kNone, 7, 5, 2, 6, kNone, 4, 1, 10, 11, kNone };
			std::vector<uint8_t> data(size_t(CW) * 4 * size_t(CH) * 4, 0);
			for (int cy = 0; cy < CH; ++cy)
				for (int cx = 0; cx < CW; ++cx)
				{
					// (the plots round the corner clockwise from the north-west: plot (cx, cy) is its north-east)
					const int quad[4] = { plotIndex(cx - 1, cy), plotIndex(cx, cy), plotIndex(cx, cy - 1), plotIndex(cx - 1, cy - 1) };
					int riverSlice = -1, turn = 0;
					const auto river = cornerRiver.size() == size_t(CW) * CH ? cornerRiver[size_t(cy) * CW + cx] : std::pair<int16_t, int16_t>{ -1, 0 };
					if (river.first >= 0 && size_t(river.first) < riverBorderSlice.size() && riverBorderSlice[size_t(river.first)] >= 0)
						riverSlice = riverBorderSlice[size_t(river.first)], turn = ((river.second / 90) % 4 + 4) % 4;
					uint8_t* out = data.data() + (size_t(cy) * CW + cx) * 16;
					int slot = 0, slice = 0;
					const auto put = [&](int16_t owner, int mask) {
						if (slot == 4 || kCell[mask] == kNone)
							return;
						uint8_t* t = out + slot++ * 4;
						t[2] = uint8_t(std::min(owner + 1, 255)), t[1] = kCell[mask], t[0] = uint8_t(slice), t[3] = uint8_t(turn);
					};
					for (int k = 0; k < 4; ++k)
					{
						const int16_t owner = quad[k] >= 0 ? owners[size_t(quad[k])] : int16_t(-1);
						if (owner < 0 || std::ranges::any_of(quad, quad + k, [&](int q) { return q >= 0 && owners[size_t(q)] == owner; }))
							continue;
						// (the curved tiles where the owner's ground at the corner is water: in the
						// original a corner of land by the sea is notched as inland, one over the sea
						// is round)
						slice = riverSlice >= 0 ? riverSlice
							: std::ranges::any_of(quad, [&](int q) { return q >= 0 && owners[size_t(q)] == owner && water[size_t(q)]; }) ? 1 : 0;
						// (unturned, the tile's quadrant j is the world's j + turn)
						int mask = 0;
						for (int j = 0; j < 4; ++j)
							if (const int q = quad[(j + turn) % 4]; q >= 0 && owners[size_t(q)] == owner)
								mask |= 1 << j;
						if (mask == 5)
							put(owner, 1), put(owner, 4);
						else if (mask == 10)
							put(owner, 2), put(owner, 8);
						else
							put(owner, mask);
					}
				}
			if (!borderTex || borderTex->width != uint32_t(CW * 4) || borderTex->height != uint32_t(CH))
			{
				const std::span<const uint8_t> bytes(data.data(), data.size());
				borderTex = renderer.createTexture(uint32_t(CW * 4), uint32_t(CH), 1, 1, TextureFormat::BGRA8, std::span(&bytes, 1));
			}
			else
				renderer.updateTexture(*borderTex, 0, 0, 0, 0, uint32_t(CW * 4), uint32_t(CH), data.data());
		}

		// The terrain shader's tile variant hash, the same bits.
		static uint32_t tileHash(int x, int y, uint32_t salt)
		{
			uint32_t h = uint32_t(x) * 0x9E3779B1u ^ (uint32_t(y) + salt * 0x632BE5ABu) * 0x85EBCA77u;
			h ^= h >> 15;
			h *= 0x2C1B3C6Du;
			h ^= h >> 12;
			h *= 0x297A2D39u;
			h ^= h >> 15;
			return h;
		}

		// The ground's tiles, each spanning four plot centres (tile -1 is before the first plot):
		// the terrain layers to blend, lowest first as a whole tile, every higher one from the
		// atlas tile for the corners at or above its layer, then the hills and peaks by the same
		// rule, as the terrain shader chose them per pixel before. Two texels a tile, six entries:
		// atlas slice | quarter turns << 14, 0xFFFF none. (Worked out here when the plots change:
		// that sorting, hashing and table reading were most of the ground's cost on slower GPUs.)
		void updateCells(const std::vector<uint16_t>& plotData)
		{
			const int W = map.width, H = map.height;
			const int count = int(map.terrains.size());
			const uint32_t CW = uint32_t(W + 1) * 2, CH = uint32_t(H + 1);
			std::vector<uint16_t> data(size_t(CW) * CH * 4, 0xFFFF);
			static const int kCorner[4][2] = { { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 } }; // top left, top right, bottom right, bottom left
			constexpr int kNone = -1000000;
			const auto wrap = [](int v, int n, bool wraps) { return wraps ? (v % n + n) % n : std::clamp(v, 0, n - 1); };
			for (int cy = -1; cy < H; ++cy)
				for (int cx = -1; cx < W; ++cx)
				{
					int terrain[4], overlay[4], layer[4], overlayLayer[4];
					for (int k = 0; k < 4; ++k)
					{
						const size_t i = size_t(wrap(cy + kCorner[k][1], H, map.wrapY)) * size_t(W) + size_t(wrap(cx + kCorner[k][0], W, map.wrapX));
						terrain[k] = plotData[i * 4];
						overlay[k] = plotData[i * 4 + 1];
						layer[k] = terrain[k] < count ? map.terrains[size_t(terrain[k])].layerOrder : kNone;
						overlayLayer[k] = overlay[k] != kNoOverlay && overlay[k] < count ? map.terrains[size_t(overlay[k])].layerOrder : kNone;
					}
					uint16_t* out = &data[(size_t(cy + 1) * CW + size_t(cx + 1) * 2) * 4];
					int used = 0;
					const int hx = wrap(cx, W, map.wrapX), hy = wrap(cy, H, map.wrapY);
					const auto blend = [&](const int* ids, const int* layers, int steps, bool wholeFirst) {
						int previous = kNone;
						for (int step = 0; step < steps; ++step)
						{
							int best = -kNone, t = 0;
							for (int k = 0; k < 4; ++k)
								if (layers[k] > previous && layers[k] < best)
								{
									best = layers[k];
									t = ids[k];
								}
							if (best == -kNone)
								break;
							uint32_t mask = 0;
							for (int k = 0; k < 4; ++k)
								if (layers[k] >= best)
									mask |= 1u << k;
							if (wholeFirst && step == 0)
								mask = 15;
							previous = best;
							const auto& slots = map.terrains[size_t(t)].slots[mask];
							if (slots.empty())
								continue;
							const size_t n = std::min<size_t>(slots.size(), 255);
							const auto& [slot, degrees] = slots[tileHash(hx, hy, uint32_t(t)) % n];
							const uint32_t slice = uint32_t(t) * kSlots + uint32_t(std::clamp(slot - 1, 0, int(kSlots) - 1));
							out[used++] = uint16_t(slice | uint32_t((degrees / 90 % 4 + 4) % 4) << 14);
						}
					};
					blend(terrain, layer, 4, true);
					blend(overlay, overlayLayer, 2, false);
				}
			if (!cellTex || cellTex->width != CW || cellTex->height != CH)
			{
				const std::span<const uint8_t> bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size() * 2);
				cellTex = renderer.createTexture(CW, CH, 1, 1, TextureFormat::RGBA16, std::span(&bytes, 1));
			}
			else
				renderer.updateTexture(*cellTex, 0, 0, 0, 0, CW, CH, data.data());
		}

		float ground(float x, float y) const
		{
			if (heights.empty())
				return 0;
			const float step = S() / kSub;
			float gx = x / step, gy = y / step;
			if (map.wrapX)
				gx = std::fmod(std::fmod(gx, float(gridW - 1)) + float(gridW - 1), float(gridW - 1));
			gx = std::clamp(gx, 0.0f, float(gridW - 1) - 0.001f);
			gy = std::clamp(gy, 0.0f, float(gridH - 1) - 0.001f);
			const int i = int(gx), j = int(gy);
			const float fx = gx - float(i), fy = gy - float(j);
			const auto at = [&](int a, int b) { return heights[size_t(std::min(b, gridH - 1)) * gridW + std::min(a, gridW - 1)]; };
			return (at(i, j) * (1 - fx) + at(i + 1, j) * fx) * (1 - fy) + (at(i, j + 1) * (1 - fx) + at(i + 1, j + 1) * fx) * fy;
		}

		// ------------------------------------------------------------ models on the map

		// A model placed once: its bind pose, every node below the root standing on the ground.
		Instance placeStatic(LoadedModel* lm, const Affine& place, const Affine& local = {}, const std::vector<uint8_t>* visible = nullptr,
			const Bounds* bounds = nullptr)
		{
			Instance inst;
			inst.model = lm;
			inst.place = place * local;
			inst.world.resize(lm->bindWorld.size());
			for (size_t i = 0; i < inst.world.size(); ++i)
				inst.world[i] = inst.place * lm->bindWorld[i];
			std::vector<float> dz(inst.world.size(), 0.0f);
			for (size_t i = 0; i < inst.world.size(); ++i)
				if (lm->model.nodes[i].parent == lm->spread && lm->spread >= 0)
				{
					const Vec3 p = inst.world[i].translation;
					dz[i] = ground(p.x, p.y) - place.translation.z;
				}
			for (size_t i = 0; i < inst.world.size(); ++i)
				inst.world[i].translation.z += dz[size_t(lm->top[i])];
			if (visible)
				inst.visible = *visible;
			const Bounds& b = bounds ? *bounds : lm->bounds;
			inst.center = inst.place.point(b.center());
			inst.radius = b.radius() * inst.place.scale + 1;
			return inst;
		}

		void buildFeatures()
		{
			features.clear();
			const int W = map.width, H = map.height;
			const auto featureArt = [&](int f) -> const FeatureArt* { return f >= 0 && size_t(f) < map.features.size() ? &map.features[size_t(f)] : nullptr; };
			const auto findPiece = [](const std::vector<FeatureArt::Piece>& pieces, int mask) -> const FeatureArt::Piece* {
				for (const auto& p : pieces)
					if (p.connections == mask)
						return &p;
				return nullptr;
			};
			// (a piece stands on the ground, or at `at.z` where that is higher: the water's level 0)
			constexpr float kOnGround = -std::numeric_limits<float>::infinity();
			const auto add = [&](const FeatureArt& art, const FeatureArt::Piece& piece, Vec3 at, uint32_t h) {
				if (piece.models.empty())
					return;
				const auto& [nif, rotation] = piece.models[h % piece.models.size()];
				// Some pieces are textures laid on the terrain (flood plains): not drawn yet.
				if (!toLowerAscii(nif).ends_with(".nif"))
					return;
				if (LoadedModel* lm = model(nif, {}))
				{
					at.z = std::max(ground(at.x, at.y), at.z);
					features.push_back(placeStatic(lm, placement(at, -float(rotation) * std::numbers::pi_v<float> / 2, art.scale)));
				}
			};

			// Trees and ice tile by plot corners: each corner takes the piece for the plots
			// around it that have the feature. (The original grows no trees on water: a forest
			// or jungle on a water plot isn't drawn, while ice, fallout and the others are.)
			const auto shownFeature = [&](const PlotView* p) {
				if (!p)
					return -1;
				const FeatureArt* art = featureArt(p->feature);
				return art && art->tiling == FeatureArt::Tiling::Trees && p->water ? -1 : p->feature;
			};
			const int cornersX = map.wrapX ? W : W + 1, cornersY = map.wrapY ? H : H + 1;
			for (int cy = 0; cy < cornersY; ++cy)
				for (int cx = 0; cx < cornersX; ++cx)
				{
					const PlotView* around[4] = { map.plot(cx - 1, cy), map.plot(cx, cy), map.plot(cx, cy - 1), map.plot(cx - 1, cy - 1) };
					const int bits[4] = { kNorthWest, kNorthEast, kSouthEast, kSouthWest };
					int done[4] = { -1, -1, -1, -1 };
					for (int k = 0; k < 4; ++k)
					{
						const int f = shownFeature(around[k]);
						const FeatureArt* art = featureArt(f);
						if (!art || (art->tiling != FeatureArt::Tiling::Trees && art->tiling != FeatureArt::Tiling::HalfTiling) ||
							std::find(std::begin(done), std::end(done), f) != std::end(done))
							continue;
						done[k] = f;
						int mask = 0;
						for (int j = 0; j < 4; ++j)
							if (shownFeature(around[j]) == f)
								mask |= bits[j];
						if (art->varieties.empty())
							continue;
						const size_t variety = std::min<size_t>(around[k]->featureVariety, art->varieties.size() - 1);
						if (const auto* piece = findPiece(art->varieties[variety], mask))
							add(*art, *piece, { float(cx) * S(), float(cy) * S(), kOnGround }, hash(cx, cy, f));
					}
				}
			// The others stand in their plot.
			for (int y = 0; y < H; ++y)
				for (int x = 0; x < W; ++x)
				{
					const PlotView& p = map.plots[size_t(x) + size_t(y) * W];
					const FeatureArt* art = featureArt(p.feature);
					if (!art || art->tiling == FeatureArt::Tiling::Trees || art->tiling == FeatureArt::Tiling::HalfTiling || art->varieties.empty())
						continue;
					const auto& pieces = art->varieties[std::min<size_t>(p.featureVariety, art->varieties.size() - 1)];
					const auto* piece = findPiece(pieces, 0);
					if (!piece && !pieces.empty())
						piece = &pieces.front();
					// (on a water plot it stands on the water, at the level of the original's plot point
					// there: an oasis's pool and sand over the sea, not under it on the sea floor)
					if (piece)
						add(*art, *piece, { (float(x) + 0.5f) * S(), (float(y) + 0.5f) * S(), p.water ? 0.0f : kOnGround }, hash(x, y, p.feature));
				}
		}

		// A model of the snapshot standing at a point, animated when it has an idle animation.
		// Draped: its heights over the terrain under each of its points, as roads are (a land
		// resource: its fields would sink under the slope of a hill beside it). Not a model with
		// skinned figures (cows, horses, deer): they stand upright.
		void addDecor(int index, Vec3 at, float yaw, uint32_t h, int state = 0, bool draped = false)
		{
			if (index < 0 || size_t(index) >= map.models.size())
				return;
			const ModelArt& art = map.models[size_t(index)];
			LoadedModel* lm = model(art.nif, art.kfm);
			if (!lm)
				return;
			if (std::ranges::any_of(lm->model.items, [](const scene::DrawItem& item) { return item.skinned; }))
				draped = false;
			const float groundZ = ground(at.x, at.y);
			at.z = draped ? 4.0f : groundZ; // (draped: a little over it: low leaves, a wheat field's, are not lost in the terrain's bumps)
			const anim::Sequence* sequence = state >= 1 && state <= 3 && lm->states[state - 1] ? &*lm->states[state - 1] : nullptr;
			if (draped && !lm->idle && !sequence)
			{
				Instance inst;
				inst.model = lm;
				inst.place = placement(at, yaw, art.scale);
				inst.world.resize(lm->bindWorld.size());
				for (size_t i = 0; i < inst.world.size(); ++i)
					inst.world[i] = inst.place * lm->bindWorld[i];
				inst.center = inst.place.point(lm->bounds.center()) + Vec3{ 0, 0, groundZ };
				inst.radius = lm->bounds.radius() * art.scale + 1;
				inst.onGround = true;
				decor.push_back(std::move(inst));
				return;
			}
			if (!lm->idle && !sequence)
			{
				decor.push_back(placeStatic(lm, placement(at, yaw, art.scale)));
				return;
			}
			Instance inst;
			inst.model = lm;
			inst.place = placement(at, yaw, art.scale);
			inst.center = inst.place.point(lm->bounds.center()) + Vec3{ 0, 0, draped ? groundZ : 0 };
			inst.radius = lm->bounds.radius() * art.scale + 1;
			inst.animated = true;
			inst.onGround = draped;
			inst.sequence = sequence;
			inst.phase = unitFloat(h) * 10;
			decor.push_back(std::move(inst));
		}

		// What the plot LSystem lays out on a plot: models standing, and nodes of models (a
		// farm's crops) laid on the ground, each over the one before it.
		void addLayout(const std::vector<PlotPiece>& pieces, Vec3 center, uint32_t h, int state = 0)
		{
			float lift = 0.5f;
			for (const PlotPiece& piece : pieces)
			{
				const Vec3 at{ center.x + piece.x * S(), center.y + piece.y * S(), 0 };
				if (piece.cut > 0)
					layoutCuts.push_back({ at, piece.cut * S() });
				if (piece.node.empty())
				{
					addDecor(piece.model, at, piece.yaw, h++, state);
					continue;
				}
				if (piece.model < 0 || size_t(piece.model) >= map.models.size())
					continue;
				const ModelArt& art = map.models[size_t(piece.model)];
				LoadedModel* lm = model(art.nif, {});
				if (!lm)
					continue;
				buildParts(*lm);
				const auto part = std::ranges::find_if(lm->parts, [&](const LoadedModel::Part& p) {
					return toLowerAscii(lm->model.nodes[size_t(p.node)].name) == toLowerAscii(piece.node);
				});
				if (part == lm->parts.end())
					continue;
				if (piece.standing)
				{
					// (a house of a city file: stands on the ground, as the city's own houses)
					const Vec3 middle = part->bounds.center();
					decor.push_back(placeStatic(lm, placement({ at.x, at.y, ground(at.x, at.y) }, piece.yaw, art.scale),
						translation({ -middle.x, -middle.y, 0 }), &part->visible, &part->bounds));
					continue;
				}
				// (the node stands somewhere in its file: moved to the middle of the plot's piece)
				const Vec3 origin = lm->bindWorld[size_t(part->node)].translation;
				Instance inst;
				inst.model = lm;
				inst.place = placement({ at.x, at.y, lift }, piece.yaw, art.scale) * translation({ -origin.x, -origin.y, 0 });
				inst.world.resize(lm->bindWorld.size());
				for (size_t i = 0; i < inst.world.size(); ++i)
					inst.world[i] = inst.place * lm->bindWorld[i];
				inst.visible = part->visible;
				inst.center = { at.x, at.y, ground(at.x, at.y) };
				inst.radius = part->bounds.radius() * art.scale + 0.2f * S();
				inst.onGround = true;
				inst.laid = true; // (the original: a farm's crops tinted by the border, as the ground)
				decor.push_back(std::move(inst));
				lift += 0.1f;
			}
		}

		// A piece laid on the terrain (roads, rivers): heights over it, a little above it
		// against z-fighting; turned clockwise.
		void addOnGround(const std::string& nif, Vec3 at, int degrees, float lift, bool route = false)
		{
			LoadedModel* lm = model(nif, {});
			if (!lm)
				return;
			Instance inst;
			inst.model = lm;
			inst.place = placement({ at.x, at.y, lift }, -float(degrees) * std::numbers::pi_v<float> / 180, 1);
			inst.world.resize(lm->bindWorld.size());
			for (size_t i = 0; i < inst.world.size(); ++i)
				inst.world[i] = inst.place * lm->bindWorld[i];
			inst.center = { at.x, at.y, ground(at.x, at.y) };
			inst.radius = lm->bounds.radius() + 0.2f * S();
			inst.onGround = true;
			inst.route = route;
			inst.laid = true;
			decor.push_back(std::move(inst));
		}

		// Rivers run along plot edges: at every plot corner the piece for the edges from it
		// that have river, a delta where one ends at the coast.
		void addRivers()
		{
			const auto south = [&](int x, int y) { const PlotView* q = map.plot(x, y); return q && q->riverSouth; };
			const auto east = [&](int x, int y) { const PlotView* q = map.plot(x, y); return q && q->riverEast; };
			const auto water = [&](int x, int y) { const PlotView* q = map.plot(x, y); return q && q->water; };
			const int cornersX = map.wrapX ? map.width : map.width + 1, cornersY = map.wrapY ? map.height : map.height + 1;
			// (which piece lies at each corner, for the culture borders' tiles there)
			std::vector<std::pair<int16_t, int16_t>> pieces(size_t(cornersX) * cornersY, { int16_t(-1), int16_t(0) });
			for (int cy = 0; cy < cornersY; ++cy)
				for (int cx = 0; cx < cornersX; ++cx)
				{
					// The plots around corner (cx, cy): north-east is (cx, cy).
					const int mask = (east(cx - 1, cy) ? 1 << 0 : 0) | (south(cx, cy) ? 1 << 2 : 0) | (east(cx - 1, cy - 1) ? 1 << 4 : 0) |
						(south(cx - 1, cy) ? 1 << 6 : 0);
					if (!mask)
						continue;
					const bool end = (mask & (mask - 1)) == 0;
					const bool coast = water(cx - 1, cy) || water(cx, cy) || water(cx, cy - 1) || water(cx - 1, cy - 1);
					std::vector<std::pair<const RiverArt*, int>> fits, deltas;
					for (const RiverArt& r : map.rivers)
						for (const int degrees : r.rotations.empty() ? std::vector<int>{ 0 } : r.rotations)
							if (turned(r.connections, degrees) == mask)
								(r.delta == RiverArt::Delta::None ? fits : deltas).push_back({ &r, degrees });
					// A river reaching the coast flows out through a delta: the one whose rapids fan
					// out over the plots around the corner that are water. Those plots are the
					// corner's diagonals (DirectionTypes bits, turned as the connections are).
					// Unturned, a delta's river comes from the north: the straight one fans out
					// over both plots ahead, the clockwise one over the south-west one (to the
					// river's right), the counter-clockwise one over the south-east one; one at a
					// bend (two connections) over the plot outside the bend.
					if (coast && !deltas.empty())
					{
						const int sea = (water(cx, cy) ? 1 << 1 : 0) | (water(cx, cy - 1) ? 1 << 3 : 0) | (water(cx - 1, cy - 1) ? 1 << 5 : 0) |
							(water(cx - 1, cy) ? 1 << 7 : 0);
						const auto fansOver = [&](const RiverArt& r) {
							if (r.connections == (1 << 0))
								return r.delta == RiverArt::Delta::Clockwise ? 1 << 5 : r.delta == RiverArt::Delta::CounterClockwise ? 1 << 3 : (1 << 3) | (1 << 5);
							int outside = 0;
							for (const int d : { 1, 3, 5, 7 })
								if (!(r.connections & (1 << (d - 1))) && !(r.connections & (1 << ((d + 1) % 8))))
									outside |= 1 << d;
							return outside;
						};
						std::pair<const RiverArt*, int> best{ nullptr, 0 };
						int bestFan = 0;
						for (const auto& d : deltas)
							if (const int fan = turned(fansOver(*d.first), d.second); fan && (fan & sea) == fan && std::popcount(unsigned(fan)) > bestFan)
								best = d, bestFan = std::popcount(unsigned(fan));
						if (best.first)
							fits = { best };
						else if (end)
						{
							// (the coast beside the river, not ahead of it: the straight one)
							const auto straight = std::ranges::find_if(deltas, [](const auto& d) { return d.first->delta == RiverArt::Delta::Straight; });
							fits = { straight != deltas.end() ? *straight : deltas.front() };
						}
					}
					if (fits.empty())
						continue;
					const auto& [r, degrees] = fits[hash(cx, cy, 31) % fits.size()];
					addOnGround(r->nif, { float(cx) * S(), float(cy) * S(), 0 }, degrees, 0.8f);
					pieces[size_t(cy) * cornersX + cx] = { int16_t(r - map.rivers.data()), int16_t(degrees) };
				}
			if (pieces != cornerRiver)
			{
				cornerRiver = std::move(pieces);
				cornerRiverChanged = true;
			}
		}

		// The files of a route piece the XML names. Most of the original's road pieces exist only as
		// variants: no RoadB03.nif, but RoadB03-000.nif to RoadB03-003.nif (the "a" to "d" of the
		// artists' road key, art/terrain/routes/roadkeypage*.jpg), and the game draws one of them.
		// Empty when there is none that loads (some files the XML lists do not exist, in the
		// original either).
		const std::vector<std::string>& routeFiles(const std::string& nif)
		{
			const auto [it, added] = routeVariants.try_emplace(nif);
			if (!added)
				return it->second;
			const auto loads = [&](const std::string& file) { return assets.locate(makeAssetKey(file)) && model(file, {}); };
			if (loads(nif))
				it->second.push_back(nif);
			else if (nif.size() > 4 && toLowerAscii(nif.substr(nif.size() - 4)) == ".nif")
				for (int i = 0; i < 100; ++i)
					if (const std::string file = std::format("{}-{:03}.nif", nif.substr(0, nif.size() - 4), i); loads(file))
						it->second.push_back(file);
					else
						break;
			return it->second;
		}

		// Route pieces: the one whose connections, turned, are the neighbours with routes;
		// else one piece per neighbour.
		void addRoute(int x, int y, const PlotView& p)
		{
			static constexpr int kDx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, kDy[8] = { 1, 1, 0, -1, -1, -1, 0, 1 }; // DirectionTypes
			int mask = 0;
			for (int d = 0; d < 8; ++d)
				if (const PlotView* q = map.plot(x + kDx[d], y + kDy[d]); q && q->route >= 0)
					mask |= 1 << d;
			const Vec3 center{ (float(x) + 0.5f) * S(), (float(y) + 0.5f) * S(), 0 };
			// (one of the piece's variants, the same for the plot each time the decor is built)
			const auto place = [&](const RouteArt& r, int degrees) {
				const auto& files = routeFiles(r.nif);
				addOnGround(files[hash(x, y, 41) % files.size()], center, degrees, 0.6f, true);
			};
			const auto find = [&](int wanted) -> std::pair<const RouteArt*, int> {
				for (const RouteArt& r : map.routes)
					if (r.route == p.route)
						for (const int degrees : r.rotations.empty() ? std::vector<int>{ 0 } : r.rotations)
							if (turned(r.connections, degrees) == wanted && !routeFiles(r.nif).empty())
								return { &r, degrees };
				return { nullptr, 0 };
			};
			if (const auto [r, degrees] = find(mask); r)
			{
				place(*r, degrees);
				return;
			}
			for (int d = 0; d < 8; ++d)
				if (mask & (1 << d))
					if (const auto [r, degrees] = find(1 << d); r)
						place(*r, degrees);
		}

		// Bridges: where a road crosses a river between two plots side by side (not corner to
		// corner), across the middle of the edge, along the road; a railroad bridge between two
		// railroads. On the higher of the banks: the river's bed is lower.
		void addBridges()
		{
			const auto bridge = [&](const PlotView& a, const PlotView& b, Vec3 at, float yaw) {
				const auto& art = a.route == map.railroadRoute && b.route == map.railroadRoute && !map.railroadBridge.nif.empty() ? map.railroadBridge : map.routeBridge;
				LoadedModel* lm = art.nif.empty() ? nullptr : model(art.nif, "");
				if (!lm)
					return;
				at.z = std::max(ground(at.x, at.y), 0.5f * (ground(at.x - S() / 2 * std::cos(yaw), at.y - S() / 2 * std::sin(yaw)) +
					ground(at.x + S() / 2 * std::cos(yaw), at.y + S() / 2 * std::sin(yaw))));
				decor.push_back(placeStatic(lm, placement(at, yaw, art.scale > 0 ? art.scale : 1)));
			};
			for (int y = 0; y < map.height; ++y)
				for (int x = 0; x < map.width; ++x)
				{
					const PlotView& p = map.plots[size_t(x) + size_t(y) * size_t(map.width)];
					if (p.route < 0)
						continue;
					if (const PlotView* east = map.plot(x + 1, y); east && east->route >= 0 && p.riverEast)
						bridge(p, *east, { float(x + 1) * S(), (float(y) + 0.5f) * S(), 0 }, 0);
					if (const PlotView* south = map.plot(x, y - 1); south && south->route >= 0 && p.riverSouth)
						bridge(p, *south, { (float(x) + 0.5f) * S(), float(y) * S(), 0 }, std::numbers::pi_v<float> / 2);
				}
		}

		void buildDecor()
		{
			decor.clear();
			layoutCuts.clear();
			addRivers();
			for (int y = 0; y < map.height; ++y)
				for (int x = 0; x < map.width; ++x)
				{
					const PlotView& p = map.plots[size_t(x) + size_t(y) * size_t(map.width)];
					const Vec3 center{ (float(x) + 0.5f) * S(), (float(y) + 0.5f) * S(), 0 };
					if (p.route >= 0)
						addRoute(x, y, p);
					if (p.layout >= 0 && size_t(p.layout) < map.plotLayouts.size())
						addLayout(map.plotLayouts[size_t(p.layout)], center, hash(x, y, 7), p.improvementState);
					else if (p.improvement >= 0 && size_t(p.improvement) < map.improvements.size())
						addDecor(map.improvements[size_t(p.improvement)], center, 0, hash(x, y, 7), p.improvementState);
					if (p.bonus >= 0 && size_t(p.bonus) < map.bonuses.size())
						addDecor(map.bonuses[size_t(p.bonus)], center, 0, hash(x, y, 8), p.bonusState, !p.water);
				}
			addBridges();
		}

		// Trees inside the tree cut spheres of the improvements, resources, roads and rivers go:
		// a tree that is a node of its own, or one of a forest piece's trees (DrawItem::trees).
		static constexpr float kRouteTreeCut = 0.12f;

		void cutTrees()
		{
			std::unordered_map<int, std::vector<std::pair<Vec3, float>>> cuts; // by plot
			const auto plotKey = [&](float x, float y) {
				int px = int(std::floor(x / S())), py = int(std::floor(y / S()));
				if (map.wrapX)
					px = (px % map.width + map.width) % map.width;
				return px + py * map.width;
			};
			std::unordered_map<int, std::vector<const Instance*>> waters; // by plot
			for (const Instance& d : decor)
			{
				for (const auto& [c, r] : d.model->treeCuts)
				{
					const Vec3 w = d.place.point(c);
					cuts[plotKey(w.x, w.y)].push_back({ w, r * d.place.scale });
				}
				if (d.model->water)
					waters[plotKey(d.place.translation.x, d.place.translation.y)].push_back(&d);
				// (a road or railroad clears a lane through a forest, as in the original: the trees
				// within kRouteTreeCut plots of its points go; its points are 10-15 units apart)
				if (d.route)
					for (const scene::DrawItem& item : d.model->model.items)
						if (item.node >= 0 && size_t(item.node) < d.world.size())
							for (const auto& v : item.vertices)
							{
								const Vec3 w = d.world[size_t(item.node)].point(v.position);
								cuts[plotKey(w.x, w.y)].push_back({ w, kRouteTreeCut * S() });
							}
			}
			for (const auto& [c, r] : layoutCuts)
				cuts[plotKey(c.x, c.y)].push_back({ c, r });
			if (cuts.empty() && waters.empty())
				return;
			// A river piece's triangles where they lie on the map, and for each cell of a grid
			// over them the triangles that reach into it (in their order): a point is tested
			// against those only. (Every triangle for every point of every tree near a river
			// took most of a second whenever an improvement or a road changed.)
			struct RiverCells
			{
				std::vector<Vec3> corners; // (placed)
				float x0 = 0, y0 = 0, size = 1;
				int columns = 0, rows = 0;
				std::vector<std::vector<uint32_t>> cells; // the first corner of each triangle
			};
			std::unordered_map<const Instance*, RiverCells> riverCells;
			const auto cellsOf = [&](const Instance& river) -> const RiverCells& {
				auto [it, added] = riverCells.try_emplace(&river);
				RiverCells& g = it->second;
				if (!added)
					return g;
				const LoadedModel::Water& w = *river.model->water;
				const size_t n = w.corners.size() / 3 * 3;
				g.corners.reserve(n);
				for (size_t i = 0; i < n; ++i)
					g.corners.push_back(river.place.point(w.corners[i]));
				if (g.corners.empty())
					return g;
				// (a triangle's box a little larger: a point just outside it counts, below)
				const float slack = 0.01f * S();
				float x0 = g.corners[0].x, y0 = g.corners[0].y, x1 = x0, y1 = y0;
				for (const Vec3& c : g.corners)
					x0 = std::min(x0, c.x), y0 = std::min(y0, c.y), x1 = std::max(x1, c.x), y1 = std::max(y1, c.y);
				g.size = S() / 8;
				g.x0 = x0 - slack, g.y0 = y0 - slack;
				g.columns = std::max(1, int((x1 + slack - g.x0) / g.size) + 1);
				g.rows = std::max(1, int((y1 + slack - g.y0) / g.size) + 1);
				g.cells.resize(size_t(g.columns) * size_t(g.rows));
				for (size_t t = 0; t < n; t += 3)
				{
					const Vec3 &a = g.corners[t], &b = g.corners[t + 1], &c = g.corners[t + 2];
					const int cx0 = std::clamp(int((std::min({ a.x, b.x, c.x }) - slack - g.x0) / g.size), 0, g.columns - 1);
					const int cx1 = std::clamp(int((std::max({ a.x, b.x, c.x }) + slack - g.x0) / g.size), 0, g.columns - 1);
					const int cy0 = std::clamp(int((std::min({ a.y, b.y, c.y }) - slack - g.y0) / g.size), 0, g.rows - 1);
					const int cy1 = std::clamp(int((std::max({ a.y, b.y, c.y }) + slack - g.y0) / g.size), 0, g.rows - 1);
					for (int cy = cy0; cy <= cy1; ++cy)
						for (int cx = cx0; cx <= cx1; ++cx)
							g.cells[size_t(cy) * size_t(g.columns) + size_t(cx)].push_back(uint32_t(t));
				}
				return g;
			};
			// In a river piece's water: the texture's alpha where the point is on its grid.
			const auto inWater = [&](const Instance& river, Vec3 p) {
				const LoadedModel::Water& w = *river.model->water;
				const RiverCells& g = cellsOf(river);
				const float fx = (p.x - g.x0) / g.size, fy = (p.y - g.y0) / g.size;
				if (g.cells.empty() || !(fx >= 0 && fy >= 0 && fx < float(g.columns) && fy < float(g.rows)))
					return false;
				for (const uint32_t t : g.cells[size_t(fy) * size_t(g.columns) + size_t(fx)])
				{
					const Vec3 &a = g.corners[t], &b = g.corners[t + 1], &c = g.corners[t + 2];
					const float d = (b.y - c.y) * (a.x - c.x) + (c.x - b.x) * (a.y - c.y);
					if (std::abs(d) < 1e-6f)
						continue;
					const float l0 = ((b.y - c.y) * (p.x - c.x) + (c.x - b.x) * (p.y - c.y)) / d;
					const float l1 = ((c.y - a.y) * (p.x - c.x) + (a.x - c.x) * (p.y - c.y)) / d;
					const float l2 = 1 - l0 - l1;
					if (l0 < -1e-4f || l1 < -1e-4f || l2 < -1e-4f)
						continue;
					float u = l0 * w.uvs[t][0] + l1 * w.uvs[t + 1][0] + l2 * w.uvs[t + 2][0];
					float v = l0 * w.uvs[t][1] + l1 * w.uvs[t + 1][1] + l2 * w.uvs[t + 2][1];
					u -= std::floor(u), v -= std::floor(v);
					const uint32_t x = std::min(uint32_t(u * float(w.width)), w.width - 1), y = std::min(uint32_t(v * float(w.height)), w.height - 1);
					return (*w.alpha)[size_t(y) * w.width + x] == 2;
				}
				return false;
			};
			const auto inCut = [&](Vec3 p) {
				for (int dy = -1; dy <= 1; ++dy)
					for (int dx = -1; dx <= 1; ++dx)
					{
						const int key = plotKey(p.x + float(dx) * S(), p.y + float(dy) * S());
						if (const auto it = cuts.find(key); it != cuts.end())
							for (const auto& [c, r] : it->second)
								if (std::hypot(p.x - c.x, p.y - c.y) < r)
									return true;
						// (a tree whose crown reaches over the water: its trunk on the bank)
						if (const auto it = waters.find(key); it != waters.end())
							for (const Instance* river : it->second)
								for (int k = 0; k < 9; ++k)
								{
									const float angle = float(k) * std::numbers::pi_v<float> / 4, reach = k == 8 ? 0 : 0.08f * S();
									if (inWater(*river, p + Vec3{ std::cos(angle) * reach, std::sin(angle) * reach, 0 }))
										return true;
								}
					}
				return false;
			};
			for (Instance& f : features)
			{
				const LoadedModel& lm = *f.model;
				for (size_t k = 0; k < lm.model.items.size(); ++k)
				{
					const scene::DrawItem& item = lm.model.items[k];
					if (item.trees.empty() || item.node < 0 || size_t(item.node) >= f.world.size())
						continue;
					uint32_t treesCut = 0;
					for (size_t t = 0; t < item.trees.size() && t < 32; ++t)
						if (!std::isnan(item.trees[t].x) && inCut(f.world[size_t(item.node)].point({ item.trees[t].x, item.trees[t].y, 0 })))
							treesCut |= 1u << t;
					if (!treesCut)
						continue;
					if (f.materials.empty())
						for (const auto& i : lm.model.items)
							f.materials.push_back(i.material);
					f.materials[k].treesCut = treesCut;
				}
				// (a piece that numbers its trees has them in one mesh, not a node each)
				if (std::ranges::any_of(lm.model.items, [](const scene::DrawItem& i) { return !i.trees.empty(); }))
					continue;
				bool changed = false;
				std::vector<uint8_t> visible = f.visible.empty() ? lm.bindVisible : f.visible;
				for (size_t i = 0; i < f.world.size(); ++i)
				{
					if (lm.model.nodes[i].parent != lm.spread || lm.spread < 0)
						continue;
					if (!inCut(f.world[i].translation))
						continue;
					for (size_t n = 0; n < visible.size(); ++n)
						if (lm.top[n] == int(i))
							visible[n] = 0;
					changed = true;
				}
				if (changed)
					f.visible = std::move(visible);
			}
		}

		Vec3 teamColor(int owner) const
		{
			if (owner >= 0 && size_t(owner) < map.players.size())
			{
				const Color& c = map.players[size_t(owner)].primary;
				return { c.r, c.g, c.b };
			}
			return kDefaultTeamColor;
		}

		// A city as the original lays it out, in short: a square of 16 x 16 lots, turned by the
		// city twist so that it fits the plot, takes the visible buildings (highest priority
		// first, nearest the middle) and then the generic houses on the lots left.
		// The Great Wall's pieces along its edges: segments end to end, posts at the corners.
		void addGreatWall()
		{
			if (map.greatWall.empty())
				return;
			LoadedModel* segment = model(map.greatWallSegment.nif, "");
			LoadedModel* post = model(map.greatWallPost.nif, "");
			if (!segment)
				return;
			const float segScale = map.greatWallSegment.scale > 0 ? map.greatWallSegment.scale : 1;
			const float postScale = map.greatWallPost.scale > 0 ? map.greatWallPost.scale : 1;
			const float length = std::max(segment->bounds.empty() ? 37.0f : std::max(segment->bounds.max.x - segment->bounds.min.x, segment->bounds.max.y - segment->bounds.min.y) * segScale, 1.0f);
			const int count = std::max(1, int(std::lround(S() / length)));
			std::unordered_set<uint64_t> corners;
			for (const auto& e : map.greatWall)
			{
				// (its ends, west to east or south to north)
				Vec3 a, b;
				const float x0 = float(e.x) * S(), y0 = float(e.y) * S(), x1 = x0 + S(), y1 = y0 + S();
				switch (e.side)
				{
				case 0: a = { x0, y1, 0 }, b = { x1, y1, 0 }; break;
				case 1: a = { x1, y0, 0 }, b = { x1, y1, 0 }; break;
				case 2: a = { x0, y0, 0 }, b = { x1, y0, 0 }; break;
				default: a = { x0, y0, 0 }, b = { x0, y1, 0 }; break;
				}
				const float yaw = std::atan2(b.y - a.y, b.x - a.x);
				for (int i = 0; i < count; ++i)
				{
					Vec3 at = a + (b - a) * ((float(i) + 0.5f) / float(count));
					at.z = ground(at.x, at.y);
					Instance inst = placeStatic(segment, placement(at, yaw, segScale));
					inst.plot = e.x + e.y * map.width;
					cities.push_back(std::move(inst));
				}
				if (post)
					for (const Vec3& c : { a, b })
					{
						const uint64_t key = uint64_t(uint32_t(std::lround(c.x))) << 32 | uint32_t(std::lround(c.y));
						if (!corners.insert(key).second)
							continue;
						Vec3 at = c;
						at.z = ground(at.x, at.y);
						Instance inst = placeStatic(post, placement(at, 0, postScale));
						inst.plot = e.x + e.y * map.width;
						cities.push_back(std::move(inst));
					}
			}
		}

		void buildCities()
		{
			cities.clear();
			addGreatWall();
			// The layout's size is the detail manager's CITY_LAYOUT_SIZE; taken as half the side
			// of the turned square here, which gives the original's proportions (a large city
			// reaches into the plots around it). A 1x1 house of the generic sets is some 55
			// units across, about a lot.
			constexpr int kLots = 16;
			const float lot = 2 * map.cityLayoutSize * S() / kLots;
			const float scale = lot / 50.0f;
			const float c0 = std::cos(map.cityTwist), s0 = std::sin(map.cityTwist);
			std::unordered_map<int, CityLayout> layouts;
			for (const CityView& c : map.cities)
			{
				std::string key;
				const auto put = [&key](const auto& v) { key.append(reinterpret_cast<const char*>(&v), sizeof(v)); };
				put(c.owner), put(c.generics), put(c.houseSet), put(teamColor(c.owner)), put(lot), put(map.cityTwist);
				for (const CityBuildingView& b : c.buildings)
					put(b.model), put(b.width), put(b.height), put(b.water), put(b.scale);
				for (const CityWallView& w : c.walls)
					for (const std::string* s : { &w.post, &w.wall, &w.gate })
						put(s->size()), key += *s;
				const int plotIndex = c.x + c.y * map.width;
				if (const auto kept = cityLayouts.find(plotIndex); kept != cityLayouts.end() && kept->second.key == key)
				{
					cities.insert(cities.end(), kept->second.pieces.begin(), kept->second.pieces.end());
					layouts.emplace(plotIndex, std::move(kept->second));
					continue;
				}
				const size_t first = cities.size();
				const Vec3 center{ (float(c.x) + 0.5f) * S(), (float(c.y) + 0.5f) * S(), 0 };
				std::array<uint8_t, kLots * kLots> taken{};
				// Lots by their distance from the middle, ties broken by the city's own order.
				std::array<float, kLots * kLots> rank;
				std::array<int, kLots * kLots> order;
				for (int i = 0; i < kLots * kLots; ++i)
				{
					const float dx = float(i % kLots) - 7.5f, dy = float(i / kLots) - 7.5f;
					rank[size_t(i)] = std::max(std::abs(dx), std::abs(dy)) + std::hypot(dx, dy) * 0.3f + unitFloat(hash(c.x, c.y, i)) * 1.5f;
					order[size_t(i)] = i;
				}
				std::ranges::sort(order, [&](int a, int b) { return rank[size_t(a)] < rank[size_t(b)]; });
				// The free rectangle nearest the middle for w x h lots, turned if it fits better.
				const auto findLots = [&](int w, int h, bool& turned) -> int {
					for (const int cell : order)
						for (const bool turn : { false, true })
						{
							const int ww = turn ? h : w, hh = turn ? w : h;
							const int x0 = cell % kLots - (ww - 1) / 2, y0 = cell / kLots - (hh - 1) / 2;
							if (x0 < 0 || y0 < 0 || x0 + ww > kLots || y0 + hh > kLots)
								continue;
							bool free = true;
							for (int y = y0; y < y0 + hh && free; ++y)
								for (int x = x0; x < x0 + ww && free; ++x)
									free = !taken[size_t(y * kLots + x)];
							if (!free)
								continue;
							for (int y = y0; y < y0 + hh; ++y)
								for (int x = x0; x < x0 + ww; ++x)
									taken[size_t(y * kLots + x)] = 1;
							turned = turn;
							return x0 + y0 * kLots;
						}
					return -1;
				};
				const auto lotsCenter = [&](int at, int w, int h, bool turned) {
					const float lx = (float(at % kLots) + float(turned ? h : w) / 2 - kLots / 2.0f) * lot;
					const float ly = (float(at / kLots) + float(turned ? w : h) / 2 - kLots / 2.0f) * lot;
					Vec3 p{ center.x + lx * c0 - ly * s0, center.y + lx * s0 + ly * c0, 0 };
					p.z = ground(p.x, p.y);
					return p;
				};
				uint32_t n = 0;
				const auto yawFor = [&](bool turned) {
					return map.cityTwist + (turned ? std::numbers::pi_v<float> / 2 : 0) + float(hash(c.x, c.y, int(500 + n++)) >> 31) * std::numbers::pi_v<float>;
				};
				for (const CityBuildingView& b : c.buildings)
				{
					if (b.water || b.model < 0 || size_t(b.model) >= map.models.size())
						continue;
					const ModelArt& art = map.models[size_t(b.model)];
					LoadedModel* lm = model(art.nif, art.kfm);
					bool turned = false;
					const int at = lm ? findLots(b.width, b.height, turned) : -1;
					if (at < 0)
						continue;
					Instance inst = placeStatic(lm, placement(lotsCenter(at, b.width, b.height, turned), yawFor(turned), scale * art.scale));
					inst.team = teamColor(c.owner);
					cities.push_back(std::move(inst));
				}
				const std::vector<int>* set = c.houseSet >= 0 && size_t(c.houseSet) < map.houseSets.size() ? &map.houseSets[size_t(c.houseSet)] : nullptr;
				for (int g = 0; set && !set->empty() && g < c.generics; ++g)
				{
					const CityHouseArt& house = map.cityHouses[size_t((*set)[hash(c.x, c.y, 1000 + g) % set->size()])];
					LoadedModel* lm = model(house.nif, {});
					if (!lm)
						continue;
					buildParts(*lm);
					const auto part = std::ranges::find_if(lm->parts, [&](const LoadedModel::Part& p) {
						return toLowerAscii(lm->model.nodes[size_t(p.node)].name) == toLowerAscii(house.node);
					});
					if (part == lm->parts.end())
						continue;
					bool turned = false;
					const int at = findLots(house.width, house.height, turned);
					if (at < 0)
						break; // the city is full
					const Vec3 middle = part->bounds.center();
					Instance inst = placeStatic(lm, placement(lotsCenter(at, house.width, house.height, turned), yawFor(turned), scale),
						translation({ -middle.x, -middle.y, 0 }), &part->visible, &part->bounds);
					inst.team = teamColor(c.owner);
					cities.push_back(std::move(inst));
				}
				for (const CityWallView& w : c.walls)
					buildWalls(c, w, taken, lot, c0, s0, center);
				placeShoreBuildings(c, center, scale);
				layouts[plotIndex] = { std::move(key), { cities.begin() + std::ptrdiff_t(first), cities.end() } };
			}
			cityLayouts = std::move(layouts);
		}

		// Harbours, lighthouses and the like stand on the shore, towards the water around the
		// city, side by side along it.
		void placeShoreBuildings(const CityView& c, Vec3 center, float scale)
		{
			static constexpr int kDx[8] = { 0, 1, 1, 1, 0, -1, -1, -1 }, kDy[8] = { 1, 1, 0, -1, -1, -1, 0, 1 };
			Vec3 toWater{};
			for (int d = 0; d < 8; ++d)
				if (const PlotView* q = map.plot(c.x + kDx[d], c.y + kDy[d]); q && q->water)
					toWater += normalize(Vec3{ float(kDx[d]), float(kDy[d]), 0 });
			if (length(toWater) < 1e-3f)
				return;
			toWater = normalize(toWater);
			const Vec3 along{ -toWater.y, toWater.x, 0 };
			int k = 0;
			for (const CityBuildingView& b : c.buildings)
			{
				if (!b.water || b.model < 0 || size_t(b.model) >= map.models.size())
					continue;
				const ModelArt& art = map.models[size_t(b.model)];
				LoadedModel* lm = model(art.nif, art.kfm);
				if (!lm)
					continue;
				const float side = float((k + 1) / 2) * (k % 2 ? 1.0f : -1.0f) * 0.28f;
				++k;
				Vec3 at = center + toWater * (0.5f * S()) + along * (side * S());
				at.z = std::max(ground(at.x, at.y), 0.0f);
				const float yaw = std::atan2(toWater.y, toWater.x) - std::numbers::pi_v<float> / 2;
				Instance inst = placeStatic(lm, placement(at, yaw, scale * b.scale * art.scale));
				inst.team = teamColor(c.owner);
				cities.push_back(std::move(inst));
			}
		}

		// The walls round the lots in use (one lot out), turned with the city: posts at the
		// corners and between the pieces, each side filled with the longest pieces that fit
		// (4, 2 and 1 lengths: a side of the whole layout takes one of each), the gate in the
		// middle of the side facing south.
		template <size_t N>
		void buildWalls(const CityView& c, const CityWallView& w, const std::array<uint8_t, N>& taken, float lot, float c0, float s0, Vec3 center)
		{
			constexpr int kLots = 16;
			int x0 = kLots, y0 = kLots, x1 = -1, y1 = -1;
			for (int i = 0; i < kLots * kLots; ++i)
				if (taken[size_t(i)])
				{
					x0 = std::min(x0, i % kLots);
					x1 = std::max(x1, i % kLots);
					y0 = std::min(y0, i / kLots);
					y1 = std::max(y1, i / kLots);
				}
			if (x1 < 0)
				return;
			x0 = std::max(x0 - 1, 0);
			y0 = std::max(y0 - 1, 0);
			x1 = std::min(x1 + 2, kLots);
			y1 = std::min(y1 + 2, kLots);
			const auto world = [&](float lx, float ly) {
				lx = (lx - kLots / 2.0f) * lot;
				ly = (ly - kLots / 2.0f) * lot;
				return Vec3{ center.x + lx * c0 - ly * s0, center.y + lx * s0 + ly * c0, 0 };
			};
			const auto pieceFile = [&](const char* length) {
				std::string f = w.wall;
				if (const size_t at = f.find("%1"); at != std::string::npos)
					f.replace(at, 2, length);
				return f;
			};
			struct Piece
			{
				LoadedModel* model;
				float length;
			};
			std::vector<Piece> pieces;
			for (const char* length : { "04", "02", "01" })
				if (LoadedModel* lm = model(pieceFile(length), {}); lm && !lm->bounds.empty())
					pieces.push_back({ lm, lm->bounds.max.x - lm->bounds.min.x });
			LoadedModel* post = model(w.post, {});
			LoadedModel* gate = w.gate.empty() ? nullptr : model(w.gate, {});
			if (pieces.empty())
				return;
			const float postWidth = post && !post->bounds.empty() ? post->bounds.max.x - post->bounds.min.x : 0;
			const Vec3 corners[4] = { world(float(x0), float(y0)), world(float(x1), float(y0)), world(float(x1), float(y1)), world(float(x0), float(y1)) };
			// The side whose middle is southmost gets the gate.
			int gateSide = 0;
			for (int s = 1; s < 4; ++s)
				if ((corners[s] + corners[(s + 1) % 4]).y < (corners[gateSide] + corners[(gateSide + 1) % 4]).y)
					gateSide = s;
			const auto put = [&](LoadedModel* lm, Vec3 at, float yaw) {
				at.z = ground(at.x, at.y);
				const Vec3 mid = lm->bounds.center();
				Instance inst = placeStatic(lm, placement(at, yaw, 1), translation({ -mid.x, -mid.y, 0 }));
				inst.team = teamColor(c.owner);
				cities.push_back(std::move(inst));
			};
			for (int s = 0; s < 4; ++s)
			{
				const Vec3 a = corners[s], b = corners[(s + 1) % 4];
				const float sideLength = length(b - a);
				const Vec3 dir = (b - a) * (1 / std::max(sideLength, 1e-3f));
				const float yaw = std::atan2(dir.y, dir.x);
				if (post)
					put(post, a, yaw);
				float done = postWidth / 2;
				while (true)
				{
					const auto fit = std::ranges::find_if(pieces, [&](const Piece& p) { return done + p.length + postWidth / 2 <= sideLength + 1; });
					if (fit == pieces.end())
						break;
					put(fit->model, a + dir * (done + fit->length / 2), yaw);
					done += fit->length;
					if (post)
						put(post, a + dir * (done + postWidth / 2), yaw);
					done += postWidth;
				}
				if (gate && s == gateSide)
					put(gate, a + dir * (sideLength / 2), yaw);
			}
		}

		Vec3 motionPoint(const Motion& m, float delay = 0) const
		{
			const float t = m.duration > 0 ? std::clamp((time - delay - m.start) / m.duration, 0.0f, 1.0f) : 1.0f;
			return lerp(m.from, m.to, t);
		}

		// The leg of a unit's walk on now (a figure delay seconds behind the first), if any.
		const Motion* activeMotion(uint64_t unit, float delay = 0) const
		{
			const auto it = motions.find(unit);
			if (it != motions.end())
				for (const Motion& m : it->second)
					if (time - delay < m.start + m.duration)
						return &m;
			return nullptr;
		}

		void finishMotions()
		{
			for (auto& [unit, route] : motions)
				while (!route.empty() && time >= route.front().start + route.front().duration + route.front().spread)
					route.pop_front();
			std::erase_if(motions, [](const auto& entry) { return entry.second.empty(); });
		}

		// Units: the plot's centre unit, as many figures as its formation has.
		void buildUnits()
		{
			units.clear();
			finishMotions();
			// When each battle began, on this view's clock.
			for (const BattleView& b : map.battles)
				if (!battleStarts.contains(b.serial))
				{
					battleStarts[b.serial] = time - b.elapsed;
					// Collateral damage: a blast round the defender for each other unit it hit,
					// one after another as the rounds begin.
					const float rounds = time - b.elapsed + float(kBattleSetupSlices) * kBattleSliceSeconds;
					for (int k = 0; k < std::min(b.collateral, 6); ++k)
					{
						const float a = unitFloat(hash(b.defenderX, b.defenderY, int(b.serial) * 7 + k)) * 2 * std::numbers::pi_v<float>;
						pendingEffects.push_back({ rounds + 0.3f * float(k), "EFFECT_EXPLOSION_DIRT", b.defenderX, b.defenderY, 0.5f + 0.28f * std::cos(a),
							0.5f + 0.28f * std::sin(a) });
					}
				}
			std::erase_if(battleStarts, [&](const auto& e) { return time - e.second > 300; });
			std::unordered_set<int> cityPlots;
			for (const CityView& c : map.cities)
				cityPlots.insert(c.x + c.y * map.width);
			for (const UnitView& u : map.units)
			{
				// DirectionTypes: north 0, clockwise in eighths. Models face -Y. (Acting at another
				// plot: facing it.)
				float yaw = -float(u.direction) * std::numbers::pi_v<float> / 4 + std::numbers::pi_v<float>;
				if (u.action >= 0 && u.actionX >= 0 && (u.actionX != u.x || u.actionY != u.y))
				{
					float dx = float(u.actionX - u.x);
					if (map.wrapX && std::abs(dx) > float(map.width) / 2)
						dx -= std::copysign(float(map.width), dx);
					yaw = std::atan2(float(u.actionY - u.y), dx) + std::numbers::pi_v<float> / 2;
				}
				Vec3 center{ (float(u.x) + 0.5f) * S(), (float(u.y) + 0.5f) * S(), 0 };
				if (cityPlots.contains(u.x + u.y * map.width))
					center.y -= 0.3f * S();
				// A unit seen on another plot before walks there, plot by plot through those the game
				// moved it through (a whole Go To this turn is one game update: the snapshots alone
				// see only its end). As the original's: the plots one order walked, in a second
				// (moveSeconds) at an even pace; a step the game did not tell, a short one.
				const uint64_t key = uint64_t(uint32_t(u.owner)) << 32 | uint32_t(u.id);
				if (const auto it = unitPositions.find(key); it != unitPositions.end() && u.id >= 0)
				{
					auto& route = motions[key];
					Vec3 from = route.empty() ? it->second : route.back().to;
					float start = route.empty() ? time : route.back().start + route.back().duration;
					std::vector<Vec3> legs;
					auto add = [&](Vec3 to) {
						Vec3 d = to - from;
						if (map.wrapX && std::abs(d.x) > worldW() / 2)
							d.x -= std::copysign(worldW(), d.x);
						const float plots = length(d) / S();
						if (plots > 0.05f && plots < 3.5f)
						{
							legs.push_back(d);
							from = from + d;
						}
					};
					for (const UnitView::MoveStep& step : u.moveSteps)
					{
						Vec3 to{ (float(step.x) + 0.5f) * S(), (float(step.y) + 0.5f) * S(), 0 };
						if (cityPlots.contains(step.x + step.y * map.width))
							to.y -= 0.3f * S();
						add(to);
					}
					add(center);
					float total = 0;
					for (const Vec3& d : legs)
						total += length(d);
					Vec3 at = route.empty() ? it->second : route.back().to;
					const float orderStart = start;
					for (const Vec3& d : legs)
					{
						const float duration = u.moveSeconds > 0 ? u.moveSeconds * length(d) / total : 0.25f + 0.3f * length(d) / S();
						// (the last figure up to 0.7 s behind, the more of them the more: a stack's)
						const float figures = float(std::max<size_t>(u.models.size(), 1));
						route.push_back({ at, at + d, start, duration, u.moveSeconds > 0 ? 0.7f * (1 - 1 / figures) : 0, orderStart, u.moveSeconds });
						at = at + d;
						start += duration;
					}
					if (route.empty())
						motions.erase(key);
				}
				// (one already resting when first seen, as in a loaded game, is fortified already)
				if (!u.resting)
					restingSince.erase(key);
				else if (!restingSince.contains(key))
					restingSince[key] = unitPositions.contains(key) ? time : time - 1000;
				unitPositions[key] = center;
				const size_t n = u.models.size();
				const int columns = int(std::ceil(std::sqrt(double(n))));
				const int rows = int((n + size_t(columns) - 1) / size_t(std::max(columns, 1)));
				const float spacing = 0.12f * S();
				// (the formation's figures, then its great general and its siege tower)
				std::vector<std::pair<int, int>> figures; // model, role
				for (const int m : u.models)
					figures.push_back({ m, 0 });
				if (u.leader >= 0)
					figures.push_back({ u.leader, 1 });
				if (u.siegeTower >= 0)
					figures.push_back({ u.siegeTower, 2 });
				for (size_t i = 0; i < figures.size(); ++i)
				{
					const auto [modelIndex, role] = figures[i];
					if (modelIndex < 0 || size_t(modelIndex) >= map.models.size())
						continue;
					const ModelArt& art = map.models[size_t(modelIndex)];
					LoadedModel* lm = model(art.nif, art.kfm);
					if (!lm)
						continue;
					// (the general beside the formation, on its right; the tower ahead of it, to the left)
					const float ox = role == 1 ? (float(columns) / 2 + 0.6f) * spacing : role == 2 ? -0.25f * S()
																								: (float(int(i) % columns) - float(columns - 1) / 2) * spacing;
					const float oy = role ? 0.0f : (float(int(i) / columns) - float(rows - 1) / 2) * spacing;
					const float c = std::cos(yaw), s = std::sin(yaw);
					Instance inst;
					inst.model = lm;
					inst.figure = true;
					inst.anchor = center;
					inst.offset = { ox * c - oy * s, ox * s + oy * c, 0 };
					inst.yaw = yaw;
					// The game draws a lone figure larger (SINGLE_UNIT_GFX_EXTRA_SCALE).
					inst.scale = art.scale * (n == 1 ? 1.25f : 1.0f);
					inst.center = center;
					inst.radius = lm->bounds.radius() * inst.scale * 2.5f + spacing * float(columns);
					inst.team = teamColor(u.shownOwner);
					inst.animated = lm->idle.has_value();
					inst.phase = unitFloat(hash(u.x, u.y, int(i))) * 10;
					inst.plot = u.x + u.y * map.width;
					inst.unit = key;
					inst.resting = u.resting;
					inst.ranged = art.ranged;
					// (the general fights as the first figure: it falls with the last)
					inst.battle = u.battle, inst.attacking = u.attacking, inst.figureIndex = role ? 0 : int(i), inst.role = role;
					inst.work = u.work;
					inst.foundAt = u.founded >= 0 ? time - u.founded : -1;
					// (as hurt as the unit: its model's states share the damage, the last the worst)
					if (int states = 0; u.health < 1)
					{
						while (states < 4 && !lm->damage[states].empty())
							++states;
						inst.damage = std::clamp(int((1 - u.health) * float(states + 1)), 0, states);
					}
					inst.air = u.air;
					if (u.action >= 0)
					{
						inst.action = u.action, inst.actionStart = time - u.actionSince, inst.actionLength = u.actionLength;
						inst.actionX = u.actionX, inst.actionY = u.actionY;
						// (its effect at the plot it aims at: once; a plane's when it is there)
						const uint64_t once = key * 1000003u + uint64_t(std::lround(inst.actionStart * 10));
						if (i == 0 && !u.actionEffect.empty() && actionsShown.insert(once).second)
						{
							if (u.air && (u.actionX != u.x || u.actionY != u.y))
								pendingEffects.push_back({ inst.actionStart + kFlightOut + strikeTime(*lm) / 2, u.actionEffect, u.actionX, u.actionY });
							else
								triggerEffectByType(u.actionEffect, u.actionX, u.actionY);
						}
					}
					if (u.work < 0)
						workSince.erase(key);
					else if (const auto w = workSince.find(key); w == workSince.end() || w->second.first != u.work)
						workSince[key] = { u.work, time };
					inst.soundAt = { float(u.x) + 0.5f, float(u.y) + 0.5f, 0 };
					// Fighting, as the original shows it (Wine, close up): the defenders stand where they
					// are; the attackers run up to them, each figure to the one across (to close to fight
					// hand to hand, a little short of its plot), or, when every round is ranged (both
					// shoot), half way; to one giving itself up, half way.
					if (u.battle >= 0 && size_t(u.battle) < map.battles.size() && u.attacking && role != 1)
					{
						const BattleView& b = map.battles[size_t(u.battle)];
						Vec3 d{ float(b.defenderX - b.attackerX) * S(), float(b.defenderY - b.attackerY) * S(), 0 };
						if (map.wrapX && std::abs(d.x) > worldW() / 2)
							d.x -= std::copysign(worldW(), d.x);
						const bool shooting = !b.rounds.empty() && std::all_of(b.rounds.begin(), b.rounds.end(), [](const BattleRoundView& r) { return r.ranged; });
						if (const float len = length(d); len > 0)
						{
							// (shooting, it stops twice its art's battle distance short, in plots: the
							// original's tanks, catapults and cannons, 0.5, fire from their plot, its
							// musketmen, 0.35, come a third of the way)
							const float shootFrom = std::clamp(2 * art.battleDistance, 0.0f, 1.0f) * S();
							inst.step = d * (role == 2 ? 0.5f : b.surrender ? 0.45f : shooting ? std::max(len - shootFrom, 0.0f) / len : 1 - (1 - kBattleCloseIn) * S() / len);
						}
					}
					if (!art.shadow.empty())
						if ((inst.shadow = model(art.shadow, {})))
						{
							inst.shadowNode = art.shadowNode.empty() ? -1 : lm->model.findNode(art.shadowNode);
							inst.shadowScale = art.shadowScale;
						}
					units.push_back(std::move(inst));
				}
			}
		}

		// ------------------------------------------------------------ drawing

		// Wrapped maps: which copy of the world (offset in X) is nearest the camera.
		float wrapOffset(float x) const
		{
			if (!map.wrapX)
				return 0;
			const float w = worldW();
			return std::round((renderer.camera().target.x - x) / w) * w;
		}

		Vec3 flagPosition(const UnitView& u) const
		{
			const uint64_t key = uint64_t(uint32_t(u.owner)) << 32 | uint32_t(u.id);
			Vec3 at{ (float(u.x) + 0.5f) * S(), (float(u.y) + 0.5f) * S(), 0 };
			if (const auto it = unitPositions.find(key); it != unitPositions.end())
				at = it->second;
			const float yaw = renderer.camera().yaw, extent = S() * 0.5f;
			at += Vec3{ -std::sin(yaw), std::cos(yaw), 0 } * (map.flagOffset.x * extent);
			at += Vec3{ -std::cos(yaw), -std::sin(yaw), 0 } * (map.flagOffset.y * extent);
			at.z = ground(at.x, at.y);
			at.x += wrapOffset(at.x);
			return at;
		}

		struct FlagDraw { Vec3 top, clothBottom; bool medallions = false; };
		std::map<int, FlagDraw> flagDraws; // projected by the overlay for health and stack counts

		float flagZoom() const { return fader(map.flagScale, 1); }

		// A CIV4DetailManager fader at the camera's distance: between its keys, the nearest
		// one's beyond them.
		float fader(const std::vector<std::pair<float, float>>& keys, float none) const
		{
			if (keys.empty())
				return none;
			const float distance = renderer.camera().distance;
			const auto upper = std::ranges::upper_bound(keys, distance, {}, &std::pair<float, float>::first);
			if (upper == keys.begin())
				return upper->second;
			if (upper == keys.end())
				return keys.back().second;
			const auto& lower = *std::prev(upper);
			const float t = (distance - lower.first) / std::max(upper->first - lower.first, 1.0f);
			return lower.second + (upper->second - lower.second) * t;
		}

		// Over the flag, a medallion for each unit on the plot (up to the model's 7): white, or
		// for each selected unit how it can move (green: not moved yet, yellow: moved
		// and can go on, red: cannot move).
		bool drawMedallions(const UnitView& u, const Affine& place)
		{
			if (map.flagMedallions < 0 || size_t(map.flagMedallions) >= map.models.size())
				return false;
			const ModelArt& art = map.models[size_t(map.flagMedallions)];
			LoadedModel* lm = model(art.nif, art.kfm, nullptr, true);
			if (!lm || lm->medallions.empty())
				return false;
			// (the states' rows of Medallions.dds: "off" shows its empty row, "fortify" the white
			// one the original shows for every unit not selected, its own and others')
			const int shown = std::clamp(int(u.medallions.size()), 1, 7);
			anim::Pose pose = anim::Pose::bind(lm->model);
			for (int k = 1; k <= shown; ++k)
			{
				const uint32_t state = size_t(k) <= u.medallions.size() ? u.medallions[size_t(k - 1)] : 3;
				if (const auto it = lm->medallions.find(uint32_t(10 * k) + state); it != lm->medallions.end())
					it->second.apply(0, pose);
			}
			// (the billboard's other pieces are the winged coin of a unit that can be promoted)
			const auto& nodes = lm->model.nodes;
			for (size_t n = 0; n < nodes.size(); ++n)
				if (const int parent = nodes[n].parent; parent >= 0 && nodes[size_t(parent)].name.starts_with("UpgradeMedallion"))
				{
					int k = 0;
					pose.hidden[n] = std::sscanf(nodes[n].name.c_str(), "Medallion_%d", &k) == 1 ? k < 1 || k > shown : !u.promotionReady;
				}
			// (in the flag's own frame: they hang over its pole)
			auto world = lm->model.worldTransforms(pose.locals);
			for (auto& w : world)
				w = place * w;
			std::vector<scene::Material> materials;
			for (const auto& item : lm->model.items)
				materials.push_back(anim::animatedMaterial(item, pose));
			renderer.draw(*lm->gpu, world, lm->model.visibility(pose.hidden), materials);
			return true;
		}

		// The units' health bars as the original's billboards: a flat bar facing the camera, as
		// long as the unit's strength now (UnitView::healthBar: a whole Modern Armor's is
		// HEALTH_BAR_WIDTH / 2, a unit without strength has none), green from two thirds of its
		// hit points, yellow from a third, red below (measured in the original: 75 % green,
		// 66 % and 40 % yellow, 33 % red), as thick at every length (2.6 units times
		// UNIT_HEALTH_BILLBOARD_SCALE: 13 px of 3200 at the start distance); ending over the
		// plot's middle and reaching left, behind the unit (UNIT_HEALTH_BILLBOARD_RADIUS) and up the
		// screen from the ground (..._Z): in the scene, so what stands in front of it hides it.
		scene::Model barModel;
		std::unique_ptr<GpuModel> barGpu;
		bool barTried = false;

		void drawHealthBars(const Frustum& frustum)
		{
			if (!barTried)
			{
				barTried = true;
				barModel.nodes.push_back({ "HealthBar" });
				scene::DrawItem bar;
				bar.node = 0;
				// (its right end at its origin, reaching left along x, z its height, facing -y)
				for (const auto [x, z] : { std::pair{ -1.0f, -0.5f }, { 0.0f, -0.5f }, { 0.0f, 0.5f }, { -1.0f, 0.5f } })
					bar.vertices.push_back({ { x, 0, z }, { 0, -1, 0 } });
				bar.indices = { 0, 1, 2, 0, 2, 3 };
				bar.material.lightingMode = 0;
				bar.material.cull = scene::Cull::None;
				barModel.items.push_back(std::move(bar));
				barGpu = renderer.upload(barModel, assets, AssetLocation{});
			}
			if (!barGpu)
				return;
			const Camera& camera = renderer.camera();
			const Vec3 eye = camera.eye();
			Vec3 toward{ eye.x - camera.target.x, eye.y - camera.target.y, 0 };
			const float length = std::sqrt(toward.x * toward.x + toward.y * toward.y);
			toward = length > 1e-3f ? toward * (1 / length) : Vec3{ 0, -1, 0 };
			const Vec3 right{ -toward.y, toward.x, 0 }, up = camera.up(), facing = cross(up, right); // (it faces the camera)
			const float unit = S() / 180; // (the faders are in the original's units: 180 a plot)
			const float height = 2.6f * fader(map.healthScale, 2) * unit;
			const float radius = fader(map.healthRadius, -60) * unit, z = fader(map.healthZ, 75) * unit;
			std::vector<scene::Material> material(1, barModel.items[0].material);
			for (const UnitView& u : map.units)
			{
				const int plot = u.x + u.y * map.width;
				const float health = std::clamp(u.health, 0.0f, 1.0f), width = u.healthBar * unit;
				// (none on a unit fighting, while its battle is shown: its damage is already the
				// battle's end, and the original shows no bar until the fight is over; seen in Wine)
				if ((fog && !map.plots[size_t(plot)].visible) || width <= 0 || u.battle >= 0)
					continue;
				Vec3 at{ (float(u.x) + 0.5f) * S(), (float(u.y) + 0.5f) * S(), 0 };
				if (const auto it = unitPositions.find(uint64_t(uint32_t(u.owner)) << 32 | uint32_t(u.id)); it != unitPositions.end())
					at = it->second;
				at = at + toward * radius;
				at.z = ground(at.x, at.y);
				at = at + up * z; // (..._Z along the screen's up, as a billboard is moved)
				at.x += wrapOffset(at.x);
				if (!frustum.sphere(at, width))
					continue;
				Affine place;
				place.translation = at;
				for (int r = 0; r < 3; ++r)
				{
					place.rotation.m[size_t(r)][0] = (r == 0 ? right.x : r == 1 ? right.y : right.z) * width;
					place.rotation.m[size_t(r)][1] = r == 0 ? facing.x : r == 1 ? facing.y : facing.z;
					place.rotation.m[size_t(r)][2] = (r == 0 ? up.x : r == 1 ? up.y : up.z) * height;
				}
				const nif::Color3 colour = health >= 2 / 3.0f ? nif::Color3{ 0, 1, 0 } : health >= 1 / 3.0f ? nif::Color3{ 1, 1, 0 } : nif::Color3{ 1, 0, 0 };
				material[0].emissive = colour;
				const Affine world[] = { place };
				const uint8_t visible[] = { 1 };
				renderer.draw(*barGpu, world, visible, material);
			}
		}

		void drawFlags(const Frustum& frustum)
		{
			flagDraws.clear();
			// BtS drops the flags at its last zoom step, where the camera reaches its minimum.
			if (renderer.camera().distance <= map.cameraMinDistance + 0.5f || renderer.camera().distance >= 22 * S())
				return;
			for (const UnitView& u : map.units)
			{
				const int plot = u.x + u.y * map.width;
				if ((fog && !map.plots[size_t(plot)].visible) || u.shownOwner < 0 || size_t(u.shownOwner) >= map.players.size())
					continue;
				// (a unit fighting has no flag, nor its health light, while its battle is shown: the
				// original's, attacker and defender, Combat Zoom on or off; seen in Wine)
				if (u.battle >= 0)
					continue;
				const PlayerView& owner = map.players[size_t(u.shownOwner)];
				const Vec3 at = flagPosition(u);
				if (owner.flagModel < 0 || size_t(owner.flagModel) >= map.models.size() || !frustum.sphere(at, 2 * S()))
					continue;
				const ModelArt& art = map.models[size_t(owner.flagModel)];
				const auto* decal = flagArt.flag(owner.flagDecal, owner.primary, owner.secondary, owner.whiteFlag);
				LoadedModel* lm = decal ? model(art.nif, art.kfm, decal) : nullptr;
				if (!lm || lm->bounds.empty())
					continue;
				const float scale = art.scale * flagZoom();
				const Affine place = placement(at + Vec3{ 0, 0, -lm->bounds.min.z * scale },
					renderer.camera().yaw + std::numbers::pi_v<float> / 2, scale);
				if (u.selected && map.flagGlow >= 0 && size_t(map.flagGlow) < map.models.size())
				{
					const ModelArt& glow = map.models[size_t(map.flagGlow)];
					if (LoadedModel* g = model(glow.nif, glow.kfm))
					{
						std::vector<Affine> world;
						for (const auto& w : g->bindWorld)
							world.push_back(place * w);
						renderer.draw(*g->gpu, world, g->bindVisible);
					}
				}
				anim::Pose pose = anim::Pose::bind(lm->model);
				if (lm->idle && !frozen)
					lm->idle->apply(time + unitFloat(hash(u.x, u.y, u.id)) * lm->idle->duration(), pose);
				auto world = lm->model.worldTransforms(pose.locals);
				for (auto& w : world)
					w = place * w;
				renderer.draw(*lm->gpu, world, lm->model.visibility(pose.hidden));
				const float height = lm->bounds.max.z - lm->bounds.min.z;
				flagDraws[plot] = { place.point({ 0, 0, lm->bounds.max.z }), place.point({ 0, 0, lm->bounds.max.z - height * 0.47f }),
					drawMedallions(u, place) };
			}
		}

		TerrainConstants terrainConstants(const MapView::Options& options) const
		{
			TerrainConstants tc{};
			tc.mapW = map.width;
			tc.mapH = map.height;
			tc.wrapX = map.wrapX;
			tc.wrapY = map.wrapY;
			tc.plotSize = S();
			tc.terrainCount = uint32_t(map.terrains.size());
			tc.showGrid = options.grid;
			tc.showBorders = options.borders;
			tc.showFog = options.fog;
			tc.waterZ = 0;
			for (size_t t = 0; t < kGridTerrains; ++t)
				std::memcpy(tc.gridLines[t], gridLines.empty() ? kDefaultGridLine.data() : gridLines[std::min(t, gridLines.size() - 1)].data(), sizeof(tc.gridLines[t]));
			for (size_t i = 0; i < std::min(map.players.size(), kMaxPlayers); ++i)
			{
				const Color& c = map.players[i].primary;
				tc.playerColors[i][0] = c.r;
				tc.playerColors[i][1] = c.g;
				tc.playerColors[i][2] = c.b;
				tc.playerColors[i][3] = 1;
			}
			return tc;
		}

		// The ground's mesh, in every copy of the map in view (wrapping), with a pipeline whose
		// fragment shader takes the terrain's seven textures.
		void drawGround(const Frustum& frustum, SDL_GPUGraphicsPipeline* pipeline, TerrainConstants& tc)
		{
			std::vector<float> copies{ 0 };
			if (map.wrapX)
				copies = { -worldW(), 0, worldW() };
			const auto visibleCopy = [&](float dx) {
				const Vec3 c{ worldW() / 2 + dx, worldH() / 2, 0 };
				return frustum.sphere(c, length(Vec3{ worldW(), worldH(), 0 }) / 2 + map.peakScale * S());
			};
			if (terrainSamplerGeneration != renderer.samplerGeneration())
				makeTerrainSamplers();
			renderer.reserveSamplers(7);
			SDL_GPURenderPass* pass = renderer.pass();
			renderer.bindPipeline(pipeline);
			const SDL_GPUTextureSamplerBinding textures[7] = { { plotTex->texture, sampler }, { atlasTex->texture, sampler },
				{ decalTex->texture, detailWrap }, { detailTex->texture, detailWrap }, { cellTex->texture, sampler }, { borderTex->texture, sampler },
				{ borderArtTex->texture, sampler } };
			SDL_BindGPUFragmentSamplers(pass, 0, textures, 7);
			const SDL_GPUBufferBinding vbb{ vb->buffer, 0 }, ibb{ ib->buffer, 0 };
			SDL_BindGPUVertexBuffers(pass, 0, &vbb, 1);
			SDL_BindGPUIndexBuffer(pass, &ibb, SDL_GPU_INDEXELEMENTSIZE_32BIT);
			for (const float dx : copies)
			{
				if (!visibleCopy(dx))
					continue;
				tc.offset[0] = dx;
				SDL_PushGPUVertexUniformData(renderer.commands(), 1, &tc, sizeof(tc));
				SDL_PushGPUFragmentUniformData(renderer.commands(), 1, &tc, sizeof(tc));
				SDL_DrawGPUIndexedPrimitives(pass, indexCount, 1, 0, 0, 0);
			}
		}

		void drawTerrain(const Frustum& frustum, const MapView::Options& options)
		{
			if (!vb || !atlasTex || !plotTex || !detailTex || !decalTex || !cellTex || !borderTex || !borderArtTex)
				return;
			TerrainConstants tc = terrainConstants(options);
			drawGround(frustum, groundPipelinesFor(renderer.passSamples()).terrain, tc);
		}

		// The culture borders (Options::borders): over the ground above the water's level (pass
		// 1, before what stands on it) or under it (pass 2, after the water).
		void drawBorders(const Frustum& frustum, const MapView::Options& options, uint32_t part)
		{
			if (!options.borders || !vb || !atlasTex || !plotTex || !detailTex || !decalTex || !cellTex || !borderTex || !borderArtTex)
				return;
			TerrainConstants tc = terrainConstants(options);
			tc.showBorders = part;
			drawGround(frustum, groundPipelinesFor(renderer.passSamples()).border, tc);
		}

		void drawWater(const Frustum& frustum, const MapView::Options& options)
		{
			if (!waterVb || !waterTex || !plotTex)
				return;
			// The texture's two placements (Water.fx: mtxWaterTextureMat, ...Mat2): the plane's texture
			// scaling in texture units a world unit, scrolled at its rates, the second the other way
			// (measured in the original: two patterns drifting apart east and west, a little north and
			// south, each about 0.0048 texture units a second: the rates are per frame at 30 a second).
			const float k = map.water.scale, t = time * kWaterFrames;
			WaterConstants wc{};
			wc.uv1[0] = k, wc.uv1[3] = k;
			wc.uv2[0] = k, wc.uv2[3] = k;
			wc.scroll[0] = map.water.rateU * t, wc.scroll[1] = map.water.rateV * t;
			wc.scroll[2] = -map.water.rateU * t, wc.scroll[3] = -map.water.rateV * t;
			for (float& v : wc.scroll)
				v -= std::floor(v);
			wc.alpha = map.water.alpha;
			wc.mapW = map.width;
			wc.mapH = map.height;
			wc.wrapX = map.wrapX;
			wc.wrapY = map.wrapY;
			wc.plotSize = S();
			wc.showFog = options.fog;
			wc.shore = kShoreDepth * S();
			wc.shoreWidth = kShoreWidth * S();

			std::vector<float> copies{ 0 };
			if (map.wrapX)
				copies = { -worldW(), 0, worldW() };
			renderer.reserveSamplers(2);
			SDL_GPURenderPass* pass = renderer.pass();
			renderer.bindPipeline(groundPipelinesFor(renderer.passSamples()).water);
			const SDL_GPUTextureSamplerBinding textures[2] = { { waterTex->texture, waterSampler }, { plotTex->texture, sampler } };
			SDL_BindGPUFragmentSamplers(pass, 0, textures, 2);
			const SDL_GPUBufferBinding vbb{ waterVb->buffer, 0 }, ibb{ waterIb->buffer, 0 };
			SDL_BindGPUVertexBuffers(pass, 0, &vbb, 1);
			SDL_BindGPUIndexBuffer(pass, &ibb, SDL_GPU_INDEXELEMENTSIZE_32BIT);
			for (const float dx : copies)
			{
				if (!frustum.sphere({ worldW() / 2 + dx, worldH() / 2, 0 }, length(Vec3{ worldW(), worldH(), 0 }) / 2))
					continue;
				wc.offset[0] = dx;
				SDL_PushGPUVertexUniformData(renderer.commands(), 1, &wc, sizeof(wc));
				SDL_PushGPUFragmentUniformData(renderer.commands(), 1, &wc, sizeof(wc));
				SDL_DrawGPUIndexedPrimitives(pass, waterIndexCount, 1, 0, 0, 0);
			}
		}

		static constexpr float kWaterFrames = 30; // a second: the plane's scroll rates are per frame
		// Of a plot: how deep the ground is under the shore, and over how much more depth the water
		// fades in there. Measured in the original: on a straight coast the water ends about a quarter
		// of a plot inside the water plot (the sand between is the coast's ground), and a lake of one
		// plot is water about 0.56 of a plot across; the ground's levels (baseLevel, blended between
		// plot centres) are that deep there.
		static constexpr float kShoreDepth = 0.042f;
		static constexpr float kShoreWidth = 0.003f;

		// Units grow as the camera moves away, as in the game (ALLOW_UNIT_SCALE_ADJUST_ON_ZOOM).
		float figureZoom() const { return std::clamp(1 + (renderer.camera().distance / S() - 5) / 8, 1.0f, 2.2f); }

		// Hidden by the fog of war: units the active team does not see; the rest where nothing
		// around has been seen (next to what has, it fades out with the ground: the shader
		// darkens the models as the terrain).
		bool fogged(const Instance& inst, bool needsSight) const
		{
			if (!fog || inst.plot < 0 || size_t(inst.plot) >= map.plots.size())
				return false;
			if (needsSight)
				return !map.plots[size_t(inst.plot)].visible;
			const int W = map.width, H = map.height, x = inst.plot % W, y = inst.plot / W;
			for (int dy = -1; dy <= 1; ++dy)
				for (int dx = -1; dx <= 1; ++dx)
				{
					int px = x + dx, py = y + dy;
					if (map.wrapX)
						px = (px + W) % W;
					if (map.wrapY)
						py = (py + H) % H;
					if (px >= 0 && py >= 0 && px < W && py < H && map.plots[size_t(px) + size_t(py) * size_t(W)].revealed)
						return false;
				}
			return true;
		}

		bool fog = false;

		// A fighting figure at its battle's time: the clip it plays and where in it (none: as
		// usual), whether it is gone (it died and faded), how far the attacker has stepped up.
		struct BattlePose
		{
			bool fighting = false, gone = false;
			const anim::Sequence* clip = nullptr;
			float at = 0, step = 0;
			bool dying = false;
		};
		// The battle's first rounds that are first strikes, one side's alone (CvUnit::planBattle: as
		// many as the strikes one has more, or the other's figures killed in them by that many);
		// the ranged rounds after them (both sides ranged) are both sides'.
		static int firstStrikeRounds(const BattleView& b)
		{
			const int more = std::abs(b.firstStrikes[0] - b.firstStrikes[1]);
			if (more == 0)
				return 0;
			const int struck = b.firstStrikes[0] > b.firstStrikes[1] ? 1 : 0;
			int killed = 0, ranged = 0;
			for (const BattleRoundView& r : b.rounds)
				if (r.ranged)
					killed += r.killed[struck], ++ranged;
			return std::min(std::max(more, killed / more), ranged);
		}
		BattlePose battlePose(const Instance& inst) const
		{
			BattlePose p;
			if (inst.battle < 0 || size_t(inst.battle) >= map.battles.size())
				return p;
			const BattleView& b = map.battles[size_t(inst.battle)];
			const auto began = battleStarts.find(b.serial);
			if (began == battleStarts.end() || (b.rounds.empty() && !b.surrender))
				return p;
			const float slice = kBattleSliceSeconds, t = time - began->second, end = float(b.slices) * slice;
			if (t < 0 || t > end + 1)
				return p; // (over: as usual again)
			p.fighting = true;
			if (b.surrender)
			{
				// Giving itself up, as the original shows it: the attacker steps up and stays; the
				// other surrenders (its clip, else it stands), fades out over the last half second (its
				// XFadeOut, else it is gone at the end) and is not seen again until the game takes it.
				constexpr float fade = 0.5f;
				if (inst.attacking)
				{
					p.step = std::clamp(t / 0.6f, 0.0f, 1.0f);
					return p;
				}
				const LoadedModel& lm = *inst.model;
				if (t >= end - fade && lm.fadeOut)
				{
					p.clip = &*lm.fadeOut, p.at = std::min((t - (end - fade)) / fade * lm.fadeOut->duration(), lm.fadeOut->duration());
					p.gone = t >= end;
				}
				else if (t >= end)
					p.gone = true;
				else if (lm.surrender)
					p.clip = &*lm.surrender, p.at = std::min(t, lm.surrender->duration());
				return p;
			}
			// (the attackers run up while it is set up; they stay to its end, when the survivors stand
			// in their places again, as the original's do)
			p.step = std::clamp(t / kBattleRunUp, 0.0f, 1.0f);
			// (the siege tower: rolled up with them, it stands there to the end)
			if (inst.role == 2)
			{
				p.gone = t >= end;
				return p;
			}
			const LoadedModel& lm = *inst.model;
			const int side = inst.attacking ? 0 : 1;
			const float roundsAt = float(kBattleSetupSlices) * slice, roundLength = float(kBattleRoundSlices) * slice;
			// A clip of the set (or the next there is), of its kind (or the other).
			const auto pick = [](const std::optional<anim::Sequence> (&clips)[3][2], int set, bool ranged) -> const anim::Sequence* {
				for (const int kind : { ranged ? 1 : 0, ranged ? 0 : 1 })
					for (int s = 0; s < 3; ++s)
						if (const auto& c = clips[(set + s) % 3][kind])
							return &*c;
				return nullptr;
			};
			// (figures past those alive when it began were dead before)
			if (inst.figureIndex >= b.rounds.front().alive[side] + b.rounds.front().killed[side])
			{
				p.gone = true;
				return p;
			}
			// Its death: in the first round after which no more than its place are alive.
			for (size_t r = 0; r < b.rounds.size(); ++r)
				if (b.rounds[r].alive[side] <= inst.figureIndex)
				{
					const float died = roundsAt + float(r) * roundLength + 0.3f * roundLength;
					if (t < died)
						break;
					const int set = int(unitFloat(hash(inst.figureIndex, int(r), side)) * 3) % 3;
					const bool ranged = b.rounds[r].ranged || inst.ranged;
					const anim::Sequence* d = pick(lm.die, set, ranged);
					const anim::Sequence* f = pick(lm.dieFade, set, ranged);
					float at = t - died;
					p.dying = true;
					if (d && at < d->duration())
					{
						p.clip = d, p.at = at;
						return p;
					}
					at -= d ? d->duration() : 0;
					if (f && at < f->duration())
					{
						p.clip = f, p.at = at;
						return p;
					}
					p.gone = true;
					return p;
				}
			// Over: those alive stand in their places again at once (the original's do).
			if (t >= end)
				return BattlePose{};
			// A round on: it strikes (the figures a little one after another), or now and then,
			// in a melee round, it is hit and staggers. The ranged rounds of first strikes are the
			// striker's alone: the other stands and takes them (the original: a crossbowman's or a
			// longbowman's volleys, the axemen not answering).
			if (t >= roundsAt)
				if (const int r = int((t - roundsAt) / roundLength); r < int(b.rounds.size()))
				{
					if (r < firstStrikeRounds(b) && b.firstStrikes[side] < b.firstStrikes[1 - side])
						return p;
					const bool ranged = b.rounds[size_t(r)].ranged || inst.ranged;
					const anim::Sequence* s = pick(lm.strike, r % 3, ranged);
					if (!ranged && unitFloat(hash(inst.figureIndex * 7 + side, r, int(b.serial & 0xFFFF))) < 0.35f)
						if (const anim::Sequence* h = pick(lm.hurt, r % 3, false))
							s = h;
					if (s)
						if (const float at = t - roundsAt - float(r) * roundLength - 0.1f * float(inst.figureIndex % 3); at >= 0)
							p.clip = s, p.at = std::min(at, s->duration());
				}
			return p;
		}

		// The sounds of a fighting figure's clip that it passed since the last frame (its text
		// keys "SOUND:...,AS3D_..."): the strikes' of its first two, every death's.
		void battleSounds(const Instance& inst, const BattlePose& p)
		{
			if (p.clip)
				clipSounds(inst, *p.clip, p.at, p.dying || inst.figureIndex < 2); // (every figure's arrows fly)
		}

		// A figure acting (a mission's animation) at this time: pillaging (its melee strike,
		// again and again), bombarding and the ranged strikes (its ranged strike, else its
		// melee one), the air strikes and bombing (the plane's strike), captured (healing).
		std::pair<const anim::Sequence*, float> actionPose(const Instance& inst) const
		{
			if (inst.action < 0)
				return { nullptr, 0.0f };
			const float t = time - inst.actionStart;
			if (t < 0 || t > inst.actionLength)
				return { nullptr, 0.0f };
			// (a plane strikes over its plot, once, after it flew there; it flies back as it does)
			if (inst.air && inst.actionX >= 0)
			{
				// (intercepting another plane: it only flies out to meet it)
				if (inst.action == 6)
					return { nullptr, 0.0f };
				// (intercepted: hit as it comes over, it strikes nothing)
				if (inst.action == 5)
				{
					const LoadedModel& lm = *inst.model;
					const anim::Sequence* h = lm.hurt[0][0] ? &*lm.hurt[0][0] : nullptr;
					const float s = t - kFlightOut;
					if (!h || s < 0 || s > h->duration())
						return { nullptr, 0.0f };
					return { h, s };
				}
				const anim::Sequence* c = strikeClip(*inst.model);
				const float s = t - kFlightOut;
				if (!c || s < 0 || s > strikeTime(*inst.model))
					return { nullptr, 0.0f };
				return { c, std::min(s, c->duration()) };
			}
			const LoadedModel& lm = *inst.model;
			const auto first = [](const std::optional<anim::Sequence> (&clips)[3][2], int kind) -> const anim::Sequence* {
				for (int s = 0; s < 3; ++s)
					if (clips[s][kind])
						return &*clips[s][kind];
				return nullptr;
			};
			const anim::Sequence* clip = nullptr;
			switch (inst.action)
			{
			case 3: // captured
				clip = lm.heal ? &*lm.heal : nullptr;
				break;
			case 1: // pillaging
				clip = first(lm.strike, 0);
				break;
			case 4: // intercepting from the ground: once, as the plane comes over (its missile flies to it)
			{
				clip = lm.antiAir ? &*lm.antiAir : first(lm.strike, 1);
				if (!clip)
					clip = first(lm.strike, 0);
				const float at = t - 0.5f * kFlightOut;
				if (!clip || at < 0 || at > clip->duration())
					return { nullptr, 0.0f };
				return { clip, at };
			}
			default: // bombarding, a ranged attack, an air strike or bomb, a paradrop, recon, a nuke
				clip = first(lm.strike, 1);
				if (!clip)
					clip = first(lm.strike, 0);
				break;
			}
			if (!clip || clip->duration() <= 0)
				return { nullptr, 0.0f };
			const float at = t - 0.12f * float(inst.figureIndex % 3);
			return { clip, std::fmod(std::max(at, 0.0f), clip->duration()) };
		}

		// A plane's mission: it flies there (kFlightOut seconds), strikes over the plot (its
		// strike clip, as long as that is), flies back; how high it flies.
		static constexpr float kFlightOut = 0.8f, kFlightHeight = 0.35f;
		static const anim::Sequence* strikeClip(const LoadedModel& lm)
		{
			for (int kind : { 1, 0 })
				for (int s = 0; s < 3; ++s)
					if (lm.strike[s][kind])
						return &*lm.strike[s][kind];
			return nullptr;
		}
		static float strikeTime(const LoadedModel& lm)
		{
			const anim::Sequence* c = strikeClip(lm);
			return std::clamp(c ? c->duration() : 1.0f, 0.5f, 2.3f);
		}
		// Where a plane on a mission is then (on its plot when it flies none) and which way it
		// heads; its height over the ground.
		bool flight(const Instance& inst, Vec3& at, float& yaw, float& height) const
		{
			if (!inst.air || inst.action < 0 || inst.actionX < 0)
				return false;
			const float t = time - inst.actionStart, over = strikeTime(*inst.model), total = 2 * kFlightOut + over;
			if (t < 0 || t > total)
				return false;
			Vec3 target{ (float(inst.actionX) + 0.5f) * S(), (float(inst.actionY) + 0.5f) * S(), 0 };
			Vec3 d = target - inst.anchor;
			d.z = 0;
			if (map.wrapX && std::abs(d.x) > worldW() / 2)
				d.x -= std::copysign(worldW(), d.x);
			if (std::abs(d.x) + std::abs(d.y) < 0.01f)
				return false;
			const auto ease = [](float x) { return x * x * (3 - 2 * x); };
			const bool back = t > kFlightOut + over;
			float along = 1, up = 1;
			if (t < kFlightOut)
				along = ease(t / kFlightOut), up = std::min(1.0f, t / kFlightOut * 3);
			else if (back)
				along = ease((total - t) / kFlightOut), up = std::min(1.0f, (total - t) / kFlightOut * 3);
			at = inst.anchor + d * along;
			// (over the plot it heads on as it came: its strike flies it)
			yaw = std::atan2(back ? -d.y : d.y, back ? -d.x : d.x) + std::numbers::pi_v<float> / 2;
			height = kFlightHeight * S() * up;
			return true;
		}

		// Effects to set off later (a plane's at its target): when, its type, the plot.
		struct PendingEffect
		{
			float at;
			std::string type;
			int x, y;
			float fx = 0.5f, fy = 0.5f; // where in the plot
		};
		std::vector<PendingEffect> pendingEffects;

		// An effect by its type (CIV4EffectInfos) at the middle of a plot.
		std::unordered_set<uint64_t> actionsShown;
		void triggerEffectByType(const std::string& type, int x, int y, float fx = 0.5f, float fy = 0.5f)
		{
			const auto art = std::ranges::find_if(map.effects, [&](const auto& a) { return _stricmp(a.type.c_str(), type.c_str()) == 0; });
			if (art == map.effects.end())
				return;
			const float wx = (float(x) + fx) * S(), wy = (float(y) + fy) * S();
			addBurst(model(art->nif, ""), placement({ wx, wy, ground(wx, wy) }, 0, art->scale > 0 ? art->scale : 1));
		}

		// A worker's figure at work: its work's beginning once, then the work again and again,
		// now and then a pause (as the animation path's random test does); none: as usual.
		std::pair<const anim::Sequence*, float> workPose(const Instance& inst) const
		{
			const auto since = workSince.find(inst.unit);
			if (inst.work < 50 || inst.work > 65 || since == workSince.end())
				return { nullptr, 0.0f };
			const auto& w = inst.model->work[(inst.work - 50) / 3];
			float t = time - since->second.second + 0.37f * float(inst.figureIndex); // (not all in step)
			if (w[0] && t < w[0]->duration())
				return { &*w[0], t };
			t -= w[0] ? w[0]->duration() : 0;
			if (!w[1])
				return { nullptr, 0.0f };
			for (int i = 0; i < 400; ++i)
			{
				const bool pause = w[2] && unitFloat(hash(int(inst.unit & 0xFFFF), inst.figureIndex, i)) < 0.3f;
				const anim::Sequence& s = pause ? *w[2] : *w[1];
				if (t < s.duration() || s.duration() <= 0)
					return { &s, t };
				t -= s.duration();
			}
			return { &*w[1], std::fmod(t, std::max(w[1]->duration(), 0.01f)) };
		}

		// The sounds of a clip a figure plays that it passed since the last frame (its text
		// keys "SOUND:...,AS3D_..."); a clip begun again sounds again.
		void clipSounds(const Instance& inst, const anim::Sequence& seq, float now, bool sounds = true)
		{
			auto& [clip, last] = clipTimes[inst.unit * 31 + uint64_t(inst.figureIndex)];
			// (a little back is not a new start: a founding's clip time comes from the last capture,
			// which lags the frame by a moment, and steps back so at each one; its keys were heard)
			if (clip == &seq && now < last && last - now < 0.5f)
				return;
			const float from = clip == &seq && now >= last ? last : -1;
			clip = &seq, last = now;
			for (const auto& [keyTime, text] : seq.textKeys())
			{
				const float at = keyTime - seq.startTime();
				if (at <= from || at > now)
					continue;
				// (its effects: a line each)
				for (size_t e = 0; (e = text.find("ffect:", e)) != std::string::npos; e += 6)
				{
					if (e == 0 || (text[e - 1] != 'E' && text[e - 1] != 'e'))
						continue;
					const size_t colon = text.find(':', e + 6);
					if (colon == std::string::npos)
						break;
					const size_t end = std::min(text.find_first_of(" ,;\r\n", colon + 1), text.size());
					keyEffects.push_back({ text.substr(e + 6, colon - e - 6), text.substr(colon + 1, end - colon - 1) });
				}
				if (!sounds || !onSound || text.size() < 6 || _strnicmp(text.c_str(), "SOUND:", 6) != 0)
					continue;
				// (every comma-separated part that names a script plays, together, as the original's:
				// a settler's founding is "SOUND:AS3D_UN_FOUND_CITY,AS3D_UN_MOVE_SACK", a fall
				// "AS3D_UN_BODY_FALL,AS3D_UN_FOOT_UNIT"; an old key's other parts, a name and a time
				// as in "WarriorAttackA,316,AS3D_UN_SWOOSH_WEAPON", are none)
				size_t begin = 6;
				while (begin <= text.size())
				{
					const size_t comma = std::min(text.find(',', begin), text.size());
					std::string part = text.substr(begin, comma - begin);
					while (!part.empty() && part.front() == ' ')
						part.erase(part.begin());
					while (!part.empty() && (part.back() == ' ' || part.back() == '\r' || part.back() == '\n'))
						part.pop_back();
					if (part.rfind("AS", 0) == 0)
						onSound(part, inst.soundAt.x, inst.soundAt.y);
					begin = comma + 1;
				}
			}
		}

		// A model's particles under that key, on since it was last drawn (from a while back when
		// first seen: the smoke already up), as quads facing the camera.
		void drawParticlesOf(const LoadedModel& lm, uint64_t key, std::span<const Affine> world, std::span<const uint8_t> visible, bool luminous = false)
		{
			if (lm.model.particles.empty())
				return;
			auto [it, fresh] = effects.try_emplace(key);
			Effect& e = it->second;
			if (fresh)
			{
				e.particles = Particles(lm.model, uint32_t(key ^ key >> 32) | 1);
				float warm = 0;
				for (const auto& p : lm.model.particles)
					if (p.loop)
						warm = std::max(warm, std::min(p.lifeSpan + p.lifeSpanVariation * 0.5f, 30.0f));
				for (float t = 0; t < warm; t += 0.1f)
					e.particles.update(lm.model, 0.1f, world, visible);
			}
			else if (time > e.last)
				e.particles.update(lm.model, time - e.last, world, visible);
			e.last = time;
			e.drawn = effectFrame;
			const Camera& cam = renderer.camera();
			const Vec3 forward = normalize(cam.target - cam.eye()), right = normalize(cross(forward, Vec3{ 0, 0, 1 })), up = cross(right, forward);
			for (size_t i = 0; i < e.particles.systems(); ++i)
			{
				std::vector<scene::Vertex> quads;
				e.particles.quads(i, world, right, up, quads);
				if (!quads.empty())
				{
					scene::Material material = lm.model.particles[i].material;
					if (luminous)
					{
						// BtS blooms this effect after rendering.  Additive composition gives this
						// one luminous effect the same bright response without changing the map.
						material.alphaBlend = true;
						material.srcBlend = scene::Blend::SrcAlpha;
						material.dstBlend = scene::Blend::One;
					}
					renderer.drawParticles(*lm.gpu, i, material, std::move(quads));
				}
			}
		}

		// The attachables of a model's nodes, loaded (once: the map's art known).
		void resolveAttachments(LoadedModel& lm)
		{
			if (lm.attachResolved || !hasMap)
				return;
			lm.attachResolved = true;
			for (const auto& [node, type] : lm.attachTags)
				for (const auto& a : map.attachables)
					if (a.type == type)
					{
						if (LoadedModel* am = model(a.nif, ""))
							lm.attachments.push_back({ am, node, a.scale > 0 ? a.scale : 1 });
						break;
					}
		}

		// An instance's effects: its particles and the attachables on its nodes (each with its
		// own particles and animation).
		bool frozen = false, showEffects = true; // (MapView::Options)

		// A model standing at a height (base: its place's): its parts marked ContourGeometry laid
		// on the terrain, as the original's ContourShader.fx does (their heights over the ground
		// there, not over the model's base), the rest as placed. A mine's ground patch sat at the
		// mine's base, over the ground in front of it, and covered the mine's wooden deck.
		void drawModel(const LoadedModel& lm, std::span<const Affine> world, std::span<const uint8_t> visible,
			std::span<const scene::Material> materials, Vec3 team, bool onGround, float base)
		{
			if (lm.contourNodes.empty() || onGround)
			{
				renderer.draw(*lm.gpu, world, visible, materials, team, onGround);
				return;
			}
			std::vector<uint8_t> standing(visible.begin(), visible.end()), laid(visible.size(), 0);
			for (size_t i = 0; i < standing.size() && i < lm.contourNodes.size(); ++i)
				if (lm.contourNodes[i])
					laid[i] = standing[i], standing[i] = 0;
			renderer.draw(*lm.gpu, world, standing, materials, team, false);
			std::vector<Affine> lowered(world.begin(), world.end());
			for (auto& w : lowered)
				w.translation.z -= base;
			renderer.draw(*lm.gpu, lowered, laid, materials, team, true);
		}

		void drawEffects(const Instance& inst, std::span<const Affine> world, std::span<const uint8_t> visible)
		{
			if (!showEffects)
				return;
			LoadedModel& lm = *inst.model;
			if (lm.model.particles.empty() && lm.attachTags.empty())
				return;
			// (laid on the terrain: its nodes' heights are over it, as the shader puts them)
			std::vector<Affine> raised;
			if (inst.onGround)
			{
				raised.assign(world.begin(), world.end());
				for (auto& w : raised)
					w.translation.z += ground(w.translation.x, w.translation.y);
				world = raised;
			}
			uint64_t key = uint64_t(reinterpret_cast<uintptr_t>(&lm)) * 0x9E3779B97F4A7C15ull ^ uint64_t(uint32_t(inst.plot)) << 24 ^ uint64_t(uint32_t(inst.figureIndex));
			key += uint64_t(effectsThisFrame[key]++) * 0x100000001B3ull; // (the same model twice on a plot)
			drawParticlesOf(lm, key, world, visible);
			resolveAttachments(lm);
			std::vector<Affine> attached;
			std::vector<scene::Material> materials;
			for (size_t k = 0; k < lm.attachments.size(); ++k)
			{
				const auto& a = lm.attachments[k];
				if (size_t(a.node) >= world.size()) // (its node itself hidden, a helper: shown all the same)
					continue;
				// ... unless the state playing hides it: a mine's smoke and fire, on in its On
				// state, off in NotWorked.
				if (inst.sequence && inst.sequence->drivesVisibility(a.node) && size_t(a.node) < visible.size() && !visible[size_t(a.node)])
					continue;
				const LoadedModel& am = *a.model;
				const Affine at = world[size_t(a.node)] * placement({}, 0, a.scale);
				anim::Pose pose = anim::Pose::bind(am.model);
				if (am.embedded)
					am.embedded->apply(time + float(key % 997) * 0.01f, pose);
				attached = am.model.worldTransforms(pose.locals);
				for (auto& w : attached)
					w = at * w;
				const auto shown = am.model.visibility(pose.hidden);
				if (!am.model.items.empty())
				{
					materials.clear();
					for (const auto& item : am.model.items)
						materials.push_back(anim::animatedMaterial(item, pose));
					renderer.draw(*am.gpu, attached, shown, materials, inst.team, false);
				}
				drawParticlesOf(am, key * 31 + k + 1, attached, shown);
			}
		}

		// An effect shown there for its time: its animation's, else its emitters' (a looping
		// one: a few seconds).
		void addBurst(LoadedModel* lm, const Affine& place)
		{
			if (!lm || bursts.size() >= 48 || !showEffects)
				return;
			Burst b;
			b.model = lm;
			b.place = place;
			b.start = b.last = time;
			float span = lm->embedded ? lm->embedded->duration() : 0;
			if (span <= 0)
				for (const auto& p : lm->model.particles)
					span = std::max(span, p.loop ? 3.0f : std::min((p.stop - p.start) / p.frequency, 10.0f));
			b.until = time + std::clamp(span, 0.5f, 10.0f);
			b.particles = Particles(lm->model, uint32_t(bursts.size() * 2654435761u + uint32_t(time * 1000)) | 1);
			bursts.push_back(std::move(b));
		}

		// The effects a figure's clip set off since the last frame (its text keys'
		// "Effect:<node>:<type>" lines), at their nodes as the figure stands now.
		struct KeyEffect
		{
			std::string node, type;
		};
		std::vector<KeyEffect> keyEffects;
		void spawnKeyEffects(const LoadedModel& lm, std::span<const Affine> world)
		{
			if (!showEffects)
			{
				keyEffects.clear();
				return;
			}
			for (const KeyEffect& k : std::exchange(keyEffects, {}))
			{
				const auto art = std::ranges::find_if(map.effects, [&](const auto& a) { return _stricmp(a.type.c_str(), k.type.c_str()) == 0; });
				if (art == map.effects.end())
					continue;
				Affine at = world.empty() ? Affine{} : world[0];
				for (size_t n = 0; n < lm.model.nodes.size() && n < world.size(); ++n)
					if (_stricmp(lm.model.nodes[n].name.c_str(), k.node.c_str()) == 0)
					{
						at = world[n];
						break;
					}
				if (art->projectile)
					addProjectile(*art, at.translation, at.scale); // (as large as the figure that sets it off)
				else
					addBurst(effectModel(art->nif), at * placement({}, 0, art->scale > 0 ? art->scale : 1));
			}
		}

		// What a figure's projectiles fly to (set before its clip's effects are set off): the plot
		// it attacks, the plane it shoots at; none: straight down to the ground.
		std::optional<Vec3> keyTarget;

		// An effect's model: a NIF, or a KFM's (with its clips).
		LoadedModel* effectModel(const std::string& path)
		{
			if (path.size() < 4 || _stricmp(path.c_str() + path.size() - 4, ".kfm") != 0)
				return model(path, "");
			const std::string key = makeAssetKey(path);
			const auto loc = assets.locate(key);
			if (!loc)
				return nullptr;
			try
			{
				const nif::KfmFile kfm = nif::parseKfm(assets.read(*loc));
				return model(folderOf(key) + kfm.nifFile, path);
			}
			catch (const std::exception&)
			{
				return nullptr;
			}
		}

		void addProjectile(const MapSnapshot::Attachable& art, const Vec3& from, float size = 1)
		{
			LoadedModel* lm = effectModel(art.nif);
			const size_t before = bursts.size();
			addBurst(lm, placement(from, 0, art.scale > 0 ? art.scale : 1));
			if (bursts.size() == before)
				return;
			Burst& b = bursts.back();
			b.projectile = true;
			b.from = from;
			b.to = keyTarget ? *keyTarget : Vec3{ from.x, from.y, ground(from.x, from.y) };
			b.scale = (art.scale > 0 ? art.scale : 1) * size;
			const float speed = std::max(art.speed, 50.0f) / 180.0f * S(); // (world units a second; a plot is 180)
			b.flight = std::clamp(length(b.to - b.from) / speed, 0.05f, 4.0f);
			const anim::Sequence* explode = lm->strike[0][0] ? &*lm->strike[0][0] : nullptr;
			b.until = b.start + b.flight + (explode ? explode->duration() : 0);
		}

		void drawBursts(const Frustum& frustum)
		{
			const Camera& cam = renderer.camera();
			const Vec3 forward = normalize(cam.target - cam.eye()), right = normalize(cross(forward, Vec3{ 0, 0, 1 })), up = cross(right, forward);
			std::vector<Affine> world;
			std::vector<scene::Material> materials;
			for (Burst& b : bursts)
			{
				const LoadedModel& lm = *b.model;
				const float at = time - b.start;
				anim::Pose pose = anim::Pose::bind(lm.model);
				if (lm.embedded)
					lm.embedded->apply(at, pose);
				const anim::Sequence* explode = b.projectile && lm.strike[0][0] ? &*lm.strike[0][0] : nullptr;
				if (b.projectile)
				{
					// (flying, it heads for its target; there, it explodes)
					const Vec3 d = b.to - b.from;
					const float f = std::clamp(at / b.flight, 0.0f, 1.0f);
					const float yaw = std::abs(d.x) + std::abs(d.y) > 0.01f ? std::atan2(d.y, d.x) + std::numbers::pi_v<float> / 2 : 0.0f;
					b.place = placement(b.from + d * f, yaw, b.scale);
					if (at < b.flight && lm.run)
						lm.run->apply(at, pose);
					else if (explode)
						explode->apply(at - b.flight, pose);
					for (const char* root : { "MD", "MD NonAccum" })
						if (const int n = lm.model.findNode(root); n >= 0 && size_t(n) < pose.locals.size())
							pose.locals[size_t(n)] = lm.model.nodes[size_t(n)].local;
				}
				world = lm.model.worldTransforms(pose.locals);
				for (auto& w : world)
					w = b.place * w;
				// (its explosion's effects and sounds: the text keys passed since the last frame)
				if (explode && at >= b.flight)
				{
					const float now = at - b.flight;
					for (const auto& [keyTime, text] : explode->textKeys())
					{
						const float k = keyTime - explode->startTime();
						if (k <= b.keysLast || k > now)
							continue;
						for (size_t e = 0; (e = text.find("ffect:", e)) != std::string::npos; e += 6)
						{
							const size_t colon = text.find(':', e + 6);
							if (colon == std::string::npos)
								break;
							const size_t end = std::min(text.find_first_of(" ,;\r\n", colon + 1), text.size());
							const std::string node = text.substr(e + 6, colon - e - 6), type = text.substr(colon + 1, end - colon - 1);
							Affine nodeAt = world.empty() ? b.place : world[0];
							for (size_t n = 0; n < lm.model.nodes.size() && n < world.size(); ++n)
								if (_stricmp(lm.model.nodes[n].name.c_str(), node.c_str()) == 0)
									nodeAt = world[n];
							explosions.push_back({ type, nodeAt });
						}
						if (onSound && text.size() > 6 && _strnicmp(text.c_str(), "SOUND:", 6) == 0)
						{
							std::string tag = text.substr(6);
							if (const size_t comma = tag.rfind(','); comma != std::string::npos)
								tag = tag.substr(comma + 1);
							while (!tag.empty() && tag.front() == ' ')
								tag.erase(tag.begin());
							onSound(tag, b.to.x / S(), b.to.y / S());
						}
					}
					b.keysLast = now;
				}
				const auto shown = lm.model.visibility(pose.hidden);
				if (time >= b.until)
					b.particles.stopEmitting();
				if (time > b.last)
					b.particles.update(lm.model, time - b.last, world, shown);
				b.last = time;
				if (!frustum.sphere(b.place.translation, std::max(lm.bounds.empty() ? 0.0f : length(lm.bounds.max - lm.bounds.min) * b.place.scale, 4 * S())))
					continue;
				if (time < b.until && !lm.model.items.empty())
				{
					materials.clear();
					for (const auto& item : lm.model.items)
						materials.push_back(anim::animatedMaterial(item, pose));
					renderer.draw(*lm.gpu, world, shown, materials, kDefaultTeamColor, false);
				}
				for (size_t i = 0; i < b.particles.systems(); ++i)
				{
					std::vector<scene::Vertex> quads;
					b.particles.quads(i, world, right, up, quads);
					if (!quads.empty())
						renderer.drawParticles(*lm.gpu, i, lm.model.particles[i].material, std::move(quads));
				}
			}
			std::erase_if(bursts, [&](const Burst& b) { return time >= b.until && (b.particles.alive() == 0 || time > b.until + 20); });
			for (const auto& [type, at] : std::exchange(explosions, {}))
			{
				const auto art = std::ranges::find_if(map.effects, [&](const auto& a) { return _stricmp(a.type.c_str(), type.c_str()) == 0; });
				if (art != map.effects.end() && !art->projectile)
					addBurst(effectModel(art->nif), at * placement({}, 0, art->scale > 0 ? art->scale : 1));
			}
		}
		std::vector<std::pair<std::string, Affine>> explosions; // (set off by projectiles this frame)

		// The original engine's showPromotionGlow: keep EFFECT_PROMOTION_UNIT at every unit
		// that can take a promotion. It is a looping blue aura and rising sparkles in BtS.
		void drawPromotionGlows(const Frustum& frustum)
		{
			if (!showEffects)
				return;
			const auto art = std::ranges::find_if(map.effects, [](const auto& a) { return _stricmp(a.type.c_str(), "EFFECT_PROMOTION_UNIT") == 0; });
			if (art == map.effects.end())
				return;
			LoadedModel* lm = model(art->nif, "");
			if (!lm)
				return;
			for (const UnitView& u : map.units)
			{
				const int plot = u.x + u.y * map.width;
				if (!u.promotionReady || (fog && !map.plots[size_t(plot)].visible))
					continue;
				const uint64_t unit = uint64_t(uint32_t(u.owner)) << 32 | uint32_t(u.id);
				Vec3 at{ (float(u.x) + 0.5f) * S(), (float(u.y) + 0.5f) * S(), 0 };
				if (const auto position = unitPositions.find(unit); position != unitPositions.end())
					at = position->second;
				if (const Motion* motion = activeMotion(unit))
					at = motionPoint(*motion);
				at.z = ground(at.x, at.y);
				at.x += wrapOffset(at.x);
				if (!frustum.sphere(at, S()))
					continue;
				const Affine place = placement(at, 0, art->scale > 0 ? art->scale : 1);
				std::vector<Affine> world = lm->bindWorld;
				for (auto& w : world)
					w = place * w;
				if (!lm->model.items.empty())
					renderer.draw(*lm->gpu, world, lm->bindVisible);
				// Keep a stable particle simulation for this unit while its ready state remains set.
				drawParticlesOf(*lm, unit ^ 0xA41F2D729C55E8BFull, world, lm->bindVisible, true);
			}
		}

		// laid: -1 every instance, 1 only those laid on the terrain (Instance::laid), 0 the others.
		void drawInstances(const std::vector<Instance>& list, const Frustum& frustum, bool needsSight = false, int laid = -1)
		{
			std::vector<Affine> world;
			std::vector<scene::Material> materials;
			const float zoom = figureZoom();
			for (const Instance& inst : list)
			{
				if (laid >= 0 && inst.laid != (laid == 1))
					continue;
				if (fogged(inst, needsSight))
					continue;
				// (each figure of a walking stack a little behind the first: the original's figures
				// run one by one; a Wine stack of 3 axemen took 1.7 s where a scout takes 1 s)
				const Motion* motion = nullptr;
				float delay = 0;
				if (inst.unit)
				{
					// (it runs the whole order slower, to arrive up to spread later)
					if (const Motion* first = activeMotion(inst.unit, 1000); first && inst.figureIndex > 0 && first->orderSeconds > 0)
					{
						const float late = first->spread * unitFloat(hash(int(inst.unit), int(inst.unit >> 32), inst.figureIndex));
						delay = std::clamp((time - first->orderStart) * late / (first->orderSeconds + late), 0.0f, late);
					}
					motion = activeMotion(inst.unit, delay);
				}
				Vec3 anchor = inst.anchor;
				float yaw = inst.yaw;
				// Fighting: the attacker steps up to the defender; the dead are gone.
				const BattlePose battle = inst.figure ? battlePose(inst) : BattlePose{};
				if (battle.gone || (inst.role == 2 && !battle.fighting))
					continue;
				// (a settler founding a city: until its founding is done; without one, gone)
				if (inst.foundAt >= 0 && (!inst.model->found || time - inst.foundAt > inst.model->found->duration()))
					continue;
				if (battle.fighting)
					anchor = anchor + inst.step * battle.step;
				float lift = 0;
				const bool flying = inst.figure && flight(inst, anchor, yaw, lift);
				if (motion)
				{
					// Walking: from where it was, facing the way, on its way to the plot.
					const Vec3 d = motion->to - motion->from;
					anchor = motionPoint(*motion, delay) + (inst.anchor - inst.center);
					if (std::abs(d.x) + std::abs(d.y) > 0.01f)
						yaw = std::atan2(d.y, d.x) + std::numbers::pi_v<float> / 2;
				}
				const Vec3 center = motion || battle.fighting || flying ? anchor : inst.center;
				const float dx = wrapOffset(center.x);
				if (!frustum.sphere(center + Vec3{ dx, 0, 0 }, inst.radius * (inst.figure ? zoom : 1)))
					continue;
				// A unit's figure (and what it carries) in the units' own light, as the original's.
				renderer.setUnitLight(inst.figure);
				const LoadedModel& lm = *inst.model;
				Affine place = inst.place;
				if (inst.figure)
				{
					const float c = std::cos(yaw - inst.yaw), s = std::sin(yaw - inst.yaw);
					const Vec3 offset{ inst.offset.x * c - inst.offset.y * s, inst.offset.x * s + inst.offset.y * c, 0 };
					Vec3 at = anchor + offset * std::min(zoom, 1.6f);
					at.z = ground(at.x, at.y) + lift;
					place = placement(at, yaw, inst.scale * zoom);
					if (!lm.bounds.empty())
					{
						// (as round as the figure is tall, a quarter of it: its weapons stick out of its bounds)
						const float size = inst.scale * zoom;
						figureHits.push_back({ at + Vec3{ dx, 0, lm.bounds.center().z * size }, std::max((lm.bounds.max.z - lm.bounds.min.z) * size * 0.25f, 0.05f * S()), inst.plot });
					}
				}
				// Far away the figures stand still: their animation would not show.
				if (inst.figure && !motion && !flying && renderer.camera().distance > 28 * S())
				{
					place = translation({ dx, 0, 0 }) * place;
					world.resize(lm.bindWorld.size());
					for (size_t k = 0; k < world.size(); ++k)
						world[k] = place * lm.bindWorld[k];
					drawModel(lm, world, lm.bindVisible, {}, inst.team, inst.onGround, place.translation.z);
					drawEffects(inst, world, lm.bindVisible);
					continue;
				}
				if (inst.animated || inst.figure)
				{
					anim::Pose pose = anim::Pose::bind(lm.model);
					// Resting: its fortify once (its kind's, or the other's), then fortified.
					const size_t kind = inst.ranged ? 1 : 0;
					const anim::Sequence* fortify = lm.fortify[kind] ? &*lm.fortify[kind] : lm.fortify[1 - kind] ? &*lm.fortify[1 - kind] : nullptr;
					const anim::Sequence* fortified = lm.fortified[kind] ? &*lm.fortified[kind] : lm.fortified[1 - kind] ? &*lm.fortified[1 - kind] : nullptr;
					const auto since = inst.resting ? restingSince.find(inst.unit) : restingSince.end();
					const float rested = since != restingSince.end() ? time - since->second : -1;
					if (battle.clip)
					{
						battle.clip->apply(battle.at, pose);
						battleSounds(inst, battle);
					}
					else if (battle.fighting && inst.attacking && inst.role == 0 && battle.step > 0 && battle.step < 1 && lm.run)
						lm.run->apply(time, pose); // (stepping up, or back)
					else if (motion && lm.run)
					{
						lm.run->apply(time - delay - motion->start, pose);
						// (its effects, the footprints: each time round)
						if (const float d = lm.run->duration(); d > 0)
							clipSounds(inst, *lm.run, std::fmod(std::max(time - delay - motion->start, 0.0f), d), false);
					}
					else if (const auto [clip, at] = actionPose(inst); clip)
					{
						clip->apply(at, pose);
						// (a plane's strike flies it over the plot by its accumulation root: here
						// the flight does)
						if (inst.air)
							for (const char* root : { "MD", "MD NonAccum" })
								if (const int n = lm.model.findNode(root); n >= 0 && size_t(n) < pose.locals.size())
									pose.locals[size_t(n)] = lm.model.nodes[size_t(n)].local;
						if (inst.figureIndex < 2)
							clipSounds(inst, *clip, at);
					}
					else if (inst.foundAt >= 0)
					{
						lm.found->apply(time - inst.foundAt, pose);
						if (inst.figureIndex == 0)
							clipSounds(inst, *lm.found, time - inst.foundAt);
					}
					else if (const auto [work, at] = workPose(inst); work)
					{
						work->apply(at, pose);
						if (inst.figureIndex == 0)
							clipSounds(inst, *work, at);
					}
					else if (rested >= 0 && fortify && rested < fortify->duration())
						fortify->apply(rested, pose);
					else if (rested >= 0 && fortified)
						fortified->apply(rested + inst.phase, pose);
					else if (rested >= 0 && fortify)
						fortify->apply(fortify->duration(), pose);
					else if (inst.animated)
						(inst.sequence ? *inst.sequence : *lm.idle).apply(frozen ? 0 : time + inst.phase, pose);
					if (inst.damage > 0)
						for (const anim::Sequence& d : lm.damage[inst.damage - 1])
							d.apply(std::fmod(time + inst.phase, std::max(d.duration(), 0.01f)), pose);
					if (lm.embedded)
						lm.embedded->apply(time + inst.phase, pose);
					world = lm.model.worldTransforms(pose.locals);
					place = translation({ dx, 0, 0 }) * place;
					for (auto& w : world)
						w = place * w;
					if (!keyEffects.empty())
					{
						// (its projectiles' target: the plot its action aims at, over it the plane a
						// SAM shoots at; in a battle, a figure on the other side's plot)
						keyTarget.reset();
						if (inst.action >= 0 && inst.actionX >= 0)
						{
							Vec3 c{ (float(inst.actionX) + 0.5f) * S() + dx, (float(inst.actionY) + 0.5f) * S(), 0 };
							c.z = ground(c.x - dx, c.y) + (inst.action == 4 ? kFlightHeight * S() : 0.0f);
							keyTarget = c;
						}
						else if (battle.fighting && inst.battle >= 0 && size_t(inst.battle) < map.battles.size())
						{
							const BattleView& b = map.battles[size_t(inst.battle)];
							const int ox = inst.attacking ? b.defenderX : b.attackerX, oy = inst.attacking ? b.defenderY : b.attackerY;
							const float jx = (unitFloat(hash(inst.figureIndex, ox, oy)) - 0.5f) * 0.3f, jy = (unitFloat(hash(oy, inst.figureIndex, ox)) - 0.5f) * 0.3f;
							Vec3 c{ (float(ox) + 0.5f + jx) * S() + dx, (float(oy) + 0.5f + jy) * S(), 0 };
							c.z = ground(c.x - dx, c.y) + 0.08f * S();
							keyTarget = c;
						}
						spawnKeyEffects(lm, world);
					}
					materials.clear();
					if (std::ranges::any_of(pose.materials, [](const anim::MaterialOverride& o) { return o.any; }))
						for (const auto& item : lm.model.items)
							materials.push_back(anim::animatedMaterial(item, pose));
					const auto shownNodes = lm.model.visibility(pose.hidden);
					drawModel(lm, world, shownNodes, materials, inst.team, inst.onGround, place.translation.z);
					drawEffects(inst, world, shownNodes);
					if (inst.shadow)
					{
						// On the ground under the node it follows (the pelvis, as the figure walks).
						const Vec3 p = inst.shadowNode >= 0 && size_t(inst.shadowNode) < world.size() ? world[size_t(inst.shadowNode)].translation : place.translation;
						const Affine at = placement({ p.x, p.y, 0.5f }, yaw, inst.scale * zoom * inst.shadowScale);
						std::vector<Affine> shadowWorld = inst.shadow->bindWorld;
						for (auto& w : shadowWorld)
							w = at * w;
						renderer.draw(*inst.shadow->gpu, shadowWorld, inst.shadow->bindVisible, {}, inst.team, true);
					}
					continue;
				}
				const auto& visible = inst.visible.empty() ? lm.bindVisible : inst.visible;
				if (lm.embedded)
				{
					// Its own controllers: materials over the placed pose, or the pose itself.
					anim::Pose pose = anim::Pose::bind(lm.model);
					lm.embedded->apply(time + inst.phase, pose);
					materials.clear();
					for (const auto& item : lm.model.items)
						materials.push_back(anim::animatedMaterial(item, pose));
					std::vector<uint8_t> shown = visible;
					if (lm.embeddedMoves)
					{
						world = lm.model.worldTransforms(pose.locals);
						const Affine at = translation({ dx, 0, 0 }) * inst.place;
						for (auto& w : world)
							w = at * w;
						const auto animated = lm.model.visibility(pose.hidden);
						for (size_t i = 0; i < shown.size() && i < animated.size(); ++i)
							shown[i] = shown[i] && animated[i];
					}
					else
					{
						world = inst.world;
						for (auto& w : world)
							w.translation.x += dx;
					}
					drawModel(lm, world, shown, materials, inst.team, inst.onGround, inst.place.translation.z);
					drawEffects(inst, world, shown);
					continue;
				}
				if (dx == 0)
				{
					drawModel(lm, inst.world, visible, inst.materials, inst.team, inst.onGround, inst.place.translation.z);
					drawEffects(inst, inst.world, visible);
					continue;
				}
				world = inst.world;
				for (auto& w : world)
					w.translation.x += dx;
				drawModel(lm, world, visible, inst.materials, inst.team, inst.onGround, inst.place.translation.z);
				drawEffects(inst, world, visible);
			}
			renderer.setUnitLight(false);
		}
	};

	MapView::MapView(Renderer& renderer, const AssetResolver& assets) : m(std::make_unique<Impl>(renderer, assets)) {}
	MapView::~MapView() = default;

	void MapView::setMap(const MapSnapshot& snapshot)
	{
		if (snapshot.width <= 0 || snapshot.height <= 0 || snapshot.plots.size() != size_t(snapshot.width) * size_t(snapshot.height))
			return;
		const bool newArt = snapshot.artGeneration != m->artGeneration;
		const bool resized = !m->hasMap || snapshot.width != m->map.width || snapshot.height != m->map.height || snapshot.wrapX != m->map.wrapX ||
			snapshot.wrapY != m->map.wrapY || snapshot.plotSize != m->map.plotSize;
		std::vector<uint8_t> shape(snapshot.plots.size());
		std::vector<int32_t> featureKey(snapshot.plots.size());
		std::vector<uint64_t> decorKey(snapshot.plots.size());
		for (size_t i = 0; i < snapshot.plots.size(); ++i)
		{
			const PlotView& p = snapshot.plots[i];
			shape[i] = uint8_t((p.water ? 1 : 0) | (p.hills ? 2 : 0) | (p.peak ? 4 : 0) | (p.riverSouth ? 8 : 0) | (p.riverEast ? 16 : 0));
			featureKey[i] = int32_t(p.feature) << 8 | p.featureVariety;
			// (the route's 8 bits and the layout's 16: two of them 256 or 65536 apart on a plot would look unchanged)
			decorKey[i] = uint64_t(uint16_t(p.improvement)) | uint64_t(uint16_t(p.bonus)) << 16 | uint64_t(uint8_t(p.route)) << 32 |
				uint64_t(p.riverSouth) << 40 | uint64_t(p.riverEast) << 41 | uint64_t(uint16_t(p.layout + 1)) << 42 |
				uint64_t(p.improvementState & 3) << 58 | uint64_t(p.bonusState & 3) << 60;
		}
		const bool reshaped = resized || shape != m->shape;
		const bool refeatured = reshaped || newArt || featureKey != m->featureKey;
		const bool redecorated = reshaped || newArt || decorKey != m->decorKey;

		m->map = snapshot;
		if (newArt || reshaped)
			m->cityLayouts.clear();
		if (newArt)
		{
			m->buildAtlas();
			m->buildDecals();
			m->buildWaterArt();
			m->artGeneration = snapshot.artGeneration;
		}
		if (reshaped)
		{
			m->shape = std::move(shape);
			m->buildTerrain();
			m->buildWater();
		}
		m->updatePlots();
		// Improvements, resources and roads first: they clear trees from the features.
		if (redecorated)
		{
			m->decorKey = std::move(decorKey);
			m->buildDecor();
		}
		m->updateBorders();
		if (refeatured || redecorated)
		{
			m->featureKey = std::move(featureKey);
			m->buildFeatures();
			m->cutTrees();
		}
		m->buildCities();
		m->buildUnits();
		m->updateMinimap();
		m->hasMap = true;
		// The plot each piece stands on, for the fog of war (a corner piece: the plot north-east of it).
		for (auto* list : { &m->features, &m->decor, &m->cities })
			for (auto& inst : *list)
				if (inst.plot < 0)
					if (const auto p = plotAt(inst.center))
						inst.plot = p->first + p->second * snapshot.width;
	}

	bool MapView::empty() const { return !m->hasMap; }
	const MapSnapshot& MapView::map() const { return m->map; }
	void MapView::triggerEffect(const std::string& nif, int x, int y, float fx, float fy, float degrees, float scale)
	{
		if (!m->hasMap)
		{
			if (m->pendingBursts.size() < 48)
				m->pendingBursts.push_back([=, this] { triggerEffect(nif, x, y, fx, fy, degrees, scale); });
			return;
		}
		if (m->bursts.size() >= 48)
			return;
		const float wx = (float(x) + fx) * m->S(), wy = (float(y) + fy) * m->S();
		m->addBurst(m->model(nif, ""), placement({ wx, wy, m->ground(wx, wy) }, degrees * std::numbers::pi_v<float> / 180, scale > 0 ? scale : 1));
	}

	void MapView::update(float dt)
	{
		m->time += dt;
		if (m->hasMap && !m->pendingBursts.empty())
			for (const auto& set : std::exchange(m->pendingBursts, {}))
				set();
		if (m->hasMap && !m->pendingEffects.empty())
		{
			std::vector<Impl::PendingEffect> due;
			std::erase_if(m->pendingEffects, [&](const Impl::PendingEffect& e) {
				if (m->time < e.at)
					return false;
				due.push_back(e);
				return true;
			});
			for (const auto& e : due)
				m->triggerEffectByType(e.type, e.x, e.y, e.fx, e.fy);
		}
		if (m->ring.ok)
			m->ring.player.update(dt);
		m->finishMotions();
	}

	void MapView::draw()
	{
		if (!m->hasMap)
			return;
		m->frozen = options.frozen;
		m->showEffects = options.effects;
		if (m->fog != options.fog)
		{
			m->fog = options.fog;
			m->updateMinimap();
		}
		Frustum frustum(m->renderer.viewProjection());
		frustum.all = m->renderer.globe();
		++m->effectFrame;
		m->effectsThisFrame.clear();
		m->drawTerrain(frustum, options);
		m->renderer.setFog(options.fog ? m->plotTex.get() : nullptr, m->S(), uint32_t(m->map.width), uint32_t(m->map.height), m->map.wrapX,
			m->map.wrapY);
		// The culture borders over the ground and what is laid on it (the original: their line over
		// a river's water along its bank), under what stands on it (the soft dirt under resources
		// and improvements blends over the border as over the ground).
		if (options.improvements)
		{
			m->drawInstances(m->decor, frustum, false, 1);
			m->renderer.drawGroundBlended();
		}
		m->drawBorders(frustum, options, 1);
		if (options.features)
			m->drawInstances(m->features, frustum);
		if (options.improvements)
			m->drawInstances(m->decor, frustum, false, 0);
		// The sea's water over what is under it: the rivers' water laid on the ground (a delta's,
		// out at sea, seen through the sea's) and what stands in the water.
		m->renderer.drawGroundBlended();
		m->drawWater(frustum, options);
		m->drawBorders(frustum, options, 2);
		if (options.cities && (options.globeBuildings || !m->renderer.globe()))
			m->drawInstances(m->cities, frustum);
		m->drawGroundCircles(frustum);
		m->figureHits.clear();
		m->flagDraws.clear();
		if (options.units && !options.cityScreen)
		{
			m->drawPromotionGlows(frustum);
			// (the selection rings under the units: the figures on them cover them)
			m->ringsDrawn = m->drawRings();
			m->drawInstances(m->units, frustum, true);
			if (options.flags)
				m->drawFlags(frustum);
			if (options.flags && options.healthBars && m->renderer.camera().distance > m->map.cameraMinDistance + 0.5f &&
				m->renderer.camera().distance < 22 * m->S())
				m->drawHealthBars(frustum);
		}
		m->drawBursts(frustum);
		// (the particles of what is no longer drawn: gone)
		std::erase_if(m->effects, [&](const auto& e) { return e.second.drawn != m->effectFrame; });
	}

	std::vector<uint8_t> MapView::territoryMinimap(float alpha, bool wholeMap) const
	{
		std::vector<uint8_t> pixels;
		if (m->hasMap)
			m->paintMinimap(pixels, alpha, alpha * 0.55f, false, wholeMap);
		return pixels;
	}

	std::optional<std::pair<int, int>> MapView::figureAt(Vec3 a, Vec3 b) const
	{
		if (!m->hasMap)
			return std::nullopt;
		const Vec3 d = b - a;
		const float dd = dot(d, d);
		if (dd <= 0)
			return std::nullopt;
		float best = std::numeric_limits<float>::max();
		int plot = -1;
		for (const auto& h : m->figureHits)
		{
			// (the ray's nearest point to the middle, within the radius)
			const float t = dot(h.center - a, d) / dd;
			if (t < 0)
				continue;
			const Vec3 p = a + d * t, off = p - h.center;
			if (dot(off, off) <= h.radius * h.radius && t < best)
				best = t, plot = h.plot;
		}
		if (plot < 0)
			return std::nullopt;
		return std::pair{ plot % m->map.width, plot / m->map.width };
	}

	namespace
	{
		std::wstring wide(std::string_view s)
		{
			if (s.empty())
				return {};
			const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
			std::wstring w(size_t(n), L'\0');
			MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
			return w;
		}
	}

	// The city bars of the game: population in the owner's colour, name, the food and
	// production bars and what is being built where the active player may see them.
	void MapView::drawOverlay(Overlay& overlay)
	{
		if (!m->hasMap || !options.banners || !options.cities || options.cityScreen || m->renderer.globe())
			return;
		const float* vp = m->renderer.viewProjection();
		const float distance = m->renderer.camera().distance / m->S();
		const float scale = std::clamp(1.3f - distance / 60, 0.7f, 1.1f);
		const float W = overlay.screenWidth(), H = overlay.screenHeight();
		const auto toScreen = [&](Vec3 p, float& x, float& y) {
			const float cw = vp[12] * p.x + vp[13] * p.y + vp[14] * p.z + vp[15];
			if (cw <= 0)
				return false;
			x = ((vp[0] * p.x + vp[1] * p.y + vp[2] * p.z + vp[3]) / cw + 1) * 0.5f * W;
			y = (1 - (vp[4] * p.x + vp[5] * p.y + vp[6] * p.z + vp[7]) / cw) * 0.5f * H;
			return x > -50 && x < W + 50 && y > -50 && y < H + 50;
		};
		// The units' flags: the visible owner's decal and colours, health, and stack count.
		if (options.flags && options.units && distance < 22 && m->renderer.camera().distance > m->map.cameraMinDistance + 0.5f)
			for (const UnitView& u : m->map.units)
			{
				const int plot = u.x + u.y * m->map.width;
				if (options.fog && !m->map.plots[size_t(plot)].visible)
					continue;
				// (a stack fighting shows no flag, nor a ring, while the battle is on: the original's)
				if (u.battle >= 0)
					continue;
				// (where the unit is: the original's flag stands there at once, while its figures
				// still walk there)
				Vec3 p{ (float(u.x) + 0.5f) * m->S(), (float(u.y) + 0.5f) * m->S(), 0 };
				const uint64_t key = uint64_t(uint32_t(u.owner)) << 32 | uint32_t(u.id);
				const bool walking = m->activeMotion(key) != nullptr;
				p.x += m->wrapOffset(p.x);
				if (u.selected && !m->ringsDrawn && !walking)
				{
					// The selection, without the game's art: a ring on the ground round the stack.
					Vec3 g = p;
					g.z = m->ground(p.x, p.y);
					float gx = 0, gy = 0, ex = 0, ey = 0;
					if (toScreen(g, gx, gy) && toScreen(g + Vec3{ 0.3f * m->S(), 0, 0 }, ex, ey))
					{
						const float rx = std::abs(ex - gx), ry = rx * 0.55f;
						const float pulse = 0.65f + 0.35f * std::sin(m->time * 5);
						overlay.ellipse(gx, gy, rx, ry, { 0, 0, 0 }, 0.6f, 4);
						overlay.ellipse(gx, gy, rx, ry, { 0.3f, 1, 0.4f }, pulse, 2.2f);
					}
				}
				p.z = m->ground(p.x, p.y);
				p.z += 0.3f * m->S() * m->figureZoom();
				float x = 0, y = 0;
				if (!toScreen(p, x, y))
					continue;
				const PlayerView* owner = u.shownOwner >= 0 && size_t(u.shownOwner) < m->map.players.size() ? &m->map.players[size_t(u.shownOwner)] : nullptr;
				const Color primary = owner ? owner->primary : Color{}, secondary = owner ? owner->secondary : Color{ 0, 0, 0 };
				const float flagScale = scale * std::clamp(18 / std::max(distance, 1.0f), 0.8f, 2.0f);
				const float fw = 24 * flagScale, fh = 30 * flagScale;
				float topY = y - fh;
				if (const auto it = m->flagDraws.find(plot); it != m->flagDraws.end())
				{
					float topX = 0;
					if (!toScreen(it->second.clothBottom, x, y) || !toScreen(it->second.top, topX, topY))
						continue;
				}
				else
				{
					// Mods without usable flag models can still show the civilization's emblem.
					if (const ButtonArt::Icon* flag = owner ? m->flagArt.flag(owner->flagDecal, primary, secondary, owner->whiteFlag) : nullptr)
						overlay.image(flag->key, flag->bgra, flag->width, flag->height, 1, x - fw / 2, y - fh, x + fw / 2, y);
					else
						overlay.rect(x - fw / 2, y - fh, x + fw / 2, y, primary, 0.95f);
				}
				const auto drawn = m->flagDraws.find(plot);
				if (u.count > 1 && (drawn == m->flagDraws.end() || !drawn->second.medallions))
					overlay.text(std::to_wstring(u.count), x + fw / 2 + 2, topY - 1, 10 * scale, { 1, 1, 1 }, 0.95f, true);
			}
		// The city bars, as the original's CityBar.nif lays them out (in its units: the banner
		// 128 x 32 with its top at the origin, the population's circle at its left end, the
		// production symbol at its right end, the icon line above), facing the screen.
		// Placed as CIV4DetailManager's CITYBILLBOARD_RADIUS places it: its top on the ground
		// south of the city (towards the bottom of the screen), 90 units out up close, 35 at
		// 5000 and beyond; the original's grid shows it no closer than 70 there. Sized as
		// measured in the original (the star's width, at camera distances of 5 to 102 plots):
		// CITYBILLBOARD_SCALE's 1.5 up close to its 0.67 far out, in points a model unit.
		const float plots = m->renderer.camera().distance / m->S();
		const float civ = m->S() / 180; // (the original's units: 180 a plot)
		const float units = plots * 180;
		const float radius = std::max(70.0f, std::lerp(90.0f, 35.0f, std::clamp((units - 700) / 4300, 0.0f, 1.0f)));
		const float k = [&] {
			static constexpr std::array<std::pair<float, float>, 8> measured{ { { 3.89f, 1.39f }, { 5.18f, 1.327f }, { 8.8f, 1.16f },
				{ 12.3f, 0.962f }, { 14.85f, 0.913f }, { 28.5f, 0.743f }, { 47.5f, 0.718f }, { 102, 0.694f } } };
			if (plots <= measured.front().first)
				return measured.front().second;
			for (size_t i = 1; i < measured.size(); ++i)
				if (plots <= measured[i].first)
					return std::lerp(measured[i - 1].second, measured[i].second,
						(plots - measured[i - 1].first) / (measured[i].first - measured[i - 1].first));
			return measured.back().second;
		}();
		const float yaw = m->renderer.camera().yaw;
		const Vec3 towardsScreenBottom{ std::cos(yaw), std::sin(yaw), 0 };
		ButtonArt* art = overlay.buttonArt();
		const auto symbol = [&](const std::string& button) -> const ButtonArt::Icon* {
			if (!art || button.empty())
				return nullptr;
			if (const auto it = m->cityBarSymbols.find(button); it != m->cityBarSymbols.end())
				return it->second.width ? &it->second : nullptr;
			ButtonArt::Icon& out = m->cityBarSymbols[button];
			const ButtonArt::Icon* icon = art->icon(button);
			const ButtonArt::Icon* mask = art->icon("Art/Interface/CityBar/ProductionIconMask.tga");
			if (!icon || !mask || !mask->width || !mask->height)
				return nullptr;
			out = *icon;
			out.key = std::hash<std::string>{}("citybar-symbol:" + button);
			for (uint32_t y = 0; y < out.height; ++y)
				for (uint32_t x = 0; x < out.width; ++x)
				{
					const uint8_t* mp = mask->bgra.data() + (size_t(y * mask->height / out.height) * mask->width + x * mask->width / out.width) * 4;
					const float keep = float(mp[3]) / 255;
					uint8_t* p = out.bgra.data() + (size_t(y) * out.width + x) * 4;
					for (int i = 0; i < 4; ++i)
						p[i] = uint8_t(float(p[i]) * keep + 0.5f);
				}
			return &out;
		};
		m->cityBars.clear();
		for (const CityView& c : m->map.cities)
		{
			if (options.fog && !m->map.plots[size_t(c.x) + size_t(c.y) * size_t(m->map.width)].revealed)
				continue;
			Vec3 p{ (float(c.x) + 0.5f) * m->S(), (float(c.y) + 0.5f) * m->S(), 0 };
			p.x += m->wrapOffset(p.x);
			p.z = m->ground(p.x, p.y);
			p = p + towardsScreenBottom * (radius * civ);
			float ox = 0, oy = 0;
			if (!toScreen(p, ox, oy))
				continue;
			// (a point of the bar's model on the screen)
			const auto X = [&](float u) { return ox + u * k; };
			const auto Y = [&](float v) { return oy - v * k; };

			// (a click on it is one on the city: the population's circle to the production symbol)
			m->cityBars.push_back({ X(-85), Y(0), X(91), Y(-32), c.x, c.y });

			// The banner: its background, the growth (upper half) and production (lower half)
			// bars, then its outline.
			overlay.rect(X(-52.935f), Y(0), X(75.065f), Y(-32), { 0, 0, 0 }, 0.7f, 16 * k);
			for (const auto& [stored, rate, color, top] :
				{ std::tuple{ c.foodStored, c.foodRate, c.foodBar, 0.0f }, std::tuple{ c.productionStored, c.productionRate, c.productionBar, -16.0f } })
			{
				if (stored < 0)
					continue;
				const float b0 = X(-21), b1 = X(60);
				const float s = std::clamp(stored, 0.0f, 1.0f), e = std::clamp(stored + rate, 0.0f, 1.0f);
				overlay.rect(b0, Y(top - 3), b0 + (b1 - b0) * s, Y(top - 13), color, 0.75f);
				overlay.rect(b0 + (b1 - b0) * s, Y(top - 3), b0 + (b1 - b0) * e, Y(top - 13), color, 0.35f);
			}
			overlay.art("Art/Interface/CityBar/CityBar_Base.dds", X(-52.935f), Y(0), X(75.065f), Y(-32));

			// The population: the star of a capital, else the circle, in the game's colour.
			overlay.art(c.capital ? "Art/Interface/CityBar/CityBar_Star.tga" : "Art/Interface/CityBar/CityBar_Circle.tga", X(-85), Y(16), X(-21), Y(-48),
				c.sizeDot);
			const float popSize = 21 * k;
			overlay.text(std::to_wstring(c.population), X(-52.935f), Y(-16) - popSize * 0.62f, popSize, c.sizeText, 1, false, Overlay::Align::Center,
				false);

			// The production symbol: what it builds, cut round, under its bevel.
			if (c.ownTeam)
			{
				const float s = 0.963f;
				const float x0 = X(70.404f - 13 * s), x1 = X(70.404f + 21 * s), y0 = Y(-15.869f + 17 * s), y1 = Y(-15.869f - 17 * s);
				if (const ButtonArt::Icon* icon = symbol(c.productionArt))
					overlay.image(icon->key, icon->bgra, icon->width, icon->height, 0, x0, y0, x1, y1);
				overlay.art("Art/Interface/CityBar/ProductionIconBevel.tga", x0, y0, x1, y1);
			}

			// The name in the upper half, what it builds in the lower, the icons above.
			const std::wstring name = wide(c.name);
			overlay.text(name, X(18.266f), Y(0.411f) + 1.5f * k, 12.5f * k, { 1, 1, 1 }, 1, false, Overlay::Align::Center);
			if (!c.production.empty())
				overlay.richText(wide(c.production), X(18.266f), Y(-15.589f) + 1.5f * k, 11 * k, false, { 1, 1, 1 }, 1, Overlay::Align::Center);
			if (!c.icons.empty())
				overlay.richText(wide(c.icons), X(-0.112f), Y(17.902f), 14 * k, false, { 1, 1, 1 }, 1, Overlay::Align::Center);
		}
	}

	std::optional<std::pair<int, int>> MapView::cityBarAt(float x, float y) const
	{
		for (auto it = m->cityBars.rbegin(); it != m->cityBars.rend(); ++it)
			if (x >= it->x0 && x <= it->x1 && y >= it->y0 && y <= it->y1)
				return std::pair{ it->x, it->y };
		return std::nullopt;
	}

	const std::vector<uint32_t>& MapView::terrainColors() const { return m->terrainColors; }
	std::pair<int, int> MapView::minimapShift() const { return m->shift(); }

	std::optional<std::array<int, 4>> MapView::seenBounds() const { return m->hasMap ? m->seen : std::nullopt; }

	const std::vector<uint8_t>& MapView::minimap(uint64_t& version) const
	{
		version = m->minimapVersion;
		return m->minimapPixels;
	}

	float MapView::plotSize() const { return m->S(); }
	Vec3 MapView::worldSize() const { return { m->worldW(), m->worldH(), 0 }; }
	float MapView::groundHeight(float x, float y) const { return m->ground(x, y); }

	std::optional<std::pair<int, int>> MapView::plotAt(Vec3 world) const
	{
		if (!m->hasMap)
			return std::nullopt;
		int x = int(std::floor(world.x / m->S())), y = int(std::floor(world.y / m->S()));
		if (m->map.wrapX)
			x = (x % m->map.width + m->map.width) % m->map.width;
		if (x < 0 || y < 0 || x >= m->map.width || y >= m->map.height)
			return std::nullopt;
		return std::pair{ x, y };
	}

	const std::vector<std::string>& MapView::problems() const { return m->problems; }
	void MapView::setGroundCircles(std::vector<GroundCircle> circles) { m->groundCircles = std::move(circles); }
	void MapView::setSoundHandler(std::function<void(const std::string&, float, float)> handler) { m->onSound = std::move(handler); }
}
