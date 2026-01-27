// Generated code

#include "{{ msg_name }}"

#include <tickle/hal.h>

struct tt_Service {{ msg_name }}Service = {
    .name = "{{ msg_name }}Service",
    .request_size = sizeof(struct {{ msg_name }}Request),
    .response_size = sizeof(struct {{ msg_name }}Response),
    .request_encode_size = (tt_REQUEST_ENCODE_SIZE){{ msg_name }}Request_encode_size,
    .request_encode = (tt_REQUEST_ENCODE){{ msg_name }}Request_encode,
    .request_decode = (tt_REQUEST_DECODE){{ msg_name }}Request_decode,
    .request_free = (tt_REQUEST_FREE){{ msg_name }}Request_free,
    .response_encode_size = (tt_RESPONSE_ENCODE_SIZE){{ msg_name }}Response_encode_size,
    .response_encode = (tt_RESPONSE_ENCODE){{ msg_name }}Response_encode,
    .response_decode = (tt_RESPONSE_DECODE){{ msg_name }}Response_decode,
    .response_free = (tt_RESPONSE_FREE){{ msg_name }}Response_free,
    .call_retry_interval = 0, // 0 means auto
    .call_retry_count = 3,
};

int32_t {{ msg_name }}Request_encode_size(struct {{ msg_name }}Request* request) {
    return
    {-% for field in fields %}
    sizeof(data->{{ field.name }}) 
        {%- if field != fields[-1] %} +{% endif %}
    {%- endfor %};
}

int32_t {{ msg_name }}Request_encode(struct {{ msg_name }}Request* request, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

{% for field in fields %}
    // encode - {{ field.type_name }} {{ field.name }}
    if (encoded + sizeof({{ field.type_name }}) > len) {
        return -1;
    }
{# *({{ field.type_name }}*)payload = data->{{field.name}}; #}
    memcpy(payload, &data->{{ field.name }}, sizeof({{ field.type_name }}));
    encoded += sizeof({{ field.type_name }});
    payload += sizeof({{ field.type_name }});

{% endfor %}
    return encoded;
}

int32_t SetBoolRequest_decode(struct SetBoolRequest* request, const uint8_t* payload, const int32_t len,
                              bool is_native_endian) {
    int32_t decoded = 0;

{% for field in fields %}
    // decode - {{ field.type_name }} {{ field.name }}
    memcpy(&data->{{ field.name }}, payload, sizeof({{ field.type_name }}));
{# data->{{ field.name }} = *({{field.type_name}}*)payload; #}
    decoded += sizeof({{ field.type_name }});
    payload += sizeof({{ field.type_name }});

{% endfor %}
    return decoded;
}

void SetBoolRequest_free(struct SetBoolRequest* request) {
    // Do nothing
}

int32_t SetBoolResponse_encode_size(struct SetBoolResponse* response) {
    return sizeof(bool) +                                            // encode success
           sizeof(uint16_t) +                                        // encode message length
           _tt_strnlen(response->message, tt_MAX_STRING_LENGTH) + 1; // encode message
}

int32_t SetBoolResponse_encode(struct SetBoolResponse* response, uint8_t* payload, const int32_t len) {
    int32_t encoded = 0;

    // encode success
    if (encoded + sizeof(bool) > len) {
        return -1;
    }

    *(bool*)payload = response->success;

    encoded += sizeof(bool);
    payload += sizeof(bool);

    // encode message length
    if (encoded + sizeof(uint16_t) > len) {
        return -1;
    }

    uint16_t message_length = _tt_strnlen(response->message, tt_MAX_STRING_LENGTH) + 1; // including '\0'
    if (message_length > tt_MAX_STRING_LENGTH) {
        return -2;
    } else {
        ; // Do nothing
    }

    *(uint16_t*)payload = message_length;

    encoded += sizeof(uint16_t);
    payload += sizeof(uint16_t);

    // encode message
    if (encoded + message_length > len) {
        return -1;
    }

    // Check if message is valid before copying
    if (response->message == NULL) {
        return -3; // Invalid message pointer
    }

    memcpy(payload, response->message, message_length);
    payload += message_length;
    encoded += message_length;

    return encoded;
}

int32_t SetBoolResponse_decode(struct SetBoolResponse* response, const uint8_t* payload, const int32_t len,
                               bool is_native_endian) {
    int32_t decoded = 0;

    // decode success
    if (decoded + sizeof(bool) > len) {
        return -1;
    }

    response->success = *(bool*)payload;

    decoded += sizeof(bool);
    payload += sizeof(bool);

    // decode message length
    if (decoded + sizeof(uint16_t) > len) {
        return -1;
    }

    uint16_t message_length = *(uint16_t*)payload;
    if (!is_native_endian) {
        message_length = _tt_bswap_16(message_length);
    }

    // Validate message length
    if (message_length == 0 || message_length > tt_MAX_STRING_LENGTH) {
        return -2; // Invalid message length
    }

    decoded += sizeof(uint16_t);
    payload += sizeof(uint16_t);

    // decode message
    if (decoded + message_length > len) {
        return -1;
    }

    response->message = (char*)payload;

    decoded += message_length;
    payload += message_length;

    return decoded;
}

void SetBoolResponse_free(struct SetBoolResponse* response) {
    // Do nothing
}
