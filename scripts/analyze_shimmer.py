#!/usr/bin/env python3
"""
Analyze shimmer from a sequence of screenshots captured with ENGINE_SHIMMER_TEST=1.

Outputs:
  - shimmer_heatmap.png      : per-pixel shimmer intensity
  - shimmer_overlay.png      : shimmer overlaid on scene
  - variance_heatmap.png     : temporal variance across all frames
  - temporal_instability.png : per-pixel change frequency
  - worst_frames_N_M.png    : the worst consecutive pair side-by-side
  - shimmer_scores.csv       : per-pair shimmer score

Metrics printed:
  - Overall shimmer score (mean per-pixel diff)
  - Region breakdown (top/middle/bottom, left/center/right)
  - Threshold analysis (% pixels above various shimmer levels)
  - Temporal instability (how consistently pixels flicker)
"""

import argparse
import glob
import os
import sys

import cv2
import numpy as np


def load_frames(directory, prefix):
    pattern = os.path.join(directory, f"{prefix}*.png")
    paths = sorted(glob.glob(pattern), key=lambda p: int(
        os.path.basename(p).replace(prefix, "").replace(".png", "")
    ))
    if not paths:
        print(f"No images matching {pattern}", file=sys.stderr)
        sys.exit(1)

    frames = []
    for p in paths:
        img = cv2.imread(p, cv2.IMREAD_COLOR)
        if img is None:
            print(f"Warning: could not read {p}", file=sys.stderr)
            continue
        frames.append(img)

    print(f"Loaded {len(frames)} frames from {directory}")
    return frames


def analyze(frames):
    float_frames = [f.astype(np.float32) for f in frames]

    # Per-pair absolute diffs
    diffs = [np.abs(float_frames[i + 1] - float_frames[i])
             for i in range(len(float_frames) - 1)]

    diff_stack = np.stack(diffs, axis=0)
    shimmer_map = np.mean(diff_stack, axis=0)
    shimmer_gray = np.max(shimmer_map, axis=2)
    per_frame = [float(np.mean(d)) for d in diffs]

    # Temporal variance
    stack = np.stack(float_frames, axis=0)
    var_map = np.max(np.var(stack, axis=0), axis=2)

    # Temporal instability
    h, w = frames[0].shape[:2]
    scale = 4
    sh, sw = h // scale, w // scale
    change_count = np.zeros((sh, sw), dtype=np.int32)
    change_thr = 10.0
    for i in range(len(float_frames) - 1):
        f1 = cv2.resize(float_frames[i], (sw, sh))
        f2 = cv2.resize(float_frames[i + 1], (sw, sh))
        diff = np.max(np.abs(f2 - f1), axis=2)
        change_count += (diff > change_thr).astype(np.int32)
    freq = change_count.astype(np.float32) / max(len(float_frames) - 1, 1)

    return shimmer_map, shimmer_gray, per_frame, var_map, freq


