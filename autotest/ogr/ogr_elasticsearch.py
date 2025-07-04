#!/usr/bin/env pytest
###############################################################################
#
# Project:  GDAL/OGR Test Suite
# Purpose:  Elasticsearch driver testing (with fake server)
# Author:   Even Rouault <even dot rouault at spatialys.com>
#
###############################################################################
# Copyright (c) 2012, Even Rouault <even dot rouault at spatialys.com>
#
# SPDX-License-Identifier: MIT
###############################################################################

import json
import sys
import time

import gdaltest
import ogrtest
import pytest

from osgeo import gdal, ogr, osr

pytestmark = pytest.mark.require_driver("Elasticsearch")

###############################################################################
@pytest.fixture(autouse=True, scope="module")
def module_disable_exceptions():
    with gdaltest.disable_exceptions():
        yield


###############################################################################


@pytest.fixture()
def es_url(server):
    return f"http://localhost:{server.port}"


@pytest.fixture(autouse=True, scope="module")
def startup_and_cleanup():
    ogrtest.srs_wgs84 = osr.SpatialReference()
    ogrtest.srs_wgs84.SetFromUserInput("WGS84")

    ogrtest.elasticsearch_drv = ogr.GetDriverByName("Elasticsearch")

    with gdal.config_options(
        {
            "CPL_CURL_ENABLE_VSIMEM": "YES",
        }
    ):

        yield

    ogrtest.srs_wgs84 = None


###############################################################################
# Test writing into an nonexistent Elasticsearch datastore.


@pytest.mark.parametrize("include_driver_name", (True, False))
def test_ogr_elasticsearch_nonexistent_server(include_driver_name):

    if include_driver_name:
        dsn = "ES:/vsimem/nonexistent_host"
    else:
        dsn = "/vsimem/nonexistent_host"

    with gdal.quiet_errors():
        ds = ogrtest.elasticsearch_drv.CreateDataSource(dsn)
    assert ds is None, "managed to open nonexistent Elasticsearch datastore."

    assert "404" in gdal.GetLastErrorMsg()


###############################################################################
# Test invalid server version responses


@pytest.mark.parametrize(
    "response",
    ("{}", '{"version":null}', '{"version":{}', '{"version":{"number":null}}'),
)
def test_ogr_elasticsearch_invalid_server_version(tmp_vsimem, response):

    gdal.FileFromMemBuffer(tmp_vsimem / "fakeelasticsearch", """{}""")

    with gdal.quiet_errors():
        ds = ogrtest.elasticsearch_drv.Open(f"ES:{tmp_vsimem}/fakeelasticsearch")
    assert ds is None, "managed to open invalid Elasticsearch datastore."

    assert "version not found" in gdal.GetLastErrorMsg()


###############################################################################
# Simple test


def test_ogr_elasticsearch_1(
    tmp_vsimem, es_url, handle_get, handle_delete, handle_put, handle_post
):

    handle_get("/fakeelasticsearch", """{"version":{"number":"2.0.0"}}""")

    ds = ogrtest.elasticsearch_drv.CreateDataSource(f"{es_url}/fakeelasticsearch")
    assert ds is not None, "did not manage to open Elasticsearch datastore"

    assert ds.TestCapability(ogr.ODsCCreateLayer) != 0
    assert ds.TestCapability(ogr.ODsCDeleteLayer) != 0
    assert ds.TestCapability(ogr.ODsCCreateGeomFieldAfterCreateLayer) != 0

    # Failed index creation
    with gdal.quiet_errors():
        lyr = ds.CreateLayer("foo", srs=ogrtest.srs_wgs84, options=["FID="])
    assert lyr is None
    assert gdal.GetLastErrorType() == gdal.CE_Failure
    gdal.ErrorReset()

    # Successful index creation
    handle_put("/fakeelasticsearch/foo", "{}")
    lyr = ds.CreateLayer("foo", srs=ogrtest.srs_wgs84, options=["FID="])
    assert lyr is not None
    assert gdal.GetLastErrorType() == gdal.CE_None
    assert lyr.GetDataset().GetDescription() == ds.GetDescription()

    handle_post(
        "/fakeelasticsearch/foo/_mapping/FeatureCollection",
        post_body='={ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { } } } }',
        contents="{}",
    )

    # OVERWRITE an nonexistent layer.
    lyr = ds.CreateLayer(
        "foo", geom_type=ogr.wkbNone, options=["OVERWRITE=TRUE", "FID="]
    )
    assert gdal.GetLastErrorType() == gdal.CE_None

    # Simulate failed overwrite
    handle_get(
        "/fakeelasticsearch/foo", '{"foo":{"mappings":{"FeatureCollection":{}}}}'
    )
    with gdal.quiet_errors():
        lyr = ds.CreateLayer("foo", geom_type=ogr.wkbNone, options=["OVERWRITE=TRUE"])
    assert gdal.GetLastErrorType() == gdal.CE_Failure
    gdal.ErrorReset()

    # Successful overwrite
    handle_delete("/fakeelasticsearch/foo", "{}")
    handle_post("/fakeelasticsearch/foo/FeatureCollection", "{}", post_body="{}")
    lyr = ds.CreateLayer(
        "foo",
        geom_type=ogr.wkbNone,
        options=["OVERWRITE=TRUE", "BULK_INSERT=NO", "FID="],
    )
    assert gdal.GetLastErrorType() == gdal.CE_None

    assert lyr.TestCapability(ogr.OLCFastFeatureCount) != 0
    assert lyr.TestCapability(ogr.OLCStringsAsUTF8) != 0
    assert lyr.TestCapability(ogr.OLCSequentialWrite) != 0
    assert lyr.TestCapability(ogr.OLCCreateField) != 0
    assert lyr.TestCapability(ogr.OLCCreateGeomField) != 0

    feat = ogr.Feature(lyr.GetLayerDefn())

    handle_post(
        "/fakeelasticsearch/foo/FeatureCollection",
        post_body='{ "properties": { } }',
        contents="{}",
    )
    ret = lyr.CreateFeature(feat)
    assert ret == 0
    feat = None

    handle_put(
        "/fakeelasticsearch/foo",
        '{"error":"IndexAlreadyExistsException","status":400}',
    )
    with gdal.quiet_errors():
        lyr = ds.CreateLayer("foo", srs=ogrtest.srs_wgs84)
    assert gdal.GetLastErrorType() == gdal.CE_Failure
    assert lyr is None

    handle_post(
        "/fakeelasticsearch/foo/_mapping/FeatureCollection",
        post_body='{ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { "properties": { } }, "geometry": { "type": "geo_shape" } } } }',
        contents="",
    )

    handle_get("/fakeelasticsearch/_cat/indices?h=i", "")

    ds.DeleteLayer(-1)
    ds.DeleteLayer(10)
    ret = ds.DeleteLayer(0)
    assert ret == 0

    handle_put("/fakeelasticsearch/foo2", "{}")
    handle_post(
        "/fakeelasticsearch/foo2/_mapping/FeatureCollection",
        post_body='{ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { "properties": { "str_field": { "type": "string", "index": "not_analyzed" }, "int_field": { "type": "integer", "store": "yes" }, "int64_field": { "type": "long", "index": "no" }, "real_field": { "type": "double" }, "real_field_unset": { "type": "double" }, "boolean_field": { "type": "boolean" }, "strlist_field": { "type": "string" }, "intlist_field": { "type": "integer" }, "int64list_field": { "type": "long" }, "reallist_field": { "type": "double" }, "date_field": { "type": "date", "format": "yyyy\\/MM\\/dd HH:mm:ss.SSSZZ||yyyy\\/MM\\/dd HH:mm:ss.SSS||yyyy\\/MM\\/dd" }, "datetime_field": { "type": "date", "format": "yyyy\\/MM\\/dd HH:mm:ss.SSSZZ||yyyy\\/MM\\/dd HH:mm:ss.SSS||yyyy\\/MM\\/dd" }, "time_field": { "type": "date", "format": "HH:mm:ss.SSS" }, "binary_field": { "type": "binary" } } }, "geometry": { "properties": { "type": { "type": "string" }, "coordinates": { "type": "geo_point" } } } }, "_meta": { "fields": { "strlist_field": "StringList", "intlist_field": "IntegerList", "int64list_field": "Integer64List", "reallist_field": "RealList" } } } }',
        contents="{}",
    )
    lyr = ds.CreateLayer(
        "foo2",
        srs=ogrtest.srs_wgs84,
        geom_type=ogr.wkbPoint,
        options=[
            "BULK_INSERT=NO",
            "FID=",
            "STORED_FIELDS=int_field",
            "NOT_ANALYZED_FIELDS=str_field",
            "NOT_INDEXED_FIELDS=int64_field",
        ],
    )
    lyr.CreateField(ogr.FieldDefn("str_field", ogr.OFTString))
    lyr.CreateField(ogr.FieldDefn("int_field", ogr.OFTInteger))
    lyr.CreateField(ogr.FieldDefn("int64_field", ogr.OFTInteger64))
    lyr.CreateField(ogr.FieldDefn("real_field", ogr.OFTReal))
    lyr.CreateField(ogr.FieldDefn("real_field_unset", ogr.OFTReal))
    fld_defn = ogr.FieldDefn("boolean_field", ogr.OFTInteger)
    fld_defn.SetSubType(ogr.OFSTBoolean)
    lyr.CreateField(fld_defn)
    lyr.CreateField(ogr.FieldDefn("strlist_field", ogr.OFTStringList))
    lyr.CreateField(ogr.FieldDefn("intlist_field", ogr.OFTIntegerList))
    lyr.CreateField(ogr.FieldDefn("int64list_field", ogr.OFTInteger64List))
    lyr.CreateField(ogr.FieldDefn("reallist_field", ogr.OFTRealList))
    lyr.CreateField(ogr.FieldDefn("date_field", ogr.OFTDate))
    lyr.CreateField(ogr.FieldDefn("datetime_field", ogr.OFTDateTime))
    lyr.CreateField(ogr.FieldDefn("time_field", ogr.OFTTime))
    lyr.CreateField(ogr.FieldDefn("binary_field", ogr.OFTBinary))

    ret = lyr.SyncToDisk()
    assert ret == 0

    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetField("str_field", "a")
    feat.SetField("int_field", 1)
    feat.SetField("int64_field", 123456789012)
    feat.SetField("real_field", 2.34)
    feat.SetField("boolean_field", 1)
    feat["strlist_field"] = ["a", "b"]
    feat["intlist_field"] = [1, 2]
    feat["int64list_field"] = [123456789012, 2]
    feat["reallist_field"] = [1.23, 4.56]
    feat["date_field"] = "2015/08/12"
    feat["datetime_field"] = "2015/08/12 12:34:56.789"
    feat["time_field"] = "12:34:56.789"
    feat["binary_field"] = b"\x01\x23\x46\x57\x89\xAB\xCD\xEF"
    feat.SetGeometry(ogr.CreateGeometryFromWkt("POINT(0 1)"))

    # Simulate server error
    with gdal.quiet_errors():
        ret = lyr.CreateFeature(feat)
    assert ret != 0

    # Success
    handle_post(
        "/fakeelasticsearch/foo2/FeatureCollection",
        post_body='{ "geometry": { "type": "Point", "coordinates": [ 0.0, 1.0 ] }, "type": "Feature", "properties": { "str_field": "a", "int_field": 1, "int64_field": 123456789012, "real_field": 2.34, "boolean_field": true, "strlist_field": [ "a", "b" ], "intlist_field": [ 1, 2 ], "int64list_field": [ 123456789012, 2 ], "reallist_field": [ 1.23, 4.56 ], "date_field": "2015\\/08\\/12", "datetime_field": "2015\\/08\\/12 12:34:56.789", "time_field": "12:34:56.789", "binary_field": "ASNGV4mrze8=" } }',
        contents='{ "_id": "my_id" }',
    )
    ret = lyr.CreateFeature(feat)
    assert ret == 0
    assert feat["_id"] == "my_id"

    # DateTime with TZ
    handle_post(
        "/fakeelasticsearch/foo2/FeatureCollection",
        post_body='{ "properties": { "datetime_field": "2015\\/08\\/12 12:34:56.789+03:00" } }',
        contents="{}",
    )
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat["datetime_field"] = "2015/08/12 12:34:56.789+0300"
    ret = lyr.CreateFeature(feat)
    assert ret == 0

    # CreateFeature() with _id set
    handle_post(
        "/fakeelasticsearch/foo2/FeatureCollection/my_id2",
        post_body='{ "properties": { } }',
        contents="{}",
    )
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat["_id"] = "my_id2"
    ret = lyr.CreateFeature(feat)
    assert ret == 0

    # Failed SetFeature because of missing _id
    feat = ogr.Feature(lyr.GetLayerDefn())
    with gdal.quiet_errors():
        ret = lyr.SetFeature(feat)
    assert ret != 0

    # Simulate server error
    feat["_id"] = "my_id"
    with gdal.quiet_errors():
        ret = lyr.SetFeature(feat)
    assert ret != 0

    handle_post(
        "/fakeelasticsearch/foo2/FeatureCollection/my_id",
        post_body='{ "properties": { } }',
        contents="{}",
    )
    ret = lyr.SetFeature(feat)
    assert ret == 0

    # With explicit GEOM_MAPPING_TYPE=GEO_POINT
    handle_put(
        "/fakeelasticsearch/foo3",
        "{}",
    )
    handle_post(
        "/fakeelasticsearch/foo3/_mapping/FeatureCollection",
        post_body='{ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { "properties": { } }, "geometry": { "properties": { "type": { "type": "string" }, "coordinates": { "type": "geo_point", "fielddata": { "format": "compressed", "precision": "1m" } } } } }, "_meta": { "fid": "ogc_fid" } } }',
        contents="{}",
    )
    lyr = ds.CreateLayer(
        "foo3",
        srs=ogrtest.srs_wgs84,
        options=["GEOM_MAPPING_TYPE=GEO_POINT", "GEOM_PRECISION=1m", "BULK_INSERT=NO"],
    )

    handle_post(
        "/fakeelasticsearch/foo3/FeatureCollection",
        post_body='{ "ogc_fid": 1, "geometry": { "type": "Point", "coordinates": [ 0.5, 0.5 ] }, "type": "Feature", "properties": { } }',
        contents="{}",
    )
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(ogr.CreateGeometryFromWkt("LINESTRING(0 0,1 1)"))
    ret = lyr.CreateFeature(feat)
    assert ret == 0
    feat = None

    # Test explicit MAPPING first with error case
    handle_put("/fakeelasticsearch/foo4", "{}")
    with gdal.quiet_errors():
        lyr = ds.CreateLayer(
            "foo4",
            srs=ogrtest.srs_wgs84,
            options=['MAPPING={ "FeatureCollection": { "properties": {} }}'],
        )
    assert lyr is None

    # Test successful explicit MAPPING with inline JSon mapping
    handle_post(
        "/fakeelasticsearch/foo4/_mapping/FeatureCollection",
        post_body='{ "FeatureCollection": { "properties": {} }}',
        contents="{}",
    )
    lyr = ds.CreateLayer(
        "foo4",
        srs=ogrtest.srs_wgs84,
        options=['MAPPING={ "FeatureCollection": { "properties": {} }}'],
    )
    assert lyr is not None

    # Test successful explicit MAPPING with reference to file with mapping
    gdal.FileFromMemBuffer(
        tmp_vsimem / "map1.txt",
        '{ "FeatureCollection": { "properties": { "foo": { "type": "string" } } }}',
    )
    handle_post(
        "/fakeelasticsearch/foo4/_mapping/FeatureCollection",
        post_body='{ "FeatureCollection": { "properties": { "foo": { "type": "string" } } }}',
        contents="{}",
    )
    lyr = ds.CreateLayer(
        "foo4", srs=ogrtest.srs_wgs84, options=[f"MAPPING={tmp_vsimem}/map1.txt"]
    )
    assert lyr is not None

    # Test successful explicit INDEX_DEFINITION with inline JSon mapping
    handle_put("/fakeelasticsearch/foo4", "{}")
    handle_post(
        "/fakeelasticsearch/foo4/_mapping/FeatureCollection",
        post_body="{}",
        contents="{}",
    )
    lyr = ds.CreateLayer(
        "foo4", srs=ogrtest.srs_wgs84, options=["INDEX_DEFINITION={}", "MAPPING={}"]
    )
    assert lyr is not None

    # Test successful explicit INDEX_DEFINITION with reference to file
    gdal.FileFromMemBuffer(tmp_vsimem / "map2.txt", '{"foo":"bar"}')
    handle_put(
        "/fakeelasticsearch/foo4",
        contents="{}",
    )
    lyr = ds.CreateLayer(
        "foo4",
        srs=ogrtest.srs_wgs84,
        options=[
            f"INDEX_DEFINITION={tmp_vsimem}/map2.txt",
            "MAPPING={}",
        ],
    )
    assert lyr is not None


