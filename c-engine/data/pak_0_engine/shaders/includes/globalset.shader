#extension GL_EXT_shader_image_load_formatted : enable

#define MAX_SAMPLERS 11
#define MAX_IMAGES 4096

#define SLOT_SAMPLER 0
#define SLOT_IMAGE 1
#define SLOT_CUBE_IMAGE 2
#define SLOT_STORAGE_IMAGE 3
#define SLOT_ADDRESS_BUFFER 4

#define SAMPLER_NEAREST 0
#define SAMPLER_LINEAR 1
#define SAMPLER_NORMALMAP 3
#define SAMPLER_SPLATMAP 4
#define SAMPLER_BORDER 5
#define SAMPLER_BORDER_NEAREST 6
#define SAMPLER_SHADOW_CMP 7
#define SAMPLER_CLAMP_LINEAR 8
#define SAMPLER_CLAMP_NEAREST 9
#define SAMPLER_CLAMP_LINEAR_MIPMAP 10

struct Transform {
    vec4 rot;
    vec4 pos; // last element is scale
};

layout(buffer_reference, std430) buffer TransformBufferRef {
    Transform transforms[];
};

struct Camera {
    mat4 view;
    mat4 projection;
    mat4 viewProjection;
    mat4 invViewProjection;
    mat4 invView;
    mat4 invProjection;
    mat4 viewProjectionNoJitter;
    mat4 invViewProjectionNoJitter;
    mat4 prevViewProjection;
    mat4 prevViewProjectionNoJitter;
    vec4 frustumPlanes[6];
    vec4 position;
    vec4 direction;
    vec2 viewport;
    uint frameIndex;
    float zNear;
    float zFar;
    float exposure;  /* scene exposure multiplier applied before tonemapping */
    float jitterX;   /* TAA sub-pixel jitter in UV space */
    float jitterY;
    float prevJitterX;
    float prevJitterY;
    float _pad1[4];
    float _pad2[4];
};

struct DirectionalLight {
    vec4 direction;   // xyz: direction, w: intensity
    vec4 color;       // rgb: color, w: unused
    vec4 ambient;     // rgb: ambient color, w: unused
    vec4 padding;     // for std430 alignment
};

#define MAX_GPU_LIGHTS 1024

/* 64 bytes, fully vec4-aligned */
struct GpuLight {
    vec4 positionAndRange;   // xyz: world pos, w: range (0 = infinite)
    vec4 directionAndType;   // xyz: direction, w: type (0=dir,1=point,2=spot)
    vec4 colorAndIntensity;  // rgb: color, w: intensity
    vec4 spotAngles;         // x: cos(inner), y: cos(outer), zw: unused
};

#define SHADOW_CASCADE_COUNT 4

struct ShadowData {
    mat4 shadowViewProjection[SHADOW_CASCADE_COUNT];
    vec4 cascadeSplits;         // view-space far plane of each cascade
    vec4 shadowParams;          // x: bias, y: normalBias, z: mapSize, w: 1/mapSize
    uint shadowMapIndex[SHADOW_CASCADE_COUNT];
    uint cascadeCount;
    float lightSize;            // unused (PCSS removed), kept for struct alignment
    uint temporalActive;         // 1 when temporal upscaler (FSR) is active — enables temporal PCF noise
    uint contactShadowImageIndex;
    uint shadowPad3;
    uint shadowPad4;
    uint shadowPad5;
};

// Must match VulkanFogData in VulkanResourceManager.h (std430 layout).

#define FOG_TYPE_NONE        0
#define FOG_TYPE_LINEAR      1
#define FOG_TYPE_EXPONENTIAL 2
#define FOG_TYPE_EXP2        3

struct FogData {
    // Base fog (screen-space)
    vec4  fogColor;            // rgb: fog color, w: unused
    float fogDensity;          // density parameter
    float fogStartDistance;    // distance at which fog begins to appear
    float fogMaxDistance;      // linear fog end distance
    int   fogType;             // FOG_TYPE_*

    // Height fog
    float fogHeightBase;       // y-coordinate where fog is densest
    float fogHeightFalloff;    // exponential falloff rate above fogHeightBase
    float fogHeightDensity;    // density multiplier for height fog
    int   fogHeightEnabled;    // 0 = off, 1 = on

    // Animation
    float fogTime;             // elapsed time for animated noise
    float fogNoiseScale;       // spatial scale of the noise
    float fogNoiseStrength;    // how much noise modulates density
    float fogPad;              // padding for std430 alignment
};

