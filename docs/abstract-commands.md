# Abstract Commands: The Debug Module's High-Level Command Interface

Abstract commands are the Debug Module's "do something complex in one
write" interface. Instead of the debugger hand-crafting hundreds of DMI
operations (halt, scan out a GPR over a debug port, resume…), it writes a
single 32-bit command word to the DM's `command` register, and the DM's
small state machine does the work — either by driving dedicated hart
debug ports, or by running a tiny program on the hart itself through the
Program Buffer.

This document covers the command mechanism, the three standard commands
(Access Register, Quick Access, Access Memory) with full field layouts,
and how a DM designer extends the command set with custom abstract
commands. Architecture context: [debug-architecture.md](debug-architecture.md);
the minimal DM register set these commands run on:
[minimal-dm.md](minimal-dm.md); ready-made DMI operation listings:
[debug-sequences.md](debug-sequences.md); the C++ implementation of the
command protocol: [src/sequences.cpp](../src/sequences.cpp).

References used: RISC-V External Debug Support
[0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
(§3.6) and
[1.0 ratified](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf)
(§3.7). Field layouts and command semantics below were verified against
both spec texts — the three commands and their encodings are identical in
the two versions.

## Table of contents

- [Registers involved](#registers-involved)
- [Execution protocol](#execution-protocol)
  - [Writing a command while one is busy](#writing-a-command-while-one-is-busy)
- [The command listing: three standard commands](#the-command-listing-three-standard-commands)
  - [Access Register (cmdtype 0)](#access-register-cmdtype-0)
  - [Quick Access (cmdtype 1)](#quick-access-cmdtype-1)
  - [Access Memory (cmdtype 2)](#access-memory-cmdtype-2)
- [The universal fallback: Program Buffer](#the-universal-fallback-program-buffer)
- [Capability discovery](#capability-discovery)
- [0.13.2 vs 1.0 differences](#0132-vs-10-differences)
- [Extending with your own abstract commands](#extending-with-your-own-abstract-commands)
- [Where this is implemented here](#where-this-is-implemented-here)

## Registers involved

| DMI addr | Register | Role |
|---|---|---|
| 0x04–0x0f | `data0`…`data11` | Arguments in / results out (RV32 typically only `data0`, `data1`) |
| 0x12–0x13 | `progbuf0`…`progbuf15` | Program Buffer words (optional, `progbufsize` of them) |
| 0x16 | `abstractcs` | `busy`, sticky `cmderr` (W1C), `progbufsize`, `datacount` |
| 0x17 | `command` | Writing it starts a command |
| 0x18 | `abstractauto` | autoexec bits — accessing a `data`/`progbuf` word re-runs the last command (burst optimization) |

The command word is 32 bits: the top 8 bits are `cmdtype`, the kind of
command; the lower 24 bits are interpreted per command. All commands use
the same argument convention: `arg0`→`data0`, `arg1`→`data1`, `arg2`→`data2`,
with the least-significant word in the lowest-numbered `data` register
(argument width = the access size encoded in the command):

| Argument width | arg0 / return value | arg1 | arg2 |
|---|---|---|---|
| 32 | data0 | data1 | data2 |
| 64 | data0, data1 | data2, data3 | data4, data5 |
| 128 | data0–data3 | data4–data7 | data8–data11 |

## Execution protocol

The debugger runs one abstract command as follows (identical in 0.13.2
and 1.0):

1. Write arguments to the `data` registers (e.g. the address into
   `data1` for Access Memory).
2. Write the command word to `command`. It only starts a command when
   `cmderr = 0` and `busy = 0` — see
   [below](#writing-a-command-while-one-is-busy) for what a write while
   `busy = 1` does.
3. Poll `abstractcs.busy` until it reads 0.
4. Check `abstractcs.cmderr` (sticky until cleared).
5. Read results from the `data` registers — only the registers the
   command is defined to modify ever change.

`cmderr` codes (bits 10:8 of `abstractcs`, write-1-to-clear):

| Code | Name | Meaning |
|---|---|---|
| 0 | none | command succeeded |
| 1 | busy | an abstract command was already running, or `data`/`command` written while `busy` |
| 2 | not supported | the command (or an option/field value in it) is unsupported; also for undefined bits set to non-0 |
| 3 | exception | the command failed executing on the hart (e.g. the requested register does not exist) |
| 4 | halt/resume | the hart was in the wrong state (e.g. Quick Access on a halted hart, hart became unavailable) |
| 5 | bus | the memory access failed on the bus (load/store fault, PMP, …) |

A key batching property is that **`cmderr` gates execution**: while
`cmderr` is non-zero, writes to `command` are ignored. A debugger can
therefore run a whole sequence of commands and check only at the end:
nothing executes after the first failure, and the final `abstractcs`
poll reveals it. To find which batched command failed, the debugger
inspects DM state (`data0`) or hart state. Note that batching means
skipping the *`cmderr`* checks — `busy` must still go low between two
`command` writes, or the new write is discarded (next section).

If a command appears hung, the recovery ladder is: hart reset
(`hartreset`/`ndmreset`) → DM reset (`dmactive` toggle).

### Writing a command while one is busy

Writing to `command` while the previous command is executing
(`abstractcs.busy = 1`) is *defined* behavior, not a protocol violation
— but it is never useful:

1. **The new command is discarded; it never executes.** The DM has no
   command queue — the write only latches the error condition.
2. **The running command still completes**, unaffected.
3. **`cmderr` becomes 1 (busy)** — 0.13.2: "if it is 0"; 1.0: "once the
   command completes". Same effect.
4. **The running command's own error report is lost**, overwritten by
   the busy code.
5. **The channel stays poisoned**: with `cmderr != 0`, every further
   `command` write is ignored until the debugger clears `cmderr` (W1C
   in `abstractcs`).

While `busy = 1`, the only safe access is the poll itself — reading
`abstractcs` (which must not change `cmderr`). Everything else the
debugger might touch is covered by a rule:

| Action while `busy = 1` | Behavior |
|---|---|
| Read `abstractcs` | fine — that's the poll; must not set `cmderr` |
| Write `command` | new command discarded, `cmderr`→1, running command's error lost (above) |
| Write `abstractcs` | `cmderr`→1 |
| Read or write `dataN` / `progbufN` | "accessing" them sets `cmderr`→1; writes don't change the value |
| Write `abstractauto` | ignored; `cmderr`→1 |
| Change `hartsel`, or write 1 to `haltreq`/`resumereq`/`ackhavereset`/`setresethaltreq`/`clrresethaltreq` | debugger *must not*; the DM should enforce it by ignoring the write |

These strict rules are what make **`abstractauto` bursts fail-safe**:
in a burst, the debugger stops polling `busy` and streams `data0`
accesses, each one auto-retriggering the last command. If a command
hasn't finished when the next access arrives, that access hits the busy
rule, `cmderr` latches to 1 — and because `cmderr` gates everything,
the burst simply fizzles out instead of corrupting anything. The
debugger notices at the end via the final `abstractcs` read.

## The command listing: three standard commands

Both the 0.13.2 and the ratified 1.0 spec define exactly three commands:

| `cmdtype` | Command | Mandatory? |
|---|---|---|
| 0 | Access Register | Yes (at minimum: R/W of all GPRs while halted) |
| 1 | Quick Access | Optional |
| 2 | Access Memory | Optional |

Everything else about them — which register groups, which hart states,
which option bits — is optional and must be discovered by probing (see
[capability discovery](#capability-discovery)).

### Access Register (cmdtype 0)

The command performs this sequence of operations; any step failing sets
`cmderr` and skips the remaining steps:

1. If `transfer` and not `write`: copy the register `regno` into `arg0`,
   with the side effects an M-mode read of it would have.
2. If `transfer` and `write`: copy `arg0` into the register `regno`,
   with the side effects an M-mode write would have.
3. If `aarpostincrement`: increment `regno` (optional feature).
4. If `postexec`: execute the Program Buffer once.

| Bits | Field | Meaning |
|---|---|---|
| 31:24 | `cmdtype` | 0 = Access Register |
| 23 | — | must be 0 |
| 22:20 | `aarsize` | 2: 32-bit, 3: 64-bit, 4: 128-bit access (encoding matches `sbcs`'s `sbaccess`). Accessing more bits than the register has fails; smaller-or-equal reads must be supported. RV32 uses 2. |
| 19 | `aarpostincrement` | After a successful access, bump `regno`. Optional. |
| 18 | `postexec` | Run the Program Buffer after the transfer. Optional (and impossible if `progbufsize=0`). With `transfer=0` this bit alone runs the Program Buffer. |
| 17 | `transfer` | 0: don't touch the register (only progbuf/postincrement); 1: do the `write` operation. |
| 16 | `write` | 0: read register → `arg0`; 1: write `arg0` → register. |
| 15:0 | `regno` | Which register (see below). |

Register number space (spec "Abstract Register Numbers" table):

| `regno` range | Group |
|---|---|
| 0x0000–0x0fff | CSRs — the "PC" is accessed here through `dpc` |
| 0x1000–0x101f | GPRs (`0x1000` + register number) |
| 0x1020–0x103f | Floating-point registers |
| 0xc000–0xffff | **Reserved for non-standard extensions and internal use** |

Required behavior: read/write access to **all GPRs while the selected
hart is halted**. Optional: other register groups (CSRs, FPRs), access
while running, `aarpostincrement`, `postexec`. A register may be
supported differently across read, write, and halt status. Registers
that M-mode code couldn't access right now (e.g. `fflags` with
`mstatus.FS=0`) may legitimately fail — the debugger is responsible for
changing state to make them accessible.

RV32 example — read `x5` of the halted selected hart:

```
command = (0 << 24)   /* cmdtype  = Access Register */
        | (2 << 20)   /* aarsize  = 32 bit          */
        | (1 << 17)   /* transfer = 1               */
        | 0x1005      /* regno    = GPR 5           */
        = 0x00221005
```

then read the value from `data0`. Writing `x5` is the same word with
`(1 << 16)` set: `0x00231005`, with the value pre-written to `data0`.
Executing only the Program Buffer makes `aarsize`/`regno` don't-care:
`transfer=0, postexec=1` → `0x00040000` (plus a don't-care `regno`).

### Quick Access (cmdtype 1)

No operands — bits 31:24 are `cmdtype` = 1, bits 23:0 must be 0 — and
the command does not touch the `data` registers. Sequence:

1. If the hart is halted: set `cmderr = 4` (halt/resume) and stop.
2. Halt the hart. If it halts for some other reason (e.g. a breakpoint
   fires first): `cmderr = 4` and stop.
3. Execute the Program Buffer.
4. If it ran without exception, resume the hart.

This is the "poke one thing into a running system and let it continue"
command — the only standard command that combines halt, execute, and
resume into a single command word. Implementing it is optional.

### Access Memory (cmdtype 2)

The command lets the debugger perform memory accesses with **exactly the
memory view and permissions the selected hart itself has** (including
hart-local memory-mapped registers). Sequence:

1. If not `write`: load from the address in `arg1` into the low bits of
   `arg0`.
2. If `write`: store the low bits of `arg0` to the address in `arg1`.
3. If `aampostincrement`: bump `arg1` by the access size.

| Bits | Field | Meaning |
|---|---|---|
| 31:24 | `cmdtype` | 2 = Access Memory |
| 23 | `aamvirtual` | 0: physical address; 1: virtual, translated as from M-mode with MPRV set. An implementation may offer only one and must fail (`cmderr=2`) the other. |
| 22:20 | `aamsize` | 0: 8-bit, 1: 16-bit, 2: 32-bit, 3: 64-bit, 4: 128-bit (encoding matches `sbaccess`). |
| 19 | `aampostincrement` | Increment `arg1` by `aamsize` bytes after the access. Optional but highly recommended — combined with `abstractauto` it gives burst block transfers. |
| 18:17 | — | must be 0 |
| 16 | `write` | 0: read memory → `arg0`; 1: write `arg0` → memory. |
| 15:14 | target-specific | reserved for target-specific uses (see [extending](#extending-with-your-own-abstract-commands)) |
| 13:0 | — | must be 0 |

An access may only fail where M-mode code on the hart would hit the
same failure (fault → `cmderr = 5` (bus)). If memory access while
running is supported, access while halted must be too.

RV32 example — read a 32-bit word at address `A`, auto-incrementing:

```
data1   = A
command = (2 << 24)   /* cmdtype = Access Memory */
        | (2 << 20)   /* aamsize = 32 bit        */
        | (1 << 19)   /* aampostincrement = 1    */
        = 0x02280000
```

Result in `data0`, next address in `data1`. Setting the `autoexecdata0`
bit in `abstractauto` (0x18) re-triggers the whole command on every
`data0` read — one DMI read per memory word instead of a full
command + poll + read round-trip.

## The universal fallback: Program Buffer

Anything the three commands can't express falls back to the Program
Buffer (§3.8 in 1.0): the debugger writes a few instructions to
`progbufN` and executes them **once on the halted hart** via Access
Register with `postexec=1`. The program must end in `ebreak`/`c.ebreak`,
or the DM may provide an implicit `ebreak` when the hart runs off the
end (`impebreak` in `abstractcs`). The hart never leaves Debug Mode
during execution; an exception stops the program, keeps the hart
halted, and sets `cmderr = 3` (exception). A program that never reaches
its `ebreak` leaves the hart spinning in Debug Mode — the debugger has
lost control. What the hart is actually doing while it runs that buffer —
the park loop and debug ROM of the canonical realization — is described in
[debug-rom.md](debug-rom.md). See [debug-sequences.md](debug-sequences.md)
for the concrete flows.

## Capability discovery

There is no capability bitmap: the *only* sanctioned way to learn
which commands and options a DM supports, in a given hart state
(running / halted / held in reset), is to **attempt the command and
read `cmderr`**: `cmderr = 2` (not supported) means "this variant
doesn't exist here", success means it does. Probing the auto-increment
variants works the same way — write the command with the bit set and
check for `cmderr=2`. (A command may be supported with some options set
and not others; undefined-as-0 bits set to non-0 must also return
`cmderr=2`.)

## 0.13.2 vs 1.0 differences

The three commands and their bit layouts are identical; the differences
are behavioral fine print:

- **Quick Access on exception**: 1.0 sets `cmderr=3`, ends Program
  Buffer execution, and *halts the hart* (cause 3); 0.13.2 reported the
  exception but continued the command and resumed the hart anyway.
- **`aarpostincrement` wrap**: 0.13.2 defines `regno` wrapping to 0;
  1.0 makes `regno` UNSPECIFIED after the highest supported value, and
  leaves undefined whether the increment happens when `transfer=0`.
- **1.0 recommends** that if one register in a group (e.g. CSRs) is
  accessible, all registers in that group should be, and that the Core
  Debug Registers (`dcsr`, `dpc`) be accessible whenever abstract CSR
  access is implemented.

## Extending with your own abstract commands

The spec deliberately leaves room below the three standard `cmdtype`s.
There are three escalation levels, from most to least standard-friendly:

### 1. Custom `regno` values (0xc000–0xffff) — the sanctioned extension point

The spec explicitly reserves this range "for non-standard extensions and
internal use." If your hart has custom state (vendor CSRs, auxiliary
status registers, core-internal debug state), map it into
`regno >= 0xc000` and serve it through the *standard* Access Register
command — the DM just decodes those `regno`s in its register-access
path. The payoff: stock debuggers can be taught to use it without any
command-layer changes — OpenOCD/GDB expose custom registers via the
target description (XML) and map them onto `regno >= 0xc000`.

### 2. Target-specific bits inside existing commands

Access Memory bits 15:14 are explicitly "reserved for target-specific
uses" — e.g. to select an address space or access mode your platform
adds. A standard debugger leaving them 0 gets defined behavior; your
own debugger can set them for extra semantics. (0.13.2 and 1.0 reserve
the same bits.)

### 3. Whole new `cmdtype` values (3–255)

The `command` register is defined generically — "`cmdtype`: the type
determines the overall functionality of this abstract command" — and
only 0, 1, 2 are assigned. The values 3–255 are unassigned, and the
existing error contract gives a clean way to handle them: a DM must set
`cmderr = 2` (not supported) for commands and options it doesn't
implement. So a custom DM may define, say, `cmdtype = 0x40` =
"flush trace FIFO into `data0`", and:

- a *standard* debugger probing it just sees `cmderr = 2` — harmless,
  well-defined;
- *your* debugger detects the feature by writing the command and
  observing `busy` toggling / a different `cmderr`.

Rules of thumb when defining a custom `cmdtype`:

- Keep the argument convention (`arg0`→`data0`, results in `data0`).
- Obey `busy` and the sticky-`cmderr` gating, so batching and error
  semantics keep working — including `cmderr=1` for writes while busy.
- Consider `abstractauto` support (autoexec) if the command is useful in
  bursts.
- Bit 23:16 of your command word are yours to define; don't collide with
  `data`-register semantics of other commands.

The cost: nothing outside your own toolchain knows about the command —
OpenOCD would need a custom target driver; in this repo it means
extending the sequence layer in [src/sequences.cpp](../src/sequences.cpp).

### What to try first

Prefer the levels in order 1 → 2 → 3. Before inventing a new command,
check whether the Program Buffer already covers it: running
`lw`/`csrr`/custom-instruction snippets via `postexec` (see
[the universal fallback](#the-universal-fallback-program-buffer)) is
fully portable and needs zero DM extensions. A new `cmdtype` only pays
off when the operation must work *without halting* (Quick Access aside)
or must be a single command word for throughput.

## Where this is implemented here

The command-words-as-named-constants, the `busy`/`cmderr` polling loop,
and the burst extension (`aampostincrement` + `abstractauto`) live in
[src/sequences.cpp](../src/sequences.cpp) /
[src/sequences.hpp](../src/sequences.hpp), exercised against a
behavioral mock target ([src/mock/mock_target.cpp](../src/mock/mock_target.cpp))
by [src/demo.cpp](../src/demo.cpp). Layering and design decisions:
[cpp-implementation.md](cpp-implementation.md).
