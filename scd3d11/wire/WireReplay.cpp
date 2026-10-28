/*
 *  SCD3D11 - a free Direct3D 11 driver for SimCity 4's SimGL interface
 *  Copyright (C) 2026
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation, under
 *  version 2.1 of the License, or (at your option) any later version.
 */

#include "WireReplay.h"

#include "cGDCombiner.h"
#include "cIGZGDriver.h"
#include "cIGZGBufferRegionExtension.h"
#include "cIGZGDriverLightingExtension.h"
#include "cIGZGDriverVertexBufferExtension.h"

#include <unordered_map>
#include <vector>

namespace nSCDWire {
	namespace {
		struct Float4 {
			float v[4];
		};

		// Recorded names to the target's own, for textures and buffer regions.
		class NameMap {
		public:
			uint32_t operator()(uint32_t recorded) const {
				auto const found = names.find(recorded);
				return found == names.end() ? recorded : found->second;
			}
			void Set(uint32_t recorded, uint32_t actual) { names[recorded] = actual; }
			void Erase(uint32_t recorded) { names.erase(recorded); }

		private:
			std::unordered_map<uint32_t, uint32_t> names;
		};

		template<typename T>
		T *Query(cIGZGDriver &driver, uint32_t iid) {
			void *pointer = nullptr;
			return driver.QueryInterface(iid, &pointer) ? static_cast<T *>(pointer) : nullptr;
		}
	}

