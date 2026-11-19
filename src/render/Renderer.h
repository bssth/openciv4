#pragma once

// Renderer for scene::Model instances through SDL's GPU API: models are uploaded once and drawn
// any number of times per frame with their own node transforms, visibility, animated materials
// and team colour. Blended items of all instances are drawn last, back to front. Other passes
// (the map's terrain) draw between beginFrame and endFrame in pass() and share the frame
// constants (kFrameConstantsHlsl, uniform slot 0 of both stages).

#include "core/AssetResolver.h"
#include "scene/NifScene.h"

#include <SDL3/SDL_gpu.h>

#include <cstdint>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <vector>

struct SDL_Window;

namespace oc4::render
{
	struct Camera
	{
		Vec3 target;
		float yaw = 0.6f;    // radians around Z (Civ4 is Z-up)
		float pitch = 0.35f; // radians above the XY plane
		float distance = 100;
		float fovY = 0.75f;

		Vec3 eye() const;
		// The screen's up in the world: towards where the camera looks along the ground, so that
		// it still holds looking straight down.
		Vec3 up() const;
	};

	struct RenderStats
	{
		size_t drawItems = 0, vertices = 0, triangles = 0;
		size_t textures = 0, missingTextures = 0;
		std::vector<std::string> missing;
	};

	enum class TextureFormat : uint8_t
	{
		// RGBA16 is unorm: SDL GPU cannot sample integer formats, so shaders read whole values as round(v * 65535).
		BGRA8, BC1, BC2, BC3, R8, R32Float, RGBA16
	};

	// A texture (2D or an array of them) on the GPU, released with its last owner.
	class GpuImage
	{
	public:
		GpuImage(SDL_GPUDevice* device, SDL_GPUTexture* texture) : device(device), texture(texture) {}
		~GpuImage() { SDL_ReleaseGPUTexture(device, texture); }
		GpuImage(const GpuImage&) = delete;
		GpuImage& operator=(const GpuImage&) = delete;

		SDL_GPUDevice* device;
		SDL_GPUTexture* texture;
		uint32_t width = 0, height = 0, layers = 1, mips = 1;
		TextureFormat format = TextureFormat::BGRA8;
		bool coverage = false; // (the overlay's: one byte of coverage a pixel)
	};

	class GpuBuffer
	{
	public:
		GpuBuffer(SDL_GPUDevice* device, SDL_GPUBuffer* buffer, uint32_t size) : device(device), buffer(buffer), size(size) {}
		~GpuBuffer() { SDL_ReleaseGPUBuffer(device, buffer); }
		GpuBuffer(const GpuBuffer&) = delete;
		GpuBuffer& operator=(const GpuBuffer&) = delete;

		SDL_GPUDevice* device;
		SDL_GPUBuffer* buffer;
		uint32_t size;
	};

	// A model's draw items on the GPU.
	class GpuModel
	{
	public:
		const RenderStats& stats() const { return m_stats; }

	private:
		friend class Renderer;

		struct Mesh
		{
			std::shared_ptr<GpuBuffer> vb, ib;
			uint32_t indexCount = 0;
			scene::Material material;
			const GpuImage* texture = nullptr; // owned by the renderer's texture cache
			const GpuImage* decal = nullptr;
			const GpuImage* leader[4]{}; // the leaders' shader's maps: environment, normals, specular, environment mask
			int node = -1;
			bool skinned = false;
			std::vector<int> bones;
			std::vector<Affine> skinToBone;
			Vec3 localCenter; // rigid: in node space
			Vec3 bindCenter;  // skinned: model space, bind pose
		};

		std::vector<Mesh> m_meshes;
		std::vector<const GpuImage*> m_particleTextures; // by scene::Model::particles
		RenderStats m_stats;
	};

