#define DESCRIPTOR_DEF
#include "driver.h"

#define bool int

static ULONG EveTPDebugLevel = 100;
static ULONG EveTPDebugCatagories = DBG_INIT || DBG_PNP || DBG_IOCTL;

static bool deviceLoaded = false;

NTSTATUS
DriverEntry(
	__in PDRIVER_OBJECT  DriverObject,
	__in PUNICODE_STRING RegistryPath
)
{
	NTSTATUS               status = STATUS_SUCCESS;
	WDF_DRIVER_CONFIG      config;
	WDF_OBJECT_ATTRIBUTES  attributes;

	EveTPPrint(DEBUG_LEVEL_INFO, DBG_INIT,
		"Driver Entry\n");

	WDF_DRIVER_CONFIG_INIT(&config, EveTPEvtDeviceAdd);

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);

	//
	// Create a framework driver object to represent our driver.
	//

	status = WdfDriverCreate(DriverObject,
		RegistryPath,
		&attributes,
		&config,
		WDF_NO_HANDLE
	);

	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_INIT,
			"WdfDriverCreate failed with status 0x%x\n", status);
	}

	return status;
}

NTSTATUS i2c_hid_set_power(PEVETP_CONTEXT  pDevice, int power_state) {
	i2c_hid_command cmd = { 0 };
	cmd.reg = pDevice->desc.wCommandRegister;
	cmd.opcode = I2C_HID_OPCODE_SET_POWER;
	cmd.reportTypeID = power_state;

	return SpbWriteDataSynchronously(&pDevice->I2CContext, &cmd, sizeof(cmd));
}

NTSTATUS i2c_hid_write_feature(PEVETP_CONTEXT  pDevice, UINT8 reportID, UINT8* buf, size_t buf_len) {
	if (reportID == buf[0]) {
		buf++;
		buf_len--;
	}

	UINT8 rawReportType = 0x03;

	UINT16 size = 2 +
		(reportID ? 1 : 0)     /* reportID */ +
		buf_len     /* buf */;

	UINT16 argumentsLength = (reportID >= 0x0F ? 1 : 0)   /* optional third byte */ +
		2                                                 /* dataRegister */ +
		size                                              /* args */;

	UINT8* arguments = (UINT8*)ExAllocatePoolWithTag(NonPagedPool, argumentsLength, EVETP_POOL_TAG);
	if (!arguments) {
		return STATUS_NO_MEMORY;
	}
	memset(arguments, 0, argumentsLength);

	UINT8 idx = 0;
	if (reportID >= 0x0F) {
		arguments[idx++] = reportID;
		reportID = 0x0F;
	}

	UINT16 dataRegister = pDevice->desc.wDataRegister;
	arguments[idx++] = dataRegister & 0xFF;
	arguments[idx++] = dataRegister >> 8;

	arguments[idx++] = size & 0xFF;
	arguments[idx++] = size >> 8;

	if (reportID)
		arguments[idx++] = reportID;

	memcpy(&arguments[idx], buf, buf_len);

	UINT8 length = 4;
	i2c_hid_command *command = (i2c_hid_command*)ExAllocatePoolWithTag(NonPagedPool, 4 + argumentsLength, EVETP_POOL_TAG);
	if (!command) {
		ExFreePoolWithTag(arguments, EVETP_POOL_TAG);
		return STATUS_NO_MEMORY;
	}
	RtlZeroMemory(command, 4 + argumentsLength);
	command->reg = pDevice->desc.wCommandRegister;
	command->opcode = 0x03;
	command->reportTypeID = reportID | rawReportType << 4;

	UINT8* rawCmd = (UINT8*)command;
	RtlCopyMemory(rawCmd + length, arguments, argumentsLength);
	length += argumentsLength;

	NTSTATUS status = SpbWriteDataSynchronously(&pDevice->I2CContext, rawCmd, length);

	LARGE_INTEGER Interval;
	Interval.QuadPart = -10 * 1000 * 10;
	KeDelayExecutionThread(KernelMode, false, &Interval);

	ExFreePoolWithTag(command, EVETP_POOL_TAG);
	ExFreePoolWithTag(arguments, EVETP_POOL_TAG);

	return status;
}

