#!/usr/bin/env pytest
# -*- coding: utf-8 -*-
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test ENVI format driver.
# Author:   Frank Warmerdam <warmerdam@pobox.com>
#
# See also: gcore/envi_read.py for a driver focused on raster data types.
#
###############################################################################
# Copyright (c) 2007, Frank Warmerdam <warmerdam@pobox.com>
# Copyright (c) 2009-2012, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import os
import struct

import gdaltest
import pytest

from osgeo import gdal, osr

pytestmark = pytest.mark.require_driver("ENVI")

###############################################################################
# Perform simple read test.


def test_envi_1():

    tst = gdaltest.GDALTest("envi", "envi/aea.dat", 1, 14823)

    prj = """PROJCS["unnamed",
    GEOGCS["Ellipse Based",
        DATUM["Ellipse Based",
            SPHEROID["Unnamed",6378206.4,294.9786982139109]],
        PRIMEM["Greenwich",0],
        UNIT["degree",0.0174532925199433]],
    PROJECTION["Albers_Conic_Equal_Area"],
    PARAMETER["standard_parallel_1",29.5],
    PARAMETER["standard_parallel_2",45.5],
    PARAMETER["latitude_of_center",23],
    PARAMETER["longitude_of_center",-96],
    PARAMETER["false_easting",0],
    PARAMETER["false_northing",0],
    UNIT["Meter",1]]"""

    tst.testOpen(
        check_prj=prj, check_gt=(-936408.178, 28.5, 0.0, 2423902.344, 0.0, -28.5)
    )


###############################################################################
# Verify this can be exported losslessly.


def test_envi_2():

    tst = gdaltest.GDALTest("envi", "envi/aea.dat", 1, 14823)
    tst.testCreateCopy(check_gt=1)


###############################################################################
# Try the Create interface with an RGB image.


def test_envi_3():

    tst = gdaltest.GDALTest("envi", "rgbsmall.tif", 2, 21053)
    tst.testCreate()


###############################################################################
# Test LCC Projection.


def test_envi_4():

    tst = gdaltest.GDALTest("envi", "envi/aea.dat", 1, 24)

    prj = """PROJCS["unnamed",
    GEOGCS["NAD83",
        DATUM["North_American_Datum_1983",
            SPHEROID["GRS 1980",6378137,298.257222101],
            TOWGS84[0,0,0,0,0,0,0]],
        PRIMEM["Greenwich",0],
        UNIT["degree",0.0174532925199433]],
    PROJECTION["Lambert_Conformal_Conic_2SP"],
    PARAMETER["standard_parallel_1",33.90363402775256],
    PARAMETER["standard_parallel_2",33.62529002776137],
    PARAMETER["latitude_of_origin",33.76446202775696],
    PARAMETER["central_meridian",-117.4745428888127],
    PARAMETER["false_easting",20000],
    PARAMETER["false_northing",30000],
    UNIT["Meter",1]]"""

    tst.testSetProjection(prj=prj)


###############################################################################
# Test TM Projection.


def test_envi_5():

    tst = gdaltest.GDALTest("envi", "envi/aea.dat", 1, 24)
    prj = """PROJCS["unnamed",
    GEOGCS["GCS_unnamed",
        DATUM["D_unnamed",
            SPHEROID["Airy 1830",6377563.396,299.3249646,
                AUTHORITY["EPSG","7001"]]],
        PRIMEM["Greenwich",0],
        UNIT["degree",0.01745329251994328]],
    PROJECTION["Transverse_Mercator"],
    PARAMETER["latitude_of_origin",49],
    PARAMETER["central_meridian",-2],
    PARAMETER["scale_factor",0.9996012717],
    PARAMETER["false_easting",400000],
    PARAMETER["false_northing",-100000],
    UNIT["metre",1,
        AUTHORITY["EPSG","9001"]],
    AXIS["Easting",EAST],
    AXIS["Northing",NORTH]]"""

    tst.testSetProjection(prj=prj)


###############################################################################
# Test LAEA Projection.


def test_envi_6():

    gdaltest.envi_tst = gdaltest.GDALTest("envi", "envi/aea.dat", 1, 24)

    prj = """PROJCS["unnamed",
    GEOGCS["Unknown datum based upon the Authalic Sphere",
        DATUM["D_Ellipse_Based",
            SPHEROID["Sphere",6370997,0]],
        PRIMEM["Greenwich",0],
        UNIT["Degree",0.0174532925199433]],
    PROJECTION["Lambert_Azimuthal_Equal_Area"],
    PARAMETER["latitude_of_center",33.764462027757],
    PARAMETER["longitude_of_center",-117.474542888813],
    PARAMETER["false_easting",0],
    PARAMETER["false_northing",0],
    UNIT["metre",1,
        AUTHORITY["EPSG","9001"]],
    AXIS["Easting",EAST],
    AXIS["Northing",NORTH]]"""

    gdaltest.envi_tst.testSetProjection(prj=prj)


