# Reference C++ Implementation of the Debug Sequences

This document explains the C++ implementation of the debugger workflows
under `src/`: how the code is layered, how each sequence from
[debug-sequences.md](debug-sequences.md) maps to a method, which design
decisions were made where the doc offers choices, and how to run the
self-checking demo against the behavioral mock target.

The code is a dependency-free C++17 implementation — not firmware, but
host-side debugger/driver code in the role OpenOCD's `riscv-013.c`
plays. All register values, command words, bit masks, and sequences are
taken verbatim from [debug-sequences.md](debug-sequences.md) (with the
transport protocol details from [jtag-dtm.md](jtag-dtm.md) and the
register set from [minimal-dm.md](minimal-dm.md)); nothing was
re-invented. Every method carries a comment naming the section of
[debug-sequences.md](debug-sequences.md) it implements.

For the surrounding architecture see
[debug-architecture.md](debug-architecture.md).

## Table of contents

- [Layering](#layering)
- [Files](#files)
- [Sequence-to-method map](#sequence-to-method-map)
- [Design decisions](#design-decisions)
- [The mock target](#the-mock-target)
- [Building and running](#building-and-running)
- [References](#references)

## Layering

The stack mirrors the layering of the documents — each class calls only
into the layer below it:

```
JtagTransport (abstract: jtag_reset, jtag_ir, jtag_dr)   ← you wire this to
        │                                                  your adapter /
        │                                                  bit-banger / sim
JtagDtm  — dmi_write / dmi_read / dmi_poll
           · the two-scan dmi op protocol (request + result)
           · sticky busy/failed handling, dtmcs.dmireset,
             Run-Test/Idle dwell ratcheting (spec B.1)
        │
DebugSequences — connect, halt, read halt reason, GPR/CSR access,
           memory access (word / block loop / postinc / abstractauto
           burst), resume, single step, software breakpoints,
           reset-and-halt, attach/detach, error recovery
```

`JtagDtm` provides exactly the three operations the sequence listings
are written in:

| Code | [debug-sequences.md](debug-sequences.md) notation |
|---|---|
| `dmi_write(addr, data)` | `dmi write <addr> <- <value>` |
| `dmi_read(addr)` | `dmi read <addr> -> <value>` |
| `dmi_poll(addr, mask, expected)` | `poll <addr> until <cond>` |

One DMI operation costs two `dmi` JTAG scans (request + result). This
driver keeps the simple one-op-at-a-time shape; real debuggers pipeline
requests into the result scan (see "Pipelined burst" in
[jtag-dtm.md](jtag-dtm.md)) — a possible optimization, but not needed
for correctness.

## Files

| File | Role |
|---|---|
| [`src/jtag_transport.hpp`](../src/jtag_transport.hpp) | Pure abstract transport: `jtag_reset()`, `jtag_ir()`, `jtag_dr()` with LSB-first bit packing and a Run-Test/Idle dwell parameter. IR/DR encoding constants live here. |
| [`src/dtm.hpp`](../src/dtm.hpp), [`src/dtm.cpp`](../src/dtm.cpp) | `JtagDtm`: IDCODE / `dtmcs` / `dmi` scans, the sticky `op` protocol, busy recovery (`dmireset` + dwell ratchet + retry), `PollTimeout` / `DmiBusyError` / `DmiFailedError`. |
| [`src/sequences.hpp`](../src/sequences.hpp), [`src/sequences.cpp`](../src/sequences.cpp) | `DebugSequences`: all workflows, plus the named constants (DMI addresses, bit masks, command words, `ebreak`/`c.ebreak` encodings) spelled out in [debug-sequences.md](debug-sequences.md). |
| [`src/mock/mock_target.hpp`](../src/mock/mock_target.hpp), [`src/mock/mock_target.cpp`](../src/mock/mock_target.cpp) | Behavioral mock target (see [below](#the-mock-target)). |
| [`src/demo.cpp`](../src/demo.cpp) | Self-checking demo running every sequence against the mock. |
| [`Makefile`](../Makefile) | `make` / `make test` / `make clean`; artifacts land in `build/`. |

## Sequence-to-method map

Every section of [debug-sequences.md](debug-sequences.md) maps to one
`DebugSequences` method (or a small group):

| Section of [debug-sequences.md](debug-sequences.md) | Method(s) |
|---|---|
| Connect and discovery | `connect()` (TAP reset, IDCODE, `dtmcs`, `dmactive=1`, `dmstatus` sanity, `abstractcs`/`hartinfo`/`sbcs` probes, the `0x07FF_FFC1` hartsel feature probe) |
| Halting the hart | `halt()`, `is_halted()`, `wait_halted()` |
| Reading the halt reason and core state | `read_halt_info()` (returns `dcsr`, `dpc`, decoded `cause`) |
| Accessing CPU registers (GPRs) | `read_gpr()` / `write_gpr()` / `dump_gprs()` on the raw `read_reg()` / `write_reg()` |
| Accessing CSRs | `read_csr()` / `write_csr()`, plus `try_read_csr()` for the OpenOCD-style "cmderr=3 → nonexistent" probing |
| Memory access | `read_mem()` / `write_mem()` (byte/half/word via `MemSize`) |
| Sequential memory read/write | `read_mem_block()` / `write_mem_block()` — the explicit per-word loop |
| Auto-increment bursts (intermediate) | `read_mem_block_postinc()` / `write_mem_block_postinc()` (`aampostincrement` only) |
| Auto-increment bursts (full) | `read_mem_burst()` / `write_mem_burst()` (`aampostincrement` + `abstractauto`), probed by `probe_burst_extension()` |
| Resuming | `resume()` |
| Single stepping | `step()`, `set_step()` |
| Software breakpoints | `install_breakpoint()`, `remove_breakpoint()`, `remove_all_breakpoints()`, `find_breakpoint()`, `continue_past_breakpoint()` |
| Reset and debug from reset | `reset_and_halt()` (returns `dpc` = reset vector) |
| Detaching | `attach()` (connect + halt + probes + `dcsr` save + `ebreakm`), `detach()` |
| Error recovery | layers 1–5, see [below](#design-decisions) |

## Design decisions

Where [debug-sequences.md](debug-sequences.md) leaves a choice open, the
code picks one and documents it in a comment:

**`halt()` clears `haltreq` after the halt.** The doc allows haltreq to
stay 1 after the halt, but requires it to be 0 before abstract commands
and before resume (`resumereq` is ignored while `haltreq` is 1). Writing
plain `dmactive` (= `0x0000_0001`) at the end of `halt()` — and of
`reset_and_halt()` — keeps that invariant without the caller having to
think about it.

**`step()` clears `dcsr.step` before returning.** The doc clears step
before the next plain resume "or the hart will single-step forever".
Folding that into `step()` means `resume()` after a step can never
single-step unintentionally; `continue_past_breakpoint()` relies on it.

**Exceptions, not error codes.** Host-side debugger code: `dmi_poll`
throws `PollTimeout`, failed abstract commands throw `CmderrError`
(carrying the decoded code), a wedged DMI throws `DmiBusyError`, and
`halt()`/`reset_and_halt()` throw `HaltTimeout`. All derive from
`DebugError`.

**`cmderr` is auto-cleared before throwing.** While `cmderr != 0`,
writes to `command` are ignored — an uncleared error would poison every
later operation. So the poll helper clears it (W1C `0x700`) *and* throws
`CmderrError`; the object stays usable and the caller can "fix the cause
and re-issue" exactly as the doc's error-recovery layer 1 says.

**Block transfers abort with partial data, not silently.** Per "Fault
handling mid-block": `cmderr` is polled every iteration; on a fault the
loop aborts, the partial words stay in the caller's buffer, the method
returns the number of words completed, and the code is available via
`last_cmderr()`. In the burst variants the sticky `cmderr` makes the
burst fizzle out on its own; one `abstractcs` poll at the end reveals the
failure, and `data1` (pointing at the faulting address) gives the
completed-word count.

**Breakpoints handle the compressed edge cases.** With the C extension,
a 16-bit patch (`c.ebreak`) is used for compressed instructions and a
32-bit patch (`ebreak`) for aligned 32-bit ones. For a 32-bit
instruction starting at `A % 4 == 2` (legal at 2-byte alignment with C),
the second halfword at `A+2` is saved too, so restore and
step-past reassemble the full instruction. A breakpoint address in the
*middle* of a 32-bit instruction is rejected.

**attach() is a real attach.** It saves `dcsr` (the control bits
restored on detach — never `dpc`, which is where the program resumes),
sets `dcsr.ebreakm` so patched `ebreak`s reach Debug Mode instead of the
debuggee's trap handler, reads `misa` for the C extension, and runs the
halted-only probes (XLEN via the rejected `aarsize=3` command, burst
extension). Detach walks the doc's five steps in order, including the
optional `dmactive=0` power-down.

**Error recovery is layered exactly like the doc.**
Layer 1 (`cmderr`) → auto-clear + `CmderrError`, `clear_cmderr()` public.
Layer 2 (DMI busy) → handled inside `JtagDtm`: `dmireset`, ratchet the
dwell, repeat the request. Layer 3 (DMI failed) → sticky cleared, then
`DmiFailedError` for the caller to replay; `JtagDtm::dmi_hard_reset()`
exposes `dmihardreset` for a stuck DTM. Layer 4 (hart never halts) →
`halt(recover_on_timeout=true)` checks `anynonexistent`/`allunavail` and
falls back to the reset-and-halt flow. Layer 5 (everything wedged) →
`recover_dm()`: the `dmactive` 0→1 pulse, then re-verify `dmstatus`.

## The mock target

`MockTarget` implements `JtagTransport` and simulates everything below it
— it is the "test vector" target the README mentions, built strictly from
[jtag-dtm.md](jtag-dtm.md) and [minimal-dm.md](minimal-dm.md):

- **DTM side** ([jtag-dtm.md](jtag-dtm.md)): IDCODE / BYPASS / `dtmcs` /
  `dmi` registers; the capture→shift→update semantics per scan; the
  sticky `op` protocol; the busy window rule "result must arrive within
  `2 + idle` TCKs" (so a too-small dwell *really* produces busy);
  `dmireset` / `dmihardreset`; TAP reset behaves like `dmihardreset`
  (the PULP/OpenTitan recommendation).
- **DM side** ([minimal-dm.md](minimal-dm.md)): `data0`/`data1`,
  `dmcontrol` (including the hartsel/hasel read-0 probe behavior),
  `dmstatus` with `allresumeack`, `abstractcs` with W1C `cmderr`,
  `command` with the Access Register and Access Memory rejection rules
  (`cmderr` 2/3/4/5, `aarsize`/`aamsize` checks, halted-hart gating,
  `aampostincrement`, `abstractauto` auto-execution), and
  `dmactive=0` resetting all DM state. Unimplemented addresses read 0,
  writes ignored.
- **Hart side** ([debug-mode.md](debug-mode.md)): GPR file (x0
  hardwired), `dcsr`/`dpc`/`misa`/`mepc`/`mcause`/`mtval` CSRs,
  halt/resume with `resumeack`, single step (`cause=4`), `ebreak` /
  `c.ebreak` halting with `cause=1` per `dcsr.ebreakm`, `ndmreset` to a
  reset vector with halt-out-of-reset when `haltreq` is held.

Two knobs make the error-recovery paths testable without hardware:
`set_dmi_latency()` (a DMI response slower than `2 + idle` forces the
busy/ratchet path) and `fail_next_dmi_op()` (the next completed op
reports `op=2`, the DMI-failed path). Reads of DMI state evaluate the DM
at Update-DR instead of Capture-DR — irrelevant here, because the mock
hart has no asynchronous activity between the two.

Documented simplifications (they cannot change what the sequences do):
abstract commands complete synchronously, so `abstractcs.busy` is never
observable; the "running" hart is modeled as an instant forward scan for
the next `ebreak`, not as real instruction execution (no branches, no
side effects); `misa` is hardwired to RV32IMC.

## Building and running

```console
$ make test
...
ALL CHECKS PASSED (0 failed checks)
```

`make` builds `build/rvdebug-demo`, `make test` runs it. The demo
(`src/demo.cpp`) exercises every sequence against the mock and checks
the results: discovery and probes, GPR/CSR access (including the
"nonexistent CSR" path), all four block-transfer variants, a
mid-block bus fault (partial data, `cmderr=5`), single stepping, the
full software-breakpoint lifecycle (install 32-bit and 16-bit patches,
hit, continue-past via restore-step-repatch-resume, remove), the
`ndmreset` halt-out-of-reset flow, and all error-recovery layers
including the busy ratchet and the `dmactive` unwedge.

To drive real hardware, implement the three pure-virtual methods of
`JtagTransport` in [`src/jtag_transport.hpp`](../src/jtag_transport.hpp)
against your adapter and construct `JtagDtm` on it — nothing above that
layer changes.

## References

- Implemented workflows: [debug-sequences.md](debug-sequences.md)
- Transport protocol underneath: [jtag-dtm.md](jtag-dtm.md)
- Register set driven: [minimal-dm.md](minimal-dm.md)
- Hart-side contract of the mock: [debug-mode.md](debug-mode.md)
- Architecture overview: [debug-architecture.md](debug-architecture.md)
