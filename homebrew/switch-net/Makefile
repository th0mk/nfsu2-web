# The client and its smoke-test program on a POSIX host (Linux, macOS, WSL).  For wss:// build with
# `make TLS=1` (mbedTLS headers and library needed); the Switch build is in README.md.
CC ?= cc
CFLAGS ?= -O2 -Wall -Wextra -Wno-unused-parameter
SRC = n2n.c n2n_sock.c
ifdef TLS
SRC += n2n_tls_mbedtls.c
CFLAGS += -DN2N_TLS
LDLIBS += -lmbedtls -lmbedx509 -lmbedcrypto
endif

n2n_cli: n2n_cli.c $(SRC) n2n.h
	$(CC) $(CFLAGS) -o $@ n2n_cli.c $(SRC) $(LDLIBS)

clean:
	rm -f n2n_cli
.PHONY: clean
