# Test-only fixture (not a real TickLE example) - exercises bounded string capacity (DESIGN.md's
# "Capacity" rule, PLAN.md's mirror of it): a ROS 2 upper-bounded string (`string<=N`,
# capacity_source "bounded") carrying its own default value, a plain `string` pinned by an
# explicit `@capacity` annotation (capacity_source "annotation"), and a plain unbounded string
# alongside them to prove the existing alias-only `char*` path is untouched. Kept to three fields
# (rather than one per capacity-source combination) to stay under clang-tidy's
# readability-function-cognitive-complexity threshold for the generated *_decode() - each string
# field's bounds-checking already contributes several branches on its own.
string<=8 bounded_name "hi"
string annotated_name  # @capacity 4
string plain_name
