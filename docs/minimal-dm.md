# Minimal Usable Debug Module: Register Set and Implementation Guide

This document describes the smallest set of Debug Module (DM) registers and
behaviors that supports **basic debug functionality with abstract register
and memory access** on a single-hart RV32 system, and what the tooling
(OpenOCD's `riscv-013` driver) does with it.

The architecture context (JTAG, DTM, DMI, DM, hart debug logic) is in
[debug-architecture.md](debug-architecture.md); this document only covers
the DM itself.

References used: the RISC-V External Debug Support specification,
[version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf),
plus the OpenOCD `riscv-013.c` / `riscv-013_reg.c` sources of the current
`riscv` branch. Register and driver details below were verified against
those sources.

## Table of contents

- [TL;DR](#tldr)
- [Why this set, per the spec](#why-this-set-per-the-spec)
- [Field-level detail](#field-level-detail)
- [Required protocol behaviors](#required-protocol-behaviors)
- [Worked debugger flows](#worked-debugger-flows)
- [DTM side requirements](#dtm-side-requirements)
- [Hart-side hooks the DM needs](#hart-side-hooks-the-dm-needs)
- [Tool compatibility (OpenOCD)](#tool-compatibility-openocd)
- [What to add next, in order of usefulness](#what-to-add-next-in-order-of-usefulness)

## TL;DR

Six DMI register addresses, everything else hardwired to read 0:

| DMI addr | Register | What you implement |
|---|---|---|
| 0x04, 0x05 | `data0`, `data1` | 2× R/W argument registers (`datacount=2`) |
| 0x10 | `dmcontrol` | `dmactive`, `haltreq`, `resumereq`, `ndmreset`, 0-bit `hartsel` |
| 0x11 | `dmstatus` | running/halted/resumeack/nonexistent bits, `version=2`, `authenticated=1` |
| 0x16 | `abstractcs` | `busy`, `cmderr` (W1C), `progbufsize=0`, `datacount=2` |
| 0x17 | `command` | write-only trigger for cmdtype 0 (Access Register) and 2 (Access Memory) |

**All other DMI addresses read 0, writes ignored** — this is spec-mandated
("When read, unimplemented Debug Module DMI Registers return 0. Writing them
has no effect.") and tooling depends on it (OpenOCD probes `hartinfo`,
`sbcs`, `abstractcs`, etc. and infers absence from 0).

## Why this set, per the spec

0.13.2 §3 defines conformance as: required features (info, halt/resume,
halt status, halted GPR access, reset control) **plus** at least one of
{Program Buffer, System Bus Access, Access Memory command} **plus** one of:

- **(a)** Program Buffer, or
- **(b)** abstract access to *all* registers visible to software (all GPRs +
  all CSRs), or
- **(c)** abstract access to **GPRs + dcsr + dpc only** — this is the
  officially named **"Minimal RISC-V Debug Specification 0.13.2"** profile.

So the absolute floor is even smaller than the table above (no `data1`, no
Access Memory, 3 abstract-accessible regnos). But this document targets
*usable with memory access*, and the cheapest memory mechanism is the
**Access Memory abstract command** — no progbuf, no debug ROM, no bus
master. That needs `data1` (address argument), giving the table above.

Bonus: if your Access Register accepts **all implemented CSRs** (not just
dcsr/dpc), you satisfy (b), and this set makes you *fully* 0.13.2-conformant,
not just "minimal"-conformant. Routing regno 0x000–0xfff into your existing
halted-CSR read/write path is usually ~free in RTL.

## Field-level detail

### dmcontrol (0x10)

Read/write with readback (OpenOCD discovers `HARTSELLEN`/`hasel` by writing
all-ones and reading back, so store the writable bits):

| Bits | Field | Minimal implementation |
|---|---|---|
| 31 | haltreq | set/clear halt request → hart; W0 clears |
| 30 | resumereq | pulse resume; clears resumeack |
| 29 | hartreset | optional, else read 0 |
| 28 | ackhavereset | implement iff you do havereset bits |
| 26 | hasel | read 0 |
| 25:16, 15:6 | hartsello/hi | **0 bits implemented** (HARTSELLEN=0) — writes ignored, read 0 |
| 1 | ndmreset | 1 wire to system reset (needed for "debug from first instruction" + OpenOCD `reset`) |
| 0 | dmactive | 0 = reset entire DM state (data, cmderr, sticky bits, haltreq…) |

### dmstatus (0x11)

Read-only:

| Bits | Field | Minimal implementation |
|---|---|---|
| 20, 19 | all/anyhavereset | sticky havereset, else 0 |
| 17, 16 | all/anyresumeack | **required in practice** — hart sets on leaving halted, cleared by resumereq |
| 15, 14 | all/anynonexistent | tie 0 for the single hart (needed for OpenOCD's hart enumeration) |
| 13, 12 | all/anyunavail | tie 0 (or assert during ndmreset) |
| 11, 10 | all/anyrunning | from hart |
| 9, 8 | all/anyhalted | from hart |
| 7 | authenticated | **tie 1** |
| 5 | hasresethaltreq | 0 (no resethaltreq support) |
| 4 | confstrptrvalid | 0 |
| 3:0 | version | **2** (= 0.13) |

Single hart: each `allX` = its `anyX` = the one bit. `impebreak[22]`=0,
`authbusy`=0.

### abstractcs (0x16)

`busy[12]`, `cmderr[10:8]` W1C, `progbufsize[28:24]=0`, `datacount[3:0]=2`.
Read anytime; write clears `cmderr` bits.

### command (0x17)

Write-only; latches on write, sets `busy=1`, completes asynchronously. What
to accept (RV32, halted-only):

```
Access Register (cmdtype=0):
  [31:24]=0, [23]=0, aarsize[22:20]=2 only, aarpostincrement[19]=0,
  postexec[18]=0 only, transfer[17], write[16], regno[15:0]
  accept:  regno 0x1000–0x101f (GPRs), regno 0x000–0xfff (CSRs you have)
  result:  read → data0 = value;  write → register ← data0

Access Memory (cmdtype=2):
  [31:24]=2, aamvirtual[23]=0, aamsize[22:20]=2 only, aampostincrement[19]=0,
  [18:17]=0, write[16], [15:14] target-specific=0
  data1 = address, data0 = data (RV32: arg0=data0, arg1=data1)
  halted hart only → issue load/store through the hart's LSU
```

Everything else → `cmderr=2` (not supported). That includes `aarsize=3/4` —
**this is load-bearing**: OpenOCD detects XLEN by trying a 64-bit Access
Register and expecting failure on RV32.

## Required protocol behaviors

These are the usual minimal-DM bugs; each one is spec-required:

1. **`cmderr` busy rules**: writing `command`/`abstractcs`, or *reading or
   writing* `data0`/`data1` while `busy=1` must set `cmderr=1` (busy) *if
   cmderr==0*. Reads of `abstractcs` must not. This is what makes batched
   debugger scans safe.
2. **`cmderr` gates execution**: while non-zero, writes to `command` are
   ignored entirely.
3. **Sticky `cmderr`** until the debugger writes 1 to the bits.
4. **`dmactive=0`** must reset all DM state (this is the debugger's
   "unwedge everything" lever).
5. **`resumeack`**: OpenOCD polls `allresumeack` after resume — without it,
   resume times out.
6. **Unimplemented = read 0, write ignored** — including `sbcs` (→ no SBA),
   `abstractauto`, `hartinfo`, `haltsum*`, `nextdm`, `progbuf*`, `authdata`.
7. `datacount=2` — Access Memory on RV32 takes its address in `data1`;
   `datacount=1` breaks memory commands.
8. Errors during the memory load/store (bus fault, PMP…) → `cmderr=5` (bus)
   or 3 (exception); command issued while running → `cmderr=4`
   (halt/resume).

## Worked debugger flows

```
halt:    dmcontrol <= dmactive=1 | haltreq=1
         poll dmstatus.allhalted==1

read x10: command <= {cmdtype=0, aarsize=2, transfer=1, write=0, regno=0x100a}
         poll abstractcs.busy==0, cmderr==0
         data0 → value          (write reg: data0 <= value, write=1)

read mem: data1 <= addr
         command <= {cmdtype=2, aamsize=2, write=0}
         poll busy==0
         data0 → word           (write mem: data0 <= value, data1 <= addr, write=1)

resume:  dmcontrol <= resumereq=1
         poll dmstatus.allresumeack==1
```

## DTM side requirements

A full implementation guide for this layer (TAP, `dtmcs`/`dmi` registers,
busy/sticky protocol, timing, CDC) is in [jtag-dtm.md](jtag-dtm.md).

IR=5 bits; implement IDCODE (0x01), BYPASS (0x00/0x1f + all unimplemented
IRs), `dtmcs` (0x10: `version=1`, `abits=7` min — spec says 7–32, `idle`,
`dmireset`/`dmihardreset`, `dmistat`), `dmi` (0x11: with `abits=7`, a
41-bit scan reg: `address[6:0]` in bits [40:34], `data[31:0]` in bits
[33:2], `op[1:0]` in bits [1:0] — op scanned first, address last). If the
DM logic isn't in the TCK domain, set `idle` to cover synchronization.

## Hart-side hooks the DM needs

- halt request in; `running/halted` + `resumeack` status out
- read/write port into the GPR file while halted (one r/w port suffices)
- read/write path into CSRs while halted (reuse your CSR file port; dcsr/dpc
  must exist)
- a way to push one load/store through the hart's LSU while halted (either a
  "debug LSU command" FSM driving the existing bus interface, or stuff a
  `lw/sw` µ-op into the pipeline — the hart is halted, so the LSU is idle)
- `ndmreset` wire into system reset
- `ebreak`→Debug Mode routing per `dcsr.ebreakm/ebreaku` (this is what makes
  **software breakpoints** work once the debugger can write memory), and
  `dcsr.step`+`cause` for single-step
- dscratch not needed (no debug ROM), triggers optional (OpenOCD falls back
  to ebreak patching if `tselect` reads fail)

## Tool compatibility (OpenOCD)

Verified against `riscv-013.c` / `riscv-013_reg.c` (riscv-openocd,
`riscv` branch):

- Discovery works: dmstatus version=2 → OK; all-ones `hartsel` write reads
  back 0 → 1 hart; `anynonexistent=0` → enumerated.
- `examine()` reads `hartinfo` (0 fine), `sbcs` (0 fine), `abstractcs`;
  progbufsize=0 only produces a warning about `fence` — then halts, reads
  dcsr/dpc/misa via abstract commands, resumes if it was running.
- Memory method order is progbuf → sysbus → abstract; with the first two
  absent it uses **Access Memory automatically** (`riscv set_memaccess
  abstract` pins it).
- Optional CSRs are probed by abstract read; not-supported cleanly marks
  them non-existent.

## What to add next, in order of usefulness

1. **Trigger module** (tselect/tdata1/tdata2/tinfo) — hardware
   breakpoints/watchpoints; needed for debugging ROM/flash.
2. `haltsum0` (0x40) — one flop, future-proofs multi-hart.
3. `havereset`/`ackhavereset` sticky bits — OpenOCD handles them when
   present.
4. `resethaltreq` — "reset and halt" (needs core reset-entry support).
5. SBA (sbcs/sbaddress0/sbdata0) — memory access while running, flash
   loaders; costs a bus master.
6. `abstractauto` + `aarpostincrement`/`aampostincrement` — burst
   performance.
7. Program Buffer — usually *more* work (debug ROM, impebreak, hartinfo
   shadowing) for little gain over what you already have.

**Reference implementations to crib from**: OpenTitan `rv_dm` (essentially
this exact profile + optional SBA, well-documented), PULP `riscv-dm` (used
with Ibex/CVA6), and SiFive's original DM.
