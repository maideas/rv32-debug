# Toolchain Integration: Making Your Chip Work with OpenOCD, GDB, and SEGGER J-Link

The other documents in this survey describe what the debugger does *to* you:
the scans, commands, and register protocols of the debug spec. This one is
about what you must ship *to* the debugger when you build your own silicon:
an identity your chip reports correctly, debug logic that survives OpenOCD's
discovery sequence, the OpenOCD and GDB configuration for your part, and the
glue (breakpoints, reset, flash) that makes board bring-up work — plus, if
your field debugger is a SEGGER J-Link, what its *native* stack expects
instead (see [SEGGER J-Link](#segger-j-link)). It is the
difference between "a debug module that implements the spec" and "a chip a
customer can type `target remote :3333` into and debug".

References used: the RISC-V External Debug Support specification,
[version 0.13.2](https://github.com/riscv/riscv-debug-spec/raw/release/riscv-debug-release.pdf)
and
[version 1.0 ratified](https://github.com/riscv/riscv-debug-spec/releases/download/1.0/riscv-debug-specification.pdf);
the OpenOCD `riscv` target driver
([`src/target/riscv/riscv.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv.c),
[`riscv-013.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv-013.c),
[`riscv-013_reg.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv-013_reg.c),
[`riscv_reg.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/target/riscv/riscv_reg.c),
and [`src/server/gdb_server.c`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/server/gdb_server.c));
shipped OpenOCD target configs
([`esp32c3.cfg`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/tcl/target/esp32c3.cfg)
+ `esp_common.cfg`, `gd32vf103.cfg`, `vd_riscv.cfg`); GDB's RISC-V support
([`gdb/riscv-tdep.c`](https://sourceware.org/git/?p=binutils-gdb.git;a=blob_plain;f=gdb/riscv-tdep.c)
and the [RISC-V target
features](https://sourceware.org/gdb/current/onlinedocs/gdb/RISC_002dV-Features.html)
documentation); the RISC-V Privileged Architecture manual (the `marchid`
allocation rules); and JEDEC JEP106 (manufacturer ID assignment). For the
J-Link sections: SEGGER's [RISC-V support
overview](https://www.segger.com/products/debug-probes/j-link/technology/cpus-and-devices/risc-v-support/)
and [J-Link RISC-V knowledge base article](https://kb.segger.com/J-Link_RISC-V)
(spec versions, memory-access selection logic, TAP auto-detection,
limitations), the [J-Link Commander](https://kb.segger.com/J-Link_Commander)
and [J-Link GDB Server](https://kb.segger.com/J-Link_GDB_Server) references,
and a real [J-Link connect log of a 0.13 RISC-V
target](https://forum.segger.com/thread/7729-SOLVED-incorrect-decoding-of-RISC-V-instructions/)
from the SEGGER forum. All
behavior below was verified against those sources.

## Table of contents

- [The deliverables](#the-deliverables)
- [Identity: IDCODE, JEDEC, and the CSR identity block](#identity-idcode-jedec-and-the-csr-identity-block)
- [Discovery: what OpenOCD does at `init`](#discovery-what-openocd-does-at-init)
- [The OpenOCD config file for your chip](#the-openocd-config-file-for-your-chip)
- [Reset: the part you almost always have to script](#reset-the-part-you-almost-always-have-to-script)
- [Flash support](#flash-support)
- [GDB against the target](#gdb-against-the-target)
- [SEGGER J-Link](#segger-j-link)
  - [What the native stack supports](#what-the-native-stack-supports)
  - [Where the native driver differs from OpenOCD](#where-the-native-driver-differs-from-openocd)
  - [Using the native stack](#using-the-native-stack)
  - [What to ship for the J-Link ecosystem](#what-to-ship-for-the-j-link-ecosystem)
- [Bring-up checklist](#bring-up-checklist)
- [Related documents](#related-documents)

## The deliverables

"Toolchain support" for your RV32 chip is a stack of five deliverables, in
increasing order of effort:

| # | Deliverable | Where it comes from | Effort |
|---|---|---|---|
| 1 | Correct identity: IDCODE with a JEDEC manufacturer ID you own, accurate `misa`/`mvendorid`/`marchid`/`mimpid` | JEDEC registration, RISC-V International, your RTL | paperwork + a few CSRs |
| 2 | Self-describing debug logic that survives OpenOCD's discovery (see [below](#discovery-what-openocd-does-at-init)) | the other documents of this survey | already your project |
| 3 | An OpenOCD target/board config (`.cfg`) for your part — plus fixes for whatever it exposes (custom IRs, custom CSRs, broken reset) | you write a Tcl file | days |
| 4 | Flash support: `flash bank` config (reused or custom driver) + a flash loader | [flash-loaders.md](flash-loaders.md) | days |
| 5 | A GDB experience: registers under their real names, breakpoints/watchpoints that work everywhere, sane `load`/`reset` | falls out of 1–4 | mostly free |

The key design fact: **RISC-V targets need no per-chip debugger code for the
basics.** There is no "OpenOCD driver for the Foo3000" — one generic driver
(the `riscv` target type, mostly the `riscv-013.c` code) talks to every
spec-compliant DM, because the DM describes itself (through `dtmcs`,
`dmstatus`, `abstractcs`, `sbcs`, `hartinfo`, `tinfo`, …). Your chip-specific
surface is: a Tcl config, the identity values, and whatever is *not* in the
spec (custom IR encodings, custom CSRs, reset sequencing, flash). That is
what this document covers. SEGGER's native J-Link stack is the same idea
with a different config mechanism — one generic RISC-V driver plus
per-device *database entries* instead of Tcl — see
[SEGGER J-Link](#segger-j-link).

## Identity: IDCODE, JEDEC, and the CSR identity block

### IDCODE: the part you must order

The 32-bit IDCODE is laid out in [jtag-tap.md](jtag-tap.md): 4-bit version,
16-bit part number, 11-bit manufacturer identity, and the fixed LSB `1`. The
manufacturer field is not free text — it encodes your **JEDEC JEP106
manufacturer ID**, packed as follows:

- IDCODE bits [7:1] hold the JEP106 **ID byte**,
- IDCODE bits [11:8] hold the number of **0x7F continuation bytes** that
  precede the ID byte in the JEDEC sequence (the JEP106 "bank" minus one).

Two production examples, decoded with OpenOCD's own JEP106 table
([`src/helper/jep106.inc`](https://github.com/riscv-collab/riscv-openocd/blob/riscv/src/helper/jep106.inc)):

| Chip | IDCODE | version | part | manufacturer bits [11:1] | JEP106 entry |
|---|---|---|---|---|---|
| SiFive FE310 (HiFive1) | `0x20000913` | `0x2` | `0x0000` | `0x489` | 9 continuation bytes, ID byte `0x09` → "SiFive Inc" |
| Espressif ESP32-C3 | `0x00005c25` | `0x0` | `0x0005` | `0x612` | 12 continuation bytes, ID byte `0x12` → "Espressif Systems (Shanghai) Co Ltd" |

OpenOCD prints the decode on every connect, e.g. for the SiFive part (from
a real session log):

```
Info : JTAG tap: riscv.cpu tap/device found: 0x20000913
       (mfg: 0x489 (SiFive, Inc.), part: 0x0000, ver: 0x2)
```

To get here you order a manufacturer ID from JEDEC (the
[JEP106 order form](https://www.jedec.org/standards-documents/id-codes-order-form);
assigned and maintained by the JEDEC office). **Do not ship someone else's
code.** The default IDCODE of Chisel/FPGA soft cores has bitten teams here:
rocket-chip's default `0x10e31913` uses SiFive's assigned JEDEC ID, and the
maintainer's answer to "should the default be 0x1?" was that "those who
aren't SiFive should not be using that IDCODE"
([rocket-chip #774](https://github.com/chipsalliance/rocket-chip/issues/774)).
For an FPGA prototype before registration, `0x00000001` (version 0, part 0,
no manufacturer) is the common placeholder — a config `-expected-id` will
still catch TAP wiring mistakes.

The 16-bit part number is yours; a common scheme encodes the product line
(so `-expected-id` per part stays stable across speed grades) and puts
mask/silicon-revision differences in the 4-bit version.

### `misa`: the CSR the debugger actually reads

Of the identity CSRs, **`misa` (0x301) is the one the toolchain depends on**.
At target examine OpenOCD reads it and:

- cross-checks the MXL field against the XLEN it detected through the DM
  (see [below](#discovery-what-openocd-does-at-init)) — `misa == 0` draws
  `"Read as zero. OpenOCD will not be able to determine some hart's
  capabilities"`, and an MXL that contradicts the detected DXLEN is a hard
  error (`exceeds the detected DXLEN`);
- uses the extension letters to decide **which registers exist**: the GDB
  register list is gated on `misa` bits — F/D gates the FPU registers and
  `fflags`/`frm`/`fcsr`, S/U gate the supervisor/user CSRs and CSRs like
  `medeleg`, H gates the hypervisor CSRs, V gates the vector registers.
  Report an extension you don't implement and GDB will list registers whose
  reads then fail; report too little and users can't `p` registers you do
  have;
- uses the S/U bits when programming trigger mode bits (`mcontrol.s`,
  `mcontrol.u`, …) so hardware breakpoints match in the right privilege
  modes.

So on an industrial RV32IMAC part: implement `misa` as MXL=32 (RV32) with
exactly your extension bitmap, and make it a real read-only CSR reachable
by abstract Access Register (or the progbuf `csrr` fallback — both are
probed automatically).

### `mvendorid` / `marchid` / `mimpid`: checked less, but ship them

OpenOCD and GDB do not consult these for configuration (verified: neither
appears in the `riscv` target driver), but they are the identity that
*field diagnostics*, kernels, and OpenSBI log. The Privileged Architecture
sets the rules:

- `mvendorid` (0xF11): your JEDEC manufacturer ID — the same JEP106 number
  as the IDCODE (packed `(bank-1) << 7 | id`), so order it once, use it
  everywhere.
- `marchid` (0xF12): identifies the microarchitecture. **Open-source core
  architectures get IDs allocated globally by RISC-V International** (MSB
  clear, nonzero); commercial vendors allocate their own independently,
  with the MSB set. A commercial fabrication of an open core should keep
  the open core's `marchid` (attribution, and often license, demand it).
  0 means "not implemented".
- `mimpid` (0xF13): your version/revision encoding. 0 means "not
  implemented".

All three must be readable; all are MXLEN-bit read-only.

## Discovery: what OpenOCD does at `init`

When OpenOCD processes `init` (or a gdb-attach), the `riscv-013` driver runs
a fixed discovery sequence against your TAP/DTM/DM. This is the
compatibility contract your debug logic must pass — every step maps to a
document in this survey. SEGGER's native J-Link driver runs a discovery of
the same shape; where it differs is covered in
[SEGGER J-Link](#segger-j-link) below.

| # | What OpenOCD does | What your hardware must do | Details in |
|---|---|---|---|
| 1 | TAP reset, scan IDCODE, match against `jtag newtap -expected-id` | BYPASS-on-reset IR, a real IDCODE | [jtag-tap.md](jtag-tap.md) |
| 2 | Read `dtmcs` — check `version=1`, learn `abits` and `idle` | DTM with spec IR encodings `IDCODE=0x01`, `DTMCS=0x10`, `DMI=0x11` and sticky `op` semantics | [jtag-dtm.md](jtag-dtm.md) |
| 3 | Bring up the DM: `dmcontrol.dmactive=1`, poll `dmstatus` sanity | `dmactive` reset-domain semantics, sane `dmstatus` | [minimal-dm.md](minimal-dm.md) |
| 4 | Enumerate harts: step `hartsel` (LO+HI, up to 20 bits) and poll `dmstatus.anynonexistent` until it is set; `ackhavereset` each hart that reports `havereset` | report `nonexistent` for absent harts, correct `havereset`/`resumeack` handshake | [dm-hart-interface.md](dm-hart-interface.md) |
| 5 | Detect XLEN: issue abstract Access Register for `s0` with **`aarsize=64`** — success ⇒ RV64, `cmderr` ⇒ RV32; then read `misa` and cross-check MXL | correct `aarsize` checking (reject mismatched sizes with `cmderr=2 notsupported`) | [abstract-commands.md](abstract-commands.md) |
| 6 | Probe capabilities: `abstractcs`/`progbufsize`, the `0x07FF_FFC1` hartsel read-coupling probe, trigger inventory (`tselect`/`tdata1`/`tinfo`), `sbcs` for SBA | only claim what you implement; read-back rules for coupled bits | [minimal-dm.md](minimal-dm.md), [debug-sequences.md](debug-sequences.md) |
| 7 | Build the GDB register cache from all of the above; serve it to GDB as a target description over `qXfer:features` | nothing extra — but see custom CSRs below | this document |

Notes worth knowing because they are *not* in the spec:

- **The `dtmcs.idle` value you publish is only a hint.** OpenOCD starts
  optimistic and *learns*: on every sticky busy it clears the `dmi` op with
  `dtmcs.dmireset` and bumps the number of Run-Test/Idle cycles it inserts
  between `dmi` scans in small steps (`increase_dmi_busy_delay()` →
  `riscv_scan_increase_delay()` in `riscv-013.c`/`batch.h`; the `riscv
  reset_delays` command exists to reset the learned values). Publishing a
  too-small `idle` therefore still works — just with slow first sessions
  and busy messages your logs will scare people with. Publishing a correct
  value keeps scans at full speed from the start.
- **Hart enumeration is a linear scan**, not a report: OpenOCD probes
  `hartsel` 0, 1, 2, … until `dmstatus.anynonexistent` reads 1 (up to
  2^20 possible `hartsel` values). A DM that reports a nonexistent hart as
  existing (or vice versa) breaks multi-hart targets; single-hart parts are
  naturally immune. Each `-coreid N` OpenOCD target binds to hart N of that
  scan.
- **XLEN detection is a 64-bit probe, always.** Every RV32 target therefore
  must reject `aarsize=64` Access Register commands with `cmderr=not
  supported` — if you size-check against a hard-wired 32 and return `ok`,
  OpenOCD concludes RV64 and every later register access corrupts.
- On success, examine ends with the log line regression suites rely on:
  `Examined RISC-V core` followed by ` XLEN=32, misa=0x40801105` (your
  values — the example is RV32IMAC). If you support that ecosystem, don't
  change the wording of your own tooling — but don't *rely* on OpenOCD's
  wording either.

### Custom registers and CSRs

OpenOCD builds the GDB register list from a built-in table of standard CSRs
(gated by `misa`, per step 7 above). Anything custom needs explicit
exposure in the config, *before* `init`:

```
# expose CSR 0x7c0-0x7cf (a company-standard debug control block),
# naming 0x7c0 "dbgctl":
riscv expose_csrs 0x7c0=dbgctl,0x7c1-0x7cf
```

The same mechanism exists for non-CSR debug-visible registers: custom
abstract registers `0xc000`–`0xcfff` (the `custom0`… extension point of
[abstract-commands.md](abstract-commands.md)):

```
riscv expose_custom 0=my_ip_status,1-3
```

GDB then shows them as registers (`info registers my_ip_status`), read
and written through abstract Access Register with your `regno`. There is
also `riscv hide_csrs` to suppress registers that exist but shouldn't be
advertised (e.g. counters behind `mcounteren`).

## The OpenOCD config file for your chip

Everything below this point is Tcl, shipped as `target/yourchip.cfg` (the
board/chip part; the *interface* — JTAG adapter — is a separate cfg the
customer picks). Real shipped configs are the best template; here is a
composite, built from `vd_riscv.cfg`, `gd32vf103.cfg` and
`esp32c3.cfg`/`esp_common.cfg`, annotated:

```tcl
# --- interface (usually shipped separately / chosen by the user) ---------
adapter driver jtag
transport select jtag
adapter speed 4000

# --- the TAP(s) --------------------------------------------------------
set _CHIPNAME mychip
# One TAP: our CPU's DTM. Expected ID: your JEDEC-registered IDCODE
# (placeholder 0x00000001 until you have one).
jtag newtap $_CHIPNAME cpu -irlen 5 -expected-id 0x00000001
# A SoC with a second TAP (e.g. boundary scan, like the GD32VF103's
# '-irlen 5 -expected-id 0x790007a3' bs tap) just adds more newtap lines.

# --- the target(s) ----------------------------------------------------
set _TARGETNAME $_CHIPNAME.cpu
target create $_TARGETNAME riscv -chain-position $_TARGETNAME \
    -coreid 0
#    -coreid N selects hart N: a multi-hart SoC creates one OpenOCD
#    target per hart ('-coreid 0', '-coreid 1', ...; the ESP configs
#    also join them with 'target smp'). '-rtos hwthread' exposes the
#    extra harts to GDB as threads instead of separate targets.

# --- debugger behavior -------------------------------------------------
# Memory access preference: which mechanism to try, in order.
riscv set_mem_access sysbus progbuf abstract
# Wall-clock patience for single debugger commands (flash erase on slow
# targets!); ESP32 defaults: 5 s. Silicon on long reset paths needs
# much more (Cadence 'vd_riscv' ships 120 s).
riscv set_command_timeout_sec 5
# Prefer hardware breakpoints everywhere (needed while code runs from
# read-only flash; see below):
gdb_breakpoint_override hard

# --- working RAM for downloaded algorithms (flash loaders!) -----------
# OpenOCD carves target RAM here for its flash loaders
# (see the flash-loaders document).
$_TARGETNAME configure -work-area-phys 0x20000000 \
    -work-area-size 0x1800 -work-area-backup 0

# --- flash --------------------------------------------------------------
# Reuse a compatible existing driver where possible: the GD32VF103
# (RISC-V!) reuses the 'stm32f1x' driver, because flash drivers talk to
# memory + algorithm, not to the CPU ISA:
flash bank $_CHIPNAME.flash myflash 0x08000000 0 0 0 $_TARGETNAME

# --- events --------------------------------------------------------------
$_TARGETNAME configure -event gdb-attach { halt; echo "hi" }
# ... reset-assert/reset-assert-post: see the reset section below.
```

### The `riscv` command group

The commands you are most likely to need (all verified in
`riscv.c`'s handler table):

| Command | What it does |
|---|---|
| `riscv set_ir idcode|dtmcs|dmi <value>` | Override the DTM IR encodings. Needed when your DTM puts `IDCODE`/`dtmcs`/`dmi` elsewhere than the spec's `0x01`/`0x10`/`0x11` (e.g. a chain-wide 8-bit IR requirement). |
| `riscv set_mem_access [sysbus] [progbuf] [abstract]` | Memory-access mechanism preference, in order. `sysbus` needs SBA; `progbuf`/`abstract` need the hart halted. |
| `riscv set_command_timeout_sec <sec>` | Timeout for each debugger command (halt, memory access, flash loader run). |
| `riscv expose_csrs <ranges>` / `riscv expose_custom <ranges>` / `riscv hide_csrs <ranges>` | Custom register exposure — see [above](#custom-registers-and-csrs). |
| `riscv dmi_read` / `riscv dmi_write` / `riscv dm_read` / `riscv dm_write` | Raw DMI/DM register access from Tcl — the escape hatch for anything the spec's registers don't cover (the ESP32-C3 reset script uses `dmi_write` on `dmcontrol`). |
| `riscv authdata_read` / `riscv authdata_write` | Access `authdata` — the hook for your authentication scheme (e.g. read a challenge, write a response). |
| `riscv reserve_trigger <index> on` | Keep trigger N for firmware's own use; OpenOCD will not touch it. |
| `riscv set_maskisr off|steponly` | Mask interrupts during single-step (a GDB `step` should not land in your handler). |
| `riscv virt2phys_mode sw|hw|off` | Address translation for `info proc`/vaddr lookups (RV32IMAC: `off`). |
| `riscv autofence on|off` | Whether OpenOCD inserts `fence` after accesses (turn off only with a coherent design). |
| `riscv use_bscan_tunnel <value>` | Reach the DM through a Xilinx BSCAN tunnel (FPGA prototypes where JTAG goes through the FPGA's USER TAP). |
| `riscv info` | Dump everything discovery learned about the target. |

### Breakpoints and triggers

GDB's software breakpoints are **memory writes**: OpenOCD reads the
instruction word, replaces it with `EBREAK` (`0x00100073`) — or
`C.EBREAK` (`0x9002`) for a compressed instruction slot — and restores it
on continue. Consequences you must design for:

- Software breakpoints need **writable, executable memory**: they fail on
  code in flash. GDB falls back to hardware breakpoints (RSP `Z1`, using
  your trigger module, see [debug-architecture.md](debug-architecture.md))
  when `gdb_breakpoint_override hard` is set, when the breakpoint address
  is inside a `flash bank` region, or when GDB itself decides. GDB
  compresses breakpoints based on the **opcode it reads** —
  `riscv_breakpoint_kind_from_pc()` inspects the low bits to pick kind 2
  (`C.EBREAK`) vs kind 4 (`EBREAK`); `set riscv use-compressed-breakpoints
  off` forces 4-byte breakpoints if your core doesn't support re-routing
  compressed `c.ebreak` (with `dcsr.ebreakm` set, it must — see
  [debug-mode.md](debug-mode.md)).
- Watchpoints go through the trigger module as well (`Z2`/`Z3`/`Z4`).
  Your trigger count and match modes (`maskmax`, address/data compare)
  directly determine how many `watch`/`awatch`/`rwatch` GDB can install.
- If firmware uses triggers itself (industrial fault-tripping on
  `textra`/`mcontrol`), reserve its slots with `riscv reserve_trigger`.

## Reset: the part you almost always have to script

`reset` (and `reset halt`) uses the DM by default: assert reset
(`dmcontrol.ndmreset`, plus `haltreq` for `reset halt`), wait, deassert,
and `ackhavereset`. `ndmreset` is *your* wiring: the DM bit must actually
reset the parts of your SoC that a user expects `reset` to reset — see the
reset-and-debug sequence in [debug-sequences.md](debug-sequences.md) for
the DMI-level flow.

Two production examples of what happens when the default isn't enough:

- **GD32VF103**: `ndmreset` doesn't work at all on the chip (it doesn't
  reset the system), so the shipped config overrides
  `-event reset-assert` with a Tcl script that halts the core, unlocks and
  writes an *undocumented SoC register* (`0xe0042008`) to trigger reset,
  and even switches `riscv set_mem_access abstract` mid-script because the
  progbuf readback fails while the core is in reset. That is the level of
  workaround real silicon sometimes needs.
- **ESP32-C3**: reset is a *resumed-code* operation (writing an RTC
  register from the target), scripted in `-event reset-assert-post` with
  raw `riscv dmi_write` + `mww` — plus watchdog disable in the
  `-event halted` handler, because a watchdog firing during a debug halt
  would reset the part under the debugger. **Silence your watchdogs when
  halted** — a classic industrial-grade rule that isn't in the spec.

`reset_config` ties in a hardware SRST line if your board has one
(`reset_config srst_nogate` on the GD32 config) — then OpenOCD asserts
SRST instead of `ndmreset` where possible.

## Flash support

GDB's `load` and OpenOCD's `program` both route flash writes through
OpenOCD's **flash drivers**, reached through the GDB server's
`qXfer:memory-map:read` (a memory map synthesized from your `flash bank`
declarations) and `vFlashWrite` packets — verified in `gdb_server.c`. A
`flash bank` entry is therefore what makes `load` "just work", and what
tells GDB/OpenOCD which addresses are read-only (and hence need hardware
breakpoints).

Your options, in increasing effort:

1. **Reused driver.** Flash controllers that are register-compatible with
   an existing driver cost you one `flash bank` line — the RISC-V
   GD32VF103 literally uses `flash bank ... stm32f1x ...` (the ST driver,
   on a RISC-V target, via the flash loader mechanism of
   [flash-loaders.md](flash-loaders.md)).
2. **Generic SPI driver.** `jtagspi` covers many QSPI-attached flashes
   driven through a few controller registers; config-only.
3. **Custom driver + loader.** A `src/flash/nor/mychip.c` driver plus the
   target-side loader binary — the full contract is in
   [flash-loaders.md](flash-loaders.md); the data-movement half benefits
   from SBA ([minimal-dm.md](minimal-dm.md)).

One shipped trick worth copying: the GD32VF103 config adds a **virtual
flash alias** (`flash bank ... virtual 0x0 0 0 0 $_TARGETNAME
$_CHIPNAME.flash`) for the boot-mapped mirror at address 0, so OpenOCD
knows the mirror is read-only and doesn't try to insert software
breakpoints into flash-resident code there.

## GDB against the target

The GDB side of the fence is small because OpenOCD serves everything
dynamically:

- Connect: `target remote localhost:3333` (OpenOCD's ports: 3333 GDB,
  4444 telnet, 6666 Tcl). No `set architecture riscv:rv32` needed — the
  target description arrives over `qXfer:features:read` (OpenOCD
  generates the XML for the `org.gnu.gdb.riscv.cpu`/`fpu`/`csr`/
  `vector`/`virtual` features from its register cache). Talking to a
  stub that has no description is the only case that needs the manual
  `set architecture`.
- Registers: `info registers` shows GPRs, FPRs and (via the `csr`
  feature) every standard CSR OpenOCD decided exists — plus your
  `expose_csrs`/`expose_custom` registers by name. `set riscv
  numeric-register-names on` switches to `x1`-style display.
- Escape hatches: any OpenOCD command runs under GDB as
  `monitor <cmd>` — `monitor reset halt`, `monitor riscv info`, `monitor
  reg mstatus`. `maintenance packet X` sends raw RSP packets when
  debugging the protocol itself.
- I/O: RISC-V semihosting reuses the *ARM* semihosting protocol, and
  (deliberately, for tool compatibility) the OpenOCD commands keep the
  `arm` prefix: `monitor arm semihosting enable` — after which
  `printf`-style semihosting calls in your target code appear on the
  debugger console. (The drivers live in `riscv_semihosting.c`; the
  command-prefix compatibility is documented in `riscv.c`.)

## SEGGER J-Link

A J-Link probe — the most common commercial debugger — can be used in two
fundamentally different ways, and the one your users pick changes what you
must ship:

| Path | Who drives the DM | What you configure |
|---|---|---|
| **J-Link as a plain USB JTAG adapter** | OpenOCD's `riscv-013` driver — everything above applies unchanged | `adapter driver jlink` in the interface cfg; OpenOCD drives the probe over libusb, no SEGGER software needed |
| **The SEGGER native stack** — `JLinkExe`, `JLinkGDBServer`, Ozone, J-Flash | SEGGER's built-in RISC-V driver inside the J-Link software (the `JLinkARM` DLL) | a **device name** from SEGGER's device database (or the generic `RV32`/`RV64`), plus a `.JLinkScript` where the part needs help |

The native stack is what a customer "with just a J-Link" gets out of the
box. It is closed source, but it is not magic: it implements the same
specification this survey documents, issues the same DMI operations
([debug-sequences.md](debug-sequences.md)), and its contract is public —
the facts below are from SEGGER's own support documentation.

### What the native stack supports

- **Spec versions: 0.11, 0.13.2, and ratified 1.0.** One explicit caveat:
  *pre-ratification* drafts of 1.0 (`1.0.0-STABLE`, `1.0.0-rc1`) are called
  out by SEGGER as possibly incompatible. Implement plain 0.13.2 or plain
  1.0, nothing in between (the deltas:
  [debug-architecture.md](debug-architecture.md)).
- **RV32 and RV64**, over **JTAG (IEEE 1149.1)** and **cJTAG
  (IEEE 1149.7)**, plus a third topology the RISC-V spec does not define:
  **RISC-V behind a CoreSight DAP** — DMI registers memory-mapped behind
  an AP (SWD/JTAG → SWJ-DP → AHB-/APB-AP → DMI), which hybrid
  ARM+RISC-V SoCs use so both kinds of core share one debug connector.
  A plain RV32 part implements none of the extras and doesn't need to.
- **Run control and breakpoints:** halt/resume/single-step, software
  breakpoints (`ebreak` patching — and SEGGER's "Unlimited Flash
  Breakpoints", software breakpoints *in* flash, is supported for
  RISC-V), hardware breakpoints (`mcontrol`/`mcontrol6`), and data
  breakpoints in the common match combinations (exact/exact,
  exact/any, any/exact of address/data — not address or data ranges).
  Not driven: `itrigger`, `etrigger`, `tmexttrigger`.
- **One hart at a time.** J-Link debugs a single hart per session (the
  hart is user-selectable — "user-controlled hart selection" in SEGGER's
  docs); halt/resume *groups* and multiple DMs are not supported.
  OpenOCD instead exposes every hart as its own GDB target. For the
  single-hart RV32 profile of this survey the difference is invisible.

That its discovery is the same shape as OpenOCD's is visible in a real
connect log (a 0.13 target, from the SEGGER forum — annotated by this
survey's documents):

```text
Debug architecture: RISC-V debug: 0.13   ← dmstatus.version = 2 (0.13)
AddrBits: 7                              ← dtmcs.abits
DataBits: 32                             ← detected XLEN (RV32 core)
IdleClks: 1                              ← dtmcs.idle
Memory access:
  Via system bus: Yes (32-bit accesses are supported)
                                         ← sbcs.sbasize / sbaccess32
  Via ProgBuf: Yes (8 ProgBuf entries)   ← abstractcs.progbufsize
DataBuf: 2 entries                        ← abstractcs.datacount
autoexec[0] implemented: Yes             ← abstractauto
Detected: RV32 core
CSR access via abs. commands: No         ← probed; progbuf csrr instead
Temp. halted CPU for NumHWBP detection   ← trigger module probing
HW instruction/data BPs: 2
RISC-V identified.
```

### Where the native driver differs from OpenOCD

**Memory access: same progbuf-first default, no override.** J-Link's
documented selection logic and OpenOCD's defaults (`riscv.c`:
progbuf → sysbus → abstract) agree more than they differ:

| Hart state | J-Link picks, in order | OpenOCD default |
|---|---|---|
| halted | progbuf (`progbufsize > 0`) → SBA (`sbcs.sbasize > 0`) → Access Memory | progbuf → sysbus → abstract |
| running | SBA, else nothing | sysbus (SBA), else nothing |

The differences that matter: OpenOCD lets you reorder
(`riscv set_mem_access sysbus progbuf abstract` — the ESP32 configs do);
J-Link has no override option (SEGGER documents one as "planned"). And
J-Link drives SBA through `sbcs` with per-width `sbaccess` values, so the
`sbaccess8/16/32/…` capability bits are a real interop surface, not
decoration — report exactly the widths your system bus serves
(PULP-derived RTL reports only its native width, which is the pattern to
copy).

Two design consequences either way: **live memory access while the hart
runs needs SBA** — neither progbuf nor abstract commands work in the
background — and a DM with *only* Access Memory (the minimal DM of
[minimal-dm.md](minimal-dm.md)) works, but only while halted.

**The JTAG chain must be self-describing.** J-Link auto-selects which TAP
to drive: a TAP with `IRlen = 5` and a *known* RISC-V IDCODE wins; else
a TAP with `IRlen = 4` and a known CoreSight DAP IDCODE
(RISC-V-behind-DAP assumed); else — if the chain has exactly one TAP
with `IRlen` 4 or 5 — that one. Since "for RISC-V almost every vendor
does their own TAPId" (SEGGER's words), anything else needs manual
selection: `JTAGConf <IRPre>,<DRPre>` in the commander (`-jtagconf` on
the command line; `IRPre` = sum of the IR lengths of all TAPs closer to
TDI, `DRPre` = their count — the same numbers an OpenOCD chain
configuration implies), or a `.JLinkScript` pinning the chain. Practical
rules: ship the spec's 5-bit IR and a real IDCODE
([jtag-tap.md](jtag-tap.md)); multi-TAP boards ship a script.

**No generic cache maintenance.** SEGGER explicitly does not drive
`fence.i`/CMO-style maintenance generically — on officially supported
devices the device entry carries it, on custom parts you may need a
script. This bites exactly where software breakpoints on cached code do
([debug-mode.md](debug-mode.md)): a patched `c.ebreak` the I-cache never
sees looks precisely like "breakpoint not hit".

### Using the native stack

J-Link Commander is the ad-hoc bring-up tool (the native-stack equivalent
of OpenOCD's telnet). A spec-conformant part needs no device entry — the
generic `RV32` name covers it (this is how VexRiscv FPGA builds are
debugged):

```console
$ JLinkExe -device RV32 -if JTAG -speed 4000 -jtagconf -1,-1
J-Link> connect
J-Link> h                         # halt — parks in Debug Mode
J-Link> regs                      # register dump (abstract Access Register)
J-Link> mem32 0x20000000 10       # memory
J-Link> w4 0x20000000 0x12345678  # memory write
J-Link> g                         # resume
```

A production part uses its device name — the database entry encodes IR
length, register set, flash loader, and init steps, which is what makes
flash download and Unlimited Flash Breakpoints work without any config
file of yours:

```console
$ JLinkGDBServer -device GD32VF103CBT6 -if JTAG -speed 4000 -port 2331
```

- GDB then connects with `target remote :2331` (J-Link defaults: 2331
  GDB, 2332 SWO, 2333 telnet — versus OpenOCD's 3333/4444/6666). The
  register description is served by the GDB server, same contract as
  OpenOCD's `qXfer:features`.
- GDB escape hatches are `monitor` commands: RISC-V-specific
  `monitor ReadCSR` / `monitor WriteCSR` for raw CSR access, plus the
  generic `monitor regs` / `monitor memU32` / `monitor halt` /
  `monitor go` / `monitor step` / `monitor reset` / `monitor setbp`.
- **Custom silicon** has three routes, in increasing officialdom:
  1. the generic `RV32`/`RV64` device names — enough for
     spec-conformant single-DM parts;
  2. a `.JLinkScript` you ship next to the binary — manual chain setup
     and `ConfigTargetSettings()`-style init (a behind-DAP part *must*
     map its APs and DMI base here: no ROM table scan exists for
     RISC-V);
  3. a real device-database entry, which SEGGER adds on request — the
     "officially supported device" path, with flash loader, init steps,
     and cache handling baked in (ESP32-C3-class support, for example,
     arrived this way in J-Link V8.10).
- Ozone (SEGGER's standalone source-level debugger) and J-Flash (flash
  programming) sit on the same device database — one entry serves all
  of them.

### What to ship for the J-Link ecosystem

Same five deliverables as [the top of this document](#the-deliverables),
different config mechanism:

| Deliverable | OpenOCD | J-Link native |
|---|---|---|
| Target description | your `target/yourchip.cfg` (you write it) | device DB entry (SEGGER adds it on request) or generic `RV32` |
| Custom init / quirks | `-event` Tcl scripts | `.JLinkScript` |
| Flash | `flash bank` + loader ([flash-loaders.md](flash-loaders.md)) | flash loader inside the device entry |
| Chain quirks | `jtag newtap -irlen/-expected-id` | `-jtagconf` / script |
| Identity (IDCODE, `misa`, …) | identical, see [above](#identity-idcode-jedec-and-the-csr-identity-block) | identical |

The register-level contract underneath is the same either way — which is
the point of the spec: [minimal-dm.md](minimal-dm.md) and
[debug-sequences.md](debug-sequences.md) describe what *both* drivers'
scans compile down to.

## Bring-up checklist

Order your first silicon bring-up the way the tools probe it; each step
says what you expect to see and which document explains the failure:

| Step | Expect | On failure, see |
|---|---|---|
| 1. Adapter + TAP wiring | `tap/device found: 0x…` with your IDCODE, no chain errors | [jtag-tap.md](jtag-tap.md) |
| 2. `-expected-id` sanity | IDCODE decodes to *your* JEDEC code in the log | [jtag-tap.md](jtag-tap.md) |
| 3. `dtmcs` read (`riscv dm_read`/direct scan) | `version=1`, plausible `abits`/`idle` | [jtag-dtm.md](jtag-dtm.md) |
| 4. DM comes up: `dmactive=1` | `dmstatus` reads sane; `allhalted/allrunning` track reality | [minimal-dm.md](minimal-dm.md) |
| 5. Halt / resume | `halt` → park/quiesce, `allhalted=1`; `resume` → running | [debug-sequences.md](debug-sequences.md) |
| 6. Registers | `XLEN=32, misa=0x…` in the examine log — *your* misa | [abstract-commands.md](abstract-commands.md) |
| 7. Memory | `mdw` works; `riscv info` shows which mechanism was chosen | [debug-sequences.md](debug-sequences.md) |
| 8. Breakpoints | software `break` in RAM; hardware (flash) via triggers; both compressed and uncompressed slots | [debug-mode.md](debug-mode.md), [debug-architecture.md](debug-architecture.md) |
| 9. Reset | `reset halt` parks at your reset vector, repeatedly; watchdog silent while halted | [debug-sequences.md](debug-sequences.md) |
| 10. Flash | `program foo.elf` or GDB `load` completes; verify a `load`ed image runs | [flash-loaders.md](flash-loaders.md) |
| 11. Native J-Link stack, if that is the field tool | `JLinkExe -device RV32` connects; `h`, `regs`, `mem32` behave; flash download with a real device name | [SEGGER J-Link above](#segger-j-link) |

Finally, before shipping the config: reconnect ten times in a row
(`shutdown` + `init` cycles), across TCK speeds, with and without GDB —
production users will do all of that in week one.

## Related documents

| Document | What it covers relative to this one |
|---|---|
| [minimal-dm.md](minimal-dm.md) | The DM register set that OpenOCD's discovery (this document, step table) probes register by register |
| [debug-sequences.md](debug-sequences.md) | The DMI operation listings the OpenOCD sequences in this document compile down to |
| [jtag-tap.md](jtag-tap.md) / [jtag-dtm.md](jtag-dtm.md) | The IDCODE layout referenced for JEDEC allocation, and the `dtmcs.idle`/IR defaults OpenOCD's step 2 depends on |
| [abstract-commands.md](abstract-commands.md) | The Access Register mechanics behind XLEN detection, register access, and `expose_custom` |
| [debug-mode.md](debug-mode.md) | `ebreak` re-routing and trigger behavior behind GDB's breakpoints |
| [flash-loaders.md](flash-loaders.md) | The loader binaries and algorithms your flash driver runs |
| [cpp-implementation.md](cpp-implementation.md) | A mock-target harness for exercising these tool-side flows against the reference sequences |
