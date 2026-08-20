#include "VulkanCommand.h"
#include "../Vulkan.h"
#include "../utils/VulkanError.h"
#include "Utils.h"
#include "thread/Thread.h"

static Thread queueLock = {.mutex = PTHREAD_MUTEX_INITIALIZER};

static const VkCommandBufferBeginInfo beginInfo = {
    .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
};

void vulkanBegin(VulkanCommand *cmd) {
  assert(cmd);
  VK_CHECK(vkBeginCommandBuffer(cmd->cmd, &beginInfo), "vkBeginCommandBuffer");
}

void vulkanEnd(VulkanCommand *cmd) {
  VK_CHECK(vkEndCommandBuffer(cmd->cmd), "vkEndCommandBuffer");
}

void vulkanFenceWait(VulkanCommand *cmd) {
  VK_CHECK(vkWaitForFences(vulkan.device, 1, &cmd->fence, 1, UINT64_MAX),
           "vkWaitForFences");
}

void vulkanWaitManual(VulkanCommand *cmd) {
  double start = millies();
  while (millies() < start + 5000) {
    VkResult status = vkGetFenceStatus(vulkan.device, cmd->fence);
    if (status == VK_SUCCESS) {
      break;
    }
    if (status == VK_ERROR_DEVICE_LOST) {
      vulkanCheckQueueError(status, "vkGetFenceStatus");
      break;
    }
  }
}

void vulkanReset(VulkanCommand *cmd) {
  VK_CHECK(vkResetFences(vulkan.device, 1, &cmd->fence), "vkResetFences");
}

void r_vulkanSubmit(VulkanSubmitInfo info) {
  VkSubmitInfo submitInfo = {};
  submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submitInfo.pWaitDstStageMask = &info.stageFlags;
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &info.cmd->cmd;

  if (info.wait) {
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = &info.wait;
  }

  if (info.signal) {
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &info.signal;
  }

  threadLock(&queueLock);
  VkResult submitResult =
      vkQueueSubmit(vulkan.graphicsQueue, 1, &submitInfo, info.cmd->fence);
  info.cmd->submitted = true;
  threadUnlock(&queueLock);

  if (vulkanCheckQueueError(submitResult, "vkQueueSubmit")) {
    vulkan.skipFrame = 1;
  }
}

void vulkanLabelBeginColor(VulkanCommand *cmd, const char *name, float r,
                           float g, float b, float a) {
  VkDebugUtilsLabelEXT param = {
      .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
      .pLabelName = name,
      .color = {r, g, b, a},
  };
  vkCmdBeginDebugUtilsLabelEXT(cmd->cmd, &param);
}

void vulkanLabelBegin(VulkanCommand *cmd, const char *name) {
  vulkanLabelBeginColor(cmd, name, 0, 1, 0, 1);
}

void vulkanLabelEnd(VulkanCommand *cmd) {
  vkCmdEndDebugUtilsLabelEXT(cmd->cmd);
}

void vulkanPresent(VkSwapchainKHR *swapchain, u32 *swapchainImageIndex,
                   VkSemaphore *semaphore) {
  static VkPresentInfoKHR presentInfo;
  presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
  presentInfo.swapchainCount = 1;
  presentInfo.pSwapchains = swapchain;
  presentInfo.waitSemaphoreCount = 1;
  presentInfo.pImageIndices = swapchainImageIndex;
  presentInfo.pWaitSemaphores = semaphore;
  threadLock(&queueLock);
  VkResult result = vkQueuePresentKHR(vulkan.graphicsQueue, &presentInfo);

  if (vulkanCheckQueueError(result, "vkQueuePresentKHR")) {
    vulkan.skipFrame = 1;
  }

  threadUnlock(&queueLock);
}

void vulkanWaitIdle(const char *reason){
  threadLock(&queueLock);
  warn("vulkanCore: waitIdle! reason: %s", reason);
  vkDeviceWaitIdle(vulkan.device);
  threadUnlock(&queueLock);
}
