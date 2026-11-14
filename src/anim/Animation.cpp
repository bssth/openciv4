#include "anim/Animation.h"

#include "nif/NifBlocks.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <format>

namespace oc4::anim
{
	using namespace nif;

	namespace
	{
		// Gamebryo marks unused interpolator values with -FLT_MAX.
		bool valid(float v) { return std::isfinite(v) && v > -1e30f; }
		bool valid(Vec3 v) { return valid(v.x) && valid(v.y) && valid(v.z); }
		bool valid(const Quat& q) { return valid(q.w) && valid(q.x) && valid(q.y) && valid(q.z); }

		// Index i so that keys[i].time <= t < keys[i+1].time.
		template <class K>
		size_t findInterval(const std::vector<K>& keys, float t)
		{
			const auto it = std::upper_bound(keys.begin(), keys.end(), t, [](float v, const K& k) { return v < k.time; });
			return it == keys.begin() ? 0 : size_t(it - keys.begin()) - 1;
		}

		// Kochanek–Bartels tangents for key i (incoming and outgoing).
		template <class T>
		void tcbTangents(const std::vector<Key<T>>& keys, size_t i, T& in, T& out)
		{
			const T p = keys[i].value;
			const T prev = i > 0 ? keys[i - 1].value : p;
			const T next = i + 1 < keys.size() ? keys[i + 1].value : p;
			const float tension = keys[i].tbc[0], continuity = keys[i].tbc[1], bias = keys[i].tbc[2];
			const T d0 = p - prev, d1 = next - p;
			out = d0 * ((1 - tension) * (1 + continuity) * (1 + bias) * 0.5f) + d1 * ((1 - tension) * (1 - continuity) * (1 - bias) * 0.5f);
			in = d0 * ((1 - tension) * (1 - continuity) * (1 + bias) * 0.5f) + d1 * ((1 - tension) * (1 + continuity) * (1 - bias) * 0.5f);
		}

		template <class T>
		std::optional<T> sampleGroup(const KeyGroup<T>& g, float t)
		{
			const auto& keys = g.keys;
			if (keys.empty())
				return std::nullopt;
			if (keys.size() == 1 || t <= keys.front().time)
				return keys.front().value;
			if (t >= keys.back().time)
				return keys.back().value;
			const size_t i = findInterval(keys, t);
			const Key<T>& a = keys[i];
			const Key<T>& b = keys[i + 1];
			const float span = b.time - a.time;
			const float s = span > 0 ? (t - a.time) / span : 0;
			switch (g.interpolation)
			{
			case KeyType::Const:
				return a.value;
			case KeyType::Quadratic:
			case KeyType::TBC:
			{
				T outA, inB;
				if (g.interpolation == KeyType::Quadratic)
				{
					// Leave a along its outgoing tangent, arrive at b along its incoming one.
					outA = a.outTangent;
					inB = b.inTangent;
				}
				else
				{
					T dummy;
					tcbTangents(keys, i, dummy, outA);
					tcbTangents(keys, i + 1, inB, dummy);
				}
				const float s2 = s * s, s3 = s2 * s;
				return a.value * (2 * s3 - 3 * s2 + 1) + b.value * (-2 * s3 + 3 * s2) + outA * (s3 - 2 * s2 + s) + inB * (s3 - s2);
			}
			default:
				return a.value + (b.value - a.value) * s;
			}
		}

		std::optional<Quat> sampleRotation(const NiTransformData& d, float t)
		{
			if (d.rotationType == KeyType::XyzRotation)
			{
				const auto x = sampleGroup(d.xyzRotations[0], t).value_or(0.0f);
				const auto y = sampleGroup(d.xyzRotations[1], t).value_or(0.0f);
				const auto z = sampleGroup(d.xyzRotations[2], t).value_or(0.0f);
				return normalize(axisAngle({ 0, 0, 1 }, z) * axisAngle({ 0, 1, 0 }, y) * axisAngle({ 1, 0, 0 }, x));
			}
			const auto& keys = d.quaternionKeys;
			if (keys.empty())
				return std::nullopt;
			if (keys.size() == 1 || t <= keys.front().time)
				return keys.front().value;
			if (t >= keys.back().time)
				return keys.back().value;
			const size_t i = findInterval(keys, t);
			const float span = keys[i + 1].time - keys[i].time;
			const float s = span > 0 ? (t - keys[i].time) / span : 0;
			if (d.rotationType == KeyType::Const)
				return keys[i].value;
			// Bezier/TCB quaternion keys are approximated by slerp for now.
			return slerp(keys[i].value, keys[i + 1].value, s);
		}

