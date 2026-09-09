#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Wrapper around build/util/lastchange.py that locates depot_tools."""

import os
import subprocess
import sys

_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.join(_REPO_ROOT, "build"))
import find_depot_tools

depot_tools_path = find_depot_tools.add_depot_tools_to_path()

env = os.environ.copy()
if depot_tools_path:
    existing_pythonpath = env.get("PYTHONPATH", "")
    env["PYTHONPATH"] = (
        f"{depot_tools_path}{os.pathsep}{existing_pythonpath}".rstrip(
            os.pathsep))

sys.exit(
    subprocess.call(
        [
            sys.executable,
            os.path.join(_REPO_ROOT, "build", "util", "lastchange.py"),
        ] + sys.argv[1:],
        env=env,
    ))
