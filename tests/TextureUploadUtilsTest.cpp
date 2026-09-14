#include "TextureUploadUtils.h"

#include <cassert>
#include <cstdint>
#include <limits>

int main() {
	uint8_t rgba[4]{};
	uint8_t bgra[] = {10, 20, 30, 40};
	assert(nSCD3D11::TextureSourcePixelBytes(3, 1) == 4);
	assert(nSCD3D11::ConvertTextureSourcePixel(3, 1, bgra, rgba));
	assert(rgba[0] == 30 && rgba[1] == 20 && rgba[2] == 10 && rgba[3] == 40);

	float luminanceAlpha[] = {0.5f, 1.0f};
	assert(nSCD3D11::ConvertTextureSourcePixel(6, 6, luminanceAlpha, rgba));
	assert(rgba[0] == 128 && rgba[1] == 128 && rgba[2] == 128 && rgba[3] == 255);
	int8_t signedByte = (std::numeric_limits<int8_t>::max)();
	int16_t signedShort = (std::numeric_limits<int16_t>::max)();
	uint16_t unsignedShort = (std::numeric_limits<uint16_t>::max)();
	int32_t signedInt = (std::numeric_limits<int32_t>::max)();
	uint32_t unsignedInt = (std::numeric_limits<uint32_t>::max)();
	double half = 0.5;
	assert(nSCD3D11::ConvertTextureSourcePixel(5, 0, &signedByte, rgba) && rgba[0] == 255);
	assert(nSCD3D11::ConvertTextureSourcePixel(5, 2, &signedShort, rgba) && rgba[0] == 255);
	assert(nSCD3D11::ConvertTextureSourcePixel(5, 3, &unsignedShort, rgba) && rgba[0] == 255);
	assert(nSCD3D11::ConvertTextureSourcePixel(5, 4, &signedInt, rgba) && rgba[0] == 255);
	assert(nSCD3D11::ConvertTextureSourcePixel(5, 5, &unsignedInt, rgba) && rgba[0] == 255);
	assert(nSCD3D11::ConvertTextureSourcePixel(5, 7, &half, rgba) && rgba[0] == 128);

	uint16_t bgra4444Rev = 0xa123;
	assert(nSCD3D11::ConvertTextureSourcePixel(3, 13, &bgra4444Rev, rgba));
	assert(rgba[0] == 0x11 && rgba[1] == 0x22 && rgba[2] == 0x33 && rgba[3] == 0xaa);
	uint16_t rgba5551 = 0xf801;
	assert(nSCD3D11::ConvertTextureSourcePixel(1, 9, &rgba5551, rgba));
	assert(rgba[0] == 255 && rgba[1] == 0 && rgba[2] == 0 && rgba[3] == 255);
	uint32_t rgba8888 = 0x10203040;
	assert(nSCD3D11::ConvertTextureSourcePixel(1, 12, &rgba8888, rgba));
	assert(rgba[0] == 0x10 && rgba[1] == 0x20 && rgba[2] == 0x30 && rgba[3] == 0x40);
	assert(nSCD3D11::TextureSourcePixelBytes(0, 13) == 0);
	assert(nSCD3D11::TextureSourcePixelBytes(1, 10) == 0);

	assert(nSCD3D11::IsValidBlockCompressedUpdate(10, 10, 0, 0, 8, 8));
	assert(nSCD3D11::IsValidBlockCompressedUpdate(10, 10, 8, 8, 2, 2));
	assert(!nSCD3D11::IsValidBlockCompressedUpdate(10, 10, 4, 4, 5, 4));
	assert(!nSCD3D11::IsValidBlockCompressedUpdate(10, 10, 2, 0, 4, 4));
	// Right/bottom edge blocks of a mip whose size is not a multiple of four.
	assert(nSCD3D11::IsValidBlockCompressedUpdate(10, 10, 8, 0, 2, 4));
	assert(!nSCD3D11::IsValidBlockCompressedUpdate(10, 10, 8, 0, 3, 4));
	// Offsets that wrap x + width back into range.
	assert(!nSCD3D11::IsValidBlockCompressedUpdate(16, 16, 0xFFFFFFFCu, 0, 8, 4));
	assert(!nSCD3D11::IsValidBlockCompressedUpdate(16, 16, 0, 0xFFFFFFFCu, 4, 8));
	assert(!nSCD3D11::IsValidBlockCompressedUpdate(16, 16, 4, 0, UINT32_MAX, 4));
	return 0;
}
