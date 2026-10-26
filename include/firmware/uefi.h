/*
 * Minimal hand-written UEFI 2.x definitions (x86-64 only).
 *
 * Only the types the bootloader actually uses are declared. Function slots we
 * do not call are typed as `void *` so the structure layout stays exact
 * without dragging in their prototypes. Every offset we rely on is checked
 * against the UEFI specification with a static assertion.
 *
 * Reference: UEFI Specification 2.10. Section numbers are given per type.
 */
#ifndef FIRMWARE_UEFI_H
#define FIRMWARE_UEFI_H

#include <stddef.h>
#include <stdint.h>

/* UEFI on x86-64 uses the Microsoft x64 calling convention. Our compile
 * target is already x86_64-windows, so this is the default, but it is stated
 * explicitly so the intent survives a change of target triple. */
#define EFIAPI __attribute__((ms_abi))

typedef uint64_t UINTN;
typedef uint16_t CHAR16; /* UCS-2; L"" literals match with -fshort-wchar */
typedef UINTN    EFI_STATUS;
typedef void    *EFI_HANDLE;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/* Status codes (Appendix D). Errors have the top bit set. */
#define EFI_ERROR_BIT          (1ULL << 63)
#define EFI_ERROR(s)           (((s) & EFI_ERROR_BIT) != 0)
#define EFI_SUCCESS            0ULL
#define EFI_LOAD_ERROR         (EFI_ERROR_BIT | 1)
#define EFI_INVALID_PARAMETER  (EFI_ERROR_BIT | 2)
#define EFI_UNSUPPORTED        (EFI_ERROR_BIT | 3)
#define EFI_BUFFER_TOO_SMALL   (EFI_ERROR_BIT | 5)
#define EFI_NOT_READY          (EFI_ERROR_BIT | 6)
#define EFI_OUT_OF_RESOURCES   (EFI_ERROR_BIT | 9)
#define EFI_VOLUME_CORRUPTED   (EFI_ERROR_BIT | 10)
#define EFI_NOT_FOUND          (EFI_ERROR_BIT | 14)
#define EFI_ABORTED            (EFI_ERROR_BIT | 21)

/* 4.2 EFI_TABLE_HEADER */
typedef struct {
    uint64_t Signature;
    uint32_t Revision;   /* (major << 16) | minor; minor 70 means "x.7.0" */
    uint32_t HeaderSize; /* bytes in the whole table, including this header */
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

#define EFI_SYSTEM_TABLE_SIGNATURE 0x5453595320494249ULL /* "IBI SYST" */
#define EFI_BOOT_SERVICES_SIGNATURE 0x56524553544F4F42ULL /* "BOOTSERV" */

/* 7.2 Memory allocation services */
#define AllocateAnyPages 0u

enum {
    EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode,
    EfiBootServicesData, EfiRuntimeServicesCode, EfiRuntimeServicesData,
    EfiConventionalMemory, EfiUnusableMemory, EfiACPIReclaimMemory,
    EfiACPIMemoryNVS, EfiMemoryMappedIO, EfiMemoryMappedIOPortSpace,
    EfiPalCode, EfiPersistentMemory, EfiUnacceptedMemoryType
};

#define EFI_MEMORY_NV            (1ULL << 15)
#define EFI_MEMORY_MORE_RELIABLE (1ULL << 16)
#define EFI_MEMORY_SP            (1ULL << 18)

/* Firmware may return descriptors larger than this struct: always step
 * through the map by the DescriptorSize GetMemoryMap returns. */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    uint64_t PhysicalStart;
    uint64_t VirtualStart;
    uint64_t NumberOfPages; /* 4 KiB pages, regardless of CPU page size */
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* 4.4 EFI_BOOT_SERVICES */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL;
    void *RestoreTPL;
    EFI_STATUS (EFIAPI *AllocatePages)(uint32_t Type, uint32_t MemoryType, UINTN Pages,
                                       EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(UINTN *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      UINTN *MapKey, UINTN *DescriptorSize,
                                      uint32_t *DescriptorVersion);
    void *AllocatePool;
    void *FreePool;
    void *CreateEvent;
    void *SetTimer;
    void *WaitForEvent;
    void *SignalEvent;
    void *CloseEvent;
    void *CheckEvent;
    void *InstallProtocolInterface;
    void *ReinstallProtocolInterface;
    void *UninstallProtocolInterface;
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    void *RegisterProtocolNotify;
    void *LocateHandle;
    void *LocateDevicePath;
    void *InstallConfigurationTable;
    void *LoadImage;
    void *StartImage;
    void *Exit;
    void *UnloadImage;
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, UINTN MapKey);
    void *GetNextMonotonicCount;
    EFI_STATUS (EFIAPI *Stall)(UINTN Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(UINTN Timeout, uint64_t WatchdogCode, UINTN DataSize,
                                          CHAR16 *WatchdogData);
    void *ConnectController;
    void *DisconnectController;
    void *OpenProtocol;
    void *CloseProtocol;
    void *OpenProtocolInformation;
    void *ProtocolsPerHandle;
    void *LocateHandleBuffer;
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
    void *InstallMultipleProtocolInterfaces;
    void *UninstallMultipleProtocolInterfaces;
    void *CalculateCrc32;
    void *CopyMem;
    void *SetMem;
    void *CreateEventEx;
} EFI_BOOT_SERVICES;

/* 12.4 EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL */
#define EFI_LIGHTGRAY            0x07
#define EFI_WHITE                0x0F
#define EFI_BACKGROUND_LIGHTGRAY 0x70

typedef struct {
    int32_t MaxMode;
    int32_t Mode;          /* current text mode, for QueryMode */
    int32_t Attribute;
    int32_t CursorColumn;
    int32_t CursorRow;
    uint8_t CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS (EFIAPI *OutputString)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, CHAR16 *String);
    void *TestString;
    EFI_STATUS (EFIAPI *QueryMode)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN ModeNumber,
                                   UINTN *Columns, UINTN *Rows);
    void *SetMode;
    EFI_STATUS (EFIAPI *SetAttribute)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Attribute);
    EFI_STATUS (EFIAPI *ClearScreen)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This);
    EFI_STATUS (EFIAPI *SetCursorPosition)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, UINTN Column,
                                           UINTN Row);
    EFI_STATUS (EFIAPI *EnableCursor)(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This, uint8_t Visible);
    SIMPLE_TEXT_OUTPUT_MODE *Mode;
};

