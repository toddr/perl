/*    regexec.c
 */

/*
 * "One Ring to rule them all, One Ring to find them..."
 */

/* This file contains functions for executing a regular expression.  See
 * also regcomp.c which funnily enough, contains functions for compiling
 * a regular expression.
 *
 * This file is also copied at build time to ext/re/re_exec.c, where
 * it's built with -DPERL_EXT_RE_BUILD -DPERL_EXT_RE_DEBUG -DPERL_EXT.
 * This causes the main functions to be compiled under new names and with
 * debugging support added, which makes "use re 'debug'" work.
 
 */

/* NOTE: this is derived from Henry Spencer's regexp code, and should not
 * confused with the original package (see point 3 below).  Thanks, Henry!
 */

/* Additional note: this code is very heavily munged from Henry's version
 * in places.  In some spots I've traded clarity for efficiency, so don't
 * blame Henry for some of the lack of readability.
 */

/* The names of the functions have been changed from regcomp and
 * regexec to  pregcomp and pregexec in order to avoid conflicts
 * with the POSIX routines of the same names.
*/

#ifdef PERL_EXT_RE_BUILD
/* need to replace pregcomp et al, so enable that */
#  ifndef PERL_IN_XSUB_RE
#    define PERL_IN_XSUB_RE
#  endif
/* need access to debugger hooks */
#  if defined(PERL_EXT_RE_DEBUG) && !defined(DEBUGGING)
#    define DEBUGGING
#  endif
#endif

#ifdef PERL_IN_XSUB_RE
/* We *really* need to overwrite these symbols: */
#  define Perl_regexec_flags my_regexec
#  define Perl_regdump my_regdump
#  define Perl_regprop my_regprop
#  define Perl_re_intuit_start my_re_intuit_start
/* *These* symbols are masked to allow static link. */
#  define Perl_pregexec my_pregexec
#  define Perl_reginitcolors my_reginitcolors
#  define Perl_regclass_swash my_regclass_swash

#  define PERL_NO_GET_CONTEXT
#endif

/*
 * pregcomp and pregexec -- regsub and regerror are not used in perl
 *
 *	Copyright (c) 1986 by University of Toronto.
 *	Written by Henry Spencer.  Not derived from licensed software.
 *
 *	Permission is granted to anyone to use this software for any
 *	purpose on any computer system, and to redistribute it freely,
 *	subject to the following restrictions:
 *
 *	1. The author is not responsible for the consequences of use of
 *		this software, no matter how awful, even if they arise
 *		from defects in it.
 *
 *	2. The origin of this software must not be misrepresented, either
 *		by explicit claim or by omission.
 *
 *	3. Altered versions must be plainly marked as such, and must not
 *		be misrepresented as being the original software.
 *
 ****    Alterations to Henry's code are...
 ****
 ****    Copyright (C) 1991, 1992, 1993, 1994, 1995, 1996, 1997, 1998, 1999,
 ****    2000, 2001, 2002, 2003, 2004, 2005, 2006, by Larry Wall and others
 ****
 ****    You may distribute under the terms of either the GNU General Public
 ****    License or the Artistic License, as specified in the README file.
 *
 * Beware that some of this code is subtly aware of the way operator
 * precedence is structured in regular expressions.  Serious changes in
 * regular-expression syntax might require a total rethink.
 */
#include "EXTERN.h"
#define PERL_IN_REGEXEC_C
#include "perl.h"

#include "regcomp.h"

#define RF_tainted	1		/* tainted information used? */
#define RF_warned	2		/* warned about big count? */
#define RF_evaled	4		/* Did an EVAL with setting? */
#define RF_utf8		8		/* String contains multibyte chars? */

#define UTF ((PL_reg_flags & RF_utf8) != 0)

#define RS_init		1		/* eval environment created */
#define RS_set		2		/* replsv value is set */

#ifndef STATIC
#define	STATIC	static
#endif

#define REGINCLASS(p,c)  (ANYOF_FLAGS(p) ? reginclass(p,c,0,0) : ANYOF_BITMAP_TEST(p,*(c)))

/*
 * Forwards.
 */

#define CHR_SVLEN(sv) (do_utf8 ? sv_len_utf8(sv) : SvCUR(sv))
#define CHR_DIST(a,b) (PL_reg_match_utf8 ? utf8_distance(a,b) : a - b)

#define reghop_c(pos,off) ((char*)reghop((U8*)pos, off))
#define reghopmaybe_c(pos,off) ((char*)reghopmaybe((U8*)pos, off))
#define HOP(pos,off) (PL_reg_match_utf8 ? reghop((U8*)pos, off) : (U8*)(pos + off))
#define HOPMAYBE(pos,off) (PL_reg_match_utf8 ? reghopmaybe((U8*)pos, off) : (U8*)(pos + off))
#define HOPc(pos,off) ((char*)HOP(pos,off))
#define HOPMAYBEc(pos,off) ((char*)HOPMAYBE(pos,off))

#define HOPBACK(pos, off) (		\
    (PL_reg_match_utf8)			\
	? reghopmaybe((U8*)pos, -off)	\
    : (pos - off >= PL_bostr)		\
	? (U8*)(pos - off)		\
    : (U8*)NULL				\
)
#define HOPBACKc(pos, off) (char*)HOPBACK(pos, off)

#define reghop3_c(pos,off,lim) ((char*)reghop3((U8*)pos, off, (U8*)lim))
#define reghopmaybe3_c(pos,off,lim) ((char*)reghopmaybe3((U8*)pos, off, (U8*)lim))
#define HOP3(pos,off,lim) (PL_reg_match_utf8 ? reghop3((U8*)pos, off, (U8*)lim) : (U8*)(pos + off))
#define HOPMAYBE3(pos,off,lim) (PL_reg_match_utf8 ? reghopmaybe3((U8*)pos, off, (U8*)lim) : (U8*)(pos + off))
#define HOP3c(pos,off,lim) ((char*)HOP3(pos,off,lim))
#define HOPMAYBE3c(pos,off,lim) ((char*)HOPMAYBE3(pos,off,lim))

#define LOAD_UTF8_CHARCLASS(class,str) STMT_START { \
    if (!CAT2(PL_utf8_,class)) { bool ok; ENTER; save_re_context(); ok=CAT2(is_utf8_,class)((const U8*)str); assert(ok); LEAVE; } } STMT_END
#define LOAD_UTF8_CHARCLASS_ALNUM() LOAD_UTF8_CHARCLASS(alnum,"a")
#define LOAD_UTF8_CHARCLASS_DIGIT() LOAD_UTF8_CHARCLASS(digit,"0")
#define LOAD_UTF8_CHARCLASS_SPACE() LOAD_UTF8_CHARCLASS(space," ")
#define LOAD_UTF8_CHARCLASS_MARK()  LOAD_UTF8_CHARCLASS(mark, "\xcd\x86")

/* for use after a quantifier and before an EXACT-like node -- japhy */
#define JUMPABLE(rn) ( \
    OP(rn) == OPEN || OP(rn) == CLOSE || OP(rn) == EVAL || \
    OP(rn) == SUSPEND || OP(rn) == IFMATCH || \
    OP(rn) == PLUS || OP(rn) == MINMOD || \
    (PL_regkind[(U8)OP(rn)] == CURLY && ARG1(rn) > 0) \
)

#define HAS_TEXT(rn) ( \
    PL_regkind[(U8)OP(rn)] == EXACT || PL_regkind[(U8)OP(rn)] == REF \
)

/*
  Search for mandatory following text node; for lookahead, the text must
  follow but for lookbehind (rn->flags != 0) we skip to the next step.
*/
#define FIND_NEXT_IMPT(rn) STMT_START { \
    while (JUMPABLE(rn)) \
	if (OP(rn) == SUSPEND || PL_regkind[(U8)OP(rn)] == CURLY) \
	    rn = NEXTOPER(NEXTOPER(rn)); \
	else if (OP(rn) == PLUS) \
	    rn = NEXTOPER(rn); \
	else if (OP(rn) == IFMATCH) \
	    rn = (rn->flags == 0) ? NEXTOPER(NEXTOPER(rn)) : rn + ARG(rn); \
	else rn += NEXT_OFF(rn); \
} STMT_END 

static void restore_pos(pTHX_ void *arg);

STATIC CHECKPOINT
S_regcppush(pTHX_ I32 parenfloor)
{
    dVAR;
    const int retval = PL_savestack_ix;
#define REGCP_PAREN_ELEMS 4
    const int paren_elems_to_push = (PL_regsize - parenfloor) * REGCP_PAREN_ELEMS;
    int p;

    if (paren_elems_to_push < 0)
	Perl_croak(aTHX_ "panic: paren_elems_to_push < 0");

#define REGCP_OTHER_ELEMS 6
    SSGROW(paren_elems_to_push + REGCP_OTHER_ELEMS);
    for (p = PL_regsize; p > parenfloor; p--) {
/* REGCP_PARENS_ELEMS are pushed per pairs of parentheses. */
	SSPUSHINT(PL_regendp[p]);
	SSPUSHINT(PL_regstartp[p]);
	SSPUSHPTR(PL_reg_start_tmp[p]);
	SSPUSHINT(p);
    }
/* REGCP_OTHER_ELEMS are pushed in any case, parentheses or no. */
    SSPUSHINT(PL_regsize);
    SSPUSHINT(*PL_reglastparen);
    SSPUSHINT(*PL_reglastcloseparen);
    SSPUSHPTR(PL_reginput);
#define REGCP_FRAME_ELEMS 2
/* REGCP_FRAME_ELEMS are part of the REGCP_OTHER_ELEMS and
 * are needed for the regexp context stack bookkeeping. */
    SSPUSHINT(paren_elems_to_push + REGCP_OTHER_ELEMS - REGCP_FRAME_ELEMS);
    SSPUSHINT(SAVEt_REGCONTEXT); /* Magic cookie. */

    return retval;
}

/* These are needed since we do not localize EVAL nodes: */
#  define REGCP_SET(cp)  DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,		\
			     "  Setting an EVAL scope, savestack=%"IVdf"\n",	\
			     (IV)PL_savestack_ix)); cp = PL_savestack_ix

#  define REGCP_UNWIND(cp)  DEBUG_EXECUTE_r(cp != PL_savestack_ix ?		\
				PerlIO_printf(Perl_debug_log,		\
				"  Clearing an EVAL scope, savestack=%"IVdf"..%"IVdf"\n", \
				(IV)(cp), (IV)PL_savestack_ix) : 0); regcpblow(cp)

STATIC char *
S_regcppop(pTHX)
{
    dVAR;
    I32 i;
    U32 paren = 0;
    char *input;

    GET_RE_DEBUG_FLAGS_DECL;

    /* Pop REGCP_OTHER_ELEMS before the parentheses loop starts. */
    i = SSPOPINT;
    assert(i == SAVEt_REGCONTEXT); /* Check that the magic cookie is there. */
    i = SSPOPINT; /* Parentheses elements to pop. */
    input = (char *) SSPOPPTR;
    *PL_reglastcloseparen = SSPOPINT;
    *PL_reglastparen = SSPOPINT;
    PL_regsize = SSPOPINT;

    /* Now restore the parentheses context. */
    for (i -= (REGCP_OTHER_ELEMS - REGCP_FRAME_ELEMS);
	 i > 0; i -= REGCP_PAREN_ELEMS) {
	I32 tmps;
	paren = (U32)SSPOPINT;
	PL_reg_start_tmp[paren] = (char *) SSPOPPTR;
	PL_regstartp[paren] = SSPOPINT;
	tmps = SSPOPINT;
	if (paren <= *PL_reglastparen)
	    PL_regendp[paren] = tmps;
	DEBUG_EXECUTE_r(
	    PerlIO_printf(Perl_debug_log,
			  "     restoring \\%"UVuf" to %"IVdf"(%"IVdf")..%"IVdf"%s\n",
			  (UV)paren, (IV)PL_regstartp[paren],
			  (IV)(PL_reg_start_tmp[paren] - PL_bostr),
			  (IV)PL_regendp[paren],
			  (paren > *PL_reglastparen ? "(no)" : ""));
	);
    }
    DEBUG_EXECUTE_r(
	if ((I32)(*PL_reglastparen + 1) <= PL_regnpar) {
	    PerlIO_printf(Perl_debug_log,
			  "     restoring \\%"IVdf"..\\%"IVdf" to undef\n",
			  (IV)(*PL_reglastparen + 1), (IV)PL_regnpar);
	}
    );
#if 1
    /* It would seem that the similar code in regtry()
     * already takes care of this, and in fact it is in
     * a better location to since this code can #if 0-ed out
     * but the code in regtry() is needed or otherwise tests
     * requiring null fields (pat.t#187 and split.t#{13,14}
     * (as of patchlevel 7877)  will fail.  Then again,
     * this code seems to be necessary or otherwise
     * building DynaLoader will fail:
     * "Error: '*' not in typemap in DynaLoader.xs, line 164"
     * --jhi */
    for (paren = *PL_reglastparen + 1; (I32)paren <= PL_regnpar; paren++) {
	if ((I32)paren > PL_regsize)
	    PL_regstartp[paren] = -1;
	PL_regendp[paren] = -1;
    }
#endif
    return input;
}

typedef struct re_cc_state
{
    I32 ss;
    regnode *node;
    struct re_cc_state *prev;
    CURCUR *cc;
    regexp *re;
} re_cc_state;

#define regcpblow(cp) LEAVE_SCOPE(cp)	/* Ignores regcppush()ed data. */

#define TRYPAREN(paren, n, input, where) {			\
    if (paren) {						\
	if (n) {						\
	    PL_regstartp[paren] = HOPc(input, -1) - PL_bostr;	\
	    PL_regendp[paren] = input - PL_bostr;		\
	}							\
	else							\
	    PL_regendp[paren] = -1;				\
    }								\
    REGMATCH(next, where);					\
    if (result)							\
	sayYES;							\
    if (paren && n)						\
	PL_regendp[paren] = -1;					\
}


/*
 * pregexec and friends
 */

/*
 - pregexec - match a regexp against a string
 */
I32
Perl_pregexec(pTHX_ register regexp *prog, char *stringarg, register char *strend,
	 char *strbeg, I32 minend, SV *screamer, U32 nosave)
/* strend: pointer to null at end of string */
/* strbeg: real beginning of string */
/* minend: end of match must be >=minend after stringarg. */
/* nosave: For optimizations. */
{
    return
	regexec_flags(prog, stringarg, strend, strbeg, minend, screamer, NULL,
		      nosave ? 0 : REXEC_COPY_STR);
}

STATIC void
S_cache_re(pTHX_ regexp *prog)
{
    dVAR;
    PL_regprecomp = prog->precomp;		/* Needed for FAIL. */
#ifdef DEBUGGING
    PL_regprogram = prog->program;
#endif
    PL_regnpar = prog->nparens;
    PL_regdata = prog->data;
    PL_reg_re = prog;
}

/*
 * Need to implement the following flags for reg_anch:
 *
 * USE_INTUIT_NOML		- Useful to call re_intuit_start() first
 * USE_INTUIT_ML
 * INTUIT_AUTORITATIVE_NOML	- Can trust a positive answer
 * INTUIT_AUTORITATIVE_ML
 * INTUIT_ONCE_NOML		- Intuit can match in one location only.
 * INTUIT_ONCE_ML
 *
 * Another flag for this function: SECOND_TIME (so that float substrs
 * with giant delta may be not rechecked).
 */

/* Assumptions: if ANCH_GPOS, then strpos is anchored. XXXX Check GPOS logic */

/* If SCREAM, then SvPVX_const(sv) should be compatible with strpos and strend.
   Otherwise, only SvCUR(sv) is used to get strbeg. */

/* XXXX We assume that strpos is strbeg unless sv. */

/* XXXX Some places assume that there is a fixed substring.
	An update may be needed if optimizer marks as "INTUITable"
	RExen without fixed substrings.  Similarly, it is assumed that
	lengths of all the strings are no more than minlen, thus they
	cannot come from lookahead.
	(Or minlen should take into account lookahead.) */

/* A failure to find a constant substring means that there is no need to make
   an expensive call to REx engine, thus we celebrate a failure.  Similarly,
   finding a substring too deep into the string means that less calls to
   regtry() should be needed.

   REx compiler's optimizer found 4 possible hints:
	a) Anchored substring;
	b) Fixed substring;
	c) Whether we are anchored (beginning-of-line or \G);
	d) First node (of those at offset 0) which may distingush positions;
   We use a)b)d) and multiline-part of c), and try to find a position in the
   string which does not contradict any of them.
 */

/* Most of decisions we do here should have been done at compile time.
   The nodes of the REx which we used for the search should have been
   deleted from the finite automaton. */

char *
Perl_re_intuit_start(pTHX_ regexp *prog, SV *sv, char *strpos,
		     char *strend, U32 flags, re_scream_pos_data *data)
{
    dVAR;
    register I32 start_shift = 0;
    /* Should be nonnegative! */
    register I32 end_shift   = 0;
    register char *s;
    register SV *check;
    char *strbeg;
    char *t;
    const int do_utf8 = sv ? SvUTF8(sv) : 0;	/* if no sv we have to assume bytes */
    I32 ml_anch;
    register char *other_last = NULL;	/* other substr checked before this */
    char *check_at = NULL;		/* check substr found at this pos */
    const I32 multiline = prog->reganch & PMf_MULTILINE;
#ifdef DEBUGGING
    const char * const i_strpos = strpos;
    SV * const dsv = PERL_DEBUG_PAD_ZERO(0);
#endif

    GET_RE_DEBUG_FLAGS_DECL;

    RX_MATCH_UTF8_set(prog,do_utf8);

    if (prog->reganch & ROPT_UTF8) {
	DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
			      "UTF-8 regex...\n"));
	PL_reg_flags |= RF_utf8;
    }

    DEBUG_EXECUTE_r({
	 const char *s   = PL_reg_match_utf8 ?
	                 sv_uni_display(dsv, sv, 60, UNI_DISPLAY_REGEX) :
	                 strpos;
	 const int   len = PL_reg_match_utf8 ?
	                 strlen(s) : strend - strpos;
	 if (!PL_colorset)
	      reginitcolors();
	 if (PL_reg_match_utf8)
	     DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
				   "UTF-8 target...\n"));
	 PerlIO_printf(Perl_debug_log,
		       "%sGuessing start of match, REx%s \"%s%.60s%s%s\" against \"%s%.*s%s%s\"...\n",
		       PL_colors[4], PL_colors[5], PL_colors[0],
		       prog->precomp,
		       PL_colors[1],
		       (strlen(prog->precomp) > 60 ? "..." : ""),
		       PL_colors[0],
		       (int)(len > 60 ? 60 : len),
		       s, PL_colors[1],
		       (len > 60 ? "..." : "")
	      );
    });

    /* CHR_DIST() would be more correct here but it makes things slow. */
    if (prog->minlen > strend - strpos) {
	DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
			      "String too short... [re_intuit_start]\n"));
	goto fail;
    }
    strbeg = (sv && SvPOK(sv)) ? strend - SvCUR(sv) : strpos;
    PL_regeol = strend;
    if (do_utf8) {
	if (!prog->check_utf8 && prog->check_substr)
	    to_utf8_substr(prog);
	check = prog->check_utf8;
    } else {
	if (!prog->check_substr && prog->check_utf8)
	    to_byte_substr(prog);
	check = prog->check_substr;
    }
   if (check == &PL_sv_undef) {
	DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
		"Non-utf string cannot match utf check string\n"));
	goto fail;
    }
    if (prog->reganch & ROPT_ANCH) {	/* Match at beg-of-str or after \n */
	ml_anch = !( (prog->reganch & ROPT_ANCH_SINGLE)
		     || ( (prog->reganch & ROPT_ANCH_BOL)
			  && !multiline ) );	/* Check after \n? */

	if (!ml_anch) {
	  if ( !(prog->reganch & (ROPT_ANCH_GPOS /* Checked by the caller */
				  | ROPT_IMPLICIT)) /* not a real BOL */
	       /* SvCUR is not set on references: SvRV and SvPVX_const overlap */
	       && sv && !SvROK(sv)
	       && (strpos != strbeg)) {
	      DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "Not at start...\n"));
	      goto fail;
	  }
	  if (prog->check_offset_min == prog->check_offset_max &&
	      !(prog->reganch & ROPT_CANY_SEEN)) {
	    /* Substring at constant offset from beg-of-str... */
	    I32 slen;

	    s = HOP3c(strpos, prog->check_offset_min, strend);
	    if (SvTAIL(check)) {
		slen = SvCUR(check);	/* >= 1 */

		if ( strend - s > slen || strend - s < slen - 1
		     || (strend - s == slen && strend[-1] != '\n')) {
		    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "String too long...\n"));
		    goto fail_finish;
		}
		/* Now should match s[0..slen-2] */
		slen--;
		if (slen && (*SvPVX_const(check) != *s
			     || (slen > 1
				 && memNE(SvPVX_const(check), s, slen)))) {
		  report_neq:
		    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "String not equal...\n"));
		    goto fail_finish;
		}
	    }
	    else if (*SvPVX_const(check) != *s
		     || ((slen = SvCUR(check)) > 1
			 && memNE(SvPVX_const(check), s, slen)))
		goto report_neq;
	    check_at = s;
	    goto success_at_start;
	  }
	}
	/* Match is anchored, but substr is not anchored wrt beg-of-str. */
	s = strpos;
	start_shift = prog->check_offset_min; /* okay to underestimate on CC */
	end_shift = prog->minlen - start_shift -
	    CHR_SVLEN(check) + (SvTAIL(check) != 0);
	if (!ml_anch) {
	    const I32 end = prog->check_offset_max + CHR_SVLEN(check)
					 - (SvTAIL(check) != 0);
	    const I32 eshift = CHR_DIST((U8*)strend, (U8*)s) - end;

	    if (end_shift < eshift)
		end_shift = eshift;
	}
    }
    else {				/* Can match at random position */
	ml_anch = 0;
	s = strpos;
	start_shift = prog->check_offset_min; /* okay to underestimate on CC */
	/* Should be nonnegative! */
	end_shift = prog->minlen - start_shift -
	    CHR_SVLEN(check) + (SvTAIL(check) != 0);
    }

