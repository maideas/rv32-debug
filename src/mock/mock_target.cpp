// rv32-debug — MockTarget implementation. See src/mock/mock_target.hpp.

#include "mock/mock_target.hpp"

#include "sequences.hpp"  // DMI register addresses, ebreak encodings

#include <stdexcept>

namespace rvdbg {

MockTarget::MockTarget() : mem_(kMemSize, 0) {
    pc_ = reset_vector_;
}

void MockTarget::set_reset_vector(uint32_t v) {
    reset_vector_ = v;
    pc_ = v;
}

// ---- direct memory preload/inspection ----

void MockTarget::poke32(uint32_t addr, uint32_t v) {
    if (addr + 4 > mem_.size()) {
        throw std::out_of_range("poke32 out of mock memory");
    }
    for (int i = 0; i < 4; ++i) {
        mem_[addr + static_cast<size_t>(i)] =
            static_cast<uint8_t>(v >> (8 * i));
    }
}

void MockTarget::poke16(uint32_t addr, uint16_t v) {
    if (addr + 2 > mem_.size()) {
        throw std::out_of_range("poke16 out of mock memory");
    }
    mem_[addr] = static_cast<uint8_t>(v);
    mem_[addr + 1] = static_cast<uint8_t>(v >> 8);
}

uint32_t MockTarget::peek32(uint32_t addr) const {
    if (addr + 4 > mem_.size()) {
        throw std::out_of_range("peek32 out of mock memory");
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(mem_[addr + static_cast<size_t>(i)])
             << (8 * i);
    }
    return v;
}

uint16_t MockTarget::peek16(uint32_t addr) const {
    if (addr + 2 > mem_.size()) {
        throw std::out_of_range("peek16 out of mock memory");
    }
    return static_cast<uint16_t>(mem_[addr]) |
           static_cast<uint16_t>(mem_[addr + 1] << 8);
}

// ---- JtagTransport ----

void MockTarget::jtag_reset() {
    // TAP reset: IR defaults to IDCODE. The reference DTM (PULP/OpenTitan)
    // treats it like dmihardreset, so a debugger that resets the TAP
    // finds a pristine DTM (docs/jtag-dtm.md "Reset and recovery").
    ir_ = IR_IDCODE;
    sticky_ = 0;
    inflight_ = false;
    fail_next_ = false;
}

void MockTarget::jtag_ir(uint32_t ir, int irlen) {
    ir_ = ir & ((irlen >= 32) ? 0xFFFFFFFFu : ((1u << irlen) - 1u));
}

void MockTarget::jtag_dr(const uint8_t* tdi, uint8_t* tdo, int nbits,
                         int idle) {
    uint8_t in[kMaxScanBytes] = {};

    // Capture-DR: load the selected register's parallel value.
    uint8_t cap[kMaxScanBytes] = {};
    switch (effective_ir()) {
        case IR_IDCODE:
            put_bits(cap, 0, idcode_, 32);
            break;
        case IR_DTMCS:
            put_bits(cap, 0, dtmcs_value(), 32);
            break;
        case IR_DMI:
            dmi_capture();  // fills cap_op_/cap_data_/cap_addr_
            put_bits(cap, 0, cap_op_, 2);
            put_bits(cap, 2, cap_data_, 32);
            put_bits(cap, 34, cap_addr_, abits_);
            break;
        default:  // BYPASS (1 bit, captures 0)
            break;
    }

    // Shift-DR: LSB-first, TDO from the capture value, TDI into `in`.
    for (int i = 0; i < nbits; ++i) {
        const bool out_bit = (cap[i / 8] >> (i % 8)) & 1u;
        if (tdo != nullptr) {
            if (out_bit) {
                tdo[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
            }
        }
        if (tdi != nullptr && ((tdi[i / 8] >> (i % 8)) & 1u)) {
            in[i / 8] |= static_cast<uint8_t>(1u << (i % 8));
        }
    }

    // Update-DR: apply what was shifted in, per selected register.
    switch (effective_ir()) {
        case IR_DTMCS:
            dtmcs_update(get_bits(in, 0, 32));
            break;
        case IR_DMI:
            dmi_update(in);
            break;
        default:  // IDCODE / BYPASS: Update-DR does nothing
            break;
    }

    // Run-Test/Idle dwell — the next scan's Capture-DR sees the result of
    // the op started by this scan only if it completes within
    // 2 + idle TCKs (docs/jtag-dtm.md "The response window").
    prev_idle_ = idle;
}

uint32_t MockTarget::effective_ir() const {
    switch (ir_) {
        case IR_IDCODE:
        case IR_DTMCS:
        case IR_DMI:
            return ir_;
        default:  // every unimplemented encoding selects BYPASS
            return IR_BYPASS;
    }
}

// ---- DTM ----

uint32_t MockTarget::dtmcs_value() const {
    // version=1, abits, idle hint, dmistat alias of the sticky status.
    const uint32_t dmistat = sticky_ == 2 ? 2u : (sticky_ == 3 ? 3u : 0u);
    return 1u | (static_cast<uint32_t>(abits_) << 4) |
           (static_cast<uint32_t>(idle_hint_) << 12) | (dmistat << 10);
}

void MockTarget::dtmcs_update(uint32_t v) {
    if (v & DTMCS_DMIHARDRESET) {
        // Full reset of the DTM: forget outstanding DMI ops, reset all
        // DTM state.
        sticky_ = 0;
        inflight_ = false;
        fail_next_ = false;
        return;
    }
    if (v & DTMCS_DMIRESET) {
        // Clear the sticky error state only; does not abort in-flight ops.
        sticky_ = 0;
    }
}

void MockTarget::dmi_capture() {
    // Load the result of the previous operation into {address, data, op}.
    if (sticky_ != 0) {
        // While the sticky status is set, Capture-DR keeps loading it and
        // Update-DR starts nothing — until dmireset.
        cap_op_ = static_cast<uint32_t>(sticky_);
        cap_data_ = 0;
        cap_addr_ = 0;
        return;
    }
    if (inflight_) {
        const bool done = latency_ <= 2 + prev_idle_;
        inflight_ = false;  // delivered, or consumed-and-dropped on busy
        if (done) {
            if (fail_next_) {
                fail_next_ = false;
                sticky_ = 2;  // failed — sticky
                cap_op_ = 2;
                cap_data_ = 0;
                cap_addr_ = 0;
                return;
            }
            cap_op_ = 0;  // success
            cap_data_ = pending_result_;
            cap_addr_ = pending_addr_;
            return;
        }
        sticky_ = 3;  // busy — sticky
        cap_op_ = 3;
        cap_data_ = 0;
        cap_addr_ = 0;
        return;
    }
    cap_op_ = 0;  // nothing pending: previous op was a nop/write already
    cap_data_ = 0;
    cap_addr_ = 0;
}

void MockTarget::dmi_update(const uint8_t* in) {
    // Update-DR starts the operation given by the shifted-in op — unless
    // the status is sticky.
    const uint32_t op = get_bits(in, 0, 2);
    const uint32_t data = get_bits(in, 2, 32);
    const uint32_t addr = get_bits(in, 34, abits_);
    if (sticky_ != 0) {
        return;  // requests ignored while sticky
    }
    if (op == 0) {
        return;  // nop starts nothing (and never triggers busy by itself)
    }
    if (op > 2) {
        return;  // op=3 is reserved as a request
    }
    // Simplification: evaluate the DM access now (Update-DR); only its
    // *delivery* is delayed by the latency model above.
    if (op == 1) {
        pending_result_ = dm_read(addr);
    } else {
        dm_write(addr, data);
        pending_result_ = 0;
    }
    inflight_ = true;
    pending_addr_ = addr;
}

// ---- DM registers ----

uint32_t MockTarget::dm_read(uint32_t addr) {
    if (!dmactive_) {
        return 0;  // all DM state takes reset values while dmactive=0
    }
    switch (addr) {
        case DMI_DATA0: {
            const uint32_t v = data0_;
            maybe_autoexec(0);  // any data0 read re-executes the command
            return v;
        }
        case DMI_DATA1: {
            const uint32_t v = data1_;
            maybe_autoexec(1);
            return v;
        }
        case DMI_DMCONTROL:
            return (haltreq_ ? DMCONTROL_HALTREQ : 0u) |
                   (ndmreset_ ? DMCONTROL_NDMRESET : 0u) |
                   (dmactive_ ? DMCONTROL_DMACTIVE : 0u);
        case DMI_DMSTATUS:
            return dmstatus_value();
        case DMI_ABSTRACTCS:
            // busy=0 (synchronous execution), cmderr, datacount=2.
            return 2u | (static_cast<uint32_t>(cmderr_) << 8);
        case DMI_COMMAND:
            return 0;  // write-only
        case DMI_ABSTRACTAUTO:
            return abstractauto_;  // autoexecdata bits 1:0
        default:
            return 0;  // unimplemented: read 0 (hartinfo, sbcs, ...)
    }
}

void MockTarget::dm_write(uint32_t addr, uint32_t value) {
    if (!dmactive_ && addr != DMI_DMCONTROL) {
        return;  // while dmactive=0, only dmactive may be written
    }
    switch (addr) {
        case DMI_DATA0:
            data0_ = value;
            maybe_autoexec(0);
            break;
        case DMI_DATA1:
            data1_ = value;
            maybe_autoexec(1);
            break;
        case DMI_DMCONTROL:
            dmcontrol_write(value);
            break;
        case DMI_ABSTRACTCS:
            cmderr_ &= ~static_cast<int>((value >> 8) & 7);  // W1C
            break;
        case DMI_COMMAND:
            command_ = value;  // remember for abstractauto
            execute_command(value);
            break;
        case DMI_ABSTRACTAUTO:
            abstractauto_ = value & 3u;  // only data0/data1 exist
            break;
        default:
            break;  // unimplemented: writes ignored
    }
}

void MockTarget::dmcontrol_write(uint32_t value) {
    const bool new_dmactive = (value & DMCONTROL_DMACTIVE) != 0;

    if (!dmactive_) {
        // DM held in reset: only the dmactive bit may be written.
        dmactive_ = new_dmactive;  // all DM state is already at reset values
        return;
    }
    if (!new_dmactive) {
        // dmactive=0 resets the whole DM: data registers, cmderr, sticky
        // bits, request bits — the debugger's universal unwedge.
        dmactive_ = false;
        data0_ = 0;
        data1_ = 0;
        command_ = 0;
        abstractauto_ = 0;
        cmderr_ = 0;
        haltreq_ = false;
        ndmreset_ = false;
        return;
    }

    haltreq_ = (value & DMCONTROL_HALTREQ) != 0;

    if (value & DMCONTROL_RESUMEREQ) {
        resumeack_ = false;  // cleared by the next resumereq
        hart_resume();
    }

    const bool new_ndmreset = (value & DMCONTROL_NDMRESET) != 0;
    if (new_ndmreset && !ndmreset_) {
        // Rising edge: everything except DM/DTM into reset.
        ndmreset_ = true;
        hart_reset();
    } else if (!new_ndmreset && ndmreset_) {
        // Falling edge: the hart leaves reset. With haltreq held it halts
        // BEFORE executing its first instruction (dcsr.cause=3).
        ndmreset_ = false;
        if (haltreq_ && !halted_) {
            hart_halt(3);
        }
    } else if (!ndmreset_ && haltreq_ && !halted_) {
        // Plain haltreq while running.
        hart_halt(3);
    }
    // ackhavereset, hartreset, hasel, hartsel, set/clrresethaltreq:
    // not implemented — reads 0, writes ignored.
}

uint32_t MockTarget::dmstatus_value() const {
    // version=2 (0.13), authenticated=1, single hart drives all/any pairs
    // from the same signals.
    uint32_t v = 0x2u | DMSTATUS_AUTHENTICATED;
    if (ndmreset_) {
        v |= DMSTATUS_ALLUNAVAIL;  // (both all/any bits — single hart)
        v |= DMSTATUS_ALLUNAVAIL >> 1;
    } else if (halted_) {
        v |= DMSTATUS_ALLHALTED;  // + anyhalted
        v |= DMSTATUS_ALLHALTED >> 1;
    } else {
        v |= DMSTATUS_ALLRUNNING;  // + anyrunning
        v |= DMSTATUS_ALLRUNNING >> 1;
    }
    if (resumeack_) {
        v |= DMSTATUS_ALLRESUMEACK;  // + anyresumeack
        v |= DMSTATUS_ALLRESUMEACK >> 1;
    }
    return v;
}

void MockTarget::maybe_autoexec(int data_index) {
    // When autoexecdata bit k is set, any read or write of dataN
    // re-executes the last command (cmderr gating applies inside).
    if (dmactive_ && (abstractauto_ & (1u << data_index))) {
        execute_command(command_);
    }
}

// ---- abstract commands ----

void MockTarget::execute_command(uint32_t cmd) {
    if (!dmactive_) {
        return;
    }
    if (cmderr_ != 0) {
        return;  // while cmderr != 0, writes to command are ignored
    }
    const uint32_t cmdtype = cmd >> 24;
    if (cmdtype == 0) {
        access_register(cmd);
    } else if (cmdtype == 2) {
        access_memory(cmd);
    } else {
        cmderr_ = 2;  // not supported
    }
}

void MockTarget::access_register(uint32_t cmd) {
    // Minimal DM: aarsize=2 only, aarpostincrement=0, postexec=0.
    const uint32_t aarsize = (cmd >> 20) & 7;
    if (aarsize != 2 || (cmd & ((1u << 19) | (1u << 18))) != 0) {
        cmderr_ = 2;  // not supported (aarsize 3/4 is the XLEN probe)
        return;
    }
    const bool transfer = (cmd & (1u << 17)) != 0;
    const bool write = (cmd & (1u << 16)) != 0;
    const uint16_t regno = static_cast<uint16_t>(cmd & 0xFFFF);
    if (!transfer) {
        return;  // no-op
    }
    if (!halted_) {
        cmderr_ = 4;  // halt/resume: abstract access requires a halted hart
        return;
    }
    if (regno >= REGNO_GPR_BASE && regno <= REGNO_GPR_BASE + 31) {
        const int n = regno - REGNO_GPR_BASE;
        if (write) {
            if (n != 0) {
                gprs_[n] = data0_;  // x0 writes are silently dropped
            }
        } else {
            data0_ = gprs_[n];  // x0 always reads 0
        }
    } else if (regno <= 0x0FFF) {
        if (write) {
            csr_write(regno, data0_);
        } else {
            csr_read(regno);
        }
    } else {
        cmderr_ = 3;  // exception: register doesn't exist (no FPRs here)
    }
}

void MockTarget::access_memory(uint32_t cmd) {
    // Minimal DM: aamvirtual=0, aamsize 0/1/2, aampostincrement optional,
    // target bits 15:14 and 13:0 must be 0.
    if ((cmd & (1u << 23)) != 0 || (cmd & 0xC000u) != 0 ||
        (cmd & 0x3FFFu) != 0) {
        cmderr_ = 2;
        return;
    }
    const uint32_t aamsize = (cmd >> 20) & 7;
    if (aamsize > 2) {
        cmderr_ = 2;  // 64/128-bit rejected on RV32
        return;
    }
    const bool postinc = (cmd & (1u << 19)) != 0;
    const bool write = (cmd & (1u << 16)) != 0;
    if (!halted_) {
        cmderr_ = 4;  // Access Memory requires a halted hart
        return;
    }

    const uint32_t addr = data1_;
    const uint32_t nb = 1u << aamsize;
    if (addr >= mem_.size() || nb > mem_.size() - addr) {
        cmderr_ = 5;  // bus error (out of the mock's memory)
        return;
    }
    if (write) {
        for (uint32_t i = 0; i < nb; ++i) {
            mem_[addr + i] = static_cast<uint8_t>(data0_ >> (8 * i));
        }
    } else {
        uint32_t v = 0;  // loads are zero-extended to data0
        for (uint32_t i = 0; i < nb; ++i) {
            v |= static_cast<uint32_t>(mem_[addr + i]) << (8 * i);
        }
        data0_ = v;
    }
    if (postinc) {
        // After each successful access the DM increments data1 by the
        // access size. No increment on failure.
        data1_ = addr + nb;
    }
}

// ---- CSRs of the behavioral hart ----

void MockTarget::csr_read(uint16_t regno) {
    switch (regno) {
        case REGNO_MSTATUS: data0_ = mstatus_; return;
        case REGNO_MISA: data0_ = kMisa; return;
        case REGNO_MEPC: data0_ = mepc_; return;
        case REGNO_MCAUSE: data0_ = mcause_; return;
        case REGNO_MTVAL: data0_ = mtval_; return;
        case REGNO_DCSR: data0_ = dcsr_value(); return;
        case REGNO_DPC: data0_ = pc_; return;  // dpc == the hart's pc
        case REGNO_DSCRATCH0: data0_ = dscratch0_; return;
        default: cmderr_ = 3; return;  // exception: CSR doesn't exist
    }
}

void MockTarget::csr_write(uint16_t regno, uint32_t value) {
    switch (regno) {
        case REGNO_MSTATUS: mstatus_ = value; return;
        case REGNO_MISA: return;  // read-only in the mock
        case REGNO_MEPC: mepc_ = value; return;
        case REGNO_MCAUSE: mcause_ = value; return;
        case REGNO_MTVAL: mtval_ = value; return;
        case REGNO_DCSR: {
            // Writable: step, stepie, ebreakm/s/u. Read-only: xdebugver,
            // cause, prv.
            constexpr uint32_t kWritable =
                DCSR_STEP | DCSR_STEPIE | DCSR_EBREAKM | DCSR_EBREAKS |
                DCSR_EBREAKU;
            dcsr_ = (dcsr_ & ~kWritable) | (value & kWritable);
            return;
        }
        case REGNO_DPC: pc_ = value; return;  // dpc is where the hart resumes
        case REGNO_DSCRATCH0: dscratch0_ = value; return;
        default: cmderr_ = 3; return;
    }
}

uint32_t MockTarget::dcsr_value() const {
    // xdebugver[31:28]=4, cause[8:6] from the last halt, prv[1:0]=M (3).
    return (dcsr_ & ~(DCSR_CAUSE_MASK << DCSR_CAUSE_SHIFT)) |
           (static_cast<uint32_t>(cause_) << DCSR_CAUSE_SHIFT) | 3u;
}

// ---- behavioral hart ----

void MockTarget::hart_reset() {
    pc_ = reset_vector_;
    halted_ = false;
    cause_ = 0;
    resumeack_ = false;
    dcsr_ = 0x4000'0000u;  // xdebugver=4 only
    for (uint32_t& g : gprs_) {
        g = 0;
    }
    mstatus_ = mepc_ = mcause_ = mtval_ = dscratch0_ = 0;
}

void MockTarget::hart_resume() {
    // resumereq is ignored while haltreq is 1 (spec rule), and while the
    // hart is unavailable (ndmreset) or already running.
    if (haltreq_ || ndmreset_ || !halted_) {
        return;
    }
    halted_ = false;
    resumeack_ = true;  // set by the hart as it leaves halted

    if (dcsr_ & DCSR_STEP) {
        // With step=1 the hart immediately re-enters halted after ONE
        // instruction; cause=4, dpc advanced. (step stays set until the
        // debugger clears it.)
        step_one();
    } else {
        run_until_ebreak();
    }
}

void MockTarget::step_one() {
    const uint16_t h = mem_half(pc_);
    if (h == INSN_C_EBREAK) {
        hart_halt(1);  // cause=1: ebreak executed (patched breakpoint)
        return;        // dpc stays at the c.ebreak
    }
    if ((h & 0x3u) == 0x3u) {
        const uint32_t w = mem_word(pc_);
        if (w == INSN_EBREAK) {
            hart_halt(1);
            return;  // dpc holds the address OF the ebreak
        }
        pc_ += 4;
    } else {
        pc_ += 2;
    }
    hart_halt(4);  // cause=4: single step completed
}

void MockTarget::run_until_ebreak() {
    // Simplified "execution": scan forward from dpc for an ebreak /
    // c.ebreak; halt there (cause=1). If none is found within the window,
    // the hart stays running. (No branches, no side effects.)
    uint32_t p = pc_;
    for (size_t i = 0; i < kRunScanWords && p + 4 <= mem_.size(); ++i) {
        const uint16_t h = mem_half(p);
        if (h == INSN_C_EBREAK) {
            pc_ = p;
            hart_halt(1);
            return;
        }
        if ((h & 0x3u) == 0x3u) {
            if (mem_word(p) == INSN_EBREAK) {
                pc_ = p;
                hart_halt(1);
                return;
            }
            p += 4;
        } else {
            p += 2;
        }
    }
    // No ebreak found: keep running (dmstatus.allrunning).
}

uint16_t MockTarget::mem_half(uint32_t addr) const {
    if (addr + 2 > mem_.size()) {
        return 0;
    }
    return static_cast<uint16_t>(mem_[addr]) |
           static_cast<uint16_t>(mem_[addr + 1] << 8);
}

uint32_t MockTarget::mem_word(uint32_t addr) const {
    if (addr + 4 > mem_.size()) {
        return 0;
    }
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) {
        v |= static_cast<uint32_t>(mem_[addr + static_cast<size_t>(i)])
             << (8 * i);
    }
    return v;
}

}  // namespace rvdbg
