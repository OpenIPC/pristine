#include "hevcdecoder.h"
#include "vp9decoder.h"
#include "av1decoder.h"
#include "aacdecoder.h"
#include "opusdecoder.h"

CFactoryTemplate g_Templates[] = {
	L"Pristine: HEVC Decoder", &CLSID_PristineHevcDecoder, CPristineHevcDecoderFilter::CreateInstance, NULL, &hevcDecoderFilt,
	L"Pristine: VP9 Decoder",  &CLSID_PristineVp9Decoder,  CPristineVp9DecoderFilter::CreateInstance,  NULL, &vp9DecoderFilt,
	L"Pristine: AV1 Decoder",  &CLSID_PristineAv1Decoder,  CPristineAv1DecoderFilter::CreateInstance,  NULL, &av1DecoderFilt,
	L"Pristine: AAC Decoder",  &CLSID_PristineAacDecoder,  CPristineAacDecoderFilter::CreateInstance,  NULL, &aacDecoderFilt,
	L"Pristine: Opus Decoder", &CLSID_PristineOpusDecoder, CPristineOpusDecoderFilter::CreateInstance, NULL, &opusDecoderFilt,
};
int g_cTemplates = sizeof(g_Templates) / sizeof(g_Templates[0]);

STDAPI DllRegisterServer()
{
	return AMovieDllRegisterServer2(TRUE);
}

STDAPI DllUnregisterServer()
{
	return AMovieDllRegisterServer2(FALSE);
}

extern "C" BOOL WINAPI DllEntryPoint(HINSTANCE, ULONG, LPVOID);

BOOL APIENTRY DllMain(HANDLE hModule, DWORD dwReason, LPVOID lpReserved)
{
	return DllEntryPoint((HINSTANCE)(hModule), dwReason, lpReserved);
}