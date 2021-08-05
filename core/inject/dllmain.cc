#include <array>
#include <atomic>
#include <cassert>
#include <string>

#include <wrl.h>
using namespace Microsoft::WRL;
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

#include <dsound.h>
#pragma comment(lib, "dsound.lib")

#include "detours/detours.h"

#include "loguru.hpp"

#include "inject.h"

constexpr size_t kPipeSize = 1024 * 1024;

class Inject {
public:
  static Inject& GetInstance() {
    static Inject instance;
    return instance;
  }

  // Worker thread
  HANDLE thread = NULL;
  std::atomic_bool threadExit = false;

  // WASAPI
  IAudioClient* wasapiAudioClient = NULL;
  WAVEFORMATEX* wasapiFormat = NULL;
  uint8_t* wasapiBuffer = NULL;

public:
  void Initialize() {
    DWORD pid = ::GetProcessIdOfThread(::GetCurrentThread());
    std::string pipename = "\\\\.\\pipe\\audiocapture_" + std::to_string(pid);
    pipe_ = ::CreateNamedPipeA(
        pipename.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, 1, kPipeSize,
        1024 * 1024, NMPWAIT_USE_DEFAULT_WAIT, NULL);

    if (pipe_ == INVALID_HANDLE_VALUE) {
      DLOG_F(ERROR, "failed CreateNamedPipe().");
    }
  }

  void Finalize() { 
    ::CloseHandle(pipe_);
  }

  void writeCaptureData(uint8_t* data, size_t size, int channels, int samples,
                        int bitspersample, int samplespersec) {
    static std::array<uint8_t, kPipeSize> buf;

    // Magic number
    buf[0] = 0xFE;
    buf[1] = 0xCF;

    // Header
    Header header;
    header.header_offset = 2;
    header.header_size = sizeof(Header);
    header.data_offset = header.header_offset + header.header_size;
    header.data_size = size;
    header.total_size =
        header.header_offset + header.header_size + header.data_size;
    header.channels = channels;
    header.samples = samples;
    header.bits_per_sample = bitspersample;
    header.sampling_rate = samplespersec;
    ::memcpy(buf.data() + header.header_offset, &header, header.header_size);

    ::memcpy(buf.data() + header.data_offset, data, size);

    // Write to the pipe
    BOOL ret;
    DWORD num_bytes_written = 0;
    ret = ::WriteFile(pipe_, buf.data(), header.total_size, &num_bytes_written, NULL);
    if (ret == FALSE) {
      DWORD err = ::GetLastError();
      DLOG_F(ERROR, "failed WriteFile(). GetLastError() = %u.", err);
    }

#if 1
    DWORD avail = 0;
    ret = ::PeekNamedPipe(pipe_, NULL, 0, NULL, &avail, NULL);
    if (ret == FALSE) {
      DWORD err = ::GetLastError();
      DLOG_F(ERROR, "failed PeekNamedPipe(). GetLastError() = %u.", err);
    }
    DLOG_F(INFO, "writeCaptureData: %u of %u bytes written. pipe has %u bytes.", num_bytes_written, header.total_size, avail);
#endif
  }

private:
  HANDLE pipe_;
};

HRESULT (__stdcall *RealGetDefaultAudioEndPoint)(IMMDeviceEnumerator* self, EDataFlow df, ERole r, IMMDevice* d) = NULL;
HRESULT __stdcall HookGetDefaultAudioEndPoint(IMMDeviceEnumerator* self, EDataFlow df, ERole r, IMMDevice* d) {
  DLOG_F(INFO, "HookGetDefaultAudioEndPoint");
  HRESULT ret = RealGetDefaultAudioEndPoint(self, df, r, d);
  return ret;
}

HRESULT (__stdcall *RealGetCurrentPadding)(IAudioClient* self, UINT32* padding) = NULL;
HRESULT __stdcall HookGetCurrentPadding(IAudioClient* self, UINT32* padding) {
  DLOG_F(INFO, "HookGetCurrentPadding");
  HRESULT ret = RealGetCurrentPadding(self, padding);

  Inject& instance = Inject::GetInstance();
  instance.wasapiAudioClient = self;

  return ret;
}

HRESULT (__stdcall *RealGetBuffer)(IAudioRenderClient* self, UINT32 frames, BYTE** data) = NULL;
HRESULT __stdcall HookGetBuffer(IAudioRenderClient* self, UINT32 frames, BYTE** data) {
  DLOG_F(INFO, "HookGetBuffer");
  HRESULT ret = RealGetBuffer(self, frames, data);

  Inject& instance = Inject::GetInstance();
  instance.wasapiBuffer = *data;
  return ret;
}

