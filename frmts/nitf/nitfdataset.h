/******************************************************************************
 *
 * Project:  NITF Read/Write Translator
 * Purpose:  GDALDataset/GDALRasterBand declarations.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2002, Frank Warmerdam
 * Copyright (c) 2011-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * Portions Copyright (c) Her majesty the Queen in right of Canada as
 * represented by the Minister of National Defence, 2006.
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef NITF_DATASET_H_INCLUDED
#define NITF_DATASET_H_INCLUDED

#include "gdal_pam.h"
#include "nitflib.h"
#include "ogr_spatialref.h"
#include "gdal_proxy.h"

#include <array>
#include <map>

CPLErr NITFSetColorInterpretation(NITFImage *psImage, int nBand,
                                  GDALColorInterp eInterp);

/* Unused in normal builds. Caller code in nitfdataset.cpp is protected by
 * #ifdef ESRI_BUILD */
#ifdef ESRI_BUILD
/* -------------------------------------------------------------------- */
/*      Functions in nitf_gcprpc.cpp.                                   */
/* -------------------------------------------------------------------- */

void NITFDensifyGCPs(GDAL_GCP **psGCPs, int *pnGCPCount);
void NITFUpdateGCPsWithRPC(NITFRPC00BInfo *psRPCInfo, GDAL_GCP *psGCPs,
                           int *pnGCPCount);
#endif

/************************************************************************/
/* ==================================================================== */
/*                              NITFDataset                             */
/* ==================================================================== */
/************************************************************************/

class NITFRasterBand;
class NITFWrapperRasterBand;

class NITFDataset final : public GDALPamDataset
{
    friend class NITFRasterBand;
    friend class NITFWrapperRasterBand;
    friend class NITFComplexRasterBand;

    NITFFile *psFile = nullptr;
    NITFImage *psImage = nullptr;

    std::unique_ptr<GDALDataset> poJ2KDataset{};
    int bJP2Writing = false;
    vsi_l_offset m_nImageOffset = 0;
    int m_nIMIndex = 0;
    int m_nImageCount = 0;
    vsi_l_offset m_nICOffset = 0;
    bool m_bHasComplexRasterBand = false;

    std::unique_ptr<GDALDataset> poJPEGDataset{};

    int bGotGeoTransform = false;
    GDALGeoTransform m_gt{};

    OGRSpatialReference m_oSRS{};

    int nGCPCount = 0;
    GDAL_GCP *pasGCPList = nullptr;
    OGRSpatialReference m_oGCPSRS{};

    GDALMultiDomainMetadata oSpecialMD{};

#ifdef ESRI_BUILD
    void InitializeNITFDESMetadata();
    void InitializeNITFTREs();
#endif
    bool InitializeNITFDESs(bool bValidate);
    void InitializeNITFMetadata();
    void InitializeCGMMetadata();
    void InitializeTextMetadata();
    bool InitializeTREMetadata(bool bValidate);
    void InitializeImageStructureMetadata();

    GIntBig *panJPEGBlockOffset = nullptr;
    GByte *pabyJPEGBlock = nullptr;
    int nQLevel = 0;

    int ScanJPEGQLevel(GUIntBig *pnDataStart, bool *pbError);
    CPLErr ScanJPEGBlocks();
    CPLErr ReadJPEGBlock(int, int);
    void CheckGeoSDEInfo();
    char **AddFile(char **papszFileList, const char *EXTENSION,
                   const char *extension);

    int nIMIndex = 0;
    CPLString osNITFFilename{};

    CPLString osRSetVRT{};
    int CheckForRSets(const char *pszFilename, char **papszSiblingFiles);

    char **papszTextMDToWrite = nullptr;
    char **papszCgmMDToWrite = nullptr;
    CPLStringList aosCreationOptions{};

    int bInLoadXML = false;

    CPLString m_osRPCTXTFilename{};

    int bExposeUnderlyingJPEGDatasetOverviews = false;

    int ExposeUnderlyingJPEGDatasetOverviews() const
    {
        return bExposeUnderlyingJPEGDatasetOverviews;
    }

    bool Validate();

    CPL_DISALLOW_COPY_ASSIGN(NITFDataset)

  protected:
    virtual int CloseDependentDatasets() override;

  public:
    NITFDataset();
    virtual ~NITFDataset();

    virtual CPLErr AdviseRead(int nXOff, int nYOff, int nXSize, int nYSize,
                              int nBufXSize, int nBufYSize, GDALDataType eDT,
                              int nBandCount, int *panBandList,
                              char **papszOptions) override;