NTSTATUS BOOTTRACKPAD(
	_In_  PEVETP_CONTEXT  pDevice
	)
{
	NTSTATUS status;

	i2c_hid_command cmd = { 0 };
	cmd.reg = 0x1;
	
	RtlZeroMemory(&pDevice->desc, sizeof(pDevice->desc));
	i2c_hid_descr* desc = &pDevice->desc;
	status = SpbXferDataSynchronously(&pDevice->I2CContext, &cmd.reg, 2, &pDevice->desc, sizeof(pDevice->desc));

	if (desc->bcdVersion != 0x0100 && desc->wHIDDescLength != sizeof(i2c_hid_descr)) {
		return STATUS_INVALID_PARAMETER;
	}

	{
		//Reset device
		status = i2c_hid_set_power(pDevice, I2C_HID_PWR_ON);
		if (!NT_SUCCESS(status)) {
			return status;
		}

		LARGE_INTEGER Interval;
		Interval.QuadPart = -10 * 1000 * 100;
		KeDelayExecutionThread(KernelMode, false, &Interval);

		cmd.reg = desc->wCommandRegister;
		cmd.opcode = I2C_HID_OPCODE_RESET;
		cmd.reportTypeID = 0;

		status = SpbWriteDataSynchronously(&pDevice->I2CContext, &cmd, sizeof(cmd));
		if (!NT_SUCCESS(status)) {
			DbgPrint("Failed to get send reset packet\n");
			return status;
		}

		Interval.QuadPart = -10 * 1000 * 100;
		KeDelayExecutionThread(KernelMode, false, &Interval);

		UINT8 nullBytes[2];
		status = SpbReadDataSynchronously(&pDevice->I2CContext, &nullBytes, 2);
		if (!NT_SUCCESS(status)) {
			DbgPrint("Failed to get ACK packet\n");
			return status;
		}
	}

	/*UINT8* reportDesc = ExAllocatePoolWithTag(NonPagedPool, desc->wReportDescLength, EVETP_POOL_TAG);
	RtlZeroMemory(reportDesc, desc->wReportDescLength);
	if (!reportDesc) {
		return STATUS_NO_MEMORY;
	}
	RtlZeroMemory(&cmd, sizeof(cmd));
	cmd.reg = desc->wReportDescRegister;

	status = SpbXferDataSynchronously(&pDevice->I2CContext, &cmd, 2, reportDesc, desc->wReportDescLength);
	DbgPrint("Read report desc? 0x%x\n", status);

	DbgPrint("Desc offset 0x40: 0x%02x 0x%02x 0x%02x 0x%02x\n", reportDesc[0x40], reportDesc[0x41], reportDesc[0x42], reportDesc[0x43]);

	{
		UNICODE_STRING     uniName;
		OBJECT_ATTRIBUTES  objAttr;

		RtlInitUnicodeString(&uniName, L"\\DosDevices\\C:\\hiddesc.bin");
		InitializeObjectAttributes(&objAttr, &uniName,
			OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
			NULL, NULL);

		HANDLE   handle;
		IO_STATUS_BLOCK    ioStatusBlock;

		status = ZwCreateFile(&handle,
			FILE_WRITE_DATA,
			&objAttr, &ioStatusBlock, NULL,
			FILE_ATTRIBUTE_NORMAL,
			0,
			FILE_OPEN_IF,
			FILE_SYNCHRONOUS_IO_NONALERT,
			NULL, 0);

		LARGE_INTEGER byteOffset;
		byteOffset.LowPart = byteOffset.HighPart = 0;
		ZwWriteFile(handle, NULL, NULL, NULL, &ioStatusBlock, reportDesc, desc->wReportDescLength, &byteOffset, NULL);

		ZwClose(handle);
	}

	ExFreePoolWithTag(reportDesc, EVETP_POOL_TAG);*/

	{ //Enable absolute mode
		eve_tp_config input_mode = { 0 };
		input_mode.ReportID = 12;
		input_mode.InputMode = INPUT_MODE_TOUCHPAD;

		status = i2c_hid_write_feature(pDevice, input_mode.ReportID, &input_mode, sizeof(input_mode));
		if (!NT_SUCCESS(status)) {
			return status;
		}
	}

	pDevice->phy_x_hid[0] = 0x06;
	pDevice->phy_x_hid[1] = 0x04;

	pDevice->max_x_hid[0] = 0x80;
	pDevice->max_x_hid[1] = 0x33;

	pDevice->phy_y_hid[0] = 0xA8;
	pDevice->phy_y_hid[1] = 0x02;

	pDevice->max_y_hid[0] = 0x00;
	pDevice->max_y_hid[1] = 0x22;

	pDevice->TrackpadBooted = true;

	return status;
}

NTSTATUS
OnPrepareHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesRaw,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

This routine caches the SPB resource connection ID.

Arguments:

FxDevice - a handle to the framework device object
FxResourcesRaw - list of translated hardware resources that
the PnP manager has assigned to the device
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PEVETP_CONTEXT pDevice = GetDeviceContext(FxDevice);
	BOOLEAN fSpbResourceFound = FALSE;
	NTSTATUS status = STATUS_INSUFFICIENT_RESOURCES;

	UNREFERENCED_PARAMETER(FxResourcesRaw);

	//
	// Parse the peripheral's resources.
	//

	ULONG resourceCount = WdfCmResourceListGetCount(FxResourcesTranslated);

	for (ULONG i = 0; i < resourceCount; i++)
	{
		PCM_PARTIAL_RESOURCE_DESCRIPTOR pDescriptor;
		UCHAR Class;
		UCHAR Type;

		pDescriptor = WdfCmResourceListGetDescriptor(
			FxResourcesTranslated, i);

		switch (pDescriptor->Type)
		{
		case CmResourceTypeConnection:
			//
			// Look for I2C or SPI resource and save connection ID.
			//
			Class = pDescriptor->u.Connection.Class;
			Type = pDescriptor->u.Connection.Type;
			if (Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
				Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C)
			{
				if (fSpbResourceFound == FALSE)
				{
					status = STATUS_SUCCESS;
					pDevice->I2CContext.I2cResHubId.LowPart = pDescriptor->u.Connection.IdLowPart;
					pDevice->I2CContext.I2cResHubId.HighPart = pDescriptor->u.Connection.IdHighPart;
					fSpbResourceFound = TRUE;
				}
				else
				{
				}
			}
			break;
		default:
			//
			// Ignoring all other resource types.
			//
			break;
		}
	}

	//
	// An SPB resource is required.
	//

	if (fSpbResourceFound == FALSE)
	{
		status = STATUS_NOT_FOUND;
	}

	status = SpbTargetInitialize(FxDevice, &pDevice->I2CContext);
	if (!NT_SUCCESS(status))
	{
		return status;
	}

	return status;
}

