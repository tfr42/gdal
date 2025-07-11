/******************************************************************************
 *
 * Project:  LCP Driver
 * Purpose:  FARSITE v.4 Landscape file (.lcp) reader for GDAL
 * Author:   Chris Toney
 *
 ******************************************************************************
 * Copyright (c) 2008, Chris Toney
 * Copyright (c) 2008-2011, Even Rouault <even dot rouault at spatialys.com>
 * Copyright (c) 2013, Kyle Shannon <kyle at pobox dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "cpl_string.h"
#include "gdal_frmts.h"
#include "ogr_spatialref.h"
#include "rawdataset.h"

#include <algorithm>
#include <limits>

constexpr size_t LCP_HEADER_SIZE = 7316;
constexpr int LCP_MAX_BANDS = 10;
constexpr int LCP_MAX_PATH = 256;
constexpr int LCP_MAX_DESC = 512;
constexpr int LCP_MAX_CLASSES = 100;

/************************************************************************/
/* ==================================================================== */
/*                              LCPDataset                              */
/* ==================================================================== */
/************************************************************************/

class LCPDataset final : public RawDataset
{
    VSILFILE *fpImage;  // image data file.
    char pachHeader[LCP_HEADER_SIZE];

    CPLString osPrjFilename{};
    OGRSpatialReference m_oSRS{};

    static CPLErr ClassifyBandData(GDALRasterBand *poBand, GInt32 &nNumClasses,
                                   GInt32 *panClasses);

    CPL_DISALLOW_COPY_ASSIGN(LCPDataset)

    CPLErr Close() override;

  public:
    LCPDataset();
    ~LCPDataset() override;

    char **GetFileList(void) override;

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;

    static int Identify(GDALOpenInfo *);
    static GDALDataset *Open(GDALOpenInfo *);
    static GDALDataset *CreateCopy(const char *pszFilename,
                                   GDALDataset *poSrcDS, int bStrict,
                                   char **papszOptions,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData);

    const OGRSpatialReference *GetSpatialRef() const override
    {
        return &m_oSRS;
    }
};

/************************************************************************/
/*                            LCPDataset()                              */
/************************************************************************/

LCPDataset::LCPDataset() : fpImage(nullptr)
{
    memset(pachHeader, 0, sizeof(pachHeader));
}

/************************************************************************/
/*                            ~LCPDataset()                             */
/************************************************************************/

LCPDataset::~LCPDataset()

