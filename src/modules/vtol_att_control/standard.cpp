/****************************************************************************
 *
 *   Copyright (c) 2015-2022 PX4 Development Team. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name PX4 nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/**
 * @file standard.cpp
 *
 * @author Simon Wilks		<simon@uaventure.com>
 * @author Roman Bapst		<bapstroman@gmail.com>
 * @author Andreas Antener	<andreas@uaventure.com>
 * @author Sander Smeets	<sander@droneslab.com>
 *
*/

#include "standard.h"
#include "vtol_att_control_main.h"

#include <float.h>
#include <uORB/topics/landing_gear.h>

using namespace matrix;

Standard::Standard(VtolAttitudeControl *attc) :
	VtolType(attc)
{
	_vtol_schedule.flight_mode = vtol_mode::MC_MODE;
	_vtol_schedule.transition_start = 0;

	_mc_roll_weight = 1.0f;
	_mc_pitch_weight = 1.0f;
	_mc_yaw_weight = 1.0f;
	_mc_throttle_weight = 1.0f;
}

void
Standard::parameters_update()
{
	VtolType::updateParams();

	// make sure that pusher ramp in backtransition is smaller than back transition (max) duration
	_param_vt_b_trans_ramp.set(math::min(_param_vt_b_trans_ramp.get(), _param_vt_b_trans_dur.get()));
}

void Standard::update_vtol_state()
{
	/* After flipping the switch the vehicle will start the pusher (or tractor) motor, picking up
	 * forward speed. After the vehicle has picked up enough speed the rotors shutdown.
	 * For the back transition the pusher motor is immediately stopped and rotors reactivated.
	 */

	float mc_weight = _mc_roll_weight;
	float time_since_trans_start = (float)(hrt_absolute_time() - _vtol_schedule.transition_start) * 1e-6f;

	if (_vtol_vehicle_status->vtol_transition_failsafe) {
		// Failsafe event, engage mc motors immediately
		_vtol_schedule.flight_mode = vtol_mode::MC_MODE;
		_pusher_throttle = 0.0f;
		_reverse_output = 0.0f;

		//reset failsafe when FW is no longer requested
		if (!_attc->is_fixed_wing_requested()) {
			_vtol_vehicle_status->vtol_transition_failsafe = false;
		}

	} else if (!_attc->is_fixed_wing_requested()) {

		// the transition to fw mode switch is off
		if (_vtol_schedule.flight_mode == vtol_mode::MC_MODE) {
			// in mc mode
			_vtol_schedule.flight_mode = vtol_mode::MC_MODE;
			mc_weight = 1.0f;
			_reverse_output = 0.0f;

		} else if (_vtol_schedule.flight_mode == vtol_mode::FW_MODE) {
			// Regular backtransition
			_vtol_schedule.flight_mode = vtol_mode::TRANSITION_TO_MC;
			_vtol_schedule.transition_start = hrt_absolute_time();
			_reverse_output = _param_vt_b_rev_out.get();

		} else if (_vtol_schedule.flight_mode == vtol_mode::TRANSITION_TO_FW) {
			// failsafe back to mc mode
			_vtol_schedule.flight_mode = vtol_mode::MC_MODE;
			mc_weight = 1.0f;
			_pusher_throttle = 0.0f;
			_reverse_output = 0.0f;

		} else if (_vtol_schedule.flight_mode == vtol_mode::TRANSITION_TO_MC) {
			// speed exit condition: use ground if valid, otherwise airspeed
			bool exit_backtransition_speed_condition = false;

			if (_local_pos->v_xy_valid) {
				const Dcmf R_to_body(Quatf(_v_att->q).inversed());
				const Vector3f vel = R_to_body * Vector3f(_local_pos->vx, _local_pos->vy, _local_pos->vz);
				exit_backtransition_speed_condition = vel(0) < _param_mpc_xy_cruise.get();

			} else if (PX4_ISFINITE(_airspeed_validated->calibrated_airspeed_m_s)) {
				exit_backtransition_speed_condition = _airspeed_validated->calibrated_airspeed_m_s < _param_mpc_xy_cruise.get();
			}

			const bool exit_backtransition_time_condition = time_since_trans_start > _param_vt_b_trans_dur.get();

			if (can_transition_on_ground() || exit_backtransition_speed_condition || exit_backtransition_time_condition) {
				_vtol_schedule.flight_mode = vtol_mode::MC_MODE;
			}
		}

	} else {
		// the transition to fw mode switch is on
		if (_vtol_schedule.flight_mode == vtol_mode::MC_MODE || _vtol_schedule.flight_mode == vtol_mode::TRANSITION_TO_MC) {
			// start transition to fw mode
			/* NOTE: The failsafe transition to fixed-wing was removed because it can result in an
			 * unsafe flying state. */
			_vtol_schedule.flight_mode = vtol_mode::TRANSITION_TO_FW;
			_vtol_schedule.transition_start = hrt_absolute_time();

		} else if (_vtol_schedule.flight_mode == vtol_mode::FW_MODE) {
			// in fw mode
			_vtol_schedule.flight_mode = vtol_mode::FW_MODE;
			mc_weight = 0.0f;

		} else if (_vtol_schedule.flight_mode == vtol_mode::TRANSITION_TO_FW) {
			// continue the transition to fw mode while monitoring airspeed for a final switch to fw mode

			const bool airspeed_triggers_transition = PX4_ISFINITE(_airspeed_validated->calibrated_airspeed_m_s)
					&& !_param_fw_arsp_mode.get();
			const bool minimum_trans_time_elapsed = time_since_trans_start > getMinimumFrontTransitionTime();

			bool transition_to_fw = false;

			if (minimum_trans_time_elapsed) {
				if (airspeed_triggers_transition) {
					transition_to_fw = _airspeed_validated->calibrated_airspeed_m_s >= _param_vt_arsp_trans.get();

				} else {
					transition_to_fw = true;
				}
			}

			transition_to_fw |= can_transition_on_ground();

			if (transition_to_fw) {
				_vtol_schedule.flight_mode = vtol_mode::FW_MODE;

				// don't set pusher throttle here as it's being ramped up elsewhere
				_trans_finished_ts = hrt_absolute_time();
			}
		}
	}

	_mc_roll_weight = mc_weight;
	_mc_pitch_weight = mc_weight;
	_mc_yaw_weight = mc_weight;
	_mc_throttle_weight = mc_weight;

	// map specific control phases to simple control modes
	switch (_vtol_schedule.flight_mode) {
	case vtol_mode::MC_MODE:
		_vtol_mode = mode::ROTARY_WING;
		break;

	case vtol_mode::FW_MODE:
		_vtol_mode = mode::FIXED_WING;
		break;

	case vtol_mode::TRANSITION_TO_FW:
		_vtol_mode = mode::TRANSITION_TO_FW;
		break;

	case vtol_mode::TRANSITION_TO_MC:
		_vtol_mode = mode::TRANSITION_TO_MC;
		break;
	}
}

