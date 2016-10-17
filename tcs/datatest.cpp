#define _TCSTYPEINTERFACE_
#include "tcstype.h"

enum {
	I_IN1,
	I_IN2,
	I_IN3,
	I_IN4,
	I_SCALE,
	I_VEC,
	I_MAT,

	O_SUM,
	O_VEC,
	O_MAT,
	O_STR,

	N_MAX };

tcsvarinfo datatest_variables[] = {
	// vartype    datatype    index   name     label    units   meta   group   default_value
	{ TCS_INPUT,  TCS_NUMBER, I_IN1, "input1", "Data 1", "",      "",      "",     "" },
	{ TCS_INPUT,  TCS_NUMBER, I_IN2, "input2", "Data 2", "",      "",      "",     "" },
	{ TCS_INPUT,  TCS_NUMBER, I_IN3, "input3", "Data 3", "",      "",      "",     "" },
	{ TCS_INPUT,  TCS_NUMBER, I_IN4, "input4", "Data 4", "",      "",      "",     "" },
	{ TCS_INPUT,  TCS_NUMBER, I_SCALE, "scale", "Scale", "",      "",      "",     "" },
	{ TCS_INPUT,  TCS_ARRAY,  I_VEC,  "vec_in", "ArrayI", "",     "",      "",     "" },
	{ TCS_INPUT,  TCS_MATRIX, I_MAT,  "mat_in", "Matrix", "",     "",      "",     "" },

	{ TCS_OUTPUT, TCS_NUMBER, O_SUM, "sum",     "Sum",    "",     "",      "",     "" },
	{ TCS_OUTPUT, TCS_ARRAY,  O_VEC, "vec_out", "Array",  "",     "",      "",     "" },
	{ TCS_OUTPUT, TCS_MATRIX, O_MAT, "mat_out", "Matrix", "",     "",      "",     "" },
	{ TCS_OUTPUT, TCS_STRING, O_STR, "str_out", "String", "",     "",      "",     "" },
	
	{ TCS_INVALID, TCS_INVALID,  N_MAX,       0,            0, 0, 0, 0, 0 }
};


class datatest : public tcstypeinterface
{
private:
public:
	datatest( tcscontext *cxt, tcstypeinfo *ti )
		: tcstypeinterface( cxt, ti )
	{
	}

	virtual ~datatest()
	{
	}

	virtual int init()
	{
		int len;
		double *vec = value( I_VEC, &len );
		
		allocate( O_VEC, 4 );


		int nrows, ncols;
		double *mat = value( I_MAT, &nrows, &ncols );		
		// make the output matrix same size as the input matrix
		if (mat && nrows > 0 && ncols > 0 )
			allocate( O_MAT, nrows, ncols );

		return 0;
	}

	virtual int call( double time, double step, int ncall )
	{
		double v[4], scale = value( I_SCALE );
		v[0] = value( I_IN1 );
		v[1] = value( I_IN2 );
		v[2] = value( I_IN3 );
		v[3] = value( I_IN4 );

		double sum = 0;
		for (int i=0;i<4;i++) sum += v[i];
		sum *= scale;

		value( O_SUM, sum );
		
		int len;
		double *vec = value(O_VEC, &len);
		if (vec && len == 4)
			for (int i=0;i<4;i++)
				vec[i] = v[3-i];


		int inr, inc, onr, onc;
		value( I_MAT, &inr, &inc ); // get sizes
		value( O_MAT, &onr, &onc ); 
		tcsvalue *imat = var( I_MAT ); // get matrix variables
		tcsvalue *omat = var( O_MAT );
		double matsum = 0;
		if (omat && inr == onr && inc == onc && imat != 0)
		{
			for (int r=0;r<inr;r++)
			{
				for (int c=0;c<inc;c++)
				{
					matsum += TCS_MATRIX_INDEX(imat, r, c);
					TCS_MATRIX_INDEX( omat, r, c) =  TCS_MATRIX_INDEX( imat, r, c ) * scale;
				}
			}
		}

		char buf[256];
		sprintf(buf, " %.2lf : %.1lf, %.1lf, %.1lf,%.1lf", matsum, v[0], v[1], v[2], v[3]);
		value_str( O_STR, buf );

		return 0;
	}
};

TCS_IMPLEMENT_TYPE( datatest, "Data test", "Aron Dobos", 1, datatest_variables, NULL, 0 )

