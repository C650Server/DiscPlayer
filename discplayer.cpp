// discplayer - a neon-styled terminal music player with a spinning optical disc
// and a live spectrum analyzer.  Single file, C++17, zero build-time libraries.
//
//   build:  g++ -std=c++17 -O2 -static -o discplayer discplayer.cpp
//   run:    ./discplayer [FILE_OR_DIR ...]        (default: ~/Music)
//
// Runtime: needs `mpv` for audio output/decoding; `ffmpeg` is optional and
// only feeds the spectrum analyzer.

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

using std::string;
using std::vector;

static const char* VERSION = "1.0";

// ============================================================================
//  small utilities
// ============================================================================
static double mono_now() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void u8_append(string& o, char32_t c) {
    if (c < 0x80) o += char(c);
    else if (c < 0x800) { o += char(0xC0 | (c >> 6)); o += char(0x80 | (c & 0x3F)); }
    else if (c < 0x10000) { o += char(0xE0 | (c >> 12)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
    else { o += char(0xF0 | (c >> 18)); o += char(0x80 | ((c >> 12) & 0x3F)); o += char(0x80 | ((c >> 6) & 0x3F)); o += char(0x80 | (c & 0x3F)); }
}
static vector<char32_t> u8_decode(const string& s) {
    vector<char32_t> r;
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = s[i];
        char32_t cp;
        size_t len;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 6) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 14) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 30) { cp = c & 7; len = 4; }
        else { r.push_back(0xFFFD); i++; continue; }
        if (i + len > n) { r.push_back(0xFFFD); break; }
        bool ok = true;
        for (size_t k = 1; k < len; k++) {
            unsigned char cc = s[i + k];
            if ((cc & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!ok) { r.push_back(0xFFFD); i++; continue; }
        r.push_back(cp);
        i += len;
    }
    return r;
}
static int cwidth(char32_t c) {
    if (c < 0x20 || (c >= 0x7F && c < 0xA0)) return 0;
    if ((c >= 0x300 && c <= 0x36F) || (c >= 0x200B && c <= 0x200F) || (c >= 0xFE00 && c <= 0xFE0F)) return 0;
    if ((c >= 0x1100 && c <= 0x115F) || (c >= 0x2E80 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7A3) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE6F) || (c >= 0xFF00 && c <= 0xFF60) ||
        (c >= 0xFFE0 && c <= 0xFFE6) || (c >= 0x1F300 && c <= 0x1F64F) || (c >= 0x1F900 && c <= 0x1F9FF) ||
        (c >= 0x20000 && c <= 0x3FFFD))
        return 2;
    return 1;
}
static int u8_width(const string& s) {
    int w = 0;
    for (char32_t c : u8_decode(s)) w += cwidth(c);
    return w;
}
static string lower_ascii(string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 32;
    return s;
}
static string trim(const string& s) {
    size_t a = 0, b = s.size();
    while (a < b && isspace((unsigned char)s[a])) a++;
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
static string fmt_time(double s) {
    if (s < 0) return "--:--";
    int t = (int)s;
    char b[32];
    if (t >= 3600) snprintf(b, sizeof b, "%d:%02d:%02d", t / 3600, (t % 3600) / 60, t % 60);
    else snprintf(b, sizeof b, "%02d:%02d", t / 60, t % 60);
    return b;
}

// ============================================================================
//  minimal JSON (for mpv's IPC)
// ============================================================================
struct J {
    enum T { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    string s;
    vector<J> a;
    vector<std::pair<string, J>> o;
    const J* get(const string& k) const {
        for (auto& p : o) if (p.first == k) return &p.second;
        return nullptr;
    }
};
static void jws(const string& s, size_t& i) { while (i < s.size() && isspace((unsigned char)s[i])) i++; }
static bool jparse(const string& s, size_t& i, J& out, int depth = 0) {
    jws(s, i);
    if (i >= s.size() || depth > 32) return false;
    char c = s[i];
    if (c == '{') {
        out.t = J::Obj;
        i++;
        jws(s, i);
        if (i < s.size() && s[i] == '}') { i++; return true; }
        while (true) {
            jws(s, i);
            J k;
            if (i >= s.size() || s[i] != '"' || !jparse(s, i, k, depth + 1)) return false;
            jws(s, i);
            if (i >= s.size() || s[i] != ':') return false;
            i++;
            J v;
            if (!jparse(s, i, v, depth + 1)) return false;
            out.o.emplace_back(k.s, std::move(v));
            jws(s, i);
            if (i >= s.size()) return false;
            if (s[i] == ',') { i++; continue; }
            if (s[i] == '}') { i++; return true; }
            return false;
        }
    }
    if (c == '[') {
        out.t = J::Arr;
        i++;
        jws(s, i);
        if (i < s.size() && s[i] == ']') { i++; return true; }
        while (true) {
            J v;
            if (!jparse(s, i, v, depth + 1)) return false;
            out.a.push_back(std::move(v));
            jws(s, i);
            if (i >= s.size()) return false;
            if (s[i] == ',') { i++; continue; }
            if (s[i] == ']') { i++; return true; }
            return false;
        }
    }
    if (c == '"') {
        out.t = J::Str;
        i++;
        while (i < s.size() && s[i] != '"') {
            char ch = s[i++];
            if (ch != '\\') { out.s += ch; continue; }
            if (i >= s.size()) return false;
            char e = s[i++];
            switch (e) {
                case 'n': out.s += '\n'; break;
                case 't': out.s += '\t'; break;
                case 'r': out.s += '\r'; break;
                case 'b': out.s += '\b'; break;
                case 'f': out.s += '\f'; break;
                case 'u': {
                    if (i + 4 > s.size()) return false;
                    unsigned cp = strtoul(s.substr(i, 4).c_str(), nullptr, 16);
                    i += 4;
                    if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                        unsigned lo = strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16);
                        if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 6; }
                    }
                    u8_append(out.s, cp);
                    break;
                }
                default: out.s += e;
            }
        }
        if (i >= s.size()) return false;
        i++;
        return true;
    }
    if (s.compare(i, 4, "true") == 0) { out.t = J::Bool; out.b = true; i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { out.t = J::Bool; out.b = false; i += 5; return true; }
    if (s.compare(i, 4, "null") == 0) { out.t = J::Null; i += 4; return true; }
    char* end = nullptr;
    double d = strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i) return false;
    out.t = J::Num;
    out.n = d;
    i = end - s.c_str();
    return true;
}
static string jq(const string& s) {
    string o = "\"";
    for (unsigned char c : s) {
        if (c == '"') o += "\\\"";
        else if (c == '\\') o += "\\\\";
        else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
        else o += char(c);
    }
    return o + "\"";
}
static string jnum(double d) {
    char b[48];
    snprintf(b, sizeof b, "%.3f", d);
    return b;
}