	bool Replay(Reader &reader, cIGZGDriver &driver, ReplayHooks &hooks, ReplayStats *stats, char const **error) {
		auto *regions = Query<cIGZGBufferRegionExtension>(driver, GZIID_cIGZGBufferRegionExtension);
		auto *lighting = Query<cIGZGDriverLightingExtension>(driver, GZIID_cIGZGDriverLightingExtension);
		auto *vertexBuffers = Query<cIGZGDriverVertexBufferExtension>(driver, GZIID_cIGZGDriverVertexBufferExtension);
		NameMap textures;
		NameMap regionNames;
		std::vector<uint8_t> scratch;
		char const *failure = nullptr;

		Op op{};
		Fields f(nullptr, 0);
		while (failure == nullptr && reader.Next(op, f)) {
			if (stats != nullptr) {
				++stats->records;
				++stats->opCounts[static_cast<size_t>(op)];
			}
			auto const blob = [&](BlobKey const &key) -> std::vector<uint8_t> const * {
				std::vector<uint8_t> const *found = reader.FindBlob(key);
				if (found == nullptr) failure = "a record refers to a blob the stream never sent";
				return found;
			};

			switch (op) {
				case Op::Frame:
					driver.Flush();
					if (stats != nullptr) ++stats->frames;
					if (!hooks.Frame()) return true;
					break;

				case Op::DrawArrays: {
					auto const format = f.Get<uint32_t>();
					auto const stride = f.Get<uint32_t>();
					auto const primitive = f.Get<uint32_t>();
					auto const count = f.Get<int32_t>();
					auto const *vertices = blob(f.Get<BlobKey>());
					if (!f.ok || vertices == nullptr || vertices->empty()) break;
					driver.InterleavedArrays(format, static_cast<int32_t>(stride), vertices->data());
					driver.DrawArrays(primitive, 0, count);
					break;
				}
				case Op::DrawElements: {
					auto const format = f.Get<uint32_t>();
					auto const stride = f.Get<uint32_t>();
					auto const primitive = f.Get<uint32_t>();
					auto const count = f.Get<int32_t>();
					auto const type = f.Get<uint32_t>();
					auto const *vertices = blob(f.Get<BlobKey>());
					auto const *indices = blob(f.Get<BlobKey>());
					if (!f.ok || vertices == nullptr || indices == nullptr || vertices->empty()) break;
					driver.InterleavedArrays(format, static_cast<int32_t>(stride), vertices->data());
					driver.DrawElements(primitive, count, type, indices->data());
					break;
				}

				case Op::Clear: driver.Clear(f.Get<uint32_t>()); break;
				case Op::ClearColor: {
					auto const c = f.Get<Float4>();
					driver.ClearColor(c.v[0], c.v[1], c.v[2], c.v[3]);
					break;
				}
				case Op::ClearDepth: driver.ClearDepth(f.Get<double>()); break;
				case Op::ClearStencil: driver.ClearStencil(f.Get<int32_t>()); break;
				case Op::ColorMask: driver.ColorMask(f.Get<bool>()); break;
				case Op::DepthFunc: driver.DepthFunc(f.Get<uint32_t>()); break;
				case Op::DepthMask: driver.DepthMask(f.Get<bool>()); break;
				case Op::StencilFunc: {
					auto const function = f.Get<uint32_t>();
					auto const reference = f.Get<int32_t>();
					driver.StencilFunc(function, reference, f.Get<uint32_t>());
					break;
				}
				case Op::StencilMask: driver.StencilMask(f.Get<uint32_t>()); break;
				case Op::StencilOp: {
					auto const fail = f.Get<uint32_t>();
					auto const depthFail = f.Get<uint32_t>();
					driver.StencilOp(fail, depthFail, f.Get<uint32_t>());
					break;
				}
				case Op::BlendFunc: {
					auto const source = f.Get<uint32_t>();
					driver.BlendFunc(source, f.Get<uint32_t>());
					break;
				}
				case Op::AlphaFunc: {
					auto const function = f.Get<uint32_t>();
					driver.AlphaFunc(function, f.Get<float>());
					break;
				}
				case Op::ShadeModel: driver.ShadeModel(f.Get<uint32_t>()); break;

				case Op::BindTexture: {
					auto const target = f.Get<uint32_t>();
					driver.BindTexture(target, textures(f.Get<uint32_t>()));
					break;
				}
				case Op::TexImage2D: {
					auto const target = f.Get<uint32_t>();
					auto const level = f.Get<int32_t>();
					auto const internalFormat = f.Get<int32_t>();
					auto const width = f.Get<int32_t>();
					auto const height = f.Get<int32_t>();
					auto const border = f.Get<int32_t>();
					auto const format = f.Get<uint32_t>();
					auto const type = f.Get<uint32_t>();
					auto const hasPixels = f.Get<bool>();
					auto const *pixels = blob(f.Get<BlobKey>());
					if (!f.ok || pixels == nullptr) break;
					driver.TexImage2D(target, level, internalFormat, width, height, border, format, type,
					                  hasPixels && !pixels->empty() ? pixels->data() : nullptr);
					break;
				}
				case Op::PixelStore: {
					auto const parameter = f.Get<uint32_t>();
					driver.PixelStore(parameter, f.Get<int32_t>());
					break;
				}
				case Op::TexEnvInt: {
					auto const target = f.Get<uint32_t>();
					auto const parameter = f.Get<uint32_t>();
					driver.TexEnv(target, parameter, f.Get<int32_t>());
					break;
				}
				case Op::TexEnvFloat: {
					auto const target = f.Get<uint32_t>();
					auto const parameter = f.Get<uint32_t>();
					auto const present = f.Get<bool>();
					auto const value = f.Get<Float4>();
					driver.TexEnv(target, parameter, present ? value.v : nullptr);
					break;
				}
				case Op::TexParameter: {
					auto const target = f.Get<uint32_t>();
					auto const parameter = f.Get<uint32_t>();
					driver.TexParameter(target, parameter, f.Get<int32_t>());
					break;
				}
				case Op::FogInt: {
					auto const parameter = f.Get<uint32_t>();
					driver.Fog(parameter, f.Get<uint32_t>());
					break;
				}
				case Op::FogFloat: {
					auto const parameter = f.Get<uint32_t>();
					auto const present = f.Get<bool>();
					auto const value = f.Get<Float4>();
					driver.Fog(parameter, present ? value.v : nullptr);
					break;
				}
				case Op::ColorMultiplier: {
					auto const r = f.Get<float>();
					auto const g = f.Get<float>();
					driver.ColorMultiplier(r, g, f.Get<float>());
					break;
				}
				case Op::AlphaMultiplier: driver.AlphaMultiplier(f.Get<float>()); break;
				case Op::EnableVertexColors: {
					auto const ambient = f.Get<bool>();
					driver.EnableVertexColors(ambient, f.Get<bool>());
					break;
				}
				case Op::GenTextures: {
					auto const count = f.Get<int32_t>();
					uint8_t const *names = f.Bytes(static_cast<size_t>(count > 0 ? count : 0) * sizeof(uint32_t));
					if (!f.ok || count <= 0) break;
					std::vector<uint32_t> recorded(count), actual(count);
					std::memcpy(recorded.data(), names, recorded.size() * sizeof(uint32_t));
					driver.GenTextures(count, actual.data());
					for (int32_t i = 0; i < count; ++i) textures.Set(recorded[i], actual[i]);
					break;
				}
				case Op::DeleteTextures: {
					auto const count = f.Get<int32_t>();
					uint8_t const *names = f.Bytes(static_cast<size_t>(count > 0 ? count : 0) * sizeof(uint32_t));
					if (!f.ok || count <= 0) break;
					std::vector<uint32_t> mapped(count);
					for (int32_t i = 0; i < count; ++i) {
						uint32_t recorded;
						std::memcpy(&recorded, names + i * sizeof(uint32_t), sizeof(recorded));
						mapped[i] = textures(recorded);
						textures.Erase(recorded);
					}
					driver.DeleteTextures(count, mapped.data());
					break;
				}

				case Op::MatrixMode: driver.MatrixMode(f.Get<uint32_t>()); break;
				case Op::LoadMatrix: {
					uint8_t const *matrix = f.Bytes(16 * sizeof(float));
					if (!f.ok) break;
					float values[16];
					std::memcpy(values, matrix, sizeof(values));
					driver.LoadMatrix(values);
					break;
				}
				case Op::LoadIdentity: driver.LoadIdentity(); break;
				case Op::Enable: driver.Enable(f.Get<uint32_t>()); break;
				case Op::Disable: driver.Disable(f.Get<uint32_t>()); break;

				case Op::TexStage: driver.TexStage(f.Get<uint32_t>()); break;
				case Op::TexStageCoord: driver.TexStageCoord(f.Get<uint32_t>()); break;
				case Op::TexStageMatrix: {
					auto const present = f.Get<bool>();
					auto const rows = f.Get<uint32_t>();
					auto const columns = f.Get<uint32_t>();
					auto const flags = f.Get<uint32_t>();
					float values[16]{};
					if (present) {
						uint8_t const *matrix = f.Bytes(sizeof(values));
						if (f.ok) std::memcpy(values, matrix, sizeof(values));
					}
					if (f.ok) driver.TexStageMatrix(present ? values : nullptr, rows, columns, flags);
					break;
				}
				case Op::TexStageCombineMode: {
					auto const parameter = f.Get<uint32_t>();
					driver.TexStageCombine(static_cast<eGDTextureStageCombineParamType>(parameter),
					                       static_cast<eGDTextureStageCombineModeParam>(f.Get<uint32_t>()));
					break;
				}
				case Op::TexStageCombineSource: {
					auto const parameter = f.Get<uint32_t>();
					driver.TexStageCombine(static_cast<eGDTextureStageCombineSourceParamType>(parameter),
					                       static_cast<eGDTextureStageCombineSourceParam>(f.Get<uint32_t>()));
					break;
				}
				case Op::TexStageCombineOperand: {
					auto const parameter = f.Get<uint32_t>();
					driver.TexStageCombine(static_cast<eGDTextureStageCombineOperandType>(parameter),
					                       static_cast<eGDBlend>(f.Get<uint32_t>()));
					break;
				}
				case Op::TexStageCombineScale: {
					auto const parameter = f.Get<uint32_t>();
					driver.TexStageCombine(static_cast<eGDTextureStageCombineScaleParamType>(parameter),
					                       static_cast<eGDTextureStageCombineScaleParam>(f.Get<uint32_t>()));
					break;
				}
				case Op::SetTexture: {
					auto const texture = f.Get<uint32_t>();
					driver.SetTexture(textures(texture), f.Get<uint32_t>());
					break;
				}
				case Op::CreateTexture: {
					auto const internalFormat = f.Get<uint32_t>();
					auto const width = f.Get<uint32_t>();
					auto const height = f.Get<uint32_t>();
					auto const levels = f.Get<uint32_t>();
					auto const hints = f.Get<uint32_t>();
					auto const recorded = f.Get<uint32_t>();
					if (!f.ok) break;
					intptr_t const actual = driver.CreateTexture(internalFormat, width, height, levels, hints);
					if (recorded != 0) textures.Set(recorded, static_cast<uint32_t>(actual));
					break;
				}
				case Op::LoadTextureLevel: {
					auto const texture = f.Get<uint32_t>();
					auto const level = f.Get<int32_t>();
					auto const x = f.Get<int32_t>();
					auto const y = f.Get<int32_t>();
					auto const width = f.Get<int32_t>();
					auto const height = f.Get<int32_t>();
					auto const format = f.Get<uint32_t>();
					auto const type = f.Get<uint32_t>();
					auto const rowLength = f.Get<uint32_t>();
					auto const hasPixels = f.Get<bool>();
					auto const *pixels = blob(f.Get<BlobKey>());
					if (!f.ok || pixels == nullptr) break;
					driver.LoadTextureLevel(textures(texture), level, x, y, width, height, format, type, rowLength,
					                        hasPixels && !pixels->empty() ? pixels->data() : nullptr);
					break;
				}
				case Op::SetCombiner: {
					uint8_t const *bytes = f.Bytes(sizeof(cGDCombiner));
					auto const stage = f.Get<uint32_t>();
					if (!f.ok) break;
					cGDCombiner combiner;
					std::memcpy(&combiner, bytes, sizeof(combiner));
					driver.SetCombiner(combiner, stage);
					break;
				}
				case Op::SetVideoMode: {
					f.Get<int32_t>(); // the recorded mode index
					auto const show = f.Get<bool>();
					f.Get<bool>();
					auto const width = f.Get<uint32_t>();
					auto const height = f.Get<uint32_t>();
					auto const fullscreen = f.Get<bool>();
					if (f.ok) hooks.VideoMode(show, width, height, fullscreen);
					break;
				}
				case Op::PolygonOffset: driver.PolygonOffset(f.Get<int32_t>()); break;
				case Op::Blit: {
					auto const kind = f.Get<BlitKind>();
					auto const x = f.Get<int32_t>();
					auto const y = f.Get<int32_t>();
					auto const width = f.Get<int32_t>();
					auto const height = f.Get<int32_t>();
					auto const sourceWidth = f.Get<int32_t>();
					auto const sourceHeight = f.Get<int32_t>();
					auto const format = f.Get<uint32_t>();
					auto const type = f.Get<uint32_t>();
					auto const keyed = f.Get<bool>();
					auto const key = f.Get<uint32_t>();
					auto const alpha = f.Get<uint32_t>();
					auto const *pixels = blob(f.Get<BlobKey>());
					if (!f.ok || pixels == nullptr || pixels->empty()) break;
					void const *data = pixels->data();
					switch (kind) {
						case BlitKind::BitBlt:
							driver.BitBlt(x, y, width, height, format, type, data, keyed, &key);
							break;
						case BlitKind::StretchBlt:
							driver.StretchBlt(x, y, width, height, sourceWidth, sourceHeight, format, type, data, keyed,
							                  &key);
							break;
						case BlitKind::BitBltAlpha:
							driver.BitBltAlpha(x, y, width, height, format, type, data, keyed, &key, alpha);
							break;
						case BlitKind::StretchBltAlpha:
							driver.StretchBltAlpha(x, y, width, height, sourceWidth, sourceHeight, format, type, data,
							                       keyed, &key, alpha);
							break;
						case BlitKind::BitBltAlphaModulate:
							driver.BitBltAlphaModulate(x, y, width, height, format, type, data, keyed, &key, alpha);
							break;
						case BlitKind::StretchBltAlphaModulate:
							driver.StretchBltAlphaModulate(x, y, width, height, sourceWidth, sourceHeight, format, type,
							                               data, keyed, &key, alpha);
							break;
					}
					break;
				}
				case Op::SetViewportFull: driver.SetViewport(); break;
				case Op::SetViewport: {
					auto const x = f.Get<int32_t>();
					auto const y = f.Get<int32_t>();
					auto const width = f.Get<int32_t>();
					driver.SetViewport(x, y, width, f.Get<int32_t>());
					break;
				}
				case Op::Init: driver.Init(); break;
				case Op::Shutdown: driver.Shutdown(); break;
				case Op::PuntBlitSourceAlpha: {
					bool value = f.Get<bool>();
					driver.Punt(0x6C4236E7, &value);
					break;
				}

				case Op::NewBufferRegion: {
					auto const type = f.Get<int32_t>();
					auto const recorded = f.Get<uint32_t>();
					if (f.ok && regions != nullptr) regionNames.Set(recorded, regions->NewBufferRegion(type));
					break;
				}
				case Op::DeleteBufferRegion: {
					auto const recorded = f.Get<int32_t>();
					if (regions != nullptr) regions->DeleteBufferRegion(static_cast<int32_t>(regionNames(recorded)));
					regionNames.Erase(static_cast<uint32_t>(recorded));
					break;
				}
				case Op::ReadBufferRegion:
				case Op::DrawBufferRegion: {
					auto const region = regionNames(f.Get<uint32_t>());
					int32_t a[6];
					for (int32_t &value: a) value = f.Get<int32_t>();
					if (!f.ok || regions == nullptr) break;
					if (op == Op::ReadBufferRegion) regions->ReadBufferRegion(region, a[0], a[1], a[2], a[3], a[4], a[5]);
					else regions->DrawBufferRegion(region, a[0], a[1], a[2], a[3], a[4], a[5]);
					break;
				}
				case Op::DeleteAllBufferRegions:
					if (regions != nullptr) regions->DeleteAllBufferRegions();
					break;

				case Op::EnableLighting:
					if (lighting != nullptr) lighting->EnableLighting(f.Get<bool>());
					break;
				case Op::EnableLight: {
					auto const light = f.Get<uint32_t>();
					if (lighting != nullptr) lighting->EnableLight(light, f.Get<bool>());
					break;
				}
				case Op::LightModelAmbient: {
					auto const c = f.Get<Float4>();
					if (lighting != nullptr) lighting->LightModelAmbient(c.v[0], c.v[1], c.v[2], c.v[3]);
					break;
				}
				case Op::LightColor: {
					auto const light = f.Get<uint32_t>();
					auto const parameter = f.Get<uint32_t>();
					auto const present = f.Get<bool>();
					auto const c = f.Get<Float4>();
					if (lighting != nullptr) lighting->LightColor(light, parameter, present ? c.v : nullptr);
					break;
				}
				case Op::LightColors: {
					auto const light = f.Get<uint32_t>();
					Float4 c[3];
					bool present[3];
					for (int i = 0; i < 3; ++i) {
						present[i] = f.Get<bool>();
						c[i] = f.Get<Float4>();
					}
					if (lighting != nullptr) {
						lighting->LightColor(light, present[0] ? c[0].v : nullptr, present[1] ? c[1].v : nullptr,
						                     present[2] ? c[2].v : nullptr);
					}
					break;
				}
				case Op::LightPosition:
				case Op::LightDirection: {
					auto const light = f.Get<uint32_t>();
					auto const present = f.Get<bool>();
					auto const c = f.Get<Float4>();
					if (lighting == nullptr) break;
					if (op == Op::LightPosition) lighting->LightPosition(light, present ? c.v : nullptr);
					else lighting->LightDirection(light, present ? c.v : nullptr);
					break;
				}
				case Op::MaterialColor: {
					auto const parameter = f.Get<uint32_t>();
					auto const present = f.Get<bool>();
					auto const c = f.Get<Float4>();
					if (lighting != nullptr) lighting->MaterialColor(parameter, present ? c.v : nullptr);
					break;
				}
				case Op::MaterialColors: {
					Float4 c[4];
					bool present[4];
					for (int i = 0; i < 4; ++i) {
						present[i] = f.Get<bool>();
						c[i] = f.Get<Float4>();
					}
					auto const shininess = f.Get<float>();
					if (lighting != nullptr) {
						lighting->MaterialColor(present[0] ? c[0].v : nullptr, present[1] ? c[1].v : nullptr,
						                        present[2] ? c[2].v : nullptr, present[3] ? c[3].v : nullptr,
						                        shininess);
					}
					break;
				}

				case Op::GetVertices:
				case Op::ContinueVertices:
					// The reservation's contents arrive with ReleaseVertices.
					break;
				case Op::ReleaseVertices:
					// Each draw carries the vertices it reads.
					break;
				case Op::DrawPrims: {
					auto const name = f.Get<uint32_t>();
					auto const primitive = f.Get<uint32_t>();
					auto const bytes = f.Get<uint32_t>();
					auto const *vertices = blob(f.Get<BlobKey>());
					if (!f.ok || vertices == nullptr || vertexBuffers == nullptr) break;
					hooks.Vertices(name, vertices->data(), static_cast<uint32_t>(vertices->size()));
					vertexBuffers->DrawPrims(name, primitive, nullptr, bytes);
					break;
				}
				case Op::DrawPrimsIndexed: {
					auto const name = f.Get<uint32_t>();
					auto const primitive = f.Get<uint32_t>();
					auto const count = f.Get<uint32_t>();
					auto const *indices = blob(f.Get<BlobKey>());
					auto const *vertices = blob(f.Get<BlobKey>());
					if (!f.ok || indices == nullptr || vertices == nullptr || vertexBuffers == nullptr) break;
					hooks.Vertices(name, vertices->data(), static_cast<uint32_t>(vertices->size()));
					scratch.assign(indices->begin(), indices->end());
					vertexBuffers->DrawPrimsIndexed(name, primitive, count,
					                                scratch.empty() ? nullptr : reinterpret_cast<uint16_t *>(scratch.data()));
					break;
				}
				case Op::ResetVertexBuffers:
					if (vertexBuffers != nullptr) vertexBuffers->Reset();
					break;

				case Op::CopyColorBuffer: {
					auto const x = f.Get<int32_t>();
					auto const y = f.Get<int32_t>();
					auto const width = f.Get<int32_t>();
					auto const height = f.Get<int32_t>();
					if (f.ok) hooks.CopyColorBuffer(x, y, width, height);
					break;
				}

				case Op::OccupantInsert:
				case Op::OccupantMove: {
					OccupantEvent event{op == Op::OccupantInsert ? OccupantEventKind::Insert : OccupantEventKind::Move};
					event.id = f.Get<uint64_t>();
					event.type = f.Get<int32_t>();
					for (float &value: event.box) value = f.Get<float>();
					if (f.ok) hooks.Occupant(event);
					break;
				}
				case Op::OccupantRemove: {
					OccupantEvent event{OccupantEventKind::Remove};
					event.id = f.Get<uint64_t>();
					if (f.ok) hooks.Occupant(event);
					break;
				}

				case Op::OwnerBegin: {
					auto const owner = f.Get<DrawOwner>();
					if (f.ok) hooks.OwnerBegin(owner);
					break;
				}
				case Op::OwnerEnd: hooks.OwnerEnd(); break;
				case Op::ModelInstance: {
					auto const model = f.Get<nSCDWire::ModelInstance>();
					if (f.ok) hooks.ModelInstance(model);
					break;
				}

				default:
					failure = "an op the replay does not handle";
					break;
			}
			if (failure == nullptr && !f.ok) failure = "a record is shorter than its op needs";
		}

		if (regions != nullptr) regions->Release();
		if (lighting != nullptr) lighting->Release();
		if (vertexBuffers != nullptr) vertexBuffers->Release();
		if (failure == nullptr && *reader.Error() != '\0') failure = reader.Error();
		if (error != nullptr) *error = failure != nullptr ? failure : "";
		return failure == nullptr;
	}
}
