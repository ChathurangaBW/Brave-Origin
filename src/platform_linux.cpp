// platform_linux.cpp
// -----------------------------------------------------------------------------
// Linux port of the Brave Origin local-state repair engine (CLI only).
//
//   Channel root : $HOME/.config/BraveSoftware/<channel>/User Data/Local State
//   OK           : brave.origin.purchase_validated == true
//                  AND skus.state is a non-empty object
//
// MODES (default is READ-ONLY; nothing is signalled, written or launched):
//   (no args) / --check       read-only analysis
//   --dry-run                 analysis + print planned actions, never writes
//   --apply                   enables patching (signal/backup/write/relaunch)
//   --channel <name>          limit to one channel
//   --restore <bakfile>       restore a "Local State.bak.*" file and re-verify
//   --help                    usage
//
// SAFETY (identical semantics to the Windows engine):
//   * Process signalling is scoped to the EXACT profile being patched:
//       Brave executable (basename brave/brave-browser/brave-bin, or an exe
//       path under BraveSoftware/<channel>)
//       AND ( --user-data-dir == profile path (exact token match)
//             OR (no --user-data-dir AND the process inherited OUR $HOME
//                 AND the profile being patched is that channel's default) )
//     A process whose /proc cmdline cannot be read is NEVER signalled
//     (cmdlineKnown fail-closed rule); ambiguous/duplicate switches also fail
//     closed. PID/exe/profile identity is revalidated immediately before
//     SIGTERM and before the SIGKILL fallback.
//   * Graceful SIGTERM first (5s), SIGKILL only as a fallback.
//   * Relaunch reuses the killed process's ORIGINAL argv.
//   * Writes use a same-directory temp file (write + fsync + re-parse) and an
//     atomic rename; the parent directory is fsynced after rename.
//
// PAYLOAD NOTE (documented assumption, same as Windows):
//   The skus.state["67"] value is a *community-bypass* shape. It is NOT
//   vendor-verified. Only the two marker keys are added/updated; every other
//   key is preserved. Post-write the file is re-parsed and verified both
//   immediately and again after a 3s settle delay.
//
// Build (inside WSL):
//   g++ -std=c++17 -O2 -Wall -Wextra -Isrc src/platform_linux.cpp
//       src/cli_linux.cpp -o dist/brave-origin-fix-linux
//
// Exit codes: 0 = nothing broken / nothing to do
//             1 = at least one channel patched (or BROKEN detected read-only)
//             2 = environmental error
// -----------------------------------------------------------------------------

#include "core_json.h"
#include "platform_linux.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

// =============================================================================
// Small helpers
// =============================================================================
static std::string envStr(const char* name) {
    const char* v = ::getenv(name);
    return v ? std::string(v) : std::string();
}

static std::string lowerAscii(std::string s) {
    for (size_t i = 0; i < s.size(); ++i)
        if (s[i] >= 'A' && s[i] <= 'Z') s[i] = (char)(s[i] + 32);
    return s;
}

static std::string normalizeLinux(std::string s) {
    while (s.size() > 1 && s[s.size() - 1] == '/') s.resize(s.size() - 1);
    return s;
}

static std::string dirnameOf(const std::string& p) {
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return std::string();
    if (pos == 0) return std::string("/");
    return p.substr(0, pos);
}

static std::string basenameOf(const std::string& p) {
    size_t pos = p.find_last_of('/');
    if (pos == std::string::npos) return p;
    return p.substr(pos + 1);
}

static bool testHook(const char* name) {
    return envStr("BRAVEORIGINFIX_TEST_MODE") == "1" && envStr(name) == "1";
}

static bool processFault(const char* value) {
    return envStr("BRAVEORIGINFIX_TEST_MODE") == "1" &&
           envStr("BRAVEORIGINFIX_TEST_PROCESS_FAULT") == value;
}

static unsigned long long tickMs() {
    struct timespec ts;
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000ull + (unsigned long long)ts.tv_nsec / 1000000ull;
}

static std::string formatMTime(time_t t) {
    struct tm lt;
    ::localtime_r(&t, &lt);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                  lt.tm_hour, lt.tm_min, lt.tm_sec);
    return std::string(buf);
}

// =============================================================================
// File helpers (POSIX)
// =============================================================================
static bool readFileBytes(const std::string& path, std::string& out,
                          unsigned long long* sizeOut = nullptr,
                          time_t* mtimeOut = nullptr) {
    int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    struct stat st;
    if (::fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) { ::close(fd); return false; }
    out.clear();
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        out.append(buf, (size_t)n);
    }
    if (sizeOut) *sizeOut = (unsigned long long)out.size();
    if (mtimeOut) *mtimeOut = st.st_mtime;
    ::close(fd);
    return true;
}

static bool fullPath(const std::string& path, std::string& out) {
    if (path.empty()) return false;
    if (path[0] == '/') { out = normalizeLinux(path); return true; }
    char cwd[PATH_MAX];
    if (!::getcwd(cwd, sizeof(cwd))) return false;
    out = normalizeLinux(std::string(cwd) + "/" + path);
    return true;
}

