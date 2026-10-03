#include <winsock2.h>
#include <ws2tcpip.h>
#include <Windows.h>

#include <array>
#include <string>
#include <vector>

namespace
{
constexpr wchar_t kWindowClass[] = L"MyDisplayClientSetupWindow";
constexpr int kServerIpId = 101;
constexpr int kServerNameId = 102;
constexpr int kPortId = 103;
constexpr int kPinId = 104;
constexpr int kConnectId = 105;
constexpr int kStopId = 106;
constexpr int kStatusId = 107;
constexpr UINT_PTR kStatusTimerId = 1;

HWND g_window = nullptr;
HWND g_ipInput = nullptr;
HWND g_nameInput = nullptr;
HWND g_portInput = nullptr;
HWND g_pinInput = nullptr;
HWND g_packageStatusLabel = nullptr;
HWND g_statusLabel = nullptr;
HANDLE g_bridgeProcess = nullptr;
std::wstring g_packageDirectory;

std::wstring GetWindowTextValue(HWND control)
{
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

std::wstring QuoteArgument(const std::wstring& argument)
{
    std::wstring quoted = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'\"')
        {
            quoted.append(backslashes * 2 + 1, L'\\');
            quoted.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        quoted.append(backslashes, L'\\');
        backslashes = 0;
        quoted.push_back(character);
    }
    quoted.append(backslashes * 2, L'\\');
    quoted.push_back(L'\"');
    return quoted;
}

HWND AddLabel(HINSTANCE instance, const wchar_t* text, int x, int y, int width, int height)
{
    return CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE,
                           x, y, width, height, g_window, nullptr, instance, nullptr);
}

HWND AddInput(HINSTANCE instance, int identifier, const wchar_t* initialValue,
              int x, int y, int width, int height, DWORD style = 0)
{
    return CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", initialValue,
                           WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | style,
                           x, y, width, height, g_window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(identifier)),
                           instance, nullptr);
}

void SetStatus(const wchar_t* text)
{
    SetWindowTextW(g_statusLabel, text);
}

std::wstring GetMissingPackageFiles()
{
    const std::array<std::pair<const wchar_t*, const wchar_t*>, 3> requiredFiles{{
        { L"ServerCertificate.cer", L"ServerCertificate.cer" },
        { L"DisplayClient.exe", L"DisplayClient.exe" },
        { L"DisplayClientTlsBridge.exe", L"DisplayClientTlsBridge.exe" }
    }};
    std::wstring missingFiles;
    for (const auto& file : requiredFiles)
    {
        const std::wstring path = g_packageDirectory + L"\\" + file.second;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            if (!missingFiles.empty())
            {
                missingFiles += L", ";
            }
            missingFiles += file.first;
        }
    }
    return missingFiles;
}

void UpdatePackageStatus()
{
    const std::wstring missingFiles = GetMissingPackageFiles();
    if (missingFiles.empty())
    {
        SetWindowTextW(g_packageStatusLabel, L"Paquet complet. Aucune installation n'est requise sur ce PC.");
        return;
    }
    const std::wstring status = L"Manque : " + missingFiles + L". Extrais tous les fichiers dans le meme dossier.";
    SetWindowTextW(g_packageStatusLabel, status.c_str());
}

bool StartHiddenProcess(const std::wstring& executable, const std::wstring& arguments,
                        HANDLE* processHandle, HANDLE childInput = nullptr,
                        HANDLE childOutput = nullptr, HANDLE childError = nullptr)
{
    std::wstring commandLine = QuoteArgument(executable);
    if (!arguments.empty())
    {
        commandLine += L" ";
        commandLine += arguments;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (childInput || childOutput || childError)
    {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdInput = childInput;
        startup.hStdOutput = childOutput;
        startup.hStdError = childError;
    }

    const BOOL created = CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr,
                                        childInput || childOutput || childError, CREATE_NO_WINDOW,
                                        nullptr, g_packageDirectory.c_str(), &startup, &process);
    if (!created)
    {
        return false;
    }

    CloseHandle(process.hThread);
    if (processHandle)
    {
        *processHandle = process.hProcess;
    }
    else
    {
        CloseHandle(process.hProcess);
    }
    return true;
}

