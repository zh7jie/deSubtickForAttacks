/*--
Copyright (c) 2008  Microsoft Corporation

Module Name:

    moufiltr.c (合并版，增加左键延迟16ms)

Abstract:

    鼠标过滤驱动程序示例 (基于 Windows Driver Framework)
    合并了头文件与源文件，并添加中文注释。

    新增功能：
        所有左键按下/释放事件延迟约16ms上报，用于解决游戏中的快速点击问题。

Environment:

    Kernel mode only - Framework Version

Notes:

    本驱动通过劫持 IOCTL_INTERNAL_MOUSE_CONNECT 替换底层端口驱动
    (mouhid.sys / i8042prt) 的回调函数，从而在数据上报给系统类驱动
    (mouclass.sys) 之前修改原始鼠标数据包 (MOUSE_INPUT_DATA)。
    这是影响应用程序 Raw Input 读取结果的关键点。

    对于 USB (HID) 鼠标，只需关注 IOCTL_INTERNAL_MOUSE_CONNECT 流程；
    IOCTL_INTERNAL_I8042_HOOK_MOUSE 仅对 PS/2 (i8042prt) 有效。

--*/

// ------------------------------------------------------------
//  头文件内容（原 moufiltr.h）
// ------------------------------------------------------------
#ifndef MOUFILTER_H
#define MOUFILTER_H

#include <ntddk.h>
#include <kbdmou.h>
#include <ntddmou.h>
#include <ntdd8042.h>
#include <wdf.h>

#if DBG

#define TRAP()                      DbgBreakPoint()
#define DebugPrint(_x_)             DbgPrint _x_

#else   // DBG

#define TRAP()
#define DebugPrint(_x_)

#endif

// 设备扩展结构（存储上下文和原始回调）
typedef struct _DEVICE_EXTENSION
{
    // 原始上层（mouclass）的上下文和 ISR 钩子（PS/2 用）
    PVOID UpperContext;
    PI8042_MOUSE_ISR UpperIsrHook;

    // PS/2 端口驱动提供的写端口函数及其上下文
    IN PI8042_ISR_WRITE_PORT IsrWritePort;
    IN PVOID CallContext;

    // PS/2 端口驱动提供的队列数据包函数
    IN PI8042_QUEUE_PACKET QueueMousePacket;

    // 保存从 mouclass 传下来的原始连接参数（包含原始回调函数指针）
    CONNECT_DATA UpperConnectData;

} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

// 声明 WDF 上下文访问宏
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_EXTENSION, FilterGetData)

// ------------------------------------------------------------
//  新增：延迟事件缓冲区
// ------------------------------------------------------------
#define EVENT_BUFFER_SIZE 256

typedef struct _MOUSE_EVENT {
    BOOLEAN IsDown;         // TRUE=按下, FALSE=释放
    LONGLONG Time;          // 事件发生时间（100ns 单位）
    LONG LastX;             // 事件发生时的 X 坐标
    LONG LastY;             // 事件发生时的 Y 坐标
} MOUSE_EVENT;

typedef struct _EVENT_BUFFER {
    MOUSE_EVENT Events[EVENT_BUFFER_SIZE];
    volatile ULONG Head;    // 队头（最旧事件）
    volatile ULONG Tail;    // 队尾（下一个写入位置）
    WDFSPINLOCK Lock;
    WDFTIMER Timer;
    WDFDEVICE Device;       // 关联的设备对象，用于获取回调上下文
} EVENT_BUFFER;

// ------------------------------------------------------------
//  函数原型声明
// ------------------------------------------------------------
DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD MouFilter_EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL MouFilter_EvtIoInternalDeviceControl;

// 设备清理回调（参数为 WDFOBJECT）
VOID MouFilter_EvtDeviceCleanup(IN WDFOBJECT DeviceObject);

VOID
MouFilter_DispatchPassThrough(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target
);

BOOLEAN
MouFilter_IsrHook(
    PVOID         DeviceExtension,
    PMOUSE_INPUT_DATA       CurrentInput,
    POUTPUT_PACKET          CurrentOutput,
    UCHAR                   StatusByte,
    PUCHAR                  DataByte,
    PBOOLEAN                ContinueProcessing,
    PMOUSE_STATE            MouseState,
    PMOUSE_RESET_SUBSTATE   ResetSubState
);

VOID
MouFilter_ServiceCallback(
    IN PDEVICE_OBJECT DeviceObject,
    IN PMOUSE_INPUT_DATA InputDataStart,
    IN PMOUSE_INPUT_DATA InputDataEnd,
    IN OUT PULONG InputDataConsumed
);

