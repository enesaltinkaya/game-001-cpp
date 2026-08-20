#include "ecs/system/System.h"
#include "ecs/system/lua/LuaSystem.h"
#include "futuretask/FutureTask.h"
#include "renderer/Renderer.h"
#include "renderer/gui/rmlui/GuiManagerRmlUi.h"
#include "renderer/vulkan/pass/bloom/VulkanBloomPass.h"
#include "renderer/vulkan/pass/gtao/VulkanGtaoPass.h"
#include "renderer/vulkan/pass/shadow/VulkanShadowPass.h"
#include "renderer/vulkan/pass/ssr/VulkanSsrPass.h"
#include "renderer/vulkan/pass/contact_shadow/VulkanContactShadowPass.h"
#include "renderer/vulkan/pass/volumetric/VulkanVolumetricPass.h"
#include "renderer/vulkan/resources/VulkanResourceManager.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "settings/Settings.h"
#include "../SettingsGui.h"
#include <stdio.h>

static void added(void);
static void removed(void);
static void syncAAUi(void);
static void persistAASettings(void* _);
static void queueAAPersist(void);
static void applyUpscalerModeLater(void* _);
static void flushPendingTasks(void);
static void renderScaleApply(void* _);

struct System settingsGraphicsGui = {
    .name    = "settingsGraphicsGui",
    .added   = added,
    .removed = removed,
};

static void* document;
static void* model;

static RendererAASettings aaSettings;
static RendererUpscalerMode upscalerMode;
static char renderScaleDisabled;
static float casStrengthPercent;
static float renderScalePercent;
static int upscalerTaskKey    = -1;
static int aaTaskKey          = -1;
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
static char* upscalerLabel;
static char* aaPolicyLabel;
static char* upscalePolicyLabel;
static char* shadowsLabel;
static char* gtaoLabel;
static char* ssrLabel;
static char* bloomLabel;
static char* contactShadowLabel;
static char* fogLabel;
static char* taaLabel;
static int   fogMode;
static char upscalerLabelText[64];
static char aaPolicyLabelText[192];
static char upscalePolicyLabelText[192];
static char shadowsLabelText[16];
static char gtaoLabelText[16];
static char ssrLabelText[16];
static char bloomLabelText[16];
static char contactShadowLabelText[16];
static char fogLabelText[16];
static char taaLabelText[16];

static int upscalerPrev(void* _);
static int upscalerNext(void* _);
static int aaCasStrengthChange(void* _);
static int renderScaleChange(void* _);
static int graphicsClose(void* _);
static int toggleShadows(void* _);
static int toggleGtao(void* _);
static int toggleSsr(void* _);
static int toggleBloom(void* _);
static int toggleContactShadow(void* _);
static int toggleFog(void* _);
static int toggleTaa(void* _);

void added(void) {
    luaRegisterFunction("upscalerPrev", upscalerPrev);
    luaRegisterFunction("upscalerNext", upscalerNext);
    luaRegisterFunction("aaCasStrengthChange", aaCasStrengthChange);
    luaRegisterFunction("renderScaleChange", renderScaleChange);
    luaRegisterFunction("graphicsClose", graphicsClose);
    luaRegisterFunction("toggleShadows", toggleShadows);
    luaRegisterFunction("toggleGtao", toggleGtao);
    luaRegisterFunction("toggleSsr", toggleSsr);
    luaRegisterFunction("toggleBloom", toggleBloom);
    luaRegisterFunction("toggleContactShadow", toggleContactShadow);
    luaRegisterFunction("toggleFog", toggleFog);
    luaRegisterFunction("toggleTaa", toggleTaa);

    /* Read live renderer state rather than stale settings values so that
     * changes made via the debug GUI (or elsewhere) are reflected. */
    aaSettings   = rendererGetAASettings();
    upscalerMode = rendererGetUpscalerMode();
    fogMode      = (int)settingsGetDouble("fogMode");
    if (fogMode < 0 || fogMode > 1) fogMode = 1;
    syncAAUi();

    renderScalePercent = rendererGetRenderScale() * 100.0f;

    document = rmlNewDocument("gui/settings/graphics/graphics.html");
    model    = rmlCreateModel("graphics");
    rmlBindBool(model, "renderScaleDisabled", &renderScaleDisabled);
    rmlBind(model, "upscalerLabel", &upscalerLabel);
    rmlBind(model, "aaPolicyLabel", &aaPolicyLabel);
    rmlBind(model, "upscalePolicyLabel", &upscalePolicyLabel);
    rmlBindFloat(model, "casStrengthPercent", &casStrengthPercent);
    rmlBindFloat(model, "renderScalePercent", &renderScalePercent);
    rmlBind(model, "shadowsLabel", &shadowsLabel);
    rmlBind(model, "gtaoLabel", &gtaoLabel);
    rmlBind(model, "ssrLabel", &ssrLabel);
    rmlBind(model, "bloomLabel", &bloomLabel);
    rmlBind(model, "contactShadowLabel", &contactShadowLabel);
    rmlBind(model, "fogLabel", &fogLabel);
    rmlBind(model, "taaLabel", &taaLabel);
    rmlLoadDocument(document);
    rmlShowDocument(document);
}

