#include "eve_dat.h"

// --- Forward Declarations ---
static void cleanup_members_evedat(EveDat* data);
static void cleanup_members_scene(Scene* data);
static void parse_json_to_scene(json_t* json_obj, Scene* out_struct);
static void cleanup_members_node(Node* data);
static void parse_json_to_node(json_t* json_obj, Node* out_struct);
static void cleanup_members_animation(Animation* data);
static void parse_json_to_animation(json_t* json_obj, Animation* out_struct);
static void cleanup_members_channel(Channel* data);
static void parse_json_to_channel(json_t* json_obj, Channel* out_struct);
static void cleanup_members_target(Target* data);
static void parse_json_to_target(json_t* json_obj, Target* out_struct);
static void cleanup_members_skin(Skin* data);
static void parse_json_to_skin(json_t* json_obj, Skin* out_struct);
static void cleanup_members_meshe(Meshe* data);
static void parse_json_to_meshe(json_t* json_obj, Meshe* out_struct);
static void cleanup_members_primitive(Primitive* data);
static void parse_json_to_primitive(json_t* json_obj, Primitive* out_struct);
static void cleanup_members_attributes(Attributes* data);
static void parse_json_to_attributes(json_t* json_obj, Attributes* out_struct);
static void cleanup_members_material(Material* data);
static void parse_json_to_material(json_t* json_obj, Material* out_struct);
static void cleanup_members_extensions(Extensions* data);
static void parse_json_to_extensions(json_t* json_obj, Extensions* out_struct);
static void cleanup_members_khrmaterialsspecular(KhrMaterialsSpecular* data);
static void parse_json_to_khrmaterialsspecular(json_t* json_obj, KhrMaterialsSpecular* out_struct);
static void cleanup_members_speculartexture(Speculartexture* data);
static void parse_json_to_speculartexture(json_t* json_obj, Speculartexture* out_struct);
static void cleanup_members_normaltexture(Normaltexture* data);
static void parse_json_to_normaltexture(json_t* json_obj, Normaltexture* out_struct);
static void cleanup_members_pbrmetallicroughness(Pbrmetallicroughness* data);
static void parse_json_to_pbrmetallicroughness(json_t* json_obj, Pbrmetallicroughness* out_struct);
static void cleanup_members_basecolortexture(Basecolortexture* data);
static void parse_json_to_basecolortexture(json_t* json_obj, Basecolortexture* out_struct);
static void cleanup_members_texture(Texture* data);
static void parse_json_to_texture(json_t* json_obj, Texture* out_struct);
static void cleanup_members_image(Image* data);
static void parse_json_to_image(json_t* json_obj, Image* out_struct);
static void cleanup_members_sampler(Sampler* data);
static void parse_json_to_sampler(json_t* json_obj, Sampler* out_struct);
static void cleanup_members_accessor(Accessor* data);
static void parse_json_to_accessor(json_t* json_obj, Accessor* out_struct);
static void cleanup_members_bufferview(Bufferview* data);
static void parse_json_to_bufferview(json_t* json_obj, Bufferview* out_struct);
static void cleanup_members_buffer(Buffer* data);
static void parse_json_to_buffer(json_t* json_obj, Buffer* out_struct);
static void cleanup_members_asset(Asset* data);
static void parse_json_to_asset(json_t* json_obj, Asset* out_struct);


// --- Cleanup and Freeing Functions ---
static void cleanup_members_asset(Asset* data) {
    if (!data) return;
    if (data->version) { free(data->version); data->version = NULL; }
    if (data->generator) { free(data->generator); data->generator = NULL; }
}

void free_asset(Asset* data) {
    if (!data) return;
    cleanup_members_asset(data);
    free(data);
}

static void cleanup_members_buffer(Buffer* data) {
    if (!data) return;
}

void free_buffer(Buffer* data) {
    if (!data) return;
    cleanup_members_buffer(data);
    free(data);
}

static void cleanup_members_bufferview(Bufferview* data) {
    if (!data) return;
}

void free_bufferview(Bufferview* data) {
    if (!data) return;
    cleanup_members_bufferview(data);
    free(data);
}

static void cleanup_members_accessor(Accessor* data) {
    if (!data) return;
    if (data->type) { free(data->type); data->type = NULL; }
    // Cleanup array: min (using arrlen)
    if (data->min) {
        size_t count = arrlen(data->min);
        arrfree(data->min);
        data->min = NULL;
    }
    // Cleanup array: max (using arrlen)
    if (data->max) {
        size_t count = arrlen(data->max);
        arrfree(data->max);
        data->max = NULL;
    }
}

void free_accessor(Accessor* data) {
    if (!data) return;
    cleanup_members_accessor(data);
    free(data);
}

static void cleanup_members_sampler(Sampler* data) {
    if (!data) return;
}

void free_sampler(Sampler* data) {
    if (!data) return;
    cleanup_members_sampler(data);
    free(data);
}

static void cleanup_members_image(Image* data) {
    if (!data) return;
    if (data->name) { free(data->name); data->name = NULL; }
    if (data->mimeType) { free(data->mimeType); data->mimeType = NULL; }
}

void free_image(Image* data) {
    if (!data) return;
    cleanup_members_image(data);
    free(data);
}

static void cleanup_members_texture(Texture* data) {
    if (!data) return;
    if (data->extensions) { free_extensions(data->extensions); data->extensions = NULL; }
}

void free_texture(Texture* data) {
    if (!data) return;
    cleanup_members_texture(data);
    free(data);
}

static void cleanup_members_basecolortexture(Basecolortexture* data) {
    if (!data) return;
    if (data->extensions) { free_extensions(data->extensions); data->extensions = NULL; }
}

void free_basecolortexture(Basecolortexture* data) {
    if (!data) return;
    cleanup_members_basecolortexture(data);
    free(data);
}

static void cleanup_members_pbrmetallicroughness(Pbrmetallicroughness* data) {
    if (!data) return;
    if (data->baseColorTexture) { free_basecolortexture(data->baseColorTexture); data->baseColorTexture = NULL; }
}

void free_pbrmetallicroughness(Pbrmetallicroughness* data) {
    if (!data) return;
    cleanup_members_pbrmetallicroughness(data);
    free(data);
}

static void cleanup_members_normaltexture(Normaltexture* data) {
    if (!data) return;
    if (data->extensions) { free_extensions(data->extensions); data->extensions = NULL; }
}

void free_normaltexture(Normaltexture* data) {
    if (!data) return;
    cleanup_members_normaltexture(data);
    free(data);
}

static void cleanup_members_speculartexture(Speculartexture* data) {
    if (!data) return;
    if (data->extensions) { free_extensions(data->extensions); data->extensions = NULL; }
}

void free_speculartexture(Speculartexture* data) {
    if (!data) return;
    cleanup_members_speculartexture(data);
    free(data);
}

static void cleanup_members_khrmaterialsspecular(KhrMaterialsSpecular* data) {
    if (!data) return;
    if (data->specularTexture) { free_speculartexture(data->specularTexture); data->specularTexture = NULL; }
}

void free_khrmaterialsspecular(KhrMaterialsSpecular* data) {
    if (!data) return;
    cleanup_members_khrmaterialsspecular(data);
    free(data);
}

static void cleanup_members_extensions(Extensions* data) {
    if (!data) return;
    if (data->KHR_materials_specular) { free_khrmaterialsspecular(data->KHR_materials_specular); data->KHR_materials_specular = NULL; }
}

void free_extensions(Extensions* data) {
    if (!data) return;
    cleanup_members_extensions(data);
    free(data);
}

static void cleanup_members_material(Material* data) {
    if (!data) return;
    if (data->name) { free(data->name); data->name = NULL; }
    if (data->pbrMetallicRoughness) { free_pbrmetallicroughness(data->pbrMetallicRoughness); data->pbrMetallicRoughness = NULL; }
    if (data->normalTexture) { free_normaltexture(data->normalTexture); data->normalTexture = NULL; }
    if (data->extensions) { free_extensions(data->extensions); data->extensions = NULL; }
}

void free_material(Material* data) {
    if (!data) return;
    cleanup_members_material(data);
    free(data);
}

static void cleanup_members_attributes(Attributes* data) {
    if (!data) return;
}

void free_attributes(Attributes* data) {
    if (!data) return;
    cleanup_members_attributes(data);
    free(data);
}

static void cleanup_members_primitive(Primitive* data) {
    if (!data) return;
    if (data->attributes) { free_attributes(data->attributes); data->attributes = NULL; }
}

void free_primitive(Primitive* data) {
    if (!data) return;
    cleanup_members_primitive(data);
    free(data);
}

static void cleanup_members_meshe(Meshe* data) {
    if (!data) return;
    // Cleanup array: primitives (using arrlen)
    if (data->primitives) {
        size_t count = arrlen(data->primitives);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_primitive(&data->primitives[i]);
        }
        arrfree(data->primitives);
        data->primitives = NULL;
    }
}

void free_meshe(Meshe* data) {
    if (!data) return;
    cleanup_members_meshe(data);
    free(data);
}

