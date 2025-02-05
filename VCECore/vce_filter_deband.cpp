﻿// -----------------------------------------------------------------------------------------
// NVEnc by rigaya
// -----------------------------------------------------------------------------------------
//
// The MIT License
//
// Copyright (c) 2014-2016 rigaya
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
//
// ------------------------------------------------------------------------------------------

#define _USE_MATH_DEFINES
#include <cmath>
#include <map>
#include <array>
#include "rgy_osdep.h"
#include "rgy_opencl.h"
#include "vce_filter_deband.h"
#pragma warning(push)
#pragma warning(disable:4819) //ファイルは、現在のコード ページ (932) で表示できない文字を含んでいます。
#pragma warning(disable:4201) //非標準の拡張機能が使用されています: 無名の構造体または共用体です。
#pragma warning(disable:4244) //'return': 'double' から 'cl_float' への変換です。データが失われる可能性があります。
#pragma warning(disable:4267) //'初期化中': 'size_t' から 'cl_uint' に変換しました。データが失われているかもしれません。
#include <clRNG/mrg31k3p.h>
#ifdef __cplusplus
extern "C" {
#endif
#include "../clRNG/src/library/mrg31k3p.c"
#ifdef __cplusplus
}
#endif
#pragma warning(pop)

using clrngStreams = clrngMrg31k3pStream;
#define clrngCreateStreams clrngMrg31k3pCreateStreams
#define clrngDestroyStreams clrngMrg31k3pDestroyStreams

void clrngStreamDeleter::operator()(void *ptr) const {
    clrngDestroyStreams((clrngStreams *)ptr);
}

static const int DEBAND_BLOCK_THREAD_X = 32;
static const int DEBAND_BLOCK_THREAD_Y = 8;
static const int DEBAND_BLOCK_LOOP_X_OUTER = 2;
static const int DEBAND_BLOCK_LOOP_Y_OUTER = 2;
static const int DEBAND_BLOCK_LOOP_X_INNER = 1;
static const int DEBAND_BLOCK_LOOP_Y_INNER = 2;

static const int GEN_RAND_BLOCK_LOOP_Y = 1;

RGY_ERR RGYFilterDeband::procPlane(FrameInfo *pOutputPlane, const FrameInfo *pInputPlane, const FrameInfo *pRandPlane,
    const int range_plane, const float dither_range, const float threshold_float, const int field_mask, const RGY_PLANE plane,
    RGYOpenCLQueue &queue, const std::vector<RGYOpenCLEvent> &wait_events, RGYOpenCLEvent *event) {
    {
        const char *kernel_name = "kernel_deband";
        RGYWorkSize local(DEBAND_BLOCK_THREAD_X, DEBAND_BLOCK_THREAD_Y);
        RGYWorkSize global(
            divCeil(pOutputPlane->width,  DEBAND_BLOCK_LOOP_X_OUTER * DEBAND_BLOCK_LOOP_X_INNER),
            divCeil(pOutputPlane->height, DEBAND_BLOCK_LOOP_Y_OUTER * DEBAND_BLOCK_LOOP_Y_INNER));

        auto err = m_deband->kernel(kernel_name).config(queue, local, global, wait_events, event).launch(
            (cl_mem)pOutputPlane->ptr[0], pOutputPlane->pitch[0], pOutputPlane->width, pOutputPlane->height,
            (cl_mem)pRandPlane->ptr[0], pRandPlane->pitch[0],
            (cl_mem)pInputPlane->ptr[0],
            range_plane, dither_range, threshold_float, field_mask, (int)plane);
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("error at %s (procPlane(%s)): %s.\n"),
                char_to_tstring(kernel_name).c_str(), RGY_CSP_NAMES[pInputPlane->csp], get_err_mes(err));
            return err;
        }
    }
    return RGY_ERR_NONE;
}

