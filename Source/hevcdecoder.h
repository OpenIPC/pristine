#pragma once

#include "videodecoder.h"


DEFINE_GUID(CLSID_PristineHevcDecoder,
	0x30108958, 0xF786, 0x49C1, 0x90, 0x17, 0xC2, 0x5C, 0x64, 0x31, 0x11, 0x0A);

const AMOVIESETUP_PIN hevcDecoderPins[] =
{
	{
		L"Input",		// Pin string name
		FALSE,			// Is it rendered
		FALSE,			// Is it an output
		FALSE,			// Allowed none
		FALSE,			// Likewise many
		NULL,			// Connects to filter
		NULL,			// Connects to pin
		0,				// Number of types
		NULL			// Pin information
	},
	{
		L"Output",
		FALSE,
		TRUE,
		FALSE,
		FALSE,
		NULL,
		NULL,
		0,
		NULL
	}
};

const AMOVIESETUP_FILTER hevcDecoderFilt =
{
	&CLSID_PristineHevcDecoder,
	L"Pristine: HEVC Decoder",
	MERIT_DO_NOT_USE,
	2, // Pin number
	hevcDecoderPins
};


class CPristineHevcDecoderFilter : public CPristineVideoDecoderFilter
{
public:
	CPristineHevcDecoderFilter(LPUNKNOWN pUnk, HRESULT* phr);
	~CPristineHevcDecoderFilter();

	static CUnknown* WINAPI CreateInstance(LPUNKNOWN, HRESULT *);

	// CTransformFilter overrides
	HRESULT Receive(IMediaSample *pSample);
	HRESULT CheckInputType(const CMediaType *mtIn);

protected:
	// CPristineVideoDecoderFilter overrides
	HRESULT ActivateDecoderMFT();
	BOOL IsInputSubtypeSupported(const GUID &subtype) const;

private:
	HRESULT SendParameterSets();

	UINT                    m_nalLengthSize;
	BYTE*                   m_pParamSets;
	UINT                    m_cbParamSets;
	BOOL                    m_bParamSetsSent;
	BOOL                    m_bIsHVC1;
};