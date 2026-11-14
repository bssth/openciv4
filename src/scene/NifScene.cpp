#include "scene/NifScene.h"

#include "nif/NifBlocks.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <format>

namespace oc4::scene
{
	std::vector<Affine> Model::bindLocals() const
	{
		std::vector<Affine> out;
		out.reserve(nodes.size());
		for (const auto& n : nodes)
			out.push_back(n.local);
		return out;
	}

	Vec2 UvTransform::apply(Vec2 uv) const
	{
		if (!enabled)
			return uv;
		Vec2 p;
		if (method == 1)
			p = { (uv.x - translation.x - center.x) * tiling.x, (uv.y - translation.y - center.y) * tiling.y };
		else
			p = { (uv.x - center.x) * tiling.x + translation.x, (uv.y - center.y) * tiling.y + translation.y };
		if (rotation != 0)
		{
			const float c = std::cos(rotation), s = std::sin(rotation);
			p = { p.x * c - p.y * s, p.x * s + p.y * c };
		}
		return { p.x + center.x, p.y + center.y };
	}

	std::vector<uint8_t> Model::bindHidden() const
	{
		std::vector<uint8_t> out;
		out.reserve(nodes.size());
		for (const auto& n : nodes)
			out.push_back(n.hidden ? 1 : 0);
		return out;
	}

	std::vector<uint8_t> Model::visibility(std::span<const uint8_t> hidden) const
	{
		std::vector<uint8_t> visible(nodes.size());
		for (size_t i = 0; i < nodes.size(); ++i)
			visible[i] = !hidden[i] && (nodes[i].parent < 0 || visible[size_t(nodes[i].parent)]);
		return visible;
	}

	std::vector<Affine> Model::worldTransforms(std::span<const Affine> locals) const
	{
		std::vector<Affine> world(nodes.size());
		for (size_t i = 0; i < nodes.size(); ++i)
			world[i] = nodes[i].parent < 0 ? locals[i] : world[size_t(nodes[i].parent)] * locals[i];
		return world;
	}

	int Model::findNode(std::string_view name) const
	{
		// Exporters write duplicate names as "name@#N" into the NIF itself, so try the exact name first.
		if (const auto it = nodesByName.find(std::string(name)); it != nodesByName.end())
			return it->second.front();
		const size_t at = name.rfind("@#");
		if (at == std::string_view::npos)
			return -1;
		size_t index = 0;
		std::from_chars(name.data() + at + 2, name.data() + name.size(), index);
		const auto it = nodesByName.find(std::string(name.substr(0, at)));
		if (it == nodesByName.end() || index >= it->second.size())
			return -1;
		return it->second[index];
	}

	namespace
	{
		using namespace nif;

		uint8_t toByte(float v) { return static_cast<uint8_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); }

		uint32_t packColor(const Color4& c)
		{
			return uint32_t(toByte(c.r)) | (uint32_t(toByte(c.g)) << 8) | (uint32_t(toByte(c.b)) << 16) | (uint32_t(toByte(c.a)) << 24);
		}

		Affine toAffine(const Transform& t) { return { t.rotation, t.translation, t.scale }; }

		scene::UvTransform toUv(const TexDesc& d)
		{
			scene::UvTransform t;
			t.enabled = d.hasTransform;
			t.translation = d.translation;
			t.tiling = d.tiling;
			t.rotation = d.rotation;
			t.center = d.center;
			t.method = d.transformMethod;
			return t;
		}

		// Properties are inherited down the graph; a child's property replaces the
		// parent's property of the same type.
		struct PropertyState
		{
			const NiMaterialProperty* material = nullptr;
			const NiTexturingProperty* texturing = nullptr;
			const NiAlphaProperty* alpha = nullptr;
			const NiVertexColorProperty* vertexColor = nullptr;
			const NiZBufferProperty* zbuffer = nullptr;
			const NiStencilProperty* stencil = nullptr;
			int materialOwner = -1, texturingOwner = -1;

