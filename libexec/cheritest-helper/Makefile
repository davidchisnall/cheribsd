#
# $FreeBSD$
#
PROG=	cheritest-helper
SRCS=	cheritest-helper.c			\
	stub.c

INCS=	cheritest-helper.h cheritest-helper-internal.h

LDFLAGS+=	-Wl,--script=${.CURDIR}/../../lib/libc_cheri/sandbox.ld \
		-nostdlib
MAN=

LIBADD=	md z c_cheri

NEED_CHERI=	sandbox
WANT_DUMP=	yes

NO_SHARED=	yes

.include <bsd.prog.mk>
