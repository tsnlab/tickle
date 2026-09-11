# A copy of sensor_msgs/Image (see tests/fixtures_ros2/sensor_msgs/msg/Image.msg, kept verbatim
# there for parser-fidelity testing - never fed to code generation) with one change: `data`'s
# `# @capacity <N>` annotation added below. The real upstream file leaves `data` a plain
# unbounded `uint8[]` with nothing else to bound it - auto-derivation refuses that on purpose
# (adapt._resolve_auto_capacities) once *any* preceding field isn't fixed-size, which `header`
# (nests std_msgs/Header, which has a `string frame_id`) and `encoding` (a plain string) both
# are here, so there's no safe worst-case budget to auto-derive from. Exercises a real ROS 2
# message's nested type (std_msgs/Header, itself nesting builtin_interfaces/Time - both resolved
# via tickle_typesupport.builtins, no -I needed) together with M2's array support in one message.
#
# This message contains an uncompressed image
# (0, 0) is at top-left corner of image

std_msgs/Header header # Header timestamp should be acquisition time of image
                             # Header frame_id should be optical frame of camera
                             # origin of frame should be optical center of cameara
                             # +x should point to the right in the image
                             # +y should point down in the image
                             # +z should point into to plane of the image
                             # If the frame_id here and the frame_id of the CameraInfo
                             # message associated with the image conflict
                             # the behavior is undefined

uint32 height                # image height, that is, number of rows
uint32 width                 # image width, that is, number of columns

# The legal values for encoding are in file include/sensor_msgs/image_encodings.hpp
# If you want to standardize a new string format, join
# ros-users@lists.ros.org and send an email proposing a new encoding.

string encoding       # Encoding of pixels -- channel meaning, ordering, size
                      # taken from the list of strings in include/sensor_msgs/image_encodings.hpp

uint8 is_bigendian    # is this data bigendian?
uint32 step           # Full row length in bytes
uint8[] data  # @capacity 1400 -- actual matrix data, size is (step * rows)
