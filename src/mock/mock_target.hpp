// rv32-debug — a behavioral mock target for exercising the debug
// sequences without hardware.
//
// Implements JtagTransport and, behind it, the pieces described in the
// docs:
//   - the DTM of docs/jtag-dtm.md: IDCODE / dtmcs / dmi registers, the
//     op request/response protocol with sticky busy/failed status, the
//     busy window rule "result must arrive within 2 + dtmcs.idle TCKs",
//     dmireset/dmihardreset, TAP reset == dmihardreset (PULP behavior)
//   - the minimal DM of docs/minimal-dm.md: data0/data1, dmcontrol,
//     dmstatus, abstractcs (W1C cmderr), command (Access Register +
//     Access Memory incl. aamsize 0/1/2, aampostincrement, cmderr 2/3/4/5
//     rejection rules), abstractauto; everything else reads 0, writes
//     ignored; dmactive=0 resets all DM state
//   - a very small behavioral hart: GPR file, dcsr/dpc/misa/mstatus/
//     mepc/mcause/mtval CSRs, halt/resume with resumeack, single step,
//     ndmreset to a reset vector, and a trivial "execute" model: ebreak /
//     c.ebreak halt, everything else just advances dpc by 2 or 4.
//
// Simplifications (documented, and irrelevant for the sequences):
//   - abstractcs.busy is never observable: commands complete synchronously.
//   - DMI reads evaluate the DM at Update-DR instead of Capture-DR (no
//     asynchronous hart activity exists in the mock, so both are equal).
//   - A resumed (running) hart executes instantly up to the next ebreak
//     / c.ebreak within a scan window; if none is found it stays running.
//   - misa is hardwired to RV32IMC (0x40000104): MXL=32-bit, I + C.
#ifndef RVDBG_MOCK_TARGET_HPP
#define RVDBG_MOCK_TARGET_HPP

#include "jtag_transport.hpp"

#include <cstdint>
#include <vector>

namespace rvdbg {

class MockTarget : public JtagTransport {
public:
    static constexpr size_t kMemSize = 0x10000;  // 64 KiB flat, from 0
    // version=1, part=0, manuf=0, LSB=1 (IEEE requires).
    static constexpr uint32_t kDefaultIdcode = 0x1000'0001u;

    MockTarget();

    // ---- configuration (before first use) ----
    void set_idcode(uint32_t v) { idcode_ = v; }
    void set_abits(int abits) { abits_ = abits; }
    void set_idle_hint(int idle) { idle_hint_ = idle; }
    // DMI response latency in TCK cycles; the busy window is
    // 2 + idle, so latency > 2 + idle makes the DTM see busy.
    void set_dmi_latency(int tck_cycles) { latency_ = tck_cycles; }
    void set_reset_vector(uint32_t v);
    // Make the next completed DMI op report op=2 (failed).
    void fail_next_dmi_op() { fail_next_ = true; }

    // ---- direct memory preload/inspection (bypasses the hart) ----
    void poke32(uint32_t addr, uint32_t v);
    void poke16(uint32_t addr, uint16_t v);
    uint32_t peek32(uint32_t addr) const;
    uint16_t peek16(uint32_t addr) const;
    size_t mem_size() const { return mem_.size(); }

    // ---- hart observation ----
    bool halted() const { return halted_; }
    uint32_t pc() const { return pc_; }
    uint32_t gpr(int n) const { return gprs_[n & 31]; }
    uint32_t dcsr() const { return dcsr_value(); }

    // ---- JtagTransport ----
    void jtag_reset() override;
    void jtag_ir(uint32_t ir, int irlen = 5) override;
    void jtag_dr(const uint8_t* tdi, uint8_t* tdo, int nbits,
                 int idle = 1) override;

private:
    static constexpr int kMaxScanBytes = 16;
    static constexpr size_t kRunScanWords = 4096;  // resume scan window

    // --- TAP/DTM ---
    uint32_t effective_ir() const;
    uint32_t dtmcs_value() const;        // capture value of the dtmcs scan
    void dtmcs_update(uint32_t v);        // update value of the dtmcs scan
    void dmi_capture();                  // Capture-DR: prepare the dmi result
    void dmi_update(const uint8_t* in);  // Update-DR: start the new op
    uint32_t idcode_ = kDefaultIdcode;
    uint32_t ir_ = IR_IDCODE;
    int abits_ = 7;
    int idle_hint_ = 1;
    int latency_ = 0;      // DMI response time in TCK cycles
    bool fail_next_ = false;
    int sticky_ = 0;       // dmi op status: 0 ok, 2 failed, 3 busy (sticky)
    bool inflight_ = false;
    uint32_t pending_addr_ = 0;
    uint32_t pending_result_ = 0;
    int prev_idle_ = 1;    // RTI dwell of the previous scan (busy window)
    uint32_t cap_op_ = 0, cap_data_ = 0, cap_addr_ = 0;  // capture register

    // --- DM registers (docs/minimal-dm.md) ---
    uint32_t dm_read(uint32_t addr);
    void dm_write(uint32_t addr, uint32_t value);
    void dmcontrol_write(uint32_t value);
    uint32_t dmstatus_value() const;
    void execute_command(uint32_t cmd);
    void access_register(uint32_t cmd);
    void access_memory(uint32_t cmd);
    void csr_read(uint16_t regno);
    void csr_write(uint16_t regno, uint32_t value);
    void maybe_autoexec(int data_index);
    bool dmactive_ = false;
    bool haltreq_ = false;
    bool ndmreset_ = false;
    bool resumeack_ = false;
    uint32_t data0_ = 0, data1_ = 0;
    uint32_t command_ = 0;       // last command (for abstractauto)
    uint32_t abstractauto_ = 0;  // autoexecdata bits 1:0
    int cmderr_ = 0;

    // --- behavioral hart ---
    uint32_t dcsr_value() const;
    void hart_reset();
    void hart_halt(int cause) { halted_ = true; cause_ = cause; }
    void hart_resume();
    void step_one();
    void run_until_ebreak();
    uint16_t mem_half(uint32_t addr) const;
    uint32_t mem_word(uint32_t addr) const;
    bool halted_ = false;
    int cause_ = 0;
    uint32_t pc_ = 0;
    uint32_t reset_vector_ = 0;
    uint32_t dcsr_ = 0x4000'0000u;  // xdebugver[31:28] = 4
    uint32_t gprs_[32] = {};
    uint32_t mstatus_ = 0, mepc_ = 0, mcause_ = 0, mtval_ = 0, dscratch0_ = 0;
    static constexpr uint32_t kMisa = 0x4000'0104u;  // RV32IMC
    std::vector<uint8_t> mem_;
};

}  // namespace rvdbg

#endif  // RVDBG_MOCK_TARGET_HPP
