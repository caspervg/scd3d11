#pragma once

#include <cstdint>

namespace nSCGL {
	uint32_t TextureSourceComponents(uint32_t format);
	uint32_t TextureSourcePixelBytes(uint32_t format, uint32_t type);
	bool ConvertTextureSourcePixel(uint32_t format, uint32_t type, void const *source, uint8_t rgba[4]);
	bool IsValidBlockCompressedUpdate(
		uint32_t mipWidth, uint32_t mipHeight,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height);
}