###############################################################################
# Geo_shape geometries


def test_ogr_elasticsearch_2(es_url, handle_get, handle_put, handle_post):

    handle_get("/fakeelasticsearch", """{"version":{"number":"2.0.0"}}""")

    ds = ogrtest.elasticsearch_drv.CreateDataSource(f"{es_url}/fakeelasticsearch")

    assert ds is not None, "did not manage to open Elasticsearch datastore"

    handle_put("/fakeelasticsearch/foo", "{}")
    handle_post(
        "/fakeelasticsearch/foo/_mapping/FeatureCollection",
        post_body='{ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { "properties": { } }, "geometry": { "type": "geo_shape" } } } }',
        contents="{}",
    )

    lyr = ds.CreateLayer(
        "foo", srs=ogrtest.srs_wgs84, options=["BULK_INSERT=NO", "FID="]
    )
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(
        ogr.CreateGeometryFromWkt(
            "GEOMETRYCOLLECTION(POINT(0 1),LINESTRING(0 1,2 3),POLYGON((0 0,0 10,10 10,0 0),(1 1,1 9,9 9,1 1)),MULTIPOINT(0 1, 2 3),MULTILINESTRING((0 1,2 3),(4 5,6 7)),MULTIPOLYGON(((0 0,0 10,10 10,0 0),(1 1,1 9,9 9,1 1)),((-1 -1,-1 -9,-9 -9,-1 -1))))"
        )
    )

    handle_post(
        "/fakeelasticsearch/foo/FeatureCollection",
        post_body='{ "geometry": { "type": "geometrycollection", "geometries": [ { "type": "point", "coordinates": [ 0.0, 1.0 ] }, { "type": "linestring", "coordinates": [ [ 0.0, 1.0 ], [ 2.0, 3.0 ] ] }, { "type": "polygon", "coordinates": [ [ [ 0.0, 0.0 ], [ 0.0, 10.0 ], [ 10.0, 10.0 ], [ 0.0, 0.0 ] ], [ [ 1.0, 1.0 ], [ 1.0, 9.0 ], [ 9.0, 9.0 ], [ 1.0, 1.0 ] ] ] }, { "type": "multipoint", "coordinates": [ [ 0.0, 1.0 ], [ 2.0, 3.0 ] ] }, { "type": "multilinestring", "coordinates": [ [ [ 0.0, 1.0 ], [ 2.0, 3.0 ] ], [ [ 4.0, 5.0 ], [ 6.0, 7.0 ] ] ] }, { "type": "multipolygon", "coordinates": [ [ [ [ 0.0, 0.0 ], [ 0.0, 10.0 ], [ 10.0, 10.0 ], [ 0.0, 0.0 ] ], [ [ 1.0, 1.0 ], [ 1.0, 9.0 ], [ 9.0, 9.0 ], [ 1.0, 1.0 ] ] ], [ [ [ -1.0, -1.0 ], [ -1.0, -9.0 ], [ -9.0, -9.0 ], [ -1.0, -1.0 ] ] ] ] } ] }, "type": "Feature", "properties": { } }',
        contents="{}",
    )
    ret = lyr.CreateFeature(feat)
    assert ret == 0
    feat = None

    # Same but with explicit GEOM_MAPPING_TYPE=GEO_SHAPE
    lyr = ds.CreateLayer(
        "foo",
        srs=ogrtest.srs_wgs84,
        options=[
            "GEOM_MAPPING_TYPE=GEO_SHAPE",
            "GEOM_PRECISION=1m",
            "BULK_INSERT=NO",
            "FID=",
        ],
    )
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetGeometry(
        ogr.CreateGeometryFromWkt(
            "GEOMETRYCOLLECTION(POINT(0 1),LINESTRING(0 1,2 3),POLYGON((0 0,0 10,10 10,0 0),(1 1,1 9,9 9,1 1)),MULTIPOINT(0 1, 2 3),MULTILINESTRING((0 1,2 3),(4 5,6 7)),MULTIPOLYGON(((0 0,0 10,10 10,0 0),(1 1,1 9,9 9,1 1)),((-1 -1,-1 -9,-9 -9,-1 -1))))"
        )
    )

    handle_post(
        "/fakeelasticsearch/foo/_mapping/FeatureCollection",
        post_body='{ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { "properties": { } }, "geometry": { "type": "geo_shape", "precision": "1m" } } } }',
        contents="{}",
    )

    ret = lyr.CreateFeature(feat)
    assert ret == 0
    feat = None


###############################################################################
# Test bulk insert and layer name laundering


def test_ogr_elasticsearch_3(es_url, handle_get, handle_post, handle_put):

    handle_get("/fakeelasticsearch", """{"version":{"number":"2.0.0"}}""")

    ds = ogrtest.elasticsearch_drv.CreateDataSource(f"{es_url}/fakeelasticsearch")
    assert ds is not None, "did not manage to open Elasticsearch datastore"

    handle_put("/fakeelasticsearch/name_laundering", "{}")
    handle_post(
        "/fakeelasticsearch/name_laundering/_mapping/FeatureCollection",
        post_body='{ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { "properties": { } }, "geometry": { "type": "geo_shape" } } } }',
        contents="{}",
    )

    lyr = ds.CreateLayer("NAME/laundering", srs=ogrtest.srs_wgs84, options=["FID="])
    feat = ogr.Feature(lyr.GetLayerDefn())
    ret = lyr.CreateFeature(feat)
    assert ret == 0
    feat = None

    with gdal.quiet_errors():
        ret = lyr.SyncToDisk()
    assert ret != 0

    feat = ogr.Feature(lyr.GetLayerDefn())
    ret = lyr.CreateFeature(feat)
    assert ret == 0
    feat = None

    handle_post(
        "/fakeelasticsearch/_bulk",
        post_body="""{"index" :{"_index":"name_laundering", "_type":"FeatureCollection"}}
{ "properties": { } }

""",
        contents="{}",
    )
    ret = lyr.SyncToDisk()
    assert ret == 0

    ds = None


###############################################################################
# Test basic read functionality


