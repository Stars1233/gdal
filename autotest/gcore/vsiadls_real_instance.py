#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Test /vsiadls
# Author:   Even Rouault <even dot rouault at spatialys dot com>
#
###############################################################################
# Copyright (c) 2020 Even Rouault <even dot rouault at spatialys dot com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import base64
import stat

import pytest

from osgeo import gdal

pytestmark = pytest.mark.require_curl()


def open_for_read(uri):
    """
    Opens a test file for reading.
    """
    return gdal.VSIFOpenExL(uri, "rb", 1)


###############################################################################
# Nominal cases (require valid credentials)


def test_vsiadls_real_instance_tests():

    adls_resource = gdal.GetConfigOption("ADLS_RESOURCE")
    if adls_resource is None:
        pytest.skip("Missing ADLS_RESOURCE")

    if "/" not in adls_resource:
        path = "/vsiadls/" + adls_resource

        try:
            statres = gdal.VSIStatL(path)
            assert statres is not None and stat.S_ISDIR(statres.mode), (
                "%s is not a valid bucket" % path
            )

            readdir = gdal.ReadDir(path)
            assert readdir is not None, "ReadDir() should not return empty list"
            for filename in readdir:
                if filename != ".":
                    subpath = path + "/" + filename
                    assert gdal.VSIStatL(subpath) is not None, (
                        "Stat(%s) should not return an error" % subpath
                    )

            unique_id = "vsiadls_test"
            subpath = path + "/" + unique_id
            ret = gdal.Mkdir(subpath, 0)
            assert ret >= 0, "Mkdir(%s) should not return an error" % subpath

            readdir = gdal.ReadDir(path)
            assert unique_id in readdir, "ReadDir(%s) should contain %s" % (
                path,
                unique_id,
            )

            ret = gdal.Mkdir(subpath, 0)
            assert ret != 0, "Mkdir(%s) repeated should return an error" % subpath

            ret = gdal.Rmdir(subpath)
            assert ret >= 0, "Rmdir(%s) should not return an error" % subpath

            readdir = gdal.ReadDir(path)
            assert unique_id not in readdir, "ReadDir(%s) should not contain %s" % (
                path,
                unique_id,
            )

            ret = gdal.Rmdir(subpath)
            assert ret != 0, "Rmdir(%s) repeated should return an error" % subpath

            ret = gdal.Mkdir(subpath, 0)
            assert ret >= 0, "Mkdir(%s) should not return an error" % subpath

            f = gdal.VSIFOpenL(subpath + "/test.txt", "wb")
            assert f is not None
            gdal.VSIFWriteL("hello", 1, 5, f)
            gdal.VSIFCloseL(f)

            ret = gdal.Rmdir(subpath)
            assert ret != 0, (
                "Rmdir(%s) on non empty directory should return an error" % subpath
            )

            f = gdal.VSIFOpenL(subpath + "/test.txt", "rb")
            assert f is not None
            data = gdal.VSIFReadL(1, 5, f).decode("utf-8")
            assert data == "hello"
            gdal.VSIFCloseL(f)

            assert gdal.VSIStatL(subpath + "/test.txt") is not None

            md = gdal.GetFileMetadata(subpath + "/test.txt", "HEADERS")
            assert "x-ms-properties" in md

            md = gdal.GetFileMetadata(subpath + "/test.txt", "STATUS")
            assert "x-ms-resource-type" in md
            assert "x-ms-properties" not in md

            md = gdal.GetFileMetadata(subpath + "/test.txt", "ACL")
            assert "x-ms-acl" in md
            assert "x-ms-permissions" in md

            # Change properties
            properties_foo_bar = "foo=" + base64.b64encode("bar")
            assert gdal.SetFileMetadata(
                subpath + "/test.txt",
                {"x-ms-properties": properties_foo_bar},
                "PROPERTIES",
            )

            md = gdal.GetFileMetadata(subpath + "/test.txt", "HEADERS")
            assert "x-ms-properties" in md
            assert md["x-ms-properties"] == properties_foo_bar

            # Change ACL
            assert gdal.SetFileMetadata(
                subpath + "/test.txt", {"x-ms-permissions": "0777"}, "ACL"
            )

            md = gdal.GetFileMetadata(subpath + "/test.txt", "ACL")
            assert "x-ms-permissions" in md
            assert md["x-ms-permissions"] == "rwxrwxrwx"

            # Change ACL recursively
            md = gdal.GetFileMetadata(subpath, "ACL")
            assert "x-ms-acl" in md
            assert gdal.SetFileMetadata(
                subpath + "/test.txt",
                {"x-ms-acl": md["x-ms-acl"]},
                "ACL",
                ["RECURSIVE=YES", "MODE=set"],
            )

            assert gdal.Rename(subpath + "/test.txt", subpath + "/test2.txt") == 0

            assert gdal.VSIStatL(subpath + "/test.txt") is None

            assert gdal.VSIStatL(subpath + "/test2.txt") is not None

            f = gdal.VSIFOpenL(subpath + "/test2.txt", "rb")
            assert f is not None
            data = gdal.VSIFReadL(1, 5, f).decode("utf-8")
            assert data == "hello"
            gdal.VSIFCloseL(f)

            ret = gdal.Unlink(subpath + "/test2.txt")
            assert ret >= 0, "Unlink(%s) should not return an error" % (
                subpath + "/test2.txt"
            )

            assert gdal.VSIStatL(subpath + "/test2.txt") is None

            assert (
                gdal.Unlink(subpath + "/test2.txt") != 0
            ), "Unlink on a deleted file should return an error"

            f = gdal.VSIFOpenL(subpath + "/test2.txt", "wb")
            assert f is not None
            gdal.VSIFCloseL(f)

            assert gdal.VSIStatL(subpath + "/test2.txt") is not None

        finally:
            assert gdal.RmdirRecursive(subpath) == 0

        return

    f = open_for_read("/vsiadls/" + adls_resource)
    assert f is not None
    ret = gdal.VSIFReadL(1, 1, f)
    gdal.VSIFCloseL(f)

    assert len(ret) == 1

    # Test GetSignedURL()
    signed_url = gdal.GetSignedURL("/vsiadls/" + adls_resource)
    f = open_for_read("/vsicurl_streaming/" + signed_url)
    assert f is not None
    ret = gdal.VSIFReadL(1, 1, f)
    gdal.VSIFCloseL(f)

    assert len(ret) == 1


###############################################################################
# Nominal cases (require valid credentials)
# Note: that test must be run with a delay > 30 seconds due to such a delay
# for re-creating a filesystem of the same name of one that has been destroyed


def test_vsiadls_real_instance_filesystem_tests():

    if gdal.GetConfigOption("ADLS_ALLOW_FILESYSTEM_TESTS") is None:
        pytest.skip("Missing ADLS_ALLOW_FILESYSTEM_TESTS")

    fspath = "/vsiadls/test-vsiadls-filesystem-tests"

    try:
        assert gdal.VSIStatL(fspath) is None

        assert gdal.Mkdir(fspath, 0) == 0

        statres = gdal.VSIStatL(fspath)
        assert statres is not None and stat.S_ISDIR(statres.mode)

        assert gdal.ReadDir(fspath) == ["."]

        assert gdal.Mkdir(fspath, 0) != 0

        assert gdal.Mkdir(fspath + "/subdir", 0) == 0

        statres = gdal.VSIStatL(fspath + "/subdir")
        assert statres is not None and stat.S_ISDIR(statres.mode)

        assert gdal.Rmdir(fspath) != 0

    finally:
        assert gdal.RmdirRecursive(fspath) == 0

        assert gdal.VSIStatL(fspath) is None
