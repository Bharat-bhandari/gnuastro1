/*********************************************************************
Image Crop - Crop a given size from one or multiple images.
Image Crop is part of GNU Astronomy Utilities (AstrUtils) package.

Copyright (C) 2013-2015 Mohammad Akhlaghi
Tohoku University Astronomical Institute, Sendai, Japan.
http://astr.tohoku.ac.jp/~akhlaghi/

AstrUtils is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

AstrUtils is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with AstrUtils. If not, see <http://www.gnu.org/licenses/>.
**********************************************************************/
#include <math.h>
#include <stdio.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <stdlib.h>

#include "box.h"
#include "timing.h"
#include "checkset.h"
#include "fitsarrayvv.h"

#include "main.h"
#include "crop.h"
























/*******************************************************************/
/************     Set/correct first and last pixel    **************/
/*******************************************************************/
/* Read the section string and set the starting and ending pixels
   based on that. */
void
sectionparser(char *section, long *naxes, long *fpixel, long *lpixel)
{
  int add;
  long read;
  size_t dim=0;
  char *tailptr;
  char forl='f', *pt=section;

  /* Initialize the fpixel and lpixel arrays: */
  lpixel[0]=naxes[0];
  lpixel[1]=naxes[1];
  fpixel[0]=fpixel[1]=1;

  /* Parse the string. */
  while(*pt!='\0')
    {
      switch(*pt)
	{
	case ',':
	  ++dim;
	  if(dim==2)
	    error(EXIT_FAILURE, 0, "Extra `,` in `%s`.", section);
	  forl='f';
	  ++pt;
	  break;
	case ':':
	  forl='l';
	  ++pt;
	  break;
	case '.':
	  error(EXIT_FAILURE, 0, "The numbers in the argument to "
		"`--section` (`-s') have to be integers. You input "
		"includes a float number: %s.",
		section);
	  break;
	case ' ':
	  ++pt;
	  break;
	default:
	  /* If it is a star, then add the given value to the maximum
	     size of the image. */
	  if(*pt=='*')
	    {
	      add=1;
	      ++pt;
	    }
	  else add=0;

	  /* Read the number: */
	  read=strtol(pt, &tailptr, 0);

	  /* Check if there actually was a number! */
	  /*printf("\n\n------\n%c: %ld (%s)\n", *pt, read, tailptr);*/
	  if(tailptr==pt)	/* No number was read! */
	    {
	      pt=NULL;
	      break;
	    }

	  /* Put it in the correct array. Note that for the last
	     point, we don't want to include the given pixel. Unlike
	     CFITSIO, in ImageCrop, the given intervals are not
	     inclusive. fpixel and lpixel will be directly passed to
	     CFITSIO. So we have to correct his here.*/
	  if(forl=='f')
	    fpixel[dim] = add ? naxes[dim]+read : read;
	  else
	    lpixel[dim] = add ? naxes[dim] + read - 1 : read - 1;
	  pt=tailptr;
	  /*
	  printf("\n\n[%ld, %ld]: fpixel=(%ld, %ld), lpixel=(%ld, %ld)\n\n",
		 naxes[0], naxes[1],
		 fpixel[0], fpixel[1], lpixel[0], lpixel[1]);
	  */
	}
    }

  if(fpixel[0]>=lpixel[0] || fpixel[1]>=lpixel[1])
    error(EXIT_FAILURE, 0, "The bottom left corner coordinates "
	  "cannot be larger than the top right's! Your section "
	  "string (%s) has been read as: bottom left coordinate "
	  "(%ld, %ld) to top right coordinate (%ld, %ld).",
	  section, fpixel[0], fpixel[1], lpixel[0], lpixel[1]);

  /*
  printf("\n%s\nfpixel=(%ld, %ld), lpixel=(%ld, %ld)\n\n", section,
	 fpixel[0], fpixel[1], lpixel[0], lpixel[1]);
  exit(0);
  */
}




















