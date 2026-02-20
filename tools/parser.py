import sys
import rosidl_parser.definition as rosdef

from rosidl_adapter import convert_to_idl
from rosidl_parser.parser import parse_idl_file
from typing import List, Optional
from pathlib import Path
from generator import setup_directory, generate_message_preprocessor
from parser_types import Content, Message, Field

TYPE_DICT = { # IDL type name to C type name
    "boolean": "bool",
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
    "float": "float",
    "double": "double",
    "string": "char*",
    "wstring": "uint16_t*",
}

class TypeError(Exception):
    pass

def get_field(typeInfo: rosdef.AbstractType, prefix: str = "") -> Field:
    if isinstance(typeInfo, rosdef.AbstractNestableType):
        if isinstance(typeInfo, rosdef.BasicType):
            typename = TYPE_DICT[typeInfo.typename]
            return Field(type_name=typename)
        elif isinstance(typeInfo, rosdef.NamedType):
            print(f"NamedType={typeInfo.name}")
            return Field(type_name=f"struct {prefix}{typeInfo.name}", named=True)
        elif isinstance(typeInfo, rosdef.NamespacedType):
            return Field(type_name=f"struct {'__'.join(typeInfo.namespaced_name())}", named=True)
        elif isinstance(typeInfo, rosdef.BoundedString):
            return Field(type_name="char*", string_size=typeInfo.maximum_size)
#       elif isinstance(typeInfo, rosdef.UnboundedString):
#           return Field(type_name="char*", string_size=-1)
        elif isinstance(typeInfo, rosdef.BoundedWString):
            return Field(type_name="uint16_t*", string_size=typeInfo.maximum_size)
#       elif isinstance(typeInfo, rosdef.UnboundedWString):
#           return Field(type_name="uint16_t*", string_size=-1)
        else:
            raise TypeError(f"Unhandled type \"{type(typeInfo)}\"")
    elif isinstance(typeInfo, rosdef.AbstractNestedType):
        if isinstance(typeInfo, rosdef.Array):
            nested_field = get_field(typeInfo.value_type)
            nested_field.size = typeInfo.size
        elif isinstance(typeInfo, rosdef.BoundedSequence):
            nested_field = get_field(typeInfo.value_type)
            nested_field.size = typeInfo.maximum_size
        elif isinstance(typeInfo, rosdef.UnboundedSequence):
            nested_field = get_field(typeInfo.value_type)
            nested_field.size = -1
        else:
            raise TypeError(f"Unhandled type \"{type(typeInfo)}\"")
        if nested_field.string_size != 0:
            raise TypeError(f"Handling array of string is not implemented yet")
        return nested_field

def read_message(ros_message: rosdef.Message) -> Message:
    namedtype_prefix = "__".join(ros_message.structure.namespaced_type.namespaced_name()[0:2]) + "__"
    message = Message(fields=[])
    message.prefix = namedtype_prefix
    for member in ros_message.structure.members:
        field = get_field(member.type, namedtype_prefix)
        field.name = member.name
        message.fields.append(field)
    return message

def read_service(content: Content, ros_service: rosdef.Service) -> Content:
    content.messages.append(read_message(ros_service.request_message))
    content.messages.append(read_message(ros_service.response_message))
    return content

def parse_external_msg(pkg_path: Path, msg_path: Path, path: rosdef.Include) -> Optional[Content]:
    idl_path = pkg_path.parents[0] / path.locator
    header_path = idl_path.with_suffix(".h")

    if header_path.exists() == True:
        print(f"\nHeader found: {header_path}")
    else:
        print(f"\nHeader not found: {header_path}")
    msg_base_path = msg_path.parents[0]
    external_msg_path = (msg_base_path / idl_path.stem).with_suffix(".msg")
    if external_msg_path.exists() == True:
        print(f".msg found: {external_msg_path}")
        external_pkg_path = idl_path.parents[1]
        setup_directory(external_pkg_path, external_msg_path)
        content = parse_msg(external_pkg_path, external_msg_path)
        generate_message_preprocessor(external_pkg_path, content)
        return content
    else:
        print(f"Warning: Header is not found: {idl_path.stem}.h in {idl_path.parents[0]}")
        print(f"         Use below command to generate header file that {msg_path.name} requires")
        print(f"         python3 {sys.argv[0]} {str(idl_path.parents[1])} <path>/{idl_path.stem}.msg\n")
        return None

def parse_msg(pkg_path: Path, msg_path: Path) -> Content:
    pkg_name = pkg_path.stem
    msg_name = msg_path.stem
    suffix = msg_path.suffix.lstrip(".")
    idl_filename = Path(convert_to_idl(pkg_path / suffix, pkg_name, Path(msg_path.name), pkg_path))
    locator = rosdef.IdlLocator(pkg_path, idl_filename)
    idl_file = parse_idl_file(locator)
    content = Content(name=msg_name, pkg_name=pkg_name, messages=[], includes=set())
    if suffix == "msg":
        message = idl_file.content.get_elements_of_type(rosdef.Message)[0]
        content.messages.append(read_message(message))
    elif suffix == "srv":
        service = idl_file.content.get_elements_of_type(rosdef.Service)[0]
        # TODO: add namedtype_prefix for service
        content = read_service(content, service)
    includes = idl_file.content.get_elements_of_type(rosdef.Include)
    for include in includes:
        child_content = parse_external_msg(pkg_path, msg_path, include)
        content.includes.update(child_content.includes)
        include_path = include.locator.replace("idl", "h")
        content.includes.add(include_path)
    return content

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: python3 {sys.argv[0]}  package_path  msg_file")
        print(f"    package_path        path to a ROS package directory")
        print(f"    msg_file            ROS2 message/service definition file (.msg, .srv)")
        print(f"                        in package_path/msg/ or package_path/srv/")
    else:
        parse_msg(sys.argv[1], sys.argv[2])
