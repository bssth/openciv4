#pragma once

// Turns a NIF scene graph into a runtime model: a flat node hierarchy with
// local transforms (so animations can drive it), and draw items whose vertices
// stay in their own space. Rigid items follow one node; skinned items carry up
// to four bone influences per vertex. Properties inherited down the graph are
// resolved into one material per item. Renderer-agnostic.

#include "core/Math.h"
#include "nif/NifBlocks.h"
#include "nif/NifFile.h"

#include <cstdint>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace oc4::scene
{
	struct Vertex
	{
		Vec3 position;
		Vec3 normal;
		Vec2 uv;
		uint32_t color = 0xFFFFFFFF; // RGBA8, R in the low byte
		uint8_t bones[4]{};          // indices into DrawItem::bones
		uint8_t weights[4]{};        // UNORM, sum 255 for skinned vertices
	};

	// Gamebryo blend factors (NiAlphaProperty::AlphaFunction).
	enum class Blend : uint8_t
	{
		One, Zero, SrcColor, InvSrcColor, DestColor, InvDestColor, SrcAlpha, InvSrcAlpha, DestAlpha, InvDestAlpha, SrcAlphaSat
	};

	// Gamebryo test functions (NiAlphaProperty::TestFunction).
	enum class Compare : uint8_t
	{
		Always, Less, Equal, LessEqual, Greater, NotEqual, GreaterEqual, Never
	};

	enum class Cull : uint8_t
	{
		Back, Front, None
	};

	// NiTexturingProperty map transform. Gamebryo's 3ds Max method:
	// uv' = (uv - translation - center) * tiling + center (rotation about center).
	struct UvTransform
	{
		Vec2 translation;
		Vec2 tiling{ 1, 1 };
		float rotation = 0;
		Vec2 center{ 0.5f, 0.5f };
		uint32_t method = 1;   // 0 Maya (deprecated), 1 Max, 2 Maya
		bool enabled = false;

		Vec2 apply(Vec2 uv) const;
	};

	// TexDesc clamp modes.
	enum class TexClamp : uint8_t
	{
		ClampClamp = 0, ClampWrap = 1, WrapClamp = 2, WrapWrap = 3
	};

	struct Material
	{
		nif::Color3 ambient{ 1, 1, 1 };
		nif::Color3 diffuse{ 1, 1, 1 };
		nif::Color3 emissive{ 0, 0, 0 };
		float alpha = 1;
		// Set by animation (NiAlphaController): the item fades even without an alpha property.
		bool alphaAnimated = false;

		std::string baseTexture; // as referenced by the model; empty = untextured
		size_t baseSlot = 0;     // TexSlot the base texture came from (mech units: detail)
		UvTransform baseUv;
		TexClamp baseClamp = TexClamp::WrapWrap;
		// Civ4 units put a "TeamColor" placeholder in the base slot and the real
		// texture in decal 0; the decal's alpha reveals the player colour. Mech
		// units put a damage atlas there instead, windowed by animated UVs.
		bool baseIsTeamColor = false;
		std::string decalTexture;
		UvTransform decalUv;
		TexClamp decalClamp = TexClamp::WrapWrap;
		// The leaders' shader's maps (TLeaderheadShader*: the texturing property's shader maps,
		// Civ4LeaderheadShader.fx's order): the sphere map reflected, the tangent space normals,
		// the specular intensity (its red: the shininess too), the reflection's mask.
		std::string envMap, normalMap, specularMap, envMask;

		bool alphaBlend = false;
		Blend srcBlend = Blend::SrcAlpha;
		Blend dstBlend = Blend::InvSrcAlpha;
		bool alphaTest = false;
		Compare alphaFunc = Compare::Greater;
		uint8_t alphaRef = 0;

		bool zTest = true;
		bool zWrite = true;
		Cull cull = Cull::Back;

		// NiVertexColorProperty: 0 ignore, 1 emissive, 2 ambient+diffuse
		uint32_t vertexColorMode = 0;
		// 0: emissive only (unlit), 1: emissive + ambient + diffuse
		uint32_t lightingMode = 1;

		std::string shaderName;

		// A forest piece's trees not drawn (DrawItem::trees): tree i by bit i - 1.
		uint32_t treesCut = 0;
	};

	struct Node
	{
		std::string name;
		int parent = -1; // parents always precede children
		Affine local;    // bind (file) transform
		bool hidden = false; // APP_CULLED flag in the file; animations may toggle it
		nif::Ref block = nif::kNoRef;
	};

	struct DrawItem
	{
		std::string nodeName;
		int node = -1; // rigid items: placed by this node's world transform
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		Material material;
		// Nodes the material / texturing properties are attached to; animation
		// channels address properties through these nodes.
		int materialOwner = -1;
		int texturingOwner = -1;

		// Skinned items: world = sum(weight * world(bones[i]) * skinToBone[i]) * v
		bool skinned = false;
		std::vector<int> bones;
		std::vector<Affine> skinToBone;

		Bounds bindBounds; // world space, bind pose

		// A forest piece: where its trees stand (its node's space), tree i (the red of its
		// vertices' colour) at [i - 1]; not a number where there is none.
		std::vector<nif::Vec2> trees;
	};

	// A particle system (NiParticleSystem) as its emitter, modifiers and controllers have it:
	// what it takes to run it (render::Particles) and draw it (quads facing the camera, in its
	// material). Times are seconds, angles radians, sizes the model's units.
	struct ParticleSystem
	{
		std::string nodeName;
		int node = -1;           // its own node (where it is placed; its particles' frame unless worldSpace)
		Material material;
		bool worldSpace = true;  // the particles stay where they were born (else they go with the node)
		size_t maxParticles = 64;
		// The emitter: its shape, centred on its emitter object's node (or the system's).
		enum class Shape : uint8_t
		{
			Point, Box, Sphere, Cylinder
		};
		Shape shape = Shape::Point;
		int emitterNode = -1;
		Vec3 size;               // box: width, height, depth; sphere: radius (x); cylinder: radius (x), height (y)
		float speed = 0, speedVariation = 0;
		float declination = 0, declinationVariation = 0; // from the emitter's z axis
		float planarAngle = 0, planarAngleVariation = 0; // round it
		nif::Color4 color{ 1, 1, 1, 1 };
		float radius = 1, radiusVariation = 0;
		float lifeSpan = 1, lifeSpanVariation = 0;
		// Its emitter controller: particles born a second (keys over its time, else the value),
		// whether it emits (keys, else on); its time runs start..stop, looping or not.
		float birthRate = 0;
		nif::KeyGroup<float> birthRateKeys;
		nif::KeyGroup<uint8_t> activeKeys;
		float start = 0, stop = 0, frequency = 1;
		bool loop = true;
		// Modifiers: growing in and fading out (seconds of a particle's age), its colour over
		// its life (key times: parts of it), its spin, the pulls on it.
		float growTime = 0, fadeTime = 0;
		nif::KeyGroup<nif::Color4> colorKeys;
		bool spins = false;
		float spinSpeed = 0, spinSpeedVariation = 0, spinAngle = 0, spinAngleVariation = 0;
		bool spinRandomSign = false;
		struct Gravity
		{
			int node = -1;   // the gravity object (its axis is in its frame; a point force pulls to it)
			Vec3 axis{ 0, 0, 1 };
			float decay = 0, strength = 0;
			bool point = false;
		};
		std::vector<Gravity> gravity;
	};

	struct Model
	{
		std::vector<Node> nodes;
		std::unordered_map<std::string, std::vector<int>> nodesByName; // duplicates in graph order
		std::vector<DrawItem> items;
		Bounds bounds; // bind pose
		size_t hiddenNodes = 0;
		size_t particleSystems = 0;
		std::vector<ParticleSystem> particles;
		std::vector<std::string> warnings;

		std::vector<Affine> bindLocals() const;
		std::vector<uint8_t> bindHidden() const;
		// World transforms for the given local transforms (one per node).
		std::vector<Affine> worldTransforms(std::span<const Affine> locals) const;
		// A node is visible when neither it nor any ancestor is hidden.
		std::vector<uint8_t> visibility(std::span<const uint8_t> hidden) const;
		// Accepts Gamebryo's "name@#N" form for the N-th (0-based) node sharing a name.
		int findNode(std::string_view name) const;
	};

	Model buildModel(const nif::File& file);
}
