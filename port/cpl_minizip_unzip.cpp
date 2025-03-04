/* Modified version by Even Rouault. :
     - Addition of cpl_unzGetCurrentFileZStreamPos
     - Decoration of symbol names unz* -> cpl_unz*
     - Undef EXPORT so that we are sure the symbols are not exported
     - Remove old C style function prototypes
     - Add support for ZIP64
     - Recode filename to UTF-8 if GP 11 is unset
     - Use Info-ZIP Unicode Path Extra Field (0x7075) to get UTF-8 filenames
     - ZIP64: accept number_disk == 0 in unzlocal_SearchCentralDir64()

 * Copyright (c) 2008-2014, Even Rouault <even dot rouault at spatialys.com>

   Original licence available in port/LICENCE_minizip
*/

/* unzip.c -- IO for uncompress .zip files using zlib
   Version 1.01e, February 12th, 2005

   Copyright (C) 1998-2005 Gilles Vollant

   Read unzip.h for more info
*/

/* Decryption code comes from crypt.c by Info-ZIP but has been greatly reduced
in terms of compatibility with older software. The following is from the
original crypt.c. Code woven in by Terry Thorsen 1/2003.
*/
/*
  Copyright (c) 1990-2000 Info-ZIP.  All rights reserved.

  See the accompanying file LICENSE, version 2000-Apr-09 or later
  (the contents of which are also included in zip.h) for terms of use.
  If, for some reason, all these files are missing, the Info-ZIP license
  also may be found at:  ftp://ftp.info-zip.org/pub/infozip/license.html
*/
/*
  crypt.c (full version) by Info-ZIP.      Last revised:  [see crypt.h]

  The encryption/decryption parts of this source code (as opposed to the
  non-echoing password parts) were originally written in Europe.  The
  whole source package can be freely distributed, including from the USA.
  (Prior to January 2000, re-export from the US was a violation of US law.)
 */

/*
  This encryption code is a direct transcription of the algorithm from
  Roger Schlafly, described by Phil Katz in the file appnote.txt.  This
  file (appnote.txt) is distributed with the PKZIP program (even in the
  version without encryption capabilities).
 */

#include "cpl_port.h"
#include "cpl_minizip_unzip.h"

#include <cstddef>
#include <cstdlib>
#include <cstring>

#include "cpl_conv.h"
#include "cpl_string.h"

#ifdef NO_ERRNO_H
extern int errno;
#else
#include <errno.h>
#endif

#ifndef CASESENSITIVITYDEFAULT_NO
#if !defined(unix) && !defined(CASESENSITIVITYDEFAULT_YES)
#define CASESENSITIVITYDEFAULT_NO
#endif
#endif

#ifndef UNZ_BUFSIZE
#define UNZ_BUFSIZE (16384)
#endif

#ifndef UNZ_MAXFILENAMEINZIP
#define UNZ_MAXFILENAMEINZIP (256)
#endif

#ifndef ALLOC
#define ALLOC(size) (malloc(size))
#endif
#ifndef TRYFREE
#define TRYFREE(p)                                                             \
    {                                                                          \
        if (p)                                                                 \
            free(p);                                                           \
    }
#endif

#define SIZECENTRALDIRITEM (0x2e)
#define SIZEZIPLOCALHEADER (0x1e)

const char unz_copyright[] = " unzip 1.01 Copyright 1998-2004 Gilles Vollant - "
                             "http://www.winimage.com/zLibDll";

/* unz_file_info_internal contain internal info about a file in zipfile */
typedef struct unz_file_info_internal_s
{
    uLong64 offset_curfile; /* relative offset of local header 4 bytes */
} unz_file_info_internal;

/* file_in_zip_read_info_s contain internal information about a file in zipfile,
    when reading and decompress it */
typedef struct
{
    char *read_buffer; /* internal buffer for compressed data */
    z_stream stream;   /* zLib stream structure for inflate */

    uLong64 pos_in_zipfile;   /* position in byte on the zipfile, for fseek */
    uLong stream_initialised; /* flag set if stream structure is initialized */

    uLong64 offset_local_extrafield; /* offset of the local extra field */
    uInt size_local_extrafield;      /* size of the local extra field */
    uLong64
        pos_local_extrafield; /* position in the local extra field in read */

    uLong crc32;      /* crc32 of all data uncompressed */
    uLong crc32_wait; /* crc32 we must obtain after decompress all */
    uLong64 rest_read_compressed; /* number of byte to be decompressed */
    uLong64
        rest_read_uncompressed; /*number of byte to be obtained after decomp */
    zlib_filefunc_def z_filefunc;
    voidpf filestream;               /* IO structure of the zipfile */
    uLong compression_method;        /* compression method (0==store) */
    uLong64 byte_before_the_zipfile; /* byte before the zipfile, (>0 for sfx)*/
    int raw;
} file_in_zip_read_info_s;

/* unz_s contain internal information about the zipfile
 */
typedef struct
{
    zlib_filefunc_def z_filefunc;
    voidpf filestream;               /* IO structure of the zipfile */
    unz_global_info gi;              /* public global information */
    uLong64 byte_before_the_zipfile; /* byte before the zipfile, (>0 for sfx)*/
    uLong64 num_file;           /* number of the current file in the zipfile*/
    uLong64 pos_in_central_dir; /* pos of the current file in the central dir*/
    uLong64 current_file_ok; /* flag about the usability of the current file*/
    uLong64 central_pos;     /* position of the beginning of the central dir*/

    uLong64 size_central_dir;   /* size of the central directory  */
    uLong64 offset_central_dir; /* offset of start of central directory with
                                 respect to the starting disk number */

    unz_file_info cur_file_info; /* public info about the current file in zip*/
    unz_file_info_internal cur_file_info_internal; /* private info about it*/
    file_in_zip_read_info_s *pfile_in_zip_read; /* structure about the current
                                        file if we are decompressing it */
    int encrypted;

    int isZip64;

#ifndef NOUNCRYPT
    unsigned long keys[3]; /* keys defining the pseudo-random sequence */
    const unsigned long *pcrc_32_tab;
#endif
} unz_s;

#ifndef NOUNCRYPT
#include "crypt.h"
#endif

/* ===========================================================================
     Read a byte from a gz_stream; update next_in and avail_in. Return EOF
   for end of file.
   IN assertion: the stream s has been successfully opened for reading.
*/

static int unzlocal_getByte(const zlib_filefunc_def *pzlib_filefunc_def,
                            voidpf filestream, int *pi)
{
    unsigned char c = 0;
    const int err =
        static_cast<int>(ZREAD(*pzlib_filefunc_def, filestream, &c, 1));
    if (err == 1)
    {
        *pi = static_cast<int>(c);
        return UNZ_OK;
    }
    else
    {
        if (ZERROR(*pzlib_filefunc_def, filestream))
            return UNZ_ERRNO;
        else
            return UNZ_EOF;
    }
}

/* ===========================================================================
   Reads a long in LSB order from the given gz_stream. Sets
*/
static int unzlocal_getShort(const zlib_filefunc_def *pzlib_filefunc_def,
                             voidpf filestream, uLong *pX)
{
    int i = 0;
    int err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    uLong x = static_cast<uLong>(i);

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong>(i) << 8;

    if (err == UNZ_OK)
        *pX = x;
    else
        *pX = 0;
    return err;
}

static int unzlocal_getLong(const zlib_filefunc_def *pzlib_filefunc_def,
                            voidpf filestream, uLong *pX)
{
    int i = 0;
    int err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    uLong x = static_cast<uLong>(i);

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong>(i) << 8;

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong>(i) << 16;

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong>(i) << 24;

    if (err == UNZ_OK)
        *pX = x;
    else
        *pX = 0;
    return err;
}

static int unzlocal_getLong64(const zlib_filefunc_def *pzlib_filefunc_def,
                              voidpf filestream, uLong64 *pX)
{
    int i = 0;
    int err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    uLong64 x = static_cast<uLong64>(i);

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong64>(i) << 8;

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong64>(i) << 16;

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong64>(i) << 24;

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong64>(i) << 32;

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong64>(i) << 40;

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong64>(i) << 48;

    if (err == UNZ_OK)
        err = unzlocal_getByte(pzlib_filefunc_def, filestream, &i);
    x += static_cast<uLong64>(i) << 56;

    if (err == UNZ_OK)
        *pX = x;
    else
        *pX = 0;
    return err;
}

