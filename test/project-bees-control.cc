#include "../src/project-bees-control.h"
#include <cassert>
#include <filesystem>
#include <limits>
int main(int argc, char **argv) {
	assert(argc == 2);
	PressurePause p;
	for (int i=0; i<10; ++i) assert(!p.sample(i, 25));
	assert(p.sample(10, 25));
	for (int i=11; i<50; ++i) assert(!p.sample(i, 90));
	assert(!p.sample(50, std::nullopt));
	assert(!p.sample(51, 90)); // Missing PSI never rearms.
	assert(!p.sample(52, 10));
	for (int i=53; i<63; ++i) assert(!p.sample(i, 20));
	assert(p.sample(63, 20));
	PressurePause gap;
	assert(!gap.sample(0, 25));
	assert(!gap.sample(30, 25)); // A blocked syscall is not continuous sampling.
	assert(!gap.sample(31, 15));
	assert(!gap.sample(32, std::numeric_limits<double>::quiet_NaN()));
	assert(!gap.sample(33, -1));
	assert(!gap.sample(34, 101));
	for (int i=35; i<45; ++i) assert(!gap.sample(i, 25));
	assert(gap.sample(45, 25));
	const std::string fixture = std::string(argv[1]) + "/psi";
	assert(!read_pressure(fixture));
	std::ofstream(fixture) << "some avg10=23.50 avg60=1 total=100\nfull avg10=2\n";
	assert(read_pressure(fixture) == 23.5);
	for (auto value : {"nan", "101", "-1", "junk", "20junk"}) {
		std::ofstream(fixture) << "some avg10=" << value << "\n";
		assert(!read_pressure(fixture));
	}
	std::ofstream(fixture) << "some avg10=0 avg60=0\nfull avg10=1 avg60=87.0 avg300=0 total=1\n";
	assert(read_pressure(fixture) == 0);
	assert(read_pressure(fixture, "full", "avg60") == 87);
	PressurePause memory, io;
	for (int i = 0; i <= 10; ++i) {
		assert(!memory.sample(i, read_pressure(fixture)));
		assert(io.sample(i, read_pressure(fixture, "full", "avg60")) == (i == 10));
	}
	PressurePause spike;
	assert(!spike.sample(0, 87));
	assert(!spike.sample(1, 0));
	assert(!spike.sample(2, 87));
	assert(!spike.sample(3, std::nullopt));
}