// ============================================================================
//  tags: ID3v2 (mp3), Vorbis comments (flac/ogg/opus), FLAC STREAMINFO length
// ============================================================================
struct Tags {
    string title, artist, album, year, genre, track;
    double length = -1;
};
static string latin1_to_u8(const string& s) {
    string o;
    for (unsigned char c : s) u8_append(o, c);
    return o;
}
static string utf16_to_u8(const string& s, bool be) {
    string o;
    size_t i = 0;
    if (s.size() >= 2) {
        unsigned char a = s[0], b = s[1];
        if (a == 0xFF && b == 0xFE) { be = false; i = 2; }
        else if (a == 0xFE && b == 0xFF) { be = true; i = 2; }
    }
    auto rd = [&](size_t k) -> unsigned {
        unsigned char x = s[k], y = s[k + 1];
        return be ? (unsigned)(x << 8 | y) : (unsigned)(y << 8 | x);
    };
    for (; i + 1 < s.size(); i += 2) {
        unsigned u = rd(i);
        if (u >= 0xD800 && u < 0xDC00 && i + 3 < s.size()) {
            unsigned l = rd(i + 2);
            if (l >= 0xDC00 && l < 0xE000) { u8_append(o, 0x10000 + ((u - 0xD800) << 10) + (l - 0xDC00)); i += 2; continue; }
        }
        u8_append(o, u);
    }
    return o;
}
static string id3_text(const string& body) {
    if (body.empty()) return "";
    int enc = (unsigned char)body[0];
    string d = body.substr(1), r;
    if (enc == 0) r = latin1_to_u8(d);
    else if (enc == 1) r = utf16_to_u8(d, false);
    else if (enc == 2) r = utf16_to_u8(d, true);
    else r = d;
    size_t z = r.find('\0');
    if (z != string::npos) r.resize(z);
    return trim(r);
}
static uint32_t syncsafe(const unsigned char* p) { return (p[0] << 21) | (p[1] << 14) | (p[2] << 7) | p[3]; }
static uint32_t be32(const unsigned char* p) { return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
static uint32_t le32(const unsigned char* p) { return ((uint32_t)p[3] << 24) | (p[2] << 16) | (p[1] << 8) | p[0]; }

static bool read_id3(const string& path, Tags& t) {
    std::ifstream f(path, std::ios::binary);
    unsigned char h[10];
    if (!f.read((char*)h, 10) || memcmp(h, "ID3", 3) != 0) return false;
    int ver = h[3];
    if (ver < 3) return false;
    uint32_t size = syncsafe(h + 6);
    size_t pos = 10;
    if (h[5] & 0x40) {
        unsigned char eh[4];
        if (!f.read((char*)eh, 4)) return false;
        pos += (ver == 4 ? syncsafe(eh) : be32(eh) + 4);
    }
    size_t end = 10 + size;
    while (pos + 10 <= end) {
        f.clear();
        f.seekg(pos);
        unsigned char fh[10];
        if (!f.read((char*)fh, 10) || fh[0] == 0) break;
        string id((char*)fh, 4);
        uint32_t fs = ver == 4 ? syncsafe(fh + 4) : be32(fh + 4);
        if (fs == 0 || pos + 10 + fs > end) break;
        bool want = id == "TIT2" || id == "TPE1" || id == "TALB" || id == "TRCK" || id == "TYER" ||
                    id == "TDRC" || id == "TCON" || id == "TLEN";
        if (want && fs < 65536) {
            string body(fs, '\0');
            if (!f.read(&body[0], fs)) break;
            string v = id3_text(body);
            if (id == "TIT2") t.title = v;
            else if (id == "TPE1") t.artist = v;
            else if (id == "TALB") t.album = v;
            else if (id == "TRCK") t.track = v.substr(0, v.find('/'));
            else if (id == "TYER" || id == "TDRC") { if (t.year.empty()) t.year = v.substr(0, 4); }
            else if (id == "TCON") {
                if (!v.empty() && v[0] == '(') { size_t c = v.find(')'); if (c != string::npos && c + 1 < v.size()) v = v.substr(c + 1); else v.clear(); }
                t.genre = v;
            } else if (id == "TLEN") { double ms = atof(v.c_str()); if (ms > 0) t.length = ms / 1000.0; }
        }
        pos += 10 + fs;
    }
    return true;
}
static void parse_vorbis_comments(const unsigned char* d, size_t n, Tags& t) {
    size_t p = 0;
    if (p + 4 > n) return;
    uint32_t vl = le32(d + p);
    p += 4;
    if (p + vl + 4 > n) return;
    p += vl;
    uint32_t cnt = le32(d + p);
    p += 4;
    for (uint32_t i = 0; i < cnt && p + 4 <= n; i++) {
        uint32_t l = le32(d + p);
        p += 4;
        if (p + l > n) return;
        string c((const char*)d + p, l);
        p += l;
        size_t eq = c.find('=');
        if (eq == string::npos) continue;
        string k = c.substr(0, eq), v = trim(c.substr(eq + 1));
        for (auto& ch : k) ch = toupper((unsigned char)ch);
        if (k == "TITLE") t.title = v;
        else if (k == "ARTIST") t.artist = v;
        else if (k == "ALBUM") t.album = v;
        else if (k == "DATE" || k == "YEAR") { if (t.year.empty()) t.year = v.substr(0, 4); }
        else if (k == "GENRE") t.genre = v;
        else if (k == "TRACKNUMBER") t.track = v.substr(0, v.find('/'));
    }
}
static void read_flac(const string& path, Tags& t) {
    std::ifstream f(path, std::ios::binary);
    char magic[4];
    if (!f.read(magic, 4) || memcmp(magic, "fLaC", 4) != 0) return;
    while (true) {
        unsigned char bh[4];
        if (!f.read((char*)bh, 4)) return;
        bool last = bh[0] & 0x80;
        int type = bh[0] & 0x7F;
        uint32_t len = (bh[1] << 16) | (bh[2] << 8) | bh[3];
        if (type == 0 && len >= 18) {
            unsigned char si[34] = {0};
            if (!f.read((char*)si, std::min<uint32_t>(len, 34))) return;
            uint32_t sr = (si[10] << 12) | (si[11] << 4) | (si[12] >> 4);
            uint64_t total = ((uint64_t)(si[13] & 0x0F) << 32) | ((uint64_t)si[14] << 24) | (si[15] << 16) | (si[16] << 8) | si[17];
            if (sr && total) t.length = (double)total / sr;
            if (len > 34) f.seekg(len - 34, std::ios::cur);
        } else if (type == 4 && len < (1u << 20)) {
            vector<unsigned char> buf(len);
            if (!f.read((char*)buf.data(), len)) return;
            parse_vorbis_comments(buf.data(), len, t);
        } else {
            f.seekg(len, std::ios::cur);
        }
        if (last) return;
    }
}
static void read_ogg(const string& path, Tags& t) {
    std::ifstream f(path, std::ios::binary);
    string buf(1 << 17, '\0');
    f.read(&buf[0], buf.size());
    buf.resize(f.gcount());
    size_t p = buf.find("OpusTags");
    size_t skip = 8;
    if (p == string::npos) { p = buf.find("\x03vorbis"); skip = 7; }
    if (p == string::npos) return;
    p += skip;
    parse_vorbis_comments((const unsigned char*)buf.data() + p, buf.size() - p, t);
}
static string ext_of(const string& p) {
    size_t d = p.rfind('.');
    return d == string::npos ? "" : lower_ascii(p.substr(d + 1));
}
static string stem_of(const string& p) {
    size_t s = p.rfind('/');
    string n = s == string::npos ? p : p.substr(s + 1);
    size_t d = n.rfind('.');
    return d == string::npos ? n : n.substr(0, d);
}
static void read_tags(const string& path, Tags& t) {
    string e = ext_of(path);
    if (e == "mp3" || e == "aac" || e == "wav" || e == "aiff") read_id3(path, t);
    else if (e == "flac") read_flac(path, t);
    else if (e == "ogg" || e == "oga" || e == "opus") read_ogg(path, t);
    if (t.title.empty()) t.title = stem_of(path);
}

// ============================================================================
//  library scan + natural sort
// ============================================================================
static bool is_audio(const string& n) {
    static const char* ex[] = {"mp3", "flac", "ogg", "oga", "opus", "wav", "m4a", "aac", "wma", "ape", "aiff", "aif", "wv", "mka", "mp2"};
    string e = ext_of(n);
    for (auto x : ex) if (e == x) return true;
    return false;
}
static void scan_dir(const string& dir, vector<string>& out, int depth) {
    if (depth > 16) return;
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        string n = e->d_name;
        if (n == "." || n == "..") continue;
        string p = (dir.empty() || dir.back() == '/') ? dir + n : dir + "/" + n;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) scan_dir(p, out, depth + 1);
        else if (S_ISREG(st.st_mode) && is_audio(n)) out.push_back(p);
    }
    closedir(d);
}
static bool nat_less(const string& a, const string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (isdigit((unsigned char)a[i]) && isdigit((unsigned char)b[j])) {
            size_t i2 = i, j2 = j;
            while (i2 < a.size() && isdigit((unsigned char)a[i2])) i2++;
            while (j2 < b.size() && isdigit((unsigned char)b[j2])) j2++;
            size_t ia = i, jb = j;
            while (ia < i2 - 1 && a[ia] == '0') ia++;
            while (jb < j2 - 1 && b[jb] == '0') jb++;
            string x = a.substr(ia, i2 - ia), y = b.substr(jb, j2 - jb);
            if (x.size() != y.size()) return x.size() < y.size();
            if (x != y) return x < y;
            i = i2;
            j = j2;
        } else {
            char x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[j]);
            if (x != y) return x < y;
            i++;
            j++;
        }
    }
    return (a.size() - i) < (b.size() - j);
}

// ============================================================================
//  colors, themes
// ============================================================================
static const uint32_t DEF = 0xFFFFFFFFu;   // terminal default color
static const uint32_t KEEP = 0xFFFFFFFEu;  // keep whatever is already in the cell
static uint32_t rgb(int r, int g, int b) { return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b; }
static void unpack(uint32_t c, int& r, int& g, int& b) { r = (c >> 16) & 255; g = (c >> 8) & 255; b = c & 255; }
static uint32_t mix(uint32_t a, uint32_t b, float t) {
    t = clampf(t, 0, 1);
    int ar, ag, ab, br, bg, bb;
    unpack(a, ar, ag, ab);
    unpack(b, br, bg, bb);
    return rgb((int)(ar + (br - ar) * t), (int)(ag + (bg - ag) * t), (int)(ab + (bb - ab) * t));
}
static uint32_t hsv(float h, float s, float v) {
    h = h - floorf(h);
    float r, g, b;
    int i = (int)(h * 6);
    float f = h * 6 - i, p = v * (1 - s), q = v * (1 - f * s), t = v * (1 - (1 - f) * s);
    switch (i % 6) {
        case 0: r = v; g = t; b = p; break;
        case 1: r = q; g = v; b = p; break;
        case 2: r = p; g = v; b = t; break;
        case 3: r = p; g = q; b = v; break;
        case 4: r = t; g = p; b = v; break;
        default: r = v; g = p; b = q; break;
    }
    return rgb((int)(r * 255), (int)(g * 255), (int)(b * 255));
}
struct Theme { const char* name; uint32_t c0, c1, c2; };
static const Theme THEMES[] = {
    {"Aurora", rgb(0, 229, 255), rgb(124, 92, 255), rgb(255, 64, 200)},
    {"Sunset", rgb(255, 214, 90), rgb(255, 96, 96), rgb(196, 64, 230)},
    {"Ocean", rgb(100, 255, 210), rgb(40, 160, 255), rgb(100, 80, 255)},
    {"Matrix", rgb(200, 255, 140), rgb(50, 225, 95), rgb(0, 150, 70)},
    {"Ember", rgb(255, 240, 150), rgb(255, 140, 40), rgb(220, 40, 40)},
    {"Mono", rgb(255, 255, 255), rgb(185, 185, 195), rgb(115, 115, 130)},
};
static const int NTHEMES = sizeof(THEMES) / sizeof(THEMES[0]);
static const uint32_t FG = rgb(232, 236, 245);
static const uint32_t DIMC = rgb(122, 128, 146);

