/*
 * uefi.h - UEFI type definitions and protocol GUIDs
 *
 * Minimal UEFI definitions for a bootloader.  Uses the Microsoft x64
 * calling convention (EFIAPI / __attribute__((ms_abi))) as required by
 * the UEFI specification.
 *
 * Reference: UEFI Specification v2.10
 */
#ifndef BOOT_UEFI_H
#define BOOT_UEFI_H

#include <stdint.h>

/* ================================================================
 * EFIAPI calling convention
 * ================================================================ */
#if defined(__GNUC__) || defined(__clang__)
  #define EFIAPI  __attribute__((ms_abi))
#else
  #define EFIAPI
#endif

/* ================================================================
 * Basic types
 * ================================================================ */
typedef uint64_t  EFI_STATUS;
typedef void     *EFI_HANDLE;
typedef uint64_t  EFI_PHYSICAL_ADDRESS;
typedef uint64_t  EFI_VIRTUAL_ADDRESS;
typedef void     *EFI_EVENT;
typedef uint16_t  CHAR16;

/* ================================================================
 * Status codes
 * ================================================================ */
#define EFI_SUCCESS               0
#define EFI_LOAD_ERROR            1
#define EFI_INVALID_PARAMETER     2
#define EFI_UNSUPPORTED           3
#define EFI_BAD_BUFFER_SIZE       4
#define EFI_BUFFER_TOO_SMALL      5
#define EFI_NOT_READY             6
#define EFI_DEVICE_ERROR          7
#define EFI_WRITE_PROTECTED       8
#define EFI_OUT_OF_RESOURCES      9
#define EFI_VOLUME_CORRUPTED      10
#define EFI_VOLUME_FULL           11
#define EFI_NO_MEDIA              12
#define EFI_MEDIA_CHANGED         13
#define EFI_NOT_FOUND             14
#define EFI_ACCESS_DENIED         15
#define EFI_NO_RESPONSE           16
#define EFI_NO_MAPPING            17
#define EFI_TIMEOUT               18
#define EFI_NOT_STARTED           19
#define EFI_ALREADY_STARTED       20
#define EFI_ABORTED               21
#define EFI_ICMP_ERROR            22
#define EFI_TFTP_ERROR            23
#define EFI_PROTOCOL_ERROR        24
#define EFI_INCOMPATIBLE_VERSION  25
#define EFI_SECURITY_VIOLATION    26
#define EFI_CRC_ERROR             27
#define EFI_END_OF_MEDIA          28
#define EFI_END_OF_FILE           31
#define EFI_INVALID_LANGUAGE      32
#define EFI_COMPROMISED_DATA      33
#define EFI_IP_ADDRESS_CONFLICT   34
#define EFI_HTTP_ERROR            35

#define EFI_ERROR(status)  (((int64_t)(status)) < 0)
#define EFI_WARN_UNKNOWN_GLYPH   1
#define EFI_WARN_DELETE_FAILURE  2
#define EFI_WARN_WRITE_FAILURE   3
#define EFI_WARN_BUFFER_TOO_SMALL 4
#define EFI_WARN_STALE_DATA      5
#define EFI_WARN_FILE_SYSTEM     6
#define EFI_WARN_RESET_REQUIRED  7

/* ================================================================
 * Memory types
 * ================================================================ */
#define EfiReservedMemoryType     0
#define EfiLoaderCode             1
#define EfiLoaderData             2
#define EfiBootServicesCode       3
#define EfiBootServicesData       4
#define EfiRuntimeServicesCode    5
#define EfiRuntimeServicesData    6
#define EfiConventionalMemory     7
#define EfiUnusableMemory         8
#define EfiACPIReclaimMemory      9
#define EfiACPIMemoryNVS         10
#define EfiMemoryMappedIO        11
#define EfiMemoryMappedIOPortSpace 12
#define EfiPalCode               13
#define EfiPersistentMemory      14
#define EfiMaxMemoryType         15

