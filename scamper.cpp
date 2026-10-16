// C++ port of scamper.ps1: nudges desktop icons a few pixels at a time, and can
// put them back with -Restore. The saved layout uses the PowerShell version's
// JSON format, and a layout that version saved is picked up (and renamed).
//
// Build (Developer Command Prompt for VS):
//   rc /nologo scamper.rc
//   cl /std:c++17 /EHsc /O2 /W4 /DUNICODE /D_UNICODE scamper.cpp scamper.res

#define NOMINMAX
// Target Windows 7 so any newer API fails at compile time instead of at load time.
#define _WIN32_WINNT 0x0601
#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <exdisp.h>
#include <dwmapi.h>
#include <wincrypt.h>
#include <wrl/client.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "advapi32.lib")

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

constexpr wchar_t kStateFileName[] = L"scamper-originalDesktopPosition.json";
constexpr wchar_t kLegacyStateFileName[] = L"DesktopDrift-OriginalLayout.json";  // written by scamper.ps1
constexpr int kAppIconId = 1;  // matches scamper.rc

constexpr DWORD kPositionFlags = FWF_AUTOARRANGE | FWF_SNAPTOGRID;
// {4C96BE40-915C-11CF-99D3-00AA004AE837}
constexpr GUID kSidTopLevelBrowser = {0x4C96BE40, 0x915C, 0x11CF, {0x99, 0xD3, 0x00, 0xAA, 0x00, 0x4A, 0xE8, 0x37}};

std::mt19937 g_random{std::random_device{}()};

// SetThreadDpiAwarenessContext is Windows 10 1607+, so look it up at run time.
using SetThreadDpiAwarenessContextFn = HANDLE(WINAPI*)(HANDLE);
const auto g_setThreadDpiAwarenessContext = reinterpret_cast<SetThreadDpiAwarenessContextFn>(
    GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetThreadDpiAwarenessContext"));
const HANDLE kDpiPerMonitorAwareV2 = reinterpret_cast<HANDLE>(-4);  // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2

int RandomBetween(int low, int high) {  // inclusive on both ends
    return std::uniform_int_distribution<int>(low, high)(g_random);
}

struct IconPosition {
    std::string key;  // base64 of the item's PIDL bytes
    int x = 0;
    int y = 0;
};

struct DesktopSnapshot {
    DWORD folderFlags = 0;
    std::vector<IconPosition> items;
};

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size, nullptr, nullptr);
    return result;
}

void Check(HRESULT hr, const char* action) {
    if (FAILED(hr)) {
        char message[256];
        std::snprintf(message, sizeof message, "%s failed (0x%08lX).", action, static_cast<unsigned long>(hr));
        throw std::runtime_error(message);
    }
}

// ---------------------------------------------------------------------------
// Fullscreen detection

bool CoversMonitor(const RECT& window, const RECT& monitor) {
    constexpr int tolerance = 2;
    return window.right > window.left && window.bottom > window.top
        && window.left <= monitor.left + tolerance
        && window.top <= monitor.top + tolerance
        && window.right >= monitor.right - tolerance
        && window.bottom >= monitor.bottom - tolerance;
}

bool IsForegroundFullscreen() {
    HWND window = GetForegroundWindow();
    if (!window || window == GetShellWindow() || window == GetDesktopWindow()
        || !IsWindowVisible(window) || IsIconic(window)) return false;
    wchar_t className[256] = {};
    GetClassNameW(window, className, static_cast<int>(std::size(className)));
    for (const wchar_t* shellClass : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"}) {
        if (std::wcscmp(className, shellClass) == 0) return false;
    }

    // Compare physical screen coordinates even with mixed monitor scaling.
    // Older Windows has no per-thread DPI context; wmain makes the process DPI aware instead.
    struct DpiScope {
        HANDLE old = g_setThreadDpiAwarenessContext ? g_setThreadDpiAwarenessContext(kDpiPerMonitorAwareV2) : nullptr;
        ~DpiScope() { if (old) g_setThreadDpiAwarenessContext(old); }
    } dpiScope;

    MONITORINFO monitor = {sizeof monitor};
    HMONITOR handle = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!handle || !GetMonitorInfoW(handle, &monitor)) return false;
    RECT bounds;
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof bounds))
        && !GetWindowRect(window, &bounds)) return false;
    return CoversMonitor(bounds, monitor.rcMonitor);
}

// ---------------------------------------------------------------------------
// Desktop folder view

struct DesktopConnection {
    ComPtr<IShellWindows> windows;
    ComPtr<IDispatch> dispatch;
    ComPtr<IShellBrowser> browser;
    ComPtr<IFolderView2> view;
};

