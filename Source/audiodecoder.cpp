#include "audiodecoder.h"


#pragma warning(disable:4355 4127)
CPristineAudioDecoderFilter::CPristineAudioDecoderFilter(
	TCHAR *pName, LPUNKNOWN pUnk, REFCLSID clsid)
	: CTransformFilter(pName, pUnk, clsid)
	, m_pDecoderTransform(nullptr)
	, m_nChannels(0)
	, m_nSamplesPerSec(0)
	, m_nBitsPerSample(16)
	, m_bMFTInitialized(FALSE)
	, m_rtNextOutputTime(0) {}

CPristineAudioDecoderFilter::~CPristineAudioDecoderFilter()
{
	SafeRelease(&m_pDecoderTransform);
}


HRESULT CPristineAudioDecoderFilter::Transform(IMediaSample *pIn, IMediaSample *pOut)
{
	// Transform() should not be called; Receive() is overridden by subclasses
	return E_UNEXPECTED;
}


// ---------------------------------------------------------------------------
// Format negotiation: offer 16-bit PCM output in two format wrappers.
// Position 0: WAVEFORMATEXTENSIBLE, Position 1: WAVEFORMATEX
// ---------------------------------------------------------------------------
HRESULT CPristineAudioDecoderFilter::GetMediaType(
	int iPosition, CMediaType *pMediaType)
{
	if (iPosition < 0) return E_INVALIDARG;
	if (iPosition > 1) return VFW_S_NO_MORE_ITEMS;

	if (!m_nChannels || !m_nSamplesPerSec)
		return E_UNEXPECTED;

	UINT32 nBlockAlign = m_nChannels * (m_nBitsPerSample / 8);
	UINT32 nAvgBytesPerSec = m_nSamplesPerSec * nBlockAlign;

	pMediaType->InitMediaType();
	pMediaType->SetType(&MEDIATYPE_Audio);
	pMediaType->SetSubtype(&MEDIASUBTYPE_PCM);
	pMediaType->SetTemporalCompression(FALSE);
	pMediaType->SetSampleSize(nBlockAlign);

	if (iPosition == 0)
	{
		// WAVEFORMATEXTENSIBLE (preferred for multi-channel or high bit-depth)
		pMediaType->SetFormatType(&FORMAT_WaveFormatEx);
		WAVEFORMATEXTENSIBLE wfx;
		ZeroMemory(&wfx, sizeof(wfx));
		wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
		wfx.Format.nChannels = (WORD)m_nChannels;
		wfx.Format.nSamplesPerSec = m_nSamplesPerSec;
		wfx.Format.wBitsPerSample = (WORD)m_nBitsPerSample;
		wfx.Format.nBlockAlign = (WORD)nBlockAlign;
		wfx.Format.nAvgBytesPerSec = nAvgBytesPerSec;
		wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
		wfx.Samples.wValidBitsPerSample = (WORD)m_nBitsPerSample;
		wfx.dwChannelMask = (m_nChannels == 1) ? SPEAKER_FRONT_CENTER
			: (m_nChannels == 2) ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
			: (m_nChannels == 6) ? 0x3F   // 5.1
			: (m_nChannels == 8) ? 0x63F  // 7.1
			: 0;
		wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
		pMediaType->SetFormat((BYTE*)&wfx, sizeof(WAVEFORMATEXTENSIBLE));
	}
	else
	{
		// Plain WAVEFORMATEX (widest compatibility)
		pMediaType->SetFormatType(&FORMAT_WaveFormatEx);
		WAVEFORMATEX wfx;
		ZeroMemory(&wfx, sizeof(wfx));
		wfx.wFormatTag = WAVE_FORMAT_PCM;
		wfx.nChannels = (WORD)m_nChannels;
		wfx.nSamplesPerSec = m_nSamplesPerSec;
		wfx.wBitsPerSample = (WORD)m_nBitsPerSample;
		wfx.nBlockAlign = (WORD)nBlockAlign;
		wfx.nAvgBytesPerSec = nAvgBytesPerSec;
		wfx.cbSize = 0;
		pMediaType->SetFormat((BYTE*)&wfx, sizeof(WAVEFORMATEX));
	}

	return NOERROR;
}

HRESULT CPristineAudioDecoderFilter::CheckTransform(
	const CMediaType *mtIn, const CMediaType *mtOut)
{
	CheckPointer(mtIn, E_POINTER);
	CheckPointer(mtOut, E_POINTER);

	if (!IsInputSubtypeSupported(mtIn->subtype))
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (mtOut->majortype != MEDIATYPE_Audio)
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (mtOut->subtype != MEDIASUBTYPE_PCM)
		return VFW_E_TYPE_NOT_ACCEPTED;

	return NOERROR;
}

