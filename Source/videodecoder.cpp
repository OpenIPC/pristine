#include "videodecoder.h"


// ===========================================================================
// NV12 to RGB32 conversion (software fallback for VMR7/VMR9/Video Renderer)
// NV12: W*H Y plane + W*H/2 interleaved UV plane
// RGB32: bottom-up BGRA, 4 bytes per pixel
// ===========================================================================

static inline BYTE Clamp(int v)
{
	return (BYTE)(v < 0 ? 0 : (v > 255 ? 255 : v));
}

static void ConvertNV12toRGB32(
	const BYTE *pNV12, UINT width, UINT height, LONG srcPitch,
	BYTE *pRGB32, LONG dstStride)
{
	const BYTE *pY  = pNV12;
	const BYTE *pUV = pNV12 + srcPitch * height;

	for (UINT y = 0; y < height; y++)
	{
		BYTE *pDstRow = pRGB32 + (height - 1 - y) * dstStride;
		const BYTE *pYRow  = pY  + y * srcPitch;
		const BYTE *pUVRow = pUV + (y / 2) * srcPitch;

		for (UINT x = 0; x < width; x++)
		{
			int yVal = (int)pYRow[x] - 16;
			int u    = (int)pUVRow[(x & ~1)]     - 128;
			int v    = (int)pUVRow[(x & ~1) + 1] - 128;

			// BT.601 conversion
			int c = 298 * yVal;
			int r = (c + 409 * v + 128) >> 8;
			int g = (c - 100 * u - 208 * v + 128) >> 8;
			int b = (c + 516 * u + 128) >> 8;

			BYTE *px = pDstRow + x * 4;
			px[0] = Clamp(b);
			px[1] = Clamp(g);
			px[2] = Clamp(r);
			px[3] = 255;
		}
	}
}


CPristineVideoDecoderOutputPin::CPristineVideoDecoderOutputPin(
	TCHAR *pObjectName,
	CTransformFilter *pFilter,
	HRESULT *phr,
	LPCWSTR pName)
	: CTransformOutputPin(pObjectName, pFilter, phr, pName) {}

STDMETHODIMP CPristineVideoDecoderOutputPin::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
	if (riid == __uuidof(IMFGetService))
		return GetInterface((IMFGetService*)this, ppv);

	return CTransformOutputPin::NonDelegatingQueryInterface(riid, ppv);
}

STDMETHODIMP CPristineVideoDecoderOutputPin::GetService(
	REFGUID guidService, REFIID riid, LPVOID *ppvObject)
{
	CPristineVideoDecoderFilter *pFilter =
		static_cast<CPristineVideoDecoderFilter*>(m_pTransformFilter);

	if (guidService == MR_VIDEO_ACCELERATION_SERVICE)
	{
		IMFDXGIDeviceManager *pMgr = pFilter->GetDXGIDeviceManager();
		if (pMgr)
			return pMgr->QueryInterface(riid, ppvObject);
	}

	return MF_E_UNSUPPORTED_SERVICE;
}


#pragma warning(disable:4355 4127)
CPristineVideoDecoderFilter::CPristineVideoDecoderFilter(
	TCHAR *pName, LPUNKNOWN pUnk, REFCLSID clsid)
	: CTransformFilter(pName, pUnk, clsid)
	, m_pD3D11Device(nullptr)
	, m_pD3D11DeviceContext(nullptr)
	, m_pDXGIDeviceManager(nullptr)
	, m_resetToken(0)
	, m_pDecoderTransform(nullptr)
	, m_bIsHardwareMFT(FALSE)
	, m_width(0)
	, m_height(0)
	, m_avgtime(0)
	, m_bMFTInitialized(FALSE)
	, m_outputSubtype(MEDIASUBTYPE_NV12)
	, m_pOutputPin(nullptr) {}

CPristineVideoDecoderFilter::~CPristineVideoDecoderFilter()
{
	SafeRelease(&m_pDecoderTransform);
	SafeRelease(&m_pDXGIDeviceManager);
	SafeRelease(&m_pD3D11DeviceContext);
	SafeRelease(&m_pD3D11Device);
}

STDMETHODIMP CPristineVideoDecoderFilter::NonDelegatingQueryInterface(REFIID riid, void **ppv)
{
	if (riid == __uuidof(IMFGetService))
		return GetInterface((IMFGetService*)this, ppv);

	return CTransformFilter::NonDelegatingQueryInterface(riid, ppv);
}

