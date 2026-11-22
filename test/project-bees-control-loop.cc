// Exercise the production controller with a PSI fixture; never traverse mounts.
#define main project_bees_cli_main
#include "../src/project-bees.cc"
#undef main
int main(int argc, char **argv)
{
	if (argc != 3) return 2;
	std::signal(SIGTERM, stop);
	fs::create_directories(argv[1]);
	const ScanPolicy policy({}, {});
	Index index(argv[1], policy, true, argv[2]);
	while (!stopping) {
		index.checkpoint();
		std::this_thread::sleep_for(std::chrono::seconds(1));
	}
	index.finish();
}
