/* gpio.c: the real-time process that handles BlinkenBoard multiplexing

 Copyright (c) 2016 Joerg Hoppe
 j_hoppe@t-online.de, www.retrocmp.com

 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 JOERG HOPPE BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
 IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 04-Aug-2016  JH    Switch polling with reduced frequency, to supress contact bounce
 25-May-2016  JH	created

 The PDP-15 is connected to a single BlinkenBoard.
 The panel consists of two independed boards:
 - Indicator Board with the light bulbs,
 - Switch Board with switches.
 switches and lamps each are arranged in a matrix and accessed via multiplexing.
 There are 3 mux signals (TWO, ONE, ZERO).

 Board      MUX codes   Output
 indicator  3,5,7       OUT7, bits<7:5>     other codes: all lamps OFF
 switch     1,2,6       OUT0, bits<2:0>

 Lamp protection:
 The lamps are light bulbs with 12V rating and are driven with 30V.
 A separate ATmega88 micro controller monitors the
 indicator-MUX signal and switches 30V OFF if the MUX pattern is
 lighting a row more than 50 millisconds.

 */

#define _GPIO_C_

#include <time.h>
#include <pthread.h>
#include <stdint.h>

#include "blinkenbus.h" // from std server

#include "main.h"   // pdp15_panel
#include "print.h"
#include "gpio.h"

static blinkenbus_map_t blinkenbus_output_cache;
static blinkenbus_map_t blinkenbus_input_cache;

static void microsleep(unsigned microsecs)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 1000 * microsecs;
    nanosleep(&ts, NULL);
}

// write mux to BlinkenBoard #0, OUT7, Bits 7:5
static void write_indicator_mux_code(unsigned mux_code)
{
    unsigned char regval = (mux_code & 7) << 5;
    blinkenbus_register_write(0, 7, regval);
}

// write mux to BlinkenBoard #0, OUT0, Bits 2:0
static void write_switch_mux_code(unsigned mux_code)
{
    unsigned char regval = mux_code & 7;
    blinkenbus_register_write(0, 0, regval);
}

// set the BlinkenBoard register bits for a lamp mux row
//
// muxcode == 0: all lamps OFF by setting mux select to 0
//
// Limitation: A control value is completely assembled from the same cache
// and can not span several MUX rows.
static void outputcontrols_to_mux_row(unsigned mux_code)
{
    unsigned i_control;
    blinkenlight_control_t *c;
    unsigned c_muxcode;

    if (mux_code != 0) {

        // lamp test: show light, but controls hold their state
        for (i_control = 0; i_control < pdp15_panel->controls_count; i_control++) {
            c = &(pdp15_panel->controls[i_control]);
            // patch: all wirings of control must have same mux code
            c_muxcode = c->blinkenbus_register_wiring[0].mux_code;

            if (mux_code == c_muxcode && !c->is_input) {
                uint64_t value;
                if (pdp15_panel->mode == RPC_PARAM_VALUE_PANEL_MODE_ALLTEST
                        || (pdp15_panel->mode == RPC_PARAM_VALUE_PANEL_MODE_LAMPTEST
                                && c->type == output_lamp))
                    value = 0xffffffffffffffff; // selftest:
                else if (pdp15_panel->mode == RPC_PARAM_VALUE_PANEL_MODE_POWERLESS
                        && c->type == output_lamp)
                    value = 0; // all OFF
                else
                    value = c->value;

                blinkenbus_outputcontrol_to_cache(blinkenbus_output_cache, c, value);
                // "write as mask": all ones, if self test
            }
        }
    }

    blinkenbus_cache_to_blinkenboards_outputs(blinkenbus_output_cache, /* force_all*/1);
}

// read the BlinkenBoard register bits for a switch mux row
//
// Limitation: A control value is completely assembled from the same cache
// and can not span several MUX rows.

