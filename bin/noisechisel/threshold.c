/*********************************************************************
NoiseChisel - Detect and segment signal in a noisy dataset.
NoiseChisel is part of GNU Astronomy Utilities (Gnuastro) package.

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

#include <stdio.h>
#include <errno.h>
#include <error.h>
#include <string.h>
#include <stdlib.h>

#include <gnuastro/fits.h>
#include <gnuastro/blank.h>
#include <gnuastro/threads.h>
#include <gnuastro/statistics.h>
#include <gnuastro/interpolate.h>

#include <timing.h>

#include "main.h"

#include "ui.h"
#include "threshold.h"




/****************************************************************
 ************           Quantile threshold           ************
 ****************************************************************/
struct qthreshparams
{
  gal_data_t        *erode_th;
  gal_data_t      *noerode_th;
  void                 *usage;
  struct noisechiselparams *p;
};





static void *
qthresh_on_tile(void *in_prm)
{
  struct gal_threads_params *tprm=(struct gal_threads_params *)in_prm;
  struct qthreshparams *qprm=(struct qthreshparams *)tprm->params;
  struct noisechiselparams *p=qprm->p;

  double *darr;
  void *tarray=NULL;
  int type=qprm->erode_th->type;
  gal_data_t *tile, *mode, *qvalue, *usage, *tblock=NULL;
  size_t i, tind, twidth=gal_type_sizeof(type), ndim=p->input->ndim;

  /* Put the temporary usage space for this thread into a data set for easy
     processing. */
  usage=gal_data_alloc(gal_data_ptr_increment(qprm->usage,
                                              tprm->id*p->maxtcontig, type),
                       type, ndim, p->maxtsize, NULL, 0, p->cp.minmapsize,
                       NULL, NULL, NULL);

  /* Go over all the tiles given to this thread. */
  for(i=0; tprm->indexs[i] != GAL_THREADS_NON_THRD_INDEX; ++i)
    {
      /* Re-initialize the usage array's space (will be changed in
         `gal_data_copy_to_allocated' for each tile). */
      usage->ndim=ndim;
      usage->size=p->maxtcontig;
      memcpy(usage->dsize, p->maxtsize, ndim*sizeof *p->maxtsize);


      /* Copy the tile's contents into the pre-allocated space. Note that
         we have to initialize the `dsize' and `size' elements of
         `contents', since they can be changed and the size is
         important. Recall that this is a 1D array. */
      tind = tprm->indexs[i];
      tile = &p->cp.tl.tiles[tind];


      /* If we have a convolved image, temporarily change the tile's
         pointers so we can do the work on the convolved image. */
      if(p->conv)
        {
          tarray=tile->array; tblock=tile->block;
          tile->array=gal_tile_block_relative_to_other(tile, p->conv);
          tile->block=p->conv;
        }
      gal_data_copy_to_allocated(tile, usage);
      if(p->conv) { tile->array=tarray; tile->block=tblock; }


      /* Find the mode on this dataset, note that we have set the `inplace'
         flag to `1'. So as a byproduct of finding the mode, `usage' is not
         going to have any blank elements and will be sorted (thus ready to
         be used by the quantile functions). */
      mode=gal_statistics_mode(usage, p->mirrordist, 1);


      /* Check the mode value. Note that if the mode is not accurate, then
         the contents of `darr' will be NaN and all conditions will
         fail. In such cases, the tile will be ignored. */
      darr=mode->array;
      if( fabs(darr[1]-0.5f) < p->modmedqdiff )
        {
          /* Get the erosion quantile for this tile and save it. Note that
             the type of `qvalue' is the same as the input dataset. */
          qvalue=gal_statistics_quantile(usage, p->qthresh, 1);
          memcpy(gal_data_ptr_increment(qprm->erode_th->array, tind, type),
                 qvalue->array, twidth);
          gal_data_free(qvalue);

          /* Do the same for the no-erode quantile. */
          qvalue=gal_statistics_quantile(tile, p->noerodequant, 1);
          memcpy(gal_data_ptr_increment(qprm->noerode_th->array, tind, type),
                 qvalue->array, twidth);
          gal_data_free(qvalue);
        }
      else
        {
          gal_blank_write(gal_data_ptr_increment(qprm->erode_th->array,
                                                 tind, type), type);
          gal_blank_write(gal_data_ptr_increment(qprm->noerode_th->array,
                                                 tind, type), type);
        }

      /* Clean up and fix the tile's pointers. */
      gal_data_free(mode);
    }

  /* Clean up and wait for the other threads to finish, then return. */
  usage->array=NULL;  /* Not allocated here. */
  gal_data_free(usage);
  if(tprm->b) pthread_barrier_wait(tprm->b);
  return NULL;
}





static void
apply_quantile_threshold(struct qthreshparams *qprm)
{
  size_t tid;
  void *tarray=NULL;
  gal_data_t *tile, *tblock=NULL;
  struct noisechiselparams *p=qprm->p;
  float *erode_th=qprm->erode_th->array, *noerode_th=qprm->noerode_th->array;

  /* A small sanity check. */
  if(qprm->erode_th->type!=GAL_TYPE_FLOAT32)
    error(EXIT_FAILURE, 0, "`apply_quantile_threshold' currently only "
          "supports float arrays.");

  /* Clear the binary array (this is mainly because the input may contain
     blank values and we won't be doing the thresholding no those
     pixels. */
  memset(p->binary->array, 0, p->binary->size);

  /* Go over all the tiles. */
  for(tid=0; tid<p->cp.tl.tottiles;++tid)
    {
      /* For easy reading. */
      tile=&p->cp.tl.tiles[tid];

      /* Correct the tile's pointers to apply the threshold on the
         convolved image. */
      if(p->conv)
        {
          tarray=tile->array; tblock=tile->block;
          tile->array=gal_tile_block_relative_to_other(tile, p->conv);
          tile->block=p->conv;
        }

      /* Apply the threshold. */
      GAL_TILE_PARSE_OPERATE({
          *o = ( *i > erode_th[tid]
                 ? ( *i > noerode_th[tid] ? THRESHOLD_NO_ERODE_VALUE : 1 )
                 : 0 );
        }, tile, p->binary, 1, 1);

      /* Revert the tile's pointers back to what they were. */
      if(p->conv) { tile->array=tarray; tile->block=tblock; }
    }
}





