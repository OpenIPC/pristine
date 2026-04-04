#pragma once

#define _CRT_SECURE_NO_WARNINGS

#include <stdlib.h>

#include <initguid.h>

#include <streams.h>
#include <shellapi.h>

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mftransform.h>

#include <mmreg.h>


#ifndef SAFERELEASE_DEFINED
#define SAFERELEASE_DEFINED
template<class T> static inline void SafeRelease(T **pp)
{
	if (*pp) { (*pp)->Release(); *pp = nullptr; }
}
#endif


// ---------------------------------------------------------------------------
// Base audio decoder filter: manages MFT lifecycle, output delivery and
// format negotiation for audio codecs. Codec-specific subclasses override
// ActivateDecoderMFT(), IsInputSubtypeSupported() and CheckInputType().
// ---------------------------------------------------------------------------
class CPristineAudioDecoderFilter : public CTransformFilter
{
public:
	CPristineAudioDecoderFilter(TCHAR *pName, LPUNKNOWN pUnk, REFCLSID clsid);
	virtual ~CPristineAudioDecoderFilter();

	DECLARE_IUNKNOWN;

	// CTransformFilter overrides
	HRESULT Transform(IMediaSample *pIn, IMediaSample *pOut);
	HRESULT CheckTransform(const CMediaType *mtIn, const CMediaType *mtOut);
	HRESULT DecideBufferSize(IMemAllocator *pAlloc,
		ALLOCATOR_PROPERTIES *pProperties);
	HRESULT GetMediaType(int iPosition, CMediaType *pMediaType);

protected:
	// Codec-specific subclasses must implement these
	virtual HRESULT ActivateDecoderMFT() = 0;
	virtual BOOL IsInputSubtypeSupported(const GUID &subtype) const = 0;

	// Shared infrastructure
	HRESULT DrainOutput(IMediaSample *pIn);
	HRESULT DeliverOneOutput(IMediaSample *pIn);

	// MFT decoder
	IMFTransform*           m_pDecoderTransform;

	// Audio parameters (populated by subclass CheckInputType)
	UINT32                  m_nChannels;
	UINT32                  m_nSamplesPerSec;
	UINT32                  m_nBitsPerSample;
	BOOL                    m_bMFTInitialized;

	// Running output timestamp (computed from delivered sample count)
	REFERENCE_TIME          m_rtNextOutputTime;
};
