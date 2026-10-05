#!/usr/bin/env python3
"""Instrument a COPY of src/tickle.c for rmw_keepall_evict_repro.sh. Never run against the repository tree.

Usage: rmw_keepall_evict_repro.py <path to the copied tickle.c>

Adds, with no change to behaviour:
  - publisher side: every eviction from the KEEP_ALL publisher's cache is checked against the slowest matched
    subscriber's acknowledgement; one that takes a sample nobody acknowledged (or happens with no matched ack
    entry at all) is counted as evict_unacked and the first 20 are printed with the state that allowed it.
  - publisher side: the first 20 ACKNACK-named seq_nos answered as "gone" (the eviction Heartbeat's trigger),
    with the slot's contents and the cache bounds.
  - subscriber side: the first 20 advance_past_unavailable() skips (what gap_evicted counts).
  - at process exit: the tt_RELIABLE_STATS counters that matter here (build with -Dtt_RELIABLE_STATS).
Every anchor must match exactly once, or the script fails: a patch that silently applied nothing would make
the instrumented arm report nothing and look like a clean result.
"""
import sys

path = sys.argv[1]
if "/home/semih/tickle/src" in path:
    sys.exit("refusing to patch the repository's own tickle.c")
src = open(path, encoding="utf-8").read()


def sub(anchor, replacement):
    global src
    count = src.count(anchor)
    if count != 1:
        sys.exit(f"anchor matched {count} times, expected 1: {anchor[:80]!r}")
    src = src.replace(anchor, replacement)


sub("static uint32_t keep_all_bound(const struct tt_Publisher* pub) {",
    "static struct tt_Publisher* g_dbg_pub; /* KEEPALL_DBG */\n"
    "static uint64_t g_dbg_evict_total, g_dbg_evict_unacked, g_dbg_gone, g_dbg_adv, g_dbg_adv_skipped;\n"
    "static int g_dbg_evict_reason; /* 1 index count, 2 arena bytes, 3 sample_depth */\n"
    "static uint32_t keep_all_bound(const struct tt_Publisher* pub) {")

sub("static bool keep_all_writable(const struct tt_Publisher* pub) {\n",
    "static bool keep_all_writable(const struct tt_Publisher* pub) {\n"
    "    if (pub->keep_all) { g_dbg_pub = (struct tt_Publisher*)pub; } /* KEEPALL_DBG */\n")

sub("static void reliable_cache_evict_one(struct tt_ReliableCache* cache, uint16_t depth) {\n"
    "    if (cache->oldest_seq_no == 0) {\n        return;\n    }\n",
    "static void reliable_cache_evict_one(struct tt_ReliableCache* cache, uint16_t depth) {\n"
    "    if (cache->oldest_seq_no == 0) {\n        return;\n    }\n"
    "    if (g_dbg_pub != NULL && g_dbg_pub->reliable_cache == cache) { /* KEEPALL_DBG */\n"
    "        g_dbg_evict_total++;\n"
    "        uint32_t ma = min_peer_ack_seq_no(g_dbg_pub);\n"
    "        uint32_t at = ma > 0 ? ma - 1 : 0;\n"
    "        bool matched = any_peer_ack_matched(g_dbg_pub);\n"
    "        if (!matched || cache->oldest_seq_no > at) {\n"
    "            g_dbg_evict_unacked++;\n"
    "            static uint64_t matched_logged;\n"
    "            if (g_dbg_evict_unacked <= 20 || (matched && ++matched_logged <= 20)) {\n"
    "                fprintf(stderr, \"KEEPALL_DBG evict_unacked seq=%u pub_seq=%u min_ack=%u matched=%d bound=%u \"\n"
    "                        \"depth=%u oldest=%u newest=%u arena=%u/%u tail=%u retained=%u reason=%d blocked_bytes=%u \"\n"
    "                        \"blocked_dgrams=%u pending=%d\\n\",\n"
    "                        cache->oldest_seq_no, g_dbg_pub->seq_no, ma, (int)matched, keep_all_bound(g_dbg_pub),\n"
    "                        depth, cache->oldest_seq_no, cache->newest_seq_no, cache->arena_size, cache->arena_limit,\n"
    "                        cache->tail, (unsigned)cache->retained_samples, g_dbg_evict_reason,\n"
    "                        (unsigned)g_dbg_pub->blocked_record_bytes, (unsigned)g_dbg_pub->blocked_datagrams,\n"
    "                        (int)g_dbg_pub->writable_pending);\n"
    "            }\n"
    "        }\n"
    "    }\n")

