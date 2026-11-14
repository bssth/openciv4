#include "Renderer.h"

#include "gfx/DdsImage.h"

#include <DirectXMath.h>
#ifdef _WIN32
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif

#include <SDL3/SDL_init.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <stdexcept>

using namespace DirectX;
#ifdef _WIN32
using Microsoft::WRL::ComPtr;
#endif

namespace oc4::render
{
	namespace
	{
#ifdef __APPLE__
		struct MslShader
		{
			const char* file;
			const char* entry;
			const char* source;
		};
		const MslShader kMslShaders[] = {
#include "MslShaders.inc"
		};
#endif
#ifdef OC4_SPIRV_SHADERS
		// Vulkan, chosen with OC4_GPU=vulkan (macOS through MoltenVK, or Windows): the same HLSL, translated
		// ahead of time by tools/shaders/gen_spirv.py.
		struct SpirvShader
		{
			const char* file;
			const char* entry;
			const uint32_t* code;
			size_t size;
		};
#include "SpirvShaders.inc"

		bool useVulkan()
		{
			const char* gpu = std::getenv("OC4_GPU");
			return gpu && std::strcmp(gpu, "vulkan") == 0;
		}
#endif
		constexpr char kShaderBody[] = R"hlsl(

cbuffer Object : register(b1, UNIFORMS)
{
	float4 matDiffuse;  // rgb + alpha
	float4 matAmbient;
	float4 matEmissive;
	float4 teamColor;
	float4 world[3];    // rigid items: 3x4 rows, p' = (dot(row0,p), dot(row1,p), dot(row2,p))
	float4 baseUv[2];   // 2x3 rows: uv' = (dot(row0.xyz, (u,v,1)), dot(row1.xyz, (u,v,1)))
	float4 decalUv[2];
	uint flags;         // bit0 base map, bit1 alpha test, bits2-3 vertex color mode, bit4 lit, bit5 decal, bit6 team colour base, bit7 skinned, bit8 on the ground, bit11 a leader's shading
	uint alphaFunc;
	float alphaRef;
	uint treesCut;      // a forest piece (its vertices' red: the tree, 1-based): trees 1..32 by bit not drawn
};

cbuffer Bones : register(b2, UNIFORMS)
{
	float4 bones[128 * 3]; // world(bone) * skinToBone, 3x4 rows each
};

Texture2D<float> groundHeights : register(t0, space0); // vertex shader: the terrain's heights
SamplerState groundSampler : register(s0, space0);
Texture2D baseMap : register(t0, space2);
Texture2D decalMap : register(t1, space2);
Texture2D fogPlots : register(t2, space2);             // the plots (16-bit unorm), for the fog of war
Texture2D leaderEnv : register(t3, space2);            // a leader's maps (flags bit 11)
Texture2D leaderNormals : register(t4, space2);
Texture2D leaderSpecular : register(t5, space2);
Texture2D leaderEnvMask : register(t6, space2);
SamplerState baseSampler : register(s0, space2);
SamplerState decalSampler : register(s1, space2);

// Height of the terrain under a world position (groundGrid: 1 / cell size, columns, rows, wrap width).
float groundAt(float2 xy)
{
	if (groundGrid.y <= 0)
		return 0;
	if (groundGrid.w > 0)
		xy.x -= floor(xy.x / groundGrid.w) * groundGrid.w;
	return groundHeights.SampleLevel(groundSampler, (xy * groundGrid.x + 0.5) / groundGrid.yz, 0);
}

// What the active team knows where a pixel is, blended between plot centres as on the terrain:
// black where it has not been, dimmed where it does not see now.
float fogAt(float2 xy)
{
	if (fogGrid.x <= 0)
		return 1;
	const int2 size = int2(fogGrid.yz);
	const uint wrap = uint(fogGrid.w);
	const float2 c = xy / fogGrid.x - 0.5;
	const int2 cell = int2(floor(c));
	const float2 w = smoothstep(0, 1, c - cell);
	float2 k[4];
	[unroll] for (int i = 0; i < 4; ++i)
	{
		int2 q = cell + int2(i & 1, i >> 1);
		// (in floats: an integer modulo is slow on some GPUs, Maxwell's among them; exact for map sizes)
		int2 m = q - int2(float2(size) * floor((float2(q) + 0.5) / float2(size)));
		q.x = (wrap & 1) ? m.x : clamp(q.x, 0, size.x - 1);
		q.y = (wrap & 2) ? m.y : clamp(q.y, 0, size.y - 1);
		const uint a = uint(round(fogPlots.Load(int3(q, 0)).a * 65535.0));
		k[i] = float2((a & 4) ? 1 : 0, (a & 8) ? 1 : 0);
	}
	const float2 v = lerp(lerp(k[0], k[1], w.x), lerp(k[2], k[3], w.x), w.y);
	return v.x * lerp(0.55, 1, v.y);
}

float2 uvXform(float4 rows[2], float2 uv) { float3 h = float3(uv, 1); return float2(dot(rows[0].xyz, h), dot(rows[1].xyz, h)); }

struct VSIn
{
	float3 pos : TEXCOORD0;
	float3 nrm : TEXCOORD1;
	float2 uv  : TEXCOORD2;
	float4 col : TEXCOORD3;
	uint4 boneIdx : TEXCOORD4;
	float4 boneW  : TEXCOORD5;
};

struct PSIn
{
	float4 pos : SV_Position;
	float3 nrm : NORMAL;
	float2 uv  : TEXCOORD0;
	float4 col : COLOR0;
	float3 world : TEXCOORD1;
	float2 flat : TEXCOORD2; // the world position before the globe bends it
};

float3 xformPoint(float4 r0, float4 r1, float4 r2, float3 p) { float4 h = float4(p, 1); return float3(dot(r0, h), dot(r1, h), dot(r2, h)); }
float3 xformVector(float4 r0, float4 r1, float4 r2, float3 v) { return float3(dot(r0.xyz, v), dot(r1.xyz, v), dot(r2.xyz, v)); }

PSIn VSMain(VSIn i)
{
	float3 p, n;
	if (flags & 128)
	{
		p = 0;
		n = 0;
		[unroll] for (int k = 0; k < 4; ++k)
		{
			uint b = i.boneIdx[k] * 3;
			p += i.boneW[k] * xformPoint(bones[b], bones[b + 1], bones[b + 2], i.pos);
			n += i.boneW[k] * xformVector(bones[b], bones[b + 1], bones[b + 2], i.nrm);
		}
	}
	else
	{
		p = xformPoint(world[0], world[1], world[2], i.pos);
		n = xformVector(world[0], world[1], world[2], i.nrm);
	}
	if (flags & 256) // laid on the terrain (roads): heights are over it
		p.z += groundAt(p.xy);
	PSIn o;
	o.flat = p.xy;
	p = globeBend(p);
	o.pos = mul(float4(p, 1), viewProj);
	o.nrm = n;
	o.uv = i.uv;
	o.col = i.col;
	o.world = p;
	if (treesCut != 0)
	{
		const uint tree = uint(round(i.col.r * 255));
		if (tree >= 1 && tree <= 32 && ((treesCut >> (tree - 1)) & 1))
			o.pos = float4(0, 0, -1, 1); // (all its vertices to one point behind the near plane: nothing)
	}
	return o;
}

bool passes(uint func, float a, float ref)
{
	switch (func)
	{
	case 0: return true;
	case 1: return a < ref;
	case 2: return a == ref;
	case 3: return a <= ref;
	case 4: return a > ref;
	case 5: return a != ref;
	case 6: return a >= ref;
	default: return false;
	}
}

float4 PSMain(PSIn i, bool front : SV_IsFrontFace) : SV_Target
{
	// A piece laid on the terrain (a river at the map's edge, its delta) ends where the map does.
	// (the map: the terrain's heights grid; around the globe it wraps, and across the poles where
	// the fog of war says so)
	if ((flags & 256) && groundGrid.y > 0)
	{
		const float2 size = (groundGrid.yz - 1) / groundGrid.x;
		const bool wrapY = fogGrid.x > 0 && (uint(fogGrid.w) & 2);
		if ((groundGrid.w <= 0 && (i.flat.x < 0 || i.flat.x > size.x)) || (!wrapY && (i.flat.y < 0 || i.flat.y > size.y)))
			discard;
	}
	float3 n = normalize(i.nrm);
	if (!front)
		n = -n;
	float ndl = saturate(dot(n, -lightDir));

	float3 amb = matAmbient.rgb;
	float3 dif = matDiffuse.rgb;
	float3 emi = matEmissive.rgb;
	float alpha = matDiffuse.a;
	uint vcMode = (flags >> 2) & 3;
	if (vcMode == 1)
		emi = i.col.rgb;
	else if (vcMode == 2)
	{
		amb = i.col.rgb;
		dif = i.col.rgb;
		alpha *= i.col.a;
	}

	float3 lit = (flags & 16) ? emi + amb * ambientLight + dif * lightColor * ndl : emi;
	float4 c = float4(saturate(lit), alpha);
	if (flags & 2048)
	{
		// A leader as Civ4LeaderheadShader.fx lights one: the normal map's normals in the
		// surface's tangent frame (from the derivatives), a key light above left of the camera,
		// a fill from its right, a back light; specular by the intensity map (its red the
		// shininess), the sphere map reflected through the mask; the base map's colour lit.
		const float2 uv = uvXform(baseUv, i.uv);
		const float3 dp1 = ddx(i.world), dp2 = ddy(i.world);
		const float2 du1 = ddx(uv), du2 = ddy(uv);
		const float3 dp2perp = cross(dp2, n), dp1perp = cross(n, dp1);
		float3 t = dp2perp * du1.x + dp1perp * du2.x, b = dp2perp * du1.y + dp1perp * du2.y;
		const float inv = rsqrt(max(max(dot(t, t), dot(b, b)), 1e-20));
		float3 tn = leaderNormals.Sample(baseSampler, uv).xyz * 2 - 1;
		float3 N = normalize(t * inv * tn.x - b * inv * tn.y + n * tn.z);
		const float3 V = normalize(eyePos - i.world);
		const float3 up0 = abs(V.z) < 0.95 ? float3(0, 0, 1) : float3(0, 1, 0);
		const float3 right = normalize(cross(V, up0)), up = cross(right, V);
		const float3 L[3] = { normalize(V - right * 0.6 + up * 0.7), normalize(V + right * 0.9), normalize(-V + up * 0.8) };
		const float3 C[3] = { float3(0.95, 0.92, 0.86), float3(0.30, 0.32, 0.36), float3(0.45, 0.45, 0.5) };
		const float3 spec = leaderSpecular.Sample(baseSampler, uv).rgb;
		float3 diffuse = 0, specular = 0;
		[unroll] for (int k = 0; k < 3; ++k)
		{
			diffuse += saturate(dot(N, L[k])) * C[k];
			specular += pow(saturate(dot(N, normalize(L[k] + V))), max(spec.r * 90, 1)) * C[k] * 0.7;
		}
		const float2 sphere = float2(dot(N, right), -dot(N, up)) * 0.5 + 0.5;
		const float3 env = leaderEnv.Sample(baseSampler, sphere).rgb * leaderEnvMask.Sample(baseSampler, uv).rgb;
		float4 baseColor = (flags & 1) ? baseMap.Sample(baseSampler, uv) : float4(1, 1, 1, 1);
		if (flags & 32)
		{
			const float4 decal = decalMap.Sample(decalSampler, uvXform(decalUv, i.uv));
			baseColor.rgb = lerp(baseColor.rgb, decal.rgb, decal.a);
		}
		float4 o = float4(spec * saturate(specular) + env + (saturate(diffuse) + ambientLight * 0.8) * baseColor.rgb, baseColor.a * alpha);
		if ((flags & 2) && !passes(alphaFunc, round(o.a * 255.0), alphaRef))
			discard;
		if (flags & 1024)
			o.a = 1;
		return o;
	}
	if (flags & 512)
		c = i.col; // (a particle: its colour, unlit)
	float4 base = float4(1, 1, 1, 1);
	if (flags & 64)
		base = teamColor;
	else if (flags & 1)
		base = baseMap.Sample(baseSampler, uvXform(baseUv, i.uv));
	if (flags & 32)
	{
		float4 decal = decalMap.Sample(decalSampler, uvXform(decalUv, i.uv));
		base.rgb = lerp(base.rgb, decal.rgb, decal.a);
	}
	c *= base;
	if ((flags & 2) && !passes(alphaFunc, round(c.a * 255.0), alphaRef))
		discard;
	c.rgb *= fogAt(i.world.xy);
	if (flags & 1024)
		c.a = 1; // (opaque on a picture on nothing: its texture's alpha is no coverage)
	return c;
}
)hlsl";

