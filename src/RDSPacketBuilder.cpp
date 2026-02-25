#include "RDSPacketBuilder.h"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

void RDSPacketBuilder::setStationCode(const std::string &sc) {
    if (sc.length() < 4) {
        return;
    }
    if (sc[0] != 'K' && sc[0] != 'W') {
        piCode_ = (uint16_t)(std::strtol(sc.c_str(), nullptr, 16) & 0xFFFF);
    } else {
        uint16_t v2 = sc[1] - 65;
        uint16_t v3 = sc[2] - 65;
        uint16_t v4 = sc[3] - 65;
        if (v2 > 26) v2 = 0;
        if (v3 > 26) v3 = 0;
        if (v4 > 26) v4 = 0;
        uint16_t v1 = (sc[0] == 'W') ? 21672 : 4096;
        v1 += v4;
        v1 += v3 * 26;
        v1 += v2 * 26 * 26;
        piCode_ = v1 & 0xFFFF;
    }
}

void RDSPacketBuilder::setProgramType(uint8_t pty) {
    ptyCode_ = pty & 0x1F;
}

void RDSPacketBuilder::setStereo(bool stereo) {
    stereo_ = stereo;
}

void RDSPacketBuilder::toggleRTPlus() {
    rtPlusToggle_ = !rtPlusToggle_;
}

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

RDSPacket RDSPacketBuilder::makePacket(uint8_t b0, uint8_t b1, uint8_t b2,
                                        uint8_t b3, uint8_t b4, uint8_t b5,
                                        uint8_t b6, uint8_t b7) const {
    return {b0, b1, b2, b3, b4, b5, b6, b7};
}

// ---------------------------------------------------------------------------
// Group 0B – Programme Service name
//
// Block B bit layout (16 bits):
//   [15:12] Group type  = 0000
//   [11]    B0 flag     = 1  (version B)
//   [10]    TP          = 0
//   [9:5]   PTY
//   [4]     TA          = 0
//   [3]     M/S         = stereo_ ? 1 : 0
//   [2]     DI          = one bit of the 4-bit DI codeword, transmitted MSB-first:
//                           seg 0 → d3 (dynamic PTY),  seg 1 → d2 (compressed),
//                           seg 2 → d1 (artificial head), seg 3 → d0 (stereo/mono)
//   [1:0]   Segment address (0–3)
//
// Block C (bytes 4-5): repeated PI code (required by Group 0B)
// ---------------------------------------------------------------------------

std::vector<RDSPacket> RDSPacketBuilder::buildPSPackets(const std::string &stationName) const {
    char buf[9] = {0}; // PS Name is max 8 characters, zero-padded
    strncpy(buf, stationName.c_str(), sizeof(buf) - 1);
    // Always send all 4 segments (8 chars): the DI stereo bit (d0) lives in
    // segment 3, so all four packets must be transmitted every time.
    const int rds_len = 8;

    const uint8_t ms = stereo_ ? 0x08 : 0x00; // M/S bit (bit 3 of byte 3)

    std::vector<RDSPacket> packets;
    for (int i = 0; i < rds_len; i += 2) {
        uint8_t seg = (uint8_t)(i / 2);
        // DI bit (bit 2): DI codeword is sent MSB-first; d0 (stereo/mono) is in segment 3
        uint8_t di = (seg == 3 && stereo_) ? 0x04 : 0x00;

        packets.push_back(makePacket(
            (piCode_ >> 8) & 0xFF,          // Block A hi – PI code
            piCode_ & 0xFF,                 // Block A lo – PI code
            ptyHi() | 0x08,                 // Block B hi – group 0, B0=1 (version B), TP=0, PTY[4:3]
            ptyLo() | ms | di | seg,        // Block B lo – PTY[2:0], TA=0, M/S, DI, segment
            (piCode_ >> 8) & 0xFF,          // Block C hi – PI code (repeated, per Group 0B spec)
            piCode_ & 0xFF,                 // Block C lo – PI code (repeated)
            buf[i],                         // Block D – PS char pair
            buf[i + 1]));
    }
    return packets;
}

