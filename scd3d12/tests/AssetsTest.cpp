/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// Checks the asset store's load order and the model key rule on made up packages, then, when the game is
// installed, reads every model and texture in its data.
//   AssetsTest [GAME_DIR] [PLUGINS_DIR]

#undef NDEBUG
#include "AssetStore.h"
#include "Models.h"
#include "Textures.h"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

using namespace nSCAssets;
namespace fs = std::filesystem;

namespace {
	struct Item {
		Tgi tgi;
		std::vector<uint8_t> bytes;
		bool compressed = false;
	};

	void Put32(std::vector<uint8_t> &out, uint32_t v) {
		for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
	}

	// A QFS stream of literal blocks only, which every QFS reader must accept.
	std::vector<uint8_t> LiteralQfs(std::vector<uint8_t> const &raw) {
		std::vector<uint8_t> out = {0x10, 0xFB, static_cast<uint8_t>(raw.size() >> 16), static_cast<uint8_t>(raw.size() >> 8),
		                            static_cast<uint8_t>(raw.size())};
		size_t at = 0;
		while (raw.size() - at >= 4) {
			size_t const n = std::min<size_t>((raw.size() - at) & ~size_t(3), 112);
			out.push_back(static_cast<uint8_t>(0xE0 | ((n - 4) >> 2)));
			out.insert(out.end(), raw.begin() + at, raw.begin() + at + n);
			at += n;
		}
		out.push_back(static_cast<uint8_t>(0xFC | (raw.size() - at)));
		out.insert(out.end(), raw.begin() + at, raw.end());
		return out;
	}

	void WritePackage(fs::path const &path, std::vector<Item> const &items) {
		std::vector<uint8_t> body, index, dir;
		for (Item const &item : items) {
			std::vector<uint8_t> stored = item.bytes;
			if (item.compressed) {
				std::vector<uint8_t> const qfs = LiteralQfs(item.bytes);
				stored.clear();
				Put32(stored, static_cast<uint32_t>(qfs.size() + 4));
				stored.insert(stored.end(), qfs.begin(), qfs.end());
				for (uint32_t v : {item.tgi.type, item.tgi.group, item.tgi.instance, static_cast<uint32_t>(item.bytes.size())}) Put32(dir, v);
			}
			for (uint32_t v : {item.tgi.type, item.tgi.group, item.tgi.instance, static_cast<uint32_t>(96 + body.size()), static_cast<uint32_t>(stored.size())})
				Put32(index, v);
			body.insert(body.end(), stored.begin(), stored.end());
		}
		uint32_t count = static_cast<uint32_t>(items.size());
		if (!dir.empty()) {
			for (uint32_t v : {kDirectoryTgi.type, kDirectoryTgi.group, kDirectoryTgi.instance, static_cast<uint32_t>(96 + body.size()),
			                   static_cast<uint32_t>(dir.size())})
				Put32(index, v);
			body.insert(body.end(), dir.begin(), dir.end());
			++count;
		}
		std::vector<uint8_t> header(96, 0);
		std::memcpy(header.data(), "DBPF", 4);
		header[4] = 1;
		header[0x20] = 7;
		std::memcpy(&header[0x24], &count, 4);
		uint32_t const indexOffset = static_cast<uint32_t>(96 + body.size()), indexSize = static_cast<uint32_t>(index.size());
		std::memcpy(&header[0x28], &indexOffset, 4);
		std::memcpy(&header[0x2C], &indexSize, 4);
		std::ofstream f(path, std::ios::binary);
		f.write(reinterpret_cast<char const *>(header.data()), header.size());
		f.write(reinterpret_cast<char const *>(body.data()), body.size());
		f.write(reinterpret_cast<char const *>(index.data()), index.size());
	}

	std::vector<uint8_t> Text(char const *s) { return std::vector<uint8_t>(s, s + std::strlen(s)); }

