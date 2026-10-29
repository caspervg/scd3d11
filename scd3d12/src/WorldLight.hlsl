// The world light's shaders (WorldLight.h). Ray queries need shader model 6.5, so DXC compiles these at build
// time (see CMakeLists.txt) and the server carries the bytecode.

// The driver's lighting constants, laid out as LightingConstants in cGDriver12.cpp.
cbuffer LightingConstants : register(b0)
{
	float4 camera;
	float4 sun;       // towards the sun in view space
	float4 sunColor;
	float4 ambient;
	float4 sc4Sun;
	float4 sc4Light;
	float4 march;
	float4 occluder;
	row_major float4x4 pixelToWorld;
	row_major float4x4 worldToView;
	float4 world;     // known, metres per pixel
	float4 terrain;
	float4 lights;
};

// The trace's own: the sun's radius (tangent of its angle), the longest ray, the offset from the surface in
// metres and in pixels, then how far from the surface the terrain starts to cast. For the emitter pass, the mesh
// to read.
cbuffer Job : register(b1)
{
	uint4 job0;
	uint4 job1;
};

Texture2D<float4> lightLayer : register(t1);
Texture2D<float> sceneDepth : register(t2);
RaytracingAccelerationStructure city : register(t7);

// Per instance in the city structure, by its instance id: what an alpha-tested mesh needs to test a hit.
struct Mesh
{
	uint texture;       // the texture's slot in the view heap
	uint buffer;        // the slot of the raw view of the buffer that holds the vertices and indices
	uint vertices;      // byte offsets in it
	uint indices;
	float reference;    // the alpha a texel needs to be solid
	uint3 padding;
};
StructuredBuffer<Mesh> meshes : register(t8);
Texture2D<float4> textures[] : register(t0, space1);
ByteAddressBuffer buffers[] : register(t0, space2);
SamplerState textureSampler : register(s0);

RWStructuredBuffer<float4> emitters : register(u0);

// Instance masks in the city structure, as in WorldLight.cpp.
static const uint kModels = 1;
static const uint kTerrain = 2;

// The driver's vertex: position, normal, colour, two texture coordinates (D3D11Vertex).
static const uint kVertexBytes = 44;
static const uint kUvOffset = 28;

uint3 Triangle(ByteAddressBuffer buffer, uint offset, uint index)
{
	// Three 16-bit indices from a 4-byte aligned buffer.
	uint first = offset + index * 6;
	uint2 words = uint2(buffer.Load(first & ~3u), buffer.Load((first & ~3u) + 4));
	uint3 result;
	if ((first & 2) == 0) result = uint3(words.x & 0xFFFF, words.x >> 16, words.y & 0xFFFF);
	else result = uint3(words.x >> 16, words.y & 0xFFFF, words.y >> 16);
	return result;
}

float2 Uv(ByteAddressBuffer buffer, uint offset, uint vertex)
{
	return asfloat(buffer.Load2(offset + vertex * kVertexBytes + kUvOffset));
}

float3 Position(ByteAddressBuffer buffer, uint offset, uint vertex)
{
	return asfloat(buffer.Load3(offset + vertex * kVertexBytes));
}

float3 WorldAt(int2 pixel)
{
	float4 at = mul(pixelToWorld, float4(float2(pixel) + 0.5f, sceneDepth.Load(int3(pixel, 0)), 1.0f));
	return at.xyz / at.w;
}

// Of the two neighbours along one axis, the step to the one on the same surface: the shorter one.
float3 SurfaceStep(int2 pixel, float3 centre, int2 axis)
{
	float3 forward = WorldAt(pixel + axis) - centre;
	float3 backward = centre - WorldAt(pixel - axis);
	return dot(forward, forward) < dot(backward, backward) ? forward : backward;
}

