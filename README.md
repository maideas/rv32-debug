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
| [docs/abstract-commands.md](docs/abstract-commands.md) | The DM's high-level command interface: execution protocol and `cmderr` semantics, the three standard commands (Access Register, Quick Access, Access Memory) with full field layouts and hex examples, capability probing, 0.13.2-vs-1.0 differences — and the three sanctioned ways to extend with custom abstract commands |
| [docs/debug-mode.md](docs/debug-mode.md) | The CPU side of the contract: Debug Mode as an execution state — entry causes, halt behavior, ebreak re-routing, single-step, dret — and the complete wire-level DM↔hart interface (halt/resume/status lines, GPR/CSR/LSU debug ports) required by the minimal DM |
| [docs/dm-hart-interface.md](docs/dm-hart-interface.md) | The DM↔hart bundle as a standalone signal reference: every request/status line and data port, its meaning and handling rules — plus handshake timeline, reset interactions, and what the full spec adds |
| [docs/debug-sequences.md](docs/debug-sequences.md) | The usual debugger workflows as concrete, ready-to-use DMI operation listings: connect, halt, inspect, single-step, software breakpoints, block memory transfers (incl. auto-increment bursts), reset-and-debug, detach, error recovery — with all hex values spelled out |
| [docs/cpp-implementation.md](docs/cpp-implementation.md) | The reference C++ implementation of the debug sequences under `src/`: layering, sequence-to-method map, design decisions, the behavioral mock target, and how to run the self-checking demo |

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

## Reference C++ implementation

`src/` contains a dependency-free C++17 reference implementation of the
[debug sequences](docs/debug-sequences.md), layered exactly like the
documents — see [docs/cpp-implementation.md](docs/cpp-implementation.md)
for the full walkthrough:

| Layer | Files | What it is |
|---|---|---|
| Transport (abstract) | `src/jtag_transport.hpp` | `jtag_ir()` / `jtag_dr()` — implement this against your JTAG adapter or simulation model |
| DTM | `src/dtm.hpp`, `src/dtm.cpp` | `dmi_write()` / `dmi_read()` / `dmi_poll()`, the `dmi` op protocol, sticky busy recovery |
| Debug sequences | `src/sequences.hpp`, `src/sequences.cpp` | connect, halt, register/memory access, block transfers (incl. bursts), step, software breakpoints, reset, detach, error recovery |
| Mock target | `src/mock/` | behavioral TAP + DTM + minimal DM + simple hart (per the docs), with busy/failed injection |
| Demo | `src/demo.cpp` | runs every sequence against the mock and self-checks |

Build and run the demo (artifacts land in `build/`):

    make test

## Status

Documentation plus the reference C++ implementation above — no RTL in
this repository (yet). If you are implementing a DM, start with
[docs/minimal-dm.md](docs/minimal-dm.md) for the register set, follow
[docs/jtag-dtm.md](docs/jtag-dtm.md) for the transport underneath, and
use [docs/debug-sequences.md](docs/debug-sequences.md) as test-vector
material for your simulation — the sequences in `src/sequences.cpp`
drive exactly those values.