#ifdef DEBUGGING	/* 7/99: reports of failure (with the older version) */
    if (end_shift < 0)
	Perl_croak(aTHX_ "panic: end_shift");
#endif

  restart:
    /* Find a possible match in the region s..strend by looking for
       the "check" substring in the region corrected by start/end_shift. */
    if (flags & REXEC_SCREAM) {
	I32 p = -1;			/* Internal iterator of scream. */
	I32 * const pp = data ? data->scream_pos : &p;

	if (PL_screamfirst[BmRARE(check)] >= 0
	    || ( BmRARE(check) == '\n'
		 && (BmPREVIOUS(check) == SvCUR(check) - 1)
		 && SvTAIL(check) ))
	    s = screaminstr(sv, check,
			    start_shift + (s - strbeg), end_shift, pp, 0);
	else
	    goto fail_finish;
	/* we may be pointing at the wrong string */
	if (s && RX_MATCH_COPIED(prog))
	    s = strbeg + (s - SvPVX_const(sv));
	if (data)
	    *data->scream_olds = s;
    }
    else if (prog->reganch & ROPT_CANY_SEEN)
	s = fbm_instr((U8*)(s + start_shift),
		      (U8*)(strend - end_shift),
		      check, multiline ? FBMrf_MULTILINE : 0);
    else
	s = fbm_instr(HOP3(s, start_shift, strend),
		      HOP3(strend, -end_shift, strbeg),
		      check, multiline ? FBMrf_MULTILINE : 0);

    /* Update the count-of-usability, remove useless subpatterns,
	unshift s.  */

    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "%s %s substr \"%s%.*s%s\"%s%s",
			  (s ? "Found" : "Did not find"),
			  (check == (do_utf8 ? prog->anchored_utf8 : prog->anchored_substr) ? "anchored" : "floating"),
			  PL_colors[0],
			  (int)(SvCUR(check) - (SvTAIL(check)!=0)),
			  SvPVX_const(check),
			  PL_colors[1], (SvTAIL(check) ? "$" : ""),
			  (s ? " at offset " : "...\n") ) );

    if (!s)
	goto fail_finish;

    check_at = s;

    /* Finish the diagnostic message */
    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "%ld...\n", (long)(s - i_strpos)) );

    /* Got a candidate.  Check MBOL anchoring, and the *other* substr.
       Start with the other substr.
       XXXX no SCREAM optimization yet - and a very coarse implementation
       XXXX /ttx+/ results in anchored="ttx", floating="x".  floating will
		*always* match.  Probably should be marked during compile...
       Probably it is right to do no SCREAM here...
     */

    if (do_utf8 ? (prog->float_utf8 && prog->anchored_utf8) : (prog->float_substr && prog->anchored_substr)) {
	/* Take into account the "other" substring. */
	/* XXXX May be hopelessly wrong for UTF... */
	if (!other_last)
	    other_last = strpos;
	if (check == (do_utf8 ? prog->float_utf8 : prog->float_substr)) {
	  do_other_anchored:
	    {
		char * const last = HOP3c(s, -start_shift, strbeg);
		char *last1, *last2;
		char *s1 = s;
		SV* must;

		t = s - prog->check_offset_max;
		if (s - strpos > prog->check_offset_max  /* signed-corrected t > strpos */
		    && (!do_utf8
			|| ((t = reghopmaybe3_c(s, -(prog->check_offset_max), strpos))
			    && t > strpos)))
		    /* EMPTY */;
		else
		    t = strpos;
		t = HOP3c(t, prog->anchored_offset, strend);
		if (t < other_last)	/* These positions already checked */
		    t = other_last;
		last2 = last1 = HOP3c(strend, -prog->minlen, strbeg);
		if (last < last1)
		    last1 = last;
 /* XXXX It is not documented what units *_offsets are in.  Assume bytes.  */
		/* On end-of-str: see comment below. */
		must = do_utf8 ? prog->anchored_utf8 : prog->anchored_substr;
		if (must == &PL_sv_undef) {
		    s = (char*)NULL;
		    DEBUG_EXECUTE_r(must = prog->anchored_utf8);	/* for debug */
		}
		else
		    s = fbm_instr(
			(unsigned char*)t,
			HOP3(HOP3(last1, prog->anchored_offset, strend)
				+ SvCUR(must), -(SvTAIL(must)!=0), strbeg),
			must,
			multiline ? FBMrf_MULTILINE : 0
		    );
		DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
			"%s anchored substr \"%s%.*s%s\"%s",
			(s ? "Found" : "Contradicts"),
			PL_colors[0],
			  (int)(SvCUR(must)
			  - (SvTAIL(must)!=0)),
			  SvPVX_const(must),
			  PL_colors[1], (SvTAIL(must) ? "$" : "")));
		if (!s) {
		    if (last1 >= last2) {
			DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
						", giving up...\n"));
			goto fail_finish;
		    }
		    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
			", trying floating at offset %ld...\n",
			(long)(HOP3c(s1, 1, strend) - i_strpos)));
		    other_last = HOP3c(last1, prog->anchored_offset+1, strend);
		    s = HOP3c(last, 1, strend);
		    goto restart;
		}
		else {
		    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, " at offset %ld...\n",
			  (long)(s - i_strpos)));
		    t = HOP3c(s, -prog->anchored_offset, strbeg);
		    other_last = HOP3c(s, 1, strend);
		    s = s1;
		    if (t == strpos)
			goto try_at_start;
		    goto try_at_offset;
		}
	    }
	}
	else {		/* Take into account the floating substring. */
	    char *last, *last1;
	    char *s1 = s;
	    SV* must;

	    t = HOP3c(s, -start_shift, strbeg);
	    last1 = last =
		HOP3c(strend, -prog->minlen + prog->float_min_offset, strbeg);
	    if (CHR_DIST((U8*)last, (U8*)t) > prog->float_max_offset)
		last = HOP3c(t, prog->float_max_offset, strend);
	    s = HOP3c(t, prog->float_min_offset, strend);
	    if (s < other_last)
		s = other_last;
 /* XXXX It is not documented what units *_offsets are in.  Assume bytes.  */
	    must = do_utf8 ? prog->float_utf8 : prog->float_substr;
	    /* fbm_instr() takes into account exact value of end-of-str
	       if the check is SvTAIL(ed).  Since false positives are OK,
	       and end-of-str is not later than strend we are OK. */
	    if (must == &PL_sv_undef) {
		s = (char*)NULL;
		DEBUG_EXECUTE_r(must = prog->float_utf8);	/* for debug message */
	    }
	    else
		s = fbm_instr((unsigned char*)s,
			      (unsigned char*)last + SvCUR(must)
				  - (SvTAIL(must)!=0),
			      must, multiline ? FBMrf_MULTILINE : 0);
	    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "%s floating substr \"%s%.*s%s\"%s",
		    (s ? "Found" : "Contradicts"),
		    PL_colors[0],
		      (int)(SvCUR(must) - (SvTAIL(must)!=0)),
		      SvPVX_const(must),
		      PL_colors[1], (SvTAIL(must) ? "$" : "")));
	    if (!s) {
		if (last1 == last) {
		    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
					    ", giving up...\n"));
		    goto fail_finish;
		}
		DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
		    ", trying anchored starting at offset %ld...\n",
		    (long)(s1 + 1 - i_strpos)));
		other_last = last;
		s = HOP3c(t, 1, strend);
		goto restart;
	    }
	    else {
		DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, " at offset %ld...\n",
		      (long)(s - i_strpos)));
		other_last = s; /* Fix this later. --Hugo */
		s = s1;
		if (t == strpos)
		    goto try_at_start;
		goto try_at_offset;
	    }
	}
    }

    t = s - prog->check_offset_max;
    if (s - strpos > prog->check_offset_max  /* signed-corrected t > strpos */
        && (!do_utf8
	    || ((t = reghopmaybe3_c(s, -prog->check_offset_max, strpos))
		 && t > strpos))) {
	/* Fixed substring is found far enough so that the match
	   cannot start at strpos. */
      try_at_offset:
	if (ml_anch && t[-1] != '\n') {
	    /* Eventually fbm_*() should handle this, but often
	       anchored_offset is not 0, so this check will not be wasted. */
	    /* XXXX In the code below we prefer to look for "^" even in
	       presence of anchored substrings.  And we search even
	       beyond the found float position.  These pessimizations
	       are historical artefacts only.  */
	  find_anchor:
	    while (t < strend - prog->minlen) {
		if (*t == '\n') {
		    if (t < check_at - prog->check_offset_min) {
			if (do_utf8 ? prog->anchored_utf8 : prog->anchored_substr) {
			    /* Since we moved from the found position,
			       we definitely contradict the found anchored
			       substr.  Due to the above check we do not
			       contradict "check" substr.
			       Thus we can arrive here only if check substr
			       is float.  Redo checking for "other"=="fixed".
			     */
			    strpos = t + 1;			
			    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "Found /%s^%s/m at offset %ld, rescanning for anchored from offset %ld...\n",
				PL_colors[0], PL_colors[1], (long)(strpos - i_strpos), (long)(strpos - i_strpos + prog->anchored_offset)));
			    goto do_other_anchored;
			}
			/* We don't contradict the found floating substring. */
			/* XXXX Why not check for STCLASS? */
			s = t + 1;
			DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "Found /%s^%s/m at offset %ld...\n",
			    PL_colors[0], PL_colors[1], (long)(s - i_strpos)));
			goto set_useful;
		    }
		    /* Position contradicts check-string */
		    /* XXXX probably better to look for check-string
		       than for "\n", so one should lower the limit for t? */
		    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "Found /%s^%s/m, restarting lookup for check-string at offset %ld...\n",
			PL_colors[0], PL_colors[1], (long)(t + 1 - i_strpos)));
		    other_last = strpos = s = t + 1;
		    goto restart;
		}
		t++;
	    }
	    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "Did not find /%s^%s/m...\n",
			PL_colors[0], PL_colors[1]));
	    goto fail_finish;
	}
	else {
	    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "Starting position does not contradict /%s^%s/m...\n",
			PL_colors[0], PL_colors[1]));
	}
	s = t;
      set_useful:
	++BmUSEFUL(do_utf8 ? prog->check_utf8 : prog->check_substr);	/* hooray/5 */
    }
    else {
	/* The found string does not prohibit matching at strpos,
	   - no optimization of calling REx engine can be performed,
	   unless it was an MBOL and we are not after MBOL,
	   or a future STCLASS check will fail this. */
      try_at_start:
	/* Even in this situation we may use MBOL flag if strpos is offset
	   wrt the start of the string. */
	if (ml_anch && sv && !SvROK(sv)	/* See prev comment on SvROK */
	    && (strpos != strbeg) && strpos[-1] != '\n'
	    /* May be due to an implicit anchor of m{.*foo}  */
	    && !(prog->reganch & ROPT_IMPLICIT))
	{
	    t = strpos;
	    goto find_anchor;
	}
	DEBUG_EXECUTE_r( if (ml_anch)
	    PerlIO_printf(Perl_debug_log, "Position at offset %ld does not contradict /%s^%s/m...\n",
			(long)(strpos - i_strpos), PL_colors[0], PL_colors[1]);
	);
      success_at_start:
	if (!(prog->reganch & ROPT_NAUGHTY)	/* XXXX If strpos moved? */
	    && (do_utf8 ? (
		prog->check_utf8		/* Could be deleted already */
		&& --BmUSEFUL(prog->check_utf8) < 0
		&& (prog->check_utf8 == prog->float_utf8)
	    ) : (
		prog->check_substr		/* Could be deleted already */
		&& --BmUSEFUL(prog->check_substr) < 0
		&& (prog->check_substr == prog->float_substr)
	    )))
	{
	    /* If flags & SOMETHING - do not do it many times on the same match */
	    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "... Disabling check substring...\n"));
	    SvREFCNT_dec(do_utf8 ? prog->check_utf8 : prog->check_substr);
	    if (do_utf8 ? prog->check_substr : prog->check_utf8)
		SvREFCNT_dec(do_utf8 ? prog->check_substr : prog->check_utf8);
	    prog->check_substr = prog->check_utf8 = NULL;	/* disable */
	    prog->float_substr = prog->float_utf8 = NULL;	/* clear */
	    check = NULL;			/* abort */
	    s = strpos;
	    /* XXXX This is a remnant of the old implementation.  It
	            looks wasteful, since now INTUIT can use many
	            other heuristics. */
	    prog->reganch &= ~RE_USE_INTUIT;
	}
	else
	    s = strpos;
    }

    /* Last resort... */
    /* XXXX BmUSEFUL already changed, maybe multiple change is meaningful... */
    if (prog->regstclass) {
	/* minlen == 0 is possible if regstclass is \b or \B,
	   and the fixed substr is ''$.
	   Since minlen is already taken into account, s+1 is before strend;
	   accidentally, minlen >= 1 guaranties no false positives at s + 1
	   even for \b or \B.  But (minlen? 1 : 0) below assumes that
	   regstclass does not come from lookahead...  */
	/* If regstclass takes bytelength more than 1: If charlength==1, OK.
	   This leaves EXACTF only, which is dealt with in find_byclass().  */
        const U8* const str = (U8*)STRING(prog->regstclass);
        const int cl_l = (PL_regkind[(U8)OP(prog->regstclass)] == EXACT
		    ? CHR_DIST(str+STR_LEN(prog->regstclass), str)
		    : 1);
	const char * const endpos = (prog->anchored_substr || prog->anchored_utf8 || ml_anch)
		? HOP3c(s, (prog->minlen ? cl_l : 0), strend)
		: (prog->float_substr || prog->float_utf8
		   ? HOP3c(HOP3c(check_at, -start_shift, strbeg),
			   cl_l, strend)
		   : strend);

	t = s;
	cache_re(prog);
        s = find_byclass(prog, prog->regstclass, s, endpos, 1);
	if (!s) {
#ifdef DEBUGGING
	    const char *what = NULL;
#endif
	    if (endpos == strend) {
		DEBUG_EXECUTE_r( PerlIO_printf(Perl_debug_log,
				"Could not match STCLASS...\n") );
		goto fail;
	    }
	    DEBUG_EXECUTE_r( PerlIO_printf(Perl_debug_log,
				   "This position contradicts STCLASS...\n") );
	    if ((prog->reganch & ROPT_ANCH) && !ml_anch)
		goto fail;
	    /* Contradict one of substrings */
	    if (prog->anchored_substr || prog->anchored_utf8) {
		if ((do_utf8 ? prog->anchored_utf8 : prog->anchored_substr) == check) {
		    DEBUG_EXECUTE_r( what = "anchored" );
		  hop_and_restart:
		    s = HOP3c(t, 1, strend);
		    if (s + start_shift + end_shift > strend) {
			/* XXXX Should be taken into account earlier? */
			DEBUG_EXECUTE_r( PerlIO_printf(Perl_debug_log,
					       "Could not match STCLASS...\n") );
			goto fail;
		    }
		    if (!check)
			goto giveup;
		    DEBUG_EXECUTE_r( PerlIO_printf(Perl_debug_log,
				"Looking for %s substr starting at offset %ld...\n",
				 what, (long)(s + start_shift - i_strpos)) );
		    goto restart;
		}
		/* Have both, check_string is floating */
		if (t + start_shift >= check_at) /* Contradicts floating=check */
		    goto retry_floating_check;
		/* Recheck anchored substring, but not floating... */
		s = check_at;
		if (!check)
		    goto giveup;
		DEBUG_EXECUTE_r( PerlIO_printf(Perl_debug_log,
			  "Looking for anchored substr starting at offset %ld...\n",
			  (long)(other_last - i_strpos)) );
		goto do_other_anchored;
	    }
	    /* Another way we could have checked stclass at the
               current position only: */
	    if (ml_anch) {
		s = t = t + 1;
		if (!check)
		    goto giveup;
		DEBUG_EXECUTE_r( PerlIO_printf(Perl_debug_log,
			  "Looking for /%s^%s/m starting at offset %ld...\n",
			  PL_colors[0], PL_colors[1], (long)(t - i_strpos)) );
		goto try_at_offset;
	    }
	    if (!(do_utf8 ? prog->float_utf8 : prog->float_substr))	/* Could have been deleted */
		goto fail;
	    /* Check is floating subtring. */
	  retry_floating_check:
	    t = check_at - start_shift;
	    DEBUG_EXECUTE_r( what = "floating" );
	    goto hop_and_restart;
	}
	if (t != s) {
            DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
			"By STCLASS: moving %ld --> %ld\n",
                                  (long)(t - i_strpos), (long)(s - i_strpos))
                   );
        }
        else {
            DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
                                  "Does not contradict STCLASS...\n"); 
                   );
        }
    }
  giveup:
    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "%s%s:%s match at offset %ld\n",
			  PL_colors[4], (check ? "Guessed" : "Giving up"),
			  PL_colors[5], (long)(s - i_strpos)) );
    return s;

  fail_finish:				/* Substring not found */
    if (prog->check_substr || prog->check_utf8)		/* could be removed already */
	BmUSEFUL(do_utf8 ? prog->check_utf8 : prog->check_substr) += 5; /* hooray */
  fail:
    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "%sMatch rejected by optimizer%s\n",
			  PL_colors[4], PL_colors[5]));
    return NULL;
}

