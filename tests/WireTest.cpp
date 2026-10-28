#include "cGDriver.h"
#include "cGDriverWire.h"
#include "VertexFormatUtils.h"
#include "wire/WireReplay.h"
#include "wire/WireStream.h"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace nSCDWire;

namespace nSCD3D11 {
	// Befriended by cGDriver. Reads the back buffer.
	struct cGDriverTestAccess {
		cGDriver &d;

		std::vector<uint8_t> BackBuffer() {
			D3D11_TEXTURE2D_DESC description{};
			d.backBufferTexture->GetDesc(&description);
			description.Usage = D3D11_USAGE_STAGING;
			description.BindFlags = 0;
			description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
			description.MiscFlags = 0;
			ComPtr<ID3D11Texture2D> staging;
			assert(SUCCEEDED(d.d3dDevice->CreateTexture2D(&description, nullptr, &staging)));
			d.d3dContext->CopyResource(staging.Get(), d.backBufferTexture.Get());
			D3D11_MAPPED_SUBRESOURCE mapping{};
			assert(SUCCEEDED(d.d3dContext->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapping)));
			std::vector<uint8_t> pixels(static_cast<size_t>(description.Width) * description.Height * 4);
			for (UINT y = 0; y < description.Height; ++y) {
				std::memcpy(pixels.data() + static_cast<size_t>(y) * description.Width * 4,
				            static_cast<uint8_t const *>(mapping.pData) + static_cast<size_t>(y) * mapping.RowPitch,
				            static_cast<size_t>(description.Width) * 4);
			}
			d.d3dContext->Unmap(staging.Get(), 0);
			return pixels;
		}
	};
}

namespace {
	using nSCD3D11::cGDriver;
	using nSCD3D11::cGDriverTestAccess;
	using nSCD3D11::cGDriverWire;

	// Blobs are sent once, refer back by key, and come again only after the writer forgot them.
	void BlobsAreSentOnceAndForgotten() {
		std::vector<uint8_t> stream;
		Writer writer([&](void const *data, size_t size) {
			stream.insert(stream.end(), static_cast<uint8_t const *>(data), static_cast<uint8_t const *>(data) + size);
			return true;
		}, 100);
		uint8_t a[60], b[60];
		std::memset(a, 1, sizeof(a));
		std::memset(b, 2, sizeof(b));
		for (uint8_t const *blob: {a, a, b, a}) {
			writer.Begin(Op::Clear);
			writer.PutBlob(blob, 60);
			writer.End();
		}
		writer.Flush();
		// a, a again (key only), b (forgets a: 120 > 100), a (forgets b, sent again).
		assert(writer.BlobBytesSent() == 180);

		size_t offset = 0;
		Reader reader([&](void *data, size_t size) {
			if (stream.size() - offset < size) return false;
			std::memcpy(data, stream.data() + offset, size);
			offset += size;
			return true;
		});
		Op op{};
		Fields fields(nullptr, 0);
		int records = 0;
		for (uint8_t const *expected: {a, a, b, a}) {
			assert(reader.Next(op, fields) && op == Op::Clear);
			std::vector<uint8_t> const *blob = reader.FindBlob(fields.Get<BlobKey>());
			assert(fields.ok && blob != nullptr && std::memcmp(blob->data(), expected, 60) == 0);
			++records;
		}
		assert(records == 4 && !reader.Next(op, fields) && *reader.Error() == '\0');
	}

	struct ColorVertex {
		float position[3];
		uint8_t bgra[4];
		float uv[2];
	};

