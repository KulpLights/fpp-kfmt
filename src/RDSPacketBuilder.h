#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <tuple>
#include <vector>

// ---------------------------------------------------------------------------
// RDS / RBDS packet type
// ---------------------------------------------------------------------------

using RDSPacket  = std::array<uint8_t, 8>;
using RDSSegment = std::tuple<uint8_t, uint8_t, uint8_t>; // index, len, content-type

// ---------------------------------------------------------------------------
// RT+ content type codes
// from https://github.com/windytan/redsea/blob/cad0b697d538bd64ac358161dfb4b435bace5514/src/tables.cc#L235
// ---------------------------------------------------------------------------

constexpr uint8_t RT_PLUS_TITLE       = 1;
constexpr uint8_t RT_PLUS_ALBUM       = 2;
constexpr uint8_t RT_PLUS_ARTIST      = 4;
constexpr uint8_t RT_PLUS_STATIONNAME = 32;
constexpr uint8_t RT_PLUS_HOMEPAGE    = 39;

// ---------------------------------------------------------------------------
// RDSPacketBuilder
//
// Builds the 8-byte RDS group payloads defined by the NRSC-4 (RBDS) and
// NRSC-G300-A specifications.  All methods are hardware-independent; callers
// are responsible for transmitting the returned packets.
// ---------------------------------------------------------------------------

class RDSPacketBuilder {
public:
    // Station code: 4-char NRSC callsign (K/W prefix) or a raw 4-hex-digit PI
    void setStationCode(const std::string &sc);

    // Program Type code (0-29)
    void setProgramType(uint8_t pty);

    // Stereo/Mono flag – sets M/S bit and the DI stereo flag in PS packets (default: true)
    void setStereo(bool stereo);

    // Advance the RT+ toggle bit to signal a new item to receivers.
    // Call this once when the song changes; do NOT call it on periodic re-broadcasts.
    void toggleRTPlus();

    // Group 0B: Programme Service name – 2 chars per packet
    // Block B carries the B0 flag, M/S, and DI; Block C repeats the PI code.
    std::vector<RDSPacket> buildPSPackets(const std::string &stationName) const;

    // Group 2A: RadioText – 4 chars per packet
    std::vector<RDSPacket> buildRTPackets(const std::string &radioText) const;

    // ODA (Group 3A) + Group 2A + Group 11A: station name / homepage RT+ sequence
    std::vector<RDSPacket> buildStationRTPlusSequence(const std::string &stationName,
                                                      const std::string &homepage);

    // ODA (Group 3A) + Group 2A + Group 11A: now-playing RT+ sequence
    std::vector<RDSPacket> buildItemRTPlusSequence(const std::string &artist,
                                                   const std::string &title,
                                                   const std::string &album);

    // Group 3A + Group 11A: disable RT+ item running bit
    std::vector<RDSPacket> buildRTPlusDisableSequence();

private:
    uint16_t piCode_       = 0;
    uint8_t  ptyCode_      = 0;
    bool     stereo_       = true;
    bool     rtPlusToggle_ = false;

    uint8_t ptyHi() const { return (ptyCode_ & 0x18) >> 3; }
    uint8_t ptyLo() const { return (ptyCode_ << 5) & 0xE0; }

    RDSPacket makePacket(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3,
                         uint8_t b4, uint8_t b5, uint8_t b6, uint8_t b7) const;

    // Inner helper shared by buildStationRTPlusSequence / buildItemRTPlusSequence
    void appendRTPlusSegments(std::string &rt,
                              std::vector<RDSSegment> &segments,
                              bool enable,
                              std::vector<RDSPacket> &out);
};
