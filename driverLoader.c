#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>
#include <stdio.h>

int main()
{
    SC_HANDLE hSCManager = OpenSCManager(NULL, NULL, SC_MANAGER_ALL_ACCESS);
    if (!hSCManager) {
        printf("OpenSCManager failed: %lu\n", GetLastError());
        return 1;
    }

    // 获取加载器自身的完整路径（例如 C:\MyFolder\DriverLoader.exe）
    WCHAR exePath[MAX_PATH];
    GetModuleFileName(NULL, exePath, MAX_PATH);

    // 去掉文件名，只保留目录部分（例如 C:\MyFolder\）
    WCHAR drive[_MAX_DRIVE];
    WCHAR dir[_MAX_DIR];
    _wsplitpath_s(exePath, drive, _MAX_DRIVE, dir, _MAX_DIR, NULL, 0, NULL, 0);

    // 拼接出 .sys 文件完整路径（例如 C:\MyFolder\MouseTickFilter.sys）
    WCHAR sysPath[MAX_PATH];
    _swprintf(sysPath, L"%s%sdeSubtick.sys", drive, dir);

    // 转换为 NT 路径格式（\??\C:\MyFolder\MouseTickFilter.sys）
    WCHAR ntPath[MAX_PATH + 4];
    swprintf(ntPath, L"\\??\\%s", sysPath);

    printf("Driver path: %ws\n", ntPath);

    // 创建服务（使用动态路径）
    SC_HANDLE hService = CreateService(
        hSCManager,
        L"MouseTickFilter",
        L"Mouse Tick Filter Driver",
        SERVICE_ALL_ACCESS,
        SERVICE_KERNEL_DRIVER,
        SERVICE_DEMAND_START,
        SERVICE_ERROR_NORMAL,
        ntPath,              // <-- 这里使用动态生成的完整路径
        NULL, NULL, NULL, NULL, NULL);

    if (!hService) {
        if (GetLastError() == ERROR_SERVICE_EXISTS)
            hService = OpenService(hSCManager, L"MouseTickFilter", SERVICE_ALL_ACCESS);
        if (!hService) {
            printf("OpenService failed: %lu\n", GetLastError());
            CloseServiceHandle(hSCManager);
            return 1;
        }
    }

    // 启动服务
    if (!StartService(hService, 0, NULL)) {
        if (GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            printf("StartService failed: %lu\n", GetLastError());
            ControlService(hService, SERVICE_CONTROL_STOP, NULL);
            DeleteService(hService);
            CloseServiceHandle(hService);
            CloseServiceHandle(hSCManager);
            return 1;
        }
    }

    printf("Driver loaded successfully. Press ENTER to stop and unload...\n");
    getchar();

    SERVICE_STATUS status;
    ControlService(hService, SERVICE_CONTROL_STOP, &status);
    DeleteService(hService);
    CloseServiceHandle(hService);
    CloseServiceHandle(hSCManager);
    printf("Driver unloaded.\n");
    return 0;
}