# The DM–Hart Interface: Signal Reference

This document is a standalone reference for the interface **between the
Debug Module (DM) and the RV32 hart**: which signals are exchanged, what
each one *means*, and the rules for driving and answering it ("handling").

It is the signal-level companion of [debug-mode.md](debug-mode.md), which
describes the same wires from the execution-state side (what Debug Mode
*is*, entry causes, `dcsr`/`dpc`, single stepping, `dret`). The DM
registers every signal derives from are specified in
[minimal-dm.md](minimal-dm.md), the DMI transport underneath in
[jtag-dtm.md](jtag-dtm.md), and the debugger-side operation listings that
drive these wires in [debug-sequences.md](debug-sequences.md). A behavioral
model of the whole bundle (TAP → DTM → DM → hart) is
[`src/mock/mock_target.hpp`](../src/mock/mock_target.hpp).

One caveat on the word "signal": the RISC-V debug specification defines
this interface as *behavior* — `dmcontrol`/`dmstatus` bits and the
required hart responses — not as a pin list. The bundle below is the
natural point-to-point realization of that behavior for a single-hart
minimal DM, and the shape production RTL (PULP/OpenTitan `rv_dm`) also
converges on; a multi-hart DM wraps the same lines in a hart array.

## Table of contents

- [Scope and model](#scope-and-model)
- [The interface at a glance](#the-interface-at-a-glance)
- [DM-to-hart signals](#dm-to-hart-signals)
  - [halt_req](#halt_req)
  - [resume_req](#resume_req)
  - [ndmreset](#ndmreset)
  - [dbg_gpr — the GPR debug port](#dbg_gpr--the-gpr-debug-port)
  - [dbg_csr — the CSR debug port](#dbg_csr--the-csr-debug-port)
  - [dbg_mem — the memory debug port](#dbg_mem--the-memory-debug-port)
- [Hart-to-DM signals](#hart-to-dm-signals)
  - [halted and running](#halted-and-running)
  - [resumeack](#resumeack)
  - [unavail and havereset (optional)](#unavail-and-havereset-optional)
  - [Where the halt reason lives](#where-the-halt-reason-lives)
- [How abstract commands drive the data ports](#how-abstract-commands-drive-the-data-ports)
- [Handling rules](#handling-rules)
- [Halt and resume handshake timeline](#halt-and-resume-handshake-timeline)
- [Reset interactions](#reset-interactions)
- [Deliberate omissions](#deliberate-omissions)
- [Related documents](#related-documents)

## Scope and model

- One RV32 hart, the minimal DM register set of
  [minimal-dm.md](minimal-dm.md): `data0`/`data1`, `dmcontrol`,
  `dmstatus`, `abstractcs`, `command`.
- No debug ROM and no Program Buffer: a halted hart executes *nothing*;
  every service the ROM would perform in software is a hardware port
  driven by the DM's abstract-command FSM (the "quiesced Debug Mode" of
  [debug-mode.md](debug-mode.md)). The debugger sees no difference.
- All request/status lines are plain handshake signals — no clock, no
  valid/stall protocol is mandated. If the DM and the hart sit in
  different clock domains, synchronize exactly as for the DMI itself
  (CDC discussion in [jtag-dtm.md](jtag-dtm.md)).
- `dmstatus` reports each condition as an `allX`/`anyX` pair; on a
  single hart the pairs are degenerate (`allX` = `anyX`), `hartsel` is
  hardwired 0.

## The interface at a glance

```
┌─────────────────────────────────┐              ┌────────────────────────────────┐
│         Debug Module            │              │            RV32 hart           │
│   (minimal register set:        │              │                                │
│    data0/1, dmcontrol,          │              │   run-control FSM              │
│    dmstatus, abstractcs,        │              │   running ⇄ halted             │
│    command)                     │              │                                │
│                                 │              │                                │
│ dmcontrol.haltreq ──flop──► halt_req ─────────►│  halt request (level)          │
│ dmcontrol.resumereq ──W1──► resume_req ───────►│  resume request (pulse)        │
│ dmcontrol.ndmreset ──flop──► ndmreset ────────►│  system reset (not DM/DTM)     │
│                                 │              │                                │
│ abstract-command FSM            │              │                                │
│  ├─ Access Register ──► dbg_gpr_* ────────────►│  extra GPR-file r/w port       │
│  │       (regno 0x1000–0x101f)  │              │                                │
│  ├─ Access Register ──► dbg_csr_* ────────────►│  CSR-file read/write path      │
│  │       (regno 0x000–0xfff)    │              │  (incl. dcsr/dpc)              │
│  └─ Access Memory ────► dbg_mem_* ────────────►│  LSU / bus interface           │
│                                 │              │  (addr = data1, data = data0)  │
│                                 │              │                                │
│ dmstatus bits 9:8   ◄─── halted ◄──────────────┤  status outputs                │
│ dmstatus bits 11:10 ◄── running ◄──────────────┤  (= !halted)                   │
│ dmstatus bits 17:16 ◄── resumeack ◄────────────┤  set on leaving halted,        │
│                                 │              │  cleared by resumereq write    │
└─────────────────────────────────┘              └────────────────────────────────┘
```

In words: three request lines and a reset wire from the DM into the hart,
three data-access ports (GPR file, CSR file, LSU) shared between the
abstract-command FSM and the hart's own register/memory logic, and status
lines out of the hart that `dmstatus` mirrors to the debugger. Everything
else — `dcsr`/`dpc`-adjacent bookkeeping, breakpoints, stepping — is
behavior *around* these wires, described in
[debug-mode.md](debug-mode.md).

## DM-to-hart signals

| Signal | Kind | Derived from | Meaning |
|---|---|---|---|
| `halt_req` | level | `dmcontrol.haltreq` flop (bit 31) | enter — and while held, stay in — Debug Mode |
| `resume_req` | pulse | `dmcontrol.resumereq` (bit 30, W1, self-clears) | leave Debug Mode once |
| `ndmreset` | level | `dmcontrol.ndmreset` flop (bit 1) | reset the system (everything but DM/DTM) |
| `dbg_gpr_*` | port | abstract FSM, Access Register, regno 0x1000–0x101f | one read/write into the GPR file |
| `dbg_csr_*` | port | abstract FSM, Access Register, regno 0x000–0xfff | one read/write into the CSR file |
| `dbg_mem_*` | port | abstract FSM, Access Memory (cmdtype=2) | one load/store through the hart's own LSU |

### halt_req

**Halt request. Level**, the stored value of `dmcontrol.haltreq`.

Meaning: while 1, the selected hart must halt — enter Debug Mode. This
is the primary "stop" mechanism (`dcsr.cause` will read 3).

Handling (hart side):

- Honored at the **next instruction boundary** — entry is *precise*:
  the in-flight instruction completes (or, if it never started, is not
  started), no architectural state is corrupted, and no further
  instruction retires.
- Latency is **bounded**: a hart buried in a stall, a cache miss, or a
  `wfi` must still halt promptly — `wfi` may never block a halt request.
- At the boundary the hart writes its entry state: `dcsr.prv` ← current
  privilege mode, `dcsr.cause` ← 3, `dpc` ← the next instruction that
  would have executed (the *target* of a taken branch; the interrupted
  instruction itself if an interrupt was about to preempt), suppresses
  fetch/commit, and asserts `halted`.
- A pending interrupt stays pending — neither taken nor lost; it fires
  after resume.
- **Dropping the level does not resume the hart.** It only releases the
  request; resume is a separate, explicit action (`resume_req`).

Handling (debugger/DM side): assert with `dmcontrol ← 0x8000_0001`
(`dmactive | haltreq`), poll `dmstatus.allhalted`, then clear with
`0x0000_0001` (keep `dmactive`) before running abstract commands — the
request level must be 0 while the DM's command FSM touches the hart.

### resume_req

**Resume request. One-shot pulse**, from a write of 1 to
`dmcontrol.resumereq` (bit 30, W1, self-clearing).

Meaning: leave Debug Mode once — the hardware equivalent of `dret`.

Handling (DM side):

- Gated by `halt_req`: **ignored while the halt request level is set**
  (spec: "resumereq is ignored if haltreq is set") — no pulse ever
  reaches the hart; the debugger must clear `haltreq` first.
- The same write also **clears the DM's `resumeack` flop**, so the
  acknowledgment the debugger polls afterwards is provably fresh.

Handling (hart side), on the pulse — the `dret`-equivalent state update:

- `pc ← dpc`, privilege mode ← `dcsr.prv`;
- arm single-step if `dcsr.step=1` (re-enter Debug Mode after exactly one
  *retired* instruction, `cause=4`);
- leave halted, assert `running`, and signal the "left halted" edge that
  sets the DM's `resumeack` flop.

Only meaningful while halted; a resumed hart may re-halt instantly (an
`ebreak` at `dpc`, a trigger, a held `halt_req`) — which is exactly why
`resumeack` exists.

### ndmreset

**Non-debug-module reset. Level**, the stored `dmcontrol.ndmreset`
(bit 1), driven into the system reset net.

Meaning: reset **everything except the DM and DTM** — the whole SoC
including the hart and its debug CSRs.

Handling:

- Assert with `dmcontrol ← 0x0000_0003` (`dmactive | ndmreset`),
  release with `0x0000_0001`.
- A halted hart is reset: debug state (`dcsr`, `dpc`, GPRs, everything)
  is lost, and the hart comes back up **running** from the reset vector.
- To debug from the first instruction, the debugger holds `halt_req`
  asserted across the `ndmreset` pulse: the hart then halts right out of
  reset. This is the minimal profile's substitute for the unimplemented
  `resethaltreq` (`dmstatus.hasresethaltreq=0`; the flows are in
  [debug-sequences.md](debug-sequences.md) "Reset and debug from reset").
- Keep apart from the two *other* resets that touch this interface — see
  [Reset interactions](#reset-interactions).

### dbg_gpr — the GPR debug port

Bundle: `req`, `addr[4:0]`, `we`, `wdata[31:0]`, `rdata[31:0]`, `done`.

Driven by the DM's abstract-command FSM for **Access Register**
(`cmdtype=0`) when `regno` is 0x1000–0x101f (`regno = 0x1000 + rd`):
one read (`write=0`: `rdata` → `data0`) or write (`write=1`: `data0` →
`wdata`, `we=1`) of a general-purpose register.

Handling rules:

- `x0` reads as 0; writes to it are ignored.
- Only meaningful while **halted** — with a running hart the command
  aborts with `cmderr=4` (halt/resume).
- The port may take multiple cycles per access; `abstractcs.busy` spans
  the whole transaction. Whether the port is req+done, ready/valid, or
  single-cycle is a free implementation choice.

### dbg_csr — the CSR debug port

Bundle: `req`, `addr[11:0]`, `we`, `wdata[31:0]`, `rdata[31:0]`, `done`.

Same abstract-command path for **Access Register** with `regno`
0x000–0xfff (`regno` = the CSR address). Implementation-wise this is a
mux in front of the CSR file's existing read/write datapath — not a
second CSR file.

Handling rules:

- Must reach at least `dcsr` (0x7b0) and `dpc` (0x7b1); routing *all*
  implemented CSRs here is nearly free and is what 0.13.2 conformance
  option (b) — "may access any implemented CSR" — buys you.
- A CSR the hart does not implement answers `cmderr=3` (exception);
  that includes the omitted `dscratch0/1` in the minimal profile, which
  correctly tells the debugger they don't exist.
- The debugger's write path into `dcsr.ebreakm` (software breakpoints)
  and `dcsr.step` (single stepping) is exactly this port; the hart-side
  meaning of those bits is in
  [debug-mode.md](debug-mode.md).

### dbg_mem — the memory debug port

Bundle: `req`, `we`, `size[1:0]`, `addr[31:0]`, `wdata[31:0]`,
`rdata[31:0]`, `done`, `bus_err`, `exc`.

Driven for **Access Memory** (`cmdtype=2`): one load (`write=0`) or store
(`write=1`) pushed through the **hart's own LSU** while the hart is
halted (and its bus interface therefore idle):

- `addr` ← `data1`, data ↔ `data0`; `size` ← `aamsize` (16- or 32-bit;
  8-bit optional — 16-bit is what software breakpoints in compressed code
  need).
- Riding the hart's own LSU is the feature: the access sees exactly what
  M-mode code would see — PMP/MPU, memory attributes, MMIO, R/W regions —
  at no cost of a second bus master. (The alternative, System Bus
  Access, is a DM↔fabric master and not part of this interface at all.)
- `aampostincrement`, if implemented, increments `data1` in the DM after
  each successful access — purely DM-side bookkeeping, the hart sees
  nothing.
- Failure mapping: a **bus error** (bad address, access fault) raises
  `bus_err` → `cmderr=5`; an access that raises an architecturally
  defined exception raises `exc` → `cmderr=3`.

## Hart-to-DM signals

| Signal | Feeds | Kind | Meaning |
|---|---|---|---|
| `halted` | `dmstatus` bits 9:8 (`all/anyhalted`) | level | the hart is in Debug Mode |
| `running` | `dmstatus` bits 11:10 (`all/anyrunning`) | level | the hart is executing normally (= `!halted`) |
| `resumeack` | `dmstatus` bits 17:16 (`all/anyresumeack`) | edge-set flop | the hart left halted since the last `resumereq` write |
| `unavail` | `dmstatus` bits 13:12 (`all/anyunavail`) | level | hart unavailable (in reset, powered down) — optional, 0 |
| `havereset` | `dmstatus` bits 19:18 (`all/anyhavereset`) | sticky | hart came out of reset, not yet acknowledged — optional pair |

### halted and running

Two status outputs, mirror images of each other on a single hart.

- `halted` asserts when the hart enters Debug Mode and stays asserted
  until resume; this is what the debugger polls (`dmstatus.allhalted`)
  after a halt request. A halted single hart reads `dmstatus` as
  `0x0000_0382` (`allhalted | anyhalted | authenticated | version=2`).
- `running` = `!halted` here; `dmstatus` requires both pairs anyway.
- Status is level, not edge: the debugger polls, and the DM samples —
  nothing is pushed. Transition *causes* are not on these wires; see
  [Where the halt reason lives](#where-the-halt-reason-lives).

### resumeack

**Resume acknowledgment.** A flop in the DM, set by the hart's
"left halted" edge, cleared by the `resumereq` write itself.

Why it must exist: "not halted" is ambiguous. After a resume the hart can
re-halt instantly — an `ebreak` at `dpc`, a trigger, a still-held
`halt_req` — so polling `allhalted` can never *prove* resumption.
The debugger instead polls `dmstatus.allresumeack` after each resume;
without it OpenOCD's resume times out (see "Required protocol behaviors"
in [minimal-dm.md](minimal-dm.md)).

### unavail and havereset (optional)

- `unavail` (`dmstatus` 13:12): hart unavailable — in reset or powered
  down. Minimal profile: read 0; it *may* be asserted during `ndmreset`.
- `havereset` (`dmstatus` 19:18): **sticky** — set when the hart comes
  out of reset (whatever the cause), cleared only by a write to
  `dmcontrol.ackhavereset` (bit 28, W1). The status bits and the
  acknowledge bit form a coupled pair: implement both or neither —
  without the acknowledge the bits could never clear, and the debugger
  would see "this hart was reset" forever. OpenOCD's examine path
  acknowledges it (`dmactive | ackhavereset`) whenever it sees it set.

### Where the halt reason lives

The *reason* for a halt is not a wire — it is exchanged through
`dcsr.cause`, which the debugger reads over the `dbg_csr` port after
halting. Hardware writes it at entry; software and debugger alike only
read it:

| `dcsr.cause` | Meaning | Produced by |
|---|---|---|
| 1 | `ebreak` re-routed per `dcsr.ebreakm/s/u` | software breakpoints |
| 2 | trigger fired with action=1 | (trigger module — not in the minimal profile) |
| 3 | halt request | `halt_req` (this interface) |
| 4 | single step completed | `dcsr.step` + resume |
| 5 | halt on leaving reset | `resethaltreq` (not in the minimal profile) |

## How abstract commands drive the data ports

The three data ports exist only because of abstract commands — the
`command` register write is what starts a port transaction. The two
command shapes of the minimal DM and their port use:

**Access Register (`cmdtype=0`)** — one transaction on `dbg_gpr_*` or
`dbg_csr_*`:

| Field | Value | Effect on the port |
|---|---|---|
| `transfer` (bit 17) | 0 | no port access at all — a no-op command validation |
| `transfer` | 1 | perform the access |
| `write` (bit 16) | 0 | read the port, result → `data0` |
| `write` | 1 | drive `data0` onto the port with `we=1` |
| `regno` (15:0) | 0x1000–0x101f | GPR `x0`–`x31` → `dbg_gpr_*` |
| `regno` | 0x000–0xfff | CSR at that address → `dbg_csr_*` |

Example — "read GPR x10" is `command ← 0x0022_100A` (cmdtype=0,
`aarsize=2` for RV32, `transfer=1`, `write=0`, `regno=0x100a`), then poll
`abstractcs.busy`, then read `data0`.

**Access Memory (`cmdtype=2`)** — one transaction on `dbg_mem_*`:

| Field | Value | Effect on the port |
|---|---|---|
| `aamsize` (22:20) | 1 or 2 | 16- or 32-bit access size on `size[1:0]` |
| `write` (bit 16) | 0 / 1 | load into `data0` / store from `data0` |
| `aamvirtual` (bit 23) | 0 only | physical address (no MMU translation) |
| `aampostincrement` (bit 19) | 0 (1 optional) | DM increments `data1` after success |

Arguments: address in `data1`, data in `data0` — which is why the minimal
profile needs `datacount=2` (`data0` + `data1`) while register access
alone would need only `data0`.

**Error reporting** — the port's `done`/`bus_err`/`exc` answers fold
into `abstractcs.cmderr` (W1C; while nonzero, further `command` writes
are ignored):

| Condition during the port transaction | `cmderr` |
|---|---|
| hart not halted when the command starts | 4 (halt/resume) |
| bus error on `dbg_mem_*` (bad address, access fault) | 5 (bus) |
| architecturally defined exception on the access | 3 (exception) |
| requested register/CSR does not exist | 3 (exception) |
| command/option not implemented (e.g. `aamsize=3/4`) | 2 (not supported) |
| `command`/data registers touched mid-command | 1 (busy) |

## Handling rules

The cross-signal rules, in one place — the parts implementations get
wrong:

1. **Halt is a level, resume is an edge.** Held `halt_req` means "stay
   halted"; the DM gates `resume_req` off entirely while it is set, so a
   held halt level can never be escaped by resuming.
2. **Entry writes state, exit reads it.** Halt entry writes
   `dcsr.prv`/`dcsr.cause`/`dpc`; resume reads them back
   (`pc ← dpc`, privilege ← `dcsr.prv`). The hart itself never fetches
   anything in either direction (quiesced Debug Mode).
3. **The debugger is a polling loop.** Poll `dmstatus.allhalted` after
   halt, `abstractcs.busy` after each command, `dmstatus.allresumeack`
   after resume. Nothing is ever pushed over this interface.
4. **Abstract commands run on a halted, stable hart**, with
   `haltreq`/`resumereq`/`ackhavereset` all 0 while `abstractcs.busy`;
   the debugger obeys, and the DM's command FSM only services the ports
   on a halted hart anyway.
5. **`resumeack` protocol**: the flop is set by the hart's "left halted"
   edge and cleared by the `resumereq` write itself — so a polled 1
   always means "resumed since the last resume request".
6. **Clock domains are free**: all lines are plain handshake signals;
   if DM and hart are in different domains, synchronize them exactly as
   the DMI is synchronized ([jtag-dtm.md](jtag-dtm.md)).
7. **Port handshakes may be multi-cycle**; `abstractcs.busy` spans the
   whole transaction. req+done vs. ready/valid is an implementation
   choice.
8. **Single-step is sticky**: `dcsr.step` stays set across the re-entry
   halt (`cause=4`) until the debugger clears it over `dbg_csr`; it must
   be cleared before a normal resume. With `dcsr.stepie=0` (minimal
   profile) interrupts stay pending across the step.

## Halt and resume handshake timeline

```
debugger                DM                          hart
────────────────────────────────────────────────────────────────
haltreq=1   ─────────► halt_req = 1 ─────────────► (retire in-flight insn)
                            │                       dpc/dcsr.cause/prv ← …
poll dmstatus ◄────────  halted = 1 ◄────────────── declare halted
(allhalted=1)
haltreq=0   ─────────► halt_req = 0           (hart stays halted)
… abstract commands (spec: haltreq must be 0 before each one) …
resumereq=1 ─────────► resume_req pulse ──────────► pc ← dpc, prv ← dcsr.prv
   (also clears                                    leave halted, step? arm step
    resumeack)
poll dmstatus ◄──────── resumeack = 1 ◄─────────── "left halted" edge
(allresumeack=1)        halted = 0, running = 1
```

The asymmetries that matter: a halt request re-asserted right after a
resume (or an `ebreak` sitting at `dpc`) re-halts the hart instantly —
which is why the acknowledgment is a separate flop and not just
"not halted"; and clearing `haltreq` never resumes, it only releases.

## Reset interactions

Three distinct resets touch this interface — keep them apart:

| Reset | What it resets | Effect on debug state |
|---|---|---|
| TAP reset (TRST* or 5×TMS=1) | the TAP/DTM only | DM and hart keep all state; IR returns to IDCODE, and the PULP/OpenTitan DTM also clears its DMI state like `dmihardreset` ([jtag-dtm.md](jtag-dtm.md)) |
| `dmactive=0` | the DM only (soft reset of all DM flops) | `halt_req`/`resume_req`/`ndmreset` drop — a held halt request silently clears; the debugger re-issues it. This lever is the debugger's "unwedge the DM" tool |
| `ndmreset=1` | everything except DM/DTM — the whole SoC incl. the hart and its debug CSRs | a halted hart is reset and comes up **running** (unless `halt_req` is held across the pulse, so it halts right out of reset) |

`resethaltreq` itself is not implemented (`dmstatus.hasresethaltreq=0`,
`dmcontrol` bits 3:2 read 0): per the coupled-bits rule in
[minimal-dm.md](minimal-dm.md), capability and control bits appear
together or not at all.

## Deliberate omissions

Compared to a full-spec (ROM-based) DM, this interface deliberately
leaves out — see
[debug-mode.md](debug-mode.md) "What this interface deliberately
omits" for the reasoning:

- **Debug ROM / Program Buffer interface** — nothing is ever fetched in
  Debug Mode: no `hartinfo`/`nscratch`, no `dscratch` CSRs, no `impebreak`
  (`progbufsize=0`), no "execute code while halted" machinery.
- **Trigger module** — no `tselect`/`tdata*`/`tinfo`; software `ebreak`
  breakpoints are the only breakpoints (hardware triggers are the first
  "what to add next" item in [minimal-dm.md](minimal-dm.md)).
- **Hart array** — single hart, `hartsel` hardwired 0, no `hawindow`,
  `haltsum*` unused.
- **`dret` as an executed instruction** — never runs in this profile; the
  hardware resume path reproduces its architectural effect
  (`pc ← dpc`, privilege ← `dcsr.prv`). Either leave `dret` unimplemented
  (illegal instruction — its correct behavior outside Debug Mode) or
  implement it and leave it unreachable.
- **System Bus Access** — SBA would be a DM↔fabric bus master, not part
  of the DM↔hart interface at all.

## Related documents

| Document | What it covers relative to this one |
|---|---|
| [debug-mode.md](debug-mode.md) | The execution-state side of the same wires: Debug Mode entry/exit, `dcsr`/`dpc`, ebreak re-routing, single stepping |
| [minimal-dm.md](minimal-dm.md) | The DM registers every signal derives from: bit maps, coupled bits, protocol behaviors, OpenOCD compatibility |
| [debug-sequences.md](debug-sequences.md) | The debugger workflows that drive these wires, as concrete DMI operation listings |
| [jtag-dtm.md](jtag-dtm.md) | The transport underneath: TAP FSM, `dtmcs`/`dmi` scan registers, op protocol, timing, clock-domain crossing |
| [debug-architecture.md](debug-architecture.md) | The full layered picture: JTAG TAP → DTM → DMI → DM → hart debug logic, all DM registers, 0.13.2 vs 1.0 |
| [`src/mock/mock_target.hpp`](../src/mock/mock_target.hpp) | Behavioral model of the whole chain; [`src/mock/mock_target.cpp`](../src/mock/mock_target.cpp) implements these lines in software |