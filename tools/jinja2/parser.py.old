from dataclasses import dataclass
from typing import List, Optional
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

LINE_RE = re.compile(r"^([A-Za-z][A-Za-z0-9_<=\[\]]*)\s+([A-Za-z][A-Za-z0-9_]*)$") # <type> <name>
TYPE_RE = re.compile(r"([A-Za-z][A-Za-z0-9_]*)(<=([0-9]+))?(\[(<=)?([0-9]+)\])?")
# 1st capture group: type name
# 2nd capture group: (bounded) number of characters only for string type
# 3th capture group: number of characters,
#                    if 3rd and 4rd group does not exists, number of character is dynamic.
#                    if only 4rd group exists, number of character is fixed.
#                    if both group exists, number of character is bounded.
# 4th capture group: (bounded) number of element that has type from 1st capture group.
# 5th capture group: bound notation "<="
# 6th capture group: number of elements

@dataclass
class Field:
    type_name: str
    name: str
    size: int
    capacity: int
    string_size: int | None
    string_capacity: int | None
    debug: str | None

    def __init__(self, type_name: str = "", name: str = "", size: int = 0, capacity: int = 0):
        self.type_name = type_name
        self.name = name
        self.size = size
        self.capacity = capacity
        self.string_size = None
        self.string_capacity = None

@dataclass
class MsgSpec:
    msg_name: str
    fields: List[Field]


class MsgParseError(ValueError):
    pass

def parse_type_token(type_token: str) -> Optional[Field]:
    field = Field()
    m = TYPE_RE.match(type_token)

    if not m:
        return None

    # NOTE: should string be null-terminated, or given size, or other option?
    type_name = m.group(1)
    field.type_name = type_name
    # if it's array
    if m.group(4) != None:
        # only "<=" exists: invalid
        if m.group(5) != None and m.group(6) == None:
            return None
        # both size and "<=" exists: bounded
        if m.group(5) != None:
            field.size = 0
            field.capacity = 0
        # both "<=" and number doesn't exist: unbounded
        elif m.group(6) == None:
            field.size = 0
            field.capacity = -1
        # only size exists: fixed
        else:
            field.size = int(m.group(6))
            field.capacity = field.size

    # if it's string type
    if type_name == "string":
        if m.group(2) != None:
            field.string_size = int(m.group(4))
            field.string_capacity = field.string_size
        else:
            field.string_size = 0
            field.string_capacity = -1

    return field

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
                    f"Line {lineno}: invalid format: \"{line}\""
            )

        type_token, field_name = m.group(1), m.group(2)
        field = parse_type_token(type_token)
        if not field:
            raise MsgParseError(
                f"Line {lineno}: unsupported type {type_token!r}"
            )
        field.debug = line
        field.name = field_name
        fields.append(field)

    return MsgSpec(msg_name=msg_name, fields=fields)


def parse_msg_file(filename: str, encoding: str = "utf-8") -> Optional[MsgSpec]:
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
