#include <spdlog/spdlog.h>

#include <SoapySDR/Registry.hpp>
#include <SoapySDR/Formats.hpp>

#include "soapy_rfnm.h"
#include <librfnm/librfnm.h>

static uint16_t librfnm_rx_chan_flags[MAX_RX_CHAN_COUNT] = {
    LIBRFNM_CH0,
    LIBRFNM_CH1,
    LIBRFNM_CH2,
    LIBRFNM_CH3,
};

static uint16_t librfnm_rx_chan_apply[MAX_RX_CHAN_COUNT] = {
    LIBRFNM_APPLY_CH0_RX,
    LIBRFNM_APPLY_CH1_RX,
    LIBRFNM_APPLY_CH2_RX,
    LIBRFNM_APPLY_CH3_RX
};

SoapyRFNM::SoapyRFNM(const SoapySDR::Kwargs& args) {
    spdlog::info("RFNMDevice::RFNMDevice()");

    if (args.count("serial") != 0) {
        lrfnm = new librfnm(LIBRFNM_TRANSPORT_USB, (std::string)args.at("serial"));
    }
    else {
        lrfnm = new librfnm(LIBRFNM_TRANSPORT_USB);
    }

    if (!lrfnm->s->transport_status.theoretical_mbps) {
        throw std::runtime_error("Couldn't open the RFNM USB device handle");
    }

    rx_chan_count = lrfnm->s->hwinfo.daughterboard[0].rx_ch_cnt +
                    lrfnm->s->hwinfo.daughterboard[1].rx_ch_cnt;

    if (rx_chan_count > MAX_RX_CHAN_COUNT) {
        // Should never happen
        spdlog::error("Invalid channel count reported by hardware");
        rx_chan_count = MAX_RX_CHAN_COUNT;
    }

    // sane defaults
    uint16_t apply_mask = 0;
    for (size_t i = 0; i < rx_chan_count; i++) {
        lrfnm->s->rx.ch[i].enable = RFNM_CH_OFF;
        lrfnm->s->rx.ch[i].stream = RFNM_CH_STREAM_AUTO;
        lrfnm->s->rx.ch[i].freq = RFNM_MHZ_TO_HZ(2450);
        lrfnm->s->rx.ch[i].path = lrfnm->s->rx.ch[0].path_preferred;
        lrfnm->s->rx.ch[i].samp_freq_div_n = 1;
        lrfnm->s->rx.ch[i].gain = 0;
        lrfnm->s->rx.ch[i].rfic_lpf_bw = 80;
        apply_mask |= librfnm_rx_chan_apply[i];
    }
    setRFNM(apply_mask);

    //s->tx.ch[0].freq = RFNM_MHZ_TO_HZ(2450);
    //s->tx.ch[0].path = s->tx.ch[0].path_preferred;
    //s->tx.ch[0].samp_freq_div_n = 2;

}

SoapyRFNM::~SoapyRFNM() {
    spdlog::info("RFNMDevice::~RFNMDevice()");
    delete lrfnm;

    for (size_t i = 0; i < rx_chan_count; i++) {
        free(partial_rx_buf[i].buf);
    }

    for (int i = 0; i < SOAPY_RFNM_BUFCNT; i++) {
        free(rxbuf[i].buf);
    }
}

std::string SoapyRFNM::getDriverKey() const {
    spdlog::info("RFNMDevice::getDriverKey()");
    return { "RFNM" };
}

std::string SoapyRFNM::getHardwareKey() const {
    spdlog::info("RFNMDevice::getHardwareKey()");
    return { "RFNM" };
}

SoapySDR::Kwargs SoapyRFNM::getHardwareInfo() const {
    spdlog::info("RFNMDevice::getHardwareInfo()");
    return {};
}

size_t SoapyRFNM::getStreamMTU(SoapySDR::Stream* stream) const {
    return RFNM_USB_RX_PACKET_ELEM_CNT * 16;
}

size_t SoapyRFNM::getNumChannels(const int direction) const {
    switch (direction) {
    case SOAPY_SDR_TX:
        return 0; // not yet implemented
    case SOAPY_SDR_RX:
        return rx_chan_count;
    default:
        return 0;
    }
}

