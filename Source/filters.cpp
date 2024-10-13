#include "hevcdecoder.h"

CFactoryTemplate g_Templates[] = {
	L"Pristine: HEVC Decoder", &CLSID_PristineHevcDecoder, CPristineHevcDecoderFilter::CreateInstance, NULL, &hevcDecoderFilt,
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