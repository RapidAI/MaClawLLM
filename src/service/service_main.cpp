// maclaw-llm-service — standalone Windows NT service host for MaClawLLM.
//
// Pure C++20, no Python, no CUDA. With no arguments the binary runs as the
// Windows service "MaClawLLM" (display name "MaClaw LLM") when started by the
// Service Control Manager, or falls back to console mode when launched
// directly. `--console` forces console mode; `--config PATH` picks the JSON
// configuration file (default: maclaw-llm.json next to the executable).
// The service logs to maclaw-llm-service.log beside the executable. It listens
// on 127.0.0.1 by default. GET /health is liveness. GET /api/config and
// PUT or POST /api/config are how the Maclaw GUI reads and writes settings:
// a successful write is saved and applied without restarting the process.
// A missing config file uses the defaults. Worker commands run in a Job
// Object, die with the service, and restart 2 seconds after a crash.
//
// SCM management commands (Windows only):
//   --install [--config PATH]   install the service (demand start)
//   --uninstall                 remove the service
//   --start                     start the installed service
//   --stop                      stop the running service
//   --console                   run in the foreground (default when not SCM)
//   --config PATH               configuration file for this run / the service
//   --help                      usage

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winsvc.h>
#endif

#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

#ifdef _WIN32
constexpr wchar_t kServiceName[] = L"MaClawLLM";
constexpr wchar_t kServiceDisplayName[] = L"MaClaw LLM";
constexpr wchar_t kDefaultConfigName[] = L"maclaw-llm.json";

// ---------------------------------------------------------------------------
// Service state
// ---------------------------------------------------------------------------

struct ServiceState {
    SERVICE_STATUS_HANDLE status_handle = nullptr;
    SERVICE_STATUS status{};
    DWORD checkpoint = 0;
    HANDLE stop_event = nullptr;  // Signalled when the SCM asks us to stop.
};

ServiceState g_service;
std::wstring g_config_path;  // Resolved --config path for this run.
HANDLE g_log = INVALID_HANDLE_VALUE;

std::filesystem::path ExeDir();

// UTF-8 for the log file and for getaddrinfo. Supplementary planes become
// U+FFFD; config paths and hostnames we accept are BMP.
std::string WideToUtf8(std::wstring_view w) {
    std::string out;
    out.reserve(w.size());
    for (wchar_t wc : w) {
        const unsigned c = static_cast<unsigned>(wc);
        if (c < 0x80) {
            out.push_back(static_cast<char>(c));
        } else if (c < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (c >> 6)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (c >> 12)));
            out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

// Timestamped line to the service log (once open) and to stderr.
void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st{};
    ::GetLocalTime(&st);
    size_t msg_len = 0;
    while (msg[msg_len] != L'\0') {
        ++msg_len;
    }
    const wchar_t* nl =
        (msg_len > 0 && msg[msg_len - 1] == L'\n') ? L"" : L"\n";
    wchar_t line[1280];
    _snwprintf_s(line, _TRUNCATE,
                 L"%04u-%02u-%02u %02u:%02u:%02u %ls%ls",
                 st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
                 st.wSecond, msg, nl);

    std::fwprintf(stderr, L"%ls", line);
    std::fflush(stderr);
    if (g_log == INVALID_HANDLE_VALUE) {
        return;
    }
    const std::string utf8 = WideToUtf8(line);
    DWORD written = 0;
    ::WriteFile(g_log, utf8.data(), static_cast<DWORD>(utf8.size()),
                &written, nullptr);
}

DWORD ServiceRun();  // Defined below, after the config parser.


void ReportStatus(DWORD state, DWORD win32_exit_code = NO_ERROR,
                  DWORD wait_hint_ms = 0) {
    g_service.status.dwCurrentState = state;
    g_service.status.dwWin32ExitCode = win32_exit_code;
    g_service.status.dwWaitHint = wait_hint_ms;
    if (state == SERVICE_START_PENDING) {
        g_service.status.dwControlsAccepted = 0;
        g_service.status.dwCheckPoint = ++g_service.checkpoint;
    } else if (state == SERVICE_RUNNING) {
        g_service.status.dwControlsAccepted =
            SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
        g_service.status.dwCheckPoint = 0;
    } else {
        g_service.status.dwCheckPoint = 0;
    }
    if (g_service.status_handle) {
        ::SetServiceStatus(g_service.status_handle, &g_service.status);
    }
}

DWORD WINAPI ServiceCtrlHandlerEx(DWORD control, DWORD event_type,
                                  LPVOID event_data, LPVOID context) {
    (void)event_type;
    (void)event_data;
    (void)context;
    switch (control) {
        case SERVICE_CONTROL_STOP:
        case SERVICE_CONTROL_SHUTDOWN:
            ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 5000);
            if (g_service.stop_event) {
                ::SetEvent(g_service.stop_event);
            }
            return NO_ERROR;
        case SERVICE_CONTROL_INTERROGATE:
            ::SetServiceStatus(g_service.status_handle, &g_service.status);
            return NO_ERROR;
        default:
            return ERROR_CALL_NOT_IMPLEMENTED;
    }
}

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    // The SCM passes the arguments quoted into binPath at install time, so a
    // service installed with `--config PATH` sees it again here.
    std::wstring config;
    for (DWORD i = 1; i + 1 < argc; ++i) {
        if (std::wstring_view(argv[i]) == L"--config") {
            config = argv[i + 1];
        }
    }
    if (!config.empty()) {
        g_config_path = config;
    }

    g_service.status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_service.status.dwServiceSpecificExitCode = 0;

    g_service.status_handle =
        ::RegisterServiceCtrlHandlerExW(kServiceName, ServiceCtrlHandlerEx,
                                        nullptr);
    if (!g_service.status_handle) {
        return;  // Nothing we can report to without a handle.
    }

    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 3000);

    if (!g_service.stop_event) {
        g_service.stop_event =
            ::CreateEventW(nullptr, /*manual_reset=*/TRUE, /*initial=*/FALSE,
                           nullptr);
    }
    if (!g_service.stop_event) {
        ReportStatus(SERVICE_STOPPED, ::GetLastError());
        return;
    }

    ReportStatus(SERVICE_RUNNING);

    const DWORD wait_result = ServiceRun();
    const DWORD exit_code =
        (wait_result == WAIT_OBJECT_0) ? NO_ERROR : wait_result;

    ReportStatus(SERVICE_STOPPED, exit_code);
}

