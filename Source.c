#include <ntddk.h>
#include <wdf.h>

//В этом простом примере мы использовали также директивы #pragma alloc_text(INIT, DriverEntry) и #pragma alloc_text(PAGE, UnloadRoutine). Объясню что они означают: первая помещает функцию DriverEntry в INIT секцию, то есть как бы говорит, что DriverEntry будет выполнена один раз и после этого код функции можно спокойно выгрузить из памяти. Вторая помечает код функции UnloadRoutine как выгружаемый, т.е. при необходимости, система может переместить его в файл подкачки, а потом забрать его оттуда.

VOID Unload(IN PDRIVER_OBJECT pDriverObject);
NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath);


#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, Unload)


VOID Unload(IN PDRIVER_OBJECT pDriverObject)
{
	UNICODE_STRING usDosDeviceName;
	RtlInitUnicodeString(&usDosDeviceName, L"\\DosDevices\\Example");

	DbgPrint("Unload Called \r\n");

	IoDeleteSymbolicLink(&usDosDeviceName);
	IoDeleteDevice(pDriverObject->DeviceObject);

	//return;
}

NTSTATUS UnSupportedFunction(PDEVICE_OBJECT pDeviceObject, PIRP Irp)
{
	NTSTATUS NtStatus = STATUS_NOT_SUPPORTED;
	DbgPrint("Example_UnSupportedFunction Called \r\n");

	return NtStatus;
}

//NTSTATUS WriteDirectIO(PDEVICE_OBJECT pDeviceObjct, PIRP Irp)
//NTSTATUS Example_WriteBufferedIO(PDEVICE_OBJECT DeviceObject, PIRP Irp)

/*
NTSTATUS WriteNeither(PDEVICE_OBJECT pDeviceObject, PIRP Irp)
{
	NTSTATUS NtStatus = STATUS_SUCCESS;
	PIO_STACK_LOCATION pIoStackIrp = NULL;
	PCHAR pWriteDataBuffer;

	DbgPrint("WriteNeither Called \r\n");

	if (pIoStackIrp)
	{
		__try {
			ProbeForRead(Irp->UserBuffer,
				pIoStackIrp->Parameters.Write.Length,
				TYPE_ALIGNMENT(char));
			pWriteDataBuffer = Irp->UserBuffer;

			if (pWriteDataBuffer)
			{
				if (IsStringTerminated(pWriteDataBuffer,
					pIoStackIrp->Parameters.Write.Length))
				{
					DbgPrint(pWriteDataBuffer);
				}
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			NtStatus = GetExceptionCode();
		}
	}
	return NtStatus;
}

*/

NTSTATUS DriverEntry(PDRIVER_OBJECT pDriverObject, PUNICODE_STRING pRegistryPath)
{
	NTSTATUS NtStatus = STATUS_SUCCESS;
	int uiIndex = 0;
	PDEVICE_OBJECT pDeviceObject = NULL;
	UNICODE_STRING usDriverName, usDosDeviceName; //Device names

	DbgPrint("DriverEntry Called \r\n");

	//initializes a UNICODE_STRING data structure.
	RtlInitUnicodeString(&usDriverName, L"\\Device\\Example");
	RtlInitUnicodeString(&usDosDeviceName, L"\\DosDevices\\Example");

	NtStatus = IoCreateDevice(pDriverObject, 0, //0 - the number of bytes to create for the device extension
								&usDriverName,
								FILE_DEVICE_UNKNOWN,
								FILE_DEVICE_SECURE_OPEN,
								FALSE, &pDeviceObject);

	if (NtStatus == STATUS_SUCCESS)
	{
		//IRP Major requests
		for (uiIndex = 0; uiIndex < IRP_MJ_MAXIMUM_FUNCTION; uiIndex++)
			pDriverObject->MajorFunction[uiIndex] = UnSupportedFunction;
		//pDriverObject->MajorFunction[IRP_MJ_CLOSE] = close;
		//pDriverObject->MajorFunction[IRP_MJ_CREATE] = create;
		//pDriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = IoControl;
		//pDriverObject->MajorFunction[IRP_MJ_READ] = Read;
		//pDriverObject->MajorFunction[IRP_MJ_WRITE] = WriteNeither;

		//Unload function
		pDriverObject->DriverUnload = Unload;

		pDeviceObject->Flags |= 0; //IO_TYPE;
		pDeviceObject->Flags &= (~DO_DEVICE_INITIALIZING);
		IoCreateSymbolicLink(&usDosDeviceName, &usDriverName);
	}

	return NtStatus;
}

