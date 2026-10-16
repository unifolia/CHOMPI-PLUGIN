// FatFs disk layer: the emulated card is always present.
#pragma once
#include "fatfs.h"

typedef BYTE DSTATUS;

typedef enum
{
    RES_OK = 0,
    RES_ERROR,
    RES_WRPRT,
    RES_NOTRDY,
    RES_PARERR
} DRESULT;

inline DSTATUS disk_status(BYTE)
{
    return RES_OK;
}
