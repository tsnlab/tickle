import sys
from jinja2 import Environment, FileSystemLoader
from typing import List
from parser import MsgSpec, parse_msg_file

env = Environment(
    loader=FileSystemLoader("."),
    trim_blocks=True,
)


def generate_source_file(spec: MsgSpec, filename: str):
    template = env.get_template("preprocessor.c.jinja")
    source_file = template.render(
        msg_name=spec.msg_name,
        fields=spec.fields,
    )
    with open(filename, "w", encoding="utf-8") as f:
        f.write(source_file)


def generate_header_file(spec: MsgSpec, filename: str):
    template = env.get_template("preprocessor.h.jinja")
    header_file = template.render(
        msg_name=spec.msg_name,
        fields=spec.fields,
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
