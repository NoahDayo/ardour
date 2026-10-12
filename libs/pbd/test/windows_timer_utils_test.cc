#include "windows_timer_utils_test.h"

#include "pbd/windows_timer_utils.h"
#include "pbd/microseconds.h"

#include <windows.h>

#include <iostream>

using namespace std;

CPPUNIT_TEST_SUITE_REGISTRATION (WindowsTimerUtilsTest);

void
WindowsTimerUtilsTest::testQPC ()
{
	int64_t last_timer_val = PBD::get_microseconds ();
	CPPUNIT_ASSERT (last_timer_val > 0);

	int64_t min_interval = 1000000;
	int64_t max_interval = 0;

	for (int i = 0; i < 10000; ++i) {
		int64_t timer_val = PBD::get_microseconds ();
		CPPUNIT_ASSERT (timer_val >= 0);
		// try and test for non-syncronized TSC(AMD K8/etc)
		CPPUNIT_ASSERT (timer_val >= last_timer_val);
		min_interval = std::min (min_interval, timer_val - last_timer_val);
		// We may get swapped out so a max interval is not so informative
		max_interval = std::max (max_interval, timer_val - last_timer_val);
		last_timer_val = timer_val;
	}

	cout << endl;
	cout << "Min QPC interval = " << min_interval << endl;
	cout << "Max QPC interval = " << max_interval << endl;
}

namespace {

void get_tgt_granularity(uint32_t& min_elapsed,
                         uint32_t& max_elapsed,
                         uint32_t& avg_elapsed)
{
	min_elapsed = 1000;
	max_elapsed = 0;
	uint32_t count = 64;
	uint32_t total_elapsed = 0;

	uint32_t last_time_ms = timeGetTime();
	for (uint32_t i = 0; i < count;) {
		uint32_t current_time_ms = timeGetTime();
		if (current_time_ms == last_time_ms) continue;
		uint32_t elapsed = current_time_ms - last_time_ms;
		cout << "TGT elapsed = " << elapsed << endl;
		min_elapsed = std::min (min_elapsed, elapsed);
		max_elapsed = std::max (max_elapsed, elapsed);
		total_elapsed += elapsed;
		last_time_ms = current_time_ms;
		++i;
	}
	avg_elapsed = total_elapsed / count;
}

void get_sleep_granularity(uint32_t& min_elapsed,
                           uint32_t& max_elapsed,
                           uint32_t& avg_elapsed)
{
	min_elapsed = 1000;
	max_elapsed = 0;
	uint32_t count = 64;
	uint32_t total_elapsed = 0;

	uint32_t last_time_ms = timeGetTime();
	for (uint32_t i = 0; i < count; ++i) {
		Sleep(1);
		uint32_t current_time_ms = timeGetTime();
		uint32_t elapsed = current_time_ms - last_time_ms;
		cout << "Sleep elapsed = " << elapsed << endl;
		min_elapsed = std::min (min_elapsed, elapsed);
		max_elapsed = std::max (max_elapsed, elapsed);
		total_elapsed += elapsed;
		last_time_ms = current_time_ms;
	}
	// the rounding here doesn't matter, we aren't interested in
	// accurate measurements
	avg_elapsed = total_elapsed / count;
}

void
test_tgt_granularity (const std::string& test_name, uint32_t& tgt_avg_elapsed)
{
	uint32_t tgt_min_elapsed = 0;
	uint32_t tgt_max_elapsed = 0;

	get_tgt_granularity(
	    tgt_min_elapsed, tgt_max_elapsed, tgt_avg_elapsed);

	cout << endl;
	cout << "TGT " << test_name << " min elapsed = " << tgt_min_elapsed << endl;
	cout << "TGT " << test_name << " max elapsed = " << tgt_max_elapsed << endl;
	cout << "TGT " << test_name << " avg elapsed = " << tgt_avg_elapsed << endl;
}

void
test_sleep_granularity (const std::string& test_name, uint32_t& sleep_avg_elapsed)
{
	uint32_t sleep_min_elapsed = 0;
	uint32_t sleep_max_elapsed = 0;

	get_sleep_granularity(
	    sleep_min_elapsed, sleep_max_elapsed, sleep_avg_elapsed);

	cout << endl;
	cout << "Sleep " << test_name << " min elapsed = " << sleep_min_elapsed << endl;
	cout << "Sleep " << test_name << " max elapsed = " << sleep_max_elapsed << endl;
	cout << "Sleep " << test_name << " avg elapsed = " << sleep_avg_elapsed << endl;
}

} // namespace

void
WindowsTimerUtilsTest::testMMTimers ()
{
	uint32_t min_timer_res = 0;
	CPPUNIT_ASSERT (PBD::MMTIMERS::get_min_resolution (min_timer_res));
	TIMECAPS caps;
	CPPUNIT_ASSERT_EQUAL (MMRESULT (TIMERR_NOERROR), timeGetDevCaps (&caps, sizeof (caps)));
	CPPUNIT_ASSERT_EQUAL (uint32_t (caps.wPeriodMin), min_timer_res);

	uint32_t avg_orig_res_tgt_elapsed = 0;

	test_tgt_granularity ("Original Timer Resolution", avg_orig_res_tgt_elapsed);

	uint32_t avg_orig_res_sleep_elapsed = 0;

	test_sleep_granularity ("Original Timer Resolution", avg_orig_res_sleep_elapsed);

	// set the min timer resolution
	CPPUNIT_ASSERT (PBD::MMTIMERS::set_min_resolution ());

	uint32_t avg_min_res_tgt_elapsed = 0;

	test_tgt_granularity ("Minimum Timer Resolution", avg_min_res_tgt_elapsed);

	uint32_t avg_min_res_sleep_elapsed = 0;

	// These measurements are diagnostic, not scheduling deadlines. Sleep does
	// not guarantee immediate scheduling when its interval expires, and Windows
	// 11 may disregard the requested resolution for invisible applications.
	// https://learn.microsoft.com/windows/win32/api/timeapi/nf-timeapi-timebeginperiod
	test_sleep_granularity ("Minimum Timer Resolution", avg_min_res_sleep_elapsed);

	CPPUNIT_ASSERT (PBD::MMTIMERS::reset_resolution());
}