		// Compact (16-bit) B-spline control points: value = bias + multiplier * (s / 32767).
		struct CompactSpline
		{
			std::span<const int16_t> data;
			size_t stride;
			float bias, multiplier;

			float at(size_t cp, size_t comp) const { return bias + multiplier * (float(data[cp * stride + comp]) / 32767.0f); }
		};

		// Open uniform cubic B-spline basis: knots 0,0,0,0,1,..,n-3,n-3,n-3,n-3.
		void bsplineBasis(size_t numCP, float u, size_t& first, std::array<float, 4>& w)
		{
			const int n = int(numCP);
			constexpr int degree = 3;
			const auto knot = [&](int j) { return float(std::clamp(j - degree, 0, n - degree)); };
			u = std::clamp(u, 0.0f, float(n - degree));
			const int span = std::min(int(u), n - degree - 1) + degree;
			// Cox-de Boor, triangular scheme (The NURBS Book, A2.2).
			std::array<float, 4> N{ 1, 0, 0, 0 };
			std::array<float, 4> left{}, right{};
			for (int j = 1; j <= degree; ++j)
			{
				left[size_t(j)] = u - knot(span + 1 - j);
				right[size_t(j)] = knot(span + j) - u;
				float saved = 0;
				for (int r = 0; r < j; ++r)
				{
					const float denom = right[size_t(r + 1)] + left[size_t(j - r)];
					const float temp = denom != 0 ? N[size_t(r)] / denom : 0;
					N[size_t(r)] = saved + right[size_t(r + 1)] * temp;
					saved = left[size_t(j - r)] * temp;
				}
				N[size_t(j)] = saved;
			}
			first = size_t(span - degree);
			w = N;
		}

		template <size_t Dim>
		std::array<float, Dim> evalCompact(const CompactSpline& s, size_t numCP, float u01)
		{
			std::array<float, Dim> out{};
			if (numCP < 4)
			{
				for (size_t c = 0; c < Dim; ++c)
					out[c] = numCP ? s.at(0, c) : 0;
				return out;
			}
			size_t first = 0;
			std::array<float, 4> w{};
			bsplineBasis(numCP, u01 * float(numCP - 3), first, w);
			for (size_t k = 0; k < 4; ++k)
				for (size_t c = 0; c < Dim; ++c)
					out[c] += w[k] * s.at(first + k, c);
			return out;
		}

		// Evaluates a compact spline channel of a B-spline interpolator, if present.
		template <size_t Dim>
		std::optional<std::array<float, Dim>> sampleSpline(const File& f, const NiBSplineInterpolator& b, uint32_t handle, float bias, float multiplier, float t)
		{
			const auto* data = f.get<NiBSplineData>(b.splineData);
			const auto* basis = f.get<NiBSplineBasisData>(b.basisData);
			if (!data || !basis || handle == 0xFFFF)
				return std::nullopt;
			const size_t numCP = basis->numControlPoints;
			const auto compact = std::span<const int16_t>(data->compactControlPoints);
			if (size_t(handle) + numCP * Dim > compact.size())
				return std::nullopt;
			const float span = b.stopTime - b.startTime;
			const float u01 = span > 0 ? std::clamp((t - b.startTime) / span, 0.0f, 1.0f) : 0.0f;
			return evalCompact<Dim>({ compact.subspan(handle), Dim, bias, multiplier }, numCP, u01);
		}

