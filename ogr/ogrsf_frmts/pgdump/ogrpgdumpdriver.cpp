/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRPGDumpDriver class.
 * Author:   Even Rouault, <even dot rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2010-2011, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_pgdump.h"
#include "cpl_conv.h"

/************************************************************************/
/*                         OGRPGDumpDriverCreate()                      */
/************************************************************************/

static GDALDataset *
OGRPGDumpDriverCreate(const char *pszName, CPL_UNUSED int nXSize,
                      CPL_UNUSED int nYSize, CPL_UNUSED int nBands,
                      CPL_UNUSED GDALDataType eDT, char **papszOptions)
{
    if (strcmp(pszName, "/dev/stdout") == 0)
        pszName = "/vsistdout/";

    OGRPGDumpDataSource *poDS = new OGRPGDumpDataSource(pszName, papszOptions);
    if (!poDS->Log("SET standard_conforming_strings = ON"))
    {
        delete poDS;
        return nullptr;
    }

    return poDS;
}

/************************************************************************/
/*                        RegisterOGRPGDump()                           */
/************************************************************************/

void RegisterOGRPGDump()

{
    if (GDALGetDriverByName("PGDUMP") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("PGDUMP");
    poDriver->SetMetadataItem(GDAL_DCAP_VECTOR, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_LAYER, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CREATE_FIELD, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_CURVE_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_MEASURED_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_Z_GEOMETRIES, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "PostgreSQL SQL dump");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/vector/pgdump.html");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "sql");

    poDriver->SetMetadataItem(
        GDAL_DMD_CREATIONOPTIONLIST,
        "<CreationOptionList>"
#ifdef _WIN32
        "  <Option name='LINEFORMAT' type='string-select' "
        "description='end-of-line sequence' default='CRLF'>"
#else
        "  <Option name='LINEFORMAT' type='string-select' "
        "description='end-of-line sequence' default='LF'>"
#endif
        "    <Value>CRLF</Value>"
        "    <Value>LF</Value>"
        "  </Option>"
        "</CreationOptionList>");

    poDriver->SetMetadataItem(
        GDAL_DS_LAYER_CREATIONOPTIONLIST,
        "<LayerCreationOptionList>"
        "  <Option name='GEOM_TYPE' type='string-select' description='Format "
        "of geometry columns' default='geometry'>"
        "    <Value>geometry</Value>"
        "    <Value>geography</Value>"
        "  </Option>"
        "  <Option name='LAUNDER' type='boolean' description='Whether layer "
        "and field names will be laundered' default='YES'/>"
        "  <Option name='LAUNDER_ASCII' type='boolean' description='Same as "
        "LAUNDER, but force generation of ASCII identifiers' default='NO'/>"
        "  <Option name='PRECISION' type='boolean' description='Whether fields "
        "created should keep the width and precision' default='YES'/>"
        "  <Option name='DIM' type='string' description='Set to 2 to force the "
        "geometries to be 2D, 3 to be 2.5D, XYM or XYZM'/>"
        "  <Option name='GEOMETRY_NAME' type='string' description='Name of "
        "geometry column. Defaults to wkb_geometry for GEOM_TYPE=geometry or "
        "the_geog for GEOM_TYPE=geography'/>"
        "  <Option name='SCHEMA' type='string' description='Name of schema "
        "into which to create the new table'/>"
        "  <Option name='CREATE_SCHEMA' type='boolean' description='Whether to "
        "explicitly emit the CREATE SCHEMA statement to create the specified "
        "schema' default='YES'/>"
        "  <Option name='SPATIAL_INDEX' type='string-select' description='Type "
        "of spatial index to create' default='GIST'>"
        "    <Value>NONE</Value>"
        "    <Value>GIST</Value>"
        "    <Value>SPGIST</Value>"
        "    <Value>BRIN</Value>"
        "  </Option>"
        "  <Option name='GEOM_COLUMN_POSITION' type='string-select' "
        "description='Whether geometry/geography columns should be created "
        "as soon they are created (IMMEDIATE) or after non-spatial columns' "
        "default='IMMEDIATE'>"
        "    <Value>IMMEDIATE</Value>"
        "    <Value>END</Value>"
        "  </Option>"
        "  <Option name='TEMPORARY' type='boolean' description='Whether to a "
        "temporary table instead of a permanent one' default='NO'/>"
        "  <Option name='UNLOGGED' type='boolean' description='Whether to "
        "create the table as a unlogged one' default='NO'/>"
        "  <Option name='WRITE_EWKT_GEOM' type='boolean' description='Whether "
        "to write EWKT geometries instead of HEX geometry' default='NO'/>"
        "  <Option name='CREATE_TABLE' type='boolean' description='Whether to "
        "explicitly recreate the table if necessary' default='YES'/>"
        "  <Option name='SKIP_CONFLICTS' type='boolean' description='Whether "
        "to ignore conflicts when inserting features' default='NO'/>"
        "  <Option name='DROP_TABLE' type='string-select' description='Whether "
        "to explicitly destroy tables before recreating them' default='YES'>"
        "    <Value>YES</Value>"
        "    <Value>ON</Value>"
        "    <Value>TRUE</Value>"
        "    <Value>NO</Value>"
        "    <Value>OFF</Value>"
        "    <Value>FALSE</Value>"
        "    <Value>IF_EXISTS</Value>"
        "  </Option>"
        "  <Option name='SRID' type='int' description='Forced SRID of the "
        "layer'/>"
        "  <Option name='NONE_AS_UNKNOWN' type='boolean' description='Whether "
        "to force non-spatial layers to be created as spatial tables' "
        "default='NO'/>"
        "  <Option name='FID' type='string' description='Name of the FID "
        "column to create. Set to empty to not create it.' default='ogc_fid'/>"
        "  <Option name='FID64' type='boolean' description='Whether to create "
        "the FID column with BIGSERIAL type to handle 64bit wide ids' "
        "default='NO'/>"
        "  <Option name='EXTRACT_SCHEMA_FROM_LAYER_NAME' type='boolean' "
        "description='Whether a dot in a layer name should be considered as "
        "the separator for the schema and table name' default='YES'/>"
        "  <Option name='COLUMN_TYPES' type='string' description='A list of "
        "strings of format field_name=pg_field_type (separated by comma) to "
        "force the PG column type of fields to be created'/>"
        "  <Option name='POSTGIS_VERSION' type='string' description='A string "
        "formatted as X.Y' default='2.2'/>"
        "  <Option name='DESCRIPTION' type='string' description='Description "
        "string to put in the pg_description system table'/>"
        "</LayerCreationOptionList>");

    poDriver->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATATYPES,
                              "Integer Integer64 Real String Date DateTime "
                              "Time IntegerList Integer64List RealList "
                              "StringList Binary");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONFIELDDATASUBTYPES,
                              "Boolean Int16 Float32");
    poDriver->SetMetadataItem(GDAL_DMD_CREATION_FIELD_DEFN_FLAGS,
                              "WidthPrecision Nullable Unique Default Comment");

    poDriver->SetMetadataItem(GDAL_DCAP_NOTNULL_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_DEFAULT_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_UNIQUE_FIELDS, "YES");
    poDriver->SetMetadataItem(GDAL_DCAP_NOTNULL_GEOMFIELDS, "YES");

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnCreate = OGRPGDumpDriverCreate;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
