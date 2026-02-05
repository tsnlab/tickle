import sys
import os

from shutil import copyfile, SameFileError
from pathlib import Path
from jinja2 import Environment, FileSystemLoader
from typing import List
from parser import parse_msg, Content, Message, Field

TOPIC_PP_SOURCE = "topic.c.jinja"
TOPIC_PP_HEADER = "topic.h.jinja"
TOPIC_PUBLISHER = "topic_publisher.c.jinja"
TOPIC_SUBSCRIBER = "topic_subscriber.c.jinja"
SERVICE_PP_SOURCE = "service.c.jinja"
SERVICE_PP_HEADER = "service.h.jinja"
SERVICE_SERVER = "service_server.c.jinja"
SERVICE_CLIENT = "service_client.c.jinja"

jinja2_env = Environment(
    loader=FileSystemLoader("./tools/jinja2"),
    trim_blocks=True,
)
class TypeError(Exception):
    pass

def generate_topic_preprocessor(path: Path, content: Content):
    msg_name = content.name
    messages = content.messages
    assert len(messages) == 1, "Number of message object is supposed to be one"
    message = messages[0]
    # TODO: include header
    includes = content.includes
    source_template = jinja2_env.get_template(TOPIC_PP_SOURCE)
    source_content = source_template.render(
        msg_name = content.name,
        message = message,
    )
    with open(f"{path}/{msg_name}.c", "w", encoding="utf-8") as f:
        f.write(source_content)
    header_template = jinja2_env.get_template(TOPIC_PP_HEADER)
    header_content = header_template.render(
        msg_name = content.name,
        message = message,
        includes = includes,
    )
    with open(f"{path}/msg/{msg_name}.h", "w", encoding="utf-8") as f:
        f.write(header_content)

def test_parsed_content(content: Content):
    message = content.messages[0]
    for field in message.fields:
        print(f"name={field.name} type={field.type_name} size={field.size} string_size={field.string_size}")

def generate_service_preprocessor(content: Content):
    assert False, "service preprocessor is not implemented yet"

def generate_test_files(content: Content):
    assert False, "test file generation is not implemented yet"

def setup_directory(pkg_path: Path, msg_path: Path):
    if msg_path.is_file() == False:
        print(f"msg_file is not a file: {msg_path}")
    if pkg_path.exists() == True and pkg_path.is_dir() == False:
        print(f"package_path is not a directory: {pkg_path}")
        sys.exit(1)
    if pkg_path.exists() == False:
        os.mkdir(pkg_path)
    if (pkg_path / "msg").exists() == False:
        os.mkdir(pkg_path / "msg")
    if (pkg_path / "srv").exists() == False:
        os.mkdir(pkg_path / "srv")
    copied_msg_path = pkg_path / msg_path.suffix.lstrip(".") / msg_path.name
    if msg_path == copied_msg_path:
        return
    copyfile(msg_path, copied_msg_path)

def generate(pkg_path: Path, msg_path: Path):
    suffix = msg_path.suffix.lstrip(".")
    if suffix != "msg" and suffix != "srv":
        print(f"Invalid file type: {msg_path} is not a .msg/.srv file")
        sys.exit(1)
    setup_directory(pkg_path, msg_path)
    content = parse_msg(pkg_path, msg_path)
    if suffix == "msg":
        generate_topic_preprocessor(pkg_path, content)
    elif suffix == "srv":
        generate_service_preprocessor(pkg_path, content)

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
