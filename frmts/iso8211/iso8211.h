/******************************************************************************
 *
 * Project:  ISO 8211 Access
 * Purpose:  Main declarations for ISO 8211.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 1999, Frank Warmerdam <warmerdam@pobox.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#ifndef ISO8211_H_INCLUDED
#define ISO8211_H_INCLUDED

#include "cpl_port.h"
#include "cpl_vsi.h"

/**
  General data type
    */
typedef enum
{
    DDFInt,
    DDFFloat,
    DDFString,
    DDFBinaryString
} DDFDataType;

/************************************************************************/
/*      These should really be private to the library ... they are      */
/*      mostly conveniences.                                            */
/************************************************************************/

int CPL_ODLL DDFScanInt(const char *pszString, int nMaxChars);
int CPL_ODLL DDFScanVariable(const char *pszString, int nMaxChars,
                             int nDelimChar);
char CPL_ODLL *DDFFetchVariable(const char *pszString, int nMaxChars,
                                int nDelimChar1, int nDelimChar2,
                                int *pnConsumedChars);

#define DDF_FIELD_TERMINATOR 30
#define DDF_UNIT_TERMINATOR 31

/************************************************************************/
/*                           Predeclarations                            */
/************************************************************************/

class DDFFieldDefn;
class DDFSubfieldDefn;
class DDFRecord;
class DDFField;

/************************************************************************/
/*                              DDFModule                               */
/************************************************************************/

/**
  The primary class for reading ISO 8211 files.  This class contains all
  the information read from the DDR record, and is used to read records
  from the file.
*/

class CPL_ODLL DDFModule
{
  public:
    DDFModule();
    ~DDFModule();

    int Open(const char *pszFilename, int bFailQuietly = FALSE);
    int Create(const char *pszFilename);
    void Close();

    int Initialize(char chInterchangeLevel = '3', char chLeaderIden = 'L',
                   char chCodeExtensionIndicator = 'E',
                   char chVersionNumber = '1', char chAppIndicator = ' ',
                   const char *pszExtendedCharSet = " ! ",
                   int nSizeFieldLength = 3, int nSizeFieldPos = 4,
                   int nSizeFieldTag = 4);

    void Dump(FILE *fp);

    DDFRecord *ReadRecord();
    void Rewind(long nOffset = -1);

    const DDFFieldDefn *FindFieldDefn(const char *) const;

    DDFFieldDefn *FindFieldDefn(const char *name)
    {
        return const_cast<DDFFieldDefn *>(
            const_cast<const DDFModule *>(this)->FindFieldDefn(name));
    }

    /** Fetch the number of defined fields. */

    int GetFieldCount() const
    {
        return nFieldDefnCount;
    }

    DDFFieldDefn *GetField(int);
    void AddField(DDFFieldDefn *poNewFDefn);

    // This is really just for internal use.
    int GetFieldControlLength() const
    {
        return _fieldControlLength;
    }

    void AddCloneRecord(DDFRecord *);
    void RemoveCloneRecord(DDFRecord *);

    // This is just for DDFRecord.
    VSILFILE *GetFP()
    {
        return fpDDF;
    }

    int GetSizeFieldTag() const
    {
        return (int)_sizeFieldTag;
    }

    // Advanced uses for 8211dump/8211createfromxml
    int GetSizeFieldPos() const
    {
        return _sizeFieldPos;
    }

    int GetSizeFieldLength() const
    {
        return _sizeFieldLength;
    }

    char GetInterchangeLevel() const
    {
        return _interchangeLevel;
    }

    char GetLeaderIden() const
    {
        return _leaderIden;
    }

    char GetCodeExtensionIndicator() const
    {
        return _inlineCodeExtensionIndicator;
    }

    char GetVersionNumber() const
    {
        return _versionNumber;
    }

    char GetAppIndicator() const
    {
        return _appIndicator;
    }

    const char *GetExtendedCharSet() const
    {
        return _extendedCharSet;
    }

    void SetFieldControlLength(int nVal)
    {
        _fieldControlLength = nVal;
    }

  private:
    VSILFILE *fpDDF;
    int bReadOnly;
    long nFirstRecordOffset;

    char _interchangeLevel;
    char _inlineCodeExtensionIndicator;
    char _versionNumber;
    char _appIndicator;
    int _fieldControlLength;
    char _extendedCharSet[4];

    int _recLength;
    char _leaderIden;
    int _fieldAreaStart;
    int _sizeFieldLength;
    int _sizeFieldPos;
    int _sizeFieldTag;

    // One DirEntry per field.
    int nFieldDefnCount;
    DDFFieldDefn **papoFieldDefns;

