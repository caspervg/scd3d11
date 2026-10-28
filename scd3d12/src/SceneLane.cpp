/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// How the game draws a cS3DModelInstance (RenderModelInstance 0x007D5710, SetRenderState(material, flags)
// 0x007D5310, RenderMesh 0x007D4A80), as the stream shows it and OpenSC4's draw-context and graphics-formats specs
// describe it:
//
//   LoadMatrix         view x model, one per instance
//   ColorMultiplier    the shade from the lighting manager, or the draw attributes' RGB for a self-lit instance
//   per mesh with vertices, in the model's order:
//     the material's packed state, sent only where it differs from the game's cache:
//       colour write (flag 0x100), depth write (0x80), depth test (0x02), alpha test (0x01), blend (0x10),
//       cull (0x08), texturing on stage 0 when the material has a texture, DepthFunc, BlendFunc, and AlphaFunc
//       with the threshold's high byte / 255 kept as 4 bits q and sent as q x (1/15), only when the function or
//       q change
//     then stage 0 filters linear and linear mipmap linear, wrap repeat, no texture transform on either stage
//     then for flag 0x20: SetTexture of FSH (0x7AB50E44, 0x1ABE787D, id) with every level as the game reads them
//       (Textures.cpp), its wrap and filter bytes
//     a lit building at night (decided per instance by the game) also gets stage 1: the texture id + 0x8000,
//       mode 2 (decal), wrap clamp, filters nearest
//     DrawElements(triangles, the mesh's indices) on the model space vertices (format 0x80004001)
//   ColorMultiplier and AlphaMultiplier(1) put back
//
// Draw attributes (byte 0): bit 1 fades the instance to the opacity a (the float at 16): blending 4 / 5, the
// alpha reference times a and AlphaMultiplier(a) when a < 1. Bit 3 sets the colour multiplier to (a, a, a) and
// cancels the fade. Bit 4 turns the depth test and depth write off, bit 5 tests "greater" without writing.
// A model whose first material blends (8, 1) loses its depth write (cS3DModelInstance::SetModel 0x00801C40).
// Bit 0 (highlighted) uses the highlight state, which the lane leaves to the game.
//
// The stage 0 texture mode is not part of the material: models inherit whatever the game set last.

#include "SceneLane.h"

#include "AssetStore.h"
#include "D3D11Conversions.h"
#include "Models.h"
#include "Textures.h"
#include "VertexFormatUtils.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;
using nSCD3D11::D3D11Vertex;

namespace nSCD3D12 {
	namespace {
		enum Reason : uint8_t {
			kNone,
			kNoModelKey, // the instance has no S3D resource
			kModelMissing, // the store has no such model, or not that frame
			kHighlighted, // drawn with the highlight state
			kExtraDraw, // more draws than the model has meshes
			kMeshMismatch, // the draw's primitive or index count is not the mesh's
			kTwoTextures, // a material with two textures of its own
			kUnboundTexture, // texturing on but no texture binding (flag 0x20 clear)
			kSecondStage, // the game set up stage 1 in a way the lane does not know
			kNightMissing, // the game lit the instance but the store has no night texture
			kTextureMissing, // the store has neither the texture nor the placeholder
			kReasonCount
		};

		char const *const kReasonNames[kReasonCount] = {
			"none", "no_model_key", "model_missing", "highlighted", "extra_draw", "mesh_mismatch",
			"two_textures", "unbound_texture", "second_stage", "night_missing", "texture_missing",
		};

		enum Field : uint8_t {
			kAlphaTest, kDepthTest, kCullFace, kBlend, kColorWrite, kDepthWrite, kDepthFunction, kBlendFactors,
			kAlphaFunction, kStage0Enabled, kStage0Sampler, kStage0Transform, kStage1Enabled, kStage1Sampler,
			kStage1Mode, kStage1Transform, kColorMultiplier, kAlphaMultiplier, kGeometry, kFieldCount
		};