			void apply(const File& f, const std::vector<Ref>& props, int node)
			{
				for (const Ref ref : props)
				{
					if (assign(f, ref, material))
						materialOwner = node;
					else if (assign(f, ref, texturing))
						texturingOwner = node;
					else
						assign(f, ref, alpha) || assign(f, ref, vertexColor) || assign(f, ref, zbuffer) || assign(f, ref, stencil);
				}
			}

			template <class T>
			static bool assign(const File& f, Ref ref, const T*& slot)
			{
				if (const auto* p = f.get<T>(ref))
				{
					slot = p;
					return true;
				}
				return false;
			}
		};

		class Builder
		{
		public:
			explicit Builder(const File& f) : m_file(f) {}

			Model run()
			{
				for (const Ref root : m_file.roots)
					addNodes(root, -1);
				m_bindWorld = m_model.worldTransforms(m_model.bindLocals());
				for (const Ref root : m_file.roots)
					visit(root, PropertyState{});
				const auto visible = m_model.visibility(m_model.bindHidden());
				for (const auto& item : m_model.items)
					if (!item.bindBounds.empty() && visible[size_t(item.node)])
					{
						m_model.bounds.add(item.bindBounds.min);
						m_model.bounds.add(item.bindBounds.max);
					}
				return std::move(m_model);
			}

		private:
			// Every NiAVObject becomes a node so bones, attachment points and
			// geometry can all be animated by name.
			void addNodes(Ref ref, int parent)
			{
				const auto* obj = m_file.get<NiAVObject>(ref);
				if (!obj || m_nodeOf.contains(ref))
					return;
				const int index = int(m_model.nodes.size());
				m_model.nodes.push_back({ obj->name, parent, { obj->rotation, obj->translation, obj->scale }, obj->hidden(), ref });
				m_model.nodesByName[obj->name].push_back(index);
				m_nodeOf[ref] = index;
				if (const auto* node = dynamic_cast<const NiNode*>(obj))
					for (const Ref child : node->children)
						addNodes(child, index);
			}

			// Hidden subtrees are built too: visibility controllers can reveal them.
			void visit(Ref ref, PropertyState props)
			{
				const auto* obj = m_file.get<NiAVObject>(ref);
				if (!obj)
					return;
				if (obj->hidden())
					++m_model.hiddenNodes;
				props.apply(m_file, obj->properties, m_nodeOf.at(ref));

				if (const auto* ps = dynamic_cast<const NiParticleSystem*>(obj))
				{
					++m_model.particleSystems;
					addParticles(ref, *ps, props);
					return;
				}
				if (const auto* geom = dynamic_cast<const NiGeometry*>(obj))
				{
					// (the shape a building clears the forest under, "CutTree": the engine's, never drawn)
					for (const Ref e : obj->extraData)
						if (const auto* s = m_file.get<NiStringExtraData>(e); s && s->value.starts_with("CutTree"))
							return;
					addGeometry(ref, *geom, props);
					return;
				}
				if (const auto* node = dynamic_cast<const NiNode*>(obj))
					for (const Ref child : node->children)
						visit(child, props);
			}