    DDFRecord *poRecord;

    int nCloneCount;
    int nMaxCloneCount;
    DDFRecord **papoClones;
};

/************************************************************************/
/*                             DDFFieldDefn                             */
/************************************************************************/

typedef enum
{
    dsc_elementary,
    dsc_vector,
    dsc_array,
    dsc_concatenated
} DDF_data_struct_code;

typedef enum
{
    dtc_char_string,
    dtc_implicit_point,
    dtc_explicit_point,
    dtc_explicit_point_scaled,
    dtc_char_bit_string,
    dtc_bit_string,
    dtc_mixed_data_type
} DDF_data_type_code;

/**
 * Information from the DDR defining one field.  Note that just because
 * a field is defined for a DDFModule doesn't mean that it actually occurs
 * on any records in the module.  DDFFieldDefns are normally just significant
 * as containers of the DDFSubfieldDefns.
 */

class CPL_ODLL DDFFieldDefn
{
  public:
    DDFFieldDefn();
    ~DDFFieldDefn();

    int Create(const char *pszTag, const char *pszFieldName,
               const char *pszDescription, DDF_data_struct_code eDataStructCode,
               DDF_data_type_code eDataTypeCode,
               const char *pszFormat = nullptr);
    void AddSubfield(DDFSubfieldDefn *poNewSFDefn,
                     int bDontAddToFormat = FALSE);
    void AddSubfield(const char *pszName, const char *pszFormat);
    int GenerateDDREntry(DDFModule *poModule, char **ppachData, int *pnLength);

    int Initialize(DDFModule *poModule, const char *pszTag, int nSize,
                   const char *pachRecord);

    void Dump(FILE *fp);

    /** Fetch a pointer to the field name (tag).
     * @return this is an internal copy and should not be freed.
     */
    const char *GetName() const
    {
        return pszTag;
    }

    /** Fetch a longer description of this field.
     * @return this is an internal copy and should not be freed.
     */
    const char *GetDescription() const
    {
        return _fieldName;
    }

    /** Get the number of subfields. */
    int GetSubfieldCount() const
    {
        return nSubfieldCount;
    }

    const DDFSubfieldDefn *GetSubfield(int i) const;
    const DDFSubfieldDefn *FindSubfieldDefn(const char *) const;

    /**
     * Get the width of this field.  This function isn't normally used
     * by applications.
     *
     * @return The width of the field in bytes, or zero if the field is not
     * apparently of a fixed width.
     */
    int GetFixedWidth() const
    {
        return nFixedWidth;
    }

    /**
     * Fetch repeating flag.
     * @see DDFField::GetRepeatCount()
     * @return TRUE if the field is marked as repeating.
     */
    int IsRepeating() const
    {
        return bRepeatingSubfields;
    }

    static char *ExpandFormat(const char *);

    /** this is just for an S-57 hack for swedish data */
    void SetRepeatingFlag(int n)
    {
        bRepeatingSubfields = n;
    }

    char *GetDefaultValue(int *pnSize);

    const char *GetArrayDescr() const
    {
        return _arrayDescr;
    }

    const char *GetFormatControls() const
    {
        return _formatControls;
    }

    DDF_data_struct_code GetDataStructCode() const
    {
        return _data_struct_code;
    }

    DDF_data_type_code GetDataTypeCode() const
    {
        return _data_type_code;
    }

    void SetFormatControls(const char *pszVal);

  private:
    static char *ExtractSubstring(const char *);

    DDFModule *poModule;
    char *pszTag;

    char *_fieldName;
    char *_arrayDescr;
    char *_formatControls;

    int bRepeatingSubfields;
    int nFixedWidth;  // zero if variable.

    void BuildSubfields();
    int ApplyFormats();

    DDF_data_struct_code _data_struct_code;

    DDF_data_type_code _data_type_code;

    int nSubfieldCount;
    DDFSubfieldDefn **papoSubfields;

    CPL_DISALLOW_COPY_ASSIGN(DDFFieldDefn)
};

/************************************************************************/
/*                           DDFSubfieldDefn                            */
/*                                                                      */
/*      Information from the DDR record for one subfield of a           */
/*      particular field.                                               */
/************************************************************************/

/**
 * Information from the DDR record describing one subfield of a DDFFieldDefn.
 * All subfields of a field will occur in each occurrence of that field
 * (as a DDFField) in a DDFRecord.  Subfield's actually contain formatted
 * data (as instances within a record).
 */

class CPL_ODLL DDFSubfieldDefn
{
  public:
    DDFSubfieldDefn();
    ~DDFSubfieldDefn();