// 定时器回调
EVT_WDF_TIMER EventBuffer_TimerCallback;

#endif  // MOUFILTER_H

// ------------------------------------------------------------
//  驱动实现（原 driver.c）
// ------------------------------------------------------------

#ifdef ALLOC_PRAGMA
#pragma alloc_text (INIT, DriverEntry)
#pragma alloc_text (PAGE, MouFilter_EvtDeviceAdd)
#pragma alloc_text (PAGE, MouFilter_EvtIoInternalDeviceControl)
#endif

#pragma warning(push)
#pragma warning(disable:4055) // type case from PVOID to PSERVICE_CALLBACK_ROUTINE
#pragma warning(disable:4152) // function/data pointer conversion in expression
#pragma warning(disable:4189)
// 全局事件缓冲区实例
static EVENT_BUFFER g_EventBuffer = { 0 };

// ============================================================
//  驱动入口点
// ============================================================
NTSTATUS
DriverEntry(
    IN  PDRIVER_OBJECT  DriverObject,
    IN  PUNICODE_STRING RegistryPath
)
/*++
例程描述:
    驱动初始化入口点，由 I/O 管理器在加载驱动时直接调用。
--*/
{
    WDF_DRIVER_CONFIG config;
    NTSTATUS status;

    DebugPrint(("Mouse Filter Driver Sample - Framework Edition.\n"));

    // 初始化驱动配置，指定设备添加回调
    WDF_DRIVER_CONFIG_INIT(&config, MouFilter_EvtDeviceAdd);

    // 创建框架驱动对象
    status = WdfDriverCreate(DriverObject,
        RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES,
        &config,
        WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfDriverCreate failed with status 0x%x\n", status));
    }
    return status;
}

// ============================================================
//  设备添加回调
// ============================================================
NTSTATUS
MouFilter_EvtDeviceAdd(
    IN WDFDRIVER        Driver,
    IN PWDFDEVICE_INIT  DeviceInit
)
/*++
例程描述:
    PnP 管理器调用 AddDevice 时触发，创建过滤设备并附加到驱动栈。
--*/
{
    WDF_OBJECT_ATTRIBUTES   deviceAttributes;
    NTSTATUS                status;
    WDFDEVICE               hDevice;
    WDF_IO_QUEUE_CONFIG     ioQueueConfig;

    UNREFERENCED_PARAMETER(Driver);
    PAGED_CODE();

    DebugPrint(("Enter FilterEvtDeviceAdd \n"));

    // 【关键】声明本驱动为过滤器驱动，框架会继承下层设备属性
    WdfFdoInitSetFilter(DeviceInit);

    // 设置设备类型为鼠标
    WdfDeviceInitSetDeviceType(DeviceInit, FILE_DEVICE_MOUSE);

    // ---- 初始化设备扩展上下文，并设置清理回调 ----
    // 注意：WDF 中设置清理回调的标准方式是通过 WDF_OBJECT_ATTRIBUTES
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&deviceAttributes, DEVICE_EXTENSION);
    deviceAttributes.EvtCleanupCallback = MouFilter_EvtDeviceCleanup; // 设置清理回调

    // 创建框架设备对象（底层会自动附加到驱动栈）
    status = WdfDeviceCreate(&DeviceInit, &deviceAttributes, &hDevice);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfDeviceCreate failed with status 0x%x\n", status));
        return status;
    }

    // ---- 新增：初始化事件缓冲区 ----
    // 保存设备句柄
    g_EventBuffer.Device = hDevice;
    g_EventBuffer.Head = 0;
    g_EventBuffer.Tail = 0;
    RtlZeroMemory(g_EventBuffer.Events, sizeof(g_EventBuffer.Events));

    // 创建自旋锁
    WDF_OBJECT_ATTRIBUTES spinLockAttr;
    WDF_OBJECT_ATTRIBUTES_INIT(&spinLockAttr);
    spinLockAttr.ParentObject = hDevice;
    status = WdfSpinLockCreate(&spinLockAttr, &g_EventBuffer.Lock);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfSpinLockCreate failed 0x%x\n", status));
        return status;
    }

    // 创建定时器，每 1ms 触发一次
    WDF_TIMER_CONFIG timerConfig;
    // 使用 INIT_PERIODIC 设置周期，周期单位是 100ns，1ms = 10000
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, EventBuffer_TimerCallback, 1);
    timerConfig.AutomaticSerialization = FALSE;  // 手动加锁

    WDF_OBJECT_ATTRIBUTES timerAttr;
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttr);
    timerAttr.ParentObject = hDevice;
    status = WdfTimerCreate(&timerConfig, &timerAttr, &g_EventBuffer.Timer);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfTimerCreate failed 0x%x\n", status));
        return status;
    }

    // 启动定时器，首次触发延迟 1ms（也可以设为 0，立即触发）
    WdfTimerStart(g_EventBuffer.Timer, WDF_REL_TIMEOUT_IN_MS(1));


    // ---- 配置默认 I/O 队列 ----
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&ioQueueConfig, WdfIoQueueDispatchParallel);

    // 注册内部 IOCTL 处理例程
    ioQueueConfig.EvtIoInternalDeviceControl = MouFilter_EvtIoInternalDeviceControl;

    status = WdfIoQueueCreate(hDevice,
        &ioQueueConfig,
        WDF_NO_OBJECT_ATTRIBUTES,
        WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        DebugPrint(("WdfIoQueueCreate failed 0x%x\n", status));
        return status;
    }

    return status;
}

