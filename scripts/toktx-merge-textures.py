#!/usr/bin/env python3
import os
import sys
import glob
import subprocess
import numpy as np
import OpenEXR
from PIL import Image


def load_image(filepath):
    """Load an image file, handling .exr (HDR float → 8-bit) and regular formats."""
    if filepath.lower().endswith(".exr"):
        f = OpenEXR.File(filepath)
        channels = f.channels()
        # OpenEXR 3.x returns a dict; get the first (usually 'RGB' or 'RGBA')
        key = list(channels.keys())[0]
        pixels = channels[key].pixels  # float32 array, shape (H, W, C)

        # Clamp to [0, 1] and convert to 8-bit
        pixels = np.clip(pixels, 0.0, 1.0)
        pixels = (pixels * 255.0 + 0.5).astype(np.uint8)

        if pixels.shape[2] == 1:
            return Image.fromarray(pixels[:, :, 0], mode="L")
        elif pixels.shape[2] == 3:
            return Image.fromarray(pixels, mode="RGB")
        elif pixels.shape[2] >= 4:
            return Image.fromarray(pixels[:, :, :4], mode="RGBA")
    else:
        return Image.open(filepath)

# =========================================================
# CONFIGURATION — your paths
# =========================================================
os.environ["LD_LIBRARY_PATH"] = "/home/enes/Sdks/ktx-4.4.2/lib/"

TOKTX = "/home/enes/Projects/ales/game-c-lib/build/ktx/linux/Release/toktx"
KTX   = "/home/enes/Projects/ales/game-c-lib/build/ktx/linux/Release/ktx"

# =========================================================
# TEXTURE TYPE DETECTION
# =========================================================
def detect_type(filename):
    n = filename.lower()

    if any(k in n for k in ("color", "basecolor", "diffuse", "albedo", "diff")):
        return "diffuse"
    if any(k in n for k in ("normalgl", "normal", "nor")):
        return "normal"
    if any(k in n for k in ("roughness", "rough", "smoothness")):
        return "roughness"
    if any(k in n for k in ("displacement", "disp", "height")):
        return "displacement"
    if any(k in n for k in ("ao", "ambientocclusion", "occlusion")):
        return "ao"

    return None