HRESULT CPristineAudioDecoderFilter::DecideBufferSize(
	IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *pProperties)
{
	if (m_pInput->IsConnected() == FALSE) return E_UNEXPECTED;

	CheckPointer(pAlloc, E_POINTER);
	CheckPointer(pProperties, E_POINTER);

	// ~200ms of PCM audio per buffer
	UINT32 nBlockAlign = m_nChannels * (m_nBitsPerSample / 8);
	DWORD cbBuffer = m_nSamplesPerSec * nBlockAlign / 5;
	if (cbBuffer < 16384)
		cbBuffer = 16384;

	pProperties->cBuffers = max(pProperties->cBuffers, 4);
	pProperties->cbBuffer = cbBuffer;

	ALLOCATOR_PROPERTIES Actual;
	HRESULT hr = pAlloc->SetProperties(pProperties, &Actual);
	if (FAILED(hr)) return hr;

	if (pProperties->cBuffers > Actual.cBuffers ||
		pProperties->cbBuffer > Actual.cbBuffer)
		return E_FAIL;

	return NOERROR;
}


// ---------------------------------------------------------------------------
// Drain all available output frames from the MFT.
// ---------------------------------------------------------------------------
HRESULT CPristineAudioDecoderFilter::DrainOutput(IMediaSample *pIn)
{
	HRESULT hr;
	for (;;)
	{
		hr = DeliverOneOutput(pIn);
		if (hr == S_FALSE)
			break;
		if (FAILED(hr))
			return hr;
	}
	return S_OK;
}

