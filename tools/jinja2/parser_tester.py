import sys
from jinja2 import Environment, FileSystemLoader
from typing import List
from parser import MsgSpec, parse_msg_file

def print_types(spec: MsgSpec):
    print(f"msg_name: {spec.msg_name}")
    for field in spec.fields:
        print(f"{field.debug}")
        print(f"    type_name={field.type_name}")
        print(f"    name={field.name}")
        print(f"    size={field.size}")
        print(f"    capacity={field.capacity}")
        if (field.string_size != None):
            print(f"    string_size={field.string_size}")
            print(f"    string_capacity={field.string_capacity}")

def main(argv: List[str] = sys.argv):
    # TODO: Add argument parser
    if len(argv) != 2:
        print(f"Usage: {argv[0]} <msg_file>")
        sys.exit(1)

    filename = argv[1]
    if not filename.endswith(".msg"):
        print(f"Invalid file type: {filename} is not a .msg file")
        sys.exit(1)

    spec = parse_msg_file(filename)
    if spec is None:
        print("failed to parse")
        sys.exit(1)

    print_types(spec)

if __name__ == "__main__":
    main()