		TRS sampleBSplineTransform(const File& f, const NiBSplineCompTransformInterpolator& b, float t)
		{
			TRS out;
			if (valid(b.translation))
				out.translation = b.translation;
			if (valid(b.rotation))
				out.rotation = b.rotation;
			if (valid(b.scale))
				out.scale = b.scale;
			if (const auto v = sampleSpline<3>(f, b, b.translationHandle, b.translationBias, b.translationMultiplier, t))
				out.translation = Vec3{ (*v)[0], (*v)[1], (*v)[2] };
			if (const auto v = sampleSpline<4>(f, b, b.rotationHandle, b.rotationBias, b.rotationMultiplier, t))
				out.rotation = normalize(Quat{ (*v)[0], (*v)[1], (*v)[2], (*v)[3] });
			if (const auto v = sampleSpline<1>(f, b, b.scaleHandle, b.scaleBias, b.scaleMultiplier, t))
				out.scale = (*v)[0];
			return out;
		}

		TRS sampleTransformData(const NiTransformData& d, float t, TRS out)
		{
			if (auto v = sampleGroup(d.translations, t))
				out.translation = *v;
			if (auto q = sampleRotation(d, t))
				out.rotation = *q;
			if (auto s = sampleGroup(d.scales, t))
				out.scale = *s;
			return out;
		}

		bool isTransformInterpolator(const NiObject* o)
		{
			return dynamic_cast<const NiTransformInterpolator*>(o) || dynamic_cast<const NiBSplineCompTransformInterpolator*>(o);
		}

		bool isFloatInterpolator(const NiObject* o)
		{
			return dynamic_cast<const NiFloatInterpolator*>(o) || dynamic_cast<const NiBSplineCompFloatInterpolator*>(o);
		}

		bool isPoint3Interpolator(const NiObject* o)
		{
			return dynamic_cast<const NiPoint3Interpolator*>(o) || dynamic_cast<const NiBSplineCompPoint3Interpolator*>(o);
		}

		// "0-6-TT_TRANSLATE_U" -> shader map flag, slot 6, op.
		std::optional<std::pair<uint8_t, UvOp>> parseTextureTransformId(std::string_view id)
		{
			const size_t a = id.find('-');
			const size_t b = a == std::string_view::npos ? a : id.find('-', a + 1);
			if (b == std::string_view::npos || id.substr(0, a) != "0")
				return std::nullopt; // shader maps are not handled
			int slot = -1;
			std::from_chars(id.data() + a + 1, id.data() + b, slot);
			if (slot < 0 || slot >= 10)
				return std::nullopt;
			const std::string_view op = id.substr(b + 1);
			static constexpr std::pair<std::string_view, UvOp> kOps[] = {
				{ "TT_TRANSLATE_U", UvOp::TranslateU }, { "TT_TRANSLATE_V", UvOp::TranslateV }, { "TT_ROTATE", UvOp::Rotate },
				{ "TT_SCALE_U", UvOp::ScaleU }, { "TT_SCALE_V", UvOp::ScaleV },
			};
			for (const auto& [name, value] : kOps)
				if (op == name)
					return std::pair{ uint8_t(slot), value };
			return std::nullopt;
		}

		void applyUv(scene::UvTransform& t, const std::array<float, size_t(UvOp::Count)>& ov)
		{
			const auto set = [&](UvOp op, float& dst) {
				if (isSet(ov[size_t(op)]))
				{
					dst = ov[size_t(op)];
					t.enabled = true;
				}
			};
			set(UvOp::TranslateU, t.translation.x);
			set(UvOp::TranslateV, t.translation.y);
			set(UvOp::Rotate, t.rotation);
			set(UvOp::ScaleU, t.tiling.x);
			set(UvOp::ScaleV, t.tiling.y);
		}
	}

	// ------------------------------------------------------------------ sampling