	// The frame constants (uniform slot 0) as HLSL, for other passes' shaders; UNIFORMS is the
	// stage's space (space1 in a vertex shader, space3 in a pixel shader).
	inline constexpr char kFrameConstantsHlsl[] = R"hlsl(
cbuffer Frame : register(b0, UNIFORMS)
{
	float4x4 viewProj;
	float3 lightDir;     float pad0;
	float3 lightColor;   float pad1;
	float3 ambientLight; float pad2;
	float3 eyePos;       float pad3;
	float4 groundGrid;   // terrain heights: 1 / cell size, columns, rows, wrap width (0: none)
	float4 fogGrid;      // the fog of war: plot size (0: none), columns, rows, wrap (1 X, 2 Y)
	float4 globe;        // the globe view: the point it is centred on (xy), its radius (z, 0: flat), the map's wrap width (w)
};

// The globe view: the map wrapped round a sphere centred under the point looked at, its width
// once round (heights above the sphere's surface).
float3 globeBend(float3 p)
{
	if (globe.z <= 0)
		return p;
	float2 d = p.xy - globe.xy;
	if (globe.w > 0)
		d.x -= globe.w * round(d.x / globe.w);
	const float len = length(d);
	const float a = min(len / globe.z, 3.14159);
	const float2 dir = len > 1e-4 ? d / len : float2(0, 0);
	const float h = globe.z + p.z;
	return float3(globe.xy + dir * h * sin(a), h * cos(a) - globe.z);
}
)hlsl";

	inline constexpr Vec3 kDefaultTeamColor{ 0.75f, 0.12f, 0.10f };

	// The overlay's triangles: in pixels, their colour premultiplied (RGBA8, red in the low byte).
	struct OverlayVertex
	{
		float x = 0, y = 0, u = 0, v = 0;
		uint32_t color = 0;
	};
	struct OverlayDraw
	{
		const GpuImage* image = nullptr; // (none: the colour alone)
		uint32_t first = 0, count = 0;
		int32_t clip[4]{}; // x0, y0, x1, y1
	};

	// A picture of its own the renderer draws models into (a leader's head on the screens), and
	// the overlay's picture of it.
	struct OffscreenTarget
	{
		std::shared_ptr<GpuImage> color, depth;
		uint32_t width = 0, height = 0;
	};

	// The light of a view: the direction it shines in, its colour, the ambient light.
	struct ViewLight
	{
		Vec3 direction{ -0.4f, -0.6f, -0.7f };
		Vec3 color{ 0.85f, 0.82f, 0.75f };
		Vec3 ambient{ 0.45f, 0.47f, 0.52f };
	};

	class Renderer
	{
	public:
		// window may be null for offscreen rendering.
		Renderer(SDL_Window* window, uint32_t width, uint32_t height);
		~Renderer();
		Renderer(const Renderer&) = delete;
		Renderer& operator=(const Renderer&) = delete;

		void resize(uint32_t width, uint32_t height);
		uint32_t width() const { return m_width; }
		uint32_t height() const { return m_height; }

		// decalOverride replaces the model's decal maps (runtime civilization flags); its owner
		// must keep the image alive while the uploaded model is used.
		std::unique_ptr<GpuModel> upload(const scene::Model& model, const AssetResolver& assets, const AssetLocation& modelLocation,
			const GpuImage* decalOverride = nullptr);

		void beginFrame(const Camera& camera, Vec3 clearColor = { 0.18f, 0.22f, 0.27f });
		// world / visible: one entry per model node (scene::Model::worldTransforms / visibility).
		// materials: one per model draw item (animated), or empty for the static ones.
		// onGround: the model's heights are over the terrain set with setGround (roads).
		void draw(const GpuModel& model, std::span<const Affine> world, std::span<const uint8_t> visible,
			std::span<const scene::Material> materials = {}, Vec3 teamColor = kDefaultTeamColor, bool onGround = false);
		// Particles: camera-facing quads (six vertices each, in the world, their colour in the
		// vertices') of a model's particle system in its material; drawn with the blended items.
		void drawParticles(const GpuModel& model, size_t system, const scene::Material& material, std::vector<scene::Vertex>&& vertices);
		// Draws the blended items of this frame's instances, back to front, then the particles.
		void endFrame();
		// Draws models into a target of its own (made width x height), as a camera of its own
		// sees them: viewProj is its view * projection (DirectXMath's row vectors, p * M), at eye;
		// no ground, no fog. endView draws its blended items and goes back to the frame's own
		// target and camera.
		// (clearAlpha 0: on nothing, what is under the picture shows through; alphaTwice: its
		// blended items cover it as the original's widgets' pictures do, their alpha counted twice,
		// colour times alpha cubed over what is under it times 1 - alpha squared: a flag's fringe)
		void beginView(OffscreenTarget& target, uint32_t width, uint32_t height, std::span<const float, 16> viewProj, Vec3 eye, const ViewLight& light,
			Vec3 clearColor, float clearAlpha = 1.0f, bool alphaTwice = false);
		void endView();
		// In a view: what was drawn so far (its blended items too) becomes the backdrop of what
		// comes next, which nothing of it hides (a leader in front of the scene behind him).
		void viewLayer();
		const GpuImage* targetImage(OffscreenTarget& target) { return target.color.get(); }
		void present();

		static constexpr size_t kMaxBones = 128;

		// BGRA8, alpha forced to 255.
		std::vector<uint8_t> readback(uint32_t& width, uint32_t& height);

		// The terrain's heights (R32Float, sample (i, j) at (i, j) * cellSize) for models drawn on the ground.
		void setGround(const GpuImage* heights, float cellSize, uint32_t columns, uint32_t rows, float wrapWidth);
		// The fog of war over the models, as the terrain shows it: the plots (RGBA16, bit 2 of
		// alpha revealed, bit 3 visible) of plotSize; nullptr for none.
		void setFog(const GpuImage* plots, float plotSize, uint32_t columns, uint32_t rows, bool wrapX, bool wrapY);

		// Textures: subresources layer by layer, each layer's mips from the largest, rows tightly packed.
		std::shared_ptr<GpuImage> createTexture(uint32_t width, uint32_t height, uint32_t layers, uint32_t mips, TextureFormat format,
			std::span<const std::span<const uint8_t>> subresources);
		// A BGRA picture made here (a flag's composed cloth) with its mips, each the mean of four
		// texels of the one above, as a DDS file has them.
		std::shared_ptr<GpuImage> createMipmapped(uint32_t width, uint32_t height, std::span<const uint8_t> bgra);
		void updateTexture(GpuImage& image, uint32_t layer, uint32_t mip, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const void* pixels);
		std::shared_ptr<GpuBuffer> createBuffer(SDL_GPUBufferUsageFlags usage, const void* data, uint32_t size);

		// Pictures for the overlay: BGRA8 with premultiplied alpha, or a byte of coverage a pixel (the glyphs').
		std::shared_ptr<GpuImage> createImage(uint32_t width, uint32_t height, bool coverage, const uint8_t* pixels);
		void updateImage(GpuImage& image, uint32_t x, uint32_t y, uint32_t width, uint32_t height, const uint8_t* pixels)
		{
			updateTexture(image, 0, 0, x, y, width, height, pixels);
		}
		// Over the frame's colour, after endFrame.
		void drawOverlay(std::span<const OverlayVertex> vertices, std::span<const OverlayDraw> draws);

		// For other passes: the render pass on the current target (begun when needed), its formats.
		SDL_GPUDevice* device() const { return m_device; }
		SDL_GPUCommandBuffer* commands();
		SDL_GPURenderPass* pass();
		// Before a draw's sampler bindings. (SDL's D3D12 backend switches a full descriptor heap without
		// rebinding the tables already set: the pass goes on in a new command buffer instead; true if it did.)
		bool reserveSamplers(uint32_t count);
		void bindPipeline(SDL_GPUGraphicsPipeline* p)
		{
			if (p != m_bound)
				SDL_BindGPUGraphicsPipeline(pass(), m_bound = p);
		}
		SDL_GPUTextureFormat colorFormat() const { return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM; }
		SDL_GPUTextureFormat depthFormat() const { return m_depthFormat; }
		// HLSL compiled for a stage (UNIFORMS defined as its space); throws with the compiler's message.
		SDL_GPUShader* compile(std::string_view source, const char* name, const char* entry, SDL_GPUShaderStage stage, uint32_t samplers,
			uint32_t storageBuffers, uint32_t uniformBuffers) const;
		const Camera& camera() const { return m_camera; }
		// The globe view: the map bent round a sphere centred under (x, y) of this radius (0: flat),
		// wrapping every wrapWidth (0: not).
		void setGlobe(float x, float y, float radius, float wrapWidth) { m_globe[0] = x, m_globe[1] = y, m_globe[2] = radius, m_globe[3] = wrapWidth; }
		bool globe() const { return m_globe[2] > 0; }
		// Row-major view-projection of the current frame (p_clip = M * p).
		const float* viewProjection() const { return m_viewProj; }
		// Cached by asset key; null when missing or unsupported (recorded in stats).
		const GpuImage* texture(const AssetResolver& assets, const AssetLocation& location, RenderStats* stats = nullptr);
		const GpuImage* texture(const AssetResolver& assets, const AssetLocation& model, const std::string& name, RenderStats* stats);

	private:
		struct Target
		{
			SDL_GPUTexture* color = nullptr;
			SDL_GPUTexture* depth = nullptr;
			uint32_t width = 0, height = 0;
		};
		struct PipelineKey
		{
			int kind = 0; // 0 a model's, 1 the overlay's
			bool blend = false;
			int src = 0, dst = 0;
			bool zTest = true, zWrite = true;
			int cull = 0;
			auto operator<=>(const PipelineKey&) const = default;
		};

		void createTargets();
		void createPipelines();
		SDL_GPUGraphicsPipeline* pipeline(const PipelineKey& key);
		SDL_GPUGraphicsPipeline* modelPipeline(const scene::Material& m, bool particles = false);
		void endPass();
		void beginTarget(const Target& target, const float* clearColor, bool clearDepth);
		void pushFrameConstants();
		void uploadFrameConstants();
		void upload(SDL_GPUTexture* texture, uint32_t layer, uint32_t mip, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const void* data,
			uint32_t bytes);
		void upload(SDL_GPUBuffer* buffer, uint32_t offset, const void* data, uint32_t bytes);
		void drawMesh(const GpuModel::Mesh& mesh, const scene::Material& material, std::span<const Affine> world, Vec3 teamColor, bool onGround);
		void bindModelTextures(const GpuImage* base, const GpuImage* decal, const scene::Material& m, const GpuImage* const* leader);
		SDL_GPUSampler* sampler(int clamp) const { return m_samplers[clamp]; }

		SDL_Window* m_window = nullptr;
		SDL_GPUDevice* m_device = nullptr;
		uint32_t m_width = 0, m_height = 0;
		SDL_GPUTextureFormat m_depthFormat = SDL_GPU_TEXTUREFORMAT_D24_UNORM_S8_UINT;
		std::shared_ptr<GpuImage> m_color, m_depth;

		SDL_GPUCommandBuffer* m_cmd = nullptr;
		uint32_t m_samplersUsed = 0; // (in m_cmd)
		SDL_GPURenderPass* m_pass = nullptr;
		Target m_target;
		bool m_clearColor = false, m_clearDepth = false;
		float m_clear[4]{};
		SDL_GPUGraphicsPipeline* m_bound = nullptr; // (the pass's)

		SDL_GPUShader* m_vs = nullptr;
		SDL_GPUShader* m_ps = nullptr;
		SDL_GPUShader* m_overlayVs = nullptr;
		SDL_GPUShader* m_overlayPs = nullptr;
		std::map<PipelineKey, SDL_GPUGraphicsPipeline*> m_pipelines;
		SDL_GPUSampler* m_samplers[4]{}; // by scene::TexClamp
		SDL_GPUSampler* m_linearClamp = nullptr;
		std::shared_ptr<GpuImage> m_white, m_noHeights, m_noPlots;
		const GpuImage* m_ground = nullptr;
		float m_groundCell = 1, m_groundWrap = 0;
		uint32_t m_groundColumns = 0, m_groundRows = 0;
		const GpuImage* m_fog = nullptr;
		float m_fogPlot = 0;
		uint32_t m_fogColumns = 0, m_fogRows = 0, m_fogWrap = 0;
		std::map<std::string, std::shared_ptr<GpuImage>> m_textures; // by asset key

		Camera m_camera;
		float m_frame[44]{}; // the frame constants pushed with each pass
		float m_viewProj[16]{};
		float m_globe[4]{};
		bool m_inView = false;        // between beginView and endView
		bool m_viewOnNothing = false; // ... a view on nothing: what is not blended covers it (alpha 1)
		bool m_viewAlphaTwice = false; // ... its blended items' alpha counted twice (beginView)
		Vec3 m_viewEye;

		// Blended items of the current frame, drawn by endFrame.
		struct Deferred
		{
			float distance;
			const GpuModel::Mesh* mesh;
			size_t world;    // into m_deferredWorlds
			size_t material; // into m_deferredMaterials
			Vec3 teamColor;
			bool onGround;
		};
		std::vector<Deferred> m_deferred;
		struct ParticleDraw
		{
			const GpuImage* texture = nullptr;
			scene::Material material;
			std::vector<scene::Vertex> vertices;
		};
		std::vector<ParticleDraw> m_particleDraws;
		std::shared_ptr<GpuBuffer> m_particleVb, m_overlayVb;
		std::vector<std::vector<Affine>> m_deferredWorlds;
		std::vector<scene::Material> m_deferredMaterials;
	};
}