/* My own strcmpi / strcasecmp */
static int strcmpcasenosensitive_internal(const char *fileName1,
                                          const char *fileName2)
{
    for (;;)
    {
        char c1 = *(fileName1++);
        char c2 = *(fileName2++);
        if ((c1 >= 'a') && (c1 <= 'z'))
            c1 -= 0x20;
        if ((c2 >= 'a') && (c2 <= 'z'))
            c2 -= 0x20;
        if (c1 == '\0')
            return ((c2 == '\0') ? 0 : -1);
        if (c2 == '\0')
            return 1;
        if (c1 < c2)
            return -1;
        if (c1 > c2)
            return 1;
    }
}

#ifdef CASESENSITIVITYDEFAULT_NO
#define CASESENSITIVITYDEFAULTVALUE 2
#else
#define CASESENSITIVITYDEFAULTVALUE 1
#endif

#ifndef STRCMPCASENOSENTIVEFUNCTION
#define STRCMPCASENOSENTIVEFUNCTION strcmpcasenosensitive_internal
#endif

/*
   Compare two filename (fileName1,fileName2).
   If iCaseSenisivity = 1, comparison is case sensitivity (like strcmp)
   If iCaseSenisivity = 2, comparison is not case sensitivity (like strcmpi
                                                               or strcasecmp)
   If iCaseSenisivity = 0, case sensitivity is default of your operating system
        (like 1 on Unix, 2 on Windows)

*/
extern int ZEXPORT cpl_unzStringFileNameCompare(const char *fileName1,
                                                const char *fileName2,
                                                int iCaseSensitivity)

{
    if (iCaseSensitivity == 0)
        iCaseSensitivity = CASESENSITIVITYDEFAULTVALUE;

    if (iCaseSensitivity == 1)
        return strcmp(fileName1, fileName2);

    return STRCMPCASENOSENTIVEFUNCTION(fileName1, fileName2);
}

#ifndef BUFREADCOMMENT
#define BUFREADCOMMENT (0x400)
#endif

/*
  Locate the Central directory of a zipfile (at the end, just before
    the global comment)
*/
static uLong64
unzlocal_SearchCentralDir(const zlib_filefunc_def *pzlib_filefunc_def,
                          voidpf filestream)
{
    if (ZSEEK(*pzlib_filefunc_def, filestream, 0, ZLIB_FILEFUNC_SEEK_END) != 0)
        return 0;

    unsigned char *buf =
        static_cast<unsigned char *>(ALLOC(BUFREADCOMMENT + 4));
    if (buf == nullptr)
        return 0;

    const uLong64 uSizeFile = ZTELL(*pzlib_filefunc_def, filestream);

    uLong64 uMaxBack = 0xffff; /* maximum size of global comment */
    if (uMaxBack > uSizeFile)
        uMaxBack = uSizeFile;

    uLong64 uPosFound = 0;
    uLong64 uBackRead = 4;
    while (uBackRead < uMaxBack)
    {
        if (uBackRead + BUFREADCOMMENT > uMaxBack)
            uBackRead = uMaxBack;
        else
            uBackRead += BUFREADCOMMENT;
        const uLong64 uReadPos = uSizeFile - uBackRead;

        const uLong uReadSize = ((BUFREADCOMMENT + 4) < (uSizeFile - uReadPos))
                                    ? (BUFREADCOMMENT + 4)
                                    : static_cast<uLong>(uSizeFile - uReadPos);
        if (ZSEEK(*pzlib_filefunc_def, filestream, uReadPos,
                  ZLIB_FILEFUNC_SEEK_SET) != 0)
            break;

        if (ZREAD(*pzlib_filefunc_def, filestream, buf, uReadSize) != uReadSize)
            break;

        // TODO(schwehr): Fix where the decrement is in this for loop.
        for (int i = static_cast<int>(uReadSize) - 3; (i--) > 0;)
            if (((*(buf + i)) == 0x50) && ((*(buf + i + 1)) == 0x4b) &&
                ((*(buf + i + 2)) == 0x05) && ((*(buf + i + 3)) == 0x06))
            {
                uPosFound = uReadPos + i;
                break;
            }

        if (uPosFound != 0)
            break;
    }
    TRYFREE(buf);
    return uPosFound;
}

/*
  Locate the Central directory 64 of a zipfile (at the end, just before
    the global comment)
*/
static uLong64
unzlocal_SearchCentralDir64(const zlib_filefunc_def *pzlib_filefunc_def,
                            voidpf filestream)
{
    unsigned char *buf;
    uLong64 uSizeFile;
    uLong64 uBackRead;
    uLong64 uMaxBack = 0xffff; /* maximum size of global comment */
    uLong64 uPosFound = 0;
    uLong uL;

    if (ZSEEK(*pzlib_filefunc_def, filestream, 0, ZLIB_FILEFUNC_SEEK_END) != 0)
        return 0;

    uSizeFile = ZTELL(*pzlib_filefunc_def, filestream);

    if (uMaxBack > uSizeFile)
        uMaxBack = uSizeFile;

    buf = static_cast<unsigned char *>(ALLOC(BUFREADCOMMENT + 4));
    if (buf == nullptr)
        return 0;

    uBackRead = 4;
    while (uBackRead < uMaxBack)
    {
        uLong uReadSize;
        uLong64 uReadPos;
        if (uBackRead + BUFREADCOMMENT > uMaxBack)
            uBackRead = uMaxBack;
        else
            uBackRead += BUFREADCOMMENT;
        uReadPos = uSizeFile - uBackRead;

        uReadSize = ((BUFREADCOMMENT + 4) < (uSizeFile - uReadPos))
                        ? (BUFREADCOMMENT + 4)
                        : static_cast<uLong>(uSizeFile - uReadPos);
        if (ZSEEK(*pzlib_filefunc_def, filestream, uReadPos,
                  ZLIB_FILEFUNC_SEEK_SET) != 0)
            break;

        if (ZREAD(*pzlib_filefunc_def, filestream, buf, uReadSize) != uReadSize)
            break;

        for (int i = static_cast<int>(uReadSize) - 3; (i--) > 0;)
            if (((*(buf + i)) == 0x50) && ((*(buf + i + 1)) == 0x4b) &&
                ((*(buf + i + 2)) == 0x06) && ((*(buf + i + 3)) == 0x07))
            {
                uPosFound = uReadPos + i;
                break;
            }

        if (uPosFound != 0)
            break;
    }
    TRYFREE(buf);
    if (uPosFound == 0)
        return 0;

    /* Zip64 end of central directory locator */
    if (ZSEEK(*pzlib_filefunc_def, filestream, uPosFound,
              ZLIB_FILEFUNC_SEEK_SET) != 0)
        return 0;

    /* the signature, already checked */
    if (unzlocal_getLong(pzlib_filefunc_def, filestream, &uL) != UNZ_OK)
        return 0;

    /* number of the disk with the start of the zip64 end of  central directory
     */
    if (unzlocal_getLong(pzlib_filefunc_def, filestream, &uL) != UNZ_OK)
        return 0;
    if (uL != 0)
        return 0;

    /* relative offset of the zip64 end of central directory record */
    uLong64 relativeOffset;
    if (unzlocal_getLong64(pzlib_filefunc_def, filestream, &relativeOffset) !=
        UNZ_OK)
        return 0;

    /* total number of disks */
    if (unzlocal_getLong(pzlib_filefunc_def, filestream, &uL) != UNZ_OK)
        return 0;
    /* Some .zip declare 0 disks, such as in
     * http://trac.osgeo.org/gdal/ticket/5615 */
    if (uL != 1 && uL != 0)
        return 0;

    /* Goto end of central directory record */
    if (ZSEEK(*pzlib_filefunc_def, filestream, relativeOffset,
              ZLIB_FILEFUNC_SEEK_SET) != 0)
        return 0;

    /* the signature */
    if (unzlocal_getLong(pzlib_filefunc_def, filestream, &uL) != UNZ_OK)
        return 0;

    if (uL != 0x06064b50)
        return 0;

    return relativeOffset;
}

