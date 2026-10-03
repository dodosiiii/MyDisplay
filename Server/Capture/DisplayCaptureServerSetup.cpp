#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <bcrypt.h>

#include <array>
#include <cstdint>
#include <string>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Crypt32.lib")
#pragma comment(lib, "Bcrypt.lib")
#pragma comment(lib, "Shell32.lib")

namespace
{
constexpr wchar_t kWindowClass[] = L"MyDisplayServerSetupWindow";
constexpr int kAddressId = 101;
constexpr int kPortId = 102;
constexpr int kRefreshId = 103;
constexpr int kCreateCertificateId = 104;
constexpr int kPackageClientId = 105;
constexpr int kInstallDriverId = 106;
constexpr int kStartId = 107;
constexpr int kStopId = 108;
constexpr int kCopyPinId = 109;
constexpr UINT_PTR kPollTimerId = 1;

HWND g_window = nullptr;
HWND g_addressInput = nullptr;
HWND g_certificateStatus = nullptr;
HWND g_displayStatus = nullptr;
HWND g_pinLabel = nullptr;
HWND g_statusLabel = nullptr;
HANDLE g_serverProcess = nullptr;
HANDLE g_serverOutput = nullptr;
std::wstring g_serverDirectory;
std::wstring g_workspaceRoot;
std::string g_outputBuffer;
std::wstring g_sessionPin;
bool g_prerequisitesReady = false;

std::wstring GetWindowTextValue(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

HWND AddLabel(HINSTANCE instance, const wchar_t* text, int x, int y, int width, int height)
{
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           x, y, width, height, g_window, nullptr, instance, nullptr);
}

HWND AddButton(HINSTANCE instance, int identifier, const wchar_t* text,
               int x, int y, int width, int height, DWORD style = 0)
{
    return CreateWindowExW(0, L"BUTTON", text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                           x, y, width, height, g_window,
                           reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)), instance, nullptr);
}

std::wstring GetLocalComputerName()
{
    std::array<wchar_t, MAX_COMPUTERNAME_LENGTH + 1> name{};
    DWORD length = static_cast<DWORD>(name.size());
    if (!GetComputerNameW(name.data(), &length))
    {
        return {};
    }
    return std::wstring(name.data(), length);
}

bool HasServerCertificate()
{
    const std::wstring computerName = GetLocalComputerName();
    if (computerName.empty())
    {
        return false;
    }
    const std::wstring subject = L"MyDisplayServer-" + computerName;
    HCERTSTORE store = CertOpenSystemStoreW(0, L"MY");
    if (!store)
    {
        return false;
    }
    PCCERT_CONTEXT certificate = CertFindCertificateInStore(
        store, X509_ASN_ENCODING | PKCS_7_ASN_ENCODING, 0,
        CERT_FIND_SUBJECT_STR_W, subject.c_str(), nullptr);
    if (certificate)
    {
        CertFreeCertificateContext(certificate);
    }
    CertCloseStore(store, 0);
    return certificate != nullptr;
}

bool HasVirtualDisplay()
{
    for (DWORD displayIndex = 0;; ++displayIndex)
    {
        DISPLAY_DEVICEW display{};
        display.cb = sizeof(display);
        if (!EnumDisplayDevicesW(nullptr, displayIndex, &display, 0))
        {
            break;
        }
        if ((display.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0)
        {
            continue;
        }
        for (DWORD monitorIndex = 0;; ++monitorIndex)
        {
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            if (!EnumDisplayDevicesW(display.DeviceName, monitorIndex, &monitor,
                                     EDD_GET_DEVICE_INTERFACE_NAME))
            {
                break;
            }
            if (wcsstr(monitor.DeviceID, L"DELD0E6") != nullptr)
            {
                return true;
            }
        }
    }
    return false;
}

std::wstring GetLocalIpv4()
{
    char hostName[256]{};
    if (gethostname(hostName, sizeof(hostName)) == SOCKET_ERROR)
    {
        return L"127.0.0.1";
    }
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (getaddrinfo(hostName, nullptr, &hints, &addresses) != 0)
    {
        return L"127.0.0.1";
    }

    std::wstring result = L"127.0.0.1";
    for (addrinfo* address = addresses; address; address = address->ai_next)
    {
        const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(address->ai_addr);
        if ((ntohl(ipv4->sin_addr.s_addr) & 0xFF000000) != 0x7F000000)
        {
            std::array<wchar_t, INET_ADDRSTRLEN> text{};
            if (InetNtopW(AF_INET, const_cast<IN_ADDR*>(&ipv4->sin_addr), text.data(),
                          static_cast<DWORD>(text.size())))
            {
                result = text.data();
                break;
            }
        }
    }
    freeaddrinfo(addresses);
    return result;
}

std::wstring CreateSessionPin()
{
    std::uint32_t randomValue = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&randomValue), sizeof(randomValue),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    {
        return {};
    }
    std::wstring pin = std::to_wstring(randomValue % 1000000);
    pin.insert(pin.begin(), 6 - pin.length(), L'0');
    return pin;
}

