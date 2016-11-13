#include <ntddk.h>

VOID Unload(PDRIVER_OBJECT pDriverObject)
{
	DbgPrint("Goodbye!");
}


//Входная точка
//PDRIVER_OBJECT - адрес объекта драйвера
//PUNICODE_STRING - путь в реестре к подразделу драйвера
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
	DbgPrint("Hello world!\n");
	pDriverObject->DriverUnload = Unload;
	return STATUS_SUCCESS;
}