/*
  Open a Zip file. path contain the full pathname (by example,
     on a Windows NT computer "c:\\test\\zlib114.zip" or on an Unix computer
     "zlib/zlib114.zip".
     If the zipfile cannot be opened (file doesn't exist or in not valid), the
       return value is NULL.
     Else, the return value is a unzFile Handle, usable with other function
       of this unzip package.
*/
extern unzFile ZEXPORT cpl_unzOpen2(const char *path,
                                    zlib_filefunc_def *pzlib_filefunc_def)
{
    unz_s us;
    unz_s *s;
    uLong64 central_pos;
    uLong uL;

    uLong number_disk;         /* number of the current dist, used for
                                  spanning ZIP, unsupported, always 0*/
    uLong number_disk_with_CD; /* number the disk with central dir, used
                                  for spanning ZIP, unsupported, always 0*/
    uLong64 number_entry_CD;   /* total number of entries in
                                the central dir
                                (same than number_entry on nospan) */

    int err = UNZ_OK;

    memset(&us, 0, sizeof(us));

    // Must be a trick to ensure that unz_copyright remains in the binary!
    // cppcheck-suppress knownConditionTrueFalse
    if (unz_copyright[0] != ' ')
        return nullptr;

    if (pzlib_filefunc_def == nullptr)
        cpl_fill_fopen_filefunc(&us.z_filefunc);
    else
        us.z_filefunc = *pzlib_filefunc_def;

    us.filestream = (*(us.z_filefunc.zopen_file))(
        us.z_filefunc.opaque, path,
        ZLIB_FILEFUNC_MODE_READ | ZLIB_FILEFUNC_MODE_EXISTING);
    if (us.filestream == nullptr)
        return nullptr;

    central_pos = unzlocal_SearchCentralDir64(&us.z_filefunc, us.filestream);
    if (central_pos)
    {
        uLong uS;
        uLong64 uL64;

        us.isZip64 = 1;

        if (ZSEEK(us.z_filefunc, us.filestream, central_pos,
                  ZLIB_FILEFUNC_SEEK_SET) != 0)
            err = UNZ_ERRNO;

        /* the signature, already checked */
        if (unzlocal_getLong(&us.z_filefunc, us.filestream, &uL) != UNZ_OK)
            err = UNZ_ERRNO;

        /* size of zip64 end of central directory record */
        if (unzlocal_getLong64(&us.z_filefunc, us.filestream, &uL64) != UNZ_OK)
            err = UNZ_ERRNO;

        /* version made by */
        if (unzlocal_getShort(&us.z_filefunc, us.filestream, &uS) != UNZ_OK)
            err = UNZ_ERRNO;

        /* version needed to extract */
        if (unzlocal_getShort(&us.z_filefunc, us.filestream, &uS) != UNZ_OK)
            err = UNZ_ERRNO;

        /* number of this disk */
        if (unzlocal_getLong(&us.z_filefunc, us.filestream, &number_disk) !=
            UNZ_OK)
            err = UNZ_ERRNO;

        /* number of the disk with the start of the central directory */
        if (unzlocal_getLong(&us.z_filefunc, us.filestream,
                             &number_disk_with_CD) != UNZ_OK)
            err = UNZ_ERRNO;

        /* total number of entries in the central directory on this disk */
        if (unzlocal_getLong64(&us.z_filefunc, us.filestream,
                               &us.gi.number_entry) != UNZ_OK)
            err = UNZ_ERRNO;

        /* total number of entries in the central directory */
        if (unzlocal_getLong64(&us.z_filefunc, us.filestream,
                               &number_entry_CD) != UNZ_OK)
            err = UNZ_ERRNO;

        if ((number_entry_CD != us.gi.number_entry) ||
            (number_disk_with_CD != 0) || (number_disk != 0))
            err = UNZ_BADZIPFILE;

        /* size of the central directory */
        if (unzlocal_getLong64(&us.z_filefunc, us.filestream,
                               &us.size_central_dir) != UNZ_OK)
            err = UNZ_ERRNO;

        /* offset of start of central directory with respect to the
          starting disk number */
        if (unzlocal_getLong64(&us.z_filefunc, us.filestream,
                               &us.offset_central_dir) != UNZ_OK)
            err = UNZ_ERRNO;

        us.gi.size_comment = 0;
    }
    else
    {
        central_pos = unzlocal_SearchCentralDir(&us.z_filefunc, us.filestream);
        if (central_pos == 0)
            err = UNZ_ERRNO;

        us.isZip64 = 0;

        if (ZSEEK(us.z_filefunc, us.filestream, central_pos,
                  ZLIB_FILEFUNC_SEEK_SET) != 0)
            err = UNZ_ERRNO;

        /* the signature, already checked */
        if (unzlocal_getLong(&us.z_filefunc, us.filestream, &uL) != UNZ_OK)
            err = UNZ_ERRNO;

        /* number of this disk */
        if (unzlocal_getShort(&us.z_filefunc, us.filestream, &number_disk) !=
            UNZ_OK)
            err = UNZ_ERRNO;

        /* number of the disk with the start of the central directory */
        if (unzlocal_getShort(&us.z_filefunc, us.filestream,
                              &number_disk_with_CD) != UNZ_OK)
            err = UNZ_ERRNO;

        /* total number of entries in the central dir on this disk */
        if (unzlocal_getShort(&us.z_filefunc, us.filestream, &uL) != UNZ_OK)
            err = UNZ_ERRNO;
        us.gi.number_entry = uL;

        /* total number of entries in the central dir */
        if (unzlocal_getShort(&us.z_filefunc, us.filestream, &uL) != UNZ_OK)
            err = UNZ_ERRNO;
        number_entry_CD = uL;

        if ((number_entry_CD != us.gi.number_entry) ||
            (number_disk_with_CD != 0) || (number_disk != 0))
            err = UNZ_BADZIPFILE;

        /* size of the central directory */
        if (unzlocal_getLong(&us.z_filefunc, us.filestream, &uL) != UNZ_OK)
            err = UNZ_ERRNO;
        us.size_central_dir = uL;

        /* offset of start of central directory with respect to the
            starting disk number */
        if (unzlocal_getLong(&us.z_filefunc, us.filestream, &uL) != UNZ_OK)
            err = UNZ_ERRNO;
        us.offset_central_dir = uL;

        /* zipfile comment length */
        if (unzlocal_getShort(&us.z_filefunc, us.filestream,
                              &us.gi.size_comment) != UNZ_OK)
            err = UNZ_ERRNO;
    }

    if ((central_pos < us.offset_central_dir + us.size_central_dir) &&
        (err == UNZ_OK))
        err = UNZ_BADZIPFILE;

    if (err != UNZ_OK)
    {
        ZCLOSE(us.z_filefunc, us.filestream);
        return nullptr;
    }

    us.byte_before_the_zipfile =
        central_pos - (us.offset_central_dir + us.size_central_dir);
    us.central_pos = central_pos;
    us.pfile_in_zip_read = nullptr;
    us.encrypted = 0;
    us.num_file = 0;
    us.pos_in_central_dir = 0;
    us.current_file_ok = 0;

    s = static_cast<unz_s *>(ALLOC(sizeof(unz_s)));
    *s = us;
    cpl_unzGoToFirstFile(reinterpret_cast<unzFile>(s));
    return reinterpret_cast<unzFile>(s);
}

extern unzFile ZEXPORT cpl_unzOpen(const char *path)
{
    return cpl_unzOpen2(path, nullptr);
}

/*
  Close a ZipFile opened with unzipOpen.
  If there is files inside the .Zip opened with unzipOpenCurrentFile (see
  later), these files MUST be closed with unzipCloseCurrentFile before call
  unzipClose. return UNZ_OK if there is no problem. */
extern int ZEXPORT cpl_unzClose(unzFile file)
{
    unz_s *s;
    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);

    if (s->pfile_in_zip_read != nullptr)
        cpl_unzCloseCurrentFile(file);

    ZCLOSE(s->z_filefunc, s->filestream);
    TRYFREE(s);
    return UNZ_OK;
}

/*
  Write info about the ZipFile in the *pglobal_info structure.
  No preparation of the structure is needed
  return UNZ_OK if there is no problem. */
extern int ZEXPORT cpl_unzGetGlobalInfo(unzFile file,
                                        unz_global_info *pglobal_info)
{
    unz_s *s;
    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    *pglobal_info = s->gi;
    return UNZ_OK;
}