/* 12.3 EFI_SIMPLE_TEXT_INPUT_PROTOCOL */
typedef struct {
    uint16_t ScanCode;    /* non-printing keys (arrows, F-keys); 0 otherwise */
    CHAR16   UnicodeChar; /* 0 for non-printing keys */
} EFI_INPUT_KEY;

/* EFI_INPUT_KEY.ScanCode values (Table 12-1) */
#define SCAN_UP     0x01
#define SCAN_DOWN   0x02
#define SCAN_RIGHT  0x03
#define SCAN_LEFT   0x04
#define SCAN_HOME   0x05
#define SCAN_END    0x06
#define SCAN_DELETE 0x08
#define SCAN_ESC    0x17

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_STATUS (EFIAPI *Reset)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, uint8_t ExtendedVerification);
    /* EFI_NOT_READY when no key is waiting; never blocks. */
    EFI_STATUS (EFIAPI *ReadKeyStroke)(EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This, EFI_INPUT_KEY *Key);
    void *WaitForKey;
};

/* 4.5 EFI_RUNTIME_SERVICES (8.2 variables, 8.5.1 ResetSystem) */
#define EFI_VARIABLE_NON_VOLATILE       0x1
#define EFI_VARIABLE_BOOTSERVICE_ACCESS 0x2
#define EFI_VARIABLE_RUNTIME_ACCESS     0x4
#define EFI_OS_INDICATIONS_BOOT_TO_FW_UI 0x1ULL

enum { EfiResetCold, EfiResetWarm, EfiResetShutdown };

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime;
    void *SetTime;
    void *GetWakeupTime;
    void *SetWakeupTime;
    void *SetVirtualAddressMap;
    void *ConvertPointer;
    EFI_STATUS (EFIAPI *GetVariable)(CHAR16 *Name, EFI_GUID *Vendor, uint32_t *Attributes,
                                     UINTN *DataSize, void *Data);
    void *GetNextVariableName;
    EFI_STATUS (EFIAPI *SetVariable)(CHAR16 *Name, EFI_GUID *Vendor, uint32_t Attributes,
                                     UINTN DataSize, void *Data);
    void *GetNextHighMonotonicCount;
    void (EFIAPI *ResetSystem)(uint32_t ResetType, EFI_STATUS ResetStatus, UINTN DataSize,
                               void *ResetData);
} EFI_RUNTIME_SERVICES;