{
    LCPDataset::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr LCPDataset::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (LCPDataset::FlushCache(true) != CE_None)
            eErr = CE_Failure;

        if (fpImage)
        {
            if (VSIFCloseL(fpImage) != 0)
            {
                CPLError(CE_Failure, CPLE_FileIO, "I/O error");
                eErr = CE_Failure;
            }
        }

        if (GDALPamDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr LCPDataset::GetGeoTransform(GDALGeoTransform &gt) const
{
    double dfEast = 0.0;
    double dfWest = 0.0;
    double dfNorth = 0.0;
    double dfSouth = 0.0;
    double dfCellX = 0.0;
    double dfCellY = 0.0;

    memcpy(&dfEast, pachHeader + 4172, sizeof(double));
    memcpy(&dfWest, pachHeader + 4180, sizeof(double));
    memcpy(&dfNorth, pachHeader + 4188, sizeof(double));
    memcpy(&dfSouth, pachHeader + 4196, sizeof(double));
    memcpy(&dfCellX, pachHeader + 4208, sizeof(double));
    memcpy(&dfCellY, pachHeader + 4216, sizeof(double));
    CPL_LSBPTR64(&dfEast);
    CPL_LSBPTR64(&dfWest);
    CPL_LSBPTR64(&dfNorth);
    CPL_LSBPTR64(&dfSouth);
    CPL_LSBPTR64(&dfCellX);
    CPL_LSBPTR64(&dfCellY);

    gt[0] = dfWest;
    gt[3] = dfNorth;
    gt[1] = dfCellX;
    gt[2] = 0.0;

    gt[4] = 0.0;
    gt[5] = -1 * dfCellY;

    return CE_None;
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

int LCPDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    /* -------------------------------------------------------------------- */
    /*      Verify that this is a FARSITE v.4 LCP file                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->nHeaderBytes < 50)
        return FALSE;

    /* check if first three fields have valid data */
    if ((CPL_LSBSINT32PTR(poOpenInfo->pabyHeader) != 20 &&
         CPL_LSBSINT32PTR(poOpenInfo->pabyHeader) != 21) ||
        (CPL_LSBSINT32PTR(poOpenInfo->pabyHeader + 4) != 20 &&
         CPL_LSBSINT32PTR(poOpenInfo->pabyHeader + 4) != 21) ||
        (CPL_LSBSINT32PTR(poOpenInfo->pabyHeader + 8) < -90 ||
         CPL_LSBSINT32PTR(poOpenInfo->pabyHeader + 8) > 90))
    {
        return FALSE;
    }

/* -------------------------------------------------------------------- */
/*      Check file extension                                            */
/* -------------------------------------------------------------------- */
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    const char *pszFileExtension = poOpenInfo->osExtension.c_str();
    if (!EQUAL(pszFileExtension, "lcp"))
    {
        return FALSE;
    }
#endif

    return TRUE;
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **LCPDataset::GetFileList()

{
    char **papszFileList = GDALPamDataset::GetFileList();

    if (!m_oSRS.IsEmpty())
    {
        papszFileList = CSLAddString(papszFileList, osPrjFilename);
    }

    return papszFileList;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *LCPDataset::Open(GDALOpenInfo *poOpenInfo)

{
    /* -------------------------------------------------------------------- */
    /*      Verify that this is a FARSITE LCP file                          */
    /* -------------------------------------------------------------------- */
    if (!Identify(poOpenInfo) || poOpenInfo->fpL == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Confirm the requested access is supported.                      */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("LCP");
        return nullptr;
    }
    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    auto poDS = std::make_unique<LCPDataset>();
    std::swap(poDS->fpImage, poOpenInfo->fpL);

    /* -------------------------------------------------------------------- */
    /*      Read the header and extract some information.                   */
    /* -------------------------------------------------------------------- */
    if (VSIFSeekL(poDS->fpImage, 0, SEEK_SET) < 0 ||
        VSIFReadL(poDS->pachHeader, 1, LCP_HEADER_SIZE, poDS->fpImage) !=
            LCP_HEADER_SIZE)
    {
        CPLError(CE_Failure, CPLE_FileIO, "File too short");
        return nullptr;
    }

    const int nWidth = CPL_LSBSINT32PTR(poDS->pachHeader + 4164);
    const int nHeight = CPL_LSBSINT32PTR(poDS->pachHeader + 4168);

    poDS->nRasterXSize = nWidth;
    poDS->nRasterYSize = nHeight;

    if (!GDALCheckDatasetDimensions(poDS->nRasterXSize, poDS->nRasterYSize))
    {
        return nullptr;
    }

    // Crown fuels = canopy height, canopy base height, canopy bulk density.
    // 21 = have them, 20 = do not have them
    const bool bHaveCrownFuels =
        CPL_TO_BOOL(CPL_LSBSINT32PTR(poDS->pachHeader + 0) - 20);
    // Ground fuels = duff loading, coarse woody.
    const bool bHaveGroundFuels =
        CPL_TO_BOOL(CPL_LSBSINT32PTR(poDS->pachHeader + 4) - 20);

    int nBands = 0;
    if (bHaveCrownFuels)
    {
        if (bHaveGroundFuels)
            nBands = 10;
        else
            nBands = 8;
    }
    else
    {
        if (bHaveGroundFuels)
            nBands = 7;
        else
            nBands = 5;
    }

    // Add dataset-level metadata.

    int nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 8);
    char szTemp[32] = {'\0'};
    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
    poDS->SetMetadataItem("LATITUDE", szTemp);

    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 4204);
    if (nTemp == 0)
        poDS->SetMetadataItem("LINEAR_UNIT", "Meters");
    if (nTemp == 1)
        poDS->SetMetadataItem("LINEAR_UNIT", "Feet");

    poDS->pachHeader[LCP_HEADER_SIZE - 1] = '\0';
    poDS->SetMetadataItem("DESCRIPTION", poDS->pachHeader + 6804);

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */

    const int iPixelSize = nBands * 2;

    if (nWidth > INT_MAX / iPixelSize)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Int overflow occurred");
        return nullptr;
    }

    for (int iBand = 1; iBand <= nBands; iBand++)
    {
        auto poBand =
            RawRasterBand::Create(poDS.get(), iBand, poDS->fpImage,
                                  LCP_HEADER_SIZE + ((iBand - 1) * 2),
                                  iPixelSize, iPixelSize * nWidth, GDT_Int16,
                                  RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN,
                                  RawRasterBand::OwnFP::NO);
        if (!poBand)
            return nullptr;

        switch (iBand)
        {
            case 1:
                poBand->SetDescription("Elevation");

                nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4224);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("ELEVATION_UNIT", szTemp);

                if (nTemp == 0)
                    poBand->SetMetadataItem("ELEVATION_UNIT_NAME", "Meters");
                if (nTemp == 1)
                    poBand->SetMetadataItem("ELEVATION_UNIT_NAME", "Feet");

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 44);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("ELEVATION_MIN", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 48);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("ELEVATION_MAX", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 52);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("ELEVATION_NUM_CLASSES", szTemp);

                *(poDS->pachHeader + 4244 + 255) = '\0';
                poBand->SetMetadataItem("ELEVATION_FILE",
                                        poDS->pachHeader + 4244);

                break;

            case 2:
                poBand->SetDescription("Slope");

                nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4226);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("SLOPE_UNIT", szTemp);

                if (nTemp == 0)
                    poBand->SetMetadataItem("SLOPE_UNIT_NAME", "Degrees");
                if (nTemp == 1)
                    poBand->SetMetadataItem("SLOPE_UNIT_NAME", "Percent");

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 456);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("SLOPE_MIN", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 460);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("SLOPE_MAX", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 464);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("SLOPE_NUM_CLASSES", szTemp);

                *(poDS->pachHeader + 4500 + 255) = '\0';
                poBand->SetMetadataItem("SLOPE_FILE", poDS->pachHeader + 4500);

                break;

            case 3:
                poBand->SetDescription("Aspect");

                nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4228);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("ASPECT_UNIT", szTemp);

                if (nTemp == 0)
                    poBand->SetMetadataItem("ASPECT_UNIT_NAME",
                                            "Grass categories");
                if (nTemp == 1)
                    poBand->SetMetadataItem("ASPECT_UNIT_NAME",
                                            "Grass degrees");
                if (nTemp == 2)
                    poBand->SetMetadataItem("ASPECT_UNIT_NAME",
                                            "Azimuth degrees");

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 868);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("ASPECT_MIN", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 872);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("ASPECT_MAX", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 876);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("ASPECT_NUM_CLASSES", szTemp);

                *(poDS->pachHeader + 4756 + 255) = '\0';
                poBand->SetMetadataItem("ASPECT_FILE", poDS->pachHeader + 4756);

                break;

            case 4:
            {
                poBand->SetDescription("Fuel models");

                nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4230);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("FUEL_MODEL_OPTION", szTemp);

                if (nTemp == 0)
                    poBand->SetMetadataItem(
                        "FUEL_MODEL_OPTION_DESC",
                        "no custom models AND no conversion file needed");
                if (nTemp == 1)
                    poBand->SetMetadataItem(
                        "FUEL_MODEL_OPTION_DESC",
                        "custom models BUT no conversion file needed");
                if (nTemp == 2)
                    poBand->SetMetadataItem(
                        "FUEL_MODEL_OPTION_DESC",
                        "no custom models BUT conversion file needed");
                if (nTemp == 3)
                    poBand->SetMetadataItem(
                        "FUEL_MODEL_OPTION_DESC",
                        "custom models AND conversion file needed");

                const int nMinFM = CPL_LSBSINT32PTR(poDS->pachHeader + 1280);
                snprintf(szTemp, sizeof(szTemp), "%d", nMinFM);
                poBand->SetMetadataItem("FUEL_MODEL_MIN", szTemp);

                const int nMaxFM = CPL_LSBSINT32PTR(poDS->pachHeader + 1284);
                snprintf(szTemp, sizeof(szTemp), "%d", nMaxFM);
                poBand->SetMetadataItem("FUEL_MODEL_MAX", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 1288);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("FUEL_MODEL_NUM_CLASSES", szTemp);

                std::string osValues;
                if (nTemp > 0 && nTemp <= 100)
                {
                    for (int i = 0; i <= nTemp; i++)
                    {
                        const int nTemp2 = CPL_LSBSINT32PTR(poDS->pachHeader +
                                                            (1292 + (i * 4)));
                        if (nTemp2 >= nMinFM && nTemp2 <= nMaxFM)
                        {
                            snprintf(szTemp, sizeof(szTemp), "%d", nTemp2);
                            if (!osValues.empty())
                                osValues += ',';
                            osValues += szTemp;
                        }
                    }
                }
                poBand->SetMetadataItem("FUEL_MODEL_VALUES", osValues.c_str());

                *(poDS->pachHeader + 5012 + 255) = '\0';
                poBand->SetMetadataItem("FUEL_MODEL_FILE",
                                        poDS->pachHeader + 5012);

                break;
            }
            case 5:
                poBand->SetDescription("Canopy cover");

                nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4232);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CANOPY_COV_UNIT", szTemp);

                if (nTemp == 0)
                    poBand->SetMetadataItem("CANOPY_COV_UNIT_NAME",
                                            "Categories (0-4)");
                if (nTemp == 1)
                    poBand->SetMetadataItem("CANOPY_COV_UNIT_NAME", "Percent");

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 1692);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CANOPY_COV_MIN", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 1696);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CANOPY_COV_MAX", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 1700);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CANOPY_COV_NUM_CLASSES", szTemp);

                *(poDS->pachHeader + 5268 + 255) = '\0';
                poBand->SetMetadataItem("CANOPY_COV_FILE",
                                        poDS->pachHeader + 5268);

                break;

            case 6:
                if (bHaveCrownFuels)
                {
                    poBand->SetDescription("Canopy height");

                    nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4234);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CANOPY_HT_UNIT", szTemp);

                    if (nTemp == 1)
                        poBand->SetMetadataItem("CANOPY_HT_UNIT_NAME",
                                                "Meters");
                    if (nTemp == 2)
                        poBand->SetMetadataItem("CANOPY_HT_UNIT_NAME", "Feet");
                    if (nTemp == 3)
                        poBand->SetMetadataItem("CANOPY_HT_UNIT_NAME",
                                                "Meters x 10");
                    if (nTemp == 4)
                        poBand->SetMetadataItem("CANOPY_HT_UNIT_NAME",
                                                "Feet x 10");

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 2104);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CANOPY_HT_MIN", szTemp);

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 2108);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CANOPY_HT_MAX", szTemp);

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 2112);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CANOPY_HT_NUM_CLASSES", szTemp);

                    *(poDS->pachHeader + 5524 + 255) = '\0';
                    poBand->SetMetadataItem("CANOPY_HT_FILE",
                                            poDS->pachHeader + 5524);
                }
                else
                {
                    poBand->SetDescription("Duff");

                    nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4240);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("DUFF_UNIT", szTemp);

                    if (nTemp == 1)
                        poBand->SetMetadataItem("DUFF_UNIT_NAME", "Mg/ha");
                    if (nTemp == 2)
                        poBand->SetMetadataItem("DUFF_UNIT_NAME", "t/ac");

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3340);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("DUFF_MIN", szTemp);

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3344);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("DUFF_MAX", szTemp);

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3348);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("DUFF_NUM_CLASSES", szTemp);

                    *(poDS->pachHeader + 6292 + 255) = '\0';
                    poBand->SetMetadataItem("DUFF_FILE",
                                            poDS->pachHeader + 6292);
                }
                break;

            case 7:
                if (bHaveCrownFuels)
                {
                    poBand->SetDescription("Canopy base height");

                    nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4236);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CBH_UNIT", szTemp);

                    if (nTemp == 1)
                        poBand->SetMetadataItem("CBH_UNIT_NAME", "Meters");
                    if (nTemp == 2)
                        poBand->SetMetadataItem("CBH_UNIT_NAME", "Feet");
                    if (nTemp == 3)
                        poBand->SetMetadataItem("CBH_UNIT_NAME", "Meters x 10");
                    if (nTemp == 4)
                        poBand->SetMetadataItem("CBH_UNIT_NAME", "Feet x 10");

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 2516);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CBH_MIN", szTemp);

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 2520);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CBH_MAX", szTemp);

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 2524);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CBH_NUM_CLASSES", szTemp);

                    *(poDS->pachHeader + 5780 + 255) = '\0';
                    poBand->SetMetadataItem("CBH_FILE",
                                            poDS->pachHeader + 5780);
                }
                else
                {
                    poBand->SetDescription("Coarse woody debris");

                    nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4242);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CWD_OPTION", szTemp);

                    // if ( nTemp == 1 )
                    //    poBand->SetMetadataItem( "CWD_UNIT_DESC", "?" );
                    // if ( nTemp == 2 )
                    //    poBand->SetMetadataItem( "CWD_UNIT_DESC", "?" );

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3752);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CWD_MIN", szTemp);

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3756);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CWD_MAX", szTemp);

                    nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3760);
                    snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                    poBand->SetMetadataItem("CWD_NUM_CLASSES", szTemp);

                    *(poDS->pachHeader + 6548 + 255) = '\0';
                    poBand->SetMetadataItem("CWD_FILE",
                                            poDS->pachHeader + 6548);
                }
                break;

            case 8:
                poBand->SetDescription("Canopy bulk density");

                nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4238);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CBD_UNIT", szTemp);

                if (nTemp == 1)
                    poBand->SetMetadataItem("CBD_UNIT_NAME", "kg/m^3");
                if (nTemp == 2)
                    poBand->SetMetadataItem("CBD_UNIT_NAME", "lb/ft^3");
                if (nTemp == 3)
                    poBand->SetMetadataItem("CBD_UNIT_NAME", "kg/m^3 x 100");
                if (nTemp == 4)
                    poBand->SetMetadataItem("CBD_UNIT_NAME", "lb/ft^3 x 1000");

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 2928);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CBD_MIN", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 2932);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CBD_MAX", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 2936);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CBD_NUM_CLASSES", szTemp);

                *(poDS->pachHeader + 6036 + 255) = '\0';
                poBand->SetMetadataItem("CBD_FILE", poDS->pachHeader + 6036);

                break;

            case 9:
                poBand->SetDescription("Duff");

                nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4240);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("DUFF_UNIT", szTemp);

                if (nTemp == 1)
                    poBand->SetMetadataItem("DUFF_UNIT_NAME", "Mg/ha");
                if (nTemp == 2)
                    poBand->SetMetadataItem("DUFF_UNIT_NAME", "t/ac");

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3340);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("DUFF_MIN", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3344);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("DUFF_MAX", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3348);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("DUFF_NUM_CLASSES", szTemp);

                *(poDS->pachHeader + 6292 + 255) = '\0';
                poBand->SetMetadataItem("DUFF_FILE", poDS->pachHeader + 6292);

                break;

            case 10:
                poBand->SetDescription("Coarse woody debris");

                nTemp = CPL_LSBUINT16PTR(poDS->pachHeader + 4242);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CWD_OPTION", szTemp);

                // if ( nTemp == 1 )
                //    poBand->SetMetadataItem( "CWD_UNIT_DESC", "?" );
                // if ( nTemp == 2 )
                //    poBand->SetMetadataItem( "CWD_UNIT_DESC", "?" );

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3752);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CWD_MIN", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3756);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CWD_MAX", szTemp);

                nTemp = CPL_LSBSINT32PTR(poDS->pachHeader + 3760);
                snprintf(szTemp, sizeof(szTemp), "%d", nTemp);
                poBand->SetMetadataItem("CWD_NUM_CLASSES", szTemp);

                *(poDS->pachHeader + 6548 + 255) = '\0';
                poBand->SetMetadataItem("CWD_FILE", poDS->pachHeader + 6548);

                break;
        }

        poDS->SetBand(iBand, std::move(poBand));
    }

    /* -------------------------------------------------------------------- */
    /*      Try to read projection file.                                    */
    /* -------------------------------------------------------------------- */
    char *const pszDirname =
        CPLStrdup(CPLGetPathSafe(poOpenInfo->pszFilename).c_str());
    char *const pszBasename =
        CPLStrdup(CPLGetBasenameSafe(poOpenInfo->pszFilename).c_str());

    poDS->osPrjFilename = CPLFormFilenameSafe(pszDirname, pszBasename, "prj");
    VSIStatBufL sStatBuf;
    int nRet = VSIStatL(poDS->osPrjFilename, &sStatBuf);

    if (nRet != 0 && VSIIsCaseSensitiveFS(poDS->osPrjFilename))
    {
        poDS->osPrjFilename =
            CPLFormFilenameSafe(pszDirname, pszBasename, "PRJ");
        nRet = VSIStatL(poDS->osPrjFilename, &sStatBuf);
    }

    if (nRet == 0)
    {
        char **papszPrj = CSLLoad(poDS->osPrjFilename);

        CPLDebug("LCP", "Loaded SRS from %s", poDS->osPrjFilename.c_str());

        poDS->m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        if (poDS->m_oSRS.importFromESRI(papszPrj) != OGRERR_NONE)
        {
            poDS->m_oSRS.Clear();
        }

        CSLDestroy(papszPrj);
    }

    CPLFree(pszDirname);
    CPLFree(pszBasename);

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for external overviews.                                   */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS.get(), poOpenInfo->pszFilename,
                                poOpenInfo->GetSiblingFiles());

    return poDS.release();
}