void UpdatePrerequisites()
{
    const bool hasCertificate = HasServerCertificate();
    const bool hasDisplay = HasVirtualDisplay();
    SetWindowTextW(g_certificateStatus, hasCertificate
        ? L"Certificat TLS : pret" : L"Certificat TLS : manquant (creer le certificat)");
    SetWindowTextW(g_displayStatus, hasDisplay
        ? L"Ecran virtuel IDD : detecte" : L"Ecran virtuel IDD : absent (installer/activer le pilote)");
    g_prerequisitesReady = hasCertificate && hasDisplay;
    EnableWindow(GetDlgItem(g_window, kStartId), g_prerequisitesReady && !g_serverProcess);
}

bool QuoteArgument(const std::wstring& argument, std::wstring& quoted)
{
    quoted = L"\"";
    for (const wchar_t character : argument)
    {
        if (character == L'\"')
        {
            return false;
        }
        quoted.push_back(character);
    }
    quoted.push_back(L'\"');
    return true;
}

void StartServer()
{
    UpdatePrerequisites();
    if (!g_prerequisitesReady)
    {
        SetWindowTextW(g_statusLabel, L"Complete les prerequis ci-dessus avant de demarrer le serveur.");
        return;
    }
    if (g_serverProcess)
    {
        return;
    }

    const std::wstring address = GetWindowTextValue(g_addressInput);
    in_addr parsedAddress{};
    if (InetPtonW(AF_INET, address.c_str(), &parsedAddress) != 1 ||
        (ntohl(parsedAddress.s_addr) & 0xFF000000) == 0x7F000000)
    {
        SetWindowTextW(g_statusLabel, L"Choisis l'adresse IPv4 du reseau local, pas 127.0.0.1.");
        return;
    }

    const std::wstring sessionPin = CreateSessionPin();
    if (sessionPin.empty())
    {
        SetWindowTextW(g_statusLabel, L"Impossible de generer le code de session.");
        return;
    }

    SECURITY_ATTRIBUTES securityAttributes{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    HANDLE childOutput = nullptr;
    HANDLE parentOutput = nullptr;
    if (!CreatePipe(&parentOutput, &childOutput, &securityAttributes, 0))
    {
        SetWindowTextW(g_statusLabel, L"Impossible de creer le canal de sortie du serveur.");
        return;
    }
    SetHandleInformation(parentOutput, HANDLE_FLAG_INHERIT, 0);
    HANDLE childInput = nullptr;
    HANDLE parentInput = nullptr;
    if (!CreatePipe(&childInput, &parentInput, &securityAttributes, 0))
    {
        CloseHandle(parentOutput);
        CloseHandle(childOutput);
        SetWindowTextW(g_statusLabel, L"Impossible de creer le canal prive du code PIN.");
        return;
    }
    SetHandleInformation(parentInput, HANDLE_FLAG_INHERIT, 0);

    const std::wstring executable = g_serverDirectory + L"\\DisplayCaptureSecureServer.exe";
    std::wstring quotedExecutable;
    if (!QuoteArgument(executable, quotedExecutable))
    {
        CloseHandle(childInput);
        CloseHandle(parentInput);
        CloseHandle(parentOutput);
        CloseHandle(childOutput);
        SetWindowTextW(g_statusLabel, L"Chemin du serveur invalide.");
        return;
    }
    std::wstring commandLine = quotedExecutable + L" \"" + address + L"\" 48001 --pin-stdin";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = childInput;
    startup.hStdOutput = childOutput;
    startup.hStdError = childOutput;
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
                                        TRUE, CREATE_NO_WINDOW, nullptr, g_serverDirectory.c_str(),
                                        &startup, &process);
    CloseHandle(childInput);
    CloseHandle(childOutput);
    if (!created)
    {
        CloseHandle(parentInput);
        CloseHandle(parentOutput);
        SetWindowTextW(g_statusLabel, L"Le serveur securise n'a pas pu demarrer. Verifie le build Release.");
        return;
    }

    CloseHandle(process.hThread);
    std::string pinBytes;
    pinBytes.reserve(sessionPin.size());
    for (const wchar_t digit : sessionPin)
    {
        pinBytes.push_back(static_cast<char>(digit));
    }
    DWORD bytesWritten = 0;
    const BOOL pinSent = WriteFile(parentInput, pinBytes.data(), static_cast<DWORD>(pinBytes.size()),
                                   &bytesWritten, nullptr);
    SecureZeroMemory(pinBytes.data(), pinBytes.size());
    CloseHandle(parentInput);
    if (!pinSent || bytesWritten != sessionPin.size())
    {
        TerminateProcess(process.hProcess, 1);
        CloseHandle(process.hProcess);
        CloseHandle(parentOutput);
        SetWindowTextW(g_statusLabel, L"Le code n'a pas pu etre transmis au serveur.");
        return;
    }

    g_serverProcess = process.hProcess;
    g_serverOutput = parentOutput;
    g_outputBuffer.clear();
    g_sessionPin = sessionPin;
    SetWindowTextW(g_pinLabel, g_sessionPin.c_str());
    SetWindowTextW(g_statusLabel, L"Code affiche. Le serveur demarre sur l'adresse choisie.");
    EnableWindow(GetDlgItem(g_window, kStartId), FALSE);
    EnableWindow(GetDlgItem(g_window, kStopId), TRUE);
    SetTimer(g_window, kPollTimerId, 150, nullptr);
}

