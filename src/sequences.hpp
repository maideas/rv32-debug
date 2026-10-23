// rv32-debug — the debugger workflows from docs/debug-sequences.md,
// implemented one-to-one on top of JtagDtm's dmi_write() / dmi_read() /
// dmi_poll().
//
// Register set targeted: the minimal DM of docs/minimal-dm.md (single-hart
// RV32, data0/data1, dmcontrol, dmstatus, abstractcs, command — no trigger
// module, no Program Buffer, no System Bus Access), plus the optional
// burst extension (aampostincrement + abstractauto) for block transfers.
//
// Every method carries a comment naming the section of
// docs/debug-sequences.md it implements; all magic values are spelled
// out there and kept as named constants here.
#ifndef RVDBG_SEQUENCES_HPP
#define RVDBG_SEQUENCES_HPP

#include "dtm.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>

namespace rvdbg {

// ---- DMI register addresses (docs/debug-sequences.md register table) ----
constexpr uint32_t DMI_DATA0 = 0x04;
constexpr uint32_t DMI_DATA1 = 0x05;
constexpr uint32_t DMI_DMCONTROL = 0x10;
constexpr uint32_t DMI_DMSTATUS = 0x11;
constexpr uint32_t DMI_HARTINFO = 0x12;   // probed, reads 0 on the minimal DM
constexpr uint32_t DMI_ABSTRACTCS = 0x16;
constexpr uint32_t DMI_COMMAND = 0x17;
constexpr uint32_t DMI_ABSTRACTAUTO = 0x18;  // only with the burst extension
constexpr uint32_t DMI_SBCS = 0x38;          // probed, reads 0 (no SBA)

// ---- dmcontrol bits / values ----
constexpr uint32_t DMCONTROL_HALTREQ = 1u << 31;
constexpr uint32_t DMCONTROL_RESUMEREQ = 1u << 30;
constexpr uint32_t DMCONTROL_ACKHAVERESET = 1u << 28;
constexpr uint32_t DMCONTROL_NDMRESET = 1u << 1;
constexpr uint32_t DMCONTROL_DMACTIVE = 1u << 0;
// Feature probe (exactly what OpenOCD writes): hasel (bit 26) | all 20
// hartsel bits (25:16, 15:6) | dmactive — request bits stay 0.
constexpr uint32_t DMCONTROL_FEATURE_PROBE = 0x07FF'FFC1u;
// hasel + hartsello/hartselhi in the readback (read 0 => HARTSELLEN=0 =>
// single hart).
constexpr uint32_t DMCONTROL_HARTSEL_MASK = 0x07FF'FFC0u;

// ---- dmstatus masks ----
constexpr uint32_t DMSTATUS_ALLRESUMEACK = 1u << 17;   // 0x20000
constexpr uint32_t DMSTATUS_ANYNONEXISTENT = 1u << 14;  // 0x4000
constexpr uint32_t DMSTATUS_ALLUNAVAIL = 1u << 13;      // 0x2000
constexpr uint32_t DMSTATUS_ALLRUNNING = 1u << 11;     // 0x800
constexpr uint32_t DMSTATUS_ALLHALTED = 1u << 9;       // 0x200
constexpr uint32_t DMSTATUS_AUTHENTICATED = 1u << 7;  // 0x80
constexpr uint32_t DMSTATUS_VERSION = 0xFu;            // 0x2 = spec 0.13

// ---- abstractcs ----
constexpr uint32_t ABSTRACTCS_BUSY = 1u << 12;    // 0x1000
constexpr uint32_t ABSTRACTCS_CMDERR = 0x700u;     // bits 10:8
constexpr uint32_t ABSTRACTCS_CMDERR_CLEAR = 0x700u;  // W1C "any"

// ---- abstract-command regnos (spec Table 3.3) ----
constexpr uint16_t REGNO_GPR_BASE = 0x1000;  // xN = 0x1000 | N
constexpr uint16_t REGNO_MSTATUS = 0x300;
constexpr uint16_t REGNO_MISA = 0x301;  // C extension = bit 2
constexpr uint16_t REGNO_MEPC = 0x341;
constexpr uint16_t REGNO_MCAUSE = 0x342;
constexpr uint16_t REGNO_MTVAL = 0x343;
constexpr uint16_t REGNO_DCSR = 0x7b0;
constexpr uint16_t REGNO_DPC = 0x7b1;
constexpr uint16_t REGNO_DSCRATCH0 = 0x7b2;

// ---- dcsr bits (docs/debug-mode.md) ----
constexpr uint32_t DCSR_EBREAKM = 1u << 15;  // 0x8000
constexpr uint32_t DCSR_EBREAKS = 1u << 13;  // 0x2000
constexpr uint32_t DCSR_EBREAKU = 1u << 12;  // 0x1000
constexpr uint32_t DCSR_STEPIE = 1u << 11;   // 0x800
constexpr uint32_t DCSR_STEP = 1u << 2;      // 0x4
constexpr uint32_t DCSR_CAUSE_SHIFT = 6;
constexpr uint32_t DCSR_CAUSE_MASK = 0x7u;
// The control bits this debugger manages; restored from the attach copy on
// detach (docs/debug-sequences.md "Detaching" step 2).
constexpr uint32_t DCSR_CONTROL_BITS =
    DCSR_STEP | DCSR_EBREAKM | DCSR_EBREAKS | DCSR_EBREAKU;

// ---- patched-instruction encodings (software breakpoints) ----
constexpr uint32_t INSN_EBREAK = 0x0010'0073u;    // ebreak
constexpr uint16_t INSN_C_EBREAK = 0x9002u;       // c.ebreak

// Access Memory aamsize values; also the stride in bytes.
enum class MemSize : uint32_t { Byte = 0, Half = 1, Word = 2 };
constexpr uint32_t mem_stride(MemSize size) {
    return 1u << static_cast<uint32_t>(size);
}

// Access Register command word:
//   cmdtype=0, aarsize=2 (32-bit), aarpostincrement=0, postexec=0,
//   transfer=1; write bit 16 set for writes. regno = GPR or CSR address.
constexpr uint32_t access_register_cmd(uint16_t regno, bool write) {
    return 0x0022'0000u | (write ? 0x1'0000u : 0u) | regno;
}