    virtual CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                             GDALDataType, int, BANDMAP_TYPE,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GSpacing nBandSpace,
                             GDALRasterIOExtraArg *psExtraArg) override;

    const OGRSpatialReference *GetSpatialRef() const override;
    CPLErr SetSpatialRef(const OGRSpatialReference *poSRS) override;

    virtual CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    virtual CPLErr SetGeoTransform(const GDALGeoTransform &gt) override;
    CPLErr SetGCPs(int nGCPCountIn, const GDAL_GCP *pasGCPListIn,
                   const OGRSpatialReference *poSRS) override;

    virtual int GetGCPCount() override;
    const OGRSpatialReference *GetGCPSpatialRef() const override;
    virtual const GDAL_GCP *GetGCPs() override;
    virtual char **GetFileList() override;

    virtual char **GetMetadataDomainList() override;
    virtual char **GetMetadata(const char *pszDomain = "") override;
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain = "") override;
    virtual CPLErr FlushCache(bool bAtClosing) override;
    virtual CPLErr IBuildOverviews(const char *, int, const int *, int,
                                   const int *, GDALProgressFunc, void *,
                                   CSLConstList papszOptions) override;

    static NITFDataset *OpenInternal(GDALOpenInfo *,
                                     GDALDataset *poWritableJ2KDataset,
                                     bool bOpenForCreate, int nIMIndex);
    static GDALDataset *Open(GDALOpenInfo *);
    static GDALDataset *NITFCreateCopy(const char *pszFilename,
                                       GDALDataset *poSrcDS, int bStrict,
                                       char **papszOptions,
                                       GDALProgressFunc pfnProgress,
                                       void *pProgressData);
    static GDALDataset *NITFDatasetCreate(const char *pszFilename, int nXSize,
                                          int nYSize, int nBands,
                                          GDALDataType eType,
                                          char **papszOptions);
};

/************************************************************************/
/* ==================================================================== */
/*                            NITFRasterBand                             */
/* ==================================================================== */
/************************************************************************/

class NITFRasterBand CPL_NON_FINAL : public GDALPamRasterBand
{
    friend class NITFDataset;

    NITFImage *psImage = nullptr;

    GDALColorTable *poColorTable = nullptr;

    GByte *pUnpackData = nullptr;

    int bScanlineAccess = false;

    CPL_DISALLOW_COPY_ASSIGN(NITFRasterBand)

  public:
    NITFRasterBand(NITFDataset *, int);
    virtual ~NITFRasterBand();

    virtual CPLErr IReadBlock(int, int, void *) override;
    virtual CPLErr IWriteBlock(int, int, void *) override;

    virtual GDALColorInterp GetColorInterpretation() override;
    virtual CPLErr SetColorInterpretation(GDALColorInterp) override;
    virtual GDALColorTable *GetColorTable() override;
    virtual CPLErr SetColorTable(GDALColorTable *) override;
    virtual double GetNoDataValue(int *pbSuccess = nullptr) override;

    void Unpack(GByte *pData);
};

/************************************************************************/
/* ==================================================================== */
/*                        NITFProxyPamRasterBand                        */
/* ==================================================================== */
/************************************************************************/

/* This class is potentially of general interest and could be moved to
 * gdal_proxy.h */
/* We don't proxy all methods. Generally speaking, the getters go to PAM first
 * and */
/* then to the underlying band if no value exist in PAM. The setters aren't */
/* overridden, so they go to PAM */

class NITFProxyPamRasterBand CPL_NON_FINAL : public GDALPamRasterBand
{
  private:
    std::map<CPLString, char **> oMDMap{};

  protected:
    virtual GDALRasterBand *RefUnderlyingRasterBand() = 0;
    virtual void
    UnrefUnderlyingRasterBand(GDALRasterBand *poUnderlyingRasterBand);

