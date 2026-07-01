#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

import argparse
import re
import sys


def main():
    parser = argparse.ArgumentParser(
        description=
        "Generate tinycbor-version.h from template and CMakeLists.txt")
    parser.add_argument("--template",
                        required=True,
                        help="Path to tinycbor-version.h.in")
    parser.add_argument("--cmakelists",
                        required=True,
                        help="Path to CMakeLists.txt")
    parser.add_argument("--output",
                        required=True,
                        help="Path to output tinycbor-version.h")
    args = parser.parse_args()

    # Extract version from CMakeLists.txt
    # e.g. project(tinycbor LANGUAGES C VERSION 7.0)
    with open(args.cmakelists, "r", encoding="utf-8") as f:
        cmake_content = f.read()

    match = re.search(r"project\(\s*tinycbor\b.*?VERSION\s+(\d+)\.(\d+)",
                      cmake_content, re.IGNORECASE | re.DOTALL)
    if not match:
        sys.stderr.write(
            f"Error: Could not parse VERSION from {args.cmakelists}\n")
        return 1

    version_major = match.group(1)
    version_minor = match.group(2)

    # Read template
    with open(args.template, "r", encoding="utf-8") as f:
        template_content = f.read()

    # Substitute placeholders
    result_content = template_content.replace("@PROJECT_VERSION_MAJOR@",
                                              version_major).replace(
                                                  "@PROJECT_VERSION_MINOR@",
                                                  version_minor)

    # Write output
    with open(args.output, "w", encoding="utf-8") as f:
        f.write(result_content)

    return 0


if __name__ == "__main__":
    sys.exit(main())
