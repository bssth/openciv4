// nifdump: prints the block list and scene graph of a NIF with geometry statistics.
//   nifdump --civ4 <Civ4 root> <asset key>
//   nifdump <file.nif>

#include "core/AssetResolver.h"
#include "core/FileUtil.h"
#include "core/Math.h"
#include "nif/NifBlocks.h"
#include "nif/NifFile.h"

#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <utility>

namespace fs = std::filesystem;
using namespace oc4;
using namespace oc4::nif;

namespace
{
	std::string vec(Vec3 v) { return std::format("({:.3f}, {:.3f}, {:.3f})", v.x, v.y, v.z); }

	void printTree(const File& f, Ref ref, int depth, std::set<Ref>& seen)
	{
		const auto* obj = f.get<NiAVObject>(ref);
		if (!obj || !seen.insert(ref).second)
			return;
		const std::string indent(size_t(depth) * 2, ' ');
		std::cout << std::format("{}[{}] {} '{}' flags=0x{:X} t={} s={:.3f}", indent, ref, f.typeOf(ref), obj->name, obj->flags,
			vec(obj->translation), obj->scale);
		const auto& m = obj->rotation.m;
		const bool identity = m[0][0] == 1 && m[1][1] == 1 && m[2][2] == 1;
		if (!identity)
			std::cout << std::format(" R=[{:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f} | {:.2f} {:.2f} {:.2f}]",
				m[0][0], m[0][1], m[0][2], m[1][0], m[1][1], m[1][2], m[2][0], m[2][1], m[2][2]);
		std::cout << '\n';

		if (const auto* nn = dynamic_cast<const NiNode*>(obj); nn && !nn->effects.empty())
		{
			std::cout << indent << "    effects:";
			for (const Ref e : nn->effects)
				std::cout << ' ' << e;
			std::cout << '\n';
		}
		if (const auto* l = dynamic_cast<const NiLight*>(obj))
		{
			std::cout << std::format("{}    light: on={} dimmer={:.2f} amb=({:.2f},{:.2f},{:.2f}) diff=({:.2f},{:.2f},{:.2f}) spec=({:.2f},{:.2f},{:.2f}) atten={:.3f},{:.5f},{:.7f} affects:", indent, l->switchState, l->dimmer,
				l->ambient.r, l->ambient.g, l->ambient.b, l->diffuse.r, l->diffuse.g, l->diffuse.b, l->specular.r, l->specular.g, l->specular.b, l->constantAttenuation, l->linearAttenuation, l->quadraticAttenuation);
			for (const Ref a : l->affectedNodes)
				std::cout << ' ' << a;
			std::cout << '\n';
		}
		if (const auto* g = dynamic_cast<const NiGeometry*>(obj))
		{
			if (const auto* d = f.get<NiTriBasedGeomData>(g->data))
			{
				Bounds b;
				for (const auto& v : d->vertices)
					b.add(v);
				const auto tris = d->triangleList();
				uint32_t maxIndex = 0;
				for (const auto& t : tris)
					maxIndex = std::max<uint32_t>({ maxIndex, t[0], t[1], t[2] });
				std::cout << std::format("{}    data [{}] {}: verts={} normals={} uvSets={} colors={} tris={} maxIndex={} flags=0x{:X}\n",
					indent, g->data, f.typeOf(g->data), d->vertices.size(), d->normals.size(), d->uvSets.size(), d->colors.size(),
					tris.size(), maxIndex, d->dataFlags);
				std::cout << std::format("{}    bounds min={} max={} center={} radius={:.3f}\n", indent, vec(b.min), vec(b.max), vec(d->center), d->radius);
				if (!d->vertices.empty())
					std::cout << std::format("{}    v0={} v1={}\n", indent, vec(d->vertices[0]), vec(d->vertices[std::min<size_t>(1, d->vertices.size() - 1)]));
			}
			if (g->hasShader)
				std::cout << std::format("{}    shader '{}' extra={}\n", indent, g->shaderName, g->shaderExtraData);
			if (const auto* skin = f.get<NiSkinInstance>(g->skinInstance))
			{
				std::cout << std::format("{}    skin: root={} bones={} partition={}\n", indent, skin->skeletonRoot, skin->bones.size(), skin->skinPartition);
				if (const auto* sd = f.get<NiSkinData>(skin->data))
					std::cout << std::format("{}    skinData: rootParentToSkin t={} s={:.3f}, bone0 t={} weights0={}\n", indent,
						vec(sd->skinTransform.translation), sd->skinTransform.scale,
						sd->bones.empty() ? "-" : vec(sd->bones[0].skinTransform.translation),
						sd->bones.empty() ? 0 : sd->bones[0].weights.size());
			}
			for (const Ref p : obj->properties)
				if (const auto* mp = f.get<NiMaterialProperty>(p))
					std::cout << std::format("{}    material [{}]: amb=({:.2f},{:.2f},{:.2f}) diff=({:.2f},{:.2f},{:.2f}) emis=({:.2f},{:.2f},{:.2f}) spec=({:.2f},{:.2f},{:.2f}) gloss={:.1f} alpha={:.2f}\n", indent, p,
						mp->ambient.r, mp->ambient.g, mp->ambient.b, mp->diffuse.r, mp->diffuse.g, mp->diffuse.b, mp->emissive.r, mp->emissive.g, mp->emissive.b,
						mp->specular.r, mp->specular.g, mp->specular.b, mp->glossiness, mp->alpha);
				else if (const auto* vp = f.get<NiVertexColorProperty>(p))
					std::cout << std::format("{}    vertexColor [{}]: mode={} lighting={}\n", indent, p, vp->vertexMode, vp->lightingMode);
				else if (const auto* tp = f.get<NiTexturingProperty>(p))
					std::cout << std::format("{}    texturing [{}]: apply={}\n", indent, p, tp->applyMode);
				else if (const auto* zp = f.get<NiZBufferProperty>(p))
					std::cout << std::format("{}    zbuffer [{}]: flags={}\n", indent, p, zp->flags);
			for (const Ref p : obj->properties)
				if (const auto* ap = f.get<NiAlphaProperty>(p))
					std::cout << std::format("{}    alpha [{}]: flags=0x{:04X} blend={} src={} dst={} test={} func={} threshold={} noSorter={}\n", indent, p, ap->flags,
						ap->blendEnabled(), ap->srcBlend(), ap->dstBlend(), ap->testEnabled(), ap->testFunc(), ap->threshold, ap->noSorter());
			for (const Ref p : obj->properties)
				if (const auto* tp = f.get<NiTexturingProperty>(p))
					for (size_t s = 0; s < tp->shaderMaps.size(); ++s)
						if (const auto& sm = tp->shaderMaps[s]; sm.map)
							if (const auto* src = f.get<NiSourceTexture>(sm.map->source))
								std::cout << std::format("{}    shader map {} (id {}): '{}'\n", indent, s, sm.mapId, src->fileName);
			for (const Ref p : obj->properties)
				if (const auto* tp = f.get<NiTexturingProperty>(p))
					for (size_t s = 0; s < kTexSlotCount; ++s)
						if (tp->maps[s])
							if (const auto* src = f.get<NiSourceTexture>(tp->maps[s]->source))
							{
								const TexDesc& td = *tp->maps[s];
								std::cout << std::format("{}    texture slot {}: '{}' uvSet={} clamp={} filter={}", indent, s, src->fileName, td.uvSet, td.clampMode, td.filterMode);
								if (td.hasTransform)
									std::cout << std::format(" xform: t=({:.3f},{:.3f}) tiling=({:.3f},{:.3f}) rot={:.3f} method={} center=({:.3f},{:.3f})",
										td.translation.x, td.translation.y, td.tiling.x, td.tiling.y, td.rotation, td.transformMethod, td.center.x, td.center.y);
								std::cout << '\n';
							}
		}
		if (const auto* n = dynamic_cast<const NiNode*>(obj))
			for (const Ref c : n->children)
				printTree(f, c, depth + 1, seen);
	}
}

