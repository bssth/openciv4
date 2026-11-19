#include "render/ModelView.h"

#include "nif/KfmFile.h"
#include "nif/NifBlocks.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>

using namespace DirectX;

namespace oc4::render
{
	namespace
	{
		std::string folderOf(const std::string& key)
		{
			const size_t slash = key.find_last_of("/\\");
			return slash == std::string::npos ? std::string() : key.substr(0, slash + 1);
		}
	}

	ModelView::ModelView(Renderer& renderer, const AssetResolver& assets, const std::string& nif, const std::string& kfm, float xRotation,
		float zRotation, float scale, bool animate, const std::string& layers, const GpuImage* decal)
		: m_renderer(renderer), m_xRotation(xRotation), m_zRotation(zRotation), m_scale(scale > 0 ? scale : 1), m_animate(animate)
	{
		try
		{
			std::optional<nif::KfmFile> anims;
			std::string folder;
			if (!kfm.empty())
				if (const auto at = assets.locate(kfm))
				{
					anims = nif::parseKfm(assets.read(*at));
					folder = folderOf(at->key);
				}
			const std::string model = !nif.empty() ? nif : anims ? folder + anims->nifFile : std::string();
			const auto at = model.empty() ? std::nullopt : assets.locate(model);
			if (!at)
				return;
			m_file = std::make_shared<const nif::File>(nif::parseFile(assets.read(*at)));
			m_model = scene::buildModel(*m_file);
			// (the engine's tree cutting spheres: never drawn)
			for (auto& node : m_model.nodes)
			{
				std::string name = node.name;
				for (char& c : name)
					c = char(std::tolower(static_cast<unsigned char>(c)));
				if (name.starts_with("treecut") || name.starts_with("cuttree"))
					node.hidden = true;
			}
			if (folder.empty())
				folder = folderOf(at->key);
			// Layers asked for: those clips of its KFM, the camera on that node.
			std::string cameraName;
			for (size_t at = 0; !layers.empty() && at < layers.size();)
			{
				const size_t end = std::min(layers.find(';', at), layers.size());
				const std::string token = layers.substr(at, end - at);
				at = end + 1;
				m_layered = true;
				if (token.starts_with("camera="))
				{
					cameraName = token.substr(7);
					continue;
				}
				const size_t colon = token.find(':');
				const uint32_t code = uint32_t(std::strtoul(token.c_str(), nullptr, 10));
				if (!anims)
					continue;
				// (by the event code's low 16 bits: the high ones carry more)
				const nif::KfmAnimation* a = anims->find(code);
				for (size_t k = 0; !a && k < anims->animations.size(); ++k)
					if ((anims->animations[k].eventCode & 0xFFFF) == code)
						a = &anims->animations[k];
				if (a)
					if (const auto kf = assets.locate(folder + a->kfFile))
						if (anim::Sequence s = anim::Sequence::bind(std::make_shared<const nif::File>(nif::parseFile(assets.read(*kf))), a->sequenceIndex, m_model);
							s.channelCount() > 0)
							m_layers.push_back({ std::move(s), colon == std::string::npos ? 'e' : token[colon + 1] });
			}
			// Its idle: the KFM's (event 1000 in the code's low 16 bits, as the map finds it), else the
			// first; without one, its own controllers.
			std::vector<anim::Clip> clips;
			bool idle = false;
			if (anims && !m_layered)
				for (const auto& a : anims->animations)
					if (const bool isIdle = (a.eventCode & 0xFFFF) == 1000; (isIdle && !idle) || clips.empty())
						if (const auto kf = assets.locate(folder + a.kfFile))
							if (anim::Sequence s = anim::Sequence::bind(std::make_shared<const nif::File>(nif::parseFile(assets.read(*kf))), a.sequenceIndex, m_model);
								s.channelCount() > 0)
							{
								if (isIdle)
								{
									clips.clear();
									idle = true;
								}
								clips.push_back({ a.kfFile, a.eventCode, std::move(s) });
							}
			if (clips.empty() && !m_layered)
				if (anim::Sequence own = anim::Sequence::embedded(m_file, m_model); own.channelCount() > 0)
					clips.push_back({ "embedded", 0, std::move(own) });
			m_player = anim::Player(std::move(clips), std::move(anims));
			if (!m_player.clips().empty())
			{
				m_player.setFallback(0);
				m_player.play(0, false);
			}
			// (a camera of its own: under which node, where)
			for (size_t b = 0; b < m_file->blocks.size() && m_cameraNode < 0; ++b)
				if (const auto* node = dynamic_cast<const nif::NiNode*>(m_file->blocks[b].get()))
					for (const nif::Ref c : node->children)
						if (const auto* cam = m_file->get<nif::NiCamera>(c))
						{
							const int n = m_model.findNode(node->name);
							if (n < 0 || (!cameraName.empty() && node->name != cameraName))
								continue;
							m_cameraTarget = m_model.findNode(node->name + ".Target");
							m_cameraNode = n;
							m_cameraLocal = Affine{ cam->rotation, cam->translation, cam->scale };
							m_frustumTop = std::max(std::abs(cam->frustumTop), 0.05f);
							m_frustumNear = cam->frustumNear > 0 ? cam->frustumNear : 1;
							m_frustumFar = cam->frustumFar > m_frustumNear ? cam->frustumFar : m_frustumNear * 10000;
							break;
						}
			m_particles = Particles(m_model, 7);
			m_gpu = renderer.upload(m_model, assets, *at, decal);
		}
		catch (const std::exception&)
		{
			m_gpu.reset();
		}
	}