/* Memory attribute flags */
#define EFI_MEMORY_UC             0x0000000000000001ULL
#define EFI_MEMORY_WC             0x0000000000000002ULL
#define EFI_MEMORY_WT             0x0000000000000004ULL
#define EFI_MEMORY_WB             0x0000000000000008ULL
#define EFI_MEMORY_UCE            0x0000000000000010ULL
#define EFI_MEMORY_WP             0x0000000000001000ULL
#define EFI_MEMORY_RP             0x0000000000002000ULL
#define EFI_MEMORY_XP             0x0000000000004000ULL
#define EFI_MEMORY_NV             0x0000000000008000ULL
#define EFI_MEMORY_MORE_RELIABLE  0x0000000000010000ULL
#define EFI_MEMORY_RO             0x0000000000020000ULL
#define EFI_MEMORY_SP             0x0000000000040000ULL
#define EFI_MEMORY_CPU_CRYPTO     0x0000000000080000ULL
#define EFI_MEMORY_RUNTIME        0x8000000000000000ULL

/* ================================================================
 * Allocate types
 * ================================================================ */
#define AllocateAnyPages    0
#define AllocateMaxAddress  1
#define AllocateAddress     2
#define AllocateMaxType     3

/* ================================================================
 * Timer types
 * ================================================================ */
#define TimerCancel          0
#define TimerPeriodic        1
#define TimerRelative        2

/* ================================================================
 * Memory descriptor
 * ================================================================ */
typedef struct {
    uint32_t Type;
    uint32_t Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    uint64_t NumberOfPages;
    uint64_t Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ================================================================
 * GUID
 * ================================================================ */
typedef struct {
    uint32_t Data1;
    uint16_t Data2;
    uint16_t Data3;
    uint8_t  Data4[8];
} EFI_GUID;

/* ================================================================
 * Table header
 * ================================================================ */
typedef struct {
    uint64_t Signature;
    uint32_t Revision;
    uint32_t HeaderSize;
    uint32_t CRC32;
    uint32_t Reserved;
} EFI_TABLE_HEADER;

/* ================================================================
 * Simple Text Output Protocol
 * ================================================================ */
typedef struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_TEXT_RESET)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    int8_t ExtendedVerification
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_TEST_STRING)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    CHAR16 *String
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_QUERY_MODE)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    uint64_t ModeNumber,
    uint64_t *Columns,
    uint64_t *Rows
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_MODE)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    uint64_t ModeNumber
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_ATTRIBUTE)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    uint64_t Attribute
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_CLEAR_SCREEN)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_SET_CURSOR_POSITION)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    uint64_t Column,
    uint64_t Row
);

typedef EFI_STATUS (EFIAPI *EFI_TEXT_ENABLE_CURSOR)(
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *This,
    int8_t Visible
);

typedef struct {
    int32_t  MaxMode;
    int32_t  Mode;
    int32_t  Attribute;
    int32_t  CursorColumn;
    int32_t  CursorRow;
    int8_t   CursorVisible;
} SIMPLE_TEXT_OUTPUT_MODE;

struct _EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    EFI_TEXT_RESET             Reset;
    EFI_TEXT_STRING            OutputString;
    EFI_TEXT_TEST_STRING       TestString;
    EFI_TEXT_QUERY_MODE        QueryMode;
    EFI_TEXT_SET_MODE          SetMode;
    EFI_TEXT_SET_ATTRIBUTE     SetAttribute;
    EFI_TEXT_CLEAR_SCREEN      ClearScreen;
    EFI_TEXT_SET_CURSOR_POSITION SetCursorPosition;
    EFI_TEXT_ENABLE_CURSOR     EnableCursor;
    SIMPLE_TEXT_OUTPUT_MODE    *Mode;
};

/* ================================================================
 * Simple Text Input Protocol
 * ================================================================ */
typedef struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

typedef struct {
    uint16_t ScanCode;
    CHAR16   UnicodeChar;
} EFI_INPUT_KEY;

