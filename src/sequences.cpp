// rv32-debug — DebugSequences implementation.
//
// Every method mirrors a section of docs/debug-sequences.md; see the
// header for the public documentation and the section references.

#include "sequences.hpp"

#include <algorithm>
#include <stdexcept>

namespace rvdbg {

// cmderr codes (docs/debug-sequences.md "Error recovery").
static const char* cmderr_name(uint32_t code) {
    switch (code) {
        case 1: return "busy";
        case 2: return "not supported";
        case 3: return "exception";
        case 4: return "halt/resume";
        case 5: return "bus";
        case 7: return "other";
        default: return "unknown";
    }
}

CmderrError::CmderrError(uint32_t code)
    : DebugError(std::string("abstract command failed with cmderr=") +
                 std::to_string(code) + " (" + cmderr_name(code) + ")"),
      code(code) {}

// ---- connect / discovery (docs/debug-sequences.md "Connect and discovery") ----

DebugSequences::Discovered DebugSequences::connect() {
    Discovered d{};

    // 1-3. TAP reset (IR defaults to IDCODE), scan IDCODE, read dtmcs,
    //       then IR = dmi — initialize() does all of it.
    dtm_.initialize();
    d.idcode = dtm_.idcode();
    d.dtmcs = dtm_.dtmcs_raw();
    d.abits = dtm_.abits();
    d.idle = dtm_.idle();

    // 4. dmcontrol: dmactive=1 (DM out of reset). From here on, every
    //    dmcontrol write carries dmactive=1, or the DM forgets everything.
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE);

    // 5. dmstatus: expect version=2 (0.13), authenticated=1, and
    //    allrunning or allhalted.
    d.dmstatus = dtm_.dmi_read(DMI_DMSTATUS);
    if ((d.dmstatus & DMSTATUS_VERSION) != 2) {
        throw DebugError("dmstatus.version != 2 (not a 0.13 DM)");
    }
    if ((d.dmstatus & DMSTATUS_AUTHENTICATED) == 0) {
        throw DebugError("DM not authenticated");
    }
    if ((d.dmstatus & (DMSTATUS_ALLRUNNING | DMSTATUS_ALLHALTED)) == 0) {
        throw DebugError("dmstatus reports neither allrunning nor allhalted");
    }

    // Capability probing — everything absent reads 0:
    d.abstractcs = dtm_.dmi_read(DMI_ABSTRACTCS);  // progbufsize=0, datacount=2
    d.hartinfo = dtm_.dmi_read(DMI_HARTINFO);       // 0: no debug ROM shadowing
    d.sbcs = dtm_.dmi_read(DMI_SBCS);              // 0: no System Bus Access

    // Feature probe (exactly what OpenOCD writes): hasel (bit 26) | all
    // 20 hartsel bits (25:16, 15:6) | dmactive — request bits stay 0.
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_FEATURE_PROBE);
    d.dmcontrol_readback = dtm_.dmi_read(DMI_DMCONTROL);
    // hasel and hartsel bits read 0 => no hart array, HARTSELLEN=0
    // => single hart.
    d.single_hart = (d.dmcontrol_readback & DMCONTROL_HARTSEL_MASK) == 0;

    // Back to plain dmactive for the request bits.
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE);

    info_ = d;
    return d;
}

DebugSequences::Discovered DebugSequences::attach() {
    connect();
    halt();  // also clears haltreq (required before abstract commands)

    // Probes that need a halted hart:
    info_.xlen = probe_xlen();
    probe_burst_extension(read_reg(REGNO_DPC));  // scratch = dpc (readable code)
    info_.burst_postinc = burst_postinc_;
    info_.burst_autoexec = burst_autoexec_;

    // Save dcsr at attach — its control bits are what we restore on
    // detach. Do not save dpc: it is where the program resumes.
    dcsr_saved_ = read_reg(REGNO_DCSR);

    // Route ebreak to Debug Mode for an M-mode debuggee, otherwise the
    // patched ebreak of a software breakpoint traps to the debuggee's
    // trap handler instead (docs/debug-sequences.md "Software
    // breakpoints").
    write_reg(REGNO_DCSR, dcsr_saved_ | DCSR_EBREAKM);

    // Compressed instructions exist? (misa >> 2) & 1 — needed for
    // breakpoints.
    if (auto misa = try_read_csr(REGNO_MISA)) {
        info_.has_c_extension = (*misa >> 2) & 1u;
    }
    has_c_ = info_.has_c_extension;

    attached_ = true;
    return info_;
}

