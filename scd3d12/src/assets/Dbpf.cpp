/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "Dbpf.h"

#include "QFSDecompressor.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <windows.h>

namespace nSCAssets {
	namespace {
		uint32_t Le32(uint8_t const *p) {
			uint32_t v;
			std::memcpy(&v, p, 4);
			return v;
		}

		bool Less(Tgi const &a, Tgi const &b) {
			if (a.type != b.type) return a.type < b.type;
			if (a.group != b.group) return a.group < b.group;
			return a.instance < b.instance;
		}
	}

	std::string ToString(Tgi const &tgi) {
		char text[32];
		std::snprintf(text, sizeof(text), "%08X-%08X-%08X", tgi.type, tgi.group, tgi.instance);
		return text;
	}

	bool ParseTgi(std::string_view text, Tgi &tgi) {
		uint32_t parts[3] = {};
		size_t at = 0;
		for (uint32_t &part : parts) {
			while (at < text.size() && (text[at] == '-' || text[at] == ',' || text[at] == ' ')) ++at;
			if (at + 1 < text.size() && text[at] == '0' && (text[at + 1] == 'x' || text[at + 1] == 'X')) at += 2;
			size_t const start = at;
			uint32_t v = 0;
			for (; at < text.size(); ++at) {
				char const c = text[at];
				int const d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
				if (d < 0) break;
				v = v << 4 | static_cast<uint32_t>(d);
			}
			if (at == start || at - start > 8) return false;
			part = v;
		}
		tgi = {parts[0], parts[1], parts[2]};
		return true;
	}

	MappedFile::~MappedFile() { Close(); }

	bool MappedFile::Open(wchar_t const *path) {
		Close();
		HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
		                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE) return false;
		file_ = file;
		LARGE_INTEGER size{};
		if (!GetFileSizeEx(file, &size) || size.QuadPart == 0) {
			Close();
			return false;
		}
		mapping_ = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
		data_ = mapping_ ? static_cast<uint8_t const *>(MapViewOfFile(mapping_, FILE_MAP_READ, 0, 0, 0)) : nullptr;
		if (!data_) {
			Close();
			return false;
		}
		size_ = static_cast<size_t>(size.QuadPart);
		return true;
	}

	void MappedFile::Close() {
		if (data_) UnmapViewOfFile(data_);
		if (mapping_) CloseHandle(mapping_);
		if (file_) CloseHandle(file_);
		data_ = nullptr;
		mapping_ = nullptr;
		file_ = nullptr;
		size_ = 0;
	}

	bool DbpfFile::Open(std::wstring path, std::string *error) {
		auto fail = [&](char const *why) {
			if (error) *error = why;
			file_.Close();
			entries_.clear();
			return false;
		};
		path_ = std::move(path);
		entries_.clear();
		if (!file_.Open(path_.c_str())) return fail("cannot open");
		uint8_t const *const d = file_.Data();
		size_t const n = file_.Size();
		if (n < 96 || std::memcmp(d, "DBPF", 4) != 0) return fail("not a DBPF file");
		if (Le32(d + 4) != 1 || Le32(d + 8) != 0 || Le32(d + 0x20) != 7) return fail("not a DBPF 1.0 package with a 7.x index");

		indexMinor_ = Le32(d + 0x3C);
		uint32_t const count = Le32(d + 0x24);
		uint32_t const indexOffset = Le32(d + 0x28);
		size_t const stride = indexMinor_ == 1 ? 24 : 20;
		if (static_cast<uint64_t>(indexOffset) + static_cast<uint64_t>(count) * stride > n) return fail("index runs past the end of the file");

		entries_.resize(count);
		uint8_t const *rec = d + indexOffset;
		size_t const at = stride == 24 ? 16 : 12;
		for (DbpfEntry &e : entries_) {
			e.tgi = {Le32(rec), Le32(rec + 4), Le32(rec + 8)};
			e.offset = Le32(rec + at);
			e.size = Le32(rec + at + 4);
			if (e.offset < 96 || static_cast<uint64_t>(e.offset) + e.size > n) return fail("an entry lies outside the file");
			rec += stride;
		}

		// Mark what the directory lists. Sorted lookups keep this cheap for packages with hundreds of thousands
		// of entries.
		if (DbpfEntry const *dir = Find(kDirectoryTgi)) {
			std::span<uint8_t const> const body = Stored(*dir);
			std::vector<Tgi> listed(body.size() / 16);
			for (size_t i = 0; i < listed.size(); ++i) {
				uint8_t const *r = body.data() + i * 16;
				listed[i] = {Le32(r), Le32(r + 4), Le32(r + 8)};
			}
			std::sort(listed.begin(), listed.end(), Less);
			for (DbpfEntry &e : entries_) {
				e.compressed = !(e.tgi == kDirectoryTgi) && std::binary_search(listed.begin(), listed.end(), e.tgi, Less);
			}
		}
		return true;
	}

	std::span<uint8_t const> DbpfFile::Stored(DbpfEntry const &entry) const {
		return {file_.Data() + entry.offset, entry.size};
	}

	bool DbpfFile::Read(DbpfEntry const &entry, std::vector<uint8_t> &out) const {
		std::span<uint8_t const> const body = Stored(entry);
		if (!entry.compressed) {
			out.assign(body.begin(), body.end());
			return true;
		}
		if (body.size() < 9) return false;
		return QFS::Decompressor::Decompress(body.subspan(4), out).has_value();
	}

	DbpfEntry const *DbpfFile::Find(Tgi const &tgi) const {
		for (DbpfEntry const &e : entries_) {
			if (e.tgi == tgi) return &e;
		}
		return nullptr;
	}
}