###############################################################################
# Verify VSIF*L capacity


def test_envi_7():

    tst = gdaltest.GDALTest("envi", "envi/aea.dat", 1, 14823)
    tst.testCreateCopy(check_gt=1, vsimem=1)


###############################################################################
# Test fix for #3751


def test_envi_8():

    ds = gdal.GetDriverByName("ENVI").Create("/vsimem/foo.bsq", 10, 10, 1)
    set_gt = (50000, 1, 0, 4500000, 0, -1)
    ds.SetGeoTransform(set_gt)
    got_gt = ds.GetGeoTransform()
    assert set_gt == got_gt, "did not get expected geotransform"
    ds = None

    gdal.GetDriverByName("ENVI").Delete("/vsimem/foo.bsq")


###############################################################################
# Verify reading a compressed file


def test_envi_9():

    tst = gdaltest.GDALTest("envi", "envi/aea_compressed.dat", 1, 14823)
    tst.testCreateCopy(check_gt=1)


###############################################################################
# Test RPC reading and writing


def test_envi_10():

    src_ds = gdal.Open("data/envi/envirpc.img")
    out_ds = gdal.GetDriverByName("ENVI").CreateCopy("/vsimem/envirpc.img", src_ds)
    src_ds = None
    del out_ds

    gdal.Unlink("/vsimem/envirpc.img.aux.xml")

    ds = gdal.Open("/vsimem/envirpc.img")
    md = ds.GetMetadata("RPC")
    ds = None

    gdal.GetDriverByName("ENVI").Delete("/vsimem/envirpc.img")

    assert md["HEIGHT_OFF"] == "3355"


###############################################################################
# Check .sta reading


def test_envi_11():

    ds = gdal.Open("data/envi/envistat")
    val = ds.GetRasterBand(1).GetStatistics(0, 0)
    ds = None

    assert val == [1.0, 3.0, 2.0, 0.5], "bad stats"


###############################################################################
# Test category names reading and writing


def test_envi_12():

    src_ds = gdal.Open("data/envi/testenviclasses")
    out_ds = gdal.GetDriverByName("ENVI").CreateCopy("/vsimem/testenviclasses", src_ds)
    src_ds = None
    del out_ds

    gdal.Unlink("/vsimem/testenviclasses.aux.xml")

    ds = gdal.Open("/vsimem/testenviclasses")
    category = ds.GetRasterBand(1).GetCategoryNames()
    ct = ds.GetRasterBand(1).GetColorTable()

    assert category == ["Black", "White"], "bad category names"

    assert ct.GetCount() == 2, "bad color entry count"

    assert ct.GetColorEntry(0) == (0, 0, 0, 255), "bad color entry"

    ds = None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/testenviclasses")


###############################################################################
# Test writing of metadata from the ENVI metadata domain and read it back (#4957)


def test_envi_13():

    ds = gdal.GetDriverByName("ENVI").Create("/vsimem/envi_13.dat", 1, 1)
    ds.SetMetadata(["lines=100", "sensor_type=Landsat TM", "foo"], "ENVI")
    ds = None

    gdal.Unlink("/vsimem/envi_13.dat.aux.xml")

    ds = gdal.Open("/vsimem/envi_13.dat")
    lines = ds.RasterYSize
    val = ds.GetMetadataItem("sensor_type", "ENVI")
    ds = None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/envi_13.dat")

    assert lines == 1

    assert val == "Landsat TM"


###############################################################################
# Test that the image file is at the expected size on closing (#6662)


def test_envi_14():

    gdal.GetDriverByName("ENVI").Create("/vsimem/envi_14.dat", 3, 4, 5, gdal.GDT_Int16)

    if os.path.exists("/vsimem/envi_14.dat.aux.xml"):
        gdal.Unlink("/vsimem/envi_14.dat.aux.xml")

    assert gdal.VSIStatL("/vsimem/envi_14.dat").size == 3 * 4 * 5 * 2

    gdal.GetDriverByName("ENVI").Delete("/vsimem/envi_14.dat")


###############################################################################
# Test reading and writing geotransform matrix with rotation