// ============================================================
//  设备清理回调（停止定时器并释放资源）
// ============================================================
VOID
MouFilter_EvtDeviceCleanup(
    IN WDFOBJECT DeviceObject
)
{
    UNREFERENCED_PARAMETER(DeviceObject);

    DebugPrint(("MouFilter_EvtDeviceCleanup: stopping timer\n"));

    if (g_EventBuffer.Timer) {
        WdfTimerStop(g_EventBuffer.Timer, FALSE);
        WdfObjectDelete(g_EventBuffer.Timer);
        g_EventBuffer.Timer = NULL;
    }
    if (g_EventBuffer.Lock) {
        WdfObjectDelete(g_EventBuffer.Lock);
        g_EventBuffer.Lock = NULL;
    }
}

// ============================================================
//  请求透传函数（即发即忘，不关心完成）
// ============================================================
VOID
MouFilter_DispatchPassThrough(
    _In_ WDFREQUEST Request,
    _In_ WDFIOTARGET Target
)
{
    WDF_REQUEST_SEND_OPTIONS options;
    BOOLEAN ret;
    NTSTATUS status = STATUS_SUCCESS;

    WDF_REQUEST_SEND_OPTIONS_INIT(&options,
        WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);

    ret = WdfRequestSend(Request, Target, &options);
    if (ret == FALSE) {
        status = WdfRequestGetStatus(Request);
        DebugPrint(("WdfRequestSend failed: 0x%x\n", status));
        WdfRequestComplete(Request, status);
    }
}

