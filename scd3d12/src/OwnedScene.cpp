/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// What the static window looks like in the stream (Big City Tutorial at 1920x1080):
//
//   frame start   the full window's projection and the camera's view, then DrawBufferRegion of colour and
//                 depth (shifted by whole pixels while scrolling)
//   a redraw      a viewport (the whole window, the uncovered strip, a rectangle around a change), a quad that
//                 resets depth in it, on a full redraw also 128 metre background quads at height 0 without
//                 depth test, then pass 0: the terrain view object (vtable 0x00AB4480) draws the cells it sees
//                 in the viewport with DrawPrimsIndexed, one draw per layer (an untextured base, textures blended
//                 in, edges, water with coordinates from view positions), pass 1: the grid drawables, pass 2:
//                 overlays (0x00AB39D0, shadows and decals with both stages' coordinates from view positions)
//                 and the terrain again. The projection's depth offset moves a little per layer
//   save          ReadBufferRegion of colour and depth
//   dynamic       the first owner of pass 3 to 5
//
// The terrain's draws cover exactly the cells the viewport sees, so the same cell comes in other draws each
// time: cells, not draws, are what stays the same. While scrolling the camera only moves along the screen.

#include "OwnedScene.h"

#include "D3D11Conversions.h"
#include "Dlss.h"
#include "wire/ImageFile.h"

#include <d3dcompiler.h>
#include <xxhash.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <unordered_map>
#include <tuple>
#include <vector>

using Microsoft::WRL::ComPtr;
using nSCD3D11::D3D11Vertex;

namespace nSCD3D12 {
	namespace {
		constexpr uint32_t kTerrainVtable = 0x00AB4480;
		constexpr float kCellMetres = 16.0f;
		constexpr float kTileMetres = 256.0f;
		constexpr uint64_t kChunkBytes = 32ull << 20;
		constexpr uint32_t kConstantSlotBytes = 1280;
		constexpr uint32_t kSlotsPerChunk = 1024;
		// Frames the GPU may still be reading, so freed memory waits this long.
		constexpr uint64_t kFramesInFlight = 4;
		constexpr uint32_t kTimingSlots = 4;
		constexpr D3D12_RESOURCE_STATES kReadable =
			D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_INDEX_BUFFER;
		// The S3D models the scene lane draws.
		constexpr uint32_t kS3dFormat = 0x80004001;

		uint64_t Align(uint64_t value, uint64_t alignment) { return (value + alignment - 1) & ~(alignment - 1); }

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

		// The inverse of a general 4x4 matrix, in double precision. False when it has none.
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

		// world = rotation (row major) x (scale x vertex) + translation, each part only when the instance has it.
		void WorldMatrix(nSCDWire::ModelInstance const &instance, float *m) {
			float const scale = (instance.parts & 2) ? instance.scale : 1.0f;
			std::memset(m, 0, 16 * sizeof(float));
			for (int row = 0; row < 3; ++row) {
				for (int c = 0; c < 3; ++c) {
					float const r = (instance.parts & 4) ? instance.rotation[row * 3 + c] : (row == c ? 1.0f : 0.0f);
					m[c * 4 + row] = r * scale;
				}
				m[12 + row] = (instance.parts & 1) ? instance.translation[row] : 0.0f;
			}
			m[15] = 1.0f;
		}

		// A modelview's first three rows, what VSInstanced reads per instance.
		struct InstanceRows {
			float rows[12];
		};

		InstanceRows Rows(float const *m) {
			InstanceRows out;
			for (int row = 0; row < 3; ++row) {
				for (int c = 0; c < 4; ++c) out.rows[row * 4 + c] = m[c * 4 + row];
			}
			return out;
		}

		struct Camera {
			bool known = false;
			float projection[16]{};
			float view[16]{};
			int32_t width = 0, height = 0;
		};

		bool SameView(Camera const &a, Camera const &b) {
			if (!a.known || !b.known || a.width != b.width || a.height != b.height) return false;
			for (int i: {0, 5, 10}) {
				if (a.projection[i] != b.projection[i]) return false;
			}
			for (int c = 0; c < 3; ++c) {
				for (int row = 0; row < 3; ++row) {
					if (std::fabs(a.view[c * 4 + row] - b.view[c * 4 + row]) > 1e-5f) return false;
				}
			}
			return true;
		}

		// A camera's view: upright (world up never leans sideways on screen), tilted, a rotation.
		bool IsCameraView(float const *v) {
			float const up = std::sqrt(v[4] * v[4] + v[5] * v[5] + v[6] * v[6]);
			return std::fabs(v[4]) <= 1e-4f && std::fabs(up - 1.0f) <= 1e-3f && std::fabs(v[6]) > 0.1f && v[15] == 1.0f &&
			       v[3] == 0.0f && v[7] == 0.0f && v[11] == 0.0f;
		}

		// Where world geometry lies on screen while the view keeps its rotation: x and y of the rotation alone,
		// as min x, min y, max x, max y. The camera's translation and projection are added per frame.
		struct Extent {
			float v[4]{1e30f, 1e30f, -1e30f, -1e30f};

			void Add(float const *view, float const *p) {
				float const x = view[0] * p[0] + view[4] * p[1] + view[8] * p[2];
				float const y = view[1] * p[0] + view[5] * p[1] + view[9] * p[2];
				v[0] = (std::min)(v[0], x);
				v[1] = (std::min)(v[1], y);
				v[2] = (std::max)(v[2], x);
				v[3] = (std::max)(v[3], y);
			}
			void Add(Extent const &other) {
				v[0] = (std::min)(v[0], other.v[0]);
				v[1] = (std::min)(v[1], other.v[1]);
				v[2] = (std::max)(v[2], other.v[2]);
				v[3] = (std::max)(v[3], other.v[3]);
			}
			bool Empty() const { return v[0] > v[2]; }
		};

		// A screen rectangle in pixels from the top left: left, top, right, bottom.
		struct Rect {
			float left, top, right, bottom;
		};

		Rect ToScreen(Extent const &e, Camera const &camera) {
			float const *p = camera.projection, *v = camera.view;
			float const w = static_cast<float>(camera.width) * 0.5f, h = static_cast<float>(camera.height) * 0.5f;
			Rect r;
			r.left = ((e.v[0] + v[12]) * p[0] + p[12] + 1.0f) * w;
			r.right = ((e.v[2] + v[12]) * p[0] + p[12] + 1.0f) * w;
			r.top = (1.0f - ((e.v[3] + v[13]) * p[5] + p[13])) * h;
			r.bottom = (1.0f - ((e.v[1] + v[13]) * p[5] + p[13])) * h;
			return r;
		}

		bool Inside(Rect const &a, Rect const &outer) {
			return a.left >= outer.left && a.right <= outer.right && a.top >= outer.top && a.bottom <= outer.bottom;
		}

		// Overlapping by more than a pixel, so what only touches the edge of a redraw is left alone.
		bool Overlaps(Rect const &a, Rect const &b) {
			return a.left < b.right - 1.0f && a.right > b.left + 1.0f && a.top < b.bottom - 1.0f && a.bottom > b.top + 1.0f;
		}

		bool OnScreen(Rect const &a, Camera const &camera) {
			return a.right >= 0.0f && a.left <= static_cast<float>(camera.width) && a.bottom >= 0.0f &&
			       a.top <= static_cast<float>(camera.height);
		}

		bool ViewCoordinates(cGDriver12::CapturedState const &state, uint32_t stage) {
			uint32_t source;
			std::memcpy(&source, state.constants + cGDriver12::kStageOffset + stage * cGDriver12::kStageBytes +
			                     cGDriver12::kCoordinateSourceOffset, sizeof(source));
			return (source & ~7u) == 0x10;
		}

		float *ConstantMatrix(cGDriver12::CapturedState &state, uint32_t offset) {
			return reinterpret_cast<float *>(state.constants + offset);
		}

		uint64_t HashBytes(void const *data, size_t size, uint64_t seed = 0) {
			return XXH3_64bits_withSeed(data, size, seed);
		}

		// GPU memory that lasts: 32 MB buffers handed out first fit, freed a few frames late.
		class Pool {
		public:
			explicit Pool(cGDriver12 &driver) : driver(driver) {}

			bool Allocate(uint64_t bytes, uint32_t &chunk, uint64_t &offset) {
				bytes = Align(bytes, 256);
				for (uint32_t i = 0; i < chunks.size(); ++i) {
					auto &free = chunks[i].free;
					for (auto it = free.begin(); it != free.end(); ++it) {
						if (it->second < bytes) continue;
						chunk = i;
						offset = it->first;
						uint64_t const rest = it->second - bytes;
						free.erase(it);
						if (rest != 0) free.emplace(offset + bytes, rest);
						used += bytes;
						return true;
					}
				}
				Chunk fresh;
				fresh.size = (std::max)(kChunkBytes, bytes);
				D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
				D3D12_RESOURCE_DESC description{};
				description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				description.Width = fresh.size;
				description.Height = description.DepthOrArraySize = description.MipLevels = 1;
				description.SampleDesc.Count = 1;
				description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				if (FAILED(driver.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
				                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				                                                    IID_PPV_ARGS(&fresh.buffer)))) return false;
				fresh.state = D3D12_RESOURCE_STATE_COPY_DEST;
				if (fresh.size > bytes) fresh.free.emplace(bytes, fresh.size - bytes);
				chunks.push_back(std::move(fresh));
				chunk = static_cast<uint32_t>(chunks.size() - 1);
				offset = 0;
				used += bytes;
				return true;
			}

			void Free(uint32_t chunk, uint64_t offset, uint64_t bytes, uint64_t frame) {
				freed.push_back({chunk, offset, Align(bytes, 256), frame});
			}

			void Reclaim(uint64_t frame) {
				while (!freed.empty() && freed.front().frame + kFramesInFlight <= frame) {
					Freed const f = freed.front();
					freed.pop_front();
					used -= f.bytes;
					auto &free = chunks[f.chunk].free;
					uint64_t offset = f.offset, bytes = f.bytes;
					auto next = free.lower_bound(offset);
					if (next != free.end() && next->first == offset + bytes) {
						bytes += next->second;
						next = free.erase(next);
					}
					if (next != free.begin()) {
						auto previous = std::prev(next);
						if (previous->first + previous->second == offset) {
							previous->second += bytes;
							continue;
						}
					}
					free.emplace(offset, bytes);
				}
			}

			D3D12_GPU_VIRTUAL_ADDRESS Address(uint32_t chunk, uint64_t offset) const {
				return chunks[chunk].buffer->GetGPUVirtualAddress() + offset;
			}

			// Copies recorded for this frame go in one batch per buffer, between two barriers.
			void Copy(uint32_t chunk, uint64_t offset, ID3D12Resource *source, uint64_t sourceOffset, uint64_t bytes) {
				copies.push_back({chunk, offset, source, sourceOffset, bytes});
			}

			uint64_t Flush() {
				if (copies.empty()) return 0;
				ID3D12GraphicsCommandList *const list = driver.CommandList();
				std::sort(copies.begin(), copies.end(), [](CopyJob const &a, CopyJob const &b) { return a.chunk < b.chunk; });
				std::vector<D3D12_RESOURCE_BARRIER> barriers;
				auto const transitions = [&](D3D12_RESOURCE_STATES to) {
					barriers.clear();
					uint32_t last = UINT32_MAX;
					for (CopyJob const &job: copies) {
						if (job.chunk == last) continue;
						last = job.chunk;
						Chunk &c = chunks[job.chunk];
						if (c.state == to) continue;
						D3D12_RESOURCE_BARRIER &barrier = barriers.emplace_back();
						barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
						barrier.Transition = {c.buffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, c.state, to};
						c.state = to;
					}
					if (!barriers.empty()) list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
				};
				transitions(D3D12_RESOURCE_STATE_COPY_DEST);
				uint64_t bytes = 0;
				for (CopyJob const &job: copies) {
					list->CopyBufferRegion(chunks[job.chunk].buffer.Get(), job.offset, job.source, job.sourceOffset, job.bytes);
					bytes += job.bytes;
				}
				transitions(kReadable);
				copies.clear();
				return bytes;
			}

			// Chunks a draw may read must be readable even when nothing was copied into them yet.
			void Ready(uint32_t chunk) {
				Chunk &c = chunks[chunk];
				if (c.state == kReadable) return;
				D3D12_RESOURCE_BARRIER barrier{};
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition = {c.buffer.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, c.state, kReadable};
				driver.CommandList()->ResourceBarrier(1, &barrier);
				c.state = kReadable;
			}