DesktopConnection GetView() {
    DesktopConnection connection;
    Check(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&connection.windows)), "Create shell windows");
    VARIANT location;
    VariantInit(&location);
    location.vt = VT_I4;
    location.lVal = 0;  // CSIDL_DESKTOP
    VARIANT root;
    VariantInit(&root);
    long hwnd = 0;
    Check(connection.windows->FindWindowSW(&location, &root, SWC_DESKTOP, &hwnd, SWFO_NEEDDISPATCH, &connection.dispatch),
          "Find desktop window");
    if (!connection.dispatch) throw std::runtime_error("Could not find the Windows desktop view.");
    ComPtr<IServiceProvider> services;
    Check(connection.dispatch.As(&services), "Find desktop shell browser");
    Check(services->QueryService(kSidTopLevelBrowser, IID_PPV_ARGS(&connection.browser)), "Find desktop shell browser");
    ComPtr<IShellView> shellView;
    Check(connection.browser->QueryActiveShellView(&shellView), "Find desktop shell view");
    Check(shellView.As(&connection.view), "Get desktop folder view");
    return connection;
}

// Shell PIDLs are __unaligned on x64, so keep that qualifier on the owned pointer.
struct PidlDeleter {
    void operator()(ITEMIDLIST UNALIGNED* p) const { ILFree(p); }
};
using PidlPtr = std::unique_ptr<ITEMIDLIST UNALIGNED, PidlDeleter>;

// Size of an ID list including its two-byte terminator, or 0 if it is malformed
// or runs past `limit` bytes.
size_t PidlSize(const BYTE* bytes, size_t limit) {
    size_t size = 0;
    for (;;) {
        if (size + 2 > limit) return 0;
        USHORT part;
        std::memcpy(&part, bytes + size, sizeof part);
        if (part == 0) return size + 2;
        if (part < 2) return 0;
        size += part;
    }
}

std::string EncodeBase64(const BYTE* data, DWORD length) {
    constexpr DWORD flags = CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF;
    DWORD chars = 0;
    if (!CryptBinaryToStringA(data, length, flags, nullptr, &chars)) throw std::runtime_error("Could not encode desktop item identifier.");
    std::string result(chars, '\0');
    if (!CryptBinaryToStringA(data, length, flags, result.data(), &chars)) throw std::runtime_error("Could not encode desktop item identifier.");
    result.resize(chars);
    return result;
}

std::string CopyPidl(PCUITEMID_CHILD pidl) {
    size_t size = PidlSize(reinterpret_cast<const BYTE*>(pidl), 65536);
    if (size == 0) throw std::runtime_error("Invalid desktop item identifier.");
    return EncodeBase64(reinterpret_cast<const BYTE*>(pidl), static_cast<DWORD>(size));
}

PidlPtr MakePidl(const std::string& key) {
    DWORD length = 0;
    if (!CryptStringToBinaryA(key.c_str(), static_cast<DWORD>(key.size()), CRYPT_STRING_BASE64, nullptr, &length, nullptr, nullptr))
        throw std::runtime_error("Saved desktop item identifier is not valid base64.");
    PidlPtr pidl(static_cast<ITEMIDLIST*>(CoTaskMemAlloc(std::max<DWORD>(length, 2))));
    if (!pidl) throw std::bad_alloc();
    auto bytes = reinterpret_cast<BYTE*>(pidl.get());
    if (!CryptStringToBinaryA(key.c_str(), static_cast<DWORD>(key.size()), CRYPT_STRING_BASE64, bytes, &length, nullptr, nullptr))
        throw std::runtime_error("Saved desktop item identifier is not valid base64.");
    // Hand Explorer only well-formed identifiers, since this came from a file.
    if (PidlSize(bytes, length) == 0) throw std::runtime_error("Saved desktop item identifier is malformed.");
    return pidl;
}

std::vector<IconPosition> ReadPositions(IFolderView2* view) {
    std::vector<IconPosition> result;
    ComPtr<IEnumIDList> enumerator;
    Check(view->Items(SVGIO_ALLVIEW, IID_PPV_ARGS(&enumerator)), "Enumerate desktop icons");
    for (;;) {
        PITEMID_CHILD raw = nullptr;
        ULONG fetched = 0;
        Check(enumerator->Next(1, &raw, &fetched), "Enumerate desktop icons");
        if (fetched == 0) break;
        PidlPtr pidl(raw);
        POINT point;
        Check(view->GetItemPosition(pidl.get(), &point), "Read desktop icon position");
        result.push_back({CopyPidl(pidl.get()), point.x, point.y});
    }
    return result;
}

void Apply(IFolderView2* view, const std::vector<IconPosition>& items) {
    if (items.empty()) return;
    std::vector<PidlPtr> owned;
    std::vector<PCUITEMID_CHILD> pidls;
    std::vector<POINT> points;
    for (const IconPosition& item : items) {
        owned.push_back(MakePidl(item.key));
        pidls.push_back(owned.back().get());
        points.push_back({item.x, item.y});
    }
    Check(view->SelectAndPositionItems(static_cast<UINT>(items.size()), pidls.data(), points.data(), SVSI_POSITIONITEM),
          "Move desktop icons");
}

