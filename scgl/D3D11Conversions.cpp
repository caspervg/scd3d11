/*
 *  SCGL - a free graphics driver for SimCity 4's SimGL interface
 *  Copyright (C) 2025  Nelson Gomez (nsgomez) <nelson@ngomez.me>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "D3D11Conversions.h"
#include "VertexFormatUtils.h"

#include <cstring>

namespace nSCGL
{
	bool IsSupportedVertexFormat(uint32_t format) {
		switch (format) {
		case kGDVertexFormat_V3F_C4UB:
		case kGDVertexFormat_V3F_T2F:
		case kGDVertexFormat_V3F_2T2F:
		case kGDVertexFormat_V3F_C4UB_T2F:
		case kGDVertexFormat_V3F_C4UB_2T2F:
		case kGDVertexFormat_V3F:
		case kGDVertexFormat_V3F_N3F:
		case kGDVertexFormat_V3F_N3F_C4UB:
		case kGDVertexFormat_V3F_N3F_T2F:
		case kGDVertexFormat_V3F_N3F_2T2F:
		case kGDVertexFormat_V3F_N3F_C4UB_T2F:
		case kGDVertexFormat_V3F_N3F_C4UB_2T2F:
			return true;
		default:
			return false;
		}
	}

	bool ConvertVertices(
		uint32_t format,
		uint32_t stride,
		void const* source,
		uint32_t count,
		std::vector<D3D11Vertex>& destination)
	{
		uint32_t const packedStride = RZVertexFormatStride(format);
		if (!IsSupportedVertexFormat(format) || source == nullptr || stride < packedStride || count == 0) {
			return false;
		}

		uint32_t const normalCount = RZVertexFormatNumElements(format, kGDElementType_Normal);
		uint32_t const colorCount = RZVertexFormatNumElements(format, kGDElementType_Color);
		uint32_t const texCoordCount = RZVertexFormatNumElements(format, kGDElementType_TexCoord);
		uint32_t const normalOffset = normalCount ? RZVertexFormatElementOffset(format, kGDElementType_Normal, 0) : 0;
		uint32_t const colorOffset = colorCount ? RZVertexFormatElementOffset(format, kGDElementType_Color, 0) : 0;
		uint32_t const texCoordOffset = texCoordCount ? RZVertexFormatElementOffset(format, kGDElementType_TexCoord, 0) : 0;

		destination.resize(count);
		uint8_t const* input = static_cast<uint8_t const*>(source);
		for (uint32_t index = 0; index < count; ++index, input += stride) {
			D3D11Vertex& vertex = destination[index];
			memcpy(vertex.position, input, sizeof(vertex.position));

			vertex.normal[0] = 0.0f;
			vertex.normal[1] = 0.0f;
			vertex.normal[2] = 1.0f;
			if (normalCount) {
				memcpy(vertex.normal, input + normalOffset, sizeof(vertex.normal));
			}

			vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 0xff;
			if (colorCount) {
				uint8_t const* bgra = input + colorOffset;
				vertex.color[0] = bgra[2];
				vertex.color[1] = bgra[1];
				vertex.color[2] = bgra[0];
				vertex.color[3] = bgra[3];
			}

			memset(vertex.texCoord, 0, sizeof(vertex.texCoord));
			for (uint32_t coordinate = 0; coordinate < texCoordCount && coordinate < 2; ++coordinate) {
				memcpy(vertex.texCoord[coordinate], input + texCoordOffset + coordinate * sizeof(vertex.texCoord[0]), sizeof(vertex.texCoord[0]));
			}
		}
		return true;
	}

	bool BuildSequentialIndices(uint32_t primitive, uint32_t vertexCount, std::vector<uint32_t>& indices) {
		std::vector<uint32_t> source(vertexCount);
		for (uint32_t i = 0; i < vertexCount; ++i) source[i] = i;
		return ConvertPrimitiveIndices(primitive, source, indices);
	}

	bool ConvertPrimitiveIndices(
		uint32_t primitive,
		std::vector<uint32_t> const& source,
		std::vector<uint32_t>& destination)
	{
		uint32_t const vertexCount = static_cast<uint32_t>(source.size());
		destination.clear();
		if (primitive == 2) {
			if (vertexCount < 3) return false;
			destination.reserve((vertexCount - 2) * 3);
			for (uint32_t i = 1; i + 1 < vertexCount; ++i) {
				destination.push_back(source[0]);
				destination.push_back(source[i]);
				destination.push_back(source[i + 1]);
			}
			return true;
		}
		if (primitive == 6) {
			if (vertexCount < 4 || (vertexCount % 4) != 0) return false;
			destination.reserve(vertexCount / 4 * 6);
			for (uint32_t i = 0; i < vertexCount; i += 4) {
				destination.insert(destination.end(), {
					source[i], source[i + 1], source[i + 2],
					source[i], source[i + 2], source[i + 3] });
			}
			return true;
		}
		if (primitive == 7) {
			if (vertexCount < 4 || (vertexCount % 2) != 0) return false;
			destination.reserve((vertexCount - 2) * 3);
			for (uint32_t i = 0; i + 3 < vertexCount; i += 2) {
				destination.insert(destination.end(), {
					source[i], source[i + 1], source[i + 2],
					source[i + 2], source[i + 1], source[i + 3] });
			}
			return true;
		}
		return false;
	}

	D3D11_PRIMITIVE_TOPOLOGY D3D11Topology(uint32_t primitive) {
		static D3D11_PRIMITIVE_TOPOLOGY const topologies[] = {
			D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST,
			D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP,
			D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED,
			D3D11_PRIMITIVE_TOPOLOGY_POINTLIST,
			D3D11_PRIMITIVE_TOPOLOGY_LINELIST,
			D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,
			D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED,
			D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED
		};
		return primitive < sizeof(topologies) / sizeof(topologies[0])
			? topologies[primitive]
			: D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
	}

	DXGI_FORMAT D3D11TextureFormat(uint32_t internalFormat) {
		static DXGI_FORMAT const formats[] = {
			DXGI_FORMAT_B5G6R5_UNORM,
			DXGI_FORMAT_B8G8R8A8_UNORM,
			DXGI_FORMAT_B4G4R4A4_UNORM,
			DXGI_FORMAT_B5G5R5A1_UNORM,
			DXGI_FORMAT_B8G8R8A8_UNORM,
			DXGI_FORMAT_BC1_UNORM,
			DXGI_FORMAT_BC2_UNORM,
			DXGI_FORMAT_BC3_UNORM
		};
		return internalFormat < sizeof(formats) / sizeof(formats[0])
			? formats[internalFormat]
			: DXGI_FORMAT_UNKNOWN;
	}

	uint32_t D3D11TextureRowPitch(DXGI_FORMAT format, uint32_t width) {
		switch (format) {
		case DXGI_FORMAT_BC1_UNORM:
			return ((width + 3) / 4) * 8;
		case DXGI_FORMAT_BC2_UNORM:
		case DXGI_FORMAT_BC3_UNORM:
			return ((width + 3) / 4) * 16;
		case DXGI_FORMAT_B5G6R5_UNORM:
		case DXGI_FORMAT_B4G4R4A4_UNORM:
		case DXGI_FORMAT_B5G5R5A1_UNORM:
			return width * 2;
		case DXGI_FORMAT_B8G8R8A8_UNORM:
			return width * 4;
		default:
			return 0;
		}
	}

	D3D11_COMPARISON_FUNC D3D11Comparison(uint32_t function) {
		static D3D11_COMPARISON_FUNC const functions[] = {
			D3D11_COMPARISON_NEVER,
			D3D11_COMPARISON_LESS,
			D3D11_COMPARISON_EQUAL,
			D3D11_COMPARISON_LESS_EQUAL,
			D3D11_COMPARISON_GREATER,
			D3D11_COMPARISON_NOT_EQUAL,
			D3D11_COMPARISON_GREATER_EQUAL,
			D3D11_COMPARISON_ALWAYS
		};
		return function < sizeof(functions) / sizeof(functions[0])
			? functions[function]
			: static_cast<D3D11_COMPARISON_FUNC>(0);
	}

	D3D11_BLEND D3D11Blend(uint32_t blend) {
		static D3D11_BLEND const blends[] = {
			D3D11_BLEND_ZERO,
			D3D11_BLEND_ONE,
			D3D11_BLEND_SRC_COLOR,
			D3D11_BLEND_INV_SRC_COLOR,
			D3D11_BLEND_SRC_ALPHA,
			D3D11_BLEND_INV_SRC_ALPHA,
			D3D11_BLEND_DEST_ALPHA,
			D3D11_BLEND_INV_DEST_ALPHA,
			D3D11_BLEND_DEST_COLOR,
			D3D11_BLEND_INV_DEST_COLOR,
			D3D11_BLEND_SRC_ALPHA_SAT
		};
		return blend < sizeof(blends) / sizeof(blends[0])
			? blends[blend]
			: static_cast<D3D11_BLEND>(0);
	}

	D3D11_STENCIL_OP D3D11StencilOperation(uint32_t operation) {
		static D3D11_STENCIL_OP const operations[] = {
			D3D11_STENCIL_OP_KEEP,
			D3D11_STENCIL_OP_REPLACE,
			D3D11_STENCIL_OP_INCR_SAT,
			D3D11_STENCIL_OP_DECR_SAT,
			D3D11_STENCIL_OP_INVERT
		};
		return operation < sizeof(operations) / sizeof(operations[0])
			? operations[operation]
			: static_cast<D3D11_STENCIL_OP>(0);
	}
}
