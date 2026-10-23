// rv32-debug — JtagDtm implementation. See docs/jtag-dtm.md for the
// protocol details (dmi register layout, scan order, sticky op status,
// busy recovery per spec Appendix B.1).

#include "dtm.hpp"

namespace rvdbg {

namespace {

// dmi register field offsets (docs/jtag-dtm.md "The dmi register and the
// op protocol"): op is scanned FIRST, then the 32 data bits, then the
// address — op at bits [1:0], data at [33:2], address at [abits+33:34].
constexpr int kOpOffset = 0;
constexpr int kDataOffset = 2;
constexpr int kAddrOffset = 34;

// dmi op encodings (request side).
constexpr uint32_t DMI_OP_NOP = 0;
constexpr uint32_t DMI_OP_READ = 1;
constexpr uint32_t DMI_OP_WRITE = 2;

}  // namespace

void JtagDtm::initialize() {
    // 1. TAP reset (TRST* or 5 x TMS=1) -> IR defaults to IDCODE.
    transport_.jtag_reset();

    // Scan IDCODE to identify the device.
    transport_.jtag_ir(IR_IDCODE);
    uint8_t tdo[kMaxScanBytes] = {};
    transport_.jtag_dr(nullptr, tdo, 32, 1);
    idcode_ = get_bits(tdo, 0, 32);

    // 2. Select dtmcs, scan -> learn scan width (abits) + dwell (idle).
    transport_.jtag_ir(IR_DTMCS);
    transport_.jtag_dr(nullptr, tdo, 32, 1);
    dtmcs_ = get_bits(tdo, 0, 32);

    const int version = static_cast<int>(dtmcs_ & 0xF);
    if (version != 1) {
        throw DebugError("dtmcs.version = " + std::to_string(version) +
                         " (want 1 = spec 0.13/1.0 DTM)");
    }
    abits_ = static_cast<int>((dtmcs_ >> 4) & 0x3F);
    if (abits_ < 7 || abits_ > 32) {
        throw DebugError("dtmcs.abits = " + std::to_string(abits_) +
                         " (spec range is 7..32)");
    }
    idle_ = static_cast<int>((dtmcs_ >> 12) & 0x7);

    // 3. Select IR = dmi — stays selected from here on.
    transport_.jtag_ir(IR_DMI);
}

uint32_t JtagDtm::dtmcs_read() {
    transport_.jtag_ir(IR_DTMCS);
    uint8_t tdo[kMaxScanBytes] = {};
    transport_.jtag_dr(nullptr, tdo, 32, 1);
    transport_.jtag_ir(IR_DMI);
    return get_bits(tdo, 0, 32);
}

void JtagDtm::dmi_reset() {
    scan_dtmcs_write(DTMCS_DMIRESET);
}

void JtagDtm::dmi_hard_reset() {
    scan_dtmcs_write(DTMCS_DMIHARDRESET);
}

void JtagDtm::scan_dtmcs_write(uint32_t value) {
    transport_.jtag_ir(IR_DTMCS);
    uint8_t tdi[kMaxScanBytes] = {};
    put_bits(tdi, 0, value, 32);
    transport_.jtag_dr(tdi, nullptr, 32, 1);
    transport_.jtag_ir(IR_DMI);
}

void JtagDtm::dmi_write(uint32_t addr, uint32_t data) {
    dmi_transaction(DMI_OP_WRITE, addr, data);
}

uint32_t JtagDtm::dmi_read(uint32_t addr) {
    return dmi_transaction(DMI_OP_READ, addr, 0);
}

uint32_t JtagDtm::dmi_poll(uint32_t addr, uint32_t mask, uint32_t expected,
                           unsigned max_reads) {
    uint32_t value = 0;
    for (unsigned i = 0; i < max_reads; ++i) {
        value = dmi_read(addr);
        if ((value & mask) == expected) {
            return value;
        }
    }
    throw PollTimeout("dmi_poll(0x" + std::to_string(addr) +
                      ") timed out waiting for (v & 0x" +
                      std::to_string(mask) + ") == 0x" +
                      std::to_string(expected), addr, mask, expected, value);
}

uint32_t JtagDtm::dmi_transaction(uint32_t op, uint32_t addr, uint32_t data) {
    const int nbits = abits_ + 34;  // dmi register width

    // Every DMI read/write costs two dmi scans: scan 1 carries the
    // request, scan 2's Capture-DR latches the result. Real debuggers
    // pipeline requests into scan 2 (see docs/jtag-dtm.md "Pipelined
    // burst"); this driver keeps the simple one-op-at-a-time shape.
    for (int attempt = 0;; ++attempt) {
        uint8_t request[kMaxScanBytes] = {};
        put_bits(request, kOpOffset, op, 2);
        put_bits(request, kDataOffset, data, 32);
        put_bits(request, kAddrOffset, addr, abits_);
        uint8_t tdo[kMaxScanBytes] = {};
        transport_.jtag_dr(request, tdo, nbits, idle_);

        // Result scan: a nop request carrying the capture of our op.
        uint8_t nop[kMaxScanBytes] = {};
        transport_.jtag_dr(nop, tdo, nbits, idle_);

        const uint32_t status = get_bits(tdo, kOpOffset, 2);
        if (status == 0) {
            // Success. After a read, data holds the value read.
            return get_bits(tdo, kDataOffset, 32);
        }
        if (status == 3) {
            // Busy: the request was ignored (sticky). Recovery per spec
            // B.1 / OpenOCD: scan dtmcs with dmireset=1, increase the
            // Run-Test/Idle dwell, perform the request scan again.
            if (attempt >= kMaxBusyRetries) {
                throw DmiBusyError("dmi op stayed busy after " +
                                   std::to_string(kMaxBusyRetries) +
                                   " retries (dtmcs.idle too small? DM stuck?)");
            }
            ++busy_events_;
            dmi_reset();
            ++idle_;  // ratchet the dwell up on each busy
            continue;
        }
        // status == 2: DMI failed. Clear the sticky status and surface it;
        // the caller decides on replay (error-recovery layer 3).
        dmi_reset();
        throw DmiFailedError("dmi scan returned op=2 (failed) for op " +
                             std::to_string(op) + " addr 0x" +
                             std::to_string(addr));
    }
}

}  // namespace rvdbg
