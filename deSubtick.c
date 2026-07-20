/*!
 * @file MouseTickFilter.c
 * @brief KMDF 上层鼠标过滤驱动：劫持物理左键，每 1/64 秒注入一次真实左键事件
 * @note  逻辑与用户态 EXE 第二版完全一致
 */

#include <ntddk.h>
#include <wdf.h>
#include <ntddmou.h>

#define TICK_INTERVAL_100NS 156250       // 1/64 秒 = 156250 * 100ns
#define SIMULATED_MAGIC     0xDEAD   // 与用户态 EXE 保持相同的魔数

typedef struct _DEVICE_EXTENSION {
    WDFTIMER  Timer;
    HANDLE    MouseHandle;      // 通过 ZwCreateFile 打开的鼠标设备句柄
    BOOLEAN   LeftButtonDown;   // 当前缓存的最新左键状态
    BOOLEAN   StateChanged;     // 自上次 tick 以来状态是否有变化
    BOOLEAN   Injecting;        // 正在注入（防止完成例程误劫持自己的包）
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_EXTENSION, FilterGetExtension)

// 前向声明
EVT_WDF_TIMER EvtTimerCallback;

/*!
 * @brief 打开鼠标设备文件，用于后续注入鼠标数据包
 */
NTSTATUS OpenMouseDevice(PDEVICE_EXTENSION DevExt)
{
    UNICODE_STRING mouseName;
    RtlInitUnicodeString(&mouseName, L"\\Device\\PointerClass0");

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, &mouseName,
        OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE,
        NULL, NULL);

    IO_STATUS_BLOCK iosb;
    return ZwCreateFile(&DevExt->MouseHandle,
        GENERIC_WRITE | SYNCHRONIZE,
        &oa, &iosb, NULL,
        FILE_ATTRIBUTE_NORMAL,
        0, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT,
        NULL, 0);
}

/*!
 * @brief 向鼠标设备注入一个左键按下或释放的数据包
 * @param DevExt 设备扩展
 * @param Down   TRUE=按下，FALSE=释放
 */
VOID InjectMousePacket(PDEVICE_EXTENSION DevExt, BOOLEAN Down)
{
    if (DevExt->MouseHandle == NULL)
        return;

    MOUSE_INPUT_DATA mid;
    RtlZeroMemory(&mid, sizeof(mid));
    mid.UnitId = 1;                     // 鼠标设备单元 ID
    mid.ButtonData = SIMULATED_MAGIC;   // 标记为模拟包

    if (Down)
        mid.ButtonFlags = MOUSE_LEFT_BUTTON_DOWN;
    else
        mid.ButtonFlags = MOUSE_LEFT_BUTTON_UP;

    IO_STATUS_BLOCK iosb;
    ZwWriteFile(DevExt->MouseHandle, NULL, NULL, NULL,
        &iosb, &mid, sizeof(mid), NULL, NULL);
}

/*!
 * @brief 周期定时器回调：每 1/64 秒检查状态变化并注入真实点击
 */
VOID EvtTimerCallback(WDFTIMER Timer)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PDEVICE_EXTENSION devExt = FilterGetExtension(device);

    if (!devExt->StateChanged)
        return;

    // 清除变化标志，并注入一次事件
    devExt->StateChanged = FALSE;

    devExt->Injecting = TRUE;          // 防止本次注入被自己的完成例程干扰
    InjectMousePacket(devExt, devExt->LeftButtonDown);
    devExt->Injecting = FALSE;
}

/*!
 * @brief IRP_MJ_READ 完成例程：拦截物理左键，放行模拟包
 */
