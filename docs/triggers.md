# The Trigger Module: Hardware Breakpoints and Watchpoints

This document describes the **trigger module** — the hart-side hardware that
implements hardware breakpoints and watchpoints: comparators that watch
instruction addresses, load/store addresses, and load/store data while the
hart runs at full speed, and stop it when something matches. It covers what
the module is, where it sits relative to the hart and the Debug Module, the
trigger CSRs (`tselect`/`tdata1`/`tdata2`/`tinfo`), the `mcontrol`
configuration word bit by bit, the exact **timing semantics** of when a
trigger halts the hart (before vs after the instruction, before vs after
the bus access — with the load-data double-access trap spelled out), and
the concrete DMI sequences a debugger uses to arm, inspect, and remove
triggers.

It extends the minimal profile of this repo: the minimal DM deliberately
omits the trigger module (see "What this interface deliberately omits" in
[debug-mode.md](debug-mode.md) and "What to add next" in
[minimal-dm.md](minimal-dm.md)); everything below is what you add to close
that gap — and, just as important, what you can *leave out* and still be
useful.

References used: the RISC-V External Debug Support specification,
[version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
(what deployed silicon implements) and
[version 1.0](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf)
(where chapter 5 became the standalone "Sdtrig" ISA extension), plus the
OpenOCD `riscv-013` driver (trigger enumeration and GDB `Z1`/`Z2`/`Z3`/`Z4`
packet handling). Register layouts and normative quotes below were verified
against those sources.

## Table of contents

- [What a trigger is](#what-a-trigger-is)
- [Where the module lives, and how it connects](#where-the-module-lives-and-how-it-connects)
- [The trigger CSRs](#the-trigger-csrs)
  - [tselect (0x7a0): the slot multiplexer](#tselect-0x7a0--the-slot-multiplexer)
  - [tdata1 (0x7a1): the configuration wrapper](#tdata1-0x7a1--the-configuration-wrapper)
  - [tdata2 (0x7a2): the compare value](#tdata2-0x7a2--the-compare-value)
  - [tdata3 (0x7a3): optional extra slot data](#tdata3-0x7a3--optional-extra-slot-data)
  - [tinfo (0x7a4): the capability report](#tinfo-0x7a4--the-capability-report)
  - [tcontrol (0x7a5): self-hosted gates](#tcontrol-0x7a5--self-hosted-gates)
- [mcontrol (type 2): the configuration word](#mcontrol-type-2-the-configuration-word)
  - [What to match](#what-to-match)
  - [What to do on a match](#what-to-do-on-a-match)
  - [Chaining triggers](#chaining-triggers)
- [Timing: when exactly the hart halts](#timing-when-exactly-the-hart-halts)
- [Actions: what a match does](#actions-what-a-match-does)
- [The halt and the aftermath: dcsr.cause, hit, re-firing](#the-halt-and-the-aftermath-dcsrcause-hit-re-firing)
- [Enumerating the triggers](#enumerating-the-triggers)
- [Sequences over DMI](#sequences-over-dmi)
- [What a minimal RV32 implementation costs](#what-a-minimal-rv32-implementation-costs)
- [0.13.2 vs 1.0](#0132-vs-10)
- [Related documents](#related-documents)

## What a trigger is

A trigger is a programmable **comparator** inside the hart. While the hart
runs normally, the comparator watches the stream of retirements — the
virtual address of each instruction executed, and the address (and
optionally data) of each load and store — against a compare value the
debugger installed. When it matches, the trigger *fires* and takes a
configurable **action**: raise an ordinary breakpoint exception, or pull
the hart into Debug Mode.

This differs from the two mechanisms the minimal profile already has:

| | Software breakpoint (ebreak patching) | Trigger, execute-address match | Trigger, load/store match |
|---|---|---|---|
| What it watches | an instruction word in memory | the **PC** of each executed instruction | **LSU address/data** of each load/store |
| Works on ROM/flash code | no (needs writable memory) | **yes** | **yes** |
| Works while running | — (halts via cause=1) | yes, hardware watches at full speed | yes |
| Monitors *data* accesses | impossible | no (matches instruction addresses) | **yes — the only watchpoint mechanism** |
| Hardware cost | one `dcsr.ebreakm` bit | 1 address comparator + CSRs | + address/data compare in LSU |

So the trigger module is what you need for debugging code out of ROM or
flash (no memory to patch), and the *only* way to watch data — GDB's
`watch` (write), `rwatch` (read), `awatch` (access) map to load/store
triggers, its `break` on flash addresses maps to execute-address triggers.

## Where the module lives, and how it connects

The trigger module is specified in the debug spec but lives entirely
**inside the hart** — layer 5 in the stack of
[debug-architecture.md](debug-architecture.md), alongside Debug Mode, the
debug CSRs, and single-step. The Debug Module contains **no trigger
logic at all**:

```
JTAG TAP → DTM → DMI → DM (run control, abstract commands)
                      │
                      │  halt_req / resumereq · halted / running / resumeack
                      │  dbg_csr_* port (abstract Access Register)
                      ▼
        ┌───────────── hart debug logic (RV32 core) ─────────────┐
        │  Debug Mode entry OR:                                  │
        │    halt_req · ebreakX · step complete                  │
        │    · trigger fired (action=1) ─────────► enter Debug   │
        │  dcsr / dpc / dret · single step                       │
        │                                                        │
        │  ┌───────────────── trigger module ─────────────────┐  │
        │  │ tselect/tdata1/tdata2/tinfo CSRs                 │  │
        │  │  ├─ comparator #0 ─┐                             │  │
        │  │  ├─ comparator #1  │ watch: pc · LSU addr · data │  │
        │  │  └─ ...           ─┘                             │  │
        │  └──────────────────────────────────────────────────┘  │
        └────────────────────────────────────────────────────────┘
```

Its three ties to the hart:

1. **CSR interface (programming).** The comparators are configured through
   ordinary CSRs (next section) — reached by M-mode software running on
   the hart, or by the debugger *through* the DM's abstract Access
   Register command while halted, via the same `dbg_csr_*` port the
   minimal profile already routes.
2. **Observation inputs.** The comparators passively tap the pipeline:
   instruction address near fetch/decode, LSU address and data at the
   memory stage. No bus master, no stalls — the hart runs at full speed.
3. **Action output.** On a match, the configured action is executed. With
   `action=1` the trigger asserts one more input into the halt-entry OR
   alongside `halt_req`, ebreak re-routing, and step completion — the hart
   enters Debug Mode with `dcsr.cause=2`.

The connection to the DM is therefore deliberately **indirect**: no wires,
no DMI registers. The debugger programs a trigger by halting the hart
(`haltreq`), writing the trigger CSRs via abstract commands, and resuming;
when the trigger fires, the DM merely sees `halted` rise and the debugger
reads `dcsr.cause=2` to learn why. The whole trigger module can be added
with zero changes to the DTM, DMI, or DM — one reason it is the top item on
the minimal profile's add-next list.

## The trigger CSRs

One narrow CSR window fronts the whole module: a bank of trigger slots,
with `tselect` picking the slot and `tdata*` reading/writing that slot's
configuration and compare value. Everything is an ordinary CSR — the
debugger reaches them with the same abstract-command shape used for
`dcsr` (regno = CSR address), and M-mode software with plain
`csrr`/`csrw`:

| CSR | Address | Access | What it holds |
|---|---|---|---|
| `tselect` | 0x7a0 | R/W | Index of the trigger slot the `tdata*` CSRs currently address |
| `tdata1` | 0x7a1 | R/W | Slot configuration — the `mcontrol` word (type 2), see below |
| `tdata2` | 0x7a2 | R/W | Slot compare value — the address (or data) to match |
| `tdata3` | 0x7a3 | R/W | Optional extra slot data (used by the rarer trigger types, not by mcontrol) |
| `tinfo` | 0x7a4 | R | (optional) Which trigger types the selected slot supports |
| `tcontrol` | 0x7a5 | R/W | (optional, self-hosted use) Gates for firing inside trap handlers |

The access and reset annotations used in the tables below are the spec's
own; they are worth reading carefully because they carry the module's
programming contract:

| Mode | Meaning |
|---|---|
| **R** | Read-only; writes are ignored (a whole missing CSR raises an illegal-instruction exception instead) |
| **R/W** | Readable and writable |
| **WARL** | Write-any-read-legal: write any value, the register keeps a *legal* one — read back to learn what stuck |
| **Preset** | The reset value is chosen by the implementation — probe it, don't assume it |

(0.13.2 labels the `tdata*` fields plain "R/W" but attaches write-any-
read-legal semantics to the whole window — "All tdata registers follow
write-any-read-legal semantics"; 1.0 formalizes this by relabeling the same
fields **WARL**. Treat the two as one discipline: every write needs a
read-back until the implementation's feature set is known.)

Access rules the spec attaches to the window as a whole (0.13.2 §5.2):

- **Machine and Debug Mode only.** "The trigger registers are only
  accessible in machine and Debug Mode to prevent untrusted user code
  from causing entry into Debug Mode without the OS's permission."
- **Write-any-read-legal.** "If a debugger writes an unsupported
  configuration, the register will read back a value that is supported
  (which may simply be a disabled trigger). This means that a debugger
  must always read back values it writes to tdata registers." (WARL —
  the discipline behind the whole module.)
- **No cross-talk.** "Writes to one tdata register may not modify the
  contents of other tdata registers, nor the configuration of any
  trigger besides the one that is currently selected."
- **Coexistence.** M-mode software may be using triggers of its own; the
  debugger "must restore" `tselect` if it modifies it, and only ever
  programs slots it has claimed — the `dmode` bit is the claim marker
  (below).
- **XLEN vs DXLEN.** "In this section XLEN means MXLEN when in M-mode,
  and DXLEN when in Debug Mode" — the wrapper fields (`type`, `dmode`)
  shift if DXLEN ≠ MXLEN. On an RV32 hart with DXLEN=MXLEN=32 (the
  usual case, and this repo's) every layout below is fixed.
- **Absence is signaled by exception.** Accessing a trigger CSR that
  isn't implemented raises an illegal-instruction exception, which
  surfaces through the abstract command as `cmderr=3` — exactly how
  OpenOCD detects "no trigger module" and falls back to ebreak patching.
- **Triggers do not fire while in Debug Mode** — a halted hart never
  trips its own comparators (not on the DM's debug-port LSU accesses,
  not on debug-ROM code in a full-spec DM).

### tselect (0x7a0) — the slot multiplexer

A plain WARL index register. "The set of accessible triggers must start
at 0, and be contiguous", and "writes of values greater than or equal to
the number of supported triggers may result in a different value in
this register than what was written" — which is precisely the
enumeration mechanism: write 0, 1, 2, … and read back until the value
stops matching (the full loop is in
[Enumerating the triggers](#enumerating-the-triggers)). Writing
`tselect` has no other effect: it retargets the `tdata*` window without
changing any slot.

### tdata1 (0x7a1) — the configuration wrapper

The slot's control word. Its top bits form a wrapper identical for
every trigger type; the low bits are whatever that type defines. RV32
layout:

| Bits | Field | Access | Reset | Meaning |
|---|---|---|---|---|
| 31:28 | `type` | R/W | preset | Which trigger format this slot implements — the value table below |
| 27 | `dmode` | R/W | 0 | Write guard: 0 = both Debug Mode and M-mode software can write this slot; 1 = only Debug Mode can (writes from other modes are ignored). Writable from Debug Mode only |
| 26:0 | `data` | R/W | preset | The configuration word proper, format per `type` — for type 2, the `mcontrol` word of the next section |

The `type` values (0.13.2 defines 0–5 and 15; 6, 7 and 12–14 are 1.0
additions):

| `type` | Name | What it is |
|---|---|---|
| 0 | — | "There is no trigger at this tselect" — the enumeration end-marker |
| 1 | legacy | Legacy SiFive address-match trigger; "should not be implemented and aren't further documented" |
| 2 | **mcontrol** | Address/data match — the trigger of this document |
| 3 | icount | Instruction-count trigger: fire after N executed instructions |
| 4 | itrigger | Interrupt trigger: fire on a specific interrupt |
| 5 | etrigger | Exception trigger: fire on a specific exception |
| 6 | mcontrol6 | 1.0: mcontrol's replacement (see [0.13.2 vs 1.0](#0132-vs-10)) |
| 7 | tmexttrigger | 1.0: fires on a signal fed into the trigger module from outside |
| 12–14 | custom | 1.0: vendor-defined, non-standard types |
| 15 | disabled | "This trigger exists (so enumeration shouldn't terminate), but is not currently available" — the deliberate off-state of a present trigger |

Two consequences of the wrapper design:

- **Safe (re)programming order.** Which `tdata2` values are legal depends
  on the active type (compare a 32-bit address vs a data value vs an
  instruction word), so a debugger never rewrites a *live* slot: first
  write 0 (or 15) to `tdata1` to disable it, then write `tdata2`, then
  write the full configuration word — the sequences below follow that
  order, and the no-cross-talk rule guarantees the other slots stay
  untouched.
- **`dmode` is the debugger's claim marker.** Both versions state that
  `action=1` is "only supported when the trigger's `dmode` is 1" — a
  trigger able to pull the hart into Debug Mode must not be rewritable
  by running software. External debuggers therefore set `dmode=1` on
  every slot they arm; co-debugging M-mode software then finds those
  slots read-only. (When a debugger *releases* a slot — clears
  `dmode` — it should also move `action` away from 1.)

### tdata2 (0x7a2) — the compare value

The value matched against; `mcontrol`'s `select` bit decides what it is
compared with: the access **virtual address** (`select=0`), or the
loaded/stored **data value** or executed **instruction word**
(`select=1`; for data matches, "any bits beyond the size of the data
access will contain 0"). 1.0 additionally recommends byte-granular
address matching: "There is at least one compare value and it contains
the lowest virtual address of the access. It is recommended that there
are additional compare values for the other accessed virtual addresses"
— i.e. a 4-byte load from 0x4000 should also satisfy a trigger set on
0x4002. The register is optional if no implemented trigger type uses
it, and if XLEN is less than DXLEN, writes are sign-extended.

### tdata3 (0x7a3) — optional extra slot data

Trigger-specific extension data. `mcontrol` never uses it; the
self-hosted/tracing-oriented types do — icount/itrigger/etrigger read it
as `textra32`/`textra64` for context matching (ASID, virtual mode —
self-hosted tracing territory). A minimal RV32 implementation can omit
it; the CSR then reads as unimplemented.

### tinfo (0x7a4) — the capability report

Read-only bitmap (access R, reset preset): "One bit for each possible
type enumerated in tdata1. Bit N corresponds to type N. If the bit is
set, then that type is supported by the currently selected trigger." A
plain mcontrol slot therefore reads `info=0x4`. Two special cases: if the selected trigger
doesn't exist, `info` reads 1 (the enumeration exit); and the whole
register may be unimplemented "if type is not writable" — reading it
then raises an illegal-instruction exception, and the debugger falls
back to reading `tdata1.type`. 1.0 adds a `version` field [31:24]
(1 = ratified 1.0 semantics; 0 = the pre-ratification snapshot — see
[0.13.2 vs 1.0](#0132-vs-10)).

### tcontrol (0x7a5) — self-hosted gates

Optional and irrelevant to external debugging, but it explains a corner
of the design. `mte` (bit 3, R/W, reset 0) gates all `action=0`
triggers in M-mode:
"Triggers with action=0 do not match/fire while the hart is in M-mode".
On any trap into M-mode the hardware sets `mpte` (bit 7, R/W, reset 0)
← `mte` and clears `mte`; `mret` restores `mte` ← `mpte`. The point: a self-hosted
breakpoint (an `action=0` trigger raising `mcause=3`) taken in M-mode
must not re-fire endlessly inside the trap handler that services it.
`action=1` triggers are unaffected — Debug Mode entry is governed by the
debug side of the spec.

## mcontrol (type 2): the configuration word

With `tdata1.type=2`, the slot is an **mcontrol** trigger — the one type
0.13.2 silicon implements (§5.2.9). An implementation "may be limited to
address comparisons (`select` always 0) only. If that is the case, then
`tdata2` must be able to hold all valid virtual addresses" — the cheapest
useful profile, and exactly what the minimal recipe below builds. RV32
layout:

| Bits | Field | Access | Reset | Meaning |
|---|---|---|---|---|
| 31:28 | `type` | R/W | preset | Read as 2 (mcontrol) |
| 27 | `dmode` | R/W | 0 | Write guard, see above |
| 26:21 | `maskmax` | R | preset | Largest NAPOT range supported (`match=1`); 0 = NAPOT not supported |
| 20 | `hit` | R/W (optional) | 0 | Set by hardware when this trigger matches/fires — read-back to identify *which* trigger matched; software-settable/clearable. If unimplemented: reads 0, writes ignored |
| 19 | `select` | R/W | 0 | 0 = match **address** against `tdata2`; 1 = match **data** (load/store data, or the executed instruction word) |
| 18 | `timing` | R/W | 0 | 0 = "before", 1 = "after" — see [Timing](#timing-when-exactly-the-hart-halts) |
| 17:16 | `sizelo` | R/W | 0 | Match only accesses of this size (0 = any; 2 = 32-bit; 16-bit instructions when `execute`+`select=1`) |
| 15:12 | `action` | R/W | 0 | What to do on match — see [Actions](#actions-what-a-match-does) |
| 11 | `chain` | R/W | 0 | Tie this trigger to the next slot: both must match on the *same instruction* for the next one to fire |
| 10:7 | `match` | R/W | 0 | 0 = equal; 1 = NAPOT range; 2 = ≥; 3 = <; 4/5 = masked halves; 8+ = negations |
| 6 | `m` | R/W | 0 | Enable while in M-mode |
| 5 | — | R | 0 | Reserved, read 0 |
| 4 | `s` | R/W | 0 | Enable while in S-mode (hardwired 0 if no S-mode) |
| 3 | `u` | R/W | 0 | Enable while in U-mode (hardwired 0 if no U-mode) |
| 2 | `execute` | R/W | 0 | Match the address (or opcode) of executed instructions |
| 1 | `store` | R/W | 0 | Match the address (or data) of stores |
| 0 | `load` | R/W | 0 | Match the address (or data) of loads |

A trigger fires when, on one instruction/access: the privilege bits allow
the current mode, at least one class bit covers the access, and the
selected compare value satisfies the `match` relation against `tdata2`.
The field groups, in detail:

### What to match

- **Class bits `execute` [2] / `store` [1] / `load` [0]** — the spec's
  definitions: `execute` fires "on the virtual address or opcode of an
  instruction that is executed"; `store`/`load` "on the virtual address or
  data of any store/load". Combinations are the point: a single slot with
  `load=1`, `store=1`, `select=0`, `tdata2=A` matches *any* load or store
  at A — that one slot is GDB's `awatch`.
- **Privilege bits `m` [6] / `s` [4] / `u` [3]** — "When set, enable this
  trigger in M-/S-/U-mode." `s`/`u` are hardwired 0 if the hart doesn't
  have the mode — on the M-mode-only hart of the minimal profile only `m`
  exists, and forgetting to set it is the classic "trigger never fires"
  bug.
- **`select` [19]** — which value `tdata2` is compared with: 0 = virtual
  address of the access; 1 = "the data value loaded or stored, or the
  instruction executed". The `execute`+`select=1` combination matches the
  instruction *word* — break on an opcode pattern at any address (rarely
  implemented; usually `select` is hardwired 0, per the
  address-comparisons-only allowance above).
- **`sizelo` [17:16]** (with `sizehi` [22:21] existing only when
  XLEN > 32) — the combined `size` field filters by access width:
  0 = any size (well-defined only for address matches, or for data
  accesses of exactly XLEN bits), 1 = 8-bit, 2 = 16-bit accesses *or*
  16-bit instructions, 3 = 32-bit, … 9 = 128-bit. Reset value 0; "any"
  is the value every implementation must accept (1.0).
- **`match` [10:7]** — the comparison relation between the observed value
  and `tdata2`:

| `match` | Relation |
|---|---|
| 0 | equal |
| 1 | **NAPOT** range (only if `maskmax > 0`) |
| 2 | greater than or equal (unsigned) |
| 3 | less than (unsigned) |
| 4 | masked **lower** half |
| 5 | masked **upper** half |
| 8 / 9 / 12 / 13 | negation of 0 / 1 / 4 / 5 (**1.0 only**) |

  NAPOT (`match=1`) decoded: "M is XLEN-1 minus the index of the
  least-significant bit containing 0 in tdata2", and the top M bits of
  the observed value must equal the top M bits of `tdata2`. Worked
  example — watch a 64-byte-aligned buffer at 0x1000: `tdata2 =
  0x0000_101F` (low bits `0b01_1111`; least-significant 0 at bit 5, so
  M=26 → 2^6-byte granularity). The masked-halves modes are the cheaper
  range match: `match=4` compares the low 16 bits of the value against
  the low 16 bits of `tdata2`, after ANDing the former with the high 16
  bits of `tdata2`. All comparisons look at the low XLEN bits only
  (and, for data matches, only the low N bits of an N-bit access).

### What to do on a match

- **`action` [15:12]** — which of the
  [actions](#actions-what-a-match-does) the trigger takes on a fire.
- **`timing` [18]** — *when* relative to the matching instruction: the
  whole subject of [Timing](#timing-when-exactly-the-hart-halts).
- **`hit` [20]** — set by hardware when the trigger matches/fires
  (0.13.2: "when this trigger matches"; 1.0: must become set on fire, may
  on match), settable and clearable by software at any time. It is how
  the debugger learns *which* of several armed triggers stopped the
  hart. Optional — if unimplemented it reads 0 and writes are ignored,
  and identification falls to comparing every slot's compare value
  against `dpc`.
- **`maskmax` [26:21]** — read-only report of the largest NAPOT range
  `match=1` supports, expressed as log2 of the number of bytes; 0 means
  only exact (byte-granular) matching, i.e. NAPOT is effectively
  unsupported.

### Chaining triggers

`chain` [11] ties this slot to the next: "While this trigger does not
match, it prevents the trigger with the next index from matching." A
chain starts at the first slot with `chain=1` after (or at) a slot with
`chain=0`, and ends at the *next* slot with `chain=0` — that final slot
is part of the chain, the actions of all earlier slots are ignored, and
the final slot's action fires "if and only if all the triggers in the
chain match at the same time" — the same instruction/access. Three
fine-print rules from the spec:

- **Timing must agree**: "A chain of triggers that don't all have the
  same timing value will never fire (unless consecutive instructions
  match the appropriate triggers)."
- **`dmode` interlock**: because `chain` reaches into the *next* slot,
  "hardware must zero it in writes ... that set `dmode` to 0 if the next
  trigger has `dmode` of 1" — one side cannot chain into a slot claimed
  by the other.
- **Length cap**: implementations may limit chain length "to meet timing
  requirements" by zeroing `chain` on writes that would make the chain
  too long — read `chain` back after arming a chain.

Worked example — watch "the value 0xDEAD being *stored* to 0x2000_0400",
using two slots (slot 0 matches the address and gates slot 1, which
matches the data and takes the action):

```
tselect 0:  tdata2 = 0x2000_0400
tdata1   = 0x2800_0842      # type 2 | dmode | chain | m | store (action ignored)
tselect 1:  tdata2 = 0x0000_DEAD
tdata1   = 0x2808_1042      # type 2 | dmode | select(data) | action=1 | m | store
```

(Decoded: `chain` is bit 11 = 0x800; `select` is bit 19 = 0x8_0000.
Both slots use `timing=0` "before" — store data is known before the bus
write, so same-timing chains work, and the halt happens with the store
suppressed.)

### The WARL discipline

Everything above is WARL — the write-any-read-legal rule quoted in
[The trigger CSRs](#the-trigger-csrs). Practical consequences for the
debugger: probe once at connect (enumerate the slots, write each field
and read it back to learn `maskmax`, supported `match` modes, `timing`,
`select`, `sizelo`), and from then on program without re-reading — but
verify the final `tdata1` of every armed trigger anyway, since the
spec's own fallback is that an unsupported configuration reads back
as "simply a disabled trigger".

## Timing: when exactly the hart halts

The subtlest part of the spec, and the part every implementer gets
questions about. `timing` (bit 18) selects one of two reference points:

- **`timing=0`, "before":** the action is taken *just before the matching
  instruction is retired*, but after all preceding instructions are
  retired — `dpc` must be set to the virtual address of the **matching
  instruction**. It has not executed (0.13.2 wording: "the instruction
  that triggered it"; 1.0 says "retired", same practical meaning).
- **`timing=1`, "after":** the action is taken *after the matching
  instruction is retired* — `dpc` must be set to the virtual address of the
  **next instruction to execute**.

Two spec freedoms shape what a real hart may do:

1. *"Execute, load, and store address/data triggers may fire at whatever
   point in time is most convenient for the implementation."* Most
   hardware implements exactly one timing and `timing` mostly serves to
   *tell the debugger* which one. And "after" may even be imprecise: "it
   should be taken before the next instruction is retired, but it is
   better to implement triggers imprecisely than to not implement them at
   all."
2. Multi-memory-access instructions: "it is UNSPECIFIED which memory
   accesses have completed before the trigger fires." Irrelevant for plain
   RV32 `lw`/`sw` (one access each); bites vector extension parts.

### Case by case

| Trigger | With "before" timing: halted | With "after" timing: halted | Suggested |
|---|---|---|---|
| **execute**, any opcode | instruction **not executed**, `dpc` = its address | executed, `dpc` = next instruction | before |
| **execute** on a `lw`/`sw` | same — the data access **has not happened on the bus** | access complete, `dpc` = next | before |
| **store address** | store **not yet on the bus** — write suppressed, happens once on resume | store committed | before |
| **store data** | data known from the register file before the bus write — fires **before the write goes out** | store committed | before |
| **load address** | match decidable at address-generation; hardware cancels the read (it then happens once, on resume) | read done, register updated | before |
| **load data** | **cannot be evaluated before the bus read returns** — see the trap below | read done, register updated | **after** |

**The load-data trap.** For a load-*data* trigger the compare value only
exists after the bus read completes. The spec warns that with `timing=0`,
"a memory access will be performed (including any side effects of
performing such an access) even though the load will not update its
destination register" — and since `dpc` points at the load, resume
re-executes it: the bus is read **twice**. Hence the explicit debugger
rule: *"For data load triggers, debuggers must first attempt to set the
breakpoint with `timing=1`."* On memory-mapped I/O, a before-timing
load-data trigger is simply dangerous; an after-timing one performs the
read once.

The engineering summary: with "before" triggers, `dpc` points at the
matching instruction, nothing it would have done has happened, and resume
does it — exactly like a re-routed `ebreak` at `dpc`. With "after"
triggers, the instruction's effects (including committed stores) are
complete and `dpc` points past it. If you implement only "before" for
execute/load-address/store-address/data — the intuitive behavior — you
match the spec's suggestions for everything except load-data triggers,
which you can leave unsupported.

## Actions: what a match does

The `action` field (bits 15:12) selects the trigger's effect on a match:

| action | Effect |
|---|---|
| 0 | Raise an ordinary **breakpoint exception** (`mcause=3`, `xepc` = the address execution must continue at). Self-hosted debug — software using triggers without any debugger attached; invisible to the DM |
| 1 | **Enter Debug Mode** with `dcsr.cause=2`. The external-debugger action |
| 2/3/4 | Trace on / off / notify — defined in the trace specification (0.13.2 merely reserves 2–5 "for use by the trace specification") |
| 8/9 | Pulse the TM's external trigger outputs 0/1 (custom event counters, SoC wiring) — 1.0 |

Two rules around `action=1`:

- Both versions restrict it: `action=1` is "only supported when the
  trigger's `dmode` is 1" (0.13.2) — a trigger that can pull the hart
  into Debug Mode must not be rewritable by running software. 1.0 adds
  that since `tdata1` is WARL, hardware must refuse a `dmode=0` +
  `action=1` combination outright. Debuggers set `dmode=1` anyway when
  arming.
- A trigger with `action=1` needs the Debug Mode entry path (the Sdext
  side of the house — Debug Mode, `dcsr`/`dpc`, halt entry) to exist;
  `action=0` triggers work on a hart that has no external debug support
  at all.

## The halt and the aftermath: dcsr.cause, hit, re-firing

When an `action=1` trigger fires:

1. The hart enters Debug Mode through the halt-entry OR, with
   `dcsr.cause=2` (the value `debug-mode.md`'s entry-cause table lists as
   "trigger (breakpoint/watchpoint) fired").
2. `dpc` follows the timing rules above — the matching instruction for
   "before", the next instruction for "after".
3. The trigger's `hit` bit (bit 20 of `tdata1`) becomes set. The
   debugger reads `tdata1` back to see *which* trigger matched when
   several are armed, and writes 0 to the bit to re-arm it.
4. **The trigger stays armed.** A "before" trigger matches again on
   resume — the matching instruction sits at `dpc` and the comparator is
   unchanged — so the hart re-halts immediately, the exact analog of a
   patched `ebreak` at `dpc`. To continue past it, the debugger steps
   *over* it: disable the trigger (or its privilege bits), single-step,
   re-enable, resume — see [the sequence](#sequences-over-dmi) below.

## Enumerating the triggers

The spec gives an exact discovery loop (1.0 wording; 0.13.2 works the
same in practice):

1. Write 0 to `tselect`. If that raises an illegal instruction exception,
   **no trigger module exists** — stop (this is the probe OpenOCD performs;
   on the minimal DM it ends here and ebreak patching takes over).
2. Read `tselect` back; if it differs from what you wrote, you've run past
   the last slot — exit the loop.
3. Read `tinfo`. On exception (or if the hardware omits `tinfo`), read
   `tdata1` instead and take the type from its `type` field; `type=0`
   means no trigger at this index — exit the loop. If `tinfo.info=1`,
   same conclusion.
4. `tinfo.info` is a bitmap: bit N set = the selected slot supports
   trigger type N (bit 2 = mcontrol, bit 6 = mcontrol6 in 1.0).
5. Increment `tselect` and repeat.

The read-back in step 2 exists so implementations with 2^n slots only
need n bits of `tselect`; checking `tinfo`/`tdata1.type` catches
implementations with more `tselect` bits than slots.

## Sequences over DMI

Same notation as [debug-sequences.md](debug-sequences.md) (symbolic DM
register names; every abstract command is `0x0022_0000|regno` for a read,
`0x0023_0000|regno` for a write after loading `data0`; poll
`abstractcs` after each). All sequences assume: `dmactive=1`, hart
halted, `haltreq` cleared. Trigger CSR regnos = CSR addresses: `tselect`
0x7a0, `tdata1` 0x7a1, `tdata2` 0x7a2, `tinfo` 0x7a4.

### Arm a hardware breakpoint at 0x0000_1000 (M-mode code)

```
dmi write data0   <- 0x00000000        # tselect <- 0 (first slot)
dmi write command <- 0x0023_07A0
poll abstractcs until busy==0 && cmderr==0

dmi write data0   <- 0x00000000        # tdata1 <- 0: disable slot 0 first
dmi write command <- 0x0023_07A1        # (safe order — compare values are
poll abstractcs until busy==0 && cmderr==0   #  only guaranteed on a disabled slot)

dmi write data0   <- 0x00001000        # tdata2 <- the address to match
dmi write command <- 0x0023_07A2
poll abstractcs until busy==0 && cmderr==0

dmi write data0   <- 0x2800_1044        # tdata1: type=2 | dmode=1
dmi write command <- 0x0023_07A1        #   | action=1 | m=1 | execute=1
poll abstractcs until busy==0 && cmderr==0
                                          #   (timing=0 "before", select=0
                                          #    address, match=0 equal,
                                          #    sizelo=0 any — the plain
                                          #    execute-address breakpoint)

# read back what WARL accepted — mandatory, not optional
dmi write command <- 0x0022_07A1
poll abstractcs until busy==0 && cmderr==0
dmi read  data0   -> 0x2800_1044        # if bits vanished, plan around it

dmi write dmcontrol <- 0x4000_0001      # resume and wait for the fire
poll dmstatus until allresumeack
poll dmstatus until allhalted            # ...typically microseconds later
```

The `mcontrol` word decomposed: `0x2000_0000` type=2, `0x0800_0000`
`dmode=1`, `0x1000` `action=1` (Debug Mode), `0x40` `m=1`, `0x04`
`execute=1`.

### What the halt looks like

```
dmi write command <- 0x0022_07B0        # read dcsr
poll abstractcs until busy==0 && cmderr==0
dmi read  data0   -> ... 0x080 ...      # dcsr.cause=2 (trigger)

dmi write command <- 0x0022_07A1        # read tdata1
poll abstractcs until busy==0 && cmderr==0
dmi read  data0   -> 0x2810_1044        # hit (bit 20) set: slot 0 fired

dmi write command <- 0x0022_07B1        # read dpc
poll abstractcs until busy==0 && cmderr==0
dmi read  data0   -> 0x0000_1000        # the breakpointed instruction
                                          # (timing=0: not yet executed)
```

Clear `hit` by writing `tdata1` back with bit 20 zeroed (the enable bits
stay as they are).

### Arm a watchpoint: writes to 0x2000_0400

Same skeleton; only the configuration words differ. GDB's three kinds map
onto the load/store/execute-class bits:

| GDB | `tdata1` bits set (besides type/dmode/action) | `tdata2` |
|---|---|---|
| `watch` (write) | `m` + `store` → `0x2800_1042` | watched address |
| `rwatch` (read) | `m` + `load` → `0x2800_1040` | watched address |
| `awatch` (access) | `m` + `load` + `store` → `0x2800_1043` | watched address |

(Add `s`/`u` bits to match in those modes; leave `sizelo=0` "any size" so
byte/halfword/word writes all match.)

### Step over the breakpoint and continue

The "before" trigger re-fires if simply resumed, so:

```
dmi write data0   <- 0x2800_1000        # tdata1: same slot, but m=0 —
dmi write command <- 0x0023_07A1        # trigger disabled (privilege bits 0)
poll abstractcs until busy==0 && cmderr==0

dmi write data0   <- (dcsr | 0x4)       # dcsr.step=1: single-step the
dmi write command <- 0x0023_07B0        # instruction at dpc (the one
poll abstractcs until busy==0 && cmderr==0
dmi write dmcontrol <- 0x4000_0001
poll dmstatus until allresumeack
poll dmstatus until allhalted           # cause=4, dpc = next instruction

dmi write data0   <- (dcsr & ~0x4)      # clear dcsr.step (keep the other
                                         # dcsr bits intact)
dmi write command <- 0x0023_07B0
poll abstractcs until busy==0 && cmderr==0

dmi write data0   <- 0x2800_1044        # tdata1: re-arm the trigger
dmi write command <- 0x0023_07A1
poll abstractcs until busy==0 && cmderr==0

dmi write dmcontrol <- 0x4000_0001      # continue
poll dmstatus until allresumeack
```

This mirrors the software-breakpoint step-over in
[debug-sequences.md](debug-sequences.md) — there the ebreak instruction is
patched back to the original word for the step and re-patched after,
here the trigger is disabled for the step and re-armed after.

### Remove a trigger

Write the slot's `tdata1` with all privilege bits (or class bits) zero —
`0x2800_1000` as above — or write `tdata1=0` (`type=0`) to fully disable
the slot. Then resume. GDB does exactly this when a breakpoint is deleted.

## What a minimal RV32 implementation costs

For the minimal profile, the practical recipe is **one trigger slot,
address compare only**:

- **CSRs:** `tselect` (1 bit), `tdata1` (read-back + WARL), `tdata2`
  (compare address), and ideally `tinfo` (return `0x0004` = type 2). All
  reached through the CSR path the abstract commands already use — if your
  Access Register accepts all implemented CSRs (minimal-dm's option (b)),
  this is a pure routing addition, no new abstract machinery.
- **Comparator:** one 32-bit equality compare against the fetch/PC address
  (execute) and the LSU effective address (load/store), qualified by the
  three class bits and the privilege bits. Place it wherever is
  convenient — the timing bit then just reports the placement.
- **Action:** support `action=0` and `action=1`; OR the `action=1` fire
  into the halt-entry logic next to `halt_req`.
- **Set `timing=0` hardwired** for everything you support. Report it as
  such (WARL reads back 0).
- **What to leave out** — debuggers probe and adapt: `select=1` (data
  compare), `match≠0` (NAPOT/ranges — read `maskmax=0`), `chain`,
  `sizelo≠0`, `tdata3`, `tcontrol`. One slot means GDB gets exactly one
  hardware breakpoint *or* watchpoint at a time; with writable RAM it
  uses software breakpoints by default anyway and saves the slot for
  flash addresses and `watch` commands.

If your Access Register path only implements dcsr/dpc plus GPRs (option
(c) of the minimal profile), extend it to the four trigger CSRs first —
without CSR access there is no way to program a trigger.

## 0.13.2 vs 1.0

The trigger chapter was restructured between the versions, but deployed
silicon (OpenTitan `rv_dm` with Ibex, SiFive parts) is 0.13.2-style:

| | 0.13.2 | 1.0 |
|---|---|---|
| Where it lives | chapter 5 of the debug spec itself | split out as the standalone **Sdtrig** ISA extension; the rest of the debug spec no longer defines triggers |
| Trigger type | **type 2 `mcontrol`** — the only one used in practice | type 2 kept but **deprecated**; new **type 6 `mcontrol6`** preferred |
| `timing` | bit 18 of `mcontrol`, writable request | unchanged for type 2; `mcontrol6` drops it — the 2-bit `hit` field (`hit1`:`hit0`) instead *reports* how it fired: 0=false, 1=before, 2=after, 3=immediately-after |
| `mcontrol6` extras | — | `uncertain`/`uncertainen` (matches the TM cannot fully observe, e.g. far AMOs), `vs`/`vu` (V-S/V-U mode enables), 3-bit `size`, `tinfo.version` field |
| `action=1` | "only supported when the trigger's `dmode` is 1" | same requirement, made explicit: WARL must reject `dmode=0`+`action=1`; also requires Sdext on the hart |
| `match` | 0–5 defined; other values reserved | adds the negations 8 (≠), 9 (not NAPOT), 12/13 (masked-halves negations) |
| Context filtering | none | optional `textra32`/`textra64`, `scontext`/`hcontext` — filter triggers by ASID/VMID/context (self-hosted tracing use) |
| Timing wording | "before/after the instruction is executed" | "before/after the instruction is **retired**" + explicit allowance for imprecision |

For a 0.13.2-targeting implementation none of the 1.0 changes matter —
but write `tdata1.type=2` (not 6) and expect the debugger to behave
exactly as the sequences above.

## Related documents

| Document | What it covers relative to this one |
|---|---|
| [debug-mode.md](debug-mode.md) | The halt-entry machinery a trigger's `action=1` feeds into: Debug Mode, `dcsr.cause`, `dpc`, the DM↔hart interface — and its entry-cause table where `cause=2` is the trigger |
| [minimal-dm.md](minimal-dm.md) | The DM this extends — the add-next list where the trigger module is item #1, and the CSR-access path (option (b)) that makes programming triggers free |
| [debug-sequences.md](debug-sequences.md) | The DMI listing conventions used here, and the software-breakpoint (ebreak patching) flows the trigger module complements |
| [abstract-commands.md](abstract-commands.md) | The Access Register command used to reach the trigger CSRs, and `cmderr=3` as the "no such CSR" signal the enumeration loop relies on |
| [toolchain-integration.md](toolchain-integration.md) | The debugger side: OpenOCD's trigger inventory probe at `init`, GDB `Z1`/`Z2`/`Z3`/`Z4` packets mapping onto `execute`/`store`/`load` bits |
| [debug-architecture.md](debug-architecture.md) | The layered picture — the trigger module's place at layer 5 and its (non-)relationship to the DM |
