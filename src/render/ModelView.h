#pragma once

// A model as the screens show it on its own (the Civilopedia's units, buildings, improvements
// and resources, the main interface's selected unit): the art's NIF in its idle (a unit's KFM
// event 1000, else the file's own controllers), turned as the script asks (x: tilted towards
// the viewer, z: turned about its up axis, degrees), as large as its picture allows times the
// script's scale, lit from the front, on nothing (the panel shows through). A scene with a
// camera of its own (the religions' "movies": a shrine, its camera moving round it) is seen
// through that camera instead (looking at its ".Target" node when it has one). With layers
// ("camera=<node>;<event code>:<mode>;...") it plays those clips of its KFM together instead of
// its idle, each held at its end (e), looped (l) or once from the start (o), through the camera
// on that node: the spaceship screen's launch pad, its parts shown as the ship is built.

#include "render/Particles.h"
#include "render/Renderer.h"

#include "anim/Animation.h"
#include "core/AssetResolver.h"
#include "nif/NifFile.h"
#include "scene/NifScene.h"

#include <memory>
#include <string>

namespace oc4::render
{
	class ModelView
	{
	public:
		// decal: in place of the model's decal maps (a flag's cloth in a player's colours); its
		// owner keeps it while the view lives.
		ModelView(Renderer& renderer, const AssetResolver& assets, const std::string& nif, const std::string& kfm, float xRotation, float zRotation,
			float scale, bool animate, const std::string& layers = {}, const GpuImage* decal = nullptr);

		bool loaded() const { return m_gpu != nullptr; }
		// Seen as the original shows a player's flag (the main interface's, by the minimap):
		// from the front, its pole in the middle across, its height filling the picture with a
		// little to spare, a little more of it below the middle than above; its blended parts
		// over the panel with their alpha counted twice (the fringe). Measured against Wine.
		void frameAsFlag() { m_flag = true; }
		void update(float dt);
		// Draws it into its picture, width x height; the picture for the overlay (null if none).
		const GpuImage* render(uint32_t width, uint32_t height);

	private:
		anim::Pose pose() const;

		Renderer& m_renderer;
		std::shared_ptr<const nif::File> m_file;
		scene::Model m_model;
		std::unique_ptr<GpuModel> m_gpu;
		anim::Player m_player;
		Particles m_particles;
		OffscreenTarget m_target;
		Bounds m_frame; // (its box as it first stands)
		// Its own camera: the node it hangs from, its place there, its frustum (at distance 1).
		int m_cameraNode = -1, m_cameraTarget = -1;
		struct Layer
		{
			anim::Sequence sequence;
			char mode = 'e';
		};
		std::vector<Layer> m_layers;
		bool m_layered = false;
		float m_time = 0;
		Affine m_cameraLocal;
		float m_frustumTop = 0.4f, m_frustumNear = 1, m_frustumFar = 10000;
		float m_xRotation = 0, m_zRotation = 0, m_scale = 1;
		bool m_animate = true;
		bool m_flag = false;
	};
}
