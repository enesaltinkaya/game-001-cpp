#define PI 3.14159265359

// Interleaved Gradient Noise (Jimenez 2014).  Returns a value in [0,1)
// that is well-distributed spatially and changes every frame when
// 'frameIndex' is varied.  Used for stochastic alpha testing so that
// TAA / FSR can resolve the dither temporally.
float interleavedGradientNoise(vec2 fragCoord, uint frameIndex) {
    // Rotate the screen-space pattern each frame so TAA sees a
    // different noise sample at every jitter offset.
    fragCoord += float(frameIndex % 64u) * 5.588238;
    return fract(52.9829189 * fract(0.06711056 * fragCoord.x +
                                     0.00583715 * fragCoord.y));
}

// Stochastic alpha test for alpha-cutout materials.
//
// When frameIndex == 0 (temporally stable mode), uses a plain alpha test
// (alpha < cutoff).  The previous stochastic approach seeded IGN noise from
// gl_FragCoord.xy (screen space).  That made the pattern temporally stable
// at a fixed camera position, but when the camera moved the noise pattern
// shifted relative to the geometry, causing alpha-cutout edges to shimmer.
// A plain test makes the edge move smoothly and monotonically with camera
// movement, which FSR/TAA can resolve via jitter + reactive masking.
//
// When frameIndex > 0, the full stochastic dither is used so that TAA/FSR
// can resolve sub-pixel alpha edges temporally.
#ifdef GL_FRAGMENT_SHADER
bool stochasticAlphaDiscard(float alpha, float cutoff, vec2 fragCoord, uint frameIndex) {
    // Temporally stable mode: plain alpha test for maximum stability.
    // FSR's reactive mask handles edge anti-aliasing.
    if (frameIndex == 0u) {
        return alpha < cutoff;
    }

    // Animated dither mode: fwidth-sharpened edge with small temporal
    // threshold variation for TAA to resolve over multiple frames.
    float fw = fwidth(alpha);
    float effective_fw = min(fw, 0.1);
    float sharpened = (alpha - cutoff) / max(effective_fw, 0.0001) * 0.5 + 0.5;

    float noise = interleavedGradientNoise(fragCoord, frameIndex);
    float threshold = 0.5 + (noise - 0.5) * 0.15;
    return sharpened < threshold;
}
#endif


// Simplex 2D noise
//
vec3 permute(vec3 x) { return mod(((x*34.0)+1.0)*x, 289.0); }

float snoise(vec2 v){
  const vec4 C = vec4(0.211324865405187, 0.366025403784439,
           -0.577350269189626, 0.024390243902439);
  vec2 i  = floor(v + dot(v, C.yy) );
  vec2 x0 = v -   i + dot(i, C.xx);
  vec2 i1;
  i1 = (x0.x > x0.y) ? vec2(1.0, 0.0) : vec2(0.0, 1.0);
  vec4 x12 = x0.xyxy + C.xxzz;
  x12.xy -= i1;
  i = mod(i, 289.0);
  vec3 p = permute( permute( i.y + vec3(0.0, i1.y, 1.0 ))
  + i.x + vec3(0.0, i1.x, 1.0 ));
  vec3 m = max(0.5 - vec3(dot(x0,x0), dot(x12.xy,x12.xy),
    dot(x12.zw,x12.zw)), 0.0);
  m = m*m ;
  m = m*m ;
  vec3 x = 2.0 * fract(p * C.www) - 1.0;
  vec3 h = abs(x) - 0.5;
  vec3 ox = floor(x + 0.5);
  vec3 a0 = x - ox;
  m *= 1.79284291400159 - 0.85373472095314 * ( a0*a0 + h*h );
  vec3 g;
  g.x  = a0.x  * x0.x  + h.x  * x0.y;
  g.yz = a0.yz * x12.xz + h.yz * x12.yw;
  return 130.0 * dot(m, g);
}


//	Simplex 3D Noise 
//	by Ian McEwan, Stefan Gustavson (https://github.com/stegu/webgl-noise)
//
vec4 permute(vec4 x){return mod(((x*34.0)+1.0)*x, 289.0);}
vec4 taylorInvSqrt(vec4 r){return 1.79284291400159 - 0.85373472095314 * r;}

