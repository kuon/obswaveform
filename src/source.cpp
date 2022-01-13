/*
    Copyright (C) 2021 Devin Davila

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "waveform_config.hpp"
#include "math_funcs.hpp"
#include "source.hpp"
#include "settings.hpp"
#include <graphics/matrix4.h>
#include <vector>
#include <string>
#include <algorithm>
#include <limits>
#include "cpuinfo_x86.h"

#ifndef HAVE_OBS_PROP_ALPHA
#define obs_properties_add_color_alpha obs_properties_add_color
#endif

const float WAVSource::DB_MIN = 20.0f * std::log10(std::numeric_limits<float>::min());

static const auto CPU_INFO = cpu_features::GetX86Info();
const bool WAVSource::HAVE_AVX2 = CPU_INFO.features.avx2 && CPU_INFO.features.fma3;
const bool WAVSource::HAVE_AVX = CPU_INFO.features.avx && CPU_INFO.features.fma3;
const bool WAVSource::HAVE_SSE41 = CPU_INFO.features.sse4_1;
const bool WAVSource::HAVE_FMA3 = CPU_INFO.features.fma3;

static bool enum_callback(void *data, obs_source_t *src)
{
    if(obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) // filter sources without audio
        static_cast<std::vector<std::string>*>(data)->push_back(obs_source_get_name(src));
    return true;
}

static std::vector<std::string> enumerate_audio_sources()
{
    std::vector<std::string> ret;
    obs_enum_sources(&enum_callback, &ret);
    return ret;
}

static void update_audio_info(obs_audio_info *info)
{
    if(!obs_get_audio_info(info))
    {
        info->samples_per_sec = 44100;
        info->speakers = SPEAKERS_UNKNOWN;
    }
}

// Callbacks for obs_source_info structure
namespace callbacks {
    static const char *get_name([[maybe_unused]] void *data)
    {
        return T("source_name");
    }

    static void *create(obs_data_t *settings, obs_source_t *source)
    {
        if(WAVSource::HAVE_AVX2)
            return static_cast<void*>(new WAVSourceAVX2(settings, source));
        else if(WAVSource::HAVE_AVX)
            return static_cast<void*>(new WAVSourceAVX(settings, source));
        else
            return static_cast<void*>(new WAVSourceSSE2(settings, source));
    }

    static void destroy(void *data)
    {
        delete static_cast<WAVSource*>(data);
    }

    static uint32_t get_width(void *data)
    {
        return static_cast<WAVSource*>(data)->width();
    }

    static uint32_t get_height(void *data)
    {
        return static_cast<WAVSource*>(data)->height();
    }

    static void get_defaults(obs_data_t *settings)
    {
        obs_data_set_default_string(settings, P_AUDIO_SRC, P_NONE);
        obs_data_set_default_string(settings, P_DISPLAY_MODE, P_CURVE);
        obs_data_set_default_int(settings, P_WIDTH, 800);
        obs_data_set_default_int(settings, P_HEIGHT, 225);
        obs_data_set_default_bool(settings, P_LOG_SCALE, true);
        obs_data_set_default_string(settings, P_CHANNEL_MODE, P_MONO);
        obs_data_set_default_int(settings, P_FFT_SIZE, 2048);
        obs_data_set_default_bool(settings, P_AUTO_FFT_SIZE, false);
        obs_data_set_default_string(settings, P_WINDOW, P_HANN);
        obs_data_set_default_string(settings, P_INTERP_MODE, P_LANCZOS);
        obs_data_set_default_string(settings, P_FILTER_MODE, P_NONE);
        obs_data_set_default_double(settings, P_FILTER_RADIUS, 1.5);
        obs_data_set_default_string(settings, P_TSMOOTHING, P_EXPAVG);
        obs_data_set_default_double(settings, P_GRAVITY, 0.65);
        obs_data_set_default_bool(settings, P_FAST_PEAKS, false);
        obs_data_set_default_int(settings, P_CUTOFF_LOW, 30);
        obs_data_set_default_int(settings, P_CUTOFF_HIGH, 17500);
        obs_data_set_default_int(settings, P_FLOOR, -65);
        obs_data_set_default_int(settings, P_CEILING, 0);
        obs_data_set_default_double(settings, P_SLOPE, 0.0);
        obs_data_set_default_string(settings, P_RENDER_MODE, P_SOLID);
        obs_data_set_default_int(settings, P_COLOR_BASE, 0xffffffff);
        obs_data_set_default_int(settings, P_COLOR_CREST, 0xffffffff);
        obs_data_set_default_double(settings, P_GRAD_RATIO, 0.75);
        obs_data_set_default_int(settings, P_BAR_WIDTH, 24);
        obs_data_set_default_int(settings, P_BAR_GAP, 6);
        obs_data_set_default_int(settings, P_STEP_WIDTH, 8);
        obs_data_set_default_int(settings, P_STEP_GAP, 4);
    }

    static obs_properties_t *get_properties([[maybe_unused]] void *data)
    {
        auto props = obs_properties_create();

        // audio source
        auto srclist = obs_properties_add_list(props, P_AUDIO_SRC, T(P_AUDIO_SRC), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(srclist, T(P_NONE), P_NONE);

        for(const auto& str : enumerate_audio_sources())
            obs_property_list_add_string(srclist, str.c_str(), str.c_str());

        // display type
        auto displaylist = obs_properties_add_list(props, P_DISPLAY_MODE, T(P_DISPLAY_MODE), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(displaylist, T(P_CURVE), P_CURVE);
        obs_property_list_add_string(displaylist, T(P_BARS), P_BARS);
        obs_property_list_add_string(displaylist, T(P_STEP_BARS), P_STEP_BARS);
        obs_properties_add_int(props, P_BAR_WIDTH, T(P_BAR_WIDTH), 1, 256, 1);
        obs_properties_add_int(props, P_BAR_GAP, T(P_BAR_GAP), 0, 256, 1);
        obs_properties_add_int(props, P_STEP_WIDTH, T(P_STEP_WIDTH), 1, 256, 1);
        obs_properties_add_int(props, P_STEP_GAP, T(P_STEP_GAP), 0, 256, 1);
        obs_property_set_modified_callback(displaylist, [](obs_properties_t *props, [[maybe_unused]] obs_property_t *property, obs_data_t *settings) -> bool {
            auto disp = obs_data_get_string(settings, P_DISPLAY_MODE);
            auto bar = p_equ(disp, P_BARS);
            auto step = p_equ(disp, P_STEP_BARS);
            obs_property_set_enabled(obs_properties_get(props, P_BAR_WIDTH), bar || step);
            obs_property_set_enabled(obs_properties_get(props, P_BAR_GAP), bar || step);
            obs_property_set_visible(obs_properties_get(props, P_BAR_WIDTH), bar || step);
            obs_property_set_visible(obs_properties_get(props, P_BAR_GAP), bar || step);
            obs_property_set_enabled(obs_properties_get(props, P_STEP_WIDTH), step);
            obs_property_set_enabled(obs_properties_get(props, P_STEP_GAP), step);
            obs_property_set_visible(obs_properties_get(props, P_STEP_WIDTH), step);
            obs_property_set_visible(obs_properties_get(props, P_STEP_GAP), step);
            return true;
            });

        // video size
        obs_properties_add_int(props, P_WIDTH, T(P_WIDTH), 32, 3840, 1);
        obs_properties_add_int(props, P_HEIGHT, T(P_HEIGHT), 32, 2160, 1);

        // log scale
        obs_properties_add_bool(props, P_LOG_SCALE, T(P_LOG_SCALE));

        // channels
        auto chanlst = obs_properties_add_list(props, P_CHANNEL_MODE, T(P_CHANNEL_MODE), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(chanlst, T(P_MONO), P_MONO);
        obs_property_list_add_string(chanlst, T(P_STEREO), P_STEREO);
        obs_property_set_long_description(chanlst, T(P_CHAN_DESC));

        // fft size
        auto autofftsz = obs_properties_add_bool(props, P_AUTO_FFT_SIZE, T(P_AUTO_FFT_SIZE));
        auto fftsz = obs_properties_add_int_slider(props, P_FFT_SIZE, T(P_FFT_SIZE), 128, 4096, 64);
        obs_property_set_long_description(autofftsz, T(P_AUTO_FFT_DESC));
        obs_property_set_long_description(fftsz, T(P_FFT_DESC));
        obs_property_set_modified_callback(autofftsz, [](obs_properties_t *props, [[maybe_unused]] obs_property_t *property, obs_data_t *settings) -> bool {
                obs_property_set_enabled(obs_properties_get(props, P_FFT_SIZE), !obs_data_get_bool(settings, P_AUTO_FFT_SIZE));
                return true;
            });

        // fft window function
        auto wndlist = obs_properties_add_list(props, P_WINDOW, T(P_WINDOW), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(wndlist, T(P_NONE), P_NONE);
        obs_property_list_add_string(wndlist, T(P_HANN), P_HANN);
        obs_property_list_add_string(wndlist, T(P_HAMMING), P_HAMMING);
        obs_property_list_add_string(wndlist, T(P_BLACKMAN), P_BLACKMAN);
        obs_property_list_add_string(wndlist, T(P_BLACKMAN_HARRIS), P_BLACKMAN_HARRIS);
        obs_property_set_long_description(wndlist, T(P_WINDOW_DESC));

        // smoothing
        auto tsmoothlist = obs_properties_add_list(props, P_TSMOOTHING, T(P_TSMOOTHING), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(tsmoothlist, T(P_NONE), P_NONE);
        obs_property_list_add_string(tsmoothlist, T(P_EXPAVG), P_EXPAVG);
        auto grav = obs_properties_add_float_slider(props, P_GRAVITY, T(P_GRAVITY), 0.0, 1.0, 0.01);
        auto peaks = obs_properties_add_bool(props, P_FAST_PEAKS, T(P_FAST_PEAKS));
        obs_property_set_long_description(tsmoothlist, T(P_TEMPORAL_DESC));
        obs_property_set_long_description(grav, T(P_GRAVITY_DESC));
        obs_property_set_long_description(peaks, T(P_FAST_PEAKS_DESC));
        obs_property_set_modified_callback(tsmoothlist, [](obs_properties_t *props, [[maybe_unused]] obs_property_t *property, obs_data_t *settings) -> bool {
            auto enable = !p_equ(obs_data_get_string(settings, P_TSMOOTHING), P_NONE);
            obs_property_set_enabled(obs_properties_get(props, P_GRAVITY), enable);
            obs_property_set_enabled(obs_properties_get(props, P_FAST_PEAKS), enable);
            obs_property_set_visible(obs_properties_get(props, P_GRAVITY), enable);
            obs_property_set_visible(obs_properties_get(props, P_FAST_PEAKS), enable);
            return true;
            });

        // interpolation
        auto interplist = obs_properties_add_list(props, P_INTERP_MODE, T(P_INTERP_MODE), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(interplist, T(P_POINT), P_POINT);
        obs_property_list_add_string(interplist, T(P_LANCZOS), P_LANCZOS);
        obs_property_set_long_description(interplist, T(P_INTERP_DESC));

        // filter
        auto filterlist = obs_properties_add_list(props, P_FILTER_MODE, T(P_FILTER_MODE), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(filterlist, T(P_NONE), P_NONE);
        obs_property_list_add_string(filterlist, T(P_GAUSS), P_GAUSS);
        obs_properties_add_float_slider(props, P_FILTER_RADIUS, T(P_FILTER_RADIUS), 0.0, 32.0, 0.01);
        obs_property_set_long_description(filterlist, T(P_FILTER_DESC));
        obs_property_set_modified_callback(filterlist, [](obs_properties_t *props, [[maybe_unused]] obs_property_t *property, obs_data_t *settings) -> bool {
            auto enable = !p_equ(obs_data_get_string(settings, P_FILTER_MODE), P_NONE);
            obs_property_set_enabled(obs_properties_get(props, P_FILTER_RADIUS), enable);
            obs_property_set_visible(obs_properties_get(props, P_FILTER_RADIUS), enable);
            return true;
            });

        // display
        auto low_cut = obs_properties_add_int_slider(props, P_CUTOFF_LOW, T(P_CUTOFF_LOW), 0, 24000, 1);
        auto high_cut = obs_properties_add_int_slider(props, P_CUTOFF_HIGH, T(P_CUTOFF_HIGH), 0, 24000, 1);
        obs_property_int_set_suffix(low_cut, " Hz");
        obs_property_int_set_suffix(high_cut, " Hz");
        auto floor = obs_properties_add_int_slider(props, P_FLOOR, T(P_FLOOR), -120, 0, 1);
        auto ceiling = obs_properties_add_int_slider(props, P_CEILING, T(P_CEILING), -120, 0, 1);
        obs_property_int_set_suffix(floor, " dBFS");
        obs_property_int_set_suffix(ceiling, " dBFS");
        auto slope = obs_properties_add_float_slider(props, P_SLOPE, T(P_SLOPE), 0.0, 10.0, 0.01);
        obs_property_set_long_description(slope, T(P_SLOPE_DESC));
        auto renderlist = obs_properties_add_list(props, P_RENDER_MODE, T(P_RENDER_MODE), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(renderlist, T(P_LINE), P_LINE);
        obs_property_list_add_string(renderlist, T(P_SOLID), P_SOLID);
        obs_property_list_add_string(renderlist, T(P_GRADIENT), P_GRADIENT);
        obs_properties_add_color_alpha(props, P_COLOR_BASE, T(P_COLOR_BASE));
        obs_properties_add_color_alpha(props, P_COLOR_CREST, T(P_COLOR_CREST));
        obs_properties_add_float_slider(props, P_GRAD_RATIO, T(P_GRAD_RATIO), 0.0, 4.0, 0.01);
        obs_property_set_modified_callback(renderlist, [](obs_properties_t *props, [[maybe_unused]] obs_property_t *property, obs_data_t *settings) -> bool {
            auto enable = p_equ(obs_data_get_string(settings, P_RENDER_MODE), P_GRADIENT);
            obs_property_set_enabled(obs_properties_get(props, P_COLOR_CREST), enable);
            obs_property_set_enabled(obs_properties_get(props, P_GRAD_RATIO), enable);
            obs_property_set_visible(obs_properties_get(props, P_GRAD_RATIO), enable);
            return true;
            });

        return props;
    }

    static void update(void *data, obs_data_t *settings)
    {
        static_cast<WAVSource*>(data)->update(settings);
    }

    static void show(void *data)
    {
        static_cast<WAVSource*>(data)->show();
    }

    static void hide(void *data)
    {
        static_cast<WAVSource*>(data)->hide();
    }

    static void tick(void *data, float seconds)
    {
        static_cast<WAVSource*>(data)->tick(seconds);
    }

    static void render(void *data, gs_effect_t *effect)
    {
        static_cast<WAVSource*>(data)->render(effect);
    }

    static void capture_audio(void *data, obs_source_t *source, const audio_data *audio, bool muted)
    {
        static_cast<WAVSource*>(data)->capture_audio(source, audio, muted);
    }
}

void WAVSource::get_settings(obs_data_t *settings)
{
    auto src_name = obs_data_get_string(settings, P_AUDIO_SRC);
    m_width = (unsigned int)obs_data_get_int(settings, P_WIDTH);
    m_height = (unsigned int)obs_data_get_int(settings, P_HEIGHT);
    m_log_scale = obs_data_get_bool(settings, P_LOG_SCALE);
    m_stereo = p_equ(obs_data_get_string(settings, P_CHANNEL_MODE), P_STEREO);
    m_fft_size = (size_t)obs_data_get_int(settings, P_FFT_SIZE);
    m_auto_fft_size = obs_data_get_bool(settings, P_AUTO_FFT_SIZE);
    auto wnd = obs_data_get_string(settings, P_WINDOW);
    auto tsmoothing = obs_data_get_string(settings, P_TSMOOTHING);
    m_gravity = (float)obs_data_get_double(settings, P_GRAVITY);
    m_fast_peaks = obs_data_get_bool(settings, P_FAST_PEAKS);
    auto interp = obs_data_get_string(settings, P_INTERP_MODE);
    auto filtermode = obs_data_get_string(settings, P_FILTER_MODE);
    m_filter_radius = (float)obs_data_get_double(settings, P_FILTER_RADIUS);
    m_cutoff_low = (int)obs_data_get_int(settings, P_CUTOFF_LOW);
    m_cutoff_high = (int)obs_data_get_int(settings, P_CUTOFF_HIGH);
    m_floor = (int)obs_data_get_int(settings, P_FLOOR);
    m_ceiling = (int)obs_data_get_int(settings, P_CEILING);
    m_slope = (float)obs_data_get_double(settings, P_SLOPE);
    auto rendermode = obs_data_get_string(settings, P_RENDER_MODE);
    auto color_base = obs_data_get_int(settings, P_COLOR_BASE);
    auto color_crest = obs_data_get_int(settings, P_COLOR_CREST);
    m_grad_ratio = (float)obs_data_get_double(settings, P_GRAD_RATIO);
    auto display = obs_data_get_string(settings, P_DISPLAY_MODE);
    m_bar_width = (int)obs_data_get_int(settings, P_BAR_WIDTH);
    m_bar_gap = (int)obs_data_get_int(settings, P_BAR_GAP);
    m_step_width = (int)obs_data_get_int(settings, P_STEP_WIDTH);
    m_step_gap = (int)obs_data_get_int(settings, P_STEP_GAP);

    m_color_base = { (uint8_t)color_base / 255.0f, (uint8_t)(color_base >> 8) / 255.0f, (uint8_t)(color_base >> 16) / 255.0f, (uint8_t)(color_base >> 24) / 255.0f };
    m_color_crest = { (uint8_t)color_crest / 255.0f, (uint8_t)(color_crest >> 8) / 255.0f, (uint8_t)(color_crest >> 16) / 255.0f, (uint8_t)(color_crest >> 24) / 255.0f };

    if(m_fft_size < 128)
        m_fft_size = 128;
    else if(m_fft_size & 15)
        m_fft_size &= -16; // align to 64-byte multiple so that N/2 is AVX aligned

    if((m_cutoff_high - m_cutoff_low) < 1)
    {
        m_cutoff_high = 17500;
        m_cutoff_low = 120;
    }

    if((m_ceiling - m_floor) < 1)
    {
        m_ceiling = 0;
        m_floor = -120;
    }

    if(src_name != nullptr)
        m_audio_source_name = src_name;
    else
        m_audio_source_name.clear();

    if(p_equ(wnd, P_HANN))
        m_window_func = FFTWindow::HANN;
    else if(p_equ(wnd, P_HAMMING))
        m_window_func = FFTWindow::HAMMING;
    else if(p_equ(wnd, P_BLACKMAN))
        m_window_func = FFTWindow::BLACKMAN;
    else if(p_equ(wnd, P_BLACKMAN_HARRIS))
        m_window_func = FFTWindow::BLACKMAN_HARRIS;
    else
        m_window_func = FFTWindow::NONE;

    if(p_equ(interp, P_LANCZOS))
        m_interp_mode = InterpMode::LANCZOS;
    else
        m_interp_mode = InterpMode::POINT;

    if(p_equ(filtermode, P_GAUSS))
        m_filter_mode = FilterMode::GAUSS;
    else
        m_filter_mode = FilterMode::NONE;

    if(p_equ(tsmoothing, P_EXPAVG))
        m_tsmoothing = TSmoothingMode::EXPONENTIAL;
    else
        m_tsmoothing = TSmoothingMode::NONE;

    if(p_equ(rendermode, P_LINE))
        m_render_mode = RenderMode::LINE;
    else if(p_equ(rendermode, P_SOLID))
        m_render_mode = RenderMode::SOLID;
    else
        m_render_mode = RenderMode::GRADIENT;

    if(p_equ(display, P_BARS))
        m_display_mode = DisplayMode::BAR;
    else if(p_equ(display, P_STEP_BARS))
        m_display_mode = DisplayMode::STEPPED_BAR;
    else
        m_display_mode = DisplayMode::CURVE;
}

void WAVSource::recapture_audio()
{
    // release old capture
    release_audio_capture();

    // add new capture
    auto src_name = m_audio_source_name.c_str();
    auto asrc = obs_get_source_by_name(src_name);
    if(asrc != nullptr)
    {
        obs_source_add_audio_capture_callback(asrc, &callbacks::capture_audio, this);
        m_audio_source = obs_source_get_weak_source(asrc);
        obs_source_release(asrc);
    }
    else if(!p_equ(src_name, "none"))
    {
        if(m_retries++ == 0)
            blog(LOG_WARNING, "[" MODULE_NAME "]: Failed to get audio source: \"%s\"", src_name);
    }
}

void WAVSource::release_audio_capture()
{
    if(m_audio_source != nullptr)
    {
        auto src = obs_weak_source_get_source(m_audio_source);
        obs_weak_source_release(m_audio_source);
        m_audio_source = nullptr;
        if(src != nullptr)
        {
            obs_source_remove_audio_capture_callback(src, &callbacks::capture_audio, this);
            obs_source_release(src);
        }
    }

    // reset circular buffers
    for(auto& i : m_capturebufs)
    {
        i.end_pos = 0;
        i.start_pos = 0;
        i.size = 0;
    }
}

void WAVSource::free_fft()
{
    for(auto i = 0; i < 2; ++i)
    {
        m_decibels[i].reset();
        m_tsmooth_buf[i].reset();
    }

    m_fft_input.reset();
    m_fft_output.reset();
    m_window_coefficients.reset();
    m_slope_modifiers.reset();

    if(m_fft_plan != nullptr)
    {
        fftwf_destroy_plan(m_fft_plan);
        m_fft_plan = nullptr;
    }

    m_fft_size = 0;
}

void WAVSource::init_interp(unsigned int sz)
{
    const auto maxbin = (m_fft_size / 2) - 1;
    const auto sr = (float)m_audio_info.samples_per_sec;
    const auto lowbin = std::clamp((float)m_cutoff_low * m_fft_size / sr, 1.0f, (float)maxbin);
    const auto highbin = std::clamp((float)m_cutoff_high * m_fft_size / sr, 1.0f, (float)maxbin);

    m_interp_indices.resize(sz);
    if(m_log_scale)
    {
        for(auto i = 0u; i < sz; ++i)
            m_interp_indices[i] = log_interp(lowbin, highbin, (float)i / (float)(sz - 1));
    }
    else
    {
        for(auto i = 0u; i < sz; ++i)
            m_interp_indices[i] = lerp(lowbin, highbin, (float)i / (float)(sz - 1));
    }
}

WAVSource::WAVSource(obs_data_t *settings, obs_source_t *source)
{
    m_source = source;
    for(auto& i : m_capturebufs)
        circlebuf_init(&i);
    update(settings);
}

WAVSource::~WAVSource()
{
    std::lock_guard lock(m_mtx);
    release_audio_capture();
    free_fft();

    for(auto& i : m_capturebufs)
        circlebuf_free(&i);
}

unsigned int WAVSource::width()
{
    std::lock_guard lock(m_mtx);
    return m_width;
}

unsigned int WAVSource::height()
{
    std::lock_guard lock(m_mtx);
    return m_height;
}

void WAVSource::update(obs_data_t *settings)
{
    std::lock_guard lock(m_mtx);

    release_audio_capture();
    free_fft();
    get_settings(settings);

    // get current audio settings
    update_audio_info(&m_audio_info);
    m_capture_channels = std::min(get_audio_channels(m_audio_info.speakers), 2u);
    if(m_capture_channels == 0)
        blog(LOG_WARNING, "[" MODULE_NAME "]: Could not determine audio channel count");

    // calculate FFT size based on video FPS
    obs_video_info vinfo = {};
    if(obs_get_video_info(&vinfo))
        m_fps = double(vinfo.fps_num) / double(vinfo.fps_den);
    else
        m_fps = 60.0;
    if(m_auto_fft_size)
    {
        // align to 64-byte multiple so that N/2 is AVX aligned
        m_fft_size = size_t(m_audio_info.samples_per_sec / m_fps) & -16;
        if(m_fft_size < 128)
            m_fft_size = 128;
    }

    // alloc fftw buffers
    m_output_channels = ((m_capture_channels > 1) || m_stereo) ? 2u : 1u;
    for(auto i = 0u; i < m_output_channels; ++i)
    {
        auto count = m_fft_size / 2;
        m_decibels[i].reset(avx_alloc<float>(count));
        if(m_tsmoothing != TSmoothingMode::NONE)
            m_tsmooth_buf[i].reset(avx_alloc<float>(count));
        for(auto j = 0u; j < count; ++j)
        {
            m_decibels[i][j] = DB_MIN;
            if(m_tsmoothing != TSmoothingMode::NONE)
                m_tsmooth_buf[i][j] = 0;
        }
    }
    m_fft_input.reset(avx_alloc<float>(m_fft_size));
    m_fft_output.reset(avx_alloc<fftwf_complex>(m_fft_size));
    m_fft_plan = fftwf_plan_dft_r2c_1d((int)m_fft_size, m_fft_input.get(), m_fft_output.get(), FFTW_ESTIMATE);

    // window function
    if(m_window_func != FFTWindow::NONE)
    {
        // precompute window coefficients
        m_window_coefficients.reset(avx_alloc<float>(m_fft_size));
        const auto N = m_fft_size - 1;
        constexpr auto pi2 = 2 * (float)M_PI;
        constexpr auto pi4 = 4 * (float)M_PI;
        constexpr auto pi6 = 6 * (float)M_PI;
        switch(m_window_func)
        {
        case FFTWindow::HAMMING:
            for(size_t i = 0; i < m_fft_size; ++i)
                m_window_coefficients[i] = 0.53836f - (0.46164f * std::cos((pi2 * i) / N));
            break;

        case FFTWindow::BLACKMAN:
            for(size_t i = 0; i < m_fft_size; ++i)
                m_window_coefficients[i] = 0.42f - (0.5f * std::cos((pi2 * i) / N)) + (0.08f * std::cos((pi4 * i) / N));
            break;

        case FFTWindow::BLACKMAN_HARRIS:
            for(size_t i = 0; i < m_fft_size; ++i)
                m_window_coefficients[i] = 0.35875f - (0.48829f * std::cos((pi2 * i) / N)) + (0.14128f * std::cos((pi4 * i) / N)) - (0.01168f * std::cos((pi6 * i) / N));
            break;

        case FFTWindow::HANN:
        default:
            for(size_t i = 0; i < m_fft_size; ++i)
                m_window_coefficients[i] = 0.5f * (1 - std::cos((pi2 * i) / N));
            break;
        }
    }

    m_last_silent = false;
    m_show = true;
    m_retries = 0;
    m_next_retry = 0.0f;

    recapture_audio();
    for(auto& i : m_capturebufs)
    {
        auto bufsz = m_fft_size * sizeof(float);
        if(i.size < bufsz)
            circlebuf_push_back_zero(&i, bufsz - i.size);
    }

    // precomupte interpolated indices
    if(m_display_mode == DisplayMode::CURVE)
    {
        init_interp(m_width);
        for(auto& i : m_interp_bufs)
            i.resize(m_width);
    }
    else
    {
        const auto bar_stride = m_bar_width + m_bar_gap;
        m_num_bars = (int)(m_width / bar_stride);
        if(((int)m_width - (m_num_bars * bar_stride)) >= m_bar_width)
            ++m_num_bars;
        init_interp(m_num_bars + 1); // make extra band for last bar
        for(auto& i : m_interp_bufs)
            i.resize(m_num_bars);
    }

    // filter
    if(m_filter_mode == FilterMode::GAUSS)
        m_kernel = make_gauss_kernel(m_filter_radius);

    // slope
    const auto num_mods = m_fft_size / 2;
    const auto maxmod = (float)(num_mods - 1);
    m_slope_modifiers.reset(avx_alloc<float>(num_mods));
    for(size_t i = 0; i < num_mods; ++i)
        m_slope_modifiers[i] = log10(log_interp(10.0f, 10000.0f, ((float)i * m_slope) / maxmod));
}
void WAVSource::render([[maybe_unused]] gs_effect_t *effect)
{
    if(m_display_mode == DisplayMode::CURVE)
        render_curve(effect);
    else
        render_bars(effect);
}

void WAVSource::render_curve([[maybe_unused]] gs_effect_t *effect)
{
    std::lock_guard lock(m_mtx);
    if(m_last_silent)
        return;

    const auto num_verts = (size_t)((m_render_mode == RenderMode::LINE) ? m_width : (m_width + 2));
    auto vbdata = gs_vbdata_create();
    vbdata->num = num_verts;
    vbdata->points = (vec3*)bmalloc(num_verts * sizeof(vec3));
    vbdata->num_tex = 1;
    vbdata->tvarray = (gs_tvertarray*)bzalloc(sizeof(gs_tvertarray));
    vbdata->tvarray->width = 2;
    vbdata->tvarray->array = bmalloc(2 * num_verts * sizeof(float));
    gs_vertbuffer_t *vbuf = nullptr;

    auto filename = obs_module_file("gradient.effect");
    auto shader = gs_effect_create_from_file(filename, nullptr);
    bfree(filename);
    auto tech = gs_effect_get_technique(shader, (m_render_mode == RenderMode::GRADIENT) ? "Gradient" : "Solid");
    
    const auto center = (float)m_height / 2 + 0.5f;
    const auto right = (float)m_width + 0.5f;
    const auto bottom = (float)m_height + 0.5f;
    const auto dbrange = m_ceiling - m_floor;
    const auto cpos = m_stereo ? center : bottom;

    auto grad_center = gs_effect_get_param_by_name(shader, "grad_center");
    gs_effect_set_float(grad_center, cpos);
    auto color_base = gs_effect_get_param_by_name(shader, "color_base");
    gs_effect_set_vec4(color_base, &m_color_base);
    auto color_crest = gs_effect_get_param_by_name(shader, "color_crest");
    gs_effect_set_vec4(color_crest, &m_color_crest);

    // interpolation
    auto miny = cpos;
    for(auto channel = 0u; channel < (m_stereo ? 2u : 1u); ++channel)
    {
        if(m_interp_mode == InterpMode::LANCZOS)
            for(auto i = 0u; i < m_width; ++i)
                m_interp_bufs[channel][i] = lanczos_interp(m_interp_indices[i], 3.0f, m_fft_size / 2, m_decibels[channel].get());
        else
            for(auto i = 0u; i < m_width; ++i)
                m_interp_bufs[channel][i] = m_decibels[channel][(int)m_interp_indices[i]];

        if(m_filter_mode != FilterMode::NONE)
        {
            if(HAVE_SSE41)
                m_interp_bufs[channel] = apply_filter_sse41(m_interp_bufs[channel], m_kernel);
            else
                m_interp_bufs[channel] = apply_filter(m_interp_bufs[channel], m_kernel);
        }
        
        const auto step = (m_render_mode == RenderMode::LINE) ? 1 : 2;
        for(auto i = 0u; i < m_width; i += step)
        {
            auto val = lerp(0.5f, cpos, std::clamp(m_ceiling - m_interp_bufs[channel][i], 0.0f, (float)dbrange) / dbrange);
            if(val < miny)
                miny = val;
            m_interp_bufs[channel][i] = val;
        }
    }
    auto grad_height = gs_effect_get_param_by_name(shader, "grad_height");
    gs_effect_set_float(grad_height, (cpos - miny) * m_grad_ratio);

    gs_technique_begin(tech);
    gs_technique_begin_pass(tech, 0);

    for(auto channel = 0u; channel < (m_stereo ? 2u : 1u); ++channel)
    {
        auto vertpos = 0u;
        if(channel)
            vbdata = gs_vertexbuffer_get_data(vbuf);
        if(m_render_mode != RenderMode::LINE)
            vec3_set(&vbdata->points[vertpos++], -0.5, cpos, 0);

        for(auto i = 0u; i < m_width; ++i)
        {
            if((m_render_mode != RenderMode::LINE) && (i & 1))
            {
                vec3_set(&vbdata->points[vertpos++], (float)i + 0.5f, cpos, 0);
                continue;
            }

            auto val = m_interp_bufs[channel][i];
            if(channel == 0)
                vec3_set(&vbdata->points[vertpos++], (float)i + 0.5f, val, 0);
            else
                vec3_set(&vbdata->points[vertpos++], (float)i + 0.5f, bottom - val, 0);
        }

        if(m_render_mode != RenderMode::LINE)
            vec3_set(&vbdata->points[vertpos++], right, cpos, 0);

        if(channel)
            gs_vertexbuffer_flush(vbuf);
        else
        {
            vbuf = gs_vertexbuffer_create(vbdata, GS_DYNAMIC);
            gs_load_vertexbuffer(vbuf);
            gs_load_indexbuffer(nullptr);
        }
        gs_draw((m_render_mode != RenderMode::LINE) ? GS_TRISTRIP : GS_LINESTRIP, 0, (uint32_t)num_verts);
    }

    gs_vertexbuffer_destroy(vbuf);
    gs_technique_end_pass(tech);
    gs_technique_end(tech);

    gs_effect_destroy(shader);
}

void WAVSource::render_bars([[maybe_unused]] gs_effect_t *effect)
{
    std::lock_guard lock(m_mtx);
    if(m_last_silent)
        return;

    auto filename = obs_module_file("gradient.effect");
    auto shader = gs_effect_create_from_file(filename, nullptr);
    bfree(filename);
    auto tech = gs_effect_get_technique(shader, (m_render_mode == RenderMode::GRADIENT) ? "Gradient" : "Solid");

    const auto bar_stride = m_bar_width + m_bar_gap;
    const auto step_stride = m_step_width + m_step_gap;
    const auto center = (float)m_height / 2 + 0.5f;
    const auto bottom = (float)m_height + 0.5f;
    const auto dbrange = m_ceiling - m_floor;
    const auto cpos = m_stereo ? center : bottom;

    auto max_steps = (size_t)(cpos / step_stride);
    if(((int)cpos - (max_steps * step_stride)) >= m_step_width)
        ++max_steps;

    // vertex buffer
    auto num_verts = (size_t)(m_num_bars * 4);
    if(m_display_mode == DisplayMode::STEPPED_BAR)
        num_verts *= max_steps;
    auto vbdata = gs_vbdata_create();
    vbdata->num = num_verts;
    vbdata->points = (vec3*)bmalloc(num_verts * sizeof(vec3));
    vbdata->num_tex = 1;
    vbdata->tvarray = (gs_tvertarray*)bzalloc(sizeof(gs_tvertarray));
    vbdata->tvarray->width = 2;
    vbdata->tvarray->array = bmalloc(2 * num_verts * sizeof(float));
    gs_vertbuffer_t *vbuf = nullptr;

    // index buffer
    auto num_idx = m_num_bars * 6;
    if(m_display_mode == DisplayMode::STEPPED_BAR)
        num_idx *= (int)max_steps;
    auto idata = (uint16_t*)bmalloc(num_idx * sizeof(uint16_t));
    uint16_t vert = 0u;
    for(auto i = 0; i < num_idx; i += 6)
    {
        idata[i] = vert;
        idata[i + 1] = vert + 1;
        idata[i + 2] = vert + 2;
        idata[i + 3] = vert + 2;
        idata[i + 4] = vert + 1;
        idata[i + 5] = vert + 3;
        vert += 4;
    }
    auto ibuf = gs_indexbuffer_create(GS_UNSIGNED_SHORT, idata, num_idx, 0);

    auto grad_center = gs_effect_get_param_by_name(shader, "grad_center");
    gs_effect_set_float(grad_center, cpos);
    auto color_base = gs_effect_get_param_by_name(shader, "color_base");
    gs_effect_set_vec4(color_base, &m_color_base);
    auto color_crest = gs_effect_get_param_by_name(shader, "color_crest");
    gs_effect_set_vec4(color_crest, &m_color_crest);

    // interpolation
    auto miny = cpos;
    for(auto channel = 0u; channel < (m_stereo ? 2u : 1u); ++channel)
    {
        if(m_interp_mode == InterpMode::LANCZOS)
        {
            for(auto i = 0; i < m_num_bars; ++i)
            {
                auto pos = m_interp_indices[i];
                float sum = 0.0f;
                int count = 0;
                float stop = m_interp_indices[i + 1];
                do
                {
                    sum += lanczos_interp(pos, 3.0f, m_fft_size / 2, m_decibels[channel].get());
                    ++count;
                    pos += 1.0f;
                } while(pos < stop);
                m_interp_bufs[channel][i] = sum / (float)count;
            }
        }
        else
        {
            for(auto i = 0; i < m_num_bars; ++i)
            {
                auto pos = (int)m_interp_indices[i];
                float sum = 0.0f;
                int count = 0;
                int stop = (int)m_interp_indices[i + 1];
                do
                {
                    sum += m_decibels[channel][pos];
                    ++count;
                    ++pos;
                } while(pos < stop);
                m_interp_bufs[channel][i] = sum / (float)count;
            }
        }

        if(m_filter_mode != FilterMode::NONE)
        {
            if(HAVE_SSE41)
                m_interp_bufs[channel] = apply_filter_sse41(m_interp_bufs[channel], m_kernel);
            else
                m_interp_bufs[channel] = apply_filter(m_interp_bufs[channel], m_kernel);
        }

        for(auto i = 0; i < m_num_bars; ++i)
        {
            auto val = lerp(0.5f, cpos, std::clamp(m_ceiling - m_interp_bufs[channel][i], 0.0f, (float)dbrange) / dbrange);
            if(val < miny)
                miny = val;
            m_interp_bufs[channel][i] = val;
        }
    }
    auto grad_height = gs_effect_get_param_by_name(shader, "grad_height");
    gs_effect_set_float(grad_height, (cpos - miny) * m_grad_ratio);

    gs_technique_begin(tech);
    gs_technique_begin_pass(tech, 0);

    for(auto channel = 0u; channel < (m_stereo ? 2u : 1u); ++channel)
    {
        auto vertpos = 0u;
        if(channel)
            vbdata = gs_vertexbuffer_get_data(vbuf);

        for(auto i = 0; i < m_num_bars; ++i)
        {
            auto x1 = (float)(i * bar_stride) + 0.5f;
            auto x2 = x1 + m_bar_width;
            auto val = m_interp_bufs[channel][i];

            if(m_display_mode == DisplayMode::STEPPED_BAR)
            {
                for(auto j = 0; j < max_steps; ++j)
                {
                    auto y1 = (float)(j * step_stride);
                    auto y2 = y1 + m_step_width;
                    if((cpos - val) < y2)
                        break;
                    if(channel)
                    {
                        y1 = cpos + y1;
                        y2 = cpos + y2;
                    }
                    else
                    {
                        y1 = cpos - y1;
                        y2 = cpos - y2;
                    }
                    vec3_set(&vbdata->points[vertpos++], x1, y1, 0);
                    vec3_set(&vbdata->points[vertpos++], x2, y1, 0);
                    vec3_set(&vbdata->points[vertpos++], x1, y2, 0);
                    vec3_set(&vbdata->points[vertpos++], x2, y2, 0);
                }
            }
            else
            {
                if(channel)
                    val = bottom - val;
                vec3_set(&vbdata->points[vertpos++], x1, val, 0);
                vec3_set(&vbdata->points[vertpos++], x2, val, 0);
                vec3_set(&vbdata->points[vertpos++], x1, cpos, 0);
                vec3_set(&vbdata->points[vertpos++], x2, cpos, 0);
            }
        }

        if(channel)
            gs_vertexbuffer_flush(vbuf);
        else
        {
            vbuf = gs_vertexbuffer_create(vbdata, GS_DYNAMIC);
            gs_load_vertexbuffer(vbuf);
            gs_load_indexbuffer(ibuf);
        }

        auto total_verts = (uint32_t)(vertpos / 4u) * 6u;
        if(total_verts > 0)
            gs_draw(GS_TRIS, 0, total_verts);
    }

    gs_vertexbuffer_destroy(vbuf);
    gs_indexbuffer_destroy(ibuf);
    gs_technique_end_pass(tech);
    gs_technique_end(tech);

    gs_effect_destroy(shader);
}

void WAVSource::show()
{
    std::lock_guard lock(m_mtx);
    m_show = true;
}

void WAVSource::hide()
{
    std::lock_guard lock(m_mtx);
    m_show = false;
}

void WAVSource::register_source()
{
    std::string arch;
    if(HAVE_AVX2)
        arch += " AVX2";
    if(HAVE_AVX)
        arch += " AVX";
    if(HAVE_SSE41)
        arch += " SSE4.1";
    if(HAVE_FMA3)
        arch += " FMA3";
    arch += " SSE2";
#if defined(__x86_64__) || defined(_M_X64)
    blog(LOG_INFO, "[" MODULE_NAME "]: Registered v%s 64-bit", VERSION_STRING);
#elif defined(__i386__) || defined(_M_IX86)
    blog(LOG_INFO, "[" MODULE_NAME "]: Registered v%s 32-bit", VERSION_STRING);
#else
    blog(LOG_INFO, "[" MODULE_NAME "]: Registered v%s Unknown Arch", VERSION_STRING);
#endif
    blog(LOG_INFO, "[" MODULE_NAME "]: Using CPU capabilities:%s", arch.c_str());

    obs_source_info info{};
    info.id = MODULE_NAME "_source";
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
    info.get_name = &callbacks::get_name;
    info.create = &callbacks::create;
    info.destroy = &callbacks::destroy;
    info.get_width = &callbacks::get_width;
    info.get_height = &callbacks::get_height;
    info.get_defaults = &callbacks::get_defaults;
    info.get_properties = &callbacks::get_properties;
    info.update = &callbacks::update;
    info.show = &callbacks::show;
    info.hide = &callbacks::hide;
    info.video_tick = &callbacks::tick;
    info.video_render = &callbacks::render;
    info.icon_type = OBS_ICON_TYPE_AUDIO_OUTPUT;

    obs_register_source(&info);
}

void WAVSource::capture_audio([[maybe_unused]] obs_source_t *source, const audio_data *audio, bool muted)
{
    if(!m_mtx.try_lock_for(std::chrono::milliseconds(10)))
        return;
    std::lock_guard lock(m_mtx, std::adopt_lock);
    if(m_audio_source == nullptr)
        return;

    auto sz = size_t(audio->frames * sizeof(float));
    for(auto i = 0u; i < m_capture_channels; ++i)
    {
        if(muted)
            circlebuf_push_back_zero(&m_capturebufs[i], sz);
        else
            circlebuf_push_back(&m_capturebufs[i], audio->data[i], sz);

        auto total = m_capturebufs[i].size;
        auto max = m_fft_size * sizeof(float) * 2;
        if(total > max)
            circlebuf_pop_front(&m_capturebufs[i], nullptr, total - max);
    }
}
