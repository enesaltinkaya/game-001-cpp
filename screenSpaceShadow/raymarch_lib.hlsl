// raymarch_lib.hlsl

// ------------------------------------------------------------------
// 1. HELPERS
// ------------------------------------------------------------------

// Vulkan Clip Space Y is down
#define CLIP_SPACE_UV_Y_DIR -1 

float2 cs_to_uv(float2 cs) { 
    return cs * float2(0.5, CLIP_SPACE_UV_Y_DIR * 0.5) + float2(0.5, 0.5); 
}

float2 uv_to_cs(float2 uv) { 
    return (uv - 0.5.xx) * float2(2, CLIP_SPACE_UV_Y_DIR * 2); 
}

struct ViewParams {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float    near_plane;

    float rcp_near_plane_distance() { 
        return 1.0 / near_plane; 
    }
    
    float3 position_world_to_sample(float3 ws_pos) {
        float4 clip = mul(view_proj, float4(ws_pos, 1.0));
        return clip.xyz / clip.w;
    }

    float3 position_clip_to_world(float3 cs_pos) {
        float4 world = mul(inv_view_proj, float4(cs_pos, 1.0));
        return world.xyz / world.w;
    }
};

// ------------------------------------------------------------------
// 2. CORE LOGIC (Functional Style)
// ------------------------------------------------------------------

// Flattened struct (No inheritance)
struct DistanceResult {
    float distance;
    bool  valid;
    float penetration;
};

// Settings for the distance query
struct RaymarchSettings {
    bool   march_behind_surfaces;
    float  depth_thickness;
    bool   use_sloppy_march;
    float2 depth_tex_size;
};

// Free function to query distance (Takes textures explicitly)
DistanceResult QueryDistance(
    float3 ray_point_cs, 
    RaymarchSettings settings,
    Texture2D<float> depth_tex, 
    SamplerState s_llc, 
    SamplerState s_nnc
) {
    const float2 interp_uv = cs_to_uv(ray_point_cs.xy);
    const float ray_depth = 1.0 / max(1e-6, ray_point_cs.z); 

    const float linear_depth = 1.0 / depth_tex.SampleLevel(s_llc, interp_uv, 0);
    const float unfiltered_depth = 1.0 / depth_tex.SampleLevel(s_nnc, interp_uv, 0);

    float max_depth = settings.use_sloppy_march ? unfiltered_depth : max(linear_depth, unfiltered_depth);
    float min_depth = settings.use_sloppy_march ? unfiltered_depth : min(linear_depth, unfiltered_depth);

    const float bias = 0.000002;
    DistanceResult res;
    res.distance = max_depth * (1.0 + bias) - ray_depth;
    res.penetration = ray_depth - min_depth;

    if (settings.march_behind_surfaces) {
        res.valid = res.penetration < settings.depth_thickness;
    } else {
        res.valid = true;
    }
    return res;
}

// ------------------------------------------------------------------
// 3. ROOT FINDER
// ------------------------------------------------------------------

struct HybridRootFinder {
    uint linear_steps;
    uint bisection_steps;
    float linear_march_exponent;
    float jitter;
    float min_t;
    float max_t;

    // Helper to create (Avoids static methods which can sometimes trip glslc)
    void init(uint steps) {
        linear_steps = steps;
        bisection_steps = 0;
        linear_march_exponent = 1;
        jitter = 1;
        min_t = 0;
        max_t = 1;
    }

    // Main loop: Takes textures as arguments to pass down
    bool find_root(
        float3 start, float3 end, 
        RaymarchSettings settings,
        Texture2D<float> depth_tex, 
        SamplerState s_llc, 
        SamplerState s_nnc,
        inout float hit_t, inout float miss_t, out DistanceResult hit_d
    ) {
        const float3 dir = end - start;
        float l_min_t = this.min_t;
        float l_max_t = this.max_t;
        
        DistanceResult l_min_d; l_min_d.distance=0; l_min_d.valid=false; l_min_d.penetration=0;
        DistanceResult l_max_d; l_max_d.distance=0; l_max_d.valid=false; l_max_d.penetration=0;

        bool intersected = false;

        // Linear March
        if (linear_steps > 0) {
            for (uint step = 0; step < linear_steps; ++step) {
                const float t_param = (step == 0) ? (0.0 + jitter) : (float(step) + jitter);
                const float candidate_t = lerp(l_min_t, l_max_t, pow(t_param / linear_steps, linear_march_exponent));
                
                const float3 candidate = start + dir * candidate_t;
                const DistanceResult candidate_d = QueryDistance(candidate, settings, depth_tex, s_llc, s_nnc);
                
                intersected = candidate_d.distance < 0 && candidate_d.valid;
                if (intersected) {
                    l_max_t = candidate_t;
                    l_max_d = candidate_d;
                    break;
                } else {
                    l_min_t = candidate_t;
                    l_min_d = candidate_d;
                }
            }
        }
        
        miss_t = l_min_t;
        hit_t = l_min_t;

        // Bisection
        if (intersected) {
            for (uint step = 0; step < bisection_steps; ++step) {
                const float mid_t = (l_min_t + l_max_t) * 0.5;
                const float3 candidate = start + dir * mid_t;
                const DistanceResult candidate_d = QueryDistance(candidate, settings, depth_tex, s_llc, s_nnc);
                if (candidate_d.distance < 0 && candidate_d.valid) {
                    l_max_t = mid_t;
                    l_max_d = candidate_d;
                } else {
                    l_min_t = mid_t;
                    l_min_d = candidate_d;
                }
            }
            hit_t = l_max_t;
            hit_d = l_max_d;
            return true;
        } 
        return false;
    }
};