			// A particle system: its emitter, modifiers and emitter controller (their fields in the
			// order NifBlocks reads them: a modifier's order and active first).
			void addParticles(Ref ref, const NiParticleSystem& ps, const PropertyState& props)
			{
				ParticleSystem p;
				p.nodeName = ps.name;
				p.node = m_nodeOf.at(ref);
				uint32_t uvSet = 0;
				p.material = makeMaterial(ps, props, uvSet);
				p.worldSpace = ps.worldSpace;
				if (const auto* data = m_file.get<NiPSysData>(ps.data))
					p.maxParticles = std::clamp<size_t>(data->numVertices, 1, 2000);
				const auto node = [&](Ref r) {
					const auto it = m_nodeOf.find(r);
					return it == m_nodeOf.end() ? -1 : it->second;
				};
				bool hasEmitter = false;
				for (const Ref mref : ps.modifiers)
				{
					const auto* o = m_file.get<NiPSysObject>(mref);
					if (!o || mref < 0)
						continue;
					const std::string& type = m_file.typeOf(mref);
					const auto& v = o->params;
					const auto at = [&](size_t i) { return i < v.size() ? v[i] : 0.0f; };
					const bool active = at(1) != 0;
					if (!active)
						continue;
					if (type == "NiPSysBoxEmitter" || type == "NiPSysSphereEmitter" || type == "NiPSysCylinderEmitter")
					{
						hasEmitter = true;
						p.shape = type == "NiPSysBoxEmitter" ? ParticleSystem::Shape::Box
							: type == "NiPSysSphereEmitter" ? ParticleSystem::Shape::Sphere : ParticleSystem::Shape::Cylinder;
						p.speed = at(2), p.speedVariation = at(3);
						p.declination = at(4), p.declinationVariation = at(5);
						p.planarAngle = at(6), p.planarAngleVariation = at(7);
						p.color = { at(8), at(9), at(10), at(11) };
						p.radius = at(12), p.radiusVariation = at(13);
						p.lifeSpan = at(14), p.lifeSpanVariation = at(15);
						p.size = { at(16), at(17), at(18) };
						p.emitterNode = o->refs.size() > 1 ? node(o->refs[1]) : -1;
					}
					else if (type == "NiPSysGrowFadeModifier")
						p.growTime = at(2), p.fadeTime = at(4);
					else if (type == "NiPSysColorModifier")
					{
						if (o->refs.size() > 1)
							if (const auto* c = m_file.get<NiColorData>(o->refs[1]))
								p.colorKeys = c->data;
					}
					else if (type == "NiPSysRotationModifier")
					{
						p.spins = true;
						p.spinSpeed = at(2), p.spinSpeedVariation = at(3), p.spinAngle = at(4), p.spinAngleVariation = at(5);
						p.spinRandomSign = at(6) != 0;
					}
					else if (type == "NiPSysGravityModifier")
					{
						ParticleSystem::Gravity g;
						g.node = o->refs.size() > 1 ? node(o->refs[1]) : -1;
						g.axis = { at(2), at(3), at(4) };
						g.decay = at(5), g.strength = at(6);
						g.point = at(7) == 1;
						p.gravity.push_back(g);
					}
				}
				if (!hasEmitter)
					return; // (a mesh emitter, or none: not drawn)
				// The emitter controller among the system's controllers: how many are born, when.
				for (Ref c = ps.controller; c >= 0;)
				{
					const auto* o = m_file.get<NiPSysObject>(c);
					if (!o)
						break;
					if (m_file.typeOf(c) == "NiPSysEmitterCtlr")
					{
						p.start = o->startTime, p.stop = o->stopTime, p.frequency = o->frequency > 0 ? o->frequency : 1;
						p.loop = ((o->flags >> 1) & 3) != 2; // (cycle: loop, reverse; clamp)
						if (!o->refs.empty())
							if (const auto* fi = m_file.get<NiFloatInterpolator>(o->refs[0]))
							{
								p.birthRate = std::isfinite(fi->value) && std::abs(fi->value) < 1e30f ? fi->value : 0.0f;
								if (const auto* d = m_file.get<NiFloatData>(fi->data))
									p.birthRateKeys = d->data;
							}
						if (o->refs.size() > 1)
							if (const auto* bi = m_file.get<NiBoolInterpolator>(o->refs[1]))
								if (const auto* d = m_file.get<NiBoolData>(bi->data))
									p.activeKeys = d->data;
						// (older files: the rate's and the on/off keys in its data, NiPSysEmitterCtlrData)
						if (o->refs.size() > 1 && o->refs[1] >= 0 && m_file.typeOf(o->refs[1]) == "NiPSysEmitterCtlrData")
							if (const auto* d = m_file.get<NiPSysObject>(o->refs[1]))
							{
								const auto& v = d->params;
								size_t i = 0;
								const auto next = [&] { return i < v.size() ? v[i++] : 0.0f; };
								for (size_t n = size_t(next()); n > 0 && i + 1 < v.size(); --n)
								{
									nif::Key<float> k;
									k.time = next(), k.value = next();
									p.birthRateKeys.keys.push_back(k);
								}
								p.birthRateKeys.interpolation = nif::KeyType::Linear;
								for (size_t n = size_t(next()); n > 0 && i + 1 < v.size(); --n)
								{
									nif::Key<uint8_t> k;
									k.time = next(), k.value = next() != 0;
									p.activeKeys.keys.push_back(k);
								}
							}
					}
					c = o->nextController;
				}
				m_model.particles.push_back(std::move(p));
			}