std::vector<double> SoapyRFNM::listSampleRates(const int direction, const size_t channel) const {
    std::vector<double> rates;

    if (direction == SOAPY_SDR_RX) {
        rates.push_back(lrfnm->s->hwinfo.clock.dcs_clk);
        rates.push_back(lrfnm->s->hwinfo.clock.dcs_clk / 2);
    }

    return rates;
}

double SoapyRFNM::getSampleRate(const int direction, const size_t channel) const {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        return lrfnm->s->hwinfo.clock.dcs_clk / lrfnm->s->rx.ch[channel].samp_freq_div_n;
    } else {
        return 0;
    }
}

void SoapyRFNM::setSampleRate(const int direction, const size_t channel, const double rate) {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        if (rate == lrfnm->s->hwinfo.clock.dcs_clk) {
            lrfnm->s->rx.ch[channel].samp_freq_div_n = 1;
        } else if (rate == lrfnm->s->hwinfo.clock.dcs_clk / 2) {
            lrfnm->s->rx.ch[channel].samp_freq_div_n = 2;
        } else {
            throw std::runtime_error("unsupported sample rate");
        }
        setRFNM(librfnm_rx_chan_apply[channel]);
    }
}

std::string SoapyRFNM::getNativeStreamFormat(const int direction, const size_t /*channel*/, double& fullScale) const {
    fullScale = 32768;
    return SOAPY_SDR_CS16;
}

std::vector<std::string> SoapyRFNM::getStreamFormats(const int direction, const size_t channel) const {
    std::vector<std::string> formats;
    formats.push_back(SOAPY_SDR_CS16);
    formats.push_back(SOAPY_SDR_CF32);
    formats.push_back(SOAPY_SDR_CS8);
    return formats;
}

template <class T>
static void measQuadDcOffset(const T *buf, size_t n, T *offsets, float filter_coeff) {
    assert((n & 0x7) == 0);

    float accum[8] = {};

    for (size_t i = 0; i < n; i += 8) {
        #pragma GCC unroll 8
        for (size_t j = 0; j < 8; j++) {
            accum[j] += buf[i+j];
        }
    }

    float f = 8.0f / n;
    for (size_t j = 0; j < 8; j++) {
        accum[j] *= f;
        offsets[j] = accum[j] * filter_coeff + offsets[j] * (1.0f - filter_coeff);
    }
}

template <class T>
static void applyQuadDcOffset(T *buf, size_t n, const T *offsets) {
    assert((n & 0x7) == 0);

    for (size_t i = 0; i < n; i += 8) {
        #pragma GCC unroll 8
        for (size_t j = 0; j < 8; j++) {
            buf[i+j] -= offsets[j];
        }
    }
}

int SoapyRFNM::activateStream(SoapySDR::Stream* stream, const int flags, const long long timeNs,
        const size_t numElems) {
    spdlog::info("RFNMDevice::activateStream()");

    for (size_t channel = 0; channel < MAX_RX_CHAN_COUNT; channel++) {
        if (lrfnm->s->rx.ch[channel].enable != RFNM_CH_ON) {
            continue;
        }

        // First sample can sometimes take a while to come, so fetch it here before normal streaming
        // This first chunk is also useful for initial calibration
        struct librfnm_rx_buf* lrxbuf;
        if (lrfnm->rx_dqbuf(&lrxbuf, librfnm_rx_chan_flags[channel], 250)) {
            throw std::runtime_error("timeout activating stream");
        }

        std::memcpy(partial_rx_buf[channel].buf, lrxbuf->buf, outbufsize);
        partial_rx_buf[channel].left = outbufsize;
        partial_rx_buf[channel].offset = 0;
        lrfnm->rx_qbuf(lrxbuf);

        // Compute initial DC offsets
        switch (lrfnm->s->transport_status.rx_stream_format) {
        case LIBRFNM_STREAM_FORMAT_CS8:
            measQuadDcOffset(reinterpret_cast<int8_t *>(partial_rx_buf[channel].buf),
                    outbufsize, dc_offsets[channel].i8, 1.0f);
            break;
        case LIBRFNM_STREAM_FORMAT_CS16:
            measQuadDcOffset(reinterpret_cast<int16_t *>(partial_rx_buf[channel].buf),
                    outbufsize / 2, dc_offsets[channel].i16, 1.0f);
            break;
        case LIBRFNM_STREAM_FORMAT_CF32:
            measQuadDcOffset(reinterpret_cast<float *>(partial_rx_buf[channel].buf),
                    outbufsize / 4, dc_offsets[channel].f32, 1.0f);
            break;
        }

        // Apply DC correction on first chunk if requested
        if (dc_correction[channel]) {
            switch (lrfnm->s->transport_status.rx_stream_format) {
            case LIBRFNM_STREAM_FORMAT_CS8:
                applyQuadDcOffset(reinterpret_cast<int8_t *>(partial_rx_buf[channel].buf),
                        outbufsize, dc_offsets[channel].i8);
                break;
            case LIBRFNM_STREAM_FORMAT_CS16:
                applyQuadDcOffset(reinterpret_cast<int16_t *>(partial_rx_buf[channel].buf),
                        outbufsize / 2, dc_offsets[channel].i16);
                break;
            case LIBRFNM_STREAM_FORMAT_CF32:
                applyQuadDcOffset(reinterpret_cast<float *>(partial_rx_buf[channel].buf),
                        outbufsize / 4, dc_offsets[channel].f32);
                break;
            }
        }
    }

    return 0;
}

