#include <ntddk.h>
#include <ntstrsafe.h>

VOID Unload(PDRIVER_OBJECT pDriverObject)
{
	DbgPrint("Goodbye!\r\n");
}


//Входная точка
//PDRIVER_OBJECT - адрес объекта драйвера
//PUNICODE_STRING - путь в реестре к подразделу драйвера
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
	UNICODE_STRING     uniName;
	OBJECT_ATTRIBUTES  objAttr;

	RtlInitUnicodeString(&uniName, L"\\DosDevices\\C:\\WINDOWS\\example.txt");  // or L"\\SystemRoot\\example.txt"
	InitializeObjectAttributes(&objAttr,
								&uniName,
								OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
								NULL,
								NULL);



	//Получаем дескриптор файла
	HANDLE   handle;
	NTSTATUS ntstatus;
	IO_STATUS_BLOCK    ioStatusBlock;

	// Do not try to perform any file operations at higher IRQL levels.
	// Instead, you may use a work item or a system worker thread to perform file operations.

	if (KeGetCurrentIrql() != PASSIVE_LEVEL)
		return STATUS_INVALID_DEVICE_STATE;

	ntstatus = ZwCreateFile(&handle,
							GENERIC_WRITE,
							&objAttr,
							&ioStatusBlock,
							NULL,
							FILE_ATTRIBUTE_NORMAL,
							0,
							FILE_OVERWRITE_IF,
							FILE_SYNCHRONOUS_IO_NONALERT,
							NULL, 0);



	CHAR     buffer[30];
	size_t  cb;

	if (NT_SUCCESS(ntstatus)) {
		ntstatus = RtlStringCbPrintfA(buffer, sizeof(buffer), "This is %d test\r\n", 0x0);
		if (NT_SUCCESS(ntstatus)) {
			ntstatus = RtlStringCbLengthA(buffer, sizeof(buffer), &cb);
			if (NT_SUCCESS(ntstatus)) {
				ntstatus = ZwWriteFile(handle, NULL, NULL, NULL, &ioStatusBlock,
					buffer, cb, NULL, NULL);
			}
		}
		ZwClose(handle);
	}

	DbgPrint("Hello world!\r\n");
	pDriverObject->DriverUnload = Unload;
	return STATUS_SUCCESS;

}

