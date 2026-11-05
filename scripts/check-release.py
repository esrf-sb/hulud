"""Check release readiness, write release_notes.md and print the release tag."""

import os
from pathlib import Path
import re
import subprocess
import tomllib


def main():
    manifest = tomllib.loads(Path("pixi.toml").read_text())
    version = manifest["workspace"]["version"]
    version_numbers = _parse_version(version)
    api_version = _read_api_version()
    changelog_version, release_notes = _read_changelog_release()

    if version != api_version or version != changelog_version:
        raise SystemExit(
            "Release versions must match: "
            f"pixi.toml={version}, plugin_api.h={api_version}, "
            f"CHANGELOG.md={changelog_version}"
        )
    if not release_notes:
        raise SystemExit(f"CHANGELOG.md has no release notes for {version}.")

    tag = f"v{version}"
    _check_existing_tags(version_numbers)
    _check_existing_releases(tag)

    Path("release_notes.md").write_text(release_notes + "\n")
    print(tag)


def _parse_version(version):
    """Read a stable X.Y.Z version as three integers for numeric comparison."""
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise SystemExit(f"Expected an X.Y.Z release version, got {version!r}.")
    return tuple(int(part) for part in version.split("."))


def _read_api_version():
    defines = {}
    for line in Path("include/hulud/plugin_api.h").read_text().splitlines():
        if line.startswith("#define VERSION_"):
            _, name, value, *_ = line.split()
            defines[name] = int(value)
    return (
        f"{defines['VERSION_MAJOR']}."
        f"{defines['VERSION_MINOR']}."
        f"{defines['VERSION_PATCH']}"
    )


def _read_changelog_release():
    """Read the newest versioned section, allowing an Unreleased section above it."""
    sections = Path("CHANGELOG.md").read_text().split("\n## [")[1:]
    for section in sections:
        heading, _, notes = section.partition("\n")
        version = heading.split("]", 1)[0]
        if version == "Unreleased":
            continue
        return version, notes.strip()
    raise SystemExit("CHANGELOG.md has no versioned release section.")


def _check_existing_tags(version_numbers):
    result = subprocess.run(
        ["git", "tag", "--list", "v*"], check=True, capture_output=True, text=True
    )
    for tag in result.stdout.splitlines():
        if re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
            if version_numbers <= _parse_version(tag[1:]):
                raise SystemExit(f"Release version must be newer than existing tag {tag}.")


def _check_existing_releases(tag):
    repository = os.environ["GH_REPO"]
    result = subprocess.run(
        ["gh", "api", "--paginate", f"repos/{repository}/releases", "--jq", ".[].tag_name"],
        check=True,
        capture_output=True,
        text=True,
    )
    if tag in result.stdout.splitlines():
        raise SystemExit(f"A GitHub Release already exists for {tag}.")


if __name__ == "__main__":
    main()