// Partial Fisher-Yates shuffle: sample without selecting an icon twice.
std::vector<IconPosition> PickIcons(std::vector<IconPosition> items, int limit) {
    size_t count = limit == 0 ? items.size() : std::min(static_cast<size_t>(limit), items.size());
    for (size_t i = 0; i < count; i++) {
        size_t other = static_cast<size_t>(RandomBetween(static_cast<int>(i), static_cast<int>(items.size() - 1)));
        std::swap(items[i], items[other]);
    }
    items.resize(count);
    return items;
}

DesktopSnapshot Capture() {
    DesktopConnection connection = GetView();
    DesktopSnapshot snapshot;
    Check(connection.view->GetCurrentFolderFlags(&snapshot.folderFlags), "Read desktop layout settings");
    snapshot.items = ReadPositions(connection.view.Get());
    return snapshot;
}

void BeginDrift() {
    DesktopConnection connection = GetView();
    Check(connection.view->SetCurrentFolderFlags(kPositionFlags, 0), "Turn off desktop auto-arrange and grid snapping");
}

int Drift(int maxPixels, int iconsPerTick, bool fullscreenOnly) {
    if (fullscreenOnly && !IsForegroundFullscreen()) return 0;
    DesktopConnection connection = GetView();
    std::vector<IconPosition> items = PickIcons(ReadPositions(connection.view.Get()), iconsPerTick);
    for (IconPosition& item : items) {
        int dx = RandomBetween(-maxPixels, maxPixels);
        int dy = RandomBetween(-maxPixels, maxPixels);
        if (dx == 0 && dy == 0) dx = 1;
        item.x += dx;
        item.y += dy;
    }
    // Recheck in case the presenter left fullscreen while enumerating icons.
    if (fullscreenOnly && !IsForegroundFullscreen()) return 0;
    Apply(connection.view.Get(), items);
    return static_cast<int>(items.size());
}

void Restore(const DesktopSnapshot& snapshot) {
    DesktopConnection connection = GetView();
    Check(connection.view->SetCurrentFolderFlags(kPositionFlags, 0), "Disable automatic arrangement for restore");
    Apply(connection.view.Get(), snapshot.items);
    Check(connection.view->SetCurrentFolderFlags(kPositionFlags, snapshot.folderFlags & kPositionFlags),
          "Restore desktop arrangement settings");
}

// ---------------------------------------------------------------------------
// Layout file (same shape ConvertTo-Json produced in the PowerShell version)

struct Json {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    double number = 0;
    std::string text;
    std::vector<Json> elements;          // array elements, or object values
    std::vector<std::string> keys;       // object keys, parallel to elements

    const Json* Find(const char* name) const {  // case-insensitive, like PowerShell
        for (size_t i = 0; i < keys.size(); i++) {
            if (_stricmp(keys[i].c_str(), name) == 0) return &elements[i];
        }
        return nullptr;
    }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& text) : text_(text) {}

    Json ParseDocument() {
        Json value = ParseValue();
        SkipSpace();
        if (pos_ != text_.size()) Fail();
        return value;
    }