// Returns true when the SCM connected and the service ran to completion.
// Returns false with ERROR_FAILED_SERVICE_CONTROLLER_CONNECT when the process
// was started as a normal console program rather than by the SCM.
bool RunAsService(DWORD* error) {
    const SERVICE_TABLE_ENTRYW table[] = {
        {const_cast<LPWSTR>(kServiceName), ServiceMain},
        {nullptr, nullptr},
    };
    if (::StartServiceCtrlDispatcherW(table)) {
        return true;
    }
    *error = ::GetLastError();
    return false;
}

// ---------------------------------------------------------------------------
// Console mode
// ---------------------------------------------------------------------------

// Console-mode stop handler: sets the same event the SCM would set.
BOOL WINAPI ConsoleCtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_SHUTDOWN_EVENT:
        case CTRL_LOGOFF_EVENT:
            if (g_service.stop_event) {
                ::SetEvent(g_service.stop_event);
            }
            return TRUE;
        default:
            return FALSE;
    }
}

int RunConsole() {
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, TRUE);
    g_service.stop_event =
        ::CreateEventW(nullptr, /*manual_reset=*/TRUE, /*initial=*/FALSE,
                       nullptr);
    if (!g_service.stop_event) {
        Log( L"CreateEventW failed: %lu\n", ::GetLastError());
        return static_cast<int>(::GetLastError());
    }

    std::fwprintf(stdout,
                  L"%ls console mode, config %ls. Ctrl+C to stop.\n",
                  kServiceName, g_config_path.c_str());
    const DWORD wait_result = ServiceRun();
    const DWORD exit_code =
        (wait_result == WAIT_OBJECT_0) ? NO_ERROR : wait_result;
    ::SetConsoleCtrlHandler(ConsoleCtrlHandler, FALSE);
    return static_cast<int>(exit_code);
}

// ---------------------------------------------------------------------------
// JSON configuration
// ---------------------------------------------------------------------------
// The service reads a small, flat JSON object. Only these fields are used;
// everything else is ignored:
//   host    string          listen address (default 127.0.0.1, this PC only)
//   port    number 1..65535 listen port                  (default 8080)
//   workdir string          working directory for worker processes
//   worker  array<string>   engine command lines to spawn and supervise
// Any parse/type/range error aborts startup with the reason on stderr.
// The same object is what GET/PUT /api/config reads and writes.

struct Config {
    std::wstring host = L"127.0.0.1";
    int port = 8080;
    std::wstring workdir;                // May stay empty (inherits ours).
    std::vector<std::wstring> worker;    // Engine command lines to run.
};

// Minimal JSON value; only what the config file needs.
struct JsonValue {
    enum class Kind { kNull, kBool, kNumber, kString, kArray, kObject };
    Kind kind = Kind::kNull;
    bool boolean = false;
    double number = 0;
    std::wstring str;
    std::vector<JsonValue> array;
    std::vector<std::pair<std::wstring, JsonValue>> object;

    const JsonValue* Find(std::wstring_view key) const {
        for (const auto& [k, v] : object) {
            if (k == key) return &v;
        }
        return nullptr;
    }
};

class JsonParser {
  public:
    explicit JsonParser(std::wstring_view text) : text_(text) {}

    // Returns false and fills `error` (with byte position) on failure.
    bool Parse(JsonValue* out, std::wstring* error) {
        SkipWs();
        if (!ParseValue(out, error)) {
            return false;
        }
        SkipWs();
        if (pos_ != text_.size()) {
            *error = L"unexpected trailing characters";
            return false;
        }
        return true;
    }

  private:
    bool Fail(std::wstring* error, const wchar_t* msg) const {
        *error = std::wstring(msg) + L" at offset " + std::to_wstring(pos_);
        return false;
    }

    void SkipWs() {
        while (pos_ < text_.size() &&
               (text_[pos_] == L' ' || text_[pos_] == L'\t' ||
                text_[pos_] == L'\r' || text_[pos_] == L'\n')) {
            ++pos_;
        }
    }

    bool Consume(std::wstring_view literal) {
        if (text_.substr(pos_, literal.size()) == literal) {
            pos_ += literal.size();
            return true;
        }
        return false;
    }

    bool ParseValue(JsonValue* out, std::wstring* error) {
        if (pos_ >= text_.size()) {
            return Fail(error, L"unexpected end of input");
        }
        const wchar_t c = text_[pos_];
        if (c == L'{') return ParseObject(out, error);
        if (c == L'[') return ParseArray(out, error);
        if (c == L'"') {
            out->kind = JsonValue::Kind::kString;
            return ParseString(&out->str, error);
        }
        if (c == L'-' || (c >= L'0' && c <= L'9')) {
            out->kind = JsonValue::Kind::kNumber;
            return ParseNumber(&out->number, error);
        }
        if (Consume(L"true")) {
            out->kind = JsonValue::Kind::kBool;
            out->boolean = true;
            return true;
        }
        if (Consume(L"false")) {
            out->kind = JsonValue::Kind::kBool;
            out->boolean = false;
            return true;
        }
        if (Consume(L"null")) {
            out->kind = JsonValue::Kind::kNull;
            return true;
        }
        return Fail(error, L"unexpected character");
    }

