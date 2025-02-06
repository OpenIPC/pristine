#include "hevcdecoder.h"

static GUID MEDIASUBTYPE_H265 = { 0x35363248, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
static GUID MEDIASUBTYPE_HEVC = { 0x43564548, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
static GUID MEDIASUBTYPE_HVC1 = { 0x31435648, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };

IMFTransform* m_pDecoderTransform = nullptr;
UINT32 m_width = 0, m_height = 0;
REFERENCE_TIME m_avgtime;


UINT GetAligned(UINT dimension, UINT alignment)
{
	return (dimension + alignment - 1) & ~(alignment - 1);
}

#pragma warning(disable:4355 4127)
CPristineHevcDecoderFilter::CPristineHevcDecoderFilter(LPUNKNOWN pUnk, HRESULT *phr) :
    CTransformFilter(NAME("Pristine: HEVC Decoder"), pUnk, CLSID_PristineHevcDecoder) {}

CPristineHevcDecoderFilter::~CPristineHevcDecoderFilter()
{
	if (m_pDecoderTransform)
	{
		m_pDecoderTransform->Release();
		m_pDecoderTransform = nullptr;
	}
	MFShutdown();
}

CUnknown * WINAPI CPristineHevcDecoderFilter::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
	static const MFT_REGISTER_TYPE_INFO InputTypeInformation = { MFMediaType_Video, MFVideoFormat_HEVC };
	IMFActivate** ppActivates;
	UINT32 nActivateCount = 0;

	ASSERT(phr);

	*phr = MFStartup(MF_VERSION);
	if (FAILED(*phr)) return NULL;

	MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
		&InputTypeInformation, NULL, &ppActivates, &nActivateCount);
	if (nActivateCount == 0)
		MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
			&InputTypeInformation, NULL, &ppActivates, &nActivateCount);
	if (nActivateCount == 0) return NULL;

	ppActivates[0]->ActivateObject(__uuidof(IMFTransform), (VOID**)&m_pDecoderTransform);

	CPristineHevcDecoderFilter *pNewObject = new CPristineHevcDecoderFilter(pUnk, phr);
	if (pNewObject == NULL && phr) *phr = E_OUTOFMEMORY;

	return pNewObject;
}


