# Debug Sequences for the Minimal Debug Module

This document walks through the usual debugger workflows — connect,
halt, inspect, step, software breakpoints, resume, reset, detach, error
recovery — as concrete DMI operations on the minimal DM register set.

It assumes exactly the register set described in
[minimal-dm.md](minimal-dm.md): single-hart RV32, `data0`/`data1`,
`dmcontrol`, `dmstatus`, `abstractcs`, `command` — no trigger module, no
Program Buffer, no System Bus Access. Register/bit reference:
[minimal-dm.md](minimal-dm.md); the transport underneath (JTAG scans,
`dmi` op protocol, busy handling):
[jtag-dtm.md](jtag-dtm.md); overall architecture:
[debug-architecture.md](debug-architecture.md).

All register write values below are spelled out in hex, so the sequences
can be used directly as test vectors against RTL or a driver.

## Table of contents

- [Conventions and notation](#conventions-and-notation)
- [Connect and discovery](#connect-and-discovery)
- [Halting the hart](#halting-the-hart)
- [Reading the halt reason and core state](#reading-the-halt-reason-and-core-state)
- [Accessing CPU registers (GPRs)](#accessing-cpu-registers-gprs)
  - [Accessing CSRs](#accessing-csrs)
- [Memory access](#memory-access)
  - [Sequential memory read/write (block transfers)](#sequential-memory-readwrite-block-transfers)
  - [Auto-increment bursts (optional DM extension)](#auto-increment-bursts-optional-dm-extension)
- [Resuming](#resuming)
- [Single stepping](#single-stepping)
- [Software breakpoints](#software-breakpoints)
- [Reset and debug from reset](#reset-and-debug-from-reset)
- [Detaching](#detaching)
- [Error recovery](#error-recovery)
- [What this configuration cannot do](#what-this-configuration-cannot-do)
- [Cost cheat sheet](#cost-cheat-sheet)
- [References](#references)

## Conventions and notation

```
dmi write <addr> <- <value>     one DMI write operation
dmi read  <addr> -> <value>     one DMI read operation
poll <addr> until <cond>        repeat the read until the condition holds
```

- Every `dmi read/write` is one DMI operation = two JTAG `dmi` scans
  (request + result), roughly 95 TCK cycles plus the Run-Test/Idle dwell
  reported in `dtmcs.idle` — see [jtag-dtm.md](jtag-dtm.md). Real
  debuggers batch many scans per JTAG transaction.
- Reads of `dmstatus` and `abstractcs` are side-effect-free — safe to
  poll in a loop.
- While `abstractcs.busy` is set, never read/write `data0`/`data1` and
  never write `command`/`abstractcs` — that would latch `cmderr=1`.
  Poll `abstractcs`, nothing else.
- While an abstract command executes, `haltreq`, `resumereq` and
  `ackhavereset` must all be 0 (spec rule).
- The `dmi` operations below use **symbolic DM register names** instead of
  hex addresses — `dmi write dmcontrol <- ...` rather than
  `dmi write 0x10 <- ...`. The names are the ones the spec uses; the
  addresses they stand for (see [minimal-dm.md](minimal-dm.md)):

| Name | DMI address | Name | DMI address |
|---|---|---|---|
| `data0` | 0x04 | `data1` | 0x05 |
| `dmcontrol` | 0x10 | `dmstatus` | 0x11 |
| `abstractcs` | 0x16 | `command` | 0x17 |
| `abstractauto` | 0x18 | | |

  (`abstractauto` exists only with the optional burst extension.)

Handy `dmstatus` bit masks:

| Condition | Bit | Mask |
|---|---|---|
| allrunning | 11 | 0x800 |
| anyrunning | 10 | 0x400 |
| allhalted | 9 | 0x200 |
| anyhalted | 8 | 0x100 |
| allunavail | 13 | 0x2000 |
| anynonexistent | 14 | 0x4000 |
| allresumeack | 17 | 0x20000 |
| authenticated | 7 | 0x80 |
| version (0.13) | 3:0 | 0x2 |

Useful `abstractcs` masks: `busy` = 0x1000 (bit 12), `cmderr` = 0x700
(bits 10:8).

## Connect and discovery

Bring up the transport, then the DM, then learn what exists:

```
1. JTAG: TAP reset (TRST* or 5 x TMS=1)
   -> IR defaults to IDCODE; scan IDCODE to identify the device
2. JTAG: select dtmcs, scan ->
   version = 1, abits, idle          # learn scan width + dwell
3. JTAG: select IR = 0x11 (dmi)      # stays selected from here on
4. dmi write dmcontrol <- 0x0000_0001     # dmcontrol: dmactive=1 (DM out of reset)
5. dmi read  dmstatus -> dmstatus        # expect version=2, authenticated=1,
                                      # and allrunning or allhalted
```

Then probe the capability bits — everything absent reads 0:

```
dmi read abstractcs -> abstractcs          # progbufsize=0, datacount=2
dmi read hartinfo   -> hartinfo = 0        # no debug ROM shadowing
dmi read sbcs       -> sbcs = 0            # no System Bus Access
                                           # (hartinfo/sbcs are at DMI
                                           #  addresses 0x12/0x38 — probed,
                                           #  but outside the minimal set)
dmi write dmcontrol <- 0x07FF_FFC1        # feature probe: hasel (bit 26) | all
                                           # 20 hartsel bits (25:16, 15:6) |
                                           # dmactive — request bits stay 0
                                           # (exactly what OpenOCD writes)
dmi read  dmcontrol -> dmcontrol           # read back: hasel and hartsel bits
                                           # read 0 => no hart array, HARTSELLEN=0
                                           # => single hart
```

From here on, `dmcontrol` writes always carry `dmactive=1` (bit 0), or
the DM forgets everything.

## Halting the hart

```
dmi write dmcontrol <- 0x8000_0001        # 0x8000_0000 = haltreq (bit 31)
                                           # | 0x1 = dmactive
poll dmstatus until (v & 0x200) != 0      # 0x200 = allhalted (bit 9)
```

- `haltreq` may stay 1 after the halt (it re-catches a hart that comes
  out of reset), or be cleared (write just `0x0000_0001` = `dmactive`).
  Before starting abstract commands it must be 0.
- A hart halted this way reports `dcsr.cause = 3` (halt request).

If the hart never halts, check why before retrying:
`(dmstatus & 0x4000)` → nonexistent (bad index), `(dmstatus & 0x2000)` →
unavailable (in reset — see [Reset and debug from reset](#reset-and-debug-from-reset)).

## Reading the halt reason and core state

Once halted, read `dcsr` and `dpc` through Access Register commands:

```
dmi write command <- 0x0022_07B0       # command: Access Register, aarsize=2
                                     # (32-bit), transfer, regno=0x7b0 (dcsr)
poll abstractcs until (v & 0x1000) == 0   # abstractcs.busy cleared
                                     # (also check cmderr == 0: (v & 0x700) == 0)
dmi read  data0 -> dcsr               # data0 holds the value

dmi write command <- 0x0022_07B1       # same, regno=0x7b1 (dpc)
poll abstractcs until (v & 0x1000) == 0
dmi read  data0 -> dpc
```

`dcsr.cause` (`(dcsr >> 6) & 0x7`) tells you why the hart is in Debug
Mode:

| cause | Meaning | Reachable with this DM? |
|---|---|---|
| 1 | `ebreak` executed | yes — a software breakpoint hit |
| 2 | trigger fired | no (no trigger module) |
| 3 | halt request | yes — your own `haltreq` |
| 4 | single step completed | yes |
| 5 | resethaltreq out of reset | no (not implemented) |

Save `dcsr` when you attach — its control bits (`ebreakm/s/u`, `step`)
are what you restore on detach. Do **not** restore `dpc` on detach: it
is where the program resumes. Re-read `dcsr` after every halt (its
`cause` and `prv` change) and modify that current value, not the attach
copy. With Access Register, `dscratch` is not needed (that is a Program
Buffer thing).

## Accessing CPU registers (GPRs)

The GPRs x0–x31 live at abstract-command `regno` values 0x1000–0x101f —
the regno is just `0x1000 | N` for xN. The command word is always the
same shape; only the regno field and the write bit change:

```
bits:        31:24    23  22:20    19       18        17        16     15:0
field:       cmdtype  0   aarsize  aarpost  postexec  transfer  write  regno
read  xN:    0x00     0   2        0        0         1         0      0x1000+N
write xN:    0x00     0   2        0        0         1         1      0x1000+N
                          (2=32b)  (0=off)  (0=off)   (1=do)           (0x1000|N)
```

As a formula: `command = 0x0022_0000 | (0x1000 | N)` to read,
`command = 0x0023_0000 | (0x1000 | N)` to write.

Reading a GPR — here x10 (regno 0x100A):

```
dmi write command <- 0x0022_100A       # command: Access Register,
                                     # aarsize=2, transfer=1, write=0, regno=0x100A
poll abstractcs until busy==0 && cmderr==0
                                     # cmderr==0 → the transfer happened
dmi read  data0 -> value              # data0 = x10
```

Writing a GPR — the argument goes to `data0` **before** the command
(the DM latches `data0` when the command starts):

```
dmi write data0 <- value              # data0 = the value for x10
dmi write command <- 0x0023_100A       # command: Access Register,
                                     # aarsize=2, transfer=1, write=1, regno=0x100A
poll abstractcs until busy==0 && cmderr==0
```

Full worked example — GPR register dump (the classic debugger `reg`
command), halted hart:

```
for N in 0 .. 31:
    dmi write command <- 0x0022_1000 | N        # Access Register, read xN
    poll abstractcs until busy==0 && cmderr==0
    dmi read  data0 -> regs[N]               # data0
```

That is 32 identical iterations — one command + one poll + one data0
read each. x0 always reads 0 (it is the zero register; a write to it is
silently dropped by the hart, not an error).

### Accessing CSRs

CSRs use the **same command shape**, just a different regno: the CSR's
address directly (0x000–0xfff). No offset is added:

```
read  csr:  command = 0x0022_0000 | csr_addr     # e.g. dcsr: 0x0022_07B0
write csr:  data0 = value
             command = 0x0023_0000 | csr_addr
```

Which regno ranges mean what (spec Table 3.3):

| regno range | Meaning |
|---|---|
| 0x0000–0x0fff | CSRs by address — including dcsr (0x7b0), dpc (0x7b1), dscratch0 (0x7b2) |
| 0x1000–0x101f | GPRs x0–x31 |
| 0x1020–0x103f | FPRs f0–f31 (none on this RV32 without F) |
| 0xc000–0xffff | custom registers (vendor-specific) |

Useful CSRs to read while halted:

| Register | regno | Why |
|---|---|---|
| dcsr | 0x7b0 | halt reason (`cause`), `step`, ebreak routing (`ebreakm`) |
| dpc | 0x7b1 | where the hart will resume |
| misa | 0x301 | detect the C extension (bit 2) — needed for breakpoints |
| mstatus | 0x300 | privilege state (optional context) |
| mcause / mepc / mtval | 0x342 / 0x341 / 0x343 | exception context |

A CSR that the hart does not implement answers `cmderr=3`
(exception) — the debugger marks it "nonexistent" and moves on (that
is exactly how OpenOCD's CSR probing works).

## Memory access

One load/store through the halted hart, address in `data1`, data in
`data0`:

```
# read a word:
dmi write data1 <- addr               # data1 = address
dmi write command <- 0x0220_0000        # Access Memory, aamsize=2 (32-bit), read
poll abstractcs until busy==0 && cmderr==0
dmi read  data0 -> word               # data0 = value

# write a word:
dmi write data0 <- value              # data0 = value first
dmi write data1 <- addr               # data1 = address
dmi write command <- 0x0221_0000        # ... + write bit
poll abstractcs until busy==0 && cmderr==0
```

Other access sizes (byte/halfword command words and strides) are listed
with the block-transfer sequences in
[Sequential memory read/write](#sequential-memory-readwrite-block-transfers)
below. The DM must reject `aamsize=3/4` (64/128-bit) with `cmderr=2`,
just as it rejects `aarsize=3/4` in Access Register (that is the field
the debugger probes for XLEN).

Note the access goes through the hart's own LSU — it sees exactly what
M-mode code would see (PMP, regions, MMIO side effects). A load fault
surfaces as `cmderr=5` (bus) or 3 (exception).

### Sequential memory read/write (block transfers)

There is no auto-increment and no SBA in this DM, so a block transfer is
an explicit loop: every iteration re-writes the address into `data1`,
then re-issues the same command word. The debugger keeps the address
arithmetic; the DM just does one access per command.

**Reading `count` words starting at `addr`:**

```
for i in 0 .. count-1:
    dmi write data1 <- addr + 4*i           # data1 = address of word i
    dmi write command <- 0x0220_0000         # Access Memory, 32-bit, read
    poll abstractcs until busy==0 && cmderr==0
    dmi read  data0 -> buf[i]               # data0 = word i
```

**Writing `count` words starting at `addr`:**

```
for i in 0 .. count-1:
    dmi write data0 <- buf[i]               # data0 = value first
    dmi write data1 <- addr + 4*i           # data1 = address
    dmi write command <- 0x0221_0000         # Access Memory, 32-bit, write
    poll abstractcs until busy==0 && cmderr==0
```

Command words for all sizes (`write` is bit 16):

| Access | aamsize | Command word (read / write) | Stride |
|---|---|---|---|
| byte | 0 | `0x0200_0000` / `0x0201_0000` | +1 |
| halfword | 1 | `0x0210_0000` / `0x0211_0000` | +2 |
| word | 2 | `0x0220_0000` / `0x0221_0000` | +4 |

(64/128-bit variants exist in the encoding but must be rejected with
`cmderr=2` on RV32.)

**Cost.** One word = 4 DMI ops (read: `data1` + `command` + poll +
`data0`; write: `data0` + `data1` + `command` + poll), so `count` words ≈ `4·count` DMI ops ≈ `40·count` µs at
10 MHz TCK before batching. A 1 KiB read (256 words) is ~1024 DMI ops.
Compare the alternatives (see "What to add next" in
[minimal-dm.md](minimal-dm.md)): SBA with `sbautoincrement` turns the
same block into `count` accesses of `sbdata0` plus one `sbaddress0` write,
and `aampostincrement`/`abstractauto` remove the per-word `data1`
rewrites — the loop structure stays, the constants get better. This is
why block transfers, not stepping, dominate optimization efforts.

**Fault handling mid-block.** If a word faults (`cmderr=3/5`), the
loop aborts with the partial data in `buf` — poll `cmderr` **every
iteration**, do not batch a whole block blindly, unless a partial read
is acceptable.

**Flushing.** After a write block into instruction memory (e.g.
patching a breakpoint), issue a `fence` before expecting the hart to
fetch it, unless the platform is coherent. On this DM there is no
progbuf to run a `fence` with — the hart itself executes one after
resume; on strongly-ordered/small RV32 systems without caches this is
usually a non-issue.

### Auto-increment bursts (optional DM extension)

Yes — there is an auto-address-increment way, but it needs **two
optional features** that the minimal DM as specified above does *not*
include (see [minimal-dm.md](minimal-dm.md), "Optional burst
extension"):

1. **`aampostincrement`** — Access Memory command bit 19
   (`0x0008_0000`): after each *successful* access, the DM increments
   `data1` by the access size (1/2/4 for aamsize 0/1/2). No increment
   on failure.
2. **`abstractauto` (DMI 0x18)** — auto-execution: when bit k of
   `autoexecdata` (bits 11:0) is set, any read *or write* of the
   corresponding `dataN` re-executes the last `command`.

```
abstractauto (0x18):
[31:16]  autoexecprogbuf   (no progbuf here: leave 0)
[11:0]   autoexecdata      bit 0 -> data0
```

`aampostincrement` alone removes the per-word `data1` rewrite;
`abstractauto` additionally removes the per-word `command` rewrite —
together one DMI op per word.

**Read burst** (`aampostincrement` + `abstractauto`):

```
# setup:
dmi write data1 <- addr               # data1 = start address
dmi write command <- 0x0228_0000        # Access Memory, 32-bit, read,
                                     # aampostincrement=1 -> word 0 in data0
poll abstractcs until busy==0 && cmderr==0
dmi write abstractauto <- 0x0000_0001        # bit 0 = autoexecdata[0]: any
                                           # data0 access re-runs command
# burst: each data0 read returns one word AND triggers the next access:
for i in 0 .. count-2:
    dmi read  data0 -> buf[i]          # = word i; triggers access i+1
dmi write abstractauto <- 0x0000_0000        # disable autoexec (no extra access)
poll abstractcs until busy==0 && cmderr==0
dmi read  data0 -> buf[count-1]       # last word, no trigger
```

**Write burst** (same features — note `data0` must hold `buf[0]`
*before* the command write, because writing `command` starts the first
execution immediately):

```
# setup:
dmi write data1 <- addr               # data1 = start address
dmi write data0 <- buf[0]             # data0 = first value
dmi write command <- 0x0229_0000        # Access Memory, 32-bit, write,
                                     # aampostincrement=1 -> stores buf[0]
poll abstractcs until busy==0 && cmderr==0
dmi write abstractauto <- 0x0000_0001        # autoexec on data0
# burst: each data0 write stores one word AND triggers the next:
for i in 1 .. count-1:
    dmi write data0 <- buf[i]
dmi write abstractauto <- 0x0000_0000        # disable autoexec
poll abstractcs until busy==0 && cmderr==0
```

**Intermediate variant** — `aampostincrement` only (no `abstractauto`):
per word write `command` (`0x0228_0000`/`0x0229_0000`), poll, then
read/write `data0`; `data1` is written once. Saves one op per word.

| Variant | Read ops/word | Write ops/word |
|---|---|---|
| plain loop (above) | 4 | 4 |
| + `aampostincrement` | 3 | 3 |
| + `abstractauto` | 1 | 1 |

**Why bursts are safe without per-word polls.** The sticky `cmderr`
gates execution: once a word faults (`cmderr=3`/`5` latched), no
further auto-triggered command starts — the burst just fizzles out,
and one `abstractcs` poll at the end reveals the failure position
(`data1` points at the faulting address). Clear with `0x700`-style
writes and replay from there. (Do keep `count` bounded — the whole
point of `abstractauto` is that the debugger does *not* check between
words.)

**Real-world use.** OpenOCD probes `aampostincrement` first (command
with bit 19, fall back to per-word `data1` writes if it reads back as
not-supported) and uses the `abstractauto.autoexecdata` pipeline for
its progbuf-based reads; both mechanisms are exercised by its batch
mode.

## Resuming

```
dmi write dmcontrol <- 0x4000_0001        # 0x4000_0000 = resumereq (bit 30)
                                           # | 0x1 = dmactive
poll dmstatus until (v & 0x20000) != 0    # 0x20000 = allresumeack (bit 17)
```

`resumereq` is a one-shot (write 1 = resume once). `resumeack` is set by
the hart and cleared by the next `resumereq` — poll it, not `allhalted`:
a hart with a breakpoint at the resume point halts again immediately,
and `allhalted` would mislead you into thinking it never resumed.

## Single stepping

```
dmi write data0 <- (dcsr | 0x4)           # 0x4 = dcsr.step (bit 2); OR it
                                           # into the current dcsr (read
                                           # after this halt) to keep
                                           # prv/ebreak bits intact
dmi write command <- 0x0023_07B0         # Access Register, write dcsr
                                           # (regno 0x7B0)
poll abstractcs until busy==0 && cmderr==0

dmi write dmcontrol <- 0x4000_0001             # resumereq
poll dmstatus until allresumeack              # it did resume...
poll dmstatus until allhalted                 # ...and re-halted after ONE instruction
                                          # dcsr.cause = 4, dpc advanced
```

Then clear `step` (write `dcsr & ~0x4`) before the next plain resume,
or the hart will single-step forever. With `stepie=0`
(reset default) interrupts are masked during the step — deterministic
stepping; the pending interrupt is serviced after the next resume.

`dpc` after the step is the address of the *next* instruction to
execute (pc + 4 for straight-line code, the branch target for taken
branches) — not the instruction just executed.

## Software breakpoints

There is no trigger module, so breakpoints are patched instructions:
replace the instruction word at address A with `ebreak`
(`0x0010_0073`) or, for compressed 16-bit instructions, `c.ebreak`
(`0x9002`). The debugger keeps a table `A -> (original word, length)`.

First check whether compressed instructions exist:
`(misa >> 2) & 1` (misa regno 0x301). With C, instructions are only
2-byte aligned: for an `A` with `A % 4 == 2`, read the original with a
16-bit access (`0x0210_0000`) first, then read the second halfword at
`A+2` only if `(w0 & 0x3) == 0x3`.

The [installing](#installing-always-while-halted) flow below shows the
word-aligned case (`A % 4 == 0`), where a single 32-bit read captures the
whole instruction. For `A % 4 == 2` slots, use the two 16-bit halfword
reads above instead — [src/sequences.cpp](../src/sequences.cpp) saves both
halfwords this way, so the restore and the single step past the hit
reassemble the full instruction.

### Installing (always while halted)

```
# read the original instruction word at A (see Memory access):
dmi write data1 <- A
dmi write command <- 0x0220_0000
poll abstractcs ...
dmi read data0 -> w0                  # save w0 in the breakpoint table

if (w0 & 0x3) == 0x3:                 # 32-bit instruction
    dmi write data0 <- 0x0010_0073    # ebreak = SYSTEM opcode (0x73) with
                                      # imm=1, rs1=0, rd=0 — a breakpoint
                                      # that Debug Mode catches when routed
                                      # by dcsr.ebreakm/ebreaku/ebreaks
    dmi write data1 <- A
    dmi write command <- 0x0221_0000    # 32-bit write
    poll abstractcs ...                     # length = 4
else:                                 # 16-bit compressed instruction
    dmi write data0 <- 0x9002         # c.ebreak = quadrant 2 (op=0b10),
                                      # funct4=0b1001, rs1=rs2=0 —
                                      # the 16-bit form of ebreak
    dmi write data1 <- A
    dmi write command <- 0x0211_0000    # 16-bit write (aamsize=1!)
    poll abstractcs ...                     # length = 2
```

The 16-bit store is required: a 32-bit store would clobber the
*following* instruction. If the DM only implements 32-bit stores, use
the read-modify-write trick instead: write the full word
`(w0 & 0xFFFF_0000) | 0x9002` (only for word-aligned `A`; for
`A % 4 == 2` patch the upper halfword of the word at `A-2`). The
minimal DM requires `aamsize=1`, so this is a fallback for other DMs.

Also set `dcsr.ebreakm` (bit 15, `0x8000`) at attach if the debuggee runs
in M-mode (`dcsr = dcsr | 0x8000`), and `ebreaku`/`ebreaks` per
privilege — otherwise the patched `ebreak` traps to the *debuggee's*
trap handler instead of Debug Mode.

### Hitting

```
poll dmstatus until allhalted
# read dcsr/dpc (see above):
cause = 1                             # ebreak => breakpoint hit
dpc == A                              # dpc holds the address OF the ebreak
```

Report the breakpoint hit to the user with the register dump.

### Continuing past a hit

The classic step-over-restore-reinsert loop:

```
1. restore the original instruction:
   write A <- saved word (32- or 16-bit)
   (dpc == A already, so the hart will re-execute the original)
2. single-step: dcsr.step=1, resumereq, poll resumeack, poll halted
   -> executed the original instruction, dpc = A+len (or branch target)
3. re-insert the patch:
   write A <- ebreak / c.ebreak
4. clear step: write dcsr = dcsr & ~0x4    # keeps ebreakm set
5. resumereq, poll allresumeack       # run on
```

### Removing

Restore the saved original word at A (a plain memory write). On detach,
walk the whole breakpoint table and restore everything.

## Reset and debug from reset

No `resethaltreq` in this DM, but `haltreq` held across a system reset
works just as well — halt out of reset and debug from the very first
instruction:

```
dmi write dmcontrol <- 0x8000_0003        # haltreq (0x8000_0000)
                                            # | ndmreset (0x2) | dmactive (0x1)
                                            # -> everything except DM/DTM in reset
... hold as long as needed ...
dmi write dmcontrol <- 0x8000_0001        # release ndmreset, haltreq stays
poll dmstatus until allhalted            # hart halts as it leaves reset,
                                     # BEFORE executing its first instruction
read dpc                             # = the reset vector / first fetch address
```

- `ndmreset` (bit 1) resets the whole SoC except the DM and DTM. `dmactive`
  must stay 1 throughout — 0 would reset the DM mid-flow.
- A pulse (1 then 0) is sufficient; the spec requires a write of 1
  followed by 0 to trigger the reset.
- The hart may read `allunavail` for a while after releasing reset —
  poll `allhalted`, not a fixed delay.

This is also the recovery when the hart wedges (see below).

## Detaching

Leave the target running as if no debugger had ever been there:

```
1. remove all breakpoints (restore every patched word)
2. restore the dcsr control bits saved at attach (clears ebreakm,
   step, etc.; keep the current prv)
3. dmi write dmcontrol <- 0x0000_0001      # clear haltreq, dmactive stays 1
4. resumereq; poll allresumeack       # run
   (skip if the hart was already running when you attached)
5. optional: dmi write dmcontrol <- 0x0000_0000   # dmactive=0: DM pristine,
                                             # all debug state reset
```

Then drop the JTAG connection; a TAP reset on disconnect is harmless
(the DTM resets, the DM keeps its state).

## Error recovery

Layered — resolve at the lowest layer that fails:

**1. `abstractcs.cmderr` after a command.** Poll `abstractcs` after
every command; if `cmderr != 0`, the command (and everything after it
in a batch) did not run. Decode:

| cmderr | Meaning | Typical cause |
|---|---|---|
| 1 | busy | you touched data/command while busy |
| 2 | not supported | unsupported option (e.g. aarsize=3 on RV32) |
| 3 | exception | bad regno / access faulted |
| 4 | halt/resume | hart was not halted (or unavailable) |
| 5 | bus | load/store bus error |
| 7 | other | implementation-specific |

Clear with write-1-to-the-bits: `cmderr=2` → `0x200`, any → `0x700`
(`dmi write abstractcs <- 0x200`). Then fix the cause and re-issue.

**2. DMI busy (`dmi` scan returns `op=3`).** The DM did not finish the
previous operation before the next capture — rare with this DM (its
DMI is a plain handshake), but the recovery is fixed: scan `dtmcs`
with `dmireset` (bit 16) = 1, increase the Run-Test/Idle dwell, repeat
the request scan (spec B.1 — OpenOCD does exactly this, ratcheting the
delay up on each busy).

**3. DMI failed (`op=2`).** The DMI itself errored. Clear the sticky
status with `dtmcs.dmireset` (bit 16); if the DTM stays stuck,
`dtmcs.dmihardreset` (bit 17). Then re-init `dmactive` and replay.

**4. Hart never halts.** Read `dmstatus`: `allunavail` → it is in reset:
do the [Reset and debug from reset](#reset-and-debug-from-reset) flow.
`anynonexistent` → wrong hart index. Otherwise ndmreset pulse with
`haltreq` held and retry.

**5. Everything wedged.** `dmi write dmcontrol <- 0` then `1`: `dmactive=0`
resets the whole DM (data registers, cmderr, sticky bits) — the
universal unwedge. Re-init from scratch. If even that stops responding,
`dtmcs.dmihardreset` at the DTM layer.

## What this configuration cannot do

Know the limits so you don't hunt for bugs that are design facts:

- **No hardware breakpoints/watchpoints** (no trigger module). All
  breakpoints are software patches — they need *writable* memory.
  A breakpoint in ROM or flash does not exist with this DM; that is the
  single strongest reason to add a trigger module (see "What to add
  next" in [minimal-dm.md](minimal-dm.md)).
- **No watchpoints at all.** The closest emulation is halt-and-poll a
  memory location in a loop (slow, and it changes timing).
- **No memory access while running** (no SBA, Access Memory requires a
  halted hart). Inspection is stop-the-world: halt, read, resume.
- **No multi-hart** (HARTSELLEN=0) and **no `resethaltreq`** (use
  held-`haltreq` across `ndmreset` instead — equally useful).
- **No Program Buffer** — nothing needs it; Access Register covers
  GPRs and CSRs, Access Memory covers loads/stores.
- **Cache coherence**: if the core has an I-cache that is not coherent
  with the DM's LSU writes, a patched `ebreak` may not be fetched. On
  small RV32 cores without caches this is a non-issue; otherwise
  invalidate or accept stale fetches.

## Cost cheat sheet

Order-of-magnitude costs (abits=7, one DMI op = 2 dmi scans of ~47
TCKs each, plus idle dwell; at 10 MHz TCK ≈ 10 µs per DMI op before
batching):

| Action | DMI ops | Notes |
|---|---|---|
| poll dmstatus | 1 | ~10 µs |
| read a GPR/CSR | 3 | command + poll + data0 read |
| write a GPR/CSR | 3 | data0 + command + poll |
| read/write a memory word | 4 | data1 + command + poll + data0 |
| halt / resume | 1 + poll | ~2–5 ops until acknowledged |
| breakpoint install | ~8 | read word + patch write (4 + 4) |
| continue past breakpoint | ~19 | restore (4) + set step (3) + step (3) + re-patch (4) + clear step (3) + resume (2) |
| register dump (33 regs) | ~99 | mostly polling |

Debuggers batch aggressively (OpenOCD pipelines whole command
sequences into one JTAG transaction and checks status at the end), so
real throughput is several times better — but the ratios hold, and
they explain why "stepi" loops and word-by-word memory dumps are the
expensive operations.

## References

- RISC-V External Debug Support,
  [version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
  (§3 Debug Module, §4 Debug Mode/dcsr/dret, §6.1 JTAG DTM,
  Appendix B debugger implementations)
- OpenOCD
  [`riscv-013.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv-013.c),
  `riscv-013_reg.c`, `riscv.c` (software breakpoints via
  `riscv_add_breakpoint`, busy-delay learning, XLEN probing)
- Related documents: [minimal-dm.md](minimal-dm.md) (the register set
  these sequences drive),
  [jtag-dtm.md](jtag-dtm.md) (the transport underneath),
  [debug-architecture.md](debug-architecture.md) (the big picture)