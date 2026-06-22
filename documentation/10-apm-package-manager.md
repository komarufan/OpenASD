# APM — ASD Package Manager

`apm` is the package manager for OpenASD, inspired by apk (Alpine) and xbps (Void Linux).

---

## Usage

```sh
apm update              # sync repository package indexes
apm install <pkg>...    # install packages
apm del <pkg>...        # remove packages
apm upgrade             # upgrade all installed packages
apm search <query>      # search by name or description
apm list                # list installed packages
apm info <pkg>          # show package details
apm clean               # remove cached archives
apm check               # verify installed file integrity
```

---

## Configuration — `/etc/apm/apm.conf`

Created automatically on first run with all repositories **commented out**.
To enable a repository, uncomment the `repo` line:

```sh
# /etc/apm/apm.conf

# Uncomment to enable:
# repo official https://github.com/komarufan/OpenASD-packages/releases/latest/download

arch=x86_64
```

Format of a `repo` line:
```
repo <name> <base-url>
```

The base URL must contain:
- `index.idx` — package index file
- `<name>-<version>-<arch>.apkg` — package archives

---

## Directory layout

```
/etc/apm/apm.conf              — configuration
/var/apm/db/installed/         — installed package records (.apd files)
/var/apm/lists/                — cached repository indexes (.idx files)
/var/apm/cache/                — downloaded package archives (.apkg files)
```

---

## Package database format (.apd)

One text file per installed package at `/var/apm/db/installed/<name>.apd`:

```
name=grep
version=1.0
arch=x86_64
description=Pattern search utility
checksum=sha256:abcdef1234567890
size=4704
installed_size=4704
repo=official
depends=
file=/bin/grep
```

Fields:
| Field | Description |
|-------|-------------|
| `name` | Package name (unique identifier) |
| `version` | Version string |
| `arch` | Architecture (`x86_64`) |
| `description` | One-line description |
| `checksum` | `sha256:<hex>` of the .apkg archive |
| `size` | Archive size in bytes |
| `installed_size` | Total size of installed files |
| `repo` | Repository name it came from |
| `depends` | Space-separated list of required packages |
| `file` | One line per installed file (repeated) |

---

## Repository index format (.idx)

The index is a text file with records separated by `---`. The first record
is a header, followed by one record per package:

```
# APM Index v1
repo=official
timestamp=1750000000
arch=x86_64
---
name=grep
version=1.0
arch=x86_64
description=Pattern search utility
filename=grep-1.0-x86_64.apkg
checksum=sha256:abcdef1234567890abcdef1234567890
size=4704
installed_size=4704
depends=
provides=grep
---
name=find
version=1.0
arch=x86_64
description=Recursive file finder
filename=find-1.0-x86_64.apkg
checksum=sha256:fedcba0987654321fedcba0987654321
size=5128
installed_size=5128
depends=
provides=find
---
```

---

## Package archive format (.apkg)

Binary format for distributing packages:

```
Offset   Size     Content
0        4        Magic: "APKG"
4        2        Version: 1 (uint16_t LE)
6        2        Flags: 0 (uint16_t LE)
8        4        Metadata length N (uint32_t LE)
12       N        Metadata: key=value text (same as .apd format)
12+N     4        File count F (uint32_t LE)

For each file (F times):
  +0     2        Path length P (uint16_t LE)
  +2     P        File path (e.g. "/bin/grep")
  +2+P   4        Data length D (uint32_t LE)
  +6+P   D        Raw file data
```

---

## Creating a package (for repository maintainers)

Here is a Python script to build an `.apkg` archive:

```python
#!/usr/bin/env python3
"""build_apkg.py — Create an OpenASD .apkg package archive"""
import struct, sys, os

def build_apkg(meta: dict, files: list[tuple[str, bytes]]) -> bytes:
    """
    meta   — dict of package metadata fields
    files  — list of (install_path, data_bytes) tuples
    """
    # Build metadata text
    meta_text = ""
    for k, v in meta.items():
        meta_text += f"{k}={v}\n"
    meta_bytes = meta_text.encode()

    out = b"APKG"                               # magic
    out += struct.pack("<HH", 1, 0)              # version, flags
    out += struct.pack("<I", len(meta_bytes))    # meta_len
    out += meta_bytes
    out += struct.pack("<I", len(files))         # nfiles

    for path, data in files:
        path_b = path.encode()
        out += struct.pack("<H", len(path_b))
        out += path_b
        out += struct.pack("<I", len(data))
        out += data

    return out


if __name__ == "__main__":
    # Example: package the 'grep' binary
    import hashlib

    with open("userland/bin/build/grep", "rb") as f:
        grep_data = f.read()

    checksum = "sha256:" + hashlib.sha256(grep_data).hexdigest()

    meta = {
        "name":           "grep",
        "version":        "1.0",
        "arch":           "x86_64",
        "description":    "Pattern search utility",
        "filename":       "grep-1.0-x86_64.apkg",
        "checksum":       checksum,
        "size":           len(grep_data),
        "installed_size": len(grep_data),
        "depends":        "",
        "provides":       "grep",
    }

    files = [("/bin/grep", grep_data)]
    pkg = build_apkg(meta, files)

    with open("grep-1.0-x86_64.apkg", "wb") as f:
        f.write(pkg)

    print(f"Built grep-1.0-x86_64.apkg ({len(pkg)} bytes)")
    print(f"Checksum: {checksum}")
```

---

## Setting up a repository

1. Create packages with the script above
2. Build an index.idx listing all packages
3. Host the files on a web server (HTTP)
4. Users add your repo to `/etc/apm/apm.conf`:
   ```
   repo myrepo http://your-server.com/apm/
   ```

> **Note:** Network downloads require TCP support in the OpenASD kernel.
> Until then, packages can be manually copied to `/var/apm/cache/` and
> installed from cache using `apm install <name>`.

---

## Manual package installation (offline)

When network is unavailable, install from a cached archive:

```sh
# Copy the .apkg file to the cache directory
# (on a live system, use another machine to download)

# The package must be listed in a repository index
# Tell apm to check the cache first — it will find the file
apm install grep

# Or for a completely offline workflow, place a minimal index.idx:
mkdir -p /var/apm/lists
cat > /var/apm/lists/local.idx << 'EOF'
repo=local
---
name=grep
version=1.0
arch=x86_64
description=Pattern search utility
filename=grep-1.0-x86_64.apkg
size=4704
installed_size=4704
depends=
---
EOF

# And add to config:
echo "repo local /var/apm/cache" >> /etc/apm/apm.conf
apm install grep
```