HRESULT CPristineHevcDecoderFilter::Transform(IMediaSample *pIn, IMediaSample *pOut)
{
	BYTE *pDecodedBuffer = nullptr, *pInputBuffer = nullptr, *pMFBuffer = nullptr, *pOutputBuffer = nullptr;
	DWORD currentLength = 0, maxLength = 0, processOutputStatus = 0;
	IMFMediaBuffer *pMFInputBuffer = nullptr, *pMFOutputBuffer = nullptr;
	IMFSample *pMFInputSample = nullptr, *pMFOutputSample = nullptr;

	CheckPointer(pIn, E_POINTER);
	CheckPointer(pOut, E_POINTER);

	HRESULT hr = pIn->GetPointer(&pInputBuffer);
	if (FAILED(hr)) return hr;

	long lInputSize = pIn->GetActualDataLength();
	hr = MFCreateMemoryBuffer(1024 * 1024, &pMFInputBuffer);
	if (FAILED(hr)) return hr;

	hr = pMFInputBuffer->Lock(&pMFBuffer, &maxLength, &currentLength);
	if (FAILED(hr))
	{
		pMFInputBuffer->Release();
		return hr;
	}

	memcpy(pMFBuffer, pInputBuffer, lInputSize);
	pMFInputBuffer->Unlock();

	pMFInputBuffer->SetCurrentLength(lInputSize);
	
	hr = MFCreateSample(&pMFInputSample);
	if (FAILED(hr))
	{
		pMFInputBuffer->Release();
		return hr;
	}

	hr = pMFInputSample->AddBuffer(pMFInputBuffer);
	pMFInputBuffer->Release();
	if (FAILED(hr))
	{
		pMFInputSample->Release();
		return hr;
	}

	hr = m_pDecoderTransform->ProcessInput(0, pMFInputSample, 0);
	if (FAILED(hr)) return hr;

	MFT_OUTPUT_DATA_BUFFER MFTOutputBuffer;
	ZeroMemory(&MFTOutputBuffer, sizeof(MFTOutputBuffer));

	hr = MFCreateMemoryBuffer(
		GetAligned(m_width, 16) * GetAligned(m_height, 16) * 12 / 8, &pMFOutputBuffer);
	if (FAILED(hr)) return hr;

	hr = MFCreateSample(&pMFOutputSample);
	if (FAILED(hr))
	{
		pMFOutputBuffer->Release();
		return hr;
	}

	hr = pMFOutputSample->AddBuffer(pMFOutputBuffer);
	pMFOutputBuffer->Release();
	if (FAILED(hr))
	{
		pMFOutputSample->Release();
		return hr;
	}

	MFTOutputBuffer.dwStreamID = 0;
	MFTOutputBuffer.pSample = pMFOutputSample;

	do {
		if ((hr = m_pDecoderTransform->ProcessOutput(0, 1, &MFTOutputBuffer, &processOutputStatus)) == MF_E_TRANSFORM_NEED_MORE_INPUT) {
			// Provide more input data
			hr = m_pDecoderTransform->ProcessInput(0, pMFInputSample, 0);
			if (FAILED(hr))
			{
				pMFOutputSample->Release();
				return hr;
			}
		}
	} while (hr == MF_E_TRANSFORM_NEED_MORE_INPUT);

	hr = pMFOutputSample->ConvertToContiguousBuffer(&pMFOutputBuffer);
	if (FAILED(hr))
	{
		pMFOutputSample->Release();
		return hr;
	}

	hr = pMFOutputBuffer->Lock(&pDecodedBuffer, &maxLength, &currentLength);
	if (FAILED(hr))
	{
		pMFOutputBuffer->Release();
		pMFOutputSample->Release();
		return hr;
	}

	hr = pOut->GetPointer(&pOutputBuffer);
	if (FAILED(hr))
	{
		pMFOutputBuffer->Unlock();
		pMFOutputBuffer->Release();
		pMFOutputSample->Release();
		return hr;
	}

	memcpy(pOutputBuffer, pDecodedBuffer, currentLength);
	pMFOutputBuffer->Unlock();

	pOut->SetActualDataLength(currentLength);

	pMFOutputBuffer->Release();
	pMFOutputSample->Release();

	REFERENCE_TIME TimeStart, TimeEnd;
	if (SUCCEEDED(pIn->GetTime(&TimeStart, &TimeEnd)))
		pOut->SetTime(&TimeStart, &TimeEnd);

	LONGLONG MediaStart, MediaEnd;
	if (SUCCEEDED(pIn->GetMediaTime(&MediaStart, &MediaEnd)))
		pOut->SetMediaTime(&MediaStart, &MediaEnd);

	hr = pIn->IsSyncPoint();
	pOut->SetSyncPoint(hr == S_OK);

	hr = pIn->IsPreroll();
	pOut->SetPreroll(hr == S_OK);

	hr = pIn->IsDiscontinuity();
	pOut->SetDiscontinuity(hr == S_OK);

	return NOERROR;
}


HRESULT CPristineHevcDecoderFilter::CheckInputType(const CMediaType *mtIn)
{
	IMFMediaType* pInputType = nullptr, * pOutputType = nullptr;

	CheckPointer(mtIn, E_POINTER);

	if (mtIn->subtype != MEDIASUBTYPE_H265 &&
		mtIn->subtype != MEDIASUBTYPE_HEVC &&
		mtIn->subtype != MEDIASUBTYPE_HVC1)
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (mtIn->formattype == FORMAT_VideoInfo2)
	{
		VIDEOINFOHEADER2* pvi = (VIDEOINFOHEADER2*)mtIn->pbFormat;
		m_height = pvi->bmiHeader.biHeight;
		m_width = pvi->bmiHeader.biWidth;
		m_avgtime = pvi->AvgTimePerFrame;
	}
	else if (mtIn->formattype == FORMAT_VideoInfo)
	{
		VIDEOINFOHEADER* pvi = (VIDEOINFOHEADER*)mtIn->pbFormat;
		m_height = pvi->bmiHeader.biHeight;
		m_width = pvi->bmiHeader.biWidth;
		m_avgtime = pvi->AvgTimePerFrame;
	}
	else if (mtIn->formattype == FORMAT_MPEG2Video)
	{
		MPEG2VIDEOINFO* pvi = (MPEG2VIDEOINFO*)mtIn->pbFormat;
		m_height = pvi->hdr.bmiHeader.biHeight;
		m_width = pvi->hdr.bmiHeader.biWidth;
		m_avgtime = pvi->hdr.AvgTimePerFrame;
	}
	else return VFW_E_TYPE_NOT_ACCEPTED;

	HRESULT hr = MFCreateMediaType(&pInputType);
	if (SUCCEEDED(hr))
	{
		pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
		MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, m_width, m_height);
		m_pDecoderTransform->SetInputType(0, pInputType, 0);
	}
	if (pInputType) pInputType->Release();

	hr = MFCreateMediaType(&pOutputType);
	if (SUCCEEDED(hr))
	{
		pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
		MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, m_width, m_height);
		hr = m_pDecoderTransform->SetOutputType(0, pOutputType, 0);
	}
	if (pOutputType) pOutputType->Release();

	return NOERROR;
}