int SoapyRFNM::deactivateStream(SoapySDR::Stream* stream, const int flags0, const long long int timeNs) {
    spdlog::info("RFNMDevice::deactivateStream()");

    return 0;
}

std::vector<std::string> SoapyRFNM::listFrequencies(const int direction, const size_t channel) const {
    std::vector<std::string> names;
    names.push_back("RF");
    return names;
}

SoapySDR::RangeList SoapyRFNM::getFrequencyRange(const int direction, const size_t channel, const std::string &name) const {
    SoapySDR::RangeList results;

    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        results.push_back(SoapySDR::Range(
                    lrfnm->s->rx.ch[channel].freq_min,
                    lrfnm->s->rx.ch[channel].freq_max));
    }

    return results;
}

double SoapyRFNM::getFrequency(const int direction, const size_t channel, const std::string &name) const {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        return lrfnm->s->rx.ch[channel].freq;
    } else {
        return 0;
    }
}

void SoapyRFNM::setFrequency(const int direction, const size_t channel, const std::string &name,
        const double frequency, const SoapySDR::Kwargs& args) {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        lrfnm->s->rx.ch[channel].freq = frequency;
        setRFNM(librfnm_rx_chan_apply[channel]);
    }
}

std::vector<std::string> SoapyRFNM::listGains(const int direction, const size_t channel) const {
    std::vector<std::string> names;
    names.push_back("RF");
    return names;
}

double SoapyRFNM::getGain(const int direction, const size_t channel, const std::string &name) const {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        return lrfnm->s->rx.ch[channel].gain;
    } else {
        return 0;
    }
}

void SoapyRFNM::setGain(const int direction, const size_t channel, const std::string &name, const double value) {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        lrfnm->s->rx.ch[channel].gain = value;
        setRFNM(librfnm_rx_chan_apply[channel]);
    }
}

SoapySDR::Range SoapyRFNM::getGainRange(const int direction, const size_t channel, const std::string &name) const {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        return SoapySDR::Range(
                lrfnm->s->rx.ch[channel].gain_range.min,
                lrfnm->s->rx.ch[channel].gain_range.max);
    } else {
        return SoapySDR::Range(0, 0);
    }
}

double SoapyRFNM::getBandwidth(const int direction, const size_t channel) const {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        return lrfnm->s->rx.ch[channel].rfic_lpf_bw * 1e6;
    } else {
        return 0;
    }
}

void SoapyRFNM::setBandwidth(const int direction, const size_t channel, const double bw) {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        if (bw == 0.0) return; // special ignore value

        lrfnm->s->rx.ch[channel].rfic_lpf_bw = bw / 1e6;
        setRFNM(librfnm_rx_chan_apply[channel]);
    }
}

SoapySDR::RangeList SoapyRFNM::getBandwidthRange(const int direction, const size_t channel) const {
    SoapySDR::RangeList bws;
    bws.push_back(SoapySDR::Range(1e6, 100e6));
    return bws;
}

