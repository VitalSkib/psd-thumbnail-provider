#include "PreviewProvider.h"
#include "GetThumbnail.h"

#pragma comment(lib, "advapi32.lib")

extern HINSTANCE g_hInst;
extern long g_cDllRef;
static const wchar_t WindowClass[] = L"VitalSkib.PsdPreview.1";

static COLORREF DefaultBackground() {
    HIGHCONTRASTW contrast = {};
    contrast.cbSize = sizeof(contrast);
    if (SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0)
        && (contrast.dwFlags & HCF_HIGHCONTRASTON)) return GetSysColor(COLOR_WINDOW);

    DWORD light = 1;
    DWORD bytes = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
        L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &bytes) == ERROR_SUCCESS && !light) {
        return RGB(25, 25, 25);
    }
    return GetSysColor(COLOR_WINDOW);
}

PreviewProvider::PreviewProvider() { InterlockedIncrement(&g_cDllRef); }
PreviewProvider::~PreviewProvider() {
    Unload();
    InterlockedDecrement(&g_cDllRef);
}
HRESULT PreviewProvider::QueryInterface(REFIID iid, void** result) {
    if (!result) return E_POINTER;
    *result = nullptr;
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IPreviewHandler))
        *result = static_cast<IPreviewHandler*>(this);
    else if (iid == __uuidof(IInitializeWithStream))
        *result = static_cast<IInitializeWithStream*>(this);
    else if (iid == __uuidof(IPreviewHandlerVisuals))
        *result = static_cast<IPreviewHandlerVisuals*>(this);
    else return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
ULONG PreviewProvider::AddRef() { return InterlockedIncrement(&references); }
ULONG PreviewProvider::Release() {
    const ULONG remaining = InterlockedDecrement(&references);
    if (!remaining) delete this;
    return remaining;
}
HRESULT PreviewProvider::Initialize(IStream* input, DWORD) {
    if (!input) return E_POINTER;
    if (stream) return HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);
    input->AddRef();
    stream = input;
    return S_OK;
}
HRESULT PreviewProvider::SetWindow(HWND host, const RECT* bounds) {
    parent = host;
    if (window && !::SetParent(window, parent)) return E_FAIL;
    return SetRect(bounds);
}
HRESULT PreviewProvider::SetRect(const RECT* bounds) {
    if (!bounds) return E_POINTER;
    rect = *bounds;
    if (window) {
        MoveWindow(window, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, TRUE);
        InvalidateRect(window, nullptr, FALSE);
    }
    return S_OK;
}
HRESULT PreviewProvider::SetBackgroundColor(COLORREF color) {
    background = color;
    if (window) InvalidateRect(window, nullptr, FALSE);
    return S_OK;
}
HRESULT PreviewProvider::DoPreview() {
    if (!stream || !parent || window) return E_UNEXPECTED;
    try {
        bitmap = GetPSDThumbnail(stream, 2048);
    } catch (...) {
        return E_FAIL;
    }
    if (!bitmap) return E_FAIL;
    WNDCLASSW wc = {};
    wc.hInstance = g_hInst;
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = WindowClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Unload();
        return E_FAIL;
    }
    window = CreateWindowExW(0, WindowClass, L"", WS_CHILD | WS_VISIBLE,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        parent, nullptr, g_hInst, this);
    if (!window) {
        Unload();
        return E_FAIL;
    }
    return S_OK;
}
HRESULT PreviewProvider::Unload() {
    if (window) DestroyWindow(window);
    window = nullptr;
    if (bitmap) DeleteObject(bitmap);
    bitmap = nullptr;
    if (stream) stream->Release();
    stream = nullptr;
    return S_OK;
}
HRESULT PreviewProvider::SetFocus() {
    if (!window) return E_FAIL;
    ::SetFocus(window);
    return S_OK;
}
HRESULT PreviewProvider::QueryFocus(HWND* focus) {
    if (!focus) return E_POINTER;
    *focus = GetFocus();
    return S_OK;
}
LRESULT CALLBACK PreviewProvider::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        auto create = reinterpret_cast<const CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (message == WM_THEMECHANGED || message == WM_SETTINGCHANGE || message == WM_SYSCOLORCHANGE)
        InvalidateRect(window, nullptr, FALSE);
    if (message != WM_PAINT) return DefWindowProcW(window, message, wparam, lparam);
    auto self = reinterpret_cast<const PreviewProvider*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    PAINTSTRUCT paint = {};
    HDC dc = BeginPaint(window, &paint);
    RECT area = {};
    GetClientRect(window, &area);
    const COLORREF defaultColor = DefaultBackground();
    const COLORREF hostColor = self ? self->background : CLR_INVALID;
    // Some Explorer hosts suggest the legacy white window color in dark mode.
    const bool legacyDefault = hostColor == RGB(255, 255, 255)
        || hostColor == GetSysColor(COLOR_WINDOW);
    const COLORREF color = hostColor == CLR_INVALID || legacyDefault ? defaultColor : hostColor;
    SetDCBrushColor(dc, color);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    BITMAP image = {};
    if (self && self->bitmap && GetObjectW(self->bitmap, sizeof(image), &image)
        && area.right > 0 && area.bottom > 0 && image.bmWidth > 0 && image.bmHeight > 0) {
        const double scale = min(double(area.right) / image.bmWidth, double(area.bottom) / image.bmHeight);
        const int width = max(1, int(image.bmWidth * scale + 0.5));
        const int height = max(1, int(image.bmHeight * scale + 0.5));
        HDC source = CreateCompatibleDC(dc);
        if (source) {
            HGDIOBJ old = SelectObject(source, self->bitmap);
            SetStretchBltMode(dc, HALFTONE);
            SetBrushOrgEx(dc, 0, 0, nullptr);
            StretchBlt(dc, (area.right - width) / 2, (area.bottom - height) / 2,
                width, height, source, 0, 0, image.bmWidth, image.bmHeight, SRCCOPY);
            SelectObject(source, old);
            DeleteDC(source);
        }
    }
    EndPaint(window, &paint);
    return 0;
}
