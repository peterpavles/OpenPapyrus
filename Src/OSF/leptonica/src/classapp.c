/*====================================================================*
   -  Copyright (C) 2001 Leptonica.  All rights reserved.
   -
   -  Redistribution and use in source and binary forms, with or without
   -  modification, are permitted provided that the following conditions
   -  are met:
   -  1. Redistributions of source code must retain the above copyright
   -     notice, this list of conditions and the following disclaimer.
   -  2. Redistributions in binary form must reproduce the above
   -     copyright notice, this list of conditions and the following
   -     disclaimer in the documentation and/or other materials
   -     provided with the distribution.
   -
   -  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   -  ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   -  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   -  A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL ANY
   -  CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   -  EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   -  PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   -  PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
   -  OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   -  NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   -  SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*====================================================================*/

/*!
 * \file classapp.c
 * <pre>
 *
 *      Top-level jb2 correlation and rank-hausdorff
 *
 *         int32         jbCorrelation()
 *         int32         jbRankHaus()
 *
 *      Extract and classify words in textline order
 *
 *         JBCLASSER      *jbWordsInTextlines()
 *         int32         pixGetWordsInTextlines()
 *         int32         pixGetWordBoxesInTextlines()
 *
 *      Use word bounding boxes to compare page images
 *
 *         NUMAA          *boxaExtractSortedPattern()
 *         int32         numaaCompareImagesByBoxes()
 *         static int32  testLineAlignmentX()
 *         static int32  countAlignedMatches()
 *         static void     printRowIndices()
 * </pre>
 */

#include "allheaders.h"
#pragma hdrstop

static const int32 L_BUF_SIZE = 512;        /*!< size of filename buffer    */
static const int32 JB_WORDS_MIN_WIDTH = 5;  /*!< min. word width in pixels  */
static const int32 JB_WORDS_MIN_HEIGHT = 3; /*!< min. word height in pixels */

/* Static comparison functions */
static int32 testLineAlignmentX(NUMA * na1, NUMA * na2, int32 shiftx,
    int32 delx, int32 nperline);
static int32 countAlignedMatches(NUMA * nai1, NUMA * nai2, NUMA * nasx,
    NUMA * nasy, int32 n1, int32 n2,
    int32 delx, int32 dely,
    int32 nreq, int32 * psame,
    int32 debugflag);
static void printRowIndices(int32 * index1, int32 n1,
    int32 * index2, int32 n2);

/*------------------------------------------------------------------*
*          Top-level jb2 correlation and rank-hausdorff            *
*------------------------------------------------------------------*/
/*!
 * \brief   jbCorrelation()
 *
 * \param[in]    dirin directory of input images
 * \param[in]    thresh typically ~0.8
 * \param[in]    weight typically ~0.6
 * \param[in]    components JB_CONN_COMPS, JB_CHARACTERS, JB_WORDS
 * \param[in]    rootname for output files
 * \param[in]    firstpage 0-based
 * \param[in]    npages use 0 for all pages in dirin
 * \param[in]    renderflag 1 to render from templates; 0 to skip
 * \return  0 if OK, 1 on error
 *
 * <pre>
 * Notes:
 *      (1) The images must be 1 bpp.  If they are not, you can convert
 *          them using convertFilesTo1bpp().
 *      (2) See prog/jbcorrelation for generating more output (e.g.,
 *          for debugging)
 * </pre>
 */