void DebugSequences::detach(bool power_down_dm) {
    if (!attached_) {
        throw DebugError("detach() called without attach()");
    }

    // 1. Remove all breakpoints (restore every patched word).
    remove_all_breakpoints();

    // 2. Restore the dcsr control bits saved at attach (clears ebreakm,
    //    step, etc.); keep the current prv — re-read dcsr after every
    //    halt and modify that current value, not the attach copy.
    const uint32_t cur = read_reg(REGNO_DCSR);
    write_reg(REGNO_DCSR, (cur & ~DCSR_CONTROL_BITS) |
                              (dcsr_saved_ & DCSR_CONTROL_BITS));

    // 3. Clear haltreq, dmactive stays 1.
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE);

    // 4. Resume (skip if the hart is already running).
    if (is_halted()) {
        resume();
    }

    // 5. Optional: dmactive=0 — DM pristine, all debug state reset.
    if (power_down_dm) {
        dtm_.dmi_write(DMI_DMCONTROL, 0);
    }
    attached_ = false;
}

// ---- halting / resuming ----

void DebugSequences::halt(bool recover_on_timeout) {
    // 0x8000_0001 = haltreq (bit 31) | dmactive.
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_HALTREQ | DMCONTROL_DMACTIVE);

    // poll dmstatus until (v & 0x200) != 0 — allhalted (bit 9).
    try {
        dtm_.dmi_poll(DMI_DMSTATUS, DMSTATUS_ALLHALTED, DMSTATUS_ALLHALTED);
    } catch (PollTimeout&) {
        // The hart never halts — check why before retrying
        // (error-recovery layer 4):
        const uint32_t st = dtm_.dmi_read(DMI_DMSTATUS);
        if (st & DMSTATUS_ANYNONEXISTENT) {
            throw HaltTimeout("hart does not halt: dmstatus.anynonexistent "
                              "(bad hart index)");
        }
        if (!recover_on_timeout) {
            throw HaltTimeout("hart does not halt: dmstatus=0x" +
                             std::to_string(st) +
                             " (in reset? try halt(true) or reset_and_halt)");
        }
        // allunavail -> it is in reset: do the reset-and-debug flow.
        // Wedged hart -> same recovery. haltreq is held across the pulse.
        reset_and_halt();
        return;
    }

    // haltreq must be 0 before abstract commands and resume
    // ("Halting the hart"). Writing just dmactive clears it.
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE);
}

bool DebugSequences::is_halted() {
    return (dtm_.dmi_read(DMI_DMSTATUS) & DMSTATUS_ALLHALTED) != 0;
}

void DebugSequences::wait_halted(unsigned max_polls) {
    dtm_.dmi_poll(DMI_DMSTATUS, DMSTATUS_ALLHALTED, DMSTATUS_ALLHALTED,
                  max_polls);
}

void DebugSequences::resume() {
    // 0x4000_0001 = resumereq (bit 30, one-shot) | dmactive.
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_RESUMEREQ | DMCONTROL_DMACTIVE);

    // Poll allresumeack (0x20000), NOT allhalted: a hart with a breakpoint
    // at the resume point halts again immediately and allhalted would
    // mislead you into thinking it never resumed.
    dtm_.dmi_poll(DMI_DMSTATUS, DMSTATUS_ALLRESUMEACK, DMSTATUS_ALLRESUMEACK);
}

