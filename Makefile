CFLAGS=-Os
CXXFLAGS=-std=c++1z ${CFLAGS}

all: producer

clean:
	rm -f producer producer.o message_queue/message_queue.o

producer: producer.o message_queue/message_queue.o
	${CXX} ${CXXFLAGS} -o $@ $^

producer.o: producer.cc message_queue/message_queue.h
	${CXX} ${CXXFLAGS} -c -o $@ $<

message_queue/message_queue.o: message_queue/message_queue.c message_queue/message_queue.h
	cd message_queue && ${CC} ${CFLAGS} -c -o message_queue.o message_queue.c 
