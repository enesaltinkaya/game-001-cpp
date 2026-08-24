#!/usr/bin/env python3
"""
Convert Blender's AgX_Base_sRGB.cube (57³) → a 2D texture atlas (64³ resampled)
stored as raw half-float RGBA for loading in the engine.

Layout: width = LUT_SIZE * LUT_SIZE, height = LUT_SIZE
        Each horizontal strip of LUT_SIZE pixels is one blue-slice.
        
Output is kept in gamma space (as the LUT produces it) — the shader
linearizes after sampling.

Also prints the Rec.709 → E-Gamut combined matrix for the shader.
"""

import struct
import sys
import numpy as np
from pathlib import Path

CUBE_PATH = Path("/home/enes/temp/blender/release/datafiles/colormanagement/luts/AgX_Base_sRGB.cube")
OUT_PATH = Path("c-engine/data/pak_0_engine/images/agx_lut.bin")
TARGET_SIZE = 64  # resample to 64³


def parse_cube(path):
    """Parse a .cube LUT file, return (size, data[size³][3])."""
    lines = path.read_text().splitlines()
    size = None
    data = []
    domain_min = [0.0, 0.0, 0.0]
    domain_max = [1.0, 1.0, 1.0]
    
    for line in lines:
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('TITLE'):
            continue
        if line.startswith('LUT_3D_SIZE'):
            size = int(line.split()[-1])
            continue
        if line.startswith('DOMAIN_MIN'):
            domain_min = [float(x) for x in line.split()[1:4]]
            continue
        if line.startswith('DOMAIN_MAX'):
            domain_max = [float(x) for x in line.split()[1:4]]
            continue
        parts = line.split()
        if len(parts) >= 3:
            try:
                r, g, b = float(parts[0]), float(parts[1]), float(parts[2])
                data.append([r, g, b])
            except ValueError:
                continue
    
    assert size is not None, "No LUT_3D_SIZE found"
    assert len(data) == size * size * size, f"Expected {size**3} entries, got {len(data)}"
    
    # Reshape to [B][G][R][3] — .cube format iterates R fastest, then G, then B
    arr = np.array(data, dtype=np.float32).reshape(size, size, size, 3)
    return size, arr


def trilinear_sample(lut, size, r, g, b):
    """Sample the LUT with trilinear interpolation. r,g,b in [0, size-1]."""
    r = np.clip(r, 0, size - 1)
    g = np.clip(g, 0, size - 1)
    b = np.clip(b, 0, size - 1)
    
    r0 = np.floor(r).astype(int)
    g0 = np.floor(g).astype(int)
    b0 = np.floor(b).astype(int)
    
    r1 = np.minimum(r0 + 1, size - 1)
    g1 = np.minimum(g0 + 1, size - 1)
    b1 = np.minimum(b0 + 1, size - 1)
    
    fr = (r - r0).reshape(-1, 1)
    fg = (g - g0).reshape(-1, 1)
    fb = (b - b0).reshape(-1, 1)
    
    # 8 corners
    c000 = lut[b0, g0, r0]
    c100 = lut[b0, g0, r1]
    c010 = lut[b0, g1, r0]
    c110 = lut[b0, g1, r1]
    c001 = lut[b1, g0, r0]
    c101 = lut[b1, g0, r1]
    c011 = lut[b1, g1, r0]
    c111 = lut[b1, g1, r1]
    
    # Trilinear
    c00 = c000 * (1 - fr) + c100 * fr
    c01 = c001 * (1 - fr) + c101 * fr
    c10 = c010 * (1 - fr) + c110 * fr
    c11 = c011 * (1 - fr) + c111 * fr
    
    c0 = c00 * (1 - fg) + c10 * fg
    c1 = c01 * (1 - fg) + c11 * fg
    
    return c0 * (1 - fb) + c1 * fb