	TRS sampleTransformInterpolator(const File& f, const NiObject& interpolator, float t)
	{
		if (const auto* ti = dynamic_cast<const NiTransformInterpolator*>(&interpolator))
		{
			TRS base;
			if (valid(ti->translation))
				base.translation = ti->translation;
			if (valid(ti->rotation))
				base.rotation = ti->rotation;
			if (valid(ti->scale))
				base.scale = ti->scale;
			if (const auto* d = f.get<NiTransformData>(ti->data))
				return sampleTransformData(*d, t, base);
			return base;
		}
		if (const auto* bi = dynamic_cast<const NiBSplineCompTransformInterpolator*>(&interpolator))
			return sampleBSplineTransform(f, *bi, t);
		return {};
	}

	std::optional<float> sampleFloatInterpolator(const File& f, const NiObject& interpolator, float t)
	{
		if (const auto* fi = dynamic_cast<const NiFloatInterpolator*>(&interpolator))
		{
			if (const auto* d = f.get<NiFloatData>(fi->data))
				if (auto v = sampleGroup(d->data, t))
					return v;
			return valid(fi->value) ? std::optional(fi->value) : std::nullopt;
		}
		if (const auto* bi = dynamic_cast<const NiBSplineCompFloatInterpolator*>(&interpolator))
		{
			if (const auto v = sampleSpline<1>(f, *bi, bi->handle, bi->offset, bi->halfRange, t))
				return (*v)[0];
			return valid(bi->value) ? std::optional(bi->value) : std::nullopt;
		}
		return std::nullopt;
	}

	std::optional<Vec3> samplePoint3Interpolator(const File& f, const NiObject& interpolator, float t)
	{
		if (const auto* pi = dynamic_cast<const NiPoint3Interpolator*>(&interpolator))
		{
			if (const auto* d = f.get<NiPosData>(pi->data))
				if (auto v = sampleGroup(d->data, t))
					return v;
			return valid(pi->value) ? std::optional(pi->value) : std::nullopt;
		}
		if (const auto* bi = dynamic_cast<const NiBSplineCompPoint3Interpolator*>(&interpolator))
		{
			if (const auto v = sampleSpline<3>(f, *bi, bi->handle, bi->offset, bi->halfRange, t))
				return Vec3{ (*v)[0], (*v)[1], (*v)[2] };
			return valid(bi->value) ? std::optional(bi->value) : std::nullopt;
		}
		return std::nullopt;
	}

	// ------------------------------------------------------------------ pose

	Pose Pose::bind(const scene::Model& model)
	{
		return { model.bindLocals(), model.bindHidden(), std::vector<MaterialOverride>(model.nodes.size()) };
	}

	Pose blend(const Pose& a, const Pose& b, float w)
	{
		if (w <= 0)
			return a;
		if (w >= 1)
			return b;
		Pose out = w < 0.5f ? a : b;
		for (size_t i = 0; i < out.locals.size() && i < a.locals.size() && i < b.locals.size(); ++i)
			out.locals[i] = blend(a.locals[i], b.locals[i], w);
		for (size_t i = 0; i < out.materials.size(); ++i)
		{
			const float aa = a.materials[i].alpha, ba = b.materials[i].alpha;
			if (isSet(aa) || isSet(ba))
			{
				out.materials[i].alpha = (isSet(aa) ? aa : 1.0f) + ((isSet(ba) ? ba : 1.0f) - (isSet(aa) ? aa : 1.0f)) * w;
				out.materials[i].any = true;
			}
		}
		return out;
	}

	scene::Material animatedMaterial(const scene::DrawItem& item, const Pose& pose)
	{
		scene::Material m = item.material;
		if (item.materialOwner >= 0 && size_t(item.materialOwner) < pose.materials.size())
		{
			const MaterialOverride& o = pose.materials[size_t(item.materialOwner)];
			if (o.any)
			{
				if (isSet(o.alpha))
				{
					m.alpha = o.alpha;
					m.alphaAnimated = true;
				}
				const auto color = [](const std::optional<Vec3>& v, nif::Color3& dst) {
					if (v)
						dst = { v->x, v->y, v->z };
				};
				color(o.emissive, m.emissive);
				color(o.diffuse, m.diffuse);
				color(o.ambient, m.ambient);
				// Specular is animated by some models but the renderer has no specular term yet.
			}
		}
		if (item.texturingOwner >= 0 && size_t(item.texturingOwner) < pose.materials.size())
		{
			const MaterialOverride& o = pose.materials[size_t(item.texturingOwner)];
			if (o.any)
			{
				applyUv(m.baseUv, o.uv[m.baseSlot]);
				applyUv(m.decalUv, o.uv[kTexDecal0]);
			}
		}
		return m;
	}

