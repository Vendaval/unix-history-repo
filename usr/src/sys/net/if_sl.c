/*
 * Copyright (c) 1987, 1989 Regents of the University of California.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms are permitted
 * provided that the above copyright notice and this paragraph are
 * duplicated in all such forms and that any documentation,
 * advertising materials, and other materials related to such
 * distribution and use acknowledge that the software was developed
 * by the University of California, Berkeley.  The name of the
 * University may not be used to endorse or promote products derived
 * from this software without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED
 * WARRANTIES OF MERCHANTIBILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 *	@(#)if_sl.c	7.16 (Berkeley) %G%
 */

/*
 * Serial Line interface
 *
 * Rick Adams
 * Center for Seismic Studies
 * 1300 N 17th Street, Suite 1450
 * Arlington, Virginia 22209
 * (703)276-7900
 * rick@seismo.ARPA
 * seismo!rick
 *
 * Pounded on heavily by Chris Torek (chris@mimsy.umd.edu, umcp-cs!chris).
 * N.B.: this belongs in netinet, not net, the way it stands now.
 * Should have a link-layer type designation, but wouldn't be
 * backwards-compatible.
 *
 * Converted to 4.3BSD Beta by Chris Torek.
 * Other changes made at Berkeley, based in part on code by Kirk Smith.
 * W. Jolitz added slip abort.
 *
 * Hacked almost beyond recognition by Van Jacobson (van@helios.ee.lbl.gov).
 * Added priority queuing for "interactive" traffic; hooks for TCP
 * header compression; ICMP filtering (at 2400 baud, some cretin
 * pinging you can use up all your bandwidth).  Made low clist behavior
 * more robust and slightly less likely to hang serial line.
 * Sped up a bunch of things.
 * 
 * Note that splimp() is used throughout to block both (tty) input
 * interrupts and network activity; thus, splimp must be >= spltty.
 */

/* $Header: if_sl.c,v 1.7 89/05/31 02:24:52 van Exp $ */
/* from if_sl.c,v 1.11 84/10/04 12:54:47 rick Exp */

#include "sl.h"
#if NSL > 0

#include "param.h"
#include "dir.h"
#include "user.h"
#include "mbuf.h"
#include "buf.h"
#include "dk.h"
#include "socket.h"
#include "ioctl.h"
#include "file.h"
#include "tty.h"
#include "kernel.h"
#include "conf.h"
#include "errno.h"

#include "if.h"
#include "netisr.h"
#include "route.h"
#if INET
#include "../netinet/in.h"
#include "../netinet/in_systm.h"
#include "../netinet/in_var.h"
#include "../netinet/ip.h"
#endif

#include "machine/mtpr.h"

#include "slcompress.h"
#include "if_slvar.h"

/*
 * SLMTU is a hard limit on input packet size.  To simplify the code
 * and improve performance, we require that packets fit in an mbuf
 * cluster, that there be enough extra room for the ifnet pointer that
 * IP input requires and, if we get a compressed packet, there's
 * enough extra room to expand the header into a max length tcp/ip
 * header (128 bytes).  So, SLMTU can be at most
 * 	MCLBYTES - sizeof(struct ifnet *) - 128 
 *
 * To insure we get good interactive response, the MTU wants to be
 * the smallest size that amortizes the header cost.  (Remember
 * that even with type-of-service queuing, we have to wait for any
 * in-progress packet to finish.  I.e., we wait, on the average,
 * 1/2 * mtu / cps, where cps is the line speed in characters per
 * second.  E.g., 533ms wait for a 1024 byte MTU on a 9600 baud
 * line.  The average compressed header size is 6-8 bytes so any
 * MTU > 90 bytes will give us 90% of the line bandwidth.  A 100ms
 * wait is tolerable (500ms is not), so want an MTU around 256.
 * (Since TCP will send 212 byte segments (to allow for 40 byte
 * headers), the typical packet size on the wire will be around 220
 * bytes).  In 4.3tahoe+ systems, we can set an MTU in a route
 * so we do that & leave the interface MTU relatively high (so we
 * don't IP fragment when acting as a gateway to someone using a
 * stupid MTU).
 */