def test_envi_15():

    src_ds = gdal.Open("data/envi/rotation.img")
    got_gt = src_ds.GetGeoTransform()
    expected_gt = [
        736600.089,
        1.0981889363046606,
        -2.4665727356350224,
        4078126.75,
        -2.4665727356350224,
        -1.0981889363046606,
    ]
    assert (
        max([abs((got_gt[i] - expected_gt[i]) / expected_gt[i]) for i in range(6)])
        <= 1e-5
    ), "did not get expected geotransform"

    gdal.GetDriverByName("ENVI").CreateCopy("/vsimem/envi_15.dat", src_ds)

    ds = gdal.Open("/vsimem/envi_15.dat")
    got_gt = ds.GetGeoTransform()
    assert (
        max([abs((got_gt[i] - expected_gt[i]) / expected_gt[i]) for i in range(6)])
        <= 1e-5
    ), "did not get expected geotransform"
    ds = None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/envi_15.dat")


###############################################################################
# Test reading a truncated ENVI dataset (see #915)


def test_envi_truncated():

    gdal.GetDriverByName("ENVI").CreateCopy(
        "/vsimem/envi_truncated.dat", gdal.Open("data/byte.tif")
    )

    f = gdal.VSIFOpenL("/vsimem/envi_truncated.dat", "rb+")
    gdal.VSIFTruncateL(f, int(20 * 20 / 2))
    gdal.VSIFCloseL(f)

    with gdaltest.config_option("RAW_CHECK_FILE_SIZE", "YES"):
        ds = gdal.Open("/vsimem/envi_truncated.dat")
    cs = ds.GetRasterBand(1).Checksum()
    ds = None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/envi_truncated.dat")

    assert cs == 2315


###############################################################################
# Test writing & reading GCPs (#1528)


def test_envi_gcp():

    filename = "/vsimem/test_envi_gcp.dat"
    ds = gdal.GetDriverByName("ENVI").Create(filename, 1, 1)
    gcp = gdal.GCP()
    gcp.GCPPixel = 1
    gcp.GCPLine = 2
    gcp.GCPX = 3
    gcp.GCPY = 4
    ds.SetGCPs([gcp], None)
    ds = None
    gdal.Unlink(filename + ".aux.xml")

    ds = gdal.Open(filename)
    assert ds.GetGCPCount() == 1
    gcps = ds.GetGCPs()
    assert len(gcps) == 1
    gcp = gcps[0]
    ds = None
    assert gcp.GCPPixel == 1
    assert gcp.GCPLine == 2
    assert gcp.GCPX == 3
    assert gcp.GCPY == 4

    gdal.GetDriverByName("ENVI").Delete(filename)


###############################################################################
# Test updating big endian ordered (#1796)


def test_envi_bigendian():

    ds = gdal.Open("data/envi/uint16_envi_bigendian.dat")
    assert ds.GetRasterBand(1).Checksum() == 4672
    ds = None

    for ext in ("dat", "hdr"):
        filename = "uint16_envi_bigendian." + ext
        gdal.FileFromMemBuffer(
            "/vsimem/" + filename, open("data/envi/" + filename, "rb").read()
        )

    filename = "/vsimem/uint16_envi_bigendian.dat"
    ds = gdal.Open(filename, gdal.GA_Update)
    ds.SetGeoTransform([0, 2, 0, 0, 0, -2])
    ds = None

    ds = gdal.Open(filename)
    assert ds.GetRasterBand(1).Checksum() == 4672
    ds = None

    gdal.GetDriverByName("ENVI").Delete(filename)


###############################################################################
# Test different interleaving


@pytest.mark.parametrize(
    "filename_suffix, expected_interleave",
    [("bip", "PIXEL"), ("bil", "LINE"), ("bsq", "BAND")],
)
def test_envi_interleaving(filename_suffix, expected_interleave):

    filename = f"data/envi/envi_rgbsmall_{filename_suffix}.img"
    ds = gdal.Open(filename)
    assert ds, filename
    assert ds.GetMetadataItem("INTERLEAVE", "IMAGE_STRUCTURE") == expected_interleave
    assert ds.GetRasterBand(1).Checksum() == 20718, filename
    assert ds.GetRasterBand(2).Checksum() == 20669, filename
    assert ds.GetRasterBand(3).Checksum() == 20895, filename
    ds = None


###############################################################################
# Test nodata


