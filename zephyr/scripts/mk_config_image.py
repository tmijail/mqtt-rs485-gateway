#!/usr/bin/env python3
"""Build a LittleFS image holding /config.json for the gateway (CFG-2).

The gateway mounts the "lfs" partition at /lfs and loads /lfs/config.json
when present. The parameters below match the fstab node in
boards/*.overlay (read/prog 16, cache 256, lookahead 32, 4 KiB blocks).

Waveshare board (3 MiB partition at 0xc20000):

    mk_config_image.py my_config.json -o lfs.bin
    esptool --chip esp32s3 write_flash 0xc20000 lfs.bin

native_sim (512 KiB partition at 0x100000 of the 2 MiB simulated flash):

    mk_config_image.py my_config.json --size 512K --flash-file flash.bin \\
        --flash-size 2M --offset 0x100000
    zephyr.exe -flash=flash.bin
"""

import argparse
import json
import os
import sys

from littlefs import LittleFS

BLOCK_SIZE = 4096


def size_arg(text):
    text = text.strip().upper()
    mult = {"K": 1024, "M": 1024 * 1024}.get(text[-1], 1)
    return int(text[:-1] if text[-1] in "KM" else text, 0) * mult


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("config", help="JSON configuration (SPEC section 6)")
    parser.add_argument("-o", "--output", help="raw partition image to write")
    parser.add_argument("--size", type=size_arg, default=size_arg("3M"),
                        help="partition size (default 3M)")
    parser.add_argument("--flash-file", help="patch the image into this whole-flash file")
    parser.add_argument("--flash-size", type=size_arg, help="size of --flash-file if created")
    parser.add_argument("--offset", type=lambda s: int(s, 0), help="partition offset")
    args = parser.parse_args()

    with open(args.config, "rb") as f:
        data = f.read()
    json.loads(data)  # catch syntax errors early; the device validates the schema

    fs = LittleFS(block_size=BLOCK_SIZE, block_count=args.size // BLOCK_SIZE, read_size=16,
                  prog_size=16, cache_size=256, lookahead_size=32, block_cycles=512)
    with fs.open("config.json", "wb") as f:
        f.write(data)
    image = bytes(fs.context.buffer)

    if args.output:
        with open(args.output, "wb") as f:
            f.write(image)
    if args.flash_file:
        if args.offset is None:
            parser.error("--flash-file needs --offset")
        if not os.path.exists(args.flash_file):
            if not args.flash_size:
                parser.error("--flash-size is needed to create --flash-file")
            with open(args.flash_file, "wb") as f:
                f.write(b"\xff" * args.flash_size)
        with open(args.flash_file, "r+b") as f:
            f.seek(args.offset)
            f.write(image)
    if not args.output and not args.flash_file:
        parser.error("nothing to do: give -o and/or --flash-file")
    print(f"{len(data)} byte config in a {len(image)} byte LittleFS image")
    return 0


if __name__ == "__main__":
    sys.exit(main())
