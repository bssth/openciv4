#include "render/Leaderhead.h"

#include "nif/KfmFile.h"
#include "nif/NifBlocks.h"

#include <DirectXMath.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

		std::string lower(std::string s)
		{
			for (char& c : s)
				c = char(std::tolower(static_cast<unsigned char>(c)));
			return s;
		}

		Vec3 column(const Mat33& m, int c) { return { m.m[0][c], m.m[1][c], m.m[2][c] }; }

		bool playLog()
		{
#pragma warning(suppress : 4996)
			static const bool log = std::getenv("OC4_PLAY_LOG") != nullptr;
			return log;
		}
	}

	Leaderhead::Leaderhead(Renderer& renderer, const AssetResolver& assets, const std::string& nif, const std::string& kfm, const std::string& backgroundKfm)
		: m_renderer(renderer), m_assets(assets)
	{
		if (!load(m_leader, nif, kfm))
			return;
		if (!backgroundKfm.empty())
			load(m_background, std::string(), backgroundKfm);

		// Its camera: the leader's NiCamera (the background has the same one).
		for (const Part* part : { &m_leader, &m_background })
		{
			if (!part->gpu || m_cameraNode >= 0)
				continue;
			for (size_t i = 0; i < part->model.nodes.size(); ++i)
				if (const auto* camera = part->file->get<nif::NiCamera>(part->model.nodes[i].block))
				{
					m_cameraPart = part;
					m_cameraNode = int(i);
					const float f[6] = { camera->frustumLeft, camera->frustumRight, camera->frustumTop, camera->frustumBottom, camera->frustumNear,
						camera->frustumFar };
					std::memcpy(m_frustum, f, sizeof(f));
					break;
				}
		}

		// Its key light (the art's "key_diffuse"), shining on the leader; the ambient one's colour.
		const auto bind = m_leader.model.worldTransforms(m_leader.model.bindLocals());
		const Vec3 centre = m_leader.model.bounds.empty() ? Vec3{} : (m_leader.model.bounds.min + m_leader.model.bounds.max) * 0.5f;
		for (size_t i = 0; i < m_leader.model.nodes.size(); ++i)
			if (const auto* light = m_leader.file->get<nif::NiLight>(m_leader.model.nodes[i].block))
			{
				const std::string name = lower(m_leader.model.nodes[i].name);
				const Vec3 colour{ light->diffuse.r * light->dimmer, light->diffuse.g * light->dimmer, light->diffuse.b * light->dimmer };
				if (name.find("key_diffuse") != std::string::npos)
				{
					m_light.direction = normalize(centre - bind[i].translation);
					m_light.color = colour;
				}
				else if (name.find("ambient") != std::string::npos)
					m_light.ambient = colour;
			}

		// Its parts drawn without its shader (fixed function) are lit by the lights its nodes hold
		// (their effects lists): point lights as Direct3D has them, the ambient lights summed. Not
		// by "BloomFactors" and "BloomColorChannelMix": they only carry the bloom's settings to the
		// shader (GSC:g_f3Bloom...); lit by them, Victoria's lace veil turned cyan and Genghis
		// Khan's fur blue, in the original they are cream and dark grey.
		for (const nif::Ref r : [&] {
				 std::vector<nif::Ref> refs;
				 for (size_t b = 0; b < m_leader.file->blocks.size(); ++b)
					 if (const auto* node = m_leader.file->get<nif::NiNode>(nif::Ref(b)))
						 for (const nif::Ref e : node->effects)
							 if (std::find(refs.begin(), refs.end(), e) == refs.end())
								 refs.push_back(e);
				 return refs;
			 }())
		{
			const auto* light = m_leader.file->get<nif::NiLight>(r);
			if (!light || !light->switchState)
				continue;
			const std::string type = m_leader.file->typeOf(r);
			if (lower(light->name).starts_with("bloom"))
				continue;
			const Vec3 ambient{ light->ambient.r * light->dimmer, light->ambient.g * light->dimmer, light->ambient.b * light->dimmer };
			m_fixedAmbient = m_fixedAmbient + ambient;
			if (type != "NiPointLight")
				continue;
			for (size_t i = 0; i < m_leader.model.nodes.size(); ++i)
				if (m_leader.model.nodes[i].block == r)
					m_fixedLights.push_back({ bind[i].translation, { light->diffuse.r * light->dimmer, light->diffuse.g * light->dimmer, light->diffuse.b * light->dimmer } });
		}

		// The greeting, then the idle of its attitude.
		m_leader.player.setFallback(clip(idleEvent()));
		if (const auto greeting = clip(0))
			m_leader.player.play(*greeting, false);
		else if (const auto idle = clip(idleEvent()))
			m_leader.player.play(*idle, false);
		if (m_background.gpu && !m_background.player.clips().empty())
			m_background.player.play(0, false);
		if (playLog())
			std::printf("  leaderhead %s: %zu animations, %s background\n", nif.c_str(), m_leader.player.clips().size(), m_background.gpu ? "its" : "no");
	}

	bool Leaderhead::load(Part& part, const std::string& nif, const std::string& kfm)
	{
		try
		{
			std::optional<nif::KfmFile> anims;
			std::string folder;
			if (!kfm.empty())
				if (const auto at = m_assets.locate(kfm))
				{
					anims = nif::parseKfm(m_assets.read(*at));
					folder = folderOf(at->key);
				}
			const std::string model = !nif.empty() ? nif : anims ? folder + anims->nifFile : std::string();
			const auto at = model.empty() ? std::nullopt : m_assets.locate(model);
			if (!at)
				return false;
			part.file = std::make_shared<const nif::File>(nif::parseFile(m_assets.read(*at)));
			part.model = scene::buildModel(*part.file);
			part.gpu = m_renderer.upload(part.model, m_assets, *at);
			if (folder.empty())
				folder = folderOf(at->key);
			std::vector<anim::Clip> clips;
			if (anims)
				for (const auto& a : anims->animations)
					if (const auto kf = m_assets.locate(folder + a.kfFile))
						if (anim::Sequence s = anim::Sequence::bind(std::make_shared<const nif::File>(nif::parseFile(m_assets.read(*kf))), a.sequenceIndex,
								part.model);
							s.channelCount() > 0)
							clips.push_back({ a.kfFile, a.eventCode, std::move(s) });
			// (what its own controllers do: the sky's clouds)
			if (clips.empty())
				if (anim::Sequence own = anim::Sequence::embedded(part.file, part.model); own.channelCount() > 0)
					clips.push_back({ "embedded", 0, std::move(own) });
			part.player = anim::Player(std::move(clips), std::move(anims));
			return true;
		}
		catch (const std::exception&)
		{
			part = {};
			return false;
		}
	}

	std::optional<size_t> Leaderhead::clip(int action) const
	{
		// (the art's event codes: the action in the low 16 bits, as 65536001 for idle 1)
		const auto& clips = m_leader.player.clips();
		for (size_t i = 0; i < clips.size(); ++i)
			if (int(clips[i].eventCode & 0xFFFF) == action)
				return i;
		return std::nullopt;
	}

	void Leaderhead::setAttitude(int attitude)
	{
		attitude = std::clamp(attitude, 0, 4);
		if (attitude == m_attitude || !loaded())
			return;
		m_attitude = attitude;
		const auto idle = clip(idleEvent());
		m_leader.player.setFallback(idle);
		// (resting in another idle: this one)
		const auto& clips = m_leader.player.clips();
		if (const size_t now = m_leader.player.current(); idle && now < clips.size() && (clips[now].eventCode & 0xFFFF) >= 1 &&
				(clips[now].eventCode & 0xFFFF) <= 5)
			m_leader.player.play(*idle, true);
	}

	void Leaderhead::perform(int action)
	{
		if (!loaded())
			return;
		// (a mood is an idle: it rests in that one from now on)
		if (action >= 1 && action <= 5)
		{
			m_attitude = 5 - action;
			m_leader.player.setFallback(clip(action));
		}
		const auto chosen = clip(action < 0 ? idleEvent() : action);
		if (chosen)
			m_leader.player.play(*chosen, true);
		if (playLog())
			std::printf("  leaderhead: action %d -> %s\n", action, chosen ? m_leader.player.clips()[*chosen].label.c_str() : "none");
	}

	void Leaderhead::update(float dt)
	{
		if (!loaded())
			return;
		m_leader.player.update(dt);
		if (m_background.gpu)
			m_background.player.update(dt);
	}

	void Leaderhead::draw(const Part& part)
	{
		const anim::Pose pose = part.player.clips().empty() ? anim::Pose::bind(part.model) : part.player.evaluate(part.model);
		std::vector<scene::Material> materials;
		materials.reserve(part.model.items.size());
		for (const auto& item : part.model.items)
		{
			materials.push_back(anim::animatedMaterial(item, pose));
			// (the leader's parts in Gamebryo's order: a blended one whose alpha property has "no
			// sorter" where the model has it, the others sorted by distance after them; Sitting
			// Bull's feather rings before the fluff in front of them)
			if (&part == &m_leader && materials.back().alphaBlend && materials.back().alphaNoSorter)
				materials.back().blendUnsorted = true;
		}
		m_renderer.draw(*part.gpu, part.model.worldTransforms(pose.locals), part.model.visibility(pose.hidden), materials);
	}

	const GpuImage* Leaderhead::render(uint32_t width, uint32_t height)
	{
		if (!loaded() || width == 0 || height == 0)
			return nullptr;
		// The camera as the model has it now: Gamebryo's look along their X, up their Y.
		Vec3 eye{ 0, -80, 25 }, forward{ 0, 1, 0 }, up{ 0, 0, 1 };
		if (m_cameraPart)
		{
			const anim::Pose pose = m_cameraPart->player.clips().empty() ? anim::Pose::bind(m_cameraPart->model) : m_cameraPart->player.evaluate(m_cameraPart->model);
			const Affine camera = m_cameraPart->model.worldTransforms(pose.locals)[size_t(m_cameraNode)];
			eye = camera.translation;
			forward = normalize(column(camera.rotation, 0));
			up = normalize(column(camera.rotation, 1));
		}
		// Its frustum, as high as it is and as wide as the picture.
		const float n = std::max(m_frustum[4], 0.01f), f = std::max(m_frustum[5], n + 1);
		const float top = m_frustum[2], bottom = m_frustum[3], centre = (m_frustum[0] + m_frustum[1]) / 2;
		const float half = (top - bottom) / 2 * float(width) / float(height);
		const XMMATRIX view = XMMatrixLookToRH(XMVectorSet(eye.x, eye.y, eye.z, 1), XMVectorSet(forward.x, forward.y, forward.z, 0),
			XMVectorSet(up.x, up.y, up.z, 0));
		const XMMATRIX proj = XMMatrixPerspectiveOffCenterRH((centre - half) * n, (centre + half) * n, bottom * n, top * n, n, f);
		XMFLOAT4X4 m;
		XMStoreFloat4x4(&m, view * proj);
		if (!m_logged && playLog())
		{
			m_logged = true;
			std::printf("  leaderhead: camera at %.1f,%.1f,%.1f looking %.2f,%.2f,%.2f, frustum %.3f %.3f %.3f %.3f %.2f %.1f, light %.2f,%.2f,%.2f\n", eye.x,
				eye.y, eye.z, forward.x, forward.y, forward.z, m_frustum[0], m_frustum[1], m_frustum[2], m_frustum[3], m_frustum[4], m_frustum[5],
				m_light.color.x, m_light.color.y, m_light.color.z);
		}
		m_renderer.beginView(m_target, width, height, std::span<const float, 16>(&m._11, 16), eye, m_light, { 0, 0, 0 });
		// (the scene behind: some of it stands nearer the camera than the leader, as Pacal's)
		if (m_background.gpu)
		{
			draw(m_background);
			m_renderer.viewLayer();
		}
		m_renderer.setFixedLights(m_fixedLights, m_fixedAmbient);
		draw(m_leader);
		m_renderer.endView();
		return m_renderer.targetImage(m_target);
	}
}