// ------------------------------------------------------------------
// 4. MAIN BUILDER
// ------------------------------------------------------------------

struct DepthRayMarchResult {
    bool hit;
    float hit_t;
    float2 hit_uv;
    float hit_penetration_frac;
};

struct DepthRayMarch {
    uint linear_steps;
    float linear_march_exponent;
    uint bisection_steps;
    float jitter;
    float3 ray_start_cs;
    float3 ray_end_cs;
    bool march_behind_surfaces;
    bool use_sloppy_march;
    float depth_thickness_linear_z;
    float2 depth_tex_size; // Stored as plain data

    // Initializer
    void init(float2 tex_size) {
        depth_tex_size = tex_size;
        linear_steps = 4;
        bisection_steps = 0;
        linear_march_exponent = 1;
        jitter = 1;
        depth_thickness_linear_z = 1;
        march_behind_surfaces = false;
        use_sloppy_march = false;
        ray_start_cs = float3(0,0,0);
        ray_end_cs = float3(0,0,0);
    }

    // Chainable setters need to modify 'this' and return 'this'
    // Since HLSL struct methods modify 'this' implicitly, we return a copy
    DepthRayMarch with_linear_steps(uint v) { DepthRayMarch res = this; res.linear_steps = v; return res; }
    DepthRayMarch with_jitter(float v) { DepthRayMarch res = this; res.jitter = v; return res; }
    DepthRayMarch with_depth_thickness(float v) { DepthRayMarch res = this; res.depth_thickness_linear_z = v; return res; }
    DepthRayMarch with_march_behind_surfaces(bool v) { DepthRayMarch res = this; res.march_behind_surfaces = v; return res; }

    DepthRayMarch from_cs(float3 v) { DepthRayMarch res = this; res.ray_start_cs = v; return res; }

    DepthRayMarch to_ws(ViewParams view, float3 end_ws) {
        float3 end_cs = view.position_world_to_sample(end_ws);
        DepthRayMarch res = this;
        res.ray_end_cs = end_cs;
        return res;
    }

    // The march function now TAKES the resources
    DepthRayMarchResult march(ViewParams view, Texture2D<float> depth_tex, SamplerState s_llc, SamplerState s_nnc) {
        DepthRayMarchResult res;
        res.hit = false; res.hit_t = 0; res.hit_uv = float2(0,0); res.hit_penetration_frac = 0;
        
        const float2 ray_start_uv = cs_to_uv(ray_start_cs.xy);
        const float2 ray_end_uv = cs_to_uv(ray_end_cs.xy);
        const float2 ray_len_px = (ray_end_uv - ray_start_uv) * depth_tex_size;
        
        const int step_count = max(2, min((int)linear_steps, int(floor(length(ray_len_px)))));
        
        const float linear_z_to_scaled_linear_z = view.rcp_near_plane_distance();
        const float depth_thickness = this.depth_thickness_linear_z * linear_z_to_scaled_linear_z;

        // Prepare settings
        RaymarchSettings settings;
        settings.march_behind_surfaces = this.march_behind_surfaces;
        settings.depth_thickness = depth_thickness;
        settings.use_sloppy_march = this.use_sloppy_march;
        settings.depth_tex_size = depth_tex_size;

        DistanceResult hit;
        float miss_t;
        
        // Setup finder
        HybridRootFinder finder;
        finder.init(step_count);
        finder.bisection_steps = this.bisection_steps;
        finder.jitter = this.jitter;

        // Execute
        bool intersected = finder.find_root(
            ray_start_cs, ray_end_cs, 
            settings, 
            depth_tex, s_llc, s_nnc, // Pass resources here
            res.hit_t, miss_t, hit
        );

        if (intersected && hit.penetration < depth_thickness && hit.distance < depth_thickness) {
            res.hit = true;
            res.hit_uv = lerp(ray_start_uv, ray_end_uv, res.hit_t);
            res.hit_penetration_frac = hit.penetration / depth_thickness;
            return res;
        }

        res.hit_t = miss_t;
        res.hit_uv = lerp(ray_start_uv, ray_end_uv, res.hit_t);
        return res;
    }
};

// Helper constructor function
DepthRayMarch MakeDepthRayMarch(float2 tex_size) {
    DepthRayMarch d;
    d.init(tex_size);
    return d;
}