	std::string ReadText(AssetStore const &store, Tgi const &tgi) {
		std::vector<uint8_t> bytes;
		return store.Read(tgi, bytes) ? std::string(bytes.begin(), bytes.end()) : std::string("(missing)");
	}

	void TestLoadOrder() {
		fs::path const root = fs::temp_directory_path() / "SCD3D12AssetsTest";
		fs::remove_all(root);
		fs::create_directories(root / "sub");
		Tgi const a{1, 2, 3}, b{1, 2, 4}, c{1, 2, 5}, d{1, 2, 6};
		// Names chosen so that name order, the .dat rule and subfolders all matter.
		WritePackage(root / "a.dat", {{a, Text("a.dat")}, {b, Text("a.dat")}});
		WritePackage(root / "z.SC4Lot", {{a, Text("z.SC4Lot")}, {b, Text("z.SC4Lot")}, {c, Text("z.SC4Lot")}});
		WritePackage(root / "b.dat", {{b, Text("b.dat, compressed and a little longer than four bytes"), true}, {d, Text("first")}, {d, Text("second")}});
		WritePackage(root / "sub" / "0.SC4Desc", {{c, Text("sub/0.SC4Desc")}});
		WritePackage(root / "sub" / "0.dat", {{a, Text("sub/0.dat")}});
		std::ofstream(root / "notes.txt") << "not a package";

		std::vector<std::wstring> listed;
		ListFiles(root.wstring(), L"*", true, listed);
		std::vector<std::wstring> names;
		for (auto const &p : listed) names.push_back(fs::path(p).lexically_relative(root).generic_wstring());
		std::vector<std::wstring> const expected = {L"a.dat", L"b.dat", L"notes.txt", L"z.SC4Lot", L"sub/0.dat", L"sub/0.SC4Desc"};
		assert(names == expected);

		AssetStore store;
		store.AddPlugins(root.wstring());
		assert(store.Files().size() == 5);
		// .dat files beat every other package in the same Plugins folder, later .dat files beat earlier ones,
		// and subfolders come after their parent's files.
		assert(ReadText(store, a) == "sub/0.dat");
		assert(ReadText(store, b) == "b.dat, compressed and a little longer than four bytes");
		assert(ReadText(store, c) == "sub/0.SC4Desc");
		// Inside one package the first entry with a key wins.
		assert(ReadText(store, d) == "first");
		fs::remove_all(root);
		std::printf("load order: ok\n");
	}

