# Host-side test of the RDS text path; needs no FPP tree or hardware.
CXX ?= c++
rds_text_test: rds_text_test.cpp ../src/RDSPacketBuilder.cpp ../src/RDSPacketBuilder.h $(wildcard ../src/RDSCharset.h)
	$(CXX) -std=c++20 -Wall -O1 -o $@ rds_text_test.cpp ../src/RDSPacketBuilder.cpp
clean:
	rm -f rds_text_test
