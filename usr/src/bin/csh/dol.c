/*-
 * Copyright (c) 1980, 1991 The Regents of the University of California.
 * All rights reserved.
 *
 * %sccs.include.redist.c%
 */

#ifndef lint
static char sccsid[] = "@(#)dol.c	5.9 (Berkeley) %G%";
#endif /* not lint */

#include "sh.h"

/*
 * These routines perform variable substitution and quoting via ' and ".
 * To this point these constructs have been preserved in the divided
 * input words.  Here we expand variables and turn quoting via ' and " into
 * QUOTE bits on characters (which prevent further interpretation).
 * If the `:q' modifier was applied during history expansion, then
 * some QUOTEing may have occurred already, so we dont "trim()" here.
 */

static int Dpeekc, Dpeekrd;	/* Peeks for DgetC and Dreadc */
static Char *Dcp, **Dvp;	/* Input vector for Dreadc */

#define	DEOF	-1

#define	unDgetC(c)	Dpeekc = c

#define QUOTES		(_Q|_Q1|_ESC)	/* \ ' " ` */

/*
 * The following variables give the information about the current
 * $ expansion, recording the current word position, the remaining
 * words within this expansion, the count of remaining words, and the
 * information about any : modifier which is being applied.
 */
static Char *dolp;		/* Remaining chars from this word */
static Char **dolnxt;		/* Further words */
static int dolcnt;		/* Count of further words */
static Char dolmod;		/* : modifier character */
static int dolmcnt;		/* :gx -> 10000, else 1 */

static void Dfix2();
static Char *Dpack();
static int Dword();
static void dolerror();
static int DgetC();
static void Dgetdol();
static void fixDolMod();
static void setDolp();
static void unDredc();
static int Dredc();
static void Dtestq();


/*
 * Fix up the $ expansions and quotations in the
 * argument list to command t.
 */
void
Dfix(t)
    register struct command *t;
{
    register Char **pp;
    register Char *p;

    if (noexec)
	return;
    /* Note that t_dcom isn't trimmed thus !...:q's aren't lost */
    for (pp = t->t_dcom; p = *pp++;)
	for (; *p; p++) {
	    if (cmap(*p, _DOL | QUOTES)) {	/* $, \, ', ", ` */
		Dfix2(t->t_dcom);	/* found one */
		blkfree(t->t_dcom);
		t->t_dcom = gargv;
		gargv = 0;
		return;
	    }
	}
}

/*
 * $ substitute one word, for i/o redirection
 */
Char   *
Dfix1(cp)
    register Char *cp;
{
    Char   *Dv[2];

    if (noexec)
	return (0);
    Dv[0] = cp;
    Dv[1] = NULL;
    Dfix2(Dv);
    if (gargc != 1) {
	setname(short2str(cp));
	stderror(ERR_NAME | ERR_AMBIG);
    }
    cp = Strsave(gargv[0]);
    blkfree(gargv), gargv = 0;
    return (cp);
}

/*
 * Subroutine to do actual fixing after state initialization.
 */
static void
Dfix2(v)
    Char  **v;
{
    ginit();			/* Initialize glob's area pointers */
    Dvp = v;
    Dcp = STRNULL;		/* Setup input vector for Dreadc */
    unDgetC(0);
    unDredc(0);			/* Clear out any old peeks (at error) */
    dolp = 0;
    dolcnt = 0;			/* Clear out residual $ expands (...) */
    while (Dword())
	continue;
}

#define MAXWLEN (BUFSIZ - 4)
/*
 * Pack up more characters in this word
 */
static Char *
Dpack(wbuf, wp)
    Char   *wbuf, *wp;
{
    register int c;
    register int i = MAXWLEN - (wp - wbuf);

    for (;;) {
	c = DgetC(DODOL);
	if (c == '\\') {
	    c = DgetC(0);
	    if (c == DEOF) {
		unDredc(c);
		*wp = 0;
		Gcat(STRNULL, wbuf);
		return (NULL);
	    }
	    if (c == '\n')
		c = ' ';
	    else
		c |= QUOTE;
	}
	if (c == DEOF) {
	    unDredc(c);
	    *wp = 0;
	    Gcat(STRNULL, wbuf);
	    return (NULL);
	}
	if (cmap(c, _SP | _NL | _Q | _Q1)) {	/* sp \t\n'"` */
	    unDgetC(c);
	    if (cmap(c, QUOTES))
		return (wp);
	    *wp++ = 0;
	    Gcat(STRNULL, wbuf);
	    return (NULL);
	}
	if (--i <= 0)
	    stderror(ERR_WTOOLONG);
	*wp++ = c;
    }
}