STDMETHODIMP CPristineVideoDecoderFilter::GetService(
	REFGUID guidService, REFIID riid, LPVOID *ppvObject)
{
	if (!ppvObject) return E_POINTER;

	if (guidService == MR_VIDEO_ACCELERATION_SERVICE && m_pDXGIDeviceManager)
		return m_pDXGIDeviceManager->QueryInterface(riid, ppvObject);

	return MF_E_UNSUPPORTED_SERVICE;
}


int CPristineVideoDecoderFilter::GetPinCount()
{
	return 2;
}

CBasePin* CPristineVideoDecoderFilter::GetPin(int n)
{
	HRESULT hr = S_OK;

	if (n == 0)
	{
		if (m_pInput == nullptr)
		{
			m_pInput = new CTransformInputPin(
				NAME("Transform input pin"),
				this, &hr, L"Input");
			if (FAILED(hr)) return nullptr;
		}
		return m_pInput;
	}

	if (n == 1)
	{
		if (m_pOutput == nullptr)
		{
			m_pOutputPin = new CPristineVideoDecoderOutputPin(
				NAME("Transform output pin"),
				this, &hr, L"Output");
			if (FAILED(hr)) return nullptr;
			m_pOutput = m_pOutputPin;
		}
		return m_pOutput;
	}

	return nullptr;
}

HRESULT CPristineVideoDecoderFilter::Transform(IMediaSample *pIn, IMediaSample *pOut)
{
	// Transform() should not be called; Receive() is overridden by subclasses
	return E_UNEXPECTED;
}


HRESULT CPristineVideoDecoderFilter::InitializeD3D11()
{
	if (m_pD3D11Device)
		return S_OK;

	HRESULT hr;
	UINT creationFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT
	                   | D3D11_CREATE_DEVICE_BGRA_SUPPORT;

	D3D_FEATURE_LEVEL featureLevels[] = {
		D3D_FEATURE_LEVEL_11_1,
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_1,
		D3D_FEATURE_LEVEL_10_0,
		D3D_FEATURE_LEVEL_9_3,
	};
	D3D_FEATURE_LEVEL featureLevel;

	hr = D3D11CreateDevice(
		nullptr,
		D3D_DRIVER_TYPE_HARDWARE,
		nullptr,
		creationFlags,
		featureLevels,
		ARRAYSIZE(featureLevels),
		D3D11_SDK_VERSION,
		&m_pD3D11Device,
		&featureLevel,
		&m_pD3D11DeviceContext);

	if (FAILED(hr)) return hr;

	ID3D10Multithread *pMultithread = nullptr;
	hr = m_pD3D11Device->QueryInterface(__uuidof(ID3D10Multithread), (void**)&pMultithread);
	if (SUCCEEDED(hr))
	{
		pMultithread->SetMultithreadProtected(TRUE);
		pMultithread->Release();
	}

	hr = MFCreateDXGIDeviceManager(&m_resetToken, &m_pDXGIDeviceManager);
	if (FAILED(hr)) return hr;

	hr = m_pDXGIDeviceManager->ResetDevice(m_pD3D11Device, m_resetToken);
	return hr;
}

HRESULT CPristineVideoDecoderFilter::ConfigureMFTForD3D11()
{
	if (!m_pDecoderTransform || !m_pDXGIDeviceManager)
		return E_FAIL;

	HRESULT hr = m_pDecoderTransform->ProcessMessage(
		MFT_MESSAGE_SET_D3D_MANAGER,
		reinterpret_cast<ULONG_PTR>(m_pDXGIDeviceManager));

	if (SUCCEEDED(hr))
		m_bIsHardwareMFT = TRUE;

	return hr;
}

HRESULT CPristineVideoDecoderFilter::DrainOutput(IMediaSample *pIn)
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

