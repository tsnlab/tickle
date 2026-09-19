# Test-only fixture - deliberately shaped so its own first field's alignment (1, `bool`) diverges
# from its own overall self-alignment (4, driven by `big`) - the array-of-nested-type edge case
# NestedArrays.msg exists to exercise (see model._nested_self_align's own doc comment): a fixed-
# size nested type whose own wire_size (13: 1 + pad3 + 8 + 1) isn't a multiple of its own self-
# alignment, so a real gap is needed *between* consecutive array elements, not just before the
# array as a whole.
bool flag
int64 big
uint8 tail
