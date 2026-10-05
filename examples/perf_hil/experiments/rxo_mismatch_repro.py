#!/usr/bin/env python3
"""One side of rxo_mismatch_repro.sh: a reliable (or best-effort) KEEP_ALL publisher or subscriber on /rxo_probe.

Used instead of perf_test when perf_test's interface typesupport does not match the rmw_tickle under test.
usage: rxo_mismatch_repro.py pub|sub reliable|best_effort SECONDS
Prints 'RXO_PY <role> start', and at the end 'RXO_PY <role> done sent=N' or 'received=N'.
"""
import sys
import time

import rclpy
from rclpy.qos import HistoryPolicy, QoSProfile, ReliabilityPolicy
from std_msgs.msg import UInt8MultiArray

role, rel, seconds = sys.argv[1], sys.argv[2], float(sys.argv[3])
rclpy.init()
node = rclpy.create_node(f'rxo_{role}', parameter_overrides=[])
qos = QoSProfile(history=HistoryPolicy.KEEP_ALL, depth=1000,
                 reliability=ReliabilityPolicy.RELIABLE if rel == 'reliable' else ReliabilityPolicy.BEST_EFFORT)
print(f'RXO_PY {role} start', flush=True)
count = 0
end = time.monotonic() + seconds
if role == 'sub':
    def cb(_msg):
        global count
        count += 1
    node.create_subscription(UInt8MultiArray, 'rxo_probe', cb, qos)
    while time.monotonic() < end:
        rclpy.spin_once(node, timeout_sec=0.05)
    print(f'RXO_PY sub done received={count}', flush=True)
else:
    pub = node.create_publisher(UInt8MultiArray, 'rxo_probe', qos)
    # Wait for the match, but not for ever: a publisher that never matches still publishes (as perf_test does once
    # its wait times out), because the subscriber's RxO check only speaks when DATA reaches it.
    wait_end = time.monotonic() + 1.0
    while pub.get_subscription_count() < 1 and time.monotonic() < wait_end:
        time.sleep(0.01)
    print(f'RXO_PY pub matched={pub.get_subscription_count()}', flush=True)
    msg = UInt8MultiArray(data=bytes(4096))
    while time.monotonic() < end:
        try:
            pub.publish(msg)
            count += 1
        except Exception as e:  # a KEEP_ALL publisher that gave up raises here
            print(f'RXO_PY pub publish failed: {e}', flush=True)
            break
        time.sleep(0.001)
    print(f'RXO_PY pub done sent={count}', flush=True)
node.destroy_node()
rclpy.shutdown()
