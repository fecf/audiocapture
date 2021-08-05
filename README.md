# AudioCapture
AudioCapture is a software that captures audio of only the specified process. 
Only DirectSound and WASAPI are supported.

- core/inject: DLL to be injected
- core/injector: CLI application
- obs-audiocapture: OBS plugin (WIP)

### Usage (CLI)
injector_x64.exe -p target_process.exe -s save_captured_data.wav