def resample_lut(src_lut, src_size, dst_size):
    """Resample src_lut from src_size³ to dst_size³."""
    # Generate target coordinates
    coords = np.arange(dst_size, dtype=np.float32)
    scale = (src_size - 1.0) / (dst_size - 1.0)
    
    dst = np.zeros((dst_size, dst_size, dst_size, 3), dtype=np.float32)
    
    for bz in range(dst_size):
        b_src = bz * scale
        for gy in range(dst_size):
            g_src = gy * scale
            r_src = coords * scale
            
            r_arr = r_src
            g_arr = np.full_like(r_arr, g_src)
            b_arr = np.full_like(r_arr, b_src)
            
            dst[bz, gy, :, :] = trilinear_sample(src_lut, src_size, r_arr, g_arr, b_arr)
    
    return dst


def float_to_half(f):
    """Convert a float32 to float16 bytes (IEEE 754 half)."""
    return struct.pack('<e', f)


def compute_rec709_to_egamut():
    """Compute combined Rec.709 → E-Gamut matrix."""
    # XYZ D65 → Rec.709 (from OCIO config)
    xyz_to_rec709 = np.array([
        [3.2409699, -1.5373832, -0.4986108],
        [-0.9692436, 1.8759675, 0.0415551],
        [0.0556301, -0.2039770, 1.0569715]
    ])
    
    # Rec.709 → XYZ D65 (inverse)
    rec709_to_xyz = np.linalg.inv(xyz_to_rec709)
    
    # XYZ D65 → E-Gamut (from OCIO config)
    xyz_to_egamut = np.array([
        [1.5250528, -0.3159135, -0.1226583],
        [-0.5091526, 1.3333274, 0.1382844],
        [0.0957153, 0.0508974, 0.7879558]
    ])
    
    # Combined: Rec.709 → E-Gamut
    combined = xyz_to_egamut @ rec709_to_xyz
    return combined


def main():
    print("Parsing .cube file...")
    src_size, src_lut = parse_cube(CUBE_PATH)
    print(f"  Source LUT size: {src_size}³ ({src_size**3} entries)")
    
    print(f"Resampling {src_size}³ → {TARGET_SIZE}³...")
    dst_lut = resample_lut(src_lut, src_size, TARGET_SIZE)
    print(f"  Done. Output shape: {dst_lut.shape}")
    
    # Layout as 2D: width = SIZE*SIZE, height = SIZE
    # Blue slices laid out horizontally
    width = TARGET_SIZE * TARGET_SIZE
    height = TARGET_SIZE
    
    # Write as raw binary: 8-byte header (width u16, height u16, size u16, pad u16)
    # then width*height RGBA half-float pixels
    OUT_PATH.parent.mkdir(parents=True, exist_ok=True)
    
    with open(OUT_PATH, 'wb') as f:
        # Header
        f.write(struct.pack('<HHH', width, height, TARGET_SIZE))
        f.write(struct.pack('<H', 0))  # padding
        
        # Pixels: row by row (y=0 at top)
        for y in range(height):  # y = green index
            for bslice in range(TARGET_SIZE):  # blue slice
                for x in range(TARGET_SIZE):  # x = red index
                    r, g, b = dst_lut[bslice, y, x]
                    # Write RGBA16F (alpha = 1.0)
                    f.write(float_to_half(r))
                    f.write(float_to_half(g))
                    f.write(float_to_half(b))
                    f.write(float_to_half(1.0))
    
    file_size = OUT_PATH.stat().st_size
    print(f"Written {OUT_PATH} ({file_size} bytes, {width}x{height} RGBA16F)")
    
    # Print the shader matrix
    mat = compute_rec709_to_egamut()
    print("\nRec.709 → E-Gamut matrix (column-major for GLSL mat3):")
    print(f"const mat3 rec709ToEGamut = mat3(")
    for col in range(3):
        vals = [f"{mat[row][col]:.10f}" for row in range(3)]
        comma = "," if col < 2 else ""
        print(f"    {', '.join(vals)}{comma}")
    print(");")
    
    print("\nRec.709 → E-Gamut matrix (row-major for reference):")
    for row in range(3):
        vals = [f"{mat[row][col]:12.8f}" for col in range(3)]
        print(f"  [{', '.join(vals)}]")
    
    # Verify with a known value
    print(f"\nLog2 range: minEv = -12.47393, maxEv = 12.5260688117")
    print(f"Total stops: {12.5260688117 - (-12.47393):.2f}")


if __name__ == '__main__':
    main()
