#include "Net.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <stdexcept>
#include <utility>

#pragma comment(lib, "ws2_32.lib")

namespace oc4::host
{
	namespace
	{
		enum : uint8_t
		{
			kHello = 1,   // a player to the host: here I am
			kWelcome = 2, // the host to a player: your seat, the humans, the game
			kSubmit = 3,  // a player to the host: an order
			kFrame = 4,   // the host to everyone: a frame's orders
			kAck = 5,     // a player to the host: a frame run (with the checksum, now and then)
		};

		// (how many frames the host may run ahead of the slowest player)
		constexpr uint32_t kMaxLead = 8;
		constexpr auto kFrameTime = std::chrono::milliseconds(33);

		std::condition_variable g_welcomed;

		void startWinsock()
		{
			static const bool started = [] {
				WSADATA data;
				return WSAStartup(MAKEWORD(2, 2), &data) == 0;
			}();
			if (!started)
				throw std::runtime_error("multiplayer: no Winsock");
		}

		void put32(Net::Bytes& b, uint32_t v)
		{
			for (int i = 0; i < 4; ++i)
				b.push_back(std::byte((v >> (8 * i)) & 0xFF));
		}

		struct Cursor
		{
			const Net::Bytes& b;
			size_t at = 0;
			bool ok = true;
			uint32_t u32()
			{
				if (at + 4 > b.size())
				{
					ok = false;
					return 0;
				}
				uint32_t v = 0;
				for (int i = 0; i < 4; ++i)
					v |= uint32_t(b[at + i]) << (8 * i);
				at += 4;
				return v;
			}
			Net::Bytes bytes(size_t n)
			{
				if (at + n > b.size())
				{
					ok = false;
					return {};
				}
				Net::Bytes out(b.begin() + ptrdiff_t(at), b.begin() + ptrdiff_t(at + n));
				at += n;
				return out;
			}
		};

		bool sendAll(SOCKET s, const char* data, size_t size)
		{
			while (size > 0)
			{
				const int n = ::send(s, data, int(std::min<size_t>(size, 1 << 20)), 0);
				if (n <= 0)
					return false;
				data += n;
				size -= size_t(n);
			}
			return true;
		}

		bool recvAll(SOCKET s, char* data, size_t size)
		{
			while (size > 0)
			{
				const int n = ::recv(s, data, int(std::min<size_t>(size, 1 << 20)), 0);
				if (n <= 0)
					return false;
				data += n;
				size -= size_t(n);
			}
			return true;
		}
	}

	struct Net::Link
	{
		SOCKET socket = INVALID_SOCKET;
		int seat = -1;
		std::mutex sending;
		std::thread reader;
		std::atomic<bool> alive = true;
		std::atomic<uint32_t> acked = 0; // frames it has run

		bool send(uint8_t type, const Bytes& payload)
		{
			Bytes head;
			put32(head, uint32_t(payload.size() + 1));
			head.push_back(std::byte(type));
			std::lock_guard lock(sending);
			return alive && sendAll(socket, reinterpret_cast<const char*>(head.data()), head.size()) &&
				sendAll(socket, reinterpret_cast<const char*>(payload.data()), payload.size());
		}

		bool receive(uint8_t& type, Bytes& payload)
		{
			char head[4];
			if (!recvAll(socket, head, 4))
				return false;
			uint32_t size = 0;
			std::memcpy(&size, head, 4);
			if (size == 0 || size > (256u << 20))
				return false;
			Bytes all(size);
			if (!recvAll(socket, reinterpret_cast<char*>(all.data()), size))
				return false;
			type = uint8_t(all[0]);
			payload.assign(all.begin() + 1, all.end());
			return true;
		}
	};

	Net& Net::get()
	{
		static Net net;
		return net;
	}

	void Net::listen(uint16_t port)
	{
		startWinsock();
		const SOCKET s = ::socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
		if (s == INVALID_SOCKET)
			throw std::runtime_error("multiplayer: no socket");
		DWORD v6only = 0; // (IPv4 players too)
		setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, reinterpret_cast<const char*>(&v6only), sizeof(v6only));
		sockaddr_in6 addr{};
		addr.sin6_family = AF_INET6;
		addr.sin6_port = htons(port);
		addr.sin6_addr = in6addr_any;
		if (::bind(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 || ::listen(s, 8) != 0)
		{
			closesocket(s);
			throw std::runtime_error(std::format("multiplayer: cannot listen on port {}", port));
		}
		m_listener = uintptr_t(s);
		m_host = true;
	}