struct PostProcessData {
    uint depthTextureIndex;
    uint pad[3];
};

#define MAX_SPLAT_GROUPS 4
#define MAX_SPLAT_CHANNELS 4
#define MAX_SPLAT_DETAIL_TEXTURES (MAX_SPLAT_GROUPS * MAX_SPLAT_CHANNELS)  // 16

#define SPLAT_UDIM_COUNT_X 10
#define SPLAT_UDIM_COUNT_Y 10
#define SPLAT_UDIM_TILES   (SPLAT_UDIM_COUNT_X * SPLAT_UDIM_COUNT_Y)  // 100

struct SplatGroup {
    uint weightTextures[SPLAT_UDIM_TILES];  // 100 UDIM tile texture IDs (0 = empty)
};

struct TerrainData {
    uint grassAlbedoIndex;
    uint grassNormalIndex;
    uint cliffAlbedoIndex;
    uint cliffNormalIndex;
    vec4 worldMin;
    vec4 worldMax;
    // Azgaar climate blending (workstream A): x = snow line low (deg C,
    // fully snowed below), y = snow line high (deg C, no snow above),
    // z = beach band height (m above sea level), w = enable flag (1 = on;
    // 0 when the world has no climate data / non-Azgaar scenes).
    vec4 climateParams;
    uint splatGroupCount;
    uint pomEnabled;
    uint biomeColorIndex;   // RGBA8 world biome tint (alpha = 255)
    uint climateIndex;      // RGBA8: R=temp+64, G=prec, B=coast+11, A=biome id
    uint snowAlbedoIndex;
    uint sandAlbedoIndex;
    SplatGroup splatGroups[MAX_SPLAT_GROUPS];
};

#define MAX_CAMERA_OCCLUDERS 16
#define CAMERA_OCCLUDER_ACTIVE_ALPHA 0.9999
#define CAMERA_OCCLUDER_OPAQUE_UNDERLAY_ALPHA 0.75

struct CameraOccluderData {
    uint entityIds[MAX_CAMERA_OCCLUDERS];
    float alphas[MAX_CAMERA_OCCLUDERS];
    uint count;
    uint pad[3];
};

struct WaterData {
    vec4 surfaceY;
    vec4 shallowColor;
    vec4 deepColor;
    vec4 foamColor;
    vec4 waveDirAmp[4];
    vec4 waveSpeedSteep[4];
    float fresnelPower;
    float fresnelScale;
    float normalStrength;
    float rippleScale;
    float windAngle;
    float sunSpecularPower;
    float sunSpecularIntensity;
    float enabled;
};

// Azgaar props (instanced vegetation / landmarks, workstream B).
//   wind:    xy = wind direction (unit), z = speed (rad/s), w = strength (m)
//   density: xyz = global multipliers (grass / tree / rock), w = enabled (0/1)
//   lod:     x = hard LOD switch distance (m): near LOD inside, far LOD
//            outside.  The vertex shader does a hard distance switch (no
//            cross-fade blend); the hidden side collapses to a point.
//   playerPos: xyz = player ground position (world metres), w = horizontal
//            speed (m/s).  Drives the vertex-shader reaction that pushes the
//            vegetation radially away from the player (amplitude = standing
//            base + speed-scaled term), so grass parts as the player runs.
//            When the player is absent, xyz sits far away so the falloff is
//            ~0 everywhere.
struct AzgaarPropsData {
    vec4 wind;
    vec4 density;
    vec4 lod;
    vec4 playerPos;
};

// Player vegetation-reaction tuning (azgaar_props vertex shaders, workstream B).
#define PROPS_PLAYER_REACH        2.0   // metres, radial falloff around the player
#define PROPS_PLAYER_BASE         0.15  // metres, static push amplitude while standing
#define PROPS_PLAYER_SPEED_SCALE  0.05  // metres per (m/s), extra amplitude while moving

// GPU weather particles (azgaar_weather pass, plans/azgaar-weather-gpu-
// particles.md).  Single source of truth read by the simulate compute, the
// billboard vertex expansion and the fragment shading.  Must match
// VulkanWeatherData in VulkanResourceManager.h (std430 layout).
struct WeatherData {
    vec4 wind;    // xy = dir (unit, world xz), z = speed m/s, w = turbulence 0..1
    vec4 types;   // spawn weights: x snow, y rain, z dust, w leaves (CPU-normalized)
    vec4 params;  // x = box half xz (m), y = box half y (m), z = density 0..1, w = size scale
    vec4 look;    // x = global opacity, y = fall speed scale, z = far fade start (m), w = enabled
    vec4 tint;    // rgb = particle tint (dust = biome colour), a unused
};

