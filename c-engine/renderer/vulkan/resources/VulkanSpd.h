#pragma once

/* AMD FidelityFX SPD (Single Pass Downsampler) utility — see docs/fsr3.1.md.
 *
 * Builds the full mip chain of an R16G16B16A16_SFLOAT image in ONE compute
 * dispatch (wave-interleaved, up to 13 mips => 4096 max dimension), instead
 * of a per-mip blit/dispatch chain. MEAN (box) filter, LOAD sampling.
 *
 * Format constraint: the shipped VK shader's storage-image format qualifier
 * is baked at compile time; our SDK fork patches it to rgba16f, so only
 * R16G16B16A16_SFLOAT images with STORAGE usage and >= 2 mips can be
 * downsampled (see the fork patch notes in docs/fsr3.1.md).
 *
 * Not a render pass — call from wherever a mip chain is needed. First
 * consumer: the SSSR input-color pyramid (plans/fidelityfx-sdk-expansion.md,
 * Phase 5). Load-time texture mips keep the blit path (arbitrary formats).
 */

namespace engine {

struct VulkanCommand;
struct VulkanImage;

/* Downsample image->mip 0 into mips 1..N via SPD.
 * Returns 1 on success. Image must be R16G16B16A16_SFLOAT, 2..13 mips,
 * STORAGE usage; left in VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL. */
char vulkanSpdGenerateMips(VulkanCommand* cmd, VulkanImage* image);

/* Teardown — frees the FFX backend + context. Safe to call uninitialized. */
void vulkanSpdDestroy(void);

/* ENGINE_SPD_SELFTEST=1: run a fill→downsample→readback round trip at
 * engine init and log PASS/FAIL (regression check for SDK/fork updates —
 * the shader-blob format patch is not covered by any compile-time assert). */
void vulkanSpdRunSelfTest(void);

}  // namespace engine