    bool ParseString(std::wstring* out, std::wstring* error) {
        ++pos_;  // opening quote
        out->clear();
        while (pos_ < text_.size()) {
            const wchar_t c = text_[pos_++];
            if (c == L'"') {
                return true;
            }
            if (c == L'\\') {
                if (pos_ >= text_.size()) break;
                const wchar_t esc = text_[pos_++];
                switch (esc) {
                    case L'"': out->push_back(L'"'); break;
                    case L'\\': out->push_back(L'\\'); break;
                    case L'/': out->push_back(L'/'); break;
                    case L'b': out->push_back(L'\b'); break;
                    case L'f': out->push_back(L'\f'); break;
                    case L'n': out->push_back(L'\n'); break;
                    case L'r': out->push_back(L'\r'); break;
                    case L't': out->push_back(L'\t'); break;
                    case L'u': {
                        if (pos_ + 4 > text_.size()) {
                            return Fail(error, L"truncated \\u escape");
                        }
                        unsigned cp = 0;
                        for (int i = 0; i < 4; ++i) {
                            const wchar_t h = text_[pos_++];
                            cp <<= 4;
                            if (h >= L'0' && h <= L'9') cp += h - L'0';
                            else if (h >= L'a' && h <= L'f') cp += h - L'a' + 10;
                            else if (h >= L'A' && h <= L'F') cp += h - L'A' + 10;
                            else return Fail(error, L"bad \\u escape");
                        }
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
                            text_[pos_] == L'\\' && text_[pos_ + 1] == L'u') {
                            // Surrogate pair.
                            unsigned lo = 0;
                            pos_ += 2;
                            for (int i = 0; i < 4; ++i) {
                                const wchar_t h = text_[pos_++];
                                lo <<= 4;
                                if (h >= L'0' && h <= L'9') lo += h - L'0';
                                else if (h >= L'a' && h <= L'f') lo += h - L'a' + 10;
                                else if (h >= L'A' && h <= L'F') lo += h - L'A' + 10;
                            }
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                            out->push_back(static_cast<wchar_t>(cp));
                        } else {
                            out->push_back(static_cast<wchar_t>(cp));
                        }
                        break;
                    }
                    default:
                        return Fail(error, L"bad escape sequence");
                }
            } else {
                out->push_back(c);
            }
        }
        return Fail(error, L"unterminated string");
    }

    bool ParseNumber(double* out, std::wstring* error) {
        const size_t start = pos_;
        if (pos_ < text_.size() && text_[pos_] == L'-') ++pos_;
        while (pos_ < text_.size() &&
               (std::isdigit(text_[pos_]) || text_[pos_] == L'.' ||
                text_[pos_] == L'e' || text_[pos_] == L'E' ||
                text_[pos_] == L'+' || text_[pos_] == L'-')) {
            ++pos_;
        }
        try {
            *out = std::stod(std::wstring(text_.substr(start, pos_ - start)));
        } catch (...) {
            return Fail(error, L"invalid number");
        }
        return true;
    }

    bool ParseArray(JsonValue* out, std::wstring* error) {
        ++pos_;  // '['
        out->kind = JsonValue::Kind::kArray;
        SkipWs();
        if (pos_ < text_.size() && text_[pos_] == L']') {
            ++pos_;
            return true;
        }
        while (true) {
            JsonValue v;
            if (!ParseValue(&v, error)) return false;
            out->array.push_back(std::move(v));
            SkipWs();
            if (pos_ >= text_.size()) return Fail(error, L"unterminated array");
            const wchar_t c = text_[pos_++];
            if (c == L']') return true;
            if (c != L',') return Fail(error, L"expected ',' or ']' in array");
            SkipWs();
        }
    }

    bool ParseObject(JsonValue* out, std::wstring* error) {
        ++pos_;  // '{'
        out->kind = JsonValue::Kind::kObject;
        SkipWs();
        if (pos_ < text_.size() && text_[pos_] == L'}') {
            ++pos_;
            return true;
        }
        while (true) {
            SkipWs();
            if (pos_ >= text_.size() || text_[pos_] != L'"') {
                return Fail(error, L"expected string key in object");
            }
            std::wstring key;
            if (!ParseString(&key, error)) return false;
            SkipWs();
            if (pos_ >= text_.size() || text_[pos_] != L':') {
                return Fail(error, L"expected ':' after key");
            }
            ++pos_;
            SkipWs();
            JsonValue v;
            if (!ParseValue(&v, error)) return false;
            out->object.emplace_back(std::move(key), std::move(v));
            SkipWs();
            if (pos_ >= text_.size()) return Fail(error, L"unterminated object");
            const wchar_t c = text_[pos_++];
            if (c == L'}') return true;
            if (c != L',') return Fail(error, L"expected ',' or '}' in object");
        }
    }

    std::wstring_view text_;
    size_t pos_ = 0;
};

// UTF-8 to UTF-16. BMP only; a 4-byte sequence becomes U+FFFD.
bool Utf8ToWide(std::string_view bytes, std::wstring* out, std::wstring* error) {
    out->clear();
    out->reserve(bytes.size());
    for (size_t i = 0; i < bytes.size();) {
        const unsigned char c = static_cast<unsigned char>(bytes[i]);
        if (c < 0x80) {
            out->push_back(c);
            ++i;
        } else if ((c >> 5) == 0x6 && i + 1 < bytes.size()) {
            const unsigned char c1 = static_cast<unsigned char>(bytes[i + 1]);
            out->push_back(static_cast<wchar_t>(((c & 0x1F) << 6) | (c1 & 0x3F)));
            i += 2;
        } else if ((c >> 4) == 0xE && i + 2 < bytes.size()) {
            const unsigned char c1 = static_cast<unsigned char>(bytes[i + 1]);
            const unsigned char c2 = static_cast<unsigned char>(bytes[i + 2]);
            out->push_back(static_cast<wchar_t>(
                ((c & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F)));
            i += 3;
        } else if ((c >> 3) == 0x1E && i + 3 < bytes.size()) {
            out->push_back(L'\uFFFD');
            i += 4;
        } else {
            *error = L"invalid UTF-8";
            return false;
        }
    }
    return true;
}

// Copies present keys from a JSON object onto `cfg`. Absent keys stay as they
// are, so a PUT can change one field. `error` is a short reason, no path.
bool ApplyConfigObject(const JsonValue& root, Config* cfg, std::wstring* error) {
    if (root.kind != JsonValue::Kind::kObject) {
        *error = L"top level must be an object";
        return false;
    }
    const JsonValue* v = root.Find(L"host");
    if (v) {
        if (v->kind != JsonValue::Kind::kString || v->str.empty()) {
            *error = L"\"host\" must be a non-empty string";
            return false;
        }
        cfg->host = v->str;
    }
    v = root.Find(L"port");
    if (v) {
        if (v->kind != JsonValue::Kind::kNumber || v->number < 1 ||
            v->number > 65535 || v->number != std::floor(v->number)) {
            *error = L"\"port\" must be an integer in 1..65535";
            return false;
        }
        cfg->port = static_cast<int>(v->number);
    }
    v = root.Find(L"workdir");
    if (v) {
        if (v->kind != JsonValue::Kind::kString) {
            *error = L"\"workdir\" must be a string";
            return false;
        }
        cfg->workdir = v->str;
    }
    v = root.Find(L"worker");
    if (v) {
        if (v->kind != JsonValue::Kind::kArray) {
            *error = L"\"worker\" must be an array of strings";
            return false;
        }
        std::vector<std::wstring> workers;
        for (const JsonValue& item : v->array) {
            if (item.kind != JsonValue::Kind::kString || item.str.empty()) {
                *error = L"\"worker\" must contain only non-empty strings";
                return false;
            }
            workers.push_back(item.str);
        }
        cfg->worker = std::move(workers);
    }
    return true;
}

// Reads the config file into `cfg` (caller passes defaults). A missing file
// keeps those defaults and returns true. A bad file logs the reason and
// returns false.
bool LoadConfig(const std::wstring& path, Config* cfg) {
    const std::filesystem::path file(path);
    if (!std::filesystem::exists(file)) {
        Log(L"config file %ls: not found, listening on %ls:%d",
            path.c_str(), cfg->host.c_str(), cfg->port);
        return true;
    }
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        Log(L"config file %ls: cannot open (%d)", path.c_str(), errno);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    std::wstring text;
    std::wstring error;
    if (!Utf8ToWide(ss.str(), &text, &error)) {
        Log(L"config file %ls: %ls", path.c_str(), error.c_str());
        return false;
    }
    JsonValue root;
    if (!JsonParser(text).Parse(&root, &error) ||
        !ApplyConfigObject(root, cfg, &error)) {
        Log(L"config file %ls: %ls", path.c_str(), error.c_str());
        return false;
    }
    return true;
}

void AppendJsonString(std::string* out, std::wstring_view value) {
    out->push_back('"');
    const std::string utf8 = WideToUtf8(value);
    for (unsigned char c : utf8) {
        switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (c < 0x20) {
                    char hex[8];
                    _snprintf_s(hex, _TRUNCATE, "\\u%04x", c);
                    out->append(hex);
                } else {
                    out->push_back(static_cast<char>(c));
                }
        }
    }
    out->push_back('"');
}

