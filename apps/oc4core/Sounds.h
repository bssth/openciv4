#pragma once

// The game's sounds and music, played: the audio scripts the DLL and the Python scripts ask
// for (2D ones as they are, 3D ones around the camera), the era's soundtracks one after
// another, and a leader's music while talking with them.
//
// Requests come from any thread (the game runs on its own); the window's thread plays them
// in update(), where the camera is. Nothing plays until start(): a scripted run (tests,
// screenshots) stays silent.

#include "Audio.h"
#include "audio/Audio.h"

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oc4::host
{
	class Sounds
	{
	public:
		Sounds(const AssetResolver& assets, AudioScripts& scripts);
		~Sounds();

		// The window's thread. Muted: everything runs (and logs) but nothing is heard.
		void start(bool muted);
		bool started() const;

		// Any thread. A 3D script at a place on the map (plot units: plot x spans [x, x + 1)),
		// a 2D one or a 3D one without a place as is.
		struct Place
		{
			float x = 0, y = 0;
		};
		void play(AudioScripts::Table table, int script, std::optional<Place> place = std::nullopt);
		// By tag: AS2D_..., AS3D_... (either table).
		void play(std::string_view tag, std::optional<Place> place = std::nullopt);
		// While set, what is asked for is not played (the game's thread, checking a popup that is
		// up again: its script's focus sound is for when it comes up).
		void setQuiet(bool quiet) { m_quiet = quiet; }
		// A talk's music (2D scripts): the intro once, then the loop; -1 for both: the era's again.
		void setTalkMusic(int intro, int loop);
		// The window's thread: a movie's sound (its voice, 0: none), the music silent while it plays.
		uint64_t playMovie(std::shared_ptr<const audio::Pcm> sound);
		void stopMovie(uint64_t voice);
		// The soundscape heard where the camera looks (an index of the soundscape table; -1:
		// none): its bed loops, its other sounds come now and then around the camera. Another
		// fades the last one out.
		void setSoundscape(int soundscape);
		// The leaders' music heard where the camera looks (the original's first era: over a
		// civilization's land its leader's peace music, the era's soundtrack elsewhere): each a
		// 2D script with how much of the view is theirs (0..1, together at most 1). Each loops,
		// as loud as its share, the soundtrack as the rest; none: the soundtrack alone.
		void setTerritoryMusic(std::vector<std::pair<int, float>> music);
		// Any thread: the player's volumes, 0..1 (the options screen's: master, music, effects,
		// speech, ambience, interface; a kind "no sound": 0).
		static void setVolumes(const float volumes[6]);

		// The window's thread, each frame: where the camera looks (plot units) and from how far
		// (plots), the active player's era (its type, "ERA_ANCIENT") for the soundtrack.
		struct Listener
		{
			float x = 0, y = 0, distance = 9;
		};
		void update(const Listener& listener, const std::string& eraType);

	private:
		struct Impl;
		std::unique_ptr<Impl> m;
		std::atomic<bool> m_quiet = false;
	};
}