float snoise(vec3 v){ 
  const vec2  C = vec2(1.0/6.0, 1.0/3.0) ;
  const vec4  D = vec4(0.0, 0.5, 1.0, 2.0);

// First corner
  vec3 i  = floor(v + dot(v, C.yyy) );
  vec3 x0 =   v - i + dot(i, C.xxx) ;

// Other corners
  vec3 g = step(x0.yzx, x0.xyz);
  vec3 l = 1.0 - g;
  vec3 i1 = min( g.xyz, l.zxy );
  vec3 i2 = max( g.xyz, l.zxy );

  //  x0 = x0 - 0. + 0.0 * C 
  vec3 x1 = x0 - i1 + 1.0 * C.xxx;
  vec3 x2 = x0 - i2 + 2.0 * C.xxx;
  vec3 x3 = x0 - 1. + 3.0 * C.xxx;

// Permutations
  i = mod(i, 289.0 ); 
  vec4 p = permute( permute( permute( 
             i.z + vec4(0.0, i1.z, i2.z, 1.0 ))
           + i.y + vec4(0.0, i1.y, i2.y, 1.0 )) 
           + i.x + vec4(0.0, i1.x, i2.x, 1.0 ));

// Gradients
// ( N*N points uniformly over a square, mapped onto an octahedron.)
  float n_ = 1.0/7.0; // N=7
  vec3  ns = n_ * D.wyz - D.xzx;

  vec4 j = p - 49.0 * floor(p * ns.z *ns.z);  //  mod(p,N*N)

  vec4 x_ = floor(j * ns.z);
  vec4 y_ = floor(j - 7.0 * x_ );    // mod(j,N)

  vec4 x = x_ *ns.x + ns.yyyy;
  vec4 y = y_ *ns.x + ns.yyyy;
  vec4 h = 1.0 - abs(x) - abs(y);

  vec4 b0 = vec4( x.xy, y.xy );
  vec4 b1 = vec4( x.zw, y.zw );

  vec4 s0 = floor(b0)*2.0 + 1.0;
  vec4 s1 = floor(b1)*2.0 + 1.0;
  vec4 sh = -step(h, vec4(0.0));

  vec4 a0 = b0.xzyw + s0.xzyw*sh.xxyy ;
  vec4 a1 = b1.xzyw + s1.xzyw*sh.zzww ;

  vec3 p0 = vec3(a0.xy,h.x);
  vec3 p1 = vec3(a0.zw,h.y);
  vec3 p2 = vec3(a1.xy,h.z);
  vec3 p3 = vec3(a1.zw,h.w);

//Normalise gradients
  vec4 norm = taylorInvSqrt(vec4(dot(p0,p0), dot(p1,p1), dot(p2, p2), dot(p3,p3)));
  p0 *= norm.x;
  p1 *= norm.y;
  p2 *= norm.z;
  p3 *= norm.w;

// Mix final noise value
  vec4 m = max(0.6 - vec4(dot(x0,x0), dot(x1,x1), dot(x2,x2), dot(x3,x3)), 0.0);
  m = m * m;
  return 42.0 * dot( m*m, vec4( dot(p0,x0), dot(p1,x1), 
                                dot(p2,x2), dot(p3,x3) ) );
}



float gradientNoise(vec2 uv) {
  const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
  return fract(magic.z * fract(dot(uv, magic.xy)));
}

// vec3 colorBandingFix(vec3 x) {
//   return x + (1.0 / 255.0) * gradientNoise(gl_FragCoord.xy) - (0.5 / 255.0);
// }

// Converts a color from linear light gamma to sRGB gamma
vec3 fromLinear(vec3 linearRGB) {
  bvec3 cutoff = lessThan(linearRGB.rgb, vec3(0.0031308));
  vec3 higher  = vec3(1.055) * pow(linearRGB.rgb, vec3(1.0 / 2.4)) - vec3(0.055);
  vec3 lower   = linearRGB.rgb * vec3(12.92);

  return mix(higher, lower, cutoff);
}