/*
   Translate date/time from Dos format to tm_unz (readable more easily).
*/
static void unzlocal_DosDateToTmuDate(uLong64 ulDosDate, tm_unz *ptm)
{
    uLong64 uDate;
    uDate = static_cast<uLong64>(ulDosDate >> 16);
    ptm->tm_mday = static_cast<uInt>(uDate & 0x1f);
    ptm->tm_mon = static_cast<uInt>(((uDate)&0x1E0) / 0x20);
    if (ptm->tm_mon)
        ptm->tm_mon--;
    ptm->tm_year = static_cast<uInt>(((uDate & 0x0FE00) / 0x0200) + 1980);

    ptm->tm_hour = static_cast<uInt>((ulDosDate & 0xF800) / 0x800);
    ptm->tm_min = static_cast<uInt>((ulDosDate & 0x7E0) / 0x20);
    ptm->tm_sec = static_cast<uInt>(2 * (ulDosDate & 0x1f));
}

/*
  Get Info about the current file in the zipfile, with internal only info
*/
static int unzlocal_GetCurrentFileInfoInternal(
    unzFile file, unz_file_info *pfile_info,
    unz_file_info_internal *pfile_info_internal, char *szFileName,
    uLong fileNameBufferSize, void * /* extraField */,
    uLong /* extraFieldBufferSize */, char * /* szComment */,
    uLong /* commentBufferSize */)
{
    unz_s *s;
    unz_file_info file_info;
    unz_file_info_internal file_info_internal;
    int err = UNZ_OK;
    uLong uMagic;
    long lSeek = 0;
    uLong uL;
    bool bHasUTF8Filename = false;

    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    if (ZSEEK(s->z_filefunc, s->filestream,
              s->pos_in_central_dir + s->byte_before_the_zipfile,
              ZLIB_FILEFUNC_SEEK_SET) != 0)
        err = UNZ_ERRNO;

    /* we check the magic */
    if (err == UNZ_OK)
    {
        if (unzlocal_getLong(&s->z_filefunc, s->filestream, &uMagic) != UNZ_OK)
            err = UNZ_ERRNO;
        else if (uMagic != 0x02014b50)
            err = UNZ_BADZIPFILE;
    }

    if (unzlocal_getShort(&s->z_filefunc, s->filestream, &file_info.version) !=
        UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream,
                          &file_info.version_needed) != UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream, &file_info.flag) !=
        UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream,
                          &file_info.compression_method) != UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getLong(&s->z_filefunc, s->filestream, &file_info.dosDate) !=
        UNZ_OK)
        err = UNZ_ERRNO;

    unzlocal_DosDateToTmuDate(file_info.dosDate, &file_info.tmu_date);

    if (unzlocal_getLong(&s->z_filefunc, s->filestream, &file_info.crc) !=
        UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getLong(&s->z_filefunc, s->filestream, &uL) != UNZ_OK)
        err = UNZ_ERRNO;
    file_info.compressed_size = uL;

    if (unzlocal_getLong(&s->z_filefunc, s->filestream, &uL) != UNZ_OK)
        err = UNZ_ERRNO;
    file_info.uncompressed_size = uL;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream,
                          &file_info.size_filename) != UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream,
                          &file_info.size_file_extra) != UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream,
                          &file_info.size_file_comment) != UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream,
                          &file_info.disk_num_start) != UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream,
                          &file_info.internal_fa) != UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getLong(&s->z_filefunc, s->filestream,
                         &file_info.external_fa) != UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getLong(&s->z_filefunc, s->filestream, &uL) != UNZ_OK)
        err = UNZ_ERRNO;
    file_info_internal.offset_curfile = uL;

    lSeek += file_info.size_filename;
    if ((err == UNZ_OK) && (szFileName != nullptr))
    {
        uLong uSizeRead = 0;
        if (file_info.size_filename < fileNameBufferSize)
        {
            *(szFileName + file_info.size_filename) = '\0';
            uSizeRead = file_info.size_filename;
        }
        else
            uSizeRead = fileNameBufferSize;

        if ((file_info.size_filename > 0) && (fileNameBufferSize > 0))
        {
            if (ZREAD(s->z_filefunc, s->filestream, szFileName, uSizeRead) !=
                uSizeRead)
                err = UNZ_ERRNO;
        }
        lSeek -= uSizeRead;
    }

#if 0
    if ((err==UNZ_OK) && (extraField != nullptr))
    {
        uLong64 uSizeRead = 0;
        if (file_info.size_file_extra<extraFieldBufferSize)
            uSizeRead = file_info.size_file_extra;
        else
            uSizeRead = extraFieldBufferSize;

        if (lSeek!=0)
        {
            if (ZSEEK(s->z_filefunc, s->filestream,lSeek,ZLIB_FILEFUNC_SEEK_CUR)==0)
                lSeek=0;
            else
                err=UNZ_ERRNO;
        }

        if ((file_info.size_file_extra>0) && (extraFieldBufferSize>0))
            if (ZREAD(s->z_filefunc, s->filestream,extraField,uSizeRead)!=uSizeRead)
                err=UNZ_ERRNO;
        lSeek += file_info.size_file_extra - uSizeRead;
    }
    else
        lSeek+=file_info.size_file_extra;
#endif
    if ((err == UNZ_OK) && (file_info.size_file_extra != 0))
    {
        if (lSeek != 0)
        {
            if (ZSEEK(s->z_filefunc, s->filestream, lSeek,
                      ZLIB_FILEFUNC_SEEK_CUR) == 0)
            {
                lSeek = 0;
                CPL_IGNORE_RET_VAL(lSeek);
            }
            else
                err = UNZ_ERRNO;
        }

        uLong acc = 0;
        while (acc < file_info.size_file_extra)
        {
            uLong headerId;
            if (unzlocal_getShort(&s->z_filefunc, s->filestream, &headerId) !=
                UNZ_OK)
                err = UNZ_ERRNO;

            uLong dataSize;
            if (unzlocal_getShort(&s->z_filefunc, s->filestream, &dataSize) !=
                UNZ_OK)
                err = UNZ_ERRNO;

            /* ZIP64 extra fields */
            if (headerId == 0x0001)
            {
                uLong64 u64;
                if (file_info.uncompressed_size == 0xFFFFFFFF)
                {
                    if (unzlocal_getLong64(&s->z_filefunc, s->filestream,
                                           &u64) != UNZ_OK)
                        err = UNZ_ERRNO;
                    file_info.uncompressed_size = u64;
                }

                if (file_info.compressed_size == 0xFFFFFFFF)
                {
                    if (unzlocal_getLong64(&s->z_filefunc, s->filestream,
                                           &u64) != UNZ_OK)
                        err = UNZ_ERRNO;
                    file_info.compressed_size = u64;
                }

                /* Relative Header offset */
                if (file_info_internal.offset_curfile == 0xFFFFFFFF)
                {
                    if (unzlocal_getLong64(&s->z_filefunc, s->filestream,
                                           &u64) != UNZ_OK)
                        err = UNZ_ERRNO;
                    file_info_internal.offset_curfile = u64;
                }

                /* Disk Start Number */
                if (file_info.disk_num_start == 0xFFFF)
                {
                    uLong uLstart;
                    if (unzlocal_getLong(&s->z_filefunc, s->filestream,
                                         &uLstart) != UNZ_OK)
                        err = UNZ_ERRNO;
                    file_info.disk_num_start = uLstart;
                }
            }
            /* Info-ZIP Unicode Path Extra Field (0x7075) */
            else if (headerId == 0x7075 && dataSize > 5 &&
                     file_info.size_filename <= fileNameBufferSize &&
                     szFileName != nullptr)
            {
                int version = 0;
                if (unzlocal_getByte(&s->z_filefunc, s->filestream, &version) !=
                    UNZ_OK)
                    err = UNZ_ERRNO;
                if (version != 1)
                {
                    /* If version != 1, ignore that extra field */
                    if (ZSEEK(s->z_filefunc, s->filestream, dataSize - 1,
                              ZLIB_FILEFUNC_SEEK_CUR) != 0)
                        err = UNZ_ERRNO;
                }
                else
                {
                    uLong nameCRC32;
                    if (unzlocal_getLong(&s->z_filefunc, s->filestream,
                                         &nameCRC32) != UNZ_OK)
                        err = UNZ_ERRNO;

                    /* Check expected CRC for filename */
                    if (nameCRC32 ==
                        crc32(0, reinterpret_cast<const Bytef *>(szFileName),
                              static_cast<uInt>(file_info.size_filename)))
                    {
                        const uLong utf8Size = dataSize - 1 - 4;
                        uLong uSizeRead = 0;

                        bHasUTF8Filename = true;

                        if (utf8Size < fileNameBufferSize)
                        {
                            *(szFileName + utf8Size) = '\0';
                            uSizeRead = utf8Size;
                        }
                        else
                            uSizeRead = fileNameBufferSize;

                        if (ZREAD(s->z_filefunc, s->filestream, szFileName,
                                  uSizeRead) != uSizeRead)
                            err = UNZ_ERRNO;
                        else if (utf8Size > fileNameBufferSize)
                        {
                            if (ZSEEK(s->z_filefunc, s->filestream,
                                      utf8Size - fileNameBufferSize,
                                      ZLIB_FILEFUNC_SEEK_CUR) != 0)
                                err = UNZ_ERRNO;
                        }
                    }
                    else
                    {
                        /* ignore unicode name if CRC mismatch */
                        if (ZSEEK(s->z_filefunc, s->filestream,
                                  dataSize - 1 - 4,
                                  ZLIB_FILEFUNC_SEEK_CUR) != 0)
                            err = UNZ_ERRNO;
                    }
                }
            }
            else
            {
                if (ZSEEK(s->z_filefunc, s->filestream, dataSize,
                          ZLIB_FILEFUNC_SEEK_CUR) != 0)
                    err = UNZ_ERRNO;
            }

            acc += 2 + 2 + dataSize;
        }
    }

    if (!bHasUTF8Filename && szFileName != nullptr &&
        (file_info.flag & (1 << 11)) == 0 &&
        file_info.size_filename < fileNameBufferSize)
    {
        const char *pszSrcEncoding = CPLGetConfigOption("CPL_ZIP_ENCODING",
#if defined(_WIN32) && !defined(HAVE_ICONV)
                                                        "CP_OEMCP"
#else
                                                        "CP437"
#endif
        );
        char *pszRecoded = CPLRecode(szFileName, pszSrcEncoding, CPL_ENC_UTF8);
        if (pszRecoded != nullptr && strlen(pszRecoded) < fileNameBufferSize)
        {
            strcpy(szFileName, pszRecoded);
        }
        CPLFree(pszRecoded);
    }

