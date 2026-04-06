# TickLE Usage Guide
## Introduction
TickLE is a middleware optimized for 10Base-T1S, aimed to be used in ROS as DDS middleware. TickLE is in development stage and you can use basic functionality of TickLE by using TickLE alone or through ROS 2. This guide provides guide to run a simple application written in C using TickLE alone.

## Build & Setup
### Requirements
Versions used in testing environment are in parenthesis
- Linux (Ubuntu 24.04)
- GNU Make
- gcc or clang (gcc 13.3)
- Python 3.12 or later (python 3.12)


Any environtments satisfying above list should work.

### TickLE Library
To build static library `libtickle.a`, run following command in the root directory of this repository.
```
make
```

### Network namespace
Default configuration of TickLE requires nodes to be executed in separate network namespace.


To create the network namespace with default preset,
```
make createns
```
To remove,
```
make deletens
```
### Message
Data sent from publisher to topic subscriber is called message. Structure of the message is defined in a `.msg` file.


Simple message example:
```
uint8       header
uint64      body
```
This message consists of two fields.
In first field, `uint8` is type of the field and `header` is name of the field.
Refer to [ROS 2 message writing rules](https://docs.ros.org/en/jazzy/Concepts/Basic/About-Interfaces.html) for detail.


Although TickLE follows the rule, it does not support all types listed in the reference. Supported types are statically sized types such as integer, FP number.
If you use TickLE with ROS 2, it supports all types.

#### Dependencies
Install message file converter and parser.


To install dependencies run either
```
pip install ros-rosidl-adapter ros-rosidl-parser
```
or
```
sudo apt-get install ros-jazzy-rosidl-adapter ros-jazzy-rosidl-parser
```

#### Generate Header files
Before proceeding, create `.msg` file like
```
cat > examples/Simple.msg << EOF
uint8 header
uint64 body
EOF
```

Following command will generate header files.
```
python3 tools/main.py <target-dir> <msg-file>
```
Substitute bracketed names. For example,
```
python3 tools/main.py examples/test examples/Simple.msg
```
Files created:
```
examples/test/Simple.c            # encoder/decoder source
examples/test/msg/Simple.h        # message struct, TickLE topic struct
examples/test/msg/Simple.idl
examples/test/msg/Simple.msg
```

## Example Code
**publisher.c**
```
#include <stdio.h>
#include <time.h>
#include <unistd.h>

// Include the header file containing message struct
#include "msg/Simple.h"

static void publisher_callback(struct tt_Node* node, uint64_t t, void* param) {
    int ret;
    struct tt_Publisher* publisher = param;

    // Unique identifier for message struct consists of
    // 1. 'test' directory name
    // 2. 'msg' either msg or srv
    // 3. 'Simple' message name
    struct test__msg__SimpleData data = {
        .header = 1,
        .body = time(NULL),
    };

    // Publish data
    ret = tt_Publisher_publish(publisher, (struct tt_Data*)&data);
    if (ret < 0) {
        fprintf(stderr, "failed to publish\n");
        return;
    }

    // Schedule this function to execute after 1 second
    tt_Node_schedule(node, tt_get_ns() + tt_SECOND, publisher_callback, publisher);
    printf("published time=%lu\n", data.body);
}

int main(void) {
    int ret;
    struct tt_Node node;
    struct tt_Publisher publisher;
    
    // Broadcast to created network namespace
    _tt_CONFIG.broadcast = "192.168.10.255";

    // Create TickLE Node
    ret = tt_Node_create(&node);
    if (ret < 0) {
        fprintf(stderr, "failed to create node\n");
        return 1;
    }

    // Create Publisher
    ret = tt_Node_create_publisher(&node, &publisher, &test__msg__SimpleTopic, "simple_topic");
    if (ret < 0) {
        fprintf(stderr, "failed to create node\n");
        return 1;
    }

    // Schedule the callback function to execute after 1 second
    tt_Node_schedule(&node, tt_get_ns() + tt_SECOND, publisher_callback, &publisher);

    // Run node's scheduled tasks & transmission
    tt_Node_poll(&node);
    tt_Node_destroy(&node);
    return 0;
}
```



**subscriber.c**
```
#include <stdio.h>

#include "msg/Simple.h"

void simple_callback(struct tt_Subscriber* subscriber, uint64_t timestamp,
                     uint16_t seq_no, struct test__msg__SimpleData* data) {
    if (data->header == 1) {
        printf("seq=%05u time=%lu\n", seq_no, data->body);
    } else {
        printf("invalid data");
    }
}

int main(void) {
    int ret;
    struct tt_Node node;
    struct tt_Subscriber subscriber;
    struct test__msg__SimpleData data;

    _tt_CONFIG.broadcast = "192.168.10.255";
    ret = tt_Node_create(&node);
    if (ret < 0) {
        fprintf(stderr, "failed to create node\n");
        return 1;
    }

    ret = tt_Node_create_subscriber(&node, &subscriber, &test__msg__SimpleTopic,
        "simple_topic", (tt_SUBSCRIBER_CALLBACK)simple_callback);
    if (ret < 0) {
        fprintf(stderr, "failed to create subscriber\n");
        return 1;
    }

    tt_Node_poll(&node);
    tt_Node_destroy(&node);
    return 0;
}
```

### Build example
```
cd examples/test
gcc subscriber.c Simple.c -o subscriber -ltickle -I.. -I../../include -L../..
gcc publisher.c Simple.c -o publisher -ltickle -I.. -I../../include -L../..
```
### Run
#### Publisher
```
sudo ip netns exec ns1 ./publisher
```
#### Subscriber
```
sudo ip netns exec ns2 ./subscriber
```