sub("    if (cache_entry == NULL) {\n        return gone;\n    }\n",
    "    if (cache_entry == NULL) {\n"
    "        if (gone && ++g_dbg_gone <= 20) { /* KEEPALL_DBG */\n"
    "            const struct tt_ReliableCacheIndex* e = &cache->index[(missing_seq_no - 1) % depth];\n"
    "            uint32_t ma = min_peer_ack_seq_no(pub);\n"
    "            fprintf(stderr, \"KEEPALL_DBG gone missing=%u slot_seq=%u slot_len=%u oldest=%u newest=%u pub_seq=%u \"\n"
    "                    \"min_ack=%u keep_all=%d depth=%u\\n\", missing_seq_no, e->seq_no, (unsigned)e->len,\n"
    "                    cache->oldest_seq_no, cache->newest_seq_no, pub->seq_no, ma, (int)pub->keep_all, depth);\n"
    "        }\n"
    "        return gone;\n    }\n")

sub("    uint32_t skipped = first_available_seq_no - proxy->ack_seq_no;\n",
    "    uint32_t skipped = first_available_seq_no - proxy->ack_seq_no;\n"
    "    g_dbg_adv++; /* KEEPALL_DBG */\n"
    "    g_dbg_adv_skipped += skipped - received_in_first(proxy, skipped);\n"
    "    if (g_dbg_adv <= 20) {\n"
    "        fprintf(stderr, \"KEEPALL_DBG advance first_available=%u ack=%u skipped=%u not_received=%u window=%u\\n\",\n"
    "                first_available_seq_no, proxy->ack_seq_no, skipped, skipped - received_in_first(proxy, skipped),\n"
    "                (unsigned)proxy_window_bits(proxy));\n"
    "    }\n")


# Which loop evicted: tag each eviction call site.
src_count = src.count("        RSTAT_INC(evicted_by_count);\n        reliable_cache_evict_oldest(cache, depth);\n")
if src_count != 2:
    sys.exit(f"evicted_by_count sites: {src_count}, expected 2")
src = src.replace("        RSTAT_INC(evicted_by_count);\n        reliable_cache_evict_oldest(cache, depth);\n    }\n\n    struct tt_ReliableCacheIndex* entry",
                  "        RSTAT_INC(evicted_by_count);\n        g_dbg_evict_reason = 1;\n        reliable_cache_evict_oldest(cache, depth);\n    }\n\n    struct tt_ReliableCacheIndex* entry", 1)
sub("        RSTAT_INC(evicted_by_bytes);\n        reliable_cache_evict_oldest(cache, depth);\n",
    "        RSTAT_INC(evicted_by_bytes);\n        g_dbg_evict_reason = 2;\n        reliable_cache_evict_oldest(cache, depth);\n")
sub("        RSTAT_INC(evicted_by_count);\n        reliable_cache_evict_oldest(cache, depth);\n",
    "        RSTAT_INC(evicted_by_count);\n        g_dbg_evict_reason = 3;\n        reliable_cache_evict_oldest(cache, depth);\n")

# Discovery side: does the publisher ever re-match the subscriber after it was created?
sub("static void reprocess_known_announces(struct tt_Context* node) {\n",
    "static uint64_t g_dbg_reproc, g_dbg_summ, g_dbg_reg;\n"
    "static void reprocess_known_announces(struct tt_Context* node) {\n"
    "    if (++g_dbg_reproc <= 20) { /* KEEPALL_DBG */\n"
    "        int seen = 0;\n"
    "        for (int k = 0; k < tt_MAX_CONTEXT_IDS; k++) { seen += node->update_seen[k] ? 1 : 0; }\n"
    "        fprintf(stderr, \"KEEPALL_DBG reprocess node=%u seen_sources=%d t=%lu\\n\", (unsigned)node->id, seen, (unsigned long)(tt_get_ns() / 1000000U));\n"
    "    }\n")

