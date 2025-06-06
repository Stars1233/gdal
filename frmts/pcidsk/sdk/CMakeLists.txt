add_library(
  pcidsk_sdk OBJECT
  blockdir/asciitiledir.cpp
  blockdir/asciitilelayer.cpp
  blockdir/binarytiledir.cpp
  blockdir/binarytilelayer.cpp
  blockdir/blockdir.cpp
  blockdir/blockfile.cpp
  blockdir/blocklayer.cpp
  blockdir/blocktiledir.cpp
  blockdir/blocktilelayer.cpp
  channel/cbandinterleavedchannel.cpp
  channel/cexternalchannel.cpp
  channel/cpcidskchannel.cpp
  channel/cpixelinterleavedchannel.cpp
  channel/ctiledchannel.cpp
  core/clinksegment.cpp
  core/cpcidskblockfile.cpp
  core/cpcidskfile.cpp
  core/edb_pcidsk.cpp
  core/libjpeg_io.cpp
  core/metadataset_p.cpp
  core/pcidsk_pubutils.cpp
  core/pcidsk_raster.cpp
  core/pcidsk_scanint.cpp
  core/pcidsk_utils.cpp
  core/pcidskbuffer.cpp
  core/pcidskcreate.cpp
  core/pcidskexception.cpp
  core/pcidskinterfaces.cpp
  core/pcidskopen.cpp
  core/pcidsk_mutex.cpp
  segment/cpcidsk_array.cpp
  segment/cpcidsk_tex.cpp
  segment/cpcidskbinarysegment.cpp
  segment/cpcidskbitmap.cpp
  segment/cpcidskblut.cpp
  segment/cpcidskpct.cpp
  segment/cpcidskephemerissegment.cpp
  segment/cpcidskgcp2segment.cpp
  segment/cpcidskgeoref.cpp
  segment/cpcidsklut.cpp
  segment/cpcidskbpct.cpp
  segment/cpcidskpolymodel.cpp
  segment/cpcidskrpcmodel.cpp
  segment/cpcidsksegment.cpp
  segment/cpcidsktoutinmodel.cpp
  segment/cpcidskvectorsegment.cpp
  segment/cpcidskvectorsegment_consistencycheck.cpp
  segment/metadatasegment_p.cpp
  segment/systiledir.cpp
  segment/vecsegdataindex.cpp
  segment/vecsegheader.cpp)

if(PCIDSK_INTERNAL)
  target_compile_definitions(pcidsk_sdk PRIVATE -DPCIDSK_INTERNAL)
else()
#if (WIN32)
#  target_sources(pcidsk_sdk PRIVATE port/io_win32.cpp port/win32_mutex.cpp)
#else ()
#  target_sources(pcidsk_sdk PRIVATE port/io_stdio.cpp port/pthread_mutex.cpp)
#endif ()
endif()

is_plugin(RES gdal_PCIDSK)
if (RES)
  target_sources(gdal_PCIDSK PRIVATE $<TARGET_OBJECTS:pcidsk_sdk>)
else ()
  target_sources(${GDAL_LIB_TARGET_NAME} PRIVATE $<TARGET_OBJECTS:pcidsk_sdk>)
endif ()

target_include_directories(pcidsk_sdk PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} $<TARGET_PROPERTY:cpl,SOURCE_DIR>
                                              $<TARGET_PROPERTY:cpl,BINARY_DIR>)
set_property(TARGET pcidsk_sdk PROPERTY POSITION_INDEPENDENT_CODE ${GDAL_OBJECT_LIBRARIES_POSITION_INDEPENDENT_CODE})

if (GDAL_USE_JPEG OR GDAL_USE_JPEG_INTERNAL)
    target_compile_definitions(pcidsk_sdk PRIVATE -DHAVE_LIBJPEG)
    if (GDAL_USE_JPEG_INTERNAL)
      gdal_add_vendored_lib(pcidsk_sdk libjpeg)
    else ()
      target_include_directories(pcidsk_sdk PRIVATE ${JPEG_INCLUDE_DIRS})
    endif ()
endif()

if (NOT TARGET PCIDSK::PCIDSK)
  add_library(PCIDSK::PCIDSK UNKNOWN IMPORTED)
  set_target_properties(PCIDSK::PCIDSK PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${CMAKE_CURRENT_SOURCE_DIR})
endif ()

target_link_libraries(gdal_PCIDSK INTERFACE pcidsk_sdk)