static void cleanup_members_skin(Skin* data) {
    if (!data) return;
    if (data->name) { free(data->name); data->name = NULL; }
    // Cleanup array: joints (using arrlen)
    if (data->joints) {
        size_t count = arrlen(data->joints);
        arrfree(data->joints);
        data->joints = NULL;
    }
}

void free_skin(Skin* data) {
    if (!data) return;
    cleanup_members_skin(data);
    free(data);
}

static void cleanup_members_target(Target* data) {
    if (!data) return;
    if (data->path) { free(data->path); data->path = NULL; }
}

void free_target(Target* data) {
    if (!data) return;
    cleanup_members_target(data);
    free(data);
}

static void cleanup_members_channel(Channel* data) {
    if (!data) return;
    if (data->target) { free_target(data->target); data->target = NULL; }
}

void free_channel(Channel* data) {
    if (!data) return;
    cleanup_members_channel(data);
    free(data);
}

static void cleanup_members_animation(Animation* data) {
    if (!data) return;
    if (data->name) { free(data->name); data->name = NULL; }
    // Cleanup array: samplers (using arrlen)
    if (data->samplers) {
        size_t count = arrlen(data->samplers);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_sampler(&data->samplers[i]);
        }
        arrfree(data->samplers);
        data->samplers = NULL;
    }
    // Cleanup array: channels (using arrlen)
    if (data->channels) {
        size_t count = arrlen(data->channels);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_channel(&data->channels[i]);
        }
        arrfree(data->channels);
        data->channels = NULL;
    }
}

void free_animation(Animation* data) {
    if (!data) return;
    cleanup_members_animation(data);
    free(data);
}

static void cleanup_members_node(Node* data) {
    if (!data) return;
    // Cleanup array: translation (using arrlen)
    if (data->translation) {
        size_t count = arrlen(data->translation);
        arrfree(data->translation);
        data->translation = NULL;
    }
    // Cleanup array: scale (using arrlen)
    if (data->scale) {
        size_t count = arrlen(data->scale);
        arrfree(data->scale);
        data->scale = NULL;
    }
}

void free_node(Node* data) {
    if (!data) return;
    cleanup_members_node(data);
    free(data);
}

static void cleanup_members_scene(Scene* data) {
    if (!data) return;
    if (data->name) { free(data->name); data->name = NULL; }
    // Cleanup array: nodes (using arrlen)
    if (data->nodes) {
        size_t count = arrlen(data->nodes);
        arrfree(data->nodes);
        data->nodes = NULL;
    }
}

void free_scene(Scene* data) {
    if (!data) return;
    cleanup_members_scene(data);
    free(data);
}

static void cleanup_members_evedat(EveDat* data) {
    if (!data) return;
    if (data->asset) { free_asset(data->asset); data->asset = NULL; }
    // Cleanup array: extensionsUsed (using arrlen)
    if (data->extensionsUsed) {
        size_t count = arrlen(data->extensionsUsed);
        for (size_t i = 0; i < count; ++i) {
            if (data->extensionsUsed[i]) free(data->extensionsUsed[i]);
        }
        arrfree(data->extensionsUsed);
        data->extensionsUsed = NULL;
    }
    // Cleanup array: extensionsRequired (using arrlen)
    if (data->extensionsRequired) {
        size_t count = arrlen(data->extensionsRequired);
        for (size_t i = 0; i < count; ++i) {
            if (data->extensionsRequired[i]) free(data->extensionsRequired[i]);
        }
        arrfree(data->extensionsRequired);
        data->extensionsRequired = NULL;
    }
    // Cleanup array: buffers (using arrlen)
    if (data->buffers) {
        size_t count = arrlen(data->buffers);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_buffer(&data->buffers[i]);
        }
        arrfree(data->buffers);
        data->buffers = NULL;
    }
    // Cleanup array: bufferViews (using arrlen)
    if (data->bufferViews) {
        size_t count = arrlen(data->bufferViews);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_bufferview(&data->bufferViews[i]);
        }
        arrfree(data->bufferViews);
        data->bufferViews = NULL;
    }
    // Cleanup array: accessors (using arrlen)
    if (data->accessors) {
        size_t count = arrlen(data->accessors);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_accessor(&data->accessors[i]);
        }
        arrfree(data->accessors);
        data->accessors = NULL;
    }
    // Cleanup array: samplers (using arrlen)
    if (data->samplers) {
        size_t count = arrlen(data->samplers);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_sampler(&data->samplers[i]);
        }
        arrfree(data->samplers);
        data->samplers = NULL;
    }
    // Cleanup array: images (using arrlen)
    if (data->images) {
        size_t count = arrlen(data->images);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_image(&data->images[i]);
        }
        arrfree(data->images);
        data->images = NULL;
    }
    // Cleanup array: textures (using arrlen)
    if (data->textures) {
        size_t count = arrlen(data->textures);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_texture(&data->textures[i]);
        }
        arrfree(data->textures);
        data->textures = NULL;
    }
    // Cleanup array: materials (using arrlen)
    if (data->materials) {
        size_t count = arrlen(data->materials);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_material(&data->materials[i]);
        }
        arrfree(data->materials);
        data->materials = NULL;
    }
    // Cleanup array: meshes (using arrlen)
    if (data->meshes) {
        size_t count = arrlen(data->meshes);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_meshe(&data->meshes[i]);
        }
        arrfree(data->meshes);
        data->meshes = NULL;
    }
    // Cleanup array: skins (using arrlen)
    if (data->skins) {
        size_t count = arrlen(data->skins);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_skin(&data->skins[i]);
        }
        arrfree(data->skins);
        data->skins = NULL;
    }
    // Cleanup array: animations (using arrlen)
    if (data->animations) {
        size_t count = arrlen(data->animations);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_animation(&data->animations[i]);
        }
        arrfree(data->animations);
        data->animations = NULL;
    }
    // Cleanup array: nodes (using arrlen)
    if (data->nodes) {
        size_t count = arrlen(data->nodes);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_node(&data->nodes[i]);
        }
        arrfree(data->nodes);
        data->nodes = NULL;
    }
    // Cleanup array: scenes (using arrlen)
    if (data->scenes) {
        size_t count = arrlen(data->scenes);
        for (size_t i = 0; i < count; ++i) {
            cleanup_members_scene(&data->scenes[i]);
        }
        arrfree(data->scenes);
        data->scenes = NULL;
    }
}

void free_evedat(EveDat* data) {
    if (!data) return;
    cleanup_members_evedat(data);
    free(data);
}