// ---- halt reason / core state ----

DebugSequences::HaltInfo DebugSequences::read_halt_info() {
    // Read dcsr and dpc through Access Register commands
    // (0x0022_07B0 / 0x0022_07B1).
    HaltInfo h;
    h.dcsr = read_reg(REGNO_DCSR);
    h.dpc = read_reg(REGNO_DPC);
    h.cause = static_cast<int>((h.dcsr >> DCSR_CAUSE_SHIFT) & DCSR_CAUSE_MASK);
    return h;
}

// ---- register access ----

uint32_t DebugSequences::read_reg(uint16_t regno) {
    // command = 0x0022_0000 | regno: Access Register, aarsize=32, transfer,
    // write=0. Poll abstractcs until busy==0 && cmderr==0, then data0
    // holds the value.
    dtm_.dmi_write(DMI_COMMAND, access_register_cmd(regno, false));
    poll_abstractcs();
    return dtm_.dmi_read(DMI_DATA0);
}

void DebugSequences::write_reg(uint16_t regno, uint32_t value) {
    // The argument goes to data0 BEFORE the command (the DM latches
    // data0 when the command starts): command = 0x0023_0000 | regno.
    dtm_.dmi_write(DMI_DATA0, value);
    dtm_.dmi_write(DMI_COMMAND, access_register_cmd(regno, true));
    poll_abstractcs();
}

uint32_t DebugSequences::read_gpr(int n) {
    if (n < 0 || n > 31) {
        throw std::invalid_argument("GPR index out of range");
    }
    return read_reg(static_cast<uint16_t>(REGNO_GPR_BASE | n));
}

void DebugSequences::write_gpr(int n, uint32_t value) {
    if (n < 0 || n > 31) {
        throw std::invalid_argument("GPR index out of range");
    }
    // x0 is the zero register: a write to it is silently dropped by the
    // hart, not an error.
    write_reg(static_cast<uint16_t>(REGNO_GPR_BASE | n), value);
}

void DebugSequences::dump_gprs(uint32_t out[32]) {
    // 32 identical iterations: one command + one poll + one data0 read
    // each. x0 always reads 0.
    for (int n = 0; n < 32; ++n) {
        out[n] = read_gpr(n);
    }
}

uint32_t DebugSequences::read_csr(uint32_t csr_addr) {
    // CSRs use the same command shape, just the CSR's address directly as
    // the regno — no offset is added.
    return read_reg(static_cast<uint16_t>(csr_addr));
}

void DebugSequences::write_csr(uint32_t csr_addr, uint32_t value) {
    write_reg(static_cast<uint16_t>(csr_addr), value);
}

std::optional<uint32_t> DebugSequences::try_read_csr(uint32_t csr_addr) {
    // A CSR that the hart does not implement answers cmderr=3 (exception)
    // — the debugger marks it "nonexistent" and moves on (that is exactly
    // how OpenOCD's CSR probing works).
    try {
        return read_reg(static_cast<uint16_t>(csr_addr));
    } catch (CmderrError& e) {
        if (e.code == 3) {
            return std::nullopt;
        }
        throw;
    }
}

// ---- memory access ----

uint32_t DebugSequences::read_mem(uint32_t addr, MemSize size) {
    // data1 = address, command = 0x0200_0000 | (aamsize << 20), poll,
    // data0 = value.
    dtm_.dmi_write(DMI_DATA1, addr);
    run_command(access_memory_cmd(size, false, false));
    return dtm_.dmi_read(DMI_DATA0);
}

void DebugSequences::write_mem(uint32_t addr, uint32_t value, MemSize size) {
    // data0 = value FIRST (latched at command start), then data1 =
    // address, then command with the write bit.
    dtm_.dmi_write(DMI_DATA0, value);
    dtm_.dmi_write(DMI_DATA1, addr);
    run_command(access_memory_cmd(size, true, false));
}