// True when any component of the path is (or traverses) a symlink.
static bool hasSymlinkComponent(const std::string& path) {
    std::string abs;
    if (!fullPath(path, abs)) return true;
    // Walk each prefix: "/", "/a", "/a/b", ...
    std::string prefix;
    size_t i = 0;
    if (!abs.empty() && abs[0] == '/') { prefix = "/"; i = 1; }
    while (true) {
        size_t slash = abs.find('/', i);
        std::string part = (slash == std::string::npos) ? abs : abs.substr(0, slash);
        if (!part.empty()) {
            struct stat lst;
            if (::lstat(part.c_str(), &lst) == 0 && S_ISLNK(lst.st_mode)) return true;
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
        if (i >= abs.size()) break;
    }
    (void)prefix;
    return false;
}

static bool canonicalExisting(const std::string& path, bool directory, std::string& out) {
    if (hasSymlinkComponent(path)) return false;
    char resolved[PATH_MAX];
    if (!::realpath(path.c_str(), resolved)) return false;
    struct stat st;
    if (::stat(resolved, &st) != 0) return false;
    if (directory != (bool)S_ISDIR(st.st_mode)) return false;
    out = normalizeLinux(std::string(resolved));
    return true;
}

static bool writeRawFile(const std::string& path, const std::string& data, bool exclusive) {
    int flags = O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC);
    int fd = ::open(path.c_str(), flags, 0600);
    if (fd < 0) return false;
    size_t total = 0;
    bool writeOk = true;
    while (total < data.size()) {
        ssize_t n = ::write(fd, data.data() + total, data.size() - total);
        if (n < 0) {
            if (errno == EINTR) continue;
            writeOk = false;
            break;
        }
        if (n == 0) { writeOk = false; break; }
        total += (size_t)n;
    }
    bool flushOk = false;
    if (writeOk) flushOk = (::fsync(fd) == 0);
    if (testHook("BRAVEORIGINFIX_TEST_FLUSH_FAIL") &&
        path.find(".tmp.") != std::string::npos) {
        errno = EIO;
        flushOk = false;
    }
    bool closeOk = (::close(fd) == 0);
    return writeOk && flushOk && closeOk;
}

struct FileIdentity {
    dev_t dev = 0;
    ino_t ino = 0;
};

static bool getFileIdentity(const std::string& path, FileIdentity& id, bool directory = false) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0) return false;
    if (directory != (bool)S_ISDIR(st.st_mode)) return false;
    id.dev = st.st_dev;
    id.ino = st.st_ino;
    return true;
}

static bool sameIdentity(const FileIdentity& a, const FileIdentity& b) {
    return a.dev == b.dev && a.ino == b.ino;
}

static bool fsyncParentDir(const std::string& path) {
    std::string dir = dirnameOf(path);
    if (dir.empty()) return true;
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    bool ok = (::fsync(fd) == 0);
    ::close(fd);
    return ok;
}

static bool writeFileBytesAtomic(const std::string& path, const std::string& data,
                                 const FileIdentity* expectedTarget = nullptr,
                                 const std::string* expectedParent = nullptr) {
    if (hasSymlinkComponent(path) || hasSymlinkComponent(dirnameOf(path))) {
        errno = ELOOP;
        return false;
    }
    char suffix[96];
    std::snprintf(suffix, sizeof(suffix), ".tmp.%ld.%llu",
                  (long)::getpid(), tickMs());
    std::string tmp = path + suffix;
    if (!writeRawFile(tmp, data, true)) { ::unlink(tmp.c_str()); return false; }
    std::string tempBytes;
    JValue parsed;
    if (!readFileBytes(tmp, tempBytes)) { ::unlink(tmp.c_str()); return false; }
    JParser parser(tempBytes);
    if (!parser.parse(parsed)) { ::unlink(tmp.c_str()); errno = EINVAL; return false; }
    if (expectedTarget) {
        FileIdentity now;
        std::string parent;
        if (testHook("BRAVEORIGINFIX_TEST_RESTORE_RACE") ||
            !getFileIdentity(path, now) || !sameIdentity(*expectedTarget, now) ||
            !canonicalExisting(dirnameOf(path), true, parent) ||
            (expectedParent && parent != *expectedParent) ||
            hasSymlinkComponent(path)) {
            ::unlink(tmp.c_str());
            errno = EINVAL;
            return false;
        }
    }
    if (::rename(tmp.c_str(), path.c_str()) != 0) { ::unlink(tmp.c_str()); return false; }
    fsyncParentDir(path);
    return true;
}

// =============================================================================
// Process helpers (/proc)
// =============================================================================
static bool readProcFile(pid_t pid, const char* kind, std::string& raw) {
    char name[64];
    std::snprintf(name, sizeof(name), "/proc/%ld/%s", (long)pid, kind);
    int fd = ::open(name, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return false;
    raw.clear();
    char buf[65536];
    for (;;) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            ::close(fd);
            return false;
        }
        if (n == 0) break;
        raw.append(buf, (size_t)n);
    }
    ::close(fd);
    return true;
}

static std::vector<std::string> splitNul(const std::string& raw) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < raw.size()) {
        size_t e = raw.find('\0', i);
        if (e == std::string::npos) e = raw.size();
        out.push_back(raw.substr(i, e - i));
        i = e + 1;
    }
    while (!out.empty() && out.back().empty()) out.pop_back();
    return out;
}

static bool readProcExe(pid_t pid, std::string& exe) {
    char name[64];
    std::snprintf(name, sizeof(name), "/proc/%ld/exe", (long)pid);
    char buf[PATH_MAX];
    ssize_t n = ::readlink(name, buf, sizeof(buf) - 1);
    if (n < 0) return false;
    buf[n] = '\0';
    exe.assign(buf, (size_t)n);
    return true;
}