def test_ogr_elasticsearch_4(es_url, handle_get, handle_post, handle_delete):

    handle_get("/fakeelasticsearch", """{"version":{"number":"2.0.0"}}""")

    with gdal.quiet_errors():
        ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch")
    assert ds is not None

    # Test case where there's no index
    handle_get("""/fakeelasticsearch/_cat/indices?h=i""", "\n")
    ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch")
    assert ds is not None
    assert ds.GetLayerCount() == 0

    # Test opening a layer by name
    handle_get(
        """/fakeelasticsearch/a_layer/_mapping?pretty""",
        """
{
    "a_layer":
    {
        "mappings":
        {
            "FeatureCollection":
            {
                "_meta": {
                    "fid": "my_fid",
                    "geomfields": {
                            "a_geoshape": "LINESTRING"
                    },
                    "fields": {
                            "strlist_field": "StringList",
                            "intlist_field": "IntegerList",
                            "int64list_field": "Integer64List",
                            "doublelist_field": "RealList"
                    }
                },
                "properties":
                {
                    "type": { "type": "string" },
                    "a_geoshape":
                    {
                        "type": "geo_shape",
                    },
                    "a_geopoint":
                    {
                        "properties":
                        {
                            "coordinates":
                            {
                                "type": "geo_point"
                            }
                        }
                    },
                    "my_fid": { "type": "long" },
                    "properties" :
                    {
                        "properties":
                        {
                            "str_field": { "type": "string"},
                            "int_field": { "type": "integer"},
                            "int64_field": { "type": "long"},
                            "double_field": { "type": "double"},
                            "float_field": { "type": "float"},
                            "boolean_field": { "type": "boolean"},
                            "binary_field": { "type": "binary"},
                            "dt_field": { "type": "date"},
                            "date_field": { "type": "date", "format": "yyyy\\/MM\\/dd"},
                            "time_field": { "type": "date", "format": "HH:mm:ss.SSS"},
                            "strlist_field": { "type": "string"},
                            "intlist_field": { "type": "integer"},
                            "int64list_field": { "type": "long"},
                            "doublelist_field": { "type": "double"}
                        }
                    }
                }
            }
        }
    }
}
""",
    )

    ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch")
    assert ds is not None
    lyr = ds.GetLayerByName("a_layer")
    assert lyr is not None
    lyr = ds.GetLayerByName("a_layer")
    assert lyr is not None
    with gdal.quiet_errors():
        lyr = ds.GetLayerByName("not_a_layer")
    assert lyr is None
    ds = None

    # Test LAYER open option
    ds = gdal.OpenEx(f"ES:{es_url}/fakeelasticsearch", open_options=["LAYER=a_layer"])
    assert ds.GetLayerCount() == 1
    ds = None

    with gdal.quiet_errors():
        ds = gdal.OpenEx(
            f"ES:{es_url}/fakeelasticsearch", open_options=["LAYER=not_a_layer"]
        )
    assert ds is None
    ds = None

    # Test GetLayerByName() and GetLayerCount()
    ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch")
    lyr = ds.GetLayerByName("a_layer")
    lyr = ds.GetLayerByName("a_layer")
    assert ds.GetLayerCount() == 1
    ds = None

    # Test GetLayerCount()
    handle_get("""/fakeelasticsearch/_cat/indices?h=i""", "a_layer  \n")

    ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch")
    assert ds is not None
    assert ds.GetLayerCount() == 1
    lyr = ds.GetLayer(0)

    with gdal.quiet_errors():
        lyr_defn = lyr.GetLayerDefn()
    idx = lyr_defn.GetFieldIndex("strlist_field")
    assert lyr_defn.GetFieldDefn(idx).GetType() == ogr.OFTStringList
    idx = lyr_defn.GetGeomFieldIndex("a_geoshape")
    assert lyr_defn.GetGeomFieldDefn(idx).GetType() == ogr.wkbLineString
    assert lyr.GetFIDColumn() == "my_fid"

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_count?pretty""",
        """{
}""",
    )
    with gdal.quiet_errors():
        lyr.GetFeatureCount()

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_count?pretty""",
        """{
    "hits": null
}""",
    )
    with gdal.quiet_errors():
        lyr.GetFeatureCount()

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_count?pretty""",
        """{
    "hits": { "count": null }
}""",
    )
    with gdal.quiet_errors():
        lyr.GetFeatureCount()

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_count?pretty""",
        """{
    "hits":
    {
        "count": 3
    }
}""",
    )
    fc = lyr.GetFeatureCount()
    assert fc == 3

    with gdal.quiet_errors():
        f = lyr.GetNextFeature()
    assert f is None

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{

}""",
    )
    lyr.ResetReading()
    f = lyr.GetNextFeature()
    assert f is None

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
    "hits": null
}""",
    )
    lyr.ResetReading()
    lyr.GetNextFeature()

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
    "hits": { "hits": null }
}""",
    )
    lyr.ResetReading()
    lyr.GetNextFeature()

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
    "hits": { "hits": [ null, {}, { "_source":null } ] }
}""",
    )
    lyr.ResetReading()
    lyr.GetNextFeature()

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
    "_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "my_fid": 5,
                "a_geopoint" : {
                    "type": "Point",
                    "coordinates": [2,49]
                },
                "a_geoshape": {
                    "type": "linestring",
                    "coordinates": [[2,49],[3,50]]
                },
                "properties": {
                    "str_field": "foo",
                    "int_field": 1,
                    "int64_field": 123456789012,
                    "double_field": 1.23,
                    "float_field": 3.45,
                    "boolean_field": true,
                    "binary_field": "ASNGV4mrze8=",
                    "dt_field": "2015\\/08\\/12 12:34:56.789",
                    "date_field": "2015\\/08\\/12",
                    "time_field": "12:34:56.789",
                    "strlist_field": ["foo"],
                    "intlist_field": [1],
                    "int64list_field": [123456789012],
                    "doublelist_field": [1.23]
                }
            },
        }]
    }
}""",
    )
    handle_get(
        """/fakeelasticsearch/_search/scroll?scroll=1m&scroll_id=my_scrollid""",
        "{}",
    )
    handle_delete(
        """/fakeelasticsearch/_search/scroll?scroll_id=my_scrollid""",
        "{}",
    )

    ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch")
    lyr = ds.GetLayer(0)

    assert lyr.GetLayerDefn().GetFieldCount() == 15

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
    "_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "a_geopoint" : {
                    "type": "Point",
                    "coordinates": [2,49]
                },
                "a_geoshape": {
                    "type": "linestring",
                    "coordinates": [[2,49],[3,50]]
                },
                "my_fid": 5,
                "properties": {
                    "str_field": "foo",
                    "int_field": 1,
                    "int64_field": 123456789012,
                    "double_field": 1.23,
                    "float_field": 3.45,
                    "boolean_field": true,
                    "binary_field": "ASNGV4mrze8=",
                    "dt_field": "2015\\/08\\/12 12:34:56.789",
                    "date_field": "2015\\/08\\/12",
                    "time_field": "12:34:56.789",
                    "strlist_field": ["foo"],
                    "intlist_field": [1],
                    "int64list_field": [123456789012],
                    "doublelist_field": [1.23]
                }
            },
        },
        {
            "_source": {
                "type": "Feature",
                "properties": {
                    "non_existing": "foo"
                }
            },
        }
        ]
    }
}""",
    )

    lyr.ResetReading()
    f = lyr.GetNextFeature()
    assert f is not None
    if (
        f.GetFID() != 5
        or f["_id"] != "my_id"
        or f["str_field"] != "foo"
        or f["int_field"] != 1
        or f["int64_field"] != 123456789012
        or f["double_field"] != 1.23
        or f["float_field"] != 3.45
        or f["boolean_field"] != 1
        or f["binary_field"] != "0123465789ABCDEF"
        or f["dt_field"] != "2015/08/12 12:34:56.789"
        or f["date_field"] != "2015/08/12"
        or f["time_field"] != "12:34:56.789"
        or f["strlist_field"] != ["foo"]
        or f["intlist_field"] != [1]
        or f["int64list_field"] != [123456789012]
        or f["doublelist_field"] != [1.23]
        or f["a_geopoint"].ExportToWkt() != "POINT (2 49)"
        or f["a_geoshape"].ExportToWkt() != "LINESTRING (2 49,3 50)"
    ):
        f.DumpReadable()
        pytest.fail()

    lyr.ResetReading()
    lyr.GetNextFeature()
    f = lyr.GetNextFeature()
    assert f is not None

    handle_get(
        """/fakeelasticsearch/_search/scroll?scroll=1m&scroll_id=my_scrollid""",
        """{
    "hits":
    {
        "hits":[
        {
            "_source": {
                "type": "Feature",
                "properties": {
                    "int_field": 2,
                }
            },
        }
        ]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f["int_field"] == 2

    handle_get(
        """/fakeelasticsearch/_search/scroll?scroll=1m&scroll_id=my_scrollid""",
        """{
    "hits":
    {
        "hits":[]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is None
    f = lyr.GetNextFeature()
    assert f is None

    lyr.SetSpatialFilterRect(1, 48, 3, 50)
    lyr.ResetReading()
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body='{ "query": { "constant_score" : { "filter": { "geo_shape": { "a_geoshape": { "shape": { "type": "envelope", "coordinates": [ [ 1.0, 50.0 ], [ 3.0, 48.0 ] ] } } } } } } }',
        contents="""{
    "hits":
    {
        "hits":[
        {
            "_source": {
                "type": "Feature",
                "a_geoshape" : {
                    "type": "Point",
                    "coordinates": [2,49]
                },
                "properties": {
                    "int_field": 3,
                }
            },
        }
        ]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f["int_field"] == 3

    lyr.SetSpatialFilterRect(1, 1, 48, 3, 50)
    lyr.ResetReading()
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body='{ "query": { "constant_score" : { "filter": { "geo_bounding_box": { "a_geopoint.coordinates": { "top_left": { "lat": 50.0, "lon": 1.0 }, "bottom_right": { "lat": 48.0, "lon": 3.0 } } } } } } }',
        contents="""{
    "hits":
    {
        "hits":[
        {
            "_source": {
                "type": "Feature",
                "a_geopoint" : {
                    "type": "Point",
                    "coordinates": [2,49]
                },
                "properties": {
                    "int_field": 4,
                }
            },
        }
        ]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f["int_field"] == 4

    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?pretty""",
        post_body='{ "size": 0, "query": { "constant_score" : { "filter": { "geo_bounding_box": { "a_geopoint.coordinates": { "top_left": { "lat": 50.0, "lon": 1.0 }, "bottom_right": { "lat": 48.0, "lon": 3.0 } } } } } } }',
        contents="""{
    "hits":
    {
        "total": 10
    }
}""",
    )
    fc = lyr.GetFeatureCount()
    assert fc == 10

    lyr.SetSpatialFilter(None)
    lyr.SetSpatialFilterRect(-180, -90, 180, 90)
    with gdal.quiet_errors():
        lyr.SetSpatialFilter(-1, None)
        lyr.SetSpatialFilter(2, None)

    lyr.SetAttributeFilter("{ 'FOO' : 'BAR' }")
    lyr.ResetReading()
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="{ 'FOO' : 'BAR' }",
        contents="""{
    "_scroll_id": "invalid",
    "hits":
    {
        "hits":[
        {
            "_source": {
                "type": "Feature",
                "a_geoshape" : {
                    "type": "Point",
                    "coordinates": [2,49]
                },
                "properties": {
                    "int_field": 5,
                }
            },
        }
        ]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f["int_field"] == 5

    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?pretty""",
        post_body="""{ "size": 0,  'FOO' : 'BAR' }""",
        contents="""{
    "hits":
    {
        "total": 1234
    }
}""",
    )
    assert lyr.GetFeatureCount() == 1234

    lyr.SetAttributeFilter(None)

    sql_lyr = ds.ExecuteSQL("{ 'FOO' : 'BAR' }", dialect="ES")
    handle_post(
        """/fakeelasticsearch/_search?scroll=1m&size=100""",
        post_body="{ 'FOO' : 'BAR' }",
        contents="""{
    "hits":
    {
        "hits":[
        {
            "_index": "some_layer",
            "_type": "some_type",
            "_source": {
                "some_field": 5
            },
        }
        ]
    }
}""",
    )
    handle_get(
        """/fakeelasticsearch/some_layer/_mapping/some_type?pretty""",
        b"""
{
    "some_layer":
    {
        "mappings":
        {
            "some_type":
            {
                "properties":
                {
                    "some_field": { "type": "string"}
                }
            }
        }
    }
}
""",
    )
    f = sql_lyr.GetNextFeature()
    if f["some_field"] != "5":
        f.DumpReadable()
        pytest.fail()
    ds.ReleaseResultSet(sql_lyr)

    # Invalid index
    with gdal.quiet_errors():
        bbox = lyr.GetExtent(geom_field=-1)

    # geo_shape
    bbox = lyr.GetExtent(geom_field=0)

    # Invalid index
    with gdal.quiet_errors():
        bbox = lyr.GetExtent(geom_field=2)

    # No response
    with gdal.quiet_errors():
        bbox = lyr.GetExtent(geom_field=1)

    # Invalid response
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?search_type=count&pretty""",
        post_body='{ "aggs" : { "bbox" : { "geo_bounds" : { "field" : "a_geopoint.coordinates" } } } }',
        contents="""{
  "aggregations" : {
    "bbox" : {
      "bounds" : {
        "top_left" : {

        },
        "bottom_right" : {

        }
      }
    }
  }
}""",
    )

    with gdal.quiet_errors():
        bbox = lyr.GetExtent(geom_field=1)

    # Valid response
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?pretty""",
        post_body='{ "size": 0, "aggs" : { "bbox" : { "geo_bounds" : { "field" : "a_geopoint.coordinates" } } } }',
        contents="""{
  "aggregations" : {
    "bbox" : {
      "bounds" : {
        "top_left" : {
          "lat" : 10,
          "lon" : 1
        },
        "bottom_right" : {
          "lat" : 9,
          "lon" : 2
        }
      }
    }
  }
}""",
    )
    bbox = lyr.GetExtent(geom_field=1)
    assert bbox == (1.0, 2.0, 9.0, 10.0)

    # Operations not available in read-only mode
    with gdal.quiet_errors():
        ret = lyr.CreateField(ogr.FieldDefn("foo", ogr.OFTString))
    assert ret != 0

    with gdal.quiet_errors():
        ret = lyr.CreateGeomField(ogr.GeomFieldDefn("shape", ogr.wkbPoint))
    assert ret != 0

    with gdal.quiet_errors():
        ret = lyr.CreateFeature(ogr.Feature(lyr.GetLayerDefn()))
    assert ret != 0

    lyr.ResetReading()
    with gdal.quiet_errors():
        ret = lyr.SetFeature(lyr.GetNextFeature())
    assert ret != 0

    lyr.ResetReading()
    with gdal.quiet_errors():
        ret = lyr.UpsertFeature(lyr.GetNextFeature())
    assert ret != 0

    with gdal.quiet_errors():
        lyr = ds.CreateLayer("will_not_work")
    assert lyr is None

    with gdal.quiet_errors():
        ret = ds.DeleteLayer(0)
    assert ret != 0


###############################################################################
# Write documents with non geojson structure


def test_ogr_elasticsearch_5(es_url, handle_get, handle_put, handle_post):

    handle_get("/fakeelasticsearch", """{"version":{"number":"2.0.0"}}""")

    ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch", update=1)

    handle_get(
        "/fakeelasticsearch/_stats",
        b"""{"_shards":{"total":0,"successful":0,"failed":0},"indices":{}}""",
    )

    handle_put("/fakeelasticsearch/non_geojson", "")
    handle_post(
        "/fakeelasticsearch/non_geojson/_mapping/my_mapping",
        post_body='{ "my_mapping": { "properties": { "str": { "type": "string", "store": "yes" }, "geometry": { "type": "geo_shape" } }, "_meta": { "fid": "ogc_fid" } } }',
        contents="{}",
    )

    lyr = ds.CreateLayer(
        "non_geojson",
        srs=ogrtest.srs_wgs84,
        options=["MAPPING_NAME=my_mapping", "BULK_INSERT=NO", "STORE_FIELDS=YES"],
    )
    lyr.CreateField(ogr.FieldDefn("str", ogr.OFTString))
    feat = ogr.Feature(lyr.GetLayerDefn())
    feat.SetFID(5)
    feat["str"] = "foo"

    handle_post(
        "/fakeelasticsearch/non_geojson/my_mapping",
        post_body='{ "ogc_fid": 5, "str": "foo" }',
        contents="{}",
    )
    ret = lyr.CreateFeature(feat)
    assert ret == 0
    feat = None

    ds = None

    handle_get("""/fakeelasticsearch/_cat/indices?h=i""", "non_geojson\n")
    handle_get(
        """/fakeelasticsearch/non_geojson/_mapping?pretty""",
        """
{
    "non_geojson":
    {
        "mappings":
        {
            "my_mapping":
            {
                "properties":
                {
                    "a_geoshape":
                    {
                        "type": "geo_shape",
                    },
                    "a_geopoint":
                    {
                        "properties":
                        {
                            "type": "string",
                            "coordinates":
                            {
                                "type": "geo_point"
                            }
                        }
                    },
                    "another_geopoint": { "type": "geo_point" },
                    "str_field": { "type": "string"},
                    "superobject": {
                        "properties": {
                            "subfield": { "type": "string" },
                            "subobject": {
                                "properties": {
                                    "subfield": { "type": "string" }
                                }
                            },
                            "another_geoshape": { "type": "geo_shape" }
                        }
                    }
                }
            }
        }
    }
}
""",
    )
    ds = gdal.OpenEx(
        f"ES:{es_url}/fakeelasticsearch",
        gdal.OF_UPDATE,
        open_options=["BULK_INSERT=NO"],
    )
    lyr = ds.GetLayer(0)

    handle_get(
        """/fakeelasticsearch/non_geojson/my_mapping/_search?scroll=1m&size=100""",
        """{
    "hits":
    {
        "hits":[
        {
            "_source": {
                "a_geopoint" : {
                    "type": "Point",
                    "coordinates": [2,49]
                },
                "a_geoshape": {
                    "type": "linestring",
                    "coordinates": [[2,49],[3,50]]
                },
                "another_geopoint": "49.5,2.5",
                "str_field": "foo",
                "superobject": {
                    "subfield": 5,
                    "subobject":
                    {
                        "subfield": "foo",
                        "another_subfield": 6
                    },
                    "another_geoshape": {
                        "type": "point",
                        "coordinates": [3,50]
                    },
                    "another_geoshape2": {
                        "type": "point",
                        "coordinates": [2,50]
                    }
                }
            }
        },
        {
            "_source": {
                "another_field": "foo",
                "another_geopoint": { "lat": 49.1, "lon": 2.1 }
            }
        },
        {
            "_source": {
                "another_geopoint": "49.2,2.2"
            }
        },
        {
            "_source": {"""
        +
        # "this is the geohash format",
        """                "another_geopoint": "u09qv80meqh16ve02equ"
            }
        }]
    }
}""",
    )
    index = lyr.GetLayerDefn().GetFieldIndex("another_field")
    assert index >= 0
    f = lyr.GetNextFeature()
    if (
        f["str_field"] != "foo"
        or f["superobject.subfield"] != "5"
        or f["superobject.subobject.subfield"] != "foo"
        or f["superobject.subobject.another_subfield"] != 6
        or f["a_geopoint"].ExportToWkt() != "POINT (2 49)"
        or f["another_geopoint"].ExportToWkt() != "POINT (2.5 49.5)"
        or f["a_geoshape"].ExportToWkt() != "LINESTRING (2 49,3 50)"
        or f["superobject.another_geoshape"].ExportToWkt() != "POINT (3 50)"
        or f["superobject.another_geoshape2"].ExportToWkt() != "POINT (2 50)"
    ):
        f.DumpReadable()
        pytest.fail()

    f["_id"] = "my_id"
    handle_post(
        """/fakeelasticsearch/non_geojson/my_mapping/my_id""",
        post_body='{ "a_geoshape": { "type": "linestring", "coordinates": [ [ 2.0, 49.0 ], [ 3.0, 50.0 ] ] }, "a_geopoint": { "type": "Point", "coordinates": [ 2.0, 49.0 ] }, "another_geopoint": [ 2.5, 49.5 ], "superobject": { "another_geoshape": { "type": "point", "coordinates": [ 3.0, 50.0 ] }, "another_geoshape2": { "type": "point", "coordinates": [ 2.0, 50.0 ] }, "subfield": "5", "subobject": { "subfield": "foo", "another_subfield": 6 } }, "str_field": "foo" }',
        contents="{}",
    )
    ret = lyr.SetFeature(f)
    assert ret == 0

    f = lyr.GetNextFeature()
    if f["another_geopoint"].ExportToWkt() != "POINT (2.1 49.1)":
        f.DumpReadable()
        pytest.fail()

    f = lyr.GetNextFeature()
    if f["another_geopoint"].ExportToWkt() != "POINT (2.2 49.2)":
        f.DumpReadable()
        pytest.fail()

    # Test geohash
    f = lyr.GetNextFeature()
    ogrtest.check_feature_geometry(f["another_geopoint"], "POINT (2 49)")

    f = None
    lyr.CreateField(ogr.FieldDefn("superobject.subfield2", ogr.OFTString))
    with gdal.quiet_errors():
        lyr.CreateGeomField(
            ogr.GeomFieldDefn("superobject.another_geoshape3", ogr.wkbPoint)
        )
    f = ogr.Feature(lyr.GetLayerDefn())
    f["superobject.subfield2"] = "foo"
    f["superobject.another_geoshape3"] = ogr.CreateGeometryFromWkt("POINT (3 50)")
    handle_post(
        """/fakeelasticsearch/non_geojson/_mapping/my_mapping""",
        post_body='{ "my_mapping": { "properties": { "str_field": { "type": "string" }, "superobject": { "properties": { "subfield": { "type": "string" }, "subobject": { "properties": { "subfield": { "type": "string" }, "another_subfield": { "type": "integer" } } }, "subfield2": { "type": "string" }, "another_geoshape": { "type": "geo_shape" }, "another_geoshape2": { "type": "geo_shape" }, "another_geoshape3": { "properties": { "type": { "type": "string" }, "coordinates": { "type": "geo_point" } } } } }, "another_field": { "type": "string" }, "a_geoshape": { "type": "geo_shape" }, "a_geopoint": { "properties": { "type": { "type": "string" }, "coordinates": { "type": "geo_point" } } }, "another_geopoint": { "type": "geo_point" } }, "_meta": { "geomfields": { "superobject.another_geoshape2": "Point" } } } }',
        contents="{}",
    )
    handle_post(
        """/fakeelasticsearch/non_geojson/my_mapping""",
        post_body="""{ "superobject": { "another_geoshape3": { "type": "Point", "coordinates": [ 3.0, 50.0 ] }, "subfield2": "foo" } }""",
        contents="{}",
    )
    handle_get("""/fakeelasticsearch/non_geojson/my_mapping/_count?pretty""", "{}")
    lyr.CreateFeature(f)

    ds = gdal.OpenEx(
        f"ES:{es_url}/fakeelasticsearch",
        open_options=["FEATURE_COUNT_TO_ESTABLISH_FEATURE_DEFN=0"],
    )
    lyr = ds.GetLayer(0)
    f = lyr.GetNextFeature()
    if (
        f["str_field"] != "foo"
        or f["superobject.subfield"] != "5"
        or f["a_geopoint"].ExportToWkt() != "POINT (2 49)"
        or f["a_geoshape"].ExportToWkt() != "LINESTRING (2 49,3 50)"
        or f["superobject.another_geoshape"].ExportToWkt() != "POINT (3 50)"
    ):
        f.DumpReadable()
        pytest.fail()

    ds = gdal.OpenEx(
        f"ES:{es_url}/fakeelasticsearch",
        open_options=[
            "FEATURE_COUNT_TO_ESTABLISH_FEATURE_DEFN=0",
            "FLATTEN_NESTED_ATTRIBUTES=FALSE",
        ],
    )
    lyr = ds.GetLayer(0)
    index = lyr.GetLayerDefn().GetFieldIndex("another_field")
    assert index < 0
    f = lyr.GetNextFeature()
    if (
        f["str_field"] != "foo"
        or f["superobject"]
        != '{ "subfield": 5, "subobject": { "subfield": "foo", "another_subfield": 6 }, "another_geoshape": { "type": "point", "coordinates": [ 3, 50 ] }, "another_geoshape2": { "type": "point", "coordinates": [ 2, 50 ] } }'
        or f["a_geopoint"].ExportToWkt() != "POINT (2 49)"
        or f["a_geoshape"].ExportToWkt() != "LINESTRING (2 49,3 50)"
    ):
        f.DumpReadable()
        pytest.fail()

    ds = gdal.OpenEx(
        f"ES:{es_url}/fakeelasticsearch",
        gdal.OF_UPDATE,
        open_options=["JSON_FIELD=YES"],
    )
    lyr = ds.GetLayer(0)
    f = lyr.GetNextFeature()
    if (
        f["str_field"] != "foo"
        or f["superobject.subfield"] != "5"
        or f["_json"].find("{") != 0
        or f["a_geopoint"].ExportToWkt() != "POINT (2 49)"
        or f["a_geoshape"].ExportToWkt() != "LINESTRING (2 49,3 50)"
        or f["superobject.another_geoshape"].ExportToWkt() != "POINT (3 50)"
    ):
        f.DumpReadable()
        pytest.fail()

    f["_id"] = "my_id"
    f["_json"] = '{ "foo": "bar" }'
    handle_post(
        """/fakeelasticsearch/non_geojson/my_mapping/my_id""",
        post_body="""{ "foo": "bar" }""",
        contents="{}",
    )
    ret = lyr.SetFeature(f)
    assert ret == 0


###############################################################################
# Test reading circle and envelope geometries


def test_ogr_elasticsearch_6(es_url, handle_get):

    handle_get("/fakeelasticsearch", """{"version":{"number":"2.0.0"}}""")

    handle_get("""/fakeelasticsearch/_cat/indices?h=i""", "non_standard_geometries\n")
    handle_get(
        """/fakeelasticsearch/non_standard_geometries/_mapping?pretty""",
        """
{
    "non_standard_geometries":
    {
        "mappings":
        {
            "my_mapping":
            {
                "properties":
                {
                    "geometry":
                    {
                        "type": "geo_shape",
                    }
                }
            }
        }
    }
}
""",
    )
    ds = gdal.OpenEx(f"ES:{es_url}/fakeelasticsearch")
    lyr = ds.GetLayer(0)

    handle_get(
        """/fakeelasticsearch/non_standard_geometries/my_mapping/_search?scroll=1m&size=100""",
        """{
    "hits":
    {
        "hits":[
        {
            "_source": {
                "geometry": {
                    "type": "envelope",
                    "coordinates": [[2,49],[3,50]]
                }
            }
        },
        {
            "_source": {
                "geometry": {
                    "type": "circle",
                    "coordinates": [2,49],
                    "radius": 100
                }
            }
        },
        {
            "_source": {
                "geometry": {
                    "type": "circle",
                    "coordinates": [2,49],
                    "radius": "100m"
                }
            }
        },
        {
            "_source": {
                "geometry": {
                    "type": "circle",
                    "coordinates": [2,49],
                    "radius": "0.1km"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    if f["geometry"].ExportToWkt() != "POLYGON ((2 49,3 49,3 50,2 50,2 49))":
        f.DumpReadable()
        pytest.fail()

    f = lyr.GetNextFeature()
    ref_txt = f["geometry"].ExportToWkt()
    if not ref_txt.startswith("POLYGON (("):
        f.DumpReadable()
        pytest.fail()

    f = lyr.GetNextFeature()
    if f["geometry"].ExportToWkt() != ref_txt:
        f.DumpReadable()
        pytest.fail()

    f = lyr.GetNextFeature()
    if f["geometry"].ExportToWkt() != ref_txt:
        f.DumpReadable()
        pytest.fail()


###############################################################################
# Test WRITE_MAPPING option


def test_ogr_elasticsearch_7(tmp_vsimem, es_url, handle_get, handle_put):

    handle_get("/fakeelasticsearch", """{"version":{"number":"2.0.0"}}""")

    handle_get(
        "/fakeelasticsearch/_stats",
        """{"_shards":{"total":0,"successful":0,"failed":0},"indices":{}}""",
    )

    ds = ogrtest.elasticsearch_drv.CreateDataSource(f"{es_url}/fakeelasticsearch")

    handle_put("/fakeelasticsearch/test_write_mapping", "{}")
    lyr = ds.CreateLayer(
        "test_write_mapping",
        srs=ogrtest.srs_wgs84,
        options=[f"WRITE_MAPPING={tmp_vsimem}/map.txt", "FID="],
    )
    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    ds = None

    f = gdal.VSIFOpenL(tmp_vsimem / "map.txt", "rb")
    assert f is not None
    data = gdal.VSIFReadL(1, 10000, f).decode("ascii")
    gdal.VSIFCloseL(f)

    assert (
        data
        == '{ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { "properties": { } }, "geometry": { "type": "geo_shape" } } } }'
    )


###############################################################################
# Test SRS support


def test_ogr_elasticsearch_8(es_url, handle_get, handle_put, handle_post):

    handle_get("/fakeelasticsearch", """{"version":{"number":"2.0.0"}}""")

    handle_get(
        "/fakeelasticsearch/_stats",
        """{"_shards":{"total":0,"successful":0,"failed":0},"indices":{}}""",
    )

    ds = ogrtest.elasticsearch_drv.CreateDataSource(f"{es_url}/fakeelasticsearch")

    # No SRS
    handle_put("/fakeelasticsearch/no_srs", "{}")
    # Will emit a warning
    gdal.ErrorReset()
    with gdal.quiet_errors():
        lyr = ds.CreateLayer("no_srs")
    assert gdal.GetLastErrorType() == gdal.CE_Warning, "warning expected"
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (-100 -200)"))
    handle_post(
        """/fakeelasticsearch/no_srs/_mapping/FeatureCollection""",
        post_body="""{ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { "properties": { } }, "geometry": { "type": "geo_shape" } }, "_meta": { "fid": "ogc_fid" } } }""",
        contents="{}",
    )
    handle_post(
        """/fakeelasticsearch/_bulk""",
        post_body="""{"index" :{"_index":"no_srs", "_type":"FeatureCollection"}}
{ "ogc_fid": 1, "geometry": { "type": "point", "coordinates": [ -100.0, -200.0 ] }, "type": "Feature", "properties": { } }

""",
        contents="{}",
    )
    # Will emit a warning
    gdal.ErrorReset()
    with gdal.quiet_errors():
        ret = lyr.CreateFeature(f)
    assert gdal.GetLastErrorType() == gdal.CE_Warning, "warning expected"
    assert ret == 0

    # Non EPSG-4326 SRS
    other_srs = osr.SpatialReference()
    other_srs.ImportFromEPSG(32631)
    handle_put("/fakeelasticsearch/other_srs", b"{}")
    lyr = ds.CreateLayer("other_srs", srs=other_srs)
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (500000 0)"))
    handle_post(
        """/fakeelasticsearch/other_srs/_mapping/FeatureCollection""",
        post_body="""{ "FeatureCollection": { "properties": { "type": { "type": "string" }, "properties": { "properties": { } }, "geometry": { "type": "geo_shape" } }, "_meta": { "fid": "ogc_fid" } } }""",
        contents="{}",
    )
    handle_post(
        """/fakeelasticsearch/_bulk""",
        post_body="""{"index" :{"_index":"other_srs", "_type":"FeatureCollection"}}
{ "ogc_fid": 1, "geometry": { "type": "point", "coordinates": [ 3.0, 0.0 ] }, "type": "Feature", "properties": { } }

""",
        contents="{}",
    )
    ret = lyr.CreateFeature(f)
    assert ret == 0


###############################################################################
# Test Elasticsearch 5.X


def test_ogr_elasticsearch_9(es_url, handle_get, handle_post, handle_delete):

    handle_get("/fakeelasticsearch", """{"version":{"number":"5.0.0"}}""")

    handle_get("""/fakeelasticsearch/_cat/indices?h=i""", "a_layer  \n")
    handle_get(
        """/fakeelasticsearch/a_layer/_mapping?pretty""",
        """
{
    "a_layer":
    {
        "mappings":
        {
            "FeatureCollection":
            {
                "properties":
                {
                    "type": { "type": "text" },
                    "a_geoshape":
                    {
                        "type": "geo_shape",
                    },
                    "properties" :
                    {
                        "properties":
                        {
                            "str_field": { "type": "text"}
                        }
                    }
                }
            }
        }
    }
}
""",
    )

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "a_geoshape": {
                        "type": "point",
                        "coordinates": [2.5,49.5]
                    },
                    "str_field": "foo"
                }
            }
        }]
    }
}""",
    )

    handle_get(
        """/fakeelasticsearch/_search/scroll?scroll=1m&scroll_id=my_scrollid""",
        """{}""",
    )
    handle_delete(
        "/fakeelasticsearch/_search/scroll?scroll_id=my_scrollid",
        "{}",
    )

    ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch")
    lyr = ds.GetLayer(0)
    lyr.SetSpatialFilterRect(2, 49, 3, 50)

    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_count?pretty""",
        post_body="""{ "query": { "constant_score" : { "filter": { "geo_shape": { "a_geoshape": { "shape": { "type": "envelope", "coordinates": [ [ 2.0, 50.0 ], [ 3.0, 49.0 ] ] } } } } } } }""",
        contents="""{
  "count" : 2
}""",
    )

    count = lyr.GetFeatureCount()
    assert count == 2

    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "geo_shape": { "a_geoshape": { "shape": { "type": "envelope", "coordinates": [ [ 2.0, 50.0 ], [ 3.0, 49.0 ] ] } } } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "a_geoshape": {
                    "type": "point",
                    "coordinates": [2.5,49.5]
                },
                "properties": {
                    "str_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None


###############################################################################
# Test SQL


def test_ogr_elasticsearch_10(
    handle_get, handle_post, handle_put, handle_delete, es_url
):

    handle_get("/fakeelasticsearch", """{"version":{"number":"5.0.0"}}""")

    handle_get("""/fakeelasticsearch/_cat/indices?h=i""", "a_layer  \n")
    handle_get(
        """/fakeelasticsearch/a_layer/_mapping?pretty""",
        """
{
    "a_layer":
    {
        "mappings":
        {
            "FeatureCollection":
            {
                "properties":
                {
                    "type": { "type": "text" },
                    "a_geoshape":
                    {
                        "type": "geo_shape",
                    },
                    "properties" :
                    {
                        "properties":
                        {
                            "text_field": { "type": "text"},
                            "text_field_with_raw": { "type": "text", "fields" : { "raw" : { "type": "keyword" } } },
                            "keyword_field": { "type": "keyword"},
                            "int_field": { "type": "integer"},
                            "long_field": { "type": "long"},
                            "double_field": { "type": "double"},
                            "dt_field": { "type": "date"},
                            "date_field": { "type": "date", "format": "yyyy\\/MM\\/dd"},
                            "time_field": { "type": "date", "format": "HH:mm:ss.SSS"},
                        }
                    }
                }
            }
        }
    }
}
""",
    )

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{}""",
    )

    handle_delete(
        "/fakeelasticsearch/_search/scroll?scroll_id=my_scrollid",
        "{}",
    )

    ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch")
    lyr = ds.GetLayer(0)
    lyr.SetAttributeFilter("keyword_field = 'foo' AND keyword_field IS NOT NULL")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "bool": { "must": [ { "term": { "properties.keyword_field": "foo" } }, { "exists": { "field": "properties.keyword_field" } } ] } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "keyword_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("text_field = 'foo'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "match": { "properties.text_field": "foo" } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "text_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("CAST(text_field AS CHARACTER) = 'foo_cast'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "match": { "properties.text_field": "foo_cast" } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "text_field": "foo_cast"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("text_field_with_raw = 'foo'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "term": { "properties.text_field_with_raw.raw": "foo" } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "text_field_with_raw": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("\"_id\" = 'my_id2'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "ids": { "values": [ "my_id2" ] } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id2",
            "_source": {
                "type": "Feature",
                "properties": {
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("keyword_field != 'foo'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "bool": { "must_not": { "term": { "properties.keyword_field": "foo" } } } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "keyword_field": "bar"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("keyword_field IS NULL")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "bool": { "must_not": { "exists": { "field": "properties.keyword_field" } } } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("keyword_field BETWEEN 'bar' AND 'foo'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "range": { "properties.keyword_field": { "gte": "bar", "lte": "foo" } } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "keyword_field": "baz"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("keyword_field IN ('foo', 'bar')")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "terms": { "properties.keyword_field": [ "foo", "bar" ] } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "keyword_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("text_field IN ('foo', 'bar')")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "bool": { "should": [ { "match": { "properties.text_field": "foo" } }, { "match": { "properties.text_field": "bar" } } ] } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "text_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("text_field_with_raw IN ('foo', 'bar')")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "terms": { "properties.text_field_with_raw.raw": [ "foo", "bar" ] } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "text_field_with_raw": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("\"_id\" IN ('my_id', 'bar')")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "ids": { "values": [ "my_id", "bar" ] } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "text_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter(
        "int_field >= 2 OR long_field >= 9876543210 OR double_field <= 3.123456"
    )
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "bool": { "should": [ { "bool": { "should": [ { "range": { "properties.int_field": { "gte": 2 } } }, { "range": { "properties.long_field": { "gte": 9876543210 } } } ] } }, { "range": { "properties.double_field": { "lte": 3.123456 } } } ] } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "double_field": 3,
                    "int_field": 2,
                    "long_field": 9876543210
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("dt_field > '2016/01/01 12:34:56.123'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "range": { "properties.dt_field": { "gt": "2016\\/01\\/01 12:34:56.123" } } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "dt_field": '2016/01/01 12:34:56.124'
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("NOT dt_field < '2016/01/01 12:34:56.123'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "bool": { "must_not": { "range": { "properties.dt_field": { "lt": "2016\\/01\\/01 12:34:56.123" } } } } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "dt_field": '2016/01/01 12:34:56.123'
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("keyword_field LIKE '_o%'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "wildcard": { "properties.keyword_field": "?o*" } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "keyword_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    # Evaluated client-side since the pattern uses ? or *
    lyr.SetAttributeFilter("text_field LIKE '?*'")
    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "text_field": "?*"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    # Evaluated client-side since the field is analyzed
    lyr.SetAttributeFilter("text_field LIKE '_Z%'")
    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "text_field": "fZo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("text_field_with_raw LIKE '_xo%' ESCAPE 'x'")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "wildcard": { "properties.text_field_with_raw.raw": "?o*" } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "text_field_with_raw": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("keyword_field = 'foo' AND 1 = 1")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "term": { "properties.keyword_field": "foo" } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "keyword_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("1 = 1 AND keyword_field = 'foo'")
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("keyword_field = 'bar' OR 1 = 0")
    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "keyword_field": "bar"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    lyr.SetAttributeFilter("keyword_field = 'foo2'")
    lyr.SetSpatialFilterRect(2, 49, 2, 49)
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "bool" : { "must" : [{ "geo_shape": { "a_geoshape": { "shape": { "type": "envelope", "coordinates": [ [ 2.0, 49.0 ], [ 2.0, 49.0 ] ] } } } }, { "term": { "properties.keyword_field": "foo2" } }] } } } } }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "a_geoshape": {
                    "type": "point",
                    "coordinates": [2.0,49.0]
                },
                "properties": {
                    "keyword_field": "foo2"
                }
            }
        }]
    }
}""",
    )
    f = lyr.GetNextFeature()
    assert f is not None

    # SQL with WHERE
    sql_lyr = ds.ExecuteSQL("SELECT * FROM a_layer WHERE keyword_field = 'foo'")
    f = sql_lyr.GetNextFeature()
    assert f is not None
    ds.ReleaseResultSet(sql_lyr)

    # SQL with WHERE and ORDER BY
    sql_lyr = ds.ExecuteSQL(
        "SELECT * FROM a_layer WHERE keyword_field = 'foo' ORDER BY keyword_field, int_field DESC, \"_id\""
    )
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "query": { "constant_score" : { "filter": { "term": { "properties.keyword_field": "foo" } } } }, "sort" : [ { "properties.keyword_field": { "order": "asc" } }, { "properties.int_field": { "order": "desc" } }, { "_uid": { "order": "asc" } } ] }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "keyword_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = sql_lyr.GetNextFeature()
    assert f is not None
    ds.ReleaseResultSet(sql_lyr)

    # SQL with ORDER BY only
    sql_lyr = ds.ExecuteSQL("SELECT * FROM a_layer ORDER BY keyword_field")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "sort": [ { "properties.keyword_field": { "order": "asc" } } ] }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "keyword_field": "foo"
                }
            }
        }]
    }
}""",
    )
    f = sql_lyr.GetNextFeature()
    assert f is not None
    ds.ReleaseResultSet(sql_lyr)

    # SQL with ORDER BY on a text field with a raw sub-field
    sql_lyr = ds.ExecuteSQL("SELECT * FROM a_layer ORDER BY text_field_with_raw")
    handle_post(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        post_body="""{ "sort": [ { "properties.text_field_with_raw.raw": { "order": "asc" } } ] }""",
        contents="""{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                }
            }
        }]
    }
}""",
    )
    f = sql_lyr.GetNextFeature()
    assert f is not None
    ds.ReleaseResultSet(sql_lyr)


###############################################################################
# Test isnull and unset


def test_ogr_elasticsearch_11(handle_get, handle_post, handle_delete, es_url):

    handle_get("/fakeelasticsearch", """{"version":{"number":"5.0.0"}}""")

    handle_get("""/fakeelasticsearch/_cat/indices?h=i""", "a_layer  \n")
    handle_get(
        """/fakeelasticsearch/a_layer/_mapping?pretty""",
        """
{
    "a_layer":
    {
        "mappings":
        {
            "FeatureCollection":
            {
                "properties":
                {
                    "type": { "type": "text" },
                    "properties" :
                    {
                        "properties":
                        {
                            "str_field": { "type": "text"}
                        }
                    }
                }
            }
        }
    }
}
""",
    )

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{}""",
    )

    handle_delete(
        "/fakeelasticsearch/_search/scroll?scroll_id=my_scrollid",
        "{}",
    )

    ds = ogr.Open(f"ES:{es_url}/fakeelasticsearch", update=1)
    lyr = ds.GetLayer(0)
    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_search?scroll=1m&size=100""",
        """{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "str_field": "foo"
                }
            }
        },
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                    "str_field": null
                }
            }
        },
        {
            "_id": "my_id",
            "_source": {
                "type": "Feature",
                "properties": {
                }
            }
        }
        ]
    }
}""",
    )

    handle_get(
        "/fakeelasticsearch/_search/scroll?scroll=1m&scroll_id=my_scrollid", "{}"
    )

    f = lyr.GetNextFeature()
    if f["str_field"] != "foo":
        f.DumpReadable()
        pytest.fail()

    f = lyr.GetNextFeature()
    if f["str_field"] is not None:
        f.DumpReadable()
        pytest.fail()

    f = lyr.GetNextFeature()
    if f.IsFieldSet("str_field"):
        f.DumpReadable()
        pytest.fail()

    handle_get(
        """/fakeelasticsearch/a_layer/FeatureCollection/_count?pretty""",
        """{
    "hits":
    {
        "count": 0
    }
}""",
    )

    handle_post(
        """/fakeelasticsearch/_bulk""",
        post_body="""{"index" :{"_index":"a_layer", "_type":"FeatureCollection"}}
{ "properties": { "str_field": null } }

