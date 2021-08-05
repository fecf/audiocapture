#include <atomic>
#include <functional>
#include <cassert>
#include <cmath>
#include <thread>
#include <iostream>

#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl.h>
#include <Functiondiscoverykeys_devpkey.h>

using namespace Microsoft::WRL;

void PlayerThread(std::atomic_bool& exit) {
	HRESULT hr;
	hr = ::CoInitializeEx(NULL, COINIT_MULTITHREADED);
	assert(SUCCEEDED(hr));

	ComPtr<IMMDeviceEnumerator> device_enumerator;
	hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), &device_enumerator);
	assert(SUCCEEDED(hr));

	ComPtr<IMMDevice> audio_device;
	hr = device_enumerator->GetDefaultAudioEndpoint(EDataFlow::eRender, ERole::eConsole, &audio_device);
	assert(SUCCEEDED(hr));

	ComPtr<IPropertyStore> ps;
	audio_device->OpenPropertyStore(STGM_READ, &ps);
	PROPVARIANT vname;
	::PropVariantInit(&vname);
	hr = ps->GetValue(PKEY_Device_FriendlyName, &vname);
	assert(SUCCEEDED(hr));
	std::wcout << L"device name: " << vname.pwszVal << std::endl;
	hr = ::PropVariantClear(&vname);
	assert(SUCCEEDED(hr));

	ComPtr<IAudioClient3> audio_client;
	hr = audio_device->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, NULL, &audio_client);
	assert(SUCCEEDED(hr));

	WAVEFORMATEXTENSIBLE wfx{};
	wfx.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
	wfx.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
	wfx.Format.nChannels = 2;
	wfx.Format.nSamplesPerSec = 48000;
	wfx.Format.wBitsPerSample = 32;
	wfx.Format.nBlockAlign = wfx.Format.nChannels * wfx.Format.wBitsPerSample / 8;
	wfx.Format.nAvgBytesPerSec = wfx.Format.nSamplesPerSec * wfx.Format.nBlockAlign;
	wfx.Samples.wValidBitsPerSample = wfx.Format.wBitsPerSample;
	wfx.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
	wfx.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;

	AUDCLNT_SHAREMODE share_mode = AUDCLNT_SHAREMODE_SHARED;
	DWORD stream_flags = AUDCLNT_STREAMFLAGS_NOPERSIST | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
	REFERENCE_TIME buffer_duration = 0;
	REFERENCE_TIME hns_periodicity = 0;

	WAVEFORMATEX* matched_wfx{};
	hr = audio_client->IsFormatSupported(share_mode, (WAVEFORMATEX*)&wfx, &matched_wfx);
	assert(SUCCEEDED(hr));

	hr = audio_client->Initialize(share_mode, stream_flags, buffer_duration, hns_periodicity, (WAVEFORMATEX*)&wfx, NULL);
	assert(SUCCEEDED(hr));
	WAVEFORMATEXTENSIBLE* wfex = (WAVEFORMATEXTENSIBLE*)matched_wfx;

	HANDLE event = ::CreateEvent(NULL, FALSE, FALSE, NULL);
	assert(event != NULL);

	hr = audio_client->SetEventHandle(event);
	assert(SUCCEEDED(hr));

	ComPtr<IAudioRenderClient> audio_render_client;
	hr = audio_client->GetService(__uuidof(IAudioRenderClient), &audio_render_client);
	assert(SUCCEEDED(hr));

	UINT32 buffer_frames;
	hr = audio_client->GetBufferSize(&buffer_frames);
	assert(SUCCEEDED(hr));

	void* data = NULL;
	hr = audio_render_client->GetBuffer(buffer_frames, (BYTE**)&data);
	assert(SUCCEEDED(hr));
	memset(data, 0, buffer_frames * wfx.Format.nBlockAlign);

	hr = audio_render_client->ReleaseBuffer(buffer_frames, 0);
	assert(SUCCEEDED(hr));

	hr = audio_client->Start();
	assert(SUCCEEDED(hr));

	constexpr double M_PI_2 = 2.0 * 3.14159265358979323846;
	const double tone = 440.0;
	const double vol = INT32_MAX * 0.25;
	double time = 0;
	while (!exit) {
		DWORD ret = ::WaitForSingleObject(event, INFINITE);
		if (ret != WAIT_OBJECT_0) {
			break;
		}

		UINT32 padding_frames;
		hr = audio_client->GetCurrentPadding(&padding_frames);

		UINT32 frames = buffer_frames - padding_frames;
		if (frames > 0) {
			void* data = NULL;

			hr = audio_render_client->GetBuffer(frames, (BYTE**)&data);
			assert(SUCCEEDED(hr));
			assert(data != NULL);

			for (UINT32 i = 0; i < frames; ++i) {
				double v = sin(time * M_PI_2 * tone);
				*((int32_t*)data + i * 2 + 0) = (int32_t)(v * vol);
				*((int32_t*)data + i * 2 + 1) = (int32_t)(v * vol);
				time += 1.0 / wfx.Format.nSamplesPerSec;
			}

			hr = audio_render_client->ReleaseBuffer(frames, 0);
			assert(SUCCEEDED(hr));
		}
	}

	hr = audio_client->Stop();
	assert(SUCCEEDED(hr));
}

int main()
{
  ::Sleep(3000);

	HRESULT hr;
	hr = ::CoInitialize(NULL);
	assert(SUCCEEDED(hr));

	std::atomic_bool exit = false;
	std::thread th(PlayerThread, std::ref(exit));
	th.join();

	::CoUninitialize();
	assert(SUCCEEDED(hr));
	return 0;
}