// Converts a color from sRGB gamma to linear light gamma
vec3 toLinear(vec3 sRGB) {
  bvec3 cutoff = lessThan(sRGB.rgb, vec3(0.04045));
  vec3 higher  = pow((sRGB.rgb + vec3(0.055)) / vec3(1.055), vec3(2.4));
  vec3 lower   = sRGB.rgb / vec3(12.92);

  return mix(higher, lower, cutoff);
}

vec3 filmic(vec3 x) {
  vec3 X = max(vec3(0.0), x - 0.004);
  vec3 result = (X * (6.2 * X + 0.5)) / (X * (6.2 * X + 1.7) + 0.06);
  return toLinear(result);
}

vec3 reinhard(vec3 x) {
  return x / (x + vec3(1.0));
}

vec3 aces(vec3 x) {
  const float a = 2.51;
  const float b = 0.03;
  const float c = 2.43;
  const float d = 0.59;
  const float e = 0.14;
  return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

// ---------------------------------------------------------------------------
// AgX tonemapper — polynomial approximation of Blender's AgX
// Based on Troy Sobotka / Eary Chow's AgX design.
//
// Polynomials were fitted against Blender's OCIO AgX pipeline
// (Linear Rec.709 → sRGB display) for accurate tonal response.
//
// Pipeline (matching Blender's OCIO config):
//   1. Linear Rec.709 → Linear E-Gamut (inset matrix)
//   2. Log2 encoding over ~25 stops [-12.47, +12.53]
//   3. Per-channel sigmoid (polynomial approximation of AgX .cube LUT)
//   4. E-Gamut → Rec.709 (outset matrix)
//   5. Linearize (sRGB EOTF) for sRGB swapchain output
//
// Note: The ground-truth LUT path (in final.frag) is preferred when available.
//       This polynomial fallback has max error ~0.031 on a gray ramp.
// ---------------------------------------------------------------------------

// AgX Base Contrast sigmoid — approximation of Blender AgX Base for sRGB.
// Fitted against the full Blender OCIO pipeline (gray ramp, E-Gamut log space).
// Max error: 0.031, RMS error: 0.011 (vs Blender OCIO reference, gray ramp)
vec3 agxBaseContrastApprox(vec3 x) {
    vec3 x2 = x * x;
    vec3 x4 = x2 * x2;
    return -98.567275 * x4 * x2 * x
           +274.943780 * x4 * x2
           -245.517317 * x4 * x
           + 48.153724 * x4
           + 29.927309 * x2 * x
           -  8.552191 * x2
           +  0.581124 * x;
}

vec3 agx(vec3 color) {
    // Rec.709 → E-Gamut (matching Blender's OCIO AgX pipeline)
    const mat3 agxInsetMatrix = mat3(
        0.559371133310077, 0.076220705483506, 0.065526711778770,
        0.304783346623009, 0.787971770102846, 0.164546754603724,
        0.135845560480477, 0.135807466174722, 0.769926501650374
    );

    // E-Gamut → Rec.709 (inverse of inset)
    const mat3 agxOutsetMatrix = mat3(
         1.907248185600754, -0.162497857783078, -0.127592967393126,
        -0.692966610411929,  1.376655342180092, -0.235238487079751,
        -0.214281699475122, -0.214157404500967,  1.362831489495140
    );

    const float minEv = -12.47393;      // log2 of minimum scene value
    const float maxEv =  12.5260688117; // log2 of maximum scene value

    // 1. Apply inset matrix (Rec.709 → E-Gamut)
    color = agxInsetMatrix * color;

    // 2. Log2 encode to [0,1] over ~25 stops
    color = clamp(log2(max(color, vec3(1e-10))), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);

    // 3. Apply sigmoid (Base Contrast, matches Blender "AgX" with no look)
    color = agxBaseContrastApprox(color);

    // 4. Apply outset matrix (E-Gamut → Rec.709)
    color = agxOutsetMatrix * color;

    // 5. The polynomial outputs sRGB-gamma values.
    //    Linearize so the sRGB swapchain hardware can re-apply the gamma.
    color = toLinear(max(color, vec3(0.0)));

    return clamp(color, 0.0, 1.0);
}

// AgX Base Contrast — same as agx() (both use Base Contrast).
vec3 agxBase(vec3 color) {
    return agx(color);
}

vec3 uncharted2Tonemap(const vec3 x) {
  const float A = 0.15;
  const float B = 0.50;
  const float C = 0.10;
  const float D = 0.20;
  const float E = 0.02;
  const float F = 0.30;
  return ((x * (A * x + C * B) + D * E) / (x * (A * x + B) + D * F)) - E / F;
}

vec3 tonemapUncharted2(const vec3 color) {
  const float W            = 11.2;
  const float exposureBias = 2.0;
  vec3 curr                = uncharted2Tonemap(exposureBias * color);
  vec3 whiteScale          = 1.0 / uncharted2Tonemap(vec3(W));
  return curr * whiteScale;
}


vec3 uchimura(vec3 x, float P, float a, float m, float l, float c, float b) {
  float l0 = ((P - m) * l) / a;
  float L0 = m - m / a;
  float L1 = m + (1.0 - m) / a;
  float S0 = m + l0;
  float S1 = m + a * l0;
  float C2 = (a * P) / (P - S1);
  float CP = -C2 / P;

  vec3 w0 = vec3(1.0 - smoothstep(0.0, m, x));
  vec3 w2 = vec3(step(m + l0, x));
  vec3 w1 = vec3(1.0 - w0 - w2);

  vec3 T = vec3(m * pow(x / m, vec3(c)) + b);
  vec3 S = vec3(P - (P - S1) * exp(CP * (x - S0)));
  vec3 L = vec3(m + a * (x - m));

  return T * w0 + L * w1 + S * w2;
}

vec3 uchimura(vec3 x) {
  const float P = 1.0;   // max display brightness
  const float a = 1.0;   // contrast
  const float m = 0.22;  // linear section start
  const float l = 0.4;   // linear section length
  const float c = 1.33;  // black
  const float b = 0.0;   // pedestal

  return uchimura(x, P, a, m, l, c, b);
}


vec3 unreal(vec3 x) {
  vec3 result = x / (x + 0.155) * 1.019;
  return toLinear(result);
}


float calculateFogFactor(float fogStartDistance, int fogType, float fogDensity /* 0.01 to 0.1.*/,float dist) {
    dist = max(0.0, dist - fogStartDistance);
    float fogFactor = 0.0;
    if (fogType == 0) { 
        const float fogEndDistance = 2000.0;
        // fogFactor increases linearly from 0 to 1 between fogStartDistance and fogEndDistance
        fogFactor = clamp(dist / (fogEndDistance - fogStartDistance), 0.0, 1.0);
    } else if (fogType == 1) { // Exponential Fog
        // fogFactor = 1.0 - exp(-density * (dist - fogStartDistance))
        // Note: I'm using exp() directly here, as it's generally preferred over exp2(x * LOG2)
        fogFactor = 1.0 - exp(-fogDensity * dist);
    } else if (fogType == 2) { // Exponential Squared Fog (Your original type)
        // fogFactor = 1.0 - exp(-(density * (dist - fogStartDistance))^2)
        float d = fogDensity * dist;
        fogFactor = 1.0 - exp(-(d * d));
    }else if (fogType == 3) { // Exponential Squared Fog (Your original type)
        const float LOG2 = -1.442695;
        float d          = fogDensity * dist;
        fogFactor = 1.0 - clamp(exp2(d * d * LOG2), 0.0, 1.0);
    }
    
    const float fogFadeDistance = 300.0;
    float blendFactor = smoothstep(fogStartDistance,
                                  fogStartDistance + fogFadeDistance,
                                  dist);

    
    return blendFactor * fogFactor;
}


// ----------------------------------------------------------------------------
float DistributionGGX(vec3 N, vec3 H, float roughness) {
    float a = roughness*roughness;
    float a2 = a*a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH*NdotH;
    float nom   = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    return nom / denom;
}

// ----------------------------------------------------------------------------
float GeometrySchlickGGX(float NdotV, float roughness) {
    float r = (roughness + 1.0);
    float k = (r*r) / 8.0;
    float nom   = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    return nom / denom;
}

// ----------------------------------------------------------------------------
float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness) {
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// ----------------------------------------------------------------------------
vec3 fresnelSchlick(float cosTheta, vec3 F0) {
    return F0 + (1.0 - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
    // return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

// ----------------------------------------------------------------------------
vec3 fresnelSchlickRoughness(float cosTheta, vec3 F0, float roughness) {
    return F0 + (max(vec3(1.0 - roughness), F0) - F0) * pow(max(1.0 - cosTheta, 0.0), 5.0);
}

// ----------------------------------------------------------------------------


float hash(vec2 p) {
    p = fract(p * 0.3183099 + vec2(0.1, 0.3));
    p *= 17.0;
    return fract(p.x * p.y * (p.x + p.y));
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);

    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));

    // smooth factor
    vec2 u = f * f * (3.0 - 2.0 * f);

    return mix(a, b, u.x) +
           (c - a) * u.y * (1.0 - u.x) +
           (d - b) * u.x * u.y;
}

