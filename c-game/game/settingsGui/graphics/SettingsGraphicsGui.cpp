#include "SettingsGraphicsGui.h"
#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "futuretask/FutureTask.h"
#include "renderer/Renderer.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/vulkan/pass/ao/VulkanAOPass.h"
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"
#include "renderer/vulkan/pass/diffuse_gi/VulkanDiffuseGIPass.h"
#include "renderer/vulkan/pass/dof/VulkanDofPass.h"
#include "renderer/vulkan/pass/lens/VulkanLensPass.h"
#include "renderer/vulkan/pass/shadow/VulkanShadowPass.h"
#include "renderer/vulkan/pass/ssr/VulkanSsrPass.h"
#include "renderer/vulkan/pass/contact_shadow/VulkanContactShadowPass.h"
#include "renderer/vulkan/pass/volumetric/VulkanVolumetricPass.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "settings/Settings.h"
#include "../SettingsGui.h"
#include <stdio.h>

namespace game {
static void syncAAUi(void);
static void persistAASettings(void* _);
static void persistLensSettings(void* _);
static void persistDofSettings(void* _);
static void queueAAPersist(void);
static void applyUpscalerModeLater(void* _);
static void flushPendingTasks(void);
static void renderScaleApply(void* _);

SettingsGraphicsGui settingsGraphicsGui;

SettingsGraphicsGui::SettingsGraphicsGui() : engine::System("settingsGraphicsGui") {}

static void* document;
static void* model;

static engine::RendererAASettings aaSettings;
static engine::RendererUpscalerMode upscalerMode;
static char renderScaleDisabled;
static float casStrengthPercent;
static float renderScalePercent;
static char lensParamsDisabled;
static float lensGrainPercent;
static float lensChromAbPercent;
static float lensVignettePercent;
static char dofParamsDisabled;
static float dofQuality;
static int upscalerTaskKey    = -1;
static int aaTaskKey          = -1;
static int lensTaskKey        = -1;
static int dofTaskKey         = -1;
static int renderScaleTaskKey = -1;

static const char* upscalerNames[] = {
    "Off",
    "Native AA",
    "Quality",
    "Balanced",
    "Performance",
    "Ultra Performance",
};
static const char* fogModeNames[] = {
    "Off",
    "Fog",
};
static const char* shadowQualityNames[] = {
    "Off",
    "Low",
    "Medium",
    "High",
};
static char* upscalerLabel;
static char* aaPolicyLabel;
static char* upscalePolicyLabel;
static char* shadowsLabel;
static char* ssrLabel;
static char* aoLabel;
static char* giLabel;
static char* bloomLabel;
static char* lensLabel;
static char* dofLabel;
static char* contactShadowLabel;
static char* fogLabel;
static char* taaLabel;
static int   fogMode;
static char upscalerLabelText[64];
static char aaPolicyLabelText[192];
static char upscalePolicyLabelText[192];
static char shadowsLabelText[16];
static char ssrLabelText[16];
static char aoLabelText[16];
static char giLabelText[16];
static char bloomLabelText[16];
static char lensLabelText[16];
static char dofLabelText[16];
static char contactShadowLabelText[16];
static char fogLabelText[16];
static char taaLabelText[16];
static int upscalerPrev(void* _);
static int upscalerNext(void* _);
static int aaCasStrengthChange(void* _);
static int renderScaleChange(void* _);
static int toggleLens(void* _);
static int lensParamChange(void* _);
static int toggleDof(void* _);
static int dofParamChange(void* _);
static int graphicsClose(void* _);
static int toggleShadows(void* _);
static int toggleSsr(void* _);
static int toggleAo(void* _);
static int toggleGi(void* _);
static int toggleBloom(void* _);
static int toggleContactShadow(void* _);
static int toggleFog(void* _);
static int toggleTaa(void* _);

void SettingsGraphicsGui::added() {
    engine::luaRegisterFunction("upscalerPrev", upscalerPrev);
    engine::luaRegisterFunction("upscalerNext", upscalerNext);
    engine::luaRegisterFunction("aaCasStrengthChange", aaCasStrengthChange);
    engine::luaRegisterFunction("renderScaleChange", renderScaleChange);
    engine::luaRegisterFunction("graphicsClose", graphicsClose);
    engine::luaRegisterFunction("toggleShadows", toggleShadows);
    engine::luaRegisterFunction("toggleSsr", toggleSsr);
    engine::luaRegisterFunction("toggleAo", toggleAo);
    engine::luaRegisterFunction("toggleGi", toggleGi);
    engine::luaRegisterFunction("toggleBloom", toggleBloom);
    engine::luaRegisterFunction("toggleLens", toggleLens);
    engine::luaRegisterFunction("lensParamChange", lensParamChange);
    engine::luaRegisterFunction("toggleDof", toggleDof);
    engine::luaRegisterFunction("dofParamChange", dofParamChange);
    engine::luaRegisterFunction("toggleContactShadow", toggleContactShadow);
    engine::luaRegisterFunction("toggleFog", toggleFog);
    engine::luaRegisterFunction("toggleTaa", toggleTaa);
    /* Read live renderer state rather than stale settings values so that
     * changes made via the debug GUI (or elsewhere) are reflected. */
    aaSettings   = engine::rendererGetAASettings();
    upscalerMode = engine::rendererGetUpscalerMode();
    fogMode      = (int)utils::settingsGetDouble("fogMode");
    if (fogMode < 0 || fogMode > 1) fogMode = 1;
    lensGrainPercent    = engine::vulkanLensPassGetGrain() * 100.0f;
    lensChromAbPercent  = engine::vulkanLensPassGetChromAb() * 100.0f;
    lensVignettePercent = engine::vulkanLensPassGetVignette() * 100.0f;
    lensParamsDisabled  = engine::vulkanLensPassIsDisabled();
    dofQuality          = (float)engine::vulkanDofPassGetQuality();
    dofParamsDisabled   = engine::vulkanDofPassIsDisabled();
    syncAAUi();

    renderScalePercent = engine::rendererGetRenderScale() * 100.0f;

    document = rmlNewDocument("gui/settings/graphics/graphics.html");
    model    = rmlCreateModel("graphics");
    rmlBindBool(model, "renderScaleDisabled", &renderScaleDisabled);
    rmlBind(model, "upscalerLabel", &upscalerLabel);
    rmlBind(model, "aaPolicyLabel", &aaPolicyLabel);
    rmlBind(model, "upscalePolicyLabel", &upscalePolicyLabel);
    rmlBindFloat(model, "casStrengthPercent", &casStrengthPercent);
    rmlBindFloat(model, "renderScalePercent", &renderScalePercent);
    rmlBindBool(model, "lensParamsDisabled", &lensParamsDisabled);
    rmlBindFloat(model, "lensGrainPercent", &lensGrainPercent);
    rmlBindFloat(model, "lensChromAbPercent", &lensChromAbPercent);
    rmlBindFloat(model, "lensVignettePercent", &lensVignettePercent);
    rmlBindBool(model, "dofParamsDisabled", &dofParamsDisabled);
    rmlBindFloat(model, "dofQuality", &dofQuality);
    rmlBind(model, "shadowsLabel", &shadowsLabel);
    rmlBind(model, "ssrLabel", &ssrLabel);
    rmlBind(model, "aoLabel", &aoLabel);
    rmlBind(model, "giLabel", &giLabel);
    rmlBind(model, "bloomLabel", &bloomLabel);
    rmlBind(model, "lensLabel", &lensLabel);
    rmlBind(model, "dofLabel", &dofLabel);
    rmlBind(model, "contactShadowLabel", &contactShadowLabel);
    rmlBind(model, "fogLabel", &fogLabel);
    rmlBind(model, "taaLabel", &taaLabel);
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void SettingsGraphicsGui::removed() {
    /* Flush any pending debounced settings before the GUI goes away.
     * The sliders debounce their apply tasks (500ms); the settings menu
     * background is opaque, so the player cannot watch the effect live
     * and typically hits BACK within the debounce window. Dropping the
     * tasks here made slider changes (CAS strength, render scale) and the
     * FSR selector silently revert to the previous value -- the label
     * showed the new one, but nothing was applied. */
    flushPendingTasks();
    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = nullptr;
    model    = nullptr;
}

static void syncAAUi(void) {
    renderScaleDisabled   = engine::rendererIsUpscalerEnabled();
    snprintf(taaLabelText, sizeof(taaLabelText), "%s", engine::rendererIsTAAEnabled() ? "On" : "Off");
    taaLabel = taaLabelText;

    snprintf(upscalerLabelText, sizeof(upscalerLabelText), "%s", upscalerNames[upscalerMode]);
    upscalerLabel = upscalerLabelText;

    snprintf(
        aaPolicyLabelText,
        sizeof(aaPolicyLabelText),
        "Post-process AA is disabled while FSR is active.");
    aaPolicyLabel = aaPolicyLabelText;

    snprintf(upscalePolicyLabelText,
             sizeof(upscalePolicyLabelText),
             engine::rendererIsUpscalerEnabled()
                 ? "FSR quality mode controls the internal render resolution. Manual resolution "
                   "scale is disabled."
                 : "Manual resolution scale is used only when the upscaler is Off.");
    upscalePolicyLabel = upscalePolicyLabelText;

    casStrengthPercent = aaSettings.casStrength * 100.0f;

    snprintf(shadowsLabelText, sizeof(shadowsLabelText), "%s",
             shadowQualityNames[engine::vulkanShadowPassGetQuality()]);
    shadowsLabel = shadowsLabelText;
    snprintf(ssrLabelText, sizeof(ssrLabelText), "%s", engine::vulkanSsrPassIsDisabled() ? "Off" : "On");
    ssrLabel = ssrLabelText;
    snprintf(aoLabelText, sizeof(aoLabelText), "%s", engine::vulkanAOPassIsDisabled() ? "Off" : "On");
    aoLabel = aoLabelText;
    snprintf(bloomLabelText, sizeof(bloomLabelText), "%s", engine::vulkanBloomPassIsDisabled() ? "Off" : "On");
    bloomLabel = bloomLabelText;
    snprintf(contactShadowLabelText, sizeof(contactShadowLabelText), "%s", engine::vulkanContactShadowPassIsDisabled() ? "Off" : "On");
    contactShadowLabel = contactShadowLabelText;
    snprintf(lensLabelText, sizeof(lensLabelText), "%s", engine::vulkanLensPassIsDisabled() ? "Off" : "On");
    lensLabel = lensLabelText;
    snprintf(dofLabelText, sizeof(dofLabelText), "%s", engine::vulkanDofPassIsDisabled() ? "Off" : "On");
    dofLabel = dofLabelText;
    snprintf(fogLabelText, sizeof(fogLabelText), "%s", fogModeNames[fogMode]);
    fogLabel = fogLabelText;
}

static void persistAASettings(void* _) {
    aaTaskKey = -1;

    /* Read the CAS slider's bound value here, in the debounced task, rather
     * than in the immediate 'change' handler. RMLUI's data controller writes
     * the fresh slider value back into casStrengthPercent only while it
     * processes the 'change' event, after our handler has already run -- so
     * reading it immediately observes a stale value and makes the slider snap
     * back. By the time this task fires, the bound value is up to date. */
    aaSettings.casStrength = casStrengthPercent / 100.0f;

    engine::rendererSetAASettings(aaSettings);
    aaSettings = engine::rendererGetAASettings();
    syncAAUi();

    utils::settingsSetDouble("aaMode", static_cast<double>(engine::AA_OFF));
    utils::settingsSetDouble("aaCasStrength", static_cast<double>(casStrengthPercent));
    utils::settingsWrite();

    if (model) {
        rmlUpdateDirtyAll(model);
    }
}

static void queueAAPersist(void) {
    if (aaTaskKey != -1) {
        utils::futureTaskRemove(aaTaskKey);
    }
    aaTaskKey = utils::futureTaskAdd(500, persistAASettings, nullptr);
}

static void applyUpscalerModeLater(void* _) {
    upscalerTaskKey = -1;

    engine::RendererUpscalerMode currentMode = engine::rendererGetUpscalerMode();
    if (currentMode == upscalerMode) {
        return;
    }

    engine::rendererSetUpscalerMode(upscalerMode);
    engine::rendererSetAASettings(aaSettings);
    aaSettings   = engine::rendererGetAASettings();
    upscalerMode = engine::rendererGetUpscalerMode();
    syncAAUi();

    utils::settingsSetDouble("upscalerMode", static_cast<double>(upscalerMode));
    /* Enabling the upscaler forces TAA off; persist that so TAA does not
     * silently re-enable itself after a restart. */
    utils::settingsSetBool("taaEnabled", engine::rendererIsTAAEnabled());
    utils::settingsWrite();

    engine::rendererApplyRenderScale();
    if (model) {
        rmlUpdateDirtyAll(model);
    }
}


int upscalerPrev(void* _) {
    int cur      = (int)upscalerMode;
    cur          = (cur + engine::RENDERER_UPSCALER_COUNT - 1) % engine::RENDERER_UPSCALER_COUNT;
    upscalerMode = static_cast<engine::RendererUpscalerMode>(cur);
    syncAAUi();
    rmlUpdateDirtyAll(model);
    if (upscalerTaskKey != -1) {
        utils::futureTaskRemove(upscalerTaskKey);
    }
    upscalerTaskKey = utils::futureTaskAdd(500, applyUpscalerModeLater, nullptr);
    return 0;
}

int upscalerNext(void* _) {
    int cur      = (int)upscalerMode;
    cur          = (cur + 1) % engine::RENDERER_UPSCALER_COUNT;
    upscalerMode = static_cast<engine::RendererUpscalerMode>(cur);
    syncAAUi();
    rmlUpdateDirtyAll(model);
    if (upscalerTaskKey != -1) {
        utils::futureTaskRemove(upscalerTaskKey);
    }
    upscalerTaskKey = utils::futureTaskAdd(500, applyUpscalerModeLater, nullptr);
    return 0;
}

int aaCasStrengthChange(void* _) {
    /* Do not read casStrengthPercent here: RMLUI only writes the fresh slider
     * value back into the bound variable while processing the 'change' event,
     * after this handler has run. Read + apply is deferred to the debounced
     * persist task (see persistAASettings). The label stays live on its own
     * because RMLUI re-renders it as it writes the value back each step. */
    queueAAPersist();
    return 0;
}

static void renderScaleApply(void* _) {
    renderScaleTaskKey = -1;

    float requestedScale = renderScalePercent / 100.0f;
    float appliedScale   = engine::rendererNormalizeRenderScale(requestedScale);
    char scaleChanged    = engine::rendererGetRenderScale() != appliedScale;
    char settingsChanged = utils::settingsGetDouble("renderScale") != static_cast<double>(appliedScale);

    renderScalePercent = appliedScale * 100.0f;

    if (scaleChanged) {
        engine::rendererSetRenderScale(appliedScale);
        engine::rendererApplyRenderScale();
    }
    if (settingsChanged) {
        utils::settingsSetDouble("renderScale", static_cast<double>(appliedScale));
        utils::settingsWrite();
    }
    if (model) {
        rmlUpdateDirtyAll(model);
    }
}

int renderScaleChange(void* _) {
    if (renderScaleDisabled) {
        return 0;
    }

    if (renderScaleTaskKey != -1) {
        utils::futureTaskRemove(renderScaleTaskKey);
    }
    /* Give the bound model value time to update, then debounce expensive
     * render-target recreation until the slider settles. */
    renderScaleTaskKey = utils::futureTaskAdd(500, renderScaleApply, nullptr);
    return 0;
}

static void flushPendingTasks(void) {
    /* Run each pending apply immediately. The map entry must be removed
     * first, otherwise the scheduled task would fire later and apply a
     * second time. Order matters: upscaler mode and render scale may
     * recreate the swapchain; AA settings must come after those so they
     * are not reset by the recreate. */
    if (upscalerTaskKey != -1) {
        utils::futureTaskRemove(upscalerTaskKey);
        applyUpscalerModeLater(nullptr);
    }
    if (renderScaleTaskKey != -1) {
        utils::futureTaskRemove(renderScaleTaskKey);
        renderScaleApply(nullptr);
    }
    if (aaTaskKey != -1) {
        utils::futureTaskRemove(aaTaskKey);
        persistAASettings(nullptr);
    }
    if (lensTaskKey != -1) {
        utils::futureTaskRemove(lensTaskKey);
        persistLensSettings(nullptr);
    }
    if (dofTaskKey != -1) {
        utils::futureTaskRemove(dofTaskKey);
        persistDofSettings(nullptr);
    }
}

int graphicsClose(void* _) {
    utils::futureTaskAddNoParam(0, settingsGuiShow);
    engine::guiManagerRemoveGuiNextFrame(&settingsGraphicsGui);
    return 0;
}

static void syncEffectLabels(void) {
    snprintf(shadowsLabelText, sizeof(shadowsLabelText), "%s",
             shadowQualityNames[engine::vulkanShadowPassGetQuality()]);
    shadowsLabel = shadowsLabelText;
    snprintf(taaLabelText, sizeof(taaLabelText), "%s", engine::rendererIsTAAEnabled() ? "On" : "Off");
    taaLabel = taaLabelText;
    snprintf(lensLabelText, sizeof(lensLabelText), "%s", engine::vulkanLensPassIsDisabled() ? "Off" : "On");
    lensLabel = lensLabelText;
    snprintf(dofLabelText, sizeof(dofLabelText), "%s", engine::vulkanDofPassIsDisabled() ? "Off" : "On");
    dofLabel = dofLabelText;
    snprintf(ssrLabelText, sizeof(ssrLabelText), "%s", engine::vulkanSsrPassIsDisabled() ? "Off" : "On");
    ssrLabel = ssrLabelText;
    snprintf(aoLabelText, sizeof(aoLabelText), "%s", engine::vulkanAOPassIsDisabled() ? "Off" : "On");
    aoLabel = aoLabelText;
    snprintf(giLabelText, sizeof(giLabelText), "%s", engine::vulkanDiffuseGIPassIsDisabled() ? "Off" : "On");
    giLabel = giLabelText;
    snprintf(bloomLabelText, sizeof(bloomLabelText), "%s", engine::vulkanBloomPassIsDisabled() ? "Off" : "On");
    bloomLabel = bloomLabelText;
    snprintf(contactShadowLabelText, sizeof(contactShadowLabelText), "%s", engine::vulkanContactShadowPassIsDisabled() ? "Off" : "On");
    contactShadowLabel = contactShadowLabelText;
    snprintf(fogLabelText, sizeof(fogLabelText), "%s", fogModeNames[fogMode]);
    fogLabel = fogLabelText;
}

static void persistEffectSettings(void) {
    utils::settingsSetInt("shadowQuality", (int)engine::vulkanShadowPassGetQuality());
    /* Keep the legacy on/off key in sync; startup reads it once to migrate
     * old settings files that predate the quality levels. */
    utils::settingsSetBool("shadowsDisabled",
                           engine::vulkanShadowPassGetQuality() == engine::SHADOW_QUALITY_OFF);
    utils::settingsSetBool("ssrDisabled", engine::vulkanSsrPassIsDisabled());
    utils::settingsSetBool("aoDisabled", engine::vulkanAOPassIsDisabled());
    utils::settingsSetBool("giDisabled", engine::vulkanDiffuseGIPassIsDisabled());
    utils::settingsSetBool("bloomDisabled", engine::vulkanBloomPassIsDisabled());
    utils::settingsSetBool("contactShadowDisabled", engine::vulkanContactShadowPassIsDisabled());
    utils::settingsSetDouble("fogMode", static_cast<double>(fogMode));
    utils::settingsWrite();

    /* Apply fog mode to engine */
    engine::VulkanFogData fog = engine::vulkanResourceGetFogData();
    switch (fogMode) {
        case 0: /* Off */
            fog.fogType = 0;
            engine::vulkanVolumetricPassSetDisabled(1);
            break;
        case 1: /* On (exponential fog + god-rays) */
            fog.fogType = 2;
            engine::vulkanVolumetricPassSetDisabled(0);
            break;
    }
    engine::vulkanResourceSetFogData(fog);
}

int toggleShadows(void* _) {
    /* Cycle the quality level: off -> low -> medium -> high -> off. */
    int q = (int)engine::vulkanShadowPassGetQuality() + 1;
    if (q >= engine::SHADOW_QUALITY_COUNT) q = 0;
    engine::vulkanShadowPassSetQuality((engine::ShadowQuality)q);
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleSsr(void* _) {
    engine::vulkanSsrPassSetDisabled(!engine::vulkanSsrPassIsDisabled());
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleAo(void* _) {
    engine::vulkanAOPassSetDisabled(!engine::vulkanAOPassIsDisabled());
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleGi(void* _) {
    engine::vulkanDiffuseGIPassSetDisabled(!engine::vulkanDiffuseGIPassIsDisabled());
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleBloom(void* _) {
    engine::vulkanBloomPassSetDisabled(!engine::vulkanBloomPassIsDisabled());
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

static void persistLensSettings(void* _) {
    lensTaskKey = -1;

    /* Read the bound slider values here (debounced), not in the change
     * handler — RMLUI writes the fresh value back only while processing
     * the 'change' event (same caveat as the CAS slider). */
    engine::vulkanLensPassSetGrain(lensGrainPercent / 100.0f);
    engine::vulkanLensPassSetChromAb(lensChromAbPercent / 100.0f);
    engine::vulkanLensPassSetVignette(lensVignettePercent / 100.0f);

    utils::settingsSetDouble("lensGrain", static_cast<double>(lensGrainPercent));
    utils::settingsSetDouble("lensChromAb", static_cast<double>(lensChromAbPercent));
    utils::settingsSetDouble("lensVignette", static_cast<double>(lensVignettePercent));
    utils::settingsWrite();

    if (model) {
        rmlUpdateDirtyAll(model);
    }
}

int toggleLens(void* _) {
    engine::vulkanLensPassSetDisabled(!engine::vulkanLensPassIsDisabled());
    utils::settingsSetBool("lensEnabled", !engine::vulkanLensPassIsDisabled());
    utils::settingsWrite();
    lensParamsDisabled = engine::vulkanLensPassIsDisabled();
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    return 0;
}

int lensParamChange(void* _) {
    if (lensTaskKey != -1) {
        utils::futureTaskRemove(lensTaskKey);
    }
    lensTaskKey = utils::futureTaskAdd(500, persistLensSettings, nullptr);
    return 0;
}

static void persistDofSettings(void* _) {
    dofTaskKey = -1;

    /* Read the bound slider value here (debounced), not in the change
     * handler — RMLUI writes the fresh value back only while processing
     * the 'change' event (same caveat as the lens sliders). Focus distance
     * is game-driven (camera-to-player), so only quality is user-facing. */
    engine::vulkanDofPassSetQuality((int)dofQuality);

    utils::settingsSetDouble("dofQuality", static_cast<double>(dofQuality));
    utils::settingsWrite();

    if (model) {
        rmlUpdateDirtyAll(model);
    }
}

int toggleDof(void* _) {
    engine::vulkanDofPassSetDisabled(!engine::vulkanDofPassIsDisabled());
    utils::settingsSetBool("dofEnabled", !engine::vulkanDofPassIsDisabled());
    utils::settingsWrite();
    dofParamsDisabled = engine::vulkanDofPassIsDisabled();
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    return 0;
}

int dofParamChange(void* _) {
    if (dofTaskKey != -1) {
        utils::futureTaskRemove(dofTaskKey);
    }
    dofTaskKey = utils::futureTaskAdd(500, persistDofSettings, nullptr);
    return 0;
}

int toggleContactShadow(void* _) {
    engine::vulkanContactShadowPassSetDisabled(!engine::vulkanContactShadowPassIsDisabled());
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleFog(void* _) {
    fogMode = (fogMode + 1) % 2;
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleTaa(void* _) {
    /* TAA and the FSR upscaler are mutually exclusive.  rendererSetAAMode
     * (AA_TAA) forces the upscaler off; enabling the upscaler elsewhere
     * forces TAA off. */
    engine::rendererSetAAMode(engine::rendererGetAAMode() == engine::AA_TAA ? engine::AA_OFF : engine::AA_TAA);
    upscalerMode = engine::rendererGetUpscalerMode();
    syncAAUi();
    rmlUpdateDirtyAll(model);
    utils::settingsSetBool("taaEnabled", engine::rendererIsTAAEnabled());
    utils::settingsSetDouble("upscalerMode", static_cast<double>(upscalerMode));
    utils::settingsWrite();
    return 0;
}

}  // namespace game
