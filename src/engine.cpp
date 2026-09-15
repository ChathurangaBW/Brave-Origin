// BraveOriginFix.cpp
// -----------------------------------------------------------------------------
// Dynamic analysis + (gated) auto-bypass for Brave "Origin" purchase-validation
// state.  Single-file C++17 Win32 console app. No external JSON library, no
// shelling out to python/powershell.
//
//   Channel root : %LOCALAPPDATA%\BraveSoftware\<channel>\User Data\Local State
//   OK           : brave.origin.purchase_validated == true
//                  AND skus.state is a non-empty object
//
// MODES (default is READ-ONLY; nothing is killed, written or launched):
//   (no args) / --check       read-only analysis
//   --dry-run                 analysis + print planned actions, never writes
//   --apply                   enables patching (kill/backup/write/relaunch)
//   --channel <name>          limit to one channel
//   --restore <bakfile>       restore a "Local State.bak.*" file and re-verify
//   --help                    usage
//
// SAFETY:
//   * Process kills are scoped to the EXACT profile being patched:
//       image path contains \BraveSoftware\<channel>\
//       AND ( --user-data-dir == profile path
//             OR (no --user-data-dir AND the process inherited OUR LOCALAPPDATA
//                 AND the profile being patched is that channel's default) )
//     A process whose command line cannot be read is NEVER killed.
//   * Graceful WM_CLOSE first (5s), TerminateProcess only as a fallback.
//   * Relaunch reuses the killed process's ORIGINAL command line.
//
// PAYLOAD NOTE (documented assumption):
//   The skus.state["67"] value is a *community-bypass* shape (a small nested
//   JSON credential-ish blob). It is NOT vendor-verified. We only add/update the
//   two marker keys and preserve every other key. Post-write we re-parse and
//   verify the markers both immediately and again after a 3s settle delay.
//
// Build:
//   g++ -std=c++17 -O2 -static -static-libgcc -static-libstdc++ \
//       -o BraveOriginFix.exe BraveOriginFix.cpp -lole32 -loleaut32
//
// Exit codes: 0 = nothing broken / nothing to do
//             1 = at least one channel patched (or BROKEN detected read-only)
//             2 = environmental error
// -----------------------------------------------------------------------------

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include <utility>
#include <algorithm>
#include <shellapi.h>
#include "engine.h"

// =============================================================================
// UTF-8 / UTF-16 helpers
// =============================================================================
static std::string toUtf8(const std::wstring& w) {
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring toLowerW(std::wstring s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] >= L'A' && s[i] <= L'Z') s[i] = (wchar_t)(s[i] + 32);
    return s;
}

static std::string lowerAscii(std::string s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
    return s;
}

static std::wstring normalizePath(const std::wstring& p) {
    std::wstring s = toLowerW(p);
    for (size_t i = 0; i < s.size(); ++i) if (s[i] == L'/') s[i] = L'\\';
    while (s.size() > 1 && s[s.size() - 1] == L'\\') s.resize(s.size() - 1);
    return s;
}

// =============================================================================
// Minimal JSON tree (parse / mutate / compact serialize) -- unchanged
// =============================================================================
struct JValue {
    enum Type { Null, Bool, Num, Str, Arr, Obj };

    Type t = Null;
    bool b = false;
    std::string num;
    std::string str;
    std::vector<JValue> arr;
    std::vector<std::pair<std::string, JValue> > obj;

    static JValue makeBool(bool v)            { JValue x; x.t = Bool; x.b = v; return x; }
    static JValue makeStr(const std::string& s){ JValue x; x.t = Str; x.str = s; return x; }
    static JValue makeObj()                   { JValue x; x.t = Obj;  return x; }

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

static void appendEscaped(std::string& out, const std::string& s) {
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

static void serialize(const JValue& v, std::string& out) {
    switch (v.t) {
        case JValue::Null: out += "null"; break;
        case JValue::Bool: out += (v.b ? "true" : "false"); break;
        case JValue::Num:  out += v.num; break;
        case JValue::Str:  appendEscaped(out, v.str); break;
        case JValue::Arr: {
            out.push_back('[');
            for (size_t k = 0; k < v.arr.size(); ++k) { if (k) out.push_back(','); serialize(v.arr[k], out); }
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
                serialize(v.obj[idx[n]].second, out);
            }
            out.push_back('}');
            break;
        }
    }
}
// =============================================================================
// File helpers
// =============================================================================
static std::wstring dirnameOf(const std::wstring& p);
static std::wstring envW(const wchar_t* name);
static bool readFileBytes(const std::wstring& path, std::string& out, ULONGLONG* sizeOut, FILETIME* mtimeOut) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz; sz.QuadPart = 0;
    GetFileSizeEx(h, &sz);
    out.clear();
    if (sz.QuadPart > 0) out.resize((size_t)sz.QuadPart);
    DWORD total = 0;
    while (total < (DWORD)sz.QuadPart) {
        DWORD rd = 0;
        if (!ReadFile(h, &out[total], (DWORD)(sz.QuadPart - total), &rd, nullptr) || rd == 0) break;
        total += rd;
    }
    out.resize(total);
    if (sizeOut) *sizeOut = (ULONGLONG)total;
    if (mtimeOut) GetFileTime(h, nullptr, nullptr, mtimeOut);
    CloseHandle(h);
    return true;
}

