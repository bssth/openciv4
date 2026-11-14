#include "nif/NifBlocks.h"

#include <cstring>
#include <format>
#include <string_view>
#include <unordered_map>

namespace oc4::nif
{
	namespace
	{
		constexpr uint32_t V(uint32_t a, uint32_t b, uint32_t c, uint32_t d) { return makeVersion(a, b, c, d); }

		// ---------------------------------------------------------------- keys

		template <class T>
		T readValue(NifReader& r);
		template <>
		float readValue<float>(NifReader& r) { return r.f32(); }
		template <>
		Vec3 readValue<Vec3>(NifReader& r) { return r.vec3(); }
		template <>
		Color4 readValue<Color4>(NifReader& r) { return r.color4(); }
		template <>
		uint8_t readValue<uint8_t>(NifReader& r) { return r.u8(); }
		template <>
		Quat readValue<Quat>(NifReader& r) { return r.quat(); }

		KeyType readKeyType(NifReader& r)
		{
			const uint32_t t = r.u32();
			if (t < 1 || t > 5)
				r.fail(std::format("invalid key type {}", t));
			return static_cast<KeyType>(t);
		}

		template <class T>
		KeyGroup<T> readKeyGroup(NifReader& r)
		{
			KeyGroup<T> g;
			const uint32_t n = r.u32();
			if (n == 0)
				return g;
			g.interpolation = readKeyType(r);
			r.requireArray(n, sizeof(float) + sizeof(T));
			g.keys.resize(n);
			for (auto& k : g.keys)
			{
				k.time = r.f32();
				k.value = readValue<T>(r);
				if (g.interpolation == KeyType::Quadratic)
				{
					k.inTangent = readValue<T>(r);
					k.outTangent = readValue<T>(r);
				}
				else if (g.interpolation == KeyType::TBC)
					k.tbc = r.read<std::array<float, 3>>();
			}
			return g;
		}

		// ---------------------------------------------------------------- bases

		void readObjectNET(NifReader& r, NiObjectNET& o)
		{
			o.name = r.string();
			// (before 10.0.1.0 one extra data, the next one linked from it)
			if (r.since(V(10, 0, 1, 0)))
				o.extraData = r.refList();
			else if (const Ref first = r.ref(); first >= 0)
				o.extraData.push_back(first);
			o.controller = r.ref();
		}

		// A bounding volume (before 10.0.1.0, on any object): sphere or box.
		void skipBoundingVolume(NifReader& r)
		{
			switch (r.u32())
			{
			case 0: r.skip(16); break;              // sphere: center, radius
			case 1: r.skip(12 + 36 + 12); break;    // box: center, axes, extent
			case 2: r.skip(12 + 12 + 8); break;     // capsule: center, origin, extent, radius
			default: r.fail("bounding volume type not supported");
			}
		}

		void readAVObject(NifReader& r, NiAVObject& o)
		{
			readObjectNET(r, o);
			o.flags = r.u16();
			o.translation = r.vec3();
			o.rotation = r.mat33();
			o.scale = r.f32();
			if (r.until(V(4, 2, 2, 0)))
				(void)r.vec3(); // velocity
			o.properties = r.refList();
			if (r.until(V(4, 2, 2, 0)))
			{
				if (r.boolean())
					skipBoundingVolume(r);
			}
			else
				o.collision = r.ref();
		}

		void readNode(NifReader& r, NiNode& o)
		{
			readAVObject(r, o);
			o.children = r.refList();
			o.effects = r.refList();
		}

		void readGeometry(NifReader& r, NiGeometry& o)
		{
			readAVObject(r, o);
			o.data = r.ref();
			o.skinInstance = r.ref();
			if (r.since(V(10, 0, 1, 0)) && r.until(V(20, 1, 0, 3)))
			{
				o.hasShader = r.boolean();
				if (o.hasShader)
				{
					o.shaderName = r.string();
					o.shaderExtraData = r.i32();
				}
			}
		}

		void readGeometryData(NifReader& r, NiGeometryData& d)
		{
			if (r.since(V(10, 1, 0, 114)))
				d.groupId = r.i32();
			d.numVertices = r.u16();
			const size_t n = d.numVertices;
			if (r.since(V(10, 1, 0, 0)))
			{
				d.keepFlags = r.u8();
				d.compressFlags = r.u8();
			}
			if (r.boolean())
				d.vertices = r.vectorOf<Vec3>(n);
			// (before 10.0.1.0 the flags, the number of UV sets, come after the colours)
			const bool old = !r.since(V(10, 0, 1, 0));
			if (!old)
				d.dataFlags = r.u16();
			const bool hasNormals = r.boolean();
			if (hasNormals)
				d.normals = r.vectorOf<Vec3>(n);
			if (hasNormals && (d.dataFlags & 0x1000) && r.since(V(10, 1, 0, 0)))
			{
				d.tangents = r.vectorOf<Vec3>(n);
				d.bitangents = r.vectorOf<Vec3>(n);
			}
			d.center = r.vec3();
			d.radius = r.f32();
			if (r.boolean())
				d.colors = r.vectorOf<Color4>(n);
			if (old)
				d.dataFlags = r.u16();
			d.uvSets.resize(d.dataFlags & 63);
			for (auto& uv : d.uvSets)
				uv = r.vectorOf<Vec2>(n);
			if (!old)
				d.consistencyFlags = r.u16();
			if (r.since(V(20, 0, 0, 4)))
				d.additionalData = r.ref();
		}

		TexDesc readTexDesc(NifReader& r)
		{
			TexDesc t;
			t.source = r.ref();
			if (r.until(V(20, 0, 0, 5)))
			{
				t.clampMode = r.u32();
				t.filterMode = r.u32();
				t.uvSet = r.u32();
			}
			else
				r.fail("TexDesc flags layout (20.1+) not supported");
			if (r.until(V(10, 4, 0, 1)))
			{
				(void)r.i16(); // PS2 L
				(void)r.i16(); // PS2 K
			}
			if (r.since(V(10, 1, 0, 0)))
			{
				t.hasTransform = r.boolean();
				if (t.hasTransform)
				{
					t.translation = r.vec2();
					t.tiling = r.vec2();
					t.rotation = r.f32();
					t.transformMethod = r.u32();
					t.center = r.vec2();
				}
			}
			return t;
		}

		void readExtraData(NifReader& r, NiExtraData& e)
		{
			// (before 10.0.1.0 no name, and a link to the next one)
			if (r.since(V(10, 0, 1, 0)))
				e.name = r.string();
			else
				e.next = r.ref();
		}

		void readTimeController(NifReader& r, NiTimeController& c)
		{
			c.nextController = r.ref();
			c.flags = r.u16();
			c.frequency = r.f32();
			c.phase = r.f32();
			c.startTime = r.f32();
			c.stopTime = r.f32();
			c.target = r.ref();
		}

		void readSingleInterpController(NifReader& r, NiSingleInterpController& c)
		{
			readTimeController(r, c);
			if (r.since(V(10, 1, 0, 104)) && r.until(V(10, 1, 0, 108)))
				(void)r.boolean(); // manager controlled
			if (r.since(V(10, 1, 0, 104)))
				c.interpolator = r.ref();
		}

		void readOldControllerData(NifReader& r, NiSingleInterpController& c)
		{
			if (r.until(V(10, 1, 0, 103)))
				c.data = r.ref();
		}

		struct QuatTransform
		{
			Vec3 translation;
			Quat rotation;
			float scale = 1;
		};

		QuatTransform readQuatTransform(NifReader& r)
		{
			QuatTransform t;
			t.translation = r.vec3();
			t.rotation = r.quat();
			t.scale = r.f32();
			if (r.until(V(10, 1, 0, 109)))
				r.skip(3); // TRS valid
			return t;
		}

		void readBSplineInterpolator(NifReader& r, NiBSplineInterpolator& b)
		{
			b.startTime = r.f32();
			b.stopTime = r.f32();
			b.splineData = r.ref();
			b.basisData = r.ref();
		}

		void readDynamicEffect(NifReader& r, NiDynamicEffect& e)
		{
			readAVObject(r, e);
			if (r.since(V(10, 1, 0, 106)))
				e.switchState = r.boolean();
			if (r.since(V(10, 1, 0, 0)))
				e.affectedNodes = r.refList();
		}

		void readLight(NifReader& r, NiLight& l)
		{
			readDynamicEffect(r, l);
			l.dimmer = r.f32();
			l.ambient = r.color3();
			l.diffuse = r.color3();
			l.specular = r.color3();
		}

