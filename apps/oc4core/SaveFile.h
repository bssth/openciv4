#pragma once

// The .CivBeyondSwordSave container, as written by the original engine (save version 302):
//
//   int     version (302)
//   string  mod folder, e.g. "Mods\Rise of Mankind\" (narrow; empty for no mod)
//   7 x int 0
//   int     size of the next block
//   bytes   CvInitCore::write, uncompressed (the load screen previews it)
//   int     0
//   chunks  [int size][bytes] of one zlib stream (64 KB output buffers, ended by a
//           sync flush instead of Z_FINISH), then int 0
//   string  pickled Python data (CvAppInterface.onSave), narrow
//   33 x 0
//   string  32 hex digits of an MD5 (salted: it is not a plain hash of the file or of
//           any part of it, so it is written over the save data and ignored on load)
//
// The zlib stream holds the DLL's game data (CvGame, CvMap, teams, players, ...).

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace oc4::host
{
	// 32 lowercase hex digits of the MD5 of the bytes (the save's last field, and the passwords the
	// game core keeps hashed: Utility::md5String).
	std::string md5Hex(const void* data, size_t size);

	struct SaveFile
	{
		static constexpr int kVersion = 302;

		int version = kVersion;
		std::string modFolder;
		std::vector<std::byte> initCore; // CvInitCore::write
		std::vector<std::byte> gameData; // decompressed DLL data
		std::string pythonData;          // pickle string
		std::string md5;

		// Throws std::runtime_error on malformed files.
		static SaveFile read(const std::filesystem::path& path);
		void write(const std::filesystem::path& path) const;
	};
}
