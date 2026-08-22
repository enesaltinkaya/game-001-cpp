#!/usr/bin/env python3
"""rdc.py — inspect RenderDoc frame captures headlessly.

Subcommands:
  list    show passes (debug-label groups) and their output render targets
  dump    extract pass output images (PNG) to disk
  latest  print the newest .rdc in /tmp/RenderDoc
  clean   delete old .rdc captures (~1 GB each)

Examples:
  scripts/rdc.py list
  scripts/rdc.py dump scene_depth_prepass          # dump that pass's output
  scripts/rdc.py dump scene_depth_prepass --out 1 --frames 3
  scripts/rdc.py dump 241                          # dump outputs at raw event id 241
  scripts/rdc.py dump --last                        # output of the last draw call
  scripts/rdc.py dump --all                         # every pass's output
  scripts/rdc.py clean --keep 1

The engine groups its passes under vk debug-label markers (PushMarker groups:
scene_depth_prepass, phase2_depth, ...). "The result of a pass" is the render
target state at the pass's *last* draw, not at vkCmdBeginRendering (which is
post-clear / pre-draw).

Uses the local v1.46-dev build's replay module; see docs/renderdoc-capture.md.
"""

import argparse
import glob
import os
import re
import sys

RD_LIB = "/home/enes/Apps/renderdoc/build/lib"
CAPTURE_DIR = "/tmp/RenderDoc"
DEFAULT_OUTDIR = "/tmp/rdc-dump"


def latest_capture(directory=CAPTURE_DIR):
    files = glob.glob(os.path.join(directory, "*.rdc"))
    if not files:
        sys.exit(
            "no .rdc files in %s — capture one first: "
            "ENGINE_RENDERDOC_CAPTURE=1 ENGINE_RENDERDOC_CAPTURE_DELAY_MS=6000 "
            "ENGINE_SKIP_MAIN_MENU=1 ENGINE_LOG_TIMEOUT=12000 ./scripts/run.sh renderdoc"
            % directory
        )
    return max(files, key=os.path.getmtime)


def open_replay(path):
    sys.path.insert(0, RD_LIB)
    import renderdoc as rd

    rd.InitialiseReplay(rd.GlobalEnvironment(), [])
    cap = rd.OpenCaptureFile()
    r = cap.OpenFile(path, "", None)
    if r.OK() is not True:
        sys.exit("failed to open capture: %s (%s)" % (path, r.Message()))
    res, ctrl = cap.OpenCapture(rd.ReplayOptions(), None)
    if res.OK() is not True:
        sys.exit("failed to open capture for replay: %s" % res.Message())
    return rd, cap, ctrl


def walk_actions(ctrl):
    """All actions in the tree — draws live under PushMarker/boundary nodes,
    so GetRootActions() alone is not enough."""
    out, stack = [], list(ctrl.GetRootActions())
    while stack:
        a = stack.pop()
        out.append(a)
        stack.extend(a.children)
    out.sort(key=lambda a: a.eventId)  # frame order, so 'last' means last
    return out


def action_name(a):
    return a.customName or "?"


def texture_map(ctrl):
    """str(resourceId) -> TextureDescription for every texture in the capture."""
    return {str(t.resourceId): t for t in ctrl.GetTextures()}


def tex_desc_str(t):
    dims = "%dx%d" % (t.width, t.height)
    if t.arraysize > 1:
        dims += "x%d" % t.arraysize
    if t.cubemap:
        dims += "cubemap"
    return "%s %s" % (t.format.Name(), dims)


def save_texture(ctrl, rd, rid, path):
    ts = rd.TextureSave()
    ts.resourceId = rid
    ts.mip = 0
    sm = rd.TextureSliceMapping()
    sm.first = 0
    sm.count = 1
    ss = rd.TextureSampleMapping()
    ss.first = 0
    ss.count = 1
    ts.slice = sm
    ts.sample = ss
    ts.destType = rd.FileType.PNG
    ok = ctrl.SaveTexture(ts, path)
    ok = ok if isinstance(ok, bool) else ok.OK()
    return ok is True and os.path.exists(path)


def slug(s):
    return re.sub(r"[^A-Za-z0-9._-]+", "_", s).strip("_") or "unnamed"


