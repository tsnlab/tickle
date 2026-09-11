# ROS 2 parser fixtures

Real, unmodified `.msg` / `.srv` files, kept in their real `pkg/msg/Name.msg` layout (the
vendored parser validates the message name derived from that path). Used only to prove
`tickle_typesupport._rosidl_parser` accepts real ROS 2 interface files, not just TickLE's own
small `examples/*.msg` / `*.srv` - see `../test_parse_smoke.py`. Not part of TickLE's own
interface set and never fed to code generation.

All fetched from `ros2` GitHub repositories at the `jazzy` ref, licensed **Apache License 2.0**
by Open Source Robotics Foundation, Inc. (per each source package's own `package.xml`):

| File | Source |
|---|---|
| `std_msgs/msg/Header.msg` | `ros2/common_interfaces/std_msgs` |
| `sensor_msgs/msg/Image.msg` | `ros2/common_interfaces/sensor_msgs` |
| `geometry_msgs/msg/{Twist,Vector3}.msg` | `ros2/common_interfaces/geometry_msgs` |
| `std_srvs/srv/SetBool.srv` | `ros2/common_interfaces/std_srvs` |
| `builtin_interfaces/msg/Time.msg` | `ros2/rcl_interfaces/builtin_interfaces` |
| `diagnostic_msgs/msg/DiagnosticStatus.msg` | `ros2/common_interfaces/diagnostic_msgs` |