void StopServer()
{
    KillTimer(g_window, kPollTimerId);
    if (g_serverProcess)
    {
        TerminateProcess(g_serverProcess, 0);
        CloseHandle(g_serverProcess);
        g_serverProcess = nullptr;
    }
    if (g_serverOutput)
    {
        CloseHandle(g_serverOutput);
        g_serverOutput = nullptr;
    }
    SecureZeroMemory(g_sessionPin.data(), g_sessionPin.size());
    g_sessionPin.clear();
    SetWindowTextW(g_pinLabel, L"------");
    SetWindowTextW(g_statusLabel, L"Serveur arrete.");
    EnableWindow(GetDlgItem(g_window, kStopId), FALSE);
    EnableWindow(GetDlgItem(g_window, kStartId), g_prerequisitesReady);
}

void PollServerOutput()
{
    if (!g_serverProcess || !g_serverOutput)
    {
        return;
    }
    DWORD available = 0;
    if (PeekNamedPipe(g_serverOutput, nullptr, 0, nullptr, &available, nullptr) && available > 0)
    {
        std::array<char, 2048> bytes{};
        DWORD bytesRead = 0;
        if (ReadFile(g_serverOutput, bytes.data(),
                     (std::min)(available, static_cast<DWORD>(bytes.size())), &bytesRead, nullptr) && bytesRead > 0)
        {
            g_outputBuffer.append(bytes.data(), bytesRead);
            if (g_outputBuffer.size() > 8192)
            {
                g_outputBuffer.erase(0, g_outputBuffer.size() - 256);
            }
        }
    }

    DWORD exitCode = STILL_ACTIVE;
    if (GetExitCodeProcess(g_serverProcess, &exitCode) && exitCode != STILL_ACTIVE)
    {
        KillTimer(g_window, kPollTimerId);
        CloseHandle(g_serverProcess);
        g_serverProcess = nullptr;
        CloseHandle(g_serverOutput);
        g_serverOutput = nullptr;
        EnableWindow(GetDlgItem(g_window, kStopId), FALSE);
        EnableWindow(GetDlgItem(g_window, kStartId), g_prerequisitesReady);
        SecureZeroMemory(g_sessionPin.data(), g_sessionPin.size());
        g_sessionPin.clear();
        SetWindowTextW(g_pinLabel, L"------");
        if (exitCode == 0)
        {
            SetWindowTextW(g_statusLabel, L"Session terminee. Redemarre pour generer un nouveau code.");
        }
        else
        {
            std::wstring status = L"Le serveur s'est ferme (code " + std::to_wstring(exitCode) + L").";
            std::wstring detail;
            for (const unsigned char character : g_outputBuffer)
            {
                if ((character >= 32 && character <= 126) || character == '\r' || character == '\n')
                {
                    detail.push_back(character == '\r' || character == '\n' ? L' ' : character);
                }
            }
            if (detail.size() > 120)
            {
                detail.erase(0, detail.size() - 120);
            }
            if (!detail.empty())
            {
                status += L" " + detail;
            }
            SetWindowTextW(g_statusLabel, status.c_str());
        }
    }
}