static void inputcontrols_from_mux_row(unsigned mux_code)
{
    uint64_t now_us;
    unsigned i_control;
    blinkenlight_control_t *c;
    unsigned c_muxcode;

    if (mux_code == 0)
        return;
    now_us = historybuffer_now_us();

    // LOCK
    blinkenbus_cache_from_blinkenboards_inputs(blinkenbus_input_cache);
    // UNLOCK

    for (i_control = 0; i_control < pdp15_panel->controls_count; i_control++) {
        c = &(pdp15_panel->controls[i_control]);
        // patch: all wirings of control must have same mux code
        c_muxcode = c->blinkenbus_register_wiring[0].mux_code;

        if (mux_code == c_muxcode && c->is_input) {
            blinkenbus_control_from_cache(blinkenbus_input_cache, c);
            // if (c->fmax > 0)
            //     historybuffer_set_val(c->history, now_us, c->value) ;
        }
    }
    if (mux_code == MUX1) {
        /* exam/deposit this/next must be constructed separateley
         * physical keyboard signals:
         * S30 "DEP THIS"       => In2.2
         * S32 "EXAMINE THIS"   => In2.1
         * S31 "DEP NEXT" & S33 "EXAMINE NEXT" is one signal, combines with EXAM/DEPOSIT THIS
         *                      => In1.0
         *
         *        keyboard                       API
         * dep_this     dep_exam_next   exam_this   exam_next
         * 0            *               0           0
         * 1            0               1           0
         * 1            1               0           1
         *
         * exam_this    dep_exam_next   dep_this    dep_next
         * 0            *               0           0
         * 1            0               1           0
         * 1            1               0           1
         */
        int keyboard_deposit_this = !(blinkenbus_input_cache[2] & 0x4); // In2.2
        int keyboard_examine_this = !(blinkenbus_input_cache[2] & 0x2); // In2.1
        int keyboard_dep_exam_next = !(blinkenbus_input_cache[1] & 0x1); // In1.0
        static blinkenbus_map_t input_cache_prev; // to detect changes
        static int dbg_eventcnt = 0;
        extern blinkenlight_control_t *switch_deposit_this; // main.c
        extern blinkenlight_control_t *switch_deposit_next;
        extern blinkenlight_control_t *switch_examine_this;
        extern blinkenlight_control_t *switch_examine_next;

        if (keyboard_deposit_this == 0) {
            switch_deposit_this->value = 0;
            switch_deposit_next->value = 0;
        } else if (keyboard_dep_exam_next == 0) {
            switch_deposit_this->value = 1;
            switch_deposit_next->value = 0;
        } else {
            switch_deposit_this->value = 0;
            switch_deposit_next->value = 1;
        }
        if (keyboard_examine_this == 0) {
            switch_examine_this->value = 0;
            switch_examine_next->value = 0;
        } else if (keyboard_dep_exam_next == 0) {
            switch_examine_this->value = 1;
            switch_examine_next->value = 0;
        } else {
            switch_examine_this->value = 0;
            switch_examine_next->value = 1;
        }

#if DBG
        /* detect contact bounce on EXAM/DEPOSIT  */
        for (int i = 0; i <= 7; i++) {
            for (int j = 0; j < 8; j++) {
                int mask = 1 << j; // detect indivdual pins
                int bitnow = !!(blinkenbus_input_cache[i] & mask);
                int bitprev = !!(input_cache_prev[i] & mask);
                if (bitnow != bitprev)
                    printf("%u) input regbit[%d.%d]: %d -> %d\n", dbg_eventcnt++, i, j, bitprev,
                            bitnow);
            }
            input_cache_prev[i] = blinkenbus_input_cache[i];
        }

        /* display changed values, use "tag" as "known"*/
        for (i_control = 0; i_control < pdp15_panel->controls_count; i_control++) {
            c = &(pdp15_panel->controls[i_control]);
            if (c->value != c->tag) {
                printf("%u) %s: %llu -> %llu\n", dbg_eventcnt++, c->name, c->tag, c->value);
            }
            c->tag = c->value;
        }
#endif
    }
}

/***********************************************
 * the multiplexing thread
 * 3 switche rows and 3 lamps rows are scanned in parallel
 * there are "all OFF" cycles to regulate lamp brightness.
 *
 *
 * The
 */
