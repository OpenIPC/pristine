#include "opusdecoder.h"

static GUID MEDIASUBTYPE_OPUS = { 0x0000704F, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };


#pragma warning(disable:4355 4127)
CPristineOpusDecoderFilter::CPristineOpusDecoderFilter(LPUNKNOWN pUnk, HRESULT *phr)
	: CPristineAudioDecoderFilter(NAME("Pristine: Opus Decoder"), pUnk, CLSID_PristineOpusDecoder) {}

CPristineOpusDecoderFilter::~CPristineOpusDecoderFilter()
{
	MFShutdown();
}

CUnknown * WINAPI CPristineOpusDecoderFilter::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
	ASSERT(phr);

	*phr = MFStartup(MF_VERSION);
	if (FAILED(*phr)) return NULL;

	CPristineOpusDecoderFilter *pNewObject = new CPristineOpusDecoderFilter(pUnk, phr);
	if (pNewObject == NULL)
	{
		if (phr) *phr = E_OUTOFMEMORY;
		return NULL;
	}

	return pNewObject;
}


// ---------------------------------------------------------------------------
// Activate the Opus MFT decoder (software only).
// The Opus MFT requires Windows 10 1809+ (Redstone 5) or the
// "Web Media Extensions" from the Microsoft Store.
// ---------------------------------------------------------------------------
HRESULT CPristineOpusDecoderFilter::ActivateDecoderMFT()
{
	if (m_pDecoderTransform)
		return S_OK;

	static const MFT_REGISTER_TYPE_INFO InputTypeInformation =
		{ MFMediaType_Audio, MFAudioFormat_Opus };

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
			L"No Opus audio decoder was found on this system.\n\n"
			L"Would you like to install the Web Media Extensions from the Microsoft Store?",
			L"Pristine: Opus Decoder", MB_YESNO | MB_ICONQUESTION);
		if (result == IDYES)
			ShellExecuteW(NULL, L"open",
				L"ms-windows-store://pdp/?ProductId=9n5tdp8vcmhs", NULL, NULL, SW_SHOWNORMAL);
		return E_FAIL;
	}

	return S_OK;
}


// ===========================================================================
// Opus frames are passed directly to the MFT.
// ===========================================================================
HRESULT CPristineOpusDecoderFilter::Receive(IMediaSample *pIn)
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

BOOL CPristineOpusDecoderFilter::IsInputSubtypeSupported(const GUID &subtype) const
{
	return (subtype == MEDIASUBTYPE_OPUS);
}

HRESULT CPristineOpusDecoderFilter::CheckInputType(const CMediaType *mtIn)
{
	CheckPointer(mtIn, E_POINTER);

	if (mtIn->majortype != MEDIATYPE_Audio)
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (mtIn->subtype != MEDIASUBTYPE_OPUS)
		return VFW_E_TYPE_NOT_ACCEPTED;

	// Opus may come with FORMAT_WaveFormatEx or no format
	if (mtIn->formattype == FORMAT_WaveFormatEx && mtIn->pbFormat)
	{
		WAVEFORMATEX *pWfx = (WAVEFORMATEX*)mtIn->pbFormat;
		m_nChannels = pWfx->nChannels;
		m_nSamplesPerSec = pWfx->nSamplesPerSec;
	}

	// Opus defaults: 48kHz, stereo
	if (!m_nChannels)      m_nChannels = 2;
	if (!m_nSamplesPerSec) m_nSamplesPerSec = 48000;

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
		pInputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_Opus);
		pInputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, m_nChannels);
		pInputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, m_nSamplesPerSec);

		// Pass codec private data if present
		if (mtIn->formattype == FORMAT_WaveFormatEx && mtIn->pbFormat)
		{
			WAVEFORMATEX *pWfx = (WAVEFORMATEX*)mtIn->pbFormat;
			if (pWfx->cbSize > 0)
			{
				pInputType->SetBlob(MF_MT_USER_DATA,
					(const UINT8*)pWfx + sizeof(WAVEFORMATEX), pWfx->cbSize);
			}
		}

		hr = m_pDecoderTransform->SetInputType(0, pInputType, 0);
	}
	SafeRelease(&pInputType);

	if (FAILED(hr))
		return NOERROR;

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

			UINT32 ch = 0, sr = 0, bps = 0;
			GUID outSub = GUID{};
			pSelectedType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
			pSelectedType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
			pSelectedType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
			pSelectedType->GetGUID(MF_MT_SUBTYPE, &outSub);
			if (ch) m_nChannels = ch;
			if (sr) m_nSamplesPerSec = sr;
			// If MFT outputs float we convert to 16-bit PCM in DeliverOneOutput,
			// so always advertise 16 bits to the downstream renderer.
			if (outSub != MFAudioFormat_Float && bps)
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
