#include <config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>

#include <direct/debug.h>
#include <direct/interface.h>
#include <direct/mem.h>
#include <direct/messages.h>

#include <directfb.h>

#include <core/layers.h>
#include <core/surface.h>
#include <core/surface_buffer.h>
#include <core/system.h>

#include <display/idirectfbsurface.h>
#include <media/idirectfbimageprovider.h>

#include "sh7722.h"

D_DEBUG_DOMAIN( SH7722_JPEG, "SH7722/JPEG", "SH7722 JPEG Processing Unit" );

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx );

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           ... );

#include <direct/interface_implementation.h>

DIRECT_INTERFACE_IMPLEMENTATION( IDirectFBImageProvider, SH7722_JPEG )

/*
 * private data struct of IDirectFBImageProvider_SH7722_JPEG
 */
typedef struct {
     int                  ref;      /* reference counter */

     int                  width;
     int                  height;

     CoreDFB             *core;

     IDirectFBDataBuffer *buffer;

     DIRenderCallback     render_callback;
     void                *render_callback_context;
} IDirectFBImageProvider_SH7722_JPEG_data;

/**********************************************************************************************************************/

static DFBResult
DecodeHW( IDirectFBImageProvider_SH7722_JPEG_data *data,
          CoreSurface                             *destination,
          const DFBRectangle                      *rect,
          const DFBRegion                         *clip )
{
     DFBResult              ret;
     CoreSurfaceBufferLock  lock;
     unsigned long          phys;
     unsigned int           len;
     int                    loaded = 2;
     SH7722DriverData      *drv    = dfb_gfxcard_get_driver_data();
     SH7722DeviceData      *dev    = drv->dev;
     SH7722GfxSharedArea   *shared = drv->gfx_shared;

     D_ASSERT( data != NULL );
     D_MAGIC_ASSERT( destination, CoreSurface );
     DFB_RECTANGLE_ASSERT( rect );
     DFB_REGION_ASSERT( clip );

     /*
      * FIRST WORKING VERSION [tm]
      *
      * Almost user space based only. To NOT USE usleep() & friends and loose throughput (sleep) or
      * CPU resources (busyloop), I added an ioctl that waits for the next interrupt to occur.
      *
      * With a 128x128 image (23k coded data) being loaded in a loop, this temporary mechanism is
      * achieving almost the same throughput as a busyloop, but with only half the CPU load.
      *
      * TODO
      * - prefetch image info with a state machine used by Construct(), GetSurfaceDescription() and RenderTo().
      * - utilize both reload buffers properly (pipelining)
      * - implement clipping, scaling and format conversion via VEU (needs line buffer mode)
      * - add locking and/or move more code into the kernel module (multiple contexts with queueing?)
      */


     D_DEBUG_AT( SH7722_JPEG, "  -> loading...\n" );

     /* Prefill both reload buffers. */
     if (data->buffer->GetData( data->buffer, SH7722GFX_JPEG_RELOAD_SIZE * 2, (void*) drv->jpeg_virt, &len ))
          return DFB_IO;

     /* Lock destination surface. */
     ret = dfb_surface_lock_buffer( destination, CSBR_BACK, CSAF_GPU_WRITE, &lock );
     if (ret)
          return ret;

     /* Calculate destination base address. */
     phys = lock.phys + rect->x + rect->y * lock.pitch;

     D_DEBUG_AT( SH7722_JPEG, "  -> setting...\n" );

     /* Program JPU from RESET. */
     SH7722_SETREG32( drv, JCCMD,    JCCMD_RESET );
     SH7722_SETREG32( drv, JCMOD,    JCMOD_INPUT_CTRL | JCMOD_DSP_DECODE );
     SH7722_SETREG32( drv, JINTE,    JINTS_INS3_HEADER | JINTS_INS5_ERROR |
                                     JINTS_INS6_DONE   |
                                     JINTS_INS10_ | JINTS_INS11_ | JINTS_INS12_ |
                                     JINTS_INS14_RELOAD );

     SH7722_SETREG32( drv, JIFCNT,   JIFCNT_VJSEL_JPU );
     SH7722_SETREG32( drv, JIFECNT,  JIFECNT_SWAP_4321 );
     SH7722_SETREG32( drv, JIFDCNT,  JIFDCNT_RELOAD_ENABLE | JIFDCNT_SWAP_4321 );
     SH7722_SETREG32( drv, JIFDSA1,  dev->jpeg_phys );
     SH7722_SETREG32( drv, JIFDSA2,  dev->jpeg_phys + SH7722GFX_JPEG_RELOAD_SIZE );
     SH7722_SETREG32( drv, JIFDDRSZ, SH7722GFX_JPEG_RELOAD_SIZE );
     SH7722_SETREG32( drv, JIFDDMW,  lock.pitch );
     SH7722_SETREG32( drv, JIFDDYA1, phys );
     SH7722_SETREG32( drv, JIFDDCA1, phys + lock.pitch * destination->config.size.h );

     D_DEBUG_AT( SH7722_JPEG, "  -> starting...\n" );

     /* Clear interrupts in shared flags. */
     shared->jpeg_ints = 0;

     /* Start decoder and begin reading from reload buffer. */
     SH7722_SETREG32( drv, JCCMD, JCCMD_START );
     SH7722_SETREG32( drv, JCCMD, JCCMD_READ_RESTART );

     /* Stall machine. */
     while (true) {
          /* Check for new interrupts in shared flags... */
          u32 ints = shared->jpeg_ints;
          if (ints) {
               /* ...and clear them (FIXME: race condition in case of multiple IRQs per command!). */
               shared->jpeg_ints &= ~ints;

               D_DEBUG_AT( SH7722_JPEG, "  -> JCSTS 0x%08x, JINTS 0x%08x\n", SH7722_GETREG32( drv, JCSTS ), ints );

               /* Check for errors! */
               if (ints & JINTS_INS5_ERROR) {
                    D_ERROR( "SH7722/JPEG: ERROR 0x%x!\n", SH7722_GETREG32( drv, JCDERR ) );
                    ret = DFB_IO;
                    break;
               }

               /* Done processing all data? */
               if (ints & JINTS_INS6_DONE) {
                    D_DEBUG_AT( SH7722_JPEG, "  -> DONE :)\n" );
                    break;
               }

               /* Check for header interception... */
               if (ints & JINTS_INS3_HEADER) {
                    /* ...remember image information... */
                    data->width  = SH7722_GETREG32( drv, JIFDDHSZ );
                    data->height = SH7722_GETREG32( drv, JIFDDVSZ );

                    D_DEBUG_AT( SH7722_JPEG, "  -> %dx%d (4:2:%c)\n", data->width, data->height,
                                '2' - (SH7722_GETREG32( drv, JCMOD ) & 2) );

                    /* ...and start decoding the actual image data... */
                    SH7722_SETREG32( drv, JCCMD, JCCMD_RESTART | JCCMD_END );
               }

               /* Check for reload interception... */
               if (ints & JINTS_INS14_RELOAD) {
                    D_DEBUG_AT( SH7722_JPEG, "  -> reloading...\n" );

                    /* ...reload buffers... */
                    if (!--loaded) {
                         data->buffer->GetData( data->buffer, SH7722GFX_JPEG_RELOAD_SIZE * 2, (void*) drv->jpeg_virt, &len );
                         loaded = 2;
                    }

                    /* ...and continue reading the image data... */
                    SH7722_SETREG32( drv, JCCMD, JCCMD_READ_RESTART );
               }
          }
          else {
               D_DEBUG_AT( SH7722_JPEG, "  -> waiting...\n" );

               /* ...otherwise wait for the arrival of new interrupt(s). */
               if (ioctl( drv->gfx_fd, SH7722GFX_IOCTL_WAIT_JPEG ) < 0) {
                    ret = errno2result( errno );

                    D_PERROR( "SH7722/JPEG: Waiting for IRQ failed! (ints: 0x%x - JINTS 0x%x, JCSTS 0x%x)\n",
                              ints, SH7722_GETREG32( drv, JINTS ), SH7722_GETREG32( drv, JCSTS ) );
                    break;
               }
          }
     }

     /* Unlock destination. */
     dfb_surface_unlock_buffer( destination, &lock );

     return ret;
}