			// A texture by its name: an external one, or an internal one by the name it has (the
			// leaderheads' are in their NIFs and beside them).
			static bool named(const NiSourceTexture& src) { return !src.fileName.empty(); }

			// A forest piece: its text holds "OFFSETCOUNT = n", and the red of its vertices' colour
			// numbers its trees (that text's "OFFSETi = x,y" lines are not always in that order).
			const NiStringExtraData* treeText(const NiGeometry& geom) const
			{
				for (const Ref e : geom.extraData)
					if (const auto* s = m_file.get<NiStringExtraData>(e); s && s->value.contains("OFFSETCOUNT"))
						return s;
				return nullptr;
			}

			// Where each tree of a forest piece stands: the middle of its vertices (tree i at
			// [i - 1], the first 32; none: not a number).
			static std::vector<Vec2> treePositions(const std::vector<Vertex>& vertices)
			{
				std::array<double, 32> x{}, y{}, n{};
				for (const Vertex& v : vertices)
					if (const uint32_t tree = v.color & 0xFF; tree >= 1 && tree <= 32)
					{
						x[tree - 1] += v.position.x;
						y[tree - 1] += v.position.y;
						n[tree - 1] += 1;
					}
				std::vector<Vec2> trees;
				for (size_t t = 0; t < 32; ++t)
					if (n[t] > 0)
					{
						trees.resize(t + 1, Vec2{ std::nanf(""), std::nanf("") });
						trees[t] = { float(x[t] / n[t]), float(y[t] / n[t]) };
					}
				return trees;
			}