static bool fullPath(const std::wstring& path, std::wstring& out) {
    DWORD n = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr); if (!n) return false;
    std::vector<wchar_t> b(n + 1); DWORD m = GetFullPathNameW(path.c_str(), (DWORD)b.size(), b.data(), nullptr);
    if (!m || m >= b.size()) return false; out.assign(b.data(), m); return true;
}
static bool hasReparseComponent(const std::wstring& path) {
    std::wstring p; if (!fullPath(path, p)) return true;
    size_t pos = (p.size() >= 3 && p[1] == L':') ? 3 : 0;
    while (true) { pos = p.find(L'\\', pos); std::wstring part = pos == std::wstring::npos ? p : p.substr(0, pos);
        DWORD a = GetFileAttributesW(part.c_str()); if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT)) return true;
        if (pos == std::wstring::npos) break; ++pos;
    } return false;
}
static bool canonicalExisting(const std::wstring& path, bool directory, std::wstring& out) {
    if (hasReparseComponent(path)) return false;
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        (directory ? FILE_FLAG_BACKUP_SEMANTICS : 0) | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false; wchar_t b[32768]; DWORD n=GetFinalPathNameByHandleW(h,b,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS); CloseHandle(h);
    if (!n || n >= 32768) return false; out.assign(b,n); if(out.rfind(L"\\\\?\\",0)==0) out.erase(0,4); out=normalizePath(out); return true;
}
static bool testHook(const wchar_t* name) { return envW(L"BRAVEORIGINFIX_TEST_MODE")==L"1" && envW(name)==L"1"; }
static bool processFault(const wchar_t* value){return envW(L"BRAVEORIGINFIX_TEST_MODE")==L"1"&&envW(L"BRAVEORIGINFIX_TEST_PROCESS_FAULT")==value;}
static bool writeRawFile(const std::wstring& path, const std::string& data, DWORD creation) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, creation, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD total = 0;
    while (total < (DWORD)data.size()) {
        DWORD wr = 0;
        if (!WriteFile(h, data.data() + total, (DWORD)(data.size() - total), &wr, nullptr) || wr == 0) break;
        total += wr;
    }
    BOOL writeOk = (total == (DWORD)data.size());
    BOOL flushOk = writeOk ? FlushFileBuffers(h) : FALSE;
    if(testHook(L"BRAVEORIGINFIX_TEST_FLUSH_FAIL") && path.find(L".tmp.")!=std::wstring::npos){SetLastError(ERROR_WRITE_FAULT);flushOk=FALSE;}
    BOOL closeOk=CloseHandle(h);
    return writeOk && flushOk && closeOk;
}
struct FileIdentity { DWORD volume=0, high=0, low=0; };
static bool getFileIdentity(const std::wstring& path,FileIdentity& id,HANDLE* retained=nullptr,bool directory=false){HANDLE h=CreateFileW(path.c_str(),FILE_READ_ATTRIBUTES|GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,(directory?FILE_FLAG_BACKUP_SEMANTICS:0)|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(h==INVALID_HANDLE_VALUE)return false;BY_HANDLE_FILE_INFORMATION x={};if(!GetFileInformationByHandle(h,&x)){CloseHandle(h);return false;}id.volume=x.dwVolumeSerialNumber;id.high=x.nFileIndexHigh;id.low=x.nFileIndexLow;if(retained)*retained=h;else CloseHandle(h);return true;}
static bool sameIdentity(const FileIdentity&a,const FileIdentity&b){return a.volume==b.volume&&a.high==b.high&&a.low==b.low;}
static bool writeFileBytesAtomic(const std::wstring& path, const std::string& data,const FileIdentity* expectedTarget=nullptr,const std::wstring* expectedParent=nullptr) {
    if (hasReparseComponent(path) || hasReparseComponent(dirnameOf(path))) { SetLastError(ERROR_REPARSE_TAG_INVALID); return false; }
    wchar_t suffix[80]; std::swprintf(suffix,80,L".tmp.%lu.%llu",(unsigned long)GetCurrentProcessId(),(unsigned long long)GetTickCount64());
    std::wstring tmp=path+suffix; if(!writeRawFile(tmp,data,CREATE_NEW)){DeleteFileW(tmp.c_str());return false;}
    std::string tempBytes; JValue parsed; if(!readFileBytes(tmp,tempBytes,nullptr,nullptr)){DeleteFileW(tmp.c_str());return false;} JParser parser(tempBytes); if(!parser.parse(parsed)) { DeleteFileW(tmp.c_str()); SetLastError(ERROR_INVALID_DATA); return false; }
    if(expectedTarget){FileIdentity now;std::wstring parent;if(testHook(L"BRAVEORIGINFIX_TEST_RESTORE_RACE")||!getFileIdentity(path,now)||!sameIdentity(*expectedTarget,now)||!canonicalExisting(dirnameOf(path),true,parent)||(expectedParent&&parent!=*expectedParent)||hasReparseComponent(path)){DeleteFileW(tmp.c_str());SetLastError(ERROR_FILE_INVALID);return false;}}
    bool ok = ReplaceFileW(path.c_str(),tmp.c_str(),nullptr,REPLACEFILE_WRITE_THROUGH,nullptr,nullptr)!=FALSE;
    if(!ok && GetLastError()==ERROR_FILE_NOT_FOUND) ok=MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok) DeleteFileW(tmp.c_str()); return ok;
}

// =============================================================================
// Process helpers
// =============================================================================
static std::wstring queryProcessExePath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return std::wstring();
    wchar_t buf[MAX_PATH * 4];
    DWORD n = (DWORD)(sizeof(buf) / sizeof(buf[0]));
    std::wstring r;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) r.assign(buf, n);
    CloseHandle(h);
    return r;
}

// Read a remote UNICODE_STRING (Length@0, MaxLen@2, pad, Buffer@8 on x64).
static bool readRemoteUnicodeString(HANDLE h, PVOID addr, std::wstring& out) {
    USHORT len = 0;
    SIZE_T rd = 0;
    if (!ReadProcessMemory(h, addr, &len, sizeof(len), &rd)) return false;
    if (len == 0) { out.clear(); return true; }
    if (len > 32767 * 2) return false;
    PVOID buf = nullptr;
    if (!ReadProcessMemory(h, (PBYTE)addr + 8, &buf, sizeof(buf), &rd)) return false;
    if (!buf) return false;
    std::vector<wchar_t> w((size_t)len / 2 + 1, 0);
    if (!ReadProcessMemory(h, buf, w.data(), len, &rd)) return false;
    out.assign(w.data(), (size_t)len / 2);
    return true;
}

// Read command line + LOCALAPPDATA env var from a remote process via
// NtQueryInformationProcess -> PEB -> RTL_USER_PROCESS_PARAMETERS (x64).
static bool getProcessInfo(DWORD pid, std::wstring& cmdline, bool& cmdKnown,
                           std::wstring& envLAD, bool& envKnown) {
    cmdKnown = false;
    envKnown = false;
    HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!h) return false;
    bool ok = false;
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        typedef LONG (NTAPI *PFN)(HANDLE, ULONG, PVOID, ULONG, PULONG);
        PFN f = (PFN)GetProcAddress(ntdll, "NtQueryInformationProcess");
        if (f) {
            struct PBI { PVOID r1; PVOID peb; PVOID r2[2]; ULONG_PTR pid; PVOID r3; } pbi;
            std::memset(&pbi, 0, sizeof(pbi));
            ULONG ret = 0;
            if (f(h, 0, &pbi, sizeof(pbi), &ret) == 0 && pbi.peb) {
                PVOID pp = nullptr;
                SIZE_T rd = 0;
                if (ReadProcessMemory(h, (PBYTE)pbi.peb + 0x20, &pp, sizeof(pp), &rd) && pp) {
                    std::wstring cl;
                    if (readRemoteUnicodeString(h, (PBYTE)pp + 0x70, cl)) { cmdline = cl; cmdKnown = true; }
                    PVOID env = nullptr;
                    if (ReadProcessMemory(h, (PBYTE)pp + 0x80, &env, sizeof(env), &rd) && env) {
                        std::vector<wchar_t> buf(32768, 0);
                        SIZE_T got = 0;
                        ReadProcessMemory(h, env, buf.data(), buf.size() * sizeof(wchar_t), &got);
                        size_t n = got / sizeof(wchar_t);
                        size_t pos = 0;
                        while (pos < n) {
                            size_t e = pos;
                            while (e < n && buf[e] != L'\0') ++e;
                            std::wstring var(buf.data() + pos, e - pos);
                            if (var.compare(0, 13, L"LOCALAPPDATA=") == 0) {
                                envLAD = var.substr(13);
                                envKnown = true;
                                break;
                            }
                            if (e >= n) break;
                            pos = e + 1;
                        }
                    }
                    ok = true;
                }
            }
        }
    }
    CloseHandle(h);
    return ok;
}

// Return 1 for one exact switch, 0 for none, -1 for ambiguous/malformed switches.
static int extractUserDataDir(const std::wstring& cl, std::wstring& out) {
    int argc=0; LPWSTR* argv=CommandLineToArgvW(cl.c_str(),&argc); if(!argv || argc<1){if(argv)LocalFree(argv);return -1;}
    int found=0;
    for(int i=1;i<argc;i++) {
        std::wstring token=argv[i], low=toLowerW(token), value;
        const std::wstring prefix=L"--user-data-dir=";
        if(low.rfind(prefix,0)==0) { value=token.substr(prefix.size()); if(value.empty()){LocalFree(argv);return -1;} }
        else if(low==L"--user-data-dir") { if(i+1>=argc || argv[i+1][0]==L'-'){LocalFree(argv);return -1;} value=argv[++i]; }
        else continue;
        if(++found>1){LocalFree(argv);return -1;} out=value;
    }
    LocalFree(argv); return found;
}