typedef EFI_STATUS (EFIAPI *EFI_INPUT_RESET)(
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    int8_t ExtendedVerification
);

typedef EFI_STATUS (EFIAPI *EFI_INPUT_READ_KEY)(
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *This,
    EFI_INPUT_KEY *Key
);

struct _EFI_SIMPLE_TEXT_INPUT_PROTOCOL {
    EFI_INPUT_RESET     Reset;
    EFI_INPUT_READ_KEY  ReadKeyStroke;
    EFI_EVENT           WaitForKey;
};

typedef struct _EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL {
    EFI_INPUT_RESET     Reset;
    EFI_STATUS (EFIAPI *ReadKeyStrokeEx)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                          EFI_INPUT_KEY *Key);
    EFI_EVENT           WaitForKeyEx;
    EFI_STATUS (EFIAPI *SetState)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                   uint8_t *State);
    EFI_STATUS (EFIAPI *RegisterKeyNotify)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                            EFI_INPUT_KEY *KeyData,
                                            EFI_STATUS (EFIAPI *KeyNotificationFn)(EFI_INPUT_KEY *KeyData),
                                            void **NotifyHandle);
    EFI_STATUS (EFIAPI *UnregisterKeyNotify)(EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL *This,
                                              void *NotificationHandle);
} EFI_SIMPLE_TEXT_INPUT_EX_PROTOCOL;

/* ================================================================
 * Boot Services
 * ================================================================ */
typedef EFI_STATUS (EFIAPI *EFI_RAISE_TPL)(uint64_t NewTpl);
typedef void        (EFIAPI *EFI_RESTORE_TPL)(uint64_t OldTpl);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_PAGES)(
    uint32_t             Type,
    uint32_t             MemoryType,
    uint64_t             Pages,
    EFI_PHYSICAL_ADDRESS *Memory
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_PAGES)(
    EFI_PHYSICAL_ADDRESS Memory,
    uint64_t             Pages
);

typedef EFI_STATUS (EFIAPI *EFI_GET_MEMORY_MAP)(
    uint64_t                *MemoryMapSize,
    EFI_MEMORY_DESCRIPTOR   *MemoryMap,
    uint64_t                *MapKey,
    uint64_t                *DescriptorSize,
    uint32_t                *DescriptorVersion
);

typedef EFI_STATUS (EFIAPI *EFI_ALLOCATE_POOL)(
    uint32_t     PoolType,
    uint64_t     Size,
    void        **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FREE_POOL)(
    void *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_CREATE_EVENT)(
    uint32_t     Type,
    uint64_t     NotifyTpl,
    void        (EFIAPI *NotifyFunction)(EFI_EVENT Event, void *Context),
    void        *NotifyContext,
    EFI_EVENT   *Event
);

typedef EFI_STATUS (EFIAPI *EFI_SET_TIMER)(
    EFI_EVENT   Event,
    uint32_t    Type,
    uint64_t    TriggerTime
);

typedef EFI_STATUS (EFIAPI *EFI_WAIT_FOR_EVENT)(
    uint64_t     NumberOfEvents,
    EFI_EVENT   *Event,
    uint64_t    *Index
);

typedef EFI_STATUS (EFIAPI *EFI_SIGNAL_EVENT)(
    EFI_EVENT Event
);

typedef EFI_STATUS (EFIAPI *EFI_CLOSE_EVENT)(
    EFI_EVENT Event
);

typedef EFI_STATUS (EFIAPI *EFI_CHECK_EVENT)(
    EFI_EVENT Event
);

typedef EFI_STATUS (EFIAPI *EFI_INSTALL_PROTOCOL_INTERFACE)(
    EFI_HANDLE *Handle,
    EFI_GUID   *Protocol,
    uint32_t    InterfaceType,
    void       *Interface
);

typedef EFI_STATUS (EFIAPI *EFI_REINSTALL_PROTOCOL_INTERFACE)(
    EFI_HANDLE  Handle,
    EFI_GUID   *Protocol,
    void       *OldInterface,
    void       *NewInterface
);

