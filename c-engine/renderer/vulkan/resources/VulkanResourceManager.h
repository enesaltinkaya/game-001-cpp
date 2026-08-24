#pragma once

#include <atomic>

#include "VulkanDesc.h"
#include "VulkanImage.h"
#include "renderer/Renderer.h"
#include "renderer/vulkan/resources/VulkanBuffer.h"
#include "ecs/system/light/LightComponent.h"

#define MAX_SAMPLERS 11
#define MAX_IMAGES 4096

namespace engine {
struct VulkanFogData {
    // Base fog (screen-space)
    vec4  fogColor;              // rgb: fog color, w: unused
    float fogDensity;            // density parameter
    float fogStartDistance;      // distance at which fog begins
    float fogMaxDistance;        // linear fog end distance
    i32   fogType;               // 0=none, 1=linear, 2=exp, 3=exp2

    // Height fog
    float fogHeightBase;         // y where fog is densest
    float fogHeightFalloff;      // falloff rate above base
    float fogHeightDensity;      // density multiplier
    i32   fogHeightEnabled;      // 0=off, 1=on

    // Animation
    float fogTime;               // elapsed time
    float fogNoiseScale;         // spatial noise scale
    float fogNoiseStrength;      // noise modulation strength
    float fogPad;                // std430 alignment
};

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

struct VulkanResources {
    VulkanDesc globalSet0[FRAMES_IN_FLIGHT];
    VulkanImage dummyImage;
    VulkanImage dummyCubeImage;
};

extern struct VulkanResources vulkanResources;

void vulkanResourceInit(void);
void vulkanResourceUpdate(void);
void vulkanResourceDestroy(void);

void vulkanResourceUploadCamera(Camera* camera);
void vulkanResourceUploadDirectionalLight(DirectionalLightUbo* directionalLight);
void vulkanResourceUploadShadow(ShadowUbo* shadow);
void vulkanResourceSetContactShadowImageIndex(u32 index);
void vulkanResourceUploadLight(GpuLight* light, u32 index);
void vulkanResourceUploadLights(const GpuLight* lights, u32 count);
void vulkanResourceUploadLightUbo(const LightUbo* lightUbo);
void vulkanResourceUploadLightCounts(ivec4 counts);
void vulkanResourceUploadMaterial(Material* material);
void vulkanAddImageToPool(VulkanImage* image);
void vulkanRemoveImageFromPool(VulkanImage* image);

int vulkanAddImageViewToPool(VkImageView view);
void vulkanRemoveImageViewFromPool(int poolIndex);
int vulkanAddStorageImageViewToPool(VkImageView view);
void vulkanRemoveStorageImageViewFromPool(int poolIndex);

VulkanCommand* vulkanTransientBegin(void);
void vulkanTransientEnd(VulkanCommand* cmd, bool wait);
// Non-blocking variant: ends + submits without waiting on the fence and without
// handing the command to the garbage collector.  The caller owns the pool,
// fence and struct afterwards and must call vulkanTransientFinish() once it
// has observed vkGetFenceStatus(cmd->fence) == VK_SUCCESS (or at teardown).
void vulkanTransientEndAsync(VulkanCommand* cmd);
void vulkanTransientFinish(VulkanCommand* cmd);

void addBufferGarbage(VulkanBuffer* buffer, VkFence fence, std::atomic<bool>* submitted);
void addImageGarbage(VulkanImage* image, VkFence fence, std::atomic<bool>* submitted);
void addCommandGarbage(VulkanCommand* command);
void vulkanCleanupGarbage(void);
void vulkanResourceSetLightBuffers(u64 gridAddress, u64 indexAddress);
VkSampler vulkanGetLinearSampler(void);

void vulkanResourceSetTerrainDefaults(u32 grassAlbedo, u32 grassNormal,
                                       u32 cliffAlbedo, u32 cliffNormal);

// Azgaar climate blending (workstream A of plans/azgaar-world-population.md).
// `biomeColorIndex`/`climateIndex` are pool indices of the per-world RGBA8
// textures (uploaded by the game at world load); 0 disables that map.
void vulkanResourceSetTerrainClimateTextures(u32 biomeColorIndex, u32 climateIndex);

// Snow / sand default albedo textures (engine pak assets, set once by the
// heightmap terrain pass like the grass/cliff defaults).
void vulkanResourceSetTerrainSnowSand(u32 snowAlbedoIndex, u32 sandAlbedoIndex);

// Thresholds for the terrain climate blends. `snowLo` = fully snowed below
// (deg C), `snowHi` = snow-free above; `beachHeight` = top of the beach band
// in metres above sea level; `enabled` = 0/1 master switch.
void vulkanResourceSetTerrainClimateParams(float snowLo,
                                           float snowHi,
                                           float beachHeight,
                                           float enabled);
void vulkanResourceSetTerrainBounds(float minX, float minY, float minZ,
                                     float maxX, float maxY, float maxZ);
void vulkanResourceSetTerrainSplatUdim(u32 groupIndex, u32 udimTileIndex, u32 textureId);
void vulkanResourceSetTerrainSplatGroupCount(u32 count);
u32 vulkanResourceGetTerrainSplatGroupCount(void);
void vulkanResourceDebugTerrainSplat(void);
u32  vulkanResourceGetTerrainPomEnabled(void);
void vulkanResourceSetTerrainPomEnabled(u32 enabled);

void vulkanResourceGetTerrainState(u32* grassAlbedoIndex, u32* cliffAlbedoIndex,
                                    u32* splatGroupCount, float worldMin[3], float worldMax[3]);

void vulkanResourceSetFogData(VulkanFogData fog);
VulkanFogData vulkanResourceGetFogData(void);

struct VulkanWaterData {
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

void vulkanResourceSetWaterParams(const VulkanWaterData* params);
VulkanWaterData vulkanResourceGetWaterData(void);

/* Must match AzgaarPropsData in globalset.shader (std430 layout).  Pushed by
 * the game each frame for the Azgaar props pass (workstream B). */
struct VulkanAzgaarPropsData {
    vec4 wind = {};    // xy = dir (unit), z = speed (rad/s), w = strength (m)
    vec4 density = {}; // xyz = global multipliers (grass / tree / rock), w = enabled
    vec4 lod = {};     // x = hard LOD switch distance (m): near inside, far outside
};
void vulkanResourceSetAzgaarPropsData(const VulkanAzgaarPropsData* params);
VulkanAzgaarPropsData vulkanResourceGetAzgaarPropsData(void);

/* Must match WeatherData in globalset.shader (std430 layout).  Pushed by the
 * game each frame (AzgaarWeather) and read by the azgaar_weather compute
 * + billboard pass.  look.w < 0.5 turns the whole pass off. */
struct VulkanWeatherData {
    vec4 wind = {};    // xy = dir (unit, world xz), z = speed m/s, w = turbulence 0..1
    vec4 types = {};   // spawn weights: x snow, y rain, z dust, w leaves (normalized)
    vec4 params = {};  // x = box half xz (m), y = box half y (m), z = density 0..1, w = size scale
    vec4 look = {};    // x = global opacity, y = fall speed scale, z = far fade start (m), w = enabled
    vec4 tint = {};    // rgb = particle tint (dust = biome colour), a unused
};
void vulkanResourceSetWeather(const VulkanWeatherData* data);
VulkanWeatherData vulkanResourceGetWeatherData(void);

#define VULKAN_MAX_CAMERA_OCCLUDERS 16
void vulkanResourceSetCameraOccluders(const u32* entities, const float* alphas, u32 count);



}  // namespace engine
