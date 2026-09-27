#!/usr/bin/env python3
# Copyright (c) 2025-2026 TSN Lab, Inc.
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# This file is part of TickLE. TickLE is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 3, as published by the Free
# Software Foundation. A proprietary license is also available on request - see README.md.
"""The nodes rmw_gap_acceptance.sh runs, one role per invocation: accept_node.py ROLE SECONDS [ARG].

Every role is an ordinary default-options rclpy node, as a user would write it, so what passes here passes for a user.
A role prints one line "RESULT: key=value ..." when it ends; the script reads only that line.

  talker        publishes std_msgs/String "msg-<n>" on /accept_chatter at 10 Hz, with a parameter accept_param=42
  listener      counts "msg-" strings received on /accept_chatter (ARG "events": with the EventsExecutor;
                ARG "dump": also prints the received numbers in order, as a SEQ: line)
  matched_pub   a publisher on /accept_matched that counts its PUBLICATION_MATCHED events
  matched_sub   a subscription on /accept_matched that counts its SUBSCRIPTION_MATCHED events
  itype_pub     a std_msgs/String publisher on /accept_itype, counting PUBLISHER_INCOMPATIBLE_TYPE events
  itype_sub     a std_msgs/Int32 subscription on /accept_itype, counting SUBSCRIPTION_INCOMPATIBLE_TYPE events
  durable_pub   TRANSIENT_LOCAL KEEP_LAST 4, publishes 6 sensor_msgs/Image samples of 60,000 bytes, then keeps running
  durable_sub   created later with the same QoS: reports how many backlog samples arrived, which, and whether in order
  inprocess     a talker node and a listener node in ONE process and executor, then a std_srvs/Trigger call between them
"""
import sys
import time

import rclpy
from rclpy.event_handler import PublisherEventCallbacks
from rclpy.event_handler import SubscriptionEventCallbacks
from rclpy.node import Node
from std_msgs.msg import Int32
from std_msgs.msg import String


def spin_for(node, seconds, executor=None):
    end = time.monotonic() + seconds
    while time.monotonic() < end and rclpy.ok():
        if executor is None:
            rclpy.spin_once(node, timeout_sec=0.05)
        else:
            executor.spin_once(timeout_sec=0.05)