std::vector<std::string> SoapyRFNM::listAntennas(const int direction, const size_t channel) const {
    std::vector<std::string> ants;
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        for (int a = 0; a < 10; a++) {
            if (lrfnm->s->rx.ch[channel].path_possible[a] == RFNM_PATH_NULL) {
                break;
            }
            ants.push_back(librfnm::rf_path_to_string(lrfnm->s->rx.ch[channel].path_possible[a]));
        }
    }
    else if (direction == SOAPY_SDR_TX) {
        //   ants.push_back("TXH");
        //    ants.push_back("TXW");
    }
    return ants;
}

std::string SoapyRFNM::getAntenna(const int direction, const size_t channel) const {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        return librfnm::rf_path_to_string(lrfnm->s->rx.ch[channel].path);
    } else {
        return "";
    }
}

void SoapyRFNM::setAntenna(const int direction, const size_t channel, const std::string& name) {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        lrfnm->s->rx.ch[channel].path = librfnm::string_to_rf_path(name);
        setRFNM(librfnm_rx_chan_apply[channel]);
    }
}

SoapySDR::Stream* SoapyRFNM::setupStream(const int direction, const std::string& format,
        const std::vector<size_t>& channels, const SoapySDR::Kwargs& args) {
    if (direction != SOAPY_SDR_RX) {
        return nullptr;
    }

    if (stream_setup) {
        throw std::runtime_error("multiple streams unsupported");
    }

    // bounds check channels before we start the stream
    for (size_t channel : channels) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }
    }

    enum librfnm_stream_format stream_format;
    bool alloc_buffers = true;

    if (!format.compare(SOAPY_SDR_CF32)) {
        stream_format = LIBRFNM_STREAM_FORMAT_CF32;
    } else if (!format.compare(SOAPY_SDR_CS16)) {
        stream_format = LIBRFNM_STREAM_FORMAT_CS16;
    } else if (!format.compare(SOAPY_SDR_CS8)) {
        stream_format = LIBRFNM_STREAM_FORMAT_CS8;
    } else {
        throw std::runtime_error("setupStream invalid format " + format);
    }

    if (lrfnm->s->transport_status.rx_stream_format) {
        if (stream_format != lrfnm->s->transport_status.rx_stream_format) {
            throw std::runtime_error("changing stream format is unsupported");
        }
        alloc_buffers = false;
    }

    lrfnm->rx_stream(stream_format, &outbufsize);

    if (alloc_buffers) {
        for (int i = 0; i < SOAPY_RFNM_BUFCNT; i++) {
            rxbuf[i].buf = (uint8_t*)malloc(outbufsize);
            lrfnm->rx_qbuf(&rxbuf[i]);
            //txbuf[i].buf = rxbuf[i].buf;
            //txbuf[i].buf = (uint8_t*)malloc(inbufsize);
        }

        for (size_t channel = 0; channel < rx_chan_count; channel++) {
            partial_rx_buf[channel].buf = (uint8_t*)malloc(outbufsize);
        }
    }

    // flush old junk before streaming new data
    lrfnm->rx_flush(20);

    uint16_t apply_mask = 0;
    for (size_t channel : channels) {
        lrfnm->s->rx.ch[channel].enable = RFNM_CH_ON;
        apply_mask |= librfnm_rx_chan_apply[channel];
    }
    setRFNM(apply_mask);

    stream_setup = true;

    return (SoapySDR::Stream*)this;
}

void SoapyRFNM::closeStream(SoapySDR::Stream* stream) {
    spdlog::info("RFNMDevice::closeStream() -> Closing stream");

    // stop the receiver threads
    lrfnm->rx_stream_stop();

    // stop the ADCs
    uint16_t apply_mask = 0;
    for (size_t i = 0; i < rx_chan_count; i++) {
        if (lrfnm->s->rx.ch[i].enable != RFNM_CH_OFF) {
            lrfnm->s->rx.ch[i].enable = RFNM_CH_OFF;
            apply_mask |= librfnm_rx_chan_apply[i];
        }
    }
    setRFNM(apply_mask);

    // flush buffers
    lrfnm->rx_flush(0);

    stream_setup = false;
}