def output_targets(rd, ctrl, eid):
    """SetFrameEvent, then read outputs (gotcha: pipeline state is relative to
    the *current* event)."""
    r = ctrl.SetFrameEvent(eid, True)
    if r is not None:
        ok = r if isinstance(r, bool) else r.OK()
        if ok is not True:
            return None
    return ctrl.GetPipelineState().GetOutputTargets()


def outputs_str(rd, ctrl, tmap, eid):
    outs = output_targets(rd, ctrl, eid)
    if not outs:
        return ""
    parts = []
    for i, ot in enumerate(outs):
        t = tmap.get(str(ot.resource))
        parts.append("[%d] %s" % (i, tex_desc_str(t) if t else "swapchain?"))
    return "  outputs: " + " | ".join(parts)


def pass_groups(rd, acts):
    """PushMarker actions with children — the engine's pass grouping
    (e.g. scene_depth_prepass, phase2_depth, scene_culling)."""
    F = rd.ActionFlags
    return [a for a in acts if F.PushMarker in a.flags and a.children]


def group_end_event(rd, g):
    """Event where the group's *result* lives: its last draw (render targets are
    only reported in draw pipeline state, not dispatch/EndPass state), else the
    group event itself."""
    F = rd.ActionFlags
    kids = []
    stack = list(g.children)
    while stack:
        c = stack.pop()
        kids.append(c)
        stack.extend(c.children)
    draws = [c.eventId for c in kids if F.Drawcall in c.flags]
    if draws:
        return max(draws)
    return g.eventId


def match_event_id(filt):
    """Allow matching a raw event id: 'dump 241'."""
    try:
        return int(filt)
    except ValueError:
        return None


def cmd_list(args):
    path = args.capture or latest_capture(args.capture_dir)
    print("capture:", path)
    rd, cap, ctrl = open_replay(path)
    tmap = texture_map(ctrl)
    acts = walk_actions(ctrl)
    F = rd.ActionFlags
    groups = pass_groups(rd, acts)
    draws = [a for a in acts if F.Drawcall in a.flags]
    dispatches = [a for a in acts if F.Dispatch in a.flags]
    print("pass groups: %d   draws: %d   dispatches: %d" % (len(groups), len(draws), len(dispatches)))
    print()
    for g in groups:
        end = group_end_event(rd, g)
        outs = outputs_str(rd, ctrl, tmap, end)
        compute = " (compute)" if (F.Dispatch in g.flags or any(F.Dispatch in c.flags for c in g.children)) and not outs else ""
        print("%6d-%-6d  %-32s%s%s" % (g.eventId, end, action_name(g)[:32], compute, outs))
    ctrl.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()


def dump_one(ctrl, rd, tmap, eid, name_hint, out_idx, outdir, frame_tag=""):
    """Save output target out_idx at event eid."""
    outs = output_targets(rd, ctrl, eid)
    if outs is None:
        print("  ! SetFrameEvent failed for eid=%d" % eid)
        return
    if not outs:
        print("  ! no color output targets at eid=%d (compute-only pass? try a raw draw eid or qrenderdoc)" % eid)
        return
    if out_idx >= len(outs):
        print("  ! out idx %d out of range (%d targets)" % (out_idx, len(outs)))
        return
    rid = outs[out_idx].resource
    t = tmap.get(str(rid))
    fname = "%s_eid%d_out%d%s.png" % (slug(name_hint), eid, out_idx, frame_tag)
    path = os.path.join(outdir, fname)
    if save_texture(ctrl, rd, rid, path):
        extra = "  (%s)" % tex_desc_str(t) if t else "  (swapchain?)"
        print("  saved: %s%s" % (path, extra))
    else:
        print("  ! failed to save out[%d] %s (swapchain image?)" % (out_idx, fname))