// Return 1 for one exact switch, 0 for none, -1 for ambiguous/malformed.
static int extractUserDataDir(const std::vector<std::string>& args, std::string& out) {
    static const std::string kPrefix = "--user-data-dir=";
    int found = 0;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& token = args[i];
        std::string low = lowerAscii(token);
        std::string value;
        bool isSwitch = false;
        if (low.compare(0, kPrefix.size(), kPrefix) == 0) {
            value = token.substr(kPrefix.size());
            if (value.empty()) return -1;
            isSwitch = true;
        } else if (low == "--user-data-dir") {
            if (i + 1 >= args.size()) return -1;
            value = args[++i];
            if (value.empty() || value[0] == '-') return -1;
            isSwitch = true;
        }
        if (!isSwitch) continue;
        if (++found > 1) return -1;
        out = value;
    }
    return found;
}

struct ProcInfo {
    pid_t pid = 0;
    std::string exePath;
    bool exeKnown = false;
    std::vector<std::string> args;
    bool cmdlineKnown = false;
    std::string envHome;
    bool envKnown = false;
    bool profileMatch = false;
    bool identityConclusive = false;
    bool mainProcess = false;
};

static bool isBraveExe(const std::string& exePath, const std::string& channelLower) {
    std::string low = lowerAscii(exePath);
    std::string base = lowerAscii(basenameOf(exePath));
    if (base == "brave" || base == "brave-browser" || base == "brave-bin") return true;
    return low.find("/bravesoftware/" + channelLower + "/") != std::string::npos;
}

static bool assessProcessIdentity(ProcInfo& pi,
                                  const std::string& profilePath,
                                  const std::string& defaultProfile,
                                  const std::string& ourHome) {
    if (!pi.cmdlineKnown) return false;
    std::string udd;
    int state = extractUserDataDir(pi.args, udd);
    if (state < 0) return false;
    if (state == 1) {
        std::string requested, expected;
        if (!fullPath(udd, requested)) return false;
        if (!canonicalExisting(profilePath, true, expected)) return false;
        std::string finalPath;
        if (canonicalExisting(requested, true, finalPath)) requested = finalPath;
        else requested = normalizeLinux(requested);
        pi.profileMatch = (normalizeLinux(requested) == expected);
        return true;
    }
    if (!pi.envKnown) return false;
    std::string envFull, ourFull, defaultFull, profileFull;
    if (!fullPath(pi.envHome, envFull)) return false;
    if (!fullPath(ourHome, ourFull)) return false;
    if (!canonicalExisting(defaultProfile, true, defaultFull)) return false;
    if (!canonicalExisting(profilePath, true, profileFull)) return false;
    pi.profileMatch = (normalizeLinux(envFull) == normalizeLinux(ourFull) &&
                       profileFull == defaultFull);
    return true;
}

static bool refreshProcInfo(ProcInfo& pi) {
    std::string raw;
    pi.cmdlineKnown = false;
    pi.args.clear();
    if (readProcFile(pi.pid, "cmdline", raw)) {
        std::vector<std::string> args = splitNul(raw);
        if (!args.empty()) {
            pi.args = std::move(args);
            pi.cmdlineKnown = true;
        }
    }
    std::string envRaw;
    pi.envKnown = false;
    pi.envHome.clear();
    if (readProcFile(pi.pid, "environ", envRaw)) {
        std::vector<std::string> vars = splitNul(envRaw);
        for (size_t i = 0; i < vars.size(); ++i) {
            if (vars[i].compare(0, 5, "HOME=") == 0) {
                pi.envHome = vars[i].substr(5);
                pi.envKnown = true;
                break;
            }
        }
    }
    std::string exe;
    pi.exeKnown = readProcExe(pi.pid, exe);
    if (pi.exeKnown) pi.exePath = exe;
    bool main = false;
    if (pi.cmdlineKnown) {
        main = true;
        for (size_t i = 1; i < pi.args.size(); ++i) {
            if (pi.args[i].compare(0, 7, "--type=") == 0) { main = false; break; }
        }
    }
    pi.mainProcess = main;
    return pi.exeKnown;
}

static std::vector<ProcInfo> enumerateChannelProcesses(const std::string& channel,
                                                       const std::string& profilePath,
                                                       const std::string& ourHome) {
    std::vector<ProcInfo> res;
    std::string defaultProfile = ourHome + "/.config/BraveSoftware/" + channel + "/User Data";
    std::string channelLower = lowerAscii(channel);
    DIR* dir = ::opendir("/proc");
    if (!dir) return res;
    while (struct dirent* de = ::readdir(dir)) {
        const char* n = de->d_name;
        if (!n[0]) continue;
        bool numeric = true;
        for (const char* p = n; *p; ++p)
            if (*p < '0' || *p > '9') { numeric = false; break; }
        if (!numeric) continue;
        ProcInfo pi;
        pi.pid = (pid_t)::atol(n);
        if (pi.pid <= 0 || pi.pid == ::getpid()) continue;
        if (!refreshProcInfo(pi)) continue;
        if (!isBraveExe(pi.exePath, channelLower)) continue;
        pi.identityConclusive = assessProcessIdentity(pi, profilePath, defaultProfile, ourHome);
        res.push_back(pi);
    }
    ::closedir(dir);
    return res;
}

static bool processAlive(pid_t pid) {
    if (::kill(pid, 0) == 0) return true;
    return errno != ESRCH;
}