HRESULT CPristineHevcDecoderFilter::CheckTransform(const CMediaType *mtIn, const CMediaType *mtOut)
{
	CheckPointer(mtIn, E_POINTER);
	CheckPointer(mtOut, E_POINTER);

	if (mtIn->subtype != MEDIASUBTYPE_H265 &&
		mtIn->subtype != MEDIASUBTYPE_HEVC &&
		mtIn->subtype != MEDIASUBTYPE_HVC1 || 
		mtOut->subtype != MEDIASUBTYPE_NV12) return VFW_E_TYPE_NOT_ACCEPTED;

	return NOERROR;
}

HRESULT CPristineHevcDecoderFilter::DecideBufferSize(IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *pProperties)
{
	if (m_pInput->IsConnected() == FALSE) return E_UNEXPECTED;

	CheckPointer(pAlloc, E_POINTER);
	CheckPointer(pProperties, E_POINTER);
	HRESULT hr = NOERROR;

	pProperties->cBuffers = 1;
	pProperties->cbBuffer = m_width * m_height * 12 / 8;
	ASSERT(pProperties->cbBuffer);

	ALLOCATOR_PROPERTIES Actual;
	hr = pAlloc->SetProperties(pProperties, &Actual);
	if (FAILED(hr)) return hr;

	ASSERT(Actual.cBuffers == 1);

	if (pProperties->cBuffers > Actual.cBuffers ||
		pProperties->cbBuffer > Actual.cbBuffer) return E_FAIL;

	return NOERROR;
}

HRESULT CPristineHevcDecoderFilter::GetMediaType(int iPosition, CMediaType *pMediaType)
{
	if (iPosition < 0) return E_INVALIDARG;
	if (iPosition > 1) return VFW_S_NO_MORE_ITEMS;

	pMediaType->InitMediaType();
	pMediaType->SetType(&MEDIATYPE_Video);
	pMediaType->SetSubtype(&MEDIASUBTYPE_NV12);

	if (iPosition == 0) {
		pMediaType->SetFormatType(&FORMAT_VideoInfo2);
		VIDEOINFOHEADER2 format;
		ZeroMemory(&format, sizeof(VIDEOINFOHEADER2));
		format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		format.bmiHeader.biPlanes = 1;
		format.AvgTimePerFrame = m_avgtime;
		format.bmiHeader.biWidth = m_width;
		format.bmiHeader.biHeight = m_height;
		format.bmiHeader.biBitCount = 12;
		format.bmiHeader.biSizeImage =
			format.bmiHeader.biWidth * format.bmiHeader.biHeight * format.bmiHeader.biBitCount / 8;
		pMediaType->SetFormat(PBYTE(&format), sizeof(VIDEOINFOHEADER2));
		pMediaType->SetSampleSize(format.bmiHeader.biSizeImage);
	} else if (iPosition == 1) {
		pMediaType->SetFormatType(&FORMAT_VideoInfo);
		VIDEOINFOHEADER format;
		ZeroMemory(&format, sizeof(VIDEOINFOHEADER));
		format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		format.bmiHeader.biPlanes = 1;
		format.AvgTimePerFrame = m_avgtime;
		format.bmiHeader.biWidth = m_width;
		format.bmiHeader.biHeight = m_height;
		format.bmiHeader.biBitCount = 12;
		format.bmiHeader.biSizeImage =
			format.bmiHeader.biWidth * format.bmiHeader.biHeight * format.bmiHeader.biBitCount / 8;
		pMediaType->SetFormat(PBYTE(&format), sizeof(VIDEOINFOHEADER));
		pMediaType->SetSampleSize(format.bmiHeader.biSizeImage);
	}

	return NOERROR;
}