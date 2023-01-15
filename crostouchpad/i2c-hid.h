#include <pshpack1.h>
typedef struct i2c_hid_descr {
	UINT16 wHIDDescLength;
	UINT16 bcdVersion;
	UINT16 wReportDescLength;
	UINT16 wReportDescRegister;
	UINT16 wInputRegister;
	UINT16 wMaxInputLength;
	UINT16 wOutputRegister;
	UINT16 wMaxOutputLength;
	UINT16 wCommandRegister;
	UINT16 wDataRegister;
	UINT16 wVendorID;
	UINT16 wProductID;
	UINT16 wVersionID;
	UINT32 reserved;
} i2c_hid_descr;

typedef struct i2c_hid_command {
	UINT16 reg;
	UINT8 reportTypeID;
	UINT8 opcode;
} i2c_hid_command;

typedef struct eve_finger_report {
	BYTE Status; //Ignore all except for maybe tip switch -- conveniently confidence / tip switch is the same as ours. (0x4 is in range)
	//Contact identifier is upper 5 bits of Status
	USHORT    XValue;
	USHORT    YValue;
	USHORT	  Width;
	USHORT	  Height;
	BYTE	  Pressure;
	USHORT	  Azimuth;
} eve_finger_report;

typedef struct eve_tp_report {
	UINT8 ReportID; //1
	UINT8 button;
	//Upper 7 bits is contact count
	eve_finger_report Touches[5];
	USHORT ScanTime;
} eve_tp_report;

typedef struct eve_tp_config {
	UINT8 ReportID; //12
	UINT8 InputMode;
} eve_tp_config;

/* Command opcodes */
#define I2C_HID_OPCODE_RESET			0x01
#define I2C_HID_OPCODE_GET_REPORT		0x02
#define I2C_HID_OPCODE_SET_REPORT		0x03
#define I2C_HID_OPCODE_GET_IDLE			0x04
#define I2C_HID_OPCODE_SET_IDLE			0x05
#define I2C_HID_OPCODE_GET_PROTOCOL		0x06
#define I2C_HID_OPCODE_SET_PROTOCOL		0x07
#define I2C_HID_OPCODE_SET_POWER		0x08

#define I2C_HID_PWR_ON 0x00
#define I2C_HID_PWR_SLEEP 0x01

#define INPUT_MODE_MOUSE 0x00
#define INPUT_MODE_TOUCHPAD 0x03

#define MAX_EVE_FINGERID 0x1F
#include <poppack.h>