void removed(void) {
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
    document = NULL;
    model    = NULL;
}

static void syncAAUi(void) {
    renderScaleDisabled   = rendererIsUpscalerEnabled();
    snprintf(taaLabelText, sizeof(taaLabelText), "%s", rendererIsTAAEnabled() ? "On" : "Off");
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
             rendererIsUpscalerEnabled()
                 ? "FSR quality mode controls the internal render resolution. Manual resolution "
                   "scale is disabled."
                 : "Manual resolution scale is used only when the upscaler is Off.");
    upscalePolicyLabel = upscalePolicyLabelText;

    casStrengthPercent = aaSettings.casStrength * 100.0f;

    snprintf(shadowsLabelText, sizeof(shadowsLabelText), "%s", vulkanShadowPassIsDisabled() ? "Off" : "On");
    shadowsLabel = shadowsLabelText;
    snprintf(gtaoLabelText, sizeof(gtaoLabelText), "%s", vulkanGtaoPassIsDisabled() ? "Off" : "On");
    gtaoLabel = gtaoLabelText;
    snprintf(ssrLabelText, sizeof(ssrLabelText), "%s", vulkanSsrPassIsDisabled() ? "Off" : "On");
    ssrLabel = ssrLabelText;
    snprintf(bloomLabelText, sizeof(bloomLabelText), "%s", vulkanBloomPassIsDisabled() ? "Off" : "On");
    bloomLabel = bloomLabelText;
    snprintf(contactShadowLabelText, sizeof(contactShadowLabelText), "%s", vulkanContactShadowPassIsDisabled() ? "Off" : "On");
    contactShadowLabel = contactShadowLabelText;
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

    rendererSetAASettings(aaSettings);
    aaSettings = rendererGetAASettings();
    syncAAUi();

    settingsSetDouble("aaMode", (double)AA_OFF);
    settingsSetDouble("aaCasStrength", (double)casStrengthPercent);
    settingsWrite();

    if (model) {
        rmlUpdateDirtyAll(model);
    }
}

static void queueAAPersist(void) {
    if (aaTaskKey != -1) {
        futureTaskRemove(aaTaskKey);
    }
    aaTaskKey = futureTaskAdd(500, persistAASettings, NULL);
}

static void applyUpscalerModeLater(void* _) {
    upscalerTaskKey = -1;

    RendererUpscalerMode currentMode = rendererGetUpscalerMode();
    if (currentMode == upscalerMode) {
        return;
    }

    rendererSetUpscalerMode(upscalerMode);
    rendererSetAASettings(aaSettings);
    aaSettings   = rendererGetAASettings();
    upscalerMode = rendererGetUpscalerMode();
    syncAAUi();

    settingsSetDouble("upscalerMode", (double)upscalerMode);
    /* Enabling the upscaler forces TAA off; persist that so TAA does not
     * silently re-enable itself after a restart. */
    settingsSetBool("taaEnabled", rendererIsTAAEnabled());
    settingsWrite();

    rendererApplyRenderScale();
    if (model) {
        rmlUpdateDirtyAll(model);
    }
}


int upscalerPrev(void* _) {
    int cur      = (int)upscalerMode;
    cur          = (cur + RENDERER_UPSCALER_COUNT - 1) % RENDERER_UPSCALER_COUNT;
    upscalerMode = (RendererUpscalerMode)cur;
    syncAAUi();
    rmlUpdateDirtyAll(model);
    if (upscalerTaskKey != -1) {
        futureTaskRemove(upscalerTaskKey);
    }
    upscalerTaskKey = futureTaskAdd(500, applyUpscalerModeLater, NULL);
    return 0;
}

int upscalerNext(void* _) {
    int cur      = (int)upscalerMode;
    cur          = (cur + 1) % RENDERER_UPSCALER_COUNT;
    upscalerMode = (RendererUpscalerMode)cur;
    syncAAUi();
    rmlUpdateDirtyAll(model);
    if (upscalerTaskKey != -1) {
        futureTaskRemove(upscalerTaskKey);
    }
    upscalerTaskKey = futureTaskAdd(500, applyUpscalerModeLater, NULL);
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
    float appliedScale   = rendererNormalizeRenderScale(requestedScale);
    char scaleChanged    = rendererGetRenderScale() != appliedScale;
    char settingsChanged = settingsGetDouble("renderScale") != (double)appliedScale;

    renderScalePercent = appliedScale * 100.0f;

    if (scaleChanged) {
        rendererSetRenderScale(appliedScale);
        rendererApplyRenderScale();
    }
    if (settingsChanged) {
        settingsSetDouble("renderScale", (double)appliedScale);
        settingsWrite();
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
        futureTaskRemove(renderScaleTaskKey);
    }
    /* Give the bound model value time to update, then debounce expensive
     * render-target recreation until the slider settles. */
    renderScaleTaskKey = futureTaskAdd(500, renderScaleApply, NULL);
    return 0;
}

