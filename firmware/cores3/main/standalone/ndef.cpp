#include "ndef.hpp"
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <stdexcept>

namespace qb::local {
namespace {
const char* prefixes[] = {"","http://www.","https://www.","http://","https://","tel:","mailto:",
    "ftp://anonymous:anonymous@","ftp://ftp.","ftps://","sftp://","smb://","nfs://","ftp://","dav://",
    "news:","telnet://","imap:","rtsp://","urn:","pop:","sip:","sips:","tftp:","btspp://","btl2cap://",
    "btgoep://","tcpobex://","irdaobex://","file://","urn:epc:id:","urn:epc:tag:","urn:epc:pat:",
    "urn:epc:raw:","urn:epc:","urn:nfc:"};
void require(bool ok, const char* message) { if (!ok) throw std::invalid_argument(message); }
Bytes bytes(const std::string& s) { return Bytes(s.begin(), s.end()); }
std::string trim(const std::string& s) {
    auto a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b-a+1);
}
bool digit(unsigned char c) { return c >= '0' && c <= '9'; }
bool decimal_valid(const std::string& s) {
    size_t i = !s.empty() && (s[0] == '+' || s[0] == '-') ? 1 : 0, digits = 0;
    while (i < s.size() && digit(s[i])) { ++i; ++digits; }
    if (i < s.size() && s[i] == '.') {
        ++i; while (i < s.size() && digit(s[i])) { ++i; ++digits; }
    }
    if (!digits) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i; if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        size_t begin = i; while (i < s.size() && digit(s[i])) ++i;
        if (i == begin) return false;
    }
    return i == s.size();
}
std::string coordinate(double d) {
    char buffer[128]; unsigned precision = 1;
    for (; precision <= 17; ++precision) {
        std::snprintf(buffer,sizeof(buffer),"%.*g",int(precision),d);
        if (std::strtod(buffer,nullptr) == d) break;
    }
    std::string out = buffer;
    auto e = out.find('e');
    if (e != std::string::npos) {
        int exponent = std::atoi(out.c_str()+e+1);
        if (exponent >= -4 && exponent < 16) {
            std::snprintf(buffer,sizeof(buffer),"%.*f",std::max(0,int(precision)-1-exponent),d);
            out = buffer;
        }
    }
    if (out.find_first_of(".eE") == std::string::npos) out += ".0";
    return out;
}
std::string lower(std::string s) {
    for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
bool alpha(unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool language_valid(const std::string& s) {
    return !s.empty() && s.size() <= 63 && std::all_of(s.begin(),s.end(),[](unsigned char c){return alpha(c) || c == '-';});
}
bool uri_valid(const std::string& s, bool http) {
    size_t colon = s.find(':');
    if (colon == std::string::npos || !colon || !alpha(s[0]) || colon+1 == s.size()) return false;
    for (size_t i = 1; i < colon; ++i) if (!alpha(s[i]) && !digit(s[i]) && s[i] != '+' && s[i] != '-' && s[i] != '.') return false;
    for (unsigned char c : s) if (std::isspace(c) || c < 32 || c == 127) return false;
    if (!http) return true;
    auto scheme = lower(s.substr(0,colon));
    if ((scheme != "http" && scheme != "https") || s.compare(colon+1,2,"//") != 0) return false;
    size_t host = colon+3, end = s.find_first_of("/?#",host);
    return host < s.size() && (end == std::string::npos || end > host);
}
bool package_valid(const std::string& s) {
    bool start = true, dotted = false;
    for (unsigned char c : s) {
        if (c == '.') { if (start) return false; start = true; dotted = true; }
        else { if (start ? !alpha(c) : !alpha(c) && !digit(c) && c != '_') return false; start = false; }
    }
    return dotted && !start;
}
bool phone_valid(const std::string& s) {
    size_t i = !s.empty() && s[0] == '+' ? 1 : 0;
    return i < s.size() && std::all_of(s.begin()+i,s.end(),[](unsigned char c){return digit(c) || c == '(' || c == ')' || c == '-';});
}
bool integer_valid(const std::string& s) {
    size_t i = !s.empty() && (s[0] == '+' || s[0] == '-') ? 1 : 0;
    return i < s.size() && std::all_of(s.begin()+i,s.end(),[](unsigned char c){return digit(c);});
}
bool mime_valid(const std::string& s) {
    size_t slash = s.find('/');
    if (!slash || slash == std::string::npos || slash+1 == s.size() || s.find('/',slash+1) != std::string::npos) return false;
    return std::all_of(s.begin(),s.end(),[](unsigned char c) {
        return alpha(c) || digit(c) || std::string("!#$&^_.+-/").find(char(c)) != std::string::npos;
    });
}
uint32_t utf8_next(const std::string& s, size_t& i) {
    uint8_t first = uint8_t(s[i++]);
    if (first < 128) return first;
    unsigned n = first >= 0xc2 && first <= 0xdf ? 1 : first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : 4;
    require(n <= 3 && n <= s.size()-i,"Invalid UTF-8 field.");
    uint32_t cp = first & (n == 1 ? 31 : n == 2 ? 15 : 7);
    for (unsigned j = 0; j < n; ++j) {
        uint8_t c = uint8_t(s[i++]); require((c&0xc0) == 0x80,"Invalid UTF-8 field."); cp = cp<<6 | (c&63);
    }
    require(cp >= (n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u) && cp <= 0x10ffff && (cp < 0xd800 || cp > 0xdfff),"Invalid UTF-8 field.");
    return cp;
}
std::string quote(const std::string& s, const std::string& safe = "") {
    const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            std::string("-._~").find(char(c)) != std::string::npos || safe.find(char(c)) != std::string::npos) out += char(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
Bytes unhex(const std::string& s, size_t exact = 0) {
    std::string clean;
    for (unsigned char c : s) {
        if (std::isspace(c) || c == ':' || c == '-') continue;
        require(std::isxdigit(c), "Invalid hexadecimal data.");
        clean += char(c);
    }
    require(clean.size() % 2 == 0 && (!exact || clean.size() == exact*2), "Wrong hexadecimal length.");
    Bytes out;
    for (size_t i = 0; i < clean.size(); i += 2) out.push_back(uint8_t(std::stoul(clean.substr(i, 2), nullptr, 16)));
    return out;
}
NdefRecord uri(const std::string& s) {
    size_t best = 0, length = 0;
    for (size_t i = 0; i < sizeof(prefixes)/sizeof(*prefixes); ++i) {
        size_t n = std::strlen(prefixes[i]);
        if (n > length && s.compare(0, n, prefixes[i]) == 0) { best = i; length = n; }
    }
    NdefRecord r{1, {'U'}, {uint8_t(best)}, {}};
    r.payload.insert(r.payload.end(), s.begin()+length, s.end());
    return r;
}
NdefRecord text(const std::string& s, const std::string& language = "en") {
    require(language_valid(language), "Invalid language code.");
    NdefRecord r{1, {'T'}, {uint8_t(language.size())}, {}};
    r.payload.insert(r.payload.end(), language.begin(), language.end());
    r.payload.insert(r.payload.end(), s.begin(), s.end());
    return r;
}
void valid(const NdefRecord& r) {
    require(r.tnf <= 5 && r.type.size() <= 255 && r.identifier.size() <= 255 && r.payload.size() <= ndef_limit,
            "Invalid record size or TNF.");
    require(!(r.tnf == 0 && (!r.type.empty() || !r.payload.empty() || !r.identifier.empty())) &&
            !(r.tnf == 5 && !r.type.empty()) && !(r.tnf > 0 && r.tnf < 5 && r.type.empty()), "Invalid NDEF type.");
}
void attribute(Bytes& out, unsigned kind, const Bytes& value) {
    out.insert(out.end(), {uint8_t(kind >> 8), uint8_t(kind), uint8_t(value.size() >> 8), uint8_t(value.size())});
    out.insert(out.end(), value.begin(), value.end());
}
std::string json(const std::string& s) {
    std::string out = "\"";
    auto escape = [&](uint32_t cp) {
        char buf[7]; std::snprintf(buf,sizeof(buf),"\\u%04x",unsigned(cp)); out += buf;
    };
    for (size_t i = 0; i < s.size();) {
        uint32_t c = utf8_next(s,i);
        if (c == '"' || c == '\\') { out += '\\'; out += char(c); }
        else if (c == '\b') out += "\\b";
        else if (c == '\f') out += "\\f";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 32 || (c >= 127 && c <= 0xffff)) escape(c);
        else if (c > 0xffff) { c -= 0x10000; escape(0xd800+(c>>10)); escape(0xdc00+(c&1023)); }
        else out += char(c);
    }
    return out + '"';
}
std::string vescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '\r') { if (i+1 < s.size() && s[i+1] == '\n') ++i; c = '\n'; }
        if (c == '\n') out += "\\n";
        else { if (c == '\\' || c == ';' || c == ',') out += '\\'; out += c; }
    }
    return out;
}
std::string fold(const std::string& s) {
    std::string out;
    size_t column = 0;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        size_t n = c < 128 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : 4;
        require(n <= s.size()-i, "Invalid UTF-8 text.");
        if (column+n > 75) { out += "\r\n "; column = 1; }
        out.append(s, i, n); column += n; i += n;
    }
    return out;
}
}
const std::vector<NdefKind>& ndef_catalog() {
    static const std::vector<NdefKind> catalog = {
        {"Text", {{"text","Text","",{}},{"language","Language","en",{}}}},
        {"URL / URI", {{"url","Complete link / URI","",{}}}},
        {"Custom URL / URI", {{"url","Complete link / URI","",{}}}},
        {"Unit.Link", {{"url","Complete link / URI","",{}}}},
        {"Payment Link", {{"url","Complete link / URI","",{}}}},
        {"Social Networks", {{"url","Complete link / URI","",{}}}},
        {"Video Link", {{"url","Complete link / URI","",{}}}},
        {"File Link", {{"url","Complete link / URI","",{}}}},
        {"Search", {{"query","Search terms","",{}},{"engine","Search engine","Google",{"Google","DuckDuckGo","Bing"}}}},
        {"Application", {{"package","Android package name","",{}}}},
        {"Mail", {{"email","Email","",{}},{"subject","Subject","",{}},{"body","Message","",{}}}},
        {"Contact", {{"name","Name","",{}},{"organization","Organization","",{}},{"phone","Phone","",{}},{"email","Email","",{}},{"url","Url","",{}}}},
        {"Phone", {{"phone","Phone number","",{}}}},
        {"Location", {{"latitude","Latitude","",{}},{"longitude","Longitude","",{}}}},
        {"SOS", {{"name","Name","",{}},{"contact","Contact","",{}},{"phone","Phone","",{}},{"medical","Medical","",{}},{"notes","Notes","",{}}}},
        {"Bluetooth", {{"address","Bluetooth address","",{}},{"name","Device name","",{}}}},
        {"3D Printing", {{"material","Material","PLA",{}},{"brand","Brand","Generic",{}},{"color","Color hex","FFFFFF",{}},{"minTemp","Minimum °C","190",{}},{"maxTemp","Maximum °C","220",{}}}},
        {"Wi-Fi", {{"ssid","Network name","",{}},{"security","Security","WPA2 Personal",{"WPA2 Personal","Open"}},{"password","Network password (stored on tag)","",{}}}},
        {"Data", {{"mime","MIME type","application/octet-stream",{}},{"encoding","Payload format","Hexadecimal",{"Hexadecimal","UTF-8 text"}},{"payload","Data","",{}}}}
    };
    return catalog;
}
Status ndef_encode(const std::vector<NdefRecord>& records, Bytes& out, std::string& error) {
    out.clear(); error.clear();
    try {
        require(!records.empty() && records.size() <= 64, "Add 1–64 records.");
        for (size_t i = 0; i < records.size(); ++i) {
            const auto& r = records[i]; valid(r);
            bool short_record = r.payload.size() < 256;
            out.insert(out.end(), {uint8_t(r.tnf | (i == 0 ? 128 : 0) | (i+1 == records.size() ? 64 : 0) |
                (short_record ? 16 : 0) | (r.identifier.empty() ? 0 : 8)), uint8_t(r.type.size())});
            unsigned count = short_record ? 1 : 4;
            while (count--) out.push_back(uint8_t(r.payload.size() >> (count*8)));
            if (!r.identifier.empty()) out.push_back(uint8_t(r.identifier.size()));
            out.insert(out.end(), r.type.begin(), r.type.end());
            out.insert(out.end(), r.identifier.begin(), r.identifier.end());
            out.insert(out.end(), r.payload.begin(), r.payload.end());
            require(out.size() <= ndef_limit, "Message exceeds 4096 bytes.");
        }
        return OK;
    } catch (const std::exception& e) { out.clear(); error = e.what(); return INVALID_ARGUMENT; }
}
Status ndef_decode(const Bytes& data, std::vector<NdefRecord>& out, std::string& error) {
    out.clear(); error.clear();
    try {
        require(data.size() <= ndef_limit, "Message exceeds 4096 bytes.");
        size_t p = 0; bool ended = false;
        auto take = [&]() { require(p < data.size(), "Truncated NDEF record."); return data[p++]; };
        auto get = [&](size_t n) {
            require(n <= data.size()-p, "Truncated NDEF data.");
            Bytes value(data.begin()+p, data.begin()+p+n); p += n; return value;
        };
        while (p < data.size()) {
            uint8_t header = take(), type_length = take();
            require(bool(header & 128) == out.empty() && !(header & 32) && !ended, "Malformed or chunked NDEF message.");
            uint32_t length = 0;
            for (unsigned n = header & 16 ? 1 : 4; n; --n) length = (length << 8) | take();
            unsigned id_length = header & 8 ? take() : 0;
            NdefRecord r; r.tnf = header & 7; r.type = get(type_length); r.identifier = get(id_length); r.payload = get(length);
            valid(r); out.push_back(r); require(out.size() <= 64, "Too many records."); ended = header & 64;
        }
        require(ended || data.empty(), "Missing NDEF end marker."); return OK;
    } catch (const std::exception& e) { out.clear(); error = e.what(); return INVALID_ARGUMENT; }
}
Status ndef_build(size_t kind, const NdefValues& values, NdefRecord& result, std::string& error) {
    result.type.clear(); result.payload.clear(); result.identifier.clear(); result.tnf = 0; error.clear();
    try {
        require(kind < ndef_catalog().size(), "Unknown record kind.");
        const auto& descriptor = ndef_catalog()[kind]; NdefValues v;
        struct WipeValues {
            NdefValues& values;
            ~WipeValues() { for (auto& field : values) secure_zero(field.second.data(),field.second.size()); }
        } wipe{v};
        for (const auto& f : descriptor.fields) {
            auto it = values.find(f.id);
            require(it == values.end() || it->second.size() <= ndef_field_limit, "Field exceeds 256 bytes.");
            v[f.id] = it == values.end() ? f.initial : it->second;
            for (size_t cursor = 0; cursor < v[f.id].size();) utf8_next(v[f.id],cursor);
            if (!f.choices.empty()) require(std::any_of(f.choices.begin(), f.choices.end(), [&](const char* choice) {
                return v[f.id] == choice;
            }), "Invalid field choice.");
        }
        auto required = [&](const char* k) { auto s = trim(v[k]); require(!s.empty(), "Complete required fields."); return s; };
        const std::string name = descriptor.label; NdefRecord r;
        if (name == "Text") { required("text"); r = text(v["text"], required("language")); }
        else if (v.count("url") && name != "Contact") {
            auto s = required("url");
            require(uri_valid(s,name != "Custom URL / URI" && name != "Payment Link" && name != "File Link"), "Enter a complete URI with the required scheme.");
            r = uri(s);
        } else if (name == "Search") {
            std::string base = v["engine"] == "Google" ? "https://www.google.com/search" :
                v["engine"] == "Bing" ? "https://www.bing.com/search" : "https://duckduckgo.com/";
            r = uri(base + "?q=" + quote(required("query")));
        } else if (name == "Application") {
            auto s = required("package"); require(package_valid(s), "Invalid Android package.");
            r = {4, bytes("android.com:pkg"), bytes(s), {}};
        } else if (name == "Mail") {
            auto s = required("email"); require(s.find('@') != std::string::npos && s.find_first_of(" \t\r\n?#") == std::string::npos, "Invalid email.");
            s = "mailto:" + quote(s, "@.+"); bool first = true;
            for (const char* k : {"subject","body"}) if (!v[k].empty()) {
                s += first ? "?" : "&"; first = false; s += std::string(k) + "=" + quote(v[k]);
            }
            r = uri(s);
        } else if (name == "Phone") {
            std::string s;
            for (unsigned char c : required("phone")) if (!std::isspace(c)) s += char(c);
            require(phone_valid(s), "Invalid phone."); r = uri("tel:" + s);
        } else if (name == "Location") {
            auto numeric = [&](const char* key, double bound) {
                auto s = trim(v[key]); require(decimal_valid(s),"Invalid coordinates.");
                char* end = nullptr; double d = std::strtod(s.c_str(), &end);
                require(!s.empty() && end == s.c_str()+s.size() && std::isfinite(d) && std::abs(d) <= bound, "Invalid coordinates.");
                return coordinate(d);
            };
            r = uri("geo:" + numeric("latitude",90) + "," + numeric("longitude",180));
        } else if (name == "Contact") {
            auto s = vescape(required("name"));
            std::vector<std::string> lines{"BEGIN:VCARD","VERSION:3.0","N:;"+s+";;;","FN:"+s};
            for (const auto& pair : std::vector<std::pair<const char*,const char*>>{{"organization","ORG"},{"phone","TEL"},{"email","EMAIL"},{"url","URL"}})
                if (!v[pair.first].empty()) lines.push_back(std::string(pair.second)+":"+vescape(v[pair.first]));
            lines.push_back("END:VCARD"); std::string payload;
            for (const auto& line : lines) payload += fold(line)+"\r\n";
            r = {2, bytes("text/vcard"), bytes(payload), {}};
        } else if (name == "SOS") {
            std::string s = "EMERGENCY INFORMATION\nName: "+required("name");
            for (const char* k : {"contact","phone","medical","notes"}) if (!v[k].empty()) {
                std::string label = k; label[0] = char(std::toupper(label[0])); s += "\n"+label+": "+v[k];
            }
            r = text(s);
        } else if (name == "Bluetooth") {
            auto address = unhex(required("address"),6); const auto& s = v["name"]; require(s.size() <= 248, "Bluetooth name too long.");
            unsigned length = 8 + (s.empty() ? 0 : s.size()+2);
            Bytes payload{uint8_t(length),uint8_t(length >> 8)}; payload.insert(payload.end(), address.rbegin(), address.rend());
            if (!s.empty()) { payload.insert(payload.end(), {uint8_t(s.size()+1),9}); payload.insert(payload.end(), s.begin(), s.end()); }
            r = {2, bytes("application/vnd.bluetooth.ep.oob"), payload, {}};
        } else if (name == "3D Printing") {
            auto color = v["color"]; color.erase(0, color.find_first_not_of('#')); while (!color.empty() && color.back() == '#') color.pop_back();
            unhex(color,3); for (auto& c : color) c = char(std::toupper(static_cast<unsigned char>(c)));
            require(integer_valid(v["minTemp"]) && integer_valid(v["maxTemp"]), "Enter whole temperatures.");
            int low = std::stoi(v["minTemp"]), high = std::stoi(v["maxTemp"]); require(low >= 0 && high >= low && high <= 500, "Temperature out of range.");
            std::string s = "{\"brand\":"+json(required("brand"))+",\"color_hex\":"+json(color)+",\"max_temp\":"+json(std::to_string(high))+
                ",\"min_temp\":"+json(std::to_string(low))+",\"protocol\":\"openspool\",\"type\":"+json(required("material"))+",\"version\":\"1.0\"}";
            r = {2, bytes("application/json"), bytes(s), {}};
        } else if (name == "Wi-Fi") {
            auto ssid = bytes(v["ssid"]), password = bytes(v["password"]); bool open = v["security"] == "Open";
            require(!ssid.empty() && ssid.size() <= 32, "SSID needs 1–32 bytes.");
            bool hex_password = password.size() == 64 && std::all_of(password.begin(),password.end(),[](unsigned char c){return std::isxdigit(c);});
            require(open || (password.size() >= 8 && password.size() <= 63) || hex_password, "Invalid WPA2 password.");
            Bytes credential; attribute(credential,0x1026,{1}); attribute(credential,0x1045,ssid);
            attribute(credential,0x1003,{0,uint8_t(open ? 1 : 32)}); attribute(credential,0x100f,{0,uint8_t(open ? 1 : 8)});
            attribute(credential,0x1027,open ? Bytes{} : password); attribute(credential,0x1020,Bytes(6,255));
            Bytes payload; attribute(payload,0x104a,{16}); attribute(payload,0x100e,credential);
            r = {2, bytes("application/vnd.wfa.wsc"), payload, {}};
        } else {
            auto mime = lower(required("mime")); require(mime_valid(mime), "Invalid MIME type.");
            r = {2, bytes(mime), v["encoding"] == "UTF-8 text" ? bytes(v["payload"]) : unhex(v["payload"]), {}};
        }
        valid(r); result = r; return OK;
    } catch (const std::exception& e) { error = e.what(); return INVALID_ARGUMENT; }
}
}
