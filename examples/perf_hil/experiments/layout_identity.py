#!/usr/bin/env python3
"""layout_identity.py - does a layout-only change leave every function's instructions as they were? Static, no run.

Compares two linked binaries (or objects) function by function: objdump -d of each, every function's instruction
sequence with addresses removed. Built for the layout A/A arms of srv_gap_pc.sh (layout_perturb.sh): the same commit,
the same flags, bytes added that nothing executes. Classes per function present in both:
  same      identical mnemonics and operands (branch targets as <function+offset>, rip-relative data as <symbol>)
  operands  identical mnemonics, some operand differs: a data displacement or an aarch64 page offset (a data move),
            or a branch to another offset (code inside the function moved), or an adrp the linker turned into an adr
            (Cortex-A53 erratum 843419 fix) - executes the same NUMBER of instructions
  nops      differs only by nops inside the body (padding executed when control passes through it)
  veneer    a load or store the erratum fix replaced by a branch to a stub (+2 executed instructions a pass)
  codegen   anything else: the compiler generated different code (inlining, register allocation, block order)
A function in one binary only is listed (the perturbation's own symbols are expected there). Dropped before comparing:
trailing nops and fill (padding after a function's last instruction, never executed), x86 gas's jump over a long
alignment gap, and an aarch64 tail of stub shape after the function's last barrier (the erratum fix's stubs - entered
only through a "veneer" difference elsewhere; a real cold tail of that shape is dropped from both sides alike).

HOW TO READ IT (written before the first comparison): for a gate build (TICKLE_LAYOUT_CFLAGS) and a perturbation that
adds unexecuted text or data, "nops", "veneer" and "codegen" must all be 0 - else the executed count can move with layout
and the build is not layout-stable. "operands" is expected for a data perturbation and allowed: it changes no count.
Usage: layout_identity.py [--objdump OBJDUMP] [--list N] [--show-live N] A B
Exit 0 when nops = veneer = codegen = 0, 1 otherwise, 2 when it could not look.
"""
import argparse
import re
import subprocess
import sys

NOP = re.compile(r"^((data16|cs|ds|rex\.\w+)\s+)*(nop\w*|xchg\s+%ax,%ax)\b")
FUNC = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")
INSN = re.compile(r"^\s*[0-9a-f]+:\t(.*)$")


def norm(ins):
    """One instruction with addresses replaced by what they name."""
    comment = None
    # objdump's comment: "// ..." on aarch64, "# <addr> <sym>" on x86 (an aarch64 immediate "#0x10" has no space)
    m = re.search(r"\s+(//|#)\s+(.*)$", ins)
    if m:
        comment = m.group(2)
        ins = ins[: m.start()]
    sym = re.search(r"<([^>]+)>", comment) if comment else None
    # x86 rip-relative data: the displacement changes when data moves; what it names is the symbol in the comment
    if sym and "(%rip)" in ins:
        ins = re.sub(r"-?0x[0-9a-f]+\(%rip\)", "<" + sym.group(1) + ">(%rip)", ins)
    # a branch or call target: "1234 <f+0x10>" -> "<f+0x10>"
    ins = re.sub(r"\b[0-9a-f]+ (<[^>]+>)", r"\1", ins)
    return re.sub(r"\s+", " ", ins).strip()


def functions(objdump, path):
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path], capture_output=True, text=True, check=True).stdout
    fns, name, body = {}, None, []
    for line in out.splitlines():
        m = FUNC.match(line)
        if m:
            if name is not None:
                fns[name] = body
            name, body = m.group(1), []
            continue
        m = INSN.match(line)
        if m and name is not None:
            body.append(norm(m.group(1)))
    if name is not None:
        fns[name] = body
    stubs = 0
    for n, b in fns.items():
        while b:
            if NOP.match(b[-1]) or FILL.match(b[-1]):
                b.pop()
            # x86 gas fills a long alignment gap with "jmp <the next function>" ahead of its nops: unreachable (it
            # follows a barrier) padding, not code
            elif len(b) > 1 and re.match(r"^jmp <[^+>]+>$", b[-1]) and BARRIER.match(b[-2]):
                b.pop()
            else:
                break
        # aarch64 GNU ld's Cortex-A53 erratum 843419 fix appends stubs after a function ("b <over>; ldr ..; b <back>"):
        # unreachable from the function itself (they follow its last barrier), entered only from a load the linker
        # turned into a branch - which shows as a "veneer" difference in that other function. Stripped from both
        # sides alike, so a cold block of the same shape ending a function compares equal either way.
        r = len(b)
        while r > 0 and (STUB.match(b[r - 1]) or NOP.match(b[r - 1]) or FILL.match(b[r - 1])):
            r -= 1
        tail = b[r:]
        back = [x for x in tail if re.match(r"^b <[^>+]+\+0x[0-9a-f]+>$", x)]
        moved = [x for x in tail if STUB.match(x) and not x.startswith("b ")]
        if 0 < r < len(b) and BARRIER.match(b[r - 1]) and back and moved:
            stubs += 1
            del b[r:]
        fns[n] = b
    fns[" stubs"] = stubs
    return fns


