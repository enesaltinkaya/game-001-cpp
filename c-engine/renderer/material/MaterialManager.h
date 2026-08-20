#pragma once

typedef struct Material Material;

void createDefaultMaterial();
void cleanupMaterials();

Material* createMaterial(const char* name);
Material* getMaterialByName(const char* name);
Material* getMaterialById(const u32 id);
void destroyMaterial(const u32 id);
