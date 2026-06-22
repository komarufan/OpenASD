#!/usr/bin/env python3
"""
build_packages.py — Build .apkg archives and index.idx for OpenASD APM.

Usage (from repo root):
    python3 scripts/build_packages.py [--out <dir>]

Output directory (default: build/packages/):
    index.idx                        — repository package index
    <name>-<version>-x86_64.apkg    — one archive per package

Upload the contents of that directory to a GitHub Release tagged vX.Y,
then set in /etc/apm/apm.conf:
    repo official https://github.com/<user>/OpenASD-packages/releases/latest/download
"""

import os, sys, struct, hashlib, json, time, argparse

ARCH    = "x86_64"
VERSION = "1.0"   # bump when rebuilding packages

# ── Package definitions ────────────────────────────────────────────────────────
# Each entry: (name, install_path, source_path, description, depends)
PACKAGES = [
    # Shell
    ("asdsh",   "/bin/asdsh",   "userland/sh/build/asdsh",
     "ASD interactive shell", []),
    # Core utilities
    ("ls",      "/bin/ls",      "userland/bin/build/ls",
     "List directory contents", []),
    ("cat",     "/bin/cat",     "userland/bin/build/cat",
     "Concatenate and print files", []),
    ("echo",    "/bin/echo",    "userland/bin/build/echo",
     "Print arguments to stdout", []),
    ("pwd",     "/bin/pwd",     "userland/bin/build/pwd",
     "Print current working directory", []),
    ("mkdir",   "/bin/mkdir",   "userland/bin/build/mkdir",
     "Create directories", []),
    ("rm",      "/bin/rm",      "userland/bin/build/rm",
     "Remove files and directories", []),
    ("touch",   "/bin/touch",   "userland/bin/build/touch",
     "Create or update file timestamps", []),
    ("id",      "/bin/id",      "userland/bin/build/id",
     "Print user and group identity", []),
    ("whoami",  "/bin/whoami",  "userland/bin/build/whoami",
     "Print effective username", []),
    ("uname",   "/bin/uname",   "userland/bin/build/uname",
     "Print system information", []),
    ("uptime",  "/bin/uptime",  "userland/bin/build/uptime",
     "Show system uptime", []),
    ("kill",    "/bin/kill",    "userland/bin/build/kill",
     "Send signals to processes", []),
    ("wc",      "/bin/wc",      "userland/bin/build/wc",
     "Word, line, and byte count", []),
    ("hexdump", "/bin/hexdump", "userland/bin/build/hexdump",
     "Hexadecimal file dump", []),
    ("ping",    "/bin/ping",    "userland/bin/build/ping",
     "ICMP echo test", []),
    # Text processing
    ("grep",    "/bin/grep",    "userland/bin/build/grep",
     "Search for patterns in files", []),
    ("find",    "/bin/find",    "userland/bin/build/find",
     "Recursive file search", []),
    ("sort",    "/bin/sort",    "userland/bin/build/sort",
     "Sort lines of text", []),
    ("head",    "/bin/head",    "userland/bin/build/head",
     "Print first N lines of a file", []),
    ("tail",    "/bin/tail",    "userland/bin/build/tail",
     "Print last N lines of a file", []),
    # System tools
    ("sysinfo", "/bin/sysinfo", "userland/bin/build/sysinfo",
     "Show system information", []),
    ("hexdump", "/bin/hexdump", "userland/bin/build/hexdump",
     "Hexadecimal dump utility", []),
    ("do",      "/bin/do",      "userland/bin/build/do",
     "Run command with elevated privileges (wheel users)", []),
    ("apm",     "/bin/apm",     "userland/bin/build/apm",
     "ASD Package Manager", []),
    # Applications
    ("mifetch", "/bin/mifetch", "userland/mifetch/build/mifetch",
     "System information display (fastfetch-style)", []),
    ("hx",      "/bin/hx",      "userland/hx/build/hx",
     "Helix-inspired modal text editor", []),
]


