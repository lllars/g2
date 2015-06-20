/*
 * planner.h - cartesian trajectory planning and motion execution
 * This file is part of the TinyG project
 *
 * Copyright (c) 2013 - 2015 Alden S. Hart, Jr.
 * Copyright (c) 2013 - 2015 Robert Giseburt
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

#ifndef PLANNER_H_ONCE
#define PLANNER_H_ONCE

#include "canonical_machine.h"	// used for GCodeState_t

/*
 * Enums and other type definitions
 */

typedef void (*cm_exec_t)(float[], bool[]); // callback to canonical_machine execution function

typedef enum {                      // bf->buffer_state values
    MP_BUFFER_EMPTY = 0,            // struct is available for use (MUST BE 0)
    MP_BUFFER_PLANNING,             // being written ("checked out") for planning
    MP_BUFFER_QUEUED,               // in queue
    MP_BUFFER_RUNNING               // current running buffer
} mpBufferState;

typedef enum {				        // bf->move_type values
    MOVE_TYPE_NULL = 0,		        // null move - does a no-op
    MOVE_TYPE_ALINE,		        // acceleration planned line
    MOVE_TYPE_DWELL,                // delay with no movement
    MOVE_TYPE_COMMAND,              // general command
    MOVE_TYPE_TOOL,                 // T command
    MOVE_TYPE_SPINDLE_SPEED,        // S command
    MOVE_TYPE_STOP,                 // program stop
    MOVE_TYPE_END                   // program end
} moveType;

typedef enum {
    MOVE_OFF = 0,                   // move inactive (MUST BE ZERO)
    MOVE_NEW,                       // general value if you need an initialization
    MOVE_RUN                        // general run state (for non-acceleration moves)
} moveState;

typedef enum {
    SECTION_HEAD = 0,               // acceleration
    SECTION_BODY,                   // cruise
    SECTION_TAIL                    // deceleration
} moveSection;
#define SECTIONS 3

typedef enum {
    SECTION_OFF = 0,                // section inactive
    SECTION_NEW,                    // uninitialized section
    SECTION_1st_HALF,               // first half of S curve
    SECTION_2nd_HALF                // second half of S curve or running a BODY (cruise)
} sectionState;

/*** Most of these factors are the result of a lot of tweaking. Change with caution.***/

/* PLANNER_BUFFER_POOL_SIZE
 *	Should be at least the number of buffers requires to support optimal
 *	planning in the case of very short lines or arc segments.
 *	Suggest 12 min. Limit is 255
 */
#define PLANNER_BUFFER_POOL_SIZE 28
#define PLANNER_BUFFER_HEADROOM 4                   // buffers to reserve in planner before processing new input line

#define JERK_MULTIPLIER			((float)1000000)	// DO NOT CHANGE - must always be 1 million
#define JERK_MATCH_TOLERANCE	((float)1000)		// precision to which jerk must match to be considered effectively the same

#define MIN_SEGMENT_USEC 		((float)750)		// minimum segment time (also minimum move time)
#define NOM_SEGMENT_USEC 		((float)1500)		// nominal segment time

#define MIN_PLANNED_USEC		((float)20000)		// minimum time in the planner below which we must replan immediately
#define PHAT_CITY_USEC			((float)80000)		// if you have at least this much time in the planner,

// Note that PLANNER_TIMEOUT is in milliseconds (seconds/1000), not microseconds (usec) like the above!
#define PLANNER_TIMEOUT_MS		(50)				// Max amount of time to wait between replans
// PLANNER_TIMEOUT should be < (MIN_PLANNED_USEC/1000) - (max time to replan)
// ++++++++ NOT SURE THIS IS STILL OPERATIVE ++++++++ ash)

//#define CORNER_TIME_QUANTUM 0.0000025               // (1.0/400000.0)  // one clock tick
#define JUNCTION_AGGRESSION     0.25               // Actually this # divided by 1 million

//*** derived definitions - do not change ***
#define MIN_SEGMENT_TIME 		(MIN_SEGMENT_USEC / MICROSECONDS_PER_MINUTE)
#define NOM_SEGMENT_TIME 		(NOM_SEGMENT_USEC / MICROSECONDS_PER_MINUTE)
#define MIN_PLANNED_TIME        (MIN_PLANNED_USEC / MICROSECONDS_PER_MINUTE)
#define PHAT_CITY_TIME          (PHAT_CITY_USEC / MICROSECONDS_PER_MINUTE)
#define MIN_SEGMENT_TIME_PLUS_MARGIN ((MIN_SEGMENT_USEC+1) / MICROSECONDS_PER_MINUTE)