/*******************************************************************/
/******************          One crop.         *********************/
/*******************************************************************/
void
changezerotonan(void *array, size_t size, int bitpix)
{
  float *fp, *ffp;
  double *dp, *fdp;

  switch(bitpix)
    {
    case FLOAT_IMG:
      ffp=(fp=array)+size;
      do
	if(*fp==0.0f) *fp=NAN;
      while(++fp<ffp);
      break;

    case DOUBLE_IMG:
      fdp=(dp=array)+size;
      do
	if(*dp==0.0f) *dp=NAN;
      while(++dp<fdp);
      break;

    default:
      error(EXIT_FAILURE, 0, "In changezerotonan, bitpix is not "
	    "recognized! This is out of the users control and is a bug, "
	    "please report it to us so we see how it was caused and fix it.");
    }
}






/* Set the output name and image output sizes. */
void
cropname(struct cropparams *crp)
{
  struct imgcropparams *p=crp->p;
  struct commonparams *cp=&p->cp;
  struct imgcroplog *log=&crp->p->log[crp->outindex];

  /* Set the output name and crop sides: */
  if(p->up.catset)
    {
      errno=0;
      log->name=malloc(crp->outlen);
      if(log->name==NULL)
	error(EXIT_FAILURE, errno, "imgmode.c, %lu bytes on "
	      "imgcroponthreads", crp->outlen);
      sprintf(log->name, "%s%lu%s", cp->output, crp->outindex+1,
	      p->suffix);
      checkremovefile(log->name, cp->dontdelete);
    }
  else
    {
      /* Set the output name. */
      if(p->outnameisfile)            /* An output file was specified. */
	{
	  log->name=cp->output;
	  checkremovefile(log->name, cp->dontdelete);
	}
      else	  /* The output was a directory, use automatic output. */
	automaticoutput(p->imgs[crp->imgindex].name, p->suffix,
			cp->removedirinfo, cp->dontdelete,
			&log->name);
    }
}





/* Find the first and last pixel of a crop from its center point (in
   image mode or WCS mode). */
void
cropflpixel(struct cropparams *crp)
{
  int ncoord=1, nelem=2, status;
  struct imgcropparams *p=crp->p;
  long *naxes=p->imgs[crp->imgindex].naxes;
  double pixcrd[2], imgcrd[2], phi[1], theta[1];
  long *fpixel=crp->fpixel, *lpixel=crp->lpixel;

  if(p->imgmode)
    {
      if(p->up.catset)
	borderfromcenter(p->cat[crp->outindex*p->cs1+p->xcol],
			 p->cat[crp->outindex*p->cs1+p->ycol],
			 p->iwidth, fpixel, lpixel);
      else if(p->up.xcset)
	borderfromcenter(p->xc, p->yc, p->iwidth, fpixel, lpixel);
      else if(p->up.sectionset)
	sectionparser(p->section, naxes, fpixel, lpixel);
      else
	error(EXIT_FAILURE, 0, "A bug! In image mode, neither of the "
	      "following has been set: a catalog, a central pixel or "
	      "a section of the image. Please contact us to see how it "
	      "got to this impossible place!");
    }
  else if(p->wcsmode) /* In wcsmode, crp->world is already filled.       */
    {		      /* Note that p->iwidth was set based on p->wwidth. */
      status=0;
      if(wcss2p(p->imgs[crp->imgindex].wcs, ncoord, nelem, crp->world,
		phi, theta, imgcrd, pixcrd, &status) )
	error(EXIT_FAILURE, 0, "wcss2p error %d: %s", status,
	      wcs_errmsg[status]);
      borderfromcenter(pixcrd[0], pixcrd[1], p->iwidth, fpixel, lpixel);
      /*
      printf("\n(%f, %f): (%ld, %ld) -- (%ld, %ld)\n\n", pixcrd[0],
	     pixcrd[1], fpixel[0], fpixel[1], lpixel[0], lpixel[1]);
      */
    }
  else
    error(EXIT_FAILURE, 0, "A bug! in cropflpixel (crop.c), "
	  "neither imgmode or wcsmode are set. Please contact us so "
	  "we can see how it got to this impossible place.");
}






