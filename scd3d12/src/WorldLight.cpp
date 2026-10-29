/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "WorldLight.h"

#include "RetainedScene.h"
#include "SceneLane.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <vector>

#ifdef SCD3D12_RAY_SHADERS
#include "EmissiveCS.h"
#include "FullScreenVS.h"
#include "TraceSunPS.h"
#endif

using Microsoft::WRL::ComPtr;

namespace nSCD3D12 {
	namespace {
		// The view object that draws the terrain (and in pass 2 the water).
		constexpr uint32_t kTerrainView = 0x00AB4480;
		// SC4's terrain grid: 16 metre cells, at most 256 a side (a large city, 4 km).
		constexpr float kCellMetres = 16.0f;
		constexpr uint32_t kFieldVertices = 257;
		constexpr float kUnknownHeight = -1.0e6f;
		// New heights reach the terrain field and the terrain's structure at most this often, in frames.
		constexpr uint64_t kTerrainInterval = 8;
		constexpr uint32_t kScratchSlots = 3;
		constexpr uint64_t kStructureAlignment = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;
		// Bottom level structures of the meshes live in pools this big.
		constexpr uint64_t kPoolBytes = 32ull << 20;
		// At most this many meshes get a structure, and get their glow read, per frame. The rest wait.
		constexpr uint32_t kBuildsPerFrame = 1024;
		constexpr uint32_t kEmittersPerFrame = 256;
		constexpr uint32_t kEmitterCapacity = 16384;
		// What EmissiveCS writes per mesh: per side, the glow-weighted positions and the glow.
		constexpr uint32_t kEmitterFloats = 12 * 4;
		// The sun's radius (the tangent of its angle, wide enough for a soft edge), the longest ray in metres,
		// and how far off the surface a ray starts, in metres and in pixels.
		constexpr float kSunRadius = 0.02f;
		constexpr float kLongestRay = 3000.0f;
		constexpr float kRayOffset = 0.05f;
		constexpr float kRayOffsetPixels = 0.35f;
		// The terrain casts from this far off the surface only (see TraceSunPS).
		constexpr float kTerrainClearance = 8.0f;
		// Instance masks: the models and the terrain are traced apart.
		constexpr uint8_t kModelsMask = 1;
		constexpr uint8_t kTerrainMask = 2;
		// Night lights. A side of a lit mesh glows with the emitted light of its night texture summed over its
		// area (square metres of full glow). Sides with less give no light. A light stands this far out from
		// its side, reaches further with more glow, between the two radii, and is this bright where it stands.
		constexpr float kLeastGlow = 0.05f;
		constexpr float kLightOffset = 1.5f;
		constexpr float kRadiusPerGlow = 9.0f;
		constexpr float kSmallestRadius = 8.0f;
		constexpr float kLargestRadius = 45.0f;
		constexpr float kLightStrength = 0.6f;
		constexpr uint32_t kTileSize = 32;
		constexpr uint32_t kLightsPerTile = 128;
		constexpr uint32_t kMostLights = 32768;

		// Column major, as OpenGL: m[column * 4 + row].
		void Multiply(float const *a, float const *b, float *out) {
			float r[16];
			for (int c = 0; c < 4; ++c) {
				for (int row = 0; row < 4; ++row) {
					r[c * 4 + row] = a[row] * b[c * 4] + a[4 + row] * b[c * 4 + 1] + a[8 + row] * b[c * 4 + 2] +
					                 a[12 + row] * b[c * 4 + 3];
				}
			}
			std::memcpy(out, r, sizeof(r));
		}

		bool Invert(float const *m, float *out) {
			double a[4][8];
			for (int row = 0; row < 4; ++row) {
				for (int c = 0; c < 4; ++c) {
					a[row][c] = m[c * 4 + row];
					a[row][c + 4] = row == c ? 1.0 : 0.0;
				}
			}
			for (int c = 0; c < 4; ++c) {
				int pivot = c;
				for (int row = c + 1; row < 4; ++row) {
					if (std::fabs(a[row][c]) > std::fabs(a[pivot][c])) pivot = row;
				}
				if (std::fabs(a[pivot][c]) < 1e-30) return false;
				std::swap(a[c], a[pivot]);
				double const scale = 1.0 / a[c][c];
				for (double &value: a[c]) value *= scale;
				for (int row = 0; row < 4; ++row) {
					if (row == c) continue;
					double const factor = a[row][c];
					for (int k = 0; k < 8; ++k) a[row][k] -= factor * a[c][k];
				}
			}
			for (int row = 0; row < 4; ++row) {
				for (int c = 0; c < 4; ++c) out[c * 4 + row] = static_cast<float>(a[row][c + 4]);
			}
			return true;
		}

		uint64_t AlignUp(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

		struct MeshKey {
			D3D12_GPU_VIRTUAL_ADDRESS vertices, indices;
			uint32_t count;
			bool operator==(MeshKey const &) const = default;
		};

		struct MeshKeyHash {
			size_t operator()(MeshKey const &key) const noexcept {
				uint64_t h = key.vertices * 0x9E3779B97F4A7C15ull;
				h ^= key.indices + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
				h ^= key.count + (h << 6) + (h >> 2);
				return static_cast<size_t>(h);
			}
		};

		// A mesh of the asset store's, as the structures and the emitter pass need it.
		struct MeshEntry {
			// Its bottom level structure, 0 until it has one.
			D3D12_GPU_VIRTUAL_ADDRESS structure = 0;
			bool failed = false;
			// The raw view of the buffer holding its vertices and indices, and their offsets, once looked up.
			bool located = false;
			uint32_t buffer = 0;
			uint32_t vertexOffset = 0, indexOffset = 0;
		};

		// The glow of a mesh under one night texture.
		struct Emitter {
			uint32_t slot = 0;
			// The frame whose commands read it, and whether its result has come back.
			uint64_t frame = 0;
			bool read = false;
			float sides[kEmitterFloats]{};
		};

		// What an alpha-tested hit looks up, by instance id. Matches Mesh in WorldLight.hlsl.
		struct HitMesh {
			uint32_t texture, buffer, vertices, indices;
			float reference;
			uint32_t padding[3];
		};
		static_assert(sizeof(HitMesh) == 32);

		struct Build {
			D3D12_RAYTRACING_GEOMETRY_DESC geometry;
			D3D12_GPU_VIRTUAL_ADDRESS result;
			uint64_t scratch;
		};

		struct Light {
			float place[4];
			float color[4];
		};

		struct Totals {
			uint64_t frames = 0, structuresBuilt = 0, topBuilds = 0, terrainBuilds = 0, fieldUploads = 0;
			uint64_t instances = 0, alphaInstances = 0, emittersRead = 0, lights = 0, litMeshes = 0;
			uint64_t poolBytes = 0;
			std::vector<float> cpuMilliseconds;
		};
	}

	struct WorldLight::Impl {
		cGDriver12 &driver;
		Options options;
		RetainedScene const *retained = nullptr;
		SceneLane const *lane = nullptr;
		ComPtr<ID3D12Device5> device;
		ComPtr<ID3D12GraphicsCommandList4> list;
		bool raysReady = false;
		ComPtr<ID3D12Fence> fence;
		HANDLE fenceEvent = nullptr;
		uint64_t frameNumber = 0;
		std::vector<std::pair<uint64_t, ComPtr<ID3D12Resource> > > garbage;