NTSTATUS
OnReleaseHardware(
_In_  WDFDEVICE     FxDevice,
_In_  WDFCMRESLIST  FxResourcesTranslated
)
/*++

Routine Description:

Arguments:

FxDevice - a handle to the framework device object
FxResourcesTranslated - list of raw hardware resources that
the PnP manager has assigned to the device

Return Value:

Status

--*/
{
	PEVETP_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status = STATUS_SUCCESS;

	UNREFERENCED_PARAMETER(FxResourcesTranslated);

	SpbTargetDeinitialize(FxDevice, &pDevice->I2CContext);

	return status;
}

NTSTATUS
OnD0Entry(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine allocates objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	PEVETP_CONTEXT pDevice = GetDeviceContext(FxDevice);
	NTSTATUS status;

	for (int i = 0; i < 5; i++){
		pDevice->Flags[i] = 0;
	}

	status = BOOTTRACKPAD(pDevice);

	pDevice->ConnectInterrupt = true;
	pDevice->RegsSet = false;

	return status;
}

NTSTATUS
OnD0Exit(
_In_  WDFDEVICE               FxDevice,
_In_  WDF_POWER_DEVICE_STATE  FxPreviousState
)
/*++

Routine Description:

This routine destroys objects needed by the driver.

Arguments:

FxDevice - a handle to the framework device object
FxPreviousState - previous power state

Return Value:

Status

--*/
{
	UNREFERENCED_PARAMETER(FxPreviousState);

	NTSTATUS status;

	PEVETP_CONTEXT pDevice = GetDeviceContext(FxDevice);

	status = i2c_hid_set_power(pDevice, I2C_HID_PWR_SLEEP);

	pDevice->ConnectInterrupt = false;

	return status;
}

BOOLEAN OnInterruptIsr(
	WDFINTERRUPT Interrupt,
	ULONG MessageID){
	UNREFERENCED_PARAMETER(MessageID);

	WDFDEVICE Device = WdfInterruptGetDevice(Interrupt);
	PEVETP_CONTEXT pDevice = GetDeviceContext(Device);

	if (!pDevice->ConnectInterrupt)
		return false;
	if (!pDevice->TrackpadBooted)
		return false;

	UINT8 rawData[66];
	memset(rawData, 0, sizeof(rawData));

	NTSTATUS status = SpbReadDataSynchronously(&pDevice->I2CContext, rawData, sizeof(rawData));
	if (!NT_SUCCESS(status))
		return false;

	eve_tp_report* rawReport = &rawData[2];
	if (rawReport->ReportID != 1) {
		return true;
	}

	LARGE_INTEGER CurrentTime;

	KeQuerySystemTime(&CurrentTime);

	LARGE_INTEGER DIFF;

	DIFF.QuadPart = 0;

	if (pDevice->LastTime.QuadPart != 0)
		DIFF.QuadPart = (CurrentTime.QuadPart - pDevice->LastTime.QuadPart) / 1000;

	struct _EVETP_MULTITOUCH_REPORT report;
	report.ReportID = REPORTID_MTOUCH;

	int x[MAX_EVE_FINGERID];
	int y[MAX_EVE_FINGERID];
	int p[MAX_EVE_FINGERID];
	int palm[MAX_EVE_FINGERID];
	for (int i = 0; i < MAX_EVE_FINGERID; i++) {
		x[i] = -1;
		y[i] = -1;
		p[i] = -1;
		palm[i] = 0;
	}

	UINT8 fingerCount = rawReport->button >> 1;
	for (UINT8 i = 0; i < fingerCount; i++) {
		BOOLEAN confidence = (rawReport->Touches[i].Status >> 0) & 0x1;
		BOOLEAN tipSwitch = (rawReport->Touches[i].Status >> 1) & 0x1;
		BOOLEAN inRange = (rawReport->Touches[i].Status >> 2) & 0x1;

		if (!tipSwitch)
			continue;

		UINT8 contactID = (rawReport->Touches[i].Status >> 3);
		if (contactID >= MAX_EVE_FINGERID)
			continue;

		x[contactID] = rawReport->Touches[i].XValue;
		y[contactID] = rawReport->Touches[i].YValue;
		p[contactID] = (rawReport->Touches[i].Pressure * 40) / 255;
		palm[contactID] = (rawReport->Touches[i].Width > 2500) || (rawReport->Touches[i].Height > 2500);

		/*DbgPrint("Finger %d [stat 0x%x]: Contact ID %d, %d x %d; z: %d, palm: %d x %d\n",
			i, rawReport->Touches[i].Status & 0x3,
			contactID,
			rawReport->Touches[i].XValue, rawReport->Touches[i].YValue,
			rawReport->Touches[i].Pressure,
			rawReport->Touches[i].Width, rawReport->Touches[i].Height);*/
	}

	for (int i = 0; i < MAX_EVE_FINGERID; i++) {
		if (pDevice->Flags[i] == MXT_T9_DETECT && x[i] == -1) {
			pDevice->Flags[i] = MXT_T9_RELEASE;
		}
		if (x[i] != -1) {
			pDevice->Flags[i] = MXT_T9_DETECT;

			pDevice->XValue[i] = (USHORT)x[i];
			pDevice->YValue[i] = (USHORT)y[i];
			pDevice->Palm[i] = (USHORT)palm[i];
			pDevice->PValue[i] = (USHORT)p[i];
		}
	}

	pDevice->BUTTONPRESSED = (rawReport->button & 0x01);

	pDevice->TIMEINT += (USHORT)DIFF.QuadPart;

	pDevice->LastTime = CurrentTime;

	BYTE count = 0, i = 0;
	while (count < 5 && i < MAX_EVE_FINGERID) {
		if (pDevice->Flags[i] != 0) {
			report.Touch[count].ContactID = i;

			report.Touch[count].XValue = pDevice->XValue[i];
			report.Touch[count].YValue = pDevice->YValue[i];
			report.Touch[count].Pressure = pDevice->PValue[i];

			UINT8 flags = pDevice->Flags[i];
			if (flags & MXT_T9_DETECT) {
				if (pDevice->Palm[i])
					report.Touch[count].Status = MULTI_TIPSWITCH_BIT;
				else
					report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_PRESS) {
				if (pDevice->Palm[i])
					report.Touch[count].Status = MULTI_TIPSWITCH_BIT;
				else
					report.Touch[count].Status = MULTI_CONFIDENCE_BIT | MULTI_TIPSWITCH_BIT;
			}
			else if (flags & MXT_T9_RELEASE) {
				report.Touch[count].Status = MULTI_CONFIDENCE_BIT;
				pDevice->Flags[i] = 0;
			}
			else
				report.Touch[count].Status = 0;

			count++;
		}
		i++;
	}

	report.ScanTime = pDevice->TIMEINT;
	report.IsDepressed = pDevice->BUTTONPRESSED;

	report.ContactCount = count;

	size_t bytesWritten;
	EveTPProcessVendorReport(pDevice, &report, sizeof(report), &bytesWritten);

	pDevice->RegsSet = true;
	return true;
}

