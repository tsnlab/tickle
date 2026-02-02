import sys

from pathlib import Path
from rosidl_adapter import convert_to_idl
from rosidl_parser.parser import parse_idl_file
import rosidl_parser.definition as rosdef

class TypeError(Exception):
    pass

def get_fieldtype(typeInfo: rosdef.AbstractType):
    if isinstance(typeInfo, rosdef.AbstractNestableType):
        if isinstance(typeInfo, rosdef.BasicType):
            return f"{typeInfo.typename}"
        elif isinstance(typeInfo, rosdef.NamedType):
            return f"{typeInfo.name}"
        elif isinstance(typeInfo, rosdef.NamespacedType):
            return f"{'::'.join(typeInfo.namespaced_name())}"
        elif isinstance(typeInfo, rosdef.BoundedString):
            return f"string<={typeInfo.maximum_size}"
        elif isinstance(typeInfo, rosdef.UnboundedString):
            return f"{"string[]"}"
        elif isinstance(typeInfo, rosdef.BoundedWString):
            return f"wstring<={typeInfo.maximum_size}"
        elif isinstance(typeInfo, rosdef.UnboundedWString):
            return f"{"wstring[]"}"
        else:
            raise TypeError(f"Unhandled type \"{type(typeInfo)}\"")
    elif isinstance(typeInfo, rosdef.AbstractNestedType):
        if isinstance(typeInfo, rosdef.Array):
            return f"{get_fieldtype(typeInfo.value_type)}[{typeInfo.size}]{""}"
        elif isinstance(typeInfo, rosdef.BoundedSequence):
            return f"{get_fieldtype(typeInfo.value_type)}[<={typeInfo.maximum_size}]{""}"
        elif isinstance(typeInfo, rosdef.UnboundedSequence):
            return f"{get_fieldtype(typeInfo.value_type)}[]{""}"
        else:
            raise TypeError(f"Unhandled type \"{type(typeInfo)}\"")



def parse_msg(path_arg, msg_arg):
    pkg_path = Path(path_arg)
    if pkg_path.is_dir() == False:
        print(f"Path is not a directory: {pkg_path}")
        return 1
    if pkg_path.is_absolute() == False:
        print(f"Path is not absolute: {pkg_path}")
        return 1
    msg_filename = Path(msg_arg)
    idl_filename = Path(convert_to_idl(pkg_path / "msg", pkg_path.stem, msg_filename, pkg_path))

    locator = rosdef.IdlLocator(pkg_path, idl_filename)
    idl_file = parse_idl_file(locator)
    messages = idl_file.content.get_elements_of_type(rosdef.Message)
    for message in messages:
        for member in message.structure.members:
            typename = get_fieldtype(member.type)
            print(f"name={member.name:<10}\ntype={typename}\n")

    includes = idl_file.content.get_elements_of_type(rosdef.Include)
    for include in includes:
        print(f"include={include.locator}")

if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(f"Usage: {sys.argv[0]}  package_path  msg_file_name")
    else:
        parse_msg(sys.argv[1], sys.argv[2])