FILL = re.compile(r"^(\.word|\.inst|udf|\(bad\))\b")
STUB = re.compile(r"^(b|ldr\w*|str\w*|ld\w+|st\w+) ")


def count_neutral(x, y):
    """Two instructions that differ only as the linker rewrites a layout-dependent one into another single one."""
    mx, my = mnem(x), mnem(y)
    return mx == my or {mx, my} == {"adrp", "adr"}


def is_veneer(x, y):
    """A load or store the erratum fix replaced by a branch to a stub: one more instruction executed each pass."""
    mx, my = mnem(x), mnem(y)
    return (mx == "b") != (my == "b") and STUB.match(y if mx == "b" else x) is not None


def mnem(ins):
    return ins.split(" ", 1)[0]


BARRIER = re.compile(r"^(jmp|ret|ud2|int3|b|br|ret[a-z]*)( |$)|^(call|bl) .*<(__stack_chk_fail|abort|exit|_exit)[@>]")


def reachable_nops(body):
    """Nops control can fall into: a run of nops not preceded by an unconditional jump, a return or a no-return call.
    Padding after a barrier is only jumped over; padding after anything else is executed on the way through."""
    count, prev = 0, ""
    live = False
    for ins in body:
        if NOP.match(ins):
            if live or (prev and not BARRIER.match(prev)):
                count += 1
                live = True
            continue
        live = False
        prev = ins
    return count


def classify(a, b):
    if a == b:
        return "same"
    an = [i for i in a if not NOP.match(i)]
    bn = [i for i in b if not NOP.match(i)]
    if an == bn:
        return "nops"
    if len(an) == len(bn):
        pairs = list(zip(an, bn))
        if any(is_veneer(x, y) for x, y in pairs):
            return "veneer"
        if all(count_neutral(x, y) for x, y in pairs):
            return "operands" if len(a) == len(b) else "nops"
    return "codegen"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--objdump", default="objdump")
    ap.add_argument("--list", type=int, default=8)
    ap.add_argument("--show-live", type=int, default=0, help="print up to N functions' first fall-through nop in A")
    ap.add_argument("a")
    ap.add_argument("b")
    o = ap.parse_args()
    try:
        fa, fb = functions(o.objdump, o.a), functions(o.objdump, o.b)
    except (subprocess.CalledProcessError, OSError) as e:
        print(f"layout_identity: cannot disassemble: {e}")
        return 2
    if not fa or not fb:
        print("layout_identity: no functions read (could not look)")
        return 2
    stubs_a, stubs_b = fa.pop(" stubs"), fb.pop(" stubs")
    for f in (fa, fb):  # the linker's own stub symbols are where the code went, not functions
        for n in [n for n in f if n.startswith("e843419@") or n.endswith("_veneer")]:
            del f[n]
    cls = {"same": [], "operands": [], "nops": [], "veneer": [], "codegen": []}
    for n in sorted(set(fa) & set(fb)):
        cls[classify(fa[n], fb[n])].append(n)
    only_a = sorted(set(fa) - set(fb))
    only_b = sorted(set(fb) - set(fa))
    own = [n for n in fa if not n.startswith(".plt") and "@plt" not in n]
    nops_a = sum(sum(1 for i in fa[n] if NOP.match(i)) for n in own)
    nops_b = sum(sum(1 for i in b if NOP.match(i)) for n, b in fb.items() if not n.startswith(".plt") and "@plt" not in n)
    live_a = sum(reachable_nops(fa[n]) for n in own)
    live_b = sum(reachable_nops(b) for n, b in fb.items() if not n.startswith(".plt") and "@plt" not in n)
    print(f"functions {len(set(fa) & set(fb))}: same {len(cls['same'])} operands {len(cls['operands'])} "
          f"nops {len(cls['nops'])} veneer {len(cls['veneer'])} codegen {len(cls['codegen'])}; only in A {len(only_a)} only in B {len(only_b)}; "
          f"in-body nops A {nops_a} B {nops_b} (fall-through A {live_a} B {live_b}); linker stubs A {stubs_a} B {stubs_b}")
    for k in ("nops", "veneer", "codegen", "operands"):
        if cls[k]:
            print(f"  {k}: {' '.join(cls[k][: o.list])}{' ...' if len(cls[k]) > o.list else ''}")
    for k, v in (("only in A", only_a), ("only in B", only_b)):
        if v:
            print(f"  {k}: {' '.join(v[: o.list])}{' ...' if len(v) > o.list else ''}")
    if o.show_live:
        shown = 0
        for n in own:
            body = fa[n]
            for i, ins in enumerate(body):
                if NOP.match(ins) and i > 0 and not NOP.match(body[i - 1]) and not BARRIER.match(body[i - 1]):
                    nxt = next((x for x in body[i:] if not NOP.match(x)), "")
                    print(f"  fall-through nop in A {n}: after '{body[i - 1]}' -> '{ins}' -> '{nxt}'")
                    shown += 1
                    break
            if shown >= o.show_live:
                break
    return 0 if not cls["nops"] and not cls["veneer"] and not cls["codegen"] else 1


if __name__ == "__main__":
    sys.exit(main())