{"index" :{"_index":"a_layer", "_type":"FeatureCollection"}}
{ "properties": { } }

""",
        contents="{}",
    )

    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetFieldNull("str_field")
    ret = lyr.CreateFeature(f)
    assert ret == 0
    f = None

    f = ogr.Feature(lyr.GetLayerDefn())
    ret = lyr.CreateFeature(f)
    assert ret == 0
    f = None
    assert lyr.SyncToDisk() == 0


###############################################################################
# Test Elasticsearch 7.x (ignore MAPPING_NAME)


def test_ogr_elasticsearch_12(tmp_vsimem, es_url, handle_get, handle_put):

    handle_get("/fakeelasticsearch", """{"version":{"number":"7.0.0"}}""")

    ds = ogrtest.elasticsearch_drv.CreateDataSource(f"{es_url}/fakeelasticsearch")
    assert ds is not None

    handle_put("/fakeelasticsearch/foo", "{}")
    lyr = ds.CreateLayer(
        "foo",
        srs=ogrtest.srs_wgs84,
        options=[f"WRITE_MAPPING={tmp_vsimem}/map.txt", "FID="],
    )
    assert lyr is not None
    f = ogr.Feature(lyr.GetLayerDefn())
    lyr.CreateFeature(f)
    ds = None

    f = gdal.VSIFOpenL(tmp_vsimem / "map.txt", "rb")
    assert f is not None
    data = gdal.VSIFReadL(1, 10000, f).decode("ascii")
    gdal.VSIFCloseL(f)

    assert data == '{ "properties": { "geometry": { "type": "geo_shape" } } }'


###############################################################################
# Test authentication


def test_ogr_elasticsearch_authentication(tmp_vsimem):

    # use /vsimem because it handles USERPWD

    gdal.FileFromMemBuffer(
        tmp_vsimem / "fakeelasticsearch&USERPWD=user:pwd",
        """{"version":{"number":"5.0.0"}}""",
    )

    gdal.FileFromMemBuffer(
        tmp_vsimem / """fakeelasticsearch/_cat/indices?h=i&USERPWD=user:pwd""",
        "a_layer  \n",
    )
    gdal.FileFromMemBuffer(
        tmp_vsimem / """fakeelasticsearch/a_layer/_mapping?pretty&USERPWD=user:pwd""",
        """
{
    "a_layer":
    {
        "mappings":
        {
            "FeatureCollection":
            {
                "properties":
                {
                    "type": { "type": "text" },
                    "properties" :
                    {
                        "properties":
                        {
                            "str_field": { "type": "text"}
                        }
                    }
                }
            }
        }
    }
}
""",
    )

    ds = gdal.OpenEx(
        f"ES:{tmp_vsimem}/fakeelasticsearch", open_options=["USERPWD=user:pwd"]
    )
    assert ds is not None


###############################################################################
# Test FORWARD_HTTP_HEADERS_FROM_ENV


def test_ogr_elasticsearch_http_headers_from_env(tmp_vsimem):

    gdal.FileFromMemBuffer(
        tmp_vsimem
        / """fakeelasticsearch/_cat/indices?h=i&HEADERS=Bar: value_of_bar\nFoo: value_of_foo\n""",
        "",
    )

    gdal.FileFromMemBuffer(
        tmp_vsimem
        / """fakeelasticsearch/_search?scroll=1m&size=100&POSTFIELDS={ 'FOO' : 'BAR' }&HEADERS=Content-Type: application/json; charset=UTF-8\nBar: value_of_bar\nFoo: value_of_foo\n""",
        """{
        "hits":
        {
            "hits":[
            {
                "_index": "some_layer",
                "_type": "some_type",
                "_source": {
                    "some_field": 5
                },
            }
            ]
        }
    }""",
    )

    gdal.FileFromMemBuffer(
        tmp_vsimem
        / """fakeelasticsearch/some_layer/_mapping/some_type?pretty&HEADERS=Bar: value_of_bar\nFoo: value_of_foo\n""",
        """
    {
        "some_layer":
        {
            "mappings":
            {
                "some_type":
                {
                    "properties":
                    {
                        "some_field": { "type": "string"}
                    }
                }
            }
        }
    }
    """,
    )

    with gdaltest.config_options(
        {
            "CPL_CURL_VSIMEM_PRINT_HEADERS": "YES",
            "FOO": "value_of_foo",
            "BAR": "value_of_bar",
        }
    ):

        gdal.FileFromMemBuffer(
            tmp_vsimem
            / "fakeelasticsearch&HEADERS=Bar: value_of_bar\nFoo: value_of_foo\n",
            """{"version":{"number":"5.0.0"}}""",
        )

        ds = gdal.OpenEx(
            f"ES:{tmp_vsimem}/fakeelasticsearch",
            open_options=[
                "FORWARD_HTTP_HEADERS_FROM_ENV=Foo=FOO,Bar=BAR,Baz=I_AM_NOT_SET"
            ],
        )
        assert ds is not None
        sql_lyr = ds.ExecuteSQL("{ 'FOO' : 'BAR' }", dialect="ES")
        f = sql_lyr.GetNextFeature()
        assert f["some_field"] == "5"
        ds.ReleaseResultSet(sql_lyr)


###############################################################################
# Test GeoShape WKT support


def test_ogr_elasticsearch_geo_shape_wkt(
    es_url, handle_get, handle_post, handle_put, handle_delete
):

    handle_get("/fakeelasticsearch", """{"version":{"number":"7.0.0"}}""")

    ds = ogrtest.elasticsearch_drv.CreateDataSource(f"{es_url}/fakeelasticsearch")
    assert ds is not None

    handle_put("/fakeelasticsearch/geo_shape_wkt", "{}")
    lyr = ds.CreateLayer(
        "geo_shape_wkt", srs=ogrtest.srs_wgs84, options=["GEO_SHAPE_ENCODING=WKT"]
    )
    f = ogr.Feature(lyr.GetLayerDefn())
    f.SetGeometry(ogr.CreateGeometryFromWkt("POINT (2 49)"))
    handle_post(
        """/fakeelasticsearch/geo_shape_wkt/_mapping""",
        post_body="""{ "properties": { "geometry": { "type": "geo_shape" } }, "_meta": { "fid": "ogc_fid" } }""",
        contents="{}",
    )
    handle_post(
        """/fakeelasticsearch/_bulk""",
        post_body="""{"index" :{"_index":"geo_shape_wkt"}}
{ "ogc_fid": 1, "geometry": "POINT (2 49)" }

