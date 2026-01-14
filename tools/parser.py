from dataclasses import dataclass
from fileinput import filename
from typing import List
import re
import sys


TYPE_DICT = { # ROS2 type name to C type name
    "bool": "bool",
    "byte": "uint8_t",
    "char": "char",
    "int8": "int8_t",
    "uint8": "uint8_t",
    "int16": "int16_t",
    "uint16": "uint16_t",
    "int32": "int32_t",
    "uint32": "uint32_t",
    "int64": "int64_t",
    "uint64": "uint64_t",
    "float32": "float",
    "float64": "double",
    "string": "char*",
    "wstring": "uint16_t*",
}


LINE_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_]*)\s+([A-Za-z][A-Za-z0-9_]*)$") # <type> <name>


@dataclass
class Field:
    type_name: str
    name: str


@dataclass
class MsgSpec:
    msg_name: str
    fields: List[Field]


class MsgParseError(ValueError):
    pass


def parse_msg_text(msg_name: str, text: str) -> MsgSpec:
    # TODO: Validate field names
    fields: List[Field] = []

    for lineno, line in enumerate(text.splitlines(), start=1):
        line = line.strip()
        if not line:
            continue

        m = LINE_RE.match(line)
        if not m:
            raise MsgParseError(
                f"Line {lineno}: invalid format"
            )

        type_name, field_name = m.group(1), m.group(2)

        if type_name not in TYPE_DICT.keys():
            raise MsgParseError(
                f"Line {lineno}: unsupported type {type_name!r}"
            )

        fields.append(Field(type_name=type_name, name=field_name))

    return MsgSpec(msg_name=msg_name, fields=fields)


def parse_msg_file(filename: str, encoding: str = "utf-8") -> MsgSpec:
    msg_name = filename.split("/")[-1].split(".")[0]
    if not msg_name[0].isupper():
        print(f"Invalid message name: {msg_name} must be camel case")
        return None

    try:
        with open(filename, "r", encoding=encoding) as f:
            return parse_msg_text(msg_name, f.read())
    except FileNotFoundError:
        print(f"File not found: {filename}")
        return None
    except MsgParseError as e:
        print(f"Error parsing message file: {e}")
        return None


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
    print(spec)


if __name__ == "__main__":
    main()
