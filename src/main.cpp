#include "../include/access.hpp"
#include "../include/appcontainer.hpp"
#include "../include/checks.hpp"
#include "../include/config.hpp"
#include "../include/exitcodes.hpp"
#include "../include/job.hpp"

#include <Windows.h>
#include <aclapi.h>
#include <winsafer.h>

#pragma comment(lib, "Userenv.lib")

#include <filesystem>
#include <iostream>
#include <string>

// from https://learn.microsoft.com/en-us/previous-versions/ms972827(v=msdn.10)?redirectedfrom=MSDN#the-dropmyrights-application
enum class saferlevel_t : DWORD
{
    NoLevelDrop = SAFER_LEVELID_FULLYTRUSTED,
    Disallowed = SAFER_LEVELID_DISALLOWED,
    Untrusted = SAFER_LEVELID_UNTRUSTED,
    Constrained = SAFER_LEVELID_CONSTRAINED,
    Normal = SAFER_LEVELID_NORMALUSER
};

saferlevel_t default_lower_rights_setting = saferlevel_t::Normal;

DWORD SpawnProcess(const cewrapper::Job &job, STARTUPINFOEX &si, HANDLE hUserToken = nullptr)
{
    auto &config = cewrapper::Config::get();

    std::wstring cmdline;
    cmdline.reserve(2048);
    cmdline += L"\"" + std::wstring(config.progid.c_str()) + L"\"";
    for (const auto &arg : config.args)
        cmdline += L" \"" + arg + L"\"";

    if (config.extra_debugging)
        std::wcerr << "Running " << config.progid.c_str() << " " << cmdline.data() << "\n";

    // todo: find out the precedence of si.StartupInfo.dwFlags and those passed directly to CreateProcess
    si.StartupInfo.dwFlags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED;

    PROCESS_INFORMATION pi = {};
    if (hUserToken == nullptr)
    {
        cewrapper::CheckWin32(CreateProcessW(config.progid.c_str(), cmdline.data(), nullptr, nullptr, false,
                                             EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT, nullptr,
                                             nullptr, &si.StartupInfo, &pi),
                              L"CreateProcessW");
    }
    else
    {
        cewrapper::CheckWin32(CreateProcessAsUserW(hUserToken, config.progid.c_str(), cmdline.data(), nullptr, nullptr,
                                                   false, NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED, nullptr, nullptr,
                                                   &si.StartupInfo, &pi),
                              L"CreateProcessAsUserW");
    }

    job.AddProcess(pi.hProcess);

    // process is always suspended at startup, so resume it always (after job.AddProcess!) unless suspend_after_start was set
    if (!config.suspend_after_start)
        ResumeThread(pi.hThread);

    const int maxtime = config.time_limit_ms;
    const int timeout = config.loopwait_ms;
    DWORD res = 0;

    int timespent = 0;
    while (maxtime == 0 || timespent < maxtime)
    {
        timespent += timeout;
        res = WaitForSingleObject(pi.hProcess, timeout);
        if (res != WAIT_TIMEOUT)
        {
            break;
        }
    }

    DWORD app_exit_code = (DWORD)SpecialExitCode::UnknownErrorWhileWaitingOnProcess;

    if (maxtime > 0 && timespent >= maxtime)
    {
        if (res != WAIT_OBJECT_0)
        {
            cewrapper::CheckWin32(TerminateProcess(pi.hProcess, (unsigned int)SpecialExitCode::ProcessTookTooLong),
                                  L"TerminateProcess");
        }

        std::wcerr << "Maximum time elapsed\n";
    }
    else if (config.debugging && res != WAIT_OBJECT_0)
    {
        cewrapper::OutputErrorMessage(res, L"WaitForSingleObject");
    }
    else
    {
        GetExitCodeProcess(pi.hProcess, &app_exit_code);

        if (config.extra_debugging)
        {
            std::wcerr << "Application exited with code: " << std::hex << app_exit_code << "\n";
        }
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return app_exit_code;
}

DWORD execute_using_lower_rights(const cewrapper::Job &job)
{
    SAFER_LEVEL_HANDLE hAuthzLevel = nullptr;
    HANDLE hToken = nullptr;
    cewrapper::CheckWin32(SaferCreateLevel(SAFER_SCOPEID_USER, static_cast<DWORD>(default_lower_rights_setting), 0, &hAuthzLevel, NULL),
                          L"SaferCreateLevel");
    cewrapper::CheckWin32(SaferComputeTokenFromLevel(hAuthzLevel, NULL, &hToken, 0, NULL),
                          L"SaferComputeTokenFromLevel");

    STARTUPINFOEX si;
    ZeroMemory(&si, sizeof(STARTUPINFO));
    si.StartupInfo.cb = sizeof(STARTUPINFO);

    DWORD app_exit_code = SpawnProcess(job, si, hToken);

    if (hAuthzLevel != nullptr)
        SaferCloseLevel(hAuthzLevel);

    return app_exit_code;
}

DWORD execute_using_appcontainer(const cewrapper::Job &job)
{
    auto &config = cewrapper::Config::get();
    cewrapper::AppContainer container(config);

    STARTUPINFOEX si = {};
    {
        si.StartupInfo.cb = sizeof(STARTUPINFOEX);
        SIZE_T attr_size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
        si.lpAttributeList = (PPROC_THREAD_ATTRIBUTE_LIST) new BYTE[attr_size]();
        cewrapper::CheckWin32(InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size),
                              L"InitializeProcThreadAttributeList");
        cewrapper::CheckWin32(UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_SECURITY_CAPABILITIES,
                                                        container.getSecurityCapabilitiesPtr(),
                                                        sizeof(SECURITY_CAPABILITIES), nullptr, nullptr),
                              L"UpdateProcThreadAttribute");

        // todo: do we need to use this at some point? -> https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-deleteprocthreadattributelist
    }

    if (config.home_set)
    {
        auto &dir = config.home;
        if (config.debugging)
            std::wcerr << "granting access to: " << dir << "\n";
        cewrapper::grant_access_to_path(container.getSid(), dir.data(), GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE);
    }
    else
    {
        // access to its own directory
        auto dir = std::filesystem::path(config.progid).parent_path().wstring();
        if (config.debugging)
            std::wcerr << "granting access to: " << dir << "\n";
        cewrapper::grant_access_to_path(container.getSid(), dir.data(), GENERIC_READ | GENERIC_WRITE | GENERIC_EXECUTE);
    }

    // todo: revoke access from the home path after we're done executing if the path keeps existing (should actually just be deleted, so revoking really isn't needed)

    for (auto &allowed : config.allowed_dirs)
    {
        if (config.debugging)
            std::wcerr << "granting access to: " << allowed.path << "\n";
        cewrapper::grant_access_to_path(container.getSid(), allowed.path.data(), allowed.rights);
    }

    for (auto &allowed : config.allowed_registry)
    {
        if (config.debugging)
            std::wcerr << "granting access to registry: " << allowed.path << ", r" << allowed.rights << "\n";
        cewrapper::grant_access_to_registry(container.getSid(), allowed.path.data(), allowed.rights, allowed.type);
    }

    if (config.wait_before_spawn)
        Sleep(10000);

    return SpawnProcess(job, si);
}

int wmain(int argc, wchar_t *argv[])
{
    if (argc < 2)
    {
        std::wcerr << L"Too few arguments\n";
        std::wcerr << L"Usage: cewrapper.exe [-v] [--config=/full/path/to/config.json] [--home=/preferred/cwdpath] "
                      L"[--time_limit=1] ExePath [args]\n";
        return (int)SpecialExitCode::NotEnoughArgs;
    }

    try
    {
        cewrapper::Config::get().initFromArguments(argc, argv);
    }
    catch (std::exception &e)
    {
        if (cewrapper::Config::get().debugging)
            std::cerr << e.what() << "\n";
        std::wcerr << L"Invalid arguments\n";
        return (int)SpecialExitCode::InvalidArgs;
    }

    cewrapper::Job job(cewrapper::Config::get());

    DWORD app_exit_code{};
    if (cewrapper::Config::get().use_appcontainer)
    {
        app_exit_code = execute_using_appcontainer(job);
    }
    else
    {
        app_exit_code = execute_using_lower_rights(job);
    }

    return app_exit_code;
}
