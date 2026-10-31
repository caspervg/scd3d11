/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, see <https://www.gnu.org/licenses/>.
 */

#include "../cGDriver.h"
#include "../Diagnostics.h"
#include "../LiveShadowHooks.h"
#include "../VertexFormatUtils.h"

#include <limits>
#include <vector>

namespace nSCD3D11 {
	char const *cGDriver::GetVertexBufferName(uint32_t gdVertexFormat) {
		if (gdVertexFormat != kGDVertexFormat_V3F_C4UB_2T2F) {
			Log(LogCategory::Unsupported, "vertex-buffer format %u requested", gdVertexFormat);
		}
		// SC4 treats zero as the name of the single terrain vertex buffer.
		return nullptr;
	}

	uint32_t cGDriver::VertexBufferType(uint32_t name) {
		return name == 0 ? kGDVertexFormat_V3F_C4UB_2T2F : UINT32_MAX;
	}

	uint32_t cGDriver::MaxVertices(uint32_t name) {
		return name == 0 ? 32768 : 0;
	}

	uint32_t cGDriver::GetVertices(int32_t name, uint32_t count) {
		uint32_t const capacity = MaxVertices(static_cast<uint32_t>(name));
		uint32_t const stride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
		if (capacity == 0 || count == 0 || count > capacity || extensionVerticesLocked) return 0;
		if (extensionVertexCursor + count > capacity) extensionVertexCursor = 0;
		if (extensionVertexData.size() != static_cast<size_t>(capacity) * stride) {
			extensionVertexData.resize(static_cast<size_t>(capacity) * stride);
		}
		extensionVertexStart = extensionVertexCursor;
		extensionVertexCursor += count;
		extensionVerticesLocked = true;
		return reinterpret_cast<uint32_t>(
			extensionVertexData.data() + static_cast<size_t>(extensionVertexStart) * stride);
	}

	uint32_t cGDriver::ContinueVertices(uint32_t name, uint32_t count) {
		uint32_t const capacity = MaxVertices(name);
		uint32_t const stride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
		if (!extensionVerticesLocked || capacity == 0 || count == 0 || count > capacity - extensionVertexCursor) return 0;
		uint8_t *result = extensionVertexData.data() + static_cast<size_t>(extensionVertexCursor) * stride;
		extensionVertexCursor += count;
		return reinterpret_cast<uint32_t>(result);
	}

	void cGDriver::ReleaseVertices(uint32_t name) {
		if (name == 0) extensionVerticesLocked = false;
	}

	bool cGDriver::ExtensionReservationHolds(uint32_t byteSize) const {
		uint32_t const stride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
		// Draws read only the current reservation, [start, cursor). Reset() empties it, so nothing
		// can be drawn again until the next GetVertices.
		return !extensionVerticesLocked && byteSize != 0 && byteSize % stride == 0 &&
		       byteSize / stride <= extensionVertexCursor - extensionVertexStart;
	}

	// SC4 reserves nearly the whole buffer for every terrain draw, draws about 15 vertices from it once, and
	// never draws them again. Only the vertices a draw reads are streamed: no cache entry could ever hit.
	bool cGDriver::UploadExtensionVertices(uint32_t first, uint32_t count) {
		uint32_t const format = kGDVertexFormat_V3F_C4UB_2T2F;
		uint32_t const stride = RZVertexFormatStride(format);
		uint8_t const *source = extensionVertexData.data() + (static_cast<size_t>(extensionVertexStart) + first) * stride;
		try {
			if (!ConvertVertices(format, stride, source, count, vertexScratch)) return false;
		} catch (std::bad_alloc const &) {
			Log(LogCategory::Resource, "out of memory converting %u terrain vertices", count);
			LogMemoryStatus(LogCategory::Resource, "when terrain vertex conversion failed");
			return false;
		}
		ID3D11Buffer *buffer = nullptr;
		uint32_t offset = 0;
		if (!StreamUpload(terrainVertexStream, TERRAIN_VERTEX_STREAM_BYTES, D3D11_BIND_VERTEX_BUFFER, vertexScratch.data(),
		                  static_cast<uint32_t>(vertexScratch.size() * sizeof(D3D11Vertex)), buffer, offset)) {
			return false;
		}
		dynamicVertexBuffer = buffer;
		dynamicVertexBufferOffset = offset;
		++frameStats.terrainDraws;
		return true;
	}

