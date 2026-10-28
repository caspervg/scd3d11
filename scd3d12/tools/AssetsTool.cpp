/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// SCD3D12Assets: checks the asset readers on real game data.
//
//   SCD3D12Assets COMMAND [--game DIR] [--plugins DIR]... [--package FILE]... ARGS
//     index                       index the folders and print counts and times
//     dump TGI [OUT]              print an entry as hex, or save its bytes
//     obj TGI OUT.obj [FRAME]     save an S3D as OBJ with its materials and textures as PNG
//     png TGI OUT.png [IMAGE]     save an FSH image as PNG
//     models TGI [STATE]          the models an exemplar shows at every zoom and rotation
//     keys FILE                   the exemplars in one package that name models
// The folders are added in the order given, later ones overriding earlier ones like SimCity 4 does.

#include "AssetStore.h"
#include "ImageFile.h"
#include "Models.h"
#include "Textures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <map>
#include <objbase.h>
#include <string>
#include <vector>

using namespace nSCAssets;

namespace {
	std::string Narrow(std::wstring const &w) {
		std::string s;
		for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
		return s;
	}

	bool ParseTgiArg(wchar_t const *text, Tgi &tgi) { return ParseTgi(Narrow(text), tgi); }

	double Ms(std::chrono::steady_clock::time_point since) {
		return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - since).count();
	}

	bool SavePngFile(std::wstring const &path, Texture const &texture) {
		std::vector<uint8_t> rgba;
		if (!TextureToRgba8(texture, 0, rgba)) return false;
		return nSCDWire::SavePng(path.c_str(), texture.levels[0].width, texture.levels[0].height, rgba.data());
	}

	// A quick orthographic look at a model from SimCity 4's usual camera angles (45 degrees down, 22.5 degrees
	// around), textured and alpha tested, so an export can be checked by eye.
	bool SavePreview(std::wstring const &path, Model const &model, std::vector<Texture> const &textures) {
		constexpr int kSize = 512;
		float const yaw = 157.5f * 3.14159265f / 180, pitch = 45.0f * 3.14159265f / 180;
		float const cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch), sp = std::sin(pitch);
		auto project = [&](float const *p, float &x, float &y, float &z) {
			float const rx = p[0] * cy - p[2] * sy, rz = p[0] * sy + p[2] * cy;
			x = rx;
			y = p[1] * cp + rz * sp;
			z = rz * cp - p[1] * sp;
		};
		float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
		for (Mesh const &mesh : model.meshes) {
			for (MeshVertex const &v : mesh.vertices) {
				float x, y, z;
				project(v.position, x, y, z);
				minX = std::min(minX, x), maxX = std::max(maxX, x), minY = std::min(minY, y), maxY = std::max(maxY, y);
			}
		}
		float const scale = (kSize - 16) / std::max({maxX - minX, maxY - minY, 1e-3f});
		std::vector<uint8_t> image(kSize * kSize * 4, 0);
		for (size_t i = 0; i < image.size(); i += 4) image[i] = 40, image[i + 1] = 60, image[i + 2] = 80;
		std::vector<float> depth(kSize * kSize, 1e30f);
		std::vector<uint8_t> rgba;
		for (size_t m = 0; m < model.meshes.size(); ++m) {
			Mesh const &mesh = model.meshes[m];
			Texture const &tex = textures[m];
			bool const textured = !tex.levels.empty() && TextureToRgba8(tex, 0, rgba);
			uint32_t const tw = textured ? tex.levels[0].width : 1, th = textured ? tex.levels[0].height : 1;
			for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
				float sx[3], syy[3], sz[3];
				MeshVertex const *v[3];
				for (int k = 0; k < 3; ++k) {
					v[k] = &mesh.vertices[mesh.indices[i + k]];
					project(v[k]->position, sx[k], syy[k], sz[k]);
					sx[k] = (sx[k] - minX) * scale + 8, syy[k] = kSize - 8 - (syy[k] - minY) * scale;
				}
				float const area = (sx[1] - sx[0]) * (syy[2] - syy[0]) - (sx[2] - sx[0]) * (syy[1] - syy[0]);
				if (std::abs(area) < 1e-6f) continue;
				int const x0 = std::max(0, static_cast<int>(std::min({sx[0], sx[1], sx[2]}))), x1 = std::min(kSize - 1, static_cast<int>(std::max({sx[0], sx[1], sx[2]})));
				int const y0 = std::max(0, static_cast<int>(std::min({syy[0], syy[1], syy[2]}))), y1 = std::min(kSize - 1, static_cast<int>(std::max({syy[0], syy[1], syy[2]})));
				for (int y = y0; y <= y1; ++y) {
					for (int x = x0; x <= x1; ++x) {
						float const px = x + 0.5f, py = y + 0.5f;
						float const w0 = ((sx[1] - px) * (syy[2] - py) - (sx[2] - px) * (syy[1] - py)) / area;
						float const w1 = ((sx[2] - px) * (syy[0] - py) - (sx[0] - px) * (syy[2] - py)) / area;
						float const w2 = 1 - w0 - w1;
						if (w0 < 0 || w1 < 0 || w2 < 0) continue;
						float const z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
						if (z >= depth[y * kSize + x]) continue;
						uint8_t c[4] = {200, 200, 200, 255};
						if (textured) {
							float u = w0 * v[0]->uv[0] + w1 * v[1]->uv[0] + w2 * v[2]->uv[0];
							float t = w0 * v[0]->uv[1] + w1 * v[1]->uv[1] + w2 * v[2]->uv[1];
							u -= std::floor(u), t -= std::floor(t);
							uint8_t const *s = &rgba[(std::min(th - 1, static_cast<uint32_t>(t * th)) * tw + std::min(tw - 1, static_cast<uint32_t>(u * tw))) * 4];
							std::copy(s, s + 4, c);
						}
						if ((mesh.material.flags & kMaterialAlphaTest) && c[3] < 128) continue;
						depth[y * kSize + x] = z;
						std::copy(c, c + 3, &image[(y * kSize + x) * 4]);
					}
				}
			}
		}
		return nSCDWire::SavePng(path.c_str(), kSize, kSize, image.data());
	}

	int Index(AssetStore const &store) {
		std::map<uint32_t, size_t> byType;
		for (auto const &[tgi, ref] : store.Index()) ++byType[tgi.type];
		std::printf("total: %zu files, %zu entries seen, %zu unique keys\n", store.Files().size(), store.EntriesSeen(), store.Index().size());
		for (auto const &[type, count] : byType) {
			if (count >= 1000) std::printf("  type %08X: %zu\n", type, count);
		}
		return 0;
	}

	int Dump(AssetStore const &store, std::vector<std::wstring> const &args) {
		Tgi tgi;
		std::vector<uint8_t> bytes;
		if (args.empty() || !ParseTgiArg(args[0].c_str(), tgi)) return 2;
		DbpfEntry const *entry = nullptr;
		DbpfFile const *file = store.Find(tgi, &entry);
		if (!file || !file->Read(*entry, bytes)) {
			std::printf("cannot read %s\n", ToString(tgi).c_str());
			return 1;
		}
		std::printf("%s from %s, %u bytes stored%s, %zu bytes\n", ToString(tgi).c_str(), Narrow(file->Path()).c_str(), entry->size,
		            entry->compressed ? " compressed" : "", bytes.size());
		if (args.size() > 1) {
			FILE *f = _wfopen(args[1].c_str(), L"wb");
			if (!f) return 1;
			fwrite(bytes.data(), 1, bytes.size(), f);
			fclose(f);
			return 0;
		}
		for (size_t i = 0; i < bytes.size() && i < 4096; i += 16) {
			std::printf("%06zX ", i);
			for (size_t j = i; j < i + 16; ++j) j < bytes.size() ? std::printf(" %02X", bytes[j]) : std::printf("   ");
			std::printf("  ");
			for (size_t j = i; j < i + 16 && j < bytes.size(); ++j) std::putchar(bytes[j] >= 32 && bytes[j] < 127 ? bytes[j] : '.');
			std::printf("\n");
		}
		return 0;
	}

	int Obj(AssetStore const &store, std::vector<std::wstring> const &args) {
		Tgi tgi;
		if (args.size() < 2 || !ParseTgiArg(args[0].c_str(), tgi)) return 2;
		uint32_t const frame = args.size() > 2 ? static_cast<uint32_t>(std::stoul(args[2])) : 0;
		Model model;
		std::string error;
		auto const t0 = std::chrono::steady_clock::now();
		if (!LoadModel(store, tgi, model, frame, &error)) {
			std::printf("%s: %s\n", ToString(tgi).c_str(), error.c_str());
			return 1;
		}
		double const loadMs = Ms(t0);

		std::filesystem::path const objPath = args[1];
		std::filesystem::path mtlPath = objPath;
		mtlPath.replace_extension(L".mtl");
		FILE *obj = _wfopen(objPath.c_str(), L"w");
		FILE *mtl = _wfopen(mtlPath.c_str(), L"w");
		if (!obj || !mtl) return 1;
		std::fprintf(obj, "# %s frame %u of %u\nmtllib %s\n", ToString(tgi).c_str(), frame, model.frameCount, Narrow(mtlPath.filename()).c_str());

		size_t base = 1, triangles = 0;
		std::vector<Texture> textures(model.meshes.size());
		for (size_t m = 0; m < model.meshes.size(); ++m) {
			Mesh const &mesh = model.meshes[m];
			MeshMaterial const &mat = mesh.material;
			std::fprintf(obj, "o %s\nusemtl m%zu\n", mesh.name.c_str(), m);
			std::fprintf(mtl, "newmtl m%zu\nKd 1 1 1\n# flags %04X alpha func %u ref %.3f blend %u %u texture %08X wrap %u %u\n", m, mat.flags,
			             mat.alphaFunc, mat.alphaRef, mat.srcBlend, mat.dstBlend, mat.texture, mat.wrapU, mat.wrapV);
			Texture &texture = textures[m];
			Tgi found;
			if (mat.texture && LoadModelTexture(store, mat.texture, tgi.group, texture, &found)) {
				std::filesystem::path png = objPath;
				png.replace_filename(objPath.stem().wstring() + L"_" + std::to_wstring(m) + L".png");
				if (SavePngFile(png, texture)) std::fprintf(mtl, "map_Kd %s\n", Narrow(png.filename()).c_str());
				std::printf("  mesh %zu %s: %zu vertices, %zu triangles, texture %s %ux%u, %zu levels\n", m, mesh.name.c_str(),
				            mesh.vertices.size(), mesh.indices.size() / 3, ToString(found).c_str(), texture.levels[0].width,
				            texture.levels[0].height, texture.levels.size());
			} else {
				std::printf("  mesh %zu %s: %zu vertices, %zu triangles, texture %08X not found\n", m, mesh.name.c_str(), mesh.vertices.size(),
				            mesh.indices.size() / 3, mat.texture);
			}
			for (MeshVertex const &v : mesh.vertices) std::fprintf(obj, "v %g %g %g\n", v.position[0], v.position[1], v.position[2]);
			for (MeshVertex const &v : mesh.vertices) std::fprintf(obj, "vt %g %g\n", v.uv[0], 1.0f - v.uv[1]);
			for (size_t i = 0; i + 2 < mesh.indices.size(); i += 3) {
				size_t const a = base + mesh.indices[i], b = base + mesh.indices[i + 1], c = base + mesh.indices[i + 2];
				std::fprintf(obj, "f %zu/%zu %zu/%zu %zu/%zu\n", a, a, b, b, c, c);
			}
			base += mesh.vertices.size();
			triangles += mesh.indices.size() / 3;
		}
		std::fclose(obj);
		std::fclose(mtl);
		std::filesystem::path preview = objPath;
		preview.replace_filename(objPath.stem().wstring() + L"_preview.png");
		SavePreview(preview, model, textures);
		std::printf("%s: %zu meshes, %zu triangles, bounds (%.2f %.2f %.2f) to (%.2f %.2f %.2f), loaded in %.2f ms\n", ToString(tgi).c_str(),
		            model.meshes.size(), triangles, model.boundsMin[0], model.boundsMin[1], model.boundsMin[2], model.boundsMax[0],
		            model.boundsMax[1], model.boundsMax[2], loadMs);
		return 0;
	}

	int Png(AssetStore const &store, std::vector<std::wstring> const &args) {
		Tgi tgi;
		if (args.size() < 2 || !ParseTgiArg(args[0].c_str(), tgi)) return 2;
		uint32_t const element = args.size() > 2 ? static_cast<uint32_t>(std::stoul(args[2])) : 0;
		std::vector<uint8_t> bytes;
		Texture texture;
		std::string error;
		if (!store.Read(tgi, bytes) || !DecodeTexture(bytes, texture, element, &error)) {
			std::printf("%s: %s\n", ToString(tgi).c_str(), error.empty() ? "not found" : error.c_str());
			return 1;
		}
		std::printf("%s: code %02X, %ux%u, %zu levels\n", ToString(tgi).c_str(), texture.code, texture.levels[0].width, texture.levels[0].height,
		            texture.levels.size());
		return SavePngFile(args[1], texture) ? 0 : 1;
	}

	int Models(AssetStore const &store, std::vector<std::wstring> const &args) {
		Tgi tgi;
		if (args.empty() || !ParseTgiArg(args[0].c_str(), tgi)) return 2;
		uint32_t const state = args.size() > 1 ? static_cast<uint32_t>(std::stoul(args[1])) : 0;
		std::vector<ModelRef> models;
		if (!OccupantModels(store, tgi, 0, 0, models, state)) {
			std::printf("cannot read exemplar %s\n", ToString(tgi).c_str());
			return 1;
		}
		size_t present = 0, total = 0;
		for (uint32_t zoom = 0; zoom < 5; ++zoom) {
			for (uint32_t rotation = 0; rotation < 4; ++rotation) {
				OccupantModels(store, tgi, zoom, rotation, models, state);
				std::printf("zoom %u rotation %u:", zoom + 1, rotation);
				if (models.empty()) std::printf(" none");
				for (ModelRef const &m : models) {
					bool const found = store.Find(m.tgi) != nullptr;
					present += found;
					++total;
					std::printf(" %s%s", ToString(m.tgi).c_str(), found ? "" : " (missing)");
					if (m.offset[0] || m.offset[1] || m.offset[2]) std::printf(" at (%g %g %g)", m.offset[0], m.offset[1], m.offset[2]);
				}
				std::printf("\n");
			}
		}
		std::printf("%zu of %zu models present\n", present, total);
		return 0;
	}

	int Keys(std::vector<std::wstring> const &args) {
		if (args.empty()) return 2;
		AssetStore store;
		if (!store.AddPackage(args[0])) return 1;
		std::vector<uint32_t> values;
		for (auto const &[tgi, ref] : store.Index()) {
			if (tgi.type != kExemplarType) continue;
			for (uint32_t k = 0; k <= 5; ++k) {
				if (ExemplarProperty(store, tgi, 0x27812820 + k, values)) {
					std::printf("%s RKT%u:", ToString(tgi).c_str(), k);
					for (size_t i = 0; i < values.size() && i < 8; ++i) std::printf(" %08X", values[i]);
					std::printf("%s\n", values.size() > 8 ? " ..." : "");
					break;
				}
			}
		}
		return 0;
	}
}