		constexpr char kOverlayHlsl[] = R"hlsl(
cbuffer Overlay : register(b1, UNIFORMS)
{
	float2 invSize;
	float coverage;
	float pad;
};
Texture2D picture : register(t0, space2);
SamplerState linearClamp : register(s0, space2);
struct In { float2 pos : TEXCOORD0; float2 uv : TEXCOORD1; float4 color : TEXCOORD2; };
struct Out { float4 pos : SV_Position; float2 uv : TEXCOORD0; float4 color : TEXCOORD1; };
Out VSOverlay(In v)
{
	Out o;
	o.pos = float4(v.pos.x * invSize.x * 2 - 1, 1 - v.pos.y * invSize.y * 2, 0, 1);
	o.uv = v.uv;
	o.color = v.color;
	return o;
}
float4 PSOverlay(Out p) : SV_Target
{
	const float4 t = picture.Sample(linearClamp, p.uv);
	return coverage > 0.5 ? p.color * t.r : p.color * t;
}
)hlsl";

		struct FrameConstants
		{
			XMFLOAT4X4 viewProj;
			XMFLOAT3 lightDir;
			float pad0;
			XMFLOAT3 lightColor;
			float pad1;
			XMFLOAT3 ambientLight;
			float pad2;
			XMFLOAT3 eyePos;
			float pad3;
			XMFLOAT4 groundGrid;
			XMFLOAT4 fogGrid;
			XMFLOAT4 globe;
		};

		struct ObjectConstants
		{
			XMFLOAT4 diffuse;
			XMFLOAT4 ambient;
			XMFLOAT4 emissive;
			XMFLOAT4 teamColor;
			XMFLOAT4 world[3];
			XMFLOAT4 baseUv[2];
			XMFLOAT4 decalUv[2];
			uint32_t flags;
			uint32_t alphaFunc;
			float alphaRef;
			uint32_t treesCut;
		};

		struct OverlayConstants
		{
			float invSize[2];
			float coverage, pad;
		};

		// The map transform is affine; recover its matrix from three points.
		void writeUvRows(const scene::UvTransform& t, XMFLOAT4* rows)
		{
			const Vec2 o = t.apply({ 0, 0 }), ex = t.apply({ 1, 0 }), ey = t.apply({ 0, 1 });
			rows[0] = { ex.x - o.x, ey.x - o.x, o.x, 0 };
			rows[1] = { ex.y - o.y, ey.y - o.y, o.y, 0 };
		}

		// Without an alpha property Gamebryo ignores material alpha unless it is animated.
		bool blended(const scene::Material& m) { return m.alphaBlend || (m.alphaAnimated && m.alpha < 0.999f); }

		void writeRows(const Affine& a, XMFLOAT4* rows)
		{
			for (int r = 0; r < 3; ++r)
				rows[r] = { a.rotation.m[r][0] * a.scale, a.rotation.m[r][1] * a.scale, a.rotation.m[r][2] * a.scale,
					r == 0 ? a.translation.x : r == 1 ? a.translation.y : a.translation.z };
		}

		SDL_GPUBlendFactor toSdl(scene::Blend b)
		{
			using B = scene::Blend;
			switch (b)
			{
			case B::One: return SDL_GPU_BLENDFACTOR_ONE;
			case B::Zero: return SDL_GPU_BLENDFACTOR_ZERO;
			case B::SrcColor: return SDL_GPU_BLENDFACTOR_SRC_COLOR;
			case B::InvSrcColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_COLOR;
			case B::DestColor: return SDL_GPU_BLENDFACTOR_DST_COLOR;
			case B::InvDestColor: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_COLOR;
			case B::SrcAlpha: return SDL_GPU_BLENDFACTOR_SRC_ALPHA;
			case B::InvSrcAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
			case B::DestAlpha: return SDL_GPU_BLENDFACTOR_DST_ALPHA;
			case B::InvDestAlpha: return SDL_GPU_BLENDFACTOR_ONE_MINUS_DST_ALPHA;
			case B::SrcAlphaSat: return SDL_GPU_BLENDFACTOR_SRC_ALPHA_SATURATE;
			}
			return SDL_GPU_BLENDFACTOR_ONE;
		}

		SDL_GPUTextureFormat toSdl(TextureFormat f)
		{
			switch (f)
			{
			case TextureFormat::BGRA8: return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
			case TextureFormat::BC1: return SDL_GPU_TEXTUREFORMAT_BC1_RGBA_UNORM;
			case TextureFormat::BC2: return SDL_GPU_TEXTUREFORMAT_BC2_RGBA_UNORM;
			case TextureFormat::BC3: return SDL_GPU_TEXTUREFORMAT_BC3_RGBA_UNORM;
			case TextureFormat::R8: return SDL_GPU_TEXTUREFORMAT_R8_UNORM;
			case TextureFormat::R32Float: return SDL_GPU_TEXTUREFORMAT_R32_FLOAT;
			case TextureFormat::RGBA16: return SDL_GPU_TEXTUREFORMAT_R16G16B16A16_UNORM;
			}
			return SDL_GPU_TEXTUREFORMAT_INVALID;
		}

		TextureFormat toFormat(gfx::PixelFormat f)
		{
			switch (f)
			{
			case gfx::PixelFormat::BC1: return TextureFormat::BC1;
			case gfx::PixelFormat::BC2: return TextureFormat::BC2;
			case gfx::PixelFormat::BC3: return TextureFormat::BC3;
			case gfx::PixelFormat::BGRA8: return TextureFormat::BGRA8;
			}
			return TextureFormat::BGRA8;
		}

		[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::format("{} failed: {}", what, SDL_GetError())); }
	}

	Vec3 Camera::eye() const
	{
		const float cp = std::cos(pitch);
		return target + Vec3{ cp * std::cos(yaw), cp * std::sin(yaw), std::sin(pitch) } * distance;
	}

	Renderer::Renderer(SDL_Window* window, uint32_t width, uint32_t height) : m_window(window), m_width(width), m_height(height)
	{
		static_assert(sizeof(FrameConstants) == sizeof(m_frame));
#ifndef NDEBUG
		const bool debug = true;
#else
		const bool debug = std::getenv("OC4_GPU_DEBUG") != nullptr;
#endif
		// (the GPU devices are the video subsystem's, with a window or without)
		if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
			fail("SDL_InitSubSystem");
#ifdef OC4_SPIRV_SHADERS
		if (useVulkan())
			m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV, debug, "vulkan");
		else