// Screen-space GI consumer data (plans/ssgi.md D5).  The GI pass runs
// AFTER the scene/props passes, so the image indexed here is the previous
// frame's temporal history (one frame of latency, same cadence as the
// TAA/AO histories).
//   giImageIndex: bindless sampled-pool index of the temporal GI history
//     (rgb = filtered cosine-weighted irradiance, a = confidence; full
//     internal resolution).  0xFFFFFFFFu = absent (GI disabled / first
//     frame after startup or resize) - consumers keep the plain IBL
//     ambient term.
//   giIntensity: master scale of the ambient mix factor (ENGINE_GI_INTENSITY).
struct GiData {
    uint giImageIndex;
    float giIntensity;
    uint giPad[2];
};

struct IblData {
    uint environmentMapIndex;
    uint irradianceMapIndex;
    uint prefilterMapIndex;
    uint brdfLutIndex;
    uint blueNoiseIndex;
    uint tonemapLutIndex;     // unused (FFX LPM does tone mapping); kept for layout
    float environmentMapMaxLod;
    float prefilterMapMaxLod;
    uint enabled;
    float intensity;
    float specularIntensity;
    float sunThreshold;
    uint hasSH;
    uint tonemapMode;         // unused (FFX LPM); kept for layout
    uint tonemapLutPunchyIndex;  // unused (FFX LPM); kept for layout
    uint pad_ibl2;
    // L1 Spherical Harmonics for diffuse irradiance (4 coefficients, RGB in xyz, w unused)
    vec4 shL0_M0;    // L0 band (DC term)
    vec4 shL1_Mn1;   // L1 band, M=-1 (Y component)
    vec4 shL1_M0;    // L1 band, M=0  (Z component)
    vec4 shL1_Mp1;   // L1 band, M=+1 (X component)
    mat4 envRotation; // inverse of sun rotation; applied to IBL sample dirs
};

layout(buffer_reference, std430) buffer SceneBuffer {
    Camera cameras[4];
    DirectionalLight directionalLight;
    ShadowData shadow;
    int time;
    int prevTime;
    int pad[2];
    ivec4 lightCounts;    // x=directional, y=point, z=spot, w=total
    GpuLight lights[MAX_GPU_LIGHTS];
    IblData ibl;
    PostProcessData post;
    TerrainData terrain;
    FogData     fog;
    CameraOccluderData cameraOccluders;
    WaterData water;
    AzgaarPropsData props;
    WeatherData weather;
    GiData gi;
};

#define MAT_HAS_TEXTURE_COLOR 0
#define MAT_HAS_TEXTURE_ROUGHNESS_METALLIC 1
#define MAT_HAS_TEXTURE_NORMAL 2
#define MAT_HAS_TEXTURE_EMISSIVE 3
#define MAT_HAS_TEXTURE_OCCLUSION 4

#define MAT_ALPHA_OPAQUE 5
#define MAT_ALPHA_MASK 6
#define MAT_ALPHA_BLEND 7
#define MAT_IS_DOUBLE_SIDED 8

#define MAT_HAS_CLEARCOAT 9
#define MAT_HAS_TRANSMISSION 10
#define MAT_HAS_SHEEN 11
#define MAT_HAS_SPECULAR 12
#define MAT_HAS_IOR 13
#define MAT_HAS_VOLUME 14
#define MAT_HAS_EMISSIVE_FACTOR 15
#define MAT_HAS_ANISOTROPY 16
#define MAT_HAS_SPLATMAP 17

struct Material {
    // Base PBR properties
    vec4 baseColor;  // RGB + Alpha
    vec4 emissive;   // RGB + emissiveStrength
    vec4 rmas;       // Roughness, Metallic, AlphaCutoff, NormalScale

    // Extension properties packed for std430 alignment
    vec4 clearcoatSheenTransmission;  // x: clearcoat, y: clearcoatRoughness, z: sheenRoughness, w: transmission
    vec4 sheenColorAnisotropy;        // xyz: sheenColor, w: anisotropyStrength
    vec4 specularColorIor;            // xyz: specularColor, w: ior
    vec4 specularAnisotropyRotation;  // x: specularFactor, y: anisotropyRotation, zw: pad
    vec4 volumeFactors;               // x: thicknessFactor, y: attenuationDistance, zw: pad
    vec4 volumeColor;                 // xyz: attenuationColor, w: pad