NTSTATUS
EveTPEvtDeviceAdd(
IN WDFDRIVER       Driver,
IN PWDFDEVICE_INIT DeviceInit
)
{
	NTSTATUS                      status = STATUS_SUCCESS;
	WDF_IO_QUEUE_CONFIG           queueConfig;
	WDF_OBJECT_ATTRIBUTES         attributes;
	WDFDEVICE                     device;
	WDF_INTERRUPT_CONFIG interruptConfig;
	WDFQUEUE                      queue;
	PEVETP_CONTEXT               devContext;

	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	EveTPPrint(DEBUG_LEVEL_INFO, DBG_PNP,
		"EveTPEvtDeviceAdd called\n");

	//
	// Tell framework this is a filter driver. Filter drivers by default are  
	// not power policy owners. This works well for this driver because
	// HIDclass driver is the power policy owner for HID minidrivers.
	//

	WdfFdoInitSetFilter(DeviceInit);

	{
		WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
		WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);

		pnpCallbacks.EvtDevicePrepareHardware = OnPrepareHardware;
		pnpCallbacks.EvtDeviceReleaseHardware = OnReleaseHardware;
		pnpCallbacks.EvtDeviceD0Entry = OnD0Entry;
		pnpCallbacks.EvtDeviceD0Exit = OnD0Exit;

		WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);
	}

	//
	// Setup the device context
	//

	WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, EVETP_CONTEXT);

	//
	// Create a framework device object.This call will in turn create
	// a WDM device object, attach to the lower stack, and set the
	// appropriate flags and attributes.
	//

	status = WdfDeviceCreate(&DeviceInit, &attributes, &device);

	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfDeviceCreate failed with status code 0x%x\n", status);

		return status;
	}

	WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);

	queueConfig.EvtIoInternalDeviceControl = EveTPEvtInternalDeviceControl;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&queue
		);

	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create manual I/O queue to take care of hid report read requests
	//

	devContext = GetDeviceContext(device);

	devContext->FxDevice = device;
	devContext->TrackpadBooted = false;

	WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);

	queueConfig.PowerManaged = WdfFalse;

	status = WdfIoQueueCreate(device,
		&queueConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->ReportQueue
		);

	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"WdfIoQueueCreate failed 0x%x\n", status);

		return status;
	}

	//
	// Create an interrupt object for hardware notifications
	//
	WDF_INTERRUPT_CONFIG_INIT(
		&interruptConfig,
		OnInterruptIsr,
		NULL);
	interruptConfig.PassiveHandling = TRUE;

	status = WdfInterruptCreate(
		device,
		&interruptConfig,
		WDF_NO_OBJECT_ATTRIBUTES,
		&devContext->Interrupt);

	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_PNP,
			"Error creating WDF interrupt object - %!STATUS!",
			status);

		return status;
	}

	//
	// Initialize DeviceMode
	//

	devContext->DeviceMode = DEVICE_MODE_MOUSE;

	return status;
}

