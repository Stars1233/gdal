/******************************************************************************
 *
 * Purpose:  Implementation of the CPCIDSK_TEX class.
 *
 ******************************************************************************
 * Copyright (c) 2010
 * PCI Geomatics, 90 Allstate Parkway, Markham, Ontario, Canada.
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "pcidsk_exception.h"
#include "segment/cpcidsk_tex.h"
#include <cassert>
#include <cstring>

using namespace PCIDSK;

PCIDSK_TEX::~PCIDSK_TEX() = default;

/************************************************************************/
/*                            CPCIDSK_TEX()                             */
/************************************************************************/

CPCIDSK_TEX::CPCIDSK_TEX( PCIDSKFile *fileIn, int segmentIn,
                          const char *segment_pointer )
        : CPCIDSKSegment( fileIn, segmentIn, segment_pointer )

{
}

/************************************************************************/
/*                            ~CPCIDSK_TEX()                            */
/************************************************************************/

CPCIDSK_TEX::~CPCIDSK_TEX()

{
}

/************************************************************************/
/*                              ReadText()                              */
/************************************************************************/

std::string CPCIDSK_TEX::ReadText()

{
    PCIDSKBuffer seg_data;

    seg_data.SetSize( (int) GetContentSize() );

    ReadFromFile( seg_data.buffer, 0, seg_data.buffer_size );

    int i;
    char *tbuffer = (char *) seg_data.buffer;

    for( i = 0; i < seg_data.buffer_size; i++ )
    {
        if( tbuffer[i] == '\r' )
            tbuffer[i] = '\n';

        if( tbuffer[i] == '\0' )
            break;
    }

    return std::string( (const char *) seg_data.buffer, i );
}

/************************************************************************/
/*                             WriteText()                              */
/************************************************************************/

void CPCIDSK_TEX::WriteText( const std::string &text_in )

{
    // Transform all \n's to \r's (chr(10) to char(13)).
    unsigned int i, i_out = 0;
    std::string text = text_in;

    for( i = 0; i < text.size(); i++ )
    {
        if( text[i] == '\0' )
        {
            text.resize( i );
            break;
        }

        if( text[i] == '\n' && text[i+1] == '\r' )
        {
            text[i_out++] = '\r';
            i++;
        }
        else if( text[i] == '\r' && text[i+1] == '\n' )
        {
            text[i_out++] = '\r';
            i++;
        }
        else if( text[i] == '\n' )
            text[i_out++] = '\r';
        else
            text[i_out++] = text[i];
    }

    text.resize( i_out );

    // make sure we have a newline at the end.

    if( i_out > 0 && text[i_out-1] != '\r' )
        text += "\r";

    // We really *ought* to ensure the rest of the segment
    // is zeroed out to properly adhere to the specification.
    // It might also be prudent to ensure the segment grows
    // in 32K increments to avoid "move to end of file churn"
    // if several text segments are growing a bit at a time
    // though this is uncommon.

    WriteToFile( text.c_str(), 0, text.size() + 1 );
}