#define	SLMTU	576
#define BUFOFFSET (128+sizeof(struct ifnet **))
#define	SLIP_HIWAT	1024	/* don't start a new packet if HIWAT on queue */
#define	CLISTRESERVE	1024	/* Can't let clists get too low */
/*
 * SLIP ABORT ESCAPE MECHANISM:
 *	(inspired by HAYES modem escape arrangement)
 *	1sec escape 1sec escape 1sec escape { 1sec escape 1sec escape }
 *	signals a "soft" exit from slip mode by usermode process
 */

#define	ABT_ESC		'\033'	/* can't be t_intr - distant host must know it*/
#define ABT_WAIT	1	/* in seconds - idle before an escape & after */
#define ABT_RECYCLE	(5*2+2)	/* in seconds - time window processing abort */

#define ABT_SOFT	3	/* count of escapes */

/*
 * The following disgusting hack gets around the problem that IP TOS
 * can't be set in BSD/Sun OS yet.  We want to put "interactive"
 * traffic on a high priority queue.  To decide if traffic is
 * interactive, we check that a) it is TCP and b) one of it's ports
 * if telnet, rlogin or ftp control.
 */
static u_short interactive_ports[8] = {
	0,	513,	0,	0,
	0,	21,	0,	23,
};
#define INTERACTIVE(p) (interactive_ports[(p) & 7] == (p))

struct sl_softc sl_softc[NSL];

#define FRAME_END	 	0xc0		/* Frame End */
#define FRAME_ESCAPE		0xdb		/* Frame Esc */
#define TRANS_FRAME_END	 	0xdc		/* transposed frame end */
#define TRANS_FRAME_ESCAPE 	0xdd		/* transposed frame esc */

#define t_sc T_LINEP

int sloutput(), slioctl(), ttrstrt();

/*
 * Called from boot code to establish sl interfaces.
 */
slattach()
{
	register struct sl_softc *sc;
	register int i = 0;

	for (sc = sl_softc; i < NSL; sc++) {
		sc->sc_if.if_name = "sl";
		sc->sc_if.if_unit = i++;
		sc->sc_if.if_mtu = SLMTU;
		sc->sc_if.if_flags = IFF_POINTOPOINT;
		sc->sc_if.if_ioctl = slioctl;
		sc->sc_if.if_output = sloutput;
		sc->sc_if.if_snd.ifq_maxlen = 50;
		sc->sc_fastq.ifq_maxlen = 32;
		if_attach(&sc->sc_if);
	}
}

static int
slinit(sc)
	register struct sl_softc *sc;
{
	register caddr_t p;

	if (sc->sc_ep == (u_char *) 0) {
		MCLALLOC(p, M_WAIT);
		if (p)
			sc->sc_ep = (u_char *)p + (BUFOFFSET + SLMTU);
		else {
			printf("sl%d: can't allocate buffer\n", sc - sl_softc);
			sc->sc_if.if_flags &= ~IFF_UP;
			return (0);
		}
	}
	sc->sc_buf = sc->sc_ep - SLMTU;
	sc->sc_mp = sc->sc_buf;
	sl_compress_init(&sc->sc_comp);
	return (1);
}

/*
 * Line specific open routine.
 * Attach the given tty to the first available sl unit.
 */
/* ARGSUSED */
slopen(dev, tp)
	dev_t dev;
	register struct tty *tp;
{
	register struct sl_softc *sc;
	register int nsl;
	int error;

	if (error = suser(u.u_cred, &u.u_acflag))
		return (error);

	if (tp->t_line == SLIPDISC)
		return (EBUSY);

	for (nsl = NSL, sc = sl_softc; --nsl >= 0; sc++)
		if (sc->sc_ttyp == NULL) {
			if (slinit(sc) == 0)
				return (ENOBUFS);
			tp->t_sc = (caddr_t)sc;
			sc->sc_ttyp = tp;
			ttyflush(tp, FREAD | FWRITE);
			return (0);
		}
	return (ENXIO);
}

/*
 * Line specific close routine.
 * Detach the tty from the sl unit.
 * Mimics part of ttyclose().
 */
slclose(tp)
	struct tty *tp;
{
	register struct sl_softc *sc;
	int s;