	void cGDriver::DrawPrims(uint32_t name, uint32_t primitive, void *, uint32_t byteSize) {
		++frameStats.draws;
		++drawDiagnostics.received;
		if (name != 0) return;
		uint32_t const extensionStride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
		// A culled draw still reaches the live shadow bookkeeping below. Only the upload and draw are skipped.
		bool const culled = viewportCullActive && ExtensionReservationHolds(byteSize) &&
		                    DrawOutsideViewport(
			                    primitive, extensionVertexData.data() +
			                               static_cast<size_t>(extensionVertexStart) * extensionStride +
			                               RZVertexFormatElementOffset(kGDVertexFormat_V3F_C4UB_2T2F,
			                                                           kGDElementType_Vertices, 0),
			                    extensionStride, byteSize / extensionStride);
		if (!culled && (!ExtensionReservationHolds(byteSize) || !UploadExtensionVertices(0, byteSize / extensionStride))) {
			return;
		}
		NoteLiveShadowTerrainView();
		// The terrain reservation path never went through MatchesLiveShadowMesh,
		// so bracketed network draws issued here were silently dropped.
		if (LiveShadowHooks::NetworkDrawActive()) {
			static bool loggedBracketPrims = false;
			if (!loggedBracketPrims) {
				loggedBracketPrims = true;
				Log(LogCategory::Initialization,
				    "live bracket draw via DrawPrims (bytes=%u)", byteSize);
			}
			D3D11_PRIMITIVE_TOPOLOGY const topology = D3D11Topology(primitive);
			uint32_t const format = kGDVertexFormat_V3F_C4UB_2T2F;
			uint32_t const stride = RZVertexFormatStride(format);
			uint32_t const count = stride == 0 ? 0 : byteSize / stride;
			if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED && count >= 3 &&
			    byteSize % stride == 0) {
				try {
					uint8_t const *const source = extensionVertexData.data() +
						static_cast<size_t>(extensionVertexStart) * stride;
					std::vector<D3D11Vertex> vertices;
					std::vector<uint32_t> liveIndices(count);
					if (ConvertVertices(format, stride, source, count, vertices)) {
						for (uint32_t index = 0; index < count; ++index) liveIndices[index] = index;
						AppendLiveShadowDraw(std::move(vertices), std::move(liveIndices), topology, true);
					}
				} catch (std::bad_alloc const &) {
				}
			}
		}
		if (culled) return;
		uint32_t const previousFormat = interleavedFormat;
		interleavedFormat = kGDVertexFormat_V3F_C4UB_2T2F;
		if (BindGeometryPipeline(primitive)) {
			d3dContext->Draw(byteSize / RZVertexFormatStride(interleavedFormat), FirstVertex());
		}
		interleavedFormat = previousFormat;
	}

	void cGDriver::DrawPrimsIndexed(
		uint32_t name, uint32_t primitive, uint32_t count, uint16_t *indices) {
		++frameStats.draws;
		++drawDiagnostics.received;
		uint64_t const indexBytes = static_cast<uint64_t>(count) * sizeof(uint16_t);
		if (name != 0 || count == 0 || indices == nullptr || indexBytes > UINT32_MAX) {
			return;
		}
		uint16_t minimumIndex = UINT16_MAX;
		uint16_t maximumIndex = 0;
		for (uint32_t i = 0; i < count; ++i) {
			if (indices[i] < minimumIndex) minimumIndex = indices[i];
			if (indices[i] > maximumIndex) maximumIndex = indices[i];
		}
		uint32_t const extensionStride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
		uint32_t const byteSize = (static_cast<uint32_t>(maximumIndex) + 1) * extensionStride;
		// Tests the index range, which holds every referenced vertex. Live shadows still see a culled draw.
		bool const culled = viewportCullActive && ExtensionReservationHolds(byteSize) &&
		                    DrawOutsideViewport(
			                    primitive, extensionVertexData.data() +
			                               (static_cast<size_t>(extensionVertexStart) + minimumIndex) * extensionStride +
			                               RZVertexFormatElementOffset(kGDVertexFormat_V3F_C4UB_2T2F,
			                                                           kGDElementType_Vertices, 0),
			                    extensionStride, static_cast<uint32_t>(maximumIndex - minimumIndex) + 1);
		uint32_t const drawnVertices = static_cast<uint32_t>(maximumIndex - minimumIndex) + 1;
		if (!culled && (!ExtensionReservationHolds(byteSize) || !UploadExtensionVertices(minimumIndex, drawnVertices))) {
			return;
		}
		NoteLiveShadowTerrainView();
		if (!culled && !UploadCachedBuffer(
			indexBufferSegments, activeIndexBufferSegment, indexBufferCache,
			IndexCacheKey(DXGI_FORMAT_R16_UINT, indices, count),
			static_cast<uint32_t>(indexBytes), D3D11_BIND_INDEX_BUFFER, indices,
			dynamicIndexBuffer, dynamicIndexBufferOffset)) {
			return;
		}
		if (LiveShadowHooks::NetworkDrawActive()) {
			static bool loggedBracketPrimsIndexed = false;
			if (!loggedBracketPrimsIndexed) {
				loggedBracketPrimsIndexed = true;
				Log(LogCategory::Initialization,
				    "live bracket draw via DrawPrimsIndexed (count=%u)", count);
			}
			D3D11_PRIMITIVE_TOPOLOGY const topology = D3D11Topology(primitive);
			uint32_t const format = kGDVertexFormat_V3F_C4UB_2T2F;
			uint32_t const stride = RZVertexFormatStride(format);
			uint32_t const vertexCount = static_cast<uint32_t>(maximumIndex) + 1;
			if (topology != D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED && count >= 3 && vertexCount >= 3) {
				try {
					uint8_t const *const source = extensionVertexData.data() +
						static_cast<size_t>(extensionVertexStart) * stride;
					std::vector<D3D11Vertex> vertices;
					if (ConvertVertices(format, stride, source, vertexCount, vertices)) {
						std::vector<uint32_t> liveIndices(indices, indices + count);
						AppendLiveShadowDraw(std::move(vertices), std::move(liveIndices), topology, true);
					}
				} catch (std::bad_alloc const &) {
				}
			}
		}
		if (culled) return;
		uint32_t const previousFormat = interleavedFormat;
		interleavedFormat = kGDVertexFormat_V3F_C4UB_2T2F;
		if (BindGeometryPipeline(primitive)) {
			UINT const firstIndex = BindIndexBuffer(DXGI_FORMAT_R16_UINT);
			// The upload starts at minimumIndex, so the unchanged (and cached) indices are offset back to it.
			d3dContext->DrawIndexed(count, firstIndex, static_cast<INT>(FirstVertex()) - minimumIndex);
		}
		interleavedFormat = previousFormat;
	}

	void cGDriver::Reset(void) {
		extensionVertexCursor = extensionVertexStart = 0;
		extensionVerticesLocked = false;
	}
}