#endif
#ifdef __APPLE__
		m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_MSL, debug, nullptr);
#else
		m_device = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_DXBC, debug, nullptr);
#endif
		if (!m_device)
			fail("SDL_CreateGPUDevice");
#ifdef OC4_SPIRV_SHADERS
		std::printf("  GPU driver: %s\n", SDL_GetGPUDeviceDriver(m_device));
#endif
		if (m_window && !SDL_ClaimWindowForGPUDevice(m_device, m_window))
			fail("SDL_ClaimWindowForGPUDevice");
		if (!SDL_GPUTextureSupportsFormat(m_device, m_depthFormat, SDL_GPU_TEXTURETYPE_2D, SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET))
			m_depthFormat = SDL_GPU_TEXTUREFORMAT_D32_FLOAT_S8_UINT;
		createTargets();
		createPipelines();
	}

	Renderer::~Renderer()
	{
		if (m_cmd)
			SDL_SubmitGPUCommandBuffer(m_cmd);
		SDL_WaitForGPUIdle(m_device);
		for (auto& [key, p] : m_pipelines)
			SDL_ReleaseGPUGraphicsPipeline(m_device, p);
		for (SDL_GPUShader* s : { m_vs, m_ps, m_overlayVs, m_overlayPs })
			SDL_ReleaseGPUShader(m_device, s);
		for (SDL_GPUSampler* s : m_samplers)
			SDL_ReleaseGPUSampler(m_device, s);
		SDL_ReleaseGPUSampler(m_device, m_linearClamp);
		m_textures.clear();
		m_white.reset();
		m_noHeights.reset();
		m_noPlots.reset();
		m_color.reset();
		m_depth.reset();
		m_particleVb.reset();
		m_overlayVb.reset();
		if (m_window)
			SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
		SDL_DestroyGPUDevice(m_device);
		SDL_QuitSubSystem(SDL_INIT_VIDEO);
	}

	void Renderer::createTargets()
	{
		SDL_GPUTextureCreateInfo ci{};
		ci.type = SDL_GPU_TEXTURETYPE_2D;
		ci.format = colorFormat();
		ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ci.width = m_width;
		ci.height = m_height;
		ci.layer_count_or_depth = 1;
		ci.num_levels = 1;
		SDL_GPUTexture* color = SDL_CreateGPUTexture(m_device, &ci);
		if (!color)
			fail("SDL_CreateGPUTexture(color)");
		m_color = std::make_shared<GpuImage>(m_device, color);
		m_color->width = m_width, m_color->height = m_height;
		ci.format = m_depthFormat;
		ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
		SDL_GPUTexture* depth = SDL_CreateGPUTexture(m_device, &ci);
		if (!depth)
			fail("SDL_CreateGPUTexture(depth)");
		m_depth = std::make_shared<GpuImage>(m_device, depth);
		m_target = { m_color->texture, m_depth->texture, m_width, m_height };
	}

	void Renderer::resize(uint32_t width, uint32_t height)
	{
		if (width == 0 || height == 0 || (width == m_width && height == m_height))
			return;
		endPass();
		const bool main = m_target.color == m_color->texture;
		m_width = width;
		m_height = height;
		createTargets();
		if (!main)
			m_target = {};
	}

	SDL_GPUShader* Renderer::compile(std::string_view source, const char* name, const char* entry, SDL_GPUShaderStage stage, uint32_t samplers,
		uint32_t storageBuffers, uint32_t uniformBuffers) const
	{
#ifdef OC4_SPIRV_SHADERS
		if (useVulkan())
		{
			(void)source;
			const SpirvShader* spirv = nullptr;
			for (const SpirvShader& candidate : kSpirvShaders)
				if (std::strcmp(candidate.file, name) == 0 && std::strcmp(candidate.entry, entry) == 0)
					spirv = &candidate;
			if (!spirv)
				throw std::runtime_error(std::format("{} {}: no SPIR-V translation (run tools/shaders/gen_spirv.py)", name, entry));
			SDL_GPUShaderCreateInfo ci{};
			ci.code = reinterpret_cast<const Uint8*>(spirv->code);
			ci.code_size = spirv->size;
			ci.entrypoint = entry;
			ci.format = SDL_GPU_SHADERFORMAT_SPIRV;
			ci.stage = stage;
			ci.num_samplers = samplers;
			ci.num_storage_buffers = storageBuffers;
			ci.num_uniform_buffers = uniformBuffers;
			SDL_GPUShader* shader = SDL_CreateGPUShader(m_device, &ci);
			if (!shader)
				throw std::runtime_error(std::format("{} {}: {}", name, entry, SDL_GetError()));
			return shader;
		}
#endif
#ifdef __APPLE__
		// Metal: the same HLSL, translated ahead of time by tools/shaders/gen_msl.py.
		(void)source;
		const MslShader* msl = nullptr;
		for (const MslShader& candidate : kMslShaders)
			if (std::strcmp(candidate.file, name) == 0 && std::strcmp(candidate.entry, entry) == 0)
				msl = &candidate;
		if (!msl)
			throw std::runtime_error(std::format("{} {}: no Metal translation (run tools/shaders/gen_msl.py)", name, entry));
		SDL_GPUShaderCreateInfo ci{};
		ci.code = reinterpret_cast<const Uint8*>(msl->source);
		ci.code_size = std::strlen(msl->source);
		ci.entrypoint = entry;
		ci.format = SDL_GPU_SHADERFORMAT_MSL;
#else
		const bool vertex = stage == SDL_GPU_SHADERSTAGE_VERTEX;
		const D3D_SHADER_MACRO defines[] = { { "UNIFORMS", vertex ? "space1" : "space3" }, { nullptr, nullptr } };
		ComPtr<ID3DBlob> blob, errors;
		if (FAILED(D3DCompile(source.data(), source.size(), name, defines, nullptr, entry, vertex ? "vs_5_1" : "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
				&blob, &errors)))
			throw std::runtime_error(std::format("{} {}: {}", name, entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "compile failed"));
		SDL_GPUShaderCreateInfo ci{};
		ci.code = static_cast<const Uint8*>(blob->GetBufferPointer());
		ci.code_size = blob->GetBufferSize();
		ci.entrypoint = entry;
		ci.format = SDL_GPU_SHADERFORMAT_DXBC;
#endif
		ci.stage = stage;
		ci.num_samplers = samplers;
		ci.num_storage_buffers = storageBuffers;
		ci.num_uniform_buffers = uniformBuffers;
		SDL_GPUShader* shader = SDL_CreateGPUShader(m_device, &ci);
		if (!shader)
			throw std::runtime_error(std::format("{} {}: {}", name, entry, SDL_GetError()));
		return shader;
	}

	void Renderer::createPipelines()
	{
		const std::string source = std::string(kFrameConstantsHlsl) + kShaderBody;
		m_vs = compile(source, "model.hlsl", "VSMain", SDL_GPU_SHADERSTAGE_VERTEX, 1, 0, 3);
		m_ps = compile(source, "model.hlsl", "PSMain", SDL_GPU_SHADERSTAGE_FRAGMENT, 7, 0, 2);
		m_overlayVs = compile(kOverlayHlsl, "overlay.hlsl", "VSOverlay", SDL_GPU_SHADERSTAGE_VERTEX, 0, 0, 2);
		m_overlayPs = compile(kOverlayHlsl, "overlay.hlsl", "PSOverlay", SDL_GPU_SHADERSTAGE_FRAGMENT, 1, 0, 2);

		for (int clamp = 0; clamp < 4; ++clamp)
		{
			// TexClamp: bit1 = S wraps, bit0 = T wraps.
			SDL_GPUSamplerCreateInfo sd{};
			sd.min_filter = sd.mag_filter = SDL_GPU_FILTER_LINEAR;
			sd.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
			sd.address_mode_u = (clamp & 2) ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			sd.address_mode_v = (clamp & 1) ? SDL_GPU_SAMPLERADDRESSMODE_REPEAT : SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
			sd.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_REPEAT;
			sd.enable_anisotropy = true;
			sd.max_anisotropy = 8;
			sd.max_lod = 1000;
			if (!(m_samplers[clamp] = SDL_CreateGPUSampler(m_device, &sd)))
				fail("SDL_CreateGPUSampler");
		}
		SDL_GPUSamplerCreateInfo gd{};
		gd.min_filter = gd.mag_filter = SDL_GPU_FILTER_LINEAR;
		gd.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_LINEAR;
		gd.address_mode_u = gd.address_mode_v = gd.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
		gd.max_lod = 1000;
		if (!(m_linearClamp = SDL_CreateGPUSampler(m_device, &gd)))
			fail("SDL_CreateGPUSampler(linear)");

		const uint8_t white[4]{ 255, 255, 255, 255 };
		m_white = createImage(1, 1, false, white);
		const float zero = 0;
		const std::span<const uint8_t> heights(reinterpret_cast<const uint8_t*>(&zero), 4);
		m_noHeights = createTexture(1, 1, 1, 1, TextureFormat::R32Float, std::span(&heights, 1));
		const uint16_t seen[4]{ 0, 0, 0, 0xC };
		const std::span<const uint8_t> plots(reinterpret_cast<const uint8_t*>(seen), 8);
		m_noPlots = createTexture(1, 1, 1, 1, TextureFormat::RGBA16, std::span(&plots, 1));
	}

	SDL_GPUGraphicsPipeline* Renderer::pipeline(const PipelineKey& key)
	{
		if (const auto it = m_pipelines.find(key); it != m_pipelines.end())
			return it->second;
		const bool overlay = key.kind == 1;
		SDL_GPUGraphicsPipelineCreateInfo ci{};
		ci.vertex_shader = overlay ? m_overlayVs : m_vs;
		ci.fragment_shader = overlay ? m_overlayPs : m_ps;
		SDL_GPUVertexBufferDescription vb{};
		vb.slot = 0;
		vb.pitch = overlay ? sizeof(OverlayVertex) : sizeof(scene::Vertex);
		vb.input_rate = SDL_GPU_VERTEXINPUTRATE_VERTEX;
		std::vector<SDL_GPUVertexAttribute> attributes;
		if (overlay)
			attributes = { { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(OverlayVertex, x) },
				{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(OverlayVertex, u) },
				{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(OverlayVertex, color) } };
		else
			attributes = { { 0, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(scene::Vertex, position) },
				{ 1, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT3, offsetof(scene::Vertex, normal) },
				{ 2, 0, SDL_GPU_VERTEXELEMENTFORMAT_FLOAT2, offsetof(scene::Vertex, uv) },
				{ 3, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(scene::Vertex, color) },
				{ 4, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4, offsetof(scene::Vertex, bones) },
				{ 5, 0, SDL_GPU_VERTEXELEMENTFORMAT_UBYTE4_NORM, offsetof(scene::Vertex, weights) } };
		ci.vertex_input_state.vertex_buffer_descriptions = &vb;
		ci.vertex_input_state.num_vertex_buffers = 1;
		ci.vertex_input_state.vertex_attributes = attributes.data();
		ci.vertex_input_state.num_vertex_attributes = uint32_t(attributes.size());
		ci.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
		ci.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
		ci.rasterizer_state.cull_mode = key.cull == int(scene::Cull::None) ? SDL_GPU_CULLMODE_NONE
			: key.cull == int(scene::Cull::Front)                          ? SDL_GPU_CULLMODE_FRONT
																		   : SDL_GPU_CULLMODE_BACK;
		ci.rasterizer_state.front_face = SDL_GPU_FRONTFACE_COUNTER_CLOCKWISE; // Gamebryo: right-handed, CCW front faces
		ci.rasterizer_state.enable_depth_clip = true;
		ci.depth_stencil_state.enable_depth_test = key.zTest;
		ci.depth_stencil_state.enable_depth_write = key.zWrite;
		ci.depth_stencil_state.compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
		SDL_GPUColorTargetDescription target{};
		target.format = colorFormat();
		auto& b = target.blend_state;
		b.enable_blend = key.blend;
		b.src_color_blendfactor = SDL_GPUBlendFactor(key.src);
		b.dst_color_blendfactor = SDL_GPUBlendFactor(key.dst);
		b.color_blend_op = b.alpha_blend_op = SDL_GPU_BLENDOP_ADD;
		b.src_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE;
		b.dst_alpha_blendfactor = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		ci.target_info.color_target_descriptions = &target;
		ci.target_info.num_color_targets = 1;
		ci.target_info.depth_stencil_format = m_depthFormat;
		ci.target_info.has_depth_stencil_target = true;
		SDL_GPUGraphicsPipeline* p = SDL_CreateGPUGraphicsPipeline(m_device, &ci);
		if (!p)
			fail("SDL_CreateGPUGraphicsPipeline");
		return m_pipelines[key] = p;
	}

	SDL_GPUGraphicsPipeline* Renderer::modelPipeline(const scene::Material& m, bool particles)
	{
		// Animated alpha on an opaque material fades it with standard blending. Blended geometry
		// writes no depth (no per-triangle sorting yet), but for what its alpha test cuts out (a
		// figure's edges smoothed: the rest of it hides what is behind it).
		PipelineKey key;
		key.blend = blended(m);
		key.src = int(toSdl(m.alphaBlend ? m.srcBlend : scene::Blend::SrcAlpha));
		key.dst = int(toSdl(m.alphaBlend ? m.dstBlend : scene::Blend::InvSrcAlpha));
		key.zTest = m.zTest;
		key.zWrite = !particles && m.zWrite && (!blended(m) || m.alphaTest);
		key.cull = particles ? int(scene::Cull::None) : int(m.cull);
		return pipeline(key);
	}

	SDL_GPUCommandBuffer* Renderer::commands()
	{
		if (m_cmd)
			return m_cmd;
		if (!(m_cmd = SDL_AcquireGPUCommandBuffer(m_device)))
			fail("SDL_AcquireGPUCommandBuffer");
		m_samplersUsed = 0;
		return m_cmd;
	}

	bool Renderer::reserveSamplers(uint32_t count)
	{
		constexpr uint32_t kSamplerHeap = 2048; // (SDL's D3D12 SAMPLER_GPU_DESCRIPTOR_COUNT)
		if (m_samplersUsed + count < kSamplerHeap)
		{
			m_samplersUsed += count;
			return false;
		}
		const bool open = m_pass != nullptr;
		endPass();
		if (m_cmd)
			SDL_SubmitGPUCommandBuffer(m_cmd);
		m_cmd = nullptr;
		commands();
		m_samplersUsed = count;
		if (open)
			pass();
		return true;
	}

	void Renderer::endPass()
	{
		if (!m_pass)
			return;
		SDL_EndGPURenderPass(m_pass);
		m_pass = nullptr;
		m_bound = nullptr;
	}

	SDL_GPURenderPass* Renderer::pass()
	{
		if (m_pass)
			return m_pass;
		if (!m_target.color)
			m_target = { m_color->texture, m_depth->texture, m_width, m_height };
		SDL_GPUColorTargetInfo color{};
		color.texture = m_target.color;
		color.load_op = m_clearColor ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
		color.clear_color = { m_clear[0], m_clear[1], m_clear[2], m_clear[3] };
		color.store_op = SDL_GPU_STOREOP_STORE;
		SDL_GPUDepthStencilTargetInfo depth{};
		depth.texture = m_target.depth;
		depth.load_op = depth.stencil_load_op = m_clearDepth ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
		depth.store_op = depth.stencil_store_op = SDL_GPU_STOREOP_STORE;
		depth.clear_depth = 1;
		m_pass = SDL_BeginGPURenderPass(commands(), &color, 1, &depth);
		if (!m_pass)
			fail("SDL_BeginGPURenderPass");
		m_clearColor = m_clearDepth = false;
		const SDL_GPUViewport vp{ 0, 0, float(m_target.width), float(m_target.height), 0, 1 };
		SDL_SetGPUViewport(m_pass, &vp);
		pushFrameConstants();
		return m_pass;
	}

	void Renderer::pushFrameConstants()
	{
		SDL_PushGPUVertexUniformData(commands(), 0, m_frame, sizeof(m_frame));
		SDL_PushGPUFragmentUniformData(commands(), 0, m_frame, sizeof(m_frame));
	}

	void Renderer::upload(SDL_GPUTexture* texture, uint32_t layer, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* data,
		uint32_t bytes)
	{
		SDL_GPUTransferBufferCreateInfo ti{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes };
		SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(m_device, &ti);
		if (!tb)
			fail("SDL_CreateGPUTransferBuffer");
		std::memcpy(SDL_MapGPUTransferBuffer(m_device, tb, false), data, bytes);
		SDL_UnmapGPUTransferBuffer(m_device, tb);
		endPass();
		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands());
		const SDL_GPUTextureTransferInfo src{ tb, 0, 0, 0 };
		const SDL_GPUTextureRegion dst{ texture, mip, layer, x, y, 0, w, h, 1 };
		SDL_UploadToGPUTexture(copy, &src, &dst, false);
		SDL_EndGPUCopyPass(copy);
		SDL_ReleaseGPUTransferBuffer(m_device, tb);
	}

	void Renderer::upload(SDL_GPUBuffer* buffer, uint32_t offset, const void* data, uint32_t bytes)
	{
		SDL_GPUTransferBufferCreateInfo ti{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, bytes };
		SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(m_device, &ti);
		if (!tb)
			fail("SDL_CreateGPUTransferBuffer");
		std::memcpy(SDL_MapGPUTransferBuffer(m_device, tb, false), data, bytes);
		SDL_UnmapGPUTransferBuffer(m_device, tb);
		endPass();
		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands());
		const SDL_GPUTransferBufferLocation src{ tb, 0 };
		const SDL_GPUBufferRegion dst{ buffer, offset, bytes };
		// (written whole when at 0: the draws before it keep what they had)
		SDL_UploadToGPUBuffer(copy, &src, &dst, offset == 0);
		SDL_EndGPUCopyPass(copy);
		SDL_ReleaseGPUTransferBuffer(m_device, tb);
	}

	std::shared_ptr<GpuImage> Renderer::createTexture(uint32_t width, uint32_t height, uint32_t layers, uint32_t mips, TextureFormat format,
		std::span<const std::span<const uint8_t>> subresources)
	{
		SDL_GPUTextureCreateInfo ci{};
		ci.type = layers > 1 ? SDL_GPU_TEXTURETYPE_2D_ARRAY : SDL_GPU_TEXTURETYPE_2D;
		ci.format = toSdl(format);
		ci.usage = SDL_GPU_TEXTUREUSAGE_SAMPLER;
		ci.width = width;
		ci.height = height;
		ci.layer_count_or_depth = layers;
		ci.num_levels = mips;
		SDL_GPUTexture* texture = SDL_CreateGPUTexture(m_device, &ci);
		if (!texture)
			return nullptr;
		auto image = std::make_shared<GpuImage>(m_device, texture);
		image->width = width, image->height = height, image->layers = layers, image->mips = mips, image->format = format;
		if (subresources.empty())
			return image;
		uint32_t total = 0;
		for (const auto& s : subresources)
			total += uint32_t(s.size());
		SDL_GPUTransferBufferCreateInfo ti{ SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD, total };
		SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(m_device, &ti);
		if (!tb)
			fail("SDL_CreateGPUTransferBuffer");
		auto* mapped = static_cast<uint8_t*>(SDL_MapGPUTransferBuffer(m_device, tb, false));
		uint32_t at = 0;
		for (const auto& s : subresources)
		{
			std::memcpy(mapped + at, s.data(), s.size());
			at += uint32_t(s.size());
		}
		SDL_UnmapGPUTransferBuffer(m_device, tb);
		endPass();
		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(commands());
		at = 0;
		for (size_t i = 0; i < subresources.size() && i < size_t(layers) * mips; ++i)
		{
			const uint32_t layer = uint32_t(i / mips), mip = uint32_t(i % mips);
			const SDL_GPUTextureTransferInfo src{ tb, at, 0, 0 };
			const SDL_GPUTextureRegion dst{ texture, mip, layer, 0, 0, 0, std::max(width >> mip, 1u), std::max(height >> mip, 1u), 1 };
			SDL_UploadToGPUTexture(copy, &src, &dst, false);
			at += uint32_t(subresources[i].size());
		}
		SDL_EndGPUCopyPass(copy);
		SDL_ReleaseGPUTransferBuffer(m_device, tb);
		return image;
	}

	void Renderer::updateTexture(GpuImage& image, uint32_t layer, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* pixels)
	{
		if (width == 0 || height == 0)
			return;
		upload(image.texture, layer, mip, x, y, width, height, pixels, SDL_CalculateGPUTextureFormatSize(toSdl(image.format), width, height, 1));
	}

	std::shared_ptr<GpuBuffer> Renderer::createBuffer(SDL_GPUBufferUsageFlags usage, const void* data, uint32_t size)
	{
		const SDL_GPUBufferCreateInfo ci{ usage, std::max(size, 4u), 0 };
		SDL_GPUBuffer* buffer = SDL_CreateGPUBuffer(m_device, &ci);
		if (!buffer)
			fail("SDL_CreateGPUBuffer");
		auto b = std::make_shared<GpuBuffer>(m_device, buffer, ci.size);
		if (data && size)
			upload(buffer, 0, data, size);
		return b;
	}

	std::shared_ptr<GpuImage> Renderer::createImage(uint32_t width, uint32_t height, bool coverage, const uint8_t* pixels)
	{
		const std::span<const uint8_t> data(pixels, pixels ? size_t(width) * height * (coverage ? 1 : 4) : 0);
		auto image = createTexture(width, height, 1, 1, coverage ? TextureFormat::R8 : TextureFormat::BGRA8,
			pixels ? std::span(&data, 1) : std::span<const std::span<const uint8_t>>{});
		if (image)
			image->coverage = coverage;
		return image;
	}

	const GpuImage* Renderer::texture(const AssetResolver& assets, const AssetLocation& loc, RenderStats* stats)
	{
		if (const auto it = m_textures.find(loc.key); it != m_textures.end())
			return it->second.get();

		std::shared_ptr<GpuImage> image;
		try
		{
			const gfx::Image img = gfx::loadTexture(assets.read(loc));
			// (block-compressed top levels must be multiples of 4)
			const bool bc = img.format != gfx::PixelFormat::BGRA8;
			if (!bc || (img.width % 4 == 0 && img.height % 4 == 0))
			{
				std::vector<std::span<const uint8_t>> levels;
				for (size_t i = 0; i < img.mips.size(); ++i)
				{
					const auto d = img.mipData(i);
					levels.emplace_back(reinterpret_cast<const uint8_t*>(d.data()), d.size());
				}
				image = createTexture(img.width, img.height, 1, uint32_t(img.mips.size()), toFormat(img.format), levels);
			}
		}
		catch (const std::exception&)
		{
		}
		if (stats)
		{
			if (image)
				++stats->textures;
			else
			{
				++stats->missingTextures;
				stats->missing.push_back(loc.key + " (unsupported)");
			}
		}
		auto* raw = image.get();
		m_textures.emplace(loc.key, std::move(image));
		return raw;
	}

	const GpuImage* Renderer::texture(const AssetResolver& assets, const AssetLocation& model, const std::string& name, RenderStats* stats)
	{
		const auto loc = assets.locateTexture(model, name);
		if (!loc)
		{
			if (stats)
			{
				++stats->missingTextures;
				stats->missing.push_back(name);
			}
			return nullptr;
		}
		return texture(assets, *loc, stats);
	}

	std::unique_ptr<GpuModel> Renderer::upload(const scene::Model& model, const AssetResolver& assets, const AssetLocation& modelLocation)
	{
		auto gpu = std::make_unique<GpuModel>();
		RenderStats& stats = gpu->m_stats;
		for (const auto& item : model.items)
		{
			GpuModel::Mesh mesh;
			mesh.vb = createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, item.vertices.data(), uint32_t(item.vertices.size() * sizeof(scene::Vertex)));
			mesh.ib = createBuffer(SDL_GPU_BUFFERUSAGE_INDEX, item.indices.data(), uint32_t(item.indices.size() * sizeof(uint32_t)));
			mesh.indexCount = static_cast<uint32_t>(item.indices.size());
			mesh.material = item.material;
			mesh.node = item.node;
			mesh.skinned = item.skinned;
			mesh.bones = item.bones;
			mesh.skinToBone = item.skinToBone;
			mesh.bindCenter = item.bindBounds.center();
			Bounds local;
			for (const auto& v : item.vertices)
				local.add(v.position);
			mesh.localCenter = local.center();
			if (mesh.skinned && mesh.bones.size() > kMaxBones)
				stats.missing.push_back(std::format("{}: {} bones exceed the palette", item.nodeName, mesh.bones.size()));
			if (!item.material.baseTexture.empty() && !item.material.baseIsTeamColor)
				mesh.texture = texture(assets, modelLocation, item.material.baseTexture, &stats);
			if (!item.material.decalTexture.empty())
				mesh.decal = texture(assets, modelLocation, item.material.decalTexture, &stats);
			// (a leader's maps: all four, or its usual look)
			const std::string* maps[] = { &item.material.envMap, &item.material.normalMap, &item.material.specularMap, &item.material.envMask };
			if (std::ranges::all_of(maps, [](const std::string* s) { return !s->empty(); }))
			{
				for (size_t k = 0; k < 4; ++k)
					mesh.leader[k] = texture(assets, modelLocation, *maps[k], &stats);
				if (!std::ranges::all_of(mesh.leader, [](auto* t) { return t != nullptr; }))
					std::ranges::fill(mesh.leader, nullptr);
			}
			stats.drawItems++;
			stats.vertices += item.vertices.size();
			stats.triangles += item.indices.size() / 3;
			gpu->m_meshes.push_back(std::move(mesh));
		}
		for (const auto& p : model.particles)
			gpu->m_particleTextures.push_back(p.material.baseTexture.empty() ? nullptr : texture(assets, modelLocation, p.material.baseTexture, &stats));
		return gpu;
	}

	void Renderer::bindModelTextures(const GpuImage* base, const GpuImage* decal, const scene::Material& m, const GpuImage* const* leader)
	{
		SDL_GPUSampler* baseSampler = sampler(int(m.baseClamp));
		const auto tex = [&](const GpuImage* i) { return (i ? i : m_white.get())->texture; };
		const SDL_GPUTextureSamplerBinding fragment[7] = {
			{ tex(base), baseSampler },
			{ tex(decal), sampler(int(m.decalClamp)) },
			{ (m_fog ? m_fog : m_noPlots.get())->texture, m_linearClamp },
			{ tex(leader ? leader[0] : nullptr), baseSampler },
			{ tex(leader ? leader[1] : nullptr), baseSampler },
			{ tex(leader ? leader[2] : nullptr), baseSampler },
			{ tex(leader ? leader[3] : nullptr), baseSampler },
		};
		SDL_BindGPUFragmentSamplers(m_pass, 0, fragment, 7);
		const SDL_GPUTextureSamplerBinding vertex{ (m_ground ? m_ground : m_noHeights.get())->texture, m_linearClamp };
		SDL_BindGPUVertexSamplers(m_pass, 0, &vertex, 1);
	}

	void Renderer::drawMesh(const GpuModel::Mesh& mesh, const scene::Material& m, std::span<const Affine> world, Vec3 teamColor, bool onGround)
	{
		reserveSamplers(8);
		pass();
		ObjectConstants oc{};
		writeUvRows(m.baseUv, oc.baseUv);
		writeUvRows(m.decalUv, oc.decalUv);
		oc.diffuse = { m.diffuse.r, m.diffuse.g, m.diffuse.b, m.alpha };
		oc.ambient = { m.ambient.r, m.ambient.g, m.ambient.b, 1 };
		oc.emissive = { m.emissive.r, m.emissive.g, m.emissive.b, 1 };
		oc.teamColor = { teamColor.x, teamColor.y, teamColor.z, 1 };
		writeRows(mesh.node >= 0 && size_t(mesh.node) < world.size() ? world[size_t(mesh.node)] : Affine{}, oc.world);
		oc.flags = (mesh.texture ? 1u : 0u) | (m.alphaTest ? 2u : 0u) | ((m.vertexColorMode & 3u) << 2) | (m.lightingMode ? 16u : 0u) |
			(mesh.decal ? 32u : 0u) | (m.baseIsTeamColor ? 64u : 0u) | (mesh.skinned ? 128u : 0u) | (onGround ? 256u : 0u) |
			(m_inView && m_viewOnNothing && !m.alphaBlend ? 1024u : 0u) | (mesh.leader[0] ? 2048u : 0u);
		oc.alphaFunc = static_cast<uint32_t>(m.alphaFunc);
		oc.alphaRef = m.alphaRef;
		oc.treesCut = m.treesCut;
		SDL_PushGPUVertexUniformData(m_cmd, 1, &oc, sizeof(oc));
		SDL_PushGPUFragmentUniformData(m_cmd, 1, &oc, sizeof(oc));
		if (mesh.skinned)
		{
			static XMFLOAT4 rows[kMaxBones * 3];
			const size_t count = std::min(mesh.bones.size(), kMaxBones);
			for (size_t b = 0; b < count; ++b)
			{
				const size_t node = size_t(mesh.bones[b]);
				writeRows((node < world.size() ? world[node] : Affine{}) * mesh.skinToBone[b], &rows[b * 3]);
			}
			SDL_PushGPUVertexUniformData(m_cmd, 2, rows, uint32_t(std::max<size_t>(count, 1) * 3 * sizeof(XMFLOAT4)));
		}
		if (SDL_GPUGraphicsPipeline* p = modelPipeline(m); p != m_bound)
			SDL_BindGPUGraphicsPipeline(m_pass, m_bound = p);
		bindModelTextures(mesh.texture, mesh.decal, m, mesh.leader[0] ? mesh.leader : nullptr);
		const SDL_GPUBufferBinding vb{ mesh.vb->buffer, 0 }, ib{ mesh.ib->buffer, 0 };
		SDL_BindGPUVertexBuffers(m_pass, 0, &vb, 1);
		SDL_BindGPUIndexBuffer(m_pass, &ib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
		SDL_DrawGPUIndexedPrimitives(m_pass, mesh.indexCount, 1, 0, 0, 0);
	}

	void Renderer::beginFrame(const Camera& camera, Vec3 clearColor)
	{
		endPass();
		m_camera = camera;
		m_deferred.clear();
		m_deferredWorlds.clear();
		m_deferredMaterials.clear();
		m_target = { m_color->texture, m_depth->texture, m_width, m_height };
		m_clearColor = m_clearDepth = true;
		m_clear[0] = clearColor.x, m_clear[1] = clearColor.y, m_clear[2] = clearColor.z, m_clear[3] = 1;
		uploadFrameConstants();
	}

	// The camera, the light, and where the terrain and the fog are: for the frame (again when
	// the fog changes during it).
	void Renderer::uploadFrameConstants()
	{
		const Camera& camera = m_camera;
		const Vec3 eye = camera.eye();
		const float nearZ = std::max(0.01f, camera.distance * 0.01f);
		const XMMATRIX view = XMMatrixLookAtRH(XMVectorSet(eye.x, eye.y, eye.z, 1),
			XMVectorSet(camera.target.x, camera.target.y, camera.target.z, 1), XMVectorSet(0, 0, 1, 0));
		const XMMATRIX proj = XMMatrixPerspectiveFovRH(camera.fovY, float(m_width) / float(m_height), nearZ, camera.distance * 20 + 1000);

		FrameConstants fc{};
		XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(view * proj));
		std::memcpy(m_viewProj, &fc.viewProj, sizeof(m_viewProj));
		const Vec3 light = normalize(Vec3{ -0.4f, -0.6f, -0.7f });
		fc.lightDir = { light.x, light.y, light.z };
		fc.lightColor = { 0.85f, 0.82f, 0.75f };
		fc.ambientLight = { 0.45f, 0.47f, 0.52f };
		fc.eyePos = { eye.x, eye.y, eye.z };
		fc.globe = { m_globe[0], m_globe[1], m_globe[2], m_globe[3] };
		fc.groundGrid = m_ground ? XMFLOAT4{ 1.0f / m_groundCell, float(m_groundColumns), float(m_groundRows), m_groundWrap } : XMFLOAT4{ 0, 0, 0, 0 };
		fc.fogGrid = m_fog ? XMFLOAT4{ m_fogPlot, float(m_fogColumns), float(m_fogRows), float(m_fogWrap) } : XMFLOAT4{ 0, 0, 0, 0 };
		std::memcpy(m_frame, &fc, sizeof(fc));
		if (m_pass)
			pushFrameConstants();
	}

	void Renderer::setFog(const GpuImage* plots, float plotSize, uint32_t columns, uint32_t rows, bool wrapX, bool wrapY)
	{
		const uint32_t wrap = (wrapX ? 1u : 0u) | (wrapY ? 2u : 0u);
		if (m_fog == plots && m_fogPlot == plotSize && m_fogColumns == columns && m_fogRows == rows && m_fogWrap == wrap)
			return;
		m_fog = plots;
		m_fogPlot = plotSize;
		m_fogColumns = columns;
		m_fogRows = rows;
		m_fogWrap = wrap;
		uploadFrameConstants();
	}

	void Renderer::setGround(const GpuImage* heights, float cellSize, uint32_t columns, uint32_t rows, float wrapWidth)
	{
		m_ground = heights;
		m_groundCell = cellSize;
		m_groundColumns = columns;
		m_groundRows = rows;
		m_groundWrap = wrapWidth;
	}

	void Renderer::draw(const GpuModel& model, std::span<const Affine> world, std::span<const uint8_t> visible,
		std::span<const scene::Material> materials, Vec3 teamColor, bool onGround)
	{
		const Vec3 eye = m_inView ? m_viewEye : m_camera.eye();
		const auto centerOf = [&](const GpuModel::Mesh& m) {
			if (!m.skinned && m.node >= 0 && size_t(m.node) < world.size())
				return world[size_t(m.node)].point(m.localCenter);
			// Skinned: the first bone stands for the item.
			return !m.bones.empty() && size_t(m.bones[0]) < world.size() ? world[size_t(m.bones[0])].translation : m.bindCenter;
		};
		size_t deferredWorld = SIZE_MAX;
		// A piece laid on the ground (a river's water and its rapids over it, a road) is drawn in
		// its own order, all at one distance: by each mesh's own centre a river's rapids came
		// before its water whenever they lay beyond it, and the water hid them.
		float groundDistance = -1;
		for (size_t i = 0; i < model.m_meshes.size(); ++i)
		{
			const GpuModel::Mesh& mesh = model.m_meshes[i];
			if (mesh.node >= 0 && size_t(mesh.node) < visible.size() && !visible[size_t(mesh.node)])
				continue;
			const scene::Material& material = i < materials.size() ? materials[i] : mesh.material;
			if (material.alphaAnimated && material.alpha <= 0.001f)
				continue;
			if (!blended(material))
			{
				drawMesh(mesh, material, world, teamColor, onGround);
				continue;
			}
			if (deferredWorld == SIZE_MAX)
			{
				deferredWorld = m_deferredWorlds.size();
				m_deferredWorlds.emplace_back(world.begin(), world.end());
			}
			if (onGround && groundDistance < 0)
				groundDistance = length(centerOf(mesh) - eye);
			m_deferred.push_back({ onGround ? groundDistance : length(centerOf(mesh) - eye), &mesh, deferredWorld, m_deferredMaterials.size(), teamColor, onGround });
			m_deferredMaterials.push_back(material);
		}
	}

	void Renderer::drawParticles(const GpuModel& model, size_t system, const scene::Material& material, std::vector<scene::Vertex>&& vertices)
	{
		if (vertices.empty())
			return;
		ParticleDraw d;
		d.texture = system < model.m_particleTextures.size() ? model.m_particleTextures[system] : nullptr;
		d.material = material;
		d.vertices = std::move(vertices);
		m_particleDraws.push_back(std::move(d));
	}

	void Renderer::endFrame()
	{
		// (the particles' vertices first, in one buffer: an upload ends the pass)
		std::vector<scene::Vertex> particles;
		for (const auto& p : m_particleDraws)
			particles.insert(particles.end(), p.vertices.begin(), p.vertices.end());
		if (!particles.empty())
		{
			const uint32_t bytes = uint32_t(particles.size() * sizeof(scene::Vertex));
			if (!m_particleVb || m_particleVb->size < bytes)
				m_particleVb = createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, nullptr, std::max(bytes * 2, 256u * 1024));
			upload(m_particleVb->buffer, 0, particles.data(), bytes);
		}
		std::ranges::stable_sort(m_deferred, std::greater<>(), &Deferred::distance);
		for (const auto& d : m_deferred)
			drawMesh(*d.mesh, m_deferredMaterials[d.material], m_deferredWorlds[d.world], d.teamColor, d.onGround);
		m_deferred.clear();
		// The particles, last: unlit, their texture by their colour, without writing depth.
		uint32_t first = 0;
		for (auto& p : m_particleDraws)
		{
			reserveSamplers(8);
			pass();
			scene::Material m = p.material;
			if (!m.alphaBlend && !m.alphaTest)
			{
				// (no alpha property: as the game blends a particle's picture)
				m.alphaBlend = true;
				m.srcBlend = scene::Blend::SrcAlpha;
				m.dstBlend = scene::Blend::InvSrcAlpha;
			}
			ObjectConstants oc{};
			writeUvRows(m.baseUv, oc.baseUv);
			writeUvRows(m.decalUv, oc.decalUv);
			oc.diffuse = { 1, 1, 1, m.alpha };
			oc.ambient = { 1, 1, 1, 1 };
			oc.emissive = { 1, 1, 1, 1 };
			oc.teamColor = { 1, 1, 1, 1 };
			writeRows(Affine{}, oc.world);
			oc.flags = (p.texture ? 1u : 0u) | (m.alphaTest ? 2u : 0u) | 512u;
			oc.alphaFunc = static_cast<uint32_t>(m.alphaFunc);
			oc.alphaRef = m.alphaRef;
			SDL_PushGPUVertexUniformData(m_cmd, 1, &oc, sizeof(oc));
			SDL_PushGPUFragmentUniformData(m_cmd, 1, &oc, sizeof(oc));
			if (SDL_GPUGraphicsPipeline* pl = modelPipeline(m, true); pl != m_bound)
				SDL_BindGPUGraphicsPipeline(m_pass, m_bound = pl);
			bindModelTextures(p.texture, nullptr, m, nullptr);
			const SDL_GPUBufferBinding vb{ m_particleVb->buffer, 0 };
			SDL_BindGPUVertexBuffers(m_pass, 0, &vb, 1);
			SDL_DrawGPUPrimitives(m_pass, uint32_t(p.vertices.size()), 1, first, 0);
			first += uint32_t(p.vertices.size());
		}
		m_particleDraws.clear();
	}

	void Renderer::beginView(OffscreenTarget& t, uint32_t width, uint32_t height, std::span<const float, 16> viewProj, Vec3 eye, const ViewLight& light,
		Vec3 clearColor, float clearAlpha)
	{
		width = std::max(width, 1u);
		height = std::max(height, 1u);
		endPass();
		if (!t.color || t.width != width || t.height != height)
		{
			t = {};
			SDL_GPUTextureCreateInfo ci{};
			ci.type = SDL_GPU_TEXTURETYPE_2D;
			ci.format = colorFormat();
			ci.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
			ci.width = width;
			ci.height = height;
			ci.layer_count_or_depth = 1;
			ci.num_levels = 1;
			SDL_GPUTexture* color = SDL_CreateGPUTexture(m_device, &ci);
			if (!color)
				fail("SDL_CreateGPUTexture(view)");
			t.color = std::make_shared<GpuImage>(m_device, color);
			t.color->width = width, t.color->height = height;
			ci.format = m_depthFormat;
			ci.usage = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
			SDL_GPUTexture* depth = SDL_CreateGPUTexture(m_device, &ci);
			if (!depth)
				fail("SDL_CreateGPUTexture(view depth)");
			t.depth = std::make_shared<GpuImage>(m_device, depth);
			t.width = width;
			t.height = height;
		}
		m_inView = true;
		m_viewEye = eye;
		m_deferred.clear();
		m_deferredWorlds.clear();
		m_deferredMaterials.clear();
		m_particleDraws.clear();
		m_viewOnNothing = clearAlpha < 1;
		m_target = { t.color->texture, t.depth->texture, width, height };
		m_clearColor = m_clearDepth = true;
		m_clear[0] = clearColor.x * clearAlpha, m_clear[1] = clearColor.y * clearAlpha, m_clear[2] = clearColor.z * clearAlpha, m_clear[3] = clearAlpha;

		FrameConstants fc{};
		XMFLOAT4X4 m;
		std::memcpy(&m, viewProj.data(), sizeof(m));
		XMStoreFloat4x4(&fc.viewProj, XMMatrixTranspose(XMLoadFloat4x4(&m)));
		const Vec3 dir = normalize(light.direction);
		fc.lightDir = { dir.x, dir.y, dir.z };
		fc.lightColor = { light.color.x, light.color.y, light.color.z };
		fc.ambientLight = { light.ambient.x, light.ambient.y, light.ambient.z };
		fc.eyePos = { eye.x, eye.y, eye.z };
		std::memcpy(m_frame, &fc, sizeof(fc));
	}

	void Renderer::viewLayer()
	{
		if (!m_inView)
			return;
		endFrame();
		pass();
		endPass();
		m_clearDepth = true;
	}

	void Renderer::endView()
	{
		endFrame();
		pass(); // (a view with nothing drawn is still cleared)
		endPass();
		m_inView = false;
		m_target = { m_color->texture, m_depth->texture, m_width, m_height };
		uploadFrameConstants();
	}

	void Renderer::drawOverlay(std::span<const OverlayVertex> vertices, std::span<const OverlayDraw> draws)
	{
		if (vertices.empty() || draws.empty())
			return;
		const uint32_t bytes = uint32_t(vertices.size_bytes());
		if (!m_overlayVb || m_overlayVb->size < bytes)
			m_overlayVb = createBuffer(SDL_GPU_BUFFERUSAGE_VERTEX, nullptr, std::max(bytes * 2, 256u * 1024));
		upload(m_overlayVb->buffer, 0, vertices.data(), bytes);
		pass();
		PipelineKey key;
		key.kind = 1;
		key.blend = true;
		key.src = SDL_GPU_BLENDFACTOR_ONE;
		key.dst = SDL_GPU_BLENDFACTOR_ONE_MINUS_SRC_ALPHA;
		key.zTest = key.zWrite = false;
		key.cull = int(scene::Cull::None);
		SDL_BindGPUGraphicsPipeline(m_pass, m_bound = pipeline(key));
		const SDL_GPUBufferBinding vb{ m_overlayVb->buffer, 0 };
		SDL_BindGPUVertexBuffers(m_pass, 0, &vb, 1);
		int coverage = -1;
		for (const OverlayDraw& d : draws)
		{
			if (reserveSamplers(1))
			{
				SDL_BindGPUGraphicsPipeline(m_pass, m_bound = pipeline(key));
				SDL_BindGPUVertexBuffers(m_pass, 0, &vb, 1);
				coverage = -1;
			}
			const GpuImage& image = d.image ? *d.image : *m_white;
			if (int(image.coverage) != coverage)
			{
				coverage = int(image.coverage);
				const OverlayConstants oc{ { 1.0f / float(m_width), 1.0f / float(m_height) }, float(coverage), 0 };
				SDL_PushGPUVertexUniformData(m_cmd, 1, &oc, sizeof(oc));
				SDL_PushGPUFragmentUniformData(m_cmd, 1, &oc, sizeof(oc));
			}
			const int32_t x0 = std::max(d.clip[0], 0), y0 = std::max(d.clip[1], 0);
			const int32_t x1 = std::min(d.clip[2], int32_t(m_width)), y1 = std::min(d.clip[3], int32_t(m_height));
			if (x1 <= x0 || y1 <= y0)
				continue;
			const SDL_Rect clip{ x0, y0, x1 - x0, y1 - y0 };
			SDL_SetGPUScissor(m_pass, &clip);
			const SDL_GPUTextureSamplerBinding binding{ image.texture, m_linearClamp };
			SDL_BindGPUFragmentSamplers(m_pass, 0, &binding, 1);
			SDL_DrawGPUPrimitives(m_pass, d.count, 1, d.first, 0);
		}
		const SDL_Rect all{ 0, 0, int(m_width), int(m_height) };
		SDL_SetGPUScissor(m_pass, &all);
	}

	void Renderer::present()
	{
		if (m_clearColor || m_clearDepth)
			pass();
		endPass();
		SDL_GPUCommandBuffer* cmd = commands();
		m_cmd = nullptr;
		SDL_GPUTexture* swapchain = nullptr;
		uint32_t w = 0, h = 0;
		if (m_window && SDL_WaitAndAcquireGPUSwapchainTexture(cmd, m_window, &swapchain, &w, &h) && swapchain)
		{
			SDL_GPUBlitInfo blit{};
			blit.source = { m_color->texture, 0, 0, 0, 0, m_width, m_height };
			blit.destination = { swapchain, 0, 0, 0, 0, w, h };
			blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
			blit.filter = SDL_GPU_FILTER_LINEAR;
			SDL_BlitGPUTexture(cmd, &blit);
		}
		SDL_SubmitGPUCommandBuffer(cmd);
	}

	std::vector<uint8_t> Renderer::readback(uint32_t& width, uint32_t& height)
	{
		if (m_clearColor || m_clearDepth)
			pass();
		endPass();
		width = m_width;
		height = m_height;
		const uint32_t bytes = width * height * 4;
		SDL_GPUTransferBufferCreateInfo ti{ SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD, bytes };
		SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(m_device, &ti);
		if (!tb)
			fail("SDL_CreateGPUTransferBuffer(readback)");
		SDL_GPUCommandBuffer* cmd = commands();
		m_cmd = nullptr;
		SDL_GPUCopyPass* copy = SDL_BeginGPUCopyPass(cmd);
		const SDL_GPUTextureRegion src{ m_color->texture, 0, 0, 0, 0, 0, width, height, 1 };
		const SDL_GPUTextureTransferInfo dst{ tb, 0, 0, 0 };
		SDL_DownloadFromGPUTexture(copy, &src, &dst);
		SDL_EndGPUCopyPass(copy);
		SDL_GPUFence* fence = SDL_SubmitGPUCommandBufferAndAcquireFence(cmd);
		if (!fence)
			fail("SDL_SubmitGPUCommandBufferAndAcquireFence");
		SDL_WaitForGPUFences(m_device, true, &fence, 1);
		SDL_ReleaseGPUFence(m_device, fence);
		std::vector<uint8_t> pixels(bytes);
		std::memcpy(pixels.data(), SDL_MapGPUTransferBuffer(m_device, tb, false), bytes);
		SDL_UnmapGPUTransferBuffer(m_device, tb);
		SDL_ReleaseGPUTransferBuffer(m_device, tb);
		for (size_t i = 3; i < pixels.size(); i += 4)
			pixels[i] = 255;
		return pixels;
	}
}
