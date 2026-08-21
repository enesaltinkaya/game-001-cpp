#include "azgaar/AzgaarWorld.h"
#include "azgaar/Delaunator.h"
#include <jansson.h>
#include <math.h>
#include <ctype.h>

// Defined later in this file; parseRivers (near the top) needs a forward
// declaration since it is inserted before the definition.
namespace game {
static const char* azgaarMapSection(const char* data, u32 size, u32 index, u32* outLen);
static void azgaarHexToRgb(const char* hex, float out[3]);

static double jsonNumberOr(json_t* object, const char* key, double fallback) {
    json_t* value = json_object_get(object, key);
    if (!value) return fallback;
    if (json_is_number(value)) return json_number_value(value);
    if (json_is_string(value)) {
        char* end     = nullptr;
        double parsed = strtod(json_string_value(value), &end);
        if (end && end != json_string_value(value)) return parsed;
    }
    return fallback;
}

static const char* jsonStringOr(json_t* object, const char* key, const char* fallback) {
    json_t* value = json_object_get(object, key);
    if (!value || !json_is_string(value)) return fallback;
    return json_string_value(value);
}

static double unitToMeters(const char* unit) {
    if (strcmp(unit, "km") == 0) return 1000.0;
    if (strcmp(unit, "mi") == 0) return 1609.344;
    if (strcmp(unit, "m") == 0) return 1.0;
    if (strcmp(unit, "ft") == 0) return 0.3048;
    return 1.0;
}

void azgaarMapToWorld(const AzgaarWorld* world,
                      float mapX,
                      float mapY,
                      float* outWx,
                      float* outWz) {
    *outWx = -((mapX - static_cast<float>(world->widthPx) * 0.5f) * static_cast<float>(world->metersPerPixel));
    *outWz = -((mapY - static_cast<float>(world->heightPx) * 0.5f) * static_cast<float>(world->metersPerPixel));
}

float azgaarSeaLevelMeters(const AzgaarWorld* world) {
    // Sea level (h = AZGAAR_SEA_LEVEL_HEIGHT) maps to 0 m of land height.
    // Kept as a function so future height offsets/scales can shift it consistently.
    return azgaarHeightToMeters(world, AZGAAR_SEA_LEVEL_HEIGHT);
}

float azgaarHeightToMeters(const AzgaarWorld* world, float h) {
    float exponent = world && world->heightExponent > 0.0 ? static_cast<float>(world->heightExponent) : 1.0f;

    // Land (h >= sea level): 0 m at the coast, rising via the height exponent.
    // FMG uses (h - 18)^exp; we subtract the coast baseline so h=20 maps to 0 m.
    // The first AZGAAR_WATER_BLEND band is smoothed so the coastline isn't a
    // hard cliff where land cells first emerge.
    if (h >= AZGAAR_SEA_LEVEL_HEIGHT) {
        float coastBase = powf(AZGAAR_SEA_LEVEL_HEIGHT - AZGAAR_HEIGHT_OFFSET, exponent);
        if (h >= AZGAAR_WATER_BLEND_HIGH)
            return powf(h - AZGAAR_HEIGHT_OFFSET, exponent) - coastBase;
        float t =
            (h - AZGAAR_SEA_LEVEL_HEIGHT) / (AZGAAR_WATER_BLEND_HIGH - AZGAAR_SEA_LEVEL_HEIGHT);
        float landH = powf(h - AZGAAR_HEIGHT_OFFSET, exponent) - coastBase;
        return t * landH;
    }

    // Water (h < sea level): seabed below sea level.  Shelves from 0 m at the
    // coast (h=20) down to -AZGAAR_OCEAN_DEPTH_METERS offshore (h=0).
    float f = (AZGAAR_SEA_LEVEL_HEIGHT - h) / AZGAAR_SEA_LEVEL_HEIGHT;  // 0..1
    return -AZGAAR_OCEAN_DEPTH_METERS * f;
}

// ── SVG river-path parsing (workstream C, plans/azgaar-world-population.md) ─
// The .map's section 5 is a single SVG blob holding the river centerlines as
// `<path id="riverN" d="M... C...">`.  Section 32 is a JSON array of river
// metadata.  parseRivers() joins them by id and flattens each cubic at 16
// samples into a map-px polyline (world->rivers[].pointsPx).

#define AZGAAR_RIVER_SVG_FLAT 16u

// Bounded substring search (avoids non-portable memmem).  Returns the first
// occurrence of `needle` in hay[0..hayLen) or nullptr.
static const char* azgaarMemFind(const char* hay, u32 hayLen, const char* needle, u32 needleLen) {
    if (!hay || !needle || needleLen == 0u || needleLen > hayLen) return nullptr;
    for (u32 i = 0; i + needleLen <= hayLen; i++) {
        bool ok = true;
        for (u32 j = 0; j < needleLen; j++) {
            if (hay[i + j] != needle[j]) { ok = false; break; }
        }
        if (ok) return hay + i;
    }
    return nullptr;
}

// Read `n` comma/space separated numbers starting at d[*i]; advance *i past
// them.  Stops (returns false) on a command letter or end-of-string.
static bool azgaarReadSvgCoords(const char* d, u32 len, u32* i, float* out, u32 n) {
    for (u32 k = 0; k < n; k++) {
        while (*i < len && (d[*i] == ' ' || d[*i] == ',')) (*i)++;
        if (*i >= len) return false;
        char ch = d[*i];
        if (ch == 'M' || ch == 'm' || ch == 'L' || ch == 'l' || ch == 'C' || ch == 'c') return false;
        char* end = nullptr;
        double val = strtod(d + *i, &end);
        if (end == d + *i) return false;
        out[k] = static_cast<float>(val);
        *i = static_cast<u32>(end - d);
    }
    return true;
}

// Tokenize an SVG path `d` attribute into a map-px polyline.  Supports
// M/m, L/l, C/c (absolute + relative).  Chilerel rivers use `M` + a chain of
// `C`.  Each cubic is flattened at AZGAAR_RIVER_SVG_FLAT samples so the ribbon
// follows the authored curve.
static void azgaarParseSvgPathD(const char* d, u32 len, float* out, u32* outCount, u32 cap) {
    u32 count = 0;
    float curX = 0.0f, curY = 0.0f;  // running pen position (for relative cmds)
    u32 i = 0;

    while (i < len) {
        char ch = d[i];
        bool isCmd = (ch == 'M' || ch == 'm' || ch == 'L' || ch == 'l' || ch == 'C' || ch == 'c');
        if (!isCmd) { i++; continue; }
        bool rel   = ch == 'm' || ch == 'l' || ch == 'c';
        char cmd   = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
        i++;

        if (cmd == 'C') {
            float c[6];
            if (!azgaarReadSvgCoords(d, len, &i, c, 6)) break;
            float p1x = c[0], p1y = c[1], p2x = c[2], p2y = c[3], p3x = c[4], p3y = c[5];
            if (rel) { p1x += curX; p1y += curY; p2x += curX; p2y += curY; p3x += curX; p3y += curY; }
            // Flatten the cubic Bezier P0=cur -> P1 -> P2 -> P3.
            for (u32 s = 1u; s <= AZGAAR_RIVER_SVG_FLAT; s++) {
                float t  = static_cast<float>(s) / static_cast<float>(AZGAAR_RIVER_SVG_FLAT);
                float it = 1.0f - t;
                float a = it * it * it;
                float b = 3.0f * it * it * t;
                float ccoef = 3.0f * it * t * t;
                float dcoef = t * t * t;
                if (count < cap) {
                    out[count * 2]     = a * curX + b * p1x + ccoef * p2x + dcoef * p3x;
                    out[count * 2 + 1] = a * curY + b * p1y + ccoef * p2y + dcoef * p3y;
                    count++;
                }
            }
            curX = p3x;
            curY = p3y;
        } else { // M or L
            float c[2];
            if (!azgaarReadSvgCoords(d, len, &i, c, 2)) break;
            if (rel) { curX += c[0]; curY += c[1]; }
            else     { curX = c[0]; curY = c[1]; }
            if (count < cap) {
                out[count * 2]     = curX;
                out[count * 2 + 1] = curY;
                count++;
            }
        }
    }
    *outCount = count;
}

struct SvgRiverPath { u32 id; const char* d; u32 dLen; };

// Scan the SVG section for every `<path id="riverN" ... d="...">`, capturing the
// numeric id and the d-attribute (up to the closing quote).
static void azgaarScanSvgRivers(const char* svg, u32 len, SvgRiverPath* out, u32* outCount, u32 cap) {
    u32 count = 0;
    u32 i = 0;
    while (i < len && count < cap) {
        if (svg[i] != '<') { i++; continue; }
        if (strncmp(svg + i, "<path", 5) == 0) {
            u32 tagEnd = i;
            while (tagEnd < len && svg[tagEnd] != '>') tagEnd++;
            u32 tagLen = tagEnd - i;
            const char* tag = svg + i;
            const char* idTok = azgaarMemFind(tag, tagLen, "id=\"river", 9);
            if (idTok) {
                const char* numStart = idTok + 9;
                u32 id = 0;
                while (numStart < tag + tagLen && isdigit(static_cast<unsigned char>(*numStart))) {
                    id = id * 10 + (static_cast<u32>(*numStart++) - '0');
                }
                // Search for ` d="` (leading space) so we don't accidentally match
                // the tail of `id="riverN"` (which also ends in `d="`).
                const char* dTok = azgaarMemFind(tag, tagLen, " d=\"", 4);
                if (dTok) {
                    const char* dVal = dTok + 4;
                    u32 dRemain = static_cast<u32>((tag + tagLen) - dVal);
                    const char* dEnd = static_cast<const char*>(memchr(dVal, '"', dRemain));
                    u32 dLen2 = dEnd ? static_cast<u32>(dEnd - dVal) : dRemain;
                    if (count < cap) {
                        out[count].id   = id;
                        out[count].d    = dVal;
                        out[count].dLen = dLen2;
                        count++;
                    }
                }
            }
            i = tagEnd + 1;
            continue;
        }
        i++;
    }
    *outCount = count;
}

static void parseRivers(AzgaarWorld* world, const char* data, u32 size) {
    u32 svgLen = 0, riversLen = 0;
    const char* svg    = azgaarMapSection(data, size, 5u, &svgLen);
    const char* rivers  = azgaarMapSection(data, size, 32u, &riversLen);

    // Section 32 metadata (JSON array)
    u32 metaCount = 0;
    std::vector<AzgaarRiver> meta;
    if (rivers && riversLen > 0u) {
        json_error_t jerr = {};
        json_t* arr = json_loadb(rivers, riversLen, 0, &jerr);
        if (arr && json_is_array(arr)) {
            metaCount = static_cast<u32>(json_array_size(arr));
            meta.resize(metaCount);
            for (u32 i = 0; i < metaCount; i++) {
                AzgaarRiver* r = &meta[i];
                *r = AzgaarRiver{};
                json_t* obj = json_array_get(arr, i);
                if (!json_is_object(obj)) continue;
                r->id            = static_cast<u32>(jsonNumberOr(obj, "i", static_cast<double>(i + 1)));
                r->source        = static_cast<u32>(jsonNumberOr(obj, "source", 0.0));
                r->mouth         = static_cast<u32>(jsonNumberOr(obj, "mouth", 0.0));
                r->discharge     = static_cast<float>(jsonNumberOr(obj, "discharge", 0.0));
                r->lengthKm      = static_cast<float>(jsonNumberOr(obj, "length", 0.0));
                r->widthPx       = static_cast<float>(jsonNumberOr(obj, "width", 0.0));
                r->widthFactor   = static_cast<float>(jsonNumberOr(obj, "widthFactor", 1.0));
                r->sourceWidthPx = static_cast<float>(jsonNumberOr(obj, "sourceWidth", 0.0));
                r->parent        = static_cast<u32>(jsonNumberOr(obj, "parent", 0.0));
                r->basin         = static_cast<u32>(jsonNumberOr(obj, "basin", 0.0));
                snprintf(r->name, sizeof(r->name), "%s", jsonStringOr(obj, "name", ""));
            }
        }
        if (arr) json_decref(arr);
    }

    // Section 5 SVG polylines (one AzgaarRiver per SVG river path)
    const u32 SVG_CAP = 512u;
    SvgRiverPath svgPaths[SVG_CAP];
    u32 svgCount = 0;
    if (svg && svgLen > 0u) {
        azgaarScanSvgRivers(svg, svgLen, svgPaths, &svgCount, SVG_CAP);
    }

    world->riverCount = svgCount;
    if (svgCount) {
        world->rivers.resize(svgCount);
        // A 4096-float temp (2048 points) comfortably covers the densest path.
        std::vector<float> tmp(4096u);
        for (u32 p = 0; p < svgCount; p++) {
            AzgaarRiver* r = &world->rivers[p];
            *r = AzgaarRiver{};
            r->id = svgPaths[p].id;

            // Merge matching section 32 metadata by id; fall back to a default
            // 4 m width (in px) when the metadata entry is missing.
            const AzgaarRiver* m = nullptr;
            for (u32 k = 0; k < metaCount; k++) {
                if (meta[k].id == r->id) { m = &meta[k]; break; }
            }
            if (m) {
                r->source        = m->source;
                r->mouth         = m->mouth;
                r->discharge     = m->discharge;
                r->lengthKm      = m->lengthKm;
                r->widthPx       = m->widthPx;
                r->widthFactor   = m->widthFactor;
                r->sourceWidthPx = m->sourceWidthPx;
                r->parent        = m->parent;
                r->basin         = m->basin;
                snprintf(r->name, sizeof(r->name), "%s", m->name);
            } else {
                r->widthPx       = 4.0 / static_cast<double>(world->metersPerPixel);
                r->sourceWidthPx = 2.0 / static_cast<double>(world->metersPerPixel);
            }

            u32 pc = 0;
            azgaarParseSvgPathD(svgPaths[p].d, svgPaths[p].dLen, tmp.data(), &pc, 2048u);
            if (pc) {
                r->pointsPx.assign(tmp.data(), tmp.data() + pc * 2);
                r->pointCount = pc;
            }
        }
    }

    u32 withGeom = 0;
    for (u32 p = 0; p < world->riverCount; p++)
        if (world->rivers[p].pointCount) withGeom++;
    utils::info("Azgaar rivers parsed: %u paths, %u with geometry, %u metadata entries",
          world->riverCount, withGeom, metaCount);
}

// FMG burg group strings (section 15 "group" field) -> AzgaarBurgGroup.
// Unknown groups default to village (the most common burg type).
static AzgaarBurgGroup parseBurgGroup(const char* group) {
    if (group && strcmp(group, "capital") == 0) return AZGAAR_BURG_CAPITAL;
    if (group && strcmp(group, "city") == 0) return AZGAAR_BURG_CITY;
    if (group && strcmp(group, "town") == 0) return AZGAAR_BURG_TOWN;
    if (group && strcmp(group, "village") == 0) return AZGAAR_BURG_VILLAGE;
    if (group && strcmp(group, "hamlet") == 0) return AZGAAR_BURG_HAMLET;
    if (group && strcmp(group, "fort") == 0) return AZGAAR_BURG_FORT;
    if (group && strcmp(group, "monastery") == 0) return AZGAAR_BURG_MONASTERY;
    if (group && strcmp(group, "caravanserai") == 0) return AZGAAR_BURG_CARAVANSERAI;
    if (group && strcmp(group, "trading_post") == 0) return AZGAAR_BURG_TRADING_POST;
    return AZGAAR_BURG_VILLAGE;
}

// Truthy test for FMG feature flags: accepts JSON booleans, non-zero numbers,
// or the strings "true"/"1".  Missing keys read as false (D9: failure-tolerant).
static bool azgaarJsonFlag(json_t* obj, const char* key) {
    json_t* v = json_object_get(obj, key);
    if (!v) return false;
    if (json_is_true(v)) return true;
    if (json_is_number(v)) return json_number_value(v) != 0.0;
    if (json_is_string(v)) {
        const char* s = json_string_value(v);
        return strcmp(s, "true") == 0 || strcmp(s, "1") == 0;
    }
    return false;
}

// Named settlements (workstream D of plans/azgaar-world-population.md).
// Parses section 15 (the burg JSON array) into world->settlements: positions
// (x,y) are converted to world space, population drives the footprint radius,
// and the state id resolves the section 14 state colour.  Failure-tolerant:
// a missing/malformed section leaves zero settlements (warn + empty, D9).
static void parseSettlements(AzgaarWorld* world, const char* data, u32 size) {
    u32 len = 0u;
    const char* json = azgaarMapSection(data, size, 15u, &len);
    if (!json || len == 0u) {
        utils::warn("Azgaar .map has no burgs section (15); no settlements");
        return;
    }
    json_error_t jerr = {};
    json_t* arr = json_loadb(json, len, 0, &jerr);
    if (!arr || !json_is_array(arr)) {
        utils::warn("Azgaar .map burgs section is malformed; no settlements (%s)", jerr.text);
        if (arr) json_decref(arr);
        return;
    }
    u32 count = static_cast<u32>(json_array_size(arr));
    if (count == 0u) {
        json_decref(arr);
        return;
    }

    world->settlements.resize(count);
    world->settlementCount = count;
    u32 capital = 0, city = 0, town = 0, village = 0;
    for (u32 i = 0u; i < count; i++) {
        AzgaarSettlement* s = &world->settlements[i];
        *s = AzgaarSettlement{};
        s->id = static_cast<u32>(i + 1);
        json_t* obj = json_array_get(arr, i);
        if (!json_is_object(obj)) continue;
        float x = static_cast<float>(jsonNumberOr(obj, "x", 0.0));
        float y = static_cast<float>(jsonNumberOr(obj, "y", 0.0));
        azgaarMapToWorld(world, x, y, &s->wx, &s->wz);
        float popK = static_cast<float>(jsonNumberOr(obj, "population", 0.0));
        s->populationK = popK;
        // Footprint radius (plan D): hamlet ~15 m, town ~50 m, capital ~120 m.
        s->radiusM = fminf(160.0f, fmaxf(12.0f, 14.0f + 26.0f * sqrtf(fmaxf(popK, 0.0f))));
        // Natural terrain height at the centre, sampled BEFORE any plateau
        // (D8).  Used by the heightmap source's settlement flattening.
        s->flatY = azgaarHeightToMeters(world, azgaarWorldSampleHeightSmooth(world, x, y));
        s->group = parseBurgGroup(jsonStringOr(obj, "group", "village"));
        u32 flags = 0u;
        if (azgaarJsonFlag(obj, "walls"))     flags |= AZGAAR_SETT_FLAG_WALLS;
        if (azgaarJsonFlag(obj, "citadel"))   flags |= AZGAAR_SETT_FLAG_CITADEL;
        if (azgaarJsonFlag(obj, "port"))      flags |= AZGAAR_SETT_FLAG_PORT;
        if (azgaarJsonFlag(obj, "plaza"))     flags |= AZGAAR_SETT_FLAG_PLAZA;
        if (azgaarJsonFlag(obj, "temple"))    flags |= AZGAAR_SETT_FLAG_TEMPLE;
        if (azgaarJsonFlag(obj, "shanty"))    flags |= AZGAAR_SETT_FLAG_SHANTY;
        s->flags     = flags;
        s->stateId   = static_cast<u32>(jsonNumberOr(obj, "state", 0.0));
        s->cultureId = static_cast<u32>(jsonNumberOr(obj, "culture", 0.0));
        // State colour from the section 14 states (fall back to neutral grey).
        // The id indexes the array directly (index 0 is the FMG placeholder),
        // matching azgaarWorldSampleZone's convention.
        if (s->stateId > 0 && s->stateId < world->stateCount && !world->states.empty()) {
            memcpy(s->stateColor, world->states[s->stateId].color, sizeof(s->stateColor));
        } else {
            s->stateColor[0] = 0.6f;
            s->stateColor[1] = 0.6f;
            s->stateColor[2] = 0.6f;
        }
        snprintf(s->name, sizeof(s->name), "%s", jsonStringOr(obj, "name", ""));
        if (s->group == AZGAAR_BURG_CAPITAL) capital++;
        else if (s->group == AZGAAR_BURG_CITY) city++;
        else if (s->group == AZGAAR_BURG_TOWN) town++;
        else village++;
    }
    json_decref(arr);
    utils::info("Azgaar settlements parsed: %u total (capitals=%u cities=%u towns=%u villages/other=%u)",
         count, capital, city, town, village);
}

// ── Markers (section 35, workstream E landmarks) ───────────────────────────

// The .map stores marker `type` as a plural string; accept the singular
// fallbacks too so hand-edited maps still classify.  sacred-pineries are
// sacred-highland-forest markers in FMG and get the same treatment.
static AzgaarMarkerKind parseMarkerKind(const char* type) {
    if (!type) return AZGAAR_MARKER_OTHER;
    if (strcmp(type, "volcanoes") == 0 || strcmp(type, "volcano") == 0) return AZGAAR_MARKER_VOLCANO;
    if (strcmp(type, "lighthouses") == 0 || strcmp(type, "lighthouse") == 0) return AZGAAR_MARKER_LIGHTHOUSE;
    if (strcmp(type, "hot-springs") == 0 || strcmp(type, "hot-spring") == 0) return AZGAAR_MARKER_HOT_SPRING;
    if (strcmp(type, "water-sources") == 0 || strcmp(type, "water-source") == 0) return AZGAAR_MARKER_WATER_SOURCE;
    if (strcmp(type, "ruins") == 0 || strcmp(type, "ruin") == 0) return AZGAAR_MARKER_RUINS;
    if (strcmp(type, "mines") == 0 || strcmp(type, "mine") == 0) return AZGAAR_MARKER_MINE;
    if (strcmp(type, "bridges") == 0 || strcmp(type, "bridge") == 0) return AZGAAR_MARKER_BRIDGE;
    if (strcmp(type, "sacred-forests") == 0 || strcmp(type, "sacred-forest") == 0 ||
        strcmp(type, "sacred-pineries") == 0 || strcmp(type, "sacred-pinery") == 0) {
        return AZGAAR_MARKER_SACRED_FOREST;
    }
    return AZGAAR_MARKER_OTHER;
}

// Parse section 35 markers into world->markers (positions -> world space).
// All markers are kept (kind OTHER included) so ids stay stable; the landmark
// system filters by kind.  Failure-tolerant: a missing section leaves zero
// markers.
static void parseMarkers(AzgaarWorld* world, const char* data, u32 size) {
    u32 len = 0u;
    const char* json = azgaarMapSection(data, size, 35u, &len);
    if (!json || len == 0u) {
        utils::warn("Azgaar .map has no markers section (35); no landmarks");
        return;
    }
    json_error_t jerr = {};
    json_t* arr = json_loadb(json, len, 0, &jerr);
    if (!arr || !json_is_array(arr)) {
        utils::warn("Azgaar .map markers section is malformed; no landmarks (%s)", jerr.text);
        if (arr) json_decref(arr);
        return;
    }
    u32 count = static_cast<u32>(json_array_size(arr));
    if (count == 0u) {
        json_decref(arr);
        return;
    }

    world->markers.resize(count);
    world->markerCount = count;
    u32 volcano = 0, light = 0, spring = 0, water = 0, ruins = 0, mine = 0, bridge = 0,
        sacred = 0, other = 0;
    for (u32 i = 0u; i < count; i++) {
        AzgaarMarker* m = &world->markers[i];
        *m = AzgaarMarker{};
        m->id   = static_cast<u32>(i + 1u);
        json_t* obj = json_array_get(arr, i);
        if (!json_is_object(obj)) continue;
        m->kind = parseMarkerKind(jsonStringOr(obj, "type", ""));
        float x = static_cast<float>(jsonNumberOr(obj, "x", 0.0));
        float y = static_cast<float>(jsonNumberOr(obj, "y", 0.0));
        m->x    = x;
        m->y    = y;
        azgaarMapToWorld(world, x, y, &m->wx, &m->wz);
        m->cell = static_cast<u32>(jsonNumberOr(obj, "cell", 0.0));
        m->size = static_cast<float>(jsonNumberOr(obj, "size", 1.0));
        if (m->kind == AZGAAR_MARKER_VOLCANO) volcano++;
        else if (m->kind == AZGAAR_MARKER_LIGHTHOUSE) light++;
        else if (m->kind == AZGAAR_MARKER_HOT_SPRING) spring++;
        else if (m->kind == AZGAAR_MARKER_WATER_SOURCE) water++;
        else if (m->kind == AZGAAR_MARKER_RUINS) ruins++;
        else if (m->kind == AZGAAR_MARKER_MINE) mine++;
        else if (m->kind == AZGAAR_MARKER_BRIDGE) bridge++;
        else if (m->kind == AZGAAR_MARKER_SACRED_FOREST) sacred++;
        else other++;
    }
    json_decref(arr);
    utils::info("Azgaar markers parsed: %u total (volcanoes=%u lighthouses=%u hot-springs=%u "
         "water-sources=%u ruins=%u mines=%u bridges=%u sacred-forests=%u other/skipped=%u)",
         count, volcano, light, spring, water, ruins, mine, bridge, sacred, other);
}

// Parse pack.cells into a parallel array used only for zone (province/state)
// lookups. Positions and ids come straight from the pack; index 0 is skipped
// naturally because it has no zone data worth showing.
static AzgaarRouteGroup parseRouteGroup(const char* group) {
    if (group && strcmp(group, "trails") == 0) return AZGAAR_ROUTE_TRAIL;
    if (group && strcmp(group, "searoutes") == 0) return AZGAAR_ROUTE_SEAROUTE;
    return AZGAAR_ROUTE_ROAD;
}

static bool parseRoutesArray(AzgaarWorld* world, json_t* routes) {
    if (!json_is_array(routes)) return false;

    world->routeCount = static_cast<u32>(json_array_size(routes));
    if (!world->routeCount) return true;

    world->routes.resize(world->routeCount);
    u32 roadCount = 0, trailCount = 0, seaCount = 0;

    for (u32 i = 0; i < world->routeCount; ++i) {
        AzgaarRoute* route = &world->routes[i];
        *route             = AzgaarRoute{};
        json_t* obj        = json_array_get(routes, i);
        if (!json_is_object(obj)) continue;

        const char* group = jsonStringOr(obj, "group", "roads");
        route->group      = parseRouteGroup(group);
        if (route->group == AZGAAR_ROUTE_ROAD)
            roadCount++;
        else if (route->group == AZGAAR_ROUTE_TRAIL)
            trailCount++;
        else
            seaCount++;

        snprintf(route->name, sizeof(route->name), "%s", jsonStringOr(obj, "name", ""));
        route->length  = static_cast<float>(jsonNumberOr(obj, "length", 0.0));
        route->feature = static_cast<u32>(jsonNumberOr(obj, "feature", 0.0));

        json_t* points = json_object_get(obj, "points");
        if (!json_is_array(points)) continue;
        route->pointCount = static_cast<u32>(json_array_size(points));
        if (!route->pointCount) continue;
        route->points.resize(route->pointCount);

        for (u32 p = 0; p < route->pointCount; ++p) {
            json_t* point = json_array_get(points, p);
            if (json_is_array(point) && json_array_size(point) >= 2) {
                route->points[p].x = static_cast<float>(json_number_value(json_array_get(point, 0)));
                route->points[p].y = static_cast<float>(json_number_value(json_array_get(point, 1)));
                if (json_array_size(point) >= 3) {
                    route->points[p].cell = static_cast<u32>(json_number_value(json_array_get(point, 2)));
                }
            } else if (json_is_object(point)) {
                route->points[p].x    = static_cast<float>(jsonNumberOr(point, "x", 0.0));
                route->points[p].y    = static_cast<float>(jsonNumberOr(point, "y", 0.0));
                route->points[p].cell = static_cast<u32>(jsonNumberOr(point, "cell", 0.0));
            }
        }
    }

    utils::info("Azgaar routes parsed: roads=%u trails=%u searoutes=%u", roadCount, trailCount, seaCount);
    return true;
}

static void parseNamedRegions(AzgaarNamedRegion* out, u32 count, json_t* arr) {
    for (u32 i = 0; i < count; i++) {
        out[i].name[0] = '\0';
        json_t* entry  = json_array_get(arr, i);
        // Azgaar uses index 0 as a placeholder (e.g. provinces[0] is the bare
        // integer 0); non-object entries are left with an empty name.
        if (!json_is_object(entry)) continue;
        const char* name = jsonStringOr(entry, "name", "");
        snprintf(out[i].name, sizeof(out[i].name), "%s", name);
        // Authored region colour (FMG hex string).  States feed the settlement
        // trim/band colour (workstream D); provinces ignore it.
        const char* color = jsonStringOr(entry, "color", nullptr);
        if (color) {
            azgaarHexToRgb(color, out[i].color);
        } else {
            out[i].color[0] = 0.6f;
            out[i].color[1] = 0.6f;
            out[i].color[2] = 0.6f; // neutral grey fallback
        }
    }
}

static i32 clampGridIndex(i32 i, u32 count) {
    if (i < 0) return 0;
    if (i >= static_cast<i32>(count)) return static_cast<i32>(count) - 1;
    return i;
}

static float catmullRom(float p0, float p1, float p2, float p3, float t) {
    float t2 = t * t;
    float t3 = t2 * t;
    return 0.5f * ((2.0f * p1) + (-p0 + p2) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t2 +
                   (-p0 + 3.0f * p1 - 3.0f * p2 + p3) * t3);
}

static bool buildCellGrid(AzgaarWorld* world) {
    if (world->cells.empty() || world->cellCount == 0) return false;
    if (world->widthPx <= 0.0 || world->heightPx <= 0.0) return false;

    const float area = static_cast<float>(world->widthPx * world->heightPx);
    float spacing    = sqrtf(area / static_cast<float>(world->cellCount));
    if (spacing < 1.0f) spacing = 1.0f;

    AzgaarCellGrid* g = &world->cellGrid;
    g->bucketSize     = spacing;
    g->invBucketSize  = 1.0f / spacing;
    g->cols           = static_cast<u32>(ceilf(static_cast<float>(world->widthPx) / spacing));
    g->rows           = static_cast<u32>(ceilf(static_cast<float>(world->heightPx) / spacing));
    if (g->cols == 0) g->cols = 1;
    if (g->rows == 0) g->rows = 1;
    const u32 bucketCount = g->cols * g->rows;

    std::vector<u32> counts(bucketCount, 0u);
    std::vector<u32> cellBucket(world->cellCount);

    for (u32 i = 0; i < world->cellCount; i++) {
        i32 bx = static_cast<i32>(world->cells[i].x * g->invBucketSize);
        i32 by = static_cast<i32>(world->cells[i].y * g->invBucketSize);
        if (bx < 0)
            bx = 0;
        else if (bx >= static_cast<i32>(g->cols))
            bx = static_cast<i32>(g->cols) - 1;
        if (by < 0)
            by = 0;
        else if (by >= static_cast<i32>(g->rows))
            by = static_cast<i32>(g->rows) - 1;
        u32 b         = static_cast<u32>(by) * g->cols + static_cast<u32>(bx);
        cellBucket[i] = b;
        counts[b]++;
    }

    g->bucketStart.resize(bucketCount + 1u);
    u32 sum        = 0;
    for (u32 i = 0; i < bucketCount; i++) {
        g->bucketStart[i] = sum;
        sum += counts[i];
    }
    g->bucketStart[bucketCount] = sum;

    g->bucketCells.resize(world->cellCount);
    for (u32 i = 0; i < bucketCount; i++) counts[i] = 0;
    for (u32 i = 0; i < world->cellCount; i++) {
        u32 b               = cellBucket[i];
        u32 pos             = g->bucketStart[b] + counts[b]++;
        g->bucketCells[pos] = i;
    }

    return true;
}

static void azgaarVisitBucket(const AzgaarWorld* world,
                              const AzgaarCellGrid* g,
                              i32 i,
                              i32 j,
                              float xPx,
                              float yPx,
                              u32 K,
                              float* dist2,
                              float* h,
                              bool* exact,
                              float* exactH) {
    if (i < 0 || j < 0 || i >= static_cast<i32>(g->cols) || j >= static_cast<i32>(g->rows)) return;
    u32 b = static_cast<u32>(j) * g->cols + static_cast<u32>(i);
    u32 s = g->bucketStart[b];
    u32 e = g->bucketStart[b + 1];
    for (u32 n = s; n < e; n++) {
        const AzgaarCell* cell = &world->cells[g->bucketCells[n]];
        float dx               = cell->x - xPx;
        float dy               = cell->y - yPx;
        float d                = dx * dx + dy * dy;
        if (d < 0.0001f) {
            *exact  = true;
            *exactH = cell->height;
            return;
        }
        u32 worst = 0;
        for (u32 k = 1; k < K; k++) {
            if (dist2[k] > dist2[worst]) worst = k;
        }
        if (d < dist2[worst]) {
            dist2[worst] = d;
            h[worst]     = cell->height;
        }
    }
}

static void azgaarKNearestCells(const AzgaarWorld* world,
                                float xPx,
                                float yPx,
                                u32 K,
                                float* dist2,
                                float* h,
                                bool* exact,
                                float* exactH) {
    const AzgaarCellGrid* g = &world->cellGrid;

    if (g->bucketStart.empty() || g->bucketCells.empty() || g->cols == 0 || g->rows == 0) {
        for (u32 i = 0; i < world->cellCount; i++) {
            const AzgaarCell* cell = &world->cells[i];
            float dx               = cell->x - xPx;
            float dy               = cell->y - yPx;
            float d                = dx * dx + dy * dy;
            if (d < 0.0001f) {
                *exact  = true;
                *exactH = cell->height;
                return;
            }
            u32 worst = 0;
            for (u32 k = 1; k < K; k++) {
                if (dist2[k] > dist2[worst]) worst = k;
            }
            if (d < dist2[worst]) {
                dist2[worst] = d;
                h[worst]     = cell->height;
            }
        }
        return;
    }

    // Clamp the query to the data extent (see nearestTwoCellIndices): keeps
    // the ring search O(1) for the many samples that fall outside the map.
    if (xPx < 0.0f)
        xPx = 0.0f;
    else if (xPx > static_cast<float>(world->widthPx))
        xPx = static_cast<float>(world->widthPx);
    if (yPx < 0.0f)
        yPx = 0.0f;
    else if (yPx > static_cast<float>(world->heightPx))
        yPx = static_cast<float>(world->heightPx);

    i32 bx = static_cast<i32>(xPx * g->invBucketSize);
    i32 by = static_cast<i32>(yPx * g->invBucketSize);
    if (bx < 0)
        bx = 0;
    else if (bx >= static_cast<i32>(g->cols))
        bx = static_cast<i32>(g->cols) - 1;
    if (by < 0)
        by = 0;
    else if (by >= static_cast<i32>(g->rows))
        by = static_cast<i32>(g->rows) - 1;

    const i32 maxR = static_cast<i32>(g->cols > g->rows ? g->cols : g->rows);
    const float bs = g->bucketSize;

    for (i32 r = 0; r < maxR; r++) {
        if (r == 0) {
            azgaarVisitBucket(world, g, bx, by, xPx, yPx, K, dist2, h, exact, exactH);
        } else {
            i32 i0 = bx - r, i1 = bx + r;
            i32 j0 = by - r, j1 = by + r;
            for (i32 i = i0; i <= i1; i++) {
                azgaarVisitBucket(world, g, i, j0, xPx, yPx, K, dist2, h, exact, exactH);
                azgaarVisitBucket(world, g, i, j1, xPx, yPx, K, dist2, h, exact, exactH);
            }
            for (i32 j = j0 + 1; j < j1; j++) {
                azgaarVisitBucket(world, g, i0, j, xPx, yPx, K, dist2, h, exact, exactH);
                azgaarVisitBucket(world, g, i1, j, xPx, yPx, K, dist2, h, exact, exactH);
            }
        }

        if (*exact) return;

        float dmax = dist2[0];
        for (u32 k = 1; k < K; k++) {
            if (dist2[k] > dmax) dmax = dist2[k];
        }
        float thr = static_cast<float>(r) * bs;
        thr       = thr * thr;
        if (dmax <= thr) return;
    }
}

static void nearestTwoConsider(const AzgaarWorld* world,
                               u32 cellIndex,
                               float xPx,
                               float yPx,
                               float bestDist2[2],
                               u32 bestIndex[2]) {
    const AzgaarCell* cell = &world->cells[cellIndex];
    float dx               = cell->x - xPx;
    float dy               = cell->y - yPx;
    float d                = dx * dx + dy * dy;

    if (d < bestDist2[0]) {
        bestDist2[1] = bestDist2[0];
        bestIndex[1] = bestIndex[0];
        bestDist2[0] = d;
        bestIndex[0] = cellIndex;
    } else if (d < bestDist2[1]) {
        bestDist2[1] = d;
        bestIndex[1] = cellIndex;
    }
}

static void visitNearestTwoBucket(const AzgaarWorld* world,
                                  const AzgaarCellGrid* g,
                                  i32 i,
                                  i32 j,
                                  float xPx,
                                  float yPx,
                                  float bestDist2[2],
                                  u32 bestIndex[2]) {
    if (i < 0 || j < 0 || i >= static_cast<i32>(g->cols) || j >= static_cast<i32>(g->rows)) return;
    u32 b = static_cast<u32>(j) * g->cols + static_cast<u32>(i);
    for (u32 n = g->bucketStart[b]; n < g->bucketStart[b + 1]; n++) {
        nearestTwoConsider(world, g->bucketCells[n], xPx, yPx, bestDist2, bestIndex);
    }
}

static void nearestTwoCellIndices(const AzgaarWorld* world,
                                  float xPx,
                                  float yPx,
                                  u32 bestIndex[2],
                                  float bestDist2[2]) {
    bestIndex[0] = bestIndex[1] = (u32)-1;
    bestDist2[0] = bestDist2[1] = FLT_MAX;
    if (!world || world->cells.empty() || world->cellCount == 0) return;

    const AzgaarCellGrid* g = &world->cellGrid;
    if (g->bucketStart.empty() || g->bucketCells.empty() || g->cols == 0 || g->rows == 0) {
        for (u32 i = 0; i < world->cellCount; i++) {
            nearestTwoConsider(world, i, xPx, yPx, bestDist2, bestIndex);
        }
        return;
    }

    // Clamp the query to the data extent. Streamed terrain tiles are far larger
    // than the Azgaar map world, so most height samples land well outside the
    // map; without clamping the expanding ring search has to walk out to the
    // query distance and degrades to ~O(cellCount) per sample. Out-of-map
    // points resolve to the nearest boundary cell, which is the right height
    // for an edge sample anyway.
    if (xPx < 0.0f)
        xPx = 0.0f;
    else if (xPx > static_cast<float>(world->widthPx))
        xPx = static_cast<float>(world->widthPx);
    if (yPx < 0.0f)
        yPx = 0.0f;
    else if (yPx > static_cast<float>(world->heightPx))
        yPx = static_cast<float>(world->heightPx);

    i32 bx = static_cast<i32>(xPx * g->invBucketSize);
    i32 by = static_cast<i32>(yPx * g->invBucketSize);
    if (bx < 0)
        bx = 0;
    else if (bx >= static_cast<i32>(g->cols))
        bx = static_cast<i32>(g->cols) - 1;
    if (by < 0)
        by = 0;
    else if (by >= static_cast<i32>(g->rows))
        by = static_cast<i32>(g->rows) - 1;

    const i32 maxR = static_cast<i32>(g->cols > g->rows ? g->cols : g->rows);
    const float bs = g->bucketSize;
    for (i32 r = 0; r < maxR; r++) {
        if (r == 0) {
            visitNearestTwoBucket(world, g, bx, by, xPx, yPx, bestDist2, bestIndex);
        } else {
            i32 i0 = bx - r, i1 = bx + r;
            i32 j0 = by - r, j1 = by + r;
            for (i32 i = i0; i <= i1; i++) {
                visitNearestTwoBucket(world, g, i, j0, xPx, yPx, bestDist2, bestIndex);
                visitNearestTwoBucket(world, g, i, j1, xPx, yPx, bestDist2, bestIndex);
            }
            for (i32 j = j0 + 1; j < j1; j++) {
                visitNearestTwoBucket(world, g, i0, j, xPx, yPx, bestDist2, bestIndex);
                visitNearestTwoBucket(world, g, i1, j, xPx, yPx, bestDist2, bestIndex);
            }
        }

        float thr = static_cast<float>(r) * bs;
        if (bestIndex[1] != (u32)-1 && bestDist2[1] <= thr * thr) return;
    }
}

static u32 nearestCellIndex(const AzgaarWorld* world, float xPx, float yPx) {
    u32 bestIndex[2];
    float bestDist2[2];
    nearestTwoCellIndices(world, xPx, yPx, bestIndex, bestDist2);
    return bestIndex[0];
}

float azgaarWorldSampleHeightNearest(const AzgaarWorld* world, float xPx, float yPx) {
    u32 cellIndex = nearestCellIndex(world, xPx, yPx);
    return cellIndex != (u32)-1 ? world->cells[cellIndex].height : 0.0f;
}

float azgaarWorldSampleHeightCell(const AzgaarWorld* world,
                                  float xPx,
                                  float yPx,
                                  u32* outCellIndex) {
    u32 cellIndex = nearestCellIndex(world, xPx, yPx);
    if (outCellIndex) *outCellIndex = cellIndex;
    return cellIndex != (u32)-1 ? world->cells[cellIndex].height : 0.0f;
}

float azgaarWorldSampleHeightSmooth(const AzgaarWorld* world, float xPx, float yPx) {
    if (!world || world->cells.empty() || world->cellCount == 0) return 0.0f;

    if (!world->heightGrid.empty() && world->heightGridWidth > 1 && world->heightGridHeight > 1) {
        float cellW = static_cast<float>(world->widthPx) / static_cast<float>(world->heightGridWidth);
        float cellH = static_cast<float>(world->heightPx) / static_cast<float>(world->heightGridHeight);
        float gx    = xPx / cellW - 0.5f;
        float gy    = yPx / cellH - 0.5f;

        i32 x1   = static_cast<i32>(floorf(gx));
        i32 y1   = static_cast<i32>(floorf(gy));
        float tx = gx - static_cast<float>(x1);
        float ty = gy - static_cast<float>(y1);

        if (x1 < 0) {
            x1 = 0;
            tx = 0.0f;
        }
        if (y1 < 0) {
            y1 = 0;
            ty = 0.0f;
        }
        if (x1 >= static_cast<i32>(world->heightGridWidth) - 1) {
            x1 = static_cast<i32>(world->heightGridWidth) - 2;
            tx = 1.0f;
        }
        if (y1 >= static_cast<i32>(world->heightGridHeight) - 1) {
            y1 = static_cast<i32>(world->heightGridHeight) - 2;
            ty = 1.0f;
        }

        u32 w = world->heightGridWidth;
        float rows[4];
        for (i32 r = -1; r <= 2; r++) {
            i32 sy = clampGridIndex(y1 + r, world->heightGridHeight);
            float p[4];
            for (i32 c = -1; c <= 2; c++) {
                i32 sx   = clampGridIndex(x1 + c, world->heightGridWidth);
                p[c + 1] = world->heightGrid[static_cast<u32>(sy) * w + static_cast<u32>(sx)];
            }
            rows[r + 1] = catmullRom(p[0], p[1], p[2], p[3], tx);
        }

        float h = catmullRom(rows[0], rows[1], rows[2], rows[3], ty);
        if (h < 0.0f)
            h = 0.0f;
        else if (h > 100.0f)
            h = 100.0f;
        return h;
    }

    enum { K = 16 };

    float dist2[K];
    float h[K];
    for (u32 k = 0; k < K; k++) {
        dist2[k] = FLT_MAX;
        h[k]     = 0.0f;
    }
    bool exact   = false;
    float exactH = 0.0f;
    azgaarKNearestCells(world, xPx, yPx, K, dist2, h, &exact, &exactH);
    if (exact) return exactH;

    const float cellSpacing = world->cellGrid.bucketSize > 0.0f ? world->cellGrid.bucketSize : 1.0f;
    const float softR       = cellSpacing * 1.5f;
    const float soft2       = softR * softR;

    float weighted  = 0.0f;
    float weightSum = 0.0f;
    for (u32 k = 0; k < K; k++) {
        if (dist2[k] == FLT_MAX) continue;
        float w = 1.0f / (dist2[k] + soft2);
        weighted += h[k] * w;
        weightSum += w;
    }

    return weightSum > 0.0f ? weighted / weightSum : 0.0f;
}

void azgaarWorldSampleZone(const AzgaarWorld* world,
                           float xPx,
                           float yPx,
                           AzgaarZoneInfo* out,
                           u32* outPackCellIndex) {
    if (out) {
        out->provinceId   = 0u;
        out->stateId      = 0u;
        out->provinceName = "";
        out->stateName    = "";
    }
    if (outPackCellIndex) *outPackCellIndex = 0u;
    if (!world || world->packCells.empty() || world->packCellCount == 0u || !out) return;

    // One brute-force nearest-cell query per frame is trivial for ~5k pack
    // cells, so there is no need for a second spatial grid here.
    u32 best        = 0u;
    float bestDist2 = FLT_MAX;
    for (u32 i = 0u; i < world->packCellCount; i++) {
        const AzgaarPackCell* pc = &world->packCells[i];
        float dx                 = pc->x - xPx;
        float dy                 = pc->y - yPx;
        float d                  = dx * dx + dy * dy;
        if (d < bestDist2) {
            bestDist2 = d;
            best      = i;
        }
    }

    out->provinceId = world->packCells[best].province;
    out->stateId    = world->packCells[best].state;
    if (outPackCellIndex) *outPackCellIndex = best;
    if (out->provinceId > 0u && out->provinceId < world->provinceCount && !world->provinces.empty()) {
        out->provinceName = world->provinces[out->provinceId].name;
    }
    if (out->stateId > 0u && out->stateId < world->stateCount && !world->states.empty()) {
        out->stateName = world->states[out->stateId].name;
    }
}

// ===================== .map (native Azgaar save) loading =====================
//
// Azgaar's native ".map" save is a text format: pipe-delimited lines joined by
// CRLF.  Crucially, the grid stores the point positions and per-cell scalar
// arrays (height, biome, ...) but NOT the Voronoi vertices / cell adjacency -
// FMG recomputes those on load from a Delaunay triangulation of the points.
// We do the same here so a .map yields the same AzgaarWorld structure as the
// JSON export path above.

// Returns a pointer (within `data`) and length of the k-th CRLF-delimited line.
static const char* azgaarMapSection(const char* data, u32 size, u32 index, u32* outLen) {
    u32 start = 0;
    u32 cur   = 0;
    while (start <= size) {
        u32 end = start;
        while (end < size && !(data[end] == '\r' && end + 1u < size && data[end + 1u] == '\n'))
            end++;
        if (cur == index) {
            *outLen = end - start;
            return data + start;
        }
        cur++;
        if (end + 1u >= size) return nullptr;
        start = end + 2u;
    }
    return nullptr;
}

// Returns a pointer (within `line`) and length of the k-th pipe-delimited field.
static const char* azgaarMapField(const char* line, u32 len, u32 index, u32* outLen) {
    u32 start = 0;
    u32 cur   = 0;
    while (start <= len) {
        u32 end = start;
        while (end < len && line[end] != '|') end++;
        if (cur == index) {
            *outLen = end - start;
            return line + start;
        }
        cur++;
        if (end >= len) return nullptr;
        start = end + 1u;
    }
    return nullptr;
}

static double azgaarMapFieldDouble(const char* line, u32 len, u32 index, double fallback) {
    u32 fLen      = 0;
    const char* f = azgaarMapField(line, len, index, &fLen);
    if (!f || fLen == 0 || fLen >= 64) return fallback;
    char buf[64];
    memcpy(buf, f, fLen);
    buf[fLen] = '\0';
    char* end = nullptr;
    double v  = strtod(buf, &end);
    if (end == buf) return fallback;
    return v;
}

static void azgaarMapFieldStr(const char* line,
                              u32 len,
                              u32 index,
                              char* out,
                              u32 outSize,
                              const char* fallback) {
    u32 fLen      = 0;
    const char* f = azgaarMapField(line, len, index, &fLen);
    if (!f || fLen == 0) {
        snprintf(out, outSize, "%s", fallback);
        return;
    }
    u32 n = fLen < outSize - 1u ? fLen : outSize - 1u;
    memcpy(out, f, n);
    out[n] = '\0';
}

// Parse a comma-separated signed-integer section (FMG .map stores the grid
// cell scalar arrays this way) into `out[0..count)`.  Missing/short sections
// leave the buffer zeroed.  Tolerates stray commas / whitespace like the
// height parser does.
static void azgaarParseCsvInts(const char* csv, u32 csvLen, i32* out, u32 count) {
    if (!csv || csvLen == 0u || !out || count == 0u) return;
    u32 i            = 0u;
    const char* s    = csv;
    const char* endp = csv + csvLen;
    while (i < count && s < endp) {
        while (s < endp && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) s++;
        if (s >= endp) break;
        char* term = nullptr;
        long v     = strtol(s, &term, 10);
        if (term == s) break;
        out[i++] = static_cast<i32>(v);
        s        = term;
        while (s < endp && *s == ',') s++;
    }
}

// Convert a "#rrggbb" hex colour to [0,1] RGB.  Returns neutral grey on error.
static void azgaarHexToRgb(const char* hex, float out[3]) {
    out[0] = out[1] = out[2] = 0.8f;
    if (!hex) return;
    const char* h   = (hex[0] == '#') ? hex + 1 : hex;
    char* end       = nullptr;
    unsigned long v = strtoul(h, &end, 16);
    if (end == h || end - h < 6) return;
    out[0] = ((v >> 16) & 0xff) / 255.0f;
    out[1] = ((v >> 8) & 0xff) / 255.0f;
    out[2] = (v & 0xff) / 255.0f;
}

// FMG biome classification matrix — rows are moisture bands [0..4] (dry→wet),
// columns are temperature bands [0..25] (hot→cold, index = clamp(20-temp,0,25)).
// Values are biome ids (0=Marine … 12=Wetland).  Lifted verbatim from FMG's
// biomes-generator.ts `biomesMatrix` so our per-grid-cell classification matches
// what Fantasy Map Generator renders.
static const u8 AZGAAR_BIOME_MATRIX[5][26] = {
    {1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 10},
    {3, 3, 3, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 9, 9, 9, 9, 10, 10, 10},
    {5, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 9, 9, 9, 9, 9, 10, 10, 10},
    {5, 6, 6, 6, 6, 6, 6, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 10, 10, 10},
    {7, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9, 10, 10},
};

// FMG `Biomes.getId` — same rules, same matrix.  `moisture`/`temperature` use
// FMG units (precipitation-ish, °C); height is the FMG 0..100 cell height.
static u32 azgaarBiomeId(float moisture, float temperature, float height) {
    if (height < 20.0f) return 0u;                           // water → Marine
    if (temperature < -5.0f) return 11u;                     // too cold → Glacier
    if (temperature >= 25.0f && moisture < 8.0f) return 1u;  // hot+dry → Hot desert
    // Wetland: near-coast very wet, or inland wet lowlands/midlands.
    if (temperature > -2.0f) {
        if (moisture > 40.0f && height < 25.0f) return 12u;
        if (moisture > 24.0f && height > 24.0f && height < 60.0f) return 12u;
    }
    u32 moistureBand = static_cast<u32>(moisture / 5.0f);
    if (moistureBand > 4u) moistureBand = 4u;
    float tb = 20.0f - temperature;
    if (tb < 0.0f)
        tb = 0.0f;
    else if (tb > 25.0f)
        tb = 25.0f;
    u32 tempBand = static_cast<u32>(tb);
    return static_cast<u32>(AZGAAR_BIOME_MATRIX[moistureBand][tempBand]);
}

void azgaarWorldBiomeColor(const AzgaarWorld* world, u32 biomeId, float outColor[3]) {
    if (world && !world->biomes.empty() && biomeId < world->biomeCount) {
        outColor[0] = world->biomes[biomeId].color[0];
        outColor[1] = world->biomes[biomeId].color[1];
        outColor[2] = world->biomes[biomeId].color[2];
    } else {
        outColor[0] = outColor[1] = outColor[2] = 0.8f;
    }
}

void azgaarWorldSampleBiomeColorSmooth(const AzgaarWorld* world,
                                       float xPx,
                                       float yPx,
                                       float outColor[3]) {
    if (world && !world->biomeColorGrid.empty() && world->biomeColorGridWidth > 1u &&
        world->biomeColorGridHeight > 1u) {
        // Bilinear at texel centres (same addressing as the height grid).  The
        // grid is already low-passed at bake time, so bilinear is enough and —
        // unlike the bicubic used for heights — cannot overshoot into invalid
        // (negative / >1) colours.
        float cellW = static_cast<float>(world->widthPx) / static_cast<float>(world->biomeColorGridWidth);
        float cellH = static_cast<float>(world->heightPx) / static_cast<float>(world->biomeColorGridHeight);
        float gx    = xPx / cellW - 0.5f;
        float gy    = yPx / cellH - 0.5f;

        i32 x1   = static_cast<i32>(floorf(gx));
        i32 y1   = static_cast<i32>(floorf(gy));
        float tx = gx - static_cast<float>(x1);
        float ty = gy - static_cast<float>(y1);
        if (x1 < 0) {
            x1 = 0;
            tx = 0.0f;
        }
        if (y1 < 0) {
            y1 = 0;
            ty = 0.0f;
        }
        if (x1 >= static_cast<i32>(world->biomeColorGridWidth) - 1) {
            x1 = static_cast<i32>(world->biomeColorGridWidth) - 2;
            tx = 1.0f;
        }
        if (y1 >= static_cast<i32>(world->biomeColorGridHeight) - 1) {
            y1 = static_cast<i32>(world->biomeColorGridHeight) - 2;
            ty = 1.0f;
        }

        u32 w   = world->biomeColorGridWidth;
        u32 gi  = static_cast<u32>(y1) * w + static_cast<u32>(x1);
        const u8* p00 = world->biomeColorGrid.data() + static_cast<size_t>(gi) * 3u;
        const u8* p10 = p00 + 3u;
        const u8* p01 = world->biomeColorGrid.data() + static_cast<size_t>(gi + w) * 3u;
        const u8* p11 = p01 + 3u;

        for (u32 c = 0u; c < 3u; c++) {
            float top    = static_cast<float>(p00[c]) + (static_cast<float>(p10[c]) - static_cast<float>(p00[c])) * tx;
            float bottom = static_cast<float>(p01[c]) + (static_cast<float>(p11[c]) - static_cast<float>(p01[c])) * tx;
            float v      = (top + (bottom - top) * ty) * (1.0f / 255.0f);
            outColor[c]  = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
        }
        return;
    }

    // No colour grid (old saves / load failure): flat nearest-cell colour.
    u32 cellIndex =
        (world && !world->cells.empty() && world->cellCount) ? nearestCellIndex(world, xPx, yPx) : (u32)-1;
    if (cellIndex == (u32)-1) {
        outColor[0] = 0.42f;
        outColor[1] = 0.46f;
        outColor[2] = 0.42f;
        return;
    }
    azgaarWorldBiomeColor(world, world->cells[cellIndex].biome, outColor);
}

// Texel-centre bilinear sample of a float grid at the pre-computed grid
// coordinates (same addressing as azgaarWorldSampleBiomeColorSmooth).
// Coordinates must already be clamped to [0, w-2] x [0, h-2] by the caller.
static float azgaarSampleGridBilinear(const float* grid,
                                      u32 w,
                                      i32 x1,
                                      i32 y1,
                                      float tx,
                                      float ty) {
    const u32 gi  = static_cast<u32>(y1) * w + static_cast<u32>(x1);
    const float a = grid[gi];
    const float b = grid[gi + 1u];
    const float c = grid[gi + w];
    const float d = grid[gi + w + 1u];
    const float top    = a + (b - a) * tx;
    const float bottom = c + (d - c) * tx;
    return top + (bottom - top) * ty;
}

void azgaarWorldSampleClimate(const AzgaarWorld* world,
                              float xPx,
                              float yPx,
                              AzgaarClimateSample* out) {
    if (out) *out = AzgaarClimateSample{.temperature = 0.0f,
                                          .precipitation = 0.0f,
                                          .coastCells = 0.0f,
                                          .biome = AZGAAR_BIOME_NONE};
    if (!world || !out) return;

    if (!world->tempGrid.empty() && world->climateGridWidth > 1u && world->climateGridHeight > 1u) {
        u32 w = world->climateGridWidth;
        u32 h = world->climateGridHeight;
        float cellW = static_cast<float>(world->widthPx) / static_cast<float>(w);
        float cellH = static_cast<float>(world->heightPx) / static_cast<float>(h);
        float gx    = xPx / cellW - 0.5f;
        float gy    = yPx / cellH - 0.5f;

        i32 x1   = static_cast<i32>(floorf(gx));
        i32 y1   = static_cast<i32>(floorf(gy));
        float tx = gx - static_cast<float>(x1);
        float ty = gy - static_cast<float>(y1);
        if (x1 < 0) {
            x1 = 0;
            tx = 0.0f;
        }
        if (y1 < 0) {
            y1 = 0;
            ty = 0.0f;
        }
        if (x1 >= static_cast<i32>(w) - 1) {
            x1 = static_cast<i32>(w) - 2;
            tx = 1.0f;
        }
        if (y1 >= static_cast<i32>(h) - 1) {
            y1 = static_cast<i32>(h) - 2;
            ty = 1.0f;
        }

        out->temperature   = azgaarSampleGridBilinear(world->tempGrid.data(), w, x1, y1, tx, ty);
        out->precipitation = azgaarSampleGridBilinear(world->precGrid.data(), w, x1, y1, tx, ty);
        out->coastCells    = azgaarSampleGridBilinear(world->coastGrid.data(), w, x1, y1, tx, ty);
        out->biome         = world->biomeGrid[static_cast<u32>(y1) * w + static_cast<u32>(x1)];
        return;
    }

    // No climate grids (old saves / missing sections): flat per-cell values.
    if (!world->cells.empty() && world->cellCount) {
        u32 i = nearestCellIndex(world, xPx, yPx);
        if (i != (u32)-1) {
            out->temperature   = world->cells[i].temp;
            out->precipitation = world->cells[i].prec;
            out->coastCells    = world->cells[i].coast;
            out->biome         = world->cells[i].biome;
        }
    }
}

static u8 azgaarFloatToU8(float v) {
    v += 0.5f;
    if (v <= 0.0f) return 0u;
    if (v >= 255.0f) return 255u;
    return static_cast<u8>(v);
}

std::vector<u8> azgaarWorldPackClimateTexture(const AzgaarWorld* world, u32* outWidth, u32* outHeight) {
    std::vector<u8> px;
    if (!world || world->tempGrid.empty() || !world->climateGridWidth || !world->climateGridHeight) {
        return px;
    }
    const u32 w     = world->climateGridWidth;
    const u32 h     = world->climateGridHeight;
    const size_t n  = static_cast<size_t>(w) * h;
    px.resize(n * 4u);
    const float tBias = 64.0f;   // deg C    -> byte (filter-safe, see header)
    const float cBias = 11.0f;   // cell units -> byte
    for (size_t i = 0; i < n; i++) {
        px[i * 4u + 0u] = azgaarFloatToU8(world->tempGrid[i] + tBias);
        px[i * 4u + 1u] = azgaarFloatToU8(world->precGrid[i]);
        px[i * 4u + 2u] = azgaarFloatToU8(world->coastGrid[i] + cBias);
        px[i * 4u + 3u] = world->biomeGrid[i];
    }
    if (outWidth) *outWidth = w;
    if (outHeight) *outHeight = h;
    return px;
}

std::vector<u8> azgaarWorldPackBiomeColorTexture(const AzgaarWorld* world, u32* outWidth, u32* outHeight) {
    std::vector<u8> px;
    if (!world || world->biomeColorGrid.empty() || !world->biomeColorGridWidth ||
        !world->biomeColorGridHeight) {
        return px;
    }
    const u32 w    = world->biomeColorGridWidth;
    const u32 h    = world->biomeColorGridHeight;
    const size_t n = static_cast<size_t>(w) * h;
    px.resize(n * 4u);
    for (size_t i = 0; i < n; i++) {
        px[i * 4u + 0u] = world->biomeColorGrid[i * 3u + 0u];
        px[i * 4u + 1u] = world->biomeColorGrid[i * 3u + 1u];
        px[i * 4u + 2u] = world->biomeColorGrid[i * 3u + 2u];
        px[i * 4u + 3u] = 255u;
    }
    if (outWidth) *outWidth = w;
    if (outHeight) *outHeight = h;
    return px;
}

// Next half-edge inside the same triangle (delaunator convention).
[[maybe_unused]]
static inline u32 delHalfNext(u32 e) {
    return e % 3u == 2u ? e - 2u : e + 1u;
}

// Voronoi vertex = Delaunay triangle circumcenter (matches voronoi.ts).
[[maybe_unused]]
static void voronoiCircumcenter(double ax,
                                double ay,
                                double bx,
                                double by,
                                double cx,
                                double cy,
                                double* outX,
                                double* outY) {
    double dx = bx - ax, dy = by - ay;
    double ex = cx - ax, ey = cy - ay;
    double bl    = dx * dx + dy * dy;
    double cl    = ex * ex + ey * ey;
    double denom = dx * ey - dy * ex;
    if (fabs(denom) < DBL_MIN) {
        *outX = ax;
        *outY = ay;
        return;
    }
    double d = 0.5 / denom;
    *outX    = ax + (ey * bl - dy * cl) * d;
    *outY    = ay + (dx * cl - ex * bl) * d;
}

// Reconstruct the Voronoi mesh from the already-loaded grid points:
//   - world->cells[p].vertices[]   ordered ring of Voronoi-vertex indices
//                                  around cell p (the cell polygon)
//   - world->cells[p].neighbors[]  adjacent cell indices
//   - world->vertices[t]           circumcenter of Delaunay triangle t,
//                                  with .cells[] = the 3 cells meeting there
//
// Cell positions/heights are already filled by the caller and are preserved —
// only adjacency + Voronoi vertices are derived here.  This is what the
// flat-cell terrain renderer walks to emit one watertight polygon per cell.
// ── Voronoi by half-plane intersection ─────────────────────────────────
// The previous implementation derived cells from the in-tree Delaunator
// port, but that port produces invalid (non-Delaunay) triangles for this
// point set, which made cell polygons span huge distances and overlap each
// other.  Instead we compute each cell's polygon directly: a Voronoi cell is
// the set of points closer to its site than to any other site, i.e. the
// intersection of half-planes bounded by the perpendicular bisectors with
// its neighbours.  This is exact, non-overlapping by construction, and needs
// only the (already correct) spatial hash to enumerate each cell's nearby
// sites — no Delaunay at all.
//
// For each cell p we start from a box around its site and Sutherland–Hodgman
// clip it against every neighbour site within a few buckets.  The resulting
// polygon vertices are stored in world->vertices (one contiguous run per
// cell; not shared between cells) and referenced by cell->vertices[], which
// is exactly what the flat-cell terrain renderer consumes.

struct VoroPt {
    float x, y;
};

#define VORO_MAX_VERTS 64u

// Clip polygon (in-place) to the half-plane of points closer to site S than
// to site Q: keep points X where dot(X - mid, Q - S) <= 0, mid = (S+Q)/2.
static u32 voroClipByBisector(VoroPt* poly, u32 n, float sx, float sy, float qx, float qy) {
    if (n == 0u) return 0u;
    float mx = (sx + qx) * 0.5f;
    float my = (sy + qy) * 0.5f;
    float nx = qx - sx;
    float ny = qy - sy;
    VoroPt out[VORO_MAX_VERTS + 1u];
    u32 m = 0u;
    for (u32 i = 0u; i < n; i++) {
        VoroPt a = poly[i];
        VoroPt b = poly[(i + 1u) % n];
        float da = (a.x - mx) * nx + (a.y - my) * ny;
        float db = (b.x - mx) * nx + (b.y - my) * ny;
        bool aIn = da <= 0.0f;
        bool bIn = db <= 0.0f;
        if (aIn) {
            if (m < (VORO_MAX_VERTS + 1u)) out[m++] = a;
            if (!bIn) {
                float t   = da / (da - db);
                VoroPt ip = {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
                if (m < (VORO_MAX_VERTS + 1u)) out[m++] = ip;
            }
        } else if (bIn) {
            float t   = da / (da - db);
            VoroPt ip = {a.x + t * (b.x - a.x), a.y + t * (b.y - a.y)};
            if (m < (VORO_MAX_VERTS + 1u)) out[m++] = ip;
        }
    }
    if (m > VORO_MAX_VERTS) m = VORO_MAX_VERTS;
    for (u32 i = 0u; i < m; i++) poly[i] = out[i];
    return m;
}

static bool azgaarWorldBuildVoronoi(AzgaarWorld* world) {
    if (world->cells.empty() || world->cellCount == 0u) return false;
    if (world->cellGrid.bucketSize <= 0.0f) return false;  // needs buildCellGrid first

    const AzgaarCellGrid* g = &world->cellGrid;
    const float spacing     = g->bucketSize;
    // Start box half-extent: a few spacings — large enough to contain the cell
    // after clipping, small enough to stay cheap.
    const float boxR = spacing * 3.0f;
    // Neighbour search radius in buckets. 3 buckets (~3 spacings) is plenty:
    // a cell's true Voronoi neighbours are within ~1 spacing.
    const i32 bucketRadius = 3;

    // First pass: compute each cell's polygon vertex count so we can allocate
    // a contiguous world->vertices run per cell.
    std::vector<std::vector<VoroPt>> cellPoly(world->cellCount);

    for (u32 p = 0u; p < world->cellCount; p++) {
        float sx = world->cells[p].x;
        float sy = world->cells[p].y;
        i32 bcx  = static_cast<i32>(sx * g->invBucketSize);
        i32 bcy  = static_cast<i32>(sy * g->invBucketSize);

        VoroPt poly[VORO_MAX_VERTS];
        u32 n   = 4u;
        poly[0] = VoroPt{sx - boxR, sy - boxR};
        poly[1] = VoroPt{sx + boxR, sy - boxR};
        poly[2] = VoroPt{sx + boxR, sy + boxR};
        poly[3] = VoroPt{sx - boxR, sy + boxR};

        for (i32 dy = -bucketRadius; dy <= bucketRadius; dy++) {
            for (i32 dx = -bucketRadius; dx <= bucketRadius; dx++) {
                i32 bx = bcx + dx, by = bcy + dy;
                if (bx < 0 || bx >= static_cast<i32>(g->cols) || by < 0 || by >= static_cast<i32>(g->rows)) continue;
                u32 b = static_cast<u32>(by) * g->cols + static_cast<u32>(bx);
                u32 s = g->bucketStart[b];
                u32 e = (b + 1u < (g->cols * g->rows)) ? g->bucketStart[b + 1u] : world->cellCount;
                for (u32 k = s; k < e; k++) {
                    u32 q = g->bucketCells[k];
                    if (q == p) continue;
                    n = voroClipByBisector(poly, n, sx, sy, world->cells[q].x, world->cells[q].y);
                    if (n == 0u) break;
                }
                if (n == 0u) break;
            }
            if (n == 0u) break;
        }

        if (n < 3u) continue;  // degenerate / fully clipped (shouldn't happen)
        for (u32 k = 0u; k < n; k++) cellPoly[p].push_back(poly[k]);
        world->cells[p].neighborCount = 0u;  // neighbours unused by the plate renderer
    }

    // Commit: give each cell a contiguous run of world->vertices and an index
    // list (cell->vertices[]) pointing into it.  Vertices are NOT shared
    // between cells (half-plane polygons meet at shared edges but emit their
    // own copies), which is exactly what the per-plate renderer expects.
    u32 totalVerts = 0u;
    for (u32 p = 0u; p < world->cellCount; p++) totalVerts += static_cast<i32>(cellPoly[p].size());
    world->gridVertexCount = totalVerts;
    world->vertices.resize(totalVerts ? totalVerts : 1u);

    u32 cursor = 0u;
    for (u32 p = 0u; p < world->cellCount; p++) {
        u32 vc = static_cast<i32>(cellPoly[p].size());
        if (vc == 0u) continue;
        world->cells[p].vertexCount = vc;
        world->cells[p].vertices.resize(vc);
        for (u32 k = 0u; k < vc; k++) {
            world->vertices[cursor]     = AzgaarVertex{};
            world->vertices[cursor].x   = cellPoly[p][k].x;
            world->vertices[cursor].y   = cellPoly[p][k].y;
            world->cells[p].vertices[k] = cursor;
            cursor++;
        }
    }
    return true;
}

// Generic separable-Gaussian low-pass over a float grid (clamped edges).
// Shared by the height grid and the climate grids (workstream A): they all
// start as nearest-cell Voronoi plateaus and need the same ~0.35 x FMG sample
// spacing cut-off so their transitions land at the same scale.
// `envName` optionally overrides sigma in texels for live tuning.
static void azgaarGaussianBlurGrid(AzgaarWorld* world,
                                   std::vector<float>& grid,
                                   u32 w,
                                   u32 h,
                                   const char* envName,
                                   const char* label) {
    if (grid.empty() || w < 3u || h < 3u) return;

    // Average FMG sample spacing, expressed in grid texels.
    const float areaPx     = static_cast<float>(world->widthPx) * static_cast<float>(world->heightPx);
    const float spacingPx  = world->cellCount ? sqrtf(areaPx / static_cast<float>(world->cellCount)) : 1.0f;
    const float spacingTex = spacingPx * (static_cast<float>(w) / static_cast<float>(world->widthPx));

    // σ = 0.35 × sample spacing: the plateau square wave's fundamental lands
    // at ~-21 dB (invisible on distant silhouettes) while >=2-cell relief
    // keeps ~55% and >=3-cell ~77% of its amplitude, so cell-scale variation
    // stays visible.  (0.5× was tried first: waves gone, but the whole map
    // read as unnaturally smooth.)  The env var (texels) overrides
    // for live tuning without a rebuild.
    float sigma          = spacingTex * 0.35f;
    const char* envSigma = getenv(envName ? envName : "");
    if (envSigma) {
        float s = static_cast<float>(atof(envSigma));
        if (s > 0.0f) sigma = s;
    }
    if (sigma < 1.0f) sigma = 1.0f;  // tiny maps: still soften the 1-texel walls
    if (sigma > 6.0f) sigma = 6.0f;  // huge maps: don't erase real relief
    const i32 radius = static_cast<i32>(ceilf(sigma * 2.5f));

    enum { AZGAAR_BLUR_KERNEL_MAX = 2 * 15 + 1 };  // ceil(6.0 * 2.5) = 15 max radius

    float kernel[AZGAAR_BLUR_KERNEL_MAX];
    float sum = 0.0f;
    for (i32 i = -radius; i <= radius; ++i) {
        float g            = expf(-(static_cast<float>(i * i)) / (2.0f * sigma * sigma));
        kernel[i + radius] = g;
        sum += g;
    }
    for (i32 i = 0; i < 2 * radius + 1; ++i) kernel[i] /= sum;

    std::vector<float> tmp(static_cast<size_t>(w) * h);
    double t0  = utils::nanos();

    // Horizontal pass (clamped edges), grid -> tmp.
    for (u32 y = 0u; y < h; ++y) {
        const float* row = grid.data() + static_cast<size_t>(y) * w;
        float* outRow    = tmp.data() + static_cast<size_t>(y) * w;
        for (u32 x = 0u; x < w; ++x) {
            float acc = 0.0f;
            for (i32 k = -radius; k <= radius; ++k) {
                i32 sx = static_cast<i32>(x) + k;
                if (sx < 0) sx = 0;
                if (sx >= static_cast<i32>(w)) sx = static_cast<i32>(w) - 1;
                acc += row[sx] * kernel[k + radius];
            }
            outRow[x] = acc;
        }
    }
    // Vertical pass (clamped edges), tmp -> grid.
    for (u32 y = 0u; y < h; ++y) {
        float* outRow = grid.data() + static_cast<size_t>(y) * w;
        for (u32 x = 0u; x < w; ++x) {
            float acc = 0.0f;
            for (i32 k = -radius; k <= radius; ++k) {
                i32 sy = static_cast<i32>(y) + k;
                if (sy < 0) sy = 0;
                if (sy >= static_cast<i32>(h)) sy = static_cast<i32>(h) - 1;
                acc += tmp[static_cast<size_t>(sy) * w + x] * kernel[k + radius];
            }
            outRow[x] = acc;
        }
    }

    utils::info("Azgaar %s grid smoothed: %ux%u, spacing=%.1f texels, sigma=%.1f (radius=%d) in %.1f "
         "ms",
         label,
         w,
         h,
         spacingTex,
         sigma,
         radius,
         (utils::nanos() - t0) / 1e6);
}

// Low-pass the rasterized height grid.
//
// The .map stores one Uint8 height per FMG grid point (spacing ~8 px here,
// i.e. ~800 m in world space).  Nearest-cell rasterization turns that into
// Voronoi plateaus separated by 1-texel walls, so the terrain reads as a
// rounded staircase ("wave/sawtooth" hills) whose period is the FMG sample
// spacing.  A separable Gaussian low-pass kills the plateau edges (the
// ~800 m square wave) while leaving the real relief — which is band-limited
// to >= one sample spacing anyway — intact.
//
// Coastline note: blurring across the sea-level boundary gently smooths
// shores (no more cliffed plateau edges at the coast); islets only one FMG
// cell wide may sink to sandbars.  Water depth sampling shares this grid, so
// terrain, roads and water all stay consistent.
//
// Each blur touches only its own grid vector, so independent grids are run as
// jobs on the default thread pool (see azgaarBlurJobFn).
struct AzgaarBlurJob {
    AzgaarWorld* world;
    std::vector<float>* grid;
    u32 w;
    u32 h;
    const char* envName;
    const char* label;
};

static void azgaarBlurJobFn(void* userData) {
    AzgaarBlurJob* job = static_cast<AzgaarBlurJob*>(userData);
    azgaarGaussianBlurGrid(job->world, *job->grid, job->w, job->h, job->envName, job->label);
}

// Biome-colour counterpart of azgaarSmoothHeightGrid.  The raw colour grid is
// nearest-cell Voronoi plateaus, so every biome border renders as a pixel-hard
// hue seam once the terrain tint samples it per texel.  A separable Gaussian
// blends neighbouring biomes over ~one FMG cell spacing (same 0.35 × spacing
// rationale as the height grid).  A land/water mask (height grid vs sea
// level) keeps land hues from bleeding into the Marine seabed tint and vice
// versa: land texels average only land neighbours (so each blend extends to
// the coastline), while water texels keep their flat Marine colour and the
// shoreline itself stays crisp.
static void azgaarSmoothBiomeColorGrid(AzgaarWorld* world) {
    const u32 w = world->biomeColorGridWidth;
    const u32 h = world->biomeColorGridHeight;
    if (world->biomeColorGrid.empty() || world->heightGrid.empty() || w < 3u || h < 3u) return;

    const float areaPx     = static_cast<float>(world->widthPx) * static_cast<float>(world->heightPx);
    const float spacingPx  = world->cellCount ? sqrtf(areaPx / static_cast<float>(world->cellCount)) : 1.0f;
    const float spacingTex = spacingPx * (static_cast<float>(w) / static_cast<float>(world->widthPx));

    // σ = 0.35 × sample spacing (see azgaarSmoothHeightGrid).
    // ENGINE_AZGAAR_TINT_SIGMA (texels) overrides for live tuning.
    float sigma          = spacingTex * 0.35f;
    const char* envSigma = getenv("ENGINE_AZGAAR_TINT_SIGMA");
    if (envSigma) {
        float s = static_cast<float>(atof(envSigma));
        if (s > 0.0f) sigma = s;
    }
    if (sigma < 1.0f) sigma = 1.0f;  // tiny maps: still soften the 1-texel walls
    if (sigma > 6.0f) sigma = 6.0f;  // huge maps: don't erase biome identity
    const i32 radius = static_cast<i32>(ceilf(sigma * 2.5f));

    enum { AZGAAR_TINT_KERNEL_MAX = 2 * 15 + 1 };  // ceil(6.0 * 2.5) = 15 max radius

    float kernel[AZGAAR_TINT_KERNEL_MAX];
    float sum = 0.0f;
    for (i32 i = -radius; i <= radius; ++i) {
        float g            = expf(-(static_cast<float>(i * i)) / (2.0f * sigma * sigma));
        kernel[i + radius] = g;
        sum += g;
    }
    for (i32 i = 0; i < 2 * radius + 1; ++i) kernel[i] /= sum;

    // Packed RGBW accumulators: colour channels are pre-multiplied by the
    // land mask, W accumulates the mask weight used to renormalize the
    // average (so partial spans still yield a convex combination).
    std::vector<float> tmp(4u * static_cast<size_t>(w) * h);
    double t0  = utils::nanos();

    // Horizontal pass (clamped edges), grid -> tmp.
    for (u32 y = 0u; y < h; ++y) {
        for (u32 x = 0u; x < w; ++x) {
            float accR = 0.0f, accG = 0.0f, accB = 0.0f, accW = 0.0f;
            for (i32 k = -radius; k <= radius; ++k) {
                i32 sx = static_cast<i32>(x) + k;
                if (sx < 0) sx = 0;
                if (sx >= static_cast<i32>(w)) sx = static_cast<i32>(w) - 1;
                u32 gi = y * w + static_cast<u32>(sx);
                if (world->heightGrid[gi] < AZGAAR_SEA_LEVEL_HEIGHT) continue;  // water: no bleed
                float kk      = kernel[k + radius];
                const u8* px = world->biomeColorGrid.data() + static_cast<size_t>(gi) * 3u;
                accR += static_cast<float>(px[0]) * kk;
                accG += static_cast<float>(px[1]) * kk;
                accB += static_cast<float>(px[2]) * kk;
                accW += kk;
            }
            float* out = tmp.data() + (static_cast<size_t>(y) * w + x) * 4u;
            out[0]     = accR;
            out[1]     = accG;
            out[2]     = accB;
            out[3]     = accW;
        }
    }
    // Vertical pass (clamped edges) + renormalization, tmp -> grid.  Only
    // land texels are written back; water texels keep the Marine colour.
    for (u32 y = 0u; y < h; ++y) {
        for (u32 x = 0u; x < w; ++x) {
            u32 gi = y * w + x;
            if (world->heightGrid[gi] < AZGAAR_SEA_LEVEL_HEIGHT) continue;

            float accR = 0.0f, accG = 0.0f, accB = 0.0f, accW = 0.0f;
            for (i32 k = -radius; k <= radius; ++k) {
                i32 sy = static_cast<i32>(y) + k;
                if (sy < 0) sy = 0;
                if (sy >= static_cast<i32>(h)) sy = static_cast<i32>(h) - 1;
                const float* in = tmp.data() + (static_cast<size_t>(sy) * w + x) * 4u;
                accR += in[0];
                accG += in[1];
                accB += in[2];
                accW += in[3];
            }
            // A land texel always accumulates its own weight, so accW > 0.
            u8* px = world->biomeColorGrid.data() + static_cast<size_t>(gi) * 3u;
            px[0]  = static_cast<u8>(accR / accW + 0.5f);
            px[1]  = static_cast<u8>(accG / accW + 0.5f);
            px[2]  = static_cast<u8>(accB / accW + 0.5f);
        }
    }

    utils::info(
        "Azgaar biome colour grid smoothed: %ux%u, spacing=%.1f texels, sigma=%.1f (radius=%d) in "
        "%.1f ms",
        w,
        h,
        spacingTex,
        sigma,
        radius,
        (utils::nanos() - t0) / 1e6);
}

// Wind directions (degrees) live in the settings section's JSON blob
// (section 1, field 19, key "winds" — FMG emits one per season/quadrant).
// Missing/malformed leaves the array untouched (0 = not authored).
static void azgaarParseWinds(const char* settings, u32 settingsLen, float winds[6]) {
    u32 jsonLen   = 0u;
    const char* json = azgaarMapField(settings, settingsLen, 19u, &jsonLen);
    if (!json || jsonLen == 0u) return;
    json_t* root = json_loadb(json, jsonLen, 0, nullptr);
    if (!root) return;
    json_t* arr  = json_object_get(root, "winds");
    if (json_is_array(arr)) {
        for (u32 i = 0u; i < 6u && i < json_array_size(arr); i++) {
            json_t* v = json_array_get(arr, i);
            if (json_is_number(v)) winds[i] = static_cast<float>(json_number_value(v));
        }
    }
    json_decref(root);
}

static bool azgaarWorldLoadMap(AzgaarWorld* world, const char* path) {
    if (!utils::dataManagerFileExists(path)) {
        utils::warn("Azgaar .map not found: %s", path);
        return false;
    }

    world->jsonData = utils::dataManagerRead(path);
    if (!world->jsonData.data || world->jsonData.size == 0u) {
        utils::warn("Azgaar .map is empty: %s", path);
        azgaarWorldDestroy(world);
        return false;
    }
    const char* data = world->jsonData.data;
    u32 size         = world->jsonData.size;

    // --- header / settings ---
    u32 paramsLen = 0u;
    const char* params =
        azgaarMapSection(data, size, 0u, &paramsLen);  // version|...|seed|width|height|mapId
    u32 settingsLen      = 0u;
    const char* settings = azgaarMapSection(data, size, 1u, &settingsLen);
    if (!params || !settings) {
        utils::warn("Azgaar .map is missing header/settings: %s", path);
        azgaarWorldDestroy(world);
        return false;
    }

    world->widthPx       = azgaarMapFieldDouble(params, paramsLen, 4u, 0.0);
    world->heightPx      = azgaarMapFieldDouble(params, paramsLen, 5u, 0.0);
    world->distanceScale = azgaarMapFieldDouble(settings, settingsLen, 1u, 1.0);
    azgaarMapFieldStr(settings,
                      settingsLen,
                      0u,
                      world->distanceUnit,
                      sizeof(world->distanceUnit),
                      "px");
    azgaarMapFieldStr(settings, settingsLen, 3u, world->heightUnit, sizeof(world->heightUnit), "m");
    world->heightExponent = azgaarMapFieldDouble(settings, settingsLen, 4u, 1.0);
    if (world->heightExponent <= 0.0) world->heightExponent = 1.0;
    azgaarMapFieldStr(settings,
                      settingsLen,
                      20u,
                      world->mapName,
                      sizeof(world->mapName),
                      "Unnamed");
    azgaarParseWinds(settings, settingsLen, world->winds);

    // FMG .map distance: settings[0] = unit ("km"/"mi"/"m"/"ft"),
    // settings[1] = scale, so 1 pixel = scale * unitToMeters(unit) metres.
    // e.g. "km|0.1" -> 0.1 km/px = 100 m/px; an 800px map is then 80 km wide
    // (exactly the size set on the FMG web UI).  Do NOT renormalise by the
    // pixel extent — that corrupts the authored scale and changes the world
    // size with the chosen resolution.
    world->metersPerPixel = world->distanceScale * unitToMeters(world->distanceUnit);

    // --- grid points + boundary + heights ---
    u32 gridLen          = 0u;
    const char* gridJson = azgaarMapSection(data, size, 6u, &gridLen);
    if (!gridJson) {
        utils::warn("Azgaar .map has no grid section: %s", path);
        azgaarWorldDestroy(world);
        return false;
    }
    json_error_t jerr = {};
    json_t* grid      = json_loadb(gridJson, gridLen, 0, &jerr);
    if (!grid) {
        utils::warn("Azgaar .map grid parse failed: %s", jerr.text);
        azgaarWorldDestroy(world);
        return false;
    }
    json_t* pointsArr = json_object_get(grid, "points");
    if (!json_is_array(pointsArr)) {
        utils::warn("Azgaar .map grid has no points");
        json_decref(grid);
        azgaarWorldDestroy(world);
        return false;
    }
    u32 pointCount = static_cast<u32>(json_array_size(pointsArr));
    // cellsX/cellsY no longer used — pixel-resolution heightmap replaces the coarse grid
    if (pointCount == 0u) {
        utils::warn("Azgaar .map grid has no points");
        json_decref(grid);
        azgaarWorldDestroy(world);
        return false;
    }

    // heights (section 7): comma separated Uint8 values, one per grid cell.
    u32 heightsLen         = 0u;
    const char* heightsCsv = azgaarMapSection(data, size, 7u, &heightsLen);
    std::vector<u8> heights(pointCount, 0u);
    if (heightsCsv) {
        u32 i            = 0u;
        const char* s    = heightsCsv;
        const char* endp = heightsCsv + heightsLen;
        while (i < pointCount && s < endp) {
            while (s < endp && (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t')) s++;
            if (s >= endp) break;
            char* term      = nullptr;
            unsigned long v = strtoul(s, &term, 10);
            if (term == s) break;
            heights[i++] = static_cast<u8>(v > 255u ? 255u : v);
            s            = term;
            while (s < endp && *s == ',') s++;  // tolerate stray commas
        }
    }

    // precipitation (section 8) + temperature (section 11): per-grid-cell
    // scalars needed to classify biomes (FMG stores biome only per *pack* cell,
    // which we don't reconstruct, so we recompute it from these grid inputs
    // using FMG's own biome matrix).  They are now ALSO kept per cell (and
    // rasterized into the climate grids below) for the world-population
    // workstream A — snow line, weather, scatter gates.
    // Section 9 (`grid.cells.f`, waterbody/landmass id) and section 10
    // (`grid.cells.t`, signed coast distance) complete the climate fields.
    std::vector<i32> prec(pointCount, 0);
    std::vector<i32> temp(pointCount, 0);
    std::vector<i32> feature(pointCount, 0);
    std::vector<i32> coast(pointCount, 0);
    bool hasClimate = false;
    {
        u32 precLen = 0u, tempLen = 0u, featLen = 0u, coastLen = 0u;
        const char* precCsv  = azgaarMapSection(data, size, 8u, &precLen);
        const char* tempCsv  = azgaarMapSection(data, size, 11u, &tempLen);
        const char* featCsv  = azgaarMapSection(data, size, 9u, &featLen);
        const char* coastCsv = azgaarMapSection(data, size, 10u, &coastLen);
        azgaarParseCsvInts(precCsv, precLen, prec.data(), pointCount);
        azgaarParseCsvInts(tempCsv, tempLen, temp.data(), pointCount);
        azgaarParseCsvInts(featCsv, featLen, feature.data(), pointCount);
        azgaarParseCsvInts(coastCsv, coastLen, coast.data(), pointCount);
        // Temperature drives the snow line; without it the climate textures
        // stay unset (failure-tolerant parse, plan decision D9).
        hasClimate = tempCsv != nullptr && tempLen > 0u;
    }

    // Allocate cells and fill their positions + heights (biome is not stored
    // per grid cell in .map).  The Voronoi mesh (cell polygons + shared
    // vertices) is derived from the positions below.
    world->cellCount = pointCount;
    world->cells.resize(pointCount);

    // Fill cell positions + heights (biome is not stored per grid cell in .map,
    // so it is recomputed below from height/precipitation/temperature).
    float maxLandH = 0.0f;
    for (u32 i = 0u; i < pointCount; i++) {
        json_t* pt = json_array_get(pointsArr, i);
        if (json_is_array(pt) && json_array_size(pt) >= 2) {
            world->cells[i].x = static_cast<float>(json_number_value(json_array_get(pt, 0)));
            world->cells[i].y = static_cast<float>(json_number_value(json_array_get(pt, 1)));
        }
        world->cells[i].height = static_cast<float>(heights[i]);
        // Moisture approximates FMG's `4 + mean(land-neighbour prec + self)`;
        // without river flux / neighbour averaging this is `4 + prec`, which
        // is close enough for the coarse 5-band moisture index FMG uses.
        float moisture        = 4.0f + static_cast<float>(prec[i]);
        world->cells[i].biome = azgaarBiomeId(moisture, static_cast<float>(temp[i]), static_cast<float>(heights[i]));
        // Climate / coast scalars (kept per cell for the climate grids and
        // the flat fallback in azgaarWorldSampleClimate).
        world->cells[i].temp    = static_cast<float>(temp[i]);
        world->cells[i].prec    = static_cast<float>(prec[i]);
        world->cells[i].coast   = coast[i] != 0
                                     ? static_cast<float>(coast[i])
                                     : (heights[i] >= AZGAAR_SEA_LEVEL_HEIGHT
                                            ? AZGAAR_COAST_INTERIOR_LAND
                                            : AZGAAR_COAST_DEEP_WATER);
        world->cells[i].feature = feature[i] > 0 ? static_cast<u32>(feature[i]) : 0u;
        if (heights[i] >= AZGAAR_SEA_LEVEL_HEIGHT && static_cast<float>(heights[i]) > maxLandH) {
            maxLandH = static_cast<float>(heights[i]);
        }
    }
    world->maxLandHeightM = azgaarHeightToMeters(world, maxLandH);

    // Biome table (section 3): authored per-biome name + colour.  FMG emits a
    // JSON array; entry 0 is Marine.  Colours drive the terrain tint.
    {
        u32 biomesLen          = 0u;
        const char* biomesJson = azgaarMapSection(data, size, 3u, &biomesLen);
        if (biomesJson && biomesLen > 0u) {
            json_t* biomesArr = json_loadb(biomesJson, biomesLen, 0, &jerr);
            if (json_is_array(biomesArr)) {
                world->biomeCount = static_cast<u32>(json_array_size(biomesArr));
                world->biomes.resize(world->biomeCount);
                for (u32 i = 0u; i < world->biomeCount; i++) {
                    AzgaarBiome* b = &world->biomes[i];
                    *b             = AzgaarBiome{};
                    json_t* obj    = json_array_get(biomesArr, i);
                    if (!json_is_object(obj)) continue;
                    b->id = static_cast<u32>(jsonNumberOr(obj, "i", static_cast<double>(i)));
                    snprintf(b->name, sizeof(b->name), "%s", jsonStringOr(obj, "name", ""));
                    azgaarHexToRgb(jsonStringOr(obj, "color", "#cccccc"), b->color);
                    b->habitability = static_cast<u32>(jsonNumberOr(obj, "habitability", 0.0));
                    b->iconsDensity = static_cast<u32>(jsonNumberOr(obj, "iconsDensity", 0.0));
                    // Species icon names (FMG weighs each by repetition).  Only
                    // used by the props scatter (workstream B); harmless if
                    // absent (empty list → no vegetation for that biome).
                    json_t* icons = json_object_get(obj, "icons");
                    if (json_is_array(icons)) {
                        u32 n = json_array_size(icons);
                        if (n > AZGAAR_BIOME_MAX_ICONS) n = AZGAAR_BIOME_MAX_ICONS;
                        for (u32 k = 0u; k < n; k++) {
                            const char* s = json_string_value(json_array_get(icons, k));
                            if (!s) continue;
                            snprintf(b->icons[k], AZGAAR_ICON_NAME_LEN, "%s", s);
                        }
                        b->iconCount = n;
                    }
                }
            }
            json_decref(biomesArr);
        }
    }
    json_decref(grid);

    // Build the spatial hash of cell sites first — the Voronoi builder uses
    // it to find each cell's neighbours for half-plane intersection.
    buildCellGrid(world);

    // Derive each cell's Voronoi polygon by half-plane intersection against
    // its nearby sites (exact, non-overlapping by construction).  The polygon
    // vertices are stored in world->vertices and referenced by cell->vertices.
    if (!azgaarWorldBuildVoronoi(world)) {
        utils::warn("Azgaar .map Voronoi build failed; terrain will be empty");
    }

    // Height grid — rasterize each cell into a full-resolution grid at map pixel
    // dimensions (e.g. 1920x993).  For each pixel, find the nearest cell using
    // the spatial hash and assign its height.  Bicubic interpolation on this
    // dense grid produces smooth, natural terrain instead of faceted Voronoi cells.
    // Cap at 2048x1024 to bound memory for very large maps.
    const u32 gridW = static_cast<u32>(world->widthPx);
    const u32 gridH = static_cast<u32>(world->heightPx);
    const u32 capW  = 2048u;
    const u32 capH  = 1024u;
    if (gridW > capW || gridH > capH) {
        float scale             = fminf(static_cast<float>(capW) / gridW, static_cast<float>(capH) / gridH);
        world->heightGridWidth  = static_cast<u32>(gridW * scale);
        world->heightGridHeight = static_cast<u32>(gridH * scale);
    } else {
        world->heightGridWidth  = gridW;
        world->heightGridHeight = gridH;
    }
    world->heightGrid.resize(static_cast<size_t>(world->heightGridWidth) * world->heightGridHeight, 0.0f);

    // Biome colour grid (RGB u8) shares the height grid's dimensions and is
    // filled in the same nearest-cell rasterization pass; it is blurred
    // separately below (see azgaarSmoothBiomeColorGrid).  Default fill is the
    // neutral grey used by azgaarWorldBiomeColor for unknown biomes.
    world->biomeColorGridWidth  = world->heightGridWidth;
    world->biomeColorGridHeight = world->heightGridHeight;
    world->biomeColorGrid.assign(3u * static_cast<size_t>(world->biomeColorGridWidth) * world->biomeColorGridHeight, 204u);

    // Climate grids (temperature / precipitation / coast distance / biome id)
    // share the height grid's dimensions and are filled in the same
    // nearest-cell rasterization pass (workstream A).
    if (hasClimate) {
        world->climateGridWidth  = world->heightGridWidth;
        world->climateGridHeight = world->heightGridHeight;
        const size_t texels = static_cast<size_t>(world->climateGridWidth) * world->climateGridHeight;
        world->tempGrid.resize(texels, 0.0f);
        world->precGrid.resize(texels, 0.0f);
        world->coastGrid.resize(texels, 0.0f);
        world->biomeGrid.resize(texels, 0u);
    }

    // Rasterize: for each grid pixel, find the nearest cell and assign its height
    const float xScale = static_cast<float>(world->heightGridWidth) / world->widthPx;
    const float yScale = static_cast<float>(world->heightGridHeight) / world->heightPx;
    for (u32 gy = 0u; gy < world->heightGridHeight; gy++) {
        for (u32 gx = 0u; gx < world->heightGridWidth; gx++) {
            float xPx     = (gx + 0.5f) / xScale;
            float yPx     = (gy + 0.5f) / yScale;
            u32 cellIndex = nearestCellIndex(world, xPx, yPx);
            if (cellIndex != (u32)-1) {
                u32 gi            = gy * world->heightGridWidth + gx;
                world->heightGrid[gi] = world->cells[cellIndex].height;
                u32 biome         = world->cells[cellIndex].biome;
                if (biome < world->biomeCount) {
                    const float* c = world->biomes[biome].color;
                    u8* px         = world->biomeColorGrid.data() + static_cast<size_t>(gi) * 3u;
                    px[0]          = static_cast<u8>(c[0] * 255.0f + 0.5f);
                    px[1]          = static_cast<u8>(c[1] * 255.0f + 0.5f);
                    px[2]          = static_cast<u8>(c[2] * 255.0f + 0.5f);
                }
                if (!world->tempGrid.empty()) {
                    const AzgaarCell* cell = &world->cells[cellIndex];
                    world->tempGrid[gi]    = cell->temp;
                    world->precGrid[gi]    = cell->prec;
                    world->coastGrid[gi]   = cell->coast;
                    world->biomeGrid[gi]   = static_cast<u8>(biome <= 255u ? biome : 255u);
                }
            }
        }
    }

    // Low-pass the plateau steps (see azgaarGaussianBlurGrid) so the terrain
    // doesn't read as an ~800 m period staircase on distant hills.  The four
    // independent float grids (height + temp/prec/coast) run concurrently on
    // the default thread pool; the biome-colour blur stays inline because it
    // must see the FINAL blurred heights as its land/water mask.
    AzgaarBlurJob heightJob = {.world   = world,
                               .grid    = &world->heightGrid,
                               .w       = world->heightGridWidth,
                               .h       = world->heightGridHeight,
                               .envName = "ENGINE_AZGAAR_HM_SIGMA",
                               .label   = "height"};
    utils::threadPoolAddWork(nullptr, azgaarBlurJobFn, &heightJob);

    // Smooth the climate scalar fields with the same Gaussian so the snow
    // line / weather fields don't follow pixel-hard Voronoi borders.  The
    // biome id field stays nearest (ids are not interpolable).
    if (hasClimate) {
        const char* climateEnv         = "ENGINE_AZGAAR_CLIMATE_SIGMA";
        AzgaarBlurJob climateJobs[3]   = {};
        climateJobs[0] = {.world = world, .grid = &world->tempGrid,  .w = world->climateGridWidth, .h = world->climateGridHeight, .envName = climateEnv, .label = "temperature"};
        climateJobs[1] = {.world = world, .grid = &world->precGrid,  .w = world->climateGridWidth, .h = world->climateGridHeight, .envName = climateEnv, .label = "precipitation"};
        climateJobs[2] = {.world = world, .grid = &world->coastGrid, .w = world->climateGridWidth, .h = world->climateGridHeight, .envName = climateEnv, .label = "coast"};
        for (u32 i = 0u; i < 3u; ++i) utils::threadPoolAddWork(nullptr, azgaarBlurJobFn, &climateJobs[i]);
    }
    utils::threadPoolWait(nullptr);

    // Blend biome colours across cell borders (runs after the height blur so
    // the land/water mask matches the final heights the water pass uses).
    azgaarSmoothBiomeColorGrid(world);

    if (hasClimate) {
        utils::info("Azgaar climate grids built: %ux%u (temp/prec/coast blurred, biome nearest), "
             "winds=[%.0f %.0f %.0f %.0f %.0f %.0f], maxLand=%.0f m",
             world->climateGridWidth,
             world->climateGridHeight,
             world->winds[0],
             world->winds[1],
             world->winds[2],
             world->winds[3],
             world->winds[4],
             world->winds[5],
             world->maxLandHeightM);
    }

    // Political regions (states / provinces) for zone name lookups.
    u32 statesLen          = 0u;
    const char* statesJson = azgaarMapSection(data, size, 14u, &statesLen);
    if (statesJson && statesLen > 0u) {
        json_t* states = json_loadb(statesJson, statesLen, 0, &jerr);
        if (json_is_array(states)) {
            world->stateCount = static_cast<u32>(json_array_size(states));
            world->states.resize(world->stateCount);
            parseNamedRegions(world->states.data(), world->stateCount, states);
        }
        json_decref(states);
    }
    u32 provLen          = 0u;
    const char* provJson = azgaarMapSection(data, size, 30u, &provLen);
    if (provJson && provLen > 0u) {
        json_t* provinces = json_loadb(provJson, provLen, 0, &jerr);
        if (json_is_array(provinces)) {
            world->provinceCount = static_cast<u32>(json_array_size(provinces));
            world->provinces.resize(world->provinceCount);
            parseNamedRegions(world->provinces.data(), world->provinceCount, provinces);
        }
        json_decref(provinces);
    }
    // Note: pack cell positions (needed for azgaarWorldSampleZone) are not
    // stored in .map (FMG derives them via reGraph), so zone lookups from a
    // .map resolve to neutral until pack cells are reconstructed.

    // Routes (section 37).  Their point cells are pack-cell indices, which do
    // not map onto our grid cells, so blank them to force position-based
    // height sampling in the road corridor builder.
    u32 routesLen          = 0u;
    const char* routesJson = azgaarMapSection(data, size, 37u, &routesLen);
    if (routesJson && routesLen > 0u) {
        json_t* routes = json_loadb(routesJson, routesLen, 0, &jerr);
        if (json_is_array(routes)) {
            parseRoutesArray(world, routes);
            for (u32 r = 0u; r < world->routeCount; r++) {
                for (u32 p = 0u; p < world->routes[r].pointCount; p++) {
                    world->routes[r].points[p].cell = (u32)-1;
                }
            }
        }
        json_decref(routes);
    }

    // Workstream C (rivers): parse section 32 (metadata) + section 5 (SVG
    // centerlines) and join them into world->rivers.  Failure-tolerant: a
    // missing section simply yields zero rivers (warn + empty).
    parseRivers(world, data, size);

    // Workstream D (settlements): parse section 15 burgs into world->settlements
    // (positions -> world space, footprint from population, state colours).
    // Failure-tolerant: a missing section leaves zero settlements (D9).
    parseSettlements(world, data, size);

    // Workstream E (landmarks): parse section 35 markers into world->markers
    // (positions -> world space, kind from the `type` string).  Failure-
    // tolerant: a missing section leaves zero markers.
    parseMarkers(world, data, size);

    utils::info(
        "Azgaar .map loaded: %s %.0fx%.0f px, %.3f %s/px (%.1f m/px), height=%s exponent=%.2f, "
        "cells=%u vertices=%u routes=%u rivers=%u settlements=%u",
        world->mapName,
        world->widthPx,
        world->heightPx,
        world->distanceScale,
        world->distanceUnit,
        world->metersPerPixel,
        world->heightUnit,
        world->heightExponent,
        world->cellCount,
        world->gridVertexCount,
        world->routeCount,
        world->riverCount,
        world->settlementCount);
    return true;
}

bool azgaarWorldLoad(AzgaarWorld* world, const char* path) {
    *world = AzgaarWorld{};
    return azgaarWorldLoadMap(world, path);
}

void azgaarWorldDestroy(AzgaarWorld* world) {
    if (world->jsonData.data) {
        utils::stringDestroy(&world->jsonData);
    }
    world->cells.clear();
    world->vertices.clear();
    world->cellGrid = AzgaarCellGrid{};
    world->heightGrid.clear();
    world->biomeColorGrid.clear();
    world->tempGrid.clear();
    world->precGrid.clear();
    world->coastGrid.clear();
    world->biomeGrid.clear();
    world->routes.clear();
    world->rivers.clear();
    world->settlements.clear();
    world->markers.clear();
    world->packCells.clear();
    world->states.clear();
    world->provinces.clear();
    world->biomes.clear();
    *world = AzgaarWorld{};
}}  // namespace game