		// The camera SC4 drew this frame's dynamic passes with.
		bool cameraRead = false;
		float projection[16]{}, view[16]{};
		int32_t width = 0, height = 0;
		std::vector<nSCDWire::DrawOwner> owners;

		// The terrain: heights by vertex (row by row in z), the frame each was last drawn in, the diagonal each
		// cell is split along (1 from its lowest corner to its highest, 2 the other, 0 not known).
		std::vector<float> heights = std::vector<float>(kFieldVertices * kFieldVertices, kUnknownHeight);
		std::vector<float> drawnHeights = std::vector<float>(kFieldVertices * kFieldVertices);
		// The size in cells of the triangles each height came from: kept, and drawn this frame.
		std::vector<uint8_t> spans = std::vector<uint8_t>(kFieldVertices * kFieldVertices, 255);
		std::vector<uint8_t> drawnSpans = std::vector<uint8_t>(kFieldVertices * kFieldVertices);
		std::vector<uint64_t> drawnIn = std::vector<uint64_t>(kFieldVertices * kFieldVertices);
		std::vector<uint32_t> drawnThisFrame;
		std::vector<uint8_t> diagonals = std::vector<uint8_t>((kFieldVertices - 1) * (kFieldVertices - 1));
		bool terrainChanged = false;
		uint64_t terrainUpdated = 0;
		ComPtr<ID3D12Resource> field;
		bool fieldReady = false;
		// Two structures of the terrain and two of the city: the trace uses the current ones while the next are
		// built. A structure traced in the command list that built it now and then hung this GPU (an RTX 5080).
		ComPtr<ID3D12Resource> terrainStructures[2], terrainInputs;
		D3D12_RAYTRACING_GEOMETRY_DESC terrainGeometry{};
		uint32_t terrainCurrent = 0;
		bool terrainSwap = false;
		bool terrainStructureDirty = false;

		// Ray tracing.
		ComPtr<ID3D12RootSignature> rootSignature;
		ComPtr<ID3D12PipelineState> tracePipeline, emissivePipeline;
		std::unordered_map<MeshKey, MeshEntry, MeshKeyHash> meshes;
		std::unordered_map<ID3D12Resource *, uint32_t> bufferViews;
		std::vector<ComPtr<ID3D12Resource> > pools;
		uint64_t poolUsed = 0;
		ComPtr<ID3D12Resource> scratch[kScratchSlots];
		uint64_t scratchFrame[kScratchSlots]{};
		struct City {
			ComPtr<ID3D12Resource> structure;
			bool ready = false;
			// What it was built from: the terrain, the instances, and per alpha-tested instance the hit mesh and
			// its texture.
			D3D12_GPU_VIRTUAL_ADDRESS ground = 0;
			std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
			std::vector<HitMesh> hits;
			std::vector<uint32_t> textures;
		};
		City cities[2];
		uint32_t cityCurrent = 0;
		bool citySwap = false;
		// This frame's instances and hit table, and the hit table the trace reads.
		std::vector<D3D12_RAYTRACING_INSTANCE_DESC> instances;
		std::vector<HitMesh> hitTable;
		std::vector<uint32_t> hitTextures;
		D3D12_GPU_VIRTUAL_ADDRESS hitMeshes = 0;
		// The retained scene's changes when it was last visited, and whether the next frame visits it anyway.
		uint64_t visitedChanges = 0;
		bool revisit = true;
		float darkness = 0.0f;
		uint32_t litMeshes = 0;
		std::vector<Build> builds;

		// Night lights.
		struct EmitterKey {
			D3D12_GPU_VIRTUAL_ADDRESS indices;
			uint32_t count, texture;
			bool operator==(EmitterKey const &) const = default;
		};
		struct EmitterKeyHash {
			size_t operator()(EmitterKey const &key) const noexcept {
				return static_cast<size_t>(key.indices * 0x9E3779B97F4A7C15ull ^ (uint64_t{key.count} << 32 | key.texture));
			}
		};
		std::unordered_map<EmitterKey, Emitter, EmitterKeyHash> emitters;
		std::vector<Emitter *> pendingEmitters;
		uint32_t nextEmitter = 0;
		ComPtr<ID3D12Resource> emitterOutput, emitterReadback;
		float const *emitterResults = nullptr;
		struct EmitterJob {
			uint32_t constants[8];
		};
		std::vector<EmitterJob> jobs;
		std::vector<Light> lights;
		std::vector<uint32_t> tiles;

		Totals totals;

		Impl(cGDriver12 &driver, Options const &options) : driver(driver), options(options) {
			driver.Device()->QueryInterface(IID_PPV_ARGS(&device));
			driver.CommandList()->QueryInterface(IID_PPV_ARGS(&list));
			if (FAILED(driver.Device()->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return;
			fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			raysReady = (options.rays || options.lights) && CreateRayTracing();
		}

		~Impl() {
			if (fence) {
				driver.Queue()->Signal(fence.Get(), frameNumber + 1);
				WaitFor(frameNumber + 1);
			}
			if (fenceEvent != nullptr) CloseHandle(fenceEvent);
		}

		// Waits for the GPU to finish frame `value`, whose completion BeginLighting of a later frame signals.
		void WaitFor(uint64_t value) {
			if (fence->GetCompletedValue() >= value) return;
			fence->SetEventOnCompletion(value, fenceEvent);
			WaitForSingleObject(fenceEvent, INFINITE);
		}

		// Keeps a resource until the GPU is past the frame being recorded.
		void Retire(ComPtr<ID3D12Resource> &resource) {
			if (resource) garbage.emplace_back(frameNumber, std::move(resource));
			resource.Reset();
		}

		bool CreateBuffer(D3D12_HEAP_TYPE type, uint64_t bytes, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource> &buffer,
		                  bool unordered = false) {
			D3D12_HEAP_PROPERTIES const heap{type};
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			description.Width = bytes;
			description.Height = description.DepthOrArraySize = description.MipLevels = 1;
			description.SampleDesc.Count = 1;
			description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			if (unordered) description.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
			return SUCCEEDED(driver.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description, state,
			                                                         nullptr, IID_PPV_ARGS(&buffer)));
		}

		void Barrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition.pResource = resource;
			barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			barrier.Transition.StateBefore = before;
			barrier.Transition.StateAfter = after;
			list->ResourceBarrier(1, &barrier);
		}

		void UnorderedBarrier() {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
			list->ResourceBarrier(1, &barrier);
		}

		D3D12_GPU_VIRTUAL_ADDRESS Upload(void const *data, uint64_t bytes) {
			ID3D12Resource *resource = nullptr;
			uint64_t offset = 0;
			if (!driver.Upload(data, bytes, resource, offset)) return 0;
			return resource->GetGPUVirtualAddress() + offset;
		}

