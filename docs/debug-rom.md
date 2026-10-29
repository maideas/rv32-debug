# The Debug ROM: what a halted hart executes

[debug-mode.md](debug-mode.md) describes Debug Mode as a *quiesced* state —
the minimal DM of this repo has no debug ROM, so a halted hart executes
nothing and every service is a hardware port. That is one of two legitimate
realizations. The RISC-V External Debug Support specification itself frames
Debug Mode differently (its "Implementation Notes" and §4.1 in 1.0):

> *the hart fetches instructions from a debug ROM* (mapped inside the Debug
> Module) *and executes them at M-privilege under special rules*, ending
> every program in `dret`.

This document unpacks that sentence: what the debug ROM is, what the
"park loop" does, what the "special rules" of debug execution are, how a
plain register read is actually *a tiny program run on the halted hart*,
and why "ends every program in `dret`" is the load-bearing part of the
design. The minimal-profile contrast (no ROM at all) is at the end; the
identical observable behavior is why both coexist.

References used: the RISC-V External Debug Support specification,
[version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
(what deployed silicon implements) and
[version 1.0](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf),
plus the OpenOCD `riscv-013` driver. Quoted phrases below are from the
spec's implementation notes ("Debug Module" / §A.1 in 1.0) and §4.1
"Debug Mode Execution" rules.

## Table of contents

- ["Halted" does not mean frozen](#halted-does-not-mean-frozen)
- [The entry jump](#the-entry-jump)
- [The park loop](#the-park-loop)
- [The debug execution context: M-privilege under special rules](#the-debug-execution-context-m-privilege-under-special-rules)
- [Servicing a debugger request end to end](#servicing-a-debugger-request-end-to-end)
- [Exceptions: how `cmderr` gets set without a trap](#exceptions-how-cmderr-gets-set-without-a-trap)
- [`dret`: the only way out](#dret-the-only-way-out)
- [The minimal profile: same behavior, no ROM](#the-minimal-profile-same-behavior-no-rom)
- [Related documents](#related-documents)

## "Halted" does not mean frozen

The single most counter-intuitive fact about the canonical realization:
**a halted hart keeps executing**. It does not retire *application*
instructions and does not take *interrupts* — but it runs a third kind of
program entirely: code owned by the debugger infrastructure, fetched from a
small ROM inside the Debug Module. "Halted" describes what happened to
*your* program (it is parked at `dpc`, frozen, inspectable), not what the
CPU is doing (it is very much running — just not your code).

So Debug Mode is genuinely an *execution state*, not a clock-gated stop:

```
 halt request ──►  pc ← debug ROM start
                   ┌──────────────────────────────────────────┐
                   │  DEBUG ROM (inside the DM, uncached)     │
                   │                                          │
                   │  park loop ──► poll DM flags ──┐         │
                   │      ▲                         │         │
                   │      │ "execute progbuf"       │         │
                   │      │         ▼               │         │
                   │  progbuf snippet               │         │
                   │  (ends in ebreak ──► back)     │         │
                   │                                │         │
                   │            "resume" flag ──────┤         │
                   │                                ▼         │
                   │                              dret         │
                   └──────────────────────────────────────────┘
                          dret: pc ← dpc, prv ← dcsr.prv
                          (back to your program, at the
                           privilege mode it was halted in)
```

Everything else in this document is a walk through that picture.

## The entry jump

On halt (`haltreq`, a re-routed `ebreak`, a trigger, or a completed single
step) the hart jumps "to a defined memory region that is serviced by the
DM and is only accessible to the harts in Debug Mode":

- **`pc` is saved to `dpc`**, and the entry reason to `dcsr.cause` — so the
  halt is precise and resumable, just like a trap, *but*:
- **"This jump is similar to a trap but it is not architecturally
  considered a trap"** — `mepc`/`mcause`/`mtval` stay untouched, `mtvec`
  plays no role, and it does not count as a trap for trigger behavior.
  This is what [debug-mode.md](debug-mode.md) "What Debug Mode is (and is
  not)" means by *orthogonal to traps*: software cannot intercept or
  disable it, because it never goes through any software-visible trap path.
- **The ROM fetches are uncached** — "accesses to this memory should be
  uncached to avoid side effects from debugging operations": halting must
  not perturb caches, because the debugger will read machine state through
  them and expects what it reads to be the truth.

## The park loop

The first thing the ROM does with a freshly halted hart is the **park
loop** — the spec's own name for the halted hart's idle loop:

> *"In the park loop the hart writes its `mhartid` to a memory location
> within the Debug Module to indicate that it is halted. To allow the DM to
> individually control one out of several halted harts, each hart polls
> for flags in a DM-controlled memory location to determine whether the
> debugger wants it to execute the Program Buffer or perform a resume."*

Two jobs, in plain words:

1. **Announce halting** — the write of `mhartid` into DM memory is how the
   hart *tells the DM it is halted*: in this realization, `dmstatus`'s
   `allhalted`/`anyhalted` bits ultimately reflect that write reaching the
   DM, not a dedicated status wire per hart.
2. **Wait for orders** — the loop polls DM-controlled flags: does the
   debugger want me to run my Program Buffer, or to resume? Nothing else
   happens until one of those flags is set.

This is also why the spec says "the hart **must** leave the stalled state"
when a halt request arrives during `wfi` (§4.3): a hart parked in `wfi`
would otherwise never come around to fetching the ROM. The halt path has
to preempt *any* stalled state, bounded in time.

## The debug execution context: M-privilege under special rules

While the ROM and Program Buffer code run, the hart stays in Debug Mode
and executes "just as they do in M-mode" — with machine privilege,
whatever mode your program was in (M/S/U) — but under §4.1 rules that
deliberately defang everything that could make debug execution lose
control of the machine:

| Rule | Why debug needs it |
|---|---|
| **All interrupts (including NMI) are masked.** | Debug code must run to completion; being preempted by the very interrupt you are trying to debug would corrupt the session. Interrupts stay *pending* and fire after `dret`. |
| **Traps don't take place — they end the program.** | An exception must never vector the halted hart into *your* `mtvec` handler: that would execute application code, corrupt debug state, and mask the real cause. Instead the hart jumps to a fixed DM address (see [below](#exceptions-how-cmderr-gets-set-without-a-trap)) and `mepc`/`mcause`/`mtval` are **not** updated. |
| **Stalled-state instructions act as `nop`.** | `wfi` (and in 1.0 `wrs.sto`/`wrs.nto`) must never park a *debugging* hart the way `wfi` can park yours. |
| **Privilege-changing instructions are UNSPECIFIED.** | `mret`/`sret`/`ecall` — the debugger changes mode by writing `dcsr.prv`, not by executing a return. |
| **Debug CSRs are accessible; `mprv` may be honored per `mprven`; permission checks full or relaxed per `relaxedpriv`.** | `dcsr`/`dpc`/`dscratch` must be reachable for the ROM's own bookkeeping; the knob decides whether debug memory accesses get M-mode's full physical rights. |
| **`lr` reservations may be lost entering Debug Mode.** | Known wart: a breakpoint between `lr` and `sc` can starve the `sc` forever; the spec documents it so debuggers can set breakpoints *after* the pair instead. |
| **`stoptime`/`stopcount` may freeze timers/counters** (if implemented; the minimal profile ties them to 0). | Otherwise counters keep ticking during a halt — normal for small cores, see [debug-mode.md](debug-mode.md) "Behavior while halted". |

And one absolute requirement that is easy to miss: **PMP and PMA must not
block the ROM** — "The PMP must not disallow fetches, loads, or stores in
the address range associated with the Debug Module when the hart is in
Debug Mode, regardless of how the PMP is configured. The same is true of
PMA. Without this guarantee, the park loop would enter an infinite loop
of traps and debug would not be possible." Since traps *end the program*
in Debug Mode, a PMP-blocked fetch would mean the park loop itself could
never make progress — the spec calls out the failure mode explicitly.

## Servicing a debugger request end to end

This is where "the hart fetches from a debug ROM" stops being an
implementation footnote and becomes the mechanism abstract commands are
built on. The trick is **the DM's registers are memory-mapped for the
halted hart**:

> *"`data0` etc. are mapped into regular memory at an address relative to
> zero with only a 12-bit imm. The exact address is an implementation
> detail that a debugger must not rely on. For example, the data registers
> might be mapped to 0x400."*

> *"For additional flexibility, `progbuf0`, etc. are mapped into regular
> memory immediately preceding `data0`, in order to form a contiguous
> region of memory which can be used for either program execution or data
> transfer."*

So the DM's `data` and `progbuf` words (the same registers the debugger
reaches over DMI — see [minimal-dm.md](minimal-dm.md)) are **just
addresses** from the halted hart's point of view: `0x400` is `data0`,
`progbuf0` sits just below, and a 12-bit load/store immediate is all the
ROM needs to move debugger data in and out. The DM drives the same
registers the debugger writes; the hart reads/writes them as memory.

Now the stuffing protocol:

> *"To execute an abstract command, the DM first populates some internal
> words of program buffer according to command. When `transfer` is set, the
> DM populates these words with `lw <gpr>, 0x400(zero)` or
> `sw <gpr>, 0x400(zero)`. 64- and 128-bit accesses use `ld`/`sd` and
> `lq`/`sq` respectively. If `transfer` is not set, the DM populates these
> instructions as nop's. If `postexec` is set, execution continues to the
> debugger-controlled Program Buffer, otherwise the DM causes an `ebreak`
> to execute immediately."*

Direction of the traffic, worked out from the semantics (`0x400` =
`data0`):

- **Read a GPR** (`write=0`): DM stuffs `sw <gpr>, 0x400(zero)` — the
  hart *stores* the register into `data0`'s memory address.
- **Write a GPR** (`write=1`): debugger's value already sits in `data0`;
  DM stuffs `lw <gpr>, 0x400(zero)` — the hart *loads* it from `data0`
  into the register.

### Worked example: "read GPR x10" on a ROM-based DM

The hart is halted, `x10` = `0xdeadbeef`. Full trace:

| # | Who | What happens |
|---|---|---|
| 1 | debugger → DM | scan `command` ← Access Register, `aarsize=2` (32-bit), `write=0`, `regno=0x100A` (x10), `transfer=1`, `postexec=0` |
| 2 | DM | `abstractcs.busy` rises; DM stuffs its internal progbuf words: `sw x10, 0x400(zero)` (+ nop padding) |
| 3 | DM → hart | sets the "execute Program Buffer" flag from [the park loop](#the-park-loop) |
| 4 | hart | park loop sees flag, jumps to the stuffed buffer, executes `sw x10, 0x400(zero)` → the store lands in DM memory = `data0` |
| 5 | DM | no `postexec` → DM causes an `ebreak` to execute; *"when `ebreak` is executed (indicating the end of the Program Buffer code) the hart returns to its park loop"*, typically after a `fence` so the `data0` write is visible ("to ensure that any effects from the abstract command, such as a write to `data0`, take effect before the DM returns busy to 0") |
| 6 | DM | sees the park-loop write → `busy` falls |
| 7 | debugger ← DM | reads `data0` = `0xdeadbeef` over DMI |

If `postexec` had been set, step 5 would instead continue into the
debugger-written `progbuf0..N` words (the universal fallback for anything
the standard commands can't express — [abstract-commands.md](abstract-commands.md)
"The universal fallback: Program Buffer"), ending in the debugger's own
`ebreak` or the DM's implicit one (`impebreak` in `abstractcs`, which lets
the ROM run a single-word buffer without the debugger wasting a word on
the terminator).

That is the full meaning of "the hart fetches from a debug ROM": **a
register read is the halted hart executing a two-instruction hand-stuffed
program on itself**, coordinated through memory-mapped DM registers and
flags polled by the park loop. No dedicated GPR read port exists in this
realization — the "port" *is* the ROM executing `sw`.

## Exceptions: how `cmderr` gets set without a trap

Per §4.1, traps don't vector into `mtvec` while in Debug Mode — but the
debugger still needs to *know* a Program Buffer execution faulted. The ROM
provides the path:

> *"If an exception is encountered, the hart jumps to an address within the
> Debug Module. The code there causes the hart to write to the Debug Module
> indicating an exception. Then the hart jumps back to the park loop. The
> DM infers from the write that there was an exception, and sets `cmderr`
> appropriately."*

Concretely: the park loop polls flags, an exception flag is written into
DM memory by the ROM's exception stub, the DM translates that into
`abstractcs.cmderr = 3` (exception), and the next debugger `command`
write fails fast instead of running. Architectural state stays clean
because the exception never trapped architecturally.

And the spec is blunt about the failure mode of a buggy Program Buffer:

> *"If the debugger executes a program that doesn't terminate with an
> `ebreak` instruction, the hart will remain in Debug Mode and the
> debugger will lose control of the hart."*

Nothing supervises a runaway progbuf program — it just loops forever
inside Debug Mode. Hence the strict protocol rules in
[abstract-commands.md](abstract-commands.md): a program that cannot reach
its `ebreak` must not be executed. One related hazard for the *debugger*:
0.13.2 notes that "executing the Program Buffer may clobber `dpc`" —
implementations may reuse the PC register for progbuf execution — so a
debugger must save `dpc` after halting and restore it before leaving Debug
Mode (or rely on `impebreak`-style side channels to recover it).

## `dret`: the only way out

The last clause of the sentence: *"to resume execution, the debug module
sets a flag which causes the hart to execute a `dret`."* The resume flag is
one of the very flags the park loop polls. When the ROM sees it, it runs
the architecturally defined exit:

- `dret` (recommended encoding `0x7b200073`) — *"an instruction that only
  has meaning while in Debug Mode and not executing from the Program
  Buffer"*:
  - `pc ← dpc` — resume where you were;
  - privilege mode ← `dcsr.prv` — resume *as* what you were (1.0 adds
    virtualization bits `v`/`pelp` to the restore);
  - the hart is no longer in Debug Mode — pending interrupts can fire
    again.
- **Executed anywhere other than Debug Mode, `dret` is an illegal
  instruction.** This is the safety net that makes the whole ROM design
  viable: debug code cannot "leak" the hart into normal execution by
  accident, and normal software can never fake its way into resuming with
  debug authority — it would just take an illegal-instruction trap.

So "ends every program in `dret`" is not a stylistic remark about ROM
layout — it is the statement that **resume is itself a program**: the
park loop sees the resume flag, jumps to the ROM's resume stub, and the
last instruction of that stub, `dret`, is the single architecturally
defined gate back into your execution.

## The minimal profile: same behavior, no ROM

[debug-mode.md](debug-mode.md) "The minimal DM twist" describes the other
realization — the one this repo's DM implements — where none of the
machinery above exists. Side by side:

| Aspect | Canonical (ROM-based) | Minimal (this repo) |
|---|---|---|
| Halted hart executes | debug ROM park loop, progbuf snippets | **nothing** — quiesced |
| Register access | DM stuffs `lw`/`sw` into internal progbuf words; hart runs them against memory-mapped `data0` | DM drives a hardware port into the GPR/CSR file directly |
| Memory access | progbuf load/store program through the hart's LSU | same LSU, sequenced by the DM's abstract-command FSM |
| Exception in a program | ROM exception stub writes DM memory; DM sets `cmderr=3` | no instructions → no exceptions; the FSM handles bus errors instead |
| `busy`/done detection | park-loop `mhartid` write / done flag in DM memory | status lines and FSM completion (see [dm-hart-interface.md](dm-hart-interface.md)) |
| Resume | park loop sees flag → ROM executes `dret` | run-control FSM performs the `dret`-equivalent state update (`pc ← dpc`, `prv ← dcsr.prv`) in hardware |
| `progbuf0..15` | present (`progbufsize` > 0), memory-mapped for the hart | absent (`progbufsize=0`), `hartinfo` reads 0 |
| `dret` encoding | implemented in the hart (ROM executes it) | optional/unreachable; executing it outside Debug Mode is illegal either way |

What makes both legitimate is that **every difference is behind the DM's
register interface**: the debugger only ever sees `data`/`command`/
`abstractcs`/`dmstatus` over DMI (plus whatever capability probing
reveals, [abstract-commands.md](abstract-commands.md) "Capability
discovery"), and those behave identically. OpenOCD services either
realization through the same abstract commands; it never runs ROM code and
never needs to know which one it is talking to. The ROM buys generality
(any instruction sequence a debugger can express), the quiesced profile
buys simplicity — for a single-hart RV32 core the ports are far cheaper
than a ROM, an execution context, and the memory-mapped `data` plumbing.

## Related documents

| Document | What it covers relative to this one |
|---|---|
| [debug-mode.md](debug-mode.md) | Debug Mode as a hart execution state: entry causes, `dcsr`/`dpc`, ebreak re-routing, single stepping — and the "minimal DM twist" this document contrasts with |
| [minimal-dm.md](minimal-dm.md) | The register set of this repo's ROM-less DM: bit maps, coupled bits, protocol behaviors, OpenOCD compatibility |
| [abstract-commands.md](abstract-commands.md) | The abstract-command interface the ROM services: Access Register/Memory, `cmderr` semantics, the Program Buffer fallback, capability probing |
| [debug-sequences.md](debug-sequences.md) | The debugger workflows over the register set, as concrete DMI operation listings |
| [debug-architecture.md](debug-architecture.md) | The full layered picture: JTAG TAP → DTM → DMI → DM → hart debug logic, 0.13.2 vs 1.0 |
| [dm-hart-interface.md](dm-hart-interface.md) | The DM↔hart signal bundle in the quiesced realization: request/status lines, data ports, handshakes |
| [flash-loaders.md](flash-loaders.md) | Flash loaders — target-side code that runs *outside* Debug Mode, unlike the ROM machinery above |