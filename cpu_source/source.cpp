#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#if __cplusplus >= 202002L
#include <numbers>
#endif
#include <shared_mutex>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <VapourSynth.h>
#include <VSHelper.h>

#include "kernel.hpp"

#include <config.h> // generated by cmake

template <typename T, typename T_in>
    requires
        (std::is_same_v<T_in, T> || std::is_same_v<T_in, std::complex<T>>)
static void dft(
    std::complex<T> * VS_RESTRICT dst,
    const T_in * VS_RESTRICT src,
    int n,
    int stride
) {
#if __cplusplus >= 202002L
    const auto pi = std::numbers::pi_v<T>;
#else
    const auto pi = static_cast<T>(M_PI);
#endif

    int out_num = std::is_floating_point_v<T_in> ? (n / 2 + 1) : n;
    for (int i = 0; i < out_num; i++) {
        std::complex<T> sum {};
        for (int j = 0; j < n; j++) {
            auto imag = -2 * i * j * pi / n;
            auto weight = std::complex(std::cos(imag), std::sin(imag));
            sum += src[j * stride] * weight;
        }
        dst[i * stride] = sum;
    }
}

template <typename T>
static T square(const T & x) {
    return x * x;
}

static int calc_pad_size(int size, int block_size, int block_step) {
    return (
        size
        + ((size % block_size) ? block_size - size % block_size : 0)
        + std::max(block_size - block_step, block_step) * 2
    );
}

static int calc_pad_num(int size, int block_size, int block_step) {
    return (calc_pad_size(size, block_size, block_step) - block_size) / block_step + 1;
}

template <typename T>
static void reflection_padding_impl(
    T * VS_RESTRICT dst, // shape: (pad_height, pad_width)
    const T * VS_RESTRICT src, // shape: (height, stride)
    int width, int height, int stride,
    int block_size, int block_step
) {

    int pad_width = calc_pad_size(width, block_size, block_step);
    int pad_height = calc_pad_size(height, block_size, block_step);

    int offset_y = (pad_height - height) / 2;
    int offset_x = (pad_width - width) / 2;

    vs_bitblt(
        &dst[offset_y * pad_width + offset_x], pad_width * sizeof(T),
        src, stride * sizeof(T),
        width * sizeof(T), height
    );

    // copy left and right regions
    for (int y = offset_y; y < offset_y + height; y++) {
        auto dst_line = &dst[y * pad_width];

        for (int x = 0; x < offset_x; x++) {
            dst_line[x] = dst_line[offset_x * 2 - x];
        }

        for (int x = offset_x + width; x < pad_width; x++) {
            dst_line[x] = dst_line[2 * (offset_x + width) - 2 - x];
        }
    }

    // copy top region
    for (int y = 0; y < offset_y; y++) {
        std::memcpy(
            &dst[y * pad_width],
            &dst[(offset_y * 2 - y) * pad_width],
            pad_width * sizeof(T)
        );
    }

    // copy bottom region
    for (int y = offset_y + height; y < pad_height; y++) {
        std::memcpy(
            &dst[y * pad_width],
            &dst[(2 * (offset_y + height) - 2 - y) * pad_width],
            pad_width * sizeof(T)
        );
    }
}

static void reflection_padding(
    uint8_t * VS_RESTRICT dst, // shape: (pad_height, pad_width)
    const uint8_t * VS_RESTRICT src, // shape: (height, stride)
    int width, int height, int stride,
    int block_size, int block_step,
    int bytes_per_sample
) {

    if (bytes_per_sample == 1) {
        reflection_padding_impl(
            static_cast<uint8_t *>(dst),
            static_cast<const uint8_t *>(src),
            width, height, stride,
            block_size, block_step
        );
    } else if (bytes_per_sample == 2) {
        reflection_padding_impl(
            reinterpret_cast<uint16_t *>(dst),
            reinterpret_cast<const uint16_t *>(src),
            width, height, stride,
            block_size, block_step
        );
    } else if (bytes_per_sample == 4) {
        reflection_padding_impl(
            reinterpret_cast<uint32_t *>(dst),
            reinterpret_cast<const uint32_t *>(src),
            width, height, stride,
            block_size, block_step
        );
    }
}

