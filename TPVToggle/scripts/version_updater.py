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
VERSION_HEADER = BASE_DIR / "src" / "version.h"
CHANGELOG_MD = BASE_DIR / "CHANGELOG.md"
README_TXT = BASE_DIR / "build" / "README.txt"

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
    """Update version in README.txt using string operations instead of regex."""
    if not README_TXT.exists():
        print(f"Warning: {README_TXT} not found, skipping.")
        return

    try:
        content = README_TXT.read_text()

        # Look for the line containing "Version" and update it
        lines = content.splitlines()
        for i, line in enumerate(lines):
            if line.startswith("Version "):
                lines[i] = f"Version {version}"
                print(f"Updated version line from '{line}' to '{lines[i]}'")
                break

        # Reconstruct the content with updated version
        updated_content = "\n".join(lines)
        README_TXT.write_text(updated_content)
        print(f"Updated {README_TXT}")

    except Exception as e:
        print(f"Error updating README.txt: {str(e)}")
        # Print the exception traceback for debugging
        import traceback
        traceback.print_exc()

def update_changelog(version, title="", changelog_entry=""):
    """Update CHANGELOG.md with a new version entry and maintain proper structure."""
    changelog_template = """# Changelog

All notable changes to the TPVToggle mod will be documented in this file.

"""

    if not CHANGELOG_MD.exists():
        # Create a new CHANGELOG.md file with proper format
        content = changelog_template
    else:
        content = CHANGELOG_MD.read_text()

        # Fix formatting to ensure the header is correct
        if "# Changelog" in content:
            # Extract everything after the title section
            if "All notable changes" in content:
                # Split at the "All notable changes" line to get the main content
                parts = content.split("All notable changes to the TPVToggle mod will be documented in this file.")
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
    header_end_pos = content.find("All notable changes to the TPVToggle mod will be documented in this file.")
    if header_end_pos != -1:
        # Find the end of this line and the double newline that follows
        insert_position = content.find("\n\n", header_end_pos) + 2
    else:
        # Fallback, should not happen with the fixes we made above
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

        # Add our version link if it doesn't exist
        version_in_links = False
        for v, _ in version_links:
            if v == version:
                version_in_links = True
                break

        if not version_in_links:
            version_links.append((version, f"https://github.com/tkhquang/KCD2Tools/releases/tag/TPVToggle-v{version}"))

        # Sort version links by version number (newest first)
        version_links.sort(key=lambda x: [int(n) for n in x[0].split('.')], reverse=True)

        # Add all version links to the end
        updated_content = updated_content.rstrip() + "\n"
        for v, link in version_links:
            updated_content += f"\n[{v}]: {link}"
        updated_content += "\n"

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
