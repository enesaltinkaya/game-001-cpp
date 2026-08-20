# Forward+ Rendering Pipeline: 4K Native
**Target:** 4K 80-100fps on GTX 1080 Ti
**Architecture:** Forward+ (Z-Prepass) + Clustered Lighting + PBR IBL + Half-Res Compute + 8x TSSAA

---

## Phase 0: Level Load (One-Time Pre-Computation)

### IBL Pre-Computation
*Generates the static lighting data for PBR environment reflections. Runs once at startup or level load.*

- **1. Diffuse Irradiance Map:**
  - **Logic:** Convolution shader (solves the diffuse integral). Samples the source HDR Skybox over a hemisphere.
  - **Resolution:** 32×32 Cubemap.
  - **Format:** `R11G11B10_FLOAT`.

- **2. Specular Pre-Filtered Map:**
  - **Logic:** Quasi-Monte Carlo simulation for varying roughness levels (Split-Sum Approximation Part 1).
  - **Resolution:** 128×128 Cubemap with 5 Mips.
  - **Format:** `R11G11B10_FLOAT`.

- **3. BRDF Integration LUT:**
  - **Logic:** Pre-integrates the BRDF response for Roughness vs. View Angle (Split-Sum Part 2).
  - **Resolution:** 512×512 2D Texture.
  - **Format:** `R16G16_SFLOAT` (RG channels only).

- **4. Blue Noise Texture:**
  - **Resolution:** 128×128 2D Texture.
  - **Format:** `R8_UNORM`.
  - **Usage:** Tiled/rotated per frame for SSR jitter using Halton-derived 2D rotation.

---

## Frame Resource Management (1 Frame In Flight)

### Static Resources (Level Load)
*Loaded once at startup or level load. Persist across all frames.*

- **IrradianceMap:** `R11G11B10_FLOAT` Cubemap (32×32, ~0.02MB)
- **PrefilterMap:** `R11G11B10_FLOAT` Cubemap (128×128, 5 mips, ~0.4MB)
- **BRDF_LUT:** `R16G16_SFLOAT` 2D Texture (512×512, ~1MB)
- **BlueNoise:** `R8_UNORM` 2D Texture (128×128, ~0.016MB)

### Single-Buffered Resources
*Single graphics queue execution for deterministic performance profiling.*

**Core Frame Resources (1x each):**
- **DepthBuffer:** `D32_SFLOAT` (3840×2160, ~32MB)
- **HiZBuffer:** `R32_SFLOAT` Mip Chain (3840×2160 down to 16×16, ~43MB)
- **VelocityBuffer:** `R16G16_SFLOAT` (3840×2160, ~32MB) - Supports up to ±32,767 pixels/frame velocity
- **Scene Color (RT0):** `R11G11B10_FLOAT` (3840×2160, ~32MB) - Raw lit output
- **Normals (RT1):** `R16G16_SFLOAT` (3840×2160, ~32MB)
- **Material (RT2):** `R8G8_UNORM` (3840×2160, ~16MB) - RG: Roughness/Metallic
- **ShadowAtlas:** `D32_SFLOAT` (4096×4096 Atlas, ~67MB)

**Screen-Space Effect Buffers (Optimized for Bandwidth):**
- **AOTexture:** `R8_UNORM` (1920×1080 half-res, ~2MB)
- **SSRTexture:** `R16G16B16A16_SFLOAT` (1920×1080 half-res, ~16MB)
- **SSRTextureUpsampled:** `R16G16B16A16_SFLOAT` (3840×2160, ~64MB)
- **CompositeScene:** `R16G16B16A16_SFLOAT` (3840×2160, ~64MB)
- **ResolvedColor:** `R16G16B16A16_SFLOAT` (3840×2160, ~64MB) - TSSAA output.

**Post-Processing Intermediate Buffers:**
- **MotionTileBuffer:** `R8_UINT` (240×135 tiles, <1MB) - Motion blur tile classification
- **BlurredScene:** `B10G11R11_UFLOAT_PACK32` (3840×2160, ~32MB)
- **BloomTexture:** `B10G11R11_UFLOAT_PACK32` (1920×1080 half-res, ~8MB)
- **Bloom Mip Chain:** `B10G11R11_UFLOAT_PACK32` (5 mips, ~10MB total)
- **BloomTempBuffer:** `B10G11R11_UFLOAT_PACK32` (1920×1080, ~8MB) - Ping-pong buffer

**Previous Frame Resources (Persistent):**
- **PrevHiZBuffer:** `R32_SFLOAT` Mip Chain (Swapped from HiZBuffer)
- **PrevDepthBuffer:** `R32_SFLOAT` Copy (3840×2160, ~32MB) - For TSSAA depth rejection
- **HistoryColor:** `R16G16B16A16_SFLOAT` (Swapped from ResolvedColor)