def test_envi_nodata():

    filename = "/vsimem/test_envi_nodata.dat"
    ds = gdal.GetDriverByName("ENVI").Create(filename, 1, 1)
    ds.GetRasterBand(1).SetNoDataValue(1)
    ds = None

    gdal.Unlink(filename + ".aux.xml")

    ds = gdal.Open(filename)
    assert ds.GetRasterBand(1).GetNoDataValue() == 1.0
    ds = None

    gdal.GetDriverByName("ENVI").Delete(filename)


###############################################################################
# Test reading and writing geotransform matrix with rotation = 180


def test_envi_rotation_180():

    filename = "/vsimem/test_envi_rotation_180.dat"
    ds = gdal.GetDriverByName("ENVI").Create(filename, 1, 1)
    ds.SetGeoTransform([0, 10, 0, 0, 0, 10])
    ds = None

    if os.path.exists(filename + ".aux.xml"):
        gdal.Unlink(filename + ".aux.xml")

    ds = gdal.Open(filename)
    got_gt = ds.GetGeoTransform()
    assert got_gt == (0, 10, 0, 0, 0, 10)
    ds = None

    gdal.GetDriverByName("ENVI").Delete(filename)


###############################################################################
# Test writing different interleaving


@pytest.mark.parametrize("interleaving", ["bip", "bil", "bsq"])
@pytest.mark.parametrize("explicit", [True, False])
def test_envi_writing_interleaving(interleaving, explicit):

    srcfilename = "data/envi/envi_rgbsmall_" + interleaving + ".img"
    dstfilename = "/vsimem/out"
    try:
        creationOptions = ["INTERLEAVE=" + interleaving] if explicit else []
        gdal.Translate(
            dstfilename, srcfilename, format="ENVI", creationOptions=creationOptions
        )
        ref_data = open(srcfilename, "rb").read()
        f = gdal.VSIFOpenL(dstfilename, "rb")
        if f:
            got_data = gdal.VSIFReadL(1, len(ref_data), f)
            gdal.VSIFCloseL(f)
            assert got_data == ref_data
    finally:
        gdal.Unlink(dstfilename)
        gdal.Unlink(dstfilename + ".hdr")


###############################################################################
# Test writing different interleaving (larger file)


