#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "rwlock_test.h"

using namespace PBD;

CPPUNIT_TEST_SUITE_REGISTRATION (RWLockTest);

RWLockTest::RWLockTest ()
{
}

void
RWLockTest::single_thread_test ()
{
	/* std::shared_mutex is not recursive, even for shared ownership. */
	RWLock::ReaderLock reader (_rwlock, RWLock::NotLock);
	CPPUNIT_ASSERT (!reader.locked ());
	reader.acquire ();
	CPPUNIT_ASSERT (reader.locked ());
	reader.release ();
	CPPUNIT_ASSERT (!reader.locked ());

	{
		RWLock::WriterLock writer (_rwlock);
		CPPUNIT_ASSERT (writer.locked ());
	}
	/* The writer's destructor releases exclusive ownership. */
	reader.acquire ();
	CPPUNIT_ASSERT (reader.locked ());
}

void
RWLockTest::run_thread_test ()
{
	std::mutex mutex;
	std::condition_variable changed;
	unsigned int ready = 0;
	bool release = false;
	std::vector<std::thread> workers;
	for (int i = 0; i < 3; ++i) {
		workers.emplace_back ([&] {
			RWLock::ReaderLock reader (_rwlock);
			std::unique_lock<std::mutex> guard (mutex);
			++ready;
			changed.notify_all ();
			changed.wait (guard, [&] { return release; });
		});
	}
	{
		std::unique_lock<std::mutex> guard (mutex);
		changed.wait (guard, [&] { return ready == workers.size (); });
	}

	bool writer_blocked;
	{
		RWLock::WriterLock writer (_rwlock, RWLock::TryLock);
		writer_blocked = !writer.locked ();
	}
	/* Multiple threads may read simultaneously. Do not depend on a
	 * non-blocking attempt succeeding: try_lock_shared may fail spuriously. */
	{
		RWLock::ReaderLock reader (_rwlock);
	}
	{
		std::lock_guard<std::mutex> guard (mutex);
		release = true;
	}
	changed.notify_all ();
	for (auto& worker : workers) { worker.join (); }

	CPPUNIT_ASSERT (writer_blocked);
	RWLock::WriterLock writer (_rwlock);
	CPPUNIT_ASSERT (writer.locked ());
}

void
RWLockTest::run_thread_sequence_test ()
{
	std::mutex mutex;
	std::condition_variable changed;
	bool writer_started = false;
	bool writer_acquired = false;
	bool release_writer = false;
	RWLock::ReaderLock reader (_rwlock);
	std::thread worker ([&] {
		{
			std::lock_guard<std::mutex> guard (mutex);
			writer_started = true;
		}
		changed.notify_all ();
		RWLock::WriterLock writer (_rwlock);
		std::unique_lock<std::mutex> guard (mutex);
		writer_acquired = true;
		changed.notify_all ();
		changed.wait (guard, [&] { return release_writer; });
	});

	bool writer_entered_early;
	{
		std::unique_lock<std::mutex> guard (mutex);
		changed.wait (guard, [&] { return writer_started; });
		writer_entered_early = writer_acquired;
	}
	/* Neither reader preference nor recursive ownership is guaranteed by
	 * std::shared_mutex. Another thread may succeed or fail to try-read while
	 * a writer is queued, but the writer must wait for existing readers. */
	std::thread extra_reader ([&] { RWLock::ReaderLock probe (_rwlock, RWLock::TryLock); });
	extra_reader.join ();
	reader.release ();
	{
		std::unique_lock<std::mutex> guard (mutex);
		changed.wait (guard, [&] { return writer_acquired; });
	}
	bool reader_blocked, writer_blocked;
	{
		RWLock::ReaderLock read_probe (_rwlock, RWLock::TryLock);
		reader_blocked = !read_probe.locked ();
	}
	{
		RWLock::WriterLock write_probe (_rwlock, RWLock::TryLock);
		writer_blocked = !write_probe.locked ();
	}
	{
		std::lock_guard<std::mutex> guard (mutex);
		release_writer = true;
	}
	changed.notify_all ();
	worker.join ();

	/* Assert on the test thread after joining, so failed assertions do not
	 * escape worker threads or leave another thread waiting indefinitely. */
	CPPUNIT_ASSERT (!writer_entered_early);
	CPPUNIT_ASSERT (reader_blocked);
	CPPUNIT_ASSERT (writer_blocked);
	reader.acquire ();
	CPPUNIT_ASSERT (reader.locked ());
}
