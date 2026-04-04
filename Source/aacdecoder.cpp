#include "aacdecoder.h"

static GUID MEDIASUBTYPE_RAW_AAC1 = { 0x000000FF, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
static GUID MEDIASUBTYPE_MPEG_ADTS_AAC = { 0x00001610, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
static GUID MEDIASUBTYPE_MPEG_LOAS = { 0x00001612, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };


#pragma warning(disable:4355 4127)
CPristineAacDecoderFilter::CPristineAacDecoderFilter(LPUNKNOWN pUnk, HRESULT *phr)
	: CPristineAudioDecoderFilter(NAME("Pristine: AAC Decoder"), pUnk, CLSID_PristineAacDecoder) {}

CPristineAacDecoderFilter::~CPristineAacDecoderFilter()
{
	MFShutdown();
}

CUnknown * WINAPI CPristineAacDecoderFilter::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
	ASSERT(phr);

	*phr = MFStartup(MF_VERSION);
	if (FAILED(*phr)) return NULL;

	CPristineAacDecoderFilter *pNewObject = new CPristineAacDecoderFilter(pUnk, phr);
	if (pNewObject == NULL)
	{
		if (phr) *phr = E_OUTOFMEMORY;
		return NULL;
	}

	return pNewObject;
}


// ---------------------------------------------------------------------------
// Activate the AAC MFT decoder (software only, no D3D needed for audio).
// ---------------------------------------------------------------------------
HRESULT CPristineAacDecoderFilter::ActivateDecoderMFT()
{
	if (m_pDecoderTransform)
		return S_OK;

	static const MFT_REGISTER_TYPE_INFO InputTypeInformation =
		{ MFMediaType_Audio, MFAudioFormat_AAC };

	IMFActivate **ppActivates = nullptr;
	UINT32 nActivateCount = 0;

	MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
		MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
		&InputTypeInformation, NULL, &ppActivates, &nActivateCount);

	HRESULT hr = E_FAIL;
	if (nActivateCount > 0)
	{
		hr = ppActivates[0]->ActivateObject(
			__uuidof(IMFTransform), (void**)&m_pDecoderTransform);
	}

	for (UINT32 i = 0; i < nActivateCount; i++)
		ppActivates[i]->Release();
	CoTaskMemFree(ppActivates);

	if (!m_pDecoderTransform)
	{
		int result = MessageBoxW(NULL,
			L"No AAC audio decoder was found on this system.\n\n"
			L"This component is normally included with Windows.\n"
			L"Please check your Windows installation.",
			L"Pristine: AAC Decoder", MB_OK | MB_ICONWARNING);
		return E_FAIL;
	}

	return S_OK;
}


// ===========================================================================
// AAC frames are passed directly to the MFT.
// ===========================================================================
HRESULT CPristineAacDecoderFilter::Receive(IMediaSample *pIn)
{
	CheckPointer(pIn, E_POINTER);

	if (!m_pOutput->IsConnected())
		return NOERROR;

	HRESULT hr;
	BYTE *pInputBuffer = nullptr;

	hr = pIn->GetPointer(&pInputBuffer);
	if (FAILED(hr)) return hr;

	long lInputSize = pIn->GetActualDataLength();

	IMFMediaBuffer *pMFInputBuffer = nullptr;
	hr = MFCreateMemoryBuffer(lInputSize, &pMFInputBuffer);
	if (FAILED(hr)) return hr;

	BYTE *pMFBuffer = nullptr;
	DWORD maxLength = 0, currentLength = 0;
	hr = pMFInputBuffer->Lock(&pMFBuffer, &maxLength, &currentLength);
	if (FAILED(hr))
	{
		SafeRelease(&pMFInputBuffer);
		return hr;
	}

	memcpy(pMFBuffer, pInputBuffer, lInputSize);
	pMFInputBuffer->Unlock();
	pMFInputBuffer->SetCurrentLength(lInputSize);

	IMFSample *pMFInputSample = nullptr;
	hr = MFCreateSample(&pMFInputSample);
	if (FAILED(hr))
	{
		SafeRelease(&pMFInputBuffer);
		return hr;
	}

	pMFInputSample->AddBuffer(pMFInputBuffer);
	SafeRelease(&pMFInputBuffer);

	REFERENCE_TIME rtStart = 0, rtEnd = 0;
	if (SUCCEEDED(pIn->GetTime(&rtStart, &rtEnd)))
	{
		pMFInputSample->SetSampleTime(rtStart);
		pMFInputSample->SetSampleDuration(rtEnd - rtStart);
	}

	hr = m_pDecoderTransform->ProcessInput(0, pMFInputSample, 0);

	if (hr == MF_E_NOTACCEPTING)
	{
		HRESULT hrDrain = DrainOutput(pIn);
		if (FAILED(hrDrain))
		{
			SafeRelease(&pMFInputSample);
			return hrDrain;
		}

		hr = m_pDecoderTransform->ProcessInput(0, pMFInputSample, 0);
	}

	SafeRelease(&pMFInputSample);

	if (FAILED(hr))
		return hr;

	hr = DrainOutput(pIn);
	return hr;
}

