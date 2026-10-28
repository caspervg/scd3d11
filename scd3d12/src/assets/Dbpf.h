/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// The index side of DBPF packages, kept small for indexing every plugin at startup. DBPFKit (vendor/DBPFKit) does
// the decompression and the formats inside the entries. Its own DBPF::Reader is not used here because it builds
// four hash maps per package (about 5 s to open the 273 packages and 4.3 million entries of a large Plugins folder,
// against about 1 s here on one thread with the store's own index included) and it guesses compression from the
// bytes instead of reading the directory, which misreads a few small uncompressed entries.
//
// The rules follow the game, as described in OpenSC4's data-formats spec (docs/specs/data-formats.md, §2) and
// https://wiki.sc4devotion.com/index.php?title=DBPF:
//   - a 96 byte header: "DBPF", version 1.0, index type 7 at 0x20, entry count 0x24, index offset 0x28, index
//     size 0x2C, index minor version 0x3C
//   - index entries of 20 bytes: type, group, instance, offset, size. The game ignores the index minor version and
//     cannot open 7.1 packages (24 byte entries with a second instance word); they are read here anyway
//   - one entry outside the file makes the whole package unusable
//   - inside one package the first entry with a key wins
//   - the directory entry E86B1EEF-E86B1EEF-286B1F03 holds 16 byte records: type, group, instance, uncompressed
//     size. An entry is decompressed if and only if it is listed there (the first record wins), and a listed entry
//     whose bytes do not decompress fails to read. A compressed body is a 4 byte size and then a QFS stream.

#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace nSCAssets {
	struct Tgi {
		uint32_t type = 0;
		uint32_t group = 0;
		uint32_t instance = 0;

		bool operator==(Tgi const &) const = default;
	};

	struct TgiHash {
		size_t operator()(Tgi const &t) const noexcept {
			uint64_t h = (static_cast<uint64_t>(t.type) * 0x9E3779B97F4A7C15ull) ^ t.group;
			h = (h * 0xC2B2AE3D27D4EB4Full) ^ t.instance;
			return static_cast<size_t>(h ^ (h >> 29));
		}
	};

	// "TTTTTTTT-GGGGGGGG-IIIIIIII"
	std::string ToString(Tgi const &tgi);
	// Three hex numbers separated by '-', ',' or spaces, each with or without 0x.
	bool ParseTgi(std::string_view text, Tgi &tgi);

	constexpr Tgi kDirectoryTgi{0xE86B1EEF, 0xE86B1EEF, 0x286B1F03};

	// A read-only view of a whole file. Pages come in as they are touched.
	class MappedFile {
	public:
		MappedFile() = default;
		MappedFile(MappedFile const &) = delete;
		MappedFile &operator=(MappedFile const &) = delete;
		~MappedFile();

		bool Open(wchar_t const *path);
		void Close();
		uint8_t const *Data() const { return data_; }
		size_t Size() const { return size_; }

	private:
		void *file_ = nullptr;
		void *mapping_ = nullptr;
		uint8_t const *data_ = nullptr;
		size_t size_ = 0;
	};

	struct DbpfEntry {
		Tgi tgi;
		uint32_t offset = 0;
		uint32_t size = 0;
		bool compressed = false;
	};

	class DbpfFile {
	public:
		// Maps the file and reads its index and directory. Entry bodies are not touched.
		bool Open(std::wstring path, std::string *error = nullptr);

		std::wstring const &Path() const { return path_; }
		// In index order, duplicates included.
		std::vector<DbpfEntry> const &Entries() const { return entries_; }
		uint32_t IndexMinor() const { return indexMinor_; }

		// The bytes as stored, compressed or not.
		std::span<uint8_t const> Stored(DbpfEntry const &entry) const;
		// The entry's bytes, decompressed when the directory lists it. Safe to call from several threads.
		bool Read(DbpfEntry const &entry, std::vector<uint8_t> &out) const;
		// The first entry with this key, as the game picks it.
		DbpfEntry const *Find(Tgi const &tgi) const;

	private:
		std::wstring path_;
		MappedFile file_;
		std::vector<DbpfEntry> entries_;
		uint32_t indexMinor_ = 0;
	};
}
