// sudo - "Super User Do" - Launches the command line as admin (elevated).

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <tchar.h>
#include <strsafe.h>

#include "commit_file.h"
#include "version.h"

// vim: set et ts=4 sw=4 cino={0s:

static const char* const usage =
"Runs the specified command line with elevation (as super user).\r\n"
"\r\n"
"SUDO [options] {command line}\r\n"
"\r\n"
"  -?, -h, --help            Display a short help message and exit.\r\n"
"  -b, --background          Run the command in the background.  Interactive\r\n"
"                            commands will likely fail to work properly when\r\n"
"                            run in the background.\r\n"
"  -D dir, --chdir=dir       Run the command in the specified directory.\r\n"
"  -n, --non-interactive     Avoid showing any UI.\r\n"
"  -p text, --prompt=text    Use a custom password prompt.\r\n"
"  -S, --stdin               Write the prompt to stderr and read the password\r\n"
"                            from stdin instead of using the console.\r\n"
"  -u user, --user=user      Run the command as the specified user.\r\n"
"  -V, --version             Print the sudo version string.\r\n"
#ifdef INCLUDE_NET_ONLY
"  --net-only                Use the credentials only on the network.\r\n"
#endif
#ifdef DEBUG // Only advertised in debug builds, but always present.
"  --debug                   Display debugging info.\r\n"
#endif
"  --                        Stop processing options in the command line.\r\n"
"\r\n"
"Redirection and pipes work if the symbols are used inside quotes; otherwise\r\n"
"the symbols are interpreted by CMD before they reach sudo.\r\n"
"\r\n"
"If you get into an endless loop of spawning sudo.exe, you can hold\r\n"
"Alt+Ctrl+Shift at the same time to cancel.\r\n"
"\r\n"
"The custom password prompt can include the following escape sequences:\r\n"
"\r\n"
"  %H                Expands to the host name with the domain name.\r\n"
"  %h                Expands to the local host name without the domain name.\r\n"
"  %p or %U          Expands to the name of the user whose password is being\r\n"
"                    requested.\r\n"
"  %u                Expands to the invoking user's account name.\r\n"
"  %%                Expands to a single percent sign.\r\n"
"\r\n"
"The password prompt uses the custom prompt string, if provided.  Otherwise it\r\n"
"uses the %SUDO_PROMPT% or a default prompt string."
;

inline BYTE ForceUnsigned(char ch) { return BYTE(ch); }
inline WORD ForceUnsigned(WCHAR ch) { return ch; }

#define PACKVERSION(major,minor)    MAKELONG(minor,major)

static WCHAR*
CopyString(const WCHAR* in)
{
    unsigned int len = (unsigned int)wcslen(in);
    const size_t bytes = (len + 1) * sizeof(*in);
    WCHAR* out = (WCHAR*)malloc(bytes);
    if (out)
        memcpy(out, in, bytes);
    return out;
}

static HANDLE
__GetStdHandle(int std_handle, bool fStd=true)
{
    if (!fStd)
    {
        static HANDLE s_hcon[2] = {};
        const bool fIn = (std_handle == STD_INPUT_HANDLE);
        if (!s_hcon[fIn])
        {
            // IMPORTANT: CONIN$ requires both read and write access so that
            // SetConsoleMode() can change the mode, e.g. to disable echo to
            // hide password input.
            const DWORD dwShare = FILE_SHARE_READ|FILE_SHARE_WRITE;
            s_hcon[fIn] = (fIn ?
                CreateFileW(L"CONIN$", GENERIC_READ|GENERIC_WRITE, dwShare, 0, OPEN_EXISTING, 0, 0) :
                CreateFileW(L"CONOUT$", GENERIC_WRITE, dwShare, 0, OPEN_EXISTING, 0, 0));
        }
        if (s_hcon[fIn])
            return s_hcon[fIn];
    }

    return GetStdHandle(std_handle);
}

static void
__OutText(const char* text, int std_handle, bool fStd=true)
{
    DWORD dummy;
    HANDLE hout = GetStdHandle(std_handle);
    const bool is_redir = !GetConsoleMode(hout, &dummy);
    const DWORD len = DWORD(strlen(text));
    if (is_redir)
        WriteFile(hout, text, len, &dummy, nullptr);
    else
        WriteConsoleA(hout, text, len, &dummy, 0);
}

