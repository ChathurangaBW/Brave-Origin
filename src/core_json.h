// core_json.h
// -----------------------------------------------------------------------------
// Shared engine core: minimal JSON tree (parse / mutate / compact serialize).
// Platform-independent C++17; shared by the Windows engine (identical semantics)
// and the Linux port. No platform headers.
//
// Semantics (must stay identical on all platforms):
//   * Strict RFC 8259 parsing + strict RFC 3629 UTF-8 validation.
//   * Duplicate object keys rejected; trailing characters rejected.
//   * Compact serialization with object keys in sorted order.
// -----------------------------------------------------------------------------
#pragma once

#include <algorithm>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

struct JValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj };

    Type t = Null;
    bool b = false;
    std::string num;
    std::string str;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue> > obj;

    static JValue makeBool(bool v)             { JValue x; x.t = Bool; x.b = v; return x; }
    static JValue makeStr(const std::string& s){ JValue x; x.t = Str; x.str = s; return x; }
    static JValue makeObj()                    { JValue x; x.t = Obj;  return x; }

    bool isObj()  const { return t == Obj; }
    bool isStr()  const { return t == Str; }
    bool isBool() const { return t == Bool; }
    bool isArr()  const { return t == Arr; }
    bool isNum()  const { return t == Num; }

    JValue* find(const std::string& key) {
        if (t != Obj) return nullptr;
        for (size_t i = 0; i < obj.size(); ++i)
            if (obj[i].first == key) return &obj[i].second;
        return nullptr;
    }

    JValue& set(const std::string& key, JValue v) {
        for (size_t i = 0; i < obj.size(); ++i)
            if (obj[i].first == key) { obj[i].second = std::move(v); return obj[i].second; }
        obj.push_back(std::make_pair(key, std::move(v)));
        return obj.back().second;
    }

    JValue& ensureObj(const std::string& key) {
        JValue* p = find(key);
        if (p && p->t == Obj) return *p;
        return set(key, makeObj());
    }
};

struct JParser {
    const std::string& s;
    size_t i;
    std::string err;

    explicit JParser(const std::string& src) : s(src), i(0) {}

    bool fail(const char* m) { if (err.empty()) err = m; return false; }

    void skipWs() {
        while (i < s.size()) {
            char c = s[i];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') ++i; else break;
        }
    }

    bool parse(JValue& out) {
        skipWs();
        if (!parseValue(out)) return false;
        skipWs();
        if (i != s.size()) return fail("trailing characters after JSON value");
        return true;
    }

    bool parseValue(JValue& out) {
        skipWs();
        if (i >= s.size()) return fail("unexpected end of input");
        char c = s[i];
        if (c == '{') return parseObject(out);
        if (c == '[') return parseArray(out);
        if (c == '"') { out.t = JValue::Str; return parseString(out.str); }
        if (c == 't') { if (s.compare(i, 4, "true")  == 0) { i += 4; out.t = JValue::Bool; out.b = true;  return true; } return fail("bad literal"); }
        if (c == 'f') { if (s.compare(i, 5, "false") == 0) { i += 5; out.t = JValue::Bool; out.b = false; return true; } return fail("bad literal"); }
        if (c == 'n') { if (s.compare(i, 4, "null")  == 0) { i += 4; out.t = JValue::Null; return true; } return fail("bad literal"); }
        return parseNumber(out);
    }

    bool parseNumber(JValue& out) {
        size_t start = i;
        if (i < s.size() && s[i] == '-') ++i;
        if (i >= s.size()) return fail("invalid number");
        if (s[i] == '0') { ++i; if (i < s.size() && s[i] >= '0' && s[i] <= '9') return fail("leading zero in number"); }
        else if (s[i] >= '1' && s[i] <= '9') while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        else return fail("invalid number");
        if (i < s.size() && s[i] == '.') {
            ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("fraction requires digit");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
            ++i;
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
            if (i >= s.size() || s[i] < '0' || s[i] > '9') return fail("exponent requires digit");
            while (i < s.size() && s[i] >= '0' && s[i] <= '9') ++i;
        }
        out.t = JValue::Num;
        out.num = s.substr(start, i - start);
        return true;
    }

