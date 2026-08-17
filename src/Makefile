# Build ringserver-feeder against EarthScope libs cloned in the Docker build stage.

CFLAGS ?= -O3
CFLAGS += -I../libslink -I../libdali -I../libmseed

LDFLAGS = -L../libslink -L../libdali -L../libmseed
LDLIBS  = -lslink -ldali -lmseed

BIN  = ../ringserver-feeder
OBJS = ringserver-feeder.o

all: $(BIN)

$(BIN): $(OBJS)
	$(CC) $(CFLAGS) -o $(BIN) $(OBJS) $(LDFLAGS) $(LDLIBS)

clean:
	rm -f $(OBJS) $(BIN)
