frame times are not stable.
i see it in mangohud's display.
but our screenshot feature does not capture mangohud overlay.

so maybe you can, create a temporary file, write individual frame times in that file, for maybe 20-30 seconds, then analyze.
but that writing file logic should not interfere with engine loop. i mean io operations can be slow.
maybe store every frame time in memory, and do a single write at the end.