	// ------------------------------------------------------------------ sequence

	Sequence Sequence::bind(std::shared_ptr<const File> kf, size_t sequenceIndex, const scene::Model& model)
	{
		Sequence s;
		s.m_file = kf;
		const File& f = *kf;

		// KFM indices count NiControllerSequence roots.
		const NiControllerSequence* seq = nullptr;
		size_t found = 0;
		for (const Ref root : f.roots)
			if (const auto* cs = f.get<NiControllerSequence>(root))
			{
				if (found == sequenceIndex)
				{
					seq = cs;
					break;
				}
				++found;
			}
		if (!seq)
			return s;

		s.m_name = seq->name;
		s.m_startTime = seq->startTime;
		s.m_stopTime = seq->stopTime;
		s.m_frequency = seq->frequency > 0 ? seq->frequency : 1.0f;
		s.m_cycle = static_cast<CycleType>(std::min<uint32_t>(seq->cycleType, 2));
		if (const auto* tk = f.get<NiTextKeyExtraData>(seq->textKeys))
			s.m_textKeys = tk->keys;

		for (const auto& block : seq->controlledBlocks)
		{
			std::string nodeName = block.targetName;
			std::string controllerType, propertyType, controllerId;
			if (const auto* palette = f.get<NiStringPalette>(block.stringPalette != kNoRef ? block.stringPalette : seq->stringPalette))
			{
				if (nodeName.empty())
					nodeName = std::string(palette->at(block.nodeNameOffset));
				controllerType = std::string(palette->at(block.controllerTypeOffset));
				propertyType = std::string(palette->at(block.propertyTypeOffset));
				controllerId = std::string(palette->at(block.controllerIdOffset));
			}
			const int node = model.findNode(nodeName);
			const auto* interp = f.get<NiObject>(block.interpolator);
			const auto* ctrl = f.get<NiSingleInterpController>(block.controller);
			const std::string legacyType = block.controller != kNoRef ? f.typeOf(block.controller) : std::string();

			const auto bound = [&]() {
				if (node >= 0)
					return true;
				++s.m_unbound;
				s.m_unboundNodes.push_back(std::format("{} ({})", nodeName, controllerType));
				return false;
			};

			if (interp && isTransformInterpolator(interp))
			{
				if (bound())
					s.m_transforms.push_back({ node, interp, nullptr });
			}
			else if (ctrl && f.get<NiTransformData>(ctrl->data))
			{
				if (bound()) // 10.1 keyframe controllers
					s.m_transforms.push_back({ node, nullptr, f.get<NiTransformData>(ctrl->data) });
			}
			else if (interp && dynamic_cast<const NiBoolInterpolator*>(interp) && (controllerType.empty() || controllerType == "NiVisController"))
			{
				if (bound())
					s.m_visibility.push_back({ node, interp, nullptr });
			}
			else if (ctrl && legacyType == "NiVisController" && f.get<NiBoolData>(ctrl->data))
			{
				if (bound())
					s.m_visibility.push_back({ node, nullptr, f.get<NiBoolData>(ctrl->data) });
			}
			else if (interp && isFloatInterpolator(interp) && controllerType == "NiAlphaController")
			{
				if (bound())
					s.m_materials.push_back({ node, MaterialTarget::Alpha, 0, UvOp::TranslateU, interp });
			}
			else if (interp && isFloatInterpolator(interp) && controllerType == "NiTextureTransformController")
			{
				if (const auto target = parseTextureTransformId(controllerId); target && bound())
					s.m_materials.push_back({ node, MaterialTarget::Uv, target->first, target->second, interp });
			}
			else if (interp && isPoint3Interpolator(interp) && controllerType == "NiMaterialColorController")
			{
				MaterialTarget target = MaterialTarget::Emissive;
				if (controllerId == "DIFF")
					target = MaterialTarget::Diffuse;
				else if (controllerId == "AMB")
					target = MaterialTarget::Ambient;
				else if (controllerId == "SPEC")
					target = MaterialTarget::Specular;
				if (bound())
					s.m_materials.push_back({ node, target, 0, UvOp::TranslateU, interp });
			}
			// Remaining: facial float extra data, look-at, lights, particle emitters.
		}
		return s;
	}

