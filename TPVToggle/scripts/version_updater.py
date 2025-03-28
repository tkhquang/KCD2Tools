#!/usr/bin/env python3
"""
TPVToggle Version Updater

This script provides a complete solution for version management:
1. Bumps the version in version.h (the single source of truth)
2. Updates the CHANGELOG.md file with new release information
3. Updates version in README.txt
4. Can be run manually or through GitHub Actions

Usage:
  python version_updater.py bump [major|minor|patch] [--changelog "Changelog entry"]
  python version_updater.py update-changelog --version X.Y.Z --title "Title" --changelog "Changelog entry"
"""

import re
import os
import sys
import subprocess
from pathlib import Path
import argparse
from datetime import datetime

# Base paths
BASE_DIR = Path(__file__).parent.parent
TPVTOGGLE_DIR = BASE_DIR / "TPVToggle"
VERSION_HEADER = TPVTOGGLE_DIR / "version.h"
CHANGELOG_MD = TPVTOGGLE_DIR / "CHANGELOG.md"
README_TXT = TPVTOGGLE_DIR / "README.txt"

def get_current_version():
    """Parse version.h to extract version information."""
    if not VERSION_HEADER.exists():
        print(f"Error: {VERSION_HEADER} not found.")
        sys.exit(1)

    version_h = VERSION_HEADER.read_text()

    # Extract version components using defines
    major_match = re.search(r'#define\s+VERSION_MAJOR\s+(\d+)', version_h)
    minor_match = re.search(r'#define\s+VERSION_MINOR\s+(\d+)', version_h)
    patch_match = re.search(r'#define\s+VERSION_PATCH\s+(\d+)', version_h)

    if not (major_match and minor_match and patch_match):
        print("Error: Could not extract version information from version.h")
        sys.exit(1)

    major = int(major_match.group(1))
    minor = int(minor_match.group(1))
    patch = int(patch_match.group(1))

    return (major, minor, patch)

def bump_version(part):
    """
    Bump the version in version.h.

    Args:
        part: The part of the version to bump: "major", "minor", or "patch"
    """
    major, minor, patch = get_current_version()

    # Bump the specified part
    if part == "major":
        major += 1
        minor = 0
        patch = 0
    elif part == "minor":
        minor += 1
        patch = 0
    elif part == "patch":
        patch += 1
    else:
        print(f"Error: Invalid version part '{part}'. Use 'major', 'minor', or 'patch'.")
        sys.exit(1)

    # Update version.h
    version_h = VERSION_HEADER.read_text()

    # Update the defines
    version_h = re.sub(
        r'#define\s+VERSION_MAJOR\s+\d+',
        f'#define VERSION_MAJOR {major}',
        version_h
    )
    version_h = re.sub(
        r'#define\s+VERSION_MINOR\s+\d+',
        f'#define VERSION_MINOR {minor}',
        version_h
    )
    version_h = re.sub(
        r'#define\s+VERSION_PATCH\s+\d+',
        f'#define VERSION_PATCH {patch}',
        version_h
    )

    VERSION_HEADER.write_text(version_h)

    # Return the new version string
    version_str = f"{major}.{minor}.{patch}"
    print(f"Version bumped to {version_str}")
    return version_str

def update_readme_txt(version):
    """Update version in README.txt."""
    if not README_TXT.exists():
        print(f"Warning: {README_TXT} not found, skipping.")
        return

    content = README_TXT.read_text()

    # Update version in header
    content = re.sub(
        r'(KINGDOM COME: DELIVERANCE II - THIRD PERSON VIEW TOGGLE\nVersion )[0-9]+\.[0-9]+\.[0-9]+',
        f'\\1{version}',
        content
    )

    README_TXT.write_text(content)
    print(f"Updated {README_TXT}")

def update_changelog(version, title="", changelog_entry=""):
    """Update CHANGELOG.md with a new version entry."""
    if not CHANGELOG_MD.exists():
        # Create a new CHANGELOG.md file if it doesn't exist
        content = "# Changelog\n\nAll notable changes to the TPVToggle mod will be documented in this file.\n\n"
    else:
        content = CHANGELOG_MD.read_text()

    # Add title suffix if provided
    version_header = f"## [{version}]"
    if title:
        version_header += f" - {title}"

    # Check if version already exists in changelog
    if f"## [{version}]" in content:
        print(f"Warning: Version {version} already exists in changelog, skipping update.")
        return

    # Add new version entry after the header
    header_end = content.find("\n\n", content.find("# Changelog"))
    if header_end == -1:  # If there's no blank line after the header
        header_end = content.find("\n", content.find("# Changelog"))
        if header_end == -1:  # If there's no header at all
            header_end = 0

    # Format the changelog entry
    if changelog_entry:
        # Ensure the changelog_entry is properly formatted with newlines and indentation
        formatted_entry = changelog_entry.strip()

        # Construct the new version section
        new_version_section = f"{version_header}\n\n{formatted_entry}\n\n"

        # Insert the new version section after the header
        updated_content = content[:header_end+2] + new_version_section + content[header_end+2:]

        CHANGELOG_MD.write_text(updated_content)
        print(f"Updated {CHANGELOG_MD} with version {version}")
    else:
        print(f"Warning: No changelog entry provided for version {version}, skipping update.")

def main():
    parser = argparse.ArgumentParser(description="TPVToggle Version Manager")
    subparsers = parser.add_subparsers(dest="command", help="Command to execute")

    # Bump command
    bump_parser = subparsers.add_parser("bump", help="Bump version and update changelog")
    bump_parser.add_argument("part", choices=["major", "minor", "patch"],
                            help="Version part to bump")
    bump_parser.add_argument("--title", help="Title for the version (e.g., 'Feature Update')")
    bump_parser.add_argument("--changelog", help="Changelog entry for the version")

    # Update changelog command
    changelog_parser = subparsers.add_parser("update-changelog", help="Update changelog with current version")
    changelog_parser.add_argument("--version", help="Version to use (defaults to current version)")
    changelog_parser.add_argument("--title", help="Title for the version (e.g., 'Feature Update')")
    changelog_parser.add_argument("--changelog", required=True, help="Changelog entry for the version")

    args = parser.parse_args()

    if args.command == "bump":
        version = bump_version(args.part)
        # Update README.txt with the new version
        update_readme_txt(version)
        if args.changelog:
            update_changelog(version, args.title, args.changelog)
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
