#!/usr/bin/env pytest
# -*- coding: utf-8 -*-
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test read/write functionality for L1B driver.
# Author:   Even Rouault <even dot rouault at spatialys.com>
#
###############################################################################
# Copyright (c) 2008-2013, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import os

import gdaltest
import pytest

from osgeo import gdal

pytestmark = pytest.mark.require_driver("L1B")

###############################################################################
#


l1b_list = [
    ("http://download.osgeo.org/gdal/data/l1b", "n12gac8bit.l1b", 51754, -1, 1938),
    ("http://download.osgeo.org/gdal/data/l1b", "n12gac10bit.l1b", 46039, -1, 1887),
    (
        "http://download.osgeo.org/gdal/data/l1b",
        "n12gac10bit_ebcdic.l1b",
        46039,
        -1,
        1887,
    ),  # 2848
    ("http://download.osgeo.org/gdal/data/l1b", "n14gac16bit.l1b", 42286, -1, 2142),
    ("http://download.osgeo.org/gdal/data/l1b", "n15gac8bit.l1b", 55772, -1, 2091),
    ("http://download.osgeo.org/gdal/data/l1b", "n16gac10bit.l1b", 6749, -1, 2142),
    ("http://download.osgeo.org/gdal/data/l1b", "n17gac16bit.l1b", 61561, -1, 2040),
    (
        "http://www.ncdc.noaa.gov/oa/pod-guide/ncdc/docs/podug/data/avhrr",
        "frang.1b",
        33700,
        30000,
        357,
    ),  # 10 bit guess
    (
        "http://www.ncdc.noaa.gov/oa/pod-guide/ncdc/docs/podug/data/avhrr",
        "franh.1b",
        56702,
        100000,
        255,
    ),  # 10 bit guess
    (
        "http://www.ncdc.noaa.gov/oa/pod-guide/ncdc/docs/podug/data/avhrr",
        "calfirel.1b",
        55071,
        30000,
        255,
    ),  # 16 bit guess
    (
        "http://www.ncdc.noaa.gov/oa/pod-guide/ncdc/docs/podug/data/avhrr",
        "rapnzg.1b",
        58084,
        30000,
        612,
    ),  # 16 bit guess
    (
        "http://www.sat.dundee.ac.uk/testdata/new_noaa/new_klm_format/",
        "noaa18.n1b",
        50229,
        50000,
        102,
    ),
    ("http://www.sat.dundee.ac.uk/testdata/metop", "noaa1b", 62411, 150000, 408),
]


@pytest.mark.parametrize(
    "downloadURL,fileName,checksum,download_size,gcpNumber",
    l1b_list,
    ids=[item[1] for item in l1b_list],
)
def test_l1b(downloadURL, fileName, checksum, download_size, gcpNumber):
    gdaltest.download_or_skip(downloadURL + "/" + fileName, fileName, download_size)

    ds = gdal.Open("tmp/cache/" + fileName)

    assert ds.GetRasterBand(1).Checksum() == checksum

    assert len(ds.GetGCPs()) == gcpNumber


def test_l1b_geoloc():
    try:
        os.stat("tmp/cache/n12gac8bit.l1b")
    except OSError:
        pytest.skip()

    ds = gdal.Open("tmp/cache/n12gac8bit.l1b")
    md = ds.GetMetadata("GEOLOCATION")
    expected_md = {
        "LINE_OFFSET": "0",
        "LINE_STEP": "1",
        "PIXEL_OFFSET": "0",
        "PIXEL_STEP": "1",
        "X_BAND": "1",
        "X_DATASET": 'L1BGCPS_INTERPOL:"tmp/cache/n12gac8bit.l1b"',
        "Y_BAND": "2",
        "Y_DATASET": 'L1BGCPS_INTERPOL:"tmp/cache/n12gac8bit.l1b"',
    }
    for key in expected_md:
        assert md[key] == expected_md[key]
    ds = None

    ds = gdal.Open('L1BGCPS_INTERPOL:"tmp/cache/n12gac8bit.l1b"')
    cs = ds.GetRasterBand(1).Checksum()
    assert cs == 62397
    cs = ds.GetRasterBand(2).Checksum()
    assert cs == 52616


###############################################################################
#


