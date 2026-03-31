CXX      = g++
CXXFLAGS = -std=c++17 -pthread
LIBS     = -lssl -lcrypto

all: server client

server:
	$(CXX) $(CXXFLAGS) \
	    src/server.cpp \
	    src/thread_pool.cpp \
	    src/scheduler.cpp \
	    src/checkpoint.cpp \
	    src/sync_buffer.cpp \
	    src/rw_lock.cpp \
	    -o server $(LIBS)

client:
	$(CXX) $(CXXFLAGS) src/client.cpp -o client $(LIBS)

clean:
	rm -f server client
