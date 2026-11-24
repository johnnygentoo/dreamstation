#!/usr/bin/env python3
"""Refuses games, firmware, saves and Sega's art in git.

File names can't be trusted: a disc image or BIOS can be renamed. So files
are judged by content as well as name. Everything this repository holds is
text; any binary file is refused unless its exact content is listed below.

  tools/check_no_roms.py               check what is staged (pre-commit hook)
  tools/check_no_roms.py --push REV..  check every object reachable from REV
                                       that the remote lacks (pre-push hook)
  tools/check_no_roms.py --audit       check every object in the whole history
"""
import hashlib
import re
import subprocess
import sys

BANNED_EXTENSIONS = {
    # Discs, cartridges and archives that could hold them
    ".gdi", ".cdi", ".chd", ".cue", ".bin", ".iso", ".nrg", ".mds", ".mdf", ".raw", ".img",
    ".dat", ".rom", ".ic22", ".zip", ".7z", ".rar", ".gz", ".xz", ".bz2", ".zst", ".tar",
    # Memory cards, saves and states
    ".vmu", ".vms", ".vmi", ".dci", ".dcm", ".sav", ".srm", ".state", ".eep", ".nvmem", ".nvmem2",
    # Pictures: covers, box art and screenshots are other people's
    ".png", ".jpg", ".jpeg", ".webp", ".gif", ".bmp", ".tga", ".ico", ".mp4", ".mkv",
}
MAX_SIZE = 2 * 1024 * 1024
# Binary files that are ours to keep, by exact SHA-256. None so far.
ALLOWED = set()
SIGNATURES = [
    (b"SEGA SEGAKATANA", "Dreamcast disc boot sector (IP.BIN)"),
    (b"SEGA ENTERPRISES", "Sega disc or firmware data"),
    (b"NAOMI           ", "Naomi cartridge header"),
    (b"NAOMI2          ", "Naomi 2 cartridge header"),
]
EMBEDDED_IMAGE = re.compile(rb"data:image/(png|jpe?g|webp|gif)", re.I)


def git(*args):
    return subprocess.run(("git",) + args, capture_output=True, check=True).stdout


def verdict(name, blob):
    """Why this file can't be committed, or None if it can."""
    lower = name.lower()
    dot = lower.rfind(".")
    extension = lower[dot:] if dot > lower.rfind("/") else ""
    if extension in BANNED_EXTENSIONS:
        return f"banned extension {extension}"
    if "bios" in lower.rsplit("/", 1)[-1] and not lower.endswith((".cpp", ".hpp", ".h", ".c", ".md", ".txt")):
        return "named like firmware"
    # Source that looks for these markers names them in text; real discs and
    # cartridges carry them in binary data.
    if b"\0" in blob:
        for signature, what in SIGNATURES:
            if signature in blob[:0x10000]:
                return what
    if blob[:4] in (b"PK\x03\x04", b"Rar!") or blob[:6] == b"7z\xbc\xaf\x27\x1c" or blob[:2] == b"\x1f\x8b":
        return "archive, which could hold anything"
    if EMBEDDED_IMAGE.search(blob):
        return "picture embedded in a text file"
    if len(blob) > MAX_SIZE:
        return f"{len(blob)} bytes is too big to be ours"
    if b"\0" in blob and hashlib.sha256(blob).hexdigest() not in ALLOWED:
        return "binary file, and this repository only holds text"
    return None


def staged():
    """(name, blob) for each staged file. Submodule links have no blob here."""
    listing = git("diff", "--cached", "--no-renames", "--raw", "--no-abbrev", "-z", "--diff-filter=ACMRT").split(b"\0")
    entries = [e for e in listing if e]
    for meta, path in zip(entries[::2], entries[1::2]):
        fields = meta.decode().split()
        if fields[1] == "160000":
            continue
        yield path.decode(errors="replace"), git("cat-file", "blob", fields[3])


def objects(*revs):
    for line in git("rev-list", "--objects", *revs).decode(errors="replace").splitlines():
        object_id, _, name = line.partition(" ")
        if name and git("cat-file", "-t", object_id).strip() == b"blob":
            yield name, git("cat-file", "blob", object_id)


def main():
    if "--audit" in sys.argv:
        source = objects("--all")
    elif "--push" in sys.argv:
        source = objects(*sys.argv[sys.argv.index("--push") + 1:])
    else:
        source = staged()
    checked, refused = 0, []
    for name, blob in source:
        checked += 1
        reason = verdict(name, blob)
        if reason:
            refused.append(f"  {name}: {reason}")
    if refused:
        print("Refusing: no games, firmware, saves or box art in this repository:", file=sys.stderr)
        print("\n".join(refused), file=sys.stderr)
        return 1
    if "--audit" in sys.argv:
        print(f"clean: {checked} objects checked")
    return 0


if __name__ == "__main__":
    sys.exit(main())