def test_l1b_solar_zenith_angles_before_noaa_15():
    try:
        os.stat("tmp/cache/n12gac10bit.l1b")
    except OSError:
        pytest.skip()

    ds = gdal.Open("tmp/cache/n12gac10bit.l1b")
    md = ds.GetMetadata("SUBDATASETS")
    expected_md = {
        "SUBDATASET_1_NAME": 'L1B_SOLAR_ZENITH_ANGLES:"tmp/cache/n12gac10bit.l1b"',
        "SUBDATASET_1_DESC": "Solar zenith angles",
    }
    for key in expected_md:
        assert md[key] == expected_md[key]
    ds = None

    ds = gdal.Open('L1B_SOLAR_ZENITH_ANGLES:"tmp/cache/n12gac10bit.l1b"')
    cs = ds.GetRasterBand(1).Checksum()
    assert cs == 22924


###############################################################################
#


def test_l1b_metadata_before_noaa_15():
    try:
        os.stat("tmp/cache/n12gac10bit.l1b")
    except OSError:
        pytest.skip()

    with gdal.config_options(
        {"L1B_FETCH_METADATA": "YES", "L1B_METADATA_DIRECTORY": "tmp"}
    ):
        ds = gdal.Open("tmp/cache/n12gac10bit.l1b")
    del ds

    f = open("tmp/n12gac10bit.l1b_metadata.csv", "rb")
    ln = f.readline().decode("ascii")
    assert (
        ln
        == "SCANLINE,NBLOCKYOFF,YEAR,DAY,MS_IN_DAY,FATAL_FLAG,TIME_ERROR,DATA_GAP,DATA_JITTER,INSUFFICIENT_DATA_FOR_CAL,NO_EARTH_LOCATION,DESCEND,P_N_STATUS,BIT_SYNC_STATUS,SYNC_ERROR,FRAME_SYNC_ERROR,FLYWHEELING,BIT_SLIPPAGE,C3_SBBC,C4_SBBC,C5_SBBC,TIP_PARITY_FRAME_1,TIP_PARITY_FRAME_2,TIP_PARITY_FRAME_3,TIP_PARITY_FRAME_4,TIP_PARITY_FRAME_5,SYNC_ERRORS,CAL_SLOPE_C1,CAL_INTERCEPT_C1,CAL_SLOPE_C2,CAL_INTERCEPT_C2,CAL_SLOPE_C3,CAL_INTERCEPT_C3,CAL_SLOPE_C4,CAL_INTERCEPT_C4,CAL_SLOPE_C5,CAL_INTERCEPT_C5,NUM_SOLZENANGLES_EARTHLOCPNTS\n"
    )
    ln = f.readline().decode("ascii")
    assert (
        ln
        == "3387,0,1998,84,16966146,0,0,0,0,0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0.102000,-4.130000,0.103000,-4.210000,-0.001677,1.667438,-0.157728,156.939636,-0.179833,179.775742,51\n"
    )
    f.close()

    os.unlink("tmp/n12gac10bit.l1b_metadata.csv")


###############################################################################
#


def test_l1b_angles_after_noaa_15():
    try:
        os.stat("tmp/cache/n16gac10bit.l1b")
    except OSError:
        pytest.skip()

    ds = gdal.Open("tmp/cache/n16gac10bit.l1b")
    md = ds.GetMetadata("SUBDATASETS")
    expected_md = {
        "SUBDATASET_1_NAME": 'L1B_ANGLES:"tmp/cache/n16gac10bit.l1b"',
        "SUBDATASET_1_DESC": "Solar zenith angles, satellite zenith angles and relative azimuth angles",
    }
    for key in expected_md:
        assert md[key] == expected_md[key]
    ds = None

    ds = gdal.Open('L1B_ANGLES:"tmp/cache/n16gac10bit.l1b"')
    cs = ds.GetRasterBand(1).Checksum()
    assert cs == 31487
    cs = ds.GetRasterBand(2).Checksum()
    assert cs == 23380
    cs = ds.GetRasterBand(3).Checksum()
    assert cs == 64989


###############################################################################
#


def test_l1b_clouds_after_noaa_15():
    try:
        os.stat("tmp/cache/n16gac10bit.l1b")
    except OSError:
        pytest.skip()

    ds = gdal.Open("tmp/cache/n16gac10bit.l1b")
    md = ds.GetMetadata("SUBDATASETS")
    expected_md = {
        "SUBDATASET_2_NAME": 'L1B_CLOUDS:"tmp/cache/n16gac10bit.l1b"',
        "SUBDATASET_2_DESC": "Clouds from AVHRR (CLAVR)",
    }
    for key in expected_md:
        assert md[key] == expected_md[key]
    ds = None

    ds = gdal.Open('L1B_CLOUDS:"tmp/cache/n16gac10bit.l1b"')
    cs = ds.GetRasterBand(1).Checksum()
    assert cs == 0