// ---------------------------------------------------------------------------
// Group 2A – RadioText
//
// Block B bit layout (16 bits):
//   [15:12] Group type  = 0010  (group 2)
//   [11]    B0 flag     = 0     (version A)
//   [10]    TP          = 0
//   [9:5]   PTY
//   [4]     A/B flag    = 0     (toggle to tell receivers to clear the display)
//   [3:0]   Segment address (0–15)
//
// Block C (bytes 4-5): RT characters at positions i, i+1
// Block D (bytes 6-7): RT characters at positions i+2, i+3
//
// Messages shorter than 64 characters must be terminated with 0x0D (CR) so
// receivers know where the text ends (NRSC-4 / RDS spec §3.1.5.3).
// Remaining bytes are 0x00 (from memset).  Length is padded to a multiple of
// 4 because each Group 2A packet carries exactly 4 characters.
// ---------------------------------------------------------------------------

std::vector<RDSPacket> RDSPacketBuilder::buildRTPackets(const std::string &radioText) const {
    char buf[65];
    memset(buf, 0, sizeof(buf));
    strncpy(buf, radioText.c_str(), 64);
    int str_len = (int)strlen(buf);

    // Append 0x0D terminator when text is shorter than the 64-char maximum
    if (str_len < 64) {
        buf[str_len++] = 0x0D;
    }

    // Pad to a multiple of 4 (one Group 2A packet = 4 chars)
    int rds_len = (str_len + 3) & ~3;

    std::vector<RDSPacket> packets;
    for (int i = 0; i < rds_len; i += 4) {
        packets.push_back(makePacket(
            (piCode_ >> 8) & 0xFF,
            piCode_ & 0xFF,
            0x20 | ptyHi(),
            ptyLo() | (i / 4),
            buf[i], buf[i + 1],
            buf[i + 2], buf[i + 3]));
    }
    return packets;
}

// ---------------------------------------------------------------------------
// Group 11A – RT+ tag data (private helper, appends to out)
//
// Each Group 11A packet carries tag descriptors for up to 2 content tags.
// Tags are drawn from the segments list; when the list exceeds 63 RT
// characters the RT string is sliced and sent in multiple rounds.
//
// Block B bit layout (16 bits):
//   [15:12] Group type  = 1011  (group 11)
//   [11]    B0 flag     = 0     (version A)
//   [10]    TP          = 0
//   [9:5]   PTY
//   [4]     IT          = rtPlusToggle_ (Item Toggle – changes on new item)
//   [3]     IR          = 0 in tag packets; 1 in the trailing "running" packet
//   [2:0]   CT1[5:3]    = ContentType1 high 3 bits
//
// Block C (16 bits):
//   [15:13] CT1[2:0]    = ContentType1 low 3 bits
//   [12:7]  Start1      = RT start position of tag1  (0–63)
//   [6:1]   Length1     = tag1 char count – 1        (0–63)
//   [0]     CT2[5]      = ContentType2 MSB
//
// Block D (16 bits):
//   [15:11] CT2[4:0]    = ContentType2 lower 5 bits
//   [10:5]  Start2      = RT start position of tag2  (0–63)
//   [4:0]   Length2     = tag2 char count – 1        (0–31, 5 bits only)
//
// When enable=true a trailing packet with IT|IR set and no tag data is
// appended to signal "item running" to receivers.
// ---------------------------------------------------------------------------

