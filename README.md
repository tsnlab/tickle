# TickLE: Fastest ROS2 middleware optimized for 10Base-T1S

## Build

```sh
$ make all      # Run unit tests, build the library, then build all examples
$ make library  # Build libtickle.a only
$ make examples # Build all examples without running unit tests
$ make set_bool # Build SetBool client/server examples
$ make uint64   # Build UInt64 publisher/subscriber examples
```

## Unit Tests

```sh
$ make test
```

`make test` builds each unit test with coverage instrumentation, runs the tests, and writes coverage reports to `tests/coverage/` only when all tests pass.

### Adding Unit Tests

Add one focused test file per behavior using the `tests/test_*.c` naming pattern, for example `tests/test_callresponse.c`. `make test` discovers those files automatically and builds one binary per test file under `obj/tests/`.

A typical test file should:

1. Include `tests/test_common.h` for assertions. Define `TEST_COMMON_DEFINE_STORAGE` before including it in exactly one file per test binary.
2. Include `tests/test_mock.h` when the test needs HAL/time mocks. Define `TEST_MOCK_DEFINE_STORAGE` before including it in exactly one file per test binary.
3. Include the implementation file under test, such as `#include "../src/tickle.c"`, when static functions need to be exercised without changing production visibility.
4. Add focused test functions for one behavior or regression.
5. Call those test functions from `main()`, then return `test_result()`.

Minimal structure:

```c
#define TEST_COMMON_DEFINE_STORAGE
#include "test_common.h"

#define TEST_MOCK_DEFINE_STORAGE
#include "test_mock.h"

#include "../src/tickle.c"

static void test_some_behavior(void) {
    test_mock_reset();
    /* Arrange inputs, call the code under test, then assert results. */
    EXPECT_TRUE(true);
}

int main(void) {
    test_some_behavior();
    return test_result();
}
```

Run `make test` after adding the file. Coverage reports are written to `tests/coverage/`; review the matching `.gcov` file for the code under test.

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
