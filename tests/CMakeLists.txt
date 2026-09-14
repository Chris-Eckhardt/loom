# Fetch GoogleTest at configure time so no system install is required.
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
)
# Keep googletest from clobbering our compiler/CRT settings on Windows.
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
# Don't let googletest add its headers/libs to our `cmake --install` output.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

include(GoogleTest)

# One executable per unit under test, so a crash in the fiber primitives
# can't take the rest of the suite down with it.
function(loom_add_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE loom::loom GTest::gtest_main)
    # Cap the runtime so a lock that never hands off fails instead of hanging.
    gtest_discover_tests(${name} PROPERTIES TIMEOUT 120)
endfunction()

loom_add_test(fiber_test)
loom_add_test(cpu_affinity_test)
loom_add_test(spin_lock_test)
loom_add_test(work_stealing_deque_test)
loom_add_test(loom_test)