// ============================================================================
//  canvas with row-diff output
// ============================================================================
enum { AT_BOLD = 1, AT_DIM = 2, AT_ITAL = 4, AT_UL = 8 };
struct Cell { char32_t ch = U' '; uint32_t fg = DEF, bg = DEF; uint8_t at = 0; };

static int to256(uint32_t c) {
    int r, g, b;
    unpack(c, r, g, b);
    if (abs(r - g) < 10 && abs(g - b) < 10) {
        int gray = (r + g + b) / 3;
        if (gray < 8) return 16;
        if (gray > 247) return 231;
        return 232 + (gray - 8) * 24 / 240;
    }
    auto q = [](int v) { return v < 48 ? 0 : v < 115 ? 1 : (v - 35) / 40; };
    return 16 + 36 * q(r) + 6 * q(g) + q(b);
}

struct Canvas {
    int w = 0, h = 0;
    bool truecolor = true;
    vector<Cell> c;
    vector<string> prev;

    void resize(int W, int H) { w = W; h = H; c.assign((size_t)W * H, Cell()); prev.assign(H, "\x01"); }
    void clear() { std::fill(c.begin(), c.end(), Cell()); }
    void set(int x, int y, char32_t ch, uint32_t fg, uint32_t bg = KEEP, uint8_t at = 0) {
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        Cell& k = c[(size_t)y * w + x];
        k.ch = ch; k.fg = fg; k.at = at;
        if (bg != KEEP) k.bg = bg;
    }
    void fill(int x, int y, int ww, int hh, uint32_t bg) {
        for (int j = y; j < y + hh; j++)
            for (int i = x; i < x + ww; i++)
                if (i >= 0 && j >= 0 && i < w && j < h) { Cell& k = c[(size_t)j * w + i]; k.ch = U' '; k.bg = bg; k.fg = DEF; k.at = 0; }
    }
    int text(int x, int y, const string& s, uint32_t fg, uint32_t bg = KEEP, uint8_t at = 0, int maxw = 1 << 20) {
        int used = 0;
        for (char32_t cp : u8_decode(s)) {
            int cw = cwidth(cp);
            if (cw == 0) continue;
            if (used + cw > maxw) break;
            set(x + used, y, cp, fg, bg, at);
            if (cw == 2) set(x + used + 1, y, 0, fg, bg, at);
            used += cw;
        }
        return used;
    }
    int gtext(int x, int y, const string& s, const Theme& th, float t0, float t1, uint8_t at = 0, int maxw = 1 << 20) {
        auto cps = u8_decode(s);
        int used = 0, n = (int)cps.size(), i = 0;
        for (char32_t cp : cps) {
            int cw = cwidth(cp);
            if (cw == 0) { i++; continue; }
            if (used + cw > maxw) break;
            float t = t0 + (t1 - t0) * (n > 1 ? (float)i / (n - 1) : 0.f);
            set(x + used, y, cp, grad(th, t), KEEP, at);
            if (cw == 2) set(x + used + 1, y, 0, grad(th, t), KEEP, at);
            used += cw;
            i++;
        }
        return used;
    }
    static uint32_t grad(const Theme& th, float t) {
        t = clampf(t, 0, 1);
        return t < 0.5f ? mix(th.c0, th.c1, t * 2) : mix(th.c1, th.c2, (t - 0.5f) * 2);
    }
    void box(int x, int y, int ww, int hh, uint32_t col, const string& title = "", uint32_t tcol = FG) {
        if (ww < 2 || hh < 2) return;
        set(x, y, U'╭', col); set(x + ww - 1, y, U'╮', col);
        set(x, y + hh - 1, U'╰', col); set(x + ww - 1, y + hh - 1, U'╯', col);
        for (int i = 1; i < ww - 1; i++) { set(x + i, y, U'─', col); set(x + i, y + hh - 1, U'─', col); }
        for (int j = 1; j < hh - 1; j++) { set(x, y + j, U'│', col); set(x + ww - 1, y + j, U'│', col); }
        if (!title.empty()) text(x + 2, y, " " + title + " ", tcol, KEEP, AT_BOLD, ww - 4);
    }

    void sgr_color(string& o, bool isfg, uint32_t col) const {
        char b[32];
        if (col == DEF) { o += isfg ? "\x1b[39m" : "\x1b[49m"; return; }
        int r, g, bl;
        unpack(col, r, g, bl);
        if (truecolor) snprintf(b, sizeof b, "\x1b[%d;2;%d;%d;%dm", isfg ? 38 : 48, r, g, bl);
        else snprintf(b, sizeof b, "\x1b[%d;5;%dm", isfg ? 38 : 48, to256(col));
        o += b;
    }
    string build_row(int y) const {
        string o = "\x1b[0m";
        uint32_t cf = DEF, cb = DEF;
        uint8_t ca = 0;
        for (int x = 0; x < w; x++) {
            const Cell& k = c[(size_t)y * w + x];
            if (k.ch == 0) continue;
            if (k.at != ca) {
                o += "\x1b[0m";
                if (k.at & AT_BOLD) o += "\x1b[1m";
                if (k.at & AT_DIM) o += "\x1b[2m";
                if (k.at & AT_ITAL) o += "\x1b[3m";
                if (k.at & AT_UL) o += "\x1b[4m";
                ca = k.at; cf = DEF; cb = DEF;
            }
            if (k.fg != cf) { sgr_color(o, true, k.fg); cf = k.fg; }
            if (k.bg != cb) { sgr_color(o, false, k.bg); cb = k.bg; }
            u8_append(o, k.ch);
        }
        o += "\x1b[0m";
        return o;
    }
    void flush(string& out) {
        string body;
        for (int y = 0; y < h; y++) {
            string row = build_row(y);
            if (row != prev[y]) {
                prev[y] = row;
                char pos[24];
                snprintf(pos, sizeof pos, "\x1b[%d;1H", y + 1);
                body += pos;
                body += row;
            }
        }
        if (!body.empty()) out += "\x1b[?2026h" + body + "\x1b[?2026l";
    }
};

// ============================================================================
//  mpv IPC backend
// ============================================================================
static bool which(const string& prog) {
    const char* p = getenv("PATH");
    string path = p ? p : "/usr/bin:/bin";
    size_t s = 0;
    while (s <= path.size()) {
        size_t e = path.find(':', s);
        if (e == string::npos) e = path.size();
        string cand = path.substr(s, e - s) + "/" + prog;
        if (access(cand.c_str(), X_OK) == 0) return true;
        s = e + 1;
    }
    return false;
}
static void child_detach_stdio(int out_fd) {
    int dn = open("/dev/null", O_RDWR);
    if (dn >= 0) {
        dup2(dn, 0);
        dup2(dn, 2);
        if (out_fd < 0) dup2(dn, 1);
        if (dn > 2) close(dn);
    }
    if (out_fd >= 0) dup2(out_fd, 1);
}

struct Mpv {
    pid_t pid = -1;
    int fd = -1;
    string buf, sock_path;
    bool dead = false;

