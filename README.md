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

## License
GPLv3 or proprietary license on request
