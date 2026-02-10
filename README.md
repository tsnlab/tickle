# TickLE: Fastest ROS2 middleware optimized for 10Base-T1S

## Compile

```sh
$ make
```

## Run examples
```sh
$ make createns
$ make runserver     # Launch SetBool service server on ns2 namespace
$ make runclient     # Launch SetBool service client on ns1 namespace
$ make runpublisher  # Launch UInt64 topic publisher on ns1 namespace
$ make runsubscriber # Launch UInt64 topic subscriber on ns2 namespace
```

## Run topic example with custom ROS2 message
### Install dependancy packages
```
$ python3 -m pip install ros-rosidl-adapter
$ python3 -m pip install ros-rosidl-parser
```
### Create `msg` file
```
$ cat << EOF > examples/Message.msg
uint16      header
uint32[10]  body
EOF
```
### Generate preprocessor
```
$ make Message.msg
```
### List of generated files
Base directory is automatically generated according to name of `.msg` file. It is `message` in this example.
```
examples/message/Makefile
examples/message/Message.c      # preprocessor source
examples/message/msg/Message.h  # preprocessor header
examples/message/Message_pub.c  # publisher
examples/message/Message_sub.c  # subscriber
```
Modify default publisher/subscriber code as needed.
### Run
Refer to [__Run examples__](#run-examples) above.

## License
GPLv3 or proprietary license on request
