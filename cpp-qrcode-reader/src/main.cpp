#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <bcrypt.h>
#include <mmsystem.h>

#include <opencv2/core.hpp>
#include <opencv2/highgui.hpp>
#include <opencv2/imgproc.hpp>
#include <opencv2/objdetect.hpp>
#include <opencv2/videoio.hpp>

#include <ZXing/Barcode.h>
#include <ZXing/BarcodeFormat.h>
#include <ZXing/ImageView.h>
#include <ZXing/ReadBarcode.h>
#include <ZXing/ReaderOptions.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "winmm.lib")

namespace fs = std::filesystem;
using json = nlohmann::json;
using Clock = std::chrono::steady_clock;

namespace {

constexpr const char* kAppName = "qrcode-reader";
constexpr const char* kDefaultIncomingDir = "qrcode-reader.incoming";
constexpr const char* kConfigFileName = "config.json";

constexpr std::string_view kProtocolMagic = "AIRQR1";
constexpr int kProtocolVersion = 1;
constexpr const char* kDefaultEsp32Host = "192.168.1.136";
constexpr int kDefaultEsp32Port = 9000;
constexpr int kSpaceTypeSettleMs = 80;
constexpr int kMinCameraWidth = 800;
constexpr int kMinCameraHeight = 450;
constexpr int kHeavyPassInterval = 12;
constexpr int kHeavyPassMissThreshold = 6;
constexpr double kStatusPollIntervalSeconds = 0.25;
constexpr double kRepeatDelaySeconds = 2.0;
constexpr double kResendSameFrameSeconds = 2.0;
constexpr double kNavigationCooldownSeconds = 0.35;
constexpr int kMaxFrameGrabs = 1;
constexpr int kBorderDetectMaxWidth = 320;
constexpr int kBorderLockFrames = 16;

const cv::Scalar kCyanLow(78, 110, 110);
const cv::Scalar kCyanHigh(102, 255, 255);

std::atomic_bool g_stopRequested = false;

struct Config {
  int cameraIndex = 1;
  fs::path outputDir = kDefaultIncomingDir;
  bool unique = false;
  std::string esp32Host = kDefaultEsp32Host;
  int esp32Port = kDefaultEsp32Port;
  double connectTimeoutSeconds = 2.0;
};

struct TransferBuffer {
  std::string sessionId;
  std::string fileId;
  std::string relativePath;
  std::string encoding = "base64";
  int expectedChunks = 0;
  std::string payloadSha256;
  std::string zipSha256;
  std::map<int, std::string> chunks;
};

struct SessionState {
  int expectedFiles = 0;
  std::set<std::string> startedFiles;
  std::set<std::string> completedFiles;
  bool sessionEndSeen = false;
  int sessionEndRepeatTotal = 0;
  std::set<int> sessionEndRepeatsSeen;
};

struct DecodedPayload {
  std::string payload;
  std::vector<int> points;
};

struct ProcessResult {
  std::optional<std::string> message;
  bool sessionComplete = false;
  bool ackAdvance = true;
};

struct SessionGate {
  bool primed = false;
  std::string sessionId;
};

struct BorderState {
  cv::Rect roi;
  std::vector<int> box;
  int framesSinceSeen = kBorderLockFrames + 1;
  bool locked = false;
};

struct LatestFrame {
  std::mutex mutex;
  cv::Mat frame;
  std::uint64_t sequence = 0;
};

std::string toLower(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

fs::path getExecutablePath() {
  std::array<wchar_t, 32768> buffer {};
  DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                    static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) {
    throw std::runtime_error("Failed to get executable path");
  }
  return fs::path(std::wstring(buffer.data(), length));
}

fs::path resolveConfigPath(const fs::path& executableDir) {
  return executableDir / kConfigFileName;
}

json makeDefaultConfigJson() {
  return json{
      {"camera_index", 1},
      {"incoming_dir", kDefaultIncomingDir},
      {"esp32_host", kDefaultEsp32Host},
      {"esp32_port", kDefaultEsp32Port},
      {"connect_timeout_seconds", 2.0},
      {"unique", false},
  };
}

void writeDefaultConfigFile(const fs::path& configPath) {
  fs::create_directories(configPath.parent_path());
  std::ofstream out(configPath, std::ios::binary);
  if (!out) {
    throw std::runtime_error("Failed to create config file: " + configPath.string());
  }
  out << makeDefaultConfigJson().dump(2) << "\n";
}

Config loadConfigFromExecutableDir(const fs::path& executableDir) {
  const fs::path configPath = resolveConfigPath(executableDir);
  if (!fs::exists(configPath)) {
    writeDefaultConfigFile(configPath);
  }

  std::ifstream in(configPath, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Failed to open config file: " + configPath.string());
  }

  json root = json::parse(in, nullptr, true, true);
  if (!root.is_object()) {
    throw std::runtime_error("Config file must contain a JSON object");
  }

  Config config;
  config.cameraIndex = root.value("camera_index", config.cameraIndex);
  config.unique = root.value("unique", config.unique);
  config.esp32Host = root.value("esp32_host", config.esp32Host);
  config.esp32Port = root.value("esp32_port", config.esp32Port);
  config.connectTimeoutSeconds =
      root.value("connect_timeout_seconds", config.connectTimeoutSeconds);

  fs::path incomingDir = fs::path(kDefaultIncomingDir);
  if (root.contains("output_dir") && root["output_dir"].is_string()) {
    incomingDir = fs::path(root["output_dir"].get<std::string>());
  } else if (root.contains("incoming_dir") && root["incoming_dir"].is_string()) {
    incomingDir = fs::path(root["incoming_dir"].get<std::string>());
  }
  if (incomingDir.is_relative()) {
    incomingDir = executableDir / incomingDir;
  }
  config.outputDir = incomingDir.lexically_normal();
  return config;
}

void beep() {
  struct TonePlayer {
    HWAVEOUT device = nullptr;
    WAVEFORMATEX format {};

    TonePlayer() {
      format.wFormatTag = WAVE_FORMAT_PCM;
      format.nChannels = 1;
      format.nSamplesPerSec = 22050;
      format.wBitsPerSample = 16;
      format.nBlockAlign =
          static_cast<WORD>((format.nChannels * format.wBitsPerSample) / 8);
      format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
      format.cbSize = 0;
    }

    ~TonePlayer() {
      close();
    }

    void close() {
      if (device != nullptr) {
        waveOutReset(device);
        waveOutClose(device);
        device = nullptr;
      }
    }

    bool ensureOpen() {
      if (device != nullptr) {
        return true;
      }
      const MMRESULT result = waveOutOpen(&device, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL);
      if (result != MMSYSERR_NOERROR) {
        device = nullptr;
        return false;
      }
      return true;
    }

    bool play(const std::vector<std::uint8_t>& pcmBytes) {
      if (!ensureOpen()) {
        return false;
      }

      WAVEHDR header {};
      header.lpData = reinterpret_cast<LPSTR>(const_cast<std::uint8_t*>(pcmBytes.data()));
      header.dwBufferLength = static_cast<DWORD>(pcmBytes.size());
      MMRESULT result = waveOutPrepareHeader(device, &header, sizeof(header));
      if (result != MMSYSERR_NOERROR) {
        close();
        return false;
      }

      result = waveOutWrite(device, &header, sizeof(header));
      if (result != MMSYSERR_NOERROR) {
        waveOutUnprepareHeader(device, &header, sizeof(header));
        close();
        return false;
      }

      const auto deadline = Clock::now() + std::chrono::milliseconds(1000);
      while ((header.dwFlags & WHDR_DONE) == 0 && Clock::now() < deadline) {
        Sleep(5);
      }

      if ((header.dwFlags & WHDR_DONE) == 0) {
        waveOutReset(device);
        waveOutUnprepareHeader(device, &header, sizeof(header));
        close();
        return false;
      }

      result = waveOutUnprepareHeader(device, &header, sizeof(header));
      if (result != MMSYSERR_NOERROR) {
        close();
        return false;
      }
      return true;
    }
  };

  auto appendLe16 = [](std::vector<std::uint8_t>& bytes, std::uint16_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xFF));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
  };
  auto appendTone = [](std::vector<std::int16_t>& samples,
                       int sampleRate,
                       double frequencyHz,
                       int durationMs,
                       int amplitude) {
    const int sampleCount = std::max(1, (sampleRate * durationMs) / 1000);
    const double twoPi = 6.28318530717958647692;
    for (int i = 0; i < sampleCount; ++i) {
      double phase = (static_cast<double>(i) * frequencyHz * twoPi) / sampleRate;
      double envelope = 1.0;
      if (sampleCount > 16) {
        const int fadeSamples = std::min(sampleCount / 6, sampleRate / 200);
        if (fadeSamples > 0) {
          if (i < fadeSamples) {
            envelope = static_cast<double>(i) / fadeSamples;
          } else if (i > sampleCount - fadeSamples) {
            envelope = static_cast<double>(sampleCount - i) / fadeSamples;
          }
        }
      }
      samples.push_back(static_cast<std::int16_t>(
          std::sin(phase) * amplitude * std::clamp(envelope, 0.0, 1.0)));
    }
  };

  static const std::vector<std::uint8_t> pcmBytes = [&]() {
    constexpr int sampleRate = 22050;
    constexpr int amplitude = 7000;
    std::vector<std::int16_t> samples;
    samples.reserve(sampleRate / 2);
    appendTone(samples, sampleRate, 1400.0, 120, amplitude);
    appendTone(samples, sampleRate, 0.0, 30, 0);
    appendTone(samples, sampleRate, 1800.0, 160, amplitude);

    std::vector<std::uint8_t> pcm;
    pcm.reserve(samples.size() * sizeof(std::int16_t));
    for (std::int16_t sample : samples) {
      appendLe16(pcm, static_cast<std::uint16_t>(sample));
    }
    return pcm;
  }();

  static TonePlayer tonePlayer;
  if (!tonePlayer.play(pcmBytes)) {
    if (!::Beep(1400, 120)) {
      MessageBeep(MB_OK);
      return;
    }
    Sleep(30);
    ::Beep(1800, 160);
  }
}

