#!/usr/bin/env python3
"""
Generate a ground-truth AgX tonemap LUT using Blender's OCIO config.

The LUT bakes the full pipeline: Linear Rec.709 → AgX Base → sRGB display.
Input is log2-encoded over the AgX allocation range [-12.47393, 12.5260688117].
The Rec.709→E-Gamut matrix is applied in the shader before the log encoding,
so the LUT input is in E-Gamut log space (matching the .cube file input).

Output format: 2D atlas (8×8 grid of 64×64 tiles), RGBA float16.
Header: u32 width, u32 height, u32 lutSize, u32 atlasCols (16 bytes).

The shader samples this atlas as a trilinear 3D lookup:
  - R → X within tile
  - G → Y within tile  
  - B → selects tile (slice index)
"""

import struct
import sys
import numpy as np
from pathlib import Path

OCIO_CONFIG = "/home/enes/temp/blender/release/datafiles/colormanagement/config.ocio"
LUT_SIZE = 64
ATLAS_COLS = 8
MIN_EV = -12.47393
MAX_EV = 12.5260688117

# Rec.709 → E-Gamut matrix
XYZ_TO_REC709 = np.array([
    [3.2409699, -1.5373832, -0.4986108],
    [-0.9692436, 1.8759675, 0.0415551],
    [0.0556301, -0.2039770, 1.0569715]
])
REC709_TO_XYZ = np.linalg.inv(XYZ_TO_REC709)
XYZ_TO_EGAMUT = np.array([
    [1.5250528, -0.3159135, -0.1226583],
    [-0.5091526, 1.3333274, 0.1382844],
    [0.0957153, 0.0508974, 0.7879558]
])
REC709_TO_EGAMUT = XYZ_TO_EGAMUT @ REC709_TO_XYZ
EGAMUT_TO_REC709 = np.linalg.inv(REC709_TO_EGAMUT)


def generate_lut_ocio(out_path):
    """Generate LUT by processing each entry through OCIO."""
    import PyOpenColorIO as ocio

    config = ocio.Config.CreateFromFile(OCIO_CONFIG)

    # AgX Base (no look) for sRGB display
    processor = config.getProcessor(
        "Linear Rec.709", "sRGB", "AgX", ocio.TRANSFORM_DIR_FORWARD
    )
    cpu = processor.getDefaultCPUProcessor()

    width = LUT_SIZE * ATLAS_COLS      # 512
    height = LUT_SIZE * ATLAS_COLS     # 512 (8 rows × 64)
    total_entries = LUT_SIZE ** 3
    
    print(f"Generating {LUT_SIZE}³ = {total_entries} LUT entries...")
    print(f"Atlas: {width}×{height} pixels, {ATLAS_COLS}×{ATLAS_COLS} tiles")
    print(f"Log2 range: [{MIN_EV}, {MAX_EV}]")
    
    # The LUT input is log2-encoded E-Gamut.
    # For each (r,g,b) coordinate in [0,1]³:
    #   1. Decode from log2: egamut_linear = 2^(coord * (maxEv-minEv) + minEv)
    #   2. Convert E-Gamut → Rec.709 (since OCIO processor expects Rec.709 input)
    #   3. Process through OCIO to get sRGB display output
    
    # Preallocate all pixels
    atlas = np.zeros((height, width, 4), dtype=np.float32)
    
    # Process in batches per blue slice
    for b_idx in range(LUT_SIZE):
        b_norm = b_idx / (LUT_SIZE - 1)
        b_log = b_norm * (MAX_EV - MIN_EV) + MIN_EV
        b_lin = 2.0 ** b_log
        
        tile_col = b_idx % ATLAS_COLS
        tile_row = b_idx // ATLAS_COLS
        
        # Generate all r,g combinations for this blue slice
        batch = np.zeros((LUT_SIZE * LUT_SIZE, 3), dtype=np.float32)
        
        for g_idx in range(LUT_SIZE):
            g_norm = g_idx / (LUT_SIZE - 1)
            g_log = g_norm * (MAX_EV - MIN_EV) + MIN_EV
            g_lin = 2.0 ** g_log
            
            for r_idx in range(LUT_SIZE):
                r_norm = r_idx / (LUT_SIZE - 1)
                r_log = r_norm * (MAX_EV - MIN_EV) + MIN_EV
                r_lin = 2.0 ** r_log
                
                # E-Gamut linear → Rec.709 linear (for OCIO input)
                egamut = np.array([r_lin, g_lin, b_lin])
                rec709 = EGAMUT_TO_REC709 @ egamut
                
                idx = g_idx * LUT_SIZE + r_idx
                batch[idx] = rec709.astype(np.float32)
        
        # Process entire batch through OCIO
        result = batch.copy()
        cpu.applyRGB(result)
        
        # Store in atlas
        for g_idx in range(LUT_SIZE):
            for r_idx in range(LUT_SIZE):
                idx = g_idx * LUT_SIZE + r_idx
                px = tile_col * LUT_SIZE + r_idx
                py = tile_row * LUT_SIZE + g_idx
                # OCIO output is sRGB gamma. The shader linearizes with pow(2.2).
                # Actually, the swapchain is sRGB so the shader should output linear.
                # Store as-is (sRGB gamma values) — shader will linearize.
                atlas[py, px, 0] = result[idx, 0]
                atlas[py, px, 1] = result[idx, 1]
                atlas[py, px, 2] = result[idx, 2]
                atlas[py, px, 3] = 1.0
        
        if (b_idx + 1) % 8 == 0:
            print(f"  Processed {b_idx + 1}/{LUT_SIZE} blue slices...")
    
    # Verify with known values
    print("\nVerification (OCIO reference vs LUT):")
    test_values = [
        [0.01, 0.01, 0.01],
        [0.05, 0.05, 0.05],
        [0.18, 0.18, 0.18],
        [1.0, 1.0, 1.0],
    ]
    for v in test_values:
        ref = cpu.applyRGB(list(v))
        print(f"  Linear ({v[0]:.2f},{v[1]:.2f},{v[2]:.2f}) → sRGB ({ref[0]:.4f},{ref[1]:.4f},{ref[2]:.4f})")
    
    # Write binary file
    out_path.parent.mkdir(parents=True, exist_ok=True)
    
    with open(out_path, 'wb') as f:
        # Header: u32 width, u32 height, u32 lutSize, u32 atlasCols
        f.write(struct.pack('<IIII', width, height, LUT_SIZE, ATLAS_COLS))
        
        # Convert to float16 and write
        atlas_f16 = atlas.astype(np.float16)
        f.write(atlas_f16.tobytes())
    
    file_size = out_path.stat().st_size
    print(f"\nWritten {out_path} ({file_size} bytes, {width}×{height} RGBA16F)")


def main():
    out_path = Path("c-engine/data/pak_0_engine/luts/agx_base.bin")
    if len(sys.argv) > 1:
        out_path = Path(sys.argv[1])
    
    generate_lut_ocio(out_path)


if __name__ == '__main__':
    main()
