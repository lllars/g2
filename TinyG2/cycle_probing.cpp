/*
 * cycle_probing.c - probing cycle extension to canonical_machine.c
 * Part of TinyG project
 *
 * Copyright (c) 2010 - 2015 Alden S Hart, Jr., Sarah Tappon, Tom Cauchois, Robert Giseburt
 * With contributions from Other Machine Company.
 *
 * This file ("the software") is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 as published by the
 * Free Software Foundation. You should have received a copy of the GNU General Public
 * License, version 2 along with the software.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, you may use this file as part of a software library without
 * restriction. Specifically, if other files instantiate templates or use macros or
 * inline functions from this file, or you compile this file and link it with  other
 * files to produce an executable, this file does not by itself cause the resulting
 * executable to be covered by the GNU General Public License. This exception does not
 * however invalidate any other reasons why the executable file might be covered by the
 * GNU General Public License.
 *
 * THE SOFTWARE IS DISTRIBUTED IN THE HOPE THAT IT WILL BE USEFUL, BUT WITHOUT ANY
 * WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
 * SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "tinyg2.h"
#include "config.h"
#include "json_parser.h"
#include "text_parser.h"
#include "canonical_machine.h"
#include "kinematics.h"
#include "encoder.h"
#include "spindle.h"
#include "report.h"
#include "gpio.h"
#include "planner.h"
#include "util.h"

/**** Probe singleton structure ****/

#define MINIMUM_PROBE_TRAVEL 0.254

struct pbProbingSingleton {						// persistent probing runtime variables
	stat_t (*func)();							// binding for callback function state machine

	// controls for probing cycle
	uint8_t probe_input;						// which input should we check?

	// state saved from gcode model
	uint8_t saved_distance_mode;				// G90,G91 global setting
	uint8_t saved_coord_system;					// G54 - G59 setting
	float saved_jerk[AXES];						// saved and restored for each axis

	// probe destination
	float target[AXES];
	bool flags[AXES];
};
static struct pbProbingSingleton pb;

/**** NOTE: global prototypes and other .h info is located in canonical_machine.h ****/

static stat_t _probing_init();
static stat_t _probing_start();
static stat_t _probing_backoff();
static stat_t _probing_finish();
static stat_t _probing_finalize_exit();
static stat_t _probing_error_exit(int8_t axis);

/**** HELPERS ***************************************************************************
 * _set_pb_func() - a convenience for setting the next dispatch vector and exiting
 */

static stat_t _set_pb_func(uint8_t (*func)())
{
	pb.func = func;
	return (STAT_EAGAIN);
}

/***********************************************************************************
 **** G38.2 Probing Cycle ***********************************************************
 ***********************************************************************************/

/****************************************************************************************
 * cm_probing_cycle_start()		- G38.2 homing cycle using limit switches
 * cm_probing_cycle_callback()	- main loop callback for running the probing cycle
 *
 *	All cm_probe_cycle_start does is prevent any new commands from queueing to the
 *	planner so that the planner can move to a stop and report MACHINE_PROGRAM_STOP.
 *	OK, it also queues the function that's called once motion has stopped.
 *
 *  NOTE: it is *not* an error condition for the probe not to trigger.
 *  it is an error for the limit or homing switches to fire,
 *  or if there is some other configuration error.
 *
 *	--- Some further details ---
 *
 *	Note: When coding a cycle (like this one) you get to perform one queued
 *	move per entry into the continuation, then you must exit.
 *
 *	Another Note: When coding a cycle (like this one) you must wait until
 *	the last move has actually been queued (or has finished) before declaring
 *	the cycle to be done. Otherwise there is a nasty race condition in
 *	_controller_HSM() that may accept the next command before the position of
 *	the final move has been recorded in the Gcode model. That's what the call
 *	to cm_get_runtime_busy() is about.
 */

