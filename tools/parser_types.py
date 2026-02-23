from dataclasses import dataclass
from typing import List, Set, Optional

@dataclass
class Field:
    type_name: str
    name: str
    size: int
    string_size: int
    named: bool
    debug: str | None
    def __init__(self, type_name="", name="", size=0, string_size=0, named=False, debug=None):
        self.type_name = type_name
        self.name = name
        self.size = size
        self.string_size = string_size
        self.named = named
        self.debug = debug


@dataclass
class Message:
    prefix: str
    fields: List[Field]
    def __init__(self, prefix="", fields=None):
        self.prefix = prefix
        if fields == None:
            self.fields = []
        else:
            self.fields = fields

@dataclass
class Content:
    name: str
    pkg_name: str
    messages: List[Message]
    external_sources: Set[str]
    include_paths: List[str]