static void flushPendingTasks(void) {
    /* Run each pending apply immediately. The map entry must be removed
     * first, otherwise the scheduled task would fire later and apply a
     * second time. Order matters: upscaler mode and render scale may
     * recreate the swapchain; AA settings must come after those so they
     * are not reset by the recreate. */
    if (upscalerTaskKey != -1) {
        futureTaskRemove(upscalerTaskKey);
        applyUpscalerModeLater(NULL);
    }
    if (renderScaleTaskKey != -1) {
        futureTaskRemove(renderScaleTaskKey);
        renderScaleApply(NULL);
    }
    if (aaTaskKey != -1) {
        futureTaskRemove(aaTaskKey);
        persistAASettings(NULL);
    }
}

int graphicsClose(void* _) {
    futureTaskAddNoParam(0, settingsGuiShow);
    guiManagerRemoveGuiNextFrame(&settingsGraphicsGui);
    return 0;
}

static void syncEffectLabels(void) {
    snprintf(shadowsLabelText, sizeof(shadowsLabelText), "%s", vulkanShadowPassIsDisabled() ? "Off" : "On");
    shadowsLabel = shadowsLabelText;
    snprintf(taaLabelText, sizeof(taaLabelText), "%s", rendererIsTAAEnabled() ? "On" : "Off");
    taaLabel = taaLabelText;
    snprintf(gtaoLabelText, sizeof(gtaoLabelText), "%s", vulkanGtaoPassIsDisabled() ? "Off" : "On");
    gtaoLabel = gtaoLabelText;
    snprintf(ssrLabelText, sizeof(ssrLabelText), "%s", vulkanSsrPassIsDisabled() ? "Off" : "On");
    ssrLabel = ssrLabelText;
    snprintf(bloomLabelText, sizeof(bloomLabelText), "%s", vulkanBloomPassIsDisabled() ? "Off" : "On");
    bloomLabel = bloomLabelText;
    snprintf(contactShadowLabelText, sizeof(contactShadowLabelText), "%s", vulkanContactShadowPassIsDisabled() ? "Off" : "On");
    contactShadowLabel = contactShadowLabelText;
    snprintf(fogLabelText, sizeof(fogLabelText), "%s", fogModeNames[fogMode]);
    fogLabel = fogLabelText;
}

static void persistEffectSettings(void) {
    settingsSetBool("shadowsDisabled", vulkanShadowPassIsDisabled());
    settingsSetBool("gtaoDisabled", vulkanGtaoPassIsDisabled());
    settingsSetBool("ssrDisabled", vulkanSsrPassIsDisabled());
    settingsSetBool("bloomDisabled", vulkanBloomPassIsDisabled());
    settingsSetBool("contactShadowDisabled", vulkanContactShadowPassIsDisabled());
    settingsSetDouble("fogMode", (double)fogMode);
    settingsWrite();

    /* Apply fog mode to engine */
    VulkanFogData fog = vulkanResourceGetFogData();
    switch (fogMode) {
        case 0: /* Off */
            fog.fogType = 0;
            vulkanVolumetricPassSetDisabled(1);
            break;
        case 1: /* On (exponential fog + god-rays) */
            fog.fogType = 2;
            vulkanVolumetricPassSetDisabled(0);
            break;
    }
    vulkanResourceSetFogData(fog);
}

int toggleShadows(void* _) {
    vulkanShadowPassSetDisabled(!vulkanShadowPassIsDisabled());
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleGtao(void* _) {
    vulkanGtaoPassSetDisabled(!vulkanGtaoPassIsDisabled());
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleSsr(void* _) {
    vulkanSsrPassSetDisabled(!vulkanSsrPassIsDisabled());
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleBloom(void* _) {
    vulkanBloomPassSetDisabled(!vulkanBloomPassIsDisabled());
    syncEffectLabels();
    rmlUpdateDirtyAll(model);
    persistEffectSettings();
    return 0;
}

int toggleContactShadow(void* _) {
    vulkanContactShadowPassSetDisabled(!vulkanContactShadowPassIsDisabled());
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
    rendererSetAAMode(rendererGetAAMode() == AA_TAA ? AA_OFF : AA_TAA);
    upscalerMode = rendererGetUpscalerMode();
    syncAAUi();
    rmlUpdateDirtyAll(model);
    settingsSetBool("taaEnabled", rendererIsTAAEnabled());
    settingsSetDouble("upscalerMode", (double)upscalerMode);
    settingsWrite();
    return 0;
}


