#pragma once

#include "videodecoder.h"


DEFINE_GUID(CLSID_PristineVp9Decoder,
	0x6A1E2B3C, 0xD4F5, 0x4A8E, 0xB9, 0x72, 0x1F, 0x3A, 0x5C, 0x7D, 0x9E, 0x0B);

const AMOVIESETUP_PIN vp9DecoderPins[] =
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

const AMOVIESETUP_FILTER vp9DecoderFilt =
{
	&CLSID_PristineVp9Decoder,
	L"Pristine: VP9 Decoder",
	MERIT_DO_NOT_USE,
	2,
	vp9DecoderPins
};


class CPristineVp9DecoderFilter : public CPristineVideoDecoderFilter
{
public:
	CPristineVp9DecoderFilter(LPUNKNOWN pUnk, HRESULT* phr);
	~CPristineVp9DecoderFilter();

	static CUnknown* WINAPI CreateInstance(LPUNKNOWN, HRESULT *);

	// CTransformFilter overrides
	HRESULT Receive(IMediaSample *pSample);
	HRESULT CheckInputType(const CMediaType *mtIn);

protected:
	// CPristineVideoDecoderFilter overrides
	HRESULT ActivateDecoderMFT();
	BOOL IsInputSubtypeSupported(const GUID &subtype) const;
};