private:
    [[noreturn]] void Fail() { throw std::runtime_error("The saved layout file is not valid JSON."); }

    void SkipSpace() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\r' || text_[pos_] == '\n')) pos_++;
    }

    bool Consume(char c) {
        SkipSpace();
        if (pos_ < text_.size() && text_[pos_] == c) { pos_++; return true; }
        return false;
    }

    void Expect(char c) { if (!Consume(c)) Fail(); }

    bool ConsumeWord(const char* word) {
        size_t length = std::strlen(word);
        if (text_.compare(pos_, length, word) != 0) return false;
        pos_ += length;
        return true;
    }

    Json ParseValue() {
        SkipSpace();
        if (pos_ >= text_.size()) Fail();
        Json value;
        char c = text_[pos_];
        if (c == '{') {
            pos_++;
            value.type = Json::Type::Object;
            if (Consume('}')) return value;
            do {
                SkipSpace();
                value.keys.push_back(ParseString());
                Expect(':');
                value.elements.push_back(ParseValue());
            } while (Consume(','));
            Expect('}');
        } else if (c == '[') {
            pos_++;
            value.type = Json::Type::Array;
            if (Consume(']')) return value;
            do value.elements.push_back(ParseValue()); while (Consume(','));
            Expect(']');
        } else if (c == '"') {
            value.type = Json::Type::String;
            value.text = ParseString();
        } else if (ConsumeWord("true") || ConsumeWord("false")) {
            value.type = Json::Type::Bool;
        } else if (ConsumeWord("null")) {
            value.type = Json::Type::Null;
        } else {
            const char* start = text_.c_str() + pos_;
            char* end = nullptr;
            value.number = std::strtod(start, &end);
            if (end == start) Fail();
            pos_ += end - start;
            value.type = Json::Type::Number;
        }
        return value;
    }

    unsigned ParseHex4() {
        if (pos_ + 4 > text_.size()) Fail();
        unsigned code = 0;
        for (int i = 0; i < 4; i++) {
            char h = text_[pos_++];
            code <<= 4;
            if (h >= '0' && h <= '9') code |= h - '0';
            else if (h >= 'a' && h <= 'f') code |= h - 'a' + 10;
            else if (h >= 'A' && h <= 'F') code |= h - 'A' + 10;
            else Fail();
        }
        return code;
    }

    static void AppendUtf8(std::string& out, unsigned code) {
        if (code < 0x80) out += static_cast<char>(code);
        else if (code < 0x800) { out += static_cast<char>(0xC0 | (code >> 6)); out += static_cast<char>(0x80 | (code & 0x3F)); }
        else if (code < 0x10000) {
            out += static_cast<char>(0xE0 | (code >> 12));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (code >> 18));
            out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (code & 0x3F));
        }
    }

    std::string ParseString() {
        if (pos_ >= text_.size() || text_[pos_] != '"') Fail();
        pos_++;
        std::string out;
        for (;;) {
            if (pos_ >= text_.size()) Fail();
            char c = text_[pos_++];
            if (c == '"') return out;
            if (c != '\\') { out += c; continue; }
            if (pos_ >= text_.size()) Fail();
            switch (char e = text_[pos_++]) {
                case '"': case '\\': case '/': out += e; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned code = ParseHex4();
                    if (code >= 0xD800 && code < 0xDC00 && text_.compare(pos_, 2, "\\u") == 0) {
                        pos_ += 2;
                        unsigned low = ParseHex4();
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    }
                    AppendUtf8(out, code);
                    break;
                }
                default: Fail();
            }
        }
    }

    const std::string& text_;
    size_t pos_ = 0;
};

long long JsonInteger(const Json* value, long long low, long long high, const char* name) {
    if (!value || value->type != Json::Type::Number || value->number < static_cast<double>(low)
        || value->number > static_cast<double>(high))
        throw std::runtime_error(std::string("The saved layout file has an invalid ") + name + ".");
    return std::llround(value->number);
}

DesktopSnapshot ParseSnapshot(const std::string& text) {
    Json root = JsonParser(text).ParseDocument();
    if (root.type != Json::Type::Object) throw std::runtime_error("The saved layout file is not a desktop layout.");
    DesktopSnapshot snapshot;
    snapshot.folderFlags = static_cast<DWORD>(JsonInteger(root.Find("FolderFlags"), 0, 0xFFFFFFFFLL, "FolderFlags"));

    // Mirror @($snapshot.Items): null means none, a lone object means one.
    const Json* items = root.Find("Items");
    std::vector<const Json*> entries;
    if (items && items->type == Json::Type::Array) {
        for (const Json& entry : items->elements) entries.push_back(&entry);
    } else if (items && items->type != Json::Type::Null) {
        entries.push_back(items);
    }
    for (const Json* entry : entries) {
        const Json* key = entry->type == Json::Type::Object ? entry->Find("Key") : nullptr;
        if (!key || key->type != Json::Type::String) throw std::runtime_error("The saved layout file has an icon without a Key.");
        snapshot.items.push_back({key->text,
                                  static_cast<int>(JsonInteger(entry->Find("X"), INT_MIN, INT_MAX, "icon X")),
                                  static_cast<int>(JsonInteger(entry->Find("Y"), INT_MIN, INT_MAX, "icon Y"))});
    }
    return snapshot;
}

std::string JsonString(const std::string& text) {
    std::string out = "\"";
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 0x20) { char escape[8]; std::snprintf(escape, sizeof escape, "\\u%04x", c); out += escape; }
        else out += static_cast<char>(c);
    }
    return out + "\"";
}

std::string FormatSnapshot(const DesktopSnapshot& snapshot) {
    std::ostringstream out;
    out << "{\n    \"FolderFlags\": " << snapshot.folderFlags << ",\n    \"Items\": [";
    for (size_t i = 0; i < snapshot.items.size(); i++) {
        const IconPosition& item = snapshot.items[i];
        out << (i ? ",\n" : "\n") << "        { \"Key\": " << JsonString(item.key)
            << ", \"X\": " << item.x << ", \"Y\": " << item.y << " }";
    }
    out << (snapshot.items.empty() ? "]\n}\n" : "\n    ]\n}\n");
    return out.str();
}

std::string ReadFile(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("Could not open '" + ToUtf8(path.wstring()) + "'.");
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.compare(0, 3, "\xEF\xBB\xBF") == 0) text.erase(0, 3);  // Windows PowerShell writes a BOM
    return text;
}