/* Find the size of the final FITS image (irrespective of how many
   crops will be needed for it) and make the image to keep the
   data.

   NOTE: The fpixel and lpixel in crp keep the first and last pixel of
   the total image for this crop, irrespective of the final keeping
   blank areas or not. While the fpixel_i and lpixel_i arrays keep the
   first and last pixels after the blank pixels have been removed.
*/
void
firstcropmakearray(struct cropparams *crp, long *fpixel_i,
		   long *lpixel_i, long *fpixel_c, long *lpixel_c)
{
  size_t i;
  fitsfile *ofp;
  long naxes[2];
  double crpix0, crpix1;
  int naxis=2, status=0;
  int bitpix=crp->p->bitpix;
  char *cp, *cpf, blankrec[80], titlerec[80];
  char startblank[]="                      / ";
  char *outname=crp->p->log[crp->outindex].name;
  struct inputimgs *img=&crp->p->imgs[crp->imgindex];


  /* Set the last element of the blank array. */
  cpf=blankrec+79;
  *cpf='\0';
  titlerec[79]='\0';
  cp=blankrec; do *cp=' '; while(++cp<cpf);


  /* Set the size of the output, in WCS mode, noblank==0. */
  if(crp->p->noblank && crp->p->wcsmode==0)
    {
      fpixel_c[0]=fpixel_c[1]=1;
      lpixel_c[0]=naxes[0]=lpixel_i[0]-fpixel_i[0]+1;
      lpixel_c[1]=naxes[1]=lpixel_i[1]-fpixel_i[1]+1;
    }
  else
    {
      naxes[0]=crp->lpixel[0]-crp->fpixel[0]+1;
      naxes[1]=crp->lpixel[1]-crp->fpixel[1]+1;
    }


  /* Create the FITS image extension and array and fill it with null
     values. */
  if(fits_create_file(&crp->outfits, outname, &status))
    fitsioerror(status, "Creating file.");
  ofp=crp->outfits;
  if(fits_create_img(ofp, bitpix, naxis, naxes, &status))
    fitsioerror(status, "Creating image.");
  if(bitpix==BYTE_IMG || bitpix==SHORT_IMG
     || bitpix==LONG_IMG || bitpix==LONGLONG_IMG)
    if(fits_write_key(ofp, crp->p->datatype, "BLANK",
		      crp->p->bitnul, "Pixels with no data.", &status) )
      fitsioerror(status, "Adding Blank.");
  if(fits_write_null_img(ofp, 1, naxes[0]*naxes[1], &status))
    fitsioerror(status, "Writing null array.");


  /* Write the WCS header keywords in the output FITS image, then
     update the header keywords. */
  crpix0 = img->wcs->crpix[0] - (fpixel_i[0]-1) + (fpixel_c[0]-1);
  crpix1 = img->wcs->crpix[1] - (fpixel_i[1]-1) + (fpixel_c[1]-1);
  if(fits_write_record(ofp, blankrec, &status))
    fitsioerror(status, NULL);
  sprintf(titlerec, "%sWCS information", startblank);
  for(i=strlen(titlerec);i<79;++i)
    titlerec[i]=' ';
  fits_write_record(ofp, titlerec, &status);
  for(i=0;i<img->nwcskeys-1;++i)
    fits_write_record(ofp, &img->wcstxt[i*80], &status);
  fits_update_key(ofp, TDOUBLE, "CRPIX1", &crpix0, NULL, &status);
  fits_update_key(ofp, TDOUBLE, "CRPIX2", &crpix1, NULL, &status);
  fitsioerror(status, NULL);


  /* Add the Crop information. */
  if(fits_write_record(ofp, blankrec, &status))
    fitsioerror(status, NULL);
  sprintf(titlerec, "%sCrop information", startblank);
  for(i=strlen(titlerec);i<79;++i)
    titlerec[i]=' ';
  if(fits_write_record(ofp, titlerec, &status))
    fitsioerror(status, NULL);
}





/* The starting and ending points are set in the cropparams structure
   for one crop from one image. Crop that region out.

   return values are:
   0: No crop was made (not in range of image).
   1: The input image covered at least part of the crop image.
 */
