#!/usr/bin/env python3
"""
Focused shimmer analysis for alpha-cutout grass planes.
Compares edge-heavy regions vs stable regions and provides
per-region shimmer metrics.
"""

import glob
import os
import sys

import cv2
import numpy as np


def load_frames(directory, prefix="shimmer_"):
    pattern = os.path.join(directory, f"{prefix}*.png")
    paths = sorted(glob.glob(pattern), key=lambda p: int(
        os.path.basename(p).replace(prefix, "").replace(".png", "")
    ))
    frames = []
    for p in paths:
        img = cv2.imread(p, cv2.IMREAD_COLOR)
        if img is not None:
            frames.append(img.astype(np.float32))
    print(f"Loaded {len(frames)} frames ({len(paths)} files found)")
    return frames


def analyze_shimmer(frames):
    """Compute consecutive-frame mean absolute diff per pixel."""
    diffs = []
    for i in range(len(frames) - 1):
        diffs.append(np.abs(frames[i + 1] - frames[i]))
    
    diff_stack = np.stack(diffs, axis=0)
    shimmer_map = np.mean(diff_stack, axis=0)  # (H, W, 3)
    shimmer_gray = np.max(shimmer_map, axis=2)  # (H, W)
    
    per_frame_scores = [float(np.mean(d)) for d in diffs]
    
    return shimmer_map, shimmer_gray, per_frame_scores


def analyze_regions(shimmer_gray, h, w):
    """Analyze shimmer in different screen regions."""
    regions = {
        "Top (far)":       shimmer_gray[:h//3, :],
        "Middle":          shimmer_gray[h//3:2*h//3, :],
        "Bottom (near)":   shimmer_gray[2*h//3:, :],
        "Left":            shimmer_gray[:, :w//3],
        "Center":          shimmer_gray[:, w//3:2*w//3],
        "Right":           shimmer_gray[:, 2*w//3:],
    }
    
    print("\n" + "=" * 70)
    print("REGION-BASED SHIMMER ANALYSIS")
    print("=" * 70)
    print(f"  {'Region':<16s} {'Mean':>8s} {'Median':>8s} {'99th %':>8s} {'Max':>8s}")
    print(f"  {'-'*16} {'-'*8} {'-'*8} {'-'*8} {'-'*8}")
    
    for name, region in regions.items():
        mean = np.mean(region)
        med = np.median(region)
        p99 = np.percentile(region, 99)
        mx = np.max(region)
        print(f"  {name:<16s} {mean:8.3f} {med:8.3f} {p99:8.2f} {mx:8.2f}")
    
    return regions


def analyze_threshold(shimmer_gray):
    """Count pixels above various shimmer thresholds."""
    thresholds = [5, 10, 15, 20, 30, 50, 80, 100]
    total = shimmer_gray.size
    
    print("\n  SHIMMER THRESHOLD ANALYSIS")
    print(f"  {'Threshold':>10s} {'Pixels':>10s} {'%':>8s}")
    print(f"  {'-'*10} {'-'*10} {'-'*8}")
    for t in thresholds:
        count = np.sum(shimmer_gray > t)
        pct = count / total * 100
        print(f"  {t:10d} {count:10d} {pct:7.3f}%")


def compute_temporal_stability(frames):
    """
    For each pixel, compute how many times it changes by more than a threshold
    across the frame sequence. High change counts indicate persistent shimmer.
    """
    change_threshold = 10.0  # pixel value difference
    h, w, c = frames[0].shape
    
    # Downsample for speed
    scale = 4
    small_h, small_w = h // scale, w // scale
    
    change_count = np.zeros((small_h, small_w), dtype=np.int32)
    
    for i in range(len(frames) - 1):
        f1 = cv2.resize(frames[i], (small_w, small_h))
        f2 = cv2.resize(frames[i + 1], (small_w, small_h))
        diff = np.max(np.abs(f2 - f1), axis=2)
        change_count += (diff > change_threshold).astype(np.int32)
    
    total_pairs = len(frames) - 1
    freq = change_count.astype(np.float32) / total_pairs
    
    print(f"\n  TEMPORAL INSTABILITY (change freq per pixel, threshold={change_threshold})")
    print(f"  Frame pairs: {total_pairs}")
    print(f"  {'Metric':<30s} {'Value':>10s}")
    print(f"  {'-'*30} {'-'*10}")
    
    # What fraction of pixels change more than X% of the time
    for pct in [0.25, 0.50, 0.75, 0.90]:
        frac = np.mean(freq > pct) * 100
        print(f"  Change > {int(pct*100)}% of frames:       {frac:8.3f}%")
    
    # Mean change frequency in the top 25% of the image (where grass edges are)
    top_quarter = freq[:small_h // 4, :]
    print(f"  Top 25% of screen mean freq:  {np.mean(top_quarter):8.4f}")
    print(f"  Middle 50% of screen mean freq: {np.mean(freq[small_h//4:3*small_h//4, :]):8.4f}")
    print(f"  Bottom 25% of screen mean freq:{np.mean(freq[3*small_h//4:, :]):8.4f}")
    
    # Save instability heatmap
    return freq


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "/tmp/shimmer_test"
    frames = load_frames(directory)
    if len(frames) < 2:
        print("Need at least 2 frames", file=sys.stderr)
        sys.exit(1)
    
    h, w = frames[0].shape[:2]
    print(f"Frame resolution: {w}x{h}")
    
    shimmer_map, shimmer_gray, per_frame_scores = analyze_shimmer(frames)
    analyze_regions(shimmer_gray, h, w)
    analyze_threshold(shimmer_gray)
    
    freq = compute_temporal_stability(frames)
    
    # Save focused analysis
    out_dir = os.path.join(directory, "analysis")
    os.makedirs(out_dir, exist_ok=True)
    
    # Instability heatmap
    freq_norm = freq / max(freq.max(), 1e-6)
    freq_color = cv2.applyColorMap((freq_norm * 255).astype(np.uint8), cv2.COLORMAP_HOT)
    freq_color_up = cv2.resize(freq_color, (w, h), interpolation=cv2.INTER_NEAREST)
    cv2.imwrite(os.path.join(out_dir, "temporal_instability_heatmap.png"), freq_color_up)
    
    # Overall score
    scores = np.array(per_frame_scores)
    print(f"\n  OVERALL SHIMMER SCORE (mean per-pixel diff): {scores.mean():.4f}")
    print(f"  This is the baseline metric. After applying fixes, re-run")
    print(f"  and compare against this value.")
    
    print(f"\n  Analysis images saved to: {out_dir}/")


if __name__ == "__main__":
    main()