int32 jbCorrelation(const char  * dirin,
    float thresh,
    float weight,
    int32 components,
    const char  * rootname,
    int32 firstpage,
    int32 npages,
    int32 renderflag)
{
	char filename[L_BUF_SIZE];
	int32 nfiles, i, numpages;
	JBDATA     * data;
	JBCLASSER  * classer;
	PIX        * pix;
	PIXA       * pixa;
	SARRAY     * safiles;

	PROCNAME("jbCorrelation");

	if(!dirin)
		return ERROR_INT("dirin not defined", procName, 1);
	if(!rootname)
		return ERROR_INT("rootname not defined", procName, 1);
	if(components != JB_CONN_COMPS && components != JB_CHARACTERS &&
	    components != JB_WORDS)
		return ERROR_INT("components invalid", procName, 1);

	safiles = getSortedPathnamesInDirectory(dirin, NULL, firstpage, npages);
	nfiles = sarrayGetCount(safiles);

	/* Classify components */
	classer = jbCorrelationInit(components, 0, 0, thresh, weight);
	jbAddPages(classer, safiles);

	/* Save data */
	data = jbDataSave(classer);
	jbDataWrite(rootname, data);

	/* Optionally, render pages using class templates */
	if(renderflag) {
		pixa = jbDataRender(data, FALSE);
		numpages = pixaGetCount(pixa);
		if(numpages != nfiles)
			fprintf(stderr, "numpages = %d, nfiles = %d, not equal!\n",
			    numpages, nfiles);
		for(i = 0; i < numpages; i++) {
			pix = pixaGetPix(pixa, i, L_CLONE);
			_snprintf(filename, L_BUF_SIZE, "%s.%04d", rootname, i);
			fprintf(stderr, "filename: %s\n", filename);
			pixWrite(filename, pix, IFF_PNG);
			pixDestroy(&pix);
		}
		pixaDestroy(&pixa);
	}

	sarrayDestroy(&safiles);
	jbClasserDestroy(&classer);
	jbDataDestroy(&data);
	return 0;
}

/*!
 * \brief   jbRankHaus()
 *
 * \param[in]    dirin directory of input images
 * \param[in]    size of Sel used for dilation; typ. 2
 * \param[in]    rank rank value of match; typ. 0.97
 * \param[in]    components JB_CONN_COMPS, JB_CHARACTERS, JB_WORDS
 * \param[in]    rootname for output files
 * \param[in]    firstpage 0-based
 * \param[in]    npages use 0 for all pages in dirin
 * \param[in]    renderflag 1 to render from templates; 0 to skip
 * \return  0 if OK, 1 on error
 *
 * <pre>
 * Notes:
 *      (1) See prog/jbrankhaus for generating more output (e.g.,
 *          for debugging)
 * </pre>
 */
int32 jbRankHaus(const char  * dirin,
    int32 size,
    float rank,
    int32 components,
    const char  * rootname,
    int32 firstpage,
    int32 npages,
    int32 renderflag)
{
	char filename[L_BUF_SIZE];
	int32 nfiles, i, numpages;
	JBDATA     * data;
	JBCLASSER  * classer;
	PIX        * pix;
	PIXA       * pixa;
	SARRAY     * safiles;

	PROCNAME("jbRankHaus");

	if(!dirin)
		return ERROR_INT("dirin not defined", procName, 1);
	if(!rootname)
		return ERROR_INT("rootname not defined", procName, 1);
	if(components != JB_CONN_COMPS && components != JB_CHARACTERS &&
	    components != JB_WORDS)
		return ERROR_INT("components invalid", procName, 1);

	safiles = getSortedPathnamesInDirectory(dirin, NULL, firstpage, npages);
	nfiles = sarrayGetCount(safiles);

	/* Classify components */
	classer = jbRankHausInit(components, 0, 0, size, rank);
	jbAddPages(classer, safiles);

	/* Save data */
	data = jbDataSave(classer);
	jbDataWrite(rootname, data);

	/* Optionally, render pages using class templates */
	if(renderflag) {
		pixa = jbDataRender(data, FALSE);
		numpages = pixaGetCount(pixa);
		if(numpages != nfiles)
			fprintf(stderr, "numpages = %d, nfiles = %d, not equal!\n",
			    numpages, nfiles);
		for(i = 0; i < numpages; i++) {
			pix = pixaGetPix(pixa, i, L_CLONE);
			_snprintf(filename, L_BUF_SIZE, "%s.%04d", rootname, i);
			fprintf(stderr, "filename: %s\n", filename);
			pixWrite(filename, pix, IFF_PNG);
			pixDestroy(&pix);
		}
		pixaDestroy(&pixa);
	}

	sarrayDestroy(&safiles);
	jbClasserDestroy(&classer);
	jbDataDestroy(&data);
	return 0;
}

