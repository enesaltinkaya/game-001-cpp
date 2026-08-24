#!/usr/bin/env python3
"""
Convert .png textures to .ktx2 using toktx.

Usage:
    # Scan all pak_*/models under c-game/data (legacy behaviour):
    python3 scripts/convert-model-textures.py

    # Convert a specific input directory into an output directory:
    python3 scripts/convert-model-textures.py --input-dir /path/to/pngs --output-dir /path/to/out

    # Other flags:
    python3 scripts/convert-model-textures.py --dry-run
    python3 scripts/convert-model-textures.py --jobs 4
"""

import os
import sys
import glob
import shutil
import subprocess
from concurrent.futures import ProcessPoolExecutor, as_completed
from multiprocessing import cpu_count

# =========================================================
# CONFIGURATION
# =========================================================
os.environ["LD_LIBRARY_PATH"] = "/home/enes/Sdks/ktx-4.4.2/lib/"
TOKTX = "/home/enes/Projects/ales/game-c-lib/build/ktx/linux/Release/toktx"

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.dirname(SCRIPT_DIR)
DATA_DIR = os.path.join(PROJECT_ROOT, "c-game", "data")


# =========================================================
# HELPERS
# =========================================================
def find_models_dirs(data_dir):
    """Return sorted list of models/ directories inside pak_* folders."""
    pattern = os.path.join(data_dir, "pak_*", "models")
    return sorted(glob.glob(pattern))


def find_png_files(root_dir):
    """Recursively find all .png files under a directory."""
    pngs = []
    for dirpath, _, filenames in os.walk(root_dir):
        for f in filenames:
            if f.lower().endswith(".png"):
                pngs.append(os.path.join(dirpath, f))
    return sorted(pngs)


def is_srgb(filename):
    """Guess whether a texture is sRGB (color) or linear (data) from its name."""
    n = filename.lower()
    linear_keywords = ("normal", "nor_", "roughness", "rough", "metallic",
                       "metal", "ao", "occlusion", "displacement", "disp",
                       "height", "mask", "splat")
    return not any(k in n for k in linear_keywords)


def convert_one(png_path, output_dir=None):
    """Convert a single .png to .ktx2 and remove the original.

    If output_dir is given the .ktx2 is placed there (preserving the
    relative sub-path from input_dir which is encoded in the tuple).
    Otherwise it lands next to the source .png.

    Returns (png_path, success, error_msg).
    """
    os.environ["LD_LIBRARY_PATH"] = "/home/enes/Sdks/ktx-4.4.2/lib/"

    if output_dir:
        ktx2_path = os.path.join(output_dir,
                                 os.path.splitext(os.path.basename(png_path))[0] + ".ktx2")
    else:
        ktx2_path = os.path.splitext(png_path)[0] + ".ktx2"

    srgb = is_srgb(os.path.basename(png_path))
    oetf = "srgb" if srgb else "linear"
    primaries = "srgb" if srgb else "none"

    cmd = [
        TOKTX,
        "--genmipmap",
        "--2d",
        "--assign_oetf", oetf,
        "--assign_primaries", primaries,
        "--encode", "uastc",
        # "--uastc_quality", "0",
        # "--uastc_rdo_l", "3.25",
        # "--uastc_rdo_d", "2048",
        "--zcmp", "22",
        ktx2_path,
        png_path,
    ]

    try:
        subprocess.run(cmd, check=True, capture_output=True, text=True)
    except subprocess.CalledProcessError as e:
        return (png_path, False, e.stderr.strip())

    # Only remove the original when converting in-place (no output_dir)
    if not output_dir:
        os.remove(png_path)
    return (png_path, True, None)


def _convert_worker(args):
    """Wrapper for ProcessPoolExecutor — unpacks the tuple."""
    return convert_one(*args)


def convert_pngs(pngs, output_dir=None, num_workers=None, dry_run=False, label=""):
    """Convert a list of .png paths to .ktx2.

    If output_dir is set, all .ktx2 files are placed there.
    Returns (converted_count, failed_count).
    """
    if num_workers is None:
        num_workers = cpu_count()

    if not pngs:
        if label:
            print(f"  {label}: no .png files found.")
        return 0, 0

    print(f"\n{label}{len(pngs)} .png files to convert, using {num_workers} workers")

    if dry_run:
        for png in pngs:
            oetf = "srgb" if is_srgb(os.path.basename(png)) else "linear"
            rel = os.path.relpath(png, PROJECT_ROOT) if png.startswith(PROJECT_ROOT) else png
            print(f"  [dry-run] {rel} -> .ktx2  ({oetf})")
        print("  (dry-run mode — no files were changed)")
        return 0, 0

    if output_dir:
        os.makedirs(output_dir, exist_ok=True)

    converted = 0
    failed = 0
    total = len(pngs)

    work_items = [(png, output_dir) for png in pngs]

    with ProcessPoolExecutor(max_workers=num_workers) as executor:
        futures = {executor.submit(_convert_worker, item): item[0] for item in work_items}
        for future in as_completed(futures):
            png_path, ok, err = future.result()
            rel = os.path.relpath(png_path, PROJECT_ROOT) if png_path.startswith(PROJECT_ROOT) else png_path
            if ok:
                converted += 1
                print(f"  ✔ [{converted + failed}/{total}] {rel}")
            else:
                failed += 1
                print(f"  ✗ [{converted + failed}/{total}] {rel}")
                print(f"    {err}")

    return converted, failed


# =========================================================
# MAIN
# =========================================================
def main():
    dry_run = "--dry-run" in sys.argv

    # Parse --jobs N
    num_workers = cpu_count()
    if "--jobs" in sys.argv:
        idx = sys.argv.index("--jobs")
        if idx + 1 < len(sys.argv):
            num_workers = int(sys.argv[idx + 1])

    # Parse --input-dir / --output-dir
    input_dir = None
    output_dir = None
    if "--input-dir" in sys.argv:
        idx = sys.argv.index("--input-dir")
        if idx + 1 < len(sys.argv):
            input_dir = sys.argv[idx + 1]
    if "--output-dir" in sys.argv:
        idx = sys.argv.index("--output-dir")
        if idx + 1 < len(sys.argv):
            output_dir = sys.argv[idx + 1]

    # ── Explicit input/output mode ─────────────────────────────────────
    if input_dir:
        if not os.path.isdir(input_dir):
            print(f"Input directory does not exist: {input_dir}", file=sys.stderr)
            sys.exit(1)

        pngs = find_png_files(input_dir)
        converted, failed = convert_pngs(pngs, output_dir=output_dir,
                                         num_workers=num_workers, dry_run=dry_run)
        print(f"\nDone.  total={len(pngs)}  converted={converted}  failed={failed}")
        if failed:
            sys.exit(1)
        return

    # ── Legacy scan mode ───────────────────────────────────────────────
    models_dirs = find_models_dirs(DATA_DIR)
    if not models_dirs:
        print("No pak_*/models directories found under", DATA_DIR)
        return

    print(f"Found {len(models_dirs)} models directory(ies):")
    for d in models_dirs:
        print(f"  {os.path.relpath(d, PROJECT_ROOT)}")

    all_pngs = []
    for models_dir in models_dirs:
        all_pngs.extend(find_png_files(models_dir))

    converted, failed = convert_pngs(all_pngs, num_workers=num_workers, dry_run=dry_run)

    print(f"\n{'='*60}")
    print(f" Done.  total={len(all_pngs)}  converted={converted}  failed={failed}")
    print(f"{'='*60}")

    if failed:
        sys.exit(1)


if __name__ == "__main__":
    main()
