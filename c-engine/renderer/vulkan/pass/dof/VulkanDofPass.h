#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;
struct VulkanCommand;

/* AMD FidelityFX DOF — ring-based bokeh blur, running between TAA and FSR
 * (pre-upscale): the upscaler then sharpens in-focus detail and the CoC
 * mask feeds the FSR reactive mask so bokeh isn't misread as detail change.
 *
 * Inputs: HDR color (TAA output when TAA is on, otherwise the post-
 * composite color) + reverse-Z depth. Internally a half-res bilateral
 * downsample + dilate + ring blur + composite chain (all inside the FFX
 * context). Output: full-res R16G16B16A16_SFLOAT image.
 *
 * Focus model is automatic: the game pushes the active camera's distance to
 * the player each frame (vulkanDofPassSetFocusDistance), so the focus plane
 * tracks the subject and follows wheel zoom. Aperture (f-number) and focal
 * length are fixed engine constants. CoC is derived from the thin-lens model
 * via ffxDofCalculateCoc{Scale,Bias} against the camera's projection matrix.
 * The only user-facing knob is blur quality (ring count). */
class VulkanDofPass : public System {
public:
    VulkanDofPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanDofPass vulkanDofPass;

/* The full-res HDR output. Non-NULL only when the pass dispatched this
 * frame (enabled + ready) — consumers (Final, Bloom) fall back to the
 * TAA/composite color otherwise. */
struct VulkanImage* vulkanDofPassGetOutput(void);

void vulkanDofPassSetDisabled(char disabled);
char vulkanDofPassIsDisabled(void);

/* Focus distance (meters from the active camera to the focus subject). The
 * game pushes this each frame while a player is loaded (it equals the
 * camera's orbit/top-down distance, so wheel zoom moves the focus plane).
 * Falls back to a default when no value has been pushed (e.g. main menu).
 * quality: number of blur rings (1..8, higher = smoother bokeh, more cost).
 * Changing quality recreates the FFX context lazily on the next dispatch. */
void vulkanDofPassSetFocusDistance(float meters);
float vulkanDofPassGetFocusDistance(void);
void vulkanDofPassSetQuality(int rings);
int vulkanDofPassGetQuality(void);

/* Max-blend a CoC-derived reactivity mask into the FSR reactive mask
 * image (pixels with significant blur tell FSR not to accumulate detail
 * that will be blurred away). Called by the FSR pass right after its own
 * reactive-mask generation, before the FSR dispatch. No-op when the FSR
 * reactive mask image does not exist (upscaler off) or DOF is disabled. */
void vulkanDofPassApplyReactiveMask(VulkanCommand* cmd, VulkanImage* depth);
}  // namespace engine