#include "cGDriver.h"
#include "cGDriverWire.h"
#include "VertexFormatUtils.h"
#include "wire/WireReplay.h"
#include "wire/WireStream.h"
#include "cIGZGDriverLightingExtension.h"

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
		wire.InterleavedArrays(kGDVertexFormat_V3F_C4UB_T2F, 0, vertices);
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

	struct LitVertex {
		float position[3];
		float normal[3];
		uint8_t bgra[4];
	};

	void Quad(cGDriverWire &wire, float left, float bottom, float right, float top, float z,
	          uint8_t const bgra[4], uint32_t primitive = 2) {
		ColorVertex const vertices[4]{
			{{left, bottom, z}, {bgra[0], bgra[1], bgra[2], bgra[3]}, {0, 1}},
			{{right, bottom, -z}, {bgra[0], bgra[1], bgra[2], bgra[3]}, {1, 1}},
			{{right, top, -z}, {bgra[0], bgra[1], bgra[2], bgra[3]}, {1, 0}},
			{{left, top, z}, {bgra[0], bgra[1], bgra[2], bgra[3]}, {0, 0}},
		};
		wire.InterleavedArrays(kGDVertexFormat_V3F_C4UB_T2F, 0, vertices);
		wire.DrawArrays(primitive, 0, 4);
	}

	// The calls the game's captures rarely or never make: textures through TexImage2D with a row length
	// and a mip level, stencil, fog, the lighting extension, quads, fans and lines, flat shading, and
	// blits of every kind from 16 and 32-bit sources with a row length.
	void DrawCoverage(cGDriverWire &wire, int frame) {
		wire.ClearColor(0.05f, 0.05f, 0.1f * frame, 1.0f);
		wire.ClearStencil(0);
		wire.Clear(0x4000 | 0x1000 | 0x2000);
		for (uint32_t mode: {0u, 1u}) {
			wire.MatrixMode(mode);
			wire.LoadIdentity();
		}

		// TexImage2D: 8x8 BGRA rows 10 pixels apart, then its 4x4 level, then 16-bit textures.
		static uint32_t names[3]{};
		if (names[0] == 0) {
			wire.GenTextures(3, names);
			std::vector<uint8_t> texels(10 * 8 * 4);
			for (size_t i = 0; i < texels.size(); ++i) texels[i] = static_cast<uint8_t>(i * 37 + 11);
			wire.BindTexture(0, names[0]);
			wire.PixelStore(0, 10);
			wire.TexImage2D(0, 0, 1, 8, 8, 0, 3, 1, texels.data());
			wire.PixelStore(0, 0);
			wire.TexImage2D(0, 1, 1, 4, 4, 0, 3, 1, texels.data());
			wire.TexParameter(0, 1, 6);
			uint16_t packed[16];
			for (int i = 0; i < 16; ++i) packed[i] = static_cast<uint16_t>(0x1234 * (i + 1));
			wire.BindTexture(0, names[1]);
			wire.TexImage2D(0, 0, 2, 4, 4, 0, 3, 13, packed);
			uint8_t rgb[4 * 4 * 3];
			for (int i = 0; i < 48; ++i) rgb[i] = static_cast<uint8_t>(i * 5);
			wire.BindTexture(0, names[2]);
			wire.TexImage2D(0, 0, 0, 4, 4, 0, 0, 1, rgb);
		}
		uint8_t const white[4]{255, 255, 255, 255};
		wire.Enable(kGDCapability_Texture2D);
		for (int i = 0; i < 3; ++i) {
			wire.BindTexture(0, names[i]);
			wire.TexParameter(0, 2, i == 1 ? 2 : 3);
			float const left = -1.0f + 0.4f * i;
			Quad(wire, left, 0.5f, left + 0.35f, 0.95f, 0.0f, white, i == 2 ? 6 : 2);
		}
		wire.BindTexture(0, 0);
		wire.Disable(kGDCapability_Texture2D);

		// Stencil: a mask written without colour, then a quad drawn only where the mask is.
		uint8_t const red[4]{0, 0, 255, 255};
		wire.Enable(kGDCapability_StencilTest);
		wire.StencilMask(0xff);
		wire.StencilFunc(7, 1, 0xff);
		wire.StencilOp(1, 1, 2);
		wire.ColorMask(false);
		Quad(wire, 0.2f, 0.5f, 0.6f, 0.95f, 0.0f, white);
		wire.ColorMask(true);
		wire.StencilFunc(2, 1, 0xff);
		wire.StencilOp(1, 1, 1);
		Quad(wire, 0.1f, 0.6f, 0.9f, 0.8f, 0.0f, red);
		wire.Disable(kGDCapability_StencilTest);

		// Linear fog over depth, flat shading, depth test with an offset.
		uint8_t const green[4]{0, 200, 0, 255};
		float const fogColor[4]{0.6f, 0.6f, 0.7f, 1.0f};
		float const fogStart = 0.0f, fogEnd = 1.0f;
		wire.Enable(kGDCapability_Fog);
		wire.Fog(0, 2u);
		wire.Fog(1, fogColor);
		wire.Fog(3, &fogStart);
		wire.Fog(4, &fogEnd);
		wire.Enable(kGDCapability_DepthTest);
		wire.DepthFunc(3);
		wire.PolygonOffset(2);
		wire.ShadeModel(0);
		Quad(wire, -1.0f, 0.0f, -0.2f, 0.45f, 0.8f, green);
		wire.ShadeModel(1);
		wire.PolygonOffset(0);
		wire.Disable(kGDCapability_DepthTest);
		wire.Disable(kGDCapability_Fog);

		// The lighting extension: one directional light on a quad with normals.
		cIGZGDriverLightingExtension *lighting = nullptr;
		if (wire.QueryInterface(GZIID_cIGZGDriverLightingExtension, reinterpret_cast<void **>(&lighting))) {
			float const diffuse[4]{1.0f, 0.8f, 0.6f, 1.0f};
			float const ambient[4]{0.1f, 0.1f, 0.2f, 1.0f};
			float const direction[4]{0.3f, 0.4f, 1.0f, 0.0f};
			float const material[4]{0.9f, 0.9f, 0.9f, 1.0f};
			float const black[4]{0.0f, 0.0f, 0.0f, 1.0f};
			lighting->EnableLighting(true);
			lighting->EnableLight(0, true);
			lighting->LightModelAmbient(0.2f, 0.2f, 0.2f, 1.0f);
			lighting->LightColor(0, ambient, diffuse, black);
			lighting->LightColor(0, 1, diffuse);
			lighting->LightDirection(0, direction);
			lighting->MaterialColor(material, material, black, black, 0.0f);
			lighting->MaterialColor(1, material);
			wire.EnableVertexColors(false, false);
			LitVertex const lit[4]{
				{{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {50, 50, 50, 255}},
				{{0.9f, 0.0f, 0.0f}, {0.7f, 0.0f, 0.7f}, {50, 50, 50, 255}},
				{{0.9f, 0.45f, 0.0f}, {0.0f, 0.7f, 0.7f}, {50, 50, 50, 255}},
				{{0.0f, 0.45f, 0.0f}, {-0.7f, 0.0f, 0.7f}, {50, 50, 50, 255}},
			};
			uint16_t const fan[]{0, 1, 2, 3};
			wire.InterleavedArrays(kGDVertexFormat_V3F_N3F_C4UB, 0, lit);
			wire.DrawElements(7, 4, 3, fan);
			wire.EnableVertexColors(true, true);
			lighting->EnableLighting(false);
			lighting->Release();
		}

		// Lines and points.
		uint8_t const yellow[4]{0, 255, 255, 255};
		Quad(wire, -0.9f, -0.2f, -0.1f, -0.05f, 0.0f, yellow, 4);
		Quad(wire, 0.1f, -0.2f, 0.9f, -0.05f, 0.0f, yellow, 3);

		// Blits: 32-bit rows 12 pixels apart, a keyed 16-bit one and the alpha kinds.
		uint32_t wide[12 * 6];
		for (int i = 0; i < 12 * 6; ++i) wide[i] = 0x80000000u | static_cast<uint32_t>(i * 0x030507);
		uint16_t narrow[8 * 8];
		for (int i = 0; i < 64; ++i) narrow[i] = static_cast<uint16_t>(i % 5 == 0 ? 0xf81f : 0x07e0 + i * 3);
		uint16_t const key = 0xf81f;
		bool sourceAlpha = true;
		wire.PixelStore(0, 12);
		wire.BitBlt(10 + frame, 300, 8, 6, 3, 1, wide, false, nullptr);
		wire.StretchBlt(40, 300, 16, 12, 8, 6, 3, 1, wide, false, nullptr);
		wire.BitBltAlpha(70, 300, 8, 6, 3, 1, wide, false, nullptr, 0x80);
		wire.StretchBltAlpha(90, 300, 16, 12, 8, 6, 3, 1, wide, false, nullptr, 0xc0ffffff);
		wire.BitBltAlphaModulate(120, 300, 8, 6, 3, 1, wide, false, nullptr, 0xff80ff40);
		wire.Punt(0x6C4236E7, &sourceAlpha);
		wire.StretchBltAlphaModulate(140, 300, 24, 18, 8, 6, 3, 1, wide, false, nullptr, 0x80ffffff);
		wire.BitBlt(170, 300, 8, 6, 3, 1, wide, false, nullptr);
		sourceAlpha = false;
		wire.Punt(0x6C4236E7, &sourceAlpha);
		wire.PixelStore(0, 0);
		wire.BitBlt(200, 300, 8, 8, 3, 8, narrow, true, &key);
		wire.Flush();
	}

	constexpr int kFrames = 5;

	// What goes through cGDriverWire comes out of a replay into another driver as the same pixels.
	int ReplayMatchesTheRecordedDriver(char const *save) {
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

		// The scene lane's records, which must come back as they went in.
		std::wstring const folders[] = {L"C:\\Game", L"C:\\Game\\English", L"", L"C:\\Game\\Plugins", L"D:\\Me\\Plugins"};
		nSCDWire::NetworkPiece piece{};
		piece.piece = 0x5E00;
		piece.flags = 0x6000000;
		piece.corners[3].uv[1] = 1.0f;
		piece.partCount = 2;
		NetworkPart parts[2]{};
		parts[1].texture = 7;
		parts[1].vertices[2].position[0] = 16.0f;
		nSCDWire::CellTexture cell{};
		cell.key = 0x26213000;
		cell.flat = 1;
		cell.vertices[3].color = 0xFFD6D2D4;
		wire->RecordGameFolders(folders);
		wire->RecordNetworkPiece(piece, parts);
		wire->RecordCellTexture(cell);

		std::vector<std::vector<uint8_t>> expected;
		for (int frame = 0; frame < kFrames; ++frame) {
			if (frame < 3) DrawScene(*wire, frame);
			else DrawCoverage(*wire, frame);
			expected.push_back(cGDriverTestAccess{*recorded}.BackBuffer());
		}
		wire->Stream().Flush();
		// The stream, for other renderers to replay and compare (SCD3D12Server replay).
		if (save != nullptr) {
			std::FILE *file = nullptr;
			assert(fopen_s(&file, save, "wb") == 0 && file != nullptr);
			std::fwrite(stream.data(), 1, stream.size(), file);
			std::fclose(file);
		}

		auto *const replayed = new cGDriver();
		replayed->AddRef();
		struct Hooks : nSCD3D11::cGDriverReplayHooks {
			cGDriver &driver;
			std::vector<std::vector<uint8_t>> frames;
			std::wstring folders[static_cast<size_t>(GameFolder::Count)];
			nSCDWire::NetworkPiece piece{};
			std::vector<NetworkPart> parts;
			nSCDWire::CellTexture cell{};
			explicit Hooks(cGDriver &driver) : cGDriverReplayHooks(driver), driver(driver) {}
			bool Frame() override {
				frames.push_back(cGDriverTestAccess{driver}.BackBuffer());
				return true;
			}
			void GameFolders(std::wstring const (&names)[static_cast<size_t>(GameFolder::Count)]) override {
				for (size_t i = 0; i < std::size(names); ++i) folders[i] = names[i];
			}
			void NetworkPiece(nSCDWire::NetworkPiece const &read, NetworkPart const *readParts) override {
				piece = read;
				parts.assign(readParts, readParts + read.partCount);
			}
			void CellTexture(nSCDWire::CellTexture const &read) override { cell = read; }
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
		assert(stats.frames == kFrames && hooks.frames.size() == kFrames);
		for (size_t i = 0; i < std::size(folders); ++i) assert(hooks.folders[i] == folders[i]);
		assert(std::memcmp(&hooks.piece, &piece, sizeof(piece)) == 0);
		assert(hooks.parts.size() == 2 && std::memcmp(hooks.parts.data(), parts, sizeof(parts)) == 0);
		assert(std::memcmp(&hooks.cell, &cell, sizeof(cell)) == 0);
		for (size_t frame = 0; frame < kFrames; ++frame) {
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

// WireTest [stream.scdw] saves the replayed stream when given a file.
int main(int argc, char **argv) {
	BlobsAreSentOnceAndForgotten();
	return ReplayMatchesTheRecordedDriver(argc > 1 ? argv[1] : nullptr);
}
