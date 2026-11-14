#pragma once

// Starting and running a game the way the original engine drives the DLL: fill
// CvInitCore (the staging room), create game, map, teams and players, run the map
// script, place the starting units and hand out the first turn.
//
// Like everything in the host this only uses what the DLL exports.

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>

#include "NewGameSettings.h"

class CvDLLFAStarIFaceBase;

namespace oc4::host
{
	class PythonHost;
	struct SaveFile;
	class Utility;

	class GameSession
	{
	public:
		GameSession(Utility& utility, PythonHost& python) : m_utility(utility), m_python(python) {}

		// Once per process, after the XML is loaded: pathfinders and the Python "Init" event.
		void initEngine();

		void startNewGame(const NewGameSettings& settings);
		// A scenario (mapScript a WorldBuilder save, .CivBeyondSwordWBSave): its game, players,
		// map and their things, as CvWBInterface reads them; the human the one of
		// settings.civilization (else the first playable).
		void startScenario(const NewGameSettings& settings);

		// AI plays `turns` turns (player 0 watches, as with CyGame.setAIAutoPlay);
		// onTurn runs after each new turn has begun.
		void autoPlay(int turns, const std::function<void(int turn)>& onTurn = {});
		// The same in steps, for a host that does other things in between (the map view):
		// stepAutoPlay is one engine update and tells whether a new turn began.
		void beginAutoPlay(int turns);
		// One engine update outside autoplay: the queued messages (orders), then the game.
		void update();
		bool stepAutoPlay();
		bool autoPlayDone() const;
		void endAutoPlay();

		// .CivBeyondSwordSave files, compatible with the original engine's (see SaveFile.h).
		void saveGame(const std::filesystem::path& path);
		// A multiplayer game, after every player loaded the same save: the human seats (the rest
		// AI players), simultaneous turns, this game's own seat.
		void setupMultiplayer(const std::vector<int>& humans, int seat);
		void loadGame(const std::filesystem::path& path);
		// Throws what loadGame would before it changes anything: not a save, another mod's.
		void checkSave(const std::filesystem::path& path) const;
		// Mod folder recorded in saves, e.g. "Mods\Rise of Mankind\" (empty without a mod).
		void setModFolder(std::string folder) { m_modFolder = std::move(folder); }

		// Development aid: reads a save step by step and reports where each part ends.
		void probeSave(const std::filesystem::path& path);

	private:
		void setupScenarioInitCore(const NewGameSettings& s);
		void setupInitCore(const NewGameSettings& settings);
		void activateFirstTurn();
		void readInitCore(const SaveFile& save);

		Utility& m_utility;
		PythonHost& m_python;
		std::string m_modFolder;

		struct AutoPlay
		{
			int turn = 0, endTurn = 0;
			long long updates = 0, sinceTurn = 0;
			bool stalled = false;
			std::chrono::steady_clock::time_point turnStart;
			double cpuStart = 0;
		} m_autoPlay;
	};
}
