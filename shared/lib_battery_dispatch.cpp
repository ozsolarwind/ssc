/*******************************************************************************************************
*  Copyright 2017 Alliance for Sustainable Energy, LLC
*
*  NOTICE: This software was developed at least in part by Alliance for Sustainable Energy, LLC
*  (�Alliance�) under Contract No. DE-AC36-08GO28308 with the U.S. Department of Energy and the U*  The Government retains for itself and others acting on its behalf a nonexclusive, paid-up,
*  irrevocable worldwide license in the software to reproduce, prepare derivative works, distribute
*  copies to the public, perform publicly and display publicly, and to permit others to do so.
*
*  Redistribution and use in source and binary forms, with or without modification, are permitted
*  provided that the following conditions are met:
*
*  1. Redistributions of source code must retain the above copyright notice, the above government
*  rights notice, this list of conditions and the following disclaimer.
*
*  2. Redistributions in binary form must reproduce the above copyright notice, the above government
*  rights notice, this list of conditions and the following disclaimer in the documentation and/or
*  other materials provided with the distribution.
*
*  3. The entire corresponding source code of any redistribution, with or without modification, by a
*  research entity, including but not limited to any contracting manager/operator of a United States
*  National Laboratory, any institution of higher learning, and any non-profit organization, must be
*  made publicly available under this license for as long as the redistribution is made available by
*  the research entity.
*
*  4. Redistribution of this software, without modification, must refer to the software by the same
*  designation. Redistribution of a modified version of this software (i) may not refer to the modified
*  version by the same designation, or by any confusingly similar designation, and (ii) must refer to
*  the underlying software originally provided by Alliance as �System Advisor Model� or �SAM�.*  to comply with the foregoing, the terms �System Advisor Model�, �SAM�, or any confusingly *  designation may not be used to refer to any modified version of this software or any modified
*  version of the underlying software originally provided by Alliance without the prior written consent
*  of Alliance.
*
*  5. The name of the copyright holder, contributors, the United States Government, the United States
*  Department of Energy, or any of their employees may not be used to endorse or promote products
*  derived from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
*  IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
*  FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER,
*  CONTRIBUTORS, UNITED STATES GOVERNMENT OR UNITED STATES DEPARTMENT OF ENERGY, NOR ANY OF THEIR
*  EMPLOYEES, BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
*  DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
*  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
*  IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
*  THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************************************/

#include "lib_battery_dispatch.h"

#include <math.h>
#include <algorithm>
#include <numeric>

/*
Dispatch base class
*/
dispatch_t::dispatch_t(battery_t * Battery, double dt_hour, double SOC_min, double SOC_max, int current_choice, double Ic_max, double Id_max, double Pc_max, double Pd_max,
	double t_min, int mode, int pv_dispatch)
{
	_Battery = Battery;
	_Battery_initial = new battery_t(*_Battery);
	init(_Battery, dt_hour, SOC_min, SOC_max, current_choice, Ic_max, Id_max, Pc_max, Pd_max, t_min, mode, pv_dispatch);

	// initialize battery power flow 
	std::unique_ptr<BatteryPowerFlow> tmp(new BatteryPowerFlow());
	m_batteryPowerFlow = std::move(tmp);
	m_batteryPower = m_batteryPowerFlow->getBatteryPower();
}

void dispatch_t::init(battery_t * Battery, double dt_hour, double SOC_min, double SOC_max, int current_choice, double Ic_max, double Id_max, double Pc_max, double Pd_max, double t_min, int mode, int pv_dispatch)
{
	_dt_hour = dt_hour; 
	_SOC_min = SOC_min;
	_SOC_max = SOC_max;
	_current_choice = current_choice;
	_Ic_max = Ic_max;
	_Id_max = Id_max;
	_Pc_max = Pc_max;
	_Pd_max = Pd_max;
	_t_min = t_min;
	_mode = mode;
	_pv_dispatch_to_battery_first = pv_dispatch;

	// limit the switch from charging to discharge so that doesn't flip-flop subhourly
	_t_at_mode = 1000;
	_prev_charging = false;
	_charging = false;
	_e_max = Battery->battery_voltage()*Battery->battery_charge_maximum()*util::watt_to_kilowatt*0.01*(SOC_max - SOC_min);
	_grid_recharge = false;

	// initialize battery operation 
	_can_charge = false;
	_can_clip_charge = false;
	_can_discharge = false;
	_can_grid_charge = false;

	// initialize powerflow model
	m_batteryPower->powerBatteryChargeMax = _Pc_max;
	m_batteryPower->powerBatteryChargeMax = _Pd_max;
}


void dispatch_t::prepareDispatch(size_t, size_t, double P_system, double P_system_clipping, double P_load_ac)
{
	_charging = true;

	m_batteryPower->powerPV = P_system;
	m_batteryPower->powerPVClipped = P_system_clipping;
	m_batteryPower->powerLoad = P_load_ac;
}

// deep copy
dispatch_t::dispatch_t(const dispatch_t& dispatch)
{
	_Battery = new battery_t(*dispatch._Battery);
	_Battery_initial = new battery_t(*dispatch._Battery_initial);
	init(_Battery, dispatch._dt_hour, dispatch._SOC_min, dispatch._SOC_max, dispatch._current_choice, dispatch._Ic_max, dispatch._Id_max, dispatch._Pc_max, dispatch._Pd_max, dispatch._t_min, dispatch._mode, dispatch._pv_dispatch_to_battery_first);
}

// shallow copy from dispatch to this
void dispatch_t::copy(const dispatch_t * dispatch)
{
	_Battery->copy(dispatch->_Battery);
	_Battery_initial->copy(dispatch->_Battery_initial);
	init(_Battery, dispatch->_dt_hour, dispatch->_SOC_min, dispatch->_SOC_max, dispatch->_current_choice, dispatch->_Ic_max, dispatch->_Id_max, dispatch->_Pc_max, dispatch->_Pc_max, dispatch->_t_min, dispatch->_mode, dispatch->_pv_dispatch_to_battery_first);
}
void dispatch_t::delete_clone()
{
	// need to delete both, since allocated memory for both in deep copy 
	if (_Battery) delete _Battery;
	if (_Battery_initial) delete _Battery_initial;
}
dispatch_t::~dispatch_t()
{
	// original _Battery doesn't need deleted, since was a pointer passed in
	_Battery_initial->delete_clone();
	delete _Battery_initial;
}
bool dispatch_t::check_constraints(double &I, int count)
{
	bool iterate = true;
	double I_initial = I;

	// decrease the current draw if took too much
	if (I > 0 && _Battery->battery_soc() < _SOC_min - tolerance)
	{
		double dQ = 0.01 * (_SOC_min - _Battery->battery_soc()) * _Battery->battery_charge_maximum_thermal();
		I -= dQ / _dt_hour;
	}
	// decrease the current charging if charged too much
	else if (I < 0 &&_Battery->battery_soc() > _SOC_max + tolerance)
	{
		double dQ = 0.01 * (_Battery->battery_soc() - _SOC_max) * _Battery->battery_charge_maximum_thermal();
		I += dQ / _dt_hour;
	}
	// Don't allow grid charging unless explicitly allowed (reduce charging) 
	else if (I < 0 && m_batteryPower->powerGridToBattery > tolerance && !_can_grid_charge)
	{
		if (fabs(m_batteryPower->powerBattery) < tolerance)
			I += (m_batteryPower->powerGridToBattery * util::kilowatt_to_watt / _Battery->battery_voltage());
		else
			I += (m_batteryPower->powerGridToBattery / fabs(m_batteryPower->powerBattery)) *fabs(I);
	}
	else
		iterate = false;

	// don't allow any changes to violate current limits
	bool current_iterate = restrict_current(I);

	// don't allow any changes to violate power limites
	bool power_iterate = restrict_power(I);

	// iterate if any of the conditions are met
	if (iterate || current_iterate || power_iterate)
		iterate = true;

	// stop iterating after 5 tries
	if (count > 5)
		iterate = false;

	// don't allow battery to flip from charging to discharging or vice versa
	if (fabs(I) > tolerance && (I_initial / I) < 0) {
		I = 0;
		iterate = false;
	}

	// reset
	if (iterate)
	{
		_Battery->copy(_Battery_initial);
		m_batteryPower->powerBattery = 0;
		m_batteryPower->powerGridToBattery = 0;
		m_batteryPower->powerBatteryToGrid = 0;
		m_batteryPower->powerPVToGrid = 0;
	}

	return iterate;
}
message dispatch_t::get_messages(){ return _message; };