typedef EFI_STATUS (EFIAPI *EFI_UNINSTALL_PROTOCOL_INTERFACE)(
    EFI_HANDLE  Handle,
    EFI_GUID   *Protocol,
    void       *Interface
);

typedef EFI_STATUS (EFIAPI *EFI_HANDLE_PROTOCOL)(
    EFI_HANDLE  Handle,
    EFI_GUID   *Protocol,
    void      **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_REGISTER_PROTOCOL_NOTIFY)(
    EFI_GUID   *Protocol,
    EFI_EVENT   Event,
    void      **Registration
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE)(
    uint32_t     SearchType,
    EFI_GUID    *Protocol,
    void        *SearchKey,
    uint64_t    *BufferSize,
    EFI_HANDLE  *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_DEVICE_PATH)(
    EFI_GUID    *Protocol,
    void       **DevicePath,
    EFI_HANDLE  *Device
);

typedef EFI_STATUS (EFIAPI *EFI_INSTALL_CONFIGURATION_TABLE)(
    EFI_GUID *Guid,
    void     *Table
);

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_LOAD)(
    int8_t      BootPolicy,
    EFI_HANDLE  ParentImageHandle,
    void        *DevicePath,
    void        *SourceBuffer,
    uint64_t     SourceSize,
    EFI_HANDLE  *ImageHandle
);

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_START)(
    EFI_HANDLE   ImageHandle,
    uint64_t     *ExitDataSize,
    CHAR16      **ExitData
);

typedef EFI_STATUS (EFIAPI *EFI_EXIT)(
    EFI_HANDLE   ImageHandle,
    EFI_STATUS   ExitStatus,
    uint64_t     ExitDataSize,
    CHAR16      *ExitData
);

typedef EFI_STATUS (EFIAPI *EFI_IMAGE_UNLOAD)(
    EFI_HANDLE ImageHandle
);

typedef EFI_STATUS (EFIAPI *EFI_EXIT_BOOT_SERVICES)(
    EFI_HANDLE  ImageHandle,
    uint64_t    MapKey
);

typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_MONOTONIC_COUNT)(
    uint64_t *Count
);

typedef EFI_STATUS (EFIAPI *EFI_STALL)(
    uint64_t Microseconds
);

typedef EFI_STATUS (EFIAPI *EFI_SET_WATCHDOG_TIMER)(
    uint64_t     Timeout,
    uint64_t     WatchdogCode,
    uint64_t     DataSize,
    CHAR16      *WatchdogData
);

typedef EFI_STATUS (EFIAPI *EFI_CONNECT_CONTROLLER)(
    EFI_HANDLE  ControllerHandle,
    EFI_HANDLE  *DriverImageHandle,
    void        *RemainingDevicePath,
    int8_t      Recursive
);

typedef EFI_STATUS (EFIAPI *EFI_DISCONNECT_CONTROLLER)(
    EFI_HANDLE  ControllerHandle,
    EFI_HANDLE  DriverImageHandle,
    EFI_HANDLE  ChildHandle
);

typedef EFI_STATUS (EFIAPI *EFI_OPEN_PROTOCOL)(
    EFI_HANDLE  Handle,
    EFI_GUID   *Protocol,
    void      **Interface,
    EFI_HANDLE  AgentHandle,
    EFI_HANDLE  ControllerHandle,
    uint32_t    Attributes
);

typedef EFI_STATUS (EFIAPI *EFI_CLOSE_PROTOCOL)(
    EFI_HANDLE  Handle,
    EFI_GUID   *Protocol,
    EFI_HANDLE  AgentHandle,
    EFI_HANDLE  ControllerHandle
);

typedef EFI_STATUS (EFIAPI *EFI_OPEN_PROTOCOL_INFORMATION)(
    EFI_HANDLE   Handle,
    EFI_GUID    *Protocol,
    void       **EntryBuffer,
    uint64_t    *EntryCount
);