	// A frame that touches the recorded paths: textures, indexed and array draws, rebased indices, the
	// terrain vertex buffer, state changes and a blit.
	void DrawScene(cGDriverWire &wire, int frame) {
		wire.ClearColor(0.1f * frame, 0.2f, 0.3f, 1.0f);
		wire.Clear(0x4100);
		for (uint32_t mode: {0u, 1u}) {
			wire.MatrixMode(mode);
			wire.LoadIdentity();
		}

		static uint32_t texture = 0;
		if (texture == 0) {
			texture = static_cast<uint32_t>(wire.CreateTexture(5, 4, 4, 1, 0));
			uint8_t texels[4 * 4 * 4];
			for (int i = 0; i < 16; ++i) {
				texels[i * 4 + 0] = static_cast<uint8_t>(i * 16);
				texels[i * 4 + 1] = static_cast<uint8_t>(255 - i * 16);
				texels[i * 4 + 2] = 64;
				texels[i * 4 + 3] = 255;
			}
			wire.LoadTextureLevel(texture, 0, 0, 0, 4, 4, 3, 1, 0, texels);
		}

		ColorVertex vertices[8];
		for (int i = 0; i < 8; ++i) {
			float const x = -0.9f + 0.25f * i;
			vertices[i] = {{x, (i % 2) ? 0.6f : -0.6f, 0.0f}, {static_cast<uint8_t>(30 * i), 200, 100, 255},
			               {static_cast<float>(i % 2), static_cast<float>(i / 4)}};
		}
		wire.SetTexture(texture, 0);
		wire.InterleavedArrays(kGDVertexFormat_V3F_C4UB_2T2F, 0, vertices);
		wire.DrawArrays(0, 0, 3);
		// Indices that do not start at zero exercise the rebasing.
		uint16_t const indices[]{4, 5, 6, 5, 6, 7};
		wire.DrawElements(0, 6, 3, indices);

		wire.SetTexture(0, 0);
		wire.Enable(2);
		wire.BlendFunc(4, 5);
		uint32_t const reserved = wire.GetVertices(0, 3);
		assert(reserved != 0);
		auto *terrain = reinterpret_cast<ColorVertex *>(static_cast<uintptr_t>(reserved));
		terrain[0] = {{-1.0f, -1.0f, 0.0f}, {0, 0, 255, 128}, {0, 0}};
		terrain[1] = {{0.0f, -1.0f, 0.0f}, {0, 0, 255, 128}, {0, 0}};
		terrain[2] = {{-1.0f, 0.0f, 0.0f}, {0, 0, 255, 128}, {0, 0}};
		wire.ReleaseVertices(0);
		wire.DrawPrims(0, 0, nullptr, 3 * sizeof(ColorVertex));
		wire.Disable(2);

		uint32_t const pixels[4]{0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff};
		wire.BitBlt(4 + frame, 4, 2, 2, 3, 1, pixels, false, nullptr);
		wire.Flush();
	}

	// What goes through cGDriverWire comes out of a replay into another driver as the same pixels.
	int ReplayMatchesTheRecordedDriver() {
		std::vector<uint8_t> stream;
		auto *const recorded = new cGDriver();
		auto *const wire = new cGDriverWire(recorded, [&](void const *data, size_t size) {
			stream.insert(stream.end(), static_cast<uint8_t const *>(data), static_cast<uint8_t const *>(data) + size);
			return true;
		});
		wire->AddRef();

		int32_t mode = -1;
		if (wire->Init()) {
			sGDMode info{};
			for (uint32_t index = 0; index < wire->CountVideoModes() && mode < 0; ++index) {
				wire->GetVideoModeInfo(index, info);
				if (!info.isFullscreen) mode = static_cast<int32_t>(index);
			}
		}
		if (mode >= 0) wire->SetVideoMode(mode, nullptr, false, false);
		if (!wire->IsDeviceReady()) {
			std::puts("no D3D11 device; skipping the replay check");
			wire->Release();
			return 77;
		}

		std::vector<std::vector<uint8_t>> expected;
		for (int frame = 0; frame < 3; ++frame) {
			DrawScene(*wire, frame);
			expected.push_back(cGDriverTestAccess{*recorded}.BackBuffer());
		}
		wire->Stream().Flush();

		auto *const replayed = new cGDriver();
		replayed->AddRef();
		struct Hooks : nSCD3D11::cGDriverReplayHooks {
			cGDriver &driver;
			std::vector<std::vector<uint8_t>> frames;
			explicit Hooks(cGDriver &driver) : cGDriverReplayHooks(driver), driver(driver) {}
			bool Frame() override {
				frames.push_back(cGDriverTestAccess{driver}.BackBuffer());
				return true;
			}
		} hooks(*replayed);

		size_t offset = 0;
		Reader reader([&](void *data, size_t size) {
			if (stream.size() - offset < size) return false;
			std::memcpy(data, stream.data() + offset, size);
			offset += size;
			return true;
		});
		ReplayStats stats;
		char const *error = nullptr;
		bool const ok = Replay(reader, *replayed, hooks, &stats, &error);
		if (!ok) std::printf("replay failed: %s\n", error);
		assert(ok);
		assert(stats.frames == 3 && hooks.frames.size() == 3);
		for (size_t frame = 0; frame < 3; ++frame) {
			assert(hooks.frames[frame].size() == expected[frame].size());
			size_t differing = 0;
			for (size_t i = 0; i < expected[frame].size(); ++i) differing += hooks.frames[frame][i] != expected[frame][i];
			if (differing != 0) std::printf("frame %zu: %zu bytes differ\n", frame, differing);
			assert(differing == 0);
		}
		std::printf("replayed %llu records, %zu stream bytes\n", static_cast<unsigned long long>(stats.records),
		            stream.size());

		replayed->Shutdown();
		replayed->Release();
		wire->Shutdown();
		wire->Release();
		return 0;
	}
}

int main() {
	BlobsAreSentOnceAndForgotten();
	return ReplayMatchesTheRecordedDriver();
}