/************************************************************************/
/*                          ClassifyBandData()                          */
/*  Classify a band and store 99 or fewer unique values.  If there are  */
/*  more than 99 unique values, then set pnNumClasses to -1 as a flag   */
/*  that represents this.  These are legacy values in the header, and   */
/*  while we should never deprecate them, we could possibly not         */
/*  calculate them by default.                                          */
/************************************************************************/

CPLErr LCPDataset::ClassifyBandData(GDALRasterBand *poBand, GInt32 &nNumClasses,
                                    GInt32 *panClasses)
{
    CPLAssert(poBand);
    CPLAssert(panClasses);

    const int nXSize = poBand->GetXSize();
    const int nYSize = poBand->GetYSize();

    GInt16 *panValues =
        static_cast<GInt16 *>(CPLMalloc(sizeof(GInt16) * nXSize));
    constexpr int MIN_VAL = std::numeric_limits<GInt16>::min();
    constexpr int MAX_VAL = std::numeric_limits<GInt16>::max();
    constexpr int RANGE_VAL = MAX_VAL - MIN_VAL + 1;
    GByte *pabyFlags = static_cast<GByte *>(CPLCalloc(1, RANGE_VAL));

    /* so that values in [-32768,32767] are mapped to [0,65535] in pabyFlags */
    constexpr int OFFSET = -MIN_VAL;

    int nFound = 0;
    bool bTooMany = false;
    CPLErr eErr = CE_None;
    for (int iLine = 0; iLine < nYSize; iLine++)
    {
        eErr = poBand->RasterIO(GF_Read, 0, iLine, nXSize, 1, panValues, nXSize,
                                1, GDT_Int16, 0, 0, nullptr);
        if (eErr != CE_None)
            break;
        for (int iPixel = 0; iPixel < nXSize; iPixel++)
        {
            if (panValues[iPixel] == -9999)
            {
                continue;
            }
            if (nFound == LCP_MAX_CLASSES)
            {
                CPLDebug("LCP",
                         "Found more that %d unique values in "
                         "band %d.  Not 'classifying' the data.",
                         LCP_MAX_CLASSES - 1, poBand->GetBand());
                nFound = -1;
                bTooMany = true;
                break;
            }
            if (pabyFlags[panValues[iPixel] + OFFSET] == 0)
            {
                pabyFlags[panValues[iPixel] + OFFSET] = 1;
                nFound++;
            }
        }
        if (bTooMany)
            break;
    }
    if (!bTooMany)
    {
        // The classes are always padded with a leading 0.  This was for aligning
        // offsets, or making it a 1-based array instead of 0-based.
        panClasses[0] = 0;
        for (int j = 0, nIndex = 1; j < RANGE_VAL; j++)
        {
            if (pabyFlags[j] == 1)
            {
                panClasses[nIndex++] = j;
            }
        }
    }
    nNumClasses = nFound;
    CPLFree(pabyFlags);
    CPLFree(panValues);

    return eErr;
}

