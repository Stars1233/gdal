add_gdal_driver(
  TARGET ogr_SOSI
  SOURCES ogr_sosi.h
          ogrsosidatatypes.h
          ogrsosidatasource.cpp
          ogrsosidatatypes.cpp
          ogrsosidriver.cpp
          ogrsosilayer.cpp
  CORE_SOURCES
          ogrsosidrivercore.cpp
  PLUGIN_CAPABLE
  NO_SHARED_SYMBOL_WITH_CORE
  NO_CXX_WFLAGS_EFFCXX
  NO_WFLAG_OLD_STYLE_CAST
)
if(NOT TARGET ogr_SOSI)
    return()
endif()

gdal_standard_includes(ogr_SOSI)
target_include_directories(ogr_SOSI PRIVATE $<TARGET_PROPERTY:ogrsf_generic,SOURCE_DIR>)
gdal_target_link_libraries(ogr_SOSI PRIVATE FYBA::FYBA FYBA::FYGM FYBA::FYUT)

if (WIN32)
  target_sources(ogr_SOSI PRIVATE fyba_melding.cpp)
else ()
  target_compile_definitions(ogr_SOSI PRIVATE -DLINUX)
endif ()