void CopyPin()
{
    if (g_sessionPin.empty() || !OpenClipboard(g_window))
    {
        return;
    }
    EmptyClipboard();
    const SIZE_T bytes = (g_sessionPin.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory)
    {
        void* data = GlobalLock(memory);
        if (data)
        {
            memcpy(data, g_sessionPin.c_str(), bytes);
            GlobalUnlock(memory);
            SetClipboardData(CF_UNICODETEXT, memory);
        }
        else
        {
            GlobalFree(memory);
        }
    }
    CloseClipboard();
}

void LaunchScript(const wchar_t* scriptName, bool requireElevation = false)
{
    const std::wstring scriptPath = g_workspaceRoot + L"\\Installer\\" + scriptName;
    std::wstring parameters = L"-NoProfile -ExecutionPolicy Bypass ";
    if (requireElevation)
    {
        parameters += L"-NoExit ";
    }
    parameters += L"-File \"" + scriptPath + L"\"";
    ShellExecuteW(g_window, requireElevation ? L"runas" : L"open", L"powershell.exe", parameters.c_str(),
                  g_workspaceRoot.c_str(), SW_SHOWNORMAL);
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        g_window = window;
        HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        CreateWindowExW(0, L"BUTTON", L"Prerequis et reseau", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                16, 64, 592, 120, window, nullptr, instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Session securisee", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                16, 190, 592, 180, window, nullptr, instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Preparation", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                16, 374, 592, 52, window, nullptr, instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Diffusion", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                16, 430, 592, 108, window, nullptr, instance, nullptr);
        AddLabel(instance, L"MyDisplay | Serveur", 26, 18, 500, 34);
        const std::wstring computerName = L"Nom a saisir sur le client : " + GetLocalComputerName();
        AddLabel(instance, computerName.c_str(), 28, 48, 560, 22);
        g_certificateStatus = AddLabel(instance, L"Verification du certificat TLS...", 28, 76, 560, 24);
        g_displayStatus = AddLabel(instance, L"Verification de l'ecran virtuel...", 28, 104, 560, 24);
        AddLabel(instance, L"Adresse reseau de ce PC", 28, 148, 200, 22);
        g_addressInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", GetLocalIpv4().c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
            240, 142, 190, 28, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(kAddressId)), instance, nullptr);
        AddLabel(instance, L"Port TLS : 48001", 28, 188, 240, 22);
        AddLabel(instance, L"CODE DE SESSION", 28, 236, 220, 24);
        g_pinLabel = AddLabel(instance, L"------", 28, 264, 250, 60);
        AddButton(instance, kCopyPinId, L"Copier le code", 300, 266, 130, 34);
        g_statusLabel = AddLabel(instance, L"Le code apparait au demarrage du serveur.", 28, 324, 560, 42);
        AddButton(instance, kRefreshId, L"Actualiser", 28, 384, 95, 34);
        AddButton(instance, kCreateCertificateId, L"Creer certificat", 133, 384, 125, 34);
        AddButton(instance, kPackageClientId, L"Paquet client", 268, 384, 155, 34);
        AddButton(instance, kInstallDriverId, L"Installer ecran IDD", 433, 384, 145, 34);
        AddButton(instance, kStartId, L"Demarrer", 28, 438, 130, 38, BS_DEFPUSHBUTTON);
        AddButton(instance, kStopId, L"Arreter", 170, 438, 130, 38);
        AddLabel(instance,
            L"L'installation IDD demande l'accord UAC et l'approbation du certificat de test. TCP 48001 doit etre autorise sur le profil Prive.",
            28, 492, 560, 40);
        EnableWindow(GetDlgItem(window, kStopId), FALSE);
        UpdatePrerequisites();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kRefreshId:
            UpdatePrerequisites();
            SetWindowTextW(g_statusLabel, L"Verification des prerequis terminee.");
            return 0;
        case kCreateCertificateId:
            LaunchScript(L"New-ServerCertificate.ps1");
            SetWindowTextW(g_statusLabel, L"Apres la creation du certificat, clique sur Actualiser.");
            return 0;
        case kPackageClientId:
            LaunchScript(L"Package-Client.ps1");
            SetWindowTextW(g_statusLabel, L"Le paquet client est prepare dans Client\\Package.");
            return 0;
        case kInstallDriverId:
            LaunchScript(L"Test-Phase1.ps1", true);
            SetWindowTextW(g_statusLabel,
                L"PowerShell reste ouvert : lis l'erreur s'il y en a une. Apres l'installation, clique sur Actualiser.");
            return 0;
        case kStartId:
            StartServer();
            return 0;
        case kStopId:
            StopServer();
            return 0;
        case kCopyPinId:
            CopyPin();
            return 0;
        }
        break;
    case WM_TIMER:
        if (wParam == kPollTimerId)
        {
            PollServerOutput();
            return 0;
        }
        break;
    case WM_CLOSE:
        StopServer();
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    WSADATA winsockData{};
    if (WSAStartup(MAKEWORD(2, 2), &winsockData) != 0)
    {
        MessageBoxW(nullptr, L"Winsock n'a pas pu demarrer.", L"MyDisplay", MB_ICONERROR);
        return 1;
    }

    std::array<wchar_t, MAX_PATH> modulePath{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath.data(),
                                                   static_cast<DWORD>(modulePath.size()));
    if (moduleLength == 0 || moduleLength >= modulePath.size())
    {
        WSACleanup();
        MessageBoxW(nullptr, L"Impossible de trouver le dossier du serveur.", L"MyDisplay", MB_ICONERROR);
        return 1;
    }
    g_serverDirectory.assign(modulePath.data(), moduleLength);
    const std::size_t separator = g_serverDirectory.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        WSACleanup();
        return 1;
    }
    g_serverDirectory.resize(separator);
    g_workspaceRoot = g_serverDirectory;
    for (int level = 0; level < 4; ++level)
    {
        const std::size_t parentSeparator = g_workspaceRoot.find_last_of(L"\\/");
        if (parentSeparator == std::wstring::npos)
        {
            WSACleanup();
            return 1;
        }
        g_workspaceRoot.resize(parentSeparator);
    }

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&windowClass))
    {
        WSACleanup();
        return 1;
    }

    HWND window = CreateWindowExW(0, kWindowClass, L"MyDisplay - Serveur",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, 640, 610, nullptr, nullptr, instance, nullptr);
    if (!window)
    {
        WSACleanup();
        return 1;
    }
    HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
    {
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
    ShowWindow(window, showCommand);
    UpdateWindow(window);

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    WSACleanup();
    return static_cast<int>(message.wParam);
}