    bool start(string& err) {
        const char* rt = getenv("XDG_RUNTIME_DIR");
        sock_path = string(rt && *rt ? rt : "/tmp") + "/discplayer-" + std::to_string(getpid()) + ".sock";
        unlink(sock_path.c_str());
        pid = fork();
        if (pid < 0) { err = "fork failed"; return false; }
        if (pid == 0) {
            setsid();
            child_detach_stdio(-1);
            string ipc = "--input-ipc-server=" + sock_path;
            vector<string> av = {"mpv", "--idle=yes", "--no-video", "--no-terminal", "--really-quiet", "--audio-display=no",
                                 "--no-input-default-bindings", "--no-config", ipc};
            if (const char* extra = getenv("DISCPLAYER_MPV_ARGS")) {  // e.g. "--audio-device=alsa/hw:1"
                string e = extra, cur;
                for (char ch : e + " ") { if (ch == ' ') { if (!cur.empty()) av.push_back(cur); cur.clear(); } else cur += ch; }
            }
            vector<char*> argvv;
            for (auto& a : av) argvv.push_back(&a[0]);
            argvv.push_back(nullptr);
            execvp("mpv", argvv.data());
            _exit(127);
        }
        for (int i = 0; i < 100; i++) {
            int st;
            if (waitpid(pid, &st, WNOHANG) == pid) { pid = -1; err = "mpv exited immediately"; return false; }
            int s = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_un a;
            memset(&a, 0, sizeof a);
            a.sun_family = AF_UNIX;
            strncpy(a.sun_path, sock_path.c_str(), sizeof(a.sun_path) - 1);
            if (connect(s, (sockaddr*)&a, sizeof a) == 0) {
                fd = s;
                fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
                return true;
            }
            close(s);
            usleep(50000);
        }
        err = "could not connect to mpv";
        return false;
    }
    void send(const string& json) {
        if (fd < 0 || dead) return;
        string m = json + "\n";
        size_t off = 0;
        int spins = 0;
        while (off < m.size()) {
            ssize_t r = ::send(fd, m.data() + off, m.size() - off, MSG_NOSIGNAL);
            if (r < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN && spins++ < 200) { usleep(1000); continue; }
                dead = true;
                return;
            }
            off += r;
        }
    }
    void cmd(std::initializer_list<string> args) {
        string m = "{\"command\":[";
        bool first = true;
        for (auto& a : args) { if (!first) m += ","; m += a; first = false; }
        send(m + "]}");
    }
    void read_msgs(vector<J>& out) {
        char tmp[16384];
        while (true) {
            ssize_t r = ::recv(fd, tmp, sizeof tmp, 0);
            if (r > 0) buf.append(tmp, r);
            else if (r == 0) { dead = true; break; }
            else { if (errno == EINTR) continue; if (errno != EAGAIN) dead = true; break; }
        }
        size_t nl;
        while ((nl = buf.find('\n')) != string::npos) {
            string line = buf.substr(0, nl);
            buf.erase(0, nl + 1);
            J j;
            size_t i = 0;
            if (jparse(line, i, j)) out.push_back(std::move(j));
        }
    }
    void stop() {
        if (fd >= 0) { close(fd); fd = -1; }
        if (pid > 0) {
            kill(pid, SIGTERM);
            for (int i = 0; i < 40; i++) {
                if (waitpid(pid, nullptr, WNOHANG) == pid) { pid = -1; break; }
                usleep(25000);
            }
            if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); pid = -1; }
        }
        if (!sock_path.empty()) unlink(sock_path.c_str());
    }
};

// ============================================================================
//  audio analysis feed: ffmpeg -> mono PCM -> ring buffer
// ============================================================================
static const int FFTN = 2048;
static const int SRATE = 44100;
struct Viz {
    pid_t pid = -1;
    int fd = -1;
    bool unavailable = false, disabled = false;
    uint64_t total = 0;
    vector<float> ring = vector<float>(8192, 0.f);
    size_t wr = 0;
    int carry = -1;

    bool active() const { return fd >= 0; }
    void stop() {
        if (fd >= 0) { close(fd); fd = -1; }
        if (pid > 0) { kill(pid, SIGKILL); waitpid(pid, nullptr, 0); pid = -1; }
        total = 0;
        carry = -1;
    }
    void start(const string& path, double pos) {
        stop();
        if (disabled || unavailable) return;
        int p[2];
        if (pipe2(p, O_CLOEXEC) != 0) return;
        pid = fork();
        if (pid < 0) { close(p[0]); close(p[1]); pid = -1; return; }
        if (pid == 0) {
            setsid();
            child_detach_stdio(p[1]);
            char ss[32];
            snprintf(ss, sizeof ss, "%.3f", pos < 0 ? 0 : pos);
            execlp("ffmpeg", "ffmpeg", "-nostdin", "-loglevel", "quiet", "-ss", ss, "-re", "-i", path.c_str(), "-vn",
                   "-ac", "1", "-ar", "44100", "-f", "s16le", "pipe:1", (char*)nullptr);
            _exit(127);
        }
        close(p[1]);
        fd = p[0];
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    }
    void pump() {
        if (fd < 0) return;
        unsigned char tmp[8192];
        while (true) {
            ssize_t r = read(fd, tmp, sizeof tmp);
            if (r > 0) {
                for (ssize_t i = 0; i < r; i++) {
                    if (carry < 0) { carry = tmp[i]; continue; }
                    int16_t s = (int16_t)(carry | (tmp[i] << 8));
                    carry = -1;
                    ring[wr] = s / 32768.f;
                    wr = (wr + 1) % ring.size();
                    total++;
                }
            } else if (r == 0) {
                int st = 0;
                waitpid(pid, &st, 0);
                if (WIFEXITED(st) && WEXITSTATUS(st) == 127) unavailable = true;
                close(fd);
                fd = -1;
                pid = -1;
                break;
            } else {
                break;  // EAGAIN
            }
        }
    }
};

