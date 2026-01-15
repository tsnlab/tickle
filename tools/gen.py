import sys
from typing import List
from parser import MsgSpec, parse_msg_file
from template import *


def generate_source_file(spec: MsgSpec, filename: str):
    source_file = MSG_SOURCE_TEMPLATE.format(
        msg_name=spec.msg_name,
        encode_size_template=" + ".join(PRIMITIVE_ENCODE_SIZE_TEMPLATE.format(type_name=field.type_name) for field in spec.fields),
        encode_template="\n".join(PRIMITIVE_ENCODE_TEMPLATE.format(type_name=field.type_name) for field in spec.fields),
        decode_template="\n".join(PRIMITIVE_DECODE_TEMPLATE.format(type_name=field.type_name) for field in spec.fields),
        free_template=PRIMITIVE_FREE_TEMPLATE if len(PRIMITIVE_FREE_TEMPLATE) > 0 else DUMMY_PLACEHOLDER,
    )

    with open(filename, "w", encoding="utf-8") as f:
        f.write(source_file)


def generate_header_file(spec: MsgSpec, filename: str):
    header_file = MSG_HEADER_TEMPLATE.format(
        msg_name=spec.msg_name,
        fields_template="\n".join(FIELDS_TEMPLATE.format(type_name=field.type_name, name=field.name) for field in spec.fields),
    )

    with open(filename, "w", encoding="utf-8") as f:
        f.write(header_file)


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
        sys.exit(1)

    generate_source_file(spec, filename.replace(".msg", ".c"))
    generate_header_file(spec, filename.replace(".msg", ".h"))


if __name__ == "__main__":
    main()