void Standard::update_transition_state()
{
	const hrt_abstime now = hrt_absolute_time();
	float mc_weight = 1.0f;
	const float time_since_trans_start = (float)(now - _vtol_schedule.transition_start) * 1e-6f;

	VtolType::update_transition_state();

	// we get attitude setpoint from a multirotor flighttask if climbrate is controlled.
	// in any other case the fixed wing attitude controller publishes attitude setpoint from manual stick input.
	if (_v_control_mode->flag_control_climb_rate_enabled) {
		// we need the incoming (virtual) attitude setpoints (both mc and fw) to be recent, otherwise return (means the previous setpoint stays active)
		if (_mc_virtual_att_sp->timestamp < (now - 1_s) || _fw_virtual_att_sp->timestamp < (now - 1_s)) {
			return;
		}

		memcpy(_v_att_sp, _mc_virtual_att_sp, sizeof(vehicle_attitude_setpoint_s));
		_v_att_sp->roll_body = _fw_virtual_att_sp->roll_body;

	} else {
		// we need a recent incoming (fw virtual) attitude setpoint, otherwise return (means the previous setpoint stays active)
		if (_fw_virtual_att_sp->timestamp < (now - 1_s)) {
			return;
		}

		memcpy(_v_att_sp, _fw_virtual_att_sp, sizeof(vehicle_attitude_setpoint_s));
		_v_att_sp->thrust_body[2] = -_fw_virtual_att_sp->thrust_body[0];
	}

	if (_vtol_schedule.flight_mode == vtol_mode::TRANSITION_TO_FW) {
		if (_param_vt_psher_rmp_dt.get() <= 0.0f) {
			// just set the final target throttle value
			_pusher_throttle = _param_vt_f_trans_thr.get();

		} else if (_pusher_throttle <= _param_vt_f_trans_thr.get()) {
			// ramp up throttle to the target throttle value
			_pusher_throttle = _param_vt_f_trans_thr.get() * time_since_trans_start / _param_vt_psher_rmp_dt.get();
		}

		_airspeed_trans_blend_margin = _param_vt_arsp_trans.get() - _param_vt_arsp_blend.get();

		// do blending of mc and fw controls if a blending airspeed has been provided and the minimum transition time has passed
		if (_airspeed_trans_blend_margin > 0.0f &&
		    PX4_ISFINITE(_airspeed_validated->calibrated_airspeed_m_s) &&
		    _airspeed_validated->calibrated_airspeed_m_s > 0.0f &&
		    _airspeed_validated->calibrated_airspeed_m_s >= _param_vt_arsp_blend.get() &&
		    time_since_trans_start > getMinimumFrontTransitionTime()) {

			mc_weight = 1.0f - fabsf(_airspeed_validated->calibrated_airspeed_m_s - _param_vt_arsp_blend.get()) /
				    _airspeed_trans_blend_margin;
			// time based blending when no airspeed sensor is set

		} else if (_param_fw_arsp_mode.get() || !PX4_ISFINITE(_airspeed_validated->calibrated_airspeed_m_s)) {
			mc_weight = 1.0f - time_since_trans_start / getMinimumFrontTransitionTime();
			mc_weight = math::constrain(2.0f * mc_weight, 0.0f, 1.0f);

		}

		// ramp up FW_PSP_OFF
		_v_att_sp->pitch_body = math::radians(_param_fw_psp_off.get()) * (1.0f - mc_weight);

		const Quatf q_sp(Eulerf(_v_att_sp->roll_body, _v_att_sp->pitch_body, _v_att_sp->yaw_body));
		q_sp.copyTo(_v_att_sp->q_d);

		// check front transition timeout
		if (_param_vt_trans_timeout.get() > FLT_EPSILON) {
			if (time_since_trans_start > _param_vt_trans_timeout.get()) {
				// transition timeout occured, abort transition
				_attc->quadchute(VtolAttitudeControl::QuadchuteReason::TransitionTimeout);
			}
		}

		// set spoiler and flaps to 0
		_flaps_setpoint_with_slewrate.update(0.f, _dt);
		_spoiler_setpoint_with_slewrate.update(0.f, _dt);

	} else if (_vtol_schedule.flight_mode == vtol_mode::TRANSITION_TO_MC) {

		if (_v_control_mode->flag_control_climb_rate_enabled) {
			// control backtransition deceleration using pitch.
			_v_att_sp->pitch_body = update_and_get_backtransition_pitch_sp();
		}

		const Quatf q_sp(Eulerf(_v_att_sp->roll_body, _v_att_sp->pitch_body, _v_att_sp->yaw_body));
		q_sp.copyTo(_v_att_sp->q_d);

		_pusher_throttle = 0.0f;

		if (time_since_trans_start >= _param_vt_b_rev_del.get()) {
			// Handle throttle reversal for active breaking
			float thrscale = (time_since_trans_start - _param_vt_b_rev_del.get()) / (_param_vt_psher_rmp_dt.get());
			thrscale = math::constrain(thrscale, 0.0f, 1.0f);
			_pusher_throttle = thrscale * _param_vt_b_trans_thr.get();
		}

		// continually increase mc attitude control as we transition back to mc mode
		if (_param_vt_b_trans_ramp.get() > FLT_EPSILON) {
			mc_weight = time_since_trans_start / _param_vt_b_trans_ramp.get();
		}
	}

	mc_weight = math::constrain(mc_weight, 0.0f, 1.0f);

	_mc_roll_weight = mc_weight;
	_mc_pitch_weight = mc_weight;
	_mc_yaw_weight = mc_weight;
	_mc_throttle_weight = mc_weight;
}