sub("    node->update_last_seen[source] = tt_get_ns();\n    if (discovery_generation_applied(node, source, generation)) {\n",
    "    node->update_last_seen[source] = tt_get_ns();\n"
    "    if (++g_dbg_summ <= 200) { /* KEEPALL_DBG */\n"
    "        fprintf(stderr, \"KEEPALL_DBG summary node=%u from=%u gen=%u stored=%u seen=%d applied=%d\\n\",\n"
    "                (unsigned)node->id, (unsigned)source, generation, node->update_generation[source],\n"
    "                (int)node->update_seen[source], (int)discovery_generation_applied(node, source, generation));\n"
    "    }\n"
    "    if (discovery_generation_applied(node, source, generation)) {\n")

sub("    bool requested_manual = (ctx->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0;\n",
    "    bool requested_manual = (ctx->qos & tt_UPDATE_QOS_LIVELINESS_MANUAL) != 0;\n"
    "    if (++g_dbg_reg <= 40) { /* KEEPALL_DBG */\n"
    "        fprintf(stderr, \"KEEPALL_DBG register_at pub_seq=%u t=%lu\\n\", pub->seq_no, (unsigned long)(tt_get_ns() / 1000000U));\n"
    "        fprintf(stderr, \"KEEPALL_DBG register pub_ep=%u from=%u entity=%08x qos=%x req_rel=%d pub_rel=%d req_dur=%d \"\n"
    "                \"pub_dur=%d req_dl=%lu pub_dl=%lu req_manual=%d pub_manual=%d req_lease=%lu pub_lease=%lu tw=%u\\n\",\n"
    "                endpoint->id, (unsigned)ctx->header->source, ctx->entity_id, (unsigned)ctx->qos,\n"
    "                (int)requested_reliable, (int)pub->reliable, (int)requested_durable, (int)pub->durable,\n"
    "                (unsigned long)ctx->deadline_duration_ns, (unsigned long)pub->deadline_duration_ns,\n"
    "                (int)requested_manual, (int)pub->liveliness_manual,\n"
    "                (unsigned long)ctx->liveliness_lease_duration_ns, (unsigned long)pub->liveliness_lease_duration_ns,\n"
    "                (unsigned)ctx->tracking_words);\n"
    "    }\n")


sub("    uint8_t source = header->source;\n    node->update_last_seen[source] = tt_get_ns();\n#if tt_SEGMENT_ENABLED\n",
    "    uint8_t source = header->source;\n"
    "    node->update_last_seen[source] = tt_get_ns();\n"
    "    { static uint64_t n; if (++n <= 60) { /* KEEPALL_DBG */\n"
    "        fprintf(stderr, \"KEEPALL_DBG announce node=%u from=%u gen=%u frag=%u/%u len=%u seen=%d stored=%u\\n\",\n"
    "                (unsigned)node->id, (unsigned)source, generation, (unsigned)frag_index, (unsigned)frag_count,\n"
    "                tail - head, (int)node->update_seen[source], node->update_generation[source]); } }\n"
    "#if tt_SEGMENT_ENABLED\n")

sub("    uint64_t tick = tt_get_ns() / tt_CONTEXT_TX_INTERVAL;\n    if (tick != node->discovery_reply_tick) {\n",
    "    { static uint64_t n; if (++n <= 30) { /* KEEPALL_DBG */\n"
    "        fprintf(stderr, \"KEEPALL_DBG answer_request node=%u to=%u ip=%08x port=%u count=%u\\n\", (unsigned)node->id,\n"
    "                (unsigned)source, sender_ip, (unsigned)sender_port, (unsigned)node->discovery_reply_count); } }\n"
    "    uint64_t tick = tt_get_ns() / tt_CONTEXT_TX_INTERVAL;\n    if (tick != node->discovery_reply_tick) {\n")
