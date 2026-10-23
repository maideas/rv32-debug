# JTAG to DMI Translation: Implementing the RISC-V Debug Transport Module

This document explains in full detail how the JTAG-to-DMI translation works
and what you need to implement it: the JTAG TAP (IEEE 1149.1), the DTM
registers (`dtmcs`, `dmi`), the `op` request/response protocol with its
sticky error rules, the timing model (`dtmcs.idle`, busy), reset/recovery
semantics, the DMI bus interface including clock-domain crossing, and
debugger-level scan sequences.

For the surrounding architecture see
[debug-architecture.md](debug-architecture.md); for the Debug Module on
the far side of the DMI see [minimal-dm.md](minimal-dm.md).

Everything below was verified against the RISC-V External Debug Support
specification [version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
and [version 1.0](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf),
the PULP/OpenTitan DTM RTL
([dmi_jtag_tap.sv](https://github.com/lowRISC/opentitan/blob/master/hw/vendor/pulp_riscv_dbg/src/dmi_jtag_tap.sv),
[dmi_jtag.sv](https://github.com/lowRISC/opentitan/blob/master/hw/vendor/pulp_riscv_dbg/src/dmi_jtag.sv),
[dmi_cdc.sv](https://github.com/lowRISC/opentitan/blob/master/hw/vendor/pulp_riscv_dbg/src/dmi_cdc.sv) —
the DTM used in OpenTitan silicon), and the OpenOCD
[`riscv-013.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv-013.c)
driver.

## Table of contents

- [What you are building](#what-you-are-building)
- [JTAG primer: the part of IEEE 1149.1 you must implement](#jtag-primer-the-part-of-ieee-11491-you-must-implement)
- [DTM instruction set](#dtm-instruction-set)
- [The simple registers: BYPASS, IDCODE, dtmcs](#the-simple-registers-bypass-idcode-dtmcs)
- [The dmi register and the op protocol](#the-dmi-register-and-the-op-protocol)
- [Timing: the busy mechanism and dtmcs.idle](#timing-the-busy-mechanism-and-dtmcsidle)
- [Reset and recovery](#reset-and-recovery)
- [The DMI bus interface](#the-dmi-bus-interface)
- [Worked scan sequences](#worked-scan-sequences)
- [Verification checklist](#verification-checklist)
- [Common pitfalls](#common-pitfalls)

## What you are building

The DTM sits between the four JTAG pads and the Debug Module. It contains:

```
TCK ─┐
TMS ─┤     ┌──────────────────────────────────────────────────┐
TDI ─┼────►│ JTAG TAP                                        │
     │    │  16-state FSM ──► IR (5 bits) ──► DR select      │
TDO ◄┼─────│        │                                        │
     │     │   capture/shift/update pulses                   │
     │     │   ┌────────┬──────────┬──────────┬─────────┐    │
     │     │   │ BYPASS │ IDCODE   │ dtmcs    │ dmi     │    │   DMI bus
     │     │   │ 1 bit  │ 32 bits  │ 32 bits  │ abits+34│    │   (req/resp
     │     │   └────────┴──────────┴──────────┴─────────┘    │    handshake)
     └─────└──────────────────────────────────────────────────┘
```

Your deliverables:

1. A JTAG TAP controller (16-state FSM, IR, update/capture/shift pulses).
2. Four data registers selected by IR: BYPASS, IDCODE, `dtmcs`, `dmi`.
3. The `op` protocol engine: start DMI operations on Update-DR, report
   results on Capture-DR, sticky busy/failed handling.
4. A DMI bus master (request/response handshake), plus a clock-domain
   crossing if the DM is not in the TCK domain.

The DM on the other side is described in [minimal-dm.md](minimal-dm.md).
The DTM does **not** interpret DMI addresses — it is a dumb address/data/op
transport.

## JTAG primer: the part of IEEE 1149.1 you must implement

### Signals

| Signal | Direction | Function |
|---|---|---|
| TCK | input | Test clock. TMS/TDI are sampled on its **rising** edge; TDO changes on its **falling** edge. |
| TMS | input | Test mode select; drives the FSM. |
| TDI | input | Serial data in (LSB of the selected shift register first). |
| TDO | output | Serial data out; only meaningful in Shift states. |
| TRST* | input (optional) | Asynchronous TAP reset. Tie high if unused. |

### The TAP state machine

16 states; every rising TCK edge advances according to TMS. Five
consecutive TCKs with TMS=1 from any state reach Test-Logic-Reset, TLR
(so does TRST*).

| Current state | TMS=0 → | TMS=1 → | Action in this state |
|---|---|---|---|
| Test-Logic-Reset | Run-Test/Idle | Test-Logic-Reset | reset TAP; IR ← IDCODE |
| Run-Test/Idle | Run-Test/Idle | Select-DR-Scan | (DMI dwell cycles spent here) |
| Select-DR-Scan | Capture-DR | Select-IR-Scan | — |
| Capture-DR | Shift-DR | Exit1-DR | load selected DR's parallel value |
| Shift-DR | Shift-DR | Exit1-DR | shift DR one bit: TDO ← LSB, MSB ← TDI |
| Exit1-DR | Pause-DR | Update-DR | — |
| Pause-DR | Pause-DR | Exit2-DR | (retry aborted bits) |
| Exit2-DR | Shift-DR | Update-DR | — |
| Update-DR | Run-Test/Idle | Select-DR-Scan | apply the shifted DR value |
| Select-IR-Scan | Capture-IR | Test-Logic-Reset | — |
| Capture-IR | Shift-IR | Exit1-IR | load fixed pattern ([1:0] must be `01`) |
| Shift-IR | Shift-IR | Exit1-IR | shift IR (≥ 5 bits) |
| Exit1-IR | Pause-IR | Update-IR | — |
| Pause-IR | Pause-IR | Exit2-IR | — |
| Exit2-IR | Shift-IR | Update-IR | — |
| Update-IR | Run-Test/Idle | Select-DR-Scan | latch IR ← shifted value |

Key semantics for your implementation:

- **Shift is LSB-first**: each Shift state consumes one TCK; the LSB of the
  selected register goes out on TDO, TDI enters at the MSB position, and the
  register shifts right.
- **Capture-DR** loads the selected register's parallel value to be shifted
  out (a result, an ID, a status — register-specific).
- **Update-DR** applies what was shifted in (register-specific: an action,
  a write, or nothing for read-only registers).
- **TDO must be updated on the falling edge of TCK** (register it in a
  negedge flop) so it is stable while the debugger samples it on the rising
  edge. Drive TDO only in Shift-IR/Shift-DR states (output-enable).
- The debugger determines scan lengths itself, so every DR must have a
  fixed, known width (see below).

## DTM instruction set

The RISC-V spec requires an **IR of at least 5 bits**. When the TAP is
reset, IR must default to `00001` (IDCODE). Register encodings:

| IR | DR selected | DR width | Notes |
|---|---|---|---|
| 0x00 | BYPASS | 1 | JTAG recommends this encoding |
| 0x01 | IDCODE | 32 | JTAG recommends this encoding; selected at TAP reset |
| 0x10 | dtmcs | 32 | DTM control/status |
| 0x11 | dmi | abits+34 | DMI access |
| 0x12–0x14 | (reserved) | 1 | reserved for future RISC-V debug |
| 0x15–0x17 | (reserved) | 1 | reserved for future RISC-V standards |
| 0x1f | BYPASS | 1 | JTAG requires this encoding |

**Every unimplemented IR encoding must select BYPASS.** (1.0
clarification: if your IR is wider than 5 bits, zero-extend the encodings
in their MSBs — except 0x1f, which must be extended with 1s.)

On Capture-IR, load a fixed pattern whose two LSBs are `01` (IEEE 1149.1
requirement; e.g. `5'h01` — the PULP TAP uses `5'b00101`). This lets the
debugger verify scan-chain integrity.

## The simple registers: BYPASS, IDCODE, dtmcs

### BYPASS (all unimplemented IRs)

A 1-bit shift register: capture 0, shift TDI→TDO. Read-only (Update-DR does
nothing). Its purpose: keep scan-chain length constant when this TAP isn't
addressed (multi-TAP boards).

### IDCODE (0x01)

32 bits, read-only, fixed value:

```
[31:28] version    [27:12] part number    [11:1] ManufId    [0] = 1 (IEEE)
```

`ManufId` must be your JEDEC JEP106 manufacturer ID: bits [6:0] are the
final (non-0x7f) byte of your ID code, bits [10:7] are the count of 0x7f
continuation bytes preceding it (modulo 16). Version/part number are free
choice (typically bumped per tape-out). This is how a probe discovers
which TAP it is talking to after reset.

### dtmcs (0x10)

A 32-bit register, **local to the DTM** — accesses never go out over the
DMI, so `dtmcs` works even when the DM is hung or in reset. This is what
makes error recovery always possible. Layout (identical in 0.13.2 and 1.0
except `errinfo`):

| Bits | Field | R/W | Meaning |
|---|---|---|---|
| 31:21 | 0 | R | — |
| 20:18 | errinfo | R | **1.0 only, optional.** extra error detail (below). 0.13.2: read 0. |
| 17 | dmihardreset | W1 | full reset of the DTM: forget outstanding DMI ops, reset all DTM state |
| 16 | dmireset | W1 | clear the sticky error state (`op`/`dmistat`) only; does **not** abort in-flight ops |
| 15 | 0 | — | — |
| 14:12 | idle | R | RTI dwell hint (see [Timing](#timing-the-busy-mechanism-and-dtmcsidle)) |
| 11:10 | dmistat | R | alias of the sticky status: 0 = OK, 2 = failed, 3 = busy |
| 9:4 | abits | R | width of the address field in the `dmi` register (7–32; **minimum 7**) |
| 3:0 | version | R | 1 = spec 0.13/1.0 (0 = 0.11; 15 = custom) |

Capture-DR loads the status value (with `dmireset`/`dmihardreset` reading
0); Update-DR acts on the write bits; all other bits ignore writes.
Report `version = 1` — that one value covers both 0.13 and 1.0 DTMs.

`errinfo` (1.0): 0 = not implemented, 1 = error between DTM and DMI,
2 = error between DMI and a subordinate, 3 = subordinate reported an error,
4 = no further info (reset value if implemented). Optional — implement as
constant 0 (reads "not implemented") for a 0.13.2-style DTM.

## The dmi register and the op protocol

### Register layout

The `dmi` DR is `abits + 34` bits wide (41 bits for the typical `abits=7`):

```
[abits+33:34]  address      (abits wide)
[33:2]         data         (32 bits)
[1:0]          op           (2 bits)
```

**Scan order** (LSB of the register first, verified against OpenOCD's
`debug_defines.h` and the PULP RTL): `op[0]`, `op[1]`, then the 32 `data`
bits, then the `address` bits — address last. A debugger building the scan
buffer packs op at bit offset 0, data at offset 2, address at offset 34.

### Per-TAP-state behavior (the whole protocol in three rules)

1. **Update-DR** (with IR = dmi): *start* the operation given by the
   shifted-in `op` — **unless** the status currently reported in `op` is
   sticky (see below). `op` values:
   - `0` (nop): do nothing, send nothing over the DMI. Never causes busy
     or error by itself. The captured address/data of the following scan
     are undefined.
   - `1` (read): issue a DMI read of `address`.
   - `2` (write): issue a DMI write of `data` to `address`.
   - `3`: reserved as a request.
2. **Capture-DR** (with IR = dmi): load the *result of the previous
   operation* into `{address, data, op}`:
   - `0` = previous op succeeded. After a read, `data` holds the value
     read and `address` the address it was read from. After a write or
     nop, address/data are unspecified.
   - `2` = previous op **failed** (the DM/DMI itself reported an error;
     there are no specified cases where this happens, so in practice a
     simple DM never returns it). Sticky.
   - `3` = **busy**: the previous operation had not completed by this
     capture, or a new op was attempted while one was still in progress.
     The request shifted in with this status is ignored. Sticky.
   - While the sticky status is set, Capture-DR keeps loading it (op is
     not updated), and Update-DR starts nothing — until `dmireset`.
3. Because the response to scan N arrives during scan N+1, **every DMI
   read or write costs two DR scans**: scan 1 carries the request, scan 2's
   Capture-DR latches the result. Debuggers exploit this by pipelining:
   each scan carries the *next* request while fetching the previous
   result, so a burst of R operations takes R+1 scans, with status checked
   only at the end.

### Sticky semantics in detail

`op = 2` or `op = 3` latch a **sticky flag** in the DTM. While set:

- Capture-DR reports the sticky code (not new results).
- Update-DR ignores read/write requests (data scanned in is ignored).
- Only writing 1 to `dmireset` in `dtmcs` clears it.

Why: debuggers batch many scans and check status only at the end. The
sticky flag guarantees that after one failure, *no later commands of the
batch execute*, and the debugger can see that something failed (then
recover and replay the batch). Without stickiness a mid-batch error would
be silently overwritten by later successes.

### The two busy sources (be precise here — this is the usual bug)

Busy (`op = 3`) is **generated by the DTM**, never by a healthy DM. It
becomes sticky in two situations:

1. **Capture-DR while the previous op is still in flight** — the response
   hadn't arrived by capture time (too little time between Update-DR and
   Capture-DR; see [Timing](#timing-the-busy-mechanism-and-dtmcsidle)).
2. **Update-DR with a read/write while an op is still in flight** — the
   new request is discarded.

Edge cases worth deciding deliberately in your design (both spec-legal):
a Capture-DR racing a response that completes in that very cycle should
report the response result (it arrived in time); but a read/write *attempt*
racing a completing response should resolve to busy (the in-flight op
still owned the engine at update time) — the debugger simply retries.

Note on nops: a nop never *starts* anything, so it never triggers source 2
by itself. But scanning a nop to poll a slow op still reports source 1 if
the op hasn't completed — the result capture simply isn't ready.

The in-flight op keeps running after a busy report; when its response
arrives it is consumed and dropped (the status is already sticky). After
`dmireset`, a new scan can start normally once the old op has drained.
The spec's debugger remedy (Appendix B.1/B.2.1, implemented literally by
OpenOCD): on `op = 3`, scan `dtmcs` with `dmireset=1`, then **perform the
request scan again**, allowing more time between Update-DR and Capture-DR
next time.

## Timing: the busy mechanism and dtmcs.idle

### The response window

The window between the Update-DR that starts an op and the Capture-DR that
must present its result is tiny:

```
Update-DR → [Run-Test/Idle × idle] → Select-DR → Capture-DR
   = 2 + idle TCK cycles
```

(idle=0 skips RTI entirely via Update-DR → Select-DR directly.) Everything
else — the 41+ shift clocks of the *next* scan — happens after its
capture, so it doesn't help. If the DMI response does not arrive within
`2 + idle` TCKs, the DTM reports busy.

### What idle means

`dtmcs.idle` tells the debugger the *minimum* RTI dwell to insert after
every DMI scan:

| idle value | debugger behavior |
|---|---|
| 0 | skip Run-Test/Idle entirely |
| 1 | enter RTI and leave immediately (1 cycle) |
| 2 | stay in RTI 2 cycles |
| n | stay n cycles |

### Sizing idle for your design

- **DM in the TCK domain** (simplest system): response latency can be ≤ 1
  TCK. Report `idle = 0` or `1`. You will never see busy.
- **DM on another clock, behind a CDC** (the normal SoC case): response
  latency ≈ (2 clk sync in) + (DM response time, e.g. 1–3 clk) + (2 clk
  sync back). OpenTitan's DM asserts its response exactly one cycle after
  the request. You need
  `(2 + idle) × T_TCK ≥ latency` → choose `idle` accordingly.

Examples: TCK 10 MHz, DM clock 100 MHz, CDC latency ≈ 6 DM cycles ≈
60 ns ≪ 1 TCK (100 ns) → `idle = 1` is ample (the PULP default). TCK
10 MHz but DM clock 1 MHz: 6 DM cycles ≈ 6 µs ≈ 60 TCKs → `idle` in the
high tens — or better, run the DMI handshake faster / reduce sync stages.

Debuggers adapt dynamically: OpenOCD starts at the reported `idle`,
increments its dwell every time it sees busy (`dtmcs.dmireset` scan +
`jtag_add_runtest(delay, TAP_IDLE)`), and re-runs the failed request —
so a slightly-too-small `idle` costs performance, not correctness. But
reporting a sane value avoids a slow first burst.

### Scan cost (for intuition)

One DR scan with abits=7 ≈ 41 shift TCKs + ~6 FSM overhead ≈ 47 TCKs. A
pipelined burst of R DMI ops ≈ (R+1) × 47 TCKs. At TCK = 10 MHz that's
~4.7 µs per DMI operation — JTAG is the bottleneck of RISC-V debugging,
which is why debuggers batch scans and never re-scan IR.

## Reset and recovery

| Trigger | Effect you must implement |
|---|---|
| `dtmcs.dmireset` (bit 16, W1) | clear the sticky `op`/`dmistat` status. Nothing else. In-flight ops continue. |
| `dtmcs.dmihardreset` (bit 17, W1) | full reset of the DTM: forget outstanding DMI transactions (assert your CDC/DMI clear), reset the dmi engine, sticky status, shift registers. The debugger uses this only when it believes an outstanding transaction will never complete (e.g. after a system reset cancelled an in-flight op). |
| TAP reset (TRST* or 5×TMS=1 → Test-Logic-Reset) | spec-mandated: IR ← IDCODE. The spec does not require resetting the DTM's DMI state, but the reference implementation (PULP/OpenTitan) treats TAP reset like `dmihardreset` — recommended: a debugger that resets the TAP expects a pristine DTM. |
| `dmactive=0` (DM side) | not a DTM concern — the DMI slave in the DM is reset; the DTM just sees responses stop or the DM's state re-initialized. |

Important interplay: the DM's `dmactive` is only reachable *through* the
DMI — if the DM wedges, `dtmcs` is still scannable, and after clearing the
DTM (`dmihardreset`) the debugger can drive `dmcontrol.dmactive` again.
Design your DMI slave so it can always accept a request eventually (or is
reset by the CDC clear), otherwise the DTM can wedge in busy forever.

Never wire the board's nRESET to TRST*: the debugger must be able to hold
the DTM alive across a system reset to debug reset-related crashes.

## The DMI bus interface

The spec leaves the DMI bus format to the designer ("a trivial bus with
one master and one slave, or ... TileLink or AMBA APB"). A validated
simple choice (PULP/OpenTitan):

```systemverilog
// request (DTM → DM), valid/ready handshake
dmi_req_valid, dmi_req_ready
dmi_req_op    [1:0]     // 1 = read, 2 = write (a nop starts nothing,
                        //  so only 1/2 ever reach the bus; 3 never issued)
dmi_req_addr  [abits-1:0]
dmi_req_data  [31:0]    // write payload
// response (DM → DTM), valid/ready handshake
dmi_resp_valid, dmi_resp_ready
dmi_resp_op   [1:0]     // 0 = success, 2 = failed (error). A simple DM
                        // always answers 0; errors are optional.
dmi_resp_data  [31:0]   // read result
```

DM-side contract: accept a request when ready (single outstanding request
is all the DTM ever issues), decode `addr` against the DM register map
(0x04–0x40, see [minimal-dm.md](minimal-dm.md)), perform the read/write,
respond — in a small system one clock later — with `op=0` (or `2` on an
internal error). Busy is *not* a legal response here; the DTM fabricates
busy itself from missing responses. Keep the DM's response time bounded
and small relative to TCK, or size `dtmcs.idle` for it.

### Clock-domain crossing

If the DM runs on the system clock, put a CDC between TAP and DM. The
proven pattern (OpenTitan `dmi_cdc`):

- One async FIFO per direction (or 2-flop-synchronized valid/ready
  handshakes with data held stable). A depth of 1–2 suffices — the DTM
  issues one request and waits.
- Both FIFOs must be resettable from the TAP side: the TCK domain pulses
  a `dmi_clear` (on TAP reset or `dmihardreset`), which is 2-flop
  synchronized into the DM domain and resets the DM-side pointer/state.
  This is what "forget outstanding transactions" means in hardware.
- Beware of reset ordering: an in-flight transaction cancelled mid-way
  must not leave a phantom response in the FIFO; the synchronized clear
  flushes both sides.

If you can, run the whole DMI in the TCK domain and treat the DM's
register interface as a slow slave (respond within 2 TCKs) — then no CDC
is needed and `idle` can be 0–1.

## Worked scan sequences

### Selecting the dmi instruction (once)

From Run-Test/Idle: `TMS=1` (Select-DR) → `TMS=1` (Select-IR) →
`TMS=0` (Capture-IR) → `TMS=0` (Shift-IR) × 5, shifting IR value 0x11
LSB-first on TDI (`1,0,0,0,1`), `TMS=1` on the last shift bit (Exit1-IR)
→ `TMS=1` (Update-IR) → `TMS=0` (Run-Test/Idle). IR stays 0x11 until
changed — from now on only DR scans are needed.

### A DMI read (e.g. dmstatus at DMI address 0x11, abits=7)

Scan 1 — the request (starting in Run-Test/Idle):

```
TMS: 1 (Select-DR)  0 (Capture-DR)  0 ×40, then 1 together with the 41st
     bit (last Shift-DR cycle, leaving into Exit1-DR)  1 (Update-DR)
TDI (41 bits, LSB first): op=01        -> 1,0
                          data 32 bits -> 0 ×32 (ignored)
                          addr 0x11    -> 1,0,0,0,1,0,0
then: TMS=0 into Run-Test/Idle, dwell 'idle' cycles
```

Scan 2 — collect the result (and carry the next request or a nop):

```
same shape; the 41 bits shifted OUT are:
  [1:0]    op    = 0 (success) | 3 (busy — retry) | 2 (failed)
  [33:2]   data  = the value read from dmstatus
  [40:34]  addr  = 0x11 (echoed back)
```

A write is identical, except scan 1 carries `op=10` plus the 32-bit write
payload in `data`.

### Pipelined burst (how real debuggers drive it)

```
scan 1: shift in {op=read, addr=A1}          (result of nothing yet)
scan 2: shift in {op=read, addr=A2}          (captures result of A1)
scan 3: shift in {op=read, addr=A3}         (captures result of A2)
...
scan N+1: shift in {op=nop}                  (captures result of AN)
check op of every captured result; on any 2/3 -> dmireset, replay batch
```

The batching works precisely *because* the sticky flag freezes the status
once something goes wrong.

### Busy recovery (debugger side, per spec B.1 / OpenOCD)

```
if captured op == 3:            # busy
    scan dtmcs with bit 16 (dmireset) = 1   # clears sticky
    increase the RTI dwell used after each dmi scan
    repeat the request scan                # until it returns op == 0
```

## Verification checklist

Simulate (or bench-test) at least these scenarios before trusting the DTM:

1. **TAP reset**: TRST* and 5×TMS=1 both land in TLR; IR reads back as
   IDCODE; a 32-bit IDCODE scan returns your ID with LSB=1.
2. **IR integrity**: Capture-IR pattern has [1:0]=`01`; unimplemented IR
   values select a 1-bit BYPASS.
3. **dtmcs read**: returns version=1, correct abits, idle, dmistat=0.
4. **Two-scan DMI read** against a behavioral DM: correct result data,
   echoed address, op=0; verify the scan bit order by shifting a known
   pattern (e.g. addr=0x7f, data=0xA5A5_0001) and checking the TDO stream.
5. **Busy path**: make the behavioral DM respond slowly; confirm capture
   reports op=3, further updates are ignored, `dmireset` restores
   operation, and the retry succeeds.
6. **Failure path**: DM responds op=2; confirm stickiness and dmireset.
7. **dmihardreset mid-op**: request in flight, assert bit 17; the DTM
   returns to idle, the CDC/DMI clear fires, subsequent scans work.
8. **TAP reset mid-op**: same expectations (with the recommended TLR =
   full-DTM-reset behavior).
9. **TDO timing**: TDO only changes on TCK falling edges and is stable at
   rising edges during Shift states; tdo_oe inactive outside Shift.
10. **Interop**: connect OpenOCD (board config with `jtag newtap` —
   `-irlen 5`, `-expected-id` your IDCODE — and a `target create` using
   the `riscv` target type) and run `init; halt; reg; resume` against a
   minimal DM.

## Common pitfalls

- **Wrong scan order** — packing address first instead of op first is the
  classic silent failure: reads return garbage but "work". The op field is
  the two LSBs of the DR; it is shifted first.
- **`idle` too small with a slow DM** — every DMI op wedges busy; the
  debugger spends forever ramping its learned delay. Size per the timing
  math above, or keep the DMI in the TCK domain.
- **Forgetting stickiness** — clearing `op` on the next success lets
  mid-batch failures vanish and batches execute partially after an error.
  Never clear status except via `dmireset`/`dmihardreset`.
- **TDO on the wrong edge** — updating TDO on the rising edge violates
  IEEE 1149.1 and breaks marginal adapters.
- **Capture-IR value** — must have `01` in the two LSBs; some adapters
  verify the scan path with it.
- **IR ≠ 5 bits** — allowed (wider), but then extend encodings per the
  1.0 rule (zeros, except 0x1f → all ones) and tell the debugger the right
  IR length in the board config.
- **dtmcs depending on the DMI** — `dtmcs` must work with the DM wedged;
  it exists (also) for recovery. Same for `dmireset`/`dmihardreset`.
- **No recovery from a wedged in-flight op** — if your CDC/DMI slave can
  stall forever, `dmihardreset` must actually clear it (the `dmi_clear`
  path), otherwise the debugger can never recover without power cycle.
- **Reporting busy for scans of other IRs** — gate the busy conditions on
  the dmi register being selected: scanning IDCODE or dtmcs while an op
  is in flight must not fabricate a busy report (the PULP DTM omits this
  gate; harmless there, but be strict).
- **dmi register width ≠ abits+34** — OpenOCD builds its scan length from
  `dtmcs.abits`; a mismatch shifts every field and breaks everything.
- **Multiple TAPs on the chain** — other TAPs must be in BYPASS and their
  1-bit registers lengthen the scan; not a DTM bug, but test the board
  configuration if your target shares JTAG with other chips.

## References

- RISC-V External Debug Support,
  [version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
  (§6.1 JTAG DTM, Appendix B) and
  [version 1.0](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf)
  (§6.1, §B.2.1)
- PULP RISC-V debug module (vendored in OpenTitan as
  [pulp_riscv_dbg](https://github.com/lowRISC/opentitan/tree/master/hw/vendor/pulp_riscv_dbg/src)):
  `dmi_jtag_tap.sv` (TAP), `dmi_jtag.sv` (DTM), `dmi_cdc.sv` (CDC)
- OpenOCD
  [`riscv-013.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv-013.c)
  and its `debug_defines.h` (bit offsets, busy-delay learning)
- Related documents: [debug-architecture.md](debug-architecture.md),
  [minimal-dm.md](minimal-dm.md)
