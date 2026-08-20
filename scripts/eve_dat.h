#ifndef EVE_DAT_H
#define EVE_DAT_H

#include <stddef.h> // For size_t (still useful for general C)

struct EveDat;
struct Scene;
struct Node;
struct Animation;
struct Channel;
struct Target;
struct Skin;
struct Meshe;
struct Primitive;
struct Attributes;
struct Material;
struct Extensions;
struct KhrMaterialsSpecular;
struct Speculartexture;
struct Normaltexture;
struct Pbrmetallicroughness;
struct Basecolortexture;
struct Texture;
struct Image;
struct Sampler;
struct Accessor;
struct Bufferview;
struct Buffer;
struct Asset;

// Struct Definitions
typedef struct EveDat {
    struct Asset* asset;
    char** extensionsUsed;
    char** extensionsRequired;
    struct Buffer* buffers;
    struct Bufferview* bufferViews;
    struct Accessor* accessors;
    struct Sampler* samplers;
    struct Image* images;
    struct Texture* textures;
    struct Material* materials;
    struct Meshe* meshes;
    struct Skin* skins;
    struct Animation* animations;
    struct Node* nodes;
    struct Scene* scenes;
    int scene;
} EveDat;

typedef struct Scene {
    char* name;
    int* nodes;
} Scene;

typedef struct Node {
    int mesh;
    int skin;
    double* translation;
    double* scale;
} Node;

typedef struct Animation {
    char* name;
    struct Sampler* samplers;
    struct Channel* channels;
} Animation;

typedef struct Channel {
    int sampler;
    struct Target* target;
} Channel;

typedef struct Target {
    int node;
    char* path;
} Target;

typedef struct Skin {
    char* name;
    int* joints;
    int inverseBindMatrices;
} Skin;

typedef struct Meshe {
    struct Primitive* primitives;
} Meshe;

typedef struct Primitive {
    struct Attributes* attributes;
    int indices;
    int material;
} Primitive;

typedef struct Attributes {
    int POSITION;
    int NORMAL;
    int TEXCOORD_0;
    int JOINTS_0;
    int WEIGHTS_0;
} Attributes;

typedef struct Material {
    char* name;
    struct Pbrmetallicroughness* pbrMetallicRoughness;
    struct Normaltexture* normalTexture;
    int doubleSided;
    struct Extensions* extensions;
} Material;

typedef struct Extensions {
    struct KhrMaterialsSpecular* KHR_materials_specular;
} Extensions;

typedef struct KhrMaterialsSpecular {
    struct Speculartexture* specularTexture;
} KhrMaterialsSpecular;

typedef struct Speculartexture {
    int index;
    struct Extensions* extensions;
} Speculartexture;

typedef struct Normaltexture {
    int index;
    int scale;
    struct Extensions* extensions;
} Normaltexture;

typedef struct Pbrmetallicroughness {
    struct Basecolortexture* baseColorTexture;
    int metallicFactor;
    double roughnessFactor;
} Pbrmetallicroughness;

typedef struct Basecolortexture {
    int index;
    struct Extensions* extensions;
} Basecolortexture;

typedef struct Texture {
    int sampler;
    struct Extensions* extensions;
} Texture;

typedef struct Image {
    char* name;
    int bufferView;
    char* mimeType;
} Image;

typedef struct Sampler {
    int magFilter;
    int minFilter;
} Sampler;

typedef struct Accessor {
    int bufferView;
    int byteOffset;
    int componentType;
    int count;
    char* type;
    int* min;
    int* max;
} Accessor;

typedef struct Bufferview {
    int buffer;
    int byteOffset;
    int byteLength;
} Bufferview;

typedef struct Buffer {
    int byteLength;
} Buffer;

typedef struct Asset {
    char* version;
    char* generator;
} Asset;


// Parsing Function Declaration
EveDat* parse_evedat_from_string(const char* json_string);
EveDat* parse_evedat_from_file(const char* filename);

// Freeing Function Declarations
void free_evedat(EveDat* data);
void free_scene(Scene* data);
void free_node(Node* data);
void free_animation(Animation* data);
void free_channel(Channel* data);
void free_target(Target* data);
void free_skin(Skin* data);
void free_meshe(Meshe* data);
void free_primitive(Primitive* data);
void free_attributes(Attributes* data);
void free_material(Material* data);
void free_extensions(Extensions* data);
void free_khrmaterialsspecular(KhrMaterialsSpecular* data);
void free_speculartexture(Speculartexture* data);
void free_normaltexture(Normaltexture* data);
void free_pbrmetallicroughness(Pbrmetallicroughness* data);
void free_basecolortexture(Basecolortexture* data);
void free_texture(Texture* data);
void free_image(Image* data);
void free_sampler(Sampler* data);
void free_accessor(Accessor* data);
void free_bufferview(Bufferview* data);
void free_buffer(Buffer* data);
void free_asset(Asset* data);

#endif // EVE_DAT_H