std::string ConfigJson(const Config& cfg) {
    std::string out = "{\"host\":";
    AppendJsonString(&out, cfg.host);
    out += ",\"port\":" + std::to_string(cfg.port);
    out += ",\"workdir\":";
    AppendJsonString(&out, cfg.workdir);
    out += ",\"worker\":[";
    for (size_t i = 0; i < cfg.worker.size(); ++i) {
        if (i) out.push_back(',');
        AppendJsonString(&out, cfg.worker[i]);
    }
    out += "]}";
    return out;
}

// Writes the live settings. A .tmp file plus replace keeps a crash from
// leaving an empty config.
bool SaveConfig(const std::wstring& path, const Config& cfg, std::wstring* error) {
    const std::wstring tmp = path + L".tmp";
    const std::string body = ConfigJson(cfg);
    HANDLE file = ::CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        *error = L"cannot write config file";
        return false;
    }
    DWORD written = 0;
    const BOOL ok = ::WriteFile(file, body.data(),
                                static_cast<DWORD>(body.size()), &written, nullptr);
    ::CloseHandle(file);
    if (!ok || written != body.size()) {
        ::DeleteFileW(tmp.c_str());
        *error = L"cannot write config file";
        return false;
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.c_str());
        *error = L"cannot replace config file";
        return false;
    }
    return true;
}

constexpr DWORD kRestartDelayMs = 2000;
constexpr int kHealthBacklog = 8;

bool OpenServiceLog(DWORD* err) {
    const auto path = ExeDir() / L"maclaw-llm-service.log";
    g_log = ::CreateFileW(path.c_str(), FILE_APPEND_DATA,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_log == INVALID_HANDLE_VALUE) {
        *err = ::GetLastError();
        std::fwprintf(stderr, L"cannot open log %ls: %lu\n", path.c_str(), *err);
        return false;
    }
    return true;
}

void CloseServiceLog() {
    if (g_log != INVALID_HANDLE_VALUE) {
        ::CloseHandle(g_log);
        g_log = INVALID_HANDLE_VALUE;
    }
}

DWORD RunServiceLoop(const Config& cfg);

// Shared service body. Log, config, GET /health, job-supervised workers.
DWORD ServiceRun() {
    DWORD err = ERROR_OPEN_FAILED;
    if (!OpenServiceLog(&err)) {
        return err;
    }
    Config cfg;
    if (!LoadConfig(g_config_path, &cfg)) {
        CloseServiceLog();
        return ERROR_INVALID_DATA;
    }
    Log(L"%ls: host=%ls port=%d workdir=%ls workers=%llu",
        kServiceName, cfg.host.c_str(), cfg.port,
        cfg.workdir.empty() ? L"(inherit)" : cfg.workdir.c_str(),
        static_cast<unsigned long long>(cfg.worker.size()));
    const DWORD result = RunServiceLoop(cfg);
    CloseServiceLog();
    return result;
}

void ServeOne(SOCKET client);

void SendHttp(SOCKET client, int status, std::string_view reason,
              std::string_view body) {
    char head[512];
    const int n = _snprintf_s(
        head, _TRUNCATE,
        "HTTP/1.1 %d %.*s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Access-Control-Allow-Methods: GET, PUT, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\n"
        "\r\n",
        status, static_cast<int>(reason.size()), reason.data(),
        static_cast<unsigned>(body.size()));
    if (n > 0) {
        ::send(client, head, n, 0);
    }
    if (!body.empty()) {
        ::send(client, body.data(), static_cast<int>(body.size()), 0);
    }
}

bool ListenHealth(const Config& cfg, SOCKET* out) {
    const std::string host = WideToUtf8(cfg.host);
    char port[8];
    _snprintf_s(port, _TRUNCATE, "%d", cfg.port);
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), port, &hints, &res) != 0) {
        Log(L"getaddrinfo(%ls:%d) failed", cfg.host.c_str(), cfg.port);
        return false;
    }
    SOCKET sock = INVALID_SOCKET;
    for (addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
        sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (sock == INVALID_SOCKET) {
            continue;
        }
        const BOOL reuse = TRUE;
        ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
                     reinterpret_cast<const char*>(&reuse), sizeof(reuse));
        if (::bind(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 &&
            ::listen(sock, kHealthBacklog) == 0) {
            break;
        }
        ::closesocket(sock);
        sock = INVALID_SOCKET;
    }
    ::freeaddrinfo(res);
    if (sock == INVALID_SOCKET) {
        Log(L"listen %ls:%d failed: %d", cfg.host.c_str(), cfg.port,
            ::WSAGetLastError());
        return false;
    }
    u_long nb = 1;
    ::ioctlsocket(sock, FIONBIO, &nb);
    *out = sock;
    Log(L"listening on %ls:%d (GET /health, GET|PUT|POST /api/config)",
        cfg.host.c_str(), cfg.port);
    return true;
}

void AcceptHealth(SOCKET listen_sock) {
    for (;;) {
        const SOCKET client = ::accept(listen_sock, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            return;
        }
        ServeOne(client);
    }
}
struct WorkerSlot {
    std::wstring command;
    HANDLE process = nullptr;
    ULONGLONG restart_at = 0;
};

// One process-wide copy. The accept loop and the config API share it; both
// run on the service thread.
struct ServiceRuntime {
    Config cfg;
    std::vector<WorkerSlot> slots;
    HANDLE job = nullptr;
    SOCKET listen = INVALID_SOCKET;
};
ServiceRuntime* g_rt = nullptr;

void ReplaceWorkers();
bool Rebind(const Config& previous, std::wstring* error);

