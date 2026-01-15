MSG_SOURCE_TEMPLATE = '''// Generated code

#include "{msg_name}.h"

#include <tickle/hal.h>

struct tt_Topic {msg_name}Topic = {{
    .name = "{msg_name}Topic",
    .data_size = sizeof(struct {msg_name}Data),
    .data_encode_size = (tt_DATA_ENCODE_SIZE){msg_name}Data_encode_size,
    .data_encode = (tt_DATA_ENCODE){msg_name}Data_encode,
    .data_decode = (tt_DATA_DECODE){msg_name}Data_decode,
    .data_free = (tt_DATA_FREE){msg_name}Data_free,
}};

int32_t {msg_name}Data_encode_size(struct {msg_name}Data* data) {{
    return {encode_size_template};
}}

int32_t {msg_name}Data_encode(struct {msg_name}Data* data, uint8_t* payload, const int32_t len) {{
    int32_t encoded = 0;
{encode_template}

    return encoded;
}}

int32_t {msg_name}Data_decode(struct {msg_name}Data* data, const uint8_t* payload, const int32_t len, bool is_native_endian) {{
    int32_t decoded = 0;
{decode_template}

    return decoded;
}}

void {msg_name}Data_free(struct {msg_name}Data* data) {{
{free_template}
}}
'''

DUMMY_PLACEHOLDER = '    // Do nothing'

PRIMITIVE_ENCODE_SIZE_TEMPLATE = 'sizeof({type_name})'

PRIMITIVE_ENCODE_TEMPLATE = '''
    if (encoded + sizeof({type_name}) > len) {{
        return -1;
    }}

    *({type_name}*)payload = data->data;

    encoded += sizeof({type_name});
    payload += sizeof({type_name});'''

PRIMITIVE_DECODE_TEMPLATE = '''
    if (decoded + sizeof({type_name}) > len) {{
        return -1;
    }}

    data->data = *({type_name}*)payload;

    decoded += sizeof({type_name});
    payload += sizeof({type_name});'''

PRIMITIVE_FREE_TEMPLATE = ''


MSG_HEADER_TEMPLATE = '''// Generated code
#pragma once

#include <tickle/tickle.h>

struct {msg_name}Data {{
{fields_template}
}};

extern struct tt_Topic {msg_name}Topic;

int32_t {msg_name}Data_encode_size(struct {msg_name}Data* data);
int32_t {msg_name}Data_encode(struct {msg_name}Data* data, uint8_t* payload, const int32_t len);
int32_t {msg_name}Data_decode(struct {msg_name}Data* data, const uint8_t* payload, const int32_t len, bool is_native_endian);
void {msg_name}Data_free(struct {msg_name}Data* data);
'''

FIELDS_TEMPLATE = '    {type_name} {name};'