	anim::Pose ModelView::pose() const
	{
		if (m_layered)
		{
			anim::Pose p = anim::Pose::bind(m_model);
			for (const Layer& l : m_layers)
			{
				const float d = std::max(l.sequence.duration(), 0.001f);
				l.sequence.apply(l.mode == 'l' ? std::fmod(m_time, d) : l.mode == 'o' ? std::min(m_time, d) : d, p);
			}
			return p;
		}
		return m_player.clips().empty() ? anim::Pose::bind(m_model) : m_player.evaluate(m_model);
	}

	void ModelView::update(float dt)
	{
		if (!loaded() || !m_animate)
			return;
		m_time += dt;
		m_player.update(dt);
		if (m_particles.any())
		{
			const anim::Pose p = pose();
			m_particles.update(m_model, dt, m_model.worldTransforms(p.locals), m_model.visibility(p.hidden));
		}
	}

	const GpuImage* ModelView::render(uint32_t width, uint32_t height)
	{
		if (!loaded() || width == 0 || height == 0)
			return nullptr;
		const anim::Pose pose = this->pose();
		const auto world = m_model.worldTransforms(pose.locals);
		// Framed by its box as it first stands (not its bind pose's: a figure's arms out),
		// filling the picture, times the scale; seen from its front (Gamebryo's models face
		// -Y), turned and tilted as asked.
		if (m_frame.empty())
		{
			const auto shown = m_model.visibility(pose.hidden);
			for (const auto& item : m_model.items)
			{
				// (what is drawn: not hidden, nor faded out, as a paratrooper's parachute)
				if (item.node >= 0 && size_t(item.node) < shown.size() && !shown[size_t(item.node)])
					continue;
				if (const scene::Material mat = anim::animatedMaterial(item, pose); mat.alphaAnimated && mat.alpha <= 0.001f)
					continue;
				for (size_t v = 0; v < item.vertices.size(); v += 3)
				{
					const scene::Vertex& vx = item.vertices[v];
					if (!item.skinned)
					{
						if (item.node >= 0 && size_t(item.node) < world.size())
							m_frame.add(world[size_t(item.node)].point(vx.position));
						continue;
					}
					Vec3 p{};
					for (int k = 0; k < 4; ++k)
						if (vx.weights[k] && vx.bones[k] < item.bones.size() && size_t(item.bones[vx.bones[k]]) < world.size())
							p = p + (world[size_t(item.bones[vx.bones[k]])] * item.skinToBone[vx.bones[k]]).point(vx.position) * (float(vx.weights[k]) / 255.0f);
					m_frame.add(p);
				}
			}
			if (m_frame.empty())
				m_frame = m_model.bounds;
		}
		if (m_cameraNode >= 0 && size_t(m_cameraNode) < world.size())
		{
			// Through its own camera: Gamebryo's look along their X axis, up their Y.
			const Affine cam = world[size_t(m_cameraNode)] * m_cameraLocal;
			const Vec3 eye = cam.translation;
			Vec3 forward = normalize(cam.vector({ 1, 0, 0 })), up = normalize(cam.vector({ 0, 1, 0 }));
			// (aimed at its target, as the scene's look-at keeps it, the world's up its up)
			if (m_cameraTarget >= 0 && size_t(m_cameraTarget) < world.size())
				if (const Vec3 d = world[size_t(m_cameraTarget)].translation - eye; length(d) > 0.001f)
				{
					forward = normalize(d);
					up = std::abs(forward.z) < 0.98f ? Vec3{ 0, 0, 1 } : up;
				}
			const XMMATRIX view = XMMatrixLookToRH(XMVectorSet(eye.x, eye.y, eye.z, 1), XMVectorSet(forward.x, forward.y, forward.z, 0), XMVectorSet(up.x, up.y, up.z, 0));
			const XMMATRIX proj = XMMatrixPerspectiveFovRH(2 * std::atan(m_frustumTop), float(width) / float(height), m_frustumNear, m_frustumFar);
			XMFLOAT4X4 m;
			XMStoreFloat4x4(&m, view * proj);
			ViewLight light;
			light.direction = normalize(forward + Vec3{ 0.3f, 0.2f, -0.5f });
			m_renderer.beginView(m_target, width, height, std::span<const float, 16>(&m._11, 16), eye, light, { 0, 0, 0 }, 0.0f);
			std::vector<scene::Material> materials;
			materials.reserve(m_model.items.size());
			for (const auto& item : m_model.items)
				materials.push_back(anim::animatedMaterial(item, pose));
			m_renderer.draw(*m_gpu, world, m_model.visibility(pose.hidden), materials);
			const Vec3 right = normalize(cross(forward, up));
			for (size_t i = 0; i < m_particles.systems(); ++i)
			{
				std::vector<scene::Vertex> quads;
				m_particles.quads(i, world, right, up, quads);
				if (!quads.empty())
					m_renderer.drawParticles(*m_gpu, i, m_model.particles[i].material, std::move(quads));
			}
			m_renderer.endView();
			return m_renderer.targetImage(m_target);
		}
		const Bounds& b = m_frame;
		const Vec3 centre = b.empty() ? Vec3{} : (b.min + b.max) * 0.5f;
		const float radius = b.empty() ? 50.0f : std::max(length(b.max - b.min) * 0.5f, 1.0f);
		const float fov = 0.5f, aspect = float(width) / float(height);
		const float yaw = m_zRotation * std::numbers::pi_v<float> / 180, pitch = -m_xRotation * std::numbers::pi_v<float> / 180;
		const Vec3 back{ std::sin(yaw) * std::cos(pitch), -std::cos(yaw) * std::cos(pitch), std::sin(pitch) };
		// (as near as its box's corners all stay in the picture, a little room round it)
		const float tanV = std::tan(fov / 2), tanH = tanV * aspect;
		const Vec3 viewRight = normalize(cross(Vec3{ -back.x, -back.y, -back.z }, Vec3{ 0, 0, 1 })), viewUp = cross(viewRight, Vec3{ -back.x, -back.y, -back.z });
		float distance = radius * 0.2f;
		if (!b.empty())
			for (int k = 0; k < 8; ++k)
			{
				const Vec3 c{ k & 1 ? b.max.x : b.min.x, k & 2 ? b.max.y : b.min.y, k & 4 ? b.max.z : b.min.z };
				const Vec3 d = c - centre;
				const float z = dot(d, back);
				distance = std::max({ distance, std::abs(dot(d, viewRight)) / tanH + z, std::abs(dot(d, viewUp)) / tanV + z });
			}
		distance = distance * 1.08f / m_scale;
		const Vec3 eye = centre + back * distance;
		const XMMATRIX view = XMMatrixLookAtRH(XMVectorSet(eye.x, eye.y, eye.z, 1), XMVectorSet(centre.x, centre.y, centre.z, 1), XMVectorSet(0, 0, 1, 0));
		const XMMATRIX proj = XMMatrixPerspectiveFovRH(fov, aspect, std::max(distance - radius * 2, distance * 0.01f), distance + radius * 2);
		XMFLOAT4X4 m;
		XMStoreFloat4x4(&m, view * proj);
		ViewLight light;
		light.direction = normalize(Vec3{ -back.x, -back.y, -back.z } + Vec3{ 0.35f, 0.2f, -0.6f });
		m_renderer.beginView(m_target, width, height, std::span<const float, 16>(&m._11, 16), eye, light, { 0, 0, 0 }, 0.0f);
		std::vector<scene::Material> materials;
		materials.reserve(m_model.items.size());
		for (const auto& item : m_model.items)
			materials.push_back(anim::animatedMaterial(item, pose));
		m_renderer.draw(*m_gpu, world, m_model.visibility(pose.hidden), materials);
		const Vec3 forward = normalize(centre - eye), right = normalize(cross(forward, Vec3{ 0, 0, 1 })), up = cross(right, forward);
		for (size_t i = 0; i < m_particles.systems(); ++i)
		{
			std::vector<scene::Vertex> quads;
			m_particles.quads(i, world, right, up, quads);
			if (!quads.empty())
				m_renderer.drawParticles(*m_gpu, i, m_model.particles[i].material, std::move(quads));
		}
		m_renderer.endView();
		return m_renderer.targetImage(m_target);
	}
}
