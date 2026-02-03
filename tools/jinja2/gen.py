import sys
from jinja2 import Environment, FileSystemLoader
from typing import List
# from parser import MsgSpec, parse_msg_file
from parse_msg import parse_msg

TOPIC_PP_SOURCE = "topic.c.jinja"
TOPIC_PP_HEADER = "topic.h.jinja"
TOPIC_PUBLISHER = "topic_publisher.c.jinja"
TOPIC_SUBSCRIBER = "topic_subscriber.c.jinja"
SERVICE_PP_SOURCE = "service.c.jinja"
SERVICE_PP_HEADER = "service.h.jinja"
SERVICE_SERVER = "service_server.c.jinja"
SERVICE_CLIENT = "service_client.c.jinja"

env = Environment(
    loader=FileSystemLoader("."),
    trim_blocks=True,
)

def generate_from_template(spec: MsgSpec, template_file: str, output_file: str):
    template = env.get_template(template_file)
    content = template.render(
        msg_name=spec.msg_name,
        fields=spec.fields,
    )
    with open(output_file, "w", encoding="utf-8") as f:
        f.write(content)

def generate_preprocessor(spec: MsgSpec):
    generate_from_template(spec, TOPIC_PP_SOURCE, f"{spec.msg_name}.c");
    generate_from_template(spec, TOPIC_PP_HEADER, f"{spec.msg_name}.h");

def generate_test_files(spec: MsgSpec):
    generate_from_template(spec, TOPIC_PUBLISHER, "server.c");
    generate_from_template(spec, TOPIC_SUBSCRIBER, "client.c");


def main(argv: List[str] = sys.argv):
    # TODO: Add argument parser
    if len(argv) != 3:
        print(f"Usage: {argv[0]} <package_path> <msg_file>")
        sys.exit(1)

    pkg_path = argv[1]
    filename = argv[2]
    if not filename.endswith(".msg"):
        print(f"Invalid file type: {filename} is not a .msg file")
        sys.exit(1)

    spec = parse_msg(pkg_path, filename)
    if spec is None:
        sys.exit(1)

    generate_preprocessor(spec)
    generate_test_files(spec)

if __name__ == "__main__":
    main()
