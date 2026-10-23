// rv32-debug — self-checking demo: runs every debug sequence from
// docs/debug-sequences.md against the behavioral mock target and checks
// the results. Build with `make` and run with `make test`.

#include "dtm.hpp"
#include "mock/mock_target.hpp"
#include "sequences.hpp"

#include <cstdio>
#include <cstring>

using namespace rvdbg;

namespace {

int failures = 0;

void check(bool ok, const char* what) {
    printf("  [%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) {
        ++failures;
    }
}

const char* cause_name(int cause) {
    switch (cause) {
        case DebugSequences::kCauseEbreak: return "ebreak (breakpoint hit)";
        case DebugSequences::kCauseTrigger: return "trigger";
        case DebugSequences::kCauseHaltReq: return "halt request";
        case DebugSequences::kCauseStep: return "single step";
        case DebugSequences::kCauseResetHaltReq: return "resethaltreq";
        default: return "unknown";
    }
}

const char* cmderr_name(uint32_t code) {
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

// The mock hart's "program": eight 32-bit nops, two compressed nops, two
// more nops, then a real ebreak as the program end.
constexpr uint32_t kProgBase = 0x1000;
constexpr uint32_t INSN_NOP = 0x00000013u;   // addi x0, x0, 0
constexpr uint16_t INSN_C_NOP = 0x0001u;     // c.nop (rd=0, imm=0)
constexpr uint32_t kProgEnd = 0x102C;        // the ebreak's address

void load_program(MockTarget& mock) {
    for (int i = 0; i < 8; ++i) {
        mock.poke32(kProgBase + 4 * static_cast<uint32_t>(i), INSN_NOP);
    }
    mock.poke16(0x1020, INSN_C_NOP);
    mock.poke16(0x1022, INSN_C_NOP);
    mock.poke32(0x1024, INSN_NOP);
    mock.poke32(0x1028, INSN_NOP);
    mock.poke32(kProgEnd, INSN_EBREAK);  // program end: a real ebreak
}

}  // namespace

int main() {
    MockTarget mock;
    load_program(mock);
    mock.set_reset_vector(kProgBase);

    JtagDtm dtm(mock);
    DebugSequences seq(dtm);

    // ---- connect & attach ----
    printf("== connect and discovery / attach ==\n");
    auto info = seq.attach();
    printf("  idcode=0x%08X  dtmcs=0x%08X (abits=%d idle=%d)\n", info.idcode,
           info.dtmcs, info.abits, info.idle);
    printf("  dmstatus=0x%08X  abstractcs=0x%08X  hartinfo=0x%X  sbcs=0x%X\n",
           info.dmstatus, info.abstractcs, info.hartinfo, info.sbcs);
    printf("  dmcontrol readback=0x%08X  single_hart=%d  xlen=%d  C=%d\n",
           info.dmcontrol_readback, info.single_hart, info.xlen,
           info.has_c_extension);
    printf("  burst: aampostincrement=%d abstractauto=%d\n",
           info.burst_postinc, info.burst_autoexec);
    check(info.single_hart, "single hart (hartsel probe reads back 0)");
    check(info.xlen == 32, "XLEN probe: aarsize=3 rejected -> RV32");
    check(info.has_c_extension, "misa.C detected");
    check(info.burst_postinc && info.burst_autoexec,
          "optional burst extension present");

    auto hi = seq.read_halt_info();
    printf("  halted: dcsr=0x%08X dpc=0x%08X cause=%d (%s)\n", hi.dcsr,
           hi.dpc, hi.cause, cause_name(hi.cause));
    check(hi.cause == DebugSequences::kCauseHaltReq,
          "attach halted via haltreq (cause=3)");
    check((hi.dcsr & DCSR_EBREAKM) != 0, "dcsr.ebreakm set at attach");

    // ---- register access ----
    printf("== GPR / CSR access ==\n");
    uint32_t regs[32];
    seq.dump_gprs(regs);  // the classic debugger "reg" command
    printf("  x0=%u x1=0x%08X ... x31=0x%08X\n", regs[0], regs[1], regs[31]);
    check(regs[0] == 0, "x0 reads 0");
    seq.write_gpr(10, 0xDEADBEEFu);
    check(seq.read_gpr(10) == 0xDEADBEEFu, "GPR write/read via data0+command");
    check(mock.gpr(10) == 0xDEADBEEFu, "value landed in the hart's reg file");
    const auto misa = seq.try_read_csr(REGNO_MISA);
    check(misa.has_value() && (*misa & 0x4u), "misa reads via abstract CSR");
    check(!seq.try_read_csr(0x7C0).has_value(),
          "unimplemented CSR -> cmderr=3 -> nonexistent");
    try {
        (void)seq.read_csr(0x7C0);
        check(false, "read of unimplemented CSR throws CmderrError");
    } catch (const CmderrError& e) {
        printf("  cmderr=%u (%s) surfaced and auto-cleared\n", e.code,
               cmderr_name(e.code));
        check(e.code == 3, "read of unimplemented CSR throws CmderrError(3)");
    }

    // ---- memory access ----
    printf("== memory access ==\n");
    seq.write_mem(0x3000, 0x11223344u);
    check(seq.read_mem(0x3000) == 0x11223344u, "32-bit write/read");
    seq.write_mem(0x3004, 0xA5A5u, MemSize::Half);
    check(seq.read_mem(0x3004, MemSize::Half) == 0xA5A5u, "16-bit access");
    check(seq.read_mem(0x3004) == 0x0000A5A5u, "word read sees the halfword");
    seq.write_mem(0x3006, 0x5Au, MemSize::Byte);
    check(seq.read_mem(0x3006, MemSize::Byte) == 0x5A, "8-bit access");

    uint32_t wbuf[8], rbuf[8];
    for (uint32_t i = 0; i < 8; ++i) {
        wbuf[i] = 0x6000'0000u + i;
    }
    check(seq.write_mem_block(0x2000, wbuf, 8) == 8,
          "plain block write (8 words, 4 DMI ops each)");
    check(seq.read_mem_block(0x2000, rbuf, 8) == 8, "plain block read");
    check(std::memcmp(wbuf, rbuf, sizeof wbuf) == 0, "block data matches");

    uint32_t pbuf[16];
    const size_t partial =
        seq.read_mem_block(0xFFF0u, pbuf, 16);  // runs off mock memory
    printf("  block read off the end: %zu words, cmderr=%u (%s)\n", partial,
           seq.last_cmderr(), cmderr_name(seq.last_cmderr()));
    check(partial == 4, "fault mid-block aborts with partial data");
    check(seq.last_cmderr() == 5, "load fault surfaces as cmderr=5 (bus)");

    // ---- block transfer variants ----
    printf("== auto-increment block variants ==\n");
    check(seq.write_mem_block_postinc(0x2100, wbuf, 8) == 8,
          "aampostincrement-only block write (3 ops/word)");
    check(seq.read_mem_block_postinc(0x2100, rbuf, 8) == 8,
          "aampostincrement-only block read");
    check(std::memcmp(wbuf, rbuf, sizeof wbuf) == 0, "postinc data matches");
    check(seq.write_mem_burst(0x2200, wbuf, 8) == 8,
          "full burst write (aampostincrement + abstractauto, 1 op/word)");
    check(seq.read_mem_burst(0x2200, rbuf, 8) == 8, "full burst read");
    check(std::memcmp(wbuf, rbuf, sizeof wbuf) == 0, "burst data matches");

    // ---- single stepping ----
    printf("== single stepping ==\n");
    seq.write_csr(REGNO_DPC, kProgBase);  // where the hart resumes
    for (int i = 1; i <= 3; ++i) {
        seq.step();
        hi = seq.read_halt_info();
        printf("  step %d: dpc=0x%08X cause=%d\n", i, hi.dpc, hi.cause);
        check(hi.dpc == kProgBase + 4 * static_cast<uint32_t>(i) &&
                  hi.cause == DebugSequences::kCauseStep,
              "stepped exactly one instruction (cause=4, dpc advanced)");
    }

    // ---- software breakpoints ----
    printf("== software breakpoints ==\n");
    seq.write_csr(REGNO_DPC, kProgBase);
    check(seq.install_breakpoint(0x1010),
          "install bp on 32-bit instr (patches ebreak)");
    check(seq.install_breakpoint(0x1020),
          "install bp on compressed instr (patches c.ebreak)");
    check(!seq.install_breakpoint(0x1010), "double install rejected");
    check(mock.peek32(0x1010) == INSN_EBREAK, "memory holds ebreak patch");
    check(mock.peek16(0x1020) == INSN_C_EBREAK, "memory holds c.ebreak patch");

    seq.resume();
    seq.wait_halted();
    hi = seq.read_halt_info();
    printf("  halted: dpc=0x%08X cause=%d (%s)\n", hi.dpc, hi.cause,
           cause_name(hi.cause));
    check(hi.dpc == 0x1010 && hi.cause == DebugSequences::kCauseEbreak,
          "hit breakpoint #1 (cause=1, dpc==A)");
    check(seq.find_breakpoint(0x1010) != nullptr, "dpc is in the bp table");

    seq.continue_past_breakpoint(0x1010);  // restore, step, re-patch, resume
    seq.wait_halted();
    hi = seq.read_halt_info();
    check(hi.dpc == 0x1020 && hi.cause == DebugSequences::kCauseEbreak,
          "hit breakpoint #2 after continue-past (step-over-restore-reinsert)");

    seq.continue_past_breakpoint(0x1020);
    seq.wait_halted();
    hi = seq.read_halt_info();
    printf("  halted: dpc=0x%08X cause=%d\n", hi.dpc, hi.cause);
    check(hi.dpc == kProgEnd && seq.find_breakpoint(kProgEnd) == nullptr,
          "halted on the program's own ebreak, not one of ours");

    seq.remove_all_breakpoints();
    check(mock.peek32(0x1010) == INSN_NOP && mock.peek16(0x1020) == INSN_C_NOP,
          "all originals restored");

    // ---- reset and debug from reset ----
    printf("== reset and debug from reset ==\n");
    const uint32_t entry = seq.reset_and_halt();
    hi = seq.read_halt_info();
    printf("  dpc=0x%08X cause=%d (%s)\n", hi.dpc, hi.cause,
           cause_name(hi.cause));
    check(entry == kProgBase, "halted at the reset vector before the first "
                              "instruction");
    check(hi.cause == DebugSequences::kCauseHaltReq,
          "haltreq held across the ndmreset pulse (cause=3)");
    check(mock.gpr(10) == 0, "ndmreset reset the hart (GPRs cleared)");

    // ---- error recovery ----
    printf("== error recovery ==\n");
    printf("  [layer 2] DMI busy: mock latency 5 TCKs, idle hint 1\n");
    mock.set_dmi_latency(5);
    (void)seq.read_csr(REGNO_DPC);  // ratchets the dwell until it fits
    printf("  dwell ratcheted 1 -> %d after %d busy events (dmireset + retry)\n",
           dtm.idle(), dtm.busy_events());
    check(dtm.idle() == 3 && dtm.busy_events() == 2,
          "DTM recovered from busy per spec B.1");
    mock.set_dmi_latency(0);

    printf("  [layer 3] DMI failed (op=2) -> sticky cleared, thrown\n");
    mock.fail_next_dmi_op();
    try {
        (void)seq.read_csr(REGNO_DPC);
        check(false, "failed DMI op throws DmiFailedError");
    } catch (const DmiFailedError&) {
        check(true, "failed DMI op throws DmiFailedError");
    }
    check(seq.read_csr(REGNO_DPC) == kProgBase,
          "after dtmcs.dmireset the op replays cleanly");

    printf("  [layer 5] dmactive 0 -> 1 unwedge\n");
    seq.recover_dm();
    check(seq.is_halted(), "DM reset; the hart keeps its halted state");

    // ---- detach ----
    printf("== detach ==\n");
    // Step past the program-end ebreak so the resume has somewhere to go.
    seq.write_csr(REGNO_DPC, kProgEnd + 4);
    seq.detach(true);  // ... and power the DM down (dmactive=0)
    check(!mock.halted(), "hart running again, as if no debugger was there");
    printf("  dmstatus now reads 0x%08X (dmactive=0, DM pristine)\n",
           dtm.dmi_read(DMI_DMSTATUS));

    printf("\n%s (%d failed checks)\n",
           failures == 0 ? "ALL CHECKS PASSED" : "SOME CHECKS FAILED", failures);
    return failures == 0 ? 0 : 1;
}
