/*
 * PS3 Custom Toolchain - samples/fw/include/cell/FWCellTime.h
 *
 * Header-only C++17 time helpers for fw header validation.
 */

#ifndef PS3TC_FW_CELL_TIME_H
#define PS3TC_FW_CELL_TIME_H

#include <stdint.h>
#include <sys/sys_time.h>
#include <sys/time_util.h>

class FWTimeVal;

class FWTime {
public:
	static void init();
	static FWTimeVal getCurrentTime();
	static void startTime();
	static void stopTime();
	static void update();

	static FWTimeVal sZero;

private:
	static FWTimeVal sStoppedTime;
	static FWTimeVal sCurrentTime;
	static bool      sIsStopped;
	static double    sFreq;

	friend class FWTimeVal;
};

class FWTimeVal {
public:
	FWTimeVal()
		: mVal(0)
	{
	}

	FWTimeVal(const FWTimeVal &val)
		: mVal(val.mVal)
	{
	}

	FWTimeVal(float secs)
		: mVal((uint64_t)((double)secs * FWTime::sFreq))
	{
	}

	FWTimeVal(double secs)
		: mVal((uint64_t)(secs * FWTime::sFreq))
	{
	}

	~FWTimeVal() {}

	FWTimeVal operator+(const FWTimeVal &val) const
	{
		FWTimeVal ret;
		ret.mVal = mVal + val.mVal;
		return ret;
	}

	FWTimeVal operator-(const FWTimeVal &val) const
	{
		FWTimeVal ret;
		ret.mVal = mVal - val.mVal;
		return ret;
	}

	FWTimeVal operator*(float mul) const
	{
		FWTimeVal ret;
		ret.mVal = (uint64_t)((double)mVal * (double)mul);
		return ret;
	}

	FWTimeVal operator*(double mul) const
	{
		FWTimeVal ret;
		ret.mVal = (uint64_t)((double)mVal * mul);
		return ret;
	}

	FWTimeVal operator/(float div) const
	{
		FWTimeVal ret;
		ret.mVal = (uint64_t)((double)mVal / (double)div);
		return ret;
	}

	FWTimeVal operator/(double div) const
	{
		FWTimeVal ret;
		ret.mVal = (uint64_t)((double)mVal / div);
		return ret;
	}

	double operator/(const FWTimeVal &val) const
	{
		return (double)mVal / (double)val.mVal;
	}

	FWTimeVal &operator=(const FWTimeVal &val)
	{
		mVal = val.mVal;
		return *this;
	}

	FWTimeVal &operator+=(const FWTimeVal &val)
	{
		mVal += val.mVal;
		return *this;
	}

	FWTimeVal &operator-=(const FWTimeVal &val)
	{
		mVal -= val.mVal;
		return *this;
	}

	FWTimeVal &operator*=(float mul)
	{
		mVal = (uint64_t)((double)mVal * (double)mul);
		return *this;
	}

	FWTimeVal &operator*=(double mul)
	{
		mVal = (uint64_t)((double)mVal * mul);
		return *this;
	}

	FWTimeVal &operator/=(float div)
	{
		mVal = (uint64_t)((double)mVal / (double)div);
		return *this;
	}

	FWTimeVal &operator/=(double div)
	{
		mVal = (uint64_t)((double)mVal / div);
		return *this;
	}

	bool operator==(const FWTimeVal &val) const { return mVal == val.mVal; }
	bool operator!=(const FWTimeVal &val) const { return mVal != val.mVal; }
	bool operator<(const FWTimeVal &val) const { return mVal < val.mVal; }
	bool operator>(const FWTimeVal &val) const { return mVal > val.mVal; }
	bool operator<=(const FWTimeVal &val) const { return mVal <= val.mVal; }
	bool operator>=(const FWTimeVal &val) const { return mVal >= val.mVal; }

	operator float() const { return (float)((double)mVal / FWTime::sFreq); }
	operator double() const { return (double)mVal / FWTime::sFreq; }

protected:
	uint64_t mVal;

	friend class FWTime;
};

#if __cplusplus < 201703L
#error "PS3DK fw tier 1 time helpers require C++17 inline variables"
#endif

inline double    FWTime::sFreq = 79800000.0;
inline FWTimeVal FWTime::sZero = FWTimeVal(0.0);
inline FWTimeVal FWTime::sStoppedTime = FWTimeVal(0.0);
inline FWTimeVal FWTime::sCurrentTime = FWTimeVal(0.0);
inline bool      FWTime::sIsStopped = false;

inline void FWTime::init()
{
	uint64_t freq = sys_time_get_timebase_frequency();
	sFreq = freq ? (double)freq : 79800000.0;
	SYS_TIMEBASE_GET(sCurrentTime.mVal);
	sStoppedTime = sCurrentTime;
	sZero = FWTimeVal(0.0);
	sIsStopped = false;
}

inline FWTimeVal FWTime::getCurrentTime()
{
	return sCurrentTime;
}

inline void FWTime::startTime()
{
	sIsStopped = false;
}

inline void FWTime::stopTime()
{
	sIsStopped = true;
	sStoppedTime = sCurrentTime;
}

inline void FWTime::update()
{
	if (!sIsStopped)
		SYS_TIMEBASE_GET(sCurrentTime.mVal);
}

#endif /* PS3TC_FW_CELL_TIME_H */
