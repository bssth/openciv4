#include "Sounds.h"

#include "audio/Audio.h"

#include "CvGameCoreDLL.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <mutex>
#include <random>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace oc4::host
{
	namespace
	{
		using Clock = std::chrono::steady_clock;
		using Kind = AudioScripts::Kind;
		using PcmPtr = std::shared_ptr<const audio::Pcm>;

		constexpr float kPlotSize = 180; // world units (PLOT_SIZE), the scripts' distances are in them
		constexpr size_t kMaxEffects = 24;

		// The player's volumes (the options' sliders, 0..1): master, music, effects, speech,
		// ambience, interface.
		std::atomic<float> g_volumes[6] = { 1, 1, 1, 1, 1, 1 };
		float userGain(Kind k)
		{
			switch (k)
			{
			case Kind::Music: return g_volumes[1];
			case Kind::Speech: return g_volumes[3];
			case Kind::Ambience: return g_volumes[4];
			case Kind::Interface: return g_volumes[5];
			default: return g_volumes[2];
			}
		}

		// How loud each kind plays (the original's options have a slider each).
		float kindGain(Kind k)
		{
			switch (k)
			{
			case Kind::Music: return 0.45f;
			case Kind::Ambience: return 0.5f;
			case Kind::Interface: return 0.7f;
			case Kind::Speech: return 1.0f;
			default: return 0.9f;
			}
		}

		bool playLog()
		{
			static const bool log = std::getenv("OC4_PLAY_LOG") != nullptr;
			return log;
		}
	}

	struct Sounds::Impl
	{
		const AssetResolver& assets;
		AudioScripts& scripts;

		struct Request
		{
			AudioScripts::Table table = AudioScripts::Script2D;
			int script = -1;
			std::optional<Place> place;
		};
		std::mutex mutex; // (the requests and the talk's music, from any thread)
		std::vector<Request> queue;
		std::optional<std::pair<int, int>> talkChange;

		std::atomic<bool> started = false;
		bool muted = false;
		float master = -1; // (the device's master volume as set)
		std::unique_ptr<audio::Device> device;
		std::mt19937 rng{ std::random_device{}() };

		// The sounds by file (null: it could not be read), and those being decoded.
		std::unordered_map<std::string, PcmPtr> cache;
		std::unordered_map<std::string, std::shared_future<PcmPtr>> loading;
		std::unordered_set<std::string> reported;
		struct Waiting
		{
			Request request;
			std::string file;
			Clock::time_point since;
		};
		std::vector<Waiting> waiting;
		std::vector<audio::Device::Voice> effects;
		std::map<std::pair<int, int>, Clock::time_point> recent;

		// Music: one voice at a time; a talk's replaces the era's, fading it out.
		audio::Device::Voice music = 0, movie = 0;
		bool musicIsEra = false, musicIsIntro = false, fadingOut = false;
		float musicGain = 0, fade = 1;
		struct Pending
		{
			std::string file, tag;
			float gain = 0;
			bool loop = false, era = false, intro = false;
		};
		std::optional<Pending> pending;
		std::optional<std::pair<int, int>> talk; // (intro, loop) while talking
		bool introDone = false;
		Clock::time_point nextTrack{}, lastUpdate{};
		std::string lastTrack;

		// The soundscape heard, and those fading out after it.
		struct Scape
		{
			int index = -1;
			struct Bed
			{
				std::string file;
				float gain = 0;
				audio::Device::Voice voice = 0; // (0 until decoded)
			};
			std::vector<Bed> beds; // its looping 2D sounds
			struct Timed
			{
				Request request;
				Clock::time_point next;
			};
			std::vector<Timed> timed; // its sounds now and then
			float fade = 0;
		};
		Scape scape;
		std::vector<Scape> fadingScapes;
		std::optional<int> scapeChange;
		int wantScape = -1;
		Clock::time_point wantSince{};

		// The leaders' music over their land, each looping as loud as its share of the view
		// (kept playing unheard a while: back over that land, it goes on where it was).
		struct Territory
		{
			int script = -1;
			std::string file, tag;
			float gain = 0, share = 0, level = 0;
			audio::Device::Voice voice = 0;
			Clock::time_point heard{};
		};
		std::vector<Territory> territory;
		std::optional<std::vector<std::pair<int, float>>> territoryChange;
		float eraShare = 1; // how loud the era's soundtrack plays beside them

		Impl(const AssetResolver& a, AudioScripts& s) : assets(a), scripts(s) {}

		Clock::time_point after(int minMs, int maxMs, Clock::time_point now)
		{
			return now + std::chrono::milliseconds(int(pick(std::max(minMs, 1500), std::max(maxMs, 1500))));
		}

		void startScape(int index, Clock::time_point now)
		{
			if (scape.index >= 0)
				fadingScapes.push_back(std::move(scape));
			scape = {};
			scape.index = index;
			const AudioScripts::Scape* s = scripts.soundscape(index);
			if (!s)
				return;
			const float volume = pick(s->minVolume, s->maxVolume) / 100;
			for (const auto& [table, script] : s->elements)
				if (const AudioScripts::Script* e = scripts.script(table, script))
				{
					if (table == AudioScripts::Script2D && e->looping)
					{
						if (std::string file = fileOf(*e); !file.empty())
							scape.beds.push_back({ std::move(file), volume * pick(e->minVolume, e->maxVolume) / 100 * kindGain(e->kind) });
					}
					else
						scape.timed.push_back({ Request{ table, script, std::nullopt }, after(e->minDelay, e->maxDelay, now) });
				}
			if (playLog())
				std::printf("  soundscape: %s\n", s->id.c_str());
		}

		void updateScapes(const Listener& l, Clock::time_point now, float dt)
		{
			// (louder from close by, as the original's: full at the nearest, a quarter from far up)
			const float zoom = std::clamp(1 - (l.distance - 4) / 28, 0.25f, 1.0f);
			const auto setVolumes = [&](Scape& s) {
				for (auto& b : s.beds)
					if (b.voice)
						device->setVolume(b.voice, b.gain * s.fade * zoom * userGain(Kind::Ambience));
			};
			// The last ones fade out.
			for (Scape& s : fadingScapes)
			{
				s.fade -= dt / 1.5f;
				setVolumes(s);
			}
			std::erase_if(fadingScapes, [&](Scape& s) {
				if (s.fade > 0)
					return false;
				for (auto& b : s.beds)
					device->stop(b.voice);
				return true;
			});
			// This one fades in: its beds once decoded, its other sounds when it is their time,
			// somewhere around the camera.
			scape.fade = std::min(1.0f, scape.fade + dt / 1.5f);
			for (auto& b : scape.beds)
				if (!b.voice)
					if (const auto pcm = sound(b.file); pcm && *pcm)
						b.voice = device->play(*pcm, 0, 0, 1, true);
			setVolumes(scape);
			for (auto& t : scape.timed)
			{
				if (now < t.next)
					continue;
				const AudioScripts::Script* e = scripts.script(t.request.table, t.request.script);
				if (!e)
					continue;
				t.next = after(e->minDelay, e->maxDelay, now);
				Request r = t.request;
				if (r.table == AudioScripts::Script3D)
				{
					const float d = pick(e->minListenerDistance, e->maxListenerDistance) / kPlotSize;
					const float a = std::uniform_real_distribution<float>(0, 6.2831853f)(rng);
					r.place = Place{ l.x + d * std::cos(a), l.y + d * std::sin(a) };
				}
				request(r, l, now);
			}
		}

		// The sound's file: "Sounds/Error" is a .wav or an .mp3 in some layer, or several
		// ("Sounds/Units/FranceSelect-000.wav", -001, ...), one of them at random each time;
		// empty if none.
		std::unordered_map<std::string, std::vector<std::string>> files;
		std::string fileOf(const AudioScripts::Script& s)
		{
			auto it = files.find(s.sound);
			if (it == files.end())
			{
				std::vector<std::string> found;
				const std::string base = scripts.soundFile(s.sound);
				for (const char* ext : { ".wav", ".mp3", ".ogg", "" })
				{
					if (base.empty() || !found.empty())
						break;
					if (*ext == 0)
					{
						if (base.find('.') != std::string::npos && assets.locate(base))
							found.push_back(base);
						break;
					}
					if (assets.locate(base + ext))
						found.push_back(base + ext);
					else
						for (int i = 0; i < 100; ++i)
						{
							char suffix[8];
							std::snprintf(suffix, sizeof suffix, "-%03d", i);
							if (!assets.locate(base + suffix + ext))
								break;
							found.push_back(base + suffix + ext);
						}
				}
				it = files.emplace(s.sound, std::move(found)).first;
			}
			if (it->second.empty())
				return {};
			return it->second[std::uniform_int_distribution<size_t>(0, it->second.size() - 1)(rng)];
		}

		// The decoded sound if it is ready; starts decoding it otherwise.
		std::optional<PcmPtr> sound(const std::string& file)
		{
			if (const auto it = cache.find(file); it != cache.end())
				return it->second;
			auto it = loading.find(file);
			if (it == loading.end())
			{
				const auto loc = assets.locate(file);
				if (!loc)
					return cache[file] = nullptr;
				auto bytes = std::make_shared<std::vector<std::byte>>(assets.read(*loc));
				it = loading.emplace(file, std::async(std::launch::async, [bytes, file]() -> PcmPtr {
					std::string error;
					auto pcm = audio::decode(*bytes, &error);
					if (!pcm)
					{
						std::fprintf(stderr, "[oc4core] sound %s: %s\n", file.c_str(), error.c_str());
						return nullptr;
					}
					return std::make_shared<const audio::Pcm>(std::move(*pcm));
				}).share()).first;
			}
			if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
				return std::nullopt;
			PcmPtr pcm = it->second.get();
			loading.erase(it);
			// (music is played once and let go: kept, a soundtrack would hold tens of megabytes)
			return cache[file] = pcm;
		}

		float pick(int lo, int hi)
		{
			if (hi < lo)
				std::swap(lo, hi);
			return lo == hi ? float(lo) : std::uniform_real_distribution<float>(float(lo), float(hi))(rng);
		}

		void playEffect(const Request& r, const AudioScripts::Script& s, const PcmPtr& pcm, const Listener& l)
		{
			float gain = pick(s.minVolume, s.maxVolume) / 100 * kindGain(s.kind) * userGain(s.kind), pan = 0;
			if (r.place)
			{
				const float dx = r.place->x - l.x, dy = r.place->y - l.y, d = std::hypot(dx, dy) * kPlotSize;
				if (s.cutoffDistance > 0 && d > float(s.cutoffDistance))
					return;
				if (s.fullVolumeDistance > 0 && d > float(s.fullVolumeDistance))
					gain *= float(s.fullVolumeDistance) / d;
				gain *= std::clamp(9.0f / std::max(l.distance, 1.0f), 0.35f, 1.0f);
				pan = std::clamp(dx / 8, -1.0f, 1.0f) * 0.8f;
			}
			if (gain < 0.01f)
				return;
			const float pitch = 1 + pick(s.pitchDown, s.pitchUp) / 100000;
			std::erase_if(effects, [&](auto v) { return !device->playing(v); });
			if (effects.size() >= kMaxEffects)
				return;
			if (const auto v = device->play(pcm, gain, pan, pitch, false))
				effects.push_back(v);
			if (playLog())
				std::printf("  sound: %s%s\n", s.id.c_str(), r.place ? " (on the map)" : "");
		}

		void request(const Request& r, const Listener& l, Clock::time_point now)
		{
			const AudioScripts::Script* s = scripts.script(r.table, r.script);
			if (!s)
				return;
			if (s->kind == Kind::Music)
				return; // (music is the soundtrack's and the talks')
			if (s->notPlayPercent > 0 && std::uniform_int_distribution<int>(0, 99)(rng) < s->notPlayPercent)
				return;
			// The same sound asked for again at once (a stack of units moving) plays once.
			auto& last = recent[{ int(r.table), r.script }];
			if (now - last < std::chrono::milliseconds(80))
				return;
			last = now;
			const std::string file = fileOf(*s);
			if (file.empty())
			{
				if (reported.insert(s->id).second)
					std::fprintf(stderr, "[oc4core] no sound file for %s (%s)\n", s->id.c_str(), s->sound.c_str());
				return;
			}
			if (const auto pcm = sound(file))
			{
				if (*pcm)
					playEffect(r, *s, *pcm, l);
			}
			else
				waiting.push_back({ r, file, now });
		}

		// The leaders' music: each toward its share (none while talking or a movie plays), the
		// era's soundtrack the rest.
		void updateTerritory(Clock::time_point now, float dt, bool silent, const std::optional<std::vector<std::pair<int, float>>>& change)
		{
			if (change)
			{
				for (Territory& t : territory)
					t.share = 0;
				for (const auto& [script, share] : *change)
				{
					auto it = std::find_if(territory.begin(), territory.end(), [&](const Territory& t) { return t.script == script; });
					if (it == territory.end())
					{
						const AudioScripts::Script* s = scripts.script(AudioScripts::Script2D, script);
						if (!s)
							continue;
						std::string file = fileOf(*s);
						if (file.empty())
							continue;
						territory.push_back({ script, std::move(file), s->id, pick(s->minVolume, s->maxVolume) / 100 * kindGain(Kind::Music) });
						it = territory.end() - 1;
					}
					it->share = std::clamp(share, 0.0f, 1.0f);
				}
			}
			float heard = 0;
			for (Territory& t : territory)
			{
				const float target = silent ? 0 : t.share;
				const float step = dt / 1.5f;
				t.level = t.level < target ? std::min(target, t.level + step) : std::max(target, t.level - step);
				if (t.level > 0 || target > 0)
					t.heard = now;
				if (!t.voice && target > 0)
					if (const auto pcm = sound(t.file))
						if (*pcm) // (one that cannot be read stays unheard: the soundtrack plays)
						{
							cache.erase(t.file); // (held by its voice)
							t.voice = device->play(*pcm, 0, 0, 1, true);
							if (playLog())
								std::printf("  territory music: %s\n", t.tag.c_str());
						}
				if (t.voice)
					device->setVolume(t.voice, t.gain * t.level * userGain(Kind::Music));
				// (a voice still decoding is not heard: the soundtrack goes on meanwhile)
				if (t.voice)
					heard += t.level;
			}
			std::erase_if(territory, [&](Territory& t) {
				if (now - t.heard < std::chrono::seconds(60))
					return false;
				if (t.voice)
					device->stop(t.voice);
				return true;
			});
			eraShare = 1 - std::min(1.0f, heard);
		}

		// The next music: the talk's intro, its loop, or one of the era's soundtracks.
		void chooseMusic(const std::string& eraType, Clock::time_point now)
		{
			const auto pend = [&](int script, bool loop, bool era, bool intro) {
				const AudioScripts::Script* s = scripts.script(AudioScripts::Script2D, script);
				if (!s)
					return false;
				const std::string file = fileOf(*s);
				if (file.empty())
					return false;
				pending = Pending{ file, s->id, pick(s->minVolume, s->maxVolume) / 100 * kindGain(Kind::Music), loop, era, intro };
				return true;
			};
			if (talk)
			{
				if (!introDone && talk->first >= 0 && pend(talk->first, false, false, true))
					return;
				introDone = true;
				pend(talk->second, true, false, false);
				return;
			}
			if (now < nextTrack)
				return;
			const AudioScripts::Soundtracks* tracks = scripts.soundtracks(eraType);
			if (!tracks || tracks->scripts.empty())
				return;
			std::vector<std::string> choices = tracks->scripts;
			if (choices.size() > 1)
				std::erase(choices, lastTrack);
			const std::string& tag = tracks->firstFirst && lastTrack.empty() ? tracks->scripts.front()
				: choices[std::uniform_int_distribution<size_t>(0, choices.size() - 1)(rng)];
			lastTrack = tag;
			if (!pend(scripts.index(tag, AUDIOTAG_2DSCRIPT), false, true, false))
				nextTrack = now + std::chrono::seconds(30); // (a missing track: try another later)
		}

		void updateMusic(const std::string& eraType, Clock::time_point now, float dt)
		{
			if (movie && device->playing(movie))
			{
				if (music)
					device->setVolume(music, 0);
				return;
			}
			movie = 0;
			if (music && !device->playing(music))
			{
				if (musicIsIntro)
					introDone = true;
				if (musicIsEra) // (some silence between the tracks)
					nextTrack = now + std::chrono::milliseconds(3000 + 10 * std::max(0, scripts.soundtracks(eraType) ? scripts.soundtracks(eraType)->space : 0));
				music = 0, fadingOut = false;
			}
			if (fadingOut && music)
			{
				fade -= dt / 0.8f;
				if (fade <= 0)
				{
					device->stop(music);
					music = 0, fadingOut = false;
				}
				else
					device->setVolume(music, musicGain * fade * (musicIsEra ? eraShare : 1) * userGain(Kind::Music));
			}
			else if (music) // (its slider may have moved; the era's under the leaders')
				device->setVolume(music, musicGain * (musicIsEra ? eraShare : 1) * userGain(Kind::Music));
			if (music)
				return;
			if (!pending)
				chooseMusic(eraType, now);
			if (!pending)
				return;
			const auto pcm = sound(pending->file);
			if (!pcm)
				return; // (decoding)
			cache.erase(pending->file);
			if (*pcm)
			{
				music = device->play(*pcm, pending->gain * (pending->era ? eraShare : 1) * userGain(Kind::Music), 0, 1, pending->loop);
				musicGain = pending->gain, fade = 1;
				musicIsEra = pending->era, musicIsIntro = pending->intro;
				if (playLog())
					std::printf("  music: %s\n", pending->tag.c_str());
			}
			else if (pending->era)
				nextTrack = now + std::chrono::seconds(30);
			else if (pending->intro)
				introDone = true;
			pending.reset();
		}
	};

	Sounds::Sounds(const AssetResolver& assets, AudioScripts& scripts) : m(std::make_unique<Impl>(assets, scripts)) {}
	Sounds::~Sounds() = default;

	void Sounds::start(bool muted)
	{
		if (m->started)
			return;
		m->device = std::make_unique<audio::Device>();
		m->muted = muted;
		m->device->setMasterVolume(muted ? 0.0f : 1.0f);
		m->lastUpdate = Clock::now();
		m->started = true;
	}

	bool Sounds::started() const { return m->started; }

	void Sounds::play(AudioScripts::Table table, int script, std::optional<Place> place)
	{
		if (!m->started || script < 0 || m_quiet)
			return;
		std::lock_guard lock(m->mutex);
		if (m->queue.size() < 256)
			m->queue.push_back({ table, script, place });
	}

	void Sounds::play(std::string_view tag, std::optional<Place> place)
	{
		if (!m->started || tag.empty())
			return;
		if (const int i = m->scripts.index(tag, AUDIOTAG_2DSCRIPT); i >= 0)
			play(AudioScripts::Script2D, i, std::nullopt);
		else if (const int j = m->scripts.index(tag, AUDIOTAG_3DSCRIPT); j >= 0)
			play(AudioScripts::Script3D, j, place);
	}

	void Sounds::setTalkMusic(int intro, int loop)
	{
		if (!m->started)
			return;
		std::lock_guard lock(m->mutex);
		m->talkChange = std::pair{ intro, loop };
	}

	uint64_t Sounds::playMovie(std::shared_ptr<const audio::Pcm> sound)
	{
		if (!m->started || !m->device->ok() || !sound)
			return 0;
		m->movie = m->device->play(std::move(sound), userGain(Kind::Music));
		return m->movie;
	}

	void Sounds::stopMovie(uint64_t voice)
	{
		if (!m->started || !voice)
			return;
		m->device->stop(voice);
		if (m->movie == voice)
			m->movie = 0;
	}

	void Sounds::setSoundscape(int soundscape)
	{
		if (!m->started)
			return;
		std::lock_guard lock(m->mutex);
		m->scapeChange = soundscape;
	}

	void Sounds::setTerritoryMusic(std::vector<std::pair<int, float>> music)
	{
		if (!m->started)
			return;
		std::lock_guard lock(m->mutex);
		m->territoryChange = std::move(music);
	}

	void Sounds::setVolumes(const float volumes[6])
	{
		for (int i = 0; i < 6; ++i)
			g_volumes[i] = std::clamp(volumes[i], 0.0f, 1.0f);
	}

	void Sounds::update(const Listener& listener, const std::string& eraType)
	{
		if (!m->started || !m->device->ok())
			return;
		if (const float master = m->muted ? 0.0f : float(g_volumes[0]); master != m->master)
		{
			m->master = master;
			m->device->setMasterVolume(master);
		}
		const auto now = Clock::now();
		const float dt = std::chrono::duration<float>(now - m->lastUpdate).count();
		m->lastUpdate = now;
		std::vector<Impl::Request> queue;
		std::optional<std::pair<int, int>> talkChange;
		std::optional<int> scapeChange;
		std::optional<std::vector<std::pair<int, float>>> territoryChange;
		{
			std::lock_guard lock(m->mutex);
			queue.swap(m->queue);
			talkChange = std::exchange(m->talkChange, std::nullopt);
			scapeChange = std::exchange(m->scapeChange, std::nullopt);
			if (m->territoryChange)
				m->territoryChange.swap(territoryChange);
		}
		// (one the camera stays over a moment: passing over a plot does not start its)
		if (scapeChange && *scapeChange != m->wantScape)
			m->wantScape = *scapeChange, m->wantSince = now;
		if (m->wantScape != m->scape.index && now - m->wantSince > std::chrono::milliseconds(800))
			m->startScape(m->wantScape, now);
		m->updateScapes(listener, now, dt);
		m->device->update();
		// A talk begins or ends: the music playing fades out for the next.
		if (talkChange)
		{
			const bool talking = talkChange->first >= 0 || talkChange->second >= 0;
			if (talking ? m->talk != talkChange : m->talk.has_value())
			{
				m->talk = talking ? talkChange : std::nullopt;
				m->introDone = false;
				m->pending.reset();
				if (m->music)
					m->fadingOut = true;
				if (!talking)
					m->nextTrack = now + std::chrono::seconds(1);
			}
		}
		m->updateTerritory(now, dt, m->talk.has_value() || (m->movie && m->device->playing(m->movie)), territoryChange);
		m->updateMusic(eraType, now, dt);
		// The effects decoded since they were asked for (late ones are dropped).
		std::erase_if(m->waiting, [&](const Impl::Waiting& w) {
			if (now - w.since > std::chrono::seconds(2))
				return true;
			const auto pcm = m->sound(w.file);
			if (!pcm)
				return false;
			if (*pcm)
				if (const AudioScripts::Script* s = m->scripts.script(w.request.table, w.request.script))
					m->playEffect(w.request, *s, *pcm, listener);
			return true;
		});
		for (const Impl::Request& r : queue)
			m->request(r, listener, now);
	}
}
