.PHONY: disable_aslr spsc_test spsc_benchmark test benchmark

# see https://stackoverflow.com/questions/77850769/fatal-threadsanitizer-unexpected-memory-mapping-when-running-on-linux-kernels
disable_aslr:
	sudo sysctl vm.mmap_rnd_bits=28

spsc_test:
	g++ -g --std=c++20 spsc.cpp -DTEST -fsanitize=thread && ./a.out
	g++ -g --std=c++20 spsc.cpp -DTEST -fsanitize=address && ./a.out
	g++ -g --std=c++20 spsc.cpp -DTEST -O3 && ./a.out

test:
	g++ -g --std=c++20 test.cpp -fsanitize=thread && ./a.out
	g++ -g --std=c++20 test.cpp -fsanitize=address && ./a.out
	g++ -g --std=c++20 test.cpp -O3 && ./a.out

benchmark:
	g++ -g --std=c++20 benchmark.cpp -O3 -o benchmark