	void TestModelKeys() {
		std::vector<ModelRef> out;
		uint32_t const rkt1[] = {kS3dType, 0xBADB57F1, 0x021E0000};
		ModelsFromKey(0x27812821, rkt1, 4, 2, 0, out);
		assert(out.size() == 1 && out[0].tgi == (Tgi{kS3dType, 0xBADB57F1, 0x021E0420}));
		out.clear();
		// Low detail remaps zoom 4 to the zoom 3 models.
		ModelsFromKey(0x27812821, rkt1, 4, 1, 0, out, 2);
		assert(out.size() == 1 && out[0].tgi.instance == 0x021E0310);
		out.clear();
		uint32_t const rkt0[] = {kS3dType, 1, 0x12345678};
		ModelsFromKey(0x27812820, rkt0, 0, 3, 0, out);
		assert(out.size() == 1 && out[0].tgi.instance == 0x12345678);
		out.clear();
		uint32_t rkt2[22] = {kS3dType, 1};
		for (uint32_t i = 0; i < 20; ++i) rkt2[2 + i] = 0x100 + i;
		ModelsFromKey(0x27812822, rkt2, 3, 1, 0, out);
		assert(out.size() == 1 && out[0].tgi.instance == 0x100 + 4 * 3 + 1);
		out.clear();
		uint32_t const rkt3[] = {kS3dType, 1, 10, 11, 12, 13, 14};
		ModelsFromKey(0x27812823, rkt3, 2, 3, 0, out);
		assert(out.size() == 1 && out[0].tgi.instance == 12);
		out.clear();
		uint32_t const rkt5[] = {kS3dType, 0xBADB57F1, 0x16900000};
		ModelsFromKey(0x27812825, rkt5, 1, 0, 2, out);
		assert(out.size() == 1 && out[0].tgi.instance == 0x16900000 + 0x100 + 0x20000);
		out.clear();
		// OpenSC4 graphics-formats §4.2: Street2x6x2_$$$LuxerySedan2_1D71.
		uint32_t const rkt4[] = {0, 0, 0, 0, 0x27812821, kS3dType, 0xBADB57F1, 0x1D710000, 1, 0, 0, 0, 0x27812820, kS3dType, 0xBADB57F1, 0};
		ModelsFromKey(0x27812824, rkt4, 4, 2, 0, out);
		assert(out.size() == 1 && out[0].tgi.instance == 0x1D710420);
		out.clear();
		ModelsFromKey(0x27812824, rkt4, 4, 2, 1, out);
		assert(out.empty());
		// Offsets are signed 16.16.
		uint32_t const offsetRecord[] = {1, 0xFFFE6667, 0x00078000, 0, 0x27812820, kS3dType, 1, 0x29DA0000};
		ModelsFromKey(0x27812824, offsetRecord, 0, 0, 1, out);
		assert(out.size() == 1 && std::abs(out[0].offset[0] + 1.5999908f) < 1e-5f && out[0].offset[1] == 7.5f);
		assert(ModelRotation(1, 0, false) == 3 && ModelRotation(1, 2, false) == 1 && ModelRotation(2, 3, true) == 2);

		Tgi tgi;
		assert(ParseTgi("5AD0E817-BADB57F1-0C006800", tgi) && ToString(tgi) == "5AD0E817-BADB57F1-0C006800");
		assert(ParseTgi("0x6534284a, 0x67bddf0c, 0x021e0000", tgi) && tgi == (Tgi{0x6534284A, 0x67BDDF0C, 0x021E0000}));
		std::printf("model keys: ok\n");
	}

