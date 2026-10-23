# RISC-V RV32 External Debug: Structure and Functionality

This document describes the layered architecture of RISC-V external debug
support for RV32 systems: the JTAG Test Access Port (TAP), the Debug Transport
Module (DTM), the Debug Module Interface (DMI), the Debug Module (DM), and the
hart-side debug logic (Debug Mode, debug CSRs, trigger module).

A practical guide to the *smallest* register set worth implementing in a Debug
Module is in [minimal-dm.md](minimal-dm.md).

References used: the RISC-V External Debug Support specification,
[version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
(what virtually all deployed silicon implements) and the ratified
[version 1.0](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf).
Register-level details below were verified against those documents.

## Table of contents

- [The big picture — a 5-layer stack](#the-big-picture--a-5-layer-stack)
- [1. JTAG TAP — the physical transport](#1-jtag-tap--the-physical-transport)
- [2. DTM — turning JTAG scans into DMI ops](#2-dtm--turning-jtag-scans-into-dmi-ops)
- [3. DMI — Debug Module Interface](#3-dmi--debug-module-interface)
- [4. DM — the Debug Module](#4-dm--the-debug-module)
- [5. Hart-side debug logic](#5-hart-side-debug-logic)
- [6. Putting it together — a typical debug session](#6-putting-it-together--a-typical-debug-session)
- [7. RV32-specific notes](#7-rv32-specific-notes)
- [8. Versions in the field: 0.13.2 vs 1.0](#8-versions-in-the-field-0132-vs-10)

## The big picture — a 5-layer stack

The RISC-V debug architecture is deliberately layered so that the transport
(JTAG), the reusable debug hardware (Debug Module), and the core-specific
parts (hart debug logic) can be developed independently:

```
        HOST (PC)                                TARGET SoC
┌──────────────────────┐
│ GDB / other client   │
│      ↕ RSP, TCP      │
│ Debug translator     │
│ (e.g. OpenOCD)       │
│      ↕ USB / Ethernet│
│ Debug probe          │
│ (J-Link, Olimex, …)  │
└──────────┬───────────┘
           │  TCK, TMS, TDI, TDO  (IEEE 1149.1)
┌──────────▼─────────────────────────────────────────────────┐
│  DTM  ── Debug Transport Module                            │
│   └─ JTAG TAP: 16-state FSM, IR + DR scan registers       │
│          ↕  DMI (Debug Module Interface): {op, addr, data} │
│  DM   ── Debug Module (transport-independent)              │
│   ├─ run control: halt/resume/reset per hart              │
│   ├─ abstract command engine + data0…11, progbuf0…15       │
│   └─ optional System Bus Access master (sbcs/sbaddr/sbdata)│
│          ↕  halt request, resume request, reset lines     │
│  Hart ── RV32 core debug logic (per core)                  │
│   ├─ Debug Mode execution, dret                            │
│   ├─ dcsr / dpc / dscratch0-1 CSRs                         │
│   └─ trigger module (hw breakpoints / watchpoints)          │
└─────────────────────────────────────────────────────────────┘
```

| Layer | Component | Responsibility |
|---|---|---|
| 1 | **JTAG TAP** (IEEE 1149.1) | Dumb generic transport: shift bits in/out of selected registers |
| 2 | **DTM** (Debug Transport Module) | Interpret JTAG scans; translate them into DMI reads/writes |
| 3 | **DMI** (Debug Module Interface) | Minimal bus: one master (DTM) → one or more slaves (DM), `{op, addr, data}` |
| 4 | **DM** (Debug Module) | The reusable "debug brain": halt/resume/reset control, register & memory access services |
| 5 | **Hart debug logic** | Core-side Debug Mode, debug CSRs, single-step, ebreak behavior, triggers |

Note the clean split: everything above DMI is transport (the spec also permits
other DTM types), the DM is standard reusable IP, and everything the DM needs
from the core (halt entry, Debug Mode, `dret`) lives inside the hart.

## 1. JTAG TAP — the physical transport

Plain IEEE 1149.1: a 16-state synchronous FSM clocked by TCK, navigated by
TMS, with a chain of scan registers (DR) selected by an instruction register
(IR). RISC-V requires **IR ≥ 5 bits**; on TAP reset the IR defaults to
`00001` (IDCODE). The debug-relevant instructions:

| IR | Register | Purpose |
|---|---|---|
| 0x00, 0x1f | BYPASS | 1-bit dummy (also for unimplemented IRs) |
| 0x01 | IDCODE | 32-bit JEDEC ID — how the probe discovers the core |
| 0x10 | dtmcs | DTM control/status — configure/clear the DTM |
| 0x11 | dmi | The actual window into the Debug Module |

A JTAG "transaction" is always the same: walk to Shift-IR and scan the
instruction, then Shift-DR the selected register in/out (LSB first),
Update-DR latches it. The JTAG TAP itself knows nothing about RISC-V — it's
a generic bit-pipe.

## 2. DTM — turning JTAG scans into DMI ops

The DTM (Chapter 6 of the debug spec, "JTAG DTM") owns the TAP and
implements two debug registers. A detailed, implementation-ready guide for
this layer — TAP state machine, scan bit order, sticky/busy protocol,
timing, and DMI/CDC — is in [jtag-dtm.md](jtag-dtm.md).

### dtmcs (IR 0x10) — control/status

| Field (0.13.2 / 1.0) | Meaning |
|---|---|
| `version [3:0]` | Spec version. `1` = 0.13 family **and also 1.0** (unchanged interface) |
| `abits [9:4]` | Width of the DMI address field |
| `idle [14:12]` | Hint: TCK cycles to spend in Run-Test/Idle between DMI ops to avoid "busy" |
| `dmistat [11:10]` | 0 = OK, 2 = op failed, 3 = busy (both sticky) |
| `dmireset` (bit 16) | W1: clears sticky error state |
| `dmihardreset` (bit 17) | W1: hard reset of the DTM, aborts outstanding DMI ops |

(The ratified 1.0 spec adds an optional `errinfo [20:18]` field with more
detail on DMI errors.)

### dmi (IR 0x11) — the DMI window

A scan register of `abits + 34` bits (op at the LSBs, so it is scanned
first; address is scanned last):

```
[abits+33:34]  address      (abits wide)
[33:2]         data         (32 bits)
[1:0]          op           (2 bits)
```

The protocol is simple — on the way **in** (TDI) `op` is the request, on the
way **out** (TDO) `op` is the response of the *previous* transaction:

- Request: `0` = nop, `1` = read `address`, `2` = write `data` to `address`,
  `3` = reserved
- Response: `0` = success, `2` = failed (sticky), `3` = still busy (sticky —
  more TCK cycles needed)

Because the response comes back on the *next* scan, each DMI op costs two
JTAG scans (debuggers pipeline these and batch a whole sequence of scans,
checking `op`/`dmistat` at the end). The sticky-error + `dmireset` mechanism
makes error recovery deterministic.

## 3. DMI — Debug Module Interface

The DMI is just a small read/write bus between DTM and DM, with **7–32
address bits**. It can be literally a point-to-point register handshake in
RTL, or something fancier (the spec even suggests TileLink or APB). The
bottom of the address space is the first (usually only) DM; if several DMs
hang off one DMI, each advertises the next one's base in `nextdm`. In
practice: DMI = the set of "DM registers" listed below, addressed by the
`address` field of the dmi scan register.

Unimplemented DMI addresses read 0 and ignore writes.

## 4. DM — the Debug Module

This is the reusable hardware block that actually implements debugging. What
it must do (per the spec): report info, halt/resume harts, report halt
status, abstract-read/write **halted GPRs**, and provide reset control.
Optional but common: Program Buffer, System Bus Access, non-GPR access,
multi-hart operations.

A DM can control up to 2^20 harts.

### DM register map (identical in 0.13.2 and 1.0)

| DMI addr | Register | Function |
|---|---|---|
| 0x04–0x0f | data0…data11 | Argument registers for abstract commands (RV32: `data0` for register access, plus `data1` as the Access Memory address) |
| 0x10 | dmcontrol | Run/reset control (see below) |
| 0x11 | dmstatus | Status: running/halted/resumed/nonexistent/unavailable/havereset, `version` |
| 0x12 | hartinfo | What the hart's debug ROM offers (nscratch, dataaccess, dataaddr) |
| 0x13 | haltsum1 | Coarse halted-hart bitmap (systems > 32 harts) |
| 0x14 / 0x15 | hawindowsel / hawindow | Window onto the hart-array mask (bulk multi-hart selection) |
| 0x16 | abstractcs | `busy`, sticky `cmderr`, `progbufsize`, `datacount` |
| 0x17 | command | Writing this triggers an abstract command |
| 0x18 | abstractauto | autoexec bits: accessing data/progbuf re-runs last command (burst optimization) |
| 0x19–0x1c | confstrptr0–3 | Pointer to configuration string (SoC description) |
| 0x1d | nextdm | Base of next DM on this DMI, 0 if last |
| 0x20–0x2f | progbuf0…15 | 16 × 32-bit words of instructions the hart executes in Debug Mode |
| 0x30 | authdata | Optional challenge/response authentication of the DM |
| 0x34 / 0x35 | haltsum2 / haltsum3 | Even coarser halt bitmaps (huge systems) |
| 0x37 | sbaddress3 | System-bus address bits 127:96 |
| 0x38 | sbcs | System Bus Access control/status |
| 0x39–0x3b | sbaddress0–2 | System-bus address (low 96 bits) |
| 0x3c–0x3f | sbdata0–3 | System-bus data (up to 128 bits) |
| 0x40 | haltsum0 | 32-bit halted bitmap of 32 harts around hartsel |

### dmcontrol (0x10) — the master control knob

```
[31] haltreq      [30] resumereq    [29] hartreset    [28] ackhavereset
[26] hasel        [25:16] hartsello ┐
[15:6]  hartselhi  ┘ = hartsel (≤ 20 bits → up to 2^20 harts)
[3] setresethaltreq  [2] clrresethaltreq  [1] ndmreset  [0] dmactive
```

- `dmactive=0` resets the whole DM (soft reset of debug state); written 1 to
  enable it
- `haltreq` halts the selected hart(s); `resumereq` resumes them (they
  acknowledge via `resumeack`)
- `hartsel` picks the hart abstract commands act on; with `hasel` +
  `hawindow` you can act on 32 harts at once
- `hartreset` resets just the selected hart(s); `ndmreset` (**non-debug-module
  reset**) resets *everything except the DM/DTM* — this is how you debug from
  the very first instruction after reset
- `resethaltreq` (set/clear bits, plus `hasresethaltreq` in dmstatus): make a
  hart halt immediately out of reset

Only the actions initiated by `dmcontrol` can apply to multiple harts at
once; abstract commands always apply to the single hart selected by
`hartsel`.

### Hart states (as seen through dmstatus)

Each selectable hart is in exactly one of: **running** / **halted** (= in
Debug Mode) / **unavailable** (reset, powered down, not yet known) /
**nonexistent**. Status reads give `allX`/`anyX` pairs for the current
selection, so the debugger polls `allhalted` after a halt request and
`allresumeack` after resume. `havereset` is sticky until `ackhavereset`.

### Abstract commands — reading/writing hart state

An abstract command = write a command word to `command` (0x17), wait for
`abstractcs.busy` to clear, get data back in the `data` registers. The key
command is **Access Register** (`cmdtype=0`):

```
[23]=0   [22:20] aarsize (2 = 32-bit → RV32)   [19] aarpostincrement
[18] postexec (run progbuf after)   [17] transfer   [16] write   [15:0] regno
```

`regno` space: `0x0000–0x0fff` CSRs, `0x1000–0x101f` GPRs (x0…x31),
`0x1020–0x103f` FPRs, `0xc000+` custom. So "read x10 of a halted hart" =
write `command` with `transfer=1, write=0, regno=0x100a`, wait for busy to
clear, then read `data0` (for a register write, put the value in `data0`
first and set `write=1`). `postexec` chains in a Program Buffer
execution. Errors are reported in the sticky `abstractcs.cmderr` (busy /
not-supported / exception / halt-resume / bus / other, write-1-to-clear).

The other commands: **Quick Access** (halt → run progbuf → resume,
optional) and **Access Memory** (load/store with the hart's own view &
permissions, optional). Argument usage per width (32-bit): arg0 = `data0`,
arg1 = `data1`.

### Program Buffer — executing arbitrary code on the halted hart

`progbuf0–15` hold up to 16 words of instructions (compressed, i.e. 16-bit
`c.*` encodings). When an abstract command sets `postexec` (or the debugger
uses Quick Access), the halted hart executes this code **in Debug Mode at
M-mode privilege**: all interrupts masked, exceptions end the program without
touching CSRs, `wfi` is a nop, branches/`mret` are illegal. This is the
universal fallback for doing anything not covered by abstract commands —
classically memory access:

```
progbuf:  c.lw x11, 0(x10)     ; x10 = address put in data0 / dscratch
          ebreak
```

The program must end with `ebreak`/`c.ebreak`; an implementation may place an
implicit `ebreak` after the buffer (`impebreak` in dmstatus), which makes a
one-instruction buffer usable.

### System Bus Access (SBA) — memory access without the hart

If implemented, the DM contains a real bus master (e.g. an AXI/APB master in
the SoC fabric). The debugger writes `sbaddress0–3`, then `sbdata0–3` (or
uses `sbreadonaddr`), with `sbaccess` selecting 8/16/32/…-bit accesses.
`sbcs` reports `sbbusy`, sticky `sberror`, supported widths. Big advantage:
works **while the hart is running** — this is what flash loaders use.

## 5. Hart-side debug logic

This is what the *core* (not the DM) must implement:

### Debug Mode

A special execution state entered on: halt request, trigger with
action=debug, `ebreak` (if enabled for the current privilege), single-step
completion, or resethaltreq out of reset. While halted, the PC fetches from
the debug ROM (a small ROM, often inside/near the DM — this is what the
Program Buffer and the save/restore sequences are fetched through).

While executing Program Buffer code in Debug Mode, the hart runs at M-mode
privilege, interrupts are masked, exceptions end execution without updating
any CSRs, `wfi` is a nop, and most privilege-changing instructions act as
illegal instructions.

### Debug CSRs

Accessible only from Debug Mode, or via abstract commands:

| CSR | Name | Highlights |
|---|---|---|
| 0x7b0 | dcsr | `xdebugver`, `ebreakm/ebreaks/ebreaku` (does `ebreak` halt or trap per privilege), `step`, `cause` (1=ebreak, 2=trigger, 3=haltreq, 4=step, 5=resethaltreq), `prv` (privilege to resume into), `stopcount`, `stoptime`, `stepie`, `mprven`, `nmip` |
| 0x7b1 | dpc | Address to resume at (address of the ebreak for ebreak-halts, "next instruction" otherwise) |
| 0x7b2/0x7b3 | dscratch0/1 | Scratch registers used by the debug ROM (e.g. `hartinfo.nscratch`) |

### dret

`dret` (encoding `0x7b200073`) is the only way out of Debug Mode:
`pc ← dpc`, privilege ← `dcsr.prv`. Illegal anywhere else. The debugger
never executes `dret` itself; the DM's debug ROM does it when servicing
resume requests.

### Single step

Set `dcsr.step=1`, then resume: the hart executes exactly one instruction
and re-enters Debug Mode with `cause=4`. The classic GDB `stepi` loop.

### ebreak re-routing

With `ebreakm=1`, an `ebreak` in M-mode doesn't take the breakpoint
exception but enters Debug Mode instead (this is how software breakpoints
inserted by the debugger work: replace the instruction word with `ebreak`,
restore on resume).

### Trigger module

Hardware breakpoints/watchpoints, specified in the same debug spec (chapter 5
in 0.13.2; the "Sdtrig" extension in 1.0): `tselect` (0x7a0) picks a
trigger, `tdata1` (0x7a1) holds configuration (type-2 `mcontrol`: match
type, execute/load/store, `action` — action=1 = enter Debug Mode), `tdata2`
(0x7a2) holds the compare address, `tinfo` (0x7a4) reports supported types.
These are ordinary CSRs the debugger reaches *through* abstract commands
while halted. An address-match trigger on execute with action=1 = a hardware
breakpoint without touching memory.

## 6. Putting it together — a typical debug session

1. **Discovery**: TAP reset → read IDCODE; select `dtmcs` → learn `version`,
   `abits`, `idle`.
2. **Bring up DM**: write `dmcontrol.dmactive=1` (0=fully resets the DM),
   verify.
3. **Halt**: set `hartsel`, write `haltreq=1`, poll `dmstatus.allhalted`.
   (Optionally first: check `authenticated` — some DMs gate everything behind
   authdata.)
4. **Read state**: Access Register commands for dpc, dcsr (`cause` tells you
   *why* it halted), GPRs; more CSRs/FPRs as supported.
5. **Memory**: System Bus Access if present; else Program Buffer
   (`c.lw`/`c.sw` snippets); else Access Memory command.
6. **Breakpoints**: software `ebreak` patching (if the code region is
   writable) or triggers via `tselect`/`tdata1`/`tdata2`.
7. **Single-step**: `dcsr.step=1` → resume → hart re-halts with cause=4.
8. **Resume**: `resumereq=1`, poll `allresumeack`.
9. **Reset & debug from reset**: `ndmreset` for whole-SoC reset, or
   `hartreset` for one hart, or `resethaltreq` to come out of reset halted.
10. **Error recovery**: `abstractcs.cmderr` is sticky W1C; DMI-level errors
    clear via `dtmcs.dmireset`; a wedged DTM gets `dmihardreset`.

## 7. RV32-specific notes

- `aarsize=2` (32-bit) everywhere; register access needs only `data0`
  (`datacount ≥ 1`), while Access Memory additionally uses `data1` as
  the address; SBA needs only `sbdata0`/`sbaddress0`.
- Program Buffer instructions must be 16-bit (c.* encodings) — an RV32 hart
  in Debug Mode runs in DXLEN=32.
- On Sv32 systems physical addresses can be 34 bits — `mprven` in dcsr
  decides whether MPRV translation applies to debug-mode accesses, which
  matters for reading physical memory from the debug ROM.
- Single-hart RV32: `allX`/`anyX` pairs are degenerate (equal), `hartsel` is
  effectively 0, `haltsum*` unused.

## 8. Versions in the field: 0.13.2 vs 1.0

- **0.13.2 (2019)** is what virtually all deployed silicon and RTL
  implements today — SiFive cores, OpenTitan's `rv_dm`, PULP's `riscv-dm`
  (used by Ibex/CVA6 and many others). OpenOCD's `riscv` target driver
  speaks it (`dtmcs.version=1`, `dmstatus.version=2`).
- **1.0 (ratified 2024)** kept the DMI protocol, JTAG IR map, and every
  register address above; `dmstatus.version=3`, `dtmcs.version` stays 1
  (plus a new `errinfo` field), the content was restructured into named
  extensions (Sdext for the external-debug parts, Sdtrig for triggers), with
  richer triggers and clarifications. A 1.0 debugger is nearly a drop-in
  over 0.13 hardware for the core flow.
