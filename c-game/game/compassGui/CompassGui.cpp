#include "CompassGui.h"
#include "Utils.h"
#include "ecs/Ecs.h"
#include "ecs/system/System.h"
#include "ecs/system/camera/CameraSystem.h"
#include "ecs/system/camera/CameraComponent.h"
#include "player/Player.h"
#include "rmlui/wrapper/src/crmlui.h"
#include "timer/Timer.h"

namespace game {

CompassGui compassGui;

CompassGui::CompassGui() : engine::System("compassGui") {}

static void* document;
static void* model;

#define DP_PER_DEG 3.0f
#define FRAME_W 360.0f
#define HALF_FRAME (FRAME_W / 2.0f)
#define NUM_DIRS 8
#define NUM_TICKS 18
#define COMPASS_SMOOTH_SPEED 10.0f

// Direction label slots
static float dPos[NUM_DIRS];
static float dAlpha[NUM_DIRS];
static char* dText[NUM_DIRS];
static char dTextBuf[NUM_DIRS][4];

// Tick mark slots
static float tPos[NUM_TICKS];
static float tAlpha[NUM_TICKS];
static float tHeight[NUM_TICKS];

// Degree readout
static char degStrBuf[16];
static char* degStr = degStrBuf;

static bool headingInitialized;
static float smoothDeg;

// Cardinal directions: CW from N on the compass rose
// index 0=N, 1=NE, 2=E, 3=SE, 4=S, 5=SW, 6=W, 7=NW
static const char* dirNames[NUM_DIRS] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
// Heading yaw convention: N=0, W=90, S=180, E=270 (CCW)
// Compass CW index to heading degrees: dir i is at (360 - i*45) % 360
static const float dirCamDeg[NUM_DIRS] =
    {0.0f, 315.0f, 270.0f, 225.0f, 180.0f, 135.0f, 90.0f, 45.0f};

void CompassGui::added() {
    document = rmlNewDocument("gui/compass/compass.html");
    model    = rmlCreateModel("compass");

    // Direction labels
    for (int i = 0; i < NUM_DIRS; i++) {
        dAlpha[i]      = 0.0f;
        dPos[i]        = -100.0f;
        dText[i]       = dTextBuf[i];
        dTextBuf[i][0] = '\0';

        char bufD[8], bufA[8], bufT[8];
        snprintf(bufD, sizeof(bufD), "d%d", i);
        snprintf(bufA, sizeof(bufA), "d%da", i);
        snprintf(bufT, sizeof(bufT), "d%dt", i);
        rmlBindFloat(model, bufD, &dPos[i]);
        rmlBindFloat(model, bufA, &dAlpha[i]);
        rmlBindCharPointer(model, bufT, &dText[i]);
    }

    // Tick marks
    for (int i = 0; i < NUM_TICKS; i++) {
        tAlpha[i] = 0.0f;
        tPos[i]   = -100.0f;

        char bufD[8], bufA[8], bufH[8];
        snprintf(bufD, sizeof(bufD), "t%d", i);
        snprintf(bufA, sizeof(bufA), "t%da", i);
        snprintf(bufH, sizeof(bufH), "t%dh", i);
        rmlBindFloat(model, bufD, &tPos[i]);
        rmlBindFloat(model, bufA, &tAlpha[i]);
        rmlBindFloat(model, bufH, &tHeight[i]);
    }

    // Degree readout
    rmlBindCharPointer(model, "compassDegStr", &degStr);

    rmlLoadDocument(document);
    rmlShowDocumentWithoutFocus(document);
}

void CompassGui::removed() {
    headingInitialized = false;

    rmlUnloadDocument(document);
    rmlUnloadModel(model);
    document = nullptr;
    model    = nullptr;
}

// Convert heading degrees to compass CW position (dp from N on the strip).
// Heading: N=0, W=90, S=180, E=270 (CCW when viewed from above).
// Compass strip: CW from N, so strip_dp = (360 - headingDeg) * DP_PER_DEG.
static float headingDegToStripDp(float headingDeg) {
    float stripDeg = fmodf(360.0f - headingDeg, 360.0f);
    if (stripDeg < 0.0f) stripDeg += 360.0f;
    return stripDeg * DP_PER_DEG;
}

static float wrapDeg(float deg) {
    deg = fmodf(deg, 360.0f);
    if (deg < 0.0f) deg += 360.0f;
    return deg;
}

static float smoothAngleDeg(float currentDeg, float targetDeg) {
    float delta = fmodf(targetDeg - currentDeg + 540.0f, 360.0f) - 180.0f;
    if (fabsf(delta) < 0.05f) return currentDeg;

    float t = 1.0f - expf(-COMPASS_SMOOTH_SPEED * utils::timer.dt);
    return wrapDeg(currentDeg + delta * t);
}

void CompassGui::update() {
    float headingYaw = 0.0f;
    if (!playerGetFacingYaw(&headingYaw)) {
        engine::Entity* camEntity = engine::cameraGetEntity();
        if (!camEntity || !camEntity->scene) return;

        engine::Camera* camera = getComponent(camEntity->scene, engine::Camera, camEntity->id);
        if (!camera) return;

        headingYaw = camera->yaw;
    }

    float targetDeg = wrapDeg(glm_deg(headingYaw));
    if (!headingInitialized) {
        smoothDeg          = targetDeg;
        headingInitialized = true;
    } else {
        smoothDeg = smoothAngleDeg(smoothDeg, targetDeg);
    }

    // Use the same rounded heading for the readout and strip layout. This
    // prevents tiny idle-facing fluctuations from moving labels/ticks while
    // the displayed integer degree is unchanged.
    float layoutDeg = wrapDeg(roundf(smoothDeg));

    // Update degree readout
    snprintf(degStrBuf, sizeof(degStrBuf), "%d°", (int)layoutDeg);

    // The center of the frame shows strip position corresponding to current heading.
    // For an element at strip position `s`, its frame position is:
    //   frame_x = HALF_FRAME + (s - headingStripDp)
    float headingDp = headingDegToStripDp(layoutDeg);

    // Place direction labels
    for (int i = 0; i < NUM_DIRS; i++) {
        float s  = headingDegToStripDp(dirCamDeg[i]);
        float fx = HALF_FRAME + (s - headingDp);

        // Wrap into visible range [-margin, FRAME_W + margin]
        float range = 360.0f * DP_PER_DEG;  // one full revolution in dp
        while (fx < -HALF_FRAME) fx += range;
        while (fx > FRAME_W + HALF_FRAME) fx -= range;

        if (fx >= -40.0f && fx <= FRAME_W + 4.0f) {
            dPos[i]   = fx - 18.0f;  // center the 36dp-wide label
            dAlpha[i] = 1.0f;
            snprintf(dTextBuf[i], sizeof(dTextBuf[i]), "%s", dirNames[i]);
        } else {
            dPos[i]        = -100.0f;
            dAlpha[i]      = 0.0f;
            dTextBuf[i][0] = '\0';
        }
    }

    // Place tick marks (every 15° = 45dp, plus small ticks every ~5.625°... simplified to
    // every 22.5° = 67.5dp) We use 18 ticks spread evenly: every 20° = 60dp
    int tickIdx = 0;
    for (int t = 0; t < 36 && tickIdx < NUM_TICKS; t++) {
        float tickDeg = t * 10.0f;  // every 10 degrees
        float s       = headingDegToStripDp(tickDeg);
        float fx      = HALF_FRAME + (s - headingDp);

        float range = 360.0f * DP_PER_DEG;
        while (fx < -HALF_FRAME) fx += range;
        while (fx > FRAME_W + HALF_FRAME) fx -= range;

        if (fx >= -10.0f && fx <= FRAME_W + 10.0f) {
            tPos[tickIdx]    = fx;
            tAlpha[tickIdx]  = 1.0f;
            tHeight[tickIdx] = (t % 3 == 0) ? 14.0f : 8.0f;
            tickIdx++;
        }
    }

    // Hide unused tick slots
    for (int i = tickIdx; i < NUM_TICKS; i++) {
        tPos[i]    = -100.0f;
        tAlpha[i]  = 0.0f;
        tHeight[i] = 8.0f;
    }

    rmlUpdateDirtyAll(model);
}
}  // namespace game