def cmd_dump(args):
    path = args.capture or latest_capture(args.capture_dir)
    print("capture:", path)
    rd, cap, ctrl = open_replay(path)
    tmap = texture_map(ctrl)
    acts = walk_actions(ctrl)
    os.makedirs(args.outdir, exist_ok=True)

    targets = []  # (eid, name)

    if args.last:
        draws = [a for a in acts if rd.ActionFlags.Drawcall in a.flags]
        if not draws:
            sys.exit("no draw calls in capture (triggered during loading?)")
        targets.append((draws[-1].eventId, action_name(draws[-1])))
    elif args.all:
        skipped = 0
        for g in pass_groups(rd, acts):
            eid = group_end_event(rd, g)
            if not output_targets(rd, ctrl, eid):
                skipped += 1
                continue
            targets.append((eid, action_name(g)))
        if skipped:
            print("(skipped %d groups without color outputs — compute passes)" % skipped)
    elif args.filter is not None:
        eid = match_event_id(args.filter)
        if eid is not None:
            targets.append((eid, "event%d" % eid))
        else:
            low = args.filter.lower()
            groups = [g for g in pass_groups(rd, acts) if low in action_name(g).lower()]
            others = [a for a in acts if a not in groups and low in action_name(a).lower()]
            matches = groups + others
            if not matches:
                sys.exit("no pass/action matching '%s' (try: %s list)" % (args.filter, os.path.basename(__file__)))
            for m in matches[: args.match]:
                eid = group_end_event(rd, m) if m in groups else m.eventId
                targets.append((eid, action_name(m)))
    else:
        sys.exit("nothing to dump — give a name filter, --last, or --all")

    for eid, name in targets:
        print("%s  (eid=%d)" % (name, eid))
        for f in range(args.frames):
            # event ids encode the frame in the high bits: (frame << 16) | idx
            feid = (((eid >> 16) + f) << 16) | (eid & 0xFFFF)
            tag = "_f%d" % f if (args.frames > 1 and f > 0) else ""
            dump_one(ctrl, rd, tmap, feid, name, args.out, args.outdir, tag)

    ctrl.Shutdown()
    cap.Shutdown()
    rd.ShutdownReplay()


def cmd_latest(args):
    print(latest_capture(args.capture_dir))


def cmd_clean(args):
    files = sorted(glob.glob(os.path.join(args.capture_dir, "*.rdc")), key=os.path.getmtime)
    if not files:
        print("nothing to clean in %s" % args.capture_dir)
        return
    keep = set(files[-args.keep:]) if args.keep else set()
    freed = 0.0
    for f in files:
        if f in keep:
            continue
        size = os.path.getsize(f) / 1e9
        if args.dry_run:
            print("would delete: %s (~%.2f GB)" % (f, size))
        else:
            os.remove(f)
            freed += size
            print("deleted: %s (~%.2f GB)" % (f, size))
    if not args.dry_run:
        print("freed ~%.1f GB" % freed)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    pl = sub.add_parser("list", help="show passes (debug-label groups) and their output targets")
    pl.add_argument("--capture", help=".rdc path (default: newest in %s)" % CAPTURE_DIR)
    pl.add_argument("--capture-dir", default=CAPTURE_DIR)
    pl.set_defaults(fn=cmd_list)

    pd = sub.add_parser("dump", help="save pass output images as PNG")
    pd.add_argument("filter", nargs="?", help="case-insensitive substring of a pass/draw debug label, or a raw event id")
    pd.add_argument("--capture", help=".rdc path (default: newest in %s)" % CAPTURE_DIR)
    pd.add_argument("--capture-dir", default=CAPTURE_DIR)
    pd.add_argument("--out", type=int, default=0, help="output target index to save (default 0; see 'list' for the indices)")
    pd.add_argument("--last", action="store_true", help="output of the last draw call in the frame")
    pd.add_argument("--all", action="store_true", help="dump every pass group's output")
    pd.add_argument("--frames", type=int, default=1, help="dump on N consecutive frames (needs a multi-frame capture; flicker debugging)")
    pd.add_argument("--match", type=int, default=1, help="max matching passes to dump when a filter is given (default 1)")
    pd.add_argument("--outdir", default=DEFAULT_OUTDIR)
    pd.set_defaults(fn=cmd_dump)

    pc = sub.add_parser("clean", help="delete old .rdc files")
    pc.add_argument("--keep", type=int, default=0, help="keep the N newest (default: delete all)")
    pc.add_argument("--capture-dir", default=CAPTURE_DIR)
    pc.add_argument("--dry-run", action="store_true")
    pc.set_defaults(fn=cmd_clean)

    pl_ = sub.add_parser("latest", help="print the newest .rdc path")
    pl_.add_argument("--capture-dir", default=CAPTURE_DIR)
    pl_.set_defaults(fn=cmd_latest)

    args = p.parse_args()
    args.fn(args)


if __name__ == "__main__":
    main()