size_t DebugSequences::read_mem_block(uint32_t addr, uint32_t* buf,
                                       size_t words) {
    // No auto-increment and no SBA in this DM: an explicit loop. Every
    // iteration re-writes the address into data1, then re-issues the same
    // command word.
    for (size_t i = 0; i < words; ++i) {
        try {
            buf[i] = read_mem(addr + 4 * static_cast<uint32_t>(i));
        } catch (CmderrError& e) {
            // Fault handling mid-block: abort with the partial data in
            // buf; cmderr is polled every iteration.
            last_cmderr_ = e.code;
            return i;
        }
    }
    last_cmderr_ = 0;
    return words;
}

size_t DebugSequences::write_mem_block(uint32_t addr, const uint32_t* buf,
                                        size_t words) {
    for (size_t i = 0; i < words; ++i) {
        try {
            write_mem(addr + 4 * static_cast<uint32_t>(i), buf[i]);
        } catch (CmderrError& e) {
            last_cmderr_ = e.code;
            return i;
        }
    }
    last_cmderr_ = 0;
    return words;
}

size_t DebugSequences::read_mem_block_postinc(uint32_t addr, uint32_t* buf,
                                              size_t words) {
    if (words == 0) {
        return 0;
    }
    // aampostincrement alone: data1 is written once, the command word
    // (0x0228_0000) re-issued per word. Saves the per-word data1 rewrite.
    dtm_.dmi_write(DMI_DATA1, addr);
    for (size_t i = 0; i < words; ++i) {
        try {
            run_command(access_memory_cmd(MemSize::Word, false, true));
            buf[i] = dtm_.dmi_read(DMI_DATA0);
        } catch (CmderrError& e) {
            last_cmderr_ = e.code;
            return i;
        }
    }
    last_cmderr_ = 0;
    return words;
}

size_t DebugSequences::write_mem_block_postinc(uint32_t addr,
                                                const uint32_t* buf,
                                                size_t words) {
    if (words == 0) {
        return 0;
    }
    dtm_.dmi_write(DMI_DATA1, addr);
    for (size_t i = 0; i < words; ++i) {
        try {
            dtm_.dmi_write(DMI_DATA0, buf[i]);
            run_command(access_memory_cmd(MemSize::Word, true, true));
        } catch (CmderrError& e) {
            last_cmderr_ = e.code;
            return i;
        }
    }
    last_cmderr_ = 0;
    return words;
}