BOOL WINAPI consoleCtrlHandler(DWORD ctrlType) {
  if (ctrlType == CTRL_C_EVENT || ctrlType == CTRL_BREAK_EVENT ||
      ctrlType == CTRL_CLOSE_EVENT || ctrlType == CTRL_SHUTDOWN_EVENT) {
    g_stopRequested = true;
    return TRUE;
  }
  return FALSE;
}

struct WinsockInit {
  WinsockInit() {
    WSADATA data {};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
      throw std::runtime_error("WSAStartup failed");
    }
  }

  ~WinsockInit() {
    WSACleanup();
  }
};

std::string wstringToUtf8(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string out(size - 1, '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), size, nullptr, nullptr);
  return out;
}

std::wstring utf8ToWstring(const std::string& value) {
  if (value.empty()) {
    return {};
  }
  int size = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (size <= 1) {
    return {};
  }
  std::wstring out(size - 1, L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), size);
  return out;
}

std::string sha256Hex(const std::vector<std::uint8_t>& data) {
  BCRYPT_ALG_HANDLE alg = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  DWORD objectSize = 0;
  DWORD hashLength = 0;
  DWORD bytesDone = 0;

  if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) {
    throw std::runtime_error("BCryptOpenAlgorithmProvider failed");
  }

  auto cleanup = [&]() {
    if (hash != nullptr) {
      BCryptDestroyHash(hash);
    }
    if (alg != nullptr) {
      BCryptCloseAlgorithmProvider(alg, 0);
    }
  };

  if (BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectSize),
                        sizeof(objectSize), &bytesDone, 0) != 0 ||
      BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&hashLength),
                        sizeof(hashLength), &bytesDone, 0) != 0) {
    cleanup();
    throw std::runtime_error("BCryptGetProperty failed");
  }

  std::vector<std::uint8_t> hashObject(objectSize);
  std::vector<std::uint8_t> hashValue(hashLength);
  if (BCryptCreateHash(alg, &hash, hashObject.data(), objectSize, nullptr, 0, 0) != 0 ||
      BCryptHashData(hash, const_cast<PUCHAR>(data.data()), static_cast<ULONG>(data.size()), 0) != 0 ||
      BCryptFinishHash(hash, hashValue.data(), hashLength, 0) != 0) {
    cleanup();
    throw std::runtime_error("BCrypt hash failed");
  }
  cleanup();

  std::ostringstream out;
  out << std::hex << std::setfill('0');
  for (std::uint8_t byte : hashValue) {
    out << std::setw(2) << static_cast<int>(byte);
  }
  return out.str();
}

std::uint32_t crc32Value(std::string_view text) {
  static std::array<std::uint32_t, 256> table = []() {
    std::array<std::uint32_t, 256> t {};
    for (std::uint32_t i = 0; i < 256; ++i) {
      std::uint32_t c = i;
      for (int j = 0; j < 8; ++j) {
        c = (c & 1U) ? (0xEDB88320U ^ (c >> 1U)) : (c >> 1U);
      }
      t[i] = c;
    }
    return t;
  }();

  std::uint32_t crc = 0xFFFFFFFFU;
  for (unsigned char ch : text) {
    crc = table[(crc ^ ch) & 0xFFU] ^ (crc >> 8U);
  }
  return crc ^ 0xFFFFFFFFU;
}

std::string crc32Hex(std::string_view text) {
  std::ostringstream out;
  out << std::hex << std::nouppercase << std::setfill('0') << std::setw(8)
      << crc32Value(text);
  return out.str();
}

std::vector<std::uint8_t> base64Decode(std::string_view input) {
  static const std::array<int, 256> table = []() {
    std::array<int, 256> t {};
    t.fill(-1);
    const std::string chars = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int i = 0; i < static_cast<int>(chars.size()); ++i) {
      t[static_cast<unsigned char>(chars[i])] = i;
    }
    t[static_cast<unsigned char>('=')] = 0;
    return t;
  }();

  std::string sanitized;
  sanitized.reserve(input.size());
  for (char ch : input) {
    if (std::isspace(static_cast<unsigned char>(ch))) {
      continue;
    }
    if (table[static_cast<unsigned char>(ch)] >= 0 || ch == '=') {
      sanitized.push_back(ch);
    }
  }
  while (!sanitized.empty() && sanitized.back() == '=') {
    sanitized.pop_back();
  }
  while (sanitized.size() % 4 != 0) {
    sanitized.push_back('=');
  }

  std::vector<std::uint8_t> out;
  out.reserve((sanitized.size() / 4) * 3);
  for (std::size_t i = 0; i < sanitized.size(); i += 4) {
    std::array<int, 4> values {};
    for (int j = 0; j < 4; ++j) {
      const char ch = sanitized[i + j];
      values[j] = table[static_cast<unsigned char>(ch)];
      if (values[j] < 0) {
        throw std::runtime_error("Invalid base64 payload");
      }
    }
    out.push_back(static_cast<std::uint8_t>((values[0] << 2) | (values[1] >> 4)));
    if (sanitized[i + 2] != '=') {
      out.push_back(static_cast<std::uint8_t>(((values[1] & 0x0F) << 4) | (values[2] >> 2)));
    }
    if (sanitized[i + 3] != '=') {
      out.push_back(static_cast<std::uint8_t>(((values[2] & 0x03) << 6) | values[3]));
    }
  }
  return out;
}

fs::path safeOutputPath(const fs::path& root, const fs::path& relativePath) {
  const fs::path canonicalRoot = fs::weakly_canonical(root);
  const fs::path destination = fs::weakly_canonical(root / relativePath);
  auto rootString = canonicalRoot.wstring();
  auto destString = destination.wstring();
  if (destString.rfind(rootString, 0) != 0) {
    throw std::runtime_error("Blocked path outside output directory");
  }
  return destination;
}

void extractZipWithPowerShell(const std::vector<std::uint8_t>& zipBytes,
                              const fs::path& outputDir,
                              const std::string& sessionId,
                              const std::string& fileId) {
  const fs::path tempRoot = outputDir / ".qrcode-reader.tmp";
  fs::create_directories(tempRoot);
  const fs::path zipPath = tempRoot / (sessionId + "_" + fileId + ".zip");
  {
    std::ofstream out(zipPath, std::ios::binary);
    out.write(reinterpret_cast<const char*>(zipBytes.data()),
              static_cast<std::streamsize>(zipBytes.size()));
  }

  std::wstring command =
      L"powershell -NoProfile -NonInteractive -ExecutionPolicy Bypass -Command "
      L"\"Expand-Archive -LiteralPath '";
  command += zipPath.wstring();
  command += L"' -DestinationPath '";
  command += outputDir.wstring();
  command += L"' -Force\"";

  STARTUPINFOW si {};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi {};
  std::vector<wchar_t> mutableCmd(command.begin(), command.end());
  mutableCmd.push_back(L'\0');
  if (!CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    throw std::runtime_error("Failed to launch PowerShell for zip extraction");
  }

  WaitForSingleObject(pi.hProcess, INFINITE);
  DWORD exitCode = 1;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  fs::remove(zipPath);
  if (exitCode != 0) {
    throw std::runtime_error("PowerShell Expand-Archive failed");
  }
}

class SpaceSender {
 public:
  SpaceSender(std::string host, int port, double connectTimeoutSeconds)
      : host_(std::move(host)),
        port_(port),
        connectTimeoutSeconds_(connectTimeoutSeconds) {}

  ~SpaceSender() { close(); }

  void close() {
    if (socket_ != INVALID_SOCKET) {
      closesocket(socket_);
      socket_ = INVALID_SOCKET;
    }
    pending_.clear();
    ready_.reset();
    lastError_.clear();
  }

  bool refreshReady(bool force) {
    const auto now = Clock::now();
    if (!force && ready_.has_value() &&
        std::chrono::duration<double>(now - lastStatusPoll_).count() < kStatusPollIntervalSeconds) {
      return *ready_;
    }
    if (!ensureConnected()) {
      return false;
    }

    const auto deadline = Clock::now() + std::chrono::duration<double>(connectTimeoutSeconds_);
    try {
      while (Clock::now() < deadline && !g_stopRequested.load()) {
        drainState();
        if (ready_.has_value()) {
          lastStatusPoll_ = Clock::now();
          lastError_.clear();
          return *ready_;
        }
        Sleep(50);
      }
    } catch (const std::exception&) {
      lastError_ = "TCP connect/status failed to " + host_ + ":" + std::to_string(port_);
      close();
      return false;
    }
    lastError_ = "ESP32 status not received";
    close();
    return false;
  }

  bool sendSpace() {
    if (!refreshReady(true)) {
      if (lastError_.empty()) {
        lastError_ = "ESP32 not in INPUT mode";
      }
      return false;
    }
    const char space = ' ';
    if (send(socket_, &space, 1, 0) != 1) {
      lastError_ = "TCP send failed to " + host_ + ":" + std::to_string(port_);
      close();
      return false;
    }
    lastError_.clear();
    return true;
  }

  bool sendLeft() { return sendControlFrame("KEY LEFT"); }
  bool sendRight() { return sendControlFrame("KEY RIGHT"); }