int wmain(int argc, wchar_t** argv)
{
	std::optional<AssetResolver> resolver;
	std::wstring target;
	for (int i = 1; i < argc; ++i)
	{
		const std::wstring a = argv[i];
		if (a == L"--civ4" && i + 1 < argc)
			resolver = AssetResolver::forInstall(argv[++i]);
		else
			target = a;
	}
	if (target.empty())
	{
		std::cerr << "usage: nifdump [--civ4 <root>] <asset key | file>\n";
		return 2;
	}
	try
	{
		std::vector<std::byte> data;
		if (fs::is_regular_file(target))
			data = readWholeFile(target);
		else if (resolver)
		{
			const auto loc = resolver->locate(fs::path(target).string());
			if (!loc)
				throw std::runtime_error("asset not found");
			data = resolver->read(*loc);
		}
		else
			throw std::runtime_error("file not found");

		const File f = parseFile(data);
		std::cout << std::format("{}  blocks={} roots={}{}\n", f.header.versionString, f.header.numBlocks, f.roots.size(),
			f.complete() ? "" : "  ERROR: " + *f.error);
		for (size_t i = 0; i < f.blocks.size(); ++i)
			std::cout << std::format("  {:>4} {}\n", i, f.header.blockTypeName(i));
		std::cout << "\nscene graph:\n";
		std::set<Ref> seen;
		for (const Ref r : f.roots)
			printTree(f, r, 0, seen);

		// The objects' text extra data (the exporter's user properties).
		bool header = false;
		for (size_t i = 0; i < f.blocks.size(); ++i)
			if (const auto* o = f.get<NiObjectNET>(Ref(i)))
				for (const Ref e : o->extraData)
					if (const auto* s = f.get<NiStringExtraData>(e))
					{
						if (!std::exchange(header, true))
							std::cout << "\nstring extra data:\n";
						std::cout << std::format("  [{}] '{}': [{}] '{}'\n", i, o->name, e, s->value);
					}

		// Time controllers: flags (bit 3 active, bit 5 manager controlled), time line, target.
		header = false;
		for (size_t i = 0; i < f.blocks.size(); ++i)
			if (const auto* c = f.get<NiTimeController>(Ref(i)))
			{
				if (!std::exchange(header, true))
					std::cout << "\ncontrollers:\n";
				const auto* targetObject = f.get<NiObjectNET>(c->target);
				std::cout << std::format("  [{}] {} flags=0x{:X} {:.3f}..{:.3f} freq={:.2f} target=[{}] '{}'", i, f.typeOf(Ref(i)), c->flags, c->startTime,
					c->stopTime, c->frequency, c->target, targetObject ? targetObject->name : std::string());
				if (const auto* s = dynamic_cast<const NiSingleInterpController*>(c))
					std::cout << std::format(" interp={}", s->interpolator >= 0 ? f.typeOf(s->interpolator) : "-");
				std::cout << '\n';
			}

		// Controller sequences (KF files): channel targets and key ranges.
		for (const Ref r : f.roots)
			if (const auto* seq = f.get<NiControllerSequence>(r))
			{
				std::cout << std::format("\nsequence '{}' {:.3f}..{:.3f} cycle={} freq={:.2f} accum='{}'\n", seq->name, seq->startTime, seq->stopTime,
					seq->cycleType, seq->frequency, seq->accumRootName);
				if (const auto* tk = f.get<NiTextKeyExtraData>(seq->textKeys))
					for (const auto& [t, text] : tk->keys)
						std::cout << std::format("  text key {:.3f} '{}'\n", t, text);
				for (const auto& b : seq->controlledBlocks)
				{
					const auto* pal = f.get<NiStringPalette>(b.stringPalette != kNoRef ? b.stringPalette : seq->stringPalette);
					const auto str = [&](uint32_t off) { return pal ? std::string(pal->at(off)) : std::string(); };
					std::cout << std::format("  [{}] node='{}' ctrl={} prop={} cid='{}' interp={}", b.interpolator, b.targetName.empty() ? str(b.nodeNameOffset) : b.targetName,
						str(b.controllerTypeOffset), str(b.propertyTypeOffset), str(b.controllerIdOffset), b.interpolator >= 0 ? f.typeOf(b.interpolator) : "-");
					if (const auto* fi = f.get<NiFloatInterpolator>(b.interpolator))
					{
						std::cout << std::format(" value={:.3f}", fi->value);
						if (const auto* d = f.get<NiFloatData>(fi->data); d && !d->data.keys.empty())
						{
							std::cout << std::format(" keys={} type={}:", d->data.keys.size(), int(d->data.interpolation));
							for (size_t k = 0; k < d->data.keys.size() && k < 6; ++k)
								std::cout << std::format(" {:.2f}@{:.2f}", d->data.keys[k].value, d->data.keys[k].time);
						}
					}
					else if (const auto* pi = f.get<NiPoint3Interpolator>(b.interpolator))
					{
						std::cout << std::format(" value={}", vec(pi->value));
						if (const auto* d = f.get<NiPosData>(pi->data); d && !d->data.keys.empty())
							std::cout << std::format(" keys={} first={} last={}", d->data.keys.size(), vec(d->data.keys.front().value), vec(d->data.keys.back().value));
					}
					std::cout << '\n';
				}
			}
	}
	catch (const std::exception& e)
	{
		std::cerr << "nifdump: " << e.what() << '\n';
		return 1;
	}
	return 0;
}