int SoapyRFNM::readStream(SoapySDR::Stream* stream, void* const* buffs, const size_t numElems, int& flags,
        long long int& timeNs, const long timeoutUs) {
    auto timeout = std::chrono::system_clock::now() + std::chrono::microseconds(timeoutUs);
    size_t bytes_per_ele = lrfnm->s->transport_status.rx_stream_format;
    struct librfnm_rx_buf* lrxbuf;
    size_t read_elems = 0;
    size_t buf_idx = 0;

    // TODO: keep usb_cc of each channel in sync

    for (size_t channel = 0; channel < MAX_RX_CHAN_COUNT; channel++) {
        if (lrfnm->s->rx.ch[channel].enable != RFNM_CH_ON) {
            continue;
        }

        read_elems = 0;

        if (partial_rx_buf[channel].left) {
            size_t can_write_bytes = numElems * bytes_per_ele;
            if (can_write_bytes > partial_rx_buf[channel].left) {
                can_write_bytes = partial_rx_buf[channel].left;
            }

            std::memcpy(((uint8_t*)buffs[buf_idx]), partial_rx_buf[channel].buf + partial_rx_buf[channel].offset, can_write_bytes);
            read_elems += (can_write_bytes / bytes_per_ele);

            partial_rx_buf[channel].left -= can_write_bytes;
            partial_rx_buf[channel].offset += can_write_bytes;
        }

        while (read_elems < numElems) {
            uint32_t wait_ms = 0;
            size_t overflowing_by_elems = 0;
            size_t can_copy_bytes = outbufsize;

            if (timeoutUs > 0) {
                auto time_remaining = timeout - std::chrono::system_clock::now();
                if (time_remaining > std::chrono::duration<int64_t>::zero()) {
                    wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(time_remaining).count();
                }
            }

            if (lrfnm->rx_dqbuf(&lrxbuf, librfnm_rx_chan_flags[channel], wait_ms)) {
                if (timeoutUs >= 10000) {
                    spdlog::info("read timeout, got {} of {} within {} us", read_elems, numElems, timeoutUs);
                }
                break;
            }

            if (dc_correction[channel]) {
                // periodically recalibrate DC offset to account for drift
                if ((lrxbuf->usb_cc & 0xF) == 0) {
                    switch (lrfnm->s->transport_status.rx_stream_format) {
                    case LIBRFNM_STREAM_FORMAT_CS8:
                        measQuadDcOffset(reinterpret_cast<int8_t *>(lrxbuf->buf), outbufsize, dc_offsets[channel].i8, 0.1f);
                        break;
                    case LIBRFNM_STREAM_FORMAT_CS16:
                        measQuadDcOffset(reinterpret_cast<int16_t *>(lrxbuf->buf), outbufsize / 2, dc_offsets[channel].i16, 0.1f);
                        break;
                    case LIBRFNM_STREAM_FORMAT_CF32:
                        measQuadDcOffset(reinterpret_cast<float *>(lrxbuf->buf), outbufsize / 4, dc_offsets[channel].f32, 0.1f);
                        break;
                    }
                }

                switch (lrfnm->s->transport_status.rx_stream_format) {
                case LIBRFNM_STREAM_FORMAT_CS8:
                    applyQuadDcOffset(reinterpret_cast<int8_t *>(lrxbuf->buf), outbufsize, dc_offsets[channel].i8);
                    break;
                case LIBRFNM_STREAM_FORMAT_CS16:
                    applyQuadDcOffset(reinterpret_cast<int16_t *>(lrxbuf->buf), outbufsize / 2, dc_offsets[channel].i16);
                    break;
                case LIBRFNM_STREAM_FORMAT_CF32:
                    applyQuadDcOffset(reinterpret_cast<float *>(lrxbuf->buf), outbufsize / 4, dc_offsets[channel].f32);
                    break;
                }
            }

            if ((read_elems + (outbufsize / bytes_per_ele)) > numElems) {

                overflowing_by_elems = (read_elems + (outbufsize / bytes_per_ele)) - numElems;
                can_copy_bytes = outbufsize - (overflowing_by_elems * bytes_per_ele);
            }

            std::memcpy(((uint8_t*)buffs[buf_idx]) + (bytes_per_ele * read_elems), lrxbuf->buf, can_copy_bytes);

            if (overflowing_by_elems) {
                std::memcpy(partial_rx_buf[channel].buf, (lrxbuf->buf + can_copy_bytes), outbufsize - can_copy_bytes);
                partial_rx_buf[channel].left = outbufsize - can_copy_bytes;
                partial_rx_buf[channel].offset = 0;
            }

            lrfnm->rx_qbuf(lrxbuf);
            read_elems += (outbufsize / bytes_per_ele) - overflowing_by_elems;
        }

        buf_idx++;
    }

    return read_elems;
}