void dispatch_t::switch_controller()
{
	// Implement rapid switching check
	if (_charging != _prev_charging)
	{
		if (_t_at_mode <= _t_min)
		{
			m_batteryPower->powerBattery = 0.;
			_charging = _prev_charging;
			_t_at_mode += (int)(round(_dt_hour * util::hour_to_min));
		}
		else
			_t_at_mode = 0;
	}
	_t_at_mode += (int)(round(_dt_hour * util::hour_to_min));
}
double dispatch_t::current_controller(double battery_voltage)
{
	double P, I = 0.; // [W],[V]
	P = util::kilowatt_to_watt*m_batteryPower->powerBattery;
	I = P / battery_voltage;
	restrict_current(I);
	return I;
}
bool dispatch_t::restrict_current(double &I)
{
	bool iterate = false;
	if (_current_choice == RESTRICT_CURRENT || _current_choice == RESTRICT_BOTH)
	{
		if (I < 0)
		{
			if (fabs(I) > _Ic_max)
			{
				I = -_Ic_max;
				iterate = true;
			}
		}
		else
		{
			if (I > _Id_max)
			{
				I = _Id_max;
				iterate = true;
			}
		}
	}
	return iterate;
}
bool dispatch_t::restrict_power(double &I)
{
	bool iterate = false;
	if (_current_choice == RESTRICT_POWER || _current_choice == RESTRICT_BOTH)
	{
		m_batteryPower->powerBattery = I * _Battery->battery_voltage() * util::watt_to_kilowatt;
		double dP = 0;

		// charging
		if (m_batteryPower->powerBattery < 0)
		{
			if (fabs(m_batteryPower->powerBattery) > _Pc_max * (1 + low_tolerance))
			{
				dP = fabs(_Pc_max - fabs(m_batteryPower->powerBattery));

				// increase (reduce) charging magnitude by percentage
				I -= (dP / fabs(m_batteryPower->powerBattery)) * I;
				iterate = true;
			}
		}
		else
		{
			if (fabs(m_batteryPower->powerBattery) > _Pd_max * (1 + low_tolerance))
			{
				dP = fabs(_Pd_max - m_batteryPower->powerBattery);

				// decrease discharging magnitude
				I -= (dP / fabs(m_batteryPower->powerBattery)) * I;
				iterate = true;
			}
		}
	}
	return iterate;
}
/*
Manual Dispatch
*/
dispatch_manual_t::dispatch_manual_t(battery_t * Battery, double dt, double SOC_min, double SOC_max, int current_choice, double Ic_max, double Id_max, double Pc_max, double Pd_max,
	double t_min, int mode, int pv_dispatch,
	util::matrix_t<size_t> dm_dynamic_sched, util::matrix_t<size_t> dm_dynamic_sched_weekend,
	std::vector<bool> dm_charge, std::vector<bool> dm_discharge, std::vector<bool> dm_gridcharge, std::map<size_t, double>  dm_percent_discharge, std::map<size_t, double>  dm_percent_gridcharge)
	: dispatch_t(Battery, dt, SOC_min, SOC_max, current_choice, Ic_max, Id_max,Pc_max, Pd_max,
	t_min, mode, pv_dispatch)
{
	init_with_vects(dm_dynamic_sched, dm_dynamic_sched_weekend, dm_charge, dm_discharge, dm_gridcharge, dm_percent_discharge, dm_percent_gridcharge);
}

void dispatch_manual_t::init_with_vects(
	util::matrix_t<size_t> dm_dynamic_sched,
	util::matrix_t<size_t> dm_dynamic_sched_weekend,
	std::vector<bool> dm_charge,
	std::vector<bool> dm_discharge,
	std::vector<bool> dm_gridcharge,
	std::map<size_t, double> dm_percent_discharge,
	std::map<size_t, double> dm_percent_gridcharge)
{
	_sched = dm_dynamic_sched;
	_sched_weekend = dm_dynamic_sched_weekend;
	_charge_array = dm_charge;
	_discharge_array = dm_discharge;
	_gridcharge_array = dm_gridcharge;
	_percent_discharge_array = dm_percent_discharge;
	_percent_charge_array = dm_percent_gridcharge;
}

// deep copy from dispatch to this
dispatch_manual_t::dispatch_manual_t(const dispatch_t & dispatch) : 
dispatch_t(dispatch)
{
	const dispatch_manual_t * tmp = dynamic_cast<const dispatch_manual_t *>(&dispatch);
	init_with_vects(tmp->_sched, tmp->_sched_weekend, tmp->_charge_array, tmp->_discharge_array, tmp->_gridcharge_array, tmp->_percent_discharge_array, tmp->_percent_charge_array);
}

// shallow copy from dispatch to this
void dispatch_manual_t::copy(const dispatch_t * dispatch)
{
	dispatch_t::copy(dispatch);
	const dispatch_manual_t * tmp = dynamic_cast<const dispatch_manual_t *>(dispatch);
	init_with_vects(tmp->_sched, tmp->_sched_weekend, tmp->_charge_array, tmp->_discharge_array, tmp->_gridcharge_array, tmp->_percent_discharge_array, tmp->_percent_charge_array);
}

void dispatch_manual_t::prepareDispatch(size_t hour_of_year, size_t step, double P_system, double P_system_clipping, double P_load_ac)
{
	dispatch_t::prepareDispatch(hour_of_year, step, P_system, P_system_clipping, P_load_ac);

	size_t m, h;
	util::month_hour(hour_of_year, m, h);
	size_t column = h - 1;
	size_t iprofile = 0;

	bool is_weekday = util::weekday(hour_of_year);
	if (!is_weekday && _mode == MANUAL)
		iprofile = _sched_weekend(m - 1, column);
	else
		iprofile = _sched(m - 1, column);  // 1-based

	_can_charge = _charge_array[iprofile - 1];
	_can_discharge = _discharge_array[iprofile - 1];
	_can_grid_charge = _gridcharge_array[iprofile - 1];
	_percent_discharge = 0.;
	_percent_charge = 0.;

	if (_can_discharge){ _percent_discharge = _percent_discharge_array[iprofile]; }
	if (_can_charge){ _percent_charge = 100.; }
	if (_can_grid_charge){ _percent_charge = _percent_charge_array[iprofile]; }

	m_batteryPower->canDischarge = _can_discharge;
	m_batteryPower->canPVCharge = _can_charge;
	m_batteryPower->canGridCharge = _can_grid_charge;
}
void dispatch_manual_t::dispatch(size_t year,
	size_t hour_of_year,
	size_t step,
	double P_system,
	double P_system_clipping_dc,
	double P_load_ac)
{
	prepareDispatch(hour_of_year, step, P_system, P_system_clipping_dc, P_load_ac);
														
	// Initialize power flow model
	m_batteryPowerFlow->initialize();

	// Controllers
	SOC_controller();
	switch_controller();
	double I = current_controller(_Battery->battery_voltage_nominal());

	// Iteration variables
	_Battery_initial->copy(_Battery);
	bool iterate = true;
	int count = 0;
	size_t idx = util::index_year_hour_step(year, hour_of_year, step, (1 / _dt_hour));

	do {
		 
		// Run Battery Model to update charge based on charge/discharge
		_Battery->run(idx, I);

		// Update how much power was actually used to/from battery
		I = _Battery->capacity_model()->I();
		double battery_voltage_new = _Battery->battery_voltage();
		m_batteryPower->powerBattery = I * battery_voltage_new * util::watt_to_kilowatt;// [kW]

		// Update power flow calculations and check the constraints
		m_batteryPowerFlow->calculate();
		iterate = check_constraints(I, count);
		m_batteryPower->powerBattery = I * _Battery->battery_voltage() * util::watt_to_kilowatt;
		count++;

	} while (iterate);

	// finalize power flow calculation and update for next step
	m_batteryPowerFlow->calculate();
	_prev_charging = _charging;
}

