# A minimal message just for rosidl_typesupport_tickle_c_tests/test/test_dispatch.c to round-trip
# through the real dispatch chain - deliberately trivial (one scalar field), since the field-by-
# field conversion logic itself is already exhaustively covered offline by tools/typesupport/
# tests/test_ros2_adapter.py (arrays, bounded/unbounded strings, capacity rejection).
uint64 value