static void load_block(
    Vec16f * VS_RESTRICT block,
    const uint8_t * VS_RESTRICT shifted_src,
    int radius,
    int block_size,
    int block_step,
    int width,
    int height,
    const Vec16f * VS_RESTRICT window,
    int bits_per_sample
) {

    float scale = 1.0f / (1 << (bits_per_sample - 8));
    if (bits_per_sample == 32) {
        scale = 255.0f;
    }

    int bytes_per_sample = (bits_per_sample + 7) / 8;

    assert(block_size == 16);
    block_size = 16; // unsafe

    int offset_x = calc_pad_size(width, block_size, block_step);
    int offset_y = calc_pad_size(height, block_size, block_step);

    if (bytes_per_sample == 1) {
        for (int i = 0; i < 2 * radius + 1; i++) {
            for (int j = 0; j < block_size; j++) {
                auto vec_input = Vec16uc().load((const uint8_t *) shifted_src + (i * offset_y + j) * offset_x);
                auto vec_input_f = to_float(Vec16i(extend(extend(vec_input))));
                block[i * block_size * 2 + j] = scale * window[i * block_size + j] * vec_input_f;
            }
        }
    }
    if (bytes_per_sample == 2) {
        for (int i = 0; i < 2 * radius + 1; i++) {
            for (int j = 0; j < block_size; j++) {
                auto vec_input = Vec16us().load((const uint16_t *) shifted_src + (i * offset_y + j) * offset_x);
                auto vec_input_f = to_float(Vec16i(extend(vec_input)));
                block[i * block_size * 2 + j] = scale * window[i * block_size + j] * vec_input_f;
            }
        }
    }
    if (bytes_per_sample == 4) {
        for (int i = 0; i < 2 * radius + 1; i++) {
            for (int j = 0; j < block_size; j++) {
                auto vec_input_f = Vec16f().load((const float *) shifted_src + (i * offset_y + j) * offset_x);
                block[i * block_size * 2 + j] = scale * window[i * block_size + j] * vec_input_f;
            }
        }
    }
}

static void store_block(
    float * VS_RESTRICT shifted_dst,
    const Vec16f * VS_RESTRICT shifted_block,
    int block_size,
    int block_step,
    int width,
    int height,
    const Vec16f * VS_RESTRICT shifted_window
) {

    assert(block_size == 16);
    block_size = 16; // unsafe

    for (int i = 0; i < block_size; i++) {
        Vec16f acc = Vec16f().load((const float *) shifted_dst + (i * calc_pad_size(width, block_size, block_step)));
        acc = mul_add(shifted_block[i], shifted_window[i], acc);
        acc.store((float *) shifted_dst + (i * calc_pad_size(width, block_size, block_step)));
    }
}

static void store_frame(
    uint8_t * VS_RESTRICT dst,
    const float * VS_RESTRICT shifted_src,
    int width,
    int height,
    int dst_stride,
    int src_stride,
    int bits_per_sample
) {

    float scale = 1.0f / (1 << (bits_per_sample - 8));
    if (bits_per_sample == 32) {
        scale = 255.0f;
    }

    int bytes_per_sample = (bits_per_sample + 7) / 8;
    int peak = (1 << bits_per_sample) - 1;

    if (bytes_per_sample == 1) {
        auto dstp = (uint8_t *) dst;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                auto clamped = std::clamp(static_cast<int>(shifted_src[y * src_stride + x] / scale + 0.5f), 0, peak);
                dstp[y * dst_stride + x] = static_cast<uint8_t>(clamped);
            }
        }
    }
    if (bytes_per_sample == 2) {
        auto dstp = (uint16_t *) dst;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                auto clamped = std::clamp(static_cast<int>(shifted_src[y * src_stride + x] / scale + 0.5f), 0, peak);
                dstp[y * dst_stride + x] = static_cast<uint16_t>(clamped);
            }
        }
    }
    if (bytes_per_sample == 4) {
        auto dstp = (float *) dst;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                dstp[y * dst_stride + x] = shifted_src[y * src_stride + x] / scale;
            }
        }
    }
}

struct DFTTestThreadData {
    uint8_t * padded; // shape: (pad_height, pad_width)
    float * padded2; // shape: (pad_height, pad_width)
};

