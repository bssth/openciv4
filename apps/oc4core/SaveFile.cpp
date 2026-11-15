#include "SaveFile.h"

#include "core/FileUtil.h"

#include <windows.h>

#ifdef _WIN32
#include <bcrypt.h>
#elif defined(__APPLE__)
#include <CommonCrypto/CommonDigest.h>
#else
#include <openssl/evp.h>
#endif
#include <zlib.h>

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace oc4::host
{
	namespace
	{
		constexpr size_t kChunk = 65536; // the original engine's compressed buffer size
		constexpr size_t kTrailerZeros = 33;

		struct Reader
		{
			const std::vector<std::byte>& d;
			size_t p = 0;

			void need(size_t n) const
			{
				if (p + n > d.size())
					throw std::runtime_error("save file truncated");
			}
			int i32()
			{
				need(4);
				int v;
				std::memcpy(&v, d.data() + p, 4);
				p += 4;
				return v;
			}
			std::vector<std::byte> bytes(size_t n)
			{
				need(n);
				std::vector<std::byte> out(d.begin() + std::ptrdiff_t(p), d.begin() + std::ptrdiff_t(p + n));
				p += n;
				return out;
			}
			std::string str()
			{
				const int n = i32();
				if (n < 0)
					throw std::runtime_error("bad string length in save");
				const auto b = bytes(size_t(n));
				return std::string(reinterpret_cast<const char*>(b.data()), b.size());
			}
		};

		struct Writer
		{
			std::vector<std::byte> d;
			void raw(const void* p, size_t n)
			{
				const auto* b = static_cast<const std::byte*>(p);
				d.insert(d.end(), b, b + n);
			}
			void i32(int v) { raw(&v, 4); }
			void str(const std::string& s)
			{
				i32(int(s.size()));
				raw(s.data(), s.size());
			}
		};

		std::vector<std::byte> inflateStream(const std::vector<std::byte>& in)
		{
			z_stream z{};
			if (inflateInit(&z) != Z_OK)
				throw std::runtime_error("inflateInit failed");
			std::vector<std::byte> out;
			std::vector<std::byte> buf(1 << 20);
			z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
			z.avail_in = uInt(in.size());
			int rc = Z_OK;
			for (bool more = true; more && rc == Z_OK;)
			{
				z.next_out = reinterpret_cast<Bytef*>(buf.data());
				z.avail_out = uInt(buf.size());
				rc = inflate(&z, Z_SYNC_FLUSH); // the stream ends with a sync flush, not Z_FINISH
				out.insert(out.end(), buf.begin(), buf.begin() + std::ptrdiff_t(buf.size() - z.avail_out));
				more = z.avail_in > 0 || z.avail_out == 0;
			}
			inflateEnd(&z);
			if (rc != Z_OK && rc != Z_STREAM_END && !(rc == Z_BUF_ERROR && z.avail_in == 0))
				throw std::runtime_error("corrupt compressed data in save");
			return out;
		}

		// One zlib stream cut into 64 KB chunks, ended with a sync flush like the original.
		void deflateChunks(const std::vector<std::byte>& in, Writer& w)
		{
			z_stream z{};
			if (deflateInit(&z, Z_DEFAULT_COMPRESSION) != Z_OK)
				throw std::runtime_error("deflateInit failed");
			std::vector<std::byte> buf(kChunk);
			z.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(in.data()));
			z.avail_in = uInt(in.size());
			for (;;)
			{
				z.next_out = reinterpret_cast<Bytef*>(buf.data());
				z.avail_out = uInt(buf.size());
				deflate(&z, Z_SYNC_FLUSH);
				const size_t n = buf.size() - z.avail_out;
				if (n > 0)
				{
					w.i32(int(n));
					w.raw(buf.data(), n);
				}
				if (z.avail_in == 0 && z.avail_out != 0)
					break;
			}
			deflateEnd(&z);
			w.i32(0);
		}

		std::string md5Hex(const std::vector<std::byte>& data)
		{
			unsigned char hash[16] = {};
#ifdef _WIN32
			BCryptHash(BCRYPT_MD5_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<std::byte*>(data.data())), ULONG(data.size()), hash,
				sizeof hash);
#elif defined(__APPLE__)
			CC_MD5(data.data(), CC_LONG(data.size()), hash);
#else
			unsigned int length = sizeof hash;
			EVP_Digest(data.data(), data.size(), hash, &length, EVP_md5(), nullptr);
#endif
			static constexpr char hex[] = "0123456789abcdef";
			std::string s;
			for (const unsigned char c : hash)
			{
				s.push_back(hex[c >> 4]);
				s.push_back(hex[c & 15]);
			}
			return s;
		}
	}

	SaveFile SaveFile::read(const std::filesystem::path& path)
	{
		const std::vector<std::byte> file = readWholeFile(path);
		Reader r{ file };
		SaveFile s;
		s.version = r.i32();
		if (s.version != kVersion)
			throw std::runtime_error("unsupported save version " + std::to_string(s.version));
		s.modFolder = r.str();
		for (int i = 0; i < 7; ++i)
			r.i32();
		const int initSize = r.i32();
		if (initSize < 0)
			throw std::runtime_error("bad init block size in save");
		s.initCore = r.bytes(size_t(initSize));
		r.i32();
		std::vector<std::byte> compressed;
		for (int n; (n = r.i32()) != 0;)
		{
			if (n < 0)
				throw std::runtime_error("bad chunk size in save");
			const auto chunk = r.bytes(size_t(n));
			compressed.insert(compressed.end(), chunk.begin(), chunk.end());
		}
		s.gameData = inflateStream(compressed);
		s.pythonData = r.str();
		r.bytes(kTrailerZeros);
		s.md5 = r.str();
		return s;
	}

	void SaveFile::write(const std::filesystem::path& path) const
	{
		Writer w;
		w.i32(version);
		w.str(modFolder);
		for (int i = 0; i < 7; ++i)
			w.i32(0);
		w.i32(int(initCore.size()));
		w.raw(initCore.data(), initCore.size());
		w.i32(0);
		deflateChunks(gameData, w);
		w.str(pythonData);
		const std::byte zeros[kTrailerZeros] = {};
		w.raw(zeros, sizeof zeros);
		w.str(md5Hex(gameData));

		const auto tmp = std::filesystem::path(path).concat(".tmp");
		{
			std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
			out.write(reinterpret_cast<const char*>(w.d.data()), std::streamsize(w.d.size()));
			if (!out)
				throw std::runtime_error("cannot write " + tmp.string());
		}
		std::filesystem::rename(tmp, path);
	}
}