		void readBoundingVolume(NifReader& r, int depth = 0)
		{
			if (depth > 8)
				r.fail("bounding volume nesting too deep");
			const uint32_t type = r.u32();
			switch (type)
			{
			case 0: r.skip(16); break;                  // sphere
			case 1: r.skip(12 + 36 + 12); break;        // box
			case 2: r.skip(12 + 12 + 4 + 4); break;     // capsule
			case 4:                                     // union
			{
				const uint32_t n = r.u32();
				for (uint32_t i = 0; i < n; ++i)
					readBoundingVolume(r, depth + 1);
				break;
			}
			case 5: r.skip(16 + 12); break;             // half space
			case 0xFFFFFFFF: break;                     // none
			default: r.fail(std::format("unknown bounding volume type {}", type));
			}
		}

		// ---------------------------------------------------------------- block parsers

		std::unique_ptr<NiObject> parseNode(NifReader& r)
		{
			auto o = std::make_unique<NiNode>();
			readNode(r, *o);
			return o;
		}

		std::unique_ptr<NiObject> parseBillboardNode(NifReader& r)
		{
			auto o = std::make_unique<NiBillboardNode>();
			readNode(r, *o);
			if (r.since(V(10, 1, 0, 0)))
				o->billboardMode = r.u16();
			return o;
		}

		template <class T>
		std::unique_ptr<NiObject> parseGeometry(NifReader& r)
		{
			auto o = std::make_unique<T>();
			readGeometry(r, *o);
			return o;
		}

		std::unique_ptr<NiObject> parseTriShapeData(NifReader& r)
		{
			auto d = std::make_unique<NiTriShapeData>();
			readGeometryData(r, *d);
			d->numTriangles = r.u16();
			(void)r.u32(); // num triangle points
			bool hasTriangles = true;
			if (r.since(V(10, 1, 0, 0)))
				hasTriangles = r.boolean();
			if (hasTriangles)
				d->triangles = r.vectorOf<Triangle>(d->numTriangles);
			const uint16_t numMatchGroups = r.u16();
			for (uint16_t i = 0; i < numMatchGroups; ++i)
			{
				const uint16_t n = r.u16();
				r.requireArray(n, 2);
				r.skip(size_t(n) * 2);
			}
			return d;
		}

		std::unique_ptr<NiObject> parseTriStripsData(NifReader& r)
		{
			auto d = std::make_unique<NiTriStripsData>();
			readGeometryData(r, *d);
			d->numTriangles = r.u16();
			const uint16_t numStrips = r.u16();
			const auto lengths = r.vectorOf<uint16_t>(numStrips);
			bool hasPoints = true;
			if (r.since(V(10, 0, 1, 3)))
				hasPoints = r.boolean();
			if (hasPoints)
			{
				d->strips.reserve(numStrips);
				for (const uint16_t len : lengths)
					d->strips.push_back(r.vectorOf<uint16_t>(len));
			}
			return d;
		}

		std::unique_ptr<NiObject> parseMaterialProperty(NifReader& r)
		{
			auto p = std::make_unique<NiMaterialProperty>();
			readObjectNET(r, *p);
			if (r.until(V(10, 0, 1, 2)))
				(void)r.u16();
			p->ambient = r.color3();
			p->diffuse = r.color3();
			p->specular = r.color3();
			p->emissive = r.color3();
			p->glossiness = r.f32();
			p->alpha = r.f32();
			return p;
		}

		std::unique_ptr<NiObject> parseAlphaProperty(NifReader& r)
		{
			auto p = std::make_unique<NiAlphaProperty>();
			readObjectNET(r, *p);
			p->flags = r.u16();
			p->threshold = r.u8();
			return p;
		}

		std::unique_ptr<NiObject> parseVertexColorProperty(NifReader& r)
		{
			auto p = std::make_unique<NiVertexColorProperty>();
			readObjectNET(r, *p);
			p->flags = r.u16();
			if (r.until(V(20, 0, 0, 5)))
			{
				p->vertexMode = r.u32();
				p->lightingMode = r.u32();
			}
			return p;
		}

		std::unique_ptr<NiObject> parseZBufferProperty(NifReader& r)
		{
			auto p = std::make_unique<NiZBufferProperty>();
			readObjectNET(r, *p);
			p->flags = r.u16();
			if (r.since(V(4, 1, 0, 12)) && r.until(V(20, 0, 0, 5)))
				p->function = r.u32();
			return p;
		}

		std::unique_ptr<NiObject> parseStencilProperty(NifReader& r)
		{
			auto p = std::make_unique<NiStencilProperty>();
			readObjectNET(r, *p);
			if (r.until(V(10, 0, 1, 2)))
				(void)r.u16();
			if (!r.until(V(20, 0, 0, 5)))
				r.fail("20.1+ stencil layout not supported");
			p->enabled = r.boolean();
			p->function = r.u32();
			p->ref = r.u32();
			p->mask = r.u32();
			p->failAction = r.u32();
			p->zFailAction = r.u32();
			p->passAction = r.u32();
			p->drawMode = r.u32();
			return p;
		}

		std::unique_ptr<NiObject> parseFlagProperty(NifReader& r)
		{
			auto p = std::make_unique<NiFlagProperty>();
			readObjectNET(r, *p);
			p->flags = r.u16();
			return p;
		}

		std::unique_ptr<NiObject> parseTexturingProperty(NifReader& r)
		{
			auto p = std::make_unique<NiTexturingProperty>();
			readObjectNET(r, *p);
			if (r.until(V(10, 0, 1, 2)) || r.since(V(20, 1, 0, 2)))
				p->flags = r.u16();
			if (r.since(V(3, 3, 0, 13)) && r.until(V(20, 1, 0, 1)))
				p->applyMode = r.u32();
			p->textureCount = r.u32();

			const auto slot = [&](TexSlot s) {
				if (r.boolean())
					p->maps[s] = readTexDesc(r);
			};
			slot(kTexBase);
			slot(kTexDark);
			slot(kTexDetail);
			slot(kTexGloss);
			slot(kTexGlow);
			if (p->textureCount > 5 && r.boolean())
			{
				p->maps[kTexBump] = readTexDesc(r);
				p->bumpLumaScale = r.f32();
				p->bumpLumaOffset = r.f32();
				p->bumpMatrix = r.read<std::array<float, 4>>();
			}
			if (!r.until(V(20, 2, 0, 4)))
				r.fail("20.2.0.5+ texturing layout not supported");
			if (p->textureCount > 6)
				slot(kTexDecal0);
			if (p->textureCount > 7)
				slot(kTexDecal1);
			if (p->textureCount > 8)
				slot(kTexDecal2);
			if (p->textureCount > 9)
				slot(kTexDecal3);

			if (!r.since(V(10, 0, 1, 0)))
				return p; // (no shader maps before 10.0.1.0)
			const uint32_t numShaderMaps = r.u32();
			r.requireArray(numShaderMaps, 1);
			p->shaderMaps.resize(numShaderMaps);
			for (auto& m : p->shaderMaps)
				if (r.boolean())
				{
					m.map = readTexDesc(r);
					m.mapId = r.u32();
				}
			return p;
		}

		std::unique_ptr<NiObject> parseSourceTexture(NifReader& r)
		{
			auto t = std::make_unique<NiSourceTexture>();
			readObjectNET(r, *t);
			t->external = r.u8() != 0;
			if (t->external)
			{
				t->fileName = r.string();
				if (r.since(V(10, 1, 0, 0)))
					(void)r.ref(); // unknown link
			}
			else
			{
				if (r.until(V(10, 0, 1, 0)))
					(void)r.u8();
				if (r.since(V(10, 1, 0, 0)))
					t->fileName = r.string();
				t->pixelData = r.ref();
			}
			t->pixelLayout = r.u32();
			t->useMipmaps = r.u32();
			t->alphaFormat = r.u32();
			t->isStatic = r.u8();
			if (r.since(V(10, 1, 0, 103)))
				t->directRender = r.boolean();
			return t;
		}

