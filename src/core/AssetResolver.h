#pragma once

#include "core/FpkArchive.h"

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace oc4
{
	// A resolved asset: either a loose file or an entry inside an FPK archive.
	struct AssetLocation
	{
		std::string key; // normalised path relative to an Assets folder
		size_t layer = 0;
		std::filesystem::path file;            // loose file, if any
		const FpkArchive* archive = nullptr;   // packed file, if any
		const FpkArchive::Entry* entry = nullptr;

		std::string display() const;
	};

	// Civ4 layers asset folders: mod > Beyond the Sword > Warlords > vanilla.
	// Within a layer loose files win over FPK archives.
	class AssetResolver
	{
	public:
		AssetResolver();
		~AssetResolver();
		AssetResolver(AssetResolver&&) noexcept;
		AssetResolver& operator=(AssetResolver&&) noexcept;

		// civ4Root: folder containing Assets, Warlords and Beyond the Sword.
		static AssetResolver forInstall(const std::filesystem::path& civ4Root,
			const std::optional<std::filesystem::path>& modAssets = std::nullopt);

		void addLayer(const std::filesystem::path& assetsDir); // also loads *.fpk in it
		size_t layerCount() const { return m_layers.size(); }
		// The layers' Assets folders, highest first (what sits beside them, as Resource, is the layer's too).
		std::vector<std::filesystem::path> layerRoots() const
		{
			std::vector<std::filesystem::path> roots;
			for (const Layer& l : m_layers)
				roots.push_back(l.root);
			return roots;
		}

		std::optional<AssetLocation> locate(std::string_view relativePath) const;

		// Maps an absolute loose file back to its layer and key.
		std::optional<AssetLocation> locateFile(const std::filesystem::path& absolute) const;

		// Texture lookup as Gamebryo/Civ4 does it: the model's folder in every
		// layer first, then as an asset path; .bmp/.tga references fall back to .dds.
		std::optional<AssetLocation> locateTexture(const AssetLocation& model, std::string_view textureName) const;

		std::vector<std::byte> read(const AssetLocation& loc) const;

		// Every asset key with the given extension (".nif"), loose and packed, highest layer wins.
		std::vector<AssetLocation> list(std::string_view extension) const;

	private:
		struct Layer
		{
			std::filesystem::path root;
			std::vector<std::unique_ptr<FpkArchive>> archives;
#if !defined(_WIN32) && !defined(__APPLE__)
			// Case-sensitive file systems (Linux): asset key -> the file's real path. The game asks for
			// paths in any case and with backslashes, as Windows and macOS allow.
			std::unordered_map<std::string, std::filesystem::path> files;
#endif
		};

		std::optional<AssetLocation> locateKey(const std::string& key) const;

		std::vector<Layer> m_layers;
		struct Found; // what locateKey found by key (the files do not change while the game runs)
		std::unique_ptr<Found> m_found;
	};
}
