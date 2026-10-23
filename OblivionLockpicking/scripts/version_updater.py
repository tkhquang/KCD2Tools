#!/usr/bin/env python3
"""OblivionLockpicking version manager.

Bumps the version in src/version.hpp, the single source of truth, and in the pak's mod.manifest, and adds release
entries to CHANGELOG.md. The release workflow runs it, and it also runs by hand.

Usage:
  python version_updater.py bump [major|minor|patch] [--changelog "Changelog entry"]
  python version_updater.py bump patch --title "Title" --changelog-file notes.txt
  python version_updater.py update-changelog --version X.Y.Z --title "Title" --changelog "Changelog entry"
"""

import argparse
import re
import sys
from pathlib import Path

BASE_DIR = Path(__file__).parent.parent
VERSION_HEADER = BASE_DIR / "src" / "version.hpp"
CHANGELOG_MD = BASE_DIR / "CHANGELOG.md"
MANIFEST = BASE_DIR / "pack" / "mod.manifest"
CHANGELOG_INTRO = "All notable changes to the OblivionLockpicking mod will be documented in this file."
CHANGELOG_TEMPLATE = f"# Changelog\n\n{CHANGELOG_INTRO}\n\n"
RELEASE_URL = "https://github.com/tkhquang/KCD2Tools/releases/tag/OblivionLockpicking-v{}"
VERSION_PARTS = ("MAJOR", "MINOR", "PATCH")


def get_current_version():
    """The (major, minor, patch) of version.hpp. Exits when the header or a macro is missing."""
    if not VERSION_HEADER.exists():
        sys.exit(f"Error: {VERSION_HEADER} not found.")
    version_h = VERSION_HEADER.read_text(encoding="utf-8")
    matches = [re.search(rf"#define\s+VERSION_{part}\s+(\d+)", version_h) for part in VERSION_PARTS]
    if not all(matches):
        sys.exit("Error: version.hpp lacks VERSION_MAJOR, VERSION_MINOR or VERSION_PATCH.")
    return tuple(int(match.group(1)) for match in matches)


def bump_version(part):
    """Bumps @p part ("major", "minor" or "patch") in version.hpp and mod.manifest, and returns the new version."""
    major, minor, patch = get_current_version()
    if part == "major":
        major, minor, patch = major + 1, 0, 0
    elif part == "minor":
        minor, patch = minor + 1, 0
    else:
        patch += 1
    version_str = f"{major}.{minor}.{patch}"

    # Both files are read before either is written, so a missing manifest leaves no half-done bump.
    version_h = VERSION_HEADER.read_text(encoding="utf-8")
    manifest = MANIFEST.read_text(encoding="utf-8")
    for name, value in zip(VERSION_PARTS, (major, minor, patch)):
        version_h = re.sub(rf"#define\s+VERSION_{name}\s+\d+", f"#define VERSION_{name} {value}", version_h)
    manifest = re.sub(r"<version>[^<]*</version>", f"<version>{version_str}</version>", manifest, count=1)
    VERSION_HEADER.write_text(version_h, encoding="utf-8")
    MANIFEST.write_text(manifest, encoding="utf-8")
    print(f"Version bumped to {version_str}")
    return version_str


def normalized_changelog():
    """CHANGELOG.md with its title and intro line in place, or the bare template when the file is missing."""
    if not CHANGELOG_MD.exists():
        return CHANGELOG_TEMPLATE
    content = CHANGELOG_MD.read_text(encoding="utf-8")
    if "# Changelog" not in content:
        return content
    if CHANGELOG_INTRO in content:
        return CHANGELOG_TEMPLATE + content.split(CHANGELOG_INTRO, 1)[1].strip()
    return CHANGELOG_TEMPLATE + content.split("# Changelog", 1)[1].strip()