HANDLE CreateWorkerJob() {
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (!job) {
        Log(L"CreateJobObjectW failed: %lu", ::GetLastError());
        return nullptr;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
    info.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!::SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                   &info, sizeof(info))) {
        Log(L"SetInformationJobObject failed: %lu", ::GetLastError());
        ::CloseHandle(job);
        return nullptr;
    }
    return job;
}

bool SpawnWorker(HANDLE job, const std::wstring& workdir, WorkerSlot* slot,
                 size_t index) {
    std::wstring cmd = slot->command;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    const wchar_t* dir = workdir.empty() ? nullptr : workdir.c_str();
    const DWORD flags =
        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP;
    if (!::CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, flags,
                          nullptr, dir, &si, &pi)) {
        Log(L"worker[%llu] start failed (%lu): %ls",
            static_cast<unsigned long long>(index), ::GetLastError(),
            slot->command.c_str());
        slot->restart_at = ::GetTickCount64() + kRestartDelayMs;
        return false;
    }
    if (!::AssignProcessToJobObject(job, pi.hProcess)) {
        Log(L"worker[%llu] AssignProcessToJobObject failed: %lu",
            static_cast<unsigned long long>(index), ::GetLastError());
        ::TerminateProcess(pi.hProcess, 1);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        slot->restart_at = ::GetTickCount64() + kRestartDelayMs;
        return false;
    }
    ::ResumeThread(pi.hThread);
    ::CloseHandle(pi.hThread);
    slot->process = pi.hProcess;
    slot->restart_at = 0;
    Log(L"worker[%llu] started pid=%lu: %ls",
        static_cast<unsigned long long>(index), pi.dwProcessId,
        slot->command.c_str());
    return true;
}

void ReplaceWorkers() {
    for (WorkerSlot& slot : g_rt->slots) {
        if (!slot.process) continue;
        ::TerminateProcess(slot.process, 1);
        ::WaitForSingleObject(slot.process, 3000);
        ::CloseHandle(slot.process);
        slot.process = nullptr;
    }
    g_rt->slots.clear();
    g_rt->slots.reserve(g_rt->cfg.worker.size());
    for (const std::wstring& cmd : g_rt->cfg.worker) {
        WorkerSlot slot;
        slot.command = cmd;
        g_rt->slots.push_back(std::move(slot));
    }
    for (size_t i = 0; i < g_rt->slots.size(); ++i) {
        SpawnWorker(g_rt->job, g_rt->cfg.workdir, &g_rt->slots[i], i);
    }
}

// Closes the current listener, then binds `g_rt->cfg`. On failure the previous
// address is bound again and `error` says why the new one was refused.
bool Rebind(const Config& previous, std::wstring* error) {
    if (g_rt->cfg.host == previous.host && g_rt->cfg.port == previous.port) {
        return true;
    }
    ::closesocket(g_rt->listen);
    g_rt->listen = INVALID_SOCKET;
    SOCKET neu = INVALID_SOCKET;
    if (ListenHealth(g_rt->cfg, &neu)) {
        g_rt->listen = neu;
        return true;
    }
    const Config rejected = g_rt->cfg;
    g_rt->cfg.host = previous.host;
    g_rt->cfg.port = previous.port;
    if (!ListenHealth(g_rt->cfg, &neu)) {
        *error = L"listener could not bind the new address or the previous one";
        Log(L"%ls", error->c_str());
        return false;
    }
    g_rt->listen = neu;
    *error = L"cannot listen on " + rejected.host + L":" +
             std::to_wstring(rejected.port);
    Log(L"%ls", error->c_str());
    return false;
}

std::string_view HeaderValue(std::string_view headers, std::string_view name) {
    std::string key(name);
    for (char& c : key) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    size_t pos = 0;
    while (pos < headers.size()) {
        const size_t end = headers.find("\r\n", pos);
        const std::string_view line = headers.substr(
            pos, end == std::string_view::npos ? headers.size() - pos : end - pos);
        const size_t colon = line.find(':');
        if (colon != std::string_view::npos) {
            std::string got(line.substr(0, colon));
            for (char& c : got) {
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            }
            if (got == key) {
                std::string_view value = line.substr(colon + 1);
                while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
                    value.remove_prefix(1);
                }
                return value;
            }
        }
        if (end == std::string_view::npos) break;
        pos = end + 2;
    }
    return {};
}

bool ReadRequest(SOCKET client, std::string* raw) {
    raw->clear();
    const DWORD timeout = 2000;
    ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                 reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    char buf[2048];
    while (raw->size() < 65536) {
        const int n = ::recv(client, buf, sizeof(buf), 0);
        if (n == 0) break;
        if (n < 0) {
            const int err = ::WSAGetLastError();
            if ((err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) && !raw->empty()) break;
            return false;
        }
        raw->append(buf, buf + n);
        const size_t header_end = raw->find("\r\n\r\n");
        if (header_end == std::string::npos) continue;
        const std::string_view headers(*raw);
        const std::string_view len_text =
            HeaderValue(headers.substr(0, header_end), "content-length");
        size_t need = header_end + 4;
        if (!len_text.empty()) {
            int len = 0;
            for (char c : len_text) {
                if (c < '0' || c > '9') {
                    len = -1;
                    break;
                }
                len = len * 10 + (c - '0');
                if (len > 65536) {
                    len = -1;
                    break;
                }
            }
            if (len < 0) return false;
            need += static_cast<size_t>(len);
        }
        if (raw->size() >= need) return true;
    }
    return raw->find("\r\n\r\n") != std::string::npos;
}

void ErrorJson(std::string* body, const std::wstring& error) {
    *body = "{\"error\":";
    AppendJsonString(body, error);
    *body += "}";
}

