/*
 * efi.h — realboot 用到的最小 UEFI 定义（UEFI 2.x 规范，x86_64，MS ABI）。
 * 只声明用得到的服务；其余表项用 void* 占位，顺序必须与规范完全一致。
 */
#ifndef REALBOOT_EFI_H
#define REALBOOT_EFI_H

typedef unsigned char      UINT8;
typedef unsigned short     UINT16;
typedef unsigned int       UINT32;
typedef unsigned long long UINT64;
typedef UINT64             UINTN;
typedef UINT16             CHAR16;
typedef UINT8              BOOLEAN;
typedef UINTN              EFI_STATUS;
typedef void              *EFI_HANDLE;
typedef void              *EFI_EVENT;

#define EFIAPI __attribute__((ms_abi))
#define EFI_SUCCESS        0ULL
#define EFI_ERROR(s)       (((INT64_)(s)) < 0)
typedef long long INT64_;
#define EFI_ERR(n)         (0x8000000000000000ULL | (n))
#define EFI_NOT_READY      EFI_ERR(6)
#define EFI_NOT_FOUND      EFI_ERR(14)
#define EFI_SECURITY_VIOLATION EFI_ERR(26)

typedef struct { UINT32 a; UINT16 b, c; UINT8 d[8]; } EFI_GUID;

typedef struct {
    UINT64 Signature;
    UINT32 Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

/* ---- 文本输入输出 ---- */
typedef struct { UINT16 ScanCode; CHAR16 UnicodeChar; } EFI_INPUT_KEY;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *ReadKeyStroke)(struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL *, EFI_INPUT_KEY *);
    EFI_EVENT WaitForKey;
} EFI_SIMPLE_TEXT_INPUT_PROTOCOL;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    EFI_STATUS (EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, const CHAR16 *);
    void *TestString;
    void *QueryMode;
    void *SetMode;
    EFI_STATUS (EFIAPI *SetAttribute)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, UINTN);
    EFI_STATUS (EFIAPI *ClearScreen)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *);
    void *SetCursorPosition;
    EFI_STATUS (EFIAPI *EnableCursor)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, BOOLEAN);
    void *Mode;
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

#define EFI_BLACK        0x00
#define EFI_LIGHTGRAY    0x07
#define EFI_DARKGRAY     0x08
#define EFI_LIGHTCYAN    0x0B
#define EFI_YELLOW       0x0E
#define EFI_WHITE        0x0F

/* ---- Boot Services ---- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    void *AllocatePages, *FreePages, *GetMemoryMap;
    EFI_STATUS (EFIAPI *AllocatePool)(UINT32 type, UINTN size, void **buf);
    EFI_STATUS (EFIAPI *FreePool)(void *buf);
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE, const EFI_GUID *, void **);
    void *Reserved;
    void *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN boot_policy, EFI_HANDLE parent, void *device_path,
                                   void *src, UINTN src_size, EFI_HANDLE *out);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE, UINTN *exit_size, CHAR16 **exit_data);
    void *Exit, *UnloadImage, *ExitBootServices;
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN timeout, UINT64 code, UINTN size, CHAR16 *data);
    /* 之后的表项用不到 */
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    UINT32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    void *ConfigurationTable;
} EFI_SYSTEM_TABLE;

#define EFI_LOADER_DATA 2

/* ---- Loaded Image ---- */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } }

typedef struct {
    UINT32 Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    UINT32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    UINT64 ImageSize;
    UINT32 ImageCodeType, ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---- 文件系统（读 ESP 上的 \realIOS\*.txt）---- */
#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } }

typedef struct EFI_FILE_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *Open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **,
                              const CHAR16 *name, UINT64 mode, UINT64 attrs);
    EFI_STATUS (EFIAPI *Close)(struct EFI_FILE_PROTOCOL *);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(struct EFI_FILE_PROTOCOL *, UINTN *size, void *buf);
    void *Write, *GetPosition, *SetPosition, *GetInfo, *SetInfo, *Flush;
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    UINT64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

#define EFI_FILE_MODE_READ 0x1ULL

#endif
