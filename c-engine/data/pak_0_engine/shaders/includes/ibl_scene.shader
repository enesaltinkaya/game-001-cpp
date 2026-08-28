/* ibl_scene.shader — scene-side IBL sampling helpers
 *
 * Include AFTER utils.shader + globalset.shader + ibl_common.shader.
 * Used by the scene and heightmap_terrain fragment shaders.  Sample
 * directions are rotated by mat3(envRotation) (the inverse of the
 * IBL sun rotation) so the extracted sun light, the shadow cascade and
 * the prefiltered reflection directions all agree.
 */

#ifndef IBL_SCENE_SHADER
#define IBL_SCENE_SHADER

vec3 iblSampleEnvironment(vec3 dir, float lod) {
    dir = mat3(sceneBuffer.ibl.envRotation) * dir;
    return textureLod(sampler2D(textures[nonuniformEXT(sceneBuffer.ibl.environmentMapIndex)],
                                samplers[SAMPLER_LINEAR]),
                      directionToEquirectUv(dir),
                      clamp(lod, 0.0, sceneBuffer.ibl.environmentMapMaxLod))
        .rgb;
}

vec3 iblSampleIrradiance(vec3 dir) {
    dir = mat3(sceneBuffer.ibl.envRotation) * dir;
    return min(texture(samplerCube(cubeTextures[nonuniformEXT(sceneBuffer.ibl.irradianceMapIndex)],
                                   samplers[SAMPLER_LINEAR]),
                       normalize(dir))
                   .rgb,
               vec3(32.0));
}

vec3 iblEvaluateSHIrradiance(vec3 N) {
    vec3 rN         = mat3(sceneBuffer.ibl.envRotation) * N;
    const float A0  = PI;
    const float A1  = 2.0 * PI / 3.0;
    const float Y00 = 0.282095;
    const float Y1x = 0.488603;
    vec3 irr        = vec3(0.0);
    irr += sceneBuffer.ibl.shL0_M0.rgb * A0 * Y00;
    irr += sceneBuffer.ibl.shL1_Mp1.rgb * A1 * Y1x * rN.x;
    irr += sceneBuffer.ibl.shL1_Mn1.rgb * A1 * Y1x * rN.y;
    irr += sceneBuffer.ibl.shL1_M0.rgb * A1 * Y1x * rN.z;
    return max(irr, vec3(0.0));
}

vec3 iblSamplePrefilter(vec3 dir, float lod) {
    dir = mat3(sceneBuffer.ibl.envRotation) * dir;
    return textureLod(samplerCube(cubeTextures[nonuniformEXT(sceneBuffer.ibl.prefilterMapIndex)],
                                  samplers[SAMPLER_LINEAR]),
                      normalize(dir),
                      clamp(lod, 0.0, sceneBuffer.ibl.prefilterMapMaxLod))
        .rgb;
}

#endif /* IBL_SCENE_SHADER */
