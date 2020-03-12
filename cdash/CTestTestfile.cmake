# Disabled since that won't work in the cmake build:
# add_test(build-tests "make" "-C" "../tmp" "-j4" "all-test-tests" "all-test-examples" "all-test-benchmarks" "OPTS=\"-g\"")

add_test(tests/ "make" "-C" "../tests" "test" "TESTOPTS=$ENV{AUTOBUILD_TEST_OPTS}")
set_tests_properties(tests/ PROPERTIES  TIMEOUT "2400")
add_test(examples/ "make" "-C" "../examples" "test" "TESTOPTS=$ENV{AUTOBUILD_TEST_OPTS}")
set_tests_properties(examples/ PROPERTIES  TIMEOUT "2400")
add_test(benchmarks/ "make" "-C" "../benchmarks" "test" "TESTOPTS=$ENV{AUTOBUILD_TEST_OPTS}")
set_tests_properties(benchmarks/ PROPERTIES  TIMEOUT "2400")