float fbm(vec2 p) {
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    for (int i = 0; i < 5; i++) {   // 5 octaves is typical
        value += amplitude * noise(p * frequency);
        frequency *= 2.0;
        amplitude *= 0.5;
    }
    return value;
}

// ---------------------------------------------------------
// VORONOI & MATH HELPERS
// ---------------------------------------------------------

// Hash function
// Precision-safe hash (no sine waves)
vec2 hash2(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(.1031, .1030, .0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.xx+p3.yz)*p3.zy);
}

// Manhattan Voronoi Logic
float getVoronoiRandom(vec2 uv, float scale) {
    vec2 scaledUV = uv * scale;
    vec2 grid_id = floor(scaledUV);
    vec2 grid_uv = fract(scaledUV);

    float minDist = 100.0;
    vec2 targetCellID = vec2(0.0);

    for(int y = -1; y <= 1; y++) {
        for(int x = -1; x <= 1; x++) {
            vec2 neighbor = vec2(float(x), float(y));
            vec2 point = hash2(grid_id + neighbor);
            vec2 diff = neighbor + point - grid_uv;
            float dist = abs(diff.x) + abs(diff.y); // Manhattan Distance

            if(dist < minDist) {
                minDist = dist;
                targetCellID = grid_id + neighbor;
            }
        }
    }
    return hash2(targetCellID).x; 
}

