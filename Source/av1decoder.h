#pragma once

#include "videodecoder.h"


DEFINE_GUID(CLSID_PristineAv1Decoder,
	0x8B2F4D5E, 0xA6C7, 0x4B19, 0x83, 0xE0, 0x4D, 0x6A, 0x8B, 0x2F, 0xC1, 0x53);

const AMOVIESETUP_PIN av1DecoderPins[] =
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

const AMOVIESETUP_FILTER av1DecoderFilt =
{
	&CLSID_PristineAv1Decoder,
	L"Pristine: AV1 Decoder",
	MERIT_DO_NOT_USE,
	2,
	av1DecoderPins
};


class CPristineAv1DecoderFilter : public CPristineVideoDecoderFilter
{
public:
	CPristineAv1DecoderFilter(LPUNKNOWN pUnk, HRESULT* phr);
	~CPristineAv1DecoderFilter();

	static CUnknown* WINAPI CreateInstance(LPUNKNOWN, HRESULT *);

	// CTransformFilter overrides
	HRESULT Receive(IMediaSample *pSample);
	HRESULT CheckInputType(const CMediaType *mtIn);

protected:
	// CPristineVideoDecoderFilter overrides
	HRESULT ActivateDecoderMFT();
	BOOL IsInputSubtypeSupported(const GUID &subtype) const;
};