VOID
EveTPEvtInternalDeviceControl(
IN WDFQUEUE     Queue,
IN WDFREQUEST   Request,
IN size_t       OutputBufferLength,
IN size_t       InputBufferLength,
IN ULONG        IoControlCode
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	WDFDEVICE           device;
	PEVETP_CONTEXT     devContext;
	BOOLEAN             completeRequest = TRUE;

	UNREFERENCED_PARAMETER(OutputBufferLength);
	UNREFERENCED_PARAMETER(InputBufferLength);

	device = WdfIoQueueGetDevice(Queue);
	devContext = GetDeviceContext(device);

	EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
		"%s, Queue:0x%p, Request:0x%p\n",
		DbgHidInternalIoctlString(IoControlCode),
		Queue,
		Request
		);

	//
	// Please note that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl. So depending on the ioctl code, we will either
	// use retreive function or escape to WDM to get the UserBuffer.
	//

	switch (IoControlCode)
	{

	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		//
		// Retrieves the device's HID descriptor.
		//
		status = EveTPGetHidDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		//
		//Retrieves a device's attributes in a HID_DEVICE_ATTRIBUTES structure.
		//
		status = EveTPGetDeviceAttributes(Request);
		break;

	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		//
		//Obtains the report descriptor for the HID device.
		//
		status = EveTPGetReportDescriptor(device, Request);
		break;

	case IOCTL_HID_GET_STRING:
		//
		// Requests that the HID minidriver retrieve a human-readable string
		// for either the manufacturer ID, the product ID, or the serial number
		// from the string descriptor of the device. The minidriver must send
		// a Get String Descriptor request to the device, in order to retrieve
		// the string descriptor, then it must extract the string at the
		// appropriate index from the string descriptor and return it in the
		// output buffer indicated by the IRP. Before sending the Get String
		// Descriptor request, the minidriver must retrieve the appropriate
		// index for the manufacturer ID, the product ID or the serial number
		// from the device extension of a top level collection associated with
		// the device.
		//
		status = EveTPGetString(Request);
		break;

	case IOCTL_HID_WRITE_REPORT:
	case IOCTL_HID_SET_OUTPUT_REPORT:
		//
		//Transmits a class driver-supplied report to the device.
		//
		status = EveTPWriteReport(devContext, Request);
		break;

	case IOCTL_HID_READ_REPORT:
	case IOCTL_HID_GET_INPUT_REPORT:
		//
		// Returns a report from the device into a class driver-supplied buffer.
		// 
		status = EveTPReadReport(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_SET_FEATURE:
		//
		// This sends a HID class feature report to a top-level collection of
		// a HID class device.
		//
		status = EveTPSetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_GET_FEATURE:
		//
		// returns a feature report associated with a top-level collection
		//
		status = EveTPGetFeature(devContext, Request, &completeRequest);
		break;

	case IOCTL_HID_ACTIVATE_DEVICE:
		//
		// Makes the device ready for I/O operations.
		//
	case IOCTL_HID_DEACTIVATE_DEVICE:
		//
		// Causes the device to cease operations and terminate all outstanding
		// I/O requests.
		//
	default:
		status = STATUS_NOT_SUPPORTED;
		break;
	}

	if (completeRequest)
	{
		WdfRequestComplete(Request, status);

		EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s completed, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
			);
	}
	else
	{
		EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
			"%s deferred, Queue:0x%p, Request:0x%p\n",
			DbgHidInternalIoctlString(IoControlCode),
			Queue,
			Request
			);
	}

	return;
}

NTSTATUS
EveTPGetHidDescriptor(
IN WDFDEVICE Device,
IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	size_t              bytesToCopy = 0;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetHidDescriptor Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);

	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded "HID Descriptor" 
	//
	bytesToCopy = DefaultHidDescriptor.bLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0, // Offset
		(PVOID)&DefaultHidDescriptor,
		bytesToCopy);

	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetHidDescriptor Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
EveTPGetReportDescriptor(
IN WDFDEVICE Device,
IN WDFREQUEST Request
)
{
	NTSTATUS            status = STATUS_SUCCESS;
	ULONG_PTR           bytesToCopy;
	WDFMEMORY           memory;

	UNREFERENCED_PARAMETER(Device);

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetReportDescriptor Entry\n");

	PEVETP_CONTEXT devContext = GetDeviceContext(Device);

#define MT_TOUCH_COLLECTION												\
			MT_TOUCH_COLLECTION0 \
			0x26, devContext->max_x_hid[0], devContext->max_x_hid[1],                   /*       LOGICAL_MAXIMUM (WIDTH)    */ \
			0x46, devContext->phy_x_hid[0], devContext->phy_x_hid[1],                   /*       PHYSICAL_MAXIMUM (WIDTH)   */ \
			MT_TOUCH_COLLECTION1 \
			0x26, devContext->max_y_hid[0], devContext->max_y_hid[1],                   /*       LOGICAL_MAXIMUM (HEIGHT)    */ \
			0x46, devContext->phy_y_hid[0], devContext->phy_y_hid[1],                   /*       PHYSICAL_MAXIMUM (HEIGHT)   */ \
			MT_TOUCH_COLLECTION2

	HID_REPORT_DESCRIPTOR ReportDescriptor[] = {
		//
		// Multitouch report starts here
		//
		//TOUCH PAD input TLC
		0x05, 0x0d,                         // USAGE_PAGE (Digitizers)          
		0x09, 0x05,                         // USAGE (Touch Pad)             
		0xa1, 0x01,                         // COLLECTION (Application)         
		0x85, REPORTID_MTOUCH,            //   REPORT_ID (Touch pad)              
		0x09, 0x22,                         //   USAGE (Finger)                 
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		MT_TOUCH_COLLECTION
		USAGE_PAGES
	};

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputMemory(Request, &memory);
	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputMemory failed 0x%x\n", status);

		return status;
	}

	//
	// Use hardcoded Report descriptor
	//
	bytesToCopy = DefaultHidDescriptor.DescriptorList[0].wReportLength;

	if (bytesToCopy == 0)
	{
		status = STATUS_INVALID_DEVICE_STATE;

		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"DefaultHidDescriptor's reportLength is zero, 0x%x\n", status);

		return status;
	}

	status = WdfMemoryCopyFromBuffer(memory,
		0,
		(PVOID)ReportDescriptor,
		bytesToCopy);
	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfMemoryCopyFromBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, bytesToCopy);

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetReportDescriptor Exit = 0x%x\n", status);

	return status;
}