bool dispatch_manual_t::check_constraints(double &I, int count)
{
	// check common constraints before checking manual dispatch specific ones
	bool iterate = dispatch_t::check_constraints(I, count);
	

	if (!iterate)
	{
		double I_initial = I;
		iterate = true;
		bool front_of_meter = false;
		if (dispatch_manual_front_of_meter_t * dispatch = dynamic_cast<dispatch_manual_front_of_meter_t*>(this))
			front_of_meter = true;

		// Don't let PV export to grid if can still charge battery (increase charging)
		if (m_batteryPower->powerPVToGrid > low_tolerance &&
			_can_charge &&									// only do if battery is allowed to charge
			_Battery->battery_soc() < _SOC_max - 1.0 &&		// and battery SOC is less than max
			fabs(I) < fabs(_Ic_max) &&						// and battery current is less than max charge current
			fabs(m_batteryPower->powerBattery) < _Pc_max &&				// and battery power is less than max charge power
			I <= 0)											// and battery was not discharging
		{
			double dI = 0;
			if (fabs(m_batteryPower->powerBattery) < tolerance)
				dI = (m_batteryPower->powerPVToGrid  * util::kilowatt_to_watt / _Battery->battery_voltage());
			else
				dI = (m_batteryPower->powerPVToGrid  / fabs(m_batteryPower->powerBattery)) *fabs(I);

			// Main problem will be that this tends to overcharge battery maximum SOC, so check
			double dQ = 0.01 * (_SOC_max - _Battery->battery_soc()) * _Battery->battery_charge_maximum();

			I -= fmin(dI, dQ / _dt_hour);
		}
		// Don't let battery export to the grid if behind the meter
		else if (!front_of_meter && I > 0 && m_batteryPower->powerBatteryToGrid > tolerance)
		{
			if (fabs(m_batteryPower->powerBattery) < tolerance)
				I -= (m_batteryPower->powerBatteryToGrid * util::kilowatt_to_watt / _Battery->battery_voltage());
			else
				I -= (m_batteryPower->powerBatteryToGrid / fabs(m_batteryPower->powerBattery)) * fabs(I);
		}
		else
			iterate = false;

		// don't allow any changes to violate current limits
		bool current_iterate = restrict_current(I);

		// don't allow any changes to violate power limites
		bool power_iterate = restrict_power(I);

		// iterate if any of the conditions are met
		if (iterate || current_iterate || power_iterate)
			iterate = true;

		// stop iterating after 5 tries
		if (count > 5)
			iterate = false;

		// don't allow battery to flip from charging to discharging or vice versa
		if ((I_initial / I) < 0)
			I = 0;

		// reset
		if (iterate)
		{
			_Battery->copy(_Battery_initial);
			m_batteryPower->powerBattery = 0;
			m_batteryPower->powerGridToBattery = 0;
			m_batteryPower->powerBatteryToGrid = 0;
			m_batteryPower->powerPVToGrid  = 0;
		}
	}
	return iterate;
}

void dispatch_manual_t::SOC_controller()
{
	// Implement minimum SOC cut-off
	if (m_batteryPower->powerBattery > 0)
	{
		_charging = false;

		if (m_batteryPower->powerBattery*_dt_hour > _e_max)
			m_batteryPower->powerBattery = _e_max / _dt_hour;

		//  discharge percent
		double e_percent = _e_max*_percent_discharge*0.01;

		if (m_batteryPower->powerBattery*_dt_hour > e_percent)
			m_batteryPower->powerBattery = e_percent / _dt_hour;
	}
	// Maximum SOC cut-off
	else if (m_batteryPower->powerBattery < 0)
	{
		_charging = true;

		if (m_batteryPower->powerBattery*_dt_hour < -_e_max)
			m_batteryPower->powerBattery = -_e_max / _dt_hour;

		//  charge percent for automated grid charging
		double e_percent = _e_max*_percent_charge*0.01;

		if (fabs(m_batteryPower->powerBattery) > fabs(e_percent) / _dt_hour)
			m_batteryPower->powerBattery = -e_percent / _dt_hour;
	}
	else
		_charging = _prev_charging;
}

dispatch_manual_front_of_meter_t::dispatch_manual_front_of_meter_t(battery_t * Battery, double dt, double SOC_min, double SOC_max, int current_choice, double Ic_max, double Id_max, double Pc_max, double Pd_max,
	double t_min, int mode, int pv_dispatch,
	util::matrix_t<size_t> dm_dynamic_sched, util::matrix_t<size_t> dm_dynamic_sched_weekend,
	std::vector<bool> dm_charge, std::vector<bool> dm_discharge, std::vector<bool> dm_gridcharge, std::map<size_t, double>  dm_percent_discharge, std::map<size_t, double>  dm_percent_gridcharge)
	: dispatch_manual_t(Battery, dt, SOC_min, SOC_max, current_choice, Ic_max, Id_max,Pc_max, Pd_max,
	t_min, mode, pv_dispatch,
	dm_dynamic_sched, dm_dynamic_sched_weekend,
	dm_charge, dm_discharge, dm_gridcharge, dm_percent_discharge, dm_percent_gridcharge){};

// deep copy from dispatch to this
dispatch_manual_front_of_meter_t::dispatch_manual_front_of_meter_t(const dispatch_t & dispatch) :
dispatch_manual_t(dispatch){}

// shallow copy from dispatch to this
void dispatch_manual_front_of_meter_t::copy(const dispatch_t * dispatch)
{
	dispatch_manual_t::copy(dispatch);
}

void dispatch_manual_front_of_meter_t::dispatch(size_t year,
	size_t hour_of_year,
	size_t step,
	double P_system,
	double P_system_clipping_dc,
	double P_load_ac)
{
	prepareDispatch(hour_of_year, step, P_system, P_system_clipping_dc, P_load_ac);

	// Initalize power flow model
	m_batteryPowerFlow->initialize();

	// Controllers
	SOC_controller();
	switch_controller();
	double I = current_controller(_Battery->battery_voltage_nominal());

	// Iteration variables
	_Battery_initial->copy(_Battery);
	bool iterate = true;
	int count = 0;
	size_t idx = util::index_year_hour_step(year, hour_of_year, step, (1 / _dt_hour));

	do {

		// Run Battery Model to update charge based on charge/discharge
		_Battery->run(idx, I);

		// Update how much power was actually used to/from battery
		I = _Battery->capacity_model()->I();
		double battery_voltage_new = _Battery->battery_voltage();
		m_batteryPower->powerBattery = I * battery_voltage_new * util::watt_to_kilowatt;

		// Update power flow calculations and check the constraints
		m_batteryPowerFlow->calculate();
		iterate = check_constraints(I, count);
		m_batteryPower->powerBattery = I * _Battery->battery_voltage() * util::watt_to_kilowatt;
		count++;

	} while (iterate);

	// finalize power flow calculation and update for next step
	m_batteryPowerFlow->calculate();
	_prev_charging = _charging;
}