static bool revalidateProcess(const ProcInfo& expected,
                              const std::string& profilePath,
                              const std::string& defaultProfile,
                              const std::string& ourHome) {
    std::string exe;
    if (!readProcExe(expected.pid, exe)) return false;
    std::string a, e;
    if (!canonicalExisting(exe, false, a)) return false;
    if (!canonicalExisting(expected.exePath, false, e)) return false;
    if (a != e) return false;
    ProcInfo fresh;
    fresh.pid = expected.pid;
    fresh.exePath = expected.exePath;
    fresh.exeKnown = true;
    if (!refreshProcInfo(fresh)) return false;
    std::string fa, fe;
    if (!canonicalExisting(fresh.exePath, false, fa)) return false;
    if (!canonicalExisting(expected.exePath, false, fe)) return false;
    if (fa != fe) return false;
    fresh.identityConclusive = assessProcessIdentity(fresh, profilePath, defaultProfile, ourHome);
    return fresh.identityConclusive && fresh.profileMatch;
}

static bool closeProcessGracefully(const ProcInfo& pi,
                                   const std::string& profilePath,
                                   const std::string& defaultProfile,
                                   const std::string& ourHome,
                                   unsigned waitMs,
                                   bool& hardKilled, bool& closed) {
    hardKilled = false;
    closed = false;
    if (!processAlive(pi.pid)) { closed = true; return true; }
    if (!revalidateProcess(pi, profilePath, defaultProfile, ourHome)) {
        errno = EACCES;
        return false;
    }
    ::kill(pi.pid, SIGTERM);
    unsigned waited = 0;
    while (waited < waitMs) {
        if (!processAlive(pi.pid)) { closed = true; return true; }
        ::usleep(100 * 1000);
        waited += 100;
    }
    if (!processAlive(pi.pid)) { closed = true; return true; }
    if (!revalidateProcess(pi, profilePath, defaultProfile, ourHome)) {
        errno = EACCES;
        return false;
    }
    if (::kill(pi.pid, SIGKILL) == 0) {
        hardKilled = true;
        for (unsigned i = 0; i < 50 && processAlive(pi.pid); ++i) ::usleep(100 * 1000);
        // Reap zombie children so "alive" reflects real exit.
        ::waitpid(pi.pid, nullptr, WNOHANG);
        for (unsigned i = 0; i < 50 && processAlive(pi.pid); ++i) ::usleep(100 * 1000);
        closed = !processAlive(pi.pid);
        return closed;
    }
    return false;
}

static std::string resolveBraveExe(const std::string& channel, const std::string& captured) {
    (void)channel;
    if (!captured.empty()) {
        struct stat st;
        if (::stat(captured.c_str(), &st) == 0 && ::access(captured.c_str(), X_OK) == 0)
            return captured;
    }
    static const char* kCandidates[] = {
        "/opt/brave.com/brave/brave",
        "/usr/bin/brave-browser",
        "/usr/bin/brave-browser-stable",
        "/usr/bin/brave",
        nullptr
    };
    for (int i = 0; kCandidates[i]; ++i) {
        if (::access(kCandidates[i], X_OK) == 0) return std::string(kCandidates[i]);
    }
    return std::string();
}

static bool launchProcess(const std::vector<std::string>& argv, const std::string& workDir) {
    if (argv.empty()) return false;
    pid_t c = ::fork();
    if (c < 0) return false;
    if (c == 0) {
        ::setsid();
        if (!workDir.empty()) (void)::chdir(workDir.c_str());
        std::vector<char*> execArgs;
        for (size_t i = 0; i < argv.size(); ++i)
            execArgs.push_back(const_cast<char*>(argv[i].c_str()));
        execArgs.push_back(nullptr);
        ::execv(execArgs[0], execArgs.data());
        ::_exit(127);
    }
    return true;
}

// =============================================================================
// Verification
// =============================================================================
struct VerifyResult {
    bool parsed = false;
    bool purchase = false;   // brave.origin.purchase_validated == true AND skus.state non-empty
    bool marker67 = false;   // skus.state["67"] parses to JSON with credentials.items."6"=="7"
    size_t stateKeys = 0;
    std::string err;
    bool ok() const { return parsed && purchase && stateKeys > 0 && marker67; }
};

static VerifyResult verifyLocalState(const std::string& path) {
    VerifyResult vr;
    std::string data;
    if (!readFileBytes(path, data)) { vr.err = "read failed"; return vr; }
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
    std::string restoreBak;
    std::string channelFilter;
    bool parseOk = true;
    std::string err;
};