	void Net::acceptPlayers(int count, const std::function<bool(int)>& waiting)
	{
		for (int i = 0; i < count; ++i)
		{
			std::printf("  multiplayer: waiting for player %d of %d\n", i + 2, count + 1);
			for (;;)
			{
				fd_set ready;
				FD_ZERO(&ready);
				FD_SET(SOCKET(m_listener), &ready);
				timeval wait{ 0, 100000 };
				if (::select(0, &ready, nullptr, nullptr, &wait) > 0)
					break;
				if (waiting && !waiting(i))
					throw std::runtime_error("multiplayer: the host gave up waiting for the others");
			}
			const SOCKET s = ::accept(SOCKET(m_listener), nullptr, nullptr);
			if (s == INVALID_SOCKET)
				throw std::runtime_error("multiplayer: accept failed");
			const BOOL noDelay = TRUE;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
			auto link = std::make_unique<Link>();
			link->socket = s;
			uint8_t type = 0;
			Bytes payload;
			if (!link->receive(type, payload) || type != kHello)
			{
				closesocket(s);
				--i;
				continue;
			}
			std::printf("  multiplayer: player %d joined\n", i + 2);
			m_links.push_back(std::move(link));
		}
	}

	void Net::welcome(const std::vector<int>& seats, const std::vector<int>& humans, const Bytes& save)
	{
		for (size_t i = 0; i < m_links.size() && i < seats.size(); ++i)
		{
			Link& link = *m_links[i];
			link.seat = seats[i];
			Bytes b;
			put32(b, uint32_t(seats[i]));
			put32(b, uint32_t(humans.size()));
			for (const int h : humans)
				put32(b, uint32_t(h));
			put32(b, uint32_t(save.size()));
			b.insert(b.end(), save.begin(), save.end());
			if (!link.send(kWelcome, b))
				throw std::runtime_error("multiplayer: a player left before the game began");
		}
	}

	void Net::join(const std::string& address)
	{
		startWinsock();
		const size_t colon = address.rfind(':');
		const std::string name = colon == std::string::npos ? address : address.substr(0, colon);
		const std::string port = colon == std::string::npos ? "2056" : address.substr(colon + 1);
		addrinfo hints{}, *found = nullptr;
		hints.ai_socktype = SOCK_STREAM;
		hints.ai_protocol = IPPROTO_TCP;
		if (getaddrinfo(name.c_str(), port.c_str(), &hints, &found) != 0 || !found)
			throw std::runtime_error("multiplayer: no such host " + address);
		// (the host may still be loading its game: a minute of tries)
		SOCKET s = INVALID_SOCKET;
		for (int attempt = 0; attempt < 60 && s == INVALID_SOCKET; ++attempt)
		{
			if (attempt > 0)
				std::this_thread::sleep_for(std::chrono::seconds(1));
			for (addrinfo* a = found; a && s == INVALID_SOCKET; a = a->ai_next)
			{
				s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
				if (s != INVALID_SOCKET && ::connect(s, a->ai_addr, int(a->ai_addrlen)) != 0)
				{
					closesocket(s);
					s = INVALID_SOCKET;
				}
			}
		}
		freeaddrinfo(found);
		if (s == INVALID_SOCKET)
			throw std::runtime_error("multiplayer: cannot reach " + address);
		const BOOL noDelay = TRUE;
		setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
		auto link = std::make_unique<Link>();
		link->socket = s;
		link->send(kHello, {});
		m_links.push_back(std::move(link));
		m_host = false;
		Link& host = *m_links.back();
		host.reader = std::thread([this, &host] { readLoop(host); });
	}

	Net::Welcome Net::awaitWelcome()
	{
		std::unique_lock lock(m_mutex);
		g_welcomed.wait(lock, [&] { return !m_welcomes.empty() || (!m_links.empty() && !m_links.front()->alive); });
		if (m_welcomes.empty())
			throw std::runtime_error("multiplayer: the host closed the link");
		Welcome w = std::move(m_welcomes.front());
		m_welcomes.pop_front();
		return w;
	}

	void Net::start()
	{
		m_active = true;
		m_running = true;
		if (!m_host)
			return;
		for (auto& link : m_links)
		{
			Link& l = *link;
			l.reader = std::thread([this, &l] { readLoop(l); });
		}
		m_producer = std::thread([this] { produceLoop(); });
	}

	void Net::stop()
	{
		m_running = false;
		m_active = false;
		for (auto& link : m_links)
		{
			link->alive = false;
			shutdown(link->socket, SD_BOTH);
			closesocket(link->socket);
		}
		if (m_listener != ~uintptr_t(0))
			closesocket(SOCKET(m_listener));
		if (m_producer.joinable())
			m_producer.join();
		for (auto& link : m_links)
			if (link->reader.joinable())
				link->reader.join();
		m_links.clear();
	}