dispatch_automatic_t::dispatch_automatic_t(
	battery_t * Battery,
	double dt_hour,
	double SOC_min,
	double SOC_max,
	int current_choice,
	double Ic_max,
	double Id_max,
	double Pc_max,
	double Pd_max,
	double t_min,
	int dispatch_mode,
	int pv_dispatch,
	size_t nyears,
	size_t look_ahead_hours,
	double dispatch_update_frequency_hours,
	bool can_charge,
	bool can_clip_charge,
	bool can_grid_charge
	) : dispatch_t(Battery, dt_hour, SOC_min, SOC_max, current_choice, Ic_max, Id_max, Pc_max, Pd_max,
	t_min, dispatch_mode, pv_dispatch)
{

	_dt_hour = dt_hour;
	_dt_hour_update = dispatch_update_frequency_hours;
	_d_index_update = size_t(std::ceil(_dt_hour_update / _dt_hour));

	_hour_last_updated = (size_t)1e10;
	_index_last_updated = 0;

	_look_ahead_hours = look_ahead_hours;

	_steps_per_hour = (size_t)(1. / dt_hour);
	_num_steps = 24 * _steps_per_hour;

	_day_index = 0;
	_month = 1;
	_nyears = nyears;

	_mode = dispatch_mode;
	_safety_factor = 0.03;
	_can_charge = can_charge;
	_can_clip_charge = can_clip_charge;
	_can_grid_charge = can_grid_charge;


}

void dispatch_automatic_t::init_with_pointer(const dispatch_automatic_t * tmp)
{
	//_P_pv_dc = tmp->_P_pv_dc;
	//_P_battery_use = tmp->_P_battery_use;
	_P_battery_current = tmp->_P_battery_current;
	_day_index = tmp->_day_index;
	_month = tmp->_month;
	_num_steps = tmp->_num_steps;
	_hour_last_updated = tmp->_hour_last_updated;
	_dt_hour = tmp->_dt_hour;
	_dt_hour_update = tmp->_dt_hour_update;
	_steps_per_hour = tmp->_steps_per_hour;
	_nyears = tmp->_nyears;
	_mode = tmp->_mode;
	_safety_factor = tmp->_safety_factor;
	_look_ahead_hours = tmp->_look_ahead_hours;
	_d_index_update = tmp->_d_index_update;
	_index_last_updated = tmp->_index_last_updated;
}

// deep copy from dispatch to this
dispatch_automatic_t::dispatch_automatic_t(const dispatch_t & dispatch) :
dispatch_t(dispatch)
{
	const dispatch_automatic_t * tmp = dynamic_cast<const dispatch_automatic_t *>(&dispatch);
	init_with_pointer(tmp);
}

// shallow copy from dispatch to this
void dispatch_automatic_t::copy(const dispatch_t * dispatch)
{
	dispatch_t::copy(dispatch);
	const dispatch_automatic_t * tmp = dynamic_cast<const dispatch_automatic_t *>(dispatch);
	init_with_pointer(tmp);
}

void dispatch_automatic_t::update_pv_data(std::vector<double> P_pv_dc){ _P_pv_dc = P_pv_dc;}
void dispatch_automatic_t::set_custom_dispatch(std::vector<double> P_batt_dc) { _P_battery_use = P_batt_dc; }
int dispatch_automatic_t::get_mode(){ return _mode; }

void dispatch_automatic_t::prepareDispatch(size_t hour_of_year, size_t step, double P_system, double P_system_clipping, double P_load_ac, double P_battery_ac)
{
	dispatch_t::prepareDispatch(hour_of_year, step, P_system, P_system_clipping, P_load_ac);
	m_batteryPower->powerBattery = P_battery_ac;
}

void dispatch_automatic_t::dispatch(size_t year,
	size_t hour_of_year,
	size_t step,
	double P_system,
	double P_system_clipping_dc,
	double P_load_ac,
	double P_battery_ac
	)
{
	prepareDispatch(hour_of_year, step, P_system, P_system_clipping_dc, P_load_ac, P_battery_ac);

	/*! Battery voltage at last time step [V]  */
	double battery_voltage_nominal = _Battery->battery_voltage_nominal();

	/*! Battery current [A] */
	double I = (m_batteryPower->powerBattery * util::kilowatt_to_watt) / battery_voltage_nominal;

	// Controllers
	switch_controller();

	// Iteration variables
	_Battery_initial->copy(_Battery);
	bool iterate = true;
	int count = 0;
	size_t idx = util::index_year_hour_step(year, hour_of_year, step, (size_t)(1 / _dt_hour));

	do {

		// Run Battery Model to update charge based on charge/discharge
		_Battery->run(idx, I);

		// Update how much power was actually used to/from battery
		I = _Battery->capacity_model()->I();
		double battery_voltage_new = _Battery->battery_voltage();
		m_batteryPower->powerBattery = I * battery_voltage_new * util::watt_to_kilowatt;// [kW]


		iterate = check_constraints(I, count);
		count++;

	} while (iterate);

	// update for next step
	_prev_charging = _charging;
}


bool dispatch_automatic_t::check_constraints(double &I, int count)
{
	// check common constraints before checking manual dispatch specific ones
	bool iterate = dispatch_t::check_constraints(I, count);

	if (!iterate)
	{
		double I_initial = I;
		double P_battery = I * _Battery->battery_voltage() * util::watt_to_kilowatt;

		bool front_of_meter = false;
		if (dispatch_automatic_front_of_meter_t * dispatch = dynamic_cast<dispatch_automatic_front_of_meter_t*>(this))
			front_of_meter = true;

		// Comomon to Behind the meter and front of meter
		iterate = true;

		// Try and force controller to meet target or custom dispatch
		if (P_battery > _P_battery_current + tolerance || P_battery < _P_battery_current - tolerance)
		{
			// But only if it's possible to meet without break grid-charge contraint
			double dP = P_battery - _P_battery_current;

			if (_P_battery_current < 0 && dP < 0 || _can_grid_charge || _P_battery_current > 0) {
				I -= dP * util::kilowatt_to_watt / _Battery->battery_voltage();
			}
			else {
				iterate = false;
			}
		}
		// Behind the meter
		else if (!front_of_meter)
		{
			// Don't let PV export to grid if can still charge battery (increase charging)
			if (m_batteryPower->powerPVToGrid  > tolerance && _can_charge && _Battery->battery_soc() < _SOC_max - tolerance && fabs(I) < fabs(_Ic_max))
			{
				if (fabs(m_batteryPower->powerBattery) < tolerance)
					I -= (m_batteryPower->powerPVToGrid  * util::kilowatt_to_watt / _Battery->battery_voltage());
				else
					I -= (m_batteryPower->powerPVToGrid  / fabs(m_batteryPower->powerBattery)) *fabs(I);
			}
			// Don't let battery export to the grid if behind the meter
			else if (m_batteryPower->powerBatteryToGrid > tolerance)
			{
				if (fabs(m_batteryPower->powerBattery) < tolerance)
					I -= (m_batteryPower->powerBatteryToGrid * util::kilowatt_to_watt / _Battery->battery_voltage());
				else
					I -= (m_batteryPower->powerBatteryToGrid / fabs(m_batteryPower->powerBattery)) * fabs(I);
			}
			else
				iterate = false;
		}
		else
			iterate = false;

		// don't allow any changes to violate current limits
		bool current_iterate = restrict_current(I);

		// don't allow any changes to violate power limites
		bool power_iterate = restrict_power(I);

		// iterate if any of the conditions are met
		if (iterate || current_iterate || power_iterate)
			iterate = true;

		// stop iterating after 5 tries
		if (count > 5)
			iterate = false;

		// don't allow battery to flip from charging to discharging or vice versa
		if ((I_initial / I) < 0)
			I = 0;

		// reset
		if (iterate)
		{
			_Battery->copy(_Battery_initial);
			m_batteryPower->powerBattery = 0;
			m_batteryPower->powerGridToBattery = 0;
			m_batteryPower->powerBatteryToGrid = 0;
			m_batteryPower->powerPVToGrid  = 0;
		}
	}
	return iterate;
}


