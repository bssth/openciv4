#pragma once

// What a new game is started with: the menu's choices or the command line's, read into the
// init core by GameSession::setupInitCore. Plain data and the parsing of the command line's
// forms of it, so a test can check them without the game.

#include <cctype>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oc4::host
{
	// One player slot changed from the usual setup (slot 0 the human, then AIs).
	struct SlotSetting
	{
		enum class Status
		{
			Human, AI, Closed
		};
		Status status = Status::AI;
		std::string civilization, leader; // CIVILIZATION_..., LEADER_...; empty: at random
		int team = -1;                    // -1: a team of its own
	};

	struct NewGameSettings
	{
		std::string mapScript = "Continents"; // module name of a PublicMaps script
		std::string worldSize = "WORLDSIZE_SMALL";
		std::string climate = "CLIMATE_TEMPERATE";
		std::string seaLevel = "SEALEVEL_MEDIUM";
		std::string era = "ERA_ANCIENT";
		std::string gameSpeed = "GAMESPEED_NORMAL";
		std::string handicap = "HANDICAP_NOBLE";
		std::string civilization, leader; // the human's (CIVILIZATION_..., LEADER_...); empty: as the others, at random
		int players = 0; // 0: the world size's default
		unsigned int seed = 1;
		// The rest: empty keeps what the game has without them.
		std::vector<std::pair<std::string, bool>> options; // GAMEOPTION_..., on or off (else the XML's default)
		std::vector<std::string> victories;                 // VICTORY_... that are on (empty: all)
		std::map<int, int> mapOptions;                      // the map script's option -> its value (else its default)
		std::map<int, SlotSetting> slots;                   // slot -> how it differs
		int advancedStartPoints = -1; // with Advanced Start (-1: the original's Custom Game's default, 600)
	};

	namespace setup
	{
		inline std::string upper(std::string s)
		{
			for (char& c : s)
				c = char(std::toupper(static_cast<unsigned char>(c)));
			return s;
		}

		// "A,B,C" -> its parts, empty ones left out.
		inline std::vector<std::string> splitList(const std::string& s, char by = ',')
		{
			std::vector<std::string> parts;
			size_t from = 0;
			for (;;)
			{
				const size_t to = s.find(by, from);
				if (std::string part = s.substr(from, to == std::string::npos ? std::string::npos : to - from); !part.empty())
					parts.push_back(std::move(part));
				if (to == std::string::npos)
					return parts;
				from = to + 1;
			}
		}

		inline int number(const std::string& s, const char* what)
		{
			if (s.empty() || s.find_first_not_of("0123456789") != std::string::npos || s.size() > 6)
				throw std::runtime_error(std::string("not a number for ") + what + ": " + s);
			return std::stoi(s);
		}

		// Adds a prefix the type names share when it is left out: "NO_EVENTS" -> "GAMEOPTION_NO_EVENTS".
		inline std::string typeName(std::string s, const std::string& prefix)
		{
			s = upper(std::move(s));
			return s.starts_with(prefix) ? s : prefix + s;
		}

		// --option NAME, NAME=1 or NAME=0 (on, on, off).
		inline std::pair<std::string, bool> parseOption(const std::string& s)
		{
			const size_t eq = s.find('=');
			std::string name = typeName(s.substr(0, eq), "GAMEOPTION_");
			if (eq == std::string::npos)
				return { name, true };
			const std::string v = s.substr(eq + 1);
			if (v != "0" && v != "1")
				throw std::runtime_error("--option wants NAME, NAME=1 or NAME=0: " + s);
			return { name, v == "1" };
		}

		// --victories A,B (VICTORY_ may be left out); "none" for none at all.
		inline std::vector<std::string> parseVictories(const std::string& s)
		{
			if (upper(s) == "NONE")
				return { "NONE" };
			std::vector<std::string> v;
			for (const std::string& part : splitList(s))
				v.push_back(typeName(part, "VICTORY_"));
			if (v.empty())
				throw std::runtime_error("--victories wants a list: " + s);
			return v;
		}

		// --map-option N=V: the map script's option N (from 0) set to its value V (from 0).
		inline std::pair<int, int> parseMapOption(const std::string& s)
		{
			const size_t eq = s.find('=');
			if (eq == std::string::npos)
				throw std::runtime_error("--map-option wants N=V: " + s);
			return { number(s.substr(0, eq), "--map-option"), number(s.substr(eq + 1), "--map-option") };
		}

		// --slot N=human|ai|closed[:CIV[:LEADER[:TEAM]]], an empty part meaning random (the
		// team: its own); CIVILIZATION_ and LEADER_ may be left out.
		inline std::pair<int, SlotSetting> parseSlot(const std::string& s)
		{
			const size_t eq = s.find('=');
			if (eq == std::string::npos)
				throw std::runtime_error("--slot wants N=human|ai|closed[:CIV[:LEADER[:TEAM]]]: " + s);
			const int slot = number(s.substr(0, eq), "--slot");
			std::vector<std::string> parts;
			{
				const std::string rest = s.substr(eq + 1);
				size_t from = 0;
				for (;;)
				{
					const size_t to = rest.find(':', from);
					parts.push_back(rest.substr(from, to == std::string::npos ? std::string::npos : to - from));
					if (to == std::string::npos)
						break;
					from = to + 1;
				}
			}
			if (parts.size() > 4)
				throw std::runtime_error("--slot has too many parts: " + s);
			SlotSetting set;
			const std::string status = upper(parts[0]);
			if (status == "HUMAN")
				set.status = SlotSetting::Status::Human;
			else if (status == "AI")
				set.status = SlotSetting::Status::AI;
			else if (status == "CLOSED")
				set.status = SlotSetting::Status::Closed;
			else
				throw std::runtime_error("--slot status is human, ai or closed: " + s);
			if (set.status == SlotSetting::Status::Closed && parts.size() > 1)
				throw std::runtime_error("--slot: a closed slot has no civilization: " + s);
			if (parts.size() > 1 && !parts[1].empty())
				set.civilization = typeName(parts[1], "CIVILIZATION_");
			if (parts.size() > 2 && !parts[2].empty())
				set.leader = typeName(parts[2], "LEADER_");
			if (parts.size() > 3 && !parts[3].empty())
				set.team = number(parts[3], "--slot team");
			return { slot, set };
		}
	}
}
