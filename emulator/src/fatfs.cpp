#include "fatfs.h"

#include <cstring>
#include "chompi/VirtualCard.h"
#include "emu_context.h"

namespace
{
emu::VirtualCard* Card()
{
    return emu::current ? emu::current->card : nullptr;
}

/** The card behind an open FIL, or nullptr. */
emu::VirtualCard* CardFor(const FIL* fp)
{
    return fp != nullptr && fp->card_handle != 0 ? Card() : nullptr;
}
} // namespace

FRESULT f_open(FIL* fp, const TCHAR* path, BYTE mode)
{
    auto* card = Card();
    if(fp == nullptr || path == nullptr)
        return FR_INVALID_OBJECT;
    if(card == nullptr)
        return FR_NOT_READY;

    const bool create   = mode & (FA_CREATE_ALWAYS | FA_OPEN_ALWAYS);
    const bool truncate = mode & FA_CREATE_ALWAYS;

    uint32_t  size   = 0;
    const int handle = card->OpenFile(path, create, truncate, &size);
    fp->card_handle  = handle;
    if(handle == 0)
        return FR_NO_FILE;

    fp->obj.objsize = size;
    fp->fptr        = 0;
    fp->flag        = mode;
    return FR_OK;
}

FRESULT f_close(FIL* fp)
{
    if(CardFor(fp) == nullptr)
        return FR_INVALID_OBJECT;
    fp->card_handle = 0;
    return FR_OK;
}

FRESULT f_read(FIL* fp, void* buff, UINT btr, UINT* br)
{
    if(br != nullptr)
        *br = 0;
    auto* card = CardFor(fp);
    if(card == nullptr)
        return FR_INVALID_OBJECT;

    // Like FatFs, bounded by the size this FIL saw at open/last write.
    const FSIZE_t remain = fp->obj.objsize > fp->fptr ? fp->obj.objsize - fp->fptr : 0;
    const UINT    count  = btr < remain ? btr : static_cast<UINT>(remain);
    const UINT    got    = card->Read(fp->card_handle, fp->fptr, buff, count);
    if(got < count) // file shrank under this FIL
        std::memset(static_cast<uint8_t*>(buff) + got, 0, count - got);

    fp->fptr += count;
    if(br != nullptr)
        *br = count;
    return FR_OK;
}

FRESULT f_write(FIL* fp, const void* buff, UINT btw, UINT* bw)
{
    // The firmwares pass bw == nullptr in places; FatFs would fault, they
    // never look at the count there.
    if(bw != nullptr)
        *bw = 0;
    auto* card = CardFor(fp);
    if(card == nullptr)
        return FR_INVALID_OBJECT;
    if(!(fp->flag & FA_WRITE))
        return FR_DENIED;

    card->Write(fp->card_handle, fp->fptr, buff, btw);
    fp->fptr += btw;
    if(fp->fptr > fp->obj.objsize)
        fp->obj.objsize = fp->fptr;
    if(bw != nullptr)
        *bw = btw;
    return FR_OK;
}

FRESULT f_lseek(FIL* fp, FSIZE_t ofs)
{
    auto* card = CardFor(fp);
    if(card == nullptr)
        return FR_INVALID_OBJECT;

    if(ofs > fp->obj.objsize)
    {
        if(fp->flag & FA_WRITE) // FatFs expands the file
        {
            card->Resize(fp->card_handle, ofs);
            fp->obj.objsize = ofs;
        }
        else
            ofs = fp->obj.objsize;
    }
    fp->fptr = ofs;
    return FR_OK;
}

FRESULT f_truncate(FIL* fp)
{
    auto* card = CardFor(fp);
    if(card == nullptr)
        return FR_INVALID_OBJECT;
    if(!(fp->flag & FA_WRITE))
        return FR_DENIED;
    card->Resize(fp->card_handle, fp->fptr);
    fp->obj.objsize = fp->fptr;
    return FR_OK;
}

FRESULT f_sync(FIL* fp)
{
    return CardFor(fp) != nullptr ? FR_OK : FR_INVALID_OBJECT;
}

FRESULT f_unlink(const TCHAR* path)
{
    auto* card = Card();
    if(card == nullptr)
        return FR_NOT_READY;
    return card->Unlink(path) ? FR_OK : FR_NO_FILE;
}

FRESULT f_rename(const TCHAR* path_old, const TCHAR* path_new)
{
    auto* card = Card();
    if(card == nullptr)
        return FR_NOT_READY;
    switch(card->Rename(path_old, path_new))
    {
        case emu::VirtualCard::RenameResult::Ok: return FR_OK;
        case emu::VirtualCard::RenameResult::Exists: return FR_EXIST;
        case emu::VirtualCard::RenameResult::NoFile: break;
    }
    return FR_NO_FILE;
}

FRESULT f_stat(const TCHAR* path, FILINFO* fno)
{
    auto* card = Card();
    if(card == nullptr)
        return FR_NOT_READY;
    uint32_t   size = 0;
    const bool file = card->Exists(path, &size);
    if(!file && !card->IsDir(path))
        return FR_NO_FILE;
    if(fno != nullptr)
    {
        std::memset(fno, 0, sizeof(*fno));
        fno->fsize   = size;
        fno->fattrib = file ? AM_ARC : AM_DIR;
        std::strncpy(fno->fname, path, sizeof(fno->fname) - 1);
    }
    return FR_OK;
}

FRESULT f_opendir(DIR* dp, const TCHAR* path)
{
    auto* card = Card();
    if(dp == nullptr || path == nullptr)
        return FR_INVALID_OBJECT;
    if(card == nullptr)
        return FR_NOT_READY;
    if(!card->IsDir(path))
        return FR_NO_PATH;
    std::memset(dp, 0, sizeof(*dp));
    std::strncpy(dp->path, path, sizeof(dp->path) - 1);
    dp->open = 1;
    return FR_OK;
}

/** The next entry, or fname[0] == 0 at the end. Entries come in name order;
 *  on FAT they come in the order they were written to the card. */
FRESULT f_readdir(DIR* dp, FILINFO* fno)
{
    auto* card = Card();
    if(dp == nullptr || !dp->open || fno == nullptr)
        return FR_INVALID_OBJECT;
    if(card == nullptr)
        return FR_NOT_READY;

    emu::VirtualCard::DirEntry entry;
    std::memset(fno, 0, sizeof(*fno));
    if(card->NextEntry(dp->path, dp->last, entry))
    {
        fno->fsize   = entry.size;
        fno->fattrib = entry.is_dir ? AM_DIR : AM_ARC;
        std::strncpy(fno->fname, entry.name, sizeof(fno->fname) - 1);
        std::strncpy(dp->last, entry.key, sizeof(dp->last) - 1);
    }
    return FR_OK;
}

FRESULT f_closedir(DIR* dp)
{
    if(dp == nullptr || !dp->open)
        return FR_INVALID_OBJECT;
    dp->open = 0;
    return FR_OK;
}