/*
 * Get a word.  This routine is analogous to the routine
 * word() in sh.lex.c for the main lexical input.  One difference
 * here is that we don't get a newline to terminate our expansion.
 * Rather, DgetC will return a DEOF when we hit the end-of-input.
 */
static int
Dword()
{
    register int c, c1;
    Char    wbuf[BUFSIZ];
    register Char *wp = wbuf;
    register int i = MAXWLEN;
    register bool dolflg;
    bool    sofar = 0, done = 0;

    while (!done) {
	done = 1;
	c = DgetC(DODOL);
	switch (c) {

	case DEOF:
	    if (sofar == 0)
		return (0);
	    /* finish this word and catch the code above the next time */
	    unDredc(c);
	    /* fall into ... */

	case '\n':
	    *wp = 0;
	    Gcat(STRNULL, wbuf);
	    return (1);

	case ' ':
	case '\t':
	    done = 0;
	    break;

	case '`':
	    /* We preserve ` quotations which are done yet later */
	    *wp++ = c, --i;
	case '\'':
	case '"':
	    /*
	     * Note that DgetC never returns a QUOTES character from an
	     * expansion, so only true input quotes will get us here or out.
	     */
	    c1 = c;
	    dolflg = c1 == '"' ? DODOL : 0;
	    for (;;) {
		c = DgetC(dolflg);
		if (c == c1)
		    break;
		if (c == '\n' || c == DEOF)
		    stderror(ERR_UNMATCHED, c1);
		if ((c & (QUOTE | TRIM)) == ('\n' | QUOTE))
		    --wp, ++i;
		if (--i <= 0)
		    stderror(ERR_WTOOLONG);
		switch (c1) {

		case '"':
		    /*
		     * Leave any `s alone for later. Other chars are all
		     * quoted, thus `...` can tell it was within "...".
		     */
		    *wp++ = c == '`' ? '`' : c | QUOTE;
		    break;

		case '\'':
		    /* Prevent all further interpretation */
		    *wp++ = c | QUOTE;
		    break;

		case '`':
		    /* Leave all text alone for later */
		    *wp++ = c;
		    break;
		}
	    }
	    if (c1 == '`')
		*wp++ = '`', --i;
	    sofar = 1;
	    if ((wp = Dpack(wbuf, wp)) == NULL)
		return (1);
	    else {
		i = MAXWLEN - (wp - wbuf);
		done = 0;
	    }
	    break;

	case '\\':
	    c = DgetC(0);	/* No $ subst! */
	    if (c == '\n' || c == DEOF) {
		done = 0;
		break;
	    }
	    c |= QUOTE;
	    break;
	}
	if (done) {
	    unDgetC(c);
	    sofar = 1;
	    if ((wp = Dpack(wbuf, wp)) == NULL)
		return (1);
	    else {
		i = MAXWLEN - (wp - wbuf);
		done = 0;
	    }
	}
    }
    /* Really NOTREACHED */
    return (0);
}


/*
 * Get a character, performing $ substitution unless flag is 0.
 * Any QUOTES character which is returned from a $ expansion is
 * QUOTEd so that it will not be recognized above.
 */
static int
DgetC(flag)
    register int flag;
{
    register int c;

top:
    if (c = Dpeekc) {
	Dpeekc = 0;
	return (c);
    }
    if (lap) {
	c = *lap++ & (QUOTE | TRIM);
	if (c == 0) {
	    lap = 0;
	    goto top;
	}
quotspec:
	if (cmap(c, QUOTES))
	    return (c | QUOTE);
	return (c);
    }
    if (dolp) {
	if (c = *dolp++ & (QUOTE | TRIM))
	    goto quotspec;
	if (dolcnt > 0) {
	    setDolp(*dolnxt++);
	    --dolcnt;
	    return (' ');
	}
	dolp = 0;
    }
    if (dolcnt > 0) {
	setDolp(*dolnxt++);
	--dolcnt;
	goto top;
    }
    c = Dredc();
    if (c == '$' && flag) {
	Dgetdol();
	goto top;
    }
    return (c);
}

