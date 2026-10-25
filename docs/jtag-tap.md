# JTAG TAP: IR and DR Semantics (IEEE 1149.1)

This document is the standards-level reference under the transport layer:
the verified facts of how an IEEE 1149.1 Test Access Port treats its
instruction register (IR) and data registers (DR) — what the IR holds after
reset, which instruction encodings the standard fixes and which the vendor
picks, in which direction and bit order scans shift, what value an IR scan
actually returns, and the 5-bit IR that the RISC-V debug DTM uses. Each
section answers one of the questions that came up when working through the
DTM, with the answer checked against the standard and real tool
documentation.

For implementing the DTM itself — the TAP FSM state table, the
`dtmcs`/`dmi` register layouts, the sticky `op` protocol, timing, DMI bus,
clock-domain crossing — see [jtag-dtm.md](jtag-dtm.md). For the layer above
see [debug-architecture.md](debug-architecture.md).

Everything below was verified against IEEE Std 1149.1 documentation (the TI
*IEEE Std 1149.1 (JTAG) Testability Primer*
([ssya002c](https://www.ti.com/lit/an/ssya002c/ssya002c.pdf)), Lauterbach's
JTAG training manual
([pdf](https://www2.lauterbach.com/pdf/training_jtag.pdf)), and BSDL
reference documentation), the RISC-V External Debug Support specification
(versions 0.13.2 and 1.0), the PULP/OpenTitan DTM RTL, and OpenOCD.

## Table of contents

- [The TAP in one paragraph](#the-tap-in-one-paragraph)
- [The IR value after TAP reset](#the-ir-value-after-tap-reset)
- [Instruction encodings: fixed by the standard vs. vendor-specific](#instruction-encodings-fixed-by-the-standard-vs-vendor-specific)
- [IDCODE: the 32-bit identification register](#idcode-the-32-bit-identification-register)
- [Shift direction and bit order](#shift-direction-and-bit-order)
- [What an IR scan returns: the Capture-IR pattern](#what-an-ir-scan-returns-the-capture-ir-pattern)
- [The RISC-V DTM: a 5-bit IR](#the-risc-v-dtm-a-5-bit-ir)
- [Quick reference](#quick-reference)
- [References](#references)

## The TAP in one paragraph

A TAP is four (optionally five) pins — `TCK`, `TMS`, `TDI`, `TDO`
(and `TRST*`) — driven by a 16-state synchronous FSM
([jtag-dtm.md](jtag-dtm.md) has the full state table). Between `TDI` and
`TDO` sits the **instruction register (IR)**; the bit pattern latched into
it selects exactly one **data register (DR)** (boundary-scan register,
`IDCODE`, `BYPASS`, `dtmcs`, `dmi`, ...) as the scan path. Each register
goes through a capture → shift → update cycle under control of the FSM,
and the whole test logic is isolated from the system logic: when the TAP is
in Test-Logic-Reset, the device operates normally with the test logic
disconnected.

## The IR value after TAP reset

The IR is never undefined after reset. When the TAP enters
**Test-Logic-Reset (TLR)** — asynchronously via `TRST*`, or synchronously
by holding `TMS` high for five `TCK` cycles, which reaches TLR from any
state — the standard mandates that the IR be loaded with:

- **IDCODE**, if the device implements a device identification register
  (the usual case), or
- **BYPASS**, if it does not.

Consequences:

- **Chain enumeration for free.** A tool can reset all TAPs and immediately
  scan the DR path without ever clocking in an instruction: every IDCODE
  device emits its 32-bit ID, every other device a single `0` bypass bit, so
  the chain composition can be reconstructed from one scan (see the
  [IDCODE layout](#idcode-the-32-bit-identification-register) for why this
  cannot be ambiguous).
- **A universal "everything to BYPASS" primitive.** Because all-ones must
  decode to BYPASS (next section), resetting the chain and then shifting an
  unbounded stream of `1`s into the IR puts every TAP into BYPASS — without
  knowing any device's IR length.

## Instruction encodings: fixed by the standard vs. vendor-specific

Exactly one instruction encoding is fixed by IEEE 1149.1:

> **BYPASS** shall be selected when the IR holds **all logic 1s**.

That is mandatory for every compliant TAP, whatever its IR length (the IR
must be at least 2 bits). Devices *may* alias additional codes to BYPASS —
and the RISC-V debug spec does: all unused encodings select BYPASS (see
[below](#the-risc-v-dtm-a-5-bit-ir)), which is stronger than 1149.1.

Everything else is **vendor-specific** and published in the device's BSDL
file or datasheet:

- the IR length (≥ 2 bits),
- the encoding of IDCODE and of every optional instruction
  (EXTEST, SAMPLE/PRELOAD, INTEST, ...).

Typical, well-known values (always confirm in the BSDL/datasheet):

| Device family | IR length | IDCODE instruction | BYPASS instruction |
|---|---|---|---|
| ARM Cortex-M (JTAG-DP, e.g. ADIv5) | 4 | `1110` (0xE) | `1111` (0xF) |
| MIPS EJTAG | 5 | `00001` (0x01) | `11111` (0x1F) |
| Xilinx 7-series | 6 | `001001` (0x09) | `111111` (0x3F) |
| RISC-V DTM (per debug spec) | 5 | `00001` (0x01) | `11111` (0x1F) |

Note the BYPASS column is `11…1` in every row — not by coincidence but by
mandate. Also note that several vendors choose the IDCODE encoding
`00…001`, i.e. with the **least significant bit set** — consistent with the
fixed `1` in bit 0 of the ID *data* register (next section), a deliberate
symmetry.

## IDCODE: the 32-bit identification register

The 32-bit value scanned out during the IDCODE instruction is fully
standardized:

| Bits | Field | Content |
|---|---|---|
| 31:28 | version | 4-bit version, vendor-assigned |
| 27:12 | part number | 16-bit part-specific identifier |
| 11:1 | manufacturer id | 11-bit JEDEC (JEP106) manufacturer code |
| 0 | **fixed** | always `1` — mandated |

The mandatory `1` in bit 0 exists for chain enumeration: after a TAP reset,
scanning the DR path yields the 32-bit ID of every IDCODE device **and**
one `0` bit from every device in BYPASS. If a bypass bit ever collided with
the leading ID bits, the stream would be ambiguous — bit 0 = `1` guarantees
the ID and the bypass bit are distinguishable (bypass devices contribute a
lone `0`, IDs start with a `1` on the wire).

In BSDL the value is published as the `IDCODE_REGISTER` attribute.

## Shift direction and bit order

Data always moves **from `TDI` toward `TDO`** — for the IR and for every
DR. There is no reverse mode. Per shift clock:

- `TDI` is sampled on the **rising** edge of `TCK`,
- `TDO` changes on the **falling** edge (so both transitions of a bit cell
  happen in opposite half-cycles and never race).

### Register orientation: MSB near TDI, LSB near TDO

Both the IR and the DRs are arranged with the **MSB closest to `TDI` and the
LSB closest to `TDO`** (TI's 1149.1 primer, Fig. 3-4, draws exactly this for
the IR and DR). Each Shift-IR/Shift-DR clock right-shifts the register: the
LSB drops out onto `TDO`, the `TDI` bit enters at the MSB position.

```
                n-bit shift register

  TDI ──►│ MSB │ b │ … │ b │ LSB │──► TDO
         └─────┴───┴───┴───┴─────┘
            ▲                   │
            │                   │
      new bit in          old LSB out
      (rising TCK)        (falling TCK)

one Shift clock = right-shift by one:
every cell moves one position toward TDO
```

### Consequences on the wire

- **`TDO` emits the LSB first.** After Capture-DR on an IDCODE scan, the
  first bit on `TDO` is bit 0 — the fixed `1`.
- **`TDI` must be fed LSB first** to load a desired value: the first bit
  shifted in travels the full register length and ends up in the LSB. To
  load `1110` into a 4-bit IR, the `TDI` stream is `0,1,1,1`. Lauterbach's
  manual states it directly: "Reading or writing the IR or DR is performed
  bitwise from LSB to MSB"; OpenOCD's scan API uses the same convention.
- **Multi-TAP chains concatenate**, chain `TDI` → TAP 1 → TAP 2 → … → chain
  `TDO`, with the same LSB-first rule per TAP. To address TAP *k*, a tool
  pads with `1`s through the IRs of the TAPs before it (the `IR_PRE`/
  `IR_POST` padding every JTAG driver computes).

### Caveats

- For the **boundary-scan register** "MSB/LSB" carries no numeric meaning —
  it is a physical cell order, per-device, described in BSDL.
- A few **non-1149.1 debug ports** deviate: TI's MSP430 embedded-emulation
  JTAG is commonly documented MSB-first for IR scans. That is a vendor
  quirk of a proprietary debug port, not standard JTAG behavior — check
  the device documentation.

## What an IR scan returns: the Capture-IR pattern

Shifting the IR out does **not** return the current (or previous)
instruction. The IR has two separate stages:

- a **shift stage** between `TDI` and `TDO` — the only stage on the scan
  path, and
- an **update latch** (parallel output) feeding the instruction decoder —
  this holds the *active* instruction.

On entering **Capture-IR**, the shift stage is unconditionally overwritten
with a fixed diagnostic pattern while the update latch is left untouched
(so the active instruction is unaffected, and only Update-IR changes what
the decoder sees).

The captured value is:

- **bits [1:0]: the fixed pattern `01`** — mandated by IEEE 1149.1
  (bit 0 = 1, bit 1 = 0),
- **the remaining upper bits: a fixed, design-specific value** — commonly
  all zeros, but chosen by the designer (the PULP/OpenTitan TAP captures
  `5'b00101`).

So an *n*-bit IR typically scans out `0b000…001`; on the wire (LSB first)
the `TDO` stream is `1, 0, 0, …`. In a chain, the capture patterns of all
TAPs concatenate. The per-device value is published in BSDL as the
`INSTRUCTION_CAPTURE` attribute:

```vhdl
attribute INSTRUCTION_LENGTH   of XYZ : entity is 4;
attribute INSTRUCTION_CAPTURE  of XYZ : entity is "0001";
```

Uses of the pattern:

1. **Scan-path integrity check** — every compliant TAP's IR capture ends in
  `01`, so a tool can verify `TDI`→`TDO` connectivity and detect broken
  chains (this was the pattern's original purpose: board-level fault
  isolation of the test data path). OpenOCD checks each TAP's captured
  value.
2. **IR-length discovery** — shifting out a long stream and locating the
  `01` patterns reveals where each TAP's IR starts and ends, without any
  device documentation.
3. **TAP identification** — since the upper bits are design-specific, they
  can double as a design ID.

The practical consequence: **the active instruction cannot be read back**.
A debugger must track what it shifted in; it can never ask the TAP.

## The RISC-V DTM: a 5-bit IR

The RISC-V External Debug Support specification (0.13.2 and 1.0) constrains
the TAP used as a Debug Transport Module more tightly than IEEE 1149.1:

> "JTAG TAPs used as a DTM must have an IR of **at least 5 bits**. When the
> TAP is reset, IR must default to `00001`, selecting the IDCODE
> instruction."

Implementations are exactly 5 bits in practice (the PULP/OpenTitan TAP
included). The full instruction set (spec Table 7.1 / "JTAG DTM Registers"):

| IR value | Register | Size | Notes |
|---|---|---|---|
| `00001` (0x01) | IDCODE | 32 bits | reset default; standard layout with bit 0 = `1` |
| `10000` (0x10) | `dtmcs` | 32 bits | DTM control/status: `abits`, `idle`, sticky error status, `dmireset` |
| `10001` (0x11) | `dmi` | `34 + abits` bits | Debug Module Interface scan: 2-bit `op`, `abits`-bit address, 32-bit data |
| `11111` (0x1F) | BYPASS | 1 bit | and **all unused encodings select BYPASS** — spec-mandated |

How this lines up with IEEE 1149.1:

- BYPASS = all-ones (`11111`), as the standard mandates.
- Reset default `00001` = IDCODE, matching 1149.1's "IDCODE if an ID
  register exists" rule — a DTM without IDCODE is not a useful DTM, so the
  spec simply requires it, with the encoding `00…001` (LSB set).
- The "all unused encodings → BYPASS" rule is *stronger* than 1149.1,
  which only requires the all-ones pattern.

Two protocol-relevant facts (details in [jtag-dtm.md](jtag-dtm.md)):

- **`dtmcs` bootstraps the debugger**: reading it yields `abits` (the DMI
  address width — which fixes the `dmi` scan length at `34 + abits` bits),
  the `idle` requirement (extra `TCK`s needed between Update-DR and the
  next Capture-DR), and the sticky error bits.
- **`dmi` scans carry transactions**, not raw register images: each scan
  shifts in a request (`op`/address/data) on Update-DR and captures the
  result of the *previous* request on Capture-DR — a pipelined
  request/response protocol, so its shift content must never be treated as
  a static register value.

Worked example (tying together reset value, encoding, and shift order):
after a TAP reset, to select `dmi` on a single RISC-V TAP, shift IR value
`0x11` = `5'b10001` LSB-first into the 5-bit IR — the `TDI` stream is
`1, 0, 0, 0, 1`.

## Quick reference

| Fact | Value |
|---|---|
| IR after TAP reset | IDCODE if the device has an ID register, else BYPASS — never undefined |
| Reaching TLR | `TRST*`, or hold `TMS` high for ≥ 5 `TCK` cycles |
| BYPASS encoding | all-ones — mandated by IEEE 1149.1 |
| IDCODE instruction encoding | vendor-specific (`00001`/0x01 in the RISC-V DTM) |
| IDCODE DR layout | [31:28] version, [27:12] part number, [11:1] JEDEC manufacturer, [0] = `1` fixed |
| Shift direction | `TDI` → register → `TDO` only, for IR and every DR |
| Shift timing | `TDI` sampled on rising `TCK`, `TDO` changes on falling `TCK` |
| Register orientation | MSB nearest `TDI`, LSB nearest `TDO`; each shift is a right-shift |
| Bit order on the wire | LSB first, both IR and DR |
| Capture-IR pattern | bits [1:0] = `01` (mandated), upper bits design-specific (often 0); BSDL `INSTRUCTION_CAPTURE` |
| Active instruction readable? | No — only the Capture-IR pattern scans out; tools must track it |
| RISC-V DTM IR | ≥ 5 bits (5 in practice), reset value `00001` |
| RISC-V DTM encodings | `0x01` IDCODE, `0x10` `dtmcs`, `0x11` `dmi`, `0x1F` BYPASS (+ all unused → BYPASS) |
| `dmi` scan length | `34 + abits` bits (2 `op` + `abits` address + 32 data) |

## References

- IEEE Std 1149.1 (IEEE Standard Test Access Port and Boundary-Scan
  Architecture) — the JTAG standard itself.
- TI, *IEEE Std 1149.1 (JTAG) Testability Primer*
  ([ssya002c](https://www.ti.com/lit/an/ssya002c/ssya002c.pdf)) — IR/DR
  orientation (Fig. 3-4), IDCODE layout (Fig. 3-8).
- Lauterbach, *JTAG Interface Training*
  ([pdf](https://www2.lauterbach.com/pdf/training_jtag.pdf)) — LSB-first
  scan order, IR/DR shifting mechanics.
- RISC-V External Debug Support, [version 0.13.2]
  (https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
  and [version 1.0]
  (https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf)
  — DTM IR requirements and the JTAG DTM register table (the same files the
  [README](../README.md) cites).
- PULP RISC-V debug module RTL, vendored in OpenTitan as
  [pulp_riscv_dbg](https://github.com/lowRISC/opentitan/tree/master/hw/vendor/pulp_riscv_dbg/src)
  (`dmi_jtag_tap.sv`) — a production 5-bit-IR TAP; Capture-IR value
  `5'b00101`.
- OpenOCD [`riscv-013.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv-013.c)
  driver and the OpenOCD JTAG core (chain init, capture checks).
- Memfault, *Diving into JTAG — BSDL (Part 4)*
  ([blog](https://interrupt.memfault.com/blog/diving-into-jtag-part-4)) —
  BSDL `INSTRUCTION_CAPTURE` / opcode attributes walkthrough.