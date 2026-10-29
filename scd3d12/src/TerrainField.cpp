/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

// What SC4's terrain draws look like (Big City Tutorial, 1.1.641), as Learn tells them apart:
//
//   base      pass 0, no texture, no blending: every cell, in a dark colour of its own
//   texture   pass 0, stage 0 textured from the vertex coordinates, added (source alpha, one): the cells whose
//             corners use the texture, the vertex alpha being its weight there
//   water     pass 2, stage 0 textured from the vertex coordinates, blended (source alpha, inverse source alpha)
//             with depth test less: the water surface, its alpha the water's opacity
//
// Each is drawn from one vertex buffer and one index buffer that holds every cell SC4 did not draw.

#include "TerrainField.h"

#include "D3D11Conversions.h"
#include "SceneLane.h"
#include "assets/Textures.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>

using Microsoft::WRL::ComPtr;
using nSCD3D11::D3D11Vertex;

namespace nSCD3D12 {
	namespace {
		constexpr uint32_t kTerrainTextureGroup = 0x891B0E1A;
		constexpr uint32_t kWaterTexture = 0x09187300;
		// Frames the GPU may still be reading a buffer that was replaced.
		constexpr uint64_t kFramesInFlight = 4;
		enum Kind : uint32_t { kBase, kTexture, kWater, kKinds };

		// A default heap buffer filled from the driver's upload space, in place while it is big enough.
		struct Buffer {
			ComPtr<ID3D12Resource> resource;
			uint64_t capacity = 0;
			D3D12_RESOURCE_STATES readable{};
		};
	}

	struct TerrainField::Impl {
		cGDriver12 &driver;
		SceneLane &lane;

		nSCDWire::TerrainField field{};
		std::vector<nSCDWire::TerrainVertex> vertices;
		bool fieldDirty = false;

		struct Template {
			bool known = false;
			cGDriver12::CapturedState state{};
		};
		Template templates[kKinds];
		uint8_t baseColor[4]{0, 0, 0, 255};
		// Whether SC4's terrain triangles turn counter-clockwise seen from above.
		bool upCounterClockwise = true;
		bool windingKnown = false;
		bool templatesDirty = false;

		// One texture's cells: its vertices sit in the shared buffer, each cell names its six indices.
		struct Mesh {
			Kind kind = kBase;
			uint32_t pin = 0;
			std::vector<uint32_t> cells;
			std::vector<uint32_t> indices;
			uint32_t firstIndex = 0, indexCount = 0;
		};
		std::vector<Mesh> meshes;
		std::map<uint32_t, uint32_t> pins;
		Buffer vertexBuffer, indexBuffer;
		std::vector<std::pair<ComPtr<ID3D12Resource>, uint64_t> > retired;
		uint64_t frame = 0;
		uint64_t keptVersion = UINT64_MAX;
		bool built = false;

		Impl(cGDriver12 &driver, SceneLane &lane) : driver(driver), lane(lane) {}

		~Impl() {
			for (auto const &[name, pin]: pins) driver.UnpinTexture(pin);
		}

		uint32_t CellsX() const { return field.verticesX > 1 ? field.verticesX - 1 : 0; }
		uint32_t CellsZ() const { return field.verticesZ > 1 ? field.verticesZ - 1 : 0; }

		uint32_t Pin(uint32_t name) {
			if (name == 0) return 0;
			auto const found = pins.find(name);
			if (found != pins.end()) return found->second;
			uint32_t const pin = driver.PinTexture(name);
			pins.emplace(name, pin);
			return pin;
		}

		// A terrain texture as SetUpTexturesForCurrentTerrainTypeAndZoom (0x007571D0) loads it: zoom x 0x100 + id,
		// else the id, the image from bits 12 to 15.
		uint32_t TerrainTexture(uint16_t id) {
			uint32_t const number = id & 0x0FFFu, element = id >> 12 & 15u;
			uint32_t name = lane.StoreTexture(kTerrainTextureGroup, field.zoom * 0x100 + number, element);
			if (name == 0) name = lane.StoreTexture(kTerrainTextureGroup, number, element);
			return name;
		}

