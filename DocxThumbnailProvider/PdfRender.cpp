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

struct RenderRequest {
    IStream* marshaled;
    UINT limit;
    UINT index;
    UINT count = 0;
    HBITMAP bitmap = nullptr;
    HRESULT result = E_FAIL;
};

static HBITMAP RenderPage(IStream* source, UINT limit, UINT index, UINT& count) {
    IRandomAccessStream input{nullptr};
    check_hresult(CreateRandomAccessStreamOverStream(source, BSOS_DEFAULT,
        guid_of<IRandomAccessStream>(), put_abi(input)));
    auto document = PdfDocument::LoadFromStreamAsync(input).get();
    count = document.PageCount();
    if (index >= count) throw hresult_error(E_INVALIDARG);
    auto page = document.GetPage(index);
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
        request.bitmap = RenderPage(source.get(), request.limit, request.index, request.count);
        request.result = S_OK;
    } catch (...) {
        request.result = to_hresult();
    }
    RoUninitialize();
    return 0;
}

HRESULT RenderOfficePdfPage(IStream* source, UINT size, UINT page, HBITMAP* bitmap, UINT* count) {
    RenderRequest request{nullptr, (std::min)(size, 1600u), page};
    HRESULT hr = CoMarshalInterThreadInterfaceInStream(__uuidof(IStream), source, &request.marshaled);
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
    if (count) *count = request.count;
    return request.result;
}
