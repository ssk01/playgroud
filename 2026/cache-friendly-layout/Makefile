CXX = g++
CXXFLAGS = -std=c++17 -O2 -Wall

all: aos_vs_soa hashmap_vs_flat pointer_chasing real_tree

aos_vs_soa: aos_vs_soa.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

hashmap_vs_flat: hashmap_vs_flat.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

pointer_chasing: pointer_chasing.cpp
	$(CXX) $(CXXFLAGS) -o $@ $<

real_tree: real_tree.cpp real_deps.h
	$(CXX) $(CXXFLAGS) -o $@ real_tree.cpp

clean:
	rm -f aos_vs_soa hashmap_vs_flat pointer_chasing real_tree

.PHONY: all clean