struct DFTTestData {
    VSNodeRef * node;
    int radius;
    int block_size;
    int block_step;
    std::array<bool, 3> process;
    bool zero_mean;
    std::unique_ptr<Vec16f []> window;
    std::unique_ptr<Vec16f []> window_freq;
    std::unique_ptr<Vec16f []> sigma;
    int filter_type;
    float sigma2;
    float pmin;
    float pmax;

    std::atomic<int> num_uninitialized_threads;
    std::unordered_map<std::thread::id, DFTTestThreadData> thread_data;
    std::shared_mutex thread_data_lock;
};

static void VS_CC DFTTestInit(
    VSMap *in, VSMap *out, void **instanceData, VSNode *node,
    VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d = static_cast<const DFTTestData *>(*instanceData);

    auto vi = vsapi->getVideoInfo(d->node);
    vsapi->setVideoInfo(vi, 1, node);
}

static const VSFrameRef *VS_CC DFTTestGetFrame(
    int n, int activationReason, void **instanceData, void **frameData,
    VSFrameContext *frameCtx, VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d = static_cast<DFTTestData *>(*instanceData);

    if (activationReason == arInitial) {
        int start = std::max(n - d->radius, 0);
        auto vi = vsapi->getVideoInfo(d->node);
        int end = std::min(n + d->radius, vi->numFrames - 1);
        for (int i = start; i <= end; i++) {
            vsapi->requestFrameFilter(i, d->node, frameCtx);
        }
        return nullptr;
    } else if (activationReason != arAllFramesReady) {
        return nullptr;
    }

    auto mxcsr = get_control_word();
    no_subnormals();

    auto vi = vsapi->getVideoInfo(d->node);

    DFTTestThreadData thread_data;

    auto thread_id = std::this_thread::get_id();
    if (d->num_uninitialized_threads.load(std::memory_order::acquire) == 0) {
        const auto & const_data = d->thread_data;
        thread_data = const_data.at(thread_id);
    } else {
        bool initialized = true;

        d->thread_data_lock.lock_shared();
        try {
            const auto & const_data = d->thread_data;
            thread_data = const_data.at(thread_id);
        } catch (const std::out_of_range &) {
            initialized = false;
        }
        d->thread_data_lock.unlock_shared();

        if (!initialized) {
            auto padded_size = (
                (2 * d->radius + 1) *
                calc_pad_size(vi->height, d->block_size, d->block_step) *
                calc_pad_size(vi->width, d->block_size, d->block_step) *
                vi->format->bytesPerSample
            );

            thread_data.padded = static_cast<uint8_t *>(std::malloc(padded_size));
            thread_data.padded2 = static_cast<float *>(std::malloc(
                calc_pad_size(vi->height, d->block_size, d->block_step) *
                calc_pad_size(vi->width, d->block_size, d->block_step) *
                sizeof(float)
            ));

            {
                std::lock_guard _ { d->thread_data_lock };
                d->thread_data.emplace(thread_id, thread_data);
            }

            d->num_uninitialized_threads.fetch_sub(1, std::memory_order::release);
        }
    }

    std::vector<std::unique_ptr<const VSFrameRef, decltype(vsapi->freeFrame)>> src_frames;
    src_frames.reserve(2 * d->radius + 1);
    for (int i = n - d->radius; i <= n + d->radius; i++) {
        src_frames.emplace_back(
            vsapi->getFrameFilter(std::clamp(i, 0, vi->numFrames - 1), d->node, frameCtx),
            vsapi->freeFrame
        );
    }

    auto & src_center_frame = src_frames[d->radius];
    auto format = vsapi->getFrameFormat(src_center_frame.get());

    const VSFrameRef * fr[] {
        d->process[0] ? nullptr : src_center_frame.get(),
        d->process[1] ? nullptr : src_center_frame.get(),
        d->process[2] ? nullptr : src_center_frame.get()
    };
    const int pl[] { 0, 1, 2 };
    std::unique_ptr<VSFrameRef, decltype(vsapi->freeFrame)> dst_frame {
        vsapi->newVideoFrame2(format, vi->width, vi->height, fr, pl, src_center_frame.get(), core),
        vsapi->freeFrame
    };

    for (int plane = 0; plane < format->numPlanes; plane++) {
        if (!d->process[plane]) {
            continue;
        }

        int width = vsapi->getFrameWidth(src_center_frame.get(), plane);
        int height = vsapi->getFrameHeight(src_center_frame.get(), plane);
        int stride = vsapi->getStride(src_center_frame.get(), plane) / vi->format->bytesPerSample;

        int padded_size_spatial = (
            calc_pad_size(height, d->block_size, d->block_step) *
            calc_pad_size(width, d->block_size, d->block_step)
        );

        std::memset(thread_data.padded2, 0,
            calc_pad_size(height, d->block_size, d->block_step) *
            calc_pad_size(width, d->block_size, d->block_step) *
            sizeof(float)
        );

        for (int i = 0; i < 2 * d->radius + 1; i++) {
            auto srcp = vsapi->getReadPtr(src_frames[i].get(), plane);
            reflection_padding(
                &thread_data.padded[(i * padded_size_spatial) * vi->format->bytesPerSample],
                srcp,
                width, height, stride,
                d->block_size, d->block_step,
                vi->format->bytesPerSample
            );
        }

        for (int i = 0; i < calc_pad_num(height, d->block_size, d->block_step); i++) {
            for (int j = 0; j < calc_pad_num(width, d->block_size, d->block_step); j++) {
                assert(d->block_size == 16);
                constexpr int block_size = 16;

                Vec16f block[7 * block_size * 2];

                int offset_x = calc_pad_size(width, d->block_size, d->block_step);

                load_block(
                    block,
                    &thread_data.padded[(i * offset_x + j) * d->block_step * vi->format->bytesPerSample],
                    d->radius, d->block_size, d->block_step,
                    width, height,
                    d->window.get(), vi->format->bitsPerSample
                );

                fused(
                    block,
                    d->sigma.get(),
                    d->sigma2,
                    d->pmin,
                    d->pmax,
                    d->filter_type,
                    d->zero_mean,
                    d->window_freq.get(),
                    d->radius
                );

                store_block(
                    &thread_data.padded2[(i * offset_x + j) * d->block_step],
                    &block[d->radius * block_size * 2],
                    block_size,
                    d->block_step,
                    width,
                    height,
                    &d->window[d->radius * block_size * 2]
                );
            }
        }

        int pad_width = calc_pad_size(width, d->block_size, d->block_step);
        int pad_height = calc_pad_size(height, d->block_size, d->block_step);
        int offset_y = (pad_height - height) / 2;
        int offset_x = (pad_width - width) / 2;

        auto dstp = vsapi->getWritePtr(dst_frame.get(), plane);
        store_frame(
            dstp,
            &thread_data.padded2[(offset_y * pad_width + offset_x)],
            width,
            height,
            stride,
            pad_width,
            vi->format->bitsPerSample
        );
    }

    set_control_word(mxcsr);

    return dst_frame.release();
}

