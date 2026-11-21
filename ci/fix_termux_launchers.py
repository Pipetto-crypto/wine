#!/usr/bin/env python3
"""Replace Wine utility symlinks with relocatable Termux/box64 launchers.

Match the launcher packaging used by the known-working termux-glibc-wine
workflow. Keep the actual wine/wine64 loaders and all other symlinks intact.
"""

import argparse
import os
from pathlib import Path
import tempfile


LAUNCHER = r'''#!/bin/sh
appname=${0##*/}
appname=${appname%.exe}.exe
preferred_loader=@PREFERRED_LOADER@
fallback_loader=@FALLBACK_LOADER@
case "$0" in
    */*) appdir=${0%/*} ;;
    *) appdir=. ;;
esac
if [ -n "$WINELOADER" ] && [ -x "$WINELOADER" ]; then
    exec "$WINELOADER" "$appname" "$@"
fi
for loader in "$appdir/$preferred_loader" "$appdir/$fallback_loader"; do
    [ ! -x "$loader" ] || exec "$loader" "$appname" "$@"
done
for name in "$preferred_loader" "$fallback_loader"; do
    loader=$(command -v "$name") || continue
    [ ! -x "$loader" ] || exec "$loader" "$appname" "$@"
done
echo "$0: the Wine loader is missing" >&2
exit 1
'''


def fix_launchers(prefix):
    bindir = (Path(prefix) / "bin").resolve(strict=True)
    if not bindir.is_dir():
        raise NotADirectoryError(bindir)
    converted = []
    for path in sorted(bindir.iterdir()):
        if path.name in ("wine", "wine64") or not path.is_symlink():
            continue
        preferred = os.readlink(path)
        if preferred not in ("wine", "wine64"):
            continue
        fallback = "wine64" if preferred == "wine" else "wine"
        launcher = LAUNCHER.replace("@PREFERRED_LOADER@", preferred).replace(
            "@FALLBACK_LOADER@", fallback
        )
        # Replace the utility symlink itself, never its loader target. Create
        # the replacement beside it so replacement is atomic on this filesystem.
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=bindir,
                                         prefix=".wine-launcher-", delete=False) as out:
            out.write(launcher)
            temporary = Path(out.name)
        try:
            temporary.chmod(0o755)
            os.replace(temporary, path)
        finally:
            temporary.unlink(missing_ok=True)
        converted.append(path.name)
    return converted


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("prefix", help="installed Wine prefix, not WINEPREFIX")
    args = parser.parse_args()
    for name in fix_launchers(args.prefix):
        print(f"Installed relocatable launcher: {name}")


if __name__ == "__main__":
    main()