// UV Rotation
vec2 rotateUV(vec2 uv, float angle) {
    float s = sin(angle);
    float c = cos(angle);
    mat2 rot = mat2(c, -s, s, c);
    return rot * uv;
}


// Returns +/- 1
float signNotZero(float v) {
    return (v >= 0.0) ? +1.0 : -1.0;
}

// Compress a Unit Vector (Normal) into vec2 [-1..1]
vec2 OctEncode(vec3 n) {
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    return (n.z < 0.0) ? (vec2(1.0) - abs(p.yx)) * vec2(signNotZero(p.x), signNotZero(p.y)) : p;
}

// Uncompress vec2 [-1..1] back into Unit Vector
vec3 OctDecode(vec2 e) {
    vec3 n = vec3(e.x, e.y, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0) {
        vec2 xy = (vec2(1.0) - abs(n.yx)) * vec2(signNotZero(n.x), signNotZero(n.y));
        n.x = xy.x;
        n.y = xy.y;
    }
    return normalize(n);
}

vec3 applyNormalStrength(vec3 normal, float strength) {
    // Scale X and Y. If strength is 1.0, nothing changes.
    // If strength is > 1.0, slopes get steeper.
    return normalize(vec3(normal.xy * strength, normal.z));
}


/*
----------------------------
TRANSFORMATIONS
----------------------------
*/

vec3 rotateVector(vec3 v, vec4 q) {
    return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);
}

vec3 transformDirection(vec3 dir, vec4 rotation, vec3 scale) {
    vec3 scaled = dir * scale;  // Only if scale is non-uniform!
    return normalize(rotateVector(scaled, rotation));
}

vec3 rotateVertexPosition(vec3 position, vec4 rotation) {
    return position + 2.0 * cross(rotation.xyz, cross(rotation.xyz, position) + rotation.w * position);
}

vec3 transformVertex(vec3 position, vec3 translation, vec4 rotation, vec3 scale) {
    vec3 scaled  = position * scale;
    vec3 rotated = rotateVertexPosition(scaled, rotation);
    return rotated + translation;
}