  std::string frameStatusMessage() const {
    if (ready_.has_value() && *ready_) {
      return "ESP32 status for frame: READY (" + host_ + ":" + std::to_string(port_) + ")";
    }
    if (!lastError_.empty()) {
      return "ESP32 status for frame: " + lastError_;
    }
    return "ESP32 status for frame: NOT READY - Turn on the INPUT mode of esp32 keyboard";
  }

 private:
  bool sendControlFrame(const char* command) {
    if (!refreshReady(true)) {
      if (lastError_.empty()) {
        lastError_ = "ESP32 not in INPUT mode";
      }
      return false;
    }
    std::string frame;
    frame.push_back(0x1E);
    frame += command;
    frame.push_back('\n');
    if (send(socket_, frame.data(), static_cast<int>(frame.size()), 0) !=
        static_cast<int>(frame.size())) {
      lastError_ = "TCP send failed to " + host_ + ":" + std::to_string(port_);
      close();
      return false;
    }
    lastError_.clear();
    return true;
  }

  bool ensureConnected() {
    if (socket_ != INVALID_SOCKET) {
      return true;
    }

    addrinfoW hints {};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfoW* result = nullptr;
    const std::wstring hostW = utf8ToWstring(host_);
    const std::wstring portW = std::to_wstring(port_);
    if (GetAddrInfoW(hostW.c_str(), portW.c_str(), &hints, &result) != 0) {
      lastError_ = "TCP connect/status failed to " + host_ + ":" + std::to_string(port_);
      return false;
    }

    SOCKET sock = INVALID_SOCKET;
    for (addrinfoW* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
      sock = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
      if (sock == INVALID_SOCKET) {
        continue;
      }
      u_long nonBlocking = 1;
      ioctlsocket(sock, FIONBIO, &nonBlocking);
      int connectResult = connect(sock, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen));
      if (connectResult == 0) {
        u_long blocking = 0;
        ioctlsocket(sock, FIONBIO, &blocking);
        break;
      }
      if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(sock, &writeSet);
        timeval timeout {};
        timeout.tv_sec = static_cast<long>(connectTimeoutSeconds_);
        timeout.tv_usec = static_cast<long>((connectTimeoutSeconds_ - timeout.tv_sec) * 1000000.0);
        int selectResult = select(0, nullptr, &writeSet, nullptr, &timeout);
        if (selectResult > 0) {
          int socketError = 0;
          int len = sizeof(socketError);
          getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError), &len);
          if (socketError == 0) {
            u_long blocking = 0;
            ioctlsocket(sock, FIONBIO, &blocking);
            break;
          }
        }
      }
      closesocket(sock);
      sock = INVALID_SOCKET;
    }
    FreeAddrInfoW(result);

    if (sock == INVALID_SOCKET) {
      lastError_ = "TCP connect/status failed to " + host_ + ":" + std::to_string(port_);
      return false;
    }
    int flag = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&flag), sizeof(flag));
    socket_ = sock;
    pending_.clear();
    ready_.reset();
    lastError_.clear();
    return true;
  }

  void drainState() {
    if (socket_ == INVALID_SOCKET) {
      return;
    }
    while (true) {
      fd_set readSet;
      FD_ZERO(&readSet);
      FD_SET(socket_, &readSet);
      timeval timeout {};
      const int result = select(0, &readSet, nullptr, nullptr, &timeout);
      if (result <= 0) {
        break;
      }
      std::array<char, 128> buffer {};
      int received = recv(socket_, buffer.data(), static_cast<int>(buffer.size()), 0);
      if (received <= 0) {
        throw std::runtime_error("socket closed by remote host");
      }
      pending_.append(buffer.data(), buffer.data() + received);
      std::size_t newlinePos = 0;
      while ((newlinePos = pending_.find('\n')) != std::string::npos) {
        std::string line = pending_.substr(0, newlinePos);
        pending_.erase(0, newlinePos + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
          line.pop_back();
        }
        if (line == "1") {
          ready_ = true;
        } else if (line == "0") {
          ready_ = false;
        }
      }
    }
  }

  std::string host_;
  int port_ = 0;
  double connectTimeoutSeconds_ = 0.0;
  SOCKET socket_ = INVALID_SOCKET;
  std::string pending_;
  std::optional<bool> ready_;
  std::string lastError_;
  Clock::time_point lastStatusPoll_ {};
};

std::optional<json> parseProtocolPayload(const std::string& payload) {
  try {
    const json message = json::parse(payload);
    if (!message.is_object()) {
      return std::nullopt;
    }
    if (message.value("m", "") != kProtocolMagic ||
        message.value("v", 0) != kProtocolVersion) {
      return std::nullopt;
    }
    return std::optional<json>(std::move(message));
  } catch (...) {
    return std::nullopt;
  }
}

bool hasNonEmptyStringField(const json& message, const char* key) {
  return message.contains(key) && message[key].is_string() &&
         !message[key].get_ref<const std::string&>().empty();
}

bool hasPositiveIntField(const json& message, const char* key) {
  return message.contains(key) && message[key].is_number_integer() &&
         message[key].get<int>() > 0;
}

bool hasSessionRoutingFields(const json& message) {
  return hasNonEmptyStringField(message, "s") &&
         hasNonEmptyStringField(message, "g") &&
         hasPositiveIntField(message, "si") &&
         hasPositiveIntField(message, "sc");
}

bool hasFileRoutingFields(const json& message) {
  return hasNonEmptyStringField(message, "f") &&
         hasPositiveIntField(message, "fi") &&
         hasPositiveIntField(message, "fc") &&
         hasPositiveIntField(message, "qi") &&
         hasPositiveIntField(message, "qc");
}

bool hasValidRoutingFields(const json& message) {
  const std::string frameType = message.value("t", "");
  if (frameType == "session_start" || frameType == "session_end") {
    return hasSessionRoutingFields(message) && hasPositiveIntField(message, "c");
  }
  if (frameType == "start" || frameType == "data" || frameType == "end") {
    return hasSessionRoutingFields(message) && hasFileRoutingFields(message);
  }
  return false;
}

std::string describeFileTarget(const TransferBuffer& buffer, const std::string& fileId) {
  return buffer.relativePath.empty() ? fileId : buffer.relativePath;
}

void appendFileProgress(std::ostringstream& out, const json& message, const char* prefix = " ") {
  const int fileIndex = message.value("fi", 0);
  const int fileCount = message.value("fc", 0);
  const int fileQrIndex = message.value("qi", 0);
  const int fileQrCount = message.value("qc", 0);
  if (fileIndex > 0 && fileCount > 0) {
    out << prefix << "file " << fileIndex << "/" << fileCount;
    prefix = " ";
  }
  if (fileQrIndex > 0 && fileQrCount > 0) {
    out << prefix << "qr " << fileQrIndex << "/" << fileQrCount;
  }
}

void resetSessionTracking(std::set<std::string>& seenPayloads,
                          std::unordered_map<std::string, Clock::time_point>& lastSeen,
                          std::set<std::string>& processedPageIds,
                          std::map<std::pair<std::string, std::string>, TransferBuffer>& transfers,
                          std::set<std::pair<std::string, std::string>>& completed,
                          std::unordered_map<std::string, SessionState>& sessions,
                          BorderState& borderState,
                          std::string& lastAcceptedPageId,
                          int& lastAcceptedSessionSeq,
                          bool& hasLastAcceptedSessionSeq) {
  transfers.clear();
  completed.clear();
  sessions.clear();
  seenPayloads.clear();
  lastSeen.clear();
  processedPageIds.clear();
  borderState = BorderState{};
  lastAcceptedPageId.clear();
  lastAcceptedSessionSeq = 0;
  hasLastAcceptedSessionSeq = false;
}

TransferBuffer& getBuffer(std::map<std::pair<std::string, std::string>, TransferBuffer>& transfers,
                          const std::string& sessionId,
                          const std::string& fileId) {
  const auto key = std::make_pair(sessionId, fileId);
  auto it = transfers.find(key);
  if (it == transfers.end()) {
    TransferBuffer buffer;
    buffer.sessionId = sessionId;
    buffer.fileId = fileId;
    it = transfers.emplace(key, std::move(buffer)).first;
  }
  return it->second;
}

std::string finalizeTransfer(const TransferBuffer& buffer, const fs::path& outputDir) {
  if (buffer.expectedChunks <= 0) {
    throw std::runtime_error("Missing chunk count for " + buffer.sessionId + "/" + buffer.fileId);
  }
  for (int index = 1; index <= buffer.expectedChunks; ++index) {
    if (!buffer.chunks.contains(index)) {
      throw std::runtime_error("Missing chunks for " + describeFileTarget(buffer, buffer.fileId));
    }
  }

  std::string payloadAscii;
  for (int index = 1; index <= buffer.expectedChunks; ++index) {
    payloadAscii += buffer.chunks.at(index);
  }
  const std::vector<std::uint8_t> payloadBytes(payloadAscii.begin(), payloadAscii.end());
  const std::string targetName = describeFileTarget(buffer, buffer.fileId);
  if (!buffer.payloadSha256.empty() && sha256Hex(payloadBytes) != buffer.payloadSha256) {
    throw std::runtime_error("Payload hash mismatch for " + targetName);
  }

  std::vector<std::uint8_t> zipBytes;
  if (buffer.encoding == "base64") {
    zipBytes = base64Decode(payloadAscii);
  } else if (buffer.encoding == "raw") {
    zipBytes = payloadBytes;
  } else {
    throw std::runtime_error("Unsupported encoding " + buffer.encoding);
  }
  if (!buffer.zipSha256.empty() && sha256Hex(zipBytes) != buffer.zipSha256) {
    throw std::runtime_error("ZIP hash mismatch for " + targetName);
  }
  extractZipWithPowerShell(zipBytes, outputDir, buffer.sessionId, buffer.fileId);
  if (!buffer.relativePath.empty()) {
    return "Saved " + buffer.relativePath;
  }
  return "Saved file(s) from " + buffer.fileId;
}