void ServeOne(SOCKET client) {
    std::string raw;
    if (!ReadRequest(client, &raw)) {
        ::closesocket(client);
        return;
    }
    const size_t line_end = raw.find("\r\n");
    const std::string_view start(raw.data(), line_end == std::string::npos ? raw.size() : line_end);
    const size_t sp1 = start.find(' ');
    const size_t sp2 = sp1 == std::string_view::npos ? std::string_view::npos : start.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos) {
        SendHttp(client, 400, "Bad Request", "{\"error\":\"bad request\"}");
        ::closesocket(client);
        return;
    }
    const std::string_view method = start.substr(0, sp1);
    std::string_view path = start.substr(sp1 + 1, (sp2 == std::string_view::npos ? start.size() : sp2) - (sp1 + 1));
    const size_t q = path.find('?');
    if (q != std::string_view::npos) path = path.substr(0, q);

    auto finish = [&](int status, std::string_view reason, std::string_view body) {
        SendHttp(client, status, reason, body);
        ::closesocket(client);
    };

    if (method == "OPTIONS" && (path == "/api/config" || path == "/health")) {
        finish(204, "No Content", "");
        return;
    }
    if (method == "GET" && path == "/health") {
        const char* worker = "none";
        if (g_rt && !g_rt->cfg.worker.empty()) {
            worker = "stopped";
            for (const WorkerSlot& slot : g_rt->slots) {
                if (slot.process) {
                    worker = "running";
                    break;
                }
            }
        }
        std::string body = std::string("{\"status\":\"ok\",\"service\":\"MaClawLLM\",\"worker\":\"") +
                           worker + "\"}";
        finish(200, "OK", body);
        return;
    }
    if (path == "/api/config" && method == "GET") {
        finish(200, "OK", ConfigJson(g_rt->cfg));
        return;
    }
    if (path == "/api/config" && (method == "PUT" || method == "POST")) {
        const size_t header_end = raw.find("\r\n\r\n");
        const std::string_view body_bytes = std::string_view(raw).substr(header_end + 4);
        std::wstring text;
        std::wstring error;
        if (!Utf8ToWide(body_bytes, &text, &error)) {
            std::string err;
            ErrorJson(&err, error);
            finish(400, "Bad Request", err);
            return;
        }
        JsonValue root;
        if (!JsonParser(text).Parse(&root, &error)) {
            std::string err;
            ErrorJson(&err, error);
            finish(400, "Bad Request", err);
            return;
        }
        Config next = g_rt->cfg;
        if (!ApplyConfigObject(root, &next, &error)) {
            std::string err;
            ErrorJson(&err, error);
            finish(400, "Bad Request", err);
            return;
        }
        const Config previous = g_rt->cfg;
        const bool new_listen = next.host != previous.host || next.port != previous.port;
        const bool new_workers = next.workdir != previous.workdir || next.worker != previous.worker;
        g_rt->cfg = next;
        if (new_listen && !Rebind(previous, &error)) {
            g_rt->cfg = previous;
            std::wstring ignored;
            SaveConfig(g_config_path, g_rt->cfg, &ignored);
            std::string err;
            ErrorJson(&err, error);
            finish(500, "Error", err);
            return;
        }
        if (!SaveConfig(g_config_path, g_rt->cfg, &error)) {
            g_rt->cfg = previous;
            if (new_listen) {
                std::wstring ignored;
                Rebind(next, &ignored);
            }
            std::string err;
            ErrorJson(&err, error);
            finish(500, "Error", err);
            return;
        }
        if (new_workers) ReplaceWorkers();
        Log(L"config updated from /api/config: host=%ls port=%d workers=%llu",
            g_rt->cfg.host.c_str(), g_rt->cfg.port,
            static_cast<unsigned long long>(g_rt->cfg.worker.size()));
        finish(200, "OK", ConfigJson(g_rt->cfg));
        return;
    }
    finish(404, "Not Found", "{\"error\":\"not_found\"}");
}

void NoteWorkerExit(WorkerSlot* slot, size_t index) {
    DWORD code = 1;
    ::GetExitCodeProcess(slot->process, &code);
    ::CloseHandle(slot->process);
    slot->process = nullptr;
    slot->restart_at = ::GetTickCount64() + kRestartDelayMs;
    Log(L"worker[%llu] exited code=%lu, restarting in 2s",
        static_cast<unsigned long long>(index), code);
}

DWORD SuperviseWorkers(HANDLE job, SOCKET listen_sock,
                       std::vector<WorkerSlot>* slots,
                       const std::wstring& workdir) {
    (void)listen_sock;
    (void)workdir;
    DWORD result = WAIT_OBJECT_0;
    for (;;) {
        HANDLE waits[MAXIMUM_WAIT_OBJECTS];
        size_t slot_of[MAXIMUM_WAIT_OBJECTS] = {};
        DWORD n = 1;
        waits[0] = g_service.stop_event;
        for (size_t i = 0; i < slots->size() && n < MAXIMUM_WAIT_OBJECTS; ++i) {
            if ((*slots)[i].process) {
                slot_of[n] = i;
                waits[n++] = (*slots)[i].process;
            }
        }
        const ULONGLONG now = ::GetTickCount64();
        DWORD timeout = 200;
        for (const WorkerSlot& slot : *slots) {
            if (slot.restart_at == 0) {
                continue;
            }
            if (slot.restart_at <= now) {
                timeout = 0;
            } else if (slot.restart_at - now < timeout) {
                timeout = static_cast<DWORD>(slot.restart_at - now);
            }
        }
        for (size_t i = 0; i < slots->size(); ++i) {
            HANDLE h = (*slots)[i].process;
            if (!h) {
                continue;
            }
            bool watched = false;
            for (DWORD w = 1; w < n; ++w) {
                if (waits[w] == h) {
                    watched = true;
                }
            }
            if (!watched && ::WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
                NoteWorkerExit(&(*slots)[i], i);
            }
        }
        const DWORD wr = ::WaitForMultipleObjects(n, waits, FALSE, timeout);
        if (wr == WAIT_OBJECT_0) {
            Log(L"stop requested");
            break;
        }
        if (wr > WAIT_OBJECT_0 && wr < WAIT_OBJECT_0 + n) {
            const size_t i = slot_of[wr - WAIT_OBJECT_0];
            NoteWorkerExit(&(*slots)[i], i);
        } else if (wr == WAIT_FAILED) {
            result = ::GetLastError();
            Log(L"WaitForMultipleObjects failed: %lu", result);
            break;
        }
        const ULONGLONG tick = ::GetTickCount64();
        for (size_t i = 0; i < slots->size(); ++i) {
            WorkerSlot& slot = (*slots)[i];
            if (!slot.process && slot.restart_at != 0 &&
                tick >= slot.restart_at) {
                SpawnWorker(job, g_rt->cfg.workdir, &slot, i);
            }
        }
        AcceptHealth(g_rt->listen);
    }
    return result;
}