		bool Fill(Buffer &buffer, void const *data, uint64_t bytes, D3D12_RESOURCE_STATES readable) {
			ID3D12GraphicsCommandList *const list = driver.CommandList();
			if (bytes == 0 || list == nullptr) return false;
			if (buffer.capacity < bytes) {
				if (buffer.resource) retired.emplace_back(std::move(buffer.resource), frame);
				buffer.capacity = bytes + bytes / 4;
				D3D12_HEAP_PROPERTIES const heap{D3D12_HEAP_TYPE_DEFAULT};
				D3D12_RESOURCE_DESC description{};
				description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
				description.Width = buffer.capacity;
				description.Height = description.DepthOrArraySize = description.MipLevels = 1;
				description.SampleDesc.Count = 1;
				description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
				if (FAILED(driver.Device()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &description,
				                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
				                                                    IID_PPV_ARGS(&buffer.resource)))) {
					buffer.capacity = 0;
					return false;
				}
			} else {
				D3D12_RESOURCE_BARRIER barrier{};
				barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
				barrier.Transition = {buffer.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, readable,
				                      D3D12_RESOURCE_STATE_COPY_DEST};
				list->ResourceBarrier(1, &barrier);
			}
			ID3D12Resource *source = nullptr;
			uint64_t offset = 0;
			bool const uploaded = driver.Upload(data, bytes, source, offset);
			if (uploaded) list->CopyBufferRegion(buffer.resource.Get(), 0, source, offset, bytes);
			D3D12_RESOURCE_BARRIER barrier{};
			barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			barrier.Transition = {buffer.resource.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
			                      D3D12_RESOURCE_STATE_COPY_DEST, readable};
			list->ResourceBarrier(1, &barrier);
			buffer.readable = readable;
			return uploaded;
		}

		static void Color(uint32_t bgra, uint8_t *rgba) {
			rgba[0] = static_cast<uint8_t>(bgra >> 16);
			rgba[1] = static_cast<uint8_t>(bgra >> 8);
			rgba[2] = static_cast<uint8_t>(bgra);
			rgba[3] = static_cast<uint8_t>(bgra >> 24);
		}

		static uint8_t Weight(nSCDWire::TerrainVertex const &v, uint16_t texture) {
			for (int k = 0; k < 3 && v.texture[k] != 0; ++k) {
				if (v.texture[k] == texture) return v.weight[k];
			}
			return 0;
		}

