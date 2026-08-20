#version 460

// ── Azgaar props CSM shadow fragment shader ───────────────────────────────
// Depth-only: no colour outputs, no alpha test.  The small cutouts flowers /
// leaf cards would make (radial disc, texture alpha) are not resolvable in
// the cascade shadow maps, so casting them solid is cheaper and reads fine.
void main() {
}