	Sequence Sequence::embedded(std::shared_ptr<const nif::File> file, const scene::Model& model)
	{
		Sequence s;
		s.m_file = file;
		s.m_name = "(embedded)";
		const nif::File& f = *file;
		bool timed = false;
		const auto follow = [&](Ref first, int node, bool onProperty) {
			for (Ref r = first, guard = 0; r != kNoRef && guard < 64; ++guard)
			{
				const auto* ctrl = f.get<NiSingleInterpController>(r);
				if (!ctrl)
					break;
				const std::string type = f.typeOf(r);
				r = ctrl->nextController;
				const auto* interp = f.get<NiObject>(ctrl->interpolator);
				if (!interp)
					continue; // older files keep their keys in the controller's data
				bool used = true;
				if (onProperty && type == "NiTextureTransformController" && isFloatInterpolator(interp) && ctrl->operation < uint32_t(UvOp::Count))
					s.m_materials.push_back({ node, MaterialTarget::Uv, uint8_t(ctrl->textureSlot), static_cast<UvOp>(ctrl->operation), interp });
				else if (onProperty && type == "NiAlphaController" && isFloatInterpolator(interp))
					s.m_materials.push_back({ node, MaterialTarget::Alpha, 0, UvOp::TranslateU, interp });
				else if (onProperty && type == "NiMaterialColorController" && isPoint3Interpolator(interp))
				{
					static constexpr MaterialTarget kColors[] = { MaterialTarget::Ambient, MaterialTarget::Diffuse, MaterialTarget::Specular, MaterialTarget::Emissive };
					s.m_materials.push_back({ node, kColors[std::min<uint16_t>(ctrl->targetColor, 3)], 0, UvOp::TranslateU, interp });
				}
				else if (!onProperty && type == "NiTransformController" && isTransformInterpolator(interp))
					s.m_transforms.push_back({ node, interp, nullptr });
				else if (!onProperty && type == "NiVisController" && dynamic_cast<const NiBoolInterpolator*>(interp))
					s.m_visibility.push_back({ node, interp, nullptr });
				else
					used = false;
				if (used)
				{
					// One time line for all of them, the widest. They loop: what a model does by
					// itself goes on (rivers flow), whatever their cycle flags say.
					if (!timed)
					{
						s.m_startTime = ctrl->startTime;
						s.m_stopTime = ctrl->stopTime;
						s.m_frequency = ctrl->frequency > 0 ? ctrl->frequency : 1.0f;
						s.m_cycle = CycleType::Loop;
						timed = true;
					}
					s.m_startTime = std::min(s.m_startTime, ctrl->startTime);
					s.m_stopTime = std::max(s.m_stopTime, ctrl->stopTime);
				}
			}
		};
		for (size_t i = 0; i < model.nodes.size(); ++i)
			if (const auto* av = f.get<NiAVObject>(model.nodes[i].block))
			{
				follow(av->controller, int(i), false);
				for (const Ref p : av->properties)
					if (const auto* prop = f.get<NiProperty>(p))
						follow(prop->controller, int(i), true);
			}
		return s;
	}

	std::optional<float> Sequence::textKeyTime(std::string_view key) const
	{
		for (const auto& [time, text] : m_textKeys)
			if (text == key)
				return time - m_startTime;
		return std::nullopt;
	}

