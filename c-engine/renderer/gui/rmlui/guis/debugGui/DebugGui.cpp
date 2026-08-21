#include "DebugGui.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "renderer/Renderer.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"
// #include "renderer/vulkan/pass/ssr/VulkanSsrPass.h"
#include "renderer/vulkan/pass/shadow/VulkanShadowPass.h"
#include "renderer/vulkan/pass/contact_shadow/VulkanContactShadowPass.h"
// #include "renderer/vulkan/pass/skybox/VulkanSkyboxPass.h"
#include "renderer/vulkan/pass/volumetric/VulkanVolumetricPass.h"
// #include "renderer/vulkan/pass/grid/VulkanGridPass.h"
#include "renderer/vulkan/resources/VulkanIbl.h"
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
static char iblEnabled;
static char skyboxEnabled;
static char gridEnabled;
static char pomEnabled;
static char contactShadowEnabled;
static char volumetricFogEnabled;

static char* iblFileLabel;
static char iblFileLabelText[128];

static char* iblSunLabel;
static char iblSunLabelText[64];

static float iblIntensityValue;

static const char* tonemapNames[] = {
    "AgX",
    "AgX Punchy",
    "ACES Filmic",
    "Filmic (Hable)",
    "Reinhard",
    "Uncharted 2",
    "Uchimura (GT)",
    "Unreal 3",
};
static char* tonemapLabel;

static char* aaPolicyLabel;
static char aaPolicyLabelText[192];
static float casStrengthPercent;

static void syncFromPasses(void) {
    shadowsEnabled    = !vulkanShadowPassIsDisabled();
    // reflectionEnabled = !vulkanSsrPassIsDisabled();
    bloomEnabled      = !vulkanBloomPassIsDisabled();
    iblEnabled        = !vulkanIblIsDisabled();
    // skyboxEnabled     = !vulkanSkyboxPassIsDisabled();
    // gridEnabled       = !vulkanGridPassIsDisabled();
    pomEnabled        = vulkanResourceGetTerrainPomEnabled() != 0;
    contactShadowEnabled = !vulkanContactShadowPassIsDisabled();
    volumetricFogEnabled = !vulkanVolumetricPassIsDisabled();
    tonemapLabel      = (char*)tonemapNames[rendererGetTonemapMode()];

    snprintf(iblFileLabelText, sizeof(iblFileLabelText), "%s", vulkanIblGetCurrentName());
    iblFileLabel = iblFileLabelText;

    IblSunLight sun = vulkanIblGetExtractedSun();
    snprintf(iblSunLabelText, sizeof(iblSunLabelText),
             "%.1f, %.1f, %.1f", sun.direction[0], sun.direction[1], sun.direction[2]);
    iblSunLabel = iblSunLabelText;

    iblIntensityValue = vulkanIblGetIntensity();

    RendererAASettings aa = rendererGetAASettings();

    snprintf(aaPolicyLabelText,
             sizeof(aaPolicyLabelText),
             "TAA and the FSR upscaler are mutually exclusive.");
    aaPolicyLabel = aaPolicyLabelText;

    casStrengthPercent = aa.casStrength * 100.0f;
}

static int toggleShadows(void* _) {
    vulkanShadowPassSetDisabled(shadowsEnabled);
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

static int toggleIBL(void* _) {
    vulkanIblSetDisabled(iblEnabled);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int iblFilePrev(void* _) {
    vulkanIblCyclePrev();
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int iblFileNext(void* _) {
    vulkanIblCycleNext();
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int iblSunLeft(void* _) {
    (void)_;
    vulkanIblRotateSun(-15.0f, 0.0f);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int iblSunRight(void* _) {
    (void)_;
    vulkanIblRotateSun(15.0f, 0.0f);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int iblSunUp(void* _) {
    (void)_;
    vulkanIblRotateSun(0.0f, 15.0f);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int iblSunDown(void* _) {
    (void)_;
    vulkanIblRotateSun(0.0f, -15.0f);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int iblIntensityDown(void* _) {
    (void)_;
    float v = vulkanIblGetIntensity() - 0.25f;
    vulkanIblSetIntensity(v);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int iblIntensityUp(void* _) {
    (void)_;
    float v = vulkanIblGetIntensity() + 0.25f;
    vulkanIblSetIntensity(v);
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

static int tonemapPrev(void* _) {
    int cur = (int)rendererGetTonemapMode();
    cur     = (cur + TONEMAP_COUNT - 1) % TONEMAP_COUNT;
    rendererSetTonemapMode((TonemapMode)cur);
    syncFromPasses();
    rmlUpdateDirtyAll(model);
    return 0;
}

static int tonemapNext(void* _) {
    int cur = (int)rendererGetTonemapMode();
    cur     = (cur + 1) % TONEMAP_COUNT;
    rendererSetTonemapMode((TonemapMode)cur);
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

    luaRegisterFunction("debugToggleShadows", toggleShadows);
    luaRegisterFunction("debugToggleReflection", toggleReflection);
    luaRegisterFunction("debugToggleBloom", toggleBloom);
    luaRegisterFunction("debugToggleIBL", toggleIBL);
    luaRegisterFunction("debugToggleSkybox", toggleSkybox);
    luaRegisterFunction("debugToggleGrid", toggleGrid);
    luaRegisterFunction("debugTogglePOM", togglePOM);
    luaRegisterFunction("debugToggleContactShadow", toggleContactShadow);
    luaRegisterFunction("debugToggleVolumetricFog", toggleVolumetricFog);
    luaRegisterFunction("debugTonemapPrev", tonemapPrev);
    luaRegisterFunction("debugTonemapNext", tonemapNext);
    luaRegisterFunction("debugIblFilePrev", iblFilePrev);
    luaRegisterFunction("debugIblFileNext", iblFileNext);
    luaRegisterFunction("debugIblSunLeft", iblSunLeft);
    luaRegisterFunction("debugIblSunRight", iblSunRight);
    luaRegisterFunction("debugIblSunUp", iblSunUp);
    luaRegisterFunction("debugIblSunDown", iblSunDown);
    luaRegisterFunction("debugIblIntensityDown", iblIntensityDown);
    luaRegisterFunction("debugIblIntensityUp", iblIntensityUp);
    luaRegisterFunction("debugAaCasPrev", aaCasPrev);
    luaRegisterFunction("debugAaCasNext", aaCasNext);
    document = rmlNewDocument("gui/debug/debug.html");
    model    = rmlCreateModel("debug");

    rmlBind(model, "shadowsEnabled", &shadowsEnabled);
    rmlBind(model, "reflectionEnabled", &reflectionEnabled);
    rmlBind(model, "bloomEnabled", &bloomEnabled);
    rmlBind(model, "iblEnabled", &iblEnabled);
    rmlBind(model, "skyboxEnabled", &skyboxEnabled);
    rmlBind(model, "gridEnabled", &gridEnabled);
    rmlBind(model, "pomEnabled", &pomEnabled);
    rmlBind(model, "contactShadowEnabled", &contactShadowEnabled);
    rmlBind(model, "volumetricFogEnabled", &volumetricFogEnabled);
    rmlBind(model, "tonemapLabel", &tonemapLabel);
    rmlBind(model, "iblFileLabel", &iblFileLabel);
    rmlBind(model, "iblSunLabel", &iblSunLabel);
    rmlBind(model, "iblIntensityValue", &iblIntensityValue);
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