/* We know what class REx starts with.  Try to find this position... */
STATIC char *
S_find_byclass(pTHX_ regexp * prog, regnode *c, char *s, const char *strend, I32 norun)
{
	dVAR;
	const I32 doevery = (prog->reganch & ROPT_SKIP) == 0;
	char *m;
	STRLEN ln;
	STRLEN lnc;
	register STRLEN uskip;
	unsigned int c1;
	unsigned int c2;
	char *e;
	register I32 tmp = 1;	/* Scratch variable? */
	register const bool do_utf8 = PL_reg_match_utf8;

	/* We know what class it must start with. */
	switch (OP(c)) {
	case ANYOF:
	    if (do_utf8) {
		 while (s + (uskip = UTF8SKIP(s)) <= strend) {
		      if ((ANYOF_FLAGS(c) & ANYOF_UNICODE) ||
			  !UTF8_IS_INVARIANT((U8)s[0]) ?
			  reginclass(c, (U8*)s, 0, do_utf8) :
			  REGINCLASS(c, (U8*)s)) {
			   if (tmp && (norun || regtry(prog, s)))
				goto got_it;
			   else
				tmp = doevery;
		      }
		      else 
			   tmp = 1;
		      s += uskip;
		 }
	    }
	    else {
		 while (s < strend) {
		      STRLEN skip = 1;

		      if (REGINCLASS(c, (U8*)s) ||
			  (ANYOF_FOLD_SHARP_S(c, s, strend) &&
			   /* The assignment of 2 is intentional:
			    * for the folded sharp s, the skip is 2. */
			   (skip = SHARP_S_SKIP))) {
			   if (tmp && (norun || regtry(prog, s)))
				goto got_it;
			   else
				tmp = doevery;
		      }
		      else 
			   tmp = 1;
		      s += skip;
		 }
	    }
	    break;
	case CANY:
	    while (s < strend) {
	        if (tmp && (norun || regtry(prog, s)))
		    goto got_it;
		else
		    tmp = doevery;
		s++;
	    }
	    break;
	case EXACTF:
	    m   = STRING(c);
	    ln  = STR_LEN(c);	/* length to match in octets/bytes */
	    lnc = (I32) ln;	/* length to match in characters */
	    if (UTF) {
	        STRLEN ulen1, ulen2;
		U8 *sm = (U8 *) m;
		U8 tmpbuf1[UTF8_MAXBYTES_CASE+1];
		U8 tmpbuf2[UTF8_MAXBYTES_CASE+1];
		const U32 uniflags = ckWARN(WARN_UTF8) ? 0 : UTF8_ALLOW_ANY;

		to_utf8_lower((U8*)m, tmpbuf1, &ulen1);
		to_utf8_upper((U8*)m, tmpbuf2, &ulen2);

		c1 = utf8n_to_uvchr(tmpbuf1, UTF8_MAXBYTES_CASE, 
				    0, uniflags);
		c2 = utf8n_to_uvchr(tmpbuf2, UTF8_MAXBYTES_CASE,
				    0, uniflags);
		lnc = 0;
		while (sm < ((U8 *) m + ln)) {
		    lnc++;
		    sm += UTF8SKIP(sm);
		}
	    }
	    else {
		c1 = *(U8*)m;
		c2 = PL_fold[c1];
	    }
	    goto do_exactf;
	case EXACTFL:
	    m   = STRING(c);
	    ln  = STR_LEN(c);
	    lnc = (I32) ln;
	    c1 = *(U8*)m;
	    c2 = PL_fold_locale[c1];
	  do_exactf:
	    e = HOP3c(strend, -((I32)lnc), s);

	    if (norun && e < s)
		e = s;			/* Due to minlen logic of intuit() */

	    /* The idea in the EXACTF* cases is to first find the
	     * first character of the EXACTF* node and then, if
	     * necessary, case-insensitively compare the full
	     * text of the node.  The c1 and c2 are the first
	     * characters (though in Unicode it gets a bit
	     * more complicated because there are more cases
	     * than just upper and lower: one needs to use
	     * the so-called folding case for case-insensitive
	     * matching (called "loose matching" in Unicode).
	     * ibcmp_utf8() will do just that. */

	    if (do_utf8) {
	        UV c, f;
	        U8 tmpbuf [UTF8_MAXBYTES+1];
		STRLEN len, foldlen;
		const U32 uniflags = ckWARN(WARN_UTF8) ? 0 : UTF8_ALLOW_ANY;
		if (c1 == c2) {
		    /* Upper and lower of 1st char are equal -
		     * probably not a "letter". */
		    while (s <= e) {
		        c = utf8n_to_uvchr((U8*)s, UTF8_MAXBYTES, &len,
					   uniflags);
			if ( c == c1
			     && (ln == len ||
				 ibcmp_utf8(s, (char **)0, 0,  do_utf8,
					    m, (char **)0, ln, (bool)UTF))
			     && (norun || regtry(prog, s)) )
			    goto got_it;
			else {
			     U8 foldbuf[UTF8_MAXBYTES_CASE+1];
			     uvchr_to_utf8(tmpbuf, c);
			     f = to_utf8_fold(tmpbuf, foldbuf, &foldlen);
			     if ( f != c
				  && (f == c1 || f == c2)
				  && (ln == foldlen ||
				      !ibcmp_utf8((char *) foldbuf,
						  (char **)0, foldlen, do_utf8,
						  m,
						  (char **)0, ln, (bool)UTF))
				  && (norun || regtry(prog, s)) )
				  goto got_it;
			}
			s += len;
		    }
		}
		else {
		    while (s <= e) {
		      c = utf8n_to_uvchr((U8*)s, UTF8_MAXBYTES, &len,
					   uniflags);

			/* Handle some of the three Greek sigmas cases.
			 * Note that not all the possible combinations
			 * are handled here: some of them are handled
			 * by the standard folding rules, and some of
			 * them (the character class or ANYOF cases)
			 * are handled during compiletime in
			 * regexec.c:S_regclass(). */
			if (c == (UV)UNICODE_GREEK_CAPITAL_LETTER_SIGMA ||
			    c == (UV)UNICODE_GREEK_SMALL_LETTER_FINAL_SIGMA)
			    c = (UV)UNICODE_GREEK_SMALL_LETTER_SIGMA;

			if ( (c == c1 || c == c2)
			     && (ln == len ||
				 ibcmp_utf8(s, (char **)0, 0,  do_utf8,
					    m, (char **)0, ln, (bool)UTF))
			     && (norun || regtry(prog, s)) )
			    goto got_it;
			else {
			     U8 foldbuf[UTF8_MAXBYTES_CASE+1];
			     uvchr_to_utf8(tmpbuf, c);
			     f = to_utf8_fold(tmpbuf, foldbuf, &foldlen);
			     if ( f != c
				  && (f == c1 || f == c2)
				  && (ln == foldlen ||
				      !ibcmp_utf8((char *) foldbuf,
						  (char **)0, foldlen, do_utf8,
						  m,
						  (char **)0, ln, (bool)UTF))
				  && (norun || regtry(prog, s)) )
				  goto got_it;
			}
			s += len;
		    }
		}
	    }
	    else {
		if (c1 == c2)
		    while (s <= e) {
			if ( *(U8*)s == c1
			     && (ln == 1 || !(OP(c) == EXACTF
					      ? ibcmp(s, m, ln)
					      : ibcmp_locale(s, m, ln)))
			     && (norun || regtry(prog, s)) )
			    goto got_it;
			s++;
		    }
		else
		    while (s <= e) {
			if ( (*(U8*)s == c1 || *(U8*)s == c2)
			     && (ln == 1 || !(OP(c) == EXACTF
					      ? ibcmp(s, m, ln)
					      : ibcmp_locale(s, m, ln)))
			     && (norun || regtry(prog, s)) )
			    goto got_it;
			s++;
		    }
	    }
	    break;
	case BOUNDL:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case BOUND:
	    if (do_utf8) {
		if (s == PL_bostr)
		    tmp = '\n';
		else {
		    U8 * const r = reghop3((U8*)s, -1, (U8*)PL_bostr);
		    tmp = utf8n_to_uvchr(r, UTF8SKIP(r), 0, 0);
		}
		tmp = ((OP(c) == BOUND ?
			isALNUM_uni(tmp) : isALNUM_LC_uvchr(UNI_TO_NATIVE(tmp))) != 0);
		LOAD_UTF8_CHARCLASS_ALNUM();
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (tmp == !(OP(c) == BOUND ?
				 swash_fetch(PL_utf8_alnum, (U8*)s, do_utf8) :
				 isALNUM_LC_utf8((U8*)s)))
		    {
			tmp = !tmp;
			if ((norun || regtry(prog, s)))
			    goto got_it;
		    }
		    s += uskip;
		}
	    }
	    else {
		tmp = (s != PL_bostr) ? UCHARAT(s - 1) : '\n';
		tmp = ((OP(c) == BOUND ? isALNUM(tmp) : isALNUM_LC(tmp)) != 0);
		while (s < strend) {
		    if (tmp ==
			!(OP(c) == BOUND ? isALNUM(*s) : isALNUM_LC(*s))) {
			tmp = !tmp;
			if ((norun || regtry(prog, s)))
			    goto got_it;
		    }
		    s++;
		}
	    }
	    if ((!prog->minlen && tmp) && (norun || regtry(prog, s)))
		goto got_it;
	    break;
	case NBOUNDL:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case NBOUND:
	    if (do_utf8) {
		if (s == PL_bostr)
		    tmp = '\n';
		else {
		    U8 * const r = reghop3((U8*)s, -1, (U8*)PL_bostr);
		    tmp = utf8n_to_uvchr(r, UTF8SKIP(r), 0, 0);
		}
		tmp = ((OP(c) == NBOUND ?
			isALNUM_uni(tmp) : isALNUM_LC_uvchr(UNI_TO_NATIVE(tmp))) != 0);
		LOAD_UTF8_CHARCLASS_ALNUM();
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (tmp == !(OP(c) == NBOUND ?
				 swash_fetch(PL_utf8_alnum, (U8*)s, do_utf8) :
				 isALNUM_LC_utf8((U8*)s)))
			tmp = !tmp;
		    else if ((norun || regtry(prog, s)))
			goto got_it;
		    s += uskip;
		}
	    }
	    else {
		tmp = (s != PL_bostr) ? UCHARAT(s - 1) : '\n';
		tmp = ((OP(c) == NBOUND ?
			isALNUM(tmp) : isALNUM_LC(tmp)) != 0);
		while (s < strend) {
		    if (tmp ==
			!(OP(c) == NBOUND ? isALNUM(*s) : isALNUM_LC(*s)))
			tmp = !tmp;
		    else if ((norun || regtry(prog, s)))
			goto got_it;
		    s++;
		}
	    }
	    if ((!prog->minlen && !tmp) && (norun || regtry(prog, s)))
		goto got_it;
	    break;
	case ALNUM:
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_ALNUM();
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (swash_fetch(PL_utf8_alnum, (U8*)s, do_utf8)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (isALNUM(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case ALNUML:
	    PL_reg_flags |= RF_tainted;
	    if (do_utf8) {
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (isALNUM_LC_utf8((U8*)s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (isALNUM_LC(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case NALNUM:
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_ALNUM();
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (!swash_fetch(PL_utf8_alnum, (U8*)s, do_utf8)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (!isALNUM(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case NALNUML:
	    PL_reg_flags |= RF_tainted;
	    if (do_utf8) {
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (!isALNUM_LC_utf8((U8*)s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (!isALNUM_LC(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case SPACE:
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_SPACE();
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (*s == ' ' || swash_fetch(PL_utf8_space,(U8*)s, do_utf8)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (isSPACE(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case SPACEL:
	    PL_reg_flags |= RF_tainted;
	    if (do_utf8) {
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (*s == ' ' || isSPACE_LC_utf8((U8*)s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (isSPACE_LC(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case NSPACE:
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_SPACE();
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (!(*s == ' ' || swash_fetch(PL_utf8_space,(U8*)s, do_utf8))) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (!isSPACE(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case NSPACEL:
	    PL_reg_flags |= RF_tainted;
	    if (do_utf8) {
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (!(*s == ' ' || isSPACE_LC_utf8((U8*)s))) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (!isSPACE_LC(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case DIGIT:
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_DIGIT();
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (swash_fetch(PL_utf8_digit,(U8*)s, do_utf8)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (isDIGIT(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case DIGITL:
	    PL_reg_flags |= RF_tainted;
	    if (do_utf8) {
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (isDIGIT_LC_utf8((U8*)s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (isDIGIT_LC(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case NDIGIT:
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_DIGIT();
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (!swash_fetch(PL_utf8_digit,(U8*)s, do_utf8)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (!isDIGIT(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	case NDIGITL:
	    PL_reg_flags |= RF_tainted;
	    if (do_utf8) {
		while (s + (uskip = UTF8SKIP(s)) <= strend) {
		    if (!isDIGIT_LC_utf8((U8*)s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s += uskip;
		}
	    }
	    else {
		while (s < strend) {
		    if (!isDIGIT_LC(*s)) {
			if (tmp && (norun || regtry(prog, s)))
			    goto got_it;
			else
			    tmp = doevery;
		    }
		    else
			tmp = 1;
		    s++;
		}
	    }
	    break;
	default:
	    Perl_croak(aTHX_ "panic: unknown regstclass %d", (int)OP(c));
	    break;
	}
	return 0;
      got_it:
	return s;
}

/*
 - regexec_flags - match a regexp against a string
 */
I32
Perl_regexec_flags(pTHX_ register regexp *prog, char *stringarg, register char *strend,
	      char *strbeg, I32 minend, SV *sv, void *data, U32 flags)
/* strend: pointer to null at end of string */
/* strbeg: real beginning of string */
/* minend: end of match must be >=minend after stringarg. */
/* data: May be used for some additional optimizations. */
/* nosave: For optimizations. */
{
    dVAR;
    register char *s;
    register regnode *c;
    register char *startpos = stringarg;
    I32 minlen;		/* must match at least this many chars */
    I32 dontbother = 0;	/* how many characters not to try at end */
    I32 end_shift = 0;			/* Same for the end. */		/* CC */
    I32 scream_pos = -1;		/* Internal iterator of scream. */
    char *scream_olds = NULL;
    SV* oreplsv = GvSV(PL_replgv);
    const bool do_utf8 = DO_UTF8(sv);
    const I32 multiline = prog->reganch & PMf_MULTILINE;
#ifdef DEBUGGING
    SV * const dsv0 = PERL_DEBUG_PAD_ZERO(0);
    SV * const dsv1 = PERL_DEBUG_PAD_ZERO(1);
#endif

    GET_RE_DEBUG_FLAGS_DECL;

    PERL_UNUSED_ARG(data);
    RX_MATCH_UTF8_set(prog,do_utf8);

    cache_re(prog);
#ifdef DEBUGGING
    PL_regnarrate = DEBUG_r_TEST;
#endif

    /* Be paranoid... */
    if (prog == NULL || startpos == NULL) {
	Perl_croak(aTHX_ "NULL regexp parameter");
	return 0;
    }

    minlen = prog->minlen;
    if (strend - startpos < minlen) {
        DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
			      "String too short [regexec_flags]...\n"));
	goto phooey;
    }

    /* Check validity of program. */
    if (UCHARAT(prog->program) != REG_MAGIC) {
	Perl_croak(aTHX_ "corrupted regexp program");
    }

    PL_reg_flags = 0;
    PL_reg_eval_set = 0;
    PL_reg_maxiter = 0;

    if (prog->reganch & ROPT_UTF8)
	PL_reg_flags |= RF_utf8;

    /* Mark beginning of line for ^ and lookbehind. */
    PL_regbol = startpos;
    PL_bostr  = strbeg;
    PL_reg_sv = sv;

    /* Mark end of line for $ (and such) */
    PL_regeol = strend;

    /* see how far we have to get to not match where we matched before */
    PL_regtill = startpos+minend;

    /* We start without call_cc context.  */
    PL_reg_call_cc = 0;

    /* If there is a "must appear" string, look for it. */
    s = startpos;

    if (prog->reganch & ROPT_GPOS_SEEN) { /* Need to have PL_reg_ganch */
	MAGIC *mg;

	if (flags & REXEC_IGNOREPOS)	/* Means: check only at start */
	    PL_reg_ganch = startpos;
	else if (sv && SvTYPE(sv) >= SVt_PVMG
		  && SvMAGIC(sv)
		  && (mg = mg_find(sv, PERL_MAGIC_regex_global))
		  && mg->mg_len >= 0) {
	    PL_reg_ganch = strbeg + mg->mg_len;	/* Defined pos() */
	    if (prog->reganch & ROPT_ANCH_GPOS) {
	        if (s > PL_reg_ganch)
		    goto phooey;
		s = PL_reg_ganch;
	    }
	}
	else				/* pos() not defined */
	    PL_reg_ganch = strbeg;
    }

    if (!(flags & REXEC_CHECKED) && (prog->check_substr != NULL || prog->check_utf8 != NULL)) {
	re_scream_pos_data d;

	d.scream_olds = &scream_olds;
	d.scream_pos = &scream_pos;
	s = re_intuit_start(prog, sv, s, strend, flags, &d);
	if (!s) {
	    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "Not present...\n"));
	    goto phooey;	/* not present */
	}
    }

    DEBUG_EXECUTE_r({
	const char * const s0   = UTF
	    ? pv_uni_display(dsv0, (U8*)prog->precomp, prog->prelen, 60,
			  UNI_DISPLAY_REGEX)
	    : prog->precomp;
	const int len0 = UTF ? SvCUR(dsv0) : prog->prelen;
	const char * const s1 = do_utf8 ? sv_uni_display(dsv1, sv, 60,
					       UNI_DISPLAY_REGEX) : startpos;
	const int len1 = do_utf8 ? SvCUR(dsv1) : strend - startpos;
	 if (!PL_colorset)
	     reginitcolors();
	 PerlIO_printf(Perl_debug_log,
		       "%sMatching REx%s \"%s%*.*s%s%s\" against \"%s%.*s%s%s\"\n",
		       PL_colors[4], PL_colors[5], PL_colors[0],
		       len0, len0, s0,
		       PL_colors[1],
		       len0 > 60 ? "..." : "",
		       PL_colors[0],
		       (int)(len1 > 60 ? 60 : len1),
		       s1, PL_colors[1],
		       (len1 > 60 ? "..." : "")
	      );
    });

    /* Simplest case:  anchored match need be tried only once. */
    /*  [unless only anchor is BOL and multiline is set] */
    if (prog->reganch & (ROPT_ANCH & ~ROPT_ANCH_GPOS)) {
	if (s == startpos && regtry(prog, startpos))
	    goto got_it;
	else if (multiline || (prog->reganch & ROPT_IMPLICIT)
		 || (prog->reganch & ROPT_ANCH_MBOL)) /* XXXX SBOL? */
	{
	    char *end;

	    if (minlen)
		dontbother = minlen - 1;
	    end = HOP3c(strend, -dontbother, strbeg) - 1;
	    /* for multiline we only have to try after newlines */
	    if (prog->check_substr || prog->check_utf8) {
		if (s == startpos)
		    goto after_try;
		while (1) {
		    if (regtry(prog, s))
			goto got_it;
		  after_try:
		    if (s >= end)
			goto phooey;
		    if (prog->reganch & RE_USE_INTUIT) {
			s = re_intuit_start(prog, sv, s + 1, strend, flags, NULL);
			if (!s)
			    goto phooey;
		    }
		    else
			s++;
		}		
	    } else {
		if (s > startpos)
		    s--;
		while (s < end) {
		    if (*s++ == '\n') {	/* don't need PL_utf8skip here */
			if (regtry(prog, s))
			    goto got_it;
		    }
		}		
	    }
	}
	goto phooey;
    } else if (prog->reganch & ROPT_ANCH_GPOS) {
	if (regtry(prog, PL_reg_ganch))
	    goto got_it;
	goto phooey;
    }

    /* Messy cases:  unanchored match. */
    if ((prog->anchored_substr || prog->anchored_utf8) && prog->reganch & ROPT_SKIP) {
	/* we have /x+whatever/ */
	/* it must be a one character string (XXXX Except UTF?) */
	char ch;
#ifdef DEBUGGING
	int did_match = 0;
#endif
	if (!(do_utf8 ? prog->anchored_utf8 : prog->anchored_substr))
	    do_utf8 ? to_utf8_substr(prog) : to_byte_substr(prog);
	ch = SvPVX_const(do_utf8 ? prog->anchored_utf8 : prog->anchored_substr)[0];

	if (do_utf8) {
	    while (s < strend) {
		if (*s == ch) {
		    DEBUG_EXECUTE_r( did_match = 1 );
		    if (regtry(prog, s)) goto got_it;
		    s += UTF8SKIP(s);
		    while (s < strend && *s == ch)
			s += UTF8SKIP(s);
		}
		s += UTF8SKIP(s);
	    }
	}
	else {
	    while (s < strend) {
		if (*s == ch) {
		    DEBUG_EXECUTE_r( did_match = 1 );
		    if (regtry(prog, s)) goto got_it;
		    s++;
		    while (s < strend && *s == ch)
			s++;
		}
		s++;
	    }
	}
	DEBUG_EXECUTE_r(if (!did_match)
		PerlIO_printf(Perl_debug_log,
                                  "Did not find anchored character...\n")
               );
    }
    else if (prog->anchored_substr != NULL
	      || prog->anchored_utf8 != NULL
	      || ((prog->float_substr != NULL || prog->float_utf8 != NULL)
		  && prog->float_max_offset < strend - s)) {
	SV *must;
	I32 back_max;
	I32 back_min;
	char *last;
	char *last1;		/* Last position checked before */
#ifdef DEBUGGING
	int did_match = 0;
#endif
	if (prog->anchored_substr || prog->anchored_utf8) {
	    if (!(do_utf8 ? prog->anchored_utf8 : prog->anchored_substr))
		do_utf8 ? to_utf8_substr(prog) : to_byte_substr(prog);
	    must = do_utf8 ? prog->anchored_utf8 : prog->anchored_substr;
	    back_max = back_min = prog->anchored_offset;
	} else {
	    if (!(do_utf8 ? prog->float_utf8 : prog->float_substr))
		do_utf8 ? to_utf8_substr(prog) : to_byte_substr(prog);
	    must = do_utf8 ? prog->float_utf8 : prog->float_substr;
	    back_max = prog->float_max_offset;
	    back_min = prog->float_min_offset;
	}
	if (must == &PL_sv_undef)
	    /* could not downgrade utf8 check substring, so must fail */
	    goto phooey;

	last = HOP3c(strend,	/* Cannot start after this */
			  -(I32)(CHR_SVLEN(must)
				 - (SvTAIL(must) != 0) + back_min), strbeg);

	if (s > PL_bostr)
	    last1 = HOPc(s, -1);
	else
	    last1 = s - 1;	/* bogus */

	/* XXXX check_substr already used to find "s", can optimize if
	   check_substr==must. */
	scream_pos = -1;
	dontbother = end_shift;
	strend = HOPc(strend, -dontbother);
	while ( (s <= last) &&
		((flags & REXEC_SCREAM)
		 ? (s = screaminstr(sv, must, HOP3c(s, back_min, strend) - strbeg,
				    end_shift, &scream_pos, 0))
		 : (s = fbm_instr((unsigned char*)HOP3(s, back_min, strend),
				  (unsigned char*)strend, must,
				  multiline ? FBMrf_MULTILINE : 0))) ) {
	    /* we may be pointing at the wrong string */
	    if ((flags & REXEC_SCREAM) && RX_MATCH_COPIED(prog))
		s = strbeg + (s - SvPVX_const(sv));
	    DEBUG_EXECUTE_r( did_match = 1 );
	    if (HOPc(s, -back_max) > last1) {
		last1 = HOPc(s, -back_min);
		s = HOPc(s, -back_max);
	    }
	    else {
		char *t = (last1 >= PL_bostr) ? HOPc(last1, 1) : last1 + 1;

		last1 = HOPc(s, -back_min);
		s = t;		
	    }
	    if (do_utf8) {
		while (s <= last1) {
		    if (regtry(prog, s))
			goto got_it;
		    s += UTF8SKIP(s);
		}
	    }
	    else {
		while (s <= last1) {
		    if (regtry(prog, s))
			goto got_it;
		    s++;
		}
	    }
	}
	DEBUG_EXECUTE_r(if (!did_match)
                    PerlIO_printf(Perl_debug_log, 
                                  "Did not find %s substr \"%s%.*s%s\"%s...\n",
			      ((must == prog->anchored_substr || must == prog->anchored_utf8)
			       ? "anchored" : "floating"),
			      PL_colors[0],
			      (int)(SvCUR(must) - (SvTAIL(must)!=0)),
			      SvPVX_const(must),
                                  PL_colors[1], (SvTAIL(must) ? "$" : ""))
               );
	goto phooey;
    }
    else if ((c = prog->regstclass)) {
	if (minlen) {
	    I32 op = (U8)OP(prog->regstclass);
	    /* don't bother with what can't match */
	    if (PL_regkind[op] != EXACT && op != CANY)
	        strend = HOPc(strend, -(minlen - 1));
	}
	DEBUG_EXECUTE_r({
	    SV *prop = sv_newmortal();
	    const char *s0;
	    const char *s1;
	    int len0;
	    int len1;

	    regprop(prop, c);
	    s0 = UTF ?
	      pv_uni_display(dsv0, (U8*)SvPVX_const(prop), SvCUR(prop), 60,
			     UNI_DISPLAY_REGEX) :
	      SvPVX_const(prop);
	    len0 = UTF ? SvCUR(dsv0) : SvCUR(prop);
	    s1 = UTF ?
	      sv_uni_display(dsv1, sv, 60, UNI_DISPLAY_REGEX) : s;
	    len1 = UTF ? SvCUR(dsv1) : strend - s;
	    PerlIO_printf(Perl_debug_log,
			  "Matching stclass \"%*.*s\" against \"%*.*s\"\n",
			  len0, len0, s0,
			  len1, len1, s1);
	});
        if (find_byclass(prog, c, s, strend, 0))
	    goto got_it;
	DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "Contradicts stclass...\n"));
    }
    else {
	dontbother = 0;
	if (prog->float_substr != NULL || prog->float_utf8 != NULL) {
	    /* Trim the end. */
	    char *last;
	    SV* float_real;

	    if (!(do_utf8 ? prog->float_utf8 : prog->float_substr))
		do_utf8 ? to_utf8_substr(prog) : to_byte_substr(prog);
	    float_real = do_utf8 ? prog->float_utf8 : prog->float_substr;

	    if (flags & REXEC_SCREAM) {
		last = screaminstr(sv, float_real, s - strbeg,
				   end_shift, &scream_pos, 1); /* last one */
		if (!last)
		    last = scream_olds; /* Only one occurrence. */
		/* we may be pointing at the wrong string */
		else if (RX_MATCH_COPIED(prog))
		    s = strbeg + (s - SvPVX_const(sv));
	    }
	    else {
		STRLEN len;
                const char * const little = SvPV_const(float_real, len);

		if (SvTAIL(float_real)) {
		    if (memEQ(strend - len + 1, little, len - 1))
			last = strend - len + 1;
		    else if (!multiline)
			last = memEQ(strend - len, little, len)
			    ? strend - len : NULL;
		    else
			goto find_last;
		} else {
		  find_last:
		    if (len)
			last = rninstr(s, strend, little, little + len);
		    else
			last = strend;	/* matching "$" */
		}
	    }
	    if (last == NULL) {
		DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
				      "%sCan't trim the tail, match fails (should not happen)%s\n",
				      PL_colors[4], PL_colors[5]));
		goto phooey; /* Should not happen! */
	    }
	    dontbother = strend - last + prog->float_min_offset;
	}
	if (minlen && (dontbother < minlen))
	    dontbother = minlen - 1;
	strend -= dontbother; 		   /* this one's always in bytes! */
	/* We don't know much -- general case. */
	if (do_utf8) {
	    for (;;) {
		if (regtry(prog, s))
		    goto got_it;
		if (s >= strend)
		    break;
		s += UTF8SKIP(s);
	    };
	}
	else {
	    do {
		if (regtry(prog, s))
		    goto got_it;
	    } while (s++ < strend);
	}
    }

    /* Failure. */
    goto phooey;

got_it:
    RX_MATCH_TAINTED_set(prog, PL_reg_flags & RF_tainted);

    if (PL_reg_eval_set) {
	/* Preserve the current value of $^R */
	if (oreplsv != GvSV(PL_replgv))
	    sv_setsv(oreplsv, GvSV(PL_replgv));/* So that when GvSV(replgv) is
						  restored, the value remains
						  the same. */
	restore_pos(aTHX_ 0);
    }

    /* make sure $`, $&, $', and $digit will work later */
    if ( !(flags & REXEC_NOT_FIRST) ) {
	RX_MATCH_COPY_FREE(prog);
	if (flags & REXEC_COPY_STR) {
	    I32 i = PL_regeol - startpos + (stringarg - strbeg);
#ifdef PERL_OLD_COPY_ON_WRITE
	    if ((SvIsCOW(sv)
		 || (SvFLAGS(sv) & CAN_COW_MASK) == CAN_COW_FLAGS)) {
		if (DEBUG_C_TEST) {
		    PerlIO_printf(Perl_debug_log,
				  "Copy on write: regexp capture, type %d\n",
				  (int) SvTYPE(sv));
		}
		prog->saved_copy = sv_setsv_cow(prog->saved_copy, sv);
		prog->subbeg = (char *)SvPVX_const(prog->saved_copy);
		assert (SvPOKp(prog->saved_copy));
	    } else
#endif
	    {
		RX_MATCH_COPIED_on(prog);
		s = savepvn(strbeg, i);
		prog->subbeg = s;
	    }
	    prog->sublen = i;
	}
	else {
	    prog->subbeg = strbeg;
	    prog->sublen = PL_regeol - strbeg;	/* strend may have been modified */
	}
    }

    return 1;

phooey:
    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "%sMatch failed%s\n",
			  PL_colors[4], PL_colors[5]));
    if (PL_reg_eval_set)
	restore_pos(aTHX_ 0);
    return 0;
}

/*
 - regtry - try match at specific point
 */
STATIC I32			/* 0 failure, 1 success */
S_regtry(pTHX_ regexp *prog, char *startpos)
{
    dVAR;
    register I32 i;
    register I32 *sp;
    register I32 *ep;
    CHECKPOINT lastcp;
    GET_RE_DEBUG_FLAGS_DECL;

#ifdef DEBUGGING
    PL_regindent = 0;	/* XXXX Not good when matches are reenterable... */
#endif
    if ((prog->reganch & ROPT_EVAL_SEEN) && !PL_reg_eval_set) {
	MAGIC *mg;

	PL_reg_eval_set = RS_init;
	DEBUG_EXECUTE_r(DEBUG_s(
	    PerlIO_printf(Perl_debug_log, "  setting stack tmpbase at %"IVdf"\n",
			  (IV)(PL_stack_sp - PL_stack_base));
	    ));
	SAVEI32(cxstack[cxstack_ix].blk_oldsp);
	cxstack[cxstack_ix].blk_oldsp = PL_stack_sp - PL_stack_base;
	/* Otherwise OP_NEXTSTATE will free whatever on stack now.  */
	SAVETMPS;
	/* Apparently this is not needed, judging by wantarray. */
	/* SAVEI8(cxstack[cxstack_ix].blk_gimme);
	   cxstack[cxstack_ix].blk_gimme = G_SCALAR; */

	if (PL_reg_sv) {
	    /* Make $_ available to executed code. */
	    if (PL_reg_sv != DEFSV) {
		SAVE_DEFSV;
		DEFSV = PL_reg_sv;
	    }
	
	    if (!(SvTYPE(PL_reg_sv) >= SVt_PVMG && SvMAGIC(PL_reg_sv)
		  && (mg = mg_find(PL_reg_sv, PERL_MAGIC_regex_global)))) {
		/* prepare for quick setting of pos */
#ifdef PERL_OLD_COPY_ON_WRITE
		if (SvIsCOW(sv))
		    sv_force_normal_flags(sv, 0);
#endif
		mg = sv_magicext(PL_reg_sv, (SV*)0, PERL_MAGIC_regex_global,
				 &PL_vtbl_mglob, NULL, 0);
		mg->mg_len = -1;
	    }
	    PL_reg_magic    = mg;
	    PL_reg_oldpos   = mg->mg_len;
	    SAVEDESTRUCTOR_X(restore_pos, 0);
        }
        if (!PL_reg_curpm) {
	    Newxz(PL_reg_curpm, 1, PMOP);
#ifdef USE_ITHREADS
            {
                SV* repointer = newSViv(0);
                /* so we know which PL_regex_padav element is PL_reg_curpm */
                SvFLAGS(repointer) |= SVf_BREAK;
                av_push(PL_regex_padav,repointer);
                PL_reg_curpm->op_pmoffset = av_len(PL_regex_padav);
                PL_regex_pad = AvARRAY(PL_regex_padav);
            }
#endif      
        }
	PM_SETRE(PL_reg_curpm, prog);
	PL_reg_oldcurpm = PL_curpm;
	PL_curpm = PL_reg_curpm;
	if (RX_MATCH_COPIED(prog)) {
	    /*  Here is a serious problem: we cannot rewrite subbeg,
		since it may be needed if this match fails.  Thus
		$` inside (?{}) could fail... */
	    PL_reg_oldsaved = prog->subbeg;
	    PL_reg_oldsavedlen = prog->sublen;
#ifdef PERL_OLD_COPY_ON_WRITE
	    PL_nrs = prog->saved_copy;
#endif
	    RX_MATCH_COPIED_off(prog);
	}
	else
	    PL_reg_oldsaved = NULL;
	prog->subbeg = PL_bostr;
	prog->sublen = PL_regeol - PL_bostr; /* strend may have been modified */
    }
    prog->startp[0] = startpos - PL_bostr;
    PL_reginput = startpos;
    PL_regstartp = prog->startp;
    PL_regendp = prog->endp;
    PL_reglastparen = &prog->lastparen;
    PL_reglastcloseparen = &prog->lastcloseparen;
    prog->lastparen = 0;
    prog->lastcloseparen = 0;
    PL_regsize = 0;
    DEBUG_EXECUTE_r(PL_reg_starttry = startpos);
    if (PL_reg_start_tmpl <= prog->nparens) {
	PL_reg_start_tmpl = prog->nparens*3/2 + 3;
        if(PL_reg_start_tmp)
            Renew(PL_reg_start_tmp, PL_reg_start_tmpl, char*);
        else
            Newx(PL_reg_start_tmp, PL_reg_start_tmpl, char*);
    }

    /* XXXX What this code is doing here?!!!  There should be no need
       to do this again and again, PL_reglastparen should take care of
       this!  --ilya*/

    /* Tests pat.t#187 and split.t#{13,14} seem to depend on this code.
     * Actually, the code in regcppop() (which Ilya may be meaning by
     * PL_reglastparen), is not needed at all by the test suite
     * (op/regexp, op/pat, op/split), but that code is needed, oddly
     * enough, for building DynaLoader, or otherwise this
     * "Error: '*' not in typemap in DynaLoader.xs, line 164"
     * will happen.  Meanwhile, this code *is* needed for the
     * above-mentioned test suite tests to succeed.  The common theme
     * on those tests seems to be returning null fields from matches.
     * --jhi */
#if 1
    sp = prog->startp;
    ep = prog->endp;
    if (prog->nparens) {
	for (i = prog->nparens; i > (I32)*PL_reglastparen; i--) {
	    *++sp = -1;
	    *++ep = -1;
	}
    }
#endif
    REGCP_SET(lastcp);
    if (regmatch(prog->program + 1)) {
	prog->endp[0] = PL_reginput - PL_bostr;
	return 1;
    }
    REGCP_UNWIND(lastcp);
    return 0;
}

#define RE_UNWIND_BRANCH	1
#define RE_UNWIND_BRANCHJ	2

union re_unwind_t;

typedef struct {		/* XX: makes sense to enlarge it... */
    I32 type;
    I32 prev;
    CHECKPOINT lastcp;
} re_unwind_generic_t;

typedef struct {
    I32 type;
    I32 prev;
    CHECKPOINT lastcp;
    I32 lastparen;
    regnode *next;
    char *locinput;
    I32 nextchr;
#ifdef DEBUGGING
    int regindent;
#endif
} re_unwind_branch_t;

typedef union re_unwind_t {
    I32 type;
    re_unwind_generic_t generic;
    re_unwind_branch_t branch;
} re_unwind_t;

#define sayYES goto yes
#define sayNO goto no
#define sayNO_ANYOF goto no_anyof
#define sayYES_FINAL goto yes_final
#define sayYES_LOUD  goto yes_loud
#define sayNO_FINAL  goto no_final
#define sayNO_SILENT goto do_no
#define saySAME(x) if (x) goto yes; else goto no

#define POSCACHE_SUCCESS 0	/* caching success rather than failure */
#define POSCACHE_SEEN 1		/* we know what we're caching */
#define POSCACHE_START 2	/* the real cache: this bit maps to pos 0 */
#define CACHEsayYES STMT_START { \
    if (cache_offset | cache_bit) { \
	if (!(PL_reg_poscache[0] & (1<<POSCACHE_SEEN))) \
	    PL_reg_poscache[0] |= (1<<POSCACHE_SUCCESS) || (1<<POSCACHE_SEEN); \
        else if (!(PL_reg_poscache[0] & (1<<POSCACHE_SUCCESS))) { \
	    /* cache records failure, but this is success */ \
	    DEBUG_r( \
		PerlIO_printf(Perl_debug_log, \
		    "%*s  (remove success from failure cache)\n", \
		    REPORT_CODE_OFF+PL_regindent*2, "") \
	    ); \
	    PL_reg_poscache[cache_offset] &= ~(1<<cache_bit); \
	} \
    } \
    sayYES; \
} STMT_END
#define CACHEsayNO STMT_START { \
    if (cache_offset | cache_bit) { \
	if (!(PL_reg_poscache[0] & (1<<POSCACHE_SEEN))) \
	    PL_reg_poscache[0] |= (1<<POSCACHE_SEEN); \
        else if ((PL_reg_poscache[0] & (1<<POSCACHE_SUCCESS))) { \
	    /* cache records success, but this is failure */ \
	    DEBUG_r( \
		PerlIO_printf(Perl_debug_log, \
		    "%*s  (remove failure from success cache)\n", \
		    REPORT_CODE_OFF+PL_regindent*2, "") \
	    ); \
	    PL_reg_poscache[cache_offset] &= ~(1<<cache_bit); \
	} \
    } \
    sayNO; \
} STMT_END

/* this is used to determine how far from the left messages like
   'failed...' are printed. Currently 29 makes these messages line
   up with the opcode they refer to. Earlier perls used 25 which
   left these messages outdented making reviewing a debug output
   quite difficult.
*/
#define REPORT_CODE_OFF 29


/* Make sure there is a test for this +1 options in re_tests */
#define TRIE_INITAL_ACCEPT_BUFFLEN 4;


/* simulate a recursive call to regmatch */

#define REGMATCH(ns, where) \
    new_scan = (ns); \
    resume_state = resume_##where; \
    goto start_recurse; \
    resume_point_##where:

typedef enum {
    resume_TRIE1,
    resume_TRIE2,
    resume_CURLYX,
    resume_WHILEM1,
    resume_WHILEM2,
    resume_WHILEM3,
    resume_WHILEM4,
    resume_WHILEM5,
    resume_WHILEM6,
    resume_CURLYM1,
    resume_CURLYM2,
    resume_CURLYM3,
    resume_CURLYM4,
    resume_IFMATCH,
    resume_PLUS1,
    resume_PLUS2,
    resume_PLUS3,
    resume_PLUS4,
    resume_END
} resume_states;


struct regmatch_state {
    struct regmatch_state *prev_state;
    resume_states resume_state;
    regnode *scan;
    regnode *next;
    bool minmod;
    bool sw;
    int logical;
    I32 unwind;
    CURCUR *cc;
    char *locinput;

    I32 n;
    I32 ln;
    I32 c1, c2, paren;
    CHECKPOINT cp, lastcp;
    CURCUR *oldcc;
    char *lastloc;
    I32 cache_offset, cache_bit;
    I32 curlym_l;
    I32 matches;
    I32 maxwanted;
    char *e; 
    char *old;
    int count;
    re_cc_state *cur_call_cc;
    regexp *end_re;
    reg_trie_accepted *accept_buff;
    U32 accepted;

    re_cc_state *reg_call_cc;	/* saved value of PL_reg_call_cc */
};

/*
 - regmatch - main matching routine
 *
 * Conceptually the strategy is simple:  check to see whether the current
 * node matches, call self recursively to see whether the rest matches,
 * and then act accordingly.  In practice we make some effort to avoid
 * recursion, in particular by going through "ordinary" nodes (that don't
 * need to know whether the rest of the match failed) by a loop instead of
 * by recursion.
 */
/* [lwall] I've hoisted the register declarations to the outer block in order to
 * maybe save a little bit of pushing and popping on the stack.  It also takes
 * advantage of machines that use a register save mask on subroutine entry.
 *
 * This function used to be heavily recursive, but since this had the
 * effect of blowing the CPU stack on complex regexes, it has been
 * restructured to be iterative, and to save state onto the heap rather
 * than the stack. Essentially whereever regmatch() used to be called, it
 * pushes the current state, notes where to return, then jumps back into
 * the main loop.
 *
 * Originally the structure of this function used to look something like

    S_regmatch() {
	int a = 1, b = 2;
	...
	while (scan != NULL) {
	    ...
    	    switch (OP(scan)) {
		case FOO: {
		    int local = 3;
		    ...
		    if (regmatch(...))  // recurse
			goto yes;
		}
		...
	    }
	}
	yes:
	return 1;
    }

 * Now it looks something like this:

    struct regmatch_state {
	int a, b, local;
	int resume_state;
    };

    S_regmatch() {
	int a = 1, b = 2;
	int local;
	...
	while (scan != NULL) {
	    ...
    	    switch (OP(scan)) {
		case FOO: {
		    local = 3;
		    ...
		    resume_state = resume_FOO;
		    new_scan = ...;
		    goto start_recurse;
		    resume_point_FOO:

		    if (result)  // recurse
			goto yes;
		}
		...
	    }
	    start_recurse:
	    ...push a, b, local onto the heap
	    a = 1; b = 2;
	    scan = new_scan;
	}
	yes:
	result = 1;
	if (states pushed on heap) {
	    ... restore a, b, local from heap
	    switch (resume_state) {
	    case resume_FOO:
		goto resume_point_FOO;
	    ...
	    }
	}
	return result
    }
	    
 * WARNING: this means that any line in this function that contains a
 * REGMATCH() or TRYPAREN() is actually simulating a recursive call to
 * regmatch() using gotos instead. Thus the values of any local variables
 * not saved in the regmatch_state structure will have been lost when
 * execution resumes on the next line .
 */
 

STATIC I32			/* 0 failure, 1 success */
S_regmatch(pTHX_ regnode *prog)
{
    dVAR;
    register const bool do_utf8 = PL_reg_match_utf8;
    const U32 uniflags = ckWARN(WARN_UTF8) ? 0 : UTF8_ALLOW_ANY;

    /************************************************************
     * the following variabes are saved and restored on each fake
     * recursive call to regmatch:
     *
     * The first ones contain state that needs to be maintained
     * across the main while loop: */

    struct regmatch_state *prev_state = NULL; /* stack of pushed states */
    resume_states resume_state; /* where to jump to on return */
    register regnode *scan;	/* Current node. */
    regnode *next;		/* Next node. */
    bool minmod = 0;		/* the next {n.m} is a {n,m}? */
    bool sw = 0;		/* the condition value in (?(cond)a|b) */
    int logical = 0;
    I32 unwind = 0;		/* savestack index of current unwind block */
    CURCUR *cc = NULL;		/* current innermost curly struct */
    register char *locinput = PL_reginput;

    /* while the rest of these are local to an individual branch, and
     * have only been hoisted into this outer scope to allow for saving and
     * restoration - thus they can be safely reused in other branches.
     * Note that they are only initialized here to silence compiler
     * warnings :-( */

    register I32 n = 0;		/* no or next */
    register I32 ln = 0;	/* len or last */
    register I32 c1 = 0, c2 = 0, paren = 0; /* case fold search, parenth */
    CHECKPOINT cp = 0;		/* remember current savestack indexes */
    CHECKPOINT lastcp = 0;
    CURCUR *oldcc = NULL;	/* tmp copy of cc */
    char *lastloc = NULL;	/* Detection of 0-len. */
    I32 cache_offset = 0;
    I32 cache_bit = 0;
    I32 curlym_l = 0;
    I32 matches = 0;
    I32 maxwanted = 0;
    char *e = NULL;
    char *old = NULL;
    int count = 0;
    re_cc_state *cur_call_cc = NULL;
    regexp *end_re = NULL;
    reg_trie_accepted *accept_buff = NULL;
    U32 accepted = 0; /* how many accepting states we have seen */

    /************************************************************
     * these variables are NOT saved: */

    register I32 nextchr;   /* is always set to UCHARAT(locinput) */
    regnode *new_scan;	/* node to begin exedcution at when recursing */
    bool result;	/* return value of S_regmatch */

    regnode *inner;	/* Next node in internal branch. */
    
#ifdef DEBUGGING
    SV *re_debug_flags = NULL;
    GET_RE_DEBUG_FLAGS;
    PL_regindent++;
#endif

    /* Note that nextchr is a byte even in UTF */
    nextchr = UCHARAT(locinput);
    scan = prog;
    while (scan != NULL) {

        DEBUG_EXECUTE_r( {
	    SV * const prop = sv_newmortal();
	    const int docolor = *PL_colors[0];
	    const int taill = (docolor ? 10 : 7); /* 3 chars for "> <" */
	    int l = (PL_regeol - locinput) > taill ? taill : (PL_regeol - locinput);
	    /* The part of the string before starttry has one color
	       (pref0_len chars), between starttry and current
	       position another one (pref_len - pref0_len chars),
	       after the current position the third one.
	       We assume that pref0_len <= pref_len, otherwise we
	       decrease pref0_len.  */
	    int pref_len = (locinput - PL_bostr) > (5 + taill) - l
		? (5 + taill) - l : locinput - PL_bostr;
	    int pref0_len;

	    while (do_utf8 && UTF8_IS_CONTINUATION(*(U8*)(locinput - pref_len)))
		pref_len++;
	    pref0_len = pref_len  - (locinput - PL_reg_starttry);
	    if (l + pref_len < (5 + taill) && l < PL_regeol - locinput)
		l = ( PL_regeol - locinput > (5 + taill) - pref_len
		      ? (5 + taill) - pref_len : PL_regeol - locinput);
	    while (do_utf8 && UTF8_IS_CONTINUATION(*(U8*)(locinput + l)))
		l--;
	    if (pref0_len < 0)
		pref0_len = 0;
	    if (pref0_len > pref_len)
		pref0_len = pref_len;
	    regprop(prop, scan);
	    {
	      const char * const s0 =
		do_utf8 && OP(scan) != CANY ?
		pv_uni_display(PERL_DEBUG_PAD(0), (U8*)(locinput - pref_len),
			       pref0_len, 60, UNI_DISPLAY_REGEX) :
		locinput - pref_len;
	      const int len0 = do_utf8 ? strlen(s0) : pref0_len;
	      const char * const s1 = do_utf8 && OP(scan) != CANY ?
		pv_uni_display(PERL_DEBUG_PAD(1),
			       (U8*)(locinput - pref_len + pref0_len),
			       pref_len - pref0_len, 60, UNI_DISPLAY_REGEX) :
		locinput - pref_len + pref0_len;
	      const int len1 = do_utf8 ? strlen(s1) : pref_len - pref0_len;
	      const char * const s2 = do_utf8 && OP(scan) != CANY ?
		pv_uni_display(PERL_DEBUG_PAD(2), (U8*)locinput,
			       PL_regeol - locinput, 60, UNI_DISPLAY_REGEX) :
		locinput;
	      const int len2 = do_utf8 ? strlen(s2) : l;
	      PerlIO_printf(Perl_debug_log,
			    "%4"IVdf" <%s%.*s%s%s%.*s%s%s%s%.*s%s>%*s|%3"IVdf":%*s%s\n",
			    (IV)(locinput - PL_bostr),
			    PL_colors[4],
			    len0, s0,
			    PL_colors[5],
			    PL_colors[2],
			    len1, s1,
			    PL_colors[3],
			    (docolor ? "" : "> <"),
			    PL_colors[0],
			    len2, s2,
			    PL_colors[1],
			    15 - l - pref_len + 1,
			    "",
			    (IV)(scan - PL_regprogram), PL_regindent*2, "",
			    SvPVX_const(prop));
	    }
	});

	next = scan + NEXT_OFF(scan);
	if (next == scan)
	    next = NULL;

	switch (OP(scan)) {
	case BOL:
	    if (locinput == PL_bostr)
	    {
		/* regtill = regbol; */
		break;
	    }
	    sayNO;
	case MBOL:
	    if (locinput == PL_bostr ||
		((nextchr || locinput < PL_regeol) && locinput[-1] == '\n'))
	    {
		break;
	    }
	    sayNO;
	case SBOL:
	    if (locinput == PL_bostr)
		break;
	    sayNO;
	case GPOS:
	    if (locinput == PL_reg_ganch)
		break;
	    sayNO;
	case EOL:
		goto seol;
	case MEOL:
	    if ((nextchr || locinput < PL_regeol) && nextchr != '\n')
		sayNO;
	    break;
	case SEOL:
	  seol:
	    if ((nextchr || locinput < PL_regeol) && nextchr != '\n')
		sayNO;
	    if (PL_regeol - locinput > 1)
		sayNO;
	    break;
	case EOS:
	    if (PL_regeol != locinput)
		sayNO;
	    break;
	case SANY:
	    if (!nextchr && locinput >= PL_regeol)
		sayNO;
 	    if (do_utf8) {
	        locinput += PL_utf8skip[nextchr];
		if (locinput > PL_regeol)
 		    sayNO;
 		nextchr = UCHARAT(locinput);
 	    }
 	    else
 		nextchr = UCHARAT(++locinput);
	    break;
	case CANY:
	    if (!nextchr && locinput >= PL_regeol)
		sayNO;
	    nextchr = UCHARAT(++locinput);
	    break;
	case REG_ANY:
	    if ((!nextchr && locinput >= PL_regeol) || nextchr == '\n')
		sayNO;
	    if (do_utf8) {
		locinput += PL_utf8skip[nextchr];
		if (locinput > PL_regeol)
		    sayNO;
		nextchr = UCHARAT(locinput);
	    }
	    else
		nextchr = UCHARAT(++locinput);
	    break;



	/*
	   traverse the TRIE keeping track of all accepting states
	   we transition through until we get to a failing node.


	*/
	case TRIE:
	case TRIEF:
	case TRIEFL:
	    {
		U8 *uc = ( U8* )locinput;
		U32 state = 1;
		U16 charid = 0;
		U32 base = 0;
		UV uvc = 0;
		STRLEN len = 0;
		STRLEN foldlen = 0;
		U8 *uscan = (U8*)NULL;
		STRLEN bufflen=0;
		SV *sv_accept_buff = NULL;
		const enum { trie_plain, trie_utf8, trie_uft8_fold }
		    trie_type = do_utf8 ?
			  (OP(scan) == TRIE ? trie_utf8 : trie_uft8_fold)
			: trie_plain;

		reg_trie_data *trie; /* what trie are we using right now */

	    	accepted = 0; /* how many accepting states we have seen */
		result = 0;
		trie = (reg_trie_data*)PL_regdata->data[ ARG( scan ) ];

		while ( state && uc <= (U8*)PL_regeol ) {

		    if (trie->states[ state ].wordnum) {
			if (!accepted ) {
			    ENTER;
			    SAVETMPS;
			    bufflen = TRIE_INITAL_ACCEPT_BUFFLEN;
			    sv_accept_buff=newSV(bufflen *
					    sizeof(reg_trie_accepted) - 1);
			    SvCUR_set(sv_accept_buff,
						sizeof(reg_trie_accepted));
			    SvPOK_on(sv_accept_buff);
			    sv_2mortal(sv_accept_buff);
			    accept_buff =
				(reg_trie_accepted*)SvPV_nolen(sv_accept_buff );
			}
			else {
			    if (accepted >= bufflen) {
				bufflen *= 2;
				accept_buff =(reg_trie_accepted*)
				    SvGROW(sv_accept_buff,
				       	bufflen * sizeof(reg_trie_accepted));
			    }
			    SvCUR_set(sv_accept_buff,SvCUR(sv_accept_buff)
				+ sizeof(reg_trie_accepted));
			}
			accept_buff[accepted].wordnum = trie->states[state].wordnum;
			accept_buff[accepted].endpos = uc;
			++accepted;
		    }

		    base = trie->states[ state ].trans.base;

		    DEBUG_TRIE_EXECUTE_r(
			        PerlIO_printf( Perl_debug_log,
			            "%*s  %sState: %4"UVxf", Base: %4"UVxf", Accepted: %4"UVxf" ",
			            REPORT_CODE_OFF + PL_regindent * 2, "", PL_colors[4],
			            (UV)state, (UV)base, (UV)accepted );
		    );

		    if ( base ) {
			switch (trie_type) {
			case trie_uft8_fold:
			    if ( foldlen>0 ) {
				uvc = utf8n_to_uvuni( uscan, UTF8_MAXLEN, &len, uniflags );
				foldlen -= len;
				uscan += len;
				len=0;
			    } else {
				U8 foldbuf[ UTF8_MAXBYTES_CASE + 1 ];
				uvc = utf8n_to_uvuni( (U8*)uc, UTF8_MAXLEN, &len, uniflags );
				uvc = to_uni_fold( uvc, foldbuf, &foldlen );
				foldlen -= UNISKIP( uvc );
				uscan = foldbuf + UNISKIP( uvc );
			    }
			    break;
			case trie_utf8:
			    uvc = utf8n_to_uvuni( (U8*)uc, UTF8_MAXLEN,
							    &len, uniflags );
			    break;
			case trie_plain:
			    uvc = (UV)*uc;
			    len = 1;
			}

			if (uvc < 256) {
			    charid = trie->charmap[ uvc ];
			}
			else {
			    charid = 0;
			    if (trie->widecharmap) {
				SV** svpp = (SV**)NULL;
				svpp = hv_fetch(trie->widecharmap,
					    (char*)&uvc, sizeof(UV), 0);
				if (svpp)
				    charid = (U16)SvIV(*svpp);
			    }
			}

			if (charid &&
			     (base + charid > trie->uniquecharcount )
			     && (base + charid - 1 - trie->uniquecharcount
				    < trie->lasttrans)
			     && trie->trans[base + charid - 1 -
				    trie->uniquecharcount].check == state)
			{
			    state = trie->trans[base + charid - 1 -
				trie->uniquecharcount ].next;
			}
			else {
			    state = 0;
			}
			uc += len;

		    }
		    else {
			state = 0;
		    }
		    DEBUG_TRIE_EXECUTE_r(
		        PerlIO_printf( Perl_debug_log,
		            "Charid:%3x CV:%4"UVxf" After State: %4"UVxf"%s\n",
		            charid, uvc, (UV)state, PL_colors[5] );
		    );
		}
		if (!accepted )
		   sayNO;

	    /*
	       There was at least one accepting state that we
	       transitioned through. Presumably the number of accepting
	       states is going to be low, typically one or two. So we
	       simply scan through to find the one with lowest wordnum.
	       Once we find it, we swap the last state into its place
	       and decrement the size. We then try to match the rest of
	       the pattern at the point where the word ends, if we
	       succeed then we end the loop, otherwise the loop
	       eventually terminates once all of the accepting states
	       have been tried.
	    */

		if ( accepted == 1 ) {
		    DEBUG_EXECUTE_r({
                        SV **tmp = av_fetch( trie->words, accept_buff[ 0 ].wordnum-1, 0 );
       	                PerlIO_printf( Perl_debug_log,
			    "%*s  %sonly one match : #%d <%s>%s\n",
			    REPORT_CODE_OFF+PL_regindent*2, "", PL_colors[4],
        		    accept_buff[ 0 ].wordnum,
        		    tmp ? SvPV_nolen_const( *tmp ) : "not compiled under -Dr",
        		    PL_colors[5] );
		    });
		    PL_reginput = (char *)accept_buff[ 0 ].endpos;
		    /* in this case we free tmps/leave before we call regmatch
		       as we wont be using accept_buff again. */
		    FREETMPS;
		    LEAVE;
		    REGMATCH(scan + NEXT_OFF(scan), TRIE1);
		    /*** all unsaved local vars undefined at this point */
		} else {
                    DEBUG_EXECUTE_r(
                        PerlIO_printf( Perl_debug_log,"%*s  %sgot %"IVdf" possible matches%s\n",
                            REPORT_CODE_OFF + PL_regindent * 2, "", PL_colors[4], (IV)accepted,
                            PL_colors[5] );
                    );
		    while ( !result && accepted-- ) {
			U32 best = 0;
			U32 cur;
			for( cur = 1 ; cur <= accepted ; cur++ ) {
			    DEBUG_TRIE_EXECUTE_r(
			        PerlIO_printf( Perl_debug_log,
			            "%*s  %sgot %"IVdf" (%d) as best, looking at %"IVdf" (%d)%s\n",
			            REPORT_CODE_OFF + PL_regindent * 2, "", PL_colors[4],
			            (IV)best, accept_buff[ best ].wordnum, (IV)cur,
			            accept_buff[ cur ].wordnum, PL_colors[5] );
			    );

			    if ( accept_buff[ cur ].wordnum < accept_buff[ best ].wordnum )
				    best = cur;
			}
			DEBUG_EXECUTE_r({
			    SV ** const tmp = av_fetch( trie->words, accept_buff[ best ].wordnum - 1, 0 );
    			    PerlIO_printf( Perl_debug_log, "%*s  %strying alternation #%d <%s> at 0x%p%s\n",
    			        REPORT_CODE_OFF+PL_regindent*2, "", PL_colors[4],
    			        accept_buff[best].wordnum,
        		        tmp ? SvPV_nolen_const( *tmp ) : "not compiled under -Dr",scan,
        		        PL_colors[5] );
			});
			if ( best<accepted ) {
			    reg_trie_accepted tmp = accept_buff[ best ];
			    accept_buff[ best ] = accept_buff[ accepted ];
			    accept_buff[ accepted ] = tmp;
			    best = accepted;
			}
			PL_reginput = (char *)accept_buff[ best ].endpos;

                        /* 
                           as far as I can tell we only need the SAVETMPS/FREETMPS 
                           for re's with EVAL in them but I'm leaving them in for 
                           all until I can be sure.
                         */
			SAVETMPS;
			REGMATCH(scan + NEXT_OFF(scan), TRIE2);
			/*** all unsaved local vars undefined at this point */
			FREETMPS;
		    }
		    FREETMPS;
		    LEAVE;
		}
		
		if (result) {
		    sayYES;
		} else {
		    sayNO;
		}
	    }
	    /* unreached codepoint */
	case EXACT: {
	    char *s = STRING(scan);
	    ln = STR_LEN(scan);
	    if (do_utf8 != UTF) {
		/* The target and the pattern have differing utf8ness. */
		char *l = locinput;
		const char *e = s + ln;

		if (do_utf8) {
		    /* The target is utf8, the pattern is not utf8. */
		    while (s < e) {
			STRLEN ulen;
			if (l >= PL_regeol)
			     sayNO;
			if (NATIVE_TO_UNI(*(U8*)s) !=
			    utf8n_to_uvuni((U8*)l, UTF8_MAXBYTES, &ulen,
					    uniflags))
			     sayNO;
			l += ulen;
			s ++;
		    }
		}
		else {
		    /* The target is not utf8, the pattern is utf8. */
		    while (s < e) {
			STRLEN ulen;
			if (l >= PL_regeol)
			    sayNO;
			if (NATIVE_TO_UNI(*((U8*)l)) !=
			    utf8n_to_uvuni((U8*)s, UTF8_MAXBYTES, &ulen,
					   uniflags))
			    sayNO;
			s += ulen;
			l ++;
		    }
		}
		locinput = l;
		nextchr = UCHARAT(locinput);
		break;
	    }
	    /* The target and the pattern have the same utf8ness. */
	    /* Inline the first character, for speed. */
	    if (UCHARAT(s) != nextchr)
		sayNO;
	    if (PL_regeol - locinput < ln)
		sayNO;
	    if (ln > 1 && memNE(s, locinput, ln))
		sayNO;
	    locinput += ln;
	    nextchr = UCHARAT(locinput);
	    break;
	    }
	case EXACTFL:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case EXACTF: {
	    char *s = STRING(scan);
	    ln = STR_LEN(scan);

	    if (do_utf8 || UTF) {
	      /* Either target or the pattern are utf8. */
		char *l = locinput;
		char *e = PL_regeol;

		if (ibcmp_utf8(s, 0,  ln, (bool)UTF,
			       l, &e, 0,  do_utf8)) {
		     /* One more case for the sharp s:
		      * pack("U0U*", 0xDF) =~ /ss/i,
		      * the 0xC3 0x9F are the UTF-8
		      * byte sequence for the U+00DF. */
		     if (!(do_utf8 &&
			   toLOWER(s[0]) == 's' &&
			   ln >= 2 &&
			   toLOWER(s[1]) == 's' &&
			   (U8)l[0] == 0xC3 &&
			   e - l >= 2 &&
			   (U8)l[1] == 0x9F))
			  sayNO;
		}
		locinput = e;
		nextchr = UCHARAT(locinput);
		break;
	    }

	    /* Neither the target and the pattern are utf8. */

	    /* Inline the first character, for speed. */
	    if (UCHARAT(s) != nextchr &&
		UCHARAT(s) != ((OP(scan) == EXACTF)
			       ? PL_fold : PL_fold_locale)[nextchr])
		sayNO;
	    if (PL_regeol - locinput < ln)
		sayNO;
	    if (ln > 1 && (OP(scan) == EXACTF
			   ? ibcmp(s, locinput, ln)
			   : ibcmp_locale(s, locinput, ln)))
		sayNO;
	    locinput += ln;
	    nextchr = UCHARAT(locinput);
	    break;
	    }
	case ANYOF:
	    if (do_utf8) {
	        STRLEN inclasslen = PL_regeol - locinput;

	        if (!reginclass(scan, (U8*)locinput, &inclasslen, do_utf8))
		    sayNO_ANYOF;
		if (locinput >= PL_regeol)
		    sayNO;
		locinput += inclasslen ? inclasslen : UTF8SKIP(locinput);
		nextchr = UCHARAT(locinput);
		break;
	    }
	    else {
		if (nextchr < 0)
		    nextchr = UCHARAT(locinput);
		if (!REGINCLASS(scan, (U8*)locinput))
		    sayNO_ANYOF;
		if (!nextchr && locinput >= PL_regeol)
		    sayNO;
		nextchr = UCHARAT(++locinput);
		break;
	    }
	no_anyof:
	    /* If we might have the case of the German sharp s
	     * in a casefolding Unicode character class. */

	    if (ANYOF_FOLD_SHARP_S(scan, locinput, PL_regeol)) {
		 locinput += SHARP_S_SKIP;
		 nextchr = UCHARAT(locinput);
	    }
	    else
		 sayNO;
	    break;
	case ALNUML:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case ALNUM:
	    if (!nextchr)
		sayNO;
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_ALNUM();
		if (!(OP(scan) == ALNUM
		      ? swash_fetch(PL_utf8_alnum, (U8*)locinput, do_utf8)
		      : isALNUM_LC_utf8((U8*)locinput)))
		{
		    sayNO;
		}
		locinput += PL_utf8skip[nextchr];
		nextchr = UCHARAT(locinput);
		break;
	    }
	    if (!(OP(scan) == ALNUM
		  ? isALNUM(nextchr) : isALNUM_LC(nextchr)))
		sayNO;
	    nextchr = UCHARAT(++locinput);
	    break;
	case NALNUML:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case NALNUM:
	    if (!nextchr && locinput >= PL_regeol)
		sayNO;
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_ALNUM();
		if (OP(scan) == NALNUM
		    ? swash_fetch(PL_utf8_alnum, (U8*)locinput, do_utf8)
		    : isALNUM_LC_utf8((U8*)locinput))
		{
		    sayNO;
		}
		locinput += PL_utf8skip[nextchr];
		nextchr = UCHARAT(locinput);
		break;
	    }
	    if (OP(scan) == NALNUM
		? isALNUM(nextchr) : isALNUM_LC(nextchr))
		sayNO;
	    nextchr = UCHARAT(++locinput);
	    break;
	case BOUNDL:
	case NBOUNDL:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case BOUND:
	case NBOUND:
	    /* was last char in word? */
	    if (do_utf8) {
		if (locinput == PL_bostr)
		    ln = '\n';
		else {
		    const U8 * const r = reghop3((U8*)locinput, -1, (U8*)PL_bostr);
		
		    ln = utf8n_to_uvchr(r, UTF8SKIP(r), 0, 0);
		}
		if (OP(scan) == BOUND || OP(scan) == NBOUND) {
		    ln = isALNUM_uni(ln);
		    LOAD_UTF8_CHARCLASS_ALNUM();
		    n = swash_fetch(PL_utf8_alnum, (U8*)locinput, do_utf8);
		}
		else {
		    ln = isALNUM_LC_uvchr(UNI_TO_NATIVE(ln));
		    n = isALNUM_LC_utf8((U8*)locinput);
		}
	    }
	    else {
		ln = (locinput != PL_bostr) ?
		    UCHARAT(locinput - 1) : '\n';
		if (OP(scan) == BOUND || OP(scan) == NBOUND) {
		    ln = isALNUM(ln);
		    n = isALNUM(nextchr);
		}
		else {
		    ln = isALNUM_LC(ln);
		    n = isALNUM_LC(nextchr);
		}
	    }
	    if (((!ln) == (!n)) == (OP(scan) == BOUND ||
				    OP(scan) == BOUNDL))
		    sayNO;
	    break;
	case SPACEL:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case SPACE:
	    if (!nextchr)
		sayNO;
	    if (do_utf8) {
		if (UTF8_IS_CONTINUED(nextchr)) {
		    LOAD_UTF8_CHARCLASS_SPACE();
		    if (!(OP(scan) == SPACE
			  ? swash_fetch(PL_utf8_space, (U8*)locinput, do_utf8)
			  : isSPACE_LC_utf8((U8*)locinput)))
		    {
			sayNO;
		    }
		    locinput += PL_utf8skip[nextchr];
		    nextchr = UCHARAT(locinput);
		    break;
		}
		if (!(OP(scan) == SPACE
		      ? isSPACE(nextchr) : isSPACE_LC(nextchr)))
		    sayNO;
		nextchr = UCHARAT(++locinput);
	    }
	    else {
		if (!(OP(scan) == SPACE
		      ? isSPACE(nextchr) : isSPACE_LC(nextchr)))
		    sayNO;
		nextchr = UCHARAT(++locinput);
	    }
	    break;
	case NSPACEL:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case NSPACE:
	    if (!nextchr && locinput >= PL_regeol)
		sayNO;
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_SPACE();
		if (OP(scan) == NSPACE
		    ? swash_fetch(PL_utf8_space, (U8*)locinput, do_utf8)
		    : isSPACE_LC_utf8((U8*)locinput))
		{
		    sayNO;
		}
		locinput += PL_utf8skip[nextchr];
		nextchr = UCHARAT(locinput);
		break;
	    }
	    if (OP(scan) == NSPACE
		? isSPACE(nextchr) : isSPACE_LC(nextchr))
		sayNO;
	    nextchr = UCHARAT(++locinput);
	    break;
	case DIGITL:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case DIGIT:
	    if (!nextchr)
		sayNO;
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_DIGIT();
		if (!(OP(scan) == DIGIT
		      ? swash_fetch(PL_utf8_digit, (U8*)locinput, do_utf8)
		      : isDIGIT_LC_utf8((U8*)locinput)))
		{
		    sayNO;
		}
		locinput += PL_utf8skip[nextchr];
		nextchr = UCHARAT(locinput);
		break;
	    }
	    if (!(OP(scan) == DIGIT
		  ? isDIGIT(nextchr) : isDIGIT_LC(nextchr)))
		sayNO;
	    nextchr = UCHARAT(++locinput);
	    break;
	case NDIGITL:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
	case NDIGIT:
	    if (!nextchr && locinput >= PL_regeol)
		sayNO;
	    if (do_utf8) {
		LOAD_UTF8_CHARCLASS_DIGIT();
		if (OP(scan) == NDIGIT
		    ? swash_fetch(PL_utf8_digit, (U8*)locinput, do_utf8)
		    : isDIGIT_LC_utf8((U8*)locinput))
		{
		    sayNO;
		}
		locinput += PL_utf8skip[nextchr];
		nextchr = UCHARAT(locinput);
		break;
	    }
	    if (OP(scan) == NDIGIT
		? isDIGIT(nextchr) : isDIGIT_LC(nextchr))
		sayNO;
	    nextchr = UCHARAT(++locinput);
	    break;
	case CLUMP:
	    if (locinput >= PL_regeol)
		sayNO;
	    if  (do_utf8) {
		LOAD_UTF8_CHARCLASS_MARK();
		if (swash_fetch(PL_utf8_mark,(U8*)locinput, do_utf8))
		    sayNO;
		locinput += PL_utf8skip[nextchr];
		while (locinput < PL_regeol &&
		       swash_fetch(PL_utf8_mark,(U8*)locinput, do_utf8))
		    locinput += UTF8SKIP(locinput);
		if (locinput > PL_regeol)
		    sayNO;
	    } 
	    else
	       locinput++;
	    nextchr = UCHARAT(locinput);
	    break;
	case REFFL:
	    PL_reg_flags |= RF_tainted;
	    /* FALL THROUGH */
        case REF:
	case REFF: {
	    char *s;
	    n = ARG(scan);  /* which paren pair */
	    ln = PL_regstartp[n];
	    PL_reg_leftiter = PL_reg_maxiter;		/* Void cache */
	    if ((I32)*PL_reglastparen < n || ln == -1)
		sayNO;			/* Do not match unless seen CLOSEn. */
	    if (ln == PL_regendp[n])
		break;

	    s = PL_bostr + ln;
	    if (do_utf8 && OP(scan) != REF) {	/* REF can do byte comparison */
		char *l = locinput;
		const char *e = PL_bostr + PL_regendp[n];
		/*
		 * Note that we can't do the "other character" lookup trick as
		 * in the 8-bit case (no pun intended) because in Unicode we
		 * have to map both upper and title case to lower case.
		 */
		if (OP(scan) == REFF) {
		    while (s < e) {
			STRLEN ulen1, ulen2;
			U8 tmpbuf1[UTF8_MAXBYTES_CASE+1];
			U8 tmpbuf2[UTF8_MAXBYTES_CASE+1];

			if (l >= PL_regeol)
			    sayNO;
			toLOWER_utf8((U8*)s, tmpbuf1, &ulen1);
			toLOWER_utf8((U8*)l, tmpbuf2, &ulen2);
			if (ulen1 != ulen2 || memNE((char *)tmpbuf1, (char *)tmpbuf2, ulen1))
			    sayNO;
			s += ulen1;
			l += ulen2;
		    }
		}
		locinput = l;
		nextchr = UCHARAT(locinput);
		break;
	    }

	    /* Inline the first character, for speed. */
	    if (UCHARAT(s) != nextchr &&
		(OP(scan) == REF ||
		 (UCHARAT(s) != ((OP(scan) == REFF
				  ? PL_fold : PL_fold_locale)[nextchr]))))
		sayNO;
	    ln = PL_regendp[n] - ln;
	    if (locinput + ln > PL_regeol)
		sayNO;
	    if (ln > 1 && (OP(scan) == REF
			   ? memNE(s, locinput, ln)
			   : (OP(scan) == REFF
			      ? ibcmp(s, locinput, ln)
			      : ibcmp_locale(s, locinput, ln))))
		sayNO;
	    locinput += ln;
	    nextchr = UCHARAT(locinput);
	    break;
	    }

	case NOTHING:
	case TAIL:
	    break;
	case BACK:
	    break;
	case EVAL:
	{
	    dSP;
	    OP_4tree * const oop = PL_op;
	    COP * const ocurcop = PL_curcop;
	    PAD *old_comppad;
	    SV *ret;
	    struct regexp * const oreg = PL_reg_re;
	
	    n = ARG(scan);
	    PL_op = (OP_4tree*)PL_regdata->data[n];
	    DEBUG_EXECUTE_r( PerlIO_printf(Perl_debug_log, "  re_eval 0x%"UVxf"\n", PTR2UV(PL_op)) );
	    PAD_SAVE_LOCAL(old_comppad, (PAD*)PL_regdata->data[n + 2]);
	    PL_regendp[0] = PL_reg_magic->mg_len = locinput - PL_bostr;

	    {
		SV ** const before = SP;
		CALLRUNOPS(aTHX);			/* Scalar context. */
		SPAGAIN;
		if (SP == before)
		    ret = &PL_sv_undef;   /* protect against empty (?{}) blocks. */
		else {
		    ret = POPs;
		    PUTBACK;
		}
	    }

	    PL_op = oop;
	    PAD_RESTORE_LOCAL(old_comppad);
	    PL_curcop = ocurcop;
	    if (logical) {
		if (logical == 2) {	/* Postponed subexpression. */
		    regexp *re;
		    MAGIC *mg = NULL;
		    re_cc_state state;
                    int toggleutf;
		    register SV *sv;

		    if(SvROK(ret) && SvSMAGICAL(sv = SvRV(ret)))
			mg = mg_find(sv, PERL_MAGIC_qr);
		    else if (SvSMAGICAL(ret)) {
			if (SvGMAGICAL(ret))
			    sv_unmagic(ret, PERL_MAGIC_qr);
			else
			    mg = mg_find(ret, PERL_MAGIC_qr);
		    }

		    if (mg) {
			re = (regexp *)mg->mg_obj;
			(void)ReREFCNT_inc(re);
		    }
		    else {
			STRLEN len;
			const char * const t = SvPV_const(ret, len);
			PMOP pm;
			char * const oprecomp = PL_regprecomp;
			const I32 osize = PL_regsize;
			const I32 onpar = PL_regnpar;

			Zero(&pm, 1, PMOP);
                        if (DO_UTF8(ret)) pm.op_pmdynflags |= PMdf_DYN_UTF8;
			re = CALLREGCOMP(aTHX_ (char*)t, (char*)t + len, &pm);
			if (!(SvFLAGS(ret)
			      & (SVs_TEMP | SVs_PADTMP | SVf_READONLY
				| SVs_GMG)))
			    sv_magic(ret,(SV*)ReREFCNT_inc(re),
					PERL_MAGIC_qr,0,0);
			PL_regprecomp = oprecomp;
			PL_regsize = osize;
			PL_regnpar = onpar;
		    }
		    DEBUG_EXECUTE_r(
			PerlIO_printf(Perl_debug_log,
				      "Entering embedded \"%s%.60s%s%s\"\n",
				      PL_colors[0],
				      re->precomp,
				      PL_colors[1],
				      (strlen(re->precomp) > 60 ? "..." : ""))
			);
		    state.node = next;
		    state.prev = PL_reg_call_cc;
		    state.cc = cc;
		    state.re = PL_reg_re;

		    cc = 0;
		
		    cp = regcppush(0);	/* Save *all* the positions. */
		    REGCP_SET(lastcp);
		    cache_re(re);
		    state.ss = PL_savestack_ix;
		    *PL_reglastparen = 0;
		    *PL_reglastcloseparen = 0;
		    PL_reg_call_cc = &state;
		    PL_reginput = locinput;
		    toggleutf = ((PL_reg_flags & RF_utf8) != 0) ^
				((re->reganch & ROPT_UTF8) != 0);
		    if (toggleutf) PL_reg_flags ^= RF_utf8;

		    /* XXXX This is too dramatic a measure... */
		    PL_reg_maxiter = 0;

		    /* XXX the only recursion left in regmatch() */
		    if (regmatch(re->program + 1)) {
			/* Even though we succeeded, we need to restore
			   global variables, since we may be wrapped inside
			   SUSPEND, thus the match may be not finished yet. */

			/* XXXX Do this only if SUSPENDed? */
			PL_reg_call_cc = state.prev;
			cc = state.cc;
			PL_reg_re = state.re;
			cache_re(PL_reg_re);
			if (toggleutf) PL_reg_flags ^= RF_utf8;

			/* XXXX This is too dramatic a measure... */
			PL_reg_maxiter = 0;

			/* These are needed even if not SUSPEND. */
			ReREFCNT_dec(re);
			regcpblow(cp);
			sayYES;
		    }
		    ReREFCNT_dec(re);
		    REGCP_UNWIND(lastcp);
		    regcppop();
		    PL_reg_call_cc = state.prev;
		    cc = state.cc;
		    PL_reg_re = state.re;
		    cache_re(PL_reg_re);
		    if (toggleutf) PL_reg_flags ^= RF_utf8;

		    /* XXXX This is too dramatic a measure... */
		    PL_reg_maxiter = 0;

		    logical = 0;
		    sayNO;
		}
		sw = SvTRUE(ret);
		logical = 0;
	    }
	    else {
		sv_setsv(save_scalar(PL_replgv), ret);
		cache_re(oreg);
	    }
	    break;
	}
	case OPEN:
	    n = ARG(scan);  /* which paren pair */
	    PL_reg_start_tmp[n] = locinput;
	    if (n > PL_regsize)
		PL_regsize = n;
	    break;
	case CLOSE:
	    n = ARG(scan);  /* which paren pair */
	    PL_regstartp[n] = PL_reg_start_tmp[n] - PL_bostr;
	    PL_regendp[n] = locinput - PL_bostr;
	    if (n > (I32)*PL_reglastparen)
		*PL_reglastparen = n;
	    *PL_reglastcloseparen = n;
	    break;
	case GROUPP:
	    n = ARG(scan);  /* which paren pair */
	    sw = ((I32)*PL_reglastparen >= n && PL_regendp[n] != -1);
	    break;
	case IFTHEN:
	    PL_reg_leftiter = PL_reg_maxiter;		/* Void cache */
	    if (sw)
		next = NEXTOPER(NEXTOPER(scan));
	    else {
		next = scan + ARG(scan);
		if (OP(next) == IFTHEN) /* Fake one. */
		    next = NEXTOPER(NEXTOPER(next));
	    }
	    break;
	case LOGICAL:
	    logical = scan->flags;
	    break;
/*******************************************************************
 cc contains infoblock about the innermost (...)* loop, and
 a pointer to the next outer infoblock.

 Here is how Y(A)*Z is processed (if it is compiled into CURLYX/WHILEM):

   1) After matching Y, regnode for CURLYX is processed;

   2) This regnode mallocs an infoblock, and calls regmatch() recursively
      with the starting point at WHILEM node;

   3) Each hit of WHILEM node tries to match A and Z (in the order
      depending on the current iteration, min/max of {min,max} and
      greediness).  The information about where are nodes for "A"
      and "Z" is read from the infoblock, as is info on how many times "A"
      was already matched, and greediness.

   4) After A matches, the same WHILEM node is hit again.

   5) Each time WHILEM is hit, cc is the infoblock created by CURLYX
      of the same pair.  Thus when WHILEM tries to match Z, it temporarily
      resets cc, since this Y(A)*Z can be a part of some other loop:
      as in (Y(A)*Z)*.  If Z matches, the automaton will hit the WHILEM node
      of the external loop.

 Currently present infoblocks form a tree with a stem formed by PL_curcc
 and whatever it mentions via ->next, and additional attached trees
 corresponding to temporarily unset infoblocks as in "5" above.

 In the following picture, infoblocks for outer loop of
 (Y(A)*?Z)*?T are denoted O, for inner I.  NULL starting block
 is denoted by x.  The matched string is YAAZYAZT.  Temporarily postponed
 infoblocks are drawn below the "reset" infoblock.

 In fact in the picture below we do not show failed matches for Z and T
 by WHILEM blocks.  [We illustrate minimal matches, since for them it is
 more obvious *why* one needs to *temporary* unset infoblocks.]

  Matched	REx position	InfoBlocks	Comment
  		(Y(A)*?Z)*?T	x
  		Y(A)*?Z)*?T	x <- O
  Y		(A)*?Z)*?T	x <- O
  Y		A)*?Z)*?T	x <- O <- I
  YA		)*?Z)*?T	x <- O <- I
  YA		A)*?Z)*?T	x <- O <- I
  YAA		)*?Z)*?T	x <- O <- I
  YAA		Z)*?T		x <- O		# Temporary unset I
				     I

  YAAZ		Y(A)*?Z)*?T	x <- O
				     I

  YAAZY		(A)*?Z)*?T	x <- O
				     I

  YAAZY		A)*?Z)*?T	x <- O <- I
				     I

  YAAZYA	)*?Z)*?T	x <- O <- I	
				     I

  YAAZYA	Z)*?T		x <- O		# Temporary unset I
				     I,I

  YAAZYAZ	)*?T		x <- O
				     I,I

  YAAZYAZ	T		x		# Temporary unset O
				O
				I,I

  YAAZYAZT			x
				O
				I,I
 *******************************************************************/

	case CURLYX: {
		/* No need to save/restore up to this paren */
		I32 parenfloor = scan->flags;

		{
		    CURCUR *newcc;
		    Newx(newcc, 1, CURCUR);
		    oldcc = cc;
		    newcc->oldcc = cc;
		    cc = newcc;
		}
		cp = PL_savestack_ix;
		if (OP(PREVOPER(next)) == NOTHING) /* LONGJMP */
		    next += ARG(next);
		/* XXXX Probably it is better to teach regpush to support
		   parenfloor > PL_regsize... */
		if (parenfloor > (I32)*PL_reglastparen)
		    parenfloor = *PL_reglastparen; /* Pessimization... */
		cc->parenfloor = parenfloor;
		cc->cur = -1;
		cc->min = ARG1(scan);
		cc->max  = ARG2(scan);
		cc->scan = NEXTOPER(scan) + EXTRA_STEP_2ARGS;
		cc->next = next;
		cc->minmod = minmod;
		cc->lastloc = 0;
		PL_reginput = locinput;
		REGMATCH(PREVOPER(next), CURLYX); /* start on the WHILEM */
		/*** all unsaved local vars undefined at this point */
		regcpblow(cp);
		Safefree(cc);
		cc = oldcc;
		saySAME(result);
	    }
	    /* NOTREACHED */
	case WHILEM: {
		/*
		 * This is really hard to understand, because after we match
		 * what we're trying to match, we must make sure the rest of
		 * the REx is going to match for sure, and to do that we have
		 * to go back UP the parse tree by recursing ever deeper.  And
		 * if it fails, we have to reset our parent's current state
		 * that we can try again after backing off.
		 */

		lastloc = cc->lastloc; /* Detection of 0-len. */
		cache_offset = 0;
		cache_bit = 0;
		
		n = cc->cur + 1;	/* how many we know we matched */
		PL_reginput = locinput;

		DEBUG_EXECUTE_r(
		    PerlIO_printf(Perl_debug_log,
				  "%*s  %ld out of %ld..%ld  cc=%"UVxf"\n",
				  REPORT_CODE_OFF+PL_regindent*2, "",
				  (long)n, (long)cc->min,
				  (long)cc->max, PTR2UV(cc))
		    );

		/* If degenerate scan matches "", assume scan done. */

		if (locinput == cc->lastloc && n >= cc->min) {
		    oldcc = cc;
		    cc = cc->oldcc;
		    if (cc)
			ln = cc->cur;
		    DEBUG_EXECUTE_r(
			PerlIO_printf(Perl_debug_log,
			   "%*s  empty match detected, try continuation...\n",
			   REPORT_CODE_OFF+PL_regindent*2, "")
			);
		    REGMATCH(oldcc->next, WHILEM1);
		    /*** all unsaved local vars undefined at this point */
		    cc = oldcc;
		    if (result)
			sayYES;
		    if (cc->oldcc)
			cc->oldcc->cur = ln;
		    sayNO;
		}

		/* First just match a string of min scans. */

		if (n < cc->min) {
		    cc->cur = n;
		    cc->lastloc = locinput;
		    REGMATCH(cc->scan, WHILEM2);
		    /*** all unsaved local vars undefined at this point */
		    if (result)
			sayYES;
		    cc->cur = n - 1;
		    cc->lastloc = lastloc;
		    sayNO;
		}

		if (scan->flags) {
		    /* Check whether we already were at this position.
			Postpone detection until we know the match is not
			*that* much linear. */
		if (!PL_reg_maxiter) {
		    PL_reg_maxiter = (PL_regeol - PL_bostr + 1) * (scan->flags>>4);
		    PL_reg_leftiter = PL_reg_maxiter;
		}
		if (PL_reg_leftiter-- == 0) {
		    const I32 size = (PL_reg_maxiter + 7 + POSCACHE_START)/8;
		    if (PL_reg_poscache) {
			if ((I32)PL_reg_poscache_size < size) {
			    Renew(PL_reg_poscache, size, char);
			    PL_reg_poscache_size = size;
			}
			Zero(PL_reg_poscache, size, char);
		    }
		    else {
			PL_reg_poscache_size = size;
			Newxz(PL_reg_poscache, size, char);
		    }
		    DEBUG_EXECUTE_r(
			PerlIO_printf(Perl_debug_log,
	      "%sDetected a super-linear match, switching on caching%s...\n",
				      PL_colors[4], PL_colors[5])
			);
		}
		if (PL_reg_leftiter < 0) {
		    cache_offset = locinput - PL_bostr;

		    cache_offset = (scan->flags & 0xf) - 1 + POSCACHE_START
			    + cache_offset * (scan->flags>>4);
		    cache_bit = cache_offset % 8;
		    cache_offset /= 8;
		    if (PL_reg_poscache[cache_offset] & (1<<cache_bit)) {
		    DEBUG_EXECUTE_r(
			PerlIO_printf(Perl_debug_log,
				      "%*s  already tried at this position...\n",
				      REPORT_CODE_OFF+PL_regindent*2, "")
			);
			if (PL_reg_poscache[0] & (1<<POSCACHE_SUCCESS))
			    /* cache records success */
			    sayYES;
			else
			    /* cache records failure */
			    sayNO_SILENT;
		    }
		    PL_reg_poscache[cache_offset] |= (1<<cache_bit);
		}
		}

		/* Prefer next over scan for minimal matching. */

		if (cc->minmod) {
		    oldcc = cc;
		    cc = cc->oldcc;
		    if (cc)
			ln = cc->cur;
		    cp = regcppush(oldcc->parenfloor);
		    REGCP_SET(lastcp);
		    REGMATCH(oldcc->next, WHILEM3);
		    /*** all unsaved local vars undefined at this point */
		    cc = oldcc;
		    if (result) {
			regcpblow(cp);
			CACHEsayYES;	/* All done. */
		    }
		    REGCP_UNWIND(lastcp);
		    regcppop();
		    if (cc->oldcc)
			cc->oldcc->cur = ln;

		    if (n >= cc->max) {	/* Maximum greed exceeded? */
			if (ckWARN(WARN_REGEXP) && n >= REG_INFTY
			    && !(PL_reg_flags & RF_warned)) {
			    PL_reg_flags |= RF_warned;
			    Perl_warner(aTHX_ packWARN(WARN_REGEXP), "%s limit (%d) exceeded",
				 "Complex regular subexpression recursion",
				 REG_INFTY - 1);
			}
			CACHEsayNO;
		    }

		    DEBUG_EXECUTE_r(
			PerlIO_printf(Perl_debug_log,
				      "%*s  trying longer...\n",
				      REPORT_CODE_OFF+PL_regindent*2, "")
			);
		    /* Try scanning more and see if it helps. */
		    PL_reginput = locinput;
		    cc->cur = n;
		    cc->lastloc = locinput;
		    cp = regcppush(cc->parenfloor);
		    REGCP_SET(lastcp);
		    REGMATCH(cc->scan, WHILEM4);
		    /*** all unsaved local vars undefined at this point */
		    if (result) {
			regcpblow(cp);
			CACHEsayYES;
		    }
		    REGCP_UNWIND(lastcp);
		    regcppop();
		    cc->cur = n - 1;
		    cc->lastloc = lastloc;
		    CACHEsayNO;
		}

		/* Prefer scan over next for maximal matching. */

		if (n < cc->max) {	/* More greed allowed? */
		    cp = regcppush(cc->parenfloor);
		    cc->cur = n;
		    cc->lastloc = locinput;
		    REGCP_SET(lastcp);
		    REGMATCH(cc->scan, WHILEM5);
		    /*** all unsaved local vars undefined at this point */
		    if (result) {
			regcpblow(cp);
			CACHEsayYES;
		    }
		    REGCP_UNWIND(lastcp);
		    regcppop();		/* Restore some previous $<digit>s? */
		    PL_reginput = locinput;
		    DEBUG_EXECUTE_r(
			PerlIO_printf(Perl_debug_log,
				      "%*s  failed, try continuation...\n",
				      REPORT_CODE_OFF+PL_regindent*2, "")
			);
		}
		if (ckWARN(WARN_REGEXP) && n >= REG_INFTY
			&& !(PL_reg_flags & RF_warned)) {
		    PL_reg_flags |= RF_warned;
		    Perl_warner(aTHX_ packWARN(WARN_REGEXP), "%s limit (%d) exceeded",
			 "Complex regular subexpression recursion",
			 REG_INFTY - 1);
		}

		/* Failed deeper matches of scan, so see if this one works. */
		oldcc = cc;
		cc = cc->oldcc;
		if (cc)
		    ln = cc->cur;
		REGMATCH(oldcc->next, WHILEM6);
		/*** all unsaved local vars undefined at this point */
		cc = oldcc;
		if (result)
		    CACHEsayYES;
		if (cc->oldcc)
		    cc->oldcc->cur = ln;
		cc->cur = n - 1;
		cc->lastloc = lastloc;
		CACHEsayNO;
	    }
	    /* NOTREACHED */
	case BRANCHJ:
	    next = scan + ARG(scan);
	    if (next == scan)
		next = NULL;
	    inner = NEXTOPER(NEXTOPER(scan));
	    goto do_branch;
	case BRANCH:
	    inner = NEXTOPER(scan);
	  do_branch:
	    {
		c1 = OP(scan);
		if (OP(next) != c1)	/* No choice. */
		    next = inner;	/* Avoid recursion. */
		else {
		    const I32 lastparen = *PL_reglastparen;
		    /* Put unwinding data on stack */
		    const I32 unwind1 = SSNEWt(1,re_unwind_branch_t);
		    re_unwind_branch_t * const uw = SSPTRt(unwind1,re_unwind_branch_t);

		    uw->prev = unwind;
		    unwind = unwind1;
		    uw->type = ((c1 == BRANCH)
				? RE_UNWIND_BRANCH
				: RE_UNWIND_BRANCHJ);
		    uw->lastparen = lastparen;
		    uw->next = next;
		    uw->locinput = locinput;
		    uw->nextchr = nextchr;
#ifdef DEBUGGING
		    uw->regindent = ++PL_regindent;
#endif

		    REGCP_SET(uw->lastcp);

		    /* Now go into the first branch */
		    next = inner;
		}
	    }
	    break;
	case MINMOD:
	    minmod = 1;
	    break;
	case CURLYM:
	{
	    curlym_l = matches = 0;
	
	    /* We suppose that the next guy does not need
	       backtracking: in particular, it is of constant non-zero length,
	       and has no parenths to influence future backrefs. */
	    ln = ARG1(scan);  /* min to match */
	    n  = ARG2(scan);  /* max to match */
	    paren = scan->flags;
	    if (paren) {
		if (paren > PL_regsize)
		    PL_regsize = paren;
		if (paren > (I32)*PL_reglastparen)
		    *PL_reglastparen = paren;
	    }
	    scan = NEXTOPER(scan) + NODE_STEP_REGNODE;
	    if (paren)
		scan += NEXT_OFF(scan); /* Skip former OPEN. */
	    PL_reginput = locinput;
	    maxwanted = minmod ? ln : n;
	    if (maxwanted) {
		while (PL_reginput < PL_regeol && matches < maxwanted) {
		    REGMATCH(scan, CURLYM1);
		    /*** all unsaved local vars undefined at this point */
		    if (!result)
			break;
		    /* on first match, determine length, curlym_l */
		    if (!matches++) {
			if (PL_reg_match_utf8) {
			    char *s = locinput;
			    while (s < PL_reginput) {
				curlym_l++;
				s += UTF8SKIP(s);
			    }
			}
			else {
			    curlym_l = PL_reginput - locinput;
			}
			if (curlym_l == 0) {
			    matches = maxwanted;
			    break;
			}
		    }
		    locinput = PL_reginput;
		}
	    }

	    PL_reginput = locinput;

	    if (minmod) {
		minmod = 0;
		if (ln && matches < ln)
		    sayNO;
		if (HAS_TEXT(next) || JUMPABLE(next)) {
		    regnode *text_node = next;

		    if (! HAS_TEXT(text_node)) FIND_NEXT_IMPT(text_node);

		    if (! HAS_TEXT(text_node)) c1 = c2 = -1000;
		    else {
			if (PL_regkind[(U8)OP(text_node)] == REF) {
			    c1 = c2 = -1000;
			    goto assume_ok_MM;
			}
			else { c1 = (U8)*STRING(text_node); }
			if (OP(text_node) == EXACTF || OP(text_node) == REFF)
			    c2 = PL_fold[c1];
			else if (OP(text_node) == EXACTFL || OP(text_node) == REFFL)
			    c2 = PL_fold_locale[c1];
			else
			    c2 = c1;
		    }
		}
		else
		    c1 = c2 = -1000;
	    assume_ok_MM:
		REGCP_SET(lastcp);
		while (n >= ln || (n == REG_INFTY && ln > 0)) { /* ln overflow ? */
		    /* If it could work, try it. */
		    if (c1 == -1000 ||
			UCHARAT(PL_reginput) == c1 ||
			UCHARAT(PL_reginput) == c2)
		    {
			if (paren) {
			    if (ln) {
				PL_regstartp[paren] =
				    HOPc(PL_reginput, -curlym_l) - PL_bostr;
				PL_regendp[paren] = PL_reginput - PL_bostr;
			    }
			    else
				PL_regendp[paren] = -1;
			}
			REGMATCH(next, CURLYM2);
			/*** all unsaved local vars undefined at this point */
			if (result)
			    sayYES;
			REGCP_UNWIND(lastcp);
		    }
		    /* Couldn't or didn't -- move forward. */
		    PL_reginput = locinput;
		    REGMATCH(scan, CURLYM3);
		    /*** all unsaved local vars undefined at this point */
		    if (result) {
			ln++;
			locinput = PL_reginput;
		    }
		    else
			sayNO;
		}
	    }
	    else {
		DEBUG_EXECUTE_r(
		    PerlIO_printf(Perl_debug_log,
			      "%*s  matched %"IVdf" times, len=%"IVdf"...\n",
			      (int)(REPORT_CODE_OFF+PL_regindent*2), "",
			      (IV) matches, (IV)curlym_l)
		    );
		if (matches >= ln) {
		    if (HAS_TEXT(next) || JUMPABLE(next)) {
			regnode *text_node = next;

			if (! HAS_TEXT(text_node)) FIND_NEXT_IMPT(text_node);

			if (! HAS_TEXT(text_node)) c1 = c2 = -1000;
			else {
			    if (PL_regkind[(U8)OP(text_node)] == REF) {
				c1 = c2 = -1000;
				goto assume_ok_REG;
			    }
			    else { c1 = (U8)*STRING(text_node); }

			    if (OP(text_node) == EXACTF || OP(text_node) == REFF)
				c2 = PL_fold[c1];
			    else if (OP(text_node) == EXACTFL || OP(text_node) == REFFL)
				c2 = PL_fold_locale[c1];
			    else
				c2 = c1;
			}
		    }
		    else
			c1 = c2 = -1000;
		}
	    assume_ok_REG:
		REGCP_SET(lastcp);
		while (matches >= ln) {
		    /* If it could work, try it. */
		    if (c1 == -1000 ||
			UCHARAT(PL_reginput) == c1 ||
			UCHARAT(PL_reginput) == c2)
		    {
			DEBUG_EXECUTE_r(
			    PerlIO_printf(Perl_debug_log,
				"%*s  trying tail with matches=%"IVdf"...\n",
				(int)(REPORT_CODE_OFF+PL_regindent*2),
				"", (IV)matches)
			    );
			if (paren) {
			    if (matches) {
				PL_regstartp[paren]
				    = HOPc(PL_reginput, -curlym_l) - PL_bostr;
				PL_regendp[paren] = PL_reginput - PL_bostr;
			    }
			    else
				PL_regendp[paren] = -1;
			}
			REGMATCH(next, CURLYM4);
			/*** all unsaved local vars undefined at this point */
			if (result)
			    sayYES;
			REGCP_UNWIND(lastcp);
		    }
		    /* Couldn't or didn't -- back up. */
		    matches--;
		    locinput = HOPc(locinput, -curlym_l);
		    PL_reginput = locinput;
		}
	    }
	    sayNO;
	    /* NOTREACHED */
	    break;
	}
	case CURLYN:
	    paren = scan->flags;	/* Which paren to set */
	    if (paren > PL_regsize)
		PL_regsize = paren;
	    if (paren > (I32)*PL_reglastparen)
		*PL_reglastparen = paren;
	    ln = ARG1(scan);  /* min to match */
	    n  = ARG2(scan);  /* max to match */
            scan = regnext(NEXTOPER(scan) + NODE_STEP_REGNODE);
	    goto repeat;
	case CURLY:
	    paren = 0;
	    ln = ARG1(scan);  /* min to match */
	    n  = ARG2(scan);  /* max to match */
	    scan = NEXTOPER(scan) + NODE_STEP_REGNODE;
	    goto repeat;
	case STAR:
	    ln = 0;
	    n = REG_INFTY;
	    scan = NEXTOPER(scan);
	    paren = 0;
	    goto repeat;
	case PLUS:
	    ln = 1;
	    n = REG_INFTY;
	    scan = NEXTOPER(scan);
	    paren = 0;
	  repeat:
	    /*
	    * Lookahead to avoid useless match attempts
	    * when we know what character comes next.
	    */

	    /*
	    * Used to only do .*x and .*?x, but now it allows
	    * for )'s, ('s and (?{ ... })'s to be in the way
	    * of the quantifier and the EXACT-like node.  -- japhy
	    */

	    if (HAS_TEXT(next) || JUMPABLE(next)) {
		U8 *s;
		regnode *text_node = next;

		if (! HAS_TEXT(text_node)) FIND_NEXT_IMPT(text_node);

		if (! HAS_TEXT(text_node)) c1 = c2 = -1000;
		else {
		    if (PL_regkind[(U8)OP(text_node)] == REF) {
			c1 = c2 = -1000;
			goto assume_ok_easy;
		    }
		    else { s = (U8*)STRING(text_node); }

		    if (!UTF) {
			c2 = c1 = *s;
			if (OP(text_node) == EXACTF || OP(text_node) == REFF)
			    c2 = PL_fold[c1];
			else if (OP(text_node) == EXACTFL || OP(text_node) == REFFL)
			    c2 = PL_fold_locale[c1];
		    }
		    else { /* UTF */
			if (OP(text_node) == EXACTF || OP(text_node) == REFF) {
			     STRLEN ulen1, ulen2;
			     U8 tmpbuf1[UTF8_MAXBYTES_CASE+1];
			     U8 tmpbuf2[UTF8_MAXBYTES_CASE+1];

			     to_utf8_lower((U8*)s, tmpbuf1, &ulen1);
			     to_utf8_upper((U8*)s, tmpbuf2, &ulen2);

			     c1 = utf8n_to_uvuni(tmpbuf1, UTF8_MAXBYTES, 0,
						 uniflags);
			     c2 = utf8n_to_uvuni(tmpbuf2, UTF8_MAXBYTES, 0,
						 uniflags);
			}
			else {
			    c2 = c1 = utf8n_to_uvchr(s, UTF8_MAXBYTES, 0,
						     uniflags);
			}
		    }
		}
	    }
	    else
		c1 = c2 = -1000;
	assume_ok_easy:
	    PL_reginput = locinput;
	    if (minmod) {
		minmod = 0;
		if (ln && regrepeat(scan, ln) < ln)
		    sayNO;
		locinput = PL_reginput;
		REGCP_SET(lastcp);
		if (c1 != -1000) {
		    old = locinput;
		    count = 0;

		    if  (n == REG_INFTY) {
			e = PL_regeol - 1;
			if (do_utf8)
			    while (UTF8_IS_CONTINUATION(*(U8*)e))
				e--;
		    }
		    else if (do_utf8) {
			int m = n - ln;
			for (e = locinput;
			     m >0 && e + UTF8SKIP(e) <= PL_regeol; m--)
			    e += UTF8SKIP(e);
		    }
		    else {
			e = locinput + n - ln;
			if (e >= PL_regeol)
			    e = PL_regeol - 1;
		    }
		    while (1) {
			/* Find place 'next' could work */
			if (!do_utf8) {
			    if (c1 == c2) {
				while (locinput <= e &&
				       UCHARAT(locinput) != c1)
				    locinput++;
			    } else {
				while (locinput <= e
				       && UCHARAT(locinput) != c1
				       && UCHARAT(locinput) != c2)
				    locinput++;
			    }
			    count = locinput - old;
			}
			else {
			    if (c1 == c2) {
				STRLEN len;
				/* count initialised to
				 * utf8_distance(old, locinput) */
				while (locinput <= e &&
				       utf8n_to_uvchr((U8*)locinput,
						      UTF8_MAXBYTES, &len,
						      uniflags) != (UV)c1) {
				    locinput += len;
				    count++;
				}
			    } else {
				STRLEN len;
				/* count initialised to
				 * utf8_distance(old, locinput) */
				while (locinput <= e) {
				    UV c = utf8n_to_uvchr((U8*)locinput,
							  UTF8_MAXBYTES, &len,
							  uniflags);
				    if (c == (UV)c1 || c == (UV)c2)
					break;
				    locinput += len;
				    count++;
				}
			    }
			}
			if (locinput > e)
			    sayNO;
			/* PL_reginput == old now */
			if (locinput != old) {
			    ln = 1;	/* Did some */
			    if (regrepeat(scan, count) < count)
				sayNO;
			}
			/* PL_reginput == locinput now */
			TRYPAREN(paren, ln, locinput, PLUS1);
			/*** all unsaved local vars undefined at this point */
			PL_reginput = locinput;	/* Could be reset... */
			REGCP_UNWIND(lastcp);
			/* Couldn't or didn't -- move forward. */
			old = locinput;
			if (do_utf8)
			    locinput += UTF8SKIP(locinput);
			else
			    locinput++;
			count = 1;
		    }
		}
		else
		while (n >= ln || (n == REG_INFTY && ln > 0)) { /* ln overflow ? */
		    UV c;
		    if (c1 != -1000) {
			if (do_utf8)
			    c = utf8n_to_uvchr((U8*)PL_reginput,
					       UTF8_MAXBYTES, 0,
					       uniflags);
			else
			    c = UCHARAT(PL_reginput);
			/* If it could work, try it. */
		        if (c == (UV)c1 || c == (UV)c2)
		        {
			    TRYPAREN(paren, ln, PL_reginput, PLUS2);
			    /*** all unsaved local vars undefined at this point */
			    REGCP_UNWIND(lastcp);
		        }
		    }
		    /* If it could work, try it. */
		    else if (c1 == -1000)
		    {
			TRYPAREN(paren, ln, PL_reginput, PLUS3);
			/*** all unsaved local vars undefined at this point */
			REGCP_UNWIND(lastcp);
		    }
		    /* Couldn't or didn't -- move forward. */
		    PL_reginput = locinput;
		    if (regrepeat(scan, 1)) {
			ln++;
			locinput = PL_reginput;
		    }
		    else
			sayNO;
		}
	    }
	    else {
		n = regrepeat(scan, n);
		locinput = PL_reginput;
		if (ln < n && PL_regkind[(U8)OP(next)] == EOL &&
		    (OP(next) != MEOL ||
			OP(next) == SEOL || OP(next) == EOS))
		{
		    ln = n;			/* why back off? */
		    /* ...because $ and \Z can match before *and* after
		       newline at the end.  Consider "\n\n" =~ /\n+\Z\n/.
		       We should back off by one in this case. */
		    if (UCHARAT(PL_reginput - 1) == '\n' && OP(next) != EOS)
			ln--;
		}
		REGCP_SET(lastcp);
		{
		    UV c = 0;
		    while (n >= ln) {
			if (c1 != -1000) {
			    if (do_utf8)
				c = utf8n_to_uvchr((U8*)PL_reginput,
						   UTF8_MAXBYTES, 0,
						   uniflags);
			    else
				c = UCHARAT(PL_reginput);
			}
			/* If it could work, try it. */
			if (c1 == -1000 || c == (UV)c1 || c == (UV)c2)
			    {
				TRYPAREN(paren, n, PL_reginput, PLUS4);
				/*** all unsaved local vars undefined at this point */
				REGCP_UNWIND(lastcp);
			    }
			/* Couldn't or didn't -- back up. */
			n--;
			PL_reginput = locinput = HOPc(locinput, -1);
		    }
		}
	    }
	    sayNO;
	    break;
	case END:
	    if (PL_reg_call_cc) {
		cur_call_cc = PL_reg_call_cc;
		end_re = PL_reg_re;

		/* Save *all* the positions. */
		cp = regcppush(0);
		REGCP_SET(lastcp);

		/* Restore parens of the caller. */
		{
		    I32 tmp = PL_savestack_ix;
		    PL_savestack_ix = PL_reg_call_cc->ss;
		    regcppop();
		    PL_savestack_ix = tmp;
		}

		/* Make position available to the callcc. */
		PL_reginput = locinput;

		cache_re(PL_reg_call_cc->re);
		oldcc = cc;
		cc = PL_reg_call_cc->cc;
		PL_reg_call_cc = PL_reg_call_cc->prev;
		REGMATCH(cur_call_cc->node, END);
		/*** all unsaved local vars undefined at this point */
		if (result) {
		    PL_reg_call_cc = cur_call_cc;
		    regcpblow(cp);
		    sayYES;
		}
		REGCP_UNWIND(lastcp);
		regcppop();
		PL_reg_call_cc = cur_call_cc;
		cc = oldcc;
		PL_reg_re = end_re;
		cache_re(end_re);

		DEBUG_EXECUTE_r(
		    PerlIO_printf(Perl_debug_log,
				  "%*s  continuation failed...\n",
				  REPORT_CODE_OFF+PL_regindent*2, "")
		    );
		sayNO_SILENT;
	    }
	    if (locinput < PL_regtill) {
		DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log,
				      "%sMatch possible, but length=%ld is smaller than requested=%ld, failing!%s\n",
				      PL_colors[4],
				      (long)(locinput - PL_reg_starttry),
				      (long)(PL_regtill - PL_reg_starttry),
				      PL_colors[5]));
		sayNO_FINAL;		/* Cannot match: too short. */
	    }
	    PL_reginput = locinput;	/* put where regtry can find it */
	    sayYES_FINAL;		/* Success! */
	case SUCCEED:
	    PL_reginput = locinput;	/* put where regtry can find it */
	    sayYES_LOUD;		/* Success! */
	case SUSPEND:
	    n = 1;
	    PL_reginput = locinput;
	    goto do_ifmatch;	
	case UNLESSM:
	    n = 0;
	    if (scan->flags) {
		char *s = HOPBACKc(locinput, scan->flags);
		if (!s)
		    goto say_yes;
		PL_reginput = s;
	    }
	    else
		PL_reginput = locinput;
	    goto do_ifmatch;
	case IFMATCH:
	    n = 1;
	    if (scan->flags) {
		char *s = HOPBACKc(locinput, scan->flags);
		if (!s)
		    goto say_no;
		PL_reginput = s;
	    }
	    else
		PL_reginput = locinput;

	  do_ifmatch:
	    REGMATCH(NEXTOPER(NEXTOPER(scan)), IFMATCH);
	    /*** all unsaved local vars undefined at this point */
	    if (result != n) {
	      say_no:
		if (logical) {
		    logical = 0;
		    sw = 0;
		    goto do_longjump;
		}
		else
		    sayNO;
	    }
	  say_yes:
	    if (logical) {
		logical = 0;
		sw = 1;
	    }
	    if (OP(scan) == SUSPEND) {
		locinput = PL_reginput;
		nextchr = UCHARAT(locinput);
	    }
	    /* FALL THROUGH. */
	case LONGJMP:
	  do_longjump:
	    next = scan + ARG(scan);
	    if (next == scan)
		next = NULL;
	    break;
	default:
	    PerlIO_printf(Perl_error_log, "%"UVxf" %d\n",
			  PTR2UV(scan), OP(scan));
	    Perl_croak(aTHX_ "regexp memory corruption");
	}

      reenter:
	scan = next;
	continue;
	/* NOTREACHED */

	/* simulate recursively calling regmatch(), but without actually
	 * recursing - ie save the current state on the heap rather than on
	 * the stack, then re-enter the loop. This avoids complex regexes
	 * blowing the processor stack */

      start_recurse:
	{
	    /* save existing local variables */
	    struct regmatch_state *p;

	    Newx(p, 1, struct regmatch_state);
	    p->prev_state = prev_state;
	    prev_state = p;
	    p->resume_state = resume_state;
	    p->scan = scan;
	    p->next = next;
	    p->minmod = minmod;
	    p->sw = sw;
	    p->logical = logical;
	    p->unwind = unwind;
	    p->cc = cc;
	    p->locinput = locinput;
	    p->n = n;
	    p->ln = ln;
	    p->c1 = c1;
	    p->c2 = c2;
	    p->paren = paren;
	    p->cp = cp;
	    p->lastcp = lastcp;
	    p->oldcc = oldcc;
	    p->lastloc = lastloc;
	    p->cache_offset = cache_offset;
	    p->cache_bit = cache_bit;
	    p->curlym_l = curlym_l;
	    p->matches = matches;
	    p->maxwanted = maxwanted;
	    p->e = e;
	    p->old = old;
	    p->count = count;
	    p->cur_call_cc = cur_call_cc;
	    p->end_re = end_re;
	    p->accept_buff = accept_buff;
	    p->accepted = accepted;
	    p->reg_call_cc = PL_reg_call_cc;

	    scan = new_scan;
	    locinput = PL_reginput;
    	    nextchr = UCHARAT(locinput);
	    minmod = 0;
	    sw = 0;
	    logical = 0;
	    unwind = 0;
#ifdef DEBUGGING
	    PL_regindent++;
#endif
	}
    }

    /*
    * We get here only if there's trouble -- normally "case END" is
    * the terminating point.
    */
    Perl_croak(aTHX_ "corrupted regexp pointers");
    /*NOTREACHED*/
    sayNO;

yes_loud:
    DEBUG_EXECUTE_r(
	PerlIO_printf(Perl_debug_log,
		      "%*s  %scould match...%s\n",
		      REPORT_CODE_OFF+PL_regindent*2, "", PL_colors[4], PL_colors[5])
	);
    goto yes;
yes_final:
    DEBUG_EXECUTE_r(PerlIO_printf(Perl_debug_log, "%sMatch successful!%s\n",
			  PL_colors[4], PL_colors[5]));
yes:
#ifdef DEBUGGING
    PL_regindent--;
#endif

    result = 1;
    goto exit_level;

no:
    DEBUG_EXECUTE_r(
	PerlIO_printf(Perl_debug_log,
		      "%*s  %sfailed...%s\n",
		      REPORT_CODE_OFF+PL_regindent*2, "", PL_colors[4], PL_colors[5])
	);
    goto do_no;
no_final:
do_no:
    if (unwind) {
	re_unwind_t * const uw = SSPTRt(unwind,re_unwind_t);

	switch (uw->type) {
	case RE_UNWIND_BRANCH:
	case RE_UNWIND_BRANCHJ:
	{
	    re_unwind_branch_t * const uwb = &(uw->branch);
	    const I32 lastparen = uwb->lastparen;
	
	    REGCP_UNWIND(uwb->lastcp);
	    for (n = *PL_reglastparen; n > lastparen; n--)
		PL_regendp[n] = -1;
	    *PL_reglastparen = n;
	    scan = next = uwb->next;
	    if ( !scan ||
		 OP(scan) != (uwb->type == RE_UNWIND_BRANCH
			      ? BRANCH : BRANCHJ) ) {		/* Failure */
		unwind = uwb->prev;
#ifdef DEBUGGING
		PL_regindent--;
#endif
		goto do_no;
	    }
	    /* Have more choice yet.  Reuse the same uwb.  */
	    if ((n = (uwb->type == RE_UNWIND_BRANCH
		      ? NEXT_OFF(next) : ARG(next))))
		next += n;
	    else
		next = NULL;	/* XXXX Needn't unwinding in this case... */
	    uwb->next = next;
	    next = NEXTOPER(scan);
	    if (uwb->type == RE_UNWIND_BRANCHJ)
		next = NEXTOPER(next);
	    locinput = uwb->locinput;
	    nextchr = uwb->nextchr;
#ifdef DEBUGGING
	    PL_regindent = uwb->regindent;
#endif

	    goto reenter;
	}
	/* NOTREACHED */
	default:
	    Perl_croak(aTHX_ "regexp unwind memory corruption");
	}
	/* NOTREACHED */
    }
#ifdef DEBUGGING
    PL_regindent--;
#endif
    result = 0;
exit_level:
    if (prev_state) {
	/* restore previous state and re-enter */

	struct regmatch_state *p = prev_state;
	resume_state = p->resume_state;
	scan = p->scan;
	next = p->next;
	minmod = p->minmod;
	sw = p->sw;
	logical = p->logical;
	unwind = p->unwind;
	cc = p->cc;
	locinput = p->locinput;
	nextchr = UCHARAT(locinput);
	n = p->n;
	ln = p->ln;
	c1 = p->c1;
	c2 = p->c2;
	paren = p->paren;
	cp = p->cp;
	lastcp = p->lastcp;
	oldcc = p->oldcc;
	lastloc = p->lastloc;
	cache_offset = p->cache_offset;
	cache_bit = p->cache_bit;
	curlym_l = p->curlym_l;
	matches = p->matches;
	maxwanted = p->maxwanted;
	e = p->e;
	old = p->old;
	count = p->count;
	cur_call_cc = p->cur_call_cc;
	end_re = p->end_re;
	accept_buff = p->accept_buff;
	accepted = p->accepted;
	PL_reg_call_cc = p->reg_call_cc;
	prev_state = p->prev_state;
	Safefree(p);

	switch (resume_state) {
	case resume_TRIE1:
	    goto resume_point_TRIE1;
	case resume_TRIE2:
	    goto resume_point_TRIE2;
	case resume_CURLYX:
	    goto resume_point_CURLYX;
	case resume_WHILEM1:
	    goto resume_point_WHILEM1;
	case resume_WHILEM2:
	    goto resume_point_WHILEM2;
	case resume_WHILEM3:
	    goto resume_point_WHILEM3;
	case resume_WHILEM4:
	    goto resume_point_WHILEM4;
	case resume_WHILEM5:
	    goto resume_point_WHILEM5;
	case resume_WHILEM6:
	    goto resume_point_WHILEM6;
	case resume_CURLYM1:
	    goto resume_point_CURLYM1;
	case resume_CURLYM2:
	    goto resume_point_CURLYM2;
	case resume_CURLYM3:
	    goto resume_point_CURLYM3;
	case resume_CURLYM4:
	    goto resume_point_CURLYM4;
	case resume_IFMATCH:
	    goto resume_point_IFMATCH;
	case resume_PLUS1:
	    goto resume_point_PLUS1;
	case resume_PLUS2:
	    goto resume_point_PLUS2;
	case resume_PLUS3:
	    goto resume_point_PLUS3;
	case resume_PLUS4:
	    goto resume_point_PLUS4;
	case resume_END:
	    goto resume_point_END;
	default:
	    Perl_croak(aTHX_ "regexp resume memory corruption");
	}
	/* NOTREACHED */
    }
    return result;

}

/*
 - regrepeat - repeatedly match something simple, report how many
 */
/*
 * [This routine now assumes that it will only match on things of length 1.
 * That was true before, but now we assume scan - reginput is the count,
 * rather than incrementing count on every character.  [Er, except utf8.]]
 */
STATIC I32
S_regrepeat(pTHX_ const regnode *p, I32 max)
{
    dVAR;
    register char *scan;
    register I32 c;
    register char *loceol = PL_regeol;
    register I32 hardcount = 0;
    register bool do_utf8 = PL_reg_match_utf8;

    scan = PL_reginput;
    if (max == REG_INFTY)
	max = I32_MAX;
    else if (max < loceol - scan)
      loceol = scan + max;
    switch (OP(p)) {
    case REG_ANY:
	if (do_utf8) {
	    loceol = PL_regeol;
	    while (scan < loceol && hardcount < max && *scan != '\n') {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && *scan != '\n')
		scan++;
	}
	break;
    case SANY:
        if (do_utf8) {
	    loceol = PL_regeol;
	    while (scan < loceol && hardcount < max) {
	        scan += UTF8SKIP(scan);
		hardcount++;
	    }
	}
	else
	    scan = loceol;
	break;
    case CANY:
	scan = loceol;
	break;
    case EXACT:		/* length of string is 1 */
	c = (U8)*STRING(p);
	while (scan < loceol && UCHARAT(scan) == c)
	    scan++;
	break;
    case EXACTF:	/* length of string is 1 */
	c = (U8)*STRING(p);
	while (scan < loceol &&
	       (UCHARAT(scan) == c || UCHARAT(scan) == PL_fold[c]))
	    scan++;
	break;
    case EXACTFL:	/* length of string is 1 */
	PL_reg_flags |= RF_tainted;
	c = (U8)*STRING(p);
	while (scan < loceol &&
	       (UCHARAT(scan) == c || UCHARAT(scan) == PL_fold_locale[c]))
	    scan++;
	break;
    case ANYOF:
	if (do_utf8) {
	    loceol = PL_regeol;
	    while (hardcount < max && scan < loceol &&
		   reginclass(p, (U8*)scan, 0, do_utf8)) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && REGINCLASS(p, (U8*)scan))
		scan++;
	}
	break;
    case ALNUM:
	if (do_utf8) {
	    loceol = PL_regeol;
	    LOAD_UTF8_CHARCLASS_ALNUM();
	    while (hardcount < max && scan < loceol &&
		   swash_fetch(PL_utf8_alnum, (U8*)scan, do_utf8)) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && isALNUM(*scan))
		scan++;
	}
	break;
    case ALNUML:
	PL_reg_flags |= RF_tainted;
	if (do_utf8) {
	    loceol = PL_regeol;
	    while (hardcount < max && scan < loceol &&
		   isALNUM_LC_utf8((U8*)scan)) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && isALNUM_LC(*scan))
		scan++;
	}
	break;
    case NALNUM:
	if (do_utf8) {
	    loceol = PL_regeol;
	    LOAD_UTF8_CHARCLASS_ALNUM();
	    while (hardcount < max && scan < loceol &&
		   !swash_fetch(PL_utf8_alnum, (U8*)scan, do_utf8)) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && !isALNUM(*scan))
		scan++;
	}
	break;
    case NALNUML:
	PL_reg_flags |= RF_tainted;
	if (do_utf8) {
	    loceol = PL_regeol;
	    while (hardcount < max && scan < loceol &&
		   !isALNUM_LC_utf8((U8*)scan)) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && !isALNUM_LC(*scan))
		scan++;
	}
	break;
    case SPACE:
	if (do_utf8) {
	    loceol = PL_regeol;
	    LOAD_UTF8_CHARCLASS_SPACE();
	    while (hardcount < max && scan < loceol &&
		   (*scan == ' ' ||
		    swash_fetch(PL_utf8_space,(U8*)scan, do_utf8))) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && isSPACE(*scan))
		scan++;
	}
	break;
    case SPACEL:
	PL_reg_flags |= RF_tainted;
	if (do_utf8) {
	    loceol = PL_regeol;
	    while (hardcount < max && scan < loceol &&
		   (*scan == ' ' || isSPACE_LC_utf8((U8*)scan))) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && isSPACE_LC(*scan))
		scan++;
	}
	break;
    case NSPACE:
	if (do_utf8) {
	    loceol = PL_regeol;
	    LOAD_UTF8_CHARCLASS_SPACE();
	    while (hardcount < max && scan < loceol &&
		   !(*scan == ' ' ||
		     swash_fetch(PL_utf8_space,(U8*)scan, do_utf8))) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && !isSPACE(*scan))
		scan++;
	    break;
	}
    case NSPACEL:
	PL_reg_flags |= RF_tainted;
	if (do_utf8) {
	    loceol = PL_regeol;
	    while (hardcount < max && scan < loceol &&
		   !(*scan == ' ' || isSPACE_LC_utf8((U8*)scan))) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && !isSPACE_LC(*scan))
		scan++;
	}
	break;
    case DIGIT:
	if (do_utf8) {
	    loceol = PL_regeol;
	    LOAD_UTF8_CHARCLASS_DIGIT();
	    while (hardcount < max && scan < loceol &&
		   swash_fetch(PL_utf8_digit, (U8*)scan, do_utf8)) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && isDIGIT(*scan))
		scan++;
	}
	break;
    case NDIGIT:
	if (do_utf8) {
	    loceol = PL_regeol;
	    LOAD_UTF8_CHARCLASS_DIGIT();
	    while (hardcount < max && scan < loceol &&
		   !swash_fetch(PL_utf8_digit, (U8*)scan, do_utf8)) {
		scan += UTF8SKIP(scan);
		hardcount++;
	    }
	} else {
	    while (scan < loceol && !isDIGIT(*scan))
		scan++;
	}
	break;
    default:		/* Called on something of 0 width. */
	break;		/* So match right here or not at all. */
    }

    if (hardcount)
	c = hardcount;
    else
	c = scan - PL_reginput;
    PL_reginput = scan;

    DEBUG_r({
	        SV *re_debug_flags = NULL;
		SV * const prop = sv_newmortal();
                GET_RE_DEBUG_FLAGS;
                DEBUG_EXECUTE_r({
		regprop(prop, p);
		PerlIO_printf(Perl_debug_log,
			      "%*s  %s can match %"IVdf" times out of %"IVdf"...\n",
			      REPORT_CODE_OFF+1, "", SvPVX_const(prop),(IV)c,(IV)max);
	});
	});

    return(c);
}


/*
- regclass_swash - prepare the utf8 swash
*/

SV *
Perl_regclass_swash(pTHX_ register const regnode* node, bool doinit, SV** listsvp, SV **altsvp)
{
    dVAR;
    SV *sw  = NULL;
    SV *si  = NULL;
    SV *alt = NULL;

    if (PL_regdata && PL_regdata->count) {
	const U32 n = ARG(node);

	if (PL_regdata->what[n] == 's') {
	    SV * const rv = (SV*)PL_regdata->data[n];
	    AV * const av = (AV*)SvRV((SV*)rv);
	    SV **const ary = AvARRAY(av);
	    SV **a, **b;
	
	    /* See the end of regcomp.c:S_regclass() for
	     * documentation of these array elements. */

	    si = *ary;
	    a  = SvROK(ary[1]) ? &ary[1] : 0;
	    b  = SvTYPE(ary[2]) == SVt_PVAV ? &ary[2] : 0;

	    if (a)
		sw = *a;
	    else if (si && doinit) {
		sw = swash_init("utf8", "", si, 1, 0);
		(void)av_store(av, 1, sw);
	    }
	    if (b)
	        alt = *b;
	}
    }
	
    if (listsvp)
	*listsvp = si;
    if (altsvp)
	*altsvp  = alt;

    return sw;
}

/*
 - reginclass - determine if a character falls into a character class
 
  The n is the ANYOF regnode, the p is the target string, lenp
  is pointer to the maximum length of how far to go in the p
  (if the lenp is zero, UTF8SKIP(p) is used),
  do_utf8 tells whether the target string is in UTF-8.

 */

STATIC bool
S_reginclass(pTHX_ register const regnode *n, register const U8* p, STRLEN* lenp, register bool do_utf8)
{
    dVAR;
    const char flags = ANYOF_FLAGS(n);
    bool match = FALSE;
    UV c = *p;
    STRLEN len = 0;
    STRLEN plen;

    if (do_utf8 && !UTF8_IS_INVARIANT(c)) {
	c = utf8n_to_uvchr(p, UTF8_MAXBYTES, &len,
			    ckWARN(WARN_UTF8) ? UTF8_CHECK_ONLY :
					UTF8_ALLOW_ANYUV|UTF8_CHECK_ONLY);
	if (len == (STRLEN)-1)
	    Perl_croak(aTHX_ "Malformed UTF-8 character (fatal)");
    }

    plen = lenp ? *lenp : UNISKIP(NATIVE_TO_UNI(c));
    if (do_utf8 || (flags & ANYOF_UNICODE)) {
        if (lenp)
	    *lenp = 0;
	if (do_utf8 && !ANYOF_RUNTIME(n)) {
	    if (len != (STRLEN)-1 && c < 256 && ANYOF_BITMAP_TEST(n, c))
		match = TRUE;
	}
	if (!match && do_utf8 && (flags & ANYOF_UNICODE_ALL) && c >= 256)
	    match = TRUE;
	if (!match) {
	    AV *av;
	    SV * const sw = regclass_swash(n, TRUE, 0, (SV**)&av);
	
	    if (sw) {
		if (swash_fetch(sw, p, do_utf8))
		    match = TRUE;
		else if (flags & ANYOF_FOLD) {
		    if (!match && lenp && av) {
		        I32 i;
			for (i = 0; i <= av_len(av); i++) {
			    SV* const sv = *av_fetch(av, i, FALSE);
			    STRLEN len;
			    const char * const s = SvPV_const(sv, len);
			
			    if (len <= plen && memEQ(s, (char*)p, len)) {
			        *lenp = len;
				match = TRUE;
				break;
			    }
			}
		    }
		    if (!match) {
		        U8 tmpbuf[UTF8_MAXBYTES_CASE+1];
			STRLEN tmplen;

		        to_utf8_fold(p, tmpbuf, &tmplen);
			if (swash_fetch(sw, tmpbuf, do_utf8))
			    match = TRUE;
		    }
		}
	    }
	}
	if (match && lenp && *lenp == 0)
	    *lenp = UNISKIP(NATIVE_TO_UNI(c));
    }
    if (!match && c < 256) {
	if (ANYOF_BITMAP_TEST(n, c))
	    match = TRUE;
	else if (flags & ANYOF_FOLD) {
	    U8 f;

	    if (flags & ANYOF_LOCALE) {
		PL_reg_flags |= RF_tainted;
		f = PL_fold_locale[c];
	    }
	    else
		f = PL_fold[c];
	    if (f != c && ANYOF_BITMAP_TEST(n, f))
		match = TRUE;
	}
	
	if (!match && (flags & ANYOF_CLASS)) {
	    PL_reg_flags |= RF_tainted;
	    if (
		(ANYOF_CLASS_TEST(n, ANYOF_ALNUM)   &&  isALNUM_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NALNUM)  && !isALNUM_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_SPACE)   &&  isSPACE_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NSPACE)  && !isSPACE_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_DIGIT)   &&  isDIGIT_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NDIGIT)  && !isDIGIT_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_ALNUMC)  &&  isALNUMC_LC(c)) ||
		(ANYOF_CLASS_TEST(n, ANYOF_NALNUMC) && !isALNUMC_LC(c)) ||
		(ANYOF_CLASS_TEST(n, ANYOF_ALPHA)   &&  isALPHA_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NALPHA)  && !isALPHA_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_ASCII)   &&  isASCII(c))     ||
		(ANYOF_CLASS_TEST(n, ANYOF_NASCII)  && !isASCII(c))     ||
		(ANYOF_CLASS_TEST(n, ANYOF_CNTRL)   &&  isCNTRL_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NCNTRL)  && !isCNTRL_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_GRAPH)   &&  isGRAPH_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NGRAPH)  && !isGRAPH_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_LOWER)   &&  isLOWER_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NLOWER)  && !isLOWER_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_PRINT)   &&  isPRINT_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NPRINT)  && !isPRINT_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_PUNCT)   &&  isPUNCT_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NPUNCT)  && !isPUNCT_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_UPPER)   &&  isUPPER_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_NUPPER)  && !isUPPER_LC(c))  ||
		(ANYOF_CLASS_TEST(n, ANYOF_XDIGIT)  &&  isXDIGIT(c))    ||
		(ANYOF_CLASS_TEST(n, ANYOF_NXDIGIT) && !isXDIGIT(c))    ||
		(ANYOF_CLASS_TEST(n, ANYOF_PSXSPC)  &&  isPSXSPC(c))    ||
		(ANYOF_CLASS_TEST(n, ANYOF_NPSXSPC) && !isPSXSPC(c))    ||
		(ANYOF_CLASS_TEST(n, ANYOF_BLANK)   &&  isBLANK(c))     ||
		(ANYOF_CLASS_TEST(n, ANYOF_NBLANK)  && !isBLANK(c))
		) /* How's that for a conditional? */
	    {
		match = TRUE;
	    }
	}
    }

    return (flags & ANYOF_INVERT) ? !match : match;
}

