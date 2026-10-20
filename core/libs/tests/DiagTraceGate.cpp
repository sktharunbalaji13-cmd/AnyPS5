// Mission 7: unit test for the diagnostic gate: every control is independent of the others
// and the defaults are off.

#include "prx/libc/include/general/DiagTrace.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <stdlib.h>
#endif

namespace {

void Require(bool value, const char* what) {
    if (!value) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        std::exit(1);
    }
}

void Set(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

} // namespace

int main() {
    // A control that is set is enabled, and one that is not is disabled: the answers are per
    // name, so one control never decides another.
    Set("APS5_DIAG_TEST_ON", "1");
    Set("APS5_DIAG_TEST_OTHER", "1");
    Require(Aps5Diag::Enabled("APS5_DIAG_TEST_ON"), "a set control is enabled");
    Require(Aps5Diag::Enabled("APS5_DIAG_TEST_OTHER"), "another set control is enabled");
    Require(!Aps5Diag::Enabled("APS5_DIAG_TEST_OFF"), "an unset control is disabled");
    Require(Aps5Diag::Enabled("APS5_DIAG_TEST_ON"), "the answer is stable");
    Require(!Aps5Diag::Enabled("APS5_DIAG_TEST_OFF"), "the disabled answer is stable too");

    // The controls are presence-based: any value counts as set, including "0".
    Set("APS5_DIAG_TEST_ZERO", "0");
    Require(Aps5Diag::Enabled("APS5_DIAG_TEST_ZERO"), "a control set to zero still counts as set");

    // The real diagnostics are off unless the environment says otherwise.
    Require(!Aps5Diag::Enabled("APS5_DIAG_NOT_SET_ANYWHERE"), "an unknown control is disabled");

    std::printf("diagnostic gate tests passed\n");
    return 0;
}