def print_report(shimmer_gray, per_frame, freq):
    h, w = shimmer_gray.shape
    scores = np.array(per_frame)

    print("\n" + "=" * 70)
    print("SHIMMER ANALYSIS REPORT")
    print("=" * 70)

    # Overall
    print(f"\n  Frames analyzed:        {len(per_frame) + 1}")
    print(f"  Overall shimmer score:  {scores.mean():.4f}")
    print(f"  Max pairwise score:     {scores.max():.4f}  (pair {int(np.argmax(scores))})")
    print(f"  Min pairwise score:     {scores.min():.4f}")

    # Regions
    regions = {
        "Top (far)":     shimmer_gray[:h // 3, :],
        "Middle":        shimmer_gray[h // 3:2 * h // 3, :],
        "Bottom (near)": shimmer_gray[2 * h // 3:, :],
        "Left":          shimmer_gray[:, :w // 3],
        "Center":        shimmer_gray[:, w // 3:2 * w // 3],
        "Right":         shimmer_gray[:, 2 * w // 3:],
    }
    print(f"\n  {'Region':<16s} {'Mean':>8s} {'Median':>8s} {'99th %':>8s} {'Max':>8s}")
    print(f"  {'-' * 16} {'-' * 8} {'-' * 8} {'-' * 8} {'-' * 8}")
    for name, r in regions.items():
        print(f"  {name:<16s} {np.mean(r):8.3f} {np.median(r):8.3f} "
              f"{np.percentile(r, 99):8.2f} {np.max(r):8.2f}")

    # Thresholds
    total = shimmer_gray.size
    print(f"\n  {'Threshold':>10s} {'Pixels':>10s} {'%':>8s}")
    print(f"  {'-' * 10} {'-' * 10} {'-' * 8}")
    for t in [5, 10, 20, 30, 50, 80, 100]:
        c = np.sum(shimmer_gray > t)
        print(f"  {t:10d} {c:10d} {c / total * 100:7.3f}%")

    # Temporal instability
    total_pairs = len(per_frame)
    print(f"\n  Temporal instability (change freq, thr={10}):")
    for pct in [0.25, 0.50, 0.75, 0.90]:
        frac = np.mean(freq > pct) * 100
        print(f"    Change > {int(pct * 100):2d}% of frames: {frac:7.3f}%")

    print("=" * 70)


def save_visuals(directory, frames, shimmer_map, shimmer_gray, var_map, freq, per_frame):
    out = os.path.join(directory, "analysis")
    os.makedirs(out, exist_ok=True)
    h, w = frames[0].shape[:2]

    def save_heatmap(name, data, cmap=cv2.COLORMAP_JET):
        norm = data / max(data.max(), 1e-6)
        color = cv2.applyColorMap((norm * 255).astype(np.uint8), cmap)
        cv2.imwrite(os.path.join(out, name), color)
        return color

    # Shimmer heatmap
    shm_color = save_heatmap("shimmer_heatmap.png", shimmer_gray)

    # Shimmer overlay
    overlay = cv2.addWeighted(frames[0], 0.5, shm_color, 0.5, 0)
    cv2.imwrite(os.path.join(out, "shimmer_overlay.png"), overlay)

    # Variance heatmap
    save_heatmap("variance_heatmap.png", var_map)

    # Temporal instability
    freq_up = cv2.resize(freq, (w, h), interpolation=cv2.INTER_NEAREST)
    save_heatmap("temporal_instability.png", freq_up, cv2.COLORMAP_HOT)

    # Worst pair
    if per_frame:
        idx = int(np.argmax(per_frame))
        diff = np.max(np.abs(
            frames[idx + 1].astype(np.float32) - frames[idx].astype(np.float32)), axis=2
        ).astype(np.uint8)
        save_heatmap(f"worst_diff_{idx}_{idx + 1}.png", diff.astype(np.float32))
        side = np.hstack([frames[idx], frames[idx + 1]])
        cv2.imwrite(os.path.join(out, f"worst_frames_{idx}_{idx + 1}.png"), side)

    # CSV
    with open(os.path.join(out, "shimmer_scores.csv"), "w") as f:
        f.write("frame_pair,mean_abs_diff\n")
        for i, s in enumerate(per_frame):
            f.write(f"{i}_{i + 1},{s:.4f}\n")

    print(f"\n  Visuals saved to {out}/")
    return out


def main():
    parser = argparse.ArgumentParser(
        description="Analyze shimmer from screenshot sequence")
    parser.add_argument("directory", nargs="?", default="/tmp/shimmer_test")
    parser.add_argument("--prefix", default="shimmer_")
    args = parser.parse_args()

    frames = load_frames(args.directory, args.prefix)
    if len(frames) < 2:
        sys.exit("Need at least 2 frames")

    shimmer_map, shimmer_gray, per_frame, var_map, freq = analyze(frames)
    print_report(shimmer_gray, per_frame, freq)
    save_visuals(args.directory, frames, shimmer_map, shimmer_gray, var_map, freq, per_frame)


if __name__ == "__main__":
    main()