		std::unique_ptr<NiObject> parsePixelData(NifReader& r)
		{
			auto p = std::make_unique<NiPixelData>();
			p->pixelFormat = r.u32();
			if (r.until(V(10, 4, 0, 1)))
			{
				for (auto& m : p->masks)
					m = r.u32();
				p->bitsPerPixel = r.u32();
				r.skip(8); // old fast compare
				if (r.since(V(10, 1, 0, 0)))
					(void)r.u32(); // tiling
			}
			else
			{
				p->bitsPerPixel = r.u8();
				(void)r.u32(); // renderer hint
				(void)r.u32(); // extra data
				(void)r.u8();  // flags
				(void)r.u32(); // tiling
				r.skip(4 * 10); // channels
			}
			(void)r.ref(); // palette
			const uint32_t numMips = r.u32();
			p->bytesPerPixel = r.u32();
			r.requireArray(numMips, 12);
			p->mipmaps.resize(numMips);
			for (auto& m : p->mipmaps)
			{
				m.width = r.u32();
				m.height = r.u32();
				m.offset = r.u32();
			}
			const uint32_t numPixels = r.u32();
			uint32_t numFaces = 1;
			if (r.since(V(10, 4, 0, 2)))
				numFaces = r.u32();
			p->pixels = r.vectorOf<uint8_t>(size_t(numPixels) * numFaces);
			return p;
		}

		std::unique_ptr<NiObject> parseSkinInstance(NifReader& r)
		{
			auto s = std::make_unique<NiSkinInstance>();
			s->data = r.ref();
			if (r.since(V(10, 1, 0, 101)))
				s->skinPartition = r.ref();
			s->skeletonRoot = r.ref();
			s->bones = r.refList();
			return s;
		}

		std::unique_ptr<NiObject> parseSkinData(NifReader& r)
		{
			auto s = std::make_unique<NiSkinData>();
			s->skinTransform = r.transform();
			const uint32_t numBones = r.u32();
			if (r.since(V(4, 0, 0, 2)) && r.until(V(10, 1, 0, 0)))
				(void)r.ref(); // skin partition
			uint8_t hasWeights = 1;
			if (r.since(V(4, 2, 1, 0)))
				hasWeights = r.u8();
			r.requireArray(numBones, 4 * 13 + 4 * 4 + 2);
			s->bones.resize(numBones);
			for (auto& b : s->bones)
			{
				b.skinTransform = r.transform();
				b.boundCenter = r.vec3();
				b.boundRadius = r.f32();
				const uint16_t n = r.u16();
				if (hasWeights)
				{
					r.requireArray(n, 6);
					b.weights.resize(n);
					for (auto& w : b.weights)
					{
						w.vertex = r.u16();
						w.weight = r.f32();
					}
				}
			}
			return s;
		}

		std::unique_ptr<NiObject> parseSkinPartition(NifReader& r)
		{
			auto s = std::make_unique<NiSkinPartition>();
			const uint32_t num = r.u32();
			r.requireArray(num, 10);
			s->partitions.resize(num);
			for (auto& p : s->partitions)
			{
				p.numVertices = r.u16();
				const uint16_t numTriangles = r.u16();
				const uint16_t numBones = r.u16();
				const uint16_t numStrips = r.u16();
				p.numWeightsPerVertex = r.u16();
				const size_t nv = p.numVertices, nw = p.numWeightsPerVertex;
				p.bones = r.vectorOf<uint16_t>(numBones);
				// (before 10.1 every array is there without its "has" flag, but the bone indices')
				const bool flagged = r.since(V(10, 1, 0, 0));
				const auto has = [&] { return !flagged || r.boolean(); };
				if (has())
					p.vertexMap = r.vectorOf<uint16_t>(nv);
				if (has())
					p.vertexWeights = r.vectorOf<float>(nv * nw);
				const auto stripLengths = r.vectorOf<uint16_t>(numStrips);
				if (has()) // faces
				{
					if (numStrips != 0)
						for (const uint16_t len : stripLengths)
							p.strips.push_back(r.vectorOf<uint16_t>(len));
					else
						p.triangles = r.vectorOf<Triangle>(numTriangles);
				}
				if (r.boolean())
					p.boneIndices = r.vectorOf<uint8_t>(nv * nw);
			}
			return s;
		}

		std::unique_ptr<NiObject> parseStringExtraData(NifReader& r)
		{
			auto e = std::make_unique<NiStringExtraData>();
			readExtraData(r, *e);
			if (r.until(V(4, 2, 2, 0)))
				(void)r.u32(); // the bytes left
			e->value = r.string();
			return e;
		}

		std::unique_ptr<NiObject> parseIntegerExtraData(NifReader& r)
		{
			auto e = std::make_unique<NiIntegerExtraData>();
			readExtraData(r, *e);
			e->value = r.u32();
			return e;
		}

		std::unique_ptr<NiObject> parseFloatExtraData(NifReader& r)
		{
			auto e = std::make_unique<NiFloatExtraData>();
			readExtraData(r, *e);
			e->value = r.f32();
			return e;
		}

		std::unique_ptr<NiObject> parseFloatsExtraData(NifReader& r)
		{
			auto e = std::make_unique<NiFloatsExtraData>();
			readExtraData(r, *e);
			const uint32_t n = r.u32();
			e->values = r.vectorOf<float>(n);
			return e;
		}

		std::unique_ptr<NiObject> parseBooleanExtraData(NifReader& r)
		{
			auto e = std::make_unique<NiIntegerExtraData>();
			readExtraData(r, *e);
			e->value = r.u8();
			return e;
		}

		std::unique_ptr<NiObject> parseColorExtraData(NifReader& r)
		{
			auto e = std::make_unique<NiFloatsExtraData>();
			readExtraData(r, *e);
			const Color4 c = r.color4();
			e->values = { c.r, c.g, c.b, c.a };
			return e;
		}

		std::unique_ptr<NiObject> parseTextKeyExtraData(NifReader& r)
		{
			auto e = std::make_unique<NiTextKeyExtraData>();
			readExtraData(r, *e);
			if (r.until(V(4, 2, 2, 0)))
				(void)r.u32();
			const uint32_t n = r.u32();
			r.requireArray(n, 8);
			e->keys.resize(n);
			for (auto& [time, text] : e->keys)
			{
				time = r.f32();
				text = r.string();
			}
			return e;
		}

		std::unique_ptr<NiObject> parseStringPalette(NifReader& r)
		{
			auto p = std::make_unique<NiStringPalette>();
			p->palette = r.readSizedString();
			const uint32_t length = r.u32();
			if (length != p->palette.size())
				r.fail("string palette length mismatch");
			return p;
		}

		std::unique_ptr<NiObject> parseTransformData(NifReader& r)
		{
			auto d = std::make_unique<NiTransformData>();
			const uint32_t numRotationKeys = r.u32();
			if (numRotationKeys != 0)
				d->rotationType = readKeyType(r);
			if (d->rotationType != KeyType::XyzRotation)
			{
				r.requireArray(numRotationKeys, 4 + 16);
				d->quaternionKeys.resize(numRotationKeys);
				for (auto& k : d->quaternionKeys)
				{
					k.time = r.f32();
					k.value = r.quat();
					if (d->rotationType == KeyType::TBC)
						k.tbc = r.read<std::array<float, 3>>();
				}
			}
			else
			{
				if (r.until(V(10, 1, 0, 0)))
					(void)r.f32(); // order
				for (auto& g : d->xyzRotations)
					g = readKeyGroup<float>(r);
			}
			d->translations = readKeyGroup<Vec3>(r);
			d->scales = readKeyGroup<float>(r);
			return d;
		}

		std::unique_ptr<NiObject> parseFloatData(NifReader& r)
		{
			auto d = std::make_unique<NiFloatData>();
			d->data = readKeyGroup<float>(r);
			return d;
		}

		std::unique_ptr<NiObject> parsePosData(NifReader& r)
		{
			auto d = std::make_unique<NiPosData>();
			d->data = readKeyGroup<Vec3>(r);
			return d;
		}

		std::unique_ptr<NiObject> parseBoolData(NifReader& r)
		{
			auto d = std::make_unique<NiBoolData>();
			d->data = readKeyGroup<uint8_t>(r);
			return d;
		}

		std::unique_ptr<NiObject> parseVisData(NifReader& r)
		{
			auto d = std::make_unique<NiBoolData>();
			const uint32_t n = r.u32();
			r.requireArray(n, 5);
			d->data.interpolation = KeyType::Const;
			d->data.keys.resize(n);
			for (auto& k : d->data.keys)
			{
				k.time = r.f32();
				k.value = r.u8();
			}
			return d;
		}

		std::unique_ptr<NiObject> parseColorData(NifReader& r)
		{
			auto d = std::make_unique<NiColorData>();
			d->data = readKeyGroup<Color4>(r);
			return d;
		}