/*------------------------------------------------------------------*
*           Extract and classify words in textline order           *
*------------------------------------------------------------------*/
/*!
 * \brief   jbWordsInTextlines()
 *
 * \param[in]    dirin directory of input pages
 * \param[in]    reduction 1 for full res; 2 for half-res
 * \param[in]    maxwidth of word mask components, to be kept
 * \param[in]    maxheight of word mask components, to be kept
 * \param[in]    thresh on correlation; 0.80 is reasonable
 * \param[in]    weight for handling thick text; 0.6 is reasonable
 * \param[out]   pnatl numa with textline index for each component
 * \param[in]    firstpage 0-based
 * \param[in]    npages use 0 for all pages in dirin
 * \return  classer for the set of pages
 *
 * <pre>
 * Notes:
 *      (1) This is a high-level function.  See prog/jbwords for example
 *          of usage.
 *      (2) Typically, words can be found reasonably well at a resolution
 *          of about 150 ppi.  For highest accuracy, you should use 300 ppi.
 *          Assuming that the input images are 300 ppi, use reduction = 1
 *          for finding words at full res, and reduction = 2 for finding
 *          them at 150 ppi.
 * </pre>
 */
JBCLASSER * jbWordsInTextlines(const char  * dirin,
    int32 reduction,
    int32 maxwidth,
    int32 maxheight,
    float thresh,
    float weight,
    NUMA       ** pnatl,
    int32 firstpage,
    int32 npages)
{
	char       * fname;
	int32 nfiles, i, w, h;
	BOXA       * boxa;
	JBCLASSER  * classer;
	NUMA       * nai, * natl;
	PIX        * pix;
	PIXA       * pixa;
	SARRAY     * safiles;

	PROCNAME("jbWordsInTextlines");

	if(!pnatl)
		return (JBCLASSER*)ERROR_PTR("&natl not defined", procName, NULL);
	*pnatl = NULL;
	if(!dirin)
		return (JBCLASSER*)ERROR_PTR("dirin not defined", procName, NULL);
	if(reduction != 1 && reduction != 2)
		return (JBCLASSER*)ERROR_PTR("reduction not in {1,2}", procName, NULL);

	safiles = getSortedPathnamesInDirectory(dirin, NULL, firstpage, npages);
	nfiles = sarrayGetCount(safiles);

	/* Classify components */
	classer = jbCorrelationInit(JB_WORDS, maxwidth, maxheight, thresh, weight);
	classer->safiles = sarrayCopy(safiles);
	natl = numaCreate(0);
	*pnatl = natl;
	for(i = 0; i < nfiles; i++) {
		fname = sarrayGetString(safiles, i, L_NOCOPY);
		if((pix = pixRead(fname)) == NULL) {
			L_WARNING2("image file %d not read\n", procName, i);
			continue;
		}
		pixGetDimensions(pix, &w, &h, NULL);
		if(reduction == 1) {
			classer->w = w;
			classer->h = h;
		}
		else { /* reduction == 2 */
			classer->w = w / 2;
			classer->h = h / 2;
		}
		pixGetWordsInTextlines(pix, reduction, JB_WORDS_MIN_WIDTH,
		    JB_WORDS_MIN_HEIGHT, maxwidth, maxheight,
		    &boxa, &pixa, &nai);
		jbAddPageComponents(classer, pix, boxa, pixa);
		numaJoin(natl, nai, 0, -1);
		pixDestroy(&pix);
		numaDestroy(&nai);
		boxaDestroy(&boxa);
		pixaDestroy(&pixa);
	}

	sarrayDestroy(&safiles);
	return classer;
}