int wmain(int argc, wchar_t **argv) {
	if (argc < 2) {
		std::printf("usage: SCD3D12Assets index|dump|obj|png|models|keys [--game DIR] [--plugins DIR] [--package FILE] ARGS\n");
		return 2;
	}
	std::wstring const cmd = argv[1];
	if (cmd == L"keys") return Keys(std::vector<std::wstring>(argv + 2, argv + argc));

	AssetStore store;
	std::vector<std::wstring> args;
	for (int i = 2; i < argc; ++i) {
		std::wstring const a = argv[i];
		if ((a == L"--game" || a == L"--plugins" || a == L"--package") && i + 1 < argc) {
			auto const t0 = std::chrono::steady_clock::now();
			size_t const files = store.Files().size(), seen = store.EntriesSeen();
			if (a == L"--game") store.AddGameData(argv[++i]);
			else if (a == L"--plugins") store.AddPlugins(argv[++i]);
			else store.AddPackage(argv[++i]);
			std::printf("%s: %zu packages, %zu entries, %.0f ms\n", Narrow(argv[i]).c_str(), store.Files().size() - files, store.EntriesSeen() - seen,
			            Ms(t0));
		} else {
			args.push_back(a);
		}
	}

	CoInitializeEx(nullptr, COINIT_MULTITHREADED);
	int rc = 2;
	if (cmd == L"index") rc = Index(store);
	else if (cmd == L"dump") rc = Dump(store, args);
	else if (cmd == L"obj") rc = Obj(store, args);
	else if (cmd == L"png") rc = Png(store, args);
	else if (cmd == L"models") rc = Models(store, args);
	if (rc == 2) std::printf("bad arguments\n");
	return rc;
}