""",
        contents="{}",
    )
    ret = lyr.CreateFeature(f)
    assert ret == 0

    lyr.ResetReading()
    handle_get(
        """/fakeelasticsearch/geo_shape_wkt/_search?scroll=1m&size=100""",
        """{
    "_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_id": "my_id",
            "_source": {
                "geometry": "POINT (2 49)"
            }
        }
        ]
    }
}""",
    )
    handle_delete(
        "/fakeelasticsearch/_search/scroll?scroll_id=my_scrollid",
        "{}",
    )
    f = lyr.GetNextFeature()
    assert f.GetGeometryRef().ExportToWkt() == "POINT (2 49)"


###############################################################################
# Test _TIMEOUT / _TERMINATE_AFTER


@pytest.mark.skipif(sys.platform == "win32", reason="Test fails on Windows")
def test_ogr_elasticsearch_timeout_terminate_after(es_url, handle_get, handle_post):

    handle_get("/fakeelasticsearch", """{"version":{"number":"7.0.0"}}""")

    handle_get("""/fakeelasticsearch/_cat/indices?h=i""", "some_layer\n")

    handle_post(
        """/fakeelasticsearch/_search?scroll=1m&size=100""",
        post_body="""{ 'FOO' : 'BAR' }""",
        contents="""{
        "hits":
        {
            "hits":[
            {
                "_index": "some_layer",
                "_source": {
                    "some_field": 5,
                    "geometry": [2, 49]
                },
            }
            ]
        }
    }""",
    )

    handle_get(
        """/fakeelasticsearch/some_layer/_mapping?pretty""",
        """
    {
        "some_layer":
        {
            "mappings":
            {
                "properties":
                {
                    "some_field": { "type": "string", "index": "not_analyzed" },
                    "geometry": { "type": "geo_point" },
                }
            }
        }
    }
    """,
    )

    ds = gdal.OpenEx(
        f"ES:{es_url}/fakeelasticsearch",
        open_options=[
            "SINGLE_QUERY_TERMINATE_AFTER=10",
            "SINGLE_QUERY_TIMEOUT=0.5",
            "FEATURE_ITERATION_TERMINATE_AFTER=2",
            "FEATURE_ITERATION_TIMEOUT=0.1",
        ],
    )
    assert ds is not None
    sql_lyr = ds.ExecuteSQL("{ 'FOO' : 'BAR' }", dialect="ES")
    f = sql_lyr.GetNextFeature()
    assert f["some_field"] == "5"
    assert f.GetGeometryRef().ExportToWkt() == "POINT (2 49)"

    handle_post(
        """/fakeelasticsearch/_search?pretty&timeout=500ms&terminate_after=10""",
        post_body="""{ "size": 0 ,  'FOO' : 'BAR' }""",
        contents="""
    {
        "took" : 1,
        "timed_out" : false,
        "terminated_early" : true,
        "hits" : {
            "total" : {
            "value" : 4,
            "relation" : "eq"
            },
            "max_score" : null,
            "hits" : [ ]
        }
    }
    """,
    )

    assert sql_lyr.GetFeatureCount() == 4

    ds.ReleaseResultSet(sql_lyr)
    sql_lyr = None

    lyr = ds.GetLayer(0)

    handle_get(
        """/fakeelasticsearch/some_layer/_search?scroll=1m&size=100""",
        """{
        "hits":
        {
            "hits":[
            {
                "_source": {
                    "some_field": 5,
                    "geometry": [2, 49]
                },
            },
            {
                "_source": {
                    "some_field": 7,
                    "geometry": [2, 49]
                },
            },
            {
                "_source": {
                    "some_field": 8,
                    "geometry": [2, 49]
                },
            }
            ]
        }
    }""",
    )

    handle_post(
        """/fakeelasticsearch/some_layer/_search?pretty&timeout=500ms&terminate_after=10""",
        post_body="""{ "size": 0 }""",
        contents="""
    {
        "took" : 1,
        "timed_out" : false,
        "terminated_early" : true,
        "hits" : {
            "total" : {
            "value" : 2,
            "relation" : "eq"
            },
            "max_score" : null,
            "hits" : [ ]
        }
    }
    """,
    )

    assert lyr.GetFeatureCount() == 2

    handle_post(
        """/fakeelasticsearch/some_layer/_search?pretty&timeout=500ms&terminate_after=10""",
        post_body="""{ "size": 0, "query": { "constant_score" : { "filter": { "term": { "some_field": "6" } } } } }""",
        contents="""
    {
        "took" : 1,
        "timed_out" : false,
        "terminated_early" : true,
        "hits" : {
            "total" : {
            "value" : 3,
            "relation" : "eq"
            },
            "max_score" : null,
            "hits" : [ ]
        }
    }
    """,
    )

    lyr.SetAttributeFilter("some_field = '6'")
    assert lyr.GetFeatureCount() == 3
    lyr.SetAttributeFilter(None)

    handle_post(
        """/fakeelasticsearch/some_layer/_search?pretty&timeout=500ms&terminate_after=10""",
        post_body="""{ "size": 0,  "foo": "bar" }""",
        contents="""
    {
        "took" : 1,
        "timed_out" : false,
        "terminated_early" : true,
        "hits" : {
            "total" : {
            "value" : 4,
            "relation" : "eq"
            },
            "max_score" : null,
            "hits" : [ ]
        }
    }
    """,
    )

    lyr.SetAttributeFilter('{ "foo": "bar" }')
    assert lyr.GetFeatureCount() == 4
    lyr.SetAttributeFilter(None)

    handle_post(
        """/fakeelasticsearch/some_layer/_search?pretty&timeout=500ms&terminate_after=10""",
        post_body="""{ "size": 0, "aggs" : { "bbox" : { "geo_bounds" : { "field" : "geometry" } } } }""",
        contents="""
    {
        "aggregations" : {
            "bbox" : {
            "bounds" : {
                "top_left" : {
                "lat" : 10,
                "lon" : 1
                },
                "bottom_right" : {
                "lat" : 9,
                "lon" : 2
                }
            }
            }
        }
    }""",
    )
    bbox = lyr.GetExtent()
    assert bbox == (1.0, 2.0, 9.0, 10.0)

    # Check FEATURE_ITERATION_TERMINATE_AFTER
    lyr.ResetReading()
    assert lyr.GetNextFeature() is not None
    assert lyr.GetNextFeature() is not None
    assert lyr.GetNextFeature() is None

    # Check FEATURE_ITERATION_TIMEOUT
    lyr.ResetReading()
    assert lyr.GetNextFeature() is not None
    time.sleep(0.15)
    assert lyr.GetNextFeature() is None


###############################################################################
# Test aggregation


def test_ogr_elasticsearch_aggregation_minimum(es_url, handle_get, handle_post):

    handle_get("/fakeelasticsearch", """{"version":{"number":"6.8.0"}}""")

    handle_get(
        """/fakeelasticsearch/test/_mapping?pretty""",
        """
    {
        "test":
        {
            "mappings":
            {
                "default":
                {
                    "properties":
                    {
                        "a_geopoint":
                        {
                            "properties":
                            {
                                "coordinates":
                                {
                                    "type": "geo_point"
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    """,
    )

    ds = gdal.OpenEx(
        f"ES:{es_url}/fakeelasticsearch", open_options=['AGGREGATION={"index":"test"}']
    )
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr.TestCapability(ogr.OLCStringsAsUTF8) == 1

    response = {
        "aggregations": {
            "grid": {
                "buckets": [
                    {
                        "key": "dummy_key",
                        "doc_count": 9876543210,
                        "centroid": {
                            "location": {"lat": 60, "lon": 50},
                            "count": 9876543210,
                        },
                    },
                    {
                        "key": "dummy_key2",
                        "doc_count": 1,
                        "centroid": {
                            "location": {"lat": -60.5, "lon": -50.5},
                            "count": 1,
                        },
                    },
                ]
            }
        }
    }

    handle_post(
        "/fakeelasticsearch/test/_search",
        post_body="""{"size":0,"aggs":{"grid":{"geohash_grid":{"field":"a_geopoint.coordinates","precision":2,"size":10000},"aggs":{"centroid":{"geo_centroid":{"field":"a_geopoint.coordinates"}}}}}}""",
        contents=json.dumps(response),
    )
    assert lyr.GetFeatureCount() == 2

    ds = gdal.OpenEx(
        f"ES:{es_url}/fakeelasticsearch", open_options=['AGGREGATION={"index":"test"}']
    )
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr.GetDataset().GetDescription() == ds.GetDescription()
    f = lyr.GetNextFeature()

    assert f is not None
    assert f["key"] == "dummy_key"
    assert f["doc_count"] == 9876543210
    assert f.GetGeometryRef().ExportToWkt() == "POINT (50 60)"

    f = lyr.GetNextFeature()
    assert f is not None
    assert f["key"] == "dummy_key2"
    assert f.GetGeometryRef().ExportToWkt() == "POINT (-50.5 -60.5)"

    assert lyr.GetFeatureCount() == 2

    # Test spatial filter coordinate clamping
    lyr.SetSpatialFilterRect(-200, -200, 200, 200)
    lyr.ResetReading()
    assert lyr.GetFeatureCount() == 2

    # Test normal spatial filter
    lyr.SetSpatialFilterRect(1, 2, 3, 4)
    lyr.ResetReading()

    handle_post(
        """/fakeelasticsearch/test/_search""",
        post_body="""{"size":0,"aggs":{"filtered":{"filter":{"geo_bounding_box":{"a_geopoint.coordinates":{"top_left":{"lat":4.0,"lon":1.0},"bottom_right":{"lat":2.0,"lon":3.0}}}},"aggs":{"grid":{"geohash_grid":{"field":"a_geopoint.coordinates","precision":5,"size":10000},"aggs":{"centroid":{"geo_centroid":{"field":"a_geopoint.coordinates"}}}}}}}}""",
        contents=json.dumps(
            {
                "aggregations": {
                    "filtered": {
                        "grid": {
                            "buckets": [
                                {
                                    "key": "dummy_key3",
                                    "doc_count": 1,
                                    "centroid": {"location": {"lat": 3.0, "lon": 2.0}},
                                }
                            ]
                        }
                    }
                }
            }
        ),
    )

    f = lyr.GetNextFeature()
    assert f is not None
    assert f["key"] == "dummy_key3"

    ###############################################################################
    # Test all options

    ds = gdal.OpenEx(
        f"ES:{es_url}/fakeelasticsearch",
        open_options=[
            'AGGREGATION={"index":"test","geohash_grid":{"size":100,"precision":4},"fields":{"min":["a", "f"],"max":["b"],"avg":["c"],"sum":["d"],"count":["e"],"stats":["f"]}}'
        ],
    )
    assert ds is not None
    lyr = ds.GetLayer(0)
    assert lyr.GetLayerDefn().GetFieldCount() == 12

    response = {
        "aggregations": {
            "grid": {
                "buckets": [
                    {
                        "key": "dummy_key",
                        "doc_count": 9876543210,
                        "centroid": {
                            "location": {"lat": 60, "lon": 50},
                            "count": 9876543210,
                        },
                        "a_min": {"value": 1.5},
                        "b_max": {"value": 2.5},
                        "c_avg": {"value": 3.5},
                        "d_sum": {"value": 4.5},
                        "e_count": {"value": 9876543211},
                        "f_stats": {
                            "min": 1,
                            "max": 2,
                            "avg": 3,
                            "sum": 4,
                            "count": 9876543212,
                        },
                    },
                ]
            }
        }
    }

    handle_post(
        "/fakeelasticsearch/test/_search",
        post_body="""{"size":0,"aggs":{"grid":{"geohash_grid":{"field":"a_geopoint.coordinates","precision":4,"size":100},"aggs":{"centroid":{"geo_centroid":{"field":"a_geopoint.coordinates"}},"f_stats":{"stats":{"field":"f"}},"a_min":{"min":{"field":"a"}},"b_max":{"max":{"field":"b"}},"c_avg":{"avg":{"field":"c"}},"d_sum":{"sum":{"field":"d"}},"e_count":{"value_count":{"field":"e"}}}}}}""",
        contents=json.dumps(response),
    )

    f = lyr.GetNextFeature()

    assert f["key"] == "dummy_key"
    assert f["doc_count"] == 9876543210
    assert f["a_min"] == 1.5
    assert f["b_max"] == 2.5
    assert f["c_avg"] == 3.5
    assert f["d_sum"] == 4.5
    assert f["e_count"] == 9876543211
    assert f["f_min"] == 1
    assert f["f_max"] == 2
    assert f["f_avg"] == 3
    assert f["f_sum"] == 4
    assert f["f_count"] == 9876543212
    assert f.GetGeometryRef().ExportToWkt() == "POINT (50 60)"


###############################################################################
# Test GetLayerByName() with a wildcard name


def test_ogr_elasticsearch_wildcard_layer_name(es_url, handle_get, handle_delete):

    handle_get("/fakeelasticsearch", """{"version":{"number":"6.8.0"}}""")

    ds = gdal.OpenEx(f"ES:{es_url}/fakeelasticsearch")

    handle_get("""/fakeelasticsearch/_cat/indices/test*?h=i""", "test1\ntest2\n")

    handle_get(
        """/fakeelasticsearch/test1/_mapping?pretty""",
        """
    {
        "test1":
        {
            "mappings":
            {
                "default":
                {
                    "properties":
                    {
                        "a_geopoint":
                        {
                            "properties":
                            {
                                "coordinates":
                                {
                                    "type": "geo_point"
                                }
                            }
                        },
                        "str_field": { "type": "string"},
                        "str_field2": { "type": "string"}
                    }
                }
            }
        }
    }
    """,
    )

    handle_get(
        """/fakeelasticsearch/test1/default/_search?scroll=1m&size=100""",
        """{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_index": "test1",
            "_id": "my_id",
            "_source": {
                "a_geopoint": {
                    "type": "Point",
                    "coordinates": [2.0,49.0]
                },
                "str_field": "foo",
                "str_field2": "bar"
            }
        }]
    }
}""",
    )
    handle_get(
        """/fakeelasticsearch/_search/scroll?scroll=1m&scroll_id=my_scrollid""",
        "{}",
    )
    handle_delete(
        """/fakeelasticsearch/_search/scroll?scroll_id=my_scrollid""",
        "{}",
    )

    lyr = ds.GetLayerByName("test*,-test3")
    assert lyr.GetLayerDefn().GetFieldCount() == 3
    assert lyr.GetLayerDefn().GetGeomFieldCount() == 1

    handle_get(
        """/fakeelasticsearch/test*,-test3/default/_search?scroll=1m&size=100""",
        """{
"_scroll_id": "my_scrollid",
    "hits":
    {
        "hits":[
        {
            "_index": "test1",
            "_id": "my_id",
            "_source": {
                "a_geopoint": {
                    "type": "Point",
                    "coordinates": [2.0,49.0]
                },
                "str_field": "foo",
                "str_field2": "bar"
            }
        },
        {
            "_index": "test2",
            "_id": "my_id2",
            "_source": {
                "a_geopoint": {
                    "type": "Point",
                    "coordinates": [3.0,50.0]
                },
                "str_field": "foo2",
                "str_field2": "bar2"
            }
        }
        ]
    }
}""",
    )
    handle_get(
        """/fakeelasticsearch/_search/scroll?scroll=1m&scroll_id=my_scrollid""",
        "{}",
    )
    handle_delete(
        """/fakeelasticsearch/_search/scroll?scroll_id=my_scrollid""",
        "{}",
    )

    f = lyr.GetNextFeature()
    assert f["_id"] == "my_id"
    assert f["str_field"] == "foo"
    assert f["str_field2"] == "bar"
    assert f.GetGeometryRef().ExportToWkt() == "POINT (2 49)"

    f = lyr.GetNextFeature()
    assert f["_id"] == "my_id2"
    assert f["str_field"] == "foo2"
    assert f["str_field2"] == "bar2"
    assert f.GetGeometryRef().ExportToWkt() == "POINT (3 50)"

    # Test with ADD_SOURCE_INDEX_NAME
    ds = gdal.OpenEx(
        f"ES:{es_url}/fakeelasticsearch", open_options=["ADD_SOURCE_INDEX_NAME=YES"]
    )

    lyr = ds.GetLayerByName("test*,-test3")
    assert lyr.GetLayerDefn().GetFieldCount() == 4
    assert lyr.GetLayerDefn().GetGeomFieldCount() == 1

    f = lyr.GetNextFeature()
    assert f["_index"] == "test1"
    assert f["_id"] == "my_id"
    assert f["str_field"] == "foo"
    assert f["str_field2"] == "bar"
    assert f.GetGeometryRef().ExportToWkt() == "POINT (2 49)"

    f = lyr.GetNextFeature()
    assert f["_index"] == "test2"
    assert f["_id"] == "my_id2"
    assert f["str_field"] == "foo2"
    assert f["str_field2"] == "bar2"
    assert f.GetGeometryRef().ExportToWkt() == "POINT (3 50)"


###############################################################################
# Test upserting a feature.


def test_ogr_elasticsearch_upsert_feature(es_url, handle_get, handle_post):

    handle_get("/fakeelasticsearch", """{"version":{"number":"6.8.0"}}""")

    ds = ogrtest.elasticsearch_drv.CreateDataSource(f"{es_url}/fakeelasticsearch")
    assert ds is not None

    handle_get(
        "/fakeelasticsearch/test_upsert",
        "{}",
    )

    handle_post(
        "/fakeelasticsearch/test_upsert/_mapping/FeatureCollection",
        post_body='{ "FeatureCollection": { "properties": {} }}',
        contents="{}",
    )

    # Create a layer that will not bulk upsert.
    lyr = ds.CreateLayer(
        "test_upsert",
        srs=ogrtest.srs_wgs84,
        options=[
            'MAPPING={ "FeatureCollection": { "properties": {} }}',
            "BULK_INSERT=NO",
        ],
    )
    assert lyr is not None

    f = ogr.Feature(lyr.GetLayerDefn())
    f["_id"] = "upsert_id"

    handle_post(
        "/fakeelasticsearch/test_upsert/FeatureCollection/upsert_id/_update",
        post_body='{"doc":{ "ogc_fid": 1, "properties": { } },"doc_as_upsert":true}',
        contents="{}",
    )

    # Upsert new feature
    assert lyr.UpsertFeature(f) == ogr.OGRERR_NONE

    # Upsert existing feature
    assert lyr.UpsertFeature(f) == ogr.OGRERR_NONE

    # Create a layer that will bulk upsert.
    lyr = ds.CreateLayer(
        "test_upsert",
        srs=ogrtest.srs_wgs84,
        options=['MAPPING={ "FeatureCollection": { "properties": {} }}'],
    )
    assert lyr is not None

    # Upsert new feature
    assert lyr.UpsertFeature(f) == ogr.OGRERR_NONE

    # Upsert existing feature
    assert lyr.UpsertFeature(f) == ogr.OGRERR_NONE

    gdal.ErrorReset()
    handle_post(
        """/fakeelasticsearch/_bulk""",
        post_body="""{"update":{"_index":"test_upsert","_id":"upsert_id", "_type":"FeatureCollection"}}
{"doc":{ "ogc_fid": 1, "properties": { } },"doc_as_upsert":true}

{"update":{"_index":"test_upsert","_id":"upsert_id", "_type":"FeatureCollection"}}
{"doc":{ "ogc_fid": 1, "properties": { } },"doc_as_upsert":true}

""",
        contents="{}",
    )
    assert lyr.SyncToDisk() == ogr.OGRERR_NONE
    assert gdal.GetLastErrorMsg() == ""

    ds = None