// ============================================================
//  内部 IOCTL 分发例程（劫持连接和数据流的关键）
// ============================================================
VOID
MouFilter_EvtIoInternalDeviceControl(
    IN WDFQUEUE      Queue,
    IN WDFREQUEST    Request,
    IN size_t        OutputBufferLength,
    IN size_t        InputBufferLength,
    IN ULONG         IoControlCode
)
/*++
例程描述:
    处理内部设备控制请求。主要关注：
    [1] IOCTL_INTERNAL_MOUSE_CONNECT —— 核心机制，影响 Raw Input
    [2] IOCTL_INTERNAL_I8042_HOOK_MOUSE —— PS/2 专用 ISR 钩子
--*/
{
    PDEVICE_EXTENSION           devExt;
    PCONNECT_DATA               connectData;
    PINTERNAL_I8042_HOOK_MOUSE  hookMouse;
    NTSTATUS                    status = STATUS_SUCCESS;
    WDFDEVICE                   hDevice;
    size_t                      length;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    PAGED_CODE();

    hDevice = WdfIoQueueGetDevice(Queue);
    devExt = FilterGetData(hDevice);

    switch (IoControlCode) {

        // --------------------------------------------------------
        // 【情况一】鼠标类驱动（mouclass）连接端口驱动（mouhid/i8042prt）
        // 这是过滤 USB 鼠标数据、影响 Raw Input 的核心拦截点。
        // --------------------------------------------------------
    case IOCTL_INTERNAL_MOUSE_CONNECT:
        // 只允许一次连接
        if (devExt->UpperConnectData.ClassService != NULL) {
            status = STATUS_SHARING_VIOLATION;
            break;
        }

        // 获取上层传来的连接参数（包含原始回调函数指针）
        status = WdfRequestRetrieveInputBuffer(Request,
            sizeof(CONNECT_DATA),
            &connectData,
            &length);
        if (!NT_SUCCESS(status)) {
            DebugPrint(("WdfRequestRetrieveInputBuffer failed %x\n", status));
            break;
        }

        // 保存原始连接参数到设备扩展
        devExt->UpperConnectData = *connectData;

        // 【关键动作】偷梁换柱：
        // 将类设备对象换成自己的过滤设备，
        // 将类服务回调换成我们的 MouFilter_ServiceCallback。
        // 从此，所有鼠标原始数据包将先进入我们的回调。
        connectData->ClassDeviceObject = WdfDeviceWdmGetDeviceObject(hDevice);
        connectData->ClassService = MouFilter_ServiceCallback;

        break;

        // --------------------------------------------------------
        // 【情况二】断开连接（本示例未实现）
        // --------------------------------------------------------
    case IOCTL_INTERNAL_MOUSE_DISCONNECT:
        status = STATUS_NOT_IMPLEMENTED;
        break;

        // --------------------------------------------------------
        // 【情况三】PS/2 鼠标 ISR 钩子（仅对 i8042prt 有效）
        // --------------------------------------------------------
    case IOCTL_INTERNAL_I8042_HOOK_MOUSE:
        DebugPrint(("hook mouse received!\n"));

        status = WdfRequestRetrieveInputBuffer(Request,
            sizeof(INTERNAL_I8042_HOOK_MOUSE),
            &hookMouse,
            &length);
        if (!NT_SUCCESS(status)) {
            DebugPrint(("WdfRequestRetrieveInputBuffer failed %x\n", status));
            break;
        }

        // 保存原始上下文和 ISR 钩子，替换成自己的
        devExt->UpperContext = hookMouse->Context;
        hookMouse->Context = (PVOID)devExt;

        if (hookMouse->IsrRoutine) {
            devExt->UpperIsrHook = hookMouse->IsrRoutine;
        }
        hookMouse->IsrRoutine = (PI8042_MOUSE_ISR)MouFilter_IsrHook;

        // 保存端口驱动提供的辅助函数
        devExt->IsrWritePort = hookMouse->IsrWritePort;
        devExt->CallContext = hookMouse->CallContext;
        devExt->QueueMousePacket = hookMouse->QueueMousePacket;

        status = STATUS_SUCCESS;
        break;

        // --------------------------------------------------------
        // 其他 IOCTL（如查询属性）直接放行
        // --------------------------------------------------------
    case IOCTL_MOUSE_QUERY_ATTRIBUTES:
    default:
        break;
    }

    // 若处理失败，直接完成请求
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }

    // 将请求传递给下层驱动
    MouFilter_DispatchPassThrough(Request, WdfDeviceGetIoTarget(hDevice));
}

// ============================================================
//  PS/2 鼠标 ISR 钩子（若只过滤 USB 鼠标，可忽略或删除）
// ============================================================
BOOLEAN
MouFilter_IsrHook(
    PVOID         DeviceExtension,
    PMOUSE_INPUT_DATA       CurrentInput,
    POUTPUT_PACKET          CurrentOutput,
    UCHAR                   StatusByte,
    PUCHAR                  DataByte,
    PBOOLEAN                ContinueProcessing,
    PMOUSE_STATE            MouseState,
    PMOUSE_RESET_SUBSTATE   ResetSubState
)
{
    PDEVICE_EXTENSION devExt;
    BOOLEAN retVal = TRUE;

    devExt = DeviceExtension;

    // 如果有上层原始的 ISR 钩子，先调用它
    if (devExt->UpperIsrHook) {
        retVal = (*devExt->UpperIsrHook)(devExt->UpperContext,
            CurrentInput,
            CurrentOutput,
            StatusByte,
            DataByte,
            ContinueProcessing,
            MouseState,
            ResetSubState);
        if (!retVal || !(*ContinueProcessing)) {
            return retVal;
        }
    }

    *ContinueProcessing = TRUE;
    return retVal;
}

