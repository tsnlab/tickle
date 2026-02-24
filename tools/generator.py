import sys
import os

from shutil import copyfile, SameFileError
from pathlib import Path
from jinja2 import Environment, FileSystemLoader
from typing import List
from parser_types import Content, Message, Field

MAKEFILE_TEMPLATE = "Makefile.jinja"
MESSAGE_PP_SOURCE = "message_preprocessor.c.jinja"
MESSAGE_PP_HEADER = "message_preprocessor.h.jinja"
MESSAGE_PUBLISHER = "message_publisher.c.jinja"
MESSAGE_SUBSCRIBER = "message_subscriber.c.jinja"
SERVICE_MAKEFILE = "Makefile.service.jinja"
SERVICE_PP_SOURCE = "service_preprocessor.c.jinja"
SERVICE_PP_HEADER = "service_preprocessor.h.jinja"
SERVICE_SERVER = "service_server.c.jinja"
SERVICE_CLIENT = "service_client.c.jinja"

jinja2_env = Environment(
    loader=FileSystemLoader("./tools/jinja2"),
    trim_blocks=True,
)

def generate_message_preprocessor(path: Path, content: Content):
    msg_name = content.name
    messages = content.messages
    assert len(messages) == 1, "Number of message object is supposed to be one"
    message = messages[0]
    source_template = jinja2_env.get_template(MESSAGE_PP_SOURCE)
    source_content = source_template.render(
        msg_name = msg_name,
        msg_unique_name = message.prefix + content.name,
        message = message,
        content = content,
    )
    with open(f"{path}/{msg_name}.c", "w", encoding="utf-8") as f:
        f.write(source_content)
    header_template = jinja2_env.get_template(MESSAGE_PP_HEADER)
    header_content = header_template.render(
        msg_name = msg_name,
        msg_unique_name = message.prefix + content.name,
        message = message,
        includes = content.include_paths,
    )
    with open(f"{path}/msg/{msg_name}.h", "w", encoding="utf-8") as f:
        f.write(header_content)

def generate_message_tester(path: Path, content: Content):
    msg_name = content.name
    messages = content.messages
    assert len(messages) == 1, "Number of message object is supposed to be one"
    message = messages[0]
    pub_template = jinja2_env.get_template(MESSAGE_PUBLISHER)
    pub_content = pub_template.render(
        msg_name = msg_name,
        msg_unique_name = message.prefix + msg_name,
        message = message,
    )
    with open(f"{path}/{msg_name}_pub.c", "w", encoding="utf-8") as f:
        f.write(pub_content)
    sub_template = jinja2_env.get_template(MESSAGE_SUBSCRIBER)
    sub_content = sub_template.render(
        msg_name = msg_name,
        msg_unique_name = message.prefix + msg_name,
        message = message,
    )
    with open(f"{path}/{msg_name}_sub.c", "w", encoding="utf-8") as f:
        f.write(sub_content)
    makefile_template = jinja2_env.get_template(MAKEFILE_TEMPLATE)
    makefile_content = makefile_template.render(
        preprocessor = f"{msg_name}.c",
        target1 = "publisher",
        target2 = "subscriber",
        source1 = f"{msg_name}_pub.c",
        source2 = f"{msg_name}_sub.c",
        external_sources = content.external_sources,
    )
    with open(f"{path}/Makefile", "w", encoding="utf-8") as f:
        f.write(makefile_content)

def generate_service_preprocessor(path: Path, content: Content):
    srv_name = content.name
    messages = content.messages
    assert len(messages) == 2, "Number of message object is supposed to be 2"
    request = messages[0]
    response = messages[1]
    source_template = jinja2_env.get_template(SERVICE_PP_SOURCE)
    source_content = source_template.render(
        srv_name = srv_name,
        srv_unique_name = request.prefix + content.name,
        request = request,
        response = response,
        content = content,
    )
    with open(f"{path}/{srv_name}.c", "w", encoding="utf-8") as f:
        f.write(source_content)
    header_template = jinja2_env.get_template(SERVICE_PP_HEADER)
    header_content = header_template.render(
        srv_name = srv_name,
        srv_unique_name = request.prefix + content.name,
        request = request,
        response = response,
        includes = content.include_paths,
    )
    with open(f"{path}/srv/{srv_name}.h", "w", encoding="utf-8") as f:
        f.write(header_content)

def generate_service_tester(path: Path, content: Content):
    srv_name = content.name
    messages = content.messages
    assert len(messages) == 2, "Number of message object is supposed to be 2"
    request = messages[0]
    response = messages[1]
    server_template = jinja2_env.get_template(SERVICE_SERVER)
    server_content = server_template.render(
        srv_name = srv_name,
        srv_unique_name = request.prefix + srv_name,
        request = request,
        response = response,
    )
    with open(f"{path}/{srv_name}_server.c", "w", encoding="utf-8") as f:
        f.write(server_content)
    client_template = jinja2_env.get_template(SERVICE_CLIENT)
    client_content = client_template.render(
        srv_name = srv_name,
        srv_unique_name = request.prefix + srv_name,
        request = request,
        response = response,
    )
    with open(f"{path}/{srv_name}_client.c", "w", encoding="utf-8") as f:
        f.write(client_content)
    makefile_template = jinja2_env.get_template(MAKEFILE_TEMPLATE)
    makefile_content = makefile_template.render(
        preprocessor = f"{srv_name}.c",
        target1 = "server",
        target2 = "client",
        source1 = f"{srv_name}_server.c",
        source2 = f"{srv_name}_client.c",
        external_sources = content.external_sources,
    )
    with open(f"{path}/Makefile", "w", encoding="utf-8") as f:
        f.write(makefile_content)

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