	ttywflush(tp);
	s = splimp();		/* actually, max(spltty, splnet) */
	tp->t_line = 0;
	sc = (struct sl_softc *)tp->t_sc;
	if (sc != NULL) {
		if_down(&sc->sc_if);
		sc->sc_ttyp = NULL;
		tp->t_sc = NULL;
		MCLFREE((struct mbuf *)(sc->sc_ep - (SLMTU + BUFOFFSET)));
		sc->sc_ep = 0;
		sc->sc_mp = 0;
		sc->sc_buf = 0;
	}
	splx(s);
}

/*
 * Line specific (tty) ioctl routine.
 * Provide a way to get the sl unit number.
 */
/* ARGSUSED */
sltioctl(tp, cmd, data, flag)
	struct tty *tp;
	caddr_t data;
{
	struct sl_softc *sc = (struct sl_softc *)tp->t_sc;
	int s;

	switch (cmd) {
	case TIOCGETD:				/* XXX */
	case SLIOGUNIT:
		*(int *)data = sc->sc_if.if_unit;
		break;

	case SLIOCGFLAGS:
		*(int *)data = sc->sc_flags;
		break;

	case SLIOCSFLAGS:
#define	SC_MASK	(SC_COMPRESS|SC_NOICMP)
		s = splimp();
		sc->sc_flags =
		    (sc->sc_flags &~ SC_MASK) | ((*(int *)data) & SC_MASK);
		splx(s);
		break;

	default:
		return (-1);
	}
	return (0);
}

/*
 * Queue a packet.  Start transmission if not active.
 */
sloutput(ifp, m, dst)
	register struct ifnet *ifp;
	register struct mbuf *m;
	struct sockaddr *dst;
{
	register struct sl_softc *sc;
	register struct ip *ip;
	register struct ifqueue *ifq;
	int s;

	/*
	 * `Cannot happen' (see slioctl).  Someday we will extend
	 * the line protocol to support other address families.
	 */
	if (dst->sa_family != AF_INET) {
		printf("sl%d: af%d not supported\n", ifp->if_unit,
			dst->sa_family);
		m_freem(m);
		return (EAFNOSUPPORT);
	}

	sc = &sl_softc[ifp->if_unit];
	if (sc->sc_ttyp == NULL) {
		m_freem(m);
		return (ENETDOWN);	/* sort of */
	}
	if ((sc->sc_ttyp->t_state & TS_CARR_ON) == 0) {
		m_freem(m);
		return (EHOSTUNREACH);
	}
	ifq = &ifp->if_snd;
	if ((ip = mtod(m, struct ip *))->ip_p == IPPROTO_TCP) {
		register int p = ((int *)ip)[ip->ip_hl];

		if (INTERACTIVE(p & 0xffff) || INTERACTIVE(p >> 16))
			ifq = &sc->sc_fastq;

		if (sc->sc_flags & SC_COMPRESS) {
			/* if two copies of sl_compress_tcp are running
			 * for the same line, the compression state can
			 * get screwed up.  We're assuming that sloutput
			 * was invoked at splnet so this isn't possible
			 * (this assumption is correct for 4.xbsd, x<=4).
			 * In a multi-threaded kernel, a lockout might
			 * be needed here. */
			p = sl_compress_tcp(m, ip, &sc->sc_comp);
			*mtod(m, u_char *) |= p;
		}
	} else if (sc->sc_flags & SC_NOICMP && ip->ip_p == IPPROTO_ICMP) {
		m_freem(m);
		return (0);
	}
	s = splimp();
	if (IF_QFULL(ifq)) {
		IF_DROP(ifq);
		m_freem(m);
		splx(s);
		sc->sc_if.if_oerrors++;
		return (ENOBUFS);
	}
	IF_ENQUEUE(ifq, m);
	if (sc->sc_ttyp->t_outq.c_cc == 0) {
		splx(s);
		slstart(sc->sc_ttyp);
	} else
		splx(s);
	return (0);
}

/*
 * Start output on interface.  Get another datagram
 * to send from the interface queue and map it to
 * the interface before starting output.
 */