/* 4.6 EFI_CONFIGURATION_TABLE */
typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* 4.3 EFI_SYSTEM_TABLE */
typedef struct {
    EFI_TABLE_HEADER                 Hdr;
    CHAR16                          *FirmwareVendor;
    uint32_t                         FirmwareRevision;
    EFI_HANDLE                       ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL  *ConIn;
    EFI_HANDLE                       ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE                       StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    EFI_RUNTIME_SERVICES            *RuntimeServices;
    EFI_BOOT_SERVICES               *BootServices;
    UINTN                            NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE         *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* 9.1 EFI_LOADED_IMAGE_PROTOCOL */
typedef struct {
    uint32_t          Revision;
    EFI_HANDLE        ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE        DeviceHandle; /* the volume we were loaded from */
    void             *FilePath;
    void             *Reserved;
    uint32_t          LoadOptionsSize;
    void             *LoadOptions;
    void             *ImageBase;
    uint64_t          ImageSize;
    uint32_t          ImageCodeType;
    uint32_t          ImageDataType;
    void             *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

/* 13.5 EFI_FILE_PROTOCOL */
#define EFI_FILE_MODE_READ 1ULL

typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;
struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName, uint64_t OpenMode, uint64_t Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    void *Delete;
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, UINTN *BufferSize, void *Buffer);
    void *Write;
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *This, uint64_t *Position);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, uint64_t Position);
    void *GetInfo;
    void *SetInfo;
    void *Flush;
};

/* 13.4 EFI_SIMPLE_FILE_SYSTEM_PROTOCOL */
typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;
struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
                                    EFI_FILE_PROTOCOL **Root);
};

/* 12.9 EFI_GRAPHICS_OUTPUT_PROTOCOL */
enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly
};

typedef struct {
    uint32_t RedMask, GreenMask, BlueMask, ReservedMask;
} EFI_PIXEL_BITMASK;

