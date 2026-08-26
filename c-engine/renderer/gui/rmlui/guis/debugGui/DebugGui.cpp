#include "DebugGui.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/Renderer.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"
// #include "renderer/vulkan/pass/ssr/VulkanSsrPass.h"
#include "renderer/vulkan/pass/shadow/VulkanShadowPass.h"
#include "renderer/vulkan/pass/contact_shadow/VulkanContactShadowPass.h"
#include "renderer/vulkan/pass/lpm/VulkanLpmPass.h"
// #include "renderer/vulkan/pass/skybox/VulkanSkyboxPass.h"
#include "renderer/vulkan/pass/volumetric/VulkanVolumetricPass.h"
// #include "renderer/vulkan/pass/grid/VulkanGridPass.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "rmlui/wrapper/src/crmlui.h"
#include <stdio.h>

namespace engine {

DebugGui debugGui;

DebugGui::DebugGui() : System("debugGui") {}

static void* document;
static void* model;

static char shadowsEnabled;
static char reflectionEnabled;
static char bloomEnabled;
static char skyboxEnabled;
static char gridEnabled;
static char pomEnabled;
static char contactShadowEnabled;
static char volumetricFogEnabled;

static char* tonemapLabel;

/* LPM tone/gamut-mapping tuning (TONEMAP section of the debug GUI).
 * Bound two-way to the document model: slider input updates the float,
 * update() pushes it into the LPM pass every frame. */
static float lpmContrast;
static float lpmHdrMax;
static float lpmShoulderContrast;
static float lpmSaturation;
static float lpmExposure;

static void syncLpmParams(void) {
    const VulkanLpmParams* p = vulkanLpmPassGetParams();
    lpmContrast              = p->contrast;
    lpmHdrMax                = p->hdrMax;
    lpmShoulderContrast      = p->shoulderContrast;
    lpmSaturation            = p->saturation;
    lpmExposure              = p->lpmExposure;
}

static char* aaPolicyLabel;
static char aaPolicyLabelText[192];
static float casStrengthPercent;

static void syncFromPasses(void) {
    shadowsEnabled    = vulkanShadowPassGetQuality() != SHADOW_QUALITY_OFF;
    // reflectionEnabled = !vulkanSsrPassIsDisabled();
    bloomEnabled      = !vulkanBloomPassIsDisabled();
    // skyboxEnabled     = !vulkanSkyboxPassIsDisabled();
    // gridEnabled       = !vulkanGridPassIsDisabled();
    pomEnabled        = vulkanResourceGetTerrainPomEnabled() != 0;
    contactShadowEnabled = !vulkanContactShadowPassIsDisabled();
    volumetricFogEnabled = !vulkanVolumetricPassIsDisabled();
    /* Tone/gamut mapping is done by the FFX LPM pass — no more custom
     * tonemapping curves to cycle through. */
    tonemapLabel      = (char*)"LPM";

    RendererAASettings aa = rendererGetAASettings();

    snprintf(aaPolicyLabelText,
             sizeof(aaPolicyLabelText),
             "TAA and the FSR upscaler are mutually exclusive.");
    aaPolicyLabel = aaPolicyLabelText;

    casStrengthPercent = aa.casStrength * 100.0f;
}

static int toggleShadows(void* _) {
    /* Debug toggle is a simple off <-> medium switch; the full quality
     * cycle (low/medium/high) lives in the settings GUI. */
    vulkanShadowPassSetQuality(vulkanShadowPassGetQuality() == SHADOW_QUALITY_OFF
                                   ? SHADOW_QUALITY_MEDIUM
                                   : SHADOW_QUALITY_OFF);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleReflection(void* _) {
    // vulkanSsrPassSetDisabled(reflectionEnabled);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleBloom(void* _) {
    vulkanBloomPassSetDisabled(bloomEnabled);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleSkybox(void* _) {
    // vulkanSkyboxPassSetDisabled(skyboxEnabled);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleGrid(void* _) {
    // vulkanGridPassSetDisabled(gridEnabled);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleVolumetricFog(void* _) {
    vulkanVolumetricPassSetDisabled(volumetricFogEnabled);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int togglePOM(void* _) {
    (void)_;
    vulkanResourceSetTerrainPomEnabled(pomEnabled ? 0 : 1);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int toggleContactShadow(void* _) {
    (void)_;
    vulkanContactShadowPassSetDisabled(contactShadowEnabled);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int aaCasPrev(void* _) {
    RendererAASettings aa = rendererGetAASettings();
    aa.casStrength -= 0.05f;
    rendererSetAASettings(aa);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int aaCasNext(void* _) {
    RendererAASettings aa = rendererGetAASettings();
    aa.casStrength += 0.05f;
    rendererSetAASettings(aa);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

void DebugGui::added() {
    syncFromPasses();
    syncLpmParams();

    luaRegisterFunction("debugToggleShadows", toggleShadows);
    luaRegisterFunction("debugToggleReflection", toggleReflection);
    luaRegisterFunction("debugToggleBloom", toggleBloom);
    luaRegisterFunction("debugToggleSkybox", toggleSkybox);
    luaRegisterFunction("debugToggleGrid", toggleGrid);
    luaRegisterFunction("debugTogglePOM", togglePOM);
    luaRegisterFunction("debugToggleContactShadow", toggleContactShadow);
    luaRegisterFunction("debugToggleVolumetricFog", toggleVolumetricFog);
    luaRegisterFunction("debugAaCasPrev", aaCasPrev);
    luaRegisterFunction("debugAaCasNext", aaCasNext);
    document = rmlNewDocument("gui/debug/debug.html");
    model    = rmlCreateModel("debug");

    rmlBind(model, "shadowsEnabled", &shadowsEnabled);
    rmlBind(model, "reflectionEnabled", &reflectionEnabled);
    rmlBind(model, "bloomEnabled", &bloomEnabled);
    rmlBind(model, "skyboxEnabled", &skyboxEnabled);
    rmlBind(model, "gridEnabled", &gridEnabled);
    rmlBind(model, "pomEnabled", &pomEnabled);
    rmlBind(model, "contactShadowEnabled", &contactShadowEnabled);
    rmlBind(model, "volumetricFogEnabled", &volumetricFogEnabled);
    rmlBind(model, "tonemapLabel", &tonemapLabel);
    rmlBind(model, "lpmContrast", &lpmContrast);
    rmlBind(model, "lpmHdrMax", &lpmHdrMax);
    rmlBind(model, "lpmShoulderContrast", &lpmShoulderContrast);
    rmlBind(model, "lpmSaturation", &lpmSaturation);
    rmlBind(model, "lpmExposure", &lpmExposure);
    rmlBind(model, "aaPolicyLabel", &aaPolicyLabel);
    rmlBind(model, "casStrengthPercent", &casStrengthPercent);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);
}

void DebugGui::removed() {
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = nullptr;
}

void DebugGui::update() {
    syncFromPasses();
    /* Push the slider values into the LPM pass — the dispatch reads them
     * every frame, so slider changes show up within a frame or two. */
    VulkanLpmParams params = {
        .contrast         = lpmContrast,
        .hdrMax           = lpmHdrMax,
        .shoulderContrast = lpmShoulderContrast,
        .saturation       = lpmSaturation,
        .lpmExposure      = lpmExposure,
    };
    vulkanLpmPassSetParams(&params);
    rmlUpdateDirtyAll(model);
}

void debugGuiToggle(void) {
    if (document) {
        guiManagerRemoveGuiNextFrame(&debugGui);
    } else {
        guiManagerAddGuiNextFrame(&debugGui);
    }
}
}  // namespace engine
