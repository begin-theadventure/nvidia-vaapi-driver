#include "export-buf.h"
#include <stdio.h>
#include <cudaEGL.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>

#ifndef EGL_NV_stream_consumer_eglimage
#define EGL_NV_stream_consumer_eglimage 1
#define EGL_STREAM_CONSUMER_IMAGE_NV      0x3373
#define EGL_STREAM_IMAGE_ADD_NV           0x3374
#define EGL_STREAM_IMAGE_REMOVE_NV        0x3375
#define EGL_STREAM_IMAGE_AVAILABLE_NV     0x3376
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
typedef EGLint (EGLAPIENTRYP PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMACQUIREIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
typedef EGLBoolean (EGLAPIENTRYP PFNEGLSTREAMRELEASEIMAGENVPROC) (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#ifdef EGL_EGLEXT_PROTOTYPES
EGLAPI EGLBoolean EGLAPIENTRY eglStreamImageConsumerConnectNV (EGLDisplay dpy, EGLStreamKHR stream, EGLint num_modifiers, EGLuint64KHR *modifiers, EGLAttrib *attrib_list);
EGLAPI EGLint EGLAPIENTRY eglQueryStreamConsumerEventNV (EGLDisplay dpy, EGLStreamKHR stream, EGLTime timeout, EGLenum *event, EGLAttrib *aux);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamAcquireImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage *pImage, EGLSync sync);
EGLAPI EGLBoolean EGLAPIENTRY eglStreamReleaseImageNV (EGLDisplay dpy, EGLStreamKHR stream, EGLImage image, EGLSync sync);
#endif
#endif /* EGL_NV_stream_consumer_eglimage */

PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC eglQueryStreamConsumerEventNV;
PFNEGLSTREAMRELEASEIMAGENVPROC eglStreamReleaseImageNV;
PFNEGLSTREAMACQUIREIMAGENVPROC eglStreamAcquireImageNV;
PFNEGLEXPORTDMABUFIMAGEMESAPROC eglExportDMABUFImageMESA;
PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC eglExportDMABUFImageQueryMESA;

void debug(EGLenum error,const char *command,EGLint messageType,EGLLabelKHR threadLabel,EGLLabelKHR objectLabel,const char* message) {
    LOG("[EGL] %s\n", message);
}

void initExporter(NVDriver *drv) {
    eglQueryStreamConsumerEventNV = (PFNEGLQUERYSTREAMCONSUMEREVENTNVPROC) eglGetProcAddress("eglQueryStreamConsumerEventNV");
    eglStreamReleaseImageNV = (PFNEGLSTREAMRELEASEIMAGENVPROC) eglGetProcAddress("eglStreamReleaseImageNV");
    eglStreamAcquireImageNV = (PFNEGLSTREAMACQUIREIMAGENVPROC) eglGetProcAddress("eglStreamAcquireImageNV");
    eglExportDMABUFImageMESA = (PFNEGLEXPORTDMABUFIMAGEMESAPROC) eglGetProcAddress("eglExportDMABUFImageMESA");
    eglExportDMABUFImageQueryMESA = (PFNEGLEXPORTDMABUFIMAGEQUERYMESAPROC) eglGetProcAddress("eglExportDMABUFImageQueryMESA");

    PFNEGLCREATESTREAMKHRPROC eglCreateStreamKHR = (PFNEGLCREATESTREAMKHRPROC) eglGetProcAddress("eglCreateStreamKHR");
    PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC eglStreamImageConsumerConnectNV = (PFNEGLSTREAMIMAGECONSUMERCONNECTNVPROC) eglGetProcAddress("eglStreamImageConsumerConnectNV");

    drv->eglDisplay = eglGetDisplay(NULL);
    eglInitialize(drv->eglDisplay, NULL, NULL);
    EGLint stream_attrib_list[] = { EGL_STREAM_FIFO_LENGTH_KHR, 10, EGL_NONE };
    drv->eglStream = eglCreateStreamKHR(drv->eglDisplay, stream_attrib_list);
    EGLAttrib consumer_attrib_list[] = { EGL_NONE };
    eglStreamImageConsumerConnectNV(drv->eglDisplay, drv->eglStream, 0, 0, consumer_attrib_list);
    checkCudaErrors(cuEGLStreamProducerConnect(&drv->cuStreamConnection, drv->eglStream, 1024, 1024));

    PFNEGLDEBUGMESSAGECONTROLKHRPROC eglDebugMessageControlKHR = (PFNEGLDEBUGMESSAGECONTROLKHRPROC) eglGetProcAddress("eglDebugMessageControlKHR");
    //setup debug logging
    EGLAttrib debugAttribs[] = {EGL_DEBUG_MSG_WARN_KHR, EGL_TRUE, EGL_DEBUG_MSG_INFO_KHR, EGL_TRUE, EGL_NONE};
    eglDebugMessageControlKHR(debug, debugAttribs);
}

void exportCudaPtr(NVDriver *drv, CUdeviceptr ptr, NVSurface *surface, uint32_t pitch, int *fourcc, int *fds, int *offsets, int *strides, uint64_t *mods, int *bppOut) {
    static int numFramesPresented = 0;
    // If there is a frame presented before we check if consumer
    // is done with it using cuEGLStreamProducerReturnFrame.
    //LOG("outstanding frames: %d\n", numFramesPresented);
    CUeglFrame eglframe = {
        .frame = {
            .pArray = {0, 0, 0}
        }
    };
    while (numFramesPresented) {
      //LOG("waiting for returned frame\n");
      CUresult cuStatus = cuEGLStreamProducerReturnFrame(&drv->cuStreamConnection, &eglframe, NULL);
      if (cuStatus == CUDA_ERROR_LAUNCH_TIMEOUT) {
        //LOG("timeout with %d outstanding\n", numFramesPresented);
        break;
      } else if (cuStatus != CUDA_SUCCESS) {
        checkCudaErrors(cuStatus);
      } else {
        //LOG("returned frame %dx%d %p %p\n", eglframe.width, eglframe.height, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
        numFramesPresented--;
      }
    }

    uint32_t width = surface->width;
    uint32_t height = surface->height;

    //check if the frame size if different and release the arrays
    //TODO figure out how to get the EGLimage freed aswell
    if (eglframe.width != width && eglframe.height != height) {
        if (eglframe.frame.pArray[0] != NULL) {
            cuArrayDestroy(eglframe.frame.pArray[0]);
            eglframe.frame.pArray[0] = NULL;
        }
        if (eglframe.frame.pArray[1] != NULL) {
            cuArrayDestroy(eglframe.frame.pArray[1]);
            eglframe.frame.pArray[1] = NULL;
        }
    }
    eglframe.width = width;
    eglframe.height = height;
    eglframe.depth = 1;
    eglframe.pitch = 0;
    eglframe.planeCount = 2;
    eglframe.numChannels = 1;
    eglframe.frameType = CU_EGL_FRAME_TYPE_ARRAY;

    int bpp = 1;
    if (surface->format == cudaVideoSurfaceFormat_NV12) {
        eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_YVU420_SEMIPLANAR;
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT8;
    } else if (surface->format == cudaVideoSurfaceFormat_P016) {
        //TODO not working, produces this error in mpv:
        //EGL_BAD_MATCH error: In eglCreateImageKHR: requested LINUX_DRM_FORMAT is not supported
        //this error seems to be coming from the NVIDIA EGL driver
        //this might be caused by the DRM_FORMAT_*'s below
        if (surface->bitdepth == 10) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y10V10U10_420_SEMIPLANAR;
        } else if (surface->bitdepth == 12) {
            eglframe.eglColorFormat = CU_EGL_COLOR_FORMAT_Y12V12U12_420_SEMIPLANAR;
        } else {
            LOG("Unknown bitdepth\n");
        }
        eglframe.cuFormat = CU_AD_FORMAT_UNSIGNED_INT16;
        bpp = 2;
    }
    *bppOut = bpp;

    //TODO in theory this should work, but the application attempting to bind that texture gets the following error:
    //GL_INVALID_OPERATION error generated. <image> and <target> are incompatible
    //eglframe.frameType = CU_EGL_FRAME_TYPE_PITCH;
    //eglframe.pitch = pitch;
    //eglframe.frame.pPitch[0] = (void*) ptr;
    //eglframe.frame.pPitch[1] = (void*) ptr + (height*pitch);

    //reuse the arrays if we can
    //creating new arrays will cause a new EGLimage to be created and you'll eventually run out of resources
    if (eglframe.frame.pArray[0] == NULL) {
        CUDA_ARRAY3D_DESCRIPTOR arrDesc = {
            .Width = width,
            .Height = height,
            .Depth = 0,
            .NumChannels = 1,
            .Flags = 0,
            .Format = eglframe.cuFormat
        };
        checkCudaErrors(cuArray3DCreate(&eglframe.frame.pArray[0], &arrDesc));
    }
    if (eglframe.frame.pArray[1] == NULL) {
        CUDA_ARRAY3D_DESCRIPTOR arr2Desc = {
            .Width = width >> 1,
            .Height = height >> 1,
            .Depth = 0,
            .NumChannels = 2,
            .Flags = 0,
            .Format = eglframe.cuFormat
        };
        checkCudaErrors(cuArray3DCreate(&eglframe.frame.pArray[1], &arr2Desc));
    }
    CUDA_MEMCPY3D cpy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = eglframe.frame.pArray[0],
        .Height = height,
        .WidthInBytes = width * bpp,
        .Depth = 1
    };
    checkCudaErrors(cuMemcpy3D(&cpy));
    CUDA_MEMCPY3D cpy2 = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = ptr,
        .srcY = height,
        .srcPitch = pitch,
        .dstMemoryType = CU_MEMORYTYPE_ARRAY,
        .dstArray = eglframe.frame.pArray[1],
        .Height = height >> 1,
        .WidthInBytes = (width >> 1) * 2 * bpp,
        .Depth = 1
    };
    checkCudaErrors(cuMemcpy3D(&cpy2));

    //LOG("Presenting frame %dx%d %p %p\n", eglframe.width, eglframe.height, eglframe.frame.pArray[0], eglframe.frame.pArray[1]);
    checkCudaErrors(cuEGLStreamProducerPresentFrame( &drv->cuStreamConnection, eglframe, NULL ));
    numFramesPresented++;

    while (1) {
        EGLenum event = 0;
        EGLAttrib aux = 0;
        EGLint eventRet = eglQueryStreamConsumerEventNV(drv->eglDisplay, drv->eglStream, 0, &event, &aux);
        if (eventRet == EGL_TIMEOUT_EXPIRED_KHR) {
            break;
        }

        if (event == EGL_STREAM_IMAGE_ADD_NV) {
            EGLImage image = eglCreateImage(drv->eglDisplay, EGL_NO_CONTEXT, EGL_STREAM_CONSUMER_IMAGE_NV, drv->eglStream, NULL);
            LOG("Adding image from EGLStream, eglCreateImage: %p\n", image);
        } else if (event == EGL_STREAM_IMAGE_AVAILABLE_NV) {
            EGLImage img;
            //somehow we get here with the previous frame, not the next one
            if (!eglStreamAcquireImageNV(drv->eglDisplay, drv->eglStream, &img, EGL_NO_SYNC_NV)) {
                LOG("eglStreamAcquireImageNV failed\n");
                exit(EXIT_FAILURE);
            }

            int planes = 0;
            if (!eglExportDMABUFImageQueryMESA(drv->eglDisplay, img, fourcc, &planes, mods)) {
                LOG("eglExportDMABUFImageQueryMESA failed\n");
                exit(EXIT_FAILURE);
            }

            LOG("eglExportDMABUFImageQueryMESA: %p %.4s (%x) planes:%d mods:%lx %lx\n", img, (char*)fourcc, *fourcc, planes, mods[0], mods[1]);

            EGLBoolean r = eglExportDMABUFImageMESA(drv->eglDisplay, img, fds, strides, offsets);

            if (!r) {
                LOG("Unable to export image\n");
            }
            //LOG("eglExportDMABUFImageMESA: %d %d %d %d %d\n", r, fds[0], fds[1], fds[2], fds[3]);
            //LOG("strides: %d %d %d %d\n", strides[0], strides[1], strides[2], strides[3]);
            //LOG("offsets: %d %d %d %d\n", offsets[0], offsets[1], offsets[2], offsets[3]);

            r = eglStreamReleaseImageNV(drv->eglDisplay, drv->eglStream, img, EGL_NO_SYNC_NV);
            if (!r) {
                LOG("Unable to release image\n");
            }
        } else if (event == EGL_STREAM_IMAGE_REMOVE_NV) {
            eglDestroyImage(drv->eglDisplay, (EGLImage) aux);
            LOG("Removing image from EGLStream, eglDestroyImage: %p\n", (EGLImage) aux);
        } else {
            LOG("Unhandled event: %X\n", event);
        }
    }
}