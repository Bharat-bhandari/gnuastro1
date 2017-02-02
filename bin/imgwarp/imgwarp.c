/*********************************************************************
ImageWarp - Warp images using projective mapping.
ImageWarp is part of GNU Astronomy Utilities (Gnuastro) package.

Original author:
     Mohammad Akhlaghi <akhlaghi@gnu.org>
Contributing author(s):
Copyright (C) 2015, Free Software Foundation, Inc.

Gnuastro is free software: you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation, either version 3 of the License, or (at your
option) any later version.

Gnuastro is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
General Public License for more details.

You should have received a copy of the GNU General Public License
along with Gnuastro. If not, see <http://www.gnu.org/licenses/>.
**********************************************************************/
#include <config.h>

#include <errno.h>
#include <error.h>
#include <stdio.h>
#include <float.h>
#include <stdlib.h>

#include <gnuastro/wcs.h>
#include <gnuastro/fits.h>
#include <gnuastro/polygon.h>

#include "main.h"
#include "imgwarp.h"







/***************************************************************/
/**************            MACROS             ******************/
/***************************************************************/
/* Multiply a 2 element vector with a transformation matrix and put
   the result in the 2 element output array. It is assumed that the
   input is from a flat coordinate systemy. */
#define mappoint(V, T, O)                                       \
  {                                                             \
    (O)[0]=( ( (T)[0]*(V)[0] + (T)[1]*(V)[1] + (T)[2] )         \
             / ( (T)[6]*(V)[0] + (T)[7]*(V)[1] + (T)[8] ) );    \
    (O)[1]=( ( (T)[3]*(V)[0] + (T)[4]*(V)[1] + (T)[5] )         \
             / ( (T)[6]*(V)[0] + (T)[7]*(V)[1] + (T)[8] ) );    \
  }                                                             \





/* A pixel's center is an integer value. This function will give the
   integer value that is nearest to a floating point number. Works for
   both positive and negative values and includes floating point
   errors.

   nearestint_halfhigher(0.5f) --> 1.0f
*/
#define nearestint_halfhigher(D)                                        \
  (ceil((D)) - (D) > 0.5f                                               \
   + GAL_POLYGON_ROUND_ERR ? ceil((D))-1.0f : ceil((D)))





/* Similar to `nearestint_halflower' but:

   nearestint_halflower(0.5f) --> 0.0f;
 */
#define nearestint_halflower(D)                                        \
  (ceil((D)) - (D) > 0.5f - GAL_POLYGON_ROUND_ERR ? ceil((D))-1.0f : ceil((D)))





#define ceilwitherr(D)                                       \
  ( fabs( nearbyint((D)) - (D) ) < GAL_POLYGON_ROUND_ERR     \
    ? nearbyint((D)) : nearbyint((D))+1  )




















