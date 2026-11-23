#pragma once

// Plays Gamebryo KF controller sequences on a scene::Model. Each controlled
// block of an NiControllerSequence is bound to a model node by name and
// evaluated into a Pose: node transforms, node visibility and per-node material
// overrides (alpha, material colours, texture map transforms).

#include "core/Math.h"
#include "nif/KfmFile.h"
#include "nif/NifFile.h"
#include "scene/NifScene.h"

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oc4::anim
{
	constexpr float kUnset = std::numeric_limits<float>::quiet_NaN();
	inline bool isSet(float v) { return !std::isnan(v); }

	// NiTextureTransformController::TransformMember
	enum class UvOp : uint8_t
	{
		TranslateU, TranslateV, Rotate, ScaleU, ScaleV, Count
	};

	// Animated material state of the properties attached to one node.
	struct MaterialOverride
	{
		float alpha = kUnset;
		std::optional<Vec3> emissive, diffuse, ambient, specular;
		// [texture slot][UvOp]; kUnset = keep the map's static transform.
		std::array<std::array<float, size_t(UvOp::Count)>, 10> uv = makeUnsetUv();

		bool any = false;

		static std::array<std::array<float, size_t(UvOp::Count)>, 10> makeUnsetUv()
		{
			std::array<std::array<float, size_t(UvOp::Count)>, 10> a;
			for (auto& slot : a)
				slot.fill(kUnset);
			return a;
		}
	};

	struct Pose
	{
		std::vector<Affine> locals;
		std::vector<uint8_t> hidden;
		std::vector<MaterialOverride> materials; // one per node

		static Pose bind(const scene::Model& model);
	};

	// w=0 -> a, w=1 -> b. Transforms blend smoothly; visibility switches at the
	// midpoint; alpha blends when both poses animate it.
	Pose blend(const Pose& a, const Pose& b, float w);

	// The material of an item after applying a pose's overrides.
	scene::Material animatedMaterial(const scene::DrawItem& item, const Pose& pose);

	enum class CycleType : uint32_t
	{
		Loop = 0,
		Reverse = 1,
		Clamp = 2,
	};

	class Sequence
	{
	public:
		// Binds the sequenceIndex-th NiControllerSequence root of a .kf file to the model's nodes.
		static Sequence bind(std::shared_ptr<const nif::File> kf, size_t sequenceIndex, const scene::Model& model);
		// The controllers a model's own file puts on its nodes and properties (water flowing
		// through texture transforms, turning wheels, fading glows): what the model does when
		// nothing plays on it. model must have been built from nif.
		static Sequence embedded(std::shared_ptr<const nif::File> nif, const scene::Model& model);

		const std::string& name() const { return m_name; }
		float duration() const { return m_stopTime - m_startTime; }
		CycleType cycle() const { return m_cycle; }
		bool loops() const { return m_cycle != CycleType::Clamp; }
		size_t transformChannels() const { return m_transforms.size(); }
		size_t visibilityChannels() const { return m_visibility.size(); }
		size_t materialChannels() const { return m_materials.size(); }
		size_t channelCount() const { return transformChannels() + visibilityChannels() + materialChannels(); }
		size_t unboundChannels() const { return m_unbound; }
		const std::vector<std::string>& unboundNodes() const { return m_unboundNodes; }
		const std::vector<std::pair<float, std::string>>& textKeys() const { return m_textKeys; } // (in the file's time: from startTime())
		float startTime() const { return m_startTime; }
		std::optional<float> textKeyTime(std::string_view key) const; // relative to the start

		// time: seconds since the sequence started playing; applies the cycle type.
		float localTime(float time) const;

		void apply(float time, Pose& pose) const;
		// Whether it shows and hides the node (a visibility channel on it).
		bool drivesVisibility(int node) const;

	private:
		struct TransformChannel
		{
			int node = -1;
			const nif::NiObject* interpolator = nullptr; // NiTransformInterpolator / NiBSplineCompTransformInterpolator
			const nif::NiObject* legacyData = nullptr;   // NiTransformData for pre-10.1.0.106 files
		};

		struct VisibilityChannel
		{
			int node = -1;
			const nif::NiObject* interpolator = nullptr; // NiBoolInterpolator / NiBoolTimelineInterpolator
			const nif::NiObject* legacyData = nullptr;   // NiVisData (parsed as NiBoolData)
		};

		enum class MaterialTarget : uint8_t
		{
			Alpha, Emissive, Diffuse, Ambient, Specular, Uv
		};

		struct MaterialChannel
		{
			int node = -1;
			MaterialTarget target = MaterialTarget::Alpha;
			uint8_t slot = 0; // Uv: texture slot
			UvOp op = UvOp::TranslateU;
			const nif::NiObject* interpolator = nullptr; // float or point3 interpolators
		};

		std::shared_ptr<const nif::File> m_file;
		std::string m_name;
		float m_startTime = 0, m_stopTime = 0, m_frequency = 1;
		CycleType m_cycle = CycleType::Loop;
		std::vector<TransformChannel> m_transforms;
		std::vector<VisibilityChannel> m_visibility;
		std::vector<MaterialChannel> m_materials;
		size_t m_unbound = 0;
		std::vector<std::string> m_unboundNodes;
		std::vector<std::pair<float, std::string>> m_textKeys;
	};

	struct Clip
	{
		std::string label;
		uint32_t eventCode = 0;
		Sequence sequence;
	};

	// Plays clips of one model with KFM transitions between them, plus optional
	// layers (Civ4 damage states) applied on top of the main animation.
	class Player
	{
	public:
		Player() = default;
		Player(std::vector<Clip> clips, std::optional<nif::KfmFile> kfm);

		const std::vector<Clip>& clips() const { return m_clips; }
		size_t current() const { return m_current; }
		std::optional<size_t> find(uint32_t eventCode) const;

		// Switches to a clip; with smooth=false the switch is instant.
		void play(size_t clip, bool smooth = true);
		void update(float dt);

		// When a non-looping clip ends, transition to this clip (Civ4 returns units to idle).
		void setFallback(std::optional<size_t> clip) { m_fallback = clip; }

		void setLayers(std::vector<size_t> clips);
		const std::vector<size_t>& layers() const { return m_layers; }

		Pose evaluate(const scene::Model& model) const;

		float time() const { return m_time; }
		bool inTransition() const { return m_from.has_value(); }
		std::string describe() const;

	private:
		struct Playing
		{
			size_t clip = 0;
			float start = 0; // player time at which local time 0 was
		};

		struct TransitionInfo
		{
			nif::KfmTransitionType type = nif::KfmTransitionType::DefaultNonSync;
			float duration = 0;
			float targetOffset = 0; // where the target starts (text key)
			std::vector<nif::KfmTransition::ChainStep> chain;
		};

		TransitionInfo transitionInfo(size_t from, size_t to) const;
		void startTransition(size_t to, float duration, float targetOffset);

		std::vector<Clip> m_clips;
		std::optional<nif::KfmFile> m_kfm;
		float m_time = 0;
		size_t m_current = 0;
		float m_currentStart = 0;
		std::optional<Playing> m_from;
		float m_fadeStart = 0, m_fadeDuration = 0;
		std::vector<std::pair<size_t, float>> m_queue; // chain steps: clip, duration
		std::optional<size_t> m_queuedTarget;
		std::optional<size_t> m_fallback;
		std::vector<size_t> m_layers;
		float m_layerStart = 0;
		std::string m_lastTransition;
	};

	// Sampling helpers, exposed for tests.
	struct TRS
	{
		std::optional<Vec3> translation;
		std::optional<Quat> rotation;
		std::optional<float> scale;
	};
	TRS sampleTransformInterpolator(const nif::File& f, const nif::NiObject& interpolator, float t);
	std::optional<float> sampleFloatInterpolator(const nif::File& f, const nif::NiObject& interpolator, float t);
	std::optional<Vec3> samplePoint3Interpolator(const nif::File& f, const nif::NiObject& interpolator, float t);
}