/*!
 * \brief   pixGetWordsInTextlines()
 *
 * \param[in]    pixs 1 bpp, typ. 300 ppi
 * \param[in]    reduction 1 for input res; 2 for 2x reduction of input res
 * \param[in]    minwidth, minheight of saved components; smaller are discarded
 * \param[in]    maxwidth, maxheight of saved components; larger are discarded
 * \param[out]   pboxad word boxes sorted in textline line order
 * \param[out]   ppixad word images sorted in textline line order
 * \param[out]   pnai index of textline for each word
 * \return  0 if OK, 1 on error
 *
 * <pre>
 * Notes:
 *      (1) The input should be at a resolution of about 300 ppi.
 *          The word masks and word images can be computed at either
 *          150 ppi or 300 ppi.  For the former, set reduction = 2.
 *      (2) The four size constraints on saved components are all
 *          scaled by %reduction.
 *      (3) The result are word images (and their b.b.), extracted in
 *          textline order, at either full res or 2x reduction,
 *          and with a numa giving the textline index for each word.
 *      (4) The pixa and boxa interfaces should make this type of
 *          application simple to put together.  The steps are:
 *           ~ optionally reduce by 2x
 *           ~ generate first estimate of word masks
 *           ~ get b.b. of these, and remove the small and big ones
 *           ~ extract pixa of the word images, using the b.b.
 *           ~ sort actual word images in textline order (2d)
 *           ~ flatten them to a pixa (1d), saving the textline index
 *             for each pix
 *      (5) In an actual application, it may be desirable to pre-filter
 *          the input image to remove large components, to extract
 *          single columns of text, and to deskew them.  For example,
 *          to remove both large components and small noisy components
 *          that can interfere with the statistics used to estimate
 *          parameters for segmenting by words, but still retain text lines,
 *          the following image preprocessing can be done:
 *                Pix *pixt = pixMorphSequence(pixs, "c40.1", 0);
 *                Pix *pixf = pixSelectBySize(pixt, 0, 60, 8,
 *                                     L_SELECT_HEIGHT, L_SELECT_IF_LT, NULL);
 *                pixAnd(pixf, pixf, pixs);  // the filtered image
 *          The closing turns text lines into long blobs, but does not
 *          significantly increase their height.  But if there are many
 *          small connected components in a dense texture, this is likely
 *          to generate tall components that will be eliminated in pixf.
 * </pre>
 */
int32 pixGetWordsInTextlines(PIX     * pixs,
    int32 reduction,
    int32 minwidth,
    int32 minheight,
    int32 maxwidth,
    int32 maxheight,
    BOXA   ** pboxad,
    PIXA   ** ppixad,
    NUMA   ** pnai)
{
	int32 maxdil;
	BOXA    * boxa1, * boxad;
	BOXAA   * baa;
	NUMA    * nai;
	NUMAA   * naa;
	PIXA    * pixa1, * pixad;
	PIX     * pix1;
	PIXAA   * paa;

	PROCNAME("pixGetWordsInTextlines");

	if(!pboxad || !ppixad || !pnai)
		return ERROR_INT("&boxad, &pixad, &nai not all defined", procName, 1);
	*pboxad = NULL;
	*ppixad = NULL;
	*pnai = NULL;
	if(!pixs)
		return ERROR_INT("pixs not defined", procName, 1);
	if(reduction != 1 && reduction != 2)
		return ERROR_INT("reduction not in {1,2}", procName, 1);

	if(reduction == 1) {
		pix1 = pixClone(pixs);
		maxdil = 18;
	}
	else { /* reduction == 2 */
		pix1 = pixReduceRankBinaryCascade(pixs, 1, 0, 0, 0);
		maxdil = 9;
	}

	/* Get the bounding boxes of the words from the word mask. */
	pixWordBoxesByDilation(pix1, maxdil, minwidth, minheight,
	    maxwidth, maxheight, &boxa1, NULL);

	/* Generate a pixa of the word images */
	pixa1 = pixaCreateFromBoxa(pix1, boxa1, NULL); /* mask over each word */

	/* Sort the bounding boxes of these words by line.  We use the
	 * index mapping to allow identical sorting of the pixa. */
	baa = boxaSort2d(boxa1, &naa, -1, -1, 4);
	paa = pixaSort2dByIndex(pixa1, naa, L_CLONE);

	/* Flatten the word paa */
	pixad = pixaaFlattenToPixa(paa, &nai, L_CLONE);
	boxad = pixaGetBoxa(pixad, L_COPY);

	*pnai = nai;
	*pboxad = boxad;
	*ppixad = pixad;

	pixDestroy(&pix1);
	pixaDestroy(&pixa1);
	boxaDestroy(&boxa1);
	boxaaDestroy(&baa);
	pixaaDestroy(&paa);
	numaaDestroy(&naa);
	return 0;
}

/*!
 * \brief   pixGetWordBoxesInTextlines()
 *
 * \param[in]    pixs 1 bpp, typ. 300 ppi
 * \param[in]    reduction 1 for input res; 2 for 2x reduction of input res
 * \param[in]    minwidth, minheight of saved components; smaller are discarded
 * \param[in]    maxwidth, maxheight of saved components; larger are discarded
 * \param[out]   pboxad word boxes sorted in textline line order
 * \param[out]   pnai [optional] index of textline for each word
 * \return  0 if OK, 1 on error
 *
 * <pre>
 * Notes:
 *      (1) The input should be at a resolution of about 300 ppi.
 *          The word masks can be computed at either 150 ppi or 300 ppi.
 *          For the former, set reduction = 2.
 *      (2) This is a special version of pixGetWordsInTextlines(), that
 *          just finds the word boxes in line order, with a numa
 *          giving the textline index for each word.
 *          See pixGetWordsInTextlines() for more details.
 * </pre>
 */