	double Since(std::chrono::steady_clock::time_point t0) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
	}

	void TestGameData(std::wstring const &game) {
		AssetStore store;
		auto t0 = std::chrono::steady_clock::now();
		store.AddGameData(game);
		std::printf("game data: %zu packages, %zu entries, %zu keys, %.0f ms\n", store.Files().size(), store.EntriesSeen(), store.Index().size(), Since(t0));
		assert(store.Files().size() >= 10);

		// Every model and texture the game ships, decoded.
		size_t models = 0, modelFailures = 0, meshes = 0, textures = 0, textureFailures = 0;
		std::map<uint8_t, size_t> codes;
		std::vector<uint8_t> bytes;
		Model model;
		Texture texture;
		t0 = std::chrono::steady_clock::now();
		for (auto const &[tgi, ref] : store.Index()) {
			if (tgi.type == kS3dType) {
				++models;
				std::string error;
				if (!store.Read(tgi, bytes) || !DecodeModel(bytes, model, 0, &error)) {
					if (++modelFailures <= 5) std::printf("  model %s: %s\n", ToString(tgi).c_str(), error.c_str());
				} else {
					meshes += model.meshes.size();
				}
			} else if (tgi.type == kFshType) {
				++textures;
				std::string error;
				if (!store.Read(tgi, bytes) || !DecodeTexture(bytes, texture, 0, &error)) {
					if (++textureFailures <= 30) std::printf("  texture %s: %s\n", ToString(tgi).c_str(), error.c_str());
				} else {
					++codes[texture.code];
				}
			}
		}
		std::printf("  %zu models (%zu failed, %zu meshes), %zu textures (%zu failed), %.0f ms\n", models, modelFailures, meshes, textures,
		            textureFailures, Since(t0));
		for (auto const &[code, count] : codes) std::printf("  FSH code %02X: %zu\n", code, count);
		// Known misses: two models with 1,440 vertex blocks (0E0C0000 and 0E0D0000) pass DBPFKit's limit of 1,000, and
		// 24 textures use the 8-bit palette code 0x7B, which is not decoded.
		assert(models > 40000 && modelFailures <= 2);
		assert(textures > 40000 && textureFailures <= 25);

		// A Maxis highway piece and a Maxis building, cross-checked against the dbpf tools.
		assert(LoadModel(store, {kS3dType, 0xBADB57F1, 0x0C006800}, model));
		assert(model.meshes.size() == 3);
		size_t triangles = 0;
		for (Mesh const &m : model.meshes) triangles += m.indices.size() / 3;
		assert(triangles == 33);
		assert(model.meshes[2].material.texture == 0xEA367DAB && model.meshes[2].material.flags == 0x1AA);
		assert(LoadModelTexture(store, 0xEA367DAB, 0xBADB57F1, texture));
		assert(texture.format == TextureFormat::Bc1 && texture.levels[0].width == 256 && texture.levels[0].data.size() == 256 * 256 / 2);

		// A DXT1 texture whose levels are packed without padding: the game uploads 8 levels read at the aligned
		// offsets, the last one from the name block that follows (first bytes as recorded from the game's upload).
		assert(LoadModelTexture(store, 0x10EC0000, 0xBADB57F1, texture) && texture.levels.size() == 8);
		uint64_t const recorded[8] = {0xA0555500A4D4DEBB, 0xAA00FE55B536CE39, 0x7A2000AA9C93C5F8, 0x55FA00003166BDB7,
		                              0xFF5F55E062ECBDB8, 0xD5EACAEAB5763187, 0xFFFFFFFE63CD59EB, 0x4345303100000070};
		for (size_t level = 0; level < 8; ++level) {
			uint64_t first = 0;
			std::memcpy(&first, texture.levels[level].data.data(), sizeof(first));
			assert(first == recorded[level] && texture.levels[level].width == std::max(1u, 128u >> level));
		}

		std::vector<ModelRef> refs;
		Tgi const condos{kExemplarType, 0x67BDDF0C, 0x021E0000};
		for (uint32_t zoom = 0; zoom < 5; ++zoom) {
			for (uint32_t rotation = 0; rotation < 4; ++rotation) {
				assert(OccupantModels(store, condos, zoom, rotation, refs));
				assert(refs.size() == 1 && refs[0].tgi.instance == 0x021E0000 + zoom * 0x100 + rotation * 0x10 && store.Find(refs[0].tgi));
			}
		}
		// ExemplarType 2 (buildings) comes from a parent cohort.
		std::vector<uint32_t> values;
		assert(ExemplarProperty(store, condos, 0x10, values) && values.size() == 1 && values[0] == 2);
		// The Living Mall's exemplar is a text exemplar, whose parent key reads group, instance, type.
		Tgi const mall{kExemplarType, 0xCA386E22, 0x04C00000};
		assert(OccupantModels(store, mall, 4, 3, refs) && refs.size() == 1 && refs[0].tgi.instance == 0x04C00430);
		assert(ExemplarProperty(store, mall, 0x10, values) && values[0] == 2);
		std::printf("game data: ok\n");
	}
}

int wmain(int argc, wchar_t **argv) {
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	TestModelKeys();
	TestLoadOrder();
	if (argc > 1 && fs::exists(argv[1])) TestGameData(argv[1]);
	else std::printf("game data: skipped (pass the install folder)\n");
	if (argc > 2) {
		AssetStore store;
		auto const t0 = std::chrono::steady_clock::now();
		store.AddPlugins(argv[2]);
		std::printf("plugins: %zu packages, %zu entries, %zu keys, %.0f ms\n", store.Files().size(), store.EntriesSeen(), store.Index().size(),
		            Since(t0));
	}
	std::printf("all passed\n");
	return 0;
}
