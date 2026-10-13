// Builds real RDS groups with RDSPacketBuilder and decodes them the way a receiver does, using
// the code table E.1 from redsea (an independent RDS decoder), to check what a radio displays.
//
//   make -C tests && ./tests/rds_text_test
#include "../src/RDSCharset.h"
#include "../src/RDSPacketBuilder.h"

#include <cstdio>
#include <map>
#include <string>
#include <vector>

// Receiver's view of each RDS byte (redsea src/text/rdsstring.cc, codetable_G0).
static const char *kG0[256] = {
    " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", "\n", " ", " ", "\r", " ", " ",
    " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", " ", "\u00AD",
    " ", "!", "\"", "#", "¤", "%", "&", "'", "(", ")", "*", "+", ",", "-", ".", "/",
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", ":", ";", "<", "=", ">", "?",
    "@", "A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O",
    "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z", "[", "\\", "]", "―", "_",
    "‖", "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l", "m", "n", "o",
    "p", "q", "r", "s", "t", "u", "v", "w", "x", "y", "z", "{", "|", "}", "¯", " ",
    "á", "à", "é", "è", "í", "ì", "ó", "ò", "ú", "ù", "Ñ", "Ç", "Ş", "β", "¡", "Ĳ",
    "â", "ä", "ê", "ë", "î", "ï", "ô", "ö", "û", "ü", "ñ", "ç", "ş", "ǧ", "ı", "ĳ",
    "ª", "α", "©", "‰", "Ǧ", "ě", "ň", "ő", "π", "€", "£", "$", "←", "↑", "→", "↓",
    "º", "¹", "²", "³", "±", "İ", "ń", "ű", "µ", "¿", "÷", "°", "¼", "½", "¾", "§",
    "Á", "À", "É", "È", "Í", "Ì", "Ó", "Ò", "Ú", "Ù", "Ř", "Č", "Š", "Ž", "Ð", "Ŀ",
    "Â", "Ä", "Ê", "Ë", "Î", "Ï", "Ô", "Ö", "Û", "Ü", "ř", "č", "š", "ž", "đ", "ŀ",
    "Ã", "Å", "Æ", "Œ", "ŷ", "Ý", "Õ", "Ø", "Þ", "Ŋ", "Ŕ", "Ć", "Ś", "Ź", "Ŧ", "ð",
    "ã", "å", "æ", "œ", "ŵ", "ý", "õ", "ø", "þ", "ŋ", "ŕ", "ć", "ś", "ź", "ŧ", " ",
};

static int failures = 0;
static void expect(bool ok, const std::string &what, const std::string &got) {
    std::printf("%s  %s%s%s\n", ok ? "PASS" : "FAIL", what.c_str(), ok ? "" : "  got: ", ok ? "" : got.c_str());
    if (!ok) failures++;
}

static std::string decode(const std::string &bytes) {
    std::string s;
    for (unsigned char c : bytes) s += kG0[c];
    return s;
}