BOOL CPristineAacDecoderFilter::IsInputSubtypeSupported(const GUID &subtype) const
{
	return (subtype == MEDIASUBTYPE_RAW_AAC1 ||
		subtype == MEDIASUBTYPE_MPEG_ADTS_AAC ||
		subtype == MEDIASUBTYPE_MPEG_LOAS);
}

HRESULT CPristineAacDecoderFilter::CheckInputType(const CMediaType *mtIn)
{
	CheckPointer(mtIn, E_POINTER);

	if (mtIn->majortype != MEDIATYPE_Audio)
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (mtIn->subtype != MEDIASUBTYPE_RAW_AAC1 &&
		mtIn->subtype != MEDIASUBTYPE_MPEG_ADTS_AAC &&
		mtIn->subtype != MEDIASUBTYPE_MPEG_LOAS)
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (mtIn->formattype != FORMAT_WaveFormatEx)
		return VFW_E_TYPE_NOT_ACCEPTED;

	WAVEFORMATEX *pWfx = (WAVEFORMATEX*)mtIn->pbFormat;
	if (!pWfx)
		return VFW_E_TYPE_NOT_ACCEPTED;

	m_nChannels = pWfx->nChannels;
	m_nSamplesPerSec = pWfx->nSamplesPerSec;

	if (!m_nChannels)    m_nChannels = 2;
	if (!m_nSamplesPerSec) m_nSamplesPerSec = 44100;

	HRESULT hr = ActivateDecoderMFT();
	if (FAILED(hr))
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (m_bMFTInitialized)
		return NOERROR;

	// Configure MFT input type
	IMFMediaType *pInputType = nullptr;
	hr = MFCreateMediaType(&pInputType);
	if (SUCCEEDED(hr))
	{
		pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		pInputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
		pInputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_nChannels);
		pInputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_nSamplesPerSec);

		// AAC payload type: 0 = raw AAC, 1 = ADTS, 2 = ADIF, 3 = LOAS/LATM
		if (mtIn->subtype == MEDIASUBTYPE_MPEG_ADTS_AAC)
			pInputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 1);
		else if (mtIn->subtype == MEDIASUBTYPE_MPEG_LOAS)
			pInputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 3);
		else
			pInputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0);

		// The AAC MFT expects MF_MT_USER_DATA to contain a 12-byte
		// HEAACWAVEINFO header followed by the AudioSpecificConfig.
		// Splitters typically deliver an AudioSpecificConfig (2+ bytes)
		// in WAVEFORMATEX::cbSize extra data.
		if (pWfx->cbSize > 0)
		{
			const BYTE *pASC = (const BYTE*)pWfx + sizeof(WAVEFORMATEX);
			UINT32 cbASC = pWfx->cbSize;

			// Build the 12-byte HEAACWAVEINFO prefix + AudioSpecificConfig
			UINT32 cbUserData = 12 + cbASC;
			BYTE pUserData[512];
			ZeroMemory(pUserData, 12);

			// HEAACWAVEINFO fields (all little-endian):
			//  WORD  wPayloadType   (offset 0)
			//  WORD  wAudioProfileLevelIndication (offset 2)
			//  WORD  wStructType    (offset 4)  - 0
			//  WORD  wReserved1     (offset 6)  - 0
			//  DWORD dwReserved2    (offset 8)  - 0
			WORD payloadType = 0;
			if (mtIn->subtype == MEDIASUBTYPE_MPEG_ADTS_AAC)
				payloadType = 1;
			else if (mtIn->subtype == MEDIASUBTYPE_MPEG_LOAS)
				payloadType = 3;
			memcpy(pUserData + 0, &payloadType, 2);

			// Profile level indication: 0xFE = no indication
			WORD profileLevel = 0xFE;
			memcpy(pUserData + 2, &profileLevel, 2);

			memcpy(pUserData + 12, pASC, cbASC);
			pInputType->SetBlob(MF_MT_USER_DATA, pUserData, cbUserData);
		}
		else
		{
			// No AudioSpecificConfig; still send the 12-byte header
			BYTE userData[12];
			ZeroMemory(userData, 12);
			WORD payloadType = 0;
			if (mtIn->subtype == MEDIASUBTYPE_MPEG_ADTS_AAC)
				payloadType = 1;
			else if (mtIn->subtype == MEDIASUBTYPE_MPEG_LOAS)
				payloadType = 3;
			memcpy(userData + 0, &payloadType, 2);
			WORD profileLevel = 0xFE;
			memcpy(userData + 2, &profileLevel, 2);
			pInputType->SetBlob(MF_MT_USER_DATA, userData, 12);
		}

		hr = m_pDecoderTransform->SetInputType(0, pInputType, 0);
	}
	SafeRelease(&pInputType);

	if (FAILED(hr))
		return VFW_E_TYPE_NOT_ACCEPTED;

	// Configure MFT output type: prefer PCM 16-bit, accept float
	{
		IMFMediaType *pSelectedType = nullptr;

		for (DWORD idx = 0; ; idx++)
		{
			IMFMediaType *pAvail = nullptr;
			hr = m_pDecoderTransform->GetOutputAvailableType(0, idx, &pAvail);
			if (FAILED(hr))
				break;

			GUID subGuid = GUID{};
			pAvail->GetGUID(MF_MT_SUBTYPE, &subGuid);

			if (subGuid == MFAudioFormat_PCM)
			{
				SafeRelease(&pSelectedType);
				pSelectedType = pAvail;
				break;
			}
			else if (subGuid == MFAudioFormat_Float && !pSelectedType)
			{
				pSelectedType = pAvail;
			}
			else
			{
				pAvail->Release();
			}
		}

		if (pSelectedType)
		{
			hr = m_pDecoderTransform->SetOutputType(0, pSelectedType, 0);

			// Update cached parameters from the negotiated output type
			UINT32 ch = 0, sr = 0, bps = 0;
			GUID outSub = GUID{};
			pSelectedType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
			pSelectedType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
			pSelectedType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
			pSelectedType->GetGUID(MF_MT_SUBTYPE, &outSub);
			if (ch) m_nChannels = ch;
			if (sr) m_nSamplesPerSec = sr;
			// If MFT outputs float, we convert to 16-bit PCM in DeliverOneOutput,
			// so always advertise 16 bits to the downstream renderer.
			if (outSub == MFAudioFormat_Float)
				m_nBitsPerSample = 16;
			else if (bps)
				m_nBitsPerSample = bps;

			SafeRelease(&pSelectedType);
		}
	}

	if (SUCCEEDED(hr))
	{
		m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
		m_pDecoderTransform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
		m_bMFTInitialized = TRUE;
	}

	return NOERROR;
}
