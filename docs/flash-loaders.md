# Flash loaders: the code a debugger runs *outside* Debug Mode

Programming flash from a debugger needs **target-side code**: the flash
controller's erase/program sequences, status polling, and timing loops run
far too often and too tightly to be driven word-by-word over JTAG. So the
debugger downloads a small binary — a **flash loader** (OpenOCD calls it an
*algorithm*) — to target RAM and executes it on the hart.

The question this document answers up front: **flash loaders are not run in
Debug Mode.** They run as ordinary *resumed* code — fetched from RAM, at the
privilege mode saved in `dcsr.prv` (in practice M-mode on the embedded
targets that get flash-programmed), under completely normal rules: traps
vector to the target's own `mtvec`, PMP applies, timers run. The
M-privilege "special rules" context of [debug-rom.md](debug-rom.md) is
reserved for the debug ROM and the ≤16-word Program Buffer. Neither spec
version (0.13.2 nor 1.0) mentions flash loaders at all — the whole
mechanism is a debugger-side convention built out of spec primitives: halt,
abstract memory/register access, and resume-at-`dpc`.

References used: the RISC-V External Debug Support specification,
[version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
and
[version 1.0](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf)
(§3.7.1.2 Quick Access, §3.11 minimally intrusive debugging, §4.1 Debug Mode
execution), and the OpenOCD `riscv` target driver
([`src/target/riscv/riscv.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv.c),
`riscv_run_algorithm()`) with the SiFive/PULP SPI-flash driver
([`src/flash/nor/fespi.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/flash/nor/fespi.c))
and its loader
([`contrib/loaders/flash/fespi/riscv_fespi.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/contrib/loaders/flash/fespi/riscv_fespi.c)).
Behavior below was verified against those sources.

## Table of contents

- [Why programming flash needs target-side code](#why-programming-flash-needs-target-side-code)
- [The answer up front: resumed, not in Debug Mode](#the-answer-up-front-resumed-not-in-debug-mode)
- [How OpenOCD runs a loader, step by step](#how-openocd-runs-a-loader-step-by-step)
- [The loader's contract](#the-loaders-contract)
- [Why Debug Mode is the wrong tool for loaders](#why-debug-mode-is-the-wrong-tool-for-loaders)
- [Quick Access and SBA: what the spec does sanction](#quick-access-and-sba-what-the-spec-does-sanction)
- [Loaders on the minimal DM of this repo](#loaders-on-the-minimal-dm-of-this-repo)
- [Related documents](#related-documents)

## Why programming flash needs target-side code

Flash is not memory-mapped RAM you can just poke: writes go through a flash
controller with command registers, page-aligned program sequences,
erase-before-program rules, and status bits (`WIP` — write-in-progress) that
must be *polled in a loop* until the operation completes. Two problems for a
debugger:

- **Speed.** One DMI operation is two JTAG `dmi` scans (~95 TCK plus dwell;
  see the conventions in [debug-sequences.md](debug-sequences.md)) — tens
  of microseconds per 32-bit word, without batching. An SPI flash shifted
  out from the core runs at SPI-clock rates, orders of magnitude faster
  than a probe-driven loop over JTAG. Programming megabytes at probe speed
  takes hours; at core speed, seconds.
- **No DM execution.** The Debug Module can halt and resume, and move data
  (abstract Access Memory, or System Bus Access), but it *executes
  nothing* on its own. Something has to run the controller's program
  sequence — and on this minimal DM, Access Memory services exactly one
  load/store per command round-trip, while halted.

So the debugger delegates: download a loader once, then hand it arguments
and let the core do the work at core speed. OpenOCD's mechanism is
`target_alloc_working_area()` (carve RAM the target isn't using) +
`target_write_buffer()` (download) + `target_run_algorithm()` (execute,
timeout, restore) — the generic algorithm framework in
`src/target/algorithm.h`, used by every RISC-V flash driver (`fespi`,
`jtagspi`, and friends) via `src/flash/nor/*.c`.

## The answer up front: resumed, not in Debug Mode

Everything a debugger can make a hart do, sorted by *where* it executes:

| Code | Runs | Privilege | Rules in effect |
|---|---|---|---|
| Debug ROM park loop ([debug-rom.md](debug-rom.md)) | Debug Mode | M | §4.1: interrupts masked, traps end program, `wfi`→nop |
| Progbuf snippet (Access Register/Memory internals, `postexec`) | Debug Mode | M | same §4.1 rules |
| Quick Access progbuf (§3.7.1.2) | briefly in Debug Mode | M | same; "hiccup of a hundred or less cycles" |
| **Flash loader** | **resumed — a *running* hart** | `dcsr.prv` (in practice M) | **normal rules**: traps vector, PMP applies, timers run; interrupts masked by the *debugger* beforehand |
| The interrupted application (before halt / after `dret`) | running | per its own mode | normal rules |

The flash loader is the odd one out: it is the only target code a debugger
injects that runs **outside** Debug Mode. Resume semantics make it what it
is: `resumereq` starts at `dpc` (`pc ← dpc`), at `dcsr.prv` — so
"resume-at-address" is just "write `dpc`, then resume". The debugger points
`dpc` at the loader's entry instead of the halt address, and the hart comes
up running normal fetched code from RAM.

Consequences of running outside Debug Mode, all visible in OpenOCD's
`riscv_run_algorithm()`:

- **Interrupts are not masked by hardware** (the §4.1 mask applies only to
  debug-ROM/progbuf execution). The debugger clears them itself, before
  resuming: OpenOCD saves `mstatus` and writes back `mstatus` with
  `MIE|HIE|SIE|UIE` cleared.
- **PMP applies** (no debug exception to PMP/PMA rules — those apply only
  in Debug Mode). The loader must live in RAM the hart can execute and
  access at its privilege mode.
- **A faulting loader traps for real** — into the *target's own* `mtvec`
  handler, which runs target code that will happily `mret` back into
  nowhere. Nothing catches it: the debugger just sees its poll loop time
  out. Hence loaders are small, hand-audited, dependency-free code.

## How OpenOCD runs a loader, step by step

`fespi_write()` in `src/flash/nor/fespi.c` + `riscv_run_algorithm()` in
`src/target/riscv/riscv.c`, exactly as implemented (SPI flash, one page
buffer at a time):

| # | Step | Mechanism (this repo's terms) |
|---|---|---|
| 1 | Halt the hart. (`riscv_run_algorithm` refuses: "not halted (run target algo)") | `dmcontrol.haltreq` → park loop ([debug-sequences.md](debug-sequences.md) "Halting") |
| 2 | Allocate two working areas in target RAM: loader binary + data buffer | `target_alloc_working_area` |
| 3 | Download the loader (RV32: 199 words = 796 bytes) and the data chunk to RAM | `target_write_buffer` → abstract Access Memory (or SBA if present) |
| 4 | Save state: `pc` (=`dpc`), the six argument GPRs, `mstatus` | abstract Access Register |
| 5 | Write arguments into `a0`–`a5`: `a0`=SPI ctrl base, `a1`=page size, `a2`=data buffer address, `a3`=flash offset, `a4`=chunk byte count, `a5`=program command (+4-byte-address flag) | abstract Access Register |
| 6 | Clear `mstatus` interrupt-enable bits | abstract Access Register |
| 7 | `dpc` ← loader entry address | abstract Access Register (CSR write) |
| 8 | `resumereq` — the hart **leaves Debug Mode** and starts fetching the loader from RAM | `dmcontrol.resumereq`; target state becomes `TARGET_DEBUG_RUNNING` (`debug_execution=true`: debugger-driven, not reported to GDB as a user resume) |
| 9 | Poll until halted again; timeout (e.g. `2 ms` per chunk byte for fespi) → force-halt and dump registers for the error log | `dmstatus.allhalted` polling |
| 10 | The loader's `ebreak` re-enters Debug Mode: `dcsr.cause=1`, `dpc` ← address of the `ebreak` | `dcsr.ebreakm` re-routing ([debug-mode.md](debug-mode.md)) |
| 11 | Read back the result register (`a0` = error code, `0` = success); optionally check `pc` against an expected exit point (fespi passes none) | abstract Access Register |
| 12 | Restore `mstatus`, the argument GPRs, and `dpc` from step 4; free the working areas | abstract Access Register |

Steps 8–10 are the heart of it: one **halt → running → halt** round trip,
with the hart executing ordinary RAM code in between. The interrupted
program's state was saved in step 4 and is put back in step 12 — from its
point of view nothing happened except lost cycles (and whatever the loader
did to the flash controller).

## The loader's contract

What OpenOCD's loader binaries (`contrib/loaders/flash/*`) must satisfy —
and what any loader for this repo's minimal DM must satisfy too:

- **Plain ABI function.** Arguments in `a0`–`a5`, 32-bit `int` result in
  `a0` (`flash_fespi(ctrl_base, page_size, buffer, offset, count,
  flash_info)`). Nonzero = error code; OpenOCD's loaders encode the failing
  step into it.
- **Ends in `ebreak`** — not `dret`. (`dret` is legal only in Debug Mode,
  and the loader is not in Debug Mode; the compiled fespi RV32 loader
  ends `... 0x00100073` = `ebreak`.) With `dcsr.ebreakm=1` (set by the
  debugger while halted), the `ebreak` re-enters Debug Mode with `cause=1`.
- **Runs at `dcsr.prv`.** In practice M-mode: the targets that get
  flash-programmed are bare-metal, halted at their reset vector or
  bootloader. If the debugger needs a different mode it can be set while
  halted — spec §4.1: *"To change the privilege mode, the debugger can write
  `prv` and `v` in `dcsr`."* PMP then applies to the loader as normal code.
- **Position-independent / linked for the working area**, or linked at a
  known address OpenOCD places it; no assumptions about DRAM contents
  beyond the passed arguments (no stack in some loaders; fespi keeps
  everything in registers).
- **Bounded time.** The debugger enforces a timeout (OpenOCD: force-halt
  and report). A loader that hangs is indistinguishable from a crashed
  target otherwise.
- **No trap net.** A fault vectors into the target's own `mtvec` handler
  (see above) — so loaders avoid faulting instructions by construction:
  they touch only the controller and the data buffer they were given.
- **Interrupts may fire** if the debugger didn't mask them — OpenOCD always
  does (step 6), and loaders never enable them.

## Why Debug Mode is the wrong tool for loaders

Given a halted hart *does* execute in the canonical realization
([debug-rom.md](debug-rom.md)), why not run loaders as debug-ROM programs?

- **Size.** The Program Buffer is `progbuf0`…`progbuf15` — at most 16
  32-bit words (see [abstract-commands.md](abstract-commands.md)), and
  implementations ship less. The fespi RV32 loader is 199 words. Program
  Buffer snippets are for `lw`/`csrr`-sized operations; flash loaders are
  real programs with loops.
- **Debug Mode's rules protect the *session*, not the workload.** Traps end
  the program (no retry), `wfi`-class instructions become nops, privilege
  changes are UNSPECIFIED — an environment designed so debug code can
  never lose the hart ([debug-rom.md](debug-rom.md) "The debug execution
  context"). A flash loader wants a normal machine: loops, timeouts via
  counters, real memory ordering. The spec's own escape for longer code
  is exactly what OpenOCD does — run it as ordinary code.
- **The sanctioned "briefly execute" path is Quick Access** (§3.7.1.2):
  halt → progbuf → resume, described in §3.11 as good for *"an occasional
  hiccup of a hundred or less cycles"*. Flash operations run millions of
  cycles; that's what halt-for-the-duration is for.

## Quick Access and SBA: what the spec does sanction

Two spec mechanisms are adjacent to loaders, and neither replaces them:

**Quick Access** (cmdtype 1, optional): per §3.7.1.2 — *"1. If the hart is
halted, the command sets `cmderr` to `halt/resume` … 2. Halt the hart. …
3. Execute the Program Buffer. If an exception occurs, `cmderr` is set to
`exception` … 4. If the Program Buffer executed without an exception, then
resume the hart."* So the hart dips into Debug Mode for the snippet and
returns to running — with 1.0 (change #585) an exception halts the target
so the debugger keeps control. Useful for a quick register/memory poke
through `hartinfo`-addressed `data` registers; useless for flash
programming, both by size (progbuf) and by design (§3.11's
minimal-intrusion intent).

**System Bus Access** (SBA): a DM bus master that moves data *without the
hart at all* — works while running, faster in bulk than per-word Access
Memory. OpenOCD uses it for the download steps (3) when present. But SBA
executes nothing; the controller's command sequences still need target-side
code. This is why [minimal-dm.md](minimal-dm.md) lists SBA as
"memory access while running, flash loaders" — SBA accelerates the
*data movement* half of loading, never the execution half.

## Loaders on the minimal DM of this repo

Nothing needs to be added to run loaders against the minimal DM — it
already provides every primitive `riscv_run_algorithm` uses:

- **halt / resume / poll**: [debug-sequences.md](debug-sequences.md)
  "Halting", "Resuming", "Reading/writing memory".
- **Resume-at-entry**: write `dpc` (abstract CSR write), then
  `resumereq` — the same sequence as "Resuming" with a different `dpc`.
- **`mstatus`/GPR argument setup and result readback**: abstract Access
  Register, `regno 0x1000`–`0x101f` and `0x300`.
- **The download**: abstract Access Memory — [debug-sequences.md](debug-sequences.md)
  "Block transfers" (no `abstractauto`/SBA in this profile: one command per
  word; the burst extension in [abstract-commands.md](abstract-commands.md)
  would cut it to one DMI op per word).
- **`ebreak` re-entry**: `dcsr.ebreakm=1` while halted, cause=1 at
  return ([debug-mode.md](debug-mode.md) "ebreak re-routing").

The C++ implementation under `src/` implements exactly these building
blocks against a behavioral mock ([cpp-implementation.md](cpp-implementation.md));
a flash-loading driver would be a thin composition of them — download via
the memory sequences, arguments via register access, the run via
resume-at-`dpc` plus the halt-poll loop — mirroring OpenOCD's
`riscv_run_algorithm()` step for step.

## Related documents

| Document | What it covers relative to this one |
|---|---|
| [debug-rom.md](debug-rom.md) | The in-Debug-Mode execution this document contrasts with: park loop, §4.1 special rules, progbuf stuffing, `dret` |
| [debug-mode.md](debug-mode.md) | Resume semantics (`pc ← dpc`, `prv ← dcsr.prv`), ebreak re-routing (the loader's return path), entry causes |
| [debug-sequences.md](debug-sequences.md) | The concrete DMI operation listings loaders are built from: halt, register access, block memory transfers, resume |
| [abstract-commands.md](abstract-commands.md) | Access Register/Memory command layouts, the Program Buffer fallback, `cmderr` semantics, burst extension |
| [minimal-dm.md](minimal-dm.md) | The register set and "what to add next" (SBA for the download half, triggers for flash-resident breakpoints) |
| [cpp-implementation.md](cpp-implementation.md) | The reference C++ sequences a flash driver would compose |