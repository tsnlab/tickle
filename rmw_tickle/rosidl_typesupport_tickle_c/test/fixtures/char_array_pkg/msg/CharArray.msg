# Test-only fixture (not a real TickLE example) - isolated on purpose (not folded into the
# shared fixtures_own/Arrays.msg used by test_crossendian.py/test_golden.py/test_lint.py, whose
# own cognitive-complexity/golden-output coverage a fourth array field would perturb) - exercises
# a real, previously-latent bug (Milestone 55): a real ROS 2 install's own generated struct member
# for an IDL `char[<=N]` field is `rosidl_runtime_c__uint8__Sequence`, not `rosidl_runtime_c__
# char__Sequence` - see ros2_adapter._ros2_sequence_scalar_type()'s own doc comment for the story.
char[<=3] char_values
