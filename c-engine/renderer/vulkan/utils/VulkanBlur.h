#pragma once

namespace engine {
struct VulkanCommand;
struct VulkanImage;

void vulkanBlur(struct VulkanCommand* cmd, struct VulkanImage* img, struct VulkanImage* tempImg);
void vulkanBlurBilateral(struct VulkanCommand* cmd, struct VulkanImage* original, struct VulkanImage* blurred);
void vulkanBlurCleanup(void);
}  // namespace engine