struct ProcInfo {
    DWORD pid = 0;
    std::wstring imagePath;
    std::wstring cmdline;
    bool cmdlineKnown = false;
    std::wstring envLocalAppData;
    bool envKnown = false;
    bool profileMatch = false;
    bool identityConclusive = false;
    bool mainProcess = false;
};

static bool processMatchesProfile(const ProcInfo& pi,
                                  const std::wstring& profilePath,
                                  const std::wstring& defaultProfile,
                                  const std::wstring& ourLAD) {
    // [D1] A process may only ever match when its command line is readable.
    // An UNREADABLE command line is a hard "no match" -> never killed.
    if (!pi.cmdlineKnown) return false;

    std::wstring udd; int argState=extractUserDataDir(pi.cmdline, udd);
    if(argState<0) return false;
    if(argState==1) { std::wstring requested, expected; if(!fullPath(udd,requested)||!canonicalExisting(profilePath,true,expected)) return false;
        std::wstring requestFinal; if(canonicalExisting(requested,true,requestFinal)) requested=requestFinal; else requested=normalizePath(requested);
        return normalizePath(requested)==expected; }
    // No explicit --user-data-dir: the env fallback additionally requires a
    // KNOWN command line (guaranteed above), that the process inherited OUR
    // LOCALAPPDATA, and that the profile being patched is the channel default.
    if (!pi.envKnown) return false;
    if (normalizePath(pi.envLocalAppData) != normalizePath(ourLAD)) return false;
    return normalizePath(profilePath) == normalizePath(defaultProfile);
}
static bool assessProcessIdentity(ProcInfo& pi,const std::wstring& profilePath,const std::wstring& defaultProfile,const std::wstring& ourLAD){
    if(!pi.cmdlineKnown)return false;std::wstring udd;int state=extractUserDataDir(pi.cmdline,udd);if(state<0)return false;
    if(state==1){std::wstring requested,expected;if(!fullPath(udd,requested)||!canonicalExisting(profilePath,true,expected))return false;std::wstring final;if(canonicalExisting(requested,true,final))requested=final;else requested=normalizePath(requested);pi.profileMatch=normalizePath(requested)==expected;return true;}
    if(!pi.envKnown)return false;std::wstring envFull,ourFull,defaultFull,profileFull;if(!fullPath(pi.envLocalAppData,envFull)||!fullPath(ourLAD,ourFull)||!canonicalExisting(defaultProfile,true,defaultFull)||!canonicalExisting(profilePath,true,profileFull))return false;pi.profileMatch=normalizePath(envFull)==normalizePath(ourFull)&&profileFull==defaultFull;return true;
}

static std::vector<ProcInfo> enumerateChannelProcesses(const std::wstring& channel,
                                                       const std::wstring& profilePath,
                                                       const std::wstring& ourLAD) {
    std::vector<ProcInfo> res;
    std::wstring defaultProfile = ourLAD + L"\\BraveSoftware\\" + channel + L"\\User Data";
    std::string needle = lowerAscii("\\bravesoftware\\" + toUtf8(channel) + "\\");
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return res;
    PROCESSENTRY32W pe;
    std::memset(&pe, 0, sizeof(pe));
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            std::wstring img = queryProcessExePath(pe.th32ProcessID);
            if (img.empty()) continue;
            std::string p = lowerAscii(toUtf8(img));
            if (p.size() < 10 || p.compare(p.size() - 10, 10, "\\brave.exe") != 0) continue;
            if (p.find(needle) == std::string::npos) continue;
            ProcInfo pi;
            pi.pid = pe.th32ProcessID;
            pi.imagePath = img;
            getProcessInfo(pi.pid, pi.cmdline, pi.cmdlineKnown, pi.envLocalAppData, pi.envKnown);
            pi.identityConclusive=assessProcessIdentity(pi,profilePath,defaultProfile,ourLAD);
            pi.mainProcess = pi.cmdlineKnown && pi.cmdline.find(L"--type=") == std::wstring::npos;
            res.push_back(pi);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return res;
}
// --- graceful close ------------------------------------------------------
static BOOL CALLBACK enumCloseProc(HWND hwnd, LPARAM lp) {
    DWORD target = (DWORD)lp;
    DWORD wp = 0;
    GetWindowThreadProcessId(hwnd, &wp);
    if (wp == target) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    return TRUE;
}
static bool revalidateProcess(const ProcInfo& expected,const std::wstring& profilePath,const std::wstring& defaultProfile,const std::wstring& ourLAD,HANDLE h) {
    wchar_t b[32768]; DWORD n=32768; if(!QueryFullProcessImageNameW(h,0,b,&n)) return false;
    std::wstring current(b,n), a, e; if(!canonicalExisting(current,false,a)||!canonicalExisting(expected.imagePath,false,e)||a!=e) return false;
    ProcInfo fresh; fresh.pid=expected.pid; fresh.imagePath=current;
    if(!getProcessInfo(fresh.pid,fresh.cmdline,fresh.cmdlineKnown,fresh.envLocalAppData,fresh.envKnown)) return false;
    fresh.identityConclusive=assessProcessIdentity(fresh,profilePath,defaultProfile,ourLAD); return fresh.identityConclusive&&fresh.profileMatch;
}

static bool closeProcessGracefully(const ProcInfo& pi,const std::wstring& profilePath,const std::wstring& defaultProfile,const std::wstring& ourLAD,DWORD waitMs, bool& hardKilled, bool& closed) {
    hardKilled = false;
    closed = false;
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pi.pid);
    if (!h) return false;
    if(!revalidateProcess(pi,profilePath,defaultProfile,ourLAD,h)){CloseHandle(h);SetLastError(ERROR_ACCESS_DENIED);return false;}
    EnumWindows(enumCloseProc, (LPARAM)pi.pid);
    DWORD r = WaitForSingleObject(h, waitMs);
    if (r == WAIT_OBJECT_0) { closed = true; CloseHandle(h); return true; }
    if(!revalidateProcess(pi,profilePath,defaultProfile,ourLAD,h)){CloseHandle(h);SetLastError(ERROR_ACCESS_DENIED);return false;}
    if (TerminateProcess(h, 0)) {
        hardKilled = true;
        WaitForSingleObject(h, 5000);
        closed = WaitForSingleObject(h,5000)==WAIT_OBJECT_0;
        CloseHandle(h);
        return closed;
    }
    CloseHandle(h);
    return false;
}

