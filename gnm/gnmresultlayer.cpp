/******************************************************************************
 *
 * Project:  GDAL/OGR Geography Network support (Geographic Network Model)
 * Purpose:  GNM result layer class.
 * Authors:  Mikhail Gusev (gusevmihs at gmail dot com)
 *           Dmitry Baryshnikov, polimax@mail.ru
 *
 ******************************************************************************
 * Copyright (c) 2014, Mikhail Gusev
 * Copyright (c) 2014-2015, NextGIS <info@nextgis.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 ****************************************************************************/
#include "gnm.h"
#include "gnm_priv.h"

/** Constructor */
OGRGNMWrappedResultLayer::OGRGNMWrappedResultLayer(GDALDataset *poDSIn,
                                                   OGRLayer *poLayerIn)
{
    this->poDS = poDSIn;
    this->poLayer = poLayerIn;

    // create standard fields

    OGRFieldDefn oFieldGID(GNM_SYSFIELD_GFID, GNMGFIDInt);
    poLayer->CreateField(&oFieldGID);

    OGRFieldDefn oFieldLayerName(GNM_SYSFIELD_LAYERNAME, OFTString);
    oFieldLayerName.SetWidth(254);
    poLayer->CreateField(&oFieldLayerName);

    OGRFieldDefn oFieldNo(GNM_SYSFIELD_PATHNUM, OFTInteger);
    poLayer->CreateField(&oFieldNo);

    OGRFieldDefn oFieldType(GNM_SYSFIELD_TYPE, OFTString);  // EDGE or VERTEX
    poLayer->CreateField(&oFieldType);
}

OGRGNMWrappedResultLayer::~OGRGNMWrappedResultLayer()
{
    delete poDS;
}

void OGRGNMWrappedResultLayer::ResetReading()
{
    poLayer->ResetReading();
}

OGRFeature *OGRGNMWrappedResultLayer::GetNextFeature()
{
    return poLayer->GetNextFeature();
}

OGRErr OGRGNMWrappedResultLayer::SetNextByIndex(GIntBig nIndex)
{
    return poLayer->SetNextByIndex(nIndex);
}

OGRFeature *OGRGNMWrappedResultLayer::GetFeature(GIntBig nFID)
{
    return poLayer->GetFeature(nFID);
}

OGRFeatureDefn *OGRGNMWrappedResultLayer::GetLayerDefn()
{
    return poLayer->GetLayerDefn();
}

GIntBig OGRGNMWrappedResultLayer::GetFeatureCount(int bForce)
{
    return poLayer->GetFeatureCount(bForce);
}

int OGRGNMWrappedResultLayer::TestCapability(const char *pszCap)
{
    return poLayer->TestCapability(pszCap);
}

OGRErr OGRGNMWrappedResultLayer::CreateField(const OGRFieldDefn *poField,
                                             int bApproxOK)
{
    return poLayer->CreateField(poField, bApproxOK);
}

OGRErr
OGRGNMWrappedResultLayer::CreateGeomField(const OGRGeomFieldDefn *poField,
                                          int bApproxOK)
{
    return poLayer->CreateGeomField(poField, bApproxOK);
}

const char *OGRGNMWrappedResultLayer::GetFIDColumn()
{
    return poLayer->GetFIDColumn();
}

const char *OGRGNMWrappedResultLayer::GetGeometryColumn()
{
    return poLayer->GetGeometryColumn();
}

OGRSpatialReference *OGRGNMWrappedResultLayer::GetSpatialRef()
{
    return poLayer->GetSpatialRef();
}

/** Undocumented */
OGRErr OGRGNMWrappedResultLayer::InsertFeature(OGRFeature *poFeature,
                                               const CPLString &soLayerName,
                                               int nPathNo, bool bIsEdge)
{
    VALIDATE_POINTER1(poFeature, "Input feature is invalid",
                      OGRERR_INVALID_HANDLE);
    // add fields from input feature
    const OGRFeatureDefn *poSrcDefn = poFeature->GetDefnRef();
    OGRFeatureDefn *poDstFDefn = GetLayerDefn();
    if (nullptr == poSrcDefn || nullptr == poDstFDefn)
        return OGRERR_INVALID_HANDLE;

    const int nSrcFieldCount = poSrcDefn->GetFieldCount();
    int nDstFieldCount = poDstFDefn->GetFieldCount();

    // Initialize the index-to-index map to -1's
    std::vector<int> anMap(nSrcFieldCount, -1);

    for (int iField = 0; iField < nSrcFieldCount; iField++)
    {
        const OGRFieldDefn *poSrcFieldDefn = poSrcDefn->GetFieldDefn(iField);
        OGRFieldDefn oFieldDefn(poSrcFieldDefn);

        /* The field may have been already created at layer creation */
        const int iDstField =
            poDstFDefn->GetFieldIndex(oFieldDefn.GetNameRef());
        if (iDstField >= 0)
        {
            // TODO: by now skip fields with different types. In future shoul
            // cast types
            OGRFieldDefn *poDstField = poDstFDefn->GetFieldDefn(iDstField);
            if (nullptr != poDstField &&
                oFieldDefn.GetType() == poDstField->GetType())
                anMap[iField] = iDstField;
        }
        else if (CreateField(&oFieldDefn) == OGRERR_NONE)
        {
            /* Sanity check : if it fails, the driver is buggy */
            if (poDstFDefn->GetFieldCount() != nDstFieldCount + 1)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "The output driver has claimed to have added the %s "
                         "field, but it did not!",
                         oFieldDefn.GetNameRef());
            }
            else
            {
                anMap[iField] = nDstFieldCount;
                nDstFieldCount++;
            }
        }
    }

    auto poInsertFeature = std::make_unique<OGRFeature>(GetLayerDefn());
    if (poInsertFeature->SetFrom(poFeature, anMap.data(), TRUE) != OGRERR_NONE)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to translate feature " CPL_FRMT_GIB
                 " from layer %s.\n",
                 poFeature->GetFID(), soLayerName.c_str());
        return OGRERR_FAILURE;
    }

    // poInsertFeature->SetField( GNM_SYSFIELD_GFID,
    // (GNMGFID)poFeature->GetFID() );
    poInsertFeature->SetField(GNM_SYSFIELD_LAYERNAME, soLayerName);
    poInsertFeature->SetField(GNM_SYSFIELD_PATHNUM, nPathNo);
    poInsertFeature->SetField(GNM_SYSFIELD_TYPE, bIsEdge ? "EDGE" : "VERTEX");

    CPLErrorReset();
    return CreateFeature(poInsertFeature.get());
}

OGRErr OGRGNMWrappedResultLayer::ISetFeature(OGRFeature *poFeature)
{
    return poLayer->SetFeature(poFeature);
}

OGRErr OGRGNMWrappedResultLayer::ICreateFeature(OGRFeature *poFeature)
{
    return poLayer->CreateFeature(poFeature);
}