DWORD RunServiceLoop(const Config& cfg) {
    if (!g_service.stop_event) {
        Log(L"stop event is missing");
        return ERROR_INVALID_HANDLE;
    }
    WSADATA wsa{};
    if (::WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        Log(L"WSAStartup failed: %d", ::WSAGetLastError());
        return static_cast<DWORD>(::WSAGetLastError());
    }
    ServiceRuntime runtime;
    runtime.cfg = cfg;
    g_rt = &runtime;
    if (!ListenHealth(runtime.cfg, &runtime.listen)) {
        const int wsa_err = ::WSAGetLastError();
        g_rt = nullptr;
        ::WSACleanup();
        return wsa_err != 0 ? static_cast<DWORD>(wsa_err) : ERROR_OPEN_FAILED;
    }
    runtime.job = CreateWorkerJob();
    if (!runtime.job) {
        ::closesocket(runtime.listen);
        g_rt = nullptr;
        ::WSACleanup();
        return ERROR_INVALID_HANDLE;
    }
    runtime.slots.reserve(runtime.cfg.worker.size());
    for (const auto& cmd : runtime.cfg.worker) {
        WorkerSlot slot;
        slot.command = cmd;
        runtime.slots.push_back(std::move(slot));
    }
    for (size_t i = 0; i < runtime.slots.size(); ++i) {
        SpawnWorker(runtime.job, runtime.cfg.workdir, &runtime.slots[i], i);
    }
    const DWORD result = SuperviseWorkers(runtime.job, runtime.listen,
                                          &runtime.slots, runtime.cfg.workdir);
    if (runtime.listen != INVALID_SOCKET) {
        ::closesocket(runtime.listen);
        runtime.listen = INVALID_SOCKET;
    }
    // Last handle close kills every process still in the job.
    ::CloseHandle(runtime.job);
    for (auto& slot : runtime.slots) {
        if (slot.process) {
            ::CloseHandle(slot.process);
            slot.process = nullptr;
        }
    }
    g_rt = nullptr;
    ::WSACleanup();
    Log(L"service stopped");
    return result;
}
std::filesystem::path ExeDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n =
        ::GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(std::size(buf)));
    if (n == 0 || n == std::size(buf)) {
        return std::filesystem::current_path();
    }
    return std::filesystem::path(buf, buf + n).parent_path();
}

std::wstring Quote(std::wstring_view s) {
    std::wstring out;
    out.reserve(s.size() + 2);
    out.push_back(L'"');
    for (const wchar_t c : s) {
        if (c == L'"') {
            out.push_back(L'"');
        }
        out.push_back(c);
    }
    out.push_back(L'"');
    return out;
}

// ---------------------------------------------------------------------------
// SCM management commands
// ---------------------------------------------------------------------------

class ScmHandle {
  public:
    ScmHandle() = default;
    explicit ScmHandle(SC_HANDLE h) : h_(h) {}
    ~ScmHandle() {
        if (h_) ::CloseServiceHandle(h_);
    }
    ScmHandle(const ScmHandle&) = delete;
    ScmHandle& operator=(const ScmHandle&) = delete;
    SC_HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }
    SC_HANDLE release() {
        SC_HANDLE h = h_;
        h_ = nullptr;
        return h;
    }

  private:
    SC_HANDLE h_ = nullptr;
};

int InstallService(const std::wstring& config_path) {
    ScmHandle scm(::OpenSCManagerW(nullptr, nullptr,
                                 SC_MANAGER_CREATE_SERVICE));
    if (!scm) {
        Log( L"OpenSCManagerW failed: %lu\n",
                      ::GetLastError());
        return 1;
    }

    wchar_t exe[MAX_PATH];
    if (!::GetModuleFileNameW(nullptr, exe,
                              static_cast<DWORD>(std::size(exe)))) {
        Log( L"GetModuleFileNameW failed: %lu\n",
                      ::GetLastError());
        return 1;
    }

    std::wstring bin_path = Quote(exe);
    if (!config_path.empty()) {
        bin_path += L" --config ";
        bin_path += Quote(config_path);
    }

    ScmHandle svc(::CreateServiceW(
        scm.get(), kServiceName, kServiceDisplayName,
        SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_DEMAND_START, SERVICE_ERROR_NORMAL, bin_path.c_str(),
        nullptr, nullptr, nullptr, nullptr, nullptr));
    if (!svc) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SERVICE_EXISTS) {
            Log(
                          L"Service %ls already exists; use --uninstall first "
                          L"to reinstall.\n",
                          kServiceName);
        } else {
            Log( L"CreateServiceW failed: %lu\n", err);
        }
        return 1;
    }

    std::fwprintf(stdout, L"Service %ls installed (binPath: %ls)\n",
                  kServiceName, bin_path.c_str());
    return 0;
}

int UninstallService() {
    ScmHandle scm(::OpenSCManagerW(nullptr, nullptr,
                                 SC_MANAGER_CONNECT));
    if (!scm) {
        Log( L"OpenSCManagerW failed: %lu\n",
                      ::GetLastError());
        return 1;
    }
    ScmHandle svc(::OpenServiceW(scm.get(), kServiceName, DELETE));
    if (!svc) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            Log( L"Service %ls is not installed.\n",
                          kServiceName);
        } else {
            Log( L"OpenServiceW failed: %lu\n", err);
        }
        return 1;
    }
    if (!::DeleteService(svc.get())) {
        Log( L"DeleteService failed: %lu\n",
                      ::GetLastError());
        return 1;
    }
    std::fwprintf(stdout, L"Service %ls uninstalled.\n", kServiceName);
    return 0;
}

// Waits for the service to leave *_PENDING states, up to timeout_ms.
bool WaitForServiceState(SC_HANDLE svc, DWORD desired, DWORD timeout_ms) {
    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    const DWORD deadline = ::GetTickCount() + timeout_ms;
    for (;;) {
        if (!::QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO,
                                    reinterpret_cast<LPBYTE>(&ssp),
                                    sizeof(ssp), &needed)) {
            return false;
        }
        if (ssp.dwCurrentState == desired) {
            return true;
        }
        if (ssp.dwCurrentState != SERVICE_START_PENDING &&
            ssp.dwCurrentState != SERVICE_STOP_PENDING) {
            return false;  // Stuck in an unexpected state.
        }
        if (::GetTickCount() >= deadline) {
            return false;
        }
        ::Sleep(250);
    }
}

int StartServiceCommand() {
    ScmHandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        Log( L"OpenSCManagerW failed: %lu\n",
                      ::GetLastError());
        return 1;
    }
    ScmHandle svc(::OpenServiceW(scm.get(), kServiceName,
                                 SERVICE_START | SERVICE_QUERY_STATUS));
    if (!svc) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            Log( L"Service %ls is not installed.\n",
                          kServiceName);
        } else {
            Log( L"OpenServiceW failed: %lu\n", err);
        }
        return 1;
    }
    if (!::StartServiceW(svc.get(), 0, nullptr)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SERVICE_ALREADY_RUNNING) {
            std::fwprintf(stdout, L"Service %ls is already running.\n",
                          kServiceName);
            return 0;
        }
        Log( L"StartServiceW failed: %lu\n", err);
        return 1;
    }
    if (!WaitForServiceState(svc.get(), SERVICE_RUNNING, 30000)) {

        Log( L"Service %ls did not reach RUNNING.\n",
                      kServiceName);
        return 1;
    }
    std::fwprintf(stdout, L"Service %ls started.\n", kServiceName);
    return 0;
}

