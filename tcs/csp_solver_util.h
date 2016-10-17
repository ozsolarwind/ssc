#ifndef __csp_solver_util_
#define __csp_solver_util_

#include <string>
#include <vector>

#include <exception>

class C_csp_reported_outputs
{

public:
	
	class C_output
	{
	private:
		float *mp_reporting_ts_array;
		int m_n_reporting_ts_array;			//[-] Length of allocated array
		std::vector<double> mv_temp_outputs;

		bool m_is_allocated;		// True = memory allocated for array. False = no memory allocated, won't write outputs
		bool m_is_ts_weighted;		// True = timestep-weighted average of mv_temp_outputs, False = take first point in mv_temp_outputs
		
		int m_counter_reporting_ts_array;	//[-] Tracking current location of reporting array

	public:
		C_output();

		int get_vector_size();

		void set_m_is_ts_weighted(bool is_ts_weighted);

		void assign(float *p_reporting_ts_array, int n_reporting_ts_array);

		void set_timestep_output(double output_value);

		void overwrite_most_recent_timestep(double value);

		void overwrite_vector_to_constant(double value);

		std::vector<double> get_output_vector();

		void send_to_reporting_ts_array(double report_time_start, int n_report,
			const std::vector<double> & v_temp_ts_time_end, double report_time_end, bool is_save_last_step, int n_pop_back);
	};

	struct S_output_info
	{
		// Finally name must be = OUTPUT_END, so that we know how many outputs are in the table
		int m_name;					//[-] Integer key for variable
		bool m_is_ts_weighted;		// True = timestep-weighted average of mv_temp_outputs, False = take first point in mv_temp_outputs	
	};

private:

	std::vector<C_output> mvc_outputs;	//[-] vector of Output Classes
	int m_n_outputs;					//[-] number of Output Classes in vector
	
	int m_n_reporting_ts_array;			//[-] Length of allocated array

	std::vector<double> mv_latest_calculated_outputs;	//[-] Output after most recent 

public:

	C_csp_reported_outputs(){};

	void construct(const S_output_info *output_info);

	bool assign(int index, float *p_reporting_ts_array, int n_reporting_ts_array);

	void send_to_reporting_ts_array(double report_time_start,
		const std::vector<double> & v_temp_ts_time_end, double report_time_end);

	std::vector<double> get_output_vector(int index);

	void value(int index, double value);

	double value(int index);

	void overwrite_most_recent_timestep(int index, double value);

	void overwrite_vector_to_constant(int index, double value);

	int size(int index);

	void set_timestep_outputs();

};

extern const C_csp_reported_outputs::S_output_info csp_info_invalid;

class C_csp_messages
{

public:
	enum 
	{
		NOTICE = 1,
		WARNING
	};

	struct S_message_def
	{
		int m_type;
		std::string msg;

		S_message_def()
		{
			m_type = -1;
			msg[0] = NULL;
		}

        S_message_def(int type, std::string msgin)
        {
            m_type = type;
            msg = msgin;
        };

	};

	std::vector<S_message_def> m_message_list;	

public:
	C_csp_messages();

	void add_message(int type, std::string msg);

	void add_notice(std::string msg);

	bool get_message(int *type, std::string *msg);

	bool get_message(std::string *msg);

};

class C_csp_exception : public std::exception
{
public:
	std::string m_error_message;
	std::string m_code_location;
	int m_error_code;
	
	// Useful in case exception goes uncatched
	virtual const char* what();

	C_csp_exception( const char *msg );
	C_csp_exception(const std::string &error_message, const std::string &code_location);
	C_csp_exception(const std::string &error_message, const std::string &code_location, int error_code);
	virtual ~C_csp_exception() throw() { }

};

bool check_double(double x);


#endif
