import sys

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
    loader=FileSystemLoader("."),
    trim_blocks=True,
)
class TypeError(Exception):
    pass

def generate_topic_preprocessor(content: Content):
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
    with open(f"{msg_name}.c", "w", encoding="utf-8") as f:
        f.write(source_content)
    header_template = jinja2_env.get_template(TOPIC_PP_HEADER)
    header_content = header_template.render(
        msg_name = content.name,
        message = message,
        includes = includes,
    )
    with open(f"{msg_name}.h", "w", encoding="utf-8") as f:
        f.write(header_content)

def test_parsed_content(content: Content):
    message = content.messages[0]
    for field in message.fields:
        print(f"name={field.name} type={field.type_name} size={field.size} string_size={field.string_size}")

def generate_service_preprocessor(content: Content):
    assert False, "service preprocessor is not implemented yet"

def generate_test_files(content: Content):
    assert False, "test file generation is not implemented yet"

def main(argv: List[str] = sys.argv):
    # TODO: Add argument parser
    if len(argv) != 3:
        print(f"Usage: python3 {argv[0]}  package_path  msg_file")
        print(f"    package_path        path to a ROS package directory")
        print(f"    msg_file            ROS2 message/service definition file(.msg, .srv)", end='')
        print(f" in package_path/msg/ or package_path/srv/")
        sys.exit(1)

    filename = argv[2]
    content = parse_msg(argv[1], filename)
    suffix = Path(filename).suffix
    if suffix == ".msg":
#       test_parsed_content(content)
        generate_topic_preprocessor(content)
    elif suffix == ".srv":
        generate_service_preprocessor(content)
#   generate_test_files(spec)


if __name__ == "__main__":
    main()