int32 pixGetWordBoxesInTextlines(PIX     * pixs,
    int32 reduction,
    int32 minwidth,
    int32 minheight,
    int32 maxwidth,
    int32 maxheight,
    BOXA   ** pboxad,
    NUMA   ** pnai)
{
	int32 maxdil;
	BOXA    * boxa1;
	BOXAA   * baa;
	NUMA    * nai;
	PIX     * pix1;

	PROCNAME("pixGetWordBoxesInTextlines");

	if(pnai) *pnai = NULL;
	if(!pboxad)
		return ERROR_INT("&boxad and &nai not both defined", procName, 1);
	*pboxad = NULL;
	if(!pixs)
		return ERROR_INT("pixs not defined", procName, 1);
	if(reduction != 1 && reduction != 2)
		return ERROR_INT("reduction not in {1,2}", procName, 1);

	if(reduction == 1) {
		pix1 = pixClone(pixs);
		maxdil = 18;
	}
	else { /* reduction == 2 */
		pix1 = pixReduceRankBinaryCascade(pixs, 1, 0, 0, 0);
		maxdil = 9;
	}

	/* Get the bounding boxes of the words from the word mask. */
	pixWordBoxesByDilation(pix1, maxdil, minwidth, minheight,
	    maxwidth, maxheight, &boxa1, NULL);

	/* 2D sort the bounding boxes of these words. */
	baa = boxaSort2d(boxa1, NULL, 3, -5, 5);

	/* Flatten the boxaa, saving the boxa index for each box */
	*pboxad = boxaaFlattenToBoxa(baa, &nai, L_CLONE);

	if(pnai)
		*pnai = nai;
	else
		numaDestroy(&nai);
	pixDestroy(&pix1);
	boxaDestroy(&boxa1);
	boxaaDestroy(&baa);
	return 0;
}

/*------------------------------------------------------------------*
*           Use word bounding boxes to compare page images         *
*------------------------------------------------------------------*/
/*!
 * \brief   boxaExtractSortedPattern()
 *
 * \param[in]    boxa typ. of word bounding boxes, in textline order
 * \param[in]    na   index of textline for each box in boxa
 * \return  naa NUMAA, where each numa represents one textline,
 *                   or NULL on error
 *
 * <pre>
 * Notes:
 *      (1) The input is expected to come from pixGetWordBoxesInTextlines().
 *      (2) Each numa in the output consists of an average y coordinate
 *          of the first box in the textline, followed by pairs of
 *          x coordinates representing the left and right edges of each
 *          of the boxes in the textline.
 * </pre>
 */
NUMAA * boxaExtractSortedPattern(BOXA  * boxa,
    NUMA  * na)
{
	int32 index, nbox, row, prevrow, x, y, w, h;
	BOX     * box;
	NUMA    * nad;
	NUMAA   * naa;

	PROCNAME("boxaExtractSortedPattern");

	if(!boxa)
		return (NUMAA*)ERROR_PTR("boxa not defined", procName, NULL);
	if(!na)
		return (NUMAA*)ERROR_PTR("na not defined", procName, NULL);

	naa = numaaCreate(0);
	nbox = boxaGetCount(boxa);
	if(nbox == 0)
		return naa;

	prevrow = -1;
	for(index = 0; index < nbox; index++) {
		box = boxaGetBox(boxa, index, L_CLONE);
		numaGetIValue(na, index, &row);
		if(row > prevrow) {
			if(index > 0)
				numaaAddNuma(naa, nad, L_INSERT);
			nad = numaCreate(0);
			prevrow = row;
			boxGetGeometry(box, NULL, &y, NULL, &h);
			numaAddNumber(nad, y + h / 2);
		}
		boxGetGeometry(box, &x, NULL, &w, NULL);
		numaAddNumber(nad, x);
		numaAddNumber(nad, x + w - 1);
		boxDestroy(&box);
	}
	numaaAddNuma(naa, nad, L_INSERT);

	return naa;
}