static void
__OutText(const WCHAR* text, int std_handle, bool fStd=true)
{
    DWORD dummy;
    HANDLE hout = GetStdHandle(std_handle);
    const bool is_redir = !GetConsoleMode(hout, &dummy);
    const DWORD len = DWORD(wcslen(text));
    if (is_redir)
    {
        const ULONG need = WideCharToMultiByte(CP_ACP, 0, text, -1, 0, 0, 0, 0);
	    if (!need)
	        return;
        char* tmp = static_cast<char*>(HeapAlloc(GetProcessHeap(), 0, need * sizeof(*tmp)));
	    if (!tmp)
		    return;
	    const ULONG used = WideCharToMultiByte(CP_ACP, 0, text, -1, tmp, need, 0, 0);
        WriteFile(hout, tmp, used - 1, &dummy, nullptr);
        HeapFree(GetProcessHeap(), 0, tmp);
    }
    else
    {
        WriteConsoleW(hout, text, len, &dummy, 0);
    }
}

inline void OutText(const char* text, bool fStd=true) { __OutText(text, STD_OUTPUT_HANDLE, fStd); }
inline void OutText(const WCHAR* text, bool fStd=true) { __OutText(text, STD_OUTPUT_HANDLE, fStd); }
inline void ErrText(const char* text, bool fStd=true) { __OutText(text, STD_ERROR_HANDLE, fStd); }
inline void ErrText(const WCHAR* text, bool fStd=true) { __OutText(text, STD_ERROR_HANDLE, fStd); }

class NoEcho
{
public:
    NoEcho(bool fStd)
        : m_std(fStd)
        , m_h(__GetStdHandle(STD_INPUT_HANDLE, fStd))
    {
        s_this = this;
        GetConsoleMode(m_h, &m_mode);
        SetConsoleMode(m_h, ENABLE_PROCESSED_INPUT|ENABLE_LINE_INPUT);
        SetConsoleCtrlHandler(Handler, true);
    }

    ~NoEcho()
    {
        SetConsoleCtrlHandler(Handler, false);
        m_newline = false; // Only needed in response to Ctrl-C or Ctrl-Break.
        Restore();
        s_this = nullptr;
    }

    HANDLE GetHandle() const
    {
        return m_h;
    }

    void Restore()
    {
        SetConsoleMode(m_h, m_mode);
        if (m_newline)
            OutText("\n", m_std);
    }

    static BOOL WINAPI Handler(DWORD dwCtrlType)
    {
        s_this->Restore();
        return false;
    }

private:
    bool m_std;
    bool m_newline = true;
    DWORD m_mode = 0;
    HANDLE m_h = NULL;
    static NoEcho* s_this;
};

NoEcho* NoEcho::s_this = nullptr;

static void
AppendTo(WCHAR*& out, unsigned int& max_len, const WCHAR* append)
{
    while (max_len && *append)
    {
        *(out++) = *(append++);
        --max_len;
    }
}

static void
PrintPrompt(const WCHAR* prompt, const WCHAR* pszUser, bool fStd)
{
    WCHAR szTmp[1024];
    WCHAR* out = szTmp;
    unsigned int remaining = _countof(szTmp) - 1;

    for (const WCHAR* walk = prompt; *walk && remaining; ++walk)
    {
        if (*walk == '%')
        {
            ++walk;
            switch (*walk)
            {
            case '%':
                *(out++) = *walk;
                break;
            case 'H':
            case 'h':
                {
                    WCHAR szComp[1024];
                    const COMPUTER_NAME_FORMAT format = ((*walk == 'H') ?
                        ComputerNameDnsFullyQualified : ComputerNameDnsHostname);

                    DWORD dwSize = _countof(szComp);
                    if (GetComputerNameExW(format, szComp, &dwSize))
                        AppendTo(out, remaining, szComp);
                }
                break;
            case 'p':
            case 'U':
                {
                    // TODO: How is "name of user whose password is requested"
                    // meant to differ from "user the command will be run as"?
                    if (pszUser)
                        AppendTo(out, remaining, pszUser);
                }
                break;
            case 'u':
                {
                    WCHAR szUser[1024];
                    DWORD dwSize = _countof(szUser);
                    if (GetUserNameW(szUser, &dwSize))
                        AppendTo(out, remaining, szUser);
                }
                break;
            default:
                break;
            }
        }
        else
        {
            *(out++) = *walk;
            --remaining;
        }
    }

    *out = '\0';

    OutText(szTmp, fStd);
}