void dispatch_automatic_t::compute_to_batt()
{
	// Compute how much power went to battery from each component
	if (m_batteryPower->powerBattery < 0)
	{
		// First take power from clipping
		m_batteryPower->powerClippedToBattery = m_batteryPower->powerPV;
		if (m_batteryPower->powerClippedToBattery > fabs(m_batteryPower->powerBattery)) {
			m_batteryPower->powerClippedToBattery = fabs(m_batteryPower->powerBattery);
		}
		else
		{
			// Next take power from PV
			m_batteryPower->powerPVToBattery  = fabs(m_batteryPower->powerBattery) - m_batteryPower->powerClippedToBattery;
			if (m_batteryPower->powerPVToBattery  > m_batteryPower->powerPV) {
				m_batteryPower->powerPVToBattery  = fabs(m_batteryPower->powerPV);
			}
		}
		m_batteryPower->powerGridToBattery = fabs(m_batteryPower->powerBattery) - m_batteryPower->powerClippedToBattery - m_batteryPower->powerPVToBattery ;
	}
}

dispatch_automatic_behind_the_meter_t::dispatch_automatic_behind_the_meter_t(
	battery_t * Battery,
	double dt_hour,
	double SOC_min,
	double SOC_max,
	int current_choice,
	double Ic_max,
	double Id_max,
	double Pc_max,
	double Pd_max,
	double t_min,
	int dispatch_mode,
	int pv_dispatch,
	size_t nyears,
	size_t look_ahead_hours,
	double dispatch_update_frequency_hours,
	bool can_charge,
	bool can_clip_charge,
	bool can_grid_charge
	) : dispatch_automatic_t(Battery, dt_hour, SOC_min, SOC_max, current_choice, Ic_max, Id_max, Pc_max, Pd_max, t_min, dispatch_mode, pv_dispatch, nyears, look_ahead_hours, dispatch_update_frequency_hours, can_charge, can_clip_charge, can_grid_charge)
{
	_P_target_month = -1e16;
	_P_target_current = -1e16;
	_P_target_use.reserve(_num_steps);
	_P_battery_use.reserve(_num_steps);

	grid.reserve(_num_steps);
	sorted_grid.reserve(_num_steps);

	for (int ii = 0; ii != _num_steps; ii++)
	{
		grid.push_back(grid_point(0., 0, 0));
		sorted_grid.push_back(grid[ii]);
	}
}

void dispatch_automatic_behind_the_meter_t::init_with_pointer(const dispatch_automatic_behind_the_meter_t* tmp)
{
	_P_target_input = tmp->_P_target_input;
	_P_target_month = tmp->_P_target_month;
	_P_target_current = tmp->_P_target_current;
	grid = tmp->grid;

	// time series data which could be slow to copy. Since this doesn't change, should probably make const and have copy point to common memory
	_P_load_dc = tmp->_P_load_dc;
	_P_target_use = tmp->_P_target_use;
	sorted_grid = tmp->sorted_grid;
}

// deep copy from dispatch to this
dispatch_automatic_behind_the_meter_t::dispatch_automatic_behind_the_meter_t(const dispatch_t & dispatch) :
dispatch_automatic_t(dispatch)
{
	const dispatch_automatic_behind_the_meter_t * tmp = dynamic_cast<const dispatch_automatic_behind_the_meter_t *>(&dispatch);
	init_with_pointer(tmp);
}

// shallow copy from dispatch to this
void dispatch_automatic_behind_the_meter_t::copy(const dispatch_t * dispatch)
{
	dispatch_automatic_t::copy(dispatch);
	const dispatch_automatic_behind_the_meter_t * tmp = dynamic_cast<const dispatch_automatic_behind_the_meter_t *>(dispatch);
	init_with_pointer(tmp);
}

void dispatch_automatic_behind_the_meter_t::dispatch(size_t year,
	size_t hour_of_year,
	size_t step,
	double P_system,
	double P_system_clipping_dc,
	double P_load_ac)
{
	size_t step_per_hour = (size_t)(1 / _dt_hour);
	size_t idx = util::index_year_hour_step(year, hour_of_year, step, step_per_hour);

	update_dispatch(hour_of_year, step, idx);
	dispatch_automatic_t::dispatch(year, hour_of_year, step, P_system, P_system_clipping_dc, P_load_ac, _P_battery_current);
}

void dispatch_automatic_behind_the_meter_t::update_load_data(std::vector<double> P_load_dc){ _P_load_dc = P_load_dc; }
void dispatch_automatic_behind_the_meter_t::set_target_power(std::vector<double> P_target){ _P_target_input = P_target; }
void dispatch_automatic_behind_the_meter_t::update_dispatch(size_t hour_of_year, size_t step, size_t idx)
{
	bool debug = false;
	FILE *p;
	check_debug(p, debug, hour_of_year, idx);
	size_t hour_of_day = util::hour_of_day(hour_of_year);
	_day_index = (hour_of_day * _steps_per_hour + step);

	if (_mode != dispatch_t::CUSTOM_DISPATCH)
	{
		// Currently hardcoded to have 24 hour look ahead and 24 dispatch_update
		if (hour_of_day == 0 && hour_of_year != _hour_last_updated)
		{

			// [kWh] - the maximum energy that can be cycled
			double E_max = 0;

			check_new_month(hour_of_year, step);

			// setup vectors
			initialize(hour_of_year);

			// compute grid power, sort highest to lowest
			sort_grid(p, debug, idx);

			// Peak shaving scheme
			compute_energy(p, debug, E_max);
			target_power(p, debug, E_max, idx);

			// Set battery power profile
			set_battery_power(p, debug);
		}
		// save for extraction
		_P_target_current = _P_target_use[_day_index];
		_P_battery_current = _P_battery_use[_day_index];
	}
	else
	{
		_P_battery_current = _P_battery_use[idx % (8760 *_steps_per_hour)];
	}
	

	if (debug)
		fclose(p);
}
void dispatch_automatic_behind_the_meter_t::initialize(size_t hour_of_year)
{
	_hour_last_updated = hour_of_year;
	_P_target_use.clear();
	_P_battery_use.clear();

	// clean up vectors
	for (int ii = 0; ii != _num_steps; ii++)
	{
		grid[ii] = grid_point(0., 0, 0); 
		sorted_grid[ii] = grid[ii];
		_P_target_use.push_back(0.);
		_P_battery_use.push_back(0.);
	}
}
void dispatch_automatic_behind_the_meter_t::check_new_month(size_t hour_of_year, size_t step)
{
	size_t hours = 0;
	for (size_t month = 1; month <= _month; month++)
		hours += util::hours_in_month(month);

	if (hours == 8760)
		hours = 0;

	if ((hour_of_year == hours) && step == 0)
	{
		_P_target_month = -1e16;
		_month < 12 ? _month++ : _month = 1;
	}
}
void dispatch_automatic_behind_the_meter_t::check_debug(FILE *&p, bool & debug, size_t hour_of_year, size_t)
{
	// for now, don't enable
	// debug = true;

	if (hour_of_year == 0 && hour_of_year != _hour_last_updated)
	{
		// debug = true;
		if (debug)
		{
			p = fopen("dispatch.txt", "w");
			fprintf(p, "Hour of Year: %zu\t Hour Last Updated: %zu \t Steps per Hour: %zu\n", hour_of_year, _hour_last_updated, _steps_per_hour);
		}
		// failed for some reason
		if (p == NULL)
			debug = false;
	}
}

