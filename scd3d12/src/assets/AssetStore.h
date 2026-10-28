/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Every DBPF entry SimCity 4 would see, by TGI, with later files overriding earlier ones the way the game
// resolves them.
//
// Load order, read from cSC4App::SetupResources in the symbolized Mac build (x86-32-cpu0x3, 0x0008113E) and
// checked against the same function in the Windows 1.1.641 exe (0x004572B0):
//   1. GetDataDirectory(-1), the install folder: files matching "*.dat", not recursive, each its own segment.
//   2. GetDataDirectory(0), the language folder (for example English\): the same.
//   3. GetSkuSpecificDirectory (Sku_Data\...): one cGZPersistDBSegmentMultiPackedFiles over every "*.dat" below it.
//   4. GetPluginDirectory, the install folder's Plugins:
//      a. every file below it whose extension is not ".dat" (any case), each its own segment, kept only when
//         cGZDBSegmentPackedFile::IsDatabaseFile finds a DBPF header (so .SC4Lot, .SC4Desc, .SC4Model or any
//         other name all load)
//      b. then one multi packed segment over every "*.dat" below it
//   5. GetUserPluginDirectory (Documents\SimCity 4\Plugins): the same as 4.
// Each new segment goes to the front of cGZPersistResourceManager's segment list (RegisterDBSegment calls
// RegisterDBSegmentFront) and FindDBSegment walks the list from the front, so the last segment registered wins.
// Inside a multi packed segment the files are opened in listing order and each key is assigned into a hash map
// (cGZPersistDBSegmentMultiPackedFiles::AddedResource), so there too the later file wins. The upshot: in each
// Plugins folder every .dat file beats every non-.dat file, whatever their names or subfolders.
//
// Listing order (cRZDirectory::ReadDirectoryFileEntriesIntoStringListRecursive): a folder's matching files first,
// then each subfolder in turn, depth first. Neither list is sorted: it is the order FindFirstFile/FindNextFile
// return, which on NTFS is the file system's own case-insensitive name order. This store lists folders through the
// same Windows calls with the same patterns, so it gets the same order and the same wildcard quirks.
//
// OpenSC4's data-formats spec (github.com/tloriato/OpenSC4, docs/specs/data-formats.md §3.1) reads the Windows exe
// to the same order. The city save, which the game adds last at the lowest priority, is not part of this store.

#pragma once

#include "Dbpf.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nSCAssets {
	class AssetStore {
	public:
		struct Ref {
			uint32_t file;
			uint32_t entry;
		};

		// Steps 1 to 4 above. The SKU folder is Sku_Data below the install folder.
		void AddGameData(std::wstring const &installDir, std::wstring const &language = L"English");
		// Steps 1 to 5 from the folders SimCity 4 itself names. An empty name is skipped.
		void AddFolders(std::wstring const &installDir, std::wstring const &languageDir, std::wstring const &skuDir,
		                std::wstring const &pluginsDir, std::wstring const &userPluginsDir);
		// Steps 4 and 5: one Plugins folder.
		void AddPlugins(std::wstring const &pluginsDir);
		// One package on top of everything added so far.
		bool AddPackage(std::wstring const &path);

		DbpfFile const *Find(Tgi const &tgi, DbpfEntry const **entry = nullptr) const;
		bool Read(Tgi const &tgi, std::vector<uint8_t> &out) const;

		std::vector<std::unique_ptr<DbpfFile>> const &Files() const { return files_; }
		std::unordered_map<Tgi, Ref, TgiHash> const &Index() const { return index_; }
		// Entries seen across all files, overridden ones included.
		size_t EntriesSeen() const { return entriesSeen_; }

	private:
		// Opens the files in parallel, then adds them in order.
		void AddFiles(std::vector<std::wstring> const &paths);

		std::vector<std::unique_ptr<DbpfFile>> files_;
		std::unordered_map<Tgi, Ref, TgiHash> index_;
		size_t entriesSeen_ = 0;
	};

	// Lists files below `dir` the way cRZDirectory does: the folder's own files matching `pattern` in
	// FindFirstFile order, then each subfolder (when `recursive`). Exposed for tests and tools.
	void ListFiles(std::wstring const &dir, wchar_t const *pattern, bool recursive, std::vector<std::wstring> &out);
}