STATIC U8 *
S_reghop(pTHX_ U8 *s, I32 off)
{
    dVAR;
    return S_reghop3(s, off, (U8*)(off >= 0 ? PL_regeol : PL_bostr));
}

STATIC U8 *
S_reghop3(U8 *s, I32 off, U8* lim)
{
    dVAR;
    if (off >= 0) {
	while (off-- && s < lim) {
	    /* XXX could check well-formedness here */
	    s += UTF8SKIP(s);
	}
    }
    else {
	while (off++) {
	    if (s > lim) {
		s--;
		if (UTF8_IS_CONTINUED(*s)) {
		    while (s > (U8*)lim && UTF8_IS_CONTINUATION(*s))
			s--;
		}
		/* XXX could check well-formedness here */
	    }
	}
    }
    return s;
}

STATIC U8 *
S_reghopmaybe(pTHX_ U8 *s, I32 off)
{
    dVAR;
    return S_reghopmaybe3(s, off, (U8*)(off >= 0 ? PL_regeol : PL_bostr));
}

STATIC U8 *
S_reghopmaybe3(U8* s, I32 off, U8* lim)
{
    dVAR;
    if (off >= 0) {
	while (off-- && s < lim) {
	    /* XXX could check well-formedness here */
	    s += UTF8SKIP(s);
	}
	if (off >= 0)
	    return 0;
    }
    else {
	while (off++) {
	    if (s > lim) {
		s--;
		if (UTF8_IS_CONTINUED(*s)) {
		    while (s > (U8*)lim && UTF8_IS_CONTINUATION(*s))
			s--;
		}
		/* XXX could check well-formedness here */
	    }
	    else
		break;
	}
	if (off <= 0)
	    return 0;
    }
    return s;
}