bool SoapyRFNM::hasDCOffsetMode(const int direction, const size_t channel) const {
    return true;
}

void SoapyRFNM::setDCOffsetMode(const int direction, const size_t channel, const bool automatic) {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        dc_correction[channel] = automatic;
    }
}

bool SoapyRFNM::getDCOffsetMode(const int direction, const size_t channel) const {
    if (direction == SOAPY_SDR_RX) {
        if (channel >= rx_chan_count) {
            throw std::runtime_error("nonexistent channel");
        }

        return dc_correction[channel];
    } else {
        return false;
    }
}

void SoapyRFNM::setRFNM(uint16_t applies) {
    rfnm_api_failcode ret = lrfnm->set(applies);

    size_t chan_idx;
    switch (applies) {
    case LIBRFNM_APPLY_CH0_RX:
        chan_idx = 0;
        break;
    case LIBRFNM_APPLY_CH1_RX:
        chan_idx = 1;
        break;
    case LIBRFNM_APPLY_CH2_RX:
        chan_idx = 2;
        break;
    case LIBRFNM_APPLY_CH3_RX:
        chan_idx = 3;
        break;
    default:
        chan_idx = 0;
    }

    // GCC cannot pass references to values in packed structs, so we need stack copies
    uint64_t freq = lrfnm->s->rx.ch[chan_idx].freq;
    int8_t gain = lrfnm->s->rx.ch[chan_idx].gain;

    switch (ret) {
    case RFNM_API_OK:
        return;
    case RFNM_API_TUNE_FAIL:
        spdlog::error("Failure tuning channel {} to {} Hz", chan_idx, freq);
        throw std::runtime_error("Tuning failure");
    case RFNM_API_GAIN_FAIL:
        spdlog::error("Failure setting channel {} gain to {} dB", chan_idx, gain);
        throw std::runtime_error("Gain setting failure");
    case RFNM_API_TIMEOUT:
        spdlog::error("Timeout configuring RFNM");
        throw std::runtime_error("Timeout configuring RFNM");
    case RFNM_API_USB_FAIL:
        spdlog::error("USB failure configuring RFNM");
        throw std::runtime_error("USB failure configuring RFNM");
    default:
        spdlog::error("Error {} configuring RFNM", static_cast<int>(ret));
        throw std::runtime_error("Error configuring RFNM");
    }
}

SoapySDR::Device* rfnm_device_create(const SoapySDR::Kwargs& args) {
    spdlog::info("rfnm_device_create()");

    return new SoapyRFNM(args);
}

SoapySDR::KwargsList rfnm_device_find(const SoapySDR::Kwargs& args) {
    std::vector<struct rfnm_dev_hwinfo> hwlist = librfnm::find(LIBRFNM_TRANSPORT_USB);
    std::vector< SoapySDR::Kwargs> ret;

    for (auto& hw : hwlist)
    {
        SoapySDR::Kwargs deviceInfo;

        deviceInfo["device_id"] = "RFNM";
        deviceInfo["label"] = "RFNM";
        if (hw.daughterboard[0].board_id) {
            std::string dgbn = reinterpret_cast<char*>(hw.daughterboard[0].user_readable_name);
            deviceInfo["label"] += " with " + dgbn;
        }

        if (hw.daughterboard[1].board_id) {
            std::string dgbn = reinterpret_cast<char*>(hw.daughterboard[1].user_readable_name);
            deviceInfo["label"] += " and " + dgbn + " daughterboards";
        }
        else {
            deviceInfo["label"] += " daughterboard";
        }

        std::string serial = reinterpret_cast<char*>(hw.motherboard.serial_number);
        deviceInfo["serial"] = serial;

        ret.push_back(deviceInfo);
    }

    return ret;
}

[[maybe_unused]] static SoapySDR::Registry rfnm_module_registration("RFNM", &rfnm_device_find, &rfnm_device_create, SOAPY_SDR_ABI_VERSION);
