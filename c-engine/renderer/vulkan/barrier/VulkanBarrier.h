#pragma once

namespace engine {
struct VulkanCommand;

enum ExecutionBarrierType {
    // A compute shader will read resources written by a previous fragment shader (e.g., post-processing).
    GRAPHICS_TO_COMPUTE,
    // A fragment shader will read resources written by a previous compute shader (e.g., sampling a generated texture).
    COMPUTE_TO_GRAPHICS,
    // A compute shader will read resources written by a previous compute shader (the "ping-pong" case).
    COMPUTE_TO_COMPUTE,
    // A shader (compute or graphics) will read resources written by a previous transfer operation.
    TRANSFER_TO_SHADER,
    // A transfer operation will read resources written by a previous compute shader.
    COMPUTE_TO_TRANSFER,
    // A transfer operation will read resources written by a previous fragment shader (e.g., blitting a render target).
    GRAPHICS_TO_TRANSFER,
    // A draw call using an indirect buffer will read the buffer that was written by a compute shader.
    COMPUTE_TO_INDIRECT_DRAW,
    // Graphics -> Graphics (e.g., post-processing, deferred rendering G-Buffer)
    GRAPHICS_TO_GRAPHICS,
    // Any Shader -> Transfer
    SHADER_TO_TRANSFER,
    // Compute -> Indirect Draw/Dispatch arguments
    COMPUTE_TO_INDIRECT_COMMAND,
    // Compute -> Vertex/Index Buffer for a subsequent graphics pass
    COMPUTE_TO_VERTEX_INPUT,
    // Device writes of any kind -> Host reads (e.g., for screenshot or data readback)
    DEVICE_WRITE_TO_HOST_READ,
    // Host writes -> Device reads (e.g., uploading data from mapped memory)
    HOST_WRITE_TO_DEVICE_READ,
    // Copy data into index/vertex buffers then use them to render
    TRANSFER_TO_VERTEX_INPUT,
};

void vulkanBarrier(struct VulkanCommand* cmd, enum ExecutionBarrierType barrierType);
}  // namespace engine
