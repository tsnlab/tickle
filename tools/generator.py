import sys
import os

from shutil import copyfile, SameFileError
from pathlib import Path
from jinja2 import Environment, FileSystemLoader
from typing import List
from parser_types import Content, Message, Field

TOPIC_MAKEFILE = "Makefile.jinja"
TOPIC_PP_SOURCE = "topic.c.jinja"
TOPIC_PP_HEADER = "topic.h.jinja"
TOPIC_PUBLISHER = "topic_publisher.c.jinja"
TOPIC_SUBSCRIBER = "topic_subscriber.c.jinja"
SERVICE_PP_SOURCE = "service.c.jinja"
SERVICE_PP_HEADER = "service.h.jinja"
SERVICE_SERVER = "service_server.c.jinja"
SERVICE_CLIENT = "service_client.c.jinja"

class TypeError(Exception):
    pass

def generate_topic_preprocessor(path: Path, content: Content):
    jinja2_env = Environment(
        loader=FileSystemLoader("./tools/jinja2"),
        trim_blocks=True,
    )
    msg_name = content.name
    messages = content.messages
    assert len(messages) == 1, "Number of message object is supposed to be one"
    message = messages[0]
    # TODO: include header
    includes = content.includes
    source_template = jinja2_env.get_template(TOPIC_PP_SOURCE)
    source_content = source_template.render(
        msg_name = content.name,
        msg_unique_name = message.prefix + content.name,
        message = message,
    )
    with open(f"{path}/{msg_name}.c", "w", encoding="utf-8") as f:
        f.write(source_content)
    header_template = jinja2_env.get_template(TOPIC_PP_HEADER)
    header_content = header_template.render(
        msg_name = content.name,
        msg_unique_name = message.prefix + content.name,
        message = message,
        includes = includes,
    )
    with open(f"{path}/msg/{msg_name}.h", "w", encoding="utf-8") as f:
        f.write(header_content)

def generate_topic_tester(path: Path, content: Content):
    jinja2_env = Environment(
        loader=FileSystemLoader("./tools/jinja2"),
        trim_blocks=True,
    )
    msg_name = content.name
    messages = content.messages
    assert len(messages) == 1, "Number of message object is supposed to be one"
    message = messages[0]
    pub_template = jinja2_env.get_template(TOPIC_PUBLISHER)
    pub_content = pub_template.render(
        msg_name = content.name,
        msg_unique_name = message.prefix + content.name,
        message = message,
    )
    with open(f"{path}/{msg_name}_pub.c", "w", encoding="utf-8") as f:
        f.write(pub_content)
    sub_template = jinja2_env.get_template(TOPIC_SUBSCRIBER)
    sub_content = sub_template.render(
        msg_name = content.name,
        msg_unique_name = message.prefix + content.name,
        message = message,
    )
    with open(f"{path}/{msg_name}_sub.c", "w", encoding="utf-8") as f:
        f.write(sub_content)
    make_template = jinja2_env.get_template(TOPIC_MAKEFILE)
    make_content = make_template.render(
        preprocessor = f"{content.name}.c",
        pub = f"{content.name}_pub.c",
        sub = f"{content.name}_sub.c",
    )
    with open(f"{path}/Makefile", "w", encoding="utf-8") as f:
        f.write(make_content)

def generate_service_preprocessor(content: Content):
    assert False, "service preprocessor is not implemented yet"

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