static Char *nulvec[] = {0};
static struct varent nulargv = {nulvec, STRargv, 0};

static void
dolerror(s)
    Char   *s;
{
    setname(short2str(s));
    stderror(ERR_NAME | ERR_RANGE);
}

/*
 * Handle the multitudinous $ expansion forms.
 * Ugh.
 */
static void
Dgetdol()
{
    register Char *np;
    register struct varent *vp = NULL;
    Char    name[4 * MAXVARLEN + 1];
    int     c, sc;
    int     subscr = 0, lwb = 1, upb = 0;
    bool    dimen = 0, bitset = 0;
    char    tnp;
    Char    wbuf[BUFSIZ];

    dolmod = dolmcnt = 0;
    c = sc = DgetC(0);
    if (c == '{')
	c = DgetC(0);		/* sc is { to take } later */
    if ((c & TRIM) == '#')
	dimen++, c = DgetC(0);	/* $# takes dimension */
    else if (c == '?')
	bitset++, c = DgetC(0);	/* $? tests existence */
    switch (c) {

    case '$':
	if (dimen || bitset)
	    stderror(ERR_SYNTAX);
	setDolp(doldol);
	goto eatbrac;

    case '<' | QUOTE:
	if (bitset)
	    stderror(ERR_NOTALLOWED, "$?<");
	if (dimen)
	    stderror(ERR_NOTALLOWED, "$?#");
	for (np = wbuf; read(OLDSTD, &tnp, 1) == 1; np++) {
	    *np = tnp;
	    if (np >= &wbuf[BUFSIZ - 1])
		stderror(ERR_LTOOLONG);
	    if (SIGN_EXTEND_CHAR(tnp) <= 0 || tnp == '\n')
		break;
	}
	*np = 0;
	/*
	 * KLUDGE: dolmod is set here because it will cause setDolp to call
	 * domod and thus to copy wbuf. Otherwise setDolp would use it
	 * directly. If we saved it ourselves, no one would know when to free
	 * it. The actual function of the 'q' causes filename expansion not to
	 * be done on the interpolated value.
	 */
	dolmod = 'q';
	dolmcnt = 10000;
	setDolp(wbuf);
	goto eatbrac;

    case DEOF:
    case '\n':
	stderror(ERR_SYNTAX);
	/* NOTREACHED */
	break;

    case '*':
	(void) Strcpy(name, STRargv);
	vp = adrof(STRargv);
	subscr = -1;		/* Prevent eating [...] */
	break;

    default:
	np = name;
	if (Isdigit(c)) {
	    if (dimen)
		stderror(ERR_NOTALLOWED, "$#<num>");
	    subscr = 0;
	    do {
		subscr = subscr * 10 + c - '0';
		c = DgetC(0);
	    } while (Isdigit(c));
	    unDredc(c);
	    if (subscr < 0) {
		dolerror(vp->v_name);
		return;
	    }
	    if (subscr == 0) {
		if (bitset) {
		    dolp = ffile ? STR1 : STR0;
		    goto eatbrac;
		}
		if (ffile == 0)
		    stderror(ERR_DOLZERO);
		fixDolMod();
		setDolp(ffile);
		goto eatbrac;
	    }
	    if (bitset)
		stderror(ERR_DOLQUEST);
	    vp = adrof(STRargv);
	    if (vp == 0) {
		vp = &nulargv;
		goto eatmod;
	    }
	    break;
	}
	if (!alnum(c))
	    stderror(ERR_VARALNUM);
	for (;;) {
	    *np++ = c;
	    c = DgetC(0);
	    if (!alnum(c))
		break;
	    if (np >= &name[MAXVARLEN])
		stderror(ERR_VARTOOLONG);
	}
	*np++ = 0;
	unDredc(c);
	vp = adrof(name);
    }
    if (bitset) {
	dolp = (vp || getenv(short2str(name))) ? STR1 : STR0;
	goto eatbrac;
    }
    if (vp == 0) {
	np = str2short(getenv(short2str(name)));
	if (np) {
	    fixDolMod();
	    setDolp(np);
	    goto eatbrac;
	}
	udvar(name);
	/* NOTREACHED */
    }
    c = DgetC(0);
    upb = blklen(vp->vec);
    if (dimen == 0 && subscr == 0 && c == '[') {
	np = name;
	for (;;) {
	    c = DgetC(DODOL);	/* Allow $ expand within [ ] */
	    if (c == ']')
		break;
	    if (c == '\n' || c == DEOF)
		stderror(ERR_INCBR);
	    if (np >= &name[sizeof(name) / sizeof(Char) - 2])
		stderror(ERR_VARTOOLONG);
	    *np++ = c;
	}
	*np = 0, np = name;
	if (dolp || dolcnt)	/* $ exp must end before ] */
	    stderror(ERR_EXPORD);
	if (!*np)
	    stderror(ERR_SYNTAX);
	if (Isdigit(*np)) {
	    int     i;

	    for (i = 0; Isdigit(*np); i = i * 10 + *np++ - '0');
	    if ((i < 0 || i > upb) && !any("-*", *np)) {
		dolerror(vp->v_name);
		return;
	    }
	    lwb = i;
	    if (!*np)
		upb = lwb, np = STRstar;
	}
	if (*np == '*')
	    np++;
	else if (*np != '-')
	    stderror(ERR_MISSING, '-');
	else {
	    register int i = upb;

	    np++;
	    if (Isdigit(*np)) {
		i = 0;
		while (Isdigit(*np))
		    i = i * 10 + *np++ - '0';
		if (i < 0 || i > upb) {
		    dolerror(vp->v_name);
		    return;
		}
	    }
	    if (i < lwb)
		upb = lwb - 1;
	    else
		upb = i;
	}
	if (lwb == 0) {
	    if (upb != 0) {
		dolerror(vp->v_name);
		return;
	    }
	    upb = -1;
	}
	if (*np)
	    stderror(ERR_SYNTAX);
    }
    else {
	if (subscr > 0)
	    if (subscr > upb)
		lwb = 1, upb = 0;
	    else
		lwb = upb = subscr;
	unDredc(c);
    }
    if (dimen) {
	Char   *cp = putn(upb - lwb + 1);

	addla(cp);
	xfree((ptr_t) cp);
    }
    else {
eatmod:
	fixDolMod();
	dolnxt = &vp->vec[lwb - 1];
	dolcnt = upb - lwb + 1;
    }
eatbrac:
    if (sc == '{') {
	c = Dredc();
	if (c != '}')
	    stderror(ERR_MISSING, '}');
    }
}