/************************************************************************/
/*                          CreateCopy()                                */
/************************************************************************/

GDALDataset *LCPDataset::CreateCopy(const char *pszFilename,
                                    GDALDataset *poSrcDS, int bStrict,
                                    char **papszOptions,
                                    GDALProgressFunc pfnProgress,
                                    void *pProgressData)

{
    /* -------------------------------------------------------------------- */
    /*      Verify input options.                                           */
    /* -------------------------------------------------------------------- */
    const int nBands = poSrcDS->GetRasterCount();
    if (nBands != 5 && nBands != 7 && nBands != 8 && nBands != 10)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "LCP driver doesn't support %d bands.  Must be 5, 7, 8 "
                 "or 10 bands.",
                 nBands);
        return nullptr;
    }

    GDALDataType eType = poSrcDS->GetRasterBand(1)->GetRasterDataType();
    if (eType != GDT_Int16 && bStrict)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "LCP only supports 16-bit signed integer data types.");
        return nullptr;
    }
    else if (eType != GDT_Int16)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Setting data type to 16-bit integer.");
    }

    /* -------------------------------------------------------------------- */
    /*      What schema do we have (ground/crown fuels)                     */
    /* -------------------------------------------------------------------- */

    const bool bHaveCrownFuels = nBands == 8 || nBands == 10;
    const bool bHaveGroundFuels = nBands == 7 || nBands == 10;

    // Since units are 'configurable', we should check for user
    // defined units.  This is a bit cumbersome, but the user should
    // be allowed to specify none to get default units/options.  Use
    // default units every chance we get.
    GInt16 panMetadata[LCP_MAX_BANDS] = {
        0,  // 0 ELEVATION_UNIT
        0,  // 1 SLOPE_UNIT
        2,  // 2 ASPECT_UNIT
        0,  // 3 FUEL_MODEL_OPTION
        1,  // 4 CANOPY_COV_UNIT
        3,  // 5 CANOPY_HT_UNIT
        3,  // 6 CBH_UNIT
        3,  // 7 CBD_UNIT
        1,  // 8 DUFF_UNIT
        0,  // 9 CWD_OPTION
    };

    // Check the units/options for user overrides.
    const char *pszTemp =
        CSLFetchNameValueDef(papszOptions, "ELEVATION_UNIT", "METERS");
    if (STARTS_WITH_CI(pszTemp, "METER"))
    {
        panMetadata[0] = 0;
    }
    else if (EQUAL(pszTemp, "FEET") || EQUAL(pszTemp, "FOOT"))
    {
        panMetadata[0] = 1;
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid value (%s) for ELEVATION_UNIT.", pszTemp);
        return nullptr;
    }

    pszTemp = CSLFetchNameValueDef(papszOptions, "SLOPE_UNIT", "DEGREES");
    if (EQUAL(pszTemp, "DEGREES"))
    {
        panMetadata[1] = 0;
    }
    else if (EQUAL(pszTemp, "PERCENT"))
    {
        panMetadata[1] = 1;
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid value (%s) for SLOPE_UNIT.", pszTemp);
        return nullptr;
    }

    pszTemp =
        CSLFetchNameValueDef(papszOptions, "ASPECT_UNIT", "AZIMUTH_DEGREES");
    if (EQUAL(pszTemp, "GRASS_CATEGORIES"))
    {
        panMetadata[2] = 0;
    }
    else if (EQUAL(pszTemp, "GRASS_DEGREES"))
    {
        panMetadata[2] = 1;
    }
    else if (EQUAL(pszTemp, "AZIMUTH_DEGREES"))
    {
        panMetadata[2] = 2;
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid value (%s) for ASPECT_UNIT.", pszTemp);
        return nullptr;
    }

    pszTemp = CSLFetchNameValueDef(papszOptions, "FUEL_MODEL_OPTION",
                                   "NO_CUSTOM_AND_NO_FILE");
    if (EQUAL(pszTemp, "NO_CUSTOM_AND_NO_FILE"))
    {
        panMetadata[3] = 0;
    }
    else if (EQUAL(pszTemp, "CUSTOM_AND_NO_FILE"))
    {
        panMetadata[3] = 1;
    }
    else if (EQUAL(pszTemp, "NO_CUSTOM_AND_FILE"))
    {
        panMetadata[3] = 2;
    }
    else if (EQUAL(pszTemp, "CUSTOM_AND_FILE"))
    {
        panMetadata[3] = 3;
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid value (%s) for FUEL_MODEL_OPTION.", pszTemp);
        return nullptr;
    }

    pszTemp = CSLFetchNameValueDef(papszOptions, "CANOPY_COV_UNIT", "PERCENT");
    if (EQUAL(pszTemp, "CATEGORIES"))
    {
        panMetadata[4] = 0;
    }
    else if (EQUAL(pszTemp, "PERCENT"))
    {
        panMetadata[4] = 1;
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Invalid value (%s) for CANOPY_COV_UNIT.", pszTemp);
        return nullptr;
    }

    if (bHaveCrownFuels)
    {
        pszTemp =
            CSLFetchNameValueDef(papszOptions, "CANOPY_HT_UNIT", "METERS_X_10");
        if (EQUAL(pszTemp, "METERS") || EQUAL(pszTemp, "METER"))
        {
            panMetadata[5] = 1;
        }
        else if (EQUAL(pszTemp, "FEET") || EQUAL(pszTemp, "FOOT"))
        {
            panMetadata[5] = 2;
        }
        else if (EQUAL(pszTemp, "METERS_X_10") || EQUAL(pszTemp, "METER_X_10"))
        {
            panMetadata[5] = 3;
        }
        else if (EQUAL(pszTemp, "FEET_X_10") || EQUAL(pszTemp, "FOOT_X_10"))
        {
            panMetadata[5] = 4;
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid value (%s) for CANOPY_HT_UNIT.", pszTemp);
            return nullptr;
        }

        pszTemp = CSLFetchNameValueDef(papszOptions, "CBH_UNIT", "METERS_X_10");
        if (EQUAL(pszTemp, "METERS") || EQUAL(pszTemp, "METER"))
        {
            panMetadata[6] = 1;
        }
        else if (EQUAL(pszTemp, "FEET") || EQUAL(pszTemp, "FOOT"))
        {
            panMetadata[6] = 2;
        }
        else if (EQUAL(pszTemp, "METERS_X_10") || EQUAL(pszTemp, "METER_X_10"))
        {
            panMetadata[6] = 3;
        }
        else if (EQUAL(pszTemp, "FEET_X_10") || EQUAL(pszTemp, "FOOT_X_10"))
        {
            panMetadata[6] = 4;
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid value (%s) for CBH_UNIT.", pszTemp);
            return nullptr;
        }

        pszTemp = CSLFetchNameValueDef(papszOptions, "CBD_UNIT",
                                       "KG_PER_CUBIC_METER_X_100");
        if (EQUAL(pszTemp, "KG_PER_CUBIC_METER"))
        {
            panMetadata[7] = 1;
        }
        else if (EQUAL(pszTemp, "POUND_PER_CUBIC_FOOT"))
        {
            panMetadata[7] = 2;
        }
        else if (EQUAL(pszTemp, "KG_PER_CUBIC_METER_X_100"))
        {
            panMetadata[7] = 3;
        }
        else if (EQUAL(pszTemp, "POUND_PER_CUBIC_FOOT_X_1000"))
        {
            panMetadata[7] = 4;
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid value (%s) for CBD_UNIT.", pszTemp);
            return nullptr;
        }
    }

    if (bHaveGroundFuels)
    {
        pszTemp = CSLFetchNameValueDef(papszOptions, "DUFF_UNIT",
                                       "MG_PER_HECTARE_X_10");
        if (EQUAL(pszTemp, "MG_PER_HECTARE_X_10"))
        {
            panMetadata[8] = 1;
        }
        else if (EQUAL(pszTemp, "TONS_PER_ACRE_X_10"))
        {
            panMetadata[8] = 2;
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Invalid value (%s) for DUFF_UNIT.", pszTemp);
            return nullptr;
        }

        panMetadata[9] = 1;
    }

    // Calculate the stats for each band.  The binary file carries along
    // these metadata for display purposes(?).
    bool bCalculateStats = CPLFetchBool(papszOptions, "CALCULATE_STATS", true);

    const bool bClassifyData =
        CPLFetchBool(papszOptions, "CLASSIFY_DATA", true);

    // We should have stats if we classify, we'll get them anyway.
    if (bClassifyData && !bCalculateStats)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Ignoring request to not calculate statistics, "
                 "because CLASSIFY_DATA was set to ON");
        bCalculateStats = true;
    }

    pszTemp = CSLFetchNameValueDef(papszOptions, "LINEAR_UNIT", "SET_FROM_SRS");
    int nLinearUnits = 0;
    bool bSetLinearUnits = false;
    if (EQUAL(pszTemp, "SET_FROM_SRS"))
    {
        bSetLinearUnits = true;
    }
    else if (STARTS_WITH_CI(pszTemp, "METER"))
    {
        nLinearUnits = 0;
    }
    else if (EQUAL(pszTemp, "FOOT") || EQUAL(pszTemp, "FEET"))
    {
        nLinearUnits = 1;
    }
    else if (STARTS_WITH_CI(pszTemp, "KILOMETER"))
    {
        nLinearUnits = 2;
    }
    bool bCalculateLatitude = true;
    int nLatitude = 0;
    if (CSLFetchNameValue(papszOptions, "LATITUDE") != nullptr)
    {
        bCalculateLatitude = false;
        nLatitude = atoi(CSLFetchNameValue(papszOptions, "LATITUDE"));
        if (nLatitude > 90 || nLatitude < -90)
        {
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "Invalid value (%d) for LATITUDE.", nLatitude);
            return nullptr;
        }
    }

    // If no latitude is supplied, attempt to extract the central latitude
    // from the image.  It must be set either manually or here, otherwise
    // we fail.
    GDALGeoTransform srcGT;
    poSrcDS->GetGeoTransform(srcGT);
    const OGRSpatialReference *poSrcSRS = poSrcDS->GetSpatialRef();
    double dfLongitude = 0.0;
    double dfLatitude = 0.0;

    const int nYSize = poSrcDS->GetRasterYSize();

    if (!bCalculateLatitude)
    {
        dfLatitude = nLatitude;
    }
    else if (poSrcSRS)
    {
        OGRSpatialReference oDstSRS;
        oDstSRS.importFromEPSG(4269);
        oDstSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        OGRCoordinateTransformation *poCT =
            reinterpret_cast<OGRCoordinateTransformation *>(
                OGRCreateCoordinateTransformation(poSrcSRS, &oDstSRS));
        if (poCT != nullptr)
        {
            dfLatitude = srcGT[3] + srcGT[5] * nYSize / 2;
            const int nErr =
                static_cast<int>(poCT->Transform(1, &dfLongitude, &dfLatitude));
            if (!nErr)
            {
                dfLatitude = 0.0;
                // For the most part, this is an invalid LCP, but it is a
                // changeable value in Flammap/Farsite, etc.  We should
                // probably be strict here all the time.
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Could not calculate latitude from spatial "
                         "reference and LATITUDE was not set.");
                return nullptr;
            }
        }
        OGRCoordinateTransformation::DestroyCT(poCT);
    }
    else
    {
        // See comment above on failure to transform.
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Could not calculate latitude from spatial reference "
                 "and LATITUDE was not set.");
        return nullptr;
    }
    // Set the linear units if the metadata item was not already set, and we
    // have an SRS.
    if (bSetLinearUnits && poSrcSRS)
    {
        const char *pszUnit = poSrcSRS->GetAttrValue("UNIT", 0);
        if (pszUnit == nullptr)
        {
            if (bStrict)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Could not parse linear unit.");
                return nullptr;
            }
            else
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Could not parse linear unit, using meters");
                nLinearUnits = 0;
            }
        }
        else
        {
            CPLDebug("LCP", "Setting linear unit to %s", pszUnit);
            if (EQUAL(pszUnit, "meter") || EQUAL(pszUnit, "metre"))
            {
                nLinearUnits = 0;
            }
            else if (EQUAL(pszUnit, "feet") || EQUAL(pszUnit, "foot"))
            {
                nLinearUnits = 1;
            }
            else if (STARTS_WITH_CI(pszUnit, "kilomet"))
            {
                nLinearUnits = 2;
            }
            else
            {
                if (bStrict)
                    nLinearUnits = 0;
            }
            pszUnit = poSrcSRS->GetAttrValue("UNIT", 1);
            if (pszUnit != nullptr)
            {
                const double dfScale = CPLAtof(pszUnit);
                if (dfScale != 1.0)
                {
                    if (bStrict)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Unit scale is %lf (!=1.0). It is not "
                                 "supported.",
                                 dfScale);
                        return nullptr;
                    }
                    else
                    {
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Unit scale is %lf (!=1.0). It is not "
                                 "supported, ignoring.",
                                 dfScale);
                    }
                }
            }
        }
    }
    else if (bSetLinearUnits)
    {
        // This can be defaulted if it is not a strict creation.
        if (bStrict)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Could not parse linear unit from spatial reference "
                     "and LINEAR_UNIT was not set.");
            return nullptr;
        }
        else
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Could not parse linear unit from spatial reference "
                     "and LINEAR_UNIT was not set, defaulting to meters.");
            nLinearUnits = 0;
        }
    }

    const char *pszDescription = CSLFetchNameValueDef(
        papszOptions, "DESCRIPTION", "LCP file created by GDAL.");

    // Loop through and get the stats for the bands if we need to calculate
    // them.  This probably should be done when we copy the data over to the
    // destination dataset, since we load the values into memory, but this is
    // much simpler code using GDALRasterBand->GetStatistics().  We also may
    // need to classify the data (number of unique values and a list of those
    // values if the number of unique values is > 100.  It is currently unclear
    // how these data are used though, so we will implement that at some point
    // if need be.
    double *padfMin = static_cast<double *>(CPLMalloc(sizeof(double) * nBands));
    double *padfMax = static_cast<double *>(CPLMalloc(sizeof(double) * nBands));

    // Initialize these arrays to zeros
    GInt32 *panFound =
        static_cast<GInt32 *>(VSIMalloc2(sizeof(GInt32), nBands));
    memset(panFound, 0, sizeof(GInt32) * nBands);
    GInt32 *panClasses = static_cast<GInt32 *>(
        VSIMalloc3(sizeof(GInt32), nBands, LCP_MAX_CLASSES));
    memset(panClasses, 0, sizeof(GInt32) * nBands * LCP_MAX_CLASSES);

    if (bCalculateStats)
    {

        for (int i = 0; i < nBands; i++)
        {
            GDALRasterBand *poBand = poSrcDS->GetRasterBand(i + 1);
            double dfDummy = 0.0;
            CPLErr eErr = poBand->GetStatistics(
                FALSE, TRUE, &padfMin[i], &padfMax[i], &dfDummy, &dfDummy);
            if (eErr != CE_None)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Failed to properly "
                         "calculate statistics "
                         "on band %d",
                         i);
                padfMin[i] = 0.0;
                padfMax[i] = 0.0;
            }

            // See comment above.
            if (bClassifyData)
            {
                eErr = ClassifyBandData(poBand, panFound[i],
                                        panClasses + (i * LCP_MAX_CLASSES));
                if (eErr != CE_None)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Failed to classify band data on band %d.", i);
                }
            }
        }
    }

    VSILFILE *fp = VSIFOpenL(pszFilename, "wb");
    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Unable to create lcp file %s.",
                 pszFilename);
        CPLFree(padfMin);
        CPLFree(padfMax);
        CPLFree(panFound);
        CPLFree(panClasses);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Write the header                                                */
    /* -------------------------------------------------------------------- */

    GInt32 nTemp = bHaveCrownFuels ? 21 : 20;
    CPL_LSBPTR32(&nTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&nTemp, 4, 1, fp));
    nTemp = bHaveGroundFuels ? 21 : 20;
    CPL_LSBPTR32(&nTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&nTemp, 4, 1, fp));

    const int nXSize = poSrcDS->GetRasterXSize();
    nTemp = static_cast<GInt32>(dfLatitude + 0.5);
    CPL_LSBPTR32(&nTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&nTemp, 4, 1, fp));
    dfLongitude = srcGT[0] + srcGT[1] * nXSize;
    CPL_LSBPTR64(&dfLongitude);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfLongitude, 8, 1, fp));
    dfLongitude = srcGT[0];
    CPL_LSBPTR64(&dfLongitude);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfLongitude, 8, 1, fp));
    dfLatitude = srcGT[3];
    CPL_LSBPTR64(&dfLatitude);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfLatitude, 8, 1, fp));
    dfLatitude = srcGT[3] + srcGT[5] * nYSize;
    CPL_LSBPTR64(&dfLatitude);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfLatitude, 8, 1, fp));

    // Swap the two classification arrays if we are writing them, and they need
    // to be swapped.
