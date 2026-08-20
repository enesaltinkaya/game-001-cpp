#include "VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanImage.h"

namespace engine {
struct VulkanDesc;
struct VulkanImage;
struct VulkanBuffer;

struct VulkanPipe {
    char name[64];
    VkPipeline pipe;
    VkPipelineLayout layout;
    struct VulkanProfile profile;
    struct VulkanDesc* set0_reserved;  // dont use
    struct VulkanDesc* set1;
    struct VulkanDesc* set2;
    struct VulkanDesc* set3;
    struct VulkanDesc* set4;
    VkClearValue clearColor1;
    VkClearValue clearColor2;
    VkClearValue clearColor3;
    VkClearValue clearDepth;
    bool isCompute;
    bool clearColor1Enabled;
    bool clearColor2Enabled;
    bool clearColor3Enabled;
    bool clearDepthEnabled;
};

struct VulkanPipeInfo {
    const char* name = nullptr;
    const char* vs = nullptr;
    const char* fs = nullptr;
    const char* tsc = nullptr;
    const char* tes = nullptr;
    const char* comp = nullptr;
    VkVertexInputAttributeDescription in1attr = {};
    VkVertexInputBindingDescription in1bind = {};
    VkVertexInputAttributeDescription in2attr = {};
    VkVertexInputBindingDescription in2bind = {};
    VkVertexInputAttributeDescription* vertexAttributes = nullptr;
    u32 vertexAttributeCount = 0;
    VkVertexInputBindingDescription* vertexBindings = nullptr;
    u32 vertexBindingCount = 0;
    struct VulkanDesc* set0_reserved = nullptr;  // dont use
    struct VulkanDesc* set1 = nullptr;
    struct VulkanDesc* set2 = nullptr;
    struct VulkanDesc* set3 = nullptr;
    struct VulkanDesc* set4 = nullptr;
    VkFormat colorFormat1 = VK_FORMAT_UNDEFINED;
    VkFormat colorFormat2 = VK_FORMAT_UNDEFINED;
    VkFormat colorFormat3 = VK_FORMAT_UNDEFINED;
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    vec4 clearColor1 = {};
    vec4 clearColor2 = {};
    vec4 clearColor3 = {};
    vec2 clearDepth = {};
    bool wireFrame = false;
    bool blend = false;
    bool blendPreserveAlpha = false;
    bool blendOit = false;  // OIT accumulation: attachment0=additive, attachment1=multiplicative
    bool blendRoad = false; // road "union": replace RGB, MAX alpha (overlapping rects stay idempotent)
    bool lineList = false;
    bool depthTestOnly = false;
    bool noCull = false;
    bool cullFront = false;  // cull front faces (render back faces only)
    bool clearColor1Enabled = false;
    bool clearColor2Enabled = false;
    bool clearColor3Enabled = false;
    bool clearDepthEnabled = false;
    bool depthClamp = false;
    VkCompareOp depthCompareOp = VK_COMPARE_OP_NEVER;
    int patchControlPoints = 0;  // 0 = default (3), otherwise use this value
    bool depthBiasEnable = false;
    float depthBiasConstantFactor = 0.0f;
    float depthBiasSlopeFactor = 0.0f;
    float depthBiasClamp = 0.0f;
};

struct VulkanBeginRenderInfo {
    struct VulkanCommand* cmd = nullptr;
    struct VulkanPipe* pipe = nullptr;
    struct VulkanImage* color1 = nullptr;
    struct VulkanImage* color2 = nullptr;
    struct VulkanImage* color3 = nullptr;
    struct VulkanImage* depth = nullptr;
    int depthLayer = -1;  // -1 or 0 = use default view, >0 = use per-layer view (1-indexed)
};

#define vulkanCreatePipe(...) engine::r_vulkanCreatePipe(engine::VulkanPipeInfo{__VA_ARGS__})
struct VulkanPipe r_vulkanCreatePipe(struct VulkanPipeInfo pipeInfo);
void vulkanDestroyPipe(struct VulkanPipe* pipe);
void vulkanBindPipe(struct VulkanCommand* cmd, struct VulkanPipe* pipe);
void vulkanPush(struct VulkanCommand* cmd, struct VulkanPipe* pipe, u32 size, void* pc);

#define vulkanBeginRender(...) engine::r_vulkanBeginRender(engine::VulkanBeginRenderInfo{__VA_ARGS__})
void r_vulkanBeginRender(struct VulkanBeginRenderInfo beginRenderInfo);
void r_vulkanEndRender(struct VulkanCommand* cmd);
#define vulkanEndRender(cmd) r_vulkanEndRender(cmd)

void vulkanDraw(struct VulkanCommand* cmd, int vertexCount, int instanceCount);
void vulkanDrawIndexed(struct VulkanCommand* cmd, int indexCount, int instanceCount);
void vulkanDrawIndexedIndirect(struct VulkanCommand* cmd,
                               struct VulkanBuffer* buf,
                               u32 drawCount,
                               u32 stride);
void vulkanDrawIndirectCount(struct VulkanCommand* cmd,
                             struct VulkanBuffer* buffer,
                             u64 offset,
                             struct VulkanBuffer* countBuffer,
                             u64 countBufferOffset,
                             u32 maxDrawCount,
                             u32 stride);
void vulkanDispatch(struct VulkanCommand* cmd, struct VulkanPipe* pipe, int x, int y, int z);
void vulkanDispatchIndirect(struct VulkanCommand* cmd,
                            struct VulkanPipe* pipe,
                            struct VulkanBuffer* buffer,
                            u64 offset);

void vulkanBindVertex(struct VulkanCommand* cmd,
                      struct VulkanBuffer* buffer1,
                      u64 offset1,
                      struct VulkanBuffer* buffer2,
                      u64 offset2,
                      struct VulkanBuffer* buffer3,
                      u64 offset3);
void vulkanBindIndex(struct VulkanCommand* cmd,
                     struct VulkanBuffer* indexBuffer,
                     u64 offset,
                     VkIndexType indexType);

void vulkanViewport(struct VulkanCommand* cmd, int x, int y, int w, int h);
void vulkanScissor(struct VulkanCommand* cmd, int x, int y, int w, int h);
}  // namespace engine