static void
fixDolMod()
{
    register int c;

    c = DgetC(0);
    if (c == ':') {
	c = DgetC(0), dolmcnt = 1;
	if (c == 'g')
	    c = DgetC(0), dolmcnt = 10000;
	if (!any("htrqxe", c))
	    stderror(ERR_BADMOD, c);
	dolmod = c;
	if (c == 'q')
	    dolmcnt = 10000;
    }
    else
	unDredc(c);
}

static void
setDolp(cp)
    register Char *cp;
{
    register Char *dp;

    if (dolmod == 0 || dolmcnt == 0) {
	dolp = cp;
	return;
    }
    dp = domod(cp, dolmod);
    if (dp) {
	dolmcnt--;
	addla(dp);
	xfree((ptr_t) dp);
    }
    else
	addla(cp);
    dolp = STRNULL;
    if (seterr)
	stderror(ERR_OLD);
}

static void
unDredc(c)
    int     c;
{

    Dpeekrd = c;
}

static int
Dredc()
{
    register int c;

    if (c = Dpeekrd) {
	Dpeekrd = 0;
	return (c);
    }
    if (Dcp && (c = *Dcp++))
	return (c & (QUOTE | TRIM));
    if (*Dvp == 0) {
	Dcp = 0;
	return (DEOF);
    }
    Dcp = *Dvp++;
    return (' ');
}

static void
Dtestq(c)
    register int c;
{

    if (cmap(c, QUOTES))
	gflag = 1;
}

/*
 * Form a shell temporary file (in unit 0) from the words
 * of the shell input up to EOF or a line the same as "term".
 * Unit 0 should have been closed before this call.
 */
