/*
 * Copyright (c) 2007, 2009 Joseph Gaeddert
 * Copyright (c) 2007, 2009 Virginia Polytechnic Institute & State University
 *
 * This file is part of liquid.
 *
 * liquid is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * liquid is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with liquid.  If not, see <http://www.gnu.org/licenses/>.
 */

//
//
//

#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>
#include <complex.h>

#include "liquid.internal.h"

#define FLEXFRAMESYNC_SYM_BW_0          (0.01f)
#define FLEXFRAMESYNC_SYM_BW_1          (0.001f)

#define FLEXFRAMESYNC_AGC_BW_0          (3e-3f)
#define FLEXFRAMESYNC_AGC_BW_1          (1e-5f)

#define FLEXFRAMESYNC_PLL_BW_0          (2e-3f)
#define FLEXFRAMESYNC_PLL_BW_1          (1e-3f)

#define FLEXFRAMESYNC_SQUELCH_THRESH    (-15.0f)
#define FLEXFRAMESYNC_SQUELCH_TIMEOUT   (32)

#define FRAME64_PN_LEN                  (64)


#define DEBUG_FLEXFRAMESYNC
#define DEBUG_FLEXFRAMESYNC_PRINT
#define DEBUG_FLEXFRAMESYNC_FILENAME    "flexframesync_internal_debug.m"
#define DEBUG_FLEXFRAMESYNC_BUFFER_LEN  (4096)

// Internal
void flexframesync_open_bandwidth(flexframesync _fs);
void flexframesync_close_bandwidth(flexframesync _fs);
void flexframesync_decode_header(flexframesync _fs);
void flexframesync_decode_payload(flexframesync _fs);

struct flexframesync_s {
    modem demod;
    modem bpsk;
    interleaver intlv;
    fec dec;

    // synchronizer objects
    agc agc_rx;
    symsync_crcf mfdecim;
    pll pll_rx;
    nco nco_rx;
    bsync_rrrf fsync;

    // status variables
    enum {
        FLEXFRAMESYNC_STATE_SEEKPN=0,
        FLEXFRAMESYNC_STATE_RXHEADER,
        FLEXFRAMESYNC_STATE_RXPAYLOAD,
        FLEXFRAMESYNC_STATE_RESET
    } state;
    unsigned int num_symbols_collected;
    unsigned int header_key;
    bool header_valid;

    // header
    unsigned char header_sym[128];
    unsigned char header_enc[32];
    unsigned char header[15];

    // payload
    unsigned char * payload_sym;
    unsigned char * payload;

    // properties
    flexframesyncprops_s props;

    // callback
    flexframesync_callback callback;
    void * userdata;

#ifdef DEBUG_FLEXFRAMESYNC
    FILE*fid;
    fwindow  debug_agc_rssi;
    cfwindow debug_agc_out;
    cfwindow debug_x;
    cfwindow debug_rxy;
    cfwindow debug_nco_rx_out;
    fwindow  debug_nco_phase;
    fwindow  debug_nco_freq;
#endif
};

