#pragma once

// Multiplayer over TCP, as a lockstep: every player's orders (the game core's messages, the
// talks' events and deals, the players' options) go to the host, which puts them in numbered
// frames and sends each frame to everyone, itself included. Every game runs the orders of a
// frame and then one game update, so all of them stay the same game; now and then each one
// tells the host its checksum of the game, and the host compares them.
//
// Joining is loading the host's save: the host sends the file it loaded, each player loads it,
// and then all of them make the same players human (setupGame).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace oc4::host
{
	class Net
	{
	public:
		using Bytes = std::vector<std::byte>;

		struct Frame
		{
			uint32_t number = 0;
			std::vector<Bytes> commands;
			int pitbossTicks = -1;
			uint32_t connected = 0;
		};

		// What a player who joins gets: the seat, the human seats, the game.
		struct Welcome
		{
			int seat = -1;
			std::vector<int> humans;
			Bytes save;
		};

		static Net& get();

		bool active() const;
		bool pitboss() const;
		bool isHost() const { return m_host; }

		// The host: the port, then the others as they come (meanwhile `waiting` now and then, with
		// how many have come; it says false to give up), then their seats and the game.
		void listen(uint16_t port);
		void acceptPlayers(int count, const std::function<bool(int)>& waiting = {});
		void welcome(const std::vector<int>& seats, const std::vector<int>& humans, const Bytes& save);
		// A player who joins: the host's address ("name:port"), then the host's answer (blocking).
		void join(const std::string& address);
		Welcome awaitWelcome();
		// The frames begin (the host makes them from now on).
		void start();
		// The others are gone (the host left, or this game ends): no more frames.
		void stop();

		// An order of this player: to the host, for the next frame.
		void submit(Bytes command);
		// The game's next frame, when there is one (the game thread).
		bool takeFrame(Frame& frame);
		// A frame done; every kChecksumEvery-th with the game's checksum.
		static constexpr uint32_t kChecksumEvery = 30;
		void executed(uint32_t frame, int checksum);
		// While a frame's orders run: the game core's own messages then are every game's.
		bool inFrame() const { return m_inFrame; }
		void setInFrame(bool in) { m_inFrame = in; }

		// The host: players whose link broke, to be made AI players (in a frame of their own).
		std::vector<int> takeLostSeats();
		// For the window's caption: the players, a game out of sync, a lost host.
		std::string status() const;

	private:
		struct Link;
		Net() = default;
		void readLoop(Link& link);
		void produceLoop();
		void broadcast(uint8_t type, const Bytes& payload);
		void deliver(Frame frame);

		std::atomic<bool> m_active = false, m_host = false, m_running = false, m_inFrame = false;
		uintptr_t m_listener = ~uintptr_t(0);
		std::vector<std::unique_ptr<Link>> m_links; // the host's: one per other player; a player's: the host
		std::thread m_producer;

		mutable std::mutex m_mutex;
		std::vector<Bytes> m_pending;            // the host's: orders for the next frame
		std::deque<Frame> m_frames;              // frames to run
		uint32_t m_nextFrame = 0;                // the host's: the next frame's number
		uint32_t m_executed = 0;                 // frames this game has run
		std::vector<std::pair<uint32_t, int>> m_checksums; // the host's own, by frame
		std::vector<int> m_lostSeats;
		std::string m_problem;                   // out of sync, a lost link
		uint32_t m_matched = 0;                  // the host's: checksums that agreed
		std::deque<Welcome> m_welcomes;
	};
}