			uint64_t Used() const { return used; }
			size_t ChunkCount() const { return chunks.size(); }

		private:
			struct Chunk {
				ComPtr<ID3D12Resource> buffer;
				uint64_t size = 0;
				std::map<uint64_t, uint64_t> free;
				D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
			};
			struct Freed {
				uint32_t chunk;
				uint64_t offset, bytes, frame;
			};
			struct CopyJob {
				uint32_t chunk;
				uint64_t offset;
				ID3D12Resource *source;
				uint64_t sourceOffset, bytes;
			};
			cGDriver12 &driver;
			std::vector<Chunk> chunks;
			std::deque<Freed> freed;
			std::vector<CopyJob> copies;
			uint64_t used = 0;
		};

		// Constants that last, in upload memory the GPU reads directly: one slot per state.
		class ConstantSlots {
		public:
			explicit ConstantSlots(cGDriver12 &driver) : driver(driver) {}

			uint32_t Allocate(void const *data) {
				if (free.empty()) {
					Chunk chunk;
					D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_UPLOAD};
					D3D12_RESOURCE_DESC description{};
					description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
					description.Width = static_cast<uint64_t>(kConstantSlotBytes) * kSlotsPerChunk;
					description.Height = description.DepthOrArraySize = description.MipLevels = 1;
					description.SampleDesc.Count = 1;
					description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
					D3D12_RANGE const none{0, 0};
					if (FAILED(driver.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
					                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
					                                                    IID_PPV_ARGS(&chunk.buffer))) ||
					    FAILED(chunk.buffer->Map(0, &none, reinterpret_cast<void **>(&chunk.cpu)))) return UINT32_MAX;
					uint32_t const base = static_cast<uint32_t>(chunks.size()) * kSlotsPerChunk;
					chunks.push_back(std::move(chunk));
					for (uint32_t i = kSlotsPerChunk; i-- > 0;) free.push_back(base + i);
				}
				uint32_t const slot = free.back();
				free.pop_back();
				std::memcpy(Cpu(slot), data, cGDriver12::kConstantBytes);
				return slot;
			}

			void Free(uint32_t slot, uint64_t frame) { freed.push_back({slot, frame}); }

			void Reclaim(uint64_t frame) {
				while (!freed.empty() && freed.front().second + kFramesInFlight <= frame) {
					free.push_back(freed.front().first);
					freed.pop_front();
				}
			}

			D3D12_GPU_VIRTUAL_ADDRESS Address(uint32_t slot) const {
				return chunks[slot / kSlotsPerChunk].buffer->GetGPUVirtualAddress() +
				       static_cast<uint64_t>(slot % kSlotsPerChunk) * kConstantSlotBytes;
			}

		private:
			struct Chunk {
				ComPtr<ID3D12Resource> buffer;
				uint8_t *cpu = nullptr;
			};
			uint8_t *Cpu(uint32_t slot) {
				return chunks[slot / kSlotsPerChunk].cpu + static_cast<size_t>(slot % kSlotsPerChunk) * kConstantSlotBytes;
			}
			cGDriver12 &driver;
			std::vector<Chunk> chunks;
			std::vector<uint32_t> free;
			std::deque<std::pair<uint32_t, uint64_t> > freed;
		};

		// One state the scene draws with: a pipeline, constants and textures, and for models the mesh.
		struct Group {
			cGDriver12::CapturedState state{};
			bool alive = false;
			uint64_t hash = 0;
			uint32_t slot = UINT32_MAX;
			uint32_t users = 0;
			uint64_t rank = 0;
			uint8_t kind = 0;
			// The pass and kind of owner it was drawn by, part of what tells groups apart.
			uint32_t layer = 0;
			// Stages with coordinates from view positions: the view the matrices were captured with and the
			// matrices themselves, which the state holds anchored to the world instead.
			bool viewCoordinates = false;
			float captureView[16]{};
			float captureMatrices[2][16]{};
			bool model = false;
			D3D12_VERTEX_BUFFER_VIEW vertices{};
			D3D12_INDEX_BUFFER_VIEW indices{};
			uint32_t count = 0;
			std::vector<uint32_t> buckets;
			// This frame's instances.
			uint32_t instanceCount = 0, instanceFirst = 0;
		};

		// World space triangles of one state, part of a bucket.
		struct Piece {
			bool alive = false;
			uint32_t group = 0;
			uint32_t bucket = UINT32_MAX;
			uint32_t place = 0;
			std::vector<D3D11Vertex> vertices;
			std::vector<uint32_t> indices;
			Extent extent;
			// Where it sits in the bucket's buffer, once it is there.
			bool placed = false;
			uint32_t firstVertex = 0, firstIndex = 0;
			// View objects: what it draws, and the redraw that last drew it.
			uint64_t hash = 0;
			uint64_t serial = 0, frame = 0;
		};

		// The pieces of one state in one tile, in one buffer that grows in place: vertices, then indices. A
		// piece that goes leaves degenerate triangles until the bucket is built again.
		struct Bucket {
			bool alive = false;
			uint32_t group = 0;
			uint64_t key = 0;
			std::vector<uint32_t> pieces;
			std::vector<uint32_t> pending;
			std::vector<std::pair<uint32_t, uint32_t> > holes;
			bool dirty = false, rebuild = false;
			uint32_t chunk = UINT32_MAX;
			uint64_t offset = 0, bytes = 0;
			uint32_t vertexCapacity = 0, indexCapacity = 0, vertexUsed = 0, indexUsed = 0, deadIndices = 0;
			Extent extent;
		};

		// A terrain cell: every layer the terrain drew in it, from the last redraw that drew it.
		struct Cell {
			std::vector<uint32_t> pieces;
			uint64_t serial = 0, frame = 0;
			Extent extent;
		};

		// A drawable of the static grid that is not a model the lane stands in for.
		struct GridItem {
			std::vector<uint32_t> pieces;
			uint64_t occupant = 0;
			uint64_t serial = 0, frame = 0;
			bool orphan = false;
			Extent extent;
		};

		struct ModelKey {
			uint64_t occupant;
			uint32_t group, instance;
			bool operator==(ModelKey const &) const = default;
		};

		struct ModelKeyHash {
			size_t operator()(ModelKey const &key) const noexcept {
				uint64_t h = key.occupant * 0x9E3779B97F4A7C15ull;
				h ^= (static_cast<uint64_t>(key.group) << 32 | key.instance) + 0x632BE59BD9B4E019ull + (h << 6) + (h >> 2);
				return static_cast<size_t>(h);
			}
		};

		struct Model {
			ModelKey key{};
			Extent extent;
			uint64_t frame = 0;
			uint32_t camera = 0;
			bool orphan = false;
			float world[16]{};
			// SC4's own modelview, used as it is while the camera has not changed since.
			float modelView[16]{};
			std::vector<uint32_t> meshes;
		};

		// Triangles of one state the current bracket drew, not yet given to the scene.
		struct Fresh {
			uint32_t group;
			std::vector<D3D11Vertex> vertices;
			std::vector<uint32_t> indices;
		};

		struct Redraw {
			// As SimGL gives viewports: x0, y0, x1, y1 from the bottom left.
			int32_t viewport[4];
			uint64_t serial;
		};

		struct Totals {
			uint64_t composites = 0, skipped = 0, viewChanges = 0;
			uint64_t captured[5]{};
			uint64_t screenDraws = 0, uncaptured = 0;
			uint64_t draws = 0, instances = 0, culled = 0;
			uint64_t removed[4]{};
			uint64_t appendedBytes = 0, rebuilds = 0, cellsKept = 0, cellsChanged = 0;
			std::vector<float> cpuMilliseconds, gpuMilliseconds;
			// CPU time of the composite's steps: what redraws removed, the buckets, the instances, the draws.
			double steps[4]{};
		};

		struct Zoom {
			uint64_t frames = 0, draws = 0, instances = 0;
			std::vector<float> cpuMilliseconds, gpuMilliseconds;
		};

		// The camera's motion of every pixel, from its depth and the two frames' cameras.
		char const kMotionShader[] = R"(
cbuffer Motion : register(b0)
{
	float4x4 current;  // this frame's clip space to world
	float4x4 previous; // the previous frame's world to clip space
	float4 size;       // width, height, 1 / width, 1 / height
};
Texture2D<float> depth : register(t0);

