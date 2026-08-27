#pragma once

#include "ecs/system/System.h"

namespace engine {
class VulkanSsgiPass : public System {
public:
    VulkanSsgiPass();
    void added() override;
    void removed() override;
    void preUpdate() override;
    void update() override;
};

extern VulkanSsgiPass vulkanSsgiPass;

void  vulkanSsgiPassSetDisabled(char disabled);
char  vulkanSsgiPassIsDisabled(void);
void  vulkanSsgiPassSetDistance(float distance);
float vulkanSsgiPassGetDistance(void);
struct VulkanImage* vulkanSsgiPassGetOutput(void);  /* final (temporally filtered) */
struct VulkanImage* vulkanSsgiPassGetRawOutput(void);  /* pre-filter raymarch */
}  // namespace engine
