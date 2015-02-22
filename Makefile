CPPFLAGS=-Wall -fPIC -g -std=gnu++0x -pthread

LD=g++
LDFLAGS=-shared

all: dlz_dnsforever.so

dlz_dnsforever.so: dlz_dnsforever.o
	$(LD) $(LDFLAGS) -o dlz_dnsforever.so  dlz_dnsforever.o -lcurl -ldl -ljsoncpp

clean:
	rm -f dlz_dnsforever.o dlz_dnsforever.so
