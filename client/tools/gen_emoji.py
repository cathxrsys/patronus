#!/usr/bin/env python3
"""Regenerate client/EmojiData.js — the emoji-picker dataset.

Source of truth: Unicode's emoji-test.txt (CLDR-ordered, 9 groups, current
names), intersected with the glyphs NotoColorEmoji actually ships so the picker
never shows a tofu box.

Usage:
    curl -sO https://unicode.org/Public/emoji/latest/emoji-test.txt
    python3 tools/gen_emoji.py emoji-test.txt

v1 keeps only single-scalar emoji (drops the emoji-presentation selector FE0F).
ZWJ sequences, skin-tone variants and 2-regional-indicator country flags are
intentionally excluded — add them here when the picker learns to render them.
"""
import subprocess, re, io, os, sys

FONT = os.environ.get("EMOJI_FONT", "/usr/share/fonts/noto/NotoColorEmoji.ttf")
SRC = sys.argv[1] if len(sys.argv) > 1 else "emoji-test.txt"
OUT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "EmojiData.js")

# Codepoints the bundled font can actually draw.
charset = subprocess.check_output(["fc-query", "--format=%{charset}\n", FONT]).decode()
cps = set()
for tok in charset.split():
    if "-" in tok:
        a, b = tok.split("-")
        cps.update(range(int(a, 16), int(b, 16) + 1))
    elif tok:
        cps.add(int(tok, 16))

# Unicode group name -> (data key, tab-strip icon)
GROUPS = {
    "Smileys & Emotion": ("smileys", "\U0001F600"),
    "People & Body":     ("people",  "\U0001F44B"),
    "Animals & Nature":  ("animals", "\U0001F43B"),
    "Food & Drink":      ("food",    "\U0001F354"),
    "Travel & Places":   ("travel",  "\U0001F697"),
    "Activities":        ("activities", "⚽"),
    "Objects":           ("objects", "\U0001F4A1"),
    "Symbols":           ("symbols", "\U0001F523"),
    "Flags":             ("flags",   "\U0001F6A9"),
}
order = list(GROUPS)
buckets = {g: [] for g in order}
seen = set()

group = None
with io.open(SRC, encoding="utf-8") as f:
    for line in f:
        if line.startswith("# group:"):
            group = line.split(":", 1)[1].strip()
            continue
        if not line.strip() or line.startswith("#"):
            continue
        m = re.match(r"^([0-9A-Fa-f ]+?)\s*;\s*(\S+)\s*#\s*(\S+)\s+E\d+\.\d+\s+(.*)$", line)
        if not m:
            continue
        cps_hex, status, glyph, name = m.groups()
        if status != "fully-qualified" or group not in GROUPS:
            continue
        seq = [c for c in (int(x, 16) for x in cps_hex.split()) if c != 0xFE0F]
        if len(seq) != 1:
            continue
        cp = seq[0]
        if cp in seen or cp not in cps:
            continue
        seen.add(cp)
        buckets[group].append((chr(cp), name.strip()))


def esc(s):
    return s.replace("\\", "\\\\").replace('"', '\\"')


lines = [
    "// AUTO-GENERATED from Unicode emoji-test.txt, intersected with NotoColorEmoji",
    "// glyph coverage. Do not edit by hand — run client/tools/gen_emoji.py.",
    ".pragma library",
    "",
    "var categories = [",
]
for gi, g in enumerate(order):
    key, icon = GROUPS[g]
    row = [f'{{e:"{ch}",n:"{esc(name)}"}}' for ch, name in buckets[g]]
    lines += ["  {", f'    key: "{key}",', f'    name: "{esc(g)}",',
              f'    icon: "{icon}",', "    emoji: ["]
    for i in range(0, len(row), 6):
        lines.append("      " + ",".join(row[i:i + 6]) + ("" if i + 6 >= len(row) else ","))
    lines += ["    ]", "  }" + ("" if gi == len(order) - 1 else ",")]
lines += ["]", ""]

with io.open(OUT, "w", encoding="utf-8") as f:
    f.write("\n".join(lines))

total = sum(len(v) for v in buckets.values())
print(f"wrote {OUT}: {total} emoji across {len(order)} categories")
for g in order:
    print(f"  {GROUPS[g][0]:11} {len(buckets[g])}")