// Reassemble Group 2A RadioText (stops at the 0x0D terminator) and the first Group 11A tag pair.
struct OnAir {
    std::string rt;
    std::vector<std::pair<int, std::string>> tags; // content type, tagged text
};
static OnAir receive(const std::vector<RDSPacket> &packets) {
    std::map<int, std::string> segs;
    std::vector<std::tuple<int, int, int>> raw;
    for (auto &p : packets) {
        int group = p[2] >> 4, version = (p[2] >> 3) & 1;
        if (group == 2 && version == 0) {
            segs[p[3] & 0x0F] = std::string{(char)p[4], (char)p[5], (char)p[6], (char)p[7]};
        } else if (group == 11 && version == 0 && !(p[3] & 0x08)) {
            int ct1 = ((p[3] & 0x07) << 3) | (p[4] >> 5);
            int s1 = ((p[4] & 0x1F) << 1) | (p[5] >> 7), l1 = (p[5] >> 1) & 0x3F;
            int ct2 = ((p[5] & 1) << 5) | (p[6] >> 3);
            int s2 = ((p[6] & 0x07) << 3) | (p[7] >> 5), l2 = p[7] & 0x1F;
            raw.emplace_back(ct1, s1, l1);
            raw.emplace_back(ct2, s2, l2);
        }
    }
    std::string bytes;
    for (auto &kv : segs) bytes += kv.second;
    size_t cr = bytes.find('\r');
    if (cr != std::string::npos) bytes.resize(cr);
    OnAir a;
    a.rt = decode(bytes);
    while (!a.rt.empty() && a.rt.back() == ' ') a.rt.pop_back(); // trailing padding is invisible
    for (auto &[ct, start, len] : raw) {
        if (ct) a.tags.emplace_back(ct, decode(bytes.substr(start, len + 1)));
    }
    return a;
}
static std::string tag(const OnAir &a, int ct) {
    for (auto &t : a.tags) if (t.first == ct) return t.second;
    return "<none>";
}
static std::string ps(const std::vector<RDSPacket> &packets) {
    std::string bytes(8, ' ');
    for (auto &p : packets) {
        int seg = p[3] & 0x03;
        bytes[seg * 2] = (char)p[6];
        bytes[seg * 2 + 1] = (char)p[7];
    }
    return decode(bytes);
}

int main() {
    RDSPacketBuilder b;
    b.setStationCode("KFMT");

    OnAir a = receive(b.buildItemRTPlusSequence("José Pérez", "Feliz Año Nuevo", ""));
    expect(a.rt == "Feliz Año Nuevo - José Pérez", "RadioText shows the accented artist", a.rt);
    expect(tag(a, RT_PLUS_TITLE) == "Feliz Año Nuevo", "RT+ title tag", tag(a, RT_PLUS_TITLE));
    expect(tag(a, RT_PLUS_ARTIST) == "José Pérez", "RT+ artist tag covers the whole name", tag(a, RT_PLUS_ARTIST));

    // 50 characters with two accents: still fits, and the album is placed after them correctly.
    a = receive(b.buildItemRTPlusSequence("Beyoncé", "Ave María", "Navidad Café"));
    expect(a.rt == "Ave María - Beyoncé - Navidad Café", "RadioText with title, artist and album", a.rt);
    expect(tag(a, RT_PLUS_ALBUM) == "Navidad Café", "RT+ album tag", tag(a, RT_PLUS_ALBUM));

    // Typographic punctuation folds; ASCII is untouched.
    a = receive(b.buildRTPackets("Won’t Stop — “Live”…"));
    expect(a.rt == "Won't Stop - \"Live\".", "curly quotes and dashes fold to ASCII", a.rt);
    a = receive(b.buildRTPackets("Silent Night - The Carolers"));
    expect(a.rt == "Silent Night - The Carolers", "plain ASCII is unchanged", a.rt);

    // Emoji are dropped rather than sent as garbage.
    a = receive(b.buildRTPackets("Christmas 🎄 Lights"));
    expect(a.rt == "Christmas  Lights", "emoji dropped", a.rt);

    expect(ps(b.buildPSPackets("José Fe")) == "José Fe ", "PS name with an accent", ps(b.buildPSPackets("José Fe")));
    expect(ps(b.buildPSPackets("RADIO")) == "RADIO   ", "plain PS name", ps(b.buildPSPackets("RADIO")));

    // The plugin splits long station text into 8-character PS fragments before conversion.
    auto frags = utf8Fragments("Feliz Año Nuevo - José Pérez");
    std::string shown;
    for (auto &f : frags) shown += ps(b.buildPSPackets(f)) + "|";
    expect(shown == "Feliz Añ|o Nuevo |- José P|érez    |", "PS fragments split by character", shown);

    a = receive(b.buildStationRTPlusSequence("Station Name", "example.com"));
    expect(a.rt == "Station Name - example.com", "station RT+ text", a.rt);

    std::printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