RGY_ERR RGYFilterDeband::procFrame(FrameInfo *pOutputFrame, const FrameInfo *pInputFrame, RGYOpenCLQueue &queue, const std::vector<RGYOpenCLEvent> &wait_events, RGYOpenCLEvent *event) {
    auto prm = std::dynamic_pointer_cast<RGYFilterParamDeband>(m_param);
    if (!prm) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }

    std::vector<RGYOpenCLEvent> frame_wait_event = wait_events;
    if (prm->deband.randEachFrame) {
        auto err = genRand(queue, wait_events, nullptr);
        if (err != RGY_ERR_NONE) {
            return err;
        }
        frame_wait_event = std::vector<RGYOpenCLEvent>();
    }

    m_srcImage = m_cl->createImageFromFrameBuffer(*pInputFrame, true, CL_MEM_READ_ONLY);
    for (int i = 0; i < RGY_CSP_PLANES[pOutputFrame->csp]; i++) {
        const auto iplane = (RGY_PLANE)i;
        auto planeDst = getPlane(pOutputFrame, iplane);
        auto planeSrc = getPlane(&m_srcImage->frame, iplane);
        auto frameRnd = (iplane == RGY_PLANE_Y) ? m_randBufY->frame : m_randBufUV->frame;
        const std::vector<RGYOpenCLEvent> &plane_wait_event = (i == 0) ? frame_wait_event : std::vector<RGYOpenCLEvent>();
        RGYOpenCLEvent *plane_event = (i == RGY_CSP_PLANES[pOutputFrame->csp] - 1) ? event : nullptr;

        const int range_plane = (RGY_CSP_CHROMA_FORMAT[pInputFrame->csp] == RGY_CHROMAFMT_YUV420 && iplane == RGY_PLANE_Y) ? prm->deband.range >> 1 : prm->deband.range;

        const auto dither = (iplane == RGY_PLANE_Y) ? prm->deband.ditherY : prm->deband.ditherC;
        const float dither_range = (float)dither * std::pow(2.0f, RGY_CSP_BIT_DEPTH[pInputFrame->csp] - 12) + 0.5f;

        const auto threshold = (iplane == RGY_PLANE_Y) ? prm->deband.threY : (iplane == RGY_PLANE_U) ? prm->deband.threCb : prm->deband.threCr;
        const float threshold_float = (threshold << (!(prm->deband.sample && prm->deband.blurFirst) + 1)) * (1.0f / (1 << 12));

        const int field_mask = (interlaced(*pInputFrame)) ? -2 : -1;

        auto err = procPlane(&planeDst, &planeSrc, &frameRnd, range_plane, dither_range, threshold_float, field_mask, iplane, queue, plane_wait_event, plane_event);
        if (err != RGY_ERR_NONE) {
            m_pLog->write(RGY_LOG_ERROR, _T("Failed to denoise(deband) frame(%d) %s: %s\n"), i, cl_errmes(err));
            return err_cl_to_rgy(err);
        }
    }
    return RGY_ERR_NONE;
}

RGY_ERR RGYFilterDeband::initRand() {

    RGYWorkSize local(DEBAND_BLOCK_THREAD_X, DEBAND_BLOCK_THREAD_Y, 1);
    RGYWorkSize global(divCeil(m_randBufY->frame.width, 2), divCeil(m_randBufY->frame.height, 2 * GEN_RAND_BLOCK_LOOP_Y), 1);
    RGYWorkSize groups = global.groups(local);

    const auto numWorkItems = groups(0) * groups(1) * local(0) * local(1);

    clrngStatus sts = CLRNG_SUCCESS;
    size_t streamBufferSize = 0;
    m_rngStream = std::unique_ptr<clrngStreams, clrngStreamDeleter>(clrngCreateStreams(nullptr, numWorkItems, &streamBufferSize, &sts), clrngStreamDeleter());
    if (sts != CLRNG_SUCCESS) {
        AddMessage(RGY_LOG_ERROR, _T("failed to create clrng stream: %s.\n"), char_to_tstring(clrngGetErrorString()).c_str());
        return RGY_ERR_MEMORY_ALLOC;
    }
    m_randStreamBuf = m_cl->copyDataToBuffer(m_rngStream.get(), streamBufferSize, CL_MEM_READ_WRITE, m_cl->queue().get());
    if (!m_randStreamBuf) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory for rnd stream: %s.\n"));
        return RGY_ERR_MEMORY_ALLOC;
    }
    return RGY_ERR_NONE;
}