void RDSPacketBuilder::appendRTPlusSegments(std::string &rt,
                                             std::vector<RDSSegment> &segments,
                                             bool enable,
                                             std::vector<RDSPacket> &out) {
    while (!rt.empty()) {
        // Find how many segments fit within the 64-char RT limit
        int maxSeg = (int)segments.size() - 1;
        int maxIdx = (int)rt.length();
        for (int x = 1; x < (int)segments.size(); x++) {
            auto &seg = segments[x];
            if (std::get<0>(seg) + std::get<1>(seg) > 63) {
                maxSeg = x - 1;
                maxIdx = std::get<0>(segments[maxSeg]) + std::get<1>(segments[maxSeg]);
                break;
            }
        }
        std::string rt2 = rt.substr(0, maxIdx);

        // Append the Group 2A RT packets for this slice
        for (auto &p : buildRTPackets(rt2)) {
            out.push_back(p);
        }

        // Emit one Group 11A packet per pair of segments (tag1 + tag2).
        // Segments with length >= 32 are sent alone (they occupy the full
        // Block D start/length space and can't share a packet).
        int numSend = 2;
        for (int x = 0; x <= maxSeg; x += numSend) {
            auto segment = segments[x];

            RDSSegment nextSegment = {0, 0, 0};
            numSend = 2;
            if (std::get<1>(segment) < 32 && x + 1 <= maxSeg) {
                // Both segments fit – pair them, longer tag goes first
                nextSegment = segments[x + 1];
                if (std::get<1>(nextSegment) >= 32) {
                    std::swap(segment, nextSegment);
                }
            } else if (x + 1 > maxSeg) {
                // Last segment with no pair – send alone with empty tag2
                numSend = 1;
            } else {
                // Segment length >= 32 – send alone
                numSend = 1;
            }

            uint8_t idx1  = std::get<0>(segment)    & 0x3F; // 6-bit start pos
            uint8_t len1  = std::get<1>(segment)    & 0x3F; // 6-bit length
            uint8_t type1 = std::get<2>(segment)    & 0x3F; // 6-bit content type

            uint8_t idx2      = std::get<0>(nextSegment) & 0x3F;
            uint8_t len2      = std::get<1>(nextSegment) & 0x3F; // only 5 bits used in Block D
            uint8_t type2_raw = std::get<2>(nextSegment) & 0x3F; // full 6-bit content type
            uint8_t type2     = type2_raw & 0x1F;                // lower 5 bits → Block D[15:11]
            uint8_t ct2_msb   = (type2_raw >> 5) & 0x01;        // CT2[5]        → Block C[0]

            uint8_t rb = rtPlusToggle_ ? 0x10 : 0x00; // IT bit (bit 4 of Block B lo); IR=0

            out.push_back(makePacket(
                (piCode_ >> 8) & 0xFF,                           // Block A hi – PI code
                piCode_ & 0xFF,                                  // Block A lo – PI code
                0xB0 | ptyHi(),                                  // Block B hi – group 11, B0=0, TP=0, PTY[4:3]
                (uint8_t)(ptyLo() | rb | ((type1 >> 3) & 0x07)),// Block B lo – PTY[2:0], IT, IR=0, CT1[5:3]
                (uint8_t)(((type1 & 0x07) << 5) | (idx1 >> 1)), // Block C hi – CT1[2:0], Start1[5:1]
                (uint8_t)(((idx1 & 0x01) << 7) | (len1 << 1) | ct2_msb), // Block C lo – Start1[0], Length1, CT2[5]
                (uint8_t)((type2 << 3) | (idx2 >> 3)),          // Block D hi – CT2[4:0], Start2[5:3]
                (uint8_t)(((idx2 & 0x07) << 5) | (len2 & 0x1F)))); // Block D lo – Start2[2:0], Length2[4:0]
        }

        rt.erase(0, rt2.length());
        for (int i = 0; i <= maxSeg; i++) {
            segments.erase(segments.begin());
        }
        for (auto &seg : segments) {
            std::get<0>(seg) -= (uint8_t)rt2.length();
        }
    }

    if (enable) {
        // Trailing "item running" marker – IT|IR set, no tag data
        uint8_t rb = rtPlusToggle_ ? 0x18 : 0x08; // IT | IR
        out.push_back(makePacket(
            (piCode_ >> 8) & 0xFF,
            piCode_ & 0xFF,
            0xB0 | ptyHi(),    // Block B hi – group 11, B0=0, TP=0, PTY[4:3]
            ptyLo() | rb,      // Block B lo – PTY[2:0], IT, IR=1, CT1[5:3]=0
            0x00, 0x00,
            0x00, 0x00));
    }
}

