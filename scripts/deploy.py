#!/usr/bin/env python3
# Jelly5 — upload the built app to the PS5 over FTP (ftpsrv / etaHEN).
# SPDX-License-Identifier: GPL-3.0-or-later
"""
    scripts/deploy.py            upload app/build/app/<TITLE_ID>/ to /data/homebrew/<TITLE_ID>/
                                 (asks first that Jelly5 is closed; --yes when it is known)
    scripts/deploy.py --check    only check that the console's FTP answers
    scripts/deploy.py --probe    upload the bring-up probe (PPSA99506) instead
    scripts/deploy.py --help     this text (any other unknown argument too: nothing is sent)

A first install is staged in /data/jelly5-staging/<TITLE_ID> and renamed into
place; later deploys overwrite the files in place, because ShadowMountPlus
bind-mounts the folder and a replaced folder leaves that mount empty.
eboot.bin and param.json always go up last. ShadowMountPlus then mounts it and registers the tile under Media.

NEVER deploy while Jelly5 is running: replacing files under a live app can
panic the console. Close it from the PS button switcher first.
"""
import ftplib
import json
import os
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def env():
    out = {}
    p = os.path.join(ROOT, ".env.local")
    if os.path.exists(p):
        for line in open(p):
            if "=" in line:
                k, v = line.strip().split("=", 1)
                out[k] = v
    return out


def connect(host, port):
    ftp = ftplib.FTP()
    ftp.connect(host, port, timeout=15)
    ftp.login()
    ftp.set_pasv(True)
    return ftp


def exists(ftp, path):
    try:
        ftp.cwd(path)
        ftp.cwd("/")
        return True
    except ftplib.error_perm:
        return False


def listdir(ftp, path):
    """(name, is_dir) for each entry; ftpsrv has LIST but not NLST/MLSD."""
    lines = []
    ftp.retrlines(f"LIST {path}", lines.append)
    out = []
    for line in lines:
        parts = line.split(None, 8)
        if len(parts) < 9 or parts[8] in (".", ".."):
            continue
        out.append((parts[8], line.startswith("d")))
    return out


# The only trees this script may ever delete: Jelly5's own.
OWN_PREFIXES = ("/data/homebrew/PPSA99505", "/data/homebrew/PPSA99506", "/data/homebrew/PPSA99507", "/data/jelly5-staging/")


def rm_rf(ftp, path, depth=0):
    if not any(path == p.rstrip("/") or path.startswith(p.rstrip("/") + "/") for p in OWN_PREFIXES):
        raise SystemExit(f"refusing to delete {path}: not a Jelly5 path")
    if depth > 16 or not exists(ftp, path):
        return
    for name, is_dir in listdir(ftp, path):
        child = f"{path}/{name}"
        if is_dir:
            rm_rf(ftp, child, depth + 1)
        else:
            ftp.voidcmd(f"DELE {child}")  # ftpsrv answers 226, which ftp.delete rejects
    ftp.rmd(path)


def upload_tree(ftp, local, remote):
    files = []
    for dp, dns, fns in os.walk(local):
        rel = os.path.relpath(dp, local)
        rdir = remote if rel == "." else f"{remote}/{rel}"
        try:
            ftp.mkd(rdir)
        except ftplib.error_perm:
            pass
        for fn in fns:
            files.append((os.path.join(dp, fn), f"{rdir}/{fn}"))
    last = ("eboot.bin", "param.json")
    files.sort(key=lambda f: os.path.basename(f[0]) in last)
    total = sum(os.path.getsize(f[0]) for f in files)
    sent = 0
    t0 = time.time()
    for lp, rp in files:
        with open(lp, "rb") as fh:
            ftp.storbinary(f"STOR {rp}", fh, blocksize=1 << 20)
        sent += os.path.getsize(lp)
        try:
            ftp.sendcmd(f"SITE CHMOD 777 {rp}")
        except ftplib.error_perm:
            pass
        print(f"  {os.path.relpath(lp, local):40s} {sent / total * 100:5.1f}%")
    dt = time.time() - t0
    print(f"  {total / 1e6:.1f} MB in {dt:.1f} s ({total / 1e6 / max(dt, 0.01):.1f} MB/s)")


KNOWN = {"--check", "--probe", "--media-probe", "--yes"}


def main():
    args = sys.argv[1:]
    if any(a in ("-h", "--help") for a in args):
        print(__doc__.strip())
        return
    unknown = [a for a in args if a not in KNOWN]
    if unknown:   # a typo must never deploy (a deploy while Jelly5 runs can panic the console)
        print(__doc__.strip())
        sys.exit(f"unknown argument {' '.join(unknown)}: nothing sent")
    e = env()
    host = e.get("PS5_HOST")
    port = int(e.get("PS5_FTP_PORT", "2121"))
    if not host:
        sys.exit("PS5_HOST missing in .env.local")
    try:
        ftp = connect(host, port)
    except OSError as ex:
        sys.exit(f"no FTP at {host}:{port} ({ex}) - is the console jailbroken and ftpsrv loaded?")
    if "--check" in sys.argv:
        print(f"FTP ok at {host}:{port}: {ftp.getwelcome()}")
        return

    if "--probe" in sys.argv:   # the bring-up probe (JELLY5_PROBE=1 build)
        tid = "PPSA99506"
    elif "--media-probe" in sys.argv:   # JELLY5_PROBE=media build
        tid = "PPSA99507"
    else:
        tid = json.load(open(os.path.join(ROOT, "app/sce_sys/param.json")))["titleId"]
    local = os.path.join(ROOT, "app/build/app", tid)
    if not os.path.exists(os.path.join(local, "eboot.bin")):
        sys.exit(f"{local} has no eboot.bin - build first (app/scripts/build.sh)")
    if "--yes" not in args:
        if not sys.stdin.isatty():
            sys.exit("not a terminal: pass --yes once Jelly5 is known to be closed")
        if input(f"Is {tid} closed on the console (PS button, close)? [y/N] ").strip().lower() not in ("y", "yes", "j", "ja"):
            sys.exit("nothing sent")

    base = "/data/homebrew"
    final = f"{base}/{tid}"
    if exists(ftp, final):
        # Update in place. ShadowMountPlus bind-mounts this folder at
        # /system_ex/app/<TITLE_ID>; replacing the folder would leave that
        # mount on the deleted one (an empty app that crashes on launch).
        print(f"==> updating {tid} in place at {host}:{final}")
        upload_tree(ftp, local, final)
    else:
        # First install: staged outside /data/homebrew (ShadowMountPlus scans
        # every folder there; a same-id folder is a duplicate to it), then
        # renamed in, so it never sees a half-written app.
        staging_root = "/data/jelly5-staging"
        staging = f"{staging_root}/{tid}"
        if not exists(ftp, staging_root):
            ftp.mkd(staging_root)
        print(f"==> installing {tid} to {host}:{final}")
        rm_rf(ftp, staging)
        ftp.mkd(staging)
        upload_tree(ftp, local, staging)
        ftp.rename(staging, final)
    ftp.quit()
    print(f"==> {final} up to date - close Jelly5 before the next deploy")


if __name__ == "__main__":
    main()
