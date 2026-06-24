# Building Custom Packages (.apkg) for OpenASD APM

The APM (ASD Package Manager) uses the `.apkg` binary archive format for its packages. OpenASD provides a built-in Python script to package compiled programs into these archives.

## Step 1: Prepare the Binary
First, you need to write your program (for example, in the `userland/my_program` directory) and set up its build system (e.g., a `Makefile`). The build process should output an executable file or a library.

## Step 2: Add the Program to the Build Script
The automatic generation of `.apkg` packages is handled by the `scripts/build_packages.py` Python script.

Open `scripts/build_packages.py` in the root of the source tree and locate the `PACKAGES` array. Add a tuple containing your program's information. The format is as follows:

```python
PACKAGES = [
    # (Package_Name, Install_Path_in_OS, Source_Binary_Path, "Description", [Dependencies]),
    ("my_program", "/bin/my_program", "userland/my_program/build/my_program", "My custom program for OpenASD", []),
]
```

Parameters:
- **Package_Name** — The internal name of the package (e.g., `mifetch`).
- **Install_Path_in_OS** — The absolute path where the binary will be placed in the installed system (most commonly `/bin/program_name`).
- **Source_Binary_Path** — The relative path from the repository root to your compiled file.
- **Description** — A short text description displayed by the `apm info` command.
- **Dependencies** — A list of other packages that your program depends on (an array of strings; use `[]` if there are none).

## Step 3: Build
Ensure that your program has been compiled successfully (the path specified in the `PACKAGES` array must exist).

Run the script from the root of the repository:
```bash
python3 scripts/build_packages.py
```

## Step 4: Result and Distribution
After the script finishes, you will find the following files in the `build/packages/` directory:
- The generated package: `my_program-1.0-x86_64.apkg`
- The updated repository index file: `index.idx`

These files can then be copied to a USB drive (for offline installation via `/var/apm/cache/`) or uploaded to a GitHub repository release. Once uploaded, users can download and install them over the network using `apm update` and `apm install my_program`.
