#include <windows.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <thumbcache.h>
#include <shlwapi.h>
#include <bcrypt.h>
#include <winrt/base.h>
#include <algorithm>
#include <memory>
#include <cstring>
#include <cwchar>
#include <utility>
#include <string>
#include <vector>

using namespace winrt;
static long objects = 0;
static constexpr GUID WordId =
    {0x06cfb719, 0x8050, 0x4cf6, {0xa5, 0x98, 0x17, 0x48, 0x47, 0x1b, 0x08, 0x49}};
static constexpr GUID GridId =
    {0x43f420f9, 0x8d44, 0x4be3, {0xae, 0xbb, 0xbd, 0x08, 0xa7, 0xdf, 0x4c, 0xd1}};
HRESULT RenderOfficePdfPage(IStream* source, UINT size, UINT page, HBITMAP* bitmap, UINT* count);

static std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    check_hresult(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &raw));
    std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> path(raw, CoTaskMemFree);
    return path.get();
}

struct Scratch {
    std::wstring input;
    std::wstring pdf;
    ~Scratch() {
        DeleteFileW(input.c_str());
        DeleteFileW(pdf.c_str());
    }
};

static std::vector<BYTE> ReadDocument(IStream* source) {
    constexpr ULONG maxSize = 32 * 1024 * 1024;
    STATSTG stat{};
    check_hresult(source->Stat(&stat, STATFLAG_NONAME));
    if (!stat.cbSize.QuadPart || stat.cbSize.QuadPart > maxSize)
        throw hresult_error(E_INVALIDARG);
    LARGE_INTEGER start{};
    check_hresult(source->Seek(start, STREAM_SEEK_SET, nullptr));
    std::vector<BYTE> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
    ULONG read = 0;
    check_hresult(source->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read));
    if (read != bytes.size()) throw hresult_error(STG_E_READFAULT);
    return bytes;
}

static std::wstring CacheKey(std::vector<BYTE>& bytes) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
        throw hresult_error(E_FAIL);
    BYTE digest[32]{};
    const auto status = BCryptHash(algorithm, nullptr, 0, bytes.data(),
        static_cast<ULONG>(bytes.size()), digest, sizeof(digest));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status != 0) throw hresult_error(E_FAIL);
    std::wstring result;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const auto value : digest) {
        result += hex[value >> 4];
        result += hex[value & 15];
    }
    return result;
}

static void Convert(const std::wstring& exe, const Scratch& scratch) {
    std::wstring command = L"\"" + exe + L"\" convert \"" + scratch.input
        + L"\" \"" + scratch.pdf + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    check_bool(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process));
    handle child{process.hProcess};
    handle thread{process.hThread};
    if (WaitForSingleObject(child.get(), 7000) != WAIT_OBJECT_0) {
        TerminateProcess(child.get(), ERROR_TIMEOUT);
        WaitForSingleObject(child.get(), 1000);
        throw hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    DWORD code = 0;
    check_bool(GetExitCodeProcess(child.get(), &code));
    if (code != 0) throw hresult_error(E_FAIL);
}

static com_ptr<IStream> OpenPdf(const std::wstring& path) {
    com_ptr<IStream> stream;
    check_hresult(SHCreateStreamOnFileEx(path.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE,
        FILE_ATTRIBUTE_NORMAL, FALSE, nullptr, stream.put()));
    return stream;
}