	float Sequence::localTime(float time) const
	{
		const float span = duration();
		const float t = time * m_frequency;
		if (span <= 0)
			return m_startTime;
		switch (m_cycle)
		{
		case CycleType::Clamp:
			return m_startTime + std::clamp(t, 0.0f, span);
		case CycleType::Reverse:
		{
			const float p = std::fmod(std::max(t, 0.0f), 2 * span);
			return m_startTime + (p <= span ? p : 2 * span - p);
		}
		default:
			return m_startTime + std::fmod(std::max(t, 0.0f), span);
		}
	}

	void Sequence::apply(float time, Pose& pose) const
	{
		const float t = localTime(time);
		const File& f = *m_file;

		for (const auto& c : m_transforms)
		{
			const TRS trs = c.interpolator ? sampleTransformInterpolator(f, *c.interpolator, t)
										   : sampleTransformData(*static_cast<const NiTransformData*>(c.legacyData), t, {});
			Affine& a = pose.locals[size_t(c.node)];
			if (trs.translation)
				a.translation = *trs.translation;
			if (trs.rotation)
				a.rotation = toMat33(*trs.rotation);
			if (trs.scale)
				a.scale = *trs.scale;
		}

		for (const auto& v : m_visibility)
		{
			const NiBoolData* data = v.legacyData ? static_cast<const NiBoolData*>(v.legacyData) : nullptr;
			bool visible = true;
			if (const auto* bi = dynamic_cast<const NiBoolInterpolator*>(v.interpolator))
			{
				visible = bi->value;
				data = f.get<NiBoolData>(bi->data);
			}
			if (data && !data->data.keys.empty())
			{
				// Step function: the last key at or before t.
				const auto& keys = data->data.keys;
				const auto it = std::upper_bound(keys.begin(), keys.end(), t, [](float x, const Key<uint8_t>& k) { return x < k.time; });
				visible = (it == keys.begin() ? keys.front() : *(it - 1)).value != 0;
			}
			pose.hidden[size_t(v.node)] = visible ? 0 : 1;
		}

		for (const auto& m : m_materials)
		{
			MaterialOverride& o = pose.materials[size_t(m.node)];
			switch (m.target)
			{
			case MaterialTarget::Alpha:
			case MaterialTarget::Uv:
				if (const auto v = sampleFloatInterpolator(f, *m.interpolator, t))
				{
					if (m.target == MaterialTarget::Alpha)
						o.alpha = std::clamp(*v, 0.0f, 1.0f);
					else
						o.uv[m.slot][size_t(m.op)] = *v;
					o.any = true;
				}
				break;
			default:
				if (const auto v = samplePoint3Interpolator(f, *m.interpolator, t))
				{
					(m.target == MaterialTarget::Emissive ? o.emissive : m.target == MaterialTarget::Diffuse ? o.diffuse
							: m.target == MaterialTarget::Ambient ? o.ambient : o.specular) = *v;
					o.any = true;
				}
				break;
			}
		}
	}

	// ------------------------------------------------------------------ player

	Player::Player(std::vector<Clip> clips, std::optional<nif::KfmFile> kfm) : m_clips(std::move(clips)), m_kfm(std::move(kfm))
	{
	}

	std::optional<size_t> Player::find(uint32_t eventCode) const
	{
		for (size_t i = 0; i < m_clips.size(); ++i)
			if (m_clips[i].eventCode == eventCode)
				return i;
		return std::nullopt;
	}

	Player::TransitionInfo Player::transitionInfo(size_t from, size_t to) const
	{
		TransitionInfo info;
		info.duration = m_kfm ? m_kfm->defaultNonSyncDuration : 0.25f;
		if (!m_kfm)
			return info;
		const auto* src = m_kfm->find(m_clips[from].eventCode);
		if (!src)
			return info;
		for (const auto& t : src->transitions)
		{
			if (t.target != m_clips[to].eventCode)
				continue;
			info.type = t.type;
			if (t.type == nif::KfmTransitionType::DefaultSync)
				info.duration = m_kfm->defaultSyncDuration;
			else if (t.type == nif::KfmTransitionType::DefaultNonSync)
				info.duration = m_kfm->defaultNonSyncDuration;
			else
				info.duration = t.duration;
			for (const auto& pair : t.blendPairs)
				if (const auto time = m_clips[to].sequence.textKeyTime(pair.targetKey))
				{
					info.targetOffset = *time;
					break;
				}
			info.chain = t.chain;
			break;
		}
		return info;
	}

