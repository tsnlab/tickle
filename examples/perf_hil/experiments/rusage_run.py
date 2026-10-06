#!/usr/bin/env python3
"""Run a command and print its own resource usage on stderr when it exits, as one line:
   RUSAGE pid=P minflt=N majflt=M nvcsw=V nivcsw=I exit=E
The counts are the child's, from wait4(), not this wrapper's. Used by p4_reorder_firsttouch.sh to count the
first-touch page faults each bench process takes; SIGINT and SIGTERM are forwarded to the child so a harness that
stops "the server" stops the bench, and the line is still printed."""
import os
import signal
import subprocess
import sys

child = subprocess.Popen(sys.argv[1:])
for sig in (signal.SIGINT, signal.SIGTERM):
    signal.signal(sig, lambda s, _f: child.send_signal(s))
while True:
    try:
        _, status, ru = os.wait4(child.pid, 0)
        break
    except InterruptedError:
        continue
code = os.waitstatus_to_exitcode(status)
print(f"RUSAGE pid={child.pid} minflt={ru.ru_minflt} majflt={ru.ru_majflt} nvcsw={ru.ru_nvcsw} "
      f"nivcsw={ru.ru_nivcsw} exit={code}", file=sys.stderr, flush=True)
sys.exit(code if code >= 0 else 128 - code)