static com_ptr<IStream> GetPdf(IStream* source, bool spreadsheet) {
    auto bytes = ReadDocument(source);
    const auto exe = KnownFolder(FOLDERID_ProgramFiles) + (spreadsheet
        ? LR"(\ArtCraft\GridCraft\gridcraft-cli.exe)" : LR"(\ArtCraft\WordCraft\wordcraft-cli.exe)");
    WIN32_FILE_ATTRIBUTE_DATA version{};
    check_bool(GetFileAttributesExW(exe.c_str(), GetFileExInfoStandard, &version));
    const auto root = KnownFolder(FOLDERID_LocalAppDataLow) + L"\\OfficeExplorerPreview-v1";
    if (!CreateDirectoryW(root.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        throw_last_error();

    const auto cache = root + (spreadsheet ? L"\\grid-" : L"\\word-")
        + std::to_wstring(version.ftLastWriteTime.dwHighDateTime) + L"-"
        + std::to_wstring(version.ftLastWriteTime.dwLowDateTime) + L"-" + CacheKey(bytes) + L".pdf";
    try { return OpenPdf(cache); }
    catch (const hresult_error&) {
        if (GetFileAttributesW(cache.c_str()) != INVALID_FILE_ATTRIBUTES) throw;
    }

    GUID guid{};
    check_hresult(CoCreateGuid(&guid));
    wchar_t id[40]{};
    StringFromGUID2(guid, id, ARRAYSIZE(id));
    const auto prefix = root + L"\\" + id;
    Scratch scratch{prefix + (spreadsheet ? L".xlsx" : L".docx"), prefix + L".pdf"};
    {
        const HANDLE raw = CreateFileW(scratch.input.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        check_bool(raw != INVALID_HANDLE_VALUE);
        handle file{raw};
        DWORD written = 0;
        check_bool(WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr));
        if (written != bytes.size()) throw hresult_error(STG_E_WRITEFAULT);
    }
    Convert(exe, scratch);
    {
        auto pdf = OpenPdf(scratch.pdf);
        STATSTG stat{};
        check_hresult(pdf->Stat(&stat, STATFLAG_NONAME));
        char header[5]{};
        ULONG read = 0;
        check_hresult(pdf->Read(header, sizeof(header), &read));
        if (read != sizeof(header) || memcmp(header, "%PDF-", sizeof(header)) != 0
            || stat.cbSize.QuadPart > 64 * 1024 * 1024)
            throw hresult_error(E_FAIL);
    }
    if (!MoveFileExW(scratch.pdf.c_str(), cache.c_str(), 0)) {
        const auto error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS && error != ERROR_FILE_EXISTS)
            throw hresult_error(HRESULT_FROM_WIN32(error));
    }
    return OpenPdf(cache);
}

struct Provider : implements<Provider, IThumbnailProvider, IInitializeWithStream,
    IPreviewHandler, IPreviewHandlerVisuals, IObjectWithSite, IOleWindow, non_agile> {
    explicit Provider(bool grid) : spreadsheet(grid) { InterlockedIncrement(&objects); }
    ~Provider() { Unload(); InterlockedDecrement(&objects); }

    HRESULT __stdcall Initialize(IStream* input, DWORD) noexcept override {
        if (!input) return E_POINTER;
        if (source) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
        source.copy_from(input);
        return S_OK;
    }

    HRESULT __stdcall GetThumbnail(UINT size, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) noexcept override {
        if (!bitmap || !alpha) return E_POINTER;
        *bitmap = nullptr;
        *alpha = WTSAT_RGB;
        if (!source || !size) return E_INVALIDARG;
        try {
            auto document = GetPdf(source.get(), spreadsheet);
            return RenderOfficePdfPage(document.get(), (std::min)(size, 1024u), 0, bitmap, nullptr);
        } catch (...) { return to_hresult(); }
    }

    HRESULT __stdcall SetWindow(HWND window, const RECT* rect) noexcept override {
        if (!rect) return E_POINTER;
        parent = window;
        if (child && GetParent(child) != window) SetParent(child, window);
        return SetRect(rect);
    }

    HRESULT __stdcall SetRect(const RECT* rect) noexcept override {
        if (!rect) return E_POINTER;
        area = *rect;
        if (child) MoveWindow(child, area.left, area.top,
            (std::max)(0L, area.right - area.left), (std::max)(0L, area.bottom - area.top), TRUE);
        return S_OK;
    }

    HRESULT __stdcall DoPreview() noexcept override {
        if (!source || !parent || child) return E_UNEXPECTED;
        try {
            pdf = GetPdf(source.get(), spreadsheet);
            check_hresult(LoadPage(0));
            HMODULE module = nullptr;
            check_bool(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(&WindowProc), &module));
            WNDCLASSW windowClass{};
            windowClass.lpfnWndProc = WindowProc;
            windowClass.hInstance = module;
            windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            windowClass.lpszClassName = L"VitalSkib.OfficeDocumentPreview.1";
            if (!RegisterClassW(&windowClass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                throw_last_error();
            check_bool(CreateWindowExW(0, windowClass.lpszClassName, L"Office document preview",
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_TABSTOP,
                area.left, area.top, (std::max)(0L, area.right - area.left),
                (std::max)(0L, area.bottom - area.top), parent, nullptr, module, this) != nullptr);
            return S_OK;
        } catch (...) {
            const HRESULT result = to_hresult();
            Unload();
            return result;
        }
    }

    HRESULT __stdcall Unload() noexcept override {
        if (child) DestroyWindow(child);
        if (pageBitmap) DeleteObject(std::exchange(pageBitmap, nullptr));
        pdf = nullptr;
        source = nullptr;
        pageIndex = 0;
        pageCount = 0;
        return S_OK;
    }
    HRESULT __stdcall SetFocus() noexcept override {
        if (!child) return S_FALSE;
        ::SetFocus(child);
        return S_OK;
    }
    HRESULT __stdcall QueryFocus(HWND* window) noexcept override {
        if (!window) return E_POINTER;
        *window = ::GetFocus();
        return *window ? S_OK : S_FALSE;
    }
    HRESULT __stdcall TranslateAccelerator(MSG* message) noexcept override {
        if (!message) return E_POINTER;
        if (message->message == WM_KEYDOWN && HandleKey(message->wParam)) return S_OK;
        if (auto frame = site.try_as<IPreviewHandlerFrame>()) return frame->TranslateAccelerator(message);
        return S_FALSE;
    }
    HRESULT __stdcall SetSite(::IUnknown* value) noexcept override {
        site.copy_from(value);
        return S_OK;
    }
    HRESULT __stdcall GetSite(REFIID iid, void** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        return site ? site->QueryInterface(iid, value) : E_FAIL;
    }
    HRESULT __stdcall GetWindow(HWND* window) noexcept override {
        if (!window) return E_POINTER;
        *window = child;
        return child ? S_OK : E_FAIL;
    }
    HRESULT __stdcall ContextSensitiveHelp(BOOL) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall SetBackgroundColor(COLORREF color) noexcept override {
        background = color;
        if (child) InvalidateRect(child, nullptr, TRUE);
        return S_OK;
    }
    HRESULT __stdcall SetTextColor(COLORREF color) noexcept override {
        foreground = color;
        if (child) InvalidateRect(child, nullptr, TRUE);
        return S_OK;
    }
    HRESULT __stdcall SetFont(const LOGFONTW*) noexcept override { return S_OK; }

private:
    HRESULT LoadPage(UINT index) noexcept {
        LARGE_INTEGER start{};
        HRESULT result = pdf->Seek(start, STREAM_SEEK_SET, nullptr);
        if (FAILED(result)) return result;
        HBITMAP bitmap = nullptr;
        UINT count = 0;
        result = RenderOfficePdfPage(pdf.get(), 1440, index, &bitmap, &count);
        if (FAILED(result)) return result;
        if (pageBitmap) DeleteObject(pageBitmap);
        pageBitmap = bitmap;
        pageIndex = index;
        pageCount = count;
        return S_OK;
    }

    void UpdateToolbar() noexcept {
        wchar_t label[48]{};
        swprintf_s(label, L"Page %u / %u", pageIndex + 1, pageCount);
        SetWindowTextW(pageLabel, label);
        EnableWindow(previous, pageIndex > 0);
        EnableWindow(next, pageIndex + 1 < pageCount);
    }

    void GoToPage(UINT index) noexcept {
        if (index >= pageCount || index == pageIndex) return;
        if (SUCCEEDED(LoadPage(index))) {
            UpdateToolbar();
            InvalidateRect(child, nullptr, FALSE);
        }
    }

    bool HandleKey(WPARAM key) noexcept {
        if (key == VK_NEXT || key == VK_DOWN || key == VK_RIGHT) GoToPage(pageIndex + 1);
        else if (key == VK_PRIOR || key == VK_UP || key == VK_LEFT) {
            if (pageIndex) GoToPage(pageIndex - 1);
        } else return false;
        return true;
    }

    void Layout(int width, int height) noexcept {
        toolbarHeight = MulDiv(30, GetDpiForWindow(child), 96);
        const int button = MulDiv(30, GetDpiForWindow(child), 96);
        const int label = MulDiv(92, GetDpiForWindow(child), 96);
        const int x = (std::max)(0, (width - button * 2 - label) / 2);
        const int y = (std::max)(0, height - toolbarHeight);
        MoveWindow(previous, x, y, button, toolbarHeight, TRUE);
        MoveWindow(pageLabel, x + button, y, label, toolbarHeight, TRUE);
        MoveWindow(next, x + button + label, y, button, toolbarHeight, TRUE);
    }

    COLORREF Background() const noexcept {
        return background == CLR_INVALID ? GetSysColor(COLOR_WINDOW) : background;
    }

    void Paint() noexcept {
        PAINTSTRUCT paint{};
        HDC target = BeginPaint(child, &paint);
        RECT panel{};
        GetClientRect(child, &panel);
        const HBRUSH brush = CreateSolidBrush(Background());
        if (brush) {
            FillRect(target, &panel, brush);
            DeleteObject(brush);
        }
        BITMAP info{};
        const int width = panel.right - panel.left;
        const int height = panel.bottom - panel.top - toolbarHeight;
        if (pageBitmap && width > 0 && height > 0 && GetObjectW(pageBitmap, sizeof(info), &info)) {
            HDC sourceDC = CreateCompatibleDC(target);
            if (sourceDC) {
                const auto previousBitmap = SelectObject(sourceDC, pageBitmap);
                const double scale = (std::min)(double(width) / info.bmWidth, double(height) / info.bmHeight);
                const int w = (std::max)(1, int(info.bmWidth * scale));
                const int h = (std::max)(1, int(info.bmHeight * scale));
                SetStretchBltMode(target, HALFTONE);
                StretchBlt(target, (width - w) / 2, (height - h) / 2, w, h,
                    sourceDC, 0, 0, info.bmWidth, info.bmHeight, SRCCOPY);
                SelectObject(sourceDC, previousBitmap);
                DeleteDC(sourceDC);
            }
        }
        EndPaint(child, &paint);
    }

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) noexcept {
        auto self = reinterpret_cast<Provider*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<Provider*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->child = window;
        }
        if (!self) return DefWindowProcW(window, message, w, l);
        switch (message) {
        case WM_CREATE:
            self->previous = CreateWindowExW(0, L"BUTTON", L"<", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(1)), nullptr, nullptr);
            self->pageLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER | SS_CENTERIMAGE,
                0, 0, 0, 0, window, nullptr, nullptr, nullptr);
            self->next = CreateWindowExW(0, L"BUTTON", L">", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(INT_PTR(2)), nullptr, nullptr);
            if (!self->previous || !self->pageLabel || !self->next) return -1;
            for (HWND ctrl : {self->previous, self->pageLabel, self->next})
                SendMessageW(ctrl, WM_SETFONT, reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)), FALSE);
            self->UpdateToolbar();
            return 0;
        case WM_SIZE:
            self->Layout(LOWORD(l), HIWORD(l));
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        case WM_COMMAND:
            if (LOWORD(w) == 2) self->GoToPage(self->pageIndex + 1);
            if (LOWORD(w) == 1 && self->pageIndex) self->GoToPage(self->pageIndex - 1);
            return 0;
        case WM_KEYDOWN:
            if (self->HandleKey(w)) return 0;
            break;
        case WM_MOUSEWHEEL:
            self->HandleKey(GET_WHEEL_DELTA_WPARAM(w) < 0 ? VK_NEXT : VK_PRIOR);
            return 0;
        case WM_LBUTTONDOWN:
            ::SetFocus(window);
            return 0;
        case WM_PAINT:
            self->Paint();
            return 0;
        case WM_CTLCOLORSTATIC:
            SetBkMode(reinterpret_cast<HDC>(w), TRANSPARENT);
            ::SetTextColor(reinterpret_cast<HDC>(w), self->foreground == CLR_INVALID
                ? GetSysColor(COLOR_WINDOWTEXT) : self->foreground);
            return reinterpret_cast<LRESULT>(GetStockObject(NULL_BRUSH));
        case WM_ERASEBKGND:
            return 1;
        case WM_NCDESTROY:
            SetWindowLongPtrW(window, GWLP_USERDATA, 0);
            self->child = nullptr;
            break;
        }
        return DefWindowProcW(window, message, w, l);
    }

    bool spreadsheet;
    com_ptr<IStream> source, pdf;
    com_ptr<::IUnknown> site;
    HWND parent = nullptr, child = nullptr, previous = nullptr, pageLabel = nullptr, next = nullptr;
    RECT area{};
    HBITMAP pageBitmap = nullptr;
    UINT pageIndex = 0, pageCount = 0;
    int toolbarHeight = 30;
    COLORREF background = CLR_INVALID, foreground = CLR_INVALID;
};

struct Factory : implements<Factory, IClassFactory, non_agile> {
    explicit Factory(bool grid) : spreadsheet(grid) { InterlockedIncrement(&objects); }
    ~Factory() { InterlockedDecrement(&objects); }
    HRESULT __stdcall CreateInstance(::IUnknown* outer, REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try { return make_self<Provider>(spreadsheet)->QueryInterface(iid, output); }
        catch (...) { return to_hresult(); }
    }
    HRESULT __stdcall LockServer(BOOL lock) noexcept override {
        if (lock) InterlockedIncrement(&objects); else InterlockedDecrement(&objects);
        return S_OK;
    }
private:
    bool spreadsheet;
};

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (clsid != WordId && clsid != GridId) return CLASS_E_CLASSNOTAVAILABLE;
    try { return make_self<Factory>(clsid == GridId)->QueryInterface(iid, output); }
    catch (...) { return to_hresult(); }
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return InterlockedCompareExchange(&objects, 0, 0) ? S_FALSE : S_OK;
}
