/******************************************************************************
 *
 * Project:  netCDF read/write Driver
 * Purpose:  GDAL bindings over netCDF library.
 * Author:   Even Rouault <even.rouault at spatialys.com>
 *
 ******************************************************************************
 * Copyright (c) 2016, Even Rouault <even.rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "netcdfdataset.h"

bool netCDFWriterConfiguration::SetNameValue(
    CPLXMLNode *psNode, std::map<CPLString, CPLString> &oMap)
{
    const char *pszName = CPLGetXMLValue(psNode, "name", nullptr);
    const char *pszValue = CPLGetXMLValue(psNode, "value", nullptr);
    if (pszName != nullptr && pszValue != nullptr)
    {
        oMap[pszName] = pszValue;
        return true;
    }
    CPLError(CE_Failure, CPLE_IllegalArg, "Missing name/value");
    return false;
}

bool netCDFWriterConfiguration::Parse(const char *pszFilename)
{
    CPLXMLNode *psRoot = STARTS_WITH(pszFilename, "<Configuration")
                             ? CPLParseXMLString(pszFilename)
                             : CPLParseXMLFile(pszFilename);
    if (psRoot == nullptr)
        return false;
    CPLXMLTreeCloser oCloser(psRoot);

    for (CPLXMLNode *psIter = psRoot->psChild; psIter != nullptr;
         psIter = psIter->psNext)
    {
        if (psIter->eType != CXT_Element)
            continue;
        if (EQUAL(psIter->pszValue, "DatasetCreationOption"))
        {
            SetNameValue(psIter, m_oDatasetCreationOptions);
        }
        else if (EQUAL(psIter->pszValue, "LayerCreationOption"))
        {
            SetNameValue(psIter, m_oLayerCreationOptions);
        }
        else if (EQUAL(psIter->pszValue, "Attribute"))
        {
            netCDFWriterConfigAttribute oAtt;
            if (oAtt.Parse(psIter))
                m_aoAttributes.push_back(std::move(oAtt));
        }
        else if (EQUAL(psIter->pszValue, "Field"))
        {
            netCDFWriterConfigField oField;
            if (oField.Parse(psIter))
                m_oFields[!oField.m_osName.empty()
                              ? oField.m_osName
                              : CPLString("__") + oField.m_osNetCDFName] =
                    oField;
        }
        else if (EQUAL(psIter->pszValue, "Layer"))
        {
            netCDFWriterConfigLayer oLayer;
            if (oLayer.Parse(psIter))
                m_oLayers[oLayer.m_osName] = std::move(oLayer);
        }
        else
        {
            CPLDebug("GDAL_netCDF", "Ignoring %s", psIter->pszValue);
        }
    }

    m_bIsValid = true;

    return true;
}

bool netCDFWriterConfigAttribute::Parse(CPLXMLNode *psNode)
{
    const char *pszName = CPLGetXMLValue(psNode, "name", nullptr);
    const char *pszValue = CPLGetXMLValue(psNode, "value", nullptr);
    const char *pszType = CPLGetXMLValue(psNode, "type", "string");
    if (!EQUAL(pszType, "string") && !EQUAL(pszType, "integer") &&
        !EQUAL(pszType, "double"))
    {
        CPLError(CE_Failure, CPLE_NotSupported, "type='%s' unsupported",
                 pszType);
        return false;
    }
    if (pszName == nullptr || pszValue == nullptr)
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Missing name/value");
        return false;
    }
    m_osName = pszName;
    m_osValue = pszValue;
    m_osType = pszType;
    return true;
}

bool netCDFWriterConfigField::Parse(CPLXMLNode *psNode)
{
    const char *pszName = CPLGetXMLValue(psNode, "name", nullptr);
    const char *pszNetCDFName = CPLGetXMLValue(psNode, "netcdf_name", pszName);
    const char *pszMainDim = CPLGetXMLValue(psNode, "main_dim", nullptr);
    if (pszName == nullptr && pszNetCDFName == nullptr)
    {
        CPLError(CE_Failure, CPLE_IllegalArg,
                 "Bot name and netcdf_name are missing");
        return false;
    }
    if (pszName != nullptr)
        m_osName = pszName;
    if (pszNetCDFName != nullptr)
        m_osNetCDFName = pszNetCDFName;
    if (pszMainDim != nullptr)
        m_osMainDim = pszMainDim;

    for (CPLXMLNode *psIter = psNode->psChild; psIter != nullptr;
         psIter = psIter->psNext)
    {
        if (psIter->eType != CXT_Element)
            continue;
        if (EQUAL(psIter->pszValue, "Attribute"))
        {
            netCDFWriterConfigAttribute oAtt;
            if (oAtt.Parse(psIter))
                m_aoAttributes.push_back(std::move(oAtt));
        }
        else
        {
            CPLDebug("GDAL_netCDF", "Ignoring %s", psIter->pszValue);
        }
    }

    return true;
}

bool netCDFWriterConfigLayer::Parse(CPLXMLNode *psNode)
{
    const char *pszName = CPLGetXMLValue(psNode, "name", nullptr);
    const char *pszNetCDFName = CPLGetXMLValue(psNode, "netcdf_name", pszName);
    if (pszName == nullptr)
    {
        CPLError(CE_Failure, CPLE_IllegalArg, "Missing name");
        return false;
    }
    m_osName = pszName;
    if (pszNetCDFName != nullptr)
        m_osNetCDFName = pszNetCDFName;

    for (CPLXMLNode *psIter = psNode->psChild; psIter != nullptr;
         psIter = psIter->psNext)
    {
        if (psIter->eType != CXT_Element)
            continue;
        if (EQUAL(psIter->pszValue, "LayerCreationOption"))
        {
            netCDFWriterConfiguration::SetNameValue(psIter,
                                                    m_oLayerCreationOptions);
        }
        else if (EQUAL(psIter->pszValue, "Attribute"))
        {
            netCDFWriterConfigAttribute oAtt;
            if (oAtt.Parse(psIter))
                m_aoAttributes.push_back(std::move(oAtt));
        }
        else if (EQUAL(psIter->pszValue, "Field"))
        {
            netCDFWriterConfigField oField;
            if (oField.Parse(psIter))
                m_oFields[!oField.m_osName.empty()
                              ? oField.m_osName
                              : CPLString("__") + oField.m_osNetCDFName] =
                    std::move(oField);
        }
        else
        {
            CPLDebug("GDAL_netCDF", "Ignoring %s", psIter->pszValue);
        }
    }

    return true;
}
