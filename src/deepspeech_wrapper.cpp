/* Copyright (C) 2021-2023 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "deepspeech_wrapper.hpp"

#include <dlfcn.h>

#include <algorithm>
#include <chrono>
#include <fstream>

#include "logger.hpp"

using namespace std::chrono_literals;

deepspeech_wrapper::deepspeech_wrapper(config_t config, callbacks_t call_backs)
    : engine_wrapper{std::move(config), std::move(call_backs)} {
    open_ds_lib();
    m_speech_buf.reserve(m_speech_max_size);
}

deepspeech_wrapper::~deepspeech_wrapper() {
    LOGD("ds dtor");

    stop();

    if (m_ds_api.ok()) {
        if (m_ds_stream) m_ds_api.STT_FreeStream(m_ds_stream);
        m_ds_stream = nullptr;
        if (m_ds_model) m_ds_api.STT_FreeModel(m_ds_model);
        m_ds_model = nullptr;
    }

    m_ds_api = {};

    if (m_dslib_handle) {
        dlclose(m_dslib_handle);
        m_dslib_handle = nullptr;
    }
}

void deepspeech_wrapper::open_ds_lib() {
    m_dslib_handle = dlopen("libstt.so", RTLD_LAZY);
    if (m_dslib_handle == nullptr) {
        LOGE("failed to open ds lib");
        throw std::runtime_error("failed to open ds lib");
    }

    m_ds_api.STT_CreateModel =
        reinterpret_cast<decltype(m_ds_api.STT_CreateModel)>(
            dlsym(m_dslib_handle, "STT_CreateModel"));
    m_ds_api.STT_FreeModel = reinterpret_cast<decltype(m_ds_api.STT_FreeModel)>(
        dlsym(m_dslib_handle, "STT_FreeModel"));
    m_ds_api.STT_EnableExternalScorer =
        reinterpret_cast<decltype(m_ds_api.STT_EnableExternalScorer)>(
            dlsym(m_dslib_handle, "STT_EnableExternalScorer"));
    m_ds_api.STT_CreateStream =
        reinterpret_cast<decltype(m_ds_api.STT_CreateStream)>(
            dlsym(m_dslib_handle, "STT_CreateStream"));
    m_ds_api.STT_FreeStream =
        reinterpret_cast<decltype(m_ds_api.STT_FreeStream)>(
            dlsym(m_dslib_handle, "STT_FreeStream"));
    m_ds_api.STT_FinishStream =
        reinterpret_cast<decltype(m_ds_api.STT_FinishStream)>(
            dlsym(m_dslib_handle, "STT_FinishStream"));
    m_ds_api.STT_IntermediateDecode =
        reinterpret_cast<decltype(m_ds_api.STT_IntermediateDecode)>(
            dlsym(m_dslib_handle, "STT_IntermediateDecode"));
    m_ds_api.STT_FeedAudioContent =
        reinterpret_cast<decltype(m_ds_api.STT_FeedAudioContent)>(
            dlsym(m_dslib_handle, "STT_FeedAudioContent"));
    m_ds_api.STT_FreeString =
        reinterpret_cast<decltype(m_ds_api.STT_FreeString)>(
            dlsym(m_dslib_handle, "STT_FreeString"));

    if (!m_ds_api.ok()) {
        LOGE("failed to register ds api");
        throw std::runtime_error("failed to register ds api");
    }
}

void deepspeech_wrapper::start_processing_impl() { create_ds_model(); }

void deepspeech_wrapper::create_ds_model() {
    if (m_ds_model) return;

    LOGD("creating ds model");

    auto status =
        m_ds_api.STT_CreateModel(m_model_file.first.c_str(), &m_ds_model);

    if (status != 0 || !m_ds_model) {
        LOGE("failed to create ds model");
        throw std::runtime_error("failed to create ds model");
    }

    if (!m_model_file.second.empty()) {
        m_ds_api.STT_EnableExternalScorer(m_ds_model,
                                          m_model_file.second.c_str());
    }

    LOGD("ds model created");
}

void deepspeech_wrapper::free_ds_stream() {
    if (m_ds_stream) {
        m_ds_api.STT_FreeStream(m_ds_stream);
        m_ds_stream = nullptr;
    }
}

void deepspeech_wrapper::create_ds_stream() {
    if (m_ds_stream || !m_ds_model) return;

    auto status = m_ds_api.STT_CreateStream(m_ds_model, &m_ds_stream);

    if (status != 0 || !m_ds_stream) {
        LOGD("failed to create ds stream");
        throw std::runtime_error("failed to create ds stream");
    }
}

void deepspeech_wrapper::reset_impl() {
    m_speech_buf.clear();
    free_ds_stream();
}

engine_wrapper::samples_process_result_t deepspeech_wrapper::process_buff() {
    if (!lock_buff_for_processing())
        return samples_process_result_t::wait_for_samples;

    auto eof = m_in_buf.eof;
    auto sof = m_in_buf.sof;

    LOGD("process samples buf: mode="
         << m_speech_mode << ", in-buf size=" << m_in_buf.size
         << ", speech-buf size=" << m_speech_buf.size() << ", sof=" << sof
         << ", eof=" << eof);

    if (sof) {
        m_speech_buf.clear();
        m_start_time.reset();
        m_vad.reset();

        free_ds_stream();
        create_ds_stream();
    }

    const auto& vad_buf =
        m_vad.remove_silence(m_in_buf.buf.data(), m_in_buf.size);

    m_in_buf.clear();

    bool vad_status = !vad_buf.empty();

    m_in_buf.clear();

    if (vad_status) {
        LOGD("vad: speech detected");

        if (m_speech_mode != speech_mode_t::manual)
            set_speech_detection_status(
                speech_detection_status_t::speech_detected);

        m_speech_buf.insert(m_speech_buf.end(), vad_buf.cbegin(),
                            vad_buf.cend());

        restart_sentence_timer();
    } else {
        LOGD("vad: no speech");

        if (m_speech_mode == speech_mode_t::single_sentence &&
            m_speech_buf.empty() &&
            (!m_intermediate_text || m_intermediate_text->empty()) &&
            sentence_timer_timed_out()) {
            LOGD("sentence timeout");
            m_call_backs.sentence_timeout();
        }
    }

    if (m_thread_exit_requested) {
        free_buf();
        return samples_process_result_t::no_samples_needed;
    }

    auto final_decode = [&] {
        if (eof) return true;
        if (m_speech_mode == speech_mode_t::single_sentence &&
            m_intermediate_text && !m_intermediate_text->empty() && !vad_status)
            return true;
        if (m_speech_mode == speech_mode_t::automatic && !vad_status)
            return true;
        return false;
    }();

    auto old_status = m_speech_detection_status;

    if (final_decode && m_speech_mode != speech_mode_t::automatic)
        set_speech_detection_status(speech_detection_status_t::decoding);

    LOGD("speech frame: samples=" << m_speech_buf.size()
                                  << ", final=" << final_decode);

    decode_speech(m_speech_buf, final_decode);

    m_speech_buf.clear();

    if (final_decode ||
        (m_speech_mode == speech_mode_t::manual && !m_speech_started))
        set_speech_detection_status(speech_detection_status_t::no_speech);
    else
        set_speech_detection_status(old_status);

    if (final_decode)
        flush(!eof && m_speech_mode == speech_mode_t::automatic
                  ? flush_t::regular
                  : flush_t::eof);

    free_buf();

    return samples_process_result_t::wait_for_samples;
}

void deepspeech_wrapper::decode_speech(const ds_buf_t& buf, bool eof) {
    if (!m_ds_stream && eof) return;

    LOGD("speech decoding started");

    create_ds_stream();

    m_ds_api.STT_FeedAudioContent(m_ds_stream, buf.data(), buf.size());

    auto* cstr = [=] {
        if (!eof) return m_ds_api.STT_IntermediateDecode(m_ds_stream);
        auto* cstr = m_ds_api.STT_FinishStream(m_ds_stream);
        m_ds_stream = nullptr;
        return cstr;
    }();

    std::string result{cstr};
    m_ds_api.STT_FreeString(cstr);

#ifdef DEBUG
    LOGD("speech decoded: text=" << result);
#else
    LOGD("speech decoded");
#endif

    if (!m_intermediate_text || m_intermediate_text != result)
        set_intermediate_text(result);
}