		std::unique_ptr<NiObject> parseBSplineData(NifReader& r)
		{
			auto d = std::make_unique<NiBSplineData>();
			const uint32_t nf = r.u32();
			d->floatControlPoints = r.vectorOf<float>(nf);
			const uint32_t nc = r.u32();
			d->compactControlPoints = r.vectorOf<int16_t>(nc);
			return d;
		}

		std::unique_ptr<NiObject> parseBSplineBasisData(NifReader& r)
		{
			auto d = std::make_unique<NiBSplineBasisData>();
			d->numControlPoints = r.u32();
			return d;
		}

		std::unique_ptr<NiObject> parseTransformInterpolator(NifReader& r)
		{
			auto i = std::make_unique<NiTransformInterpolator>();
			const auto t = readQuatTransform(r);
			i->translation = t.translation;
			i->rotation = t.rotation;
			i->scale = t.scale;
			if (r.since(V(10, 1, 0, 104)) && r.until(V(10, 1, 0, 108)))
				r.skip(3);
			i->data = r.ref();
			return i;
		}

		std::unique_ptr<NiObject> parseFloatInterpolator(NifReader& r)
		{
			auto i = std::make_unique<NiFloatInterpolator>();
			i->value = r.f32();
			i->data = r.ref();
			return i;
		}

		std::unique_ptr<NiObject> parsePoint3Interpolator(NifReader& r)
		{
			auto i = std::make_unique<NiPoint3Interpolator>();
			i->value = r.vec3();
			i->data = r.ref();
			return i;
		}

		std::unique_ptr<NiObject> parseBoolInterpolator(NifReader& r)
		{
			auto i = std::make_unique<NiBoolInterpolator>();
			i->value = r.boolean();
			i->data = r.ref();
			return i;
		}

		std::unique_ptr<NiObject> parseLookAtInterpolator(NifReader& r)
		{
			auto i = std::make_unique<NiLookAtInterpolator>();
			i->flags = r.u16();
			i->lookAt = r.ref();
			i->lookAtName = r.string();
			const auto t = readQuatTransform(r);
			i->translation = t.translation;
			i->rotation = t.rotation;
			i->scale = t.scale;
			i->translationInterp = r.ref();
			i->rollInterp = r.ref();
			i->scaleInterp = r.ref();
			return i;
		}

		std::unique_ptr<NiObject> parsePathInterpolator(NifReader& r)
		{
			auto i = std::make_unique<NiPathInterpolator>();
			i->flags = r.u16();
			i->bankDir = r.i32();
			i->maxBankAngle = r.f32();
			i->smoothing = r.f32();
			i->followAxis = r.i16();
			i->pathData = r.ref();
			i->percentData = r.ref();
			return i;
		}

		std::unique_ptr<NiObject> parseBSplineCompTransformInterpolator(NifReader& r)
		{
			auto i = std::make_unique<NiBSplineCompTransformInterpolator>();
			readBSplineInterpolator(r, *i);
			const auto t = readQuatTransform(r);
			i->translation = t.translation;
			i->rotation = t.rotation;
			i->scale = t.scale;
			i->translationHandle = r.u32();
			i->rotationHandle = r.u32();
			i->scaleHandle = r.u32();
			i->translationBias = r.f32();
			i->translationMultiplier = r.f32();
			i->rotationBias = r.f32();
			i->rotationMultiplier = r.f32();
			i->scaleBias = r.f32();
			i->scaleMultiplier = r.f32();
			return i;
		}

		std::unique_ptr<NiObject> parseBSplineCompFloatInterpolator(NifReader& r)
		{
			auto i = std::make_unique<NiBSplineCompFloatInterpolator>();
			readBSplineInterpolator(r, *i);
			i->value = r.f32();
			i->handle = r.u32();
			i->offset = r.f32();
			i->halfRange = r.f32();
			return i;
		}

		std::unique_ptr<NiObject> parseBSplineCompPoint3Interpolator(NifReader& r)
		{
			auto i = std::make_unique<NiBSplineCompPoint3Interpolator>();
			readBSplineInterpolator(r, *i);
			i->value = r.vec3();
			i->handle = r.u32();
			i->offset = r.f32();
			i->halfRange = r.f32();
			return i;
		}

		// Controllers whose extra fields are only the pre-10.1.0.104 data ref.
		std::unique_ptr<NiObject> parseSimpleInterpController(NifReader& r)
		{
			auto c = std::make_unique<NiSingleInterpController>();
			readSingleInterpController(r, *c);
			readOldControllerData(r, *c);
			return c;
		}

		std::unique_ptr<NiObject> parseLightDimmerController(NifReader& r)
		{
			auto c = std::make_unique<NiSingleInterpController>();
			readSingleInterpController(r, *c);
			return c;
		}

		std::unique_ptr<NiObject> parseColorController(NifReader& r)
		{
			auto c = std::make_unique<NiSingleInterpController>();
			readSingleInterpController(r, *c);
			if (r.since(V(10, 1, 0, 0)))
				c->targetColor = r.u16();
			readOldControllerData(r, *c);
			return c;
		}

		std::unique_ptr<NiObject> parseTextureTransformController(NifReader& r)
		{
			auto c = std::make_unique<NiSingleInterpController>();
			readSingleInterpController(r, *c);
			c->shaderMap = r.boolean();
			c->textureSlot = r.u32();
			c->operation = r.u32();
			readOldControllerData(r, *c);
			return c;
		}

		std::unique_ptr<NiObject> parseFloatExtraDataController(NifReader& r)
		{
			auto c = std::make_unique<NiSingleInterpController>();
			readSingleInterpController(r, *c);
			if (r.since(V(10, 2, 0, 0)))
				c->extraDataName = r.string();
			if (r.until(V(10, 1, 0, 0)))
			{
				const uint8_t n = r.u8();
				r.skip(7);
				r.skip(n);
			}
			readOldControllerData(r, *c);
			return c;
		}

		std::unique_ptr<NiObject> parseMultiTargetTransformController(NifReader& r)
		{
			auto c = std::make_unique<NiMultiTargetTransformController>();
			readTimeController(r, *c);
			if (r.since(V(10, 1, 0, 104)) && r.until(V(10, 1, 0, 108)))
				(void)r.boolean();
			const uint16_t n = r.u16();
			c->extraTargets = r.vectorOf<Ref>(n);
			return c;
		}

		// NiSwitchNode, NiLODNode: a node of which one child shows (here the first, the nearest level).
		std::unique_ptr<NiObject> parseSwitchNode(NifReader& r, bool lod)
		{
			auto o = std::make_unique<NiNode>();
			readNode(r, *o);
			if (r.since(V(10, 1, 0, 0)))
				(void)r.u16(); // switch flags
			(void)r.u32(); // the child shown
			if (lod)
			{
				if (r.since(V(10, 1, 0, 0)))
					(void)r.ref(); // NiRangeLODData
				else
				{
					(void)r.vec3();
					const uint32_t n = r.u32();
					r.requireArray(n, 8);
					r.skip(size_t(n) * 8);
				}
			}
			if (o->children.size() > 1)
				o->children.resize(1);
			return o;
		}

		std::unique_ptr<NiObject> parseRangeLODData(NifReader& r)
		{
			auto d = std::make_unique<NiLODData>();
			(void)r.vec3(); // center
			const uint32_t n = r.u32();
			r.requireArray(n, 8);
			r.skip(size_t(n) * 8); // near, far
			return d;
		}

		std::unique_ptr<NiObject> parseBoneLODController(NifReader& r)
		{
			auto c = std::make_unique<NiBoneLODController>();
			readTimeController(r, *c);
			(void)r.u32(); // the level
			const uint32_t groups = r.u32();
			(void)r.u32();
			for (uint32_t i = 0; i < groups; ++i)
			{
				const uint32_t n = r.u32();
				r.requireArray(n, 4);
				r.skip(size_t(n) * 4);
			}
			const uint32_t shapeGroups = r.u32();
			for (uint32_t i = 0; i < shapeGroups; ++i)
			{
				const uint32_t n = r.u32();
				r.requireArray(n, 8);
				r.skip(size_t(n) * 8); // shape, skin instance
			}
			const uint32_t shapes = r.u32();
			r.requireArray(shapes, 4);
			r.skip(size_t(shapes) * 4);
			return c;
		}

		// (a manager's: flags with bit 0 set, then no items of its own)
		std::unique_ptr<NiObject> parseBlendInterpolator(NifReader& r, size_t valueBytes)
		{
			auto b = std::make_unique<NiBlendInterpolator>();
			const uint8_t flags = r.u8();
			(void)r.u8(); // array size
			(void)r.f32(); // weight threshold
			if (!(flags & 1))
				r.fail("blend interpolator not controlled by a manager");
			r.skip(valueBytes);
			return b;
		}

