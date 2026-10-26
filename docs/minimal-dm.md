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
- [Register and bit-field reference](#register-and-bit-field-reference)
- [Required protocol behaviors](#required-protocol-behaviors)
- [Worked debugger flows](#worked-debugger-flows)
- [DTM side requirements](#dtm-side-requirements)
- [Hart-side hooks the DM needs](#hart-side-hooks-the-dm-needs)
- [Tool compatibility (OpenOCD)](#tool-compatibility-openocd)
- [Tool compatibility (SEGGER J-Link)](#tool-compatibility-segger-j-link)
- [What to add next, in order of usefulness](#what-to-add-next-in-order-of-usefulness)

## TL;DR

Six DMI register addresses, everything else hardwired to read 0:

| DMI addr | Register | What you implement |
|---|---|---|
| 0x04, 0x05 | `data0`, `data1` | 2× R/W argument registers (`datacount=2`) |
| 0x10 | `dmcontrol` | `dmactive`, `haltreq`, `resumereq`, `ndmreset`, hart selection fixed to hart 0 (HARTSELLEN=0) |
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

So the absolute floor is only slightly smaller than the table above:
option (c) needs abstract access to just the GPRs, dcsr and dpc, but rule
2 still demands one memory mechanism. Of the three, the cheapest is the
**Access Memory abstract command** — no progbuf, no debug ROM, no bus
master. That needs `data1` (address argument), giving the table above.

Bonus: if your Access Register accepts **all implemented CSRs** (not just
dcsr/dpc), you satisfy (b), and this set makes you *fully* 0.13.2-conformant,
not just "minimal"-conformant. Routing regno 0x000–0xfff into your existing
halted-CSR read/write path is usually ~free in RTL.

## Register and bit-field reference

All DM registers are 32 bits wide, at the DMI addresses from the table
above. Notation used below:

- **R** read-only, **R/W** readable and writable, **W** write-only (reads
  unspecified), **W1** write-1-to-trigger (self-clears), **W1C**
  write-1-to-clear (sticky)
- "read 0" = hardwire to 0, ignore writes
- dmstatus reports each hart condition in two separately named fields: an
  `all`-variant ("true for every selected hart") and an `any`-variant
  ("true for at least one selected hart"), with the `all`-variant on the
  higher bit — e.g. `allhalted` (bit 9) above `anyhalted` (bit 8). On a
  single-hart system both fields of a pair are driven by the same signal.

### dmcontrol (0x10) — what the debugger writes

The debugger's main control knob. Implemented R/W bits must read back
what was written — OpenOCD discovers `HARTSELLEN` and `hasel` support by
writing all-ones and reading back.

| Bits | Field | Access | What it does | Minimal implementation |
|---|---|---|---|---|
| 31 | haltreq | W | 1 = request selected harts to halt; 0 = cancel the request | stored flop → hart's halt request input (reads may return the stored value) |
| 30 | resumereq | W1 | 1 = resume selected harts once; clears `resumeack`; ignored while haltreq=1 | one-shot pulse, self-clears |
| 29 | hartreset | R/W | 1 = hold selected harts in reset, 0 = release; reads 0 if unimplemented | optional — read 0 if absent (the debugger discovers this by writing 1 and reading back 0) |
| 28 | ackhavereset | W1 | 1 = clear sticky `havereset` of selected harts | only together with the sticky `havereset` status (dmstatus `allhavereset` and `anyhavereset`) — the two form a pair (see [Coupled bits](#coupled-bits-and-feature-probing) below) |
| 27 | reserved | R | — | read 0, writes ignored |
| 26 | hasel | R/W | 1 = additionally select harts via the hawindow mask | read 0 (writes ignored; the debugger probes by writing 1 and reading back 0) |
| 25:16 | hartsello | R/W | low 10 bits of hartsel, the DM-internal index of the hart that dmcontrol/abstract commands act on | HARTSELLEN=0: all bits read 0, writes ignored |
| 15:6 | hartselhi | R/W | high 10 bits of hartsel (the same hart index) | HARTSELLEN=0: all bits read 0, writes ignored |
| 5:4 | reserved | R | — | read 0, writes ignored |
| 3 | setresethaltreq | W1 | 1 = selected harts halt when they next leave reset (sticky until cleared) | read 0 — if you implement the feature, this must pair with dmstatus `hasresethaltreq` (bit 5) (see [Coupled bits](#coupled-bits-and-feature-probing) below) |
| 2 | clrresethaltreq | W1 | 1 = clear that request | read 0 |
| 1 | ndmreset | R/W | 1 = assert non-debug-module reset: resets everything except DM/DTM | stored flop → system reset wire (needed for "debug from first instruction" + OpenOCD `reset`) |
| 0 | dmactive | R/W | 0 = DM held in reset: all DM state takes reset values, and **only this bit may be written**; 1 = normal operation | stored flop; gate all DM state with it |

Concrete write values (single hart):

```
0x8000_0001   dmactive | haltreq     halt the hart
0x4000_0001   dmactive | resumereq   resume the hart
0x0000_0003   dmactive | ndmreset    reset the whole SoC (except DM/DTM)
0x0000_0001   dmactive              normal operation
0x0000_0000                        DM held in reset (power-up state)
```

### dmstatus (0x11) — what the debugger polls

Read-only status of the DM and the selected harts:

| Bits | Field | What it reports | Minimal implementation |
|---|---|---|---|
| 31:23 | reserved | — | read 0 |
| 22 | impebreak | implicit ebreak after the program buffer | 0 (no progbuf) |
| 21:20 | reserved | — | read 0 |
| 19 | allhavereset | every selected hart came out of reset, not yet acknowledged | 0, or (implemented together with dmcontrol `ackhavereset`): sticky 1 from reset deassertion until `ackhavereset` |
| 18 | anyhavereset | at least one selected hart came out of reset, not yet acknowledged | same signal as `allhavereset` (single hart) |
| 17 | allresumeack | every selected hart acknowledged the last resume (needed because a hart can re-halt immediately, so `allhalted` is no proof of resumption) | **required in practice**: flop set by the hart when it leaves halted, cleared by resumereq |
| 16 | anyresumeack | at least one selected hart acknowledged the last resume | same signal as `allresumeack` (single hart) |
| 15 | allnonexistent | none of the selected harts exist | 0 (hart 0 exists — needed for OpenOCD's hart enumeration) |
| 14 | anynonexistent | at least one selected hart doesn't exist | same signal as `allnonexistent` (single hart) |
| 13 | allunavail | every selected hart unavailable (in reset, powered down) | 0 (optionally assert during ndmreset) |
| 12 | anyunavail | at least one selected hart unavailable | same signal as `allunavail` (single hart) |
| 11 | allrunning | every selected hart executing normally | from hart (= !halted) |
| 10 | anyrunning | at least one selected hart executing normally | same signal as `allrunning` (single hart) |
| 9 | allhalted | every selected hart in Debug Mode | from hart |
| 8 | anyhalted | at least one selected hart in Debug Mode | same signal as `allhalted` (single hart) |
| 7 | authenticated | DM usable without authentication | **tie 1** |
| 6 | authbusy | authentication in progress | 0 |
| 5 | hasresethaltreq | resethaltreq supported | 0 |
| 4 | confstrptrvalid | confstrptr0–3 hold a valid pointer | 0 |
| 3:0 | version | spec version of the DM | **2** (= 0.13) |

Single hart: drive each `all`-field and the `any`-field directly below it
from the same signal. Example: a halted hart reads as `0x0000_0382`
(`allhalted` | `anyhalted` | `authenticated` | `version`).

### Coupled bits and feature probing

Two mechanisms in this register set come as pairs that must be implemented
together — or not at all:

**`havereset` acknowledgement.** `allhavereset` and `anyhavereset` (dmstatus
bits 19:18) are sticky: they read 1 once the selected hart has come out of
reset (whatever the reset cause), and the *only* specified way to clear
them is dmcontrol `ackhavereset` (bit 28) — the debugger's acknowledgement.
Two coherent implementations:

1. **Neither** (minimal): `allhavereset` and `anyhavereset` read 0,
   `ackhavereset` reads 0 and ignores writes. The debugger simply never
   learns that the hart was reset — fine on a single-hart system.
2. **Both**: a sticky flop set on reset deassertion and cleared by
   `ackhavereset`. OpenOCD's examine path acknowledges it (writes
   `dmactive | ackhavereset`) whenever it sees the bit set.

What does *not* work: the status bits without the acknowledge bit — once
set they could never clear, and the debugger would see "this hart was
reset" forever.

**`resethaltreq` capability.** dmcontrol bits 3 (`setresethaltreq`) and 2
(`clrresethaltreq`) request that selected harts halt the next time they
leave reset; dmstatus bit 5 (`hasresethaltreq`) advertises that the feature
exists. All three or none: a debugger reading `hasresethaltreq=0` skips the
mechanism entirely (and instead holds `haltreq` across the `ndmreset` pulse
so the hart halts as it leaves reset).

**Feature probing.** The debugger discovers optional dmcontrol features by
writing 1 and reading the register back: a bit that reads back 0 is not
implemented (`hartreset`, `hasel`). This is why "read 0, ignore writes"
for absent features must be literal hardware behavior — it is how the
debugger tells optional features apart.

### abstractcs (0x16) — abstract command status

Status of the abstract command engine — the debugger polls `busy` after
every `command` write and clears sticky errors via `cmderr` (W1C):

| Bits | Field | Access | Meaning |
|---|---|---|---|
| 31:29 | reserved | R | read 0 |
| 28:24 | progbufsize | R | implemented program buffer words: **0** |
| 23:13 | reserved | R | read 0 |
| 12 | busy | R | 1 = an abstract command is executing |
| 11 | reserved | R | read 0 |
| 10:8 | cmderr | R/W1C | sticky error code of the last command |
| 7:4 | reserved | R | read 0 |
| 3:0 | datacount | R | implemented data registers: **2** (`data0`, `data1`) |

`cmderr` codes:

| Value | Name | Set when |
|---|---|---|
| 0 | none | no error since the last clear |
| 1 | busy | `command`/`abstractcs`/`abstractauto` written, or a data register read or written, while an abstract command was executing |
| 2 | not supported | the command or an option isn't implemented for this hart |
| 3 | exception | the hart raised an exception executing the command, or the requested register doesn't exist |
| 4 | halt/resume | hart was in the wrong state (e.g. command requires halted, hart is running) |
| 5 | bus | bus error during the memory access |
| 7 | other | anything else |

Clear by writing 1s to the set bit positions: e.g. `cmderr=2` → write
`0x200` to abstractcs; `0x700` clears any code. While `cmderr != 0`,
writes to `command` are ignored (see
[Required protocol behaviors](#required-protocol-behaviors)).

### command (0x17) — trigger an abstract command

Write-only: the write latches the command and starts it; `abstractcs.busy`
rises until it completes. The minimal DM accepts exactly two command
shapes, only while the hart is halted.

**Access Register (`cmdtype=0`)** — read/write one register:

| Bits | Field | Accept | Meaning |
|---|---|---|---|
| 31:24 | cmdtype | 0 | Access Register |
| 23 | reserved | 0 only | — |
| 22:20 | aarsize | 2 only | 32-bit access |
| 19 | aarpostincrement | 0 only | regno is not auto-incremented (stays unsupported even with the burst extension — that concerns `aampostincrement` only) |
| 18 | postexec | 0 only | nothing to run afterwards (no progbuf) |
| 17 | transfer | 0/1 | 1 = perform the access; 0 = no-op |
| 16 | write | 0/1 | 0 = register → data0; 1 = data0 → register |
| 15:0 | regno | see below | which register |

Accepted `regno` values:

| Range | Meaning |
|---|---|
| 0x1000–0x101f | GPRs x0–x31 |
| 0x0000–0x0fff | CSRs you implement — must include dcsr (0x7b0) and dpc (0x7b1) |
| others | FPRs/custom: `cmderr=3` if the register doesn't exist in the hart |

Example — read GPR x10: write `0x0022_100A` to `command`
(cmdtype=0, aarsize=2, transfer=1, write=0, regno=0x100a), poll
abstractcs, read `data0`. To write x10: put the value in `data0` first,
then write `0x0023_100A` (write=1).

**Access Memory (`cmdtype=2`)** — one load/store through the halted hart:

| Bits | Field | Accept | Meaning |
|---|---|---|---|
| 31:24 | cmdtype | 2 | Access Memory |
| 23 | aamvirtual | 0 only | physical addresses only |
| 22:20 | aamsize | 1 or 2 | 16- or 32-bit access (8-bit = 0 optional). 16-bit is required for software breakpoints in compressed code; 3/4 → cmderr=2 |
| 19 | aampostincrement | 0 (1 = optional) | 0 = debugger re-writes `data1` per word; 1 = DM increments `data1` by the access size after each successful access (see [Optional burst extension](#optional-burst-extension) below) |
| 18:17 | reserved | 0 only | — |
| 16 | write | 0/1 | 0 = load; 1 = store |
| 15:14 | target-specific | 0 only | — |
| 13:0 | reserved | 0 only | — |

Arguments (RV32): address in `data1`, data in `data0` (arg0 = data0,
arg1 = data1). The DM pushes the load/store through the hart's own LSU,
so the access sees exactly what M-mode code would see (PMP, memory
regions, …).

Example — read a word: write the address to `data1`, write `0x0220_0000`
to `command` (cmdtype=2, aamsize=2, write=0), poll abstractcs, read
`data0`. To write: value → `data0`, address → `data1`, command
`0x0221_0000` (write=1).

**Anything else → rejection**, per the spec rules:

- unsupported command options (aarsize 3/4 — 64/128-bit — postexec=1,
  virtual memory, `aarpostincrement`, …) → `cmderr=2` (not supported).
  Rejecting `aarsize=3/4` is **load-bearing**: OpenOCD detects XLEN by
  attempting a 64-bit Access Register and expecting failure on RV32.
  (`aampostincrement` is the one postincrement that may be accepted —
  see [Optional burst extension](#optional-burst-extension).)
- requested register doesn't exist → `cmderr=3` (exception)
- hart not halted → `cmderr=4` (halt/resume)
- load/store faults on the bus → `cmderr=5` (bus)

### Optional burst extension

Two optional features turn the per-word block loops into bursts (the
sequences are in
[debug-sequences.md](debug-sequences.md), "Auto-increment bursts"):

- **`aampostincrement` = 1** (Access Memory command bit 19): after each
  *successful* access, increment `data1` by the access size (1/2/4
  bytes for aamsize 0/1/2). No increment on failure. Accept `0` always;
  accepting `1` is optional. A debugger discovers this by issuing a
  command with the bit set and checking for `cmderr=2`.
- **`abstractauto` (DMI 0x18)**, R/W, reset 0:

```
[31:16]  autoexecprogbuf   bit N -> progbufN (no progbuf here: 0)
[15:12]  reserved          0
[11:0]   autoexecdata      bit 0 -> data0
```

  When a bit is set, any read or write of the corresponding data
  register re-executes the last `command`. Detectable by writing bits
  and reading them back. While `autoexecdata` bit 0 is set, each `data0`
  access both carries a word and triggers the next access — one DMI op
  per word.

Both together need no other register additions. The base minimal DM
(both absent) stays conformant: `abstractauto` then reads 0, writes
ignored, and `aampostincrement=1` answers `cmderr=2`.

## Required protocol behaviors

These are the usual minimal-DM bugs; each one is spec-required:

1. **`cmderr` busy rules**: writing `command`/`abstractcs` (and
   `abstractauto`, if implemented), or *reading or writing*
   `data0`/`data1` while `busy=1` must set `cmderr=1` (busy) *if
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
   `abstractauto` (unless you add the
   [Optional burst extension](#optional-burst-extension)), `hartinfo`,
   `haltsum*`, `nextdm`, `progbuf*`, `authdata`.
7. `datacount=2` — Access Memory on RV32 takes its address in `data1`;
   `datacount=1` breaks memory commands.
8. Errors during the memory load/store (bus fault, PMP…) → `cmderr=5` (bus)
   or 3 (exception); command issued while running → `cmderr=4`
   (halt/resume).

## Worked debugger flows

```
halt:    dmcontrol <= dmactive=1 | haltreq=1        (= 0x8000_0001)
         poll dmstatus.allhalted==1                   (dmstatus = 0x382 when halted)
         dmcontrol <= dmactive=1                      (= 0x0000_0001: clear haltreq,
                                                       required before abstract commands)

read x10: command <= {cmdtype=0, aarsize=2, transfer=1, write=0, regno=0x100a}
                                               (= 0x0022_100A)
         poll abstractcs.busy==0, cmderr==0
         data0 → value          (write reg: data0 <= value first, then 0x0023_100A)

read mem: data1 <= addr
         command <= {cmdtype=2, aamsize=2, write=0}   (= 0x0220_0000)
         poll busy==0
         data0 → word           (write mem: data0 <= value, data1 <= addr,
                                 command = 0x0221_0000)

resume:  dmcontrol <= resumereq=1                    (= 0x4000_0001)
         poll dmstatus.allresumeack==1
```

## DTM side requirements

A full implementation guide for this layer (TAP, `dtmcs`/`dmi` registers,
busy/sticky protocol, timing, CDC) is in [jtag-dtm.md](jtag-dtm.md).
The debugger workflows built on this register set are in
[debug-sequences.md](debug-sequences.md).

IR=5 bits; implement IDCODE (0x01), BYPASS (0x00/0x1f + all unimplemented
IRs), `dtmcs` (0x10: `version=1`, `abits=7` min — spec says 7–32, `idle`,
`dmireset`/`dmihardreset`, `dmistat`), `dmi` (0x11: with `abits=7`, a
41-bit scan reg: `address[6:0]` in bits [40:34], `data[31:0]` in bits
[33:2], `op[1:0]` in bits [1:0] — op scanned first, address last). If the
DM logic isn't in the TCK domain, set `idle` to cover synchronization.

## Hart-side hooks the DM needs

A detailed description of the CPU side — Debug Mode behavior and this
interface's signals, handshake timing, and reset interactions — is in
[debug-mode.md](debug-mode.md); the signal-by-signal reference is
[dm-hart-interface.md](dm-hart-interface.md). In short:

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

- Discovery works: dmstatus version=2 → OK; an all-ones write to
  `hartsello` and `hartselhi` reads back 0 → 1 hart; `anynonexistent=0` →
  enumerated.
- `examine()` reads `hartinfo` (0 fine), `sbcs` (0 fine), `abstractcs`;
  progbufsize=0 only produces a warning about `fence` — then halts, reads
  dcsr/dpc/misa via abstract commands, resumes if it was running.
- Memory method order is progbuf → sysbus → abstract; with the first two
  absent it uses **Access Memory automatically** (`riscv set_mem_access
  abstract` pins it).
- Optional CSRs are probed by abstract read; not-supported cleanly marks
  them non-existent.

## Tool compatibility (SEGGER J-Link)

The same register set passes SEGGER's native J-Link driver (full contract
and usage: [toolchain-integration.md](toolchain-integration.md) →
"SEGGER J-Link"):

- The generic `RV32` device name connects against a spec-conformant
  single-DM part — no device-database entry needed for register and
  memory access.
- `progbufsize = 0` is fine: while halted, J-Link falls back to **Access
  Memory** (OpenOCD's `riscv set_mem_access abstract` pins the same
  choice).
- `sbcs.sbasize = 0` means **no memory access while the hart runs** —
  neither driver has another background mechanism. Live memory watching
  from the debugger is the feature SBA buys ("What to add next", #5).
- Single-hart parts are immune to J-Link's one-hart-at-a-time
  limitation.

## What to add next, in order of usefulness

1. **Trigger module** (tselect/tdata1/tdata2/tinfo) — hardware
   breakpoints/watchpoints; needed for debugging ROM/flash.
2. `haltsum0` (0x40) — one flop, future-proofs multi-hart.
3. Sticky `havereset` status plus `ackhavereset` (see
   [Coupled bits](#coupled-bits-and-feature-probing)) — OpenOCD handles
   them when present.
4. `resethaltreq` — "reset and halt" (needs core reset-entry support).
5. SBA (sbcs/sbaddress0/sbdata0) — memory access while running, flash
   loaders; costs a bus master. (SBA only accelerates the data-movement
   half of flash loading — the execution half is the target-run loader,
   see [flash-loaders.md](flash-loaders.md).)
6. `abstractauto` + `aampostincrement` — burst performance: 1 DMI op
   per memory word instead of 4 (`aarpostincrement` would add the same
   for register bursts; see
   [Optional burst extension](#optional-burst-extension) above and the
   burst sequences in [debug-sequences.md](debug-sequences.md)).
7. Program Buffer — usually *more* work (debug ROM, impebreak, hartinfo
   shadowing) for little gain over what you already have; what that work is,
   see [debug-rom.md](debug-rom.md).

**Reference implementations to crib from**: OpenTitan `rv_dm` (essentially
this exact profile + optional SBA, well-documented), PULP `riscv-dm` (used
with Ibex/CVA6), and SiFive's original DM.