#if 0
    if ((err==UNZ_OK) && (szComment != nullptr))
    {
        uLong64 uSizeRead = 0;
        if (file_info.size_file_comment<commentBufferSize)
        {
            *(szComment+file_info.size_file_comment)='\0';
            uSizeRead = file_info.size_file_comment;
        }
        else
            uSizeRead = commentBufferSize;

        if (lSeek!=0)
        {
            if (ZSEEK(s->z_filefunc, s->filestream,lSeek,ZLIB_FILEFUNC_SEEK_CUR)==0)
                lSeek=0;
            else
                err=UNZ_ERRNO;
        }

        if ((file_info.size_file_comment>0) && (commentBufferSize>0))
            if (ZREAD(s->z_filefunc, s->filestream,szComment,uSizeRead)!=uSizeRead)
                err=UNZ_ERRNO;
        lSeek+=file_info.size_file_comment - uSizeRead;
    }
    else
        lSeek+=file_info.size_file_comment;
#endif

    if ((err == UNZ_OK) && (pfile_info != nullptr))
        *pfile_info = file_info;

    if ((err == UNZ_OK) && (pfile_info_internal != nullptr))
        *pfile_info_internal = file_info_internal;

    return err;
}

/*
  Write info about the ZipFile in the *pglobal_info structure.
  No preparation of the structure is needed
  return UNZ_OK if there is no problem.
*/
extern int ZEXPORT cpl_unzGetCurrentFileInfo(
    unzFile file, unz_file_info *pfile_info, char *szFileName,
    uLong fileNameBufferSize, void *extraField, uLong extraFieldBufferSize,
    char *szComment, uLong commentBufferSize)
{
    return unzlocal_GetCurrentFileInfoInternal(
        file, pfile_info, nullptr, szFileName, fileNameBufferSize, extraField,
        extraFieldBufferSize, szComment, commentBufferSize);
}

/*
  Set the current file of the zipfile to the first file.
  return UNZ_OK if there is no problem
*/
extern int ZEXPORT cpl_unzGoToFirstFile(unzFile file)
{
    int err = UNZ_OK;
    unz_s *s;
    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    s->pos_in_central_dir = s->offset_central_dir;
    s->num_file = 0;
    err = unzlocal_GetCurrentFileInfoInternal(
        file, &s->cur_file_info, &s->cur_file_info_internal, nullptr, 0,
        nullptr, 0, nullptr, 0);
    s->current_file_ok = (err == UNZ_OK);
    return err;
}

/*
  Set the current file of the zipfile to the next file.
  return UNZ_OK if there is no problem
  return UNZ_END_OF_LIST_OF_FILE if the actual file was the latest.
*/
extern int ZEXPORT cpl_unzGoToNextFile(unzFile file)
{
    unz_s *s;

    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    if (!s->current_file_ok)
        return UNZ_END_OF_LIST_OF_FILE;
    if (s->gi.number_entry != 0xffff) /* 2^16 files overflow hack */
        if (s->num_file + 1 == s->gi.number_entry)
            return UNZ_END_OF_LIST_OF_FILE;

    s->pos_in_central_dir +=
        SIZECENTRALDIRITEM + s->cur_file_info.size_filename +
        s->cur_file_info.size_file_extra + s->cur_file_info.size_file_comment;
    s->num_file++;
    int err = unzlocal_GetCurrentFileInfoInternal(
        file, &s->cur_file_info, &s->cur_file_info_internal, nullptr, 0,
        nullptr, 0, nullptr, 0);
    s->current_file_ok = (err == UNZ_OK);
    return err;
}

/*
  Try locate the file szFileName in the zipfile.
  For the iCaseSensitivity signification, see unzipStringFileNameCompare

  return value :
  UNZ_OK if the file is found. It becomes the current file.
  UNZ_END_OF_LIST_OF_FILE if the file is not found
*/
extern int ZEXPORT cpl_unzLocateFile(unzFile file, const char *szFileName,
                                     int iCaseSensitivity)
{
    unz_s *s;

    /* We remember the 'current' position in the file so that we can jump
     * back there if we fail.
     */
    unz_file_info cur_file_infoSaved;
    unz_file_info_internal cur_file_info_internalSaved;
    uLong64 num_fileSaved;
    uLong64 pos_in_central_dirSaved;

    if (file == nullptr)
        return UNZ_PARAMERROR;

    if (strlen(szFileName) >= UNZ_MAXFILENAMEINZIP)
        return UNZ_PARAMERROR;

    s = reinterpret_cast<unz_s *>(file);
    if (!s->current_file_ok)
        return UNZ_END_OF_LIST_OF_FILE;

    /* Save the current state */
    num_fileSaved = s->num_file;
    pos_in_central_dirSaved = s->pos_in_central_dir;
    cur_file_infoSaved = s->cur_file_info;
    cur_file_info_internalSaved = s->cur_file_info_internal;

    int err = cpl_unzGoToFirstFile(file);

    while (err == UNZ_OK)
    {
        char szCurrentFileName[UNZ_MAXFILENAMEINZIP + 1];
        err = cpl_unzGetCurrentFileInfo(file, nullptr, szCurrentFileName,
                                        sizeof(szCurrentFileName) - 1, nullptr,
                                        0, nullptr, 0);
        if (err == UNZ_OK)
        {
            if (cpl_unzStringFileNameCompare(szCurrentFileName, szFileName,
                                             iCaseSensitivity) == 0)
                return UNZ_OK;
            err = cpl_unzGoToNextFile(file);
        }
    }

    /* We failed, so restore the state of the 'current file' to where we
     * were.
     */
    s->num_file = num_fileSaved;
    s->pos_in_central_dir = pos_in_central_dirSaved;
    s->cur_file_info = cur_file_infoSaved;
    s->cur_file_info_internal = cur_file_info_internalSaved;
    return err;
}

/*
///////////////////////////////////////////
// Contributed by Ryan Haksi (mailto://cryogen@infoserve.net)
// I need random access
//
// Further optimization could be realized by adding an ability
// to cache the directory in memory. The goal being a single
// comprehensive file read to put the file I need in a memory.
*/

/*
typedef struct unz_file_pos_s
{
    uLong64 pos_in_zip_directory;   // offset in file
    uLong64 num_of_file;            // # of file
} unz_file_pos;
*/

