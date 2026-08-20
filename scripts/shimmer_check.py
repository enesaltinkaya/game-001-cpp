#!/usr/bin/env python3
"""Numerically quantify frame-to-frame shimmer in consecutive screenshots.

Usage:
  python3 scripts/shimmer_check.py /tmp/taa_before 4
  python3 scripts/shimmer_check.py /tmp/taa_before 4 --threshold 8 --out /tmp/

Loads /tmp/taa_before_1.jpg .. _N.jpg, and for each consecutive pair
computes:
  * mean_abs   : mean |F(t+1) - F(t)| per channel (0-255 scale)
  * shimmer_pct : fraction of pixels whose max-channel diff exceeds
                   the threshold  (the key "shimmer" metric)
  * max_diff    : worst pixel change
Also writes, for each pair:
  * <out>/<base>_diff_<i>.jpg  : 8x-amplified grayscale diff (where it shimmered)
  * <out>/<base>_mask_<i>.jpg  : binary mask of changed pixels

A stable frame set (after a TAA fix) should show shimmer_pct ~0 and
mean_abs near 0. A shimmering canopy shows a clearly elevated shimmer_pct
concentrated in the leaf region.
"""

import argparse
import sys
from pathlib import Path

import numpy as np
from PIL import Image


def load_rgb(path: str) -> np.ndarray:
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("prefix", help="base path, expects <prefix>_1.jpg .. <prefix>_N.jpg")
    ap.add_argument("count", type=int, help="number of consecutive frames")
    ap.add_argument("--threshold", type=float, default=8.0,
                    help="per-pixel max-channel diff (0-255) above which a pixel counts as changed")
    ap.add_argument("--amplify", type=float, default=8.0,
                    help="amplification factor for the saved diff image")
    ap.add_argument("--out", default="", help="directory for diff/mask images (default: same dir as prefix)")
    args = ap.parse_args()

    base = Path(args.prefix)
    out_dir = Path(args.out) if args.out else base.parent
    out_dir.mkdir(parents=True, exist_ok=True)

    files = [f"{args.prefix}_{i}.jpg" for i in range(1, args.count + 1)]
    imgs = [load_rgb(f) for f in files]

    worst_shimmer = 0.0
    worst_mean = 0.0
    print(f"shimmer_check: {args.count} frames, threshold={args.threshold}")
    print(f"  pair  mean_abs  max_diff  shimmer_pct")
    for i in range(len(imgs) - 1):
        a, b = imgs[i], imgs[i + 1]
        diff = np.abs(b - a)                     # (H,W,3)
        maxc = diff.max(axis=2)                   # (H,W)
        mean_abs = float(diff.mean())
        max_diff = float(maxc.max())
        changed = maxc > args.threshold
        shimmer_pct = 100.0 * float(changed.mean())
        worst_shimmer = max(worst_shimmer, shimmer_pct)
        worst_mean = max(worst_mean, mean_abs)

        tag = base.stem if base.suffix else base.name
        # amplified grayscale diff so faint shimmer is visible
        amp = np.clip(maxc * args.amplify, 0, 255).astype(np.uint8)
        Image.fromarray(amp, mode="L").save(str(out_dir / f"{tag}_diff_{i + 1}.jpg"), quality=90)
        mask = (changed * 255).astype(np.uint8)
        Image.fromarray(mask, mode="L").save(str(out_dir / f"{tag}_mask_{i + 1}.jpg"), quality=90)

        print(f"  {i + 1:>3}  {mean_abs:9.3f}  {max_diff:9.3f}  {shimmer_pct:11.4f}%")

    print(f"summary: worst shimmer_pct={worst_shimmer:.4f}%  worst mean_abs={worst_mean:.3f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
