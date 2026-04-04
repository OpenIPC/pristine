#pragma once

#include "audiodecoder.h"


DEFINE_GUID(CLSID_PristineOpusDecoder,
	0xB5C8D6E7, 0xF2A3, 0x4B14, 0x85, 0xC6, 0x7D, 0x8E, 0x9F, 0x0A, 0x1B, 0x2C);

const AMOVIESETUP_PIN opusDecoderPins[] =
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

const AMOVIESETUP_FILTER opusDecoderFilt =
{
	&CLSID_PristineOpusDecoder,
	L"Pristine: Opus Decoder",
	MERIT_DO_NOT_USE,
	2,
	opusDecoderPins
};


class CPristineOpusDecoderFilter : public CPristineAudioDecoderFilter
{
public:
	CPristineOpusDecoderFilter(LPUNKNOWN pUnk, HRESULT* phr);
	~CPristineOpusDecoderFilter();

	static CUnknown* WINAPI CreateInstance(LPUNKNOWN, HRESULT *);

	// CTransformFilter overrides
	HRESULT Receive(IMediaSample *pSample);
	HRESULT CheckInputType(const CMediaType *mtIn);

protected:
	// CPristineAudioDecoderFilter overrides
	HRESULT ActivateDecoderMFT();
	BOOL IsInputSubtypeSupported(const GUID &subtype) const;
};