    virtual CPLErr IReadBlock(int, int, void *) override;
    virtual CPLErr IWriteBlock(int, int, void *) override;
    virtual CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                             GDALDataType, GSpacing nPixelSpace,
                             GSpacing nLineSpace,
                             GDALRasterIOExtraArg *psExtraArg) override;

  public:
    virtual ~NITFProxyPamRasterBand();

    virtual char **GetMetadata(const char *pszDomain = "") override;
    /*virtual CPLErr      SetMetadata( char ** papszMetadata,
                                    const char * pszDomain = ""  );*/
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain = "") override;
    /*virtual CPLErr      SetMetadataItem( const char * pszName,
                                        const char * pszValue,
                                        const char * pszDomain = "" );*/
    virtual CPLErr FlushCache(bool bAtClosing) override;
    /*virtual char **GetCategoryNames();*/
    virtual double GetNoDataValue(int *pbSuccess = nullptr) override;
    virtual double GetMinimum(int *pbSuccess = nullptr) override;
    virtual double GetMaximum(int *pbSuccess = nullptr) override;
    /*virtual double GetOffset( int *pbSuccess = NULL );
    virtual double GetScale( int *pbSuccess = NULL );*/
    /*virtual const char *GetUnitType();*/
    virtual GDALColorInterp GetColorInterpretation() override;
    virtual GDALColorTable *GetColorTable() override;
    virtual CPLErr Fill(double dfRealValue,
                        double dfImaginaryValue = 0) override;

    /*
    virtual CPLErr SetCategoryNames( char ** );
    virtual CPLErr SetNoDataValue( double );
    virtual CPLErr SetColorTable( GDALColorTable * );
    virtual CPLErr SetColorInterpretation( GDALColorInterp );
    virtual CPLErr SetOffset( double );
    virtual CPLErr SetScale( double );
    virtual CPLErr SetUnitType( const char * );
    */

    virtual CPLErr GetStatistics(int bApproxOK, int bForce, double *pdfMin,
                                 double *pdfMax, double *pdfMean,
                                 double *padfStdDev) override;
    virtual CPLErr ComputeStatistics(int bApproxOK, double *pdfMin,
                                     double *pdfMax, double *pdfMean,
                                     double *pdfStdDev, GDALProgressFunc,
                                     void *pProgressData) override;
    /*virtual CPLErr SetStatistics( double dfMin, double dfMax,
                                double dfMean, double dfStdDev );*/
    virtual CPLErr ComputeRasterMinMax(int, double *) override;

    virtual int HasArbitraryOverviews() override;
    virtual int GetOverviewCount() override;
    virtual GDALRasterBand *GetOverview(int) override;
    virtual GDALRasterBand *GetRasterSampleOverview(GUIntBig) override;
    virtual CPLErr BuildOverviews(const char *, int, const int *,
                                  GDALProgressFunc, void *,
                                  CSLConstList papszOptions) override;

    virtual CPLErr AdviseRead(int nXOff, int nYOff, int nXSize, int nYSize,
                              int nBufXSize, int nBufYSize, GDALDataType eDT,
                              char **papszOptions) override;

    /*virtual CPLErr  GetHistogram( double dfMin, double dfMax,
                        int nBuckets, GUIntBig * panHistogram,
                        int bIncludeOutOfRange, int bApproxOK,
                        GDALProgressFunc, void *pProgressData );

    virtual CPLErr GetDefaultHistogram( double *pdfMin, double *pdfMax,
                                        int *pnBuckets, GUIntBig **
    ppanHistogram, int bForce, GDALProgressFunc, void *pProgressData); virtual
    CPLErr SetDefaultHistogram( double dfMin, double dfMax, int nBuckets,
    GUIntBig *panHistogram );*/

    /*virtual const GDALRasterAttributeTable *GetDefaultRAT();
    virtual CPLErr SetDefaultRAT( const GDALRasterAttributeTable * );*/

    virtual GDALRasterBand *GetMaskBand() override;
    virtual int GetMaskFlags() override;
    virtual CPLErr CreateMaskBand(int nFlags) override;
};

/************************************************************************/
/* ==================================================================== */
/*                       NITFWrapperRasterBand                          */
/* ==================================================================== */
/************************************************************************/

/* This class is used to wrap bands from JPEG or JPEG2000 datasets in */
/* bands of the NITF dataset. Previously a trick was applied in the */
/* relevant drivers to define a SetColorInterpretation() method and */
/* to make sure they keep the proper pointer to their "natural" dataset */
/* This trick is no longer necessary with the NITFWrapperRasterBand */
/* We just override the few specific methods where we want that */
/* the NITFWrapperRasterBand behavior differs from the JPEG/JPEG2000 one */

class NITFWrapperRasterBand final : public NITFProxyPamRasterBand
{
    GDALRasterBand *const poBaseBand;
    GDALColorTable *poColorTable = nullptr;
    GDALColorInterp eInterp = GCI_Undefined;
    const bool bIsJPEG;

    CPL_DISALLOW_COPY_ASSIGN(NITFWrapperRasterBand)

  protected:
    /* Pure virtual method of the NITFProxyPamRasterBand */
    virtual GDALRasterBand *RefUnderlyingRasterBand() override;

  public:
    NITFWrapperRasterBand(NITFDataset *poDS, GDALRasterBand *poBaseBand,
                          int nBand);
    virtual ~NITFWrapperRasterBand();

    /* Methods from GDALRasterBand we want to override */
    virtual GDALColorInterp GetColorInterpretation() override;
    virtual CPLErr SetColorInterpretation(GDALColorInterp) override;

    virtual GDALColorTable *GetColorTable() override;

    virtual int GetOverviewCount() override;
    virtual GDALRasterBand *GetOverview(int) override;

    /* Specific method */
    void SetColorTableFromNITFBandInfo();
};

/************************************************************************/
/* ==================================================================== */
/*                        NITFComplexRasterBand                         */
/* ==================================================================== */
/************************************************************************/

/* This class is used to wrap 2 bands (I and Q) as a complex raster band */
class NITFComplexRasterBand final : public NITFRasterBand
{
    std::unique_ptr<NITFDataset> poIntermediateDS{};
    std::array<int, 2> anBandMap = {0, 0};
    GDALDataType underlyingDataType = GDT_Unknown;
    int complexDataTypeSize = 0;
    int underlyingDataTypeSize = 0;

  private:
    CPLErr IBlockIO(int nBlockXOff, int nBlockYOff, void *pImage,
                    GDALRWFlag rwFlag);

  public:
    NITFComplexRasterBand(NITFDataset *poDSIn, GDALRasterBand *poBandI,
                          GDALRasterBand *poBandQ, int nIBand, int nQBand);

    CPLErr IReadBlock(int, int, void *) override;
    CPLErr IWriteBlock(int, int, void *) override;
};

#endif /* NITF_DATASET_H_INCLUDED */
