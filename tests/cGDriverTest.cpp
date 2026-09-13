#include "cGDriver.h"
#include "SCD3D11Service.h"
#include "VertexFormatUtils.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace nSCD3D11 {
	// Befriended by cGDriver so the checks can reach driver state and D3D11 resources directly.
	struct cGDriverTestAccess {
		cGDriver &d;

		// Returns a subresource's texels with rows packed tightly.
		std::vector<uint8_t> Read(ID3D11Texture2D *texture, UINT level, uint32_t bytesPerPixel) {
			D3D11_TEXTURE2D_DESC description{};
			texture->GetDesc(&description);
			UINT const width = D3D11MipDimension(description.Width, level);
			UINT const height = D3D11MipDimension(description.Height, level);
			description.Width = width;
			description.Height = height;
			description.MipLevels = 1;
			description.Usage = D3D11_USAGE_STAGING;
			description.BindFlags = 0;
			description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			description.MiscFlags = 0;
			ComPtr<ID3D11Texture2D> staging;
			assert(SUCCEEDED(d.d3dDevice->CreateTexture2D(&description, nullptr, &staging)));
			d.d3dContext->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, level, nullptr);
			D3D11_MAPPED_SUBRESOURCE mapping{};
			assert(SUCCEEDED(d.d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
			std::vector<uint8_t> texels(static_cast<size_t>(width) * height * bytesPerPixel);
			for (UINT y = 0; y < height; ++y) {
				memcpy(texels.data() + static_cast<size_t>(y) * width * bytesPerPixel,
				       static_cast<uint8_t const *>(mapping.pData) + static_cast<size_t>(y) * mapping.RowPitch,
				       static_cast<size_t>(width) * bytesPerPixel);
			}
			d.d3dContext->Unmap(staging.Get(), 0);
			return texels;
		}

		// RGBA of the back buffer pixel at top-left coordinates.
		uint32_t Pixel(int x, int y) {
			std::vector<uint8_t> const pixels = Read(d.backBufferTexture.Get(), 0, 4);
			uint8_t const *p = pixels.data() + (static_cast<size_t>(y) * d.windowWidth + x) * 4;
			return static_cast<uint32_t>(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3];
		}

		struct ColorVertex {
			float position[3];
			uint8_t bgra[4];
		};

		// A clip-space triangle covering the whole viewport.
		void DrawFullscreen(uint8_t r, uint8_t g, uint8_t b) {
			ColorVertex const vertices[3]{
				{{-1.0f, -1.0f, 0.0f}, {b, g, r, 255}},
				{{3.0f, -1.0f, 0.0f}, {b, g, r, 255}},
				{{-1.0f, 3.0f, 0.0f}, {b, g, r, 255}},
			};
			d.SetTexture(0, 0);
			d.InterleavedArrays(kGDVertexFormat_V3F_C4UB, 0, vertices);
			d.DrawArrays(0, 0, 3);
		}

		void DrawsGeometry() {
			d.ClearColor(0.0f, 0.0f, 1.0f, 1.0f);
			d.Clear(0x4000);
			assert(Pixel(1, 1) == 0x0000ffff);
			DrawFullscreen(255, 0, 0);
			assert(Pixel(1, 1) == 0xff0000ff);
		}

		std::vector<uint8_t> ReadBuffer(ID3D11Buffer *buffer, uint32_t offset, uint32_t size) {
			D3D11_BUFFER_DESC description{};
			buffer->GetDesc(&description);
			description.Usage = D3D11_USAGE_STAGING;
			description.BindFlags = 0;
			description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			ComPtr<ID3D11Buffer> staging;
			assert(SUCCEEDED(d.d3dDevice->CreateBuffer(&description, nullptr, &staging)));
			d.d3dContext->CopyResource(staging.Get(), buffer);
			D3D11_MAPPED_SUBRESOURCE mapping{};
			assert(SUCCEEDED(d.d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
			uint8_t const *bytes = static_cast<uint8_t const *>(mapping.pData) + offset;
			std::vector<uint8_t> result(bytes, bytes + size);
			d.d3dContext->Unmap(staging.Get(), 0);
			return result;
		}

		void ResetGeometryCache() {
			for (auto &segment: d.indexBufferSegments) segment = {};
			for (auto &segment: d.vertexBufferSegments) segment = {};
			d.indexBufferCache.clear();
			d.vertexBufferCache.clear();
			d.activeIndexBufferSegment = d.activeVertexBufferSegment = 0;
			d.indexBufferCacheHits = d.indexBufferCacheMisses = 0;
		}

		// A native 32-bit line list [0, 1] once hashed exactly like a converted fan uploading [5, 0, 1].
		void IndexKeysDoNotAlias() {
			ColorVertex vertices[6]{};
			d.InterleavedArrays(kGDVertexFormat_V3F_C4UB, 0, vertices);
			uint32_t const lineIndices[]{0, 1};
			uint32_t const fanIndices[]{5, 0, 1};
			for (bool lineFirst: {true, false}) {
				ResetGeometryCache();
				for (int draw = 0; draw < 2; ++draw) {
					bool const line = (draw == 0) == lineFirst;
					if (line) d.DrawElements(4, 2, 5, lineIndices);
					else d.DrawElements(2, 3, 5, fanIndices);
					uint32_t const *expected = line ? lineIndices : fanIndices;
					uint32_t const bytes = (line ? 2u : 3u) * sizeof(uint32_t);
					assert(ReadBuffer(d.dynamicIndexBuffer.Get(), d.dynamicIndexBufferOffset, bytes) ==
						std::vector<uint8_t>(reinterpret_cast<uint8_t const *>(expected),
						                     reinterpret_cast<uint8_t const *>(expected) + bytes));
				}
				assert(d.indexBufferCacheMisses == 2 && d.indexBufferCacheHits == 0);
				assert(d.indexBufferCache.size() == 2);
			}
		}

		template <typename T>
		std::vector<uint8_t> Bytes(T const *values, size_t count) {
			return {reinterpret_cast<uint8_t const *>(values), reinterpret_cast<uint8_t const *>(values) + count * sizeof(T)};
		}

		std::vector<uint8_t> UploadedIndices(uint32_t bytes) {
			return ReadBuffer(d.dynamicIndexBuffer.Get(), d.dynamicIndexBufferOffset, bytes);
		}

		void IndexUploadsUseFinalContents() {
			std::vector<ColorVertex> vertices(400);
			d.InterleavedArrays(kGDVertexFormat_V3F_C4UB, 0, vertices.data());
			ResetGeometryCache();

			// 16-bit indices above 255, uploaded natively.
			uint16_t const wide16[]{300, 301, 302, 256, 399, 300};
			d.DrawElements(0, 6, 3, wide16);
			assert(UploadedIndices(sizeof(wide16)) == Bytes(wide16, 6));
			assert(d.indexBufferCache.count(IndexCacheKey(DXGI_FORMAT_R16_UINT, wide16, 6)) == 1);

			// The same values as 32-bit indices are a different upload.
			uint32_t const wide32[]{300, 301, 302, 256, 399, 300};
			d.DrawElements(0, 6, 5, wide32);
			assert(UploadedIndices(sizeof(wide32)) == Bytes(wide32, 6));
			assert(d.indexBufferCacheMisses == 2 && d.indexBufferCacheHits == 0);

			// A 16-bit fan converts to the 32-bit list [300, 301, 302, 300, 302, 256], hashed as uploaded.
			uint16_t const fan16[]{300, 301, 302, 256};
			uint32_t const fanList[]{300, 301, 302, 300, 302, 256};
			d.DrawElements(2, 4, 3, fan16);
			assert(UploadedIndices(sizeof(fanList)) == Bytes(fanList, 6));
			assert(d.indexBufferCache.count(IndexCacheKey(DXGI_FORMAT_R32_UINT, fanList, 6)) == 1);
			assert(d.indexBufferCacheMisses == 3);

			// A native list with those exact 32-bit contents reuses the converted upload, and vice versa.
			d.DrawElements(0, 6, 5, fanList);
			uint32_t const fan32[]{300, 301, 302, 256};
			d.DrawElements(2, 4, 5, fan32);
			assert(d.indexBufferCacheMisses == 3 && d.indexBufferCacheHits == 2);
			assert(UploadedIndices(sizeof(fanList)) == Bytes(fanList, 6));
		}

		bool Rejected() {
			bool const invalid = d.lastError == cGDriver::DriverError::INVALID_VALUE;
			d.lastError = cGDriver::DriverError::OK;
			return invalid;
		}

		// Every input below would fault if the driver read the source memory it points at.
		void OversizedInputsFailBeforeReading() {
			void *const noAccess = VirtualAlloc(nullptr, 4096, MEM_RESERVE, PAGE_NOACCESS);
			void const *const nearTop = reinterpret_cast<void const *>(uintptr_t{0xFFFFF000u});
			assert(noAccess != nullptr);
			d.lastError = cGDriver::DriverError::OK;

			uint32_t const texture = static_cast<uint32_t>(d.CreateTexture(1, 4, 4, 1, 0));
			d.LoadTextureLevel(texture, 0, INT32_MAX, 0, 2, 2, 3, 1, 0, noAccess);
			assert(Rejected());
			d.LoadTextureLevel(texture, 0, 0, INT32_MAX, 2, 2, 3, 1, 0, noAccess);
			assert(Rejected());
			// A row pitch that still fits 32 bits, but whose second row lies past the address space.
			d.LoadTextureLevel(texture, 0, 0, 0, 2, 2, 3, 1, 0x3FFFFFFF, nearTop);
			assert(Rejected());
			if (d.supportedExtensions.textureCompression) {
				uint32_t const compressed = static_cast<uint32_t>(d.CreateTexture(5, 8, 8, 1, 0));
				d.LoadTextureLevel(compressed, 0, 0, 0, 8, 8, 7, 0, 0x7FFFFFFC, nearTop);
				assert(Rejected());
				d.DeleteTextures(1, &compressed);
			}

			d.InterleavedArrays(kGDVertexFormat_V3F_C4UB, 0x40000000, noAccess);
			d.DrawArrays(0, 0, 5);
			d.InterleavedArrays(kGDVertexFormat_V3F_C4UB, 0, nearTop);
			d.DrawArrays(0, 0, 1000);
			d.DrawArrays(0, INT32_MAX, 3);
			uint32_t const indices[]{0, 1, UINT32_MAX - 1};
			d.DrawElements(0, 3, 5, indices);

			int32_t before[4]{};
			d.GetViewport(before);
			d.SetViewport(INT32_MAX, 0, 10, 10);
			assert(Rejected());
			d.SetViewport(0, INT32_MAX - 5, 10, 10);
			assert(Rejected());
			int32_t after[4]{};
			d.GetViewport(after);
			assert(memcmp(before, after, sizeof(before)) == 0);
			d.SetViewport(INT32_MAX - 10, 0, 10, 10);
			assert(!Rejected());
			d.SetViewport();

			d.DeleteTextures(1, &texture);
			VirtualFree(noAccess, 0, MEM_RELEASE);
		}

		uint64_t VertexUploads() {
			return d.vertexBufferCacheHits + d.vertexBufferCacheMisses;
		}

		void TerrainDrawsStayInReservation() {
			uint32_t const stride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
			uint32_t const capacity = d.MaxVertices(0);
			d.Reset();

			// Exact capacity, then continuations that would run or wrap past it.
			assert(d.GetVertices(0, capacity) != 0);
			assert(d.ContinueVertices(0, 1) == 0);
			d.ReleaseVertices(0);
			assert(d.GetVertices(0, 100) != 0);
			assert(d.ContinueVertices(0, UINT32_MAX) == 0);
			assert(d.ContinueVertices(0, UINT32_MAX - 98) == 0);
			assert(d.ContinueVertices(0, capacity - 100) != 0);
			assert(d.ContinueVertices(0, 1) == 0);
			d.ReleaseVertices(0);

			// A 100-vertex reservation in the middle of the backing store.
			d.Reset();
			assert(d.GetVertices(0, 50) != 0);
			d.ReleaseVertices(0);
			assert(d.GetVertices(0, 100) != 0);
			uint64_t uploads = VertexUploads();
			d.DrawPrims(0, 0, nullptr, 3 * stride);
			assert(VertexUploads() == uploads); // still locked
			d.ReleaseVertices(0);

			d.DrawPrims(0, 0, nullptr, 101 * stride);
			assert(VertexUploads() == uploads);
			d.DrawPrims(0, 0, nullptr, 100 * stride);
			assert(VertexUploads() == ++uploads);

			uint16_t outside[]{0, 1, 100};
			d.DrawPrimsIndexed(0, 0, 3, outside);
			assert(VertexUploads() == uploads);
			outside[2] = 99;
			d.DrawPrimsIndexed(0, 0, 3, outside);
			assert(VertexUploads() == ++uploads);

			d.Reset();
			d.DrawPrims(0, 0, nullptr, 3 * stride);
			d.DrawPrimsIndexed(0, 0, 3, outside);
			assert(VertexUploads() == uploads);
		}

		float UploadedX(uint32_t vertex) {
			std::vector<uint8_t> const bytes = ReadBuffer(
				d.dynamicVertexBuffer.Get(), d.dynamicVertexBufferOffset + vertex * sizeof(D3D11Vertex),
				sizeof(float));
			float x;
			memcpy(&x, bytes.data(), sizeof(x));
			return x;
		}

		// Cached uploads must never outlive the data they were made from.
		void CachedVerticesFollowTheirSource() {
			ResetGeometryCache();
			d.vertexBufferCacheHits = d.vertexBufferCacheMisses = 0;

			// Content keys: rewriting the same memory is a new upload, restoring it a hit.
			ColorVertex vertices[3]{};
			d.InterleavedArrays(kGDVertexFormat_V3F_C4UB, 0, vertices);
			d.DrawArrays(0, 0, 3);
			vertices[2].position[0] = 7.0f;
			d.DrawArrays(0, 0, 3);
			assert(d.vertexBufferCacheMisses == 2 && UploadedX(2) == 7.0f);
			vertices[2].position[0] = 0.0f;
			d.DrawArrays(0, 0, 3);
			assert(d.vertexBufferCacheMisses == 2 && d.vertexBufferCacheHits == 1 && UploadedX(2) == 0.0f);

			// Generation keys: a reservation that wraps back to the same offset and size is new data.
			uint32_t const stride = RZVertexFormatStride(kGDVertexFormat_V3F_C4UB_2T2F);
			uint32_t const capacity = d.MaxVertices(0);
			auto writeX = [&](uint32_t address, float x) {
				memcpy(reinterpret_cast<uint8_t *>(static_cast<uintptr_t>(address)), &x, sizeof(x));
			};
			d.Reset();
			uint32_t const first = d.GetVertices(0, 20);
			writeX(first, 1.0f);
			d.ReleaseVertices(0);
			d.DrawPrims(0, 0, nullptr, 20 * stride);
			d.DrawPrims(0, 0, nullptr, 20 * stride);
			assert(d.vertexBufferCacheMisses == 3 && d.vertexBufferCacheHits == 2 && UploadedX(0) == 1.0f);
			assert(d.GetVertices(0, capacity - 20) != 0);
			d.ReleaseVertices(0);
			uint32_t const wrapped = d.GetVertices(0, 20);
			assert(wrapped == first);
			writeX(wrapped, 2.0f);
			d.ReleaseVertices(0);
			d.DrawPrims(0, 0, nullptr, 20 * stride);
			assert(d.vertexBufferCacheMisses == 4 && UploadedX(0) == 2.0f);

			// Device recreation drops every cached upload.
			assert(d.RecoverD3D11Device());
			assert(d.vertexBufferCache.empty() && d.indexBufferCache.empty());
			d.InterleavedArrays(kGDVertexFormat_V3F_C4UB, 0, vertices);
			d.DrawArrays(0, 0, 3);
			assert(d.vertexBufferCacheMisses == 5 && UploadedX(2) == 0.0f);
		}

		// Leaves the immediate context in a state that would break the next draw if not cleaned up.
		static void __stdcall ClobberingCallback(SCD3D11FrameContext const *frame, void *) {
			if (frame->event != SCD3D11_EVENT_RENDER) return;
			ID3D11DeviceContext *const context = frame->context;
			D3D11_VIEWPORT const tiny{0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
			context->RSSetViewports(1, &tiny);
			context->OMSetRenderTargets(0, nullptr, nullptr);
			context->PSSetShader(nullptr, nullptr, 0);
			context->IASetInputLayout(nullptr);
		}

		void FrameCallbackCleanupOnlyWhenNeeded() {
			d.SetViewport();
			DrawFullscreen(0, 0, 255);
			d.Flush();
			// No callback: the cached pipeline bindings survive the frame.
			assert(d.geometryPipelineBound && d.appliedPixelShader != nullptr);
			DrawFullscreen(0, 255, 0);
			assert(Pixel(1, 1) == 0x00ff00ff && Pixel(d.windowWidth - 2, d.windowHeight - 2) == 0x00ff00ff);

			assert(SCD3D11RegisterFrameCallback(ClobberingCallback, this));
			d.Flush();
			assert(!d.geometryPipelineBound && d.appliedPixelShader == nullptr);
			DrawFullscreen(255, 255, 0);
			assert(Pixel(1, 1) == 0xffff00ff && Pixel(d.windowWidth - 2, d.windowHeight - 2) == 0xffff00ff);
			assert(SCD3D11UnregisterFrameCallback(ClobberingCallback, this));
		}

		// Uploads a width x height rectangle whose source rows are rowLength texels wide (0 = width),
		// then checks the whole level against a CPU model of the texture.
		void UploadAndCompare(uint32_t texture, std::vector<uint8_t> &model, uint32_t textureWidth,
		                      uint32_t texelBytes, int32_t x, int32_t y, int32_t width, int32_t height,
		                      uint32_t rowLength, uint32_t format, uint32_t type, uint32_t sourceTexelBytes,
		                      uint8_t seed) {
			uint32_t const sourceWidth = rowLength ? rowLength : static_cast<uint32_t>(width);
			std::vector<uint8_t> source(static_cast<size_t>(sourceWidth) * height * sourceTexelBytes, 0xEE);
			for (int32_t row = 0; row < height; ++row) {
				for (int32_t column = 0; column < width; ++column) {
					uint8_t *texel = source.data() + (static_cast<size_t>(row) * sourceWidth + column) * sourceTexelBytes;
					for (uint32_t byte = 0; byte < sourceTexelBytes; ++byte) {
						texel[byte] = static_cast<uint8_t>(seed + row * 16 + column * 4 + byte);
					}
					// The texture's own layout: BGRA8 and B4G4R4A4 copy through, RGBA8 swaps red and blue.
					uint8_t *destination = model.data() + (static_cast<size_t>(y + row) * textureWidth + x + column) * texelBytes;
					memcpy(destination, texel, texelBytes);
					if (format == 1) std::swap(destination[0], destination[2]);
				}
			}
			d.LoadTextureLevel(texture, 0, x, y, width, height, format, type, rowLength, source.data());
			assert(Read(d.textures[texture].texture.Get(), 0, texelBytes) == model);
		}

		void TextureUploadsHonorRowPitch() {
			std::vector<uint8_t>().swap(d.textureUploadScratch);
			for (uint32_t internalFormat: {1u, 2u}) {
				bool const bgra8 = internalFormat == 1;
				uint32_t const texelBytes = bgra8 ? 4 : 2;
				uint32_t const type = bgra8 ? 1 : 13;
				uint32_t const texture = static_cast<uint32_t>(d.CreateTexture(internalFormat, 8, 6, 1, 0));
				assert(texture != 0);
				std::vector<uint8_t> model(8 * 6 * texelBytes);
				UploadAndCompare(texture, model, 8, texelBytes, 0, 0, 8, 6, 0, 3, type, texelBytes, 1);
				UploadAndCompare(texture, model, 8, texelBytes, 0, 0, 8, 6, 11, 3, type, texelBytes, 2);
				UploadAndCompare(texture, model, 8, texelBytes, 2, 1, 3, 4, 0, 3, type, texelBytes, 3);
				UploadAndCompare(texture, model, 8, texelBytes, 5, 2, 3, 4, 7, 3, type, texelBytes, 4);
				// Matching layouts never touch the conversion scratch.
				assert(d.textureUploadScratch.capacity() == 0);
				if (bgra8) {
					// RGBA still converts, padded or not.
					UploadAndCompare(texture, model, 8, 4, 1, 1, 4, 3, 6, 1, 1, 4, 5);
					UploadAndCompare(texture, model, 8, 4, 0, 3, 8, 3, 0, 1, 1, 4, 6);
					assert(d.textureUploadScratch.capacity() != 0);
					std::vector<uint8_t>().swap(d.textureUploadScratch);
				}
				d.DeleteTextures(1, &texture);
			}
		}

		int Run() {
			DrawsGeometry();
			IndexKeysDoNotAlias();
			IndexUploadsUseFinalContents();
			OversizedInputsFailBeforeReading();
			TerrainDrawsStayInReservation();
			CachedVerticesFollowTheirSource();
			FrameCallbackCleanupOnlyWhenNeeded();
			TextureUploadsHonorRowPitch();
			return 0;
		}
	};
}

int main() {
	using nSCD3D11::cGDriver;
	cGDriver *const driver = new cGDriver();
	driver->AddRef();
	int32_t mode = -1;
	if (driver->Init()) {
		sGDMode info{};
		for (uint32_t index = 0; index < driver->CountVideoModes(); ++index) {
			driver->GetVideoModeInfo(index, info);
			if (!info.isFullscreen) {
				mode = static_cast<int32_t>(index);
				break;
			}
		}
	}
	if (mode >= 0) driver->SetVideoMode(mode, nullptr, false, false);
	if (!driver->IsDeviceReady()) {
		std::puts("no D3D11 device; skipping");
		driver->Release();
		return 77;
	}
	int const result = nSCD3D11::cGDriverTestAccess{*driver}.Run();
	driver->Shutdown();
	driver->Release();
	return result;
}
