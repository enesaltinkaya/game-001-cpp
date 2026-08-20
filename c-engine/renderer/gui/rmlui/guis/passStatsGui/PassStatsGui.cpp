#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "platform/Platform.h"
#include "renderer/Renderer.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/vulkan/Vulkan.h"
#include "renderer/vulkan/pipeline/VulkanProfile.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "timer/Timer.h"

static void added(void);
static void update(void);
static void removed(void);
static void onClickPassRow(void* element, const char* eventType, const char* id, EventParameter* parameter);

System passStatsGui = {
    .name                = "passStatsGui",
    .added               = added,
    .removed             = removed,
    .preUpdate           = nullptr,
    .update              = update,
    .postUpdate          = nullptr,
    .cpuElapsedLastFrame = 0.0,
    .cpuElapsed          = 0.0,
    .gpuElapsed          = 0.0,
    .priority            = 0,
};

static void* document;
static void* model;
static void* bodyElement;
static double lastUpdate;

// Selected pass index (for detail view)
static int selectedPassIndex = -1;
static size_t passCount;

// Cached pass data for the selected pass
static char selectedPassName[64];
static double selectedPassGpuElapsed;
static double selectedPassCpuElapsed;
static VulkanPipelineStats selectedPassStats;

// Transform functions for RMLUI data binding
static void passListInfo(int index, int type, char* out);
static void selectedPassInfo(int index, int type, char* out);

// Helper: walk up from clicked element to find a pass-row
static void* findPassRowAncestor(void* element) {
    void* current = element;
    while (current) {
        if (rmlElementHasClass(current, "pass-row")) {
            return current;
        }
        current = rmlElementGetParentNode(current);
    }
    return nullptr;
}

// Helper: collect pass-row elements (RMLUI QuerySelectorAll doesn't support class selectors)
static int collectPassRows(void** outElements, int maxElements) {
    void* allDivs[256];
    int total = rmlQuerySelectorAll(bodyElement, "div", allDivs, 256);
    int count = 0;
    for (int i = 0; i < total && count < maxElements; i++) {
        if (rmlElementHasClass(allDivs[i], "pass-row")) {
            outElements[count++] = allDivs[i];
        }
    }
    return count;
}

// Helper: compute index of a pass-row
static int getPassRowIndex(void* passRow) {
    void* elements[64];
    int count = collectPassRows(elements, 64);
    for (int i = 0; i < count; i++) {
        if (elements[i] == passRow) {
            return i;
        }
    }
    return -1;
}

void added(void) {
    ecs.showStats = 1;

    document = rmlNewDocument("gui/passstats/passstats.html");
    model    = rmlCreateModel("passstats");

    rmlBind(model, "selectedPassIndex", &selectedPassIndex);
    rmlBind(model, "passCount", &passCount);

    rmlRegisterTransformFunc(model, "passListInfo", passListInfo);
    rmlRegisterTransformFunc(model, "selectedPassInfo", selectedPassInfo);

    rmlBindArray(model, "passes", &passCount);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);

    bodyElement = rmlGetElementById(document, "passstats");

    // Bind click event handler to the document
    rmlBindEventListener(document, "click", onClickPassRow);
}

void onClickPassRow(void* element, const char* eventType, const char* id, EventParameter* parameter) {
    (void)eventType;
    (void)id;
    (void)parameter;

    // Find the pass-row element (walk up from clicked element)
    void* passRow = findPassRowAncestor(element);
    if (!passRow) {
        return;  // Click was not on a pass row
    }

    // Compute index using QuerySelectorAll
    int index = getPassRowIndex(passRow);
    if (index < 0 || (size_t)index >= passCount) {
        return;  // Invalid index
    }

    if (selectedPassIndex != index) {
        int prevSelected = selectedPassIndex;
        selectedPassIndex = index;

        // Highlight selected row, unhighlight previous
        void* elements[64];
        int count = collectPassRows(elements, 64);
        for (int i = 0; i < count; i++) {
            if (i == selectedPassIndex)
                rmlSetElementClass(elements[i], "selected");
            else if (i == prevSelected)
                rmlRemoveElementClass(elements[i], "selected");
        }

        // Update cached detail data immediately for responsive UI
        if ((size_t)selectedPassIndex < passCount) {
            struct VulkanProfile* profiles = vulkanGetPassProfiles();
            struct VulkanProfile* prof     = &profiles[selectedPassIndex];
            struct System* pass            = renderer.passes[selectedPassIndex];

            snprintf(selectedPassName, sizeof(selectedPassName), "%s", pass->name);
            selectedPassGpuElapsed = prof->elapsed / MILLION;
            selectedPassCpuElapsed = pass->cpuElapsed / MILLION;
            selectedPassStats      = prof->stats;
        }

        rmlUpdateDirtyAll(model);
    }
}

