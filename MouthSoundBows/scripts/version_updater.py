#!/usr/bin/env python3
"""
MouthSoundBows Version Updater

This script provides a complete solution for version management:
1. Bumps the version in src/mod.manifest (the single source of truth)
2. Updates the CHANGELOG.md file with new release information
3. Can be run manually or through GitHub Actions

Usage:
  python version_updater.py bump [major|minor|patch] [--changelog "Changelog entry"]
  python version_updater.py bump patch --title "Title" --changelog-file notes.txt
  python version_updater.py update-changelog --version X.Y.Z --title "Title" --changelog "Changelog entry"
"""

import re
import sys
from pathlib import Path
import argparse

# Base paths
BASE_DIR = Path(__file__).parent.parent
MANIFEST = BASE_DIR / "src" / "mod.manifest"
CHANGELOG_MD = BASE_DIR / "CHANGELOG.md"
VERSION_TAG = re.compile(r"<version>\s*(\d+)\.(\d+)\.(\d+)\s*</version>")

def get_current_version():
    """Parse mod.manifest to extract version information."""
    if not MANIFEST.exists():
        print(f"Error: {MANIFEST} not found.")
        sys.exit(1)
    match = VERSION_TAG.search(MANIFEST.read_text(encoding="utf-8"))
    if not match:
        print("Error: Cannot extract a X.Y.Z <version> from mod.manifest")
        sys.exit(1)
    return tuple(int(part) for part in match.groups())

def bump_version(part):
    """
    Bump the version in mod.manifest.

    Args:
        part: The part of the version to bump: "major", "minor", or "patch"
    """
    major, minor, patch = get_current_version()

    if part == "major":
        major, minor, patch = major + 1, 0, 0
    elif part == "minor":
        minor, patch = minor + 1, 0
    elif part == "patch":
        patch += 1
    else:
        print(f"Error: Invalid version part '{part}'. Use 'major', 'minor', or 'patch'.")
        sys.exit(1)

    version_str = f"{major}.{minor}.{patch}"
    manifest = MANIFEST.read_text(encoding="utf-8")
    MANIFEST.write_text(VERSION_TAG.sub(f"<version>{version_str}</version>", manifest, count=1), encoding="utf-8")
    print(f"Version bumped to {version_str}")
    return version_str

def update_changelog(version, title="", changelog_entry=""):
    """Update CHANGELOG.md with a new version entry and maintain proper structure."""
    changelog_template = """# Changelog

All notable changes to the MouthSoundBows mod will be documented in this file.

"""

    if not CHANGELOG_MD.exists():
        # Create a new CHANGELOG.md file with proper format
        content = changelog_template
    else:
        content = CHANGELOG_MD.read_text(encoding="utf-8")

        # Fix formatting to ensure the header is correct
        if "# Changelog" in content:
            # Extract everything after the title section
            if "All notable changes" in content:
                # Split at the "All notable changes" line to get the main content
                parts = content.split("All notable changes to the MouthSoundBows mod will be documented in this file.")
                if len(parts) > 1:
                    # Extract the main content (version entries and links)
                    main_content = parts[1].strip()

                    # Rebuild with proper header
                    content = changelog_template + main_content
                else:
                    # Couldn't split properly, rebuild from scratch
                    content = changelog_template
            else:
                # Missing "All notable changes" line, fix this
                content = changelog_template + content.split("# Changelog")[1].strip()

    # Add title suffix if provided
    version_header = f"## [{version}]"
    if title:
        version_header += f" - {title}"

    # Check if version already exists in changelog
    if f"## [{version}]" in content:
        print(f"Warning: Version {version} already exists in changelog, skipping update.")
        return

    # Find where to insert the new version entry - after the intro text and before the first version
    header_end_pos = content.find("All notable changes to the MouthSoundBows mod will be documented in this file.")
    if header_end_pos != -1:
        # Find the end of this line and the double newline that follows
        insert_position = content.find("\n\n", header_end_pos) + 2
    else:
        # Without the intro line, the entry goes after the "# Changelog" title.
        insert_position = content.find("\n\n", content.find("# Changelog")) + 2

    # Format the changelog entry
    if changelog_entry:
        # Format the changelog entry to match the style of existing entries
        # Replace any double line breaks with single line breaks within bullet points
        formatted_lines = []
        for line in changelog_entry.strip().split('\n'):
            # Keep indentation but remove extra blank lines
            line = line.rstrip()
            formatted_lines.append(line)

        # Join the lines with a single newline to match existing format
        formatted_entry = '\n'.join(formatted_lines)

        # Add the new version section with proper formatting
        new_version_section = f"{version_header}\n\n{formatted_entry}\n\n"

        # Insert the new version section
        updated_content = content[:insert_position] + new_version_section + content[insert_position:]

        # Extract existing version links (if any)
        version_links = []
        link_pattern = r'\[([0-9]+\.[0-9]+\.[0-9]+)\]: (.*)'
        for match in re.finditer(link_pattern, updated_content):
            version_links.append((match.group(1), match.group(2)))

        # Remove ALL existing version links using regex instead of trying to find the section start
        updated_content = re.sub(r'\n\[[0-9]+\.[0-9]+\.[0-9]+\]: .*', '', updated_content)

        # Add the new version's link when it is missing
        version_in_links = False
        for v, _ in version_links:
            if v == version:
                version_in_links = True
                break

        if not version_in_links:
            version_links.append((version, f"https://github.com/tkhquang/KCD2Tools/releases/tag/MouthSoundBows-v{version}"))

        # Sort version links by version number (newest first)
        version_links.sort(key=lambda x: [int(n) for n in x[0].split('.')], reverse=True)

        # Add all version links to the end
        updated_content = updated_content.rstrip() + "\n"
        for v, link in version_links:
            updated_content += f"\n[{v}]: {link}"
        updated_content += "\n"

        CHANGELOG_MD.write_text(updated_content, encoding="utf-8")
        print(f"Updated {CHANGELOG_MD} with version {version}")
    else:
        print(f"Warning: No changelog entry provided for version {version}, skipping update.")

def main():
    parser = argparse.ArgumentParser(description="MouthSoundBows Version Manager")
    subparsers = parser.add_subparsers(dest="command", help="Command to execute")

    # Bump command
    bump_parser = subparsers.add_parser("bump", help="Bump version and update changelog")
    bump_parser.add_argument("part", choices=["major", "minor", "patch"],
                            help="Version part to bump")
    bump_parser.add_argument("--title", help="Title for the version (e.g., 'Feature Update')")
    bump_parser.add_argument("--changelog", help="Changelog entry for the version")
    bump_parser.add_argument("--changelog-file", type=Path,
                             help="UTF-8 file containing the changelog entry (instead of --changelog)")

    # Update changelog command
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
            # A title with no changelog body writes no entry; warn rather than silently drop it.
            print("Warning: --title was given without --changelog, so no changelog entry was written.")
        print(f"\nSuccessfully bumped version to {version}")
    elif args.command == "update-changelog":
        if args.version:
            version = args.version
        else:
            major, minor, patch = get_current_version()
            version = f"{major}.{minor}.{patch}"
        update_changelog(version, args.title, args.changelog)
        print(f"\nSuccessfully updated changelog for version {version}")
    else:
        parser.print_help()

if __name__ == "__main__":
    main()