// Access Memory command word:
//   cmdtype=2, aamvirtual=0, target bits 15:14 = 0, aamsize in 22:20,
//   aampostincrement (bit 19) optional, write bit 16.
constexpr uint32_t access_memory_cmd(MemSize size, bool write, bool postinc) {
    return 0x0200'0000u | (static_cast<uint32_t>(size) << 20) |
           (postinc ? 0x8'0000u : 0u) | (write ? 0x1'0000u : 0u);
}

// ---- sequence-level exceptions ----

// abstractcs.cmderr after an abstract command. The sticky cmderr has
// already been cleared (W1C) so the object stays usable; fix the cause
// and re-issue (error-recovery layer 1, docs/debug-sequences.md).
class CmderrError : public DebugError {
public:
    explicit CmderrError(uint32_t code);
    uint32_t code;  // 1 busy, 2 not supported, 3 exception, 4 halt/resume,
                    // 5 bus, 7 other
};

// The hart did not halt / resume in time.
class HaltTimeout : public DebugError {
public:
    explicit HaltTimeout(const std::string& what) : DebugError(what) {}
};

class DebugSequences {
public:
    // What connect()/attach() learned about the target.
    struct Discovered {
        uint32_t idcode = 0;            // JTAG IDCODE
        uint32_t dtmcs = 0;             // raw dtmcs (version=1, abits, idle)
        int abits = 0;                 // dmi address field width
        int idle = 0;                   // Run-Test/Idle dwell hint
        uint32_t dmstatus = 0;          // expect version=2, authenticated=1
        uint32_t abstractcs = 0;        // progbufsize=0, datacount=2
        uint32_t hartinfo = 0;          // 0: no debug ROM shadowing
        uint32_t sbcs = 0;              // 0: no System Bus Access
        uint32_t dmcontrol_readback = 0;  // after the 0x07FFFFC1 probe
        bool single_hart = true;        // hartsel/hasel read back 0
        int xlen = 32;                  // from the aarsize=3 probe
        bool has_c_extension = false;   // misa bit 2 — needed for breakpoints
        bool burst_postinc = false;     // aampostincrement supported
        bool burst_autoexec = false;    // abstractauto supported
    };

    // Halt reason / core state (docs/debug-sequences.md "Reading the halt
    // reason and core state").
    struct HaltInfo {
        uint32_t dcsr = 0;
        uint32_t dpc = 0;
        int cause = 0;
    };