NTSTATUS ReadCompletionRoutine(
    WDFREQUEST Request,
    WDFIOTARGET Target,
    PWDF_REQUEST_COMPLETION_PARAMS Params,
    WDFCONTEXT Context
)
{
    UNREFERENCED_PARAMETER(Target);
    PDEVICE_EXTENSION devExt = (PDEVICE_EXTENSION)Context;

    if (!NT_SUCCESS(Params->IoStatus.Status))
        return Params->IoStatus.Status;

    // 正在注入期间，不处理任何数据包（避免竞争）
    if (devExt->Injecting)
        return Params->IoStatus.Status;

    PVOID buffer;
    size_t length;
    NTSTATUS status = WdfRequestRetrieveOutputBuffer(Request,
        sizeof(MOUSE_INPUT_DATA),
        &buffer, &length);
    if (!NT_SUCCESS(status) || length < sizeof(MOUSE_INPUT_DATA))
        return status;

    PMOUSE_INPUT_DATA mouseData = (PMOUSE_INPUT_DATA)buffer;
    ULONG count = (ULONG)(length / sizeof(MOUSE_INPUT_DATA));
    ULONG newCount = 0;

    for (ULONG i = 0; i < count; i++) {
        // 识别并保留我们自己注入的包（魔数匹配）
        if (mouseData[i].ButtonData == SIMULATED_MAGIC) {
            if (newCount != i)
                mouseData[newCount] = mouseData[i];
            newCount++;
            continue;
        }

        // 物理左键按下/释放 → 更新缓存状态，并清除标志
        if (mouseData[i].ButtonFlags & MOUSE_LEFT_BUTTON_DOWN) {
            devExt->LeftButtonDown = TRUE;
            devExt->StateChanged = TRUE;
        }
        if (mouseData[i].ButtonFlags & MOUSE_LEFT_BUTTON_UP) {
            devExt->LeftButtonDown = FALSE;
            devExt->StateChanged = TRUE;
        }

        // 清除左键标志（劫持）
        mouseData[i].ButtonFlags &= ~(MOUSE_LEFT_BUTTON_DOWN | MOUSE_LEFT_BUTTON_UP);

        // 如果清除后数据包仍有效（移动/滚轮/其他按键），则保留
        if (mouseData[i].ButtonFlags != 0 ||
            mouseData[i].LastX != 0 ||
            mouseData[i].LastY != 0 ||
            mouseData[i].ButtonData != 0) {
            if (newCount != i)
                mouseData[newCount] = mouseData[i];
            newCount++;
        }
    }

    WdfRequestSetInformation(Request, newCount * sizeof(MOUSE_INPUT_DATA));
    return STATUS_SUCCESS;
}

/*!
 * @brief IRP_MJ_READ 派遣函数（设置完成例程）
 */
VOID DispatchRead(
    WDFQUEUE   Queue,
    WDFREQUEST Request,
    size_t     Length
)
{
    UNREFERENCED_PARAMETER(Length);
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    PDEVICE_EXTENSION devExt = FilterGetExtension(device);
    WDFIOTARGET ioTarget = WdfDeviceGetIoTarget(device);

    WdfRequestFormatRequestUsingCurrentType(Request);
    WdfRequestSetCompletionRoutine(Request, ReadCompletionRoutine, devExt);

    if (!WdfRequestSend(Request, ioTarget, WDF_NO_SEND_OPTIONS)) {
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

/*!
 * @brief 设备添加回调（设置过滤设备、队列、定时器、打开鼠标设备）
 */
NTSTATUS FilterEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit
)
{
    UNREFERENCED_PARAMETER(Driver);
    WdfFdoInitSetFilter(DeviceInit);

    WDF_IO_QUEUE_CONFIG queueConfig;
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoRead = DispatchRead;

    WDFDEVICE device;
    NTSTATUS status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &device);
    if (!NT_SUCCESS(status)) {
        KdPrint(("[MouseTick] WdfDeviceCreate failed: 0x%X\n", status));
        return status;
    }

    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        KdPrint(("[MouseTick] WdfIoQueueCreate failed: 0x%X\n", status));
        return status;
    }

    PDEVICE_EXTENSION devExt = FilterGetExtension(device);
    devExt->LeftButtonDown = FALSE;
    devExt->StateChanged = FALSE;
    devExt->Injecting = FALSE;

    // 打开鼠标设备（失败仅警告，不终止）
    status = OpenMouseDevice(devExt);
    KdPrint(("[MouseTick] OpenMouseDevice returned 0x%X\n", status));

    // 创建定时器
    WDF_TIMER_CONFIG timerConfig;
    WDF_TIMER_CONFIG_INIT_PERIODIC(&timerConfig, EvtTimerCallback, TICK_INTERVAL_100NS);
    WDF_OBJECT_ATTRIBUTES timerAttrs;
    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttrs);
    timerAttrs.ParentObject = device;

    status = WdfTimerCreate(&timerConfig, &timerAttrs, &devExt->Timer);
    if (!NT_SUCCESS(status)) {
        KdPrint(("[MouseTick] WdfTimerCreate failed: 0x%X\n", status));
        return status;   // 这里返回失败 → 服务启动报 1450
    }
    KdPrint(("[MouseTick] Timer created successfully.\n"));

    return STATUS_SUCCESS;
}

/*!
 * @brief DriverEntry
 */
NTSTATUS DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath
)
{
    WDF_DRIVER_CONFIG config;
    WDF_DRIVER_CONFIG_INIT(&config, FilterEvtDeviceAdd);
    config.DriverPoolTag = 'TikM';

    return WdfDriverCreate(DriverObject, RegistryPath,
        WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}