		char const *const kFieldNames[kFieldCount] = {
			"alpha_test", "depth_test", "cull", "blend", "color_write", "depth_write", "depth_func", "blend_factors",
			"alpha_func", "stage0_enabled", "stage0_sampler", "stage0_transform", "stage1_enabled", "stage1_sampler",
			"stage1_mode", "stage1_transform", "color_multiplier", "alpha_multiplier", "geometry",
		};

		constexpr uint32_t kS3dFormat = 0x80004001;
		constexpr uint32_t kNightOffset = 0x8000;
		constexpr uint64_t kChunkBytes = 8ull << 20;
		constexpr D3D12_RESOURCE_STATES kReadable =
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;

		struct ModelKey {
			nSCAssets::Tgi tgi;
			uint32_t frame;
			bool operator==(ModelKey const &) const = default;
		};

		struct ModelKeyHash {
			size_t operator()(ModelKey const &key) const noexcept {
				return nSCAssets::TgiHash{}(key.tgi) ^ (static_cast<size_t>(key.frame) * 0x9E3779B97F4A7C15ull);
			}
		};

		struct GpuMesh {
			D3D12_VERTEX_BUFFER_VIEW vertices{};
			D3D12_INDEX_BUFFER_VIEW indices{};
			uint32_t count = 0;
			nSCAssets::MeshMaterial material;
			Reason reason = kNone;
			// Kept for the check only.
			std::vector<nSCAssets::MeshVertex> cpuVertices;
			std::vector<uint16_t> cpuIndices;
		};

		struct GpuModel {
			Reason reason = kNone;
			uint32_t group = 0;
			std::vector<GpuMesh> meshes;
		};

		struct LoadedTexture {
			uint32_t name = 0;
			bool found = false;
		};

		struct Bracket {
			GpuModel const *model = nullptr;
			Reason reason = kNone;
			uint8_t attributes = 0;
			float opacity = 1.0f;
			uint32_t draws = 0;
			uint32_t substituted = 0;
			Reason firstFallback = kNone;
			nSCDWire::ModelInstance instance{};
		};

		struct Counts {
			uint64_t brackets = 0, full = 0, partial = 0, fallback = 0, empty = 0;
			uint64_t drawsSubstituted = 0;
			uint64_t fallbackDraws[kReasonCount]{};
			uint64_t fallbackBrackets[kReasonCount]{};
			uint64_t stateMismatches[kFieldCount]{};

			void Add(Counts const &other) {
				brackets += other.brackets;
				full += other.full;
				partial += other.partial;
				fallback += other.fallback;
				empty += other.empty;
				drawsSubstituted += other.drawsSubstituted;
				for (uint32_t i = 0; i < kReasonCount; ++i) {
					fallbackDraws[i] += other.fallbackDraws[i];
					fallbackBrackets[i] += other.fallbackBrackets[i];
				}
				for (uint32_t i = 0; i < kFieldCount; ++i) stateMismatches[i] += other.stateMismatches[i];
			}

			uint64_t DrawsFallenBack() const {
				uint64_t total = 0;
				for (uint64_t value: fallbackDraws) total += value;
				return total;
			}
		};

		// The 4 bits of the alpha reference the game's state cache keeps.
		uint32_t AlphaBits(float reference) { return static_cast<uint32_t>(std::nearbyint(reference * 15.0f)) & 15; }

		bool SameMatrix(float const *a, float const *b) { return std::memcmp(a, b, 16 * sizeof(float)) == 0; }
	}

	struct SceneLane::Impl {
		cGDriver12 &driver;
		nSCAssets::AssetStore const &store;
		bool check;
		uint32_t reported = 0;
		std::unordered_map<ModelKey, GpuModel, ModelKeyHash> models;
		// Keyed by group << 32 | instance, one map with the placeholder fallback and one without.
		std::unordered_map<uint64_t, LoadedTexture> textures[2];
		struct Chunk {
			ComPtr<ID3D12Resource> buffer;
			uint64_t used = 0;
		};
		std::vector<Chunk> chunks;
		std::vector<Bracket> stack;
		Counts frame, total;
		std::vector<D3D11Vertex> vertexScratch;
		std::vector<uint8_t> uploadScratch;
		std::vector<uint8_t> rgbaScratch;
		double loadSeconds = 0;
		uint64_t modelsLoaded = 0, texturesLoaded = 0, bytesPlaced = 0;