			Material makeMaterial(const NiGeometry& geom, const PropertyState& p, uint32_t& uvSet)
			{
				Material m;
				m.shaderName = geom.shaderName;
				if (p.material)
				{
					m.ambient = p.material->ambient;
					m.diffuse = p.material->diffuse;
					m.emissive = p.material->emissive;
					m.alpha = p.material->alpha;
				}
				// Civ4 mech units (TCiv4Mech* shaders) have no base map: the colour
				// texture sits in the detail slot.
				const size_t baseSlot = p.texturing && !p.texturing->maps[kTexBase] && p.texturing->maps[kTexDetail] ? kTexDetail : kTexBase;
				if (p.texturing && p.texturing->maps[baseSlot])
				{
					const TexDesc& baseDesc = *p.texturing->maps[baseSlot];
					uvSet = baseDesc.uvSet;
					m.baseSlot = baseSlot;
					m.baseUv = toUv(baseDesc);
					m.baseClamp = static_cast<TexClamp>(std::min<uint32_t>(baseDesc.clampMode, 3));
					if (const auto* src = m_file.get<NiSourceTexture>(baseDesc.source); src && named(*src))
					{
						m.baseTexture = src->fileName;
						std::string lower = src->fileName;
						for (char& c : lower)
							c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
						m.baseIsTeamColor = lower.starts_with("teamcolor");
					}
				}
				if (p.texturing && p.texturing->maps[kTexDecal0])
				{
					const TexDesc& decal = *p.texturing->maps[kTexDecal0];
					if (const auto* src = m_file.get<NiSourceTexture>(decal.source); src && named(*src))
					{
						m.decalTexture = src->fileName;
						m.decalUv = toUv(decal);
						m.decalClamp = static_cast<TexClamp>(std::min<uint32_t>(decal.clampMode, 3));
					}
				}
				if (p.texturing && m.shaderName.starts_with("TLeaderheadShader"))
					for (size_t s = 0; s < p.texturing->shaderMaps.size() && s < 4; ++s)
						if (const auto& sm = p.texturing->shaderMaps[s]; sm.map)
							if (const auto* src = m_file.get<NiSourceTexture>(sm.map->source); src && named(*src))
								(s == 0 ? m.envMap : s == 1 ? m.normalMap : s == 2 ? m.specularMap : m.envMask) = src->fileName;
				if (p.alpha)
				{
					m.alphaBlend = p.alpha->blendEnabled();
					m.srcBlend = static_cast<Blend>(std::min<uint32_t>(p.alpha->srcBlend(), 10));
					m.dstBlend = static_cast<Blend>(std::min<uint32_t>(p.alpha->dstBlend(), 10));
					m.alphaTest = p.alpha->testEnabled();
					m.alphaFunc = static_cast<Compare>(p.alpha->testFunc());
					m.alphaRef = p.alpha->threshold;
				}
				// A forest piece numbers each tree's vertices in the red of their colour (green and
				// blue 1): no colour, the game's trees keep the texture's own.
				if (p.vertexColor && !treeText(geom))
				{
					m.vertexColorMode = p.vertexColor->vertexMode;
					m.lightingMode = p.vertexColor->lightingMode;
				}
				if (p.zbuffer)
				{
					m.zTest = p.zbuffer->testEnabled();
					m.zWrite = p.zbuffer->writeEnabled();
				}
				// An additive layer (One, One: the rivers' rapids, glows) only adds light, in any
				// order: its depth would hide what is drawn under it after it (a river's water, the
				// sea at its mouth), as the blended meshes go by their distance from the eye.
				if (m.alphaBlend && m.srcBlend == Blend::One && m.dstBlend == Blend::One)
					m.zWrite = false;
				if (p.stencil)
					m.cull = p.stencil->drawMode == 3 ? Cull::None : p.stencil->drawMode == 2 ? Cull::Front : Cull::Back;
				// The leaderheads' skin shaders (TLeaderheadShader*) draw it opaque: the alpha of
				// their colour map is a mask of their own, not where the skin is.
				// Their see-through parts (brows, moustaches: "..._trans") keep their alpha. The
				// shader sets the depth state itself: what it draws hides what is behind it (Pacal's
				// face has the property's "no depth writes", and drawn so its lips and teeth showed
				// through it).
				if (m.shaderName.starts_with("TLeaderheadShader"))
				{
					std::string name = geom.name;
					for (char& c : name)
						c = char(std::tolower(static_cast<unsigned char>(c)));
					if (name.find("trans") == std::string::npos)
					{
						m.alphaBlend = false;
						m.alphaTest = false;
					}
					m.zTest = m.zWrite = true;
				}
				return m;
			}

			// Keeps the four strongest influences per vertex, renormalised to 255.
			static void packWeights(Vertex& v, std::span<const std::pair<float, uint8_t>> influences)
			{
				std::array<std::pair<float, uint8_t>, 4> top{};
				for (const auto& inf : influences)
				{
					auto weakest = std::ranges::min_element(top, {}, &std::pair<float, uint8_t>::first);
					if (inf.first > weakest->first)
						*weakest = inf;
				}
				float sum = 0;
				for (const auto& t : top)
					sum += t.first;
				if (sum <= 0)
					return;
				int total = 0;
				for (int i = 0; i < 4; ++i)
				{
					v.bones[i] = top[i].second;
					v.weights[i] = static_cast<uint8_t>(std::lround(top[i].first / sum * 255.0f));
					total += v.weights[i];
				}
				// Put rounding error on the strongest influence so weights sum to 255.
				auto strongest = std::max_element(v.weights, v.weights + 4);
				*strongest = static_cast<uint8_t>(*strongest + (255 - total));
			}

