#include <array>
#include <cassert>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#define NOMINMAX
#include <windows.h>
#include <psapi.h>

#define DR_WAV_IMPLEMENTATION
#include "../inject/inject.h"
#include "CLI11.hpp"
#include "dr_wav.h"
#include "loguru.hpp"

int ActivateSeDebugPrivilege(void) {
  HANDLE hToken;
  BOOL ret;
  ret = ::OpenProcessToken(::GetCurrentProcess(),
                           TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken);
  if (ret == FALSE) {
    return ::GetLastError();
  }

  LUID Val;
  ret = ::LookupPrivilegeValue(NULL, SE_DEBUG_NAME, &Val);
  if (ret == FALSE) {
    return ::GetLastError();
  }

  TOKEN_PRIVILEGES tp;
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Luid = Val;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  ret = ::AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(tp), NULL, NULL);
  if (ret == FALSE) {
    return ::GetLastError();
  }

  ret = ::CloseHandle(hToken);
  if (ret == FALSE) {
    return ::GetLastError();
  }

  return 0;
}

int main(int argc, char** argv) {
  CLI::App app{"injector"};
  bool use_32bit_dll;
  std::string record_wav_path;
  std::string target_process_path;
  app.add_flag("--x86", use_32bit_dll, "use 32-bit dll")->default_val(false);
  app.add_option("-p,--process", target_process_path,
                 "target process path (partial match)")
      ->required();
  app.add_option("-s,--save", record_wav_path, "save to .wav file");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return app.exit(e);
  }

  LOG_F(INFO, "Injector started. x86(%s) process(%s) wav(%s)",
        (use_32bit_dll ? "true" : "false"), target_process_path.c_str(),
        record_wav_path.c_str());

  bool injected = false;
  DWORD injected_pid = 0;

  char temp[1024]{};
  ::GetModuleFileNameA(NULL, temp, 1024);
  std::string cwd = std::string(temp).substr(0, std::string(temp).rfind("\\"));

  std::filesystem::path path(cwd + "\\inject" +
                             (use_32bit_dll ? "_x86" : "_x64") + ".dll");
  if (!std::filesystem::exists(path)) {
    DLOG_F(ERROR, "Can't find inject dll.");
    return 1;
  }
  std::string fullpath = std::filesystem::absolute(path).string();

  {
    int ret = ActivateSeDebugPrivilege();
    if (ret != 0) {
      DLOG_F(ERROR, "failed ActivateSeDebugPrivilege().");
      return 1;
    }
  }

  std::vector<DWORD> processes(65535, NULL);
  while (!injected) {
    DWORD needed = 0;
    BOOL ret;
    ret = ::EnumProcesses(processes.data(), processes.size() * sizeof(DWORD),
                          &needed);
    if (ret == FALSE) {
      DLOG_F(ERROR, "failed EnumProcesses().");
      return 1;
    }

    int process_count = needed / sizeof(DWORD);
    for (int i = 0; i < process_count; ++i) {
      int pid = processes[i];
      if (pid == 0) {
        continue;
      }

      HANDLE handle = ::OpenProcess(
          PROCESS_QUERY_INFORMATION | PROCESS_CREATE_THREAD |
              PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ,
          FALSE, pid);
      if (handle == NULL) {
        continue;
      }

      char str[MAX_PATH];
      DWORD len = ::GetModuleFileNameExA(handle, NULL, str, MAX_PATH);
      if (len == 0) {
        continue;
      }

      std::filesystem::path path(str);
      std::string filename = path.filename().string();
      if (filename.find(target_process_path) == std::string::npos) {
        ::CloseHandle(handle);
        continue;
      }

      DLOG_F(INFO, "found pid(%d) path(%s)", pid, str);
      size_t size = fullpath.size() + 1;
      LPVOID ptr =
          ::VirtualAllocEx(handle, NULL, size, MEM_COMMIT, PAGE_READWRITE);
      if (ptr == NULL) {
        DLOG_F(ERROR, "failed VirtualAllocEx().");
        return 1;
      }

      SIZE_T written = 0;
      BOOL ret =
          ::WriteProcessMemory(handle, ptr, fullpath.data(), size, &written);
      if (ret == FALSE || written != size) {
        DLOG_F(ERROR, "failed WriteProcessMemory().");
        return 1;
      }

      HANDLE remote_thread_handle = ::CreateRemoteThread(
          handle, NULL, 0, (LPTHREAD_START_ROUTINE)LoadLibraryA, ptr, 0, NULL);
      if (remote_thread_handle == NULL) {
        DLOG_F(ERROR, "failed CreateRemoteThread().");
        return 1;
      }

      ::CloseHandle(handle);

      injected = true;
      injected_pid = processes[i];
      DLOG_F(INFO, "Injected to pid(%d).", injected_pid);
      break;
    }

    if (injected) {
      break;
    }

    DLOG_F(WARNING, "Can't find target process. sleeping 3 sec ...");
    ::Sleep(3000);
  }

  if (record_wav_path.empty()) {
    // No need to consume data from the named pipe.
    return 0;
  }

  // Connect to pipe
  std::string pipename =
      "\\\\.\\pipe\\audiocapture_" + std::to_string(injected_pid);
  HANDLE hPipe = INVALID_HANDLE_VALUE;
  while (hPipe == INVALID_HANDLE_VALUE) {
    DLOG_F(INFO, "Connecting to named pipe(%s) ...", pipename.c_str());
    hPipe = ::CreateFileA(pipename.c_str(), GENERIC_READ, 0, NULL,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hPipe == INVALID_HANDLE_VALUE) {
      DLOG_F(WARNING,
             "error: Can't connect to the named pipe. sleeping 3 sec ...");
      ::Sleep(3000);
      continue;
    }
  }
  DLOG_F(INFO, "Connected.");

  std::vector<uint8_t> buf;
  size_t pipesize = 1024 * 1024;
  buf.resize(pipesize);

  std::vector<uint8_t> audiodata;

  drwav_data_format df;
  df.container = drwav_container_riff;
  df.format = DR_WAVE_FORMAT_PCM;
  int samples = 0;

  time_t rawtime;
  std::time(&rawtime);
  char tb[256];
  std::tm ti;
  localtime_s(&ti, &rawtime);
  std::strftime(tb, sizeof(tb), "%Y%m%d_%H%M%S", &ti);
  std::string filename = "record_" + std::string(tb) + ".wav";

  while (true) {
    DWORD read_bytes = 0;
    DWORD avail_bytes = 0;
    BOOL ret;
    ret = ::PeekNamedPipe(hPipe, buf.data(), pipesize, &read_bytes,
                          &avail_bytes, NULL);
    if (!ret) {
      DLOG_F(WARNING, "failed PeekNamedPipe().");
      break;
    }

    if (read_bytes < (2 + sizeof(Header))) {
      continue;
    }

    Header* h = (Header*)(buf.data() + 2);
    if ((int)read_bytes < h->total_size) {
      continue;
    }

    if (buf[0] != 0xfe || buf[1] != 0xcf) {
      DLOG_F(ERROR, "unexpected data.");
      return 1;
    }

    uint8_t* ptr = buf.data() + h->data_offset;
    size_t offset = audiodata.size();
    audiodata.resize(audiodata.size() + h->data_size);
    memcpy(audiodata.data() + offset, ptr, h->data_size);

    df.channels = h->channels;
    df.sampleRate = h->sampling_rate;
    df.bitsPerSample = h->bits_per_sample;
    samples += h->samples;

    ret = ::ReadFile(hPipe, buf.data(), read_bytes, &read_bytes, NULL);
    if (!ret) {
      DLOG_F(ERROR, "failed ReadFile().");
      return 1;
    }
  }

  DLOG_F("The named pipe is closed.");

  drwav wav;
  if (!drwav_init_file_write(&wav, filename.c_str(), &df, NULL)) {
    DLOG_F(ERROR, "failed drwav_init_file_write().");
    return 1;
  }

  drwav_uint64 written =
      drwav_write_pcm_frames(&wav, samples, audiodata.data());
  DLOG_F(INFO, "Saved to %s (%d bytes).", record_wav_path.c_str(), written);
  drwav_uninit(&wav);

  return 0;
}
