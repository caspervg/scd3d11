#pragma once

#include <cstdint>

namespace nSCD3D11 {
	uint32_t TextureSourceComponents(uint32_t format);
	uint32_t TextureSourcePixelBytes(uint32_t format, uint32_t type);
	// Bytes a driver reads from an image the game hands over: compressed sources (formats 7 to 10) in
	// whole 4x4 blocks, the rest a pixel at a time, with rows rowLength pixels apart when it is set.
	// 0 for an empty or unknown image.
	uint32_t SourceImageBytes(uint32_t format, uint32_t type, int32_t width, int32_t height, int32_t rowLength);
	bool ConvertTextureSourcePixel(uint32_t format, uint32_t type, void const *source, uint8_t rgba[4]);
	bool IsValidBlockCompressedUpdate(
		uint32_t mipWidth, uint32_t mipHeight,
		uint32_t x, uint32_t y, uint32_t width, uint32_t height);
}