static void VS_CC DFTTestFree(
    void *instanceData, VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d = static_cast<const DFTTestData *>(instanceData);

    vsapi->freeNode(d->node);

    for (const auto & [_, thread_data] : d->thread_data) {
        std::free(thread_data.padded2);
        std::free(thread_data.padded);
    }

    delete d;
}

static void VS_CC DFTTestCreate(
    const VSMap *in, VSMap *out, void *userData,
    VSCore *core, const VSAPI *vsapi
) noexcept {

    auto d = std::make_unique<DFTTestData>();

    d->node = vsapi->propGetNode(in, "clip", 0, nullptr);

    auto set_error = [vsapi, out, &d](const char * error_message) -> void {
        vsapi->freeNode(d->node);
        vsapi->setError(out, error_message);
        return ;
    };

    auto vi = vsapi->getVideoInfo(d->node);
    if (!isConstantFormat(vi)) {
        return set_error("only constant format input is supported");
    }
    if (vi->format->sampleType == stInteger && vi->format->bytesPerSample > 2) {
        return set_error("only 8-16 bit integer format input is supported");
    }
    if (vi->format->sampleType == stFloat && vi->format->bitsPerSample != 32) {
        return set_error("only 32-bit float format input is supported");
    }

    int error;

    d->radius = int64ToIntS(vsapi->propGetInt(in, "radius", 0, &error));
    if (error) {
        d->radius = 0;
    }

    if (d->radius < 0 || d->radius > 3) {
        return set_error("\"radius\" must be in [0, 1, 2, 3]");
    }

    d->block_size = int64ToIntS(vsapi->propGetInt(in, "block_size", 0, &error));
    if (error) {
        d->block_size = 16;
    }

    if (d->block_size != 16) {
        return set_error("\"block_size\" must be 16");
    }

    d->block_step = int64ToIntS(vsapi->propGetInt(in, "block_step", 0, &error));
    if (error) {
        d->block_step = d->block_size;
    }

    int num_planes_args = vsapi->propNumElements(in, "planes");
    d->process.fill(num_planes_args <= 0);
    for (int i = 0; i < num_planes_args; ++i) {
        int plane = static_cast<int>(vsapi->propGetInt(in, "planes", i, nullptr));

        if (plane < 0 || plane >= vi->format->numPlanes) {
            return set_error("plane index out of range");
        }

        if (d->process[plane]) {
            return set_error("plane specified twice");
        }

        d->process[plane] = true;
    }

    d->window = std::make_unique<Vec16f []>((2 * d->radius + 1) * d->block_size * d->block_size / 16);
    {
        auto window = vsapi->propGetFloatArray(in, "window", nullptr);
        for (int i = 0; i < (2 * d->radius + 1) * d->block_size * d->block_size / 16; i++) {
            d->window[i] = Vec16f(to_float(Vec8d().load(&window[i * 16])), to_float(Vec8d().load(&window[i * 16 + 8])));
        }
    }

    d->sigma = std::make_unique<Vec16f []>((2 * d->radius + 1) * d->block_size * ((d->block_size / 2 + 1 + 15) / 16));
    {
        auto sigma = vsapi->propGetFloatArray(in, "sigma", nullptr);
        for (int i = 0; i < (2 * d->radius + 1) * d->block_size; i++) {
            float sigma_padded[16] {};
            for (int j = 0; j < d->block_size / 2 + 1; j++) {
                sigma_padded[j] = static_cast<float>(sigma[i * (d->block_size / 2 + 1) + j]);
            }
            d->sigma[i] = Vec16f().load(&sigma_padded[0]);
        }
    }

    d->sigma2 = static_cast<float>(vsapi->propGetFloat(in, "sigma2", 0, nullptr));
    d->pmin = static_cast<float>(vsapi->propGetFloat(in, "pmin", 0, nullptr));
    d->pmax = static_cast<float>(vsapi->propGetFloat(in, "pmax", 0, nullptr));

    d->filter_type = static_cast<int>(vsapi->propGetInt(in, "filter_type", 0, nullptr));

    d->zero_mean = !!vsapi->propGetInt(in, "zero_mean", 0, &error);
    if (error) {
        d->zero_mean = true;
    }
    if (d->zero_mean) {
        d->window_freq = std::make_unique<Vec16f []>((2 * d->radius + 1) * d->block_size * ((d->block_size / 2 + 1 + 15) / 16) * 2);
        auto window_freq = vsapi->propGetFloatArray(in, "window_freq", nullptr);
        for (int i = 0; i < (2 * d->radius + 1) * d->block_size; i++) {
            float sigma_padded[32] {};
            for (int j = 0; j < d->block_size / 2 + 1; j++) {
                sigma_padded[j] = static_cast<float>(window_freq[(i * (d->block_size / 2 + 1) + j) * 2]);
                sigma_padded[16 + j] = static_cast<float>(window_freq[(i * (d->block_size / 2 + 1) + j) * 2 + 1]);
            }
            d->window_freq[i * 2] = Vec16f().load(&sigma_padded[0]);
            d->window_freq[i * 2 + 1] = Vec16f().load(&sigma_padded[16]);
        }
    }

    VSCoreInfo info;
    vsapi->getCoreInfo2(core, &info);
    d->num_uninitialized_threads.store(info.numThreads, std::memory_order::relaxed);
    d->thread_data.reserve(info.numThreads);

    vsapi->createFilter(
        in, out, "DFTTest",
        DFTTestInit, DFTTestGetFrame, DFTTestFree,
        fmParallel, 0, d.release(), core
    );
}