		std::unique_ptr<NiObject> parseStringsExtraData(NifReader& r)
		{
			auto e = std::make_unique<NiStringExtraData>();
			readExtraData(r, *e);
			const uint32_t n = r.u32();
			r.requireArray(n, 4);
			for (uint32_t i = 0; i < n; ++i)
				e->value += (i ? "\n" : "") + r.readSizedString();
			return e;
		}

		std::unique_ptr<NiObject> parseBinaryExtraData(NifReader& r)
		{
			auto e = std::make_unique<NiBinaryExtraData>();
			readExtraData(r, *e);
			const uint32_t n = r.u32();
			r.requireArray(n, 1);
			e->data.resize(n);
			for (auto& b : e->data)
				b = r.u8();
			return e;
		}

		// The old particle system (as the public NifTools description has it, up to 20.0.0.4).
		std::unique_ptr<NiObject> parseOldParticlesData(NifReader& r, bool rotating)
		{
			auto d = std::make_unique<NiOldParticlesData>();
			readGeometryData(r, *d);
			const size_t n = d->numVertices;
			if (r.until(V(10, 0, 1, 0)))
				(void)r.f32(); // particle radius
			if (r.since(V(10, 1, 0, 0)) && r.boolean())
				r.skip(n * 4); // radii
			(void)r.u16(); // active
			if (r.boolean())
				r.skip(n * 4); // sizes
			if (r.since(V(10, 0, 1, 0)) && r.boolean())
				r.skip(n * 16); // rotations
			if (r.since(V(20, 0, 0, 4)))
			{
				if (r.boolean())
					r.skip(n * 4); // rotation angles
				if (r.boolean())
					r.skip(n * 12); // rotation axes
			}
			if (rotating && r.until(V(4, 2, 2, 0)) && r.boolean())
				r.skip(n * 16); // rotations, the old place
			return d;
		}

		std::unique_ptr<NiObject> parseParticleSystemController(NifReader& r)
		{
			auto c = std::make_unique<NiParticleSystemController>();
			readTimeController(r, *c);
			if (!r.since(V(4, 0, 0, 2)))
				r.fail("particle system controller before 4.0.0.2 not supported");
			r.skip(4 * 6);  // speed, its spread, vertical and horizontal direction and angle
			r.skip(12 + 16); // normal, colour
			r.skip(4 * 3);  // size, emit start, emit stop
			r.skip(1);
			r.skip(4 * 3);  // emit rate, lifetime, its spread
			r.skip(2);      // emit flags
			r.skip(12);     // start spread
			(void)r.ref();  // emitter
			r.skip(2 + 4 + 4 + 4 + 2);
			const uint16_t count = r.u16();
			(void)r.u16(); // valid
			r.requireArray(count, 40);
			r.skip(size_t(count) * 40); // velocity, ?, lifetime, life span, time stamp, ?, vertex
			(void)r.ref();
			c->firstModifier = r.ref();
			(void)r.ref();
			r.skip(1);
			return c;
		}

		std::unique_ptr<NiObject> parseParticleModifier(NifReader& r, size_t extra)
		{
			auto m = std::make_unique<NiParticleModifier>();
			m->next = r.ref();
			if (r.since(V(4, 0, 0, 2)))
				(void)r.ref(); // controller
			r.skip(extra);
			return m;
		}

		std::unique_ptr<NiObject> parseGravity(NifReader& r)
		{
			auto m = std::make_unique<NiParticleModifier>();
			m->next = r.ref();
			if (r.since(V(4, 0, 0, 2)))
				(void)r.ref();
			if (r.since(V(4, 0, 0, 2)))
				(void)r.f32();
			r.skip(4 + 4 + 12 + 12); // force, type, position, direction
			return m;
		}

		std::unique_ptr<NiObject> parseParticleBomb(NifReader& r)
		{
			auto m = std::make_unique<NiParticleModifier>();
			m->next = r.ref();
			if (r.since(V(4, 0, 0, 2)))
				(void)r.ref();
			r.skip(4 * 4 + 4); // decay, duration, delta v, start, decay type
			if (r.since(V(4, 1, 0, 12)))
				r.skip(4); // symmetry
			r.skip(12 + 12); // position, direction
			return m;
		}

		std::unique_ptr<NiObject> parseCollider(NifReader& r, bool planar)
		{
			auto m = std::make_unique<NiParticleModifier>();
			m->next = r.ref();
			if (r.since(V(4, 0, 0, 2)))
				(void)r.ref();
			r.skip(4); // bounce
			if (r.since(V(4, 2, 0, 2)))
				r.skip(2); // spawn, die on collide
			if (planar)
				r.skip(4 + 4 + 12 + 12 + 12 + 16); // height, width, position, x and y vectors, plane
			else
				r.skip(4 + 12); // radius, position
			return m;
		}

		// A paletted texture's colours: whether they have alpha, then the entries (RGBA bytes).
		std::unique_ptr<NiObject> parsePalette(NifReader& r)
		{
			auto p = std::make_unique<NiLODData>();
			(void)r.u8();
			const uint32_t n = r.u32();
			r.requireArray(n, 4);
			r.skip(size_t(n) * 4);
			return p;
		}

		std::unique_ptr<NiObject> parseControllerManager(NifReader& r)
		{
			auto c = std::make_unique<NiControllerManager>();
			readTimeController(r, *c);
			c->cumulative = r.boolean();
			const uint32_t n = r.u32();
			c->sequences = r.vectorOf<Ref>(n);
			c->objectPalette = r.ref();
			return c;
		}

		std::unique_ptr<NiObject> parseDefaultAVObjectPalette(NifReader& r)
		{
			auto p = std::make_unique<NiDefaultAVObjectPalette>();
			p->scene = r.ref();
			const uint32_t n = r.u32();
			r.requireArray(n, 8);
			p->objects.reserve(n);
			for (uint32_t i = 0; i < n; ++i)
			{
				std::string name = r.readSizedString();
				p->objects.emplace_back(std::move(name), r.ref());
			}
			return p;
		}

		std::unique_ptr<NiObject> parseGeomMorpherController(NifReader& r)
		{
			auto c = std::make_unique<NiGeomMorpherController>();
			readTimeController(r, *c);
			if (r.since(V(10, 1, 0, 104)) && r.until(V(10, 1, 0, 108)))
				(void)r.boolean();
			if (r.since(V(10, 0, 1, 2)))
				c->morpherFlags = r.u16();
			c->data = r.ref();
			c->alwaysUpdate = r.u8();
			if (r.since(V(10, 1, 0, 106)))
			{
				const uint32_t n = r.u32();
				c->interpolators = r.vectorOf<Ref>(n);
			}
			return c;
		}

		std::unique_ptr<NiObject> parseMorphData(NifReader& r)
		{
			auto d = std::make_unique<NiMorphData>();
			const uint32_t numMorphs = r.u32();
			const uint32_t numVertices = r.u32();
			d->relativeTargets = r.u8();
			r.requireArray(numMorphs, 4);
			d->morphs.resize(numMorphs);
			for (auto& m : d->morphs)
			{
				if (r.since(V(10, 1, 0, 106)))
					m.frameName = r.string();
				if (r.until(V(10, 1, 0, 0)))
					m.keys = readKeyGroup<float>(r);
				if (r.since(V(10, 1, 0, 104)) && r.until(V(20, 1, 0, 2)))
					m.legacyWeight = r.f32();
				m.vectors = r.vectorOf<Vec3>(numVertices);
			}
			return d;
		}

		std::unique_ptr<NiObject> parseLookAtController(NifReader& r)
		{
			auto c = std::make_unique<NiSingleInterpController>();
			readTimeController(r, *c);
			if (r.since(V(10, 1, 0, 0)))
				(void)r.u16(); // flags
			c->data = r.ref(); // look-at node
			return c;
		}

