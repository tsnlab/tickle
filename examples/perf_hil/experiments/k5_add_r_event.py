import sys

p = sys.argv[1]
s = open(p).read()
old = "        if (path && !proxy->probe_busy) {\n            note_recovery_sample(proxy, took);\n        }\n"
assert s.count(old) == 1
new = ("        R4T('R', took / 1000U, (proxy->retry & 0xffU) | ((uint32_t)path << 8) | ((uint32_t)plain << 9) |\n"
       "                                  ((uint32_t)proxy->probe_busy << 10) | ((proxy->recovery_srtt_ns / 1000U) << 11));\n"
       + old +
       "        R4T('r', proxy->recovery_srtt_ns / 1000U, proxy->recovery_rttvar_ns / 1000U);\n")
open(p, 'w').write(s.replace(old, new))
print("patched", p)
