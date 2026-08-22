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
 * Focus model is manual (settings GUI): focus distance, f-number, focal
 * length, blur quality (ring count). CoC is derived from the thin-lens
 * model via ffxDofCalculateCoc{Scale,Bias} against the camera's
 * projection matrix. */
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

/* Manual focus model. focusMeters: distance to the focus plane.
 * fNumber: lens f-number (aperture radius = focalLength / fNumber).
 * focalLengthMm: lens focal length in millimetres. quality: number of
 * blur rings (1..8, higher = smoother bokeh, more cost). Changing
 * quality recreates the FFX context lazily on the next dispatch. */
void vulkanDofPassSetFocus(float focusMeters);
void vulkanDofPassSetFNumber(float fNumber);
void vulkanDofPassSetFocalLength(float focalLengthMm);
void vulkanDofPassSetQuality(int rings);
float vulkanDofPassGetFocus(void);
float vulkanDofPassGetFNumber(void);
float vulkanDofPassGetFocalLength(void);
int vulkanDofPassGetQuality(void);

/* Max-blend a CoC-derived reactivity mask into the FSR reactive mask
 * image (pixels with significant blur tell FSR not to accumulate detail
 * that will be blurred away). Called by the FSR pass right after its own
 * reactive-mask generation, before the FSR dispatch. No-op when the FSR
 * reactive mask image does not exist (upscaler off) or DOF is disabled. */
void vulkanDofPassApplyReactiveMask(VulkanCommand* cmd, VulkanImage* depth);
}  // namespace engine