static Options parseOptions(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        std::string t = argv[i] ? argv[i] : "";
        std::string low = lowerAscii(t);
        if (low == "--apply")            o.apply = true;
        else if (low == "--dry-run")     o.dryRun = true;
        else if (low == "--check")       o.check = true;
        else if (low == "--help" || low == "-h" || low == "/?") o.help = true;
        else if (low == "--channel") {
            if (i + 1 >= argc) { o.parseOk = false; o.err = "--channel requires a value"; break; }
            o.channelFilter = argv[++i];
        } else if (low == "--restore") {
            if (i + 1 >= argc) { o.parseOk = false; o.err = "--restore requires a backup file path"; break; }
            o.restore = true;
            o.restoreBak = argv[++i];
        } else {
            o.parseOk = false;
            o.err = "unknown argument: " + t;
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
        "BraveOriginFix - Brave Origin purchase-validation analyser/patcher (Linux)\n"
        "\n"
        "Usage: brave-origin-fix-linux [--check | --apply | --dry-run] [--channel <name>]\n"
        "                              [--restore <bakfile>] [--help]\n"
        "\n"
        "  (no args) / --check   READ-ONLY analysis (default). Nothing is changed.\n"
        "  --apply               Enable patching (signal/backup/write/relaunch).\n"
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
    std::string name;
    std::string localStatePath;
    std::string profilePath;      // <...>/User Data
    bool exists = false;
    bool parsed = false;
    bool ok = false;
    bool hasPurchase = false;
    bool purchaseTrue = false;
    size_t stateKeys = 0;
    unsigned long long size = 0;
    time_t mtime = 0;
    std::vector<ProcInfo> procs;
    std::string status;
    std::string parseErr;
    bool patched = false;
    bool patchOk = false;
    std::string backupPath;
};

static std::string g_lastBackupPath;

static std::string collisionSafeBackup(const std::string& target) {
    std::string fixed = envStr("BRAVEORIGINFIX_TEST_TIMESTAMP");
    std::string test = envStr("BRAVEORIGINFIX_TEST_MODE");
    char ts[48];
    if (test == "1" && fixed.size() == 15) {
        std::snprintf(ts, sizeof(ts), ".bak.%s", fixed.c_str());
    } else {
        time_t now = ::time(nullptr);
        struct tm lt;
        ::localtime_r(&now, &lt);
        std::snprintf(ts, sizeof(ts), ".bak.%04d%02d%02d-%02d%02d%02d",
                      lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday,
                      lt.tm_hour, lt.tm_min, lt.tm_sec);
    }
    std::string base = target + ts;
    std::string candidate = base;
    struct stat st;
    for (int n = 2; ::stat(candidate.c_str(), &st) == 0; ++n) {
        char s[16];
        std::snprintf(s, sizeof(s), "_%d", n);
        candidate = base + s;
    }
    return candidate;
}

static std::string modeName(const Options& o) {
    if (o.dryRun) return "DRY-RUN";
    if (o.apply)  return "APPLY";
    return "CHECK (read-only)";
}

static std::string joinArgv(const std::vector<std::string>& argv) {
    std::string out;
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) out.push_back(' ');
        bool quote = argv[i].find(' ') != std::string::npos || argv[i].empty();
        if (quote) out.push_back('"');
        out += argv[i];
        if (quote) out.push_back('"');
    }
    return out;
}