static void
restore_pos(pTHX_ void *arg)
{
    dVAR;
    PERL_UNUSED_ARG(arg);
    if (PL_reg_eval_set) {
	if (PL_reg_oldsaved) {
	    PL_reg_re->subbeg = PL_reg_oldsaved;
	    PL_reg_re->sublen = PL_reg_oldsavedlen;
#ifdef PERL_OLD_COPY_ON_WRITE
	    PL_reg_re->saved_copy = PL_nrs;
#endif
	    RX_MATCH_COPIED_on(PL_reg_re);
	}
	PL_reg_magic->mg_len = PL_reg_oldpos;
	PL_reg_eval_set = 0;
	PL_curpm = PL_reg_oldcurpm;
    }	
}

STATIC void
S_to_utf8_substr(pTHX_ register regexp *prog)
{
    if (prog->float_substr && !prog->float_utf8) {
	SV* sv;
	prog->float_utf8 = sv = newSVsv(prog->float_substr);
	sv_utf8_upgrade(sv);
	if (SvTAIL(prog->float_substr))
	    SvTAIL_on(sv);
	if (prog->float_substr == prog->check_substr)
	    prog->check_utf8 = sv;
    }
    if (prog->anchored_substr && !prog->anchored_utf8) {
	SV* sv;
	prog->anchored_utf8 = sv = newSVsv(prog->anchored_substr);
	sv_utf8_upgrade(sv);
	if (SvTAIL(prog->anchored_substr))
	    SvTAIL_on(sv);
	if (prog->anchored_substr == prog->check_substr)
	    prog->check_utf8 = sv;
    }
}