		// Every mesh's vertices, and the indices of each of its cells.
		void Build() {
			auto const start = std::chrono::steady_clock::now();
			uint32_t const columns = field.verticesX, cellsX = CellsX(), cellsZ = CellsZ();
			meshes.clear();
			std::vector<D3D11Vertex> out;
			std::vector<uint32_t> remap(vertices.size());
			auto const vertex = [&](uint32_t index, bool water) {
				nSCDWire::TerrainVertex const &v = vertices[index];
				float const x = static_cast<float>(index % columns) * field.cellWidth;
				float const z = static_cast<float>(index / columns) * field.cellWidth;
				D3D11Vertex &o = out.emplace_back();
				o.position[0] = x;
				o.position[1] = water ? v.water : v.height;
				o.position[2] = z;
				o.normal[0] = o.normal[1] = 0.0f;
				o.normal[2] = 1.0f;
				o.texCoord[0][0] = field.uv[0] * x + field.uv[1] * z + field.uv[2];
				o.texCoord[0][1] = field.uv[3] * x + field.uv[4] * z + field.uv[5];
				o.texCoord[1][0] = o.texCoord[1][1] = 0.0f;
				return &o;
			};
			// The two triangles of a cell, as corners 0 (x, z), 1 (x + 1, z), 2 (x, z + 1), 3 (x + 1, z + 1). Both
			// turn clockwise seen from above (y up), and the other way round when SC4's do.
			static constexpr uint8_t kSplit[2][6]{{0, 1, 3, 0, 3, 2}, {0, 1, 2, 1, 3, 2}};
			auto const add = [&](Mesh &mesh, uint32_t cell, uint32_t const *corners) {
				uint8_t const *const split = kSplit[vertices[corners[0]].flags & 1];
				mesh.cells.push_back(cell);
				for (int t = 0; t < 2; ++t) {
					uint32_t const a = remap[corners[split[t * 3]]], b = remap[corners[split[t * 3 + 1]]],
					               c = remap[corners[split[t * 3 + 2]]];
					if (upCounterClockwise) mesh.indices.insert(mesh.indices.end(), {a, c, b});
					else mesh.indices.insert(mesh.indices.end(), {a, b, c});
				}
			};
			auto const corners = [&](uint32_t x, uint32_t z, uint32_t *out4) {
				uint32_t const first = z * columns + x;
				out4[0] = first, out4[1] = first + 1, out4[2] = first + columns, out4[3] = first + columns + 1;
			};

			// The base, every cell.
			if (templates[kBase].known) {
				Mesh &mesh = meshes.emplace_back();
				mesh.kind = kBase;
				for (uint32_t i = 0; i < vertices.size(); ++i) {
					remap[i] = static_cast<uint32_t>(out.size());
					std::memcpy(vertex(i, false)->color, baseColor, 4);
				}
				for (uint32_t z = 0; z < cellsZ; ++z) {
					for (uint32_t x = 0; x < cellsX; ++x) {
						uint32_t c[4];
						corners(x, z, c);
						add(mesh, z * cellsX + x, c);
					}
				}
			}

			// One mesh per texture, over the cells where a corner uses it.
			if (templates[kTexture].known) {
				std::vector<uint16_t> textures;
				for (nSCDWire::TerrainVertex const &v: vertices) {
					for (int k = 0; k < 3 && v.texture[k] != 0; ++k) textures.push_back(v.texture[k]);
				}
				std::sort(textures.begin(), textures.end());
				textures.erase(std::unique(textures.begin(), textures.end()), textures.end());
				for (uint16_t const texture: textures) {
					uint32_t const name = TerrainTexture(texture);
					if (name == 0) continue;
					Mesh &mesh = meshes.emplace_back();
					mesh.kind = kTexture;
					mesh.pin = Pin(name);
					std::fill(remap.begin(), remap.end(), UINT32_MAX);
					for (uint32_t z = 0; z < cellsZ; ++z) {
						for (uint32_t x = 0; x < cellsX; ++x) {
							uint32_t c[4];
							corners(x, z, c);
							bool used = false;
							for (uint32_t corner: c) used = used || Weight(vertices[corner], texture) != 0;
							if (!used) continue;
							for (uint32_t corner: c) {
								if (remap[corner] != UINT32_MAX) continue;
								remap[corner] = static_cast<uint32_t>(out.size());
								D3D11Vertex *const o = vertex(corner, false);
								Color(vertices[corner].color, o->color);
								o->color[3] = Weight(vertices[corner], texture);
							}
							add(mesh, z * cellsX + x, c);
						}
					}
					if (mesh.cells.empty()) meshes.pop_back();
				}
			}

			// The water, over the cells where it lies above a corner.
			if (templates[kWater].known) {
				uint32_t const name = lane.StoreTexture(kTerrainTextureGroup, kWaterTexture + field.zoom, 0);
				Mesh &mesh = meshes.emplace_back();
				mesh.kind = kWater;
				mesh.pin = Pin(name);
				std::fill(remap.begin(), remap.end(), UINT32_MAX);
				for (uint32_t z = 0; z < cellsZ; ++z) {
					for (uint32_t x = 0; x < cellsX; ++x) {
						uint32_t c[4];
						corners(x, z, c);
						bool wet = false;
						for (uint32_t corner: c) wet = wet || vertices[corner].water > vertices[corner].height;
						if (!wet) continue;
						for (uint32_t corner: c) {
							if (remap[corner] != UINT32_MAX) continue;
							remap[corner] = static_cast<uint32_t>(out.size());
							Color(vertices[corner].waterColor, vertex(corner, true)->color);
						}
						add(mesh, z * cellsX + x, c);
					}
				}
				if (mesh.cells.empty() || name == 0) meshes.pop_back();
			}
			built = !meshes.empty() && Fill(vertexBuffer, out.data(), out.size() * sizeof(D3D11Vertex),
			                                 D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
			std::printf("terrain field: %ux%u cells, %zu meshes, %zu vertices (%.1f MB), built in %.1f ms\n", cellsX, cellsZ,
			            meshes.size(), out.size(), static_cast<double>(out.size() * sizeof(D3D11Vertex)) / (1 << 20),
			            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
		}

		// The indices of every cell SC4 did not draw, mesh after mesh.
		void Index(std::vector<uint8_t> const &kept) {
			std::vector<uint32_t> all;
			for (Mesh &mesh: meshes) {
				mesh.firstIndex = static_cast<uint32_t>(all.size());
				for (size_t i = 0; i < mesh.cells.size(); ++i) {
					uint32_t const cell = mesh.cells[i];
					// A cell with SC4's base alone still gets the textures and the water over it.
					uint8_t const held = cell < kept.size() ? kept[cell] : 0;
					if (held == 2 || (held == 1 && mesh.kind == kBase)) continue;
					all.insert(all.end(), mesh.indices.begin() + i * 6, mesh.indices.begin() + i * 6 + 6);
				}
				mesh.indexCount = static_cast<uint32_t>(all.size()) - mesh.firstIndex;
			}
			if (all.empty()) all.push_back(0);
			Fill(indexBuffer, all.data(), all.size() * sizeof(uint32_t), D3D12_RESOURCE_STATE_INDEX_BUFFER);
		}
	};

	TerrainField::TerrainField(cGDriver12 &driver, SceneLane &lane) : impl(std::make_unique<Impl>(driver, lane)) {}

	TerrainField::~TerrainField() = default;

	void TerrainField::Set(nSCDWire::TerrainField const &field, nSCDWire::TerrainVertex const *vertices) {
		Impl &d = *impl;
		d.field = field;
		d.vertices.assign(vertices, vertices + static_cast<size_t>(field.verticesX) * field.verticesZ);
		d.fieldDirty = true;
	}

	bool TerrainField::Learn(cGDriver12::CapturedState const &state, uint32_t pass, D3D11Vertex const *vertices,
	                         uint32_t const *indices, uint32_t indexCount) {
		Impl &d = *impl;
		cGDriver12::DrawState draw;
		d.driver.SaveDrawState(draw);
		cGDriver12::DrawState::Stage const &stage = draw.stages[0];
		bool const textured = stage.enabled && stage.coordinates == 0;
		Kind kind;
		if (pass == 0 && !stage.enabled && !draw.blend) kind = kBase;
		else if (pass == 0 && textured && draw.blend && draw.destinationBlend == 1) kind = kTexture;
		else if (pass == 2 && textured && draw.blend && draw.destinationBlend == 5) kind = kWater;
		else return false;
		Impl::Template &slot = d.templates[kind];
		if (!slot.known) d.templatesDirty = true;
		slot.known = true;
		slot.state = state;
		if (kind == kBase && indexCount != 0) std::memcpy(d.baseColor, vertices[indices[0]].color, 4);
		if (!d.windingKnown) {
			for (uint32_t i = 0; i + 2 < indexCount; i += 3) {
				float const *const a = vertices[indices[i]].position, *const b = vertices[indices[i + 1]].position,
				            *const c = vertices[indices[i + 2]].position;
				float const up = (b[2] - a[2]) * (c[0] - a[0]) - (b[0] - a[0]) * (c[2] - a[2]);
				if (std::fabs(up) < 1.0f) continue;
				d.upCounterClockwise = up > 0.0f;
				d.windingKnown = true;
				d.templatesDirty = true;
				break;
			}
		}
		return kind == kTexture;
	}

	uint32_t TerrainField::CellsX() const { return impl->CellsX(); }
	uint32_t TerrainField::CellsZ() const { return impl->CellsZ(); }

	bool TerrainField::Prepare(std::vector<uint8_t> const &kept, uint64_t version) {
		Impl &d = *impl;
		++d.frame;
		while (!d.retired.empty() && d.retired.front().second + kFramesInFlight <= d.frame) d.retired.erase(d.retired.begin());
		if (d.vertices.empty() || !d.windingKnown) return false;
		if (d.fieldDirty || d.templatesDirty) {
			d.fieldDirty = d.templatesDirty = false;
			cGDriver12::DrawState saved;
			d.driver.SaveDrawState(saved);
			d.Build();
			d.driver.RestoreDrawState(saved);
			d.keptVersion = UINT64_MAX;
		}
		if (!d.built) return false;
		if (version != d.keptVersion) {
			d.keptVersion = version;
			d.Index(kept);
		}
		return d.indexBuffer.resource != nullptr;
	}

	void TerrainField::Draw(D3D12_VERTEX_BUFFER_VIEW const &view) {
		Impl &d = *impl;
		if (!d.built || !d.indexBuffer.resource) return;
		D3D12_GPU_VIRTUAL_ADDRESS constants[kKinds]{};
		for (Impl::Mesh const &mesh: d.meshes) {
			if (mesh.indexCount == 0) continue;
			cGDriver12::CapturedState const &state = d.templates[mesh.kind].state;
			if (constants[mesh.kind] == 0) {
				ID3D12Resource *resource = nullptr;
				uint64_t offset = 0;
				if (!d.driver.Upload(state.constants, cGDriver12::kConstantBytes, resource, offset,
				                     D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT)) return;
				constants[mesh.kind] = resource->GetGPUVirtualAddress() + offset;
			}
			cGDriver12::RetainedDraw draw{};
			draw.pipeline = state.pipeline;
			draw.depthBias = state.depthBias;
			draw.stencilReference = state.stencilReference;
			draw.pins[0] = mesh.pin;
			draw.samplers[0] = state.samplers[0];
			draw.samplers[1] = state.samplers[1];
			draw.constants = constants[mesh.kind];
			draw.vertices = {d.vertexBuffer.resource->GetGPUVirtualAddress(), static_cast<UINT>(d.vertexBuffer.capacity),
			                 sizeof(D3D11Vertex)};
			draw.indices = {d.indexBuffer.resource->GetGPUVirtualAddress(), static_cast<UINT>(d.indexBuffer.capacity),
			                DXGI_FORMAT_R32_UINT};
			draw.indexCount = mesh.indexCount;
			draw.startIndex = mesh.firstIndex;
			draw.instances = view;
			d.driver.DrawRetained(draw);
		}
	}

	void TerrainField::VideoMode() {
		Impl &d = *impl;
		d.pins.clear();
		for (Impl::Template &slot: d.templates) slot.known = false;
		d.meshes.clear();
		d.built = false;
	}
}