void mux(int * terminate)
{
#define PHASES  3 // 3 lamp phases, some idle phases
    unsigned mux_sleep_us;
    unsigned phase = 0;
    unsigned mux_code;
    unsigned regaddr;

    unsigned switch_prescaler_val;
    unsigned switch_prescaler = 0;

// default
    if (opt_mux_frequency)
        mux_sleep_us = 1000000 / opt_mux_frequency;
    else
        mux_sleep_us = 1000; // 1 kHz
    print(LOG_NOTICE, "Lamp multiplexing period = %u us.\n", mux_sleep_us);

    switch_prescaler_val = 0;
    if (opt_switch_mux_frequency)
        switch_prescaler_val = opt_mux_frequency / opt_switch_mux_frequency;
    if (switch_prescaler_val <= 0)
        // else  poll switch with 10 Hz
        switch_prescaler_val = opt_mux_frequency / 10;
    switch_prescaler = 0;
    print(LOG_NOTICE, "Switch multiplexing period = %d Hz = %u us.\n", opt_switch_mux_frequency,
            switch_prescaler_val * mux_sleep_us);

    blinkenbus_init();

// set thread to real time priority
// seems not to work under BeagleBone Angstrom: now realtime-kernel?
    {
        struct sched_param sp;
        int policy, prio;
        int res;
        sp.sched_priority = 10;
        // sp.sched_priority = 98; // maybe 99, 32, 31?
        //res = pthread_setschedparam(pthread_self(), SCHED_RR, &sp) ;
        pthread_getschedparam(pthread_self(), &policy, &sp);
        prio = sp.sched_priority + 1;
        res = pthread_setschedprio(pthread_self(), prio);
        // res = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) ;
        if (res)
            print(LOG_ERR,
                    "Warning: failed to set RT priority to %d, pthread_setschedparam() returned %d\n",
                    prio, res);
        // 1 = EPERM
        // 22 = EINVAL
    }

    while (!*terminate) {
        phase = (phase + 1) % PHASES;

        // 1. set board #0 control register periodically to "outputs enabled"
        if (phase == 0) {
            blinkenbus_board_control_write(0, 0);
        }

        // 2. drive lamp row. Option: distribute "ON" phases evenly
        switch (phase) {
        case 0:
            mux_code = 3;
            break;
        case 1:
            mux_code = 5;
            break;
        case 2:
            mux_code = 7;
            break;
        default:
            mux_code = 0; // all off
        }
        // printf("%u ", mux_code) ;
        // assemble value for BlinkenBoard output registers
        // from control&wiring struct
        // muxcode = 0: all OFF
        write_indicator_mux_code(mux_code);

        outputcontrols_to_mux_row(mux_code);

        /* Poll the switches with reduced freqeuncy */
        if (switch_prescaler > PHASES) {
            switch_prescaler--;
            microsleep(mux_sleep_us);
        } else {
            // poll switches for 3 phases = prescaler 2,1,0
            // 3. drive switch row
            switch (switch_prescaler % PHASES) {
            case 0:
                mux_code = 1;
                break;
            case 1:
                mux_code = 2;
                break;
            case 2:
                mux_code = 6;
                break;
            default:
                mux_code = 0; // no scan
            }
            // printf("Polling switches with mux code %u\n", mux_code) ;
            // leave switch polling after last phases
            if (switch_prescaler > 0)
                switch_prescaler--;
            else
                switch_prescaler = switch_prescaler_val; // reload


            // 3.1. assemble switch values from BlinkenBoard input registers
            // from control&wiring struct
            // muxcode = 0: no-op
            if (mux_code)
                write_switch_mux_code(mux_code);

            // 4. thread wait 1 millisecond
            // the ATmega samples the MUX signal with 10kHz,
            // we must generated here max of that half frequency.
            microsleep(mux_sleep_us);

            if (mux_code) {
                // 3.2. read delayed, after mux has settled
                inputcontrols_from_mux_row(mux_code);
            }
        }
    }
}

