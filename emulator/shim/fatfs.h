// The FatFs calls the firmwares make, backed by the machine's VirtualCard
// (emulator/src/fatfs.cpp). Names, codes and macro semantics follow FatFs
// R0.12+, which libDaisy vendors.
#pragma once
#include <cstdint>

typedef unsigned int  UINT;
typedef unsigned char BYTE;
typedef uint16_t      WORD;
typedef uint32_t      DWORD;
typedef DWORD         FSIZE_t;
typedef char          TCHAR;

typedef enum
{
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED,
    FR_TIMEOUT,
    FR_LOCKED,
    FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES,
    FR_INVALID_PARAMETER
} FRESULT;

#define FA_READ 0x01
#define FA_WRITE 0x02
#define FA_CREATE_ALWAYS 0x08
#define FA_OPEN_ALWAYS 0x10

#define AM_DIR 0x10
#define AM_ARC 0x20

/** A mounted volume; the emulated card is always mounted. */
typedef struct
{
    BYTE fs_type;
} FATFS;

// The firmware reads obj.objsize directly and zeroes it after f_close() to mark
// a FIL as closed; f_size() == 0 is its "no file open" test.
typedef struct
{
    FSIZE_t objsize;
} FFOBJID;

typedef struct
{
    FFOBJID obj;
    BYTE    flag;
    FSIZE_t fptr;
    int32_t card_handle; // 0 = not open
} FIL;

typedef struct
{
    FSIZE_t fsize;
    WORD    fdate;
    WORD    ftime;
    BYTE    fattrib;
    TCHAR   fname[256];
} FILINFO;

typedef struct
{
    TCHAR path[256];
    TCHAR last[256]; // key of the entry f_readdir returned last ("": none yet)
    BYTE  open;
} DIR;

FRESULT f_open(FIL* fp, const TCHAR* path, BYTE mode);
FRESULT f_close(FIL* fp);
FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br);
FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw);
FRESULT f_lseek(FIL* fp, FSIZE_t ofs);
FRESULT f_truncate(FIL* fp);
FRESULT f_sync(FIL* fp);
FRESULT f_unlink(const TCHAR* path);
FRESULT f_rename(const TCHAR* path_old, const TCHAR* path_new);
FRESULT f_stat(const TCHAR* path, FILINFO* fno);
FRESULT f_opendir(DIR* dp, const TCHAR* path);
FRESULT f_readdir(DIR* dp, FILINFO* fno);
FRESULT f_closedir(DIR* dp);

#define f_eof(fp) ((int)((fp)->fptr == (fp)->obj.objsize))
#define f_tell(fp) ((fp)->fptr)
#define f_size(fp) ((fp)->obj.objsize)
#define f_rewind(fp) f_lseek((fp), 0)
