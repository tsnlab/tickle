CC			:=	gcc
CFLAGS	:=	-I../../../include -I..
LDFLAGS	:=	-L../../..
LDLIBS	:=	-ltickle

all: ping pong

ping: rtt_ping.c Rtt.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

pong: rtt_pong.c Rtt.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS) $(LDLIBS)

fclean:
	$(RM)	ping pong

clean: fclean

re: clean
	$(MAKE) all

.PHONY: all run clean
