import sys
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
        assert False, "srv file processing is not implemented yet"
        gen.generate_service_preprocessor(pkg_path, content)

def main(argv: List[str] = sys.argv):
    # TODO: Add argument parser
    if len(argv) != 3:
        print(f"Usage: python3 {argv[0]}  package_path  msg_file")
        print(f"    package_path        path to a ROS package directory")
        print(f"    msg_file            ROS2 message/service definition file(.msg, .srv)")
        sys.exit(1)

    pkg_path = Path(argv[1]).resolve()
    msg_path = Path(argv[2]).resolve()
    generate(pkg_path, msg_path)

if __name__ == "__main__":
    main()