src += r'''
/* KEEPALL_DBG */
#ifdef tt_RELIABLE_STATS
__attribute__((destructor)) static void keepall_dbg_dump(void) {
    fprintf(stderr,
            "KEEPALL_DBG exit evict_total=%lu evict_unacked=%lu gone_answers=%lu advances=%lu advance_skipped=%lu "
            "evicted_by_count=%lu evicted_by_bytes=%lu not_cached_oversize=%lu publish_refused=%lu "
            "publish_refused_bytes=%lu writable_callbacks=%lu writable_no_peers=%lu acknack_received=%lu "
            "bits_requested=%lu retransmitted=%lu null_evicted=%lu null_retry_cap=%lu eviction_heartbeats=%lu "
            "ack_solicit_sent=%lu jump_data=%lu jump_heartbeat=%lu retry_giveups=%lu heartbeat_advances=%lu "
            "heartbeat_abandoned_seq=%lu late_below_ack=%lu\n",
            (unsigned long)g_dbg_evict_total, (unsigned long)g_dbg_evict_unacked, (unsigned long)g_dbg_gone,
            (unsigned long)g_dbg_adv, (unsigned long)g_dbg_adv_skipped, (unsigned long)g_rstats.evicted_by_count,
            (unsigned long)g_rstats.evicted_by_bytes, (unsigned long)g_rstats.not_cached_oversize,
            (unsigned long)g_rstats.publish_refused, (unsigned long)g_rstats.publish_refused_bytes,
            (unsigned long)g_rstats.writable_callbacks, (unsigned long)g_rstats.writable_no_peers,
            (unsigned long)g_rstats.acknack_received, (unsigned long)g_rstats.bits_requested,
            (unsigned long)g_rstats.retransmitted, (unsigned long)g_rstats.null_evicted,
            (unsigned long)g_rstats.null_retry_cap, (unsigned long)g_rstats.eviction_heartbeats,
            (unsigned long)g_rstats.ack_solicit_sent, (unsigned long)g_rstats.jump_data,
            (unsigned long)g_rstats.jump_heartbeat, (unsigned long)g_rstats.retry_giveups,
            (unsigned long)g_rstats.heartbeat_advances, (unsigned long)g_rstats.heartbeat_abandoned_seq,
            (unsigned long)g_rstats.late_below_ack);
}
#endif
'''
# Follow-up (after b6de8a4d): every non-self datagram's source and socket, and every list request sent.
sub("    if (self_sent) {\n        node->rx_self_sent++;\n    }\n",
    "    if (self_sent) {\n        node->rx_self_sent++;\n    }\n"
    "    { static uint64_t n; if (!self_sent && ++n <= 300) { /* KEEPALL_DBG */\n"
    "        fprintf(stderr, \"KEEPALL_DBG rx node=%u from=%u via_data=%d len=%u port=%u t=%lu\\n\", (unsigned)node->id,\n"
    "                (unsigned)header->source, (int)node->rx_via_data_port, tail - head, (unsigned)sender_port,\n"
    "                (unsigned long)(tt_get_ns() / 1000000U)); } }\n")
sub("    flush_pending_broadcast(node);\n    uint32_t old_tx_tail = node->tx_tail;\n    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_ACKNACK, source);\n",
    "    { static uint64_t n; if (++n <= 50) { /* KEEPALL_DBG */\n"
    "        fprintf(stderr, \"KEEPALL_DBG send_request node=%u to=%u gen=%u port=%u t=%lu\\n\", (unsigned)node->id,\n"
    "                (unsigned)source, generation, (unsigned)sender_port, (unsigned long)(tt_get_ns() / 1000000U)); } }\n"
    "    flush_pending_broadcast(node);\n    uint32_t old_tx_tail = node->tx_tail;\n"
    "    struct tt_SubmessageHeader* submessage_header = start_encode(node, tt_SUBMESSAGE_TYPE_ACKNACK, source);\n")

