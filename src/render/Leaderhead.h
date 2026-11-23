#pragma once

// A leader's head as the original's screens show it (diplomacy, the Civilopedia's leaders,
// Dawn of Man): the leader's model and its background scene (the art's NIF, KFM and
// background KFM), drawn through the camera the model has (its NiCamera) and lit by its key
// light, into a picture of its own the screens draw where the head goes. The greeting
// first, then the idle of its attitude; the gestures it is asked for (LeaderheadAction, which
// are the KFM's event codes: greeting 0, the idles from friendly 1 to furious 5, disagree 6,
// agree 7), then that idle again.

#include "render/Renderer.h"

#include "anim/Animation.h"
#include "core/AssetResolver.h"
#include "nif/NifFile.h"
#include "scene/NifScene.h"

#include <memory>
#include <optional>
#include <string>

namespace oc4::render
{
	class Leaderhead
	{
	public:
		// Paths as the art defines them (CvArtInfoLeaderhead: NIF, KFM, background KFM).
		Leaderhead(Renderer& renderer, const AssetResolver& assets, const std::string& nif, const std::string& kfm, const std::string& backgroundKfm);

		bool loaded() const { return m_leader.gpu != nullptr; }
		// AttitudeTypes (furious 0 to friendly 4): which idle it rests in.
		void setAttitude(int attitude);
		// LeaderheadAction (-1: its idle).
		void perform(int action);
		void update(float dt);
		// Draws it into its picture, width x height; the picture for the overlay (null if none).
		const GpuImage* render(uint32_t width, uint32_t height);

	private:
		struct Part
		{
			std::shared_ptr<const nif::File> file;
			scene::Model model;
			std::unique_ptr<GpuModel> gpu;
			anim::Player player;
		};
		// A model and its animations: the NIF (or, with none, the one the KFM names).
		bool load(Part& part, const std::string& nif, const std::string& kfm);
		void draw(const Part& part);
		int idleEvent() const { return 5 - m_attitude; }
		// The leader's clip of a LeaderheadAction.
		std::optional<size_t> clip(int action) const;

		Renderer& m_renderer;
		const AssetResolver& m_assets;
		Part m_leader, m_background;
		OffscreenTarget m_target;
		int m_attitude = 3; // ATTITUDE_PLEASED
		// Where the camera and the key light are: (part, node), the first part's when both have them.
		const Part* m_cameraPart = nullptr;
		int m_cameraNode = -1;
		float m_frustum[6]{ -0.5f, 0.5f, 0.5f, -0.5f, 1, 1000 }; // left, right, top, bottom, near, far
		ViewLight m_light;
		// The leader's own lights, for its parts drawn without its shader (fixed function).
		std::vector<PointLight> m_fixedLights;
		Vec3 m_fixedAmbient{};
		bool m_logged = false;
	};
}