    // Base texture transforms
    vec4 baseColorOffsetScale;  // xy=offset, zw=scale
    vec4 rmaOffsetScale;
    vec4 normalOffsetScale;
    vec4 emissionOffsetScale;
    vec4 occlusionOffsetScale;

    // Texture indices
    uint colorTexture;
    uint rmTexture;
    uint normalTexture;
    uint emissiveTexture;
    uint occlusionTexture;

    // Sampler indices
    uint colorTextureSampler;
    uint rmTextureSampler;
    uint normalTextureSampler;
    uint emissiveTextureSampler;
    uint occlusionTextureSampler;

    // Splatmap terrain blending — detail textures only.
    uint splatGroupCount;
    uint splatPad[3];
    uint splatAlbedoTextures[MAX_SPLAT_DETAIL_TEXTURES];   // 4 groups x 4 channels = 16
    uint splatNormalTextures[MAX_SPLAT_DETAIL_TEXTURES];    // normal+AO+displacement packed

    // Meta
    uint featureMask;
    uint id;
    uint refCount;
    uint pad0;
    uint pad1;
    uint pad2;
};

layout(buffer_reference, std430) buffer MaterialBufferRef {
    Material materials[];
};

#define MAX_TILES_X         240
#define MAX_TILES_Y         135
#define MAX_LIGHTS_PER_TILE 64

/* Indexed by tileY * tileCountX + tileX.
   x = start index into LightIndexList, y = light count in this tile. */
layout(buffer_reference, std430) buffer LightGridBuffer {
    uvec2 tiles[];
};

/* Flat list of light indices written by the culling compute pass. */
layout(buffer_reference, std430) buffer LightIndexBuffer {
    uint indices[];
};

layout(buffer_reference, std430) buffer JointMatrixBuffer {
    mat4 matrices[];
};

layout(buffer_reference, std430) buffer EntitySkinMapBuffer {
    uint offsets[];
};

struct AddressBuffer {
    uint64_t sceneBufferAddress;
    uint64_t materialBufferAddress;
    uint64_t lightGridAddress;   /* LightGridBuffer  */
    uint64_t lightIndexAddress;  /* LightIndexBuffer */
    uint64_t jointMatrixBufferAddress;
    uint64_t entitySkinMapBufferAddress;
    uint64_t prevJointMatrixBufferAddress;
};

layout(set = 0, binding = SLOT_SAMPLER) uniform sampler samplers[MAX_SAMPLERS];
layout(set = 0, binding = SLOT_IMAGE) uniform texture2D textures[MAX_IMAGES];
layout(set = 0, binding = SLOT_CUBE_IMAGE) uniform textureCube cubeTextures[MAX_IMAGES];
layout(set = 0, binding = SLOT_IMAGE) uniform texture3D textures3D[MAX_IMAGES];
layout(set = 0, binding = SLOT_STORAGE_IMAGE) uniform image2D storageImages[MAX_IMAGES];
layout(set = 0, binding = SLOT_STORAGE_IMAGE) uniform image3D storageImages3D[MAX_IMAGES];
// Cube storage views (same STORAGE_IMAGE binding — the pool stores whatever
// VkImageView type the image was created with).
layout(set = 0, binding = SLOT_STORAGE_IMAGE) uniform imageCube storageImagesCube[MAX_IMAGES];
layout(set = 0, binding = SLOT_ADDRESS_BUFFER) uniform AddressUniform { AddressBuffer addressBuffer; };

SceneBuffer sceneBuffer = SceneBuffer(addressBuffer.sceneBufferAddress);
MaterialBufferRef materialBuffer = MaterialBufferRef(addressBuffer.materialBufferAddress);
LightGridBuffer  lightGridBuffer  = LightGridBuffer(addressBuffer.lightGridAddress);
LightIndexBuffer lightIndexBuffer = LightIndexBuffer(addressBuffer.lightIndexAddress);

float cameraOccluderAlpha(uint entity) {
    uint count = min(sceneBuffer.cameraOccluders.count, uint(MAX_CAMERA_OCCLUDERS));
    for (uint i = 0u; i < count; i++) {
        if (sceneBuffer.cameraOccluders.entityIds[i] == entity) {
            return sceneBuffer.cameraOccluders.alphas[i];
        }
    }
    return 1.0;
}
