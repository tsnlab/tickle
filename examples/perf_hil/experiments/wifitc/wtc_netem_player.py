#!/usr/bin/env python3
"""Plays a schedule (wtc_schedules5g.py JSON) on one tap: at each step `tc qdisc change ... netem` in the given namespace, logging "<mono_ns> <rate_kbit> <delay_ms> <loss>" after each
applied step (time.monotonic_ns = CLOCK_MONOTONIC of this Pi). Time zero = the start of this process. Usage: wtc_netem_player.py --ns NS --tap TAP --schedule FILE --log FILE"""
import argparse,json,subprocess,time
p=argparse.ArgumentParser(); p.add_argument("--ns"); p.add_argument("--tap"); p.add_argument("--schedule"); p.add_argument("--log"); a=p.parse_args()
s=json.load(open(a.schedule)); limit=s["base"]["limit_pkts"]; t0=time.monotonic_ns(); f=open(a.log,"w")
for st in s["steps"]:
    wait=(t0+st["t_ms"]*1_000_000-time.monotonic_ns())/1e9
    if wait>0: time.sleep(wait)
    args=["sudo","-n","ip","netns","exec",a.ns,"tc","qdisc","change","dev",a.tap,"root","netem","limit",str(limit),"delay",f"{st['delay_ms']}ms","rate",f"{st['rate_kbit']}kbit","loss"]+st["loss"].replace("loss ","").split()
    subprocess.run(args,check=False,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
    f.write(f"{time.monotonic_ns()} {st['rate_kbit']} {st['delay_ms']} {st['loss'].replace(' ','_')}\n"); f.flush()
f.close()
