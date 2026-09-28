# The shape of actionlib_msgs/GoalStatus: a nested type ending in a string, then a uint8, then a
# string. CI: old 25 B, new 24 B, first difference at byte 8.
DcGoalId goal_id
uint8 status
string text