/*!
 * \brief   numaaCompareImagesByBoxes()
 *
 * \param[in]    naa1 for image 1, formatted by boxaExtractSortedPattern()
 * \param[in]    naa2 ditto; for image 2
 * \param[in]    nperline number of box regions to be used in each textline
 * \param[in]    nreq number of complete row matches required
 * \param[in]    maxshiftx max allowed x shift between two patterns, in pixels
 * \param[in]    maxshifty max allowed y shift between two patterns, in pixels
 * \param[in]    delx max allowed difference in x data, after alignment
 * \param[in]    dely max allowed difference in y data, after alignment
 * \param[out]   psame 1 if %nreq row matches are found; 0 otherwise
 * \param[in]    debugflag 1 for debug output
 * \return  0 if OK, 1 on error
 *
 * <pre>
 * Notes:
 *      (1) Each input numaa describes a set of sorted bounding boxes
 *          (sorted by textline and, within each textline, from
 *          left to right) in the images from which they are derived.
 *          See boxaExtractSortedPattern() for a description of the data
 *          format in each of the input numaa.
 *      (2) This function does an alignment between the input
 *          descriptions of bounding boxes for two images. The
 *          input parameter %nperline specifies the number of boxes
 *          to consider in each line when testing for a match, and
 *          %nreq is the required number of lines that must be well-aligned
 *          to get a match.
 *      (3) Testing by alignment has 3 steps:
 *          (a) Generating the location of word bounding boxes from the
 *              images (prior to calling this function).
 *          (b) Listing all possible pairs of aligned rows, based on
 *              tolerances in horizontal and vertical positions of
 *              the boxes.  Specifically, all pairs of rows are enumerated
 *              whose first %nperline boxes can be brought into close
 *              alignment, based on the delx parameter for boxes in the
 *              line and within the overall the %maxshiftx and %maxshifty
 *              constraints.
 *          (c) Each pair, starting with the first, is used to search
 *              for a set of %nreq - 1 other pairs that can all be aligned
 *              with a difference in global translation of not more
 *              than (%delx, %dely).
 * </pre>
 */
