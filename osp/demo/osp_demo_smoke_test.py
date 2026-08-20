#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Smoke test for osp_demo executable to prevent regression of startup crashes and command handling."""

import os
import signal
import subprocess
import sys
import time


def run_smoke_test(binary_path):
    if not os.path.exists(binary_path):
        print(f"Error: binary not found at {binary_path}")
        return False

    print(f"Testing {binary_path} --help...")
    res = subprocess.run([binary_path, "-h"], capture_output=True, text=True)
    if "usage:" not in res.stderr and "usage:" not in res.stdout:
        print("Error: help output unexpected.")
        return False

    fifo_path = "_recv_fifo"
    if os.path.exists(fifo_path):
        os.remove(fifo_path)
    os.mkfifo(fifo_path)

    try:
        print("Testing publisher demo startup (Receiver mode)...")
        read_fd = os.open(fifo_path, os.O_RDONLY | os.O_NONBLOCK)
        proc = subprocess.Popen(
            [binary_path, "SmokeTestServer"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        time.sleep(1.5)
        poll_res = proc.poll()
        if poll_res is not None:
            print(
                f"Error: publisher demo exited prematurely with code {poll_res}"
            )
            return False

        print("Testing interactive command ('avail')...")
        proc.stdin.write("avail\n")
        proc.stdin.flush()
        time.sleep(0.5)

        fifo_data = os.read(read_fd, 4096).decode("utf-8", errors="ignore")
        if "publisher->state()" not in fifo_data:
            print(
                "Error: interactive command 'avail' output not found in FIFO.")
            return False

        print("Publisher demo successfully processed interactive commands.")
        proc.stdin.close()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
        os.close(read_fd)
    finally:
        if os.path.exists(fifo_path):
            os.remove(fifo_path)

    cntl_fifo = "_cntl_fifo"
    if os.path.exists(cntl_fifo):
        os.remove(cntl_fifo)
    os.mkfifo(cntl_fifo)

    try:
        print("Testing listener demo startup (Controller mode)...")
        read_fd = os.open(cntl_fifo, os.O_RDONLY | os.O_NONBLOCK)
        proc = subprocess.Popen(
            [binary_path],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )

        time.sleep(1.5)
        poll_res = proc.poll()
        if poll_res is not None:
            print(
                f"Error: listener demo exited prematurely with code {poll_res}"
            )
            return False

        print("Listener demo started successfully without crashing.")
        proc.stdin.close()
        try:
            proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            proc.kill()
        os.close(read_fd)
    finally:
        if os.path.exists(cntl_fifo):
            os.remove(cntl_fifo)

    print("ALL SMOKE TESTS PASSED.")
    return True


if __name__ == "__main__":
    path = sys.argv[1] if len(sys.argv) > 1 else "./out/Default/osp_demo"
    success = run_smoke_test(path)
    sys.exit(0 if success else 1)