    void SetName(const char *pszName);

    /** Get pointer to subfield name. */
    const char *GetName() const
    {
        return pszName;
    }

    /** Get pointer to subfield format string */
    const char *GetFormat() const
    {
        return pszFormatString;
    }

    int SetFormat(const char *pszFormat);

    /**
     * Get the general type of the subfield.  This can be used to
     * determine which of ExtractFloatData(), ExtractIntData() or
     * ExtractStringData() should be used.
     * @return The subfield type.  One of DDFInt, DDFFloat, DDFString or
     * DDFBinaryString.
     */

    DDFDataType GetType() const
    {
        return eType;
    }

    double ExtractFloatData(const char *pachData, int nMaxBytes,
                            int *pnConsumedBytes) const;
    int ExtractIntData(const char *pachData, int nMaxBytes,
                       int *pnConsumedBytes) const;
    const char *ExtractStringData(const char *pachData, int nMaxBytes,
                                  int *pnConsumedBytes) const;
    int GetDataLength(const char *, int, int *) const;
    void DumpData(const char *pachData, int nMaxBytes, FILE *fp) const;

    int FormatStringValue(char *pachData, int nBytesAvailable, int *pnBytesUsed,
                          const char *pszValue, int nValueLength = -1) const;

    int FormatIntValue(char *pachData, int nBytesAvailable, int *pnBytesUsed,
                       int nNewValue) const;

    int FormatFloatValue(char *pachData, int nBytesAvailable, int *pnBytesUsed,
                         double dfNewValue) const;

    /** Get the subfield width (zero for variable). */
    int GetWidth() const
    {
        return nFormatWidth;
    }  // zero for variable.

    int GetDefaultValue(char *pachData, int nBytesAvailable,
                        int *pnBytesUsed) const;

    void Dump(FILE *fp);

    /**
      Binary format: this is the digit immediately following the B or b for
      binary formats.
      */
    typedef enum
    {
        NotBinary = 0,
        UInt = 1,
        SInt = 2,
        FPReal = 3,
        FloatReal = 4,
        FloatComplex = 5
    } DDFBinaryFormat;

    DDFBinaryFormat GetBinaryFormat() const
    {
        return eBinaryFormat;
    }

  private:
    char *pszName;  // a.k.a. subfield mnemonic
    char *pszFormatString;

    DDFDataType eType;
    DDFBinaryFormat eBinaryFormat;

    /* -------------------------------------------------------------------- */
    /*      bIsVariable determines whether we using the                     */
    /*      chFormatDelimiter (TRUE), or the fixed width (FALSE).           */
    /* -------------------------------------------------------------------- */
    int bIsVariable;

    char chFormatDelimiter;
    int nFormatWidth;

    /* -------------------------------------------------------------------- */
    /*      Fetched string cache.  This is where we hold the values         */
    /*      returned from ExtractStringData().                              */
    /* -------------------------------------------------------------------- */
    mutable int nMaxBufChars;
    mutable char *pachBuffer;
};

/************************************************************************/
/*                              DDFRecord                               */
/*                                                                      */
/*      Class that contains one DR record from a file.  We read into    */
/*      the same record object repeatedly to ensure that repeated       */
/*      leaders can be easily preserved.                                */
/************************************************************************/

/**
 * Contains instance data from one data record (DR).  The data is contained
 * as a list of DDFField instances partitioning the raw data into fields.
 */

class CPL_ODLL DDFRecord
{
  public:
    explicit DDFRecord(DDFModule *);
    ~DDFRecord();

    DDFRecord *Clone();
    DDFRecord *CloneOn(DDFModule *);

    void Dump(FILE *);

    /** Get the number of DDFFields on this record. */
    int GetFieldCount() const
    {
        return nFieldCount;
    }

    const DDFField *FindField(const char *, int = 0) const;

    DDFField *FindField(const char *name, int i = 0)
    {
        return const_cast<DDFField *>(
            const_cast<const DDFRecord *>(this)->FindField(name, i));
    }

    const DDFField *GetField(int) const;

    DDFField *GetField(int i)
    {
        return const_cast<DDFField *>(
            const_cast<const DDFRecord *>(this)->GetField(i));
    }

    int GetIntSubfield(const char *, int, const char *, int,
                       int * = nullptr) const;
    double GetFloatSubfield(const char *, int, const char *, int,
                            int * = nullptr);
    const char *GetStringSubfield(const char *, int, const char *, int,
                                  int * = nullptr);

