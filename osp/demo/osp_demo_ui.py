#!/usr/bin/env python3
# Copyright 2026 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""ncurses split-screen terminal UI wrapper for osp_demo.

Splits the terminal into a top scrolling log panel and a bottom command prompt.
Works regardless of the invocation directory and supports configurable out/ dirs.
"""

import argparse
import curses
import os
import select
import subprocess
import sys
import time


def curses_main(stdscr, repo_root, binary_path, demo_args):
    try:
        curses.curs_set(1)
    except curses.error:
        pass
    try:
        curses.use_default_colors()
    except curses.error:
        pass
    curses.init_pair(1, curses.COLOR_CYAN, -1)
    curses.init_pair(2, curses.COLOR_RED, -1)
    curses.init_pair(3, curses.COLOR_GREEN, -1)

    is_receiver = len(demo_args) > 0
    fifo_name = "_recv_fifo" if is_receiver else "_cntl_fifo"
    fifo_path = os.path.join(repo_root, fifo_name)

    if os.path.exists(fifo_path):
        try:
            os.remove(fifo_path)
        except OSError:
            pass
    os.mkfifo(fifo_path)

    fifo_fd = os.open(fifo_path, os.O_RDONLY | os.O_NONBLOCK)

    proc = subprocess.Popen(
        [binary_path] + demo_args,
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        text=True,
        bufsize=1,
        cwd=repo_root,
    )

    height, width = stdscr.getmaxyx()
    log_height = height - 3

    log_win = curses.newwin(log_height, width, 0, 0)
    log_win.scrollok(True)
    log_win.idlok(True)

    sep_win = curses.newwin(1, width, log_height, 0)
    sep_win.addstr(0, 0, "─" * (width - 1), curses.A_DIM)
    sep_win.refresh()

    prompt_str = f"[{'Receiver: ' + demo_args[0] if is_receiver else 'Controller'}] $ "
    input_win = curses.newwin(2, width, log_height + 1, 0)
    input_win.keypad(True)
    input_win.nodelay(True)

    input_buf = ""

    help_hint = (
        "Type 'quit' or 'exit' to quit. Commands: avail, msg, close, term"
        if is_receiver else
        "Type 'quit' or 'exit' to quit. Commands: avail <url>, connect, start, msg, close, reconnect, term"
    )

    def redraw_input():
        input_win.erase()
        input_win.addstr(0, 0, prompt_str,
                         curses.color_pair(1) | curses.A_BOLD)
        input_win.addstr(0, len(prompt_str), input_buf)
        input_win.addstr(1, 0, help_hint, curses.A_DIM)
        input_win.move(0, len(prompt_str) + len(input_buf))
        input_win.refresh()

    redraw_input()
    log_win.addstr("=== OSP Demo Terminal UI Started ===\n",
                   curses.color_pair(3))
    log_win.addstr(f"Binary: {binary_path}\nWorking Dir: {repo_root}\n\n",
                   curses.A_DIM)
    log_win.refresh()

    buffer_leftover = ""

    while True:
        if proc.poll() is not None:
            log_win.addstr("\n[Process exited]\n", curses.color_pair(2))
            log_win.refresh()
            time.sleep(1.5)
            break

        rlist, _, _ = select.select([fifo_fd], [], [], 0.05)
        if fifo_fd in rlist:
            try:
                data = os.read(fifo_fd, 4096).decode("utf-8", errors="ignore")
                if data:
                    lines = (buffer_leftover + data).split("\n")
                    buffer_leftover = lines[-1]
                    for line in lines[:-1]:
                        if not line.strip():
                            continue
                        color = curses.A_NORMAL
                        if "[ERROR" in line or "[FATAL" in line:
                            color = curses.color_pair(2)
                        elif "[INFO" in line:
                            color = curses.color_pair(3)
                        log_win.addstr(line + "\n", color)
                    log_win.refresh()
                    redraw_input()
            except OSError:
                pass

        try:
            ch = input_win.getch()
        except curses.error:
            ch = -1

        if ch != -1:
            if ch in (curses.KEY_ENTER, 10, 13):
                cmd = input_buf.strip()
                input_buf = ""
                redraw_input()
                if cmd in ("quit", "exit"):
                    proc.terminate()
                    break
                elif cmd:
                    log_win.addstr(f"> {cmd}\n", curses.color_pair(1))
                    log_win.refresh()
                    try:
                        proc.stdin.write(cmd + "\n")
                        proc.stdin.flush()
                    except (OSError, ValueError):
                        pass
            elif ch in (curses.KEY_BACKSPACE, 127, 8):
                if len(input_buf) > 0:
                    input_buf = input_buf[:-1]
                    redraw_input()
            elif 32 <= ch <= 126:
                input_buf += chr(ch)
                redraw_input()

    os.close(fifo_fd)
    if os.path.exists(fifo_path):
        try:
            os.remove(fifo_path)
        except OSError:
            pass


def find_binary(repo_root, out_dir=None, binary_path=None):
    if binary_path:
        if os.path.isabs(binary_path):
            return binary_path
        return os.path.abspath(binary_path)

    if out_dir:
        candidate = os.path.join(repo_root, out_dir, "osp_demo")
        if os.path.exists(candidate):
            return candidate
        candidate_rel = os.path.abspath(os.path.join(out_dir, "osp_demo"))
        if os.path.exists(candidate_rel):
            return candidate_rel
        print(f"Error: binary osp_demo not found under out-dir '{out_dir}'.")
        sys.exit(1)

    # Auto-detect standard build directories
    for standard_out in [
            "out/Default", "out/Debug", "out/Release", "out/NoLibs"
    ]:
        candidate = os.path.join(repo_root, standard_out, "osp_demo")
        if os.path.exists(candidate):
            return candidate

    default_candidate = os.path.join(repo_root, "out/Default", "osp_demo")
    return default_candidate


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    repo_root = os.path.abspath(os.path.join(script_dir, "..", ".."))

    parser = argparse.ArgumentParser(
        description="ncurses UI wrapper for Open Screen osp_demo executable.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        "--out-dir",
        "-o",
        help=
        "Build output directory relative to repo root (e.g. out/Default, out/NoLibs).",
    )
    parser.add_argument(
        "--binary",
        "-b",
        help="Explicit path to osp_demo binary.",
    )
    parser.add_argument(
        "friendly_name",
        nargs="?",
        help=
        "Optional friendly name. If specified, runs in Receiver mode. Omission runs in Controller mode.",
    )

    # We use parse_known_args to pass through any extra flags (-v, -t) to osp_demo
    parsed_args, extra_demo_args = parser.parse_known_args()

    binary = find_binary(repo_root, parsed_args.out_dir, parsed_args.binary)

    if not os.path.exists(binary):
        print(f"Error: binary '{binary}' not found.")
        print("Please build osp_demo first or specify --out-dir / --binary.")
        sys.exit(1)

    demo_args = []
    if parsed_args.friendly_name:
        demo_args.append(parsed_args.friendly_name)
    demo_args.extend(extra_demo_args)

    try:
        curses.wrapper(curses_main, repo_root, binary, demo_args)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
