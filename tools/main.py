# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 TSN Lab
import sys
import argparse
import generator as gen

from pathlib import Path
from typing import List
from parser import parse_msg

TEST = False

def generate(pkg_path: Path, msg_path: Path):
    suffix = msg_path.suffix.lstrip(".")
    if suffix != "msg" and suffix != "srv":
        print(f"Invalid file type: {msg_path} is not a .msg/.srv file")
        sys.exit(1)
    gen.setup_directory(pkg_path, msg_path)
    content = parse_msg(pkg_path, msg_path)
    if suffix == "msg":
        gen.generate_message_preprocessor(pkg_path, content)
        if TEST == True:
            gen.generate_message_tester(pkg_path, content)
    elif suffix == "srv":
        gen.generate_service_preprocessor(pkg_path, content)
        if TEST == True:
            gen.generate_service_tester(pkg_path, content)

def main(argv: List[str] = sys.argv):
    arg_parser = argparse.ArgumentParser(
        prog=f"{argv[0]}",
        description="Typesupport generator for TickLE and rmw_tickle",
    )
    arg_parser.add_argument('-e', '--example', action='store_const', const=True)
    arg_parser.add_argument('package_path', help="path to a ROS package directory")
    arg_parser.add_argument('message_file', help="ROS2 message/service definition file(.msg, .srv)")
    args = arg_parser.parse_args()
    global TEST
    TEST = args.example
    pkg_path = Path(args.package_path).resolve()
    msg_path = Path(args.message_file).resolve()
    generate(pkg_path, msg_path)

if __name__ == "__main__":
    main()