#ifdef CPL_MSB
    if (bClassifyData)
    {
        GDALSwapWords(panFound, 2, nBands, 2);
        GDALSwapWords(panClasses, 2, LCP_MAX_CLASSES, 2);
    }
#endif

    if (bCalculateStats)
    {
        for (int i = 0; i < nBands; i++)
        {
            // If we don't have Crown fuels, but do have Ground fuels, we
            // have to 'fast forward'.
            if (i == 5 && !bHaveCrownFuels && bHaveGroundFuels)
            {
                CPL_IGNORE_RET_VAL(VSIFSeekL(fp, 3340, SEEK_SET));
            }
            nTemp = static_cast<GInt32>(padfMin[i]);
            CPL_LSBPTR32(&nTemp);
            CPL_IGNORE_RET_VAL(VSIFWriteL(&nTemp, 4, 1, fp));
            nTemp = static_cast<GInt32>(padfMax[i]);
            CPL_LSBPTR32(&nTemp);
            CPL_IGNORE_RET_VAL(VSIFWriteL(&nTemp, 4, 1, fp));
            if (bClassifyData)
            {
                // These two arrays were swapped in their entirety above.
                CPL_IGNORE_RET_VAL(VSIFWriteL(panFound + i, 4, 1, fp));
                CPL_IGNORE_RET_VAL(
                    VSIFWriteL(panClasses + (i * LCP_MAX_CLASSES), 4,
                               LCP_MAX_CLASSES, fp));
            }
            else
            {
                nTemp = -1;
                CPL_LSBPTR32(&nTemp);
                CPL_IGNORE_RET_VAL(VSIFWriteL(&nTemp, 4, 1, fp));
                CPL_IGNORE_RET_VAL(
                    VSIFSeekL(fp, 4 * LCP_MAX_CLASSES, SEEK_CUR));
            }
        }
    }
    else
    {
        CPL_IGNORE_RET_VAL(VSIFSeekL(fp, 4164, SEEK_SET));
    }
    CPLFree(padfMin);
    CPLFree(padfMax);
    CPLFree(panFound);
    CPLFree(panClasses);

    // Should be at one of 3 locations, 2104, 3340, or 4164.
    CPLAssert(VSIFTellL(fp) == 2104 || VSIFTellL(fp) == 3340 ||
              VSIFTellL(fp) == 4164);
    CPL_IGNORE_RET_VAL(VSIFSeekL(fp, 4164, SEEK_SET));

    /* Image size */
    nTemp = static_cast<GInt32>(nXSize);
    CPL_LSBPTR32(&nTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&nTemp, 4, 1, fp));
    nTemp = static_cast<GInt32>(nYSize);
    CPL_LSBPTR32(&nTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&nTemp, 4, 1, fp));

    // X and Y boundaries.
    // Max x.
    double dfTemp = srcGT[0] + srcGT[1] * nXSize;
    CPL_LSBPTR64(&dfTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfTemp, 8, 1, fp));
    // Min x.
    dfTemp = srcGT[0];
    CPL_LSBPTR64(&dfTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfTemp, 8, 1, fp));
    // Max y.
    dfTemp = srcGT[3];
    CPL_LSBPTR64(&dfTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfTemp, 8, 1, fp));
    // Min y.
    dfTemp = srcGT[3] + srcGT[5] * nYSize;
    CPL_LSBPTR64(&dfTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfTemp, 8, 1, fp));

    nTemp = nLinearUnits;
    CPL_LSBPTR32(&nTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&nTemp, 4, 1, fp));

    // Resolution.
    // X resolution.
    dfTemp = srcGT[1];
    CPL_LSBPTR64(&dfTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfTemp, 8, 1, fp));
    // Y resolution.
    dfTemp = fabs(srcGT[5]);
    CPL_LSBPTR64(&dfTemp);
    CPL_IGNORE_RET_VAL(VSIFWriteL(&dfTemp, 8, 1, fp));