NTSTATUS
EveTPGetDeviceAttributes(
IN WDFREQUEST Request
)
{
	NTSTATUS                 status = STATUS_SUCCESS;
	PHID_DEVICE_ATTRIBUTES   deviceAttributes = NULL;

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetDeviceAttributes Entry\n");

	//
	// This IOCTL is METHOD_NEITHER so WdfRequestRetrieveOutputMemory
	// will correctly retrieve buffer from Irp->UserBuffer. 
	// Remember that HIDCLASS provides the buffer in the Irp->UserBuffer
	// field irrespective of the ioctl buffer type. However, framework is very
	// strict about type checking. You cannot get Irp->UserBuffer by using
	// WdfRequestRetrieveOutputMemory if the ioctl is not a METHOD_NEITHER
	// internal ioctl.
	//
	status = WdfRequestRetrieveOutputBuffer(Request,
		sizeof(HID_DEVICE_ATTRIBUTES),
		&deviceAttributes,
		NULL);
	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestRetrieveOutputBuffer failed 0x%x\n", status);

		return status;
	}

	//
	// Set USB device descriptor
	//

	deviceAttributes->Size = sizeof(HID_DEVICE_ATTRIBUTES);
	deviceAttributes->VendorID = EVETP_VID;
	deviceAttributes->ProductID = EVETP_PID;
	deviceAttributes->VersionNumber = EVETP_VERSION;

	//
	// Report how many bytes were copied
	//
	WdfRequestSetInformation(Request, sizeof(HID_DEVICE_ATTRIBUTES));

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetDeviceAttributes Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
EveTPGetString(
IN WDFREQUEST Request
)
{

	NTSTATUS status = STATUS_SUCCESS;
	PWSTR pwstrID;
	size_t lenID;
	WDF_REQUEST_PARAMETERS params;
	void *pStringBuffer = NULL;

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetString Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	switch ((ULONG_PTR)params.Parameters.DeviceIoControl.Type3InputBuffer & 0xFFFF)
	{
	case HID_STRING_ID_IMANUFACTURER:
		pwstrID = L"EveTP.\0";
		break;

	case HID_STRING_ID_IPRODUCT:
		pwstrID = L"MaxTouch Touch Screen\0";
		break;

	case HID_STRING_ID_ISERIALNUMBER:
		pwstrID = L"123123123\0";
		break;

	default:
		pwstrID = NULL;
		break;
	}

	lenID = pwstrID ? wcslen(pwstrID)*sizeof(WCHAR) + sizeof(UNICODE_NULL) : 0;

	if (pwstrID == NULL)
	{

		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"EveTPGetString Invalid request type\n");

		status = STATUS_INVALID_PARAMETER;

		return status;
	}

	status = WdfRequestRetrieveOutputBuffer(Request,
		lenID,
		&pStringBuffer,
		&lenID);

	if (!NT_SUCCESS(status))
	{

		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"EveTPGetString WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);

		return status;
	}

	RtlCopyMemory(pStringBuffer, pwstrID, lenID);

	WdfRequestSetInformation(Request, lenID);

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetString Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
EveTPWriteReport(
IN PEVETP_CONTEXT DevContext,
IN WDFREQUEST Request
)
{
	UNREFERENCED_PARAMETER(DevContext);

	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPWriteReport Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"EveTPWriteReport Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"EveTPWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			default:

				EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"EveTPWriteReport Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPWriteReport Exit = 0x%x\n", status);

	return status;

}

NTSTATUS
EveTPProcessVendorReport(
IN PEVETP_CONTEXT DevContext,
IN PVOID ReportBuffer,
IN ULONG ReportBufferLen,
OUT size_t* BytesWritten
)
{
	NTSTATUS status = STATUS_SUCCESS;
	WDFREQUEST reqRead;
	PVOID pReadReport = NULL;
	size_t bytesReturned = 0;

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPProcessVendorReport Entry\n");

	status = WdfIoQueueRetrieveNextRequest(DevContext->ReportQueue,
		&reqRead);

	if (NT_SUCCESS(status))
	{
		status = WdfRequestRetrieveOutputBuffer(reqRead,
			ReportBufferLen,
			&pReadReport,
			&bytesReturned);

		if (NT_SUCCESS(status))
		{
			//
			// Copy ReportBuffer into read request
			//

			if (bytesReturned > ReportBufferLen)
			{
				bytesReturned = ReportBufferLen;
			}

			RtlCopyMemory(pReadReport,
				ReportBuffer,
				bytesReturned);

			//
			// Complete read with the number of bytes returned as info
			//

			WdfRequestCompleteWithInformation(reqRead,
				status,
				bytesReturned);

			EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"EveTPProcessVendorReport %d bytes returned\n", bytesReturned);

			//
			// Return the number of bytes written for the write request completion
			//

			*BytesWritten = bytesReturned;

			EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
				"%s completed, Queue:0x%p, Request:0x%p\n",
				DbgHidInternalIoctlString(IOCTL_HID_READ_REPORT),
				DevContext->ReportQueue,
				reqRead);
		}
		else
		{
			EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"WdfRequestRetrieveOutputBuffer failed Status 0x%x\n", status);
		}
	}
	else
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfIoQueueRetrieveNextRequest failed Status 0x%x\n", status);
	}

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPProcessVendorReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
EveTPReadReport(
IN PEVETP_CONTEXT DevContext,
IN WDFREQUEST Request,
OUT BOOLEAN* CompleteRequest
)
{
	NTSTATUS status = STATUS_SUCCESS;

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPReadReport Entry\n");

	//
	// Forward this read request to our manual queue
	// (in other words, we are going to defer this request
	// until we have a corresponding write request to
	// match it with)
	//

	status = WdfRequestForwardToIoQueue(Request, DevContext->ReportQueue);

	if (!NT_SUCCESS(status))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"WdfRequestForwardToIoQueue failed Status 0x%x\n", status);
	}
	else
	{
		*CompleteRequest = FALSE;
	}

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPReadReport Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
EveTPSetFeature(
IN PEVETP_CONTEXT DevContext,
IN WDFREQUEST Request,
OUT BOOLEAN* CompleteRequest
)
{
	UNREFERENCED_PARAMETER(CompleteRequest);

	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;
	EveTPFeatureReport* pReport = NULL;

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPSetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.InputBufferLength < sizeof(HID_XFER_PACKET))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"EveTPSetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"EveTPWriteReport No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_FEATURE:

				if (transferPacket->reportBufferLen == sizeof(EveTPFeatureReport))
				{
					pReport = (EveTPFeatureReport*)transferPacket->reportBuffer;

					DevContext->DeviceMode = pReport->DeviceMode;

					EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"EveTPSetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"EveTPSetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(EveTPFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(EveTPFeatureReport));
				}

				break;

			default:

				EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"EveTPSetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPSetFeature Exit = 0x%x\n", status);

	return status;
}

