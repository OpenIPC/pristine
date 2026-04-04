#include "av1decoder.h"

static GUID MEDIASUBTYPE_AV01 = { 0x31305641, 0x0000, 0x0010, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 };


#pragma warning(disable:4355 4127)
CPristineAv1DecoderFilter::CPristineAv1DecoderFilter(LPUNKNOWN pUnk, HRESULT *phr)
	: CPristineVideoDecoderFilter(NAME("Pristine: AV1 Decoder"), pUnk, CLSID_PristineAv1Decoder) {}

CPristineAv1DecoderFilter::~CPristineAv1DecoderFilter()
{
	MFShutdown();
}

CUnknown * WINAPI CPristineAv1DecoderFilter::CreateInstance(LPUNKNOWN pUnk, HRESULT *phr)
{
	ASSERT(phr);

	*phr = MFStartup(MF_VERSION);
	if (FAILED(*phr)) return NULL;

	CPristineAv1DecoderFilter *pNewObject = new CPristineAv1DecoderFilter(pUnk, phr);
	if (pNewObject == NULL)
	{
		if (phr) *phr = E_OUTOFMEMORY;
		return NULL;
	}

	HRESULT hrD3D = pNewObject->InitializeD3D11();

	return pNewObject;
}


// ---------------------------------------------------------------------------
// Tries hardware MFT first (with D3D11 acceleration), falls back to software.
// ---------------------------------------------------------------------------
HRESULT CPristineAv1DecoderFilter::ActivateDecoderMFT()
{
	if (m_pDecoderTransform)
		return S_OK;

	static const MFT_REGISTER_TYPE_INFO InputTypeInformation =
		{ MFMediaType_Video, MFVideoFormat_AV1 };

	IMFActivate **ppActivates = nullptr;
	UINT32 nActivateCount = 0;
	HRESULT hr = E_FAIL;

	// Try hardware MFT first (only if D3D11 is available)
	if (m_pDXGIDeviceManager && m_pD3D11Device)
	{
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

			if (SUCCEEDED(hr) && m_pDXGIDeviceManager)
			{
				HRESULT hrD3D = m_pDecoderTransform->ProcessMessage(
					MFT_MESSAGE_SET_D3D_MANAGER,
					reinterpret_cast<ULONG_PTR>(m_pDXGIDeviceManager));
				if (SUCCEEDED(hrD3D))
					m_bIsHardwareMFT = TRUE;
			}
		}

		for (UINT32 i = 0; i < nActivateCount; i++)
			ppActivates[i]->Release();
		CoTaskMemFree(ppActivates);
	}

	if (!m_pDecoderTransform)
	{
		int result = MessageBoxW(NULL,
			L"No AV1 video decoder was found on this system.\n\n"
			L"Would you like to install the AV1 Video Extension from the Microsoft Store?",
			L"Pristine: AV1 Decoder", MB_YESNO | MB_ICONQUESTION);
		if (result == IDYES)
			ShellExecuteW(NULL, L"open",
				L"ms-windows-store://pdp/?ProductId=9mvzqvxjbq9v", NULL, NULL, SW_SHOWNORMAL);
		return E_FAIL;
	}

	return S_OK;
}

// ===========================================================================
// AV1 frames are passed directly to the MFT (no NAL conversion needed).
// ===========================================================================
HRESULT CPristineAv1DecoderFilter::Receive(IMediaSample *pIn)
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

BOOL CPristineAv1DecoderFilter::IsInputSubtypeSupported(const GUID &subtype) const
{
	return (subtype == MEDIASUBTYPE_AV01);
}

HRESULT CPristineAv1DecoderFilter::CheckInputType(const CMediaType *mtIn)
{
	CheckPointer(mtIn, E_POINTER);

	if (mtIn->subtype != MEDIASUBTYPE_AV01)
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
	else return VFW_E_TYPE_NOT_ACCEPTED;

	HRESULT hr = ActivateDecoderMFT();
	if (FAILED(hr))
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (m_bMFTInitialized)
		return NOERROR;

	IMFMediaType *pInputType = nullptr;
	hr = MFCreateMediaType(&pInputType);
	if (SUCCEEDED(hr))
	{
		pInputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		pInputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_AV1);
		MFSetAttributeSize(pInputType, MF_MT_FRAME_SIZE, m_width, m_height);
		if (m_avgtime > 0)
		{
			MFSetAttributeRatio(pInputType, MF_MT_FRAME_RATE,
				(UINT32)(10000000 / m_avgtime), 1);
		}
		hr = m_pDecoderTransform->SetInputType(0, pInputType, 0);

		if (FAILED(hr) && m_bIsHardwareMFT)
		{
			SafeRelease(&m_pDecoderTransform);
			m_bIsHardwareMFT = FALSE;

			static const MFT_REGISTER_TYPE_INFO inType =
				{ MFMediaType_Video, MFVideoFormat_AV1 };
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
				break;

			GUID subGuid = GUID{};
			pAvail->GetGUID(MF_MT_SUBTYPE, &subGuid);

			if (subGuid == MFVideoFormat_NV12)
			{
				SafeRelease(&pFallbackRGB32);
				pSelectedType = pAvail;
				mftOutputSubtype = MFVideoFormat_NV12;
				break;
			}
			else if (subGuid == MFVideoFormat_RGB32 && !pFallbackRGB32)
			{
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