void WriteFile(const fs::path& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out || !out.write(text.data(), static_cast<std::streamsize>(text.size())) || !out.flush())
        throw std::runtime_error("Could not write '" + ToUtf8(path.wstring()) + "'.");
}

fs::path ExecutablePath() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) throw std::runtime_error("Could not find this program's location.");
        if (length < buffer.size()) { buffer.resize(length); break; }
        buffer.resize(buffer.size() * 2);
    }
    return fs::path(buffer);
}

void RestoreSaved(const fs::path& statePath) {
    if (!fs::exists(statePath)) {
        throw std::runtime_error("No saved layout found at '" + ToUtf8(statePath.wstring())
                                 + "'. Run this program once before using -Restore.");
    }
    Restore(ParseSnapshot(ReadFile(statePath)));
    fs::remove(statePath);
}

void PrepareDrift(const fs::path& statePath) {
    if (fs::exists(statePath)) {
        // Start every run from the original layout so drift doesn't pile up across runs.
        // The file keeps the original until an explicit -Restore removes it.
        DesktopSnapshot original = ParseSnapshot(ReadFile(statePath));
        try {
            Restore(original);
        } catch (const std::exception&) {
            // Best effort (an icon may have been deleted since); keep drifting from where icons are.
        }
    } else {
        WriteFile(statePath, FormatSnapshot(Capture()));
    }
    BeginDrift();
}

// ---------------------------------------------------------------------------
// Command line (PowerShell-style: -Name value, -Name:value, case-insensitive, unique prefixes)

struct Options {
    int intervalSeconds = 15;
    int pixelsPerStep = 5;
    int iconsPerTick = 0;  // 0 moves all icons; otherwise choose this many distinct icons each tick.
    bool fullscreenOnly = false;  // Drift only while the foreground app covers an entire monitor.
    bool restore = false;
    bool tray = true;            // Run in the background with a notification-area icon.
    bool startup = false;        // Also start in the tray at every sign-in.
    bool removeStartup = false;
    bool help = false;
};

void PrintUsage() {
    std::printf(
        "Usage: scamper [-IntervalSeconds 1-3600] [-PixelsPerStep 1-20] [-IconsPerTick 0-2147483647]\n"
        "               [-FullscreenOnly] [-Console] [-Startup]\n"
        "       scamper -Restore\n"
        "       scamper -RemoveStartup\n"
        "\n"
        "Runs in the background with a notification-area icon. Each start first puts icons back\n"
        "where they were before the first run, then drifts from there.\n"
        "\n"
        "  -IntervalSeconds  Seconds between moves (default 15).\n"
        "  -PixelsPerStep    Maximum pixels an icon moves per tick (default 5).\n"
        "  -IconsPerTick     How many random icons move per tick; 0 moves all (default 0).\n"
        "  -FullscreenOnly   Only move icons while the foreground app fills a monitor.\n"
        "  -Console          Run in this console instead of the tray (Ctrl+C stops it).\n"
        "  -Startup          Start in the tray at every sign-in with these options (and start now).\n"
        "  -RemoveStartup    Stop starting at sign-in.\n"
        "  -Restore          Stop any running copy and put icons back where they were before the first run.\n");
}

