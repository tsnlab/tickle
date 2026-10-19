# The L4 round trip's reply (large_rtt_rx -> large_rtt_tx) and, in its control cells, the ping as well: the sequence
# id alone, 8 bytes. The user's decision (2026-10-19): for a large message the reply is a small message.
uint64 id
