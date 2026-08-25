#!/usr/bin/env python3
# Copyright 2020 The Chromium Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""
This script is used to download a file from the Chromium repository.

It's equivalent to using curl to download the file, and is intended to be ran
as a gclient hook.
"""

import argparse
import base64
import os
import sys
import urllib.request
import urllib.error

GITILES_URL_TEMPLATE = 'https://chromium.googlesource.com/chromium/src/+/{}/{}?format=TEXT'


def download_file(revision: str, path: str, output_path: str) -> bool:
    url = GITILES_URL_TEMPLATE.format(revision or 'main', path)
    print(f'  -> Downloading from "{url}" to "{output_path}"...')
    req = urllib.request.Request(url, headers={'User-Agent': 'Mozilla/5.0'})
    try:
        with urllib.request.urlopen(req) as resp:
            content = base64.b64decode(resp.read())
        dir_path = os.path.dirname(os.path.abspath(output_path))
        os.makedirs(dir_path, exist_ok=True)
        with open(output_path, 'wb') as f:
            f.write(content)
        return True
    except (urllib.error.HTTPError, urllib.error.URLError) as e:
        print(f'ERROR: Failed to download {url}: {e}', file=sys.stderr)
        return False


def main():
    parser = argparse.ArgumentParser(
        description='Download a file from the Chromium repository')
    parser.add_argument('--output',
                        required=True,
                        help='path to file to create/overwrite')
    parser.add_argument('--revision',
                        required=True,
                        help='revision to download')
    parser.add_argument('--path',
                        required=True,
                        help='path within the Chromium repository')
    args = parser.parse_args()

    return 0 if download_file(args.revision, args.path, args.output) else 1


if __name__ == '__main__':
    sys.exit(main())
