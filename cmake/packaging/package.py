#!/usr/bin/env python3
"""Produce a native service package from the complete CMake install tree."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile


HERE = Path(__file__).resolve().parent


def run(*args, **kwargs):
    return subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def output(*args, **kwargs):
    return run(*args, stdout=subprocess.PIPE, text=True, **kwargs).stdout.strip()


def render(source, destination, **values):
    text = source.read_text(encoding="utf-8")
    for name, value in values.items():
        text = text.replace(f"@{name}@", str(value))
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(text, encoding="utf-8")


def worker_executables(stage, system):
    suffix = ".exe" if system == "Windows" else ""
    return [stage / f"bin/fughetta-service{suffix}",
            stage / f"libexec/fughetta-service/llvm/fughetta-llvm{suffix}"]


def worker_assets(stage, args, system):
    for binary in worker_executables(stage, system):
        target = args.output / f"{binary.stem}-{sys.platform}-{args.arch}{binary.suffix}"
        shutil.copy2(binary, target)
        if system != "Windows":
            clang = os.environ.get("CLANG_ROOT")
            strip = Path(clang) / "bin/llvm-strip" if clang else "llvm-strip"
            run(strip, target)
        if system == "Darwin":
            run("codesign", "--force", "--sign", "-", target)


def runtime(stage, scratch, system, search):
    suffix = ".exe" if system == "Windows" else ""
    executables = worker_executables(stage, system)
    dependencies = scratch / "runtime.txt"
    run("cmake", f"-DSTAGE={stage.as_posix()}", f"-DSUFFIX={suffix}",
        f"-DSEARCH={';'.join(search)}", f"-DOUTPUT={dependencies.as_posix()}",
        "-P", HERE / "runtime.cmake")
    libraries = []
    for line in dependencies.read_text().splitlines():
        if not line:
            continue
        path = Path(line)
        if system == "Linux" and re.fullmatch(
                r"(?:ld-linux[^/]*|lib(?:c|m|dl|pthread|rt|resolv|util|anl)\.so\..*)", path.name):
            continue
        if system == "Darwin" and str(path).startswith(("/usr/lib/", "/System/Library/")):
            continue
        if system == "Windows":
            windows = Path(os.environ["SystemRoot"]).resolve()
            if path.resolve().is_relative_to(windows) and not path.name.lower().startswith(("msvcp", "vcruntime", "concrt")):
                continue
        libraries.append(path)

    destination = stage / "libexec/fughetta-service/runtime"
    destination.mkdir(parents=True, exist_ok=True)
    copied = []
    for library in libraries:
        target = destination / library.name
        shutil.copy2(library, target)
        copied.append(target)
    if system == "Linux":
        # DT_RUNPATH is relative to each object, including transitive libraries.
        for binary in executables + copied:
            relative = os.path.relpath(destination, binary.parent)
            run("patchelf", "--set-rpath", f"$ORIGIN/{relative}", binary)
    elif system == "Darwin":
        for binary in executables + copied:
            relative = os.path.relpath(destination, binary.parent)
            linked = output("otool", "-L", binary).splitlines()[1:]
            for line in linked:
                name = line.strip().split(" (", 1)[0]
                if Path(name).name in {p.name for p in libraries}:
                    run("install_name_tool", "-change", name,
                        f"@loader_path/{relative}/{Path(name).name}", binary)
            if binary in copied:
                run("install_name_tool", "-id", f"@rpath/{binary.name}", binary)
            run("codesign", "--force", "--sign", "-", binary)
    else:
        # Windows searches the executable's own directory for its DLLs.
        for binary in executables:
            for library in copied:
                shutil.copy2(library, binary.parent / library.name)
        shutil.rmtree(destination)


def deb(stage, scratch, args):
    root = scratch / "deb"
    root.mkdir()
    shutil.move(stage, root / "usr")
    control = root / "DEBIAN"
    control.mkdir()
    architecture = output("dpkg", "--print-architecture")
    # All non-system native libraries are bundled. Obtain the libc dependency
    # from the symbols used by those libraries and the two executables.
    metadata = scratch / "debian"
    metadata.mkdir()
    (metadata / "control").write_text("Source: fughetta-service\n\nPackage: fughetta-service\nArchitecture: any\n")
    native = [root / "usr/bin/fughetta-service",
              root / "usr/libexec/fughetta-service/llvm/fughetta-llvm"]
    libraries = root / "usr/libexec/fughetta-service/runtime"
    native.extend(libraries.iterdir())
    dependencies = output("dpkg-shlibdeps", "-O", "--ignore-missing-info", f"-l{libraries}",
                          *(f"-e{path}" for path in native), cwd=scratch)
    dependencies = dependencies.removeprefix("shlibs:Depends=").split(", ")
    libc = next(dependency for dependency in dependencies if re.match(r"libc6(?:\s|$)", dependency))
    size = sum(p.stat().st_size for p in root.rglob("*") if p.is_file()) // 1024
    (control / "control").write_text(
        f"Package: fughetta-service\nVersion: {args.version}\nArchitecture: {architecture}\n"
        "Maintainer: FantasticFugue\nSection: devel\nPriority: optional\n"
        f"Installed-Size: {size}\nDepends: {libc}, adduser, init-system-helpers\n"
        "Homepage: https://github.com/FantasticFugue/fughetta-service\n"
        "Description: Workspace source, semantic analysis, and build service\n"
        " Includes the default workspace service and language provider hosts.\n",
        encoding="utf-8")
    for script in (HERE / "deb").iterdir():
        target = control / script.name
        shutil.copy2(script, target)
        target.chmod(0o755)
    render(HERE.parent / "daemon/fughetta-service.service.in",
           root / "usr/lib/systemd/system/fughetta-service@.service",
           FUGHETTA_SERVICE_USER="fughetta", CMAKE_INSTALL_FULL_BINDIR="/usr/bin",
           CMAKE_INSTALL_FULL_SYSCONFDIR="/etc")
    render(HERE / "config.json.in", root / "usr/share/fughetta-service/default.json",
           PREFIX="/usr", STATE="/var/lib/fughetta-service/default", EXE="")
    run("dpkg-deb", "--root-owner-group", "--build", root,
        args.output / f"fughetta-service_{args.version}_{architecture}.deb")


def msi(stage, scratch, args):
    shutil.copy2(HERE / "windows/initialize.ps1", stage / "share/fughetta-service/initialize.ps1")
    run("wix", "build", HERE / "windows/package.wxs", "-arch", args.arch,
        "-d", f"Stage={stage.as_posix()}", "-d", f"Version={args.version}",
        "-ext", "WixToolset.Util.wixext", "-intermediateFolder", scratch / "wix", "-pdbtype", "none",
        "-out", args.output / f"fughetta-service-{args.version}-{args.arch}.msi")


def homebrew(stage, scratch, args):
    archive = args.output / f"fughetta-service-{args.version}-{sys.platform}-{args.arch}.tar.gz"
    with tarfile.open(archive, "w:gz") as package:
        for child in sorted(stage.iterdir()):
            package.add(child, arcname=child.name)
    # Homebrew requires a digest in its download metadata.
    with archive.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    render(HERE / "homebrew/fughetta-service.rb.in", args.output / "fughetta-service.rb",
           URL=f"{args.release_url.rstrip('/')}/{archive.name}", VERSION=args.version, SHA256=digest)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, default=Path("dist"))
    version = parser.add_mutually_exclusive_group(required=True)
    version.add_argument("--version")
    version.add_argument("--build-number", help="Patch number for the configured CMake project version")
    parser.add_argument("--worker-assets", action="store_true", help="Also emit the standalone worker binaries")
    parser.add_argument("--release-url", help="Download directory of the release; required on macOS")
    parser.add_argument("--runtime-search", action="append", default=[], help="Native library search directory")
    args = parser.parse_args()
    if args.build_number is not None:
        cache = (args.build / "CMakeCache.txt").read_text(encoding="utf-8")
        project_version = re.search(r"^CMAKE_PROJECT_VERSION:STATIC=(\d+\.\d+)\.\d+$", cache, re.MULTILINE)
        if not project_version:
            parser.error("the CMake build directory must contain a configured project version")
        args.version = f"{project_version[1]}.{args.build_number}"
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
        parser.error("--version must have three numeric components")
    major, minor, patch = map(int, args.version.split("."))
    if major > 255 or minor > 255 or patch > 65535:
        parser.error("package versions must fit MSI's 255.255.65535 fields")
    system = platform.system()
    machine = platform.machine().lower()
    if (system, machine) not in {("Linux", "x86_64"), ("Windows", "amd64"), ("Darwin", "arm64")}:
        parser.error("packages are built on Linux x64, Windows x64, or macOS arm64")
    args.arch = {"x86_64": "x64", "amd64": "x64"}.get(machine, machine)
    if system == "Darwin" and not args.release_url:
        parser.error("--release-url is required for the Homebrew formula")
    args.build = args.build.resolve()
    args.output = args.output.resolve()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="package-", dir=args.build) as temporary:
        scratch = Path(temporary)
        stage = scratch / "install"
        run("cmake", "--install", args.build, "--prefix", stage)
        # The package managers own service registration; source installs keep
        # using the templates supplied by the existing CMake installation.
        shutil.rmtree(stage / "share/fughetta-service/daemon", ignore_errors=True)
        share = stage / "share/fughetta-service"
        share.mkdir(parents=True, exist_ok=True)
        shutil.copy2(HERE / "config.json.in", share / "config.json.in")
        # A package must include every provider referenced by its initial config.
        config = json.loads((HERE / "config.json.in").read_text())
        for path in config["tools"].values():
            path = path.replace("@PREFIX@", str(stage)).replace("@EXE@", ".exe" if system == "Windows" else "")
            if not Path(path).is_file():
                raise FileNotFoundError(f"Build all language providers before packaging: {path}")
        runtime_search = args.runtime_search
        if compiler_runtime := os.environ.get("CLANG_RUNTIME_DIR"):
            runtime_search = [*runtime_search, compiler_runtime]
        elif clang := os.environ.get("CLANG_ROOT"):
            runtime_search = [*runtime_search, (Path(clang) / "lib").as_posix()]
        if args.worker_assets:
            worker_assets(stage, args, system)
        runtime(stage, scratch, system, runtime_search)
        {"Linux": deb, "Windows": msi, "Darwin": homebrew}[system](stage, scratch, args)


if __name__ == "__main__":
    main()