static bool
InputPassword(WCHAR* out, DWORD len, bool fStd)
{
    bool ok;
    DWORD err;

    {
        DWORD dummy;
        NoEcho noecho(fStd);

        ok = !!ReadConsoleW(noecho.GetHandle(), out, len, &dummy, nullptr);
        err = GetLastError();
    }

    SetLastError(err);
    return ok;
}

static void
TrimString(wchar_t* psz, bool spaces)
{
    size_t len = wcslen(psz);
    while (len-- && (psz[len] == '\r' || psz[len] == '\n'))
        psz[len] = '\0';

    if (!spaces)
        return;

    while (len-- && (psz[len] == ' ' || psz[len] == '\t'))
        psz[len] = '\0';
}

static void
ExitFailure(DWORD err)
{
    WCHAR sz[1024];
    const DWORD dwFlags = FORMAT_MESSAGE_FROM_SYSTEM|FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD cch = FormatMessageW(dwFlags, 0, err, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), sz, _countof(sz), 0);

    if (cch)
    {
        TrimString(sz, true/*spaces*/);
    }
    else
    {
        if (err < 65536)
            swprintf_s(sz, _countof(sz), L"Error %u.", err);
        else
            swprintf_s(sz, _countof(sz), L"Error 0x%08X.", err);
    }

    ErrText(sz);
    ErrText("\r\nsudo failed.\r\n");

    ExitProcess(-1);
}

static bool s_more_flags = false;

static bool
TestFlag(LPCWSTR& pszLine, LPCWSTR pszFlag, bool fHasArg=false)
{
    size_t len = wcslen(pszFlag);
    if (s_more_flags && len == 2 && pszFlag[0] == '-' && pszFlag[1] != '-')
    {
        ++pszFlag;
        --len;
    }

    if (_wcsnicmp(pszLine, pszFlag, len))
        return false;

    if (!pszLine[len] ||
        pszLine[len] == ' ' ||
        pszLine[len] == '\t' ||
        (fHasArg && pszLine[len] == '='))
    {
        s_more_flags = false;
        pszLine += len;
        if (fHasArg && *pszLine == '=')
            ++pszLine;
        while (*pszLine == ' ' || *pszLine == '\t')
            ++pszLine;
        return true;
    }

    if (fHasArg)
    {
        s_more_flags = false;
        pszLine += len;
        return (pszFlag[1] != '-');
    }

    if (!fHasArg && len == 2 && pszLine[len] != '-')
    {
        s_more_flags = true;
        pszLine += len;
        return true;
    }

    return false;
}

static bool
GetArg(LPCWSTR& pszLine, LPWSTR pszOut, DWORD maxlen)
{
    bool fHasArg = false;
    bool fTooLong = false;

    s_more_flags = false;

    // FUTURE:  Does this need to handle "\"" quotation escaping?

    bool fQuote = false;
    for (; *pszLine; ++pszLine)
    {
        if (*pszLine == '"')
        {
            fQuote = !(fQuote && _istspace(ForceUnsigned(pszLine[1])));
        }
        else if (!fQuote && _istspace(ForceUnsigned(*pszLine)))
        {
            break;
        }
        else
        {
            if (maxlen <= 1)
            {
                fTooLong = true;
                if (maxlen)
                    *pszOut = '\0';
                maxlen = 0;
            }
            else
            {
                fHasArg = true;
                *pszOut = *pszLine;
                ++pszOut;
            }
        }
    }

    if (maxlen)
        *pszOut = '\0';

    while (*pszLine && _istspace(ForceUnsigned(*pszLine)))
        ++pszLine;

    SetLastError(fTooLong ? ERROR_BUFFER_OVERFLOW : ERROR_SUCCESS);
    return fHasArg && !fTooLong;
}

