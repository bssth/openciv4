#undef NDEBUG
#include "Pitboss.h"
#include <arpa/inet.h>
#include <cassert>
#include <csignal>
#include <fstream>
#include <sstream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace oc4::host;
namespace fs = std::filesystem;
struct FakeWorld : pb::World
{
	int turns = 0, orders = 0;
	void create(const pb::Config &) override {}
	void load(const fs::path &p) override { std::ifstream(p) >> turns >> orders; }
	void save(const fs::path &p) override { std::ofstream(p) << turns << ' ' << orders; }
	int turn() const override { return turns; }
	uint32_t checksum() const override { return turns * 1000 + orders; }
	bool validate(int seat, const pb::Bytes &b) const override { return (seat == 0 || seat == 1) && b == pb::Bytes{std::byte{42}}; }
	void step(const pb::Frame &f) override
	{
		for (auto &o : f.orders)
			orders += uint8_t(o.command.at(0));
		if (f.remainingTicks == 0)
			++turns;
	}
	void replaceAI(int) override {}
	void snapshot(const pb::Bytes &b)
	{
		std::string s(reinterpret_cast<const char *>(b.data()), b.size());
		std::istringstream(s) >> turns >> orders;
	}
};
int main()
{
	alarm(60);
	pb::Timer t;
	t.duration = t.remaining = 1000;
	t.advance(400);
	assert(t.remaining == 600 && t.ticks() == 3);
	t.paused = true;
	t.advance(9999);
	assert(t.remaining == 600);
	t.paused = false;
	t.advance(600);
	assert(t.ticks() == 0);
	t.reset();
	assert(t.remaining == 1000);
	t.duration = 0;
	t.reset();
	t.advance(9999);
	assert(t.ticks() == -1);
	assert(pb::credential("secret") != pb::credential("secret"));
	const fs::path state = fs::temp_directory_path() / ("oc4-pitboss-test-" + std::to_string(getpid()));
	fs::create_directory(state);
	struct Cleanup
	{
		fs::path p;
		~Cleanup() { fs::remove_all(p); }
	} cleanup{state};
	pb::Config c;
	c.state = state;
	c.humans = {0, 1};
	c.durationMs = 60000;
	int probe = socket(AF_INET, SOCK_STREAM, 0);
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	assert(!bind(probe, reinterpret_cast<sockaddr *>(&a), sizeof a));
	socklen_t n = sizeof a;
	assert(!getsockname(probe, reinterpret_cast<sockaddr *>(&a), &n));
	c.port = ntohs(a.sin_port);
	close(probe);
	pb::setCredential(state, 0, "secret");
	pb::setCredential(state, 1, "other-secret");
	auto launch = [&]
	{
		pid_t pid = fork();
		assert(pid >= 0);
		if (!pid)
		{
			alarm(55);
			try
			{
				FakeWorld w;
				pb::serve(c, w);
				_exit(0);
			}
			catch (const std::exception &e)
			{
				fprintf(stderr, "server: %s\n", e.what());
				_exit(1);
			}
		}
		for (int i = 0; i < 500 && !fs::exists(state / "admin.sock"); ++i)
			usleep(10000);
		assert(fs::exists(state / "admin.sock"));
		return pid;
	};
	auto stop = [&](pid_t pid)
	{
		assert(pb::admin(state, "stop") == "OK");
		int status;
		assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	};
	pid_t pid = launch();
	bool refused = false;
	try
	{
		pb::join("127.0.0.1:" + std::to_string(c.port), 0, "wrong");
	}
	catch (...)
	{
		refused = true;
	}
	assert(refused);
	pb::identity("incompatible-ruleset");
	refused = false;
	try
	{
		pb::join("127.0.0.1:" + std::to_string(c.port), 0, "secret");
	}
	catch (...)
	{
		refused = true;
	}
	assert(refused);
	pb::identity("openciv4-pitboss-1");
	FakeWorld mirror;
	auto welcome = pb::join("127.0.0.1:" + std::to_string(c.port), 0, "secret");
	mirror.snapshot(welcome.save);
	assert(mirror.checksum() == welcome.checksum);
	pb::ready(mirror.checksum());
	// Another process cannot take an occupied seat, even with its correct password.
	pid_t duplicate = fork();
	assert(duplicate >= 0);
	if (!duplicate)
	{
		try
		{
			pb::join("127.0.0.1:" + std::to_string(c.port), 0, "secret");
			_exit(1);
		}
		catch (...)
		{
			_exit(0);
		}
	}
	int status;
	assert(waitpid(duplicate, &status, 0) == duplicate && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	// A late second client synchronizes at a common boundary and its order reaches both.
	pid_t second = fork();
	assert(second >= 0);
	if (!second)
	{
		try
		{
			auto w = pb::join("127.0.0.1:" + std::to_string(c.port), 1, "other-secret");
			FakeWorld other;
			other.snapshot(w.save);
			pb::ready(other.checksum());
			pb::submit({std::byte{42}});
			while (other.orders != 42)
			{
				pb::Frame f;
				if (pb::takeFrame(f))
				{
					other.step(f);
					pb::executed(f.number, other.checksum());
				}
				usleep(5000);
			}
			pb::disconnect();
			_exit(0);
		}
		catch (...)
		{
			_exit(1);
		}
	}
	while (waitpid(second, &status, WNOHANG) == 0)
	{
		pb::Frame f;
		if (pb::takeFrame(f))
		{
			mirror.step(f);
			pb::executed(f.number, mirror.checksum());
		}
		usleep(5000);
	}
	assert(WIFEXITED(status) && WEXITSTATUS(status) == 0 && mirror.orders == 42);
	pb::submit({std::byte{42}});
	for (int i = 0; i < 1000 && mirror.orders != 84; ++i)
	{
		pb::Frame f;
		if (pb::takeFrame(f))
		{
			mirror.step(f);
			pb::executed(f.number, mirror.checksum());
		}
		usleep(5000);
	}
	assert(mirror.orders == 84);
	pb::disconnect();
	// The order is only in the fsynced journal, not a checkpoint. Kill before a save.
	std::string current;
	std::getline(std::ifstream(state / "CURRENT"), current);
	FakeWorld checkpoint;
	checkpoint.load(state / current / "game.CivBeyondSwordSave");
	assert(checkpoint.orders == 0 && fs::file_size(state / "journal") > 0);
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	fs::remove(state / "admin.sock");
	// A torn final journal record must not erase the last complete record.
	{
		std::ofstream f(state / "journal", std::ios::binary | std::ios::app);
		f << "bad";
	}
	pid = launch();
	welcome = pb::join("127.0.0.1:" + std::to_string(c.port), 0, "secret");
	mirror.snapshot(welcome.save);
	assert(mirror.orders == 84);
	pb::ready(mirror.checksum());
	pb::disconnect();
	assert(pb::admin(state, "pause") == "OK");
	auto before = pb::admin(state, "status");
	usleep(50000);
	assert(pb::admin(state, "status") == before);
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	fs::remove(state / "admin.sock");
	pid = launch();
	assert(pb::admin(state, "status").find("paused=1") != std::string::npos);
	assert(pb::admin(state, "status") == before); // downtime did not consume the timer
	assert(pb::admin(state, "replace-ai 1") == "OK");
	refused = false;
	try
	{
		pb::admin(state, "replace-ai 0");
	}
	catch (...)
	{
		refused = true;
	}
	assert(refused);
	assert(pb::admin(state, "password 0 changed-secret") == "OK");
	refused = false;
	try
	{
		pb::join("127.0.0.1:" + std::to_string(c.port), 0, "secret");
	}
	catch (...)
	{
		refused = true;
	}
	assert(refused);
	welcome = pb::join("127.0.0.1:" + std::to_string(c.port), 0, "changed-secret");
	mirror.snapshot(welcome.save);
	assert(welcome.humans == std::vector<int>{0} && mirror.orders == 84);
	pb::ready(mirror.checksum());
	assert(pb::admin(state, "kick 0") == "OK");
	refused = false;
	for (int i = 0; i < 100 && !refused; ++i)
	{
		try
		{
			pb::Frame f;
			pb::takeFrame(f);
		}
		catch (...)
		{
			refused = true;
		}
		usleep(5000);
	}
	assert(refused);
	assert(pb::admin(state, "resume") == "OK");
	assert(pb::admin(state, "timer 0") == "OK");
	stop(pid);
	puts("PitBoss authentication, occupied seats, transport, pause, journal replay, credentials, roster and shutdown passed");
}