    // dcsr.cause values (dcsr.cause = (dcsr >> 6) & 7).
    enum HaltCause {
        kCauseEbreak = 1,       // ebreak executed — a software breakpoint hit
        kCauseTrigger = 2,      // trigger fired (not with this DM)
        kCauseHaltReq = 3,      // own haltreq
        kCauseStep = 4,         // single step completed
        kCauseResetHaltReq = 5  // resethaltreq out of reset (not implemented)
    };

    // One entry of the breakpoint table "A -> (original word, length)".
    struct Breakpoint {
        uint32_t original = 0;       // saved first word (or halfword)
        uint32_t original_hi = 0;    // second halfword, only if straddles_32
        bool straddles_32 = false;   // 32-bit instruction at addr % 4 == 2
        uint32_t length = 0;         // patch length in bytes: 2 or 4
    };

    explicit DebugSequences(JtagDtm& dtm) : dtm_(dtm) {}

    // ---- connect / attach / detach ----

    // "Connect and discovery": bring up transport, DM, learn what exists.
    // Leaves the DM active and the dmi instruction selected.
    Discovered connect();

    // connect() + halt + the halted-only probes (XLEN, burst extension,
    // misa.C) + save dcsr + set dcsr.ebreakm (the debuggee is assumed to
    // run in M-mode; without ebreakm a patched ebreak would trap to the
    // debuggee's own trap handler). Called with the hart running or halted.
    Discovered attach();

    // "Detaching": remove all breakpoints, restore the dcsr control bits
    // saved at attach (keeping the current prv/cause), clear haltreq,
    // resume, optionally power the DM down (dmactive=0). Expected to be
    // called while halted; skips the resume if already running.
    void detach(bool power_down_dm = false);

    bool attached() const { return attached_; }
    const Discovered& discovered() const { return info_; }

    // ---- halting / resuming ----

    // "Halting the hart": haltreq | dmactive, poll allhalted; on timeout
    // with recover_on_timeout, run the reset-and-halt recovery (the hart
    // is unavailable / wedged — error-recovery layer 4). Clears haltreq
    // afterwards: it must be 0 before abstract commands and resume.
    void halt(bool recover_on_timeout = false);

    bool is_halted();

    // Poll dmstatus until allhalted (no side effects — safe to poll).
    void wait_halted(unsigned max_polls = 65536);

    // "Resuming": resumereq | dmactive, poll allresumeack (NOT allhalted:
    // a hart with a breakpoint at the resume point halts again
    // immediately). Only call while halted.
    void resume();

    // ---- halt reason / core state ----
    HaltInfo read_halt_info();

    // ---- register access (GPRs and CSRs) ----
    uint32_t read_gpr(int n);
    void write_gpr(int n, uint32_t value);
    void dump_gprs(uint32_t out[32]);  // x0..x31, the classic "reg" command

    uint32_t read_csr(uint32_t csr_addr);
    void write_csr(uint32_t csr_addr, uint32_t value);

    // OpenOCD-style CSR probing: an unimplemented CSR answers cmderr=3
    // (exception) — report it "nonexistent" and move on.
    std::optional<uint32_t> try_read_csr(uint32_t csr_addr);

    // Raw Access Register commands for any regno (CSRs 0x000-0xfff, GPRs
    // 0x1000-0x101f, custom 0xc000-0xffff).
    uint32_t read_reg(uint16_t regno);
    void write_reg(uint16_t regno, uint32_t value);

    // ---- memory access (through the halted hart's LSU) ----

    // One load/store: address in data1, data in data0. Access goes through
    // the hart's own LSU, so it sees exactly what M-mode code would see
    // (PMP, regions, MMIO side effects).
    uint32_t read_mem(uint32_t addr, MemSize size = MemSize::Word);
    void write_mem(uint32_t addr, uint32_t value,
                   MemSize size = MemSize::Word);

    // "Sequential memory read/write (block transfers)": explicit loop —
    // every iteration re-writes the address into data1, then re-issues the
    // same command word. cmderr is polled every iteration; on a fault the
    // loop aborts and returns the number of words done (partial data
    // stays in the buffer), with the code in last_cmderr().
    size_t read_mem_block(uint32_t addr, uint32_t* buf, size_t words);
    size_t write_mem_block(uint32_t addr, const uint32_t* buf, size_t words);

    // "Auto-increment bursts", intermediate variant: aampostincrement only
    // (data1 written once, command re-issued per word). 3 ops/word.
    size_t read_mem_block_postinc(uint32_t addr, uint32_t* buf, size_t words);
    size_t write_mem_block_postinc(uint32_t addr, const uint32_t* buf,
                                    size_t words);