float4 FullScreenVS(uint id : SV_VertexID) : SV_POSITION
{
	float2 uv = float2((id << 1) & 2, id & 2);
	return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

// How much of the sun each city pixel sees, from a ray through the city towards it. Each pixel of a 4x4 tile
// aims at another point of the sun's disc, which the relight's blur averages into a soft edge.
float TraceSunPS(float4 position : SV_POSITION) : SV_TARGET
{
	int2 pixel = int2(position.xy);
	if (lightLayer.Load(int3(pixel, 0)).a <= 0.0f || world.x == 0.0f) return 1.0f;
	float depth = sceneDepth.Load(int3(pixel, 0));
	if (depth >= 1.0f) return 1.0f;
	float3 centre = WorldAt(pixel);
	float3x3 toView = (float3x3)worldToView;
	float3 towardsSun = normalize(mul(sun.xyz, toView));
	float3 normal = normalize(cross(SurfaceStep(pixel, centre, int2(1, 0)), SurfaceStep(pixel, centre, int2(0, 1))));
	// Towards the camera, which looks down view -z. A pixel without neighbours on its surface takes that.
	if (any(isnan(normal))) normal = toView[2];
	if (dot(normal, toView[2]) < 0.0f) normal = -normal;
	if (dot(normal, towardsSun) <= 0.0f) return 1.0f;

	static const float bayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
	float sample = bayer[(pixel.y & 3) * 4 + (pixel.x & 3)];
	float radius = sqrt((sample + 0.5f) / 16.0f) * asfloat(job0.x);
	float angle = sample * 2.39996323f;
	float3 side = normalize(cross(towardsSun, abs(towardsSun.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0)));
	float3 other = cross(towardsSun, side);
	RayDesc ray;
	ray.Direction = normalize(towardsSun + (side * cos(angle) + other * sin(angle)) * radius);
	ray.Origin = centre + normal * (asfloat(job0.z) + asfloat(job0.w) * world.y);
	ray.TMin = 0.0f;
	ray.TMax = asfloat(job0.y);

	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES> query;
	query.TraceRayInline(city, RAY_FLAG_NONE, kModels, ray);
	// A ray through a forest meets many cut-out leaves: past this many it counts as lit, so no frame can take
	// long enough to stall the GPU.
	uint candidates = 0;
	while (query.Proceed())
	{
		if (++candidates > 64)
		{
			query.Abort();
			break;
		}
		// Only alpha-tested meshes come here: the texel's alpha decides.
		Mesh mesh = meshes[query.CandidateInstanceID()];
		ByteAddressBuffer buffer = buffers[NonUniformResourceIndex(mesh.buffer)];
		uint3 corners = Triangle(buffer, mesh.indices, query.CandidatePrimitiveIndex());
		float2 b = query.CandidateTriangleBarycentrics();
		float2 uv = Uv(buffer, mesh.vertices, corners.x) * (1.0f - b.x - b.y) + Uv(buffer, mesh.vertices, corners.y) * b.x +
		            Uv(buffer, mesh.vertices, corners.z) * b.y;
		float alpha = textures[NonUniformResourceIndex(mesh.texture)].SampleLevel(textureSampler, uv, 0).a;
		if (alpha > mesh.reference) query.CommitNonOpaqueTriangleHit();
	}
	if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT) return 0.0f;
	// The terrain's structure is a 16 metre grid that does not follow every surface drawn on it, so near the
	// pixel it would shade the ground it stands for: only hills further off cast.
	ray.TMin = asfloat(job1.x);
	RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH | RAY_FLAG_SKIP_PROCEDURAL_PRIMITIVES | RAY_FLAG_FORCE_OPAQUE> ground;
	ground.TraceRayInline(city, RAY_FLAG_NONE, kTerrain, ray);
	ground.Proceed();
	return ground.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0f : 1.0f;
}

// Where a mesh glows at night, from its night texture: the emitted light of every triangle, gathered by the
// axis its face looks along (six bins, +x -x +y -y +z -z in model space). Per bin two float4 go out: the
// glow-weighted sum of positions with the weight, and the emitted colour summed over the area.
//   job0: texture slot, buffer slot, vertex offset, index offset
//   job1: index count, first output element
groupshared float4 gathered[64][12];

[numthreads(64, 1, 1)]
void EmissiveCS(uint thread : SV_GroupIndex)
{
	float4 sums[12];
	[unroll] for (uint s = 0; s < 12; ++s) sums[s] = 0.0f;
	Texture2D<float4> night = textures[job0.x];
	ByteAddressBuffer buffer = buffers[job0.y];
	uint width, height;
	night.GetDimensions(width, height);
	for (uint t = thread; t * 3 + 2 < job1.x; t += 64)
	{
		uint3 corners = Triangle(buffer, job0.w, t);
		float3 p0 = Position(buffer, job0.z, corners.x), p1 = Position(buffer, job0.z, corners.y),
		       p2 = Position(buffer, job0.z, corners.z);
		float2 u0 = Uv(buffer, job0.z, corners.x), u1 = Uv(buffer, job0.z, corners.y), u2 = Uv(buffer, job0.z, corners.z);
		float3 facing = cross(p1 - p0, p2 - p0);
		float area = length(facing) * 0.5f;
		if (area <= 0.0f) continue;
		// Sixteen samples spread over the triangle, from the mip level whose texels they about cover.
		float texels = abs((u1.x - u0.x) * (u2.y - u0.y) - (u2.x - u0.x) * (u1.y - u0.y)) * 0.5f * width * height;
		float level = max(0.0f, 0.5f * log2(max(texels / 16.0f, 1.0f)));
		float3 glow = 0.0f;
		[loop] for (uint i = 0; i < 4; ++i)
		{
			[loop] for (uint j = 0; j < 4 - i; ++j)
			{
				float a = (i + 0.33f) / 4.0f, b = (j + 0.33f) / 4.0f;
				float4 texel = night.SampleLevel(textureSampler, u0 + (u1 - u0) * a + (u2 - u0) * b, level);
				glow += texel.rgb * texel.a;
			}
		}
		glow *= area / 10.0f;
		float weight = dot(glow, float3(0.3f, 0.59f, 0.11f));
		if (weight <= 0.0f) continue;
		float3 n = abs(facing);
		uint axis = n.x >= n.y && n.x >= n.z ? 0 : (n.y >= n.z ? 1 : 2);
		uint bin = axis * 2 + (facing[axis] < 0.0f ? 1 : 0);
		float3 centre = (p0 + p1 + p2) / 3.0f;
		sums[bin * 2] += float4(centre * weight, weight);
		sums[bin * 2 + 1] += float4(glow, area);
	}
	[unroll] for (uint k = 0; k < 12; ++k) gathered[thread][k] = sums[k];
	GroupMemoryBarrierWithGroupSync();
	if (thread < 12)
	{
		float4 total = 0.0f;
		for (uint i = 0; i < 64; ++i) total += gathered[i][thread];
		emitters[job1.y + thread] = total;
	}
}