static void VS_CC RDFT(
    const VSMap *in, VSMap *out, void *userData,
    VSCore *core, const VSAPI *vsapi
) noexcept {

    auto set_error = [vsapi, out](const char * error_message) -> void {
        vsapi->setError(out, error_message);
    };

    int ndim = vsapi->propNumElements(in, "shape");
    if (ndim != 1 && ndim != 2 && ndim != 3) {
        return set_error("\"shape\" must be an array of ints with 1, 2 or 3 values");
    }

    std::array<int, 3> shape {};
    {
        auto shape_array = vsapi->propGetIntArray(in, "shape", nullptr);
        for (int i = 0; i < ndim; i++) {
            shape[i] = int64ToIntS(shape_array[i]);
        }
    }

    int size = 1;
    for (int i = 0; i < ndim; i++) {
        size *= shape[i];
    }
    if (vsapi->propNumElements(in, "data") != size) {
        return set_error("cannot reshape array");
    }

    int complex_size = shape[ndim - 1] / 2 + 1;
    for (int i = 0; i < ndim - 1; i++) {
        complex_size *= shape[i];
    }

    auto input = vsapi->propGetFloatArray(in, "data", nullptr);

    auto output = std::make_unique<std::complex<double> []>(complex_size);

    if (ndim == 1) {
        dft(output.get(), input, size, 1);
        vsapi->propSetFloatArray(out, "ret", (const double *) output.get(), complex_size * 2);
    } else if (ndim == 2) {
        for (int i = 0; i < shape[0]; i++) {
            dft(&output[i * (shape[1] / 2 + 1)], &input[i * shape[1]], shape[1], 1);
        }

        auto output2 = std::make_unique<std::complex<double> []>(complex_size);

        for (int i = 0; i < shape[1] / 2 + 1; i++) {
            dft(&output2[i], &output[i], shape[0], shape[1] / 2 + 1);
        }

        vsapi->propSetFloatArray(out, "ret", (const double *) output2.get(), complex_size * 2);
    } else {
        for (int i = 0; i < shape[0] * shape[1]; i++) {
            dft(&output[i * (shape[2] / 2 + 1)], &input[i * shape[2]], shape[2], 1);
        }

        auto output2 = std::make_unique<std::complex<double> []>(complex_size);

        for (int i = 0; i < shape[0]; i++) {
            for (int j = 0; j < shape[2] / 2 + 1; j++) {
                dft(
                    &output2[i * shape[1] * (shape[2] / 2 + 1) + j],
                    &output[i * shape[1] * (shape[2] / 2 + 1) + j],
                    shape[1],
                    (shape[2] / 2 + 1)
                );
            }
        }

        for (int i = 0; i < shape[1] * (shape[2] / 2 + 1); i++) {
            dft(&output[i], &output2[i], shape[0], shape[1] * (shape[2] / 2 + 1));
        }

        vsapi->propSetFloatArray(out, "ret", (const double *) output.get(), complex_size * 2);
    }
}