# if 0
// THESE ARE NO LONGER USED -- but the code that uses them is still conditional in plan_zoid.cpp
/* Some parameters for _generate_trapezoid()
 * TRAPEZOID_ITERATION_MAX	 				Max iterations for convergence in the HT asymmetric case.
 * TRAPEZOID_ITERATION_ERROR_PERCENT		Error percentage for iteration convergence. As percent - 0.01 = 1%
 * TRAPEZOID_LENGTH_FIT_TOLERANCE			Tolerance for "exact fit" for H and T cases
 * TRAPEZOID_VELOCITY_TOLERANCE				Adaptive velocity tolerance term
 */
#define TRAPEZOID_ITERATION_MAX				10
#define TRAPEZOID_ITERATION_ERROR_PERCENT	((float)0.10)
#define TRAPEZOID_LENGTH_FIT_TOLERANCE		((float)0.0001)	// allowable mm of error in planning phase
#endif //0

#define TRAPEZOID_VELOCITY_TOLERANCE		(max(2.0,bf->entry_velocity/100.0))

/*
 *	Planner structures
 */

// All the enums that equal zero must be zero. Don't change this

typedef struct mpBuffer {           // See Planning Velocity Notes for variable usage
	struct mpBuffer *pv;            // static pointer to previous buffer
	struct mpBuffer *nx;            // static pointer to next buffer

    // If you rearrange this structure, you *MUST* change mp_clear_buffer!!
	stat_t (*bf_func)(struct mpBuffer *bf); // callback to buffer exec function
	cm_exec_t cm_func;              // callback to canonical machine execution function

	mpBufferState buffer_state;     // used to manage queuing/dequeuing
	moveType move_type;             // used to dispatch to run routine
	moveState move_state;           // move state machine sequence
	uint8_t move_code;              // byte that can be used by used exec functions
	bool replannable;               // TRUE if move can be re-planned
    bool locked;                    // TRUE if the move is locked from replanning

	float unit[AXES];				// unit vector for axis scaling & planning
    bool unit_flags[AXES];          // set true for axes participating in the move
    bool flag_vector[AXES];

	float length;					// total length of line or helix in mm
	float head_length;
	float body_length;
	float tail_length;
									// *** SEE NOTES ON THESE VARIABLES, in aline() ***
	float entry_velocity;			// entry velocity requested for the move
	float cruise_velocity;			// cruise velocity requested & achieved
	float exit_velocity;			// exit velocity requested for the move

	float entry_vmax;				// max junction velocity at entry of this move
	float cruise_vmax;				// max cruise velocity requested for move
	float exit_vmax;				// max exit velocity possible (redundant)
	float delta_vmax;				// max velocity difference for this move
	float braking_velocity;			// current value for braking velocity

	uint8_t jerk_axis;				// rate limiting axis used to compute jerk for the move
	float jerk;						// maximum linear jerk term for this move
	float recip_jerk;				// 1/Jm used for planning (computed and cached)
	float cbrt_jerk;				// cube root of Jm used for planning (computed and cached)

    float real_move_time;          // amount of time it'll take for the move, in us

	GCodeState_t gm;				// Gcode model state - passed from model, used by planner and runtime

} mpBuf_t;

typedef struct mpBufferPool {		// ring buffer for sub-moves
	magic_t magic_start;			// magic number to test memory integrity
	uint8_t buffers_available;		// running count of available buffers
	mpBuf_t *w;						// get_write_buffer pointer
	mpBuf_t *q;						// queue_write_buffer pointer
	mpBuf_t *r;						// get/end_run_buffer pointer
    bool needs_replanned;           // mark to indicate that at least one ALINE was put in the buffer
    bool needs_time_accounting;     // mark to indicate that the buffer has changed and the times (below) may be wrong
    bool planning;                  // the planner marks this to indicate it's (re)planning the block list
    bool force_replan;              // true to indicate that we must plan, ignoring the normal timing tests

    volatile float time_in_run;		// time left in the buffer executed by the runtime
    volatile float time_in_planner;	// total time of the buffer

    uint32_t planner_timer;         // timout to compare against SysTickTimer.getValue() to know when to force planning

	mpBuf_t bf[PLANNER_BUFFER_POOL_SIZE];// buffer storage
	magic_t magic_end;
} mpBufferPool_t;

typedef struct mpMoveMasterSingleton { // common variables for planning (move master)
	magic_t magic_start;			// magic number to test memory integrity
	float position[AXES];			// final move position for planning purposes

	float jerk;						// jerk values cached from previous block
	float recip_jerk;
	float cbrt_jerk;

	magic_t magic_end;
} mpMoveMasterSingleton_t;

