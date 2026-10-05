#!/usr/bin/env python3
"""Which ROS 2 QoS policies can each rmw actually express? Asked of the rmw, not of its documentation.

COMPARISON rows 42-47 are DURABILITY, HISTORY, DEADLINE, LIVELINESS and LIFESPAN. Their zenoh columns are empty,
and the question is whether that is a gap or a fact. zenoh-pico has no such policies at all - it is not a DDS - so
its column is settled. rmw_zenoh_cpp is a ROS 2 rmw and ROS 2 defines all five, so for that column the answer has
to come from the rmw rather than from a reading of its name.

Two things are probed separately, because a rmw can accept a QoS and still not report on it:
  create  - does creating a publisher/subscription with the policy succeed, and is the policy readable back?
  event   - can the matching QoS event be subscribed to? Rows 45-47 measure DETECTION, which is an event. A rmw
            that accepts DEADLINE but cannot deliver a deadline-missed event cannot produce those rows' figures.

Run under each rmw:  RMW_IMPLEMENTATION=rmw_tickle python3 rmw_qos_support_probe.py
rmw_zenoh_cpp additionally needs its router (rmw_zenohd) running, or every create fails for an unrelated reason.
"""
import os
import sys

import rclpy
from rclpy.duration import Duration
from rclpy.node import Node
from rclpy.qos import (DurabilityPolicy, HistoryPolicy, LivelinessPolicy, QoSProfile, ReliabilityPolicy)
# builtin_interfaces/Time rather than a std_msgs type: the message type is not the question, and the rig's rmw_tickle
# workspace carries tickle typesupport for builtin_interfaces and not for std_msgs (2026-10-05) - with std_msgs every
# rmw_tickle case was refused for a reason that had nothing to do with QoS.
from builtin_interfaces.msg import Time as ProbeMsg

RMW = os.environ.get("RMW_IMPLEMENTATION", "(default)")


def probe(idx, label, make_qos, event_kwargs=None):
    """Report create and event support separately; an exception in one must not hide the other.

    The topic name is an index, not the policy label. The first version built it from the label, so
    `probe_durability.transient_local` tripped InvalidTopicNameException and EVERY case was refused for a reason
    that had nothing to do with QoS. Running the two DDS rmws first - which support all five policies - is what
    showed it: a probe that refuses everything, including its controls, is not measuring its subject.
    """
    out = {"policy": label, "create": "?", "event": "-"}
    node = None
    try:
        node = Node(f"qos_probe_{idx}")
        qos = make_qos()
        # ONLY the call under test is inside this try. The first version read the profile back here too, and
        # QoSProfile uses __slots__, so my own bookkeeping raised AttributeError and was reported as the rmw
        # refusing the policy - in all six cases, on both control rmws. A probe must not be able to fail on
        # behalf of its subject.
        node.create_publisher(ProbeMsg, f"qos_probe_{idx}", qos)
        out["create"] = "ok"
    except Exception as exc:  # noqa: BLE001 - the exception type is the finding
        out["create"] = f"REFUSED: {type(exc).__name__}: {exc}"[:120]
    if event_kwargs and out["create"] == "ok":
        try:
            node.create_publisher(ProbeMsg, f"qos_probe_evt_{idx}", make_qos(), event_callbacks=event_kwargs())
            out["event"] = "ok"
        except Exception as exc:  # noqa: BLE001
            out["event"] = f"REFUSED: {type(exc).__name__}: {exc}"[:120]
    if node is not None:
        try:
            node.destroy_node()
        except Exception:  # noqa: BLE001
            pass
    return out


def main():
    # The type description service every rclpy node starts by default needs typesupport for its own service type, which
    # is not a QoS question either. Off for every rmw alike, so the controls run exactly what the subjects run.
    rclpy.init(args=["--ros-args", "-p", "start_type_description_service:=false"])
    from rclpy.event_handler import PublisherEventCallbacks
    cases = [
        ("DURABILITY.TRANSIENT_LOCAL",
         lambda: QoSProfile(depth=10, durability=DurabilityPolicy.TRANSIENT_LOCAL,
                            reliability=ReliabilityPolicy.RELIABLE), None),
        ("HISTORY.KEEP_LAST.64",
         lambda: QoSProfile(history=HistoryPolicy.KEEP_LAST, depth=64,
                            reliability=ReliabilityPolicy.RELIABLE), None),
        ("HISTORY.KEEP_ALL",
         lambda: QoSProfile(history=HistoryPolicy.KEEP_ALL, depth=1,
                            reliability=ReliabilityPolicy.RELIABLE), None),
        ("DEADLINE.50ms",
         lambda: QoSProfile(depth=10, deadline=Duration(nanoseconds=50_000_000)),
         lambda: PublisherEventCallbacks(deadline=lambda e: None)),
        ("LIVELINESS.AUTOMATIC.2s",
         lambda: QoSProfile(depth=10, liveliness=LivelinessPolicy.AUTOMATIC,
                            liveliness_lease_duration=Duration(seconds=2)),
         lambda: PublisherEventCallbacks(liveliness=lambda e: None)),
        ("LIFESPAN.100ms",
         lambda: QoSProfile(depth=10, lifespan=Duration(nanoseconds=100_000_000)), None),
    ]
    print(f"rmw={RMW}")
    for idx, (label, mk, evt) in enumerate(cases):
        r = probe(idx, label, mk, evt)
        print(f"  {r['policy']:<28} create={r['create']:<60} event={r['event']}")
    rclpy.shutdown()
    return 0


if __name__ == "__main__":
    sys.exit(main())