HRESULT CPristineVideoDecoderFilter::DeliverOneOutput(IMediaSample *pIn)
{
	// Returns S_OK if a frame was delivered, 
	// S_FALSE if no output is available, or an error code.
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
		{
			IMFMediaType *pCurOut = nullptr;
			GUID curSub = GUID{};
			if (SUCCEEDED(m_pDecoderTransform->GetOutputCurrentType(0, &pCurOut)))
			{
				pCurOut->GetGUID(MF_MT_SUBTYPE, &curSub);
				pCurOut->Release();
			}
			if (curSub == MFVideoFormat_RGB32)
				cbOutputBuffer = GetAligned(m_width, 16) * GetAligned(m_height, 16) * 4;
			else
				cbOutputBuffer = GetAligned(m_width, 16) * GetAligned(m_height, 16) * 12 / 8;
		}

		if (outputStreamInfo.cbAlignment > 1)
			hr = MFCreateAlignedMemoryBuffer(cbOutputBuffer, outputStreamInfo.cbAlignment - 1, &pMFOutputBuffer);
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
		IMFMediaType *pSelectedType = nullptr;
		IMFMediaType *pFallbackRGB32 = nullptr;

		for (DWORD idx = 0; ; idx++)
		{
			IMFMediaType *pAvail = nullptr;
			HRESULT hrEnum = m_pDecoderTransform->GetOutputAvailableType(0, idx, &pAvail);
			if (FAILED(hrEnum))
				break;

			GUID subGuid = GUID{};
			pAvail->GetGUID(MF_MT_SUBTYPE, &subGuid);

			if (subGuid == MFVideoFormat_NV12)
			{
				SafeRelease(&pFallbackRGB32);
				pSelectedType = pAvail;
				break;
			}
			else if (subGuid == MFVideoFormat_RGB32 && !pFallbackRGB32)
			{
				pFallbackRGB32 = pAvail;
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
			GUID chosenSub = GUID{};
			pSelectedType->GetGUID(MF_MT_SUBTYPE, &chosenSub);
			HRESULT hrSet = m_pDecoderTransform->SetOutputType(0, pSelectedType, 0);
			pSelectedType->Release();
		}

		SafeRelease(&pMFOutputSample);
		return DeliverOneOutput(pIn);
	}

	if (FAILED(hr))
	{
		SafeRelease(&pMFOutputSample);
		return hr;
	}

	IMFSample *pResultSample = bMFTProvidesSamples ? outputBuffer.pSample : pMFOutputSample;
	if (!pResultSample)
	{
		SafeRelease(&pMFOutputSample);
		return E_FAIL;
	}

	// Get a DirectShow output sample from the downstream allocator
	IMediaSample *pOut = nullptr;
	hr = m_pOutput->GetDeliveryBuffer(&pOut, NULL, NULL, 0);
	if (FAILED(hr))
	{
		if (bMFTProvidesSamples)
			SafeRelease(&outputBuffer.pSample);
		else
			SafeRelease(&pMFOutputSample);
		return hr;
	}

	// Derive destination stride from buffer size (EVR may pad for GPU alignment)
	BOOL bOutputRGB32 = (m_outputSubtype == MEDIASUBTYPE_RGB32);
	LONG dstStride = 0;
	{
		LONG cbOut = pOut->GetSize();
		if (bOutputRGB32)
			dstStride = cbOut / (LONG)m_height;
		else
			dstStride = cbOut * 2 / ((LONG)m_height * 3);
	}

	DWORD dwOutputSize = (DWORD)pOut->GetSize();

	// Query the MFT's actual output frame size (may be aligned/padded)
	UINT32 mftWidth = m_width, mftHeight = m_height;
	{
		IMFMediaType *pOutType = nullptr;
		if (SUCCEEDED(m_pDecoderTransform->GetOutputCurrentType(0, &pOutType)))
		{
			MFGetAttributeSize(pOutType, MF_MT_FRAME_SIZE, &mftWidth, &mftHeight);
			pOutType->Release();
		}
	}

	IMFMediaBuffer *pResultBuffer = nullptr;
	hr = pResultSample->ConvertToContiguousBuffer(&pResultBuffer);
	if (SUCCEEDED(hr))
	{
		BYTE *pOutputBuf = nullptr;
		hr = pOut->GetPointer(&pOutputBuf);
		if (FAILED(hr))
		{
			SafeRelease(&pResultBuffer);
			goto done;
		}

		// Try IMF2DBuffer first (D3D11 texture or padded buffer)
		IMF2DBuffer *p2DBuffer = nullptr;
		if (SUCCEEDED(pResultBuffer->QueryInterface(__uuidof(IMF2DBuffer), (void**)&p2DBuffer)))
		{
			BYTE *pScanline0 = nullptr;
			LONG lPitch = 0;
			hr = p2DBuffer->Lock2D(&pScanline0, &lPitch);
			if (SUCCEEDED(hr))
			{
				if (bOutputRGB32)
				{
					ConvertNV12toRGB32(pScanline0, m_width, m_height,
						lPitch, pOutputBuf, dstStride);
					pOut->SetActualDataLength(dwOutputSize);
				}
				else
				{
					for (UINT row = 0; row < m_height; row++)
						memcpy(pOutputBuf + row * dstStride,
						       pScanline0 + row * lPitch,
						       m_width);
					BYTE *pDstUV = pOutputBuf + dstStride * m_height;
					const BYTE *pSrcUV = pScanline0 + lPitch * mftHeight;
					UINT uvHeight = m_height / 2;
					for (UINT row = 0; row < uvHeight; row++)
						memcpy(pDstUV + row * dstStride,
						       pSrcUV + row * lPitch,
						       m_width);
					pOut->SetActualDataLength(dwOutputSize);
				}
				p2DBuffer->Unlock2D();
			}
			SafeRelease(&p2DBuffer);
		}
		else
		{
			// Linear buffer path – use MFT's aligned height for pitch & UV offset
			BYTE *pDecoded = nullptr;
			DWORD cbDecoded = 0;
			hr = pResultBuffer->Lock(&pDecoded, nullptr, &cbDecoded);
			if (SUCCEEDED(hr))
			{
				LONG srcPitch = (LONG)(cbDecoded * 2 / (mftHeight * 3));

				if (bOutputRGB32)
				{
					ConvertNV12toRGB32(pDecoded, m_width, m_height,
						srcPitch, pOutputBuf, dstStride);
					pOut->SetActualDataLength(dwOutputSize);
				}
				else
				{
					for (UINT row = 0; row < m_height; row++)
						memcpy(pOutputBuf + row * dstStride,
						       pDecoded   + row * srcPitch,
						       m_width);
					BYTE *pDstUV = pOutputBuf + dstStride * m_height;
					const BYTE *pSrcUV = pDecoded + srcPitch * mftHeight;
					UINT uvHeight = m_height / 2;
					for (UINT row = 0; row < uvHeight; row++)
						memcpy(pDstUV + row * dstStride,
						       pSrcUV + row * srcPitch,
						       m_width);
					pOut->SetActualDataLength(dwOutputSize);
				}
				pResultBuffer->Unlock();
			}
		}
		SafeRelease(&pResultBuffer);
	}

done:
	{
		LONGLONG llSampleTime = 0, llSampleDuration = 0;
		if (SUCCEEDED(pResultSample->GetSampleTime(&llSampleTime)))
		{
			REFERENCE_TIME rtEnd2 = llSampleTime;
			if (SUCCEEDED(pResultSample->GetSampleDuration(&llSampleDuration)))
				rtEnd2 = llSampleTime + llSampleDuration;
			pOut->SetTime(&llSampleTime, &rtEnd2);
		}
		else
		{
			REFERENCE_TIME TimeStart2, TimeEnd2;
			if (SUCCEEDED(pIn->GetTime(&TimeStart2, &TimeEnd2)))
				pOut->SetTime(&TimeStart2, &TimeEnd2);
		}

		LONGLONG MediaStart, MediaEnd;
		if (SUCCEEDED(pIn->GetMediaTime(&MediaStart, &MediaEnd)))
			pOut->SetMediaTime(&MediaStart, &MediaEnd);

		pOut->SetSyncPoint(pIn->IsSyncPoint() == S_OK);
		pOut->SetPreroll(pIn->IsPreroll() == S_OK);
		pOut->SetDiscontinuity(pIn->IsDiscontinuity() == S_OK);
	}

	hr = m_pOutput->Deliver(pOut);
	pOut->Release();

	if (bMFTProvidesSamples)
		SafeRelease(&outputBuffer.pSample);
	else
		SafeRelease(&pMFOutputSample);

	return SUCCEEDED(hr) ? S_OK : hr;
}