// ---------------------------------------------------------------------------
// Extract one output buffer from the MFT and deliver it downstream.
// Returns S_OK if delivered, S_FALSE if no output available.
// ---------------------------------------------------------------------------
HRESULT CPristineAudioDecoderFilter::DeliverOneOutput(IMediaSample *pIn)
{
	HRESULT hr;
	DWORD processOutputStatus = 0;

	MFT_OUTPUT_STREAM_INFO outputStreamInfo;
	ZeroMemory(&outputStreamInfo, sizeof(outputStreamInfo));
	m_pDecoderTransform->GetOutputStreamInfo(0, &outputStreamInfo);

	BOOL bMFTProvidesSamples =
		(outputStreamInfo.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;

	MFT_OUTPUT_DATA_BUFFER outputBuffer;
	ZeroMemory(&outputBuffer, sizeof(outputBuffer));
	outputBuffer.dwStreamID = 0;

	IMFSample *pMFOutputSample = nullptr;
	IMFMediaBuffer *pMFOutputBuffer = nullptr;

	if (!bMFTProvidesSamples)
	{
		DWORD cbOutputBuffer = outputStreamInfo.cbSize;
		if (!cbOutputBuffer)
			cbOutputBuffer = 16384;

		if (outputStreamInfo.cbAlignment > 1)
			hr = MFCreateAlignedMemoryBuffer(cbOutputBuffer,
				outputStreamInfo.cbAlignment - 1, &pMFOutputBuffer);
		else
			hr = MFCreateMemoryBuffer(cbOutputBuffer, &pMFOutputBuffer);
		if (FAILED(hr)) return hr;

		hr = MFCreateSample(&pMFOutputSample);
		if (FAILED(hr)) { SafeRelease(&pMFOutputBuffer); return hr; }

		pMFOutputSample->AddBuffer(pMFOutputBuffer);
		SafeRelease(&pMFOutputBuffer);
		outputBuffer.pSample = pMFOutputSample;
	}

	hr = m_pDecoderTransform->ProcessOutput(0, 1, &outputBuffer, &processOutputStatus);

	if (hr == MF_E_TRANSFORM_NEED_MORE_INPUT)
	{
		SafeRelease(&pMFOutputSample);
		return S_FALSE;
	}

	if (hr == MF_E_TRANSFORM_STREAM_CHANGE)
	{
		// Re-negotiate output type (the MFT may change sample rate/channels)
		IMFMediaType *pNewType = nullptr;
		if (SUCCEEDED(m_pDecoderTransform->GetOutputAvailableType(0, 0, &pNewType)))
		{
			m_pDecoderTransform->SetOutputType(0, pNewType, 0);

			// Update our cached audio parameters
			UINT32 ch = 0, sr = 0, bps = 0;
			GUID outSub = GUID{};
			pNewType->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
			pNewType->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sr);
			pNewType->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bps);
			pNewType->GetGUID(MF_MT_SUBTYPE, &outSub);
			if (ch) m_nChannels = ch;
			if (sr) m_nSamplesPerSec = sr;
			// If MFT outputs float, we convert to 16-bit PCM in delivery,
			// so keep m_nBitsPerSample at 16.
			if (outSub == MFAudioFormat_Float)
				m_nBitsPerSample = 16;
			else if (bps)
				m_nBitsPerSample = bps;

			SafeRelease(&pNewType);
		}
		SafeRelease(&pMFOutputSample);
		return DeliverOneOutput(pIn);
	}

	if (FAILED(hr))
	{
		SafeRelease(&pMFOutputSample);
		return hr;
	}

	IMFSample *pResultSample = bMFTProvidesSamples
		? outputBuffer.pSample : pMFOutputSample;
	if (!pResultSample)
	{
		SafeRelease(&pMFOutputSample);
		return E_FAIL;
	}

	// Convert MF output to DirectShow sample
	IMFMediaBuffer *pResultBuffer = nullptr;
	hr = pResultSample->ConvertToContiguousBuffer(&pResultBuffer);
	if (FAILED(hr))
	{
		if (bMFTProvidesSamples)
			SafeRelease(&outputBuffer.pSample);
		else
			SafeRelease(&pMFOutputSample);
		return hr;
	}

	BYTE *pDecoded = nullptr;
	DWORD cbDecoded = 0;
	hr = pResultBuffer->Lock(&pDecoded, nullptr, &cbDecoded);
	if (FAILED(hr))
	{
		SafeRelease(&pResultBuffer);
		if (bMFTProvidesSamples)
			SafeRelease(&outputBuffer.pSample);
		else
			SafeRelease(&pMFOutputSample);
		return hr;
	}

	// The MFT may output IEEE float; convert to 16-bit PCM if needed
	IMFMediaType *pCurOut = nullptr;
	GUID curSub = GUID{};
	BOOL bFloat = FALSE;
	if (SUCCEEDED(m_pDecoderTransform->GetOutputCurrentType(0, &pCurOut)))
	{
		pCurOut->GetGUID(MF_MT_SUBTYPE, &curSub);
		bFloat = (curSub == MFAudioFormat_Float);
		SafeRelease(&pCurOut);
	}

	DWORD cbPCM = cbDecoded;
	if (bFloat)
		cbPCM = cbDecoded / 2;  // 32-bit float -> 16-bit int

	IMediaSample *pOut = nullptr;
	hr = m_pOutput->GetDeliveryBuffer(&pOut, NULL, NULL, 0);
	if (FAILED(hr))
	{
		pResultBuffer->Unlock();
		SafeRelease(&pResultBuffer);
		if (bMFTProvidesSamples)
			SafeRelease(&outputBuffer.pSample);
		else
			SafeRelease(&pMFOutputSample);
		return hr;
	}

	BYTE *pOutBuf = nullptr;
	hr = pOut->GetPointer(&pOutBuf);
	if (SUCCEEDED(hr))
	{
		DWORD cbCopy = min(cbPCM, (DWORD)pOut->GetSize());

		if (bFloat)
		{
			// Convert IEEE float to 16-bit PCM
			const float *pSrc = (const float*)pDecoded;
			short *pDst = (short*)pOutBuf;
			DWORD nSamples = cbCopy / 2;
			for (DWORD i = 0; i < nSamples; i++)
			{
				float val = pSrc[i] * 32767.0f;
				if (val > 32767.0f) val = 32767.0f;
				if (val < -32768.0f) val = -32768.0f;
				pDst[i] = (short)val;
			}
		}
		else
		{
			memcpy(pOutBuf, pDecoded, cbCopy);
		}

		pOut->SetActualDataLength(cbCopy);
	}

	pResultBuffer->Unlock();
	SafeRelease(&pResultBuffer);

	// Set timestamps based on the actual PCM data delivered
	REFERENCE_TIME rtStart = 0, rtEnd = 0;

	// Try to use the MFT sample time as the base; if the MFT provides
	// a valid time, snap our running counter to it.
	LONGLONG llSampleTime = 0;
	if (SUCCEEDED(pResultSample->GetSampleTime(&llSampleTime)))
		m_rtNextOutputTime = llSampleTime;

	rtStart = m_rtNextOutputTime;

	// Compute duration from the actual number of PCM samples delivered
	DWORD cbDelivered = pOut->GetActualDataLength();
	UINT32 nBlockAlign = m_nChannels * (m_nBitsPerSample / 8);
	if (nBlockAlign && m_nSamplesPerSec)
	{
		DWORD nFrames = cbDelivered / nBlockAlign;
		REFERENCE_TIME rtDuration = (REFERENCE_TIME)nFrames * 10000000LL
			/ m_nSamplesPerSec;
		rtEnd = rtStart + rtDuration;
		m_rtNextOutputTime = rtEnd;
	}
	else
	{
		rtEnd = rtStart;
	}

	pOut->SetTime(&rtStart, &rtEnd);

	pOut->SetSyncPoint(TRUE);
	pOut->SetPreroll(pIn->IsPreroll() == S_OK);
	pOut->SetDiscontinuity(pIn->IsDiscontinuity() == S_OK);

	hr = m_pOutput->Deliver(pOut);
	pOut->Release();

	if (bMFTProvidesSamples)
		SafeRelease(&outputBuffer.pSample);
	else
		SafeRelease(&pMFOutputSample);

	return SUCCEEDED(hr) ? S_OK : hr;
}