		std::unique_ptr<NiObject> parseControllerSequence(NifReader& r)
		{
			auto s = std::make_unique<NiControllerSequence>();
			s->name = r.string();
			if (r.until(V(10, 1, 0, 103)))
			{
				s->accumRootName = r.string();
				s->textKeys = r.ref();
			}
			const uint32_t numBlocks = r.u32();
			if (r.since(V(10, 1, 0, 106)))
				(void)r.u32(); // array grow by
			r.requireArray(numBlocks, 8);
			s->controlledBlocks.resize(numBlocks);
			for (auto& b : s->controlledBlocks)
			{
				if (r.until(V(10, 1, 0, 103)))
				{
					b.targetName = r.string();
					b.controller = r.ref();
				}
				if (r.since(V(10, 1, 0, 106)))
				{
					b.interpolator = r.ref();
					b.controller = r.ref();
				}
				if (r.since(V(10, 1, 0, 104)) && r.until(V(10, 1, 0, 110)))
				{
					(void)r.ref(); // blend interpolator
					(void)r.u16(); // blend index
				}
				if (r.since(V(10, 1, 0, 106)) && r.userVersion > 0)
					(void)r.u8(); // priority (Bethesda only)
				if (r.since(V(10, 2, 0, 0)) && r.until(V(20, 1, 0, 0)))
				{
					b.stringPalette = r.ref();
					b.nodeNameOffset = r.u32();
					b.propertyTypeOffset = r.u32();
					b.controllerTypeOffset = r.u32();
					b.controllerIdOffset = r.u32();
					b.interpolatorIdOffset = r.u32();
				}
			}
			if (r.since(V(10, 1, 0, 106)))
			{
				s->weight = r.f32();
				s->textKeys = r.ref();
				s->cycleType = r.u32();
				if (r.version == V(10, 1, 0, 106))
					(void)r.u32();
				s->frequency = r.f32();
				if (r.until(V(10, 4, 0, 1)))
					(void)r.f32(); // phase
				s->startTime = r.f32();
				s->stopTime = r.f32();
				if (r.version == V(10, 1, 0, 106))
					(void)r.boolean();
				s->manager = r.ref();
				s->accumRootName = r.string();
				if (r.since(V(10, 1, 0, 113)) && r.until(V(20, 1, 0, 0)))
					s->stringPalette = r.ref();
			}
			return s;
		}

		std::unique_ptr<NiObject> parseAmbientOrDirectionalLight(NifReader& r)
		{
			auto l = std::make_unique<NiLight>();
			readLight(r, *l);
			return l;
		}

		std::unique_ptr<NiObject> parsePointLight(NifReader& r)
		{
			auto l = std::make_unique<NiLight>();
			readLight(r, *l);
			l->constantAttenuation = r.f32();
			l->linearAttenuation = r.f32();
			l->quadraticAttenuation = r.f32();
			return l;
		}

		std::unique_ptr<NiObject> parseSpotLight(NifReader& r)
		{
			auto l = std::make_unique<NiLight>();
			readLight(r, *l);
			l->constantAttenuation = r.f32();
			l->linearAttenuation = r.f32();
			l->quadraticAttenuation = r.f32();
			l->outerSpotAngle = r.f32();
			if (r.since(V(20, 2, 0, 5)))
				(void)r.f32(); // inner spot angle
			l->spotExponent = r.f32();
			return l;
		}

		std::unique_ptr<NiObject> parseTextureEffect(NifReader& r)
		{
			auto e = std::make_unique<NiTextureEffect>();
			readDynamicEffect(r, *e);
			e->projectionMatrix = r.mat33();
			e->projectionTranslation = r.vec3();
			e->filtering = r.u32();
			e->clamping = r.u32();
			e->textureType = r.u32();
			e->coordGenType = r.u32();
			e->sourceTexture = r.ref();
			e->enablePlane = r.u8();
			e->planeNormal = r.vec3();
			e->planeConstant = r.f32();
			if (r.until(V(10, 2, 0, 0)))
				r.skip(4); // PS2 L, K
			return e;
		}

		std::unique_ptr<NiObject> parseCamera(NifReader& r)
		{
			auto c = std::make_unique<NiCamera>();
			readAVObject(r, *c);
			if (r.since(V(10, 1, 0, 0)))
				(void)r.u16(); // camera flags
			c->frustumLeft = r.f32();
			c->frustumRight = r.f32();
			c->frustumTop = r.f32();
			c->frustumBottom = r.f32();
			c->frustumNear = r.f32();
			c->frustumFar = r.f32();
			if (r.since(V(10, 1, 0, 0)))
				c->orthographic = r.boolean();
			r.skip(4 * 4); // viewport
			(void)r.f32();  // LOD adjust
			c->scene = r.ref();
			(void)r.refList(); // screen polygons
			(void)r.refList(); // screen textures
			return c;
		}

		std::unique_ptr<NiObject> parseCollisionData(NifReader& r)
		{
			auto o = std::make_unique<NiOpaque>();
			(void)r.ref(); // target
			(void)r.u32(); // propagation mode
			if (r.since(V(10, 1, 0, 0)))
				(void)r.u32(); // collision mode
			if (r.u8() == 1)
				readBoundingVolume(r);
			return o;
		}

		// ---------------------------------------------------------------- particles

		std::unique_ptr<NiObject> parseParticleSystem(NifReader& r)
		{
			auto p = std::make_unique<NiParticleSystem>();
			readGeometry(r, *p);
			if (r.since(V(10, 1, 0, 0)))
			{
				p->worldSpace = r.boolean();
				p->modifiers = r.refList();
			}
			return p;
		}

		std::unique_ptr<NiObject> parsePSysData(NifReader& r)
		{
			auto d = std::make_unique<NiPSysData>();
			readGeometryData(r, *d);
			const size_t n = d->numVertices;
			// NiParticlesData
			if (r.since(V(10, 1, 0, 0)) && r.boolean())
				d->radii = r.vectorOf<float>(n);
			d->numActive = r.u16();
			if (r.boolean())
				d->sizes = r.vectorOf<float>(n);
			if (r.boolean())
				d->rotations = r.vectorOf<Quat>(n);
			if (r.since(V(20, 0, 0, 4)))
			{
				if (r.boolean())
					d->rotationAngles = r.vectorOf<float>(n);
				if (r.boolean())
					d->rotationAxes = r.vectorOf<Vec3>(n);
			}
			// NiPSysData: particle info (velocity, age, life span, last update, generation, code)
			r.requireArray(n, 28);
			r.skip(n * (r.until(V(10, 4, 0, 1)) ? 40 : 28));
			if (r.since(V(20, 0, 0, 2)) && r.boolean())
				d->rotationSpeeds = r.vectorOf<float>(n);
			(void)r.u16(); // num added particles
			(void)r.u16(); // added particles base
			return d;
		}

		struct PSysBuilder
		{
			NifReader& r;
			std::unique_ptr<NiPSysObject> o = std::make_unique<NiPSysObject>();

			PSysBuilder& f(int count = 1)
			{
				for (int i = 0; i < count; ++i)
					o->params.push_back(r.f32());
				return *this;
			}
			PSysBuilder& u32(int count = 1)
			{
				for (int i = 0; i < count; ++i)
					o->params.push_back(static_cast<float>(r.u32()));
				return *this;
			}
			PSysBuilder& u16(int count = 1)
			{
				for (int i = 0; i < count; ++i)
					o->params.push_back(r.u16());
				return *this;
			}
			PSysBuilder& b(int count = 1)
			{
				for (int i = 0; i < count; ++i)
					o->params.push_back(r.boolean() ? 1.f : 0.f);
				return *this;
			}
			PSysBuilder& ref(int count = 1)
			{
				for (int i = 0; i < count; ++i)
					o->refs.push_back(r.ref());
				return *this;
			}
			PSysBuilder& refList()
			{
				const auto list = r.refList();
				o->refs.insert(o->refs.end(), list.begin(), list.end());
				return *this;
			}

			// NiPSysModifier: name, order, target, active
			PSysBuilder& modifier()
			{
				o->name = r.string();
				return u32().ref().b();
			}
			// NiPSysEmitter: speed..life span variation
			PSysBuilder& emitter()
			{
				modifier().f(6).f(4); // speed, var, declination, var, planar angle, var, initial color
				f();                  // initial radius
				if (r.since(V(10, 4, 0, 1)))
					f(); // radius variation
				else
					o->params.push_back(0); // (none: the same layout either way)
				return f(2);           // life span, variation
			}
			PSysBuilder& volumeEmitter()
			{
				emitter();
				if (r.since(V(10, 1, 0, 0)))
					ref(); // emitter object
				return *this;
			}
			// NiPSysModifierCtlr: NiSingleInterpController + modifier name
			void keepTime(const NiTimeController& t)
			{
				o->nextController = t.nextController;
				o->flags = t.flags;
				o->frequency = t.frequency, o->phase = t.phase, o->startTime = t.startTime, o->stopTime = t.stopTime;
			}
			PSysBuilder& modifierCtlr()
			{
				NiSingleInterpController tmp;
				readSingleInterpController(r, tmp);
				keepTime(tmp);
				o->refs.push_back(tmp.interpolator);
				o->name = r.string();
				return *this;
			}
			PSysBuilder& timeCtlr()
			{
				NiTimeController tmp;
				readTimeController(r, tmp);
				keepTime(tmp);
				return *this;
			}
			PSysBuilder& oldData()
			{
				if (r.until(V(10, 1, 0, 103)))
					ref();
				return *this;
			}

