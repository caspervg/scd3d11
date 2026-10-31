/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#pragma once

#include "cGDriverBase.h"
#include "wire/WireReplay.h"
#include "wire/WireStream.h"

#include <cRZBaseUnknown.h>

#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace nSCD3D11 {
	class RemoteRenderer;

	// Stands in front of cGDriver: every call goes to the driver, and the calls a renderer elsewhere would
	// have to repeat are also written to an SCDW stream (wire/Wire.h). SC4 sees this object as its driver.
	//
	// Data the game owns (vertices, indices, texels) is copied into the stream when it is used, because the
	// game may change or free it right after the call returns.
	class cGDriverWire final :
			public cIGZGDriver,
			public cIGZGBufferRegionExtension,
			public cIGZGDriverLightingExtension,
			public cIGZGDriverVertexBufferExtension,
			public cIGZGSnapshotExtension,
			public cRZBaseUnknown {
	public:
		// Takes a reference on driver.
		cGDriverWire(cGDriverBase *driver, std::unique_ptr<nSCDWire::Output> output);
		cGDriverWire(cGDriverBase *driver, nSCDWire::Writer::Sink sink)
			: cGDriverWire(driver, std::make_unique<nSCDWire::SinkOutput>(std::move(sink))) {}
		~cGDriverWire();

		cGDriverBase &Inner() { return *driver; }
		// Renderer=server in SCD3D11: the renderer the driver draws through, released after the driver.
		void KeepRemote(std::unique_ptr<RemoteRenderer> renderer);
		// The driver that writes a stream this session, if any. Render thread only, which in SC4 is also the
		// thread that pumps messages.
		static cGDriverWire *Current();
		void RecordOccupant(nSCDWire::OccupantEvent const &event);
		void RecordOwnerBegin(nSCDWire::DrawOwner const &owner) { Record(nSCDWire::Op::OwnerBegin, owner); }
		void RecordOwnerEnd() { Record(nSCDWire::Op::OwnerEnd); }
		void RecordModelInstance(nSCDWire::ModelInstance const &model) { Record(nSCDWire::Op::ModelInstance, model); }
		void RecordNetworkPiece(nSCDWire::NetworkPiece const &piece, nSCDWire::NetworkPart const *parts) {
			writer.Begin(nSCDWire::Op::NetworkPiece);
			writer.Put(piece);
			writer.PutBytes(parts, piece.partCount * sizeof(nSCDWire::NetworkPart));
			writer.End();
		}
		void RecordCellTexture(nSCDWire::CellTexture const &cell) { Record(nSCDWire::Op::CellTexture, cell); }
		void RecordFreeCamera(nSCDWire::FreeCamera const &camera) { Record(nSCDWire::Op::FreeCamera, camera); }
		void RecordTerrainField(nSCDWire::TerrainField const &field, nSCDWire::TerrainVertex const *vertices) {
			writer.Begin(nSCDWire::Op::TerrainField);
			writer.Put(field);
			writer.PutBlob(vertices, field.verticesX * field.verticesZ * sizeof(nSCDWire::TerrainVertex));
			writer.End();
		}
		void RecordOccupantExemplar(nSCDWire::OccupantExemplar const &occupant, uint32_t const *values) {
			writer.Begin(nSCDWire::Op::OccupantExemplar);
			writer.Put(occupant);
			writer.PutBytes(values, occupant.count * sizeof(uint32_t));
			writer.End();
		}
		// One folder per nSCDWire::GameFolder kind.
		void RecordGameFolders(std::wstring const *folders) {
			gameFolders.assign(folders, folders + static_cast<size_t>(nSCDWire::GameFolder::Count));
			writer.Begin(nSCDWire::Op::GameFolders);
			for (size_t i = 0; i < static_cast<size_t>(nSCDWire::GameFolder::Count); ++i) {
				writer.Put(static_cast<uint32_t>(folders[i].size()));
				writer.PutBytes(folders[i].data(), folders[i].size() * sizeof(wchar_t));
			}
			writer.End();
		}
		nSCDWire::Writer &Stream() { return writer; }

		// cIGZUnknown
		bool QueryInterface(uint32_t riid, void **ppvObj) override;
		uint32_t AddRef(void) override;
		uint32_t Release(void) override;
		bool FinalRelease(void) override;

		// cIGZGDriver
		void DrawArrays(uint32_t primitive, int32_t first, int32_t count) override;
		void DrawElements(uint32_t primitive, int32_t count, uint32_t type, void const *indices) override;
		void InterleavedArrays(uint32_t format, int32_t stride, void const *pointer) override;
		uint32_t MakeVertexFormat(uint32_t count, intptr_t elements) override;
		uint32_t MakeVertexFormat(uint32_t format) override;
		uint32_t VertexFormatStride(uint32_t format) override;
		uint32_t VertexFormatElementOffset(uint32_t format, uint32_t element, uint32_t index) override;
		uint32_t VertexFormatNumElements(uint32_t format, uint32_t element) override;
		void Clear(uint32_t mask) override;
		void ClearColor(float r, float g, float b, float a) override;
		void ClearDepth(double depth) override;
		void ClearStencil(int32_t stencil) override;
		void ColorMask(bool enabled) override;
		void DepthFunc(uint32_t function) override;
		void DepthMask(bool enabled) override;
		void StencilFunc(uint32_t function, int32_t reference, uint32_t mask) override;
		void StencilMask(uint32_t mask) override;
		void StencilOp(uint32_t fail, uint32_t depthFail, uint32_t pass) override;
		void BlendFunc(uint32_t source, uint32_t destination) override;
		void AlphaFunc(uint32_t function, float reference) override;
		void ShadeModel(uint32_t mode) override;
		void BindTexture(uint32_t target, uint32_t texture) override;
		void TexImage2D(uint32_t target, int32_t level, int32_t internalFormat, int32_t width, int32_t height,
		                int32_t border, uint32_t format, uint32_t type, void const *pixels) override;
		void PixelStore(uint32_t parameter, int32_t value) override;
		void TexEnv(uint32_t target, uint32_t parameter, int32_t value) override;
		void TexEnv(uint32_t target, uint32_t parameter, float const *value) override;
		void TexParameter(uint32_t target, uint32_t parameter, int32_t value) override;
		void Fog(uint32_t parameter, uint32_t value) override;
		void Fog(uint32_t parameter, float const *value) override;
		void ColorMultiplier(float r, float g, float b) override;
		void AlphaMultiplier(float a) override;
		void EnableVertexColors(bool ambient, bool diffuse) override;
		void GenTextures(int32_t count, uint32_t *names) override;
		void DeleteTextures(int32_t count, uint32_t const *names) override;
		bool IsTexture(uint32_t name) override;
		void PrioritizeTextures(int32_t count, uint32_t const *names, float const *priorities) override;
		bool AreTexturesResident(int32_t count, uint32_t const *names, bool *resident) override;
		void MatrixMode(uint32_t mode) override;
		void LoadMatrix(float const *matrix) override;
		void LoadIdentity(void) override;
		void Flush(void) override;
		void Enable(uint32_t state) override;
		void Disable(uint32_t state) override;
		bool IsEnabled(uint32_t state) override;
		void GetBoolean(uint32_t parameter, bool *value) override;
		void GetInteger(uint32_t parameter, int32_t *value) override;
		void GetFloat(uint32_t parameter, float *value) override;
		uint32_t GetError(void) override;
		void TexStage(uint32_t stage) override;
		void TexStageCoord(uint32_t source) override;
		void TexStageMatrix(float const *matrix, uint32_t rows, uint32_t columns, uint32_t flags) override;
		void TexStageCombine(eGDTextureStageCombineScaleParamType parameter,
		                     eGDTextureStageCombineScaleParam value) override;
		void TexStageCombine(eGDTextureStageCombineOperandType parameter, eGDBlend value) override;
		void TexStageCombine(eGDTextureStageCombineSourceParamType parameter,
		                     eGDTextureStageCombineSourceParam value) override;
		void TexStageCombine(eGDTextureStageCombineParamType parameter,
		                     eGDTextureStageCombineModeParam value) override;
		void SetTexture(uint32_t texture, uint32_t stage) override;
		intptr_t GetTexture(uint32_t stage) override;
		intptr_t CreateTexture(uint32_t internalFormat, uint32_t width, uint32_t height, uint32_t levels,
		                       uint32_t hints) override;
		void LoadTextureLevel(uint32_t texture, int32_t level, int32_t x, int32_t y, int32_t width, int32_t height,
		                      uint32_t format, uint32_t type, uint32_t rowLength, void const *pixels) override;
		void SetCombiner(cGDCombiner const &combiner, uint32_t stage) override;
		uint32_t CountVideoModes(void) const override;
		void GetVideoModeInfo(uint32_t index, sGDMode &mode) override;
		void GetVideoModeInfo(sGDMode &mode) override;
		void SetVideoMode(int32_t index, void *windowProcedure, bool show, bool fourth) override;
		void PolygonOffset(int32_t offset) override;
		void BitBlt(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type,
		            void const *pixels, bool colorKeyed, void const *colorKey) override;
		void StretchBlt(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
		                int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels, bool colorKeyed,
		                void const *colorKey) override;
		void BitBltAlpha(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format, uint32_t type,
		                 void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha) override;
		void StretchBltAlpha(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
		                     int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels,
		                     bool colorKeyed, void const *colorKey, uint32_t alpha) override;
		void BitBltAlphaModulate(int32_t x, int32_t y, int32_t width, int32_t height, uint32_t format,
		                         uint32_t type, void const *pixels, bool colorKeyed, void const *colorKey,
		                         uint32_t alpha) override;
		void StretchBltAlphaModulate(int32_t x, int32_t y, int32_t width, int32_t height, int32_t sourceWidth,
		                             int32_t sourceHeight, uint32_t format, uint32_t type, void const *pixels,
		                             bool colorKeyed, void const *colorKey, uint32_t alpha) override;
		void SetViewport(int32_t x, int32_t y, int32_t width, int32_t height) override;
		void SetViewport(void) override;
		void GetViewport(int32_t dimensions[4]) override;
		char const *GetDriverInfo(void) const override;
		uint32_t GetGZCLSID(void) const override;
		bool Init(void) override;
		bool Shutdown(void) override;
		bool IsDeviceReady(void) override;
		bool Punt(uint32_t id, void *data) override;

		// cIGZGDriverVertexBufferExtension
		char const *GetVertexBufferName(uint32_t format) override;
		uint32_t VertexBufferType(uint32_t name) override;
		uint32_t MaxVertices(uint32_t name) override;
		uint32_t GetVertices(int32_t name, uint32_t count) override;
		uint32_t ContinueVertices(uint32_t name, uint32_t count) override;
		void ReleaseVertices(uint32_t name) override;
		void DrawPrims(uint32_t name, uint32_t primitive, void *data, uint32_t byteSize) override;
		void DrawPrimsIndexed(uint32_t name, uint32_t primitive, uint32_t count, uint16_t *indices) override;
		void Reset(void) override;

		// cIGZGBufferRegionExtension
		bool BufferRegionEnabled(void) override;
		uint32_t NewBufferRegion(int32_t type) override;
		bool DeleteBufferRegion(int32_t region) override;
		bool ReadBufferRegion(uint32_t region, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e,
		                      int32_t f) override;
		bool DrawBufferRegion(uint32_t region, int32_t a, int32_t b, int32_t c, int32_t d, int32_t e,
		                      int32_t f) override;
		bool IsBufferRegion(uint32_t region) override;
		bool CanDoPartialRegionWrites(void) override;
		bool CanDoOffsetReads(void) override;
		bool DeleteAllBufferRegions(void) override;

		// cIGZGSnapshotExtension
		cIGZBuffer *CopyColorBuffer(int32_t x, int32_t y, int32_t width, int32_t height,
		                            cIGZBuffer *buffer) override;

		// cIGZGDriverLightingExtension
		void EnableLighting(bool enabled) override;
		void EnableLight(uint32_t light, bool enabled) override;
		void LightModelAmbient(float r, float g, float b, float a) override;
		void LightColor(uint32_t light, uint32_t parameter, float const *color) override;
		void LightColor(uint32_t light, float const *ambient, float const *diffuse,
		                float const *specular) override;
		void LightPosition(uint32_t light, float const *position) override;
		void LightDirection(uint32_t light, float const *direction) override;
		void MaterialColor(uint32_t parameter, float const *color) override;
		void MaterialColor(float const *ambient, float const *diffuse, float const *specular,
		                   float const *emission, float shininess) override;

	private:
		template<typename... Fields>
		void Record(nSCDWire::Op op, Fields const &... fields) {
			writer.Begin(op);
			(writer.Put(fields), ...);
			writer.End();
			if constexpr (sizeof...(Fields) != 0) {
				if (IsState(op)) {
					uint8_t bytes[(sizeof(Fields) + ...)];
					size_t size = 0;
					((std::memcpy(bytes + size, &fields, sizeof(fields)), size += sizeof(fields)), ...);
					Remember(op, bytes, size);
				}
			} else if (IsState(op)) {
				Remember(op, nullptr, 0);
			}
		}

		// What a server started in place of one that stopped must be told before the game's next frame: the
		// video mode, the textures and buffer regions that exist, and the last call of each kind of state setter
		// (per capability, texture stage, matrix mode, light and parameter). Kept as the calls go by.
		static bool IsState(nSCDWire::Op op);
		void Remember(nSCDWire::Op op, uint8_t const *fields, size_t size);
		// After TakeRestartedOutput: writes the stream from the start again, to the new server.
		void Restart(std::unique_ptr<nSCDWire::Output> output);
		struct StateCall {
			uint64_t key = 0;
			// When it was last made: replayed in that order, as some calls change what others set. 0 for a free slot.
			uint64_t sequence = 0;
			nSCDWire::Op op{};
			// The texture stage or matrix mode the call acts on, or kAny.
			uint8_t stage = 0, mode = 0, size = 0;
			uint8_t fields[96];
		};
		uint64_t stateSequence = 0;
		static constexpr uint8_t kAny = 0xFF;
		// Open addressing by key, the fields inline: a state call costs no allocation and no tree walk. Grows at
		// half full; calls are never removed, as there is one per kind of state at most.
		std::vector<StateCall> stateCalls = std::vector<StateCall>(1024);
		size_t stateCallCount = 0;
		StateCall &StateCallFor(uint64_t key) {
			if ((stateCallCount + 1) * 2 > stateCalls.size()) {
				std::vector<StateCall> old(stateCalls.size() * 2);
				old.swap(stateCalls);
				stateCallCount = 0;
				for (StateCall const &call: old) {
					if (call.sequence != 0) {
						StateCall &moved = StateCallFor(call.key);
						if (moved.sequence == 0) ++stateCallCount;
						moved = call;
					}
				}
			}
			size_t const mask = stateCalls.size() - 1;
			for (size_t at = static_cast<size_t>((key * 0x9E3779B97F4A7C15ull) >> 32) & mask;; at = (at + 1) & mask) {
				StateCall &call = stateCalls[at];
				if (call.sequence == 0 || call.key == key) return call;
			}
		}
		struct TextureState {
			// Made by CreateTexture (with format, size, levels and hints), or by GenTextures and given an image by
			// TexImage2D (with its internal format, size, and the format and type of its pixels).
			bool created = false, image = false;
			uint32_t format = 0, width = 0, height = 0, levels = 0, hints = 0, sourceFormat = 0, sourceType = 0;
			bool live = false;
		};
		// By name: drivers hand out names counting up from 1, so a vector holds them without lookups.
		struct TextureTable {
			// Names past this are not kept (a driver handing out other names): a restart then lacks them.
			static constexpr uint32_t kMaxName = 1u << 22;
			std::vector<TextureState> slots;
			size_t live = 0;
			bool Has(uint32_t name) const { return name < slots.size() && slots[name].live; }
			TextureState *Find(uint32_t name) { return Has(name) ? &slots[name] : nullptr; }
			// A fresh state for name, or null for a name past kMaxName.
			TextureState *Make(uint32_t name) {
				if (name >= kMaxName) return nullptr;
				if (name >= slots.size()) slots.resize((std::max)(static_cast<size_t>(name) + 1, slots.size() * 2));
				if (!slots[name].live) ++live;
				slots[name] = TextureState{};
				slots[name].live = true;
				return &slots[name];
			}
			void Erase(uint32_t name) {
				if (!Has(name)) return;
				slots[name].live = false;
				--live;
			}
		};
		TextureTable textures;
		uint32_t boundTextures[2]{};
		// Buffer regions by number, with their type.
		std::map<uint32_t, int32_t> bufferRegions;
		bool initialized = false;
		struct {
			bool set = false;
			int32_t index = 0;
			bool show = false, fourth = false;
			sGDMode mode{};
		} videoMode;
		std::vector<std::wstring> gameFolders;

		// State setters the game repeats a lot, each with a slot that remembers its last recorded call. A call
		// that repeats it changes nothing on the other side and is not recorded. Calls that change the same
		// state another way forget the slot, so the next setter call is recorded again.
		enum Slot : uint32_t {
			kTexStageSlot,
			kTexParameterSlot, // 2 stages x 4 parameters
			kColorMultiplierSlot = kTexParameterSlot + 8,
			kAlphaMultiplierSlot,
			kVertexColorsSlot,
			kAlphaFuncSlot,
			kBlendFuncSlot,
			kDepthMaskSlot,
			kColorMaskSlot,
			kDepthFuncSlot,
			kShadeModelSlot,
			kPolygonOffsetSlot,
			kMatrixModeSlot,
			kMatrixSlot, // 2 matrix modes
			kCapabilitySlot = kMatrixSlot + 2, // kGDNumCapabilities, then Texture2D for the second stage
			kTexEnvSlot = kCapabilitySlot + kGDNumCapabilities + 1, // 2 stages
			kTexStageCoordSlot = kTexEnvSlot + 2, // 2 stages
			kTexStageMatrixSlot = kTexStageCoordSlot + 2, // 2 stages
			kTextureSlot = kTexStageMatrixSlot + 2, // 2 stages
			kSlotCount = kTextureSlot + 2,
		};
		struct SentState {
			uint8_t bytes[80];
			uint32_t size = 0;
			bool known = false;
		};

		// True when slot's last recorded call had the same op and fields. Otherwise remembers this one, which
		// the caller then records.
		template<typename... Fields>
		bool Unchanged(uint32_t slot, nSCDWire::Op op, Fields const &... fields) {
			constexpr uint32_t size = static_cast<uint32_t>(sizeof(op) + (sizeof(Fields) + ... + 0));
			static_assert(size <= sizeof(SentState::bytes));
			SentState &sent = sentState[slot];
			// Compared in place, field by field (each a compare of known size), and copied only when it changed.
			if (sent.known && sent.size == size) {
				uint32_t at = sizeof(op);
				bool same = std::memcmp(sent.bytes, &op, sizeof(op)) == 0;
				((same = same && std::memcmp(sent.bytes + at, &fields, sizeof(fields)) == 0, at += sizeof(fields)), ...);
				if (same) return true;
			}
			uint32_t at = sizeof(op);
			std::memcpy(sent.bytes, &op, sizeof(op));
			((std::memcpy(sent.bytes + at, &fields, sizeof(fields)), at += sizeof(fields)), ...);
			sent.size = size;
			sent.known = true;
			return false;
		}

		template<typename... Fields>
		void RecordState(uint32_t slot, nSCDWire::Op op, Fields const &... fields) {
			if (!Unchanged(slot, op, fields...)) Record(op, fields...);
		}
		void Forget(uint32_t slot, uint32_t count = 1) {
			for (uint32_t i = 0; i < count; ++i) sentState[slot + i].known = false;
		}
		// The lighting extension sets the lighting switch and colours the multipliers set too.
		void ForgetMultipliers() {
			Forget(kColorMultiplierSlot);
			Forget(kAlphaMultiplierSlot);
		}
		// Enable and Disable share a slot per capability. Texture2D is one per texture stage.
		void RecordSwitch(nSCDWire::Op op, uint32_t state) {
			if (state >= kGDNumCapabilities) {
				Record(op, state);
				return;
			}
			bool const secondStage = state == kGDCapability_Texture2D && activeStage == 1;
			RecordState(kCapabilitySlot + (secondStage ? kGDNumCapabilities : state), op, state);
		}

		struct Matrix {
			float v[16];
		};

		void RecordReservation(uint32_t name, uint32_t bytes);
		void RecordBlit(nSCDWire::BlitKind kind, int32_t x, int32_t y, int32_t width, int32_t height,
		                int32_t sourceWidth, int32_t sourceHeight, uint32_t format, uint32_t type,
		                void const *pixels, bool colorKeyed, void const *colorKey, uint32_t alpha);

		cGDriverBase *driver;
		nSCDWire::Writer writer;
		std::unique_ptr<RemoteRenderer> remote;

		uint32_t vertexFormat = 0;
		uint32_t vertexStride = 0;
		uint8_t const *vertexPointer = nullptr;
		int32_t unpackRowLength = 0;
		// The back buffer size the stream last told the other end about.
		int32_t recordedWidth = 0;
		int32_t recordedHeight = 0;
		// The vertex-buffer extension's current reservation, which the game fills between GetVertices and
		// ReleaseVertices and draws from until the next GetVertices.
		uint8_t const *reservation = nullptr;
		uint32_t reservationVertices = 0;
		std::vector<uint8_t> scratch;
		SentState sentState[kSlotCount];
		// The texture stage and matrix mode the other side has, which pick the slots of the setters that
		// act on them.
		uint32_t activeStage = 0;
		uint32_t matrixMode = 0;
	};

	// Lets nSCDWire::Replay drive a cGDriver: opens a windowed mode of the recorded size and refills the
	// terrain vertex buffer.
	class cGDriverReplayHooks : public nSCDWire::ReplayHooks {
	public:
		// A cGDriver, or a cGDriverWire in front of one to record the replay again.
		template<typename Driver>
		explicit cGDriverReplayHooks(Driver &driver) : driver(driver), vertexBuffers(driver) {}
		void VideoMode(bool show, uint32_t width, uint32_t height, bool fullscreen) override;
		void Vertices(uint32_t name, void const *data, uint32_t bytes) override;

	private:
		cIGZGDriver &driver;
		cIGZGDriverVertexBufferExtension &vertexBuffers;
	};
}