int32 numaaCompareImagesByBoxes(NUMAA    * naa1,
    NUMAA    * naa2,
    int32 nperline,
    int32 nreq,
    int32 maxshiftx,
    int32 maxshifty,
    int32 delx,
    int32 dely,
    int32  * psame,
    int32 debugflag)
{
	int32 n1, n2, i, j, nbox, y1, y2, xl1, xl2;
	int32 shiftx, shifty, match;
	int32  * line1, * line2; /* indicator for sufficient boxes in a line */
	int32  * yloc1, * yloc2; /* arrays of y value for first box in a line */
	int32  * xleft1, * xleft2; /* arrays of x value for left side of first box */
	NUMA     * na1, * na2, * nai1, * nai2, * nasx, * nasy;

	PROCNAME("numaaCompareImagesByBoxes");

	if(!psame)
		return ERROR_INT("&same not defined", procName, 1);
	*psame = 0;
	if(!naa1)
		return ERROR_INT("naa1 not defined", procName, 1);
	if(!naa2)
		return ERROR_INT("naa2 not defined", procName, 1);
	if(nperline < 1)
		return ERROR_INT("nperline < 1", procName, 1);
	if(nreq < 1)
		return ERROR_INT("nreq < 1", procName, 1);

	n1 = numaaGetCount(naa1);
	n2 = numaaGetCount(naa2);
	if(n1 < nreq || n2 < nreq)
		return 0;

	/* Find the lines in naa1 and naa2 with sufficient boxes.
	 * Also, find the y-values for each of the lines, and the
	 * LH x-values of the first box in each line. */
	line1 = (int32*)LEPT_CALLOC(n1, sizeof(int32));
	line2 = (int32*)LEPT_CALLOC(n2, sizeof(int32));
	yloc1 = (int32*)LEPT_CALLOC(n1, sizeof(int32));
	yloc2 = (int32*)LEPT_CALLOC(n2, sizeof(int32));
	xleft1 = (int32*)LEPT_CALLOC(n1, sizeof(int32));
	xleft2 = (int32*)LEPT_CALLOC(n2, sizeof(int32));
	for(i = 0; i < n1; i++) {
		na1 = numaaGetNuma(naa1, i, L_CLONE);
		numaGetIValue(na1, 0, yloc1 + i);
		numaGetIValue(na1, 1, xleft1 + i);
		nbox = (numaGetCount(na1) - 1) / 2;
		if(nbox >= nperline)
			line1[i] = 1;
		numaDestroy(&na1);
	}
	for(i = 0; i < n2; i++) {
		na2 = numaaGetNuma(naa2, i, L_CLONE);
		numaGetIValue(na2, 0, yloc2 + i);
		numaGetIValue(na2, 1, xleft2 + i);
		nbox = (numaGetCount(na2) - 1) / 2;
		if(nbox >= nperline)
			line2[i] = 1;
		numaDestroy(&na2);
	}

	/* Enumerate all possible line matches.  A 'possible' line
	 * match is one where the x and y shifts for the first box
	 * in each line are within the maxshiftx and maxshifty
	 * constraints, and the left and right sides of the remaining
	 * (nperline - 1) successive boxes are within delx of each other.
	 * The result is a set of four numas giving parameters of
	 * each set of matching lines. */
	nai1 = numaCreate(0); /* line index 1 of match */
	nai2 = numaCreate(0); /* line index 2 of match */
	nasx = numaCreate(0); /* shiftx for match */
	nasy = numaCreate(0); /* shifty for match */
	for(i = 0; i < n1; i++) {
		if(line1[i] == 0) continue;
		y1 = yloc1[i];
		xl1 = xleft1[i];
		na1 = numaaGetNuma(naa1, i, L_CLONE);
		for(j = 0; j < n2; j++) {
			if(line2[j] == 0) continue;
			y2 = yloc2[j];
			if(L_ABS(y1 - y2) > maxshifty) continue;
			xl2 = xleft2[j];
			if(L_ABS(xl1 - xl2) > maxshiftx) continue;
			shiftx = xl1 - xl2; /* shift to add to x2 values */
			shifty = y1 - y2; /* shift to add to y2 values */
			na2 = numaaGetNuma(naa2, j, L_CLONE);

			/* Now check if 'nperline' boxes in the two lines match */
			match = testLineAlignmentX(na1, na2, shiftx, delx, nperline);
			if(match) {
				numaAddNumber(nai1, i);
				numaAddNumber(nai2, j);
				numaAddNumber(nasx, shiftx);
				numaAddNumber(nasy, shifty);
			}
			numaDestroy(&na2);
		}
		numaDestroy(&na1);
	}

	/* Determine if there are a sufficient number of mutually
	 * aligned matches.  Mutually aligned matches place an additional
	 * constraint on the 'possible' matches, where the relative
	 * shifts must not exceed the (delx, dely) distances. */
	countAlignedMatches(nai1, nai2, nasx, nasy, n1, n2, delx, dely,
	    nreq, psame, debugflag);

	LEPT_FREE(line1);
	LEPT_FREE(line2);
	LEPT_FREE(yloc1);
	LEPT_FREE(yloc2);
	LEPT_FREE(xleft1);
	LEPT_FREE(xleft2);
	numaDestroy(&nai1);
	numaDestroy(&nai2);
	numaDestroy(&nasx);
	numaDestroy(&nasy);
	return 0;
}

static int32 testLineAlignmentX(NUMA    * na1,
    NUMA    * na2,
    int32 shiftx,
    int32 delx,
    int32 nperline)
{
	int32 i, xl1, xr1, xl2, xr2, diffl, diffr;

	PROCNAME("testLineAlignmentX");

	if(!na1)
		return ERROR_INT("na1 not defined", procName, 1);
	if(!na2)
		return ERROR_INT("na2 not defined", procName, 1);

	for(i = 0; i < nperline; i++) {
		numaGetIValue(na1, i + 1, &xl1);
		numaGetIValue(na1, i + 2, &xr1);
		numaGetIValue(na2, i + 1, &xl2);
		numaGetIValue(na2, i + 2, &xr2);
		diffl = L_ABS(xl1 - xl2 - shiftx);
		diffr = L_ABS(xr1 - xr2 - shiftx);
		if(diffl > delx || diffr > delx)
			return 0;
	}

	return 1;
}