void Standard::update_mc_state()
{
	VtolType::update_mc_state();

	_pusher_throttle = VtolType::pusher_assist();
}

void Standard::update_fw_state()
{
	VtolType::update_fw_state();
}

/**
 * Prepare message to actuators with data from mc and fw attitude controllers. An mc attitude weighting will determine
 * what proportion of control should be applied to each of the control groups (mc and fw).
 */
void Standard::fill_actuator_outputs()
{
	auto &mc_in = _actuators_mc_in->control;
	auto &fw_in = _actuators_fw_in->control;

	auto &mc_out = _actuators_out_0->control;
	auto &fw_out = _actuators_out_1->control;

	switch (_vtol_schedule.flight_mode) {
	case vtol_mode::MC_MODE:

		// MC out = MC in
		mc_out[actuator_controls_s::INDEX_ROLL]         = mc_in[actuator_controls_s::INDEX_ROLL];
		mc_out[actuator_controls_s::INDEX_PITCH]        = mc_in[actuator_controls_s::INDEX_PITCH];
		mc_out[actuator_controls_s::INDEX_YAW]          = mc_in[actuator_controls_s::INDEX_YAW];
		mc_out[actuator_controls_s::INDEX_THROTTLE]     = mc_in[actuator_controls_s::INDEX_THROTTLE];
		mc_out[actuator_controls_s::INDEX_LANDING_GEAR] = landing_gear_s::GEAR_DOWN;

		// FW out = 0, other than roll and pitch depending on elevon lock
		fw_out[actuator_controls_s::INDEX_ROLL]         = _param_vt_elev_mc_lock.get() ? 0 :
				fw_in[actuator_controls_s::INDEX_ROLL];
		fw_out[actuator_controls_s::INDEX_PITCH]        = _param_vt_elev_mc_lock.get() ? 0 :
				fw_in[actuator_controls_s::INDEX_PITCH];
		fw_out[actuator_controls_s::INDEX_YAW]          = 0;
		fw_out[actuator_controls_s::INDEX_THROTTLE]     = _pusher_throttle;
		fw_out[actuator_controls_s::INDEX_FLAPS]        = _flaps_setpoint_with_slewrate.getState();
		fw_out[actuator_controls_s::INDEX_SPOILERS]    	= _spoiler_setpoint_with_slewrate.getState();
		fw_out[actuator_controls_s::INDEX_AIRBRAKES]    = 0.f;

		break;

	case vtol_mode::TRANSITION_TO_FW:

	// FALLTHROUGH
	case vtol_mode::TRANSITION_TO_MC:
		// MC out = MC in (weighted)
		mc_out[actuator_controls_s::INDEX_ROLL]         = mc_in[actuator_controls_s::INDEX_ROLL]     * _mc_roll_weight;
		mc_out[actuator_controls_s::INDEX_PITCH]        = mc_in[actuator_controls_s::INDEX_PITCH]    * _mc_pitch_weight;
		mc_out[actuator_controls_s::INDEX_YAW]          = mc_in[actuator_controls_s::INDEX_YAW]      * _mc_yaw_weight;
		mc_out[actuator_controls_s::INDEX_THROTTLE]     = mc_in[actuator_controls_s::INDEX_THROTTLE] * _mc_throttle_weight;
		mc_out[actuator_controls_s::INDEX_LANDING_GEAR] = landing_gear_s::GEAR_UP;

		// FW out = FW in, with VTOL transition controlling throttle and airbrakes
		fw_out[actuator_controls_s::INDEX_ROLL]         = fw_in[actuator_controls_s::INDEX_ROLL];
		fw_out[actuator_controls_s::INDEX_PITCH]        = fw_in[actuator_controls_s::INDEX_PITCH];
		fw_out[actuator_controls_s::INDEX_YAW]          = fw_in[actuator_controls_s::INDEX_YAW];
		fw_out[actuator_controls_s::INDEX_THROTTLE]     = _pusher_throttle;
		fw_out[actuator_controls_s::INDEX_FLAPS]        = _flaps_setpoint_with_slewrate.getState();
		fw_out[actuator_controls_s::INDEX_SPOILERS]    	= _spoiler_setpoint_with_slewrate.getState();
		fw_out[actuator_controls_s::INDEX_AIRBRAKES]    = _reverse_output;

		break;

	case vtol_mode::FW_MODE:
		// MC out = 0
		mc_out[actuator_controls_s::INDEX_ROLL]         = 0;
		mc_out[actuator_controls_s::INDEX_PITCH]        = 0;
		mc_out[actuator_controls_s::INDEX_YAW]          = 0;
		mc_out[actuator_controls_s::INDEX_THROTTLE]     = 0;
		mc_out[actuator_controls_s::INDEX_LANDING_GEAR] = landing_gear_s::GEAR_UP;

		// FW out = FW in
		fw_out[actuator_controls_s::INDEX_ROLL]         = fw_in[actuator_controls_s::INDEX_ROLL];
		fw_out[actuator_controls_s::INDEX_PITCH]        = fw_in[actuator_controls_s::INDEX_PITCH];
		fw_out[actuator_controls_s::INDEX_YAW]          = fw_in[actuator_controls_s::INDEX_YAW];
		fw_out[actuator_controls_s::INDEX_THROTTLE]     = fw_in[actuator_controls_s::INDEX_THROTTLE];
		fw_out[actuator_controls_s::INDEX_FLAPS]        = _flaps_setpoint_with_slewrate.getState();
		fw_out[actuator_controls_s::INDEX_SPOILERS]    	= _spoiler_setpoint_with_slewrate.getState();
		fw_out[actuator_controls_s::INDEX_AIRBRAKES]    = 0;
		break;
	}

	_torque_setpoint_0->timestamp = hrt_absolute_time();
	_torque_setpoint_0->timestamp_sample = _actuators_mc_in->timestamp_sample;
	_torque_setpoint_0->xyz[0] = mc_out[actuator_controls_s::INDEX_ROLL];
	_torque_setpoint_0->xyz[1] = mc_out[actuator_controls_s::INDEX_PITCH];
	_torque_setpoint_0->xyz[2] = mc_out[actuator_controls_s::INDEX_YAW];

	_torque_setpoint_1->timestamp = hrt_absolute_time();
	_torque_setpoint_1->timestamp_sample = _actuators_fw_in->timestamp_sample;
	_torque_setpoint_1->xyz[0] = fw_out[actuator_controls_s::INDEX_ROLL];
	_torque_setpoint_1->xyz[1] = fw_out[actuator_controls_s::INDEX_PITCH];
	_torque_setpoint_1->xyz[2] = fw_out[actuator_controls_s::INDEX_YAW];

	_thrust_setpoint_0->timestamp = hrt_absolute_time();
	_thrust_setpoint_0->timestamp_sample = _actuators_mc_in->timestamp_sample;
	_thrust_setpoint_0->xyz[0] = fw_out[actuator_controls_s::INDEX_THROTTLE];
	_thrust_setpoint_0->xyz[1] = 0.f;
	_thrust_setpoint_0->xyz[2] = -mc_out[actuator_controls_s::INDEX_THROTTLE];

	_thrust_setpoint_1->timestamp = hrt_absolute_time();
	_thrust_setpoint_1->timestamp_sample = _actuators_fw_in->timestamp_sample;
	_thrust_setpoint_1->xyz[0] = 0.f;
	_thrust_setpoint_1->xyz[1] = 0.f;
	_thrust_setpoint_1->xyz[2] = 0.f;

	_actuators_out_0->timestamp_sample = _actuators_mc_in->timestamp_sample;
	_actuators_out_1->timestamp_sample = _actuators_fw_in->timestamp_sample;

	_actuators_out_0->timestamp = _actuators_out_1->timestamp = hrt_absolute_time();
}

void
Standard::waiting_on_tecs()
{
	// keep thrust from transition
	_v_att_sp->thrust_body[0] = _pusher_throttle;
};

void Standard::blendThrottleAfterFrontTransition(float scale)
{
	const float tecs_throttle = _v_att_sp->thrust_body[0];
	_v_att_sp->thrust_body[0] = scale * tecs_throttle + (1.0f - scale) * _pusher_throttle;
}