void removed(void) {
    ecs.showStats = 0;
    selectedPassIndex = -1;
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = nullptr;
}

void update(void) {
    double now = nanos();
    if (now > lastUpdate + BILLION / 2.) {  // twice per second
        passCount = vulkanGetPassProfileCount();

        // Auto-select first pass
        if (selectedPassIndex < 0 && passCount > 0) {
            selectedPassIndex = 0;
        }

        // Highlight selected row (rows exist after first rmlUpdateDirtyAll call)
        if (selectedPassIndex >= 0 && (size_t)selectedPassIndex < passCount) {
            void* elements[64];
            int count = collectPassRows(elements, 64);
            if (count > 0 && (size_t)selectedPassIndex < (size_t)count) {
                rmlSetElementClass(elements[selectedPassIndex], "selected");
            }
        }

        // Update selected pass data
        if (selectedPassIndex >= 0 && (size_t)selectedPassIndex < passCount) {
            struct VulkanProfile* profiles = vulkanGetPassProfiles();
            struct VulkanProfile* prof     = &profiles[selectedPassIndex];
            struct System* pass            = renderer.passes[selectedPassIndex];

            snprintf(selectedPassName, sizeof(selectedPassName), "%s", pass->name);
            selectedPassGpuElapsed = prof->elapsed / MILLION;
            selectedPassCpuElapsed = pass->cpuElapsed / MILLION;
            selectedPassStats      = prof->stats;
        }

        lastUpdate = now;
        rmlUpdateDirtyAll(model);
    }
}

void passListInfo(int index, int type, char* out) {
    if ((size_t)index >= passCount) {
        out[0] = '\0';
        return;
    }

    struct VulkanProfile* profiles = vulkanGetPassProfiles();
    struct VulkanProfile* prof     = &profiles[index];

    if (type == 0) {
        // Pass name
        snprintf(out, 64, "%s", renderer.passes[index]->name);
    } else if (type == 1) {
        // GPU time
        snprintf(out, 64, "%.2f", prof->elapsed / MILLION);
    } else if (type == 2) {
        // CPU time
        snprintf(out, 64, "%.2f", renderer.passes[index]->cpuElapsed / MILLION);
    }
}

void selectedPassInfo(int index, int type, char* out) {
    (void)index;
    if (selectedPassIndex < 0) {
        out[0] = '\0';
        return;
    }

    switch (type) {
        case 0: // name
            snprintf(out, 64, "%s", selectedPassName);
            break;
        case 1: // gpu elapsed ms
            snprintf(out, 64, "%.2f", selectedPassGpuElapsed);
            break;
        case 2: // cpu elapsed ms
            snprintf(out, 64, "%.2f", selectedPassCpuElapsed);
            break;
        case 3: // inputAssemblyVertices
            snprintf(out, 64, "%llu", (unsigned long long)selectedPassStats.inputAssemblyVertices);
            break;
        case 4: // inputAssemblyPrimitives
            snprintf(out, 64, "%llu", (unsigned long long)selectedPassStats.inputAssemblyPrimitives);
            break;
        case 5: // vertexShaderInvocations
            snprintf(out, 64, "%llu", (unsigned long long)selectedPassStats.vertexShaderInvocations);
            break;
        case 6: // fragmentShaderInvocations
            snprintf(out, 64, "%llu", (unsigned long long)selectedPassStats.fragmentShaderInvocations);
            break;
        case 7: // computeShaderInvocations
            snprintf(out, 64, "%llu", (unsigned long long)selectedPassStats.computeShaderInvocations);
            break;
        case 8: // clippingInvocations
            snprintf(out, 64, "%llu", (unsigned long long)selectedPassStats.clippingInvocations);
            break;
        case 9: // clippingPrimitives
            snprintf(out, 64, "%llu", (unsigned long long)selectedPassStats.clippingPrimitives);
            break;
        case 10: // tessellationControlPatches
            snprintf(out, 64, "%llu", (unsigned long long)selectedPassStats.tessellationControlPatches);
            break;
        case 11: // tessellationEvalInvocations
            snprintf(out, 64, "%llu", (unsigned long long)selectedPassStats.tessellationEvalInvocations);
            break;
        default:
            out[0] = '\0';
            break;
    }
}

void passStatsGuiToggle(void) {
    if (document) {
        guiManagerRemoveGuiNextFrame(&passStatsGui);
    } else {
        guiManagerAddGuiNextFrame(&passStatsGui);
    }
}
