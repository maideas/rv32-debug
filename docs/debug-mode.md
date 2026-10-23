# RV32 Debug Mode and the DM–Hart Connection

This document describes the **CPU side** of external debug: what Debug Mode
*is* as an execution state of an RV32 hart, how the hart enters it, how it
behaves while halted, and how it leaves it — and then the concrete
**interface between the Debug Module and the hart**: the signal list, the
handshake timing, and how the abstract commands of the minimal DM
([minimal-dm.md](minimal-dm.md)) map onto debug ports inside the hart.

It is the hart-side companion of [minimal-dm.md](minimal-dm.md) (the DM
register set), which sits on top of the transport described in
[jtag-dtm.md](jtag-dtm.md); the debugger workflows that drive all of this
are in [debug-sequences.md](debug-sequences.md); the full layered picture
is in [debug-architecture.md](debug-architecture.md).

References used: the RISC-V External Debug Support specification,
[version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
(what deployed silicon implements) and
[version 1.0](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf),
the PULP/OpenTitan debug module (`rv_dm`, used with Ibex) and the OpenOCD
`riscv-013` driver. Behavior details below were verified against those
sources and against the DM register set described in
[minimal-dm.md](minimal-dm.md).

## Table of contents

- [What Debug Mode is (and is not)](#what-debug-mode-is-and-is-not)
- [The hart's debug state machine](#the-harts-debug-state-machine)
- [Entering Debug Mode](#entering-debug-mode)
- [Behavior while halted](#behavior-while-halted)
- [ebreak re-routing and software breakpoints](#ebreak-re-routing-and-software-breakpoints)
- [Single stepping](#single-stepping)
- [Leaving Debug Mode: dret and hardware resume](#leaving-debug-mode-dret-and-hardware-resume)
- [The debug CSRs (dcsr, dpc, dscratch)](#the-debug-csrs-dcsr-dpc-dscratch)
- [The interface between DM and hart](#the-interface-between-dm-and-hart)
- [One command, end to end](#one-command-end-to-end)
- [Related documents](#related-documents)

## What Debug Mode is (and is not)

Debug Mode is a special **execution state** of the hart, orthogonal to the
privilege modes (M/S/U). It is *not* a trap or exception:

- Entering it does **not** update `mepc`/`mcause`/`mtval`, does not go
  through `mtvec`, and cannot be intercepted or disabled by running
  software. A hart can be dragged into Debug Mode from any privilege mode
  (and even from inside an interrupt handler) at any time.
- Running software cannot observe it architecturally — only through timing
  side effects (lost cycles) and, after the fact, through the debug CSRs
  (`dcsr.cause`, `dpc`), which normal code is discouraged from touching.
- While the hart is in Debug Mode it does not retire normal instructions
  and does not take interrupts; everything else (GPRs, memory, CSRs) is
  frozen but fully accessible to the DM.

The spec frames Debug Mode as: *the hart fetches instructions from a debug
ROM* (usually mapped inside the DM) and executes them at M-privilege under
special rules. The minimal DM described in this repo has no debug ROM and
no Program Buffer, so Debug Mode is realized in its quiesced form — the
hart executes *nothing*, and every service the ROM would perform in
software (register access, memory access, resume bookkeeping) is a
hardware port driven by the DM. Both realizations show identical
observable behavior to the debugger; see
[The minimal DM twist](#the-minimal-dm-twist-debug-mode-without-a-debug-rom)
below.

## The hart's debug state machine

Externally (through `dmstatus`, see
[debug-architecture.md](debug-architecture.md) "Hart states") a hart is in
exactly one of: **running** / **halted** (in Debug Mode) / **unavailable**
(in reset or powered down) / **nonexistent**. Internally, the run-control
part relevant here is a two-state machine plus the reset entry:

```
        halt_req | ebreak (per dcsr.ebreakm/u) | trigger action=1
        | single step done | resethaltreq out of reset
   ┌──────────┐                                       ┌──────────────┐
   │ RUNNING  │ ────────────────────────────────────► │    HALTED    │
   │          │                                       │  (Debug Mode)│
   └──────────┘ ◄──────────────────────────────────── └──────────────┘
        ▲           resume_req: pc ← dpc, prv ← dcsr.prv
        │           (with dcsr.step=1 the hart re-enters HALTED
        │            after retiring exactly one instruction, cause=4)
        │
        │  ndmreset / power-on reset: hart comes up RUNNING
        └── (unless a halt request is pending at reset deassertion)
```

Transitions, in terms of the signals of
[The interface between DM and hart](#the-interface-between-dm-and-hart):

| From | To | Trigger | DM-visible effect |
|---|---|---|---|
| running | halted | `halt_req` rising (or held) | `dmstatus.allhalted=1`, `dcsr.cause=3` |
| running | halted | `ebreak` with routing enabled | `dcsr.cause=1` |
| running | halted | step completion after resume | `dcsr.cause=4` |
| running | halted | trigger with action=1 (optional) | `dcsr.cause=2` |
| reset | halted | `resethaltreq` (optional) | `dcsr.cause=5` |
| halted | running | `resume_req` pulse, `halt_req=0` | `dmstatus.allresumeack=1` |
| any | running | `ndmreset` pulse or power-on reset | hart restarts from reset vector |

## Entering Debug Mode

### Entry causes

A hart enters Debug Mode for exactly one of five spec-defined reasons,
recorded in `dcsr.cause` so the debugger knows *why* it halted:

| `dcsr.cause` | Meaning | Exists in the minimal DM? |
|---|---|---|
| 1 | `ebreak` executed, re-routed per `dcsr.ebreakm`/`ebreaks`/`ebreaku` | **yes** — this is how software breakpoints work |
| 2 | trigger (breakpoint/watchpoint) fired with action=1 | no (no trigger module; OpenOCD falls back to ebreak patching) |
| 3 | halt request (`dmcontrol.haltreq` → `halt_req` wire) | **yes** — the primary mechanism |
| 4 | single step completed (`dcsr.step`) | **yes** |
| 5 | resethaltreq: halted on leaving reset | no (`hasresethaltreq=0`; debugger emulates it by holding `halt_req` across the `ndmreset` pulse) |

Rules for the two workhorses:

- **Halt request.** `dmcontrol.haltreq` is a level: while 1, the DM's
  `halt_req` wire is asserted and the hart must halt. The hart honors it
  at the next instruction boundary (see below), promptly — a hart buried
  in a stall, a cache miss, or a `wfi` must still halt in bounded time
  (`wfi` must never block a halt request). Clearing `halt_req` does *not*
  resume the hart; it only releases the request. Resume is a separate,
  explicit action.
- **ebreak re-routing.** `ebreak` (and `c.ebreak`) takes the normal
  breakpoint exception (`mcause=3`) *unless* the `dcsr.ebreakX` bit for
  the current privilege mode is set, in which case the hart enters Debug
  Mode with `cause=1` instead. Details in
  [ebreak re-routing](#ebreak-re-routing-and-software-breakpoints).

### What happens at entry

Entry is **precise**: whatever instruction is in flight completes (or, if
it has not started, is not started), no architectural state is corrupted,
and no further instruction retires. The hardware actions at the halt
boundary are:

1. `dcsr.prv` ← current privilege mode (so resume can restore it).
2. `dcsr.cause` ← entry cause from the table above (sticky until the next
   entry; read-only to software and debugger alike).
3. `dpc` ←
   - the address of the `ebreak`/trigger instruction itself, for
     `cause=1` (and trigger `cause=2`) — so the debugger can locate,
     patch back, and step over it; otherwise
   - the address of the **next instruction that would have executed** —
     for `haltreq` (3), step (4), resethaltreq (5). On a taken branch the
     next instruction is the *target*; on an interrupted instruction it is
     the interrupted instruction.
4. Fetch and commit are suppressed; the hart declares `halted` (this is
   what `dmstatus.all/anyhalted` reports).
5. A pending interrupt stays pending — it is neither taken nor lost; it
   will be taken after resume (unless the debugger masks it via `mie`).

Everything else — GPRs, memory, all other CSRs — is untouched by entry.

## Behavior while halted

While halted, from the hart's perspective:

- **No instructions retire.** PC is frozen (architecturally "parked" at
  the value `dpc` was set to on entry, though in the quiesced realization
  there is simply no fetch).
- **Interrupts are not taken.** They remain pending in `mip` and fire
  after resume. (With `dcsr.stepie=0`, held during single steps too.)
- **Timers and performance counters keep running.** `stoptime`/`stopcount`
  would stop them; the minimal profile implements neither (both read 0),
  so counts accumulate during a halt — normal for small cores.
- **Architectural state is frozen** except for what entry wrote
  (`dcsr.cause/prv`, `dpc`) and what the DM changes through its debug
  ports: abstract Access Register (GPRs, CSRs) and abstract Access Memory
  (one load/store through the hart's LSU). Those are the *only* ways
  anything changes while halted — see
  [How abstract commands use the ports](#how-abstract-commands-use-the-ports).
- **The hart's bus interface is idle** (nothing fetches, nothing
  accesses), which is exactly what lets the DM reuse the LSU for Access
  Memory commands.

### The minimal DM twist: Debug Mode without a debug ROM

In the spec's canonical shape, a halted hart *executes* — it fetches
debug-ROM code at M-privilege with special rules (interrupts masked,
exceptions end the program without updating CSRs, `wfi` is a nop,
`mret`/branches illegal in progbuf context, ending in `dret`). All the
"Debug Mode logic" lives in that ROM.

The minimal DM of this repo has no debug ROM, no Program Buffer, no
`dscratch`, and `hartinfo` reads 0. Debug Mode therefore never executes an
instruction. Consequences:

- Register and memory access are not "the ROM runs `csrr`/`lw` snippets"
  but **dedicated hardware ports** into the GPR file, the CSR file, and
  the LSU, sequenced by the DM's abstract-command FSM.
- There are no instruction-fetch side effects of halting (no ROM reads,
  no progbuf execution), and no privilege-mode subtleties: the ports see
  the plain register file, not an M-mode view of it.
- Resume is performed by the hart's run-control FSM in hardware (the
  `dret`-equivalent state update), not by a fetched `dret` — see
  [Leaving Debug Mode](#leaving-debug-mode-dret-and-hardware-resume).

This is a legitimate (and common) realization: the debugger never runs
ROM code; OpenOCD services everything through abstract commands on this
register set, as [minimal-dm.md](minimal-dm.md) documents.

## ebreak re-routing and software breakpoints

`ebreak` normally raises a breakpoint exception. Debug support adds one
option per privilege mode — `dcsr.ebreakm` (M-mode; always exists),
`dcsr.ebreaks`, `dcsr.ebreaku` (only if those modes exist; on a plain
M-mode RV32 they read 0):

| `dcsr.ebreakX` | `ebreak` in mode X does |
|---|---|
| 0 | breakpoint exception: `mepc ← pc`, `mcause=3`, trap to `mtvec` |
| 1 | **enter Debug Mode**, `dcsr.cause=1`, `dpc ← address of the ebreak` |

That single bit is the entire hardware cost of **software breakpoints**.
The rest is a debugger protocol, walked through in
[debug-sequences.md](debug-sequences.md) "Software breakpoints":

1. While halted, the debugger replaces the first instruction at the
   breakpoint address with `ebreak` (`c.ebreak` — the 16-bit compressed
   form — if the slot is 2 bytes wide; never smash 4 bytes of a compressed
   slot).
2. Resume. When execution reaches the slot, the hart halts with
   `cause=1` and `dpc` pointing at the patched instruction — the debugger
   reads `dpc` (abstract CSR read) to learn where it stopped.
3. To continue, the debugger patches the original instruction back and
   writes `dpc` past it (`dpc + 2/4`), or restores and re-executes it.
4. To remove the breakpoint, restore the original instruction.

Two caveats in the minimal configuration: this is the **only** breakpoint
mechanism (there is no trigger module), and it **requires writable memory**
— for code in ROM/flash you need to add hardware breakpoints (triggers;
the first item in "What to add next" in [minimal-dm.md](minimal-dm.md)).

## Single stepping

Single stepping is the *combination* of step and resume:

1. The debugger (halted hart) writes `dcsr.step=1` (abstract CSR write).
2. Resume as usual (`resumereq`). The hart leaves Debug Mode and executes
   **exactly one instruction**.
3. The hart immediately re-enters Debug Mode with `dcsr.cause=4` and
   `dpc ← address of the next instruction to execute` (branch/jump taken
   into account: the target).
4. The debugger polls `dmstatus.allhalted`, inspects state, and repeats.
   `dcsr.step` **stays set** across the re-entry — the hart keeps halting
   after every instruction until the debugger explicitly clears it (it
   must clear it before a normal resume).

Details that implementations get wrong:

- "Exactly one instruction" means one **retired** instruction — an
  interrupted, faulted, or non-retiring attempt does not count; the hart
  re-enters Debug Mode only after a real instruction boundary.
- If the stepped instruction **raises an exception**, the exception is
  taken *normally* (`mepc`/`mcause`/`mtval` updated, trap vector
  selected), and the hart then re-enters Debug Mode before the handler's
  first instruction: `dpc` = handler entry address, `cause=4`. The
  debugger sees a "step" that landed in a trap.
- `dcsr.stepie=0` (the minimal implementation; tie the bit to 0):
  interrupts raised during the step are held pending, not taken, so a
  step cannot be pre-empted invisibly.

## Leaving Debug Mode: dret and hardware resume

### dret

`dret` (encoding `0x7b200073`) is the architecturally
defined way out of Debug Mode: `pc ← dpc`, privilege mode ← `dcsr.prv`. It
is legal **only** in Debug Mode — executed anywhere else it is an illegal
instruction. In ROM-based DMs the debug ROM executes `dret` when it sees
a resume request from the DM.

### Hardware resume in the minimal profile

With no ROM, the hart's run-control FSM performs the **dret-equivalent
state update on the `resume_req` wire**:

- `pc ← dpc`
- privilege mode ← `dcsr.prv`
- if `dcsr.step=1`, arm single-step (halt again after one instruction)
- declare `running`, and report the transition so the DM can set
  `dmstatus.all/anyresumeack`

The protocol rules around resume, all spec-mandated and all load-bearing:

1. **`resumereq` is ignored while `haltreq=1`** — the hart would re-halt
   immediately, so the DM must not even present it. The debugger clears
   `haltreq` first.
2. **`resumeack` exists because "not halted" is ambiguous.** The hart may
   re-halt instantly (an `ebreak` at `dpc`, a trigger, a held `halt_req`),
   so polling `allhalted` cannot prove resumption. The DM therefore keeps
   a `resumeack` flop, **set by the hart when it actually leaves halted,
   cleared by the `resumereq` write itself** — the debugger polls
   `allresumeack` after resume, and without it OpenOCD's resume times
   out (see [minimal-dm.md](minimal-dm.md) "Required protocol
   behaviors").
3. While `abstractcs.busy` (an abstract command executing), `haltreq`,
   `resumereq`, and `ackhavereset` must all be 0 — the debugger obeys
   this, and the DM's command FSM only runs on a halted, stable hart
   anyway.

## The debug CSRs (dcsr, dpc, dscratch)

Three CSRs exist solely for debug; they are accessible to M-mode code only
while the hart is in Debug Mode (never in the minimal profile, since
nothing executes there) and to the debugger through abstract Access
Register commands. All other accesses are illegal instructions.

### dcsr (0x7b0) — debug status and control

| Bits | Field | Behavior | Minimal implementation |
|---|---|---|---|
| 31:28 | `xdebugver` | external debug support version | read **4** (= 0.13) |
| 15 | `ebreakm` | `ebreak` in M-mode → Debug Mode | **R/W, functional** (software breakpoints) |
| 13 | `ebreaks` | same for S-mode | 0 (or R/W if S exists) |
| 12 | `ebreaku` | same for U-mode | 0 (or R/W if U exists) |
| 11 | `stepie` | interrupts taken during single step | read 0 (interrupts masked during step) |
| 10 | `stopcount` | halt performance counters while halted | read 0 |
| 9 | `stoptime` | halt timers while halted | read 0 |
| 8:6 | `cause` | why the hart entered Debug Mode | read-only, written by hardware at entry |
| 4 | `mprven` | MPRV applies to debug-mode accesses | 0 (only relevant with ROM/progbuf execution) |
| 3 | `nmip` | non-maskable interrupt pending | read 0 |
| 2 | `step` | single-step on next resume | **R/W, functional** (single stepping) |
| 1:0 | `prv` | privilege mode to resume into | written by hardware at entry, writable by debugger |

### dpc (0x7b1) — debug PC

R/W, any value. Written by hardware at Debug Mode entry (see
[What happens at entry](#what-happens-at-entry)); the debugger writes it
to redirect execution at resume (e.g. step over a patched `ebreak`).

### dscratch0/1 (0x7b2/0x7b3) — debug scratch

Scratch registers for debug ROM code. **Not needed here** — no ROM ever
runs — and they may be omitted entirely (an abstract access to them then
answers `cmderr=3`, correctly telling the debugger they don't exist).

## The interface between DM and hart

Everything above converges into one flat bundle of wires between the DM
and the hart. This is the complete interface for the minimal DM of
[minimal-dm.md](minimal-dm.md) — a handful of request/status lines, three
data-access ports, and the reset wire:

```
┌─────────────────────────────────┐              ┌────────────────────────────────┐
│         Debug Module            │              │            RV32 hart           │
│   (minimal register set:         │              │                                │
│    data0/1, dmcontrol,          │              │   run-control FSM              │
│    dmstatus, abstractcs,        │              │   running ⇄ halted            │
│    command)                     │              │                                │
│                                 │              │                                │
│ dmcontrol.haltreq ──flop──► halt_req ─────────►│  halt request (level)         │
│ dmcontrol.resumereq ──W1──► resume_req ───────►│  resume request (pulse)        │
│ dmcontrol.ndmreset ──flop──► ndmreset ────────►│  system reset (not DM/DTM)     │
│                                 │              │                                │
│ abstract-command FSM            │              │                                │
│  ├─ Access Register ──► dbg_gpr_* ────────────►│  extra GPR-file r/w port      │
│  │       (regno 0x1000–         │              │                                │
│  │        0x101f)               │              │                                │
│  ├─ Access Register ──► dbg_csr_* ────────────►│  CSR-file read/write path      │
│  │       (regno 0x000–0xfff)     │              │  (incl. dcsr/dpc)             │
│  └─ Access Memory ────► dbg_mem_* ─────────────►│  LSU / bus interface           │
│                                 │              │  (addr from data1, data data0)│
│                                 │              │                                │
│ dmstatus bits 9:8   ◄─── halted ◄──────────────┤  status outputs                │
│ dmstatus bits 11:10 ◄── running ◄─────────────┤  (= !halted)                   │
│ dmstatus bits 17:16 ◄── resumeack ◄───────────┤  set on leaving halted,        │
│                                 │              │  cleared by resumereq write   │
└─────────────────────────────────┘              └────────────────────────────────┘
```

### Signal list

**DM → hart:**

| Signal | Derived from | Behavior |
|---|---|---|
| `halt_req` | `dmcontrol.haltreq` flop | Level. Honored at next instruction boundary, even from stalls/`wfi`. Cleared level does not resume. |
| `resume_req` | `dmcontrol.resumereq` (W1, self-clears) | One-shot pulse. Ignored while `halt_req=1`. Hart performs the dret-equivalent update. |
| `ndmreset` | `dmcontrol.ndmreset` flop | Level into the system reset net: resets everything *except* DM and DTM (incl. the hart and its debug CSRs). |
| `dbg_gpr_*` (req, `addr[4:0]`, we, `wdata[31:0]`, `rdata[31:0]`, done) | Access Register, regno 0x1000–0x101f | One r/w port into the GPR file. `x0` reads 0, writes ignored. Only meaningful while halted. |
| `dbg_csr_*` (req, `addr[11:0]`, we, `wdata[31:0]`, `rdata[31:0]`, done) | Access Register, regno 0x0000–0x0fff | Reuses the existing CSR read/write path (a mux in front of it). Must reach at least `dcsr` and `dpc`; routing *all* implemented CSRs here is nearly free and satisfies 0.13.2 conformance option (b). |
| `dbg_mem_*` (req, we, `size[1:0]`, `addr[31:0]`, `wdata[31:0]`, `rdata[31:0]`, done, `bus_err`, `exc`) | Access Memory (cmdtype=2) | Pushes one load/store through the hart's own LSU. `addr` = `data1`, data = `data0`. |

**Hart → DM:**

| Signal | Feeds | Behavior |
|---|---|---|
| `halted` | `dmstatus` bits 9:8 (any/allhalted) | 1 while in Debug Mode. |
| `running` | bits 11:10 (any/allrunning) | = `!halted` on a single hart. |
| `resumeack` | bits 17:16 (any/allresumeack) | Edge "left halted" sets the DM's flop; the `resumereq` write clears it. **Required** — OpenOCD polls it. |
| `unavail` (optional) | bits 13:12 | 0; may assert during `ndmreset`. |
| `havereset` (optional) | bits 19:18, sticky | Implement only together with `dmcontrol.ackhavereset` — the coupled pair described in [minimal-dm.md](minimal-dm.md). |

Timing and clocking rules:

- All request/status lines are plain handshake signals; the DM may sit in
  a different clock domain than the hart — synchronize exactly as for the
  DMI itself (CDC discussion in [jtag-dtm.md](jtag-dtm.md)).
- Halt entry has bounded latency (finish the in-flight instruction, then
  park) and is precise; the debugger just polls `dmstatus`.
- The abstract-command ports may take multiple cycles per access;
  `abstractcs.busy` spans the whole transaction. Handshake detail
  (req+done vs. ready/valid) is a free implementation choice.

### How abstract commands use the ports

**Access Register (`cmdtype=0`)** — one transaction on `dbg_gpr_*` or
`dbg_csr_*`:

- `write=0`: DM reads the port, result → `data0`.
- `write=1`: value from `data0` → port with `we=1`.
- `transfer=0`: no port access at all (no-op, just command validation).
- Hart must be halted, else `cmderr=4` (halt/resume). Nonexistent `regno`
  → `cmderr=3` (exception). `abstractcs.busy` covers the handshake.

**Access Memory (`cmdtype=2`)** — one transaction on `dbg_mem_*`:

- DM drives `addr` (from `data1`), size (from `aamsize`, 16/32-bit), and
  direction (`write`); result data → `data0`.
- The access rides the **hart's own LSU**, so it sees exactly what M-mode
  code would see: PMP/MPU, memory attributes, MMIO, R/W regions. That is
  the feature — memory access with the hart's true visibility, at no cost
  of a second bus master.
- Failure mapping: a **bus error** (bad address, access fault) →
  `cmderr=5`; the access raising an architecturally defined exception →
  `cmderr=3`.
- `aampostincrement` (if implemented) increments `data1` in the DM after
  each successful access — purely DM-side bookkeeping, the hart sees
  nothing.

### Halt and resume handshake, as a timeline

```
debugger                DM                          hart
────────────────────────────────────────────────────────────────
haltreq=1   ─────────► halt_req = 1 ─────────────► (retire in-flight insn)
                            │                       dpc/dcsr.cause/prv ← …
poll dmstatus ◄────────  halted = 1 ◄────────────── declare halted
(allhalted=1)
… abstract commands (hart stays halted, haltreq may stay 1)…
haltreq=0   ─────────► halt_req = 0
resumereq=1 ─────────► resume_req pulse ──────────► pc ← dpc, prv ← dcsr.prv
   (also clears                                    leave halted, step? arm step
    resumeack)
poll dmstatus ◄──────── resumeack = 1 ◄─────────── "left halted" edge
(allresumeack=1)        halted = 0, running = 1
```

The important asymmetries, in one place:

- **Halt is a level, resume is an edge.** `halt_req` held means "stay
  halted"; a stray `halt_req` during/after resume makes the hart re-halt
  immediately (which is why `resumeack` exists).
- **Entry writes state (`dcsr`, `dpc`); exit reads it** (`pc ← dpc`,
  `prv ← dcsr.prv`). The hart itself never fetches anything during either
  — the quiesced-Debug-Mode property.
- **The debugger is a polling loop**: `dmstatus.allhalted` after halt,
  `abstractcs.busy` after commands, `dmstatus.allresumeack` after resume.

### Reset interactions

Three distinct resets touch this interface — keep them apart:

| Reset | What it resets | Effect on debug state |
|---|---|---|
| TAP reset (TRST or 5×TMS=1) | the DTM only | DM and hart keep all state; DMI sticky errors clear via `dtmcs.dmireset`/`dmihardreset` (see [jtag-dtm.md](jtag-dtm.md)) |
| `dmactive=0` | the DM only (soft reset of all DM flops) | `halt_req`/`resume_req`/`ndmreset` drop — a held halt request silently clears (the debugger re-issues it; this lever is the debugger's "unwedge the DM" tool) |
| `ndmreset=1` | everything except DM/DTM — the whole SoC incl. the hart and its debug CSRs | a halted hart is reset and comes up **running** (unless `halt_req` is held across the pulse: it halts right out of reset — the minimal-DM substitute for `resethaltreq`; the flows are in [debug-sequences.md](debug-sequences.md) "Reset and debug from reset") |

`resethaltreq` itself is not implemented (dmstatus `hasresethaltreq=0`,
`dmcontrol` bits 3:2 read 0) — per the coupled-bits rule in
[minimal-dm.md](minimal-dm.md), capability and control bits appear
together or not at all.

### What this interface deliberately omits

Compared to a full-spec (ROM-based) DM, the minimal DM's hart interface
does **not** contain:

- **Debug ROM / progbuf interface** — nothing is ever fetched in Debug
  Mode, so no `hartinfo`/`nscratch`, no `dscratch` CSRs, no `impebreak`
  (`progbufsize=0`). This removes the entire "execute code while halted"
  machinery from the hart.
- **Trigger module** — `tselect`/`tdata*`/`tinfo` don't exist; software
  `ebreak` breakpoints are the only breakpoints. Needed next for
  ROM/flash debugging (see "What to add next" in
  [minimal-dm.md](minimal-dm.md)).
- **Hart array** — single hart, `hartsel` hardwired 0, no `hawindow`,
  `haltsum*` unused.
- **`dret` as an executed instruction** — it never runs in this profile.
  Either don't implement it at all (it is then simply an illegal
  instruction, which is also its correct behavior *outside* Debug Mode),
  or implement it and leave it unreachable; the hardware resume path
  reproduces its architectural effect.
- **System Bus Access** — SBA would be a DM↔fabric bus master, not part
  of the DM↔hart interface at all.

## One command, end to end

To tie the layers together, here is "halt a running hart and read GPR
x10" (the full DMI-level flows are in [debug-sequences.md](debug-sequences.md))
traced through every hop — transport, DM, interface, hart:

| # | What happens | Where |
|---|---|---|
| 1 | JTAG `dmi` scan: write `dmcontrol ← 0x8000_0001` | DTM → DMI (see [jtag-dtm.md](jtag-dtm.md)) |
| 2 | `dmactive`/`haltreq` flops set; `halt_req` asserts | DM |
| 3 | Hart finishes the in-flight instruction, writes `dpc`/`dcsr.prv`/`dcsr.cause=3`, parks | hart run-control FSM |
| 4 | `halted` rises; `dmstatus` now reads `0x0000_0382` (allhalted \| anyhalted \| authenticated \| version=2) | DM ↔ hart status |
| 5 | Debugger polls `dmstatus` until allhalted, then scans `command ← 0x0022_100A` (Access Register, read, regno 0x100a) | DMI → DM |
| 6 | DM's abstract FSM drives `dbg_gpr_*` (addr=10), `abstractcs.busy` rises | DM ↔ hart GPR port |
| 7 | GPR file answers; result → `data0`; `busy` falls | DM |
| 8 | Debugger reads `data0` over DMI | DMI |
| 9 | Debugger scans `dmcontrol ← 0x0000_0001` then `0x4000_0001`: `halt_req` drops, `resume_req` pulses | DM |
| 10 | Hart: `pc ← dpc`, `prv ← dcsr.prv`, leaves halted; `resumeack` sets | hart run-control FSM |
| 11 | Debugger polls `dmstatus.allresumeack` — done | DM ↔ hart status |

## Related documents

| Document | What it covers relative to this one |
|---|---|
| [minimal-dm.md](minimal-dm.md) | The DM register set this interface serves: bit maps, protocol behaviors, OpenOCD compatibility |
| [debug-sequences.md](debug-sequences.md) | The same flows as concrete DMI operation listings (halt, inspect, step, breakpoints, reset, recovery) |
| [debug-architecture.md](debug-architecture.md) | The full layered picture: JTAG TAP → DTM → DMI → DM → hart debug logic, all DM registers, 0.13.2 vs 1.0 |
| [jtag-dtm.md](jtag-dtm.md) | The transport underneath: TAP FSM, `dtmcs`/`dmi` scan registers, op protocol, timing, CDC |