typedef EFI_STATUS (EFIAPI *EFI_PROTOCOLS_PER_HANDLE)(
    EFI_HANDLE   Handle,
    void       ***ProtocolBuffer,
    uint64_t     *ProtocolBufferCount
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_HANDLE_BUFFER)(
    uint32_t     SearchType,
    EFI_GUID    *Protocol,
    void        *SearchKey,
    uint64_t    *NoHandles,
    EFI_HANDLE **Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_LOCATE_PROTOCOL)(
    EFI_GUID    *Protocol,
    void        *Registration,
    void       **Interface
);

typedef EFI_STATUS (EFIAPI *EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES)(
    EFI_HANDLE *Handle,
    ...
);

typedef EFI_STATUS (EFIAPI *EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES)(
    EFI_HANDLE Handle,
    ...
);

typedef EFI_STATUS (EFIAPI *EFI_CALCULATE_CRC32)(
    void        *Data,
    uint64_t     DataSize,
    uint32_t    *Crc32
);

typedef void (EFIAPI *EFI_COPY_MEM)(
    void    *Destination,
    void    *Source,
    uint64_t Length
);

typedef void (EFIAPI *EFI_SET_MEM)(
    void    *Buffer,
    uint64_t Size,
    uint8_t  Value
);

typedef EFI_STATUS (EFIAPI *EFI_CREATE_EVENT_EX)(
    uint32_t     Type,
    uint64_t     NotifyTpl,
    void        (EFIAPI *NotifyFunction)(EFI_EVENT Event, void *Context),
    const void  *NotifyContext,
    const EFI_GUID *EventGroup,
    EFI_EVENT   *Event
);

typedef struct {
    EFI_TABLE_HEADER                      Hdr;
    EFI_RAISE_TPL                         RaiseTPL;
    EFI_RESTORE_TPL                       RestoreTPL;
    EFI_ALLOCATE_PAGES                    AllocatePages;
    EFI_FREE_PAGES                        FreePages;
    EFI_GET_MEMORY_MAP                    GetMemoryMap;
    EFI_ALLOCATE_POOL                     AllocatePool;
    EFI_FREE_POOL                         FreePool;
    EFI_CREATE_EVENT                      CreateEvent;
    EFI_SET_TIMER                         SetTimer;
    EFI_WAIT_FOR_EVENT                    WaitForEvent;
    EFI_SIGNAL_EVENT                      SignalEvent;
    EFI_CLOSE_EVENT                       CloseEvent;
    EFI_CHECK_EVENT                       CheckEvent;
    EFI_INSTALL_PROTOCOL_INTERFACE        InstallProtocolInterface;
    EFI_REINSTALL_PROTOCOL_INTERFACE      ReinstallProtocolInterface;
    EFI_UNINSTALL_PROTOCOL_INTERFACE      UninstallProtocolInterface;
    EFI_HANDLE_PROTOCOL                   HandleProtocol;
    void                                 *Reserved;
    EFI_REGISTER_PROTOCOL_NOTIFY          RegisterProtocolNotify;
    EFI_LOCATE_HANDLE                     LocateHandle;
    EFI_LOCATE_DEVICE_PATH                LocateDevicePath;
    EFI_INSTALL_CONFIGURATION_TABLE       InstallConfigurationTable;
    EFI_IMAGE_LOAD                        LoadImage;
    EFI_IMAGE_START                       StartImage;
    EFI_EXIT                              Exit;
    EFI_IMAGE_UNLOAD                      UnloadImage;
    EFI_EXIT_BOOT_SERVICES                ExitBootServices;
    EFI_GET_NEXT_MONOTONIC_COUNT          GetNextMonotonicCount;
    EFI_STALL                             Stall;
    EFI_SET_WATCHDOG_TIMER                SetWatchdogTimer;
    EFI_CONNECT_CONTROLLER                ConnectController;
    EFI_DISCONNECT_CONTROLLER             DisconnectController;
    EFI_OPEN_PROTOCOL                     OpenProtocol;
    EFI_CLOSE_PROTOCOL                    CloseProtocol;
    EFI_OPEN_PROTOCOL_INFORMATION         OpenProtocolInformation;
    EFI_PROTOCOLS_PER_HANDLE              ProtocolsPerHandle;
    EFI_LOCATE_HANDLE_BUFFER              LocateHandleBuffer;
    EFI_LOCATE_PROTOCOL                   LocateProtocol;
    EFI_INSTALL_MULTIPLE_PROTOCOL_INTERFACES InstallMultipleProtocolInterfaces;
    EFI_UNINSTALL_MULTIPLE_PROTOCOL_INTERFACES UninstallMultipleProtocolInterfaces;
    EFI_CALCULATE_CRC32                   CalculateCrc32;
    EFI_COPY_MEM                          CopyMem;
    EFI_SET_MEM                           SetMem;
    EFI_CREATE_EVENT_EX                   CreateEventEx;
} EFI_BOOT_SERVICES;

