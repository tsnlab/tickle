# Nests Leaf with no package prefix - ROS 2's own grammar resolves this to *this* package
# (test_msgs, in the ros2_cli.py tests that use this fixture) rather than requiring "test_msgs/Leaf"
# spelled out - see tickle_typesupport._rosidl_parser's own Type.__init__ (context_package_name).
# Mirrors test_msgs/msg/Nested.msg's own real-world shape (`BasicTypes basic_types_value`) - the
# actual gap this fixture exists to exercise (resolve.Ros2Resolver's own same-package-sibling
# case), just with a smaller, hand-picked leaf type instead of test_msgs' own real BasicTypes.
int32 tag
Leaf leaf_value
