/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "Models.h"

#include "ExemplarReader.h"
#include "S3DReader.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace nSCAssets {
	namespace {
		constexpr uint32_t kRkt0 = 0x27812820;
		constexpr uint32_t kAlternateRkt0 = 0x27812920;
		constexpr uint32_t kZoomMask = 0x0ABFC024;

		// cSC4ModelMakerUtility::nModelLODFromZoom, rows by detail (Mac build 0x00BF2440, Windows 0x00B094A4).
		constexpr uint32_t kModelLodFromZoom[3][5] = {{0, 1, 2, 3, 4}, {0, 1, 2, 3, 3}, {0, 0, 1, 2, 3}};

		uint32_t PackColor(S3D::Vec4 const &c) {
			auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
			return byte(c.x) | byte(c.y) << 8 | byte(c.z) << 16 | byte(c.w) << 24;
		}

		// Expands one PRIM entry into triangle list indices. Without an index block the game draws `length`
		// vertices from `first`.
		void AddTriangles(S3D::Primitive const &prim, S3D::IndexBuffer const *ib, std::vector<uint16_t> &out) {
			auto at = [&](uint32_t i) -> uint16_t { return ib ? ib->indices[prim.first + i] : static_cast<uint16_t>(prim.first + i); };
			uint32_t const n = prim.length;
			switch (prim.type) {
			case 0:
				for (uint32_t i = 0; i + 2 < n; i += 3) out.insert(out.end(), {at(i), at(i + 1), at(i + 2)});
				break;
			case 1:
				for (uint32_t i = 0; i + 2 < n; ++i) {
					if (i & 1) out.insert(out.end(), {at(i + 1), at(i), at(i + 2)});
					else out.insert(out.end(), {at(i), at(i + 1), at(i + 2)});
				}
				break;
			case 2:
				for (uint32_t i = 1; i + 1 < n; ++i) out.insert(out.end(), {at(0), at(i), at(i + 1)});
				break;
			default:
				// Points, lines and quads: none in shipped or plugin data seen so far.
				break;
			}
		}

		std::vector<uint32_t> Words(Exemplar::Property const &p) {
			std::vector<uint32_t> words;
			words.reserve(p.values.size());
			for (auto const &v : p.values) {
				std::visit([&]<typename T>(T const &x) {
					if constexpr (std::is_same_v<T, float>) words.push_back(std::bit_cast<uint32_t>(x));
					else if constexpr (std::is_integral_v<T>) words.push_back(static_cast<uint32_t>(x));
				}, v);
			}
			return words;
		}

		// The exemplar and its parent cohorts, child first. Stops at a missing parent, a repeated key or 32 levels.
		void LoadChain(AssetStore const &store, Tgi tgi, std::vector<Exemplar::Record> &chain) {
			std::vector<uint8_t> bytes;
			std::vector<Tgi> seen;
			while (chain.size() < 32 && !(tgi == Tgi{}) && std::find(seen.begin(), seen.end(), tgi) == seen.end()) {
				seen.push_back(tgi);
				if (!store.Read(tgi, bytes)) break;
				auto record = Exemplar::Parse(bytes);
				if (!record) break;
				tgi = {record->parent.type, record->parent.group, record->parent.instance};
				chain.push_back(std::move(*record));
			}
		}

		Exemplar::Property const *FindInChain(std::vector<Exemplar::Record> const &chain, uint32_t id) {
			for (auto const &record : chain) {
				// The last property with an ID wins inside one record.
				for (size_t i = record.properties.size(); i-- > 0;) {
					if (record.properties[i].id == id) return &record.properties[i];
				}
			}
			return nullptr;
		}
	}

	bool DecodeModel(std::span<uint8_t const> bytes, Model &model, uint32_t frame, std::string *error) {
		model = {};
		auto parsed = S3D::Reader::Parse(bytes);
		if (!parsed) {
			if (error) *error = parsed.error().message;
			return false;
		}
		S3D::Record const &rec = *parsed;
		model.frameCount = rec.animation.frameCount;
		model.frameRate = rec.animation.frameRate;
		if (frame >= rec.animation.frameCount) {
			if (error) *error = "no such frame";
			return false;
		}

		bool any = false;
		for (S3D::AnimatedMesh const &am : rec.animation.animatedMeshes) {
			// The game drops a mesh when any frame names a block that does not exist.
			bool valid = true;
			for (S3D::Frame const &f : am.frames) {
				valid = valid && f.vertBlock < rec.vertexBuffers.size() && f.indexBlock < rec.indexBuffers.size() &&
				        f.primBlock < rec.primitiveBlocks.size() && f.matsBlock < rec.materials.size();
			}
			if (!valid) continue;
			S3D::Frame const &f = am.frames[frame];
			S3D::VertexBuffer const &vb = rec.vertexBuffers[f.vertBlock];
			S3D::IndexBuffer const &ib = rec.indexBuffers[f.indexBlock];
			S3D::Material const &mat = rec.materials[f.matsBlock];

			Mesh mesh;
			mesh.name = am.name;
			for (S3D::Primitive const &prim : rec.primitiveBlocks[f.primBlock]) {
				uint64_t const end = static_cast<uint64_t>(prim.first) + prim.length;
				if (ib.indices.empty() ? end > vb.vertices.size() : end > ib.indices.size()) continue;
				AddTriangles(prim, ib.indices.empty() ? nullptr : &ib, mesh.indices);
			}
			std::erase_if(mesh.indices, [&](uint16_t i) { return i >= vb.vertices.size(); });
			if (mesh.indices.size() % 3 != 0 || mesh.indices.empty()) continue;

			mesh.vertices.reserve(vb.vertices.size());
			for (S3D::Vertex const &v : vb.vertices) {
				mesh.vertices.push_back({{v.position.x, v.position.y, v.position.z}, {v.uv.x, v.uv.y}, PackColor(v.color)});
			}

			MeshMaterial &m = mesh.material;
			m.flags = (mat.flags & 0xFFFF) | kMaterialDepthWrite | kMaterialColorWrite;
			m.alphaFunc = mat.alphaFunc;
			m.depthFunc = mat.depthFunc;
			m.srcBlend = mat.srcBlend;
			m.dstBlend = mat.dstBlend;
			// The game keeps the threshold's high byte and then 4 bits of it.
			uint32_t const high = static_cast<uint32_t>(std::lround(mat.alphaThreshold * 65535.0f)) >> 8;
			m.alphaRef = std::round(high / 255.0f * 15.0f) / 15.0f;
			m.alphaThreshold = static_cast<uint8_t>(high);
			m.index = f.matsBlock;
			// A material whose last texture is 0 has no textures at all.
			if (!mat.textures.empty() && mat.textures.back().textureID != 0) {
				S3D::MaterialTexture const &t = mat.textures.front();
				m.texture = t.textureID;
				m.textureCount = static_cast<uint8_t>(std::min<size_t>(mat.textures.size(), 255));
				m.wrapU = t.wrapS;
				m.wrapV = t.wrapT;
				m.magFilter = t.magFilter;
				m.minFilter = t.minFilter;
			}

			for (uint16_t i : mesh.indices) {
				float const *p = mesh.vertices[i].position;
				for (int k = 0; k < 3; ++k) {
					model.boundsMin[k] = any ? std::min(model.boundsMin[k], p[k]) : p[k];
					model.boundsMax[k] = any ? std::max(model.boundsMax[k], p[k]) : p[k];
				}
				any = true;
			}
			model.meshes.push_back(std::move(mesh));
		}
		return true;
	}

	bool LoadModel(AssetStore const &store, Tgi const &tgi, Model &model, uint32_t frame, std::string *error) {
		std::vector<uint8_t> bytes;
		if (!store.Read(tgi, bytes)) {
			if (error) *error = "not found";
			return false;
		}
		return DecodeModel(bytes, model, frame, error);
	}

	uint32_t ModelRotation(uint32_t cameraRotation, uint32_t orientation, bool ignoreOrientation) {
		return (cameraRotation * 3 + (ignoreOrientation ? 0 : orientation)) & 3;
	}

	void ModelsFromKey(uint32_t property, std::span<uint32_t const> v, uint32_t zoom, uint32_t rotation, uint32_t state,
	                   std::vector<ModelRef> &out, uint32_t lodOffset) {
		if (zoom > 4) return;
		uint32_t const z = kModelLodFromZoom[std::min(lodOffset, 2u)][zoom];
		uint32_t const r = rotation & 3;
		uint32_t const kind = (property & ~0x100u) - kRkt0;

		// One RKT0 to RKT3 key starting at v[at], as GetModelInstanceID reads it. Like the game, an RKT3 record
		// inside RKT4 may read on into the next record.
		auto single = [&](uint32_t k, size_t at, float const *offset, uint32_t add) {
			size_t const index = at + 2 + (k == 2 ? 4 * z + r : k == 3 ? z : 0);
			if (index >= v.size()) return;
			uint32_t instance = v[index];
			if (k == 1 && instance) instance += z * 0x100 + r * 0x10;
			// RKT5 adds the state even to a base of 0.
			instance += add;
			if (!instance) return;
			ModelRef ref{{v[at], v[at + 1], instance}};
			if (offset) std::copy(offset, offset + 3, ref.offset);
			out.push_back(ref);
		};

		if (kind <= 3) {
			single(kind, 0, nullptr, 0);
		} else if (kind == 5) {
			single(1, 0, nullptr, state * 0x10000);
		} else if (kind == 4) {
			size_t at = 0;
			while (v.size() > 4 && at + 5 <= v.size()) {
				uint32_t const k = v[at + 4] - kRkt0;
				size_t const length = k <= 1 ? 3 : k == 2 ? 30 : k == 3 ? 5 : 0;
				if (!length || at + 5 + length > v.size()) break;
				if (v[at] == state) {
					float const offset[3] = {static_cast<int32_t>(v[at + 1]) / 65536.0f, static_cast<int32_t>(v[at + 2]) / 65536.0f,
					                         static_cast<int32_t>(v[at + 3]) / 65536.0f};
					single(k, at + 5, offset, 0);
				}
				at += 5 + length;
			}
		}
	}

	bool OccupantModels(AssetStore const &store, Tgi const &exemplar, uint32_t zoom, uint32_t rotation, std::vector<ModelRef> &out,
	                    uint32_t state, bool alternate, uint32_t lodOffset) {
		out.clear();
		std::vector<Exemplar::Record> chain;
		LoadChain(store, exemplar, chain);
		if (chain.empty()) return false;
		if (Exemplar::Property const *mask = FindInChain(chain, kZoomMask)) {
			std::vector<uint32_t> const words = Words(*mask);
			if (words.empty() || !(words[0] >> (zoom & 31) & 1)) return true;
		}
		uint32_t const base = alternate ? kAlternateRkt0 : kRkt0;
		for (uint32_t k = 0; k <= 5; ++k) {
			if (Exemplar::Property const *key = FindInChain(chain, base + k)) {
				std::vector<uint32_t> const words = Words(*key);
				ModelsFromKey(base + k, words, zoom, rotation, state, out, lodOffset);
				return true;
			}
		}
		return true;
	}

	bool ExemplarProperty(AssetStore const &store, Tgi const &exemplar, uint32_t property, std::vector<uint32_t> &values) {
		std::vector<Exemplar::Record> chain;
		LoadChain(store, exemplar, chain);
		Exemplar::Property const *p = FindInChain(chain, property);
		if (!p) return false;
		values = Words(*p);
		return true;
	}
}