static void Version(const VSMap *, VSMap * out, void *, VSCore *, const VSAPI *vsapi) {
    vsapi->propSetData(out, "version", VERSION, -1, paReplace);
}

VS_EXTERNAL_API(void)
VapourSynthPluginInit(VSConfigPlugin configFunc, VSRegisterFunction registerFunc, VSPlugin *plugin) {
    configFunc(
        "io.github.amusementclub.dfttest2_avx2",
        "dfttest2_avx2",
        "DFTTest2 (AVX2)",
        VAPOURSYNTH_API_VERSION, 1, plugin
    );

    registerFunc(
        "DFTTest",
        "clip:clip;"
        "window:float[];"
        "sigma:float[];"
        "sigma2:float;"
        "pmin:float;"
        "pmax:float;"
        "filter_type:int;"
        "radius:int:opt;"
        "block_size:int:opt;"
        "block_step:int:opt;"
        "zero_mean:int:opt;"
        "window_freq:float[]:opt;"
        "planes:int[]:opt;",
        DFTTestCreate, nullptr, plugin
    );

    registerFunc(
        "RDFT",
        "data:float[];"
        "shape:int[];",
        RDFT, nullptr, plugin
    );

    registerFunc(
        "Version",
        "",
        Version, nullptr, plugin
    );
}
