/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "AssetStore.h"

#include <algorithm>
#include <execution>
#include <windows.h>

namespace nSCAssets {
	namespace {
		bool IsDotDat(std::wstring const &path) {
			size_t const slash = path.find_last_of(L"\\/");
			size_t const dot = path.find_last_of(L'.');
			if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;
			return _wcsicmp(path.c_str() + dot, L".dat") == 0;
		}

		void ListEntries(std::wstring const &dir, wchar_t const *pattern, bool directories, std::vector<std::wstring> &out) {
			WIN32_FIND_DATAW fd;
			HANDLE find = FindFirstFileExW((dir + L"\\" + pattern).c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
			                               FIND_FIRST_EX_LARGE_FETCH);
			if (find == INVALID_HANDLE_VALUE) return;
			do {
				bool const isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
				if (isDir != directories) continue;
				if (isDir && (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0)) continue;
				out.push_back(dir + L"\\" + fd.cFileName);
			} while (FindNextFileW(find, &fd));
			FindClose(find);
		}
	}

	void ListFiles(std::wstring const &dir, wchar_t const *pattern, bool recursive, std::vector<std::wstring> &out) {
		ListEntries(dir, pattern, false, out);
		if (!recursive) return;
		std::vector<std::wstring> subdirs;
		ListEntries(dir, L"*", true, subdirs);
		for (std::wstring const &sub : subdirs) ListFiles(sub, pattern, true, out);
	}

	void AssetStore::AddGameData(std::wstring const &installDir, std::wstring const &language) {
		AddFolders(installDir, installDir + L"\\" + language, installDir + L"\\Sku_Data", installDir + L"\\Plugins", L"");
	}

	void AssetStore::AddFolders(std::wstring const &installDir, std::wstring const &languageDir,
	                            std::wstring const &skuDir, std::wstring const &pluginsDir,
	                            std::wstring const &userPluginsDir) {
		std::vector<std::wstring> paths;
		if (!installDir.empty()) ListFiles(installDir, L"*.dat", false, paths);
		if (!languageDir.empty()) ListFiles(languageDir, L"*.dat", false, paths);
		if (!skuDir.empty()) ListFiles(skuDir, L"*.dat", true, paths);
		AddFiles(paths);
		if (!pluginsDir.empty()) AddPlugins(pluginsDir);
		if (!userPluginsDir.empty()) AddPlugins(userPluginsDir);
	}

	void AssetStore::AddPlugins(std::wstring const &pluginsDir) {
		std::vector<std::wstring> paths;
		ListFiles(pluginsDir, L"*", true, paths);
		std::erase_if(paths, IsDotDat);
		ListFiles(pluginsDir, L"*.dat", true, paths);
		AddFiles(paths);
	}

	bool AssetStore::AddPackage(std::wstring const &path) {
		size_t const before = files_.size();
		AddFiles({path});
		return files_.size() > before;
	}

	void AssetStore::AddFiles(std::vector<std::wstring> const &paths) {
		// Opening is mostly waiting on the index pages, so spread it across threads. The order the entries
		// are added in is what decides overrides, and that stays the listing order.
		std::vector<std::unique_ptr<DbpfFile>> opened(paths.size());
		std::vector<size_t> order(paths.size());
		for (size_t i = 0; i < order.size(); ++i) order[i] = i;
		std::for_each(std::execution::par, order.begin(), order.end(), [&](size_t i) {
			auto file = std::make_unique<DbpfFile>();
			std::wstring path = paths[i];
			std::replace(path.begin(), path.end(), L'/', L'\\');
			if (file->Open(path)) opened[i] = std::move(file);
		});

		size_t total = 0;
		for (auto const &f : opened) {
			if (f) total += f->Entries().size();
		}
		index_.reserve(index_.size() + total);
		for (auto &f : opened) {
			if (!f) continue;
			uint32_t const fileIndex = static_cast<uint32_t>(files_.size());
			std::vector<DbpfEntry> const &entries = f->Entries();
			// Backwards, so that inside one package the first entry with a key is the one left standing.
			for (uint32_t i = static_cast<uint32_t>(entries.size()); i-- > 0;) {
				if (entries[i].tgi == kDirectoryTgi) continue;
				index_.insert_or_assign(entries[i].tgi, Ref{fileIndex, i});
			}
			entriesSeen_ += entries.size();
			files_.push_back(std::move(f));
		}
	}

	DbpfFile const *AssetStore::Find(Tgi const &tgi, DbpfEntry const **entry) const {
		auto it = index_.find(tgi);
		if (it == index_.end()) return nullptr;
		DbpfFile const *file = files_[it->second.file].get();
		if (entry) *entry = &file->Entries()[it->second.entry];
		return file;
	}

	bool AssetStore::Read(Tgi const &tgi, std::vector<uint8_t> &out) const {
		DbpfEntry const *entry = nullptr;
		DbpfFile const *file = Find(tgi, &entry);
		return file && file->Read(*entry, out);
	}
}
