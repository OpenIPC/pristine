#pragma once

#include <stdlib.h>

#include <initguid.h>
#include <codecapi.h>

#include <streams.h>
#include <dvdmedia.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mftransform.h>

#include <d3d11.h>
#include <dxgi1_2.h>

#include <evr.h>


template<class T> static inline void SafeRelease(T **pp)
{
	if (*pp) { (*pp)->Release(); *pp = nullptr; }
}

static inline UINT GetAligned(UINT dimension, UINT alignment)
{
	return (dimension + alignment - 1) & ~(alignment - 1);
}


// ---------------------------------------------------------------------------
// Custom output pin that exposes IMFGetService so the EVR can retrieve the
// DXGI Device Manager and share the D3D11 device for zero-copy rendering.
// ---------------------------------------------------------------------------
class CPristineVideoDecoderOutputPin : public CTransformOutputPin, public IMFGetService
{
public:
	CPristineVideoDecoderOutputPin(
		TCHAR *pObjectName,
		CTransformFilter *pFilter,
		HRESULT *phr,
		LPCWSTR pName);

	DECLARE_IUNKNOWN;
	STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void **ppv);

	// IMFGetService
	STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID *ppvObject);
};


// ---------------------------------------------------------------------------
// Base decoder filter: manages D3D11 device, DXGI device manager, MFT
// lifecycle, output delivery and format negotiation. Codec-specific
// subclasses override ActivateDecoderMFT() and OnCheckInputType().
// ---------------------------------------------------------------------------
class CPristineVideoDecoderFilter : public CTransformFilter, public IMFGetService
{
public:
	CPristineVideoDecoderFilter(TCHAR *pName, LPUNKNOWN pUnk, REFCLSID clsid);
	virtual ~CPristineVideoDecoderFilter();

	DECLARE_IUNKNOWN;
	STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void **ppv);

	// CTransformFilter overrides
	HRESULT Transform(IMediaSample *pIn, IMediaSample *pOut);
	HRESULT CheckTransform(const CMediaType *mtIn, const CMediaType *mtOut);
	HRESULT DecideBufferSize(IMemAllocator *pAlloc,
		ALLOCATOR_PROPERTIES *pProperties);
	HRESULT GetMediaType(int iPosition, CMediaType *pMediaType);
	CBasePin *GetPin(int n);
	int GetPinCount();

	// IMFGetService
	STDMETHODIMP GetService(REFGUID guidService, REFIID riid, LPVOID *ppvObject);

	// Accessors
	IMFDXGIDeviceManager* GetDXGIDeviceManager() const { return m_pDXGIDeviceManager; }

protected:
	// Codec-specific subclasses must implement these
	virtual HRESULT ActivateDecoderMFT() = 0;
	virtual BOOL IsInputSubtypeSupported(const GUID &subtype) const = 0;

	// Shared infrastructure
	HRESULT InitializeD3D11();
	HRESULT ConfigureMFTForD3D11();
	HRESULT DrainOutput(IMediaSample *pIn);
	HRESULT DeliverOneOutput(IMediaSample *pIn);

	// D3D11 device and DXGI manager
	ID3D11Device*           m_pD3D11Device;
	ID3D11DeviceContext*    m_pD3D11DeviceContext;
	IMFDXGIDeviceManager*   m_pDXGIDeviceManager;
	UINT                    m_resetToken;

	// MFT decoder
	IMFTransform*           m_pDecoderTransform;
	BOOL                    m_bIsHardwareMFT;

	// Video parameters
	UINT32                  m_width;
	UINT32                  m_height;
	REFERENCE_TIME          m_avgtime;
	BOOL                    m_bMFTInitialized;
	GUID                    m_outputSubtype;

	// Custom output pin
	CPristineVideoDecoderOutputPin*     m_pOutputPin;
};