# KEEPALL_DBG_NOREPLY=1 (set on the subscriber): reply_with_own_announce() sends nothing, so the publisher can only
# learn the reader through its summaries (rule 3 request) - the path the rig's unregistered runs were left with.
sub("    struct tt_Peer reply_to = {sender_node_id, sender_ip, sender_port};\n    build_and_send_update(node, &reply_to, 1);\n",
    "    { extern char* getenv(const char*); const char* e = getenv(\"KEEPALL_DBG_NOREPLY\"); /* KEEPALL_DBG */\n"
    "      if (e != NULL && *e == '1') { fprintf(stderr, \"KEEPALL_DBG noreply to=%u\\n\", (unsigned)sender_node_id); return; } }\n"
    "    struct tt_Peer reply_to = {sender_node_id, sender_ip, sender_port};\n    build_and_send_update(node, &reply_to, 1);\n")

# Fix-option (a) prototype, experiment only: KEEPALL_DBG_ACK_CLAIM=1 (publisher) - an ACKNACK from a reliable reader
# this writer has no ack entry for claims one (window unknown, so the default bound) and makes the sender a unicast
# peer, instead of record_peer_ack() ignoring it.
sub("    record_peer_ack(pub, header->source, sender_entity_id, seq_no);\n",
    "    { extern char* getenv(const char*); static int on = -1; /* KEEPALL_DBG */\n"
    "      if (on < 0) { const char* e = getenv(\"KEEPALL_DBG_ACK_CLAIM\"); on = e != NULL && *e == '1'; }\n"
    "      if (on && pub->reliable && sender_entity_id != 0 && find_peer_ack(pub, header->source, sender_entity_id) == NULL &&\n"
    "          claim_peer_ack(pub, header->source, sender_entity_id) != NULL) {\n"
    "        (void)upsert_peer(pub->peers, header->source, sender_ip, sender_port);\n"
    "        fprintf(stderr, \"KEEPALL_DBG ack_claim pub_seq=%u from=%u entity=%08x ack=%u t=%lu\\n\", pub->seq_no,\n"
    "                (unsigned)header->source, sender_entity_id, seq_no, (unsigned long)(tt_get_ns() / 1000000U));\n"
    "      } }\n"
    "    record_peer_ack(pub, header->source, sender_entity_id, seq_no);\n")

# drain_rx(): how long a drain session runs (it ends only when every non-idle socket reads empty).
sub("    uint32_t since_clock = 1; // the first datagram was stamped with the reading its caller took\n    while (true) {\n",
    "    uint32_t since_clock = 1; // the first datagram was stamped with the reading its caller took\n"
    "    uint64_t dbg_iter = 0; /* KEEPALL_DBG */\n"
    "    g_dbg_drain_sessions++;\n"
    "    while (true) {\n"
    "        if (++dbg_iter > g_dbg_drain_max) { g_dbg_drain_max = dbg_iter; }\n")
sub("static tt_ret_t drain_rx(struct tt_Context* node, tt_ret_t first_result) {\n",
    "uint64_t g_dbg_drain_sessions, g_dbg_drain_max; /* KEEPALL_DBG */\n"
    "static tt_ret_t drain_rx(struct tt_Context* node, tt_ret_t first_result) {\n")
# node_poll(): how often the receive side runs at all, and what the state lock cost the poller.
sub("static tt_ret_t node_poll(struct tt_Context* node, int64_t timeout) {\n",
    "#ifndef KEEPALL_DBG_LOCK\n#define KEEPALL_DBG_LOCK 0\n#endif\n"
    "static struct tt_Context* g_dbg_node; /* KEEPALL_DBG */\n"
    "static uint64_t g_dbg_node_polls;\n"
    "__attribute__((destructor)) static void keepall_dbg_lock_dump(void) {\n"
    "    if (g_dbg_node == NULL || !KEEPALL_DBG_LOCK) { return; } /* reads a freed node otherwise */\n"
    "    const struct tt_LockStats* l = &g_dbg_node->state_lock_stats;\n"
    "    fprintf(stderr, \"KEEPALL_DBG lock node_polls=%lu acquisitions=%lu contended=%lu wait_ms=%lu poller_contended=%lu \"\n"
    "            \"poller_wait_ms=%lu\\n\", (unsigned long)g_dbg_node_polls, (unsigned long)l->acquisitions,\n"
    "            (unsigned long)l->contended, (unsigned long)(l->wait_ns / 1000000U), (unsigned long)l->poller_contended,\n"
    "            (unsigned long)(l->poller_wait_ns / 1000000U));\n"
    "}\n"
    "static tt_ret_t node_poll(struct tt_Context* node, int64_t timeout) {\n"
    "    g_dbg_node = node;\n"
    "    g_dbg_node_polls++;\n")
