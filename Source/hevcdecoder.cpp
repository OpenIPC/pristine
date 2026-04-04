#include "hevcdecoder.h"

static GUID MEDIASUBTYPE_H265 = { 0x35363248, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
static GUID MEDIASUBTYPE_HEVC = { 0x43564548, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };
static GUID MEDIASUBTYPE_HVC1 = { 0x31435648, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };


#pragma warning(disable:4355 4127)
CPristineHevcDecoderFilter::CPristineHevcDecoderFilter(LPUNKNOWN pUnk, HRESULT *phr)
	: CPristineVideoDecoderFilter(NAME("Pristine: HEVC Decoder"), pUnk, CLSID_PristineHevcDecoder)
	, m_nalLengthSize(4)
	, m_pParamSets(nullptr)
	, m_cbParamSets(0)
	, m_bParamSetsSent(FALSE)
	, m_bIsHVC1(FALSE) {}

CPristineHevcDecoderFilter::~CPristineHevcDecoderFilter()
{
	free(m_pParamSets);
	m_pParamSets = nullptr;
	MFShutdown();
}

CUnknown * WINAPI CPristineHevcDecoderFilter::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
	ASSERT(phr);

	*phr = MFStartup(MF_VERSION);
	if (FAILED(*phr)) return NULL;

	CPristineHevcDecoderFilter *pNewObject = new CPristineHevcDecoderFilter(pUnk, phr);
	if (pNewObject == NULL)
	{
		if (phr) *phr = E_OUTOFMEMORY;
		return NULL;
	}

	// Try to initialize D3D11 â€“ this is optional.
	// If it fails, we continue in software mode.
	HRESULT hrD3D = pNewObject->InitializeD3D11();

	// MFT activation is deferred to CheckInputType, where we know
	// the exact input format and can properly configure D3D11 before
	// SetInputType. This is required for hardware MFTs.
	return pNewObject;
}