/* ================================================================
 * Runtime Services
 * ================================================================ */
typedef EFI_STATUS (EFIAPI *EFI_GET_TIME)(
    void *Time, void *Capabilities
);
typedef EFI_STATUS (EFIAPI *EFI_SET_TIME)(void *Time);
typedef EFI_STATUS (EFIAPI *EFI_GET_WAKEUP_TIME)(
    int8_t *Enabled, int8_t *Pending, void *Time
);
typedef EFI_STATUS (EFIAPI *EFI_SET_WAKEUP_TIME)(int8_t Enable, void *Time);
typedef EFI_STATUS (EFIAPI *EFI_SET_VIRTUAL_ADDRESS_MAP)(
    uint64_t MemoryMapSize, uint64_t DescriptorSize,
    uint32_t DescriptorVersion, EFI_MEMORY_DESCRIPTOR *VirtualMap
);
typedef EFI_STATUS (EFIAPI *EFI_CONVERT_POINTER)(
    uint64_t DebugDisposition, void **Address
);
typedef EFI_STATUS (EFIAPI *EFI_GET_VARIABLE)(
    CHAR16 *VariableName, EFI_GUID *VendorGuid,
    uint32_t *Attributes, uint64_t *DataSize, void *Data
);
typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_VARIABLE_NAME)(
    uint64_t *VariableNameSize, CHAR16 *VariableName, EFI_GUID *VendorGuid
);
typedef EFI_STATUS (EFIAPI *EFI_SET_VARIABLE)(
    CHAR16 *VariableName, EFI_GUID *VendorGuid,
    uint32_t Attributes, uint64_t DataSize, void *Data
);
typedef EFI_STATUS (EFIAPI *EFI_GET_NEXT_HIGH_MONO_COUNT)(uint32_t *HighCount);
typedef void (EFIAPI *EFI_RESET_SYSTEM)(
    uint32_t ResetType, EFI_STATUS ResetStatus,
    uint64_t DataSize, void *ResetData
);

typedef struct {
    EFI_TABLE_HEADER              Hdr;
    EFI_GET_TIME                  GetTime;
    EFI_SET_TIME                  SetTime;
    EFI_GET_WAKEUP_TIME           GetWakeupTime;
    EFI_SET_WAKEUP_TIME           SetWakeupTime;
    EFI_SET_VIRTUAL_ADDRESS_MAP   SetVirtualAddressMap;
    EFI_CONVERT_POINTER           ConvertPointer;
    EFI_GET_VARIABLE              GetVariable;
    EFI_GET_NEXT_VARIABLE_NAME    GetNextVariableName;
    EFI_SET_VARIABLE              SetVariable;
    EFI_GET_NEXT_HIGH_MONO_COUNT  GetNextHighMonotonicCount;
    EFI_RESET_SYSTEM              ResetSystem;
    void                         *UefiRuntimeServicesLib;
} EFI_RUNTIME_SERVICES;

