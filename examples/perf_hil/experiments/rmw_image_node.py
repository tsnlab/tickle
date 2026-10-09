#!/usr/bin/env python3
# Copyright (c) 2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""Large-message stage 2 (docs/DESIGN.md section 8): a sensor_msgs/Image stream through rclpy, for rmw_image_pc.sh.

  rmw_image_node.py pub --width W --height H --rate HZ --duration S [--best-effort]
  rmw_image_node.py sub --width W --height H --duration S [--best-effort]

rgba8, so the data is W x H x 4 bytes: 512 x 512 is the design's I1 (1,048,576 B), 1024 x 1024 its I4 (4,194,304 B).
RELIABLE KEEP_LAST 10 (ROS's default) or, with --best-effort, sensor_data (BEST_EFFORT KEEP_LAST 5). Two data
buffers alternate by sample, each with its CRC known to both ends, so the subscriber checks every byte (torn=); the
header's stamp is CLOCK_MONOTONIC at publish (one host, so one clock) and the frame_id the sample's index.
Latency is publish to callback and includes rclpy's own deserialisation - a Python figure, not rmw_tickle's alone.
"""
import argparse
import resource
import sys
import time
import zlib

import rclpy
from rclpy.node import Node
from rclpy.qos import HistoryPolicy, QoSProfile, ReliabilityPolicy
from sensor_msgs.msg import Image


def buffers(size):
    first = bytes((i * 31 + (i >> 11)) & 0xFF for i in range(size))
    second = bytes((i * 17 + 5 + (i >> 9)) & 0xFF for i in range(size))
    return [first, second], [zlib.crc32(first), zlib.crc32(second)]


def qos(best_effort):
    if best_effort:
        return QoSProfile(depth=5, history=HistoryPolicy.KEEP_LAST, reliability=ReliabilityPolicy.BEST_EFFORT)
    return QoSProfile(depth=10, history=HistoryPolicy.KEEP_LAST, reliability=ReliabilityPolicy.RELIABLE)


def cpu_ms():
    usage = resource.getrusage(resource.RUSAGE_SELF)
    return (usage.ru_utime + usage.ru_stime) * 1e3, usage.ru_maxrss


def run_pub(args, node):
    size = args.width * args.height * 4
    data, _ = buffers(size)
    pub = node.create_publisher(Image, "large_image", qos(args.best_effort))
    deadline = time.monotonic() + 2.0  # discovery
    while time.monotonic() < deadline:
        rclpy.spin_once(node, timeout_sec=0.05)
    period = 1.0 / args.rate
    published = 0
    failed = 0
    cpu0, _ = cpu_ms()
    start = time.monotonic()
    nxt = start
    while time.monotonic() - start < args.duration:
        now = time.monotonic()
        if now < nxt:
            rclpy.spin_once(node, timeout_sec=nxt - now)
            continue
        msg = Image()
        msg.height = args.height
        msg.width = args.width
        msg.encoding = "rgba8"
        msg.step = args.width * 4
        msg.data = data[published % 2]
        msg.header.frame_id = str(published)
        stamp = time.monotonic_ns()
        msg.header.stamp.sec = stamp // 1_000_000_000
        msg.header.stamp.nanosec = stamp % 1_000_000_000
        try:
            pub.publish(msg)
            published += 1
        except Exception as error:  # noqa: BLE001 - a refused publish is reported, not fatal
            failed += 1
            print(f"publish failed: {error}", file=sys.stderr)
        nxt += period
    deadline = time.monotonic() + 3.0  # resends answered
    while time.monotonic() < deadline:
        rclpy.spin_once(node, timeout_sec=0.05)
    cpu1, rss = cpu_ms()
    mb = published * size / 1e6
    print(f"RESULT: role=pub size={size} reliable={0 if args.best_effort else 1} published={published} failed={failed} "
          f"cpu_ms={cpu1 - cpu0:.1f} cpu_ms_per_mb={(cpu1 - cpu0) / mb if mb else 0:.3f} peak_rss_kb={rss}")


def run_sub(args, node):
    size = args.width * args.height * 4
    _, crcs = buffers(size)
    state = {"delivered": 0, "torn": 0, "out_of_order": 0, "last": -1, "latencies": [], "seen": set()}

    def on_image(msg):
        now = time.monotonic_ns()
        index = int(msg.header.frame_id)
        if len(msg.data) != size or zlib.crc32(bytes(msg.data)) != crcs[index % 2]:
            state["torn"] += 1
            return
        if index <= state["last"]:
            state["out_of_order"] += 1
        state["last"] = index
        state["seen"].add(index)
        sent = msg.header.stamp.sec * 1_000_000_000 + msg.header.stamp.nanosec
        state["latencies"].append((now - sent) / 1e3)
        state["delivered"] += 1

    node.create_subscription(Image, "large_image", on_image, qos(args.best_effort))
    cpu0, _ = cpu_ms()
    deadline = time.monotonic() + args.duration
    while time.monotonic() < deadline:
        rclpy.spin_once(node, timeout_sec=0.05)
    cpu1, rss = cpu_ms()
    lat = sorted(state["latencies"])
    median = lat[len(lat) // 2] if lat else 0.0
    p99 = lat[(len(lat) * 99) // 100] if lat else 0.0
    mb = state["delivered"] * size / 1e6
    print(f"RESULT: role=sub size={size} reliable={0 if args.best_effort else 1} delivered={state['delivered']} "
          f"torn={state['torn']} out_of_order={state['out_of_order']} latency_median_us={median:.1f} "
          f"latency_p99_us={p99:.1f} cpu_ms={cpu1 - cpu0:.1f} cpu_ms_per_mb={(cpu1 - cpu0) / mb if mb else 0:.3f} "
          f"peak_rss_kb={rss} last_index={state['last']} "
          f"missing={','.join(str(i) for i in range(state['last']) if i not in state['seen'])[:80] or 'none'}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("role", choices=["pub", "sub"])
    parser.add_argument("--width", type=int, default=512)
    parser.add_argument("--height", type=int, default=512)
    parser.add_argument("--rate", type=float, default=30.0)
    parser.add_argument("--duration", type=float, default=10.0)
    parser.add_argument("--best-effort", action="store_true")
    args = parser.parse_args()
    # No /rosout and no type-description service: their interface types are not part of what this measures.
    rclpy.init(args=["--ros-args", "--disable-rosout-logs", "-p", "start_type_description_service:=false"])
    node = Node(f"large_image_{args.role}")
    try:
        run_pub(args, node) if args.role == "pub" else run_sub(args, node)
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    main()