void
onecrop(struct cropparams *crp)
{
  struct imgcropparams *p=crp->p;
  struct inputimgs *img=&p->imgs[crp->imgindex];

  void *array;
  size_t cropsize;
  char basename[FLEN_KEYWORD];
  long fpixel_i[2] , lpixel_i[2];
  fitsfile *ifp=crp->infits, *ofp;
  struct fitsheaderll *headers=NULL;
  int status=0, anynul=0, bitpix=p->bitpix;
  long fpixel_o[2], lpixel_o[2], inc[2]={1,1};
  char region[FLEN_VALUE], regionkey[FLEN_KEYWORD];


  /* Find the first and last pixel of this crop box from this input
     image. */
  cropflpixel(crp);
  fpixel_i[0]=crp->fpixel[0];      fpixel_i[1]=crp->fpixel[1];
  lpixel_i[0]=crp->lpixel[0];      lpixel_i[1]=crp->lpixel[1];


  /* Find the overlap and apply it if there is any overlap. */
  if( overlap(img->naxes, fpixel_i, lpixel_i, fpixel_o, lpixel_o) )
    {
      /* Make the output FITS image and initialize it with an array of
	 NaN or BLANK values. Note that for FLOAT_IMG and DOUBLE_IMG,
	 it will automatically fill them with the NaN value.*/
      if(crp->outfits==NULL)
	firstcropmakearray(crp, fpixel_i, lpixel_i, fpixel_o, lpixel_o);
      ofp=crp->outfits;


      /* Read the desired part of the image, then write it into this
	 array. */
      cropsize=(lpixel_i[0]-fpixel_i[0]+1)*(lpixel_i[1]-fpixel_i[1]+1);
      array=bitpixalloc(cropsize, bitpix);
      status=0;
      if(fits_read_subset(ifp, p->datatype, fpixel_i, lpixel_i, inc,
			  p->bitnul, array, &anynul, &status))
	fitsioerror(status, NULL);


      /* If we have a floating point or double image, pixels with zero
	 value should actually be a NaN. Unless the user specificly
	 asks for it, make the conversion.*/
      if(p->zeroisnotblank==0
	 && (bitpix==FLOAT_IMG || bitpix==DOUBLE_IMG) )
	changezerotonan(array, cropsize, bitpix);


      /* Write the array into the image. */
      status=0;
      if( fits_write_subset(ofp, p->datatype, fpixel_o, lpixel_o,
			    array, &status) )
	fitsioerror(status, NULL);


      /* A section has been added to the cropped image from this input
	 image, so increment crp->imgcount and save the information of
	 this image. */
      sprintf(basename, "ICF%lu", ++p->log[crp->outindex].numimg);
      filenameinkeywords(basename, img->name, &headers);
      sprintf(regionkey, "%sPIX", basename);
      sprintf(region, "%ld:%ld,%ld:%ld", fpixel_i[0], lpixel_i[0]+1,
	      fpixel_i[1], lpixel_i[1]+1);
      add_to_fitsheaderllend(&headers, TSTRING, regionkey, 0, region, 0,
			     "Range of pixels used for this output.",
			     0, NULL);
      updatekeys(ofp, &headers);


      /* Free the allocated array. */
      free(array);
    }
  return;
}




















/*******************************************************************/
/******************        Check center        *********************/
/*******************************************************************/
int
iscenterfilled(struct cropparams *crp)
{
  struct imgcropparams *p=crp->p;

  void *array;
  int bitpix=p->bitpix;
  size_t size, nulcount;
  fitsfile *ofp=crp->outfits;
  int status=0, maxdim=10, anynul;
  long checkcenter=p->checkcenter;
  long naxes[2], fpixel[2], lpixel[2], inc[2]={1,1};

  uint8_t *b, *fb, *nb;
  int16_t *s, *fs, *ns;
  int32_t *l, *fl, *nl;
  int64_t *L, *fL, *nL;
  float   *f, *ff; /* isnan will check. */
  double  *d, *fd; /* isnan will check */

  /* Get the final size of the output image. */
  if( fits_get_img_size(ofp, maxdim, naxes, &status) )
    fitsioerror(status, NULL);

  /* Get the size and range of the central region to check. The +1 is
     because in FITS, counting begins from 1, not zero. */
  fpixel[0]=(naxes[0]/2+1)-checkcenter/2;
  fpixel[1]=(naxes[1]/2+1)-checkcenter/2;
  lpixel[0]=(naxes[0]/2+1)+checkcenter/2;
  lpixel[1]=(naxes[1]/2+1)+checkcenter/2;

  /* Allocate the array and read in the pixels. */
  size=checkcenter*checkcenter;
  array=bitpixalloc(size, bitpix);
  if( fits_read_subset(ofp, p->datatype, fpixel, lpixel, inc,
		       p->bitnul, array, &anynul, &status) )
    fitsioerror(status, NULL);

  /* Depending on bitpix, check the central pixels of the image. */
  nulcount=0;
  switch(bitpix)
    {
    case BYTE_IMG:
      nb=p->bitnul;
      fb=(b=array)+size;
      do if(*b==*nb) ++nulcount; while(++b<fb);
      break;

    case SHORT_IMG:
      ns=p->bitnul;
      fs=(s=array)+size;
      do if(*s==*ns) ++nulcount; while(++s<fs);
      break;

    case LONG_IMG:
      nl=p->bitnul;
      fl=(l=array)+size;
      do if(*l==*nl) ++nulcount; while(++l<fl);
      break;

    case LONGLONG_IMG:
      nL=p->bitnul;
      fL=(L=array)+size;
      do if(*L==*nL) ++nulcount; while(++L<fL);
      break;

    case FLOAT_IMG:
      ff=(f=array)+size;
      do if(isnan(*f)) ++nulcount; while(++f<ff);
      break;

    case DOUBLE_IMG:
      fd=(d=array)+size;
      do if(isnan(*d)) ++nulcount; while(++d<fd);
      break;

    default:
      error(EXIT_FAILURE, 0, "In iscenterfilled, the bitbix is not "
	    "recognized! This is not possible by the user, so it is a "
	    "a bug. Please contact us so we can correct it.");
    }
  free(array);

  if(nulcount==size)
    return 0;
  else
    return 1;
}



















