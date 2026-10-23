// rv32-debug — reference C++ implementation of docs/debug-sequences.md.
//
// JTAG transport abstraction (the bottom layer).
//
// Everything above this file (JtagDtm, DebugSequences) talks to the target
// exclusively through this interface: `jtag_ir()` selects a JTAG
// instruction, `jtag_dr()` shifts one data-register scan, `jtag_reset()`
// resets the TAP (TRST* or 5 x TMS=1, docs/jtag-dtm.md "JTAG primer").
// Implement it against your JTAG adapter / bit-banger / simulation model.
//
// Conventions (docs/jtag-dtm.md):
//   - IR is >= 5 bits, shifted LSB-first.
//   - Scan data is packed LSB-first: bit i of the scan stream lives in
//     byte i/8, bit i%8 of the buffer (see put_bits()/get_bits()).
//   - `idle` is the number of Run-Test/Idle cycles to spend after the scan
//     (the dwell hint reported in dtmcs.idle); 0 skips RTI entirely.
#ifndef RVDBG_JTAG_TRANSPORT_HPP
#define RVDBG_JTAG_TRANSPORT_HPP

#include <cstdint>
#include <cstddef>

namespace rvdbg {

// RISC-V DTM instruction encodings (docs/jtag-dtm.md "DTM instruction
// set"). Every unimplemented encoding must select BYPASS.
constexpr uint32_t IR_BYPASS = 0x00;
constexpr uint32_t IR_IDCODE = 0x01;  // selected at TAP reset
constexpr uint32_t IR_DTMCS = 0x10;
constexpr uint32_t IR_DMI = 0x11;

// dtmcs write-1-to-trigger bits.
constexpr uint32_t DTMCS_DMIRESET = 1u << 16;      // clear sticky status
constexpr uint32_t DTMCS_DMIHARDRESET = 1u << 17;  // full DTM reset

class JtagTransport {
public:
    virtual ~JtagTransport() = default;

    // TAP reset: IR defaults to IDCODE (0x01). The PULP/OpenTitan DTM also
    // treats this like dmihardreset — see docs/jtag-dtm.md "Reset and
    // recovery".
    virtual void jtag_reset() = 0;

    // Select a JTAG instruction (Update-IR). Stays selected until changed.
    virtual void jtag_ir(uint32_t ir, int irlen = 5) = 0;

    // One data-register scan (Capture-DR -> Shift-DR x nbits -> Update-DR
    // -> Run-Test/Idle x idle). LSB-first. `tdi` is shifted into the target
    // (nullptr => shift zeros, e.g. for pure reads); the bits shifted out
    // are stored LSB-first in `tdo` (nullptr => discard).
    virtual void jtag_dr(const uint8_t* tdi, uint8_t* tdo, int nbits,
                         int idle = 1) = 0;
};

// ---- LSB-first bit packing helpers shared by the DTM and its transports ----

// Store `nbits` of `value` starting at stream bit `offset`. `buf` must be
// zero-initialized by the caller.
inline void put_bits(uint8_t* buf, int offset, uint32_t value, int nbits) {
    for (int i = 0; i < nbits; ++i) {
        if ((value >> i) & 1u) {
            int bit = offset + i;
            buf[bit / 8] |= static_cast<uint8_t>(1u << (bit % 8));
        }
    }
}

// Extract `nbits` starting at stream bit `offset` as a little-endian value.
inline uint32_t get_bits(const uint8_t* buf, int offset, int nbits) {
    uint32_t value = 0;
    for (int i = 0; i < nbits; ++i) {
        int bit = offset + i;
        value |= static_cast<uint32_t>((buf[bit / 8] >> (bit % 8)) & 1u) << i;
    }
    return value;
}

}  // namespace rvdbg

#endif  // RVDBG_JTAG_TRANSPORT_HPP