def sha256_file(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return "sha256:" + h.hexdigest()


def build_apkg(name: str, version: str, arch: str, description: str,
               depends: list[str], files: list[tuple[str, bytes]],
               checksum_placeholder: str = "") -> bytes:
    """Build a .apkg binary archive."""
    installed_size = sum(len(d) for _, d in files)
    filename = f"{name}-{version}-{arch}.apkg"

    meta_lines = [
        f"name={name}",
        f"version={version}",
        f"arch={arch}",
        f"description={description}",
        f"filename={filename}",
        f"checksum={checksum_placeholder}",
        f"size=0",           # filled in after
        f"installed_size={installed_size}",
        f"depends={' '.join(depends)}",
        f"provides={name}",
    ]
    meta_text = "\n".join(meta_lines) + "\n"
    meta_bytes = meta_text.encode()

    body = b""
    body += struct.pack("<I", len(files))
    for install_path, data in files:
        path_b = install_path.encode()
        body += struct.pack("<H", len(path_b))
        body += path_b
        body += struct.pack("<I", len(data))
        body += data

    # Header
    hdr = b"APKG"
    hdr += struct.pack("<HH", 1, 0)           # version, flags
    hdr += struct.pack("<I", len(meta_bytes))  # meta_len

    raw = hdr + meta_bytes + body

    # Patch checksum and size now that we know the final size
    # Compute actual checksum of the archive (with placeholder)
    actual_cksum = "sha256:" + hashlib.sha256(raw).hexdigest()
    actual_size  = len(raw)

    meta_lines[5] = f"checksum={actual_cksum}"
    meta_lines[6] = f"size={actual_size}"
    meta_text = "\n".join(meta_lines) + "\n"
    meta_bytes = meta_text.encode()

    hdr2 = b"APKG"
    hdr2 += struct.pack("<HH", 1, 0)
    hdr2 += struct.pack("<I", len(meta_bytes))
    raw2 = hdr2 + meta_bytes + body
    return raw2, actual_cksum, actual_size


def build_index(packages_meta: list[dict], repo_name: str = "official") -> str:
    """Build repository index.idx text."""
    lines = [
        f"# OpenASD APM Repository Index v1",
        f"repo={repo_name}",
        f"timestamp={int(time.time())}",
        f"arch={ARCH}",
        "---",
    ]
    for m in packages_meta:
        lines += [
            f"name={m['name']}",
            f"version={m['version']}",
            f"arch={m['arch']}",
            f"description={m['description']}",
            f"filename={m['filename']}",
            f"checksum={m['checksum']}",
            f"size={m['size']}",
            f"installed_size={m['installed_size']}",
            f"depends={m['depends']}",
            f"provides={m['name']}",
            "---",
        ]
    return "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description="Build OpenASD APM packages")
    parser.add_argument("--out", default="build/packages",
                        help="Output directory (default: build/packages)")
    parser.add_argument("--repo", default="official",
                        help="Repository name in index (default: official)")
    args = parser.parse_args()

    out_dir = args.out
    os.makedirs(out_dir, exist_ok=True)

    packages_meta = []
    built = 0
    skipped = 0

    # Deduplicate by name (keep first occurrence)
    seen = set()
    pkgs = []
    for entry in PACKAGES:
        if entry[0] not in seen:
            seen.add(entry[0])
            pkgs.append(entry)

    for name, install_path, src_path, description, depends in pkgs:
        if not os.path.isfile(src_path):
            print(f"  SKIP  {name:<12} — {src_path} not found")
            skipped += 1
            continue

        with open(src_path, "rb") as f:
            data = f.read()

        files = [(install_path, data)]
        pkg_bytes, checksum, size = build_apkg(
            name=name, version=VERSION, arch=ARCH,
            description=description, depends=depends, files=files)

        filename = f"{name}-{VERSION}-{ARCH}.apkg"
        out_path = os.path.join(out_dir, filename)
        with open(out_path, "wb") as f:
            f.write(pkg_bytes)

        installed_size = len(data)
        packages_meta.append({
            "name":           name,
            "version":        VERSION,
            "arch":           ARCH,
            "description":    description,
            "filename":       filename,
            "checksum":       checksum,
            "size":           size,
            "installed_size": installed_size,
            "depends":        " ".join(depends),
        })
        print(f"  BUILD {name:<12} -> {filename}  ({size//1024} KiB)")
        built += 1

    # Write index
    index_text = build_index(packages_meta, repo_name=args.repo)
    index_path = os.path.join(out_dir, "index.idx")
    with open(index_path, "w") as f:
        f.write(index_text)

    print(f"\nBuilt {built} packages, skipped {skipped}.")
    print(f"Index: {index_path}")
    print(f"Output: {out_dir}/")
    print()
    print("Next steps:")
    print(f"  1. Create GitHub repo: github.com/<you>/OpenASD-packages")
    print(f"  2. Upload contents of {out_dir}/ to a GitHub Release (tag: v{VERSION})")
    print(f"  3. In OpenASD, edit /etc/apm/apm.conf and uncomment:")
    print(f"       repo official https://github.com/<you>/OpenASD-packages/releases/latest/download")
    print(f"  4. Run: apm update  (needs TCP in kernel)")
    print(f"     Or copy .apkg files to /var/apm/cache/ for offline install")


if __name__ == "__main__":
    main()