extern int ZEXPORT cpl_unzGetFilePos(unzFile file, unz_file_pos *file_pos)
{
    unz_s *s;

    if (file == nullptr || file_pos == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    if (!s->current_file_ok)
        return UNZ_END_OF_LIST_OF_FILE;

    file_pos->pos_in_zip_directory = s->pos_in_central_dir;
    file_pos->num_of_file = s->num_file;

    return UNZ_OK;
}

extern int ZEXPORT cpl_unzGoToFilePos(unzFile file, unz_file_pos *file_pos)
{
    unz_s *s;

    if (file == nullptr || file_pos == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);

    /* jump to the right spot */
    s->pos_in_central_dir = file_pos->pos_in_zip_directory;
    s->num_file = file_pos->num_of_file;

    /* set the current file */
    int err = unzlocal_GetCurrentFileInfoInternal(
        file, &s->cur_file_info, &s->cur_file_info_internal, nullptr, 0,
        nullptr, 0, nullptr, 0);
    /* return results */
    s->current_file_ok = (err == UNZ_OK);
    return err;
}

/*
// Unzip Helper Functions - should be here?
///////////////////////////////////////////
*/

/*
  Read the local header of the current zipfile
  Check the coherency of the local header and info in the end of central
        directory about this file
  store in *piSizeVar the size of extra info in local header
        (filename and size of extra field data)
*/
static int
unzlocal_CheckCurrentFileCoherencyHeader(unz_s *s, uInt *piSizeVar,
                                         uLong64 *poffset_local_extrafield,
                                         uInt *psize_local_extrafield)
{
    uLong uMagic, uData, uFlags;
    uLong size_filename;
    uLong size_extra_field;
    int err = UNZ_OK;

    *piSizeVar = 0;
    *poffset_local_extrafield = 0;
    *psize_local_extrafield = 0;

    if (ZSEEK(s->z_filefunc, s->filestream,
              s->cur_file_info_internal.offset_curfile +
                  s->byte_before_the_zipfile,
              ZLIB_FILEFUNC_SEEK_SET) != 0)
        return UNZ_ERRNO;

    if (err == UNZ_OK)
    {
        if (unzlocal_getLong(&s->z_filefunc, s->filestream, &uMagic) != UNZ_OK)
            err = UNZ_ERRNO;
        else if (uMagic != 0x04034b50)
            err = UNZ_BADZIPFILE;
    }

    if (unzlocal_getShort(&s->z_filefunc, s->filestream, &uData) != UNZ_OK)
        err = UNZ_ERRNO;
    /*
        else if ((err==UNZ_OK) && (uData!=s->cur_file_info.wVersion))
            err=UNZ_BADZIPFILE;
    */
    if (unzlocal_getShort(&s->z_filefunc, s->filestream, &uFlags) != UNZ_OK)
        err = UNZ_ERRNO;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream, &uData) != UNZ_OK)
        err = UNZ_ERRNO;
    else if ((err == UNZ_OK) && (uData != s->cur_file_info.compression_method))
        err = UNZ_BADZIPFILE;

    if ((err == UNZ_OK) && (s->cur_file_info.compression_method != 0) &&
        (s->cur_file_info.compression_method != Z_DEFLATED))
    {
#ifdef ENABLE_DEFLATE64
        if (s->cur_file_info.compression_method == 9)
        {
            // ok
        }
        else
#endif
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "A file in the ZIP archive uses a unsupported "
                     "compression method (%lu)",
                     s->cur_file_info.compression_method);
            err = UNZ_BADZIPFILE;
        }
    }

    if (unzlocal_getLong(&s->z_filefunc, s->filestream, &uData) !=
        UNZ_OK) /* date/time */
        err = UNZ_ERRNO;

    if (unzlocal_getLong(&s->z_filefunc, s->filestream, &uData) !=
        UNZ_OK) /* crc */
        err = UNZ_ERRNO;
    else if ((err == UNZ_OK) && (uData != s->cur_file_info.crc) &&
             ((uFlags & 8) == 0))
        err = UNZ_BADZIPFILE;

    if (unzlocal_getLong(&s->z_filefunc, s->filestream, &uData) !=
        UNZ_OK) /* size compr */
        err = UNZ_ERRNO;
    else if (uData != 0xFFFFFFFF && (err == UNZ_OK) &&
             (uData != s->cur_file_info.compressed_size) && ((uFlags & 8) == 0))
        err = UNZ_BADZIPFILE;

    if (unzlocal_getLong(&s->z_filefunc, s->filestream, &uData) !=
        UNZ_OK) /* size uncompr */
        err = UNZ_ERRNO;
    else if (uData != 0xFFFFFFFF && (err == UNZ_OK) &&
             (uData != s->cur_file_info.uncompressed_size) &&
             ((uFlags & 8) == 0))
        err = UNZ_BADZIPFILE;

    if (unzlocal_getShort(&s->z_filefunc, s->filestream, &size_filename) !=
        UNZ_OK)
        err = UNZ_ERRNO;
    else if ((err == UNZ_OK) &&
             (size_filename != s->cur_file_info.size_filename))
        err = UNZ_BADZIPFILE;

    *piSizeVar += static_cast<uInt>(size_filename);

    if (unzlocal_getShort(&s->z_filefunc, s->filestream, &size_extra_field) !=
        UNZ_OK)
        err = UNZ_ERRNO;
    *poffset_local_extrafield = s->cur_file_info_internal.offset_curfile +
                                SIZEZIPLOCALHEADER + size_filename;
    *psize_local_extrafield = static_cast<uInt>(size_extra_field);

    *piSizeVar += static_cast<uInt>(size_extra_field);

    return err;
}

/*
  Open for reading data the current file in the zipfile.
  If there is no error and the file is opened, the return value is UNZ_OK.
*/
extern int ZEXPORT cpl_unzOpenCurrentFile3(unzFile file, int *method,
                                           int *level, int raw,
                                           const char *password)
{
    int err = UNZ_OK;
    uInt iSizeVar;
    unz_s *s;
    file_in_zip_read_info_s *pfile_in_zip_read_info;
    uLong64 offset_local_extrafield; /* offset of the local extra field */
    uInt size_local_extrafield;      /* size of the local extra field */
#ifndef NOUNCRYPT
    char source[12];
#else
    if (password != nullptr)
        return UNZ_PARAMERROR;
#endif

    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    if (!s->current_file_ok)
        return UNZ_PARAMERROR;

    if (s->pfile_in_zip_read != nullptr)
        cpl_unzCloseCurrentFile(file);

    if (unzlocal_CheckCurrentFileCoherencyHeader(
            s, &iSizeVar, &offset_local_extrafield, &size_local_extrafield) !=
        UNZ_OK)
        return UNZ_BADZIPFILE;

    pfile_in_zip_read_info = static_cast<file_in_zip_read_info_s *>(
        ALLOC(sizeof(file_in_zip_read_info_s)));
    if (pfile_in_zip_read_info == nullptr)
        return UNZ_INTERNALERROR;

    pfile_in_zip_read_info->read_buffer =
        static_cast<char *>(ALLOC(UNZ_BUFSIZE));
    pfile_in_zip_read_info->offset_local_extrafield = offset_local_extrafield;
    pfile_in_zip_read_info->size_local_extrafield = size_local_extrafield;
    pfile_in_zip_read_info->pos_local_extrafield = 0;
    pfile_in_zip_read_info->raw = raw;

    if (pfile_in_zip_read_info->read_buffer == nullptr)
    {
        TRYFREE(pfile_in_zip_read_info);
        return UNZ_INTERNALERROR;
    }

    pfile_in_zip_read_info->stream_initialised = 0;

    if (method != nullptr)
        *method = static_cast<int>(s->cur_file_info.compression_method);

    if (level != nullptr)
    {
        *level = 6;
        switch (s->cur_file_info.flag & 0x06)
        {
            case 6:
                *level = 1;
                break;
            case 4:
                *level = 2;
                break;
            case 2:
                *level = 9;
                break;
        }
    }

    /*if ((s->cur_file_info.compression_method!=0) &&
        (s->cur_file_info.compression_method!=Z_DEFLATED))
        err=UNZ_BADZIPFILE;*/

    pfile_in_zip_read_info->crc32_wait = s->cur_file_info.crc;
    pfile_in_zip_read_info->crc32 = 0;
    pfile_in_zip_read_info->compression_method =
        s->cur_file_info.compression_method;
    pfile_in_zip_read_info->filestream = s->filestream;
    pfile_in_zip_read_info->z_filefunc = s->z_filefunc;
    pfile_in_zip_read_info->byte_before_the_zipfile =
        s->byte_before_the_zipfile;

    pfile_in_zip_read_info->stream.total_out = 0;

    if ((s->cur_file_info.compression_method == Z_DEFLATED) && (!raw))
    {
        pfile_in_zip_read_info->stream.zalloc = nullptr;
        pfile_in_zip_read_info->stream.zfree = nullptr;
        pfile_in_zip_read_info->stream.opaque = nullptr;
        pfile_in_zip_read_info->stream.next_in = nullptr;
        pfile_in_zip_read_info->stream.avail_in = 0;

        err = inflateInit2(&pfile_in_zip_read_info->stream, -MAX_WBITS);
        if (err == Z_OK)
            pfile_in_zip_read_info->stream_initialised = 1;
        else
        {
            TRYFREE(pfile_in_zip_read_info);
            return err;
        }
        /* windowBits is passed < 0 to tell that there is no zlib header.
         * Note that in this case inflate *requires* an extra "dummy" byte
         * after the compressed stream in order to complete decompression and
         * return Z_STREAM_END.
         * In unzip, i don't wait absolutely Z_STREAM_END because I known the
         * size of both compressed and uncompressed data
         */
    }
    pfile_in_zip_read_info->rest_read_compressed =
        s->cur_file_info.compressed_size;
    pfile_in_zip_read_info->rest_read_uncompressed =
        s->cur_file_info.uncompressed_size;

    pfile_in_zip_read_info->pos_in_zipfile =
        s->cur_file_info_internal.offset_curfile + SIZEZIPLOCALHEADER +
        iSizeVar;

    pfile_in_zip_read_info->stream.avail_in = 0;

    s->pfile_in_zip_read = pfile_in_zip_read_info;

#ifndef NOUNCRYPT
    if (password != nullptr)
    {
        s->pcrc_32_tab = get_crc_table();
        init_keys(password, s->keys, s->pcrc_32_tab);
        if (ZSEEK(s->z_filefunc, s->filestream,
                  s->pfile_in_zip_read->pos_in_zipfile +
                      s->pfile_in_zip_read->byte_before_the_zipfile,
                  SEEK_SET) != 0)
            return UNZ_INTERNALERROR;
        if (ZREAD(s->z_filefunc, s->filestream, source, 12) < 12)
            return UNZ_INTERNALERROR;

        for (int i = 0; i < 12; i++)
            zdecode(s->keys, s->pcrc_32_tab, source[i]);

        s->pfile_in_zip_read->pos_in_zipfile += 12;
        s->encrypted = 1;
    }
#endif

    return UNZ_OK;
}