static DWORD
GetDllVersion(LPCWSTR lpszDllName)
{
    DWORD dwVersion = 0;
    HINSTANCE hinstDll = LoadLibrary(lpszDllName);

    if (hinstDll)
    {
        DLLGETVERSIONPROC pDllGetVersion = DLLGETVERSIONPROC(GetProcAddress(hinstDll, "DllGetVersion"));

        // Because some DLLs might not implement this function, you must test
        // for it explicitly.  Depending on the particular DLL, the lack of a
        // DllGetVersion function can be a useful indicator of the version.
        if (pDllGetVersion)
        {
            DLLVERSIONINFO dvi = { sizeof(dvi) };
            HRESULT hr = (*pDllGetVersion)(&dvi);
            if (SUCCEEDED(hr))
                dwVersion = PACKVERSION(dvi.dwMajorVersion, dvi.dwMinorVersion);
        }

        FreeLibrary(hinstDll);
    }

    return dwVersion;
}

static bool
IsElevationNeeded()
{
    if (GetDllVersion(L"shell32.dll") < PACKVERSION(5,0))
        return false; // Elevation is not supported.

    HMODULE const hLib = LoadLibrary(L"shell32.dll");
    if (!hLib)
        return false;

    union
    {
        FARPROC proc[1];
        struct
        {
            BOOL (WINAPI* IsUserAnAdmin)();
        };
    } shell32;

    shell32.proc[0] = GetProcAddress(hLib, "IsUserAnAdmin");
    bool const fElevationNeeded = shell32.proc[0] && !shell32.IsUserAnAdmin();
    FreeLibrary(hLib);
    return fElevationNeeded;
}

static LPWSTR
BuildParameters(LPCWSTR pszFile, LPCWSTR pszLine, bool fElevated)
{
    const size_t file_len = pszFile ? wcslen(pszFile) : 0;
    const size_t line_len = wcslen(pszLine);

    const size_t cch = 3 + file_len + 64 + line_len + 1;
    LPWSTR pszArgs = LPWSTR(malloc(cch * sizeof(*pszArgs)));

    WCHAR szInsert[64] = {};
    if (!fElevated)
        wsprintf(szInsert, L"--elevated %u", GetCurrentProcessId());
    else
        wsprintf(szInsert, L"/c");

    if (pszFile)
        StringCchPrintfW(pszArgs, cch, L"\"%s\" %s %s", pszFile, szInsert, pszLine);
    else
        StringCchPrintfW(pszArgs, cch, L"%s %s", szInsert, pszLine);

    return pszArgs;
}

static void
ShowHelp()
{
    OutText(usage);
}

