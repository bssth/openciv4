#include "Pitboss.h"
#ifdef __linux__
#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <charconv>
#include <cmath>
#include <csignal>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <limits>
#include <netdb.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <termios.h>
#include <unistd.h>

namespace oc4::host::pb
{
namespace fs = std::filesystem;
namespace
{
constexpr uint32_t Version = 1, MaxPacket = 256u << 20;
constexpr int Iterations = 600000;
using Clock = std::chrono::steady_clock;
enum Type : uint32_t
{
	Hello = 32,
	Challenge,
	Authenticate,
	Snapshot,
	Ready,
	Tick,
	Submit,
	Ack,
	Accepted,
	Info,
	Error
};
struct Writer
{
	Bytes b;
	void u(uint64_t n)
	{
		for (int i = 0; i < 8; ++i)
			b.push_back(std::byte(n >> (i * 8)));
	}
	void bytes(const Bytes &x)
	{
		u(x.size());
		b.insert(b.end(), x.begin(), x.end());
	}
	void str(const std::string &s)
	{
		bytes(Bytes(reinterpret_cast<const std::byte *>(s.data()), reinterpret_cast<const std::byte *>(s.data() + s.size())));
	}
};
struct Reader
{
	const Bytes &b;
	size_t at = 0;
	uint64_t u()
	{
		if (b.size() - at < 8)
			throw std::runtime_error("truncated PitBoss message");
		uint64_t n = 0;
		for (int i = 0; i < 8; ++i)
			n |= uint64_t(b[at++]) << (i * 8);
		return n;
	}
	Bytes bytes(size_t limit = MaxPacket)
	{
		auto n = u();
		if (n > limit || n > b.size() - at)
			throw std::runtime_error("PitBoss field too large");
		Bytes x(b.begin() + at, b.begin() + at + n);
		at += n;
		return x;
	}
	std::string str(size_t limit = 4096)
	{
		auto x = bytes(limit);
		return std::string(reinterpret_cast<const char *>(x.data()), x.size());
	}
	void end()
	{
		if (at != b.size())
			throw std::runtime_error("trailing PitBoss data");
	}
};
std::string hex(const unsigned char *p, size_t n)
{
	static const char *h = "0123456789abcdef";
	std::string out;
	for (size_t i = 0; i < n; ++i)
	{
		out += h[p[i] >> 4];
		out += h[p[i] & 15];
	}
	return out;
}
std::string hash(const Bytes &b)
{
	unsigned char d[32];
	unsigned n = 0;
	if (!EVP_Digest(b.data(), b.size(), d, &n, EVP_sha256(), nullptr))
		throw std::runtime_error("SHA256 failed");
	return hex(d, n);
}
std::string randomHex(int n)
{
	std::vector<unsigned char> b(n);
	if (RAND_bytes(b.data(), n) != 1)
		throw std::runtime_error("random generator failed");
	return hex(b.data(), b.size());
}
std::string derive(const std::string &password, const std::string &salt)
{
	unsigned char key[32];
	if (!PKCS5_PBKDF2_HMAC(password.data(), int(password.size()), reinterpret_cast<const unsigned char *>(salt.data()), int(salt.size()),
						   Iterations, EVP_sha256(), 32, key))
		throw std::runtime_error("password derivation failed");
	return hex(key, 32);
}
std::string proof(const std::string &key, const std::string &nonce, int seat)
{
	auto text = "openciv4-pitboss-v1:" + nonce + ":" + std::to_string(seat);
	unsigned char d[32];
	unsigned n = 0;
	HMAC(EVP_sha256(), key.data(), int(key.size()), reinterpret_cast<const unsigned char *>(text.data()), text.size(), d, &n);
	return hex(d, n);
}
bool equal(const std::string &a, const std::string &b)
{
	return a.size() == b.size() && CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}
// std::byte has no implicit char conversion.
Bytes rawFile(const fs::path &p)
{
	std::ifstream f(p, std::ios::binary | std::ios::ate);
	if (!f)
		throw std::runtime_error("cannot read " + p.string());
	auto n = f.tellg();
	if (n < 0 || n > MaxPacket)
		throw std::runtime_error("state file too large");
	Bytes b(size_t(n), std::byte{});
	f.seekg(0);
	f.read(reinterpret_cast<char *>(b.data()), n);
	if (!f)
		throw std::runtime_error("short state read");
	return b;
}
void writeAll(int fd, const void *p, size_t n)
{
	const char *s = static_cast<const char *>(p);
	while (n)
	{
		ssize_t got = ::write(fd, s, n);
		if (got < 0 && errno == EINTR)
			continue;
		if (got <= 0)
			throw std::runtime_error("state write failed");
		s += got;
		n -= got;
	}
}
void syncDir(const fs::path &p)
{
	int fd = open(p.c_str(), O_RDONLY | O_DIRECTORY);
	if (fd < 0)
		throw std::runtime_error("cannot open state directory");
	int rc = fsync(fd);
	close(fd);
	if (rc)
		throw std::runtime_error("cannot sync state directory");
}
void atomicFile(const fs::path &p, const Bytes &b)
{
	auto temp = p.string() + ".tmp";
	int fd = open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
	if (fd < 0)
		throw std::runtime_error("cannot write " + p.string());
	try
	{
		writeAll(fd, b.data(), b.size());
		if (fsync(fd))
			throw std::runtime_error("cannot sync state");
		close(fd);
		fd = -1;
		fs::rename(temp, p);
		syncDir(p.parent_path());
	}
	catch (...)
	{
		if (fd >= 0)
			close(fd);
		throw;
	}
}
void atomicText(const fs::path &p, const std::string &s)
{
	Bytes b;
	for (char c : s)
		b.push_back(std::byte(c));
	atomicFile(p, b);
}
std::string trim(std::string s)
{
	auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
	return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
int64_t number(const std::string &s, int64_t low, int64_t high)
{
	int64_t n;
	auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), n);
	if (e != std::errc{} || p != s.data() + s.size() || n < low || n > high)
		throw std::runtime_error("invalid numeric configuration");
	return n;
}
void privateState(const fs::path &p)
{
	fs::create_directories(p);
	struct stat st{};
	if (lstat(p.c_str(), &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid())
		throw std::runtime_error("state directory must be owned by this user and not a symlink");
	if (chmod(p.c_str(), 0700))
		throw std::runtime_error("cannot protect state directory");
}
struct FD
{
	int fd = -1;
	FD() = default;
	explicit FD(int f) : fd(f) {}
	~FD()
	{
		if (fd >= 0)
			close(fd);
	}
	FD(const FD &) = delete;
	FD &operator=(const FD &) = delete;
};
void nonblock(int fd)
{
	int f = fcntl(fd, F_GETFL);
	if (f < 0 || fcntl(fd, F_SETFL, f | O_NONBLOCK))
		throw std::runtime_error("nonblocking socket failed");
}
struct Peer
{
	FD socket;
	Bytes input, output;
	size_t sent = 0;
	int seat = -1;
	bool authenticated = false, sync = false, live = false, dead = false;
	bool closed = false, closing = false;
	size_t inputLimit = MaxPacket;
	uint32_t ack = 0;
	uint64_t lastSubmitted = 0;
	std::string nonce, salt, key;
	Clock::time_point since = Clock::now(), lastAck = since;
	explicit Peer(int fd) : socket(fd) { nonblock(fd); }
	void send(Type type, const Bytes &data)
	{
		if (data.size() > MaxPacket - 8 || output.size() - sent + data.size() > MaxPacket + 4096)
			throw std::runtime_error("peer output limit");
		Writer w;
		w.u(data.size() + 8);
		w.u(type);
		output.insert(output.end(), w.b.begin(), w.b.end());
		output.insert(output.end(), data.begin(), data.end());
	}
	void info(Type type, const std::string &s)
	{
		Writer w;
		w.str(s);
		send(type, w.b);
	}
	bool pump()
	{
		if (closed)
			return !input.empty();
		char buffer[65536];
		for (;;)
		{
			auto n = recv(socket.fd, buffer, sizeof buffer, MSG_DONTWAIT);
			if (n == 0)
			{
				closed = true;
				return !input.empty();
			}
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				closed = true;
				return !input.empty();
			}
			if (input.size() + n > inputLimit + 8)
			{
				closed = true;
				return false;
			}
			auto p = reinterpret_cast<std::byte *>(buffer);
			input.insert(input.end(), p, p + n);
		}
		while (sent < output.size())
		{
			auto n = ::send(socket.fd, output.data() + sent, output.size() - sent, MSG_NOSIGNAL | MSG_DONTWAIT);
			if (n < 0)
			{
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				closed = true;
				return !input.empty();
			}
			if (!n)
			{
				closed = true;
				return !input.empty();
			}
			sent += n;
		}
		if (sent == output.size())
		{
			output.clear();
			sent = 0;
		}
		return true;
	}
	void reject(const std::string &reason)
	{
		info(Error, reason);
		live = authenticated = sync = false;
		closing = true;
		since = Clock::now();
		pump();
		if (sent == output.size())
			shutdown(socket.fd, SHUT_WR);
	}
	bool packet(Type &type, Bytes &b)
	{
		if (input.size() < 8)
		{
			if (closed && !input.empty())
				throw std::runtime_error("truncated PitBoss packet");
			return false;
		}
		Reader r{input};
		auto n = r.u();
		if (n < 8 || n > inputLimit)
			throw std::runtime_error("invalid PitBoss packet length");
		if (input.size() < n + 8)
		{
			if (closed)
				throw std::runtime_error("truncated PitBoss packet");
			return false;
		}
		type = Type(r.u());
		b.assign(input.begin() + 16, input.begin() + 8 + n);
		input.erase(input.begin(), input.begin() + 8 + n);
		return true;
	}
};
int listener(const std::string &address, int port)
{
	addrinfo hint{}, *list = nullptr;
	hint.ai_socktype = SOCK_STREAM;
	hint.ai_flags = AI_PASSIVE;
	int rc = getaddrinfo(address.c_str(), std::to_string(port).c_str(), &hint, &list);
	if (rc)
		throw std::runtime_error("invalid listen address");
	int fd = -1;
	for (auto a = list; a; a = a->ai_next)
	{
		fd = socket(a->ai_family, SOCK_STREAM, 0);
		if (fd < 0)
			continue;
		int yes = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);
		if (!bind(fd, a->ai_addr, a->ai_addrlen) && !listen(fd, 16))
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(list);
	if (fd < 0)
		throw std::runtime_error("cannot listen");
	nonblock(fd);
	return fd;
}
int connectTCP(std::string address)
{
	auto colon = address.rfind(':');
	std::string port = "2056";
	if (colon != std::string::npos)
	{
		port = address.substr(colon + 1);
		address.resize(colon);
	}
	if (address.starts_with('[') && address.ends_with(']'))
		address = address.substr(1, address.size() - 2);
	addrinfo hint{}, *list = nullptr;
	hint.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(address.c_str(), port.c_str(), &hint, &list))
		throw std::runtime_error("unknown PitBoss host");
	int fd = -1;
	for (auto a = list; a; a = a->ai_next)
	{
		fd = socket(a->ai_family, SOCK_STREAM, 0);
		if (fd < 0)
			continue;
		nonblock(fd);
		if (!connect(fd, a->ai_addr, a->ai_addrlen))
			break;
		if (errno == EINPROGRESS)
		{
			pollfd poller{fd, POLLOUT, 0};
			int error = 1;
			socklen_t len = sizeof error;
			if (poll(&poller, 1, 5000) > 0 && !getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) && !error)
				break;
		}
		close(fd);
		fd = -1;
	}
	freeaddrinfo(list);
	if (fd < 0)
		throw std::runtime_error("cannot connect to PitBoss");
	return fd;
}
Bytes frameBytes(const Frame &f)
{
	Writer w;
	w.u(f.number);
	w.u(f.connected);
	w.u(uint64_t(int64_t(f.remainingTicks)));
	w.u(f.orders.size());
	for (auto &o : f.orders)
	{
		w.u(o.seat);
		w.u(o.sequence);
		w.bytes(o.command);
	}
	return w.b;
}
Frame parseFrame(const Bytes &b)
{
	Reader r{b};
	Frame f;
	f.number = uint32_t(r.u());
	f.connected = uint32_t(r.u());
	f.remainingTicks = int(int64_t(r.u()));
	auto n = r.u();
	if (n > 1024)
		throw std::runtime_error("too many orders");
	for (size_t i = 0; i < n; ++i)
	{
		Frame::Order o;
		o.seat = int(r.u());
		o.sequence = r.u();
		o.command = r.bytes(65536);
		f.orders.push_back(std::move(o));
	}
	r.end();
	return f;
}
std::unique_ptr<Peer> client;
std::deque<Frame> frames;
std::string clientStatus = "PitBoss disconnected", epoch;
bool clientReady = false;
uint64_t sequence = 0;
uint32_t expected = 0, connectionMask = 0;
std::string compatibility = "openciv4-pitboss-1";
Type waitPacket(Bytes &b)
{
	auto until = Clock::now() + std::chrono::seconds(30);
	while (Clock::now() < until)
	{
		if (!client->pump())
			throw std::runtime_error("PitBoss connection closed");
		Type t;
		if (client->packet(t, b))
		{
			if (t == Error)
			{
				Reader r{b};
				throw std::runtime_error(r.str());
			}
			return t;
		}
		poll(nullptr, 0, 2);
	}
	throw std::runtime_error("PitBoss handshake timed out");
}
volatile sig_atomic_t stopping = 0;
void stopSignal(int)
{
	stopping = 1;
}
} // namespace
Ini readIni(const fs::path &file)
{
	std::ifstream f(file);
	if (!f)
		throw std::runtime_error("cannot open " + file.string());
	Ini out;
	std::string section, line;
	while (std::getline(f, line))
	{
		line = trim(line);
		if (line.empty() || line[0] == '#' || line[0] == ';')
			continue;
		if (line[0] == '[' && line.back() == ']')
		{
			section = line.substr(1, line.size() - 2) + ".";
			continue;
		}
		auto eq = line.find('=');
		if (eq == std::string::npos)
			throw std::runtime_error("invalid INI line");
		if (!out.emplace(section + trim(line.substr(0, eq)), trim(line.substr(eq + 1))).second)
			throw std::runtime_error("duplicate INI key");
	}
	return out;
}
std::string value(const Ini &i, const std::string &k, const std::string &fallback)
{
	auto x = i.find(k);
	return x == i.end() ? fallback : x->second;
}
Config Config::read(const fs::path &file)
{
	Config c;
	c.values = readIni(file);
	c.address = value(c.values, "server.listen", "127.0.0.1");
	c.port = int(number(value(c.values, "server.port", "2056"), 1, 65535));
	auto hours = value(c.values, "server.turn_hours", "24");
	size_t used = 0;
	double h = std::stod(hours, &used);
	if (used != hours.size() || !std::isfinite(h) || h < 0 || h > 8760)
		throw std::runtime_error("turn_hours must be between 0 and 8760");
	c.durationMs = int64_t(h * 3600000);
	if (h > 0 && c.durationMs < 250)
		throw std::runtime_error("turn timer must be at least 250ms");
	auto state = value(c.values, "server.state_dir");
	if (state.empty())
	{
		const char *x = std::getenv("XDG_STATE_HOME");
		const char *home = std::getenv("HOME");
		if (!x && !home)
			throw std::runtime_error("set server.state_dir");
		state = (x ? fs::path(x) : fs::path(home) / ".local/state").string() + "/openciv4/pitboss/default";
	}
	c.state = fs::absolute(file.parent_path() / state);
	std::stringstream s(value(c.values, "game.humans", "0,1"));
	for (std::string part; std::getline(s, part, ',');)
	{
		int p = int(number(trim(part), 0, 17));
		if (std::find(c.humans.begin(), c.humans.end(), p) != c.humans.end())
			throw std::runtime_error("duplicate human seat");
		c.humans.push_back(p);
	}
	if (c.humans.empty())
		throw std::runtime_error("at least one human seat required");
	return c;
}
std::string passwordPrompt()
{
	termios old{}, hidden{};
	bool tty = tcgetattr(STDIN_FILENO, &old) == 0;
	if (tty)
	{
		hidden = old;
		hidden.c_lflag &= ~ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &hidden);
	}
	std::cerr << "Seat password: ";
	std::string p;
	std::getline(std::cin, p);
	if (tty)
	{
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &old);
		std::cerr << '\n';
	}
	if (p.empty() || p.size() > 1024)
		throw std::runtime_error("password must contain 1..1024 bytes");
	return p;
}
std::string credential(const std::string &password)
{
	if (password.empty() || password.size() > 1024)
		throw std::runtime_error("invalid password length");
	auto salt = randomHex(16);
	return salt + ":" + derive(password, salt);
}
void setCredential(const fs::path &state, int seat, const std::string &password)
{
	if (seat < 0 || seat > 17)
		throw std::runtime_error("invalid seat");
	privateState(state);
	FD lock(open((state / "server.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600));
	if (lock.fd < 0 || flock(lock.fd, LOCK_EX | LOCK_NB))
		throw std::runtime_error("server running: use admin password command");
	Ini i;
	if (fs::exists(state / "credentials.ini"))
		i = readIni(state / "credentials.ini");
	i[std::to_string(seat)] = credential(password);
	std::string out;
	for (auto &[k, v] : i)
		out += k + "=" + v + "\n";
	atomicText(state / "credentials.ini", out);
}
void connections(uint32_t mask)
{
	connectionMask = mask;
}
bool connected(int seat)
{
	return seat >= 0 && seat < 18 && (connectionMask & (1u << seat));
}
void identity(std::string id)
{
	compatibility = std::move(id);
}

struct Store
{
	fs::path dir;
	FD lock;
	uint32_t frame = 0;
	std::string epoch = randomHex(16);
	Timer timer;
	std::vector<int> humans;
	std::map<int, uint64_t> sequences;
	std::string motd;
	std::string fingerprint;
	int generation = 0;
	explicit Store(const Config &c) : dir(c.state), humans(c.humans), fingerprint(hash(serializeConfig(c)))
	{
		privateState(dir);
		lock.fd = open((dir / "server.lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
		if (lock.fd < 0 || flock(lock.fd, LOCK_EX | LOCK_NB))
			throw std::runtime_error("PitBoss state already in use");
		timer.duration = timer.remaining = c.durationMs;
	}
	static Bytes serializeConfig(const Config &config)
	{
		Writer w;
		for (auto &[k, v] : config.values)
		{
			if (k.starts_with("game."))
			{
				w.str(k);
				w.str(v);
			}
		}
		w.u(config.durationMs);
		for (int seat : config.humans)
			w.u(seat);
		w.str(compatibility);
		return w.b;
	}
	Bytes meta(World &world)
	{
		Writer w;
		w.u(Version);
		w.str(fingerprint);
		w.u(frame);
		w.u(timer.duration);
		w.u(timer.remaining);
		w.u(timer.paused);
		w.str(motd);
		w.u(humans.size());
		for (int seat : humans)
		{
			w.u(seat);
			w.u(sequences[seat]);
		}
		w.str(world.runtimeState());
		return w.b;
	}
	void loadMeta(const Bytes &b)
	{
		Reader r{b};
		if (r.u() != Version || r.str() != fingerprint)
			throw std::runtime_error("checkpoint/config/ruleset mismatch");
		frame = uint32_t(r.u());
		timer.duration = int64_t(r.u());
		timer.remaining = int64_t(r.u());
		timer.paused = r.u() != 0;
		motd = r.str();
		auto n = r.u();
		if (n > 18)
			throw std::runtime_error("invalid roster");
		humans.clear();
		for (size_t i = 0; i < n; ++i)
		{
			int seat = int(r.u());
			if (seat < 0 || seat > 17)
				throw std::runtime_error("invalid checkpoint seat");
			humans.push_back(seat);
			sequences[seat] = r.u();
		}
		runtimeState = r.str(32768);
		r.end();
	}
	void checkpoint(World &world)
	{
		auto name = "checkpoint-" + std::to_string(++generation) + "-" + randomHex(4);
		auto target = dir / name;
		fs::create_directory(target);
		world.save(target / "game.CivBeyondSwordSave");
		auto game = rawFile(target / "game.CivBeyondSwordSave");
		int fd = open((target / "game.CivBeyondSwordSave").c_str(), O_RDONLY);
		if (fd < 0 || fsync(fd))
		{
			if (fd >= 0)
				close(fd);
			throw std::runtime_error("save sync failed");
		}
		close(fd);
		auto metadata = meta(world);
		atomicFile(target / "metadata", metadata);
		atomicText(target / "checksums", hash(game) + "\n" + hash(metadata) + "\n");
		syncDir(target);
		atomicText(dir / "CURRENT", name + "\n");
		atomicText(dir / "journal", "");
		// Keep the previous generation for manual inspection, without accumulating saves forever.
		for (const auto &entry : fs::directory_iterator(dir))
			if (entry.is_directory() && entry.path().filename().string().starts_with("checkpoint-") && entry.path() != target &&
				entry.path() != previousCheckpoint)
				fs::remove_all(entry.path());
		previousCheckpoint = target;
		checkpointFrame = frame;
	}
	void append(const Frame &f)
	{
		auto b = frameBytes(f);
		Writer w;
		w.bytes(b);
		w.str(hash(b));
		FD fd(open((dir / "journal").c_str(), O_WRONLY | O_APPEND | O_CREAT | O_NOFOLLOW, 0600));
		if (fd.fd < 0)
			throw std::runtime_error("journal open failed");
		writeAll(fd.fd, w.b.data(), w.b.size());
		if (fsync(fd.fd))
			throw std::runtime_error("journal sync failed");
	}
	bool recover(World &world)
	{
		if (!fs::exists(dir / "CURRENT"))
			return false;
		std::ifstream current(dir / "CURRENT");
		std::string name;
		std::getline(current, name);
		if (name.find('/') != std::string::npos || !name.starts_with("checkpoint-"))
			throw std::runtime_error("invalid checkpoint manifest");
		auto p = dir / name;
		auto game = rawFile(p / "game.CivBeyondSwordSave"), metadata = rawFile(p / "metadata");
		std::ifstream sums(p / "checksums");
		std::string a, b;
		std::getline(sums, a);
		std::getline(sums, b);
		if (a != hash(game) || b != hash(metadata))
			throw std::runtime_error("checkpoint checksum failed");
		loadMeta(metadata);
		previousCheckpoint = p;
		checkpointFrame = frame;
		world.load(p / "game.CivBeyondSwordSave");
		world.runtimeState(runtimeState);
		lastTurn = world.turn();
		if (fs::exists(dir / "journal"))
		{
			auto journal = rawFile(dir / "journal");
			Reader r{journal};
			size_t good = 0;
			while (r.at < journal.size())
			{
				size_t start = r.at;
				Bytes bytes;
				std::string sum;
				try
				{
					bytes = r.bytes(2u << 20);
					sum = r.str(64);
				}
				catch (const std::exception &)
				{
					r.at = start;
					break;
				}
				if (hash(bytes) != sum)
					throw std::runtime_error("journal checksum failed");
				auto f = parseFrame(bytes);
				if (f.number >= frame)
				{
					if (f.number != frame)
						throw std::runtime_error("journal frame gap");
					world.step(f);
					for (auto &o : f.orders)
						sequences[o.seat] = o.sequence;
					++frame;
					timer.remaining = f.remainingTicks < 0 ? 0 : int64_t(f.remainingTicks) * 250;
					if (world.turn() != lastTurn)
					{
						timer.reset();
						lastTurn = world.turn();
					}
				}
				good = r.at;
			}
			if (good < journal.size())
			{
				journal.resize(good);
				atomicFile(dir / "journal", journal);
			}
		}
		epoch = randomHex(16);
		return true;
	}
	int lastTurn = -1;
	uint32_t checkpointFrame = 0;
	fs::path previousCheckpoint;
	std::string runtimeState;
};
void serve(const Config &config, World &world)
{
	Store store(config);
	Ini credentials = readIni(config.state / "credentials.ini");
	for (int seat : config.humans)
	{
		auto s = value(credentials, std::to_string(seat));
		if (s.size() != 97 || s[32] != ':')
			throw std::runtime_error("missing credential for seat " + std::to_string(seat));
	}
	if (!store.recover(world))
	{
		world.create(config);
		store.lastTurn = world.turn();
		store.checkpoint(world);
	}
	store.lastTurn = world.turn();
	FD tcp(listener(config.address, config.port)), control(socket(AF_UNIX, SOCK_STREAM, 0));
	if (control.fd < 0)
		throw std::runtime_error("admin socket failed");
	auto socketPath = (store.dir / "admin.sock").string();
	if (socketPath.size() >= sizeof(sockaddr_un::sun_path))
		throw std::runtime_error("state path too long for admin socket");
	unlink(socketPath.c_str());
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	std::strcpy(addr.sun_path, socketPath.c_str());
	if (bind(control.fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) || chmod(socketPath.c_str(), 0600) || listen(control.fd, 4))
		throw std::runtime_error("admin listener failed");
	nonblock(control.fd);
	struct Cleanup
	{
		std::string path;
		~Cleanup() { unlink(path.c_str()); }
	} cleanup{socketPath};
	std::vector<std::unique_ptr<Peer>> peers, admins;
	std::vector<Frame::Order> pending;
	std::map<uint32_t, uint32_t> checks;
	stopping = 0;
	auto oldTerm = std::signal(SIGTERM, stopSignal), oldInt = std::signal(SIGINT, stopSignal);
	struct Signals
	{
		decltype(oldTerm) t, i;
		~Signals()
		{
			std::signal(SIGTERM, t);
			std::signal(SIGINT, i);
		}
	} signals{oldTerm, oldInt};
	auto last = Clock::now(), next = last;
	bool stop = false;
	auto rosterMask = [&]
	{
		uint32_t mask = 0;
		for (auto &p : peers)
			if (p->live && !p->dead)
				mask |= 1u << p->seat;
		return mask;
	};
	auto caughtUp = [&]
	{
		for (auto &p : peers)
			if (p->live && !p->dead && p->ack < store.frame)
				return false;
		return true;
	};
	auto save = [&] { store.checkpoint(world); };
	std::cout << "PitBoss listening " << config.address << ":" << config.port << ", turn " << world.turn() << ", frame " << store.frame
			  << "\n"
			  << std::flush;
	while (!stop && !stopping)
	{
		auto now = Clock::now();
		bool barrier = false;
		for (auto &p : peers)
			barrier |= p->authenticated && !p->live && !p->dead;
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last);
		if (!barrier)
			store.timer.advance(elapsed.count());
		last += elapsed; // retain sub-millisecond time instead of losing it every poll
		for (int listenerFd : {tcp.fd, control.fd})
			for (;;)
			{
				int fd = accept(listenerFd, nullptr, nullptr);
				if (fd < 0)
					break;
				auto p = std::make_unique<Peer>(fd);
				p->inputLimit = 131072; // clients only send small commands, never save files
				if (listenerFd == control.fd)
				{
					if (admins.size() < 4)
						admins.push_back(std::move(p));
				}
				else if (peers.size() < 32)
					peers.push_back(std::move(p));
			}
		for (auto &p : peers)
		{
			if (p->dead)
				continue;
			if (p->closing)
			{
				if (!p->pump() || now - p->since > std::chrono::seconds(1))
					p->dead = true;
				p->input.clear();
				if (p->sent == p->output.size())
					shutdown(p->socket.fd, SHUT_WR);
				continue;
			}
			try
			{
				if (!p->pump())
				{
					p->dead = true;
					continue;
				}
				if ((!p->live && now - p->since > std::chrono::seconds(30)) ||
					(p->live && p->ack < store.frame && now - p->lastAck > std::chrono::seconds(5)))
				{
					p->dead = true;
					continue;
				}
				Type type;
				Bytes data;
				while (p->packet(type, data))
				{
					Reader r{data};
					if (type == Hello && !p->authenticated && p->nonce.empty())
					{
						if (r.u() != Version || r.str() != compatibility)
							throw std::runtime_error("PitBoss protocol/game-core/ruleset mismatch");
						int seat = int(r.u());
						r.end();
						if (std::find(store.humans.begin(), store.humans.end(), seat) == store.humans.end())
							throw std::runtime_error("seat is not human");
						for (auto &other : peers)
							if (other.get() != p.get() && !other->dead && other->authenticated && other->seat == seat)
								throw std::runtime_error("seat is occupied");
						p->seat = seat;
						auto stored = value(credentials, std::to_string(seat));
						p->salt = stored.substr(0, 32);
						p->key = stored.substr(33);
						p->nonce = randomHex(32);
						Writer w;
						w.str(p->salt);
						w.str(p->nonce);
						p->send(Challenge, w.b);
					}
					else if (type == Authenticate && !p->nonce.empty() && !p->authenticated)
					{
						auto answer = r.str(64);
						r.end();
						if (!equal(answer, proof(p->key, p->nonce, p->seat)))
							throw std::runtime_error("authentication failed");
						for (auto &other : peers)
							if (other.get() != p.get() && !other->dead && other->authenticated && other->seat == p->seat)
								throw std::runtime_error("seat is occupied");
						p->key.clear();
						p->authenticated = true;
						p->since = now;
					}
					else if (type == Ready && p->sync && !p->live)
					{
						auto e = r.str(32);
						auto f = r.u(), sum = r.u();
						r.end();
						if (e != store.epoch || f != store.frame || sum != world.checksum())
							throw std::runtime_error("snapshot checksum mismatch; reconnect required");
						p->live = true;
						p->ack = store.frame;
						p->lastAck = now;
						p->lastSubmitted = store.sequences[p->seat];
						p->info(Info, store.timer.paused ? "PitBoss paused by operator" : "PitBoss connected: " + store.motd);
					}
					else if (type == Submit && p->live)
					{
						auto e = r.str(32);
						uint64_t seq = r.u();
						auto command = r.bytes(65536);
						r.end();
						size_t queuedBytes = command.size();
						for (auto &order : pending)
							queuedBytes += order.command.size();
						if (e != store.epoch || seq != p->lastSubmitted + 1 || pending.size() >= 1024 || queuedBytes > (1u << 20) ||
							!world.validate(p->seat, command))
							throw std::runtime_error("invalid, stale or unauthorized command");
						p->lastSubmitted = seq;
						pending.push_back({p->seat, seq, std::move(command)});
					}
					else if (type == Ack && p->live)
					{
						if (r.str(32) != store.epoch)
							throw std::runtime_error("stale epoch");
						auto f = r.u(), sum = r.u();
						r.end();
						if (f >= store.frame || f + 1 < p->ack)
							throw std::runtime_error("invalid frame acknowledgement");
						auto found = checks.find(uint32_t(f));
						if (found != checks.end() && found->second != sum)
							throw std::runtime_error("out of sync; reconnect to resynchronize");
						p->ack = uint32_t(f + 1);
						p->lastAck = now;
					}
					else
						throw std::runtime_error("unexpected PitBoss message");
				}
			}
			catch (const std::exception &e)
			{
				try
				{
					p->reject(e.what());
				}
				catch (...)
				{
					p->dead = true;
				}
			}
		}
		// Snapshotting waits for a frame boundary acknowledged by every existing client.
		barrier = false;
		for (auto &p : peers)
			barrier |= p->authenticated && !p->live && !p->dead;
		if (barrier && caughtUp())
		{
			for (auto &p : peers)
				if (p->authenticated && !p->live && !p->sync && !p->dead)
				{
					auto file = store.dir / "joining.CivBeyondSwordSave";
					world.save(file);
					Writer w;
					w.str(store.epoch);
					w.u(store.frame);
					w.u(p->seat);
					w.u(world.checksum());
					w.u(uint64_t(int64_t(store.timer.ticks())));
					w.u(store.sequences[p->seat]);
					w.u(store.humans.size());
					for (int seat : store.humans)
						w.u(seat);
					w.str(world.runtimeState());
					w.bytes(rawFile(file));
					p->send(Snapshot, w.b);
					p->sync = true;
				}
		}
		for (auto &p : admins)
		{
			try
			{
				if (p->dead)
					continue;
				if (!p->pump())
				{
					p->dead = true;
					continue;
				}
				Type type;
				Bytes b;
				if (p->packet(type, b))
				{
					Reader r{b};
					auto command = r.str(4096);
					r.end();
					if (type != Info)
						throw std::runtime_error("invalid admin message");
					std::istringstream words(command);
					std::string action;
					words >> action;
					std::string response = "OK";
					if (action == "status")
					{
						response = "turn=" + std::to_string(world.turn()) + " frame=" + std::to_string(store.frame) +
								   " remaining_ms=" + std::to_string(store.timer.remaining) +
								   " paused=" + std::to_string(store.timer.paused) + " connected_mask=" + std::to_string(rosterMask());
					}
					else if (action == "save")
						save();
					else if (action == "pause" || action == "resume")
					{
						store.timer.paused = action == "pause";
						save();
						for (auto &peer : peers)
							if (peer->live)
								peer->info(Info, store.timer.paused ? "PitBoss paused by operator" : "PitBoss resumed: " + store.motd);
					}
					else if (action == "timer")
					{
						std::string h;
						words >> h;
						size_t used = 0;
						double hours = std::stod(h, &used);
						if (used != h.size() || !std::isfinite(hours) || hours < 0 || hours > 8760 || (hours > 0 && hours * 3600000 < 250))
							throw std::runtime_error("invalid timer hours");
						store.timer.duration = int64_t(hours * 3600000);
						store.timer.reset();
						save();
					}
					else if (action == "kick" || action == "replace-ai" || action == "password")
					{
						int seat = -1;
						words >> seat;
						if (!words || std::find(store.humans.begin(), store.humans.end(), seat) == store.humans.end())
							throw std::runtime_error("invalid human seat");
						if (action == "replace-ai" && (!pending.empty() || !caughtUp()))
							throw std::runtime_error("retry after pending frame");
						if (action == "replace-ai" && store.humans.size() == 1)
							throw std::runtime_error("cannot replace the last human seat");
						if (action == "password")
						{
							std::string password;
							std::getline(words, password);
							if (!password.empty() && password.front() == ' ')
								password.erase(0, 1);
							credentials[std::to_string(seat)] = credential(password);
							std::string out;
							for (auto &[k, v] : credentials)
								out += k + "=" + v + "\n";
							atomicText(store.dir / "credentials.ini", out);
						}
						for (auto &peer : peers)
							if (peer->seat == seat)
								peer->reject("seat disconnected by operator; reconnect required");
						if (action == "replace-ai")
						{
							world.replaceAI(seat);
							std::erase(store.humans, seat);
							for (auto &peer : peers)
							{
								peer->reject("roster changed; reconnect");
							}
							store.epoch = randomHex(16);
							save();
						}
					}
					else if (action == "motd")
					{
						std::getline(words, store.motd);
						store.motd = trim(store.motd);
						save();
						for (auto &peer : peers)
							if (peer->live)
								peer->info(Info, store.motd);
					}
					else if (action == "stop")
					{
						save();
						stop = true;
					}
					else
						throw std::runtime_error("unknown admin command");
					p->info(Info, response);
					p->pump();
					p->dead = true;
				}
			}
			catch (const std::exception &e)
			{
				try
				{
					p->info(Error, e.what());
					p->pump();
				}
				catch (...)
				{
				}
				p->dead = true;
			}
		}
		std::erase_if(admins, [](auto &p) { return p->dead || Clock::now() - p->since > std::chrono::seconds(5); });
		// Orders from a dead connection were not acknowledged and must not run later.
		std::erase_if(
			pending, [&](auto &o)
			{ return std::none_of(peers.begin(), peers.end(), [&](auto &p) { return p->live && !p->dead && p->seat == o.seat; }); });
		std::erase_if(peers, [](auto &p) { return p->dead; });
		if (!stop && !store.timer.paused && !barrier && caughtUp() && now >= next)
		{
			Frame f;
			f.number = store.frame;
			f.connected = rosterMask();
			f.remainingTicks = store.timer.ticks();
			f.orders.swap(pending);
			store.append(f);
			world.step(f);
			for (auto &o : f.orders)
				store.sequences[o.seat] = o.sequence;
			checks[f.number] = world.checksum();
			while (checks.size() > 64)
				checks.erase(checks.begin());
			++store.frame;
			auto payload = frameBytes(f);
			for (auto &p : peers)
				if (p->live)
				{
					p->send(Tick, payload);
					for (auto &o : f.orders)
						if (o.seat == p->seat)
						{
							Writer w;
							w.u(o.sequence);
							w.u(f.number);
							p->send(Accepted, w.b);
						}
				}
			if (world.turn() != store.lastTurn)
			{
				store.timer.reset();
				store.lastTurn = world.turn();
				save();
				std::cout << "PitBoss turn " << world.turn() << " checksum " << world.checksum() << "\n" << std::flush;
			}
			else if (store.frame - store.checkpointFrame >= 240)
				save(); // bound the journal and replay work even when turns have no timer
			next = now + std::chrono::milliseconds(250);
		}
		poll(nullptr, 0, 5);
	}
	store.checkpoint(world);
	for (auto &p : peers)
	{
		try
		{
			p->info(Error, "server stopped; reconnect after restart");
			p->pump();
		}
		catch (...)
		{
		}
	}
}
std::string admin(const fs::path &state, const std::string &command)
{
	FD fd(socket(AF_UNIX, SOCK_STREAM, 0));
	sockaddr_un addr{};
	addr.sun_family = AF_UNIX;
	auto p = (state / "admin.sock").string();
	if (p.size() >= sizeof addr.sun_path)
		throw std::runtime_error("admin path too long");
	std::strcpy(addr.sun_path, p.c_str());
	if (connect(fd.fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr))
		throw std::runtime_error("cannot connect to admin socket");
	Peer peer(fd.fd);
	fd.fd = -1;
	Writer w;
	w.str(command);
	peer.send(Info, w.b);
	auto end = Clock::now() + std::chrono::seconds(10);
	while (Clock::now() < end)
	{
		bool alive = peer.pump();
		Type type;
		Bytes data;
		if (peer.packet(type, data))
		{
			Reader r{data};
			auto s = r.str();
			if (type == Error)
				throw std::runtime_error(s);
			return s;
		}
		if (!alive)
			break;
		poll(nullptr, 0, 2);
	}
	throw std::runtime_error("admin response timed out");
}
Welcome join(const std::string &address, int seat, const std::string &password)
{
	disconnect();
	if (password.empty() || password.size() > 1024 || seat < 0 || seat > 17)
		throw std::runtime_error("invalid seat or password length");
	client = std::make_unique<Peer>(connectTCP(address));
	try
	{
		Writer w;
		w.u(Version);
		w.str(compatibility);
		w.u(seat);
		client->send(Hello, w.b);
		Bytes data;
		if (waitPacket(data) != Challenge)
			throw std::runtime_error("expected authentication challenge");
		Reader challenge{data};
		auto salt = challenge.str(32), nonce = challenge.str(64);
		challenge.end();
		Writer answer;
		answer.str(proof(derive(password, salt), nonce, seat));
		client->send(Authenticate, answer.b);
		if (waitPacket(data) != Snapshot)
			throw std::runtime_error("expected snapshot");
		Reader r{data};
		epoch = r.str(32);
		Welcome welcome;
		welcome.frame = uint32_t(r.u());
		welcome.seat = int(r.u());
		welcome.checksum = uint32_t(r.u());
		welcome.remainingTicks = int(int64_t(r.u()));
		sequence = r.u();
		auto n = r.u();
		if (n > 18 || welcome.seat != seat)
			throw std::runtime_error("invalid roster");
		for (size_t i = 0; i < n; ++i)
			welcome.humans.push_back(int(r.u()));
		welcome.runtimeState = r.str(32768);
		welcome.save = r.bytes();
		r.end();
		expected = welcome.frame;
		clientStatus = "PitBoss synchronizing";
		return welcome;
	}
	catch (...)
	{
		disconnect();
		throw;
	}
}
void ready(uint32_t checksum)
{
	if (!client)
		throw std::runtime_error("not connected");
	Writer w;
	w.str(epoch);
	w.u(expected);
	w.u(checksum);
	client->send(Ready, w.b);
	Bytes data;
	if (waitPacket(data) != Info)
		throw std::runtime_error("expected ready confirmation");
	Reader r{data};
	clientStatus = r.str();
	clientReady = true;
}
bool clientActive()
{
	return bool(client) && clientReady;
}
bool takeFrame(Frame &f)
{
	if (!client)
		return false;
	try
	{
		if (!client->pump())
			throw std::runtime_error("PitBoss server disconnected; reconnect required");
		Type type;
		Bytes data;
		while (client->packet(type, data))
		{
			if (type == Tick)
			{
				auto frame = parseFrame(data);
				if (frame.number != expected++)
					throw std::runtime_error("PitBoss frame gap");
				frames.push_back(std::move(frame));
			}
			else if (type == Info)
			{
				Reader r{data};
				clientStatus = r.str();
			}
			else if (type == Error)
			{
				Reader r{data};
				throw std::runtime_error(r.str());
			}
			else if (type != Accepted)
				throw std::runtime_error("unexpected server message");
		}
		if (frames.empty())
			return false;
		f = std::move(frames.front());
		frames.pop_front();
		return true;
	}
	catch (...)
	{
		disconnect();
		throw;
	}
}
void submit(Bytes command)
{
	if (!client)
		throw std::runtime_error("PitBoss disconnected");
	Writer w;
	w.str(epoch);
	w.u(++sequence);
	w.bytes(command);
	client->send(Submit, w.b);
}
void executed(uint32_t frame, uint32_t checksum)
{
	if (!client)
		return;
	Writer w;
	w.str(epoch);
	w.u(frame);
	w.u(checksum);
	client->send(Ack, w.b);
	client->pump();
}
void disconnect()
{
	client.reset();
	clientReady = false;
	frames.clear();
	clientStatus = "PitBoss disconnected";
	connectionMask = 0;
}
std::string status()
{
	return clientStatus;
}
} // namespace oc4::host::pb
#endif
