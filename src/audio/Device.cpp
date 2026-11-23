#include "audio/Audio.h"

#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <unordered_map>

namespace oc4::audio
{
	namespace
	{
		constexpr int kRate = 48000;
	}

	struct Device::Impl
	{
		SDL_AudioStream* stream = nullptr;
		struct Playing
		{
			std::shared_ptr<const Pcm> sound;
			double position = 0; // in the sound's frames
			double step = 1;     // its frames a frame of the output
			bool loop = false, done = false;
			std::array<float, 4> gain{}; // each output's (left, right) from each of the sound's channels
		};
		std::mutex mutex; // (the voices are mixed on SDL's audio thread)
		std::unordered_map<Voice, Playing> voices;
		Voice next = 1;
		std::vector<float> mix;
		std::atomic<bool> mono{ false }; // (the player's speakers: one, both sides the same)

		static void route(Playing& p, float volume, float pan)
		{
			pan = std::clamp(pan, -1.0f, 1.0f);
			if (p.sound->channels == 1)
			{
				const float a = (pan + 1) * 0.25f * 3.14159265f;
				p.gain = { std::min(volume * std::cos(a) * 1.4142f, volume), 0, std::min(volume * std::sin(a) * 1.4142f, volume), 0 };
			}
			else
				p.gain = { volume * std::min(1.0f, 1 - pan), 0, 0, volume * std::min(1.0f, 1 + pan) };
		}

		void render(float* out, int frames)
		{
			std::fill(out, out + size_t(frames) * 2, 0.0f);
			const std::lock_guard lock(mutex);
			for (auto& [id, p] : voices)
			{
				const Pcm& s = *p.sound;
				const size_t channels = s.channels, length = s.samples.size() / channels;
				for (int f = 0; f < frames && !p.done; ++f)
				{
					if (p.position >= double(length))
					{
						if (!p.loop)
						{
							p.done = true;
							break;
						}
						p.position = std::fmod(p.position, double(length));
					}
					const size_t i = size_t(p.position), j = i + 1 < length ? i + 1 : p.loop ? 0 : i;
					const float t = float(p.position - double(i));
					for (size_t c = 0; c < channels; ++c)
					{
						const float v = (float(s.samples[i * channels + c]) * (1 - t) + float(s.samples[j * channels + c]) * t) / 32768.0f;
						out[f * 2] += v * p.gain[c];
						out[f * 2 + 1] += v * p.gain[2 + c];
					}
					p.position += p.step;
				}
			}
		}

		static void SDLCALL feed(void* self, SDL_AudioStream* stream, int additional, int)
		{
			auto& d = *static_cast<Impl*>(self);
			const int frames = additional / int(sizeof(float) * 2);
			if (frames <= 0)
				return;
			d.mix.resize(size_t(frames) * 2);
			d.render(d.mix.data(), frames);
			if (d.mono)
				for (int f = 0; f < frames; ++f)
					d.mix[size_t(f) * 2] = d.mix[size_t(f) * 2 + 1] = (d.mix[size_t(f) * 2] + d.mix[size_t(f) * 2 + 1]) * 0.5f;
			SDL_PutAudioStreamData(stream, d.mix.data(), frames * int(sizeof(float) * 2));
		}
	};

	std::vector<std::string> deviceNames(bool recording)
	{
		std::vector<std::string> out;
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
			return out;
		int count = 0;
		if (SDL_AudioDeviceID* ids = recording ? SDL_GetAudioRecordingDevices(&count) : SDL_GetAudioPlaybackDevices(&count))
		{
			for (int i = 0; i < count; ++i)
				if (const char* name = SDL_GetAudioDeviceName(ids[i]))
					out.emplace_back(name);
			SDL_free(ids);
		}
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
		return out;
	}

	Device::Device() : m(std::make_unique<Impl>())
	{
		if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
		{
			std::fprintf(stderr, "[oc4core] no sound: %s\n", SDL_GetError());
			return;
		}
		const SDL_AudioSpec spec{ SDL_AUDIO_F32, 2, kRate };
		m->stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, &Impl::feed, m.get());
		if (!m->stream)
		{
			std::fprintf(stderr, "[oc4core] no sound: no audio output device (%s)\n", SDL_GetError());
			SDL_QuitSubSystem(SDL_INIT_AUDIO);
			return;
		}
		SDL_ResumeAudioStreamDevice(m->stream);
	}

	Device::~Device()
	{
		if (!m->stream)
			return;
		SDL_DestroyAudioStream(m->stream);
		SDL_QuitSubSystem(SDL_INIT_AUDIO);
	}

	bool Device::ok() const { return m->stream != nullptr; }

	void Device::setMasterVolume(float volume)
	{
		if (m->stream)
			SDL_SetAudioStreamGain(m->stream, std::max(0.0f, volume));
	}

	void Device::setMono(bool mono) { m->mono = mono; }

	Device::Voice Device::play(std::shared_ptr<const Pcm> sound, float volume, float pan, float pitch, bool loop)
	{
		if (!m->stream || !sound || sound->samples.empty() || !sound->channels || sound->channels > 2 || !sound->rate)
			return 0;
		Impl::Playing p;
		p.step = double(sound->rate) * std::clamp(pitch, 0.25f, 4.0f) / kRate;
		p.loop = loop;
		p.sound = std::move(sound);
		Impl::route(p, volume, pan);
		const std::lock_guard lock(m->mutex);
		const Voice id = m->next++;
		m->voices.emplace(id, std::move(p));
		return id;
	}

	void Device::setVolume(Voice voice, float volume, float pan)
	{
		const std::lock_guard lock(m->mutex);
		if (const auto it = m->voices.find(voice); it != m->voices.end())
			Impl::route(it->second, volume, pan);
	}

	void Device::stop(Voice voice)
	{
		const std::lock_guard lock(m->mutex);
		m->voices.erase(voice);
	}

	bool Device::playing(Voice voice) const
	{
		const std::lock_guard lock(m->mutex);
		return m->voices.contains(voice);
	}

	void Device::update()
	{
		const std::lock_guard lock(m->mutex);
		std::erase_if(m->voices, [](const auto& e) { return e.second.done; });
	}
}