extern int ZEXPORT cpl_unzOpenCurrentFile(unzFile file)
{
    return cpl_unzOpenCurrentFile3(file, nullptr, nullptr, 0, nullptr);
}

extern int ZEXPORT cpl_unzOpenCurrentFilePassword(unzFile file,
                                                  const char *password)
{
    return cpl_unzOpenCurrentFile3(file, nullptr, nullptr, 0, password);
}

extern int ZEXPORT cpl_unzOpenCurrentFile2(unzFile file, int *method,
                                           int *level, int raw)
{
    return cpl_unzOpenCurrentFile3(file, method, level, raw, nullptr);
}

/** Addition for GDAL : START */

extern uLong64 ZEXPORT cpl_unzGetCurrentFileZStreamPos(unzFile file)
{
    unz_s *s;
    file_in_zip_read_info_s *pfile_in_zip_read_info;
    s = reinterpret_cast<unz_s *>(file);
    if (file == nullptr)
        return 0;  // UNZ_PARAMERROR;
    pfile_in_zip_read_info = s->pfile_in_zip_read;
    if (pfile_in_zip_read_info == nullptr)
        return 0;  // UNZ_PARAMERROR;
    return pfile_in_zip_read_info->pos_in_zipfile +
           pfile_in_zip_read_info->byte_before_the_zipfile;
}

/** Addition for GDAL : END */

/*
  Read bytes from the current file.
  buf contain buffer where data must be copied
  len the size of buf.

  return the number of byte copied if some bytes are copied
  return 0 if the end of file was reached
  return <0 with error code if there is an error
    (UNZ_ERRNO for IO error, or zLib error for uncompress error)
*/
extern int ZEXPORT cpl_unzReadCurrentFile(unzFile file, voidp buf, unsigned len)
{
    int err = UNZ_OK;
    uInt iRead = 0;
    unz_s *s;
    file_in_zip_read_info_s *pfile_in_zip_read_info;
    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    pfile_in_zip_read_info = s->pfile_in_zip_read;

    if (pfile_in_zip_read_info == nullptr)
        return UNZ_PARAMERROR;

    if (pfile_in_zip_read_info->read_buffer == nullptr)
        return UNZ_END_OF_LIST_OF_FILE;
    if (len == 0)
        return 0;

    pfile_in_zip_read_info->stream.next_out = reinterpret_cast<Bytef *>(buf);

    pfile_in_zip_read_info->stream.avail_out = static_cast<uInt>(len);

    if ((len > pfile_in_zip_read_info->rest_read_uncompressed) &&
        (!(pfile_in_zip_read_info->raw)))
        pfile_in_zip_read_info->stream.avail_out =
            static_cast<uInt>(pfile_in_zip_read_info->rest_read_uncompressed);

    if ((len > pfile_in_zip_read_info->rest_read_compressed +
                   pfile_in_zip_read_info->stream.avail_in) &&
        (pfile_in_zip_read_info->raw))
        pfile_in_zip_read_info->stream.avail_out =
            static_cast<uInt>(pfile_in_zip_read_info->rest_read_compressed) +
            pfile_in_zip_read_info->stream.avail_in;

    while (pfile_in_zip_read_info->stream.avail_out > 0)
    {
        if ((pfile_in_zip_read_info->stream.avail_in == 0) &&
            (pfile_in_zip_read_info->rest_read_compressed > 0))
        {
            uInt uReadThis = UNZ_BUFSIZE;
            if (pfile_in_zip_read_info->rest_read_compressed < uReadThis)
                uReadThis = static_cast<uInt>(
                    pfile_in_zip_read_info->rest_read_compressed);
            if (uReadThis == 0)
                return UNZ_EOF;
            if (ZSEEK(pfile_in_zip_read_info->z_filefunc,
                      pfile_in_zip_read_info->filestream,
                      pfile_in_zip_read_info->pos_in_zipfile +
                          pfile_in_zip_read_info->byte_before_the_zipfile,
                      ZLIB_FILEFUNC_SEEK_SET) != 0)
                return UNZ_ERRNO;
            if (ZREAD(pfile_in_zip_read_info->z_filefunc,
                      pfile_in_zip_read_info->filestream,
                      pfile_in_zip_read_info->read_buffer,
                      uReadThis) != uReadThis)
                return UNZ_ERRNO;

#ifndef NOUNCRYPT
            if (s->encrypted)
            {
                uInt i;
                for (i = 0; i < uReadThis; i++)
                    pfile_in_zip_read_info->read_buffer[i] =
                        zdecode(s->keys, s->pcrc_32_tab,
                                pfile_in_zip_read_info->read_buffer[i]);
            }
#endif

            pfile_in_zip_read_info->pos_in_zipfile += uReadThis;

            pfile_in_zip_read_info->rest_read_compressed -= uReadThis;

            pfile_in_zip_read_info->stream.next_in =
                reinterpret_cast<Bytef *>(pfile_in_zip_read_info->read_buffer);
            pfile_in_zip_read_info->stream.avail_in =
                static_cast<uInt>(uReadThis);
        }

        if ((pfile_in_zip_read_info->compression_method == 0) ||
            (pfile_in_zip_read_info->raw))
        {
            uInt uDoCopy = 0;
            uInt i = 0;

            if ((pfile_in_zip_read_info->stream.avail_in == 0) &&
                (pfile_in_zip_read_info->rest_read_compressed == 0))
                return (iRead == 0) ? UNZ_EOF : iRead;

            if (pfile_in_zip_read_info->stream.avail_out <
                pfile_in_zip_read_info->stream.avail_in)
                uDoCopy = pfile_in_zip_read_info->stream.avail_out;
            else
                uDoCopy = pfile_in_zip_read_info->stream.avail_in;

            for (i = 0; i < uDoCopy; i++)
                *(pfile_in_zip_read_info->stream.next_out + i) =
                    *(pfile_in_zip_read_info->stream.next_in + i);

            pfile_in_zip_read_info->crc32 =
                crc32(pfile_in_zip_read_info->crc32,
                      pfile_in_zip_read_info->stream.next_out, uDoCopy);
            pfile_in_zip_read_info->rest_read_uncompressed -= uDoCopy;
            pfile_in_zip_read_info->stream.avail_in -= uDoCopy;
            pfile_in_zip_read_info->stream.avail_out -= uDoCopy;
            pfile_in_zip_read_info->stream.next_out += uDoCopy;
            pfile_in_zip_read_info->stream.next_in += uDoCopy;
            pfile_in_zip_read_info->stream.total_out += uDoCopy;
            iRead += uDoCopy;
        }
        else
        {
            uLong64 uTotalOutBefore, uTotalOutAfter;
            const Bytef *bufBefore;
            uLong64 uOutThis;
            int flush = Z_SYNC_FLUSH;

            uTotalOutBefore = pfile_in_zip_read_info->stream.total_out;
            bufBefore = pfile_in_zip_read_info->stream.next_out;

            /*
            if ((pfile_in_zip_read_info->rest_read_uncompressed ==
                     pfile_in_zip_read_info->stream.avail_out) &&
                (pfile_in_zip_read_info->rest_read_compressed == 0))
                flush = Z_FINISH;
            */
            err = inflate(&pfile_in_zip_read_info->stream, flush);

            if ((err >= 0) && (pfile_in_zip_read_info->stream.msg != nullptr))
                err = Z_DATA_ERROR;

            uTotalOutAfter = pfile_in_zip_read_info->stream.total_out;
            uOutThis = uTotalOutAfter - uTotalOutBefore;

            pfile_in_zip_read_info->crc32 =
                crc32(pfile_in_zip_read_info->crc32, bufBefore,
                      static_cast<uInt>(uOutThis));

            pfile_in_zip_read_info->rest_read_uncompressed -= uOutThis;

            iRead += static_cast<uInt>(uTotalOutAfter - uTotalOutBefore);

            if (err == Z_STREAM_END)
                return (iRead == 0) ? UNZ_EOF : iRead;
            if (err != Z_OK)
                break;
        }
    }

    if (err == Z_OK)
        return iRead;
    return err;
}