STATIC void
S_to_byte_substr(pTHX_ register regexp *prog)
{
    dVAR;
    if (prog->float_utf8 && !prog->float_substr) {
	SV* sv;
	prog->float_substr = sv = newSVsv(prog->float_utf8);
	if (sv_utf8_downgrade(sv, TRUE)) {
	    if (SvTAIL(prog->float_utf8))
		SvTAIL_on(sv);
	} else {
	    SvREFCNT_dec(sv);
	    prog->float_substr = sv = &PL_sv_undef;
	}
	if (prog->float_utf8 == prog->check_utf8)
	    prog->check_substr = sv;
    }
    if (prog->anchored_utf8 && !prog->anchored_substr) {
	SV* sv;
	prog->anchored_substr = sv = newSVsv(prog->anchored_utf8);
	if (sv_utf8_downgrade(sv, TRUE)) {
	    if (SvTAIL(prog->anchored_utf8))
		SvTAIL_on(sv);
	} else {
	    SvREFCNT_dec(sv);
	    prog->anchored_substr = sv = &PL_sv_undef;
	}
	if (prog->anchored_utf8 == prog->check_utf8)
	    prog->check_substr = sv;
    }
}

/*
 * Local variables:
 * c-indentation-style: bsd
 * c-basic-offset: 4
 * indent-tabs-mode: t
 * End:
 *
 * ex: set ts=8 sts=4 sw=4 noet:
 */