Options ParseArgs(int argc, wchar_t** argv) {
    struct Param { const wchar_t* name; int* target; long long low, high; bool* flag; };
    Options options;
    bool console = false;
    const Param params[] = {
        {L"IntervalSeconds", &options.intervalSeconds, 1, 3600, nullptr},
        {L"PixelsPerStep", &options.pixelsPerStep, 1, 20, nullptr},
        {L"IconsPerTick", &options.iconsPerTick, 0, INT_MAX, nullptr},
        {L"FullscreenOnly", nullptr, 0, 0, &options.fullscreenOnly},
        {L"Restore", nullptr, 0, 0, &options.restore},
        {L"Tray", nullptr, 0, 0, &options.tray},  // the default; still accepted
        {L"Console", nullptr, 0, 0, &console},
        {L"Startup", nullptr, 0, 0, &options.startup},
        {L"RemoveStartup", nullptr, 0, 0, &options.removeStartup},
    };

    for (int i = 1; i < argc; i++) {
        std::wstring arg = argv[i];
        if (arg == L"-?" || arg == L"/?" || _wcsicmp(arg.c_str(), L"-h") == 0 || _wcsicmp(arg.c_str(), L"--help") == 0
            || _wcsicmp(arg.c_str(), L"-Help") == 0) {
            options.help = true;
            continue;
        }
        size_t start = arg.find_first_not_of(L"-/");
        if (start == 0 || start == std::wstring::npos) throw std::runtime_error("Unexpected argument '" + ToUtf8(arg) + "'.");
        std::wstring name = arg.substr(start), value;
        bool hasValue = false;
        size_t separator = name.find_first_of(L":=");
        if (separator != std::wstring::npos) {
            value = name.substr(separator + 1);
            name.resize(separator);
            hasValue = true;
        }

        const Param* match = nullptr;
        for (const Param& param : params) {
            if (_wcsnicmp(param.name, name.c_str(), name.size()) != 0) continue;
            if (match) throw std::runtime_error("Parameter '-" + ToUtf8(name) + "' is ambiguous.");
            match = &param;
        }
        if (!match) throw std::runtime_error("Unknown parameter '-" + ToUtf8(name) + "'.");

        if (match->flag) {
            bool on = true;
            if (hasValue) {
                const wchar_t* v = value.c_str();
                if (_wcsicmp(v, L"true") == 0 || _wcsicmp(v, L"$true") == 0 || wcscmp(v, L"1") == 0) on = true;
                else if (_wcsicmp(v, L"false") == 0 || _wcsicmp(v, L"$false") == 0 || wcscmp(v, L"0") == 0) on = false;
                else throw std::runtime_error("Switch '-" + ToUtf8(match->name) + "' takes true or false.");
            }
            *match->flag = on;
            continue;
        }

        if (!hasValue) {
            if (++i >= argc) throw std::runtime_error("Parameter '-" + ToUtf8(match->name) + "' needs a value.");
            value = argv[i];
        }
        wchar_t* end = nullptr;
        errno = 0;
        long long number = std::wcstoll(value.c_str(), &end, 10);
        if (value.empty() || *end != L'\0' || errno == ERANGE || number < match->low || number > match->high) {
            throw std::runtime_error("Parameter '-" + ToUtf8(match->name) + "' must be a whole number from "
                                     + std::to_string(match->low) + " to " + std::to_string(match->high) + ".");
        }
        *match->target = static_cast<int>(number);
    }
    if (console) options.tray = false;
    return options;
}

struct ComApartment {
    ComApartment() { Check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "Initialize COM"); }
    ~ComApartment() { CoUninitialize(); }
};

// ---------------------------------------------------------------------------
// Single instance, startup registration, and background launch

constexpr wchar_t kMutexName[] = L"Local\\Scamper.DesktopDrift";
constexpr wchar_t kStopEventName[] = L"Local\\Scamper.Stop";
constexpr wchar_t kTrayWindowClass[] = L"Scamper.TrayWindow";
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValue[] = L"Scamper";

// True when attached to any console, even a windowless one (CREATE_NO_WINDOW).
bool HasConsole() { return GetConsoleCP() != 0; }

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

void ShowError(const std::string& message) {
    MessageBoxW(nullptr, FromUtf8(message).c_str(), L"Scamper", MB_OK | MB_ICONERROR);
}

bool IsAlreadyRunning() {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
    if (!mutex) return false;
    CloseHandle(mutex);
    return true;
}

// Returns the stop event the running copy waits on; it and the (owned) mutex
// stay open until the process exits, which is how StopRunningCopy knows it's gone.
HANDLE ClaimSingleInstance() {
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        throw std::runtime_error("Scamper is already running. Use its tray icon, or run with -Restore to stop it.");
    }
    HANDLE stop = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
    if (!stop) throw std::runtime_error("Could not create Scamper's stop signal.");
    return stop;
}

// Asks a running copy (tray or console) to exit and waits until it has, so it
// can't drift icons again after a restore. Returns whether one was running.
bool StopRunningCopy() {
    HANDLE stop = OpenEventW(EVENT_MODIFY_STATE, FALSE, kStopEventName);
    if (!stop) return false;
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, kMutexName);
    SetEvent(stop);
    if (mutex) {
        DWORD result = WaitForSingleObject(mutex, 10000);
        if (result == WAIT_OBJECT_0 || result == WAIT_ABANDONED) ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    CloseHandle(stop);
    return true;
}

std::wstring StartupCommand(const Options& options) {
    std::wstring command = L"\"" + ExecutablePath().wstring() + L"\" -Tray"
        + L" -IntervalSeconds " + std::to_wstring(options.intervalSeconds)
        + L" -PixelsPerStep " + std::to_wstring(options.pixelsPerStep)
        + L" -IconsPerTick " + std::to_wstring(options.iconsPerTick);
    if (options.fullscreenOnly) command += L" -FullscreenOnly";
    return command;
}

bool IsStartupEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}

void SetStartup(const Options& options, bool enable) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        throw std::runtime_error("Could not open the startup programs registry key.");
    LSTATUS status;
    if (enable) {
        std::wstring command = StartupCommand(options);
        status = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                                static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    } else {
        status = RegDeleteValueW(key, kRunValue);
        if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (status != ERROR_SUCCESS)
        throw std::runtime_error(enable ? "Could not add Scamper to startup programs." : "Could not remove Scamper from startup programs.");
}

