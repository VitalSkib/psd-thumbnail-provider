#pragma once
#include <windows.h>
#include <shobjidl.h>

class PreviewProvider final : public IPreviewHandler, public IInitializeWithStream, public IPreviewHandlerVisuals {
public:
    PreviewProvider();
    ~PreviewProvider();
    IFACEMETHODIMP QueryInterface(REFIID iid, void** result) override;
    IFACEMETHODIMP_(ULONG) AddRef() override;
    IFACEMETHODIMP_(ULONG) Release() override;
    IFACEMETHODIMP Initialize(IStream* stream, DWORD mode) override;
    IFACEMETHODIMP SetWindow(HWND parent, const RECT* rect) override;
    IFACEMETHODIMP SetRect(const RECT* rect) override;
    IFACEMETHODIMP DoPreview() override;
    IFACEMETHODIMP Unload() override;
    IFACEMETHODIMP SetFocus() override;
    IFACEMETHODIMP QueryFocus(HWND* focus) override;
    IFACEMETHODIMP TranslateAccelerator(MSG*) override { return S_FALSE; }
    IFACEMETHODIMP SetBackgroundColor(COLORREF color) override;
    IFACEMETHODIMP SetFont(const LOGFONTW*) override { return S_OK; }
    IFACEMETHODIMP SetTextColor(COLORREF) override { return S_OK; }
private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    long references = 1;
    IStream* stream = nullptr;
    HWND parent = nullptr;
    HWND window = nullptr;
    RECT rect = {};
    HBITMAP bitmap = nullptr;
    COLORREF background = CLR_INVALID;
};