/* ================================================================
 * Configuration Table
 * ================================================================ */
typedef struct {
    EFI_GUID VendorGuid;
    void    *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ================================================================
 * System Table
 * ================================================================ */
typedef struct {
    EFI_TABLE_HEADER                  Hdr;
    CHAR16                           *FirmwareVendor;
    uint32_t                          FirmwareRevision;
    EFI_HANDLE                        ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL   *ConIn;
    EFI_HANDLE                        ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *ConOut;
    EFI_HANDLE                        StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL  *StdErr;
    EFI_RUNTIME_SERVICES             *RuntimeServices;
    EFI_BOOT_SERVICES                *BootServices;
    uint64_t                          NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE          *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ================================================================
 * Protocol GUIDs
 * ================================================================ */
#define EFI_LOADED_IMAGE_PROTOCOL_GUID \
    {0x5B1B31A1, 0x9562, 0x11D2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID \
    {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}}

#define EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID \
    {0x0964E5B2, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_FILE_INFO_GUID \
    {0x09576E92, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

#define EFI_DEVICE_PATH_PROTOCOL_GUID \
    {0x09576E91, 0x6D3F, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}}

/* ================================================================
 * Loaded Image Protocol
 * ================================================================ */
typedef struct {
    uint32_t        Revision;
    EFI_HANDLE      ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE      DeviceHandle;
    void           *FilePath;
    void           *Reserved;
    uint32_t        LoadOptionsSize;
    void           *LoadOptions;
    void           *ImageBase;
    uint64_t        ImageSize;
    uint32_t        ImageCodeType;
    uint32_t        ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

/* ================================================================
 * Graphics Output Protocol (GOP)
 * ================================================================ */
typedef struct {
    uint32_t RedMask;
    uint32_t GreenMask;
    uint32_t BlueMask;
    uint32_t ReservedMask;
} EFI_PIXEL_BITMASK;

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor,
    PixelBlueGreenRedReserved8BitPerColor,
    PixelBitMask,
    PixelBltOnly,
    PixelFormatMax
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    uint32_t                    Version;
    uint32_t                    HorizontalResolution;
    uint32_t                    VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT   PixelFormat;
    EFI_PIXEL_BITMASK           PixelInformation;
    uint32_t                    PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    uint32_t                            MaxMode;
    uint32_t                            Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    uint64_t                            SizeOfInfo;
    EFI_PHYSICAL_ADDRESS                FrameBufferBase;
    uint64_t                            FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct _EFI_GRAPHICS_OUTPUT_PROTOCOL EFI_GRAPHICS_OUTPUT_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL             *This,
    uint32_t                                  ModeNumber,
    uint64_t                                 *SizeOfInfo,
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION    **Info
);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL *This,
    uint32_t                      ModeNumber
);

typedef EFI_STATUS (EFIAPI *EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT)(
    EFI_GRAPHICS_OUTPUT_PROTOCOL  *This,
    void                          *BltBuffer,
    uint64_t                       BltOperation,
    uint64_t                       SourceX,
    uint64_t                       SourceY,
    uint64_t                       DestinationX,
    uint64_t                       DestinationY,
    uint64_t                       Width,
    uint64_t                       Height,
    uint64_t                       Delta
);

struct _EFI_GRAPHICS_OUTPUT_PROTOCOL {
    EFI_GRAPHICS_OUTPUT_PROTOCOL_QUERY_MODE  QueryMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_SET_MODE    SetMode;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_BLT         Blt;
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE       *Mode;
};

/* ================================================================
 * File Protocol
 * ================================================================ */
typedef struct _EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_FILE_OPEN)(
    EFI_FILE_PROTOCOL  *This,
    EFI_FILE_PROTOCOL **NewHandle,
    CHAR16             *FileName,
    uint64_t            OpenMode,
    uint64_t            Attributes
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_CLOSE)(
    EFI_FILE_PROTOCOL *This
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_DELETE)(
    EFI_FILE_PROTOCOL *This
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_READ)(
    EFI_FILE_PROTOCOL *This,
    uint64_t          *BufferSize,
    void              *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_WRITE)(
    EFI_FILE_PROTOCOL *This,
    uint64_t          *BufferSize,
    void              *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_POSITION)(
    EFI_FILE_PROTOCOL *This,
    uint64_t          *Position
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_POSITION)(
    EFI_FILE_PROTOCOL *This,
    uint64_t           Position
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_GET_INFO)(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID          *InformationType,
    uint64_t          *BufferSize,
    void              *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_SET_INFO)(
    EFI_FILE_PROTOCOL *This,
    EFI_GUID          *InformationType,
    uint64_t           BufferSize,
    void              *Buffer
);

typedef EFI_STATUS (EFIAPI *EFI_FILE_FLUSH)(
    EFI_FILE_PROTOCOL *This
);

struct _EFI_FILE_PROTOCOL {
    uint64_t              Revision;
    EFI_FILE_OPEN         Open;
    EFI_FILE_CLOSE        Close;
    EFI_FILE_DELETE       Delete;
    EFI_FILE_READ         Read;
    EFI_FILE_WRITE        Write;
    EFI_FILE_GET_POSITION GetPosition;
    EFI_FILE_SET_POSITION SetPosition;
    EFI_FILE_GET_INFO     GetInfo;
    EFI_FILE_SET_INFO     SetInfo;
    EFI_FILE_FLUSH        Flush;
};

/* File open modes */
#define EFI_FILE_MODE_READ       0x0000000000000001ULL
#define EFI_FILE_MODE_WRITE      0x0000000000000002ULL
#define EFI_FILE_MODE_CREATE     0x8000000000000000ULL

/* File attributes */
#define EFI_FILE_READ_ONLY       0x0000000000000001ULL
#define EFI_FILE_HIDDEN          0x0000000000000002ULL
#define EFI_FILE_SYSTEM          0x0000000000000004ULL
#define EFI_FILE_RESERVED        0x0000000000000008ULL
#define EFI_FILE_DIRECTORY       0x0000000000000010ULL
#define EFI_FILE_ARCHIVE         0x0000000000000020ULL
#define EFI_FILE_VALID_ATTR      0x0000000000000037ULL

/* File info */
typedef struct {
    uint64_t  Size;
    uint64_t  FileSize;
    uint64_t  PhysicalSize;
    void     *CreateTime;
    void     *LastAccessTime;
    void     *ModificationTime;
    uint64_t  Attribute;
    CHAR16    FileName[1];
} EFI_FILE_INFO;

/* ================================================================
 * Simple File System Protocol
 * ================================================================ */
typedef struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef EFI_STATUS (EFIAPI *EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME)(
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *This,
    EFI_FILE_PROTOCOL              **Root
);

struct _EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t                                      Revision;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_OPEN_VOLUME   OpenVolume;
};

/* ================================================================
 * Device Path Protocol
 * ================================================================ */
typedef struct {
    uint8_t  Type;
    uint8_t  SubType;
    uint8_t  Length[2];
} EFI_DEVICE_PATH_PROTOCOL;

/* Device path utilities */
#define EFI_DEVICE_PATH_PROTOCOL_END_ENTIRE     0xFF
#define EFI_DEVICE_PATH_PROTOCOL_END_INSTANCE   0x01
#define END_DEVICE_PATH_LENGTH                  4

/* ================================================================
 * Helper: GUID comparison
 * ================================================================ */
static inline int guid_equals(EFI_GUID *a, EFI_GUID *b) {
    uint32_t *pa = (uint32_t *)a;
    uint32_t *pb = (uint32_t *)b;
    return (pa[0] == pb[0]) && (pa[1] == pb[1]) && (pa[2] == pb[2]) && (pa[3] == pb[3]);
}

#endif /* BOOT_UEFI_H */