size_t DebugSequences::read_mem_burst(uint32_t addr, uint32_t* buf,
                                      size_t words) {
    if (words == 0) {
        return 0;
    }
    try {
        // Setup: data1 = start address, command = 0x0228_0000 (Access
        // Memory, 32-bit, read, aampostincrement=1) -> word 0 in data0.
        dtm_.dmi_write(DMI_DATA1, addr);
        run_command(access_memory_cmd(MemSize::Word, false, true));

        if (words > 1) {
            // abstractauto bit 0 (autoexecdata[0]): any data0 access
            // re-executes the last command. Each data0 read returns one
            // word AND triggers the next access.
            dtm_.dmi_write(DMI_ABSTRACTAUTO, 0x0000'0001u);
            for (size_t i = 0; i + 1 < words; ++i) {
                buf[i] = dtm_.dmi_read(DMI_DATA0);  // word i; triggers i+1
            }
            // Disable autoexec (writing abstractauto triggers nothing).
            dtm_.dmi_write(DMI_ABSTRACTAUTO, 0x0000'0000u);
        }
        // One abstractcs poll reveals a mid-burst fault: the sticky
        // cmderr gates execution, so once a word faults no further
        // auto-triggered command starts.
        poll_abstractcs();
        buf[words - 1] = dtm_.dmi_read(DMI_DATA0);
        last_cmderr_ = 0;
        return words;
    } catch (CmderrError& e) {
        // Keep abstractauto disabled no matter where the burst failed.
        dtm_.dmi_write(DMI_ABSTRACTAUTO, 0x0000'0000u);
        last_cmderr_ = e.code;
        // data1 points at the faulting address (no increment on failure).
        const uint32_t d1 = dtm_.dmi_read(DMI_DATA1);
        const size_t done = d1 > addr ? (d1 - addr) / 4 : 0;
        return std::min(done, words);
    }
}

size_t DebugSequences::write_mem_burst(uint32_t addr, const uint32_t* buf,
                                       size_t words) {
    if (words == 0) {
        return 0;
    }
    try {
        // data0 must hold buf[0] BEFORE the command write, because
        // writing command starts the first execution immediately.
        dtm_.dmi_write(DMI_DATA1, addr);
        dtm_.dmi_write(DMI_DATA0, buf[0]);
        run_command(access_memory_cmd(MemSize::Word, true, true));  // 0x0229_0000

        if (words > 1) {
            dtm_.dmi_write(DMI_ABSTRACTAUTO, 0x0000'0001u);
            for (size_t i = 1; i < words; ++i) {
                dtm_.dmi_write(DMI_DATA0, buf[i]);  // stores word i AND
                                                    // triggers the next
            }
            dtm_.dmi_write(DMI_ABSTRACTAUTO, 0x0000'0000u);
        }
        poll_abstractcs();
        last_cmderr_ = 0;
        return words;
    } catch (CmderrError& e) {
        dtm_.dmi_write(DMI_ABSTRACTAUTO, 0x0000'0000u);
        last_cmderr_ = e.code;
        const uint32_t d1 = dtm_.dmi_read(DMI_DATA1);
        const size_t done = d1 > addr ? (d1 - addr) / 4 : 0;
        return std::min(done, words);
    }
}

bool DebugSequences::probe_burst_extension(uint32_t scratch_addr) {
    // OpenOCD probes aampostincrement first: issue a command with bit 19,
    // fall back to per-word data1 writes if it answers cmderr=2 (not
    // supported).
    dtm_.dmi_write(DMI_DATA1, scratch_addr);
    try {
        run_command(access_memory_cmd(MemSize::Half, false, true));
    } catch (CmderrError& e) {
        if (e.code == 2) {
            burst_postinc_ = false;
            burst_autoexec_ = false;
            return false;
        }
        throw;  // e.g. cmderr=4: not halted — probe when halted
    }
    burst_postinc_ = true;

    // abstractauto is detectable by writing autoexecdata bits and
    // reading them back (unimplemented registers read 0, writes ignored).
    dtm_.dmi_write(DMI_ABSTRACTAUTO, 0x0000'0001u);
    burst_autoexec_ = (dtm_.dmi_read(DMI_ABSTRACTAUTO) & 0x1u) != 0;
    dtm_.dmi_write(DMI_ABSTRACTAUTO, 0x0000'0000u);
    return burst_postinc_;
}

// ---- single stepping ----

void DebugSequences::step() {
    set_step(true);
    // resumereq; poll allresumeack (it did resume)...
    resume();
    // ...and poll allhalted: re-halted after ONE instruction,
    // dcsr.cause = 4, dpc advanced.
    wait_halted();
    // Clear step before the next plain resume, or the hart will
    // single-step forever ("keeps ebreakm set" — the current dcsr value).
    set_step(false);
}

void DebugSequences::set_step(bool on) {
    // Read the CURRENT dcsr (after this halt — cause and prv change) and
    // OR/clear step into it, keeping prv/ebreak bits intact.
    const uint32_t dcsr = read_reg(REGNO_DCSR);
    write_reg(REGNO_DCSR, on ? (dcsr | DCSR_STEP) : (dcsr & ~DCSR_STEP));
}

// ---- software breakpoints ----

bool DebugSequences::install_breakpoint(uint32_t addr) {
    if (breakpoints_.count(addr) != 0) {
        return false;  // already installed (reading back would fetch the patch)
    }

    Breakpoint bp{};

    if (!has_c_) {
        // No C extension: instructions are word-aligned.
        if (addr % 4 != 0) {
            throw std::invalid_argument("breakpoint address not word-aligned");
        }
        // Read the original instruction word at A, save it.
        const uint32_t w0 = read_mem(addr, MemSize::Word);
        // 32-bit instruction (low 2 bits == 0b11): patch a full ebreak.
        bp.original = w0;
        bp.length = 4;
    } else if (addr % 4 == 0) {
        // With C, read the word at A and decode the low bits.
        const uint32_t w0 = read_mem(addr, MemSize::Word);
        bp.original = w0;
        if ((w0 & 0x3u) == 0x3u) {
            bp.length = 4;  // 32-bit instruction at A
        } else {
            // 16-bit compressed instruction: only the low halfword is the
            // instruction; patch c.ebreak with a 16-bit store (aamsize=1) —
            // a 32-bit store would clobber the following instruction.
            bp.original &= 0xFFFFu;
            bp.length = 2;
        }
    } else {  // has_c_ && addr % 4 == 2
        // Instructions are only 2-byte aligned here: read the original
        // with a 16-bit access first.
        const uint32_t h = read_mem(addr, MemSize::Half);
        if ((h & 0x3u) == 0x3u) {
            // The halfword at A starts a 32-bit instruction (allowed at
            // 2-byte alignment with C). Patching c.ebreak over its first
            // halfword still halts before it; save the second halfword at
            // A+2 so the restore (and the single step past the hit) can
            // put the whole instruction back.
            bp.original = h;
            bp.original_hi = read_mem(addr + 2, MemSize::Half);
            bp.straddles_32 = true;
            bp.length = 2;
        } else {
            bp.original = h;
            bp.length = 2;  // 16-bit compressed instruction
        }
    }

    patch_breakpoint(addr, bp);
    breakpoints_[addr] = bp;
    return true;
}

void DebugSequences::remove_breakpoint(uint32_t addr) {
    const auto it = breakpoints_.find(addr);
    if (it == breakpoints_.end()) {
        throw DebugError("no breakpoint installed at 0x" +
                         std::to_string(addr));
    }
    // Restore the saved original word at A (a plain memory write).
    restore_breakpoint(addr, it->second);
    breakpoints_.erase(it);
}

void DebugSequences::remove_all_breakpoints() {
    // Walk the whole breakpoint table and restore everything
    // ("Detaching" step 1).
    for (const auto& [addr, bp] : breakpoints_) {
        restore_breakpoint(addr, bp);
    }
    breakpoints_.clear();
}

const DebugSequences::Breakpoint* DebugSequences::find_breakpoint(
    uint32_t addr) const {
    const auto it = breakpoints_.find(addr);
    return it == breakpoints_.end() ? nullptr : &it->second;
}

void DebugSequences::continue_past_breakpoint(uint32_t addr) {
    const auto it = breakpoints_.find(addr);
    if (it == breakpoints_.end()) {
        throw DebugError("continue_past_breakpoint: no breakpoint at 0x" +
                         std::to_string(addr));
    }
    // The classic step-over-restore-reinsert loop:
    // 1. Restore the original instruction (dpc == A already, so the hart
    //    will re-execute the original).
    restore_breakpoint(addr, it->second);
    // 2. Single-step: executed the original instruction,
    //    dpc = A + len (or the branch target).
    step();
    // 3. Re-insert the patch.
    patch_breakpoint(addr, it->second);
    // 4. Clear step — already done by step().
    // 5. Resume and run on.
    resume();
}

void DebugSequences::restore_breakpoint(uint32_t addr, const Breakpoint& bp) {
    if (bp.length == 4) {
        write_mem(addr, bp.original, MemSize::Word);
    } else {
        write_mem(addr, bp.original, MemSize::Half);
        if (bp.straddles_32) {
            write_mem(addr + 2, bp.original_hi, MemSize::Half);
        }
    }
}

void DebugSequences::patch_breakpoint(uint32_t addr, const Breakpoint& bp) {
    // ebreak = SYSTEM opcode (0x73) with imm=1, rs1=0, rd=0; c.ebreak =
    // the 16-bit form. The minimal DM requires aamsize=1 for the
    // halfword store — DMs without it need the read-modify-write trick
    // instead (docs/debug-sequences.md "Installing").
    if (bp.length == 4) {
        write_mem(addr, INSN_EBREAK, MemSize::Word);
    } else {
        write_mem(addr, INSN_C_EBREAK, MemSize::Half);
    }
}

// ---- reset ----

uint32_t DebugSequences::reset_and_halt() {
    // haltreq (0x8000_0000) | ndmreset (0x2) | dmactive (0x1):
    // everything except DM/DTM in reset. dmactive must stay 1 throughout.
    dtm_.dmi_write(DMI_DMCONTROL,
                   DMCONTROL_HALTREQ | DMCONTROL_NDMRESET | DMCONTROL_DMACTIVE);
    // Hold as long as needed; a 1-then-0 pulse is sufficient.
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_HALTREQ | DMCONTROL_DMACTIVE);
    // Release ndmreset, haltreq stays. The hart may read allunavail for a
    // while — poll allhalted, not a fixed delay. It halts as it leaves
    // reset, BEFORE executing its first instruction.
    dtm_.dmi_poll(DMI_DMSTATUS, DMSTATUS_ALLHALTED, DMSTATUS_ALLHALTED);
    // Clear haltreq before abstract commands, then read dpc — the reset
    // vector / first fetch address.
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE);
    return read_reg(REGNO_DPC);
}

// ---- error recovery ----

void DebugSequences::clear_cmderr() {
    // W1C: any code clears with 0x700.
    dtm_.dmi_write(DMI_ABSTRACTCS, ABSTRACTCS_CMDERR_CLEAR);
}

void DebugSequences::recover_dm() {
    // dmactive=0 resets the whole DM (data registers, cmderr, sticky bits)
    // — the universal unwedge — then dmactive=1 and re-verify.
    dtm_.dmi_write(DMI_DMCONTROL, 0);
    dtm_.dmi_write(DMI_DMCONTROL, DMCONTROL_DMACTIVE);
    const uint32_t st = dtm_.dmi_read(DMI_DMSTATUS);
    if ((st & DMSTATUS_VERSION) != 2 ||
        (st & DMSTATUS_AUTHENTICATED) == 0) {
        throw DebugError("recover_dm: DM did not come back after dmactive "
                         "pulse (dmstatus=0x" +
                         std::to_string(st) + ")");
    }
}

// ---- private helpers ----

uint32_t DebugSequences::poll_abstractcs() {
    // Poll abstractcs until (v & 0x1000) == 0 — busy cleared — while busy
    // is set, never read/write data0/data1 and never write
    // command/abstractcs. Also check cmderr == 0.
    const uint32_t v = dtm_.dmi_poll(DMI_ABSTRACTCS, ABSTRACTCS_BUSY, 0);
    const uint32_t err = (v & ABSTRACTCS_CMDERR) >> 8;
    if (err != 0) {
        // Clear with write-1-to-the-bits (auto-clear keeps the object
        // usable), then surface the code so the caller can fix the cause
        // and re-issue.
        clear_cmderr();
        last_cmderr_ = err;
        throw CmderrError(err);
    }
    return v;
}

uint32_t DebugSequences::run_command(uint32_t cmd) {
    dtm_.dmi_write(DMI_COMMAND, cmd);
    return poll_abstractcs();
}

int DebugSequences::probe_xlen() {
    // Attempt a 64-bit Access Register: the DM must reject aarsize=3 with
    // cmderr=2 on RV32 (that is the field the debugger probes for XLEN).
    try {
        run_command(0x0032'0000u | REGNO_DCSR);
    } catch (CmderrError& e) {
        if (e.code == 2) {
            return 32;
        }
        throw;
    }
    return 64;
}

}  // namespace rvdbg
