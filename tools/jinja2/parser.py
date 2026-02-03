import sys

from typing import List, Optional
from dataclasses import dataclass
from pathlib import Path
from rosidl_adapter import convert_to_idl
from rosidl_parser.parser import parse_idl_file
import rosidl_parser.definition as rosdef

TEST = False

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

@dataclass
class Field:
    type_name: str
    name: str
    size: int
    string_size: int | None
    debug: str | None
    def __init__(self, type_name="", name="", size=0, string_size=None, debug=None):
        self.type_name = type_name
        self.name = name
        self.size = size
        self.string_size = string_size
        self.debug = debug


@dataclass
class Message:
    fields: List[Field]
    def __init__(self, fields=[]):
        self.fields = fields

@dataclass
class Content:
    name: str
    messages: List[Message]
    includes: List[str]


class MsgParseError(ValueError):
    pass

class TypeError(Exception):
    pass

def get_field(typeInfo: rosdef.AbstractType) -> Field:
    if isinstance(typeInfo, rosdef.AbstractNestableType):
        if isinstance(typeInfo, rosdef.BasicType):
            typename = TYPE_DICT[typeInfo.typename]
            return Field(type_name=typename)
        elif isinstance(typeInfo, rosdef.NamedType):
            return Field(type_name=f"struct {typeInfo.name}")
        elif isinstance(typeInfo, rosdef.NamespacedType):
            return Field(type_name=f"struct {'_'.join(typeInfo.namespaced_name())}")
        elif isinstance(typeInfo, rosdef.BoundedString):
            return Field(type_name="char*", string_size=typeInfo.maximum_size)
        elif isinstance(typeInfo, rosdef.UnboundedString):
            return Field(type_name="char*", string_size=-1)
        elif isinstance(typeInfo, rosdef.BoundedWString):
            return Field(type_name="uint16_t*", string_size=typeInfo.maximum_size)
        elif isinstance(typeInfo, rosdef.UnboundedWString):
            return Field(type_name="uint16_t*", string_size=-1)
        else:
            raise TypeError(f"Unhandled type \"{type(typeInfo)}\"")
    elif isinstance(typeInfo, rosdef.AbstractNestedType):
        if isinstance(typeInfo, rosdef.Array):
            nested_field = get_field(typeInfo.value_type)
            nested_field.size = typeInfo.size
            return nested_field
        elif isinstance(typeInfo, rosdef.BoundedSequence):
            nested_field = get_field(typeInfo.value_type)
            nested_field.size = typeInfo.maximum_size
            return nested_field
        elif isinstance(typeInfo, rosdef.UnboundedSequence):
            nested_field = get_field(typeInfo.value_type)
            nested_field.size = 0
            return nested_field
        else:
            raise TypeError(f"Unhandled type \"{type(typeInfo)}\"")

def read_message(ros_message: rosdef.Message) -> Message:
    message = Message()
    for member in ros_message.structure.members:
        field = get_field(member.type)
        field.name = member.name
        message.fields.append(field)
        print(f"name={field.name:<10}\ntype={field.type_name}\n")
    return message

def read_service(ros_service: rosdef.Service) -> Content:
    content = Content(name="", messages=[], includes=[])
    print(f"request message")
    content.messages.append(read_message(ros_service.request_message))
    print(f"response message")
    content.messages.append(read_message(ros_service.response_message))
    return content

def parse_msg(path_arg: str, msg_arg: str) -> Content:
    pkg_path = Path(path_arg)
    if pkg_path.is_dir() == False:
        print(f"Path is not a directory: {pkg_path}")
        sys.exit(1)
    if pkg_path.is_absolute() == False:
        pkg_path = pkg_path.resolve()
    pkg_name = pkg_path.stem
    msg_filename = Path(msg_arg)
    suffix = msg_filename.suffix.lstrip(".")
    if suffix != "msg" and suffix != "srv":
        print(f"Invalid file type: {msg_filename} is not a .msg/.srv file")
        sys.exit(1)
    idl_filename = Path(convert_to_idl(pkg_path / suffix, pkg_name, msg_filename, pkg_path))

    locator = rosdef.IdlLocator(pkg_path, idl_filename)
    idl_file = parse_idl_file(locator)
    msg_name = msg_arg.replace(f".{suffix}", "")
    content = Content(name=msg_name, messages=[], includes=[])
    if suffix == "msg":
        message = idl_file.content.get_elements_of_type(rosdef.Message)[0]
        print(f"namespace={get_field(message.structure.namespaced_type).type_name}\n")
        content.messages.append(read_message(message))
    elif suffix == "srv":
        service = idl_file.content.get_elements_of_type(rosdef.Service)[0]
        print(f"namespace={get_field(service.namespaced_type)}\n")
        content = read_service(service)
    includes = idl_file.content.get_elements_of_type(rosdef.Include)
    for include in includes:
        include_path = include.locator.replace("idl", "h")
        content.includes.append(include_path)
        print(f"include={include_path}")
    return content

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: python3 {sys.argv[0]}  package_path  msg_file")
        print(f"    package_path        path to a ROS package directory")
        print(f"    msg_file            ROS2 message/service definition file (.msg, .srv)")
        print(f"                        in package_path/msg/ or package_path/srv/")
    else:
        TEST = True
        parse_msg(sys.argv[1], sys.argv[2])