uint8_t cm_straight_probe(float target[], bool flags[])
{
    // trap zero feed rate condition
    if (fp_ZERO(cm.gm.feed_rate)) {
        return (STAT_GCODE_FEEDRATE_NOT_SPECIFIED);
    }

    // error if no axes specified
    if (!flags[AXIS_X] && !flags[AXIS_Y] && !flags[AXIS_Z]) {
        return (STAT_GCODE_AXIS_IS_MISSING);
    }

    // set probe move endpoint
    copy_vector(pb.target, target);     // set probe move endpoint
    copy_vector(pb.flags, flags);       // set axes involved on the move
    clear_vector(cm.probe_results);     // clear the old probe position.
    // NOTE: relying on probe_result will not detect a probe to 0,0,0.

    cm.probe_state = PROBE_WAITING;     // wait until planner queue empties before completing initialization
    pb.func = _probing_init;            // bind probing initialization function
    return (STAT_OK);
}

uint8_t cm_probing_cycle_callback(void)
{
    if ((cm.cycle_state != CYCLE_PROBE) && (cm.probe_state != PROBE_WAITING)) {
        return (STAT_NOOP);         // exit if not in a probe cycle or waiting for one
    }
	if (cm_get_runtime_busy()) return (STAT_EAGAIN);    // sync to planner move ends
	return (pb.func());                                 // execute the current probing move
}

/*
 * _probing_init()	- G38.2 probing cycle using limit switches
 *
 *	These initializations are required before starting the probing cycle.
 *	They must be done after the planner has exhausted all current CYCLE moves as
 *	they affect the runtime (specifically the switch modes). Side effects would
 *	include limit switches initiating probe actions instead of just killing movement
 */

static uint8_t _probing_init()
{
    float start_position[AXES];

    // so optimistic... ;)
    // NOTE: it is *not* an error condition for the probe not to trigger.
    // it is an error for the limit or homing switches to fire, or for some other configuration error.
    cm.probe_state = PROBE_FAILED;
    cm.machine_state = MACHINE_CYCLE;
    cm.cycle_state = CYCLE_PROBE;

    // save relevant non-axis parameters from Gcode model
    pb.saved_coord_system = cm_get_coord_system(ACTIVE_MODEL);
    pb.saved_distance_mode = cm_get_distance_mode(ACTIVE_MODEL);

    // set working values
    cm_set_distance_mode(ABSOLUTE_MODE);
    cm_set_coord_system(ABSOLUTE_COORDS);   // probing is done in machine coordinates

    // initialize the axes - save the jerk settings & switch to the jerk_homing settings
    for( uint8_t axis=0; axis<AXES; axis++ ) {
        pb.saved_jerk[axis] = cm_get_axis_jerk(axis);	// save the max jerk value
        cm_set_axis_jerk(axis, cm.a[axis].jerk_high);	// use the high-speed jerk for probe
        start_position[axis] = cm_get_absolute_position(ACTIVE_MODEL, axis);
    }

    // error if the probe target is too close to the current position
    if (get_axis_vector_length(start_position, pb.target) < MINIMUM_PROBE_TRAVEL) {
        _probing_error_exit(-2);
    }

	// error if the probe target requires a move along the A/B/C axes
	for ( uint8_t axis=AXIS_A; axis<AXES; axis++ ) {
//		if (fp_NE(start_position[axis], pb.target[axis])) { // old style
		if (fp_TRUE(pb.flags[axis])) {
//		if (pb.flags[axis]) {           // will reduce to this once flags are booleans
   			_probing_error_exit(axis);
        }
	}

	// initialize the probe switch
    pb.probe_input = 5;     // TODO -- for now we hard code it to zmin
    gpio_set_probing_mode(pb.probe_input, true);

    // turn off spindle and start the move
    cm_spindle_optional_pause(true);        // pause the spindle if it's on
	return (_set_pb_func(_probing_start));	// start the probe move
}

/*
 * _probing_start() - start the probe or skip it if switch is already active
 */

static stat_t _probing_start()
{
	// initial probe state, don't probe if we're already contacted!
    int8_t probe = gpio_read_input(pb.probe_input);

    // INPUT_INACTIVE means switch is OPEN
	if ( probe == INPUT_INACTIVE ) {
		cm_straight_feed(pb.target, pb.flags);
        return (_set_pb_func(_probing_backoff));

	} else {
        cm.probe_state = PROBE_SUCCEEDED;
        return (_set_pb_func(_probing_finish));
    }
}

/*
 * _probing_backoff() - runs after the probe move, whether it contacted or not
 *
 * Back off to the measured touch position captured by encoder snapshot
 */