bool StartConnection()
{
    if (g_bridgeProcess)
    {
        SetStatus(L"Une connexion est deja en cours.");
        return false;
    }

    const std::wstring serverIp = GetWindowTextValue(g_ipInput);
    const std::wstring serverName = GetWindowTextValue(g_nameInput);
    const std::wstring portText = GetWindowTextValue(g_portInput);
    const std::wstring pin = GetWindowTextValue(g_pinInput);
    const std::wstring certificatePath = g_packageDirectory + L"\\ServerCertificate.cer";
    const std::wstring viewerPath = g_packageDirectory + L"\\DisplayClient.exe";
    const std::wstring bridgePath = g_packageDirectory + L"\\DisplayClientTlsBridge.exe";

    WSADATA winsockData{};
    const int winsockResult = WSAStartup(MAKEWORD(2, 2), &winsockData);
    in_addr parsedAddress{};
    const bool validAddress = winsockResult == 0 && InetPtonW(AF_INET, serverIp.c_str(), &parsedAddress) == 1;
    if (winsockResult == 0)
    {
        WSACleanup();
    }
    if (!validAddress)
    {
        SetStatus(L"Entre l'adresse IPv4 du PC serveur, par exemple 192.168.1.25.");
        return false;
    }
    if (serverName.empty())
    {
        SetStatus(L"Entre le nom Windows du PC serveur (il s'affiche dans l'interface serveur).");
        return false;
    }
    if (pin.size() != 6 || pin.find_first_not_of(L"0123456789") != std::wstring::npos)
    {
        SetStatus(L"Le code PIN doit contenir exactement six chiffres.");
        return false;
    }
    const unsigned long port = std::wcstoul(portText.c_str(), nullptr, 10);
    if (port == 0 || port > 65535)
    {
        SetStatus(L"Le port doit etre compris entre 1 et 65535.");
        return false;
    }
    const std::wstring missingFiles = GetMissingPackageFiles();
    if (!missingFiles.empty())
    {
        const std::wstring status = L"Fichiers manquants : " + missingFiles +
            L". Extrais tout Client\\Package dans un seul dossier.";
        SetStatus(status.c_str());
        return false;
    }

    const std::wstring bridgeArguments = QuoteArgument(serverIp) + L" " + QuoteArgument(serverName) + L" " +
        QuoteArgument(certificatePath) + L" " + std::to_wstring(port) + L" --pin-stdin";

    HANDLE childRead = nullptr;
    HANDLE parentWrite = nullptr;
    SECURITY_ATTRIBUTES securityAttributes{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
    if (!CreatePipe(&childRead, &parentWrite, &securityAttributes, 0))
    {
        SetStatus(L"Impossible de creer le canal securise de saisie PIN.");
        return false;
    }
    SetHandleInformation(parentWrite, HANDLE_FLAG_INHERIT, 0);

    HANDLE nullOutput = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                    &securityAttributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (nullOutput == INVALID_HANDLE_VALUE)
    {
        CloseHandle(childRead);
        CloseHandle(parentWrite);
        SetStatus(L"Impossible de preparer le processus bridge.");
        return false;
    }

    if (!StartHiddenProcess(bridgePath, bridgeArguments, &g_bridgeProcess, childRead, nullOutput, nullOutput))
    {
        CloseHandle(childRead);
        CloseHandle(parentWrite);
        CloseHandle(nullOutput);
        SetStatus(L"Le bridge TLS n'a pas pu demarrer.");
        return false;
    }
    CloseHandle(childRead);
    CloseHandle(nullOutput);

    std::string pinBytes;
    pinBytes.reserve(pin.size() + 1);
    for (const wchar_t digit : pin)
    {
        pinBytes.push_back(static_cast<char>(digit));
    }
    pinBytes.push_back('\n');
    DWORD bytesWritten = 0;
    const BOOL pinWritten = WriteFile(parentWrite, pinBytes.data(), static_cast<DWORD>(pinBytes.size()), &bytesWritten, nullptr);
    SecureZeroMemory(pinBytes.data(), pinBytes.size());
    CloseHandle(parentWrite);
    SetWindowTextW(g_pinInput, L"");

    if (!pinWritten || bytesWritten != pinBytes.size())
    {
        TerminateProcess(g_bridgeProcess, 1);
        CloseHandle(g_bridgeProcess);
        g_bridgeProcess = nullptr;
        SetStatus(L"Le PIN n'a pas pu etre transmis au bridge.");
        return false;
    }

    STARTUPINFOW viewerStartup{};
    viewerStartup.cb = sizeof(viewerStartup);
    PROCESS_INFORMATION viewerProcess{};
    std::wstring viewerCommandLine = QuoteArgument(viewerPath);
    if (!CreateProcessW(viewerPath.c_str(), viewerCommandLine.data(), nullptr, nullptr, FALSE,
                        0, nullptr, g_packageDirectory.c_str(), &viewerStartup, &viewerProcess))
    {
        TerminateProcess(g_bridgeProcess, 1);
        CloseHandle(g_bridgeProcess);
        g_bridgeProcess = nullptr;
        SetStatus(L"Le viewer n'a pas pu demarrer.");
        return false;
    }
    CloseHandle(viewerProcess.hThread);
    CloseHandle(viewerProcess.hProcess);

    EnableWindow(GetDlgItem(g_window, kConnectId), FALSE);
    EnableWindow(GetDlgItem(g_window, kStopId), TRUE);
    SetStatus(L"Connexion TLS en cours. Le serveur doit afficher que le PIN est accepte.");
    return true;
}

void StopConnection()
{
    if (g_bridgeProcess)
    {
        TerminateProcess(g_bridgeProcess, 0);
        CloseHandle(g_bridgeProcess);
        g_bridgeProcess = nullptr;
    }
    EnableWindow(GetDlgItem(g_window, kConnectId), TRUE);
    EnableWindow(GetDlgItem(g_window, kStopId), FALSE);
    SetStatus(L"Connexion arretee.");
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    switch (message)
    {
    case WM_CREATE:
    {
        g_window = window;
        HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;
        CreateWindowExW(0, L"BUTTON", L"Connexion distante", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                14, 58, 558, 194, window, nullptr, instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Paquet client", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        14, 258, 558, 110, window, nullptr, instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Etat", WS_CHILD | WS_VISIBLE | BS_GROUPBOX,
                        14, 372, 558, 72, window, nullptr, instance, nullptr);
        AddLabel(instance, L"MyDisplay | Ecran distant", 28, 22, 500, 34);
        AddLabel(instance, L"Adresse IPv4 du PC serveur", 30, 78, 230, 22);
        g_ipInput = AddInput(instance, kServerIpId, L"", 280, 72, 260, 30);
        AddLabel(instance, L"Nom du PC serveur", 30, 126, 230, 22);
        g_nameInput = AddInput(instance, kServerNameId, L"", 280, 120, 260, 30);
        AddLabel(instance, L"Port TLS", 30, 174, 230, 22);
        g_portInput = AddInput(instance, kPortId, L"48001", 280, 168, 100, 30, ES_NUMBER);
        AddLabel(instance, L"Code PIN (6 chiffres)", 30, 222, 230, 22);
        g_pinInput = AddInput(instance, kPinId, L"", 280, 216, 150, 30, ES_PASSWORD | ES_NUMBER);
        g_packageStatusLabel = AddLabel(instance, L"Verification du paquet client...", 30, 270, 520, 34);
        CreateWindowExW(0, L"BUTTON", L"Connecter", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
                        280, 322, 130, 38, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kConnectId)), instance, nullptr);
        CreateWindowExW(0, L"BUTTON", L"Deconnecter", WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED,
                        420, 322, 130, 38, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kStopId)), instance, nullptr);
        g_statusLabel = AddLabel(instance, L"Pret. Le viewer contacte uniquement le serveur indique.", 30, 384, 520, 45);
        UpdatePackageStatus();
        return 0;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam))
        {
        case kConnectId:
            StartConnection();
            return 0;
        case kStopId:
            StopConnection();
            return 0;
        }
        break;
    case WM_CLOSE:
        StopConnection();
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
    wchar_t modulePath[MAX_PATH]{};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath));
    if (moduleLength == 0 || moduleLength >= ARRAYSIZE(modulePath))
    {
        MessageBoxW(nullptr, L"Impossible de trouver le dossier client.", L"MyDisplay", MB_ICONERROR);
        return 1;
    }
    g_packageDirectory.assign(modulePath, moduleLength);
    const std::size_t separator = g_packageDirectory.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        return 1;
    }
    g_packageDirectory.resize(separator);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&windowClass))
    {
        return 1;
    }

    HWND window = CreateWindowExW(0, kWindowClass, L"MyDisplay | Connexion client",
                                  WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                  CW_USEDEFAULT, CW_USEDEFAULT, 600, 500,
                                  nullptr, nullptr, instance, nullptr);
    if (!window)
    {
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
    return static_cast<int>(message.wParam);
}