// --- Parsing Helper Functions (exit on error) ---
static void parse_json_to_evedat(json_t* json_obj, EveDat* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into EveDat, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_evedat\n"); exit(EXIT_FAILURE); }
    // Parsing field: asset -> asset
    json_t* j_asset = json_object_get(json_obj, "asset");
    if (j_asset && !json_is_null(j_asset)) {
        if (json_is_object(j_asset)) {
            out_struct->asset = (struct Asset*)calloc(1, sizeof(Asset));
            if (!out_struct->asset) { fprintf(stderr, "Error: calloc failed for 'asset' in EveDat.\n"); exit(EXIT_FAILURE); }
            parse_json_to_asset(j_asset, out_struct->asset);
        } else { fprintf(stderr, "Error: Expected object for 'asset', got %s in EveDat.\n", json_typeof_str(json_typeof(j_asset))); exit(EXIT_FAILURE); }
    } else if (!j_asset) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: extensionsUsed -> extensionsUsed
    json_t* j_extensionsUsed = json_object_get(json_obj, "extensionsUsed");
    if (j_extensionsUsed && !json_is_null(j_extensionsUsed)) {
        if (json_is_array(j_extensionsUsed)) {
            size_t json_array_size_val = json_array_size(j_extensionsUsed);
            out_struct->extensionsUsed = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_extensionsUsed, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'extensionsUsed' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_string(item_json)) {
                    const char* str_val = json_string_value(item_json);
                    char* new_str = str_val ? strdup(str_val) : NULL;
                    if (str_val && !new_str) { fprintf(stderr, "Error: strdup failed for string in array 'extensionsUsed' in EveDat.\n"); exit(EXIT_FAILURE); }
                    arrput(out_struct->extensionsUsed, new_str);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected string in array 'extensionsUsed', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'extensionsUsed', got %s in EveDat.\n", json_typeof_str(json_typeof(j_extensionsUsed))); exit(EXIT_FAILURE); }
    } else if (!j_extensionsUsed) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: extensionsRequired -> extensionsRequired
    json_t* j_extensionsRequired = json_object_get(json_obj, "extensionsRequired");
    if (j_extensionsRequired && !json_is_null(j_extensionsRequired)) {
        if (json_is_array(j_extensionsRequired)) {
            size_t json_array_size_val = json_array_size(j_extensionsRequired);
            out_struct->extensionsRequired = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_extensionsRequired, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'extensionsRequired' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_string(item_json)) {
                    const char* str_val = json_string_value(item_json);
                    char* new_str = str_val ? strdup(str_val) : NULL;
                    if (str_val && !new_str) { fprintf(stderr, "Error: strdup failed for string in array 'extensionsRequired' in EveDat.\n"); exit(EXIT_FAILURE); }
                    arrput(out_struct->extensionsRequired, new_str);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected string in array 'extensionsRequired', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'extensionsRequired', got %s in EveDat.\n", json_typeof_str(json_typeof(j_extensionsRequired))); exit(EXIT_FAILURE); }
    } else if (!j_extensionsRequired) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: buffers -> buffers
    json_t* j_buffers = json_object_get(json_obj, "buffers");
    if (j_buffers && !json_is_null(j_buffers)) {
        if (json_is_array(j_buffers)) {
            size_t json_array_size_val = json_array_size(j_buffers);
            out_struct->buffers = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_buffers, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'buffers' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Buffer temp_item;
                    memset(&temp_item, 0, sizeof(struct Buffer));
                    parse_json_to_buffer(item_json, &temp_item);
                    arrput(out_struct->buffers, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'buffers', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'buffers', got %s in EveDat.\n", json_typeof_str(json_typeof(j_buffers))); exit(EXIT_FAILURE); }
    } else if (!j_buffers) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: bufferViews -> bufferViews
    json_t* j_bufferViews = json_object_get(json_obj, "bufferViews");
    if (j_bufferViews && !json_is_null(j_bufferViews)) {
        if (json_is_array(j_bufferViews)) {
            size_t json_array_size_val = json_array_size(j_bufferViews);
            out_struct->bufferViews = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_bufferViews, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'bufferViews' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Bufferview temp_item;
                    memset(&temp_item, 0, sizeof(struct Bufferview));
                    parse_json_to_bufferview(item_json, &temp_item);
                    arrput(out_struct->bufferViews, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'bufferViews', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'bufferViews', got %s in EveDat.\n", json_typeof_str(json_typeof(j_bufferViews))); exit(EXIT_FAILURE); }
    } else if (!j_bufferViews) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: accessors -> accessors
    json_t* j_accessors = json_object_get(json_obj, "accessors");
    if (j_accessors && !json_is_null(j_accessors)) {
        if (json_is_array(j_accessors)) {
            size_t json_array_size_val = json_array_size(j_accessors);
            out_struct->accessors = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_accessors, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'accessors' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Accessor temp_item;
                    memset(&temp_item, 0, sizeof(struct Accessor));
                    parse_json_to_accessor(item_json, &temp_item);
                    arrput(out_struct->accessors, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'accessors', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'accessors', got %s in EveDat.\n", json_typeof_str(json_typeof(j_accessors))); exit(EXIT_FAILURE); }
    } else if (!j_accessors) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: samplers -> samplers
    json_t* j_samplers = json_object_get(json_obj, "samplers");
    if (j_samplers && !json_is_null(j_samplers)) {
        if (json_is_array(j_samplers)) {
            size_t json_array_size_val = json_array_size(j_samplers);
            out_struct->samplers = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_samplers, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'samplers' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Sampler temp_item;
                    memset(&temp_item, 0, sizeof(struct Sampler));
                    parse_json_to_sampler(item_json, &temp_item);
                    arrput(out_struct->samplers, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'samplers', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'samplers', got %s in EveDat.\n", json_typeof_str(json_typeof(j_samplers))); exit(EXIT_FAILURE); }
    } else if (!j_samplers) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: images -> images
    json_t* j_images = json_object_get(json_obj, "images");
    if (j_images && !json_is_null(j_images)) {
        if (json_is_array(j_images)) {
            size_t json_array_size_val = json_array_size(j_images);
            out_struct->images = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_images, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'images' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Image temp_item;
                    memset(&temp_item, 0, sizeof(struct Image));
                    parse_json_to_image(item_json, &temp_item);
                    arrput(out_struct->images, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'images', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'images', got %s in EveDat.\n", json_typeof_str(json_typeof(j_images))); exit(EXIT_FAILURE); }
    } else if (!j_images) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: textures -> textures
    json_t* j_textures = json_object_get(json_obj, "textures");
    if (j_textures && !json_is_null(j_textures)) {
        if (json_is_array(j_textures)) {
            size_t json_array_size_val = json_array_size(j_textures);
            out_struct->textures = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_textures, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'textures' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Texture temp_item;
                    memset(&temp_item, 0, sizeof(struct Texture));
                    parse_json_to_texture(item_json, &temp_item);
                    arrput(out_struct->textures, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'textures', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'textures', got %s in EveDat.\n", json_typeof_str(json_typeof(j_textures))); exit(EXIT_FAILURE); }
    } else if (!j_textures) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: materials -> materials
    json_t* j_materials = json_object_get(json_obj, "materials");
    if (j_materials && !json_is_null(j_materials)) {
        if (json_is_array(j_materials)) {
            size_t json_array_size_val = json_array_size(j_materials);
            out_struct->materials = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_materials, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'materials' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Material temp_item;
                    memset(&temp_item, 0, sizeof(struct Material));
                    parse_json_to_material(item_json, &temp_item);
                    arrput(out_struct->materials, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'materials', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'materials', got %s in EveDat.\n", json_typeof_str(json_typeof(j_materials))); exit(EXIT_FAILURE); }
    } else if (!j_materials) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: meshes -> meshes
    json_t* j_meshes = json_object_get(json_obj, "meshes");
    if (j_meshes && !json_is_null(j_meshes)) {
        if (json_is_array(j_meshes)) {
            size_t json_array_size_val = json_array_size(j_meshes);
            out_struct->meshes = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_meshes, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'meshes' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Meshe temp_item;
                    memset(&temp_item, 0, sizeof(struct Meshe));
                    parse_json_to_meshe(item_json, &temp_item);
                    arrput(out_struct->meshes, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'meshes', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'meshes', got %s in EveDat.\n", json_typeof_str(json_typeof(j_meshes))); exit(EXIT_FAILURE); }
    } else if (!j_meshes) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: skins -> skins
    json_t* j_skins = json_object_get(json_obj, "skins");
    if (j_skins && !json_is_null(j_skins)) {
        if (json_is_array(j_skins)) {
            size_t json_array_size_val = json_array_size(j_skins);
            out_struct->skins = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_skins, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'skins' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Skin temp_item;
                    memset(&temp_item, 0, sizeof(struct Skin));
                    parse_json_to_skin(item_json, &temp_item);
                    arrput(out_struct->skins, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'skins', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'skins', got %s in EveDat.\n", json_typeof_str(json_typeof(j_skins))); exit(EXIT_FAILURE); }
    } else if (!j_skins) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: animations -> animations
    json_t* j_animations = json_object_get(json_obj, "animations");
    if (j_animations && !json_is_null(j_animations)) {
        if (json_is_array(j_animations)) {
            size_t json_array_size_val = json_array_size(j_animations);
            out_struct->animations = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_animations, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'animations' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Animation temp_item;
                    memset(&temp_item, 0, sizeof(struct Animation));
                    parse_json_to_animation(item_json, &temp_item);
                    arrput(out_struct->animations, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'animations', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'animations', got %s in EveDat.\n", json_typeof_str(json_typeof(j_animations))); exit(EXIT_FAILURE); }
    } else if (!j_animations) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: nodes -> nodes
    json_t* j_nodes = json_object_get(json_obj, "nodes");
    if (j_nodes && !json_is_null(j_nodes)) {
        if (json_is_array(j_nodes)) {
            size_t json_array_size_val = json_array_size(j_nodes);
            out_struct->nodes = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_nodes, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'nodes' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Node temp_item;
                    memset(&temp_item, 0, sizeof(struct Node));
                    parse_json_to_node(item_json, &temp_item);
                    arrput(out_struct->nodes, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'nodes', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'nodes', got %s in EveDat.\n", json_typeof_str(json_typeof(j_nodes))); exit(EXIT_FAILURE); }
    } else if (!j_nodes) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: scenes -> scenes
    json_t* j_scenes = json_object_get(json_obj, "scenes");
    if (j_scenes && !json_is_null(j_scenes)) {
        if (json_is_array(j_scenes)) {
            size_t json_array_size_val = json_array_size(j_scenes);
            out_struct->scenes = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_scenes, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'scenes' in EveDat.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Scene temp_item;
                    memset(&temp_item, 0, sizeof(struct Scene));
                    parse_json_to_scene(item_json, &temp_item);
                    arrput(out_struct->scenes, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'scenes', got %s in EveDat.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'scenes', got %s in EveDat.\n", json_typeof_str(json_typeof(j_scenes))); exit(EXIT_FAILURE); }
    } else if (!j_scenes) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: scene -> scene
    json_t* j_scene = json_object_get(json_obj, "scene");
    if (j_scene && !json_is_null(j_scene)) {
        if (json_is_integer(j_scene)) { out_struct->scene = (int)json_integer_value(j_scene); }
        else if (json_is_boolean(j_scene)) { out_struct->scene = json_is_true(j_scene) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'scene', got %s in EveDat.\n", json_typeof_str(json_typeof(j_scene))); exit(EXIT_FAILURE); }
    } else if (!j_scene) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_scene(json_t* json_obj, Scene* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Scene, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_scene\n"); exit(EXIT_FAILURE); }
    // Parsing field: name -> name
    json_t* j_name = json_object_get(json_obj, "name");
    if (j_name && !json_is_null(j_name)) {
        if (json_is_string(j_name)) {
            const char* str_val = json_string_value(j_name);
            if (str_val) {
                out_struct->name = strdup(str_val);
                if (!out_struct->name) { fprintf(stderr, "Error: strdup failed for 'name' in Scene.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'name', got %s in Scene.\n", json_typeof_str(json_typeof(j_name))); exit(EXIT_FAILURE); }
    } else if (!j_name) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: nodes -> nodes
    json_t* j_nodes = json_object_get(json_obj, "nodes");
    if (j_nodes && !json_is_null(j_nodes)) {
        if (json_is_array(j_nodes)) {
            size_t json_array_size_val = json_array_size(j_nodes);
            out_struct->nodes = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_nodes, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'nodes' in Scene.\n", i); exit(EXIT_FAILURE); }
                if (json_is_integer(item_json)) { arrput(out_struct->nodes, (int)json_integer_value(item_json)); }
                else if (json_is_boolean(item_json)) { arrput(out_struct->nodes, json_is_true(item_json) ? 1 : 0); }
                else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected int/bool in array 'nodes', got %s in Scene.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'nodes', got %s in Scene.\n", json_typeof_str(json_typeof(j_nodes))); exit(EXIT_FAILURE); }
    } else if (!j_nodes) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_node(json_t* json_obj, Node* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Node, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_node\n"); exit(EXIT_FAILURE); }
    // Parsing field: mesh -> mesh
    json_t* j_mesh = json_object_get(json_obj, "mesh");
    if (j_mesh && !json_is_null(j_mesh)) {
        if (json_is_integer(j_mesh)) { out_struct->mesh = (int)json_integer_value(j_mesh); }
        else if (json_is_boolean(j_mesh)) { out_struct->mesh = json_is_true(j_mesh) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'mesh', got %s in Node.\n", json_typeof_str(json_typeof(j_mesh))); exit(EXIT_FAILURE); }
    } else if (!j_mesh) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: skin -> skin
    json_t* j_skin = json_object_get(json_obj, "skin");
    if (j_skin && !json_is_null(j_skin)) {
        if (json_is_integer(j_skin)) { out_struct->skin = (int)json_integer_value(j_skin); }
        else if (json_is_boolean(j_skin)) { out_struct->skin = json_is_true(j_skin) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'skin', got %s in Node.\n", json_typeof_str(json_typeof(j_skin))); exit(EXIT_FAILURE); }
    } else if (!j_skin) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: translation -> translation
    json_t* j_translation = json_object_get(json_obj, "translation");
    if (j_translation && !json_is_null(j_translation)) {
        if (json_is_array(j_translation)) {
            size_t json_array_size_val = json_array_size(j_translation);
            out_struct->translation = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_translation, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'translation' in Node.\n", i); exit(EXIT_FAILURE); }
                if (json_is_number(item_json)) { arrput(out_struct->translation, json_number_value(item_json)); }
                else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected number in array 'translation', got %s in Node.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'translation', got %s in Node.\n", json_typeof_str(json_typeof(j_translation))); exit(EXIT_FAILURE); }
    } else if (!j_translation) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: scale -> scale
    json_t* j_scale = json_object_get(json_obj, "scale");
    if (j_scale && !json_is_null(j_scale)) {
        if (json_is_array(j_scale)) {
            size_t json_array_size_val = json_array_size(j_scale);
            out_struct->scale = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_scale, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'scale' in Node.\n", i); exit(EXIT_FAILURE); }
                if (json_is_number(item_json)) { arrput(out_struct->scale, json_number_value(item_json)); }
                else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected number in array 'scale', got %s in Node.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'scale', got %s in Node.\n", json_typeof_str(json_typeof(j_scale))); exit(EXIT_FAILURE); }
    } else if (!j_scale) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_animation(json_t* json_obj, Animation* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Animation, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_animation\n"); exit(EXIT_FAILURE); }
    // Parsing field: name -> name
    json_t* j_name = json_object_get(json_obj, "name");
    if (j_name && !json_is_null(j_name)) {
        if (json_is_string(j_name)) {
            const char* str_val = json_string_value(j_name);
            if (str_val) {
                out_struct->name = strdup(str_val);
                if (!out_struct->name) { fprintf(stderr, "Error: strdup failed for 'name' in Animation.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'name', got %s in Animation.\n", json_typeof_str(json_typeof(j_name))); exit(EXIT_FAILURE); }
    } else if (!j_name) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: samplers -> samplers
    json_t* j_samplers = json_object_get(json_obj, "samplers");
    if (j_samplers && !json_is_null(j_samplers)) {
        if (json_is_array(j_samplers)) {
            size_t json_array_size_val = json_array_size(j_samplers);
            out_struct->samplers = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_samplers, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'samplers' in Animation.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Sampler temp_item;
                    memset(&temp_item, 0, sizeof(struct Sampler));
                    parse_json_to_sampler(item_json, &temp_item);
                    arrput(out_struct->samplers, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'samplers', got %s in Animation.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'samplers', got %s in Animation.\n", json_typeof_str(json_typeof(j_samplers))); exit(EXIT_FAILURE); }
    } else if (!j_samplers) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: channels -> channels
    json_t* j_channels = json_object_get(json_obj, "channels");
    if (j_channels && !json_is_null(j_channels)) {
        if (json_is_array(j_channels)) {
            size_t json_array_size_val = json_array_size(j_channels);
            out_struct->channels = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_channels, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'channels' in Animation.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Channel temp_item;
                    memset(&temp_item, 0, sizeof(struct Channel));
                    parse_json_to_channel(item_json, &temp_item);
                    arrput(out_struct->channels, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'channels', got %s in Animation.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'channels', got %s in Animation.\n", json_typeof_str(json_typeof(j_channels))); exit(EXIT_FAILURE); }
    } else if (!j_channels) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_channel(json_t* json_obj, Channel* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Channel, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_channel\n"); exit(EXIT_FAILURE); }
    // Parsing field: sampler -> sampler
    json_t* j_sampler = json_object_get(json_obj, "sampler");
    if (j_sampler && !json_is_null(j_sampler)) {
        if (json_is_integer(j_sampler)) { out_struct->sampler = (int)json_integer_value(j_sampler); }
        else if (json_is_boolean(j_sampler)) { out_struct->sampler = json_is_true(j_sampler) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'sampler', got %s in Channel.\n", json_typeof_str(json_typeof(j_sampler))); exit(EXIT_FAILURE); }
    } else if (!j_sampler) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: target -> target
    json_t* j_target = json_object_get(json_obj, "target");
    if (j_target && !json_is_null(j_target)) {
        if (json_is_object(j_target)) {
            out_struct->target = (struct Target*)calloc(1, sizeof(Target));
            if (!out_struct->target) { fprintf(stderr, "Error: calloc failed for 'target' in Channel.\n"); exit(EXIT_FAILURE); }
            parse_json_to_target(j_target, out_struct->target);
        } else { fprintf(stderr, "Error: Expected object for 'target', got %s in Channel.\n", json_typeof_str(json_typeof(j_target))); exit(EXIT_FAILURE); }
    } else if (!j_target) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_target(json_t* json_obj, Target* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Target, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_target\n"); exit(EXIT_FAILURE); }
    // Parsing field: node -> node
    json_t* j_node = json_object_get(json_obj, "node");
    if (j_node && !json_is_null(j_node)) {
        if (json_is_integer(j_node)) { out_struct->node = (int)json_integer_value(j_node); }
        else if (json_is_boolean(j_node)) { out_struct->node = json_is_true(j_node) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'node', got %s in Target.\n", json_typeof_str(json_typeof(j_node))); exit(EXIT_FAILURE); }
    } else if (!j_node) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: path -> path
    json_t* j_path = json_object_get(json_obj, "path");
    if (j_path && !json_is_null(j_path)) {
        if (json_is_string(j_path)) {
            const char* str_val = json_string_value(j_path);
            if (str_val) {
                out_struct->path = strdup(str_val);
                if (!out_struct->path) { fprintf(stderr, "Error: strdup failed for 'path' in Target.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'path', got %s in Target.\n", json_typeof_str(json_typeof(j_path))); exit(EXIT_FAILURE); }
    } else if (!j_path) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_skin(json_t* json_obj, Skin* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Skin, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_skin\n"); exit(EXIT_FAILURE); }
    // Parsing field: name -> name
    json_t* j_name = json_object_get(json_obj, "name");
    if (j_name && !json_is_null(j_name)) {
        if (json_is_string(j_name)) {
            const char* str_val = json_string_value(j_name);
            if (str_val) {
                out_struct->name = strdup(str_val);
                if (!out_struct->name) { fprintf(stderr, "Error: strdup failed for 'name' in Skin.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'name', got %s in Skin.\n", json_typeof_str(json_typeof(j_name))); exit(EXIT_FAILURE); }
    } else if (!j_name) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: joints -> joints
    json_t* j_joints = json_object_get(json_obj, "joints");
    if (j_joints && !json_is_null(j_joints)) {
        if (json_is_array(j_joints)) {
            size_t json_array_size_val = json_array_size(j_joints);
            out_struct->joints = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_joints, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'joints' in Skin.\n", i); exit(EXIT_FAILURE); }
                if (json_is_integer(item_json)) { arrput(out_struct->joints, (int)json_integer_value(item_json)); }
                else if (json_is_boolean(item_json)) { arrput(out_struct->joints, json_is_true(item_json) ? 1 : 0); }
                else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected int/bool in array 'joints', got %s in Skin.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'joints', got %s in Skin.\n", json_typeof_str(json_typeof(j_joints))); exit(EXIT_FAILURE); }
    } else if (!j_joints) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: inverseBindMatrices -> inverseBindMatrices
    json_t* j_inverseBindMatrices = json_object_get(json_obj, "inverseBindMatrices");
    if (j_inverseBindMatrices && !json_is_null(j_inverseBindMatrices)) {
        if (json_is_integer(j_inverseBindMatrices)) { out_struct->inverseBindMatrices = (int)json_integer_value(j_inverseBindMatrices); }
        else if (json_is_boolean(j_inverseBindMatrices)) { out_struct->inverseBindMatrices = json_is_true(j_inverseBindMatrices) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'inverseBindMatrices', got %s in Skin.\n", json_typeof_str(json_typeof(j_inverseBindMatrices))); exit(EXIT_FAILURE); }
    } else if (!j_inverseBindMatrices) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_meshe(json_t* json_obj, Meshe* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Meshe, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_meshe\n"); exit(EXIT_FAILURE); }
    // Parsing field: primitives -> primitives
    json_t* j_primitives = json_object_get(json_obj, "primitives");
    if (j_primitives && !json_is_null(j_primitives)) {
        if (json_is_array(j_primitives)) {
            size_t json_array_size_val = json_array_size(j_primitives);
            out_struct->primitives = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_primitives, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'primitives' in Meshe.\n", i); exit(EXIT_FAILURE); }
                if (json_is_object(item_json)) {
                    struct Primitive temp_item;
                    memset(&temp_item, 0, sizeof(struct Primitive));
                    parse_json_to_primitive(item_json, &temp_item);
                    arrput(out_struct->primitives, temp_item);
                } else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected object in array 'primitives', got %s in Meshe.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'primitives', got %s in Meshe.\n", json_typeof_str(json_typeof(j_primitives))); exit(EXIT_FAILURE); }
    } else if (!j_primitives) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_primitive(json_t* json_obj, Primitive* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Primitive, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_primitive\n"); exit(EXIT_FAILURE); }
    // Parsing field: attributes -> attributes
    json_t* j_attributes = json_object_get(json_obj, "attributes");
    if (j_attributes && !json_is_null(j_attributes)) {
        if (json_is_object(j_attributes)) {
            out_struct->attributes = (struct Attributes*)calloc(1, sizeof(Attributes));
            if (!out_struct->attributes) { fprintf(stderr, "Error: calloc failed for 'attributes' in Primitive.\n"); exit(EXIT_FAILURE); }
            parse_json_to_attributes(j_attributes, out_struct->attributes);
        } else { fprintf(stderr, "Error: Expected object for 'attributes', got %s in Primitive.\n", json_typeof_str(json_typeof(j_attributes))); exit(EXIT_FAILURE); }
    } else if (!j_attributes) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: indices -> indices
    json_t* j_indices = json_object_get(json_obj, "indices");
    if (j_indices && !json_is_null(j_indices)) {
        if (json_is_integer(j_indices)) { out_struct->indices = (int)json_integer_value(j_indices); }
        else if (json_is_boolean(j_indices)) { out_struct->indices = json_is_true(j_indices) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'indices', got %s in Primitive.\n", json_typeof_str(json_typeof(j_indices))); exit(EXIT_FAILURE); }
    } else if (!j_indices) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: material -> material
    json_t* j_material = json_object_get(json_obj, "material");
    if (j_material && !json_is_null(j_material)) {
        if (json_is_integer(j_material)) { out_struct->material = (int)json_integer_value(j_material); }
        else if (json_is_boolean(j_material)) { out_struct->material = json_is_true(j_material) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'material', got %s in Primitive.\n", json_typeof_str(json_typeof(j_material))); exit(EXIT_FAILURE); }
    } else if (!j_material) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_attributes(json_t* json_obj, Attributes* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Attributes, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_attributes\n"); exit(EXIT_FAILURE); }
    // Parsing field: POSITION -> POSITION
    json_t* j_POSITION = json_object_get(json_obj, "POSITION");
    if (j_POSITION && !json_is_null(j_POSITION)) {
        if (json_is_integer(j_POSITION)) { out_struct->POSITION = (int)json_integer_value(j_POSITION); }
        else if (json_is_boolean(j_POSITION)) { out_struct->POSITION = json_is_true(j_POSITION) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'POSITION', got %s in Attributes.\n", json_typeof_str(json_typeof(j_POSITION))); exit(EXIT_FAILURE); }
    } else if (!j_POSITION) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: NORMAL -> NORMAL
    json_t* j_NORMAL = json_object_get(json_obj, "NORMAL");
    if (j_NORMAL && !json_is_null(j_NORMAL)) {
        if (json_is_integer(j_NORMAL)) { out_struct->NORMAL = (int)json_integer_value(j_NORMAL); }
        else if (json_is_boolean(j_NORMAL)) { out_struct->NORMAL = json_is_true(j_NORMAL) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'NORMAL', got %s in Attributes.\n", json_typeof_str(json_typeof(j_NORMAL))); exit(EXIT_FAILURE); }
    } else if (!j_NORMAL) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: TEXCOORD_0 -> TEXCOORD_0
    json_t* j_TEXCOORD_0 = json_object_get(json_obj, "TEXCOORD_0");
    if (j_TEXCOORD_0 && !json_is_null(j_TEXCOORD_0)) {
        if (json_is_integer(j_TEXCOORD_0)) { out_struct->TEXCOORD_0 = (int)json_integer_value(j_TEXCOORD_0); }
        else if (json_is_boolean(j_TEXCOORD_0)) { out_struct->TEXCOORD_0 = json_is_true(j_TEXCOORD_0) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'TEXCOORD_0', got %s in Attributes.\n", json_typeof_str(json_typeof(j_TEXCOORD_0))); exit(EXIT_FAILURE); }
    } else if (!j_TEXCOORD_0) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: JOINTS_0 -> JOINTS_0
    json_t* j_JOINTS_0 = json_object_get(json_obj, "JOINTS_0");
    if (j_JOINTS_0 && !json_is_null(j_JOINTS_0)) {
        if (json_is_integer(j_JOINTS_0)) { out_struct->JOINTS_0 = (int)json_integer_value(j_JOINTS_0); }
        else if (json_is_boolean(j_JOINTS_0)) { out_struct->JOINTS_0 = json_is_true(j_JOINTS_0) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'JOINTS_0', got %s in Attributes.\n", json_typeof_str(json_typeof(j_JOINTS_0))); exit(EXIT_FAILURE); }
    } else if (!j_JOINTS_0) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: WEIGHTS_0 -> WEIGHTS_0
    json_t* j_WEIGHTS_0 = json_object_get(json_obj, "WEIGHTS_0");
    if (j_WEIGHTS_0 && !json_is_null(j_WEIGHTS_0)) {
        if (json_is_integer(j_WEIGHTS_0)) { out_struct->WEIGHTS_0 = (int)json_integer_value(j_WEIGHTS_0); }
        else if (json_is_boolean(j_WEIGHTS_0)) { out_struct->WEIGHTS_0 = json_is_true(j_WEIGHTS_0) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'WEIGHTS_0', got %s in Attributes.\n", json_typeof_str(json_typeof(j_WEIGHTS_0))); exit(EXIT_FAILURE); }
    } else if (!j_WEIGHTS_0) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_material(json_t* json_obj, Material* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Material, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_material\n"); exit(EXIT_FAILURE); }
    // Parsing field: name -> name
    json_t* j_name = json_object_get(json_obj, "name");
    if (j_name && !json_is_null(j_name)) {
        if (json_is_string(j_name)) {
            const char* str_val = json_string_value(j_name);
            if (str_val) {
                out_struct->name = strdup(str_val);
                if (!out_struct->name) { fprintf(stderr, "Error: strdup failed for 'name' in Material.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'name', got %s in Material.\n", json_typeof_str(json_typeof(j_name))); exit(EXIT_FAILURE); }
    } else if (!j_name) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: pbrMetallicRoughness -> pbrMetallicRoughness
    json_t* j_pbrMetallicRoughness = json_object_get(json_obj, "pbrMetallicRoughness");
    if (j_pbrMetallicRoughness && !json_is_null(j_pbrMetallicRoughness)) {
        if (json_is_object(j_pbrMetallicRoughness)) {
            out_struct->pbrMetallicRoughness = (struct Pbrmetallicroughness*)calloc(1, sizeof(Pbrmetallicroughness));
            if (!out_struct->pbrMetallicRoughness) { fprintf(stderr, "Error: calloc failed for 'pbrMetallicRoughness' in Material.\n"); exit(EXIT_FAILURE); }
            parse_json_to_pbrmetallicroughness(j_pbrMetallicRoughness, out_struct->pbrMetallicRoughness);
        } else { fprintf(stderr, "Error: Expected object for 'pbrMetallicRoughness', got %s in Material.\n", json_typeof_str(json_typeof(j_pbrMetallicRoughness))); exit(EXIT_FAILURE); }
    } else if (!j_pbrMetallicRoughness) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: normalTexture -> normalTexture
    json_t* j_normalTexture = json_object_get(json_obj, "normalTexture");
    if (j_normalTexture && !json_is_null(j_normalTexture)) {
        if (json_is_object(j_normalTexture)) {
            out_struct->normalTexture = (struct Normaltexture*)calloc(1, sizeof(Normaltexture));
            if (!out_struct->normalTexture) { fprintf(stderr, "Error: calloc failed for 'normalTexture' in Material.\n"); exit(EXIT_FAILURE); }
            parse_json_to_normaltexture(j_normalTexture, out_struct->normalTexture);
        } else { fprintf(stderr, "Error: Expected object for 'normalTexture', got %s in Material.\n", json_typeof_str(json_typeof(j_normalTexture))); exit(EXIT_FAILURE); }
    } else if (!j_normalTexture) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: doubleSided -> doubleSided
    json_t* j_doubleSided = json_object_get(json_obj, "doubleSided");
    if (j_doubleSided && !json_is_null(j_doubleSided)) {
        if (json_is_integer(j_doubleSided)) { out_struct->doubleSided = (int)json_integer_value(j_doubleSided); }
        else if (json_is_boolean(j_doubleSided)) { out_struct->doubleSided = json_is_true(j_doubleSided) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'doubleSided', got %s in Material.\n", json_typeof_str(json_typeof(j_doubleSided))); exit(EXIT_FAILURE); }
    } else if (!j_doubleSided) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: extensions -> extensions
    json_t* j_extensions = json_object_get(json_obj, "extensions");
    if (j_extensions && !json_is_null(j_extensions)) {
        if (json_is_object(j_extensions)) {
            out_struct->extensions = (struct Extensions*)calloc(1, sizeof(Extensions));
            if (!out_struct->extensions) { fprintf(stderr, "Error: calloc failed for 'extensions' in Material.\n"); exit(EXIT_FAILURE); }
            parse_json_to_extensions(j_extensions, out_struct->extensions);
        } else { fprintf(stderr, "Error: Expected object for 'extensions', got %s in Material.\n", json_typeof_str(json_typeof(j_extensions))); exit(EXIT_FAILURE); }
    } else if (!j_extensions) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_extensions(json_t* json_obj, Extensions* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Extensions, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_extensions\n"); exit(EXIT_FAILURE); }
    // Parsing field: KHR_materials_specular -> KHR_materials_specular
    json_t* j_KHR_materials_specular = json_object_get(json_obj, "KHR_materials_specular");
    if (j_KHR_materials_specular && !json_is_null(j_KHR_materials_specular)) {
        if (json_is_object(j_KHR_materials_specular)) {
            out_struct->KHR_materials_specular = (struct KhrMaterialsSpecular*)calloc(1, sizeof(KhrMaterialsSpecular));
            if (!out_struct->KHR_materials_specular) { fprintf(stderr, "Error: calloc failed for 'KHR_materials_specular' in Extensions.\n"); exit(EXIT_FAILURE); }
            parse_json_to_khrmaterialsspecular(j_KHR_materials_specular, out_struct->KHR_materials_specular);
        } else { fprintf(stderr, "Error: Expected object for 'KHR_materials_specular', got %s in Extensions.\n", json_typeof_str(json_typeof(j_KHR_materials_specular))); exit(EXIT_FAILURE); }
    } else if (!j_KHR_materials_specular) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_khrmaterialsspecular(json_t* json_obj, KhrMaterialsSpecular* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into KhrMaterialsSpecular, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_khrmaterialsspecular\n"); exit(EXIT_FAILURE); }
    // Parsing field: specularTexture -> specularTexture
    json_t* j_specularTexture = json_object_get(json_obj, "specularTexture");
    if (j_specularTexture && !json_is_null(j_specularTexture)) {
        if (json_is_object(j_specularTexture)) {
            out_struct->specularTexture = (struct Speculartexture*)calloc(1, sizeof(Speculartexture));
            if (!out_struct->specularTexture) { fprintf(stderr, "Error: calloc failed for 'specularTexture' in KhrMaterialsSpecular.\n"); exit(EXIT_FAILURE); }
            parse_json_to_speculartexture(j_specularTexture, out_struct->specularTexture);
        } else { fprintf(stderr, "Error: Expected object for 'specularTexture', got %s in KhrMaterialsSpecular.\n", json_typeof_str(json_typeof(j_specularTexture))); exit(EXIT_FAILURE); }
    } else if (!j_specularTexture) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_speculartexture(json_t* json_obj, Speculartexture* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Speculartexture, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_speculartexture\n"); exit(EXIT_FAILURE); }
    // Parsing field: index -> index
    json_t* j_index = json_object_get(json_obj, "index");
    if (j_index && !json_is_null(j_index)) {
        if (json_is_integer(j_index)) { out_struct->index = (int)json_integer_value(j_index); }
        else if (json_is_boolean(j_index)) { out_struct->index = json_is_true(j_index) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'index', got %s in Speculartexture.\n", json_typeof_str(json_typeof(j_index))); exit(EXIT_FAILURE); }
    } else if (!j_index) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: extensions -> extensions
    json_t* j_extensions = json_object_get(json_obj, "extensions");
    if (j_extensions && !json_is_null(j_extensions)) {
        if (json_is_object(j_extensions)) {
            out_struct->extensions = (struct Extensions*)calloc(1, sizeof(Extensions));
            if (!out_struct->extensions) { fprintf(stderr, "Error: calloc failed for 'extensions' in Speculartexture.\n"); exit(EXIT_FAILURE); }
            parse_json_to_extensions(j_extensions, out_struct->extensions);
        } else { fprintf(stderr, "Error: Expected object for 'extensions', got %s in Speculartexture.\n", json_typeof_str(json_typeof(j_extensions))); exit(EXIT_FAILURE); }
    } else if (!j_extensions) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_normaltexture(json_t* json_obj, Normaltexture* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Normaltexture, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_normaltexture\n"); exit(EXIT_FAILURE); }
    // Parsing field: index -> index
    json_t* j_index = json_object_get(json_obj, "index");
    if (j_index && !json_is_null(j_index)) {
        if (json_is_integer(j_index)) { out_struct->index = (int)json_integer_value(j_index); }
        else if (json_is_boolean(j_index)) { out_struct->index = json_is_true(j_index) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'index', got %s in Normaltexture.\n", json_typeof_str(json_typeof(j_index))); exit(EXIT_FAILURE); }
    } else if (!j_index) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: scale -> scale
    json_t* j_scale = json_object_get(json_obj, "scale");
    if (j_scale && !json_is_null(j_scale)) {
        if (json_is_integer(j_scale)) { out_struct->scale = (int)json_integer_value(j_scale); }
        else if (json_is_boolean(j_scale)) { out_struct->scale = json_is_true(j_scale) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'scale', got %s in Normaltexture.\n", json_typeof_str(json_typeof(j_scale))); exit(EXIT_FAILURE); }
    } else if (!j_scale) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: extensions -> extensions
    json_t* j_extensions = json_object_get(json_obj, "extensions");
    if (j_extensions && !json_is_null(j_extensions)) {
        if (json_is_object(j_extensions)) {
            out_struct->extensions = (struct Extensions*)calloc(1, sizeof(Extensions));
            if (!out_struct->extensions) { fprintf(stderr, "Error: calloc failed for 'extensions' in Normaltexture.\n"); exit(EXIT_FAILURE); }
            parse_json_to_extensions(j_extensions, out_struct->extensions);
        } else { fprintf(stderr, "Error: Expected object for 'extensions', got %s in Normaltexture.\n", json_typeof_str(json_typeof(j_extensions))); exit(EXIT_FAILURE); }
    } else if (!j_extensions) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_pbrmetallicroughness(json_t* json_obj, Pbrmetallicroughness* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Pbrmetallicroughness, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_pbrmetallicroughness\n"); exit(EXIT_FAILURE); }
    // Parsing field: baseColorTexture -> baseColorTexture
    json_t* j_baseColorTexture = json_object_get(json_obj, "baseColorTexture");
    if (j_baseColorTexture && !json_is_null(j_baseColorTexture)) {
        if (json_is_object(j_baseColorTexture)) {
            out_struct->baseColorTexture = (struct Basecolortexture*)calloc(1, sizeof(Basecolortexture));
            if (!out_struct->baseColorTexture) { fprintf(stderr, "Error: calloc failed for 'baseColorTexture' in Pbrmetallicroughness.\n"); exit(EXIT_FAILURE); }
            parse_json_to_basecolortexture(j_baseColorTexture, out_struct->baseColorTexture);
        } else { fprintf(stderr, "Error: Expected object for 'baseColorTexture', got %s in Pbrmetallicroughness.\n", json_typeof_str(json_typeof(j_baseColorTexture))); exit(EXIT_FAILURE); }
    } else if (!j_baseColorTexture) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: metallicFactor -> metallicFactor
    json_t* j_metallicFactor = json_object_get(json_obj, "metallicFactor");
    if (j_metallicFactor && !json_is_null(j_metallicFactor)) {
        if (json_is_integer(j_metallicFactor)) { out_struct->metallicFactor = (int)json_integer_value(j_metallicFactor); }
        else if (json_is_boolean(j_metallicFactor)) { out_struct->metallicFactor = json_is_true(j_metallicFactor) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'metallicFactor', got %s in Pbrmetallicroughness.\n", json_typeof_str(json_typeof(j_metallicFactor))); exit(EXIT_FAILURE); }
    } else if (!j_metallicFactor) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: roughnessFactor -> roughnessFactor
    json_t* j_roughnessFactor = json_object_get(json_obj, "roughnessFactor");
    if (j_roughnessFactor && !json_is_null(j_roughnessFactor)) {
        if (json_is_number(j_roughnessFactor)) { out_struct->roughnessFactor = json_number_value(j_roughnessFactor); }
        else { fprintf(stderr, "Error: Expected number for 'roughnessFactor', got %s in Pbrmetallicroughness.\n", json_typeof_str(json_typeof(j_roughnessFactor))); exit(EXIT_FAILURE); }
    } else if (!j_roughnessFactor) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_basecolortexture(json_t* json_obj, Basecolortexture* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Basecolortexture, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_basecolortexture\n"); exit(EXIT_FAILURE); }
    // Parsing field: index -> index
    json_t* j_index = json_object_get(json_obj, "index");
    if (j_index && !json_is_null(j_index)) {
        if (json_is_integer(j_index)) { out_struct->index = (int)json_integer_value(j_index); }
        else if (json_is_boolean(j_index)) { out_struct->index = json_is_true(j_index) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'index', got %s in Basecolortexture.\n", json_typeof_str(json_typeof(j_index))); exit(EXIT_FAILURE); }
    } else if (!j_index) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: extensions -> extensions
    json_t* j_extensions = json_object_get(json_obj, "extensions");
    if (j_extensions && !json_is_null(j_extensions)) {
        if (json_is_object(j_extensions)) {
            out_struct->extensions = (struct Extensions*)calloc(1, sizeof(Extensions));
            if (!out_struct->extensions) { fprintf(stderr, "Error: calloc failed for 'extensions' in Basecolortexture.\n"); exit(EXIT_FAILURE); }
            parse_json_to_extensions(j_extensions, out_struct->extensions);
        } else { fprintf(stderr, "Error: Expected object for 'extensions', got %s in Basecolortexture.\n", json_typeof_str(json_typeof(j_extensions))); exit(EXIT_FAILURE); }
    } else if (!j_extensions) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_texture(json_t* json_obj, Texture* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Texture, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_texture\n"); exit(EXIT_FAILURE); }
    // Parsing field: sampler -> sampler
    json_t* j_sampler = json_object_get(json_obj, "sampler");
    if (j_sampler && !json_is_null(j_sampler)) {
        if (json_is_integer(j_sampler)) { out_struct->sampler = (int)json_integer_value(j_sampler); }
        else if (json_is_boolean(j_sampler)) { out_struct->sampler = json_is_true(j_sampler) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'sampler', got %s in Texture.\n", json_typeof_str(json_typeof(j_sampler))); exit(EXIT_FAILURE); }
    } else if (!j_sampler) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: extensions -> extensions
    json_t* j_extensions = json_object_get(json_obj, "extensions");
    if (j_extensions && !json_is_null(j_extensions)) {
        if (json_is_object(j_extensions)) {
            out_struct->extensions = (struct Extensions*)calloc(1, sizeof(Extensions));
            if (!out_struct->extensions) { fprintf(stderr, "Error: calloc failed for 'extensions' in Texture.\n"); exit(EXIT_FAILURE); }
            parse_json_to_extensions(j_extensions, out_struct->extensions);
        } else { fprintf(stderr, "Error: Expected object for 'extensions', got %s in Texture.\n", json_typeof_str(json_typeof(j_extensions))); exit(EXIT_FAILURE); }
    } else if (!j_extensions) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_image(json_t* json_obj, Image* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Image, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_image\n"); exit(EXIT_FAILURE); }
    // Parsing field: name -> name
    json_t* j_name = json_object_get(json_obj, "name");
    if (j_name && !json_is_null(j_name)) {
        if (json_is_string(j_name)) {
            const char* str_val = json_string_value(j_name);
            if (str_val) {
                out_struct->name = strdup(str_val);
                if (!out_struct->name) { fprintf(stderr, "Error: strdup failed for 'name' in Image.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'name', got %s in Image.\n", json_typeof_str(json_typeof(j_name))); exit(EXIT_FAILURE); }
    } else if (!j_name) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: bufferView -> bufferView
    json_t* j_bufferView = json_object_get(json_obj, "bufferView");
    if (j_bufferView && !json_is_null(j_bufferView)) {
        if (json_is_integer(j_bufferView)) { out_struct->bufferView = (int)json_integer_value(j_bufferView); }
        else if (json_is_boolean(j_bufferView)) { out_struct->bufferView = json_is_true(j_bufferView) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'bufferView', got %s in Image.\n", json_typeof_str(json_typeof(j_bufferView))); exit(EXIT_FAILURE); }
    } else if (!j_bufferView) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: mimeType -> mimeType
    json_t* j_mimeType = json_object_get(json_obj, "mimeType");
    if (j_mimeType && !json_is_null(j_mimeType)) {
        if (json_is_string(j_mimeType)) {
            const char* str_val = json_string_value(j_mimeType);
            if (str_val) {
                out_struct->mimeType = strdup(str_val);
                if (!out_struct->mimeType) { fprintf(stderr, "Error: strdup failed for 'mimeType' in Image.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'mimeType', got %s in Image.\n", json_typeof_str(json_typeof(j_mimeType))); exit(EXIT_FAILURE); }
    } else if (!j_mimeType) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_sampler(json_t* json_obj, Sampler* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Sampler, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_sampler\n"); exit(EXIT_FAILURE); }
    // Parsing field: magFilter -> magFilter
    json_t* j_magFilter = json_object_get(json_obj, "magFilter");
    if (j_magFilter && !json_is_null(j_magFilter)) {
        if (json_is_integer(j_magFilter)) { out_struct->magFilter = (int)json_integer_value(j_magFilter); }
        else if (json_is_boolean(j_magFilter)) { out_struct->magFilter = json_is_true(j_magFilter) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'magFilter', got %s in Sampler.\n", json_typeof_str(json_typeof(j_magFilter))); exit(EXIT_FAILURE); }
    } else if (!j_magFilter) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: minFilter -> minFilter
    json_t* j_minFilter = json_object_get(json_obj, "minFilter");
    if (j_minFilter && !json_is_null(j_minFilter)) {
        if (json_is_integer(j_minFilter)) { out_struct->minFilter = (int)json_integer_value(j_minFilter); }
        else if (json_is_boolean(j_minFilter)) { out_struct->minFilter = json_is_true(j_minFilter) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'minFilter', got %s in Sampler.\n", json_typeof_str(json_typeof(j_minFilter))); exit(EXIT_FAILURE); }
    } else if (!j_minFilter) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_accessor(json_t* json_obj, Accessor* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Accessor, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_accessor\n"); exit(EXIT_FAILURE); }
    // Parsing field: bufferView -> bufferView
    json_t* j_bufferView = json_object_get(json_obj, "bufferView");
    if (j_bufferView && !json_is_null(j_bufferView)) {
        if (json_is_integer(j_bufferView)) { out_struct->bufferView = (int)json_integer_value(j_bufferView); }
        else if (json_is_boolean(j_bufferView)) { out_struct->bufferView = json_is_true(j_bufferView) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'bufferView', got %s in Accessor.\n", json_typeof_str(json_typeof(j_bufferView))); exit(EXIT_FAILURE); }
    } else if (!j_bufferView) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: byteOffset -> byteOffset
    json_t* j_byteOffset = json_object_get(json_obj, "byteOffset");
    if (j_byteOffset && !json_is_null(j_byteOffset)) {
        if (json_is_integer(j_byteOffset)) { out_struct->byteOffset = (int)json_integer_value(j_byteOffset); }
        else if (json_is_boolean(j_byteOffset)) { out_struct->byteOffset = json_is_true(j_byteOffset) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'byteOffset', got %s in Accessor.\n", json_typeof_str(json_typeof(j_byteOffset))); exit(EXIT_FAILURE); }
    } else if (!j_byteOffset) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: componentType -> componentType
    json_t* j_componentType = json_object_get(json_obj, "componentType");
    if (j_componentType && !json_is_null(j_componentType)) {
        if (json_is_integer(j_componentType)) { out_struct->componentType = (int)json_integer_value(j_componentType); }
        else if (json_is_boolean(j_componentType)) { out_struct->componentType = json_is_true(j_componentType) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'componentType', got %s in Accessor.\n", json_typeof_str(json_typeof(j_componentType))); exit(EXIT_FAILURE); }
    } else if (!j_componentType) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: count -> count
    json_t* j_count = json_object_get(json_obj, "count");
    if (j_count && !json_is_null(j_count)) {
        if (json_is_integer(j_count)) { out_struct->count = (int)json_integer_value(j_count); }
        else if (json_is_boolean(j_count)) { out_struct->count = json_is_true(j_count) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'count', got %s in Accessor.\n", json_typeof_str(json_typeof(j_count))); exit(EXIT_FAILURE); }
    } else if (!j_count) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: type -> type
    json_t* j_type = json_object_get(json_obj, "type");
    if (j_type && !json_is_null(j_type)) {
        if (json_is_string(j_type)) {
            const char* str_val = json_string_value(j_type);
            if (str_val) {
                out_struct->type = strdup(str_val);
                if (!out_struct->type) { fprintf(stderr, "Error: strdup failed for 'type' in Accessor.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'type', got %s in Accessor.\n", json_typeof_str(json_typeof(j_type))); exit(EXIT_FAILURE); }
    } else if (!j_type) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: min -> min
    json_t* j_min = json_object_get(json_obj, "min");
    if (j_min && !json_is_null(j_min)) {
        if (json_is_array(j_min)) {
            size_t json_array_size_val = json_array_size(j_min);
            out_struct->min = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_min, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'min' in Accessor.\n", i); exit(EXIT_FAILURE); }
                if (json_is_integer(item_json)) { arrput(out_struct->min, (int)json_integer_value(item_json)); }
                else if (json_is_boolean(item_json)) { arrput(out_struct->min, json_is_true(item_json) ? 1 : 0); }
                else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected int/bool in array 'min', got %s in Accessor.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'min', got %s in Accessor.\n", json_typeof_str(json_typeof(j_min))); exit(EXIT_FAILURE); }
    } else if (!j_min) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: max -> max
    json_t* j_max = json_object_get(json_obj, "max");
    if (j_max && !json_is_null(j_max)) {
        if (json_is_array(j_max)) {
            size_t json_array_size_val = json_array_size(j_max);
            out_struct->max = NULL;
            for (size_t i = 0; i < json_array_size_val; ++i) {
                json_t* item_json = json_array_get(j_max, i);
                if (!item_json) { fprintf(stderr, "Error: Failed to get item %zu from array 'max' in Accessor.\n", i); exit(EXIT_FAILURE); }
                if (json_is_integer(item_json)) { arrput(out_struct->max, (int)json_integer_value(item_json)); }
                else if (json_is_boolean(item_json)) { arrput(out_struct->max, json_is_true(item_json) ? 1 : 0); }
                else if (!json_is_null(item_json)) { fprintf(stderr, "Error: Expected int/bool in array 'max', got %s in Accessor.\n", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected array for 'max', got %s in Accessor.\n", json_typeof_str(json_typeof(j_max))); exit(EXIT_FAILURE); }
    } else if (!j_max) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_bufferview(json_t* json_obj, Bufferview* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Bufferview, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_bufferview\n"); exit(EXIT_FAILURE); }
    // Parsing field: buffer -> buffer
    json_t* j_buffer = json_object_get(json_obj, "buffer");
    if (j_buffer && !json_is_null(j_buffer)) {
        if (json_is_integer(j_buffer)) { out_struct->buffer = (int)json_integer_value(j_buffer); }
        else if (json_is_boolean(j_buffer)) { out_struct->buffer = json_is_true(j_buffer) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'buffer', got %s in Bufferview.\n", json_typeof_str(json_typeof(j_buffer))); exit(EXIT_FAILURE); }
    } else if (!j_buffer) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: byteOffset -> byteOffset
    json_t* j_byteOffset = json_object_get(json_obj, "byteOffset");
    if (j_byteOffset && !json_is_null(j_byteOffset)) {
        if (json_is_integer(j_byteOffset)) { out_struct->byteOffset = (int)json_integer_value(j_byteOffset); }
        else if (json_is_boolean(j_byteOffset)) { out_struct->byteOffset = json_is_true(j_byteOffset) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'byteOffset', got %s in Bufferview.\n", json_typeof_str(json_typeof(j_byteOffset))); exit(EXIT_FAILURE); }
    } else if (!j_byteOffset) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: byteLength -> byteLength
    json_t* j_byteLength = json_object_get(json_obj, "byteLength");
    if (j_byteLength && !json_is_null(j_byteLength)) {
        if (json_is_integer(j_byteLength)) { out_struct->byteLength = (int)json_integer_value(j_byteLength); }
        else if (json_is_boolean(j_byteLength)) { out_struct->byteLength = json_is_true(j_byteLength) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'byteLength', got %s in Bufferview.\n", json_typeof_str(json_typeof(j_byteLength))); exit(EXIT_FAILURE); }
    } else if (!j_byteLength) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_buffer(json_t* json_obj, Buffer* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Buffer, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_buffer\n"); exit(EXIT_FAILURE); }
    // Parsing field: byteLength -> byteLength
    json_t* j_byteLength = json_object_get(json_obj, "byteLength");
    if (j_byteLength && !json_is_null(j_byteLength)) {
        if (json_is_integer(j_byteLength)) { out_struct->byteLength = (int)json_integer_value(j_byteLength); }
        else if (json_is_boolean(j_byteLength)) { out_struct->byteLength = json_is_true(j_byteLength) ? 1 : 0; }
        else { fprintf(stderr, "Error: Expected int/bool for 'byteLength', got %s in Buffer.\n", json_typeof_str(json_typeof(j_byteLength))); exit(EXIT_FAILURE); }
    } else if (!j_byteLength) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}

static void parse_json_to_asset(json_t* json_obj, Asset* out_struct) {
    if (!json_is_object(json_obj)) { fprintf(stderr, "Error: Expected JSON object for parsing into Asset, but got %s.\n", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }
    if (!out_struct) { fprintf(stderr, "Error: NULL out_struct provided to parse_json_to_asset\n"); exit(EXIT_FAILURE); }
    // Parsing field: version -> version
    json_t* j_version = json_object_get(json_obj, "version");
    if (j_version && !json_is_null(j_version)) {
        if (json_is_string(j_version)) {
            const char* str_val = json_string_value(j_version);
            if (str_val) {
                out_struct->version = strdup(str_val);
                if (!out_struct->version) { fprintf(stderr, "Error: strdup failed for 'version' in Asset.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'version', got %s in Asset.\n", json_typeof_str(json_typeof(j_version))); exit(EXIT_FAILURE); }
    } else if (!j_version) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
    // Parsing field: generator -> generator
    json_t* j_generator = json_object_get(json_obj, "generator");
    if (j_generator && !json_is_null(j_generator)) {
        if (json_is_string(j_generator)) {
            const char* str_val = json_string_value(j_generator);
            if (str_val) {
                out_struct->generator = strdup(str_val);
                if (!out_struct->generator) { fprintf(stderr, "Error: strdup failed for 'generator' in Asset.\n"); exit(EXIT_FAILURE); }
            }
        } else { fprintf(stderr, "Error: Expected string for 'generator', got %s in Asset.\n", json_typeof_str(json_typeof(j_generator))); exit(EXIT_FAILURE); }
    } else if (!j_generator) { /* Key not found, field remains default */ }
    else { /* Key is null, field remains default */ }
}


// --- Main Parsing Function (exit on error) ---
EveDat* parse_evedat_from_string(const char* json_string) {
    if (!json_string) { fprintf(stderr, "Error: NULL JSON string provided.\n"); exit(EXIT_FAILURE); }
    json_error_t error;
    json_t* root_json = json_loads(json_string, 0, &error);
    if (!root_json) { fprintf(stderr, "JSON parsing error at string: %s (line %d, column %d)\n", error.text, error.line, error.column); exit(EXIT_FAILURE); }
    if (!json_is_object(root_json)) { fprintf(stderr, "Error: Root JSON is not an object, got %s.\n", json_typeof_str(json_typeof(root_json))); json_decref(root_json); exit(EXIT_FAILURE); }
    EveDat* result = (EveDat*)calloc(1, sizeof(EveDat));
    if (!result) { fprintf(stderr, "Memory allocation failed for root struct\n"); json_decref(root_json); exit(EXIT_FAILURE); }
    parse_json_to_evedat(root_json, result);
    json_decref(root_json);
    return result;
}

EveDat* parse_evedat_from_file(const char* filename) {
    if (!filename) { fprintf(stderr, "Error: NULL filename provided.\n"); exit(EXIT_FAILURE); }
    FILE* fp = fopen(filename, "rb");
    if (!fp) { perror("Error opening file"); fprintf(stderr, "Filename: %s\n", filename); exit(EXIT_FAILURE); }
    fseek(fp, 0, SEEK_END); long length = ftell(fp); fseek(fp, 0, SEEK_SET);
    if (length < 0) { fprintf(stderr, "Error: Could not determine size of file %s\n", filename); fclose(fp); exit(EXIT_FAILURE); }
    char* buffer = (char*)malloc(length + 1);
    if (!buffer) { fprintf(stderr, "Memory allocation failed for file buffer (%ld bytes) for %s\n", length + 1, filename); fclose(fp); exit(EXIT_FAILURE); }
    size_t read_len = fread(buffer, 1, length, fp);
    if (read_len != (size_t)length) { fprintf(stderr, "Error: Failed to read entire file %s (read %zu of %ld bytes)\n", filename, read_len, length); free(buffer); fclose(fp); exit(EXIT_FAILURE); }
    fclose(fp); buffer[length] = '\0';
    EveDat* result = parse_evedat_from_string(buffer);
    free(buffer);
    return result;
}