@pytest.mark.parametrize("interleaving", ["bip", "bil", "bsq"])
def test_envi_writing_interleaving_larger_file(interleaving):

    dstfilename = "/vsimem/out"
    try:
        xsize = 10000
        ysize = 10
        bands = 100
        with gdaltest.SetCacheMax(xsize * (ysize // 2)):
            ds = gdal.GetDriverByName("ENVI").Create(
                dstfilename, xsize, ysize, bands, options=["INTERLEAVE=" + interleaving]
            )
            ds.GetRasterBand(1).Fill(1)
            for i in range(bands):
                v = struct.pack("B", i + 1)
                ds.GetRasterBand(i + 1).WriteRaster(
                    0, 0, xsize, ysize // 2, v * (xsize * (ysize // 2))
                )
            for i in range(bands):
                v = struct.pack("B", i + 1)
                ds.GetRasterBand(i + 1).WriteRaster(
                    0, ysize // 2, xsize, ysize // 2, v * (xsize * (ysize // 2))
                )
            ds = None

        ds = gdal.Open(dstfilename)
        for i in range(bands):
            v = struct.pack("B", i + 1)
            assert ds.GetRasterBand(i + 1).ReadRaster() == v * (xsize * ysize)
    finally:
        gdal.Unlink(dstfilename)
        gdal.Unlink(dstfilename + ".hdr")


###############################################################################
# Test .hdr as an additional extension, not a replacement one


def test_envi_add_hdr():

    drv = gdal.GetDriverByName("ENVI")

    ds = drv.Create(
        "/vsimem/test.int",
        xsize=10,
        ysize=10,
        bands=1,
        eType=gdal.GDT_CFloat32,
        options=["SUFFIX=ADD"],
    )
    ds = None

    ds = gdal.Open("/vsimem/test.int")
    assert ds.RasterCount == 1
    ds = None

    ds = drv.Create(
        "/vsimem/test.int.mph",
        xsize=10,
        ysize=10,
        bands=2,
        eType=gdal.GDT_Float32,
        options=["SUFFIX=ADD"],
    )
    # Will check that test.int.mph.hdr is used prioritarily over test.int.hdr
    assert ds.RasterCount == 2
    ds = None

    ds = gdal.Open("/vsimem/test.int.mph")
    assert ds.RasterCount == 2
    ds = None

    drv.Delete("/vsimem/test.int")
    drv.Delete("/vsimem/test.int.mph")


###############################################################################
# Test .hdr as an additional extension, not a replacement one


def test_envi_edit_coordinate_system_string():

    filename = "/vsimem/test.bin"
    drv = gdal.GetDriverByName("ENVI")
    ds = drv.Create(filename, 1, 1)
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(4326)
    ds.SetSpatialRef(srs)
    ds = None

    ds = gdal.Open(filename, gdal.GA_Update)
    assert ds.GetSpatialRef().GetAuthorityCode(None) == "4326"
    srs = osr.SpatialReference()
    srs.ImportFromEPSG(3261)
    ds.SetSpatialRef(srs)
    ds = None

    fp = gdal.VSIFOpenL(filename[0:-4] + ".hdr", "rb")
    assert fp
    content = gdal.VSIFReadL(1, 1000, fp).decode("utf-8")
    gdal.VSIFCloseL(fp)

    assert content.count("coordinate system string") == 1

    ds = gdal.Open(filename)
    srs = ds.GetSpatialRef()
    assert srs.IsProjected()
    assert srs.GetAuthorityCode(None) == "3261"
    ds = None

    drv.Delete(filename)


###############################################################################
# Test reading "default bands" in RGB mode


def test_envi_read_default_bands_rgb():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
samples = 1
lines = 1
bands = 3
header offset = 0
file type = ENVI Standard
data type = 1
interleave = bip
sensor type = Unknown
byte order = 0
default bands = {3, 2, 1}""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    ds = gdal.Open("/vsimem/test.bin")
    assert ds.GetRasterBand(1).GetColorInterpretation() == gdal.GCI_BlueBand
    assert ds.GetRasterBand(2).GetColorInterpretation() == gdal.GCI_GreenBand
    assert ds.GetRasterBand(3).GetColorInterpretation() == gdal.GCI_RedBand
    ds = None
    assert gdal.VSIStatL("/vsimem/test.bin.aux.xml") is None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")


###############################################################################
# Test reading "default bands" in Gray mode


def test_envi_read_default_bands_gray():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
samples = 1
lines = 1
bands = 3
header offset = 0
file type = ENVI Standard
data type = 1
interleave = bip
sensor type = Unknown
byte order = 0
default bands = {2}""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    ds = gdal.Open("/vsimem/test.bin")
    assert ds.GetRasterBand(1).GetColorInterpretation() == gdal.GCI_Undefined
    assert ds.GetRasterBand(2).GetColorInterpretation() == gdal.GCI_GrayIndex
    assert ds.GetRasterBand(3).GetColorInterpretation() == gdal.GCI_Undefined
    ds = None
    assert gdal.VSIStatL("/vsimem/test.bin.aux.xml") is None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")


###############################################################################
# Test writing "default bands" in RGB mode


def test_envi_write_default_bands_rgb():

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 3)
    src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_BlueBand)
    src_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_RedBand)
    src_ds.GetRasterBand(3).SetColorInterpretation(gdal.GCI_GreenBand)
    gdal.GetDriverByName("ENVI").CreateCopy("/vsimem/test.bin", src_ds)

    fp = gdal.VSIFOpenL("/vsimem/test.hdr", "rb")
    assert fp
    content = gdal.VSIFReadL(1, 1000, fp).decode("utf-8")
    gdal.VSIFCloseL(fp)

    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")

    assert "default bands = {2, 3, 1}" in content, content


###############################################################################
# Test writing "default bands" in Gray mode


def test_envi_write_default_bands_gray():

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 3)
    src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_Undefined)
    src_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_GrayIndex)
    src_ds.GetRasterBand(3).SetColorInterpretation(gdal.GCI_Undefined)
    gdal.GetDriverByName("ENVI").CreateCopy("/vsimem/test.bin", src_ds)

    fp = gdal.VSIFOpenL("/vsimem/test.hdr", "rb")
    assert fp
    content = gdal.VSIFReadL(1, 1000, fp).decode("utf-8")
    gdal.VSIFCloseL(fp)

    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")

    assert "default bands = {2}" in content, content


###############################################################################
# Test writing "default bands" when it doesn't work