RGY_ERR RGYFilterDeband::genRand(RGYOpenCLQueue &queue, const std::vector<RGYOpenCLEvent> &wait_events, RGYOpenCLEvent *event) {
    const char *kernel_name = "kernel_deband_gen_rand";
    RGYWorkSize local(DEBAND_BLOCK_THREAD_X, DEBAND_BLOCK_THREAD_Y, 1);
    RGYWorkSize global(divCeil(m_randBufY->frame.width, 2), divCeil(m_randBufY->frame.height, 2 * GEN_RAND_BLOCK_LOOP_Y), 1);

    auto err = m_debandGenRand->kernel(kernel_name).config(queue, local, global, wait_events, event).launch(
        (cl_mem)m_randBufY->frame.ptr[0], (cl_mem)m_randBufUV->frame.ptr[0],
        m_randBufY->frame.pitch[0], m_randBufUV->frame.pitch[0],
        m_randBufY->frame.width, m_randBufY->frame.height,
        m_randStreamBuf->mem());
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("error at %s (genRand): %s.\n"),
            char_to_tstring(kernel_name).c_str(), get_err_mes(err));
        return err;
    }
    return RGY_ERR_NONE;
}

RGYFilterDeband::RGYFilterDeband(shared_ptr<RGYOpenCLContext> context) : RGYFilter(context), m_deband(), m_debandGenRand(), m_rngStream(), m_randStreamBuf(), m_randBufY(), m_randBufUV(), m_srcImage() {
    m_name = _T("deband");
}

RGYFilterDeband::~RGYFilterDeband() {
    close();
}

std::string RGYFilterDeband::getEmbeddedResourceStr(const tstring &name, const tstring &type) {
    std::string data_str;
    AddMessage(RGY_LOG_DEBUG, _T("Load resource type: %s, name: %s\n"), type.c_str(), name.c_str());
    {
        char *data = nullptr;
        int size = getEmbeddedResource((void **)&data, name.c_str(), type.c_str());
        if (size == 0) {
            AddMessage(RGY_LOG_ERROR, _T("failed to load %s(m_deband)\n"), name.c_str());
        } else {

            auto datalen = size;
            {
                const uint8_t *ptr = (const uint8_t *)data;
                if (ptr[0] == 0xEF && ptr[1] == 0xBB && ptr[2] == 0xBF) { //skip UTF-8 BOM
                    data += 3;
                    datalen -= 3;
                }
            }
            data_str = data;
        }
    }
    return data_str;
}

