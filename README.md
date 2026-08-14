# TickLE: Real-Time ROS2 communication middleware optimized for 10Base-T1S

## Build

```sh
$ make all      # Run unit tests, build the library, then build all examples
$ make library  # Build libtickle.a only
$ make examples # Build all examples without running unit tests
$ make set_bool # Build SetBool client/server examples
$ make uint64   # Build UInt64 publisher/subscriber examples
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