#ifdef CPL_MSB
    GDALSwapWords(panMetadata, 2, LCP_MAX_BANDS, 2);
#endif
    CPL_IGNORE_RET_VAL(VSIFWriteL(panMetadata, 2, LCP_MAX_BANDS, fp));

    // Write the source filenames.
    char **papszFileList = poSrcDS->GetFileList();
    if (papszFileList != nullptr)
    {
        for (int i = 0; i < nBands; i++)
        {
            if (i == 5 && !bHaveCrownFuels && bHaveGroundFuels)
            {
                CPL_IGNORE_RET_VAL(VSIFSeekL(fp, 6292, SEEK_SET));
            }
            CPL_IGNORE_RET_VAL(
                VSIFWriteL(papszFileList[0], 1,
                           CPLStrnlen(papszFileList[0], LCP_MAX_PATH), fp));
            CPL_IGNORE_RET_VAL(VSIFSeekL(fp, 4244 + (256 * (i + 1)), SEEK_SET));
        }
    }
    // No file list, mem driver, etc.
    else
    {
        CPL_IGNORE_RET_VAL(VSIFSeekL(fp, 6804, SEEK_SET));
    }
    CSLDestroy(papszFileList);
    // Should be at location 5524, 6292 or 6804.
    CPLAssert(VSIFTellL(fp) == 5524 || VSIFTellL(fp) == 6292 ||
              VSIFTellL(fp) == 6804);
    CPL_IGNORE_RET_VAL(VSIFSeekL(fp, 6804, SEEK_SET));

    // Description .
    CPL_IGNORE_RET_VAL(VSIFWriteL(
        pszDescription, 1, CPLStrnlen(pszDescription, LCP_MAX_DESC), fp));

    // Should be at or below location 7316, all done with the header.
    CPLAssert(VSIFTellL(fp) <= 7316);
    CPL_IGNORE_RET_VAL(VSIFSeekL(fp, 7316, SEEK_SET));

    /* -------------------------------------------------------------------- */
    /*      Loop over image, copying image data.                            */
    /* -------------------------------------------------------------------- */

    GInt16 *panScanline = static_cast<GInt16 *>(VSIMalloc3(2, nBands, nXSize));

    if (!pfnProgress(0.0, nullptr, pProgressData))
    {
        CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
        VSIFree(panScanline);
        return nullptr;
    }
    for (int iLine = 0; iLine < nYSize; iLine++)
    {
        for (int iBand = 0; iBand < nBands; iBand++)
        {
            GDALRasterBand *poBand = poSrcDS->GetRasterBand(iBand + 1);
            CPLErr eErr = poBand->RasterIO(
                GF_Read, 0, iLine, nXSize, 1, panScanline + iBand, nXSize, 1,
                GDT_Int16, nBands * 2, static_cast<size_t>(nBands) * nXSize * 2,
                nullptr);
            // Not sure what to do here.
            if (eErr != CE_None)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Error reported in "
                         "RasterIO");
            }
        }