void
threshold_quantile_find_apply(struct noisechiselparams *p)
{
  char *msg;
  gal_data_t *tmp;
  struct timeval t1;
  struct qthreshparams qprm;
  struct gal_options_common_params *cp=&p->cp;
  struct gal_tile_two_layer_params *tl=&cp->tl;


  /* Get the starting time if necessary. */
  if(!p->cp.quiet) gettimeofday(&t1, NULL);


  /* Add image to check image if requested. If the user has asked for
     `oneelempertile', then the size of values is not going to be the same
     as the input, making it hard to inspect visually. So we'll only put
     the full input when `oneelempertile' isn't requested. */
  if(p->qthreshname && !tl->oneelempertile)
    gal_fits_img_write(p->conv ? p->conv : p->input, p->qthreshname, NULL,
                       PROGRAM_STRING);


  /* Allocate space for the quantile threshold values. */
  qprm.erode_th=gal_data_alloc(NULL, p->input->type, p->input->ndim,
                               tl->numtiles, NULL, 0, cp->minmapsize,
                               "QTHRESH-ERODE", p->input->unit, NULL);
  qprm.noerode_th=gal_data_alloc(NULL, p->input->type, p->input->ndim,
                                 tl->numtiles, NULL, 0, cp->minmapsize,
                                 "QTHRESH-NOERODE", p->input->unit, NULL);


  /* Allocate temporary space for processing in each tile. */
  qprm.usage=gal_data_malloc_array(p->input->type,
                                   cp->numthreads * p->maxtcontig);

  /* Find the threshold on each tile, then clean up the temporary space. */
  qprm.p=p;
  gal_threads_spin_off(qthresh_on_tile, &qprm, tl->tottiles, cp->numthreads);
  if(p->qthreshname)
    {
      gal_tile_full_values_write(qprm.erode_th, tl, p->qthreshname,
                                 PROGRAM_STRING);
      gal_tile_full_values_write(qprm.noerode_th, tl, p->qthreshname,
                                 PROGRAM_STRING);
    }
  free(qprm.usage);


  /* Interpolate over the blank tiles. */
  qprm.erode_th->next = qprm.noerode_th;
  tmp=gal_interpolate_close_neighbors(qprm.erode_th, tl, cp->interpnumngb,
                                      cp->numthreads, cp->interponlyblank, 1);
  gal_data_free(qprm.erode_th);
  gal_data_free(qprm.noerode_th);
  qprm.erode_th=tmp;
  qprm.noerode_th=tmp->next;
  qprm.erode_th->next=qprm.noerode_th->next=NULL;
  if(p->qthreshname)
    {
      gal_tile_full_values_write(qprm.erode_th, tl, p->qthreshname,
                                 PROGRAM_STRING);
      gal_tile_full_values_write(qprm.noerode_th, tl, p->qthreshname,
                                 PROGRAM_STRING);
    }


  /* Smooth the threshold if requested. */
  if(p->smoothwidth>1)
    {
      /* Do the smoothing on the erosion quantile. */
      if(!cp->quiet) gettimeofday(&t1, NULL);
      tmp=gal_tile_full_values_smooth(qprm.erode_th, tl, p->smoothwidth,
                                      p->cp.numthreads);
      gal_data_free(qprm.erode_th);
      qprm.erode_th=tmp;

      /* Same for the no-erosion quantile. */
      tmp=gal_tile_full_values_smooth(qprm.noerode_th, tl, p->smoothwidth,
                                      p->cp.numthreads);
      gal_data_free(qprm.noerode_th);
      qprm.noerode_th=tmp;

      /* Add them to the check image. */
      if(p->qthreshname)
        {
          gal_tile_full_values_write(qprm.erode_th, tl, p->qthreshname,
                                     PROGRAM_STRING);
          gal_tile_full_values_write(qprm.noerode_th, tl, p->qthreshname,
                                     PROGRAM_STRING);
        }
    }


  /* The quantile threshold is found, now apply it. */
  apply_quantile_threshold(&qprm);


  /* Write the binary image if check is requested. */
  if(p->qthreshname && !tl->oneelempertile)
    gal_fits_img_write(p->binary, p->qthreshname, NULL, PROGRAM_STRING);


  /* Clean up and report duration if necessary. */
  gal_data_free(qprm.erode_th);
  gal_data_free(qprm.noerode_th);
  if(!p->cp.quiet)
    {
      asprintf(&msg, "%.2f quantile threshold found and applied.",
               p->qthresh);
      gal_timing_report(&t1, msg, 2);
    }


  /* If the user wanted to check the threshold and hasn't called
     `continueaftercheck', then stop NoiseChisel. */
  if(p->qthreshname && !p->continueaftercheck)
    ui_abort_after_check(p, p->qthreshname, "quantile threshold check");
}