static bool launchProcess(const std::wstring& exePath, const std::wstring& cmdline, const std::wstring& workDir) {
    std::vector<wchar_t> mutableCmd(cmdline.begin(), cmdline.end());
    mutableCmd.push_back(0);
    STARTUPINFOW si;
    std::memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    std::memset(&pi, 0, sizeof(pi));
    BOOL ok = CreateProcessW(exePath.c_str(), mutableCmd.data(), nullptr, nullptr, FALSE,
                             0, nullptr, workDir.empty() ? nullptr : workDir.c_str(), &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

// =============================================================================
// Misc
// =============================================================================
static std::wstring envW(const wchar_t* name) {
    DWORD n = GetEnvironmentVariableW(name, nullptr, 0);
    if (n == 0) return std::wstring();
    std::wstring s((size_t)n, L'\0');
    DWORD m = GetEnvironmentVariableW(name, &s[0], n);
    s.resize((size_t)m);
    return s;
}

static std::wstring dirnameOf(const std::wstring& p) {
    size_t pos = p.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return std::wstring();
    return p.substr(0, pos);
}

static std::string formatFileTime(const FILETIME& ft) {
    FILETIME lft;
    FileTimeToLocalFileTime(&ft, &lft);
    SYSTEMTIME st;
    FileTimeToSystemTime(&lft, &st);
    char buf[64];
    std::sprintf(buf, "%04d-%02d-%02d %02d:%02d:%02d",
                 (int)st.wYear, (int)st.wMonth, (int)st.wDay,
                 (int)st.wHour, (int)st.wMinute, (int)st.wSecond);
    return std::string(buf);
}

static std::wstring resolveBraveExe(const std::wstring& channel, const std::wstring& captured) {
    const wchar_t* envs[2] = { L"ProgramFiles", L"ProgramFiles(x86)" };
    for (int i = 0; i < 2; ++i) {
        std::wstring base = envW(envs[i]);
        if (base.empty()) continue;
        std::wstring p = base + L"\\BraveSoftware\\" + channel + L"\\Application\\brave.exe";
        if (GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES) return p;
    }
    if (!captured.empty() && GetFileAttributesW(captured.c_str()) != INVALID_FILE_ATTRIBUTES)
        return captured;
    return std::wstring();
}

// =============================================================================
// Verification
// =============================================================================
struct VerifyResult {
    bool parsed = false;
    bool purchase = false;   // brave.origin.purchase_validated == true AND skus.state non-empty
    bool marker67 = false;   // [D2] skus.state["67"] parses to JSON with credentials.items."6"=="7"
    size_t stateKeys = 0;
    std::string err;
    bool ok() const { return parsed && purchase && stateKeys > 0 && marker67; }
};

static VerifyResult verifyLocalState(const std::wstring& path) {
    VerifyResult vr;
    std::string data;
    if (!readFileBytes(path, data, nullptr, nullptr)) { vr.err = "read failed"; return vr; }
    JValue root;
    JParser p(data);
    if (!p.parse(root)) { vr.err = p.err; return vr; }
    vr.parsed = true;
    if (!root.isObj()) { vr.err = "root not object"; return vr; }
    JValue* vb = root.find("brave");
    if (vb && vb->isObj()) {
        JValue* vo = vb->find("origin");
        if (vo && vo->isObj()) {
            JValue* pv = vo->find("purchase_validated");
            if (pv && pv->isBool() && pv->b) {
                JValue* vs = root.find("skus");
                if (vs && vs->isObj()) {
                    JValue* st = vs->find("state");
                    if (st && st->isObj()) {
                        vr.stateKeys = st->obj.size();
                        vr.purchase = vr.stateKeys > 0;
                        // [D2] assert the "67" marker survived AND carries
                        // credentials.items."6" == "7"
                        JValue* m67 = st->find("67");
                        if (m67 && m67->isStr()) {
                            JValue inner;
                            JParser ip(m67->str);
                            if (ip.parse(inner) && inner.isObj()) {
                                JValue* cred = inner.find("credentials");
                                if (cred && cred->isObj()) {
                                    JValue* items = cred->find("items");
                                    if (items && items->isObj()) {
                                        JValue* six = items->find("6");
                                        if (six && six->isStr() && six->str == "7") vr.marker67 = true;
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    return vr;
}

// =============================================================================
// Options
// =============================================================================
struct Options {
    bool apply = false;
    bool dryRun = false;
    bool check = false;
    bool help = false;
    bool restore = false;
    std::wstring restoreBak;
    std::wstring channelFilter;
    bool parseOk = true;
    std::string err;
};

static std::vector<std::wstring> splitCommandLine(const std::wstring& cl) {
    std::vector<std::wstring> out;
    std::wstring cur;
    bool inQ = false, have = false;
    for (size_t i = 0; i < cl.size(); ++i) {
        wchar_t c = cl[i];
        if (inQ) {
            if (c == L'"') inQ = false;
            else cur.push_back(c);
        } else {
            if (c == L'"') { inQ = true; have = true; }
            else if (c == L' ' || c == L'\t') {
                if (have) { out.push_back(cur); cur.clear(); have = false; }
            } else { cur.push_back(c); have = true; }
        }
    }
    if (have) out.push_back(cur);
    return out;
}

static Options parseOptions() {
    Options o;
    LPWSTR raw = GetCommandLineW();
    std::vector<std::wstring> a = splitCommandLine(raw ? raw : L"");
    for (size_t i = 1; i < a.size(); ++i) {
        std::wstring t = a[i];
        std::wstring low = toLowerW(t);
        if (low == L"--apply")            o.apply = true;
        else if (low == L"--dry-run")     o.dryRun = true;
        else if (low == L"--check")       o.check = true;
        else if (low == L"--help" || low == L"-h" || low == L"/?") o.help = true;
        else if (low == L"--channel") {
            if (i + 1 >= a.size()) { o.parseOk = false; o.err = "--channel requires a value"; break; }
            o.channelFilter = a[++i];
        } else if (low == L"--restore") {
            if (i + 1 >= a.size()) { o.parseOk = false; o.err = "--restore requires a backup file path"; break; }
            o.restore = true;
            o.restoreBak = a[++i];
        } else {
            o.parseOk = false;
            o.err = "unknown argument: " + toUtf8(t);
            break;
        }
    }
    if (o.parseOk) {
        int modes = (o.check ? 1 : 0) + (o.apply ? 1 : 0) + (o.dryRun ? 1 : 0) + (o.restore ? 1 : 0);
        if (modes > 1) { o.parseOk = false; o.err = "--check, --apply, --dry-run, and --restore are mutually exclusive"; }
    }
    return o;
}

static void printUsage() {
    std::printf(
        "BraveOriginFix - Brave Origin purchase-validation analyser/patcher\n"
        "\n"
        "Usage: BraveOriginFix.exe [--check | --apply | --dry-run] [--channel <name>]\n"
        "                          [--restore <bakfile>] [--help]\n"
        "\n"
        "  (no args) / --check   READ-ONLY analysis (default). Nothing is changed.\n"
        "  --apply               Enable patching (kill/backup/write/relaunch).\n"
        "  --dry-run             Analysis + print planned actions; never writes.\n"
        "  --channel <name>      Limit to one channel (Brave-Origin, Brave-Origin-Beta,\n"
        "                        Brave-Origin-Nightly).\n"
        "  --restore <bakfile>   Copy a \"Local State.bak.*\" file back over Local State\n"
        "                        and re-verify.\n"
        "  --help                This text.\n"
        "\n"
        "Exit codes: 0=nothing to do, 1=patched (or BROKEN in read-only), 2=env error\n");
}
// =============================================================================
// Channel handling
// =============================================================================
struct ChannelInfo {
    std::wstring name;
    std::wstring localStatePath;
    std::wstring profilePath;      // <...>\User Data
    bool exists = false;
    bool parsed = false;
    bool ok = false;
    bool hasPurchase = false;
    bool purchaseTrue = false;
    size_t stateKeys = 0;
    ULONGLONG size = 0;
    FILETIME mtime;
    std::vector<ProcInfo> procs;
    std::string status;
    std::string parseErr;
    bool patched = false;
    bool patchOk = false;
    std::wstring backupPath;
};
static std::wstring g_lastBackupPath;
static std::wstring collisionSafeBackup(const std::wstring& target) {
    SYSTEMTIME st; GetLocalTime(&st); wchar_t ts[40]; std::wstring fixed=envW(L"BRAVEORIGINFIX_TEST_TIMESTAMP"), test=envW(L"BRAVEORIGINFIX_TEST_MODE");
    if(test==L"1" && fixed.size()==15) std::swprintf(ts,40,L".bak.%ls",fixed.c_str());
    else std::swprintf(ts,40,L".bak.%04d%02d%02d-%02d%02d%02d",(int)st.wYear,(int)st.wMonth,(int)st.wDay,(int)st.wHour,(int)st.wMinute,(int)st.wSecond);
    std::wstring base=target+ts,candidate=base; for(int n=2;GetFileAttributesW(candidate.c_str())!=INVALID_FILE_ATTRIBUTES;n++){wchar_t s[16];std::swprintf(s,16,L"_%d",n);candidate=base+s;} return candidate;
}

static std::string modeName(const Options& o) {
    if (o.dryRun) return "DRY-RUN";
    if (o.apply)  return "APPLY";
    return "CHECK (read-only)";
}

// Returns: 0 = no action, 1 = patched, -1 = error
static int doPatch(ChannelInfo& ci, const Options& o, const std::wstring& ourLAD) {
    std::string ch = toUtf8(ci.name);

    if(!o.dryRun){for(size_t i=0;i<ci.procs.size();i++)if(!ci.procs[i].identityConclusive){std::printf("  ERROR: channel process PID %lu has unreadable/ambiguous identity; mutation aborted\n",(unsigned long)ci.procs[i].pid);return -1;}
        if(processFault(L"unreadable")){std::printf("  ERROR: injected unreadable channel candidate; mutation aborted\n");return -1;}
        if(processFault(L"shutdown")){std::printf("  ERROR: injected shutdown failure; mutation aborted\n");return -1;}}

    std::vector<ProcInfo> matches;
    for (size_t i = 0; i < ci.procs.size(); ++i) if (ci.procs[i].profileMatch) matches.push_back(ci.procs[i]);

    // choose relaunch command line: prefer the main browser process
    std::wstring relaunchCmd;
    bool haveCmd = false;
    for (size_t i = 0; i < matches.size(); ++i)
        if (matches[i].cmdlineKnown && matches[i].mainProcess) { relaunchCmd = matches[i].cmdline; haveCmd = true; break; }
    if (!haveCmd)
        for (size_t i = 0; i < matches.size(); ++i)
            if (matches[i].cmdlineKnown) { relaunchCmd = matches[i].cmdline; haveCmd = true; break; }

    std::wstring capturedExe;
    if (!matches.empty()) capturedExe = matches[0].imagePath;

    std::wstring bakPath=collisionSafeBackup(ci.localStatePath);

    if (o.dryRun) {
        std::printf("  [DRY-RUN] planned actions for %s:\n", ch.c_str());
        std::printf("    - graceful-close %d matching process(es)%s\n",
                    (int)matches.size(), matches.empty() ? " (none)" : "");
        for (size_t i = 0; i < matches.size(); ++i)
            std::printf("        pid %lu%s\n", (unsigned long)matches[i].pid,
                        matches[i].mainProcess ? " (main)" : "");
        std::printf("    - backup  -> %s\n", toUtf8(bakPath).c_str());
        std::printf("    - set brave.origin.purchase_validated = true\n");
        std::printf("    - set skus.state.67 = \"{\\\"credentials\\\": {\\\"items\\\": {\\\"6\\\": \\\"7\\\"}}}\"\n");
        std::printf("    - write compact single-line sorted JSON, then re-parse + verify\n");
        if (haveCmd) std::printf("    - relaunch: %s\n", toUtf8(relaunchCmd).c_str());
        else         std::printf("    - relaunch: (no captured command line)\n");
        std::printf("  Result        : DRY-RUN (no changes written)\n");
        return 0;
    }

    std::printf("  ACTION: channel is BROKEN -> applying patch\n");

    int closedCount = 0;
    bool anyHardKill = false;
    if (!matches.empty()) {
        for (size_t i = 0; i < matches.size(); ++i) {
            bool hard = false, closed = false;
            std::wstring defaultProfile=ourLAD+L"\\BraveSoftware\\"+ci.name+L"\\User Data";
            if(!closeProcessGracefully(matches[i],ci.profilePath,defaultProfile,ourLAD,5000,hard,closed)||!closed){std::printf("    ERROR: PID %lu could not be safely closed/confirmed (err=%lu); mutation aborted\n",(unsigned long)matches[i].pid,(unsigned long)GetLastError());return -1;}
            ++closedCount; if(hard) anyHardKill=true;
        }
        std::printf("    gracefully closed %d/%d matching process(es)%s\n",
                    closedCount, (int)matches.size(), anyHardKill ? " (some needed hard-kill fallback)" : "");
    } else {
        std::printf("    WARNING: no process matched the exact profile %s\n",
                    toUtf8(ci.profilePath).c_str());
    }

    if(processFault(L"revalidation")){std::printf("    ERROR: injected PID/image revalidation change; mutation aborted\n");return -1;}
    std::vector<ProcInfo> finalCandidates=enumerateChannelProcesses(ci.name,ci.profilePath,ourLAD);for(size_t i=0;i<finalCandidates.size();i++)if(!finalCandidates[i].identityConclusive){std::printf("    ERROR: channel candidate became unidentified before mutation; aborted\n");return -1;}

    if (!CopyFileW(ci.localStatePath.c_str(), bakPath.c_str(), FALSE)) {
        std::printf("    ERROR: backup failed (err=%lu)\n", (unsigned long)GetLastError());
        return -1;
    }
    ci.backupPath = bakPath;
    g_lastBackupPath = bakPath;
    std::printf("    backup: %s\n", toUtf8(bakPath).c_str());

    std::string data;
    if (!readFileBytes(ci.localStatePath, data, nullptr, nullptr)) { std::printf("    ERROR: re-read failed\n"); return -1; }
    JValue root;
    JParser p(data);
    if (!p.parse(root) || !root.isObj()) { std::printf("    ERROR: parse failed: %s\n", p.err.c_str()); return -1; }

    JValue& brave  = root.ensureObj("brave");
    JValue& origin = brave.ensureObj("origin");
    origin.set("purchase_validated", JValue::makeBool(true));
    JValue& skus  = root.ensureObj("skus");
    JValue& state = skus.ensureObj("state");
    state.set("67", JValue::makeStr("{\"credentials\": {\"items\": {\"6\": \"7\"}}}"));

    std::string out;
    serialize(root, out);
    if (!writeFileBytesAtomic(ci.localStatePath, out)) { std::printf("    ERROR: atomic write failed (err=%lu)\n", (unsigned long)GetLastError()); return -1; }
    std::printf("    wrote %d bytes (compact, sorted keys; only 2 keys added/updated)\n", (int)out.size());

    VerifyResult v1 = verifyLocalState(ci.localStatePath);
    std::printf("    verify[immediate]: parsed=%s purchase_validated=%s skus.state keys=%d marker67=%s%s\n",
                v1.parsed ? "yes" : "no", v1.purchase ? "true" : "false",
                (int)v1.stateKeys, v1.marker67 ? "ok" : "MISSING",
                v1.err.empty() ? "" : (" err=" + v1.err).c_str());

    std::printf("    waiting 3s to let Brave's file watcher settle...\n");
    Sleep(3000);
    VerifyResult v2 = verifyLocalState(ci.localStatePath);
    std::printf("    verify[after 3s]: parsed=%s purchase_validated=%s skus.state keys=%d marker67=%s%s\n",
                v2.parsed ? "yes" : "no", v2.purchase ? "true" : "false",
                (int)v2.stateKeys, v2.marker67 ? "ok" : "MISSING",
                v2.err.empty() ? "" : (" err=" + v2.err).c_str());
    std::printf("    payload note: skus.state.67 is a COMMUNITY-BYPASS shape (not vendor-verified).\n");

    if (!v2.ok()) { std::printf("    ERROR: post-write verification failed\n"); return -1; }

    if (closedCount > 0 && o.apply) {
        std::wstring exe = resolveBraveExe(ci.name, capturedExe);
        if (exe.empty()) {
            std::printf("    WARNING: brave.exe not found for relaunch\n");
        } else {
            std::wstring cmd;
            if (haveCmd) cmd = relaunchCmd; // SAME args -> SAME profile
            else cmd = L"\"" + exe + L"\" --user-data-dir=\"" + ci.profilePath + L"\"";
            if (launchProcess(exe, cmd, dirnameOf(exe))) {
                Sleep(2500);
                std::vector<ProcInfo> after = enumerateChannelProcesses(ci.name, ci.profilePath, ourLAD);
                int alive = 0;
                for (size_t i = 0; i < after.size(); ++i) if (after[i].profileMatch) ++alive;
                if (alive > 0) std::printf("    relaunched %s (alive: %d matching process(es))\n", ch.c_str(), alive);
                else           std::printf("    WARNING: relaunch not confirmed alive\n");
            } else {
                std::printf("    WARNING: relaunch failed (err=%lu)\n", (unsigned long)GetLastError());
            }
        }
    } else if (closedCount > 0 && !o.apply) {
        std::printf("    not relaunching (--apply not given)\n");
    } else {
        std::printf("    not relaunching (no matching process was running)\n");
    }

    std::printf("  Result        : PATCHED\n");
    return 1;
}
// =============================================================================
// Restore
// =============================================================================
static int doRestore(const Options& o) {
    std::wstring bak = o.restoreBak;
    std::printf("=== BraveOriginFix --restore ===\n");
    std::printf("Backup file : %s\n", toUtf8(bak).c_str());
    if(GetFileAttributesW(bak.c_str())==INVALID_FILE_ATTRIBUTES||hasReparseComponent(bak)){std::printf("ERROR: backup missing or reparse path rejected\n");return 2;}
    std::wstring sourceCanonical,parentCanonical;if(!canonicalExisting(bak,false,sourceCanonical)||!canonicalExisting(dirnameOf(bak),true,parentCanonical)){std::printf("ERROR: canonical validation failed\n");return 2;}
    size_t slash=sourceCanonical.find_last_of(L"\\/");std::wstring base=slash==std::wstring::npos?sourceCanonical:sourceCanonical.substr(slash+1);
    if(base.rfind(L"local state.bak.",0)!=0||base.size()<=16){std::printf("ERROR: basename must match Local State.bak.*\n");return 2;}
    std::wstring lad=envW(L"LOCALAPPDATA"),channel;const wchar_t* allowed[3]={L"Brave-Origin",L"Brave-Origin-Beta",L"Brave-Origin-Nightly"};bool contained=false;
    for(int i=0;i<3;i++){std::wstring root=lad+L"\\BraveSoftware\\"+allowed[i]+L"\\User Data",canon;if(canonicalExisting(root,true,canon)&&canon==parentCanonical){contained=true;channel=allowed[i];break;}}
    if(!contained){std::printf("ERROR: backup outside allowed canonical profiles\n");return 2;}
    std::wstring target=dirnameOf(bak)+L"\\Local State",targetCanonical;if(hasReparseComponent(target)||!canonicalExisting(target,false,targetCanonical)||dirnameOf(targetCanonical)!=parentCanonical){std::printf("ERROR: target containment/identity failed\n");return 2;}
    HANDLE hs=CreateFileW(bak.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr),ht=CreateFileW(target.c_str(),FILE_READ_ATTRIBUTES,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);BY_HANDLE_FILE_INFORMATION si={},ti={};bool ids=hs!=INVALID_HANDLE_VALUE&&ht!=INVALID_HANDLE_VALUE&&GetFileInformationByHandle(hs,&si)&&GetFileInformationByHandle(ht,&ti);if(hs!=INVALID_HANDLE_VALUE)CloseHandle(hs);if(ht!=INVALID_HANDLE_VALUE)CloseHandle(ht);if(!ids||(si.dwVolumeSerialNumber==ti.dwVolumeSerialNumber&&si.nFileIndexHigh==ti.nFileIndexHigh&&si.nFileIndexLow==ti.nFileIndexLow)){std::printf("ERROR: source/target handle identity failed\n");return 2;}
    std::printf("Target      : %s\n", toUtf8(target).c_str());
    VerifyResult sourceCheck = verifyLocalState(bak);
    if (!sourceCheck.parsed) {
        std::printf("ERROR: backup is not valid JSON; target left unchanged\n");
        return 2;
    }
    if (o.dryRun) {
        std::printf("[DRY-RUN] would copy backup over target and re-verify. No changes written.\n");
        return 0;
    }
    if(processFault(L"unreadable")||processFault(L"shutdown")){std::printf("ERROR: injected unidentified/shutdown channel process; restore aborted\n");return 2;}
    std::wstring profile=dirnameOf(target);std::vector<ProcInfo> procs=enumerateChannelProcesses(channel,profile,lad);for(size_t i=0;i<procs.size();i++)if(!procs[i].identityConclusive){std::printf("ERROR: channel process PID %lu has unreadable/ambiguous identity; restore aborted\n",(unsigned long)procs[i].pid);return 2;}for(size_t i=0;i<procs.size();i++)if(procs[i].profileMatch){bool hard=false,closed=false;if(!closeProcessGracefully(procs[i],profile,profile,lad,5000,hard,closed)||!closed){std::printf("ERROR: PID %lu could not be safely closed; restore aborted\n",(unsigned long)procs[i].pid);return 2;}}
    std::vector<ProcInfo> finalCandidates=enumerateChannelProcesses(channel,profile,lad);for(size_t i=0;i<finalCandidates.size();i++)if(!finalCandidates[i].identityConclusive){std::printf("ERROR: channel candidate unidentified before restore mutation\n");return 2;}if(processFault(L"revalidation")){std::printf("ERROR: injected PID/image revalidation change; restore aborted\n");return 2;}
    FileIdentity sourceNow,targetNow;HANDLE sourceHandle=INVALID_HANDLE_VALUE,targetHandle=INVALID_HANDLE_VALUE;if(!getFileIdentity(bak,sourceNow,&sourceHandle)||!getFileIdentity(target,targetNow,&targetHandle)||!sameIdentity(sourceNow,FileIdentity{si.dwVolumeSerialNumber,si.nFileIndexHigh,si.nFileIndexLow})||!sameIdentity(targetNow,FileIdentity{ti.dwVolumeSerialNumber,ti.nFileIndexHigh,ti.nFileIndexLow})){if(sourceHandle!=INVALID_HANDLE_VALUE)CloseHandle(sourceHandle);if(targetHandle!=INVALID_HANDLE_VALUE)CloseHandle(targetHandle);std::printf("ERROR: source/target identity changed before mutation\n");return 2;}
    auto readHeld=[](HANDLE h,std::string& out){LARGE_INTEGER z={};if(!GetFileSizeEx(h,&z)||z.QuadPart<0||z.QuadPart>0x7fffffff)return false;out.resize((size_t)z.QuadPart);SetFilePointer(h,0,nullptr,FILE_BEGIN);DWORD total=0;while(total<out.size()){DWORD got=0;if(!ReadFile(h,&out[total],(DWORD)(out.size()-total),&got,nullptr)||!got)return false;total+=got;}return true;};
    std::string current,restoreBytes;if(!readHeld(targetHandle,current)||!readHeld(sourceHandle,restoreBytes)){CloseHandle(sourceHandle);CloseHandle(targetHandle);std::printf("ERROR: held source/target read failed\n");return 2;}
    std::wstring currentBak=collisionSafeBackup(target);if(!writeRawFile(currentBak,current,CREATE_NEW)){std::printf("ERROR: pre-restore backup failed\n");return 2;}
    if(!writeFileBytesAtomic(target,restoreBytes,&targetNow,&parentCanonical)){CloseHandle(sourceHandle);CloseHandle(targetHandle);std::printf("ERROR: atomic restore failed (err=%lu)\n",(unsigned long)GetLastError());return 2;}CloseHandle(sourceHandle);CloseHandle(targetHandle);
    g_lastBackupPath=currentBak;std::printf("Current target backup: %s\n",toUtf8(currentBak).c_str());
    VerifyResult v = verifyLocalState(target);
    std::printf("Restored. verify: parsed=%s purchase_validated=%s skus.state keys=%d%s\n",
                v.parsed ? "yes" : "no", v.purchase ? "true" : "false",
                (int)v.stateKeys, v.err.empty() ? "" : (" err=" + v.err).c_str());
    if (!v.parsed) { std::printf("ERROR: restored file is not valid JSON\n"); return 2; }
    if (!v.ok()) std::printf("NOTE: restored file is valid JSON but has BROKEN purchase markers (expected for a pre-patch backup)\n");
    std::printf("Restore OK (backup bytes copied and parsed)\n");
    return 0;
}

// =============================================================================
// main
// =============================================================================
static int runOptions(const Options& o) {
    if (o.restore) return doRestore(o);

    std::printf("=== BraveOriginFix ===\n");
    std::printf("Mode: %s\n", modeName(o).c_str());

    std::wstring localAppData = envW(L"LOCALAPPDATA");
    if (localAppData.empty()) {
        std::printf("FATAL: LOCALAPPDATA is not set (environmental error)\n");
        return 2;
    }
    std::printf("LOCALAPPDATA = %s\n", toUtf8(localAppData).c_str());

    const wchar_t* allChannels[3] = { L"Brave-Origin", L"Brave-Origin-Beta", L"Brave-Origin-Nightly" };
    std::vector<std::wstring> channels;
    if (!o.channelFilter.empty()) {
        bool found = false;
        for (int i = 0; i < 3; ++i) if (o.channelFilter == allChannels[i]) { found = true; break; }
        if (!found) {
            std::printf("ERROR: unknown --channel '%s'\n", toUtf8(o.channelFilter).c_str());
            return 2;
        }
        channels.push_back(o.channelFilter);
    } else {
        for (int i = 0; i < 3; ++i) channels.push_back(allChannels[i]);
    }

    std::vector<ChannelInfo> infos(channels.size());
    bool anyError = false, anyPatched = false, anyBroken = false;

    for (size_t ci = 0; ci < channels.size(); ++ci) {
        ChannelInfo& info = infos[ci];
        info.name = channels[ci];
        info.profilePath = localAppData + L"\\BraveSoftware\\" + info.name + L"\\User Data";
        info.localStatePath = info.profilePath + L"\\Local State";

        std::printf("\n[%s]\n", toUtf8(info.name).c_str());
        std::printf("  File          : %s\n", toUtf8(info.localStatePath).c_str());

        info.procs = enumerateChannelProcesses(info.name, info.profilePath, localAppData);
        int profMatches = 0;
        for (size_t i = 0; i < info.procs.size(); ++i) if (info.procs[i].profileMatch) ++profMatches;

        WIN32_FILE_ATTRIBUTE_DATA fad;
        std::memset(&fad, 0, sizeof(fad));
        bool present = GetFileAttributesExW(info.localStatePath.c_str(), GetFileExInfoStandard, &fad) != FALSE;
        if (!present) {
            info.status = "NOT-FOUND";
            std::printf("  Status        : NOT-FOUND (skipped)\n");
            std::printf("  Processes     : %d brave.exe on install path, %d matching this profile\n",
                        (int)info.procs.size(), profMatches);
            continue;
        }

        info.exists = true;
        info.size = ((ULONGLONG)fad.nFileSizeHigh << 32) | (ULONGLONG)fad.nFileSizeLow;
        info.mtime = fad.ftLastWriteTime;

        std::string data;
        if (!readFileBytes(info.localStatePath, data, nullptr, nullptr)) {
            info.status = "ERROR";
            std::printf("  Status        : ERROR (read failed, err=%lu)\n", (unsigned long)GetLastError());
            anyError = true;
            continue;
        }
        JValue root;
        JParser p(data);
        if (!p.parse(root) || !root.isObj()) {
            info.status = "ERROR";
            info.parseErr = p.err.empty() ? "root not an object" : p.err;
            std::printf("  Status        : ERROR (JSON parse: %s)\n", info.parseErr.c_str());
            std::printf("  Size          : %llu bytes\n", (unsigned long long)info.size);
            std::printf("  MTime         : %s\n", formatFileTime(info.mtime).c_str());
            std::printf("  Processes     : %d brave.exe on install path, %d matching this profile\n",
                        (int)info.procs.size(), profMatches);
            anyError = true;
            continue;
        }
        info.parsed = true;

        JValue* vb = root.find("brave");
        if (vb && vb->isObj()) {
            JValue* vo = vb->find("origin");
            if (vo && vo->isObj()) {
                JValue* pv = vo->find("purchase_validated");
                if (pv) { info.hasPurchase = true; info.purchaseTrue = (pv->isBool() && pv->b); }
            }
        }
        JValue* vs = root.find("skus");
        if (vs && vs->isObj()) {
            JValue* vst = vs->find("state");
            if (vst && vst->isObj()) info.stateKeys = vst->obj.size();
        }

        info.ok = info.purchaseTrue && (info.stateKeys > 0);
        info.status = info.ok ? "OK" : "BROKEN";

        std::printf("  Status        : %s\n", info.status.c_str());
        std::printf("  Size          : %llu bytes\n", (unsigned long long)info.size);
        std::printf("  MTime         : %s\n", formatFileTime(info.mtime).c_str());
        std::printf("  purchase_validated : %s%s\n",
                    info.hasPurchase ? (info.purchaseTrue ? "true" : "false") : "absent",
                    info.hasPurchase ? "" : " (key missing)");
        std::printf("  skus.state keys    : %d\n", (int)info.stateKeys);
        std::printf("  Processes     : %d brave.exe on install path, %d matching this profile\n",
                    (int)info.procs.size(), profMatches);
        for (size_t i = 0; i < info.procs.size(); ++i) {
            const ProcInfo& pi = info.procs[i];
            std::printf("      pid %-6lu match=%s cmdline=%s%s\n",
                        (unsigned long)pi.pid,
                        pi.profileMatch ? "YES" : "no ",
                        pi.cmdlineKnown ? "known" : "UNREADABLE",
                        pi.mainProcess ? " (main)" : "");
        }

        if (info.ok) {
            std::printf("  Result        : OK (no changes written)\n");
            continue;
        }

        anyBroken = true;
        if (o.apply && !o.dryRun) {
            int r = doPatch(info, o, localAppData);
            if (r == 1) { info.patched = true; info.patchOk = true; anyPatched = true; info.status = "PATCHED"; }
            else if (r < 0) { anyError = true; std::printf("  Result        : PATCH FAILED\n"); }
        } else if (o.dryRun) {
            doPatch(info, o, localAppData);
        } else {
            std::printf("  Result        : BROKEN (read-only; re-run with --apply to patch)\n");
        }
    }

    std::printf("\n=== SUMMARY ===\n");
    std::printf("Mode: %s\n", modeName(o).c_str());
    for (size_t i = 0; i < infos.size(); ++i) {
        std::string label = toUtf8(infos[i].name);
        while (label.size() < 20) label.push_back(' ');
        std::printf("  %s: %s\n", label.c_str(), infos[i].status.c_str());
    }
    for (size_t i = 0; i < infos.size(); ++i) {
        if (!infos[i].backupPath.empty())
            std::printf("  Backup (%s): %s\n", toUtf8(infos[i].name).c_str(), toUtf8(infos[i].backupPath).c_str());
    }
    int exitCode;
    if (anyError)        exitCode = 2;
    else if (anyPatched) exitCode = 1;
    else if (anyBroken)  exitCode = 1;
    else                 exitCode = 0;

    if (exitCode == 0)      std::printf("Result: nothing to do (all channels OK / not present)\n");
    else if (anyPatched)    std::printf("Result: at least one channel was patched\n");
    else if (anyBroken)     std::printf("Result: BROKEN channel(s) detected but not patched (read-only/dry-run)\n");
    else                    std::printf("Result: environmental error encountered\n");
    std::printf("Exit code: %d\n", exitCode);
    return exitCode;
}

int EngineRunCli() {
    SetConsoleOutputCP(CP_UTF8);
    std::vector<std::wstring> raw=splitCommandLine(GetCommandLineW());
    if(raw.size()==2 && raw[1]==L"--security-self-test") {
        struct Case{const char* json;bool valid;};Case cases[]={{"+1",false},{"01",false},{"1.",false},{"{\"x\":\"a\n\"}",false},{"\"\\uD800\"",false},{"\"\\uDC00\"",false},{"{\"brave\":{},\"brave\":{}}",false},{"-1.25e+2",true}};bool parserOk=true;
        for(size_t n=0;n<sizeof(cases)/sizeof(cases[0]);n++){std::string text=cases[n].json;JValue v;JParser p(text);if(p.parse(v)!=cases[n].valid)parserOk=false;}
        std::vector<std::string> badUtf8={std::string("\"\x80\"",3),std::string("\"\xC0\xAF\"",4),std::string("\"\xE2\x28\xA1\"",5),std::string("\"\xED\xA0\x80\"",5),std::string("\"\xF4\x90\x80\x80\"",6),std::string("\"\xE2\x82\"",4)};
        for(size_t n=0;n<badUtf8.size();n++){JValue v;JParser p(badUtf8[n]);if(p.parse(v))parserOk=false;}
        std::wstring value;bool argOk=extractUserDataDir(L"brave.exe --user-data-dir=\"C:\\One Two\"",value)==1&&value==L"C:\\One Two";
        argOk=argOk&&extractUserDataDir(L"brave.exe --user-data-dir \"C:\\One Two\"",value)==1&&extractUserDataDir(L"brave.exe --foo=--user-data-dir=C:\\decoy",value)==0&&extractUserDataDir(L"brave.exe --user-data-dir=A --user-data-dir=B",value)==-1;
        bool mismatch=normalizePath(L"C:\\profile-a")!=normalizePath(L"C:\\profile-b");
        std::printf("parser_rfc8259_utf8=%s exact_switch_and_pair=%s decoy_ambiguous_mismatch_safe=%s\n",parserOk?"PASS":"FAIL",argOk?"PASS":"FAIL",argOk&&mismatch?"PASS":"FAIL");return parserOk&&argOk&&mismatch?0:2;
    }
    Options o = parseOptions();
    if (o.help) { printUsage(); return 0; }
    if (!o.parseOk) {
        std::printf("ERROR: %s\n\n", o.err.c_str());
        printUsage();
        return 2;
    }
    return runOptions(o);
}

EngineChannelSnapshot EngineScanChannel(const std::wstring& channel) {
    EngineChannelSnapshot s;
    s.channel = channel;
    std::wstring lad = envW(L"LOCALAPPDATA");
    s.profilePath = lad + L"\\BraveSoftware\\" + channel + L"\\User Data";
    s.localStatePath = s.profilePath + L"\\Local State";
    WIN32_FILE_ATTRIBUTE_DATA fad = {};
    if (lad.empty()) { s.status = EngineStatus::Error; s.detail = L"LOCALAPPDATA is not set"; return s; }
    if (!GetFileAttributesExW(s.localStatePath.c_str(), GetFileExInfoStandard, &fad)) {
        s.status = EngineStatus::NotInstalled; s.detail = L"Local State was not found"; return s;
    }
    s.exists = true;
    s.size = ((unsigned long long)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
    s.lastWrite = fad.ftLastWriteTime;
    std::string data;
    if (!readFileBytes(s.localStatePath, data, nullptr, nullptr)) {
        s.status = EngineStatus::Error; s.detail = L"Unable to read Local State"; return s;
    }
    JValue root; JParser p(data);
    if (!p.parse(root) || !root.isObj()) {
        s.status = EngineStatus::Error; s.detail = L"Malformed JSON: " + std::wstring(p.err.begin(), p.err.end()); return s;
    }
    bool purchase = false; size_t keys = 0;
    JValue* brave = root.find("brave");
    if (brave && brave->isObj()) { JValue* origin = brave->find("origin"); if (origin && origin->isObj()) { JValue* pv = origin->find("purchase_validated"); purchase = pv && pv->isBool() && pv->b; } }
    JValue* skus = root.find("skus");
    if (skus && skus->isObj()) { JValue* state = skus->find("state"); if (state && state->isObj()) keys = state->obj.size(); }
    s.status = purchase && keys ? EngineStatus::Ready : EngineStatus::NeedsRepair;
    s.detail = s.status == EngineStatus::Ready ? L"Required local-state shape is present" : L"Required local-state shape is incomplete";
    return s;
}

int EngineApplyChannel(const std::wstring& channel, std::wstring* backupPath) {
    Options o; o.apply = true; o.channelFilter = channel;
    g_lastBackupPath.clear();
    int rc = runOptions(o);
    if (backupPath) *backupPath=g_lastBackupPath;
    return rc;
}

int EngineRestoreBackup(const std::wstring& backupPath, std::wstring* currentBackupPath) {
    g_lastBackupPath.clear(); Options o; o.restore = true; o.restoreBak = backupPath; int rc=runOptions(o); if(currentBackupPath)*currentBackupPath=g_lastBackupPath; return rc;
}