/*
 *  countAlignedMatches()
 *      Input:  nai1, nai2 (numas of row pairs for matches)
 *              nasx, nasy (numas of x and y shifts for the matches)
 *              n1, n2 (number of rows in images 1 and 2)
 *              delx, dely (allowed difference in shifts of the match,
 *                          compared to the reference match)
 *              nreq (number of required aligned matches)
 *              &same (<return> 1 if %nreq row matches are found; 0 otherwise)
 *      Return: 0 if OK, 1 on error
 *
 *  Notes:
 *      (1) This takes 4 input arrays giving parameters of all the
 *          line matches.  It looks for the maximum set of aligned
 *          matches (matches with approximately the same overall shifts)
 *          that do not use rows from either image more than once.
 */
static int32 countAlignedMatches(NUMA     * nai1,
    NUMA     * nai2,
    NUMA     * nasx,
    NUMA     * nasy,
    int32 n1,
    int32 n2,
    int32 delx,
    int32 dely,
    int32 nreq,
    int32  * psame,
    int32 debugflag)
{
	int32 i, j, nm, shiftx, shifty, nmatch, diffx, diffy;
	int32  * ia1, * ia2, * iasx, * iasy, * index1, * index2;

	PROCNAME("countAlignedMatches");

	if(!nai1 || !nai2 || !nasx || !nasy)
		return ERROR_INT("4 input numas not defined", procName, 1);
	if(!psame)
		return ERROR_INT("&same not defined", procName, 1);
	*psame = 0;

	/* Check for sufficient aligned matches, doing a double iteration
	 * over the set of raw matches.  The row index arrays
	 * are used to verify that the same rows in either image
	 * are not used in more than one match.  Whenever there
	 * is a match that is properly aligned, those rows are
	 * marked in the index arrays.  */
	nm = numaGetCount(nai1); /* number of matches */
	if(nm < nreq)
		return 0;

	ia1 = numaGetIArray(nai1);
	ia2 = numaGetIArray(nai2);
	iasx = numaGetIArray(nasx);
	iasy = numaGetIArray(nasy);
	index1 = (int32*)LEPT_CALLOC(n1, sizeof(int32)); /* keep track of rows */
	index2 = (int32*)LEPT_CALLOC(n2, sizeof(int32));
	for(i = 0; i < nm; i++) {
		if(*psame == 1)
			break;

		/* Reset row index arrays */
		memzero(index1, 4 * n1);
		memzero(index2, 4 * n2);
		nmatch = 1;
		index1[ia1[i]] = nmatch; /* mark these rows as taken */
		index2[ia2[i]] = nmatch;
		shiftx = iasx[i]; /* reference shift between two rows */
		shifty = iasy[i]; /* ditto */
		if(nreq == 1) {
			*psame = 1;
			break;
		}
		for(j = 0; j < nm; j++) {
			if(j == i) continue;
			/* Rows must both be different from any previously seen */
			if(index1[ia1[j]] > 0 || index2[ia2[j]] > 0) continue;
			/* Check the shift for this match */
			diffx = L_ABS(shiftx - iasx[j]);
			diffy = L_ABS(shifty - iasy[j]);
			if(diffx > delx || diffy > dely) continue;
			/* We have a match */
			nmatch++;
			index1[ia1[j]] = nmatch; /* mark the rows */
			index2[ia2[j]] = nmatch;
			if(nmatch >= nreq) {
				*psame = 1;
				if(debugflag)
					printRowIndices(index1, n1, index2, n2);
				break;
			}
		}
	}

	LEPT_FREE(ia1);
	LEPT_FREE(ia2);
	LEPT_FREE(iasx);
	LEPT_FREE(iasy);
	LEPT_FREE(index1);
	LEPT_FREE(index2);
	return 0;
}

static void printRowIndices(int32  * index1,
    int32 n1,
    int32  * index2,
    int32 n2)
{
	int32 i;

	fprintf(stderr, "Index1: ");
	for(i = 0; i < n1; i++) {
		if(i && (i % 20 == 0))
			fprintf(stderr, "\n        ");
		fprintf(stderr, "%3d", index1[i]);
	}
	fprintf(stderr, "\n");

	fprintf(stderr, "Index2: ");
	for(i = 0; i < n2; i++) {
		if(i && (i % 20 == 0))
			fprintf(stderr, "\n        ");
		fprintf(stderr, "%3d", index2[i]);
	}
	fprintf(stderr, "\n");
	return;
}