			std::unique_ptr<NiObject> done() { return std::move(o); }
		};

		std::unique_ptr<NiObject> parsePSysAgeDeathModifier(NifReader& r) { return PSysBuilder{ r }.modifier().b().ref().done(); }
		std::unique_ptr<NiObject> parsePSysBoundUpdateModifier(NifReader& r) { return PSysBuilder{ r }.modifier().u16().done(); }
		std::unique_ptr<NiObject> parsePSysPositionModifier(NifReader& r) { return PSysBuilder{ r }.modifier().done(); }
		std::unique_ptr<NiObject> parsePSysSpawnModifier(NifReader& r) { return PSysBuilder{ r }.modifier().u16().f().u16(2).f(4).done(); }
		std::unique_ptr<NiObject> parsePSysGrowFadeModifier(NifReader& r) { return PSysBuilder{ r }.modifier().f().u16().f().u16().done(); }
		std::unique_ptr<NiObject> parsePSysColorModifier(NifReader& r) { return PSysBuilder{ r }.modifier().ref().done(); }
		std::unique_ptr<NiObject> parsePSysGravityModifier(NifReader& r) { return PSysBuilder{ r }.modifier().ref().f(3).f(2).u32().f(2).done(); }
		std::unique_ptr<NiObject> parsePSysDragModifier(NifReader& r) { return PSysBuilder{ r }.modifier().ref().f(3).f(3).done(); }
		std::unique_ptr<NiObject> parsePSysColliderManager(NifReader& r) { return PSysBuilder{ r }.modifier().ref().done(); }
		std::unique_ptr<NiObject> parsePSysMeshUpdateModifier(NifReader& r) { return PSysBuilder{ r }.modifier().refList().done(); }

		std::unique_ptr<NiObject> parsePSysRotationModifier(NifReader& r)
		{
			PSysBuilder p{ r };
			p.modifier().f();
			if (r.since(V(20, 0, 0, 2)))
				p.f().f(2).b(); // speed variation, angle, angle variation, random sign
			else
				p.o->params.insert(p.o->params.end(), 4, 0.0f); // (none: the same layout either way)
			return p.b().f(3).done(); // random axis, axis
		}

		std::unique_ptr<NiObject> parsePSysPlanarCollider(NifReader& r)
		{
			// NiPSysCollider: bounce, spawn on collide, die on collide, spawn modifier, parent, next, collider object
			return PSysBuilder{ r }.f().b(2).ref(4).f(2).f(6).done();
		}

		std::unique_ptr<NiObject> parsePSysBoxEmitter(NifReader& r) { return PSysBuilder{ r }.volumeEmitter().f(3).done(); }
		std::unique_ptr<NiObject> parsePSysSphereEmitter(NifReader& r) { return PSysBuilder{ r }.volumeEmitter().f().done(); }
		std::unique_ptr<NiObject> parsePSysCylinderEmitter(NifReader& r) { return PSysBuilder{ r }.volumeEmitter().f(2).done(); }
		std::unique_ptr<NiObject> parsePSysMeshEmitter(NifReader& r) { return PSysBuilder{ r }.emitter().refList().u32(2).f(3).done(); }

		std::unique_ptr<NiObject> parsePSysUpdateCtlr(NifReader& r) { return PSysBuilder{ r }.timeCtlr().done(); }
		std::unique_ptr<NiObject> parsePSysResetOnLoopCtlr(NifReader& r) { return PSysBuilder{ r }.timeCtlr().done(); }

		std::unique_ptr<NiObject> parsePSysEmitterCtlr(NifReader& r)
		{
			PSysBuilder p{ r };
			p.modifierCtlr().oldData();
			if (r.since(V(10, 1, 0, 104)))
				p.ref(); // visibility interpolator
			return p.done();
		}

		std::unique_ptr<NiObject> parsePSysModifierValueCtlr(NifReader& r) { return PSysBuilder{ r }.modifierCtlr().oldData().done(); }

		std::unique_ptr<NiObject> parseMeshPSysData(NifReader& r)
		{
			auto d = parsePSysData(r);
			if (r.since(V(10, 2, 0, 0)))
			{
				(void)r.u32();     // default pool size
				(void)r.boolean(); // fill pools on load
				const uint32_t n = r.u32();
				r.requireArray(n, 4);
				r.skip(size_t(n) * 4); // generations
			}
			(void)r.ref(); // particle meshes
			return d;
		}

		std::unique_ptr<NiObject> parseUVController(NifReader& r)
		{
			auto c = std::make_unique<NiSingleInterpController>();
			readTimeController(r, *c);
			c->textureSlot = r.u16(); // texture set
			c->data = r.ref();
			return c;
		}

		std::unique_ptr<NiObject> parseUVData(NifReader& r)
		{
			auto d = std::make_unique<NiUVData>();
			for (auto& g : d->groups) // u translation, v translation, u scale, v scale
				g = readKeyGroup<float>(r);
			return d;
		}

		std::unique_ptr<NiObject> parsePSysEmitterCtlrData(NifReader& r)
		{
			// (params: the birth rate's keys as count, then time, value each; the on/off keys the same)
			auto o = std::make_unique<NiPSysObject>();
			const auto rate = readKeyGroup<float>(r).keys;
			o->params.push_back(float(rate.size()));
			for (const auto& k : rate)
				o->params.insert(o->params.end(), { k.time, k.value });
			const uint32_t numActive = r.u32();
			r.requireArray(numActive, 5);
			o->params.push_back(float(numActive));
			for (uint32_t i = 0; i < numActive; ++i)
			{
				const float time = r.f32();
				o->params.insert(o->params.end(), { time, r.u8() ? 1.0f : 0.0f });
			}
			return o;
		}

		using ParseFn = std::unique_ptr<NiObject> (*)(NifReader&);

