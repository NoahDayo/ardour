#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

#include "pbd/pthread_utils.h"

namespace {

struct ThreadLocalExit {
	explicit ThreadLocalExit (std::atomic<unsigned>& count) : count (count) {}
	~ThreadLocalExit () { ++count; }
	std::atomic<unsigned>& count;
};

void
destroy_thread_value (void* value)
{
	++*static_cast<std::atomic<unsigned>*> (value);
}

}

class ThreadTest : public CppUnit::TestFixture
{
	CPPUNIT_TEST_SUITE (ThreadTest);
	CPPUNIT_TEST (normal_exit);
	CPPUNIT_TEST_SUITE_END ();

public:
	void normal_exit ();
};

CPPUNIT_TEST_SUITE_REGISTRATION (ThreadTest);

void
ThreadTest::normal_exit ()
{
	std::atomic<unsigned> completed (0);
	std::atomic<unsigned> local_destroyed (0);
	std::atomic<unsigned> key_destroyed (0);
	std::atomic<unsigned> errors (0);
	pthread_key_t key;
	CPPUNIT_ASSERT_EQUAL (0, pthread_key_create (&key, destroy_thread_value));

	unsigned total = 0;
	for (unsigned batch = 0; batch < 32; ++batch) {
		std::mutex mutex;
		std::condition_variable changed;
		bool release = false;
		std::vector<std::unique_ptr<PBD::Thread>> workers;
		for (unsigned i = 0; i < 8; ++i) {
			PBD::Thread* worker = PBD::Thread::create ([&] {
				thread_local ThreadLocalExit local (local_destroyed);
				(void) local;
				if (pthread_setspecific (key, &key_destroyed) != 0 || std::string (pthread_name ()) != "ThreadTest") {
					++errors;
				}
				std::unique_lock<std::mutex> guard (mutex);
				changed.wait (guard, [&] { return release; });
				++completed;
			}, "ThreadTest");
			if (!worker) {
				++errors;
				break;
			}
			workers.emplace_back (worker);
		}
		/* Let registration finish before these deliberately short workers exit. */
		{
			std::lock_guard<std::mutex> guard (mutex);
			release = true;
		}
		changed.notify_all ();
		for (auto& worker : workers) {
			worker->join ();
		}
		total += workers.size ();
	}
	const int deleted = pthread_key_delete (key);
	/* Verify after joining every worker, including on partial creation failure. */
	CPPUNIT_ASSERT_EQUAL (0, deleted);
	CPPUNIT_ASSERT_EQUAL (0u, errors.load ());
	CPPUNIT_ASSERT_EQUAL (256u, total);
	CPPUNIT_ASSERT_EQUAL (total, completed.load ());
	CPPUNIT_ASSERT_EQUAL (total, local_destroyed.load ());
	CPPUNIT_ASSERT_EQUAL (total, key_destroyed.load ());
}
