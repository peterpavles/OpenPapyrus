/*
 * parser.c : an XML 1.0 parser, namespaces and validity support are mostly
 *            implemented on top of the SAX interfaces
 *
 * References:
 *   The XML specification:
 *     http://www.w3.org/TR/REC-xml
 *   Original 1.0 version:
 *     http://www.w3.org/TR/1998/REC-xml-19980210
 *   XML second edition working draft
 *     http://www.w3.org/TR/2000/WD-xml-2e-20000814
 *
 * Okay this is a big file, the parser core is around 7000 lines, then it
 * is followed by the progressive parser top routines, then the various
 * high level APIs to call the parser and a few miscellaneous functions.
 * A number of helper functions and deprecated ones have been moved to
 * parserInternals.c to reduce this file size.
 * As much as possible the functions are associated with their relative
 * production in the XML specification. A few productions defining the
 * different ranges of character are actually implanted either in
 * parserInternals.h or parserInternals.c
 * The DOM tree build is realized from the default SAX callbacks in
 * the module SAX.c.
 * The routines doing the validation checks are in valid.c and called either
 * from the SAX callbacks or as standalone functions using a preparsed
 * document.
 *
 * See Copyright for the status of this software.
 *
 * daniel@veillard.com
 */

#define IN_LIBXML
#include "libxml.h"
#pragma hdrstop
#if defined(WIN32) && !defined (__CYGWIN__)
	#define XML_DIR_SEP '\\'
#else
	#define XML_DIR_SEP '/'
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
	#include <libxml/xmlschemastypes.h>
	#include <libxml/relaxng.h>
#endif
#ifdef HAVE_UNISTD_H
	#include <unistd.h>
#endif
#ifdef HAVE_ZLIB_H
	#include <zlib.h>
#endif
#ifdef HAVE_LZMA_H
	#include <lzma.h>
#endif

static void FASTCALL xmlFatalErr(xmlParserCtxt * ctxt, xmlParserErrors error, const char * info);
static xmlParserCtxt * xmlCreateEntityParserCtxtInternal(const xmlChar * URL, const xmlChar * ID, const xmlChar * base, xmlParserCtxt * pctx);

/************************************************************************
*									*
*	Arbitrary limits set in the parser. See XML_PARSE_HUGE		*
*									*
************************************************************************/

#define XML_PARSER_BIG_ENTITY 1000
#define XML_PARSER_LOT_ENTITY 5000

/*
 * XML_PARSER_NON_LINEAR is the threshold where the ratio of parsed entity
 *    replacement over the size in byte of the input indicates that you have
 *    and eponential behaviour. A value of 10 correspond to at least 3 entity
 *    replacement per byte of input.
 */
#define XML_PARSER_NON_LINEAR 10

/*
 * xmlParserEntityCheck
 *
 * Function to check non-linear entity expansion behaviour
 * This is here to detect and stop exponential linear entity expansion
 * This is not a limitation of the parser but a safety
 * boundary feature. It can be disabled with the XML_PARSE_HUGE
 * parser option.
 */
static int xmlParserEntityCheck(xmlParserCtxt * ctxt, size_t size, xmlEntity * ent, size_t replacement)
{
	size_t consumed = 0;
	if(!ctxt || (ctxt->options & XML_PARSE_HUGE))
		return 0;
	if(ctxt->lastError.code == XML_ERR_ENTITY_LOOP)
		return 1;
	/*
	 * This may look absurd but is needed to detect
	 * entities problems
	 */
	if(ent && (ent->etype != XML_INTERNAL_PREDEFINED_ENTITY) && ent->content && (ent->checked == 0)) {
		ulong oldnbent = ctxt->nbentities;
		xmlChar * rep;
		ent->checked = 1;
		rep = xmlStringDecodeEntities(ctxt, ent->content, XML_SUBSTITUTE_REF, 0, 0, 0);
		ent->checked = (ctxt->nbentities - oldnbent + 1) * 2;
		if(rep) {
			if(xmlStrchr(rep, '<'))
				ent->checked |= 1;
			ZFREE(rep);
		}
	}
	if(replacement != 0) {
		if(replacement < XML_MAX_TEXT_LENGTH)
			return 0;
		/*
		 * If the volume of entity copy reaches 10 times the
		 * amount of parsed data and over the large text threshold
		 * then that's very likely to be an abuse.
		 */
		if(ctxt->input) {
			consumed = ctxt->input->consumed + (ctxt->input->cur - ctxt->input->base);
		}
		consumed += ctxt->sizeentities;
		if(replacement < XML_PARSER_NON_LINEAR * consumed)
			return 0;
	}
	else if(size != 0) {
		/*
		 * Do the check based on the replacement size of the entity
		 */
		if(size < XML_PARSER_BIG_ENTITY)
			return 0;
		/*
		 * A limit on the amount of text data reasonably used
		 */
		if(ctxt->input) {
			consumed = ctxt->input->consumed + (ctxt->input->cur - ctxt->input->base);
		}
		consumed += ctxt->sizeentities;
		if((size < XML_PARSER_NON_LINEAR * consumed) && (ctxt->nbentities * 3 < XML_PARSER_NON_LINEAR * consumed))
			return 0;
	}
	else if(ent) {
		/*
		 * use the number of parsed entities in the replacement
		 */
		size = ent->checked / 2;
		/*
		 * The amount of data parsed counting entities size only once
		 */
		if(ctxt->input) {
			consumed = ctxt->input->consumed + (ctxt->input->cur - ctxt->input->base);
		}
		consumed += ctxt->sizeentities;
		/*
		 * Check the density of entities for the amount of data
		 * knowing an entity reference will take at least 3 bytes
		 */
		if(size * 3 < consumed * XML_PARSER_NON_LINEAR)
			return 0;
	}
	else {
		// strange we got no data for checking
		if(!oneof2(ctxt->lastError.code, XML_ERR_UNDECLARED_ENTITY, XML_WAR_UNDECLARED_ENTITY) || (ctxt->nbentities <= 10000))
			return 0;
	}
	xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, 0);
	return 1;
}
/**
 * xmlParserMaxDepth:
 *
 * arbitrary depth limit for the XML documents that we allow to
 * process. This is not a limitation of the parser but a safety
 * boundary feature. It can be disabled with the XML_PARSE_HUGE
 * parser option.
 */
uint xmlParserMaxDepth = 256;

#define SAX2 1
#define XML_PARSER_BIG_BUFFER_SIZE 300
#define XML_PARSER_BUFFER_SIZE 100
#define SAX_COMPAT_MODE BAD_CAST "SAX compatibility mode document"

/**
 * XML_PARSER_CHUNK_SIZE
 *
 * When calling GROW that's the minimal amount of data
 * the parser expected to have received. It is not a hard
 * limit but an optimization when reading strings like Names
 * It is not strictly needed as long as inputs available characters
 * are followed by 0, which should be provided by the I/O level
 */
#define XML_PARSER_CHUNK_SIZE 100

/*
 * List of XML prefixed PI allowed by W3C specs
 */

static const char * xmlW3CPIs[] = {
	"xml-stylesheet",
	"xml-model",
	NULL
};

/* DEPR void xmlParserHandleReference(xmlParserCtxt * ctxt); */
static xmlEntity * xmlParseStringPEReference(xmlParserCtxt * ctxt, const xmlChar ** str);
static xmlParserErrors xmlParseExternalEntityPrivate(xmlDoc * doc, xmlParserCtxt * oldctxt,
    xmlSAXHandlerPtr sax, void * user_data, int depth, const xmlChar * URL, const xmlChar * ID, xmlNode ** list);
static int xmlCtxtUseOptionsInternal(xmlParserCtxt * ctxt, int options, const char * encoding);
#ifdef LIBXML_LEGACY_ENABLED
static void xmlAddEntityReference(xmlEntity * ent, xmlNode * firstNode, xmlNode * lastNode);
#endif /* LIBXML_LEGACY_ENABLED */
static xmlParserErrors xmlParseBalancedChunkMemoryInternal(xmlParserCtxt * oldctxt, const xmlChar * string, void * user_data, xmlNode ** lst);
static int xmlLoadEntityContent(xmlParserCtxt * ctxt, xmlEntity * entity);

/************************************************************************
*									*
*		Some factorized error routines				*
*									*
************************************************************************/

/**
 * xmlErrAttributeDup:
 * @ctxt:  an XML parser context
 * @prefix:  the attribute prefix
 * @localname:  the attribute localname
 *
 * Handle a redefinition of attribute error
 */
static void FASTCALL xmlErrAttributeDup(xmlParserCtxt * ctxt, const xmlChar * prefix, const xmlChar * localname)
{
	if(ctxt && ctxt->disableSAX && (ctxt->instate == XML_PARSER_EOF))
		return;
	if(ctxt)
		ctxt->errNo = XML_ERR_ATTRIBUTE_REDEFINED;
	if(prefix == NULL)
		__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_PARSER, XML_ERR_ATTRIBUTE_REDEFINED, XML_ERR_FATAL, NULL, 0, (const char*)localname, NULL, NULL, 0, 0,
		    "Attribute %s redefined\n", localname);
	else
		__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_PARSER, XML_ERR_ATTRIBUTE_REDEFINED, XML_ERR_FATAL, NULL, 0, (const char*)prefix, (const char*)localname,
		    NULL, 0, 0, "Attribute %s:%s redefined\n", prefix, localname);
	if(ctxt) {
		ctxt->wellFormed = 0;
		if(ctxt->recovery == 0)
			ctxt->disableSAX = 1;
	}
}
/**
 * xmlFatalErr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @extra:  extra information string
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void FASTCALL xmlFatalErr(xmlParserCtxt * ctxt, xmlParserErrors error, const char * info)
{
	const char * errmsg;
	char errstr[129] = "";
	if(ctxt && ctxt->disableSAX && (ctxt->instate == XML_PARSER_EOF))
		return;
	switch(error) {
		case XML_ERR_INVALID_HEX_CHARREF: errmsg = "CharRef: invalid hexadecimal value"; break;
		case XML_ERR_INVALID_DEC_CHARREF: errmsg = "CharRef: invalid decimal value"; break;
		case XML_ERR_INVALID_CHARREF: errmsg = "CharRef: invalid value"; break;
		case XML_ERR_INTERNAL_ERROR: errmsg = "internal error"; break;
		case XML_ERR_PEREF_AT_EOF: errmsg = "PEReference at end of document"; break;
		case XML_ERR_PEREF_IN_PROLOG: errmsg = "PEReference in prolog"; break;
		case XML_ERR_PEREF_IN_EPILOG: errmsg = "PEReference in epilog"; break;
		case XML_ERR_PEREF_NO_NAME: errmsg = "PEReference: no name"; break;
		case XML_ERR_PEREF_SEMICOL_MISSING: errmsg = "PEReference: expecting ';'"; break;
		case XML_ERR_ENTITY_LOOP: errmsg = "Detected an entity reference loop"; break;
		case XML_ERR_ENTITY_NOT_STARTED: errmsg = "EntityValue: \" or ' expected"; break;
		case XML_ERR_ENTITY_PE_INTERNAL: errmsg = "PEReferences forbidden in internal subset"; break;
		case XML_ERR_ENTITY_NOT_FINISHED: errmsg = "EntityValue: \" or ' expected"; break;
		case XML_ERR_ATTRIBUTE_NOT_STARTED: errmsg = "AttValue: \" or ' expected"; break;
		case XML_ERR_LT_IN_ATTRIBUTE: errmsg = "Unescaped '<' not allowed in attributes values"; break;
		case XML_ERR_LITERAL_NOT_STARTED: errmsg = "SystemLiteral \" or ' expected"; break;
		case XML_ERR_LITERAL_NOT_FINISHED: errmsg = "Unfinished System or Public ID \" or ' expected"; break;
		case XML_ERR_MISPLACED_CDATA_END: errmsg = "Sequence ']]>' not allowed in content"; break;
		case XML_ERR_URI_REQUIRED: errmsg = "SYSTEM or PUBLIC, the URI is missing"; break;
		case XML_ERR_PUBID_REQUIRED: errmsg = "PUBLIC, the Public Identifier is missing"; break;
		case XML_ERR_HYPHEN_IN_COMMENT: errmsg = "Comment must not contain '--' (double-hyphen)"; break;
		case XML_ERR_PI_NOT_STARTED: errmsg = "xmlParsePI : no target name"; break;
		case XML_ERR_RESERVED_XML_NAME: errmsg = "Invalid PI name"; break;
		case XML_ERR_NOTATION_NOT_STARTED: errmsg = "NOTATION: Name expected here"; break;
		case XML_ERR_NOTATION_NOT_FINISHED: errmsg = "'>' required to close NOTATION declaration"; break;
		case XML_ERR_VALUE_REQUIRED: errmsg = "Entity value required"; break;
		case XML_ERR_URI_FRAGMENT: errmsg = "Fragment not allowed"; break;
		case XML_ERR_ATTLIST_NOT_STARTED: errmsg = "'(' required to start ATTLIST enumeration"; break;
		case XML_ERR_NMTOKEN_REQUIRED: errmsg = "NmToken expected in ATTLIST enumeration"; break;
		case XML_ERR_ATTLIST_NOT_FINISHED: errmsg = "')' required to finish ATTLIST enumeration"; break;
		case XML_ERR_MIXED_NOT_STARTED: errmsg = "MixedContentDecl : '|' or ')*' expected"; break;
		case XML_ERR_PCDATA_REQUIRED: errmsg = "MixedContentDecl : '#PCDATA' expected"; break;
		case XML_ERR_ELEMCONTENT_NOT_STARTED: errmsg = "ContentDecl : Name or '(' expected"; break;
		case XML_ERR_ELEMCONTENT_NOT_FINISHED: errmsg = "ContentDecl : ',' '|' or ')' expected"; break;
		case XML_ERR_PEREF_IN_INT_SUBSET: errmsg = "PEReference: forbidden within markup decl in internal subset"; break;
		case XML_ERR_GT_REQUIRED: errmsg = "expected '>'"; break;
		case XML_ERR_CONDSEC_INVALID: errmsg = "XML conditional section '[' expected"; break;
		case XML_ERR_EXT_SUBSET_NOT_FINISHED: errmsg = "Content error in the external subset"; break;
		case XML_ERR_CONDSEC_INVALID_KEYWORD: errmsg = "conditional section INCLUDE or IGNORE keyword expected"; break;
		case XML_ERR_CONDSEC_NOT_FINISHED: errmsg = "XML conditional section not closed"; break;
		case XML_ERR_XMLDECL_NOT_STARTED: errmsg = "Text declaration '<?xml' required"; break;
		case XML_ERR_XMLDECL_NOT_FINISHED: errmsg = "parsing XML declaration: '?>' expected"; break;
		case XML_ERR_EXT_ENTITY_STANDALONE: errmsg = "external parsed entities cannot be standalone"; break;
		case XML_ERR_ENTITYREF_SEMICOL_MISSING: errmsg = "EntityRef: expecting ';'"; break;
		case XML_ERR_DOCTYPE_NOT_FINISHED: errmsg = "DOCTYPE improperly terminated"; break;
		case XML_ERR_LTSLASH_REQUIRED: errmsg = "EndTag: '</' not found"; break;
		case XML_ERR_EQUAL_REQUIRED: errmsg = "expected '='"; break;
		case XML_ERR_STRING_NOT_CLOSED: errmsg = "String not closed expecting \" or '"; break;
		case XML_ERR_STRING_NOT_STARTED: errmsg = "String not started expecting ' or \""; break;
		case XML_ERR_ENCODING_NAME: errmsg = "Invalid XML encoding name"; break;
		case XML_ERR_STANDALONE_VALUE: errmsg = "standalone accepts only 'yes' or 'no'"; break;
		case XML_ERR_DOCUMENT_EMPTY: errmsg = "Document is empty"; break;
		case XML_ERR_DOCUMENT_END: errmsg = "Extra content at the end of the document"; break;
		case XML_ERR_NOT_WELL_BALANCED: errmsg = "chunk is not well balanced"; break;
		case XML_ERR_EXTRA_CONTENT: errmsg = "extra content at the end of well balanced chunk"; break;
		case XML_ERR_VERSION_MISSING: errmsg = "Malformed declaration expecting version"; break;
		case XML_ERR_NAME_TOO_LONG: errmsg = "Name too long use XML_PARSE_HUGE option"; break;
#if 0
		case: errmsg = ""; break;
#endif
		default: errmsg = "Unregistered error message";
	}
	if(info == NULL)
		snprintf(errstr, 128, "%s\n", errmsg);
	else
		snprintf(errstr, 128, "%s: %%s\n", errmsg);
	if(ctxt)
		ctxt->errNo = error;
	__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_PARSER, error, XML_ERR_FATAL, NULL, 0, info, NULL, NULL, 0, 0, &errstr[0], info);
	if(ctxt) {
		ctxt->wellFormed = 0;
		if(ctxt->recovery == 0)
			ctxt->disableSAX = 1;
	}
}
/**
 * xmlFatalErrMsg:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void FASTCALL xmlFatalErrMsg(xmlParserCtxt * ctxt, xmlParserErrors error, const char * msg)
{
	if(ctxt && ctxt->disableSAX && ctxt->instate == XML_PARSER_EOF)
		return;
	if(ctxt)
		ctxt->errNo = error;
	__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_PARSER, error, XML_ERR_FATAL, 0, 0, 0, 0, 0, 0, 0, "%s", msg);
	if(ctxt) {
		ctxt->wellFormed = 0;
		if(ctxt->recovery == 0)
			ctxt->disableSAX = 1;
	}
}
/**
 * xmlWarningMsg:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @str1:  extra data
 * @str2:  extra data
 *
 * Handle a warning.
 */
static void FASTCALL xmlWarningMsg(xmlParserCtxt * ctxt, xmlParserErrors error, const char * msg, const xmlChar * str1, const xmlChar * str2)
{
	xmlStructuredErrorFunc schannel = NULL;
	if(ctxt && ctxt->disableSAX && (ctxt->instate == XML_PARSER_EOF))
		return;
	if(ctxt && ctxt->sax && (ctxt->sax->initialized == XML_SAX2_MAGIC))
		schannel = ctxt->sax->serror;
	if(ctxt) {
		__xmlRaiseError(schannel, (ctxt->sax) ? ctxt->sax->warning : NULL,
		    ctxt->userData, ctxt, NULL, XML_FROM_PARSER, error,
		    XML_ERR_WARNING, NULL, 0, (const char*)str1, (const char*)str2, NULL, 0, 0,
		    msg, (const char*)str1, (const char*)str2);
	}
	else {
		__xmlRaiseError(schannel, 0, 0, ctxt, NULL, XML_FROM_PARSER, error,
		    XML_ERR_WARNING, NULL, 0, (const char*)str1, (const char*)str2, NULL, 0, 0,
		    msg, (const char*)str1, (const char*)str2);
	}
}
/**
 * xmlValidityError:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @str1:  extra data
 *
 * Handle a validity error.
 */
static void FASTCALL xmlValidityError(xmlParserCtxt * ctxt, xmlParserErrors error, const char * msg, const xmlChar * str1, const xmlChar * str2)
{
	xmlStructuredErrorFunc schannel = NULL;
	if(ctxt && ctxt->disableSAX && (ctxt->instate == XML_PARSER_EOF))
		return;
	if(ctxt) {
		ctxt->errNo = error;
		if(ctxt->sax && (ctxt->sax->initialized == XML_SAX2_MAGIC))
			schannel = ctxt->sax->serror;
	}
	if(ctxt) {
		__xmlRaiseError(schannel, ctxt->vctxt.error, ctxt->vctxt.userData, ctxt, NULL, XML_FROM_DTD, error, XML_ERR_ERROR, NULL, 0, (const char*)str1,
		    (const char*)str2, NULL, 0, 0, msg, (const char*)str1, (const char*)str2);
		ctxt->valid = 0;
	}
	else {
		__xmlRaiseError(schannel, 0, 0, ctxt, NULL, XML_FROM_DTD, error, XML_ERR_ERROR, NULL, 0, (const char*)str1, (const char*)str2, NULL, 0, 0,
		    msg, (const char*)str1, (const char*)str2);
	}
}
/**
 * xmlFatalErrMsgInt:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @val:  an integer value
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void FASTCALL xmlFatalErrMsgInt(xmlParserCtxt * ctxt, xmlParserErrors error, const char * msg, int val)
{
	if(!ctxt || !ctxt->disableSAX || ctxt->instate != XML_PARSER_EOF) {
		if(ctxt)
			ctxt->errNo = error;
		__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_PARSER, error, XML_ERR_FATAL, 0, 0, 0, 0, 0, val, 0, msg, val);
		if(ctxt) {
			ctxt->wellFormed = 0;
			if(ctxt->recovery == 0)
				ctxt->disableSAX = 1;
		}
	}
}
/**
 * xmlFatalErrMsgStrIntStr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @str1:  an string info
 * @val:  an integer value
 * @str2:  an string info
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void FASTCALL xmlFatalErrMsgStrIntStr(xmlParserCtxt * ctxt, xmlParserErrors error, const char * msg, const xmlChar * str1, int val, const xmlChar * str2)
{
	if(!ctxt || !ctxt->disableSAX || ctxt->instate != XML_PARSER_EOF) {
		if(ctxt)
			ctxt->errNo = error;
		__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_PARSER, error, XML_ERR_FATAL, NULL, 0, (const char*)str1, (const char*)str2, NULL, val, 0, msg, str1, val, str2);
		if(ctxt) {
			ctxt->wellFormed = 0;
			if(ctxt->recovery == 0)
				ctxt->disableSAX = 1;
		}
	}
}
/**
 * xmlFatalErrMsgStr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @val:  a string value
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void FASTCALL xmlFatalErrMsgStr(xmlParserCtxt * ctxt, xmlParserErrors error, const char * msg, const xmlChar * val)
{
	if(ctxt && ctxt->disableSAX && (ctxt->instate == XML_PARSER_EOF))
		return;
	if(ctxt)
		ctxt->errNo = error;
	__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_PARSER, error, XML_ERR_FATAL, NULL, 0, (const char*)val, NULL, NULL, 0, 0, msg, val);
	if(ctxt) {
		ctxt->wellFormed = 0;
		if(ctxt->recovery == 0)
			ctxt->disableSAX = 1;
	}
}
/**
 * xmlErrMsgStr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the error message
 * @val:  a string value
 *
 * Handle a non fatal parser error
 */
static void FASTCALL xmlErrMsgStr(xmlParserCtxt * ctxt, xmlParserErrors error, const char * msg, const xmlChar * val)
{
	if(ctxt && ctxt->disableSAX && (ctxt->instate == XML_PARSER_EOF))
		return;
	if(ctxt)
		ctxt->errNo = error;
	__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_PARSER, error, XML_ERR_ERROR, NULL, 0, (const char*)val, NULL, NULL, 0, 0, msg, val);
}
/**
 * xmlNsErr:
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the message
 * @info1:  extra information string
 * @info2:  extra information string
 *
 * Handle a fatal parser error, i.e. violating Well-Formedness constraints
 */
static void FASTCALL xmlNsErr(xmlParserCtxt * ctxt, xmlParserErrors error, const char * msg, const xmlChar * info1, const xmlChar * info2, const xmlChar * info3)
{
	if(ctxt && ctxt->disableSAX && (ctxt->instate == XML_PARSER_EOF))
		return;
	if(ctxt)
		ctxt->errNo = error;
	__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_NAMESPACE, error, XML_ERR_ERROR, NULL, 0, (const char*)info1, (const char*)info2, (const char*)info3, 0, 0, msg, info1, info2, info3);
	if(ctxt)
		ctxt->nsWellFormed = 0;
}
/**
 * xmlNsWarn
 * @ctxt:  an XML parser context
 * @error:  the error number
 * @msg:  the message
 * @info1:  extra information string
 * @info2:  extra information string
 *
 * Handle a namespace warning error
 */
static void xmlNsWarn(xmlParserCtxt * ctxt, xmlParserErrors error, const char * msg, const xmlChar * info1, const xmlChar * info2, const xmlChar * info3)
{
	if(ctxt && ctxt->disableSAX && (ctxt->instate == XML_PARSER_EOF))
		return;
	__xmlRaiseError(0, 0, 0, ctxt, 0, XML_FROM_NAMESPACE, error, XML_ERR_WARNING, NULL, 0, (const char*)info1, (const char*)info2, (const char*)info3, 0, 0, msg, info1, info2, info3);
}

/************************************************************************
*									*
*		Library wide options					*
*									*
************************************************************************/

/**
 * xmlHasFeature:
 * @feature: the feature to be examined
 *
 * Examines if the library has been compiled with a given feature.
 *
 * Returns a non-zero value if the feature exist, otherwise zero.
 * Returns zero (0) if the feature does not exist or an unknown
 * unknown feature is requested, non-zero otherwise.
 */
int xmlHasFeature(xmlFeature feature)
{
	switch(feature) {
		case XML_WITH_THREAD:
#ifdef LIBXML_THREAD_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_TREE:
#ifdef LIBXML_TREE_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_OUTPUT:
#ifdef LIBXML_OUTPUT_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_PUSH:
#ifdef LIBXML_PUSH_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_READER:
#ifdef LIBXML_READER_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_PATTERN:
#ifdef LIBXML_PATTERN_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_WRITER:
#ifdef LIBXML_WRITER_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_SAX1:
#ifdef LIBXML_SAX1_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_FTP:
#ifdef LIBXML_FTP_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_HTTP:
#ifdef LIBXML_HTTP_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_VALID:
#ifdef LIBXML_VALID_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_HTML:
#ifdef LIBXML_HTML_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_LEGACY:
#ifdef LIBXML_LEGACY_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_C14N:
#ifdef LIBXML_C14N_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_CATALOG:
#ifdef LIBXML_CATALOG_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_XPATH:
#ifdef LIBXML_XPATH_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_XPTR:
#ifdef LIBXML_XPTR_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_XINCLUDE:
#ifdef LIBXML_XINCLUDE_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_ICONV:
#ifdef LIBXML_ICONV_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_ISO8859X:
#ifdef LIBXML_ISO8859X_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_UNICODE:
#ifdef LIBXML_UNICODE_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_REGEXP:
#ifdef LIBXML_REGEXP_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_AUTOMATA:
#ifdef LIBXML_AUTOMATA_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_EXPR:
#ifdef LIBXML_EXPR_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_SCHEMAS:
#ifdef LIBXML_SCHEMAS_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_SCHEMATRON:
#ifdef LIBXML_SCHEMATRON_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_MODULES:
#ifdef LIBXML_MODULES_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_DEBUG:
#ifdef LIBXML_DEBUG_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_DEBUG_MEM:
#ifdef DEBUG_MEMORY_LOCATION
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_DEBUG_RUN:
#ifdef LIBXML_DEBUG_RUNTIME
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_ZLIB:
#ifdef LIBXML_ZLIB_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_LZMA:
#ifdef LIBXML_LZMA_ENABLED
		    return 1;
#else
		    return 0;
#endif
		case XML_WITH_ICU:
#ifdef LIBXML_ICU_ENABLED
		    return 1;
#else
		    return 0;
#endif
		default:
		    break;
	}
	return 0;
}

/************************************************************************
*									*
*		SAX2 defaulted attributes handling			*
*									*
************************************************************************/

/**
 * xmlDetectSAX2:
 * @ctxt:  an XML parser context
 *
 * Do the SAX2 detection and specific intialization
 */
/* @sobolev static*/ void FASTCALL xmlDetectSAX2(xmlParserCtxt * pCtxt)
{
	if(pCtxt) {
	#ifdef LIBXML_SAX1_ENABLED
		if((pCtxt->sax) && (pCtxt->sax->initialized == XML_SAX2_MAGIC) && (pCtxt->sax->startElementNs || pCtxt->sax->endElementNs))
			pCtxt->sax2 = 1;
	#else
		ctxt->sax2 = 1;
	#endif /* LIBXML_SAX1_ENABLED */
		pCtxt->str_xml = xmlDictLookup(pCtxt->dict, BAD_CAST "xml", 3);
		pCtxt->str_xmlns = xmlDictLookup(pCtxt->dict, BAD_CAST "xmlns", 5);
		pCtxt->str_xml_ns = xmlDictLookup(pCtxt->dict, XML_XML_NAMESPACE, 36);
		if(!pCtxt->str_xml || !pCtxt->str_xmlns || !pCtxt->str_xml_ns)
			xmlErrMemory(pCtxt, 0);
	}
}

//typedef struct _xmlDefAttrs xmlDefAttrs;
struct xmlDefAttrs {
	int    nbAttrs;    /* number of defaulted attributes on that element */
	int    maxAttrs;   /* the size of the array */
	const  xmlChar * values[5]; /* array of localname/prefix/values/external */
};

typedef xmlDefAttrs * xmlDefAttrsPtr;
/**
 * xmlAttrNormalizeSpace:
 * @src: the source string
 * @dst: the target string
 *
 * Normalize the space in non CDATA attribute values:
 * If the attribute type is not CDATA, then the XML processor MUST further
 * process the normalized attribute value by discarding any leading and
 * trailing space (#x20) characters, and by replacing sequences of space
 * (#x20) characters by a single space (#x20) character.
 * Note that the size of dst need to be at least src, and if one doesn't need
 * to preserve dst (and it doesn't come from a dictionary or read-only) then
 * passing src as dst is just fine.
 *
 * Returns a pointer to the normalized value (dst) or NULL if no conversion
 *         is needed.
 */
static xmlChar * FASTCALL xmlAttrNormalizeSpace(const xmlChar * src, xmlChar * dst)
{
	if(!src || !dst)
		return 0;
	else {
		while(*src == 0x20)
			src++;
		while(*src != 0) {
			if(*src == 0x20) {
				while(*src == 0x20)
					src++;
				if(*src != 0)
					*dst++ = 0x20;
			}
			else {
				*dst++ = *src++;
			}
		}
		*dst = 0;
		return (dst == src) ? 0 : dst;
	}
}
/**
 * xmlAttrNormalizeSpace2:
 * @src: the source string
 *
 * Normalize the space in non CDATA attribute values, a slightly more complex
 * front end to avoid allocation problems when running on attribute values
 * coming from the input.
 *
 * Returns a pointer to the normalized value (dst) or NULL if no conversion
 *         is needed.
 */
static const xmlChar * xmlAttrNormalizeSpace2(xmlParserCtxt * ctxt, xmlChar * src, int * len)
{
	int i;
	int remove_head = 0;
	int need_realloc = 0;
	const xmlChar * cur;
	if(!ctxt || (src == NULL) || (len == NULL))
		return 0;
	i = *len;
	if(i <= 0)
		return 0;
	cur = src;
	while(*cur == 0x20) {
		cur++;
		remove_head++;
	}
	while(*cur != 0) {
		if(*cur == 0x20) {
			cur++;
			if((*cur == 0x20) || (*cur == 0)) {
				need_realloc = 1;
				break;
			}
		}
		else
			cur++;
	}
	if(need_realloc) {
		xmlChar * ret = xmlStrndup(src + remove_head, i - remove_head + 1);
		if(!ret) {
			xmlErrMemory(ctxt, 0);
			return 0;
		}
		xmlAttrNormalizeSpace(ret, ret);
		*len = (int)strlen((const char*)ret);
		return ret;
	}
	else if(remove_head) {
		*len -= remove_head;
		memmove(src, src + remove_head, 1 + *len);
		return(src);
	}
	return 0;
}
/**
 * xmlAddDefAttrs:
 * @ctxt:  an XML parser context
 * @fullname:  the element fullname
 * @fullattr:  the attribute fullname
 * @value:  the attribute value
 *
 * Add a defaulted attribute for an element
 */
static void xmlAddDefAttrs(xmlParserCtxt * ctxt, const xmlChar * fullname, const xmlChar * fullattr, const xmlChar * value)
{
	xmlDefAttrsPtr defaults;
	int len;
	const xmlChar * name;
	const xmlChar * prefix;
	/*
	 * Allows to detect attribute redefinitions
	 */
	if(ctxt->attsSpecial) {
		if(xmlHashLookup2(ctxt->attsSpecial, fullname, fullattr))
			return;
	}
	if(ctxt->attsDefault == NULL) {
		ctxt->attsDefault = xmlHashCreateDict(10, ctxt->dict);
		if(ctxt->attsDefault == NULL)
			goto mem_error;
	}
	/*
	 * split the element name into prefix:localname , the string found
	 * are within the DTD and then not associated to namespace names.
	 */
	name = xmlSplitQName3(fullname, &len);
	if(!name) {
		name = xmlDictLookupSL(ctxt->dict, fullname);
		prefix = NULL;
	}
	else {
		name = xmlDictLookupSL(ctxt->dict, name);
		prefix = xmlDictLookup(ctxt->dict, fullname, len);
	}
	/*
	 * make sure there is some storage
	 */
	defaults = (xmlDefAttrsPtr)xmlHashLookup2(ctxt->attsDefault, name, prefix);
	if(defaults == NULL) {
		defaults = (xmlDefAttrsPtr)SAlloc::M(sizeof(xmlDefAttrs) + (4 * 5) * sizeof(const xmlChar *));
		if(defaults == NULL)
			goto mem_error;
		defaults->nbAttrs = 0;
		defaults->maxAttrs = 4;
		if(xmlHashUpdateEntry2(ctxt->attsDefault, name, prefix, defaults, NULL) < 0) {
			SAlloc::F(defaults);
			goto mem_error;
		}
	}
	else if(defaults->nbAttrs >= defaults->maxAttrs) {
		xmlDefAttrsPtr temp = (xmlDefAttrsPtr)SAlloc::R(defaults, sizeof(xmlDefAttrs) + (2 * defaults->maxAttrs * 5) * sizeof(const xmlChar *));
		if(temp == NULL)
			goto mem_error;
		defaults = temp;
		defaults->maxAttrs *= 2;
		if(xmlHashUpdateEntry2(ctxt->attsDefault, name, prefix, defaults, NULL) < 0) {
			SAlloc::F(defaults);
			goto mem_error;
		}
	}
	/*
	 * Split the element name into prefix:localname , the string found
	 * are within the DTD and hen not associated to namespace names.
	 */
	name = xmlSplitQName3(fullattr, &len);
	if(!name) {
		name = xmlDictLookupSL(ctxt->dict, fullattr);
		prefix = NULL;
	}
	else {
		name = xmlDictLookupSL(ctxt->dict, name);
		prefix = xmlDictLookup(ctxt->dict, fullattr, len);
	}
	defaults->values[5 * defaults->nbAttrs] = name;
	defaults->values[5 * defaults->nbAttrs + 1] = prefix;
	// intern the string and precompute the end 
	len = sstrlen(value);
	value = xmlDictLookup(ctxt->dict, value, len);
	defaults->values[5 * defaults->nbAttrs + 2] = value;
	defaults->values[5 * defaults->nbAttrs + 3] = value + len;
	defaults->values[5 * defaults->nbAttrs + 4] = ctxt->external ? (BAD_CAST "external") : 0;
	defaults->nbAttrs++;
	return;
mem_error:
	xmlErrMemory(ctxt, 0);
	return;
}
/**
 * xmlAddSpecialAttr:
 * @ctxt:  an XML parser context
 * @fullname:  the element fullname
 * @fullattr:  the attribute fullname
 * @type:  the attribute type
 *
 * Register this attribute type
 */
static void xmlAddSpecialAttr(xmlParserCtxt * ctxt, const xmlChar * fullname, const xmlChar * fullattr, int type)
{
	if(ctxt->attsSpecial == NULL) {
		ctxt->attsSpecial = xmlHashCreateDict(10, ctxt->dict);
		if(ctxt->attsSpecial == NULL)
			goto mem_error;
	}
	if(xmlHashLookup2(ctxt->attsSpecial, fullname, fullattr))
		return;
	xmlHashAddEntry2(ctxt->attsSpecial, fullname, fullattr, (void*)(long)type);
	return;
mem_error:
	xmlErrMemory(ctxt, 0);
	return;
}
/**
 * xmlCleanSpecialAttrCallback:
 *
 * Removes CDATA attributes from the special attribute table
 */
static void xmlCleanSpecialAttrCallback(void * payload, void * data,
    const xmlChar * fullname, const xmlChar * fullattr, const xmlChar * unused ATTRIBUTE_UNUSED)
{
	xmlParserCtxt * ctxt = (xmlParserCtxt *)data;
	if(((long)payload) == XML_ATTRIBUTE_CDATA) {
		xmlHashRemoveEntry2(ctxt->attsSpecial, fullname, fullattr, 0);
	}
}
/**
 * xmlCleanSpecialAttr:
 * @ctxt:  an XML parser context
 *
 * Trim the list of attributes defined to remove all those of type
 * CDATA as they are not special. This call should be done when finishing
 * to parse the DTD and before starting to parse the document root.
 */
static void FASTCALL xmlCleanSpecialAttr(xmlParserCtxt * ctxt)
{
	if(ctxt->attsSpecial) {
		xmlHashScanFull(ctxt->attsSpecial, xmlCleanSpecialAttrCallback, ctxt);
		if(xmlHashSize(ctxt->attsSpecial) == 0) {
			xmlHashFree(ctxt->attsSpecial, 0);
			ctxt->attsSpecial = NULL;
		}
	}
}
/**
 * xmlCheckLanguageID:
 * @lang:  pointer to the string value
 *
 * Checks that the value conforms to the LanguageID production:
 *
 * NOTE: this is somewhat deprecated, those productions were removed from
 *       the XML Second edition.
 *
 * [33] LanguageID ::= Langcode ('-' Subcode)*
 * [34] Langcode ::= ISO639Code |  IanaCode |  UserCode
 * [35] ISO639Code ::= ([a-z] | [A-Z]) ([a-z] | [A-Z])
 * [36] IanaCode ::= ('i' | 'I') '-' ([a-z] | [A-Z])+
 * [37] UserCode ::= ('x' | 'X') '-' ([a-z] | [A-Z])+
 * [38] Subcode ::= ([a-z] | [A-Z])+
 *
 * The current REC reference the sucessors of RFC 1766, currently 5646
 *
 * http://www.rfc-editor.org/rfc/rfc5646.txt
 * langtag       = language
 *                 ["-" script]
 *                 ["-" region]
 *                 *("-" variant)
 *                 *("-" extension)
 *                 ["-" privateuse]
 * language      = 2*3ALPHA            ; shortest ISO 639 code
 *                 ["-" extlang]       ; sometimes followed by
 *                                     ; extended language subtags
 *               / 4ALPHA              ; or reserved for future use
 *               / 5*8ALPHA            ; or registered language subtag
 *
 * extlang       = 3ALPHA              ; selected ISO 639 codes
 *                 *2("-" 3ALPHA)      ; permanently reserved
 *
 * script        = 4ALPHA              ; ISO 15924 code
 *
 * region        = 2ALPHA              ; ISO 3166-1 code
 *               / 3DIGIT              ; UN M.49 code
 *
 * variant       = 5*8alphanum         ; registered variants
 *               / (DIGIT 3alphanum)
 *
 * extension     = singleton 1*("-" (2*8alphanum))
 *
 *                                     ; Single alphanumerics
 *                                     ; "x" reserved for private use
 * singleton     = DIGIT               ; 0 - 9
 *               / %x41-57             ; A - W
 *               / %x59-5A             ; Y - Z
 *               / %x61-77             ; a - w
 *               / %x79-7A             ; y - z
 *
 * it sounds right to still allow Irregular i-xxx IANA and user codes too
 * The parser below doesn't try to cope with extension or privateuse
 * that could be added but that's not interoperable anyway
 *
 * Returns 1 if correct 0 otherwise
 **/
int xmlCheckLanguageID(const xmlChar * lang)
{
	const xmlChar * cur = lang;
	const xmlChar * nxt;
	if(!cur)
		return 0;
	// @sobolev if(((cur[0] == 'i') && (cur[1] == '-')) || ((cur[0] == 'I') && (cur[1] == '-')) || ((cur[0] == 'x') && (cur[1] == '-')) || ((cur[0] == 'X') && (cur[1] == '-'))) {
	if(oneof4(cur[0], 'i', 'I', 'x', 'X') && cur[1] == '-') { // @sobolev 
		/*
		 * Still allow IANA code and user code which were coming
		 * from the previous version of the XML-1.0 specification
		 * it's deprecated but we should not fail
		 */
		cur += 2;
		while(IsLetterASCII(cur[0]))
			cur++;
		return(cur[0] == 0);
	}
	nxt = cur;
	while(IsLetterASCII(nxt[0]))
		nxt++;
	if(nxt - cur >= 4) {
		/*
		 * Reserved
		 */
		return ((nxt - cur > 8) || nxt[0]) ? 0 : 1;
	}
	if(nxt - cur < 2)
		return 0;
	/* we got an ISO 639 code */
	if(nxt[0] == 0)
		return 1;
	if(nxt[0] != '-')
		return 0;
	nxt++;
	cur = nxt;
	/* now we can have extlang or script or region or variant */
	if((nxt[0] >= '0') && (nxt[0] <= '9'))
		goto region_m49;
	while(IsLetterASCII(nxt[0]))
		nxt++;
	if(nxt - cur == 4)
		goto script;
	if(nxt - cur == 2)
		goto region;
	if((nxt - cur >= 5) && (nxt - cur <= 8))
		goto variant;
	if(nxt - cur != 3)
		return 0;
	/* we parsed an extlang */
	if(nxt[0] == 0)
		return 1;
	if(nxt[0] != '-')
		return 0;
	nxt++;
	cur = nxt;
	/* now we can have script or region or variant */
	if((nxt[0] >= '0') && (nxt[0] <= '9'))
		goto region_m49;
	while(IsLetterASCII(nxt[0]))
		nxt++;
	if(nxt - cur == 2)
		goto region;
	if((nxt - cur >= 5) && (nxt - cur <= 8))
		goto variant;
	if(nxt - cur != 4)
		return 0;
	/* we parsed a script */
script:
	if(nxt[0] == 0)
		return 1;
	if(nxt[0] != '-')
		return 0;
	nxt++;
	cur = nxt;
	/* now we can have region or variant */
	if((nxt[0] >= '0') && (nxt[0] <= '9'))
		goto region_m49;
	while(IsLetterASCII(nxt[0]))
		nxt++;
	if((nxt - cur >= 5) && (nxt - cur <= 8))
		goto variant;
	if(nxt - cur != 2)
		return 0;
	/* we parsed a region */
region:
	if(nxt[0] == 0)
		return 1;
	if(nxt[0] != '-')
		return 0;
	nxt++;
	cur = nxt;
	/* now we can just have a variant */
	while(IsLetterASCII(nxt[0]))
		nxt++;
	if((nxt - cur < 5) || (nxt - cur > 8))
		return 0;
	/* we parsed a variant */
variant:
	if(nxt[0] == 0)
		return 1;
	if(nxt[0] != '-')
		return 0;
	/* extensions and private use subtags not checked */
	return 1;
region_m49:
	if(((nxt[1] >= '0') && (nxt[1] <= '9')) && ((nxt[2] >= '0') && (nxt[2] <= '9'))) {
		nxt += 3;
		goto region;
	}
	return 0;
}

/************************************************************************
*									*
*		Parser stacks related functions and macros		*
*									*
************************************************************************/

static xmlEntity * xmlParseStringEntityRef(xmlParserCtxt * pCtxt, const xmlChar ** ppStr);

#ifdef SAX2
/**
 * nsPush:
 * @ctxt:  an XML parser context
 * @prefix:  the namespace prefix or NULL
 * @URL:  the namespace name
 *
 * Pushes a new parser namespace on top of the ns stack
 *
 * Returns -1 in case of error, -2 if the namespace should be discarded
 *	   and the index in the stack otherwise.
 */
static int nsPush(xmlParserCtxt * ctxt, const xmlChar * prefix, const xmlChar * URL)
{
	if(ctxt->options & XML_PARSE_NSCLEAN) {
		for(int i = ctxt->nsNr - 2; i >= 0; i -= 2) {
			if(ctxt->nsTab[i] == prefix) {
				/* in scope */
				if(ctxt->nsTab[i + 1] == URL)
					return -2;
				/* out of scope keep it */
				break;
			}
		}
	}
	if((ctxt->nsMax == 0) || (ctxt->nsTab == NULL)) {
		ctxt->nsMax = 10;
		ctxt->nsNr = 0;
		ctxt->nsTab = (const xmlChar**)SAlloc::M(ctxt->nsMax * sizeof(xmlChar *));
		if(ctxt->nsTab == NULL) {
			xmlErrMemory(ctxt, 0);
			ctxt->nsMax = 0;
			return -1;
		}
	}
	else if(ctxt->nsNr >= ctxt->nsMax) {
		const xmlChar ** tmp;
		ctxt->nsMax *= 2;
		tmp = (const xmlChar**)SAlloc::R((char*)ctxt->nsTab, ctxt->nsMax * sizeof(ctxt->nsTab[0]));
		if(!tmp) {
			xmlErrMemory(ctxt, 0);
			ctxt->nsMax /= 2;
			return -1;
		}
		ctxt->nsTab = tmp;
	}
	ctxt->nsTab[ctxt->nsNr++] = prefix;
	ctxt->nsTab[ctxt->nsNr++] = URL;
	return (ctxt->nsNr);
}

/**
 * nsPop:
 * @ctxt: an XML parser context
 * @nr:  the number to pop
 *
 * Pops the top @nr parser prefix/namespace from the ns stack
 *
 * Returns the number of namespaces removed
 */
static int FASTCALL nsPop(xmlParserCtxt * ctxt, int nr)
{
	if(ctxt->nsTab == NULL)
		return 0;
	else {
		if(ctxt->nsNr < nr) {
			xmlGenericError(0, "Pbm popping %d NS\n", nr);
			nr = ctxt->nsNr;
		}
		if(ctxt->nsNr <= 0)
			return 0;
		else {
			for(int i = 0; i < nr; i++) {
				ctxt->nsNr--;
				ctxt->nsTab[ctxt->nsNr] = NULL;
			}
			return nr;
		}
	}
}

#endif

static int xmlCtxtGrowAttrs(xmlParserCtxt * ctxt, int nr)
{
	const xmlChar ** atts;
	int * attallocs;
	int maxatts;
	if(ctxt->atts == NULL) {
		maxatts = 55; /* allow for 10 attrs by default */
		atts = (const xmlChar**)SAlloc::M(maxatts * sizeof(xmlChar *));
		if(atts == NULL) goto mem_error;
		ctxt->atts = atts;
		attallocs = (int*)SAlloc::M((maxatts / 5) * sizeof(int));
		if(attallocs == NULL) goto mem_error;
		ctxt->attallocs = attallocs;
		ctxt->maxatts = maxatts;
	}
	else if(nr + 5 > ctxt->maxatts) {
		maxatts = (nr + 5) * 2;
		atts = (const xmlChar**)SAlloc::R((void*)ctxt->atts, maxatts * sizeof(const xmlChar *));
		if(atts == NULL) goto mem_error;
		ctxt->atts = atts;
		attallocs = (int*)SAlloc::R((void*)ctxt->attallocs, (maxatts / 5) * sizeof(int));
		if(attallocs == NULL) goto mem_error;
		ctxt->attallocs = attallocs;
		ctxt->maxatts = maxatts;
	}
	return ctxt->maxatts;
mem_error:
	xmlErrMemory(ctxt, 0);
	return -1;
}

/**
 * inputPush:
 * @ctxt:  an XML parser context
 * @value:  the parser input
 *
 * Pushes a new parser input on top of the input stack
 *
 * Returns -1 in case of error, the index in the stack otherwise
 */
int FASTCALL inputPush(xmlParserCtxt * ctxt, xmlParserInputPtr value)
{
	if(!ctxt || !value)
		return -1;
	else {
		if(ctxt->inputNr >= ctxt->inputMax) {
			ctxt->inputMax *= 2;
			ctxt->inputTab = (xmlParserInput **)SAlloc::R(ctxt->inputTab, ctxt->inputMax * sizeof(ctxt->inputTab[0]));
			if(ctxt->inputTab == NULL) {
				xmlErrMemory(ctxt, 0);
				xmlFreeInputStream(value);
				ctxt->inputMax /= 2;
				value = NULL;
				return -1;
			}
		}
		ctxt->inputTab[ctxt->inputNr] = value;
		ctxt->input = value;
		return (ctxt->inputNr++);
	}
}
/**
 * inputPop:
 * @ctxt: an XML parser context
 *
 * Pops the top parser input from the input stack
 *
 * Returns the input just removed
 */
xmlParserInputPtr inputPop(xmlParserCtxt * ctxt)
{
	xmlParserInputPtr ret = 0;
	if(ctxt) {
		if(ctxt->inputNr > 0) {
			ctxt->inputNr--;
			ctxt->input = (ctxt->inputNr > 0) ? ctxt->inputTab[ctxt->inputNr - 1] : 0;
			ret = ctxt->inputTab[ctxt->inputNr];
			ctxt->inputTab[ctxt->inputNr] = NULL;
		}
	}
	return ret;
}

/**
 * nodePush:
 * @ctxt:  an XML parser context
 * @value:  the element node
 *
 * Pushes a new element node on top of the node stack
 *
 * Returns -1 in case of error, the index in the stack otherwise
 */
int nodePush(xmlParserCtxt * ctxt, xmlNode * value)
{
	if(!ctxt)
		return 0;
	if(ctxt->nodeNr >= ctxt->nodeMax) {
		xmlNode ** tmp = (xmlNode **)SAlloc::R(ctxt->PP_NodeTab, ctxt->nodeMax * 2 * sizeof(ctxt->PP_NodeTab[0]));
		if(!tmp) {
			xmlErrMemory(ctxt, 0);
			return -1;
		}
		ctxt->PP_NodeTab = tmp;
		ctxt->nodeMax *= 2;
	}
	if((((uint)ctxt->nodeNr) > xmlParserMaxDepth) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_INTERNAL_ERROR, "Excessive depth in document: %d use XML_PARSE_HUGE option\n", xmlParserMaxDepth);
		ctxt->instate = XML_PARSER_EOF;
		return -1;
	}
	ctxt->PP_NodeTab[ctxt->nodeNr] = value;
	ctxt->P_Node = value;
	return (ctxt->nodeNr++);
}
/**
 * nodePop:
 * @ctxt: an XML parser context
 *
 * Pops the top element node from the node stack
 *
 * Returns the node just removed
 */
xmlNode * nodePop(xmlParserCtxt * ctxt)
{
	xmlNode * ret = 0;
	if(ctxt && ctxt->nodeNr > 0) {
		ctxt->nodeNr--;
		ctxt->P_Node = (ctxt->nodeNr > 0) ? ctxt->PP_NodeTab[ctxt->nodeNr - 1] : NULL;
		ret = ctxt->PP_NodeTab[ctxt->nodeNr];
		ctxt->PP_NodeTab[ctxt->nodeNr] = NULL;
	}
	return ret;
}

#ifdef LIBXML_PUSH_ENABLED
/**
 * nameNsPush:
 * @ctxt:  an XML parser context
 * @value:  the element name
 * @prefix:  the element prefix
 * @URI:  the element namespace name
 *
 * Pushes a new element name/prefix/URL on top of the name stack
 *
 * Returns -1 in case of error, the index in the stack otherwise
 */
static int nameNsPush(xmlParserCtxt * ctxt, const xmlChar * value, const xmlChar * prefix, const xmlChar * URI, int nsNr)
{
	if(ctxt->nameNr >= ctxt->nameMax) {
		const xmlChar ** tmp;
		void ** tmp2;
		ctxt->nameMax *= 2;
		tmp = (const xmlChar**)SAlloc::R((xmlChar**)ctxt->nameTab,
		    ctxt->nameMax *
		    sizeof(ctxt->nameTab[0]));
		if(!tmp) {
			ctxt->nameMax /= 2;
			goto mem_error;
		}
		ctxt->nameTab = tmp;
		tmp2 = (void**)SAlloc::R((void**)ctxt->pushTab,
		    ctxt->nameMax * 3 *
		    sizeof(ctxt->pushTab[0]));
		if(tmp2 == NULL) {
			ctxt->nameMax /= 2;
			goto mem_error;
		}
		ctxt->pushTab = tmp2;
	}
	ctxt->nameTab[ctxt->nameNr] = value;
	ctxt->name = value;
	ctxt->pushTab[ctxt->nameNr * 3] = (void*)prefix;
	ctxt->pushTab[ctxt->nameNr * 3 + 1] = (void*)URI;
	ctxt->pushTab[ctxt->nameNr * 3 + 2] = (void*)(long)nsNr;
	return (ctxt->nameNr++);
mem_error:
	xmlErrMemory(ctxt, 0);
	return -1;
}
/**
 * nameNsPop:
 * @ctxt: an XML parser context
 *
 * Pops the top element/prefix/URI name from the name stack
 *
 * Returns the name just removed
 */
static const xmlChar * FASTCALL nameNsPop(xmlParserCtxt * ctxt)
{
	const xmlChar * ret = 0;
	if(ctxt->nameNr > 0) {
		ctxt->nameNr--;
		ctxt->name = (ctxt->nameNr > 0) ? ctxt->nameTab[ctxt->nameNr-1] : 0;
		ret = ctxt->nameTab[ctxt->nameNr];
		ctxt->nameTab[ctxt->nameNr] = NULL;
	}
	return ret;
}

#endif /* LIBXML_PUSH_ENABLED */

/**
 * namePush:
 * @ctxt:  an XML parser context
 * @value:  the element name
 *
 * Pushes a new element name on top of the name stack
 *
 * Returns -1 in case of error, the index in the stack otherwise
 */
int namePush(xmlParserCtxt * ctxt, const xmlChar * value)
{
	if(!ctxt)
		return -1;
	if(ctxt->nameNr >= ctxt->nameMax) {
		const xmlChar ** tmp;
		tmp = (const xmlChar**)SAlloc::R((xmlChar**)ctxt->nameTab, ctxt->nameMax * 2 * sizeof(ctxt->nameTab[0]));
		if(!tmp) {
			goto mem_error;
		}
		ctxt->nameTab = tmp;
		ctxt->nameMax *= 2;
	}
	ctxt->nameTab[ctxt->nameNr] = value;
	ctxt->name = value;
	return (ctxt->nameNr++);
mem_error:
	xmlErrMemory(ctxt, 0);
	return -1;
}

/**
 * namePop:
 * @ctxt: an XML parser context
 *
 * Pops the top element name from the name stack
 *
 * Returns the name just removed
 */
const xmlChar * namePop(xmlParserCtxt * ctxt)
{
	const xmlChar * ret = 0;
	if(ctxt && ctxt->nameNr > 0) {
		ctxt->nameNr--;
		ctxt->name = (ctxt->nameNr > 0) ? ctxt->nameTab[ctxt->nameNr - 1] : 0;
		ret = ctxt->nameTab[ctxt->nameNr];
		ctxt->nameTab[ctxt->nameNr] = NULL;
	}
	return ret;
}

static int FASTCALL spacePush(xmlParserCtxt * ctxt, int val)
{
	if(ctxt->spaceNr >= ctxt->spaceMax) {
		ctxt->spaceMax *= 2;
		int * tmp = (int*)SAlloc::R(ctxt->spaceTab, ctxt->spaceMax * sizeof(ctxt->spaceTab[0]));
		if(!tmp) {
			xmlErrMemory(ctxt, 0);
			ctxt->spaceMax /= 2;
			return -1;
		}
		ctxt->spaceTab = tmp;
	}
	ctxt->spaceTab[ctxt->spaceNr] = val;
	ctxt->space = &ctxt->spaceTab[ctxt->spaceNr];
	return(ctxt->spaceNr++);
}

static int spacePop(xmlParserCtxt * ctxt)
{
	int ret = 0;
	if(ctxt->spaceNr > 0) {
		ctxt->spaceNr--;
		ctxt->space = (ctxt->spaceNr > 0) ? &ctxt->spaceTab[ctxt->spaceNr - 1] : &ctxt->spaceTab[0];
		ret = ctxt->spaceTab[ctxt->spaceNr];
		ctxt->spaceTab[ctxt->spaceNr] = -1;
	}
	return ret;
}
/*
 * Macros for accessing the content. Those should be used only by the parser,
 * and not exported.
 *
 * Dirty macros, i.e. one often need to make assumption on the context to
 * use them
 *
 *   CUR_PTR return the current pointer to the xmlChar to be parsed.
 *           To be used with extreme caution since operations consuming
 *           characters may move the input buffer to a different location !
 *   CUR     returns the current xmlChar value, i.e. a 8 bit value if compiled
 *           This should be used internally by the parser
 *           only to compare to ASCII values otherwise it would break when
 *           running with UTF-8 encoding.
 *   RAW     same as CUR but in the input buffer, bypass any token
 *           extraction that may have been done
 *   NXT(n)  returns the n'th next xmlChar. Same as CUR is should be used only
 *           to compare on ASCII based substring.
 *   SKIP(n) Skip n xmlChar, and must also be used only to skip ASCII defined
 *           strings without newlines within the parser.
 *   NEXT1(l) Skip 1 xmlChar, and must also be used only to skip 1 non-newline ASCII
 *           defined char within the parser.
 * Clean macros, not dependent of an ASCII context, expect UTF-8 encoding
 *
 *   NEXT    Skip to the next character, this does the proper decoding
 *           in UTF-8 mode. It also pop-up unfinished entities on the fly.
 *   NEXTL(l) Skip the current unicode character of l xmlChars long.
 *   CUR_CHAR(l) returns the current unicode character (int), set l
 *           to the number of xmlChars used for the encoding [0-5].
 *   CUR_SCHAR  same but operate on a string instead of the context
 *   COPY_BUF  copy the current unicode char to the target buffer, increment
 *            the index
 *   GROW, SHRINK  handling of input buffers
 */

#define RAW (*ctxt->input->cur)
#define CUR (*ctxt->input->cur)
#define NXT(val) ctxt->input->cur[(val)]
#define CUR_PTR ctxt->input->cur

#define CMP4(s, c1, c2, c3, c4)	( ((uchar*)s)[0] == c1 && ((uchar*)s)[1] == c2 && ((uchar*)s)[2] == c3 && ((uchar*)s)[3] == c4 )
#define CMP5(s, c1, c2, c3, c4, c5) (CMP4(s, c1, c2, c3, c4) && ((uchar*)s)[4] == c5)
#define CMP6(s, c1, c2, c3, c4, c5, c6)	(CMP5(s, c1, c2, c3, c4, c5) && ((uchar*)s)[5] == c6)
#define CMP7(s, c1, c2, c3, c4, c5, c6, c7) (CMP6(s, c1, c2, c3, c4, c5, c6) && ((uchar*)s)[6] == c7)
#define CMP8(s, c1, c2, c3, c4, c5, c6, c7, c8)	(CMP7(s, c1, c2, c3, c4, c5, c6, c7) && ((uchar*)s)[7] == c8)
#define CMP9(s, c1, c2, c3, c4, c5, c6, c7, c8, c9) (CMP8(s, c1, c2, c3, c4, c5, c6, c7, c8) && ((uchar*)s)[8] == c9)
#define CMP10(s, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10) (CMP9(s, c1, c2, c3, c4, c5, c6, c7, c8, c9) && ((uchar*)s)[9] == c10)

#define SKIP(val) do {							\
		ctxt->nbChars += (val), ctxt->input->cur += (val), ctxt->input->col += (val);			\
		if(*ctxt->input->cur == '%') xmlParserHandlePEReference(ctxt);	   \
		if((*ctxt->input->cur == 0) && (xmlParserInputGrow(ctxt->input, INPUT_CHUNK) <= 0))	    \
			xmlPopInput(ctxt);					    \
} while(0)

#define SKIPL(val) do {							\
		for(int skipl = 0; skipl<val; skipl++) {				      \
			if(*(ctxt->input->cur) == '\n') { ctxt->input->line++; ctxt->input->col = 1; } else ctxt->input->col++; \
			ctxt->nbChars++;						\
			ctxt->input->cur++;						\
		}								    \
		if(*ctxt->input->cur == '%') xmlParserHandlePEReference(ctxt); \
		if((*ctxt->input->cur == 0) && (xmlParserInputGrow(ctxt->input, INPUT_CHUNK) <= 0)) \
			xmlPopInput(ctxt);					    \
} while(0)

#define SHRINK if((ctxt->progressive == 0) && (ctxt->input->cur - ctxt->input->base > 2 * INPUT_CHUNK) && (ctxt->input->end - ctxt->input->cur < 2 * INPUT_CHUNK)) xmlSHRINK(ctxt);

static void FASTCALL xmlSHRINK(xmlParserCtxt * ctxt) 
{
	xmlParserInputShrink(ctxt->input);
	if((*ctxt->input->cur == 0) && (xmlParserInputGrow(ctxt->input, INPUT_CHUNK) <= 0))
		xmlPopInput(ctxt);
}

#define GROW if((ctxt->progressive == 0) && (ctxt->input->end - ctxt->input->cur < INPUT_CHUNK)) xmlGROW(ctxt);

static void FASTCALL xmlGROW(xmlParserCtxt * ctxt) 
{
	ulong  curEnd = ctxt->input->end - ctxt->input->cur;
	ulong  curBase = ctxt->input->cur - ctxt->input->base;
	if(((curEnd > (ulong)XML_MAX_LOOKUP_LIMIT) || (curBase > (ulong)XML_MAX_LOOKUP_LIMIT)) &&
	    ((ctxt->input->buf) && (ctxt->input->buf->readcallback != (xmlInputReadCallback)xmlNop)) &&
	    ((ctxt->options & XML_PARSE_HUGE) == 0)) {
		xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "Huge input lookup");
		ctxt->instate = XML_PARSER_EOF;
	}
	xmlParserInputGrow(ctxt->input, INPUT_CHUNK);
	if(ctxt->input->cur && (*ctxt->input->cur == 0) && (xmlParserInputGrow(ctxt->input, INPUT_CHUNK) <= 0))
		xmlPopInput(ctxt);
}

#define SKIP_BLANKS xmlSkipBlankChars(ctxt)
#define NEXT xmlNextChar(ctxt)
#define NEXT1 {								\
		ctxt->input->col++;						\
		ctxt->input->cur++;						\
		ctxt->nbChars++;						\
		if(*ctxt->input->cur == 0)				       \
			xmlParserInputGrow(ctxt->input, INPUT_CHUNK);		    \
}

#define NEXTL(l) do {							\
		if(*(ctxt->input->cur) == '\n') {				   \
			ctxt->input->line++; ctxt->input->col = 1;			\
		} else ctxt->input->col++;					    \
		ctxt->input->cur += l;				    \
		if(*ctxt->input->cur == '%') xmlParserHandlePEReference(ctxt);	   \
} while(0)

#define CUR_CHAR(l) xmlCurrentChar(ctxt, &l)
#define CUR_SCHAR(s, l) xmlStringCurrentChar(ctxt, s, &l)
#define COPY_BUF(l, b, i, v) if(l == 1) b[i++] = (xmlChar)v; else i += xmlCopyCharMultiByte(&b[i], v)

/**
 * xmlSkipBlankChars:
 * @ctxt:  the XML parser context
 *
 * skip all blanks character found at that point in the input streams.
 * It pops up finished entities in the process if allowable at that point.
 *
 * Returns the number of space chars skipped
 */
int FASTCALL xmlSkipBlankChars(xmlParserCtxt * ctxt) 
{
	int res = 0;
	// It's Okay to use CUR/NEXT here since all the blanks are on the ASCII range.
	if((ctxt->inputNr == 1) && (ctxt->instate != XML_PARSER_DTD)) {
		// if we are in the document content, go really fast
		const xmlChar * cur = ctxt->input->cur;
		while(IS_BLANK_CH(*cur)) {
			if(*cur == '\n') {
				ctxt->input->line++; 
				ctxt->input->col = 1;
			}
			else {
				ctxt->input->col++;
			}
			cur++;
			res++;
			if(*cur == 0) {
				ctxt->input->cur = cur;
				xmlParserInputGrow(ctxt->input, INPUT_CHUNK);
				cur = ctxt->input->cur;
			}
		}
		ctxt->input->cur = cur;
	}
	else {
		int cur;
		do {
			cur = CUR;
			while(IS_BLANK_CH(cur)) { /* CHECKED tstblanks.xml */
				NEXT;
				cur = CUR;
				res++;
			}
			while((cur == 0) && (ctxt->inputNr > 1) && (ctxt->instate != XML_PARSER_COMMENT)) {
				xmlPopInput(ctxt);
				cur = CUR;
			}
			// Need to handle support of entities branching here
			if(*ctxt->input->cur == '%') 
				xmlParserHandlePEReference(ctxt);
		} while(IS_BLANK(cur)); /* CHECKED tstblanks.xml */
	}
	return res;
}

/************************************************************************
*									*
*		Commodity functions to handle entities			*
*									*
************************************************************************/
/**
 * xmlPopInput:
 * @ctxt:  an XML parser context
 *
 * xmlPopInput: the current input pointed by ctxt->input came to an end
 *          pop it and return the next char.
 *
 * Returns the current xmlChar in the parser context
 */
xmlChar FASTCALL xmlPopInput(xmlParserCtxt * ctxt)
{
	if(!ctxt || (ctxt->inputNr <= 1))
		return 0;
	if(xmlParserDebugEntities)
		xmlGenericError(0, "Popping input %d\n", ctxt->inputNr);
	xmlFreeInputStream(inputPop(ctxt));
	if((*ctxt->input->cur == 0) && (xmlParserInputGrow(ctxt->input, INPUT_CHUNK) <= 0))
		return xmlPopInput(ctxt); // @recursion
	return CUR;
}
/**
 * xmlPushInput:
 * @ctxt:  an XML parser context
 * @input:  an XML parser input fragment (entity, XML fragment ...).
 *
 * xmlPushInput: switch to a new input stream which is stacked on top
 *               of the previous one(s).
 * Returns -1 in case of error or the index in the input stack
 */
int FASTCALL xmlPushInput(xmlParserCtxt * ctxt, xmlParserInput * input)
{
	int ret;
	if(!input)
		return -1;
	if(xmlParserDebugEntities) {
		if(ctxt->input && ctxt->input->filename)
			xmlGenericError(0, "%s(%d): ", ctxt->input->filename, ctxt->input->line);
		xmlGenericError(0, "Pushing input %d : %.30s\n", ctxt->inputNr+1, input->cur);
	}
	ret = inputPush(ctxt, input);
	if(ctxt->instate == XML_PARSER_EOF)
		return -1;
	GROW;
	return ret;
}

/**
 * xmlParseCharRef:
 * @ctxt:  an XML parser context
 *
 * parse Reference declarations
 *
 * [66] CharRef ::= '&#' [0-9]+ ';' |
 *                  '&#x' [0-9a-fA-F]+ ';'
 *
 * [WFC: Legal Character]
 * Characters referred to using character references must match the
 * production for Char.
 *
 * Returns the value parsed (as an int), 0 in case of error
 */
int xmlParseCharRef(xmlParserCtxt * ctxt)
{
	uint val = 0;
	int count = 0;
	uint outofrange = 0;
	/*
	 * Using RAW/CUR/NEXT is okay since we are working on ASCII range here
	 */
	if((RAW == '&') && (NXT(1) == '#') &&
	    (NXT(2) == 'x')) {
		SKIP(3);
		GROW;
		while(RAW != ';') { /* loop blocked by count */
			if(count++ > 20) {
				count = 0;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
			}
			if((RAW >= '0') && (RAW <= '9'))
				val = val * 16 + (CUR - '0');
			else if((RAW >= 'a') && (RAW <= 'f') && (count < 20))
				val = val * 16 + (CUR - 'a') + 10;
			else if((RAW >= 'A') && (RAW <= 'F') && (count < 20))
				val = val * 16 + (CUR - 'A') + 10;
			else {
				xmlFatalErr(ctxt, XML_ERR_INVALID_HEX_CHARREF, 0);
				val = 0;
				break;
			}
			if(val > 0x10FFFF)
				outofrange = val;

			NEXT;
			count++;
		}
		if(RAW == ';') {
			/* on purpose to avoid reentrancy problems with NEXT and SKIP */
			ctxt->input->col++;
			ctxt->nbChars++;
			ctxt->input->cur++;
		}
	}
	else if((RAW == '&') && (NXT(1) == '#')) {
		SKIP(2);
		GROW;
		while(RAW != ';') { /* loop blocked by count */
			if(count++ > 20) {
				count = 0;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
			}
			if((RAW >= '0') && (RAW <= '9'))
				val = val * 10 + (CUR - '0');
			else {
				xmlFatalErr(ctxt, XML_ERR_INVALID_DEC_CHARREF, 0);
				val = 0;
				break;
			}
			if(val > 0x10FFFF)
				outofrange = val;

			NEXT;
			count++;
		}
		if(RAW == ';') {
			/* on purpose to avoid reentrancy problems with NEXT and SKIP */
			ctxt->input->col++;
			ctxt->nbChars++;
			ctxt->input->cur++;
		}
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_CHARREF, 0);
	}

	/*
	 * [WFC: Legal Character]
	 * Characters referred to using character references must match the
	 * production for Char.
	 */
	if((IS_CHAR(val) && (outofrange == 0))) {
		return val;
	}
	else {
		xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR, "xmlParseCharRef: invalid xmlChar value %d\n", val);
	}
	return 0;
}

/**
 * xmlParseStringCharRef:
 * @ctxt:  an XML parser context
 * @str:  a pointer to an index in the string
 *
 * parse Reference declarations, variant parsing from a string rather
 * than an an input flow.
 *
 * [66] CharRef ::= '&#' [0-9]+ ';' |
 *                  '&#x' [0-9a-fA-F]+ ';'
 *
 * [WFC: Legal Character]
 * Characters referred to using character references must match the
 * production for Char.
 *
 * Returns the value parsed (as an int), 0 in case of error, str will be
 *         updated to the current value of the index
 */
static int xmlParseStringCharRef(xmlParserCtxt * ctxt, const xmlChar ** str)
{
	const xmlChar * ptr;
	xmlChar cur;
	uint val = 0;
	uint outofrange = 0;
	if((str == NULL) || (*str == NULL))
		return 0;
	ptr = *str;
	cur = *ptr;
	if((cur == '&') && (ptr[1] == '#') && (ptr[2] == 'x')) {
		ptr += 3;
		cur = *ptr;
		while(cur != ';') { /* Non input consuming loop */
			if((cur >= '0') && (cur <= '9'))
				val = val * 16 + (cur - '0');
			else if((cur >= 'a') && (cur <= 'f'))
				val = val * 16 + (cur - 'a') + 10;
			else if((cur >= 'A') && (cur <= 'F'))
				val = val * 16 + (cur - 'A') + 10;
			else {
				xmlFatalErr(ctxt, XML_ERR_INVALID_HEX_CHARREF, 0);
				val = 0;
				break;
			}
			if(val > 0x10FFFF)
				outofrange = val;

			ptr++;
			cur = *ptr;
		}
		if(cur == ';')
			ptr++;
	}
	else if((cur == '&') && (ptr[1] == '#')) {
		ptr += 2;
		cur = *ptr;
		while(cur != ';') { /* Non input consuming loops */
			if((cur >= '0') && (cur <= '9'))
				val = val * 10 + (cur - '0');
			else {
				xmlFatalErr(ctxt, XML_ERR_INVALID_DEC_CHARREF, 0);
				val = 0;
				break;
			}
			if(val > 0x10FFFF)
				outofrange = val;

			ptr++;
			cur = *ptr;
		}
		if(cur == ';')
			ptr++;
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_INVALID_CHARREF, 0);
		return 0;
	}
	*str = ptr;

	/*
	 * [WFC: Legal Character]
	 * Characters referred to using character references must match the
	 * production for Char.
	 */
	if((IS_CHAR(val) && (outofrange == 0))) {
		return val;
	}
	else {
		xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR, "xmlParseStringCharRef: invalid xmlChar value %d\n", val);
	}
	return 0;
}

/**
 * xmlNewBlanksWrapperInputStream:
 * @ctxt:  an XML parser context
 * @entity:  an Entity pointer
 *
 * Create a new input stream for wrapping
 * blanks around a PEReference
 *
 * Returns the new input stream or NULL
 */

static void deallocblankswrapper(xmlChar * str)
{
	SAlloc::F(str);
}

static xmlParserInputPtr xmlNewBlanksWrapperInputStream(xmlParserCtxt * ctxt, xmlEntity * entity)
{
	xmlParserInput * input = 0;
	if(!entity) {
		xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "xmlNewBlanksWrapperInputStream entity\n");
	}
	else {
		if(xmlParserDebugEntities)
			xmlGenericError(0, "new blanks wrapper for entity: %s\n", entity->name);
		input = xmlNewInputStream(ctxt);
		if(input) {
			size_t length = sstrlen(entity->name) + 5;
			xmlChar * buffer = (xmlChar *)SAlloc::M(length);
			if(!buffer) {
				xmlErrMemory(ctxt, 0);
				SAlloc::F(input);
			}
			else {
				buffer[0] = ' ';
				buffer[1] = '%';
				buffer[length-3] = ';';
				buffer[length-2] = ' ';
				buffer[length-1] = 0;
				memcpy(buffer + 2, entity->name, length - 5);
				input->free = deallocblankswrapper;
				input->base = buffer;
				input->cur = buffer;
				input->length = length;
				input->end = &buffer[length];
			}
		}
	}
	return input;
}
/**
 * xmlParserHandlePEReference:
 * @ctxt:  the parser context
 *
 * [69] PEReference ::= '%' Name ';'
 *
 * [WFC: No Recursion]
 * A parsed entity must not contain a recursive
 * reference to itself, either directly or indirectly.
 *
 * [WFC: Entity Declared]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", ...  ... The declaration of a parameter
 * entity must precede any reference to it...
 *
 * [VC: Entity Declared]
 * In a document with an external subset or external parameter entities
 * with "standalone='no'", ...  ... The declaration of a parameter entity
 * must precede any reference to it...
 *
 * [WFC: In DTD]
 * Parameter-entity references may only appear in the DTD.
 * NOTE: misleading but this is handled.
 *
 * A PEReference may have been detected in the current input stream
 * the handling is done accordingly to
 *      http://www.w3.org/TR/REC-xml#entproc
 * i.e.
 *   - Included in literal in entity values
 *   - Included as Parameter Entity reference within DTDs
 */
void FASTCALL xmlParserHandlePEReference(xmlParserCtxt * ctxt)
{
	const xmlChar * name;
	xmlEntity * entity = NULL;
	xmlParserInputPtr input;
	if(RAW != '%')
		return;
	switch(ctxt->instate) {
		case XML_PARSER_CDATA_SECTION: return;
		case XML_PARSER_COMMENT: return;
		case XML_PARSER_START_TAG: return;
		case XML_PARSER_END_TAG: return;
		case XML_PARSER_EOF: xmlFatalErr(ctxt, XML_ERR_PEREF_AT_EOF, 0); return;
		case XML_PARSER_PROLOG:
		case XML_PARSER_START:
		case XML_PARSER_MISC: xmlFatalErr(ctxt, XML_ERR_PEREF_IN_PROLOG, 0); return;
		case XML_PARSER_ENTITY_DECL:
		case XML_PARSER_CONTENT:
		case XML_PARSER_ATTRIBUTE_VALUE:
		case XML_PARSER_PI:
		case XML_PARSER_SYSTEM_LITERAL:
		case XML_PARSER_PUBLIC_LITERAL: return; // we just ignore it there 
		case XML_PARSER_EPILOG: xmlFatalErr(ctxt, XML_ERR_PEREF_IN_EPILOG, 0); return;
		case XML_PARSER_ENTITY_VALUE:
		    /*
		     * NOTE: in the case of entity values, we don't do the
		     *       substitution here since we need the literal
		     *       entity value to be able to save the internal
		     *       subset of the document.
		     *       This will be handled by xmlStringDecodeEntities
		     */
		    return;
		case XML_PARSER_DTD:
		    /*
		     * [WFC: Well-Formedness Constraint: PEs in Internal Subset]
		     * In the internal DTD subset, parameter-entity references
		     * can occur only where markup declarations can occur, not
		     * within markup declarations.
		     * In that case this is handled in xmlParseMarkupDecl
		     */
		    if((ctxt->external == 0) && (ctxt->inputNr == 1))
			    return;
		    if(IS_BLANK_CH(NXT(1)) || NXT(1) == 0)
			    return;
		    break;
		case XML_PARSER_IGNORE:
		    return;
	}
	NEXT;
	name = xmlParseName(ctxt);
	if(xmlParserDebugEntities)
		xmlGenericError(0, "PEReference: %s\n", name);
	if(!name) {
		xmlFatalErr(ctxt, XML_ERR_PEREF_NO_NAME, 0);
	}
	else {
		if(RAW == ';') {
			NEXT;
			if(ctxt->sax && ctxt->sax->getParameterEntity)
				entity = ctxt->sax->getParameterEntity(ctxt->userData, name);
			if(ctxt->instate == XML_PARSER_EOF)
				return;
			if(entity == NULL) {
				/*
				 * [WFC: Entity Declared]
				 * In a document without any DTD, a document with only an
				 * internal DTD subset which contains no parameter entity
				 * references, or a document with "standalone='yes'", ...
				 * ... The declaration of a parameter entity must precede
				 * any reference to it...
				 */
				if((ctxt->standalone == 1) || ((ctxt->hasExternalSubset == 0) && (ctxt->hasPErefs == 0))) {
					xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY, "PEReference: %%%s; not found\n", name);
				}
				else {
					/*
					 * [VC: Entity Declared]
					 * In a document with an external subset or external
					 * parameter entities with "standalone='no'", ...
					 * ... The declaration of a parameter entity must precede
					 * any reference to it...
					 */
					if(ctxt->validate && ctxt->vctxt.error) {
						xmlValidityError(ctxt, XML_WAR_UNDECLARED_ENTITY, "PEReference: %%%s; not found\n", name, 0);
					}
					else
						xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY, "PEReference: %%%s; not found\n", name, 0);
					ctxt->valid = 0;
				}
				xmlParserEntityCheck(ctxt, 0, NULL, 0);
			}
			else if(ctxt->input->free != deallocblankswrapper) {
				input = xmlNewBlanksWrapperInputStream(ctxt, entity);
				if(xmlPushInput(ctxt, input) < 0)
					return;
			}
			else {
				if(oneof2(entity->etype, XML_INTERNAL_PARAMETER_ENTITY, XML_EXTERNAL_PARAMETER_ENTITY)) {
					xmlChar start[4];
					xmlCharEncoding enc;
					/*
					 * Note: external parameter entities will not be loaded, it
					 * is not required for a non-validating parser, unless the
					 * option of validating, or substituting entities were
					 * given. Doing so is far more secure as the parser will
					 * only process data coming from the document entity by
					 * default.
					 */
					if((entity->etype == XML_EXTERNAL_PARAMETER_ENTITY) && (!(ctxt->options & XML_PARSE_NOENT)) &&
					    (!(ctxt->options & XML_PARSE_DTDVALID)) && (!(ctxt->options & XML_PARSE_DTDLOAD)) &&
					    (!(ctxt->options & XML_PARSE_DTDATTR)) && !ctxt->replaceEntities && !ctxt->validate)
						return;
					/*
					 * handle the extra spaces added before and after
					 * c.f. http://www.w3.org/TR/REC-xml#as-PE
					 * this is done independently.
					 */
					input = xmlNewEntityInputStream(ctxt, entity);
					if(xmlPushInput(ctxt, input) < 0)
						return;
					/*
					 * Get the 4 first bytes and decode the charset
					 * if enc != XML_CHAR_ENCODING_NONE
					 * plug some encoding conversion routines.
					 * Note that, since we may have some non-UTF8
					 * encoding (like UTF16, bug 135229), the 'length'
					 * is not known, but we can calculate based upon
					 * the amount of data in the buffer.
					 */
					GROW
					if(ctxt->instate == XML_PARSER_EOF)
						return;
					if((ctxt->input->end - ctxt->input->cur)>=4) {
						start[0] = RAW;
						start[1] = NXT(1);
						start[2] = NXT(2);
						start[3] = NXT(3);
						enc = xmlDetectCharEncoding(start, 4);
						if(enc != XML_CHAR_ENCODING_NONE) {
							xmlSwitchEncoding(ctxt, enc);
						}
					}
					if((entity->etype == XML_EXTERNAL_PARAMETER_ENTITY) && (CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
						xmlParseTextDecl(ctxt);
					}
				}
				else {
					xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_IS_PARAMETER, "PEReference: %s is not a parameter entity\n", name);
				}
			}
		}
		else {
			xmlFatalErr(ctxt, XML_ERR_PEREF_SEMICOL_MISSING, 0);
		}
	}
}
/*
 * Macro used to grow the current buffer.
 * buffer##_size is expected to be a size_t
 * mem_error: is expected to handle memory allocation failures
 */
#define growBuffer(buffer, n) {						\
		xmlChar * tmp;							     \
		size_t new_size = buffer ## _size * 2 + n;			      \
		if(new_size < buffer ## _size) goto mem_error;			     \
		tmp = (xmlChar*)SAlloc::R(buffer, new_size);			  \
		if(!tmp) goto mem_error;					   \
		buffer = tmp;							    \
		buffer ## _size = new_size;					      \
}

/**
 * xmlStringLenDecodeEntities:
 * @ctxt:  the parser context
 * @str:  the input string
 * @len: the string length
 * @what:  combination of XML_SUBSTITUTE_REF and XML_SUBSTITUTE_PEREF
 * @end:  an end marker xmlChar, 0 if none
 * @end2:  an end marker xmlChar, 0 if none
 * @end3:  an end marker xmlChar, 0 if none
 *
 * Takes a entity string content and process to do the adequate substitutions.
 *
 * [67] Reference ::= EntityRef | CharRef
 *
 * [69] PEReference ::= '%' Name ';'
 *
 * Returns A newly allocated string with the substitution done. The caller
 *      must deallocate it !
 */
xmlChar * xmlStringLenDecodeEntities(xmlParserCtxt * ctxt, const xmlChar * str, int len,
    int what, xmlChar end, xmlChar end2, xmlChar end3)
{
	xmlChar * buffer = NULL;
	size_t buffer_size = 0;
	size_t nbchars = 0;
	xmlChar * current = NULL;
	xmlChar * rep = NULL;
	const xmlChar * last;
	xmlEntity * ent;
	int c, l;
	if(!ctxt || (str == NULL) || (len < 0))
		return 0;
	last = str + len;
	if(((ctxt->depth > 40) && ((ctxt->options & XML_PARSE_HUGE) == 0)) || (ctxt->depth > 1024)) {
		xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, 0);
		return 0;
	}
	/*
	 * allocate a translation buffer.
	 */
	buffer_size = XML_PARSER_BIG_BUFFER_SIZE;
	buffer = (xmlChar*)SAlloc::M(buffer_size);
	if(!buffer)
		goto mem_error;
	/*
	 * OK loop until we reach one of the ending char or a size limit.
	 * we are operating on already parsed values.
	 */
	if(str < last)
		c = CUR_SCHAR(str, l);
	else
		c = 0;
	while((c != 0) && (c != end) && /* non input consuming loop */ (c != end2) && (c != end3)) {
		if(c == 0)
			break;
		if((c == '&') && (str[1] == '#')) {
			int val = xmlParseStringCharRef(ctxt, &str);
			if(val) {
				COPY_BUF(0, buffer, nbchars, val);
			}
			if(nbchars + XML_PARSER_BUFFER_SIZE > buffer_size) {
				growBuffer(buffer, XML_PARSER_BUFFER_SIZE);
			}
		}
		else if((c == '&') && (what & XML_SUBSTITUTE_REF)) {
			if(xmlParserDebugEntities)
				xmlGenericError(0, "String decoding Entity Reference: %.30s\n", str);
			ent = xmlParseStringEntityRef(ctxt, &str);
			if((ctxt->lastError.code == XML_ERR_ENTITY_LOOP) || (ctxt->lastError.code == XML_ERR_INTERNAL_ERROR))
				goto int_error;
			xmlParserEntityCheck(ctxt, 0, ent, 0);
			if(ent)
				ctxt->nbentities += ent->checked / 2;
			if(ent && (ent->etype == XML_INTERNAL_PREDEFINED_ENTITY)) {
				if(ent->content) {
					COPY_BUF(0, buffer, nbchars, ent->content[0]);
					if(nbchars + XML_PARSER_BUFFER_SIZE > buffer_size) {
						growBuffer(buffer, XML_PARSER_BUFFER_SIZE);
					}
				}
				else {
					xmlFatalErrMsg(ctxt, XML_ERR_INTERNAL_ERROR, "predefined entity has no content\n");
				}
			}
			else if(ent && ent->content) {
				ctxt->depth++;
				rep = xmlStringDecodeEntities(ctxt, ent->content, what, 0, 0, 0);
				ctxt->depth--;
				if(rep) {
					current = rep;
					while(*current != 0) { /* non input consuming loop */
						buffer[nbchars++] = *current++;
						if(nbchars + XML_PARSER_BUFFER_SIZE > buffer_size) {
							if(xmlParserEntityCheck(ctxt, nbchars, ent, 0))
								goto int_error;
							growBuffer(buffer, XML_PARSER_BUFFER_SIZE);
						}
					}
					ZFREE(rep);
				}
			}
			else if(ent) {
				int i = sstrlen(ent->name);
				const xmlChar * cur = ent->name;
				buffer[nbchars++] = '&';
				if(nbchars + i + XML_PARSER_BUFFER_SIZE > buffer_size) {
					growBuffer(buffer, i + XML_PARSER_BUFFER_SIZE);
				}
				for(; i > 0; i--)
					buffer[nbchars++] = *cur++;
				buffer[nbchars++] = ';';
			}
		}
		else if(c == '%' && (what & XML_SUBSTITUTE_PEREF)) {
			if(xmlParserDebugEntities)
				xmlGenericError(0, "String decoding PE Reference: %.30s\n", str);
			ent = xmlParseStringPEReference(ctxt, &str);
			if(ctxt->lastError.code == XML_ERR_ENTITY_LOOP)
				goto int_error;
			xmlParserEntityCheck(ctxt, 0, ent, 0);
			if(ent)
				ctxt->nbentities += ent->checked / 2;
			if(ent) {
				if(ent->content == NULL) {
					xmlLoadEntityContent(ctxt, ent);
				}
				ctxt->depth++;
				rep = xmlStringDecodeEntities(ctxt, ent->content, what, 0, 0, 0);
				ctxt->depth--;
				if(rep) {
					current = rep;
					while(*current != 0) { /* non input consuming loop */
						buffer[nbchars++] = *current++;
						if(nbchars + XML_PARSER_BUFFER_SIZE > buffer_size) {
							if(xmlParserEntityCheck(ctxt, nbchars, ent, 0))
								goto int_error;
							growBuffer(buffer, XML_PARSER_BUFFER_SIZE);
						}
					}
					ZFREE(rep);
				}
			}
		}
		else {
			COPY_BUF(l, buffer, nbchars, c);
			str += l;
			if(nbchars + XML_PARSER_BUFFER_SIZE > buffer_size) {
				growBuffer(buffer, XML_PARSER_BUFFER_SIZE);
			}
		}
		if(str < last)
			c = CUR_SCHAR(str, l);
		else
			c = 0;
	}
	buffer[nbchars] = 0;
	return(buffer);
mem_error:
	xmlErrMemory(ctxt, 0);
int_error:
	SAlloc::F(rep);
	SAlloc::F(buffer);
	return 0;
}

/**
 * xmlStringDecodeEntities:
 * @ctxt:  the parser context
 * @str:  the input string
 * @what:  combination of XML_SUBSTITUTE_REF and XML_SUBSTITUTE_PEREF
 * @end:  an end marker xmlChar, 0 if none
 * @end2:  an end marker xmlChar, 0 if none
 * @end3:  an end marker xmlChar, 0 if none
 *
 * Takes a entity string content and process to do the adequate substitutions.
 *
 * [67] Reference ::= EntityRef | CharRef
 *
 * [69] PEReference ::= '%' Name ';'
 *
 * Returns A newly allocated string with the substitution done. The caller
 *      must deallocate it !
 */
xmlChar * xmlStringDecodeEntities(xmlParserCtxt * ctxt, const xmlChar * str, int what, xmlChar end, xmlChar end2, xmlChar end3)
{
	return (ctxt && str) ? xmlStringLenDecodeEntities(ctxt, str, sstrlen(str), what, end, end2, end3) : 0;
}

/************************************************************************
*									*
*		Commodity functions, cleanup needed ?			*
*									*
************************************************************************/

/**
 * areBlanks:
 * @ctxt:  an XML parser context
 * @str:  a xmlChar *
 * @len:  the size of @str
 * @blank_chars: we know the chars are blanks
 *
 * Is this a sequence of blank chars that one can ignore ?
 *
 * Returns 1 if ignorable 0 otherwise.
 */
static int areBlanks(xmlParserCtxt * ctxt, const xmlChar * str, int len, int blank_chars) 
{
	// Don't spend time trying to differentiate them, the same callback is used !
	if(ctxt->sax->ignorableWhitespace == ctxt->sax->characters)
		return 0;
	// Check for xml:space value.
	else if(!ctxt->space || (*(ctxt->space) == 1) || (*(ctxt->space) == -2))
		return 0;
	else {
		// Check that the string is made of blanks
		if(blank_chars == 0) {
			for(int i = 0; i < len; i++)
				if(!(IS_BLANK_CH(str[i]))) 
					return 0;
		}
		// Look if the element is mixed content in the DTD if available
		if(!ctxt->P_Node) 
			return 0;
		else {
			if(ctxt->myDoc) {
				int ret = xmlIsMixedElement(ctxt->myDoc, ctxt->P_Node->name);
				if(ret == 0) 
					return 1;
				else if(ret == 1) 
					return 0;
			}
			// 
			// Otherwise, heuristic :-\
			// 
			if((RAW != '<') && (RAW != 0xD)) 
				return 0;
			else if(!ctxt->P_Node->children && (RAW == '<') && (NXT(1) == '/')) 
				return 0;
			else {
				xmlNode * lastChild = xmlGetLastChild(ctxt->P_Node);
				if(!lastChild) {
					if(ctxt->P_Node->type != XML_ELEMENT_NODE && ctxt->P_Node->content) 
						return 0;
				}
				else if(xmlNodeIsText(lastChild))
					return 0;
				else if(ctxt->P_Node->children && (xmlNodeIsText(ctxt->P_Node->children)))
					return 0;
				return 1;
			}
		}
	}
}

/************************************************************************
*									*
*		Extra stuff for namespace support			*
*	Relates to http://www.w3.org/TR/WD-xml-names			*
*									*
************************************************************************/

/**
 * xmlSplitQName:
 * @ctxt:  an XML parser context
 * @name:  an XML parser context
 * @prefix:  a xmlChar **
 *
 * parse an UTF8 encoded XML qualified name string
 *
 * [NS 5] QName ::= (Prefix ':')? LocalPart
 *
 * [NS 6] Prefix ::= NCName
 *
 * [NS 7] LocalPart ::= NCName
 *
 * Returns the local part, and prefix is updated
 *   to get the Prefix if any.
 */
xmlChar * xmlSplitQName(xmlParserCtxt * ctxt, const xmlChar * name, xmlChar ** prefix) 
{
	xmlChar buf[XML_MAX_NAMELEN + 5];
	xmlChar * buffer = NULL;
	int len = 0;
	int max = XML_MAX_NAMELEN;
	xmlChar * ret = NULL;
	const xmlChar * cur = name;
	int c;
	if(prefix == NULL) 
		return 0;
	*prefix = NULL;
	if(!cur) 
		return 0;
#ifndef XML_XML_NAMESPACE
	/* xml: prefix is not really a namespace */
	if((cur[0] == 'x') && (cur[1] == 'm') && (cur[2] == 'l') && (cur[3] == ':'))
		return sstrdup(name);
#endif
	/* nasty but well=formed */
	if(cur[0] == ':')
		return sstrdup(name);
	c = *cur++;
	while((c != 0) && (c != ':') && (len < max)) { /* tested bigname.xml */
		buf[len++] = c;
		c = *cur++;
	}
	if(len >= max) {
		/*
		 * Okay someone managed to make a huge name, so he's ready to pay
		 * for the processing speed.
		 */
		max = len * 2;

		buffer = (xmlChar*)SAlloc::M(max * sizeof(xmlChar));
		if(!buffer) {
			xmlErrMemory(ctxt, 0);
			return 0;
		}
		memcpy(buffer, buf, len);
		while((c != 0) && (c != ':')) { /* tested bigname.xml */
			if(len + 10 > max) {
				xmlChar * tmp;

				max *= 2;
				tmp = (xmlChar*)SAlloc::R(buffer,
				    max * sizeof(xmlChar));
				if(!tmp) {
					SAlloc::F(buffer);
					xmlErrMemory(ctxt, 0);
					return 0;
				}
				buffer = tmp;
			}
			buffer[len++] = c;
			c = *cur++;
		}
		buffer[len] = 0;
	}

	if((c == ':') && (*cur == 0)) {
		SAlloc::F(buffer);
		*prefix = NULL;
		return sstrdup(name);
	}
	if(!buffer)
		ret = xmlStrndup(buf, len);
	else {
		ret = buffer;
		buffer = NULL;
		max = XML_MAX_NAMELEN;
	}
	if(c == ':') {
		c = *cur;
		*prefix = ret;
		if(c == 0) {
			return(xmlStrndup(BAD_CAST "", 0));
		}
		len = 0;

		/*
		 * Check that the first character is proper to start
		 * a new name
		 */
		if(!(((c >= 0x61) && (c <= 0x7A)) || ((c >= 0x41) && (c <= 0x5A)) || (c == '_') || (c == ':'))) {
			int l;
			int first = CUR_SCHAR(cur, l);
			if(!IS_LETTER(first) && (first != '_')) {
				xmlFatalErrMsgStr(ctxt, XML_NS_ERR_QNAME, "Name %s is not XML Namespace compliant\n", name);
			}
		}
		cur++;
		while((c != 0) && (len < max)) { /* tested bigname2.xml */
			buf[len++] = c;
			c = *cur++;
		}
		if(len >= max) {
			/*
			 * Okay someone managed to make a huge name, so he's ready to pay
			 * for the processing speed.
			 */
			max = len * 2;

			buffer = (xmlChar*)SAlloc::M(max * sizeof(xmlChar));
			if(!buffer) {
				xmlErrMemory(ctxt, 0);
				return 0;
			}
			memcpy(buffer, buf, len);
			while(c != 0) { /* tested bigname2.xml */
				if(len + 10 > max) {
					xmlChar * tmp;

					max *= 2;
					tmp = (xmlChar*)SAlloc::R(buffer,
					    max * sizeof(xmlChar));
					if(!tmp) {
						xmlErrMemory(ctxt, 0);
						SAlloc::F(buffer);
						return 0;
					}
					buffer = tmp;
				}
				buffer[len++] = c;
				c = *cur++;
			}
			buffer[len] = 0;
		}

		if(!buffer)
			ret = xmlStrndup(buf, len);
		else {
			ret = buffer;
		}
	}

	return ret;
}

/************************************************************************
*									*
*			The parser itself				*
*	Relates to http://www.w3.org/TR/REC-xml				*
*									*
************************************************************************/

/************************************************************************
*									*
*	Routines to parse Name, NCName and NmToken			*
*									*
************************************************************************/
#ifdef DEBUG
	static ulong nbParseName = 0;
	static ulong nbParseNmToken = 0;
	static ulong nbParseNCName = 0;
	static ulong nbParseNCNameComplex = 0;
	static ulong nbParseNameComplex = 0;
	static ulong nbParseStringName = 0;
#endif
/*
 * The two following functions are related to the change of accepted
 * characters for Name and NmToken in the Revision 5 of XML-1.0
 * They correspond to the modified production [4] and the new production [4a]
 * changes in that revision. Also note that the macros used for the
 * productions Letter, Digit, CombiningChar and Extender are not needed
 * anymore.
 * We still keep compatibility to pre-revision5 parsing semantic if the
 * new XML_PARSE_OLD10 option is given to the parser.
 */
static int FASTCALL xmlIsNameStartChar(xmlParserCtxt * ctxt, int c)
{
	if((ctxt->options & XML_PARSE_OLD10) == 0) {
		//
		// Use the new checks of production [4] [4a] amd [5] of the Update 5 of XML-1.0
		//
		if((c != ' ') && (c != '>') && (c != '/') && /* accelerators */ (IsLetterASCII(c) || oneof2(c, '_', ':') ||
			((c >= 0xC0) && (c <= 0xD6)) || ((c >= 0xD8) && (c <= 0xF6)) || ((c >= 0xF8) && (c <= 0x2FF)) ||
			((c >= 0x370) && (c <= 0x37D)) || ((c >= 0x37F) && (c <= 0x1FFF)) || ((c >= 0x200C) && (c <= 0x200D)) ||
			((c >= 0x2070) && (c <= 0x218F)) || ((c >= 0x2C00) && (c <= 0x2FEF)) || ((c >= 0x3001) && (c <= 0xD7FF)) ||
			((c >= 0xF900) && (c <= 0xFDCF)) || ((c >= 0xFDF0) && (c <= 0xFFFD)) || ((c >= 0x10000) && (c <= 0xEFFFF))))
			return 1;
	}
	else {
		if(IS_LETTER(c) || (c == '_') || (c == ':'))
			return 1;
	}
	return 0;
}

static int FASTCALL xmlIsNameChar(xmlParserCtxt * ctxt, int c)
{
	if((ctxt->options & XML_PARSE_OLD10) == 0) {
		/*
		 * Use the new checks of production [4] [4a] amd [5] of the
		 * Update 5 of XML-1.0
		 */
		if((c != ' ') && (c != '>') && (c != '/') && /* accelerators */ (IsLetterASCII(c) || isdec(c) || /* !start */
			    (c == '_') || (c == ':') ||
			    (c == '-') || (c == '.') || (c == 0xB7) || /* !start */
			    ((c >= 0xC0) && (c <= 0xD6)) ||
			    ((c >= 0xD8) && (c <= 0xF6)) ||
			    ((c >= 0xF8) && (c <= 0x2FF)) ||
			    ((c >= 0x300) && (c <= 0x36F)) || /* !start */
			    ((c >= 0x370) && (c <= 0x37D)) ||
			    ((c >= 0x37F) && (c <= 0x1FFF)) ||
			    ((c >= 0x200C) && (c <= 0x200D)) ||
			    ((c >= 0x203F) && (c <= 0x2040)) || /* !start */
			    ((c >= 0x2070) && (c <= 0x218F)) ||
			    ((c >= 0x2C00) && (c <= 0x2FEF)) ||
			    ((c >= 0x3001) && (c <= 0xD7FF)) ||
			    ((c >= 0xF900) && (c <= 0xFDCF)) ||
			    ((c >= 0xFDF0) && (c <= 0xFFFD)) ||
			    ((c >= 0x10000) && (c <= 0xEFFFF))))
			return 1;
	}
	else {
		if((IS_LETTER(c)) || (IS_DIGIT(c)) || (c == '.') || (c == '-') || (c == '_') || (c == ':') || (IS_COMBINING(c)) || (IS_EXTENDER(c)))
			return 1;
	}
	return 0;
}

static xmlChar * xmlParseAttValueInternal(xmlParserCtxt * ctxt, int * len, int * alloc, int normalize);

static const xmlChar * xmlParseNameComplex(xmlParserCtxt * ctxt)
{
	int len = 0, l;
	int c;
	int count = 0;
#ifdef DEBUG
	nbParseNameComplex++;
#endif
	/*
	 * Handler for more complex cases
	 */
	GROW;
	if(ctxt->instate == XML_PARSER_EOF)
		return 0;
	c = CUR_CHAR(l);
	if((ctxt->options & XML_PARSE_OLD10) == 0) {
		/*
		 * Use the new checks of production [4] [4a] amd [5] of the
		 * Update 5 of XML-1.0
		 */
		if((c == ' ') || (c == '>') || (c == '/') || /* accelerators */
		    (!(IsLetterASCII(c) || (c == '_') || (c == ':') ||
				    ((c >= 0xC0) && (c <= 0xD6)) ||
				    ((c >= 0xD8) && (c <= 0xF6)) ||
				    ((c >= 0xF8) && (c <= 0x2FF)) ||
				    ((c >= 0x370) && (c <= 0x37D)) ||
				    ((c >= 0x37F) && (c <= 0x1FFF)) ||
				    ((c >= 0x200C) && (c <= 0x200D)) ||
				    ((c >= 0x2070) && (c <= 0x218F)) ||
				    ((c >= 0x2C00) && (c <= 0x2FEF)) ||
				    ((c >= 0x3001) && (c <= 0xD7FF)) ||
				    ((c >= 0xF900) && (c <= 0xFDCF)) ||
				    ((c >= 0xFDF0) && (c <= 0xFFFD)) ||
				    ((c >= 0x10000) && (c <= 0xEFFFF))))) {
			return 0;
		}
		len += l;
		NEXTL(l);
		c = CUR_CHAR(l);
		while((c != ' ') && (c != '>') && (c != '/') && /* accelerators */
		    (IsLetterASCII(c) || isdec(c) || /* !start */
			    (c == '_') || (c == ':') || (c == '-') || (c == '.') || (c == 0xB7) || /* !start */
			    ((c >= 0xC0) && (c <= 0xD6)) ||
			    ((c >= 0xD8) && (c <= 0xF6)) ||
			    ((c >= 0xF8) && (c <= 0x2FF)) ||
			    ((c >= 0x300) && (c <= 0x36F)) || /* !start */
			    ((c >= 0x370) && (c <= 0x37D)) ||
			    ((c >= 0x37F) && (c <= 0x1FFF)) ||
			    ((c >= 0x200C) && (c <= 0x200D)) ||
			    ((c >= 0x203F) && (c <= 0x2040)) || /* !start */
			    ((c >= 0x2070) && (c <= 0x218F)) ||
			    ((c >= 0x2C00) && (c <= 0x2FEF)) ||
			    ((c >= 0x3001) && (c <= 0xD7FF)) ||
			    ((c >= 0xF900) && (c <= 0xFDCF)) ||
			    ((c >= 0xFDF0) && (c <= 0xFFFD)) ||
			    ((c >= 0x10000) && (c <= 0xEFFFF))
		    )) {
			if(count++ > XML_PARSER_CHUNK_SIZE) {
				count = 0;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
			}
			len += l;
			NEXTL(l);
			c = CUR_CHAR(l);
		}
	}
	else {
		if((c == ' ') || (c == '>') || (c == '/') || /* accelerators */ (!IS_LETTER(c) && (c != '_') && (c != ':'))) {
			return 0;
		}
		len += l;
		NEXTL(l);
		c = CUR_CHAR(l);

		while((c != ' ') && (c != '>') && (c != '/') && /* test bigname.xml */
		    ((IS_LETTER(c)) || (IS_DIGIT(c)) || oneof4(c, '.', '-', '_', ':') || (IS_COMBINING(c)) || (IS_EXTENDER(c)))) {
			if(count++ > XML_PARSER_CHUNK_SIZE) {
				count = 0;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
			}
			len += l;
			NEXTL(l);
			c = CUR_CHAR(l);
			if(c == 0) {
				count = 0;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
				c = CUR_CHAR(l);
			}
		}
	}
	if((len > XML_MAX_NAME_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
		xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "Name");
		return 0;
	}
	if((*ctxt->input->cur == '\n') && (ctxt->input->cur[-1] == '\r'))
		return xmlDictLookup(ctxt->dict, ctxt->input->cur - (len + 1), len);
	return xmlDictLookup(ctxt->dict, ctxt->input->cur - len, len);
}

/**
 * xmlParseName:
 * @ctxt:  an XML parser context
 *
 * parse an XML name.
 *
 * [4] NameChar ::= Letter | Digit | '.' | '-' | '_' | ':' |
 *                  CombiningChar | Extender
 *
 * [5] Name ::= (Letter | '_' | ':') (NameChar)*
 *
 * [6] Names ::= Name (#x20 Name)*
 *
 * Returns the Name parsed or NULL
 */
const xmlChar * FASTCALL xmlParseName(xmlParserCtxt * ctxt)
{
	const xmlChar * ret = 0;
	GROW;
#ifdef DEBUG
	nbParseName++;
#endif
	//
	// Accelerator for simple ASCII names
	//
	const xmlChar * in = ctxt->input->cur;
	if(((*in >= 0x61) && (*in <= 0x7A)) || ((*in >= 0x41) && (*in <= 0x5A)) || (*in == '_') || (*in == ':')) {
		in++;
		while(((*in >= 0x61) && (*in <= 0x7A)) || ((*in >= 0x41) && (*in <= 0x5A)) ||
		    ((*in >= 0x30) && (*in <= 0x39)) || (*in == '_') || (*in == '-') || (*in == ':') || (*in == '.'))
			in++;
		if((*in > 0) && (*in < 0x80)) {
			const int count = in - ctxt->input->cur;
			if((count > XML_MAX_NAME_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
				xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "Name");
			}
			else {
				ret = xmlDictLookup(ctxt->dict, ctxt->input->cur, count);
				ctxt->input->cur = in;
				ctxt->nbChars += count;
				ctxt->input->col += count;
				if(!ret)
					xmlErrMemory(ctxt, 0);
			}
			return ret;
		}
	}
	// accelerator for special cases 
	return xmlParseNameComplex(ctxt);
}

static const xmlChar * xmlParseNCNameComplex(xmlParserCtxt * ctxt)
{
	int len = 0, l;
	int c;
	int count = 0;
	const xmlChar * end; /* needed because CUR_CHAR() can move cur on \r\n */
#ifdef DEBUG
	nbParseNCNameComplex++;
#endif
	/*
	 * Handler for more complex cases
	 */
	GROW;
	end = ctxt->input->cur;
	c = CUR_CHAR(l);
	if(oneof3(c, ' ', '>', '/') || /* accelerators */ (!xmlIsNameStartChar(ctxt, c) || (c == ':'))) {
		return 0;
	}
	else {
		while(!oneof3(c, ' ', '>', '/') && /* test bigname.xml */ (xmlIsNameChar(ctxt, c) && (c != ':'))) {
			if(count++ > XML_PARSER_CHUNK_SIZE) {
				if((len > XML_MAX_NAME_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
					xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NCName");
					return 0;
				}
				else {
					count = 0;
					GROW;
					if(ctxt->instate == XML_PARSER_EOF)
						return 0;
				}
			}
			len += l;
			NEXTL(l);
			end = ctxt->input->cur;
			c = CUR_CHAR(l);
			if(c == 0) {
				count = 0;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
				else {
					end = ctxt->input->cur;
					c = CUR_CHAR(l);
				}
			}
		}
		if((len > XML_MAX_NAME_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
			xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NCName");
			return 0;
		}
		else
			return xmlDictLookup(ctxt->dict, end - len, len);
	}
}

/**
 * xmlParseNCName:
 * @ctxt:  an XML parser context
 * @len:  length of the string parsed
 *
 * parse an XML name.
 *
 * [4NS] NCNameChar ::= Letter | Digit | '.' | '-' | '_' |
 *                      CombiningChar | Extender
 *
 * [5NS] NCName ::= (Letter | '_') (NCNameChar)*
 *
 * Returns the Name parsed or NULL
 */

static const xmlChar * FASTCALL xmlParseNCName(xmlParserCtxt * ctxt)
{
	const xmlChar * ret = 0;
#ifdef DEBUG
	nbParseNCName++;
#endif
	/*
	 * Accelerator for simple ASCII names
	 */
	const xmlChar * in = ctxt->input->cur;
	if(((*in >= 0x61) && (*in <= 0x7A)) || ((*in >= 0x41) && (*in <= 0x5A)) || (*in == '_')) {
		in++;
		while(((*in >= 0x61) && (*in <= 0x7A)) || ((*in >= 0x41) && (*in <= 0x5A)) || ((*in >= 0x30) && (*in <= 0x39)) || oneof3(*in, '_', '-', '.'))
			in++;
		if((*in > 0) && (*in < 0x80)) {
			const int count = in - ctxt->input->cur;
			if((count > XML_MAX_NAME_LENGTH) && !(ctxt->options & XML_PARSE_HUGE)) {
				xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NCName");
			}
			else {
				ret = xmlDictLookup(ctxt->dict, ctxt->input->cur, count);
				ctxt->input->cur = in;
				ctxt->nbChars += count;
				ctxt->input->col += count;
				if(!ret)
					xmlErrMemory(ctxt, 0);
			}
			return ret;
		}
	}
	return xmlParseNCNameComplex(ctxt);
}
/**
 * xmlParseNameAndCompare:
 * @ctxt:  an XML parser context
 *
 * parse an XML name and compares for match
 * (specialized for endtag parsing)
 *
 * Returns NULL for an illegal name, (xmlChar*) 1 for success
 * and the name for mismatch
 */

static const xmlChar * xmlParseNameAndCompare(xmlParserCtxt * ctxt, xmlChar const * other)
{
	register const xmlChar * cmp = other;
	const xmlChar * ret = 0;
	GROW;
	if(ctxt->instate != XML_PARSER_EOF) {
		register const xmlChar * in = ctxt->input->cur;
		while(*in != 0 && *in == *cmp) {
			++in;
			++cmp;
			ctxt->input->col++;
		}
		if(*cmp == 0 && (*in == '>' || IS_BLANK_CH(*in))) {
			/* success */
			ctxt->input->cur = in;
			ret = (const xmlChar*)1;
		}
		else {
			/* failure (or end of input buffer), check with full function */
			ret = xmlParseName(ctxt);
			/* strings coming from the dictionnary direct compare possible */
			if(ret == other)
				ret = (const xmlChar*)1;
		}
	}
	return ret;
}

/**
 * xmlParseStringName:
 * @ctxt:  an XML parser context
 * @str:  a pointer to the string pointer (IN/OUT)
 *
 * parse an XML name.
 *
 * [4] NameChar ::= Letter | Digit | '.' | '-' | '_' | ':' | CombiningChar | Extender
 *
 * [5] Name ::= (Letter | '_' | ':') (NameChar)*
 *
 * [6] Names ::= Name (#x20 Name)*
 *
 * Returns the Name parsed or NULL. The @str pointer
 * is updated to the current location in the string.
 */
static xmlChar * FASTCALL xmlParseStringName(xmlParserCtxt * ctxt, const xmlChar** str)
{
	xmlChar buf[XML_MAX_NAMELEN + 5];
	const xmlChar * cur = *str;
	int len = 0, l;
	int c;
#ifdef DEBUG
	nbParseStringName++;
#endif
	c = CUR_SCHAR(cur, l);
	if(!xmlIsNameStartChar(ctxt, c)) {
		return 0;
	}
	COPY_BUF(l, buf, len, c);
	cur += l;
	c = CUR_SCHAR(cur, l);
	while(xmlIsNameChar(ctxt, c)) {
		COPY_BUF(l, buf, len, c);
		cur += l;
		c = CUR_SCHAR(cur, l);
		if(len >= XML_MAX_NAMELEN) { /* test bigentname.xml */
			/*
			 * Okay someone managed to make a huge name, so he's ready to pay
			 * for the processing speed.
			 */
			int max = len * 2;
			xmlChar * buffer = (xmlChar*)SAlloc::M(max * sizeof(xmlChar));
			if(!buffer) {
				xmlErrMemory(ctxt, 0);
				return 0;
			}
			memcpy(buffer, buf, len);
			while(xmlIsNameChar(ctxt, c)) {
				if(len + 10 > max) {
					xmlChar * tmp;
					if((len > XML_MAX_NAME_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
						xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NCName");
						SAlloc::F(buffer);
						return 0;
					}
					max *= 2;
					tmp = (xmlChar*)SAlloc::R(buffer, max * sizeof(xmlChar));
					if(!tmp) {
						xmlErrMemory(ctxt, 0);
						SAlloc::F(buffer);
						return 0;
					}
					buffer = tmp;
				}
				COPY_BUF(l, buffer, len, c);
				cur += l;
				c = CUR_SCHAR(cur, l);
			}
			buffer[len] = 0;
			*str = cur;
			return buffer;
		}
	}
	if((len > XML_MAX_NAME_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
		xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NCName");
		return 0;
	}
	else {
		*str = cur;
		return xmlStrndup(buf, len);
	}
}

/**
 * xmlParseNmtoken:
 * @ctxt:  an XML parser context
 *
 * parse an XML Nmtoken.
 *
 * [7] Nmtoken ::= (NameChar)+
 *
 * [8] Nmtokens ::= Nmtoken (#x20 Nmtoken)*
 *
 * Returns the Nmtoken parsed or NULL
 */

xmlChar * xmlParseNmtoken(xmlParserCtxt * ctxt)
{
	xmlChar buf[XML_MAX_NAMELEN + 5];
	int len = 0, l;
	int c;
	int count = 0;
#ifdef DEBUG
	nbParseNmToken++;
#endif
	GROW;
	if(ctxt->instate == XML_PARSER_EOF)
		return 0;
	c = CUR_CHAR(l);
	while(xmlIsNameChar(ctxt, c)) {
		if(count++ > XML_PARSER_CHUNK_SIZE) {
			count = 0;
			GROW;
		}
		COPY_BUF(l, buf, len, c);
		NEXTL(l);
		c = CUR_CHAR(l);
		if(c == 0) {
			count = 0;
			GROW;
			if(ctxt->instate == XML_PARSER_EOF)
				return 0;
			c = CUR_CHAR(l);
		}
		if(len >= XML_MAX_NAMELEN) {
			/*
			 * Okay someone managed to make a huge token, so he's ready to pay
			 * for the processing speed.
			 */
			int max = len * 2;
			xmlChar * buffer = (xmlChar*)SAlloc::M(max * sizeof(xmlChar));
			if(!buffer) {
				xmlErrMemory(ctxt, 0);
				return 0;
			}
			memcpy(buffer, buf, len);
			while(xmlIsNameChar(ctxt, c)) {
				if(count++ > XML_PARSER_CHUNK_SIZE) {
					count = 0;
					GROW;
					if(ctxt->instate == XML_PARSER_EOF) {
						SAlloc::F(buffer);
						return 0;
					}
				}
				if(len + 10 > max) {
					xmlChar * tmp;
					if((max > XML_MAX_NAME_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
						xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NmToken");
						SAlloc::F(buffer);
						return 0;
					}
					max *= 2;
					tmp = (xmlChar*)SAlloc::R(buffer, max * sizeof(xmlChar));
					if(!tmp) {
						xmlErrMemory(ctxt, 0);
						SAlloc::F(buffer);
						return 0;
					}
					buffer = tmp;
				}
				COPY_BUF(l, buffer, len, c);
				NEXTL(l);
				c = CUR_CHAR(l);
			}
			buffer[len] = 0;
			return buffer;
		}
	}
	if(len == 0)
		return 0;
	else if((len > XML_MAX_NAME_LENGTH) && !(ctxt->options & XML_PARSE_HUGE)) {
		xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "NmToken");
		return 0;
	}
	else
		return xmlStrndup(buf, len);
}
/**
 * xmlParseEntityValue:
 * @ctxt:  an XML parser context
 * @orig:  if non-NULL store a copy of the original entity value
 *
 * parse a value for ENTITY declarations
 *
 * [9] EntityValue ::= '"' ([^%&"] | PEReference | Reference)* '"' |
 *	               "'" ([^%&'] | PEReference | Reference)* "'"
 *
 * Returns the EntityValue parsed with reference substituted or NULL
 */
xmlChar * xmlParseEntityValue(xmlParserCtxt * ctxt, xmlChar ** orig)
{
	xmlChar * buf = NULL;
	int len = 0;
	int size = XML_PARSER_BUFFER_SIZE;
	int c, l;
	xmlChar stop;
	xmlChar * ret = NULL;
	const xmlChar * cur = NULL;
	xmlParserInputPtr input;
	if(RAW == '"')
		stop = '"';
	else if(RAW == '\'')
		stop = '\'';
	else {
		xmlFatalErr(ctxt, XML_ERR_ENTITY_NOT_STARTED, 0);
		return 0;
	}
	buf = (xmlChar*)SAlloc::M(size * sizeof(xmlChar));
	if(!buf) {
		xmlErrMemory(ctxt, 0);
		return 0;
	}
	/*
	 * The content of the entity definition is copied in a buffer.
	 */
	ctxt->instate = XML_PARSER_ENTITY_VALUE;
	input = ctxt->input;
	GROW;
	if(ctxt->instate == XML_PARSER_EOF) {
		SAlloc::F(buf);
		return 0;
	}
	NEXT;
	c = CUR_CHAR(l);
	/*
	 * NOTE: 4.4.5 Included in Literal
	 * When a parameter entity reference appears in a literal entity
	 * value, ... a single or double quote character in the replacement
	 * text is always treated as a normal data character and will not
	 * terminate the literal.
	 * In practice it means we stop the loop only when back at parsing
	 * the initial entity and the quote is found
	 */
	while(((IS_CHAR(c)) && ((c != stop) || /* checked */ (ctxt->input != input))) && (ctxt->instate != XML_PARSER_EOF)) {
		if(len + 5 >= size) {
			size *= 2;
			xmlChar * tmp = (xmlChar*)SAlloc::R(buf, size * sizeof(xmlChar));
			if(!tmp) {
				xmlErrMemory(ctxt, 0);
				SAlloc::F(buf);
				return 0;
			}
			buf = tmp;
		}
		COPY_BUF(l, buf, len, c);
		NEXTL(l);
		/*
		 * Pop-up of finished entities.
		 */
		while((RAW == 0) && (ctxt->inputNr > 1)) /* non input consuming */
			xmlPopInput(ctxt);
		GROW;
		c = CUR_CHAR(l);
		if(c == 0) {
			GROW;
			c = CUR_CHAR(l);
		}
	}
	buf[len] = 0;
	if(ctxt->instate == XML_PARSER_EOF) {
		SAlloc::F(buf);
		return 0;
	}
	/*
	 * Raise problem w.r.t. '&' and '%' being used in non-entities
	 * reference constructs. Note Charref will be handled in
	 * xmlStringDecodeEntities()
	 */
	cur = buf;
	while(*cur != 0) { /* non input consuming */
		if((*cur == '%') || ((*cur == '&') && (cur[1] != '#'))) {
			xmlChar tmp = *cur;
			cur++;
			xmlChar * name = xmlParseStringName(ctxt, &cur);
			if((name == NULL) || (*cur != ';')) {
				xmlFatalErrMsgInt(ctxt, XML_ERR_ENTITY_CHAR_ERROR, "EntityValue: '%c' forbidden except for entities references\n", tmp);
			}
			if((tmp == '%') && (ctxt->inSubset == 1) && (ctxt->inputNr == 1)) {
				xmlFatalErr(ctxt, XML_ERR_ENTITY_PE_INTERNAL, 0);
			}
			SAlloc::F(name);
			if(*cur == 0)
				break;
		}
		cur++;
	}
	/*
	 * Then PEReference entities are substituted.
	 */
	if(c != stop) {
		xmlFatalErr(ctxt, XML_ERR_ENTITY_NOT_FINISHED, 0);
		SAlloc::F(buf);
	}
	else {
		NEXT;
		/*
		 * NOTE: 4.4.7 Bypassed
		 * When a general entity reference appears in the EntityValue in
		 * an entity declaration, it is bypassed and left as is.
		 * so XML_SUBSTITUTE_REF is not set here.
		 */
		ret = xmlStringDecodeEntities(ctxt, buf, XML_SUBSTITUTE_PEREF, 0, 0, 0);
		if(orig)
			*orig = buf;
		else
			SAlloc::F(buf);
	}
	return ret;
}
/**
 * xmlParseAttValueComplex:
 * @ctxt:  an XML parser context
 * @len:   the resulting attribute len
 * @normalize:  wether to apply the inner normalization
 *
 * parse a value for an attribute, this is the fallback function
 * of xmlParseAttValue() when the attribute parsing requires handling
 * of non-ASCII characters, or normalization compaction.
 *
 * Returns the AttValue parsed or NULL. The value has to be freed by the caller.
 */
static xmlChar * xmlParseAttValueComplex(xmlParserCtxt * ctxt, int * attlen, int normalize)
{
	xmlChar limit = 0;
	xmlChar * buf = NULL;
	xmlChar * rep = NULL;
	size_t len = 0;
	size_t buf_size = 0;
	int c, l, in_space = 0;
	xmlChar * current = NULL;
	xmlEntity * ent;
	if(NXT(0) == '"') {
		ctxt->instate = XML_PARSER_ATTRIBUTE_VALUE;
		limit = '"';
		NEXT;
	}
	else if(NXT(0) == '\'') {
		limit = '\'';
		ctxt->instate = XML_PARSER_ATTRIBUTE_VALUE;
		NEXT;
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_ATTRIBUTE_NOT_STARTED, 0);
		return 0;
	}
	/*
	 * allocate a translation buffer.
	 */
	buf_size = XML_PARSER_BUFFER_SIZE;
	buf = (xmlChar*)SAlloc::M(buf_size);
	if(!buf)
		goto mem_error;
	/*
	 * OK loop until we reach one of the ending char or a size limit.
	 */
	c = CUR_CHAR(l);
	while(((NXT(0) != limit) && /* checked */ (IS_CHAR(c)) && (c != '<')) && (ctxt->instate != XML_PARSER_EOF)) {
		/*
		 * Impose a reasonable limit on attribute size, unless XML_PARSE_HUGE
		 * special option is given
		 */
		if((len > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
			xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED, "AttValue length too long\n");
			goto mem_error;
		}
		if(c == 0)
			break;
		if(c == '&') {
			in_space = 0;
			if(NXT(1) == '#') {
				int val = xmlParseCharRef(ctxt);
				if(val == '&') {
					if(ctxt->replaceEntities) {
						if(len + 10 > buf_size) {
							growBuffer(buf, 10);
						}
						buf[len++] = '&';
					}
					else {
						/*
						 * The reparsing will be done in xmlStringGetNodeList()
						 * called by the attribute() function in SAX.c
						 */
						if(len + 10 > buf_size) {
							growBuffer(buf, 10);
						}
						buf[len++] = '&';
						buf[len++] = '#';
						buf[len++] = '3';
						buf[len++] = '8';
						buf[len++] = ';';
					}
				}
				else if(val) {
					if(len + 10 > buf_size) {
						growBuffer(buf, 10);
					}
					len += xmlCopyChar(0, &buf[len], val);
				}
			}
			else {
				ent = xmlParseEntityRef(ctxt);
				ctxt->nbentities++;
				if(ent)
					ctxt->nbentities += ent->owner;
				if(ent && ent->etype == XML_INTERNAL_PREDEFINED_ENTITY) {
					if(len + 10 > buf_size) {
						growBuffer(buf, 10);
					}
					if((ctxt->replaceEntities == 0) && (ent->content[0] == '&')) {
						buf[len++] = '&';
						buf[len++] = '#';
						buf[len++] = '3';
						buf[len++] = '8';
						buf[len++] = ';';
					}
					else {
						buf[len++] = ent->content[0];
					}
				}
				else if(ent && ctxt->replaceEntities) {
					if(ent->etype != XML_INTERNAL_PREDEFINED_ENTITY) {
						rep = xmlStringDecodeEntities(ctxt, ent->content, XML_SUBSTITUTE_REF, 0, 0, 0);
						if(rep) {
							current = rep;
							while(*current) { /* non input consuming */
								if(oneof3(*current, 0xD, 0xA, 0x9)) {
									buf[len++] = 0x20;
									current++;
								}
								else
									buf[len++] = *current++;
								if(len + 10 > buf_size) {
									growBuffer(buf, 10);
								}
							}
							ZFREE(rep);
						}
					}
					else {
						if(len + 10 > buf_size) {
							growBuffer(buf, 10);
						}
						if(ent->content)
							buf[len++] = ent->content[0];
					}
				}
				else if(ent) {
					int i = sstrlen(ent->name);
					const xmlChar * cur = ent->name;
					/*
					 * This may look absurd but is needed to detect
					 * entities problems
					 */
					if((ent->etype != XML_INTERNAL_PREDEFINED_ENTITY) && ent->content && !ent->checked) {
						ulong oldnbent = ctxt->nbentities;
						rep = xmlStringDecodeEntities(ctxt, ent->content, XML_SUBSTITUTE_REF, 0, 0, 0);
						ent->checked = (ctxt->nbentities - oldnbent + 1) * 2;
						if(rep) {
							if(xmlStrchr(rep, '<'))
								ent->checked |= 1;
							ZFREE(rep);
						}
					}
					/*
					 * Just output the reference
					 */
					buf[len++] = '&';
					while(len + i + 10 > buf_size) {
						growBuffer(buf, i + 10);
					}
					for(; i > 0; i--)
						buf[len++] = *cur++;
					buf[len++] = ';';
				}
			}
		}
		else {
			if(oneof4(c, 0x20, 0xD, 0xA, 0x9)) {
				if(len || !normalize) {
					if((!normalize) || (!in_space)) {
						COPY_BUF(l, buf, len, 0x20);
						while(len + 10 > buf_size) {
							growBuffer(buf, 10);
						}
					}
					in_space = 1;
				}
			}
			else {
				in_space = 0;
				COPY_BUF(l, buf, len, c);
				if(len + 10 > buf_size) {
					growBuffer(buf, 10);
				}
			}
			NEXTL(l);
		}
		GROW;
		c = CUR_CHAR(l);
	}
	if(ctxt->instate == XML_PARSER_EOF)
		goto error;
	if(in_space && normalize) {
		while((len > 0) && (buf[len - 1] == 0x20))
			len--;
	}
	buf[len] = 0;
	if(RAW == '<') {
		xmlFatalErr(ctxt, XML_ERR_LT_IN_ATTRIBUTE, 0);
	}
	else if(RAW != limit) {
		if((c != 0) && (!IS_CHAR(c))) {
			xmlFatalErrMsg(ctxt, XML_ERR_INVALID_CHAR, "invalid character in attribute value\n");
		}
		else {
			xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED, "AttValue: ' expected\n");
		}
	}
	else
		NEXT;
	/*
	 * There we potentially risk an overflow, don't allow attribute value of
	 * length more than INT_MAX it is a very reasonnable assumption !
	 */
	if(len >= INT_MAX) {
		xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED, "AttValue length too long\n");
		goto mem_error;
	}
	ASSIGN_PTR(attlen, (int)len);
	return buf;
mem_error:
	xmlErrMemory(ctxt, 0);
error:
	SAlloc::F(buf);
	SAlloc::F(rep);
	return 0;
}

/**
 * xmlParseAttValue:
 * @ctxt:  an XML parser context
 *
 * parse a value for an attribute
 * Note: the parser won't do substitution of entities here, this
 * will be handled later in xmlStringGetNodeList
 *
 * [10] AttValue ::= '"' ([^<&"] | Reference)* '"' |
 *                   "'" ([^<&'] | Reference)* "'"
 *
 * 3.3.3 Attribute-Value Normalization:
 * Before the value of an attribute is passed to the application or
 * checked for validity, the XML processor must normalize it as follows:
 * - a character reference is processed by appending the referenced
 *   character to the attribute value
 * - an entity reference is processed by recursively processing the
 *   replacement text of the entity
 * - a whitespace character (#x20, #xD, #xA, #x9) is processed by
 *   appending #x20 to the normalized value, except that only a single
 *   #x20 is appended for a "#xD#xA" sequence that is part of an external
 *   parsed entity or the literal entity value of an internal parsed entity
 * - other characters are processed by appending them to the normalized value
 * If the declared value is not CDATA, then the XML processor must further
 * process the normalized attribute value by discarding any leading and
 * trailing space (#x20) characters, and by replacing sequences of space
 * (#x20) characters by a single space (#x20) character.
 * All attributes for which no declaration has been read should be treated
 * by a non-validating parser as if declared CDATA.
 *
 * Returns the AttValue parsed or NULL. The value has to be freed by the caller.
 */

xmlChar * xmlParseAttValue(xmlParserCtxt * ctxt)
{
	return (ctxt && ctxt->input) ? xmlParseAttValueInternal(ctxt, NULL, NULL, 0) : 0;
}

/**
 * xmlParseSystemLiteral:
 * @ctxt:  an XML parser context
 *
 * parse an XML Literal
 *
 * [11] SystemLiteral ::= ('"' [^"]* '"') | ("'" [^']* "'")
 *
 * Returns the SystemLiteral parsed or NULL
 */

xmlChar * xmlParseSystemLiteral(xmlParserCtxt * ctxt)
{
	xmlChar * buf = NULL;
	int len = 0;
	int size = XML_PARSER_BUFFER_SIZE;
	int cur, l;
	xmlChar stop;
	int state = ctxt->instate;
	int count = 0;
	SHRINK;
	if(RAW == '"') {
		NEXT;
		stop = '"';
	}
	else if(RAW == '\'') {
		NEXT;
		stop = '\'';
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_STARTED, 0);
		return 0;
	}

	buf = (xmlChar*)SAlloc::M(size * sizeof(xmlChar));
	if(!buf) {
		xmlErrMemory(ctxt, 0);
		return 0;
	}
	ctxt->instate = XML_PARSER_SYSTEM_LITERAL;
	cur = CUR_CHAR(l);
	while((IS_CHAR(cur)) && (cur != stop)) { /* checked */
		if(len + 5 >= size) {
			xmlChar * tmp;
			if((size > XML_MAX_NAME_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
				xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "SystemLiteral");
				SAlloc::F(buf);
				ctxt->instate = (xmlParserInputState)state;
				return 0;
			}
			size *= 2;
			tmp = (xmlChar*)SAlloc::R(buf, size * sizeof(xmlChar));
			if(!tmp) {
				SAlloc::F(buf);
				xmlErrMemory(ctxt, 0);
				ctxt->instate = (xmlParserInputState)state;
				return 0;
			}
			buf = tmp;
		}
		count++;
		if(count > 50) {
			GROW;
			count = 0;
			if(ctxt->instate == XML_PARSER_EOF) {
				SAlloc::F(buf);
				return 0;
			}
		}
		COPY_BUF(l, buf, len, cur);
		NEXTL(l);
		cur = CUR_CHAR(l);
		if(cur == 0) {
			GROW;
			SHRINK;
			cur = CUR_CHAR(l);
		}
	}
	buf[len] = 0;
	ctxt->instate = (xmlParserInputState)state;
	if(!IS_CHAR(cur)) {
		xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_FINISHED, 0);
	}
	else {
		NEXT;
	}
	return buf;
}
/**
 * xmlParsePubidLiteral:
 * @ctxt:  an XML parser context
 *
 * parse an XML public literal
 *
 * [12] PubidLiteral ::= '"' PubidChar* '"' | "'" (PubidChar - "'")* "'"
 *
 * Returns the PubidLiteral parsed or NULL.
 */
xmlChar * xmlParsePubidLiteral(xmlParserCtxt * ctxt) 
{
	xmlChar * buf = NULL;
	int len = 0;
	int size = XML_PARSER_BUFFER_SIZE;
	xmlChar cur;
	xmlChar stop;
	int count = 0;
	xmlParserInputState oldstate = ctxt->instate;

	SHRINK;
	if(RAW == '"') {
		NEXT;
		stop = '"';
	}
	else if(RAW == '\'') {
		NEXT;
		stop = '\'';
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_STARTED, 0);
		return 0;
	}
	buf = (xmlChar*)SAlloc::M(size * sizeof(xmlChar));
	if(!buf) {
		xmlErrMemory(ctxt, 0);
		return 0;
	}
	ctxt->instate = XML_PARSER_PUBLIC_LITERAL;
	cur = CUR;
	while((IS_PUBIDCHAR_CH(cur)) && (cur != stop)) { /* checked */
		if(len + 1 >= size) {
			xmlChar * tmp;
			if((size > XML_MAX_NAME_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
				xmlFatalErr(ctxt, XML_ERR_NAME_TOO_LONG, "Public ID");
				SAlloc::F(buf);
				return 0;
			}
			size *= 2;
			tmp = (xmlChar*)SAlloc::R(buf, size * sizeof(xmlChar));
			if(!tmp) {
				xmlErrMemory(ctxt, 0);
				SAlloc::F(buf);
				return 0;
			}
			buf = tmp;
		}
		buf[len++] = cur;
		count++;
		if(count > 50) {
			GROW;
			count = 0;
			if(ctxt->instate == XML_PARSER_EOF) {
				SAlloc::F(buf);
				return 0;
			}
		}
		NEXT;
		cur = CUR;
		if(cur == 0) {
			GROW;
			SHRINK;
			cur = CUR;
		}
	}
	buf[len] = 0;
	if(cur != stop) {
		xmlFatalErr(ctxt, XML_ERR_LITERAL_NOT_FINISHED, 0);
	}
	else {
		NEXT;
	}
	ctxt->instate = oldstate;
	return buf;
}

static void xmlParseCharDataComplex(xmlParserCtxt * ctxt, int cdata);

/*
 * used for the test in the inner loop of the char data testing
 */
static const uchar test_char_data[256] = {
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x09, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 0x9, CR/LF separated */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x00, 0x27, /* & */
	0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
	0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37,
	0x38, 0x39, 0x3A, 0x3B, 0x00, 0x3D, 0x3E, 0x3F, /* < */
	0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
	0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
	0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
	0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x00, 0x5E, 0x5F, /*] */
	0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
	0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
	0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77,
	0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* non-ascii */
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

/**
 * xmlParseCharData:
 * @ctxt:  an XML parser context
 * @cdata:  int indicating whether we are within a CDATA section
 *
 * parse a CharData section.
 * if we are within a CDATA section ']]>' marks an end of section.
 *
 * The right angle bracket (>) may be represented using the string "&gt;",
 * and must, for compatibility, be escaped using "&gt;" or a character
 * reference when it appears in the string "]]>" in content, when that
 * string is not marking the end of a CDATA section.
 *
 * [14] CharData ::= [^<&]* - ([^<&]* ']]>' [^<&]*)
 */
void FASTCALL xmlParseCharData(xmlParserCtxt * ctxt, int cdata)
{
	const xmlChar * in;
	int nbchar = 0;
	int line = ctxt->input->line;
	int col = ctxt->input->col;
	int ccol;
	SHRINK;
	GROW;
	// 
	// Accelerated common case where input don't need to be
	// modified before passing it to the handler.
	// 
	if(!cdata) {
		charactersSAXFunc _saxchr_func = ctxt->sax ? ctxt->sax->characters : 0;
		in = ctxt->input->cur;
		do {
			//get_more_space:
			for(int _skip_ws_loop = 1; _skip_ws_loop;) {
				while(*in == 0x20) {
					in++;
					ctxt->input->col++;
				}
				if(*in == 0xA) {
					do {
						ctxt->input->line++;
						ctxt->input->col = 1;
						in++;
					} while(*in == 0xA);
					//goto get_more_space;
				}
				else
					_skip_ws_loop = 0;
			}
			if(*in == '<') {
				nbchar = in - ctxt->input->cur;
				if(nbchar > 0) {
					const xmlChar * tmp = ctxt->input->cur;
					ctxt->input->cur = in;
					if(ctxt->sax) {
						if(ctxt->sax->ignorableWhitespace != _saxchr_func) {
							if(areBlanks(ctxt, tmp, nbchar, 1)) {
								if(ctxt->sax->ignorableWhitespace)
									ctxt->sax->ignorableWhitespace(ctxt->userData, tmp, nbchar);
							}
							else {
								if(_saxchr_func)
									_saxchr_func(ctxt->userData, tmp, nbchar);
								if(*ctxt->space == -1)
									*ctxt->space = -2;
							}
						}
						else if(_saxchr_func)
							_saxchr_func(ctxt->userData, tmp, nbchar);
					}
				}
				return;
			}
get_more:
			ccol = ctxt->input->col;
			while(test_char_data[*in]) {
				in++;
				ccol++;
			}
			ctxt->input->col = ccol;
			if(*in == 0xA) {
				do {
					ctxt->input->line++;
					ctxt->input->col = 1;
					in++;
				} while(*in == 0xA);
				goto get_more;
			}
			if(*in == ']') {
				if((in[1] == ']') && (in[2] == '>')) {
					xmlFatalErr(ctxt, XML_ERR_MISPLACED_CDATA_END, 0);
					ctxt->input->cur = in;
					return;
				}
				in++;
				ctxt->input->col++;
				goto get_more;
			}
			nbchar = in - ctxt->input->cur;
			if(nbchar > 0) {
				if(ctxt->sax && (ctxt->sax->ignorableWhitespace != _saxchr_func) && (IS_BLANK_CH(*ctxt->input->cur))) {
					const xmlChar * tmp = ctxt->input->cur;
					ctxt->input->cur = in;
					if(areBlanks(ctxt, tmp, nbchar, 0)) {
						if(ctxt->sax->ignorableWhitespace)
							ctxt->sax->ignorableWhitespace(ctxt->userData, tmp, nbchar);
					}
					else {
						if(_saxchr_func)
							_saxchr_func(ctxt->userData, tmp, nbchar);
						if(*ctxt->space == -1)
							*ctxt->space = -2;
					}
					line = ctxt->input->line;
					col = ctxt->input->col;
				}
				else if(ctxt->sax) {
					if(_saxchr_func)
						_saxchr_func(ctxt->userData, ctxt->input->cur, nbchar);
					line = ctxt->input->line;
					col = ctxt->input->col;
				}
				/* something really bad happened in the SAX callback */
				if(ctxt->instate != XML_PARSER_CONTENT)
					return;
			}
			ctxt->input->cur = in;
			if(*in == 0xD) {
				in++;
				if(*in == 0xA) {
					ctxt->input->cur = in;
					in++;
					ctxt->input->line++;
					ctxt->input->col = 1;
					continue; /* while */
				}
				in--;
			}
			if(*in == '<') {
				return;
			}
			if(*in == '&') {
				return;
			}
			SHRINK;
			GROW;
			if(ctxt->instate == XML_PARSER_EOF)
				return;
			in = ctxt->input->cur;
		} while(((*in >= 0x20) && (*in <= 0x7F)) || (*in == 0x09));
		nbchar = 0;
	}
	ctxt->input->line = line;
	ctxt->input->col = col;
	xmlParseCharDataComplex(ctxt, cdata);
}

/**
 * xmlParseCharDataComplex:
 * @ctxt:  an XML parser context
 * @cdata:  int indicating whether we are within a CDATA section
 *
 * parse a CharData section.this is the fallback function
 * of xmlParseCharData() when the parsing requires handling
 * of non-ASCII characters.
 */
static void xmlParseCharDataComplex(xmlParserCtxt * ctxt, int cdata)
{
	xmlChar buf[XML_PARSER_BIG_BUFFER_SIZE + 5];
	int nbchar = 0;
	int cur, l;
	int count = 0;
	charactersSAXFunc _saxchr_func = ctxt->sax ? ctxt->sax->characters : 0;
	SHRINK;
	GROW;
	cur = CUR_CHAR(l);
	while((cur != '<') && /* checked */ (cur != '&') && (IS_CHAR(cur))) { /* test also done in xmlCurrentChar() */
		if((cur == ']') && (NXT(1) == ']') && (NXT(2) == '>')) {
			if(cdata)
				break;
			else {
				xmlFatalErr(ctxt, XML_ERR_MISPLACED_CDATA_END, 0);
			}
		}
		COPY_BUF(l, buf, nbchar, cur);
		if(nbchar >= XML_PARSER_BIG_BUFFER_SIZE) {
			buf[nbchar] = 0;
			/*
			 * OK the segment is to be consumed as chars.
			 */
			if(ctxt->sax && (!ctxt->disableSAX)) {
				if(areBlanks(ctxt, buf, nbchar, 0)) {
					if(ctxt->sax->ignorableWhitespace)
						ctxt->sax->ignorableWhitespace(ctxt->userData, buf, nbchar);
				}
				else {
					if(_saxchr_func)
						_saxchr_func(ctxt->userData, buf, nbchar);
					if(_saxchr_func != ctxt->sax->ignorableWhitespace && *ctxt->space == -1)
						*ctxt->space = -2;
				}
			}
			nbchar = 0;
			// something really bad happened in the SAX callback 
			if(ctxt->instate != XML_PARSER_CONTENT)
				return;
		}
		count++;
		if(count > 50) {
			GROW;
			count = 0;
			if(ctxt->instate == XML_PARSER_EOF)
				return;
		}
		NEXTL(l);
		cur = CUR_CHAR(l);
	}
	if(nbchar != 0) {
		buf[nbchar] = 0;
		/*
		 * OK the segment is to be consumed as chars.
		 */
		if(ctxt->sax && (!ctxt->disableSAX)) {
			if(areBlanks(ctxt, buf, nbchar, 0)) {
				if(ctxt->sax->ignorableWhitespace)
					ctxt->sax->ignorableWhitespace(ctxt->userData, buf, nbchar);
			}
			else {
				if(_saxchr_func)
					_saxchr_func(ctxt->userData, buf, nbchar);
				if(_saxchr_func != ctxt->sax->ignorableWhitespace && *ctxt->space == -1)
					*ctxt->space = -2;
			}
		}
	}
	if(cur && !IS_CHAR(cur)) {
		// Generate the error and skip the offending character 
		xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR, "PCDATA invalid Char value %d\n", cur);
		NEXTL(l);
	}
}
/**
 * xmlParseExternalID:
 * @ctxt:  an XML parser context
 * @publicID:  a xmlChar** receiving PubidLiteral
 * @strict: indicate whether we should restrict parsing to only
 *          production [75], see NOTE below
 *
 * Parse an External ID or a Public ID
 *
 * NOTE: Productions [75] and [83] interact badly since [75] can generate
 *       'PUBLIC' S PubidLiteral S SystemLiteral
 *
 * [75] ExternalID ::= 'SYSTEM' S SystemLiteral
 *                   | 'PUBLIC' S PubidLiteral S SystemLiteral
 *
 * [83] PublicID ::= 'PUBLIC' S PubidLiteral
 *
 * Returns the function returns SystemLiteral and in the second
 *                case publicID receives PubidLiteral, is strict is off
 *                it is possible to return NULL and have publicID set.
 */

xmlChar * xmlParseExternalID(xmlParserCtxt * ctxt, xmlChar ** publicID, int strict)
{
	xmlChar * URI = NULL;
	SHRINK;
	*publicID = NULL;
	const xmlChar * ptr = CUR_PTR;
	if(CMP6(ptr, 'S', 'Y', 'S', 'T', 'E', 'M')) {
		SKIP(6);
		if(!IS_BLANK_CH(CUR)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after 'SYSTEM'\n");
		}
		SKIP_BLANKS;
		URI = xmlParseSystemLiteral(ctxt);
		if(URI == NULL) {
			xmlFatalErr(ctxt, XML_ERR_URI_REQUIRED, 0);
		}
	}
	else if(CMP6(ptr, 'P', 'U', 'B', 'L', 'I', 'C')) {
		SKIP(6);
		if(!IS_BLANK_CH(CUR)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after 'PUBLIC'\n");
		}
		SKIP_BLANKS;
		*publicID = xmlParsePubidLiteral(ctxt);
		if(*publicID == NULL)
			xmlFatalErr(ctxt, XML_ERR_PUBID_REQUIRED, 0);
		if(strict) {
			//
			// We don't handle [83] so "S SystemLiteral" is required.
			//
			if(!IS_BLANK_CH(CUR))
				xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after the Public Identifier\n");
		}
		else {
			//
			// We handle [83] so we return immediately, if "S SystemLiteral" is not detected. From a purely parsing point of view that's a nice mess.
			//
			GROW;
			ptr = CUR_PTR;
			if(!IS_BLANK_CH(*ptr))
				return 0;
			while(IS_BLANK_CH(*ptr))
				ptr++;  /* @todo dangerous, fix ! */
			if((*ptr != '\'') && (*ptr != '"'))
				return 0;
		}
		SKIP_BLANKS;
		URI = xmlParseSystemLiteral(ctxt);
		if(URI == NULL)
			xmlFatalErr(ctxt, XML_ERR_URI_REQUIRED, 0);
	}
	return(URI);
}
/**
 * xmlParseCommentComplex:
 * @ctxt:  an XML parser context
 * @buf:  the already parsed part of the buffer
 * @len:  number of bytes filles in the buffer
 * @size:  allocated size of the buffer
 *
 * Skip an XML (SGML) comment <!-- .... -->
 *  The spec says that "For compatibility, the string "--" (double-hyphen)
 *  must not occur within comments. "
 * This is the slow routine in case the accelerator for ascii didn't work
 *
 * [15] Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'
 */
static void xmlParseCommentComplex(xmlParserCtxt * ctxt, xmlChar * buf, size_t len, size_t size)
{
	int q, ql;
	int r, rl;
	int cur, l;
	size_t count = 0;
	int inputid = ctxt->input->id;
	if(!buf) {
		len = 0;
		size = XML_PARSER_BUFFER_SIZE;
		buf = (xmlChar*)SAlloc::M(size * sizeof(xmlChar));
		if(!buf) {
			xmlErrMemory(ctxt, 0);
			return;
		}
	}
	GROW;   /* Assure there's enough input data */
	q = CUR_CHAR(ql);
	if(q == 0)
		goto not_terminated;
	if(!IS_CHAR(q)) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR, "xmlParseComment: invalid xmlChar value %d\n", q);
		SAlloc::F(buf);
		return;
	}
	NEXTL(ql);
	r = CUR_CHAR(rl);
	if(r == 0)
		goto not_terminated;
	if(!IS_CHAR(r)) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR, "xmlParseComment: invalid xmlChar value %d\n", q);
		SAlloc::F(buf);
		return;
	}
	NEXTL(rl);
	cur = CUR_CHAR(l);
	if(cur == 0)
		goto not_terminated;
	while(IS_CHAR(cur) && /* checked */ ((cur != '>') || (r != '-') || (q != '-'))) {
		if((r == '-') && (q == '-')) {
			xmlFatalErr(ctxt, XML_ERR_HYPHEN_IN_COMMENT, 0);
		}
		if((len > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
			xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED, "Comment too big found", 0);
			SAlloc::F(buf);
			return;
		}
		if(len + 5 >= size) {
			size_t new_size = size * 2;
			xmlChar * new_buf = (xmlChar*)SAlloc::R(buf, new_size);
			if(new_buf == NULL) {
				SAlloc::F(buf);
				xmlErrMemory(ctxt, 0);
				return;
			}
			buf = new_buf;
			size = new_size;
		}
		COPY_BUF(ql, buf, len, q);
		q = r;
		ql = rl;
		r = cur;
		rl = l;
		count++;
		if(count > 50) {
			GROW;
			count = 0;
			if(ctxt->instate == XML_PARSER_EOF) {
				SAlloc::F(buf);
				return;
			}
		}
		NEXTL(l);
		cur = CUR_CHAR(l);
		if(cur == 0) {
			SHRINK;
			GROW;
			cur = CUR_CHAR(l);
		}
	}
	buf[len] = 0;
	if(cur == 0) {
		xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED, "Comment not terminated \n<!--%.50s\n", buf);
	}
	else if(!IS_CHAR(cur)) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR, "xmlParseComment: invalid xmlChar value %d\n", cur);
	}
	else {
		if(inputid != ctxt->input->id) {
			xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY, "Comment doesn't start and stop in the same entity\n");
		}
		NEXT;
		if(ctxt->sax && ctxt->sax->comment && (!ctxt->disableSAX))
			ctxt->sax->comment(ctxt->userData, buf);
	}
	SAlloc::F(buf);
	return;
not_terminated:
	xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED, "Comment not terminated\n", 0);
	SAlloc::F(buf);
	return;
}

/**
 * xmlParseComment:
 * @ctxt:  an XML parser context
 *
 * Skip an XML (SGML) comment <!-- .... -->
 *  The spec says that "For compatibility, the string "--" (double-hyphen)
 *  must not occur within comments. "
 *
 * [15] Comment ::= '<!--' ((Char - '-') | ('-' (Char - '-')))* '-->'
 */
void xmlParseComment(xmlParserCtxt * ctxt)
{
	xmlChar * buf = NULL;
	size_t size = XML_PARSER_BUFFER_SIZE;
	size_t len = 0;
	xmlParserInputState state;
	const xmlChar * in;
	size_t nbchar = 0;
	int ccol;
	int inputid;
	/*
	 * Check that there is a comment right here.
	 */
	if((RAW != '<') || (NXT(1) != '!') || (NXT(2) != '-') || (NXT(3) != '-')) return;
	state = ctxt->instate;
	ctxt->instate = XML_PARSER_COMMENT;
	inputid = ctxt->input->id;
	SKIP(4);
	SHRINK;
	GROW;
	/*
	 * Accelerated common case where input don't need to be
	 * modified before passing it to the handler.
	 */
	in = ctxt->input->cur;
	do {
		if(*in == 0xA) {
			do {
				ctxt->input->line++; ctxt->input->col = 1;
				in++;
			} while(*in == 0xA);
		}
get_more:
		ccol = ctxt->input->col;
		while(((*in > '-') && (*in <= 0x7F)) ||
		    ((*in >= 0x20) && (*in < '-')) ||
		    (*in == 0x09)) {
			in++;
			ccol++;
		}
		ctxt->input->col = ccol;
		if(*in == 0xA) {
			do {
				ctxt->input->line++; ctxt->input->col = 1;
				in++;
			} while(*in == 0xA);
			goto get_more;
		}
		nbchar = in - ctxt->input->cur;
		/*
		 * save current set of data
		 */
		if(nbchar > 0) {
			if(ctxt->sax && ctxt->sax->comment) {
				if(!buf) {
					if((*in == '-') && (in[1] == '-'))
						size = nbchar + 1;
					else
						size = XML_PARSER_BUFFER_SIZE + nbchar;
					buf = (xmlChar*)SAlloc::M(size * sizeof(xmlChar));
					if(!buf) {
						xmlErrMemory(ctxt, 0);
						ctxt->instate = state;
						return;
					}
					len = 0;
				}
				else if(len + nbchar + 1 >= size) {
					xmlChar * new_buf;
					size  += len + nbchar + XML_PARSER_BUFFER_SIZE;
					new_buf = (xmlChar*)SAlloc::R(buf,
					    size * sizeof(xmlChar));
					if(new_buf == NULL) {
						SAlloc::F(buf);
						xmlErrMemory(ctxt, 0);
						ctxt->instate = state;
						return;
					}
					buf = new_buf;
				}
				memcpy(&buf[len], ctxt->input->cur, nbchar);
				len += nbchar;
				buf[len] = 0;
			}
		}
		if((len > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
			xmlFatalErrMsgStr(ctxt, XML_ERR_COMMENT_NOT_FINISHED, "Comment too big found", 0);
			SAlloc::F(buf);
			return;
		}
		ctxt->input->cur = in;
		if(*in == 0xA) {
			in++;
			ctxt->input->line++; ctxt->input->col = 1;
		}
		if(*in == 0xD) {
			in++;
			if(*in == 0xA) {
				ctxt->input->cur = in;
				in++;
				ctxt->input->line++; ctxt->input->col = 1;
				continue; /* while */
			}
			in--;
		}
		SHRINK;
		GROW;
		if(ctxt->instate == XML_PARSER_EOF) {
			SAlloc::F(buf);
			return;
		}
		in = ctxt->input->cur;
		if(*in == '-') {
			if(in[1] == '-') {
				if(in[2] == '>') {
					if(ctxt->input->id != inputid) {
						xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY, "comment doesn't start and stop in the same entity\n");
					}
					SKIP(3);
					if(ctxt->sax && ctxt->sax->comment && (!ctxt->disableSAX)) {
						ctxt->sax->comment(ctxt->userData, buf ? buf : BAD_CAST "");
					}
					SAlloc::F(buf);
					if(ctxt->instate != XML_PARSER_EOF)
						ctxt->instate = state;
					return;
				}
				if(buf)
					xmlFatalErrMsgStr(ctxt, XML_ERR_HYPHEN_IN_COMMENT, "Double hyphen within comment: <!--%.50s\n", buf);
				else
					xmlFatalErrMsgStr(ctxt, XML_ERR_HYPHEN_IN_COMMENT, "Double hyphen within comment\n", 0);
				in++;
				ctxt->input->col++;
			}
			in++;
			ctxt->input->col++;
			goto get_more;
		}
	} while(((*in >= 0x20) && (*in <= 0x7F)) || (*in == 0x09));
	xmlParseCommentComplex(ctxt, buf, len, size);
	ctxt->instate = state;
}

/**
 * xmlParsePITarget:
 * @ctxt:  an XML parser context
 *
 * parse the name of a PI
 *
 * [17] PITarget ::= Name - (('X' | 'x') ('M' | 'm') ('L' | 'l'))
 *
 * Returns the PITarget name or NULL
 */

const xmlChar * xmlParsePITarget(xmlParserCtxt * ctxt)
{
	const xmlChar * name = xmlParseName(ctxt);
	if(name && ((name[0] == 'x') || (name[0] == 'X')) && ((name[1] == 'm') || (name[1] == 'M')) && ((name[2] == 'l') || (name[2] == 'L'))) {
		int i;
		if((name[0] == 'x') && (name[1] == 'm') && (name[2] == 'l') && (name[3] == 0)) {
			xmlFatalErrMsg(ctxt, XML_ERR_RESERVED_XML_NAME, "XML declaration allowed only at the start of the document\n");
			return name;
		}
		else if(name[3] == 0) {
			xmlFatalErr(ctxt, XML_ERR_RESERVED_XML_NAME, 0);
			return name;
		}
		for(i = 0;; i++) {
			if(xmlW3CPIs[i] == NULL)
				break;
			if(sstreq(name, (const xmlChar*)xmlW3CPIs[i]))
				return name;
		}
		xmlWarningMsg(ctxt, XML_ERR_RESERVED_XML_NAME, "xmlParsePITarget: invalid name prefix 'xml'\n", 0, 0);
	}
	if(name && xmlStrchr(name, ':')) {
		xmlNsErr(ctxt, XML_NS_ERR_COLON, "colons are forbidden from PI names '%s'\n", name, 0, 0);
	}
	return name;
}

#ifdef LIBXML_CATALOG_ENABLED
/**
 * xmlParseCatalogPI:
 * @ctxt:  an XML parser context
 * @catalog:  the PI value string
 *
 * parse an XML Catalog Processing Instruction.
 *
 * <?oasis-xml-catalog catalog="http://example.com/catalog.xml"?>
 *
 * Occurs only if allowed by the user and if happening in the Misc
 * part of the document before any doctype informations
 * This will add the given catalog to the parsing context in order
 * to be used if there is a resolution need further down in the document
 */

static void xmlParseCatalogPI(xmlParserCtxt * ctxt, const xmlChar * catalog)
{
	xmlChar * URL = NULL;
	const xmlChar * base;
	xmlChar marker;
	const xmlChar * tmp = catalog;
	while(IS_BLANK_CH(*tmp))
		tmp++;
	if(xmlStrncmp(tmp, BAD_CAST "catalog", 7))
		goto error;
	tmp += 7;
	while(IS_BLANK_CH(*tmp)) tmp++;
	if(*tmp != '=') {
		return;
	}
	tmp++;
	while(IS_BLANK_CH(*tmp))
		tmp++;
	marker = *tmp;
	if((marker != '\'') && (marker != '"'))
		goto error;
	tmp++;
	base = tmp;
	while((*tmp != 0) && (*tmp != marker)) tmp++;
	if(*tmp == 0)
		goto error;
	URL = xmlStrndup(base, tmp - base);
	tmp++;
	while(IS_BLANK_CH(*tmp)) tmp++;
	if(*tmp != 0)
		goto error;
	if(URL) {
		ctxt->catalogs = xmlCatalogAddLocal(ctxt->catalogs, URL);
		SAlloc::F(URL);
	}
	return;
error:
	xmlWarningMsg(ctxt, XML_WAR_CATALOG_PI, "Catalog PI syntax error: %s\n", catalog, 0);
	SAlloc::F(URL);
}

#endif

/**
 * xmlParsePI:
 * @ctxt:  an XML parser context
 *
 * parse an XML Processing Instruction.
 *
 * [16] PI ::= '<?' PITarget (S (Char* - (Char* '?>' Char*)))? '?>'
 *
 * The processing is transfered to SAX once parsed.
 */

void xmlParsePI(xmlParserCtxt * ctxt)
{
	xmlChar * buf = NULL;
	size_t len = 0;
	size_t size = XML_PARSER_BUFFER_SIZE;
	int cur, l;
	const xmlChar * target;
	xmlParserInputState state;
	int count = 0;
	if((RAW == '<') && (NXT(1) == '?')) {
		xmlParserInputPtr input = ctxt->input;
		state = ctxt->instate;
		ctxt->instate = XML_PARSER_PI;
		/*
		 * this is a Processing Instruction.
		 */
		SKIP(2);
		SHRINK;

		/*
		 * Parse the target name and check for special support like
		 * namespace.
		 */
		target = xmlParsePITarget(ctxt);
		if(target) {
			if((RAW == '?') && (NXT(1) == '>')) {
				if(input != ctxt->input) {
					xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY, "PI declaration doesn't start and stop in the same entity\n");
				}
				SKIP(2);
				/*
				 * SAX: PI detected.
				 */
				if(ctxt->sax && (!ctxt->disableSAX) && ctxt->sax->processingInstruction)
					ctxt->sax->processingInstruction(ctxt->userData, target, 0);
				if(ctxt->instate != XML_PARSER_EOF)
					ctxt->instate = state;
				return;
			}
			buf = (xmlChar*)SAlloc::M(size * sizeof(xmlChar));
			if(!buf) {
				xmlErrMemory(ctxt, 0);
				ctxt->instate = state;
				return;
			}
			cur = CUR;
			if(!IS_BLANK(cur)) {
				xmlFatalErrMsgStr(ctxt, XML_ERR_SPACE_REQUIRED, "ParsePI: PI %s space expected\n", target);
			}
			SKIP_BLANKS;
			cur = CUR_CHAR(l);
			while(IS_CHAR(cur) && /* checked */
			    ((cur != '?') || (NXT(1) != '>'))) {
				if(len + 5 >= size) {
					xmlChar * tmp;
					size_t new_size = size * 2;
					tmp = (xmlChar*)SAlloc::R(buf, new_size);
					if(!tmp) {
						xmlErrMemory(ctxt, 0);
						SAlloc::F(buf);
						ctxt->instate = state;
						return;
					}
					buf = tmp;
					size = new_size;
				}
				count++;
				if(count > 50) {
					GROW;
					if(ctxt->instate == XML_PARSER_EOF) {
						SAlloc::F(buf);
						return;
					}
					count = 0;
					if((len > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
						xmlFatalErrMsgStr(ctxt, XML_ERR_PI_NOT_FINISHED, "PI %s too big found", target);
						SAlloc::F(buf);
						ctxt->instate = state;
						return;
					}
				}
				COPY_BUF(l, buf, len, cur);
				NEXTL(l);
				cur = CUR_CHAR(l);
				if(cur == 0) {
					SHRINK;
					GROW;
					cur = CUR_CHAR(l);
				}
			}
			if((len > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
				xmlFatalErrMsgStr(ctxt, XML_ERR_PI_NOT_FINISHED, "PI %s too big found", target);
				SAlloc::F(buf);
				ctxt->instate = state;
				return;
			}
			buf[len] = 0;
			if(cur != '?') {
				xmlFatalErrMsgStr(ctxt, XML_ERR_PI_NOT_FINISHED, "ParsePI: PI %s never end ...\n", target);
			}
			else {
				if(input != ctxt->input) {
					xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "PI declaration doesn't start and stop in the same entity\n");
				}
				SKIP(2);

#ifdef LIBXML_CATALOG_ENABLED
				if(oneof2(state, XML_PARSER_MISC, XML_PARSER_START) && (sstreq(target, XML_CATALOG_PI))) {
					xmlCatalogAllow allow = xmlCatalogGetDefaults();
					if((allow == XML_CATA_ALLOW_DOCUMENT) || (allow == XML_CATA_ALLOW_ALL))
						xmlParseCatalogPI(ctxt, buf);
				}
#endif
				/*
				 * SAX: PI detected.
				 */
				if(ctxt->sax && (!ctxt->disableSAX) && ctxt->sax->processingInstruction)
					ctxt->sax->processingInstruction(ctxt->userData, target, buf);
			}
			SAlloc::F(buf);
		}
		else {
			xmlFatalErr(ctxt, XML_ERR_PI_NOT_STARTED, 0);
		}
		if(ctxt->instate != XML_PARSER_EOF)
			ctxt->instate = state;
	}
}

/**
 * xmlParseNotationDecl:
 * @ctxt:  an XML parser context
 *
 * parse a notation declaration
 *
 * [82] NotationDecl ::= '<!NOTATION' S Name S (ExternalID |  PublicID) S? '>'
 *
 * Hence there is actually 3 choices:
 *     'PUBLIC' S PubidLiteral
 *     'PUBLIC' S PubidLiteral S SystemLiteral
 * and 'SYSTEM' S SystemLiteral
 *
 * See the NOTE on xmlParseExternalID().
 */
void xmlParseNotationDecl(xmlParserCtxt * ctxt)
{
	const xmlChar * name;
	xmlChar * Pubid;
	xmlChar * Systemid;
	const xmlChar * ptr = CUR_PTR;
	if(CMP10(ptr, '<', '!', 'N', 'O', 'T', 'A', 'T', 'I', 'O', 'N')) {
		xmlParserInputPtr input = ctxt->input;
		SHRINK;
		SKIP(10);
		if(!IS_BLANK_CH(CUR)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after '<!NOTATION'\n");
		}
		else {
			SKIP_BLANKS;
			name = xmlParseName(ctxt);
			if(!name) {
				xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_STARTED, 0);
			}
			else if(!IS_BLANK_CH(CUR)) {
				xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after the NOTATION name'\n");
			}
			else {
				if(xmlStrchr(name, ':')) {
					xmlNsErr(ctxt, XML_NS_ERR_COLON, "colons are forbidden from notation names '%s'\n", name, 0, 0);
				}
				SKIP_BLANKS;
				//
				// Parse the IDs.
				//
				Systemid = xmlParseExternalID(ctxt, &Pubid, 0);
				SKIP_BLANKS;
				if(RAW == '>') {
					if(input != ctxt->input) {
						xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Notation declaration doesn't start and stop in the same entity\n");
					}
					NEXT;
					if(ctxt->sax && !ctxt->disableSAX && ctxt->sax->notationDecl)
						ctxt->sax->notationDecl(ctxt->userData, name, Pubid, Systemid);
				}
				else {
					xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_FINISHED, 0);
				}
				SAlloc::F(Systemid);
				SAlloc::F(Pubid);
			}
		}
	}
}

/**
 * xmlParseEntityDecl:
 * @ctxt:  an XML parser context
 *
 * parse <!ENTITY declarations
 *
 * [70] EntityDecl ::= GEDecl | PEDecl
 *
 * [71] GEDecl ::= '<!ENTITY' S Name S EntityDef S? '>'
 *
 * [72] PEDecl ::= '<!ENTITY' S '%' S Name S PEDef S? '>'
 *
 * [73] EntityDef ::= EntityValue | (ExternalID NDataDecl?)
 *
 * [74] PEDef ::= EntityValue | ExternalID
 *
 * [76] NDataDecl ::= S 'NDATA' S Name
 *
 * [VC: Notation Declared]
 * The Name must match the declared name of a notation.
 */

void xmlParseEntityDecl(xmlParserCtxt * ctxt)
{
	const xmlChar * name = NULL;
	xmlChar * value = NULL;
	xmlChar * URI = NULL, * literal = NULL;
	const xmlChar * ndata = NULL;
	int isParameter = 0;
	xmlChar * orig = NULL;
	int skipped;
	// GROW; done in the caller 
	if(CMP8(CUR_PTR, '<', '!', 'E', 'N', 'T', 'I', 'T', 'Y')) {
		xmlParserInputPtr input = ctxt->input;
		SHRINK;
		SKIP(8);
		skipped = SKIP_BLANKS;
		if(skipped == 0) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after '<!ENTITY'\n");
		}
		if(RAW == '%') {
			NEXT;
			skipped = SKIP_BLANKS;
			if(skipped == 0) {
				xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after '%'\n");
			}
			isParameter = 1;
		}
		name = xmlParseName(ctxt);
		if(!name) {
			xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "xmlParseEntityDecl: no name\n");
			return;
		}
		if(xmlStrchr(name, ':')) {
			xmlNsErr(ctxt, XML_NS_ERR_COLON, "colons are forbidden from entities names '%s'\n", name, 0, 0);
		}
		skipped = SKIP_BLANKS;
		if(skipped == 0) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after the entity name\n");
		}
		ctxt->instate = XML_PARSER_ENTITY_DECL;
		/*
		 * handle the various case of definitions...
		 */
		if(isParameter) {
			if((RAW == '"') || (RAW == '\'')) {
				value = xmlParseEntityValue(ctxt, &orig);
				if(value) {
					if(ctxt->sax && (!ctxt->disableSAX) && ctxt->sax->entityDecl)
						ctxt->sax->entityDecl(ctxt->userData, name, XML_INTERNAL_PARAMETER_ENTITY, NULL, NULL, value);
				}
			}
			else {
				URI = xmlParseExternalID(ctxt, &literal, 1);
				if((URI == NULL) && (literal == NULL)) {
					xmlFatalErr(ctxt, XML_ERR_VALUE_REQUIRED, 0);
				}
				if(URI) {
					xmlURIPtr uri = xmlParseURI((const char*)URI);
					if(uri == NULL) {
						xmlErrMsgStr(ctxt, XML_ERR_INVALID_URI, "Invalid URI: %s\n", URI);
						/*
						 * This really ought to be a well formedness error
						 * but the XML Core WG decided otherwise c.f. issue
						 * E26 of the XML erratas.
						 */
					}
					else {
						if(uri->fragment) { // Okay this is foolish to block those but not invalid URIs.
							xmlFatalErr(ctxt, XML_ERR_URI_FRAGMENT, 0);
						}
						else {
							if(ctxt->sax && (!ctxt->disableSAX) && ctxt->sax->entityDecl)
								ctxt->sax->entityDecl(ctxt->userData, name, XML_EXTERNAL_PARAMETER_ENTITY, literal, URI, 0);
						}
						xmlFreeURI(uri);
					}
				}
			}
		}
		else {
			if((RAW == '"') || (RAW == '\'')) {
				value = xmlParseEntityValue(ctxt, &orig);
				if(ctxt->sax && (!ctxt->disableSAX) && ctxt->sax->entityDecl)
					ctxt->sax->entityDecl(ctxt->userData, name, XML_INTERNAL_GENERAL_ENTITY, NULL, NULL, value);
				/*
				 * For expat compatibility in SAX mode.
				 */
				if((ctxt->myDoc == NULL) || (sstreq(ctxt->myDoc->version, SAX_COMPAT_MODE))) {
					if(ctxt->myDoc == NULL) {
						ctxt->myDoc = xmlNewDoc(SAX_COMPAT_MODE);
						if(ctxt->myDoc == NULL) {
							xmlErrMemory(ctxt, "New Doc failed");
							return;
						}
						ctxt->myDoc->properties = XML_DOC_INTERNAL;
					}
					SETIFZ(ctxt->myDoc->intSubset, xmlNewDtd(ctxt->myDoc, BAD_CAST "fake", NULL, NULL));
					xmlSAX2EntityDecl(ctxt, name, XML_INTERNAL_GENERAL_ENTITY, NULL, NULL, value);
				}
			}
			else {
				URI = xmlParseExternalID(ctxt, &literal, 1);
				if((URI == NULL) && (literal == NULL)) {
					xmlFatalErr(ctxt, XML_ERR_VALUE_REQUIRED, 0);
				}
				if(URI) {
					xmlURIPtr uri = xmlParseURI((const char*)URI);
					if(uri == NULL) {
						xmlErrMsgStr(ctxt, XML_ERR_INVALID_URI, "Invalid URI: %s\n", URI);
						/*
						 * This really ought to be a well formedness error
						 * but the XML Core WG decided otherwise c.f. issue
						 * E26 of the XML erratas.
						 */
					}
					else {
						if(uri->fragment) { // Okay this is foolish to block those but not invalid URIs.
							xmlFatalErr(ctxt, XML_ERR_URI_FRAGMENT, 0);
						}
						xmlFreeURI(uri);
					}
				}
				if((RAW != '>') && (!IS_BLANK_CH(CUR))) {
					xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required before 'NDATA'\n");
				}
				SKIP_BLANKS;
				if(CMP5(CUR_PTR, 'N', 'D', 'A', 'T', 'A')) {
					SKIP(5);
					if(!IS_BLANK_CH(CUR)) {
						xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after 'NDATA'\n");
					}
					SKIP_BLANKS;
					ndata = xmlParseName(ctxt);
					if(ctxt->sax && !ctxt->disableSAX && ctxt->sax->unparsedEntityDecl)
						ctxt->sax->unparsedEntityDecl(ctxt->userData, name, literal, URI, ndata);
				}
				else {
					if(ctxt->sax && !ctxt->disableSAX && ctxt->sax->entityDecl)
						ctxt->sax->entityDecl(ctxt->userData, name, XML_EXTERNAL_GENERAL_PARSED_ENTITY, literal, URI, 0);
					/*
					 * For expat compatibility in SAX mode.
					 * assuming the entity repalcement was asked for
					 */
					if(ctxt->replaceEntities && (!ctxt->myDoc || (sstreq(ctxt->myDoc->version, SAX_COMPAT_MODE)))) {
						if(ctxt->myDoc == NULL) {
							ctxt->myDoc = xmlNewDoc(SAX_COMPAT_MODE);
							if(ctxt->myDoc == NULL) {
								xmlErrMemory(ctxt, "New Doc failed");
								return;
							}
							ctxt->myDoc->properties = XML_DOC_INTERNAL;
						}
						SETIFZ(ctxt->myDoc->intSubset, xmlNewDtd(ctxt->myDoc, BAD_CAST "fake", NULL, NULL));
						xmlSAX2EntityDecl(ctxt, name, XML_EXTERNAL_GENERAL_PARSED_ENTITY, literal, URI, 0);
					}
				}
			}
		}
		if(ctxt->instate == XML_PARSER_EOF)
			return;
		SKIP_BLANKS;
		if(RAW != '>') {
			xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_NOT_FINISHED, "xmlParseEntityDecl: entity %s not terminated\n", name);
		}
		else {
			if(input != ctxt->input) {
				xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY, "Entity declaration doesn't start and stop in the same entity\n");
			}
			NEXT;
		}
		if(orig) {
			/*
			 * Ugly mechanism to save the raw entity value.
			 */
			xmlEntity * cur = NULL;
			if(isParameter) {
				if(ctxt->sax && ctxt->sax->getParameterEntity)
					cur = ctxt->sax->getParameterEntity(ctxt->userData, name);
			}
			else {
				if(ctxt->sax && ctxt->sax->getEntity)
					cur = ctxt->sax->getEntity(ctxt->userData, name);
				if(!cur && ctxt->userData == ctxt) {
					cur = xmlSAX2GetEntity(ctxt, name);
				}
			}
			if(cur) {
				if(cur->orig)
					SAlloc::F(orig);
				else
					cur->orig = orig;
			}
			else
				SAlloc::F(orig);
		}
		SAlloc::F(value);
		SAlloc::F(URI);
		SAlloc::F(literal);
	}
}

/**
 * xmlParseDefaultDecl:
 * @ctxt:  an XML parser context
 * @value:  Receive a possible fixed default value for the attribute
 *
 * Parse an attribute default declaration
 *
 * [60] DefaultDecl ::= '#REQUIRED' | '#IMPLIED' | (('#FIXED' S)? AttValue)
 *
 * [VC: Required Attribute]
 * if the default declaration is the keyword #REQUIRED, then the
 * attribute must be specified for all elements of the type in the
 * attribute-list declaration.
 *
 * [VC: Attribute Default Legal]
 * The declared default value must meet the lexical constraints of
 * the declared attribute type c.f. xmlValidateAttributeDecl()
 *
 * [VC: Fixed Attribute Default]
 * if an attribute has a default value declared with the #FIXED
 * keyword, instances of that attribute must match the default value.
 *
 * [WFC: No < in Attribute Values]
 * handled in xmlParseAttValue()
 *
 * returns: XML_ATTRIBUTE_NONE, XML_ATTRIBUTE_REQUIRED, XML_ATTRIBUTE_IMPLIED
 *          or XML_ATTRIBUTE_FIXED.
 */

int xmlParseDefaultDecl(xmlParserCtxt * ctxt, xmlChar ** value)
{
	*value = NULL;
	const xmlChar * p__cp = CUR_PTR;
	if(CMP9(p__cp, '#', 'R', 'E', 'Q', 'U', 'I', 'R', 'E', 'D')) {
		SKIP(9);
		return XML_ATTRIBUTE_REQUIRED;
	}
	else if(CMP8(p__cp, '#', 'I', 'M', 'P', 'L', 'I', 'E', 'D')) {
		SKIP(8);
		return XML_ATTRIBUTE_IMPLIED;
	}
	else {
		int val = XML_ATTRIBUTE_NONE;
		if(CMP6(p__cp, '#', 'F', 'I', 'X', 'E', 'D')) {
			SKIP(6);
			val = XML_ATTRIBUTE_FIXED;
			if(!IS_BLANK_CH(CUR))
				xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after '#FIXED'\n");
			SKIP_BLANKS;
		}
		{
			xmlChar * ret = xmlParseAttValue(ctxt);
			ctxt->instate = XML_PARSER_DTD;
			if(!ret)
				xmlFatalErrMsg(ctxt, (xmlParserErrors)ctxt->errNo, "Attribute default value declaration error\n");
			else
				*value = ret;
			return val;
		}
	}
}
/**
 * xmlParseNotationType:
 * @ctxt:  an XML parser context
 *
 * parse an Notation attribute type.
 *
 * Note: the leading 'NOTATION' S part has already being parsed...
 *
 * [58] NotationType ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
 *
 * [VC: Notation Attributes]
 * Values of this type must match one of the notation names included
 * in the declaration; all notation names in the declaration must be declared.
 *
 * Returns: the notation attribute tree built while parsing
 */

xmlEnumeration * xmlParseNotationType(xmlParserCtxt * ctxt)
{
	const xmlChar * name;
	xmlEnumeration * ret = NULL;
	xmlEnumeration * last = NULL;
	xmlEnumeration * cur;
	xmlEnumeration * tmp;
	if(RAW != '(') {
		xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_STARTED, 0);
		return 0;
	}
	SHRINK;
	do {
		NEXT;
		SKIP_BLANKS;
		name = xmlParseName(ctxt);
		if(!name) {
			xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "Name expected in NOTATION declaration\n");
			xmlFreeEnumeration(ret);
			return 0;
		}
		for(tmp = ret; tmp; tmp = tmp->next) {
			if(sstreq(name, tmp->name)) {
				xmlValidityError(ctxt, XML_DTD_DUP_TOKEN, "standalone: attribute notation value token %s duplicated\n", name, 0);
				if(!xmlDictOwns(ctxt->dict, name))
					SAlloc::F((xmlChar*)name);
				break;
			}
		}
		if(!tmp) {
			cur = xmlCreateEnumeration(name);
			if(!cur) {
				xmlFreeEnumeration(ret);
				return 0;
			}
			if(last == NULL)
				ret = last = cur;
			else {
				last->next = cur;
				last = cur;
			}
		}
		SKIP_BLANKS;
	} while(RAW == '|');
	if(RAW != ')') {
		xmlFatalErr(ctxt, XML_ERR_NOTATION_NOT_FINISHED, 0);
		xmlFreeEnumeration(ret);
		return 0;
	}
	NEXT;
	return ret;
}

/**
 * xmlParseEnumerationType:
 * @ctxt:  an XML parser context
 *
 * parse an Enumeration attribute type.
 *
 * [59] Enumeration ::= '(' S? Nmtoken (S? '|' S? Nmtoken)* S? ')'
 *
 * [VC: Enumeration]
 * Values of this type must match one of the Nmtoken tokens in
 * the declaration
 *
 * Returns: the enumeration attribute tree built while parsing
 */
xmlEnumeration * xmlParseEnumerationType(xmlParserCtxt * ctxt)
{
	xmlChar * name;
	xmlEnumeration * ret = NULL;
	xmlEnumeration * last = NULL;
	xmlEnumeration * cur;
	xmlEnumeration * tmp;
	if(RAW != '(') {
		xmlFatalErr(ctxt, XML_ERR_ATTLIST_NOT_STARTED, 0);
		return 0;
	}
	SHRINK;
	do {
		NEXT;
		SKIP_BLANKS;
		name = xmlParseNmtoken(ctxt);
		if(!name) {
			xmlFatalErr(ctxt, XML_ERR_NMTOKEN_REQUIRED, 0);
			return ret;
		}
		else {
			tmp = ret;
			while(tmp) {
				if(sstreq(name, tmp->name)) {
					xmlValidityError(ctxt, XML_DTD_DUP_TOKEN, "standalone: attribute enumeration value token %s duplicated\n", name, 0);
					if(!xmlDictOwns(ctxt->dict, name))
						SAlloc::F(name);
					break;
				}
				tmp = tmp->next;
			}
			if(!tmp) {
				cur = xmlCreateEnumeration(name);
				if(!xmlDictOwns(ctxt->dict, name))
					SAlloc::F(name);
				if(!cur) {
					xmlFreeEnumeration(ret);
					return 0;
				}
				if(last == NULL)
					ret = last = cur;
				else {
					last->next = cur;
					last = cur;
				}
			}
			SKIP_BLANKS;
		}
	} while(RAW == '|');
	if(RAW != ')') {
		xmlFatalErr(ctxt, XML_ERR_ATTLIST_NOT_FINISHED, 0);
	}
	else {
		NEXT;
	}
	return ret;
}

/**
 * xmlParseEnumeratedType:
 * @ctxt:  an XML parser context
 * @tree:  the enumeration tree built while parsing
 *
 * parse an Enumerated attribute type.
 *
 * [57] EnumeratedType ::= NotationType | Enumeration
 *
 * [58] NotationType ::= 'NOTATION' S '(' S? Name (S? '|' S? Name)* S? ')'
 *
 *
 * Returns: XML_ATTRIBUTE_ENUMERATION or XML_ATTRIBUTE_NOTATION
 */

int xmlParseEnumeratedType(xmlParserCtxt * ctxt, xmlEnumeration ** tree)
{
	const xmlChar * p__cp = CUR_PTR;
	if(CMP8(p__cp, 'N', 'O', 'T', 'A', 'T', 'I', 'O', 'N')) {
		SKIP(8);
		if(!IS_BLANK_CH(CUR)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after 'NOTATION'\n");
			return 0;
		}
		else {
			SKIP_BLANKS;
			*tree = xmlParseNotationType(ctxt);
			return *tree ? XML_ATTRIBUTE_NOTATION : 0;
		}
	}
	else {
		*tree = xmlParseEnumerationType(ctxt);
		return *tree ? XML_ATTRIBUTE_ENUMERATION : 0;
	}
}

/**
 * xmlParseAttributeType:
 * @ctxt:  an XML parser context
 * @tree:  the enumeration tree built while parsing
 *
 * parse the Attribute list def for an element
 *
 * [54] AttType ::= StringType | TokenizedType | EnumeratedType
 *
 * [55] StringType ::= 'CDATA'
 *
 * [56] TokenizedType ::= 'ID' | 'IDREF' | 'IDREFS' | 'ENTITY' |
 *                        'ENTITIES' | 'NMTOKEN' | 'NMTOKENS'
 *
 * Validity constraints for attribute values syntax are checked in
 * xmlValidateAttributeValue()
 *
 * [VC: ID]
 * Values of type ID must match the Name production. A name must not
 * appear more than once in an XML document as a value of this type;
 * i.e., ID values must uniquely identify the elements which bear them.
 *
 * [VC: One ID per Element Type]
 * No element type may have more than one ID attribute specified.
 *
 * [VC: ID Attribute Default]
 * An ID attribute must have a declared default of #IMPLIED or #REQUIRED.
 *
 * [VC: IDREF]
 * Values of type IDREF must match the Name production, and values
 * of type IDREFS must match Names; each IDREF Name must match the value
 * of an ID attribute on some element in the XML document; i.e. IDREF
 * values must match the value of some ID attribute.
 *
 * [VC: Entity Name]
 * Values of type ENTITY must match the Name production, values
 * of type ENTITIES must match Names; each Entity Name must match the
 * name of an unparsed entity declared in the DTD.
 *
 * [VC: Name Token]
 * Values of type NMTOKEN must match the Nmtoken production; values
 * of type NMTOKENS must match Nmtokens.
 *
 * Returns the attribute type
 */
int xmlParseAttributeType(xmlParserCtxt * ctxt, xmlEnumeration ** tree)
{
	SHRINK;
	const xmlChar * p__cp = CUR_PTR;
	if(CMP5(p__cp, 'C', 'D', 'A', 'T', 'A')) {
		SKIP(5);
		return(XML_ATTRIBUTE_CDATA);
	}
	else if(CMP6(p__cp, 'I', 'D', 'R', 'E', 'F', 'S')) {
		SKIP(6);
		return(XML_ATTRIBUTE_IDREFS);
	}
	else if(CMP5(p__cp, 'I', 'D', 'R', 'E', 'F')) {
		SKIP(5);
		return(XML_ATTRIBUTE_IDREF);
	}
	else if((RAW == 'I') && (NXT(1) == 'D')) {
		SKIP(2);
		return(XML_ATTRIBUTE_ID);
	}
	else if(CMP6(p__cp, 'E', 'N', 'T', 'I', 'T', 'Y')) {
		SKIP(6);
		return(XML_ATTRIBUTE_ENTITY);
	}
	else if(CMP8(p__cp, 'E', 'N', 'T', 'I', 'T', 'I', 'E', 'S')) {
		SKIP(8);
		return(XML_ATTRIBUTE_ENTITIES);
	}
	else if(CMP8(p__cp, 'N', 'M', 'T', 'O', 'K', 'E', 'N', 'S')) {
		SKIP(8);
		return(XML_ATTRIBUTE_NMTOKENS);
	}
	else if(CMP7(p__cp, 'N', 'M', 'T', 'O', 'K', 'E', 'N')) {
		SKIP(7);
		return(XML_ATTRIBUTE_NMTOKEN);
	}
	else
		return xmlParseEnumeratedType(ctxt, tree);
}

/**
 * xmlParseAttributeListDecl:
 * @ctxt:  an XML parser context
 *
 * : parse the Attribute list def for an element
 *
 * [52] AttlistDecl ::= '<!ATTLIST' S Name AttDef* S? '>'
 *
 * [53] AttDef ::= S Name S AttType S DefaultDecl
 *
 */
void xmlParseAttributeListDecl(xmlParserCtxt * ctxt)
{
	const xmlChar * elemName;
	const xmlChar * attrName;
	xmlEnumeration * tree;
	if(CMP9(CUR_PTR, '<', '!', 'A', 'T', 'T', 'L', 'I', 'S', 'T')) {
		xmlParserInput * input = ctxt->input;
		SKIP(9);
		if(!IS_BLANK_CH(CUR)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after '<!ATTLIST'\n");
		}
		SKIP_BLANKS;
		elemName = xmlParseName(ctxt);
		if(elemName == NULL) {
			xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "ATTLIST: no name for Element\n");
			return;
		}
		SKIP_BLANKS;
		GROW;
		while((RAW != '>') && (ctxt->instate != XML_PARSER_EOF)) {
			const xmlChar * check = CUR_PTR;
			int type;
			int def;
			xmlChar * defaultValue = NULL;
			GROW;
			tree = NULL;
			attrName = xmlParseName(ctxt);
			if(attrName == NULL) {
				xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "ATTLIST: no name for Attribute\n");
				break;
			}
			GROW;
			if(!IS_BLANK_CH(CUR)) {
				xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after the attribute name\n");
				break;
			}
			SKIP_BLANKS;
			type = xmlParseAttributeType(ctxt, &tree);
			if(type <= 0) {
				break;
			}
			GROW;
			if(!IS_BLANK_CH(CUR)) {
				xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after the attribute type\n");
				xmlFreeEnumeration(tree);
				break;
			}
			SKIP_BLANKS;
			def = xmlParseDefaultDecl(ctxt, &defaultValue);
			if(def <= 0) {
				SAlloc::F(defaultValue);
				xmlFreeEnumeration(tree);
				break;
			}
			if((type != XML_ATTRIBUTE_CDATA) && defaultValue)
				xmlAttrNormalizeSpace(defaultValue, defaultValue);
			GROW;
			if(RAW != '>') {
				if(!IS_BLANK_CH(CUR)) {
					xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after the attribute default value\n");
					SAlloc::F(defaultValue);
					xmlFreeEnumeration(tree);
					break;
				}
				SKIP_BLANKS;
			}
			if(check == CUR_PTR) {
				xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "in xmlParseAttributeListDecl\n");
				SAlloc::F(defaultValue);
				xmlFreeEnumeration(tree);
				break;
			}
			if(ctxt->sax && !ctxt->disableSAX && ctxt->sax->attributeDecl)
				ctxt->sax->attributeDecl(ctxt->userData, elemName, attrName, type, def, defaultValue, tree);
			else
				xmlFreeEnumeration(tree);
			if(ctxt->sax2 && defaultValue && (def != XML_ATTRIBUTE_IMPLIED) && (def != XML_ATTRIBUTE_REQUIRED))
				xmlAddDefAttrs(ctxt, elemName, attrName, defaultValue);
			if(ctxt->sax2)
				xmlAddSpecialAttr(ctxt, elemName, attrName, type);
			SAlloc::F(defaultValue);
			GROW;
		}
		if(RAW == '>') {
			if(input != ctxt->input) {
				xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY, "Attribute list declaration doesn't start and stop in the same entity\n", 0, 0);
			}
			NEXT;
		}
	}
}

/**
 * xmlParseElementMixedContentDecl:
 * @ctxt:  an XML parser context
 * @inputchk:  the input used for the current entity, needed for boundary checks
 *
 * parse the declaration for a Mixed Element content
 * The leading '(' and spaces have been skipped in xmlParseElementContentDecl
 *
 * [51] Mixed ::= '(' S? '#PCDATA' (S? '|' S? Name)* S? ')*' |
 *                '(' S? '#PCDATA' S? ')'
 *
 * [VC: Proper Group/PE Nesting] applies to [51] too (see [49])
 *
 * [VC: No Duplicate Types]
 * The same name must not appear more than once in a single
 * mixed-content declaration.
 *
 * returns: the list of the xmlElementContentPtr describing the element choices
 */
xmlElementContent * xmlParseElementMixedContentDecl(xmlParserCtxt * ctxt, int inputchk)
{
	xmlElementContent * ret = NULL;
	xmlElementContent * cur = NULL;
	xmlElementContent * n;
	const xmlChar * elem = NULL;
	GROW;
	if(CMP7(CUR_PTR, '#', 'P', 'C', 'D', 'A', 'T', 'A')) {
		SKIP(7);
		SKIP_BLANKS;
		SHRINK;
		if(RAW == ')') {
			if((ctxt->validate) && (ctxt->input->id != inputchk)) {
				xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY, "Element content declaration doesn't start and stop in the same entity\n", 0, 0);
			}
			NEXT;
			ret = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_PCDATA);
			if(!ret)
				return 0;
			if(RAW == '*') {
				ret->ocur = XML_ELEMENT_CONTENT_MULT;
				NEXT;
			}
			return ret;
		}
		if((RAW == '(') || (RAW == '|')) {
			ret = cur = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_PCDATA);
			if(!ret) return 0;
		}
		while((RAW == '|') && (ctxt->instate != XML_PARSER_EOF)) {
			NEXT;
			if(elem == NULL) {
				ret = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_OR);
				if(!ret)
					return 0;
				ret->c1 = cur;
				if(cur)
					cur->parent = ret;
				cur = ret;
			}
			else {
				n = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_OR);
				if(n == NULL)
					return 0;
				n->c1 = xmlNewDocElementContent(ctxt->myDoc, elem, XML_ELEMENT_CONTENT_ELEMENT);
				if(n->c1 != NULL)
					n->c1->parent = n;
				cur->c2 = n;
				if(n != NULL)
					n->parent = cur;
				cur = n;
			}
			SKIP_BLANKS;
			elem = xmlParseName(ctxt);
			if(elem == NULL) {
				xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "xmlParseElementMixedContentDecl : Name expected\n");
				xmlFreeDocElementContent(ctxt->myDoc, cur);
				return 0;
			}
			SKIP_BLANKS;
			GROW;
		}
		if((RAW == ')') && (NXT(1) == '*')) {
			if(elem != NULL) {
				cur->c2 = xmlNewDocElementContent(ctxt->myDoc, elem, XML_ELEMENT_CONTENT_ELEMENT);
				if(cur->c2 != NULL)
					cur->c2->parent = cur;
			}
			if(ret)
				ret->ocur = XML_ELEMENT_CONTENT_MULT;
			if((ctxt->validate) && (ctxt->input->id != inputchk)) {
				xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY, "Element content declaration doesn't start and stop in the same entity\n", 0, 0);
			}
			SKIP(2);
		}
		else {
			xmlFreeDocElementContent(ctxt->myDoc, ret);
			xmlFatalErr(ctxt, XML_ERR_MIXED_NOT_STARTED, 0);
			return 0;
		}
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_PCDATA_REQUIRED, 0);
	}
	return ret;
}
/**
 * xmlParseElementChildrenContentDeclPriv:
 * @ctxt:  an XML parser context
 * @inputchk:  the input used for the current entity, needed for boundary checks
 * @depth: the level of recursion
 *
 * parse the declaration for a Mixed Element content
 * The leading '(' and spaces have been skipped in xmlParseElementContentDecl
 *
 * [47] children ::= (choice | seq) ('?' | '*' | '+')?
 * [48] cp ::= (Name | choice | seq) ('?' | '*' | '+')?
 * [49] choice ::= '(' S? cp ( S? '|' S? cp )* S? ')'
 * [50] seq ::= '(' S? cp ( S? ',' S? cp )* S? ')'
 *
 * [VC: Proper Group/PE Nesting] applies to [49] and [50]
 * @todo Parameter-entity replacement text must be properly nested
 *	with parenthesized groups. That is to say, if either of the
 *	opening or closing parentheses in a choice, seq, or Mixed
 *	construct is contained in the replacement text for a parameter
 *	entity, both must be contained in the same replacement text. For
 *	interoperability, if a parameter-entity reference appears in a
 *	choice, seq, or Mixed construct, its replacement text should not
 *	be empty, and neither the first nor last non-blank character of
 *	the replacement text should be a connector (| or ,).
 *
 * Returns the tree of xmlElementContentPtr describing the element hierarchy.
 */
static xmlElementContent * xmlParseElementChildrenContentDeclPriv(xmlParserCtxt * ctxt, int inputchk, int depth)
{
	xmlElementContent * ret = NULL;
	xmlElementContent * cur = NULL;
	xmlElementContent * last = NULL;
	xmlElementContent * op = NULL;
	const xmlChar * elem;
	xmlChar type = 0;
	if(((depth > 128) && ((ctxt->options & XML_PARSE_HUGE) == 0)) || (depth >  2048)) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_ELEMCONTENT_NOT_FINISHED,
		    "xmlParseElementChildrenContentDecl : depth %d too deep, use XML_PARSE_HUGE\n", depth);
		return 0;
	}
	SKIP_BLANKS;
	GROW;
	if(RAW == '(') {
		int inputid = ctxt->input->id;
		/* Recurse on first child */
		NEXT;
		SKIP_BLANKS;
		cur = ret = xmlParseElementChildrenContentDeclPriv(ctxt, inputid, depth + 1);
		SKIP_BLANKS;
		GROW;
	}
	else {
		elem = xmlParseName(ctxt);
		if(elem == NULL) {
			xmlFatalErr(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED, 0);
			return 0;
		}
		cur = ret = xmlNewDocElementContent(ctxt->myDoc, elem, XML_ELEMENT_CONTENT_ELEMENT);
		if(!cur) {
			xmlErrMemory(ctxt, 0);
			return 0;
		}
		GROW;
		if(RAW == '?') {
			cur->ocur = XML_ELEMENT_CONTENT_OPT;
			NEXT;
		}
		else if(RAW == '*') {
			cur->ocur = XML_ELEMENT_CONTENT_MULT;
			NEXT;
		}
		else if(RAW == '+') {
			cur->ocur = XML_ELEMENT_CONTENT_PLUS;
			NEXT;
		}
		else {
			cur->ocur = XML_ELEMENT_CONTENT_ONCE;
		}
		GROW;
	}
	SKIP_BLANKS;
	SHRINK;
	while((RAW != ')') && (ctxt->instate != XML_PARSER_EOF)) {
		/*
		 * Each loop we parse one separator and one element.
		 */
		if(RAW == ',') {
			if(type == 0) 
				type = CUR;
			else if(type != CUR) { // Detect "Name | Name , Name" error
				xmlFatalErrMsgInt(ctxt, XML_ERR_SEPARATOR_REQUIRED, "xmlParseElementChildrenContentDecl : '%c' expected\n", type);
				if(last && last != ret)
					xmlFreeDocElementContent(ctxt->myDoc, last);
				xmlFreeDocElementContent(ctxt->myDoc, ret);
				return 0;
			}
			NEXT;
			op = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_SEQ);
			if(op == NULL) {
				if(last && last != ret)
					xmlFreeDocElementContent(ctxt->myDoc, last);
				xmlFreeDocElementContent(ctxt->myDoc, ret);
				return 0;
			}
			if(last == NULL) {
				op->c1 = ret;
				if(ret)
					ret->parent = op;
				ret = cur = op;
			}
			else {
				cur->c2 = op;
				if(op != NULL)
					op->parent = cur;
				op->c1 = last;
				if(last != NULL)
					last->parent = op;
				cur = op;
				last = NULL;
			}
		}
		else if(RAW == '|') {
			if(type == 0)
				type = CUR;
			else if(type != CUR) { // Detect "Name , Name | Name" error
				xmlFatalErrMsgInt(ctxt, XML_ERR_SEPARATOR_REQUIRED, "xmlParseElementChildrenContentDecl : '%c' expected\n", type);
				if(last && last != ret)
					xmlFreeDocElementContent(ctxt->myDoc, last);
				xmlFreeDocElementContent(ctxt->myDoc, ret);
				return 0;
			}
			NEXT;
			op = xmlNewDocElementContent(ctxt->myDoc, NULL, XML_ELEMENT_CONTENT_OR);
			if(op == NULL) {
				if(last && last != ret)
					xmlFreeDocElementContent(ctxt->myDoc, last);
				xmlFreeDocElementContent(ctxt->myDoc, ret);
				return 0;
			}
			if(last == NULL) {
				op->c1 = ret;
				if(ret)
					ret->parent = op;
				ret = cur = op;
			}
			else {
				cur->c2 = op;
				if(op)
					op->parent = cur;
				op->c1 = last;
				if(last)
					last->parent = op;
				cur = op;
				last = NULL;
			}
		}
		else {
			xmlFatalErr(ctxt, XML_ERR_ELEMCONTENT_NOT_FINISHED, 0);
			if(last && last != ret)
				xmlFreeDocElementContent(ctxt->myDoc, last);
			xmlFreeDocElementContent(ctxt->myDoc, ret);
			return 0;
		}
		GROW;
		SKIP_BLANKS;
		GROW;
		if(RAW == '(') {
			int inputid = ctxt->input->id;
			// Recurse on second child 
			NEXT;
			SKIP_BLANKS;
			last = xmlParseElementChildrenContentDeclPriv(ctxt, inputid, depth + 1);
			SKIP_BLANKS;
		}
		else {
			elem = xmlParseName(ctxt);
			if(elem == NULL) {
				xmlFatalErr(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED, 0);
				xmlFreeDocElementContent(ctxt->myDoc, ret);
				return 0;
			}
			last = xmlNewDocElementContent(ctxt->myDoc, elem, XML_ELEMENT_CONTENT_ELEMENT);
			if(last == NULL) {
				xmlFreeDocElementContent(ctxt->myDoc, ret);
				return 0;
			}
			if(RAW == '?') {
				last->ocur = XML_ELEMENT_CONTENT_OPT;
				NEXT;
			}
			else if(RAW == '*') {
				last->ocur = XML_ELEMENT_CONTENT_MULT;
				NEXT;
			}
			else if(RAW == '+') {
				last->ocur = XML_ELEMENT_CONTENT_PLUS;
				NEXT;
			}
			else {
				last->ocur = XML_ELEMENT_CONTENT_ONCE;
			}
		}
		SKIP_BLANKS;
		GROW;
	}
	if(cur && last) {
		cur->c2 = last;
		if(last)
			last->parent = cur;
	}
	if((ctxt->validate) && (ctxt->input->id != inputchk)) {
		xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY, "Element content declaration doesn't start and stop in the same entity\n", 0, 0);
	}
	NEXT;
	if(RAW == '?') {
		if(ret) {
			ret->ocur = (oneof2(ret->ocur, XML_ELEMENT_CONTENT_PLUS, XML_ELEMENT_CONTENT_MULT)) ? XML_ELEMENT_CONTENT_MULT : XML_ELEMENT_CONTENT_OPT;
		}
		NEXT;
	}
	else if(RAW == '*') {
		if(ret) {
			ret->ocur = XML_ELEMENT_CONTENT_MULT;
			cur = ret;
			// 
			// Some normalization: (a | b* | c?)* == (a | b | c)*
			// 
			while(cur && (cur->type == XML_ELEMENT_CONTENT_OR)) {
				if(cur->c1 && oneof2(cur->c1->ocur, XML_ELEMENT_CONTENT_OPT, XML_ELEMENT_CONTENT_MULT))
					cur->c1->ocur = XML_ELEMENT_CONTENT_ONCE;
				if(cur->c2 && oneof2(cur->c2->ocur, XML_ELEMENT_CONTENT_OPT, XML_ELEMENT_CONTENT_MULT))
					cur->c2->ocur = XML_ELEMENT_CONTENT_ONCE;
				cur = cur->c2;
			}
		}
		NEXT;
	}
	else if(RAW == '+') {
		if(ret) {
			int found = 0;
			if((ret->ocur == XML_ELEMENT_CONTENT_OPT) || (ret->ocur == XML_ELEMENT_CONTENT_MULT))
				ret->ocur = XML_ELEMENT_CONTENT_MULT;
			else
				ret->ocur = XML_ELEMENT_CONTENT_PLUS;
			// 
			// Some normalization:
			// (a | b*)+ == (a | b)*
			// (a | b?)+ == (a | b)*
			// 
			while(cur && (cur->type == XML_ELEMENT_CONTENT_OR)) {
				if(cur->c1 && ((cur->c1->ocur == XML_ELEMENT_CONTENT_OPT) || (cur->c1->ocur == XML_ELEMENT_CONTENT_MULT))) {
					cur->c1->ocur = XML_ELEMENT_CONTENT_ONCE;
					found = 1;
				}
				if(cur->c2 && ((cur->c2->ocur == XML_ELEMENT_CONTENT_OPT) || (cur->c2->ocur == XML_ELEMENT_CONTENT_MULT))) {
					cur->c2->ocur = XML_ELEMENT_CONTENT_ONCE;
					found = 1;
				}
				cur = cur->c2;
			}
			if(found)
				ret->ocur = XML_ELEMENT_CONTENT_MULT;
		}
		NEXT;
	}
	return ret;
}
/**
 * xmlParseElementChildrenContentDecl:
 * @ctxt:  an XML parser context
 * @inputchk:  the input used for the current entity, needed for boundary checks
 *
 * parse the declaration for a Mixed Element content
 * The leading '(' and spaces have been skipped in xmlParseElementContentDecl
 *
 * [47] children ::= (choice | seq) ('?' | '*' | '+')?
 *
 * [48] cp ::= (Name | choice | seq) ('?' | '*' | '+')?
 *
 * [49] choice ::= '(' S? cp ( S? '|' S? cp )* S? ')'
 *
 * [50] seq ::= '(' S? cp ( S? ',' S? cp )* S? ')'
 *
 * [VC: Proper Group/PE Nesting] applies to [49] and [50]
 * @todo Parameter-entity replacement text must be properly nested
 *	with parenthesized groups. That is to say, if either of the
 *	opening or closing parentheses in a choice, seq, or Mixed
 *	construct is contained in the replacement text for a parameter
 *	entity, both must be contained in the same replacement text. For
 *	interoperability, if a parameter-entity reference appears in a
 *	choice, seq, or Mixed construct, its replacement text should not
 *	be empty, and neither the first nor last non-blank character of
 *	the replacement text should be a connector (| or ,).
 *
 * Returns the tree of xmlElementContentPtr describing the element hierarchy.
 */
xmlElementContent * xmlParseElementChildrenContentDecl(xmlParserCtxt * ctxt, int inputchk)
{
	/* stub left for API/ABI compat */
	return xmlParseElementChildrenContentDeclPriv(ctxt, inputchk, 1);
}
/**
 * xmlParseElementContentDecl:
 * @ctxt:  an XML parser context
 * @name:  the name of the element being defined.
 * @result:  the Element Content pointer will be stored here if any
 *
 * parse the declaration for an Element content either Mixed or Children,
 * the cases EMPTY and ANY are handled directly in xmlParseElementDecl
 *
 * [46] contentspec ::= 'EMPTY' | 'ANY' | Mixed | children
 *
 * returns: the type of element content XML_ELEMENT_TYPE_xxx
 */
int xmlParseElementContentDecl(xmlParserCtxt * ctxt, const xmlChar * name, xmlElementContent ** result)
{
	xmlElementContent * tree = NULL;
	int inputid = ctxt->input->id;
	int res;
	*result = NULL;
	if(RAW != '(') {
		xmlFatalErrMsgStr(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED, "xmlParseElementContentDecl : %s '(' expected\n", name);
		return -1;
	}
	NEXT;
	GROW;
	if(ctxt->instate == XML_PARSER_EOF)
		return -1;
	SKIP_BLANKS;
	if(CMP7(CUR_PTR, '#', 'P', 'C', 'D', 'A', 'T', 'A')) {
		tree = xmlParseElementMixedContentDecl(ctxt, inputid);
		res = XML_ELEMENT_TYPE_MIXED;
	}
	else {
		tree = xmlParseElementChildrenContentDeclPriv(ctxt, inputid, 1);
		res = XML_ELEMENT_TYPE_ELEMENT;
	}
	SKIP_BLANKS;
	*result = tree;
	return res;
}
/**
 * xmlParseElementDecl:
 * @ctxt:  an XML parser context
 *
 * parse an Element declaration.
 *
 * [45] elementdecl ::= '<!ELEMENT' S Name S contentspec S? '>'
 *
 * [VC: Unique Element Type Declaration]
 * No element type may be declared more than once
 *
 * Returns the type of the element, or -1 in case of error
 */
int xmlParseElementDecl(xmlParserCtxt * ctxt)
{
	const xmlChar * name;
	int ret = -1;
	xmlElementContent * content  = NULL;
	// GROW; done in the caller 
	if(CMP9(CUR_PTR, '<', '!', 'E', 'L', 'E', 'M', 'E', 'N', 'T')) {
		xmlParserInputPtr input = ctxt->input;
		SKIP(9);
		if(!IS_BLANK_CH(CUR)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after 'ELEMENT'\n");
		}
		SKIP_BLANKS;
		name = xmlParseName(ctxt);
		if(!name) {
			xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "xmlParseElementDecl: no name for Element\n");
			return -1;
		}
		while((RAW == 0) && (ctxt->inputNr > 1))
			xmlPopInput(ctxt);
		if(!IS_BLANK_CH(CUR)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space required after the element name\n");
		}
		SKIP_BLANKS;
		if(CMP5(CUR_PTR, 'E', 'M', 'P', 'T', 'Y')) {
			SKIP(5);
			/*
			 * Element must always be empty.
			 */
			ret = XML_ELEMENT_TYPE_EMPTY;
		}
		else if((RAW == 'A') && (NXT(1) == 'N') && (NXT(2) == 'Y')) {
			SKIP(3);
			/*
			 * Element is a generic container.
			 */
			ret = XML_ELEMENT_TYPE_ANY;
		}
		else if(RAW == '(') {
			ret = xmlParseElementContentDecl(ctxt, name, &content);
		}
		else {
			/*
			 * [WFC: PEs in Internal Subset] error handling.
			 */
			if((RAW == '%') && (ctxt->external == 0) && (ctxt->inputNr == 1)) {
				xmlFatalErrMsg(ctxt, XML_ERR_PEREF_IN_INT_SUBSET, "PEReference: forbidden within markup decl in internal subset\n");
			}
			else {
				xmlFatalErrMsg(ctxt, XML_ERR_ELEMCONTENT_NOT_STARTED, "xmlParseElementDecl: 'EMPTY', 'ANY' or '(' expected\n");
			}
			return -1;
		}

		SKIP_BLANKS;
		/*
		 * Pop-up of finished entities.
		 */
		while((RAW == 0) && (ctxt->inputNr > 1))
			xmlPopInput(ctxt);
		SKIP_BLANKS;
		if(RAW != '>') {
			xmlFatalErr(ctxt, XML_ERR_GT_REQUIRED, 0);
			xmlFreeDocElementContent(ctxt->myDoc, content);
		}
		else {
			if(input != ctxt->input) {
				xmlFatalErrMsg(ctxt, XML_ERR_ENTITY_BOUNDARY, "Element declaration doesn't start and stop in the same entity\n");
			}
			NEXT;
			if(ctxt->sax && !ctxt->disableSAX && ctxt->sax->elementDecl) {
				if(content)
					content->parent = NULL;
				ctxt->sax->elementDecl(ctxt->userData, name, ret, content);
				if(content && content->parent == NULL) {
					/*
					 * this is a trick: if xmlAddElementDecl is called,
					 * instead of copying the full tree it is plugged directly
					 * if called from the parser. Avoid duplicating the interfaces or change the API/ABI
					 */
					xmlFreeDocElementContent(ctxt->myDoc, content);
				}
			}
			else
				xmlFreeDocElementContent(ctxt->myDoc, content);
		}
	}
	return ret;
}
/**
 * xmlParseConditionalSections
 * @ctxt:  an XML parser context
 *
 * [61] conditionalSect ::= includeSect | ignoreSect
 * [62] includeSect ::= '<![' S? 'INCLUDE' S? '[' extSubsetDecl ']]>'
 * [63] ignoreSect ::= '<![' S? 'IGNORE' S? '[' ignoreSectContents* ']]>'
 * [64] ignoreSectContents ::= Ignore ('<![' ignoreSectContents ']]>' Ignore)*
 * [65] Ignore ::= Char* - (Char* ('<![' | ']]>') Char*)
 */

static void xmlParseConditionalSections(xmlParserCtxt * ctxt) 
{
	int id = ctxt->input->id;
	SKIP(3);
	SKIP_BLANKS;
	if(CMP7(CUR_PTR, 'I', 'N', 'C', 'L', 'U', 'D', 'E')) {
		SKIP(7);
		SKIP_BLANKS;
		if(RAW != '[') {
			xmlFatalErr(ctxt, XML_ERR_CONDSEC_INVALID, 0);
		}
		else {
			if(ctxt->input->id != id) {
				xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY, "All markup of the conditional section is not in the same entity\n", 0, 0);
			}
			NEXT;
		}
		if(xmlParserDebugEntities) {
			if(ctxt->input && (ctxt->input->filename))
				xmlGenericError(0, "%s(%d): ", ctxt->input->filename, ctxt->input->line);
			xmlGenericError(0, "Entering INCLUDE Conditional Section\n");
		}
		while(((RAW != 0) && ((RAW != ']') || (NXT(1) != ']') || (NXT(2) != '>'))) && (ctxt->instate != XML_PARSER_EOF)) {
			const xmlChar * check = CUR_PTR;
			uint cons = ctxt->input->consumed;
			if((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
				xmlParseConditionalSections(ctxt);
			}
			else if(IS_BLANK_CH(CUR)) {
				NEXT;
			}
			else if(RAW == '%') {
				xmlParsePEReference(ctxt);
			}
			else
				xmlParseMarkupDecl(ctxt);
			/*
			 * Pop-up of finished entities.
			 */
			while((RAW == 0) && (ctxt->inputNr > 1))
				xmlPopInput(ctxt);
			if((CUR_PTR == check) && (cons == ctxt->input->consumed)) {
				xmlFatalErr(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED, 0);
				break;
			}
		}
		if(xmlParserDebugEntities) {
			if(ctxt->input && (ctxt->input->filename))
				xmlGenericError(0, "%s(%d): ", ctxt->input->filename, ctxt->input->line);
			xmlGenericError(0, "Leaving INCLUDE Conditional Section\n");
		}
	}
	else if(CMP6(CUR_PTR, 'I', 'G', 'N', 'O', 'R', 'E')) {
		int state;
		xmlParserInputState instate;
		int depth = 0;
		SKIP(6);
		SKIP_BLANKS;
		if(RAW != '[') {
			xmlFatalErr(ctxt, XML_ERR_CONDSEC_INVALID, 0);
		}
		else {
			if(ctxt->input->id != id) {
				xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY, "All markup of the conditional section is not in the same entity\n", 0, 0);
			}
			NEXT;
		}
		if(xmlParserDebugEntities) {
			if(ctxt->input && (ctxt->input->filename))
				xmlGenericError(0, "%s(%d): ", ctxt->input->filename, ctxt->input->line);
			xmlGenericError(0, "Entering IGNORE Conditional Section\n");
		}
		/*
		 * Parse up to the end of the conditional section
		 * But disable SAX event generating DTD building in the meantime
		 */
		state = ctxt->disableSAX;
		instate = ctxt->instate;
		if(ctxt->recovery == 0) 
			ctxt->disableSAX = 1;
		ctxt->instate = XML_PARSER_IGNORE;
		while(((depth >= 0) && (RAW != 0)) && (ctxt->instate != XML_PARSER_EOF)) {
			if((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
				depth++;
				SKIP(3);
				continue;
			}
			if((RAW == ']') && (NXT(1) == ']') && (NXT(2) == '>')) {
				if(--depth >= 0) SKIP(3);
				continue;
			}
			NEXT;
			continue;
		}
		ctxt->disableSAX = state;
		ctxt->instate = instate;
		if(xmlParserDebugEntities) {
			if(ctxt->input && (ctxt->input->filename))
				xmlGenericError(0, "%s(%d): ", ctxt->input->filename, ctxt->input->line);
			xmlGenericError(0, "Leaving IGNORE Conditional Section\n");
		}
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_CONDSEC_INVALID_KEYWORD, 0);
	}
	if(RAW == 0)
		SHRINK;
	if(RAW == 0) {
		xmlFatalErr(ctxt, XML_ERR_CONDSEC_NOT_FINISHED, 0);
	}
	else {
		if(ctxt->input->id != id) {
			xmlValidityError(ctxt, XML_ERR_ENTITY_BOUNDARY, "All markup of the conditional section is not in the same entity\n", 0, 0);
		}
		SKIP(3);
	}
}
/**
 * xmlParseMarkupDecl:
 * @ctxt:  an XML parser context
 *
 * parse Markup declarations
 *
 * [29] markupdecl ::= elementdecl | AttlistDecl | EntityDecl | NotationDecl | PI | Comment
 *
 * [VC: Proper Declaration/PE Nesting]
 * Parameter-entity replacement text must be properly nested with
 * markup declarations. That is to say, if either the first character
 * or the last character of a markup declaration (markupdecl above) is
 * contained in the replacement text for a parameter-entity reference,
 * both must be contained in the same replacement text.
 *
 * [WFC: PEs in Internal Subset]
 * In the internal DTD subset, parameter-entity references can occur
 * only where markup declarations can occur, not within markup declarations.
 * (This does not apply to references that occur in external parameter entities or to the external subset.)
 */
void xmlParseMarkupDecl(xmlParserCtxt * ctxt)
{
	GROW;
	if(CUR == '<') {
		if(NXT(1) == '!') {
			switch(NXT(2)) {
				case 'E':
				    if(NXT(3) == 'L')
					    xmlParseElementDecl(ctxt);
				    else if(NXT(3) == 'N')
					    xmlParseEntityDecl(ctxt);
				    break;
				case 'A':
				    xmlParseAttributeListDecl(ctxt);
				    break;
				case 'N':
				    xmlParseNotationDecl(ctxt);
				    break;
				case '-':
				    xmlParseComment(ctxt);
				    break;
				default: // there is an error but it will be detected later 
				    break;
			}
		}
		else if(NXT(1) == '?') {
			xmlParsePI(ctxt);
		}
	}
	/*
	 * This is only for internal subset. On external entities,
	 * the replacement is done before parsing stage
	 */
	if((ctxt->external == 0) && (ctxt->inputNr == 1))
		xmlParsePEReference(ctxt);
	/*
	 * Conditional sections are allowed from entities included
	 * by PE References in the internal subset.
	 */
	if((ctxt->external == 0) && (ctxt->inputNr > 1)) {
		if((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
			xmlParseConditionalSections(ctxt);
		}
	}
	ctxt->instate = XML_PARSER_DTD;
}
/**
 * xmlParseTextDecl:
 * @ctxt:  an XML parser context
 *
 * parse an XML declaration header for external entities
 *
 * [77] TextDecl ::= '<?xml' VersionInfo? EncodingDecl S? '?>'
 */
void xmlParseTextDecl(xmlParserCtxt * ctxt)
{
	xmlChar * version;
	const xmlChar * encoding;
	/*
	 * We know that '<?xml' is here.
	 */
	if((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
		SKIP(5);
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_STARTED, 0);
		return;
	}
	if(!IS_BLANK_CH(CUR)) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space needed after '<?xml'\n");
	}
	SKIP_BLANKS;
	/*
	 * We may have the VersionInfo here.
	 */
	version = xmlParseVersionInfo(ctxt);
	if(!version)
		version = xmlCharStrdup(XML_DEFAULT_VERSION);
	else {
		if(!IS_BLANK_CH(CUR)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Space needed here\n");
		}
	}
	ctxt->input->version = version;
	/*
	 * We must have the encoding declaration
	 */
	encoding = xmlParseEncodingDecl(ctxt);
	if(ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
		/*
		 * The XML REC instructs us to stop parsing right here
		 */
		return;
	}
	if(!encoding && (ctxt->errNo == XML_ERR_OK)) {
		xmlFatalErrMsg(ctxt, XML_ERR_MISSING_ENCODING, "Missing encoding in text declaration\n");
	}
	SKIP_BLANKS;
	if((RAW == '?') && (NXT(1) == '>')) {
		SKIP(2);
	}
	else if(RAW == '>') {
		/* Deprecated old WD ... */
		xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, 0);
		NEXT;
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, 0);
		MOVETO_ENDTAG(CUR_PTR);
		NEXT;
	}
}
/**
 * xmlParseExternalSubset:
 * @ctxt:  an XML parser context
 * @ExternalID: the external identifier
 * @SystemID: the system identifier (or URL)
 *
 * parse Markup declarations from an external subset
 *
 * [30] extSubset ::= textDecl? extSubsetDecl
 *
 * [31] extSubsetDecl ::= (markupdecl | conditionalSect | PEReference | S) *
 */
void xmlParseExternalSubset(xmlParserCtxt * ctxt, const xmlChar * ExternalID, const xmlChar * SystemID)
{
	xmlDetectSAX2(ctxt);
	GROW;
	if(!ctxt->encoding && (ctxt->input->end - ctxt->input->cur >= 4)) {
		xmlChar start[4];
		xmlCharEncoding enc;
		start[0] = RAW;
		start[1] = NXT(1);
		start[2] = NXT(2);
		start[3] = NXT(3);
		enc = xmlDetectCharEncoding(start, 4);
		if(enc != XML_CHAR_ENCODING_NONE)
			xmlSwitchEncoding(ctxt, enc);
	}
	if(CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) {
		xmlParseTextDecl(ctxt);
		if(ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
			/*
			 * The XML REC instructs us to stop parsing right here
			 */
			ctxt->instate = XML_PARSER_EOF;
			return;
		}
	}
	if(ctxt->myDoc == NULL) {
		ctxt->myDoc = xmlNewDoc(BAD_CAST "1.0");
		if(ctxt->myDoc == NULL) {
			xmlErrMemory(ctxt, "New Doc failed");
			return;
		}
		ctxt->myDoc->properties = XML_DOC_INTERNAL;
	}
	if(ctxt->myDoc && (ctxt->myDoc->intSubset == NULL))
		xmlCreateIntSubset(ctxt->myDoc, NULL, ExternalID, SystemID);
	ctxt->instate = XML_PARSER_DTD;
	ctxt->external = 1;
	while(((RAW == '<') && (NXT(1) == '?')) || ((RAW == '<') && (NXT(1) == '!')) || (RAW == '%') || IS_BLANK_CH(CUR)) {
		const xmlChar * check = CUR_PTR;
		uint cons = ctxt->input->consumed;
		GROW;
		if((RAW == '<') && (NXT(1) == '!') && (NXT(2) == '[')) {
			xmlParseConditionalSections(ctxt);
		}
		else if(IS_BLANK_CH(CUR)) {
			NEXT;
		}
		else if(RAW == '%') {
			xmlParsePEReference(ctxt);
		}
		else
			xmlParseMarkupDecl(ctxt);
		/*
		 * Pop-up of finished entities.
		 */
		while((RAW == 0) && (ctxt->inputNr > 1))
			xmlPopInput(ctxt);
		if((CUR_PTR == check) && (cons == ctxt->input->consumed)) {
			xmlFatalErr(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED, 0);
			break;
		}
	}
	if(RAW != 0) {
		xmlFatalErr(ctxt, XML_ERR_EXT_SUBSET_NOT_FINISHED, 0);
	}
}

/**
 * xmlParseReference:
 * @ctxt:  an XML parser context
 *
 * parse and handle entity references in content, depending on the SAX
 * interface, this may end-up in a call to character() if this is a
 * CharRef, a predefined entity, if there is no reference() callback.
 * or if the parser was asked to switch to that mode.
 *
 * [67] Reference ::= EntityRef | CharRef
 */
void xmlParseReference(xmlParserCtxt * ctxt)
{
	xmlEntity * ent;
	xmlChar * val;
	int was_checked;
	xmlNode * list = NULL;
	xmlParserErrors ret = XML_ERR_OK;
	if(RAW != '&')
		return;
	// 
	// Simple case of a CharRef
	// 
	if(NXT(1) == '#') {
		int i = 0;
		xmlChar out[10];
		int hex = NXT(2);
		int value = xmlParseCharRef(ctxt);
		if(value) {
			if(ctxt->charset != XML_CHAR_ENCODING_UTF8) {
				// 
				// So we are using non-UTF-8 buffers. 
				// Check that the char fit on 8bits, if not generate a CharRef.
				// 
				if(value <= 0xFF) {
					out[0] = value;
					out[1] = 0;
					if(ctxt->sax && ctxt->sax->characters && !ctxt->disableSAX)
						ctxt->sax->characters(ctxt->userData, out, 1);
				}
				else {
					if((hex == 'x') || (hex == 'X'))
						snprintf((char*)out, sizeof(out), "#x%X", value);
					else
						snprintf((char*)out, sizeof(out), "#%d", value);
					if(ctxt->sax && ctxt->sax->reference && !ctxt->disableSAX)
						ctxt->sax->reference(ctxt->userData, out);
				}
			}
			else {
				// 
				// Just encode the value in UTF-8
				// 
				COPY_BUF(0, out, i, value);
				out[i] = 0;
				if(ctxt->sax && ctxt->sax->characters && !ctxt->disableSAX)
					ctxt->sax->characters(ctxt->userData, out, i);
			}
		}
		return;
	}
	else {
		// 
		// We are seeing an entity reference
		// 
		ent = xmlParseEntityRef(ctxt);
		if(!ent || !ctxt->wellFormed)
			return;
		was_checked = ent->checked;
		// special case of predefined entities 
		if(!ent->name || ent->etype == XML_INTERNAL_PREDEFINED_ENTITY) {
			val = ent->content;
			if(val) {
				// inline the entity.
				if(ctxt->sax && ctxt->sax->characters && !ctxt->disableSAX)
					ctxt->sax->characters(ctxt->userData, val, sstrlen(val));
			}
			return;
		}
		// 
		// The first reference to the entity trigger a parsing phase
		// where the ent->children is filled with the result from the parsing.
		// Note: external parsed entities will not be loaded, it is not
		// required for a non-validating parser, unless the parsing option
		// of validating, or substituting entities were given. Doing so is
		// far more secure as the parser will only process data coming from the document entity by default.
		// 
		if(!ent->checked && ((ent->etype != XML_EXTERNAL_GENERAL_PARSED_ENTITY) || (ctxt->options & (XML_PARSE_NOENT | XML_PARSE_DTDVALID)))) {
			ulong oldnbent = ctxt->nbentities;
			// 
			// This is a bit hackish but this seems the best
			// way to make sure both SAX and DOM entity support behaves okay.
			// 
			void * user_data = (ctxt->userData == ctxt) ? NULL : ctxt->userData;
			// 
			// Check that this entity is well formed 
			// 4.3.2: An internal general parsed entity is well-formed
			// if its replacement text matches the production labeled content.
			// 
			if(ent->etype == XML_INTERNAL_GENERAL_ENTITY) {
				ctxt->depth++;
				ret = xmlParseBalancedChunkMemoryInternal(ctxt, ent->content, user_data, &list);
				ctxt->depth--;
			}
			else if(ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY) {
				ctxt->depth++;
				ret = xmlParseExternalEntityPrivate(ctxt->myDoc, ctxt, ctxt->sax, user_data, ctxt->depth, ent->URI, ent->ExternalID, &list);
				ctxt->depth--;
			}
			else {
				ret = XML_ERR_ENTITY_PE_INTERNAL;
				xmlErrMsgStr(ctxt, XML_ERR_INTERNAL_ERROR, "invalid entity type found\n", 0);
			}
			// 
			// Store the number of entities needing parsing for this entity content and do checkings
			// 
			ent->checked = (ctxt->nbentities - oldnbent + 1) * 2;
			if(ent->content && (xmlStrchr(ent->content, '<')))
				ent->checked |= 1;
			if(ret == XML_ERR_ENTITY_LOOP) {
				xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, 0);
				xmlFreeNodeList(list);
				return;
			}
			if(xmlParserEntityCheck(ctxt, 0, ent, 0)) {
				xmlFreeNodeList(list);
				return;
			}
			if((ret == XML_ERR_OK) && list) {
				if(oneof2(ent->etype, XML_INTERNAL_GENERAL_ENTITY, XML_EXTERNAL_GENERAL_PARSED_ENTITY) && !ent->children) {
					ent->children = list;
					if(ctxt->replaceEntities) {
						// 
						// Prune it directly in the generated document except for single text nodes.
						// 
						if(((list->type == XML_TEXT_NODE) && !list->next) || (ctxt->parseMode == XML_PARSE_READER)) {
							list->parent = (xmlNode *)ent;
							list = NULL;
							ent->owner = 1;
						}
						else {
							ent->owner = 0;
							for(; list; list = list->next) {
								list->parent = (xmlNode *)ctxt->P_Node;
								list->doc = ctxt->myDoc;
								if(list->next == NULL)
									ent->last = list;
							}
							list = ent->children;
#ifdef LIBXML_LEGACY_ENABLED
							if(ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)
								xmlAddEntityReference(ent, list, 0);
#endif
						}
					}
					else {
						ent->owner = 1;
						for(; list; list = list->next) {
							list->parent = (xmlNode *)ent;
							xmlSetTreeDoc(list, ent->doc);
							if(list->next == NULL)
								ent->last = list;
						}
					}
				}
				else {
					xmlFreeNodeList(list);
					list = NULL;
				}
			}
			else if(!oneof2(ret, XML_ERR_OK, XML_WAR_UNDECLARED_ENTITY)) {
				xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY, "Entity '%s' failed to parse\n", ent->name);
				xmlParserEntityCheck(ctxt, 0, ent, 0);
			}
			else {
				xmlFreeNodeList(list);
				list = NULL;
			}
			SETIFZ(ent->checked, 2);
		}
		else if(ent->checked != 1) {
			ctxt->nbentities += ent->checked / 2;
		}
		// 
		// Now that the entity content has been gathered
		// provide it to the application, this can take different forms based on the parsing modes.
		// 
		if(!ent->children) {
			// 
			// Probably running in SAX mode and the callbacks don't
			// build the entity content. So unless we already went
			// though parsing for first checking go though the entity
			// content to generate callbacks associated to the entity
			// 
			if(was_checked) {
				// 
				// This is a bit hackish but this seems the best
				// way to make sure both SAX and DOM entity support behaves okay.
				// 
				void * user_data = (ctxt->userData == ctxt) ? 0 : ctxt->userData;
				if(ent->etype == XML_INTERNAL_GENERAL_ENTITY) {
					ctxt->depth++;
					ret = xmlParseBalancedChunkMemoryInternal(ctxt, ent->content, user_data, 0);
					ctxt->depth--;
				}
				else if(ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY) {
					ctxt->depth++;
					ret = xmlParseExternalEntityPrivate(ctxt->myDoc, ctxt, ctxt->sax, user_data, ctxt->depth, ent->URI, ent->ExternalID, 0);
					ctxt->depth--;
				}
				else {
					ret = XML_ERR_ENTITY_PE_INTERNAL;
					xmlErrMsgStr(ctxt, XML_ERR_INTERNAL_ERROR, "invalid entity type found\n", 0);
				}
				if(ret == XML_ERR_ENTITY_LOOP) {
					xmlFatalErr(ctxt, XML_ERR_ENTITY_LOOP, 0);
					return;
				}
			}
			if(ctxt->sax && ctxt->sax->reference && !ctxt->replaceEntities && !ctxt->disableSAX) {
				// 
				// Entity reference callback comes second, it's somewhat
				// superfluous but a compatibility to historical behaviour
				// 
				ctxt->sax->reference(ctxt->userData, ent->name);
			}
		}
		// 
		// If we didn't get any children for the entity being built
		// 
		else if(ctxt->sax && ctxt->sax->reference && !ctxt->replaceEntities && !ctxt->disableSAX) {
			ctxt->sax->reference(ctxt->userData, ent->name); // Create a node.
		}
		else if(ctxt->replaceEntities || !ent->children) {
			// 
			// There is a problem on the handling of _private for entities (bug 155816): Should we copy the content of the field from
			// the entity (possibly overwriting some value set by the user when a copy is created), should we leave it alone, or should
			// we try to take care of different situations?  The problem is exacerbated by the usage of this field by the xmlReader.
			// To fix this bug, we look at _private on the created node and, if it's NULL, we copy in whatever was in the entity.
			// If it's not NULL we leave it alone.  This is somewhat of a hack - maybe we should have further tests to determine what to do.
			// 
			if(ctxt->P_Node && ent->children) {
				// 
				// Seems we are generating the DOM content, do
				// a simple tree copy for all references except the first
				// In the first occurrence list contains the replacement.
				// 
				if((!list && !ent->owner) || (ctxt->parseMode == XML_PARSE_READER)) {
					xmlNode * nw = NULL;
					xmlNode * firstChild = NULL;
					// 
					// We are copying here, make sure there is no abuse
					// 
					ctxt->sizeentcopy += ent->length + 5;
					if(xmlParserEntityCheck(ctxt, 0, ent, ctxt->sizeentcopy))
						return;
					// 
					// when operating on a reader, the entities definitions
					// are always owning the entities subtree.
					//   if(ctxt->parseMode == XML_PARSE_READER) ent->owner = 1;
					// 
					for(xmlNode * cur = ent->children; cur; cur = cur->next) {
						nw = xmlDocCopyNode(cur, ctxt->myDoc, 1);
						if(nw) {
							SETIFZ(nw->_private, cur->_private);
							SETIFZ(firstChild, nw);
							nw = xmlAddChild(ctxt->P_Node, nw);
						}
						if(cur == ent->last) {
							// needed to detect some strange empty node cases in the reader tests
							if(ctxt->parseMode == XML_PARSE_READER && nw && nw->type == XML_ELEMENT_NODE && !nw->children)
								nw->extra = 1;
							break;
						}
					}
#ifdef LIBXML_LEGACY_ENABLED
					if(ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)
						xmlAddEntityReference(ent, firstChild, nw);
#endif
				}
				else if(!list || ctxt->inputNr > 0) {
					xmlNode * nw = NULL;
					xmlNode * cur;
					xmlNode * next;
					xmlNode * last;
					xmlNode * firstChild = NULL;
					// 
					// We are copying here, make sure there is no abuse
					// 
					ctxt->sizeentcopy += ent->length + 5;
					if(xmlParserEntityCheck(ctxt, 0, ent, ctxt->sizeentcopy))
						return;
					// 
					// Copy the entity child list and make it the new
					// entity child list. The goal is to make sure any
					// ID or REF referenced will be the one from the
					// document content and not the entity copy.
					// 
					cur = ent->children;
					ent->children = NULL;
					last = ent->last;
					ent->last = NULL;
					while(cur) {
						next = cur->next;
						cur->next = NULL;
						cur->parent = NULL;
						nw = xmlDocCopyNode(cur, ctxt->myDoc, 1);
						if(nw) {
							SETIFZ(nw->_private, cur->_private);
							SETIFZ(firstChild, cur);
							xmlAddChild((xmlNode *)ent, nw);
							xmlAddChild(ctxt->P_Node, cur);
						}
						if(cur == last)
							break;
						cur = next;
					}
					SETIFZ(ent->owner, 1);
#ifdef LIBXML_LEGACY_ENABLED
					if(ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)
						xmlAddEntityReference(ent, firstChild, nw);
#endif
				}
				else {
					// 
					// the name change is to avoid coalescing of the
					// node with a possible previous text one which
					// would make ent->children a dangling pointer
					// 
					const xmlChar * nbktext = xmlDictLookupSL(ctxt->dict, BAD_CAST "nbktext");
					if(ent->children->type == XML_TEXT_NODE)
						ent->children->name = nbktext;
					if((ent->last != ent->children) && (ent->last->type == XML_TEXT_NODE))
						ent->last->name = nbktext;
					xmlAddChildList(ctxt->P_Node, ent->children);
				}
				// 
				// This is to avoid a nasty side effect, see characters() in SAX.c
				// 
				ctxt->nodemem = 0;
				ctxt->nodelen = 0;
				return;
			}
		}
	}
}
/**
 * xmlParseEntityRef:
 * @ctxt:  an XML parser context
 *
 * parse ENTITY references declarations
 *
 * [68] EntityRef ::= '&' Name ';'
 *
 * [WFC: Entity Declared]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", the Name given in the entity reference
 * must match that in an entity declaration, except that well-formed
 * documents need not declare any of the following entities: amp, lt,
 * gt, apos, quot.  The declaration of a parameter entity must precede
 * any reference to it.  Similarly, the declaration of a general entity
 * must precede any reference to it which appears in a default value in an
 * attribute-list declaration. Note that if entities are declared in the
 * external subset or in external parameter entities, a non-validating
 * processor is not obligated to read and process their declarations;
 * for such documents, the rule that an entity must be declared is a
 * well-formedness constraint only if standalone='yes'.
 *
 * [WFC: Parsed Entity]
 * An entity reference must not contain the name of an unparsed entity
 *
 * Returns the xmlEntityPtr if found, or NULL otherwise.
 */
xmlEntity * xmlParseEntityRef(xmlParserCtxt * ctxt)
{
	const xmlChar * name;
	xmlEntity * ent = NULL;
	GROW;
	if(ctxt->instate == XML_PARSER_EOF)
		return 0;
	if(RAW != '&')
		return 0;
	NEXT;
	name = xmlParseName(ctxt);
	if(!name) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "xmlParseEntityRef: no name\n");
		return 0;
	}
	if(RAW != ';') {
		xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, 0);
		return 0;
	}
	NEXT;
	/*
	 * Predefined entities override any extra definition
	 */
	if((ctxt->options & XML_PARSE_OLDSAX) == 0) {
		ent = xmlGetPredefinedEntity(name);
		if(ent != NULL)
			return(ent);
	}
	/*
	 * Increase the number of entity references parsed
	 */
	ctxt->nbentities++;
	/*
	 * Ask first SAX for entity resolution, otherwise try the
	 * entities which may have stored in the parser context.
	 */
	if(ctxt->sax) {
		if(ctxt->sax->getEntity)
			ent = ctxt->sax->getEntity(ctxt->userData, name);
		if((ctxt->wellFormed == 1 ) && !ent && (ctxt->options & XML_PARSE_OLDSAX))
			ent = xmlGetPredefinedEntity(name);
		if((ctxt->wellFormed == 1 ) && !ent && (ctxt->userData==ctxt))
			ent = xmlSAX2GetEntity(ctxt, name);
	}
	if(ctxt->instate == XML_PARSER_EOF)
		return 0;
	/*
	 * [WFC: Entity Declared]
	 * In a document without any DTD, a document with only an
	 * internal DTD subset which contains no parameter entity
	 * references, or a document with "standalone='yes'", the
	 * Name given in the entity reference must match that in an
	 * entity declaration, except that well-formed documents
	 * need not declare any of the following entities: amp, lt,
	 * gt, apos, quot.
	 * The declaration of a parameter entity must precede any
	 * reference to it.
	 * Similarly, the declaration of a general entity must
	 * precede any reference to it which appears in a default
	 * value in an attribute-list declaration. Note that if
	 * entities are declared in the external subset or in
	 * external parameter entities, a non-validating processor
	 * is not obligated to read and process their declarations;
	 * for such documents, the rule that an entity must be
	 * declared is a well-formedness constraint only if
	 * standalone='yes'.
	 */
	if(ent == NULL) {
		if((ctxt->standalone == 1) || ((ctxt->hasExternalSubset == 0) && (ctxt->hasPErefs == 0))) {
			xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY, "Entity '%s' not defined\n", name);
		}
		else {
			xmlErrMsgStr(ctxt, XML_WAR_UNDECLARED_ENTITY, "Entity '%s' not defined\n", name);
			if((ctxt->inSubset == 0) && (ctxt->sax != NULL) && (ctxt->sax->reference != NULL)) {
				ctxt->sax->reference(ctxt->userData, name);
			}
		}
		xmlParserEntityCheck(ctxt, 0, ent, 0);
		ctxt->valid = 0;
	}
	/*
	 * [WFC: Parsed Entity]
	 * An entity reference must not contain the name of an
	 * unparsed entity
	 */
	else if(ent->etype == XML_EXTERNAL_GENERAL_UNPARSED_ENTITY) {
		xmlFatalErrMsgStr(ctxt, XML_ERR_UNPARSED_ENTITY, "Entity reference to unparsed entity %s\n", name);
	}
	/*
	 * [WFC: No External Entity References]
	 * Attribute values cannot contain direct or indirect
	 * entity references to external entities.
	 */
	else if((ctxt->instate == XML_PARSER_ATTRIBUTE_VALUE) && (ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)) {
		xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_IS_EXTERNAL, "Attribute references external entity '%s'\n", name);
	}
	/*
	 * [WFC: No < in Attribute Values]
	 * The replacement text of any entity referred to directly or
	 * indirectly in an attribute value (other than "&lt;") must
	 * not contain a <.
	 */
	else if((ctxt->instate == XML_PARSER_ATTRIBUTE_VALUE) && (ent != NULL) && (ent->etype != XML_INTERNAL_PREDEFINED_ENTITY)) {
		if(((ent->checked & 1) || (ent->checked == 0)) && (ent->content != NULL) && (xmlStrchr(ent->content, '<'))) {
			xmlFatalErrMsgStr(ctxt, XML_ERR_LT_IN_ATTRIBUTE, "'<' in entity '%s' is not allowed in attributes values\n", name);
		}
	}

	/*
	 * Internal check, no parameter entities here ...
	 */
	else {
		switch(ent->etype) {
			case XML_INTERNAL_PARAMETER_ENTITY:
			case XML_EXTERNAL_PARAMETER_ENTITY:
			    xmlFatalErrMsgStr(ctxt, XML_ERR_ENTITY_IS_PARAMETER, "Attempt to reference the parameter entity '%s'\n", name);
			    break;
			default:
			    break;
		}
	}
	/*
	 * [WFC: No Recursion]
	 * A parsed entity must not contain a recursive reference
	 * to itself, either directly or indirectly.
	 * Done somewhere else
	 */
	return(ent);
}

/**
 * @ctxt:  an XML parser context
 * @str:  a pointer to an index in the string
 *
 * parse ENTITY references declarations, but this version parses it from
 * a string value.
 *
 * [68] EntityRef ::= '&' Name ';'
 *
 * [WFC: Entity Declared]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", the Name given in the entity reference
 * must match that in an entity declaration, except that well-formed
 * documents need not declare any of the following entities: amp, lt,
 * gt, apos, quot.  The declaration of a parameter entity must precede
 * any reference to it.  Similarly, the declaration of a general entity
 * must precede any reference to it which appears in a default value in an
 * attribute-list declaration. Note that if entities are declared in the
 * external subset or in external parameter entities, a non-validating
 * processor is not obligated to read and process their declarations;
 * for such documents, the rule that an entity must be declared is a
 * well-formedness constraint only if standalone='yes'.
 *
 * [WFC: Parsed Entity]
 * An entity reference must not contain the name of an unparsed entity
 *
 * Returns the xmlEntityPtr if found, or NULL otherwise. The str pointer
 * is updated to the current location in the string.
 */
static xmlEntity * xmlParseStringEntityRef(xmlParserCtxt * pCtxt, const xmlChar ** ppStr)
{
	xmlEntity * p_ent = NULL;
	if(ppStr && *ppStr) {
		const xmlChar * ptr = *ppStr;
		xmlChar cur = *ptr;
		if(cur == '&') {
			ptr++;
			xmlChar * name = xmlParseStringName(pCtxt, &ptr);
			if(!name) {
				xmlFatalErrMsg(pCtxt, XML_ERR_NAME_REQUIRED, "xmlParseStringEntityRef: no name\n");
				*ppStr = ptr;
			}
			else if(*ptr != ';') {
				xmlFatalErr(pCtxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, 0);
				SAlloc::F(name);
				*ppStr = ptr;
			}
			else {
				ptr++;
				// Predefined entities override any extra definition
				if(!(pCtxt->options & XML_PARSE_OLDSAX)) {
					p_ent = xmlGetPredefinedEntity(name);
					if(p_ent) {
						SAlloc::F(name);
						*ppStr = ptr;
						return p_ent;
					}
				}
				// Increate the number of entity references parsed
				pCtxt->nbentities++;
				// Ask first SAX for entity resolution, otherwise try the entities which may have stored in the parser context.
				if(pCtxt->sax) {
					if(pCtxt->sax->getEntity)
						p_ent = pCtxt->sax->getEntity(pCtxt->userData, name);
					if(!p_ent && (pCtxt->options & XML_PARSE_OLDSAX))
						p_ent = xmlGetPredefinedEntity(name);
					if(!p_ent && (pCtxt->userData==pCtxt))
						p_ent = xmlSAX2GetEntity(pCtxt, name);
				}
				if(pCtxt->instate == XML_PARSER_EOF) {
					SAlloc::F(name);
					return 0;
				}
				/*
				 * [WFC: Entity Declared]
				 * In a document without any DTD, a document with only an internal DTD subset which contains no parameter entity
				 * references, or a document with "standalone='yes'", the Name given in the entity reference must match that in an
				 * entity declaration, except that well-formed documents need not declare any of the following entities: amp, lt, gt, apos, quot.
				 * The declaration of a parameter entity must precede any reference to it.
				 * Similarly, the declaration of a general entity must precede any reference to it which appears in a default
				 * value in an attribute-list declaration. Note that if entities are declared in the external subset or in
				 * external parameter entities, a non-validating processor is not obligated to read and process their declarations;
				 * for such documents, the rule that an entity must be declared is a well-formedness constraint only if standalone='yes'.
				 */
				if(!p_ent) {
					if((pCtxt->standalone == 1) || ((pCtxt->hasExternalSubset == 0) && (pCtxt->hasPErefs == 0)))
						xmlFatalErrMsgStr(pCtxt, XML_ERR_UNDECLARED_ENTITY, "Entity '%s' not defined\n", name);
					else
						xmlErrMsgStr(pCtxt, XML_WAR_UNDECLARED_ENTITY, "Entity '%s' not defined\n", name);
					xmlParserEntityCheck(pCtxt, 0, p_ent, 0);
					/* @todo ? check regressions ctxt->valid = 0; */
				}
				/*
				 * [WFC: Parsed Entity]
				 * An entity reference must not contain the name of an unparsed entity
				 */
				else if(p_ent->etype == XML_EXTERNAL_GENERAL_UNPARSED_ENTITY) {
					xmlFatalErrMsgStr(pCtxt, XML_ERR_UNPARSED_ENTITY, "Entity reference to unparsed entity %s\n", name);
				}
				/*
				 * [WFC: No External Entity References]
				 * Attribute values cannot contain direct or indirect entity references to external entities.
				 */
				else if((pCtxt->instate == XML_PARSER_ATTRIBUTE_VALUE) && (p_ent->etype == XML_EXTERNAL_GENERAL_PARSED_ENTITY)) {
					xmlFatalErrMsgStr(pCtxt, XML_ERR_ENTITY_IS_EXTERNAL, "Attribute references external entity '%s'\n", name);
				}
				/*
				 * [WFC: No < in Attribute Values]
				 * The replacement text of any entity referred to directly or
				 * indirectly in an attribute value (other than "&lt;") must not contain a <.
				 */
				else if((pCtxt->instate == XML_PARSER_ATTRIBUTE_VALUE) && p_ent && p_ent->content && (p_ent->etype != XML_INTERNAL_PREDEFINED_ENTITY) && (xmlStrchr(p_ent->content, '<'))) {
					xmlFatalErrMsgStr(pCtxt, XML_ERR_LT_IN_ATTRIBUTE, "'<' in entity '%s' is not allowed in attributes values\n", name);
				}
				/*
				 * Internal check, no parameter entities here ...
				 */
				else {
					switch(p_ent->etype) {
						case XML_INTERNAL_PARAMETER_ENTITY:
						case XML_EXTERNAL_PARAMETER_ENTITY:
							xmlFatalErrMsgStr(pCtxt, XML_ERR_ENTITY_IS_PARAMETER, "Attempt to reference the parameter entity '%s'\n", name);
							break;
						default:
							break;
					}
				}
				/*
				 * [WFC: No Recursion]
				 * A parsed entity must not contain a recursive reference to itself, either directly or indirectly.
				 * Done somewhere else
				 */
				SAlloc::F(name);
				*ppStr = ptr;
			}
		}
	}
	return p_ent;
}

/**
 * xmlParsePEReference:
 * @ctxt:  an XML parser context
 *
 * parse PEReference declarations
 * The entity content is handled directly by pushing it's content as
 * a new input stream.
 *
 * [69] PEReference ::= '%' Name ';'
 *
 * [WFC: No Recursion]
 * A parsed entity must not contain a recursive
 * reference to itself, either directly or indirectly.
 *
 * [WFC: Entity Declared]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", ...  ... The declaration of a parameter
 * entity must precede any reference to it...
 *
 * [VC: Entity Declared]
 * In a document with an external subset or external parameter entities
 * with "standalone='no'", ...  ... The declaration of a parameter entity
 * must precede any reference to it...
 *
 * [WFC: In DTD]
 * Parameter-entity references may only appear in the DTD.
 * NOTE: misleading but this is handled.
 */
void xmlParsePEReference(xmlParserCtxt * ctxt)
{
	const xmlChar * name;
	xmlEntity * entity = NULL;
	xmlParserInputPtr input;
	if(RAW != '%')
		return;
	NEXT;
	name = xmlParseName(ctxt);
	if(!name) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "xmlParsePEReference: no name\n");
		return;
	}
	if(RAW != ';') {
		xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, 0);
		return;
	}
	NEXT;
	/*
	 * Increate the number of entity references parsed
	 */
	ctxt->nbentities++;
	/*
	 * Request the entity from SAX
	 */
	if(ctxt->sax && (ctxt->sax->getParameterEntity != NULL))
		entity = ctxt->sax->getParameterEntity(ctxt->userData, name);
	if(ctxt->instate == XML_PARSER_EOF)
		return;
	if(entity == NULL) {
		/*
		 * [WFC: Entity Declared]
		 * In a document without any DTD, a document with only an
		 * internal DTD subset which contains no parameter entity
		 * references, or a document with "standalone='yes'", ...
		 * ... The declaration of a parameter entity must precede
		 * any reference to it...
		 */
		if((ctxt->standalone == 1) || ((ctxt->hasExternalSubset == 0) && (ctxt->hasPErefs == 0))) {
			xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY, "PEReference: %%%s; not found\n", name);
		}
		else {
			/*
			 * [VC: Entity Declared]
			 * In a document with an external subset or external
			 * parameter entities with "standalone='no'", ...
			 * ... The declaration of a parameter entity must
			 * precede any reference to it...
			 */
			xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY, "PEReference: %%%s; not found\n", name, 0);
			ctxt->valid = 0;
		}
		xmlParserEntityCheck(ctxt, 0, NULL, 0);
	}
	else {
		/*
		 * Internal checking in case the entity quest barfed
		 */
		if((entity->etype != XML_INTERNAL_PARAMETER_ENTITY) && (entity->etype != XML_EXTERNAL_PARAMETER_ENTITY)) {
			xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY, "Internal: %%%s; is not a parameter entity\n", name, 0);
		}
		else if(ctxt->input->free != deallocblankswrapper) {
			input = xmlNewBlanksWrapperInputStream(ctxt, entity);
			if(xmlPushInput(ctxt, input) < 0)
				return;
		}
		else {
			/*
			 * @todo !!!
			 * handle the extra spaces added before and after
			 * c.f. http://www.w3.org/TR/REC-xml#as-PE
			 */
			input = xmlNewEntityInputStream(ctxt, entity);
			if(xmlPushInput(ctxt, input) < 0)
				return;
			if((entity->etype == XML_EXTERNAL_PARAMETER_ENTITY) && (CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
				xmlParseTextDecl(ctxt);
				if(ctxt->errNo ==
				    XML_ERR_UNSUPPORTED_ENCODING) {
					/*
					 * The XML REC instructs us to stop parsing
					 * right here
					 */
					ctxt->instate = XML_PARSER_EOF;
					return;
				}
			}
		}
	}
	ctxt->hasPErefs = 1;
}

/**
 * xmlLoadEntityContent:
 * @ctxt:  an XML parser context
 * @entity: an unloaded system entity
 *
 * Load the original content of the given system entity from the
 * ExternalID/SystemID given. This is to be used for Included in Literal
 * http://www.w3.org/TR/REC-xml/#inliteral processing of entities references
 *
 * Returns 0 in case of success and -1 in case of failure
 */
static int xmlLoadEntityContent(xmlParserCtxt * ctxt, xmlEntity * entity)
{
	xmlParserInputPtr input;
	xmlBuffer * buf;
	int l, c;
	int count = 0;
	if(!ctxt || !entity || ((entity->etype != XML_EXTERNAL_PARAMETER_ENTITY) && (entity->etype != XML_EXTERNAL_GENERAL_PARSED_ENTITY)) || entity->content) {
		xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "xmlLoadEntityContent parameter error");
		return -1;
	}
	if(xmlParserDebugEntities)
		xmlGenericError(0, "Reading %s entity content input\n", entity->name);
	buf = xmlBufferCreate();
	if(!buf) {
		xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "xmlLoadEntityContent parameter error");
		return -1;
	}
	input = xmlNewEntityInputStream(ctxt, entity);
	if(!input) {
		xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "xmlLoadEntityContent input error");
		xmlBufferFree(buf);
		return -1;
	}
	/*
	 * Push the entity as the current input, read char by char
	 * saving to the buffer until the end of the entity or an error
	 */
	if(xmlPushInput(ctxt, input) < 0) {
		xmlBufferFree(buf);
		return -1;
	}
	GROW;
	c = CUR_CHAR(l);
	while((ctxt->input == input) && (ctxt->input->cur < ctxt->input->end) && (IS_CHAR(c))) {
		xmlBufferAdd(buf, ctxt->input->cur, l);
		if(count++ > XML_PARSER_CHUNK_SIZE) {
			count = 0;
			GROW;
			if(ctxt->instate == XML_PARSER_EOF) {
				xmlBufferFree(buf);
				return -1;
			}
		}
		NEXTL(l);
		c = CUR_CHAR(l);
		if(c == 0) {
			count = 0;
			GROW;
			if(ctxt->instate == XML_PARSER_EOF) {
				xmlBufferFree(buf);
				return -1;
			}
			c = CUR_CHAR(l);
		}
	}

	if((ctxt->input == input) && (ctxt->input->cur >= ctxt->input->end)) {
		xmlPopInput(ctxt);
	}
	else if(!IS_CHAR(c)) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_INVALID_CHAR, "xmlLoadEntityContent: invalid char value %d\n", c);
		xmlBufferFree(buf);
		return -1;
	}
	entity->content = buf->content;
	buf->content = NULL;
	xmlBufferFree(buf);
	return 0;
}
/**
 * xmlParseStringPEReference:
 * @ctxt:  an XML parser context
 * @str:  a pointer to an index in the string
 *
 * parse PEReference declarations
 *
 * [69] PEReference ::= '%' Name ';'
 *
 * [WFC: No Recursion]
 * A parsed entity must not contain a recursive
 * reference to itself, either directly or indirectly.
 *
 * [WFC: Entity Declared]
 * In a document without any DTD, a document with only an internal DTD
 * subset which contains no parameter entity references, or a document
 * with "standalone='yes'", ...  ... The declaration of a parameter
 * entity must precede any reference to it...
 *
 * [VC: Entity Declared]
 * In a document with an external subset or external parameter entities
 * with "standalone='no'", ...  ... The declaration of a parameter entity
 * must precede any reference to it...
 *
 * [WFC: In DTD]
 * Parameter-entity references may only appear in the DTD.
 * NOTE: misleading but this is handled.
 *
 * Returns the string of the entity content.
 *         str is updated to the current value of the index
 */
static xmlEntity * xmlParseStringPEReference(xmlParserCtxt * ctxt, const xmlChar ** str)
{
	const xmlChar * ptr;
	xmlChar cur;
	xmlChar * name;
	xmlEntity * entity = NULL;
	if(!str || *str == NULL)
		return 0;
	ptr = *str;
	cur = *ptr;
	if(cur != '%')
		return 0;
	ptr++;
	name = xmlParseStringName(ctxt, &ptr);
	if(!name) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "xmlParseStringPEReference: no name\n");
		*str = ptr;
		return 0;
	}
	cur = *ptr;
	if(cur != ';') {
		xmlFatalErr(ctxt, XML_ERR_ENTITYREF_SEMICOL_MISSING, 0);
		SAlloc::F(name);
		*str = ptr;
		return 0;
	}
	ptr++;
	/*
	 * Increate the number of entity references parsed
	 */
	ctxt->nbentities++;
	/*
	 * Request the entity from SAX
	 */
	if(ctxt->sax && ctxt->sax->getParameterEntity)
		entity = ctxt->sax->getParameterEntity(ctxt->userData, name);
	if(ctxt->instate == XML_PARSER_EOF) {
		SAlloc::F(name);
		return 0;
	}
	if(!entity) {
		/*
		 * [WFC: Entity Declared]
		 * In a document without any DTD, a document with only an
		 * internal DTD subset which contains no parameter entity
		 * references, or a document with "standalone='yes'", ...
		 * ... The declaration of a parameter entity must precede
		 * any reference to it...
		 */
		if((ctxt->standalone == 1) || ((ctxt->hasExternalSubset == 0) && (ctxt->hasPErefs == 0))) {
			xmlFatalErrMsgStr(ctxt, XML_ERR_UNDECLARED_ENTITY, "PEReference: %%%s; not found\n", name);
		}
		else {
			/*
			 * [VC: Entity Declared]
			 * In a document with an external subset or external
			 * parameter entities with "standalone='no'", ...
			 * ... The declaration of a parameter entity must
			 * precede any reference to it...
			 */
			xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY, "PEReference: %%%s; not found\n", name, 0);
			ctxt->valid = 0;
		}
		xmlParserEntityCheck(ctxt, 0, NULL, 0);
	}
	else {
		/*
		 * Internal checking in case the entity quest barfed
		 */
		if((entity->etype != XML_INTERNAL_PARAMETER_ENTITY) && (entity->etype != XML_EXTERNAL_PARAMETER_ENTITY)) {
			xmlWarningMsg(ctxt, XML_WAR_UNDECLARED_ENTITY, "%%%s; is not a parameter entity\n", name, 0);
		}
	}
	ctxt->hasPErefs = 1;
	SAlloc::F(name);
	*str = ptr;
	return(entity);
}

/**
 * xmlParseDocTypeDecl:
 * @ctxt:  an XML parser context
 *
 * parse a DOCTYPE declaration
 *
 * [28] doctypedecl ::= '<!DOCTYPE' S Name (S ExternalID)? S?
 *                      ('[' (markupdecl | PEReference | S)* ']' S?)? '>'
 *
 * [VC: Root Element Type]
 * The Name in the document type declaration must match the element
 * type of the root element.
 */

void xmlParseDocTypeDecl(xmlParserCtxt * ctxt)
{
	const xmlChar * name = NULL;
	xmlChar * ExternalID = NULL;
	xmlChar * URI = NULL;
	/*
	 * We know that '<!DOCTYPE' has been detected.
	 */
	SKIP(9);
	SKIP_BLANKS;
	/*
	 * Parse the DOCTYPE name.
	 */
	name = xmlParseName(ctxt);
	if(!name) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "xmlParseDocTypeDecl : no DOCTYPE name !\n");
	}
	ctxt->intSubName = name;
	SKIP_BLANKS;
	/*
	 * Check for SystemID and ExternalID
	 */
	URI = xmlParseExternalID(ctxt, &ExternalID, 1);
	if(URI || ExternalID) {
		ctxt->hasExternalSubset = 1;
	}
	ctxt->extSubURI = URI;
	ctxt->extSubSystem = ExternalID;
	SKIP_BLANKS;
	/*
	 * Create and update the internal subset.
	 */
	if(ctxt->sax && (ctxt->sax->internalSubset != NULL) && (!ctxt->disableSAX))
		ctxt->sax->internalSubset(ctxt->userData, name, ExternalID, URI);
	if(ctxt->instate == XML_PARSER_EOF)
		return;
	/*
	 * Is there any internal subset declarations ?
	 * they are handled separately in xmlParseInternalSubset()
	 */
	if(RAW == '[')
		return;
	/*
	 * We should be at the end of the DOCTYPE declaration.
	 */
	if(RAW != '>') {
		xmlFatalErr(ctxt, XML_ERR_DOCTYPE_NOT_FINISHED, 0);
	}
	NEXT;
}

/**
 * xmlParseInternalSubset:
 * @ctxt:  an XML parser context
 *
 * parse the internal subset declaration
 *
 * [28 end] ('[' (markupdecl | PEReference | S)* ']' S?)? '>'
 */

static void xmlParseInternalSubset(xmlParserCtxt * ctxt)
{
	/*
	 * Is there any DTD definition ?
	 */
	if(RAW == '[') {
		ctxt->instate = XML_PARSER_DTD;
		NEXT;
		/*
		 * Parse the succession of Markup declarations and
		 * PEReferences.
		 * Subsequence (markupdecl | PEReference | S)*
		 */
		while((RAW != ']') && (ctxt->instate != XML_PARSER_EOF)) {
			const xmlChar * check = CUR_PTR;
			uint cons = ctxt->input->consumed;
			SKIP_BLANKS;
			xmlParseMarkupDecl(ctxt);
			xmlParsePEReference(ctxt);
			/*
			 * Pop-up of finished entities.
			 */
			while((RAW == 0) && (ctxt->inputNr > 1))
				xmlPopInput(ctxt);
			if((CUR_PTR == check) && (cons == ctxt->input->consumed)) {
				xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "xmlParseInternalSubset: error detected in Markup declaration\n");
				break;
			}
		}
		if(RAW == ']') {
			NEXT;
			SKIP_BLANKS;
		}
	}

	/*
	 * We should be at the end of the DOCTYPE declaration.
	 */
	if(RAW != '>') {
		xmlFatalErr(ctxt, XML_ERR_DOCTYPE_NOT_FINISHED, 0);
	}
	NEXT;
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlParseAttribute:
 * @ctxt:  an XML parser context
 * @value:  a xmlChar ** used to store the value of the attribute
 *
 * parse an attribute
 *
 * [41] Attribute ::= Name Eq AttValue
 *
 * [WFC: No External Entity References]
 * Attribute values cannot contain direct or indirect entity references
 * to external entities.
 *
 * [WFC: No < in Attribute Values]
 * The replacement text of any entity referred to directly or indirectly in
 * an attribute value (other than "&lt;") must not contain a <.
 *
 * [VC: Attribute Value Type]
 * The attribute must have been declared; the value must be of the type
 * declared for it.
 *
 * [25] Eq ::= S? '=' S?
 *
 * With namespace:
 *
 * [NS 11] Attribute ::= QName Eq AttValue
 *
 * Also the case QName == xmlns:??? is handled independently as a namespace
 * definition.
 *
 * Returns the attribute name, and the value in *value.
 */
const xmlChar * xmlParseAttribute(xmlParserCtxt * ctxt, xmlChar ** value)
{
	const xmlChar * name = 0;
	xmlChar * val;
	*value = NULL;
	GROW;
	name = xmlParseName(ctxt);
	if(!name) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "error parsing attribute name\n");
	}
	else {
		/*
		* read the value
		*/
		SKIP_BLANKS;
		if(RAW == '=') {
			NEXT;
			SKIP_BLANKS;
			val = xmlParseAttValue(ctxt);
			ctxt->instate = XML_PARSER_CONTENT;
		}
		else {
			xmlFatalErrMsgStr(ctxt, XML_ERR_ATTRIBUTE_WITHOUT_VALUE, "Specification mandate value for attribute %s\n", name);
			return 0;
		}
		/*
		* Check that xml:lang conforms to the specification
		* No more registered as an error, just generate a warning now
		* since this was deprecated in XML second edition
		*/
		if(ctxt->pedantic && sstreq(name, "xml:lang")) {
			if(!xmlCheckLanguageID(val)) {
				xmlWarningMsg(ctxt, XML_WAR_LANG_VALUE, "Malformed value for xml:lang : %s\n", val, 0);
			}
		}
		/*
		* Check that xml:space conforms to the specification
		*/
		if(sstreq(name, "xml:space")) {
			if(sstreq(val, "default"))
				*(ctxt->space) = 0;
			else if(sstreq(val, "preserve"))
				*(ctxt->space) = 1;
			else {
				xmlWarningMsg(ctxt, XML_WAR_SPACE_VALUE, "Invalid value \"%s\" for xml:space : \"default\" or \"preserve\" expected\n", val, 0);
			}
		}
		*value = val;
	}
	return name;
}

/**
 * xmlParseStartTag:
 * @ctxt:  an XML parser context
 *
 * parse a start of tag either for rule element or
 * EmptyElement. In both case we don't parse the tag closing chars.
 *
 * [40] STag ::= '<' Name (S Attribute)* S? '>'
 *
 * [WFC: Unique Att Spec]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag.
 *
 * [44] EmptyElemTag ::= '<' Name (S Attribute)* S? '/>'
 *
 * [WFC: Unique Att Spec]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag.
 *
 * With namespace:
 *
 * [NS 8] STag ::= '<' QName (S Attribute)* S? '>'
 *
 * [NS 10] EmptyElement ::= '<' QName (S Attribute)* S? '/>'
 *
 * Returns the element name parsed
 */
const xmlChar * xmlParseStartTag(xmlParserCtxt * ctxt)
{
	const xmlChar * name = 0;
	const xmlChar * attname;
	xmlChar * attvalue;
	const xmlChar ** atts = ctxt->atts;
	int nbatts = 0;
	int maxatts = ctxt->maxatts;
	int i;
	if(RAW == '<') {
		NEXT1;
		name = xmlParseName(ctxt);
		if(!name) {
			xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "xmlParseStartTag: invalid element name\n");
		}
		else {
			/*
			* Now parse the attributes, it ends up with the ending
			*
			* (S Attribute)* S?
			*/
			SKIP_BLANKS;
			GROW;
			while((RAW != '>' && (RAW != '/' || NXT(1) != '>') && (IS_BYTE_CHAR(RAW))) && ctxt->instate != XML_PARSER_EOF) {
				const xmlChar * q = CUR_PTR;
				uint cons = ctxt->input->consumed;
				attname = xmlParseAttribute(ctxt, &attvalue);
				if(attname && attvalue) {
					/*
					* [WFC: Unique Att Spec]
					* No attribute name may appear more than once in the same
					* start-tag or empty-element tag.
					*/
					for(i = 0; i < nbatts; i += 2) {
						if(sstreq(atts[i], attname)) {
							xmlErrAttributeDup(ctxt, NULL, attname);
							SAlloc::F(attvalue);
							goto failed;
						}
					}
					/*
					* Add the pair to atts
					*/
					if(atts == NULL) {
						maxatts = 22; /* allow for 10 attrs by default */
						atts = (const xmlChar**)SAlloc::M(maxatts * sizeof(xmlChar *));
						if(atts == NULL) {
							xmlErrMemory(ctxt, 0);
							SAlloc::F(attvalue);
							goto failed;
						}
						ctxt->atts = atts;
						ctxt->maxatts = maxatts;
					}
					else if(nbatts + 4 > maxatts) {
						const xmlChar ** n;
						maxatts *= 2;
						n = (const xmlChar**)SAlloc::R((void*)atts, maxatts * sizeof(const xmlChar *));
						if(n == NULL) {
							xmlErrMemory(ctxt, 0);
							SAlloc::F(attvalue);
							goto failed;
						}
						atts = n;
						ctxt->atts = atts;
						ctxt->maxatts = maxatts;
					}
					atts[nbatts++] = attname;
					atts[nbatts++] = attvalue;
					atts[nbatts] = NULL;
					atts[nbatts + 1] = NULL;
				}
				else {
					SAlloc::F(attvalue);
				}
failed:
				GROW
				if((RAW == '>') || (((RAW == '/') && (NXT(1) == '>'))))
					break;
				if(!IS_BLANK_CH(RAW)) {
					xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "attributes construct error\n");
				}
				SKIP_BLANKS;
				if(cons == ctxt->input->consumed && q == CUR_PTR && !attname && !attvalue) {
					xmlFatalErrMsg(ctxt, XML_ERR_INTERNAL_ERROR, "xmlParseStartTag: problem parsing attributes\n");
					break;
				}
				SHRINK;
				GROW;
			}
			/*
			* SAX: Start of Element !
			*/
			if(ctxt->sax && ctxt->sax->startElement && !ctxt->disableSAX) {
				ctxt->sax->startElement(ctxt->userData, name, (nbatts > 0) ? atts : 0);
			}
			if(atts) { // Free only the content strings
				for(i = 1; i < nbatts; i += 2)
					SAlloc::F((xmlChar*)atts[i]);
			}
		}
	}
	return name;
}

/**
 * xmlParseEndTag1:
 * @ctxt:  an XML parser context
 * @line:  line of the start tag
 * @nsNr:  number of namespaces on the start tag
 *
 * parse an end of tag
 *
 * [42] ETag ::= '</' Name S? '>'
 *
 * With namespace
 *
 * [NS 9] ETag ::= '</' QName S? '>'
 */

static void xmlParseEndTag1(xmlParserCtxt * ctxt, int line)
{
	const xmlChar * name;
	GROW;
	if((RAW != '<') || (NXT(1) != '/')) {
		xmlFatalErrMsg(ctxt, XML_ERR_LTSLASH_REQUIRED, "xmlParseEndTag: '</' not found\n");
		return;
	}
	SKIP(2);
	name = xmlParseNameAndCompare(ctxt, ctxt->name);
	/*
	 * We should definitely be at the ending "S? '>'" part
	 */
	GROW;
	SKIP_BLANKS;
	if((!IS_BYTE_CHAR(RAW)) || (RAW != '>')) {
		xmlFatalErr(ctxt, XML_ERR_GT_REQUIRED, 0);
	}
	else
		NEXT1;
	/*
	 * [WFC: Element Type Match]
	 * The Name in an element's end-tag must match the element type in the
	 * start-tag.
	 *
	 */
	if(name != (xmlChar*)1) {
		if(!name)
			name = BAD_CAST "unparseable";
		xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NAME_MISMATCH, "Opening and ending tag mismatch: %s line %d and %s\n", ctxt->name, line, name);
	}
	/*
	 * SAX: End of Tag
	 */
	if(ctxt->sax && ctxt->sax->endElement && !ctxt->disableSAX)
		ctxt->sax->endElement(ctxt->userData, ctxt->name);
	namePop(ctxt);
	spacePop(ctxt);
}

/**
 * xmlParseEndTag:
 * @ctxt:  an XML parser context
 *
 * parse an end of tag
 *
 * [42] ETag ::= '</' Name S? '>'
 *
 * With namespace
 *
 * [NS 9] ETag ::= '</' QName S? '>'
 */

void xmlParseEndTag(xmlParserCtxt * ctxt)
{
	xmlParseEndTag1(ctxt, 0);
}

#endif /* LIBXML_SAX1_ENABLED */

/************************************************************************
*									*
*		      SAX 2 specific operations				*
*									*
************************************************************************/

/*
 * xmlGetNamespace:
 * @ctxt:  an XML parser context
 * @prefix:  the prefix to lookup
 *
 * Lookup the namespace name for the @prefix (which ca be NULL)
 * The prefix must come from the @ctxt->dict dictionnary
 *
 * Returns the namespace name or NULL if not bound
 */
static const xmlChar * FASTCALL xmlGetNamespace(xmlParserCtxt * ctxt, const xmlChar * prefix)
{
	if(prefix == ctxt->str_xml)
		return ctxt->str_xml_ns;
	else {
		for(int i = ctxt->nsNr - 2; i >= 0; i -= 2) {
			if(ctxt->nsTab[i] == prefix) {
				return (!prefix && (*ctxt->nsTab[i + 1] == 0)) ? 0 : ctxt->nsTab[i + 1];
			}
		}
		return 0;
	}
}
/**
 * xmlParseQName:
 * @ctxt:  an XML parser context
 * @prefix:  pointer to store the prefix part
 *
 * parse an XML Namespace QName
 *
 * [6]  QName  ::= (Prefix ':')? LocalPart
 * [7]  Prefix  ::= NCName
 * [8]  LocalPart  ::= NCName
 *
 * Returns the Name parsed or NULL
 */

static const xmlChar * FASTCALL xmlParseQName(xmlParserCtxt * ctxt, const xmlChar ** prefix)
{
	const xmlChar * l, * p;
	GROW;
	l = xmlParseNCName(ctxt);
	if(l == NULL) {
		if(CUR == ':') {
			l = xmlParseName(ctxt);
			if(l != NULL) {
				xmlNsErr(ctxt, XML_NS_ERR_QNAME, "Failed to parse QName '%s'\n", l, 0, 0);
				*prefix = NULL;
				return l;
			}
		}
		return 0;
	}
	if(CUR == ':') {
		NEXT;
		p = l;
		l = xmlParseNCName(ctxt);
		if(l == NULL) {
			xmlChar * tmp;
			xmlNsErr(ctxt, XML_NS_ERR_QNAME, "Failed to parse QName '%s:'\n", p, 0, 0);
			l = xmlParseNmtoken(ctxt);
			if(l == NULL)
				tmp = xmlBuildQName(BAD_CAST "", p, NULL, 0);
			else {
				tmp = xmlBuildQName(l, p, NULL, 0);
				SAlloc::F((char*)l);
			}
			p = xmlDictLookupSL(ctxt->dict, tmp);
			SAlloc::F(tmp);
			*prefix = NULL;
			return(p);
		}
		if(CUR == ':') {
			xmlChar * tmp;
			xmlNsErr(ctxt, XML_NS_ERR_QNAME, "Failed to parse QName '%s:%s:'\n", p, l, 0);
			NEXT;
			tmp = (xmlChar*)xmlParseName(ctxt);
			if(tmp) {
				tmp = xmlBuildQName(tmp, l, NULL, 0);
				l = xmlDictLookupSL(ctxt->dict, tmp);
				SAlloc::F(tmp);
				*prefix = p;
				return l;
			}
			tmp = xmlBuildQName(BAD_CAST "", l, NULL, 0);
			l = xmlDictLookupSL(ctxt->dict, tmp);
			SAlloc::F(tmp);
			*prefix = p;
			return l;
		}
		*prefix = p;
	}
	else
		*prefix = NULL;
	return l;
}
/**
 * xmlParseQNameAndCompare:
 * @ctxt:  an XML parser context
 * @name:  the localname
 * @prefix:  the prefix, if any.
 *
 * parse an XML name and compares for match
 * (specialized for endtag parsing)
 *
 * Returns NULL for an illegal name, (xmlChar*) 1 for success
 * and the name for mismatch
 */
static const xmlChar * xmlParseQNameAndCompare(xmlParserCtxt * ctxt, xmlChar const * name, xmlChar const * prefix)
{
	const xmlChar * cmp;
	const xmlChar * in;
	const xmlChar * ret;
	const xmlChar * prefix2;
	if(prefix == NULL)
		return xmlParseNameAndCompare(ctxt, name);
	else {
		GROW;
		in = ctxt->input->cur;
		cmp = prefix;
		while(*in != 0 && *in == *cmp) {
			++in;
			++cmp;
		}
		if((*cmp == 0) && (*in == ':')) {
			in++;
			cmp = name;
			while(*in != 0 && *in == *cmp) {
				++in;
				++cmp;
			}
			if(*cmp == 0 && (*in == '>' || IS_BLANK_CH(*in))) {
				// success 
				ctxt->input->cur = in;
				return (const xmlChar*)1;
			}
		}
		// all strings coms from the dictionary, equality can be done directly
		ret = xmlParseQName(ctxt, &prefix2);
		return ((ret == name) && (prefix == prefix2)) ? (const xmlChar*)1 : ret;
	}
}

/**
 * xmlParseAttValueInternal:
 * @ctxt:  an XML parser context
 * @len:  attribute len result
 * @alloc:  whether the attribute was reallocated as a new string
 * @normalize:  if 1 then further non-CDATA normalization must be done
 *
 * parse a value for an attribute.
 * NOTE: if no normalization is needed, the routine will return pointers
 *       directly from the data buffer.
 *
 * 3.3.3 Attribute-Value Normalization:
 * Before the value of an attribute is passed to the application or
 * checked for validity, the XML processor must normalize it as follows:
 * - a character reference is processed by appending the referenced
 *   character to the attribute value
 * - an entity reference is processed by recursively processing the
 *   replacement text of the entity
 * - a whitespace character (#x20, #xD, #xA, #x9) is processed by
 *   appending #x20 to the normalized value, except that only a single
 *   #x20 is appended for a "#xD#xA" sequence that is part of an external
 *   parsed entity or the literal entity value of an internal parsed entity
 * - other characters are processed by appending them to the normalized value
 * If the declared value is not CDATA, then the XML processor must further
 * process the normalized attribute value by discarding any leading and
 * trailing space (#x20) characters, and by replacing sequences of space
 * (#x20) characters by a single space (#x20) character.
 * All attributes for which no declaration has been read should be treated
 * by a non-validating parser as if declared CDATA.
 *
 * Returns the AttValue parsed or NULL. The value has to be freed by the
 *     caller if it was copied, this can be detected by val[*len] == 0.
 */

static xmlChar * xmlParseAttValueInternal(xmlParserCtxt * ctxt, int * len, int * alloc, int normalize)
{
	xmlChar limit = 0;
	const xmlChar * in = NULL, * start, * end, * last;
	xmlChar * ret = NULL;
	int line, col;
	GROW;
	in = (xmlChar*)CUR_PTR;
	line = ctxt->input->line;
	col = ctxt->input->col;
	if(*in != '"' && *in != '\'') {
		xmlFatalErr(ctxt, XML_ERR_ATTRIBUTE_NOT_STARTED, 0);
		return 0;
	}
	ctxt->instate = XML_PARSER_ATTRIBUTE_VALUE;

	/*
	 * try to handle in this routine the most common case where no
	 * allocation of a new string is required and where content is
	 * pure ASCII.
	 */
	limit = *in++;
	col++;
	end = ctxt->input->end;
	start = in;
	if(in >= end) {
		const xmlChar * oldbase = ctxt->input->base;
		GROW;
		if(oldbase != ctxt->input->base) {
			long delta = ctxt->input->base - oldbase;
			start = start + delta;
			in = in + delta;
		}
		end = ctxt->input->end;
	}
	if(normalize) {
		/*
		 * Skip any leading spaces
		 */
		while((in < end) && (*in != limit) && ((*in == 0x20) || (*in == 0x9) || (*in == 0xA) || (*in == 0xD))) {
			if(*in == 0xA) {
				line++; col = 1;
			}
			else {
				col++;
			}
			in++;
			start = in;
			if(in >= end) {
				const xmlChar * oldbase = ctxt->input->base;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
				if(oldbase != ctxt->input->base) {
					long delta = ctxt->input->base - oldbase;
					start = start + delta;
					in = in + delta;
				}
				end = ctxt->input->end;
				if(((in - start) > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
					xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED, "AttValue length too long\n");
					return 0;
				}
			}
		}
		while((in < end) && (*in != limit) && (*in >= 0x20) && (*in <= 0x7f) && (*in != '&') && (*in != '<')) {
			col++;
			if((*in++ == 0x20) && (*in == 0x20))
				break;
			if(in >= end) {
				const xmlChar * oldbase = ctxt->input->base;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
				if(oldbase != ctxt->input->base) {
					long delta = ctxt->input->base - oldbase;
					start = start + delta;
					in = in + delta;
				}
				end = ctxt->input->end;
				if(((in - start) > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
					xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED, "AttValue length too long\n");
					return 0;
				}
			}
		}
		last = in;
		/*
		 * skip the trailing blanks
		 */
		while((last[-1] == 0x20) && (last > start)) last--;
		while((in < end) && (*in != limit) &&
		    ((*in == 0x20) || (*in == 0x9) ||
			    (*in == 0xA) || (*in == 0xD))) {
			if(*in == 0xA) {
				line++, col = 1;
			}
			else {
				col++;
			}
			in++;
			if(in >= end) {
				const xmlChar * oldbase = ctxt->input->base;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
				if(oldbase != ctxt->input->base) {
					long delta = ctxt->input->base - oldbase;
					start = start + delta;
					in = in + delta;
					last = last + delta;
				}
				end = ctxt->input->end;
				if(((in - start) > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
					xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED, "AttValue length too long\n");
					return 0;
				}
			}
		}
		if(((in - start) > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
			xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED, "AttValue length too long\n");
			return 0;
		}
		if(*in != limit) goto need_complex;
	}
	else {
		while((in < end) && (*in != limit) && (*in >= 0x20) &&
		    (*in <= 0x7f) && (*in != '&') && (*in != '<')) {
			in++;
			col++;
			if(in >= end) {
				const xmlChar * oldbase = ctxt->input->base;
				GROW;
				if(ctxt->instate == XML_PARSER_EOF)
					return 0;
				if(oldbase != ctxt->input->base) {
					long delta = ctxt->input->base - oldbase;
					start = start + delta;
					in = in + delta;
				}
				end = ctxt->input->end;
				if(((in - start) > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
					xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED, "AttValue length too long\n");
					return 0;
				}
			}
		}
		last = in;
		if(((in - start) > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
			xmlFatalErrMsg(ctxt, XML_ERR_ATTRIBUTE_NOT_FINISHED, "AttValue length too long\n");
			return 0;
		}
		if(*in != limit) goto need_complex;
	}
	in++;
	col++;
	if(len != NULL) {
		*len = last - start;
		ret = (xmlChar*)start;
	}
	else {
		if(alloc) *alloc = 1;
		ret = xmlStrndup(start, last - start);
	}
	CUR_PTR = in;
	ctxt->input->line = line;
	ctxt->input->col = col;
	if(alloc) *alloc = 0;
	return ret;
need_complex:
	if(alloc) *alloc = 1;
	return xmlParseAttValueComplex(ctxt, len, normalize);
}

/**
 * xmlParseAttribute2:
 * @ctxt:  an XML parser context
 * @pref:  the element prefix
 * @elem:  the element name
 * @prefix:  a xmlChar ** used to store the value of the attribute prefix
 * @value:  a xmlChar ** used to store the value of the attribute
 * @len:  an int * to save the length of the attribute
 * @alloc:  an int * to indicate if the attribute was allocated
 *
 * parse an attribute in the new SAX2 framework.
 *
 * Returns the attribute name, and the value in *value, .
 */
static const xmlChar * xmlParseAttribute2(xmlParserCtxt * ctxt, const xmlChar * pref, const xmlChar * elem,
    const xmlChar ** prefix, xmlChar ** value, int * len, int * alloc)
{
	const xmlChar * name;
	xmlChar * val, * internal_val = NULL;
	int normalize = 0;
	*value = NULL;
	GROW;
	name = xmlParseQName(ctxt, prefix);
	if(!name) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "error parsing attribute name\n");
		return 0;
	}
	/*
	 * get the type if needed
	 */
	if(ctxt->attsSpecial) {
		int type = (int)(long)xmlHashQLookup2(ctxt->attsSpecial, pref, elem, *prefix, name);
		if(type != 0)
			normalize = 1;
	}
	/*
	 * read the value
	 */
	SKIP_BLANKS;
	if(RAW == '=') {
		NEXT;
		SKIP_BLANKS;
		val = xmlParseAttValueInternal(ctxt, len, alloc, normalize);
		if(normalize) {
			/*
			 * Sometimes a second normalisation pass for spaces is needed
			 * but that only happens if charrefs or entities refernces
			 * have been used in the attribute value, i.e. the attribute
			 * value have been extracted in an allocated string already.
			 */
			if(*alloc) {
				const xmlChar * val2 = xmlAttrNormalizeSpace2(ctxt, val, len);
				if(val2 && val2 != val) {
					SAlloc::F(val);
					val = (xmlChar*)val2;
				}
			}
		}
		ctxt->instate = XML_PARSER_CONTENT;
	}
	else {
		xmlFatalErrMsgStr(ctxt, XML_ERR_ATTRIBUTE_WITHOUT_VALUE, "Specification mandate value for attribute %s\n", name);
		return 0;
	}
	if(*prefix == ctxt->str_xml) {
		/*
		 * Check that xml:lang conforms to the specification
		 * No more registered as an error, just generate a warning now
		 * since this was deprecated in XML second edition
		 */
		if(ctxt->pedantic && sstreq(name, "lang")) {
			internal_val = xmlStrndup(val, *len);
			if(!xmlCheckLanguageID(internal_val)) {
				xmlWarningMsg(ctxt, XML_WAR_LANG_VALUE, "Malformed value for xml:lang : %s\n", internal_val, 0);
			}
		}
		/*
		 * Check that xml:space conforms to the specification
		 */
		if(sstreq(name, "space")) {
			internal_val = xmlStrndup(val, *len);
			if(sstreq(internal_val, "default"))
				*(ctxt->space) = 0;
			else if(sstreq(internal_val, "preserve"))
				*(ctxt->space) = 1;
			else {
				xmlWarningMsg(ctxt, XML_WAR_SPACE_VALUE, "Invalid value \"%s\" for xml:space : \"default\" or \"preserve\" expected\n", internal_val, 0);
			}
		}
		SAlloc::F(internal_val);
	}
	*value = val;
	return (name);
}

/**
 * xmlParseStartTag2:
 * @ctxt:  an XML parser context
 *
 * parse a start of tag either for rule element or
 * EmptyElement. In both case we don't parse the tag closing chars.
 * This routine is called when running SAX2 parsing
 *
 * [40] STag ::= '<' Name (S Attribute)* S? '>'
 *
 * [WFC: Unique Att Spec]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag.
 *
 * [44] EmptyElemTag ::= '<' Name (S Attribute)* S? '/>'
 *
 * [WFC: Unique Att Spec]
 * No attribute name may appear more than once in the same start-tag or
 * empty-element tag.
 *
 * With namespace:
 *
 * [NS 8] STag ::= '<' QName (S Attribute)* S? '>'
 *
 * [NS 10] EmptyElement ::= '<' QName (S Attribute)* S? '/>'
 *
 * Returns the element name parsed
 */
static const xmlChar * xmlParseStartTag2(xmlParserCtxt * ctxt, const xmlChar ** pref, const xmlChar ** URI, int * tlen)
{
	const xmlChar * localname;
	const xmlChar * prefix;
	const xmlChar * attname;
	const xmlChar * aprefix;
	const xmlChar * nsname;
	xmlChar * attvalue;
	const xmlChar ** atts = ctxt->atts;
	int maxatts = ctxt->maxatts;
	int nratts, nbatts, nbdef;
	int i, j, nbNs, attval, oldline, oldcol;
	const xmlChar * base;
	ulong cur;
	int nsNr = ctxt->nsNr;
	if(RAW != '<')
		return 0;
	NEXT1;

	/*
	 * NOTE: it is crucial with the SAX2 API to never call SHRINK beyond that
	 *       point since the attribute values may be stored as pointers to
	 *       the buffer and calling SHRINK would destroy them !
	 *       The Shrinking is only possible once the full set of attribute
	 *       callbacks have been done.
	 */
reparse:
	SHRINK;
	base = ctxt->input->base;
	cur = ctxt->input->cur - ctxt->input->base;
	oldline = ctxt->input->line;
	oldcol = ctxt->input->col;
	nbatts = 0;
	nratts = 0;
	nbdef = 0;
	nbNs = 0;
	attval = 0;
	/* Forget any namespaces added during an earlier parse of this element. */
	ctxt->nsNr = nsNr;
	localname = xmlParseQName(ctxt, &prefix);
	if(localname == NULL) {
		xmlFatalErrMsg(ctxt, XML_ERR_NAME_REQUIRED, "StartTag: invalid element name\n");
		return 0;
	}
	*tlen = ctxt->input->cur - ctxt->input->base - cur;
	/*
	 * Now parse the attributes, it ends up with the ending
	 *
	 * (S Attribute)* S?
	 */
	SKIP_BLANKS;
	GROW;
	if(ctxt->input->base != base)
		goto base_changed;
	while(((RAW != '>') && ((RAW != '/') || (NXT(1) != '>')) && (IS_BYTE_CHAR(RAW))) && (ctxt->instate != XML_PARSER_EOF)) {
		const xmlChar * q = CUR_PTR;
		uint cons = ctxt->input->consumed;
		int len = -1, alloc = 0;
		attname = xmlParseAttribute2(ctxt, prefix, localname, &aprefix, &attvalue, &len, &alloc);
		if(ctxt->input->base != base) {
			if((attvalue != NULL) && (alloc != 0))
				SAlloc::F(attvalue);
			attvalue = NULL;
			goto base_changed;
		}
		if(attname && attvalue) {
			if(len < 0)
				len = sstrlen(attvalue);
			if((attname == ctxt->str_xmlns) && (aprefix == NULL)) {
				const xmlChar * URL = xmlDictLookup(ctxt->dict, attvalue, len);
				xmlURIPtr uri;
				if(URL == NULL) {
					xmlErrMemory(ctxt, "dictionary allocation failure");
					if((attvalue != NULL) && (alloc != 0))
						SAlloc::F(attvalue);
					return 0;
				}
				if(*URL != 0) {
					uri = xmlParseURI((const char*)URL);
					if(uri == NULL) {
						xmlNsErr(ctxt, XML_WAR_NS_URI, "xmlns: '%s' is not a valid URI\n", URL, 0, 0);
					}
					else {
						if(uri->scheme == NULL) {
							xmlNsWarn(ctxt, XML_WAR_NS_URI_RELATIVE, "xmlns: URI %s is not absolute\n", URL, 0, 0);
						}
						xmlFreeURI(uri);
					}
					if(URL == ctxt->str_xml_ns) {
						if(attname != ctxt->str_xml) {
							xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE, "xml namespace URI cannot be the default namespace\n", NULL, 0, 0);
						}
						goto skip_default_ns;
					}
					if((len == 29) && (sstreq(URL, "http://www.w3.org/2000/xmlns/"))) {
						xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE, "reuse of the xmlns namespace name is forbidden\n", NULL, 0, 0);
						goto skip_default_ns;
					}
				}
				/*
				 * check that it's not a defined namespace
				 */
				for(j = 1; j <= nbNs; j++)
					if(ctxt->nsTab[ctxt->nsNr - 2 * j] == NULL)
						break;
				if(j <= nbNs)
					xmlErrAttributeDup(ctxt, NULL, attname);
				else if(nsPush(ctxt, NULL, URL) > 0)
					nbNs++;
skip_default_ns:
				if(alloc != 0)
					SAlloc::F(attvalue);
				if((RAW == '>') || (((RAW == '/') && (NXT(1) == '>'))))
					break;
				if(!IS_BLANK_CH(RAW)) {
					xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "attributes construct error\n");
					break;
				}
				SKIP_BLANKS;
				continue;
			}
			if(aprefix == ctxt->str_xmlns) {
				const xmlChar * URL = xmlDictLookup(ctxt->dict, attvalue, len);
				xmlURIPtr uri;
				if(attname == ctxt->str_xml) {
					if(URL != ctxt->str_xml_ns) {
						xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE, "xml namespace prefix mapped to wrong URI\n", NULL, 0, 0);
					}
					/*
					 * Do not keep a namespace definition node
					 */
					goto skip_ns;
				}
				if(URL == ctxt->str_xml_ns) {
					if(attname != ctxt->str_xml) {
						xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE, "xml namespace URI mapped to wrong prefix\n", NULL, 0, 0);
					}
					goto skip_ns;
				}
				if(attname == ctxt->str_xmlns) {
					xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE, "redefinition of the xmlns prefix is forbidden\n", NULL, 0, 0);
					goto skip_ns;
				}
				if((len == 29) && (sstreq(URL, "http://www.w3.org/2000/xmlns/"))) {
					xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE, "reuse of the xmlns namespace name is forbidden\n", NULL, 0, 0);
					goto skip_ns;
				}
				if(isempty(URL)) {
					xmlNsErr(ctxt, XML_NS_ERR_XML_NAMESPACE, "xmlns:%s: Empty XML namespace is not allowed\n", attname, 0, 0);
					goto skip_ns;
				}
				else {
					uri = xmlParseURI((const char*)URL);
					if(!uri) {
						xmlNsErr(ctxt, XML_WAR_NS_URI, "xmlns:%s: '%s' is not a valid URI\n", attname, URL, 0);
					}
					else {
						if(ctxt->pedantic && !uri->scheme) {
							xmlNsWarn(ctxt, XML_WAR_NS_URI_RELATIVE, "xmlns:%s: URI %s is not absolute\n", attname, URL, 0);
						}
						xmlFreeURI(uri);
					}
				}
				/*
				 * check that it's not a defined namespace
				 */
				for(j = 1; j <= nbNs; j++)
					if(ctxt->nsTab[ctxt->nsNr - 2 * j] == attname)
						break;
				if(j <= nbNs)
					xmlErrAttributeDup(ctxt, aprefix, attname);
				else if(nsPush(ctxt, attname, URL) > 0)
					nbNs++;
skip_ns:
				if(alloc != 0)
					SAlloc::F(attvalue);
				if((RAW == '>') || (((RAW == '/') && (NXT(1) == '>'))))
					break;
				if(!IS_BLANK_CH(RAW)) {
					xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "attributes construct error\n");
					break;
				}
				SKIP_BLANKS;
				if(ctxt->input->base != base)
					goto base_changed;
				continue;
			}

			/*
			 * Add the pair to atts
			 */
			if((atts == NULL) || (nbatts + 5 > maxatts)) {
				if(xmlCtxtGrowAttrs(ctxt, nbatts + 5) < 0) {
					if(attvalue[len] == 0)
						SAlloc::F(attvalue);
					goto failed;
				}
				maxatts = ctxt->maxatts;
				atts = ctxt->atts;
			}
			ctxt->attallocs[nratts++] = alloc;
			atts[nbatts++] = attname;
			atts[nbatts++] = aprefix;
			atts[nbatts++] = NULL; /* the URI will be fetched later */
			atts[nbatts++] = attvalue;
			attvalue += len;
			atts[nbatts++] = attvalue;
			/*
			 * tag if some deallocation is needed
			 */
			if(alloc != 0)
				attval = 1;
		}
		else {
			if((attvalue != NULL) && (attvalue[len] == 0))
				SAlloc::F(attvalue);
		}
failed:
		GROW
		if(ctxt->instate == XML_PARSER_EOF)
			break;
		if(ctxt->input->base != base)
			goto base_changed;
		if((RAW == '>') || (((RAW == '/') && (NXT(1) == '>'))))
			break;
		if(!IS_BLANK_CH(RAW)) {
			xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "attributes construct error\n");
			break;
		}
		SKIP_BLANKS;
		if((cons == ctxt->input->consumed) && (q == CUR_PTR) && (attname == NULL) && (attvalue == NULL)) {
			xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "xmlParseStartTag: problem parsing attributes\n");
			break;
		}
		GROW;
		if(ctxt->input->base != base)
			goto base_changed;
	}
	/*
	 * The attributes defaulting
	 */
	if(ctxt->attsDefault != NULL) {
		xmlDefAttrsPtr defaults = (xmlDefAttrsPtr)xmlHashLookup2(ctxt->attsDefault, localname, prefix);
		if(defaults != NULL) {
			for(i = 0; i < defaults->nbAttrs; i++) {
				attname = defaults->values[5 * i];
				aprefix = defaults->values[5 * i + 1];
				/*
				 * special work for namespaces defaulted defs
				 */
				if((attname == ctxt->str_xmlns) && (aprefix == NULL)) {
					/*
					 * check that it's not a defined namespace
					 */
					for(j = 1; j <= nbNs; j++)
						if(ctxt->nsTab[ctxt->nsNr - 2 * j] == NULL)
							break;
					if(j <= nbNs)
						continue;
					nsname = xmlGetNamespace(ctxt, 0);
					if(nsname != defaults->values[5 * i + 2]) {
						if(nsPush(ctxt, NULL, defaults->values[5 * i + 2]) > 0)
							nbNs++;
					}
				}
				else if(aprefix == ctxt->str_xmlns) {
					/*
					 * check that it's not a defined namespace
					 */
					for(j = 1; j <= nbNs; j++)
						if(ctxt->nsTab[ctxt->nsNr - 2 * j] == attname)
							break;
					if(j <= nbNs)
						continue;
					nsname = xmlGetNamespace(ctxt, attname);
					if(nsname != defaults->values[2]) {
						if(nsPush(ctxt, attname, defaults->values[5 * i + 2]) > 0)
							nbNs++;
					}
				}
				else {
					/*
					 * check that it's not a defined attribute
					 */
					for(j = 0; j < nbatts; j += 5) {
						if((attname == atts[j]) && (aprefix == atts[j+1]))
							break;
					}
					if(j < nbatts)
						continue;
					if(!atts || (nbatts + 5 > maxatts)) {
						if(xmlCtxtGrowAttrs(ctxt, nbatts + 5) < 0) {
							return 0;
						}
						maxatts = ctxt->maxatts;
						atts = ctxt->atts;
					}
					atts[nbatts++] = attname;
					atts[nbatts++] = aprefix;
					atts[nbatts++] = aprefix ? xmlGetNamespace(ctxt, aprefix) : 0;
					atts[nbatts++] = defaults->values[5 * i + 2];
					atts[nbatts++] = defaults->values[5 * i + 3];
					if(ctxt->standalone == 1 && defaults->values[5 * i + 4]) {
						xmlValidityError(ctxt, XML_DTD_STANDALONE_DEFAULTED,
						    "standalone: attribute %s on %s defaulted from external subset\n", attname, localname);
					}
					nbdef++;
				}
			}
		}
	}
	/*
	 * The attributes checkings
	 */
	for(i = 0; i < nbatts; i += 5) {
		/*
		 * The default namespace does not apply to attribute names.
		 */
		if(atts[i + 1] != NULL) {
			nsname = xmlGetNamespace(ctxt, atts[i + 1]);
			if(nsname == NULL) {
				xmlNsErr(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE, "Namespace prefix %s for %s on %s is not defined\n", atts[i + 1], atts[i], localname);
			}
			atts[i + 2] = nsname;
		}
		else
			nsname = NULL;
		/*
		 * [WFC: Unique Att Spec]
		 * No attribute name may appear more than once in the same
		 * start-tag or empty-element tag.
		 * As extended by the Namespace in XML REC.
		 */
		for(j = 0; j < i; j += 5) {
			if(atts[i] == atts[j]) {
				if(atts[i+1] == atts[j+1]) {
					xmlErrAttributeDup(ctxt, atts[i+1], atts[i]);
					break;
				}
				if((nsname != NULL) && (atts[j + 2] == nsname)) {
					xmlNsErr(ctxt, XML_NS_ERR_ATTRIBUTE_REDEFINED, "Namespaced Attribute %s in '%s' redefined\n", atts[i], nsname, 0);
					break;
				}
			}
		}
	}
	nsname = xmlGetNamespace(ctxt, prefix);
	if((prefix != NULL) && (nsname == NULL)) {
		xmlNsErr(ctxt, XML_NS_ERR_UNDEFINED_NAMESPACE, "Namespace prefix %s on %s is not defined\n", prefix, localname, 0);
	}
	*pref = prefix;
	*URI = nsname;
	/*
	 * SAX: Start of Element !
	 */
	if(ctxt->sax && ctxt->sax->startElementNs && !ctxt->disableSAX) {
		if(nbNs > 0)
			ctxt->sax->startElementNs(ctxt->userData, localname, prefix, nsname, nbNs, &ctxt->nsTab[ctxt->nsNr - 2 * nbNs], nbatts / 5, nbdef, atts);
		else
			ctxt->sax->startElementNs(ctxt->userData, localname, prefix, nsname, 0, NULL, nbatts / 5, nbdef, atts);
	}
	/*
	 * Free up attribute allocated strings if needed
	 */
	if(attval != 0) {
		for(i = 3, j = 0; j < nratts; i += 5, j++)
			if((ctxt->attallocs[j] != 0) && (atts[i] != NULL))
				SAlloc::F((xmlChar*)atts[i]);
	}
	return localname;
base_changed:
	/*
	 * the attribute strings are valid iif the base didn't changed
	 */
	if(attval != 0) {
		for(i = 3, j = 0; j < nratts; i += 5, j++)
			if((ctxt->attallocs[j] != 0) && (atts[i] != NULL))
				SAlloc::F((xmlChar*)atts[i]);
	}
	ctxt->input->cur = ctxt->input->base + cur;
	ctxt->input->line = oldline;
	ctxt->input->col = oldcol;
	if(ctxt->wellFormed == 1) {
		goto reparse;
	}
	return 0;
}
/**
 * xmlParseEndTag2:
 * @ctxt:  an XML parser context
 * @line:  line of the start tag
 * @nsNr:  number of namespaces on the start tag
 *
 * parse an end of tag
 *
 * [42] ETag ::= '</' Name S? '>'
 *
 * With namespace
 *
 * [NS 9] ETag ::= '</' QName S? '>'
 */

static void xmlParseEndTag2(xmlParserCtxt * ctxt, const xmlChar * prefix, const xmlChar * URI, int line, int nsNr, int tlen)
{
	const xmlChar * name;
	GROW;
	if((RAW != '<') || (NXT(1) != '/')) {
		xmlFatalErr(ctxt, XML_ERR_LTSLASH_REQUIRED, 0);
		return;
	}
	SKIP(2);
	if((tlen > 0) && (xmlStrncmp(ctxt->input->cur, ctxt->name, tlen) == 0)) {
		if(ctxt->input->cur[tlen] == '>') {
			ctxt->input->cur += tlen + 1;
			ctxt->input->col += tlen + 1;
			goto done;
		}
		ctxt->input->cur += tlen;
		ctxt->input->col += tlen;
		name = (xmlChar*)1;
	}
	else
		name = prefix ? xmlParseQNameAndCompare(ctxt, ctxt->name, prefix) : xmlParseNameAndCompare(ctxt, ctxt->name);
	/*
	 * We should definitely be at the ending "S? '>'" part
	 */
	GROW;
	if(ctxt->instate == XML_PARSER_EOF)
		return;
	SKIP_BLANKS;
	if((!IS_BYTE_CHAR(RAW)) || (RAW != '>')) {
		xmlFatalErr(ctxt, XML_ERR_GT_REQUIRED, 0);
	}
	else
		NEXT1;
	/*
	 * [WFC: Element Type Match]
	 * The Name in an element's end-tag must match the element type in the
	 * start-tag.
	 *
	 */
	if(name != (xmlChar*)1) {
		SETIFZ(name, BAD_CAST "unparseable");
		if((line == 0) && (ctxt->P_Node != NULL))
			line = ctxt->P_Node->line;
		xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NAME_MISMATCH, "Opening and ending tag mismatch: %s line %d and %s\n", ctxt->name, line, name);
	}
	/*
	 * SAX: End of Tag
	 */
done:
	if(ctxt->sax && ctxt->sax->endElementNs && (!ctxt->disableSAX))
		ctxt->sax->endElementNs(ctxt->userData, ctxt->name, prefix, URI);
	spacePop(ctxt);
	if(nsNr != 0)
		nsPop(ctxt, nsNr);
	return;
}

/**
 * xmlParseCDSect:
 * @ctxt:  an XML parser context
 *
 * Parse escaped pure raw content.
 *
 * [18] CDSect ::= CDStart CData CDEnd
 *
 * [19] CDStart ::= '<![CDATA['
 *
 * [20] Data ::= (Char* - (Char* ']]>' Char*))
 *
 * [21] CDEnd ::= ']]>'
 */
void xmlParseCDSect(xmlParserCtxt * ctxt)
{
	xmlChar * buf = NULL;
	int len = 0;
	int size = XML_PARSER_BUFFER_SIZE;
	int r, rl;
	int s, sl;
	int cur, l;
	int count = 0;
	/* Check 2.6.0 was NXT(0) not RAW */
	if(CMP9(CUR_PTR, '<', '!', '[', 'C', 'D', 'A', 'T', 'A', '[')) {
		SKIP(9);
	}
	else
		return;
	ctxt->instate = XML_PARSER_CDATA_SECTION;
	r = CUR_CHAR(rl);
	if(!IS_CHAR(r)) {
		xmlFatalErr(ctxt, XML_ERR_CDATA_NOT_FINISHED, 0);
		ctxt->instate = XML_PARSER_CONTENT;
		return;
	}
	NEXTL(rl);
	s = CUR_CHAR(sl);
	if(!IS_CHAR(s)) {
		xmlFatalErr(ctxt, XML_ERR_CDATA_NOT_FINISHED, 0);
		ctxt->instate = XML_PARSER_CONTENT;
		return;
	}
	NEXTL(sl);
	cur = CUR_CHAR(l);
	buf = (xmlChar*)SAlloc::M(size * sizeof(xmlChar));
	if(!buf) {
		xmlErrMemory(ctxt, 0);
		return;
	}
	while(IS_CHAR(cur) && ((r != ']') || (s != ']') || (cur != '>'))) {
		if(len + 5 >= size) {
			xmlChar * tmp;
			if((size > XML_MAX_TEXT_LENGTH) && ((ctxt->options & XML_PARSE_HUGE) == 0)) {
				xmlFatalErrMsgStr(ctxt, XML_ERR_CDATA_NOT_FINISHED, "CData section too big found", 0);
				SAlloc::F(buf);
				return;
			}
			tmp = (xmlChar*)SAlloc::R(buf, size * 2 * sizeof(xmlChar));
			if(!tmp) {
				SAlloc::F(buf);
				xmlErrMemory(ctxt, 0);
				return;
			}
			buf = tmp;
			size *= 2;
		}
		COPY_BUF(rl, buf, len, r);
		r = s;
		rl = sl;
		s = cur;
		sl = l;
		count++;
		if(count > 50) {
			GROW;
			if(ctxt->instate == XML_PARSER_EOF) {
				SAlloc::F(buf);
				return;
			}
			count = 0;
		}
		NEXTL(l);
		cur = CUR_CHAR(l);
	}
	buf[len] = 0;
	ctxt->instate = XML_PARSER_CONTENT;
	if(cur != '>') {
		xmlFatalErrMsgStr(ctxt, XML_ERR_CDATA_NOT_FINISHED, "CData section not finished\n%.50s\n", buf);
		SAlloc::F(buf);
		return;
	}
	NEXTL(l);
	/*
	 * OK the buffer is to be consumed as cdata.
	 */
	if(ctxt->sax && !ctxt->disableSAX) {
		if(ctxt->sax->cdataBlock)
			ctxt->sax->cdataBlock(ctxt->userData, buf, len);
		else if(ctxt->sax->characters)
			ctxt->sax->characters(ctxt->userData, buf, len);
	}
	SAlloc::F(buf);
}
/**
 * xmlParseContent:
 * @ctxt:  an XML parser context
 *
 * Parse a content:
 *
 * [43] content ::= (element | CharData | Reference | CDSect | PI | Comment)*
 */
void xmlParseContent(xmlParserCtxt * ctxt)
{
	GROW;
	while((RAW != 0) && ((RAW != '<') || (NXT(1) != '/')) && (ctxt->instate != XML_PARSER_EOF)) {
		const xmlChar * test = CUR_PTR;
		uint cons = ctxt->input->consumed;
		const xmlChar * cur = ctxt->input->cur;
		/*
		 * First case : a Processing Instruction.
		 */
		if((*cur == '<') && (cur[1] == '?')) {
			xmlParsePI(ctxt);
		}
		// 
		// Second case : a CDSection
		// 
		// 2.6.0 test was *cur not RAW */
		else if(CMP9(CUR_PTR, '<', '!', '[', 'C', 'D', 'A', 'T', 'A', '[')) {
			xmlParseCDSect(ctxt);
		}
		// 
		// Third case :  a comment
		// 
		else if((*cur == '<') && (NXT(1) == '!') && (NXT(2) == '-') && (NXT(3) == '-')) {
			xmlParseComment(ctxt);
			ctxt->instate = XML_PARSER_CONTENT;
		}
		// 
		// Fourth case :  a sub-element.
		// 
		else if(*cur == '<') {
			xmlParseElement(ctxt);
		}
		// 
		// Fifth case : a reference. If if has not been resolved, parsing returns it's Name, create the node
		// 
		else if(*cur == '&') {
			xmlParseReference(ctxt);
		}
		// 
		// Last case, text. Note that References are handled directly.
		// 
		else {
			xmlParseCharData(ctxt, 0);
		}
		GROW;
		/*
		 * Pop-up of finished entities.
		 */
		while((RAW == 0) && (ctxt->inputNr > 1))
			xmlPopInput(ctxt);
		SHRINK;
		if((cons == ctxt->input->consumed) && (test == CUR_PTR)) {
			xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "detected an error in element content\n");
			ctxt->instate = XML_PARSER_EOF;
			break;
		}
	}
}
/**
 * xmlParseElement:
 * @ctxt:  an XML parser context
 *
 * parse an XML element, this is highly recursive
 *
 * [39] element ::= EmptyElemTag | STag content ETag
 *
 * [WFC: Element Type Match]
 * The Name in an element's end-tag must match the element type in the
 * start-tag.
 *
 */
void FASTCALL xmlParseElement(xmlParserCtxt * ctxt)
{
	const xmlChar * name;
	const xmlChar * prefix = NULL;
	const xmlChar * URI = NULL;
	xmlParserNodeInfo node_info;
	int line, tlen = 0;
	xmlNode * ret;
	int nsNr = ctxt->nsNr;
	if(((uint)ctxt->nameNr > xmlParserMaxDepth) && !(ctxt->options & XML_PARSE_HUGE)) {
		xmlFatalErrMsgInt(ctxt, XML_ERR_INTERNAL_ERROR, "Excessive depth in document: %d use XML_PARSE_HUGE option\n", xmlParserMaxDepth);
		ctxt->instate = XML_PARSER_EOF;
		return;
	}
	/* Capture start position */
	if(ctxt->record_info) {
		node_info.begin_pos = ctxt->input->consumed + (CUR_PTR - ctxt->input->base);
		node_info.begin_line = ctxt->input->line;
	}
	if(ctxt->spaceNr == 0)
		spacePush(ctxt, -1);
	else if(*ctxt->space == -2)
		spacePush(ctxt, -1);
	else
		spacePush(ctxt, *ctxt->space);
	line = ctxt->input->line;
#ifdef LIBXML_SAX1_ENABLED
	if(ctxt->sax2)
#endif /* LIBXML_SAX1_ENABLED */
	name = xmlParseStartTag2(ctxt, &prefix, &URI, &tlen);
#ifdef LIBXML_SAX1_ENABLED
	else
		name = xmlParseStartTag(ctxt);
#endif /* LIBXML_SAX1_ENABLED */
	if(ctxt->instate == XML_PARSER_EOF)
		return;
	if(!name) {
		spacePop(ctxt);
		return;
	}
	namePush(ctxt, name);
	ret = ctxt->P_Node;
#ifdef LIBXML_VALID_ENABLED
	/*
	 * [VC: Root Element Type]
	 * The Name in the document type declaration must match the element
	 * type of the root element.
	 */
	if(ctxt->validate && ctxt->wellFormed && ctxt->myDoc && ctxt->P_Node && (ctxt->P_Node == ctxt->myDoc->children))
		ctxt->valid &= xmlValidateRoot(&ctxt->vctxt, ctxt->myDoc);
#endif /* LIBXML_VALID_ENABLED */
	/*
	 * Check for an Empty Element.
	 */
	if((RAW == '/') && (NXT(1) == '>')) {
		SKIP(2);
		if(ctxt->sax2) {
			if(ctxt->sax && ctxt->sax->endElementNs && !ctxt->disableSAX)
				ctxt->sax->endElementNs(ctxt->userData, name, prefix, URI);
#ifdef LIBXML_SAX1_ENABLED
		}
		else {
			if(ctxt->sax && ctxt->sax->endElement && !ctxt->disableSAX)
				ctxt->sax->endElement(ctxt->userData, name);
#endif /* LIBXML_SAX1_ENABLED */
		}
		namePop(ctxt);
		spacePop(ctxt);
		if(nsNr != ctxt->nsNr)
			nsPop(ctxt, ctxt->nsNr - nsNr);
		if(ret && ctxt->record_info) {
			node_info.end_pos = ctxt->input->consumed + (CUR_PTR - ctxt->input->base);
			node_info.end_line = ctxt->input->line;
			node_info.P_Node = ret;
			xmlParserAddNodeInfo(ctxt, &node_info);
		}
		return;
	}
	if(RAW == '>') {
		NEXT1;
	}
	else {
		xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_GT_REQUIRED, "Couldn't find end of Start Tag %s line %d\n", name, line, 0);
		//
		// end of parsing of this node.
		//
		nodePop(ctxt);
		namePop(ctxt);
		spacePop(ctxt);
		if(nsNr != ctxt->nsNr)
			nsPop(ctxt, ctxt->nsNr - nsNr);
		/*
		 * Capture end position and add node
		 */
		if(ret && ctxt->record_info) {
			node_info.end_pos = ctxt->input->consumed + (CUR_PTR - ctxt->input->base);
			node_info.end_line = ctxt->input->line;
			node_info.P_Node = ret;
			xmlParserAddNodeInfo(ctxt, &node_info);
		}
		return;
	}
	/*
	 * Parse the content of the element:
	 */
	xmlParseContent(ctxt);
	if(ctxt->instate == XML_PARSER_EOF)
		return;
	if(!IS_BYTE_CHAR(RAW)) {
		xmlFatalErrMsgStrIntStr(ctxt, XML_ERR_TAG_NOT_FINISHED, "Premature end of data in tag %s line %d\n", name, line, 0);
		/*
		 * end of parsing of this node.
		 */
		nodePop(ctxt);
		namePop(ctxt);
		spacePop(ctxt);
		if(nsNr != ctxt->nsNr)
			nsPop(ctxt, ctxt->nsNr - nsNr);
		return;
	}
	/*
	 * parse the end of tag: '</' should be here.
	 */
	if(ctxt->sax2) {
		xmlParseEndTag2(ctxt, prefix, URI, line, ctxt->nsNr - nsNr, tlen);
		namePop(ctxt);
	}
#ifdef LIBXML_SAX1_ENABLED
	else
		xmlParseEndTag1(ctxt, line);
#endif /* LIBXML_SAX1_ENABLED */

	/*
	 * Capture end position and add node
	 */
	if(ret && ctxt->record_info) {
		node_info.end_pos = ctxt->input->consumed + (CUR_PTR - ctxt->input->base);
		node_info.end_line = ctxt->input->line;
		node_info.P_Node = ret;
		xmlParserAddNodeInfo(ctxt, &node_info);
	}
}
/**
 * xmlParseVersionNum:
 * @ctxt:  an XML parser context
 *
 * parse the XML version value.
 *
 * [26] VersionNum ::= '1.' [0-9]+
 *
 * In practice allow [0-9].[0-9]+ at that level
 *
 * Returns the string giving the XML version number, or NULL
 */
xmlChar * xmlParseVersionNum(xmlParserCtxt * ctxt)
{
	int len = 0;
	int size = 10;
	xmlChar cur;
	xmlChar * buf = (xmlChar*)SAlloc::M(size * sizeof(xmlChar));
	if(!buf) {
		xmlErrMemory(ctxt, 0);
		return 0;
	}
	cur = CUR;
	if(!((cur >= '0') && (cur <= '9'))) {
		SAlloc::F(buf);
		return 0;
	}
	buf[len++] = cur;
	NEXT;
	cur = CUR;
	if(cur != '.') {
		SAlloc::F(buf);
		return 0;
	}
	buf[len++] = cur;
	NEXT;
	cur = CUR;
	while((cur >= '0') && (cur <= '9')) {
		if(len + 1 >= size) {
			xmlChar * tmp;
			size *= 2;
			tmp = (xmlChar*)SAlloc::R(buf, size * sizeof(xmlChar));
			if(!tmp) {
				SAlloc::F(buf);
				xmlErrMemory(ctxt, 0);
				return 0;
			}
			buf = tmp;
		}
		buf[len++] = cur;
		NEXT;
		cur = CUR;
	}
	buf[len] = 0;
	return buf;
}

/**
 * xmlParseVersionInfo:
 * @ctxt:  an XML parser context
 *
 * parse the XML version.
 *
 * [24] VersionInfo ::= S 'version' Eq (' VersionNum ' | " VersionNum ")
 *
 * [25] Eq ::= S? '=' S?
 *
 * Returns the version string, e.g. "1.0"
 */

xmlChar * xmlParseVersionInfo(xmlParserCtxt * ctxt)
{
	xmlChar * version = NULL;
	if(CMP7(CUR_PTR, 'v', 'e', 'r', 's', 'i', 'o', 'n')) {
		SKIP(7);
		SKIP_BLANKS;
		if(RAW != '=') {
			xmlFatalErr(ctxt, XML_ERR_EQUAL_REQUIRED, 0);
			return 0;
		}
		NEXT;
		SKIP_BLANKS;
		if(RAW == '"') {
			NEXT;
			version = xmlParseVersionNum(ctxt);
			if(RAW != '"') {
				xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, 0);
			}
			else
				NEXT;
		}
		else if(RAW == '\'') {
			NEXT;
			version = xmlParseVersionNum(ctxt);
			if(RAW != '\'') {
				xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, 0);
			}
			else
				NEXT;
		}
		else {
			xmlFatalErr(ctxt, XML_ERR_STRING_NOT_STARTED, 0);
		}
	}
	return(version);
}

/**
 * xmlParseEncName:
 * @ctxt:  an XML parser context
 *
 * parse the XML encoding name
 *
 * [81] EncName ::= [A-Za-z] ([A-Za-z0-9._] | '-')*
 *
 * Returns the encoding name value or NULL
 */
xmlChar * xmlParseEncName(xmlParserCtxt * ctxt)
{
	xmlChar * buf = NULL;
	int len = 0;
	int size = 10;
	xmlChar cur = CUR;
	if(((cur >= 'a') && (cur <= 'z')) || ((cur >= 'A') && (cur <= 'Z'))) {
		buf = (xmlChar*)SAlloc::M(size * sizeof(xmlChar));
		if(!buf) {
			xmlErrMemory(ctxt, 0);
			return 0;
		}
		buf[len++] = cur;
		NEXT;
		cur = CUR;
		while(((cur >= 'a') && (cur <= 'z')) || ((cur >= 'A') && (cur <= 'Z')) ||
		    ((cur >= '0') && (cur <= '9')) || (cur == '.') || (cur == '_') || (cur == '-')) {
			if((len + 1) >= size) {
				xmlChar * tmp;
				size *= 2;
				tmp = (xmlChar*)SAlloc::R(buf, size * sizeof(xmlChar));
				if(!tmp) {
					xmlErrMemory(ctxt, 0);
					SAlloc::F(buf);
					return 0;
				}
				buf = tmp;
			}
			buf[len++] = cur;
			NEXT;
			cur = CUR;
			if(cur == 0) {
				SHRINK;
				GROW;
				cur = CUR;
			}
		}
		buf[len] = 0;
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_ENCODING_NAME, 0);
	}
	return buf;
}

/**
 * xmlParseEncodingDecl:
 * @ctxt:  an XML parser context
 *
 * parse the XML encoding declaration
 *
 * [80] EncodingDecl ::= S 'encoding' Eq ('"' EncName '"' |  "'" EncName "'")
 *
 * this setups the conversion filters.
 *
 * Returns the encoding value or NULL
 */

const xmlChar * xmlParseEncodingDecl(xmlParserCtxt * ctxt)
{
	xmlChar * encoding = NULL;
	SKIP_BLANKS;
	if(CMP8(CUR_PTR, 'e', 'n', 'c', 'o', 'd', 'i', 'n', 'g')) {
		SKIP(8);
		SKIP_BLANKS;
		if(RAW != '=') {
			xmlFatalErr(ctxt, XML_ERR_EQUAL_REQUIRED, 0);
			return 0;
		}
		NEXT;
		SKIP_BLANKS;
		if(RAW == '"') {
			NEXT;
			encoding = xmlParseEncName(ctxt);
			if(RAW != '"') {
				xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, 0);
			}
			else
				NEXT;
		}
		else if(RAW == '\'') {
			NEXT;
			encoding = xmlParseEncName(ctxt);
			if(RAW != '\'') {
				xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, 0);
			}
			else
				NEXT;
		}
		else {
			xmlFatalErr(ctxt, XML_ERR_STRING_NOT_STARTED, 0);
		}
		/*
		 * Non standard parsing, allowing the user to ignore encoding
		 */
		if(ctxt->options & XML_PARSE_IGNORE_ENC) {
			SAlloc::F((xmlChar*)encoding);
			return 0;
		}
		/*
		 * UTF-16 encoding stwich has already taken place at this stage,
		 * more over the little-endian/big-endian selection is already done
		 */
		if(encoding && (sstreqi_ascii(encoding, BAD_CAST "UTF-16") || sstreqi_ascii(encoding, BAD_CAST "UTF16"))) {
			/*
			 * If no encoding was passed to the parser, that we are
			 * using UTF-16 and no decoder is present i.e. the
			 * document is apparently UTF-8 compatible, then raise an
			 * encoding mismatch fatal error
			 */
			if(!ctxt->encoding && ctxt->input->buf && !ctxt->input->buf->encoder) {
				xmlFatalErrMsg(ctxt, XML_ERR_INVALID_ENCODING, "Document labelled UTF-16 but has UTF-8 content\n");
			}
			SAlloc::F((xmlChar*)ctxt->encoding);
			ctxt->encoding = encoding;
		}
		/*
		 * UTF-8 encoding is handled natively
		 */
		else if(encoding && (sstreqi_ascii(encoding, BAD_CAST "UTF-8") || sstreqi_ascii(encoding, BAD_CAST "UTF8"))) {
			SAlloc::F((xmlChar*)ctxt->encoding);
			ctxt->encoding = encoding;
		}
		else if(encoding) {
			xmlCharEncodingHandler * handler;
			SAlloc::F((xmlChar*)ctxt->input->encoding);
			ctxt->input->encoding = encoding;
			handler = xmlFindCharEncodingHandler((const char*)encoding);
			if(handler) {
				xmlSwitchToEncoding(ctxt, handler);
			}
			else {
				xmlFatalErrMsgStr(ctxt, XML_ERR_UNSUPPORTED_ENCODING, "Unsupported encoding %s\n", encoding);
				return 0;
			}
		}
	}
	return(encoding);
}

/**
 * xmlParseSDDecl:
 * @ctxt:  an XML parser context
 *
 * parse the XML standalone declaration
 *
 * [32] SDDecl ::= S 'standalone' Eq
 *                 (("'" ('yes' | 'no') "'") | ('"' ('yes' | 'no')'"'))
 *
 * [VC: Standalone Document Declaration]
 * @todo The standalone document declaration must have the value "no"
 * if any external markup declarations contain declarations of:
 *  - attributes with default values, if elements to which these
 *    attributes apply appear in the document without specifications
 *    of values for these attributes, or
 *  - entities (other than amp, lt, gt, apos, quot), if references
 *    to those entities appear in the document, or
 *  - attributes with values subject to normalization, where the
 *    attribute appears in the document with a value which will change
 *    as a result of normalization, or
 *  - element types with element content, if white space occurs directly
 *    within any instance of those types.
 *
 * Returns:
 *   1 if standalone="yes"
 *   0 if standalone="no"
 *  -2 if standalone attribute is missing or invalid
 *	  (A standalone value of -2 means that the XML declaration was found,
 *	   but no value was specified for the standalone attribute).
 */

int xmlParseSDDecl(xmlParserCtxt * ctxt)
{
	int standalone = -2;
	SKIP_BLANKS;
	if(CMP10(CUR_PTR, 's', 't', 'a', 'n', 'd', 'a', 'l', 'o', 'n', 'e')) {
		SKIP(10);
		SKIP_BLANKS;
		if(RAW != '=') {
			xmlFatalErr(ctxt, XML_ERR_EQUAL_REQUIRED, 0);
			return(standalone);
		}
		NEXT;
		SKIP_BLANKS;
		if(RAW == '\'') {
			NEXT;
			if((RAW == 'n') && (NXT(1) == 'o')) {
				standalone = 0;
				SKIP(2);
			}
			else if((RAW == 'y') && (NXT(1) == 'e') && (NXT(2) == 's')) {
				standalone = 1;
				SKIP(3);
			}
			else {
				xmlFatalErr(ctxt, XML_ERR_STANDALONE_VALUE, 0);
			}
			if(RAW != '\'') {
				xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, 0);
			}
			else
				NEXT;
		}
		else if(RAW == '"') {
			NEXT;
			if((RAW == 'n') && (NXT(1) == 'o')) {
				standalone = 0;
				SKIP(2);
			}
			else if((RAW == 'y') && (NXT(1) == 'e') &&
			    (NXT(2) == 's')) {
				standalone = 1;
				SKIP(3);
			}
			else {
				xmlFatalErr(ctxt, XML_ERR_STANDALONE_VALUE, 0);
			}
			if(RAW != '"') {
				xmlFatalErr(ctxt, XML_ERR_STRING_NOT_CLOSED, 0);
			}
			else
				NEXT;
		}
		else {
			xmlFatalErr(ctxt, XML_ERR_STRING_NOT_STARTED, 0);
		}
	}
	return(standalone);
}

/**
 * xmlParseXMLDecl:
 * @ctxt:  an XML parser context
 *
 * parse an XML declaration header
 *
 * [23] XMLDecl ::= '<?xml' VersionInfo EncodingDecl? SDDecl? S? '?>'
 */

void xmlParseXMLDecl(xmlParserCtxt * ctxt)
{
	xmlChar * version;
	/*
	 * This value for standalone indicates that the document has an
	 * XML declaration but it does not have a standalone attribute.
	 * It will be overwritten later if a standalone attribute is found.
	 */
	ctxt->input->standalone = -2;
	/*
	 * We know that '<?xml' is here.
	 */
	SKIP(5);
	if(!IS_BLANK_CH(RAW)) {
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Blank needed after '<?xml'\n");
	}
	SKIP_BLANKS;
	/*
	 * We must have the VersionInfo here.
	 */
	version = xmlParseVersionInfo(ctxt);
	if(version == NULL) {
		xmlFatalErr(ctxt, XML_ERR_VERSION_MISSING, 0);
	}
	else {
		if(!sstreq(version, (const xmlChar*)XML_DEFAULT_VERSION)) {
			/*
			 * Changed here for XML-1.0 5th edition
			 */
			if(ctxt->options & XML_PARSE_OLD10) {
				xmlFatalErrMsgStr(ctxt, XML_ERR_UNKNOWN_VERSION, "Unsupported version '%s'\n", version);
			}
			else {
				if((version[0] == '1') && ((version[1] == '.'))) {
					xmlWarningMsg(ctxt, XML_WAR_UNKNOWN_VERSION, "Unsupported version '%s'\n", version, 0);
				}
				else {
					xmlFatalErrMsgStr(ctxt, XML_ERR_UNKNOWN_VERSION, "Unsupported version '%s'\n", version);
				}
			}
		}
		SAlloc::F((void*)ctxt->version);
		ctxt->version = version;
	}
	/*
	 * We may have the encoding declaration
	 */
	if(!IS_BLANK_CH(RAW)) {
		if((RAW == '?') && (NXT(1) == '>')) {
			SKIP(2);
			return;
		}
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Blank needed here\n");
	}
	xmlParseEncodingDecl(ctxt);
	if(ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
		/*
		 * The XML REC instructs us to stop parsing right here
		 */
		return;
	}
	/*
	 * We may have the standalone status.
	 */
	if(ctxt->input->encoding && (!IS_BLANK_CH(RAW))) {
		if((RAW == '?') && (NXT(1) == '>')) {
			SKIP(2);
			return;
		}
		xmlFatalErrMsg(ctxt, XML_ERR_SPACE_REQUIRED, "Blank needed here\n");
	}

	/*
	 * We can grow the input buffer freely at that point
	 */
	GROW;

	SKIP_BLANKS;
	ctxt->input->standalone = xmlParseSDDecl(ctxt);

	SKIP_BLANKS;
	if((RAW == '?') && (NXT(1) == '>')) {
		SKIP(2);
	}
	else if(RAW == '>') {
		/* Deprecated old WD ... */
		xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, 0);
		NEXT;
	}
	else {
		xmlFatalErr(ctxt, XML_ERR_XMLDECL_NOT_FINISHED, 0);
		MOVETO_ENDTAG(CUR_PTR);
		NEXT;
	}
}

/**
 * xmlParseMisc:
 * @ctxt:  an XML parser context
 *
 * parse an XML Misc* optional field.
 *
 * [27] Misc ::= Comment | PI |  S
 */
void xmlParseMisc(xmlParserCtxt * ctxt)
{
	while((ctxt->instate != XML_PARSER_EOF) && (((RAW == '<') && (NXT(1) == '?')) || (CMP4(CUR_PTR, '<', '!', '-', '-')) || IS_BLANK_CH(CUR))) {
		if((RAW == '<') && (NXT(1) == '?')) {
			xmlParsePI(ctxt);
		}
		else if(IS_BLANK_CH(CUR)) {
			NEXT;
		}
		else
			xmlParseComment(ctxt);
	}
}
/**
 * xmlParseDocument:
 * @ctxt:  an XML parser context
 *
 * parse an XML document (and build a tree if using the standard SAX interface).
 *
 * [1] document ::= prolog element Misc*
 *
 * [22] prolog ::= XMLDecl? Misc* (doctypedecl Misc*)?
 *
 * Returns 0, -1 in case of error. the parser context is augmented as a result of the parsing.
 */

int xmlParseDocument(xmlParserCtxt * ctxt)
{
	xmlChar start[4];
	xmlCharEncoding enc;
	xmlInitParser();
	if(!ctxt || !ctxt->input)
		return -1;
	GROW;
	/*
	 * SAX: detecting the level.
	 */
	xmlDetectSAX2(ctxt);
	/*
	 * SAX: beginning of the document processing.
	 */
	if(ctxt->sax && ctxt->sax->setDocumentLocator)
		ctxt->sax->setDocumentLocator(ctxt->userData, &xmlDefaultSAXLocator);
	if(ctxt->instate == XML_PARSER_EOF)
		return -1;
	if(!ctxt->encoding && ((ctxt->input->end - ctxt->input->cur) >= 4)) {
		/*
		 * Get the 4 first bytes and decode the charset
		 * if enc != XML_CHAR_ENCODING_NONE
		 * plug some encoding conversion routines.
		 */
		start[0] = RAW;
		start[1] = NXT(1);
		start[2] = NXT(2);
		start[3] = NXT(3);
		enc = xmlDetectCharEncoding(&start[0], 4);
		if(enc != XML_CHAR_ENCODING_NONE)
			xmlSwitchEncoding(ctxt, enc);
	}
	if(CUR == 0) {
		xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, 0);
	}
	/*
	 * Check for the XMLDecl in the Prolog.
	 * do not GROW here to avoid the detected encoder to decode more
	 * than just the first line, unless the amount of data is really
	 * too small to hold "<?xml version="1.0" encoding="foo"
	 */
	if((ctxt->input->end - ctxt->input->cur) < 35) {
		GROW;
	}
	if((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
		// Note that we will switch encoding on the fly.
		xmlParseXMLDecl(ctxt);
		if(ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING)
			return -1; // The XML REC instructs us to stop parsing right here
		ctxt->standalone = ctxt->input->standalone;
		SKIP_BLANKS;
	}
	else {
		ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
	}
	if(ctxt->sax && ctxt->sax->startDocument && !ctxt->disableSAX)
		ctxt->sax->startDocument(ctxt->userData);
	if(ctxt->instate == XML_PARSER_EOF)
		return -1;
	if(ctxt->myDoc && ctxt->input && ctxt->input->buf && ctxt->input->buf->compressed >= 0) {
		ctxt->myDoc->compression = ctxt->input->buf->compressed;
	}
	/*
	 * The Misc part of the Prolog
	 */
	GROW;
	xmlParseMisc(ctxt);
	/*
	 * Then possibly doc type declaration(s) and more Misc (doctypedecl Misc*)?
	 */
	GROW;
	if(CMP9(CUR_PTR, '<', '!', 'D', 'O', 'C', 'T', 'Y', 'P', 'E')) {
		ctxt->inSubset = 1;
		xmlParseDocTypeDecl(ctxt);
		if(RAW == '[') {
			ctxt->instate = XML_PARSER_DTD;
			xmlParseInternalSubset(ctxt);
			if(ctxt->instate == XML_PARSER_EOF)
				return -1;
		}
		/*
		 * Create and update the external subset.
		 */
		ctxt->inSubset = 2;
		if(ctxt->sax && ctxt->sax->externalSubset && !ctxt->disableSAX)
			ctxt->sax->externalSubset(ctxt->userData, ctxt->intSubName, ctxt->extSubSystem, ctxt->extSubURI);
		if(ctxt->instate == XML_PARSER_EOF)
			return -1;
		ctxt->inSubset = 0;
		xmlCleanSpecialAttr(ctxt);
		ctxt->instate = XML_PARSER_PROLOG;
		xmlParseMisc(ctxt);
	}
	/*
	 * Time to start parsing the tree itself
	 */
	GROW;
	if(RAW != '<') {
		xmlFatalErrMsg(ctxt, XML_ERR_DOCUMENT_EMPTY, "Start tag expected, '<' not found\n");
	}
	else {
		ctxt->instate = XML_PARSER_CONTENT;
		xmlParseElement(ctxt);
		ctxt->instate = XML_PARSER_EPILOG;
		/*
		 * The Misc part at the end
		 */
		xmlParseMisc(ctxt);
		if(RAW != 0) {
			xmlFatalErr(ctxt, XML_ERR_DOCUMENT_END, 0);
		}
		ctxt->instate = XML_PARSER_EOF;
	}
	/*
	 * SAX: end of the document processing.
	 */
	if(ctxt->sax && ctxt->sax->endDocument)
		ctxt->sax->endDocument(ctxt->userData);
	/*
	 * Remove locally kept entity definitions if the tree was not built
	 */
	if(ctxt->myDoc && sstreq(ctxt->myDoc->version, SAX_COMPAT_MODE)) {
		xmlFreeDoc(ctxt->myDoc);
		ctxt->myDoc = NULL;
	}
	if(ctxt->wellFormed && ctxt->myDoc) {
		ctxt->myDoc->properties |= XML_DOC_WELLFORMED;
		if(ctxt->valid)
			ctxt->myDoc->properties |= XML_DOC_DTDVALID;
		if(ctxt->nsWellFormed)
			ctxt->myDoc->properties |= XML_DOC_NSVALID;
		if(ctxt->options & XML_PARSE_OLD10)
			ctxt->myDoc->properties |= XML_DOC_OLD10;
	}
	if(!ctxt->wellFormed) {
		ctxt->valid = 0;
		return -1;
	}
	else
		return 0;
}

/**
 * xmlParseExtParsedEnt:
 * @ctxt:  an XML parser context
 *
 * parse a general parsed entity
 * An external general parsed entity is well-formed if it matches the
 * production labeled extParsedEnt.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * Returns 0, -1 in case of error. the parser context is augmented
 *                as a result of the parsing.
 */

int xmlParseExtParsedEnt(xmlParserCtxt * ctxt)
{
	xmlChar start[4];
	xmlCharEncoding enc;
	if(!ctxt || (ctxt->input == NULL))
		return -1;
	xmlDefaultSAXHandlerInit();
	xmlDetectSAX2(ctxt);
	GROW;
	/*
	 * SAX: beginning of the document processing.
	 */
	if(ctxt->sax && (ctxt->sax->setDocumentLocator))
		ctxt->sax->setDocumentLocator(ctxt->userData, &xmlDefaultSAXLocator);
	/*
	 * Get the 4 first bytes and decode the charset
	 * if enc != XML_CHAR_ENCODING_NONE
	 * plug some encoding conversion routines.
	 */
	if((ctxt->input->end - ctxt->input->cur) >= 4) {
		start[0] = RAW;
		start[1] = NXT(1);
		start[2] = NXT(2);
		start[3] = NXT(3);
		enc = xmlDetectCharEncoding(start, 4);
		if(enc != XML_CHAR_ENCODING_NONE) {
			xmlSwitchEncoding(ctxt, enc);
		}
	}
	if(CUR == 0) {
		xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, 0);
	}
	/*
	 * Check for the XMLDecl in the Prolog.
	 */
	GROW;
	if((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
		/*
		 * Note that we will switch encoding on the fly.
		 */
		xmlParseXMLDecl(ctxt);
		if(ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
			/*
			 * The XML REC instructs us to stop parsing right here
			 */
			return -1;
		}
		SKIP_BLANKS;
	}
	else {
		ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
	}
	if(ctxt->sax && (ctxt->sax->startDocument) && (!ctxt->disableSAX))
		ctxt->sax->startDocument(ctxt->userData);
	if(ctxt->instate == XML_PARSER_EOF)
		return -1;
	/*
	 * Doing validity checking on chunk doesn't make sense
	 */
	ctxt->instate = XML_PARSER_CONTENT;
	ctxt->validate = 0;
	ctxt->loadsubset = 0;
	ctxt->depth = 0;
	xmlParseContent(ctxt);
	if(ctxt->instate == XML_PARSER_EOF)
		return -1;
	if((RAW == '<') && (NXT(1) == '/')) {
		xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
	}
	else if(RAW != 0) {
		xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, 0);
	}
	/*
	 * SAX: end of the document processing.
	 */
	if(ctxt->sax && ctxt->sax->endDocument)
		ctxt->sax->endDocument(ctxt->userData);
	return ctxt->wellFormed ? 0 : -1;
}

#ifdef LIBXML_PUSH_ENABLED
/************************************************************************
*									*
*		Progressive parsing interfaces				*
*									*
************************************************************************/

/**
 * xmlParseLookupSequence:
 * @ctxt:  an XML parser context
 * @first:  the first char to lookup
 * @next:  the next char to lookup or zero
 * @third:  the next char to lookup or zero
 *
 * Try to find if a sequence (first, next, third) or  just (first next) or
 * (first) is available in the input stream.
 * This function has a side effect of (possibly) incrementing ctxt->checkIndex
 * to avoid rescanning sequences of bytes, it DOES change the state of the
 * parser, do not use liberally.
 *
 * Returns the index to the current parsing point if the full sequence
 *      is available, -1 otherwise.
 */
static int xmlParseLookupSequence(xmlParserCtxt * ctxt, xmlChar first, xmlChar next, xmlChar third)
{
	int base, len;
	const xmlChar * buf;
	xmlParserInputPtr in = ctxt->input;
	if(!in)
		return -1;
	base = in->cur - in->base;
	if(base < 0)
		return -1;
	if(ctxt->CheckIndex > base)
		base = ctxt->CheckIndex;
	if(in->buf == NULL) {
		buf = in->base;
		len = in->length;
	}
	else {
		buf = xmlBufContent(in->buf->buffer);
		len = xmlBufUse(in->buf->buffer);
	}
	/* take into account the sequence length */
	if(third)
		len -= 2;
	else if(next)
		len--;
	for(; base < len; base++) {
		if(buf[base] == first) {
			if(third != 0) {
				if((buf[base + 1] != next) || (buf[base + 2] != third))
					continue;
			}
			else if(next != 0) {
				if(buf[base + 1] != next)
					continue;
			}
			ctxt->CheckIndex = 0;
#ifdef DEBUG_PUSH
			if(next == 0)
				xmlGenericError(0, "PP: lookup '%c' found at %d\n", first, base);
			else if(third == 0)
				xmlGenericError(0, "PP: lookup '%c%c' found at %d\n", first, next, base);
			else
				xmlGenericError(0, "PP: lookup '%c%c%c' found at %d\n", first, next, third, base);
#endif
			return(base - (in->cur - in->base));
		}
	}
	ctxt->CheckIndex = base;
#ifdef DEBUG_PUSH
	if(next == 0)
		xmlGenericError(0, "PP: lookup '%c' failed\n", first);
	else if(third == 0)
		xmlGenericError(0, "PP: lookup '%c%c' failed\n", first, next);
	else
		xmlGenericError(0, "PP: lookup '%c%c%c' failed\n", first, next, third);
#endif
	return -1;
}

/**
 * xmlParseGetLasts:
 * @ctxt:  an XML parser context
 * @lastlt:  pointer to store the last '<' from the input
 * @lastgt:  pointer to store the last '>' from the input
 *
 * Lookup the last < and > in the current chunk
 */
static void xmlParseGetLasts(xmlParserCtxt * ctxt, const xmlChar ** lastlt, const xmlChar ** lastgt)
{
	const xmlChar * tmp;
	if(!ctxt || (lastlt == NULL) || (lastgt == NULL)) {
		xmlGenericError(0, "Internal error: xmlParseGetLasts\n");
		return;
	}
	if((ctxt->progressive != 0) && (ctxt->inputNr == 1)) {
		tmp = ctxt->input->end;
		tmp--;
		while((tmp >= ctxt->input->base) && (*tmp != '<'))
			tmp--;
		if(tmp < ctxt->input->base) {
			*lastlt = NULL;
			*lastgt = NULL;
		}
		else {
			*lastlt = tmp;
			tmp++;
			while((tmp < ctxt->input->end) && (*tmp != '>')) {
				if(*tmp == '\'') {
					tmp++;
					while((tmp < ctxt->input->end) && (*tmp != '\''))
						tmp++;
					if(tmp < ctxt->input->end)
						tmp++;
				}
				else if(*tmp == '"') {
					tmp++;
					while((tmp < ctxt->input->end) && (*tmp != '"'))
						tmp++;
					if(tmp < ctxt->input->end)
						tmp++;
				}
				else
					tmp++;
			}
			if(tmp < ctxt->input->end)
				*lastgt = tmp;
			else {
				tmp = *lastlt;
				tmp--;
				while((tmp >= ctxt->input->base) && (*tmp != '>'))
					tmp--;
				if(tmp >= ctxt->input->base)
					*lastgt = tmp;
				else
					*lastgt = NULL;
			}
		}
	}
	else {
		*lastlt = NULL;
		*lastgt = NULL;
	}
}

/**
 * xmlCheckCdataPush:
 * @cur: pointer to the bock of characters
 * @len: length of the block in bytes
 *
 * Check that the block of characters is okay as SCdata content [20]
 *
 * Returns the number of bytes to pass if okay, a negative index where an
 *         UTF-8 error occured otherwise
 */
static int xmlCheckCdataPush(const xmlChar * utf, int len)
{
	int ix;
	uchar c;
	int codepoint;
	if((utf == NULL) || (len <= 0))
		return 0;
	for(ix = 0; ix < len; ) {  /* string is 0-terminated */
		c = utf[ix];
		if((c & 0x80) == 0x00) { /* 1-byte code, starts with 10 */
			if(c >= 0x20)
				ix++;
			else if((c == 0xA) || (c == 0xD) || (c == 0x9))
				ix++;
			else
				return -ix;
		}
		else if((c & 0xe0) == 0xc0) { /* 2-byte code, starts with 110 */
			if(ix + 2 > len)
				return ix;
			if((utf[ix+1] & 0xc0 ) != 0x80)
				return -ix;
			codepoint = (utf[ix] & 0x1f) << 6;
			codepoint |= utf[ix+1] & 0x3f;
			if(!xmlIsCharQ(codepoint))
				return -ix;
			ix += 2;
		}
		else if((c & 0xf0) == 0xe0) { /* 3-byte code, starts with 1110 */
			if(ix + 3 > len)
				return ix;
			if(((utf[ix+1] & 0xc0) != 0x80) || ((utf[ix+2] & 0xc0) != 0x80))
				return -ix;
			codepoint = (utf[ix] & 0xf) << 12;
			codepoint |= (utf[ix+1] & 0x3f) << 6;
			codepoint |= utf[ix+2] & 0x3f;
			if(!xmlIsCharQ(codepoint))
				return -ix;
			ix += 3;
		}
		else if((c & 0xf8) == 0xf0) { /* 4-byte code, starts with 11110 */
			if(ix + 4 > len) return ix;
			if(((utf[ix+1] & 0xc0) != 0x80) || ((utf[ix+2] & 0xc0) != 0x80) || ((utf[ix+3] & 0xc0) != 0x80))
				return -ix;
			codepoint = (utf[ix] & 0x7) << 18;
			codepoint |= (utf[ix+1] & 0x3f) << 12;
			codepoint |= (utf[ix+2] & 0x3f) << 6;
			codepoint |= utf[ix+3] & 0x3f;
			if(!xmlIsCharQ(codepoint))
				return -ix;
			ix += 4;
		}
		else                    /* unknown encoding */
			return -ix;
	}
	return ix;
}

/**
 * xmlParseTryOrFinish:
 * @ctxt:  an XML parser context
 * @terminate:  last chunk indicator
 *
 * Try to progress on parsing
 *
 * Returns zero if no parsing was possible
 */
static int xmlParseTryOrFinish(xmlParserCtxt * ctxt, int terminate)
{
	int ret = 0;
	int avail, tlen;
	xmlChar cur, next;
	const xmlChar * lastlt, * lastgt;
	if(ctxt->input == NULL)
		return 0;
#ifdef DEBUG_PUSH
	switch(ctxt->instate) {
		case XML_PARSER_EOF: xmlGenericError(0, "PP: try EOF\n"); break;
		case XML_PARSER_START: xmlGenericError(0, "PP: try START\n"); break;
		case XML_PARSER_MISC: xmlGenericError(0, "PP: try MISC\n"); break;
		case XML_PARSER_COMMENT: xmlGenericError(0, "PP: try COMMENT\n"); break;
		case XML_PARSER_PROLOG: xmlGenericError(0, "PP: try PROLOG\n"); break;
		case XML_PARSER_START_TAG: xmlGenericError(0, "PP: try START_TAG\n"); break;
		case XML_PARSER_CONTENT: xmlGenericError(0, "PP: try CONTENT\n"); break;
		case XML_PARSER_CDATA_SECTION: xmlGenericError(0, "PP: try CDATA_SECTION\n"); break;
		case XML_PARSER_END_TAG: xmlGenericError(0, "PP: try END_TAG\n"); break;
		case XML_PARSER_ENTITY_DECL: xmlGenericError(0, "PP: try ENTITY_DECL\n"); break;
		case XML_PARSER_ENTITY_VALUE: xmlGenericError(0, "PP: try ENTITY_VALUE\n"); break;
		case XML_PARSER_ATTRIBUTE_VALUE: xmlGenericError(0, "PP: try ATTRIBUTE_VALUE\n"); break;
		case XML_PARSER_DTD: xmlGenericError(0, "PP: try DTD\n"); break;
		case XML_PARSER_EPILOG: xmlGenericError(0, "PP: try EPILOG\n"); break;
		case XML_PARSER_PI: xmlGenericError(0, "PP: try PI\n"); break;
		case XML_PARSER_IGNORE: xmlGenericError(0, "PP: try IGNORE\n"); break;
	}
#endif
	if(ctxt->input && (ctxt->input->cur - ctxt->input->base > 4096)) {
		xmlSHRINK(ctxt);
		ctxt->CheckIndex = 0;
	}
	xmlParseGetLasts(ctxt, &lastlt, &lastgt);
	while(ctxt->instate != XML_PARSER_EOF) {
		if((ctxt->errNo != XML_ERR_OK) && (ctxt->disableSAX == 1))
			return 0;
		/*
		 * Pop-up of finished entities.
		 */
		while((RAW == 0) && (ctxt->inputNr > 1))
			xmlPopInput(ctxt);
		if(ctxt->input == NULL) 
			break;
		if(ctxt->input->buf == NULL)
			avail = ctxt->input->length - (ctxt->input->cur - ctxt->input->base);
		else {
			/*
			 * If we are operating on converted input, try to flush
			 * remainng chars to avoid them stalling in the non-converted
			 * buffer. But do not do this in document start where
			 * encoding="..." may not have been read and we work on a
			 * guessed encoding.
			 */
			if((ctxt->instate != XML_PARSER_START) && (ctxt->input->buf->raw != NULL) && (xmlBufIsEmpty(ctxt->input->buf->raw) == 0)) {
				size_t base = xmlBufGetInputBase(ctxt->input->buf->buffer, ctxt->input);
				size_t current = ctxt->input->cur - ctxt->input->base;
				xmlParserInputBufferPush(ctxt->input->buf, 0, "");
				xmlBufSetInputBaseCur(ctxt->input->buf->buffer, ctxt->input, base, current);
			}
			avail = xmlBufUse(ctxt->input->buf->buffer) - (ctxt->input->cur - ctxt->input->base);
		}
		if(avail < 1)
			goto done;
		switch(ctxt->instate) {
			case XML_PARSER_EOF:
			    /*
			     * Document parsing is done !
			     */
			    goto done;
			case XML_PARSER_START:
			    if(ctxt->charset == XML_CHAR_ENCODING_NONE) {
				    xmlChar start[4];
				    xmlCharEncoding enc;
				    /*
				     * Very first chars read from the document flow.
				     */
				    if(avail < 4)
					    goto done;
				    /*
				     * Get the 4 first bytes and decode the charset
				     * if enc != XML_CHAR_ENCODING_NONE
				     * plug some encoding conversion routines,
				     * else xmlSwitchEncoding will set to (default)
				     * UTF8.
				     */
				    start[0] = RAW;
				    start[1] = NXT(1);
				    start[2] = NXT(2);
				    start[3] = NXT(3);
				    enc = xmlDetectCharEncoding(start, 4);
				    xmlSwitchEncoding(ctxt, enc);
				    break;
			    }
			    if(avail < 2)
				    goto done;
			    cur = ctxt->input->cur[0];
			    next = ctxt->input->cur[1];
			    if(cur == 0) {
				    if(ctxt->sax && (ctxt->sax->setDocumentLocator))
					    ctxt->sax->setDocumentLocator(ctxt->userData, &xmlDefaultSAXLocator);
				    xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, 0);
				    ctxt->instate = XML_PARSER_EOF;
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: entering EOF\n");
#endif
				    if(ctxt->sax && ctxt->sax->endDocument)
					    ctxt->sax->endDocument(ctxt->userData);
				    goto done;
			    }
			    if((cur == '<') && (next == '?')) {
				    /* PI or XML decl */
				    if(avail < 5) return ret;
				    if((!terminate) &&
				    (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0))
					    return ret;
				    if(ctxt->sax && (ctxt->sax->setDocumentLocator))
					    ctxt->sax->setDocumentLocator(ctxt->userData,
					    &xmlDefaultSAXLocator);
				    if((ctxt->input->cur[2] == 'x') &&
				    (ctxt->input->cur[3] == 'm') &&
				    (ctxt->input->cur[4] == 'l') &&
				    (IS_BLANK_CH(ctxt->input->cur[5]))) {
					    ret += 5;
#ifdef DEBUG_PUSH
					    xmlGenericError(0, "PP: Parsing XML Decl\n");
#endif
					    xmlParseXMLDecl(ctxt);
					    if(ctxt->errNo == XML_ERR_UNSUPPORTED_ENCODING) {
						    /*
						     * The XML REC instructs us to stop parsing right
						     * here
						     */
						    ctxt->instate = XML_PARSER_EOF;
						    return 0;
					    }
					    ctxt->standalone = ctxt->input->standalone;
					    if(!ctxt->encoding && ctxt->input->encoding)
						    ctxt->encoding = sstrdup(ctxt->input->encoding);
					    if(ctxt->sax && (ctxt->sax->startDocument) && (!ctxt->disableSAX))
						    ctxt->sax->startDocument(ctxt->userData);
					    ctxt->instate = XML_PARSER_MISC;
#ifdef DEBUG_PUSH
					    xmlGenericError(0, "PP: entering MISC\n");
#endif
				    }
				    else {
					    ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
					    if(ctxt->sax && (ctxt->sax->startDocument) && (!ctxt->disableSAX))
						    ctxt->sax->startDocument(ctxt->userData);
					    ctxt->instate = XML_PARSER_MISC;
#ifdef DEBUG_PUSH
					    xmlGenericError(0, "PP: entering MISC\n");
#endif
				    }
			    }
			    else {
				    if(ctxt->sax && (ctxt->sax->setDocumentLocator))
					    ctxt->sax->setDocumentLocator(ctxt->userData, &xmlDefaultSAXLocator);
				    ctxt->version = xmlCharStrdup(XML_DEFAULT_VERSION);
				    if(!ctxt->version) {
					    xmlErrMemory(ctxt, 0);
					    break;
				    }
				    if(ctxt->sax && ctxt->sax->startDocument && !ctxt->disableSAX)
					    ctxt->sax->startDocument(ctxt->userData);
				    ctxt->instate = XML_PARSER_MISC;
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: entering MISC\n");
#endif
			    }
			    break;
			case XML_PARSER_START_TAG: {
			    const xmlChar * name;
			    const xmlChar * prefix = NULL;
			    const xmlChar * URI = NULL;
			    int nsNr = ctxt->nsNr;
			    if((avail < 2) && (ctxt->inputNr == 1))
				    goto done;
			    cur = ctxt->input->cur[0];
			    if(cur != '<') {
				    xmlFatalErr(ctxt, XML_ERR_DOCUMENT_EMPTY, 0);
				    ctxt->instate = XML_PARSER_EOF;
				    if(ctxt->sax && ctxt->sax->endDocument)
					    ctxt->sax->endDocument(ctxt->userData);
				    goto done;
			    }
			    if(!terminate) {
				    if(ctxt->progressive) {
					    /* > can be found unescaped in attribute values */
					    if((lastgt == NULL) || (ctxt->input->cur >= lastgt))
						    goto done;
				    }
				    else if(xmlParseLookupSequence(ctxt, '>', 0, 0) < 0) {
					    goto done;
				    }
			    }
			    if(ctxt->spaceNr == 0)
				    spacePush(ctxt, -1);
			    else if(*ctxt->space == -2)
				    spacePush(ctxt, -1);
			    else
				    spacePush(ctxt, *ctxt->space);
#ifdef LIBXML_SAX1_ENABLED
			    if(ctxt->sax2)
#endif /* LIBXML_SAX1_ENABLED */
			    name = xmlParseStartTag2(ctxt, &prefix, &URI, &tlen);
#ifdef LIBXML_SAX1_ENABLED
			    else
				    name = xmlParseStartTag(ctxt);
#endif /* LIBXML_SAX1_ENABLED */
			    if(ctxt->instate == XML_PARSER_EOF)
				    goto done;
			    if(!name) {
				    spacePop(ctxt);
				    ctxt->instate = XML_PARSER_EOF;
				    if(ctxt->sax && ctxt->sax->endDocument)
					    ctxt->sax->endDocument(ctxt->userData);
				    goto done;
			    }
#ifdef LIBXML_VALID_ENABLED
			    /*
			     * [VC: Root Element Type]
			     * The Name in the document type declaration must match
			     * the element type of the root element.
			     */
			    if(ctxt->validate && ctxt->wellFormed && ctxt->myDoc &&
				    ctxt->P_Node && (ctxt->P_Node == ctxt->myDoc->children))
				    ctxt->valid &= xmlValidateRoot(&ctxt->vctxt, ctxt->myDoc);
#endif /* LIBXML_VALID_ENABLED */

			    /*
			     * Check for an Empty Element.
			     */
			    if((RAW == '/') && (NXT(1) == '>')) {
				    SKIP(2);
				    if(ctxt->sax2) {
					    if(ctxt->sax && ctxt->sax->endElementNs && (!ctxt->disableSAX))
						    ctxt->sax->endElementNs(ctxt->userData, name, prefix, URI);
					    if(ctxt->nsNr - nsNr > 0)
						    nsPop(ctxt, ctxt->nsNr - nsNr);
#ifdef LIBXML_SAX1_ENABLED
				    }
				    else {
					    if(ctxt->sax && ctxt->sax->endElement && (!ctxt->disableSAX))
						    ctxt->sax->endElement(ctxt->userData, name);
#endif /* LIBXML_SAX1_ENABLED */
				    }
				    if(ctxt->instate == XML_PARSER_EOF)
					    goto done;
				    spacePop(ctxt);
				    if(ctxt->nameNr == 0) {
					    ctxt->instate = XML_PARSER_EPILOG;
				    }
				    else {
					    ctxt->instate = XML_PARSER_CONTENT;
				    }
				    ctxt->progressive = 1;
				    break;
			    }
			    if(RAW == '>') {
				    NEXT;
			    }
			    else {
				    xmlFatalErrMsgStr(ctxt, XML_ERR_GT_REQUIRED, "Couldn't find end of Start Tag %s\n", name);
				    nodePop(ctxt);
				    spacePop(ctxt);
			    }
			    if(ctxt->sax2)
				    nameNsPush(ctxt, name, prefix, URI, ctxt->nsNr - nsNr);
#ifdef LIBXML_SAX1_ENABLED
			    else
				    namePush(ctxt, name);
#endif /* LIBXML_SAX1_ENABLED */

			    ctxt->instate = XML_PARSER_CONTENT;
			    ctxt->progressive = 1;
			    break;
		    }
			case XML_PARSER_CONTENT: {
			    const xmlChar * test;
			    uint cons;
			    if((avail < 2) && (ctxt->inputNr == 1))
				    goto done;
			    cur = ctxt->input->cur[0];
			    next = ctxt->input->cur[1];

			    test = CUR_PTR;
			    cons = ctxt->input->consumed;
			    if((cur == '<') && (next == '/')) {
				    ctxt->instate = XML_PARSER_END_TAG;
				    break;
			    }
			    else if((cur == '<') && (next == '?')) {
				    if(!terminate && (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0)) {
					    ctxt->progressive = XML_PARSER_PI;
					    goto done;
				    }
				    xmlParsePI(ctxt);
				    ctxt->instate = XML_PARSER_CONTENT;
				    ctxt->progressive = 1;
			    }
			    else if((cur == '<') && (next != '!')) {
				    ctxt->instate = XML_PARSER_START_TAG;
				    break;
			    }
			    else if((cur == '<') && (next == '!') && (ctxt->input->cur[2] == '-') && (ctxt->input->cur[3] == '-')) {
				    int term;
				    if(avail < 4)
					    goto done;
				    ctxt->input->cur += 4;
				    term = xmlParseLookupSequence(ctxt, '-', '-', '>');
				    ctxt->input->cur -= 4;
				    if((!terminate) && (term < 0)) {
					    ctxt->progressive = XML_PARSER_COMMENT;
					    goto done;
				    }
				    xmlParseComment(ctxt);
				    ctxt->instate = XML_PARSER_CONTENT;
				    ctxt->progressive = 1;
			    }
			    else if((cur == '<') && (ctxt->input->cur[1] == '!') &&
				    (ctxt->input->cur[2] == '[') &&
				    (ctxt->input->cur[3] == 'C') &&
				    (ctxt->input->cur[4] == 'D') &&
				    (ctxt->input->cur[5] == 'A') &&
				    (ctxt->input->cur[6] == 'T') &&
				    (ctxt->input->cur[7] == 'A') &&
				    (ctxt->input->cur[8] == '[')) {
				    SKIP(9);
				    ctxt->instate = XML_PARSER_CDATA_SECTION;
				    break;
			    }
			    else if((cur == '<') && (next == '!') && (avail < 9)) {
				    goto done;
			    }
			    else if(cur == '&') {
				    if((!terminate) && (xmlParseLookupSequence(ctxt, ';', 0, 0) < 0))
					    goto done;
				    xmlParseReference(ctxt);
			    }
			    else {
				    /* @todo Avoid the extra copy, handle directly !!! */
				    /*
				     * Goal of the following test is:
				     *  - minimize calls to the SAX 'character' callback
				     *    when they are mergeable
				     *  - handle an problem for isBlank when we only parse
				     *    a sequence of blank chars and the next one is
				     *    not available to check against '<' presence.
				     *  - tries to homogenize the differences in SAX
				     *    callbacks between the push and pull versions
				     *    of the parser.
				     */
				    if((ctxt->inputNr == 1) && (avail < XML_PARSER_BIG_BUFFER_SIZE)) {
					    if(!terminate) {
						    if(ctxt->progressive) {
							    if((lastlt == NULL) || (ctxt->input->cur > lastlt))
								    goto done;
						    }
						    else if(xmlParseLookupSequence(ctxt, '<', 0, 0) < 0) {
							    goto done;
						    }
					    }
				    }
				    ctxt->CheckIndex = 0;
				    xmlParseCharData(ctxt, 0);
			    }
			    /*
			     * Pop-up of finished entities.
			     */
			    while((RAW == 0) && (ctxt->inputNr > 1))
				    xmlPopInput(ctxt);
			    if((cons == ctxt->input->consumed) && (test == CUR_PTR)) {
				    xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "detected an error in element content\n");
				    ctxt->instate = XML_PARSER_EOF;
				    break;
			    }
			    break;
		    }
			case XML_PARSER_END_TAG:
			    if(avail < 2)
				    goto done;
			    if(!terminate) {
				    if(ctxt->progressive) {
					    /* > can be found unescaped in attribute values */
					    if((lastgt == NULL) || (ctxt->input->cur >= lastgt))
						    goto done;
				    }
				    else if(xmlParseLookupSequence(ctxt, '>', 0, 0) < 0) {
					    goto done;
				    }
			    }
			    if(ctxt->sax2) {
				    xmlParseEndTag2(ctxt, (xmlChar *)ctxt->pushTab[ctxt->nameNr * 3 - 3],
						(xmlChar *)ctxt->pushTab[ctxt->nameNr * 3 - 2], 0, (int)(long)ctxt->pushTab[ctxt->nameNr * 3 - 1], 0);
				    nameNsPop(ctxt);
			    }
#ifdef LIBXML_SAX1_ENABLED
			    else
				    xmlParseEndTag1(ctxt, 0);
#endif /* LIBXML_SAX1_ENABLED */
			    if(ctxt->instate == XML_PARSER_EOF) {
				    /* Nothing */
			    }
			    else if(ctxt->nameNr == 0) {
				    ctxt->instate = XML_PARSER_EPILOG;
			    }
			    else {
				    ctxt->instate = XML_PARSER_CONTENT;
			    }
			    break;
			case XML_PARSER_CDATA_SECTION: {
			    /*
			     * The Push mode need to have the SAX callback for
			     * cdataBlock merge back contiguous callbacks.
			     */
			    int base;

			    base = xmlParseLookupSequence(ctxt, ']', ']', '>');
			    if(base < 0) {
				    if(avail >= XML_PARSER_BIG_BUFFER_SIZE + 2) {
					    int tmp = xmlCheckCdataPush(ctxt->input->cur, XML_PARSER_BIG_BUFFER_SIZE);
					    if(tmp < 0) {
						    tmp = -tmp;
						    ctxt->input->cur += tmp;
						    goto encoding_error;
					    }
					    if(ctxt->sax && (!ctxt->disableSAX)) {
						    if(ctxt->sax->cdataBlock)
							    ctxt->sax->cdataBlock(ctxt->userData, ctxt->input->cur, tmp);
						    else if(ctxt->sax->characters)
							    ctxt->sax->characters(ctxt->userData, ctxt->input->cur, tmp);
					    }
					    if(ctxt->instate == XML_PARSER_EOF)
						    goto done;
					    SKIPL(tmp);
					    ctxt->CheckIndex = 0;
				    }
				    goto done;
			    }
			    else {
				    int tmp = xmlCheckCdataPush(ctxt->input->cur, base);
				    if((tmp < 0) || (tmp != base)) {
					    tmp = -tmp;
					    ctxt->input->cur += tmp;
					    goto encoding_error;
				    }
				    if(ctxt->sax && (base == 0) && (ctxt->sax->cdataBlock != NULL) && (!ctxt->disableSAX)) {
					    /*
					     * Special case to provide identical behaviour
					     * between pull and push parsers on enpty CDATA
					     * sections
					     */
					    if((ctxt->input->cur - ctxt->input->base >= 9) && (!strncmp((const char*)&ctxt->input->cur[-9], "<![CDATA[", 9)))
						    ctxt->sax->cdataBlock(ctxt->userData, BAD_CAST "", 0);
				    }
				    else if(ctxt->sax && (base > 0) && (!ctxt->disableSAX)) {
					    if(ctxt->sax->cdataBlock)
						    ctxt->sax->cdataBlock(ctxt->userData, ctxt->input->cur, base);
					    else if(ctxt->sax->characters)
						    ctxt->sax->characters(ctxt->userData, ctxt->input->cur, base);
				    }
				    if(ctxt->instate == XML_PARSER_EOF)
					    goto done;
				    SKIPL(base + 3);
				    ctxt->CheckIndex = 0;
				    ctxt->instate = XML_PARSER_CONTENT;
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: entering CONTENT\n");
#endif
			    }
			    break;
		    }
			case XML_PARSER_MISC:
			    SKIP_BLANKS;
			    if(ctxt->input->buf == NULL)
				    avail = ctxt->input->length - (ctxt->input->cur - ctxt->input->base);
			    else
				    avail = xmlBufUse(ctxt->input->buf->buffer) - (ctxt->input->cur - ctxt->input->base);
			    if(avail < 2)
				    goto done;
			    cur = ctxt->input->cur[0];
			    next = ctxt->input->cur[1];
			    if((cur == '<') && (next == '?')) {
				    if((!terminate) && (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0)) {
					    ctxt->progressive = XML_PARSER_PI;
					    goto done;
				    }
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: Parsing PI\n");
#endif
				    xmlParsePI(ctxt);
				    if(ctxt->instate == XML_PARSER_EOF)
					    goto done;
				    ctxt->instate = XML_PARSER_MISC;
				    ctxt->progressive = 1;
				    ctxt->CheckIndex = 0;
			    }
			    else if((cur == '<') && (next == '!') && (ctxt->input->cur[2] == '-') && (ctxt->input->cur[3] == '-')) {
				    if((!terminate) && (xmlParseLookupSequence(ctxt, '-', '-', '>') < 0)) {
					    ctxt->progressive = XML_PARSER_COMMENT;
					    goto done;
				    }
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: Parsing Comment\n");
#endif
				    xmlParseComment(ctxt);
				    if(ctxt->instate == XML_PARSER_EOF)
					    goto done;
				    ctxt->instate = XML_PARSER_MISC;
				    ctxt->progressive = 1;
				    ctxt->CheckIndex = 0;
			    }
			    else if((cur == '<') && (next == '!') && (ctxt->input->cur[2] == 'D') &&
					(ctxt->input->cur[3] == 'O') && (ctxt->input->cur[4] == 'C') && (ctxt->input->cur[5] == 'T') &&
					(ctxt->input->cur[6] == 'Y') && (ctxt->input->cur[7] == 'P') && (ctxt->input->cur[8] == 'E')) {
				    if((!terminate) && (xmlParseLookupSequence(ctxt, '>', 0, 0) < 0)) {
					    ctxt->progressive = XML_PARSER_DTD;
					    goto done;
				    }
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: Parsing internal subset\n");
#endif
				    ctxt->inSubset = 1;
				    ctxt->progressive = 0;
				    ctxt->CheckIndex = 0;
				    xmlParseDocTypeDecl(ctxt);
				    if(ctxt->instate == XML_PARSER_EOF)
					    goto done;
				    if(RAW == '[') {
					    ctxt->instate = XML_PARSER_DTD;
#ifdef DEBUG_PUSH
					    xmlGenericError(0, "PP: entering DTD\n");
#endif
				    }
				    else {
					    /*
					     * Create and update the external subset.
					     */
					    ctxt->inSubset = 2;
					    if(ctxt->sax && (!ctxt->disableSAX) && (ctxt->sax->externalSubset != NULL))
						    ctxt->sax->externalSubset(ctxt->userData, ctxt->intSubName, ctxt->extSubSystem, ctxt->extSubURI);
					    ctxt->inSubset = 0;
					    xmlCleanSpecialAttr(ctxt);
					    ctxt->instate = XML_PARSER_PROLOG;
#ifdef DEBUG_PUSH
					    xmlGenericError(0, "PP: entering PROLOG\n");
#endif
				    }
			    }
			    else if((cur == '<') && (next == '!') && (avail < 9)) {
				    goto done;
			    }
			    else {
				    ctxt->instate = XML_PARSER_START_TAG;
				    ctxt->progressive = XML_PARSER_START_TAG;
				    xmlParseGetLasts(ctxt, &lastlt, &lastgt);
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: entering START_TAG\n");
#endif
			    }
			    break;
			case XML_PARSER_PROLOG:
			    SKIP_BLANKS;
			    if(ctxt->input->buf == NULL)
				    avail = ctxt->input->length - (ctxt->input->cur - ctxt->input->base);
			    else
				    avail = xmlBufUse(ctxt->input->buf->buffer) - (ctxt->input->cur - ctxt->input->base);
			    if(avail < 2)
				    goto done;
			    cur = ctxt->input->cur[0];
			    next = ctxt->input->cur[1];
			    if((cur == '<') && (next == '?')) {
				    if((!terminate) && (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0)) {
					    ctxt->progressive = XML_PARSER_PI;
					    goto done;
				    }
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: Parsing PI\n");
#endif
				    xmlParsePI(ctxt);
				    if(ctxt->instate == XML_PARSER_EOF)
					    goto done;
				    ctxt->instate = XML_PARSER_PROLOG;
				    ctxt->progressive = 1;
			    }
			    else if((cur == '<') && (next == '!') && (ctxt->input->cur[2] == '-') && (ctxt->input->cur[3] == '-')) {
				    if((!terminate) && (xmlParseLookupSequence(ctxt, '-', '-', '>') < 0)) {
					    ctxt->progressive = XML_PARSER_COMMENT;
					    goto done;
				    }
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: Parsing Comment\n");
#endif
				    xmlParseComment(ctxt);
				    if(ctxt->instate == XML_PARSER_EOF)
					    goto done;
				    ctxt->instate = XML_PARSER_PROLOG;
				    ctxt->progressive = 1;
			    }
			    else if((cur == '<') && (next == '!') && (avail < 4)) {
				    goto done;
			    }
			    else {
				    ctxt->instate = XML_PARSER_START_TAG;
					SETIFZ(ctxt->progressive, XML_PARSER_START_TAG);
				    xmlParseGetLasts(ctxt, &lastlt, &lastgt);
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: entering START_TAG\n");
#endif
			    }
			    break;
			case XML_PARSER_EPILOG:
			    SKIP_BLANKS;
			    if(ctxt->input->buf == NULL)
				    avail = ctxt->input->length - (ctxt->input->cur - ctxt->input->base);
			    else
				    avail = xmlBufUse(ctxt->input->buf->buffer) - (ctxt->input->cur - ctxt->input->base);
			    if(avail < 2)
				    goto done;
			    cur = ctxt->input->cur[0];
			    next = ctxt->input->cur[1];
			    if((cur == '<') && (next == '?')) {
				    if((!terminate) && (xmlParseLookupSequence(ctxt, '?', '>', 0) < 0)) {
					    ctxt->progressive = XML_PARSER_PI;
					    goto done;
				    }
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: Parsing PI\n");
#endif
				    xmlParsePI(ctxt);
				    if(ctxt->instate == XML_PARSER_EOF)
					    goto done;
				    ctxt->instate = XML_PARSER_EPILOG;
				    ctxt->progressive = 1;
			    }
			    else if((cur == '<') && (next == '!') && (ctxt->input->cur[2] == '-') && (ctxt->input->cur[3] == '-')) {
				    if((!terminate) && (xmlParseLookupSequence(ctxt, '-', '-', '>') < 0)) {
					    ctxt->progressive = XML_PARSER_COMMENT;
					    goto done;
				    }
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: Parsing Comment\n");
#endif
				    xmlParseComment(ctxt);
				    if(ctxt->instate == XML_PARSER_EOF)
					    goto done;
				    ctxt->instate = XML_PARSER_EPILOG;
				    ctxt->progressive = 1;
			    }
			    else if((cur == '<') && (next == '!') && (avail < 4)) {
				    goto done;
			    }
			    else {
				    xmlFatalErr(ctxt, XML_ERR_DOCUMENT_END, 0);
				    ctxt->instate = XML_PARSER_EOF;
#ifdef DEBUG_PUSH
				    xmlGenericError(0, "PP: entering EOF\n");
#endif
				    if(ctxt->sax && ctxt->sax->endDocument)
					    ctxt->sax->endDocument(ctxt->userData);
				    goto done;
			    }
			    break;
			case XML_PARSER_DTD: {
			    /*
			     * Sorry but progressive parsing of the internal subset
			     * is not expected to be supported. We first check that
			     * the full content of the internal subset is available and
			     * the parsing is launched only at that point.
			     * Internal subset ends up with "']' S? '>'" in an unescaped
			     * section and not in a ']]>' sequence which are conditional
			     * sections (whoever argued to keep that crap in XML deserve
			     * a place in hell !).
			     */
			    int i;
			    xmlChar * buf;
			    xmlChar quote = 0;
			    size_t use;
			    int base = ctxt->input->cur - ctxt->input->base;
			    if(base < 0)
					return 0;
			    if(ctxt->CheckIndex > base)
				    base = ctxt->CheckIndex;
			    buf = xmlBufContent(ctxt->input->buf->buffer);
			    use = xmlBufUse(ctxt->input->buf->buffer);
			    for(; (uint)base < use; base++) {
				    if(quote != 0) {
					    if(buf[base] == quote)
						    quote = 0;
					    continue;
				    }
				    if((quote == 0) && (buf[base] == '<')) {
					    int found  = 0;
					    /* special handling of comments */
					    if(((uint)base + 4 < use) && (buf[base + 1] == '!') && (buf[base + 2] == '-') && (buf[base + 3] == '-')) {
						    for(; (uint)base + 3 < use; base++) {
							    if((buf[base] == '-') && (buf[base + 1] == '-') && (buf[base + 2] == '>')) {
								    found = 1;
								    base += 2;
								    break;
							    }
						    }
						    if(!found) {
#if 0
							    fprintf(stderr, "unfinished comment\n");
#endif
							    break; /* for */
						    }
						    continue;
					    }
				    }
				    if(buf[base] == '"') {
					    quote = '"';
					    continue;
				    }
				    if(buf[base] == '\'') {
					    quote = '\'';
					    continue;
				    }
				    if(buf[base] == ']') {
#if 0
					    fprintf(stderr, "%c%c%c%c: ", buf[base], buf[base + 1], buf[base + 2], buf[base + 3]);
#endif
					    if((uint)base +1 >= use)
						    break;
					    if(buf[base + 1] == ']') {
						    /* conditional crap, skip both ']' ! */
						    base++;
						    continue;
					    }
					    for(i = 1; (uint)base + i < use; i++) {
						    if(buf[base + i] == '>') {
#if 0
							    fprintf(stderr, "found\n");
#endif
							    goto found_end_int_subset;
						    }
						    if(!IS_BLANK_CH(buf[base + i])) {
#if 0
							    fprintf(stderr, "not found\n");
#endif
							    goto not_end_of_int_subset;
						    }
					    }
#if 0
					    fprintf(stderr, "end of stream\n");
#endif
					    break;
				    }
not_end_of_int_subset:
				    continue; /* for */
			    }
			    /*
			     * We didn't found the end of the Internal subset
			     */
				ctxt->CheckIndex = (quote == 0) ? base : 0;
#ifdef DEBUG_PUSH
			    if(next == 0)
				    xmlGenericError(0, "PP: lookup of int subset end filed\n");
#endif
			    goto done;

found_end_int_subset:
			    ctxt->CheckIndex = 0;
			    xmlParseInternalSubset(ctxt);
			    if(ctxt->instate == XML_PARSER_EOF)
				    goto done;
			    ctxt->inSubset = 2;
			    if(ctxt->sax && (!ctxt->disableSAX) && (ctxt->sax->externalSubset != NULL))
				    ctxt->sax->externalSubset(ctxt->userData, ctxt->intSubName, ctxt->extSubSystem, ctxt->extSubURI);
			    ctxt->inSubset = 0;
			    xmlCleanSpecialAttr(ctxt);
			    if(ctxt->instate == XML_PARSER_EOF)
				    goto done;
			    ctxt->instate = XML_PARSER_PROLOG;
			    ctxt->CheckIndex = 0;
#ifdef DEBUG_PUSH
			    xmlGenericError(0, "PP: entering PROLOG\n");
#endif
			    break;
		    }
			case XML_PARSER_COMMENT:
			    xmlGenericError(0, "PP: internal error, state == COMMENT\n");
			    ctxt->instate = XML_PARSER_CONTENT;
#ifdef DEBUG_PUSH
			    xmlGenericError(0, "PP: entering CONTENT\n");
#endif
			    break;
			case XML_PARSER_IGNORE:
			    xmlGenericError(0, "PP: internal error, state == IGNORE");
			    ctxt->instate = XML_PARSER_DTD;
#ifdef DEBUG_PUSH
			    xmlGenericError(0, "PP: entering DTD\n");
#endif
			    break;
			case XML_PARSER_PI:
			    xmlGenericError(0, "PP: internal error, state == PI\n");
			    ctxt->instate = XML_PARSER_CONTENT;
#ifdef DEBUG_PUSH
			    xmlGenericError(0, "PP: entering CONTENT\n");
#endif
			    break;
			case XML_PARSER_ENTITY_DECL:
			    xmlGenericError(0, "PP: internal error, state == ENTITY_DECL\n");
			    ctxt->instate = XML_PARSER_DTD;
#ifdef DEBUG_PUSH
			    xmlGenericError(0, "PP: entering DTD\n");
#endif
			    break;
			case XML_PARSER_ENTITY_VALUE:
			    xmlGenericError(0, "PP: internal error, state == ENTITY_VALUE\n");
			    ctxt->instate = XML_PARSER_CONTENT;
#ifdef DEBUG_PUSH
			    xmlGenericError(0, "PP: entering DTD\n");
#endif
			    break;
			case XML_PARSER_ATTRIBUTE_VALUE:
			    xmlGenericError(0, "PP: internal error, state == ATTRIBUTE_VALUE\n");
			    ctxt->instate = XML_PARSER_START_TAG;
#ifdef DEBUG_PUSH
			    xmlGenericError(0, "PP: entering START_TAG\n");
#endif
			    break;
			case XML_PARSER_SYSTEM_LITERAL:
			    xmlGenericError(0, "PP: internal error, state == SYSTEM_LITERAL\n");
			    ctxt->instate = XML_PARSER_START_TAG;
#ifdef DEBUG_PUSH
			    xmlGenericError(0, "PP: entering START_TAG\n");
#endif
			    break;
			case XML_PARSER_PUBLIC_LITERAL:
			    xmlGenericError(0, "PP: internal error, state == PUBLIC_LITERAL\n");
			    ctxt->instate = XML_PARSER_START_TAG;
#ifdef DEBUG_PUSH
			    xmlGenericError(0, "PP: entering START_TAG\n");
#endif
			    break;
		}
	}
done:
#ifdef DEBUG_PUSH
	xmlGenericError(0, "PP: done %d\n", ret);
#endif
	return ret;
encoding_error:
	{
		char buffer[150];
		snprintf(buffer, 149, "Bytes: 0x%02X 0x%02X 0x%02X 0x%02X\n", ctxt->input->cur[0], ctxt->input->cur[1], ctxt->input->cur[2], ctxt->input->cur[3]);
		__xmlErrEncoding(ctxt, XML_ERR_INVALID_CHAR, "Input is not proper UTF-8, indicate encoding !\n%s", BAD_CAST buffer, 0);
	}
	return 0;
}

/**
 * xmlParseCheckTransition:
 * @ctxt:  an XML parser context
 * @chunk:  a char array
 * @size:  the size in byte of the chunk
 *
 * Check depending on the current parser state if the chunk given must be
 * processed immediately or one need more data to advance on parsing.
 *
 * Returns -1 in case of error, 0 if the push is not needed and 1 if needed
 */
static int xmlParseCheckTransition(xmlParserCtxt * ctxt, const char * chunk, int size)
{
	if(!ctxt || !chunk || (size < 0))
		return -1;
	else {
		int    do_find_rangl = 0;
		if(ctxt->instate == XML_PARSER_START_TAG)
			do_find_rangl = 1;
		else if(ctxt->progressive == XML_PARSER_COMMENT)
			do_find_rangl = 1;
		else if(ctxt->instate == XML_PARSER_CDATA_SECTION)
			do_find_rangl = 1;
		else if(ctxt->progressive == XML_PARSER_PI)
			do_find_rangl = 1;
		else if(ctxt->instate == XML_PARSER_END_TAG)
			do_find_rangl = 1;
		else if(ctxt->progressive == XML_PARSER_DTD || ctxt->instate == XML_PARSER_DTD)
			do_find_rangl = 1;
		return BIN(!do_find_rangl || memchr(chunk, '>', size));
	}
}

/**
 * xmlParseChunk:
 * @ctxt:  an XML parser context
 * @chunk:  an char array
 * @size:  the size in byte of the chunk
 * @terminate:  last chunk indicator
 *
 * Parse a Chunk of memory
 *
 * Returns zero if no error, the xmlParserErrors otherwise.
 */
int xmlParseChunk(xmlParserCtxt * ctxt, const char * chunk, int size, int terminate)
{
	int end_in_lf = 0;
	int remain = 0;
	size_t old_avail = 0;
	size_t avail = 0;
	if(!ctxt)
		return XML_ERR_INTERNAL_ERROR;
	if((ctxt->errNo != XML_ERR_OK) && (ctxt->disableSAX == 1))
		return(ctxt->errNo);
	if(ctxt->instate == XML_PARSER_EOF)
		return -1;
	if(ctxt->instate == XML_PARSER_START)
		xmlDetectSAX2(ctxt);
	if(size > 0 && chunk && (!terminate) && (chunk[size - 1] == '\r')) {
		end_in_lf = 1;
		size--;
	}
xmldecl_done:
	if(size > 0 && chunk && ctxt->input && ctxt->input->buf && (ctxt->instate != XML_PARSER_EOF)) {
		size_t base = xmlBufGetInputBase(ctxt->input->buf->buffer, ctxt->input);
		size_t cur = ctxt->input->cur - ctxt->input->base;
		int res;
		old_avail = xmlBufUse(ctxt->input->buf->buffer);
		/*
		 * Specific handling if we autodetected an encoding, we should not
		 * push more than the first line ... which depend on the encoding
		 * And only push the rest once the final encoding was detected
		 */
		if((ctxt->instate == XML_PARSER_START) && ctxt->input && ctxt->input->buf && ctxt->input->buf->encoder) {
			uint len = 45;
			if((xmlStrcasestr(BAD_CAST ctxt->input->buf->encoder->name, BAD_CAST "UTF-16")) ||
			    (xmlStrcasestr(BAD_CAST ctxt->input->buf->encoder->name, BAD_CAST "UTF16")))
				len = 90;
			else if((xmlStrcasestr(BAD_CAST ctxt->input->buf->encoder->name, BAD_CAST "UCS-4")) ||
			    (xmlStrcasestr(BAD_CAST ctxt->input->buf->encoder->name, BAD_CAST "UCS4")))
				len = 180;
			if(ctxt->input->buf->rawconsumed < len)
				len -= ctxt->input->buf->rawconsumed;

			/*
			 * Change size for reading the initial declaration only
			 * if size is greater than len. Otherwise, memmove in xmlBufferAdd
			 * will blindly copy extra bytes from memory.
			 */
			if((uint)size > len) {
				remain = size - len;
				size = len;
			}
			else {
				remain = 0;
			}
		}
		res = xmlParserInputBufferPush(ctxt->input->buf, size, chunk);
		if(res < 0) {
			ctxt->errNo = XML_PARSER_EOF;
			ctxt->disableSAX = 1;
			return (XML_PARSER_EOF);
		}
		xmlBufSetInputBaseCur(ctxt->input->buf->buffer, ctxt->input, base, cur);
#ifdef DEBUG_PUSH
		xmlGenericError(0, "PP: pushed %d\n", size);
#endif
	}
	else if(ctxt->instate != XML_PARSER_EOF) {
		if(ctxt->input && ctxt->input->buf != NULL) {
			xmlParserInputBufferPtr in = ctxt->input->buf;
			if((in->encoder != NULL) && (in->buffer != NULL) && (in->raw != NULL)) {
				size_t base = xmlBufGetInputBase(in->buffer, ctxt->input);
				size_t current = ctxt->input->cur - ctxt->input->base;
				int nbchars = xmlCharEncInput(in, terminate);
				if(nbchars < 0) {
					/* @todo 2.6.0 */
					xmlGenericError(0, "xmlParseChunk: encoder error\n");
					return(XML_ERR_INVALID_ENCODING);
				}
				xmlBufSetInputBaseCur(in->buffer, ctxt->input, base, current);
			}
		}
	}
	if(remain != 0) {
		xmlParseTryOrFinish(ctxt, 0);
	}
	else {
		if(ctxt->input && (ctxt->input->buf != NULL))
			avail = xmlBufUse(ctxt->input->buf->buffer);
		/*
		 * Depending on the current state it may not be such
		 * a good idea to try parsing if there is nothing in the chunk
		 * which would be worth doing a parser state transition and we
		 * need to wait for more data
		 */
		if((terminate) || (avail > XML_MAX_TEXT_LENGTH) || (old_avail == 0) || (avail == 0) ||
		    (xmlParseCheckTransition(ctxt, (const char*)&ctxt->input->base[old_avail], avail - old_avail)))
			xmlParseTryOrFinish(ctxt, terminate);
	}
	if(ctxt->instate == XML_PARSER_EOF)
		return(ctxt->errNo);
	if(ctxt->input && (((ctxt->input->end - ctxt->input->cur) > XML_MAX_LOOKUP_LIMIT) || ((ctxt->input->cur - ctxt->input->base) > XML_MAX_LOOKUP_LIMIT)) && !(ctxt->options & XML_PARSE_HUGE)) {
		xmlFatalErr(ctxt, XML_ERR_INTERNAL_ERROR, "Huge input lookup");
		ctxt->instate = XML_PARSER_EOF;
	}
	if((ctxt->errNo != XML_ERR_OK) && (ctxt->disableSAX == 1))
		return(ctxt->errNo);
	if(remain != 0) {
		chunk += size;
		size = remain;
		remain = 0;
		goto xmldecl_done;
	}
	if((end_in_lf == 1) && ctxt->input && ctxt->input->buf) {
		size_t base = xmlBufGetInputBase(ctxt->input->buf->buffer, ctxt->input);
		size_t current = ctxt->input->cur - ctxt->input->base;
		xmlParserInputBufferPush(ctxt->input->buf, 1, "\r");
		xmlBufSetInputBaseCur(ctxt->input->buf->buffer, ctxt->input, base, current);
	}
	if(terminate) {
		/*
		 * Check for termination
		 */
		int cur_avail = 0;
		if(ctxt->input) {
			if(ctxt->input->buf == NULL)
				cur_avail = ctxt->input->length - (ctxt->input->cur - ctxt->input->base);
			else
				cur_avail = xmlBufUse(ctxt->input->buf->buffer) - (ctxt->input->cur - ctxt->input->base);
		}
		if((ctxt->instate != XML_PARSER_EOF) && (ctxt->instate != XML_PARSER_EPILOG)) {
			xmlFatalErr(ctxt, XML_ERR_DOCUMENT_END, 0);
		}
		if((ctxt->instate == XML_PARSER_EPILOG) && (cur_avail > 0)) {
			xmlFatalErr(ctxt, XML_ERR_DOCUMENT_END, 0);
		}
		if(ctxt->instate != XML_PARSER_EOF) {
			if(ctxt->sax && ctxt->sax->endDocument)
				ctxt->sax->endDocument(ctxt->userData);
		}
		ctxt->instate = XML_PARSER_EOF;
	}
	return ctxt->wellFormed ? 0 : (xmlParserErrors)ctxt->errNo;
}

/************************************************************************
*									*
*		I/O front end functions to the parser			*
*									*
************************************************************************/

/**
 * xmlCreatePushParserCtxt:
 * @sax:  a SAX handler
 * @user_data:  The user data returned on SAX callbacks
 * @chunk:  a pointer to an array of chars
 * @size:  number of chars in the array
 * @filename:  an optional file name or URI
 *
 * Create a parser context for using the XML parser in push mode.
 * If @buffer and @size are non-NULL, the data is used to detect
 * the encoding.  The remaining characters will be parsed so they
 * don't need to be fed in again through xmlParseChunk.
 * To allow content encoding detection, @size should be >= 4
 * The value of @filename is used for fetching external entities
 * and error/warning reports.
 *
 * Returns the new parser context or NULL
 */

xmlParserCtxt * xmlCreatePushParserCtxt(xmlSAXHandlerPtr sax, void * user_data, const char * chunk, int size, const char * filename)
{
	xmlParserCtxt * ctxt;
	xmlParserInputPtr inputStream;
	xmlParserInputBufferPtr buf;
	xmlCharEncoding enc = XML_CHAR_ENCODING_NONE;
	/*
	 * plug some encoding conversion routines
	 */
	if((chunk != NULL) && (size >= 4))
		enc = xmlDetectCharEncoding((const xmlChar*)chunk, size);

	buf = xmlAllocParserInputBuffer(enc);
	if(!buf)
		return 0;
	ctxt = xmlNewParserCtxt();
	if(!ctxt) {
		xmlErrMemory(NULL, "creating parser: out of memory\n");
		xmlFreeParserInputBuffer(buf);
		return 0;
	}
	ctxt->dictNames = 1;
	ctxt->pushTab = (void**)SAlloc::M(ctxt->nameMax * 3 * sizeof(xmlChar *));
	if(ctxt->pushTab == NULL) {
		xmlErrMemory(ctxt, 0);
		xmlFreeParserInputBuffer(buf);
		xmlFreeParserCtxt(ctxt);
		return 0;
	}
	if(sax) {
#ifdef LIBXML_SAX1_ENABLED
		if(ctxt->sax != (xmlSAXHandlerPtr)&xmlDefaultSAXHandler)
#endif /* LIBXML_SAX1_ENABLED */
		SAlloc::F(ctxt->sax);
		ctxt->sax = (xmlSAXHandlerPtr)SAlloc::M(sizeof(xmlSAXHandler));
		if(ctxt->sax == NULL) {
			xmlErrMemory(ctxt, 0);
			xmlFreeParserInputBuffer(buf);
			xmlFreeParserCtxt(ctxt);
			return 0;
		}
		memzero(ctxt->sax, sizeof(xmlSAXHandler));
		if(sax->initialized == XML_SAX2_MAGIC)
			memcpy(ctxt->sax, sax, sizeof(xmlSAXHandler));
		else
			memcpy(ctxt->sax, sax, sizeof(xmlSAXHandlerV1));
		if(user_data != NULL)
			ctxt->userData = user_data;
	}
	ctxt->directory = filename ? xmlParserGetDirectory(filename) : 0;
	inputStream = xmlNewInputStream(ctxt);
	if(inputStream == NULL) {
		xmlFreeParserCtxt(ctxt);
		xmlFreeParserInputBuffer(buf);
		return 0;
	}
	if(filename == NULL)
		inputStream->filename = NULL;
	else {
		inputStream->filename = (char*)xmlCanonicPath((const xmlChar*)filename);
		if(inputStream->filename == NULL) {
			xmlFreeParserCtxt(ctxt);
			xmlFreeParserInputBuffer(buf);
			return 0;
		}
	}
	inputStream->buf = buf;
	xmlBufResetInput(inputStream->buf->buffer, inputStream);
	inputPush(ctxt, inputStream);
	/*
	 * If the caller didn't provide an initial 'chunk' for determining
	 * the encoding, we set the context to XML_CHAR_ENCODING_NONE so
	 * that it can be automatically determined later
	 */
	if((size == 0) || (chunk == NULL)) {
		ctxt->charset = XML_CHAR_ENCODING_NONE;
	}
	else if(ctxt->input && (ctxt->input->buf != NULL)) {
		size_t base = xmlBufGetInputBase(ctxt->input->buf->buffer, ctxt->input);
		size_t cur = ctxt->input->cur - ctxt->input->base;
		xmlParserInputBufferPush(ctxt->input->buf, size, chunk);
		xmlBufSetInputBaseCur(ctxt->input->buf->buffer, ctxt->input, base, cur);
#ifdef DEBUG_PUSH
		xmlGenericError(0, "PP: pushed %d\n", size);
#endif
	}
	if(enc != XML_CHAR_ENCODING_NONE) {
		xmlSwitchEncoding(ctxt, enc);
	}
	return ctxt;
}

#endif /* LIBXML_PUSH_ENABLED */

/**
 * xmlStopParser:
 * @ctxt:  an XML parser context
 *
 * Blocks further parser processing
 */
void xmlStopParser(xmlParserCtxt * ctxt)
{
	if(ctxt) {
		ctxt->instate = XML_PARSER_EOF;
		ctxt->errNo = XML_ERR_USER_STOP;
		ctxt->disableSAX = 1;
		if(ctxt->input) {
			ctxt->input->cur = BAD_CAST "";
			ctxt->input->base = ctxt->input->cur;
		}
	}
}

/**
 * xmlCreateIOParserCtxt:
 * @sax:  a SAX handler
 * @user_data:  The user data returned on SAX callbacks
 * @ioread:  an I/O read function
 * @ioclose:  an I/O close function
 * @ioctx:  an I/O handler
 * @enc:  the charset encoding if known
 *
 * Create a parser context for using the XML parser with an existing
 * I/O stream
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxt * xmlCreateIOParserCtxt(xmlSAXHandlerPtr sax, void * user_data, xmlInputReadCallback ioread, xmlInputCloseCallback ioclose,
    void * ioctx, xmlCharEncoding enc)
{
	xmlParserCtxt * ctxt;
	xmlParserInputPtr inputStream;
	xmlParserInputBufferPtr buf;
	if(ioread == NULL)
		return 0;
	buf = xmlParserInputBufferCreateIO(ioread, ioclose, ioctx, enc);
	if(!buf) {
		if(ioclose)
			ioclose(ioctx);
		return 0;
	}
	ctxt = xmlNewParserCtxt();
	if(!ctxt) {
		xmlFreeParserInputBuffer(buf);
		return 0;
	}
	if(sax) {
#ifdef LIBXML_SAX1_ENABLED
		if(ctxt->sax != (xmlSAXHandlerPtr)&xmlDefaultSAXHandler)
#endif /* LIBXML_SAX1_ENABLED */
		SAlloc::F(ctxt->sax);
		ctxt->sax = (xmlSAXHandlerPtr)SAlloc::M(sizeof(xmlSAXHandler));
		if(ctxt->sax == NULL) {
			xmlErrMemory(ctxt, 0);
			xmlFreeParserCtxt(ctxt);
			return 0;
		}
		memzero(ctxt->sax, sizeof(xmlSAXHandler));
		if(sax->initialized == XML_SAX2_MAGIC)
			memcpy(ctxt->sax, sax, sizeof(xmlSAXHandler));
		else
			memcpy(ctxt->sax, sax, sizeof(xmlSAXHandlerV1));
		if(user_data != NULL)
			ctxt->userData = user_data;
	}
	inputStream = xmlNewIOInputStream(ctxt, buf, enc);
	if(inputStream == NULL) {
		xmlFreeParserCtxt(ctxt);
		return 0;
	}
	inputPush(ctxt, inputStream);
	return ctxt;
}

#ifdef LIBXML_VALID_ENABLED
/************************************************************************
*									*
*		Front ends when parsing a DTD				*
*									*
************************************************************************/

/**
 * xmlIOParseDTD:
 * @sax:  the SAX handler block or NULL
 * @input:  an Input Buffer
 * @enc:  the charset encoding if known
 *
 * Load and parse a DTD
 *
 * Returns the resulting xmlDtdPtr or NULL in case of error.
 * @input will be freed by the function in any case.
 */

xmlDtdPtr xmlIOParseDTD(xmlSAXHandlerPtr sax, xmlParserInputBufferPtr input, xmlCharEncoding enc)
{
	xmlDtd * ret = NULL;
	xmlParserCtxt * ctxt;
	xmlParserInputPtr pinput = NULL;
	xmlChar start[4];
	if(!input)
		return 0;
	ctxt = xmlNewParserCtxt();
	if(!ctxt) {
		xmlFreeParserInputBuffer(input);
		return 0;
	}
	/* We are loading a DTD */
	ctxt->options |= XML_PARSE_DTDLOAD;
	/*
	 * Set-up the SAX context
	 */
	if(sax) {
		SAlloc::F(ctxt->sax);
		ctxt->sax = sax;
		ctxt->userData = ctxt;
	}
	xmlDetectSAX2(ctxt);
	/*
	 * generate a parser input from the I/O handler
	 */
	pinput = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
	if(pinput == NULL) {
		if(sax)
			ctxt->sax = NULL;
		xmlFreeParserInputBuffer(input);
		xmlFreeParserCtxt(ctxt);
		return 0;
	}
	/*
	 * plug some encoding conversion routines here.
	 */
	if(xmlPushInput(ctxt, pinput) < 0) {
		if(sax)
			ctxt->sax = NULL;
		xmlFreeParserCtxt(ctxt);
		return 0;
	}
	if(enc != XML_CHAR_ENCODING_NONE) {
		xmlSwitchEncoding(ctxt, enc);
	}
	pinput->filename = NULL;
	pinput->line = 1;
	pinput->col = 1;
	pinput->base = ctxt->input->cur;
	pinput->cur = ctxt->input->cur;
	pinput->free = NULL;
	/*
	 * let's parse that entity knowing it's an external subset.
	 */
	ctxt->inSubset = 2;
	ctxt->myDoc = xmlNewDoc(BAD_CAST "1.0");
	if(ctxt->myDoc == NULL) {
		xmlErrMemory(ctxt, "New Doc failed");
		return 0;
	}
	ctxt->myDoc->properties = XML_DOC_INTERNAL;
	ctxt->myDoc->extSubset = xmlNewDtd(ctxt->myDoc, BAD_CAST "none", BAD_CAST "none", BAD_CAST "none");
	if((enc == XML_CHAR_ENCODING_NONE) && ((ctxt->input->end - ctxt->input->cur) >= 4)) {
		/*
		 * Get the 4 first bytes and decode the charset
		 * if enc != XML_CHAR_ENCODING_NONE
		 * plug some encoding conversion routines.
		 */
		start[0] = RAW;
		start[1] = NXT(1);
		start[2] = NXT(2);
		start[3] = NXT(3);
		enc = xmlDetectCharEncoding(start, 4);
		if(enc != XML_CHAR_ENCODING_NONE) {
			xmlSwitchEncoding(ctxt, enc);
		}
	}
	xmlParseExternalSubset(ctxt, BAD_CAST "none", BAD_CAST "none");
	if(ctxt->myDoc) {
		if(ctxt->wellFormed) {
			ret = ctxt->myDoc->extSubset;
			ctxt->myDoc->extSubset = NULL;
			if(ret) {
				ret->doc = NULL;
				for(xmlNode * tmp = ret->children; tmp; tmp = tmp->next)
					tmp->doc = NULL;
			}
		}
		else
			ret = NULL;
		xmlFreeDoc(ctxt->myDoc);
		ctxt->myDoc = NULL;
	}
	if(sax)
		ctxt->sax = NULL;
	xmlFreeParserCtxt(ctxt);
	return ret;
}
/**
 * xmlSAXParseDTD:
 * @sax:  the SAX handler block
 * @ExternalID:  a NAME* containing the External ID of the DTD
 * @SystemID:  a NAME* containing the URL to the DTD
 *
 * Load and parse an external subset.
 *
 * Returns the resulting xmlDtdPtr or NULL in case of error.
 */

xmlDtdPtr xmlSAXParseDTD(xmlSAXHandlerPtr sax, const xmlChar * ExternalID, const xmlChar * SystemID)
{
	xmlDtd * ret = NULL;
	xmlParserCtxt * ctxt;
	xmlParserInputPtr input = NULL;
	xmlCharEncoding enc;
	xmlChar* systemIdCanonic;
	if((ExternalID == NULL) && (SystemID == NULL))
		return 0;
	ctxt = xmlNewParserCtxt();
	if(!ctxt) {
		return 0;
	}
	/* We are loading a DTD */
	ctxt->options |= XML_PARSE_DTDLOAD;
	/*
	 * Set-up the SAX context
	 */
	if(sax) {
		SAlloc::F(ctxt->sax);
		ctxt->sax = sax;
		ctxt->userData = ctxt;
	}
	/*
	 * Canonicalise the system ID
	 */
	systemIdCanonic = xmlCanonicPath(SystemID);
	if((SystemID != NULL) && (systemIdCanonic == NULL)) {
		xmlFreeParserCtxt(ctxt);
		return 0;
	}
	/*
	 * Ask the Entity resolver to load the damn thing
	 */
	if(ctxt->sax && (ctxt->sax->resolveEntity != NULL))
		input = ctxt->sax->resolveEntity(ctxt->userData, ExternalID, systemIdCanonic);
	if(!input) {
		if(sax)
			ctxt->sax = NULL;
		xmlFreeParserCtxt(ctxt);
		SAlloc::F(systemIdCanonic);
		return 0;
	}
	/*
	 * plug some encoding conversion routines here.
	 */
	if(xmlPushInput(ctxt, input) < 0) {
		if(sax)
			ctxt->sax = NULL;
		xmlFreeParserCtxt(ctxt);
		SAlloc::F(systemIdCanonic);
		return 0;
	}
	if((ctxt->input->end - ctxt->input->cur) >= 4) {
		enc = xmlDetectCharEncoding(ctxt->input->cur, 4);
		xmlSwitchEncoding(ctxt, enc);
	}
	if(input->filename == NULL)
		input->filename = (char*)systemIdCanonic;
	else
		SAlloc::F(systemIdCanonic);
	input->line = 1;
	input->col = 1;
	input->base = ctxt->input->cur;
	input->cur = ctxt->input->cur;
	input->free = NULL;
	/*
	 * let's parse that entity knowing it's an external subset.
	 */
	ctxt->inSubset = 2;
	ctxt->myDoc = xmlNewDoc(BAD_CAST "1.0");
	if(ctxt->myDoc == NULL) {
		xmlErrMemory(ctxt, "New Doc failed");
		if(sax) ctxt->sax = NULL;
		xmlFreeParserCtxt(ctxt);
		return 0;
	}
	ctxt->myDoc->properties = XML_DOC_INTERNAL;
	ctxt->myDoc->extSubset = xmlNewDtd(ctxt->myDoc, BAD_CAST "none", ExternalID, SystemID);
	xmlParseExternalSubset(ctxt, ExternalID, SystemID);
	if(ctxt->myDoc) {
		if(ctxt->wellFormed) {
			ret = ctxt->myDoc->extSubset;
			ctxt->myDoc->extSubset = NULL;
			if(ret) {
				xmlNode * tmp;
				ret->doc = NULL;
				tmp = ret->children;
				while(tmp) {
					tmp->doc = NULL;
					tmp = tmp->next;
				}
			}
		}
		else {
			ret = NULL;
		}
		xmlFreeDoc(ctxt->myDoc);
		ctxt->myDoc = NULL;
	}
	if(sax) ctxt->sax = NULL;
	xmlFreeParserCtxt(ctxt);

	return ret;
}

/**
 * xmlParseDTD:
 * @ExternalID:  a NAME* containing the External ID of the DTD
 * @SystemID:  a NAME* containing the URL to the DTD
 *
 * Load and parse an external subset.
 *
 * Returns the resulting xmlDtdPtr or NULL in case of error.
 */

xmlDtdPtr xmlParseDTD(const xmlChar * ExternalID, const xmlChar * SystemID) 
{
	return xmlSAXParseDTD(NULL, ExternalID, SystemID);
}

#endif /* LIBXML_VALID_ENABLED */

/************************************************************************
*									*
*		Front ends when parsing an Entity			*
*									*
************************************************************************/

/**
 * xmlParseCtxtExternalEntity:
 * @ctx:  the existing parsing context
 * @URL:  the URL for the entity to load
 * @ID:  the System ID for the entity to load
 * @lst:  the return value for the set of parsed nodes
 *
 * Parse an external general entity within an existing parsing context
 * An external general parsed entity is well-formed if it matches the
 * production labeled extParsedEnt.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * Returns 0 if the entity is well formed, -1 in case of args problem and
 *    the parser error code otherwise
 */

int xmlParseCtxtExternalEntity(xmlParserCtxt * ctx, const xmlChar * URL, const xmlChar * ID, xmlNode ** lst) 
{
	xmlParserCtxt * ctxt;
	xmlDoc * newDoc;
	xmlNode * newRoot;
	xmlSAXHandlerPtr oldsax = NULL;
	int ret = 0;
	xmlChar start[4];
	xmlCharEncoding enc;
	if(!ctx) 
		return -1;
	if(((ctx->depth > 40) && ((ctx->options & XML_PARSE_HUGE) == 0)) || (ctx->depth > 1024)) {
		return(XML_ERR_ENTITY_LOOP);
	}
	if(lst != NULL)
		*lst = NULL;
	if((URL == NULL) && (ID == NULL))
		return -1;
	if(ctx->myDoc == NULL) /* @@ relax but check for dereferences */
		return -1;
	ctxt = xmlCreateEntityParserCtxtInternal(URL, ID, NULL, ctx);
	if(!ctxt) {
		return -1;
	}
	oldsax = ctxt->sax;
	ctxt->sax = ctx->sax;
	xmlDetectSAX2(ctxt);
	newDoc = xmlNewDoc(BAD_CAST "1.0");
	if(newDoc == NULL) {
		xmlFreeParserCtxt(ctxt);
		return -1;
	}
	newDoc->properties = XML_DOC_INTERNAL;
	if(ctx->myDoc->dict) {
		newDoc->dict = ctx->myDoc->dict;
		xmlDictReference(newDoc->dict);
	}
	if(ctx->myDoc) {
		newDoc->intSubset = ctx->myDoc->intSubset;
		newDoc->extSubset = ctx->myDoc->extSubset;
	}
	if(ctx->myDoc->URL) {
		newDoc->URL = sstrdup(ctx->myDoc->URL);
	}
	newRoot = xmlNewDocNode(newDoc, NULL, BAD_CAST "pseudoroot", 0);
	if(newRoot == NULL) {
		ctxt->sax = oldsax;
		xmlFreeParserCtxt(ctxt);
		newDoc->intSubset = NULL;
		newDoc->extSubset = NULL;
		xmlFreeDoc(newDoc);
		return -1;
	}
	xmlAddChild((xmlNode *)newDoc, newRoot);
	nodePush(ctxt, newDoc->children);
	if(ctx->myDoc == NULL) {
		ctxt->myDoc = newDoc;
	}
	else {
		ctxt->myDoc = ctx->myDoc;
		newDoc->children->doc = ctx->myDoc;
	}
	/*
	 * Get the 4 first bytes and decode the charset
	 * if enc != XML_CHAR_ENCODING_NONE
	 * plug some encoding conversion routines.
	 */
	GROW
	if((ctxt->input->end - ctxt->input->cur) >= 4) {
		start[0] = RAW;
		start[1] = NXT(1);
		start[2] = NXT(2);
		start[3] = NXT(3);
		enc = xmlDetectCharEncoding(start, 4);
		if(enc != XML_CHAR_ENCODING_NONE) {
			xmlSwitchEncoding(ctxt, enc);
		}
	}
	/*
	 * Parse a possible text declaration first
	 */
	if((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
		xmlParseTextDecl(ctxt);
		/*
		 * An XML-1.0 document can't reference an entity not XML-1.0
		 */
		if(sstreq(ctx->version, "1.0") && !sstreq(ctxt->input->version, "1.0")) {
			xmlFatalErrMsg(ctxt, XML_ERR_VERSION_MISMATCH, "Version mismatch between document and entity\n");
		}
	}
	/*
	 * If the user provided its own SAX callbacks then reuse the
	 * useData callback field, otherwise the expected setup in a
	 * DOM builder is to have userData == ctxt
	 */
	ctxt->userData = (ctx->userData == ctx) ? ctxt : ctx->userData;
	/*
	 * Doing validity checking on chunk doesn't make sense
	 */
	ctxt->instate = XML_PARSER_CONTENT;
	ctxt->validate = ctx->validate;
	ctxt->valid = ctx->valid;
	ctxt->loadsubset = ctx->loadsubset;
	ctxt->depth = ctx->depth + 1;
	ctxt->replaceEntities = ctx->replaceEntities;
	if(ctxt->validate) {
		ctxt->vctxt.error = ctx->vctxt.error;
		ctxt->vctxt.warning = ctx->vctxt.warning;
	}
	else {
		ctxt->vctxt.error = NULL;
		ctxt->vctxt.warning = NULL;
	}
	ctxt->vctxt.PP_NodeTab = NULL;
	ctxt->vctxt.nodeNr = 0;
	ctxt->vctxt.nodeMax = 0;
	ctxt->vctxt.P_Node = NULL;
	xmlDictFree(ctxt->dict);
	ctxt->dict = ctx->dict;
	ctxt->str_xml = xmlDictLookup(ctxt->dict, BAD_CAST "xml", 3);
	ctxt->str_xmlns = xmlDictLookup(ctxt->dict, BAD_CAST "xmlns", 5);
	ctxt->str_xml_ns = xmlDictLookup(ctxt->dict, XML_XML_NAMESPACE, 36);
	ctxt->dictNames = ctx->dictNames;
	ctxt->attsDefault = ctx->attsDefault;
	ctxt->attsSpecial = ctx->attsSpecial;
	ctxt->linenumbers = ctx->linenumbers;
	xmlParseContent(ctxt);
	ctx->validate = ctxt->validate;
	ctx->valid = ctxt->valid;
	if((RAW == '<') && (NXT(1) == '/')) {
		xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
	}
	else if(RAW != 0) {
		xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, 0);
	}
	if(ctxt->P_Node != newDoc->children) {
		xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
	}
	if(!ctxt->wellFormed) {
		if(ctxt->errNo == 0)
			ret = 1;
		else
			ret = ctxt->errNo;
	}
	else {
		if(lst) {
			// Return the newly created nodeset after unlinking it from they pseudo parent.
			xmlNode * cur = newDoc->children->children;
			*lst = cur;
			while(cur) {
				cur->parent = NULL;
				cur = cur->next;
			}
			newDoc->children->children = NULL;
		}
		ret = 0;
	}
	ctxt->sax = oldsax;
	ctxt->dict = NULL;
	ctxt->attsDefault = NULL;
	ctxt->attsSpecial = NULL;
	xmlFreeParserCtxt(ctxt);
	newDoc->intSubset = NULL;
	newDoc->extSubset = NULL;
	xmlFreeDoc(newDoc);
	return ret;
}
/**
 * xmlParseExternalEntityPrivate:
 * @doc:  the document the chunk pertains to
 * @oldctxt:  the previous parser context if available
 * @sax:  the SAX handler bloc (possibly NULL)
 * @user_data:  The user data returned on SAX callbacks (possibly NULL)
 * @depth:  Used for loop detection, use 0
 * @URL:  the URL for the entity to load
 * @ID:  the System ID for the entity to load
 * @list:  the return value for the set of parsed nodes
 *
 * Private version of xmlParseExternalEntity()
 *
 * Returns 0 if the entity is well formed, -1 in case of args problem and
 *    the parser error code otherwise
 */

static xmlParserErrors xmlParseExternalEntityPrivate(xmlDoc * doc, xmlParserCtxt * oldctxt, xmlSAXHandlerPtr sax, 
	void * user_data, int depth, const xmlChar * URL, const xmlChar * ID, xmlNode ** list)
{
	xmlParserCtxt * ctxt;
	xmlDoc * newDoc;
	xmlNode * newRoot;
	xmlSAXHandlerPtr oldsax = NULL;
	xmlParserErrors ret = XML_ERR_OK;
	xmlChar start[4];
	xmlCharEncoding enc;
	if(((depth > 40) && ((oldctxt == NULL) || (oldctxt->options & XML_PARSE_HUGE) == 0)) || (depth > 1024)) {
		return(XML_ERR_ENTITY_LOOP);
	}
	ASSIGN_PTR(list, 0);
	if((URL == NULL) && (ID == NULL))
		return XML_ERR_INTERNAL_ERROR;
	if(!doc)
		return XML_ERR_INTERNAL_ERROR;
	ctxt = xmlCreateEntityParserCtxtInternal(URL, ID, NULL, oldctxt);
	if(!ctxt)
		return XML_WAR_UNDECLARED_ENTITY;
	ctxt->userData = ctxt;
	if(oldctxt != NULL) {
		ctxt->_private = oldctxt->_private;
		ctxt->loadsubset = oldctxt->loadsubset;
		ctxt->validate = oldctxt->validate;
		ctxt->external = oldctxt->external;
		ctxt->record_info = oldctxt->record_info;
		ctxt->node_seq.maximum = oldctxt->node_seq.maximum;
		ctxt->node_seq.length = oldctxt->node_seq.length;
		ctxt->node_seq.buffer = oldctxt->node_seq.buffer;
	}
	else {
		/*
		 * Doing validity checking on chunk without context
		 * doesn't make sense
		 */
		ctxt->_private = NULL;
		ctxt->validate = 0;
		ctxt->external = 2;
		ctxt->loadsubset = 0;
	}
	if(sax) {
		oldsax = ctxt->sax;
		ctxt->sax = sax;
		if(user_data != NULL)
			ctxt->userData = user_data;
	}
	xmlDetectSAX2(ctxt);
	newDoc = xmlNewDoc(BAD_CAST "1.0");
	if(newDoc == NULL) {
		ctxt->node_seq.maximum = 0;
		ctxt->node_seq.length = 0;
		ctxt->node_seq.buffer = NULL;
		xmlFreeParserCtxt(ctxt);
		return XML_ERR_INTERNAL_ERROR;
	}
	newDoc->properties = XML_DOC_INTERNAL;
	newDoc->intSubset = doc->intSubset;
	newDoc->extSubset = doc->extSubset;
	newDoc->dict = doc->dict;
	xmlDictReference(newDoc->dict);

	if(doc->URL != NULL) {
		newDoc->URL = sstrdup(doc->URL);
	}
	newRoot = xmlNewDocNode(newDoc, NULL, BAD_CAST "pseudoroot", 0);
	if(newRoot == NULL) {
		if(sax)
			ctxt->sax = oldsax;
		ctxt->node_seq.maximum = 0;
		ctxt->node_seq.length = 0;
		ctxt->node_seq.buffer = NULL;
		xmlFreeParserCtxt(ctxt);
		newDoc->intSubset = NULL;
		newDoc->extSubset = NULL;
		xmlFreeDoc(newDoc);
		return XML_ERR_INTERNAL_ERROR;
	}
	xmlAddChild((xmlNode *)newDoc, newRoot);
	nodePush(ctxt, newDoc->children);
	ctxt->myDoc = doc;
	newRoot->doc = doc;

	/*
	 * Get the 4 first bytes and decode the charset
	 * if enc != XML_CHAR_ENCODING_NONE
	 * plug some encoding conversion routines.
	 */
	GROW;
	if((ctxt->input->end - ctxt->input->cur) >= 4) {
		start[0] = RAW;
		start[1] = NXT(1);
		start[2] = NXT(2);
		start[3] = NXT(3);
		enc = xmlDetectCharEncoding(start, 4);
		if(enc != XML_CHAR_ENCODING_NONE) {
			xmlSwitchEncoding(ctxt, enc);
		}
	}
	/*
	 * Parse a possible text declaration first
	 */
	if((CMP5(CUR_PTR, '<', '?', 'x', 'm', 'l')) && (IS_BLANK_CH(NXT(5)))) {
		xmlParseTextDecl(ctxt);
	}
	ctxt->instate = XML_PARSER_CONTENT;
	ctxt->depth = depth;
	xmlParseContent(ctxt);
	if((RAW == '<') && (NXT(1) == '/')) {
		xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
	}
	else if(RAW != 0) {
		xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, 0);
	}
	if(ctxt->P_Node != newDoc->children) {
		xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
	}
	if(!ctxt->wellFormed) {
		if(ctxt->errNo == 0)
			ret = XML_ERR_INTERNAL_ERROR;
		else
			ret = (xmlParserErrors)ctxt->errNo;
	}
	else {
		if(list) {
			/*
			 * Return the newly created nodeset after unlinking it from
			 * they pseudo parent.
			 */
			xmlNode * cur = newDoc->children->children;
			*list = cur;
			while(cur) {
				cur->parent = NULL;
				cur = cur->next;
			}
			newDoc->children->children = NULL;
		}
		ret = XML_ERR_OK;
	}
	/*
	 * Record in the parent context the number of entities replacement
	 * done when parsing that reference.
	 */
	if(oldctxt != NULL)
		oldctxt->nbentities += ctxt->nbentities;

	/*
	 * Also record the size of the entity parsed
	 */
	if(ctxt->input) {
		oldctxt->sizeentities += ctxt->input->consumed;
		oldctxt->sizeentities += (ctxt->input->cur - ctxt->input->base);
	}
	/*
	 * And record the last error if any
	 */
	if(ctxt->lastError.code != XML_ERR_OK)
		xmlCopyError(&ctxt->lastError, &oldctxt->lastError);
	if(sax)
		ctxt->sax = oldsax;
	oldctxt->node_seq.maximum = ctxt->node_seq.maximum;
	oldctxt->node_seq.length = ctxt->node_seq.length;
	oldctxt->node_seq.buffer = ctxt->node_seq.buffer;
	ctxt->node_seq.maximum = 0;
	ctxt->node_seq.length = 0;
	ctxt->node_seq.buffer = NULL;
	xmlFreeParserCtxt(ctxt);
	newDoc->intSubset = NULL;
	newDoc->extSubset = NULL;
	xmlFreeDoc(newDoc);
	return ret;
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlParseExternalEntity:
 * @doc:  the document the chunk pertains to
 * @sax:  the SAX handler bloc (possibly NULL)
 * @user_data:  The user data returned on SAX callbacks (possibly NULL)
 * @depth:  Used for loop detection, use 0
 * @URL:  the URL for the entity to load
 * @ID:  the System ID for the entity to load
 * @lst:  the return value for the set of parsed nodes
 *
 * Parse an external general entity
 * An external general parsed entity is well-formed if it matches the
 * production labeled extParsedEnt.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * Returns 0 if the entity is well formed, -1 in case of args problem and
 *    the parser error code otherwise
 */
int xmlParseExternalEntity(xmlDoc * doc, xmlSAXHandlerPtr sax, void * user_data, int depth, const xmlChar * URL, const xmlChar * ID, xmlNode ** lst)
{
	return(xmlParseExternalEntityPrivate(doc, NULL, sax, user_data, depth, URL, ID, lst));
}

/**
 * xmlParseBalancedChunkMemory:
 * @doc:  the document the chunk pertains to
 * @sax:  the SAX handler bloc (possibly NULL)
 * @user_data:  The user data returned on SAX callbacks (possibly NULL)
 * @depth:  Used for loop detection, use 0
 * @string:  the input string in UTF8 or ISO-Latin (zero terminated)
 * @lst:  the return value for the set of parsed nodes
 *
 * Parse a well-balanced chunk of an XML document
 * called by the parser
 * The allowed sequence for the Well Balanced Chunk is the one defined by
 * the content production in the XML grammar:
 *
 * [43] content ::= (element | CharData | Reference | CDSect | PI | Comment)*
 *
 * Returns 0 if the chunk is well balanced, -1 in case of args problem and
 *    the parser error code otherwise
 */

int xmlParseBalancedChunkMemory(xmlDoc * doc, xmlSAXHandlerPtr sax, void * user_data, int depth, const xmlChar * string, xmlNode ** lst)
{
	return xmlParseBalancedChunkMemoryRecover(doc, sax, user_data, depth, string, lst, 0);
}

#endif /* LIBXML_SAX1_ENABLED */

/**
 * xmlParseBalancedChunkMemoryInternal:
 * @oldctxt:  the existing parsing context
 * @string:  the input string in UTF8 or ISO-Latin (zero terminated)
 * @user_data:  the user data field for the parser context
 * @lst:  the return value for the set of parsed nodes
 *
 *
 * Parse a well-balanced chunk of an XML document
 * called by the parser
 * The allowed sequence for the Well Balanced Chunk is the one defined by
 * the content production in the XML grammar:
 *
 * [43] content ::= (element | CharData | Reference | CDSect | PI | Comment)*
 *
 * Returns XML_ERR_OK if the chunk is well balanced, and the parser
 * error code otherwise
 *
 * In case recover is set to 1, the nodelist will not be empty even if
 * the parsed chunk is not well balanced.
 */
static xmlParserErrors xmlParseBalancedChunkMemoryInternal(xmlParserCtxt * oldctxt, const xmlChar * string, void * user_data, xmlNode ** lst)
{
	xmlParserCtxt * ctxt;
	xmlDoc * newDoc = NULL;
	xmlNode * newRoot;
	xmlSAXHandlerPtr oldsax = NULL;
	xmlNode * content = NULL;
	xmlNode * last = NULL;
	int size;
	xmlParserErrors ret = XML_ERR_OK;
#ifdef SAX2
	int i;
#endif
	if(((oldctxt->depth > 40) && ((oldctxt->options & XML_PARSE_HUGE) == 0)) || (oldctxt->depth >  1024)) {
		return(XML_ERR_ENTITY_LOOP);
	}
	ASSIGN_PTR(lst, 0);
	if(string == NULL)
		return XML_ERR_INTERNAL_ERROR;
	size = sstrlen(string);
	ctxt = xmlCreateMemoryParserCtxt((char*)string, size);
	if(!ctxt)
		return XML_WAR_UNDECLARED_ENTITY;
	ctxt->userData = NZOR(user_data, ctxt);
	xmlDictFree(ctxt->dict);
	ctxt->dict = oldctxt->dict;
	ctxt->str_xml = xmlDictLookup(ctxt->dict, BAD_CAST "xml", 3);
	ctxt->str_xmlns = xmlDictLookup(ctxt->dict, BAD_CAST "xmlns", 5);
	ctxt->str_xml_ns = xmlDictLookup(ctxt->dict, XML_XML_NAMESPACE, 36);

#ifdef SAX2
	/* propagate namespaces down the entity */
	for(i = 0; i < oldctxt->nsNr; i += 2) {
		nsPush(ctxt, oldctxt->nsTab[i], oldctxt->nsTab[i+1]);
	}
#endif
	oldsax = ctxt->sax;
	ctxt->sax = oldctxt->sax;
	xmlDetectSAX2(ctxt);
	ctxt->replaceEntities = oldctxt->replaceEntities;
	ctxt->options = oldctxt->options;
	ctxt->_private = oldctxt->_private;
	if(oldctxt->myDoc == NULL) {
		newDoc = xmlNewDoc(BAD_CAST "1.0");
		if(newDoc == NULL) {
			ctxt->sax = oldsax;
			ctxt->dict = NULL;
			xmlFreeParserCtxt(ctxt);
			return XML_ERR_INTERNAL_ERROR;
		}
		newDoc->properties = XML_DOC_INTERNAL;
		newDoc->dict = ctxt->dict;
		xmlDictReference(newDoc->dict);
		ctxt->myDoc = newDoc;
	}
	else {
		ctxt->myDoc = oldctxt->myDoc;
		content = ctxt->myDoc->children;
		last = ctxt->myDoc->last;
	}
	newRoot = xmlNewDocNode(ctxt->myDoc, NULL, BAD_CAST "pseudoroot", 0);
	if(newRoot == NULL) {
		ctxt->sax = oldsax;
		ctxt->dict = NULL;
		xmlFreeParserCtxt(ctxt);
		xmlFreeDoc(newDoc);
		return XML_ERR_INTERNAL_ERROR;
	}
	ctxt->myDoc->children = NULL;
	ctxt->myDoc->last = NULL;
	xmlAddChild((xmlNode *)ctxt->myDoc, newRoot);
	nodePush(ctxt, ctxt->myDoc->children);
	ctxt->instate = XML_PARSER_CONTENT;
	ctxt->depth = oldctxt->depth + 1;

	ctxt->validate = 0;
	ctxt->loadsubset = oldctxt->loadsubset;
	if((oldctxt->validate) || (oldctxt->replaceEntities != 0)) {
		//
		// ID/IDREF registration will be done in xmlValidateElement below
		//
		ctxt->loadsubset |= XML_SKIP_IDS;
	}
	ctxt->dictNames = oldctxt->dictNames;
	ctxt->attsDefault = oldctxt->attsDefault;
	ctxt->attsSpecial = oldctxt->attsSpecial;
	xmlParseContent(ctxt);
	if((RAW == '<') && (NXT(1) == '/')) {
		xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
	}
	else if(RAW != 0) {
		xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, 0);
	}
	if(ctxt->P_Node != ctxt->myDoc->children) {
		xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
	}
	if(!ctxt->wellFormed) {
		ret = (ctxt->errNo == 0) ? XML_ERR_INTERNAL_ERROR : (xmlParserErrors)ctxt->errNo;
	}
	else {
		ret = XML_ERR_OK;
	}
	if((lst != NULL) && (ret == XML_ERR_OK)) {
		/*
		 * Return the newly created nodeset after unlinking it from
		 * they pseudo parent.
		 */
		xmlNode * cur = ctxt->myDoc->children->children;
		*lst = cur;
		while(cur) {
#ifdef LIBXML_VALID_ENABLED
			if((oldctxt->validate) && (oldctxt->wellFormed) && (oldctxt->myDoc) && (oldctxt->myDoc->intSubset) && (cur->type == XML_ELEMENT_NODE)) {
				oldctxt->valid &= xmlValidateElement(&oldctxt->vctxt, oldctxt->myDoc, cur);
			}
#endif /* LIBXML_VALID_ENABLED */
			cur->parent = NULL;
			cur = cur->next;
		}
		ctxt->myDoc->children->children = NULL;
	}
	if(ctxt->myDoc) {
		xmlFreeNode(ctxt->myDoc->children);
		ctxt->myDoc->children = content;
		ctxt->myDoc->last = last;
	}
	/*
	 * Record in the parent context the number of entities replacement
	 * done when parsing that reference.
	 */
	if(oldctxt != NULL)
		oldctxt->nbentities += ctxt->nbentities;
	/*
	 * Also record the last error if any
	 */
	if(ctxt->lastError.code != XML_ERR_OK)
		xmlCopyError(&ctxt->lastError, &oldctxt->lastError);
	ctxt->sax = oldsax;
	ctxt->dict = NULL;
	ctxt->attsDefault = NULL;
	ctxt->attsSpecial = NULL;
	xmlFreeParserCtxt(ctxt);
	xmlFreeDoc(newDoc);
	return ret;
}

/**
 * xmlParseInNodeContext:
 * @node:  the context node
 * @data:  the input string
 * @datalen:  the input string length in bytes
 * @options:  a combination of xmlParserOption
 * @lst:  the return value for the set of parsed nodes
 *
 * Parse a well-balanced chunk of an XML document
 * within the context (DTD, namespaces, etc ...) of the given node.
 *
 * The allowed sequence for the data is a Well Balanced Chunk defined by
 * the content production in the XML grammar:
 *
 * [43] content ::= (element | CharData | Reference | CDSect | PI | Comment)*
 *
 * Returns XML_ERR_OK if the chunk is well balanced, and the parser
 * error code otherwise
 */
xmlParserErrors xmlParseInNodeContext(xmlNode * P_Node, const char * data, int datalen, int options, xmlNode ** lst)
{
#ifdef SAX2
	xmlParserCtxt * ctxt;
	xmlDoc * doc = NULL;
	xmlNode * fake;
	xmlNode * cur;
	int nsnr = 0;
	xmlParserErrors ret = XML_ERR_OK;
	/*
	 * check all input parameters, grab the document
	 */
	if(!lst || !P_Node || !data || (datalen < 0))
		return XML_ERR_INTERNAL_ERROR;
	switch(P_Node->type) {
		case XML_ELEMENT_NODE:
		case XML_ATTRIBUTE_NODE:
		case XML_TEXT_NODE:
		case XML_CDATA_SECTION_NODE:
		case XML_ENTITY_REF_NODE:
		case XML_PI_NODE:
		case XML_COMMENT_NODE:
		case XML_DOCUMENT_NODE:
		case XML_HTML_DOCUMENT_NODE:
		    break;
		default:
		    return XML_ERR_INTERNAL_ERROR;
	}
	while(P_Node && !oneof3(P_Node->type, XML_ELEMENT_NODE, XML_DOCUMENT_NODE, XML_HTML_DOCUMENT_NODE))
		P_Node = P_Node->parent;
	if(!P_Node)
		return XML_ERR_INTERNAL_ERROR;
	doc = (P_Node->type == XML_ELEMENT_NODE) ? P_Node->doc : (xmlDoc *)P_Node;
	if(!doc)
		return XML_ERR_INTERNAL_ERROR;
	/*
	 * allocate a context and set-up everything not related to the
	 * node position in the tree
	 */
	if(doc->type == XML_DOCUMENT_NODE)
		ctxt = xmlCreateMemoryParserCtxt((char*)data, datalen);
#ifdef LIBXML_HTML_ENABLED
	else if(doc->type == XML_HTML_DOCUMENT_NODE) {
		ctxt = htmlCreateMemoryParserCtxt((char*)data, datalen);
		/*
		 * When parsing in context, it makes no sense to add implied
		 * elements like html/body/etc...
		 */
		options |= HTML_PARSE_NOIMPLIED;
	}
#endif
	else
		return XML_ERR_INTERNAL_ERROR;
	if(!ctxt)
		return(XML_ERR_NO_MEMORY);
	/*
	 * Use input doc's dict if present, else assure XML_PARSE_NODICT is set.
	 * We need a dictionary for xmlDetectSAX2, so if there's no doc dict
	 * we must wait until the last moment to free the original one.
	 */
	if(doc->dict) {
		xmlDictFree(ctxt->dict);
		ctxt->dict = doc->dict;
	}
	else
		options |= XML_PARSE_NODICT;
	if(doc->encoding) {
		xmlCharEncodingHandler * hdlr;
		SAlloc::F((xmlChar*)ctxt->encoding);
		ctxt->encoding = sstrdup((const xmlChar*)doc->encoding);
		hdlr = xmlFindCharEncodingHandler((const char*)doc->encoding);
		if(hdlr)
			xmlSwitchToEncoding(ctxt, hdlr);
		else
			return XML_ERR_UNSUPPORTED_ENCODING;
	}
	xmlCtxtUseOptionsInternal(ctxt, options, 0);
	xmlDetectSAX2(ctxt);
	ctxt->myDoc = doc;
	/* parsing in context, i.e. as within existing content */
	ctxt->instate = XML_PARSER_CONTENT;
	fake = xmlNewComment(NULL);
	if(fake == NULL) {
		xmlFreeParserCtxt(ctxt);
		return XML_ERR_NO_MEMORY;
	}
	xmlAddChild(P_Node, fake);
	if(P_Node->type == XML_ELEMENT_NODE) {
		nodePush(ctxt, P_Node);
		/*
		 * initialize the SAX2 namespaces stack
		 */
		cur = P_Node;
		while(cur && (cur->type == XML_ELEMENT_NODE)) {
			xmlNs * ns = cur->nsDef;
			const xmlChar * iprefix, * ihref;
			while(ns) {
				if(ctxt->dict) {
					iprefix = xmlDictLookupSL(ctxt->dict, ns->prefix);
					ihref = xmlDictLookupSL(ctxt->dict, ns->href);
				}
				else {
					iprefix = ns->prefix;
					ihref = ns->href;
				}
				if(xmlGetNamespace(ctxt, iprefix) == NULL) {
					nsPush(ctxt, iprefix, ihref);
					nsnr++;
				}
				ns = ns->next;
			}
			cur = cur->parent;
		}
	}
	if((ctxt->validate) || (ctxt->replaceEntities != 0)) {
		/*
		 * ID/IDREF registration will be done in xmlValidateElement below
		 */
		ctxt->loadsubset |= XML_SKIP_IDS;
	}
#ifdef LIBXML_HTML_ENABLED
	if(doc->type == XML_HTML_DOCUMENT_NODE)
		__htmlParseContent(ctxt);
	else
#endif
	xmlParseContent(ctxt);
	nsPop(ctxt, nsnr);
	if((RAW == '<') && (NXT(1) == '/')) {
		xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
	}
	else if(RAW != 0) {
		xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, 0);
	}
	if(ctxt->P_Node && (ctxt->P_Node != P_Node)) {
		xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
		ctxt->wellFormed = 0;
	}
	if(!ctxt->wellFormed) {
		ret = (ctxt->errNo == 0) ? XML_ERR_INTERNAL_ERROR : (xmlParserErrors)ctxt->errNo;
	}
	else {
		ret = XML_ERR_OK;
	}
	/*
	 * Return the newly created nodeset after unlinking it from
	 * the pseudo sibling.
	 */
	cur = fake->next;
	fake->next = NULL;
	P_Node->last = fake;
	if(cur) {
		cur->prev = NULL;
	}
	*lst = cur;
	while(cur) {
		cur->parent = NULL;
		cur = cur->next;
	}
	xmlUnlinkNode(fake);
	xmlFreeNode(fake);
	if(ret != XML_ERR_OK) {
		xmlFreeNodeList(*lst);
		*lst = NULL;
	}
	if(doc->dict)
		ctxt->dict = NULL;
	xmlFreeParserCtxt(ctxt);
	return ret;
#else /* !SAX2 */
	return XML_ERR_INTERNAL_ERROR;
#endif
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlParseBalancedChunkMemoryRecover:
 * @doc:  the document the chunk pertains to
 * @sax:  the SAX handler bloc (possibly NULL)
 * @user_data:  The user data returned on SAX callbacks (possibly NULL)
 * @depth:  Used for loop detection, use 0
 * @string:  the input string in UTF8 or ISO-Latin (zero terminated)
 * @lst:  the return value for the set of parsed nodes
 * @recover: return nodes even if the data is broken (use 0)
 *
 *
 * Parse a well-balanced chunk of an XML document
 * called by the parser
 * The allowed sequence for the Well Balanced Chunk is the one defined by
 * the content production in the XML grammar:
 *
 * [43] content ::= (element | CharData | Reference | CDSect | PI | Comment)*
 *
 * Returns 0 if the chunk is well balanced, -1 in case of args problem and
 *    the parser error code otherwise
 *
 * In case recover is set to 1, the nodelist will not be empty even if
 * the parsed chunk is not well balanced, assuming the parsing succeeded to
 * some extent.
 */
int xmlParseBalancedChunkMemoryRecover(xmlDoc * doc, xmlSAXHandlerPtr sax,
	void * user_data, int depth, const xmlChar * string, xmlNode ** lst, int recover)
{
	xmlParserCtxt * ctxt;
	xmlDoc * newDoc;
	xmlSAXHandlerPtr oldsax = NULL;
	xmlNode * content;
	xmlNode * newRoot;
	int size;
	int ret = 0;
	if(depth > 40) {
		ret = XML_ERR_ENTITY_LOOP;
	}
	else {
		ASSIGN_PTR(lst, 0);
		if(string == NULL)
			ret = -1;
		else {
			size = sstrlen(string);
			ctxt = xmlCreateMemoryParserCtxt((char*)string, size);
			if(!ctxt) 
				ret = -1;
			else {
				ctxt->userData = ctxt;
				if(sax) {
					oldsax = ctxt->sax;
					ctxt->sax = sax;
					if(user_data != NULL)
						ctxt->userData = user_data;
				}
				newDoc = xmlNewDoc(BAD_CAST "1.0");
				if(newDoc == NULL) {
					xmlFreeParserCtxt(ctxt);
					ret = -1;
				}
				else {
					newDoc->properties = XML_DOC_INTERNAL;
					if(doc && doc->dict) {
						xmlDictFree(ctxt->dict);
						ctxt->dict = doc->dict;
						xmlDictReference(ctxt->dict);
						ctxt->str_xml = xmlDictLookup(ctxt->dict, BAD_CAST "xml", 3);
						ctxt->str_xmlns = xmlDictLookup(ctxt->dict, BAD_CAST "xmlns", 5);
						ctxt->str_xml_ns = xmlDictLookup(ctxt->dict, XML_XML_NAMESPACE, 36);
						ctxt->dictNames = 1;
					}
					else {
						xmlCtxtUseOptionsInternal(ctxt, XML_PARSE_NODICT, 0);
					}
					if(doc) {
						newDoc->intSubset = doc->intSubset;
						newDoc->extSubset = doc->extSubset;
					}
					newRoot = xmlNewDocNode(newDoc, NULL, BAD_CAST "pseudoroot", 0);
					if(newRoot == NULL) {
						if(sax)
							ctxt->sax = oldsax;
						xmlFreeParserCtxt(ctxt);
						newDoc->intSubset = NULL;
						newDoc->extSubset = NULL;
						xmlFreeDoc(newDoc);
						ret = -1;
					}
					else {
						xmlAddChild((xmlNode *)newDoc, newRoot);
						nodePush(ctxt, newRoot);
						if(!doc) {
							ctxt->myDoc = newDoc;
						}
						else {
							ctxt->myDoc = newDoc;
							newDoc->children->doc = doc;
							// Ensure that doc has XML spec namespace 
							xmlSearchNsByHref(doc, (xmlNode *)doc, XML_XML_NAMESPACE);
							newDoc->oldNs = doc->oldNs;
						}
						ctxt->instate = XML_PARSER_CONTENT;
						ctxt->depth = depth;
						//
						// Doing validity checking on chunk doesn't make sense
						//
						ctxt->validate = 0;
						ctxt->loadsubset = 0;
						xmlDetectSAX2(ctxt);
						if(doc) {
							content = doc->children;
							doc->children = NULL;
							xmlParseContent(ctxt);
							doc->children = content;
						}
						else {
							xmlParseContent(ctxt);
						}
						if((RAW == '<') && (NXT(1) == '/')) {
							xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
						}
						else if(RAW != 0) {
							xmlFatalErr(ctxt, XML_ERR_EXTRA_CONTENT, 0);
						}
						if(ctxt->P_Node != newDoc->children) {
							xmlFatalErr(ctxt, XML_ERR_NOT_WELL_BALANCED, 0);
						}
						ret = ctxt->wellFormed ? 0 : ((ctxt->errNo == 0) ? 1 : ctxt->errNo);
						if(lst && (!ret || recover == 1)) {
							//
							// Return the newly created nodeset after unlinking it from they pseudo parent.
							//
							xmlNode * cur = newDoc->children->children;
							*lst = cur;
							while(cur) {
								xmlSetTreeDoc(cur, doc);
								cur->parent = NULL;
								cur = cur->next;
							}
							newDoc->children->children = NULL;
						}
						if(sax)
							ctxt->sax = oldsax;
						xmlFreeParserCtxt(ctxt);
						newDoc->intSubset = NULL;
						newDoc->extSubset = NULL;
						newDoc->oldNs = NULL;
						xmlFreeDoc(newDoc);
					}
				}
			}
		}
	}
	return ret;
}

/**
 * xmlSAXParseEntity:
 * @sax:  the SAX handler block
 * @filename:  the filename
 *
 * parse an XML external entity out of context and build a tree.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * This correspond to a "Well Balanced" chunk
 *
 * Returns the resulting document tree
 */

xmlDoc * xmlSAXParseEntity(xmlSAXHandlerPtr sax, const char * filename)
{
	xmlDoc * ret = 0;
	xmlParserCtxt * ctxt = xmlCreateFileParserCtxt(filename);
	if(ctxt) {
		if(sax) {
			SAlloc::F(ctxt->sax);
			ctxt->sax = sax;
			ctxt->userData = NULL;
		}
		xmlParseExtParsedEnt(ctxt);
		if(ctxt->wellFormed)
			ret = ctxt->myDoc;
		else {
			ret = NULL;
			xmlFreeDoc(ctxt->myDoc);
			ctxt->myDoc = NULL;
		}
		if(sax)
			ctxt->sax = NULL;
		xmlFreeParserCtxt(ctxt);
	}
	return ret;
}

/**
 * xmlParseEntity:
 * @filename:  the filename
 *
 * parse an XML external entity out of context and build a tree.
 *
 * [78] extParsedEnt ::= TextDecl? content
 *
 * This correspond to a "Well Balanced" chunk
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlParseEntity(const char * filename)
{
	return(xmlSAXParseEntity(NULL, filename));
}

#endif /* LIBXML_SAX1_ENABLED */

/**
 * xmlCreateEntityParserCtxtInternal:
 * @URL:  the entity URL
 * @ID:  the entity PUBLIC ID
 * @base:  a possible base for the target URI
 * @pctx:  parser context used to set options on new context
 *
 * Create a parser context for an external entity
 * Automatic support for ZLIB/Compress compressed document is provided
 * by default if found at compile-time.
 *
 * Returns the new parser context or NULL
 */
static xmlParserCtxt * xmlCreateEntityParserCtxtInternal(const xmlChar * URL, const xmlChar * ID, const xmlChar * base, xmlParserCtxt * pctx)
{
	xmlParserCtxt * ctxt = xmlNewParserCtxt();
	if(ctxt) {
		xmlParserInputPtr inputStream;
		char * directory = NULL;
		xmlChar * uri;
		if(pctx != NULL) {
			ctxt->options = pctx->options;
			ctxt->_private = pctx->_private;
		}
		uri = xmlBuildURI(URL, base);
		if(uri == NULL) {
			inputStream = xmlLoadExternalEntity((char*)URL, (char*)ID, ctxt);
			if(inputStream == NULL) {
				xmlFreeParserCtxt(ctxt);
				return 0;
			}
			inputPush(ctxt, inputStream);
			if(!ctxt->directory && !directory)
				directory = xmlParserGetDirectory((char*)URL);
			if(!ctxt->directory && directory)
				ctxt->directory = directory;
		}
		else {
			inputStream = xmlLoadExternalEntity((char*)uri, (char*)ID, ctxt);
			if(inputStream == NULL) {
				SAlloc::F(uri);
				xmlFreeParserCtxt(ctxt);
				return 0;
			}
			inputPush(ctxt, inputStream);
			if(!ctxt->directory && !directory)
				directory = xmlParserGetDirectory((char*)uri);
			if(!ctxt->directory && directory)
				ctxt->directory = directory;
			SAlloc::F(uri);
		}
	}
	return ctxt;
}

/**
 * xmlCreateEntityParserCtxt:
 * @URL:  the entity URL
 * @ID:  the entity PUBLIC ID
 * @base:  a possible base for the target URI
 *
 * Create a parser context for an external entity
 * Automatic support for ZLIB/Compress compressed document is provided
 * by default if found at compile-time.
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxt * xmlCreateEntityParserCtxt(const xmlChar * URL, const xmlChar * ID, const xmlChar * base)
{
	return xmlCreateEntityParserCtxtInternal(URL, ID, base, 0);
}

/************************************************************************
*									*
*		Front ends when parsing from a file			*
*									*
************************************************************************/

/**
 * xmlCreateURLParserCtxt:
 * @filename:  the filename or URL
 * @options:  a combination of xmlParserOption
 *
 * Create a parser context for a file or URL content.
 * Automatic support for ZLIB/Compress compressed document is provided
 * by default if found at compile-time and for file accesses
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxt * xmlCreateURLParserCtxt(const char * filename, int options)
{
	xmlParserCtxt * ctxt = xmlNewParserCtxt();
	if(!ctxt) {
		xmlErrMemory(NULL, "cannot allocate parser context");
	}
	else {
		xmlParserInputPtr inputStream;
		char * directory = NULL;
		if(options)
			xmlCtxtUseOptionsInternal(ctxt, options, 0);
		ctxt->linenumbers = 1;
		inputStream = xmlLoadExternalEntity(filename, NULL, ctxt);
		if(inputStream == NULL) {
			xmlFreeParserCtxt(ctxt);
			ctxt = 0;
		}
		else {
			inputPush(ctxt, inputStream);
			if(!ctxt->directory && !directory)
				directory = xmlParserGetDirectory(filename);
			if(!ctxt->directory && directory)
				ctxt->directory = directory;
		}
	}
	return ctxt;
}

/**
 * xmlCreateFileParserCtxt:
 * @filename:  the filename
 *
 * Create a parser context for a file content.
 * Automatic support for ZLIB/Compress compressed document is provided
 * by default if found at compile-time.
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxt * xmlCreateFileParserCtxt(const char * filename)
{
	return xmlCreateURLParserCtxt(filename, 0);
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlSAXParseFileWithData:
 * @sax:  the SAX handler block
 * @filename:  the filename
 * @recovery:  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 * @data:  the userdata
 *
 * parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * User data (void *) is stored within the parser context in the
 * context's _private member, so it is available nearly everywhere in libxml
 *
 * Returns the resulting document tree
 */

xmlDoc * xmlSAXParseFileWithData(xmlSAXHandlerPtr sax, const char * filename, int recovery, void * data)
{
	xmlDoc * ret = 0;
	xmlParserCtxt * ctxt;
	xmlInitParser();
	ctxt = xmlCreateFileParserCtxt(filename);
	if(ctxt) {
		if(sax) {
			SAlloc::F(ctxt->sax);
			ctxt->sax = sax;
		}
		xmlDetectSAX2(ctxt);
		if(data)
			ctxt->_private = data;
		SETIFZ(ctxt->directory, xmlParserGetDirectory(filename));
		ctxt->recovery = recovery;
		xmlParseDocument(ctxt);
		if((ctxt->wellFormed) || recovery) {
			ret = ctxt->myDoc;
			if(ret) {
				if(ctxt->input->buf->compressed > 0)
					ret->compression = 9;
				else
					ret->compression = ctxt->input->buf->compressed;
			}
		}
		else {
			ret = NULL;
			xmlFreeDoc(ctxt->myDoc);
			ctxt->myDoc = NULL;
		}
		if(sax)
			ctxt->sax = NULL;
		xmlFreeParserCtxt(ctxt);
	}
	return ret;
}

/**
 * xmlSAXParseFile:
 * @sax:  the SAX handler block
 * @filename:  the filename
 * @recovery:  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 *
 * parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * Returns the resulting document tree
 */

xmlDoc * xmlSAXParseFile(xmlSAXHandlerPtr sax, const char * filename, int recovery)
{
	return xmlSAXParseFileWithData(sax, filename, recovery, 0);
}
/**
 * xmlRecoverDoc:
 * @cur:  a pointer to an array of xmlChar
 *
 * parse an XML in-memory document and build a tree.
 * In the case the document is not Well Formed, a attempt to build a
 * tree is tried anyway
 *
 * Returns the resulting document tree or NULL in case of failure
 */

xmlDoc * xmlRecoverDoc(const xmlChar * cur)
{
	return(xmlSAXParseDoc(NULL, cur, 1));
}

/**
 * xmlParseFile:
 * @filename:  the filename
 *
 * parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 *
 * Returns the resulting document tree if the file was wellformed,
 * NULL otherwise.
 */

xmlDoc * xmlParseFile(const char * filename)
{
	return xmlSAXParseFile(NULL, filename, 0);
}

/**
 * xmlRecoverFile:
 * @filename:  the filename
 *
 * parse an XML file and build a tree. Automatic support for ZLIB/Compress
 * compressed document is provided by default if found at compile-time.
 * In the case the document is not Well Formed, it attempts to build
 * a tree anyway
 *
 * Returns the resulting document tree or NULL in case of failure
 */

xmlDoc * xmlRecoverFile(const char * filename)
{
	return xmlSAXParseFile(NULL, filename, 1);
}
/**
 * xmlSetupParserForBuffer:
 * @ctxt:  an XML parser context
 * @buffer:  a xmlChar * buffer
 * @filename:  a file name
 *
 * Setup the parser context to parse a new buffer; Clears any prior
 * contents from the parser context. The buffer parameter must not be
 * NULL, but the filename parameter can be
 */
void xmlSetupParserForBuffer(xmlParserCtxt * ctxt, const xmlChar* buffer, const char* filename)
{
	if(ctxt && buffer) {
		xmlParserInputPtr input = xmlNewInputStream(ctxt);
		if(!input) {
			xmlErrMemory(NULL, "parsing new buffer: out of memory\n");
			xmlClearParserCtxt(ctxt);
		}
		else {
			xmlClearParserCtxt(ctxt);
			if(filename != NULL)
				input->filename = (char*)xmlCanonicPath((const xmlChar*)filename);
			input->base = buffer;
			input->cur = buffer;
			input->end = &buffer[sstrlen(buffer)];
			inputPush(ctxt, input);
		}
	}
}
/**
 * xmlSAXUserParseFile:
 * @sax:  a SAX handler
 * @user_data:  The user data returned on SAX callbacks
 * @filename:  a file name
 *
 * parse an XML file and call the given SAX handler routines.
 * Automatic support for ZLIB/Compress compressed document is provided
 *
 * Returns 0 in case of success or a error number otherwise
 */
int xmlSAXUserParseFile(xmlSAXHandlerPtr sax, void * user_data, const char * filename)
{
	int ret = 0;
	xmlParserCtxt * ctxt = xmlCreateFileParserCtxt(filename);
	if(!ctxt)
		ret = -1;
	else {
		if(ctxt->sax != (xmlSAXHandlerPtr)&xmlDefaultSAXHandler)
			SAlloc::F(ctxt->sax);
		ctxt->sax = sax;
		xmlDetectSAX2(ctxt);
		if(user_data)
			ctxt->userData = user_data;
		xmlParseDocument(ctxt);
		ret = ctxt->wellFormed ? 0 : NZOR(ctxt->errNo, -1);
		if(sax)
			ctxt->sax = NULL;
		if(ctxt->myDoc) {
			xmlFreeDoc(ctxt->myDoc);
			ctxt->myDoc = NULL;
		}
		xmlFreeParserCtxt(ctxt);
	}
	return ret;
}

#endif /* LIBXML_SAX1_ENABLED */

/************************************************************************
*									*
*		Front ends when parsing from memory			*
*									*
************************************************************************/

/**
 * xmlCreateMemoryParserCtxt:
 * @buffer:  a pointer to a char array
 * @size:  the size of the array
 *
 * Create a parser context for an XML in-memory document.
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxt * xmlCreateMemoryParserCtxt(const char * buffer, int size)
{
	xmlParserCtxt * ctxt = 0;
	if(buffer && size > 0) {
		ctxt = xmlNewParserCtxt();
		if(ctxt) {
			// @todo xmlParserInputBufferCreateStatic, requires some serious changes 
			xmlParserInputBuffer * buf = xmlParserInputBufferCreateMem(buffer, size, XML_CHAR_ENCODING_NONE);
			if(!buf) {
				xmlFreeParserCtxt(ctxt);
				ctxt = 0;
			}
			else {
				xmlParserInput * input = xmlNewInputStream(ctxt);
				if(!input) {
					xmlFreeParserInputBuffer(buf);
					xmlFreeParserCtxt(ctxt);
					ctxt = 0;
				}
				else {
					input->filename = NULL;
					input->buf = buf;
					xmlBufResetInput(input->buf->buffer, input);
					inputPush(ctxt, input);
				}
			}
		}
	}
	return ctxt;
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlSAXParseMemoryWithData:
 * @sax:  the SAX handler block
 * @buffer:  an pointer to a char array
 * @size:  the size of the array
 * @recovery:  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 * @data:  the userdata
 *
 * parse an XML in-memory block and use the given SAX function block
 * to handle the parsing callback. If sax is NULL, fallback to the default
 * DOM tree building routines.
 *
 * User data (void *) is stored within the parser context in the
 * context's _private member, so it is available nearly everywhere in libxml
 *
 * Returns the resulting document tree
 */

xmlDoc * xmlSAXParseMemoryWithData(xmlSAXHandlerPtr sax, const char * buffer, int size, int recovery, void * data)
{
	xmlDoc * ret = 0;
	xmlInitParser();
	xmlParserCtxt * ctxt = xmlCreateMemoryParserCtxt(buffer, size);
	if(ctxt) {
		if(sax) {
			SAlloc::F(ctxt->sax);
			ctxt->sax = sax;
		}
		xmlDetectSAX2(ctxt);
		if(data)
			ctxt->_private = data;
		ctxt->recovery = recovery;
		xmlParseDocument(ctxt);
		if(ctxt->wellFormed || recovery)
			ret = ctxt->myDoc;
		else {
			ret = NULL;
			xmlFreeDoc(ctxt->myDoc);
			ctxt->myDoc = NULL;
		}
		if(sax)
			ctxt->sax = NULL;
		xmlFreeParserCtxt(ctxt);
	}
	return ret;
}

/**
 * xmlSAXParseMemory:
 * @sax:  the SAX handler block
 * @buffer:  an pointer to a char array
 * @size:  the size of the array
 * @recovery:  work in recovery mode, i.e. tries to read not Well Formed
 *             documents
 *
 * parse an XML in-memory block and use the given SAX function block
 * to handle the parsing callback. If sax is NULL, fallback to the default
 * DOM tree building routines.
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlSAXParseMemory(xmlSAXHandlerPtr sax, const char * buffer, int size, int recovery)
{
	return xmlSAXParseMemoryWithData(sax, buffer, size, recovery, 0);
}

/**
 * xmlParseMemory:
 * @buffer:  an pointer to a char array
 * @size:  the size of the array
 *
 * parse an XML in-memory block and build a tree.
 *
 * Returns the resulting document tree
 */

xmlDoc * xmlParseMemory(const char * buffer, int size)
{
	return xmlSAXParseMemory(NULL, buffer, size, 0);
}

/**
 * xmlRecoverMemory:
 * @buffer:  an pointer to a char array
 * @size:  the size of the array
 *
 * parse an XML in-memory block and build a tree.
 * In the case the document is not Well Formed, an attempt to
 * build a tree is tried anyway
 *
 * Returns the resulting document tree or NULL in case of error
 */

xmlDoc * xmlRecoverMemory(const char * buffer, int size)
{
	return xmlSAXParseMemory(NULL, buffer, size, 1);
}
/**
 * xmlSAXUserParseMemory:
 * @sax:  a SAX handler
 * @user_data:  The user data returned on SAX callbacks
 * @buffer:  an in-memory XML document input
 * @size:  the length of the XML document in bytes
 *
 * A better SAX parsing routine.
 * parse an XML in-memory buffer and call the given SAX handler routines.
 *
 * Returns 0 in case of success or a error number otherwise
 */
int xmlSAXUserParseMemory(xmlSAXHandlerPtr sax, void * user_data, const char * buffer, int size)
{
	int ret = 0;
	xmlParserCtxt * ctxt;
	xmlInitParser();
	ctxt = xmlCreateMemoryParserCtxt(buffer, size);
	if(!ctxt)
		ret = -1;
	else {
		if(ctxt->sax != (xmlSAXHandlerPtr)&xmlDefaultSAXHandler)
			SAlloc::F(ctxt->sax);
		ctxt->sax = sax;
		xmlDetectSAX2(ctxt);
		if(user_data)
			ctxt->userData = user_data;
		xmlParseDocument(ctxt);
		ret = ctxt->wellFormed ? 0 : (ctxt->errNo ? ctxt->errNo : -1);
		if(sax)
			ctxt->sax = NULL;
		if(ctxt->myDoc) {
			xmlFreeDoc(ctxt->myDoc);
			ctxt->myDoc = NULL;
		}
		xmlFreeParserCtxt(ctxt);
	}
	return ret;
}

#endif /* LIBXML_SAX1_ENABLED */

/**
 * xmlCreateDocParserCtxt:
 * @cur:  a pointer to an array of xmlChar
 *
 * Creates a parser context for an XML in-memory document.
 *
 * Returns the new parser context or NULL
 */
xmlParserCtxt * xmlCreateDocParserCtxt(const xmlChar * cur) 
{
	return cur ? xmlCreateMemoryParserCtxt((const char*)cur, sstrlen(cur)) : 0;
}

#ifdef LIBXML_SAX1_ENABLED
/**
 * xmlSAXParseDoc:
 * @sax:  the SAX handler block
 * @cur:  a pointer to an array of xmlChar
 * @recovery:  work in recovery mode, i.e. tries to read no Well Formed
 *             documents
 *
 * parse an XML in-memory document and build a tree.
 * It use the given SAX function block to handle the parsing callback.
 * If sax is NULL, fallback to the default DOM tree building routines.
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlSAXParseDoc(xmlSAXHandlerPtr sax, const xmlChar * cur, int recovery)
{
	xmlDoc * ret = 0;
	if(cur) {
		xmlParserCtxt * ctxt = xmlCreateDocParserCtxt(cur);
		if(ctxt) {
			xmlSAXHandler * oldsax = 0;
			if(sax) {
				oldsax = ctxt->sax;
				ctxt->sax = sax;
				ctxt->userData = NULL;
			}
			xmlDetectSAX2(ctxt);
			xmlParseDocument(ctxt);
			if((ctxt->wellFormed) || recovery)
				ret = ctxt->myDoc;
			else {
				ret = NULL;
				xmlFreeDoc(ctxt->myDoc);
				ctxt->myDoc = NULL;
			}
			if(sax)
				ctxt->sax = oldsax;
			xmlFreeParserCtxt(ctxt);
		}
	}
	return ret;
}
/**
 * xmlParseDoc:
 * @cur:  a pointer to an array of xmlChar
 *
 * parse an XML in-memory document and build a tree.
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlParseDoc(const xmlChar * cur)
{
	return xmlSAXParseDoc(NULL, cur, 0);
}

#endif /* LIBXML_SAX1_ENABLED */

#ifdef LIBXML_LEGACY_ENABLED
/************************************************************************
*									*
*	Specific function to keep track of entities references		*
*	and used by the XSLT debugger					*
*									*
************************************************************************/

static xmlEntityReferenceFunc xmlEntityRefFunc = NULL;
/**
 * xmlAddEntityReference:
 * @ent : A valid entity
 * @firstNode : A valid first node for children of entity
 * @lastNode : A valid last node of children entity
 *
 * Notify of a reference to an entity of type XML_EXTERNAL_GENERAL_PARSED_ENTITY
 */
static void xmlAddEntityReference(xmlEntity * ent, xmlNode * firstNode, xmlNode * lastNode)
{
	if(xmlEntityRefFunc)
		(*xmlEntityRefFunc)(ent, firstNode, lastNode);
}
/**
 * xmlSetEntityReferenceFunc:
 * @func: A valid function
 *
 * Set the function to call call back when a xml reference has been made
 */
void xmlSetEntityReferenceFunc(xmlEntityReferenceFunc func)
{
	xmlEntityRefFunc = func;
}

#endif /* LIBXML_LEGACY_ENABLED */

/************************************************************************
*									*
*				Miscellaneous				*
*									*
************************************************************************/

#ifdef LIBXML_XPATH_ENABLED
	//#include <libxml/xpath.h>
#endif

extern void XMLCDECL xmlGenericErrorDefaultFunc(void * ctx, const char * msg, ...);
static int xmlParserInitialized = 0;

/**
 * xmlInitParser:
 *
 * Initialization function for the XML parser.
 * This is not reentrant. Call once before processing in case of
 * use in multithreaded programs.
 */

void xmlInitParser()
{
	if(xmlParserInitialized != 0)
		return;
#ifdef LIBXML_THREAD_ENABLED
	__xmlGlobalInitMutexLock();
	if(xmlParserInitialized == 0) {
#endif
	xmlInitThreads();
	xmlInitGlobals();
	if(xmlGenericError == xmlGenericErrorDefaultFunc || !xmlGenericError)
		initGenericErrorDefaultFunc(NULL);
	xmlInitMemory();
	xmlInitializeDict();
	xmlInitCharEncodingHandlers();
	xmlDefaultSAXHandlerInit();
	xmlRegisterDefaultInputCallbacks();
#ifdef LIBXML_OUTPUT_ENABLED
	xmlRegisterDefaultOutputCallbacks();
#endif /* LIBXML_OUTPUT_ENABLED */
#ifdef LIBXML_HTML_ENABLED
	htmlInitAutoClose();
	htmlDefaultSAXHandlerInit();
#endif
#ifdef LIBXML_XPATH_ENABLED
	xmlXPathInit();
#endif
#ifdef LIBXML_CATALOG_ENABLED
	xmlInitializeCatalog();
#endif
	xmlParserInitialized = 1;
#ifdef LIBXML_THREAD_ENABLED
}
__xmlGlobalInitMutexUnlock();
#endif
}

/**
 * xmlCleanupParser:
 *
 * This function name is somewhat misleading. It does not clean up
 * parser state, it cleans up memory allocated by the library itself.
 * It is a cleanup function for the XML library. It tries to reclaim all
 * related global memory allocated for the library processing.
 * It doesn't deallocate any document related memory. One should
 * call xmlCleanupParser() only when the process has finished using
 * the library and all XML/HTML documents built with it.
 * See also xmlInitParser() which has the opposite function of preparing
 * the library for operations.
 *
 * WARNING: if your application is multithreaded or has plugin support
 *          calling this may crash the application if another thread or
 *          a plugin is still using libxml2. It's sometimes very hard to
 *          guess if libxml2 is in use in the application, some libraries
 *          or plugins may use it without notice. In case of doubt abstain
 *          from calling this function or do it just before calling exit()
 *          to avoid leak reports from valgrind !
 */
void xmlCleanupParser()
{
	if(xmlParserInitialized) {
		xmlCleanupCharEncodingHandlers();
#ifdef LIBXML_CATALOG_ENABLED
		xmlCatalogCleanup();
#endif
		xmlDictCleanup();
		xmlCleanupInputCallbacks();
#ifdef LIBXML_OUTPUT_ENABLED
		xmlCleanupOutputCallbacks();
#endif
#ifdef LIBXML_SCHEMAS_ENABLED
		xmlSchemaCleanupTypes();
		xmlRelaxNGCleanupTypes();
#endif
		xmlResetLastError();
		xmlCleanupGlobals();
		xmlCleanupThreads(); // must be last if called not from the main thread
		xmlCleanupMemory();
		xmlParserInitialized = 0;
	}
}

/************************************************************************
*									*
*	New set (2.6.0) of simpler and more flexible APIs		*
*									*
************************************************************************/

/**
 * @str:  a string
 *
 * Free a string if it is not owned by the "dict" dictionnary in the current scope
 */
//#define DICT_FREE(str) if((str) && ((!dict) || (xmlDictOwns(dict, (const xmlChar*)(str)) == 0))) SAlloc::F((char*)(str));
/**
 * xmlCtxtReset:
 * @ctxt: an XML parser context
 *
 * Reset a parser context
 */
void xmlCtxtReset(xmlParserCtxt * ctxt)
{
	xmlParserInputPtr input;
	if(ctxt) {
		xmlDict * dict = ctxt->dict;
		while((input = inputPop(ctxt)) != NULL) { /* Non consuming */
			xmlFreeInputStream(input);
		}
		ctxt->inputNr = 0;
		ctxt->input = NULL;
		ctxt->spaceNr = 0;
		if(ctxt->spaceTab) {
			ctxt->spaceTab[0] = -1;
			ctxt->space = &ctxt->spaceTab[0];
		}
		else {
			ctxt->space = NULL;
		}
		ctxt->nodeNr = 0;
		ctxt->P_Node = NULL;
		ctxt->nameNr = 0;
		ctxt->name = NULL;
		XmlDestroyStringWithDict(dict, (xmlChar *)ctxt->version); // @badcast
		ctxt->version = NULL;
		XmlDestroyStringWithDict(dict, (xmlChar *)ctxt->encoding); // @badcast
		ctxt->encoding = NULL;
		XmlDestroyStringWithDict(dict, (xmlChar *)ctxt->directory); // @badcast
		ctxt->directory = NULL;
		XmlDestroyStringWithDict(dict, ctxt->extSubURI);
		ctxt->extSubURI = NULL;
		XmlDestroyStringWithDict(dict, ctxt->extSubSystem);
		ctxt->extSubSystem = NULL;
		xmlFreeDoc(ctxt->myDoc);
		ctxt->myDoc = NULL;
		ctxt->standalone = -1;
		ctxt->hasExternalSubset = 0;
		ctxt->hasPErefs = 0;
		ctxt->html = 0;
		ctxt->external = 0;
		ctxt->instate = XML_PARSER_START;
		ctxt->token = 0;
		ctxt->wellFormed = 1;
		ctxt->nsWellFormed = 1;
		ctxt->disableSAX = 0;
		ctxt->valid = 1;
#if 0
		ctxt->vctxt.userData = ctxt;
		ctxt->vctxt.error = xmlParserValidityError;
		ctxt->vctxt.warning = xmlParserValidityWarning;
#endif
		ctxt->record_info = 0;
		ctxt->nbChars = 0;
		ctxt->CheckIndex = 0;
		ctxt->inSubset = 0;
		ctxt->errNo = XML_ERR_OK;
		ctxt->depth = 0;
		ctxt->charset = XML_CHAR_ENCODING_UTF8;
		ctxt->catalogs = NULL;
		ctxt->nbentities = 0;
		ctxt->sizeentities = 0;
		ctxt->sizeentcopy = 0;
		xmlInitNodeInfoSeq(&ctxt->node_seq);
		xmlHashFree(ctxt->attsDefault, (xmlHashDeallocator)free);
		ctxt->attsDefault = NULL;
		xmlHashFree(ctxt->attsSpecial, 0);
		ctxt->attsSpecial = NULL;
#ifdef LIBXML_CATALOG_ENABLED
		xmlCatalogFreeLocal(ctxt->catalogs);
#endif
		if(ctxt->lastError.code != XML_ERR_OK)
			xmlResetError(&ctxt->lastError);
	}
}
/**
 * xmlCtxtResetPush:
 * @ctxt: an XML parser context
 * @chunk:  a pointer to an array of chars
 * @size:  number of chars in the array
 * @filename:  an optional file name or URI
 * @encoding:  the document encoding, or NULL
 *
 * Reset a push parser context
 *
 * Returns 0 in case of success and 1 in case of error
 */
int xmlCtxtResetPush(xmlParserCtxt * ctxt, const char * chunk, int size, const char * filename, const char * encoding)
{
	xmlParserInputPtr inputStream;
	xmlParserInputBufferPtr buf;
	xmlCharEncoding enc = XML_CHAR_ENCODING_NONE;
	if(!ctxt)
		return 1;
	if(!encoding && chunk && size >= 4)
		enc = xmlDetectCharEncoding((const xmlChar*)chunk, size);
	buf = xmlAllocParserInputBuffer(enc);
	if(!buf)
		return 1;
	if(!ctxt) {
		xmlFreeParserInputBuffer(buf);
		return 1;
	}
	xmlCtxtReset(ctxt);
	if(ctxt->pushTab == NULL) {
		ctxt->pushTab = (void**)SAlloc::M(ctxt->nameMax * 3 * sizeof(xmlChar *));
		if(ctxt->pushTab == NULL) {
			xmlErrMemory(ctxt, 0);
			xmlFreeParserInputBuffer(buf);
			return 1;
		}
	}
	ctxt->directory = filename ? xmlParserGetDirectory(filename) : 0;
	inputStream = xmlNewInputStream(ctxt);
	if(inputStream == NULL) {
		xmlFreeParserInputBuffer(buf);
		return 1;
	}
	inputStream->filename = filename ? (char *)xmlCanonicPath((const xmlChar*)filename) : 0;
	inputStream->buf = buf;
	xmlBufResetInput(buf->buffer, inputStream);
	inputPush(ctxt, inputStream);
	if(size > 0 && chunk && ctxt->input && ctxt->input->buf) {
		size_t base = xmlBufGetInputBase(ctxt->input->buf->buffer, ctxt->input);
		size_t cur = ctxt->input->cur - ctxt->input->base;
		xmlParserInputBufferPush(ctxt->input->buf, size, chunk);
		xmlBufSetInputBaseCur(ctxt->input->buf->buffer, ctxt->input, base, cur);
#ifdef DEBUG_PUSH
		xmlGenericError(0, "PP: pushed %d\n", size);
#endif
	}
	if(encoding) {
		xmlCharEncodingHandler * hdlr;
		SAlloc::F((xmlChar*)ctxt->encoding);
		ctxt->encoding = sstrdup((const xmlChar*)encoding);
		hdlr = xmlFindCharEncodingHandler(encoding);
		if(hdlr)
			xmlSwitchToEncoding(ctxt, hdlr);
		else
			xmlFatalErrMsgStr(ctxt, XML_ERR_UNSUPPORTED_ENCODING, "Unsupported encoding %s\n", BAD_CAST encoding);
	}
	else if(enc != XML_CHAR_ENCODING_NONE) {
		xmlSwitchEncoding(ctxt, enc);
	}
	return 0;
}

/**
 * xmlCtxtUseOptionsInternal:
 * @ctxt: an XML parser context
 * @options:  a combination of xmlParserOption
 * @encoding:  the user provided encoding to use
 *
 * Applies the options to the parser context
 *
 * Returns 0 in case of success, the set of unknown or unimplemented options
 *         in case of error.
 */
static int xmlCtxtUseOptionsInternal(xmlParserCtxt * ctxt, int options, const char * encoding)
{
	if(!ctxt)
		return -1;
	if(encoding) {
		SAlloc::F((xmlChar*)ctxt->encoding);
		ctxt->encoding = sstrdup((const xmlChar*)encoding);
	}
	if(options & XML_PARSE_RECOVER) {
		ctxt->recovery = 1;
		options -= XML_PARSE_RECOVER;
		ctxt->options |= XML_PARSE_RECOVER;
	}
	else
		ctxt->recovery = 0;
	if(options & XML_PARSE_DTDLOAD) {
		ctxt->loadsubset = XML_DETECT_IDS;
		options -= XML_PARSE_DTDLOAD;
		ctxt->options |= XML_PARSE_DTDLOAD;
	}
	else
		ctxt->loadsubset = 0;
	if(options & XML_PARSE_DTDATTR) {
		ctxt->loadsubset |= XML_COMPLETE_ATTRS;
		options -= XML_PARSE_DTDATTR;
		ctxt->options |= XML_PARSE_DTDATTR;
	}
	if(options & XML_PARSE_NOENT) {
		ctxt->replaceEntities = 1;
		/* ctxt->loadsubset |= XML_DETECT_IDS; */
		options -= XML_PARSE_NOENT;
		ctxt->options |= XML_PARSE_NOENT;
	}
	else
		ctxt->replaceEntities = 0;
	if(options & XML_PARSE_PEDANTIC) {
		ctxt->pedantic = 1;
		options -= XML_PARSE_PEDANTIC;
		ctxt->options |= XML_PARSE_PEDANTIC;
	}
	else
		ctxt->pedantic = 0;
	if(options & XML_PARSE_NOBLANKS) {
		ctxt->keepBlanks = 0;
		ctxt->sax->ignorableWhitespace = xmlSAX2IgnorableWhitespace;
		options -= XML_PARSE_NOBLANKS;
		ctxt->options |= XML_PARSE_NOBLANKS;
	}
	else
		ctxt->keepBlanks = 1;
	if(options & XML_PARSE_DTDVALID) {
		ctxt->validate = 1;
		if(options & XML_PARSE_NOWARNING)
			ctxt->vctxt.warning = NULL;
		if(options & XML_PARSE_NOERROR)
			ctxt->vctxt.error = NULL;
		options -= XML_PARSE_DTDVALID;
		ctxt->options |= XML_PARSE_DTDVALID;
	}
	else
		ctxt->validate = 0;
	if(options & XML_PARSE_NOWARNING) {
		ctxt->sax->warning = NULL;
		options -= XML_PARSE_NOWARNING;
	}
	if(options & XML_PARSE_NOERROR) {
		ctxt->sax->error = NULL;
		ctxt->sax->fatalError = NULL;
		options -= XML_PARSE_NOERROR;
	}
#ifdef LIBXML_SAX1_ENABLED
	if(options & XML_PARSE_SAX1) {
		ctxt->sax->startElement = xmlSAX2StartElement;
		ctxt->sax->endElement = xmlSAX2EndElement;
		ctxt->sax->startElementNs = NULL;
		ctxt->sax->endElementNs = NULL;
		ctxt->sax->initialized = 1;
		options -= XML_PARSE_SAX1;
		ctxt->options |= XML_PARSE_SAX1;
	}
#endif /* LIBXML_SAX1_ENABLED */
	if(options & XML_PARSE_NODICT) {
		ctxt->dictNames = 0;
		options -= XML_PARSE_NODICT;
		ctxt->options |= XML_PARSE_NODICT;
	}
	else {
		ctxt->dictNames = 1;
	}
	if(options & XML_PARSE_NOCDATA) {
		ctxt->sax->cdataBlock = NULL;
		options -= XML_PARSE_NOCDATA;
		ctxt->options |= XML_PARSE_NOCDATA;
	}
	if(options & XML_PARSE_NSCLEAN) {
		ctxt->options |= XML_PARSE_NSCLEAN;
		options -= XML_PARSE_NSCLEAN;
	}
	if(options & XML_PARSE_NONET) {
		ctxt->options |= XML_PARSE_NONET;
		options -= XML_PARSE_NONET;
	}
	if(options & XML_PARSE_COMPACT) {
		ctxt->options |= XML_PARSE_COMPACT;
		options -= XML_PARSE_COMPACT;
	}
	if(options & XML_PARSE_OLD10) {
		ctxt->options |= XML_PARSE_OLD10;
		options -= XML_PARSE_OLD10;
	}
	if(options & XML_PARSE_NOBASEFIX) {
		ctxt->options |= XML_PARSE_NOBASEFIX;
		options -= XML_PARSE_NOBASEFIX;
	}
	if(options & XML_PARSE_HUGE) {
		ctxt->options |= XML_PARSE_HUGE;
		options -= XML_PARSE_HUGE;
		xmlDictSetLimit(ctxt->dict, 0);
	}
	if(options & XML_PARSE_OLDSAX) {
		ctxt->options |= XML_PARSE_OLDSAX;
		options -= XML_PARSE_OLDSAX;
	}
	if(options & XML_PARSE_IGNORE_ENC) {
		ctxt->options |= XML_PARSE_IGNORE_ENC;
		options -= XML_PARSE_IGNORE_ENC;
	}
	if(options & XML_PARSE_BIG_LINES) {
		ctxt->options |= XML_PARSE_BIG_LINES;
		options -= XML_PARSE_BIG_LINES;
	}
	ctxt->linenumbers = 1;
	return (options);
}
/**
 * xmlCtxtUseOptions:
 * @ctxt: an XML parser context
 * @options:  a combination of xmlParserOption
 *
 * Applies the options to the parser context
 *
 * Returns 0 in case of success, the set of unknown or unimplemented options
 *         in case of error.
 */
int xmlCtxtUseOptions(xmlParserCtxt * ctxt, int options)
{
	return xmlCtxtUseOptionsInternal(ctxt, options, NULL);
}
/**
 * xmlDoRead:
 * @ctxt:  an XML parser context
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 * @reuse:  keep the context for reuse
 *
 * Common front-end for the xmlRead functions
 *
 * Returns the resulting document tree or NULL
 */
static xmlDoc * FASTCALL xmlDoRead(xmlParserCtxt * ctxt, const char * URL, const char * encoding, int options, int reuse)
{
	xmlDoc * ret = 0;
	if(ctxt) {
		xmlCtxtUseOptionsInternal(ctxt, options, encoding);
		if(encoding) {
			xmlCharEncodingHandler * hdlr = xmlFindCharEncodingHandler(encoding);
			if(hdlr)
				xmlSwitchToEncoding(ctxt, hdlr);
		}
		if(URL && ctxt->input && ctxt->input->filename == 0)
			ctxt->input->filename = (char *)sstrdup((const xmlChar*)URL);
		xmlParseDocument(ctxt);
		if(ctxt->wellFormed || ctxt->recovery)
			ret = ctxt->myDoc;
		else {
			ret = NULL;
			xmlFreeDoc(ctxt->myDoc);
		}
		ctxt->myDoc = NULL;
		if(!reuse)
			xmlFreeParserCtxt(ctxt);
	}
	return ret;
}
/**
 * xmlReadDoc:
 * @cur:  a pointer to a zero terminated string
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML in-memory document and build a tree.
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlReadDoc(const xmlChar * cur, const char * URL, const char * encoding, int options)
{
	if(!cur)
		return 0;
	xmlInitParser();
	xmlParserCtxt * ctxt = xmlCreateDocParserCtxt(cur);
	return xmlDoRead(ctxt, URL, encoding, options, 0);
}
/**
 * xmlReadFile:
 * @filename:  a file or URL
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML file from the filesystem or the network.
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlReadFile(const char * filename, const char * encoding, int options)
{
	xmlInitParser();
	xmlParserCtxt * ctxt = xmlCreateURLParserCtxt(filename, options);
	return xmlDoRead(ctxt, NULL, encoding, options, 0);
}

/**
 * xmlReadMemory:
 * @buffer:  a pointer to a char array
 * @size:  the size of the array
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML in-memory document and build a tree.
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlReadMemory(const char * buffer, int size, const char * URL, const char * encoding, int options)
{
	xmlInitParser();
	xmlParserCtxt * ctxt = xmlCreateMemoryParserCtxt(buffer, size);
	return xmlDoRead(ctxt, URL, encoding, options, 0);
}

/**
 * xmlReadFd:
 * @fd:  an open file descriptor
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML from a file descriptor and build a tree.
 * NOTE that the file descriptor will not be closed when the
 *      reader is closed or reset.
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlReadFd(int fd, const char * URL, const char * encoding, int options)
{
	xmlParserCtxt * ctxt;
	xmlParserInputBufferPtr input;
	xmlParserInputPtr stream;
	if(fd < 0)
		return 0;
	xmlInitParser();
	input = xmlParserInputBufferCreateFd(fd, XML_CHAR_ENCODING_NONE);
	if(!input)
		return 0;
	input->closecallback = NULL;
	ctxt = xmlNewParserCtxt();
	if(!ctxt) {
		xmlFreeParserInputBuffer(input);
		return 0;
	}
	stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
	if(stream == NULL) {
		xmlFreeParserInputBuffer(input);
		xmlFreeParserCtxt(ctxt);
		return 0;
	}
	inputPush(ctxt, stream);
	return xmlDoRead(ctxt, URL, encoding, options, 0);
}

/**
 * xmlReadIO:
 * @ioread:  an I/O read function
 * @ioclose:  an I/O close function
 * @ioctx:  an I/O handler
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML document from I/O functions and source and build a tree.
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlReadIO(xmlInputReadCallback ioread, xmlInputCloseCallback ioclose,
    void * ioctx, const char * URL, const char * encoding, int options)
{
	xmlParserCtxt * ctxt;
	xmlParserInputBufferPtr input;
	xmlParserInputPtr stream;
	if(ioread == NULL)
		return 0;
	xmlInitParser();
	input = xmlParserInputBufferCreateIO(ioread, ioclose, ioctx, XML_CHAR_ENCODING_NONE);
	if(!input) {
		if(ioclose)
			ioclose(ioctx);
		return 0;
	}
	ctxt = xmlNewParserCtxt();
	if(!ctxt) {
		xmlFreeParserInputBuffer(input);
		return 0;
	}
	stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
	if(stream == NULL) {
		xmlFreeParserInputBuffer(input);
		xmlFreeParserCtxt(ctxt);
		return 0;
	}
	inputPush(ctxt, stream);
	return xmlDoRead(ctxt, URL, encoding, options, 0);
}
/**
 * xmlCtxtReadDoc:
 * @ctxt:  an XML parser context
 * @cur:  a pointer to a zero terminated string
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML in-memory document and build a tree.
 * This reuses the existing @ctxt parser context
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlCtxtReadDoc(xmlParserCtxt * ctxt, const xmlChar * cur, const char * URL, const char * encoding, int options)
{
	if(cur && ctxt) {
		xmlInitParser();
		xmlCtxtReset(ctxt);
		{
			xmlParserInput * stream = xmlNewStringInputStream(ctxt, cur);
			if(stream) {
				inputPush(ctxt, stream);
				return xmlDoRead(ctxt, URL, encoding, options, 1);
			}
			else
				return 0;
		}
	}
	else
		return 0;
}
/**
 * xmlCtxtReadFile:
 * @ctxt:  an XML parser context
 * @filename:  a file or URL
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML file from the filesystem or the network.
 * This reuses the existing @ctxt parser context
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlCtxtReadFile(xmlParserCtxt * ctxt, const char * filename, const char * encoding, int options)
{
	xmlDoc * p_doc = 0;
	if(filename && ctxt) {
		xmlInitParser();
		xmlCtxtReset(ctxt);
		xmlParserInputPtr stream = xmlLoadExternalEntity(filename, NULL, ctxt);
		if(stream) {
			inputPush(ctxt, stream);
			p_doc = xmlDoRead(ctxt, NULL, encoding, options, 1);
		}
	}
	return p_doc;
}
/**
 * xmlCtxtReadMemory:
 * @ctxt:  an XML parser context
 * @buffer:  a pointer to a char array
 * @size:  the size of the array
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML in-memory document and build a tree.
 * This reuses the existing @ctxt parser context
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlCtxtReadMemory(xmlParserCtxt * ctxt, const char * buffer, int size, const char * URL, const char * encoding, int options)
{
	xmlParserInputBufferPtr input;
	xmlParserInputPtr stream;
	if(!ctxt)
		return 0;
	if(!buffer)
		return 0;
	xmlInitParser();
	xmlCtxtReset(ctxt);
	input = xmlParserInputBufferCreateMem(buffer, size, XML_CHAR_ENCODING_NONE);
	if(!input) {
		return 0;
	}
	stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
	if(stream == NULL) {
		xmlFreeParserInputBuffer(input);
		return 0;
	}
	inputPush(ctxt, stream);
	return xmlDoRead(ctxt, URL, encoding, options, 1);
}
/**
 * xmlCtxtReadFd:
 * @ctxt:  an XML parser context
 * @fd:  an open file descriptor
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML from a file descriptor and build a tree.
 * This reuses the existing @ctxt parser context
 * NOTE that the file descriptor will not be closed when the
 *      reader is closed or reset.
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlCtxtReadFd(xmlParserCtxt * ctxt, int fd, const char * URL, const char * encoding, int options)
{
	xmlParserInputBufferPtr input;
	xmlParserInputPtr stream;
	if(fd < 0)
		return 0;
	if(!ctxt)
		return 0;
	xmlInitParser();
	xmlCtxtReset(ctxt);
	input = xmlParserInputBufferCreateFd(fd, XML_CHAR_ENCODING_NONE);
	if(!input)
		return 0;
	input->closecallback = NULL;
	stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
	if(stream == NULL) {
		xmlFreeParserInputBuffer(input);
		return 0;
	}
	inputPush(ctxt, stream);
	return xmlDoRead(ctxt, URL, encoding, options, 1);
}

/**
 * xmlCtxtReadIO:
 * @ctxt:  an XML parser context
 * @ioread:  an I/O read function
 * @ioclose:  an I/O close function
 * @ioctx:  an I/O handler
 * @URL:  the base URL to use for the document
 * @encoding:  the document encoding, or NULL
 * @options:  a combination of xmlParserOption
 *
 * parse an XML document from I/O functions and source and build a tree.
 * This reuses the existing @ctxt parser context
 *
 * Returns the resulting document tree
 */
xmlDoc * xmlCtxtReadIO(xmlParserCtxt * ctxt, xmlInputReadCallback ioread,
    xmlInputCloseCallback ioclose, void * ioctx, const char * URL, const char * encoding, int options)
{
	xmlParserInputBufferPtr input;
	xmlParserInputPtr stream;
	if(ioread == NULL)
		return 0;
	if(!ctxt)
		return 0;
	xmlInitParser();
	xmlCtxtReset(ctxt);
	input = xmlParserInputBufferCreateIO(ioread, ioclose, ioctx, XML_CHAR_ENCODING_NONE);
	if(!input) {
		if(ioclose)
			ioclose(ioctx);
		return 0;
	}
	stream = xmlNewIOInputStream(ctxt, input, XML_CHAR_ENCODING_NONE);
	if(stream == NULL) {
		xmlFreeParserInputBuffer(input);
		return 0;
	}
	inputPush(ctxt, stream);
	return xmlDoRead(ctxt, URL, encoding, options, 1);
}

#define bottom_parser
//#include "elfgcchack.h"