// ============================================================
//  【新增】定时器回调：检查并上报到期的左键事件
// ============================================================
VOID
EventBuffer_TimerCallback(
    IN WDFTIMER Timer
)
{
    if (Timer == NULL) {
        DebugPrint(("[moufiltr] Timer is NULL!\n"));
        return;
    }
    DebugPrint(("[moufiltr] Timer callback ENTERED\n"));
    static LONG callCount = 0;
    LONG count = InterlockedIncrement(&callCount);
    DebugPrint(("[moufiltr] Timer callback ENTERED, count=%ld\n", count));
    
    UNREFERENCED_PARAMETER(Timer);

    LONGLONG now = KeQueryInterruptTime();
    LONGLONG deadline = now - (12 * 10000); // 16ms 之前

    WdfSpinLockAcquire(g_EventBuffer.Lock);

    while (g_EventBuffer.Head != g_EventBuffer.Tail) {
        MOUSE_EVENT *pEvent = &g_EventBuffer.Events[g_EventBuffer.Head];
        if (pEvent->Time > deadline) {
            break; // 最早的事件还未到期
        }

        // 构造上报数据包
        MOUSE_INPUT_DATA report = { 0 };
        report.UnitId = 0;
        report.Flags = MOUSE_MOVE_RELATIVE;
        report.LastX = pEvent->LastX;
        report.LastY = pEvent->LastY;
        report.Buttons = pEvent->IsDown ? MOUSE_LEFT_BUTTON_DOWN : MOUSE_LEFT_BUTTON_UP;
        report.ExtraInformation = 0;

        // 获取原始回调
        PDEVICE_EXTENSION devExt = FilterGetData(g_EventBuffer.Device);
        ULONG consumed = 0;
        (*(PSERVICE_CALLBACK_ROUTINE)devExt->UpperConnectData.ClassService)(
            devExt->UpperConnectData.ClassDeviceObject,
            &report,
            &report + 1,
            &consumed
            );

        DebugPrint(("[moufiltr] Delayed event: %s at time %I64d\n",
            pEvent->IsDown ? "DOWN" : "UP", pEvent->Time));

        // 移动队头
        g_EventBuffer.Head = (g_EventBuffer.Head + 1) % EVENT_BUFFER_SIZE;
    }

    WdfSpinLockRelease(g_EventBuffer.Lock);
}

// ============================================================
//  【核心过滤函数】—— 修改鼠标原始数据，左键事件延迟16ms
// ============================================================
VOID
MouFilter_ServiceCallback(
    IN PDEVICE_OBJECT DeviceObject,
    IN PMOUSE_INPUT_DATA InputDataStart,
    IN PMOUSE_INPUT_DATA InputDataEnd,
    IN OUT PULONG InputDataConsumed
)
/*++
例程描述:
    每当底层端口驱动有鼠标数据包要上报时，本函数被调用。
    我们检测左键按下/释放事件，将其存入延迟缓冲区，并立即清除原包中的左键标志。
    定时器会延迟约16ms后上报这些事件。
--*/
{
    PDEVICE_EXTENSION   devExt;
    WDFDEVICE           hDevice;

    hDevice = WdfWdmDeviceGetWdfDeviceHandle(DeviceObject);
    devExt = FilterGetData(hDevice);

    // 遍历数据包
    for (PMOUSE_INPUT_DATA pData = InputDataStart; pData < InputDataEnd; ++pData) {
        // 检查是否有左键事件（按下或释放）
        ULONG leftBits = pData->Buttons & (MOUSE_LEFT_BUTTON_DOWN | MOUSE_LEFT_BUTTON_UP);

        if (leftBits) {
            // 判断是按下还是释放（数据包不会同时包含按下和释放）
            BOOLEAN isDown = (pData->Buttons & MOUSE_LEFT_BUTTON_DOWN) ? TRUE : FALSE;

            // 入队
            WdfSpinLockAcquire(g_EventBuffer.Lock);

            ULONG nextTail = (g_EventBuffer.Tail + 1) % EVENT_BUFFER_SIZE;
            if (nextTail != g_EventBuffer.Head) {
                MOUSE_EVENT *pEvent = &g_EventBuffer.Events[g_EventBuffer.Tail];
                pEvent->IsDown = isDown;
                pEvent->Time = KeQueryInterruptTime();
                pEvent->LastX = pData->LastX;
                pEvent->LastY = pData->LastY;
                g_EventBuffer.Tail = nextTail;
                DebugPrint(("[moufiltr] Queued event: %s at time %I64d\n",
                    isDown ? "DOWN" : "UP", pEvent->Time));
            }
            else {
                DebugPrint(("[moufiltr] WARNING: Event buffer full! Event lost.\n"));
            }

            WdfSpinLockRelease(g_EventBuffer.Lock);

            // 清除原数据包中的左键标志（阻止立即上报）
            pData->Buttons &= ~(MOUSE_LEFT_BUTTON_DOWN | MOUSE_LEFT_BUTTON_UP);
        }
        // 其他按钮和移动数据保持不变
    }

    // 【必须】调用原始的上层回调，上报修改后的数据包（不含左键事件）
    (*(PSERVICE_CALLBACK_ROUTINE)devExt->UpperConnectData.ClassService)(
        devExt->UpperConnectData.ClassDeviceObject,
        InputDataStart,
        InputDataEnd,
        InputDataConsumed
        );
}

#pragma warning(pop)