// ---------------------------------------------------------------------------
// ODA + RT+ – station name / homepage (Group 3A announcement + Group 11A)
//
// Emits:
//   1. One Group 3A ODA announcement packet (Block D = RT+ AID 0x4BD7,
//      Block B[4:0] = 0x16 = group 11A app-group-type code).
//   2. Group 2A + Group 11A packets for the RT string:
//        "<station_name> - <homepage_url>"
//      with RT+ tags marking the station name (type 32) and homepage (type 39).
//
// Group 3A Block B bit layout:
//   [15:12] Group type   = 0011  (group 3)
//   [11]    B0 flag      = 0     (version A)
//   [10]    TP           = 0
//   [9:5]   PTY
//   [4:0]   App Group Type = 0x16 = 22 = group 11 * 2 + version A
//
// Group 3A Block C = 0x0000  (message bits, zero for RT+ announcement)
// Group 3A Block D = 0x4BD7  (RT+ Application Identification)
// ---------------------------------------------------------------------------

std::vector<RDSPacket> RDSPacketBuilder::buildStationRTPlusSequence(
    const std::string &stationName,
    const std::string &homepage) {

    std::vector<RDSPacket> out;

    // ODA announcement (Group 3A)
    out.push_back(makePacket(
        (piCode_ >> 8) & 0xFF,      // Block A hi – PI code
        piCode_ & 0xFF,             // Block A lo – PI code
        0x30 | ptyHi(),             // Block B hi – group 3, B0=0, TP=0, PTY[4:3]
        ptyLo() | 0x16,             // Block B lo – PTY[2:0], App Group Type 22 (Group 11A)
        0x00, 0x00,                 // Block C   – message bits (zero for RT+)
        0x4B, 0xD7));               // Block D   – RT+ AID

    std::string rt;
    int stationNameLen = 1;
    int homepageLen    = 1;

    int stationNameIdx = (int)rt.length();
    if (!stationName.empty()) {
        stationNameLen = (int)stationName.length();
        rt += stationName;
    } else {
        rt += " ";
    }
    // homepageIdx must be captured AFTER the " - " separator is appended
    int homepageIdx = (int)rt.length();
    if (!homepage.empty()) {
        rt += " - ";
        homepageIdx = (int)rt.length(); // update to point past the separator
        homepageLen = (int)homepage.length();
        rt += homepage;
    } else {
        rt += " ";
    }

    std::vector<RDSSegment> segments;
    segments.emplace_back((uint8_t)stationNameIdx, (uint8_t)stationNameLen, RT_PLUS_STATIONNAME);
    segments.emplace_back((uint8_t)homepageIdx,    (uint8_t)homepageLen,    RT_PLUS_HOMEPAGE);
    appendRTPlusSegments(rt, segments, false, out);
    return out;
}

// ---------------------------------------------------------------------------
// ODA + RT+ – now-playing item (artist / title / album)
//
// Emits:
//   1. One Group 3A ODA announcement packet (same layout as station sequence).
//   2. Group 2A + Group 11A packets for the RT string:
//        "<title> - <artist> - <album>"
//      with RT+ tags marking title (type 1), artist (type 4), album (type 2).
//      Text is truncated to keep title + artist within the 64-char RT limit;
//      album is omitted if it would overflow.
//
// Call toggleRTPlus() once before this method when the song changes so that
// receivers see a new Item Toggle value and update their display.
// ---------------------------------------------------------------------------