/***************************************************************/
/**************      Processing function      ******************/
/***************************************************************/
void *
imgwarponthread(void *inparam)
{
  struct iwpparams *iwp=(struct iwpparams*)inparam;
  struct imgwarpparams *p=iwp->p;

  long is0=p->is0, is1=p->is1;
  double area, nanarea, *input=p->input, v=NAN;
  size_t *extinds=p->extinds, *ordinds=p->ordinds;
  size_t i, j, ind, os1=p->onaxes[0], numcrn, numinput;
  double ocrn[8], icrn_base[8], icrn[8], *output=p->output;
  long x, y, xstart, xend, ystart, yend; /* Might be negative */
  double pcrn[8], *outfpixval=p->outfpixval, ccrn[GAL_POLYGON_MAX_CORNERS];

  for(i=0;(ind=iwp->indexs[i])!=GAL_THREADS_NON_THRD_INDEX;++i)
    {
      /* Initialize the output pixel value: */
      numinput=0;
      output[ind]=nanarea=0.0f;

      /* Set the corners of this output pixel. The ind/os1 and ind%os1
         start from 0. Note that the outfpixval already contains the
         correction for the fact that the FITS standard considers the
         center of first pixel to be at (1.0f, 1.0f).*/
      ocrn[0]=(double)(ind%os1)-0.5f+outfpixval[0];
      ocrn[1]=(double)(ind/os1)-0.5f+outfpixval[1];
      ocrn[2]=(double)(ind%os1)+0.5f+outfpixval[0];
      ocrn[3]=(double)(ind/os1)-0.5f+outfpixval[1];
      ocrn[4]=(double)(ind%os1)-0.5f+outfpixval[0];
      ocrn[5]=(double)(ind/os1)+0.5f+outfpixval[1];
      ocrn[6]=(double)(ind%os1)+0.5f+outfpixval[0];
      ocrn[7]=(double)(ind/os1)+0.5f+outfpixval[1];


      /* Transform the four corners of the output pixel to the input
         image coordinates. */
      for(j=0;j<4;++j) mappoint(&ocrn[j*2], p->inverse, &icrn_base[j*2]);

      /* Using the known relationships between the vertice locations,
         put everything in the right place: */
      xstart = nearestint_halfhigher( icrn_base[extinds[0]] );
      xend   = nearestint_halflower(  icrn_base[extinds[1]] ) + 1;
      ystart = nearestint_halfhigher( icrn_base[extinds[2]] );
      yend   = nearestint_halflower(  icrn_base[extinds[3]] ) + 1;
      icrn[0]=icrn_base[ordinds[0]*2]; icrn[1]=icrn_base[ordinds[0]*2+1];
      icrn[2]=icrn_base[ordinds[1]*2]; icrn[3]=icrn_base[ordinds[1]*2+1];
      icrn[4]=icrn_base[ordinds[2]*2]; icrn[5]=icrn_base[ordinds[2]*2+1];
      icrn[6]=icrn_base[ordinds[3]*2]; icrn[7]=icrn_base[ordinds[3]*2+1];

      /* For a check:
      if(ind==9999)
        {
          printf("\n\n\nind: %zu: (%zu, %zu):\n",
                 ind, ind%os1+1, ind/os1+1);
          for(j=0;j<4;++j)
            printf("(%.3f, %.3f) --> (%.3f, %.3f)\n",
                   ocrn[j*2], ocrn[j*2+1], icrn_base[j*2],
                   icrn_base[j*2+1]);
          printf("------- Ordered -------\n");
          for(j=0;j<4;++j) printf("(%.3f, %.3f)\n", icrn[j*2], icrn[j*2+1]);
          printf("------- Start and ending pixels -------\n");
          printf("X: %ld -- %ld\n", xstart, xend);
          printf("Y: %ld -- %ld\n", ystart, yend);
        }
      */

      /* Go over all the input pixels that are covered. Note that x
         and y are the centers of the pixel. */
      for(y=ystart;y<yend;++y)
        {
          /* If the pixel isn't in the image (note that the pixel
             coordinates start from 1), contine to next. Note that the
             pixel polygon should be counter clockwise. */
          if( y<1 || y>is0 ) continue;
          pcrn[1]=y-0.5f;      pcrn[3]=y-0.5f;
          pcrn[5]=y+0.5f;      pcrn[7]=y+0.5f;
          for(x=xstart;x<xend;++x)
            {
              if( x<1 || x>is1 ) continue;

              /* Read the value of the input pixel. */
              v=input[(y-1)*is1+x-1];

              pcrn[0]=x-0.5f;          pcrn[2]=x+0.5f;
              pcrn[4]=x+0.5f;          pcrn[6]=x-0.5f;

              /* Find the overlapping (clipped) polygon: */
              gal_polygon_clip(icrn, 4, pcrn, 4, ccrn, &numcrn);
              area=gal_polygon_area(ccrn, numcrn);

              /* Add the fractional value of this pixel. If this
                 output pixel covers a NaN pixel in the input grid,
                 then calculate the area of this NaN pixel to account
                 for it later. */
              if( isnan(v) )
                nanarea+=area;
              else
                {
                  ++numinput;
                  output[ind]+=v*area;
                }

              /* For a polygon check:
              if(ind==9999)
                {
                  printf("%zu -- (%zd, %zd):\n", ind, x, y);
                  printf("icrn:\n");
                  for(j=0;j<4;++j)
                    printf("\t%.3f, %.3f\n", icrn[j*2], icrn[j*2+1]);
                  printf("pcrn:\n");
                  for(j=0;j<4;++j)
                    printf("\t%.3f, %.3f\n", pcrn[j*2], pcrn[j*2+1]);
                  printf("ccrn:\n");
                  for(j=0;j<numcrn;++j)
                    printf("\t%.3f, %.3f\n", ccrn[j*2], ccrn[j*2+1]);
                  printf("[%zu]: %.3f of [%ld, %ld]: %f\n", ind,
                         gal_polygon_area(ccrn, numcrn), x, y,
                         input[(y-1)*is1+x-1]);
                }
              */

              /* For a simple pixel value check:
              if(ind==97387)
                printf("%f --> (%zu) %f\n",
                       v*gal_polygon_area(ccrn, numcrn),
                       numinput, output[ind]);
              */
            }
        }

      /* Correct for the area covered by a NaN. The basic idea is
         this: The full pixel (with area `A') would have a value of
         `F'. But we have only measured the value `f' over the area
         `a', so a simple linear extrapolation would give: F=fA/a. */
      if(numinput && nanarea!=0.0f)
        {
          /* For a check:
          printf("%zu: %f/%f --> %f\n", ind, nanarea, p->opixarea,
                 nanarea/p->opixarea);
          */

          if(nanarea/p->opixarea<p->maxblankfrac)
            output[ind]*=p->opixarea/(p->opixarea-nanarea);
          else
            numinput=0;
        }

      /* Write the final value to disk: */
      if(numinput==0 && p->zerofornoinput==0)
        {
          output[ind]=NAN;
          ++p->numnul;
        }
    }



  /* Wait until all other threads finish. */
  if(p->cp.numthreads>1)
    pthread_barrier_wait(iwp->b);

  return NULL;
}


