#ifdef CPL_MSB
        GDALSwapWordsEx(panScanline, 2, static_cast<size_t>(nBands) * nXSize,
                        2);
#endif
        CPL_IGNORE_RET_VAL(VSIFWriteL(
            panScanline, 2, static_cast<size_t>(nBands) * nXSize, fp));

        if (!pfnProgress(iLine / static_cast<double>(nYSize), nullptr,
                         pProgressData))
        {
            VSIFree(reinterpret_cast<void *>(panScanline));
            CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
            return nullptr;
        }
    }
    VSIFree(panScanline);
    CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
    if (!pfnProgress(1.0, nullptr, pProgressData))
    {
        return nullptr;
    }

    // Try to write projection file.  *Most* landfire data follows ESRI
    // style projection files, so we use the same code as the AAIGrid driver.
    if (poSrcSRS)
    {
        char *pszESRIProjection = nullptr;
        const char *const apszOptions[] = {"FORMAT=WKT1_ESRI", nullptr};
        poSrcSRS->exportToWkt(&pszESRIProjection, apszOptions);
        if (pszESRIProjection)
        {
            char *const pszDirname =
                CPLStrdup(CPLGetPathSafe(pszFilename).c_str());
            char *const pszBasename =
                CPLStrdup(CPLGetBasenameSafe(pszFilename).c_str());
            char *pszPrjFilename = CPLStrdup(
                CPLFormFilenameSafe(pszDirname, pszBasename, "prj").c_str());
            fp = VSIFOpenL(pszPrjFilename, "wt");
            if (fp != nullptr)
            {
                CPL_IGNORE_RET_VAL(VSIFWriteL(pszESRIProjection, 1,
                                              strlen(pszESRIProjection), fp));
                CPL_IGNORE_RET_VAL(VSIFCloseL(fp));
            }
            else
            {
                CPLError(CE_Failure, CPLE_FileIO, "Unable to create file %s.",
                         pszPrjFilename);
            }
            CPLFree(pszDirname);
            CPLFree(pszBasename);
            CPLFree(pszPrjFilename);
        }
        CPLFree(pszESRIProjection);
    }
    return static_cast<GDALDataset *>(GDALOpen(pszFilename, GA_ReadOnly));
}