void dispatch_automatic_behind_the_meter_t::sort_grid(FILE *p, bool debug, size_t idx)
{

	if (debug)
		fprintf(p, "Index\t P_load (kW)\t P_pv (kW)\t P_grid (kW)\n");

	// compute grid net from pv and load (no battery)
	int count = 0;
	for (int hour = 0; hour != 24; hour++)
	{
		for (int step = 0; step != _steps_per_hour; step++)
		{
			grid[count] = grid_point(_P_load_dc[idx] - _P_pv_dc[idx], hour, step);
			sorted_grid[count] = grid[count];

			if (debug)
				fprintf(p, "%d\t %.1f\t %.1f\t %.1f\n", count, _P_load_dc[idx], _P_pv_dc[idx], _P_load_dc[idx] - _P_pv_dc[idx]);

			idx++;
			count++;
		}
	}
	std::sort(sorted_grid.begin(), sorted_grid.end(), byGrid());
}

void dispatch_automatic_behind_the_meter_t::compute_energy(FILE *p, bool debug, double & E_max)
{

	E_max = _Battery->battery_voltage() *_Battery->battery_charge_maximum()*(_SOC_max - _SOC_min) *0.01 *util::watt_to_kilowatt;

	if (debug)
	{
		fprintf(p, "Energy Max: %.3f\t", E_max);
		fprintf(p, "Battery Voltage: %.3f\n", _Battery->battery_voltage());
	}
}

void dispatch_automatic_behind_the_meter_t::target_power(FILE*p, bool debug, double E_useful, size_t idx)
{
	// if target power set, use that
	if ((int)_P_target_input.size() > idx && _P_target_input[idx] >= 0)
	{
		double_vec::const_iterator first = _P_target_input.begin() + idx;
		double_vec::const_iterator last = _P_target_input.begin() + idx + _num_steps;
		double_vec tmp(first, last);
		_P_target_use = tmp;
		return;
	}
	// don't calculate if peak grid demand is less than a previous target in the month
	else if (sorted_grid[0].Grid() < _P_target_month)
	{
		for (int i = 0; i != _num_steps; i++)
			_P_target_use[i] = _P_target_month;
		return;
	}
	// otherwise, compute one target for the next 24 hours.
	else
	{
		// First compute target power which will allow battery to charge up to E_useful over 24 hour period
		if (debug)
			fprintf(p, "Index\tRecharge_target\t charge_energy\n");

		double P_target = sorted_grid[0].Grid();
		double P_target_min = 1e16;
		double E_charge = 0.;
		int index = (int)_num_steps - 1;
		std::vector<double> E_charge_vec;
		for (int jj = (int)_num_steps - 1; jj >= 0; jj--)
		{
			E_charge = 0.;
			P_target_min = sorted_grid[index].Grid();

			for (int ii = (int)_num_steps - 1; ii >= 0; ii--)
			{
				if (sorted_grid[ii].Grid() > P_target_min)
					break;

				E_charge += (P_target_min - sorted_grid[ii].Grid())*_dt_hour;
			}
			E_charge_vec.push_back(E_charge);
			if (debug)
				fprintf(p, "%u: index\t%.3f\t %.3f\n", index, P_target_min, E_charge);
			index--;

			if (index < 0)
				break;
		}
		std::reverse(E_charge_vec.begin(), E_charge_vec.end());

		// Calculate target power 
		std::vector<double> sorted_grid_diff;
		sorted_grid_diff.reserve(_num_steps - 1);

		for (int ii = 0; ii != _num_steps - 1; ii++)
			sorted_grid_diff.push_back(sorted_grid[ii].Grid() - sorted_grid[ii + 1].Grid());

		P_target = sorted_grid[0].Grid(); // target power to shave to [kW]
		double sum = 0;			   // energy [kWh];
		if (debug)
			fprintf(p, "Step\tTarget_Power\tEnergy_Sum\tEnergy_charged\n");

		for (int ii = 0; ii != _num_steps - 1; ii++)
		{
			// don't look at negative grid power
			if (sorted_grid[ii + 1].Grid() < 0)
				break;
			// Update power target
			else
				P_target = sorted_grid[ii + 1].Grid();

			if (debug)
				fprintf(p, "%d\t %.3f\t", ii, P_target);

			// implies a repeated power
			if (sorted_grid_diff[ii] == 0)
			{
				if (debug)
					fprintf(p, "\n");
				continue;
			}
			// add to energy we are trimming
			else
				sum += sorted_grid_diff[ii] * (ii + 1)*_dt_hour;

			if (debug)
				fprintf(p, "%.3f\t%.3f\n", sum, E_charge_vec[ii + 1]);

			if (sum < E_charge_vec[ii + 1] && sum < E_useful)
				continue;
			// we have limited power, we'll shave what more we can
			else if (sum > E_charge_vec[ii + 1])
			{
				P_target += (sum - E_charge_vec[ii]) / ((ii + 1)*_dt_hour);
				sum = E_charge_vec[ii];
				if (debug)
					fprintf(p, "%d\t %.3f\t%.3f\t%.3f\n", ii, P_target, sum, E_charge_vec[ii]);
				break;
			}
			// only allow one cycle per day
			else if (sum > E_useful)
			{
				P_target += (sum - E_useful) / ((ii + 1)*_dt_hour);
				sum = E_useful;
				if (debug)
					fprintf(p, "%d\t %.3f\t%.3f\t%.3f\n", ii, P_target, sum, E_charge_vec[ii]);
				break;
			}
		}
		// set safety factor in case voltage differences make it impossible to achieve target without violated minimum SOC
		P_target *= (1 + _safety_factor);

		// don't set target lower than previous high in month
		if (P_target < _P_target_month)
		{
			P_target = _P_target_month;
			if (debug)
				fprintf(p, "P_target exceeds monthly target, move to  %.3f\n", P_target);
		}
		else
			_P_target_month = P_target;

		// write vector of targets
		for (int i = 0; i != _num_steps; i++)
			_P_target_use[i] = P_target;
	}
}

void dispatch_automatic_behind_the_meter_t::set_battery_power(FILE *p, bool debug)
{
	for (size_t i = 0; i != _P_target_use.size(); i++)
		_P_battery_use[i] = grid[i].Grid() - _P_target_use[i];

	if (debug)
	{
		for (size_t i = 0; i != _P_target_use.size(); i++)
			fprintf(p, "i=%zu  P_battery: %.2f\n", i, _P_battery_use[i]);
	}
}

