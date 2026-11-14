#include "core/AssetResolver.h"

#include "core/FileUtil.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <system_error>

namespace fs = std::filesystem;

namespace oc4
{
	struct AssetResolver::Found
	{
		std::mutex lock;
		std::unordered_map<std::string, std::optional<AssetLocation>> byKey;
	};

	namespace
	{
		bool isFile(const fs::path& p)
		{
			std::error_code ec;
			return fs::is_regular_file(p, ec);
		}

		std::string withExtension(std::string_view key, std::string_view ext)
		{
			const size_t slash = key.rfind('/');
			const size_t dot = key.rfind('.');
			const size_t base = (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash)) ? dot : key.size();
			return std::string(key.substr(0, base)) + std::string(ext);
		}

		std::string parentKey(std::string_view key)
		{
			const size_t slash = key.rfind('/');
			return slash == std::string_view::npos ? std::string() : std::string(key.substr(0, slash + 1));
		}

		std::string fileNameKey(std::string_view key)
		{
			const size_t slash = key.rfind('/');
			return std::string(slash == std::string_view::npos ? key : key.substr(slash + 1));
		}
	}

	AssetResolver::AssetResolver() : m_found(std::make_unique<Found>()) {}
	AssetResolver::~AssetResolver() = default;
	AssetResolver::AssetResolver(AssetResolver&&) noexcept = default;
	AssetResolver& AssetResolver::operator=(AssetResolver&&) noexcept = default;

	std::string AssetLocation::display() const
	{
		if (archive)
			return archive->path().filename().string() + ":" + key;
		return file.string();
	}

	AssetResolver AssetResolver::forInstall(const fs::path& civ4Root, const std::optional<fs::path>& modAssets)
	{
		AssetResolver r;
		if (modAssets)
			r.addLayer(*modAssets);
		for (const char* layer : { "Beyond the Sword/Assets", "Warlords/Assets", "Assets" })
			if (fs::is_directory(civ4Root / layer))
				r.addLayer(civ4Root / layer);
		return r;
	}

	void AssetResolver::addLayer(const fs::path& assetsDir)
	{
		Layer layer{ assetsDir, {} };
		std::vector<fs::path> packs;
		std::error_code ec;
		for (const auto& e : fs::directory_iterator(assetsDir, ec))
			if (e.is_regular_file() && toLowerAscii(utf8Path(e.path().extension())) == ".fpk")
				packs.push_back(e.path());
		std::ranges::sort(packs);
		for (const auto& p : packs)
			layer.archives.push_back(std::make_unique<FpkArchive>(p));
#if !defined(_WIN32) && !defined(__APPLE__)
		for (auto it = fs::recursive_directory_iterator(assetsDir, fs::directory_options::skip_permission_denied, ec);
			it != fs::recursive_directory_iterator(); it.increment(ec))
			if (!ec && it->is_regular_file())
				layer.files.try_emplace(makeAssetKey(utf8GenericPath(it->path().lexically_relative(assetsDir))), it->path());
#endif
		m_layers.push_back(std::move(layer));
		m_found->byKey.clear();
	}

	std::optional<AssetLocation> AssetResolver::locateKey(const std::string& key) const
	{
		if (key.empty())
			return std::nullopt;
		{
			std::lock_guard hold(m_found->lock);
			if (const auto f = m_found->byKey.find(key); f != m_found->byKey.end())
				return f->second;
		}
		std::optional<AssetLocation> found;
		for (size_t i = 0; i < m_layers.size() && !found; ++i)
		{
			const Layer& layer = m_layers[i];
#if !defined(_WIN32) && !defined(__APPLE__)
			if (const auto f = layer.files.find(key); f != layer.files.end())
				found = AssetLocation{ key, i, f->second, nullptr, nullptr };
#else
			if (const fs::path p = layer.root / fs::path(key); isFile(p))
				found = AssetLocation{ key, i, p, nullptr, nullptr };
#endif
			for (size_t a = 0; a < layer.archives.size() && !found; ++a)
				if (const auto* entry = layer.archives[a]->find(key))
					found = AssetLocation{ key, i, {}, layer.archives[a].get(), entry };
		}
		std::lock_guard hold(m_found->lock);
		return m_found->byKey.emplace(key, std::move(found)).first->second;
	}

	std::optional<AssetLocation> AssetResolver::locate(std::string_view relativePath) const
	{
		return locateKey(makeAssetKey(relativePath));
	}

	std::optional<AssetLocation> AssetResolver::locateFile(const fs::path& absolute) const
	{
		const fs::path abs = fs::absolute(absolute).lexically_normal();
		for (size_t i = 0; i < m_layers.size(); ++i)
		{
			const fs::path rel = abs.lexically_relative(fs::absolute(m_layers[i].root).lexically_normal());
			if (!rel.empty() && *rel.begin() != "..")
				return AssetLocation{ makeAssetKey(rel.generic_string()), i, abs, nullptr, nullptr };
		}
		return std::nullopt;
	}

	std::optional<AssetLocation> AssetResolver::locateTexture(const AssetLocation& model, std::string_view textureName) const
	{
		const std::string key = makeAssetKey(textureName);
		if (key.empty())
			return std::nullopt;

		std::vector<std::string> names{ key };
		if (!key.ends_with(".dds"))
			names.push_back(withExtension(key, ".dds"));

		// Shared texture folders found by nifscan's unresolved-texture analysis.
		static constexpr std::string_view kSearchDirs[] = { "art/shared/", "art/interface/teamcolor/", "art/terrain/plottextures/" };

		const std::string dir = parentKey(model.key);
		for (const auto& n : names)
			if (auto loc = locateKey(dir + fileNameKey(n)))
				return loc;
		for (const auto& n : names)
			if (auto loc = locateKey(n))
				return loc;
		for (const auto searchDir : kSearchDirs)
			for (const auto& n : names)
				if (auto loc = locateKey(std::string(searchDir) + fileNameKey(n)))
					return loc;
		return std::nullopt;
	}

	std::vector<std::byte> AssetResolver::read(const AssetLocation& loc) const
	{
		if (loc.archive)
			return loc.archive->read(*loc.entry);
		return readWholeFile(loc.file);
	}

	std::vector<AssetLocation> AssetResolver::list(std::string_view extension) const
	{
		const std::string ext = toLowerAscii(std::string(extension));
		std::map<std::string, AssetLocation> found;
		for (size_t i = 0; i < m_layers.size(); ++i)
		{
			const Layer& layer = m_layers[i];
			std::error_code ec;
			for (auto it = fs::recursive_directory_iterator(layer.root, fs::directory_options::skip_permission_denied, ec);
				it != fs::recursive_directory_iterator(); it.increment(ec))
			{
				if (ec || !it->is_regular_file() || toLowerAscii(utf8Path(it->path().extension())) != ext)
					continue;
				std::string key = makeAssetKey(utf8GenericPath(it->path().lexically_relative(layer.root)));
				found.try_emplace(key, AssetLocation{ key, i, it->path(), nullptr, nullptr });
			}
			for (const auto& archive : layer.archives)
				for (const auto& e : archive->entries())
				{
					std::string key = makeAssetKey(e.name);
					if (key.ends_with(ext))
						found.try_emplace(key, AssetLocation{ key, i, {}, archive.get(), &e });
				}
		}
		std::vector<AssetLocation> out;
		out.reserve(found.size());
		for (auto& [k, v] : found)
			out.push_back(std::move(v));
		return out;
	}
}
