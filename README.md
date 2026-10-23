# RV32 Debug

A documentation set for **RISC-V RV32 external debug support** — the
hardware structures that let a debugger (GDB via OpenOCD, or similar)
halt, inspect, single-step, and resume a RISC-V hart over JTAG.

The documents describe the debug architecture of the RISC-V External
Debug Support specification (versions 0.13.2 and ratified 1.0), with all
register-level details verified against the spec text, production DTM RTL
(the PULP/OpenTitan debug module used in OpenTitan silicon), and the
OpenOCD `riscv-013` driver — i.e. what the spec says, what real hardware
implements, and what real debuggers actually do.

## The layered architecture in one paragraph

Debug hardware is layered so that transport, reusable debug logic, and
core-specific logic can be developed independently: a JTAG Test Access
Port (IEEE 1149.1) feeds a Debug Transport Module (DTM), which translates
JTAG scans into operations on a small bus — the Debug Module Interface
(DMI) — that reaches the Debug Module (DM). The DM is the transport-
independent "debug brain": it halts and resumes harts, services abstract
register/memory access commands, and drives reset. The hart itself adds
the core-side pieces — Debug Mode, `dcsr`/`dpc` debug CSRs, single-step,
`ebreak` re-routing, and the trigger module (hardware breakpoints/
watchpoints).

## Documents

Read roughly in this order; each links to the others.

| Document | What it covers |
|---|---|
| [docs/debug-architecture.md](docs/debug-architecture.md) | The full layered picture: JTAG TAP → DTM → DMI → DM → hart debug logic, every DM register, Debug Mode, debug CSRs, triggers, plus 0.13.2-vs-1.0 differences |
| [docs/jtag-dtm.md](docs/jtag-dtm.md) | Implementation guide for the transport layer: the 16-state TAP, IR/DR scan registers, `dtmcs`/`dmi` layouts, the sticky busy/failed `op` protocol, timing and `dtmcs.idle`, reset/recovery, the DMI bus with clock-domain crossing |
| [docs/minimal-dm.md](docs/minimal-dm.md) | The smallest usable DM register set (single-hart RV32): complete bit maps and field tables for `data0`/`data1`/`dmcontrol`/`dmstatus`/`abstractcs`/`command`, required protocol behaviors, coupled bits, and OpenOCD compatibility |
| [docs/debug-sequences.md](docs/debug-sequences.md) | The usual debugger workflows as concrete, ready-to-use DMI operation listings: connect, halt, inspect, single-step, software breakpoints, block memory transfers (incl. auto-increment bursts), reset-and-debug, detach, error recovery — with all hex values spelled out |

## Specifications and sources

- RISC-V External Debug Support:
  [0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
  (what deployed silicon implements) and
  [1.0 ratified](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf)
- PULP RISC-V debug module RTL, vendored in OpenTitan as
  [pulp_riscv_dbg](https://github.com/lowRISC/opentitan/tree/master/hw/vendor/pulp_riscv_dbg/src)
- OpenOCD
  [`riscv-013.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv-013.c)
  and its `debug_defines.h`

## Status

Documentation only — no RTL in this repository (yet). If you are
implementing a DM, start with [docs/minimal-dm.md](docs/minimal-dm.md)
for the register set, follow [docs/jtag-dtm.md](docs/jtag-dtm.md) for the
transport underneath, and use [docs/debug-sequences.md](docs/debug-sequences.md)
as test-vector material for your simulation.