float4 FullScreenVS(uint id : SV_VertexID) : SV_POSITION
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float2 MotionPS(float4 position : SV_POSITION) : SV_TARGET
{
	float z = depth.Load(int3(position.xy, 0));
	float2 ndc = float2(position.x * size.z * 2.0f - 1.0f, 1.0f - position.y * size.w * 2.0f);
	float4 world = mul(current, float4(ndc, z * 2.0f - 1.0f, 1.0f));
	float4 clip = mul(previous, world / world.w);
	float2 before = float2(clip.x / clip.w + 1.0f, 1.0f - clip.y / clip.w) * 0.5f * size.xy;
	return before - position.xy;
}
)";

		enum CaptureKind : uint8_t { kBackground, kTerrain, kView, kGrid, kModel };
		char const *const kCaptureNames[5] = {"background", "terrain", "view object", "grid", "model mesh"};
	}

	struct OwnedScene::Impl {
		cGDriver12 &driver;
		SceneLane &lane;
		Pool pool;
		ConstantSlots slots;

		std::vector<Group> groups;
		std::vector<uint32_t> freeGroups;
		std::unordered_multimap<uint64_t, uint32_t> groupsByHash;
		std::vector<uint32_t> order;
		bool orderDirty = true;
		uint64_t gridRank = 0;

		std::vector<Piece> pieces;
		std::vector<uint32_t> freePieces;
		std::vector<Bucket> buckets;
		std::vector<uint32_t> freeBuckets;
		std::unordered_map<uint64_t, uint32_t> bucketsByKey;
		std::vector<uint32_t> dirtyBuckets;
		// Groups made or matched this frame that nothing may use.
		std::vector<uint32_t> unused;

		std::unordered_map<uint64_t, Cell> cells;
		std::unordered_map<uint64_t, uint32_t> viewPieces;
		std::unordered_map<uint64_t, GridItem> grid;
		std::unordered_map<uint64_t, std::vector<uint64_t> > gridByOccupant;
		std::unordered_map<ModelKey, Model, ModelKeyHash> models;
		std::unordered_map<uint64_t, std::vector<ModelKey> > modelsByOccupant;

		// Where everything kept lies on screen, so a redraw only looks at what is near it: each item in the bin of
		// its extent's middle, or in the large list when it is bigger than a bin. Entries are not removed when an
		// item goes or moves: a lookup that finds the item gone or elsewhere drops the entry then.
		struct Entry {
			uint8_t kind; // 0 model, 1 grid, 2 cell, 3 view piece
			uint64_t key;
			uint32_t group, instance; // the rest of a model's key
			bool operator==(Entry const &) const = default;
			bool operator<(Entry const &other) const {
				return std::tie(kind, key, group, instance) < std::tie(other.kind, other.key, other.group, other.instance);
			}
		};
		static constexpr float kBinMetres = 64.0f;
		std::unordered_map<uint64_t, std::vector<Entry> > bins;
		std::vector<Entry> large;
		std::vector<Entry> candidates;

		static uint64_t BinKey(int32_t x, int32_t y) {
			return static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 | static_cast<uint32_t>(y);
		}

		// The bin of an extent's middle, or false for an extent too big for one.
		static bool BinOf(Extent const &e, int32_t &x, int32_t &y) {
			if (e.Empty() || e.v[2] - e.v[0] > kBinMetres || e.v[3] - e.v[1] > kBinMetres) return false;
			x = static_cast<int32_t>(std::floor((e.v[0] + e.v[2]) * 0.5f / kBinMetres));
			y = static_cast<int32_t>(std::floor((e.v[1] + e.v[3]) * 0.5f / kBinMetres));
			return true;
		}

		void Index(Entry const &entry, Extent const &extent) {
			int32_t x, y;
			std::vector<Entry> &list = BinOf(extent, x, y) ? bins[BinKey(x, y)] : large;
			list.push_back(entry);
			// An item indexed again in the same place leaves a copy behind: drop them as the list doubles.
			if (list.size() >= 64 && (list.size() & (list.size() - 1)) == 0) Unique(list);
		}

		static void Unique(std::vector<Entry> &list) {
			std::sort(list.begin(), list.end());
			list.erase(std::unique(list.begin(), list.end()), list.end());
		}

		// The item an entry names, by its extent, or null when it is gone.
		Extent const *Find(Entry const &entry) {
			switch (entry.kind) {
				case 0: {
					auto const found = models.find({entry.key, entry.group, entry.instance});
					return found == models.end() ? nullptr : &found->second.extent;
				}
				case 1: {
					auto const found = grid.find(entry.key);
					return found == grid.end() ? nullptr : &found->second.extent;
				}
				case 2: {
					auto const found = cells.find(entry.key);
					return found == cells.end() ? nullptr : &found->second.extent;
				}
				default: {
					auto const found = viewPieces.find(entry.key);
					return found == viewPieces.end() ? nullptr : &pieces[found->second].extent;
				}
			}
		}

		// Everything that may lie in a screen rectangle: the bins around it, then the large list. Drops the entries
		// that no longer say where their item is.
		void Gather(Rect const &rect) {
			float const *p = camera.projection, *v = camera.view;
			float const w = static_cast<float>(camera.width) * 0.5f, h = static_cast<float>(camera.height) * 0.5f;
			// ToScreen backwards: pixels to the view's rotated x and y.
			float const x0 = ((rect.left / w - 1.0f) - p[12]) / p[0] - v[12];
			float const x1 = ((rect.right / w - 1.0f) - p[12]) / p[0] - v[12];
			float const y0 = ((1.0f - rect.bottom / h) - p[13]) / p[5] - v[13];
			float const y1 = ((1.0f - rect.top / h) - p[13]) / p[5] - v[13];
			// An item's middle lies at most half a bin outside what it overlaps.
			int32_t const bx0 = static_cast<int32_t>(std::floor(x0 / kBinMetres)) - 1;
			int32_t const bx1 = static_cast<int32_t>(std::floor(x1 / kBinMetres)) + 1;
			int32_t const by0 = static_cast<int32_t>(std::floor(y0 / kBinMetres)) - 1;
			int32_t const by1 = static_cast<int32_t>(std::floor(y1 / kBinMetres)) + 1;
			for (int32_t bx = bx0; bx <= bx1; ++bx) {
				for (int32_t by = by0; by <= by1; ++by) {
					auto const found = bins.find(BinKey(bx, by));
					if (found == bins.end()) continue;
					Unique(found->second);
					std::erase_if(found->second, [&](Entry const &entry) {
						Extent const *const extent = Find(entry);
						int32_t x, y;
						if (extent == nullptr || !BinOf(*extent, x, y) || x != bx || y != by) return true;
						candidates.push_back(entry);
						return false;
					});
				}
			}
		}

		// The frame.
		uint64_t frame = 0;
		bool staticWindow = true;
		bool owning = false, composited = false;
		std::vector<nSCDWire::DrawOwner> owners;
		int32_t lastPass = -1;
		uint64_t serial = 0;
		bool redrawOpen = false;
		std::vector<Redraw> redraws;
		std::vector<Fresh> background;
		// The view static draws are made with, from the frame's camera loads and the view objects' brackets.
		float captureView[16]{};
		bool captureViewKnown = false;
		uint32_t viewBracket[3]{};
		uint32_t drawOrdinal = 0;
		// The open bracket's fresh triangles: terrain by cell, a grid drawable's as they come.
		std::unordered_map<uint64_t, std::vector<Fresh> > freshCells;
		std::vector<Fresh> freshGrid;
		// Grid draws per 16 metre spot in this redraw.
		std::unordered_map<uint64_t, uint32_t> spots;
		// Models captured this frame, and the one being captured.
		std::vector<ModelKey> modelsThisFrame;
		Model *capturing = nullptr;
		uint64_t capturingDrawable = 0;
		uint32_t lastMesh = 0;

		Camera camera;
		uint32_t cameraId = 0;
		Totals totals;
		std::map<int32_t, Zoom> zooms;
		std::vector<uint8_t> staging;
		std::vector<InstanceRows> instanceScratch;

		// GPU timing of the composite.
		ComPtr<ID3D12QueryHeap> queries;
		ComPtr<ID3D12Resource> queryReadback;
		ComPtr<ID3D12Fence> fence;
		HANDLE fenceEvent = nullptr;
		uint64_t fenceValue = 0;
		uint64_t slotFence[kTimingSlots]{};
		bool slotTimed[kTimingSlots]{};
		int32_t slotZoom[kTimingSlots]{};
		double tickMilliseconds = 0.0;

		// What M5 needs (FrameInputs): the city's colour before the UI and the camera's motion.
		OwnedScene::FrameInputs inputs;
		float jitter[2]{};
		float look[2]{};
		Camera previousCamera;
		ComPtr<ID3D12Resource> cityColor, motion;
		ComPtr<ID3D12DescriptorHeap> motionTargets, motionViews;
		ComPtr<ID3D12RootSignature> motionSignature;
		ComPtr<ID3D12PipelineState> motionPipeline;
		bool cityCopied = false;

		bool CreateMotion() {
			ID3D12Device *const device = driver.Device();
			D3D12_DESCRIPTOR_RANGE const range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
			D3D12_ROOT_PARAMETER parameters[2]{};
			parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
			parameters[0].Constants = {0, 0, 36};
			parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
			parameters[1].DescriptorTable = {1, &range};
			parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
			D3D12_ROOT_SIGNATURE_DESC const signature{2, parameters, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
			ComPtr<ID3DBlob> serialized, errors, vs, ps;
			if (FAILED(D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
			    FAILED(device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
			                                       IID_PPV_ARGS(&motionSignature))) ||
			    FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, "OwnedScene", nullptr, nullptr, "FullScreenVS",
			                      "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vs, &errors)) ||
			    FAILED(D3DCompile(kMotionShader, sizeof(kMotionShader) - 1, "OwnedScene", nullptr, nullptr, "MotionPS",
			                      "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &ps, &errors))) return false;
			D3D12_GRAPHICS_PIPELINE_STATE_DESC description{};
			description.pRootSignature = motionSignature.Get();
			description.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
			description.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
			description.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
			description.SampleMask = UINT_MAX;
			description.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
			description.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
			description.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
			description.NumRenderTargets = 1;
			description.RTVFormats[0] = DXGI_FORMAT_R16G16_FLOAT;
			description.SampleDesc.Count = 1;
			if (FAILED(device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&motionPipeline)))) return false;
			D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
			if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&motionTargets)))) return false;
			heap = {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
			return SUCCEEDED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&motionViews)));
		}

		// The colour copy and the motion target, at the window's size. Both rest in RENDER_TARGET.
		bool Targets(int32_t width, int32_t height) {
			if (cityColor && inputs.outputWidth == static_cast<uint32_t>(width) && inputs.outputHeight == static_cast<uint32_t>(height)) {
				return true;
			}
			if (!motionPipeline && !CreateMotion()) return false;
			WaitFor(fenceValue);
			D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
			D3D12_RESOURCE_DESC description = driver.BackBuffer()->GetDesc();
			description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			cityColor.Reset();
			motion.Reset();
			D3D12_CLEAR_VALUE const black{description.Format, {0, 0, 0, 1}};
			if (FAILED(driver.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &black,
			                                                    IID_PPV_ARGS(&cityColor)))) return false;
			description.Format = DXGI_FORMAT_R16G16_FLOAT;
			description.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
			D3D12_CLEAR_VALUE const clear{DXGI_FORMAT_R16G16_FLOAT, {0, 0, 0, 0}};
			if (FAILED(driver.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
			                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
			                                                    IID_PPV_ARGS(&motion)))) return false;
			driver.Device()->CreateRenderTargetView(motion.Get(), nullptr, motionTargets->GetCPUDescriptorHandleForHeapStart());
			inputs.outputWidth = inputs.renderWidth = static_cast<uint32_t>(width);
			inputs.outputHeight = inputs.renderHeight = static_cast<uint32_t>(height);
			return true;
		}

		void Barrier(ID3D12Resource *resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
			driver.CommandList()->ResourceBarrier(1, &barrier);
		}

		// The camera's motion from the depth the owned view left, without jitter.
		void Motion(Camera const &now) {
			if (!Targets(now.width, now.height)) return;
			Camera const &before = previousCamera.known ? previousCamera : now;
			float constants[36];
			float pv[16];
			Multiply(now.projection, now.view, pv);
			if (!Invert(pv, constants)) return;
			Multiply(before.projection, before.view, constants + 16);
			constants[32] = static_cast<float>(now.width);
			constants[33] = static_cast<float>(now.height);
			constants[34] = 1.0f / constants[32];
			constants[35] = 1.0f / constants[33];
			ID3D12GraphicsCommandList *const list = driver.CommandList();
			ID3D12Resource *const depth = driver.DepthBuffer();
			D3D12_SHADER_RESOURCE_VIEW_DESC view{};
			view.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
			view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			view.Texture2D.MipLevels = 1;
			driver.Device()->CreateShaderResourceView(depth, &view, motionViews->GetCPUDescriptorHandleForHeapStart());
			Barrier(depth, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
			ID3D12DescriptorHeap *const heaps[]{motionViews.Get()};
			list->SetDescriptorHeaps(1, heaps);
			list->SetGraphicsRootSignature(motionSignature.Get());
			list->SetGraphicsRoot32BitConstants(0, 36, constants, 0);
			list->SetGraphicsRootDescriptorTable(1, motionViews->GetGPUDescriptorHandleForHeapStart());
			D3D12_VIEWPORT const full{0.0f, 0.0f, constants[32], constants[33], 0.0f, 1.0f};
			D3D12_RECT const all{0, 0, now.width, now.height};
			list->RSSetViewports(1, &full);
			list->RSSetScissorRects(1, &all);
			D3D12_CPU_DESCRIPTOR_HANDLE const target = motionTargets->GetCPUDescriptorHandleForHeapStart();
			list->OMSetRenderTargets(1, &target, FALSE, nullptr);
			list->SetPipelineState(motionPipeline.Get());
			list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			list->DrawInstanced(3, 1, 0, 0);
			Barrier(depth, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
			driver.Rebind();
		}

		// The city as it stands once the dynamic passes are done, before the UI draws over it.
		void CopyCity() {
			if (!cityColor) return;
			ID3D12Resource *const back = driver.BackBuffer();
			Barrier(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
			Barrier(cityColor.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
			driver.CommandList()->CopyResource(cityColor.Get(), back);
			Barrier(cityColor.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
			Barrier(back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
			cityCopied = true;
		}

		Impl(cGDriver12 &driver, SceneLane &lane) : driver(driver), lane(lane), pool(driver), slots(driver) {
			ID3D12Device *const device = driver.Device();
			D3D12_QUERY_HEAP_DESC const heap{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kTimingSlots * 2};
			D3D12_HEAP_PROPERTIES const readbackHeap{D3D12_HEAP_TYPE_READBACK};
			D3D12_RESOURCE_DESC description{};
			description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
			description.Width = kTimingSlots * 2 * sizeof(uint64_t);
			description.Height = description.DepthOrArraySize = description.MipLevels = 1;
			description.SampleDesc.Count = 1;
			description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
			uint64_t frequency = 0;
			if (FAILED(device->CreateQueryHeap(&heap, IID_PPV_ARGS(&queries))) ||
			    FAILED(device->CreateCommittedResource(&readbackHeap, D3D12_HEAP_FLAG_NONE, &description,
			                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
			                                           IID_PPV_ARGS(&queryReadback))) ||
			    FAILED(driver.Queue()->GetTimestampFrequency(&frequency)) || frequency == 0 ||
			    FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
				queries.Reset();
				return;
			}
			tickMilliseconds = 1000.0 / static_cast<double>(frequency);
			fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
		}

		~Impl() {
			if (fence) {
				WaitFor(fenceValue);
				for (uint32_t slot = 0; slot < kTimingSlots; ++slot) ReadTimes(slot);
			}
			if (fenceEvent != nullptr) CloseHandle(fenceEvent);
		}

		void WaitFor(uint64_t value) {
			if (!fence || fence->GetCompletedValue() >= value) return;
			fence->SetEventOnCompletion(value, fenceEvent);
			WaitForSingleObject(fenceEvent, INFINITE);
		}

		void ReadTimes(uint32_t slot) {
			if (!slotTimed[slot]) return;
			slotTimed[slot] = false;
			D3D12_RANGE const range{slot * 16, slot * 16 + 16};
			uint64_t *ticks = nullptr;
			if (FAILED(queryReadback->Map(0, &range, reinterpret_cast<void **>(&ticks)))) return;
			uint64_t const begin = ticks[slot * 2], end = ticks[slot * 2 + 1];
			D3D12_RANGE const none{0, 0};
			queryReadback->Unmap(0, &none);
			if (end <= begin) return;
			float const milliseconds = static_cast<float>(static_cast<double>(end - begin) * tickMilliseconds);
			totals.gpuMilliseconds.push_back(milliseconds);
			zooms[slotZoom[slot]].gpuMilliseconds.push_back(milliseconds);
		}

		// Groups

		// Clears what the shaders do not read for this state, so state the game left behind from other draws
		// does not keep equal states apart: alpha test, lights, fog and the stages that are off, the combiner
		// of a stage that does not combine, and the texture factor when nothing reads it.
		static void Normalize(cGDriver12::CapturedState &state) {
			uint8_t *const c = state.constants;
			uint32_t flags;
			std::memcpy(&flags, c + 192, sizeof(flags));
			if ((flags & 8) == 0) std::memset(c + 196, 0, 8);
			if ((flags & 4) == 0) std::memset(c + 204, 0, 816 - 204);
			if ((flags & 16) == 0) std::memset(c + 1040, 0, 32);
			bool factor = false;
			for (uint32_t stage = 0; stage < 2; ++stage) {
				uint8_t *const block = c + cGDriver12::kStageOffset + stage * cGDriver12::kStageBytes;
				if ((flags & (1u << stage)) == 0) {
					uint32_t source;
					std::memcpy(&source, block + cGDriver12::kCoordinateSourceOffset, sizeof(source));
					std::memset(block, 0, cGDriver12::kStageBytes);
					std::memcpy(block + cGDriver12::kCoordinateSourceOffset, &source, sizeof(source));
					std::memset(c + cGDriver12::kTextureMatrixOffset + stage * 64, 0, 64);
					state.samplers[stage] = 0;
					continue;
				}
				uint32_t mode;
				std::memcpy(&mode, block, sizeof(mode));
				if (mode < 4) {
					// Only the environment mode and the coordinate source matter.
					std::memset(block + 4, 0, cGDriver12::kCoordinateSourceOffset - 4);
				}
				factor |= mode >= 3;
			}
			if (!factor) std::memset(c + 1072, 0, 16);
		}

		// The group for a state captured with captureView (and for a model, its mesh). The state is made
		// independent of the camera: no modelview, the projection's depth row only, and stages with coordinates
		// from view positions anchored to the world. Pins its textures.
		uint32_t GroupFor(cGDriver12::CapturedState const &captured, float const *view, D3D12_VERTEX_BUFFER_VIEW const *vertices,
		                  D3D12_INDEX_BUFFER_VIEW const *indices, uint32_t count, uint64_t rank, uint32_t layer) {
			cGDriver12::CapturedState state = captured;
			float *const modelView = ConstantMatrix(state, cGDriver12::kModelViewOffset);
			std::memset(modelView, 0, 16 * sizeof(float));
			std::memset(modelView + 32, 0, 16 * sizeof(float)); // the normal matrix
			float *const projection = ConstantMatrix(state, cGDriver12::kProjectionOffset);
			for (int row: {0, 1, 3}) {
				for (int c = 0; c < 4; ++c) projection[c * 4 + row] = 0.0f;
			}
			// Terrain cells compare their state from redraw to redraw. Elsewhere merging states would change the order
			// layers that share a spot are drawn in.
			if ((layer >> 8 & 15) == kTerrain) Normalize(state);
			bool viewCoordinates = false;
			float matrices[2][16];
			for (uint32_t stage = 0; stage < 2; ++stage) {
				float *const matrix = ConstantMatrix(state, cGDriver12::kTextureMatrixOffset + stage * 64);
				std::memcpy(matrices[stage], matrix, sizeof(matrices[stage]));
				if (!ViewCoordinates(state, stage)) continue;
				viewCoordinates = true;
				Multiply(matrices[stage], view, matrix);
				// Only x and y are read. The camera's translation leaves rounding noise in the anchored matrix, which
				// would keep the same anchor apart from frame to frame.
				for (int c = 0; c < 4; ++c) {
					for (int row = 0; row < 2; ++row) {
						float &value = matrix[c * 4 + row];
						// Scales to 16 bits of precision, offsets to 1/4096 of a texture.
						int exponent = 0;
						std::frexp(value, &exponent);
						value = c == 3 ? std::round(value * 4096.0f) / 4096.0f
						               : std::ldexp(std::round(std::ldexp(value, 16 - exponent)), exponent - 16);
					}
					matrix[c * 4 + 2] = matrix[c * 4 + 3] = 0.0f;
				}
			}
			uint32_t pins[2]{};
			for (uint32_t stage = 0; stage < 2; ++stage) {
				if (state.textures[stage] != 0) pins[stage] = driver.PinTexture(state.textures[stage]);
				state.textures[stage] = pins[stage];
			}
			uint64_t hash = HashBytes(&state, sizeof(state), layer);
			if (vertices != nullptr) {
				hash = HashBytes(&vertices->BufferLocation, sizeof(vertices->BufferLocation), hash);
				hash = HashBytes(&indices->BufferLocation, sizeof(indices->BufferLocation), hash ^ count);
			}
			auto const range = groupsByHash.equal_range(hash);
			for (auto it = range.first; it != range.second; ++it) {
				Group const &g = groups[it->second];
				if (g.layer != layer || std::memcmp(&g.state, &state, sizeof(state)) != 0 || g.model != (vertices != nullptr)) {
					continue;
				}
				if (vertices != nullptr && (g.vertices.BufferLocation != vertices->BufferLocation ||
				                            g.indices.BufferLocation != indices->BufferLocation || g.count != count)) {
					continue;
				}
				for (uint32_t pin: pins) driver.UnpinTexture(pin);
				return it->second;
			}
			uint32_t index;
			if (!freeGroups.empty()) {
				index = freeGroups.back();
				freeGroups.pop_back();
			} else {
				index = static_cast<uint32_t>(groups.size());
				groups.emplace_back();
			}
			Group &g = groups[index];
			g = {};
			g.alive = true;
			g.state = state;
			g.hash = hash;
			g.rank = rank;
			g.layer = layer;
			g.viewCoordinates = viewCoordinates;
			if (viewCoordinates) {
				std::memcpy(g.captureView, view, sizeof(g.captureView));
				std::memcpy(g.captureMatrices, matrices, sizeof(matrices));
			} else {
				g.slot = slots.Allocate(state.constants);
			}
			if (vertices != nullptr) {
				g.model = true;
				g.vertices = *vertices;
				g.indices = *indices;
				g.count = count;
			}
			groupsByHash.emplace(hash, index);
			unused.push_back(index);
			orderDirty = true;
			return index;
		}

		// A group stays until Sweep even when nothing uses it: captures still waiting to be resolved may hold it.
		void Release(uint32_t index) {
			if (--groups[index].users == 0) unused.push_back(index);
		}

		void Free(uint32_t index) {
			Group &g = groups[index];
			auto const range = groupsByHash.equal_range(g.hash);
			for (auto it = range.first; it != range.second; ++it) {
				if (it->second == index) {
					groupsByHash.erase(it);
					break;
				}
			}
			for (uint32_t pin: g.state.textures) driver.UnpinTexture(pin);
			if (g.slot != UINT32_MAX) slots.Free(g.slot, frame);
			g.alive = false;
			g.buckets.clear();
			freeGroups.push_back(index);
			orderDirty = true;
		}

		void Use(uint32_t index) { ++groups[index].users; }

		// A group that may have no users once this frame's captures are resolved. Other pieces of the same
		// frame may still take it, so it is only looked at again in Sweep.
		void DropUnused(uint32_t index) { unused.push_back(index); }

		// Frees the groups nothing uses once a frame's captures are resolved.
		void Sweep() {
			for (uint32_t index: unused) {
				if (groups[index].alive && groups[index].users == 0) Free(index);
			}
			unused.clear();
		}

		// Pieces and buckets

		uint32_t AddPiece(uint32_t group, std::vector<D3D11Vertex> &&vertices, std::vector<uint32_t> &&indices,
		                  uint64_t hash = 0) {
			uint32_t index;
			if (!freePieces.empty()) {
				index = freePieces.back();
				freePieces.pop_back();
			} else {
				index = static_cast<uint32_t>(pieces.size());
				pieces.emplace_back();
			}
			Piece &p = pieces[index];
			p = {};
			p.alive = true;
			p.group = group;
			p.vertices = std::move(vertices);
			p.indices = std::move(indices);
			p.hash = hash;
			p.serial = serial;
			p.frame = frame;
			float lo[3]{1e30f, 1e30f, 1e30f}, hi[3]{-1e30f, -1e30f, -1e30f};
			for (D3D11Vertex const &v: p.vertices) {
				p.extent.Add(captureView, v.position);
				for (int a = 0; a < 3; ++a) {
					lo[a] = (std::min)(lo[a], v.position[a]);
					hi[a] = (std::max)(hi[a], v.position[a]);
				}
			}
			Use(group);
			int32_t const tx = static_cast<int32_t>(std::floor((lo[0] + hi[0]) * 0.5f / kTileMetres));
			int32_t const tz = static_cast<int32_t>(std::floor((lo[2] + hi[2]) * 0.5f / kTileMetres));
			uint64_t const key = static_cast<uint64_t>(group) << 40 | (static_cast<uint64_t>(tx) & 0xFFFFF) << 20 |
			                     (static_cast<uint64_t>(tz) & 0xFFFFF);
			auto [found, added] = bucketsByKey.try_emplace(key, 0);
			if (added) {
				uint32_t b;
				if (!freeBuckets.empty()) {
					b = freeBuckets.back();
					freeBuckets.pop_back();
				} else {
					b = static_cast<uint32_t>(buckets.size());
					buckets.emplace_back();
				}
				buckets[b] = {};
				buckets[b].alive = true;
				buckets[b].group = group;
				buckets[b].key = key;
				groups[group].buckets.push_back(b);
				found->second = b;
			}
			Bucket &bucket = buckets[found->second];
			p.bucket = found->second;
			p.place = static_cast<uint32_t>(bucket.pieces.size());
			bucket.pieces.push_back(index);
			bucket.pending.push_back(index);
			bucket.extent.Add(p.extent);
			MarkDirty(found->second);
			return index;
		}

		void MarkDirty(uint32_t b) {
			if (buckets[b].dirty) return;
			buckets[b].dirty = true;
			dirtyBuckets.push_back(b);
		}

		void RemovePiece(uint32_t index, uint32_t reason) {
			Piece &p = pieces[index];
			if (!p.alive) return;
			Bucket &bucket = buckets[p.bucket];
			uint32_t const last = bucket.pieces.back();
			bucket.pieces[p.place] = last;
			pieces[last].place = p.place;
			bucket.pieces.pop_back();
			if (p.placed) {
				bucket.holes.emplace_back(p.firstIndex, static_cast<uint32_t>(p.indices.size()));
				bucket.deadIndices += static_cast<uint32_t>(p.indices.size());
			}
			MarkDirty(p.bucket);
			uint32_t const group = p.group;
			p.alive = false;
			p.vertices = {};
			p.indices = {};
			freePieces.push_back(index);
			++totals.removed[reason];
			Release(group);
		}

		// Puts this frame's changes on the GPU: new pieces at the end of their bucket, removed ones zeroed, a
		// bucket built again when it is full or half empty.
		void UpdateBuckets() {
			for (uint32_t b: dirtyBuckets) {
				Bucket &bucket = buckets[b];
				bucket.dirty = false;
				if (!bucket.alive) continue;
				std::erase_if(bucket.pending, [&](uint32_t piece) { return !pieces[piece].alive || pieces[piece].bucket != b; });
				if (bucket.pieces.empty()) {
					if (bucket.chunk != UINT32_MAX) pool.Free(bucket.chunk, bucket.offset, bucket.bytes, frame);
					bucketsByKey.erase(bucket.key);
					std::erase(groups[bucket.group].buckets, b);
					bucket = {};
					freeBuckets.push_back(b);
					continue;
				}
				uint32_t addVertices = 0, addIndices = 0;
				for (uint32_t piece: bucket.pending) {
					addVertices += static_cast<uint32_t>(pieces[piece].vertices.size());
					addIndices += static_cast<uint32_t>(pieces[piece].indices.size());
				}
				bool const fits = bucket.chunk != UINT32_MAX && bucket.vertexUsed + addVertices <= bucket.vertexCapacity &&
				                  bucket.indexUsed + addIndices <= bucket.indexCapacity;
				if (!fits || bucket.deadIndices * 2 > bucket.indexUsed) Rebuild(bucket);
				else Append(bucket);
				bucket.pending.clear();
				bucket.holes.clear();
			}
			dirtyBuckets.clear();
		}

		uint64_t IndexOffset(Bucket const &bucket) const {
			return Align(static_cast<uint64_t>(bucket.vertexCapacity) * sizeof(D3D11Vertex), 4);
		}

		// Writes pieces at the end of the bucket's buffer, vertices and indices each in one copy.
		void Write(Bucket &bucket, std::vector<uint32_t> const &list) {
			uint32_t vertexCount = 0, indexCount = 0;
			for (uint32_t piece: list) {
				vertexCount += static_cast<uint32_t>(pieces[piece].vertices.size());
				indexCount += static_cast<uint32_t>(pieces[piece].indices.size());
			}
			if (indexCount == 0) return;
			size_t const vertexBytes = static_cast<size_t>(vertexCount) * sizeof(D3D11Vertex);
			staging.resize(vertexBytes + static_cast<size_t>(indexCount) * 4);
			auto *const vertices = reinterpret_cast<D3D11Vertex *>(staging.data());
			auto *const indices = reinterpret_cast<uint32_t *>(staging.data() + vertexBytes);
			uint32_t v = 0, i = 0;
			for (uint32_t piece: list) {
				Piece &p = pieces[piece];
				p.placed = true;
				p.firstVertex = bucket.vertexUsed + v;
				p.firstIndex = bucket.indexUsed + i;
				std::memcpy(vertices + v, p.vertices.data(), p.vertices.size() * sizeof(D3D11Vertex));
				for (uint32_t index: p.indices) indices[i++] = index + p.firstVertex;
				v += static_cast<uint32_t>(p.vertices.size());
			}
			ID3D12Resource *source = nullptr;
			uint64_t sourceOffset = 0;
			if (!driver.Upload(staging.data(), staging.size(), source, sourceOffset)) return;
			pool.Copy(bucket.chunk, bucket.offset + static_cast<uint64_t>(bucket.vertexUsed) * sizeof(D3D11Vertex), source,
			          sourceOffset, vertexBytes);
			pool.Copy(bucket.chunk, bucket.offset + IndexOffset(bucket) + static_cast<uint64_t>(bucket.indexUsed) * 4, source,
			          sourceOffset + vertexBytes, static_cast<uint64_t>(indexCount) * 4);
			bucket.vertexUsed += vertexCount;
			bucket.indexUsed += indexCount;
			totals.appendedBytes += staging.size();
		}

		void Append(Bucket &bucket) {
			// Zeroed indices make degenerate triangles of the pieces that went.
			for (auto const &[first, count]: bucket.holes) {
				staging.assign(static_cast<size_t>(count) * 4, 0);
				ID3D12Resource *source = nullptr;
				uint64_t sourceOffset = 0;
				if (!driver.Upload(staging.data(), staging.size(), source, sourceOffset)) continue;
				pool.Copy(bucket.chunk, bucket.offset + IndexOffset(bucket) + static_cast<uint64_t>(first) * 4, source,
				          sourceOffset, staging.size());
			}
			Write(bucket, bucket.pending);
		}

		void Rebuild(Bucket &bucket) {
			++totals.rebuilds;
			if (bucket.chunk != UINT32_MAX) pool.Free(bucket.chunk, bucket.offset, bucket.bytes, frame);
			bucket.chunk = UINT32_MAX;
			uint32_t vertexCount = 0, indexCount = 0;
			bucket.extent = {};
			for (uint32_t piece: bucket.pieces) {
				vertexCount += static_cast<uint32_t>(pieces[piece].vertices.size());
				indexCount += static_cast<uint32_t>(pieces[piece].indices.size());
				bucket.extent.Add(pieces[piece].extent);
			}
			// Room to grow, so a scroll adds to it in place.
			bucket.vertexCapacity = (std::max)(vertexCount * 2, 64u);
			bucket.indexCapacity = (std::max)(indexCount * 2, 96u);
			bucket.vertexUsed = bucket.indexUsed = bucket.deadIndices = 0;
			bucket.bytes = IndexOffset(bucket) + static_cast<uint64_t>(bucket.indexCapacity) * 4;
			if (!pool.Allocate(bucket.bytes, bucket.chunk, bucket.offset)) {
				bucket.chunk = UINT32_MAX;
				return;
			}
			Write(bucket, bucket.pieces);
		}

		// Capture

		// stack: for the grid, how many grid draws this redraw put on the same spot before, so layers of a lot
		// (a path over the lot's ground) keep their order while groups are drawn one after the other.
		uint32_t RankedGroup(cGDriver12::CapturedState const &state, CaptureKind kind, uint32_t pass, uint32_t stack = 0) {
			uint64_t rank;
			if (kind == kBackground) rank = drawOrdinal;
			else if (kind == kGrid) rank = 2ull << 48 | static_cast<uint64_t>(stack) << 40 | gridRank;
			else rank = static_cast<uint64_t>(pass + 1) << 48 | static_cast<uint64_t>(viewBracket[pass % 3]) << 32 | drawOrdinal;
			uint32_t const group = GroupFor(state, captureView, nullptr, nullptr, 0, rank, stack << 12 | kind << 8 | pass);
			if (groups[group].rank == rank) groups[group].kind = kind;
			if (kind == kGrid && groups[group].rank == rank) ++gridRank;
			return group;
		}

		// The capture's triangles in world space: the draw's modelview over the capture view, or as they are
		// when the draw used that view.
		void ToWorld(cGDriver12::CapturedState const &state, D3D11Vertex const *vertices, uint32_t vertexCount,
		             std::vector<D3D11Vertex> &out) {
			out.assign(vertices, vertices + vertexCount);
			float const *const modelView = reinterpret_cast<float const *>(state.constants + cGDriver12::kModelViewOffset);
			if (std::memcmp(modelView, captureView, 16 * sizeof(float)) == 0) return;
			float inverse[16], world[16];
			if (!Invert(captureView, inverse)) return;
			Multiply(inverse, modelView, world);
			for (D3D11Vertex &v: out) {
				float const p[3]{v.position[0], v.position[1], v.position[2]};
				float const n[3]{v.normal[0], v.normal[1], v.normal[2]};
				for (int a = 0; a < 3; ++a) {
					v.position[a] = world[a] * p[0] + world[4 + a] * p[1] + world[8 + a] * p[2] + world[12 + a];
					v.normal[a] = world[a] * n[0] + world[4 + a] * n[1] + world[8 + a] * n[2];
				}
			}
		}

		// Stages with coordinates from view positions get them computed once, into the vertices' own texture
		// coordinates, as the capture view saw them. The state then reads those, so draws that only differ in where
		// their texture lies (shadows and decals, each placed on its own) share a state.
		void Bake(cGDriver12::CapturedState &state, std::vector<D3D11Vertex> &world) {
			uint32_t flags;
			std::memcpy(&flags, state.constants + 192, sizeof(flags));
			uint32_t sources[2];
			for (uint32_t stage = 0; stage < 2; ++stage) {
				std::memcpy(&sources[stage], state.constants + cGDriver12::kStageOffset + stage * cGDriver12::kStageBytes +
				                             cGDriver12::kCoordinateSourceOffset, sizeof(uint32_t));
			}
			for (uint32_t stage = 0; stage < 2; ++stage) {
				if (!ViewCoordinates(state, stage) || (flags & (1u << stage)) == 0) continue;
				// The other stage may read this stage's texture coordinates.
				uint32_t const other = 1 - stage;
				if ((flags & (1u << other)) != 0 && !ViewCoordinates(state, other) && sources[other] == stage) continue;
				float *const matrix = ConstantMatrix(state, cGDriver12::kTextureMatrixOffset + stage * 64);
				float t[16];
				Multiply(matrix, captureView, t);
				for (D3D11Vertex &v: world) {
					float const *const p = v.position;
					v.texCoord[stage][0] = t[0] * p[0] + t[4] * p[1] + t[8] * p[2] + t[12];
					v.texCoord[stage][1] = t[1] * p[0] + t[5] * p[1] + t[9] * p[2] + t[13];
				}
				std::memset(matrix, 0, 16 * sizeof(float));
				matrix[0] = matrix[5] = matrix[10] = matrix[15] = 1.0f;
				std::memcpy(state.constants + cGDriver12::kStageOffset + stage * cGDriver12::kStageBytes +
				            cGDriver12::kCoordinateSourceOffset, &stage, sizeof(stage));
			}
		}

		bool CaptureDraw(cGDriver12::CapturedState const &captured, D3D11Vertex const *vertices, uint32_t vertexCount,
		                 uint32_t const *indices, uint32_t indexCount) {
			cGDriver12::CapturedState state = captured;
			nSCDWire::DrawOwner const *const owner = owners.empty() ? nullptr : &owners.back();
			float const *const modelView = reinterpret_cast<float const *>(state.constants + cGDriver12::kModelViewOffset);
			if (owner == nullptr) {
				// Before the first owner: the background quads of a full redraw are in the world, the quad
				// that resets depth is on the screen.
				if (!IsCameraView(modelView)) {
					++totals.screenDraws;
					return owning;
				}
				std::memcpy(captureView, modelView, sizeof(captureView));
				captureViewKnown = true;
				Fresh &fresh = background.emplace_back();
				ToWorld(state, vertices, vertexCount, fresh.vertices);
				Bake(state, fresh.vertices);
				fresh.group = RankedGroup(state, kBackground, 0);
				fresh.indices.assign(indices, indices + indexCount);
				++drawOrdinal;
				return owning;
			}
			if (!captureViewKnown) {
				++totals.uncaptured;
				return owning;
			}
			uint32_t const pass = owner->pass;
			if (owner->kind == nSCDWire::DrawOwnerKind::View && owner->vtable == kTerrainVtable) {
				std::vector<D3D11Vertex> world;
				ToWorld(state, vertices, vertexCount, world);
				Bake(state, world);
				uint32_t const group = RankedGroup(state, kTerrain, pass);
				++drawOrdinal;
				// Each triangle goes to the cell under its middle.
				for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
					D3D11Vertex const &a = world[indices[i]], &b = world[indices[i + 1]], &c = world[indices[i + 2]];
					int32_t const x = static_cast<int32_t>(std::floor((a.position[0] + b.position[0] + c.position[0]) / (3.0f * kCellMetres)));
					int32_t const z = static_cast<int32_t>(std::floor((a.position[2] + b.position[2] + c.position[2]) / (3.0f * kCellMetres)));
					uint64_t const key = static_cast<uint64_t>(pass) << 48 | (static_cast<uint64_t>(x) & 0xFFFFFF) << 24 |
					                     (static_cast<uint64_t>(z) & 0xFFFFFF);
					std::vector<Fresh> &list = freshCells[key];
					if (list.empty() || list.back().group != group) list.push_back({group, {}, {}});
					Fresh &fresh = list.back();
					uint32_t const base = static_cast<uint32_t>(fresh.vertices.size());
					fresh.vertices.insert(fresh.vertices.end(), {a, b, c});
					fresh.indices.insert(fresh.indices.end(), {base, base + 1, base + 2});
				}
				++totals.captured[kTerrain];
				return owning;
			}
			if (owner->kind == nSCDWire::DrawOwnerKind::View) {
				std::vector<D3D11Vertex> world;
				ToWorld(state, vertices, vertexCount, world);
				Bake(state, world);
				uint32_t const group = RankedGroup(state, kView, pass);
				++drawOrdinal;
				uint64_t hash = HashBytes(world.data(), world.size() * sizeof(D3D11Vertex), group);
				hash = HashBytes(indices, indexCount * sizeof(uint32_t), hash ^ owner->vtable);
				auto const found = viewPieces.find(hash);
				if (found != viewPieces.end()) {
					pieces[found->second].serial = serial;
					pieces[found->second].frame = frame;
					DropUnused(group);
				} else {
					uint32_t const piece = AddPiece(group, std::move(world), std::vector<uint32_t>(indices, indices + indexCount), hash);
					viewPieces.emplace(hash, piece);
					Index({3, hash, 0, 0}, pieces[piece].extent);
				}
				++totals.captured[kView];
				return owning;
			}
			Fresh &fresh = freshGrid.emplace_back();
			ToWorld(state, vertices, vertexCount, fresh.vertices);
			Bake(state, fresh.vertices);
			fresh.indices.assign(indices, indices + indexCount);
			float lo[2]{1e30f, 1e30f}, hi[2]{-1e30f, -1e30f};
			for (D3D11Vertex const &v: fresh.vertices) {
				lo[0] = (std::min)(lo[0], v.position[0]);
				hi[0] = (std::max)(hi[0], v.position[0]);
				lo[1] = (std::min)(lo[1], v.position[2]);
				hi[1] = (std::max)(hi[1], v.position[2]);
			}
			int32_t const x = static_cast<int32_t>(std::floor((lo[0] + hi[0]) * 0.5f / kCellMetres));
			int32_t const z = static_cast<int32_t>(std::floor((lo[1] + hi[1]) * 0.5f / kCellMetres));
			uint32_t &count = spots[static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32 | static_cast<uint32_t>(z)];
			fresh.group = RankedGroup(state, kGrid, pass, (std::min)(count++, 15u));
			++totals.captured[kGrid];
			return owning;
		}

		static bool Same(Fresh const &fresh, Piece const &piece) {
			return fresh.group == piece.group && fresh.vertices.size() == piece.vertices.size() &&
			       fresh.indices == piece.indices &&
			       std::memcmp(fresh.vertices.data(), piece.vertices.data(), fresh.vertices.size() * sizeof(D3D11Vertex)) == 0;
		}

		// A terrain bracket ended: each cell it drew now holds what it drew, and keeps the pieces that did not
		// change.
		void ResolveCells() {
			for (auto &[key, list]: freshCells) {
				Cell &cell = cells[key];
				std::vector<uint32_t> kept;
				bool changed = false;
				for (Fresh &fresh: list) {
					uint32_t match = UINT32_MAX;
					for (uint32_t piece: cell.pieces) {
						if (pieces[piece].alive && Same(fresh, pieces[piece]) &&
						    std::find(kept.begin(), kept.end(), piece) == kept.end()) {
							match = piece;
							break;
						}
					}
					if (match != UINT32_MAX) {
						kept.push_back(match);
						DropUnused(fresh.group);
					} else {
						kept.push_back(AddPiece(fresh.group, std::move(fresh.vertices), std::move(fresh.indices)));
						changed = true;
					}
				}
				// Earlier draws of this redraw stay, older ones go.
				for (uint32_t piece: cell.pieces) {
					if (std::find(kept.begin(), kept.end(), piece) != kept.end()) continue;
					if (pieces[piece].serial == serial) kept.push_back(piece);
					else {
						RemovePiece(piece, 2);
						changed = true;
					}
				}
				cell.pieces = std::move(kept);
				cell.extent = {};
				for (uint32_t piece: cell.pieces) {
					pieces[piece].serial = serial;
					pieces[piece].frame = frame;
					cell.extent.Add(pieces[piece].extent);
				}
				cell.serial = serial;
				cell.frame = frame;
				if (changed) Index({2, key, 0, 0}, cell.extent);
				++(changed ? totals.cellsChanged : totals.cellsKept);
			}
			freshCells.clear();
		}

		// A grid drawable's bracket ended. Drawn again in the same redraw, it adds to what it drew.
		void ResolveGrid(nSCDWire::DrawOwner const &owner) {
			GridItem &item = grid[owner.drawable];
			bool const again = item.serial == serial && item.frame == frame;
			bool same = !again && item.pieces.size() == freshGrid.size();
			for (size_t i = 0; same && i < freshGrid.size(); ++i) same = Same(freshGrid[i], pieces[item.pieces[i]]);
			if (same) {
				for (Fresh const &fresh: freshGrid) DropUnused(fresh.group);
			} else {
				if (!again) {
					for (uint32_t piece: item.pieces) RemovePiece(piece, 1);
					item.pieces.clear();
					item.extent = {};
				}
				for (Fresh &fresh: freshGrid) {
					uint32_t const piece = AddPiece(fresh.group, std::move(fresh.vertices), std::move(fresh.indices));
					item.pieces.push_back(piece);
					item.extent.Add(pieces[piece].extent);
				}
			}
			if (item.occupant != owner.occupant) {
				if (item.occupant != 0) std::erase(gridByOccupant[item.occupant], owner.drawable);
				if (owner.occupant != 0) gridByOccupant[owner.occupant].push_back(owner.drawable);
				item.occupant = owner.occupant;
			}
			if (!same) Index({1, owner.drawable, 0, 0}, item.extent);
			item.serial = serial;
			item.frame = frame;
			item.orphan = false;
			freshGrid.clear();
			if (item.pieces.empty()) EraseGrid(owner.drawable);
		}

		// Whether the redraw going on covers the whole window.
		bool FullRedraw() const {
			if (redraws.empty() || !camera.known) return false;
			int32_t const *const v = redraws.back().viewport;
			return v[0] == 0 && v[1] == 0 && v[2] == camera.width && v[3] == camera.height;
		}

		void EraseGrid(uint64_t drawable) {
			auto const found = grid.find(drawable);
			if (found == grid.end()) return;
			for (uint32_t piece: found->second.pieces) RemovePiece(piece, 1);
			if (found->second.occupant != 0) {
				auto const list = gridByOccupant.find(found->second.occupant);
				if (list != gridByOccupant.end()) {
					std::erase(list->second, drawable);
					if (list->second.empty()) gridByOccupant.erase(list);
				}
			}
			grid.erase(found);
		}

		void EraseModel(ModelKey const &key) {
			auto const found = models.find(key);
			if (found == models.end()) return;
			for (uint32_t group: found->second.meshes) Release(group);
			auto const list = modelsByOccupant.find(key.occupant);
			if (list != modelsByOccupant.end()) {
				std::erase(list->second, key);
				if (list->second.empty()) modelsByOccupant.erase(list);
			}
			if (capturing == &found->second) capturing = nullptr;
			models.erase(found);
		}

		void CommitBackground() {
			for (Fresh &fresh: background) {
				uint64_t hash = HashBytes(fresh.vertices.data(), fresh.vertices.size() * sizeof(D3D11Vertex), fresh.group);
				hash = HashBytes(fresh.indices.data(), fresh.indices.size() * sizeof(uint32_t), hash);
				auto const found = viewPieces.find(hash);
				if (found != viewPieces.end()) {
					pieces[found->second].serial = serial;
					pieces[found->second].frame = frame;
					DropUnused(fresh.group);
				} else {
					uint32_t const piece = AddPiece(fresh.group, std::move(fresh.vertices), std::move(fresh.indices), hash);
					viewPieces.emplace(hash, piece);
					Index({3, hash, 0, 0}, pieces[piece].extent);
				}
				++totals.captured[kBackground];
			}
			background.clear();
		}

		void DropBackground() {
			for (Fresh const &fresh: background) DropUnused(fresh.group);
			background.clear();
		}

		// Composite

		// The frame's camera, as SC4 left it for the dynamic passes. False when what is loaded is not a city
		// camera over the whole window.
		bool ReadCamera(Camera &out) {
			cGDriver12::Transform transform{};
			driver.SaveTransform(transform);
			float const *const p = transform.projection;
			float const *const v = transform.modelView;
			int32_t viewport[4];
			driver.GetViewport(viewport);
			if (viewport[0] != 0 || viewport[1] != 0 || viewport[2] <= 0 || viewport[3] <= 0) return false;
			if (p[3] != 0.0f || p[7] != 0.0f || p[11] != 0.0f || p[15] != 1.0f || p[0] <= 0.0f || p[5] <= 0.0f) return false;
			if (p[4] != 0.0f || p[8] != 0.0f || p[1] != 0.0f || p[9] != 0.0f) return false;
			if (!IsCameraView(v)) return false;
			out.known = true;
			std::memcpy(out.projection, p, sizeof(out.projection));
			std::memcpy(out.view, v, sizeof(out.view));
			out.width = viewport[2];
			out.height = viewport[3];
			return true;
		}

		void KillAll(bool keepFresh) {
			std::vector<ModelKey> modelKeys;
			for (auto const &[key, model]: models) {
				if (!keepFresh || model.frame != frame) modelKeys.push_back(key);
			}
			for (ModelKey const &key: modelKeys) {
				EraseModel(key);
				++totals.removed[0];
			}
			std::vector<uint64_t> drawables;
			for (auto const &[drawable, item]: grid) {
				if (!keepFresh || item.frame != frame) drawables.push_back(drawable);
			}
			for (uint64_t drawable: drawables) EraseGrid(drawable);
			for (auto it = cells.begin(); it != cells.end();) {
				if (keepFresh && it->second.frame == frame) {
					++it;
					continue;
				}
				for (uint32_t piece: it->second.pieces) RemovePiece(piece, 2);
				it = cells.erase(it);
			}
			for (auto it = viewPieces.begin(); it != viewPieces.end();) {
				if (keepFresh && pieces[it->second].frame == frame) {
					++it;
					continue;
				}
				RemovePiece(it->second, 3);
				it = viewPieces.erase(it);
			}
		}

		// What each redraw of this frame says is gone.
		void Kill() {
			if (redraws.empty()) return;
			std::vector<std::pair<Rect, uint64_t> > rects;
			for (Redraw const &redraw: redraws) {
				float const height = static_cast<float>(camera.height);
				rects.push_back({{static_cast<float>(redraw.viewport[0]), height - static_cast<float>(redraw.viewport[3]),
				                  static_cast<float>(redraw.viewport[2]), height - static_cast<float>(redraw.viewport[1])},
				                 redraw.serial});
			}
			// Only what lies near a redraw can be gone.
			candidates.clear();
			for (auto const &[rect, serial]: rects) Gather(rect);
			Unique(large);
			std::erase_if(large, [&](Entry const &entry) { return Find(entry) == nullptr; });
			candidates.insert(candidates.end(), large.begin(), large.end());
			std::sort(candidates.begin(), candidates.end());
			candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
			for (Entry const &entry: candidates) {
				Extent const *const extent = Find(entry);
				if (extent == nullptr) continue;
				Rect const r = ToScreen(*extent, camera);
				switch (entry.kind) {
					case 0: {
						ModelKey const key{entry.key, entry.group, entry.instance};
						Model const &model = models.find(key)->second;
						if (model.frame == frame) break;
						for (auto const &[rect, serial]: rects) {
							if (model.orphan ? Overlaps(r, rect) : Inside(r, rect)) {
								EraseModel(key);
								++totals.removed[0];
								break;
							}
						}
						break;
					}
					case 1: {
						GridItem const &item = grid.find(entry.key)->second;
						if (item.frame == frame) break;
						for (auto const &[rect, serial]: rects) {
							if (item.orphan ? Overlaps(r, rect) : Inside(r, rect)) {
								EraseGrid(entry.key);
								break;
							}
						}
						break;
					}
					case 2: {
						auto const found = cells.find(entry.key);
						bool gone = false;
						for (auto const &[rect, redrawSerial]: rects) gone |= found->second.serial < redrawSerial && Inside(r, rect);
						if (!gone) break;
						for (uint32_t piece: found->second.pieces) RemovePiece(piece, 2);
						cells.erase(found);
						break;
					}
					default: {
						auto const found = viewPieces.find(entry.key);
						Piece const &p = pieces[found->second];
						bool gone = false;
						for (auto const &[rect, redrawSerial]: rects) gone |= p.serial < redrawSerial && Overlaps(r, rect);
						if (!gone) break;
						RemovePiece(found->second, 3);
						viewPieces.erase(found);
						break;
					}
				}
			}
		}

		void Composite() {
			composited = true;
			auto const start = std::chrono::steady_clock::now();
			Camera now;
			if (!ReadCamera(now)) {
				++totals.skipped;
				return;
			}
			ID3D12GraphicsCommandList *const list = driver.CommandList();
			uint32_t const slot = static_cast<uint32_t>(frame % kTimingSlots);
			if (queries) {
				WaitFor(slotFence[slot]);
				ReadTimes(slot);
				list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2);
			}
			if (camera.known && !SameView(now, camera)) {
				++totals.viewChanges;
				KillAll(true);
			}
			if (!camera.known || std::memcmp(now.view, camera.view, sizeof(now.view)) != 0 ||
			    std::memcmp(now.projection, camera.projection, sizeof(now.projection)) != 0) {
				++cameraId;
			}
			camera = now;

			// What SC4 drew this frame normally has this frame's camera, and then its own modelview is used as it
			// is.
			for (ModelKey const &key: modelsThisFrame) {
				auto const found = models.find(key);
				if (found == models.end()) continue;
				Model &model = found->second;
				float mine[16];
				Multiply(now.view, model.world, mine);
				double worst = 0.0;
				for (int i = 0; i < 16; ++i) worst = (std::max)(worst, std::fabs(static_cast<double>(mine[i]) - model.modelView[i]));
				model.camera = worst < 0.01 ? cameraId : 0;
			}
			auto step = std::chrono::steady_clock::now();
			auto const lap = [&](int index) {
				auto const at = std::chrono::steady_clock::now();
				totals.steps[index] += std::chrono::duration<double, std::milli>(at - step).count();
				step = at;
			};
			Kill();
			lap(0);
			pool.Reclaim(frame);
			slots.Reclaim(frame);
			UpdateBuckets();
			Sweep();
			pool.Flush();
			lap(1);

			if (orderDirty) {
				order.clear();
				for (uint32_t g = 0; g < groups.size(); ++g) {
					if (groups[g].alive) order.push_back(g);
				}
				std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return groups[a].rank < groups[b].rank; });
				orderDirty = false;
			}

			// Looking somewhere else (SetLook): what is kept is drawn from a camera moved along the screen, while
			// what SC4 redrew was judged with its own.
			Camera const sc4 = camera;
			bool const looking = look[0] != 0.0f || look[1] != 0.0f;
			if (looking) {
				float const metres = 2.0f / (now.projection[0] * static_cast<float>(now.width));
				now.view[12] -= look[0] * metres;
				now.view[13] += look[1] * metres;
				camera = now;
			}
			// Instances: every visible model's rows per mesh group, then one record for all world geometry.
			for (uint32_t g: order) groups[g].instanceCount = 0;
			uint32_t instanceTotal = 1;
			for (auto &[key, model]: models) {
				if (!OnScreen(ToScreen(model.extent, camera), camera)) {
					++totals.culled;
					continue;
				}
				for (uint32_t group: model.meshes) ++groups[group].instanceCount;
				instanceTotal += static_cast<uint32_t>(model.meshes.size());
			}
			instanceScratch.resize(instanceTotal);
			instanceScratch[0] = Rows(now.view);
			uint32_t next = 1;
			for (uint32_t g: order) {
				groups[g].instanceFirst = next;
				next += groups[g].instanceCount;
				groups[g].instanceCount = 0;
			}
			for (auto &[key, model]: models) {
				if (!OnScreen(ToScreen(model.extent, camera), camera)) continue;
				float modelView[16];
				if (model.camera == cameraId && !looking) std::memcpy(modelView, model.modelView, sizeof(modelView));
				else Multiply(now.view, model.world, modelView);
				InstanceRows const rows = Rows(modelView);
				for (uint32_t group: model.meshes) {
					Group &g = groups[group];
					instanceScratch[g.instanceFirst + g.instanceCount++] = rows;
				}
			}
			ID3D12Resource *instanceBuffer = nullptr;
			uint64_t instanceOffset = 0;
			if (!driver.Upload(instanceScratch.data(), instanceScratch.size() * sizeof(InstanceRows), instanceBuffer,
			                   instanceOffset)) return;
			D3D12_GPU_VIRTUAL_ADDRESS const instanceBase = instanceBuffer->GetGPUVirtualAddress() + instanceOffset;

			lap(2);
			float inverseView[16];
			bool const inverted = Invert(now.view, inverseView);
			cGDriver12::Transform game{};
			driver.SaveTransform(game);
			driver.ClearTargets();
			float jittered[16];
			std::memcpy(jittered, now.projection, sizeof(jittered));
			jittered[12] += 2.0f * jitter[0] / static_cast<float>(now.width);
			jittered[13] -= 2.0f * jitter[1] / static_cast<float>(now.height);
			driver.BeginRetained(jittered);
			uint64_t draws = 0, instances = 0;
			for (uint32_t g: order) {
				Group &group = groups[g];
				if (group.model ? group.instanceCount == 0 : group.buckets.empty()) continue;
				cGDriver12::RetainedDraw draw{};
				draw.pipeline = group.state.pipeline;
				draw.depthBias = group.state.depthBias;
				draw.stencilReference = group.state.stencilReference;
				for (uint32_t stage = 0; stage < 2; ++stage) {
					draw.pins[stage] = group.state.textures[stage];
					draw.samplers[stage] = group.state.samplers[stage];
				}
				if (group.viewCoordinates) {
					// Back from the world to this frame's view, or as captured with that view.
					cGDriver12::CapturedState state = group.state;
					bool const sameView = std::memcmp(now.view, group.captureView, sizeof(now.view)) == 0;
					for (uint32_t stage = 0; stage < 2; ++stage) {
						if (!ViewCoordinates(state, stage)) continue;
						float *const matrix = ConstantMatrix(state, cGDriver12::kTextureMatrixOffset + stage * 64);
						if (sameView || !inverted) std::memcpy(matrix, group.captureMatrices[stage], 16 * sizeof(float));
						else Multiply(ConstantMatrix(group.state, cGDriver12::kTextureMatrixOffset + stage * 64), inverseView, matrix);
					}
					ID3D12Resource *resource = nullptr;
					uint64_t offset = 0;
					if (!driver.Upload(state.constants, cGDriver12::kConstantBytes, resource, offset,
					                   D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)) continue;
					draw.constants = resource->GetGPUVirtualAddress() + offset;
				} else {
					draw.constants = slots.Address(group.slot);
				}
				if (group.model) {
					draw.vertices = group.vertices;
					draw.indices = group.indices;
					draw.indexCount = group.count;
					draw.instances = {instanceBase + static_cast<uint64_t>(group.instanceFirst) * sizeof(InstanceRows),
					                  group.instanceCount * static_cast<UINT>(sizeof(InstanceRows)), sizeof(InstanceRows)};
					driver.DrawRetained(draw);
					++draws;
					instances += group.instanceCount;
					continue;
				}
				draw.instances = {instanceBase, sizeof(InstanceRows), sizeof(InstanceRows)};
				for (uint32_t b: group.buckets) {
					Bucket const &bucket = buckets[b];
					if (bucket.chunk == UINT32_MAX || bucket.indexUsed == 0) continue;
					if (!OnScreen(ToScreen(bucket.extent, camera), camera)) continue;
					pool.Ready(bucket.chunk);
					D3D12_GPU_VIRTUAL_ADDRESS const address = pool.Address(bucket.chunk, bucket.offset);
					draw.vertices = {address, bucket.vertexCapacity * static_cast<UINT>(sizeof(D3D11Vertex)), sizeof(D3D11Vertex)};
					draw.indices = {address + IndexOffset(bucket), bucket.indexCapacity * 4u, DXGI_FORMAT_R32_UINT};
					draw.indexCount = bucket.indexUsed;
					driver.DrawRetained(draw);
					++draws;
					++instances;
				}
			}
			driver.EndRetained();
			lap(3);
			driver.RestoreTransform(game);
			camera = sc4;
			Motion(now);
			inputs.valid = motion != nullptr;
			inputs.color = cityColor.Get();
			inputs.depth = driver.DepthBuffer();
			inputs.motion = motion.Get();
			std::memcpy(inputs.jitter, jitter, sizeof(jitter));
			std::memcpy(inputs.projection, now.projection, sizeof(inputs.projection));
			std::memcpy(inputs.view, now.view, sizeof(inputs.view));
			Camera const &before = previousCamera.known ? previousCamera : now;
			std::memcpy(inputs.previousProjection, before.projection, sizeof(inputs.previousProjection));
			std::memcpy(inputs.previousView, before.view, sizeof(inputs.previousView));
			previousCamera = now;
			totals.draws += draws;
			totals.instances += instances;
			++totals.composites;
			int32_t const zoom = static_cast<int32_t>(std::lround(2000.0 / (now.projection[0] * now.width)));
			if (queries) {
				list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2 + 1);
				list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, slot * 2, 2, queryReadback.Get(), slot * 16);
				slotTimed[slot] = true;
				slotZoom[slot] = zoom;
			}
			float const milliseconds = static_cast<float>(
				std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
			totals.cpuMilliseconds.push_back(milliseconds);
			Zoom &z = zooms[zoom];
			++z.frames;
			z.draws += draws;
			z.instances += instances;
			z.cpuMilliseconds.push_back(milliseconds);
		}
	};

	OwnedScene::OwnedScene(cGDriver12 &driver, SceneLane &lane) : impl(std::make_unique<Impl>(driver, lane)) {
		lane.SetSink(this);
		lane.SubstituteGrid(false);
		driver.SetCapture(this, false);
	}

	OwnedScene::~OwnedScene() {
		impl->driver.SetCapture(nullptr, false);
		impl->lane.SetSink(nullptr);
		impl->lane.SubstituteGrid(true);
	}

	void OwnedScene::OwnerBegin(nSCDWire::DrawOwner const &owner) {
		Impl &d = *impl;
		if (owner.pass <= 2 && d.staticWindow) {
			// A static redraw starts over at pass 0 (or 1 when it has no pass 0 views).
			if (static_cast<int32_t>(owner.pass) < d.lastPass || d.lastPass < 0 || d.lastPass > 2) {
				Redraw &redraw = d.redraws.emplace_back();
				d.driver.GetViewport(redraw.viewport);
				redraw.serial = ++d.serial;
				std::fill(std::begin(d.viewBracket), std::end(d.viewBracket), 0u);
				d.spots.clear();
				d.CommitBackground();
			}
			d.lastPass = owner.pass;
			if (owner.kind == nSCDWire::DrawOwnerKind::View) {
				++d.viewBracket[owner.pass];
				d.drawOrdinal = 0;
				cGDriver12::Transform transform{};
				d.driver.SaveTransform(transform);
				if (IsCameraView(transform.modelView)) {
					std::memcpy(d.captureView, transform.modelView, sizeof(d.captureView));
					d.captureViewKnown = true;
				}
			}
			d.owners.push_back(owner);
			return;
		}
		if (owner.pass > 2 && d.staticWindow) {
			d.staticWindow = false;
			d.driver.SetCapture(nullptr, false);
			d.DropBackground();
			if (d.owning) d.Composite();
		}
		d.owners.push_back(owner);
	}

	void OwnedScene::OwnerEnd() {
		Impl &d = *impl;
		if (d.owners.empty()) return;
		nSCDWire::DrawOwner const owner = d.owners.back();
		d.owners.pop_back();
		if (owner.pass > 2) {
			// Each post-dynamic view object leaves the city one step further, the last one before the UI.
			if (owner.pass == 5 && d.owners.empty() && d.composited) d.CopyCity();
			return;
		}
		if (owner.kind == nSCDWire::DrawOwnerKind::View) {
			if (owner.vtable == kTerrainVtable) d.ResolveCells();
		} else if (!d.freshGrid.empty() || (d.FullRedraw() && d.grid.count(owner.drawable) != 0)) {
			// In a partial redraw a bracket that drew nothing says nothing: a lot cell on a slope draws only where
			// the terrain is redrawn. In a full one it drew nothing because it shows nothing.
			d.ResolveGrid(owner);
		}
	}

	void OwnedScene::Occupant(nSCDWire::OccupantEvent const &event) {
		Impl &d = *impl;
		if (event.kind == nSCDWire::OccupantEventKind::Insert) return;
		auto const models = d.modelsByOccupant.find(event.id);
		if (models != d.modelsByOccupant.end()) {
			for (ModelKey const &key: models->second) d.models[key].orphan = true;
		}
		auto const drawables = d.gridByOccupant.find(event.id);
		if (drawables != d.gridByOccupant.end()) {
			for (uint64_t drawable: drawables->second) d.grid[drawable].orphan = true;
		}
	}

	void OwnedScene::VideoMode() {
		Impl &d = *impl;
		d.KillAll(false);
		d.bins.clear();
		d.large.clear();
		d.camera = {};
		d.owning = false;
	}

	void OwnedScene::EndFrame() {
		Impl &d = *impl;
		if (d.fence) {
			d.driver.Queue()->Signal(d.fence.Get(), ++d.fenceValue);
			d.slotFence[d.frame % kTimingSlots] = d.fenceValue;
		}
		d.DropBackground();
		d.Sweep();
		// A frame that never reached a dynamic owner is not the city: the next frame draws as the game says.
		d.owning = !d.staticWindow && !(d.models.empty() && d.cells.empty() && d.grid.empty());
		if (!d.composited || !d.cityCopied) d.inputs.valid = false;
		d.cityCopied = false;
		d.composited = false;
		d.staticWindow = true;
		d.redraws.clear();
		d.modelsThisFrame.clear();
		d.capturing = nullptr;
		d.lastPass = -1;
		d.owners.clear();
		d.captureViewKnown = false;
		d.drawOrdinal = 0;
		++d.frame;
		d.driver.SetCapture(this, d.owning);
	}

	void OwnedScene::SetJitter(float x, float y) {
		impl->jitter[0] = x;
		impl->jitter[1] = y;
	}

	OwnedScene::FrameInputs const &OwnedScene::Inputs() const { return impl->inputs; }

	bool OwnedScene::UpscalerCamera(UpscalerInputs &inputs) const {
		FrameInputs const &mine = impl->inputs;
		if (!mine.valid) return false;
		inputs.motion = mine.motion;
		inputs.hudless = mine.color;
		std::memcpy(inputs.jitter, mine.jitter, sizeof(inputs.jitter));
		std::memcpy(inputs.projection, mine.projection, sizeof(inputs.projection));
		std::memcpy(inputs.view, mine.view, sizeof(inputs.view));
		std::memcpy(inputs.previousProjection, mine.previousProjection, sizeof(inputs.previousProjection));
		std::memcpy(inputs.previousView, mine.previousView, sizeof(inputs.previousView));
		inputs.reset = std::memcmp(mine.projection, mine.previousProjection, 3 * sizeof(float)) != 0;
		return true;
	}

	void OwnedScene::SetLook(float x, float y) {
		impl->look[0] = x;
		impl->look[1] = y;
	}

	void OwnedScene::SaveInputs(std::wstring const &prefix) {
		Impl &d = *impl;
		if (!d.inputs.valid) return;
		uint32_t const width = d.inputs.outputWidth, height = d.inputs.outputHeight;
		// Both textures through one readback buffer: colour, then motion.
		ID3D12Device *const device = d.driver.Device();
		D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprints[2]{};
		UINT64 sizes[2]{};
		ID3D12Resource *const sources[2]{d.inputs.color, d.inputs.motion};
		for (int i = 0; i < 2; ++i) {
			D3D12_RESOURCE_DESC const description = sources[i]->GetDesc();
			device->GetCopyableFootprints(&description, 0, 1, 0, &footprints[i], nullptr, nullptr, &sizes[i]);
		}
		footprints[1].Offset = Align(sizes[0], D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
		D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_READBACK};
		D3D12_RESOURCE_DESC buffer{};
		buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		buffer.Width = footprints[1].Offset + sizes[1];
		buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
		buffer.SampleDesc.Count = 1;
		buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		ComPtr<ID3D12Resource> readback;
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
		                                           nullptr, IID_PPV_ARGS(&readback)))) return;
		// A list of its own, as the driver's has been submitted.
		ComPtr<ID3D12CommandAllocator> allocator;
		ComPtr<ID3D12GraphicsCommandList> list;
		if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
		    FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) {
			return;
		}
		for (int i = 0; i < 2; ++i) {
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition = {sources[i], D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			                      D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
			list->ResourceBarrier(1, &barrier);
			D3D12_TEXTURE_COPY_LOCATION const to{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {footprints[i]}};
			D3D12_TEXTURE_COPY_LOCATION from{sources[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
			from.SubresourceIndex = 0;
			list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
			std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
			list->ResourceBarrier(1, &barrier);
		}
		list->Close();
		ID3D12CommandList *const lists[]{list.Get()};
		d.driver.Queue()->ExecuteCommandLists(1, lists);
		d.driver.Queue()->Signal(d.fence.Get(), ++d.fenceValue);
		d.WaitFor(d.fenceValue);
		uint8_t *mapped = nullptr;
		if (FAILED(readback->Map(0, nullptr, reinterpret_cast<void **>(&mapped)))) return;
		std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
		for (uint32_t y = 0; y < height; ++y) {
			std::memcpy(rgba.data() + static_cast<size_t>(y) * width * 4,
			            mapped + footprints[0].Offset + static_cast<size_t>(y) * footprints[0].Footprint.RowPitch, width * 4);
		}
		nSCDWire::SavePng((prefix + L"city.png").c_str(), width, height, rgba.data());
		auto const half = [](uint16_t h) {
			uint32_t const sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1F, mantissa = h & 0x3FF;
			if (exponent == 0) return (sign ? -1.0f : 1.0f) * std::ldexp(static_cast<float>(mantissa), -24);
			uint32_t const bits = exponent == 31 ? sign | 0x7F800000u | mantissa << 13 : sign | (exponent + 112) << 23 | mantissa << 13;
			float value;
			std::memcpy(&value, &bits, sizeof(value));
			return value;
		};
		std::vector<float> vectors(static_cast<size_t>(width) * height * 2);
		double sumX = 0.0, sumY = 0.0, longest = 0.0;
		for (uint32_t y = 0; y < height; ++y) {
			auto const *row = reinterpret_cast<uint16_t const *>(mapped + footprints[1].Offset +
			                                                      static_cast<size_t>(y) * footprints[1].Footprint.RowPitch);
			for (uint32_t x = 0; x < width; ++x) {
				float const dx = half(row[x * 2]), dy = half(row[x * 2 + 1]);
				size_t const i = static_cast<size_t>(y) * width + x;
				vectors[i * 2] = dx;
				vectors[i * 2 + 1] = dy;
				sumX += dx;
				sumY += dy;
				longest = (std::max)(longest, std::sqrt(static_cast<double>(dx) * dx + static_cast<double>(dy) * dy));
			}
		}
		readback->Unmap(0, nullptr);
		for (size_t i = 0; i < vectors.size() / 2; ++i) {
			double const dx = vectors[i * 2], dy = vectors[i * 2 + 1];
			double const value = longest > 0.0 ? std::sqrt(dx * dx + dy * dy) / longest : 0.0;
			double const hue = (std::atan2(dy, dx) / (2.0 * 3.14159265358979) + 1.0) * 6.0;
			double const f = hue - std::floor(hue);
			double const channels[6][3] = {{1, f, 0}, {1 - f, 1, 0}, {0, 1, f}, {0, 1 - f, 1}, {f, 0, 1}, {1, 0, 1 - f}};
			double const *const c = channels[static_cast<int>(std::floor(hue)) % 6];
			for (int k = 0; k < 3; ++k) rgba[i * 4 + k] = static_cast<uint8_t>(std::clamp(c[k] * value, 0.0, 1.0) * 255.0);
			rgba[i * 4 + 3] = 255;
		}
		nSCDWire::SavePng((prefix + L"motion.png").c_str(), width, height, rgba.data());
		double const pixels = static_cast<double>(width) * height;
		std::printf("owned: motion of frame %llu: average %.3f, %.3f pixels, longest %.3f, jitter %.3f, %.3f\n",
		            static_cast<unsigned long long>(d.frame), sumX / pixels, sumY / pixels, longest, d.inputs.jitter[0],
		            d.inputs.jitter[1]);
	}

	bool OwnedScene::Capture(cGDriver12::CapturedState const &state, D3D11Vertex const *vertices, uint32_t vertexCount,
	                         uint32_t const *indices, uint32_t indexCount, uint32_t) {
		return impl->CaptureDraw(state, vertices, vertexCount, indices, indexCount);
	}

	bool OwnedScene::TakeMesh(nSCDWire::DrawOwner const &owner, nSCDWire::ModelInstance const &instance,
	                          float const *bounds, uint32_t mesh, D3D12_VERTEX_BUFFER_VIEW const &vertices,
	                          D3D12_INDEX_BUFFER_VIEW const &indices, uint32_t count) {
		Impl &d = *impl;
		if (!d.staticWindow || owner.pass > 2 || !d.captureViewKnown) return false;
		cGDriver12::CapturedState state;
		if (!d.driver.CaptureState(kS3dFormat, state)) return d.owning;
		// A new bracket, or the same drawable drawn again, starts the instance's meshes over.
		if (d.capturing == nullptr || owner.drawable != d.capturingDrawable || mesh <= d.lastMesh) {
			uint64_t const occupant = owner.occupant != 0 ? owner.occupant : owner.drawable;
			ModelKey const key{occupant, instance.group, instance.instance};
			auto [found, added] = d.models.try_emplace(key);
			Model &model = found->second;
			if (added) {
				model.key = key;
				d.modelsByOccupant[occupant].push_back(key);
			}
			WorldMatrix(instance, model.world);
			Extent const before = model.extent;
			model.extent = {};
			for (int corner = 0; corner < 8; ++corner) {
				float const local[3]{bounds[(corner & 1) ? 3 : 0], bounds[(corner & 2) ? 4 : 1], bounds[(corner & 4) ? 5 : 2]};
				float world[3];
				for (int a = 0; a < 3; ++a) {
					world[a] = model.world[a] * local[0] + model.world[4 + a] * local[1] + model.world[8 + a] * local[2] +
					           model.world[12 + a];
				}
				model.extent.Add(d.captureView, world);
			}
			std::memcpy(model.modelView, state.constants + cGDriver12::kModelViewOffset, sizeof(model.modelView));
			model.frame = d.frame;
			model.camera = 0;
			model.orphan = false;
			for (uint32_t group: model.meshes) d.Release(group);
			model.meshes.clear();
			d.modelsThisFrame.push_back(key);
			if (added || std::memcmp(&before, &model.extent, sizeof(before)) != 0) {
				d.Index({0, key.occupant, key.group, key.instance}, model.extent);
			}
			d.capturing = &model;
			d.capturingDrawable = owner.drawable;
		}
		d.lastMesh = mesh;
		uint32_t const group = d.GroupFor(state, d.captureView, &vertices, &indices, count, 2ull << 48 | d.gridRank, kModel << 8 | 1);
		if (d.groups[group].rank == (2ull << 48 | d.gridRank)) ++d.gridRank;
		d.Use(group);
		d.capturing->meshes.push_back(group);
		++d.totals.captured[kModel];
		return true;
	}

	void OwnedScene::PrintTotals() const {
		Impl const &d = *impl;
		Totals const &t = d.totals;
		auto const u = [](uint64_t value) { return static_cast<unsigned long long>(value); };
		std::printf("owned: captured");
		for (uint32_t kind = 0; kind < 5; ++kind) std::printf("%s %llu %s draws", kind ? "," : "", u(t.captured[kind]), kCaptureNames[kind]);
		std::printf(", %llu screen draws dropped, %llu without a view\n", u(t.screenDraws), u(t.uncaptured));
		uint32_t alivePieces = 0, aliveBuckets = 0, aliveGroups = 0;
		for (Piece const &p: d.pieces) alivePieces += p.alive;
		for (Bucket const &b: d.buckets) aliveBuckets += b.alive;
		for (Group const &g: d.groups) aliveGroups += g.alive;
		std::printf("owned: kept %zu models, %zu grid drawables, %zu terrain cells, %zu view pieces: %u pieces in %u buckets, "
		            "%u states, %.1f MB of GPU memory\n",
		            d.models.size(), d.grid.size(), d.cells.size(), d.viewPieces.size(), alivePieces, aliveBuckets,
		            aliveGroups, static_cast<double>(d.pool.Used()) / (1 << 20));
		std::printf("owned: removed %llu models, %llu grid pieces, %llu terrain pieces, %llu view pieces; terrain cells "
		            "kept %llu times, changed %llu times\n",
		            u(t.removed[0]), u(t.removed[1]), u(t.removed[2]), u(t.removed[3]), u(t.cellsKept), u(t.cellsChanged));
		std::printf("owned: %llu frames composited (%llu skipped, %llu new views), %.1f draws and %.1f instances per frame, "
		            "%.1f MB written in place, %llu buckets built again\n",
		            u(t.composites), u(t.skipped), u(t.viewChanges),
		            t.composites ? static_cast<double>(t.draws) / t.composites : 0.0,
		            t.composites ? static_cast<double>(t.instances) / t.composites : 0.0,
		            static_cast<double>(t.appendedBytes) / (1 << 20), u(t.rebuilds));
		auto const report = [](char const *indent, char const *name, std::vector<float> values) {
			if (values.empty()) return;
			std::sort(values.begin(), values.end());
			double total = 0.0;
			for (float value: values) total += value;
			std::printf("owned: %s%s per composite: %.3f ms average, median %.3f, 95th percentile %.3f, slowest %.3f\n",
			            indent, name, total / static_cast<double>(values.size()), values[values.size() / 2],
			            values[values.size() * 95 / 100], values.back());
		};
		if (t.composites != 0) {
			std::printf("owned: CPU per composite by step: removing %.3f ms, buckets %.3f ms, instances %.3f ms, "
			            "drawing %.3f ms\n", t.steps[0] / t.composites, t.steps[1] / t.composites,
			            t.steps[2] / t.composites, t.steps[3] / t.composites);
		}
		report("", "CPU", t.cpuMilliseconds);
		report("", "GPU", t.gpuMilliseconds);
		for (auto const &[millimetres, z]: d.zooms) {
			std::printf("owned:   at %.4f metres per pixel: %llu frames, %.0f draws and %.0f instances per frame\n",
			            millimetres / 1000.0, u(z.frames), static_cast<double>(z.draws) / z.frames,
			            static_cast<double>(z.instances) / z.frames);
			report("    ", "CPU", z.cpuMilliseconds);
			report("    ", "GPU", z.gpuMilliseconds);
		}
	}
}