// Start a console-less copy with the same arguments, so the terminal (or the
// sign-in console window) can close while the tray copy keeps running.
void RelaunchDetached() {
    std::wstring exe = ExecutablePath().wstring();
    std::wstring commandLine = GetCommandLineW();
    std::vector<wchar_t> buffer(commandLine.begin(), commandLine.end());
    buffer.push_back(L'\0');
    STARTUPINFOW startup = {sizeof startup};
    PROCESS_INFORMATION process = {};
    if (!CreateProcessW(exe.c_str(), buffer.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("Could not start Scamper in the background.");
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
}

// ---------------------------------------------------------------------------
// Tray icon

constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kSetupTimer = 1, kDriftTimer = 2;
constexpr UINT kSetupRetryMs = 2000, kSetupAttempts = 30;  // Explorer can still be starting right after sign-in.
enum : UINT { kCmdPause = 1, kCmdStartup, kCmdRestore, kCmdExit };

struct TrayState {
    Options options;
    fs::path statePath;
    NOTIFYICONDATAW icon = {};
    UINT taskbarCreated = 0;
    UINT setupAttempts = 0;
    bool ready = false;
    bool paused = false;
} g_tray;

void UpdateTrayTip() {
    if (!g_tray.ready) swprintf_s(g_tray.icon.szTip, L"Scamper: waiting for the desktop");
    else if (g_tray.paused) swprintf_s(g_tray.icon.szTip, L"Scamper: paused");
    else swprintf_s(g_tray.icon.szTip, L"Scamper: drifting every %d s", g_tray.options.intervalSeconds);
    Shell_NotifyIconW(NIM_MODIFY, &g_tray.icon);
}

void TrySetup(HWND window) {
    try {
        PrepareDrift(g_tray.statePath);
    } catch (const std::exception& e) {
        if (++g_tray.setupAttempts < kSetupAttempts) return;
        KillTimer(window, kSetupTimer);
        ShowError(e.what());
        DestroyWindow(window);
        return;
    }
    KillTimer(window, kSetupTimer);
    g_tray.ready = true;
    SetTimer(window, kDriftTimer, static_cast<UINT>(g_tray.options.intervalSeconds) * 1000, nullptr);
    UpdateTrayTip();
}

void ShowTrayMenu(HWND window) {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (g_tray.paused ? MF_CHECKED : 0), kCmdPause, L"Pause");
    AppendMenuW(menu, MF_STRING | (IsStartupEnabled() ? MF_CHECKED : 0), kCmdStartup, L"Run at startup");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdRestore, L"Restore icons and exit");
    AppendMenuW(menu, MF_STRING, kCmdExit, L"Exit (leave icons)");
    POINT cursor;
    GetCursorPos(&cursor);
    SetForegroundWindow(window);  // so the menu closes when clicking elsewhere
    UINT command = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                    cursor.x, cursor.y, 0, window, nullptr));
    PostMessageW(window, WM_NULL, 0, 0);
    DestroyMenu(menu);

    try {
        switch (command) {
            case kCmdPause:
                g_tray.paused = !g_tray.paused;
                UpdateTrayTip();
                break;
            case kCmdStartup:
                SetStartup(g_tray.options, !IsStartupEnabled());
                break;
            case kCmdRestore:
                KillTimer(window, kDriftTimer);
                RestoreSaved(g_tray.statePath);
                DestroyWindow(window);
                break;
            case kCmdExit:
                DestroyWindow(window);
                break;
        }
    } catch (const std::exception& e) {
        ShowError(e.what());
        if (command == kCmdRestore && g_tray.ready) {
            SetTimer(window, kDriftTimer, static_cast<UINT>(g_tray.options.intervalSeconds) * 1000, nullptr);
        }
    }
}