    bool parseHex4(unsigned& v) {
        if (i + 4 > s.size()) return fail("bad \\u escape");
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char h = s[i++];
            v <<= 4;
            if (h >= '0' && h <= '9')      v |= (unsigned)(h - '0');
            else if (h >= 'a' && h <= 'f') v |= (unsigned)(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') v |= (unsigned)(h - 'A' + 10);
            else return fail("bad hex in \\u escape");
        }
        return true;
    }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out.push_back((char)cp);
        } else if (cp < 0x800) {
            out.push_back((char)(0xC0 | (cp >> 6)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else if (cp < 0x10000) {
            out.push_back((char)(0xE0 | (cp >> 12)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        } else {
            out.push_back((char)(0xF0 | (cp >> 18)));
            out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back((char)(0x80 | (cp & 0x3F)));
        }
    }

    bool parseString(std::string& out) {
        ++i;
        out.clear();
        while (true) {
            if (i >= s.size()) return fail("unterminated string");
            unsigned char c = (unsigned char)s[i];
            if (c == '"') { ++i; return true; }
            if (c == '\\') {
                ++i;
                if (i >= s.size()) return fail("bad escape");
                char e = s[i++];
                switch (e) {
                    case '"':  out.push_back('"');  break;
                    case '\\': out.push_back('\\'); break;
                    case '/':  out.push_back('/');  break;
                    case 'b':  out.push_back('\b'); break;
                    case 'f':  out.push_back('\f'); break;
                    case 'n':  out.push_back('\n'); break;
                    case 'r':  out.push_back('\r'); break;
                    case 't':  out.push_back('\t'); break;
                    case 'u': {
                        unsigned cp = 0;
                        if (!parseHex4(cp)) return false;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (!(i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u')) return fail("lone high surrogate");
                            i += 2;
                            unsigned lo = 0;
                            if (!parseHex4(lo)) return false;
                            if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            else return fail("invalid surrogate pair");
                        }
                        else if (cp >= 0xDC00 && cp <= 0xDFFF) return fail("lone low surrogate");
                        appendUtf8(out, cp);
                        break;
                    }
                    default: return fail("unknown escape");
                }
            } else {
                if (c < 0x20) return fail("unescaped control character");
                if (c < 0x80) { out.push_back((char)c); ++i; continue; }
                size_t len=0; unsigned cp=0, mincp=0;
                if(c>=0xC2&&c<=0xDF){len=2;cp=c&0x1F;mincp=0x80;}
                else if(c>=0xE0&&c<=0xEF){len=3;cp=c&0x0F;mincp=0x800;}
                else if(c>=0xF0&&c<=0xF4){len=4;cp=c&0x07;mincp=0x10000;}
                else return fail("invalid UTF-8 lead byte");
                if(i+len>s.size()) return fail("truncated UTF-8 sequence");
                for(size_t n=1;n<len;n++){unsigned char cc=(unsigned char)s[i+n];if((cc&0xC0)!=0x80)return fail("invalid UTF-8 continuation");cp=(cp<<6)|(cc&0x3F);}
                if(cp<mincp||cp>0x10FFFF||(cp>=0xD800&&cp<=0xDFFF))return fail("invalid UTF-8 scalar");
                out.append(s,i,len); i+=len;
            }
        }
    }

    bool parseObject(JValue& out) {
        out.t = JValue::Obj;
        ++i;
        skipWs();
        if (i < s.size() && s[i] == '}') { ++i; return true; }
        while (true) {
            skipWs();
            if (i >= s.size() || s[i] != '"') return fail("expected object key");
            std::string key;
            if (!parseString(key)) return false;
            for (size_t k = 0; k < out.obj.size(); ++k) if (out.obj[k].first == key) return fail("duplicate object key");
            skipWs();
            if (i >= s.size() || s[i] != ':') return fail("expected ':'");
            ++i;
            JValue v;
            if (!parseValue(v)) return false;
            out.obj.push_back(std::make_pair(std::move(key), std::move(v)));
            skipWs();
            if (i >= s.size()) return fail("unterminated object");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == '}') { ++i; return true; }
            return fail("expected ',' or '}'");
        }
    }

    bool parseArray(JValue& out) {
        out.t = JValue::Arr;
        ++i;
        skipWs();
        if (i < s.size() && s[i] == ']') { ++i; return true; }
        while (true) {
            JValue v;
            if (!parseValue(v)) return false;
            out.arr.push_back(std::move(v));
            skipWs();
            if (i >= s.size()) return fail("unterminated array");
            if (s[i] == ',') { ++i; continue; }
            if (s[i] == ']') { ++i; return true; }
            return fail("expected ',' or ']'");
        }
    }
};

inline void appendEscaped(std::string& out, const std::string& s) {
    out.push_back('"');
    for (size_t k = 0; k < s.size(); ++k) {
        unsigned char c = (unsigned char)s[k];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::sprintf(buf, "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    out.push_back('"');
}

inline void serializeJson(const JValue& v, std::string& out) {
    switch (v.t) {
        case JValue::Null: out += "null"; break;
        case JValue::Bool: out += (v.b ? "true" : "false"); break;
        case JValue::Num:  out += v.num; break;
        case JValue::Str:  appendEscaped(out, v.str); break;
        case JValue::Arr: {
            out.push_back('[');
            for (size_t k = 0; k < v.arr.size(); ++k) { if (k) out.push_back(','); serializeJson(v.arr[k], out); }
            out.push_back(']');
            break;
        }
        case JValue::Obj: {
            out.push_back('{');
            std::vector<size_t> idx(v.obj.size());
            for (size_t k = 0; k < idx.size(); ++k) idx[k] = k;
            std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) {
                return v.obj[a].first < v.obj[b].first;
            });
            for (size_t n = 0; n < idx.size(); ++n) {
                if (n) out.push_back(',');
                appendEscaped(out, v.obj[idx[n]].first);
                out.push_back(':');
                serializeJson(v.obj[idx[n]].second, out);
            }
            out.push_back('}');
            break;
        }
    }
}