		const std::unordered_map<std::string_view, ParseFn>& registry()
		{
			static const std::unordered_map<std::string_view, ParseFn> map = {
				{ "NiNode", &parseNode },
				{ "NiBillboardNode", &parseBillboardNode },
				{ "NiTriShape", &parseGeometry<NiTriShape> },
				{ "NiTriStrips", &parseGeometry<NiTriStrips> },
				{ "NiTriShapeData", &parseTriShapeData },
				{ "NiTriStripsData", &parseTriStripsData },

				{ "NiMaterialProperty", &parseMaterialProperty },
				{ "NiAlphaProperty", &parseAlphaProperty },
				{ "NiVertexColorProperty", &parseVertexColorProperty },
				{ "NiZBufferProperty", &parseZBufferProperty },
				{ "NiStencilProperty", &parseStencilProperty },
				{ "NiSpecularProperty", &parseFlagProperty },
				{ "NiWireframeProperty", &parseFlagProperty },
				{ "NiDitherProperty", &parseFlagProperty },
				{ "NiShadeProperty", &parseFlagProperty },
				{ "NiTexturingProperty", &parseTexturingProperty },
				{ "NiSourceTexture", &parseSourceTexture },
				{ "NiPixelData", &parsePixelData },

				{ "NiSkinInstance", &parseSkinInstance },
				{ "NiSkinData", &parseSkinData },
				{ "NiSkinPartition", &parseSkinPartition },

				{ "NiStringExtraData", &parseStringExtraData },
				{ "NiIntegerExtraData", &parseIntegerExtraData },
				{ "NiFloatExtraData", &parseFloatExtraData },
				{ "NiFloatsExtraData", &parseFloatsExtraData },
				{ "NiTextKeyExtraData", &parseTextKeyExtraData },
				{ "NiStringPalette", &parseStringPalette },

				{ "NiTransformData", &parseTransformData },
				{ "NiKeyframeData", &parseTransformData },
				{ "NiFloatData", &parseFloatData },
				{ "NiPosData", &parsePosData },
				{ "NiBoolData", &parseBoolData },
				{ "NiVisData", &parseVisData },
				{ "NiColorData", &parseColorData },
				{ "NiBSplineData", &parseBSplineData },
				{ "NiBSplineBasisData", &parseBSplineBasisData },

				{ "NiTransformInterpolator", &parseTransformInterpolator },
				{ "NiFloatInterpolator", &parseFloatInterpolator },
				{ "NiPoint3Interpolator", &parsePoint3Interpolator },
				{ "NiBoolInterpolator", &parseBoolInterpolator },
				{ "NiBoolTimelineInterpolator", &parseBoolInterpolator },
				{ "NiLookAtInterpolator", &parseLookAtInterpolator },
				{ "NiPathInterpolator", &parsePathInterpolator },
				{ "NiBSplineCompTransformInterpolator", &parseBSplineCompTransformInterpolator },
				{ "NiBSplineCompFloatInterpolator", &parseBSplineCompFloatInterpolator },
				{ "NiBSplineCompPoint3Interpolator", &parseBSplineCompPoint3Interpolator },

				{ "NiTransformController", &parseSimpleInterpController },
				{ "NiKeyframeController", &parseSimpleInterpController },
				{ "NiAlphaController", &parseSimpleInterpController },
				{ "NiVisController", &parseSimpleInterpController },
				{ "NiLightDimmerController", &parseLightDimmerController },
				{ "NiMaterialColorController", &parseColorController },
				{ "NiLightColorController", &parseColorController },
				{ "NiTextureTransformController", &parseTextureTransformController },
				{ "NiFloatExtraDataController", &parseFloatExtraDataController },
				{ "NiMultiTargetTransformController", &parseMultiTargetTransformController },
				{ "NiControllerSequence", &parseControllerSequence },
				{ "NiControllerManager", &parseControllerManager },
				{ "NiPalette", &parsePalette },
				{ "NiParticles", &parseGeometry<NiOldParticles> },
				{ "NiAutoNormalParticles", &parseGeometry<NiOldParticles> },
				{ "NiRotatingParticles", &parseGeometry<NiOldParticles> },
				{ "NiParticlesData", [](NifReader& r) { return parseOldParticlesData(r, false); } },
				{ "NiAutoNormalParticlesData", [](NifReader& r) { return parseOldParticlesData(r, false); } },
				{ "NiRotatingParticlesData", [](NifReader& r) { return parseOldParticlesData(r, true); } },
				{ "NiParticleSystemController", &parseParticleSystemController },
				{ "NiParticleGrowFade", [](NifReader& r) { return parseParticleModifier(r, 8); } },
				{ "NiParticleColorModifier", [](NifReader& r) { return parseParticleModifier(r, 4); } },
				{ "NiParticleRotation", [](NifReader& r) { return parseParticleModifier(r, 1 + 12 + 4); } },
				{ "NiGravity", &parseGravity },
				{ "NiParticleBomb", &parseParticleBomb },
				{ "NiSphericalCollider", [](NifReader& r) { return parseCollider(r, false); } },
				{ "NiPlanarCollider", [](NifReader& r) { return parseCollider(r, true); } },
				{ "NiSwitchNode", [](NifReader& r) { return parseSwitchNode(r, false); } },
				{ "NiLODNode", [](NifReader& r) { return parseSwitchNode(r, true); } },
				{ "NiRangeLODData", &parseRangeLODData },
				{ "NiBoneLODController", &parseBoneLODController },
				{ "NiBlendBoolInterpolator", [](NifReader& r) { return parseBlendInterpolator(r, 1); } },
				{ "NiBlendFloatInterpolator", [](NifReader& r) { return parseBlendInterpolator(r, 4); } },
				{ "NiBlendPoint3Interpolator", [](NifReader& r) { return parseBlendInterpolator(r, 12); } },
				{ "NiBlendTransformInterpolator", [](NifReader& r) { return parseBlendInterpolator(r, 0); } },
				{ "NiBinaryExtraData", &parseBinaryExtraData },
				{ "NiStringsExtraData", &parseStringsExtraData },
				{ "NiDefaultAVObjectPalette", &parseDefaultAVObjectPalette },

				{ "NiAmbientLight", &parseAmbientOrDirectionalLight },
				{ "NiDirectionalLight", &parseAmbientOrDirectionalLight },
				{ "NiPointLight", &parsePointLight },
				{ "NiSpotLight", &parseSpotLight },
				{ "NiTextureEffect", &parseTextureEffect },
				{ "NiCamera", &parseCamera },
				{ "NiCollisionData", &parseCollisionData },

				{ "NiBooleanExtraData", &parseBooleanExtraData },
				{ "NiColorExtraData", &parseColorExtraData },
				{ "NiGeomMorpherController", &parseGeomMorpherController },
				{ "NiMorphData", &parseMorphData },
				{ "NiLookAtController", &parseLookAtController },

				{ "NiParticleSystem", &parseParticleSystem },
				{ "NiMeshParticleSystem", &parseParticleSystem },
				{ "NiMeshPSysData", &parseMeshPSysData },
				{ "NiPSysMeshUpdateModifier", &parsePSysMeshUpdateModifier },
				{ "NiUVController", &parseUVController },
				{ "NiUVData", &parseUVData },
				{ "NiPSysEmitterCtlrData", &parsePSysEmitterCtlrData },
				{ "NiPSysEmitterDeclinationVarCtlr", &parsePSysModifierValueCtlr },
				{ "NiPSysEmitterDeclinationCtlr", &parsePSysModifierValueCtlr },
				{ "NiPSysEmitterSpeedCtlr", &parsePSysModifierValueCtlr },
				{ "NiPSysEmitterLifeSpanCtlr", &parsePSysModifierValueCtlr },
				{ "NiPSysEmitterPlanarAngleCtlr", &parsePSysModifierValueCtlr },
				{ "NiPSysGravityStrengthCtlr", &parsePSysModifierValueCtlr },
				{ "NiPSysData", &parsePSysData },
				{ "NiPSysAgeDeathModifier", &parsePSysAgeDeathModifier },
				{ "NiPSysBoundUpdateModifier", &parsePSysBoundUpdateModifier },
				{ "NiPSysPositionModifier", &parsePSysPositionModifier },
				{ "NiPSysSpawnModifier", &parsePSysSpawnModifier },
				{ "NiPSysGrowFadeModifier", &parsePSysGrowFadeModifier },
				{ "NiPSysColorModifier", &parsePSysColorModifier },
				{ "NiPSysRotationModifier", &parsePSysRotationModifier },
				{ "NiPSysGravityModifier", &parsePSysGravityModifier },
				{ "NiPSysDragModifier", &parsePSysDragModifier },
				{ "NiPSysColliderManager", &parsePSysColliderManager },
				{ "NiPSysPlanarCollider", &parsePSysPlanarCollider },
				{ "NiPSysBoxEmitter", &parsePSysBoxEmitter },
				{ "NiPSysSphereEmitter", &parsePSysSphereEmitter },
				{ "NiPSysCylinderEmitter", &parsePSysCylinderEmitter },
				{ "NiPSysMeshEmitter", &parsePSysMeshEmitter },
				{ "NiPSysUpdateCtlr", &parsePSysUpdateCtlr },
				{ "NiPSysResetOnLoopCtlr", &parsePSysResetOnLoopCtlr },
				{ "NiPSysEmitterCtlr", &parsePSysEmitterCtlr },
				{ "NiPSysModifierActiveCtlr", &parsePSysModifierValueCtlr },
				{ "NiPSysEmitterInitialRadiusCtlr", &parsePSysModifierValueCtlr },
			};
			return map;
		}
	}

	std::unique_ptr<NiObject> parseBlock(NifReader& r, const std::string& type)
	{
		const auto& map = registry();
		const auto it = map.find(type);
		if (it == map.end())
			r.fail(std::format("unknown block type {}", type));
		return it->second(r);
	}

	std::vector<Triangle> NiTriStripsData::triangleList() const
	{
		std::vector<Triangle> out;
		for (const auto& s : strips)
			for (size_t i = 2; i < s.size(); ++i)
			{
				Triangle t = (i % 2 == 0) ? Triangle{ s[i - 2], s[i - 1], s[i] } : Triangle{ s[i - 2], s[i], s[i - 1] };
				if (t[0] != t[1] && t[1] != t[2] && t[0] != t[2])
					out.push_back(t);
			}
		return out;
	}

	std::string_view NiStringPalette::at(uint32_t offset) const
	{
		if (offset >= palette.size())
			return {};
		const char* start = palette.data() + offset;
		return std::string_view(start, ::strnlen(start, palette.size() - offset));
	}
}