/*******************************************************************/
/******************          Log file          *********************/
/*******************************************************************/
void
printlog(struct imgcropparams *p)
{
  size_t i;
  FILE *logfile;
  char msg[VERBMSGLENGTH_V];
  struct imgcroplog *log=p->log;
  size_t numfiles=0, numcentfilled=0, numstitched=0;

  /* Only for a catalog are these statistics worth it! */
  if(p->up.catset && p->cp.verb)
    for(i=0;log[i].name;++i)
      if(log[i].numimg)
	{
	  if(log[i].centerfilled || p->keepblankcenter)
	    {
	      ++numfiles;
	      if(log[i].numimg>1)
		++numstitched;
	    }
	  if(log[i].centerfilled)
	    ++numcentfilled;
	}

  /* Check to see if the file exists and remove if if it is ok. */
  checkremovefile(LOGFILENAME, p->cp.dontdelete);

  /* Make the file and print the top comments. If the file can't be
     opened for write mode, there is no problem, this is a log file,
     the user can set it on verbose mode and the same information will
     be printed. */
  errno=0;
  logfile=fopen(LOGFILENAME, "w");
  if(logfile)
    {
      /* First print the comments to the file. */
      fprintf(logfile,
	      "# "SPACK_STRING" log file.\n"
	      "# "SPACK_NAME" was run on %s#\n",
	      ctime(&p->rawtime));
      if(p->keepblankcenter==0)
	fprintf(logfile, "# NOTE: by default images with a blank "
		"center are deleted.\n# To keep such images, run again "
		"with `--keepblankcenter`.\n#\n");
      fprintf(logfile,
	      "# Column numbers below start from zero.\n"
	      "# 0: Output file name.\n"
	      "# 1: Number of images used in this cropped image.\n"
	      "# 2: Are the central %lu pixels filled? (1: yes, 0: no)\n",
	      p->checkcenter);

      /* Then print each output's information. */
      for(i=0;log[i].name;++i)
	fprintf(logfile, "%s     %-8lu%-2d\n", log[i].name,
		log[i].numimg, log[i].centerfilled);

      /* Report Summary: */
      if(p->cp.verb && p->up.catset)
	{
	  sprintf(msg, "%lu images created.", numfiles);
	  reporttiming(NULL, msg, 1);
	  sprintf(msg, "%lu were filled in the center.",
		  numcentfilled);
	  reporttiming(NULL, msg, 1);
	  if(numstitched)
	    {
	      sprintf(msg, "%lu were stiched from more than one image.",
		      numstitched);
	      reporttiming(NULL, msg, 1);
	    }
	}

      /* Close the file. */
      errno=0;
      if(fclose(logfile))
	error(EXIT_FAILURE, errno, LOGFILENAME" could not be closed");
    }
}