// Returns: 0 = no action, 1 = patched, -1 = error
static int doPatch(ChannelInfo& ci, const Options& o, const std::string& ourHome) {
    const std::string& ch = ci.name;

    if (!o.dryRun) {
        for (size_t i = 0; i < ci.procs.size(); ++i) {
            if (!ci.procs[i].identityConclusive) {
                std::printf("  ERROR: channel process PID %ld has unreadable/ambiguous identity; mutation aborted\n",
                            (long)ci.procs[i].pid);
                return -1;
            }
        }
        if (processFault("unreadable")) { std::printf("  ERROR: injected unreadable channel candidate; mutation aborted\n"); return -1; }
        if (processFault("shutdown"))   { std::printf("  ERROR: injected shutdown failure; mutation aborted\n"); return -1; }
    }

    std::vector<ProcInfo> matches;
    for (size_t i = 0; i < ci.procs.size(); ++i)
        if (ci.procs[i].profileMatch) matches.push_back(ci.procs[i]);

    std::vector<std::string> relaunchArgv;
    bool haveCmd = false;
    for (size_t i = 0; i < matches.size(); ++i)
        if (matches[i].cmdlineKnown && matches[i].mainProcess) { relaunchArgv = matches[i].args; haveCmd = true; break; }
    if (!haveCmd)
        for (size_t i = 0; i < matches.size(); ++i)
            if (matches[i].cmdlineKnown) { relaunchArgv = matches[i].args; haveCmd = true; break; }

    std::string capturedExe;
    if (!matches.empty()) capturedExe = matches[0].exePath;

    std::string bakPath = collisionSafeBackup(ci.localStatePath);

    if (o.dryRun) {
        std::printf("  [DRY-RUN] planned actions for %s:\n", ch.c_str());
        std::printf("    - graceful-close %d matching process(es)%s\n",
                    (int)matches.size(), matches.empty() ? " (none)" : "");
        for (size_t i = 0; i < matches.size(); ++i)
            std::printf("        pid %ld%s\n", (long)matches[i].pid,
                        matches[i].mainProcess ? " (main)" : "");
        std::printf("    - backup  -> %s\n", bakPath.c_str());
        std::printf("    - set brave.origin.purchase_validated = true\n");
        std::printf("    - set skus.state.67 = \"{\\\"credentials\\\": {\\\"items\\\": {\\\"6\\\": \\\"7\\\"}}}\"\n");
        std::printf("    - write compact single-line sorted JSON, then re-parse + verify\n");
        if (haveCmd) std::printf("    - relaunch: %s\n", joinArgv(relaunchArgv).c_str());
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
            std::string defaultProfile = ourHome + "/.config/BraveSoftware/" + ci.name + "/User Data";
            if (!closeProcessGracefully(matches[i], ci.profilePath, defaultProfile, ourHome, 5000, hard, closed) || !closed) {
                std::printf("    ERROR: PID %ld could not be safely closed/confirmed (err=%d); mutation aborted\n",
                            (long)matches[i].pid, errno);
                return -1;
            }
            ++closedCount;
            if (hard) anyHardKill = true;
        }
        std::printf("    gracefully closed %d/%d matching process(es)%s\n",
                    closedCount, (int)matches.size(), anyHardKill ? " (some needed hard-kill fallback)" : "");
    } else {
        std::printf("    WARNING: no process matched the exact profile %s\n", ci.profilePath.c_str());
    }

    if (processFault("revalidation")) { std::printf("    ERROR: injected PID/image revalidation change; mutation aborted\n"); return -1; }
    std::vector<ProcInfo> finalCandidates = enumerateChannelProcesses(ci.name, ci.profilePath, ourHome);
    for (size_t i = 0; i < finalCandidates.size(); ++i) {
        if (!finalCandidates[i].identityConclusive) {
            std::printf("    ERROR: channel candidate became unidentified before mutation; aborted\n");
            return -1;
        }
    }

    std::string current;
    if (!readFileBytes(ci.localStatePath, current)) {
        std::printf("    ERROR: re-read failed\n");
        return -1;
    }
    if (!writeRawFile(bakPath, current, true)) {
        std::printf("    ERROR: backup failed (err=%d)\n", errno);
        return -1;
    }
    ci.backupPath = bakPath;
    g_lastBackupPath = bakPath;
    std::printf("    backup: %s\n", bakPath.c_str());

    JValue root;
    JParser p(current);
    if (!p.parse(root) || !root.isObj()) { std::printf("    ERROR: parse failed: %s\n", p.err.c_str()); return -1; }

    JValue& brave  = root.ensureObj("brave");
    JValue& origin = brave.ensureObj("origin");
    origin.set("purchase_validated", JValue::makeBool(true));
    JValue& skus  = root.ensureObj("skus");
    JValue& state = skus.ensureObj("state");
    state.set("67", JValue::makeStr("{\"credentials\": {\"items\": {\"6\": \"7\"}}}"));

    std::string out;
    serializeJson(root, out);
    if (!writeFileBytesAtomic(ci.localStatePath, out)) {
        std::printf("    ERROR: atomic write failed (err=%d)\n", errno);
        return -1;
    }
    std::printf("    wrote %d bytes (compact, sorted keys; only 2 keys added/updated)\n", (int)out.size());

    VerifyResult v1 = verifyLocalState(ci.localStatePath);
    std::printf("    verify[immediate]: parsed=%s purchase_validated=%s skus.state keys=%d marker67=%s%s\n",
                v1.parsed ? "yes" : "no", v1.purchase ? "true" : "false",
                (int)v1.stateKeys, v1.marker67 ? "ok" : "MISSING",
                v1.err.empty() ? "" : (" err=" + v1.err).c_str());

    std::printf("    waiting 3s to let Brave's file watcher settle...\n");
    ::sleep(3);
    VerifyResult v2 = verifyLocalState(ci.localStatePath);
    std::printf("    verify[after 3s]: parsed=%s purchase_validated=%s skus.state keys=%d marker67=%s%s\n",
                v2.parsed ? "yes" : "no", v2.purchase ? "true" : "false",
                (int)v2.stateKeys, v2.marker67 ? "ok" : "MISSING",
                v2.err.empty() ? "" : (" err=" + v2.err).c_str());
    std::printf("    payload note: skus.state.67 is a COMMUNITY-BYPASS shape (not vendor-verified).\n");

    if (!v2.ok()) { std::printf("    ERROR: post-write verification failed\n"); return -1; }

    if (closedCount > 0 && o.apply) {
        std::string exe = resolveBraveExe(ci.name, capturedExe);
        if (exe.empty()) {
            std::printf("    WARNING: brave executable not found for relaunch\n");
        } else {
            std::vector<std::string> cmd;
            if (haveCmd) cmd = relaunchArgv;  // SAME args -> SAME profile
            else { cmd.push_back(exe); cmd.push_back("--user-data-dir=" + ci.profilePath); }
            if (launchProcess(cmd, dirnameOf(exe))) {
                ::sleep(2);
                // Extra settle so the relaunched process appears in /proc.
                ::usleep(500 * 1000);
                std::vector<ProcInfo> after = enumerateChannelProcesses(ci.name, ci.profilePath, ourHome);
                int alive = 0;
                for (size_t i = 0; i < after.size(); ++i) if (after[i].profileMatch) ++alive;
                if (alive > 0) std::printf("    relaunched %s (alive: %d matching process(es))\n", ch.c_str(), alive);
                else           std::printf("    WARNING: relaunch not confirmed alive\n");
            } else {
                std::printf("    WARNING: relaunch failed (err=%d)\n", errno);
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
static int doRestore(const Options& o, const std::string& ourHome) {
    const std::string& bak = o.restoreBak;
    std::printf("=== BraveOriginFix --restore ===\n");
    std::printf("Backup file : %s\n", bak.c_str());
    struct stat bst;
    if (::lstat(bak.c_str(), &bst) != 0 || S_ISLNK(bst.st_mode) || hasSymlinkComponent(bak)) {
        std::printf("ERROR: backup missing or symlink path rejected\n");
        return 2;
    }
    std::string sourceCanonical, parentCanonical;
    if (!canonicalExisting(bak, false, sourceCanonical) ||
        !canonicalExisting(dirnameOf(bak), true, parentCanonical)) {
        std::printf("ERROR: canonical validation failed\n");
        return 2;
    }
    std::string base = basenameOf(sourceCanonical);
    const std::string kBakPrefix = "Local State.bak.";
    if (base.compare(0, kBakPrefix.size(), kBakPrefix) != 0 || base.size() <= kBakPrefix.size()) {
        std::printf("ERROR: basename must match Local State.bak.*\n");
        return 2;
    }
    const char* allowed[3] = { "Brave-Origin", "Brave-Origin-Beta", "Brave-Origin-Nightly" };
    bool contained = false;
    std::string channel;
    for (int i = 0; i < 3; ++i) {
        std::string root = ourHome + "/.config/BraveSoftware/" + allowed[i] + "/User Data";
        std::string canon;
        if (canonicalExisting(root, true, canon) && canon == parentCanonical) {
            contained = true;
            channel = allowed[i];
            break;
        }
    }
    if (!contained) { std::printf("ERROR: backup outside allowed canonical profiles\n"); return 2; }
    std::string target = dirnameOf(bak) + "/Local State";
    std::string targetCanonical;
    if (hasSymlinkComponent(target) || !canonicalExisting(target, false, targetCanonical) ||
        dirnameOf(targetCanonical) != parentCanonical) {
        std::printf("ERROR: target containment/identity failed\n");
        return 2;
    }
    FileIdentity si{}, ti{};
    if (!getFileIdentity(sourceCanonical, si) || !getFileIdentity(targetCanonical, ti) ||
        sameIdentity(si, ti)) {
        std::printf("ERROR: source/target handle identity failed\n");
        return 2;
    }
    std::printf("Target      : %s\n", target.c_str());
    VerifyResult sourceCheck = verifyLocalState(bak);
    if (!sourceCheck.parsed) {
        std::printf("ERROR: backup is not valid JSON; target left unchanged\n");
        return 2;
    }
    if (processFault("unreadable") || processFault("shutdown")) {
        std::printf("ERROR: injected unidentified/shutdown channel process; restore aborted\n");
        return 2;
    }
    std::string profile = dirnameOf(target);
    std::vector<ProcInfo> procs = enumerateChannelProcesses(channel, profile, ourHome);
    for (size_t i = 0; i < procs.size(); ++i) {
        if (!procs[i].identityConclusive) {
            std::printf("ERROR: channel process PID %ld has unreadable/ambiguous identity; restore aborted\n",
                        (long)procs[i].pid);
            return 2;
        }
    }
    for (size_t i = 0; i < procs.size(); ++i) {
        if (procs[i].profileMatch) {
            bool hard = false, closed = false;
            if (!closeProcessGracefully(procs[i], profile, profile, ourHome, 5000, hard, closed) || !closed) {
                std::printf("ERROR: PID %ld could not be safely closed; restore aborted\n", (long)procs[i].pid);
                return 2;
            }
        }
    }
    std::vector<ProcInfo> finalCandidates = enumerateChannelProcesses(channel, profile, ourHome);
    for (size_t i = 0; i < finalCandidates.size(); ++i) {
        if (!finalCandidates[i].identityConclusive) {
            std::printf("ERROR: channel candidate unidentified before restore mutation\n");
            return 2;
        }
    }
    if (processFault("revalidation")) { std::printf("ERROR: injected PID/image revalidation change; restore aborted\n"); return 2; }
    FileIdentity sourceNow, targetNow;
    if (!getFileIdentity(bak, sourceNow) || !getFileIdentity(target, targetNow) ||
        !sameIdentity(sourceNow, si) || !sameIdentity(targetNow, ti)) {
        std::printf("ERROR: source/target identity changed before mutation\n");
        return 2;
    }
    std::string current, restoreBytes;
    if (!readFileBytes(target, current) || !readFileBytes(bak, restoreBytes)) {
        std::printf("ERROR: held source/target read failed\n");
        return 2;
    }
    std::string currentBak = collisionSafeBackup(target);
    if (!writeRawFile(currentBak, current, true)) {
        std::printf("ERROR: pre-restore backup failed\n");
        return 2;
    }
    if (!writeFileBytesAtomic(target, restoreBytes, &targetNow, &parentCanonical)) {
        std::printf("ERROR: atomic restore failed (err=%d)\n", errno);
        return 2;
    }
    g_lastBackupPath = currentBak;
    std::printf("Current target backup: %s\n", currentBak.c_str());
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
    std::string home = envStr("HOME");
    if (home.empty()) {
        struct stat st;
        // Fall back to getpwuid only for diagnostics; HOME is required.
        if (::stat(home.c_str(), &st) != 0) { /* fall through to fatal */ }
        std::printf("FATAL: HOME is not set (environmental error)\n");
        return 2;
    }
    if (o.restore) return doRestore(o, home);

    std::printf("=== BraveOriginFix ===\n");
    std::printf("Mode: %s\n", modeName(o).c_str());
    std::printf("HOME = %s\n", home.c_str());

    static const char* kAllChannels[3] = { "Brave-Origin", "Brave-Origin-Beta", "Brave-Origin-Nightly" };
    std::vector<std::string> channels;
    if (!o.channelFilter.empty()) {
        bool found = false;
        for (int i = 0; i < 3; ++i)
            if (o.channelFilter == kAllChannels[i]) { found = true; break; }
        if (!found) {
            std::printf("ERROR: unknown --channel '%s'\n", o.channelFilter.c_str());
            return 2;
        }
        channels.push_back(o.channelFilter);
    } else {
        for (int i = 0; i < 3; ++i) channels.push_back(kAllChannels[i]);
    }

    std::vector<ChannelInfo> infos(channels.size());
    bool anyError = false, anyPatched = false, anyBroken = false;

    for (size_t ci = 0; ci < channels.size(); ++ci) {
        ChannelInfo& info = infos[ci];
        info.name = channels[ci];
        info.profilePath = home + "/.config/BraveSoftware/" + info.name + "/User Data";
        info.localStatePath = info.profilePath + "/Local State";

        std::printf("\n[%s]\n", info.name.c_str());
        std::printf("  File          : %s\n", info.localStatePath.c_str());

        info.procs = enumerateChannelProcesses(info.name, info.profilePath, home);
        int profMatches = 0;
        for (size_t i = 0; i < info.procs.size(); ++i)
            if (info.procs[i].profileMatch) ++profMatches;

        struct stat st;
        bool present = (::stat(info.localStatePath.c_str(), &st) == 0 && S_ISREG(st.st_mode));
        if (!present) {
            info.status = "NOT-FOUND";
            std::printf("  Status        : NOT-FOUND (skipped)\n");
            std::printf("  Processes     : %d brave on install path, %d matching this profile\n",
                        (int)info.procs.size(), profMatches);
            continue;
        }

        info.exists = true;
        info.size = (unsigned long long)st.st_size;
        info.mtime = st.st_mtime;

        std::string data;
        if (!readFileBytes(info.localStatePath, data)) {
            info.status = "ERROR";
            std::printf("  Status        : ERROR (read failed, err=%d)\n", errno);
            anyError = true;
            continue;
        }
        JValue root;
        JParser p(data);
        if (!p.parse(root) || !root.isObj()) {
            info.status = "ERROR";
            info.parseErr = p.err.empty() ? "root not an object" : p.err;
            std::printf("  Status        : ERROR (JSON parse: %s)\n", info.parseErr.c_str());
            std::printf("  Size          : %llu bytes\n", info.size);
            std::printf("  MTime         : %s\n", formatMTime(info.mtime).c_str());
            std::printf("  Processes     : %d brave on install path, %d matching this profile\n",
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
        std::printf("  Size          : %llu bytes\n", info.size);
        std::printf("  MTime         : %s\n", formatMTime(info.mtime).c_str());
        std::printf("  purchase_validated : %s%s\n",
                    info.hasPurchase ? (info.purchaseTrue ? "true" : "false") : "absent",
                    info.hasPurchase ? "" : " (key missing)");
        std::printf("  skus.state keys    : %d\n", (int)info.stateKeys);
        std::printf("  Processes     : %d brave on install path, %d matching this profile\n",
                    (int)info.procs.size(), profMatches);
        for (size_t i = 0; i < info.procs.size(); ++i) {
            const ProcInfo& pi = info.procs[i];
            std::printf("      pid %-6ld match=%s cmdline=%s%s\n",
                        (long)pi.pid,
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
            int r = doPatch(info, o, home);
            if (r == 1) { info.patched = true; info.patchOk = true; anyPatched = true; info.status = "PATCHED"; }
            else if (r < 0) { anyError = true; std::printf("  Result        : PATCH FAILED\n"); }
        } else if (o.dryRun) {
            doPatch(info, o, home);
        } else {
            std::printf("  Result        : BROKEN (read-only; re-run with --apply to patch)\n");
        }
    }

    std::printf("\n=== SUMMARY ===\n");
    std::printf("Mode: %s\n", modeName(o).c_str());
    for (size_t i = 0; i < infos.size(); ++i) {
        std::string label = infos[i].name;
        while (label.size() < 20) label.push_back(' ');
        std::printf("  %s: %s\n", label.c_str(), infos[i].status.c_str());
    }
    for (size_t i = 0; i < infos.size(); ++i) {
        if (!infos[i].backupPath.empty())
            std::printf("  Backup (%s): %s\n", infos[i].name.c_str(), infos[i].backupPath.c_str());
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

int EngineLinuxRunCli(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--security-self-test") {
        struct Case { const char* json; bool valid; };
        Case cases[] = {
            {"+1", false}, {"01", false}, {"1.", false},
            {"{\"x\":\"a\n\"}", false}, {"\"\\uD800\"", false}, {"\"\\uDC00\"", false},
            {"{\"brave\":{},\"brave\":{}}", false}, {"-1.25e+2", true}
        };
        bool parserOk = true;
        for (size_t n = 0; n < sizeof(cases) / sizeof(cases[0]); ++n) {
            std::string text = cases[n].json;
            JValue v;
            JParser p(text);
            if (p.parse(v) != cases[n].valid) parserOk = false;
        }
        std::vector<std::string> badUtf8 = {
            std::string("\"\x80\"", 3), std::string("\"\xC0\xAF\"", 4),
            std::string("\"\xE2\x28\xA1\"", 5), std::string("\"\xED\xA0\x80\"", 5),
            std::string("\"\xF4\x90\x80\x80\"", 6), std::string("\"\xE2\x82\"", 4)
        };
        for (size_t n = 0; n < badUtf8.size(); ++n) {
            JValue v;
            JParser p(badUtf8[n]);
            if (p.parse(v)) parserOk = false;
        }
        std::string value;
        std::vector<std::string> a1 = {"brave", "--user-data-dir=/One Two"};
        std::vector<std::string> a2 = {"brave", "--user-data-dir", "/One Two"};
        std::vector<std::string> a3 = {"brave", "--foo=--user-data-dir=/decoy"};
        std::vector<std::string> a4 = {"brave", "--user-data-dir=A", "--user-data-dir=B"};
        bool argOk = extractUserDataDir(a1, value) == 1 && value == "/One Two";
        argOk = argOk && extractUserDataDir(a2, value) == 1 &&
                extractUserDataDir(a3, value) == 0 && extractUserDataDir(a4, value) == -1;
        bool mismatch = normalizeLinux("/profile-a") != normalizeLinux("/profile-b");
        std::printf("parser_rfc8259_utf8=%s exact_switch_and_pair=%s decoy_ambiguous_mismatch_safe=%s\n",
                    parserOk ? "PASS" : "FAIL", argOk ? "PASS" : "FAIL",
                    (argOk && mismatch) ? "PASS" : "FAIL");
        return (parserOk && argOk && mismatch) ? 0 : 2;
    }
    Options o = parseOptions(argc, argv);
    if (o.help) { printUsage(); return 0; }
    if (!o.parseOk) {
        std::printf("ERROR: %s\n\n", o.err.c_str());
        printUsage();
        return 2;
    }
    return runOptions(o);
}