HRESULT CPristineVideoDecoderFilter::CheckTransform(
	const CMediaType *mtIn, const CMediaType *mtOut)
{
	CheckPointer(mtIn, E_POINTER);
	CheckPointer(mtOut, E_POINTER);

	if (!IsInputSubtypeSupported(mtIn->subtype))
		return VFW_E_TYPE_NOT_ACCEPTED;

	if (mtOut->subtype != MEDIASUBTYPE_NV12 &&
		mtOut->subtype != MEDIASUBTYPE_RGB32)
		return VFW_E_TYPE_NOT_ACCEPTED;

	m_outputSubtype = mtOut->subtype;

	return NOERROR;
}

HRESULT CPristineVideoDecoderFilter::DecideBufferSize(
	IMemAllocator *pAlloc, ALLOCATOR_PROPERTIES *pProperties)
{
	if (m_pInput->IsConnected() == FALSE) return E_UNEXPECTED;

	CheckPointer(pAlloc, E_POINTER);
	CheckPointer(pProperties, E_POINTER);

	DWORD cbBuffer;
	if (m_outputSubtype == MEDIASUBTYPE_RGB32)
		cbBuffer = m_width * m_height * 4;
	else
		cbBuffer = m_width * m_height * 3 / 2;

	pProperties->cBuffers = max(pProperties->cBuffers, 8);
	pProperties->cbBuffer = cbBuffer;
	ASSERT(pProperties->cbBuffer);

	ALLOCATOR_PROPERTIES Actual;
	HRESULT hr = pAlloc->SetProperties(pProperties, &Actual);
	if (FAILED(hr)) return hr;

	if (pProperties->cBuffers > Actual.cBuffers ||
		pProperties->cbBuffer > Actual.cbBuffer)
		return E_FAIL;

	return NOERROR;
}

