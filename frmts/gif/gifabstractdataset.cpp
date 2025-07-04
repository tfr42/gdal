/******************************************************************************
 *
 * Project:  GIF Driver
 * Purpose:  GIF Abstract Dataset
 * Author:   Even Rouault <even dot rouault at spatialys.com>
 *
 ****************************************************************************
 * Copyright (c) 2011-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "gifabstractdataset.h"

/************************************************************************/
/* ==================================================================== */
/*                         GIFAbstractDataset                           */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                         GIFAbstractDataset()                         */
/************************************************************************/

GIFAbstractDataset::GIFAbstractDataset()
    : fp(nullptr), hGifFile(nullptr), bGeoTransformValid(FALSE), nGCPCount(0),
      pasGCPList(nullptr), bHasReadXMPMetadata(FALSE)
{
}

/************************************************************************/
/*                        ~GIFAbstractDataset()                         */
/************************************************************************/

GIFAbstractDataset::~GIFAbstractDataset()

{
    FlushCache(true);

    if (nGCPCount > 0)
    {
        GDALDeinitGCPs(nGCPCount, pasGCPList);
        CPLFree(pasGCPList);
    }

    if (hGifFile)
        myDGifCloseFile(hGifFile);

    if (fp != nullptr)
        VSIFCloseL(fp);
}

/************************************************************************/
/*                       GIFCollectXMPMetadata()                        */
/************************************************************************/

/* See §2.1.2 of
 * http://wwwimages.adobe.com/www.adobe.com/content/dam/Adobe/en/devnet/xmp/pdfs/XMPSpecificationPart3.pdf
 */

static CPLString GIFCollectXMPMetadata(VSILFILE *fp)

{
    CPLString osXMP;

    /* Save current position to avoid disturbing GIF stream decoding */
    vsi_l_offset nCurOffset = VSIFTellL(fp);

    char abyBuffer[2048 + 1];

    VSIFSeekL(fp, 0, SEEK_SET);

    /* Loop over file */

    int iStartSearchOffset = 1024;
    while (true)
    {
        int nRead = static_cast<int>(VSIFReadL(abyBuffer + 1024, 1, 1024, fp));
        if (nRead <= 0)
            break;
        abyBuffer[1024 + nRead] = 0;

        int iFoundOffset = -1;
        for (int i = iStartSearchOffset; i < 1024 + nRead - 14; i++)
        {
            if (memcmp(abyBuffer + i, "\x21\xff\x0bXMP DataXMP", 14) == 0)
            {
                iFoundOffset = i + 14;
                break;
            }
        }

        iStartSearchOffset = 0;

        if (iFoundOffset >= 0)
        {
            int nSize = 1024 + nRead - iFoundOffset;
            char *pszXMP = (char *)VSIMalloc(nSize + 1);
            if (pszXMP == nullptr)
                break;

            pszXMP[nSize] = 0;
            memcpy(pszXMP, abyBuffer + iFoundOffset, nSize);

            /* Read from file until we find a NUL character */
            int nLen = (int)strlen(pszXMP);
            while (nLen == nSize)
            {
                char *pszNewXMP = (char *)VSIRealloc(pszXMP, nSize + 1024 + 1);
                if (pszNewXMP == nullptr)
                    break;
                pszXMP = pszNewXMP;

                nRead =
                    static_cast<int>(VSIFReadL(pszXMP + nSize, 1, 1024, fp));
                if (nRead <= 0)
                    break;

                pszXMP[nSize + nRead] = 0;
                nLen += (int)strlen(pszXMP + nSize);
                nSize += nRead;
            }

            if (nLen > 256 && pszXMP[nLen - 1] == '\x01' &&
                pszXMP[nLen - 2] == '\x02' && pszXMP[nLen - 255] == '\xff' &&
                pszXMP[nLen - 256] == '\x01')
            {
                pszXMP[nLen - 256] = 0;

                osXMP = pszXMP;
            }

            VSIFree(pszXMP);

            break;
        }

        if (nRead != 1024)
            break;

        memcpy(abyBuffer, abyBuffer + 1024, 1024);
    }

    VSIFSeekL(fp, nCurOffset, SEEK_SET);

    return osXMP;
}

/************************************************************************/
/*                       CollectXMPMetadata()                           */
/************************************************************************/

void GIFAbstractDataset::CollectXMPMetadata()

