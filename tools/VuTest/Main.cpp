#include <stdio.h>
#include <assert.h>
#include <fenv.h>
#include "AddTest.h"
#include "FlagsTest.h"
#include "FlagsTest2.h"
#include "FlagsTest3.h"
#include "StallTest.h"
#include "TriAceTest.h"

typedef std::function<CTest*()> TestFactoryFunction;

// clang-format off
static const TestFactoryFunction s_factories[] =
{
	[]() { return new CAddTest(); },
	[]() { return new CFlagsTest(); },
	[]() { return new CFlagsTest2(); },
	[]() { return new CFlagsTest3(); },
	[]() { return new CStallTest(); },
	[]() { return new CTriAceTest(); },
};
// clang-format on

int main(int argc, const char** argv)
{
	fesetround(FE_TOWARDZERO);

	CTestVm virtualMachine;

	for(const auto& factory : s_factories)
	{
		virtualMachine.Reset();
		auto test = factory();
		test->Execute(virtualMachine);
		delete test;
	}
	return 0;
}