/**********************************************************************************************************************/

static void
IDirectFBImageProvider_SH7722_JPEG_Destruct( IDirectFBImageProvider *thiz )
{
     IDirectFBImageProvider_SH7722_JPEG_data *data = thiz->priv;

     data->buffer->Release( data->buffer );

     DIRECT_DEALLOCATE_INTERFACE( thiz );
}

static DFBResult
IDirectFBImageProvider_SH7722_JPEG_AddRef( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA(IDirectFBImageProvider_SH7722_JPEG)

     data->ref++;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SH7722_JPEG_Release( IDirectFBImageProvider *thiz )
{
     DIRECT_INTERFACE_GET_DATA(IDirectFBImageProvider_SH7722_JPEG)

     if (--data->ref == 0)
          IDirectFBImageProvider_SH7722_JPEG_Destruct( thiz );

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SH7722_JPEG_RenderTo( IDirectFBImageProvider *thiz,
                                             IDirectFBSurface       *destination,
                                             const DFBRectangle     *dest_rect )
{
     DFBResult              ret;
     DFBRegion              clip;
     DFBRectangle           rect; 
     DFBSurfacePixelFormat  format;
     IDirectFBSurface_data *dst_data;
     CoreSurface           *dst_surface;

     DIRECT_INTERFACE_GET_DATA(IDirectFBImageProvider_SH7722_JPEG);

     DIRECT_INTERFACE_GET_DATA_FROM(destination, dst_data, IDirectFBSurface);

     dst_surface = dst_data->surface;
     if (!dst_surface)
          return DFB_DESTROYED;

     ret = destination->GetPixelFormat( destination, &format );
     if (ret)
          return ret;

     if (format != DSPF_NV12)
          return DFB_UNSUPPORTED;

     dfb_region_from_rectangle( &clip, &dst_data->area.current );

     if (dest_rect) {
          if (dest_rect->w < 1 || dest_rect->h < 1)
               return DFB_INVARG;
          
          rect.x = dest_rect->x + dst_data->area.wanted.x;
          rect.y = dest_rect->y + dst_data->area.wanted.y;
          rect.w = dest_rect->w;
          rect.h = dest_rect->h;
     }
     else
          rect = dst_data->area.wanted;

     if (!dfb_rectangle_region_intersects( &rect, &clip ))
          return DFB_OK;

     return DecodeHW( data, dst_surface, &rect, &clip );
}

static DFBResult
IDirectFBImageProvider_SH7722_JPEG_SetRenderCallback( IDirectFBImageProvider *thiz,
                                                      DIRenderCallback        callback,
                                                      void                   *context )
{
     DIRECT_INTERFACE_GET_DATA (IDirectFBImageProvider_SH7722_JPEG)

     data->render_callback         = callback;
     data->render_callback_context = context;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SH7722_JPEG_GetSurfaceDescription( IDirectFBImageProvider *thiz,
                                                          DFBSurfaceDescription  *desc )
{
     DIRECT_INTERFACE_GET_DATA(IDirectFBImageProvider_SH7722_JPEG)
     
     desc->flags       = DSDESC_WIDTH | DSDESC_HEIGHT | DSDESC_PIXELFORMAT;
     desc->height      = 128;//data->height;
     desc->width       = 128;//data->width;
     desc->pixelformat = DSPF_NV12;

     return DFB_OK;
}

static DFBResult
IDirectFBImageProvider_SH7722_JPEG_GetImageDescription( IDirectFBImageProvider *thiz,
                                                        DFBImageDescription    *desc )
{
     DIRECT_INTERFACE_GET_DATA(IDirectFBImageProvider_SH7722_JPEG)

     if (!desc)
          return DFB_INVARG;

     desc->caps = DICAPS_NONE;

     return DFB_OK;
}

/**********************************************************************************************************************/

static DFBResult
Probe( IDirectFBImageProvider_ProbeContext *ctx )
{
     if (ctx->header[0] == 0xff && ctx->header[1] == 0xd8)
          return DFB_OK;

     return DFB_UNSUPPORTED;
}

static DFBResult
Construct( IDirectFBImageProvider *thiz,
           ... )
{
     DFBResult            ret;
     IDirectFBDataBuffer *buffer;
     CoreDFB             *core;
     va_list              tag;

     DIRECT_ALLOCATE_INTERFACE_DATA(thiz, IDirectFBImageProvider_SH7722_JPEG);

     va_start( tag, thiz );
     buffer = va_arg( tag, IDirectFBDataBuffer * );
     core = va_arg( tag, CoreDFB * );
     va_end( tag );

     data->ref    = 1;
     data->buffer = buffer;
     data->core   = core;

     ret = buffer->AddRef( buffer );
     if (ret) {
          DIRECT_DEALLOCATE_INTERFACE(thiz);
          return ret;
     }

     thiz->AddRef                = IDirectFBImageProvider_SH7722_JPEG_AddRef;
     thiz->Release               = IDirectFBImageProvider_SH7722_JPEG_Release;
     thiz->RenderTo              = IDirectFBImageProvider_SH7722_JPEG_RenderTo;
     thiz->SetRenderCallback     = IDirectFBImageProvider_SH7722_JPEG_SetRenderCallback;
     thiz->GetImageDescription   = IDirectFBImageProvider_SH7722_JPEG_GetImageDescription;
     thiz->GetSurfaceDescription = IDirectFBImageProvider_SH7722_JPEG_GetSurfaceDescription;

     return DFB_OK;
}