std::vector<RDSPacket> RDSPacketBuilder::buildItemRTPlusSequence(
    const std::string &artist,
    const std::string &title,
    const std::string &album) {

    std::vector<RDSPacket> out;

    // ODA announcement (Group 3A)
    out.push_back(makePacket(
        (piCode_ >> 8) & 0xFF,      // Block A hi – PI code
        piCode_ & 0xFF,             // Block A lo – PI code
        0x30 | ptyHi(),             // Block B hi – group 3, B0=0, TP=0, PTY[4:3]
        ptyLo() | 0x16,             // Block B lo – PTY[2:0], App Group Type 22 (Group 11A)
        0x00, 0x00,                 // Block C   – message bits (zero for RT+)
        0x4B, 0xD7));               // Block D   – RT+ AID

    std::string rt;
    int titleLen  = 1;
    int albumLen  = 1;
    int artistLen = 1;

    int titleIdx = (int)rt.length();
    if (!title.empty()) {
        titleLen = (int)title.length();
        rt += title;
    } else {
        rt += " ";
    }

    int artistIdx = (int)rt.length();
    if (!artist.empty()) {
        rt += " - ";
        artistIdx = (int)rt.length();
        artistLen = (int)artist.length();
        rt += artist;
    } else {
        rt += " ";
    }

    int albumIdx = (int)rt.length();
    if (albumIdx > 63) {
        artistLen = 63 - artistIdx + 1;
        rt.resize(63);
        albumLen = 1;
        albumIdx = (int)rt.length();
        rt += " ";
    } else {
        if (!album.empty()) {
            if (rt.length() + album.length() + 3 > 64) {
                albumLen = 1;
                rt += " ";
            } else {
                rt += " - ";
                albumIdx = (int)rt.length();
                albumLen = (int)album.length();
                rt += album;
            }
        } else {
            rt += " ";
        }
    }

    std::vector<RDSSegment> segments;
    segments.emplace_back((uint8_t)titleIdx,  (uint8_t)titleLen,  RT_PLUS_TITLE);
    segments.emplace_back((uint8_t)artistIdx, (uint8_t)artistLen, RT_PLUS_ARTIST);
    segments.emplace_back((uint8_t)albumIdx,  (uint8_t)albumLen,  RT_PLUS_ALBUM);
    appendRTPlusSegments(rt, segments,
                         !title.empty() || !album.empty() || !artist.empty(),
                         out);
    return out;
}

// ---------------------------------------------------------------------------
// RT+ disable (Group 3A announcement + Group 11A marker)
//
// Emits:
//   1. Group 3A ODA announcement (same as item sequence).
//   2. Group 11A packet with IT=0 and IR=0 (item not running).
//      Block B lo = PTY[2:0] only; IT, IR, and all tag bits are zero.
// ---------------------------------------------------------------------------

std::vector<RDSPacket> RDSPacketBuilder::buildRTPlusDisableSequence() {
    std::vector<RDSPacket> out;

    // ODA announcement (Group 3A)
    out.push_back(makePacket(
        (piCode_ >> 8) & 0xFF,      // Block A hi – PI code
        piCode_ & 0xFF,             // Block A lo – PI code
        0x30 | ptyHi(),             // Block B hi – group 3, B0=0, TP=0, PTY[4:3]
        ptyLo() | 0x16,             // Block B lo – PTY[2:0], App Group Type 22 (Group 11A)
        0x00, 0x00,                 // Block C   – message bits (zero)
        0x4B, 0xD7));               // Block D   – RT+ AID

    // Item not running (Group 11A – IT=0, IR=0, no tag data)
    out.push_back(makePacket(
        (piCode_ >> 8) & 0xFF,      // Block A hi – PI code
        piCode_ & 0xFF,             // Block A lo – PI code
        0xB0 | ptyHi(),             // Block B hi – group 11, B0=0, TP=0, PTY[4:3]
        ptyLo(),                    // Block B lo – PTY[2:0], IT=0, IR=0, CT1=0
        0x00, 0x00,                 // Block C   – no tag data
        0x00, 0x00));               // Block D   – no tag data

    return out;
}