#ifdef GUI_SUDO
int PASCAL
wWinMain(HINSTANCE hinstCurrent, HINSTANCE hinstPrevious, LPWSTR lpszCmdLine, int nCmdShow)
#else
int __cdecl
main(int argc, const char** argv)
#endif
{
#ifdef GUI_SUDO
    hinstCurrent = 0;
    hinstPrevious = 0;
    nCmdShow = 0;
#else
    LPCWSTR pszLine = GetCommandLineW();
    GetArg(pszLine, nullptr, 0);
#endif

    WCHAR szFile[1024];
    WCHAR szDir[1024];
    WCHAR szUser[1024];
    WCHAR szPrompt[1024];
    LPWSTR pszArgs = nullptr;
    LPCWSTR pszDir = nullptr;
    LPWSTR pszUser = nullptr;
    LPCWSTR pszPrompt = nullptr;
    bool fDebug = false;
    bool fBackground = false;
    bool fNOUI = false;
    bool fHavePID = false;
    bool fNetOnly = false;
    bool fStd = false;

    DWORD dwPID = 0;
    const bool fElevated = TestFlag(pszLine, L"--elevated");
    LPCWSTR pszSavedLine = pszLine;
    if (fElevated)
    {
        WCHAR szPID[64];
        if (!GetArg(pszLine, szPID, _countof(szPID)))
        {
            ShowHelp();
            return 1;
        }
        dwPID = _wtoi(szPID);
    }

    while (true)
    {
        if (TestFlag(pszLine, L"-?") || TestFlag(pszLine, L"-h") || TestFlag(pszLine, L"--help"))
        {
            ShowHelp();
            return 0;
        }
        else if (TestFlag(pszLine, L"-V") || TestFlag(pszLine, L"--version"))
        {
            OutText("SUDO " SUDO_VERSION_STR "; " SUDO_COPYRIGHT_STR "; MIT License.\r\n");
            return 0;
        }
        else if (TestFlag(pszLine, L"-b") || TestFlag(pszLine, L"--background"))
        {
            fBackground = true;
        }
        else if (TestFlag(pszLine, L"-n") || TestFlag(pszLine, L"--non-interactive"))
        {
            fNOUI = true;
        }
        else if (TestFlag(pszLine, L"-p", true) || TestFlag(pszLine, L"--prompt", true))
        {
            szPrompt[0] = 0;
            GetArg(pszLine, szPrompt, _countof(szPrompt));
            pszPrompt = szPrompt;
        }
        else if (TestFlag(pszLine, L"-u", true) || TestFlag(pszLine, L"--user", true))
        {
            szUser[0] = 0;
            GetArg(pszLine, szUser, _countof(szUser));
            pszUser = szUser;
        }
        else if (TestFlag(pszLine, L"-D", true) || TestFlag(pszLine, L"--chdir", true))
        {
            szDir[0] = 0;
            GetArg(pszLine, szDir, _countof(szDir));
            pszDir = szDir;
        }
        else if (TestFlag(pszLine, L"-S") || TestFlag(pszLine, L"--stdin"))
        {
            fStd = true;
        }
#ifdef INCLUDE_NET_ONLY
        else if (TestFlag(pszLine, L"--net-only"))
        {
            fNetOnly = true;
        }
#endif
        else if (TestFlag(pszLine, L"--debug"))
        {
            fDebug = true;
        }
        else if (TestFlag(pszLine, L"--"))
        {
            break;
        }
        else if (*pszLine == '-')
        {
            ShowHelp();
            return 1;
        }
        else
        {
            break;
        }
    }

    if (!*pszLine)
    {
        ErrText("Missing command to execute.\r\n");
        OutText("\r\nUsage:\r\n\r\n");
        ShowHelp();
        return 1;
    }

    if (fElevated)
    {
        if (fDebug)
        {
            char szPID[64];
            sprintf(szPID, "%u\r\n", dwPID);
            OutText("FREECONSOLE, ATTACH TO "); OutText(szPID);
        }

        FreeConsole();
        AttachConsole(dwPID);
    }
    else
    {
        const DWORD cch = GetModuleFileName(0, szFile, _countof(szFile));
        if (!cch)
            ExitFailure(ERROR_FILE_NOT_FOUND);
        if (cch >= _countof(szFile))
            ExitFailure(ERROR_BUFFER_OVERFLOW);

        pszLine = pszSavedLine;

        if (fDebug)
        {
            OutText("MODULE='"); OutText(szFile); OutText("'\r\n");
            OutText("ARGUMENTS='"); OutText(pszLine); OutText("'\r\n");
        }
    }

    // Cancel if Alt+Ctrl+Shift are held.  This is intended to provide a way
    // to break out of an infinite cycle of sudo calls.

    if (GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0 && GetKeyState(VK_MENU) < 0)
    {
        ErrText("... sudo aborted because Alt+Ctrl+Shift are held ...\r\n");
        return -1;
    }

    // Spawn the process.  First use ShellExecuteEx() with "runas" to spawn a
    // hidden sudo.exe as Administrator, passing it the original process ID.
    // Once that is running as an Administrator it attaches to the original
    // console and spawns the specified process.

    HANDLE hProcess = 0;
    if (fElevated)
    {
        DWORD dw = GetEnvironmentVariableW(L"COMSPEC", szFile, _countof(szFile));
        if (dw <= 0 || dw >= _countof(szFile))
            wcscpy(szFile, L"cmd.exe");

        STARTUPINFO si = { sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION pi = {};
        LPWSTR pszCmdLine = BuildParameters(szFile, pszLine, fElevated);

        if (fDebug)
        {
            OutText("\r\n---- CreateProcessW ----\r\n");
            OutText("FILE='"); OutText(szFile); OutText("'\r\n");
            OutText("CMDLINE='"); OutText(pszCmdLine); OutText("'\r\n");
            if (pszDir)
            {
                OutText("DIR='"); OutText(pszDir); OutText("'\r\n");
            }
        }

        const DWORD dwFlags = fBackground ? CREATE_NEW_PROCESS_GROUP|CREATE_NO_WINDOW : 0;

        if (!CreateProcessW(szFile, pszCmdLine, nullptr, nullptr, true, dwFlags,
                            nullptr, pszDir, &si, &pi))
        {
            ExitFailure(GetLastError());
            return -1;
        }

        hProcess = pi.hProcess;
    }
    else if (pszUser)
    {
        WCHAR szPassword[1024] = {};
        const WCHAR* pszDomain = nullptr;

        if (pszUser)
        {
            WCHAR szPrompt[1024];
            szPrompt[0] = '\0';

            if (!pszPrompt)
            {
                const DWORD len = GetEnvironmentVariableW(L"SUDO_PROMPT", szPrompt, _countof(szPrompt));
                if (len && len < _countof(szPrompt))
                    pszPrompt = szPrompt;
            }

            if (!pszPrompt)
                pszPrompt = L"Enter password for %u: ";

            PrintPrompt(pszPrompt, pszUser, fStd);

            InputPassword(szPassword, _countof(szPassword), fStd);
            OutText("\r\n", fStd);
            TrimString(szPassword, false/*spaces*/);

            WCHAR* pszSep = wcschr(pszUser, '\\');
            if (pszSep)
            {
                pszDomain = pszUser;
                *(pszSep++) = '\0';
                pszUser = pszSep;
                while (*pszUser == '\\')
                    ++pszUser;
            }
        }

        STARTUPINFO si = { sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
        si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION pi = {};
        LPWSTR pszCmdLine = BuildParameters(szFile, pszLine, fElevated);

        if (fDebug)
        {
            OutText("\r\n---- CreateProcessWithLogonW ----\r\n");
            OutText("USER='"); OutText(pszUser); OutText("'\r\n");
            if (pszDomain)
            {
                OutText("DOMAIN='"); OutText(pszDomain); OutText("'\r\n");
            }
            OutText("PASSWORD='*****'\r\n");
            OutText("FILE='"); OutText(szFile); OutText("'\r\n");
            OutText("CMDLINE='"); OutText(pszCmdLine); OutText("'\r\n");
            if (pszDir)
            {
                OutText("DIR='"); OutText(pszDir); OutText("'\r\n");
            }
        }

        const DWORD dwLogon = fNetOnly ? LOGON_NETCREDENTIALS_ONLY : LOGON_WITH_PROFILE;
        if (!CreateProcessWithLogonW(pszUser, pszDomain, szPassword, dwLogon,
                                     szFile, pszCmdLine, CREATE_NO_WINDOW,
                                     nullptr, pszDir, &si, &pi))
        {
            ExitFailure(GetLastError());
            return -1;
        }

        hProcess = pi.hProcess;
    }
    else
    {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE);

        // Must use SW_HIDE so that FreeConsole() and AttachConsole() can work.
        SHELLEXECUTEINFO sei = {};
        sei.cbSize = sizeof(sei);
        sei.hwnd = NULL;
        sei.fMask = SEE_MASK_NOASYNC|SEE_MASK_NOCLOSEPROCESS|(fNOUI ? SEE_MASK_FLAG_NO_UI : 0);
        sei.lpVerb = L"runas";
        sei.lpFile = szFile;
        sei.lpParameters = BuildParameters(nullptr, pszLine, fElevated);
        sei.lpDirectory = pszDir;
        sei.nShow = SW_HIDE;

        if (fDebug)
        {
            OutText("ShellExecuteEx:\r\n");
            OutText("FILE='"); OutText(szFile); OutText("'\r\n");
            OutText("PARAMETERS='"); OutText(sei.lpParameters); OutText("'\r\n");
            if (pszDir)
            {
                OutText("DIR='"); OutText(pszDir); OutText("'\r\n");
            }
        }

        if (!ShellExecuteEx(&sei))
        {
            ExitFailure(GetLastError());
            return -1;
        }

        hProcess = sei.hProcess;
    }

    // Return the exit code.
    DWORD dwExit = 0;
    if (fBackground)
    {
        if (fDebug)
            OutText("BACKGROUND; not waiting for completion.\r\n");
    }
    else
    {
        WaitForSingleObject(hProcess, INFINITE);
        GetExitCodeProcess(hProcess, &dwExit);
    }
    return dwExit;
}