flexframesync flexframesync_create(flexframesyncprops_s * _props,
                                   flexframesync_callback _callback,
                                   void * _userdata)
{
    flexframesync fs = (flexframesync) malloc(sizeof(struct flexframesync_s));
    fs->callback = _callback;
    fs->userdata = _userdata;

#if 0
    // agc, rssi, squelch
    fs->agc_rx = agc_create(1.0f, FLEXFRAMESYNC_AGC_BW_0);
    agc_set_gain_limits(fs->agc_rx, 1e-6, 1e2);
    fs->squelch_threshold = FLEXFRAMESYNC_SQUELCH_THRESH;
    fs->squelch_timeout = FLEXFRAMESYNC_SQUELCH_TIMEOUT;
    fs->squelch_timer = fs->squelch_timeout;

    // pll, nco
    fs->pll_rx = pll_create();
    fs->nco_rx = nco_create();
    pll_set_bandwidth(fs->pll_rx, FLEXFRAMESYNC_PLL_BW_0);

    // bsync (p/n synchronizer)
    unsigned int i;
    msequence ms = msequence_create(6);
    float pn_sequence[FRAME64_PN_LEN];
    for (i=0; i<FRAME64_PN_LEN; i++)
        pn_sequence[i] = (msequence_advance(ms)) ? 1.0f : -1.0f;
    fs->fsync = bsync_rrrf_create(FRAME64_PN_LEN, pn_sequence);
    msequence_destroy(ms);

    // design symsync (k=2)
    unsigned int npfb = 32;
    unsigned int H_len = 2*2*npfb*3 + 1;// 2*2*npfb*_m + 1;
    float H[H_len];
    design_rrc_filter(2*npfb,3,0.7f,0,H);
    fs->mfdecim =  symsync_crcf_create(2, npfb, H, H_len-1);

    // create (de)interleaver
    fs->intlv = interleaver_create(128, INT_BLOCK);

    // create decoder
    fs->dec = fec_create(FEC_HAMMING74, NULL);

    // create demod
    fs->demod = modem_create(MOD_QPSK, 2);
    fs->bpsk = modem_create(MOD_BPSK, 1);

    // set status flags
    fs->state = FLEXFRAMESYNC_STATE_SEEKPN;
    fs->num_symbols_collected = 0;

    // set open/closed bandwidth values
    flexframesync_set_agc_bw0(fs,FLEXFRAMESYNC_AGC_BW_0);
    flexframesync_set_agc_bw1(fs,FLEXFRAMESYNC_AGC_BW_1);
    flexframesync_set_pll_bw0(fs,FLEXFRAMESYNC_PLL_BW_0);
    flexframesync_set_pll_bw1(fs,FLEXFRAMESYNC_PLL_BW_1);
    flexframesync_set_sym_bw0(fs,FLEXFRAMESYNC_SYM_BW_0);
    flexframesync_set_sym_bw1(fs,FLEXFRAMESYNC_SYM_BW_1);
    flexframesync_set_squelch_threshold(fs,FLEXFRAMESYNC_SQUELCH_THRESH);

    // open bandwidth
    flexframesync_open_bandwidth(fs);
#endif

#ifdef DEBUG_FLEXFRAMESYNC
    fs->debug_agc_rssi  =  fwindow_create(DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    fs->debug_agc_out   = cfwindow_create(DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    fs->debug_x         = cfwindow_create(DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    fs->debug_rxy       = cfwindow_create(DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    fs->debug_nco_rx_out= cfwindow_create(DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    fs->debug_nco_phase=   fwindow_create(DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    fs->debug_nco_freq =   fwindow_create(DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
#endif

    return fs;
}

void flexframesync_destroy(flexframesync _fs)
{
#if 0
    symsync_crcf_destroy(_fs->mfdecim);
    fec_destroy(_fs->dec);
    interleaver_destroy(_fs->intlv);
    agc_destroy(_fs->agc_rx);
    pll_destroy(_fs->pll_rx);
    nco_destroy(_fs->nco_rx);
    bsync_rrrf_destroy(_fs->fsync);
    modem_destroy(_fs->bpsk);
    modem_destroy(_fs->demod);
#endif

#ifdef DEBUG_FLEXFRAMESYNC
    unsigned int i;
    float * r;
    float complex * rc;
    FILE* fid = fopen(DEBUG_FLEXFRAMESYNC_FILENAME,"w");
    fprintf(fid,"%% %s: auto-generated file", DEBUG_FLEXFRAMESYNC_FILENAME);
    fprintf(fid,"\n\n");
    fprintf(fid,"clear all;\n");
    fprintf(fid,"close all;\n\n");

    // write agc_rssi
    fprintf(fid,"agc_rssi = zeros(1,%u);\n", DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    fwindow_read(_fs->debug_agc_rssi, &r);
    for (i=0; i<DEBUG_FLEXFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"agc_rssi(%4u) = %12.4e;\n", i+1, r[i]);
    fprintf(fid,"\n\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(10*log10(agc_rssi))\n");
    fprintf(fid,"ylabel('RSSI [dB]');\n");

    // write agc out
    fprintf(fid,"agc_out = zeros(1,%u);\n", DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    cfwindow_read(_fs->debug_agc_out, &rc);
    for (i=0; i<DEBUG_FLEXFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"agc_out(%4u) = %12.4e + j*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));
    fprintf(fid,"\n\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(1:length(agc_out),real(agc_out), 1:length(agc_out),imag(agc_out));\n");
    fprintf(fid,"ylabel('agc-out');\n");


    // write x
    fprintf(fid,"x = zeros(1,%u);\n", DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    cfwindow_read(_fs->debug_x, &rc);
    for (i=0; i<DEBUG_FLEXFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"x(%4u) = %12.4e + j*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));
    fprintf(fid,"\n\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(1:length(x),real(x), 1:length(x),imag(x));\n");
    fprintf(fid,"ylabel('received signal, x');\n");

    // write rxy
    fprintf(fid,"rxy = zeros(1,%u);\n", DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    cfwindow_read(_fs->debug_rxy, &rc);
    for (i=0; i<DEBUG_FLEXFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"rxy(%4u) = %12.4e + j*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));
    fprintf(fid,"\n\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(abs(rxy))\n");
    fprintf(fid,"ylabel('|r_{xy}|');\n");

    // write nco_rx_out
    fprintf(fid,"nco_rx_out = zeros(1,%u);\n", DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    cfwindow_read(_fs->debug_nco_rx_out, &rc);
    for (i=0; i<DEBUG_FLEXFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"nco_rx_out(%4u) = %12.4e + j*%12.4e;\n", i+1, crealf(rc[i]), cimagf(rc[i]));
    fprintf(fid,"\n\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(nco_rx_out,'x')\n");
    fprintf(fid,"xlabel('I');\n");
    fprintf(fid,"ylabel('Q');\n");
    fprintf(fid,"axis square;\n");
    fprintf(fid,"axis([-1.2 1.2 -1.2 1.2]);\n");

    // write nco_phase
    fprintf(fid,"nco_phase = zeros(1,%u);\n", DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    fwindow_read(_fs->debug_nco_phase, &r);
    for (i=0; i<DEBUG_FLEXFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"nco_phase(%4u) = %12.4e;\n", i+1, r[i]);
    fprintf(fid,"\n\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(nco_phase)\n");
    fprintf(fid,"ylabel('nco phase [radians]');\n");

    // write nco_freq
    fprintf(fid,"nco_freq = zeros(1,%u);\n", DEBUG_FLEXFRAMESYNC_BUFFER_LEN);
    fwindow_read(_fs->debug_nco_freq, &r);
    for (i=0; i<DEBUG_FLEXFRAMESYNC_BUFFER_LEN; i++)
        fprintf(fid,"nco_freq(%4u) = %12.4e;\n", i+1, r[i]);
    fprintf(fid,"\n\n");
    fprintf(fid,"figure;\n");
    fprintf(fid,"plot(nco_freq)\n");
    fprintf(fid,"ylabel('nco freq');\n");


    fprintf(fid,"\n\n");
    fclose(fid);

    printf("flexframesync/debug: results written to %s\n", DEBUG_FLEXFRAMESYNC_FILENAME);

    // clean up debug windows
    fwindow_destroy(_fs->debug_agc_rssi);
    cfwindow_destroy(_fs->debug_agc_out);
    cfwindow_destroy(_fs->debug_rxy);
    cfwindow_destroy(_fs->debug_x);
    cfwindow_destroy(_fs->debug_nco_rx_out);
#endif
    free(_fs);
}

void flexframesync_print(flexframesync _fs)
{
    printf("flexframesync:\n");
}

void flexframesync_reset(flexframesync _fs)
{
    symsync_crcf_clear(_fs->mfdecim);
    pll_reset(_fs->pll_rx);
    agc_set_bandwidth(_fs->agc_rx, FLEXFRAMESYNC_AGC_BW_0);
    nco_set_phase(_fs->nco_rx, 0.0f);
    nco_set_frequency(_fs->nco_rx, 0.0f);
}

void flexframesync_execute(flexframesync _fs, float complex *_x, unsigned int _n)
{
#if 0
    unsigned int i, j, nw;
    float complex agc_rx_out;
    float complex mfdecim_out[4];
    float complex nco_rx_out;
    float phase_error;
    //float complex rxy;
    float rxy;
    unsigned int demod_sym;

    for (i=0; i<_n; i++) {
        // agc
        agc_execute(_fs->agc_rx, _x[i], &agc_rx_out);
        _fs->rssi = 10*log10(agc_get_signal_level(_fs->agc_rx));
#ifdef DEBUG_FLEXFRAMESYNC
        cfwindow_push(_fs->debug_x, _x[i]);
        fwindow_push(_fs->debug_agc_rssi, agc_get_signal_level(_fs->agc_rx));
        cfwindow_push(_fs->debug_agc_out, agc_rx_out);
#endif

        // squelch: block agc output from synchronizer only if
        // 1. received signal strength indicator has not exceeded squelch
        //    threshold at any time within the past <squelch_timeout> samples
        // 2. mode is FLEXFRAMESYNC_STATE_SEEKPN (seek p/n sequence)
        if (_fs->rssi < _fs->squelch_threshold &&
            _fs->state == FLEXFRAMESYNC_STATE_SEEKPN)
        {
            if (_fs->squelch_timer > 1) {
                // signal low, but we haven't reached timout yet; decrement
                // counter and continue
                _fs->squelch_timer--;
            } else if (_fs->squelch_timer == 1) {
                // squelch timeout: signal has been too low for too long

                //printf("squelch enabled\n");
                _fs->squelch_timer = 0;
                flexframesync_reset(_fs);
                continue;
            } else {
                // squelch enabled: ignore sample (wait for high signal)
                continue;
            }
        } else {
            // signal high: reset timer and continue
            _fs->squelch_timer = _fs->squelch_timeout;
        }

        // symbol synchronizer
        symsync_crcf_execute(_fs->mfdecim, &agc_rx_out, 1, mfdecim_out, &nw);

        for (j=0; j<nw; j++) {
            // mix down, demodulate, run PLL
            nco_mix_down(_fs->nco_rx, mfdecim_out[j], &nco_rx_out);
            if (_fs->state == FLEXFRAMESYNC_STATE_SEEKPN) {
            //if (false) {
                modem_demodulate(_fs->bpsk, nco_rx_out, &demod_sym);
                get_demodulator_phase_error(_fs->bpsk, &phase_error);
            } else {
                modem_demodulate(_fs->demod, nco_rx_out, &demod_sym);
                get_demodulator_phase_error(_fs->demod, &phase_error);
            }

            //if (_fs->rssi < _fs->squelch_threshold)
            //    phase_error *= 0.01f;

            pll_step(_fs->pll_rx, _fs->nco_rx, phase_error);
            /*
            float fmax = 0.05f;
            if (_fs->nco_rx->d_theta >  fmax) _fs->nco_rx->d_theta =  fmax;
            if (_fs->nco_rx->d_theta < -fmax) _fs->nco_rx->d_theta = -fmax;
            */
            nco_step(_fs->nco_rx);
#ifdef DEBUG_FLEXFRAMESYNC
            fwindow_push(_fs->debug_nco_phase, _fs->nco_rx->theta);
            fwindow_push(_fs->debug_nco_freq,  _fs->nco_rx->d_theta);
            cfwindow_push(_fs->debug_nco_rx_out, nco_rx_out);
#endif
            if (_fs->rssi < _fs->squelch_threshold)
                continue;

            //
            switch (_fs->state) {
            case FLEXFRAMESYNC_STATE_SEEKPN:
                //
                bsync_rrrf_correlate(_fs->fsync, nco_rx_out, &rxy);
#ifdef DEBUG_FLEXFRAMESYNC
                cfwindow_push(_fs->debug_rxy, rxy);
#endif
                if (fabsf(rxy) > 0.7f) {
                    // printf("|rxy| = %8.4f, angle: %8.4f\n",cabsf(rxy),cargf(rxy));
                    // close bandwidth
                    pll_reset(_fs->pll_rx);
                    flexframesync_close_bandwidth(_fs);
                    nco_adjust_phase(_fs->nco_rx, M_PI - cargf(rxy));
                    _fs->state = FLEXFRAMESYNC_STATE_RXHEADER;
                }
                break;
            case FLEXFRAMESYNC_STATE_RXHEADER:
                _fs->header_sym[_fs->num_symbols_collected] = (unsigned char) demod_sym;
                _fs->num_symbols_collected++;
                if (_fs->num_symbols_collected==256) {
                    _fs->num_symbols_collected = 0;
                    _fs->state = FLEXFRAMESYNC_STATE_RXPAYLOAD;
                    flexframesync_decode_header(_fs);
                }
                break;
            case FLEXFRAMESYNC_STATE_RXPAYLOAD:
                _fs->payload_sym[_fs->num_symbols_collected] = (unsigned char) demod_sym;
                _fs->num_symbols_collected++;
                if (_fs->num_symbols_collected==512) {
                    _fs->num_symbols_collected = 0;
                    flexframesync_decode_payload(_fs);

                    // invoke callback method
                    _fs->callback(_fs->header,  _fs->header_valid,
                                  _fs->payload, _fs->payload_valid,
                                  _fs->userdata);

                    _fs->state = FLEXFRAMESYNC_STATE_RESET;
                    //_fs->state = FLEXFRAMESYNC_STATE_SEEKPN;
//#ifdef DEBUG_FLEXFRAMESYNC
#if 0
                    printf("flexframesync exiting prematurely\n");
                    flexframesync_destroy(_fs);
                    exit(0);
#endif
                }
                break;
            case FLEXFRAMESYNC_STATE_RESET:
                // open bandwidth
                flexframesync_open_bandwidth(_fs);
                _fs->state = FLEXFRAMESYNC_STATE_SEEKPN;
                _fs->num_symbols_collected = 0;

                _fs->nco_rx->theta=0.0f;
                _fs->nco_rx->d_theta=0.0f;
                pll_reset(_fs->pll_rx);
                break;
            default:;
            }
        }
    }
    //printf("rssi: %8.4f\n", 10*log10(agc_get_signal_level(_fs->agc_rx)));
#endif
}

// 
// internal
//

void flexframesync_open_bandwidth(flexframesync _fs)
{
    agc_set_bandwidth(_fs->agc_rx, _fs->props.agc_bw0);
    symsync_crcf_set_lf_bw(_fs->mfdecim, _fs->props.sym_bw0);
    pll_set_bandwidth(_fs->pll_rx, _fs->props.pll_bw0);
}

void flexframesync_close_bandwidth(flexframesync _fs)
{
    agc_set_bandwidth(_fs->agc_rx, _fs->props.agc_bw1);
    symsync_crcf_set_lf_bw(_fs->mfdecim, _fs->props.sym_bw1);
    pll_set_bandwidth(_fs->pll_rx, _fs->props.pll_bw1);
}

void flexframesync_decode_header(flexframesync _fs)
{
    unsigned int i;
    unsigned char b;
    for (i=0; i<32; i++) {
        b = 0;
        b |= (_fs->header_sym[0] << 6) & 0xc0;
        b |= (_fs->header_sym[1] << 4) & 0x30;
        b |= (_fs->header_sym[2] << 2) & 0x0c;
        b |= (_fs->header_sym[3]     ) & 0x03;
        _fs->header_enc[i] = b;
    }

#ifdef DEBUG_FLEXFRAMESYNC_PRINT
    printf("header ENCODED (rx):\n");
    for (i=0; i<64; i++) {
        printf("%2x ", _fs->header_enc[i]);
        if (!((i+1)%16)) printf("\n");
    }
    printf("\n");
#endif

    // decode header
    fec_decode(_fs->dec, 32, _fs->header_enc, _fs->header);

    // unscramble header data
    unscramble_data(_fs->header, 32);

#ifdef DEBUG_FLEXFRAMESYNC_PRINT
    printf("header (rx):\n");
    for (i=0; i<32; i++) {
        printf("%2x ", _fs->header[i]);
        if (!((i+1)%8)) printf("\n");
    }
    printf("\n");
#endif

    // strip off crc32
    unsigned int header_key=0;
    header_key |= ( _fs->header[28] << 24 );
    header_key |= ( _fs->header[29] << 16 );
    header_key |= ( _fs->header[30] <<  8 );
    header_key |= ( _fs->header[31]       );
    _fs->header_key = header_key;
    //printf("rx: header_key:  0x%8x\n", header_key);

    // validate crc
    _fs->header_valid = crc32_validate_message(_fs->header,28,_fs->header_key);
}

void flexframesync_decode_payload(flexframesync _fs)
{
#if 0
    unsigned int i;
    for (i=0; i<128; i++)
        flexframesync_syms_to_byte(&(_fs->payload_sym[4*i]), &(_fs->payload_intlv[i]));

    // deinterleave payload
    interleaver_deinterleave(_fs->intlv, _fs->payload_intlv, _fs->payload_enc);
    
    // decode payload
    fec_decode(_fs->dec, 64, _fs->payload_enc, _fs->payload);

    // unscramble payload data
    unscramble_data(_fs->payload, 64);

    // validate crc
    _fs->payload_valid = crc32_validate_message(_fs->payload,64,_fs->payload_key);

#ifdef DEBUG_FLEXFRAMESYNC_PRINT
    printf("payload (rx):\n");
    for (i=0; i<64; i++) {
        printf("%2x ", _fs->payload[i]);
        if (!((i+1)%8)) printf("\n");
    }
    printf("\n");
#endif

#endif

}