typedef struct {
    uint32_t          Version;
    uint32_t          HorizontalResolution;
    uint32_t          VerticalResolution;
    uint32_t          PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    uint32_t          PixelsPerScanLine; /* pixels are always 32 bits */
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t                              MaxMode;
    uint32_t                              Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    UINTN                                 SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                  FrameBufferBase;
    UINTN                                 FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    void                              *QueryMode;
    void                              *SetMode;
    void                              *Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* 13.9 EFI_BLOCK_IO_PROTOCOL (only the fields we read) */
typedef struct {
    uint32_t MediaId;
    uint8_t  RemovableMedia;
    uint8_t  MediaPresent;
    uint8_t  LogicalPartition; /* 1: this handle is a partition, not a disk */
    uint8_t  ReadOnly;
    uint8_t  WriteCaching;
    uint32_t BlockSize;
    uint32_t IoAlign;
    uint64_t LastBlock;
} EFI_BLOCK_IO_MEDIA;

typedef struct {
    uint64_t            Revision;
    EFI_BLOCK_IO_MEDIA *Media;
} EFI_BLOCK_IO_PROTOCOL;

/* 10.2-10.3 Device path nodes: a byte stream of {type, subtype, length16}
 * headers. Nodes are not aligned, so fields are read byte-wise. */
#define DP_TYPE_MESSAGING 0x03
#define DP_TYPE_MEDIA     0x04
#define DP_TYPE_END       0x7F
#define DP_MSG_ATAPI      0x01
#define DP_MSG_SCSI       0x02
#define DP_MSG_USB        0x05
#define DP_MSG_USB_CLASS  0x0F
#define DP_MSG_SATA       0x12
#define DP_MSG_NVME       0x17
#define DP_MSG_SD         0x1A
#define DP_MSG_EMMC       0x1D
#define DP_MEDIA_HD       0x01
#define DP_MEDIA_CDROM    0x02

/* Layout verification against the specification (x86-64, 8-byte pointers). */
_Static_assert(sizeof(EFI_GUID) == 16, "EFI_GUID size");
_Static_assert(sizeof(EFI_TABLE_HEADER) == 24, "EFI_TABLE_HEADER size");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, FirmwareVendor) == 24, "ST.FirmwareVendor");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, FirmwareRevision) == 32, "ST.FirmwareRevision");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, ConIn) == 48, "ST.ConIn");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, ConOut) == 64, "ST.ConOut");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, RuntimeServices) == 88, "ST.RuntimeServices");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, BootServices) == 96, "ST.BootServices");
_Static_assert(offsetof(EFI_RUNTIME_SERVICES, GetVariable) == 72, "RT.GetVariable");
_Static_assert(offsetof(EFI_RUNTIME_SERVICES, SetVariable) == 88, "RT.SetVariable");
_Static_assert(offsetof(EFI_RUNTIME_SERVICES, ResetSystem) == 104, "RT.ResetSystem");
_Static_assert(offsetof(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, ClearScreen) == 48, "ConOut.ClearScreen");
_Static_assert(offsetof(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, Mode) == 72, "ConOut.Mode");
_Static_assert(offsetof(SIMPLE_TEXT_OUTPUT_MODE, CursorVisible) == 20, "Mode.CursorVisible");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, NumberOfTableEntries) == 104, "ST.NumberOfTableEntries");
_Static_assert(offsetof(EFI_SYSTEM_TABLE, ConfigurationTable) == 112, "ST.ConfigurationTable");
_Static_assert(sizeof(EFI_SYSTEM_TABLE) == 120, "EFI_SYSTEM_TABLE size");
_Static_assert(offsetof(EFI_BOOT_SERVICES, AllocatePages) == 40, "BS.AllocatePages");
_Static_assert(offsetof(EFI_BOOT_SERVICES, GetMemoryMap) == 56, "BS.GetMemoryMap");
_Static_assert(offsetof(EFI_BOOT_SERVICES, HandleProtocol) == 152, "BS.HandleProtocol");
_Static_assert(offsetof(EFI_BOOT_SERVICES, ExitBootServices) == 232, "BS.ExitBootServices");
_Static_assert(offsetof(EFI_BOOT_SERVICES, Stall) == 248, "BS.Stall");
_Static_assert(offsetof(EFI_BOOT_SERVICES, SetWatchdogTimer) == 256, "BS.SetWatchdogTimer");
_Static_assert(offsetof(EFI_BOOT_SERVICES, LocateProtocol) == 320, "BS.LocateProtocol");
_Static_assert(sizeof(EFI_BOOT_SERVICES) == 376, "EFI_BOOT_SERVICES size");
_Static_assert(offsetof(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL, OutputString) == 8, "ConOut.OutputString");
_Static_assert(sizeof(EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL) == 80, "ConOut size");
_Static_assert(offsetof(EFI_SIMPLE_TEXT_INPUT_PROTOCOL, ReadKeyStroke) == 8, "ConIn.ReadKeyStroke");
_Static_assert(sizeof(EFI_INPUT_KEY) == 4, "EFI_INPUT_KEY size");
_Static_assert(sizeof(EFI_MEMORY_DESCRIPTOR) == 40, "EFI_MEMORY_DESCRIPTOR size");
_Static_assert(sizeof(EFI_CONFIGURATION_TABLE) == 24, "EFI_CONFIGURATION_TABLE size");
_Static_assert(offsetof(EFI_LOADED_IMAGE_PROTOCOL, DeviceHandle) == 24, "LI.DeviceHandle");
_Static_assert(offsetof(EFI_LOADED_IMAGE_PROTOCOL, LoadOptionsSize) == 48, "LI.LoadOptionsSize");
_Static_assert(offsetof(EFI_LOADED_IMAGE_PROTOCOL, LoadOptions) == 56, "LI.LoadOptions");
_Static_assert(offsetof(EFI_LOADED_IMAGE_PROTOCOL, ImageBase) == 64, "LI.ImageBase");
_Static_assert(offsetof(EFI_FILE_PROTOCOL, Read) == 32, "File.Read");
_Static_assert(offsetof(EFI_FILE_PROTOCOL, SetPosition) == 56, "File.SetPosition");
_Static_assert(sizeof(EFI_GRAPHICS_OUTPUT_MODE_INFORMATION) == 36, "GOP mode info size");
_Static_assert(offsetof(EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE, FrameBufferBase) == 24, "GOP.FrameBufferBase");
_Static_assert(offsetof(EFI_GRAPHICS_OUTPUT_PROTOCOL, Mode) == 24, "GOP.Mode");
_Static_assert(offsetof(EFI_BLOCK_IO_MEDIA, BlockSize) == 12, "Media.BlockSize");
_Static_assert(offsetof(EFI_BLOCK_IO_MEDIA, LastBlock) == 24, "Media.LastBlock");

#endif