typedef struct mpMoveRuntimeSingleton {	// persistent runtime variables
//	uint8_t (*run_move)(struct mpMoveRuntimeSingleton *m); // currently running move - left in for reference
	magic_t magic_start;                // magic number to test memory integrity
	moveState move_state;               // state of the overall move
	moveSection section;                // what section is the move in?
	sectionState section_state;         // state within a move section

	float unit[AXES];                   // unit vector for axis scaling & planning
	float target[AXES];                 // final target for bf (used to correct rounding errors)
	float position[AXES];               // current move position
	float waypoint[SECTIONS][AXES];     // head/body/tail endpoints for correction

	float target_steps[MOTORS];         // current MR target (absolute target as steps)
	float position_steps[MOTORS];       // current MR position (target from previous segment)
	float commanded_steps[MOTORS];      // will align with next encoder sample (target from 2nd previous segment)
	float encoder_steps[MOTORS];        // encoder position in steps - ideally the same as commanded_steps
	float following_error[MOTORS];      // difference between encoder_steps and commanded steps

	float head_length;                  // copies of bf variables of same name
	float body_length;
	float tail_length;

	float entry_velocity;               // actual velocities for the move
	float cruise_velocity;
	float exit_velocity;

	float segments;                     // number of segments in line (also used by arc generation)
	uint32_t segment_count;             // count of running segments
	float segment_velocity;             // computed velocity for aline segment
	float segment_time;                 // actual time increment per aline segment
	float jerk;                         // max linear jerk

	float forward_diff_1;               // forward difference level 1
	float forward_diff_2;               // forward difference level 2
	float forward_diff_3;               // forward difference level 3
	float forward_diff_4;               // forward difference level 4
	float forward_diff_5;               // forward difference level 5

	GCodeState_t gm;                    // gcode model state currently executing

	magic_t magic_end;
} mpMoveRuntimeSingleton_t;

// Reference global scope structures
extern mpBufferPool_t mb;               // move buffer queue
extern mpMoveMasterSingleton_t mm;      // context for line planning
extern mpMoveRuntimeSingleton_t mr;     // context for line runtime

/*
 * Global Scope Functions
 */

//planner.c functions

void planner_init(void);
void planner_reset(void);
void planner_init_assertions(void);
stat_t planner_test_assertions(void);

void mp_halt_runtime(void);
void mp_flush_planner(void);
void mp_set_planner_position(uint8_t axis, const float position);
void mp_set_runtime_position(uint8_t axis, const float position);
void mp_set_steps_to_runtime_position(void);

//void mp_queue_command(void(*cm_exec_t)(float[], float[]), float *value, float *flag);
void mp_queue_command(void(*cm_exec_t)(float[], bool[]), float *value, bool *flag);
stat_t mp_runtime_command(mpBuf_t *bf);

stat_t mp_dwell(const float seconds);
void mp_end_dwell(void);
void mp_request_out_of_band_dwell(float seconds);
stat_t mp_exec_out_of_band_dwell(void);

void mp_init_buffers(void);                             // planner buffer handlers...
uint8_t mp_get_planner_buffers_available(void);
mpBuf_t * mp_get_write_buffer(void);
void mp_commit_write_buffer(const moveType move_type);
bool mp_has_runnable_buffer();
mpBuf_t * mp_get_run_buffer(void);
bool mp_free_run_buffer(void);

//mpBuf_t * mp_get_prev_buffer(const mpBuf_t *bf);      // Use macro below instead
//mpBuf_t * mp_get_next_buffer(const mpBuf_t *bf);      // Use macro below instead
mpBuf_t * mp_get_first_buffer(void);
//void mp_unget_write_buffer(void);                     // UNUSED
//mpBuf_t * mp_get_last_buffer(void);                   // UNUSED
//void mp_copy_buffer(mpBuf_t *bf, const mpBuf_t *bp);  // UNUSED

#define mp_get_prev_buffer(b) ((mpBuf_t *)(b->pv))
#define mp_get_next_buffer(b) ((mpBuf_t *)(b->nx))

stat_t mp_plan_buffer();                                // planner functions and helpers...
bool mp_is_it_phat_city_time();

// plan_line.c functions

void mp_zero_segment_velocity(void);                    // getters and setters...
float mp_get_runtime_velocity(void);
float mp_get_runtime_absolute_position(uint8_t axis);
void mp_set_runtime_work_offset(float offset[]);
float mp_get_runtime_work_position(uint8_t axis);
uint8_t mp_get_runtime_busy(void);
bool mp_runtime_is_idle(void);

stat_t mp_aline(GCodeState_t *gm_in);                   // line planning...
void mp_plan_block_list(mpBuf_t *bf);
void mp_reset_replannable_list(void);

// plan_zoid.c functions
void mp_calculate_trapezoid(mpBuf_t *bf);
float mp_get_target_length(const float Vi, const float Vf, const mpBuf_t *bf);
float mp_get_meet_velocity(const float v_0, const float v_2, const float L, const mpBuf_t *bf);
float mp_get_target_velocity(const float Vi, const float L, const mpBuf_t *bf);

// plan_exec.c functions
stat_t mp_exec_move(void);
stat_t mp_exec_aline(mpBuf_t *bf);
void mp_exit_hold_state(void);

#endif	// End of include Guard: PLANNER_H_ONCE