HRESULT (__stdcall *RealReleaseBuffer)(IAudioRenderClient* self, UINT32 framesWritten, DWORD flags) = NULL;
HRESULT __stdcall HookReleaseBuffer(IAudioRenderClient* self, UINT32 framesWritten, DWORD flags) {
  Inject& instance = Inject::GetInstance();

  instance.wasapiAudioClient->GetMixFormat(&instance.wasapiFormat);
  int channels = instance.wasapiFormat->nChannels;
  int framesize = instance.wasapiFormat->nBlockAlign;
  int bitspersample = instance.wasapiFormat->wBitsPerSample;
  int samplespersec = instance.wasapiFormat->nSamplesPerSec;
  int size = framesWritten * instance.wasapiFormat->nBlockAlign;
  int samples = framesWritten;
  if (instance.wasapiFormat->nBlockAlign !=
      instance.wasapiFormat->nChannels * (instance.wasapiFormat->wBitsPerSample / 8)) {
    assert(false && "not aligned.");
  }
  instance.writeCaptureData((uint8_t*)instance.wasapiBuffer, size, channels, samples, bitspersample, samplespersec);

  HRESULT ret = RealReleaseBuffer(self, framesWritten, flags);
  return ret;
}

HRESULT(__stdcall* RealDirectSoundLock)
(IDirectSoundBuffer* self, DWORD dwOffset, DWORD dwBytes, LPVOID* ppvAudioPtr1,
 LPDWORD pdwAudioBytes1, LPVOID* ppvAudioPtr2, LPDWORD pdwAudioBytes2,
 DWORD dwFlags) = NULL;
HRESULT __stdcall HookDirectSoundLock(IDirectSoundBuffer* self, DWORD dwOffset,
                                      DWORD dwBytes, LPVOID* ppvAudioPtr1,
                                      LPDWORD pdwAudioBytes1,
                                      LPVOID* ppvAudioPtr2,
                                      LPDWORD pdwAudioBytes2, DWORD dwFlags) {
  DLOG_F(INFO, "HookDirectSoundLock");

  HRESULT ret =
      RealDirectSoundLock(self, dwOffset, dwBytes, ppvAudioPtr1, pdwAudioBytes1,
                          ppvAudioPtr2, pdwAudioBytes2, dwFlags);
  return ret;
}

HRESULT(__stdcall* RealDirectSoundUnlock)
(IDirectSoundBuffer* self, LPVOID pvAudioPtr1, DWORD dwAudioBytes1,
 LPVOID pvAudioPtr2, DWORD dwAudioBytes2) = NULL;
HRESULT __stdcall HookDirectSoundUnlock(IDirectSoundBuffer* self,
                                        LPVOID ppvAudioPtr1,
                                        DWORD pdwAudioBytes1,
                                        LPVOID ppvAudioPtr2,
                                        DWORD pdwAudioBytes2) {
  DLOG_F(INFO, "HookDirectSoundUnlock");

  Inject& instance = Inject::GetInstance();
  static WAVEFORMATEX wfex{};
  wfex.cbSize = sizeof(WAVEFORMATEX);
  self->GetFormat(&wfex, sizeof(WAVEFORMATEX), NULL);

  DLOG_F(INFO, "ppvAudioBytes1=%u", pdwAudioBytes1);
  DLOG_F(INFO, "ppvAudioBytes2=%u", pdwAudioBytes2);

  int samples = pdwAudioBytes1 / wfex.nBlockAlign;
  instance.writeCaptureData((uint8_t*)ppvAudioPtr1, pdwAudioBytes1,
                            wfex.nChannels, samples,
                            wfex.wBitsPerSample, wfex.nSamplesPerSec);

  HRESULT ret = RealDirectSoundUnlock(self, ppvAudioPtr1, pdwAudioBytes1,
                                      ppvAudioPtr2, pdwAudioBytes2);
  return ret;
}

void* getVTableFunction(void* instance, int offset) {
  void** vtable = (LPVOID*)*(void**)instance;
  void* addr = *(vtable + offset);
  return addr;
}

void hookWASAPI() {
  // Get vtable of IAudioClient, IAudioRenderClient

	HRESULT hr;
	hr = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
	assert(SUCCEEDED(hr));

	ComPtr<IMMDeviceEnumerator> device_enumerator;
	hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), &device_enumerator);
	assert(SUCCEEDED(hr));

	ComPtr<IMMDevice> audio_device;
	hr = device_enumerator->GetDefaultAudioEndpoint(EDataFlow::eRender, ERole::eConsole, &audio_device);
	assert(SUCCEEDED(hr));

	ComPtr<IAudioClient> audio_client;
	hr = audio_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, &audio_client);
	assert(SUCCEEDED(hr));

  WAVEFORMATEX* fmt;
  hr = audio_client->GetMixFormat(&fmt);
	assert(SUCCEEDED(hr));

  hr = audio_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                AUDCLNT_STREAMFLAGS_NOPERSIST, 0, 0, fmt, NULL);
  assert(SUCCEEDED(hr));

	ComPtr<IAudioRenderClient> audio_render_client;
	hr = audio_client->GetService(__uuidof(IAudioRenderClient), &audio_render_client);
	assert(SUCCEEDED(hr));

  RealGetDefaultAudioEndPoint = (decltype(RealGetDefaultAudioEndPoint))(getVTableFunction(device_enumerator.Get(), 4));
  RealGetCurrentPadding = decltype(RealGetCurrentPadding)(getVTableFunction(audio_client.Get(), 6));
  RealGetBuffer = decltype(RealGetBuffer)(getVTableFunction(audio_render_client.Get(), 3));
  RealReleaseBuffer = decltype(RealReleaseBuffer)(getVTableFunction(audio_render_client.Get(), 4));

  DetourTransactionBegin();
  DetourUpdateThread(::GetCurrentThread());
  DetourAttach(&(PVOID&)RealGetDefaultAudioEndPoint, HookGetDefaultAudioEndPoint);
  DetourAttach(&(PVOID&)RealGetCurrentPadding, HookGetCurrentPadding);
  DetourAttach(&(PVOID&)RealGetBuffer, HookGetBuffer);
  DetourAttach(&(PVOID&)RealReleaseBuffer, HookReleaseBuffer);
  DetourTransactionCommit();
}