/************************************************************************/
/*                         GDALRegister_LCP()                           */
/************************************************************************/

void GDALRegister_LCP()

{
    if (GDALGetDriverByName("LCP") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("LCP");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME,
                              "FARSITE v.4 Landscape File (.lcp)");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "lcp");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/lcp.html");

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES, "Int16");
    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONOPTIONLIST,
        "<CreationOptionList>"
        "   <Option name='ELEVATION_UNIT' type='string-select' "
        "default='METERS' description='Elevation units'>"
        "       <Value>METERS</Value>"
        "       <Value>FEET</Value>"
        "   </Option>"
        "   <Option name='SLOPE_UNIT' type='string-select' default='DEGREES' "
        "description='Slope units'>"
        "       <Value>DEGREES</Value>"
        "       <Value>PERCENT</Value>"
        "   </Option>"
        "   <Option name='ASPECT_UNIT' type='string-select' "
        "default='AZIMUTH_DEGREES'>"
        "       <Value>GRASS_CATEGORIES</Value>"
        "       <Value>AZIMUTH_DEGREES</Value>"
        "       <Value>GRASS_DEGREES</Value>"
        "   </Option>"
        "   <Option name='FUEL_MODEL_OPTION' type='string-select' "
        "default='NO_CUSTOM_AND_NO_FILE'>"
        "       <Value>NO_CUSTOM_AND_NO_FILE</Value>"
        "       <Value>CUSTOM_AND_NO_FILE</Value>"
        "       <Value>NO_CUSTOM_AND_FILE</Value>"
        "       <Value>CUSTOM_AND_FILE</Value>"
        "   </Option>"
        "   <Option name='CANOPY_COV_UNIT' type='string-select' "
        "default='PERCENT'>"
        "       <Value>CATEGORIES</Value>"
        "       <Value>PERCENT</Value>"
        "   </Option>"
        "   <Option name='CANOPY_HT_UNIT' type='string-select' "
        "default='METERS_X_10'>"
        "       <Value>METERS</Value>"
        "       <Value>FEET</Value>"
        "       <Value>METERS_X_10</Value>"
        "       <Value>FEET_X_10</Value>"
        "   </Option>"
        "   <Option name='CBH_UNIT' type='string-select' default='METERS_X_10'>"
        "       <Value>METERS</Value>"
        "       <Value>FEET</Value>"
        "       <Value>METERS_X_10</Value>"
        "       <Value>FEET_X_10</Value>"
        "   </Option>"
        "   <Option name='CBD_UNIT' type='string-select' "
        "default='KG_PER_CUBIC_METER_X_100'>"
        "       <Value>KG_PER_CUBIC_METER</Value>"
        "       <Value>POUND_PER_CUBIC_FOOT</Value>"
        "       <Value>KG_PER_CUBIC_METER_X_100</Value>"
        "       <Value>POUND_PER_CUBIC_FOOT_X_1000</Value>"
        "   </Option>"
        "   <Option name='DUFF_UNIT' type='string-select' "
        "default='MG_PER_HECTARE_X_10'>"
        "       <Value>MG_PER_HECTARE_X_10</Value>"
        "       <Value>TONS_PER_ACRE_X_10</Value>"
        "   </Option>"
        // Kyle does not think we need to override this, but maybe?
        // "   <Option name='CWD_OPTION' type='boolean' default='FALSE'
        // description='Override logic for setting the coarse woody presence'/>"
        // */
        "   <Option name='CALCULATE_STATS' type='boolean' default='YES' "
        "description='Write the stats to the lcp'/>"
        "   <Option name='CLASSIFY_DATA' type='boolean' default='YES' "
        "description='Write the stats to the lcp'/>"
        "   <Option name='LINEAR_UNIT' type='string-select' "
        "default='SET_FROM_SRS' description='Set the linear units in the lcp'>"
        "       <Value>SET_FROM_SRS</Value>"
        "       <Value>METER</Value>"
        "       <Value>FOOT</Value>"
        "       <Value>KILOMETER</Value>"
        "   </Option>"
        "   <Option name='LATITUDE' type='int' default='' description='Set the "
        "latitude for the dataset, this overrides the driver trying to set it "
        "programmatically in EPSG:4269'/>"
        "   <Option name='DESCRIPTION' type='string' default='LCP file created "
        "by GDAL' description='A short description of the lcp file'/>"
        "</CreationOptionList>");

    poDriver->pfnOpen = LCPDataset::Open;
    poDriver->pfnCreateCopy = LCPDataset::CreateCopy;
    poDriver->pfnIdentify = LCPDataset::Identify;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
