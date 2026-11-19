#include <windows.h>
#include <objidl.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

namespace {
constexpr int BitmapBgrx = 3;

struct FileAccess {
    unsigned long length;
    int (*read)(void*, unsigned long, unsigned char*, unsigned long);
    void* context;
};

int ReadBlock(void* context, unsigned long offset, unsigned char* buffer, unsigned long length) {
    auto stream = static_cast<IStream*>(context);
    LARGE_INTEGER position{};
    position.QuadPart = offset;
    if (FAILED(stream->Seek(position, STREAM_SEEK_SET, nullptr))) return 0;
    while (length) {
        ULONG read = 0;
        if (FAILED(stream->Read(buffer, length, &read)) || !read || read > length) return 0;
        buffer += read;
        length -= read;
    }
    return 1;
}

struct Pdfium {
    void (*init)() = nullptr;
    void (*shutdown)() = nullptr;
    void* (*open)(FileAccess*, const char*) = nullptr;
    void (*closeDocument)(void*) = nullptr;
    void* (*loadPage)(void*, int) = nullptr;
    void (*closePage)(void*) = nullptr;
    float (*pageWidth)(void*) = nullptr;
    float (*pageHeight)(void*) = nullptr;
    void* (*createBitmap)(int, int, int, void*, int) = nullptr;
    void (*destroyBitmap)(void*) = nullptr;
    int (*fillBitmap)(void*, int, int, int, int, unsigned long) = nullptr;
    void (*render)(void*, void*, int, int, int, int, int, int) = nullptr;

    template<class T>
    static bool Load(HMODULE module, T& fn, const char* name) {
        fn = reinterpret_cast<T>(GetProcAddress(module, name));
        return fn != nullptr;
    }

    bool Bind(HMODULE module) {
        return Load(module, init, "FPDF_InitLibrary")
            && Load(module, shutdown, "FPDF_DestroyLibrary")
            && Load(module, open, "FPDF_LoadCustomDocument")
            && Load(module, closeDocument, "FPDF_CloseDocument")
            && Load(module, loadPage, "FPDF_LoadPage")
            && Load(module, closePage, "FPDF_ClosePage")
            && Load(module, pageWidth, "FPDF_GetPageWidthF")
            && Load(module, pageHeight, "FPDF_GetPageHeightF")
            && Load(module, createBitmap, "FPDFBitmap_CreateEx")
            && Load(module, destroyBitmap, "FPDFBitmap_Destroy")
            && Load(module, fillBitmap, "FPDFBitmap_FillRect")
            && Load(module, render, "FPDF_RenderPageBitmap");
    }
};

struct Library {
    HMODULE module;
    ~Library() { if (module) FreeLibrary(module); }
};

struct Session {
    Pdfium& api;
    ~Session() { api.shutdown(); }
};

struct Dib {
    HBITMAP bitmap;
    ~Dib() { if (bitmap) DeleteObject(bitmap); }
};
}

HBITMAP RenderPdfiumThumbnail(IStream* stream, UINT limit) {
    STATSTG stat{};
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || !stat.cbSize.QuadPart
        || stat.cbSize.QuadPart > (std::numeric_limits<unsigned long>::max)()) return nullptr;

    HMODULE owner = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&RenderPdfiumThumbnail), &owner)) return nullptr;
    wchar_t path[32768];
    const DWORD length = GetModuleFileNameW(owner, path, ARRAYSIZE(path));
    if (!length || length >= ARRAYSIZE(path)) return nullptr;
    std::wstring sibling(path, length);
    const auto slash = sibling.find_last_of(L'\\');
    if (slash == std::wstring::npos) return nullptr;
    sibling.replace(slash + 1, std::wstring::npos, L"pdfium.dll");

    static std::mutex pdfiumMutex;
    const std::lock_guard<std::mutex> lock(pdfiumMutex);
    Library library{LoadLibraryExW(sibling.c_str(), nullptr,
        LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)};
    Pdfium api;
    if (!library.module || !api.Bind(library.module)) return nullptr;
    api.init();
    Session session{api};

    FileAccess access{static_cast<unsigned long>(stat.cbSize.QuadPart), ReadBlock, stream};
    std::unique_ptr<void, decltype(api.closeDocument)> document(api.open(&access, nullptr), api.closeDocument);
    if (!document) return nullptr;
    std::unique_ptr<void, decltype(api.closePage)> page(api.loadPage(document.get(), 0), api.closePage);
    if (!page) return nullptr;

    const double width = api.pageWidth(page.get());
    const double height = api.pageHeight(page.get());
    if (!(width > 0 && height > 0) || !std::isfinite(width) || !std::isfinite(height)) return nullptr;
    const double scale = limit / (std::max)(width, height);
    const int w = (std::max)(1, int(width * scale + 0.5));
    const int h = (std::max)(1, int(height * scale + 0.5));

    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    Dib dib{CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0)};
    if (!dib.bitmap) return nullptr;
    std::unique_ptr<void, decltype(api.destroyBitmap)> bitmap(
        api.createBitmap(w, h, BitmapBgrx, pixels, w * 4), api.destroyBitmap);
    if (!bitmap || !api.fillBitmap(bitmap.get(), 0, 0, w, h, 0xffffffffUL)) return nullptr;
    api.render(bitmap.get(), page.get(), 0, 0, w, h, 0, 0);
    return std::exchange(dib.bitmap, nullptr);
}