static void fft(vector<std::complex<float>>& a) {
    size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -2.f * (float)M_PI / len;
        std::complex<float> wl(cosf(ang), sinf(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.f, 0.f);
            for (size_t j = 0; j < len / 2; j++) {
                auto u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// ============================================================================
//  terminal handling
// ============================================================================
static termios g_orig;
static bool g_raw = false;
static volatile sig_atomic_t g_winch = 0, g_quit = 0;
static void term_restore() {
    if (!g_raw) return;
    g_raw = false;
    const char* s = "\x1b[?1006l\x1b[?1002l\x1b[?25h\x1b[?1049l\x1b[0m";
    ssize_t r = write(1, s, strlen(s));
    (void)r;
    tcsetattr(0, TCSAFLUSH, &g_orig);
}
static void term_setup() {
    tcgetattr(0, &g_orig);
    termios t = g_orig;
    t.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    t.c_iflag &= ~(IXON | ICRNL | INLCR | BRKINT | ISTRIP);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    tcsetattr(0, TCSAFLUSH, &t);
    const char* s = "\x1b[?1049h\x1b[?25l\x1b[?1002h\x1b[?1006h\x1b[2J";
    ssize_t r = write(1, s, strlen(s));
    (void)r;
    g_raw = true;
    atexit(term_restore);
}
static void write_all(const string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t r = write(1, s.data() + off, s.size() - off);
        if (r < 0) { if (errno == EINTR || errno == EAGAIN) continue; return; }
        off += r;
    }
}

// special keys
enum { K_UP = 1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_ESC, K_ENTER, K_BS };

// ============================================================================
//  the application
// ============================================================================
struct Track {
    string path;
    Tags t;
    bool loaded = false;
};
struct Zone { int x, y, w, h, id; };
enum { Z_PREV = 1, Z_PLAY, Z_NEXT, Z_STOP, Z_SHUF, Z_REP, Z_VOL, Z_PROG, Z_LIST, Z_THEME };

struct App {
    Mpv mpv;
    Viz viz;
    Canvas cv;
    vector<Track> tr;
    vector<int> view;
    int sel = 0, top = 0;
    bool follow = true;
    int cur = -1;
    vector<int> hist;

    bool paused = false, muted = false, shuffle = false;
    int repeat = 0;  // 0 off, 1 all, 2 one
    int volume = 80;
    double pos = 0, dur = -1;
    double bitrate = 0, srate = 0;
    string chans, codec;
    int fails = 0;

    int theme = 0;
    float angle = 0, vel = 0;
    string filter;
    bool typing = false;
    string toast;
    double toast_until = 0;
    vector<Zone> zones;
    int drag_zone = 0;
    double last_click = 0;
    int last_click_idx = -1;

    // spectrum
    int nbars = 16;
    vector<float> lvl;
    vector<float> window = vector<float>(FFTN);
    string status_err;

    App() {
        for (int i = 0; i < FFTN; i++) window[i] = 0.5f - 0.5f * cosf(2.f * (float)M_PI * i / (FFTN - 1));
    }

    // ---------------------------------------------------------------- data
    void ensure(int i) {
        Track& t = tr[i];
        if (t.loaded) return;
        t.loaded = true;
        read_tags(t.path, t.t);
    }
    string label(int i) {
        ensure(i);
        const Tags& t = tr[i].t;
        return t.artist.empty() ? t.title : t.artist + " – " + t.title;
    }
    void say(const string& s, double secs = 2.0) { toast = s; toast_until = mono_now() + secs; }

    void rebuild_view() {
        int keep = (sel >= 0 && sel < (int)view.size()) ? view[sel] : -1;
        view.clear();
        string f = lower_ascii(filter);
        for (int i = 0; i < (int)tr.size(); i++) {
            if (f.empty()) { view.push_back(i); continue; }
            string hay = lower_ascii(label(i) + " " + tr[i].t.album + " " + stem_of(tr[i].path));
            if (hay.find(f) != string::npos) view.push_back(i);
        }
        sel = 0;
        for (int k = 0; k < (int)view.size(); k++) if (view[k] == keep) { sel = k; break; }
        follow = true;
    }

    // ------------------------------------------------------------ playback
    void play(int i, bool push = true) {
        if (i < 0 || i >= (int)tr.size()) return;
        ensure(i);
        if (push && cur >= 0 && cur != i) hist.push_back(cur);
        cur = i;
        paused = false;
        pos = 0;
        dur = tr[i].t.length;
        bitrate = srate = 0;
        chans.clear();
        codec.clear();
        for (int k = 0; k < (int)view.size(); k++) if (view[k] == i) { sel = k; follow = true; break; }
        mpv.cmd({jq("loadfile"), jq(tr[i].path), jq("replace")});
        mpv.cmd({jq("set_property"), jq("pause"), "false"});
        viz.start(tr[i].path, 0);
    }
    void stop() {
        mpv.cmd({jq("stop")});
        viz.stop();
        cur = -1;
        paused = false;
        pos = 0;
        dur = -1;
    }
    void toggle_pause() {
        if (cur < 0) {
            if (!view.empty()) play(view[sel]);
            return;
        }
        paused = !paused;
        mpv.cmd({jq("set_property"), jq("pause"), paused ? "true" : "false"});
        if (paused) viz.stop();
        else viz.start(tr[cur].path, pos);
    }
    int pick_next(bool manual) {
        int n = (int)tr.size();
        if (n == 0) return -1;
        if (shuffle && n > 1) {
            int r;
            do { r = rand() % n; } while (r == cur);
            return r;
        }
        if (cur < 0) return view.empty() ? 0 : view[sel];
        int nx = cur + 1;
        if (nx >= n) return (manual || repeat == 1) ? 0 : -1;
        return nx;
    }
    void next() { int i = pick_next(true); if (i >= 0) play(i); }
    void prev() {
        if (cur < 0) return;
        if (pos > 3) { seek_abs(0); return; }
        if (!hist.empty()) { int i = hist.back(); hist.pop_back(); play(i, false); }
        else if (cur > 0) play(cur - 1, false);
        else seek_abs(0);
    }
    void on_eof() {
        if (cur < 0) return;
        if (repeat == 2) { play(cur, false); return; }
        int nx = pick_next(false);
        if (nx < 0) stop();
        else play(nx);
    }
    void seek_abs(double t) {
        if (cur < 0) return;
        if (t < 0) t = 0;
        if (dur > 0 && t > dur - 0.2) t = dur - 0.2;
        mpv.cmd({jq("seek"), jnum(t), jq("absolute")});
        pos = t;
        if (!paused) viz.start(tr[cur].path, t);
    }
    void seek_rel(double d) { seek_abs(pos + d); }
    void set_volume(int v) {
        volume = std::max(0, std::min(130, v));
        mpv.cmd({jq("set_property"), jq("volume"), std::to_string(volume)});
    }
    void toggle_mute() {
        muted = !muted;
        mpv.cmd({jq("set_property"), jq("mute"), muted ? "true" : "false"});
    }

    // ------------------------------------------------------------ mpv events
    void apply_meta(const J& d) {
        if (cur < 0 || d.t != J::Obj) return;
        Tags& t = tr[cur].t;
        for (auto& kv : d.o) {
            if (kv.second.t != J::Str || kv.second.s.empty()) continue;
            string k = lower_ascii(kv.first);
            const string& v = kv.second.s;
            if (k == "title") t.title = v;
            else if (k == "artist") t.artist = v;
            else if (k == "album") t.album = v;
            else if (k == "date" || k == "year") t.year = v.substr(0, 4);
            else if (k == "genre") t.genre = v;
            else if (k == "track" || k == "tracknumber") t.track = v.substr(0, v.find('/'));
        }
    }
    void on_msg(const J& m) {
        const J* ev = m.get("event");
        if (!ev || ev->t != J::Str) return;
        const string& e = ev->s;
        if (e == "property-change") {
            const J* nm = m.get("name");
            const J* da = m.get("data");
            if (!nm) return;
            const string& n = nm->s;
            bool num = da && da->t == J::Num;
            if (n == "time-pos") { if (num) pos = da->n; }
            else if (n == "duration") { if (num) { dur = da->n; if (cur >= 0) tr[cur].t.length = da->n; } }
            else if (n == "pause") { if (da && da->t == J::Bool && cur >= 0) paused = da->b; }
            else if (n == "mute") { if (da && da->t == J::Bool) muted = da->b; }
            else if (n == "volume") { if (num) volume = (int)lround(da->n); }
            else if (n == "audio-bitrate") { bitrate = num ? da->n : 0; }
            else if (n == "audio-codec-name") { codec = (da && da->t == J::Str) ? da->s : ""; }
            else if (n == "metadata") { if (da) apply_meta(*da); }
            else if (n == "audio-params") {
                if (da && da->t == J::Obj) {
                    const J* sr = da->get("samplerate");
                    const J* hc = da->get("hr-channels");
                    srate = (sr && sr->t == J::Num) ? sr->n : 0;
                    chans = (hc && hc->t == J::Str) ? hc->s : "";
                }
            }
        } else if (e == "file-loaded") {
            fails = 0;
        } else if (e == "end-file") {
            const J* r = m.get("reason");
            if (!r) return;
            if (r->s == "eof") on_eof();
            else if (r->s == "error") {
                say("Can't play: " + (cur >= 0 ? stem_of(tr[cur].path) : string("file")), 3);
                if (++fails >= 5) stop();
                else on_eof();
            }
        }
    }

    // ------------------------------------------------------------ per-frame
    void tick(double dt) {
        float target = (cur >= 0 && !paused) ? 3.2f : 0.f;
        vel += (target - vel) * std::min(1.0, dt * 3.5);
        angle = fmodf(angle + vel * (float)dt, 2.f * (float)M_PI);

        if ((int)lvl.size() != nbars) lvl.assign(nbars, 0.f);
        vector<float> tg(nbars, 0.f);
        if (viz.active() && viz.total >= (uint64_t)FFTN && cur >= 0 && !paused) {
            vector<std::complex<float>> a(FFTN);
            size_t rs = viz.ring.size();
            for (int k = 0; k < FFTN; k++) a[k] = {viz.ring[(viz.wr + rs - FFTN + k) % rs] * window[k], 0.f};
            fft(a);
            vector<float> mag(FFTN / 2);
            for (int k = 0; k < FFTN / 2; k++) mag[k] = std::abs(a[k]) / (FFTN / 4.f);
            const double FMIN = 45, FMAX = 16000;
            for (int b = 0; b < nbars; b++) {
                double f0 = FMIN * pow(FMAX / FMIN, (double)b / nbars);
                double f1 = FMIN * pow(FMAX / FMIN, (double)(b + 1) / nbars);
                double fc = sqrt(f0 * f1);
                double b0 = f0 * FFTN / SRATE, b1 = f1 * FFTN / SRATE;
                float m = 0;
                if (b1 - b0 < 1.0) {
                    double bc = fc * FFTN / SRATE;
                    int k0 = (int)bc;
                    double fr = bc - k0;
                    m = (float)(mag[k0] * (1 - fr) + mag[k0 + 1] * fr);
                } else {
                    for (int k = (int)ceil(b0); k <= (int)floor(b1) && k < FFTN / 2; k++) m = std::max(m, mag[k]);
                }
                float db = 20.f * log10f(m + 1e-7f) + 3.f * log2f((float)(fc / 200.0));
                tg[b] = clampf((db + 72.f) / 60.f, 0.f, 1.f);
            }
        }
        for (int b = 0; b < nbars; b++) {
            if (tg[b] > lvl[b]) lvl[b] += (tg[b] - lvl[b]) * 0.65f;
            else lvl[b] = std::max(tg[b], lvl[b] - (float)dt * 1.5f);
        }
    }

    // ------------------------------------------------------------- drawing
    uint32_t pixel(int ix, int iy, int cols, int rows) const {
        float dx = ix + 0.5f - cols / 2.f, dy = iy + 0.5f - rows;
        float R = std::min((float)cols, 2.f * rows) / 2.f - 0.2f;
        float r = sqrtf(dx * dx + dy * dy);
        if (r > R) return DEF;
        float u = r / R, th = atan2f(dy, dx);
        if (u < 0.13f) return DEF;  // spindle hole
        if (u < 0.30f) {            // hub
            float sh = 0.62f + 0.28f * cosf(2 * (th - angle));
            return rgb((int)(188 * sh), (int)(192 * sh), (int)(204 * sh));
        }
        if (u < 0.36f) return rgb(26, 28, 36);  // inner clear ring
        if (u > 0.965f) return rgb(205, 210, 222);  // rim
        float a = th - angle;
        float glint = powf(std::max(0.f, cosf(2 * a)), 2.4f);
        float glint2 = powf(std::max(0.f, cosf(a * 2 + 1.7f)), 6.f) * 0.35f;
        float hue = fmodf(a / (2.f * (float)M_PI) * 2.f + u * 0.35f + 1.f, 1.f);
        float v = 0.13f + 0.80f * glint + glint2;
        v *= 0.93f + 0.07f * sinf(u * 90.f);
        return hsv(hue, 0.30f + 0.45f * glint, clampf(v, 0, 1));
    }
    void draw_disc(int x0, int y0, int cols, int rows) {
        for (int cy = 0; cy < rows; cy++)
            for (int cx = 0; cx < cols; cx++) {
                uint32_t t = pixel(cx, 2 * cy, cols, rows), b = pixel(cx, 2 * cy + 1, cols, rows);
                if (t == DEF && b == DEF) continue;
                if (b == DEF) cv.set(x0 + cx, y0 + cy, U'▀', t);
                else if (t == DEF) cv.set(x0 + cx, y0 + cy, U'▄', b);
                else cv.set(x0 + cx, y0 + cy, U'▀', t, b);
            }
    }
    void zone(int x, int y, int w, int h, int id) { zones.push_back({x, y, w, h, id}); }

    void draw() {
        const Theme& th = THEMES[theme];
        uint32_t border = mix(th.c1, rgb(36, 38, 52), 0.62f);
        uint32_t selbg = mix(th.c1, rgb(8, 8, 14), 0.80f);
        int W = cv.w, H = cv.h;
        cv.clear();
        zones.clear();
        if (W < 72 || H < 22) {
            cv.text(1, 1, "Terminal too small", th.c0, KEEP, AT_BOLD);
            cv.text(1, 2, "need at least 72x22, have " + std::to_string(W) + "x" + std::to_string(H), DIMC);
            return;
        }
        double now = mono_now();
        int n = (int)tr.size();

        // ---- header
        cv.gtext(1, 0, "♫  D I S C P L A Y E R", th, 0, 1, AT_BOLD);
        string right;
        if (now < toast_until) right = toast;
        else {
            char tb[16];
            time_t tt = time(nullptr);
            tm lt;
            localtime_r(&tt, &lt);
            strftime(tb, sizeof tb, "%H:%M", &lt);
            right = std::to_string(n) + " tracks · " + THEMES[theme].name + " · " + tb;
        }
        int rw = u8_width(right);
        for (int lx = 25; lx < W - rw - 3; lx++) cv.set(lx, 0, U'─', mix(border, rgb(14, 15, 22), 0.35f));
        cv.text(W - rw - 1, 0, right, now < toast_until ? th.c0 : DIMC, KEEP, now < toast_until ? AT_BOLD : 0);
        zone(W - rw - 1, 0, rw + 1, 1, Z_THEME);

        // ---- top: disc + now playing
        const int TH = 13, DW = 26;
        cv.box(0, 1, DW, TH, border);
        draw_disc(2, 2, 22, 11);
        int ix = DW, iw = W - DW;
        cv.box(ix, 1, iw, TH, border, "NOW PLAYING", th.c0);
        int tx = ix + 2, tw = iw - 4;

        int idx = cur >= 0 ? cur : (view.empty() ? -1 : view[sel]);
        if (idx >= 0) ensure(idx);
        const Tags* tg = idx >= 0 ? &tr[idx].t : nullptr;

        string st;
        uint32_t stc;
        if (cur < 0) { st = "■ STOPPED"; stc = DIMC; }
        else if (paused) { st = "❚❚ PAUSED"; stc = rgb(255, 200, 80); }
        else { st = "▶ PLAYING"; stc = th.c0; }
        cv.text(tx, 2, st, stc, KEEP, AT_BOLD);
        if (cur < 0 && tg) cv.text(tx + 11, 2, "(selected)", DIMC);
        if (idx >= 0) {
            string ct = "track " + std::to_string(idx + 1) + " / " + std::to_string(n);
            cv.text(tx + tw - u8_width(ct), 2, ct, DIMC);
        }
        if (tg) {
            cv.gtext(tx, 3, tg->title, th, 0.0f, 1.0f, AT_BOLD, tw);
            cv.text(tx, 4, tg->artist.empty() ? "Unknown artist" : tg->artist, tg->artist.empty() ? DIMC : FG, KEEP, 0, tw);
            string al = tg->album;
            for (const string& x : {tg->year, tg->genre})
                if (!x.empty()) al += (al.empty() ? "" : "  ·  ") + x;
            cv.text(tx, 5, al.empty() ? "Unknown album" : al, DIMC, KEEP, AT_ITAL, tw);
            string tech;
            auto add = [&](const string& s) { if (!s.empty()) tech += (tech.empty() ? "" : "  ·  ") + s; };
            string cname = cur >= 0 ? codec : string();
            for (auto& ch : cname) ch = toupper((unsigned char)ch);
            if (cname.empty()) { cname = ext_of(tr[idx].path); for (auto& ch : cname) ch = toupper((unsigned char)ch); }
            add(cname);
            char b[48];
            if (cur >= 0 && srate > 0) { snprintf(b, sizeof b, "%g kHz", srate / 1000.0); add(b); }
            if (cur >= 0) add(chans);
            if (cur >= 0 && bitrate > 0) { snprintf(b, sizeof b, "%d kbps", (int)(bitrate / 1000.0 + 0.5)); add(b); }
            cv.text(tx, 6, tech, th.c1, KEEP, AT_BOLD, tw);
        }

        // ---- spectrum (rows 8..12)
        int sy = 8, srows = 5;
        nbars = std::max(8, std::min(48, tw / 3));
        int span = nbars * 3 - 1, sx = tx + (tw - span) / 2;
        static const char32_t blocks[] = {U'▁', U'▂', U'▃', U'▄', U'▅', U'▆', U'▇', U'█'};
        for (int b = 0; b < nbars && b < (int)lvl.size(); b++) {
            float level = lvl[b] * srows * 8;
            for (int r = 0; r < srows; r++) {
                int fillv = (int)clampf(level - r * 8, 0, 8);
                int y = sy + srows - 1 - r;
                float gt = ((float)r / srows) * 0.75f + ((float)b / nbars) * 0.25f;
                uint32_t col = Canvas::grad(th, gt);
                if (fillv >= 1) {
                    char32_t ch = blocks[fillv - 1];
                    cv.set(sx + b * 3, y, ch, col);
                    cv.set(sx + b * 3 + 1, y, ch, col);
                } else if (r == 0) {
                    cv.set(sx + b * 3, y, U'▁', border);
                    cv.set(sx + b * 3 + 1, y, U'▁', border);
                }
            }
        }
        if (viz.unavailable) cv.text(tx, sy + 2, "install ffmpeg for the live spectrum", DIMC, KEEP, AT_ITAL, tw);
        else if (viz.disabled) cv.text(tx, sy + 2, "spectrum disabled (--no-viz)", DIMC, KEEP, AT_ITAL, tw);

        // ---- progress row
        int py = TH + 1;
        double shown = cur >= 0 ? pos : 0;
        double total = dur > 0 ? dur : (tg ? tg->length : -1);
        string tl = fmt_time(shown), trr = fmt_time(total);
        cv.text(1, py, tl, FG, KEEP, AT_BOLD);
        int bx = 1 + u8_width(tl) + 1, ex = W - 1 - u8_width(trr);
        int bw = ex - 1 - bx;
        cv.text(ex, py, trr, DIMC);
        float frac = (total > 0) ? clampf((float)(shown / total), 0, 1) : 0;
        int fl = (int)(frac * bw);
        for (int i = 0; i < bw; i++) {
            if (i < fl) cv.set(bx + i, py, U'━', Canvas::grad(th, (float)i / std::max(1, bw)));
            else if (i == fl && cur >= 0) cv.set(bx + i, py, U'●', FG, KEEP, AT_BOLD);
            else cv.set(bx + i, py, U'─', border);
        }
        zone(bx, py, bw, 1, Z_PROG);
        prog_x = bx; prog_w = bw;

        // ---- controls row
        int cy = TH + 2, x = 1;
        auto button = [&](const string& s, int id, uint32_t col) {
            int w = u8_width(s) + 2;
            cv.fill(x, cy, w, 1, selbg);
            cv.text(x + 1, cy, s, col, KEEP, AT_BOLD);
            zone(x, cy, w, 1, id);
            x += w + 1;
        };
        button("◀◀", Z_PREV, FG);
        button(cur >= 0 && !paused ? "❚❚" : "▶", Z_PLAY, th.c0);
        button("▶▶", Z_NEXT, FG);
        button("■", Z_STOP, FG);
        x += 2;
        string sh = string("⇄ shuffle ") + (shuffle ? "on" : "off");
        cv.text(x, cy, sh, shuffle ? th.c0 : DIMC, KEEP, shuffle ? AT_BOLD : 0);
        zone(x, cy, u8_width(sh), 1, Z_SHUF);
        x += u8_width(sh) + 3;
        const char* rn[] = {"off", "all", "one"};
        string rp = string("↻ repeat ") + rn[repeat];
        cv.text(x, cy, rp, repeat ? th.c0 : DIMC, KEEP, repeat ? AT_BOLD : 0);
        zone(x, cy, u8_width(rp), 1, Z_REP);
        // volume (right aligned)
        int vbw = 14;
        string vt = muted ? "MUTE" : std::to_string(volume) + "%";
        int vx = W - 2 - (int)vt.size() - 1 - vbw - 5;
        cv.text(vx, cy, "VOL ", DIMC);
        int vfill = (int)lround(std::min(volume, 100) / 100.0 * vbw);
        for (int i = 0; i < vbw; i++) {
            if (!muted && i < vfill) cv.set(vx + 4 + i, cy, U'━', Canvas::grad(th, (float)i / vbw));
            else cv.set(vx + 4 + i, cy, U'─', border);
        }
        cv.text(vx + 4 + vbw + 1, cy, vt, muted ? rgb(255, 110, 110) : FG);
        zone(vx + 4, cy, vbw, 1, Z_VOL);
        vol_x = vx + 4; vol_w = vbw;

        // ---- playlist
        int ly = TH + 3, lh = H - ly - 1;
        string ptitle = "PLAYLIST";
        if (!filter.empty()) ptitle += "  /" + filter + "  (" + std::to_string(view.size()) + " match)";
        cv.box(0, ly, W, lh, border, ptitle, th.c0);
        int rows = lh - 2, vn = (int)view.size();
        if (follow) {
            if (sel < top) top = sel;
            if (sel >= top + rows) top = sel - rows + 1;
            follow = false;
        }
        top = std::max(0, std::min(top, std::max(0, vn - rows)));
        list_y = ly + 1; list_rows = rows;
        zone(1, ly + 1, W - 2, rows, Z_LIST);
        for (int r = 0; r < rows; r++) {
            int k = top + r;
            if (k >= vn) break;
            int i = view[k];
            ensure(i);
            int y = ly + 1 + r;
            bool isel = (k == sel), iplay = (i == cur);
            if (isel) cv.fill(1, y, W - 2, 1, selbg);
            uint32_t base = iplay ? th.c0 : FG;
            uint8_t at = (iplay || isel) ? AT_BOLD : 0;
            cv.text(2, y, iplay ? (paused ? "❚" : "▶") : (isel ? "›" : " "), iplay ? th.c0 : th.c1, KEEP, AT_BOLD);
            char nb[16];
            snprintf(nb, sizeof nb, "%4d", i + 1);
            cv.text(4, y, nb, iplay ? th.c0 : DIMC, KEEP, 0);
            string dur_s = tr[i].t.length > 0 ? fmt_time(tr[i].t.length) : "";
            int maxw = W - 11 - 8;
            const Tags& t = tr[i].t;
            int used = 0;
            if (!t.artist.empty()) {
                used += cv.text(10, y, t.artist, iplay ? th.c1 : DIMC, KEEP, at, maxw);
                used += cv.text(10 + used, y, " – ", DIMC, KEEP, 0, maxw - used);
            }
            cv.text(10 + used, y, t.title, base, KEEP, at, maxw - used);
            if (!dur_s.empty()) cv.text(W - 2 - (int)dur_s.size() - 1, y, dur_s, DIMC);
        }
        if (vn > rows) {  // scrollbar
            int th_h = std::max(1, rows * rows / vn);
            int th_y = (int)((long)(rows - th_h) * top / std::max(1, vn - rows));
            for (int r = 0; r < rows; r++)
                cv.set(W - 1, ly + 1 + r, (r >= th_y && r < th_y + th_h) ? U'┃' : U'│', (r >= th_y && r < th_y + th_h) ? th.c1 : border);
        }
        if (vn == 0) cv.text(3, ly + 1, filter.empty() ? "no tracks" : "no matches – Esc clears the filter", DIMC, KEEP, AT_ITAL);

        // ---- footer
        int fy = H - 1;
        if (typing) {
            cv.text(1, fy, "/", th.c0, KEEP, AT_BOLD);
            int u = cv.text(3, fy, filter, FG);
            cv.set(3 + u, fy, U'▌', th.c0);
            cv.text(5 + u, fy, "Enter keep · Esc clear", DIMC);
        } else {
            static const char* hints[][2] = {{"space", "play/pause"}, {"n/p", "next/prev"}, {"←→", "seek"}, {"↑↓", "select"},
                                             {"⏎", "play"}, {"+/-", "volume"}, {"s", "shuffle"}, {"r", "repeat"},
                                             {"/", "search"}, {"t", "theme"}, {"q", "quit"}};
            int fx = 1;
            for (auto& h : hints) {
                int need = u8_width(h[0]) + 1 + u8_width(h[1]) + 3;
                if (fx + need > W) break;
                fx += cv.text(fx, fy, h[0], th.c0, KEEP, AT_BOLD) + 1;
                fx += cv.text(fx, fy, h[1], DIMC) + 3;
            }
        }
    }
    int prog_x = 0, prog_w = 1, vol_x = 0, vol_w = 1, list_y = 0, list_rows = 0;

    // --------------------------------------------------------------- input
    void move_sel(int d) {
        if (view.empty()) return;
        sel = std::max(0, std::min((int)view.size() - 1, sel + d));
        follow = true;
    }
    void cycle_theme() { theme = (theme + 1) % NTHEMES; say(string("Theme: ") + THEMES[theme].name); }

    bool key(int k) {  // returns false to quit
        if (typing) {
            if (k == K_ENTER) typing = false;
            else if (k == K_ESC) { typing = false; filter.clear(); rebuild_view(); }
            else if (k == K_BS) {
                while (!filter.empty() && ((unsigned char)filter.back() & 0xC0) == 0x80) filter.pop_back();
                if (!filter.empty()) filter.pop_back();
                rebuild_view();
            } else if (k == K_UP) move_sel(-1);
            else if (k == K_DOWN) move_sel(1);
            else if (k >= 32 && k < 0x110000) { u8_append(filter, k); rebuild_view(); }
            return true;
        }
        switch (k) {
            case 'q': case 3: return false;
            case ' ': toggle_pause(); break;
            case 'n': case 'N': next(); break;
            case 'p': case 'P': prev(); break;
            case 'x': stop(); break;
            case K_RIGHT: case 'l': seek_rel(5); break;
            case K_LEFT: case 'h': seek_rel(-5); break;
            case ']': seek_rel(30); break;
            case '[': seek_rel(-30); break;
            case K_DOWN: case 'j': move_sel(1); break;
            case K_UP: case 'k': move_sel(-1); break;
            case K_PGDN: move_sel(std::max(1, list_rows - 1)); break;
            case K_PGUP: move_sel(-std::max(1, list_rows - 1)); break;
            case K_HOME: case 'g': sel = 0; follow = true; break;
            case K_END: case 'G': sel = std::max(0, (int)view.size() - 1); follow = true; break;
            case K_ENTER: if (!view.empty()) play(view[sel]); break;
            case '+': case '=': set_volume(volume + 5); break;
            case '-': case '_': set_volume(volume - 5); break;
            case 'm': toggle_mute(); break;
            case 's': shuffle = !shuffle; say(shuffle ? "Shuffle on" : "Shuffle off"); break;
            case 'r': repeat = (repeat + 1) % 3; { const char* rn[] = {"off", "all", "one"}; say(string("Repeat ") + rn[repeat]); } break;
            case 't': cycle_theme(); break;
            case 'c':
                if (cur >= 0) for (int i = 0; i < (int)view.size(); i++) if (view[i] == cur) { sel = i; follow = true; }
                break;
            case '/': typing = true; break;
            case K_ESC: if (!filter.empty()) { filter.clear(); rebuild_view(); } break;
        }
        return true;
    }
    void mouse(int b, int x, int y, bool press, bool release) {
        x -= 1; y -= 1;
        int base = b & ~(4 | 8 | 16);
        if (base == 64 || base == 65) {
            top += base == 64 ? -3 : 3;
            int vn = (int)view.size();
            top = std::max(0, std::min(top, std::max(0, vn - list_rows)));
            return;
        }
        if (release) { drag_zone = 0; return; }
        bool drag = (base & 32) != 0;
        if ((base & 3) != 0 && !drag) return;  // only left button
        int id = 0;
        if (drag) id = drag_zone;
        else for (auto& z : zones) if (x >= z.x && x < z.x + z.w && y >= z.y && y < z.y + z.h) { id = z.id; break; }
        if (!press && !drag) return;
        if (!drag) drag_zone = id;
        switch (id) {
            case Z_PREV: if (!drag) prev(); break;
            case Z_PLAY: if (!drag) toggle_pause(); break;
            case Z_NEXT: if (!drag) next(); break;
            case Z_STOP: if (!drag) stop(); break;
            case Z_SHUF: if (!drag) key('s'); break;
            case Z_REP: if (!drag) key('r'); break;
            case Z_THEME: if (!drag) cycle_theme(); break;
            case Z_VOL: set_volume((int)lround((double)(x - vol_x + 1) / vol_w * 100)); break;
            case Z_PROG:
                if (!drag && cur >= 0 && dur > 0) seek_abs(dur * clampf((float)(x - prog_x) / std::max(1, prog_w - 1), 0, 1));
                break;
            case Z_LIST:
                if (!drag) {
                    int k = top + (y - list_y);
                    if (k >= 0 && k < (int)view.size()) {
                        double now = mono_now();
                        if (k == last_click_idx && now - last_click < 0.45) play(view[k]);
                        sel = k;
                        last_click = now;
                        last_click_idx = k;
                    }
                }
                break;
        }
    }

    void parse_input(const string& in) {
        size_t i = 0, n = in.size();
        while (i < n) {
            unsigned char c = in[i];
            if (c == 0x1b) {
                if (i + 1 >= n) { if (!key(K_ESC)) quit = true; i++; continue; }
                if (in[i + 1] == '[') {
                    size_t j = i + 2;
                    if (j < n && in[j] == '<') {  // SGR mouse
                        j++;
                        int v[3] = {0, 0, 0}, vi = 0;
                        while (j < n && (isdigit((unsigned char)in[j]) || in[j] == ';')) {
                            if (in[j] == ';') { if (vi < 2) vi++; }
                            else v[vi] = v[vi] * 10 + (in[j] - '0');
                            j++;
                        }
                        if (j < n) {
                            mouse(v[0], v[1], v[2], in[j] == 'M', in[j] == 'm');
                            j++;
                        }
                        i = j;
                        continue;
                    }
                    string params;
                    while (j < n && in[j] >= 0x30 && in[j] <= 0x3F) params += in[j++];
                    while (j < n && in[j] >= 0x20 && in[j] <= 0x2F) j++;
                    char f = j < n ? in[j] : 0;
                    int k = 0;
                    switch (f) {
                        case 'A': k = K_UP; break;
                        case 'B': k = K_DOWN; break;
                        case 'C': k = K_RIGHT; break;
                        case 'D': k = K_LEFT; break;
                        case 'H': k = K_HOME; break;
                        case 'F': k = K_END; break;
                        case '~':
                            if (params == "5") k = K_PGUP;
                            else if (params == "6") k = K_PGDN;
                            else if (params == "1" || params == "7") k = K_HOME;
                            else if (params == "4" || params == "8") k = K_END;
                            else if (params == "3") k = K_DEL;
                            break;
                    }
                    if (k && !key(k)) quit = true;
                    i = j + 1;
                    continue;
                }
                if (in[i + 1] == 'O' && i + 2 < n) {
                    int k = 0;
                    switch (in[i + 2]) { case 'A': k = K_UP; break; case 'B': k = K_DOWN; break; case 'C': k = K_RIGHT; break; case 'D': k = K_LEFT; break; case 'H': k = K_HOME; break; case 'F': k = K_END; break; }
                    if (k && !key(k)) quit = true;
                    i += 3;
                    continue;
                }
                if (!key(K_ESC)) quit = true;
                i++;
                continue;
            }
            if (c >= 0x80) {  // utf-8 text (search box)
                size_t len = (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
                auto cps = u8_decode(in.substr(i, len));
                if (!cps.empty() && !key((int)cps[0])) quit = true;
                i += len;
                continue;
            }
            int k = c;
            if (c == 13 || c == 10) k = K_ENTER;
            else if (c == 127 || c == 8) k = K_BS;
            if (!key(k)) quit = true;
            i++;
        }
    }
    bool quit = false;
};

// ============================================================================
//  main
// ============================================================================
static void usage() {
    puts("discplayer " "1.0" " - terminal music player with a spinning disc and live spectrum\n\n"
         "usage: discplayer [options] [FILE_OR_DIR ...]      (default: ~/Music)\n\n"
         "options:\n  --no-viz     disable the ffmpeg-based spectrum analyzer\n  -h, --help   this text\n"
         "  -v, --version\n\n"
         "env:   DISCPLAYER_MPV_ARGS  extra flags for mpv, e.g. \"--audio-device=alsa/hw:1\"\n\n"
         "keys:  space play/pause   n/p next/prev   <-/-> seek 5s   [ ] seek 30s\n"
         "       up/down select     enter play      +/- volume      m mute\n"
         "       s shuffle          r repeat        / search        t theme\n"
         "       c jump to playing  x stop          q quit\n"
         "mouse: click buttons, progress bar, volume bar; double-click a track; wheel scrolls");
}

int main(int argc, char** argv) {
    vector<string> inputs;
    bool no_viz = false;
    for (int i = 1; i < argc; i++) {
        string a = argv[i];
        if (a == "-h" || a == "--help") { usage(); return 0; }
        if (a == "-v" || a == "--version") { printf("discplayer %s\n", VERSION); return 0; }
        if (a == "--no-viz") { no_viz = true; continue; }
        inputs.push_back(a);
    }
    if (inputs.empty()) {
        const char* home = getenv("HOME");
        inputs.push_back(string(home ? home : ".") + "/Music");
    }
    if (!which("mpv")) {
        fprintf(stderr, "discplayer: mpv not found.\n  Arch:  sudo pacman -S mpv\n");
        return 1;
    }

    vector<string> files;
    for (auto& in : inputs) {
        char rp[4096];
        string p = realpath(in.c_str(), rp) ? string(rp) : in;
        struct stat st;
        if (stat(p.c_str(), &st) != 0) { fprintf(stderr, "discplayer: cannot open %s\n", in.c_str()); continue; }
        if (S_ISDIR(st.st_mode)) scan_dir(p, files, 0);
        else if (is_audio(p)) files.push_back(p);
    }
    std::sort(files.begin(), files.end(), nat_less);
    files.erase(std::unique(files.begin(), files.end()), files.end());
    if (files.empty()) {
        fprintf(stderr, "discplayer: no audio files found in:");
        for (auto& i : inputs) fprintf(stderr, " %s", i.c_str());
        fprintf(stderr, "\n");
        return 1;
    }

    signal(SIGPIPE, SIG_IGN);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = [](int) { g_winch = 1; };
    sigaction(SIGWINCH, &sa, nullptr);
    sa.sa_handler = [](int) { g_quit = 1; };
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    sigaction(SIGHUP, &sa, nullptr);

    App app;
    app.viz.disabled = no_viz || !which("ffmpeg");
    if (!no_viz && app.viz.disabled) app.viz.unavailable = true;
    for (auto& f : files) { Track t; t.path = f; app.tr.push_back(std::move(t)); }

    string err;
    if (!app.mpv.start(err)) {
        app.mpv.stop();
        fprintf(stderr, "discplayer: %s\n", err.c_str());
        return 1;
    }
    const char* obs[] = {"time-pos", "duration", "pause", "volume", "mute", "metadata", "audio-bitrate", "audio-params", "audio-codec-name"};
    for (int i = 0; i < 9; i++) app.mpv.cmd({jq("observe_property"), std::to_string(i + 1), jq(obs[i])});
    app.set_volume(80);

    const char* ct = getenv("COLORTERM");
    const char* tm = getenv("TERM");
    string cts = ct ? ct : "", tms = tm ? tm : "";
    app.cv.truecolor = cts.find("truecolor") != string::npos || cts.find("24bit") != string::npos ||
                       tms.find("direct") != string::npos || tms.find("kitty") != string::npos ||
                       tms.find("alacritty") != string::npos || tms.find("foot") != string::npos ||
                       tms.find("wezterm") != string::npos;
    srand(time(nullptr));

    app.rebuild_view();
    term_setup();
    app.play(0);

    double last = mono_now(), next_frame = last;
    string out;
    while (!app.quit && !g_quit) {
        if (app.mpv.dead) { app.say("mpv stopped unexpectedly", 5); break; }
        double now = mono_now();
        int timeout = (int)std::max(0.0, (next_frame - now) * 1000.0);
        pollfd fds[3];
        int nf = 0;
        fds[nf++] = {0, POLLIN, 0};
        fds[nf++] = {app.mpv.fd, POLLIN, 0};
        int vidx = -1;
        if (app.viz.fd >= 0) { vidx = nf; fds[nf++] = {app.viz.fd, POLLIN, 0}; }
        int pr = poll(fds, nf, timeout);
        if (pr > 0) {
            if (fds[0].revents & POLLIN) {
                char buf[512];
                ssize_t r = read(0, buf, sizeof buf);
                if (r > 0) app.parse_input(string(buf, r));
                else if (r == 0) break;
            }
            if (fds[1].revents & (POLLIN | POLLHUP | POLLERR)) {
                vector<J> msgs;
                app.mpv.read_msgs(msgs);
                for (auto& m : msgs) app.on_msg(m);
            }
            if (vidx >= 0 && (fds[vidx].revents & (POLLIN | POLLHUP))) app.viz.pump();
        }
        now = mono_now();
        if (now >= next_frame) {
            struct winsize ws;
            if (g_winch || app.cv.w == 0) {
                g_winch = 0;
                if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
                    app.cv.resize(ws.ws_col, ws.ws_row);
                    write_all("\x1b[2J");
                }
            }
            app.viz.pump();
            app.tick(std::min(0.2, now - last));
            last = now;
            app.draw();
            out.clear();
            app.cv.flush(out);
            write_all(out);
            next_frame = now + 1.0 / 30.0;
        }
    }

    app.viz.stop();
    app.mpv.stop();
    term_restore();
    return 0;
}