int StopServiceCommand() {
    ScmHandle scm(::OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!scm) {
        Log( L"OpenSCManagerW failed: %lu\n",
                      ::GetLastError());
        return 1;
    }
    ScmHandle svc(::OpenServiceW(scm.get(), kServiceName,
                                 SERVICE_STOP | SERVICE_QUERY_STATUS));
    if (!svc) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SERVICE_DOES_NOT_EXIST) {
            Log( L"Service %ls is not installed.\n",
                          kServiceName);
        } else {
            Log( L"OpenServiceW failed: %lu\n", err);
        }
        return 1;
    }

    SERVICE_STATUS_PROCESS ssp{};
    DWORD needed = 0;
    if (::QueryServiceStatusEx(svc.get(), SC_STATUS_PROCESS_INFO,
                               reinterpret_cast<LPBYTE>(&ssp), sizeof(ssp),
                               &needed) &&
        ssp.dwCurrentState == SERVICE_STOPPED) {
        std::fwprintf(stdout, L"Service %ls is already stopped.\n",
                      kServiceName);
        return 0;
    }

    SERVICE_STATUS status{};
    if (!::ControlService(svc.get(), SERVICE_CONTROL_STOP, &status)) {
        const DWORD err = ::GetLastError();
        if (err == ERROR_SERVICE_NOT_ACTIVE) {
            std::fwprintf(stdout, L"Service %ls is already stopped.\n",
                          kServiceName);
            return 0;
        }
        Log( L"ControlService(STOP) failed: %lu\n", err);
        return 1;
    }
    if (!WaitForServiceState(svc.get(), SERVICE_STOPPED, 30000)) {
        Log( L"Service %ls did not reach STOPPED.\n",
                      kServiceName);
        return 1;
    }
    std::fwprintf(stdout, L"Service %ls stopped.\n", kServiceName);
    return 0;
}
#endif  // _WIN32

void PrintUsage(const wchar_t* prog) {
    std::fwprintf(
        stdout,
        L"Usage: %ls [command] [--config PATH]\n"
        L"\n"
        L"With no command, runs as the Windows service %ls when started by\n"
        L"the Service Control Manager, otherwise runs in console mode.\n"
        L"\n"
        L"Commands (Windows only):\n"
        L"  --install     install the service (optional --config is baked in)\n"
        L"  --uninstall   remove the service\n"
        L"  --start       start the installed service\n"
        L"  --stop        stop the running service\n"
        L"  --console     force console mode in the foreground\n"
        L"\n"
        L"Options:\n"
        L"  --config PATH configuration file (default: maclaw-llm.json next\n"
        L"                to the executable)\n"
        L"  --help        show this help\n",
        prog,
#ifdef _WIN32
        kServiceName
#else
        L"MaClawLLM"
#endif
    );
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

enum class Command {
    kNone,       // Service if SCM-launched, else console.
    kConsole,
    kInstall,
    kUninstall,
    kStart,
    kStop,
    kHelp,
    kInvalid,
};

struct Options {
    Command command = Command::kNone;
    std::wstring config_arg;    // Raw --config value (may be relative).
};

Options ParseArgs(int argc, wchar_t* argv[]) {
    Options opt;
    bool command_seen = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring_view arg = argv[i];
        Command c = Command::kInvalid;
        if (arg == L"--install") {
            c = Command::kInstall;
        } else if (arg == L"--uninstall") {
            c = Command::kUninstall;
        } else if (arg == L"--start") {
            c = Command::kStart;
        } else if (arg == L"--stop") {
            c = Command::kStop;
        } else if (arg == L"--console") {
            c = Command::kConsole;
        } else if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
            c = Command::kHelp;
        } else if (arg == L"--config") {
            if (i + 1 >= argc) {
                Log( L"--config requires a path.\n");
                opt.command = Command::kInvalid;
                return opt;
            }
            opt.config_arg = argv[++i];
            continue;
        } else {
            Log( L"Unknown argument: %ls\n", argv[i]);
            opt.command = Command::kInvalid;
            return opt;
        }
        if (command_seen) {
            Log( L"Conflicting commands given.\n");
            opt.command = Command::kInvalid;
            return opt;
        }
        command_seen = true;
        opt.command = c;
    }
    return opt;
}

}  // namespace

int wmain(int argc, wchar_t* argv[]) {
    const Options opt = ParseArgs(argc, argv);

    switch (opt.command) {
        case Command::kInvalid:
            PrintUsage(argv[0]);
            return 2;
        case Command::kHelp:
            PrintUsage(argv[0]);
            return 0;
        default:
            break;
    }

#ifdef _WIN32
    // Resolve --config: relative paths are anchored to the executable's
    // directory; the default is maclaw-llm.json next to the exe.
    std::filesystem::path config;
    if (!opt.config_arg.empty()) {
        config = opt.config_arg;
        if (config.is_relative()) {
            config = ExeDir() / config;
        }
    } else {
        config = ExeDir() / kDefaultConfigName;
    }
    g_config_path = config.lexically_normal().wstring();

    switch (opt.command) {
        case Command::kInstall:
            return InstallService(g_config_path);
        case Command::kUninstall:
            return UninstallService();
        case Command::kStart:
            return StartServiceCommand();
        case Command::kStop:
            return StopServiceCommand();
        case Command::kConsole:
            return RunConsole();
        case Command::kNone: {
            // No arguments: run as a service when the SCM launched us,
            // otherwise fall back to console mode.
            DWORD err = 0;
            if (RunAsService(&err)) {
                return 0;
            }
            if (err == ERROR_FAILED_SERVICE_CONTROLLER_CONNECT) {
                return RunConsole();
            }
            Log( L"StartServiceCtrlDispatcherW failed: %lu\n",
                          err);
            return static_cast<int>(err);
        }
        default:
            return 2;
    }
#else   // !_WIN32
    // Service management is Windows-only; other platforms are not supported.
    switch (opt.command) {
        case Command::kInstall:
        case Command::kUninstall:
        case Command::kStart:
        case Command::kStop:
            std::fwprintf(stderr,
                          L"--install/--uninstall/--start/--stop are only "
                          L"supported on Windows.\n");
            return 1;
        default:
            std::fwprintf(stderr,
                          L"maclaw-llm-service is only supported on Windows.\n");
            return 1;
    }
#endif  // _WIN32
}