def test_envi_write_default_bands_duplicate_color_rgb():

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 6)
    src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_BlueBand)
    src_ds.GetRasterBand(2).SetColorInterpretation(gdal.GCI_RedBand)
    src_ds.GetRasterBand(3).SetColorInterpretation(gdal.GCI_GreenBand)
    src_ds.GetRasterBand(4).SetColorInterpretation(gdal.GCI_BlueBand)
    src_ds.GetRasterBand(5).SetColorInterpretation(gdal.GCI_RedBand)
    src_ds.GetRasterBand(6).SetColorInterpretation(gdal.GCI_GreenBand)
    gdal.GetDriverByName("ENVI").CreateCopy("/vsimem/test.bin", src_ds)

    fp = gdal.VSIFOpenL("/vsimem/test.hdr", "rb")
    assert fp
    content = gdal.VSIFReadL(1, 1000, fp).decode("utf-8")
    gdal.VSIFCloseL(fp)

    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")

    assert "default bands" not in content, content


###############################################################################
# Test writing "default bands" when it doesn't work


def test_envi_write_default_bands_duplicate_color_gray():

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 6)
    src_ds.GetRasterBand(1).SetColorInterpretation(gdal.GCI_GrayIndex)
    src_ds.GetRasterBand(3).SetColorInterpretation(gdal.GCI_GrayIndex)
    gdal.GetDriverByName("ENVI").CreateCopy("/vsimem/test.bin", src_ds)

    fp = gdal.VSIFOpenL("/vsimem/test.hdr", "rb")
    assert fp
    content = gdal.VSIFReadL(1, 1000, fp).decode("utf-8")
    gdal.VSIFCloseL(fp)

    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")

    assert "default bands" not in content, content


###############################################################################
# Test reading "data offset values"


def test_envi_read_data_offset_values():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
samples = 1
lines = 1
bands = 3
header offset = 0
file type = ENVI Standard
data type = 1
interleave = bip
sensor type = Unknown
byte order = 0
data offset values = {3.5,2,1}""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    ds = gdal.Open("/vsimem/test.bin")
    assert ds.GetRasterBand(1).GetOffset() == 3.5
    assert ds.GetRasterBand(2).GetOffset() == 2
    assert ds.GetRasterBand(3).GetOffset() == 1
    ds = None
    assert gdal.VSIStatL("/vsimem/test.bin.aux.xml") is None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")


###############################################################################
# Test reading "data gain values"


def test_envi_read_data_gain_values():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
samples = 1
lines = 1
bands = 3
header offset = 0
file type = ENVI Standard
data type = 1
interleave = bip
sensor type = Unknown
byte order = 0
data gain values = {3.5,2,1}""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    ds = gdal.Open("/vsimem/test.bin")
    assert ds.GetRasterBand(1).GetScale() == 3.5
    assert ds.GetRasterBand(2).GetScale() == 2
    assert ds.GetRasterBand(3).GetScale() == 1
    ds = None
    assert gdal.VSIStatL("/vsimem/test.bin.aux.xml") is None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")


###############################################################################
# Test writing "data offset values"


def test_envi_write_data_offset_values():

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 3)
    src_ds.GetRasterBand(2).SetOffset(10)
    gdal.GetDriverByName("ENVI").CreateCopy("/vsimem/test.bin", src_ds)

    fp = gdal.VSIFOpenL("/vsimem/test.hdr", "rb")
    assert fp
    content = gdal.VSIFReadL(1, 1000, fp).decode("utf-8")
    gdal.VSIFCloseL(fp)

    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")

    assert "data offset values = {0, 10, 0}" in content, content


###############################################################################
# Test writing "data gain values"


def test_envi_write_data_gain_values():

    src_ds = gdal.GetDriverByName("MEM").Create("", 1, 1, 3)
    src_ds.GetRasterBand(2).SetScale(10)
    gdal.GetDriverByName("ENVI").CreateCopy("/vsimem/test.bin", src_ds)

    fp = gdal.VSIFOpenL("/vsimem/test.hdr", "rb")
    assert fp
    content = gdal.VSIFReadL(1, 1000, fp).decode("utf-8")
    gdal.VSIFCloseL(fp)

    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")

    assert "data gain values = {1, 10, 1}" in content, content


###############################################################################
# Test direct access to BIP scanlines


