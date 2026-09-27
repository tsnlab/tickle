# Every primitive type as a fixed array, a bounded sequence and an unbounded sequence (2026-09-27): what
# test_primitives_cpp.cpp round-trips through the C++ converters, which since RMW_PERF_PLAN.md 12.1 copy these
# with std::copy / assign() rather than element loops. The unbounded ones get their capacities from
# capacities/rosidl_typesupport_tickle_c_tests.capacities. No bool[<=3]: this build of rosidl_typesupport_fastrtps_cpp,
# generated alongside ours, fails to compile a bounded bool sequence (it binds a vector<bool>::reference to a
# u16string&) - not TickLE's to fix, and bool's std::vector<bool> is exercised by bool_sequence.
bool[3] bool_array
bool[] bool_sequence
byte[3] byte_array
byte[<=3] byte_bounded
byte[] byte_sequence
char[3] char_array
char[<=3] char_bounded
char[] char_sequence
float32[3] float32_array
float32[<=3] float32_bounded
float32[] float32_sequence
float64[3] float64_array
float64[<=3] float64_bounded
float64[] float64_sequence
int8[3] int8_array
int8[<=3] int8_bounded
int8[] int8_sequence
uint8[3] uint8_array
uint8[<=3] uint8_bounded
uint8[] uint8_sequence
int16[3] int16_array
int16[<=3] int16_bounded
int16[] int16_sequence
uint16[3] uint16_array
uint16[<=3] uint16_bounded
uint16[] uint16_sequence
int32[3] int32_array
int32[<=3] int32_bounded
int32[] int32_sequence
uint32[3] uint32_array
uint32[<=3] uint32_bounded
uint32[] uint32_sequence
int64[3] int64_array
int64[<=3] int64_bounded
int64[] int64_sequence
uint64[3] uint64_array
uint64[<=3] uint64_bounded
uint64[] uint64_sequence