/***************************************************************/
/**************          Preparations         ******************/
/***************************************************************/
/* Do all the preparations.

   Make the output array. We transform the four corners of the image
   into the output space. To find the four sides of the image.

   About fpixel and lpixel. The point is that we don't want to spend
   time, transforming any pixels which we know will not be in the
   input image.

   Find the proper order of transformed pixel corners from the output
   array to the input array. The order is fixed for all the pixels in
   the image altough the scale might change.
*/
void
imgwarppreparations(struct imgwarpparams *p)
{
  double output[8], forarea[8];
  double icrn[8]={0,0,0,0,0,0,0,0};
  size_t i, *extinds=p->extinds, size;
  double xmin=DBL_MAX, xmax=-DBL_MAX, ymin=DBL_MAX, ymax=-DBL_MAX;
  double ocrn[8]={0.5f,0.5f,  1.5f,0.5f, 0.5f,1.5f,   1.5f, 1.5f};
  double input[8]={0.5f,0.5f,             p->is1+0.5f, 0.5f,
                   0.5f, p->is0+0.5f,     p->is1+0.5f, p->is0+0.5f};

  /* Find the range of pixels of the input image. All the input
     positions are moved to the negative by half a pixel since the
     center of the pixel is an integer value.*/
  for(i=0;i<4;++i)
    {
      mappoint(&input[i*2], p->matrix, &output[i*2]);
      if(output[i*2]<xmin)     xmin = output[i*2];
      if(output[i*2]>xmax)     xmax = output[i*2];
      if(output[i*2+1]<ymin)   ymin = output[i*2+1];
      if(output[i*2+1]>ymax)   ymax = output[i*2+1];
    }
  /* For a check:
  for(i=0;i<4;++i)
      printf("(%.3f, %.3f) --> (%.3f, %.3f)\n",
             input[i*2], input[i*2+1],
             output[i*2], output[i*2+1]);
  printf("xmin: %.3f\nxmax: %.3f\nymin: %.3f\nymax: %.3f\n",
         xmin, xmax, ymin, ymax);
  */

  /* Set the final size of the image. The X axis is horizontal. The
     reason we are using the halflower variation of `nearestint' for
     the maximums is that these points are the farthest extremes of
     the input image. If they are half a pixel value, they should
     point to the pixel before. */
  p->onaxes[0]=nearestint_halflower(xmax)-nearestint_halfhigher(xmin)+1;
  p->onaxes[1]=nearestint_halflower(ymax)-nearestint_halfhigher(ymin)+1;
  p->outfpixval[0]=nearestint_halfhigher(xmin);
  p->outfpixval[1]=nearestint_halfhigher(ymin);
  /* For a check:
  printf("Wrapped:\n");
  printf("onaxes: (%zu, %zu)\n", p->onaxes[0], p->onaxes[1]);
  printf("outfpixval=(%.4f, %.4f)\n", p->outfpixval[0], p->outfpixval[1]);
  */

  /* We now know the size of the output and the starting and ending
     coordinates in the output image (bottom left corners of pixels)
     for the transformation. */
  errno=0;
  size=p->onaxes[0]*p->onaxes[1];
  p->output=malloc(size*sizeof *p->output);
  if(p->output==NULL)
    error(EXIT_FAILURE, errno, "%zu bytes for the output array",
          size*sizeof *p->output);


  /* Order the corners of the inverse-transformed pixel (from the
     output to the input) in an anti-clockwise transformation. In a
     general homographic transform, the scales of the output pixels
     may change, but the relative positions of the corners will
     not.  */
  for(i=0;i<4;++i)
    {
      ocrn[i*2]   += p->outfpixval[0];
      ocrn[i*2+1] += p->outfpixval[1];
      mappoint(&ocrn[i*2], p->inverse, &icrn[i*2]);
    }


  /* Order the transformed output pixel. */
  gal_polygon_ordered_corners(icrn, 4, p->ordinds);


  /* Find the area of the output pixel in units of the input pixel,
     this is necessary if we are going to have NAN pixels in the image
     to account for the area lost due to a NAN value. */
  for(i=0;i<4;++i)
    {
      forarea[2*i]=icrn[2*p->ordinds[i]];
      forarea[2*i+1]=icrn[2*p->ordinds[i]+1];
    }
  p->opixarea=gal_polygon_area(forarea, 4);




  /* Find which index after transformation will have the minimum and
     maximum positions along the two axises. We can't use the starting
     loop because that is based on the input image which can be
     not-a-square! So we do it here where pixels are squares. */
  xmin=DBL_MAX; xmax=-DBL_MAX; ymin=DBL_MAX; ymax=-DBL_MAX;
  for(i=0;i<4;++i)
    {
      if(icrn[i*2]<xmin)   { xmin=icrn[i*2];   extinds[0]=i*2;   }
      if(icrn[i*2]>xmax)   { xmax=icrn[i*2];   extinds[1]=i*2;   }
      if(icrn[i*2+1]<ymin) { ymin=icrn[i*2+1]; extinds[2]=i*2+1; }
      if(icrn[i*2+1]>ymax) { ymax=icrn[i*2+1]; extinds[3]=i*2+1; }
    }

  /* For a check:
  for(i=0;i<4;++i)
    printf("(%.3f, %.3f) --> (%.3f, %.3f)\n",
           ocrn[i*2], ocrn[i*2+1], icrn[i*2], icrn[i*2+1]);
  printf("xmin: %.3f\nxmax: %.3f\nymin: %.3f\nymax: %.3f\n",
         xmin, xmax, ymin, ymax);
  */
}





