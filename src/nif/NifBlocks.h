#pragma once

// Block definitions for Gamebryo NIF 20.0.0.4 as used by Civilization IV.
// Field layouts follow NifTools' nif.xml; only fields present in Civ4-era
// versions (10.1 .. 20.0.0.5, user version 0) are handled.

#include "nif/NifFile.h"
#include "nif/NifReader.h"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace oc4::nif
{
	std::unique_ptr<NiObject> parseBlock(NifReader& r, const std::string& type);

	// ------------------------------------------------------------------ keys

	enum class KeyType : uint32_t
	{
		None = 0,
		Linear = 1,
		Quadratic = 2,
		TBC = 3,
		XyzRotation = 4,
		Const = 5,
	};

	template <class T>
	struct Key
	{
		float time = 0;
		T value{};
		// Quadratic keys store the incoming tangent first, then the outgoing one
		// (nifxml calls them Forward/Backward, which reads backwards).
		T inTangent{};
		T outTangent{};
		std::array<float, 3> tbc{};
	};

	template <class T>
	struct KeyGroup
	{
		KeyType interpolation = KeyType::None;
		std::vector<Key<T>> keys;
	};

	// ------------------------------------------------------------------ scene graph

	struct NiObjectNET : NiObject
	{
		std::string name;
		std::vector<Ref> extraData;
		Ref controller = kNoRef;
	};

	struct NiAVObject : NiObjectNET
	{
		uint16_t flags = 0;
		Vec3 translation;
		Mat33 rotation;
		float scale = 1;
		std::vector<Ref> properties;
		Ref collision = kNoRef;

		bool hidden() const { return (flags & 1) != 0; }
	};

	struct NiNode : NiAVObject
	{
		std::vector<Ref> children;
		std::vector<Ref> effects;
	};

	struct NiBillboardNode : NiNode
	{
		uint16_t billboardMode = 0;
	};

	struct NiGeometry : NiAVObject
	{
		Ref data = kNoRef;
		Ref skinInstance = kNoRef;
		bool hasShader = false;
		std::string shaderName;
		int32_t shaderExtraData = 0;
	};

	struct NiTriShape : NiGeometry
	{
	};

	struct NiTriStrips : NiGeometry
	{
	};

	struct NiGeometryData : NiObject
	{
		int32_t groupId = 0;
		uint16_t numVertices = 0;
		uint8_t keepFlags = 0;
		uint8_t compressFlags = 0;
		std::vector<Vec3> vertices;
		uint16_t dataFlags = 0;
		std::vector<Vec3> normals;
		std::vector<Vec3> tangents;
		std::vector<Vec3> bitangents;
		Vec3 center;
		float radius = 0;
		std::vector<Color4> colors;
		std::vector<std::vector<Vec2>> uvSets;
		uint16_t consistencyFlags = 0;
		Ref additionalData = kNoRef;
	};

	struct NiTriBasedGeomData : NiGeometryData
	{
		uint16_t numTriangles = 0;

		// Triangle list regardless of storage (lists or strips); degenerate strip
		// triangles are dropped.
		virtual std::vector<Triangle> triangleList() const = 0;
	};

	struct NiTriShapeData : NiTriBasedGeomData
	{
		std::vector<Triangle> triangles;

		std::vector<Triangle> triangleList() const override { return triangles; }
	};

	struct NiTriStripsData : NiTriBasedGeomData
	{
		std::vector<std::vector<uint16_t>> strips;

		std::vector<Triangle> triangleList() const override;
	};

	// ------------------------------------------------------------------ properties

	struct NiProperty : NiObjectNET
	{
	};

	struct NiMaterialProperty : NiProperty
	{
		Color3 ambient{ 1, 1, 1 };
		Color3 diffuse{ 1, 1, 1 };
		Color3 specular;
		Color3 emissive;
		float glossiness = 10;
		float alpha = 1;
	};

	struct NiAlphaProperty : NiProperty
	{
		uint16_t flags = 0;
		uint8_t threshold = 0;

		bool blendEnabled() const { return (flags & 0x0001) != 0; }
		uint32_t srcBlend() const { return (flags >> 1) & 0xF; }
		uint32_t dstBlend() const { return (flags >> 5) & 0xF; }
		bool testEnabled() const { return (flags & 0x0200) != 0; }
		uint32_t testFunc() const { return (flags >> 10) & 0x7; }
		bool noSorter() const { return (flags & 0x2000) != 0; }
	};

	struct NiVertexColorProperty : NiProperty
	{
		uint16_t flags = 0;
		uint32_t vertexMode = 0;   // 0 ignore, 1 emissive, 2 ambient+diffuse
		uint32_t lightingMode = 1; // 0 emissive only, 1 emissive+ambient+diffuse
	};

	struct NiZBufferProperty : NiProperty
	{
		uint16_t flags = 3; // bit0 test, bit1 write
		uint32_t function = 3;

		bool testEnabled() const { return (flags & 1) != 0; }
		bool writeEnabled() const { return (flags & 2) != 0; }
	};

	struct NiStencilProperty : NiProperty
	{
		bool enabled = false;
		uint32_t function = 0, ref = 0, mask = 0xFFFFFFFF, failAction = 0, zFailAction = 0, passAction = 0;
		uint32_t drawMode = 0; // 0 ccw/default, 1 ccw, 2 cw, 3 both
	};

	// NiSpecularProperty, NiWireframeProperty, NiDitherProperty, NiShadeProperty.
	struct NiFlagProperty : NiProperty
	{
		uint16_t flags = 0;
	};

	struct TexDesc
	{
		Ref source = kNoRef;
		uint32_t clampMode = 3; // WRAP_S_WRAP_T
		uint32_t filterMode = 2;
		uint32_t uvSet = 0;
		bool hasTransform = false;
		Vec2 translation;
		Vec2 tiling{ 1, 1 };
		float rotation = 0;
		uint32_t transformMethod = 0;
		Vec2 center;
	};

	enum TexSlot : size_t
	{
		kTexBase,
		kTexDark,
		kTexDetail,
		kTexGloss,
		kTexGlow,
		kTexBump,
		kTexDecal0,
		kTexDecal1,
		kTexDecal2,
		kTexDecal3,
		kTexSlotCount,
	};

	struct NiTexturingProperty : NiProperty
	{
		uint16_t flags = 0;
		uint32_t applyMode = 2; // APPLY_MODULATE
		uint32_t textureCount = 0;
		std::array<std::optional<TexDesc>, kTexSlotCount> maps;
		float bumpLumaScale = 0, bumpLumaOffset = 0;
		std::array<float, 4> bumpMatrix{};
		struct ShaderMap
		{
			std::optional<TexDesc> map;
			uint32_t mapId = 0;
		};
		std::vector<ShaderMap> shaderMaps;
	};

	struct NiSourceTexture : NiObjectNET
	{
		bool external = true;
		std::string fileName;
		Ref pixelData = kNoRef;
		uint32_t pixelLayout = 0, useMipmaps = 0, alphaFormat = 0;
		uint8_t isStatic = 1;
		bool directRender = false;
	};

	struct NiPixelData : NiObject
	{
		uint32_t pixelFormat = 0;
		std::array<uint32_t, 4> masks{};
		uint32_t bitsPerPixel = 0;
		uint32_t bytesPerPixel = 0;
		struct MipMap
		{
			uint32_t width = 0, height = 0, offset = 0;
		};
		std::vector<MipMap> mipmaps;
		std::vector<uint8_t> pixels;
	};

	// ------------------------------------------------------------------ skinning

	struct NiSkinInstance : NiObject
	{
		Ref data = kNoRef;
		Ref skinPartition = kNoRef;
		Ref skeletonRoot = kNoRef;
		std::vector<Ref> bones;
	};

	struct NiSkinData : NiObject
	{
		struct BoneWeight
		{
			uint16_t vertex = 0;
			float weight = 0;
		};
		struct Bone
		{
			Transform skinTransform;
			Vec3 boundCenter;
			float boundRadius = 0;
			std::vector<BoneWeight> weights;
		};
		Transform skinTransform;
		std::vector<Bone> bones;
	};

	struct NiSkinPartition : NiObject
	{
		struct Partition
		{
			uint16_t numVertices = 0;
			uint16_t numWeightsPerVertex = 0;
			std::vector<uint16_t> bones;
			std::vector<uint16_t> vertexMap;
			std::vector<float> vertexWeights; // numVertices * numWeightsPerVertex
			std::vector<std::vector<uint16_t>> strips;
			std::vector<Triangle> triangles;
			std::vector<uint8_t> boneIndices; // numVertices * numWeightsPerVertex
		};
		std::vector<Partition> partitions;
	};

	// ------------------------------------------------------------------ extra data

	struct NiExtraData : NiObject
	{
		std::string name;
		Ref next = kNoRef; // (before 10.0.1.0: the next one of the object's)
	};

	struct NiStringExtraData : NiExtraData
	{
		std::string value;
	};

	struct NiIntegerExtraData : NiExtraData
	{
		uint32_t value = 0;
	};

	struct NiFloatExtraData : NiExtraData
	{
		float value = 0;
	};

	struct NiFloatsExtraData : NiExtraData
	{
		std::vector<float> values;
	};

	struct NiTextKeyExtraData : NiExtraData
	{
		std::vector<std::pair<float, std::string>> keys;
	};

	struct NiStringPalette : NiObject
	{
		std::string palette; // NUL-separated strings, addressed by byte offset

		std::string_view at(uint32_t offset) const;
	};

	// ------------------------------------------------------------------ animation data

	struct NiTransformData : NiObject // also NiKeyframeData
	{
		KeyType rotationType = KeyType::None;
		std::vector<Key<Quat>> quaternionKeys;
		std::array<KeyGroup<float>, 3> xyzRotations;
		KeyGroup<Vec3> translations;
		KeyGroup<float> scales;
	};

	struct NiFloatData : NiObject
	{
		KeyGroup<float> data;
	};

	struct NiPosData : NiObject
	{
		KeyGroup<Vec3> data;
	};

	struct NiBoolData : NiObject // also NiVisData
	{
		KeyGroup<uint8_t> data;
	};

	struct NiColorData : NiObject
	{
		KeyGroup<Color4> data;
	};

	struct NiUVData : NiObject
	{
		std::array<KeyGroup<float>, 4> groups; // u/v translation, u/v scale
	};

	struct NiBSplineData : NiObject
	{
		std::vector<float> floatControlPoints;
		std::vector<int16_t> compactControlPoints;
	};

	struct NiBSplineBasisData : NiObject
	{
		uint32_t numControlPoints = 0;
	};

	// ------------------------------------------------------------------ interpolators

	struct NiInterpolator : NiObject
	{
	};

	struct NiTransformInterpolator : NiInterpolator
	{
		Vec3 translation;
		Quat rotation;
		float scale = 1;
		Ref data = kNoRef;
	};

	struct NiFloatInterpolator : NiInterpolator
	{
		float value = 0;
		Ref data = kNoRef;
	};

	struct NiPoint3Interpolator : NiInterpolator
	{
		Vec3 value;
		Ref data = kNoRef;
	};

	struct NiBoolInterpolator : NiInterpolator // also NiBoolTimelineInterpolator
	{
		bool value = false;
		Ref data = kNoRef;
	};

	struct NiLookAtInterpolator : NiInterpolator
	{
		uint16_t flags = 0;
		Ref lookAt = kNoRef;
		std::string lookAtName;
		Vec3 translation;
		Quat rotation;
		float scale = 1;
		Ref translationInterp = kNoRef, rollInterp = kNoRef, scaleInterp = kNoRef;
	};

	struct NiPathInterpolator : NiInterpolator
	{
		uint16_t flags = 0;
		int32_t bankDir = 0;
		float maxBankAngle = 0, smoothing = 0;
		int16_t followAxis = 0;
		Ref pathData = kNoRef, percentData = kNoRef;
	};

	struct NiBSplineInterpolator : NiInterpolator
	{
		float startTime = 0, stopTime = 0;
		Ref splineData = kNoRef, basisData = kNoRef;
	};

	struct NiBSplineCompTransformInterpolator : NiBSplineInterpolator
	{
		Vec3 translation;
		Quat rotation;
		float scale = 1;
		uint32_t translationHandle = 0xFFFF, rotationHandle = 0xFFFF, scaleHandle = 0xFFFF;
		float translationBias = 0, translationMultiplier = 0;
		float rotationBias = 0, rotationMultiplier = 0;
		float scaleBias = 0, scaleMultiplier = 0;
	};

	struct NiBSplineCompFloatInterpolator : NiBSplineInterpolator
	{
		float value = 0;
		uint32_t handle = 0xFFFF;
		float offset = 0, halfRange = 0;
	};

	struct NiBSplineCompPoint3Interpolator : NiBSplineInterpolator
	{
		Vec3 value;
		uint32_t handle = 0xFFFF;
		float offset = 0, halfRange = 0;
	};

	// ------------------------------------------------------------------ controllers

	struct NiTimeController : NiObject
	{
		Ref nextController = kNoRef;
		uint16_t flags = 0;
		float frequency = 1, phase = 0, startTime = 0, stopTime = 0;
		Ref target = kNoRef;
	};

	// Covers NiSingleInterpController subclasses (transform, alpha, vis, material
	// color, texture transform, light, extra data controllers).
	struct NiSingleInterpController : NiTimeController
	{
		Ref interpolator = kNoRef;
		Ref data = kNoRef; // pre-10.1.0.104 files
		uint16_t targetColor = 0; // material/light color controllers
		bool shaderMap = false;   // texture transform controller
		uint32_t textureSlot = 0;
		uint32_t operation = 0;
		std::string extraDataName; // float extra data controller
	};

	struct NiMultiTargetTransformController : NiTimeController
	{
		std::vector<Ref> extraTargets;
	};

	// Animation sequences kept inside a model (some mods' units); the game plays the KFM's.
	struct NiControllerManager : NiTimeController
	{
		bool cumulative = false;
		std::vector<Ref> sequences;
		Ref objectPalette = kNoRef;
	};

	// Levels of detail (some mods' buildings and animals): read so that the rest of the file is,
	// the nearest level drawn.
	struct NiLODData : NiObject
	{
	};

	struct NiBoneLODController : NiTimeController
	{
	};

	// Blends of a sequence manager's interpolators (some mods' units): their value unused.
	struct NiBlendInterpolator : NiObject
	{
	};

	struct NiBinaryExtraData : NiExtraData
	{
		std::vector<uint8_t> data;
	};

	// The old particle systems (NiParticles and its kin, before NiParticleSystem): read so that
	// the rest of a model is; their particles are not drawn.
	struct NiOldParticles : NiGeometry
	{
	};

	struct NiOldParticlesData : NiGeometryData
	{
	};

	struct NiParticleSystemController : NiTimeController
	{
		Ref firstModifier = kNoRef;
	};

	struct NiParticleModifier : NiObject
	{
		Ref next = kNoRef;
	};

	struct NiDefaultAVObjectPalette : NiObject
	{
		Ref scene = kNoRef;
		std::vector<std::pair<std::string, Ref>> objects;
	};

	// Vertex morphing (leaderhead facial animation).
	struct NiGeomMorpherController : NiTimeController
	{
		uint16_t morpherFlags = 0;
		Ref data = kNoRef;
		uint8_t alwaysUpdate = 0;
		std::vector<Ref> interpolators; // one per morph target
	};

	struct NiMorphData : NiObject
	{
		struct Morph
		{
			std::string frameName;
			KeyGroup<float> keys; // pre-10.1.0.106 only
			float legacyWeight = 0;
			std::vector<Vec3> vectors;
		};
		uint8_t relativeTargets = 1;
		std::vector<Morph> morphs;
	};

	struct NiControllerSequence : NiObject
	{
		struct ControlledBlock
		{
			std::string targetName; // pre-10.1.0.106
			Ref interpolator = kNoRef;
			Ref controller = kNoRef;
			Ref stringPalette = kNoRef;
			uint32_t nodeNameOffset = 0xFFFFFFFF;
			uint32_t propertyTypeOffset = 0xFFFFFFFF;
			uint32_t controllerTypeOffset = 0xFFFFFFFF;
			uint32_t controllerIdOffset = 0xFFFFFFFF;
			uint32_t interpolatorIdOffset = 0xFFFFFFFF;
		};

		std::string name;
		std::vector<ControlledBlock> controlledBlocks;
		float weight = 1;
		Ref textKeys = kNoRef;
		uint32_t cycleType = 0;
		float frequency = 1, startTime = 0, stopTime = 0;
		Ref manager = kNoRef;
		std::string accumRootName;
		Ref stringPalette = kNoRef;
	};

	// ------------------------------------------------------------------ effects, lights, cameras

	struct NiDynamicEffect : NiAVObject
	{
		bool switchState = true;
		std::vector<Ref> affectedNodes;
	};

	struct NiLight : NiDynamicEffect
	{
		float dimmer = 1;
		Color3 ambient, diffuse, specular;
		float constantAttenuation = 1, linearAttenuation = 0, quadraticAttenuation = 0;
		float outerSpotAngle = 0, spotExponent = 0;
	};

	struct NiTextureEffect : NiDynamicEffect
	{
		Mat33 projectionMatrix;
		Vec3 projectionTranslation;
		uint32_t filtering = 0, clamping = 0, textureType = 0, coordGenType = 0;
		Ref sourceTexture = kNoRef;
		uint8_t enablePlane = 0;
		Vec3 planeNormal;
		float planeConstant = 0;
	};

	struct NiCamera : NiAVObject
	{
		float frustumLeft = 0, frustumRight = 0, frustumTop = 0, frustumBottom = 0, frustumNear = 0, frustumFar = 0;
		bool orthographic = false;
		Ref scene = kNoRef;
	};

	// ------------------------------------------------------------------ particles

	struct NiParticleSystem : NiGeometry
	{
		bool worldSpace = true;
		std::vector<Ref> modifiers;
	};

	struct NiPSysData : NiGeometryData
	{
		std::vector<float> radii;
		uint16_t numActive = 0;
		std::vector<float> sizes;
		std::vector<Quat> rotations;
		std::vector<float> rotationAngles;
		std::vector<Vec3> rotationAxes;
		std::vector<float> rotationSpeeds;
	};

	// Particle modifiers, emitters, colliders and their controllers, their fields kept as a
	// flat list (in the file's order; scene::buildModel reads them by their type). The
	// controllers also keep their timing and the next controller of their chain.
	struct NiPSysObject : NiObject
	{
		std::string name;
		std::vector<float> params;
		std::vector<Ref> refs;
		Ref nextController = kNoRef;
		uint16_t flags = 0;
		float frequency = 1, phase = 0, startTime = 0, stopTime = 0;
	};

	// Parsed for coverage, nothing retained (collision volumes etc.).
	struct NiOpaque : NiObject
	{
	};
}
