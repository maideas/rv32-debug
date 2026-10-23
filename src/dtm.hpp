// rv32-debug — JTAG Debug Transport Module (DTM) driver.
//
// Implements the transport described in docs/jtag-dtm.md on top of a
// JtagTransport:
//   - dtmcs register (version, abits, idle, dmistat, dmireset, dmihardreset)
//   - the `dmi` register op protocol: one DMI read/write costs two `dmi`
//     scans (request + result), scan order op -> data -> address (LSB-first)
//   - sticky busy/failed handling with the spec B.1 recovery: on busy,
//     dtmcs.dmireset, ratchet up the Run-Test/Idle dwell, repeat the
//     request scan
//
// One level above the raw transport, one level below the debug sequences:
// dmi_write(), dmi_read(), dmi_poll() (docs/debug-sequences.md notation
// "dmi write <addr> <- <value>" / "dmi read <addr> -> <value>" /
// "poll <addr> until <cond>").
#ifndef RVDBG_DTM_HPP
#define RVDBG_DTM_HPP

#include "jtag_transport.hpp"

#include <stdexcept>
#include <string>

namespace rvdbg {

// ---- exceptions ----

class DebugError : public std::runtime_error {
public:
    explicit DebugError(const std::string& what) : std::runtime_error(what) {}
};

// The DTM stayed busy for too many retries (spec B.1 recovery exhausted).
class DmiBusyError : public DebugError {
public:
    explicit DmiBusyError(const std::string& what) : DebugError(what) {}
};

// The DMI itself reported failure (dmi scan returned op=2). The sticky
// status has already been cleared with dtmcs.dmireset; the caller decides
// whether to replay (error-recovery layer 3, docs/debug-sequences.md).
class DmiFailedError : public DebugError {
public:
    explicit DmiFailedError(const std::string& what) : DebugError(what) {}
};

// dmi_poll() gave up; carries the last observed value for diagnostics.
class PollTimeout : public DebugError {
public:
    PollTimeout(const std::string& what, uint32_t addr_, uint32_t mask_,
                uint32_t expected_, uint32_t last)
        : DebugError(what), addr(addr_), mask(mask_), expected(expected_),
          last_value(last) {}
    uint32_t addr;
    uint32_t mask;
    uint32_t expected;
    uint32_t last_value;
};

class JtagDtm {
public:
    explicit JtagDtm(JtagTransport& transport) : transport_(transport) {}

    // Bring up the transport and the DTM (docs/debug-sequences.md
    // "Connect and discovery", steps 1-3): TAP reset, scan IDCODE, read
    // dtmcs (verify version=1, learn abits and the idle dwell hint),
    // then select the dmi instruction — it stays selected from here on.
    void initialize();

    uint32_t idcode() const { return idcode_; }   // from initialize()
    uint32_t dtmcs_raw() const { return dtmcs_; }  // raw dtmcs read
    int abits() const { return abits_; }
    int idle() const { return idle_; }            // current RTI dwell used
    int busy_events() const { return busy_events_; }  // busy recoveries seen

    // Re-scan dtmcs (e.g. to inspect dmistat during recovery).
    uint32_t dtmcs_read();

    // Clear the sticky dmi status (error-recovery layer 2/3).
    void dmi_reset();       // dtmcs.dmireset = 1
    void dmi_hard_reset();  // dtmcs.dmihardreset = 1 (full DTM reset)

    // One DMI write: "dmi write <addr> <- <data>".
    void dmi_write(uint32_t addr, uint32_t data);

    // One DMI read: "dmi read <addr> -> <data>".
    uint32_t dmi_read(uint32_t addr);

    // "poll <addr> until (v & mask) == expected": repeat dmi_read() until
    // the condition holds, then return that value. dmstatus and abstractcs
    // are side-effect-free, so polling them is safe (docs/debug-sequences.md
    // "Conventions and notation"). Throws PollTimeout after max_reads.
    uint32_t dmi_poll(uint32_t addr, uint32_t mask, uint32_t expected,
                      unsigned max_reads = 4096);

private:
    // One complete DMI operation: request scan + result scan, with the
    // sticky-busy recovery loop around it. Returns the captured read data
    // (0 for writes).
    uint32_t dmi_transaction(uint32_t op, uint32_t addr, uint32_t data);

    // Scan the dtmcs register (IR detour and back) for a write.
    void scan_dtmcs_write(uint32_t value);

    static constexpr int kMaxBusyRetries = 64;
    static constexpr size_t kMaxScanBytes = 16;  // fits abits=32: 66 bits

    JtagTransport& transport_;
    uint32_t idcode_ = 0;
    uint32_t dtmcs_ = 0;
    int abits_ = 7;
    int idle_ = 1;
    int busy_events_ = 0;
};

}  // namespace rvdbg

#endif  // RVDBG_DTM_HPP