// ---------------------------------------------------------------------------
// Called from CheckInputType once we know the input format.
// Tries hardware MFT first (with D3D11 acceleration), falls back to software.
// ---------------------------------------------------------------------------
HRESULT CPristineHevcDecoderFilter::ActivateDecoderMFT()
{
	if (m_pDecoderTransform)
		return S_OK;

	static const MFT_REGISTER_TYPE_INFO InputTypeInformation =
		{ MFMediaType_Video, MFVideoFormat_HEVC };

	IMFActivate **ppActivates = nullptr;
	UINT32 nActivateCount = 0;
	HRESULT hr = E_FAIL;

	// Try hardware MFT first (only if D3D11 is available)
	if (m_pDXGIDeviceManager && m_pD3D11Device)
	{
		// Retrieve the adapter LUID from our D3D11 device.
		// MFTEnum2 needs this to find hardware MFTs for the correct GPU.
		LUID adapterLuid = {};
		IDXGIDevice *pDXGIDevice = nullptr;
		hr = m_pD3D11Device->QueryInterface(__uuidof(IDXGIDevice), (void**)&pDXGIDevice);
		if (SUCCEEDED(hr))
		{
			IDXGIAdapter *pAdapter = nullptr;
			hr = pDXGIDevice->GetAdapter(&pAdapter);
			if (SUCCEEDED(hr))
			{
				DXGI_ADAPTER_DESC desc;
				if (SUCCEEDED(pAdapter->GetDesc(&desc)))
					adapterLuid = desc.AdapterLuid;
				pAdapter->Release();
			}
			pDXGIDevice->Release();
		}

		IMFAttributes *pEnumAttrs = nullptr;
		hr = MFCreateAttributes(&pEnumAttrs, 1);
		if (SUCCEEDED(hr) && (adapterLuid.HighPart || adapterLuid.LowPart))
		{
			UINT64 luid64 = ((UINT64)adapterLuid.HighPart << 32) | (UINT32)adapterLuid.LowPart;
			pEnumAttrs->SetUINT64(MFT_ENUM_ADAPTER_LUID, luid64);
		}

		// Try MFTEnum2 (Windows 10 1709+), fallback to MFTEnumEx (no adapter LUID support)
		typedef HRESULT (WINAPI *PFN_MFTEnum2)(
			GUID, UINT32, const MFT_REGISTER_TYPE_INFO*,
			const MFT_REGISTER_TYPE_INFO*, IMFAttributes*,
			IMFActivate***, UINT32*);

		PFN_MFTEnum2 pfnMFTEnum2 = nullptr;
		HMODULE hMfplat = GetModuleHandleW(L"mfplat.dll");
		if (hMfplat)
			pfnMFTEnum2 = (PFN_MFTEnum2)GetProcAddress(hMfplat, "MFTEnum2");

		if (pfnMFTEnum2)
		{
			hr = pfnMFTEnum2(MFT_CATEGORY_VIDEO_DECODER,
				MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
				&InputTypeInformation, NULL, pEnumAttrs,
				&ppActivates, &nActivateCount);
		}
		else
		{
			MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
				MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER,
				&InputTypeInformation, NULL, &ppActivates, &nActivateCount);
		}

		SafeRelease(&pEnumAttrs);

		for (UINT32 i = 0; i < nActivateCount; i++)
		{
			IMFTransform *pMFT = nullptr;
			hr = ppActivates[i]->ActivateObject(
				__uuidof(IMFTransform), (void**)&pMFT);
			if (FAILED(hr))
				continue;

			// Set D3D manager BEFORE configuring input/output types.
			// This is critical for hardware MFTs.
			hr = pMFT->ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER,
				reinterpret_cast<ULONG_PTR>(m_pDXGIDeviceManager));
			if (FAILED(hr))
			{
				pMFT->Release();
				continue;
			}

			m_pDecoderTransform = pMFT;
			m_bIsHardwareMFT = TRUE;
			break;
		}

		for (UINT32 i = 0; i < nActivateCount; i++)
			ppActivates[i]->Release();
		CoTaskMemFree(ppActivates);
		ppActivates = nullptr;
	}

	// Fallback to software MFT
	if (!m_pDecoderTransform)
	{
		nActivateCount = 0;
		MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
			MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
			&InputTypeInformation, NULL, &ppActivates, &nActivateCount);

		if (nActivateCount > 0)
		{
			hr = ppActivates[0]->ActivateObject(
				__uuidof(IMFTransform), (void**)&m_pDecoderTransform);
			m_bIsHardwareMFT = FALSE;

			// Microsoft's HEVC software decoder can use DXVA2/D3D11
			// for hardware-accelerated decoding when given a D3D manager.
			if (SUCCEEDED(hr) && m_pDXGIDeviceManager)
			{
				HRESULT hrD3D = m_pDecoderTransform->ProcessMessage(
					MFT_MESSAGE_SET_D3D_MANAGER,
					reinterpret_cast<ULONG_PTR>(m_pDXGIDeviceManager));
				if (SUCCEEDED(hrD3D))
					m_bIsHardwareMFT = TRUE;  // DXVA2-accelerated path
			}
		}

		for (UINT32 i = 0; i < nActivateCount; i++)
			ppActivates[i]->Release();
		CoTaskMemFree(ppActivates);
	}

	if (!m_pDecoderTransform)
	{
		int result = MessageBoxW(NULL,
			L"No HEVC (H.265) video decoder was found on this system.\n\n"
			L"Would you like to install the HEVC Video Extensions from the Microsoft Store?",
			L"Pristine: HEVC Decoder", MB_YESNO | MB_ICONQUESTION);
		if (result == IDYES)
			ShellExecuteW(NULL, L"open",
				L"ms-windows-store://pdp/?ProductId=9nmzlz57r3t7", NULL, NULL, SW_SHOWNORMAL);
		return E_FAIL;
	}

	return S_OK;
}