		// The root signature both ray shaders share, their pipelines, and the emitter buffers.
		bool CreateRayTracing() {
#ifndef SCD3D12_RAY_SHADERS
			return false;
#else
			D3D12_FEATURE_DATA_D3D12_OPTIONS5 features{};
			if (!device || !list ||
			    FAILED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &features, sizeof(features))) ||
			    features.RaytracingTier < D3D12_RAYTRACING_TIER_1_1) {
				std::puts("world light: this GPU has no DXR 1.1, shadows stay traced in screen space");
				return false;
			}
			// The driver's constants and lighting views, the city structure, the hit meshes, every texture and raw
			// buffer of the driver's heap, the job's constants and the emitter output.
			D3D12_DESCRIPTOR_RANGE const lighting{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 5, 0, 0, 0};
			D3D12_DESCRIPTOR_RANGE const textures{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 1, 0};
			D3D12_DESCRIPTOR_RANGE const buffers{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, UINT_MAX, 0, 2, 0};
			D3D12_ROOT_PARAMETER parameters[8]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
			parameters[0].Descriptor = {0, 0};
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[1].DescriptorTable = {1, &lighting};
			parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			parameters[2].Descriptor = {7, 0};
			parameters[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
			parameters[3].Descriptor = {8, 0};
			parameters[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[4].DescriptorTable = {1, &textures};
			parameters[5].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[5].DescriptorTable = {1, &buffers};
			parameters[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			parameters[6].Constants = {1, 0, 8};
			parameters[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
			parameters[7].Descriptor = {0, 0};
			D3D12_STATIC_SAMPLER_DESC sampler{};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
			D3D12_ROOT_SIGNATURE_DESC const signature{8, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
			ComPtr<ID3DBlob> serialized, errors;
			if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
			    FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
			                                       IID_PPV_ARGS(&rootSignature)))) {
				if (errors) std::printf("world light: %s\n", static_cast<char const *>(errors->GetBufferPointer()));
				return false;
			}
			D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
			description.pRootSignature = rootSignature.Get();
			description.VS = {kFullScreenVS, sizeof(kFullScreenVS)};
			description.PS = {kTraceSunPS, sizeof(kTraceSunPS)};
			description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			description.SampleMask = UINT_MAX;
			description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			description.RasterizerState.DepthClipEnable = TRUE;
			description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			description.NumRenderTargets = 1;
			description.RTVFormats[0] = DXGI_FORMAT_R8_UNORM;
			description.SampleDesc.Count = 1;
			if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&tracePipeline)))) {
				std::puts("world light: cannot create the sunlight trace");
				return false;
			}
			D3D12_COMPUTE_PIPELINE_STATE_DESC compute{};
			compute.pRootSignature = rootSignature.Get();
			compute.CS = {kEmissiveCS, sizeof(kEmissiveCS)};
			if (FAILED(device->CreateComputePipelineState(&compute, IID_PPV_ARGS(&emissivePipeline)))) return false;
			uint64_t const bytes = uint64_t{kEmitterCapacity} * kEmitterFloats * sizeof(float);
			if (!CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, emitterOutput, true) ||
			    !CreateBuffer(D3D12_HEAP_TYPE_READBACK, bytes, D3D12_RESOURCE_STATE_COPY_DEST, emitterReadback)) {
				return false;
			}
			void *mapped = nullptr;
			if (FAILED(emitterReadback->Map(0, nullptr, &mapped))) return false;
			emitterResults = static_cast<float const *>(mapped);
			return true;
