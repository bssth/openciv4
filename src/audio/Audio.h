#pragma once

// Sound for the game: the sounds decoded to 16-bit PCM (WAV, plain or ADPCM as Civ4 keeps
// most of them, and MP3 through dr_mp3) and mixed for the default output device
// through SDL3.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oc4::audio
{
	struct Pcm
	{
		std::vector<int16_t> samples; // interleaved
		uint32_t rate = 0;
		uint16_t channels = 0;

		double seconds() const { return rate && channels ? double(samples.size()) / channels / rate : 0; }
	};

	// A RIFF WAVE file: PCM (8 or 16 bit), IMA ADPCM or Microsoft ADPCM.
	std::optional<Pcm> decodeWav(std::span<const std::byte> file, std::string* error = nullptr);
	// An MP3 file. Any thread.
	std::optional<Pcm> decodeMedia(std::span<const std::byte> file, std::string* error = nullptr);
	// By its first bytes: a WAVE file or else an MP3.
	std::optional<Pcm> decode(std::span<const std::byte> file, std::string* error = nullptr);

	// The names of the system's sound devices: its microphones (recording) or its outputs.
	std::vector<std::string> deviceNames(bool recording);

	// The output device. Its calls are the thread's that made it.
	class Device
	{
	public:
		Device();
		~Device();
		Device(const Device&) = delete;
		Device& operator=(const Device&) = delete;

		// False without an output device (then nothing plays).
		bool ok() const;
		void setMasterVolume(float volume);
		// Both sides the same (the player's speakers set to Mono).
		void setMono(bool mono);

		using Voice = uint64_t; // 0: none
		// Plays the sound from its start: volume as a gain (0..1), pan from -1 (left) to 1, pitch as
		// a frequency ratio; a looping one plays until stopped. The sound is kept while it plays.
		Voice play(std::shared_ptr<const Pcm> sound, float volume, float pan = 0, float pitch = 1, bool loop = false);
		void setVolume(Voice voice, float volume, float pan = 0);
		void stop(Voice voice);
		bool playing(Voice voice) const;
		// Lets go of the voices that have finished.
		void update();

	private:
		struct Impl;
		std::unique_ptr<Impl> m;
	};
}
