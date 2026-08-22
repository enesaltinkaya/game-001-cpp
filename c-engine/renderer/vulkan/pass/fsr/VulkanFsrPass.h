#pragma once

#include "ecs/system/System.h"

namespace engine {
struct VulkanImage;

class VulkanFsrPass : public System {
public:
    VulkanFsrPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
    void postUpdate() override;
};

extern VulkanFsrPass vulkanFsrPass;

struct VulkanImage* vulkanFsrPassGetOutput(void);
char vulkanFsrPassIsEnabled(void);
void vulkanFsrPassSetReactiveMask(char enabled);
char vulkanFsrPassGetReactiveMask(void);
/* The FSR reactive-mask image (render-res R32F), or NULL while the
 * upscaler context is not created. The DOF pass max-blends its CoC mask
 * into it before the FSR dispatch. */
struct VulkanImage* vulkanFsrPassGetReactiveMaskImage(void);
}  // namespace engine
