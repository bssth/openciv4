#pragma once
#include <algorithm>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace oc4::host::pb
{
using Bytes = std::vector<std::byte>;
using Ini = std::map<std::string, std::string>;
Ini readIni(const std::filesystem::path &file);
std::string value(const Ini &ini, const std::string &key, const std::string &fallback = {});
struct Config
{
	Ini values;
	std::filesystem::path state;
	std::string address = "127.0.0.1";
	int port = 2056;
	int64_t durationMs = 86400000;
	std::vector<int> humans;
	static Config read(const std::filesystem::path &file);
};
struct Frame
{
	uint32_t number = 0, connected = 0;
	int remainingTicks = -1;
	struct Order
	{
		int seat;
		uint64_t sequence;
		Bytes command;
	};
	std::vector<Order> orders;
};
struct World
{
	virtual ~World() = default;
	virtual void create(const Config &) = 0;
	virtual void load(const std::filesystem::path &) = 0;
	virtual void save(const std::filesystem::path &) = 0;
	virtual int turn() const = 0;
	virtual uint32_t checksum() const = 0;
	virtual bool validate(int seat, const Bytes &) const = 0;
	virtual void step(const Frame &) = 0;
	virtual void replaceAI(int seat) = 0;
	virtual std::string runtimeState() const { return {}; }
	virtual void runtimeState(const std::string &) {}
};
// Explicit time arguments make timer and recovery semantics testable without sleeps.
struct Timer
{
	int64_t duration = 86400000, remaining = 86400000;
	bool paused = false;
	void advance(int64_t elapsed)
	{
		if (!paused && duration > 0)
			remaining = std::max<int64_t>(0, remaining - elapsed);
	}
	void reset() { remaining = duration; }
	int ticks() const { return duration == 0 ? -1 : int((remaining + 249) / 250); }
};
void serve(const Config &, World &);
std::string admin(const std::filesystem::path &state, const std::string &command);
std::string passwordPrompt();
std::string credential(const std::string &password);
void setCredential(const std::filesystem::path &state, int seat, const std::string &password);

struct Welcome
{
	int seat = -1;
	uint32_t frame = 0, checksum = 0;
	int remainingTicks = -1;
	std::vector<int> humans;
	Bytes save;
	std::string runtimeState;
};
Welcome join(const std::string &address, int seat, const std::string &password);
void ready(uint32_t checksum);
bool clientActive();
bool takeFrame(Frame &);
void submit(Bytes);
void executed(uint32_t frame, uint32_t checksum);
void disconnect();
std::string status();
void identity(std::string);
// Replicated connection IDs, independent of process-local socket descriptors.
void connections(uint32_t mask);
bool connected(int seat);
} // namespace oc4::host::pb