void
heredoc(term)
    Char   *term;
{
    register int c;
    Char   *Dv[2];
    Char    obuf[BUFSIZ], lbuf[BUFSIZ], mbuf[BUFSIZ];
    int     ocnt, lcnt, mcnt;
    register Char *lbp, *obp, *mbp;
    Char  **vp;
    bool    quoted;
    char   *tmp;

    if (creat(tmp = short2str(shtemp), 0600) < 0)
	stderror(ERR_SYSTEM, tmp, strerror(errno));
    (void) close(0);
    if (open(tmp, O_RDWR) < 0) {
	int     oerrno = errno;

	(void) unlink(tmp);
	errno = oerrno;
	stderror(ERR_SYSTEM, tmp, strerror(errno));
    }
    (void) unlink(tmp);		/* 0 0 inode! */
    Dv[0] = term;
    Dv[1] = NULL;
    gflag = 0;
    trim(Dv);
    rscan(Dv, Dtestq);
    quoted = gflag;
    ocnt = BUFSIZ;
    obp = obuf;
    for (;;) {
	/*
	 * Read up a line
	 */
	lbp = lbuf;
	lcnt = BUFSIZ - 4;
	for (;;) {
	    c = readc(1);	/* 1 -> Want EOF returns */
	    if (c < 0 || c == '\n')
		break;
	    if (c &= TRIM) {
		*lbp++ = c;
		if (--lcnt < 0) {
		    setname("<<");
		    stderror(ERR_NAME | ERR_OVERFLOW);
		}
	    }
	}
	*lbp = 0;

	/*
	 * Check for EOF or compare to terminator -- before expansion
	 */
	if (c < 0 || eq(lbuf, term)) {
	    (void) write(0, short2str(obuf), (size_t) (BUFSIZ - ocnt));
	    (void) lseek(0, 0l, L_SET);
	    return;
	}

	/*
	 * If term was quoted or -n just pass it on
	 */
	if (quoted || noexec) {
	    *lbp++ = '\n';
	    *lbp = 0;
	    for (lbp = lbuf; c = *lbp++;) {
		*obp++ = c;
		if (--ocnt == 0) {
		    (void) write(0, short2str(obuf), BUFSIZ);
		    obp = obuf;
		    ocnt = BUFSIZ;
		}
	    }
	    continue;
	}

	/*
	 * Term wasn't quoted so variable and then command expand the input
	 * line
	 */
	Dcp = lbuf;
	Dvp = Dv + 1;
	mbp = mbuf;
	mcnt = BUFSIZ - 4;
	for (;;) {
	    c = DgetC(DODOL);
	    if (c == DEOF)
		break;
	    if ((c &= TRIM) == 0)
		continue;
	    /* \ quotes \ $ ` here */
	    if (c == '\\') {
		c = DgetC(0);
		if (!any("$\\`", c))
		    unDgetC(c | QUOTE), c = '\\';
		else
		    c |= QUOTE;
	    }
	    *mbp++ = c;
	    if (--mcnt == 0) {
		setname("<<");
		stderror(ERR_NAME | ERR_OVERFLOW);
	    }
	}
	*mbp++ = 0;

	/*
	 * If any ` in line do command substitution
	 */
	mbp = mbuf;
	if (any(short2str(mbp), '`')) {
	    /*
	     * 1 arg to dobackp causes substitution to be literal. Words are
	     * broken only at newlines so that all blanks and tabs are
	     * preserved.  Blank lines (null words) are not discarded.
	     */
	    vp = dobackp(mbuf, 1);
	}
	else
	    /* Setup trivial vector similar to return of dobackp */
	    Dv[0] = mbp, Dv[1] = NULL, vp = Dv;

	/*
	 * Resurrect the words from the command substitution each separated by
	 * a newline.  Note that the last newline of a command substitution
	 * will have been discarded, but we put a newline after the last word
	 * because this represents the newline after the last input line!
	 */
	for (; *vp; vp++) {
	    for (mbp = *vp; *mbp; mbp++) {
		*obp++ = *mbp & TRIM;
		if (--ocnt == 0) {
		    (void) write(0, short2str(obuf), BUFSIZ);
		    obp = obuf;
		    ocnt = BUFSIZ;
		}
	    }
	    *obp++ = '\n';
	    if (--ocnt == 0) {
		(void) write(0, short2str(obuf), BUFSIZ);
		obp = obuf;
		ocnt = BUFSIZ;
	    }
	}
	if (pargv)
	    blkfree(pargv), pargv = 0;
    }
}