{
    if (fp == nullptr || bHasReadXMPMetadata)
        return;

    CPLString osXMP = GIFCollectXMPMetadata(fp);
    if (!osXMP.empty())
    {
        /* Avoid setting the PAM dirty bit just for that */
        const int nOldPamFlags = nPamFlags;

        char *apszMDList[2];
        apszMDList[0] = (char *)osXMP.c_str();
        apszMDList[1] = nullptr;
        SetMetadata(apszMDList, "xml:XMP");

        // cppcheck-suppress redundantAssignment
        nPamFlags = nOldPamFlags;
    }

    bHasReadXMPMetadata = TRUE;
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **GIFAbstractDataset::GetMetadataDomainList()
{
    return BuildMetadataDomainList(GDALPamDataset::GetMetadataDomainList(),
                                   TRUE, "xml:XMP", nullptr);
}

/************************************************************************/
/*                           GetMetadata()                              */
/************************************************************************/

char **GIFAbstractDataset::GetMetadata(const char *pszDomain)
{
    if (fp == nullptr)
        return nullptr;
    if (eAccess == GA_ReadOnly && !bHasReadXMPMetadata &&
        (pszDomain != nullptr && EQUAL(pszDomain, "xml:XMP")))
        CollectXMPMetadata();
    return GDALPamDataset::GetMetadata(pszDomain);
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr GIFAbstractDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    if (bGeoTransformValid)
    {
        gt = m_gt;
        return CE_None;
    }

    return GDALPamDataset::GetGeoTransform(gt);
}

/************************************************************************/
/*                            GetGCPCount()                             */
/************************************************************************/

int GIFAbstractDataset::GetGCPCount()

{
    if (nGCPCount > 0)
        return nGCPCount;

    return GDALPamDataset::GetGCPCount();
}

/************************************************************************/
/*                               GetGCPs()                              */
/************************************************************************/

const GDAL_GCP *GIFAbstractDataset::GetGCPs()

{
    if (nGCPCount > 0)
        return pasGCPList;

    return GDALPamDataset::GetGCPs();
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **GIFAbstractDataset::GetFileList()

{
    char **papszFileList = GDALPamDataset::GetFileList();

    if (!osWldFilename.empty() &&
        CSLFindString(papszFileList, osWldFilename) == -1)
    {
        papszFileList = CSLAddString(papszFileList, osWldFilename);
    }

    return papszFileList;
}

/************************************************************************/
/*                         DetectGeoreferencing()                       */
/************************************************************************/

void GIFAbstractDataset::DetectGeoreferencing(GDALOpenInfo *poOpenInfo)
{
    char *pszWldFilename = nullptr;

    bGeoTransformValid =
        GDALReadWorldFile2(poOpenInfo->pszFilename, nullptr, m_gt,
                           poOpenInfo->GetSiblingFiles(), &pszWldFilename);
    if (!bGeoTransformValid)
    {
        bGeoTransformValid =
            GDALReadWorldFile2(poOpenInfo->pszFilename, ".wld", m_gt,
                               poOpenInfo->GetSiblingFiles(), &pszWldFilename);
    }

    if (pszWldFilename)
    {
        osWldFilename = pszWldFilename;
        CPLFree(pszWldFilename);
    }
}

/************************************************************************/
/*                            myDGifOpen()                              */
/************************************************************************/

GifFileType *GIFAbstractDataset::myDGifOpen(void *userPtr, InputFunc readFunc)
{
#if defined(GIFLIB_MAJOR) && GIFLIB_MAJOR >= 5
    int nErrorCode;
    return DGifOpen(userPtr, readFunc, &nErrorCode);
#else
    return DGifOpen(userPtr, readFunc);
#endif
}

/************************************************************************/
/*                          myDGifCloseFile()                           */
/************************************************************************/

int GIFAbstractDataset::myDGifCloseFile(GifFileType *hGifFile)
{
#if defined(GIFLIB_MAJOR) &&                                                   \
    ((GIFLIB_MAJOR == 5 && GIFLIB_MINOR >= 1) || GIFLIB_MAJOR > 5)
    int nErrorCode;
    return DGifCloseFile(hGifFile, &nErrorCode);
#else
    return DGifCloseFile(hGifFile);
#endif
}

/************************************************************************/
/*                          myEGifCloseFile()                           */
/************************************************************************/

int GIFAbstractDataset::myEGifCloseFile(GifFileType *hGifFile)
{
#if defined(GIFLIB_MAJOR) &&                                                   \
    ((GIFLIB_MAJOR == 5 && GIFLIB_MINOR >= 1) || GIFLIB_MAJOR > 5)
    int nErrorCode;
    return EGifCloseFile(hGifFile, &nErrorCode);
#else
    return EGifCloseFile(hGifFile);
#endif
}

/************************************************************************/
/*                           VSIGIFReadFunc()                           */
/*                                                                      */
/*      Proxy function for reading from GIF file.                       */
/************************************************************************/

int GIFAbstractDataset::ReadFunc(GifFileType *psGFile, GifByteType *pabyBuffer,
                                 int nBytesToRead)

{
    return static_cast<int>(
        VSIFReadL(pabyBuffer, 1, nBytesToRead, (VSILFILE *)psGFile->UserData));
}

/************************************************************************/
/*                          FindFirstImage()                            */
/************************************************************************/

GifRecordType GIFAbstractDataset::FindFirstImage(GifFileType *hGifFile)
{
    GifRecordType RecordType = TERMINATE_RECORD_TYPE;

    while (DGifGetRecordType(hGifFile, &RecordType) != GIF_ERROR &&
           RecordType != TERMINATE_RECORD_TYPE &&
           RecordType != IMAGE_DESC_RECORD_TYPE)
    {
        /* Skip extension records found before IMAGE_DESC_RECORD_TYPE */
        if (RecordType == EXTENSION_RECORD_TYPE)
        {
            int nFunction;
            GifByteType *pExtData = nullptr;
            if (DGifGetExtension(hGifFile, &nFunction, &pExtData) == GIF_ERROR)
                break;
            while (pExtData != nullptr)
            {
                if (DGifGetExtensionNext(hGifFile, &pExtData) == GIF_ERROR)
                    break;
            }
        }
    }

    return RecordType;
}

/************************************************************************/
/*                        GIFAbstractRasterBand()                       */
/************************************************************************/

GIFAbstractRasterBand::GIFAbstractRasterBand(GIFAbstractDataset *poDSIn,
                                             int nBandIn,
                                             SavedImage *psSavedImage,
                                             int nBackground,
                                             int bAdvertiseInterlacedMDI)
    : psImage(psSavedImage), panInterlaceMap(nullptr), poColorTable(nullptr),
      nTransparentColor(0)
{
    poDS = poDSIn;
    nBand = nBandIn;

    eDataType = GDT_Byte;

    nBlockXSize = poDS->GetRasterXSize();
    nBlockYSize = 1;

    if (psImage == nullptr)
        return;

    /* -------------------------------------------------------------------- */
    /*      Setup interlacing map if required.                              */
    /* -------------------------------------------------------------------- */
    panInterlaceMap = nullptr;
    if (psImage->ImageDesc.Interlace)
    {
        int iLine = 0;

        if (bAdvertiseInterlacedMDI)
            poDS->SetMetadataItem("INTERLACED", "YES", "IMAGE_STRUCTURE");

        panInterlaceMap = (int *)CPLCalloc(poDSIn->nRasterYSize, sizeof(int));

        for (int i = 0; i < 4; i++)
        {
            for (int j = InterlacedOffset[i]; j < poDSIn->nRasterYSize;
                 j += InterlacedJumps[i])
                panInterlaceMap[j] = iLine++;
        }
    }
    else if (bAdvertiseInterlacedMDI)
    {
        poDS->SetMetadataItem("INTERLACED", "NO", "IMAGE_STRUCTURE");
    }

    /* -------------------------------------------------------------------- */
    /*      Check for transparency.  We just take the first graphic         */
    /*      control extension block we find, if any.                        */
    /* -------------------------------------------------------------------- */
    nTransparentColor = -1;
    for (int iExtBlock = 0; iExtBlock < psImage->ExtensionBlockCount;
         iExtBlock++)
    {
        if (psImage->ExtensionBlocks[iExtBlock].Function != 0xf9 ||
            psImage->ExtensionBlocks[iExtBlock].ByteCount < 4)
            continue;

        unsigned char *pExtData = reinterpret_cast<unsigned char *>(
            psImage->ExtensionBlocks[iExtBlock].Bytes);

        /* check if transparent color flag is set */
        if (!(pExtData[0] & 0x1))
            continue;

        nTransparentColor = pExtData[3];
    }

    /* -------------------------------------------------------------------- */
    /*      Setup colormap.                                                 */
    /* -------------------------------------------------------------------- */
    ColorMapObject *psGifCT = psImage->ImageDesc.ColorMap;
    if (psGifCT == nullptr)
        psGifCT = poDSIn->hGifFile->SColorMap;

    poColorTable = new GDALColorTable();
    for (int iColor = 0; iColor < psGifCT->ColorCount; iColor++)
    {
        GDALColorEntry oEntry;

        oEntry.c1 = psGifCT->Colors[iColor].Red;
        oEntry.c2 = psGifCT->Colors[iColor].Green;
        oEntry.c3 = psGifCT->Colors[iColor].Blue;

        if (iColor == nTransparentColor)
            oEntry.c4 = 0;
        else
            oEntry.c4 = 255;

        poColorTable->SetColorEntry(iColor, &oEntry);
    }

    /* -------------------------------------------------------------------- */
    /*      If we have a background value, return it here.  Some            */
    /*      applications might want to treat this as transparent, but in    */
    /*      many uses this is inappropriate so we don't return it as        */
    /*      nodata or transparent.                                          */
    /* -------------------------------------------------------------------- */
    if (nBackground != 255)
    {
        char szBackground[10];

        snprintf(szBackground, sizeof(szBackground), "%d", nBackground);
        SetMetadataItem("GIF_BACKGROUND", szBackground);
    }
}

/************************************************************************/
/*                       ~GIFAbstractRasterBand()                       */
/************************************************************************/

GIFAbstractRasterBand::~GIFAbstractRasterBand()

{
    if (poColorTable != nullptr)
        delete poColorTable;

    CPLFree(panInterlaceMap);
}

/************************************************************************/
/*                       GetColorInterpretation()                       */
/************************************************************************/

GDALColorInterp GIFAbstractRasterBand::GetColorInterpretation()

{
    return GCI_PaletteIndex;
}

/************************************************************************/
/*                           GetColorTable()                            */
/************************************************************************/

GDALColorTable *GIFAbstractRasterBand::GetColorTable()

{
    return poColorTable;
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double GIFAbstractRasterBand::GetNoDataValue(int *pbSuccess)

{
    if (pbSuccess != nullptr)
        *pbSuccess = nTransparentColor != -1;

    return nTransparentColor;
}