    int SetIntSubfield(const char *pszField, int iFieldIndex,
                       const char *pszSubfield, int iSubfieldIndex, int nValue);
    int SetStringSubfield(const char *pszField, int iFieldIndex,
                          const char *pszSubfield, int iSubfieldIndex,
                          const char *pszValue, int nValueLength = -1);
    int SetFloatSubfield(const char *pszField, int iFieldIndex,
                         const char *pszSubfield, int iSubfieldIndex,
                         double dfNewValue);

    /** Fetch size of records raw data (GetData()) in bytes. */
    int GetDataSize() const
    {
        return nDataSize;
    }

    /**
     * Fetch the raw data for this record.  The returned pointer is effectively
     * to the data for the first field of the record, and is of size
     * GetDataSize().
     */
    const char *GetData() const
    {
        return pachData;
    }

    /**
     * Fetch the DDFModule with which this record is associated.
     */

    DDFModule *GetModule()
    {
        return poModule;
    }

    int ResizeField(DDFField *poField, int nNewDataSize);
    int DeleteField(DDFField *poField);
    DDFField *AddField(DDFFieldDefn *);

    int CreateDefaultFieldInstance(DDFField *poField, int iIndexWithinField);

    int SetFieldRaw(DDFField *poField, int iIndexWithinField,
                    const char *pachRawData, int nRawDataSize);
    int UpdateFieldRaw(DDFField *poField, int iIndexWithinField,
                       int nStartOffset, int nOldSize, const char *pachRawData,
                       int nRawDataSize);

    int Write();

    // Advanced uses for 8211dump/8211createfromxml
    int GetReuseHeader() const
    {
        return nReuseHeader;
    }

    int GetSizeFieldTag() const
    {
        return _sizeFieldTag;
    }

    int GetSizeFieldPos() const
    {
        return _sizeFieldPos;
    }

    int GetSizeFieldLength() const
    {
        return _sizeFieldLength;
    }

    // void        SetReuseHeader(int bFlag) { nReuseHeader = bFlag; }
    void SetSizeFieldTag(int nVal)
    {
        _sizeFieldTag = nVal;
    }

    void SetSizeFieldPos(int nVal)
    {
        _sizeFieldPos = nVal;
    }

    void SetSizeFieldLength(int nVal)
    {
        _sizeFieldLength = nVal;
    }

    // This is really just for the DDFModule class.
    int Read();
    void Clear();
    void ResetDirectory();

    void RemoveIsCloneFlag()
    {
        bIsClone = FALSE;
    }

  private:
    int ReadHeader();

    DDFModule *poModule;

    int nReuseHeader;

    int nFieldOffset;  // field data area, not dir entries.

    int _sizeFieldTag;
    int _sizeFieldPos;
    int _sizeFieldLength;

    int nDataSize;  // Whole record except leader with header
    char *pachData;

    int nFieldCount;
    DDFField *paoFields;

    int bIsClone;
};

/************************************************************************/
/*                               DDFField                               */
/*                                                                      */
/*      This object represents one field in a DDFRecord.                */
/************************************************************************/

/**
 * This object represents one field in a DDFRecord.  This
 * models an instance of the fields data, rather than its data definition,
 * which is handled by the DDFFieldDefn class.  Note that a DDFField
 * doesn't have DDFSubfield children as you would expect.  To extract
 * subfield values use GetSubfieldData() to find the right data pointer and
 * then use ExtractIntData(), ExtractFloatData() or ExtractStringData().
 */

class CPL_ODLL DDFField
{
  public:
    DDFField() : poDefn(nullptr), nDataSize(0), pachData(nullptr)
    {
    }

    void Initialize(DDFFieldDefn *, const char *pszData, int nSize);

    void Dump(FILE *fp);

    const char *GetSubfieldData(const DDFSubfieldDefn *, int * = nullptr,
                                int = 0) const;

    const char *GetInstanceData(int nInstance, int *pnSize);

    /**
     * Return the pointer to the entire data block for this record. This
     * is an internal copy, and should not be freed by the application.
     */
    const char *GetData() const
    {
        return pachData;
    }

    /** Return the number of bytes in the data block returned by GetData(). */
    int GetDataSize() const
    {
        return nDataSize;
    }

    int GetRepeatCount() const;

    /** Fetch the corresponding DDFFieldDefn. */
    DDFFieldDefn *GetFieldDefn()
    {
        return poDefn;
    }

    /** Fetch the corresponding DDFFieldDefn. */
    const DDFFieldDefn *GetFieldDefn() const
    {
        return poDefn;
    }

  private:
    DDFFieldDefn *poDefn;

    int nDataSize;

    const char *pachData;
};

#endif /* ndef ISO8211_H_INCLUDED */