ProcessResult processProtocolMessage(
    const json& message,
    const fs::path& outputDir,
    std::map<std::pair<std::string, std::string>, TransferBuffer>& transfers,
    std::set<std::pair<std::string, std::string>>& completed,
    std::unordered_map<std::string, SessionState>& sessions) {
  auto activeTransferCount = [&](const std::string& sessionId) {
    int count = 0;
    for (const auto& [key, _buffer] : transfers) {
      if (key.first == sessionId) {
        ++count;
      }
    }
    return count;
  };

  const std::string frameType = message.value("t", "");
  const std::string sessionId = message.value("s", "");
  const std::string fileId = message.value("f", "");

  if (frameType == "session_start") {
    if (sessionId.empty()) {
      return {std::string("Rejected SESSION_START without session id"), false, false};
    }
    auto& session = sessions[sessionId];
    session.expectedFiles = std::max(session.expectedFiles, message.value("c", 0));
    const int sessionPageTotal = message.value("sc", 0);
    const int sessionPageIndex = message.value("si", 0);
    std::ostringstream out;
    out << "Session start: " << sessionId << " (" << session.expectedFiles << " files";
    if (sessionPageTotal > 0 && sessionPageIndex > 0) {
      out << ", qr " << sessionPageIndex << "/" << sessionPageTotal;
    }
    out << ")";
    return {out.str(), false};
  }

  if (frameType == "session_end") {
    if (sessionId.empty()) {
      return {std::string("Rejected SESSION_END without session id"), false, false};
    }
    auto& session = sessions[sessionId];
    session.expectedFiles = std::max(session.expectedFiles, message.value("c", 0));
    const int repeatIndex = message.value("ri", 0);
    const int repeatTotal = message.value("rc", 0);
    if (repeatTotal > 0) {
      session.sessionEndRepeatTotal = std::max(session.sessionEndRepeatTotal, repeatTotal);
    }
    if (repeatIndex > 0) {
      session.sessionEndRepeatsSeen.insert(repeatIndex);
    }
    int requiredRepeats = session.sessionEndRepeatTotal >= 2 ? 2 : 1;
    session.sessionEndSeen = static_cast<int>(session.sessionEndRepeatsSeen.size()) >= requiredRepeats;
    const int completedCount = static_cast<int>(session.completedFiles.size());
    const int startedCount = static_cast<int>(session.startedFiles.size());
    const int activeCount = activeTransferCount(sessionId);
    if (session.sessionEndSeen && session.expectedFiles > 0 &&
        completedCount >= session.expectedFiles &&
        startedCount >= session.expectedFiles &&
        activeCount == 0) {
      std::ostringstream out;
      out << "Session complete: " << sessionId << " (" << completedCount
          << "/" << session.expectedFiles << " files)";
      return {out.str(), true};
    }
    std::ostringstream out;
    out << "Session end seen for " << sessionId << ", waiting for files (started "
        << startedCount << "/" << session.expectedFiles << ", complete "
        << completedCount << "/" << session.expectedFiles << ", active "
        << activeCount << ", session_end " << session.sessionEndRepeatsSeen.size()
        << "/" << session.sessionEndRepeatTotal << ")";
    return {out.str(), false};
  }

  if (sessionId.empty() || fileId.empty()) {
    return {std::string("Rejected frame without session/file id"), false, false};
  }

  auto& session = sessions[sessionId];
  const auto key = std::make_pair(sessionId, fileId);
  if (frameType == "start") {
    auto& buffer = getBuffer(transfers, sessionId, fileId);
    session.startedFiles.insert(fileId);
    const std::string path = message.value("p", buffer.relativePath);
    const std::string encoding = message.value("e", buffer.encoding);
    const int expectedChunks = message.value("c", buffer.expectedChunks);
    const std::string zipSha256 = message.value("zh", buffer.zipSha256);
    const bool duplicate = buffer.relativePath == path &&
                           buffer.encoding == encoding &&
                           buffer.expectedChunks == expectedChunks &&
                           buffer.zipSha256 == zipSha256;
    buffer.relativePath = path;
    buffer.encoding = encoding;
    buffer.expectedChunks = expectedChunks;
    buffer.zipSha256 = zipSha256;
    if (duplicate) {
      return {};
    }
    std::ostringstream out;
    out << "Started " << describeFileTarget(buffer, fileId);
    out << " (" << buffer.expectedChunks << " data chunks";
    appendFileProgress(out, message, ", ");
    out << ")";
    return {out.str(), false};
  }

  if (frameType == "data") {
    if (completed.contains(key)) {
      return {};
    }
    auto& buffer = getBuffer(transfers, sessionId, fileId);
    const int index = message.value("i", 0);
    const int total = message.value("n", 0);
    buffer.relativePath = message.value("p", buffer.relativePath);
    buffer.encoding = message.value("e", buffer.encoding);
    const std::string chunk = message.value("d", "");
    const std::string chunkCrc = message.value("k", "");
    if (index <= 0 || total <= 0 || chunk.empty()) {
      return {std::string("Rejected DATA frame with missing chunk metadata"), false, false};
    }
    if (!chunkCrc.empty() && crc32Hex(chunk) != chunkCrc) {
      std::ostringstream out;
      out << "Rejected corrupt chunk " << index << "/" << total << " for "
          << describeFileTarget(buffer, fileId);
      return {out.str(), false, false};
    }
    buffer.expectedChunks = std::max(buffer.expectedChunks, total);
    auto existing = buffer.chunks.find(index);
    if (existing == buffer.chunks.end()) {
      buffer.chunks[index] = chunk;
    } else if (existing->second != chunk) {
      std::ostringstream out;
      out << "Conflicting chunk " << index << "/" << buffer.expectedChunks << " for "
          << describeFileTarget(buffer, fileId);
      return {out.str(), false, false};
    } else {
      return {};
    }
    std::ostringstream out;
    out << "Chunk " << index << "/" << buffer.expectedChunks << " for "
        << describeFileTarget(buffer, fileId);
    appendFileProgress(out, message);
    return {out.str(), false};
  }

  if (frameType == "end") {
    if (completed.contains(key)) {
      return {};
    }
    auto& buffer = getBuffer(transfers, sessionId, fileId);
    buffer.relativePath = message.value("p", buffer.relativePath);
    buffer.encoding = message.value("e", buffer.encoding);
    buffer.expectedChunks = message.value("c", buffer.expectedChunks);
    buffer.payloadSha256 = message.value("ph", buffer.payloadSha256);
    buffer.zipSha256 = message.value("zh", buffer.zipSha256);
    std::string result = finalizeTransfer(buffer, outputDir);
    if (message.value("fi", 0) > 0 && message.value("fc", 0) > 0) {
      std::ostringstream prefixed;
      prefixed << result << " |";
      appendFileProgress(prefixed, message);
      result = prefixed.str();
    }
    completed.insert(key);
    session.completedFiles.insert(fileId);
    transfers.erase(key);

    if (session.sessionEndSeen && session.expectedFiles > 0) {
      const int completedCount = static_cast<int>(session.completedFiles.size());
      const int startedCount = static_cast<int>(session.startedFiles.size());
      const int activeCount = activeTransferCount(sessionId);
      if (completedCount >= session.expectedFiles &&
          startedCount >= session.expectedFiles &&
          activeCount == 0) {
        std::ostringstream out;
        out << result << " | Session complete: " << sessionId << " ("
            << completedCount << "/" << session.expectedFiles << " files)";
        return {out.str(), true};
      }
      std::ostringstream out;
      out << result << " | Waiting for more files in session " << sessionId
          << " (started " << startedCount << "/" << session.expectedFiles
          << ", complete " << completedCount << "/" << session.expectedFiles
          << ", active " << activeCount << ")";
      return {out.str(), false};
    }
    return {result, false};
  }

  return {};
}

cv::Mat quickGrayFrame(const cv::Mat& frame) {
  cv::Mat gray;
  if (frame.channels() == 1) {
    gray = frame.clone();
  } else if (frame.channels() == 3) {
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
  } else if (frame.channels() == 4) {
    cv::cvtColor(frame, gray, cv::COLOR_BGRA2GRAY);
  } else {
    throw std::runtime_error("Unsupported camera frame format");
  }
  return gray;
}

cv::Mat preprocessFrame(const cv::Mat& frame) {
  cv::Mat gray = quickGrayFrame(frame);
  cv::Mat filtered;
  cv::bilateralFilter(gray, filtered, 5, 50.0, 50.0);
  auto clahe = cv::createCLAHE(2.5, cv::Size(8, 8));
  cv::Mat enhanced;
  clahe->apply(filtered, enhanced);
  int minSide = std::min(enhanced.rows, enhanced.cols);
  double scale = 1.0;
  if (minSide < 480) {
    scale = 4.0;
  } else if (minSide < 720) {
    scale = 2.0;
  }
  if (scale != 1.0) {
    cv::resize(enhanced, enhanced, {}, scale, scale, cv::INTER_CUBIC);
  }
  return enhanced;
}

cv::Mat thresholdFrame(const cv::Mat& gray) {
  cv::Mat blurred;
  cv::GaussianBlur(gray, blurred, cv::Size(5, 5), 0);
  cv::Mat out;
  cv::adaptiveThreshold(blurred, out, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                        cv::THRESH_BINARY, 41, 10);
  return out;
}

