add_gdal_driver(TARGET gdal_TIL SOURCES tildataset.cpp
  PLUGIN_CAPABLE
  NO_DEPS
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
gdal_standard_includes(gdal_TIL)
target_include_directories(gdal_TIL PRIVATE ${GDAL_RASTER_FORMAT_SOURCE_DIR}/vrt)