#endif
		}

		// Camera

		void ReadCamera() {
			cGDriver12::Transform transform{};
			driver.SaveTransform(transform);
			float const *const p = transform.projection;
			float const *const v = transform.modelView;
			int32_t viewport[4];
			driver.GetViewport(viewport);
			if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] <= 0 || viewport[3] <= 0) return;
			if (p[3] != 0.0f || p[7] != 0.0f || p[11] != 0.0f || p[15] != 1.0f || p[0] == 0.0f || p[5] == 0.0f) return;
			float const up = std::sqrt(v[4] * v[4] + v[5] * v[5] + v[6] * v[6]);
			if (std::fabs(v[4]) > 1e-4f || std::fabs(up - 1.0f) > 1e-3f || v[15] != 1.0f) return;
			std::memcpy(projection, p, sizeof(projection));
			std::memcpy(view, v, sizeof(view));
			width = viewport[2];
			height = viewport[3];
			cameraRead = true;
		}

		bool Camera(cGDriver12::WorldFrame &frame) const {
			float pv[16], inverse[16];
			Multiply(projection, view, pv);
			if (!Invert(pv, inverse)) return false;
			// A pixel (x right, y down, depth 0 to 1) to clip space.
			float pixel[16]{};
			pixel[0] = 2.0f / static_cast<float>(width);
			pixel[5] = -2.0f / static_cast<float>(height);
			pixel[10] = 2.0f;
			pixel[12] = -1.0f;
			pixel[13] = 1.0f;
			pixel[14] = -1.0f;
			pixel[15] = 1.0f;
			float m[16];
			Multiply(inverse, pixel, m);
			for (int row = 0; row < 4; ++row) {
				for (int c = 0; c < 4; ++c) {
					frame.pixelToWorld[row * 4 + c] = m[c * 4 + row];
					frame.worldToView[row * 4 + c] = row < 3 && c < 3 ? view[c * 4 + row] : (row == c ? 1.0f : 0.0f);
				}
			}
			frame.metresPerPixel = 2.0f / (projection[0] * static_cast<float>(width));
			return true;
		}

		// Terrain

		// A height as a draw gives it, with the size of the triangles it came from in cells. Far out SC4 draws the
		// terrain coarser, and the vertices it keeps then lie on the coarse surface, not at their own heights, so
		// a vertex keeps the height from the finest triangles it has been drawn with.
		void DrawnHeight(uint32_t index, float height, uint8_t span) {
			// Draws of this frame are counted before BeginLighting counts the frame.
			if (drawnIn[index] != frameNumber + 1) {
				drawnIn[index] = frameNumber + 1;
				drawnHeights[index] = height;
				drawnSpans[index] = span;
				drawnThisFrame.push_back(index);
			} else if (span < drawnSpans[index]) {
				drawnHeights[index] = height;
				drawnSpans[index] = span;
			} else if (span == drawnSpans[index]) {
				// Within one frame the highest: what lies below the surface is the edge of the map.
				drawnHeights[index] = (std::max)(drawnHeights[index], height);
			}
		}

		void TerrainDraw(uint8_t const *vertices, uint32_t bytes, uint32_t stride, uint16_t const *indices,
		                 uint32_t count) {
			uint32_t const vertexCount = bytes / stride;
			for (uint32_t i = 0; i + 2 < count; i += 3) {
				int32_t gx[3], gz[3];
				float y[3];
				bool onGrid = true;
				for (uint32_t k = 0; k < 3 && onGrid; ++k) {
					uint32_t const index = indices[i + k];
					if (index >= vertexCount) {
						onGrid = false;
						break;
					}
					float const *const p = reinterpret_cast<float const *>(vertices + static_cast<size_t>(index) * stride);
					float const fx = p[0] / kCellMetres, fz = p[2] / kCellMetres;
					gx[k] = static_cast<int32_t>(std::lround(fx));
					gz[k] = static_cast<int32_t>(std::lround(fz));
					y[k] = p[1];
					onGrid = std::fabs(fx - static_cast<float>(gx[k])) < 1e-3f &&
					         std::fabs(fz - static_cast<float>(gz[k])) < 1e-3f && gx[k] >= 0 && gz[k] >= 0 &&
					         gx[k] < static_cast<int32_t>(kFieldVertices) && gz[k] < static_cast<int32_t>(kFieldVertices) &&
					         std::isfinite(p[1]);
				}
				if (!onGrid) continue;
				int32_t const x0 = (std::min)({gx[0], gx[1], gx[2]}), x1 = (std::max)({gx[0], gx[1], gx[2]});
				int32_t const z0 = (std::min)({gz[0], gz[1], gz[2]}), z1 = (std::max)({gz[0], gz[1], gz[2]});
				// Only a triangle that fills half of one cell is at full detail. Others, the thin ones that stitch
				// coarse to fine included, rank by their size.
				bool const fine = x1 - x0 == 1 && z1 - z0 == 1;
				uint8_t const span = fine ? 1 : static_cast<uint8_t>((std::min)((std::max)(x1 - x0, z1 - z0) + 1, 254));
				for (uint32_t k = 0; k < 3; ++k) {
					DrawnHeight(static_cast<uint32_t>(gz[k]) * kFieldVertices + static_cast<uint32_t>(gx[k]), y[k], span);
				}
				if (x1 - x0 != 1 || z1 - z0 != 1) continue;
				bool low = false, high = false;
				for (uint32_t k = 0; k < 3; ++k) {
					low = low || (gx[k] == x0 && gz[k] == z0);
					high = high || (gx[k] == x1 && gz[k] == z1);
				}
				uint8_t &diagonal = diagonals[static_cast<size_t>(z0) * (kFieldVertices - 1) + x0];
				// Some of the terrain's draws split cells the other way, so a cell keeps the first split it is seen
				// with. A later terraform that flips one is missed.
				uint8_t const split = low && high ? 1 : 2;
				if (diagonal == 0) {
					diagonal = split;
					terrainChanged = true;
				}
			}
		}

		// Takes this frame's heights, and every so often hands changes on to the field and the structure.
		void UpdateTerrain() {
			for (uint32_t index: drawnThisFrame) {
				if (drawnSpans[index] > spans[index] || heights[index] == drawnHeights[index]) continue;
				spans[index] = drawnSpans[index];
				heights[index] = drawnHeights[index];
				terrainChanged = true;
			}
			drawnThisFrame.clear();
			if (!terrainChanged || (terrainUpdated != 0 && frameNumber - terrainUpdated < kTerrainInterval)) return;
			terrainChanged = false;
			terrainUpdated = frameNumber;
			terrainStructureDirty = true;
			if (options.terrain) UploadField();
		}

		float Height(int32_t x, int32_t z) const {
			if (x < 0 || z < 0 || x >= static_cast<int32_t>(kFieldVertices) || z >= static_cast<int32_t>(kFieldVertices)) {
				return kUnknownHeight;
			}
			return heights[static_cast<size_t>(z) * kFieldVertices + x];
		}

		// The field the lighting pass reads: per vertex the smooth normal from the neighbours' heights, and the
		// height. Unknown vertices lie far below, so no pixel takes their normal.
		void UploadField() {
			std::vector<float> values(static_cast<size_t>(kFieldVertices) * kFieldVertices * 4);
			auto const known = [](float height) { return height > kUnknownHeight * 0.5f; };
			for (int32_t z = 0; z < static_cast<int32_t>(kFieldVertices); ++z) {
				for (int32_t x = 0; x < static_cast<int32_t>(kFieldVertices); ++x) {
					float *const out = values.data() + (static_cast<size_t>(z) * kFieldVertices + x) * 4;
					float const centre = Height(x, z);
					out[3] = centre;
					out[0] = out[2] = 0.0f;
					out[1] = 1.0f;
					if (!known(centre)) continue;
					// Slopes over both neighbours when known, else over the one there is.
					auto const slope = [&](int32_t dx, int32_t dz) {
						float const before = Height(x - dx, z - dz), after = Height(x + dx, z + dz);
						if (known(before) && known(after)) return (after - before) / (2.0f * kCellMetres);
						if (known(after)) return (after - centre) / kCellMetres;
						if (known(before)) return (centre - before) / kCellMetres;
						return 0.0f;
					};
					float const sx = slope(1, 0), sz = slope(0, 1);
					float const length = std::sqrt(sx * sx + 1.0f + sz * sz);
					out[0] = -sx / length;
					out[1] = 1.0f / length;
					out[2] = -sz / length;
				}
			}
			uint64_t const bytes = values.size() * sizeof(float);
			bool const fresh = !field;
			if (fresh && !CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, bytes, D3D12_RESOURCE_STATE_COPY_DEST, field)) return;
			ID3D12Resource *source = nullptr;
			uint64_t offset = 0;
			if (!driver.Upload(values.data(), bytes, source, offset)) return;
			if (!fresh) Barrier(field.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			list->CopyBufferRegion(field.Get(), 0, source, offset, bytes);
			Barrier(field.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			if (fresh) driver.SetTerrainField(field.Get());
			fieldReady = true;
			++totals.fieldUploads;
		}

		// Structures

		D3D12_GPU_VIRTUAL_ADDRESS AllocateStructure(uint64_t bytes) {
			bytes = AlignUp(bytes, kStructureAlignment);
			if (pools.empty() || poolUsed + bytes > pools.back()->GetDesc().Width) {
				ComPtr<ID3D12Resource> pool;
				if (!CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, (std::max)(kPoolBytes, bytes),
				                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, pool, true)) return 0;
				totals.poolBytes += pool->GetDesc().Width;
				pools.push_back(std::move(pool));
				poolUsed = 0;
			}
			D3D12_GPU_VIRTUAL_ADDRESS const address = pools.back()->GetGPUVirtualAddress() + poolUsed;
			poolUsed += bytes;
			return address;
		}

		// Where a mesh's vertices and indices live, as raw views for shaders.
		bool Locate(MeshEntry &entry, D3D12_VERTEX_BUFFER_VIEW const &vertices, D3D12_INDEX_BUFFER_VIEW const &indices) {
			if (entry.located) return entry.buffer != 0;
			entry.located = true;
			if (lane == nullptr) return false;
			uint64_t vertexOffset = 0, indexOffset = 0;
			ID3D12Resource *const buffer = lane->GeometryBuffer(vertices.BufferLocation, vertexOffset);
			if (buffer == nullptr || lane->GeometryBuffer(indices.BufferLocation, indexOffset) != buffer ||
			    vertexOffset > UINT32_MAX || indexOffset > UINT32_MAX) return false;
			auto found = bufferViews.find(buffer);
			if (found == bufferViews.end()) found = bufferViews.emplace(buffer, driver.CreateBufferView(buffer)).first;
			entry.buffer = found->second;
			entry.vertexOffset = static_cast<uint32_t>(vertexOffset);
			entry.indexOffset = static_cast<uint32_t>(indexOffset);
			return entry.buffer != 0;
		}

		// A bottom level structure for the mesh, queued for this frame's builds, when the budget allows.
		bool QueueStructure(MeshEntry &entry, D3D12_VERTEX_BUFFER_VIEW const &vertices,
		                    D3D12_INDEX_BUFFER_VIEW const &indices, uint32_t count) {
			if (builds.size() >= kBuildsPerFrame) {
				revisit = true;
				return false;
			}
			Build build{};
			D3D12_RAYTRACING_GEOMETRY_TRIANGLES_DESC &triangles = build.geometry.Triangles;
			build.geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			triangles.VertexBuffer = {vertices.BufferLocation, vertices.StrideInBytes};
			triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			triangles.VertexCount = vertices.SizeInBytes / vertices.StrideInBytes;
			triangles.IndexBuffer = indices.BufferLocation;
			triangles.IndexFormat = indices.Format;
			triangles.IndexCount = count;
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			inputs.NumDescs = 1;
			inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			inputs.pGeometryDescs = &build.geometry;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &info);
			build.result = info.ResultDataMaxSizeInBytes != 0 ? AllocateStructure(info.ResultDataMaxSizeInBytes) : 0;
			if (build.result == 0) {
				entry.failed = true;
				return false;
			}
			build.scratch = AlignUp(info.ScratchDataSizeInBytes, kStructureAlignment);
			entry.structure = build.result;
			builds.push_back(build);
			return true;
		}

		// One mesh of a retained instance: an instance of its structure in the city's, and for night lights the
		// glow of its night texture.
		void AddMesh(float const *world, D3D12_VERTEX_BUFFER_VIEW const &vertices, D3D12_INDEX_BUFFER_VIEW const &indices,
		             uint32_t count, cGDriver12::DrawState const &state) {
			if (count < 3 || vertices.StrideInBytes == 0) return;
			for (int i = 0; i < 16; ++i) {
				if (!std::isfinite(world[i]) || std::fabs(world[i]) > 1.0e6f) return;
			}
			MeshEntry &entry = meshes[{vertices.BufferLocation, indices.BufferLocation, count}];
			cGDriver12::DrawState::Stage const &night = state.stages[1];
			if (options.lights && darkness > 0.0f && night.enabled && night.mode == 2 && night.coordinates == 0) {
				Glow(world, entry, vertices, indices, count, night.texture);
			}
			// Invisible and additive meshes cast no shadow.
			if (!options.rays || !state.colorWrite || (state.blend && state.destinationBlend == 1)) return;
			if (entry.structure == 0 && (entry.failed || !QueueStructure(entry, vertices, indices, count))) return;
			D3D12_RAYTRACING_INSTANCE_DESC &instance = instances.emplace_back();
			for (int row = 0; row < 3; ++row) {
				for (int c = 0; c < 4; ++c) instance.Transform[row][c] = world[c * 4 + row];
			}
			instance.InstanceMask = kModelsMask;
			instance.AccelerationStructure = entry.structure;
			uint32_t const texture = state.stages[0].enabled ? driver.TextureView(state.stages[0].texture) : 0;
			bool const tested = (state.alphaTest || state.blend) && texture != 0 && Locate(entry, vertices, indices);
			uint32_t flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
			if (tested) {
				instance.InstanceID = static_cast<uint32_t>(hitTable.size());
				float const reference = state.alphaTest ? state.alphaReference : 0.5f;
				hitTable.push_back({texture, entry.buffer, entry.vertexOffset, entry.indexOffset, reference, {}});
				hitTextures.push_back(state.stages[0].texture);
				flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
			} else {
				flags |= D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
			}
			instance.Flags = flags;
		}

		// The terrain's structure from the known cells, split the way SC4 splits them, made ready to build into the
		// spare of the two terrain buffers: its inputs are copied, the build itself is left to BuildCity.
		bool PrepareTerrain(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC &build, uint64_t &scratchBytes) {
			std::vector<float> positions(static_cast<size_t>(kFieldVertices) * kFieldVertices * 3);
			for (uint32_t z = 0; z < kFieldVertices; ++z) {
				for (uint32_t x = 0; x < kFieldVertices; ++x) {
					float *const p = positions.data() + (static_cast<size_t>(z) * kFieldVertices + x) * 3;
					p[0] = static_cast<float>(x) * kCellMetres;
					p[1] = (std::max)(heights[static_cast<size_t>(z) * kFieldVertices + x], -1000.0f);
					p[2] = static_cast<float>(z) * kCellMetres;
				}
			}
			std::vector<uint32_t> indices;
			for (uint32_t z = 0; z + 1 < kFieldVertices; ++z) {
				for (uint32_t x = 0; x + 1 < kFieldVertices; ++x) {
					uint32_t const a = z * kFieldVertices + x, b = a + 1, c = a + kFieldVertices, d = c + 1;
					if (heights[a] <= kUnknownHeight || heights[b] <= kUnknownHeight || heights[c] <= kUnknownHeight ||
					    heights[d] <= kUnknownHeight) continue;
					if (diagonals[static_cast<size_t>(z) * (kFieldVertices - 1) + x] == 2) {
						indices.insert(indices.end(), {a, b, c, b, d, c});
					} else {
						indices.insert(indices.end(), {a, b, d, a, d, c});
					}
				}
			}
			if (indices.empty()) return false;
			// The inputs go to a buffer of their own first.
			uint64_t const positionBytes = positions.size() * sizeof(float);
			uint64_t const indexBytes = indices.size() * sizeof(uint32_t);
			uint64_t const mostIndexBytes = uint64_t{kFieldVertices - 1} * (kFieldVertices - 1) * 6 * sizeof(uint32_t);
			bool const fresh = !terrainInputs;
			if (fresh && !CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, positionBytes + mostIndexBytes, D3D12_RESOURCE_STATE_COPY_DEST,
			                           terrainInputs)) return false;
			if (!fresh) Barrier(terrainInputs.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
			ID3D12Resource *source = nullptr;
			uint64_t offset = 0;
			bool const copied = driver.Upload(positions.data(), positionBytes, source, offset);
			if (copied) list->CopyBufferRegion(terrainInputs.Get(), 0, source, offset, positionBytes);
			bool const copiedIndices = copied && driver.Upload(indices.data(), indexBytes, source, offset);
			if (copiedIndices) list->CopyBufferRegion(terrainInputs.Get(), positionBytes, source, offset, indexBytes);
			Barrier(terrainInputs.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
			if (!copiedIndices) return false;

			terrainGeometry = {};
			terrainGeometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
			terrainGeometry.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
			terrainGeometry.Triangles.VertexBuffer = {terrainInputs->GetGPUVirtualAddress(), 12};
			terrainGeometry.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
			terrainGeometry.Triangles.VertexCount = kFieldVertices * kFieldVertices;
			terrainGeometry.Triangles.IndexBuffer = terrainInputs->GetGPUVirtualAddress() + positionBytes;
			terrainGeometry.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
			terrainGeometry.Triangles.IndexCount = static_cast<UINT>(indices.size());
			build = {};
			build.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
			build.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
			build.Inputs.NumDescs = 1;
			build.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			build.Inputs.pGeometryDescs = &terrainGeometry;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
			device->GetRaytracingAccelerationStructurePrebuildInfo(&build.Inputs, &info);
			// Both buffers hold the whole grid.
			ComPtr<ID3D12Resource> &spare = terrainStructures[terrainCurrent ^ 1];
			if (!spare) {
				D3D12_RAYTRACING_GEOMETRY_DESC whole = terrainGeometry;
				whole.Triangles.IndexCount = static_cast<UINT>(mostIndexBytes / sizeof(uint32_t));
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs = build.Inputs;
				inputs.pGeometryDescs = &whole;
				D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO most{};
				device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &most);
				if (!CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, AlignUp(most.ResultDataMaxSizeInBytes, kStructureAlignment),
				                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, spare, true)) return false;
			}
			if (spare->GetDesc().Width < info.ResultDataMaxSizeInBytes) return false;
			build.DestAccelerationStructureData = spare->GetGPUVirtualAddress();
			scratchBytes = AlignUp(info.ScratchDataSizeInBytes, kStructureAlignment);
			return true;
		}

		// Scratch memory for this frame's builds, from a ring the GPU is done with.
		D3D12_GPU_VIRTUAL_ADDRESS Scratch(uint64_t bytes) {
			uint32_t const slot = static_cast<uint32_t>(frameNumber % kScratchSlots);
			WaitFor(scratchFrame[slot]);
			scratchFrame[slot] = frameNumber;
			if (!scratch[slot] || scratch[slot]->GetDesc().Width < bytes) {
				scratch[slot].Reset();
				if (!CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, AlignUp((std::max)(bytes, uint64_t{1} << 20), 1 << 20),
				                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, scratch[slot], true)) return 0;
			}
			return scratch[slot]->GetGPUVirtualAddress();
		}

		// Builds what is new, and when the city changed, the spare structure of the city (every retained mesh and the
		// terrain) for the frames to come. This frame is traced with the current one.
		void BuildCity() {
			hitMeshes = 0;
			if (terrainSwap) terrainCurrent ^= 1;
			if (citySwap) cityCurrent ^= 1;
			terrainSwap = citySwap = false;
			uint64_t needed = 0;
			for (Build const &build: builds) needed += build.scratch;
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC terrainBuild{};
			uint64_t terrainScratch = 0;
			bool const terrainNow = terrainStructureDirty && PrepareTerrain(terrainBuild, terrainScratch);
			terrainStructureDirty = false;
			needed += terrainScratch;
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
			inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
			inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
			inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
			// With the terrain first, when there is one.
			inputs.NumDescs = static_cast<uint32_t>(instances.size()) + 1;
			D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO topInfo{};
			device->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &topInfo);
			needed += AlignUp(topInfo.ScratchDataSizeInBytes, kStructureAlignment);
			D3D12_GPU_VIRTUAL_ADDRESS const scratchAddress = Scratch(needed);
			if (scratchAddress == 0) {
				terrainStructureDirty = terrainNow;
				return;
			}
			uint64_t used = 0;
			for (Build &build: builds) {
				D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC description{};
				description.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
				description.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
				description.Inputs.NumDescs = 1;
				description.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
				description.Inputs.pGeometryDescs = &build.geometry;
				description.DestAccelerationStructureData = build.result;
				description.ScratchAccelerationStructureData = scratchAddress + used;
				used += build.scratch;
				list->BuildRaytracingAccelerationStructure(&description, 0, nullptr);
			}
			totals.structuresBuilt += builds.size();
			if (terrainNow) {
				terrainBuild.ScratchAccelerationStructureData = scratchAddress + used;
				used += terrainScratch;
				list->BuildRaytracingAccelerationStructure(&terrainBuild, 0, nullptr);
				++totals.terrainBuilds;
				terrainSwap = true;
			}
			if (!builds.empty() || terrainNow) UnorderedBarrier();
			builds.clear();

			// The ground is the newest terrain, the current one by the time this structure is traced.
			ID3D12Resource *const terrain = terrainStructures[terrainCurrent ^ (terrainSwap ? 1 : 0)].Get();
			D3D12_GPU_VIRTUAL_ADDRESS const ground = terrain != nullptr ? terrain->GetGPUVirtualAddress() : 0;
			if (ground == 0 && instances.empty()) return;
			totals.instances += instances.size();
			totals.alphaInstances += hitTable.size();
			City &current = cities[cityCurrent];
			if (current.ready && ground == current.ground && instances.size() == current.instances.size() &&
			    std::memcmp(instances.data(), current.instances.data(),
			                instances.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)) == 0) {
				// The same instances in the same order, but this frame's hit table: a texture SC4 deleted gives its
				// view slot to another.
				UploadHits(hitTable);
				return;
			}
			inputs.NumDescs = static_cast<uint32_t>(instances.size()) + (ground != 0 ? 1 : 0);
			citySwap = BuildSpareCity(inputs, topInfo, scratchAddress + used, ground);
			if (!current.ready) return;
			for (size_t i = 0; i < current.textures.size(); ++i) {
				current.hits[i].texture = driver.TextureView(current.textures[i]);
			}
			UploadHits(current.hits);
		}

		bool BuildSpareCity(D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs,
		                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO const &info,
		                    D3D12_GPU_VIRTUAL_ADDRESS scratchAddress, D3D12_GPU_VIRTUAL_ADDRESS ground) {
			City &spare = cities[cityCurrent ^ 1];
			spare.ready = false;
			// The terrain first, when there is one.
			std::vector<D3D12_RAYTRACING_INSTANCE_DESC> descriptions;
			descriptions.reserve(instances.size() + 1);
			if (ground != 0) {
				D3D12_RAYTRACING_INSTANCE_DESC &terrain = descriptions.emplace_back();
				terrain.Transform[0][0] = terrain.Transform[1][1] = terrain.Transform[2][2] = 1.0f;
				terrain.InstanceMask = kTerrainMask;
				terrain.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
				terrain.AccelerationStructure = ground;
			}
			descriptions.insert(descriptions.end(), instances.begin(), instances.end());
			inputs.InstanceDescs = Upload(descriptions.data(), descriptions.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
			if (inputs.InstanceDescs == 0) return false;
			uint64_t const bytes = AlignUp(info.ResultDataMaxSizeInBytes, kStructureAlignment);
			if (!spare.structure || spare.structure->GetDesc().Width < bytes) {
				Retire(spare.structure);
				if (!CreateBuffer(D3D12_HEAP_TYPE_DEFAULT, bytes + bytes / 4,
				                  D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, spare.structure, true)) return false;
			}
			D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC description{};
			description.Inputs = inputs;
			description.DestAccelerationStructureData = spare.structure->GetGPUVirtualAddress();
			description.ScratchAccelerationStructureData = scratchAddress;
			list->BuildRaytracingAccelerationStructure(&description, 0, nullptr);
			spare.ground = ground;
			spare.instances = instances;
			spare.hits = hitTable;
			spare.textures = hitTextures;
			spare.ready = true;
			++totals.topBuilds;
			return true;
		}

		void UploadHits(std::vector<HitMesh> const &hits) {
			HitMesh const none{};
			hitMeshes = hits.empty() ? Upload(&none, sizeof(none)) : Upload(hits.data(), hits.size() * sizeof(HitMesh));
		}

		// Night lights

		// Queues the glow of a lit mesh to be read, or makes its lights once it has been.
		void Glow(float const *world, MeshEntry &entry, D3D12_VERTEX_BUFFER_VIEW const &vertices,
		          D3D12_INDEX_BUFFER_VIEW const &indices, uint32_t count, uint32_t nightTexture) {
			uint32_t const texture = driver.TextureView(nightTexture);
			if (texture == 0) return;
			auto [found, fresh] = emitters.try_emplace(EmitterKey{indices.BufferLocation, count, texture});
			Emitter &emitter = found->second;
			if (fresh) {
				if (nextEmitter >= kEmitterCapacity || jobs.size() >= kEmittersPerFrame ||
				    !Locate(entry, vertices, indices)) {
					revisit = revisit || jobs.size() >= kEmittersPerFrame;
					emitters.erase(found);
					return;
				}
				emitter.slot = nextEmitter++;
				emitter.frame = frameNumber;
				EmitterJob &job = jobs.emplace_back();
				uint32_t const values[8]{texture, entry.buffer, entry.vertexOffset, entry.indexOffset, count,
				                         emitter.slot * (kEmitterFloats / 4), 0, 0};
				std::memcpy(job.constants, values, sizeof(values));
				pendingEmitters.push_back(&emitter);
				return;
			}
			if (!emitter.read) return;
			++litMeshes;
			for (uint32_t side = 0; side < 6; ++side) {
				float const *const place = emitter.sides + side * 8;
				float const *const glow = place + 4;
				float const weight = place[3];
				if (weight < kLeastGlow || lights.size() >= kMostLights) continue;
				float at[3]{place[0] / weight, place[1] / weight, place[2] / weight};
				at[side / 2] += (side & 1) ? -kLightOffset : kLightOffset;
				Light &light = lights.emplace_back();
				for (int row = 0; row < 3; ++row) {
					light.place[row] = world[row] * at[0] + world[4 + row] * at[1] + world[8 + row] * at[2] + world[12 + row];
				}
				light.place[3] = std::clamp(kRadiusPerGlow * std::sqrt(std::sqrt(weight)), kSmallestRadius, kLargestRadius);
				// The glow's colour at the same brightness for every light, dimmer for the faintest.
				float const strength = kLightStrength * darkness * (std::min)(1.0f, std::sqrt(weight));
				for (int i = 0; i < 3; ++i) light.color[i] = glow[i] / weight * strength;
				light.color[3] = 0.0f;
			}
		}

		// Reads the glow of meshes whose commands the GPU has finished, and queues the new ones.
		void Emitters() {
			uint64_t const completed = fence->GetCompletedValue();
			std::erase_if(pendingEmitters, [&](Emitter *emitter) {
				if (emitter->frame > completed || emitter->frame == frameNumber) return false;
				std::memcpy(emitter->sides, emitterResults + static_cast<size_t>(emitter->slot) * kEmitterFloats,
				            sizeof(emitter->sides));
				emitter->read = true;
				++totals.emittersRead;
				revisit = true;
				return true;
			});
			if (jobs.empty()) return;
			list->SetComputeRootSignature(rootSignature.Get());
			list->SetComputeRootDescriptorTable(4, driver.ViewTable());
			list->SetComputeRootDescriptorTable(5, driver.ViewTable());
			list->SetComputeRootUnorderedAccessView(7, emitterOutput->GetGPUVirtualAddress());
			list->SetPipelineState(emissivePipeline.Get());
			for (EmitterJob const &job: jobs) {
				list->SetComputeRoot32BitConstants(6, 8, job.constants, 0);
				list->Dispatch(1, 1, 1);
			}
			Barrier(emitterOutput.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
			uint64_t const bytes = kEmitterFloats * sizeof(float);
			for (EmitterJob const &job: jobs) {
				uint64_t const offset = uint64_t{job.constants[5]} * 16;
				list->CopyBufferRegion(emitterReadback.Get(), offset, emitterOutput.Get(), offset, bytes);
			}
			Barrier(emitterOutput.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
			jobs.clear();
		}

		// Each light's index in every screen tile it reaches: per tile its offset and count, then the indices.
		void BinLights(cGDriver12::WorldFrame &frame) {
			uint32_t const across = (static_cast<uint32_t>(width) + kTileSize - 1) / kTileSize;
			uint32_t const down = (static_cast<uint32_t>(height) + kTileSize - 1) / kTileSize;
			uint32_t const tileCount = across * down;
			float pv[16];
			Multiply(projection, view, pv);
			struct Span { uint32_t x0, y0, x1, y1; };
			std::vector<Span> spans(lights.size());
			std::vector<uint32_t> counts(tileCount);
			for (size_t i = 0; i < lights.size(); ++i) {
				float const *const p = lights[i].place;
				float const cx = pv[0] * p[0] + pv[4] * p[1] + pv[8] * p[2] + pv[12];
				float const cy = pv[1] * p[0] + pv[5] * p[1] + pv[9] * p[2] + pv[13];
				float const px = (cx + 1.0f) * 0.5f * static_cast<float>(width);
				float const py = (1.0f - cy) * 0.5f * static_cast<float>(height);
				float const reach = p[3] / frame.metresPerPixel + 1.0f;
				Span &span = spans[i];
				span = {1, 1, 0, 0};
				if (px + reach < 0.0f || py + reach < 0.0f || px - reach >= static_cast<float>(width) ||
				    py - reach >= static_cast<float>(height)) continue;
				span.x0 = static_cast<uint32_t>((std::max)(0.0f, px - reach)) / kTileSize;
				span.y0 = static_cast<uint32_t>((std::max)(0.0f, py - reach)) / kTileSize;
				span.x1 = (std::min)(static_cast<uint32_t>(px + reach) / kTileSize, across - 1);
				span.y1 = (std::min)(static_cast<uint32_t>(py + reach) / kTileSize, down - 1);
				for (uint32_t y = span.y0; y <= span.y1; ++y) {
					for (uint32_t x = span.x0; x <= span.x1; ++x) ++counts[y * across + x];
				}
			}
			tiles.assign(tileCount * 2, 0);
			uint32_t offset = tileCount * 2;
			for (uint32_t t = 0; t < tileCount; ++t) {
				tiles[t * 2] = offset;
				offset += (std::min)(counts[t], kLightsPerTile);
			}
			tiles.resize(offset);
			for (uint32_t i = 0; i < lights.size(); ++i) {
				Span const &span = spans[i];
				for (uint32_t y = span.y0; y <= span.y1 && span.x0 <= span.x1; ++y) {
					for (uint32_t x = span.x0; x <= span.x1; ++x) {
						uint32_t const t = y * across + x;
						if (tiles[t * 2 + 1] >= kLightsPerTile) continue;
						tiles[tiles[t * 2] + tiles[t * 2 + 1]++] = i;
					}
				}
			}
			frame.lights = Upload(lights.data(), lights.size() * sizeof(Light));
			frame.tiles = Upload(tiles.data(), tiles.size() * sizeof(uint32_t));
			if (frame.lights == 0 || frame.tiles == 0) return;
			frame.lightCount = static_cast<uint32_t>(lights.size());
			frame.tileSize = kTileSize;
			frame.tilesAcross = across;
			frame.tileCount = tileCount;
			totals.lights += lights.size();
		}

		struct Visitor final : RetainedScene::MeshVisitor {
			Impl &world;
			explicit Visitor(Impl &world) : world(world) {}
			void Mesh(float const *transform, D3D12_VERTEX_BUFFER_VIEW const &vertices,
			          D3D12_INDEX_BUFFER_VIEW const &indices, uint32_t count,
			          cGDriver12::DrawState const &state) override {
				world.AddMesh(transform, vertices, indices, count, state);
			}
		};

		bool BeginLighting(cGDriver12::WorldFrame &frame) {
			auto const start = std::chrono::steady_clock::now();
			++frameNumber;
			// Everything submitted so far belongs to earlier frames.
			driver.Queue()->Signal(fence.Get(), frameNumber - 1);
			uint64_t const completed = fence->GetCompletedValue();
			std::erase_if(garbage, [&](auto const &entry) { return entry.first <= completed; });
			bool const known = cameraRead;
			cameraRead = false;
			UpdateTerrain();
			if (!known || !Camera(frame)) return false;
			if (fieldReady) {
				frame.terrainSize = kFieldVertices;
				frame.terrainScale = 1.0f / kCellMetres;
			}
			if (raysReady && retained != nullptr) {
				uint64_t const changes = retained->Changes();
				if (revisit || changes != visitedChanges) {
					revisit = false;
					visitedChanges = changes;
					// Lamps light the night: fully under the night sky's ambient light, not at all under the day's.
					float const *const sky = driver.RelightSun().ambient;
					darkness = std::clamp((0.4f - (0.3f * sky[0] + 0.59f * sky[1] + 0.11f * sky[2])) / 0.25f, 0.0f, 1.0f);
					instances.clear();
					hitTable.clear();
					hitTextures.clear();
					lights.clear();
					litMeshes = 0;
					Visitor visitor(*this);
					retained->VisitMeshes(visitor);
				}
				totals.litMeshes += litMeshes;
				if (options.rays) BuildCity();
				if (options.lights) {
					Emitters();
					if (!lights.empty()) BinLights(frame);
				}
			}
			++totals.frames;
			totals.cpuMilliseconds.push_back(static_cast<float>(
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count()));
			return true;
		}

		bool TraceSunlight(D3D12_GPU_VIRTUAL_ADDRESS constants, D3D12_GPU_DESCRIPTOR_HANDLE views) {
			City const &city = cities[cityCurrent];
			if (!raysReady || !options.rays || !city.ready || hitMeshes == 0) return false;
			float const values[8]{kSunRadius, kLongestRay, kRayOffset, kRayOffsetPixels, kTerrainClearance, 0.0f, 0.0f, 0.0f};
			list->SetGraphicsRootSignature(rootSignature.Get());
			list->SetGraphicsRootConstantBufferView(0, constants);
			list->SetGraphicsRootDescriptorTable(1, views);
			list->SetGraphicsRootShaderResourceView(2, city.structure->GetGPUVirtualAddress());
			list->SetGraphicsRootShaderResourceView(3, hitMeshes);
			list->SetGraphicsRootDescriptorTable(4, driver.ViewTable());
			list->SetGraphicsRootDescriptorTable(5, driver.ViewTable());
			list->SetGraphicsRoot32BitConstants(6, 8, values, 0);
			list->SetGraphicsRootUnorderedAccessView(7, emitterOutput->GetGPUVirtualAddress());
			list->SetPipelineState(tracePipeline.Get());
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->DrawInstanced(3, 1, 0, 0);
			return true;
		}
	};

	WorldLight::WorldLight(cGDriver12 &driver, Options const &options) : impl(std::make_unique<Impl>(driver, options)) {
		driver.SetLightHooks(this);
	}

	WorldLight::~WorldLight() { impl->driver.SetLightHooks(nullptr); }

	void WorldLight::SetScene(RetainedScene const *retained, SceneLane const *lane) {
		impl->retained = retained;
		impl->lane = lane;
		impl->revisit = true;
	}

	void WorldLight::OwnerBegin(nSCDWire::DrawOwner const &owner) {
		Impl &d = *impl;
		d.owners.push_back(owner);
		if (owner.pass >= 3 && !d.cameraRead) d.ReadCamera();
	}

	void WorldLight::OwnerEnd() {
		if (!impl->owners.empty()) impl->owners.pop_back();
	}

	void WorldLight::VideoMode() {
		Impl &d = *impl;
		// Texture views are gone, and with them what the glow and the hit table point at.
		d.emitters.clear();
		d.pendingEmitters.clear();
		d.nextEmitter = 0;
		d.cities[0].ready = d.cities[1].ready = false;
		d.citySwap = false;
		d.revisit = true;
		d.cameraRead = false;
	}

	void WorldLight::IndexedVertices(uint8_t const *vertices, uint32_t bytes, uint32_t stride, uint16_t const *indices,
	                                 uint32_t count) {
		Impl &d = *impl;
		if (d.owners.empty() || d.owners.back().vtable != kTerrainView || d.owners.back().pass != 0) return;
		if (stride < 12 || !(d.options.terrain || d.options.rays)) return;
		d.TerrainDraw(vertices, bytes, stride, indices, count);
	}

	bool WorldLight::BeginLighting(cGDriver12::WorldFrame &frame) { return impl->BeginLighting(frame); }

	bool WorldLight::TraceSunlight(D3D12_GPU_VIRTUAL_ADDRESS constants, D3D12_GPU_DESCRIPTOR_HANDLE views) {
		return impl->TraceSunlight(constants, views);
	}

	void WorldLight::PrintTotals() const {
		Impl const &d = *impl;
		Totals const &t = d.totals;
		auto const u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
		double const frames = t.frames != 0 ? static_cast<double>(t.frames) : 1.0;
		uint64_t known = 0;
		for (float height: d.heights) known += height > kUnknownHeight;
		if (FAILED(d.driver.Device()->GetDeviceRemovedReason())) {
			std::printf("world light: the device was removed (%08lX)\n", d.driver.Device()->GetDeviceRemovedReason());
		}
		std::printf("world light: %llu frames lit from the world, rays %s, terrain: %llu of %u vertices known, %llu field "
		            "uploads, %llu structure builds\n",
		            u(t.frames), d.raysReady && d.options.rays ? "on" : "off", u(known), kFieldVertices * kFieldVertices,
		            u(t.fieldUploads), u(t.terrainBuilds));
		std::printf("world light: %llu mesh structures (%.1f MB of pools), %llu top level builds, %.0f instances and "
		            "%.0f alpha-tested per frame\n",
		            u(t.structuresBuilt), static_cast<double>(t.poolBytes) / (1 << 20), u(t.topBuilds),
		            static_cast<double>(t.instances) / frames, static_cast<double>(t.alphaInstances) / frames);
		std::printf("world light: %llu glowing meshes read, %.0f lit meshes and %.0f night lights per frame\n",
		            u(t.emittersRead), static_cast<double>(t.litMeshes) / frames, static_cast<double>(t.lights) / frames);
		if (!t.cpuMilliseconds.empty()) {
			std::vector<float> sorted = t.cpuMilliseconds;
			std::sort(sorted.begin(), sorted.end());
			double total = 0.0;
			for (float value: sorted) total += value;
			std::printf("world light: CPU per frame %.3f ms average, median %.3f, 95th percentile %.3f, slowest %.3f\n",
			            total / static_cast<double>(sorted.size()), sorted[sorted.size() / 2],
			            sorted[sorted.size() * 95 / 100], sorted.back());
		}
	}
}
