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
#include <cstring>
#include <format>
#include <functional>
#include <map>
#include <numbers>
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
		constexpr uint16_t kNoOverlay = 0xFFFF;

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
};

Texture2D plots : register(t0, space2);          // 16-bit unorm: terrain, overlay terrain (hills, peaks), owner + 1, flags
Texture2DArray atlas : register(t1, space2);     // 32 tiles per terrain
Texture2DArray decals : register(t2, space2);    // features painted on the ground (flood plains)
Texture2DArray details : register(t3, space2);   // the terrains' detail textures (grey around one half)
Texture2D cells : register(t4, space2);          // 16-bit unorm, two texels a tile: its layers lowest first (atlas slice | rotation << 14), 0xFFFF none
SamplerState atlasSampler : register(s1, space2);
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
};

PSIn VSTerrain(VSIn i)
{
	PSIn o;
	o.pos = mul(float4(globeBend(i.pos + worldOffset), 1), viewProj);
	o.world = i.pos;
	o.nrm = i.nrm;
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

static const int2 kCorner[4] = { int2(0, 1), int2(1, 1), int2(1, 0), int2(0, 0) }; // top left, top right, bottom right, bottom left

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
	float3 detail = 0.5; // blended as the colours are
	float2 dp = p * 3.0; // detail repeats
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
	}

	// The detail textures sharpen the ground up close (their mips fade to grey far away).
	color *= detail * 2;

	// Features painted on the ground: blended between the plot centres that have them, with
	// a ragged edge from the texture's own alpha.
	uint d = max(max(decal[0], decal[1]), max(decal[2], decal[3]));
	if (d > 0)
	{
		float2 w = smoothstep(0, 1, f);
		float4 has = float4(decal[0] == d, decal[1] == d, decal[2] == d, decal[3] == d);
		float amount = lerp(lerp(has[3], has[2], w.x), lerp(has[0], has[1], w.x), w.y);
		float4 tex = decals.Sample(decalSampler, float3(p * 2, d - 1)); // (tiled: wrapping)
		float ragged = decals.Sample(decalSampler, float3(p * 0.37, d - 1)).a;
		color = lerp(color, tex.rgb, smoothstep(0.35, 0.75, amount * (0.55 + 0.6 * ragged)) * 0.9);
	}

	float3 n = normalize(i.nrm);
	float ndl = saturate(dot(n, -lightDir));
	color *= ambientLight * 1.25 + lightColor * ndl * 0.7;

	// Water by its depth over the ground, so that shores have no edge: tint and sun glint.
	float depth = saturate((waterZ - i.world.z) / (0.04 * plotSize));
	if (depth > 0)
	{
		float3 v = normalize(eyePos - (i.world + worldOffset));
		float glint = pow(saturate(dot(reflect(lightDir, float3(0, 0, 1)), v)), 160);
		color = lerp(color, float3(0.08, 0.20, 0.28), depth * 0.35) + glint * 0.18 * depth;
	}

	int2 plot = int2(floor(p));
	float2 g = p - plot;
	uint owner = plotAt(plot).b;
	if (showBorders && owner != 0)
	{
		float3 oc = playerColors[min(owner - 1, 255)].rgb;
		const float w = 0.09;
		float edge = 0;
		if (plotAt(plot + int2(-1, 0)).b != owner)
			edge = max(edge, 1 - g.x / w);
		if (plotAt(plot + int2(1, 0)).b != owner)
			edge = max(edge, 1 - (1 - g.x) / w);
		if (plotAt(plot + int2(0, -1)).b != owner)
			edge = max(edge, 1 - g.y / w);
		if (plotAt(plot + int2(0, 1)).b != owner)
			edge = max(edge, 1 - (1 - g.y) / w);
		// A line along the edge with a glow towards the inside, as the game's borders.
		edge = saturate(edge);
		float stroke = smoothstep(0.72, 0.86, edge);
		color = lerp(color, oc, max(stroke * 0.95, edge * edge * 0.45));
	}
	if (showGrid)
	{
		float2 e = min(g, 1 - g) / max(fwidth(p), 1e-5);
		color *= 1 - 0.35 * (1 - saturate(min(e.x, e.y) - 0.5));
	}
	if (showFog)
	{
		// What the active team knows, blended between plot centres: black where it has not
		// been, dimmed where it does not see now.
		float2 w = smoothstep(0, 1, f);
		float2 k = lerp(lerp(known[3], known[2], w.x), lerp(known[0], known[1], w.x), w.y);
		color *= k.x * lerp(0.55, 1, k.y);
	}
	return float4(color, 1);
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
		std::unique_ptr<GpuModel> gpu;
		std::optional<anim::Sequence> idle, run; // event codes 1000, 1010
		// Fortifying, then fortified: melee (1020, 1021) and ranged (1030, 1031), as the
		// animation categories ANIMCAT_*_FORTIFY and *_FORTIFIED of the first set.
		std::optional<anim::Sequence> fortify[2], fortified[2];
		// Fighting, by animation set (a, b, c; event code + 1000 a set) and kind (melee, ranged):
		// a strike (ANIMCAT_*_STRIKE 22, 32), a death (*_DIE 24, 33) and its fading (25, 34).
		std::optional<anim::Sequence> strike[3][2], die[3][2], dieFade[3][2];
		// Hurt in a round it survives (ANIMCAT_MELEE_HURT 23, by set; the ranged have none of
		// their own); healing (ANIMCAT_HEAL 4).
		std::optional<anim::Sequence> hurt[3][2], heal;
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
		Impl(Renderer& r, const AssetResolver& a) : renderer(r), assets(a) { createPipeline(); }
		~Impl()
		{
			SDL_ReleaseGPUGraphicsPipeline(renderer.device(), terrainPipeline);
			SDL_ReleaseGPUShader(renderer.device(), terrainVS);
			SDL_ReleaseGPUShader(renderer.device(), terrainPS);
			SDL_ReleaseGPUSampler(renderer.device(), sampler);
			SDL_ReleaseGPUSampler(renderer.device(), detailWrap);
		}

		struct Instance
		{
			LoadedModel* model = nullptr;
			Affine place;
			std::vector<Affine> world;    // static instances: the final transforms
			std::vector<uint8_t> visible; // empty: the model's own
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
			int plot = -1;         // the plot it belongs to (fog of war)
			uint64_t unit = 0;     // a unit's figure: owner and id
			bool resting = false, ranged = false; // fortified (and so on); its art's fortify is the ranged one
			// In a battle (into map.battles): its side, its place in the formation (the last
			// ones die first), and (the attacker) the step it takes towards the defender.
			int battle = -1;
			bool attacking = false;
			int figureIndex = 0;
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
		std::shared_ptr<GpuImage> plotTex, cellTex, atlasTex, groundTex, decalTex, detailTex;
		std::vector<uint32_t> terrainColors; // BGRA: the average of each terrain's plain tile
		std::vector<uint8_t> minimapPixels;
		uint64_t minimapVersion = 0;
		std::vector<uint16_t> featureDecal; // by FeatureTypes: 1-based slice of decalTex, 0 none
		SDL_GPUShader* terrainVS = nullptr;
		SDL_GPUShader* terrainPS = nullptr;
		SDL_GPUGraphicsPipeline* terrainPipeline = nullptr;
		SDL_GPUSampler* sampler = nullptr;
		SDL_GPUSampler* detailWrap = nullptr;

		// Models
		std::unordered_map<std::string, std::unique_ptr<LoadedModel>> models;
		std::map<std::string, std::shared_ptr<const nif::File>> kfCache;
		std::vector<Instance> features, decor, cities, units;
		// Each city's pieces by its plot, kept while what it shows stays the same (cleared with the terrain or the art).
		struct CityLayout
		{
			std::string key;
			std::vector<Instance> pieces;
		};
		std::unordered_map<int, CityLayout> cityLayouts;

		// Units walking from plot to plot between two states of the game.
		struct Motion
		{
			Vec3 from, to;
			float start = 0, duration = 0;
		};
		std::unordered_map<uint64_t, Vec3> unitPositions; // where each unit was last shown
		// Where each unit's figure was drawn last (round its middle), for clicks on them.
		struct FigureHit
		{
			Vec3 center;
			float radius = 0;
			int plot = -1;
		};
		std::vector<FigureHit> figureHits;
		bool ringsDrawn = false; // the selection rings are the game's (not the overlay's own)

		// The ring the game puts under the selected units (Art/Units/Selection Effect): its
		// circle turning (the default clip) and its dots over it as the head unit can move
		// (no dots, red, yellow, green: UnitView::moves + 1).
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
				if (!u.selected)
					continue;
				// (its dots: how the head unit can move)
				if (const int d = std::clamp(u.moves + 1, 0, 3); d != ring.shown && ring.dots[d])
				{
					ring.shown = d;
					ring.player.setLayers({ *ring.dots[d] });
				}
				Vec3 at{ (float(u.x) + 0.5f) * S(), (float(u.y) + 0.5f) * S(), 0 };
				const uint64_t key = uint64_t(uint32_t(u.owner)) << 32 | uint32_t(u.id);
				if (const auto it = motions.find(key); it != motions.end() && time < it->second.start + it->second.duration)
					at = motionPoint(it->second);
				at.x += wrapOffset(at.x);
				const anim::Pose pose = ring.player.evaluate(ring.model);
				const auto local = ring.model.worldTransforms(pose.locals);
				// (a little narrower than a plot: its circle is 154 across)
				const Affine place = placement({ at.x, at.y, 0 }, 0, S() / 154.0f * 0.95f);
				std::vector<Affine> world(local.size());
				for (size_t k = 0; k < local.size(); ++k)
					world[k] = place * local[k];
				std::vector<scene::Material> materials;
				for (const auto& item : ring.model.items)
					materials.push_back(anim::animatedMaterial(item, pose));
				renderer.draw(*ring.gpu, world, ring.model.visibility(pose.hidden), materials, kDefaultTeamColor, true);
			}
			return true;
		}
		std::unordered_map<uint64_t, Motion> motions;
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
			SDL_GPUDevice* dev = renderer.device();
			const std::string source = std::string(kFrameConstantsHlsl) + kTerrainHlsl;
			terrainVS = renderer.compile(source, "terrain.hlsl", "VSTerrain", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 2);
			terrainPS = renderer.compile(source, "terrain.hlsl", "PSTerrain", SDL_GPU_SHADERSTAGE_FRAGMENT, 5, 0, 2);
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
			if (!(terrainPipeline = SDL_CreateGPUGraphicsPipeline(dev, &ci)))
				throw std::runtime_error(std::string("terrain pipeline: ") + SDL_GetError());

			SDL_GPUSamplerCreateInfo sd{};
			sd.min_filter = sd.mag_filter = SDL_GPU_FILTER_LINEAR;
			sd.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
			sd.address_mode_u = sd.address_mode_v = sd.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			sd.enable_anisotropy = true;
			sd.max_anisotropy = 8;
			sd.max_lod = 1000;
			sampler = SDL_CreateGPUSampler(dev, &sd);
			sd.address_mode_u = sd.address_mode_v = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
			detailWrap = SDL_CreateGPUSampler(dev, &sd);
			if (!sampler || !detailWrap)
				throw std::runtime_error(std::string("terrain samplers: ") + SDL_GetError());
		}

		// ------------------------------------------------------------ art

		// Every terrain's blend atlas as 32 tiles of kTile pixels in one texture array, so that
		// tiles filter and mip without bleeding into each other.
		void buildAtlas()
		{
			const size_t count = map.terrains.size();
			atlasTex.reset();
			if (count == 0)
				return;
			std::vector<std::vector<uint8_t>> levels(count * kSlots * kTileMips);
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
			if (textures.empty())
				levels.assign(kMips, std::vector<uint8_t>(kSize * kSize * 4, 0)); // the shader still samples one
			const std::vector<std::span<const uint8_t>> subresources(levels.begin(), levels.end());
			decalTex = renderer.createTexture(kSize, kSize, uint32_t(std::max<size_t>(textures.size(), 1)), kMips, TextureFormat::BGRA8, subresources);
		}

		LoadedModel* model(const std::string& nif, const std::string& kfm)
		{
			if (nif.empty())
				return nullptr;
			const std::string key = makeAssetKey(nif);
			auto& slot = models[key + '|' + makeAssetKey(kfm)];
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
				lm.gpu = renderer.upload(lm.model, assets, *loc);
				for (const auto& missing : lm.gpu->stats().missing)
					problem(std::format("{}: texture {}", nif, missing));
				lm.bindLocals = lm.model.bindLocals();
				lm.bindWorld = lm.model.worldTransforms(lm.bindLocals);
				lm.bindVisible = lm.model.visibility(lm.model.bindHidden());
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
				loadIdle(lm, *loc, kfm);
				lm.ok = true;
			}
			catch (const std::exception& e)
			{
				problem(std::format("{}: {}", nif, e.what()));
			}
			return lm.ok ? &lm : nullptr;
		}

		// The idle animation (event 1000) from the art's KFM, or the one next to the model.
		void loadIdle(LoadedModel& lm, const AssetLocation& nifLocation, const std::string& kfm)
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
				std::vector<std::pair<uint32_t, std::optional<anim::Sequence>*>> wanted{ { 1000u, &lm.idle }, { 1010u, &lm.run }, { 1020u, &lm.fortify[0] },
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
				wanted.emplace_back(1004u, &lm.heal);
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
					decal << 8);
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
			const auto add = [&](const FeatureArt& art, const FeatureArt::Piece& piece, Vec3 at, uint32_t h) {
				if (piece.models.empty())
					return;
				const auto& [nif, rotation] = piece.models[h % piece.models.size()];
				// Some pieces are textures laid on the terrain (flood plains): not drawn yet.
				if (!toLowerAscii(nif).ends_with(".nif"))
					return;
				if (LoadedModel* lm = model(nif, {}))
				{
					at.z = ground(at.x, at.y);
					features.push_back(placeStatic(lm, placement(at, -float(rotation) * std::numbers::pi_v<float> / 2, art.scale)));
				}
			};

			// Trees and ice tile by plot corners: each corner takes the piece for the plots
			// around it that have the feature.
			const int cornersX = map.wrapX ? W : W + 1, cornersY = map.wrapY ? H : H + 1;
			for (int cy = 0; cy < cornersY; ++cy)
				for (int cx = 0; cx < cornersX; ++cx)
				{
					const PlotView* around[4] = { map.plot(cx - 1, cy), map.plot(cx, cy), map.plot(cx, cy - 1), map.plot(cx - 1, cy - 1) };
					const int bits[4] = { kNorthWest, kNorthEast, kSouthEast, kSouthWest };
					int done[4] = { -1, -1, -1, -1 };
					for (int k = 0; k < 4; ++k)
					{
						const int f = around[k] ? around[k]->feature : -1;
						const FeatureArt* art = featureArt(f);
						if (!art || (art->tiling != FeatureArt::Tiling::Trees && art->tiling != FeatureArt::Tiling::HalfTiling) ||
							std::find(std::begin(done), std::end(done), f) != std::end(done))
							continue;
						done[k] = f;
						int mask = 0;
						for (int j = 0; j < 4; ++j)
							if (around[j] && around[j]->feature == f)
								mask |= bits[j];
						if (art->varieties.empty())
							continue;
						const size_t variety = std::min<size_t>(around[k]->featureVariety, art->varieties.size() - 1);
						if (const auto* piece = findPiece(art->varieties[variety], mask))
							add(*art, *piece, { float(cx) * S(), float(cy) * S(), 0 }, hash(cx, cy, f));
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
					if (piece)
						add(*art, *piece, { (float(x) + 0.5f) * S(), (float(y) + 0.5f) * S(), 0 }, hash(x, y, p.feature));
				}
		}

		// A model of the snapshot standing at a point, animated when it has an idle animation.
		void addDecor(int index, Vec3 at, float yaw, uint32_t h)
		{
			if (index < 0 || size_t(index) >= map.models.size())
				return;
			const ModelArt& art = map.models[size_t(index)];
			LoadedModel* lm = model(art.nif, art.kfm);
			if (!lm)
				return;
			at.z = ground(at.x, at.y);
			if (!lm->idle)
			{
				decor.push_back(placeStatic(lm, placement(at, yaw, art.scale)));
				return;
			}
			Instance inst;
			inst.model = lm;
			inst.place = placement(at, yaw, art.scale);
			inst.center = inst.place.point(lm->bounds.center());
			inst.radius = lm->bounds.radius() * art.scale + 1;
			inst.animated = true;
			inst.phase = unitFloat(h) * 10;
			decor.push_back(std::move(inst));
		}

		// A piece laid on the terrain (roads, rivers): heights over it, a little above it
		// against z-fighting; turned clockwise.
		void addOnGround(const std::string& nif, Vec3 at, int degrees, float lift)
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
				}
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
			const auto place = [&](const RouteArt& r, int degrees) { addOnGround(r.nif, center, degrees, 0.6f); };
			const auto find = [&](int wanted) -> std::pair<const RouteArt*, int> {
				for (const RouteArt& r : map.routes)
					if (r.route == p.route)
						for (const int degrees : r.rotations.empty() ? std::vector<int>{ 0 } : r.rotations)
							// (some files the XML lists do not exist, in the original either)
							if (turned(r.connections, degrees) == wanted && assets.locate(makeAssetKey(r.nif)) && model(r.nif, {}))
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

		void buildDecor()
		{
			decor.clear();
			addRivers();
			for (int y = 0; y < map.height; ++y)
				for (int x = 0; x < map.width; ++x)
				{
					const PlotView& p = map.plots[size_t(x) + size_t(y) * size_t(map.width)];
					const Vec3 center{ (float(x) + 0.5f) * S(), (float(y) + 0.5f) * S(), 0 };
					if (p.route >= 0)
						addRoute(x, y, p);
					if (p.improvement >= 0 && size_t(p.improvement) < map.improvements.size())
						addDecor(map.improvements[size_t(p.improvement)], center, 0, hash(x, y, 7));
					if (p.bonus >= 0 && size_t(p.bonus) < map.bonuses.size())
						addDecor(map.bonuses[size_t(p.bonus)], center, 0, hash(x, y, 8));
				}
		}

		// Trees inside the tree cut spheres of the improvements, resources and roads go.
		void cutTrees()
		{
			std::unordered_map<int, std::vector<std::pair<Vec3, float>>> cuts; // by plot
			const auto plotKey = [&](float x, float y) {
				int px = int(std::floor(x / S())), py = int(std::floor(y / S()));
				if (map.wrapX)
					px = (px % map.width + map.width) % map.width;
				return px + py * map.width;
			};
			for (const Instance& d : decor)
				for (const auto& [c, r] : d.model->treeCuts)
				{
					const Vec3 w = d.place.point(c);
					cuts[plotKey(w.x, w.y)].push_back({ w, r * d.place.scale });
				}
			if (cuts.empty())
				return;
			for (Instance& f : features)
			{
				const LoadedModel& lm = *f.model;
				bool changed = false;
				std::vector<uint8_t> visible = f.visible.empty() ? lm.bindVisible : f.visible;
				for (size_t i = 0; i < f.world.size(); ++i)
				{
					if (lm.model.nodes[i].parent != lm.spread || lm.spread < 0)
						continue;
					const Vec3 p = f.world[i].translation;
					bool cut = false;
					for (int dy = -1; dy <= 1 && !cut; ++dy)
						for (int dx = -1; dx <= 1 && !cut; ++dx)
							if (const auto it = cuts.find(plotKey(p.x + float(dx) * S(), p.y + float(dy) * S())); it != cuts.end())
								for (const auto& [c, r] : it->second)
									if (std::hypot(p.x - c.x, p.y - c.y) < r)
									{
										cut = true;
										break;
									}
					if (!cut)
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

		Vec3 motionPoint(const Motion& m) const
		{
			const float t = m.duration > 0 ? std::clamp((time - m.start) / m.duration, 0.0f, 1.0f) : 1.0f;
			return lerp(m.from, m.to, t);
		}

		// Units: the plot's centre unit, as many figures as its formation has.
		void buildUnits()
		{
			units.clear();
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
				// A unit seen on another plot before walks there (the whole way, if it is near).
				const uint64_t key = uint64_t(uint32_t(u.owner)) << 32 | uint32_t(u.id);
				if (const auto it = unitPositions.find(key); it != unitPositions.end() && u.id >= 0)
				{
					Vec3 from = it->second;
					if (const auto walking = motions.find(key); walking != motions.end())
						from = motionPoint(walking->second);
					Vec3 d = center - from;
					if (map.wrapX && std::abs(d.x) > worldW() / 2)
						d.x -= std::copysign(worldW(), d.x);
					const float plots = length(d) / S();
					if (plots > 0.05f && plots < 3.5f)
						motions[key] = { center - d, center, time, 0.25f + 0.3f * plots };
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
				for (size_t i = 0; i < n; ++i)
				{
					if (u.models[i] < 0 || size_t(u.models[i]) >= map.models.size())
						continue;
					const ModelArt& art = map.models[size_t(u.models[i])];
					LoadedModel* lm = model(art.nif, art.kfm);
					if (!lm)
						continue;
					const float ox = (float(int(i) % columns) - float(columns - 1) / 2) * spacing;
					const float oy = (float(int(i) / columns) - float(rows - 1) / 2) * spacing;
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
					inst.battle = u.battle, inst.attacking = u.attacking, inst.figureIndex = int(i);
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
					if (u.battle >= 0 && u.attacking && size_t(u.battle) < map.battles.size())
					{
						const BattleView& b = map.battles[size_t(u.battle)];
						Vec3 d{ float(b.defenderX - b.attackerX) * S(), float(b.defenderY - b.attackerY) * S(), 0 };
						if (map.wrapX && std::abs(d.x) > worldW() / 2)
							d.x -= std::copysign(worldW(), d.x);
						if (const float len = length(d); len > 0)
							inst.step = d * (0.4f * S() / len);
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

		void drawTerrain(const Frustum& frustum, const MapView::Options& options)
		{
			if (!vb || !atlasTex || !plotTex || !detailTex || !decalTex || !cellTex)
				return;
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
			for (size_t i = 0; i < std::min(map.players.size(), kMaxPlayers); ++i)
			{
				const Color& c = map.players[i].primary;
				tc.playerColors[i][0] = c.r;
				tc.playerColors[i][1] = c.g;
				tc.playerColors[i][2] = c.b;
				tc.playerColors[i][3] = 1;
			}

			std::vector<float> copies{ 0 };
			if (map.wrapX)
				copies = { -worldW(), 0, worldW() };
			const auto visibleCopy = [&](float dx) {
				const Vec3 c{ worldW() / 2 + dx, worldH() / 2, 0 };
				return frustum.sphere(c, length(Vec3{ worldW(), worldH(), 0 }) / 2 + map.peakScale * S());
			};
			renderer.reserveSamplers(5);
			SDL_GPURenderPass* pass = renderer.pass();
			renderer.bindPipeline(terrainPipeline);
			const SDL_GPUTextureSamplerBinding textures[5] = { { plotTex->texture, sampler }, { atlasTex->texture, sampler },
				{ decalTex->texture, detailWrap }, { detailTex->texture, detailWrap }, { cellTex->texture, sampler } };
			SDL_BindGPUFragmentSamplers(pass, 0, textures, 5);
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
		BattlePose battlePose(const Instance& inst) const
		{
			BattlePose p;
			if (inst.battle < 0 || size_t(inst.battle) >= map.battles.size())
				return p;
			const BattleView& b = map.battles[size_t(inst.battle)];
			const auto began = battleStarts.find(b.serial);
			if (began == battleStarts.end() || b.rounds.empty())
				return p;
			const float slice = kBattleSliceSeconds, t = time - began->second, end = float(b.slices) * slice;
			if (t < 0 || t > end + 1)
				return p; // (over: as usual again)
			p.fighting = true;
			p.step = inst.attacking ? std::clamp(t / 0.6f, 0.0f, 1.0f) * std::clamp((end - t) / 0.6f, 0.0f, 1.0f) : 0;
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
			// A round on: it strikes (the figures a little one after another), or now and then,
			// in a melee round, it is hit and staggers.
			if (t >= roundsAt)
				if (const int r = int((t - roundsAt) / roundLength); r < int(b.rounds.size()))
				{
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
			if (p.clip && (p.dying || inst.figureIndex < 2))
				clipSounds(inst, *p.clip, p.at);
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
				// (the script is the last of its comma-separated parts that names one)
				std::string tag;
				size_t begin = 6;
				while (begin <= text.size())
				{
					const size_t comma = std::min(text.find(',', begin), text.size());
					std::string part = text.substr(begin, comma - begin);
					while (!part.empty() && part.front() == ' ')
						part.erase(part.begin());
					if (part.rfind("AS", 0) == 0)
						tag = part;
					begin = comma + 1;
				}
				if (!tag.empty())
					onSound(tag, inst.soundAt.x, inst.soundAt.y);
			}
		}

		// A model's particles under that key, on since it was last drawn (from a while back when
		// first seen: the smoke already up), as quads facing the camera.
		void drawParticlesOf(const LoadedModel& lm, uint64_t key, std::span<const Affine> world, std::span<const uint8_t> visible)
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
					renderer.drawParticles(*lm.gpu, i, lm.model.particles[i].material, std::move(quads));
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

		void drawEffects(const Instance& inst, std::span<const Affine> world, std::span<const uint8_t> visible)
		{
			if (!showEffects)
				return;
			LoadedModel& lm = *inst.model;
			if (lm.model.particles.empty() && lm.attachTags.empty())
				return;
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
				addBurst(model(art->nif, ""), at * placement({}, 0, art->scale > 0 ? art->scale : 1));
			}
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
				world = lm.model.worldTransforms(pose.locals);
				for (auto& w : world)
					w = b.place * w;
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
		}

		void drawInstances(const std::vector<Instance>& list, const Frustum& frustum, bool needsSight = false)
		{
			std::vector<Affine> world;
			std::vector<scene::Material> materials;
			const float zoom = figureZoom();
			for (const Instance& inst : list)
			{
				if (fogged(inst, needsSight))
					continue;
				const Motion* motion = nullptr;
				if (inst.unit)
					if (const auto it = motions.find(inst.unit); it != motions.end() && time < it->second.start + it->second.duration)
						motion = &it->second;
				Vec3 anchor = inst.anchor;
				float yaw = inst.yaw;
				// Fighting: the attacker steps up to the defender; the dead are gone.
				const BattlePose battle = inst.figure ? battlePose(inst) : BattlePose{};
				if (battle.gone)
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
					anchor = motionPoint(*motion) + (inst.anchor - motion->to);
					if (std::abs(d.x) + std::abs(d.y) > 0.01f)
						yaw = std::atan2(d.y, d.x) + std::numbers::pi_v<float> / 2;
				}
				const Vec3 center = motion || battle.fighting || flying ? anchor : inst.center;
				const float dx = wrapOffset(center.x);
				if (!frustum.sphere(center + Vec3{ dx, 0, 0 }, inst.radius * (inst.figure ? zoom : 1)))
					continue;
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
					renderer.draw(*lm.gpu, world, lm.bindVisible, {}, inst.team, inst.onGround);
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
					else if (battle.fighting && inst.attacking && battle.step > 0 && battle.step < 1 && lm.run)
						lm.run->apply(time, pose); // (stepping up, or back)
					else if (motion && lm.run)
					{
						lm.run->apply(time - motion->start, pose);
						// (its effects, the footprints: each time round)
						if (const float d = lm.run->duration(); d > 0)
							clipSounds(inst, *lm.run, std::fmod(time - motion->start, d), false);
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
						lm.idle->apply(frozen ? 0 : time + inst.phase, pose);
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
						spawnKeyEffects(lm, world);
					materials.clear();
					if (std::ranges::any_of(pose.materials, [](const anim::MaterialOverride& o) { return o.any; }))
						for (const auto& item : lm.model.items)
							materials.push_back(anim::animatedMaterial(item, pose));
					const auto shownNodes = lm.model.visibility(pose.hidden);
					renderer.draw(*lm.gpu, world, shownNodes, materials, inst.team, inst.onGround);
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
					renderer.draw(*lm.gpu, world, shown, materials, inst.team, inst.onGround);
					drawEffects(inst, world, shown);
					continue;
				}
				if (dx == 0)
				{
					renderer.draw(*lm.gpu, inst.world, visible, {}, inst.team, inst.onGround);
					drawEffects(inst, inst.world, visible);
					continue;
				}
				world = inst.world;
				for (auto& w : world)
					w.translation.x += dx;
				renderer.draw(*lm.gpu, world, visible, {}, inst.team, inst.onGround);
				drawEffects(inst, world, visible);
			}
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
			decorKey[i] = uint64_t(uint16_t(p.improvement)) | uint64_t(uint16_t(p.bonus)) << 16 | uint64_t(uint16_t(p.route)) << 32 |
				uint64_t(p.riverSouth) << 48 | uint64_t(p.riverEast) << 49;
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
			m->artGeneration = snapshot.artGeneration;
		}
		if (reshaped)
		{
			m->shape = std::move(shape);
			m->buildTerrain();
		}
		m->updatePlots();
		// Improvements, resources and roads first: they clear trees from the features.
		if (redecorated)
		{
			m->decorKey = std::move(decorKey);
			m->buildDecor();
		}
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
		std::erase_if(m->motions, [&](const auto& e) { return m->time > e.second.start + e.second.duration; });
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
		if (options.features)
			m->drawInstances(m->features, frustum);
		if (options.improvements)
			m->drawInstances(m->decor, frustum);
		if (options.cities)
			m->drawInstances(m->cities, frustum);
		m->figureHits.clear();
		if (options.units)
		{
			m->drawInstances(m->units, frustum, true);
			m->ringsDrawn = m->drawRings();
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
		if (!m->hasMap || !options.banners || !options.cities || m->renderer.globe())
			return;
		const float* vp = m->renderer.viewProjection();
		const float distance = m->renderer.camera().distance / m->S();
		if (distance > 45)
			return;
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
		// The units' flags: the owner's colours, health, how many are on the plot.
		if (options.flags && options.units && distance < 22)
			for (const UnitView& u : m->map.units)
			{
				const int plot = u.x + u.y * m->map.width;
				if (options.fog && !m->map.plots[size_t(plot)].visible)
					continue;
				Vec3 p{ (float(u.x) + 0.5f) * m->S(), (float(u.y) + 0.5f) * m->S(), 0 };
				const uint64_t key = uint64_t(uint32_t(u.owner)) << 32 | uint32_t(u.id);
				if (const auto it = m->motions.find(key); it != m->motions.end() && m->time < it->second.start + it->second.duration)
					p = m->motionPoint(it->second);
				p.x += m->wrapOffset(p.x);
				if (u.selected && !m->ringsDrawn)
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
				p.z = m->ground(p.x, p.y) + 0.3f * m->S() * m->figureZoom();
				float x = 0, y = 0;
				if (!toScreen(p, x, y))
					continue;
				const PlayerView* owner = u.shownOwner >= 0 && size_t(u.shownOwner) < m->map.players.size() ? &m->map.players[size_t(u.shownOwner)] : nullptr;
				const Color primary = owner ? owner->primary : Color{}, secondary = owner ? owner->secondary : Color{ 0, 0, 0 };
				const float fw = 9 * scale, fh = 12 * scale;
				overlay.line(x - fw / 2, y - fh, x - fw / 2, y + fh * 0.6f, { 0.15f, 0.12f, 0.1f }, 0.9f, 1.2f);
				overlay.rect(x - fw / 2, y - fh, x + fw / 2, y, primary, 0.95f);
				overlay.frame(x - fw / 2, y - fh, x + fw / 2, y, secondary, 0.95f, 0, 1.2f);
				const float health = std::clamp(u.health, 0.0f, 1.0f);
				if (options.healthBars)
				{
					overlay.rect(x - fw / 2, y + 1, x + fw / 2, y + 3.5f * scale, { 0, 0, 0 }, 0.7f);
					overlay.rect(x - fw / 2, y + 1, x - fw / 2 + fw * health, y + 3.5f * scale, { 1 - health, 0.3f + 0.6f * health, 0.15f }, 0.95f);
				}
				if (u.count > 1)
					overlay.text(std::to_wstring(u.count), x + fw / 2 + 2, y - fh - 1, 10 * scale, { 1, 1, 1 }, 0.95f, true);
			}
		for (const CityView& c : m->map.cities)
		{
			if (options.fog && !m->map.plots[size_t(c.x) + size_t(c.y) * size_t(m->map.width)].revealed)
				continue;
			Vec3 p{ (float(c.x) + 0.5f) * m->S(), (float(c.y) + 0.5f) * m->S(), 0 };
			p.x += m->wrapOffset(p.x);
			p.z = m->ground(p.x, p.y) + 0.35f * m->S();
			const float cw = vp[12] * p.x + vp[13] * p.y + vp[14] * p.z + vp[15];
			if (cw <= 0)
				continue;
			const float sx = (vp[0] * p.x + vp[1] * p.y + vp[2] * p.z + vp[3]) / cw, sy = (vp[4] * p.x + vp[5] * p.y + vp[6] * p.z + vp[7]) / cw;
			const float x = (sx + 1) * 0.5f * W, y = (1 - sy) * 0.5f * H;
			if (x < -200 || x > W + 200 || y < -60 || y > H + 60)
				continue;

			const Color owner = c.owner >= 0 && size_t(c.owner) < m->map.players.size() ? m->map.players[size_t(c.owner)].primary : Color{};
			const std::wstring name = (c.capital ? L"\u2605 " : L"") + wide(c.name);
			const std::wstring pop = std::to_wstring(c.population);
			const float size = 13 * scale, h = 20 * scale, r = h / 2;
			const float nameW = overlay.width(name, size, true), popR = r + 1;
			const float w = popR * 2 + 6 * scale + nameW + 10 * scale;
			const float x0 = x - w / 2, y0 = y - h / 2;
			overlay.rect(x0, y0, x0 + w, y0 + h, { 0.05f, 0.06f, 0.08f }, 0.72f, r);
			overlay.frame(x0, y0, x0 + w, y0 + h, owner, 0.9f, r, 1.5f);
			overlay.circle(x0 + popR, y, popR, c.sizeDot, 1);
			overlay.text(pop, x0 + popR, y - size * 0.68f, size * 0.95f, c.sizeText, 1, true, Overlay::Align::Center, false);
			overlay.text(name, x0 + popR * 2 + 6 * scale, y - size * 0.68f, size, { 1, 1, 1 }, 1, true);
			// The bars under the banner, as the game's.
			float by = y0 + h + 2;
			const float bx0 = x0 + popR * 2, bx1 = x0 + w - r * 0.6f, bh = 3.5f * scale;
			for (const auto& [stored, rate, color] :
				{ std::tuple{ c.foodStored, c.foodRate, c.foodBar }, std::tuple{ c.productionStored, c.productionRate, c.productionBar } })
			{
				if (stored < 0)
					continue;
				overlay.rect(bx0, by, bx1, by + bh, { 0, 0, 0 }, 0.6f);
				const float s = std::clamp(stored, 0.0f, 1.0f), e = std::clamp(stored + rate, 0.0f, 1.0f);
				overlay.rect(bx0, by, bx0 + (bx1 - bx0) * s, by + bh, color, 0.95f);
				overlay.rect(bx0 + (bx1 - bx0) * s, by, bx0 + (bx1 - bx0) * e, by + bh, color, 0.45f);
				by += bh + 1;
			}
			if (!c.production.empty() && distance < 25)
				overlay.richText(wide(c.production), x, by, size * 0.8f, false, { 0.85f, 0.88f, 0.95f }, 0.95f, Overlay::Align::Center);
		}
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
	void MapView::setSoundHandler(std::function<void(const std::string&, float, float)> handler) { m->onSound = std::move(handler); }
}