	void Net::readLoop(Link& link)
	{
		uint8_t type = 0;
		Bytes payload;
		while (link.receive(type, payload))
		{
			Cursor c{ payload };
			if (type == kWelcome && !m_host)
			{
				Welcome w;
				w.seat = int(c.u32());
				const uint32_t n = c.u32();
				for (uint32_t i = 0; i < n && c.ok; ++i)
					w.humans.push_back(int(c.u32()));
				w.save = c.bytes(c.u32());
				if (!c.ok)
					break;
				std::lock_guard lock(m_mutex);
				m_welcomes.push_back(std::move(w));
				g_welcomed.notify_all();
			}
			else if (type == kFrame && !m_host)
			{
				Frame f;
				f.number = c.u32();
				const uint32_t n = c.u32();
				for (uint32_t i = 0; i < n && c.ok; ++i)
					f.commands.push_back(c.bytes(c.u32()));
				if (!c.ok)
					break;
				deliver(std::move(f));
			}
			else if (type == kSubmit && m_host)
			{
				std::lock_guard lock(m_mutex);
				m_pending.push_back(std::move(payload));
			}
			else if (type == kAck && m_host)
			{
				const uint32_t frame = c.u32();
				const int checksum = int(c.u32());
				const bool has = c.u32() != 0;
				link.acked = frame + 1;
				if (has)
				{
					std::lock_guard lock(m_mutex);
					for (const auto& [f, mine] : m_checksums)
						if (f == frame && mine == checksum)
							++m_matched;
						else if (f == frame && m_problem.empty())
						{
							m_problem = std::format("out of sync at frame {}", frame);
							std::printf("  multiplayer: player %d is out of sync at frame %u (%08X, the host %08X)\n", link.seat, frame,
								unsigned(checksum), unsigned(mine));
						}
				}
			}
		}
		const bool wasAlive = link.alive.exchange(false);
		std::lock_guard lock(m_mutex);
		if (m_host)
		{
			if (wasAlive && m_running && link.seat >= 0)
			{
				std::printf("  multiplayer: player at seat %d left\n", link.seat);
				m_lostSeats.push_back(link.seat);
			}
		}
		else
		{
			if (wasAlive && m_running)
				m_problem = "the host has left";
			g_welcomed.notify_all();
		}
	}

	void Net::produceLoop()
	{
		auto next = std::chrono::steady_clock::now();
		while (m_running)
		{
			next += kFrameTime;
			std::this_thread::sleep_until(next);
			if (std::chrono::steady_clock::now() > next + kFrameTime * 10)
				next = std::chrono::steady_clock::now(); // (a long stall: no burst after it)
			Frame f;
			{
				std::lock_guard lock(m_mutex);
				uint32_t slowest = m_executed;
				for (const auto& link : m_links)
					if (link->alive)
						slowest = std::min<uint32_t>(slowest, link->acked);
				if (m_nextFrame >= slowest + kMaxLead)
					continue;
				f.number = m_nextFrame++;
				f.commands.swap(m_pending);
			}
			Bytes b;
			put32(b, f.number);
			put32(b, uint32_t(f.commands.size()));
			for (const Bytes& c : f.commands)
			{
				put32(b, uint32_t(c.size()));
				b.insert(b.end(), c.begin(), c.end());
			}
			broadcast(kFrame, b);
			deliver(std::move(f));
		}
	}

	void Net::broadcast(uint8_t type, const Bytes& payload)
	{
		for (auto& link : m_links)
			if (link->alive && !link->send(type, payload))
				link->alive = false;
	}

	void Net::deliver(Frame frame)
	{
		std::lock_guard lock(m_mutex);
		m_frames.push_back(std::move(frame));
	}

	void Net::submit(Bytes command)
	{
		if (m_host)
		{
			std::lock_guard lock(m_mutex);
			m_pending.push_back(std::move(command));
		}
		else if (!m_links.empty())
			m_links.front()->send(kSubmit, command);
	}

	bool Net::takeFrame(Frame& frame)
	{
		std::lock_guard lock(m_mutex);
		if (m_frames.empty())
			return false;
		frame = std::move(m_frames.front());
		m_frames.pop_front();
		return true;
	}

	void Net::executed(uint32_t frame, int checksum)
	{
		const bool has = frame % kChecksumEvery == 0;
		{
			std::lock_guard lock(m_mutex);
			m_executed = frame + 1;
			if (m_host && has)
			{
				m_checksums.push_back({ frame, checksum });
				if (m_checksums.size() > 64)
					m_checksums.erase(m_checksums.begin());
			}
		}
		if (!m_host && !m_links.empty())
		{
			Bytes b;
			put32(b, frame);
			put32(b, uint32_t(checksum));
			put32(b, has ? 1 : 0);
			m_links.front()->send(kAck, b);
		}
	}

	std::vector<int> Net::takeLostSeats()
	{
		std::lock_guard lock(m_mutex);
		return std::exchange(m_lostSeats, {});
	}

	std::string Net::status() const
	{
		std::lock_guard lock(m_mutex);
		if (!m_problem.empty())
			return "multiplayer: " + m_problem;
		size_t alive = 0;
		for (const auto& link : m_links)
			alive += link->alive ? 1 : 0;
		return m_host ? std::format("multiplayer host, {} other player{}, {} checks in sync", alive, alive == 1 ? "" : "s", m_matched)
					  : std::string("multiplayer");
	}
}
