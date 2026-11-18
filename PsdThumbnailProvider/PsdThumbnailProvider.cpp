#include "PsdThumbnailProvider.h"
#include "GetThumbnail.h"
#include <Shlwapi.h>

#pragma comment(lib, "Shlwapi.lib")

extern HINSTANCE g_hInst;
extern long g_cDllRef;

PsdThumbnailProvider::PsdThumbnailProvider() : m_cRef(1), m_pStream(NULL) {
	InterlockedIncrement(&g_cDllRef);
}

PsdThumbnailProvider::~PsdThumbnailProvider() {
	if (m_pStream) m_pStream->Release();
	InterlockedDecrement(&g_cDllRef);
}

// IUnknown

IFACEMETHODIMP PsdThumbnailProvider::QueryInterface(REFIID riid, void **ppv) {
	static const QITAB qit[] =
	{
		QITABENT(PsdThumbnailProvider, IThumbnailProvider),
		QITABENT(PsdThumbnailProvider, IInitializeWithStream),
		{ 0 },
	};
	return QISearch(this, qit, riid, ppv);
}

IFACEMETHODIMP_(ULONG) PsdThumbnailProvider::AddRef() {
	return InterlockedIncrement(&m_cRef);
}

IFACEMETHODIMP_(ULONG) PsdThumbnailProvider::Release() {
	ULONG cRef = InterlockedDecrement(&m_cRef);

	if (0 == cRef)
		delete this;

	return cRef;
}

// IInitializeWithStream

IFACEMETHODIMP PsdThumbnailProvider::Initialize(IStream *pStream, DWORD grfMode) {
	if (!pStream) return E_POINTER;
	HRESULT hr = HRESULT_FROM_WIN32(ERROR_ALREADY_INITIALIZED);

	if (m_pStream == NULL)
		hr = pStream->QueryInterface(&m_pStream);

	return hr;
}

// IThumbnailProvider

IFACEMETHODIMP PsdThumbnailProvider::GetThumbnail(UINT cx, HBITMAP *phbmp, WTS_ALPHATYPE *pdwAlpha) {
	if (!phbmp || !pdwAlpha) return E_POINTER;
	*phbmp = nullptr;
	*pdwAlpha = WTSAT_ARGB;
	if (!m_pStream) return E_UNEXPECTED;
	LARGE_INTEGER start = {};
	if (FAILED(m_pStream->Seek(start, STREAM_SEEK_SET, nullptr))) return E_FAIL;
	try {
		const int largeSize = cx >= 512 ? int(min(cx, 1024U)) : 0;
		*phbmp = GetPSDThumbnail(m_pStream, largeSize);
	} catch (...) {
		return E_FAIL;
	}

	return *phbmp != NULL ? NOERROR : E_NOTIMPL;
}
