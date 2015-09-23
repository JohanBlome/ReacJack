CC=g++
CFLAGS= -c -Wall -O3 -ftree-vectorize  -ftree-vectorizer-verbose=3 -mtune=native
LDFLAGS= -lpthread -ljack -llo -lncurses
SOURCES= reacjack.c
OBJECTS=$(SOURCES:.c=.o)
EXECUTABLE=reacjack

all: $(SOURCES) $(EXECUTABLE)
$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)


valgrind: $(SOURCES) $(EXECUTABLE)

$(EXECUTABLE): $(OBJECTS) 
	$(CC) $(OBJECTS) -o $@ $(LDFLAGS)
		
	
test: test.c test

test: test.o
	$(CC) test.o -o $@ $(LDFLAGS)
	
install:
	sudo cp $(EXECUTABLE) ~/bin/
	sudo setcap cap_net_raw,cap_net_admin=eip ~/bin/$(EXECUTABLE)

.c.o:
	$(CC) $(CFLAGS) $< -o $@
	#sudo setcap cap_net_raw,cap_net_admin=eip ./$(EXECUTABLE)
	
clean:
	rm *.o
	rm $(EXECUTABLE)