		Impl(cGDriver12 &driver, nSCAssets::AssetStore const &store, bool check)
			: driver(driver), store(store), check(check) {}

		// Copies bytes into GPU memory the lane keeps for good. Returns the address, or 0.
		D3D12_GPU_VIRTUAL_ADDRESS Place(void const *data, uint64_t bytes) {
			bytes = (bytes + 15) & ~15ull;
			if (chunks.empty() || chunks.back().used + bytes > chunks.back().buffer->GetDesc().Width) {
				Chunk chunk;
				D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
				D3D12_RESOURCE_DESC description{};
				description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				description.Width = (std::max)(kChunkBytes, bytes);
				description.Height = description.DepthOrArraySize = description.MipLevels = 1;
				description.SampleDesc.Count = 1;
				description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				// Created ready for copies, and moved to the readable state after each one.
				if (FAILED(driver.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
				                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				                                                    IID_PPV_ARGS(&chunk.buffer)))) return 0;
				chunks.push_back(std::move(chunk));
			} else {
				Barrier(chunks.back().buffer.Get(), kReadable, D3D12_RESOURCE_STATE_COPY_DEST);
			}
			Chunk &chunk = chunks.back();
			ID3D12Resource *source = nullptr;
			uint64_t offset = 0;
			if (!driver.Upload(data, bytes, source, offset)) return 0;
			driver.CommandList()->CopyBufferRegion(chunk.buffer.Get(), chunk.used, source, offset, bytes);
			Barrier(chunk.buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, kReadable);
			D3D12_GPU_VIRTUAL_ADDRESS const address = chunk.buffer->GetGPUVirtualAddress() + chunk.used;
			chunk.used += bytes;
			bytesPlaced += bytes;
			return address;
		}