static stat_t _probing_backoff()
{
    // Test if we've contacted
    int8_t probe = gpio_read_input(pb.probe_input);

    // INPUT_INACTIVE means switch is OPEN (at least for now)
    if ( probe == INPUT_INACTIVE ) {
        cm.probe_state = PROBE_FAILED;

    } else {
        cm.probe_state = PROBE_SUCCEEDED;

        // capture contact position in step space and convert from steps to mm.
        // snapshot was taken by switch interrupt at the time of closure
        float contact_position[AXES];
        kn_forward_kinematics(en_get_encoder_snapshot_vector(), contact_position);

        cm_queue_flush();                               // flush queue & end feedhold
        cm_straight_feed(contact_position, pb.flags);   // NB: feed rate is the same as the probe move
    }
    return (_set_pb_func(_probing_finish));
}

/*
 * _probing_finish() - report probe results and clean up
 */

static stat_t _probing_finish()
{
    int8_t probe = gpio_read_input(pb.probe_input);
	cm.probe_state = (probe==true) ? PROBE_SUCCEEDED : PROBE_FAILED;

    // store the probe results
	for (uint8_t axis=0; axis<AXES; axis++ ) {
		cm.probe_results[axis] = cm_get_absolute_position(ACTIVE_MODEL, axis);
	}

	// If probe was successful the 'e' word == 1, otherwise e == 0 to signal an error
	printf_P(PSTR("{\"prb\":{\"e\":%i"), (int)cm.probe_state);

	if (pb.flags[AXIS_X]) { printf_P(PSTR(",\"x\":%0.3f"), cm.probe_results[AXIS_X]); }
	if (pb.flags[AXIS_Y]) { printf_P(PSTR(",\"y\":%0.3f"), cm.probe_results[AXIS_Y]); }
	if (pb.flags[AXIS_Z]) { printf_P(PSTR(",\"z\":%0.3f"), cm.probe_results[AXIS_Z]); }
	if (pb.flags[AXIS_A]) { printf_P(PSTR(",\"a\":%0.3f"), cm.probe_results[AXIS_A]); }
	if (pb.flags[AXIS_B]) { printf_P(PSTR(",\"b\":%0.3f"), cm.probe_results[AXIS_B]); }
	if (pb.flags[AXIS_C]) { printf_P(PSTR(",\"c\":%0.3f"), cm.probe_results[AXIS_C]); }

	printf_P(PSTR("}}\n"));

	return (_set_pb_func(_probing_finalize_exit));
}

/*
 * _probe_restore_settings()
 * _probing_finalize_exit()
 * _probing_error_exit()
 */

static void _probe_restore_settings()
{
    // flush queue and end feedhold (if any)
    cm_queue_flush();

    // set input back to normal operation
    gpio_set_probing_mode(pb.probe_input, false);

	// restore axis jerk
	for (uint8_t axis=0; axis<AXES; axis++) {
		cm.a[axis].jerk_max = pb.saved_jerk[axis];
    }

	// restore coordinate system and distance mode
	cm_set_coord_system(pb.saved_coord_system);
	cm_set_distance_mode(pb.saved_distance_mode);

    // restart spindle if it was paused
    cm_spindle_resume(spindle.dwell_seconds);

	// cancel the feed modes used during probing
	cm_set_motion_mode(MODEL, MOTION_MODE_CANCEL_MOTION_MODE);
	cm_canned_cycle_end();
}

static stat_t _probing_finalize_exit()
{
	_probe_restore_settings();
	return (STAT_OK);
}

static stat_t _probing_error_exit(int8_t axis)
{
	// Generate the warning message. Since the error exit returns via the probing callback
	// - and not the main controller - it requires its own display processing
	nv_reset_nv_list();
	if (axis == -2) {
		nv_add_conditional_message((const char *)"Probing error - invalid probe destination");
	} else {
		char msg[NV_MESSAGE_LEN];
		sprintf_P(msg, PSTR("Probing error - %c axis cannot move during probing"), cm_get_axis_char(axis));
		nv_add_conditional_message(msg);
	}
	nv_print_list(STAT_PROBE_CYCLE_FAILED, TEXT_INLINE_VALUES, JSON_RESPONSE_FORMAT);

	// clean up and exit
	_probe_restore_settings();
	return (STAT_PROBE_CYCLE_FAILED);
}