			void addGeometry(Ref ref, const NiGeometry& geom, const PropertyState& props)
			{
				const auto* data = m_file.get<NiTriBasedGeomData>(geom.data);
				if (!data || data->vertices.empty())
					return;

				DrawItem item;
				item.nodeName = geom.name;
				item.node = m_nodeOf.at(ref);
				uint32_t uvSet = 0;
				item.material = makeMaterial(geom, props, uvSet);
				item.materialOwner = props.materialOwner;
				item.texturingOwner = props.texturingOwner;

				const size_t n = data->vertices.size();
				item.vertices.resize(n);
				const std::vector<Vec2>* uvs = data->uvSets.empty() ? nullptr : &data->uvSets[std::min<size_t>(uvSet, data->uvSets.size() - 1)];
				for (size_t i = 0; i < n; ++i)
				{
					Vertex& v = item.vertices[i];
					v.position = data->vertices[i];
					v.normal = data->normals.empty() ? Vec3{ 0, 0, 1 } : normalize(data->normals[i]);
					if (uvs)
						v.uv = (*uvs)[i]; // map transforms are applied at draw time (they animate)
					if (!data->colors.empty())
						v.color = packColor(data->colors[i]);
				}

				const auto* skin = m_file.get<NiSkinInstance>(geom.skinInstance);
				const auto* skinData = skin ? m_file.get<NiSkinData>(skin->data) : nullptr;
				if (skin && skinData && skinData->bones.size() == skin->bones.size() && skin->bones.size() <= 255)
				{
					std::vector<std::vector<std::pair<float, uint8_t>>> influences(n);
					for (size_t b = 0; b < skin->bones.size(); ++b)
					{
						const auto it = m_nodeOf.find(skin->bones[b]);
						item.bones.push_back(it == m_nodeOf.end() ? item.node : it->second);
						item.skinToBone.push_back(toAffine(skinData->bones[b].skinTransform));
						for (const auto& w : skinData->bones[b].weights)
							if (w.vertex < n && w.weight > 0)
								influences[w.vertex].push_back({ w.weight, uint8_t(b) });
					}
					item.skinned = true;
					for (size_t i = 0; i < n; ++i)
					{
						if (influences[i].empty())
							influences[i].push_back({ 1.0f, uint8_t(0) }); // unweighted: follow the first bone
						packWeights(item.vertices[i], influences[i]);
					}
				}

				for (const Triangle& t : data->triangleList())
					if (t[0] < n && t[1] < n && t[2] < n)
						item.indices.insert(item.indices.end(), { t[0], t[1], t[2] });
				if (item.indices.empty())
					return;

				// Bind-pose bounds for framing the camera.
				for (const Vertex& v : item.vertices)
				{
					if (!item.skinned)
					{
						item.bindBounds.add(m_bindWorld[size_t(item.node)].point(v.position));
						continue;
					}
					Vec3 p;
					for (int i = 0; i < 4; ++i)
						if (v.weights[i])
						{
							const Affine m = m_bindWorld[size_t(item.bones[v.bones[i]])] * item.skinToBone[v.bones[i]];
							p += m.point(v.position) * (v.weights[i] / 255.0f);
						}
					item.bindBounds.add(p);
				}
				if (treeText(geom))
					item.trees = treePositions(item.vertices);
				m_model.items.push_back(std::move(item));
			}

			const File& m_file;
			Model m_model;
			std::unordered_map<Ref, int> m_nodeOf;
			std::vector<Affine> m_bindWorld;
		};
	}

	Model buildModel(const nif::File& file)
	{
		Model m = Builder(file).run();
		if (!file.complete())
			m.warnings.push_back(std::format("partial file: {}", *file.error));
		return m;
	}
}