RGY_ERR RGYFilterDeband::init(shared_ptr<RGYFilterParam> pParam, shared_ptr<RGYLog> pPrintMes) {
    RGY_ERR sts = RGY_ERR_NONE;
    m_pLog = pPrintMes;
    auto prm = std::dynamic_pointer_cast<RGYFilterParamDeband>(pParam);
    if (!prm) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter type.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    //パラメータチェック
    if (prm->frameOut.height <= 0 || prm->frameOut.width <= 0) {
        AddMessage(RGY_LOG_ERROR, _T("Invalid parameter.\n"));
        return RGY_ERR_INVALID_PARAM;
    }
    if (prm->deband.range < 0 || 127 < prm->deband.range) {
        AddMessage(RGY_LOG_WARN, _T("range must be in range of 0 - 127.\n"));
        prm->deband.range = clamp(prm->deband.range, 0, 127);
    }
    if (prm->deband.threY < 0 || 31 < prm->deband.threY) {
        AddMessage(RGY_LOG_WARN, _T("threY must be in range of 0 - 31.\n"));
        prm->deband.threY = clamp(prm->deband.threY, 0, 31);
    }
    if (prm->deband.threCb < 0 || 31 < prm->deband.threCb) {
        AddMessage(RGY_LOG_WARN, _T("threCb must be in range of 0 - 31.\n"));
        prm->deband.threCb = clamp(prm->deband.threCb, 0, 31);
    }
    if (prm->deband.threCr < 0 || 31 < prm->deband.threCr) {
        AddMessage(RGY_LOG_WARN, _T("threCr must be in range of 0 - 31.\n"));
        prm->deband.threCr = clamp(prm->deband.threCr, 0, 31);
    }
    if (prm->deband.ditherY < 0 || 31 < prm->deband.ditherY) {
        AddMessage(RGY_LOG_WARN, _T("ditherY must be in range of 0 - 31.\n"));
        prm->deband.ditherY = clamp(prm->deband.ditherY, 0, 31);
    }
    if (prm->deband.ditherC < 0 || 31 < prm->deband.ditherC) {
        AddMessage(RGY_LOG_WARN, _T("ditherC must be in range of 0 - 31.\n"));
        prm->deband.ditherC = clamp(prm->deband.ditherC, 0, 31);
    }
    if (prm->deband.sample < 0 || 2 < prm->deband.sample) {
        AddMessage(RGY_LOG_WARN, _T("mode must be in range of 0 - 2.\n"));
        prm->deband.sample = clamp(prm->deband.sample, 0, 2);
    }
    if (!m_deband
        || !m_debandGenRand
        || std::dynamic_pointer_cast<RGYFilterParamDeband>(m_param)->deband != prm->deband) {

        {
            const auto options = strsprintf("-D Type=%s -D bit_depth=%d -D sample_mode=%d -D blur_first=%d"
                " -D block_loop_x_inner=%d  -D block_loop_y_inner=%d  -D block_loop_x_outer=%d -D block_loop_y_outer=%d",
                RGY_CSP_BIT_DEPTH[prm->frameOut.csp] > 8 ? "ushort" : "uchar",
                RGY_CSP_BIT_DEPTH[prm->frameOut.csp],
                prm->deband.sample,
                prm->deband.blurFirst,
                DEBAND_BLOCK_LOOP_X_INNER, DEBAND_BLOCK_LOOP_Y_INNER, DEBAND_BLOCK_LOOP_X_OUTER, DEBAND_BLOCK_LOOP_Y_OUTER);
            m_deband = m_cl->buildResource(_T("VCE_FILTER_DEBAND_CL"), _T("EXE_DATA"), options.c_str());
            if (!m_deband) {
                AddMessage(RGY_LOG_ERROR, _T("failed to load VCE_FILTER_DEBAND_CL(m_deband)\n"));
                return RGY_ERR_OPENCL_CRUSH;
            }
        }

        {
            auto deband_gen_rand_cl   = getEmbeddedResourceStr(_T("VCE_FILTER_DEBAND_GEN_RAND_CL"),        _T("EXE_DATA"));
            auto clrng_clh            = getEmbeddedResourceStr(_T("VCE_FILTER_CLRNG_CLH"),                 _T("EXE_DATA"));
            auto mrg31k3p_clh         = getEmbeddedResourceStr(_T("VCE_FILTER_CLRNG_MRG31K3P_CLH"),        _T("EXE_DATA"));
            auto mrg31k3p_private_c_h = getEmbeddedResourceStr(_T("VCE_FILTER_CLRNG_MRG31K3P_PRIVATE_CH"), _T("EXE_DATA"));

            //includeをチェック
            {
                auto pos = mrg31k3p_clh.find("#include <clRNG/clRNG.clh>");
                if (pos == std::string::npos) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to search #include <clRNG/clRNG.clh>\n"));
                    return RGY_ERR_UNKNOWN;
                }
                pos = mrg31k3p_clh.find("#include <clRNG/private/mrg31k3p.c.h>");
                if (pos == std::string::npos) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to search #include <clRNG/private/mrg31k3p.c.h>\n"));
                    return RGY_ERR_UNKNOWN;
                }
                pos = deband_gen_rand_cl.find("#include <clRNG/mrg31k3p.clh>");
                if (pos == std::string::npos) {
                    AddMessage(RGY_LOG_ERROR, _T("failed to search #include <clRNG/mrg31k3p.clh>\n"));
                    return RGY_ERR_UNKNOWN;
                }
            }

            //includeの反映
            mrg31k3p_clh = str_replace(mrg31k3p_clh, "#include <clRNG/clRNG.clh>", clrng_clh);
            mrg31k3p_clh = str_replace(mrg31k3p_clh, "#include <clRNG/private/mrg31k3p.c.h>", mrg31k3p_private_c_h);
            auto deband_gen_rand_source = str_replace(deband_gen_rand_cl, "#include <clRNG/mrg31k3p.clh>", mrg31k3p_clh);
            const auto options = strsprintf("-D yuv420=%d -D gen_rand_block_loop_y=%d",
                RGY_CSP_CHROMA_FORMAT[prm->frameOut.csp] == RGY_CHROMAFMT_YUV420,
                GEN_RAND_BLOCK_LOOP_Y);
            m_debandGenRand = m_cl->build(deband_gen_rand_source, options.c_str());
            if (!m_debandGenRand) {
                AddMessage(RGY_LOG_ERROR, _T("failed to load VCE_FILTER_DEBAND_GEN_RAND_CL(m_debandGenRand)\n"));
                return RGY_ERR_OPENCL_CRUSH;
            }
        }
        {
            FrameInfo rndBufFrame = prm->frameOut;
            rndBufFrame.csp = RGY_CSP_RGB32;
            m_randBufY = m_cl->createFrameBuffer(rndBufFrame, CL_MEM_READ_WRITE);
            if (!m_randBufY) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate buffer for Random numbers\n"));
                return RGY_ERR_OPENCL_CRUSH;
            }
            m_randBufUV = m_cl->createFrameBuffer(rndBufFrame, CL_MEM_READ_WRITE);
            if (!m_randBufUV) {
                AddMessage(RGY_LOG_ERROR, _T("failed to allocate buffer for Random numbers\n"));
                return RGY_ERR_OPENCL_CRUSH;
            }
        }
        auto err = initRand();
        if (err != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed to initilaize Random generator: %s\n"), get_err_mes(err));
            return RGY_ERR_OPENCL_CRUSH;
        }
        if ((err = genRand(m_cl->queue(), {}, nullptr)) != RGY_ERR_NONE) {
            AddMessage(RGY_LOG_ERROR, _T("failed to generate Random numbers: %s\n"), get_err_mes(err));
            return RGY_ERR_OPENCL_CRUSH;
        }
    }

    auto err = AllocFrameBuf(prm->frameOut, 1);
    if (err != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("failed to allocate memory: %s.\n"), get_err_mes(err));
        return RGY_ERR_MEMORY_ALLOC;
    }
    for (int i = 0; i < RGY_CSP_PLANES[m_frameBuf[0]->frame.csp]; i++) {
        prm->frameOut.pitch[i] = m_frameBuf[0]->frame.pitch[i];
    }

    //コピーを保存
    setFilterInfo(prm->print());
    m_param = prm;
    return sts;
}

