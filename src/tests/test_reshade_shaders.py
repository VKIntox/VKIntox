#!/usr/bin/env python3
"""Compile every ReShade .fx shader in a tree or fetched EffectPackages set."""

import argparse
import configparser
import os
import shutil
import subprocess
import sys
import tempfile
import urllib.request
import zipfile
from pathlib import Path


DEFAULT_MANIFEST_URL = (
    "https://raw.githubusercontent.com/buwryme/VKIntox/refs/heads/main/assets/EffectPackages.ini"
)


def fetch_manifest(url: str, destination: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "VKIntox-tests"})
    with urllib.request.urlopen(request, timeout=60) as response, destination.open("wb") as output:
        shutil.copyfileobj(response, output)


def install_manifest_packages(manifest: Path, work_dir: Path, shader_dir: Path) -> None:
    config = configparser.ConfigParser(strict=False)
    config.read_string(manifest.read_text(encoding="utf-8-sig"))
    shader_dir.mkdir(parents=True, exist_ok=True)

    failures = []
    installed = 0
    for section in config.sections():
        package = config[section]
        name = package.get("PackageName", section)
        url = package.get("DownloadUrl")
        if not url:
            continue

        install_path = package.get("InstallPath", r".\reshade-shaders\Shaders")
        install_path = install_path.replace("\\", "/").lstrip("./")
        subdir = install_path.replace("reshade-shaders/Shaders", "", 1).strip("/")
        target_dir = shader_dir / subdir if subdir else shader_dir
        target_dir.mkdir(parents=True, exist_ok=True)

        allowed = {value.strip() for value in package.get("EffectFiles", "").split(",") if value.strip()}
        denied = {value.strip() for value in package.get("DenyEffectFiles", "").split(",") if value.strip()}
        archive_path = work_dir / f"{section}.zip"
        extract_dir = work_dir / section
        try:
            request = urllib.request.Request(url, headers={"User-Agent": "VKIntox-tests"})
            with urllib.request.urlopen(request, timeout=120) as response, archive_path.open("wb") as output:
                shutil.copyfileobj(response, output)
            extract_dir.mkdir(parents=True, exist_ok=True)
            with zipfile.ZipFile(archive_path) as archive:
                for member in archive.infolist():
                    member_path = Path(member.filename)
                    if member_path.is_absolute() or ".." in member_path.parts:
                        continue
                    if member.is_dir():
                        continue
                    filename = member_path.name
                    if filename in denied or not filename.endswith((".fx", ".fxh")):
                        continue
                    if filename.endswith(".fx") and allowed and filename not in allowed:
                        continue
                    output_path = extract_dir / filename
                    with archive.open(member) as source, output_path.open("wb") as output:
                        shutil.copyfileobj(source, output)

            copied = 0
            for source in extract_dir.iterdir():
                if source.is_file() and source.name.endswith((".fx", ".fxh")):
                    shutil.copy2(source, target_dir / source.name)
                    copied += 1
            if copied == 0:
                failures.append(f"{name}: package yielded no matching shader files")
            else:
                installed += 1
                print(f"fetched {name}: {copied} shader/include file(s)", flush=True)
        except Exception as error:  # Network and archive failures need package context.
            failures.append(f"{name}: {error}")

    if failures:
        raise RuntimeError("Package fetch failures:\n  " + "\n  ".join(failures))
    if installed == 0:
        raise RuntimeError("EffectPackages.ini did not provide any shader packages")


def compile_shaders(shader_check: Path, shader_dir: Path) -> int:
    shaders = sorted(shader_dir.rglob("*.fx"))
    if not shaders:
        print(f"No .fx shaders found under {shader_dir}", file=sys.stderr)
        return 1

    include_dirs = sorted({shader_dir, *(path.parent for path in shader_dir.rglob("*.fxh"))})
    command_prefix = [str(shader_check)]
    for include_dir in include_dirs:
        command_prefix.extend(["--include", str(include_dir)])

    failures = []
    print(f"compiling {len(shaders)} ReShade shader(s) from {shader_dir}", flush=True)
    for index, shader in enumerate(shaders, start=1):
        result = subprocess.run(
            [*command_prefix, str(shader)],
            check=False,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            details = (result.stdout + result.stderr).strip()
            failures.append((shader, details))
            print(f"failed [{index}/{len(shaders)}]: {shader}", flush=True)
        elif index % 25 == 0 or index == len(shaders):
            print(f"compiled [{index}/{len(shaders)}]", flush=True)

    if failures:
        print(f"\n{len(failures)} of {len(shaders)} shader(s) failed:", file=sys.stderr)
        for shader, details in failures:
            print(f"\n--- {shader} ---\n{details}", file=sys.stderr)
        return 1

    print(f"all {len(shaders)} ReShade shader(s) compiled successfully", flush=True)
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--shader-check", required=True, type=Path, help="vkintox-shader-check executable")
    parser.add_argument("--shader-dir", type=Path, help="use an existing installed shader tree")
    parser.add_argument("--manifest-url", default=os.environ.get("VKINTOX_EFFECT_PACKAGES_URL", DEFAULT_MANIFEST_URL))
    args = parser.parse_args()

    if not args.shader_check.is_file():
        parser.error(f"shader checker does not exist: {args.shader_check}")
    if args.shader_dir and not args.shader_dir.is_dir():
        parser.error(f"shader directory does not exist: {args.shader_dir}")

    with tempfile.TemporaryDirectory(prefix="vkintox-shader-tests-") as temporary:
        work_dir = Path(temporary)
        manifest = work_dir / "EffectPackages.ini"
        try:
            fetch_manifest(args.manifest_url, manifest)
        except Exception as error:
            print(f"Could not fetch EffectPackages.ini: {error}", file=sys.stderr)
            return 1

        shader_dir = args.shader_dir
        if shader_dir is None:
            shader_dir = work_dir / "Shaders"
            try:
                install_manifest_packages(manifest, work_dir, shader_dir)
            except Exception as error:
                print(error, file=sys.stderr)
                return 1

        return compile_shaders(args.shader_check.resolve(), shader_dir.resolve())


if __name__ == "__main__":
    raise SystemExit(main())
