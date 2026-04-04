#pragma once

#include "audiodecoder.h"


DEFINE_GUID(CLSID_PristineAacDecoder,
	0xA4B7C8D9, 0xE0F1, 0x4A2B, 0x93, 0xD4, 0x5E, 0x6F, 0x7A, 0x8B, 0x9C, 0x0D);

const AMOVIESETUP_PIN aacDecoderPins[] =
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

const AMOVIESETUP_FILTER aacDecoderFilt =
{
	&CLSID_PristineAacDecoder,
	L"Pristine: AAC Decoder",
	MERIT_DO_NOT_USE,
	2,
	aacDecoderPins
};


class CPristineAacDecoderFilter : public CPristineAudioDecoderFilter
{
public:
	CPristineAacDecoderFilter(LPUNKNOWN pUnk, HRESULT* phr);
	~CPristineAacDecoderFilter();

	static CUnknown* WINAPI CreateInstance(LPUNKNOWN, HRESULT *);

	// CTransformFilter overrides
	HRESULT Receive(IMediaSample *pSample);
	HRESULT CheckInputType(const CMediaType *mtIn);

protected:
	// CPristineAudioDecoderFilter overrides
	HRESULT ActivateDecoderMFT();
	BOOL IsInputSubtypeSupported(const GUID &subtype) const;
};