###############################################################################
#


def test_l1b_metadata_after_noaa_15():
    try:
        os.stat("tmp/cache/n16gac10bit.l1b")
    except OSError:
        pytest.skip()

    with gdal.config_options(
        {"L1B_FETCH_METADATA": "YES", "L1B_METADATA_DIRECTORY": "tmp"}
    ):
        ds = gdal.Open("tmp/cache/n16gac10bit.l1b")
    del ds

    f = open("tmp/n16gac10bit.l1b_metadata.csv", "rb")
    ln = f.readline().decode("ascii")
    assert (
        ln
        == "SCANLINE,NBLOCKYOFF,YEAR,DAY,MS_IN_DAY,SAT_CLOCK_DRIF_DELTA,SOUTHBOUND,SCANTIME_CORRECTED,C3_SELECT,FATAL_FLAG,TIME_ERROR,DATA_GAP,INSUFFICIENT_DATA_FOR_CAL,NO_EARTH_LOCATION,FIRST_GOOD_TIME_AFTER_CLOCK_UPDATE,INSTRUMENT_STATUS_CHANGED,SYNC_LOCK_DROPPED,FRAME_SYNC_ERROR,FRAME_SYNC_DROPPED_LOCK,FLYWHEELING,BIT_SLIPPAGE,TIP_PARITY_ERROR,REFLECTED_SUNLIGHT_C3B,REFLECTED_SUNLIGHT_C4,REFLECTED_SUNLIGHT_C5,RESYNC,P_N_STATUS,BAD_TIME_CAN_BE_INFERRED,BAD_TIME_CANNOT_BE_INFERRED,TIME_DISCONTINUITY,REPEAT_SCAN_TIME,UNCALIBRATED_BAD_TIME,CALIBRATED_FEWER_SCANLINES,UNCALIBRATED_BAD_PRT,CALIBRATED_MARGINAL_PRT,UNCALIBRATED_CHANNELS,NO_EARTH_LOC_BAD_TIME,EARTH_LOC_QUESTIONABLE_TIME,EARTH_LOC_QUESTIONABLE,EARTH_LOC_VERY_QUESTIONABLE,C3B_UNCALIBRATED,C3B_QUESTIONABLE,C3B_ALL_BLACKBODY,C3B_ALL_SPACEVIEW,C3B_MARGINAL_BLACKBODY,C3B_MARGINAL_SPACEVIEW,C4_UNCALIBRATED,C4_QUESTIONABLE,C4_ALL_BLACKBODY,C4_ALL_SPACEVIEW,C4_MARGINAL_BLACKBODY,C4_MARGINAL_SPACEVIEW,C5_UNCALIBRATED,C5_QUESTIONABLE,C5_ALL_BLACKBODY,C5_ALL_SPACEVIEW,C5_MARGINAL_BLACKBODY,C5_MARGINAL_SPACEVIEW,BIT_ERRORS,VIS_OP_CAL_C1_SLOPE_1,VIS_OP_CAL_C1_INTERCEPT_1,VIS_OP_CAL_C1_SLOPE_2,VIS_OP_CAL_C1_INTERCEPT_2,VIS_OP_CAL_C1_INTERSECTION,VIS_TEST_CAL_C1_SLOPE_1,VIS_TEST_CAL_C1_INTERCEPT_1,VIS_TEST_CAL_C1_SLOPE_2,VIS_TEST_CAL_C1_INTERCEPT_2,VIS_TEST_CAL_C1_INTERSECTION,VIS_PRELAUNCH_CAL_C1_SLOPE_1,VIS_PRELAUNCH_CAL_C1_INTERCEPT_1,VIS_PRELAUNCH_CAL_C1_SLOPE_2,VIS_PRELAUNCH_CAL_C1_INTERCEPT_2,VIS_PRELAUNCH_CAL_C1_INTERSECTION,VIS_OP_CAL_C2_SLOPE_1,VIS_OP_CAL_C2_INTERCEPT_1,VIS_OP_CAL_C2_SLOPE_2,VIS_OP_CAL_C2_INTERCEPT_2,VIS_OP_CAL_C2_INTERSECTION,VIS_TEST_CAL_C2_SLOPE_1,VIS_TEST_CAL_C2_INTERCEPT_1,VIS_TEST_CAL_C2_SLOPE_2,VIS_TEST_CAL_C2_INTERCEPT_2,VIS_TEST_CAL_C2_INTERSECTION,VIS_PRELAUNCH_CAL_C2_SLOPE_1,VIS_PRELAUNCH_CAL_C2_INTERCEPT_1,VIS_PRELAUNCH_CAL_C2_SLOPE_2,VIS_PRELAUNCH_CAL_C2_INTERCEPT_2,VIS_PRELAUNCH_CAL_C2_INTERSECTION,VIS_OP_CAL_C3A_SLOPE_1,VIS_OP_CAL_C3A_INTERCEPT_1,VIS_OP_CAL_C3A_SLOPE_2,VIS_OP_CAL_C3A_INTERCEPT_2,VIS_OP_CAL_C3A_INTERSECTION,VIS_TEST_CAL_C3A_SLOPE_1,VIS_TEST_CAL_C3A_INTERCEPT_1,VIS_TEST_CAL_C3A_SLOPE_2,VIS_TEST_CAL_C3A_INTERCEPT_2,VIS_TEST_CAL_C3A_INTERSECTION,VIS_PRELAUNCH_CAL_C3A_SLOPE_1,VIS_PRELAUNCH_CAL_C3A_INTERCEPT_1,VIS_PRELAUNCH_CAL_C3A_SLOPE_2,VIS_PRELAUNCH_CAL_C3A_INTERCEPT_2,VIS_PRELAUNCH_CAL_C3A_INTERSECTION,IR_OP_CAL_C3B_COEFF_1,IR_OP_CAL_C3B_COEFF_2,IR_OP_CAL_C3B_COEFF_3,IR_TEST_CAL_C3B_COEFF_1,IR_TEST_CAL_C3B_COEFF_2,IR_TEST_CAL_C3B_COEFF_3,IR_OP_CAL_C4_COEFF_1,IR_OP_CAL_C4_COEFF_2,IR_OP_CAL_C4_COEFF_3,IR_TEST_CAL_C4_COEFF_1,IR_TEST_CAL_C4_COEFF_2,IR_TEST_CAL_C4_COEFF_3,IR_OP_CAL_C5_COEFF_1,IR_OP_CAL_C5_COEFF_2,IR_OP_CAL_C5_COEFF_3,IR_TEST_CAL_C5_COEFF_1,IR_TEST_CAL_C5_COEFF_2,IR_TEST_CAL_C5_COEFF_3,EARTH_LOC_CORR_TIP_EULER,EARTH_LOC_IND,SPACECRAFT_ATT_CTRL,ATT_SMODE,ATT_PASSIVE_WHEEL_TEST,TIME_TIP_EULER,TIP_EULER_ROLL,TIP_EULER_PITCH,TIP_EULER_YAW,SPACECRAFT_ALT\n"
    )
    ln = f.readline().decode("ascii")
    assert (
        ln
        == "3406,0,2003,85,3275054,79,1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0.052300,-2.015999,0.152800,-51.910000,499,0.052300,-2.015999,0.152800,-51.910000,498,0.052300,-2.015999,0.152800,-51.910000,498,0.051300,-1.942999,0.151000,-51.770000,500,0.051300,-1.942999,0.151000,-51.770000,500,0.051300,-1.942999,0.151000,-51.770000,500,0.000000,0.000000,0.000000,0.000000,0,0.000000,0.000000,0.000000,0.000000,0,0.000000,0.000000,0.000000,0.000000,0,2.488212,-0.002511,0.000000,2.488212,-0.002511,0.000000,179.546496,-0.188553,0.000008,179.546496,-0.188553,0.000008,195.236384,-0.201709,0.000006,195.236384,-0.201709,0.000006,0,0,0,0,0,608093,-0.021000,-0.007000,0.000000,862.000000\n"
    )
    f.close()

    os.unlink("tmp/n16gac10bit.l1b_metadata.csv")


###############################################################################
#


def test_l1b_little_endian():

    ds = gdal.Open("/vsizip/data/l1b/hrpt_little_endian.l1b.zip")
    assert ds.GetGCPProjection().find("GRS80") >= 0
    assert ds.GetRasterBand(1).Checksum() == 14145
    assert ds.GetRasterBand(1).GetMaskFlags() == gdal.GMF_PER_DATASET
    assert ds.GetRasterBand(1).GetMaskBand().Checksum() == 25115
    ds = None