/*
  Give the current position in uncompressed data
*/
extern z_off_t ZEXPORT cpl_unztell(unzFile file)
{
    unz_s *s;
    file_in_zip_read_info_s *pfile_in_zip_read_info;
    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    pfile_in_zip_read_info = s->pfile_in_zip_read;

    if (pfile_in_zip_read_info == nullptr)
        return UNZ_PARAMERROR;

    return static_cast<z_off_t>(pfile_in_zip_read_info->stream.total_out);
}

/*
  return 1 if the end of file was reached, 0 elsewhere
*/
extern int ZEXPORT cpl_unzeof(unzFile file)
{
    unz_s *s;
    file_in_zip_read_info_s *pfile_in_zip_read_info;
    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    pfile_in_zip_read_info = s->pfile_in_zip_read;

    if (pfile_in_zip_read_info == nullptr)
        return UNZ_PARAMERROR;

    if (pfile_in_zip_read_info->rest_read_uncompressed == 0)
        return 1;
    else
        return 0;
}

/*
  Read extra field from the current file (opened by unzOpenCurrentFile)
  This is the local-header version of the extra field (sometimes, there is
    more info in the local-header version than in the central-header)

  if buf==NULL, it return the size of the local extra field that can be read

  if buf!=NULL, len is the size of the buffer, the extra header is copied in
    buf.
  the return value is the number of bytes copied in buf, or (if <0)
    the error code
*/
extern int ZEXPORT cpl_unzGetLocalExtrafield(unzFile file, voidp buf,
                                             unsigned len)
{
    unz_s *s;
    file_in_zip_read_info_s *pfile_in_zip_read_info;
    uInt read_now;
    uLong64 size_to_read;

    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    pfile_in_zip_read_info = s->pfile_in_zip_read;

    if (pfile_in_zip_read_info == nullptr)
        return UNZ_PARAMERROR;

    size_to_read = (pfile_in_zip_read_info->size_local_extrafield -
                    pfile_in_zip_read_info->pos_local_extrafield);

    if (buf == nullptr)
        return static_cast<int>(size_to_read);

    if (len > size_to_read)
        read_now = static_cast<uInt>(size_to_read);
    else
        read_now = static_cast<uInt>(len);

    if (read_now == 0)
        return 0;

    if (ZSEEK(pfile_in_zip_read_info->z_filefunc,
              pfile_in_zip_read_info->filestream,
              pfile_in_zip_read_info->offset_local_extrafield +
                  pfile_in_zip_read_info->pos_local_extrafield,
              ZLIB_FILEFUNC_SEEK_SET) != 0)
        return UNZ_ERRNO;

    if (ZREAD(pfile_in_zip_read_info->z_filefunc,
              pfile_in_zip_read_info->filestream, buf, read_now) != read_now)
        return UNZ_ERRNO;

    return static_cast<int>(read_now);
}

/*
  Close the file in zip opened with unzipOpenCurrentFile
  Return UNZ_CRCERROR if all the file was read but the CRC is not good
*/
extern int ZEXPORT cpl_unzCloseCurrentFile(unzFile file)
{
    int err = UNZ_OK;

    unz_s *s;
    file_in_zip_read_info_s *pfile_in_zip_read_info;
    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    pfile_in_zip_read_info = s->pfile_in_zip_read;

    if (pfile_in_zip_read_info == nullptr)
        return UNZ_PARAMERROR;

    if ((pfile_in_zip_read_info->rest_read_uncompressed == 0) &&
        (!pfile_in_zip_read_info->raw))
    {
        if (pfile_in_zip_read_info->crc32 != pfile_in_zip_read_info->crc32_wait)
            err = UNZ_CRCERROR;
    }

    TRYFREE(pfile_in_zip_read_info->read_buffer);
    pfile_in_zip_read_info->read_buffer = nullptr;
    if (pfile_in_zip_read_info->stream_initialised)
        inflateEnd(&pfile_in_zip_read_info->stream);

    pfile_in_zip_read_info->stream_initialised = 0;
    TRYFREE(pfile_in_zip_read_info);

    s->pfile_in_zip_read = nullptr;

    return err;
}

/*
  Get the global comment string of the ZipFile, in the szComment buffer.
  uSizeBuf is the size of the szComment buffer.
  return the number of byte copied or an error code <0
*/
extern int ZEXPORT cpl_unzGetGlobalComment(unzFile file, char *szComment,
                                           uLong uSizeBuf)
{
    /* int err=UNZ_OK; */
    unz_s *s;
    uLong uReadThis;
    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);

    uReadThis = uSizeBuf;
    if (uReadThis > s->gi.size_comment)
        uReadThis = s->gi.size_comment;

    if (ZSEEK(s->z_filefunc, s->filestream, s->central_pos + 22,
              ZLIB_FILEFUNC_SEEK_SET) != 0)
        return UNZ_ERRNO;

    if (uReadThis > 0)
    {
        *szComment = '\0';
        if (ZREAD(s->z_filefunc, s->filestream, szComment, uReadThis) !=
            uReadThis)
            return UNZ_ERRNO;
    }

    if ((szComment != nullptr) && (uSizeBuf > s->gi.size_comment))
        *(szComment + s->gi.size_comment) = '\0';
    return static_cast<int>(uReadThis);
}

// Additions by RX '2004.
extern uLong64 ZEXPORT cpl_unzGetOffset(unzFile file)
{
    unz_s *s;

    if (file == nullptr)
        return 0;  // UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);
    if (!s->current_file_ok)
        return 0;
    if (s->gi.number_entry != 0 && s->gi.number_entry != 0xffff)
        if (s->num_file == s->gi.number_entry)
            return 0;
    return s->pos_in_central_dir;
}

extern int ZEXPORT cpl_unzSetOffset(unzFile file, uLong64 pos)
{
    unz_s *s;

    if (file == nullptr)
        return UNZ_PARAMERROR;
    s = reinterpret_cast<unz_s *>(file);

    s->pos_in_central_dir = pos;
    s->num_file = s->gi.number_entry; /* hack */
    int err = unzlocal_GetCurrentFileInfoInternal(
        file, &s->cur_file_info, &s->cur_file_info_internal, nullptr, 0,
        nullptr, 0, nullptr, 0);
    s->current_file_ok = (err == UNZ_OK);
    return err;
}