HRESULT CPristineVideoDecoderFilter::GetMediaType(
	int iPosition, CMediaType *pMediaType)
{
	if (iPosition < 0) return E_INVALIDARG;
	if (iPosition > 3) return VFW_S_NO_MORE_ITEMS;

	BOOL bNV12 = (iPosition < 2);
	BOOL bVIH2 = (iPosition % 2 == 0);

	const GUID &subtype = bNV12 ? MEDIASUBTYPE_NV12 : MEDIASUBTYPE_RGB32;
	WORD bitCount   = bNV12 ? 12 : 32;
	DWORD compression = bNV12 ? MAKEFOURCC('N','V','1','2') : BI_RGB;
	DWORD imageSize = bNV12
		? m_width * m_height * 3 / 2
		: m_width * m_height * 4;

	pMediaType->InitMediaType();
	pMediaType->SetType(&MEDIATYPE_Video);
	pMediaType->SetSubtype(&subtype);

	if (bVIH2)
	{
		pMediaType->SetFormatType(&FORMAT_VideoInfo2);
		VIDEOINFOHEADER2 format;
		ZeroMemory(&format, sizeof(VIDEOINFOHEADER2));
		format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		format.bmiHeader.biPlanes = 1;
		format.AvgTimePerFrame = m_avgtime;
		format.bmiHeader.biWidth = m_width;
		format.bmiHeader.biHeight = m_height;
		format.bmiHeader.biBitCount = bitCount;
		format.bmiHeader.biCompression = compression;
		format.bmiHeader.biSizeImage = imageSize;
		SetRect(&format.rcSource, 0, 0, m_width, m_height);
		SetRect(&format.rcTarget, 0, 0, m_width, m_height);
		pMediaType->SetFormat(PBYTE(&format), sizeof(VIDEOINFOHEADER2));
		pMediaType->SetSampleSize(imageSize);
	}
	else
	{
		pMediaType->SetFormatType(&FORMAT_VideoInfo);
		VIDEOINFOHEADER format;
		ZeroMemory(&format, sizeof(VIDEOINFOHEADER));
		format.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		format.bmiHeader.biPlanes = 1;
		format.AvgTimePerFrame = m_avgtime;
		format.bmiHeader.biWidth = m_width;
		format.bmiHeader.biHeight = m_height;
		format.bmiHeader.biBitCount = bitCount;
		format.bmiHeader.biCompression = compression;
		format.bmiHeader.biSizeImage = imageSize;
		pMediaType->SetFormat(PBYTE(&format), sizeof(VIDEOINFOHEADER));
		pMediaType->SetSampleSize(imageSize);
	}

	return NOERROR;
}