open(path, "w", encoding="utf-8").write(src)
print(f"patched {path}: {src.count('KEEPALL_DBG')} KEEPALL_DBG markers")

# Optional second file: the copied src/hal_linux.c - which socket each receive asks, and what it skips.
if len(sys.argv) > 2:
    hpath = sys.argv[2]
    if "/home/semih/tickle/src" in hpath:
        sys.exit("refusing to patch the repository's own hal_linux.c")
    src = open(hpath, encoding="utf-8").read()
    sub("int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {\n",
        "uint64_t g_dbg_skip_data, g_dbg_skip_wk, g_dbg_read_data, g_dbg_got_data, g_dbg_read_wk, g_dbg_got_wk,\n"
        "    g_dbg_drained; /* KEEPALL_DBG */\n"
        "extern uint64_t g_dbg_drain_sessions, g_dbg_drain_max;\n"
        "__attribute__((destructor)) static void keepall_dbg_hal_dump(void) {\n"
        "    fprintf(stderr, \"KEEPALL_DBG hal skip_data=%lu skip_wk=%lu read_data=%lu got_data=%lu read_wk=%lu got_wk=%lu \"\n"
        "            \"drained=%lu drain_sessions=%lu drain_max_iter=%lu\\n\", (unsigned long)g_dbg_skip_data,\n"
        "            (unsigned long)g_dbg_skip_wk, (unsigned long)g_dbg_read_data, (unsigned long)g_dbg_got_data,\n"
        "            (unsigned long)g_dbg_read_wk, (unsigned long)g_dbg_got_wk, (unsigned long)g_dbg_drained,\n"
        "            (unsigned long)g_dbg_drain_sessions, (unsigned long)g_dbg_drain_max);\n"
        "}\n"
        "int32_t tt_try_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port) {\n")
    sub("        if ((node->hal.rx_idle & bit) != 0) {\n            continue;\n        }\n        ret = rx_fill(node, order[i], buf, len, ip, port);\n",
        "        if ((node->hal.rx_idle & bit) != 0) {\n"
        "            if (bit == TT_RX_IDLE_DATA) { g_dbg_skip_data++; } else { g_dbg_skip_wk++; } /* KEEPALL_DBG */\n"
        "            continue;\n        }\n"
        "        ret = rx_fill(node, order[i], buf, len, ip, port);\n"
        "        if (bit == TT_RX_IDLE_DATA) { g_dbg_read_data++; g_dbg_got_data += ret >= 0; } else { g_dbg_read_wk++; g_dbg_got_wk += ret >= 0; }\n")
    sub("        node->hal.rx_idle = 0; // drained: the next drain session asks every socket again\n",
        "        g_dbg_drained++; /* KEEPALL_DBG */\n"
        "        node->hal.rx_idle = 0; // drained: the next drain session asks every socket again\n")
    sub("int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {\n",
        "static int32_t tt_receive_dbg_inner(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port,\n"
        "                                    int64_t timeout); /* KEEPALL_DBG */\n"
        "uint64_t g_dbg_wait_calls, g_dbg_wait_got, g_dbg_wait_timeout, g_dbg_wait_interrupted;\n"
        "int32_t tt_receive(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port, int64_t timeout) {\n"
        "    g_dbg_wait_calls++;\n"
        "    int32_t r = tt_receive_dbg_inner(node, buf, len, ip, port, timeout);\n"
        "    if (r >= 0) { g_dbg_wait_got++; } else if (r == -1) { g_dbg_wait_timeout++; } else if (r == -3) { g_dbg_wait_interrupted++; }\n"
        "    return r;\n"
        "}\n"
        "static int32_t tt_receive_dbg_inner(struct tt_Context* node, void* buf, size_t len, uint32_t* ip, uint16_t* port,\n"
        "                                    int64_t timeout) {\n")
    sub("            \"drained=%lu drain_sessions=%lu drain_max_iter=%lu\\n\", (unsigned long)g_dbg_skip_data,\n",
        "            \"drained=%lu drain_sessions=%lu drain_max_iter=%lu wait_calls=%lu wait_got=%lu wait_timeout=%lu \"\n"
        "            \"wait_interrupted=%lu\\n\", (unsigned long)g_dbg_skip_data,\n")
    sub("            (unsigned long)g_dbg_drain_sessions, (unsigned long)g_dbg_drain_max);\n",
        "            (unsigned long)g_dbg_drain_sessions, (unsigned long)g_dbg_drain_max, (unsigned long)g_dbg_wait_calls,\n"
        "            (unsigned long)g_dbg_wait_got, (unsigned long)g_dbg_wait_timeout, (unsigned long)g_dbg_wait_interrupted);\n")
    # The rig's Pis grant only 425,984 bytes (net.core.rmem_max default); this PC grants 4 MiB. KEEPALL_DBG_RCVBUF
    # asks for a smaller buffer at run time so the reproduction can match the rig.
    sub("    int buffer_size = tt_SOCKET_BUFFER_SIZE;\n",
        "    int buffer_size = tt_SOCKET_BUFFER_SIZE;\n"
        "    { const char* e = getenv(\"KEEPALL_DBG_RCVBUF\"); if (e != NULL && *e != 0) { buffer_size = atoi(e); } } /* KEEPALL_DBG */\n")
    # Fix-option (c) prototype, experiment only: KEEPALL_DBG_DROP_SRC=<own IP> attaches a classic BPF filter to the
    # well-known socket that drops every datagram from that source address, so the context's own looped-back
    # broadcasts never occupy it. A blanket own-address drop - production would have to limit it to DATA/DATA_FRAG
    # (same-context CALLREQUEST/CALLRESPONSE ride the loopback), which this deliberately does not attempt.
    sub("#include <sys/socket.h>\n",
        "#include <sys/socket.h>\n#include <linux/filter.h> /* KEEPALL_DBG */\n")
    sub("    node->hal.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);\n    if (node->hal.sock < 0) {\n"
        "        TT_LOG_ERROR(\"Cannot create UDP socket: %s\", strerror(errno));\n        return tt_RET_IO_ERROR;\n    }\n",
        "    node->hal.sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);\n    if (node->hal.sock < 0) {\n"
        "        TT_LOG_ERROR(\"Cannot create UDP socket: %s\", strerror(errno));\n        return tt_RET_IO_ERROR;\n    }\n"
        "    { const char* e = getenv(\"KEEPALL_DBG_DROP_SRC\"); /* KEEPALL_DBG */\n"
        "      if (e != NULL && *e != 0) {\n"
        "        uint32_t own = ntohl(inet_addr(e));\n"
        "        struct sock_filter code[] = {BPF_STMT(BPF_LD | BPF_W | BPF_ABS, (uint32_t)(SKF_NET_OFF + 12)),\n"
        "                                     BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, own, 0, 1),\n"
        "                                     BPF_STMT(BPF_RET | BPF_K, 0), BPF_STMT(BPF_RET | BPF_K, 0xffffffffU)};\n"
        "        struct sock_fprog prog = {4, code};\n"
        "        int r = setsockopt(node->hal.sock, SOL_SOCKET, SO_ATTACH_FILTER, &prog, sizeof(prog));\n"
        "        fprintf(stderr, \"KEEPALL_DBG drop_src filter on well-known socket for %s: %d\\n\", e, r);\n"
        "      } }\n")
    open(hpath, "w", encoding="utf-8").write(src)
    print(f"patched {hpath}: {src.count('KEEPALL_DBG')} KEEPALL_DBG markers")