dispatch_automatic_front_of_meter_t::dispatch_automatic_front_of_meter_t(
	battery_t * Battery,
	double dt_hour,
	double SOC_min,
	double SOC_max,
	int current_choice,
	double Ic_max,
	double Id_max,
	double Pc_max,
	double Pd_max,
	double t_min,
	int dispatch_mode,
	int pv_dispatch,
	size_t nyears,
	size_t look_ahead_hours,
	double dispatch_update_frequency_hours,
	bool can_charge,
	bool can_clip_charge,
	bool can_grid_charge,
	double inverter_paco,
	double batt_cost_per_kwh,
	int battCycleCostChoice,
	double battCycleCost,
	std::vector<double> ppa_factors,
	util::matrix_t<size_t> ppa_weekday_schedule,
	util::matrix_t<size_t> ppa_weekend_schedule,
	UtilityRate * utilityRate,
	double etaPVCharge,
	double etaGridCharge,
	double etaDischarge) : dispatch_automatic_t(Battery, dt_hour, SOC_min, SOC_max, current_choice, Ic_max, Id_max, Pc_max, Pd_max, t_min, dispatch_mode, pv_dispatch, nyears, look_ahead_hours, dispatch_update_frequency_hours, can_charge, can_clip_charge, can_grid_charge)
{
	_inverter_paco = inverter_paco;
	_ppa_factors = ppa_factors;

	// only create utility rate calculator if utility rate is defined
	_utilityRateCalculator = NULL;
	if (utilityRate) {
		_utilityRateCalculator = new UtilityRateCalculator(utilityRate, _steps_per_hour);
	}

	m_battReplacementCostPerKWH = batt_cost_per_kwh;
	m_etaPVCharge = etaPVCharge * 0.01;
	m_etaGridCharge = etaGridCharge * 0.01;
	m_etaDischarge = etaDischarge * 0.01;

	m_battCycleCostChoice = battCycleCostChoice;
	m_cycleCost = 0.05;
	if (battCycleCostChoice == dispatch_t::INPUT_CYCLE_COST) {
		m_cycleCost = battCycleCost;
	}
	
	setup_cost_vector(ppa_weekday_schedule, ppa_weekend_schedule);
}
dispatch_automatic_front_of_meter_t::~dispatch_automatic_front_of_meter_t()
{
	if (_utilityRateCalculator) {
		delete _utilityRateCalculator;
	}
}
void dispatch_automatic_front_of_meter_t::init_with_pointer(const dispatch_automatic_front_of_meter_t* tmp)
{
	_look_ahead_hours = tmp->_look_ahead_hours;
	_inverter_paco = tmp->_inverter_paco;
	_ppa_factors = tmp->_ppa_factors;
	_ppa_cost_vector = tmp->_ppa_cost_vector;

	m_battReplacementCostPerKWH = tmp->m_battReplacementCostPerKWH;
	m_etaPVCharge = tmp->m_etaPVCharge;
	m_etaGridCharge = tmp->m_etaGridCharge;
	m_etaDischarge = tmp->m_etaDischarge;
}

void dispatch_automatic_front_of_meter_t::setup_cost_vector(util::matrix_t<size_t> ppa_weekday_schedule, util::matrix_t<size_t> ppa_weekend_schedule)
{
	_ppa_cost_vector.clear();
	_ppa_cost_vector.reserve(8760 * _steps_per_hour);
	size_t month, hour, iprofile;
	double cost;
	for (size_t hour_of_year = 0; hour_of_year != 8760 + _look_ahead_hours; hour_of_year++)
	{
		size_t mod_hour_of_year = hour_of_year % 8760;
		util::month_hour(mod_hour_of_year, month, hour);
		if (util::weekday(mod_hour_of_year))
			iprofile = ppa_weekday_schedule(month - 1, hour - 1);
		else
			iprofile = ppa_weekend_schedule(month - 1, hour - 1);

		cost = _ppa_factors[iprofile - 1];

		for (size_t s = 0; s != _steps_per_hour; s++)
			_ppa_cost_vector.push_back(cost);
	}
}

// deep copy from dispatch to this
dispatch_automatic_front_of_meter_t::dispatch_automatic_front_of_meter_t(const dispatch_t & dispatch) :
dispatch_automatic_t(dispatch)
{
	const dispatch_automatic_front_of_meter_t * tmp = dynamic_cast<const dispatch_automatic_front_of_meter_t *>(&dispatch);
	init_with_pointer(tmp);
}

// shallow copy from dispatch to this
void dispatch_automatic_front_of_meter_t::copy(const dispatch_t * dispatch)
{
	dispatch_automatic_t::copy(dispatch);
	const dispatch_automatic_front_of_meter_t * tmp = dynamic_cast<const dispatch_automatic_front_of_meter_t *>(dispatch);
	init_with_pointer(tmp);
}

void dispatch_automatic_front_of_meter_t::dispatch(size_t year,
	size_t hour_of_year,
	size_t step,
	double P_system,
	double P_system_clipped,
	double P_load_ac)
{
	size_t step_per_hour = (size_t)(1 / _dt_hour);
	size_t idx = util::index_year_hour_step(year, hour_of_year, step, step_per_hour);

	prepareDispatch(hour_of_year, step, P_system, P_system_clipped, P_load_ac);
	update_dispatch(hour_of_year, step, idx);
	dispatch_automatic_t::dispatch(year, hour_of_year, step, P_system, P_system_clipped, P_load_ac, _P_battery_current);
}

void dispatch_automatic_front_of_meter_t::update_dispatch(size_t hour_of_year, size_t step_of_hour, size_t idx)
{

	if (_mode != dispatch_t::FOM_CUSTOM_DISPATCH)
	{

		// Power to charge (<0) or discharge (>0)
		double powerBattery = 0;

		if (idx == _index_last_updated + _d_index_update || idx == 0)
		{
			if (idx > 0) {
				_index_last_updated += _d_index_update;
			}

			/*! Cost to cycle the battery at all, using maximum DOD or user input */
			costToCycle();

			/*! Cost to purchase electricity from the utility */
			double usage_cost = _utilityRateCalculator->getEnergyRate(hour_of_year);

			// Compute forecast variables which don't change from year to year
			auto max_ppa_cost = std::max_element(_ppa_cost_vector.begin() + hour_of_year, _ppa_cost_vector.begin() + hour_of_year + _look_ahead_hours);
			double ppa_cost = _ppa_cost_vector[hour_of_year];

			// Compute forecast variables which potentially do change from year to year
			double energyToStoreClipped = std::accumulate(_P_cliploss_dc.begin() + idx, _P_cliploss_dc.begin() + idx + _look_ahead_hours, 0.0f) * _dt_hour;

			/*! Economic benefit of charging from the grid in current time step to discharge sometime in next X hours ($/kWh)*/
			double benefitToGridCharge = *max_ppa_cost * m_etaDischarge - usage_cost / m_etaGridCharge;

			/*! Economic benefit of charging from regular PV in current time step to discharge sometime in next X hours ($/kWh)*/
			double benefitToPVCharge = *max_ppa_cost * m_etaDischarge - ppa_cost / m_etaPVCharge;

			/*! Economic benefit of charging from clipped PV in current time step to discharge sometime in the next X hours (clipped PV is free) ($/kWh) */
			double benefitToClipCharge = *max_ppa_cost * m_etaDischarge;

			/*! Energy need to charge the battery (kWh) */
			double energyNeededToFillBattery = _Battery->battery_energy_to_fill(_SOC_max);

			/* Booleans to assist decisions */
			bool highValuePeriod = ppa_cost == *max_ppa_cost;
			bool excessAcCapacity = _inverter_paco > m_batteryPower->powerPV;
			bool batteryHasCapacity = _Battery->battery_soc() >= _SOC_min + 1.0;

			// Always Charge if PV is clipping 
			if (_can_clip_charge && m_batteryPower->powerPVClipped > 0 && benefitToClipCharge > m_cycleCost && m_batteryPower->powerPVClipped > 0)
			{
				powerBattery = -m_batteryPower->powerPVClipped;
			}

			// Increase charge from PV if it is more valuable later than selling now
			if (_can_charge && benefitToPVCharge > m_cycleCost && benefitToPVCharge > 0 && m_batteryPower->powerPV > 0)
			{
				// leave EnergyToStoreClipped capacity in battery
				if (_can_clip_charge)
				{
					if (energyToStoreClipped < energyNeededToFillBattery)
					{
						double energyCanCharge = (energyNeededToFillBattery - energyToStoreClipped);
						if (energyCanCharge <= m_batteryPower->powerPV * _dt_hour)
							powerBattery = -std::fmax(energyCanCharge / _dt_hour, m_batteryPower->powerPVClipped);
						else
							powerBattery = -std::fmax(m_batteryPower->powerPV, m_batteryPower->powerPVClipped);

						energyNeededToFillBattery = std::fmax(0, energyNeededToFillBattery + (powerBattery * _dt_hour));
					}

				}
				// otherwise, don't reserve capacity for clipping
				else
					powerBattery = -m_batteryPower->powerPV;
			}

			// Also charge from grid if it is valuable to do so, still leaving EnergyToStoreClipped capacity in battery
			if (_can_grid_charge && benefitToGridCharge > m_cycleCost && benefitToGridCharge > 0 && energyNeededToFillBattery > 0)
			{
				// leave EnergyToStoreClipped capacity in battery
				if (_can_clip_charge)
				{
					if (energyToStoreClipped < energyNeededToFillBattery)
					{
						double energyCanCharge = (energyNeededToFillBattery - energyToStoreClipped);
						powerBattery -= energyCanCharge / _dt_hour;
					}
				}
				else
					powerBattery = -energyNeededToFillBattery / _dt_hour;
			}

			// Discharge if we are in a high-price period and have battery and inverter capacity
			if (highValuePeriod && excessAcCapacity && batteryHasCapacity) {
				powerBattery = _inverter_paco - m_batteryPower->powerPV;
			}
		}
		// save for extraction
		_P_battery_current = powerBattery;
	}
	else
	{
		_P_battery_current = _P_battery_use[idx % (8760 * _steps_per_hour)];
	}
}