	void Player::startTransition(size_t to, float duration, float targetOffset)
	{
		if (m_clips.empty())
			return;
		if (duration > 0)
		{
			m_from = Playing{ m_current, m_currentStart };
			m_fadeStart = m_time;
			m_fadeDuration = duration;
		}
		else
			m_from.reset();
		m_current = to;
		m_currentStart = m_time - targetOffset;
	}

	void Player::play(size_t clip, bool smooth)
	{
		if (clip >= m_clips.size())
			return;
		m_queue.clear();
		m_queuedTarget.reset();
		if (!smooth)
		{
			m_from.reset();
			m_current = clip;
			m_currentStart = m_time;
			m_lastTransition = "instant";
			return;
		}
		const TransitionInfo info = transitionInfo(m_current, clip);
		static constexpr const char* kNames[] = { "blend", "morph", "crossfade", "chain", "default sync", "default nonsync" };
		m_lastTransition = std::format("{} {:.2f}s", kNames[size_t(info.type)], info.duration);
		if (info.type == nif::KfmTransitionType::Chain && !info.chain.empty())
		{
			for (const auto& step : info.chain)
				if (const auto c = find(step.eventCode))
					m_queue.push_back({ *c, step.duration });
			m_queuedTarget = clip;
			if (!m_queue.empty())
			{
				const auto [first, duration] = m_queue.front();
				startTransition(first, std::min(duration, info.duration > 0 ? info.duration : duration), 0);
				return;
			}
		}
		startTransition(clip, info.duration, info.targetOffset);
	}

	void Player::update(float dt)
	{
		m_time += dt;
		if (m_from && m_time - m_fadeStart >= m_fadeDuration)
			m_from.reset();
		if (m_clips.empty())
			return;

		const float elapsed = m_time - m_currentStart;
		if (!m_queue.empty() && m_queue.front().first == m_current && elapsed >= m_queue.front().second)
		{
			// Chain: next intermediate animation or the final target.
			m_queue.erase(m_queue.begin());
			const size_t next = !m_queue.empty() ? m_queue.front().first : m_queuedTarget.value_or(m_current);
			if (m_queue.empty())
				m_queuedTarget.reset();
			const TransitionInfo info = transitionInfo(m_current, next);
			startTransition(next, info.duration, info.targetOffset);
			return;
		}
		const Sequence& seq = m_clips[m_current].sequence;
		if (m_queue.empty() && !m_from && !seq.loops() && elapsed >= seq.duration() && m_fallback && *m_fallback != m_current)
			play(*m_fallback);
	}

	void Player::setLayers(std::vector<size_t> clips)
	{
		m_layers = std::move(clips);
		m_layerStart = m_time;
	}

	Pose Player::evaluate(const scene::Model& model) const
	{
		Pose pose = Pose::bind(model);
		if (m_clips.empty())
			return pose;
		m_clips[m_current].sequence.apply(m_time - m_currentStart, pose);
		if (m_from)
		{
			Pose from = Pose::bind(model);
			m_clips[m_from->clip].sequence.apply(m_time - m_from->start, from);
			pose = blend(from, pose, std::clamp((m_time - m_fadeStart) / m_fadeDuration, 0.0f, 1.0f));
		}
		for (const size_t layer : m_layers)
			if (layer < m_clips.size())
				m_clips[layer].sequence.apply(m_time - m_layerStart, pose);
		return pose;
	}

	std::string Player::describe() const
	{
		if (m_clips.empty())
			return "no animations";
		std::string s = m_clips[m_current].label;
		if (m_from)
			s = std::format("{} -> {} ({}, {:.0f}%)", m_clips[m_from->clip].label, s, m_lastTransition,
				100.0f * std::clamp((m_time - m_fadeStart) / m_fadeDuration, 0.0f, 1.0f));
		return s;
	}
}