// ===========================================================================
// Send VPS/SPS/PPS (Annex B) to the MFT as the first input sample,
// so the decoder knows the stream parameters.
// ===========================================================================
HRESULT CPristineHevcDecoderFilter::SendParameterSets()
{
	if (m_bParamSetsSent || !m_pParamSets || !m_cbParamSets)
		return S_OK;

	IMFMediaBuffer *pBuf = nullptr;
	HRESULT hr = MFCreateMemoryBuffer(m_cbParamSets, &pBuf);
	if (FAILED(hr)) return hr;

	BYTE *pData = nullptr;
	hr = pBuf->Lock(&pData, nullptr, nullptr);
	if (SUCCEEDED(hr))
	{
		memcpy(pData, m_pParamSets, m_cbParamSets);
		pBuf->Unlock();
		pBuf->SetCurrentLength(m_cbParamSets);
	}

	IMFSample *pSample = nullptr;
	hr = MFCreateSample(&pSample);
	if (SUCCEEDED(hr))
	{
		pSample->AddBuffer(pBuf);
		hr = m_pDecoderTransform->ProcessInput(0, pSample, 0);
		SafeRelease(&pSample);
	}
	SafeRelease(&pBuf);

	m_bParamSetsSent = TRUE;
	return hr;
}

// ===========================================================================
// Used to feed the MFT and drain ALL available output frames in a loop.
// The base-class Transform() model is 1-in-1-out which doesn't work for video
// decoders that need several reference frames before producing output.
// ===========================================================================
HRESULT CPristineHevcDecoderFilter::Receive(IMediaSample *pIn)
{
	CheckPointer(pIn, E_POINTER);

	if (!m_pOutput->IsConnected())
		return NOERROR;

	HRESULT hr;
	BYTE *pInputBuffer = nullptr;

	hr = pIn->GetPointer(&pInputBuffer);
	if (FAILED(hr)) return hr;

	long lInputSize = pIn->GetActualDataLength();

	if (m_bIsHVC1 && !m_bParamSetsSent)
		SendParameterSets();

	// Convert HVC1 length-prefixed NAL units into Annex B start codes
	BYTE *pConvertedBuf = nullptr;
	BYTE *pSrcData = pInputBuffer;
	long  lSrcSize = lInputSize;

	if (m_bIsHVC1 && m_nalLengthSize > 0)
	{
		// Worst case: each NAL gets a 4-byte start code (same or larger than length field)
		// Extra room: (4 - m_nalLengthSize) per NAL unit, but simpler to allocate lInputSize + slack
		pConvertedBuf = (BYTE*)malloc(lInputSize + 1024);
		if (!pConvertedBuf) return E_OUTOFMEMORY;

		BYTE *pDst = pConvertedBuf;
		long  srcPos = 0;

		while (srcPos + (long)m_nalLengthSize <= lInputSize)
		{
			// Read NAL unit length (big-endian)
			UINT32 nalLen = 0;
			for (UINT i = 0; i < m_nalLengthSize; i++)
				nalLen = (nalLen << 8) | pInputBuffer[srcPos + i];
			srcPos += m_nalLengthSize;

			if (!nalLen || srcPos + (long)nalLen > lInputSize)
				break;

			// Write Annex B start code
			pDst[0] = 0; pDst[1] = 0; pDst[2] = 0; pDst[3] = 1;
			pDst += 4;

			// Copy NAL unit data
			memcpy(pDst, pInputBuffer + srcPos, nalLen);
			pDst += nalLen;
			srcPos += nalLen;
		}

		pSrcData = pConvertedBuf;
		lSrcSize = (long)(pDst - pConvertedBuf);
	}

	IMFMediaBuffer *pMFInputBuffer = nullptr;
	hr = MFCreateMemoryBuffer(lSrcSize, &pMFInputBuffer);
	if (FAILED(hr))
	{
		free(pConvertedBuf);
		return hr;
	}

	BYTE *pMFBuffer = nullptr;
	DWORD maxLength = 0, currentLength = 0;
	hr = pMFInputBuffer->Lock(&pMFBuffer, &maxLength, &currentLength);
	if (FAILED(hr))
	{
		SafeRelease(&pMFInputBuffer);
		free(pConvertedBuf);
		return hr;
	}

	memcpy(pMFBuffer, pSrcData, lSrcSize);
	pMFInputBuffer->Unlock();
	pMFInputBuffer->SetCurrentLength(lSrcSize);

	free(pConvertedBuf);
	pConvertedBuf = nullptr;

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
		// When MFT internal buffers are full, output must be drained first
		HRESULT hrDrain = DrainOutput(pIn);
		if (FAILED(hrDrain))
		{
			SafeRelease(&pMFInputSample);
			return hrDrain;
		}

		// Retry the input after draining
		hr = m_pDecoderTransform->ProcessInput(0, pMFInputSample, 0);
	}

	SafeRelease(&pMFInputSample);

	if (FAILED(hr))
		return hr;

	hr = DrainOutput(pIn);
	return hr;
}

