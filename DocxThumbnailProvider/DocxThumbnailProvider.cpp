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
#include <string>
#include <vector>

using namespace winrt;
static long objects = 0;
static constexpr GUID WordId =
    {0x06cfb719, 0x8050, 0x4cf6, {0xa5, 0x98, 0x17, 0x48, 0x47, 0x1b, 0x08, 0x49}};
static constexpr GUID GridId =
    {0x43f420f9, 0x8d44, 0x4be3, {0xae, 0xbb, 0xbd, 0x08, 0xa7, 0xdf, 0x4c, 0xd1}};
static constexpr GUID PdfPreviewId =
    {0x3a84f9c2, 0x6164, 0x485c, {0xa7, 0xd9, 0x4b, 0x27, 0xf8, 0xac, 0x00, 0x9e}};

HRESULT RenderOfficePdfThumbnail(IStream* source, UINT size, HBITMAP* bitmap);

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
            auto pdf = GetPdf(source.get(), spreadsheet);
            return RenderOfficePdfThumbnail(pdf.get(), (std::min)(size, 1024u), bitmap);
        } catch (...) { return to_hresult(); }
    }

    HRESULT __stdcall SetWindow(HWND window, const RECT* rect) noexcept override {
        if (!rect) return E_POINTER;
        parent = window;
        area = *rect;
        return preview ? preview->SetWindow(window, rect) : S_OK;
    }
    HRESULT __stdcall SetRect(const RECT* rect) noexcept override {
        if (!rect) return E_POINTER;
        area = *rect;
        return preview ? preview->SetRect(rect) : S_OK;
    }
    HRESULT __stdcall DoPreview() noexcept override {
        if (!source || !parent || preview) return E_UNEXPECTED;
        try {
            auto pdf = GetPdf(source.get(), spreadsheet);
            auto handler = create_instance<IPreviewHandler>(PdfPreviewId, CLSCTX_INPROC_SERVER);
            auto objectSite = handler.try_as<IObjectWithSite>();
            if (objectSite && site) check_hresult(objectSite->SetSite(site.get()));
            check_hresult(handler.as<IInitializeWithStream>()->Initialize(pdf.get(), STGM_READ));
            if (auto visuals = handler.try_as<IPreviewHandlerVisuals>()) {
                if (background != CLR_INVALID) visuals->SetBackgroundColor(background);
                if (foreground != CLR_INVALID) visuals->SetTextColor(foreground);
                if (hasFont) visuals->SetFont(&font);
            }
            check_hresult(handler->SetWindow(parent, &area));
            preview = std::move(handler);
            const HRESULT result = preview->DoPreview();
            if (FAILED(result)) Unload();
            return result;
        } catch (...) { return to_hresult(); }
    }
    HRESULT __stdcall Unload() noexcept override {
        const HRESULT result = preview ? preview->Unload() : S_OK;
        preview = nullptr;
        source = nullptr;
        return result;
    }
    HRESULT __stdcall SetFocus() noexcept override { return preview ? preview->SetFocus() : S_FALSE; }
    HRESULT __stdcall QueryFocus(HWND* window) noexcept override {
        if (!window) return E_POINTER;
        *window = nullptr;
        return preview ? preview->QueryFocus(window) : S_FALSE;
    }
    HRESULT __stdcall TranslateAccelerator(MSG* message) noexcept override {
        return preview ? preview->TranslateAccelerator(message) : S_FALSE;
    }
    HRESULT __stdcall SetSite(::IUnknown* value) noexcept override {
        site.copy_from(value);
        if (auto objectSite = preview.try_as<IObjectWithSite>()) return objectSite->SetSite(value);
        return S_OK;
    }
    HRESULT __stdcall GetSite(REFIID iid, void** value) noexcept override {
        if (!value) return E_POINTER;
        *value = nullptr;
        return site ? site->QueryInterface(iid, value) : E_FAIL;
    }
    HRESULT __stdcall GetWindow(HWND* window) noexcept override {
        if (!window) return E_POINTER;
        *window = parent;
        if (auto ole = preview.try_as<IOleWindow>()) return ole->GetWindow(window);
        return parent ? S_OK : E_FAIL;
    }
    HRESULT __stdcall ContextSensitiveHelp(BOOL) noexcept override { return E_NOTIMPL; }
    HRESULT __stdcall SetBackgroundColor(COLORREF color) noexcept override {
        background = color;
        if (auto visuals = preview.try_as<IPreviewHandlerVisuals>()) return visuals->SetBackgroundColor(color);
        return S_OK;
    }
    HRESULT __stdcall SetTextColor(COLORREF color) noexcept override {
        foreground = color;
        if (auto visuals = preview.try_as<IPreviewHandlerVisuals>()) return visuals->SetTextColor(color);
        return S_OK;
    }
    HRESULT __stdcall SetFont(const LOGFONTW* value) noexcept override {
        if (!value) return E_POINTER;
        font = *value;
        hasFont = true;
        if (auto visuals = preview.try_as<IPreviewHandlerVisuals>()) return visuals->SetFont(value);
        return S_OK;
    }

private:
    bool spreadsheet;
    com_ptr<IStream> source;
    com_ptr<IPreviewHandler> preview;
    com_ptr<::IUnknown> site;
    HWND parent = nullptr;
    RECT area{};
    COLORREF background = CLR_INVALID, foreground = CLR_INVALID;
    LOGFONTW font{};
    bool hasFont = false;
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

