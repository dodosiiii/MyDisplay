#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <shellapi.h>

#include "../../Common/Protocol/FrameDatagram.h"
#include "../../Common/Video/H264Codec.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "Ws2_32.lib")

namespace
{
constexpr wchar_t kWindowClass[] = L"MyDisplayClientWindow";
constexpr UINT_PTR kPaintTimerId = 1;
constexpr UINT kFrameAvailableMessage = WM_APP + 1;
constexpr UINT kPaintIntervalMilliseconds = 250;

struct VideoFrame
{
    std::uint64_t id;
    std::uint16_t width;
    std::uint16_t height;
    std::vector<std::uint8_t> pixels;
};

struct FrameAssembly
{
    std::uint64_t id = 0;
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    std::uint16_t pixelFormat = 0;
    std::uint16_t fragmentCount = 0;
    std::vector<std::vector<std::uint8_t>> fragments;
    std::vector<bool> received;
    std::size_t receivedBytes = 0;
    std::uint16_t receivedFragments = 0;
    bool active = false;
};

SOCKET g_socket = INVALID_SOCKET;
std::atomic<HWND> g_window = nullptr;
std::atomic<bool> g_stopReceiver = false;
std::atomic<std::uint64_t> g_receivedFrames = 0;
std::atomic<std::uint64_t> g_droppedFrames = 0;
std::atomic<std::uint64_t> g_receivedBytes = 0;
std::mutex g_frameMutex;
std::shared_ptr<const VideoFrame> g_latestFrame;
bool g_showStats = false;
bool g_fullscreen = false;
DWORD g_fpsWindowStart = 0;
std::uint64_t g_fpsWindowFrames = 0;
double g_renderFps = 0.0;
std::uint64_t g_lastPaintedFrameId = 0;
DWORD g_networkWindowStart = 0;
std::uint64_t g_networkWindowBytes = 0;
double g_networkMbps = 0.0;
WINDOWPLACEMENT g_previousPlacement{ sizeof(WINDOWPLACEMENT) };
DWORD g_previousStyle = 0;

void PublishVideoFrame(std::uint64_t id, std::uint16_t width, std::uint16_t height,
                       std::vector<std::uint8_t>&& pixels)
{
    const std::size_t expectedBytes = static_cast<std::size_t>(width) * height * 4;
    if (pixels.size() != expectedBytes)
    {
        g_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    auto frame = std::make_shared<VideoFrame>();
    frame->id = id;
    frame->width = width;
    frame->height = height;
    frame->pixels = std::move(pixels);

    bool published = false;
    {
        std::lock_guard<std::mutex> lock(g_frameMutex);
        if (!g_latestFrame || frame->id > g_latestFrame->id)
        {
            g_latestFrame = std::move(frame);
            g_receivedFrames.fetch_add(1, std::memory_order_relaxed);
            published = true;
        }
    }
    const HWND window = g_window.load(std::memory_order_acquire);
    if (published && window)
    {
        PostMessageW(window, kFrameAvailableMessage, 0, 0);
    }
}

void PublishFrame(FrameAssembly& assembly,
                  std::unique_ptr<mydisplay::video::H264Decoder>& decoder,
                  std::uint16_t& decoderWidth, std::uint16_t& decoderHeight)
{
    std::vector<std::uint8_t> frameBytes;
    frameBytes.reserve(assembly.receivedBytes);
    for (const auto& fragment : assembly.fragments)
    {
        frameBytes.insert(frameBytes.end(), fragment.begin(), fragment.end());
    }

    if (assembly.pixelFormat == mydisplay::protocol::kPixelFormatBgra8)
    {
        PublishVideoFrame(assembly.id, assembly.width, assembly.height, std::move(frameBytes));
        return;
    }
    if (assembly.pixelFormat != mydisplay::protocol::kPixelFormatH264 || frameBytes.empty() ||
        frameBytes.size() > mydisplay::protocol::kMaxEncodedFrameBytes)
    {
        g_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    if (!decoder || decoderWidth != assembly.width || decoderHeight != assembly.height)
    {
        decoder = std::make_unique<mydisplay::video::H264Decoder>();
        if (!decoder->Initialize(assembly.width, assembly.height, 60))
        {
            decoder.reset();
            g_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        decoderWidth = assembly.width;
        decoderHeight = assembly.height;
    }

    std::vector<mydisplay::video::DecodedFrame> decodedFrames;
    if (!decoder->Decode(frameBytes.data(), frameBytes.size(), assembly.id, decodedFrames))
    {
        g_droppedFrames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    for (auto& decodedFrame : decodedFrames)
    {
        PublishVideoFrame(decodedFrame.frameId, assembly.width, assembly.height,
                          std::move(decodedFrame.bgra));
    }
}

void ReceiveFrames()
{
    FrameAssembly assembly;
    std::unique_ptr<mydisplay::video::H264Decoder> decoder;
    std::uint16_t decoderWidth = 0;
    std::uint16_t decoderHeight = 0;
    std::vector<char> datagram(mydisplay::protocol::kMaxUdpDatagramBytes);

    while (!g_stopReceiver.load(std::memory_order_relaxed))
    {
        const int bytesReceived = recvfrom(g_socket, datagram.data(), static_cast<int>(datagram.size()), 0, nullptr, nullptr);
        if (bytesReceived == SOCKET_ERROR)
        {
            if (g_stopReceiver.load(std::memory_order_relaxed))
            {
                return;
            }
            const int socketError = WSAGetLastError();
            if (socketError == WSAETIMEDOUT || socketError == WSAEWOULDBLOCK)
            {
                continue;
            }
            g_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (bytesReceived < static_cast<int>(sizeof(mydisplay::protocol::FrameDatagramHeader)))
        {
            continue;
        }

        mydisplay::protocol::FrameDatagramHeader header{};
        std::memcpy(&header, datagram.data(), sizeof(header));
        if (!mydisplay::protocol::IsValid(header, static_cast<std::size_t>(bytesReceived)))
        {
            continue;
        }

        if (!assembly.active || header.frameId > assembly.id)
        {
            if (assembly.active && assembly.receivedFragments != assembly.fragmentCount)
            {
                g_droppedFrames.fetch_add(1, std::memory_order_relaxed);
            }
            assembly = {};
            assembly.active = true;
            assembly.id = header.frameId;
            assembly.width = header.width;
            assembly.height = header.height;
            assembly.pixelFormat = header.pixelFormat;
            assembly.fragmentCount = header.fragmentCount;
            assembly.fragments.resize(header.fragmentCount);
            assembly.received.resize(header.fragmentCount, false);
        }
        else if (header.frameId < assembly.id)
        {
            continue;
        }

        if (header.width != assembly.width || header.height != assembly.height ||
            header.pixelFormat != assembly.pixelFormat ||
            header.fragmentCount != assembly.fragmentCount || assembly.received[header.fragmentIndex])
        {
            continue;
        }

        if (assembly.pixelFormat == mydisplay::protocol::kPixelFormatBgra8)
        {
            const std::size_t fragmentOffset = static_cast<std::size_t>(header.fragmentIndex) *
                                               mydisplay::protocol::kMaxFragmentPayloadBytes;
            const std::size_t expectedFrameBytes = static_cast<std::size_t>(header.width) * header.height * 4;
            const std::size_t expectedPayloadBytes = (std::min)(
                mydisplay::protocol::kMaxFragmentPayloadBytes, expectedFrameBytes - fragmentOffset);
            if (header.payloadBytes != expectedPayloadBytes)
            {
                continue;
            }
        }
        else if ((header.fragmentIndex + 1 < header.fragmentCount &&
                  header.payloadBytes != mydisplay::protocol::kMaxFragmentPayloadBytes) ||
                 assembly.receivedBytes + header.payloadBytes > mydisplay::protocol::kMaxEncodedFrameBytes)
        {
            continue;
        }

        const char* payload = datagram.data() + sizeof(header);
        auto& fragment = assembly.fragments[header.fragmentIndex];
        fragment.resize(header.payloadBytes);
        std::memcpy(fragment.data(), payload, header.payloadBytes);
        assembly.received[header.fragmentIndex] = true;
        assembly.receivedBytes += header.payloadBytes;
        ++assembly.receivedFragments;
        g_receivedBytes.fetch_add(static_cast<std::uint64_t>(bytesReceived), std::memory_order_relaxed);

        if (assembly.receivedFragments == assembly.fragmentCount)
        {
            PublishFrame(assembly, decoder, decoderWidth, decoderHeight);
            assembly.active = false;
        }
    }
}

std::shared_ptr<const VideoFrame> GetLatestFrame()
{
    std::lock_guard<std::mutex> lock(g_frameMutex);
    return g_latestFrame;
}

void ToggleFullscreen(HWND window)
{
    if (!g_fullscreen)
    {
        g_previousStyle = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
        GetWindowPlacement(window, &g_previousPlacement);
        MONITORINFO monitorInfo{ sizeof(MONITORINFO) };
        GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitorInfo);
        SetWindowLongPtrW(window, GWL_STYLE, g_previousStyle & ~WS_OVERLAPPEDWINDOW);
        SetWindowPos(window, HWND_TOP, monitorInfo.rcMonitor.left, monitorInfo.rcMonitor.top,
                     monitorInfo.rcMonitor.right - monitorInfo.rcMonitor.left,
                     monitorInfo.rcMonitor.bottom - monitorInfo.rcMonitor.top,
                     SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = true;
    }
    else
    {
        SetWindowLongPtrW(window, GWL_STYLE, g_previousStyle);
        SetWindowPlacement(window, &g_previousPlacement);
        SetWindowPos(window, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        g_fullscreen = false;
    }
}

void UpdateRenderFps(const VideoFrame& frame)
{
    if (frame.id == g_lastPaintedFrameId)
    {
        return;
    }

    g_lastPaintedFrameId = frame.id;
    ++g_fpsWindowFrames;
    const DWORD now = GetTickCount();
    if (g_fpsWindowStart == 0)
    {
        g_fpsWindowStart = now;
        return;
    }

    const DWORD elapsed = now - g_fpsWindowStart;
    if (elapsed >= 1000)
    {
        g_renderFps = static_cast<double>(g_fpsWindowFrames) * 1000.0 / elapsed;
        g_fpsWindowFrames = 0;
        g_fpsWindowStart = now;
    }
}

void DrawStats(HDC deviceContext, const VideoFrame* frame)
{
    const std::uint64_t receivedBytes = g_receivedBytes.exchange(0, std::memory_order_relaxed);
    g_networkWindowBytes += receivedBytes;
    const DWORD now = GetTickCount();
    if (g_networkWindowStart == 0)
    {
        g_networkWindowStart = now;
    }
    const DWORD elapsed = now - g_networkWindowStart;
    if (elapsed >= 1000)
    {
        g_networkMbps = static_cast<double>(g_networkWindowBytes) * 8.0 / (elapsed * 1000.0);
        g_networkWindowBytes = 0;
        g_networkWindowStart = now;
    }
    const std::uint64_t droppedFrames = g_droppedFrames.load(std::memory_order_relaxed);

    wchar_t text[256]{};
    if (frame)
    {
        swprintf_s(text, L"Images/s affichees : %.1f\nResolution : %ux%u\nDebit reseau : %.2f Mbit/s\nImages recues : %llu  Perdues : %llu",
                   g_renderFps, frame->width, frame->height, g_networkMbps,
                   static_cast<unsigned long long>(g_receivedFrames.load(std::memory_order_relaxed)),
                   static_cast<unsigned long long>(droppedFrames));
    }
    else
    {
        swprintf_s(text, L"En attente du flux video...\nImages perdues : %llu",
                   static_cast<unsigned long long>(droppedFrames));
    }

    RECT panel{ 12, 12, 330, 116 };
    HBRUSH background = CreateSolidBrush(RGB(12, 18, 24));
    FillRect(deviceContext, &panel, background);
    DeleteObject(background);
    SetBkMode(deviceContext, TRANSPARENT);
    SetTextColor(deviceContext, RGB(245, 248, 250));
    panel.left += 10;
    panel.top += 8;
    DrawTextW(deviceContext, text, -1, &panel, DT_LEFT | DT_TOP | DT_NOPREFIX);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case kFrameAvailableMessage:
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    case WM_KEYDOWN:
        if (wParam == 'F' && (lParam & (1LL << 30)) == 0)
        {
            g_showStats = !g_showStats;
            InvalidateRect(window, nullptr, FALSE);
            return 0;
        }
        if (wParam == VK_F11 && (lParam & (1LL << 30)) == 0)
        {
            ToggleFullscreen(window);
            return 0;
        }
        if (wParam == VK_ESCAPE && g_fullscreen)
        {
            ToggleFullscreen(window);
            return 0;
        }
        if (wParam == VK_ESCAPE)
        {
            DestroyWindow(window);
            return 0;
        }
        break;
    case WM_PAINT:
    {
        PAINTSTRUCT paint{};
        HDC deviceContext = BeginPaint(window, &paint);
        RECT client{};
        GetClientRect(window, &client);
        FillRect(deviceContext, &client, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));

        const auto frame = GetLatestFrame();
        if (frame)
        {
            UpdateRenderFps(*frame);
            const int clientWidth = client.right - client.left;
            const int clientHeight = client.bottom - client.top;
            const double scale = (std::min)(static_cast<double>(clientWidth) / frame->width,
                                            static_cast<double>(clientHeight) / frame->height);
            const int targetWidth = static_cast<int>(frame->width * scale);
            const int targetHeight = static_cast<int>(frame->height * scale);
            const int targetX = (clientWidth - targetWidth) / 2;
            const int targetY = (clientHeight - targetHeight) / 2;

            BITMAPINFO bitmapInfo{};
            bitmapInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bitmapInfo.bmiHeader.biWidth = frame->width;
            bitmapInfo.bmiHeader.biHeight = -static_cast<LONG>(frame->height);
            bitmapInfo.bmiHeader.biPlanes = 1;
            bitmapInfo.bmiHeader.biBitCount = 32;
            bitmapInfo.bmiHeader.biCompression = BI_RGB;

            SetStretchBltMode(deviceContext, COLORONCOLOR);
            StretchDIBits(deviceContext, targetX, targetY, targetWidth, targetHeight,
                          0, 0, frame->width, frame->height, frame->pixels.data(),
                          &bitmapInfo, DIB_RGB_COLORS, SRCCOPY);
        }
        else
        {
            SetBkMode(deviceContext, TRANSPARENT);
            SetTextColor(deviceContext, RGB(190, 202, 212));
            DrawTextW(deviceContext, L"MyDisplay\nEn attente du flux video...\nF : statistiques   F11 : plein ecran",
                      -1, &client, DT_CENTER | DT_VCENTER | DT_NOPREFIX);
        }

        if (g_showStats)
        {
            DrawStats(deviceContext, frame.get());
        }
        EndPaint(window, &paint);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(window, kPaintTimerId);
        g_stopReceiver.store(true, std::memory_order_relaxed);
        if (g_socket != INVALID_SOCKET)
        {
            closesocket(g_socket);
            g_socket = INVALID_SOCKET;
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    int argumentCount = 0;
    LPWSTR* arguments = CommandLineToArgvW(GetCommandLineW(), &argumentCount);
    if (!arguments)
    {
        MessageBoxW(nullptr, L"Could not parse command-line arguments.", L"MyDisplay Client", MB_ICONERROR);
        return 1;
    }
    const wchar_t* bindAddress = argumentCount > 1 ? arguments[1] : L"127.0.0.1";
    const unsigned long portValue = argumentCount > 2 ? std::wcstoul(arguments[2], nullptr, 10) : 48000;
    if (portValue == 0 || portValue > 65535)
    {
        MessageBoxW(nullptr, L"Port must be between 1 and 65535.", L"MyDisplay Client", MB_ICONERROR);
        LocalFree(arguments);
        return 2;
    }

    WSADATA winsockData{};
    int result = WSAStartup(MAKEWORD(2, 2), &winsockData);
    if (result != 0)
    {
        MessageBoxW(nullptr, L"Winsock initialization failed.", L"MyDisplay Client", MB_ICONERROR);
        LocalFree(arguments);
        return 1;
    }

    g_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_socket == INVALID_SOCKET)
    {
        MessageBoxW(nullptr, L"Could not create the UDP socket.", L"MyDisplay Client", MB_ICONERROR);
        WSACleanup();
        LocalFree(arguments);
        return 1;
    }

    const int receiveBufferSize = 2 * 1024 * 1024;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<const char*>(&receiveBufferSize), sizeof(receiveBufferSize));
    const DWORD receiveTimeout = 250;
    setsockopt(g_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receiveTimeout), sizeof(receiveTimeout));

    sockaddr_in localAddress{};
    localAddress.sin_family = AF_INET;
    localAddress.sin_port = htons(static_cast<u_short>(portValue));
    if (InetPtonW(AF_INET, bindAddress, &localAddress.sin_addr) != 1 ||
        bind(g_socket, reinterpret_cast<const sockaddr*>(&localAddress), sizeof(localAddress)) == SOCKET_ERROR)
    {
        wchar_t errorText[128]{};
        swprintf_s(errorText, L"Could not bind to %s:%lu (Winsock %d).", bindAddress, portValue, WSAGetLastError());
        MessageBoxW(nullptr, errorText, L"MyDisplay Client", MB_ICONERROR);
        closesocket(g_socket);
        WSACleanup();
        LocalFree(arguments);
        return 1;
    }
    LocalFree(arguments);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    if (!RegisterClassExW(&windowClass))
    {
        closesocket(g_socket);
        WSACleanup();
        MessageBoxW(nullptr, L"Could not register the client window.", L"MyDisplay Client", MB_ICONERROR);
        return 1;
    }

    HWND window = CreateWindowExW(0, kWindowClass, L"MyDisplay | Ecran distant | F : statistiques | F11 : plein ecran | Echap : quitter",
                                  WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 650,
                                  nullptr, nullptr, instance, nullptr);
    if (!window)
    {
        closesocket(g_socket);
        WSACleanup();
        MessageBoxW(nullptr, L"Could not create the client window.", L"MyDisplay Client", MB_ICONERROR);
        return 1;
    }

    g_fpsWindowStart = GetTickCount();
    g_networkWindowStart = g_fpsWindowStart;
    SetTimer(window, kPaintTimerId, kPaintIntervalMilliseconds, nullptr);
    g_window.store(window, std::memory_order_release);
    std::thread receiver(ReceiveFrames);
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    g_stopReceiver.store(true, std::memory_order_relaxed);
    g_window.store(nullptr, std::memory_order_release);
    if (g_socket != INVALID_SOCKET)
    {
        closesocket(g_socket);
        g_socket = INVALID_SOCKET;
    }
    if (receiver.joinable())
    {
        receiver.join();
    }
    WSACleanup();
    return static_cast<int>(message.wParam);
}