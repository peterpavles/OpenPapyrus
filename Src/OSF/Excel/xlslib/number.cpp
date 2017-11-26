/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 *
 * This file is part of xlslib -- A multiplatform, C/C++ library
 * for dynamic generation of Excel(TM) files.
 *
 * Copyright 2004 Yeico S. A. de C. V. All Rights Reserved.
 * Copyright 2008-2011 David Hoerl All Rights Reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification, are
 * permitted provided that the following conditions are met:
 *
 *    1. Redistributions of source code must retain the above copyright notice, this list of
 *       conditions and the following disclaimer.
 *
 *    2. Redistributions in binary form must reproduce the above copyright notice, this list
 *       of conditions and the following disclaimer in the documentation and/or other materials
 *       provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY David Hoerl ''AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL David Hoerl OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 * ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
 * NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
 * ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include "xlslib-internal.h"
#pragma hdrstop

using namespace std;
using namespace xlslib_core;

/*
 *********************************
 *  number_t class implementation
 *********************************
 */
number_t::number_t(CGlobalRecords& gRecords,
				   uint32 rowval,
				   uint32 colval,
				   double numval,
				   xf_t* pxfval) :
	cell_t(gRecords, rowval, colval, pxfval),
	isDouble(true),
	num()
{
	num.dblNum = numval;
}

number_t::number_t(CGlobalRecords& gRecords,
				   uint32 rowval,
				   uint32 colval,
				   int32 numval,
				   xf_t* pxfval) :
	cell_t(gRecords, rowval, colval, pxfval),
	isDouble(false),
	num()
{
	// 536870911 >= numval >= -536870912
	if(numval <= 0x1FFFFFFF && numval >= (int32)0xE0000000) {
		num.intNum = numval;
	} else {
		isDouble	= true;
		num.dblNum	= (double)numval;   // original value
	}
}

number_t::number_t(CGlobalRecords& gRecords,
				   uint32 rowval,
				   uint32 colval,
				   uint32 numval,
				   xf_t* pxfval) :
	cell_t(gRecords, rowval, colval, pxfval),
	isDouble(false),
	num()
{
	// 536870911 >= numval
	if(numval >= 0xE0000000) {
		num.intNum = (int32)numval;
	} else {
		isDouble	= true;
		num.dblNum	= (double)numval;   // original value
	}
}

CUnit* number_t::GetData(CDataStorage &datastore) const
{
	return datastore.MakeCNumber(*this);
}

/*
 *********************************
 *  number_t class implementation
 *********************************
 */
CNumber::CNumber(CDataStorage &datastore, const number_t& numdef) :
	CRecord(datastore)
{
	uint16 type;

	type = numdef.GetIsDouble() ? RECTYPE_NUMBER : RECTYPE_RK;

	SetRecordType(type);

	AddValue16((uint16)numdef.GetRow());
	AddValue16((uint16)numdef.GetCol());
	AddValue16((uint16)numdef.GetXFIndex());

	if(type == RECTYPE_RK) {
		uint32 val;

		val = (uint32)numdef.GetInt() << 2;       // lower two bits for flags
		val |= 0x2;                                     // Integral type
		AddValue32(val);
	} else {
		AddValue64FP(numdef.GetDouble());
	}

	SetRecordLength(GetDataSize()-RECORD_HEADER_SIZE);
}

CNumber::~CNumber()
{
}
