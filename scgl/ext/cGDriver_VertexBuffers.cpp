/*
 *  SCGL - a free OpenGL driver for SimCity 4's SimGL interface
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
#include "../VertexFormatUtils.h"

#include <limits>

namespace nSCGL {
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
		if (!extensionVerticesLocked || count == 0 || extensionVertexCursor + count > capacity) return 0;
		uint8_t *result = extensionVertexData.data() + static_cast<size_t>(extensionVertexCursor) * stride;
		extensionVertexCursor += count;
		return reinterpret_cast<uint32_t>(result);
	}

	void cGDriver::ReleaseVertices(uint32_t name) {
		if (name == 0) extensionVerticesLocked = false;
	}

	bool cGDriver::UploadExtensionVertices(uint32_t byteSize) {
		uint32_t const format = kGDVertexFormat_V3F_C4UB_2T2F;
		uint32_t const stride = RZVertexFormatStride(format);
		size_t const offset = static_cast<size_t>(extensionVertexStart) * stride;
		if (extensionVerticesLocked || byteSize == 0 || byteSize % stride != 0 ||
		    offset + byteSize > extensionVertexData.size()) {
			return false;
		}
		uint8_t const *source = extensionVertexData.data() + offset;
		uint64_t key = HashBytes(&format, sizeof(format));
		key = HashBytes(source, byteSize, key);
		if (UseCachedBuffer(vertexBufferSegments, vertexBufferCache, key,
		                    dynamicVertexBuffer, dynamicVertexBufferOffset, D3D11_BIND_VERTEX_BUFFER)) return true;
		if (!ConvertVertices(format, stride, source, byteSize / stride, vertexScratch)) return false;
		return UploadCachedBuffer(
			vertexBufferSegments, activeVertexBufferSegment, vertexBufferCache,
			key, static_cast<uint32_t>(vertexScratch.size() * sizeof(D3D11Vertex)),
			D3D11_BIND_VERTEX_BUFFER, vertexScratch.data(), dynamicVertexBuffer, dynamicVertexBufferOffset);
	}

	void cGDriver::DrawPrims(uint32_t name, uint32_t primitive, void *, uint32_t byteSize) {
		if (name != 0 || !UploadExtensionVertices(byteSize)) return;
		uint32_t const previousFormat = interleavedFormat;
		interleavedFormat = kGDVertexFormat_V3F_C4UB_2T2F;
		if (BindGeometryPipeline(primitive)) {
			d3dContext->Draw(byteSize / RZVertexFormatStride(interleavedFormat), 0);
		}
		interleavedFormat = previousFormat;
	}

	void cGDriver::DrawPrimsIndexed(
		uint32_t name, uint32_t primitive, uint32_t count, uint16_t *indices) {
		uint64_t const indexBytes = static_cast<uint64_t>(count) * sizeof(uint16_t);
		if (name != 0 || count == 0 || indices == nullptr || indexBytes > UINT32_MAX) {
			return;
		}
		uint16_t maximumIndex = 0;
		for (uint32_t i = 0; i < count; ++i) {
			if (indices[i] > maximumIndex) maximumIndex = indices[i];
		}
		uint32_t const byteSize =
			(static_cast<uint32_t>(maximumIndex) + 1) * RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
		if (!UploadExtensionVertices(byteSize)) return;
		uint32_t const indexType = 3;
		uint64_t key = HashBytes(&indexType, sizeof(indexType));
		key = HashBytes(indices, static_cast<size_t>(indexBytes), key);
		if (!UploadCachedBuffer(
			indexBufferSegments, activeIndexBufferSegment, indexBufferCache,
			key, static_cast<uint32_t>(indexBytes), D3D11_BIND_INDEX_BUFFER, indices,
			dynamicIndexBuffer, dynamicIndexBufferOffset)) {
			return;
		}
		uint32_t const previousFormat = interleavedFormat;
		interleavedFormat = kGDVertexFormat_V3F_C4UB_2T2F;
		if (BindGeometryPipeline(primitive)) {
			d3dContext->IASetIndexBuffer(
				dynamicIndexBuffer.Get(), DXGI_FORMAT_R16_UINT, dynamicIndexBufferOffset);
			d3dContext->DrawIndexed(count, 0, 0);
		}
		interleavedFormat = previousFormat;
	}

	void cGDriver::Reset(void) {
		extensionVertexCursor = extensionVertexStart = 0;
		extensionVerticesLocked = false;
	}
}