BOOL CPristineHevcDecoderFilter::IsInputSubtypeSupported(const GUID &subtype) const
{
	return (subtype == MEDIASUBTYPE_H265 ||
		subtype == MEDIASUBTYPE_HEVC ||
		subtype == MEDIASUBTYPE_HVC1);
}

HRESULT CPristineHevcDecoderFilter::CheckInputType(const CMediaType *mtIn)
{
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

		// Parse HEVCDecoderConfigurationRecord from dwSequenceHeader
		m_bIsHVC1 = (mtIn->subtype == MEDIASUBTYPE_HVC1);
		if (m_bIsHVC1 && pvi->cbSequenceHeader > 22)
		{
			const BYTE *cfg = (const BYTE*)pvi->dwSequenceHeader;
			UINT cbCfg = pvi->cbSequenceHeader;

			// HEVCDecoderConfigurationRecord layout:
			//  byte 0:  configurationVersion (1)
			//  ...
			//  byte 21: lengthSizeMinusOne (lower 2 bits)
			//  byte 22: numOfArrays
			m_nalLengthSize = (cfg[21] & 0x03) + 1;

			// First pass: calculate total size needed
			UINT totalSize = 0;
			UINT pos = 22;
			if (pos < cbCfg)
			{
				BYTE numArrays = cfg[pos++];
				UINT savedPos = pos;

				for (BYTE a = 0; a < numArrays && pos + 2 < cbCfg; a++)
				{
					pos++; // Array_completeness + NAL_unit_type
					UINT16 numNalus = (cfg[pos] << 8) | cfg[pos + 1];
					pos += 2;
					for (UINT16 n = 0; n < numNalus && pos + 2 <= cbCfg; n++)
					{
						UINT16 nalLen = (cfg[pos] << 8) | cfg[pos + 1];
						pos += 2;
						totalSize += 4 + nalLen; // Start code (4) + NAL data
						pos += nalLen;
					}
				}

				// Second pass: build Annex B buffer
				if (totalSize > 0)
				{
					free(m_pParamSets);
					m_pParamSets = (BYTE*)malloc(totalSize);
					m_cbParamSets = totalSize;
					m_bParamSetsSent = FALSE;

					BYTE *pDst = m_pParamSets;
					pos = savedPos;
					for (BYTE a = 0; a < numArrays && pos + 2 < cbCfg; a++)
					{
						pos++; // Skip array header byte
						UINT16 numNalus = (cfg[pos] << 8) | cfg[pos + 1];
						pos += 2;
						for (UINT16 n = 0; n < numNalus && pos + 2 <= cbCfg; n++)
						{
							UINT16 nalLen = (cfg[pos] << 8) | cfg[pos + 1];
							pos += 2;
							// Annex B start code
							pDst[0] = 0; pDst[1] = 0; pDst[2] = 0; pDst[3] = 1;
							pDst += 4;
							memcpy(pDst, cfg + pos, nalLen);
							pDst += nalLen;
							pos += nalLen;
						}
					}
				}
			}
		}
		else if (!m_bIsHVC1)
		{
			m_nalLengthSize = 0;
		}
	}
	else return VFW_E_TYPE_NOT_ACCEPTED;

	// Must happen before SetInputType so the D3D11 device manager
	// is already configured on hardware MFTs.
	HRESULT hr = ActivateDecoderMFT();
	if (FAILED(hr))
		return VFW_E_TYPE_NOT_ACCEPTED;

	// If the MFT is already fully initialized, just accept the type
	if (m_bMFTInitialized)
		return NOERROR;

	IMFMediaType *pInputType = nullptr;
	hr = MFCreateMediaType(&pInputType);
	if (SUCCEEDED(hr))
	{
		pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_HEVC);
		MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, m_width, m_height);
		if (m_avgtime > 0)
		{
			MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE,
				(UINT32)(10000000 / m_avgtime), 1);
		}
		hr = m_pDecoderTransform->SetInputType(0, pInputType, 0);

		// If hardware MFT rejected the input type, fallback to software
		if (FAILED(hr) && m_bIsHardwareMFT)
		{
			SafeRelease(&m_pDecoderTransform);
			m_bIsHardwareMFT = FALSE;

			// Force software-only re-activation
			static const MFT_REGISTER_TYPE_INFO inType =
				{ MFMediaType_Video, MFVideoFormat_HEVC };
			IMFActivate **ppAct = nullptr;
			UINT32 nAct = 0;

			MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER,
				MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
				&inType, NULL, &ppAct, &nAct);

			if (nAct > 0)
				ppAct[0]->ActivateObject(
					__uuidof(IMFTransform), (void**)&m_pDecoderTransform);
			for (UINT32 i = 0; i < nAct; i++) ppAct[i]->Release();
			CoTaskMemFree(ppAct);

			if (m_pDecoderTransform)
				hr = m_pDecoderTransform->SetInputType(0, pInputType, 0);
			else
				hr = E_FAIL;
		}
	}
	SafeRelease(&pInputType);

	if (FAILED(hr))
		return NOERROR;

	{
		GUID mftOutputSubtype = GUID{};
		IMFMediaType *pSelectedType = nullptr;
		IMFMediaType *pFallbackRGB32 = nullptr;

		for (DWORD idx = 0; ; idx++)
		{
			IMFMediaType *pAvail = nullptr;
			hr = m_pDecoderTransform->GetOutputAvailableType(0, idx, &pAvail);
			if (FAILED(hr))
				break;   // No more types

			GUID subGuid = GUID{};
			pAvail->GetGUID(MF_MT_SUBTYPE, &subGuid);

			if (subGuid == MFVideoFormat_NV12)
			{
				// Preferred, take ownership immediately
				SafeRelease(&pFallbackRGB32);
				pSelectedType = pAvail;
				mftOutputSubtype = MFVideoFormat_NV12;
				break;
			}
			else if (subGuid == MFVideoFormat_RGB32 && !pFallbackRGB32)
			{
				// Hold as fallback, keep looking
				pFallbackRGB32 = pAvail;  
				mftOutputSubtype = MFVideoFormat_RGB32;
			}
			else
			{
				pAvail->Release();
			}
		}

		if (!pSelectedType && pFallbackRGB32)
			pSelectedType = pFallbackRGB32;
		else
			SafeRelease(&pFallbackRGB32);

		if (pSelectedType)
		{
			hr = m_pDecoderTransform->SetOutputType(0, pSelectedType, 0);
			SafeRelease(&pSelectedType);
		}
		else
		{
			// Create NV12 format manually as last resort
			IMFMediaType *pOutputType = nullptr;
			hr = MFCreateMediaType(&pOutputType);
			if (SUCCEEDED(hr))
			{
				pOutputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
				pOutputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
				MFSetAttributeSize(pOutputType, MF_MT_FRAME_SIZE, m_width, m_height);
				hr = m_pDecoderTransform->SetOutputType(0, pOutputType, 0);
			}
			SafeRelease(&pOutputType);
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