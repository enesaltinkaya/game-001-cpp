#include "events/Events.h"
#include "file/File.h"
#include "json/Json.h"
#include "platform/Platform.h"
#include "logger/Logger.h"
#include "string/String.h"
#include <assert.h>
#include <cstdlib>

namespace utils {
struct Template {
    const char* name;
    const char* type;
    double defaultValue;
};

std::vector<Template> templates;

static void writeDefault(void);
static void readSettingsFile(void);
static bool validateSetting(Json* settingsJson, Template* tpl);
static String* settingsPath;
static Json* json;

void settingsInit(void) {
    info("settings: initializing");

    templates.push_back((Template{"effects", "double", 80.}));
    templates.push_back((Template{"music", "double", 40.}));
    templates.push_back((Template{"fpsLimit", "double", 60.}));
    templates.push_back((Template{"fpsLimitChecked", "boolean", 1.}));
    templates.push_back((Template{"uiScale", "double", 0.}));
    templates.push_back((Template{"cursorScale", "double", 0.}));
    templates.push_back((Template{"showFps", "boolean", 1.}));
    templates.push_back((Template{"vsync", "boolean", 0.}));
    templates.push_back((Template{"aaMode", "double", 0.}));
    templates.push_back((Template{"aaCasStrength", "double", 100.}));

    /* TAA is on by default; it forces the FSR upscaler off, so the
     * upscaler default must be Off to match the effective startup state. */
    templates.push_back((Template{"upscalerMode", "double", 0.}));
    templates.push_back((Template{"renderScale", "double", 1.}));
    templates.push_back((Template{"fullScreen", "boolean", 0.}));
    templates.push_back((Template{"busyLoopLinux", "boolean", 1.}));
    templates.push_back((Template{"moreShadows", "boolean", 1.}));
    templates.push_back((Template{"shadowsDisabled", "boolean", 0.}));
    /* Shadow quality: 0=off 1=low (1024/1 cascade/40m) 2=medium (2048/2/80m)
     * 3=high (4096/3/160m).  shadowsDisabled above is the legacy on/off key,
     * kept in sync by the settings GUI and read once for migration. */
    templates.push_back((Template{"shadowQuality", "int", 2.}));
    templates.push_back((Template{"ssrDisabled", "boolean", 0.}));
    templates.push_back((Template{"aoDisabled", "boolean", 0.}));
    /* Screen-space GI (plans/ssgi.md, phases 1-4 validated): on by
     * default; the GUI toggle persists "giDisabled". */
    templates.push_back((Template{"giDisabled", "boolean", 0.}));
    templates.push_back((Template{"bloomDisabled", "boolean", 0.}));
    templates.push_back((Template{"contactShadowDisabled", "boolean", 0.}));
    templates.push_back((Template{"fogMode", "double", 1.}));
    templates.push_back((Template{"taaEnabled", "boolean", 1.}));
    templates.push_back((Template{"taaWeight", "double", 0.9}));
    templates.push_back((Template{"taaGhost", "double", 1.0}));
    templates.push_back((Template{"taaDepth", "double", 0.06}));
    templates.push_back((Template{"lensEnabled", "boolean", 1.}));
    templates.push_back((Template{"lensGrain", "double", 20.0}));
    templates.push_back((Template{"lensChromAb", "double", 20.0}));
    templates.push_back((Template{"lensVignette", "double", 70.0}));
    /* DOF is off by default — a strong aesthetic effect, player's choice.
     * Focus model: focus distance (m), f-number, focal length (mm),
     * blur quality (ring count 1..8). */
    templates.push_back((Template{"dofEnabled", "boolean", 0.}));
    templates.push_back((Template{"dofFocus", "double", 10.0}));
    templates.push_back((Template{"dofFNumber", "double", 2.8}));
    templates.push_back((Template{"dofFocalLength", "double", 50.0}));
    templates.push_back((Template{"dofQuality", "double", 4.0}));

    settingsPath = stringNew("%s%s%s%s", platform.cwd, "data", platform.seperator, "settings.json");
    debug("settings: path %s", settingsPath->data);
    if (!fileExists(settingsPath->data)) {  // file not found, write default
        writeDefault();
    }
    readSettingsFile();

    if (json == nullptr) {  // parse failed, write default
        writeDefault();
        readSettingsFile();
    }

    for (i32 i = 0, si = static_cast<i32>(templates.size()); i < si; i++) {
        Template* tpl = &templates[i];
        if (json_object_get(json, tpl->name) == nullptr) {
            /* New key (tpl added after the user's file was last
             * written): seed the default in place instead of rewriting
             * the whole file, which would clobber the user's other
             * settings. */
            if (strcmp(tpl->type, "boolean") == 0) {
                jsonSetBool(json, tpl->name, (int)tpl->defaultValue);
            } else if (strcmp(tpl->type, "double") == 0) {
                jsonSetDouble(json, tpl->name, tpl->defaultValue);
            } else if (strcmp(tpl->type, "int") == 0) {
                jsonSetInt(json, tpl->name, (int)tpl->defaultValue);
            }
        } else if (!validateSetting(json, tpl)) {  // key type mismatch, write default
            writeDefault();
            readSettingsFile();
            break;
        }
    }
}

void writeDefault(void) {
    Json* settingsJson = jsonNew();

    for (i32 i = 0, si = static_cast<i32>(templates.size()); i < si; i++) {
        Template* tpl = &templates[i];
        if (strcmp(tpl->type, "boolean") == 0) {
            jsonSetBool(settingsJson, tpl->name, (int)tpl->defaultValue);
        }
        if (strcmp(tpl->type, "double") == 0) {
            jsonSetDouble(settingsJson, tpl->name, tpl->defaultValue);
        }
        if (strcmp(tpl->type, "number") == 0) {
            jsonSetInt(settingsJson, tpl->name, (int)tpl->defaultValue);
        }
    }

    char* settingsJsonString = jsonToStringPrettyAlloc(settingsJson);
    fileWrite(settingsPath->data, settingsJsonString);
    jsonFree(settingsJson);
    free(settingsJsonString);

    warn("settings: write default settings");
}

void settingsWrite(void) {
    char* settingsJsonString = jsonToStringPrettyAlloc(json);
    fileWrite(settingsPath->data, settingsJsonString);
    signalEmit("settingsSaved", nullptr);
    free(settingsJsonString);
}

void settingsDestroy(void) {
    stringDestroy(settingsPath);
    jsonFree(json);
}

void readSettingsFile(void) {
    if (json) {
        jsonFree(json);
    }
    String settingsFileString = fileRead(settingsPath->data);
    json                      = jsonParse(settingsFileString.data);
    stringDestroy(&settingsFileString);
}

bool validateSetting(Json* settingsJson, Template* tpl) {
    if (strcmp(tpl->type, "boolean") == 0) {
        if (!jsonIsBool(settingsJson, tpl->name)) {
            return false;
        }
    }

    if (strcmp(tpl->type, "double") == 0) {
        if (!jsonIsDouble(settingsJson, tpl->name)) {
            return false;
        }
    }

    if (strcmp(tpl->type, "int") == 0) {
        if (!jsonIsInt(json, tpl->name)) {
            return false;
        }
    }

    return true;
}

double settingsGetDouble(const char* key) {
    assert(key && "need key");
    assert(jsonIsDouble(json, key) && "not double");
    return jsonGetDouble(json, key);
}

int settingsGetInt(const char* key) {
    assert(key && "need key");
    assert(jsonIsInt(json, key) && "not int");
    return jsonGetInt(json, key);
}

bool settingsGetBool(const char* key) {
    assert(key && "need key");
    assert(jsonIsBool(json, key) && "not bool");
    return jsonGetBool(json, key);
}

void settingsSetInt(const char* key, int value) {
    assert(key && "need key");
    assert(jsonIsInt(json, key) && "not int");
    jsonSetInt(json, key, value);
}

void settingsSetDouble(const char* key, double value) {
    assert(key && "need key");
    assert(jsonIsDouble(json, key) && "not double");
    jsonSetDouble(json, key, value);
}

void settingsSetBool(const char* key, bool value) {
    assert(key && "need key");
    assert(jsonIsBool(json, key) && "not bool");
    jsonSetBool(json, key, value);
}
}  // namespace utils