void dispatch_automatic_front_of_meter_t::update_cliploss_data(double_vec P_cliploss)
{
	_P_cliploss_dc = P_cliploss;

	// append to end to allow for look-ahead
	for (size_t i = 0; i != _look_ahead_hours; i++)
		_P_cliploss_dc.push_back(P_cliploss[i]);
}

void dispatch_automatic_front_of_meter_t::costToCycle()
{
	if (m_battCycleCostChoice == dispatch_t::MODEL_CYCLE_COST)
	{
		double capacityPercentDamagePerCycle = _Battery->lifetime_model()->cycleModel()->computeCycleDamageAtDOD();
		m_cycleCost = 0.01 * capacityPercentDamagePerCycle * m_battReplacementCostPerKWH;
	}
}

battery_metrics_t::battery_metrics_t(battery_t * Battery, double dt_hour)
{
	_Battery = Battery;
	_dt_hour = dt_hour;

	// single value metrics
	_e_charge_accumulated = 0; 
	_e_charge_from_pv = 0.;
	_e_charge_from_grid = _e_charge_accumulated; // assumes initial charge from grid
	_e_discharge_accumulated = 0.;
	_e_loss_system = 0.;
	_average_efficiency = 100.;
	_average_roundtrip_efficiency = 100.;
	_pv_charge_percent = 0.;

	// annual metrics
	_e_charge_from_pv_annual = 0.;
	_e_charge_from_grid_annual = _e_charge_from_grid;
	_e_charge_annual = _e_charge_accumulated;
	_e_discharge_annual = 0.;
	_e_loss_system_annual = _e_loss_system;
	_e_grid_import_annual = 0.;
	_e_grid_export_annual = 0.;
	_e_loss_annual = 0.;
}
double battery_metrics_t::average_battery_conversion_efficiency(){ return _average_efficiency; }
double battery_metrics_t::average_battery_roundtrip_efficiency(){ return _average_roundtrip_efficiency; }
double battery_metrics_t::pv_charge_percent(){ return _pv_charge_percent; }
double battery_metrics_t::energy_pv_charge_annual(){ return _e_charge_from_pv_annual; }
double battery_metrics_t::energy_grid_charge_annual(){ return _e_charge_from_grid_annual; }
double battery_metrics_t::energy_charge_annual(){ return _e_charge_annual; }
double battery_metrics_t::energy_discharge_annual(){ return _e_discharge_annual; }
double battery_metrics_t::energy_grid_import_annual(){ return _e_grid_import_annual; }
double battery_metrics_t::energy_grid_export_annual(){ return _e_grid_export_annual; }
double battery_metrics_t::energy_loss_annual(){ return _e_loss_annual; }
double battery_metrics_t::energy_system_loss_annual(){ return _e_loss_system_annual; };

void battery_metrics_t::compute_metrics_ac(double P_tofrom_batt, double P_system_loss, double P_pv_to_batt, double P_grid_to_batt, double P_tofrom_grid)
{
	accumulate_grid_annual(P_tofrom_grid);
	accumulate_battery_charge_components(P_tofrom_batt, P_pv_to_batt, P_grid_to_batt);
	accumulate_energy_charge(P_tofrom_batt);
	accumulate_energy_discharge(P_tofrom_batt);
	accumulate_energy_system_loss(P_system_loss);
	compute_annual_loss();
}

void battery_metrics_t::compute_metrics_dc(dispatch_t * dispatch)
{
	// dc quantities
	double P_tofrom_grid = dispatch->power_tofrom_grid();
	double P_tofrom_batt = dispatch->power_tofrom_battery();
	double P_pv_to_batt = dispatch->power_pv_to_batt();
	double P_grid_to_batt = dispatch->power_grid_to_batt();

	accumulate_grid_annual(P_tofrom_grid);
	accumulate_energy_charge(P_tofrom_batt);
	accumulate_energy_discharge(P_tofrom_batt);
	accumulate_battery_charge_components(P_tofrom_batt, P_pv_to_batt, P_grid_to_batt);
	compute_annual_loss();
}
void battery_metrics_t::compute_annual_loss()
{
	double e_conversion_loss = 0.;
	if (_e_charge_annual > _e_discharge_annual)
		e_conversion_loss = _e_charge_annual - _e_discharge_annual;
	_e_loss_annual = e_conversion_loss + _e_loss_system_annual;
}
void battery_metrics_t::accumulate_energy_charge(double P_tofrom_batt)
{
	if (P_tofrom_batt < 0.)
	{
		_e_charge_accumulated += (-P_tofrom_batt)*_dt_hour;
		_e_charge_annual += (-P_tofrom_batt)*_dt_hour;
	}
}
void battery_metrics_t::accumulate_energy_discharge(double P_tofrom_batt)
{
	if (P_tofrom_batt > 0.)
	{
		_e_discharge_accumulated += P_tofrom_batt*_dt_hour;
		_e_discharge_annual += P_tofrom_batt*_dt_hour;
	}
}
void battery_metrics_t::accumulate_energy_system_loss(double P_system_loss)
{
	_e_loss_system += P_system_loss * _dt_hour;
	_e_loss_system_annual += P_system_loss * _dt_hour;
}
void battery_metrics_t::accumulate_battery_charge_components(double P_tofrom_batt, double P_pv_to_batt, double P_grid_to_batt)
{
	if (P_tofrom_batt < 0.)
	{
		_e_charge_from_pv += P_pv_to_batt * _dt_hour;
		_e_charge_from_pv_annual += P_pv_to_batt * _dt_hour;
		_e_charge_from_grid += P_grid_to_batt * _dt_hour;
		_e_charge_from_grid_annual += P_grid_to_batt * _dt_hour;
	}
	_average_efficiency = 100.*(_e_discharge_accumulated / _e_charge_accumulated);
	_average_roundtrip_efficiency = 100.*(_e_discharge_accumulated / (_e_charge_accumulated + _e_loss_system));
	_pv_charge_percent = 100.*(_e_charge_from_pv / _e_charge_accumulated);
}
void battery_metrics_t::accumulate_grid_annual(double P_tofrom_grid)
{
	// e_grid > 0 (export to grid) 
	// e_grid < 0 (import from grid)

	if (P_tofrom_grid > 0)
		_e_grid_export_annual += P_tofrom_grid*_dt_hour;
	else
		_e_grid_import_annual += (-P_tofrom_grid)*_dt_hour;
}

void battery_metrics_t::new_year()
{
	_e_charge_from_pv_annual = 0.;
	_e_charge_from_grid_annual = 0;
	_e_charge_annual = 0.;
	_e_discharge_annual = 0.;
	_e_grid_import_annual = 0.;
	_e_grid_export_annual = 0.;
	_e_loss_system_annual = 0.;
}