void unhookWASAPI() {
  DetourTransactionBegin();
  DetourUpdateThread(::GetCurrentThread());
  DetourDetach(&(PVOID&)RealGetDefaultAudioEndPoint, HookGetDefaultAudioEndPoint);
  DetourDetach(&(PVOID&)RealGetCurrentPadding, HookGetCurrentPadding);
  DetourDetach(&(PVOID&)RealGetBuffer, HookGetBuffer);
  DetourDetach(&(PVOID&)RealReleaseBuffer, HookReleaseBuffer);
  DetourTransactionCommit();
}

void hookDSound() { 
  HRESULT hr;
  ComPtr<IDirectSound8> ds;
  hr = ::DirectSoundCreate8(NULL, &ds, NULL); 
  assert(SUCCEEDED(hr));

  DSBUFFERDESC dsbd{};
  dsbd.dwSize = sizeof(DSBUFFERDESC);
  dsbd.dwFlags = DSBCAPS_GLOBALFOCUS | DSBCAPS_CTRLPOSITIONNOTIFY |
                 DSBCAPS_GETCURRENTPOSITION2;
  dsbd.dwBufferBytes = 4 * 192000;
  dsbd.lpwfxFormat = (WAVEFORMATEX*)malloc(sizeof(WAVEFORMATEX));
  dsbd.lpwfxFormat->wFormatTag = WAVE_FORMAT_PCM;
  (dsbd.lpwfxFormat)->nChannels = 2;
  (dsbd.lpwfxFormat)->nSamplesPerSec = 44100;
  (dsbd.lpwfxFormat)->nAvgBytesPerSec = 44100 * (16 / 8) * 2;
  (dsbd.lpwfxFormat)->nBlockAlign = (16 / 8) * 2;
  (dsbd.lpwfxFormat)->wBitsPerSample = 16;
  (dsbd.lpwfxFormat)->cbSize = 0;  

  ComPtr<IDirectSoundBuffer> buffer;
  hr = ds->CreateSoundBuffer(&dsbd, &buffer, NULL);
  assert(SUCCEEDED(hr));

  RealDirectSoundLock = (decltype(RealDirectSoundLock))(getVTableFunction(buffer.Get(), 11));
  RealDirectSoundUnlock = (decltype(RealDirectSoundUnlock))(getVTableFunction(buffer.Get(), 19));

  DetourTransactionBegin();
  DetourUpdateThread(::GetCurrentThread());
  DetourAttach(&(PVOID&)RealDirectSoundLock, HookDirectSoundLock);
  DetourAttach(&(PVOID&)RealDirectSoundUnlock, HookDirectSoundUnlock);
  DetourTransactionCommit();
}

void unhookDSound() {
  DetourTransactionBegin();
  DetourUpdateThread(::GetCurrentThread());
  DetourDetach(&(PVOID&)RealDirectSoundLock, HookDirectSoundLock);
  DetourDetach(&(PVOID&)RealDirectSoundUnlock, HookDirectSoundUnlock);
  DetourTransactionCommit();
}

void installHook() {
  hookWASAPI();
  hookDSound();
}

void uninstallHook() {
  unhookWASAPI();
  unhookDSound();
}

DWORD WINAPI thread(LPVOID lpParam) {
#ifdef _DEBUG
  loguru::add_file("audiocapture.debug.log", loguru::FileMode::Append, loguru::Verbosity_MAX);
#endif

  HRESULT hr;
  hr = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
  assert(SUCCEEDED(hr));

  Inject& instance = Inject::GetInstance();
  instance.Initialize();

  installHook();

  std::atomic_bool* exit = (std::atomic_bool*)lpParam;
  while (!(*exit)) {
    ::Sleep(10);
  }

  uninstallHook();

  instance.Finalize();

  ::CoUninitialize();

  return 0;
}

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
  Inject& instance = Inject::GetInstance();

  switch (ul_reason_for_call) {
    case DLL_PROCESS_ATTACH:
      ::DisableThreadLibraryCalls(hModule);
      instance.threadExit = false;
      instance.thread = ::CreateThread(NULL, 0, thread, &instance.threadExit, 0, NULL);
      break;
    case DLL_THREAD_ATTACH:
      break;
    case DLL_THREAD_DETACH:
      break;
    case DLL_PROCESS_DETACH:
      instance.threadExit = true;
      ::WaitForSingleObject(instance.thread, INFINITE);
      break;
    default:
      break;
  }
  return TRUE;
}