/* Correct the WCS coordinates (Multiply the 2x2 PC matrix of the WCS
   structure by the INVERSE of the transform in 2x2 form that has been
   converted from homogeneous coordinates). Then Multiply the crpix
   array with the ACTUAL transformation matrix. */
void
correctwcssaveoutput(struct imgwarpparams *p)
{
  size_t i;
  void *array;
  double *pixelscale;
  double *m=p->matrix, diff;
  char keyword[9*FLEN_KEYWORD];
  struct gal_fits_key_ll *headers=NULL;
  double tpc[4], tcrpix[3], *crpix=p->wcs->crpix, *pc=p->wcs->pc;
  double tinv[4]={p->inverse[0]/p->inverse[8], p->inverse[1]/p->inverse[8],
                  p->inverse[3]/p->inverse[8], p->inverse[4]/p->inverse[8]};

  if(p->correctwcs && p->wcs)
    {
      /* Correct the PC matrix: */
      tpc[0]=pc[0]*tinv[0]+pc[1]*tinv[2];
      tpc[1]=pc[0]*tinv[1]+pc[1]*tinv[3];
      tpc[2]=pc[2]*tinv[0]+pc[3]*tinv[2];
      tpc[3]=pc[2]*tinv[1]+pc[3]*tinv[3];
      pc[0]=tpc[0]; pc[1]=tpc[1]; pc[2]=tpc[2]; pc[3]=tpc[3];

      /* Correct the CRPIX point. The +1 in the end of the last two
         lines is because FITS counts from 1. */
      tcrpix[0]=m[0]*crpix[0]+m[1]*crpix[1]+m[2];
      tcrpix[1]=m[3]*crpix[0]+m[4]*crpix[1]+m[5];
      tcrpix[2]=m[6]*crpix[0]+m[7]*crpix[1]+m[8];
      crpix[0]=tcrpix[0]/tcrpix[2]-p->outfpixval[0]+1;
      crpix[1]=tcrpix[1]/tcrpix[2]-p->outfpixval[1]+1;
    }


  /* Convert the output to the input image format: */
  if(p->inputbitpix==DOUBLE_IMG || p->doubletype)
    {
      array=p->output;
      p->inputbitpix=DOUBLE_IMG; /* Not converted and p->doubletype==1 */
    }
  else
    gal_fits_change_type((void **)p->output, DOUBLE_IMG,
                              p->onaxes[1]*p->onaxes[0],
                              p->numnul, &array, p->inputbitpix);

  /* Add the appropriate headers: */
  gal_fits_file_name_in_keywords("INF", p->up.inputname, &headers);
  for(i=0;i<9;++i)
    {
      sprintf(&keyword[i*FLEN_KEYWORD], "WMTX%zu_%zu", i/3+1, i%3+1);
      gal_fits_add_to_key_ll_end(&headers, TDOUBLE,
                                 &keyword[i*FLEN_KEYWORD], 0,
                                 &p->matrix[i], 0, "Warp matrix "
                                 "element value.", 0, NULL);
    }

  /* Due to floating point errors extremely small values of PC matrix can
     be set to zero and extremely small differences between PC1_1 and PC2_2
     can be ignored. The reason for all the `fabs' functions is because the
     signs are usually different.*/
  if( p->wcs->pc[1]<ABSOLUTEFLTERROR ) p->wcs->pc[1]=0.0f;
  if( p->wcs->pc[2]<ABSOLUTEFLTERROR ) p->wcs->pc[2]=0.0f;
  pixelscale=gal_wcs_pixel_scale_deg(p->wcs);
  diff=fabs(p->wcs->pc[0])-fabs(p->wcs->pc[3]);
  if( fabs(diff/pixelscale[0])<RELATIVEFLTERROR )
    p->wcs->pc[3] =  ( (p->wcs->pc[3] < 0.0f ? -1.0f : 1.0f)
                       * fabs(p->wcs->pc[0]) );

  /* Save the output: */
  gal_fits_array_to_file(p->cp.output, "Warped", p->inputbitpix, array,
                         p->onaxes[1], p->onaxes[0], p->numnul, p->wcs,
                         headers, SPACK_STRING);

  /* Clean up. */
  free(pixelscale);
  if(array!=p->output)
    free(array);
}




