**GPU-Only Visibility Resources:**
- **VisibilityBuffer:** `R32_UINT` (Per-object visibility flags, ~200KB for 50k objects with padding)
- **IndirectDrawBuffer:** `DrawIndexedIndirectCommand[]` (~1MB for 50k objects)
- **LightGrid:** `StructuredBuffer<uint>` (16×16×24 clusters, variable light count, ~5MB typical)

**Resource Swap Pattern (End of Frame):**
```cpp
swap(hizBuffer, prevHizBuffer);          // Hi-Z for next frame's occlusion
swap(resolvedColor, historyColor);       // TSSAA output becomes input
// Copy current depth to prevDepth for next frame's TSSAA rejection
copyImage(depthBuffer, prevDepthBuffer);
```

---

## Phase 1: Per-Frame Setup (CPU)

### 1a. Jitter Generation
- **Algorithm:** Halton (2, 3) Sequence.
- **Cycle Length:** 8 Frames.
- **Logic:**
  - Generate sub-pixel offset `(x, y)` range `[-0.5, +0.5]`.
  - Apply offset to **Projection Matrix** (Shear Transform).
- **Output Uniforms:**
  - `JitterOffset` (vec2)
  - `ViewProjectionMatrix` (Jittered)
  - `ViewProjectionMatrixNoJitter` (Non-Jittered - for velocity calculation)
  - `PrevViewProjectionMatrix` (Previous frame's jittered matrix)
  - `PrevViewProjectionMatrixNoJitter` (Previous frame's non-jittered matrix)

### 1b. Frustum & Occlusion Culling (Compute - On Graphics Queue)

**Phase 1: Visibility Test (Compute)**
- **Logic:**
  1. Frustum cull all objects against view frustum.
  2. For frustum-visible objects, test bounding box against **PrevHiZBuffer** (conservative occlusion query).
  3. Project AABB to screen-space and calculate covering rectangle.
  4. Sample coarsest mip level that fully covers the screen-space AABB.
  5. Compare object's nearest depth against Hi-Z depth value.
- **Inputs:** `ObjectList` (StructuredBuffer), `PrevHiZBuffer`
- **Outputs:** `VisibilityBuffer` (GPU-only, uint per object)
- **Note:** This queries the previous frame's Hi-Z, so newly visible objects get a 1-frame latency (acceptable trade-off).

**Phase 2: Indirect Draw Setup (Compute)**
- **Logic:** Build indirect draw commands (`DrawIndexedIndirect`) for visible objects.
- **Inputs:** `VisibilityBuffer`
- **Outputs:** `IndirectDrawBuffer`

---

## Phase 2: Depth Pre-Pass (Graphics)
*Critical: Runs first to populate Depth for AO, Light Culling, and Hi-Z.*
*Uses JITTERED ViewProj for both Pre-Pass and Main Pass to ensure perfect depth matching.*

**Pascal Hardware Warning:**
- Although `VelocityBuffer` uses `R16G16_SFLOAT` storage, **do not use `half` or `float16_t` for arithmetic** in the shader.
- The GTX 1080 Ti (Pascal) has a 1:64 FP16 rate. Perform all calculations in `float` (FP32) and only cast to 16-bit when writing to the output attachment.

**Jitter Strategy:**
- **Depth Calculation:** Use Jittered ViewProj Matrix (same as Main Pass) to ensure `gl_FragCoord.z` matches exactly.
- **Velocity Calculation:** Use Non-Jittered matrices to calculate clean motion vectors.
  - `CurrentPosNoJitter = ViewProjectionMatrixNoJitter * worldPos`
  - `PrevPosNoJitter = PrevViewProjectionMatrixNoJitter * prevWorldPos`
  - `Velocity = (CurrentPosNoJitter.xy / CurrentPosNoJitter.w) - (PrevPosNoJitter.xy / PrevPosNoJitter.w)`

**2a. Static Geometry (Depth + Camera Velocity)**
- **State:** Depth Write = ON, Color Write = ON (Velocity Only).
- **Depth Function:** `GREATER_EQUAL` (Reverse-Z).
- **Vertex Shader:**
  - Calculate depth using **Jittered** ViewProj.
  - Calculate velocity using **Non-Jittered** current and previous ViewProj.
  - *Note:* Even for static objects, `PrevPosNoJitter` differs from `CurrentPosNoJitter` if the camera moved.
  - `Velocity = (CurrentPosNoJitter.xy / CurrentPosNoJitter.w) - (PrevPosNoJitter.xy / PrevPosNoJitter.w)`.
  - Scale velocity to pixel units: `Velocity *= vec2(ScreenWidth, ScreenHeight)`.
  - Clamp velocity to prevent overflow: `Velocity = clamp(Velocity, vec2(-32767.0), vec2(32767.0))`.
  - Store in `R16G16_SFLOAT` (supports ±65,504 range, clamped to ±32,767 for safety).
- **Inputs:** `VertexData`, `IndirectDrawBuffer` (Static objects)
- **Outputs:**
  - **DepthBuffer:** `D32_SFLOAT`
  - **VelocityBuffer:** `R16G16_SFLOAT` (pixel-space velocity)

**2b. Dynamic Geometry (Depth + Object Velocity)**
- **State:** Depth Write = ON, Color Write = ON (Velocity Only).
- **Depth Function:** `GREATER_EQUAL` (Reverse-Z - same as 2a, combined into single depth buffer).
- **Vertex Shader:**
  - Calculate depth using **Jittered** ViewProj.
  - Calculate `Velocity` using skinned/animated `PrevPosition` with **Non-Jittered** matrices.
  - Apply same clamping as 2a.
- **Inputs:** `VertexData` (Pos + PrevPos for skinned meshes), `IndirectDrawBuffer` (Dynamic objects)
- **Outputs:**
  - **DepthBuffer:** `D32_SFLOAT` (Appended)
  - **VelocityBuffer:** `R16G16_SFLOAT` (Appended)
- **Note:** Both 2a and 2b render to the same depth buffer in a single pass. Objects naturally sort by depth due to Reverse-Z `GREATER_EQUAL` test.

**Pipeline Barrier 2→3:**
- **Stage:** `VERTEX_SHADER_BIT | FRAGMENT_SHADER_BIT` → `COMPUTE_SHADER_BIT`
- **Access:** `SHADER_WRITE` → `SHADER_READ`
- **Resources:** `VelocityBuffer`
- **Ensures:** Velocity buffer fully written before compute shaders read it in Phase 7b.

**Pipeline Barrier 2→4:**
- **Stage:** `LATE_FRAGMENT_TESTS_BIT` → `COMPUTE_SHADER_BIT`
- **Access:** `DEPTH_STENCIL_ATTACHMENT_WRITE` → `SHADER_READ`
- **Resources:** `DepthBuffer`
- **Ensures:** Depth is fully written before compute shaders read it.

---

## Phase 3: Shadow Atlas (Graphics)
*Single queue execution - no async overlap.*

### Cascaded Shadow Maps (CSM)
- **Technique:** Stable CSM + PCF 5×5.
- **Config:** 4 Cascades.
  - **Resolution:** 2048×2048 per cascade.
  - **Layout:** Single Atlas (4096×4096 split 2×2).
  - **Precision:** `D32_SFLOAT` (~67MB total).
  - **Split Scheme:** Logarithmic-Linear Blend (Lambda = 0.75).
    - **Near Plane:** Camera's near plane (e.g., 0.1m).
    - **Far Plane:** Camera's far plane (e.g., 500m).
    - **Formula per cascade i:**
      ```
      C_log = near * pow(far/near, i/cascadeCount)
      C_linear = near + (far - near) * (i/cascadeCount)
      C_i = lambda * C_log + (1 - lambda) * C_linear
      ```
    - **Runtime Calculation:** Split distances are recalculated each frame based on camera frustum.
- **Culling:** Frustum Cull from Light's POV per cascade.
- **Inputs:** `VertexData` (Position Only), `IndirectDrawBuffer` (Shadow casters)
- **Outputs:** **ShadowAtlas** `D32_SFLOAT`

**Pipeline Barrier 3→5:**
- **Stage:** `LATE_FRAGMENT_TESTS_BIT` → `FRAGMENT_SHADER_BIT`
- **Access:** `DEPTH_STENCIL_ATTACHMENT_WRITE` → `SHADER_READ`
- **Resources:** `ShadowAtlas`
- **Ensures:** Shadow map writes complete before Main Forward Pass samples shadows.

---

## Phase 4: Compute Prep (Compute)
*Sequential execution on graphics queue after shadow pass.*

### 4a. Hierarchical Z-Buffer (Hi-Z) Generation
- **Logic:** Generate mip levels from the now-filled `DepthBuffer`.
- **Reduction:** Max reduction (Reverse-Z: take maximum value for conservative occlusion, representing the closest visible depth in the tile, or Min if representing farthest - *Correction:* For Occlusion Culling in Reverse-Z, we need the **Minimum** depth in the tile (the farthest point) to test if an object is behind it. For SSR, we often need Min/Max).
  - *Standard Reverse-Z Hi-Z:* **Minimum** (furthest) for culling.
- **Inputs:** `DepthBuffer`
- **Outputs:** **HiZBuffer**
- **Note:** This Hi-Z is used for SSR in the current frame (Phase 6a) and for occlusion culling in the **next frame** (Phase 1b).

### 4b. Visibility Bitmask AO (VB-AO)
- **Resolution:** Half-Res (1920×1080).
- **Logic:** Raymarch against `DepthBuffer` to calculate occlusion.
- **Precision:** `R8_UNORM` (Single channel).
- **Inputs:** `DepthBuffer` (Full-Res, Read-Only)
- **Outputs:** **AOTexture** `R8_UNORM` (Half-Res)

### 4c. Clustered Light Culling
- **Logic:**
  1. Compute AABB for each frustum cluster (16×16×24 grid in screen-space + depth).
  2. Calculate min/max depth per cluster from `DepthBuffer`.
  3. Intersect cluster AABB with all scene lights (sphere-AABB or cone-AABB tests).
  4. Write light indices to per-cluster linked list in `LightGrid`.
- **Inputs:** `DepthBuffer` (Min/Max depth per tile), `LightList` (StructuredBuffer of all scene lights)
- **Outputs:** **LightGrid** `StructuredBuffer<uint>` (Light indices per cluster)

**Compute-to-Graphics Barrier:**
- **Stage:** `COMPUTE_SHADER_BIT` → `FRAGMENT_SHADER_BIT`
- **Access:** `SHADER_WRITE` → `SHADER_READ`
- **Resources:** `HiZBuffer`, `AOTexture`, `LightGrid`

---

## Phase 5: Main Forward Pass (Graphics)
*Uses JITTERED ViewProj Matrix (same as Pre-Pass) to ensure perfect depth matching.*

### 5a. Opaque Geometry Shading
- **State:** Depth Test = `EQUAL` (Matches Pre-Pass), Depth Write = `OFF`.
- **Depth Bias (Optional):** If precision issues arise on specific hardware, enable `depthBiasEnable` with:
  - `depthBiasConstantFactor = 0`
  - `depthBiasSlopeFactor = 0.0`
  - `depthBiasClamp = 0.0`
- **Logic:**
  - **Cluster Lookup:** Calculate cluster index from `gl_FragCoord.xy` and depth.
    ```glsl
    uint clusterX = uint(gl_FragCoord.x) / TILE_SIZE;
    uint clusterY = uint(gl_FragCoord.y) / TILE_SIZE;
    float linearDepth = linearizeDepth(gl_FragCoord.z);
    uint clusterZ = uint(linearDepth / depthSliceSize);
    uint clusterIndex = clusterX + clusterY * gridWidth + clusterZ * gridWidth * gridHeight;
    ```
  - **Direct Light:** Loop through lights in current cluster + CSM Shadows (PCF 5×5).
  - **Ambient Occlusion:**
    1. Calculate half-res UV from full-res fragment coord: `aoUV = gl_FragCoord.xy / vec2(3840, 2160)`.
    2. Sample `AOTexture` at half-res.
    3. Apply depth-aware bilateral upsample (3×3 tap, weight by depth similarity).
  - **Indirect:** IBL (Irradiance + Prefilter) × BRDF.
    ```glsl
    vec3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kD = (1.0 - F) * (1.0 - metallic);
    vec3 irradiance = texture(irradianceMap, N).rgb;
    vec3 diffuseIBL = kD * irradiance * albedo;
    vec3 prefilteredColor = textureLod(prefilterMap, R, roughness * maxMipLevel).rgb;
    vec2 brdf = texture(brdfLUT, vec2(NdotV, roughness)).rg;
    vec3 specularIBL = prefilteredColor * (F * brdf.x + brdf.y);
    vec3 ambient = (diffuseIBL + specularIBL) * ao;
    ```
- **Inputs:**
  - `VertexData`, `IrradianceMap`, `PrefilterMap`, `BRDF_LUT`
  - `DepthBuffer`, `ShadowAtlas`, `LightGrid`, `AOTexture`
- **Outputs (MRT):**
  - **RT0 (Scene Color):** `R11G11B10_FLOAT` (HDR)
  - **RT1 (Normals):** `R16G16_SFLOAT` (Octahedron-encoded view-space normals)
  - **RT2 (Material):** `R8G8_UNORM` (R: Roughness, G: Metallic)

### 5b. Skybox
- **State:** Depth Test = `EQUAL` or `LESS_EQUAL`, Depth Write = `OFF`.
- **Vertex Shader:** Output `depth = 0.0` (far plane in Reverse-Z).
- **Logic:** Sample HDR cubemap using view direction.
  - Since we use Reverse-Z (Far=0.0), and we cleared Depth to 0.0, we can use `EQUAL` to 0.0 to only draw on the background.
  ```glsl
  vec3 viewDir = normalize(worldPos - cameraPos);
  vec3 skyColor = texture(skyboxCubemap, viewDir).rgb;
  ```
- **Outputs:** **RT0 (Scene Color)**

### 5c. Transparency
- **State:** Depth Test = `GREATER` (Reverse-Z), Depth Write = `OFF`, Blend = `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`.
- **Logic:** Render transparent objects back-to-front (CPU-sorted by distance to camera).
- **Outputs:** **RT0 (Scene Color)**

---

## Phase 6: Screen Space Reflections (Compute)

### 6a. SSR Raymarching (Half-Res)
- **Resolution:** Half-Res (1920×1080).
- **Precision:** `R16G16B16A16_SFLOAT`
- **Logic:**
  1. **Cull:** Skip if `Roughness > 0.7` (from RT2 upsampled to half-res).
  2. **Reflection Vector:** Calculate from view direction and normal (RT1).
  3. **Jitter:** Apply Halton-derived 2D rotation to reflection vector using blue noise.
  4. **Trace:** Hi-Z Raymarch using `HiZBuffer`:
    - Start from surface position in screen-space.
    - March along reflection vector in screen-space.
    - Use hierarchical ray marching: start with coarse mip, refine on potential hits.
    - Test depth at each step against Hi-Z mip hierarchy.
    - **Reverse-Z Check:** Hit if `currentPos.z >= sampledDepth`.
    - Early exit on hit or screen-edge.
  5. **Sample:** On hit, sample `RT0 (Scene Color)` at intersection point.
  6. **Fade:** Apply edge fade + depth fade + roughness fade.
- **Inputs:** `RT0 (Scene Color)`, `RT1 (Normals)`, `RT2 (Material)`, `HiZBuffer`, `BlueNoise`, `JitterOffset`
- **Outputs:** **SSRTexture** `R16G16B16A16_SFLOAT` (RGB: Color, A: Confidence)

### 6b. SSR Bilateral Upsample
- **Resolution:** Full-Res (3840×2160).
- **Precision:** `R16G16B16A16_SFLOAT` (Fixed).
- **Logic:**
  1. For each full-res pixel, sample 4 nearest half-res SSR texels (bilinear footprint).
  2. Weight each sample by:
     - **Depth similarity:** `weight_depth = exp(-abs(depth_sample - depth_center) / depthSigma)`
     - **Normal similarity:** `weight_normal = pow(max(0.0, dot(normal_sample, normal_center)), normalPower)`
  3. Normalize and output weighted average for both Color (RGB) and Confidence (A).
- **Inputs:** `SSRTexture` (Half-Res), `DepthBuffer`, `RT1 (Normals)`
- **Outputs:** **SSRTextureUpsampled** `R16G16B16A16_SFLOAT` (Full-Res)

---

## Phase 7: Composite & TSSAA (Compute)

### 7a. Composition
- **Logic:** Combine Scene Color with upsampled SSR using Fresnel term.
- **Formula:**
  ```glsl
  // Sample inputs
  vec3 sceneColor = texture(sceneColorRT, uv).rgb;
  vec4 ssrData = texture(ssrUpsampled, uv); // RGBA
  vec3 ssrColor = ssrData.rgb;
  float ssrConfidence = ssrData.a;
  
  vec3 normal = decodeOctahedron(texture(normalsRT, uv).rg);
  vec2 material = texture(materialRT, uv).rg;
  float roughness = material.r;
  float metallic = material.g;
  
  // Calculate Fresnel
  vec3 viewDir = normalize(cameraPos - worldPos);
  float NdotV = max(0.0, dot(normal, viewDir));
  vec3 F0 = mix(vec3(0.04), baseColor, metallic);
  vec3 fresnel = fresnelSchlickRoughness(NdotV, F0, roughness);
  
  // Composite with confidence mask
  vec3 composite = sceneColor + (ssrColor * fresnel * ssrConfidence);
  ```
- **Inputs:** `RT0 (Scene Color)`, `SSRTextureUpsampled`, `RT1 (Normals)`, `RT2 (Material)`
- **Outputs:** **CompositeScene** `R16G16B16A16_SFLOAT`

### 7b. TSSAA Resolve
- **Algorithm:** Temporal Super-Sampling Anti-Aliasing with Variance Clipping and Depth Rejection.
- **Optimization:** Use subgroup shuffle operations for 3×3 velocity dilation.
- **Logic:**
  1. **Velocity Dilation (3×3):** Find closest depth velocity in 3×3 neighborhood using subgroup ops.
    - In Reverse-Z, "closest" means largest depth value.
  2. **Reproject:** Sample `HistoryColor` using dilated velocity.
    ```glsl
    // VelocityBuffer stores (Current - Prev).
    // To get History UV: CurrentUV - Velocity.
    vec2 velocity = closestVelocity / vec2(screenWidth, screenHeight); // Convert to UV space
    vec2 historyUV = currentUV - velocity;
    ```
  3. **Sample Current:** Sample `CompositeScene` at current pixel (jittered).
  4. **Color Space:** Convert Current and History to YCoCg for better variance estimation.
  5. **Neighborhood AABB (3×3):** Sample 3×3 neighborhood; calculate Min/Max AABB in YCoCg space.
  6. **Rectify:** Clip History color to AABB (variance clipping).
  7. **Disocclusion Check:**
    - **Test 1 (Boundaries):** Check if `historyUV` is outside `[0,1]`.
    - **Test 2 (Depth):** Reproject current world position to previous frame and compare depths.
      ```glsl
      // Reconstruct world position from current depth
      vec3 worldPos = reconstructWorldPos(currentUV, currentDepth);
      
      // Project to previous frame's screen space
      vec4 prevClipPos = prevViewProjectionMatrix * vec4(worldPos, 1.0);
      vec3 prevNDC = prevClipPos.xyz / prevClipPos.w;
      vec2 prevUV = prevNDC.xy * 0.5 + 0.5;
      float prevDepthReprojected = prevNDC.z;
      
      // Sample previous depth buffer at reprojected location
      float prevDepthStored = texture(prevDepthBuffer, prevUV).r;
      
      // Compare depths (account for depth derivative to avoid false positives on slopes)
      float depthThreshold = 0.001 + length(vec2(dFdx(currentDepth), dFdy(currentDepth))) * 2.0;
      bool depthMatch = abs(prevDepthReprojected - prevDepthStored) < depthThreshold;
      ```
    - **Test 3 (Boundary):** Check if reprojected UV is in valid range.
      ```glsl
      bool inBounds = all(greaterThanEqual(historyUV, vec2(0.0))) &&
                      all(lessThanEqual(historyUV, vec2(1.0)));
      bool isValid = inBounds && depthMatch;
      ```
  8. **Blend:** `result = mix(history, current, alpha)`.
    ```glsl
    float alpha = isValid ? 0.05 : 1.0; // Low alpha for temporal stability, high for disocclusion
    vec3 resolved = mix(historyYCoCg, currentYCoCg, alpha);
    ```
  9. **Convert:** YCoCg → RGB.
    ```glsl
    vec3 finalColor = YCoCgtoRGB(resolved);
    ```
- **Inputs:**
  - **Current:** `CompositeScene`
  - **History:** `HistoryColor`
  - **Velocity:** `VelocityBuffer`
  - **Depth (Current):** `DepthBuffer`
  - **Depth (Previous):** `PrevDepthBuffer`
  - **Camera Matrices:** `PrevViewProjectionMatrix` (for reprojection)
- **Outputs:** **ResolvedColor** `R16G16B16A16_SFLOAT` (Becomes HistoryColor for next frame)

**Pipeline Barrier 7→8:**
- **Stage:** `COMPUTE_SHADER_BIT` → `COMPUTE_SHADER_BIT`
- **Access:** `SHADER_WRITE` → `SHADER_READ`
- **Resources:** `ResolvedColor`

---

## Phase 8: Linear Post-Processing (Compute)

### 8a. Motion Blur Tile Classification
- **Logic:**
  1. Divide screen into 16×16 pixel tiles (240×135 tiles at 4K).
  2. For each tile, calculate max velocity magnitude using parallel reduction.
  3. **Fix:** Use `subgroupMax` (or atomic `InterlockedMax` in shared memory) to ensure a single fast-moving pixel flags the entire tile.
    ```glsl
    // Compute shader with 16x16 thread group per tile
    shared float maxVelocityTile;
    if (gl_LocalInvocationIndex == 0) maxVelocityTile = 0.0;
    barrier();
    
    vec2 velocity = texelFetch(velocityBuffer, ivec2(gl_GlobalInvocationID.xy), 0).rg;
    float velocityMag = length(velocity);
    
    // Parallel reduction using subgroup
    float maxVel = subgroupMax(velocityMag);
    if (subgroupElect()) {
      atomicMax(maxVelocityTile, maxVel); // Write max to shared memory
    }
    barrier();
    
    // First thread writes to tile buffer
    if (gl_LocalInvocationIndex == 0) {
      uint tileIndex = tileX + tileY * tilesWide;
      motionTileBuffer[tileIndex] = maxVelocityTile > 2.0 ? 1 : 0; // Mark as dynamic if > 2 pixels
    }
    ```
  4. Mark tiles with velocity < 2 pixels as "static" (value 0), >= 2 pixels as "dynamic" (value 1).
- **Inputs:** `VelocityBuffer`
- **Outputs:** **MotionTileBuffer** `R8_UINT` (1 byte per tile, 240×135 = ~32KB)

### 8b. Motion Blur
- **Resolution:** Full-Res (3840×2160).
- **Precision:** `B10G11R11_UFLOAT_PACK32` (Optimized).
- **Note:** Alpha channel is discarded here.
- **Logic:**
  1. Calculate tile index from pixel coordinate.
  2. Sample `MotionTileBuffer` to check if tile is static or dynamic.
  3. If static (value 0): Early exit, output `ResolvedColor` unchanged.
  4. If dynamic (value 1): Apply 12-tap reconstruction filter along velocity vector.
    ```glsl
    uint tileIndex = (uint(gl_FragCoord.x) / 16) + (uint(gl_FragCoord.y) / 16) * tilesWide;
    uint isDynamic = motionTileBuffer[tileIndex];
    
    if (isDynamic == 0) {
      imageStore(blurredScene, pixelCoord, texture(resolvedColor, uv));
      return;
    }
    
    vec2 velocity = texture(velocityBuffer, uv).rg / vec2(screenWidth, screenHeight);
    vec3 blurred = vec3(0.0);
    const int samples = 12;
    
    for (int i = 0; i < samples; i++) {
      float t = (float(i) / float(samples - 1)) - 0.5; // Range [-0.5, 0.5]
      vec2 sampleUV = uv + velocity * t;
      blurred += texture(resolvedColor, sampleUV).rgb;
    }
    blurred /= float(samples);
    ```
- **Inputs:** `ResolvedColor`, `VelocityBuffer`, `MotionTileBuffer`
- **Outputs:** **BlurredScene** `B10G11R11_UFLOAT_PACK32`

### 8c. Bloom (Dual Filtering)
- **Pipeline:** Kawase Dual Filter with Soft Knee Threshold.
- **Precision:** `B10G11R11_UFLOAT_PACK32` (Optimized).

  **Step 1: Prefilter (Full-Res → Half-Res)**
  - **Logic:** Apply luminance-based threshold with soft knee to extract bright regions.
    ```glsl
    vec3 color = texture(blurredScene, uv).rgb;
    float luma = dot(color, vec3(0.2126, 0.7152, 0.0722));
    
    // Soft knee curve
    float threshold = bloomThreshold; // e.g., 1.0
    float knee = bloomKnee;           // e.g., 0.5
    float curve = luma - threshold + knee;
    curve = clamp(curve, 0.0, 2.0 * knee);
    curve = curve * curve / (4.0 * knee + 0.00001);
    
    float weight = max(curve, luma - threshold) / max(luma, 0.00001);
    vec3 filtered = color * weight; // Only bright areas pass through
    ```
  - **Output:** `BloomTexture` (1920×1080)

  **Step 2: Downsample Chain**
  - **Pass 1:** 1920×1080 → 960×540 (Mip 1)
  - **Pass 2:** 960×540 → 480×270 (Mip 2)
  - **Pass 3:** 480×270 → 240×135 (Mip 3)
  - **Pass 4:** 240×135 → 120×68 (Mip 4)
  - **Logic per pass:** 5-tap bilinear filter (Kawase pattern).

  **Step 3: Upsample Chain**
  - **Pass 1:** 120×68 → 240×135 (Mip 3)
  - **Pass 2:** 240×135 → 480×270 (Mip 2)
  - **Pass 3:** 480×270 → 960×540 (Mip 1)
  - **Pass 4:** 960×540 → 1920×1080 (Base)
  - **Logic per pass:** 9-tap tent filter + additive blend.

- **Inputs:** `BlurredScene`
- **Outputs:** **BloomTexture** (Half-Res, `B10G11R11_UFLOAT_PACK32`)

---

## Phase 9: Final Output (Graphics)

### 9a. CAS (Contrast Adaptive Sharpening)
- **Logic:**
  1. Upsample `BloomTexture` from half-res to full-res using bilinear filter.
  2. Combine with `BlurredScene`: `combined = blurredScene + bloom * bloomStrength`.
  3. Apply AMD FidelityFX CAS in linear HDR space (recovers TAA softness).
- **Inputs:** `BlurredScene`, `BloomTexture`
- **Outputs:** **SharpenedScene** `R11G11B10_FLOAT`

### 9b. Tonemap & Dither
- **Logic:**
  1. **ACES Tonemapping:** Convert linear HDR to LDR (linear).
    ```glsl
    // ACES Filmic Tone Mapping (Narkowicz 2015)
    vec3 acesTonemap(vec3 x) {
      float a = 2.51;
      float b = 0.03;
      float c = 2.43;
      float d = 0.59;
      float e = 0.14;
      return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
    }
    vec3 tonemapped = acesTonemap(sharpenedScene);
    ```
  2. **Gamma Correction:**
    - Since we use a **UNORM** swapchain, we must apply the sRGB OETF manually.
    ```glsl
    tonemapped = pow(tonemapped, vec3(1.0 / 2.2));
    ```
  3. **Blue Noise Dither:** Add rotated blue noise to prevent banding in gradients.
    ```glsl
    vec2 noiseUV = fract(gl_FragCoord.xy / 128.0); // Tile 128x128 blue noise
    float angle = float(frameIndex % 64) * GOLDEN_RATIO * 2.0 * PI;
    mat2 rotation = mat2(cos(angle), -sin(angle), sin(angle), cos(angle));
    noiseUV = rotation * (noiseUV - 0.5) + 0.5;
    
    float noise = texture(blueNoise, noiseUV).r;
    vec3 dither = vec3(noise - 0.5) / 255.0; // ±1/255 to cover 8-bit quantization gap
    tonemapped += dither;
    ```
- **Inputs:** `SharpenedScene`, `BlueNoise`, `FrameIndex`
- **Outputs:** **SwapchainImage** `B8G8R8A8_UNORM` or `R8G8B8A8_UNORM`.

### 9c. UI Overlay
- **Logic:** Render UI elements (HUD, menus, etc.) directly to Swapchain in sRGB space.
- **State:** Depth Test = OFF, Blend = `SRC_ALPHA, ONE_MINUS_SRC_ALPHA`.
- **Outputs:** **SwapchainImage** (final composite)

**End of Frame Swap:**
```cpp
// Prepare resources for next frame
swap(hizBuffer, prevHizBuffer);          // Hi-Z for next frame's occlusion culling
swap(resolvedColor, historyColor);       // TSSAA output becomes history input
copyImage(depthBuffer, prevDepthBuffer); // Current depth becomes previous depth for TSSAA
```

---

## Memory Budget Summary (4K, Optimized)

### Static Resources (Level Load)
- **IBL Maps (Irradiance + Prefilter + BRDF):** ~1.5MB
- **Blue Noise Texture:** <0.1MB
- **Subtotal:** ~1.5MB

### Current Frame Resources
- **Depth Buffer (D32):** ~32MB
- **Hi-Z Buffer (R32 + Mips):** ~43MB
- **G-Buffers (RT0, RT1, RT2):** ~80MB
- **Velocity Buffer (R16G16_SFLOAT):** ~32MB
- **Shadow Atlas (D32, 4096×4096):** ~67MB
- **Composite Scene (R16G16B16A16):** ~64MB
- **Resolved Color (R16G16B16A16):** ~64MB
- **Subtotal:** ~382MB

### Screen-Space Effects (Optimized)
- **AO Texture (R8_UNORM):** ~2MB
- **SSR Texture (R16G16B16A16):** ~16MB
- **SSR Upsampled (R16G16B16A16):** ~64MB
- **Subtotal:** ~82MB

### Post-Processing Chain (Optimized)
- **Motion Tile Buffer:** <1MB
- **Blurred Scene (Packed32):** ~32MB
- **Bloom Texture + Mip Chain (Packed32):** ~18MB
- **Bloom Temp Buffer (Packed32):** ~8MB
- **Sharpened Scene:** ~32MB
- **Subtotal:** ~91MB

### Previous Frame Persistent Resources
- **PrevHiZ (R32 + Mips):** ~43MB
- **PrevDepth (R32 Copy):** ~32MB
- **TSSAA History Buffer (R16G16B16A16):** ~64MB
- **Subtotal:** ~139MB

### GPU-Only Visibility & Lighting
- **Visibility & Indirect Buffers:** ~1.2MB
- **Light Grid (Clustered):** ~5MB
- **Subtotal:** ~6.2MB

### Swapchain
- **Swapchain Image (B8G8R8A8_UNORM):** ~32MB

**Total VRAM Usage:** ~734MB (Render Targets + Persistent Resources)
**Available on GTX 1080 Ti:** 11GB VRAM
**Headroom:** ~10.2GB remaining for textures, meshes, and other game assets.
**Bandwidth Utilization:** Optimized by using packed formats where Alpha is not needed (Bloom, Blur).
**Performance Impact:** Half-res compute passes (AO, SSR, Bloom) reduce pixel shading load by 75% for those effects.

---

## Additional Notes

### Performance Profiling Strategy
- **Single Queue Execution:** All passes run sequentially on graphics queue for deterministic profiling.
- **GPU Timestamps:** Insert timestamp queries before/after each phase to identify bottlenecks.
- **Expected Bottlenecks:**
  - Phase 5a (Main Forward Pass): Pixel shading with clustered lighting.
  - Phase 6a (SSR Raymarching): Hierarchical ray marching compute cost.
  - Phase 8c (Bloom): Multiple full-screen passes, though mitigated by half-res operation.

### Scaling for Lower-End Hardware
- **Dynamic Resolution:** Render at lower internal resolution (e.g., 1440p) and upscale to 4K.
- **Reduce Cascade Count:** Use 3 CSM cascades instead of 4.
- **Simplify SSR:** Reduce max ray steps or skip SSR entirely on rough surfaces earlier (e.g., roughness > 0.5).
- **Lower TAA Sample Count:** Use 4-frame Halton sequence instead of 8.

### Future Optimizations
- **Async Compute:** Overlap shadow rendering (Phase 3) with previous frame's post-processing.
- **Variable Rate Shading (VRS):** Reduce shading rate in low-detail areas (requires VK_KHR_fragment_shading_rate).
- **Mesh Shaders:** Replace traditional vertex/index buffers with mesh shader pipeline for better culling (requires VK_EXT_mesh_shader).

---

## Pipeline Validation Checklist

- [x] Depth Pre-Pass uses identical jittered ViewProj as Main Pass
- [x] Velocity calculation uses non-jittered matrices for clean motion vectors
- [x] Main Pass depth test is `EQUAL` to utilize Pre-Pass optimization in Reverse-Z
- [x] Skybox depth test is `EQUAL` (to 0.0) or `LESS_EQUAL` relative to Far Plane
- [x] TSSAA reprojects world positions for accurate depth rejection
- [x] Bloom prefilter removes dark areas instead of dimming them
- [x] Motion blur uses parallel reduction to flag tiles correctly
- [x] Swapchain format queries for device compatibility (B8G8R8A8_UNORM) and includes Manual Gamma
- [x] CSM split distances documented as runtime-calculated
- [x] Velocity buffer clamping prevents overflow at 4K resolution
- [x] Hi-Z usage documented for both current frame SSR and next frame culling
- [x] Blue noise dither amount corrected to ±1/255 for smooth gradients
- [x] SSR Texture format updated to RGBA16F to store confidence value
-
