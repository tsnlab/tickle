# Direct codec: a struct that starts at alignment 1 but self-aligns to 4 (so a sequence of it pads
# between elements), holding a string with a profile capacity, a bounded string and a sequence.
uint8 tag
string name
string<=8 label
float64[] values
int16 small
uint32 id