slstart(tp)
	register struct tty *tp;
{
	register struct sl_softc *sc = (struct sl_softc *)tp->t_sc;
	register struct mbuf *m;
	register u_char *cp;
	int s;
	struct mbuf *m2;
	extern int cfreecount;

	for (;;) {
		/*
		 * If there is more in the output queue, just send it now.
		 * We are being called in lieu of ttstart and must do what
		 * it would.
		 */
		if (tp->t_outq.c_cc != 0) {
			(*tp->t_oproc)(tp);
			if (tp->t_outq.c_cc > SLIP_HIWAT)
				return;
		}
		/*
		 * This happens briefly when the line shuts down.
		 */
		if (sc == NULL)
			return;

		/*
		 * Get a packet and send it to the interface.
		 */
		s = splimp();
		IF_DEQUEUE(&sc->sc_fastq, m);
		if (m == NULL)
			IF_DEQUEUE(&sc->sc_if.if_snd, m);
		splx(s);
		if (m == NULL)
			return;
		/*
		 * If system is getting low on clists, just flush our
		 * output queue (if the stuff was important, it'll get
		 * retransmitted).
		 */
		if (cfreecount < CLISTRESERVE + SLMTU) {
			m_freem(m);
			sc->sc_if.if_collisions++;
			continue;
		}

		/*
		 * The extra FRAME_END will start up a new packet, and thus
		 * will flush any accumulated garbage.  We do this whenever
		 * the line may have been idle for some time.
		 */
		if (tp->t_outq.c_cc == 0) {
			++sc->sc_bytessent;
			(void) putc(FRAME_END, &tp->t_outq);
		}

		while (m) {
			register u_char *ep;

			cp = mtod(m, u_char *); ep = cp + m->m_len;
			while (cp < ep) {
				/*
				 * Find out how many bytes in the string we can
				 * handle without doing something special.
				 */
				register u_char *bp = cp;

				while (cp < ep) {
					switch (*cp++) {
					case FRAME_ESCAPE:
					case FRAME_END:
						--cp;
						goto out;
					}
				}
				out:
				if (cp > bp) {
					/*
					 * Put n characters at once
					 * into the tty output queue.
					 */
					if (b_to_q((char *)bp, cp - bp, &tp->t_outq))
						break;
					sc->sc_bytessent += cp - bp;
				}
				/*
				 * If there are characters left in the mbuf,
				 * the first one must be special..
				 * Put it out in a different form.
				 */
				if (cp < ep) {
					if (putc(FRAME_ESCAPE, &tp->t_outq))
						break;
					if (putc(*cp++ == FRAME_ESCAPE ?
					   TRANS_FRAME_ESCAPE : TRANS_FRAME_END,
					   &tp->t_outq)) {
						(void) unputc(&tp->t_outq);
						break;
					}
					sc->sc_bytessent += 2;
				}
			}
			MFREE(m, m2);
			m = m2;
		}

		if (putc(FRAME_END, &tp->t_outq)) {
			/*
			 * Not enough room.  Remove a char to make room
			 * and end the packet normally.
			 * If you get many collisions (more than one or two
			 * a day) you probably do not have enough clists
			 * and you should increase "nclist" in param.c.
			 */
			(void) unputc(&tp->t_outq);
			(void) putc(FRAME_END, &tp->t_outq);
			sc->sc_if.if_collisions++;
		} else {
			++sc->sc_bytessent;
			sc->sc_if.if_opackets++;
		}
	}
}

/*
 * Copy data buffer to mbuf chain; add ifnet pointer.
 */
static struct mbuf *
sl_btom(sc, len)
	register struct sl_softc *sc;
	register int len;
{
	register u_char *cp;
	register struct mbuf *m;

	MGETHDR(m, M_DONTWAIT, MT_DATA);
	if (m == NULL)
		return (NULL);

	/*
	 * If we have more than MLEN bytes, it's cheaper to
	 * queue the cluster we just filled & allocate a new one
	 * for the input buffer.  Otherwise, fill the mbuf we
	 * allocated above.  Note that code in the input routine
	 * guarantees that packet will fit in a cluster.
	 */
	cp = sc->sc_buf;
	if (len >= MHLEN) {
		MCLGET(m, M_DONTWAIT);
		if ((m->m_flags & M_EXT) == 0) {
			/* we couldn't get a cluster - if memory's this
			 * low, it's time to start dropping packets. */
			m_freem(m);
			return (NULL);
		}
		sc->sc_ep = mtod(m, u_char *) + (BUFOFFSET + SLMTU);
		m->m_data = (caddr_t)cp;
	} else
		bcopy((caddr_t)cp, mtod(m, caddr_t), len);

	m->m_len = len;
	m->m_pkthdr.len = len;
	m->m_pkthdr.rcvif = &sc->sc_if;
	return (m);
}