    // "Auto-increment bursts", full variant: aampostincrement +
    // abstractauto (any data0 access re-executes the last command) — one
    // DMI op per word. The sticky cmderr gates execution, so a mid-burst
    // fault makes the burst fizzle out; the final abstractcs poll reveals
    // it and data1 points at the faulting address.
    size_t read_mem_burst(uint32_t addr, uint32_t* buf, size_t words);
    size_t write_mem_burst(uint32_t addr, const uint32_t* buf, size_t words);

    // Probe the optional burst extension (OpenOCD-style): issue a memory
    // read with aampostincrement — cmderr=2 means not supported. Also
    // detects abstractauto by writing autoexecdata bits and reading them
    // back. Requires a halted hart and a scratch address that is safe to
    // read (the probe performs one halfword read there).
    bool probe_burst_extension(uint32_t scratch_addr);

    uint32_t last_cmderr() const { return last_cmderr_; }

    // ---- single stepping ----

    // "Single stepping": set dcsr.step on the *current* dcsr (read after
    // this halt — keeps prv/ebreak bits intact), resumereq, poll
    // allresumeack (it did resume), poll allhalted (and re-halted after
    // ONE instruction, dcsr.cause=4). Clears step again before returning so
    // a plain resume does not single-step forever. With stepie=0 (reset
    // default) interrupts are masked during the step.
    void step();

    void set_step(bool on);  // OR/clear dcsr.step in the current dcsr

    // ---- software breakpoints ----

    // "Installing (always while halted)": save the original instruction
    // word, patch in ebreak (32-bit) or c.ebreak (16-bit, when the C
    // extension is present / the instruction is compressed). Returns
    // false if a breakpoint already exists at addr. Requires the halted
    // hart (abstract memory access) and dcsr.ebreakm set (attach() does
    // that).
    bool install_breakpoint(uint32_t addr);

    // Restore the saved original word at addr (a plain memory write).
    void remove_breakpoint(uint32_t addr);
    void remove_all_breakpoints();

    const Breakpoint* find_breakpoint(uint32_t addr) const;
    size_t breakpoint_count() const { return breakpoints_.size(); }

    // "Continuing past a hit": restore the original instruction (dpc == A
    // already, so the hart re-executes it), single-step (dpc = A+len or
    // branch target), re-insert the patch, clear step, resume and run on.
    void continue_past_breakpoint(uint32_t addr);

    // ---- reset ----

    // "Reset and debug from reset": ndmreset pulse with haltreq held
    // (0x80000003 -> 0x80000001) — the hart halts as it leaves reset,
    // before executing its first instruction. Returns dpc (the reset
    // vector / first fetch address). dmactive must stay 1 throughout.
    uint32_t reset_and_halt();

    // ---- error recovery (docs/debug-sequences.md "Error recovery") ----

    // Layer 1: clear abstractcs.cmderr (write-1-to-the-bits).
    void clear_cmderr();

    // Layer 5 ("everything wedged"): dmactive=0 resets the whole DM (data
    // registers, cmderr, sticky bits) — the universal unwedge — then
    // dmactive=1 and re-verify dmstatus. The hart itself keeps its state.
    void recover_dm();

private:
    // Poll abstractcs until busy==0, then check cmderr: nonzero → clear it
    // (so further commands run) and throw CmderrError.
    uint32_t poll_abstractcs();

    // Write `command` and wait for it to finish cleanly (poll + cmderr
    // check). While abstractcs.busy is set, nothing else may be touched.
    uint32_t run_command(uint32_t cmd);

    // Restore / re-insert the patch of a known breakpoint.
    void restore_breakpoint(uint32_t addr, const Breakpoint& bp);
    void patch_breakpoint(uint32_t addr, const Breakpoint& bp);

    // Try a 64-bit Access Register: RV32 DMs reject aarsize=3 with
    // cmderr=2 (this is how OpenOCD detects XLEN).
    int probe_xlen();

    JtagDtm& dtm_;
    Discovered info_{};
    uint32_t dcsr_saved_ = 0;  // dcsr control bits saved at attach
    bool attached_ = false;
    bool has_c_ = false;
    bool burst_postinc_ = false;
    bool burst_autoexec_ = false;
    uint32_t last_cmderr_ = 0;
    std::unordered_map<uint32_t, Breakpoint> breakpoints_;
};

}  // namespace rvdbg

#endif  // RVDBG_SEQUENCES_HPP
