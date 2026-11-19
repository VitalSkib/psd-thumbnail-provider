#include <windows.h>
#include <shobjidl.h>
#include <thumbcache.h>
#include <shcore.h>
#include <roapi.h>
#include <utility>
#include <wincodec.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Storage.Streams.h>
#include <algorithm>
#include <cmath>
#include <new>

using namespace winrt;
using namespace winrt::Windows::Data::Pdf;
using namespace winrt::Windows::Storage::Streams;

static constexpr GUID ProviderId =
    {0xcd1aa892, 0x67a1, 0x48a9, {0xa7, 0x04, 0x50, 0xf4, 0x3b, 0xf4, 0xd5, 0x69}};
static long objects = 0;

struct RenderRequest {
    IStream* marshaled;
    UINT limit;
    HBITMAP bitmap = nullptr;
    HRESULT result = E_FAIL;
};

static HBITMAP RenderPage(IStream* source, UINT limit) {
    IRandomAccessStream input{nullptr};
    check_hresult(CreateRandomAccessStreamOverStream(source, BSOS_DEFAULT,
        guid_of<IRandomAccessStream>(), put_abi(input)));
    auto document = PdfDocument::LoadFromStreamAsync(input).get();
    if (!document.PageCount()) throw hresult_error(E_FAIL);
    auto page = document.GetPage(0);
    const auto size = page.Size();
    if (!(size.Width > 0 && size.Height > 0)
        || !std::isfinite(size.Width) || !std::isfinite(size.Height))
        throw hresult_error(E_FAIL);
    const double scale = double(limit) / (std::max)(size.Width, size.Height);
    PdfPageRenderOptions options;
    options.DestinationWidth((std::max)(1u, UINT(size.Width * scale + 0.5)));
    options.DestinationHeight((std::max)(1u, UINT(size.Height * scale + 0.5)));
    InMemoryRandomAccessStream rendered;
    page.RenderToStreamAsync(rendered, options).get();
    rendered.Seek(0);
    com_ptr<IStream> stream;
    check_hresult(CreateStreamOverRandomAccessStream(
        reinterpret_cast<::IUnknown*>(get_abi(rendered)), __uuidof(IStream), stream.put_void()));
    auto factory = create_instance<IWICImagingFactory>(CLSID_WICImagingFactory, CLSCTX_INPROC_SERVER);
    com_ptr<IWICBitmapDecoder> decoder;
    check_hresult(factory->CreateDecoderFromStream(stream.get(), nullptr, WICDecodeMetadataCacheOnLoad, decoder.put()));
    com_ptr<IWICBitmapFrameDecode> frame;
    check_hresult(decoder->GetFrame(0, frame.put()));
    UINT width = 0, height = 0;
    check_hresult(frame->GetSize(&width, &height));
    if (!width || !height || width > limit || height > limit) throw hresult_error(E_FAIL);
    com_ptr<IWICFormatConverter> converter;
    check_hresult(factory->CreateFormatConverter(converter.put()));
    check_hresult(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGR,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom));
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -LONG(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    HBITMAP bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap) throw hresult_error(E_OUTOFMEMORY);
    const HRESULT copied = converter->CopyPixels(nullptr, width * 4, width * height * 4, static_cast<BYTE*>(pixels));
    if (FAILED(copied)) {
        DeleteObject(bitmap);
        check_hresult(copied);
    }
    return bitmap;
}

static DWORD WINAPI RenderOnWorker(void* context) noexcept {
    auto& request = *static_cast<RenderRequest*>(context);
    const HRESULT initialized = RoInitialize(RO_INIT_MULTITHREADED);
    if (FAILED(initialized)) {
        request.result = initialized;
        return 0;
    }
    try {
        com_ptr<IStream> source;
        IStream* transport = std::exchange(request.marshaled, nullptr);
        check_hresult(CoGetInterfaceAndReleaseStream(transport, __uuidof(IStream), source.put_void()));
        request.bitmap = RenderPage(source.get(), request.limit);
        request.result = S_OK;
    } catch (...) {
        request.result = to_hresult();
    }
    RoUninitialize();
    return 0;
}

struct Provider final : implements<Provider, IThumbnailProvider, IInitializeWithStream, non_agile> {
    com_ptr<IStream> source;
    Provider() { InterlockedIncrement(&objects); }
    ~Provider() { InterlockedDecrement(&objects); }

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
        RenderRequest request{nullptr, (std::min)(size, 1024u)};
        HRESULT hr = CoMarshalInterThreadInterfaceInStream(__uuidof(IStream), source.get(), &request.marshaled);
        if (FAILED(hr)) return hr;
        handle worker{CreateThread(nullptr, 0, RenderOnWorker, &request, 0, nullptr)};
        if (worker) {
            HANDLE wait = worker.get();
            DWORD index = 0;
            hr = CoWaitForMultipleHandles(COWAIT_DISPATCH_CALLS, INFINITE, 1, &wait, &index);
            if (FAILED(hr)) WaitForSingleObject(wait, INFINITE);
        } else {
            request.result = HRESULT_FROM_WIN32(GetLastError());
        }
        if (request.marshaled) {
            LARGE_INTEGER origin{};
            request.marshaled->Seek(origin, STREAM_SEEK_SET, nullptr);
            CoReleaseMarshalData(request.marshaled);
            request.marshaled->Release();
        }
        *bitmap = request.bitmap;
        return request.result;
    }
};

struct Factory final : implements<Factory, IClassFactory, non_agile> {
    Factory() { InterlockedIncrement(&objects); }
    ~Factory() { InterlockedDecrement(&objects); }

    HRESULT __stdcall CreateInstance(::IUnknown* outer, REFIID iid, void** output) noexcept override {
        if (!output) return E_POINTER;
        *output = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        try { return make_self<Provider>()->QueryInterface(iid, output); }
        catch (...) { return to_hresult(); }
    }
    HRESULT __stdcall LockServer(BOOL lock) noexcept override {
        if (lock) InterlockedIncrement(&objects); else InterlockedDecrement(&objects);
        return S_OK;
    }
};

extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** output) {
    if (!output) return E_POINTER;
    *output = nullptr;
    if (clsid != ProviderId) return CLASS_E_CLASSNOTAVAILABLE;
    try { return make_self<Factory>()->QueryInterface(iid, output); }
    catch (...) { return to_hresult(); }
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return InterlockedCompareExchange(&objects, 0, 0) ? S_FALSE : S_OK;
}