def update_changelog(version, title="", changelog_entry=""):
    """Inserts a CHANGELOG.md section for @p version above the previous ones and rewrites the version links."""
    if not changelog_entry:
        print(f"Warning: No changelog entry provided for version {version}, skipping update.")
        return
    content = normalized_changelog()
    if f"## [{version}]" in content:
        print(f"Warning: Version {version} already exists in changelog, skipping update.")
        return

    version_header = f"## [{version}]" + (f" - {title}" if title else "")
    intro_at = content.find(CHANGELOG_INTRO)
    anchor = intro_at if intro_at != -1 else content.find("# Changelog")
    insert_position = content.find("\n\n", anchor) + 2
    entry = "\n".join(line.rstrip() for line in changelog_entry.strip().split("\n"))
    updated = content[:insert_position] + f"{version_header}\n\n{entry}\n\n" + content[insert_position:]

    # The links move to the end of the file, newest first, with the new version's release added.
    links = dict(re.findall(r"\[([0-9]+\.[0-9]+\.[0-9]+)\]: (.*)", updated))
    links.setdefault(version, RELEASE_URL.format(version))
    updated = re.sub(r"\n\[[0-9]+\.[0-9]+\.[0-9]+\]: .*", "", updated).rstrip() + "\n"
    for number in sorted(links, key=lambda v: [int(n) for n in v.split(".")], reverse=True):
        updated += f"\n[{number}]: {links[number]}"
    CHANGELOG_MD.write_text(updated + "\n", encoding="utf-8")
    print(f"Updated {CHANGELOG_MD} with version {version}")


def main():
    parser = argparse.ArgumentParser(description="OblivionLockpicking Version Manager")
    subparsers = parser.add_subparsers(dest="command", help="Command to execute")

    bump_parser = subparsers.add_parser("bump", help="Bump version and update changelog")
    bump_parser.add_argument("part", choices=["major", "minor", "patch"], help="Version part to bump")
    bump_parser.add_argument("--title", help="Title for the version (e.g., 'Feature Update')")
    bump_parser.add_argument("--changelog", help="Changelog entry for the version")
    bump_parser.add_argument("--changelog-file", type=Path,
                             help="UTF-8 file containing the changelog entry (instead of --changelog)")

    changelog_parser = subparsers.add_parser("update-changelog", help="Update changelog with current version")
    changelog_parser.add_argument("--version", help="Version to use (defaults to current version)")
    changelog_parser.add_argument("--title", help="Title for the version (e.g., 'Feature Update')")
    changelog_parser.add_argument("--changelog", help="Changelog entry for the version")
    changelog_parser.add_argument("--changelog-file", type=Path,
                                  help="UTF-8 file containing the changelog entry (instead of --changelog)")

    args = parser.parse_args()

    if args.command:
        if args.changelog is not None and args.changelog_file is not None:
            parser.error("Use either --changelog or --changelog-file, not both")
        if args.changelog_file is not None:
            try:
                args.changelog = args.changelog_file.read_text(encoding="utf-8-sig").strip()
            except (OSError, UnicodeError) as error:
                parser.error(f"Cannot read changelog file: {error}")
            if not args.changelog:
                parser.error("Changelog file must not be empty")
        if args.command == "update-changelog" and not args.changelog:
            parser.error("update-changelog requires --changelog or --changelog-file")
        if args.command == "update-changelog" and args.version:
            if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", args.version):
                parser.error("--version must use the X.Y.Z format")

    if args.command == "bump":
        version = bump_version(args.part)
        if args.changelog:
            update_changelog(version, args.title, args.changelog)
        elif args.title:
            print("Warning: --title was given without --changelog, so no changelog entry was written.")
        print(f"\nSuccessfully bumped version to {version}")
    elif args.command == "update-changelog":
        version = args.version or "{}.{}.{}".format(*get_current_version())
        update_changelog(version, args.title, args.changelog)
        print(f"\nSuccessfully updated changelog for version {version}")
    else:
        parser.print_help()


if __name__ == "__main__":
    main()