LRESULT CALLBACK TrayWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (message == g_tray.taskbarCreated && message != 0) {  // Explorer restarted: put the icon back.
        Shell_NotifyIconW(NIM_ADD, &g_tray.icon);
        return 0;
    }
    switch (message) {
        case WM_TIMER:
            if (wParam == kSetupTimer) {
                TrySetup(window);
            } else if (wParam == kDriftTimer && !g_tray.paused) {
                try {
                    Drift(g_tray.options.pixelsPerStep, g_tray.options.iconsPerTick, g_tray.options.fullscreenOnly);
                } catch (const std::exception&) {
                    // Explorer may be restarting; try again next tick.
                }
            }
            return 0;
        case kTrayMessage:
            if (lParam == WM_RBUTTONUP || lParam == WM_LBUTTONUP) ShowTrayMenu(window);
            return 0;
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            Shell_NotifyIconW(NIM_DELETE, &g_tray.icon);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

int RunTray(const Options& options, const fs::path& statePath) {
    HANDLE stop = ClaimSingleInstance();
    g_tray.options = options;
    g_tray.statePath = statePath;
    g_tray.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSW windowClass = {};
    windowClass.lpfnWndProc = TrayWindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.lpszClassName = kTrayWindowClass;
    if (!RegisterClassW(&windowClass)) throw std::runtime_error("Could not create the tray window.");
    // A hidden top-level window (not message-only) so it receives TaskbarCreated.
    HWND window = CreateWindowW(kTrayWindowClass, L"Scamper", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr,
                                windowClass.hInstance, nullptr);
    if (!window) throw std::runtime_error("Could not create the tray window.");

    g_tray.icon.cbSize = sizeof g_tray.icon;
    g_tray.icon.hWnd = window;
    g_tray.icon.uID = 1;
    g_tray.icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_tray.icon.uCallbackMessage = kTrayMessage;
    g_tray.icon.hIcon = static_cast<HICON>(LoadImageW(windowClass.hInstance, MAKEINTRESOURCEW(kAppIconId), IMAGE_ICON,
                                                      GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    if (!g_tray.icon.hIcon) g_tray.icon.hIcon = LoadIconW(nullptr, IDI_APPLICATION);  // built without scamper.res
    Shell_NotifyIconW(NIM_ADD, &g_tray.icon);  // may fail early in sign-in; TaskbarCreated re-adds it
    UpdateTrayTip();

    SetTimer(window, kSetupTimer, kSetupRetryMs, nullptr);
    TrySetup(window);

    // Pump messages until the window closes or -Restore signals the stop event.
    for (;;) {
        if (MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0) {
            DestroyWindow(window);
            return 0;
        }
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) return 0;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    SetConsoleOutputCP(CP_UTF8);
    // Without per-thread DPI contexts, DWM frame bounds are physical pixels while
    // window and monitor rects are scaled for DPI-unaware apps; avoid the mismatch.
    // System awareness also keeps the tray menu sharp.
    SetProcessDPIAware();
    try {
        Options options = ParseArgs(argc, argv);
        if (options.help) {
            PrintUsage();
            return 0;
        }

        ComApartment com;
        fs::path directory = ExecutablePath().parent_path();
        fs::path statePath = directory / kStateFileName;
        if (!fs::exists(statePath) && fs::exists(directory / kLegacyStateFileName)) {
            fs::rename(directory / kLegacyStateFileName, statePath);
        }

        if (options.removeStartup) {
            SetStartup(options, false);
            std::printf("Scamper will no longer start when you sign in.\n");
            if (!options.restore) return 0;
        }

        if (options.restore) {
            if (StopRunningCopy()) std::printf("Stopped the running copy of Scamper.\n");
            RestoreSaved(statePath);
            std::printf("Restored the saved desktop icon positions and arrangement settings.\n");
            return 0;
        }

        if (options.startup) {
            SetStartup(options, true);
            options.tray = true;
        }

        if (options.tray) {
            // With a console (a terminal, or the window a sign-in launch opens), hand off to a detached copy.
            if (HasConsole()) {
                if (options.startup) std::printf("Scamper will start in the tray when you sign in.\n");
                if (IsAlreadyRunning()) {
                    if (options.startup) return 0;
                    throw std::runtime_error("Scamper is already running. Use its tray icon, or run with -Restore to stop it.");
                }
                RelaunchDetached();
                std::printf("Scamper is running in the notification area. Click its icon to pause, restore, or exit.\n");
                return 0;
            }
            return RunTray(options, statePath);
        }

        HANDLE stop = ClaimSingleInstance();
        PrepareDrift(statePath);
        std::string countDescription = options.iconsPerTick == 0
            ? "all icons"
            : "up to " + std::to_string(options.iconsPerTick) + " randomly chosen icons";
        const char* conditionDescription = options.fullscreenOnly
            ? " Only ticks while the foreground app fills a monitor; other ticks are skipped."
            : "";
        std::printf("Desktop icon drift is running. Every %d seconds, %s move by up to %d pixels.%s Press Ctrl+C to stop "
                    "the drift; icons will stay where they are. Run this program with -Restore to put them back.\n",
                    options.intervalSeconds, countDescription.c_str(), options.pixelsPerStep, conditionDescription);
        std::fflush(stdout);

        // A plain wait is fine on this STA thread: no COM objects are alive between ticks.
        while (WaitForSingleObject(stop, static_cast<DWORD>(options.intervalSeconds) * 1000) == WAIT_TIMEOUT) {
            Drift(options.pixelsPerStep, options.iconsPerTick, options.fullscreenOnly);
        }
        std::printf("Stopped by scamper -Restore.\n");
        return 0;
    } catch (const std::exception& e) {
        if (HasConsole()) std::fprintf(stderr, "Error: %s\n", e.what());
        else ShowError(e.what());
        return 1;
    }
}