def main():
    role, seconds = sys.argv[1], float(sys.argv[2])
    arg = sys.argv[3] if len(sys.argv) > 3 else ''
    rclpy.init()
    counts = {'n': 0}
    executor = None
    if role == 'talker':
        node = Node('accept_talker')
        node.declare_parameter('accept_param', 42)
        pub = node.create_publisher(String, '/accept_chatter', 10)

        def tick():
            counts['n'] += 1
            pub.publish(String(data='msg-%d' % counts['n']))
        node.create_timer(0.1, tick)
        spin_for(node, seconds)
        print('RESULT: role=talker sent=%d' % counts['n'], flush=True)
    elif role == 'listener':
        node = Node('accept_listener')

        seen = []

        def on_msg(msg):
            if msg.data.startswith('msg-'):
                counts['n'] += 1
                if arg == 'dump':
                    seen.append(msg.data)
        node.create_subscription(String, '/accept_chatter', on_msg, 10)
        if arg == 'events':
            from rclpy.experimental import EventsExecutor
            executor = EventsExecutor()
            executor.add_node(node)
        spin_for(node, seconds, executor)
        if arg == 'dump':
            # the numbers received, in order - rmw_gap_acceptance.sh's bag test checks they run without a gap
            print('SEQ: ' + ' '.join(m[4:] for m in seen), flush=True)
        print('RESULT: role=listener executor=%s received=%d' % ('default' if arg == 'dump' else (arg or 'default'),
                                                                   counts['n']), flush=True)
    elif role == 'inprocess':
        # Two nodes in ONE process and one executor (g9): a talker node and a listener node, then a service call from
        # one node to the other's service. Composition, component containers and multi-node processes are this shape.
        from rclpy.executors import SingleThreadedExecutor
        from std_srvs.srv import Trigger
        talker = Node('accept_inproc_talker')
        listener = Node('accept_inproc_listener')
        pub = talker.create_publisher(String, '/accept_inproc', 10)

        def on_msg(msg):
            if msg.data.startswith('msg-'):
                counts['n'] += 1
        listener.create_subscription(String, '/accept_inproc', on_msg, 10)
        talker.create_service(Trigger, '/accept_inproc_srv',
                              lambda req, resp: setattr(resp, 'success', True) or setattr(resp, 'message', 'pong') or resp)
        client = listener.create_client(Trigger, '/accept_inproc_srv')
        executor = SingleThreadedExecutor()
        executor.add_node(talker)
        executor.add_node(listener)
        sent = {'n': 0}

        def tick():
            sent['n'] += 1
            pub.publish(String(data='msg-%d' % sent['n']))
        talker.create_timer(0.1, tick)
        spin_for(talker, seconds * 0.7, executor)
        service_ok = 0
        if client.wait_for_service(timeout_sec=2.0):
            future = client.call_async(Trigger.Request())
            end = time.monotonic() + 3.0
            while not future.done() and time.monotonic() < end:
                executor.spin_once(timeout_sec=0.05)
            service_ok = int(future.done() and future.result() is not None and future.result().message == 'pong')
        print('RESULT: role=inprocess sent=%d received=%d service_ok=%d' % (sent['n'], counts['n'], service_ok),
              flush=True)
        listener.destroy_node()
        node = talker
    elif role in ('durable_pub', 'durable_sub'):
        # TRANSIENT_LOCAL KEEP_LAST 4 with 60,000-byte sensor_msgs/Image samples, fragmented on the wire (g10): the
        # publisher sends 6 and keeps running; a subscriber created afterwards must get exactly the last 4, in order.
        # 60,000 and not smaller: at 16,000 bytes rmw_tickle's first arena already held 4, so the first version of this
        # test passed on the unfixed build (2026-09-28) - it could not fail.
        from rclpy.qos import DurabilityPolicy
        from rclpy.qos import HistoryPolicy
        from rclpy.qos import QoSProfile
        from rclpy.qos import ReliabilityPolicy
        from sensor_msgs.msg import Image
        qos = QoSProfile(depth=4, history=HistoryPolicy.KEEP_LAST, reliability=ReliabilityPolicy.RELIABLE,
                         durability=DurabilityPolicy.TRANSIENT_LOCAL)
        node = Node('accept_' + role)
        if role == 'durable_pub':
            pub = node.create_publisher(Image, '/accept_durable', qos)
            for i in range(1, 7):
                pub.publish(Image(height=1, width=60000, encoding='mono8', step=60000, data=bytes([i]) * 60000))
            spin_for(node, seconds)
            print('RESULT: role=durable_pub sent=6', flush=True)
        else:
            got = []

            def on_msg(msg):
                data = bytes(msg.data)
                if len(data) == 60000 and data == data[:1] * 60000:
                    got.append(data[0])
            node.create_subscription(Image, '/accept_durable', on_msg, qos)
            spin_for(node, seconds)
            print('RESULT: role=durable_sub received=%d first=%d last=%d in_order=%d' % (
                len(got), got[0] if got else 0, got[-1] if got else 0,
                int(got == sorted(got) and len(set(got)) == len(got))), flush=True)
    elif role in ('matched_pub', 'matched_sub'):
        node = Node('accept_' + role)

        def on_matched(info):
            counts['n'] += 1
        if role == 'matched_pub':
            node.create_publisher(String, '/accept_matched', 10,
                                  event_callbacks=PublisherEventCallbacks(matched=on_matched))
        else:
            node.create_subscription(String, '/accept_matched', lambda m: None, 10,
                                     event_callbacks=SubscriptionEventCallbacks(matched=on_matched))
        spin_for(node, seconds)
        print('RESULT: role=%s matched_events=%d' % (role, counts['n']), flush=True)
    elif role in ('itype_pub', 'itype_sub'):
        node = Node('accept_' + role)

        def on_itype(info):
            counts['n'] += 1
        if role == 'itype_pub':
            node.create_publisher(String, '/accept_itype', 10,
                                  event_callbacks=PublisherEventCallbacks(incompatible_type=on_itype))
        else:
            node.create_subscription(Int32, '/accept_itype', lambda m: None, 10,
                                     event_callbacks=SubscriptionEventCallbacks(incompatible_type=on_itype))
        spin_for(node, seconds)
        print('RESULT: role=%s incompatible_type_events=%d' % (role, counts['n']), flush=True)
    else:
        print('RESULT: role=%s error=unknown_role' % role, flush=True)
        return 2
    node.destroy_node()
    rclpy.shutdown()
    return 0


if __name__ == '__main__':
    sys.exit(main())
