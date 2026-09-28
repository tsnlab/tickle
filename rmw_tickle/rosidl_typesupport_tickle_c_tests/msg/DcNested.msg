# Direct codec: sequences of strings, of structs holding strings and of structs holding sequences,
# bounded strings and sequences, fixed arrays of strings and of structs, and a bool sequence.
string[] names
string[3] fixed_names
DcElem[] elems
DcElem[2] fixed_elems
DcElem[<=3] bounded_elems
DcOuter[] outers
string<=5 short_name
uint32[<=4] bounded_u32
bool[] flags
float32 f
DcInner[] inners
int8 last