NTSTATUS
EveTPGetFeature(
IN PEVETP_CONTEXT DevContext,
IN WDFREQUEST Request,
OUT BOOLEAN* CompleteRequest
)
{
	UNREFERENCED_PARAMETER(CompleteRequest);

	NTSTATUS status = STATUS_SUCCESS;
	WDF_REQUEST_PARAMETERS params;
	PHID_XFER_PACKET transferPacket = NULL;

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetFeature Entry\n");

	WDF_REQUEST_PARAMETERS_INIT(&params);
	WdfRequestGetParameters(Request, &params);

	if (params.Parameters.DeviceIoControl.OutputBufferLength < sizeof(HID_XFER_PACKET))
	{
		EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
			"EveTPGetFeature Xfer packet too small\n");

		status = STATUS_BUFFER_TOO_SMALL;
	}
	else
	{

		transferPacket = (PHID_XFER_PACKET)WdfRequestWdmGetIrp(Request)->UserBuffer;

		if (transferPacket == NULL)
		{
			EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
				"EveTPGetFeature No xfer packet\n");

			status = STATUS_INVALID_DEVICE_REQUEST;
		}
		else
		{
			//
			// switch on the report id
			//

			switch (transferPacket->reportId)
			{
			case REPORTID_MTOUCH:
			{

				EveTPMaxCountReport* pReport = NULL;
				EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
					"EveTPGetFeature MaximumCount = 0x%x\n", MULTI_MAX_COUNT);
				if (transferPacket->reportBufferLen >= sizeof(EveTPMaxCountReport))
				{
					pReport = (EveTPMaxCountReport*)transferPacket->reportBuffer;

					pReport->MaximumCount = MULTI_MAX_COUNT;

					pReport->PadType = 0;

					EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"EveTPGetFeature MaximumCount = 0x%x\n", MULTI_MAX_COUNT);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"EveTPGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(EveTPMaxCountReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(EveTPMaxCountReport));
				}

				break;
			}

			case REPORTID_FEATURE:
			{

				EveTPFeatureReport* pReport = NULL;

				if (transferPacket->reportBufferLen >= sizeof(EveTPFeatureReport))
				{
					pReport = (EveTPFeatureReport*)transferPacket->reportBuffer;

					pReport->DeviceMode = DevContext->DeviceMode;

					pReport->DeviceIdentifier = 0;

					EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"EveTPGetFeature DeviceMode = 0x%x\n", DevContext->DeviceMode);
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"EveTPGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(EveTPFeatureReport) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(EveTPFeatureReport));
				}

				break;
			}

			case REPORTID_PTPHQA:
			{
				UINT8 PTPHQA_BLOB[] = { REPORTID_PTPHQA, 0xfc, 0x28, 0xfe, 0x84, 0x40, 0xcb, 0x9a, 0x87, 0x0d, 0xbe, 0x57, 0x3c, 0xb6, 0x70, 0x09, 0x88, 0x07,\
					0x97, 0x2d, 0x2b, 0xe3, 0x38, 0x34, 0xb6, 0x6c, 0xed, 0xb0, 0xf7, 0xe5, 0x9c, 0xf6, 0xc2, 0x2e, 0x84,\
					0x1b, 0xe8, 0xb4, 0x51, 0x78, 0x43, 0x1f, 0x28, 0x4b, 0x7c, 0x2d, 0x53, 0xaf, 0xfc, 0x47, 0x70, 0x1b,\
					0x59, 0x6f, 0x74, 0x43, 0xc4, 0xf3, 0x47, 0x18, 0x53, 0x1a, 0xa2, 0xa1, 0x71, 0xc7, 0x95, 0x0e, 0x31,\
					0x55, 0x21, 0xd3, 0xb5, 0x1e, 0xe9, 0x0c, 0xba, 0xec, 0xb8, 0x89, 0x19, 0x3e, 0xb3, 0xaf, 0x75, 0x81,\
					0x9d, 0x53, 0xb9, 0x41, 0x57, 0xf4, 0x6d, 0x39, 0x25, 0x29, 0x7c, 0x87, 0xd9, 0xb4, 0x98, 0x45, 0x7d,\
					0xa7, 0x26, 0x9c, 0x65, 0x3b, 0x85, 0x68, 0x89, 0xd7, 0x3b, 0xbd, 0xff, 0x14, 0x67, 0xf2, 0x2b, 0xf0,\
					0x2a, 0x41, 0x54, 0xf0, 0xfd, 0x2c, 0x66, 0x7c, 0xf8, 0xc0, 0x8f, 0x33, 0x13, 0x03, 0xf1, 0xd3, 0xc1, 0x0b,\
					0x89, 0xd9, 0x1b, 0x62, 0xcd, 0x51, 0xb7, 0x80, 0xb8, 0xaf, 0x3a, 0x10, 0xc1, 0x8a, 0x5b, 0xe8, 0x8a,\
					0x56, 0xf0, 0x8c, 0xaa, 0xfa, 0x35, 0xe9, 0x42, 0xc4, 0xd8, 0x55, 0xc3, 0x38, 0xcc, 0x2b, 0x53, 0x5c,\
					0x69, 0x52, 0xd5, 0xc8, 0x73, 0x02, 0x38, 0x7c, 0x73, 0xb6, 0x41, 0xe7, 0xff, 0x05, 0xd8, 0x2b, 0x79,\
					0x9a, 0xe2, 0x34, 0x60, 0x8f, 0xa3, 0x32, 0x1f, 0x09, 0x78, 0x62, 0xbc, 0x80, 0xe3, 0x0f, 0xbd, 0x65,\
					0x20, 0x08, 0x13, 0xc1, 0xe2, 0xee, 0x53, 0x2d, 0x86, 0x7e, 0xa7, 0x5a, 0xc5, 0xd3, 0x7d, 0x98, 0xbe,\
					0x31, 0x48, 0x1f, 0xfb, 0xda, 0xaf, 0xa2, 0xa8, 0x6a, 0x89, 0xd6, 0xbf, 0xf2, 0xd3, 0x32, 0x2a, 0x9a,\
					0xe4, 0xcf, 0x17, 0xb7, 0xb8, 0xf4, 0xe1, 0x33, 0x08, 0x24, 0x8b, 0xc4, 0x43, 0xa5, 0xe5, 0x24, 0xc2 };
				if (transferPacket->reportBufferLen >= sizeof(PTPHQA_BLOB))
				{
					UINT8*blobBuffer = (UINT8*)transferPacket->reportBuffer;
					for (int i = 0; i < sizeof(PTPHQA_BLOB); i++) {
						blobBuffer[i] = PTPHQA_BLOB[i];
					}
					EveTPPrint(DEBUG_LEVEL_INFO, DBG_IOCTL,
						"EveTPGetFeature PHPHQA\n");
				}
				else
				{
					status = STATUS_INVALID_PARAMETER;

					EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
						"EveTPGetFeature Error transferPacket->reportBufferLen (%d) is different from sizeof(PTPHEQ_BLOB) (%d)\n",
						transferPacket->reportBufferLen,
						sizeof(EveTPFeatureReport));
				}
				break;
			}

			default:

				EveTPPrint(DEBUG_LEVEL_ERROR, DBG_IOCTL,
					"EveTPGetFeature Unhandled report type %d\n", transferPacket->reportId);

				status = STATUS_INVALID_PARAMETER;

				break;
			}
		}
	}

	EveTPPrint(DEBUG_LEVEL_VERBOSE, DBG_IOCTL,
		"EveTPGetFeature Exit = 0x%x\n", status);

	return status;
}

