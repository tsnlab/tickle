# TickLE
TickLE is fastest ROS2 middleware optimized for 10Base-T1S.

## Features
- Topic (Pub/Sub)
- Service (server/client)
- ROS 2 interface (`.msg`/`.srv`)

## Installation
### TickLE Library
Build library
```bash
make
```
### Preprocessor dependencies
Install ROS 2 message interface parser. 
```bash
pip install ros-rosidl-adapter ros-rosidl-parser
```
Use poetry or venv to manage python virtual environment.

## Usage example

### Preprocessor
Preprocessor handles tasks related to message such as serialization/deserialization and provides header files that application references.


#### Create `.msg` file
Preprocessor is generated from message definition file(`.msg`).
```
cat > examples/MyMessage.msg << EOF
uint8 header
uint64 body
EOF
```
For details of `.msg`, refer to [ROS 2 interface](https://docs.ros.org/en/foxy/Concepts/About-ROS-Interfaces.html).

#### Generate preprocessor
```bash
python3 tools/main.py examples/my_app examples/MyMessage.msg
```
Files created:
```
examples/my_app/MyMessage.c
examples/my_app/msg/MyMessage.h
```

### Example code
- [publisher.c](examples/my_app/publisher.c)
- [subscriber.c](examples/my_app/subscriber.c)

#### Build
```bash
cd examples/my_app
gcc subscriber.c MyMessage.c -o subscriber -ltickle -I.. -I../../include -L../..
gcc publisher.c MyMessage.c -o publisher -ltickle -I.. -I../../include -L../..
```

### Network namespace
By default, network namespaces are required for TickLE nodes to work.
To create the network namespaces,
```bash
make -C../.. createns
```
To remove,
```bash
make -C../.. deletens
```

### Run
#### Publisher
```bash
sudo ip netns exec ns1 ./publisher
```

#### Subscriber
Open a new terminal and run
```bash
sudo ip netns exec ns2 ./subscriber
```

## License
GPLv3 or proprietary license on request
