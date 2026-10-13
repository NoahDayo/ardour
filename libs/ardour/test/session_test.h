
#include <cppunit/TestFixture.h>
#include <cppunit/extensions/HelperMacros.h>

class SessionTest : public CppUnit::TestFixture
{
	CPPUNIT_TEST_SUITE (SessionTest);
	CPPUNIT_TEST (new_session);
	CPPUNIT_TEST (new_session_from_template);
	CPPUNIT_TEST (open_session_utf8_path);
	CPPUNIT_TEST (midnam_shutdown);
	CPPUNIT_TEST (backend_connections);
	CPPUNIT_TEST (misc_port_state);
	CPPUNIT_TEST_SUITE_END ();

public:

	void new_session ();
	void new_session_from_template ();
	void open_session_utf8_path ();
	void midnam_shutdown ();
	void backend_connections ();
	void misc_port_state ();
};