/***************************************************************/
/**************       Outside function        ******************/
/***************************************************************/
void
imgwarp(struct imgwarpparams *p)
{
  int err;
  pthread_t t;          /* All thread ids saved in this, not used. */
  pthread_attr_t attr;
  pthread_barrier_t b;
  struct iwpparams *iwp;
  size_t i, nb, *indexs, thrdcols;
  size_t nt=p->cp.numthreads, size;

  /* Set the number of output blank pixels to zero: */
  p->numnul=0;

  /* Array keeping thread parameters for each thread. */
  errno=0;
  iwp=malloc(nt*sizeof *iwp);
  if(iwp==NULL)
    error(EXIT_FAILURE, errno, "%zu bytes in imgwarp "
          "(imgwarp.c) for iwp", nt*sizeof *iwp);


  /* Prepare the output array and all the necessary things: */
  imgwarppreparations(p);


  /* Distribute the output pixels into the threads: */
  size=p->onaxes[0]*p->onaxes[1];
  gal_threads_dist_in_threads(size, nt, &indexs, &thrdcols);


  /* Start the convolution. */
  if(nt==1)
    {
      iwp[0].p=p;
      iwp[0].indexs=indexs;
      imgwarponthread(&iwp[0]);
    }
  else
    {
      /* Initialize the attributes. Note that this running thread
         (that spinns off the nt threads) is also a thread, so the
         number the barrier should be one more than the number of
         threads spinned off. */
      if(size<nt) nb=size+1;
      else nb=nt+1;
      gal_threads_attr_barrier_init(&attr, &b, nb);

      /* Spin off the threads: */
      for(i=0;i<nt;++i)
        if(indexs[i*thrdcols]!=GAL_THREADS_NON_THRD_INDEX)
          {
            iwp[i].p=p;
            iwp[i].b=&b;
            iwp[i].indexs=&indexs[i*thrdcols];
            err=pthread_create(&t, &attr, imgwarponthread, &iwp[i]);
            if(err)
              error(EXIT_FAILURE, 0, "can't create thread %zu", i);
          }

      /* Wait for all threads to finish and free the spaces. */
      pthread_barrier_wait(&b);
      pthread_attr_destroy(&attr);
      pthread_barrier_destroy(&b);
    }

  /* Correct the WCS information and save the output. */
  correctwcssaveoutput(p);

  /* Free the allocated spaces: */
  free(iwp);
  free(indexs);
  free(p->output);
}