PCHAR
DbgHidInternalIoctlString(
IN ULONG IoControlCode
)
{
	switch (IoControlCode)
	{
	case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
		return "IOCTL_HID_GET_DEVICE_DESCRIPTOR";
	case IOCTL_HID_GET_REPORT_DESCRIPTOR:
		return "IOCTL_HID_GET_REPORT_DESCRIPTOR";
	case IOCTL_HID_READ_REPORT:
		return "IOCTL_HID_READ_REPORT";
	case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
		return "IOCTL_HID_GET_DEVICE_ATTRIBUTES";
	case IOCTL_HID_WRITE_REPORT:
		return "IOCTL_HID_WRITE_REPORT";
	case IOCTL_HID_SET_FEATURE:
		return "IOCTL_HID_SET_FEATURE";
	case IOCTL_HID_GET_FEATURE:
		return "IOCTL_HID_GET_FEATURE";
	case IOCTL_HID_GET_STRING:
		return "IOCTL_HID_GET_STRING";
	case IOCTL_HID_ACTIVATE_DEVICE:
		return "IOCTL_HID_ACTIVATE_DEVICE";
	case IOCTL_HID_DEACTIVATE_DEVICE:
		return "IOCTL_HID_DEACTIVATE_DEVICE";
	case IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST:
		return "IOCTL_HID_SEND_IDLE_NOTIFICATION_REQUEST";
	case IOCTL_HID_SET_OUTPUT_REPORT:
		return "IOCTL_HID_SET_OUTPUT_REPORT";
	case IOCTL_HID_GET_INPUT_REPORT:
		return "IOCTL_HID_GET_INPUT_REPORT";
	default:
		return "Unknown IOCTL";
	}
}