cv::Mat otsuFrame(const cv::Mat& gray) {
  cv::Mat blurred;
  cv::GaussianBlur(gray, blurred, cv::Size(3, 3), 0);
  cv::Mat out;
  cv::threshold(blurred, out, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
  return out;
}

cv::Mat sharpenFrame(const cv::Mat& gray) {
  cv::Mat blurred;
  cv::GaussianBlur(gray, blurred, cv::Size(), 1.2);
  cv::Mat out;
  cv::addWeighted(gray, 1.7, blurred, -0.7, 0.0, out);
  return out;
}

cv::Mat rotateImage(const cv::Mat& image, int mode) {
  cv::Mat out;
  cv::rotate(image, out, mode);
  return out;
}

cv::QRCodeDetector makeDetector() {
  cv::QRCodeDetector detector;
  detector.setEpsX(0.2);
  detector.setEpsY(0.2);
  return detector;
}

ZXing::ReaderOptions makeZXingOptions() {
  ZXing::ReaderOptions options;
  options.setFormats(ZXing::BarcodeFormat::QRCode);
  options.setTryHarder(true);
  options.setTryRotate(true);
  options.setTryInvert(true);
  options.setTryDownscale(false);
  options.setTextMode(ZXing::TextMode::Plain);
  options.setMaxNumberOfSymbols(8);
  return options;
}

std::vector<std::vector<int>> findQuadCandidates(const cv::Mat& gray);

std::vector<DecodedPayload> decodeWithOpenCv(const cv::Mat& frame, cv::QRCodeDetector& detector) {
  std::vector<DecodedPayload> out;

  auto appendSingle = [&](const std::string& payload, const cv::Mat& points) {
    if (payload.empty()) {
      return;
    }
    DecodedPayload decoded;
    decoded.payload = payload;
    if (!points.empty()) {
      cv::Mat flat = points.reshape(1, 1);
      for (int i = 0; i < flat.cols; ++i) {
        decoded.points.push_back(static_cast<int>(std::lround(flat.at<float>(0, i))));
      }
    }
    out.push_back(std::move(decoded));
  };

  try {
    cv::Mat points;
    const std::string payload = detector.detectAndDecode(frame, points);
    appendSingle(payload, points);
  } catch (...) {
  }

  if (out.empty()) {
    try {
      cv::Mat points;
      const std::string payload = detector.detectAndDecodeCurved(frame, points);
      appendSingle(payload, points);
    } catch (...) {
    }
  }

  try {
    std::vector<cv::String> payloads;
    cv::Mat points;
    bool ok = detector.detectAndDecodeMulti(frame, payloads, points);
    if (ok) {
      for (int row = 0; row < points.rows && row < static_cast<int>(payloads.size()); ++row) {
        appendSingle(payloads[row], points.row(row));
      }
    }
  } catch (...) {
  }

  return out;
}

std::vector<DecodedPayload> decodeWithZXing(const cv::Mat& frame, const ZXing::ReaderOptions& options) {
  std::vector<DecodedPayload> out;
  if (frame.empty()) {
    return out;
  }

  ZXing::ImageFormat format = ZXing::ImageFormat::None;
  if (frame.channels() == 1) {
    format = ZXing::ImageFormat::Lum;
  } else if (frame.channels() == 3) {
    format = ZXing::ImageFormat::BGR;
  } else if (frame.channels() == 4) {
    format = ZXing::ImageFormat::BGRA;
  } else {
    return out;
  }

  auto collect = [&](const cv::Mat& imageMat, ZXing::ReaderOptions localOptions) -> bool {
    try {
      ZXing::ImageFormat localFormat = ZXing::ImageFormat::None;
      if (imageMat.channels() == 1) {
        localFormat = ZXing::ImageFormat::Lum;
      } else if (imageMat.channels() == 3) {
        localFormat = ZXing::ImageFormat::BGR;
      } else if (imageMat.channels() == 4) {
        localFormat = ZXing::ImageFormat::BGRA;
      } else {
        return false;
      }
      ZXing::ImageView image(imageMat.data, imageMat.cols, imageMat.rows, localFormat,
                             static_cast<int>(imageMat.step),
                             static_cast<int>(imageMat.elemSize()));
      auto results = ZXing::ReadBarcodes(image, localOptions);
      for (const auto& result : results) {
        if (!result.isValid() || result.format() != ZXing::BarcodeFormat::QRCode) {
          continue;
        }
        DecodedPayload decoded;
        decoded.payload = result.text();
        if (decoded.payload.empty()) {
          continue;
        }
        const auto& pos = result.position();
        decoded.points = {
            pos.topLeft().x, pos.topLeft().y,
            pos.topRight().x, pos.topRight().y,
            pos.bottomRight().x, pos.bottomRight().y,
            pos.bottomLeft().x, pos.bottomLeft().y,
        };
        out.push_back(std::move(decoded));
      }
      return !out.empty();
    } catch (...) {
      return false;
    }
  };

  for (ZXing::Binarizer binarizer : {
           ZXing::Binarizer::LocalAverage,
           ZXing::Binarizer::GlobalHistogram,
           ZXing::Binarizer::FixedThreshold,
       }) {
    ZXing::ReaderOptions localOptions = options;
    localOptions.setBinarizer(binarizer);
    if (collect(frame, localOptions)) {
      return out;
    }
  }

  cv::Mat gray = quickGrayFrame(frame);
  cv::Mat inverted;
  cv::bitwise_not(gray, inverted);
  for (ZXing::Binarizer binarizer : {
           ZXing::Binarizer::LocalAverage,
           ZXing::Binarizer::GlobalHistogram,
           ZXing::Binarizer::FixedThreshold,
       }) {
    ZXing::ReaderOptions localOptions = options;
    localOptions.setBinarizer(binarizer);
    if (collect(inverted, localOptions)) {
      return out;
    }
  }
  return out;
}

std::vector<DecodedPayload> decodeVariants(
    const std::vector<cv::Mat>& variants,
    cv::QRCodeDetector& detector,
    const ZXing::ReaderOptions& zxingOptions,
    bool includeRotations,
    const std::atomic_bool* cancelRequested = nullptr) {
  std::vector<DecodedPayload> out;
  for (const auto& variant : variants) {
    if (cancelRequested != nullptr && cancelRequested->load()) {
      return {};
    }
    auto decoded = decodeWithZXing(variant, zxingOptions);
    if (decoded.empty()) {
      decoded = decodeWithOpenCv(variant, detector);
    }
    out.insert(out.end(), decoded.begin(), decoded.end());
    if (includeRotations) {
      for (int mode : {cv::ROTATE_90_CLOCKWISE, cv::ROTATE_180, cv::ROTATE_90_COUNTERCLOCKWISE}) {
        if (cancelRequested != nullptr && cancelRequested->load()) {
          return {};
        }
        cv::Mat rotated = rotateImage(variant, mode);
        auto rotatedDecoded = decodeWithZXing(rotated, zxingOptions);
        if (rotatedDecoded.empty()) {
          rotatedDecoded = decodeWithOpenCv(rotated, detector);
        }
        out.insert(out.end(), rotatedDecoded.begin(), rotatedDecoded.end());
      }
    }
    if (!out.empty()) {
      return out;
    }
  }
  return out;
}

std::vector<std::vector<int>> detectPointsOnly(const cv::Mat& frame, cv::QRCodeDetector& detector) {
  std::vector<std::vector<int>> boxes;
  try {
    cv::Mat points;
    bool found = detector.detect(frame, points);
    if (found && !points.empty()) {
      cv::Mat flat = points.reshape(1, 1);
      std::vector<int> pts;
      pts.reserve(flat.cols);
      for (int i = 0; i < flat.cols; ++i) {
        pts.push_back(static_cast<int>(std::lround(flat.at<float>(0, i))));
      }
      boxes.push_back(std::move(pts));
      return boxes;
    }
  } catch (...) {
  }

  try {
    cv::Mat points;
    bool found = detector.detectMulti(frame, points);
    if (found && !points.empty()) {
      for (int row = 0; row < points.rows; ++row) {
        cv::Mat flat = points.row(row).reshape(1, 1);
        std::vector<int> pts;
        pts.reserve(flat.cols);
        for (int i = 0; i < flat.cols; ++i) {
          pts.push_back(static_cast<int>(std::lround(flat.at<float>(0, i))));
        }
        boxes.push_back(std::move(pts));
      }
    }
  } catch (...) {
  }
  return boxes;
}

std::optional<cv::Mat> cropBoxRegion(const cv::Mat& image, const std::vector<int>& pts, double padRatio = 0.18) {
  if (pts.size() < 8) {
    return std::nullopt;
  }
  std::vector<cv::Point2f> quad;
  quad.reserve(4);
  for (std::size_t i = 0; i + 1 < pts.size() && quad.size() < 4; i += 2) {
    quad.emplace_back(static_cast<float>(pts[i]), static_cast<float>(pts[i + 1]));
  }
  float minX = quad.front().x;
  float minY = quad.front().y;
  float maxX = quad.front().x;
  float maxY = quad.front().y;
  for (const auto& p : quad) {
    minX = std::min(minX, p.x);
    minY = std::min(minY, p.y);
    maxX = std::max(maxX, p.x);
    maxY = std::max(maxY, p.y);
  }
  int x0 = std::max(0, static_cast<int>(std::floor(minX)));
  int y0 = std::max(0, static_cast<int>(std::floor(minY)));
  int x1 = std::min(image.cols, static_cast<int>(std::ceil(maxX)));
  int y1 = std::min(image.rows, static_cast<int>(std::ceil(maxY)));
  int width = x1 - x0;
  int height = y1 - y0;
  if (width <= 8 || height <= 8) {
    return std::nullopt;
  }
  int pad = static_cast<int>(std::max(width, height) * padRatio);
  x0 = std::max(0, x0 - pad);
  y0 = std::max(0, y0 - pad);
  x1 = std::min(image.cols, x1 + pad);
  y1 = std::min(image.rows, y1 + pad);
  return image(cv::Rect(x0, y0, x1 - x0, y1 - y0)).clone();
}

std::vector<DecodedPayload> decodeRoiCandidates(
    const cv::Mat& frame,
    const cv::Mat& quickGray,
    const std::vector<std::vector<int>>& boxes,
    cv::QRCodeDetector& detector,
    const ZXing::ReaderOptions& zxingOptions) {
  std::vector<DecodedPayload> out;
  const std::size_t limit = std::min<std::size_t>(2, boxes.size());
  for (std::size_t i = 0; i < limit; ++i) {
    auto frameRoi = cropBoxRegion(frame, boxes[i]);
    if (frameRoi.has_value()) {
      cv::Mat grayRoi = quickGrayFrame(*frameRoi);
      std::vector<cv::Mat> variants = {
          grayRoi,
          sharpenFrame(grayRoi),
          thresholdFrame(grayRoi),
      };
      out = decodeVariants(variants, detector, zxingOptions, false);
      if (!out.empty()) {
        return out;
      }
    }
    auto grayRoi = cropBoxRegion(quickGray, boxes[i]);
    if (grayRoi.has_value()) {
      std::vector<cv::Mat> variants = {
          *grayRoi,
          sharpenFrame(*grayRoi),
          thresholdFrame(*grayRoi),
      };
      out = decodeVariants(variants, detector, zxingOptions, false);
      if (!out.empty()) {
        return out;
      }
    }
  }
  return out;
}

std::vector<std::vector<int>> findDisplayBoxes(const cv::Mat& frame, cv::QRCodeDetector& detector) {
  auto boxes = detectPointsOnly(frame, detector);
  if (!boxes.empty()) {
    return boxes;
  }
  return findQuadCandidates(quickGrayFrame(frame));
}

std::vector<std::vector<int>> findQuadCandidates(const cv::Mat& gray) {
  cv::Mat edges;
  cv::Canny(gray, edges, 60, 180);
  cv::dilate(edges, edges, cv::Mat::ones(3, 3, CV_8U), cv::Point(-1, -1), 1);

  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(edges, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
  const double imageArea = static_cast<double>(gray.rows * gray.cols);
  std::vector<std::pair<double, std::vector<int>>> candidates;

  for (const auto& contour : contours) {
    double area = cv::contourArea(contour);
    if (area < imageArea * 0.01 || area > imageArea * 0.85) {
      continue;
    }
    double perimeter = cv::arcLength(contour, true);
    std::vector<cv::Point> approx;
    cv::approxPolyDP(contour, approx, 0.04 * perimeter, true);
    if (approx.size() != 4 || !cv::isContourConvex(approx)) {
      continue;
    }
    cv::RotatedRect rect = cv::minAreaRect(contour);
    float width = rect.size.width;
    float height = rect.size.height;
    if (width <= 1.0f || height <= 1.0f) {
      continue;
    }
    double aspect = std::max(static_cast<double>(width), static_cast<double>(height)) /
                    std::max(1.0, std::min(static_cast<double>(width), static_cast<double>(height)));
    if (aspect > 1.6) {
      continue;
    }
    std::vector<int> pts;
    pts.reserve(8);
    for (const auto& point : approx) {
      pts.push_back(point.x);
      pts.push_back(point.y);
    }
    candidates.emplace_back(area, std::move(pts));
  }

  std::sort(candidates.begin(), candidates.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });

  std::vector<std::vector<int>> deduped;
  std::vector<cv::Point2f> seenCenters;
  for (const auto& candidate : candidates) {
    const auto& pts = candidate.second;
    cv::Point2f center(0.0f, 0.0f);
    for (int i = 0; i < 8; i += 2) {
      center.x += static_cast<float>(pts[i]);
      center.y += static_cast<float>(pts[i + 1]);
    }
    center.x /= 4.0f;
    center.y /= 4.0f;
    bool tooClose = false;
    for (const auto& seen : seenCenters) {
      if (std::abs(center.x - seen.x) < 24.0f && std::abs(center.y - seen.y) < 24.0f) {
        tooClose = true;
        break;
      }
    }
    if (tooClose) {
      continue;
    }
    seenCenters.push_back(center);
    deduped.push_back(pts);
    if (deduped.size() >= 6) {
      break;
    }
  }
  return deduped;
}

std::array<cv::Point2f, 4> orderQuadPoints(const std::vector<int>& pts) {
  if (pts.size() < 8) {
    throw std::runtime_error("Quad needs 4 points");
  }
  std::array<cv::Point2f, 4> input = {
      cv::Point2f(static_cast<float>(pts[0]), static_cast<float>(pts[1])),
      cv::Point2f(static_cast<float>(pts[2]), static_cast<float>(pts[3])),
      cv::Point2f(static_cast<float>(pts[4]), static_cast<float>(pts[5])),
      cv::Point2f(static_cast<float>(pts[6]), static_cast<float>(pts[7])),
  };
  std::array<cv::Point2f, 4> ordered {};
  auto sum = [](const cv::Point2f& p) { return p.x + p.y; };
  auto diff = [](const cv::Point2f& p) { return p.x - p.y; };
  ordered[0] = *std::min_element(input.begin(), input.end(),
                                 [&](const auto& a, const auto& b) { return sum(a) < sum(b); });
  ordered[2] = *std::max_element(input.begin(), input.end(),
                                 [&](const auto& a, const auto& b) { return sum(a) < sum(b); });
  ordered[1] = *std::min_element(input.begin(), input.end(),
                                 [&](const auto& a, const auto& b) { return diff(a) < diff(b); });
  ordered[3] = *std::max_element(input.begin(), input.end(),
                                 [&](const auto& a, const auto& b) { return diff(a) < diff(b); });
  return ordered;
}

std::optional<cv::Mat> warpQrCandidate(const cv::Mat& frame, const std::vector<int>& pts) {
  try {
    const auto quad = orderQuadPoints(pts);
    auto dist = [](const cv::Point2f& a, const cv::Point2f& b) {
      return std::hypot(static_cast<double>(a.x - b.x), static_cast<double>(a.y - b.y));
    };
    int side = static_cast<int>(std::lround(std::max(std::max(dist(quad[0], quad[1]), dist(quad[1], quad[2])),
                                                     std::max(dist(quad[2], quad[3]), dist(quad[3], quad[0])))));
    side = std::clamp(side + 32, 160, 900);
    std::array<cv::Point2f, 4> dst = {
        cv::Point2f(0.0f, 0.0f),
        cv::Point2f(static_cast<float>(side - 1), 0.0f),
        cv::Point2f(static_cast<float>(side - 1), static_cast<float>(side - 1)),
        cv::Point2f(0.0f, static_cast<float>(side - 1)),
    };
    cv::Mat transform = cv::getPerspectiveTransform(quad.data(), dst.data());
    cv::Mat warped;
    cv::warpPerspective(frame, warped, transform, cv::Size(side, side));
    return warped;
  } catch (...) {
    return std::nullopt;
  }
}

std::vector<DecodedPayload> decodeWarpedCandidates(
    const cv::Mat& frame,
    const std::vector<std::vector<int>>& boxes,
    cv::QRCodeDetector& detector,
    const ZXing::ReaderOptions& zxingOptions) {
  std::vector<DecodedPayload> out;
  const std::size_t limit = std::min<std::size_t>(1, boxes.size());
  for (std::size_t i = 0; i < limit; ++i) {
    auto warped = warpQrCandidate(frame, boxes[i]);
    if (!warped.has_value()) {
      continue;
    }
    cv::Mat gray = preprocessFrame(*warped);
    std::vector<cv::Mat> variants = {
        gray,
        sharpenFrame(gray),
        thresholdFrame(gray),
        otsuFrame(gray),
    };
    out = decodeVariants(variants, detector, zxingOptions, true);
    if (!out.empty()) {
      return out;
    }
  }
  return out;
}

std::vector<int> detectColoredBorderRoi(const cv::Mat& frame, cv::Rect& roi) {
  cv::Mat sample = frame;
  double scale = 1.0;
  if (frame.cols > kBorderDetectMaxWidth) {
    scale = static_cast<double>(kBorderDetectMaxWidth) / static_cast<double>(frame.cols);
    int targetHeight = std::max(1, static_cast<int>(std::lround(frame.rows * scale)));
    cv::resize(frame, sample, cv::Size(kBorderDetectMaxWidth, targetHeight), 0, 0, cv::INTER_AREA);
  }

  cv::Mat hsv;
  cv::cvtColor(sample, hsv, cv::COLOR_BGR2HSV);
  cv::Mat mask;
  cv::inRange(hsv, kCyanLow, kCyanHigh, mask);
  cv::Mat kernel = cv::Mat::ones(3, 3, CV_8U);
  cv::morphologyEx(mask, mask, cv::MORPH_CLOSE, kernel, cv::Point(-1, -1), 2);
  cv::dilate(mask, mask, kernel, cv::Point(-1, -1), 1);

  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
  const double sampleArea = static_cast<double>(sample.rows * sample.cols);
  std::vector<cv::Rect> selected;
  for (const auto& contour : contours) {
    double area = cv::contourArea(contour);
    if (area < sampleArea * 0.0015) {
      continue;
    }
    cv::Rect rect = cv::boundingRect(contour);
    if (rect.width <= 8 || rect.height <= 8) {
      continue;
    }
    selected.push_back(rect);
  }
  if (selected.empty()) {
    return {};
  }

  int x0 = selected.front().x;
  int y0 = selected.front().y;
  int x1 = selected.front().x + selected.front().width;
  int y1 = selected.front().y + selected.front().height;
  for (const auto& rect : selected) {
    x0 = std::min(x0, rect.x);
    y0 = std::min(y0, rect.y);
    x1 = std::max(x1, rect.x + rect.width);
    y1 = std::max(y1, rect.y + rect.height);
  }
  if (static_cast<double>((x1 - x0) * (y1 - y0)) < sampleArea * 0.05) {
    return {};
  }
  int inset = std::max(8, static_cast<int>(std::min(x1 - x0, y1 - y0) * 0.06));
  x0 = std::clamp(x0 + inset, 0, sample.cols - 1);
  y0 = std::clamp(y0 + inset, 0, sample.rows - 1);
  x1 = std::clamp(x1 - inset, x0 + 1, sample.cols);
  y1 = std::clamp(y1 - inset, y0 + 1, sample.rows);

  if (scale != 1.0) {
    x0 = std::clamp(static_cast<int>(std::lround(x0 / scale)), 0, frame.cols - 1);
    y0 = std::clamp(static_cast<int>(std::lround(y0 / scale)), 0, frame.rows - 1);
    x1 = std::clamp(static_cast<int>(std::lround(x1 / scale)), x0 + 1, frame.cols);
    y1 = std::clamp(static_cast<int>(std::lround(y1 / scale)), y0 + 1, frame.rows);
  }
  roi = cv::Rect(x0, y0, x1 - x0, y1 - y0);
  return {x0, y0, x1, y0, x1, y1, x0, y1};
}

bool resolveBorderRoi(const cv::Mat& frame, BorderState& borderState, cv::Rect& roi, std::vector<int>& roiBox) {
  roiBox = detectColoredBorderRoi(frame, roi);
  if (!roiBox.empty()) {
    borderState.roi = roi;
    borderState.box = roiBox;
    borderState.framesSinceSeen = 0;
    borderState.locked = true;
    return true;
  }

  if (borderState.locked && borderState.framesSinceSeen < kBorderLockFrames) {
    ++borderState.framesSinceSeen;
    roi = borderState.roi & cv::Rect(0, 0, frame.cols, frame.rows);
    if (roi.width > 1 && roi.height > 1) {
      roiBox = borderState.box;
      return true;
    }
  }

  borderState.locked = false;
  borderState.framesSinceSeen = kBorderLockFrames + 1;
  return false;
}

std::pair<std::vector<std::string>, std::vector<std::vector<int>>> detectQr(
    const cv::Mat& frame,
    bool useHeavyPass,
    BorderState& borderState,
    cv::QRCodeDetector& detector,
    const ZXing::ReaderOptions& zxingOptions) {
  std::vector<std::vector<int>> boxes;
  std::vector<std::string> payloads;

  cv::Rect roi;
  std::vector<int> roiBox;
  if (resolveBorderRoi(frame, borderState, roi, roiBox)) {
    cv::Mat roiFrame = frame(roi);
    std::vector<cv::Mat> roiFastVariants = {
        roiFrame,
        quickGrayFrame(roiFrame),
    };
    for (auto& decoded : decodeVariants(roiFastVariants, detector, zxingOptions, false)) {
      if (!decoded.payload.empty()) {
        payloads.push_back(decoded.payload);
      }
    }
    if (!payloads.empty()) {
      boxes.push_back(roiBox);
      return {payloads, boxes};
    }
    if (useHeavyPass) {
      cv::Mat roiPrep = preprocessFrame(roiFrame);
      std::vector<std::vector<int>> roiBoxes = detectPointsOnly(roiFrame, detector);
      if (roiBoxes.empty()) {
        roiBoxes = findQuadCandidates(quickGrayFrame(roiFrame));
      }
      if (roiBoxes.empty()) {
        roiBoxes = findQuadCandidates(thresholdFrame(quickGrayFrame(roiFrame)));
      }
      std::vector<cv::Mat> roiHeavyVariants = {
          roiPrep,
          sharpenFrame(roiPrep),
          thresholdFrame(roiPrep),
          otsuFrame(roiPrep),
      };
      for (auto& decoded : decodeVariants(roiHeavyVariants, detector, zxingOptions, true)) {
        if (!decoded.payload.empty()) {
          payloads.push_back(decoded.payload);
        }
      }
      if (payloads.empty() && !roiBoxes.empty()) {
        for (auto& decoded : decodeRoiCandidates(roiFrame, quickGrayFrame(roiFrame), roiBoxes, detector, zxingOptions)) {
          if (!decoded.payload.empty()) {
            payloads.push_back(decoded.payload);
          }
        }
      }
      if (payloads.empty() && !roiBoxes.empty()) {
        for (auto& decoded : decodeWarpedCandidates(roiFrame, roiBoxes, detector, zxingOptions)) {
          if (!decoded.payload.empty()) {
            payloads.push_back(decoded.payload);
          }
        }
      }
      if (!payloads.empty()) {
        boxes.push_back(roiBox);
        return {payloads, boxes};
      }
    }
  }

  std::vector<cv::Mat> fullFastVariants = {
      frame,
      quickGrayFrame(frame),
  };
  for (auto& decoded : decodeVariants(fullFastVariants, detector, zxingOptions, false)) {
    if (!decoded.payload.empty()) {
      payloads.push_back(decoded.payload);
      if (!decoded.points.empty()) {
        boxes.push_back(decoded.points);
      }
    }
  }
  if (!payloads.empty() || !useHeavyPass) {
    if (boxes.empty()) {
      boxes = findDisplayBoxes(frame, detector);
    }
    return {payloads, boxes};
  }

  // If the colored border is already locked, prefer staying on the ROI path.
  // Full-frame heavy recovery is much more expensive and can make the live view stall.
  if (borderState.locked) {
    if (boxes.empty() && !roiBox.empty()) {
      boxes.push_back(roiBox);
    }
    return {payloads, boxes};
  }

  cv::Mat quickGray = quickGrayFrame(frame);
  std::vector<std::vector<int>> detectBoxes = findQuadCandidates(quickGray);
  if (detectBoxes.empty()) {
    detectBoxes = findQuadCandidates(thresholdFrame(quickGray));
  }
  if (detectBoxes.empty()) {
    detectBoxes = detectPointsOnly(frame, detector);
  }

  cv::Mat prep = preprocessFrame(frame);
  std::vector<cv::Mat> fullHeavyVariants = {
      prep,
      sharpenFrame(prep),
      thresholdFrame(prep),
      otsuFrame(prep),
  };
  for (auto& decoded : decodeVariants(fullHeavyVariants, detector, zxingOptions, true)) {
    if (!decoded.payload.empty()) {
      payloads.push_back(decoded.payload);
    }
  }
  if (payloads.empty() && !detectBoxes.empty()) {
    for (auto& decoded : decodeRoiCandidates(frame, quickGray, detectBoxes, detector, zxingOptions)) {
      if (!decoded.payload.empty()) {
        payloads.push_back(decoded.payload);
      }
    }
  }
  if (payloads.empty() && !detectBoxes.empty()) {
    for (auto& decoded : decodeWarpedCandidates(frame, detectBoxes, detector, zxingOptions)) {
      if (!decoded.payload.empty()) {
        payloads.push_back(decoded.payload);
      }
    }
  }
  if (boxes.empty()) {
    boxes = detectBoxes;
  }
  return {payloads, boxes};
}

void drawBoxes(cv::Mat& frame, const std::vector<std::vector<int>>& boxes) {
  for (const auto& pts : boxes) {
    if (pts.size() < 8) {
      continue;
    }
    std::vector<cv::Point> points;
    for (std::size_t i = 0; i + 1 < pts.size(); i += 2) {
      points.emplace_back(pts[i], pts[i + 1]);
    }
    for (std::size_t i = 0; i < points.size(); ++i) {
      cv::line(frame, points[i], points[(i + 1) % points.size()], cv::Scalar(0, 255, 0), 2);
    }
  }
}

cv::VideoCapture openCamera(int index) {
  cv::VideoCapture cap(index, cv::CAP_DSHOW);
  if (!cap.isOpened()) {
    throw std::runtime_error("Cannot open camera index " + std::to_string(index));
  }
  cap.set(cv::CAP_PROP_FRAME_WIDTH, kMinCameraWidth);
  cap.set(cv::CAP_PROP_FRAME_HEIGHT, kMinCameraHeight);
  cap.set(cv::CAP_PROP_FPS, 60);
  cap.set(cv::CAP_PROP_BUFFERSIZE, 1);
  cap.set(cv::CAP_PROP_AUTOFOCUS, 1);
  cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
  return cap;
}

std::optional<cv::Mat> readLatestFrame(cv::VideoCapture& cap) {
  for (int i = 0; i < kMaxFrameGrabs; ++i) {
    if (!cap.grab()) {
      break;
    }
  }
  cv::Mat frame;
  if (!cap.read(frame) || frame.empty()) {
    return std::nullopt;
  }
  return frame;
}

Config parseArgs(int argc, char** argv, Config config) {
  std::vector<std::string> args(argv + 1, argv + argc);
  for (std::size_t i = 0; i < args.size(); ++i) {
    const std::string& arg = args[i];
    auto requireValue = [&](const std::string& flag) -> std::string {
      if (i + 1 >= args.size()) {
        throw std::runtime_error("Missing value for " + flag);
      }
      return args[++i];
    };
    if (arg == "--output") {
      config.outputDir = requireValue(arg);
    } else if (arg == "--unique") {
      config.unique = true;
    } else if (arg == "--esp32-port") {
      config.esp32Port = std::stoi(requireValue(arg));
    } else if (arg == "--esp32-host") {
      config.esp32Host = requireValue(arg);
    } else if (arg == "--connect-timeout") {
      config.connectTimeoutSeconds = std::stod(requireValue(arg));
    } else if (!arg.empty() && arg[0] != '-') {
      config.cameraIndex = std::stoi(arg);
    } else {
      throw std::runtime_error("Unknown argument: " + arg);
    }
  }
  return config;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    SetConsoleCtrlHandler(consoleCtrlHandler, TRUE);
    WinsockInit winsock;
    const fs::path executableDir = getExecutablePath().parent_path();
    const Config config = parseArgs(argc, argv, loadConfigFromExecutableDir(executableDir));
    fs::create_directories(config.outputDir);
    cv::QRCodeDetector detector = makeDetector();
    ZXing::ReaderOptions zxingOptions = makeZXingOptions();

    cv::VideoCapture cap = openCamera(config.cameraIndex);
    SpaceSender spaceSender(config.esp32Host, config.esp32Port, config.connectTimeoutSeconds);
    LatestFrame latestFrame;
    std::atomic_bool captureStop = false;
    std::thread captureThread([&]() {
      while (!captureStop.load() && !g_stopRequested.load()) {
        auto maybeFrame = readLatestFrame(cap);
        if (!maybeFrame.has_value()) {
          Sleep(5);
          continue;
        }
        std::lock_guard<std::mutex> lock(latestFrame.mutex);
        latestFrame.frame = maybeFrame->clone();
        ++latestFrame.sequence;
      }
    });

    std::cout << "Opening camera #" << config.cameraIndex
              << " with output dir " << config.outputDir.string()
              << ". Press Ctrl-C to quit.\n";

    std::set<std::string> seenPayloads;
    std::unordered_map<std::string, Clock::time_point> lastSeen;
    std::set<std::string> processedPageIds;
    std::map<std::pair<std::string, std::string>, TransferBuffer> transfers;
    std::set<std::pair<std::string, std::string>> completed;
    std::unordered_map<std::string, SessionState> sessions;
    BorderState borderState;
    SessionGate sessionGate;
    std::string waitingNotice;
    std::string lastAcceptedPageId;
    int lastAcceptedSessionSeq = 0;
    bool hasLastAcceptedSessionSeq = false;
    Clock::time_point lastSpaceSentAt {};
    Clock::time_point lastNavigationSentAt {};

    int frameCount = 0;
    int consecutiveMisses = 0;
    std::uint64_t lastProcessedSequence = 0;

    while (!g_stopRequested.load()) {
      cv::Mat frame;
      std::uint64_t currentSequence = 0;
      {
        std::lock_guard<std::mutex> lock(latestFrame.mutex);
        currentSequence = latestFrame.sequence;
        if (currentSequence != 0 && currentSequence != lastProcessedSequence && !latestFrame.frame.empty()) {
          frame = latestFrame.frame.clone();
          lastProcessedSequence = currentSequence;
        }
      }
      if (frame.empty()) {
        cv::waitKey(1);
        Sleep(2);
        continue;
      }
      ++frameCount;

      const bool useHeavyPass =
          consecutiveMisses >= kHeavyPassMissThreshold &&
          (frameCount % kHeavyPassInterval == 0);
      auto [decodedPayloads, boxes] = detectQr(frame, useHeavyPass, borderState, detector, zxingOptions);
      if (decodedPayloads.empty()) {
        ++consecutiveMisses;
      } else {
        consecutiveMisses = 0;
      }

      for (const auto& payload : decodedPayloads) {
        if (g_stopRequested.load()) {
          break;
        }
        auto message = parseProtocolPayload(payload);
        if (!message.has_value()) {
          continue;
        }
        auto noteIgnoredFrame = [&](const std::string& notice) {
          if (notice != waitingNotice) {
            std::cout << notice << "\n";
            waitingNotice = notice;
          }
        };
        auto noteSessionMismatch = [&](const std::string& notice) {
          std::cout << notice << "\n";
          waitingNotice = notice;
        };
        const std::string frameType = message->value("t", "");
        const std::string sessionId = message->value("s", "");
        if (frameType == "session_start") {
          if (!hasValidRoutingFields(*message)) {
            noteIgnoredFrame("Ignored malformed SESSION_START frame");
            continue;
          }
          if (sessionGate.primed && sessionGate.sessionId != sessionId) {
            noteSessionMismatch("Ignored SESSION_START for unexpected session " + sessionId +
                                " while waiting on " + sessionGate.sessionId);
            continue;
          }
          const bool newSession = !sessionGate.primed;
          if (newSession) {
            resetSessionTracking(seenPayloads, lastSeen, processedPageIds,
                                 transfers, completed, sessions, borderState,
                                 lastAcceptedPageId, lastAcceptedSessionSeq,
                                 hasLastAcceptedSessionSeq);
          }
          sessionGate.primed = true;
          sessionGate.sessionId = sessionId;
          waitingNotice.clear();
        } else if (!sessionGate.primed) {
          noteIgnoredFrame("Waiting for SESSION_START. Ignored frame type: " + frameType);
          continue;
        } else if (sessionId.empty() || sessionId != sessionGate.sessionId) {
          noteSessionMismatch("Ignored frame from unexpected session " +
                              (sessionId.empty() ? std::string("<missing>") : sessionId) +
                              " while waiting on " + sessionGate.sessionId);
          continue;
        } else if (!hasValidRoutingFields(*message)) {
          noteIgnoredFrame("Ignored malformed frame in session " + sessionGate.sessionId +
                           " type " + frameType);
          continue;
        }
        waitingNotice.clear();
        const std::string pageId = message->value("g", "");
        const int sessionSeq = message->value("si", 0);
        const int sessionSeqTotal = message->value("sc", 0);
        auto now = Clock::now();
        if (!pageId.empty()) {
          if (!lastAcceptedPageId.empty() && pageId == lastAcceptedPageId) {
            if (std::chrono::duration<double>(now - lastSpaceSentAt).count() >=
                kResendSameFrameSeconds) {
              if (spaceSender.sendSpace()) {
                std::cout << "Resent SPACE for unchanged frame " << pageId;
                if (sessionSeq > 0 && sessionSeqTotal > 0) {
                  std::cout << " (" << sessionSeq << "/" << sessionSeqTotal << ")";
                }
                std::cout << "\n";
                std::cout << spaceSender.frameStatusMessage() << "\n";
                lastSpaceSentAt = Clock::now();
              } else {
                std::cout << spaceSender.frameStatusMessage() << "\n";
              }
            }
            continue;
          }
          if (hasLastAcceptedSessionSeq && sessionSeq > 0) {
            const int expectedSeq = lastAcceptedSessionSeq + 1;
            if (sessionSeq > expectedSeq) {
              if (std::chrono::duration<double>(now - lastNavigationSentAt).count() >=
                  kNavigationCooldownSeconds) {
                if (spaceSender.sendLeft()) {
                  std::cout << "Sent LEFT for skipped frame sequence " << sessionSeq
                            << " while expecting " << expectedSeq << "\n";
                  std::cout << spaceSender.frameStatusMessage() << "\n";
                  lastNavigationSentAt = Clock::now();
                } else {
                  std::cout << spaceSender.frameStatusMessage() << "\n";
                }
              }
              continue;
            }
            if (sessionSeq < expectedSeq) {
              if (std::chrono::duration<double>(now - lastNavigationSentAt).count() >=
                  kNavigationCooldownSeconds) {
                if (spaceSender.sendRight()) {
                  std::cout << "Sent RIGHT for backward frame sequence " << sessionSeq
                            << " while expecting " << expectedSeq << "\n";
                  std::cout << spaceSender.frameStatusMessage() << "\n";
                  lastNavigationSentAt = Clock::now();
                } else {
                  std::cout << spaceSender.frameStatusMessage() << "\n";
                }
              }
              continue;
            }
          }
          if (processedPageIds.contains(pageId)) {
            continue;
          }
        } else {
          if (config.unique && seenPayloads.contains(payload)) {
            continue;
          }
          auto it = lastSeen.find(payload);
          if (it != lastSeen.end() &&
              std::chrono::duration<double>(now - it->second).count() < kRepeatDelaySeconds) {
            continue;
          }
        }

        ProcessResult processResult;
        try {
          processResult = processProtocolMessage(*message, config.outputDir, transfers, completed, sessions);
        } catch (const std::exception& ex) {
          processResult.message = std::string("Error processing transfer frame: ") + ex.what();
          processResult.ackAdvance = false;
        }

        if (processResult.message.has_value()) {
          std::cout << *processResult.message << "\n";
        }
        if (!processResult.ackAdvance) {
          break;
        }

        if (!spaceSender.sendSpace()) {
          std::cout << spaceSender.frameStatusMessage() << "\n";
          continue;
        }

        std::cout << spaceSender.frameStatusMessage() << "\n";
        Sleep(kSpaceTypeSettleMs);
        beep();
        lastSpaceSentAt = Clock::now();
        lastNavigationSentAt = lastSpaceSentAt;

        if (!pageId.empty()) {
          processedPageIds.insert(pageId);
          lastAcceptedPageId = pageId;
          if (sessionSeq > 0) {
            lastAcceptedSessionSeq = sessionSeq;
            hasLastAcceptedSessionSeq = true;
          }
        } else {
          lastSeen[payload] = Clock::now();
          seenPayloads.insert(payload);
        }
        if (processResult.sessionComplete) {
          g_stopRequested = true;
        }
        break;
      }

      drawBoxes(frame, boxes);
      cv::imshow(kAppName, frame);
      cv::waitKey(1);
    }

    captureStop = true;
    if (captureThread.joinable()) {
      captureThread.join();
    }
    cap.release();
    cv::destroyAllWindows();
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << kAppName << " error: " << ex.what() << "\n";
    return 1;
  }
}
