#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <stdio.h>
#include <mmsystem.h>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "user32.lib")

#define TICK_INTERVAL_MS  16      // 约 1/64 秒
#define SIMULATED_EXTRA    0x20240628  // 标记模拟输入

// 全局状态（钩子线程与定时器线程共享）
static volatile LONG g_leftDown = 0;
static volatile LONG g_stateChanged = 0;
static volatile LONG g_exit = 0;

static HANDLE g_hHookThread = NULL;   // 钩子线程句柄
static MMRESULT g_timerId = 0;

// ------------------------------------------------------------------
// 低级鼠标钩子过程 —— 只做最少的必要判断，其余直接返回
// ------------------------------------------------------------------
LRESULT CALLBACK LowLevelMouseProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode == HC_ACTION) {
        MSLLHOOKSTRUCT *pMouse = (MSLLHOOKSTRUCT *)lParam;

        // 如果是自己注入的模拟事件，立即放行，不更新状态
        if (pMouse->dwExtraInfo == SIMULATED_EXTRA)
            return CallNextHookEx(NULL, nCode, wParam, lParam);

        // 只拦截左键按下/释放，其他消息完全原样放行
        switch (wParam) {
        case WM_LBUTTONDOWN:
            InterlockedExchange(&g_leftDown, 1);
            InterlockedExchange(&g_stateChanged, 1);
            return 1;   // 吞噬

        case WM_LBUTTONUP:
            InterlockedExchange(&g_leftDown, 0);
            InterlockedExchange(&g_stateChanged, 1);
            return 1;   // 吞噬

        default:  // 移动、滚轮、其他按键 → 零开销放行
            break;
        }
    }
    return CallNextHookEx(NULL, nCode, wParam, lParam);
}

// ------------------------------------------------------------------
// 定时器回调：将缓存状态“原样”注入真实左键点击
// ------------------------------------------------------------------
void CALLBACK TickTimerProc(UINT uID, UINT uMsg, DWORD_PTR dwUser,
    DWORD_PTR dw1, DWORD_PTR dw2)
{
    UNREFERENCED_PARAMETER(uID);
    UNREFERENCED_PARAMETER(uMsg);
    UNREFERENCED_PARAMETER(dwUser);
    UNREFERENCED_PARAMETER(dw1);
    UNREFERENCED_PARAMETER(dw2);

    if (!InterlockedExchange(&g_stateChanged, 0))
        return;   // 无变化，不注入

    LONG down = InterlockedCompareExchange(&g_leftDown, 0, 0);

    INPUT input = { 0 };
    input.type = INPUT_MOUSE;
    input.mi.dwExtraInfo = SIMULATED_EXTRA;   // 标记为模拟
    if (down)
        input.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    else
        input.mi.dwFlags = MOUSEEVENTF_LEFTUP;

    SendInput(1, &input, sizeof(INPUT));
}

// ------------------------------------------------------------------
// 钩子线程 —— 负责创建消息窗口、注册热键、运行消息循环
// ------------------------------------------------------------------
DWORD WINAPI HookThreadProc(LPVOID lpParam)
{
    UNREFERENCED_PARAMETER(lpParam);

    // 1. 创建隐藏窗口（用来接收热键消息）
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.lpfnWndProc = DefWindowProc;   // 直接用默认过程，热键会以 WM_HOTKEY 到达
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = L"MouseHookHiddenWnd";
    if (!RegisterClassEx(&wc))
        return 1;

    HWND hWnd = CreateWindowEx(0, wc.lpszClassName, L"", 0,
        0, 0, 0, 0, NULL, NULL, wc.hInstance, NULL);
    if (!hWnd)
        return 1;

    // 注册 Ctrl+Q 热键
    if (!RegisterHotKey(hWnd, 1, MOD_CONTROL, 'Q')) {
        DestroyWindow(hWnd);
        return 1;
    }

    // 2. 安装低级鼠标钩子（全局）
    HHOOK hMouseHook = SetWindowsHookEx(WH_MOUSE_LL, LowLevelMouseProc,
        GetModuleHandle(NULL), 0);
    if (!hMouseHook) {
        UnregisterHotKey(hWnd, 1);
        DestroyWindow(hWnd);
        return 1;
    }

    // 3. 消息循环（GetMessage 阻塞等待，不会浪费 CPU）
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        if (msg.message == WM_HOTKEY && msg.wParam == 1) {
            InterlockedExchange(&g_exit, 1);
            PostQuitMessage(0);   // 退出消息循环
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    // 4. 清理
    UnhookWindowsHookEx(hMouseHook);
    UnregisterHotKey(hWnd, 1);
    DestroyWindow(hWnd);
    return 0;
}

// ------------------------------------------------------------------
// 主函数
// ------------------------------------------------------------------
int main()
{
    // 1. 启动钩子线程，并提升其优先级（让鼠标事件处理更及时）
    g_hHookThread = CreateThread(NULL, 0, HookThreadProc, NULL, 0, NULL);
    if (!g_hHookThread) {
        fprintf(stderr, "Failed to create hook thread\n");
        return 1;
    }
    SetThreadPriority(g_hHookThread, THREAD_PRIORITY_HIGHEST);

    // 2. 启动 1/64 秒定时器
    g_timerId = timeSetEvent(TICK_INTERVAL_MS, 1, TickTimerProc, 0,
        TIME_PERIODIC | TIME_CALLBACK_FUNCTION);
    if (!g_timerId) {
        fprintf(stderr, "Failed to start timer\n");
        InterlockedExchange(&g_exit, 1);
        PostThreadMessage(GetThreadId(g_hHookThread), WM_QUIT, 0, 0);
        WaitForSingleObject(g_hHookThread, INFINITE);
        CloseHandle(g_hHookThread);
        return 1;
    }

    printf("=== Optimized Mouse Click Injector ===\n");
    printf("Left button hijacked, real clicks injected every 1/64 sec.\n");
    printf("Mouse movement unaffected. Press Ctrl+Q to exit.\n");

    // 3. 等待钩子线程退出（Ctrl+Q 触发）
    WaitForSingleObject(g_hHookThread, INFINITE);

    // 4. 清理定时器
    timeKillEvent(g_timerId);
    CloseHandle(g_hHookThread);

    printf("Exiting... left button restored.\n");
    return 0;
}