RGY_ERR RGYFilterDeband::run_filter(const FrameInfo *pInputFrame, FrameInfo **ppOutputFrames, int *pOutputFrameNum, RGYOpenCLQueue &queue, const std::vector<RGYOpenCLEvent> &wait_events, RGYOpenCLEvent *event) {
    RGY_ERR sts = RGY_ERR_NONE;
    if (pInputFrame->ptr[0] == nullptr) {
        return sts;
    }

    *pOutputFrameNum = 1;
    if (ppOutputFrames[0] == nullptr) {
        auto pOutFrame = m_frameBuf[0].get();
        ppOutputFrames[0] = &pOutFrame->frame;
    }
    ppOutputFrames[0]->picstruct = pInputFrame->picstruct;
    //if (interlaced(*pInputFrame)) {
    //    return filter_as_interlaced_pair(pInputFrame, ppOutputFrames[0], cudaStreamDefault);
    //}
    const auto memcpyKind = getMemcpyKind(pInputFrame->mem_type, ppOutputFrames[0]->mem_type);
    if (memcpyKind != RGYCLMemcpyD2D) {
        AddMessage(RGY_LOG_ERROR, _T("only supported on device memory.\n"));
        return RGY_ERR_UNSUPPORTED;
    }
    if (m_param->frameOut.csp != m_param->frameIn.csp) {
        AddMessage(RGY_LOG_ERROR, _T("csp does not match.\n"));
        return RGY_ERR_UNSUPPORTED;
    }

    sts = procFrame(ppOutputFrames[0], pInputFrame, queue, wait_events, event);
    if (sts != RGY_ERR_NONE) {
        AddMessage(RGY_LOG_ERROR, _T("error at denoiseFrame (%s): %s.\n"),
            RGY_CSP_NAMES[pInputFrame->csp], get_err_mes(sts));
        return sts;
    }

    return sts;
}

void RGYFilterDeband::close() {
    m_frameBuf.clear();
    m_srcImage.reset();
    m_randBufUV.reset();
    m_randBufY.reset();
    m_rngStream.reset();
    m_randStreamBuf.reset();
    m_debandGenRand.reset();
    m_deband.reset();
    m_cl.reset();
    m_bInterlacedWarn = false;
}