# =========================================================
# PACK MODE — build albedo.png + normal.png + convert to .ktx2
# =========================================================
def process_directory(path):
    print(f"\n==============================")
    print(f" PACKING: {path}")
    print(f"==============================")

    os.chdir(path)

    # ----------------------------------------
    # Remove old albedo/normal (if present)
    # ----------------------------------------
    for f in ("albedo.png", "normal.png"):
        if os.path.exists(f):
            print(f"🗑 Removing old {f}")
            os.remove(f)

    # ----------------------------------------
    # Find relevant textures
    # ----------------------------------------
    textures = {}
    for f in os.listdir("."):
        if f.lower().endswith((".png", ".jpg", ".jpeg", ".exr")):
            t = detect_type(f)
            if t:
                textures[t] = f

    print("Found:", textures)

    if "diffuse" not in textures or "normal" not in textures:
        print("⚠ Missing diffuse or normal — skipping this folder.")
        return

    # ----------------------------------------
    # Load helpers
    # ----------------------------------------
    diffuse_img = load_image(textures["diffuse"]).convert("RGB")
    normal_img  = load_image(textures["normal"]).convert("RGB")

    # default fallback values
    def load_gray(key, fallback):
        if key in textures:
            return load_image(textures[key]).convert("L")
        print(f"⚠ No {key} map found. Using constant {fallback}.")
        w, h = diffuse_img.size
        return Image.new("L", (w, h), fallback)

    rough_img = load_gray("roughness", 255)
    ao_img    = load_gray("ao", 255)
    disp_img  = load_gray("displacement", 128)

    # Resize all maps to diffuse resolution
    w, h = diffuse_img.size
    normal_img = normal_img.resize((w, h), Image.BILINEAR)
    rough_img  = rough_img.resize((w, h), Image.BILINEAR)
    ao_img     = ao_img.resize((w, h), Image.BILINEAR)
    disp_img   = disp_img.resize((w, h), Image.BILINEAR)

    # ----------------------------------------
    # ALBEDO.png = diffuse.rgb + roughness
    # ----------------------------------------
    r, g, b = diffuse_img.split()
    albedo = Image.merge("RGBA", (r, g, b, rough_img))
    albedo.save("albedo.png")
    print("✔ Saved albedo.png")

    # ----------------------------------------
    # NORMAL.png = normal.x, normal.y, AO, displacement
    # ----------------------------------------
    nx, ny, nz = normal_img.split()
    packed_normal = Image.merge("RGBA", (nx, ny, ao_img, disp_img))
    packed_normal.save("normal.png")
    print("✔ Saved normal.png")

    # ----------------------------------------
    # Convert to KTX2
    # ----------------------------------------
    def convert_ktx(src, dst, oetf, primaries):
        if oetf == "srgb":
            cmd = [
                TOKTX,
                "--genmipmap",
                "--2d",
                "--assign_oetf", oetf,
                "--assign_primaries", primaries,
                "--target_type", "RGBA",
                # "--resize", "1024x1024",
                "--encode", "uastc",
                # "--uastc_quality", "0",
                # "--uastc_rdo_l", "3.25",
                # "--uastc_rdo_d", "2048",
                "--zcmp", "19",
                dst,
                src
            ]
        else:
            cmd = [
                TOKTX,
                "--genmipmap",
                "--2d",
                "--assign_oetf", oetf,
                "--assign_primaries", primaries,
                "--target_type", "RGBA",
                # "--resize", "1024x1024",
                "--encode", "uastc",
                # "--uastc_quality", "0",
                # "--uastc_rdo_l", "3.25",
                # "--uastc_rdo_d", "2048",
                "--zcmp", "19",
                dst,
                src
            ]

                # "--encode", "etc1s",
                # "--resize", "2048x2048",
                # "--clevel", "5",
                # "--qlevel", "255",

            
        print("Running:", " ".join(cmd))
        subprocess.run(cmd, check=True)

    convert_ktx("albedo.png", "albedo.ktx2", "srgb", "srgb")
    convert_ktx("normal.png", "normal.ktx2", "linear", "none")

    cmd = [
        "ls",
        "-lh",
        path
    ]
    subprocess.run(cmd, check=True)


# =========================================================
# CLEAN MODE — delete JPG/PNG/JSON (except .ktx2)
# =========================================================
def cleanup_directory(path):
    print(f"\n=== CLEANING: {path} ===")
    os.chdir(path)

    removed = 0

    for f in os.listdir("."):
        lf = f.lower()

        # Delete everything except .ktx2
        if lf.endswith((".jpg", ".jpeg", ".png", ".json", ".exr")) and not lf.endswith(".ktx2") and lf.find("mask") == -1:
            print("🗑 Removing", f)
            os.remove(f)
            removed += 1

    if removed == 0:
        print("Nothing to clean.")
    else:
        print(f"✔ Removed {removed} files.")

    print("✔ Done:", path)
    
    cmd = [
        "ls",
        "-lh",
        path
    ]
    subprocess.run(cmd, check=True)

    cmd = [
        "du",
        "-sh",
        path
    ]
    subprocess.run(cmd, check=True)


# =========================================================
# DIRECTORY SCAN
# =========================================================
def find_texture_folders(root):
    folders = []
    for dirpath, dirnames, filenames in os.walk(root):
        if any(f.lower().endswith((".png", ".jpg", ".jpeg", ".exr")) for f in filenames):
            folders.append(dirpath)
    return folders


# =========================================================
# MAIN ENTRY
# =========================================================
CLEAN_MODE = len(sys.argv) > 1 and sys.argv[1].lower() == "clean"

if __name__ == "__main__":
    root = os.getcwd()
    folders = sorted(find_texture_folders(root))

    if CLEAN_MODE:
        print("\n🧹 CLEAN MODE ACTIVE — deleting PNG/JPG/JSON\n")
        for folder in folders:
            cleanup_directory(folder)
        print("\n✨ Cleanup complete!")
    else:
        print("\n🎨 PACK MODE ACTIVE — generating KTX2\n")
        for folder in folders:
            process_directory(folder)
        print("\n🎉 All folders processed!")
        print("\n\n\n🧹 CLEAN MODE ACTIVE — deleting PNG/JPG/JSON\n")
        for folder in folders:
            cleanup_directory(folder)
        print("\n✨ Cleanup complete!")
 