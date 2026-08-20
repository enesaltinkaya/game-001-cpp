#include "VulkanProfile.h"
#include "renderer/vulkan/resources/VulkanImage.h"

struct VulkanDesc;
struct VulkanImage;
struct VulkanBuffer;

typedef struct VulkanPipe {
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
    char isCompute;
    char clearColor1Enabled;
    char clearColor2Enabled;
    char clearColor3Enabled;
    char clearDepthEnabled;
} VulkanPipe;

typedef struct VulkanPipeInfo {
    const char* name;
    const char* vs;
    const char* fs;
    const char* tsc;
    const char* tes;
    const char* comp;
    VkVertexInputAttributeDescription in1attr;
    VkVertexInputBindingDescription in1bind;
    VkVertexInputAttributeDescription in2attr;
    VkVertexInputBindingDescription in2bind;
    VkVertexInputAttributeDescription* vertexAttributes;
    u32 vertexAttributeCount;
    VkVertexInputBindingDescription* vertexBindings;
    u32 vertexBindingCount;
    struct VulkanDesc* set0_reserved;  // dont use
    struct VulkanDesc* set1;
    struct VulkanDesc* set2;
    struct VulkanDesc* set3;
    struct VulkanDesc* set4;
    VkFormat colorFormat1;
    VkFormat colorFormat2;
    VkFormat colorFormat3;
    VkFormat depthFormat;
    vec4 clearColor1;
    vec4 clearColor2;
    vec4 clearColor3;
    vec2 clearDepth;
    char wireFrame;
    char blend;
    char blendPreserveAlpha;
    char blendOit;  // OIT accumulation: attachment0=additive, attachment1=multiplicative
    char blendRoad; // road "union": replace RGB, MAX alpha (overlapping rects stay idempotent)
    char lineList;
    char depthTestOnly;
    char noCull;
    char cullFront;  // cull front faces (render back faces only)
    char clearColor1Enabled;
    char clearColor2Enabled;
    char clearColor3Enabled;
    char clearDepthEnabled;
    char depthClamp;
    VkCompareOp depthCompareOp;
    int patchControlPoints;  // 0 = default (3), otherwise use this value
    char depthBiasEnable;
    float depthBiasConstantFactor;
    float depthBiasSlopeFactor;
    float depthBiasClamp;
} VulkanPipeInfo;

typedef struct VulkanBeginRenderInfo {
    struct VulkanCommand* cmd;
    struct VulkanPipe* pipe;
    struct VulkanImage* color1;
    struct VulkanImage* color2;
    struct VulkanImage* color3;
    struct VulkanImage* depth;
    int depthLayer;  // -1 or 0 = use default view, >0 = use per-layer view (1-indexed)
} VulkanBeginRenderInfo;

#define vulkanCreatePipe(...) \
    r_vulkanCreatePipe((struct VulkanPipeInfo){.blend = 0, __VA_ARGS__})
struct VulkanPipe r_vulkanCreatePipe(struct VulkanPipeInfo pipeInfo);
void vulkanDestroyPipe(struct VulkanPipe* pipe);
void vulkanBindPipe(struct VulkanCommand* cmd, struct VulkanPipe* pipe);
void vulkanPush(struct VulkanCommand* cmd, struct VulkanPipe* pipe, u32 size, void* pc);

#define vulkanBeginRender(...) r_vulkanBeginRender((struct VulkanBeginRenderInfo){__VA_ARGS__})
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