		void Barrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = before;
			barrier.Transition.StateAfter = after;
			driver.CommandList()->ResourceBarrier(1, &barrier);
		}

		// The model at that frame, its meshes on the GPU. Loaded once per key, failures included.
		GpuModel const &Model(nSCDWire::ModelInstance const &instance) {
			ModelKey const key{{instance.type, instance.group, instance.instance}, instance.frame};
			auto const found = models.find(key);
			if (found != models.end()) return found->second;
			auto const start = std::chrono::steady_clock::now();
			GpuModel &gpu = models[key];
			gpu.group = instance.group;
			nSCAssets::Model model;
			if (instance.type != nSCAssets::kS3dType) gpu.reason = kNoModelKey;
			else if (!nSCAssets::LoadModel(store, key.tgi, model, instance.frame)) gpu.reason = kModelMissing;
			if (gpu.reason == kNone) {
				// All vertices, then all indices, in one copy.
				uploadScratch.clear();
				std::vector<std::pair<uint64_t, uint64_t> > offsets;
				for (nSCAssets::Mesh const &mesh: model.meshes) {
					vertexScratch.resize(mesh.vertices.size());
					for (size_t i = 0; i < mesh.vertices.size(); ++i) {
						nSCAssets::MeshVertex const &from = mesh.vertices[i];
						D3D11Vertex &to = vertexScratch[i];
						std::memcpy(to.position, from.position, sizeof(to.position));
						to.normal[0] = to.normal[1] = 0.0f;
						to.normal[2] = 1.0f;
						std::memcpy(to.color, &from.color, sizeof(to.color));
						to.texCoord[0][0] = from.uv[0];
						to.texCoord[0][1] = from.uv[1];
						to.texCoord[1][0] = to.texCoord[1][1] = 0.0f;
					}
					uint64_t const vertexOffset = uploadScratch.size();
					auto const *bytes = reinterpret_cast<uint8_t const *>(vertexScratch.data());
					uploadScratch.insert(uploadScratch.end(), bytes, bytes + vertexScratch.size() * sizeof(D3D11Vertex));
					offsets.emplace_back(vertexOffset, 0);
				}
				for (size_t m = 0; m < model.meshes.size(); ++m) {
					uploadScratch.resize((uploadScratch.size() + 3) & ~size_t(3));
					offsets[m].second = uploadScratch.size();
					auto const *bytes = reinterpret_cast<uint8_t const *>(model.meshes[m].indices.data());
					uploadScratch.insert(uploadScratch.end(), bytes, bytes + model.meshes[m].indices.size() * sizeof(uint16_t));
				}
				D3D12_GPU_VIRTUAL_ADDRESS const base =
					uploadScratch.empty() ? 0 : Place(uploadScratch.data(), uploadScratch.size());
				for (size_t m = 0; m < model.meshes.size(); ++m) {
					nSCAssets::Mesh const &mesh = model.meshes[m];
					GpuMesh &out = gpu.meshes.emplace_back();
					out.vertices = {base + offsets[m].first, static_cast<UINT>(mesh.vertices.size() * sizeof(D3D11Vertex)),
					                sizeof(D3D11Vertex)};
					out.indices = {base + offsets[m].second, static_cast<UINT>(mesh.indices.size() * sizeof(uint16_t)),
					               DXGI_FORMAT_R16_UINT};
					out.count = static_cast<uint32_t>(mesh.indices.size());
					out.material = mesh.material;
					if (check) {
						out.cpuVertices = mesh.vertices;
						out.cpuIndices = mesh.indices;
					}
					// A model whose first material blends (8, 1) loses its depth write.
					if (out.material.index == 0 && out.material.srcBlend == 8 && out.material.dstBlend == 1) {
						out.material.flags &= ~nSCAssets::kMaterialDepthWrite;
					}
					if (base == 0) out.reason = kModelMissing;
					else if (out.material.textureCount > 1) out.reason = kTwoTextures;
					else if (out.material.textureCount == 1 && !(out.material.flags & nSCAssets::kMaterialTexturing)) {
						out.reason = kUnboundTexture;
					}
				}
				++modelsLoaded;
			}
			loadSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			return gpu;
		}

		// A texture from the store as a driver texture, made the way the game uploads FSH images: DXT stays
		// compressed, everything else becomes BGRA8, every level the file has. 0 when the store has none.
		// Changes the bound texture of the active stage, so it runs inside a saved draw state.
		uint32_t Texture(uint32_t id, uint32_t group, bool placeholder) {
			LoadedTexture &entry = textures[placeholder ? 1 : 0][static_cast<uint64_t>(group) << 32 | id];
			if (entry.name != 0 && driver.IsTexture(entry.name)) return entry.name;
			if (entry.name == 0 && entry.found) return 0;
			auto const start = std::chrono::steady_clock::now();
			entry = {};
			nSCAssets::Texture texture;
			bool loaded = false;
			if (placeholder) {
				loaded = nSCAssets::LoadModelTexture(store, id, group, texture);
			} else {
				std::vector<uint8_t> bytes;
				for (uint32_t const textureGroup: {nSCAssets::kModelTextureGroup, group}) {
					if (store.Read({nSCAssets::kFshType, textureGroup, id}, bytes) &&
					    nSCAssets::DecodeTexture(bytes, texture)) {
						loaded = true;
						break;
					}
				}
			}
			entry.found = true;
			if (loaded && !texture.levels.empty()) {
				uint32_t internalFormat = 4, sourceFormat = 3, sourceType = 1;
				switch (texture.format) {
					case nSCAssets::TextureFormat::Bc1: internalFormat = 5, sourceFormat = 7, sourceType = 0; break;
					case nSCAssets::TextureFormat::Bc2: internalFormat = 6, sourceFormat = 9, sourceType = 0; break;
					case nSCAssets::TextureFormat::Bc3: internalFormat = 7, sourceFormat = 10, sourceType = 0; break;
					case nSCAssets::TextureFormat::Rgba8: break;
				}
				nSCAssets::TextureLevel const &top = texture.levels[0];
				uint32_t const name = static_cast<uint32_t>(driver.CreateTexture(
					internalFormat, top.width, top.height, static_cast<uint32_t>(texture.levels.size()), 2));
				for (size_t level = 0; name != 0 && level < texture.levels.size(); ++level) {
					nSCAssets::TextureLevel const &l = texture.levels[level];
					void const *pixels = l.data.data();
					if (texture.format == nSCAssets::TextureFormat::Rgba8) {
						rgbaScratch = l.data;
						for (size_t i = 0; i + 3 < rgbaScratch.size(); i += 4) std::swap(rgbaScratch[i], rgbaScratch[i + 2]);
						pixels = rgbaScratch.data();
					}
					driver.LoadTextureLevel(name, static_cast<int32_t>(level), 0, 0, static_cast<int32_t>(l.width),
					                        static_cast<int32_t>(l.height), sourceFormat, sourceType, 0, pixels);
				}
				entry.name = name;
				++texturesLoaded;
			}
			loadSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
			return entry.name;
		}

		// Whether the game's draw reads the same positions and texture coordinates, in the same order, as the
		// lane's copy of the mesh.
		bool SameGeometry(cGDriver12::DrawState const &game, GpuMesh const &mesh, int32_t first, uint32_t indexType,
		                  void const *indices) const {
			uint32_t const format = game.vertexFormat;
			if (game.vertexPointer == nullptr || RZVertexFormatNumElements(format, kGDElementType_TexCoord) == 0) {
				return false;
			}
			uint32_t const uvOffset = RZVertexFormatElementOffset(format, kGDElementType_TexCoord, 0);
			for (uint32_t i = 0; i < mesh.count; ++i) {
				uint32_t index = static_cast<uint32_t>(first) + i;
				if (indexType == 3) index = static_cast<uint16_t const *>(indices)[i];
				else if (indexType == 5) index = static_cast<uint32_t const *>(indices)[i];
				uint8_t const *vertex = game.vertexPointer + static_cast<size_t>(index) * game.vertexStride;
				nSCAssets::MeshVertex const &own = mesh.cpuVertices[mesh.cpuIndices[i]];
				if (std::memcmp(vertex, own.position, sizeof(own.position)) != 0 ||
				    std::memcmp(vertex + uvOffset, own.uv, sizeof(own.uv)) != 0) return false;
			}
			return true;
		}

		// Stands in for the game's draw of a mesh. Returns the reason when it cannot.
		Reason Draw(Bracket const &bracket, GpuMesh const &mesh, int32_t first, uint32_t indexType, void const *indices) {
			cGDriver12::DrawState game{};
			driver.SaveDrawState(game);
			if (check && !SameGeometry(game, mesh, first, indexType, indices)) {
				++frame.stateMismatches[kGeometry];
				if (reported++ < 20) {
					std::printf("scene: geometry differs: %08X-%08X-%08X frame %u mesh %u\n", bracket.instance.type,
					            bracket.instance.group, bracket.instance.instance, bracket.instance.frame,
					            static_cast<uint32_t>(&mesh - bracket.model->meshes.data()));
				}
			}
			nSCAssets::MeshMaterial const &material = mesh.material;

			// Night lights: the game decides per instance and shows it by putting the night texture on stage 1.
			cGDriver12::DrawState::Stage const &second = game.stages[1];
			bool const lit = second.enabled;
			if (lit && (second.mode != 2 || second.coordinates != 0 || material.texture == 0)) return kSecondStage;

			uint32_t day = 0, night = 0;
			if (material.textureCount != 0) {
				day = Texture(material.texture, bracket.model->group, true);
				if (day == 0) {
					driver.RestoreDrawState(game);
					return kTextureMissing;
				}
			}
			if (lit) {
				night = Texture(material.texture + kNightOffset, bracket.model->group, false);
				if (night == 0) {
					driver.RestoreDrawState(game);
					return kNightMissing;
				}
			}

			uint32_t const flags = material.flags;
			uint8_t const attributes = bracket.attributes;
			float const a = bracket.opacity;
			bool const grey = (attributes & 8) != 0;
			bool const fade = (attributes & 2) != 0 && !grey;
			bool depthTest = (flags & nSCAssets::kMaterialDepthTest) != 0;
			if (attributes & 0x20) depthTest = true;
			if (attributes & 0x10) depthTest = false;
			bool const depthWrite = (flags & nSCAssets::kMaterialDepthWrite) != 0 && !(attributes & 0x30);
			// The reference is the threshold's high byte / 255, times a when fading, kept as 4 bits and sent back
			// as q x (1/15). The game's cache sends AlphaFunc only when the function or those bits change, so the
			// driver can still hold an unrounded reference some other draw set, and that is the one the test uses.
			float threshold = material.alphaThreshold * (1.0f / 255.0f);
			if (fade && a < 1.0f) threshold *= a;
			uint32_t const alphaBits = AlphaBits(threshold);
			float alphaReference = static_cast<float>(alphaBits) * (1.0f / 15.0f);
			if (game.alphaFunction == material.alphaFunc && AlphaBits(game.alphaReference) == alphaBits) {
				alphaReference = game.alphaReference;
			}
			if (fade && a < 1.0f) driver.AlphaMultiplier(a);
			if (grey) driver.ColorMultiplier(a, a, a);

			auto const set = [&](uint32_t capability, bool on) {
				if (on) driver.Enable(capability);
				else driver.Disable(capability);
			};
			driver.ColorMask((flags & nSCAssets::kMaterialColorWrite) != 0);
			driver.DepthMask(depthWrite);
			set(kGDCapability_DepthTest, depthTest);
			set(kGDCapability_AlphaTest, (flags & nSCAssets::kMaterialAlphaTest) != 0);
			set(kGDCapability_Blend, (flags & nSCAssets::kMaterialBlend) != 0 || fade);
			set(kGDCapability_CullFace, (flags & nSCAssets::kMaterialBackfaceCulling) != 0);
			driver.DepthFunc((attributes & 0x20) ? 4 : material.depthFunc);
			driver.AlphaFunc(material.alphaFunc, alphaReference);
			if (fade) driver.BlendFunc(4, 5);
			else driver.BlendFunc(material.srcBlend, material.dstBlend);

			driver.TexStage(1);
			set(kGDCapability_Texture2D, lit);
			driver.TexStageCoord(0);
			driver.TexStageMatrix(nullptr, 3, 2, 1);
			if (lit) {
				driver.SetTexture(night, 1);
				driver.TexParameter(0, 0, 0);
				driver.TexParameter(0, 1, 0);
				driver.TexParameter(0, 2, 2);
				driver.TexParameter(0, 3, 2);
				driver.TexEnv(0, 0, 2);
			}
			driver.TexStage(0);
			set(kGDCapability_Texture2D, day != 0);
			driver.TexStageCoord(0);
			driver.TexStageMatrix(nullptr, 3, 2, 1);
			if (day != 0) {
				driver.SetTexture(day, 0);
				driver.TexParameter(0, 0, material.magFilter);
				driver.TexParameter(0, 1, material.minFilter);
				driver.TexParameter(0, 2, material.wrapU);
				driver.TexParameter(0, 3, material.wrapV);
			}

			if (check) {
				cGDriver12::DrawState lane{};
				driver.SaveDrawState(lane);
				Compare(game, lane);
			}
			driver.DrawTriangles(mesh.vertices, mesh.indices, mesh.count, kS3dFormat);
			driver.RestoreDrawState(game);
			return kNone;
		}

		// Counts where the lane's state differs from what the game set for the same draw. Textures are
		// compared by what they hold, in the images, as the names differ.
		void Compare(cGDriver12::DrawState const &game, cGDriver12::DrawState const &lane) {
			uint64_t *const miss = frame.stateMismatches;
			miss[kAlphaTest] += game.alphaTest != lane.alphaTest;
			miss[kDepthTest] += game.depthTest != lane.depthTest;
			miss[kCullFace] += game.cullFace != lane.cullFace;
			miss[kBlend] += game.blend != lane.blend;
			miss[kColorWrite] += game.colorWrite != lane.colorWrite;
			miss[kDepthWrite] += game.depthWrite != lane.depthWrite;
			miss[kDepthFunction] += game.depthFunction != lane.depthFunction;
			miss[kBlendFactors] += lane.blend && (game.sourceBlend != lane.sourceBlend ||
			                                      game.destinationBlend != lane.destinationBlend);
			miss[kAlphaFunction] += lane.alphaTest && (game.alphaFunction != lane.alphaFunction ||
			                                           std::memcmp(&game.alphaReference, &lane.alphaReference,
			                                                       sizeof(float)) != 0);
			for (uint32_t i = 0; i < 2; ++i) {
				cGDriver12::DrawState::Stage const &g = game.stages[i];
				cGDriver12::DrawState::Stage const &l = lane.stages[i];
				miss[i == 0 ? kStage0Enabled : kStage1Enabled] += g.enabled != l.enabled;
				if (!l.enabled) continue;
				miss[i == 0 ? kStage0Sampler : kStage1Sampler] +=
					g.magFilter != l.magFilter || g.minFilter != l.minFilter || g.wrapU != l.wrapU || g.wrapV != l.wrapV;
				miss[i == 0 ? kStage0Transform : kStage1Transform] +=
					g.coordinates != l.coordinates || !SameMatrix(g.matrix, l.matrix);
				if (i == 1) miss[kStage1Mode] += g.mode != l.mode;
			}
			miss[kColorMultiplier] += std::memcmp(game.globalAmbient, lane.globalAmbient, sizeof(game.globalAmbient)) != 0;
			miss[kAlphaMultiplier] += game.materialDiffuse[3] != lane.materialDiffuse[3] ||
			                          game.diffuseFromVertex != lane.diffuseFromVertex;
		}

		void Close(Bracket const &bracket) {
			if (bracket.model == nullptr && bracket.reason == kNone) return;
			++frame.brackets;
			if (bracket.draws == 0) ++frame.empty;
			else if (bracket.substituted == bracket.draws) ++frame.full;
			else if (bracket.substituted != 0) ++frame.partial;
			else {
				++frame.fallback;
				++frame.fallbackBrackets[bracket.firstFallback];
			}
		}
	};

	SceneLane::SceneLane(cGDriver12 &driver, nSCAssets::AssetStore const &store, bool check)
		: impl(std::make_unique<Impl>(driver, store, check)) {
		driver.SetDrawFilter(this);
	}

	SceneLane::~SceneLane() { impl->driver.SetDrawFilter(nullptr); }

	void SceneLane::OwnerBegin(nSCDWire::DrawOwner const &) { impl->stack.emplace_back(); }

	void SceneLane::OwnerEnd() {
		if (impl->stack.empty()) return;
		impl->Close(impl->stack.back());
		impl->stack.pop_back();
	}

	void SceneLane::ModelInstance(nSCDWire::ModelInstance const &model) {
		if (impl->stack.empty()) return;
		Bracket &bracket = impl->stack.back();
		bracket.instance = model;
		bracket.attributes = model.attributes[0];
		std::memcpy(&bracket.opacity, model.attributes + 16, sizeof(bracket.opacity));
		GpuModel const &gpu = impl->Model(model);
		bracket.model = &gpu;
		bracket.reason = gpu.reason;
		if (bracket.reason == kNone && (bracket.attributes & 1)) bracket.reason = kHighlighted;
	}

	bool SceneLane::AllowDraw(uint32_t primitive, int32_t first, uint32_t count, uint32_t indexType,
	                          void const *indices) {
		Impl &d = *impl;
		if (d.stack.empty()) return true;
		Bracket &bracket = d.stack.back();
		if (bracket.model == nullptr) return true;
		uint32_t const index = bracket.draws++;
		Reason reason = bracket.reason;
		GpuMesh const *mesh = nullptr;
		if (reason == kNone) {
			if (index >= bracket.model->meshes.size()) reason = kExtraDraw;
			else {
				mesh = &bracket.model->meshes[index];
				if (primitive != 0 || count != mesh->count) reason = kMeshMismatch;
				else reason = mesh->reason;
			}
		}
		if (reason == kNone) reason = d.Draw(bracket, *mesh, first, indexType, indices);
		if (reason == kNone) {
			++bracket.substituted;
			++d.frame.drawsSubstituted;
			return false;
		}
		if (bracket.firstFallback == kNone) bracket.firstFallback = reason;
		++d.frame.fallbackDraws[reason];
		return true;
	}

	void SceneLane::WriteCsvHeader(std::FILE *csv) {
		std::fprintf(csv, "frame,brackets,full,partial,fallback,empty,draws_substituted,draws_fallback");
		for (uint32_t i = 1; i < kReasonCount; ++i) std::fprintf(csv, ",%s", kReasonNames[i]);
		std::fprintf(csv, "\n");
	}

	void SceneLane::EndFrame(uint64_t frameNumber, std::FILE *csv) {
		Impl &d = *impl;
		Counts const &c = d.frame;
		if (csv != nullptr) {
			std::fprintf(csv, "%llu,%llu,%llu,%llu,%llu,%llu,%llu,%llu", static_cast<unsigned long long>(frameNumber),
			             static_cast<unsigned long long>(c.brackets), static_cast<unsigned long long>(c.full),
			             static_cast<unsigned long long>(c.partial), static_cast<unsigned long long>(c.fallback),
			             static_cast<unsigned long long>(c.empty), static_cast<unsigned long long>(c.drawsSubstituted),
			             static_cast<unsigned long long>(c.DrawsFallenBack()));
			for (uint32_t i = 1; i < kReasonCount; ++i) {
				std::fprintf(csv, ",%llu", static_cast<unsigned long long>(c.fallbackDraws[i]));
			}
			std::fprintf(csv, "\n");
		}
		d.total.Add(c);
		d.frame = {};
	}

	void SceneLane::PrintTotals() const {
		Impl const &d = *impl;
		Counts const &c = d.total;
		auto const u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
		std::printf("scene: %llu model brackets: %llu substituted, %llu partly, %llu left to the game, %llu without draws\n",
		            u(c.brackets), u(c.full), u(c.partial), u(c.fallback), u(c.empty));
		std::printf("scene: %llu draws substituted, %llu left to the game\n", u(c.drawsSubstituted), u(c.DrawsFallenBack()));
		for (uint32_t i = 1; i < kReasonCount; ++i) {
			if (c.fallbackDraws[i] != 0 || c.fallbackBrackets[i] != 0) {
				std::printf("scene:   %-16s %llu draws, %llu whole brackets\n", kReasonNames[i], u(c.fallbackDraws[i]),
				            u(c.fallbackBrackets[i]));
			}
		}
		for (uint32_t i = 0; i < kFieldCount; ++i) {
			if (c.stateMismatches[i] != 0) {
				std::printf("scene:   differs: %-16s %llu draws\n", kFieldNames[i], u(c.stateMismatches[i]));
			}
		}
		std::printf("scene: %llu models and %llu textures loaded in %.2f s, %.1f MB of geometry on the GPU\n",
		            u(d.modelsLoaded), u(d.texturesLoaded), d.loadSeconds, static_cast<double>(d.bytesPlaced) / (1 << 20));
	}
}
