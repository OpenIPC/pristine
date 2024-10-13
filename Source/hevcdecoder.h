#include <streams.h>
#include <initguid.h>

#include <dvdmedia.h>
#include <mfapi.h>
#include <Mferror.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <mfplay.h>
#include <mfreadwrite.h>
#include <mftransform.h>


DEFINE_GUID(CLSID_PristineHevcDecoder,
	0x30108958, 0xf786, 0x49C1, 0x90, 0x17, 0xC2, 0x5C, 0x64, 0x31, 0x11, 0x0A);

const AMOVIESETUP_PIN hevcDecoderPins[] =
{
	{
		L"Input",		// Pin string name
		FALSE,			// Is it rendered
		FALSE,			// Is it an output
		FALSE,			// Allowed none
		FALSE,			// Likewise many
		&CLSID_NULL,	// Connects to filter
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
		&CLSID_NULL,
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

class CPristineHevcDecoderFilter : public CTransformFilter
{

public:

	CPristineHevcDecoderFilter(LPUNKNOWN pUnk, HRESULT* phr);
	~CPristineHevcDecoderFilter();

	DECLARE_IUNKNOWN;
	static CUnknown* WINAPI CreateInstance(LPUNKNOWN, HRESULT *);

	HRESULT Transform(IMediaSample *pIn, IMediaSample *pOut);
	HRESULT CheckInputType(const CMediaType *mtIn);
	HRESULT CheckTransform(const CMediaType *mtIn, const CMediaType *mtOut);
	HRESULT DecideBufferSize(IMemAllocator *pAlloc,
		ALLOCATOR_PROPERTIES *pProperties);
	HRESULT GetMediaType(int iPosition, CMediaType *pMediaType);

};