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

        # Both events, because the two implementations signal the same mismatch differently and the
        # difference is the finding. rmw_tickle raises INCOMPATIBLE_TYPE, which is the event the
        # mismatch is; rmw_cyclonedds_cpp raises INCOMPATIBLE_QOS with policy INVALID instead (its own
        # log says so: "requesting incompatible QoS ... Last incompatible policy: INVALID"). Counting
        # only the first made the control report 0 and the row read as VOID on every run, which hides
        # a real VOID among the noise - the control was detecting the mismatch the whole time.
        counts['qos'] = 0

        def on_itype(info):
            counts['n'] += 1

        def on_iqos(info):
            counts['qos'] += 1
        if role == 'itype_pub':
            node.create_publisher(String, '/accept_itype', 10,
                                  event_callbacks=PublisherEventCallbacks(incompatible_type=on_itype,
                                                                          incompatible_qos=on_iqos))
        else:
            node.create_subscription(Int32, '/accept_itype', lambda m: None, 10,
                                     event_callbacks=SubscriptionEventCallbacks(incompatible_type=on_itype,
                                                                                incompatible_qos=on_iqos))
        spin_for(node, seconds)
        print('RESULT: role=%s incompatible_type_events=%d incompatible_qos_events=%d'
              % (role, counts['n'], counts['qos']), flush=True)
    elif role == 'intropeer':
        # The endpoint the introspecting node will read. It must be in another PROCESS (here, another namespace), or
        # the reader goes through get_topic_endpoint_info_by_topic()'s local branch, where the QoS is the real profile
        # the publisher was created with - and that branch never had g14's defect. Dev demonstrated that the hard way
        # on 2026-09-29: with the g14 fix reverted and rebuilt, the self-reading version of this case still passed.
        node = Node('accept_intropeer')
        from rclpy.qos import DurabilityPolicy
        from rclpy.qos import LivelinessPolicy
        from rclpy.qos import QoSProfile
        from rclpy.qos import ReliabilityPolicy
        qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE,
                         durability=DurabilityPolicy.VOLATILE, liveliness=LivelinessPolicy.AUTOMATIC)
        pub = node.create_publisher(String, '/accept_intro', qos)

        def tick():
            counts['n'] += 1
            pub.publish(String(data='msg-%d' % counts['n']))
        node.create_timer(0.2, tick)
        spin_for(node, seconds)
        print('RESULT: role=intropeer sent=%d' % counts['n'], flush=True)
    elif role == 'introspect':
        # g14's generalisation (RMW_GAPS_PLAN): any value an introspection API reports must be one we would accept
        # back. The reported values are handed straight to the APIs that would consume them rather than inspected.
        #
        # This node creates NO publisher on the topic, so every endpoint it is told about is a REMOTE one, by
        # construction rather than by filtering. That is both the branch g14 lived in - the one that starts from
        # rmw_qos_profile_unknown - and the realistic shape, since a tool like rosbag2 is always reading somebody
        # else's endpoint.
        #
        # discovered=0 is reported as its own field and is NOT a pass: a case that reports success when it found
        # nothing to check is the failure this file has met twice in two days.
        #
        # Every checked value is PRINTED as well as judged, because a failure that says only "mismatch" does not say
        # what was compared.
        node = Node('accept_introspect')
        from rosidl_runtime_py.utilities import get_message
        deadline = time.time() + max(seconds - 1.0, 1.0)
        infos = []
        while time.time() < deadline:
            rclpy.spin_once(node, timeout_sec=0.2)
            infos = node.get_publishers_info_by_topic('/accept_intro')
            if infos:
                break
        fails = []
        gids = []
        for info in infos:
            tname = info.topic_type
            gids.append(bytes(info.endpoint_gid))
            print('REPORTED: node=%s type=%s qos=%s gid=%s'
                  % (info.node_name, tname, info.qos_profile, bytes(info.endpoint_gid).hex()), flush=True)
            try:
                msg_type = get_message(tname)
            except Exception as exc:  # noqa: BLE001 - the failure itself is the result
                fails.append('type_unresolvable:%s:%s' % (tname, type(exc).__name__))
                continue
            # The round trip that g14 failed: the profile we reported for a discovered endpoint, handed back to us.
            try:
                again = node.create_publisher(msg_type, '/accept_intro_rt', info.qos_profile)
                node.destroy_publisher(again)
            except Exception as exc:  # noqa: BLE001
                fails.append('recreate_refused:%s:%s' % (type(exc).__name__, str(exc)[:60].replace(' ', '_')))
        for gid in gids:
            if gid == bytes(len(gid)):
                fails.append('gid_all_zero')
        if len(set(gids)) != len(gids):
            fails.append('gid_not_unique')
        print('RESULT: role=introspect discovered=%d roundtrip_failures=%d detail=%s'
              % (len(infos), len(fails), ','.join(fails) if fails else 'none'), flush=True)
    elif role in ('namepeer', 'names'):
        # The name and GID rows of g14's generalisation (RMW_GAPS_PLAN, "the other introspection surfaces"), which
        # that table orders first because a bridge or a `ros2 topic pub` round-trips both in ordinary use.
        #
        # The edge this is built around, measured rather than assumed: tt_MAX_NAME_LENGTH is 255, and rmw's own
        # limit for a full topic name is 255 MINUS 8 reserved for the prefixes it adds (rt/, rq/, rr/), so 247 is
        # the longest name rclpy will accept. A name our core would carry happily therefore sits eight characters
        # past what rmw takes, and anything we REPORT at that length would be a value we would refuse back.
        # The test runs at the maximum that can be created, since rcl refuses to create anything longer: what it
        # can decide is whether a name at the edge survives the round trip intact.
        LONG = '/' + 'n' * 246      # 247 incl. the slash: the longest rclpy accepts (verified 2026-10-02)
        if role == 'namepeer':
            node = Node('accept_namepeer')
            pub = node.create_publisher(String, LONG, 10)

            def tick():
                counts['n'] += 1
                pub.publish(String(data='msg-%d' % counts['n']))
            node.create_timer(0.2, tick)
            spin_for(node, seconds)
            print('RESULT: role=namepeer sent=%d' % counts['n'], flush=True)
        else:
            from rclpy.validate_full_topic_name import validate_full_topic_name
            from rclpy.validate_namespace import validate_namespace
            from rclpy.validate_node_name import validate_node_name
            node = Node('accept_names')
            got = {'info': None}

            # Two-argument callback, so the sample's MessageInfo arrives WITH it. The first version of this role
            # registered a one-argument callback and then called sub.handle.take_message() for the info; that always
            # returned None, because the executor had already consumed the message to dispatch the callback - a
            # check that could never pass, found by probing the API rather than by reading it. Subscribing is not
            # decoration here: the GID row's second half asks whether a DELIVERED sample's publisher_gid is the
            # value the graph reports for that writer, which the graph alone cannot answer.
            def on_msg(_msg, info):
                if got['info'] is None:
                    got['info'] = info
            node.create_subscription(String, LONG, on_msg, 10)
            deadline = time.time() + max(seconds - 1.0, 1.0)
            infos = []
            while time.time() < deadline:
                rclpy.spin_once(node, timeout_sec=0.2)
                infos = node.get_publishers_info_by_topic(LONG)
                if infos and got['info'] is not None:
                    break
            fails = []
            # The topic name has to come from the API that REPORTS one. TopicEndpointInfo does not carry it - you
            # query by it - so validating LONG there would be checking a constant this file defines against itself.
            # get_topic_names_and_types() is the surface the task list names, and what it hands back is a value a
            # bridge or `ros2 topic pub` would use.
            reported_topics = [t for t, _types in node.get_topic_names_and_types() if len(t) > 200]
            if not reported_topics:
                fails.append('long_topic_not_reported_in_graph')
            for t in reported_topics:
                print('REPORTED: topic_len=%d topic_tail=%s' % (len(t), t[-12:]), flush=True)
                if t != LONG:
                    fails.append('topic_name_altered:len_%d_vs_%d' % (len(t), len(LONG)))
                try:
                    validate_full_topic_name(t)
                except Exception as exc:  # noqa: BLE001
                    fails.append('topic_refused:%s:%s' % (type(exc).__name__, str(exc)[:40].replace(' ', '_')))
            for info in infos:
                print('REPORTED: node=%s ns=%s gid=%s'
                      % (info.node_name, info.node_namespace,
                         bytes(info.endpoint_gid).hex()), flush=True)
                # Each reported name goes back to the validator that governs its kind. A name we report and would
                # then refuse is the defect; which of the two refused it is the useful half of the message.
                for label, value, fn in (('node', info.node_name, validate_node_name),
                                         ('ns', info.node_namespace, validate_namespace)):
                    try:
                        fn(value)
                    except Exception as exc:  # noqa: BLE001 - the refusal is the result
                        fails.append('%s_refused:%s:%s' % (label, type(exc).__name__, str(exc)[:40].replace(' ', '_')))
                # And to the API a tool would actually call with it.
                try:
                    again = node.create_publisher(String, LONG, 10)
                    node.destroy_publisher(again)
                except Exception as exc:  # noqa: BLE001
                    fails.append('recreate_refused:%s:%s' % (type(exc).__name__, str(exc)[:40].replace(' ', '_')))
            # The delivered sample's publisher_gid. "Could not look" gets its own answer rather than passing
            # quietly: a check that reports success when it never ran is the failure this file has met twice.
            #
            # What this does NOT check, and why: the first version asked whether the sample's gid EQUALS the one
            # the graph reports for that writer. The control disproved the premise - CycloneDDS reports
            # 011074bc88519fb4... in the graph and ee9da3dbfffbb4b6... on the sample, two encodings of the same
            # writer - so that equality is not something rmw guarantees and a test asserting it fails everywhere.
            # What a tool matching samples to writers actually needs is weaker and is checkable: the sample must
            # carry a gid at all, and it must not be all zeros, or every writer looks like every other.
            if got['info'] is None:
                fails.append('no_sample_delivered')
            else:
                pg = got['info'].get('publisher_gid')
                raw = pg.get('data') if isinstance(pg, dict) else pg
                if raw is None:
                    fails.append('sample_carries_no_publisher_gid')
                else:
                    b = bytes(raw)
                    print('REPORTED: sample_publisher_gid=%s' % b.hex(), flush=True)
                    if not b or b == bytes(len(b)):
                        fails.append('sample_gid_all_zero:len_%d' % len(b))
            # Two writers on ONE topic in ONE node must not share a graph gid. This is checkable where
            # graph-versus-sample equality is not: the control reports two encodings of the same writer
            # (above), so equality is not an rmw guarantee - but a gid that cannot tell two endpoints
            # apart fails at the field's only purpose, in every implementation.
            #
            # It is the collision Milestone 47 removed from rmw_get_gid_for_publisher() by moving it off
            # the shared name hash. rmw_graph.c's encode_gid() still encodes endpoint_id, which IS that
            # hash - hash(topic name + endpoint name) - so two publishers of one topic in one node get
            # the same bytes by construction.
            PAIR = '/gid_collision_probe'
            p_one = node.create_publisher(String, PAIR, 10)
            p_two = node.create_publisher(String, PAIR, 10)
            pair_deadline = time.time() + 5.0
            pair_infos = []
            while time.time() < pair_deadline:
                rclpy.spin_once(node, timeout_sec=0.2)
                pair_infos = node.get_publishers_info_by_topic(PAIR)
                if len(pair_infos) >= 2:
                    break
            pair_gids = []
            for info in pair_infos:
                raw = info.endpoint_gid if hasattr(info, 'endpoint_gid') else None
                if raw is not None:
                    pair_gids.append(bytes(raw).hex())
            print('REPORTED: pair_graph_gids=%s' % ','.join(pair_gids), flush=True)
            if len(pair_infos) < 2:
                # Its own answer rather than a quiet pass: two publishers that never both appeared
                # cannot say anything about whether their gids differ.
                fails.append('pair_not_discovered:saw_%d' % len(pair_infos))
            elif len(pair_gids) < 2:
                fails.append('pair_gids_unreadable:got_%d' % len(pair_gids))
            elif len(set(pair_gids)) < len(pair_gids):
                fails.append('two_writers_share_a_graph_gid')
            node.destroy_publisher(p_one)
            node.destroy_publisher(p_two)
            print('RESULT: role=names discovered=%d roundtrip_failures=%d detail=%s'
                  % (len(infos), len(fails), ','.join(fails) if fails else 'none'), flush=True)
    else:
        print('RESULT: role=%s error=unknown_role' % role, flush=True)
        return 2
    node.destroy_node()
    rclpy.shutdown()
    return 0


if __name__ == '__main__':
    sys.exit(main())