@pytest.mark.parametrize("byte_order", ["LITTLE_ENDIAN", "BIG_ENDIAN"])
def test_envi_read_direct_access(byte_order):

    src_ds = gdal.Open("data/rgbsmall.tif")
    filename = "/vsimem/test.bin"
    gdal.Translate(
        filename,
        src_ds,
        format="ENVI",
        outputType=gdal.GDT_UInt16,
        creationOptions=["INTERLEAVE=BIP", "@BYTE_ORDER=" + byte_order],
    )
    ds = gdal.Open(filename)

    # Using optimization
    assert ds.ReadRaster(
        0,
        0,
        ds.RasterXSize,
        ds.RasterYSize,
        buf_type=gdal.GDT_UInt16,
        buf_pixel_space=2 * ds.RasterCount,
        buf_band_space=2,
    ) == src_ds.ReadRaster(
        0,
        0,
        ds.RasterXSize,
        ds.RasterYSize,
        buf_type=gdal.GDT_UInt16,
        buf_pixel_space=2 * ds.RasterCount,
        buf_band_space=2,
    )

    assert ds.ReadRaster(
        1,
        2,
        3,
        4,
        buf_type=gdal.GDT_UInt16,
        buf_pixel_space=2 * ds.RasterCount,
        buf_band_space=2,
    ) == src_ds.ReadRaster(
        1,
        2,
        3,
        4,
        buf_type=gdal.GDT_UInt16,
        buf_pixel_space=2 * ds.RasterCount,
        buf_band_space=2,
    )

    # Non-optimized (at time of writing...)

    # buffer type != native data type
    assert ds.ReadRaster(
        1,
        2,
        3,
        4,
        buf_type=gdal.GDT_Byte,
        buf_pixel_space=ds.RasterCount,
        buf_band_space=1,
    ) == src_ds.ReadRaster(
        1,
        2,
        3,
        4,
        buf_type=gdal.GDT_Byte,
        buf_pixel_space=ds.RasterCount,
        buf_band_space=1,
    )

    ds = None

    gdal.GetDriverByName("ENVI").Delete(filename)


###############################################################################
# Test direct access to BIP scanlines in GA_Update mode


def test_envi_read_direct_access_update_scenario():

    src_ds = gdal.Open("data/rgbsmall.tif")
    filename = "/vsimem/test.bin"
    ds = gdal.GetDriverByName("ENVI").Create(
        filename,
        src_ds.RasterXSize,
        src_ds.RasterYSize,
        src_ds.RasterCount,
        options=["INTERLEAVE=BIP"],
    )
    ds.WriteRaster(0, 0, ds.RasterXSize, ds.RasterYSize, src_ds.ReadRaster())

    # Using optimization
    assert ds.ReadRaster(
        0,
        0,
        ds.RasterXSize,
        ds.RasterYSize,
        buf_type=gdal.GDT_Byte,
        buf_pixel_space=ds.RasterCount,
        buf_band_space=1,
    ) == src_ds.ReadRaster(
        0,
        0,
        ds.RasterXSize,
        ds.RasterYSize,
        buf_type=gdal.GDT_Byte,
        buf_pixel_space=ds.RasterCount,
        buf_band_space=1,
    )

    ds = None

    gdal.GetDriverByName("ENVI").Delete(filename)


###############################################################################
# Test setting different nodata values


@pytest.mark.parametrize(
    "nd1,nd2,expected_warning",
    [
        (1, 1, False),
        (float("nan"), float("nan"), False),
        (float("nan"), 1, True),
        (1, float("nan"), True),
    ],
)
def test_envi_write_warn_different_nodata(tmp_vsimem, nd1, nd2, expected_warning):
    filename = str(tmp_vsimem / "test_envi_write_warn_different_nodata.img")
    ds = gdal.GetDriverByName("ENVI").Create(filename, 1, 1, 2)
    assert ds.GetRasterBand(1).SetNoDataValue(nd1) == gdal.CE_None
    gdal.ErrorReset()
    with gdal.quiet_errors():
        assert ds.GetRasterBand(2).SetNoDataValue(nd2) == gdal.CE_None
        assert gdal.GetLastErrorType() == (
            gdal.CE_Warning if expected_warning else gdal.CE_None
        )


###############################################################################
# Test reading "default bands" in RGB mode


def test_envi_read_metadata_with_leading_space():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
samples = 1
lines = 1
bands = 3
header offset = 0
file type = ENVI Standard
data type = 1
interleave = bip
sensor type = Unknown
byte order = 0
 wavelength = {3, 2, 1}""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    ds = gdal.Open("/vsimem/test.bin")
    assert ds.GetRasterBand(1).GetMetadataItem("wavelength") == "3"
    ds = None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")


###############################################################################
# Test wavelength / fwhm