/*
 * tty interface receiver interrupt.
 */
slinput(c, tp)
	register int c;
	register struct tty *tp;
{
	register struct sl_softc *sc;
	register struct mbuf *m;
	register int len;
	int s;

	tk_nin++;
	sc = (struct sl_softc *)tp->t_sc;
	if (sc == NULL)
		return;
	if (!(tp->t_state&TS_CARR_ON))	/* XXX */
		return;

	++sc->sc_bytesrcvd;
	c &= 0xff;			/* XXX */

#ifdef ABT_ESC
	if (sc->sc_flags & SC_ABORT) {
		/* if we see an abort after "idle" time, count it */
		if (c == ABT_ESC && time.tv_sec >= sc->sc_lasttime + ABT_WAIT) {
			sc->sc_abortcount++;
			/* record when the first abort escape arrived */
			if (sc->sc_abortcount == 1)
				sc->sc_starttime = time.tv_sec;
		}
		/*
		 * if we have an abort, see that we have not run out of time,
		 * or that we have an "idle" time after the complete escape
		 * sequence
		 */
		if (sc->sc_abortcount) {
			if (time.tv_sec >= sc->sc_starttime + ABT_RECYCLE)
				sc->sc_abortcount = 0;
			if (sc->sc_abortcount >= ABT_SOFT &&
			    time.tv_sec >= sc->sc_lasttime + ABT_WAIT) {
				slclose(tp);
				return;
			}
		}
		sc->sc_lasttime = time.tv_sec;
	}
#endif

	switch (c) {

	case TRANS_FRAME_ESCAPE:
		if (sc->sc_escape)
			c = FRAME_ESCAPE;
		break;

	case TRANS_FRAME_END:
		if (sc->sc_escape)
			c = FRAME_END;
		break;

	case FRAME_ESCAPE:
		sc->sc_escape = 1;
		return;

	case FRAME_END:
		len = sc->sc_mp - sc->sc_buf;
		if (len < 3)
			/* less than min length packet - ignore */
			goto newpack;

		if ((c = (*sc->sc_buf & 0xf0)) != (IPVERSION << 4)) {
			if (c & 0x80)
				c = TYPE_COMPRESSED_TCP;
			else if (c == TYPE_UNCOMPRESSED_TCP)
				*sc->sc_buf &= 0x4f; /* XXX */
			len = sl_uncompress_tcp(&sc->sc_buf, len, (u_int)c,
						&sc->sc_comp);
			if (len <= 0)
				goto error;
		}
		m = sl_btom(sc, len);
		if (m == NULL)
			goto error;

		sc->sc_if.if_ipackets++;
		s = splimp();
		if (IF_QFULL(&ipintrq)) {
			IF_DROP(&ipintrq);
			sc->sc_if.if_ierrors++;
			m_freem(m);
		} else {
			IF_ENQUEUE(&ipintrq, m);
			schednetisr(NETISR_IP);
		}
		splx(s);
		goto newpack;
	}
	if (sc->sc_mp < sc->sc_ep) {
		*sc->sc_mp++ = c;
		sc->sc_escape = 0;
		return;
	}
error:
	sc->sc_if.if_ierrors++;
newpack:
	sc->sc_mp = sc->sc_buf = sc->sc_ep - SLMTU;
	sc->sc_escape = 0;
}

/*
 * Process an ioctl request.
 */
slioctl(ifp, cmd, data)
	register struct ifnet *ifp;
	int cmd;
	caddr_t data;
{
	register struct ifaddr *ifa = (struct ifaddr *)data;
	int s = splimp(), error = 0;

	switch (cmd) {

	case SIOCSIFADDR:
		if (ifa->ifa_addr->sa_family == AF_INET)
			ifp->if_flags |= IFF_UP;
		else
			error = EAFNOSUPPORT;
		break;

	case SIOCSIFDSTADDR:
		if (ifa->ifa_addr->sa_family != AF_INET)
			error = EAFNOSUPPORT;
		break;

	default:
		error = EINVAL;
	}
	splx(s);
	return (error);
}
#endif