def test_envi_read_wavelength_fwhm_um():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
samples = 1
lines = 1
bands = 3
header offset = 0
file type = ENVI Standard
data type = 1
interleave = bip
sensor type = Unknown
byte order = 0
wavelength units = um
wavelength = {3, 2, 1}
fwhm = {.3, .2, .1}""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    ds = gdal.Open("/vsimem/test.bin")
    assert (
        ds.GetRasterBand(1).GetMetadataItem("CENTRAL_WAVELENGTH_UM", "IMAGERY")
        == "3.000"
    )
    assert ds.GetRasterBand(1).GetMetadataItem("FWHM_UM", "IMAGERY") == "0.300"
    assert (
        ds.GetRasterBand(2).GetMetadataItem("CENTRAL_WAVELENGTH_UM", "IMAGERY")
        == "2.000"
    )
    assert ds.GetRasterBand(2).GetMetadataItem("FWHM_UM", "IMAGERY") == "0.200"
    ds = None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")


###############################################################################
# Test wavelength / fwhm


def test_envi_read_wavelength_fwhm_nm():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
samples = 1
lines = 1
bands = 3
header offset = 0
file type = ENVI Standard
data type = 1
interleave = bip
sensor type = Unknown
byte order = 0
wavelength units = nm
wavelength = {3000, 2000, 1000}
fwhm = {300, 200, 100}""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    ds = gdal.Open("/vsimem/test.bin")
    assert (
        ds.GetRasterBand(1).GetMetadataItem("CENTRAL_WAVELENGTH_UM", "IMAGERY")
        == "3.000"
    )
    assert ds.GetRasterBand(1).GetMetadataItem("FWHM_UM", "IMAGERY") == "0.300"
    assert (
        ds.GetRasterBand(2).GetMetadataItem("CENTRAL_WAVELENGTH_UM", "IMAGERY")
        == "2.000"
    )
    assert ds.GetRasterBand(2).GetMetadataItem("FWHM_UM", "IMAGERY") == "0.200"
    ds = None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")


###############################################################################
# Test wavelength / fwhm


def test_envi_read_wavelength_fwhm_mm():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
samples = 1
lines = 1
bands = 3
header offset = 0
file type = ENVI Standard
data type = 1
interleave = bip
sensor type = Unknown
byte order = 0
wavelength units = mm
wavelength = {0.003, 0.002, 0.001}
fwhm = {0.0003, 0.0002, 0.0001}""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    ds = gdal.Open("/vsimem/test.bin")
    assert (
        ds.GetRasterBand(1).GetMetadataItem("CENTRAL_WAVELENGTH_UM", "IMAGERY")
        == "3.000"
    )
    assert ds.GetRasterBand(1).GetMetadataItem("FWHM_UM", "IMAGERY") == "0.300"
    assert (
        ds.GetRasterBand(2).GetMetadataItem("CENTRAL_WAVELENGTH_UM", "IMAGERY")
        == "2.000"
    )
    assert ds.GetRasterBand(2).GetMetadataItem("FWHM_UM", "IMAGERY") == "0.200"
    ds = None
    gdal.GetDriverByName("ENVI").Delete("/vsimem/test.bin")


###############################################################################


def test_envi_read_too_large_lines():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
file type = ENVI Standard
sensor type = Unknown
byte order = 0
header offset = 0
samples = 2
lines = 2147483648
bands = 1
x start = 1
y start = 1
interleave = bip
""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    with gdaltest.error_raised(
        gdal.CE_Warning,
        match="Limiting number of lines from 2147483648 to 2147483647 due to GDAL raster data model limitation",
    ):
        ds = gdal.Open("/vsimem/test.bin")
        assert ds.RasterXSize == 2
        assert ds.RasterYSize == 2147483647


###############################################################################


def test_envi_read_too_large_samples():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
file type = ENVI Standard
sensor type = Unknown
byte order = 0
header offset = 0
samples = 2147483648
lines = 2
bands = 1
x start = 1
y start = 1
interleave = bip
""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    with pytest.raises(
        Exception,
        match="Cannot handle samples=2147483648 due to GDAL raster data model limitation",
    ):
        gdal.Open("/vsimem/test.bin")


###############################################################################


def test_envi_read_too_large_bands():

    gdal.FileFromMemBuffer(
        "/vsimem/test.hdr",
        """ENVI
file type = ENVI Standard
sensor type = Unknown
byte order = 0
header offset = 0
samples = 1
lines = 2
bands = 2147483648
x start = 1
y start = 1
interleave = bip
""",
    )
    gdal.FileFromMemBuffer("/vsimem/test.bin", "xyz")

    with pytest.raises(
        Exception,
        match="Cannot handle bands=2147483648 due to GDAL raster data model limitation",
    ):
        gdal.Open("/vsimem/test.bin")
