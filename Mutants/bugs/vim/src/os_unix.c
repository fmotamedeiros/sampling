/* vi:set ts=8 sts=4 sw=4:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *	      OS/2 port by Paul Slootman
 *	      VMS merge by Zoltan Arpadffy
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

/*
 * os_unix.c -- code for all flavors of Unix (BSD, SYSV, SVR4, POSIX, ...)
 *	     Also for OS/2, using the excellent EMX package!!!
 *	     Also for BeOS and Atari MiNT.
 *
 * A lot of this file was originally written by Juergen Weigert and later
 * changed beyond recognition.
 */

/*
 * Some systems have a prototype for select() that has (int *) instead of
 * (fd_set *), which is wrong. This define removes that prototype. We define
 * our own prototype below.
 */
#define select select_declared_wrong

#include "vim.h"

#ifdef HAVE_FCNTL_H
# include <fcntl.h>
#endif

#include "os_unixx.h"	    /* unix includes for os_unix.c only */

/*
 * Use this prototype for select, some include files have a wrong prototype
 */
#undef select
#ifdef __BEOS__
# define select	beos_select
#endif

#if defined(HAVE_SELECT)
extern int   select __ARGS((int, fd_set *, fd_set *, fd_set *, struct timeval *));
#endif

#ifdef FEAT_MOUSE_GPM
# include <gpm.h>
/* <linux/keyboard.h> contains defines conflicting with "keymap.h",
 * I just copied relevant defines here. A cleaner solution would be to put gpm
 * code into separate file and include there linux/keyboard.h
 */
/* #include <linux/keyboard.h> */
# define KG_SHIFT	0
# define KG_CTRL	2
# define KG_ALT		3
# define KG_ALTGR	1
# define KG_SHIFTL	4
# define KG_SHIFTR	5
# define KG_CTRLL	6
# define KG_CTRLR	7
# define KG_CAPSSHIFT	8

static void gpm_close __ARGS((void));
static int gpm_open __ARGS((void));
static int mch_gpm_process __ARGS((void));
#endif

/*
 * end of autoconf section. To be extended...
 */

/* Are the following #ifdefs still required? And why? Is that for X11? */

#if defined(ESIX) || defined(M_UNIX) && !defined(SCO)
# ifdef SIGWINCH
#  undef SIGWINCH
# endif
# ifdef TIOCGWINSZ
#  undef TIOCGWINSZ
# endif
#endif

#if defined(SIGWINDOW) && !defined(SIGWINCH)	/* hpux 9.01 has it */
# define SIGWINCH SIGWINDOW
#endif

#ifdef FEAT_X11
# include <X11/Xlib.h>
# include <X11/Xutil.h>
# include <X11/Xatom.h>
# ifdef FEAT_XCLIPBOARD
#  include <X11/Intrinsic.h>
#  include <X11/Shell.h>
#  include <X11/StringDefs.h>
static Widget	xterm_Shell = (Widget)0;
static void xterm_update __ARGS((void));
# endif

# if defined(FEAT_XCLIPBOARD) || defined(FEAT_TITLE)
Window	    x11_window = 0;
# endif
Display	    *x11_display = NULL;

# ifdef FEAT_TITLE
static int  get_x11_windis __ARGS((void));
static void set_x11_title __ARGS((char_u *));
static void set_x11_icon __ARGS((char_u *));
# endif
#endif

#ifdef FEAT_TITLE
static int get_x11_title __ARGS((int));
static int get_x11_icon __ARGS((int));

static char_u	*oldtitle = NULL;
static int	did_set_title = FALSE;
static char_u	*oldicon = NULL;
static int	did_set_icon = FALSE;
#endif

static void may_core_dump __ARGS((void));

static int  WaitForChar __ARGS((long));
#if defined(__BEOS__) || defined(VMS)
int  RealWaitForChar __ARGS((int, long, int *));
#else
static int  RealWaitForChar __ARGS((int, long, int *));
#endif

#ifdef FEAT_XCLIPBOARD
static int do_xterm_trace __ARGS((void));
#define XT_TRACE_DELAY	50	/* delay for xterm tracing */
#endif

static void handle_resize __ARGS((void));

#if defined(SIGWINCH)
static RETSIGTYPE sig_winch __ARGS(SIGPROTOARG);
#endif
#if defined(SIGINT)
static RETSIGTYPE catch_sigint __ARGS(SIGPROTOARG);
#endif
#if defined(SIGPWR)
static RETSIGTYPE catch_sigpwr __ARGS(SIGPROTOARG);
#endif
#if defined(SIGALRM) && defined(FEAT_X11) \
	&& defined(FEAT_TITLE) && !defined(FEAT_GUI_GTK)
# define SET_SIG_ALARM
static RETSIGTYPE sig_alarm __ARGS(SIGPROTOARG);
static int sig_alarm_called;
#endif
static RETSIGTYPE deathtrap __ARGS(SIGPROTOARG);

static void set_signals __ARGS((void));
static void catch_signals __ARGS((RETSIGTYPE (*func_deadly)(), RETSIGTYPE (*func_other)()));
#ifndef __EMX__
static int  have_wildcard __ARGS((int, char_u **));
static int  have_dollars __ARGS((int, char_u **));
#endif

#ifndef NO_EXPANDPATH
static int	pstrcmp __ARGS((const void *, const void *));
static int	unix_expandpath __ARGS((garray_T *gap, char_u *path, int wildoff, int flags));
#endif

#ifndef __EMX__
static int save_patterns __ARGS((int num_pat, char_u **pat, int *num_file, char_u ***file));
#endif

#ifndef SIG_ERR
# define SIG_ERR	((RETSIGTYPE (*)())-1)
#endif

static int	do_resize = FALSE;
#ifndef __EMX__
static char_u	*extra_shell_arg = NULL;
static int	show_shell_mess = TRUE;
#endif
static int	deadly_signal = 0;	    /* The signal we caught */

static int curr_tmode = TMODE_COOK;	/* contains current terminal mode */

#ifdef SYS_SIGLIST_DECLARED
/*
 * I have seen
 *  extern char *_sys_siglist[NSIG];
 * on Irix, Linux, NetBSD and Solaris. It contains a nice list of strings
 * that describe the signals. That is nearly what we want here.  But
 * autoconf does only check for sys_siglist (without the underscore), I
 * do not want to change everything today.... jw.
 * This is why AC_DECL_SYS_SIGLIST is commented out in configure.in
 */
#endif

static struct signalinfo
{
    int	    sig;	/* Signal number, eg. SIGSEGV etc */
    char    *name;	/* Signal name (not char_u!). */
    char    deadly;	/* Catch as a deadly signal? */
} signal_info[] =
{
#ifdef SIGHUP
    {SIGHUP,	    "HUP",	TRUE},
#endif
#ifdef SIGQUIT
    {SIGQUIT,	    "QUIT",	TRUE},
#endif
#ifdef SIGILL
    {SIGILL,	    "ILL",	TRUE},
#endif
#ifdef SIGTRAP
    {SIGTRAP,	    "TRAP",	TRUE},
#endif
#ifdef SIGABRT
    {SIGABRT,	    "ABRT",	TRUE},
#endif
#ifdef SIGEMT
    {SIGEMT,	    "EMT",	TRUE},
#endif
#ifdef SIGFPE
    {SIGFPE,	    "FPE",	TRUE},
#endif
#ifdef SIGBUS
    {SIGBUS,	    "BUS",	TRUE},
#endif
#ifdef SIGSEGV
    {SIGSEGV,	    "SEGV",	TRUE},
#endif
#ifdef SIGSYS
    {SIGSYS,	    "SYS",	TRUE},
#endif
#ifdef SIGALRM
    {SIGALRM,	    "ALRM",	FALSE},	/* Perl's alarm() can trigger it */
#endif
#ifdef SIGTERM
    {SIGTERM,	    "TERM",	TRUE},
#endif
#ifdef SIGVTALRM
    {SIGVTALRM,	    "VTALRM",	TRUE},
#endif
#ifdef SIGPROF
    {SIGPROF,	    "PROF",	TRUE},
#endif
#ifdef SIGXCPU
    {SIGXCPU,	    "XCPU",	TRUE},
#endif
#ifdef SIGXFSZ
    {SIGXFSZ,	    "XFSZ",	TRUE},
#endif
#ifdef SIGUSR1
    {SIGUSR1,	    "USR1",	TRUE},
#endif
#ifdef SIGUSR2
    {SIGUSR2,	    "USR2",	TRUE},
#endif
#ifdef SIGINT
    {SIGINT,	    "INT",	FALSE},
#endif
#ifdef SIGWINCH
    {SIGWINCH,	    "WINCH",	FALSE},
#endif
#ifdef SIGTSTP
    {SIGTSTP,	    "TSTP",	FALSE},
#endif
#ifdef SIGPIPE
    {SIGPIPE,	    "PIPE",	FALSE},
#endif
    {-1,	    "Unknown!", FALSE}
};

    void
mch_write(s, len)
    char_u	*s;
    int		len;
{
    write(1, (char *)s, len);
    if (p_wd)		/* Unix is too fast, slow down a bit more */
	RealWaitForChar(read_cmd_fd, p_wd, NULL);
}

#ifndef VMS

/*
 * mch_inchar(): low level input funcion.
 * Get a characters from the keyboard.
 * Return the number of characters that are available.
 * If wtime == 0 do not wait for characters.
 * If wtime == n wait a short time for characters.
 * If wtime == -1 wait forever for characters.
 */
    int
mch_inchar(buf, maxlen, wtime)
    char_u	*buf;
    int		maxlen;
    long	wtime;	    /* don't use "time", MIPS cannot handle it */
{
    int		len;
#ifdef FEAT_AUTOCMD
    static int	once_already = 0;
#endif

    /* Check if window changed size while we were busy, perhaps the ":set
     * columns=99" command was used. */
    while (do_resize)
	handle_resize();

    if (wtime >= 0)
    {
	while (WaitForChar(wtime) == 0)		/* no character available */
	{
	    if (!do_resize)	/* return if not interrupted by resize */
	    {
#ifdef FEAT_AUTOCMD
		once_already = 0;
#endif
		return 0;
	    }
	    handle_resize();
	}
    }
    else	/* wtime == -1 */
    {
#ifdef FEAT_AUTOCMD
	if (once_already == 2)
	    updatescript(0);
	else if (once_already == 1)
	{
	    setcursor();
	    once_already = 2;
	    return 0;
	}
	else
#endif
	/*
	 * If there is no character available within 'updatetime' seconds
	 * flush all the swap files to disk
	 * Also done when interrupted by SIGWINCH.
	 */
	if (WaitForChar(p_ut) == 0)
	{
#ifdef FEAT_AUTOCMD
	    if (has_cursorhold() && get_real_state() == NORMAL_BUSY)
	    {
		apply_autocmds(EVENT_CURSORHOLD, NULL, NULL, FALSE, curbuf);
		update_screen(VALID);
		once_already = 1;
		return 0;
	    }
	    else
#endif
		updatescript(0);
	}
    }

    for (;;)	/* repeat until we got a character */
    {
	while (do_resize)    /* window changed size */
	    handle_resize();
	/*
	 * we want to be interrupted by the winch signal
	 */
	WaitForChar(-1L);
	if (do_resize)	    /* interrupted by SIGWINCH signal */
	    continue;

	/*
	 * For some terminals we only get one character at a time.
	 * We want the get all available characters, so we could keep on
	 * trying until none is available
	 * For some other terminals this is quite slow, that's why we don't do
	 * it.
	 */
	len = read_from_input_buf(buf, (long)maxlen);
	if (len > 0)
	{
#ifdef OS2
	    int i;

	    for (i = 0; i < len; i++)
		if (buf[i] == 0)
		    buf[i] = K_NUL;
#endif
#ifdef FEAT_AUTOCMD
	    once_already = 0;
#endif
	    return len;
	}
    }
}

#endif /* VMS */

    static void
handle_resize()
{
    do_resize = FALSE;
    shell_resized();
}

/*
 * return non-zero if a character is available
 */
    int
mch_char_avail()
{
    return WaitForChar(0L);
}

#if defined(HAVE_TOTAL_MEM) || defined(PROTO)
# ifdef HAVE_SYS_RESOURCE_H
#  include <sys/resource.h>
# endif

/*
 * Return total amount of memory available.  Doesn't change when memory has
 * been allocated.
 */
/* ARGSUSED */
    long_u
mch_total_mem(special)
    int special;
{
# ifdef __EMX__
    return ulimit(3, 0L);   /* always 32MB? */
# else
    struct rlimit	rlp;

    if (getrlimit(RLIMIT_DATA, &rlp) == 0
#  ifdef RLIM_INFINITY
	    && rlp.rlim_cur != RLIM_INFINITY
#  endif
	    )
	return (long_u)rlp.rlim_cur;
    return (long_u)0x7fffffff;
# endif
}
#endif

    void
mch_delay(msec, ignoreinput)
    long	msec;
    int		ignoreinput;
{
    int		old_tmode;

    if (ignoreinput)
    {
	/* Go to cooked mode without echo, to allow SIGINT interrupting us
	 * here */
	old_tmode = curr_tmode;
	settmode(TMODE_SLEEP);

	/*
	 * Everybody sleeps in a different way...
	 * Prefer nanosleep(), some versions of usleep() can only sleep up to
	 * one second.
	 */
#ifdef HAVE_NANOSLEEP
	{
	    struct timespec ts;

	    ts.tv_sec = msec / 1000;
	    ts.tv_nsec = (msec % 1000) * 1000000;
	    (void)nanosleep(&ts, NULL);
	}
#else
# ifdef HAVE_USLEEP
	while (msec >= 1000)
	{
	    usleep((unsigned int)(999 * 1000));
	    msec -= 999;
	}
	usleep((unsigned int)(msec * 1000));
# else
#  ifndef HAVE_SELECT
	poll(NULL, 0, (int)msec);
#  else
#   ifdef __EMX__
	_sleep2(msec);
#   else
	{
	    struct timeval tv;

	    tv.tv_sec = msec / 1000;
	    tv.tv_usec = (msec % 1000) * 1000;
	    /*
	     * NOTE: Solaris 2.6 has a bug that makes select() hang here.  Get
	     * a patch from Sun to fix this.  Reported by Gunnar Pedersen.
	     */
	    select(0, NULL, NULL, NULL, &tv);
	}
#   endif /* __EMX__ */
#  endif /* HAVE_SELECT */
# endif /* HAVE_NANOSLEEP */
#endif /* HAVE_USLEEP */

	settmode(old_tmode);
    }
    else
	WaitForChar(msec);
}

#if defined(HAVE_GETRLIMIT) \
	|| (!defined(HAVE_SIGALTSTACK) && defined(HAVE_SIGSTACK))
# define HAVE_CHECK_STACK_GROWTH
/*
 * Support for checking for an almost-out-of-stack-space situation.
 */

/*
 * Return a pointer to an item on the stack.  Used to find out if the stack
 * grows up or down.
 */
static void check_stack_growth __ARGS((char *p));
static int stack_grows_downwards;

/*
 * Find out if the stack grows upwards or downwards.
 * "p" points to a variable on the stack of the caller.
 */
    static void
check_stack_growth(p)
    char	*p;
{
    int		i;

    stack_grows_downwards = (p > (char *)&i);
}
#endif

#if defined(HAVE_GETRLIMIT) || defined(PROTO)
static char *stack_limit = NULL;

/*
 * Find out until how var the stack can grow without getting into trouble.
 * Called when starting up and when switching to the signal stack in
 * deathtrap().
 */
    static void
get_stack_limit()
{
    struct rlimit	rlp;
    int			i;

    /* Set the stack limit to 15/16 of the allowable size. */
    if (getrlimit(RLIMIT_STACK, &rlp) == 0
#  ifdef RLIM_INFINITY
	    && rlp.rlim_cur != RLIM_INFINITY
#  endif
       )
    {
	if (stack_grows_downwards)
	    stack_limit = (char *)((long)&i - ((long)rlp.rlim_cur / 16L * 15L));
	else
	    stack_limit = (char *)((long)&i + ((long)rlp.rlim_cur / 16L * 15L));
    }
}

/*
 * Return FAIL when running out of stack space.
 * "p" must point to any variable local to the caller that's on the stack.
 */
    int
mch_stackcheck(p)
    char	*p;
{
    if (stack_limit != NULL)
    {
	if (stack_grows_downwards)
	{
	    if (p < stack_limit)
		return FAIL;
	}
	else if (p > stack_limit)
	    return FAIL;
    }
    return OK;
}
#endif

#if defined(HAVE_SIGALTSTACK) || defined(HAVE_SIGSTACK)
/*
 * Support for using the signal stack.
 * This helps when we run out of stack space, which causes a SIGSEGV.  The
 * signal handler then must run on another stack, since the normal stack is
 * completely full.
 */

#ifndef SIGSTKSZ
# define SIGSTKSZ 8000    /* just a guess of how much stack is needed... */
#endif

# ifdef HAVE_SIGALTSTACK
static stack_t sigstk;			/* for sigaltstack() */
# else
static struct sigstack sigstk;		/* for sigstack() */
# endif

static void init_signal_stack __ARGS((void));
static char *signal_stack;

    static void
init_signal_stack()
{
    if (signal_stack != NULL)
    {
# ifdef HAVE_SIGALTSTACK
	sigstk.ss_sp = signal_stack;
	sigstk.ss_size = SIGSTKSZ;
	sigstk.ss_flags = 0;
	(void)sigaltstack(&sigstk, NULL);
# else
	sigstk.ss_sp = signal_stack;
	if (stack_grows_downwards)
	    sigstk.ss_sp += SIGSTKSZ - 1;
	sigstk.ss_onstack = 0;
	(void)sigstack(&sigstk, NULL);
# endif
    }
}
#endif

/*
 * We need correct potatotypes for a signal function, otherwise mean compilers
 * will barf when the second argument to signal() is ``wrong''.
 * Let me try it with a few tricky defines from my own osdef.h	(jw).
 */
#if defined(SIGWINCH)
/* ARGSUSED */
    static RETSIGTYPE
sig_winch SIGDEFARG(sigarg)
{
    /* this is not required on all systems, but it doesn't hurt anybody */
    signal(SIGWINCH, (RETSIGTYPE (*)())sig_winch);
    do_resize = TRUE;
    SIGRETURN;
}
#endif

#if defined(SIGINT)
/* ARGSUSED */
    static RETSIGTYPE
catch_sigint SIGDEFARG(sigarg)
{
    /* this is not required on all systems, but it doesn't hurt anybody */
    signal(SIGINT, (RETSIGTYPE (*)())catch_sigint);
    got_int = TRUE;
    SIGRETURN;
}
#endif

#if defined(SIGPWR)
/* ARGSUSED */
    static RETSIGTYPE
catch_sigpwr SIGDEFARG(sigarg)
{
    /*
     * I'm not sure we get the SIGPWR signal when the system is really going
     * down or when the batteries are almost empty.  Just preserve the swap
     * files and don't exit, that can't do any harm.
     */
    ml_sync_all(FALSE, FALSE);
    SIGRETURN;
}
#endif

#ifdef SET_SIG_ALARM
/*
 * signal function for alarm().
 */
/* ARGSUSED */
    static RETSIGTYPE
sig_alarm SIGDEFARG(sigarg)
{
    /* doesn't do anything, just to break a system call */
    sig_alarm_called = TRUE;
    SIGRETURN;
}
#endif

#if defined(HAVE_SETJMP_H) || defined(PROTO)
/*
 * A simplistic version of setjmp() that only allows one level of using.
 * Don't call twice before calling mch_endjmp()!.
 * Usage:
 *	mch_startjmp()
 *	if (SETJMP(lc_jump_env) != 0)
 *	{
 *	    mch_didjmp();
 *	    EMSG("crash!");
 *	}
 *	else
 *	{
 *	    do_the_work;
 *	    mch_endjmp();
 *	}
 * Note: Can't move SETJMP() here, because a function calling setjmp() must
 * not return before the saved environment is used.
 * Returns OK for normal return, FAIL when the protected code caused a
 * problem and LONGJMP() was used.
 */
    void
mch_startjmp()
{
#ifdef SIGHASARG
    lc_signal = 0;
#endif
    lc_active = TRUE;
}

    void
mch_endjmp()
{
    lc_active = FALSE;
}

    void
mch_didjmp()
{
# if defined(HAVE_SIGALTSTACK) || defined(HAVE_SIGSTACK)
    /* On FreeBSD the signal stack has to be reset after using siglongjmp(),
     * otherwise catching the signal only works once. */
    init_signal_stack();
# endif
}
#endif

/*
 * This function handles deadly signals.
 * It tries to preserve any swap file and exit properly.
 * (partly from Elvis).
 */
    static RETSIGTYPE
deathtrap SIGDEFARG(sigarg)
{
    static int	entered = 0;	    /* count the number of times we got here.
				       Note: when memory has been corrupted
				       this may get an arbitrary value! */
#ifdef SIGHASARG
    int		i;
#endif

#if defined(HAVE_SETJMP_H)
    /*
     * Catch a crash in protected code.
     * Restores the environment saved in lc_jump_env, which looks like
     * SETJMP() returns 1.
     */
    if (lc_active)
    {
# if defined(SIGHASARG)
	lc_signal = sigarg;
# endif
	lc_active = FALSE;	/* don't jump again */
	LONGJMP(lc_jump_env, 1);
	/* NOTREACHED */
    }
#endif

    /* Remember how often we have been called. */
    ++entered;

#ifdef HAVE_GETRLIMIT
    /* Since we are now using the signal stack, need to reset the stack
     * limit.  Otherwise using a regexp will fail. */
    get_stack_limit();
#endif

#ifdef SIGHASARG
    /* try to find the name of this signal */
    for (i = 0; signal_info[i].sig != -1; i++)
	if (sigarg == signal_info[i].sig)
	    break;
    deadly_signal = sigarg;
#endif

    full_screen = FALSE;	/* don't write message to the GUI, it might be
				 * part of the problem... */
    /*
     * If something goes wrong after entering here, we may get here again.
     * When this happens, give a message and try to exit nicely (resetting the
     * terminal mode, etc.)
     * When this happens twice, just exit, don't even try to give a message,
     * stack may be corrupt or something weird.
     * When this still happens again (or memory was corrupted in such a way
     * that "entered" was clobbered) use _exit(), don't try freeing resources.
     */
    if (entered >= 3)
    {
	reset_signals();	/* don't catch any signals anymore */
	may_core_dump();
	if (entered >= 4)
	    _exit(8);
	exit(7);
    }
    if (entered == 2)
    {
	OUT_STR(_("Vim: Double signal, exiting\n"));
	out_flush();
	getout(1);
    }

#ifdef SIGHASARG
    sprintf((char *)IObuff, _("Vim: Caught deadly signal %s\n"),
							 signal_info[i].name);
#else
    sprintf((char *)IObuff, _("Vim: Caught deadly signal\n"));
#endif
    preserve_exit();		    /* preserve files and exit */

    SIGRETURN;
}

#ifdef _REENTRANT
/*
 * On Solaris with multi-threading, suspending might not work immediately.
 * Catch the SIGCONT signal, which will be used as an indication whether the
 * suspending has been done or not.
 */
static int sigcont_received;
static RETSIGTYPE sigcont_handler __ARGS(SIGPROTOARG);

/*
 * signal handler for SIGCONT
 */
/* ARGSUSED */
    static RETSIGTYPE
sigcont_handler SIGDEFARG(sigarg)
{
    sigcont_received = TRUE;
    SIGRETURN;
}
#endif

/*
 * If the machine has job control, use it to suspend the program,
 * otherwise fake it by starting a new shell.
 */
    void
mch_suspend()
{
    /* BeOS does have SIGTSTP, but it doesn't work. */
#if defined(SIGTSTP) && !defined(__BEOS__)
    out_flush();	    /* needed to make cursor visible on some systems */
    settmode(TMODE_COOK);
    out_flush();	    /* needed to disable mouse on some systems */

# if defined(FEAT_CLIPBOARD) && defined(FEAT_X11)
    /* Since we are going to sleep, we can't respond to requests for the X
     * selections.  Lose them, otherwise other applications will hang.  But
     * first copy the text to cut buffer 0. */
    if (clip_star.owned || clip_plus.owned)
    {
	x11_export_final_selection();
	if (clip_star.owned)
	    clip_lose_selection(&clip_star);
	if (clip_plus.owned)
	    clip_lose_selection(&clip_plus);
	if (x11_display != NULL)
	    XFlush(x11_display);
    }
# endif

# ifdef _REENTRANT
    sigcont_received = FALSE;
# endif
    kill(0, SIGTSTP);	    /* send ourselves a STOP signal */
# ifdef _REENTRANT
    /* When we didn't suspend immediately in the kill(), do it now.  Happens
     * on multi-threaded Solaris. */
    if (!sigcont_received)
	pause();
# endif

# ifdef FEAT_TITLE
    /*
     * Set oldtitle to NULL, so the current title is obtained again.
     */
    vim_free(oldtitle);
    oldtitle = NULL;
# endif
    settmode(TMODE_RAW);
    need_check_timestamps = TRUE;
    did_check_timestamps = FALSE;
#else
    suspend_shell();
#endif
}

    void
mch_init()
{
    Columns = 80;
    Rows = 24;

    out_flush();
    set_signals();
}

    static void
set_signals()
{
#if defined(SIGWINCH)
    /*
     * WINDOW CHANGE signal is handled with sig_winch().
     */
    signal(SIGWINCH, (RETSIGTYPE (*)())sig_winch);
#endif

    /*
     * We want the STOP signal to work, to make mch_suspend() work.
     * For "rvim" the STOP signal is ignored.
     */
#ifdef SIGTSTP
    signal(SIGTSTP, restricted ? SIG_IGN : SIG_DFL);
#endif
#ifdef _REENTRANT
    signal(SIGCONT, sigcont_handler);
#endif

    /*
     * We want to ignore breaking of PIPEs.
     */
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    /*
     * We want to catch CTRL-C (only works while in Cooked mode).
     */
#ifdef SIGINT
    signal(SIGINT, (RETSIGTYPE (*)())catch_sigint);
#endif

    /*
     * Ignore alarm signals (Perl's alarm() generates it).
     */
#ifdef SIGALRM
    signal(SIGALRM, SIG_IGN);
#endif

    /*
     * Catch SIGPWR (power failure?) to preserve the swap files, so that no
     * work will be lost.
     */
#ifdef SIGPWR
    signal(SIGPWR, (RETSIGTYPE (*)())catch_sigpwr);
#endif

    /*
     * Arrange for other signals to gracefully shutdown Vim.
     */
    catch_signals(deathtrap, SIG_ERR);

#if defined(FEAT_GUI) && defined(SIGHUP)
    /*
     * When the GUI is running, ignore the hangup signal.
     */
    if (gui.in_use)
	signal(SIGHUP, SIG_IGN);
#endif
}

    void
reset_signals()
{
    catch_signals(SIG_DFL, SIG_DFL);
#ifdef _REENTRANT
    /* SIGCONT isn't in the list, because its default action is ignore */
    signal(SIGCONT, SIG_DFL);
#endif
}

    static void
catch_signals(func_deadly, func_other)
    RETSIGTYPE (*func_deadly)();
    RETSIGTYPE (*func_other)();
{
    int	    i;

    for (i = 0; signal_info[i].sig != -1; i++)
	if (signal_info[i].deadly)
	{
#if defined(HAVE_SIGALTSTACK) && defined(HAVE_SIGACTION)
	    struct sigaction sa;

	    /* Setup to use the alternate stack for the signal function. */
	    sa.sa_handler = func_deadly;
	    sigemptyset(&sa.sa_mask);
	    sa.sa_flags = SA_ONSTACK;
	    sigaction(signal_info[i].sig, &sa, NULL);
#else
# if defined(HAVE_SIGALTSTACK) && defined(HAVE_SIGVEC)
	    struct sigvec sv;

	    /* Setup to use the alternate stack for the signal function. */
	    sv.sv_handler = func_deadly;
	    sv.sv_mask = 0;
	    sv.sv_flags = SV_ONSTACK;
	    sigvec(signal_info[i].sig, &sv, NULL);
# else
	    signal(signal_info[i].sig, func_deadly);
# endif
#endif
	}
	else if (func_other != SIG_ERR)
	    signal(signal_info[i].sig, func_other);
}

/*
 * Check_win checks whether we have an interactive stdout.
 */
/* ARGSUSED */
    int
mch_check_win(argc, argv)
    int	    argc;
    char    **argv;
{
#ifdef OS2
    /*
     * Store argv[0], may be used for $VIM.  Only use it if it is an absolute
     * name, mostly it's just "vim" and found in the path, which is unusable.
     */
    if (mch_isFullName(argv[0]))
	exe_name = vim_strsave((char_u *)argv[0]);
#endif
    if (isatty(1))
	return OK;
    return FAIL;
}

/*
 * Return TRUE if the input comes from a terminal, FALSE otherwise.
 */
    int
mch_input_isatty()
{
    if (isatty(read_cmd_fd))
	return TRUE;
    return FALSE;
}

#ifdef FEAT_X11

# if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H) \
	&& (defined(FEAT_XCLIPBOARD) || defined(FEAT_TITLE))

static void xopen_message __ARGS((struct timeval *tvp));

/*
 * Give a message about the elapsed time for opening the X window.
 */
    static void
xopen_message(tvp)
    struct timeval *tvp;	/* must contain start time */
{
    struct timeval  end_tv;

    /* Compute elapsed time. */
    gettimeofday(&end_tv, NULL);
    smsg((char_u *)_("Opening the X display took %ld msec"),
	    (end_tv.tv_sec - tvp->tv_sec) * 1000L
	    + (end_tv.tv_usec - tvp->tv_usec) / 1000L);
}
# endif
#endif

#if defined(FEAT_X11) && (defined(FEAT_TITLE) || defined(FEAT_XCLIPBOARD))
/*
 * A few functions shared by X11 title and clipboard code.
 */
static int x_error_handler __ARGS((Display *dpy, XErrorEvent *error_event));
static int x_error_check __ARGS((Display *dpy, XErrorEvent *error_event));
static int x_connect_to_server __ARGS((void));
static int test_x11_window __ARGS((Display *dpy));

static int	got_x_error = FALSE;

/*
 * X Error handler, otherwise X just exits!  (very rude) -- webb
 */
    static int
x_error_handler(dpy, error_event)
    Display	*dpy;
    XErrorEvent	*error_event;
{
    XGetErrorText(dpy, error_event->error_code, (char *)IObuff, IOSIZE);
    STRCAT(IObuff, _("\nVim: Got X error\n"));

    /* We cannot print a message and continue, because no X calls are allowed
     * here (causes my system to hang).  Silently continuing might be an
     * alternative... */
    preserve_exit();		    /* preserve files and exit */

    return 0;		/* NOTREACHED */
}

/*
 * Another X Error handler, just used to check for errors.
 */
/* ARGSUSED */
    static int
x_error_check(dpy, error_event)
    Display *dpy;
    XErrorEvent	*error_event;
{
    got_x_error = TRUE;
    return 0;
}

/*
 * Return TRUE when connection to the X server is desired.
 */
    static int
x_connect_to_server()
{
    regmatch_T	regmatch;

    if (x_no_connect)
	return FALSE;

    /* Check for a match with "exclude:" from 'clipboard'. */
    if (clip_exclude_prog != NULL)
    {
	regmatch.rm_ic = FALSE;		/* Don't ignore case */
	regmatch.regprog = clip_exclude_prog;
	if (vim_regexec(&regmatch, T_NAME, (colnr_T)0))
	    return FALSE;
    }
    return TRUE;
}

/*
 * Test if "dpy" and x11_window are valid by getting the window title.
 * I don't actually want it yet, so there may be a simpler call to use, but
 * this will cause the error handler x_error_check() to be called if anything
 * is wrong, such as the window pointer being invalid (as can happen when the
 * user changes his DISPLAY, but not his WINDOWID) -- webb
 */
    static int
test_x11_window(dpy)
    Display	*dpy;
{
    int			(*old_handler)();
    XTextProperty	text_prop;

    old_handler = XSetErrorHandler(x_error_check);
    got_x_error = FALSE;
    if (XGetWMName(dpy, x11_window, &text_prop))
	XFree((void *)text_prop.value);
    XSync(dpy, False);
    (void)XSetErrorHandler(old_handler);

    if (p_verbose > 0 && got_x_error)
	MSG(_("Testing the X display failed"));

    return (got_x_error ? FAIL : OK);
}
#endif

#ifdef FEAT_TITLE

#ifdef FEAT_X11

static int get_x11_thing __ARGS((int get_title, int test_only));

/*
 * try to get x11 window and display
 *
 * return FAIL for failure, OK otherwise
 *
 * FIXME:
 * This code is responsible for setting the window manager name of the
 * editor's window. We should provide a special version of this for usage with
 * the GTK+ widget set, which wouldn't use any direct x11 calls. (--mdcki)
 */
    static int
get_x11_windis()
{
    char	    *winid;
    static int	    result = -1;
#define XD_NONE	 0	/* x11_display not set here */
#define XD_HERE	 1	/* x11_display opened here */
#define XD_GUI	 2	/* x11_display used from gui.dpy */
#define XD_XTERM 3	/* x11_display used from xterm_dpy */
    static int	    x11_display_from = XD_NONE;
    static int	    did_set_error_handler = FALSE;

    if (!did_set_error_handler)
    {
	/* X just exits if it finds an error otherwise! */
	(void)XSetErrorHandler(x_error_handler);
	did_set_error_handler = TRUE;
    }

#if defined(FEAT_GUI_X11) || defined(FEAT_GUI_GTK)
    if (gui.in_use)
    {
	/*
	 * If the X11 display was opened here before, for the window where Vim
	 * was started, close that one now to avoid a memory leak.
	 */
	if (x11_display_from == XD_HERE && x11_display != NULL)
	{
	    XCloseDisplay(x11_display);
	    x11_display_from = XD_NONE;
	}
	if (gui_get_x11_windis(&x11_window, &x11_display) == OK)
	{
	    x11_display_from = XD_GUI;
	    return OK;
	}
	x11_display = NULL;
	return FAIL;
    }
    else if (x11_display_from == XD_GUI)
    {
	/* GUI must have stopped somehow, clear x11_display */
	x11_window = 0;
	x11_display = NULL;
	x11_display_from = XD_NONE;
    }
#endif

    /* When started with the "-X" argument, don't try connecting. */
    if (!x_connect_to_server())
	return FAIL;

    /*
     * If WINDOWID not set, should try another method to find out
     * what the current window number is. The only code I know for
     * this is very complicated.
     * We assume that zero is invalid for WINDOWID.
     */
    if (x11_window == 0 && (winid = getenv("WINDOWID")) != NULL)
	x11_window = (Window)atol(winid);

#ifdef FEAT_XCLIPBOARD
    if (xterm_dpy != NULL && x11_window != 0)
    {
	/* Checked it already. */
	if (x11_display_from == XD_XTERM)
	    return OK;

	/*
	 * If the X11 display was opened here before, for the window where Vim
	 * was started, close that one now to avoid a memory leak.
	 */
	if (x11_display_from == XD_HERE && x11_display != NULL)
	    XCloseDisplay(x11_display);
	x11_display = xterm_dpy;
	x11_display_from = XD_XTERM;
	if (test_x11_window(x11_display) == FAIL)
	{
	    /* probably bad $WINDOWID */
	    x11_window = 0;
	    x11_display = NULL;
	    x11_display_from = XD_NONE;
	    return FAIL;
	}
	return OK;
    }
#endif

    if (x11_window == 0 || x11_display == NULL)
	result = -1;

    if (result != -1)	    /* Have already been here and set this */
	return result;	    /* Don't do all these X calls again */

    if (x11_window != 0 && x11_display == NULL)
    {
#ifdef SET_SIG_ALARM
	RETSIGTYPE (*sig_save)();
#endif
#if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
	struct timeval  start_tv;

	if (p_verbose > 0)
	    gettimeofday(&start_tv, NULL);
#endif

#ifdef SET_SIG_ALARM
	/*
	 * Opening the Display may hang if the DISPLAY setting is wrong, or
	 * the network connection is bad.  Set an alarm timer to get out.
	 */
	sig_alarm_called = FALSE;
	sig_save = (RETSIGTYPE (*)())signal(SIGALRM,
						 (RETSIGTYPE (*)())sig_alarm);
	alarm(2);
#endif
	x11_display = XOpenDisplay(NULL);

#ifdef SET_SIG_ALARM
	alarm(0);
	signal(SIGALRM, (RETSIGTYPE (*)())sig_save);
	if (p_verbose > 0 && sig_alarm_called)
	    MSG(_("Opening the X display timed out"));
#endif
	if (x11_display != NULL)
	{
# if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
	    if (p_verbose > 0)
		xopen_message(&start_tv);
# endif
	    if (test_x11_window(x11_display) == FAIL)
	    {
		/* Maybe window id is bad */
		x11_window = 0;
		XCloseDisplay(x11_display);
		x11_display = NULL;
	    }
	    else
		x11_display_from = XD_HERE;
	}
    }
    if (x11_window == 0 || x11_display == NULL)
	return (result = FAIL);
    return (result = OK);
}

/*
 * Determine original x11 Window Title
 */
    static int
get_x11_title(test_only)
    int		test_only;
{
    int		retval;

    retval = get_x11_thing(TRUE, test_only);

    /* could not get old title: oldtitle == NULL */

    return retval;
}

/*
 * Determine original x11 Window icon
 */
    static int
get_x11_icon(test_only)
    int		test_only;
{
    int		retval = FALSE;

    retval = get_x11_thing(FALSE, test_only);

    /* could not get old icon, use terminal name */
    if (oldicon == NULL && !test_only)
    {
	if (STRNCMP(T_NAME, "builtin_", 8) == 0)
	    oldicon = T_NAME + 8;
	else
	    oldicon = T_NAME;
    }

    return retval;
}

    static int
get_x11_thing(get_title, test_only)
    int		get_title;	/* get title string */
    int		test_only;
{
    XTextProperty	text_prop;
    int			retval = FALSE;
    Status		status;

    if (get_x11_windis() == OK)
    {
	/* Get window/icon name if any */
	if (get_title)
	    status = XGetWMName(x11_display, x11_window, &text_prop);
	else
	    status = XGetWMIconName(x11_display, x11_window, &text_prop);

	/*
	 * If terminal is xterm, then x11_window may be a child window of the
	 * outer xterm window that actually contains the window/icon name, so
	 * keep traversing up the tree until a window with a title/icon is
	 * found.
	 */
	if (term_is_xterm)
	{
	    Window	    root;
	    Window	    parent;
	    Window	    win = x11_window;
	    Window	   *children;
	    unsigned int    num_children;

	    while (!status || text_prop.value == NULL)
	    {
		if (!XQueryTree(x11_display, win, &root, &parent, &children,
							       &num_children))
		    break;
		if (children)
		    XFree((void *)children);
		if (parent == root || parent == 0)
		    break;

		win = parent;
		if (get_title)
		    status = XGetWMName(x11_display, win, &text_prop);
		else
		    status = XGetWMIconName(x11_display, win, &text_prop);
	    }
	}
	if (status && text_prop.value != NULL)
	{
	    retval = TRUE;
	    if (!test_only)
	    {
#ifdef FEAT_XFONTSET
		if (text_prop.encoding == XA_STRING)
		{
#endif
		    if (get_title)
			oldtitle = vim_strsave((char_u *)text_prop.value);
		    else
			oldicon = vim_strsave((char_u *)text_prop.value);
#ifdef FEAT_XFONTSET
		}
		else
		{
		    char    **cl;
		    Status  transform_status;
		    int	    n = 0;

		    transform_status = XmbTextPropertyToTextList(x11_display,
								 &text_prop,
								 &cl, &n);
		    if (transform_status >= Success && n > 0 && cl[0])
		    {
			if (get_title)
			    oldtitle = vim_strsave((char_u *) cl[0]);
			else
			    oldicon = vim_strsave((char_u *) cl[0]);
			XFreeStringList(cl);
		    }
		    else
		    {
			if (get_title)
			    oldtitle = vim_strsave((char_u *)text_prop.value);
			else
			    oldicon = vim_strsave((char_u *)text_prop.value);
		    }
		}
#endif
	    }
	    XFree((void *)text_prop.value);
	}
    }
    return retval;
}

/*
 * Set x11 Window Title
 *
 * get_x11_windis() must be called before this and have returned OK
 */
    static void
set_x11_title(title)
    char_u	*title;
{
#if XtSpecificationRelease >= 4
    XTextProperty text_prop;

    text_prop.value = title;
    text_prop.nitems = STRLEN(title);
    text_prop.encoding = XA_STRING;
    text_prop.format = 8;
    XSetWMName(x11_display, x11_window, &text_prop);
#else
    XStoreName(x11_display, x11_window, (char *)title);
#endif
    XFlush(x11_display);
}

/*
 * Set x11 Window icon
 *
 * get_x11_windis() must be called before this and have returned OK
 */
    static void
set_x11_icon(icon)
    char_u	*icon;
{
#if XtSpecificationRelease >= 4
    XTextProperty text_prop;

    text_prop.value = icon;
    text_prop.nitems = STRLEN(icon);
    text_prop.encoding = XA_STRING;
    text_prop.format = 8;
    XSetWMIconName(x11_display, x11_window, &text_prop);
#else
    XSetIconName(x11_display, x11_window, (char *)icon);
#endif
    XFlush(x11_display);
}

#else  /* FEAT_X11 */

/*ARGSUSED*/
    static int
get_x11_title(test_only)
    int	    test_only;
{
    return FALSE;
}

    static int
get_x11_icon(test_only)
    int	    test_only;
{
    if (!test_only)
    {
	if (STRNCMP(T_NAME, "builtin_", 8) == 0)
	    oldicon = T_NAME + 8;
	else
	    oldicon = T_NAME;
    }
    return FALSE;
}

#endif /* FEAT_X11 */

    int
mch_can_restore_title()
{
    return get_x11_title(TRUE);
}

    int
mch_can_restore_icon()
{
    return get_x11_icon(TRUE);
}

/*
 * Set the window title and icon.
 */
    void
mch_settitle(title, icon)
    char_u *title;
    char_u *icon;
{
    int		type = 0;
    static int	recursive = 0;

    if (T_NAME == NULL)	    /* no terminal name (yet) */
	return;
    if (title == NULL && icon == NULL)	    /* nothing to do */
	return;

    /* When one of the X11 functions causes a deadly signal, we get here again
     * recursively.  Avoid hanging then (something is probably locked). */
    if (recursive)
	return;
    ++recursive;

    /*
     * if the window ID and the display is known, we may use X11 calls
     */
#ifdef FEAT_X11
    if (get_x11_windis() == OK)
	type = 1;
#else
# if defined(FEAT_GUI_PHOTON) || defined(FEAT_GUI_MAC)
    if (gui.in_use)
	type = 1;
# endif
# ifdef FEAT_GUI_BEOS
    /* TODO: If this means (gui.in_use) why not merge with above? (Dany) */
    /* we always have a 'window' */
    type = 1;
# endif
#endif

    /*
     * Note: if "t_TS" is set, title is set with escape sequence rather
     *	     than x11 calls, because the x11 calls don't always work
     */

    if ((type || *T_TS != NUL) && title != NULL)
    {
	if (oldtitle == NULL
#ifdef FEAT_GUI
		&& !gui.in_use
#endif
		)		/* first call but not in GUI, save title */
	    (void)get_x11_title(FALSE);

	if (*T_TS != NUL)		/* it's OK if t_fs is empty */
	    term_settitle(title);
#ifdef FEAT_X11
	else
	    set_x11_title(title);		/* x11 */
#else
# if defined(FEAT_GUI_BEOS) || defined(FEAT_GUI_PHOTON) || defined(FEAT_GUI_MAC)
	else
	    gui_mch_settitle(title, icon);
# endif
#endif
	did_set_title = TRUE;
    }

    if ((type || *T_CIS != NUL) && icon != NULL)
    {
	if (oldicon == NULL
#ifdef FEAT_GUI
		&& !gui.in_use
#endif
		)		/* first call, save icon */
	    get_x11_icon(FALSE);

	if (*T_CIS != NUL)
	{
	    out_str(T_CIS);			/* set icon start */
	    out_str_nf(icon);
	    out_str(T_CIE);			/* set icon end */
	    out_flush();
	}
#ifdef FEAT_X11
	else
	    set_x11_icon(icon);			/* x11 */
#endif
	did_set_icon = TRUE;
    }
    --recursive;
}

/*
 * Restore the window/icon title.
 * "which" is one of:
 *  1  only restore title
 *  2  only restore icon
 *  3  restore title and icon
 */
    void
mch_restore_title(which)
    int which;
{
    /* only restore the title or icon when it has been set */
    mch_settitle(((which & 1) && did_set_title) ?
			(oldtitle ? oldtitle : p_titleold) : NULL,
			      ((which & 2) && did_set_icon) ? oldicon : NULL);
}

#endif /* FEAT_TITLE */

/*
 * Return TRUE if "name" looks like some xterm name.
 */
    int
vim_is_xterm(name)
    char_u *name;
{
    if (name == NULL)
	return FALSE;
    return (STRNICMP(name, "xterm", 5) == 0
		|| STRNICMP(name, "nxterm", 6) == 0
		|| STRNICMP(name, "kterm", 5) == 0
		|| STRCMP(name, "builtin_xterm") == 0);
}

#if defined(FEAT_MOUSE_TTY) || defined(PROTO)
/*
 * Return non-zero when using an xterm mouse, according to 'ttymouse'.
 * Return 1 for "xterm".
 * Return 2 for "xterm2".
 */
    int
use_xterm_mouse()
{
    if (ttym_flags == TTYM_XTERM2)
	return 2;
    if (ttym_flags == TTYM_XTERM)
	return 1;
    return 0;
}
#endif

    int
vim_is_iris(name)
    char_u  *name;
{
    if (name == NULL)
	return FALSE;
    return (STRNICMP(name, "iris-ansi", 9) == 0
	    || STRCMP(name, "builtin_iris-ansi") == 0);
}

    int
vim_is_vt300(name)
    char_u  *name;
{
    if (name == NULL)
	return FALSE;	       /* actually all ANSI comp. terminals should be here  */
    return (STRNICMP(name, "vt3", 3) == 0     /* it will cover all from VT100-VT300 */
	    || STRNICMP(name, "vt2", 3) == 0  /* TODO: from VT340 can hanle colors  */
	    || STRNICMP(name, "vt1", 3) == 0
	    || STRCMP(name, "builtin_vt320") == 0);
}

/*
 * Return TRUE if "name" is a terminal for which 'ttyfast' should be set.
 * This should include all windowed terminal emulators.
 */
    int
vim_is_fastterm(name)
    char_u  *name;
{
    if (name == NULL)
	return FALSE;
    if (vim_is_xterm(name) || vim_is_vt300(name) || vim_is_iris(name))
	return TRUE;
    return (   STRNICMP(name, "hpterm", 6) == 0
	    || STRNICMP(name, "sun-cmd", 7) == 0
	    || STRNICMP(name, "screen", 6) == 0
	    || STRNICMP(name, "rxvt", 4) == 0
	    || STRNICMP(name, "dtterm", 6) == 0);
}

/*
 * Insert user name in s[len].
 * Return OK if a name found.
 */
    int
mch_get_user_name(s, len)
    char_u  *s;
    int	    len;
{
#ifdef VMS
    STRNCPY((char *)s, cuserid(NULL), len);
    return OK;
#else
    return mch_get_uname(getuid(), s, len);
#endif
}

/*
 * Insert user name for "uid" in s[len].
 * Return OK if a name found.
 */
    int
mch_get_uname(uid, s, len)
    uid_t	uid;
    char_u	*s;
    int		len;
{
#if defined(HAVE_PWD_H) && defined(HAVE_GETPWUID)
    struct passwd   *pw;

    if ((pw = getpwuid(uid)) != NULL
	    && pw->pw_name != NULL && *(pw->pw_name) != NUL)
    {
	STRNCPY(s, pw->pw_name, len);
	return OK;
    }
#endif
    sprintf((char *)s, "%d", (int)uid);	    /* assumes s is long enough */
    return FAIL;			    /* a number is not a name */
}

/*
 * Insert host name is s[len].
 */

#ifdef HAVE_SYS_UTSNAME_H
    void
mch_get_host_name(s, len)
    char_u  *s;
    int	    len;
{
    struct utsname vutsname;

    uname(&vutsname);
    STRNCPY(s, vutsname.nodename, len);
}
#else /* HAVE_SYS_UTSNAME_H */

# ifdef HAVE_SYS_SYSTEMINFO_H
#  define gethostname(nam, len) sysinfo(SI_HOSTNAME, nam, len)
# endif

    void
mch_get_host_name(s, len)
    char_u  *s;
    int	    len;
{
    gethostname((char *)s, len);
}
#endif /* HAVE_SYS_UTSNAME_H */

/*
 * return process ID
 */
    long
mch_get_pid()
{
    return (long)getpid();
}

#if !defined(HAVE_STRERROR) && defined(USE_GETCWD)
static char *strerror __ARGS((int));

    static char *
strerror(err)
    int err;
{
    extern int	    sys_nerr;
    extern char	    *sys_errlist[];
    static char	    er[20];

    if (err > 0 && err < sys_nerr)
	return (sys_errlist[err]);
    sprintf(er, "Error %d", err);
    return er;
}
#endif

/*
 * Get name of current directory into buffer 'buf' of length 'len' bytes.
 * Return OK for success, FAIL for failure.
 */
    int
mch_dirname(buf, len)
    char_u  *buf;
    int	    len;
{
#if defined(USE_GETCWD)
    if (getcwd((char *)buf, len) == NULL)
    {
	STRCPY(buf, strerror(errno));
	return FAIL;
    }
    return OK;
#else
    return (getwd((char *)buf) != NULL ? OK : FAIL);
#endif
}

#if defined(OS2) || defined(PROTO)
/*
 * Replace all slashes by backslashes.
 * When 'shellslash' set do it the other way around.
 */
    void
slash_adjust(p)
    char_u  *p;
{
    while (*p)
    {
	if (*p == psepcN)
	    *p = psepc;
#ifdef FEAT_MBYTE
	if (has_mbyte)
	    p += (*mb_ptr2len_check)(p);
	else
#endif
	    ++p;
    }
}
#endif

/*
 * Get absolute file name into buffer 'buf' of length 'len' bytes.
 *
 * return FAIL for failure, OK for success
 */
    int
mch_FullName(fname, buf, len, force)
    char_u	*fname, *buf;
    int		len;
    int		force;		/* also expand when already absolute path */
{
    int		l;
#ifdef OS2
    int		only_drive;	/* file name is only a drive letter */
#endif
#ifdef HAVE_FCHDIR
    int		fd = -1;
    static int	dont_fchdir = FALSE;	/* TRUE when fchdir() doesn't work */
#endif
    char_u	olddir[MAXPATHL];
    char_u	*p;
    int		retval = OK;

    /* expand it if forced or not an absolute path */
    if (force || !mch_isFullName(fname))
    {
	/*
	 * If the file name has a path, change to that directory for a moment,
	 * and then do the getwd() (and get back to where we were).
	 * This will get the correct path name with "../" things.
	 */
#ifdef OS2
	only_drive = 0;
	if (((p = vim_strrchr(fname, '/')) != NULL)
		|| ((p = vim_strrchr(fname, '\\')) != NULL)
		|| (((p = vim_strchr(fname,  ':')) != NULL) && ++only_drive))
#else
	if ((p = vim_strrchr(fname, '/')) != NULL)
#endif
	{
#ifdef HAVE_FCHDIR
	    /*
	     * Use fchdir() if possible, it's said to be faster and more
	     * reliable.  But on SunOS 4 it might not work.  Check this by
	     * doing a fchdir() right now.
	     */
	    if (!dont_fchdir)
	    {
		fd = open(".", O_RDONLY | O_EXTRA, 0);
		if (fd >= 0 && fchdir(fd) < 0)
		{
		    close(fd);
		    fd = -1;
		    dont_fchdir = TRUE;	    /* don't try again */
		}
	    }
#endif
	    if (
#ifdef HAVE_FCHDIR
		fd < 0 &&
#endif
			    mch_dirname(olddir, MAXPATHL) == FAIL)
	    {
		p = NULL;	/* can't get current dir: don't chdir */
		retval = FAIL;
	    }
	    else
	    {
#ifdef OS2
		/*
		 * compensate for case where ':' from "D:" was the only
		 * path separator detected in the file name; the _next_
		 * character has to be removed, and then restored later.
		 */
		if (only_drive)
		    p++;
#endif
		/* The directory is copied into buf[], to be able to remove
		 * the file name without changing it (could be a string in
		 * read-only memory) */
		if (p - fname >= len)
		    retval = FAIL;
		else
		{
		    STRNCPY(buf, fname, p - fname);
		    buf[p - fname] = NUL;
		    if (mch_chdir((char *)buf))
			retval = FAIL;
		    else
			fname = p + 1;
		    *buf = NUL;
		}
#ifdef OS2
		if (only_drive)
		{
		    p--;
		    if (retval != FAIL)
			fname--;
		}
#endif
	    }
	}
	if (mch_dirname(buf, len) == FAIL)
	{
	    retval = FAIL;
	    *buf = NUL;
	}
	if (p != NULL)
	{
#ifdef HAVE_FCHDIR
	    if (fd >= 0)
	    {
		fchdir(fd);
		close(fd);
	    }
	    else
#endif
		mch_chdir((char *)olddir);
	}

	l = STRLEN(buf);
	if (l >= len)
	    retval = FAIL;
#ifndef VMS
	else
	{
	    if (l > 0 && buf[l - 1] != '/' && *fname != NUL)
		STRCAT(buf, "/");
	}
#endif
    }
    /* Catch file names which are too long. */
    if (retval == FAIL || STRLEN(buf) + STRLEN(fname) >= len)
	return FAIL;

#ifdef VMS
    STRCAT(buf, vms_fixfilename(fname));
#else
    STRCAT(buf, fname);
#endif

    return OK;
}

/*
 * Return TRUE if "fname" does not depend on the current directory.
 */
    int
mch_isFullName(fname)
    char_u	*fname;
{
#ifdef __EMX__
    return _fnisabs(fname);
#else
# ifdef VMS
    return ( fname[0] == '/' || fname[0] == '.' || strchr((char *)fname, ':') ||
	     strchr((char *)fname,'[') || strchr((char *)fname,']') ||
	     strchr((char *)fname,'<') || strchr((char *)fname,'>')	 );
# else
    return (*fname == '/' || *fname == '~');
# endif
#endif
}

/*
 * Get file permissions for 'name'.
 * Returns -1 when it doesn't exist.
 */
    long
mch_getperm(name)
    char_u *name;
{
    struct stat statb;

    if (stat((char *)name, &statb))
	return -1;
    return statb.st_mode;
}

/*
 * set file permission for 'name' to 'perm'
 *
 * return FAIL for failure, OK otherwise
 */
    int
mch_setperm(name, perm)
    char_u  *name;
    long    perm;
{
    return (chmod((char *)name, (mode_t)perm) == 0 ? OK : FAIL);
}

#if defined(HAVE_ACL) || defined(PROTO)
# ifdef HAVE_SYS_ACL_H
#  include <sys/acl.h>
# endif
# ifdef HAVE_SYS_ACCESS_H
#  include <sys/access.h>
# endif

# ifdef HAVE_SOLARIS_ACL
typedef struct vim_acl_solaris_T {
    int acl_cnt;
    aclent_t *acl_entry;
} vim_acl_solaris_T;
# endif

/*
 * Return a pointer to the ACL of file "fname" in allocated memory.
 * Return NULL if the ACL is not available for whatever reason.
 */
    vim_acl_T
mch_get_acl(fname)
    char_u	*fname;
{
    vim_acl_T	ret = NULL;
#ifdef HAVE_POSIX_ACL
    ret = (vim_acl_T)acl_get_file((char *)fname, ACL_TYPE_ACCESS);
#else
#ifdef HAVE_SOLARIS_ACL
    vim_acl_solaris_T   *aclent;

    aclent = malloc(sizeof(vim_acl_solaris_T));
    if ((aclent->acl_cnt = acl((char *)fname, GETACLCNT, 0, NULL)) < 0)
    {
	free(aclent);
	return NULL;
    }
    aclent->acl_entry = malloc(aclent->acl_cnt * sizeof(aclent_t));
    if (acl((char *)fname, GETACL, aclent->acl_cnt, aclent->acl_entry) < 0)
    {
	free(aclent->acl_entry);
	free(aclent);
	return NULL;
    }
    ret = (vim_acl_T)aclent;
#else
#if defined(HAVE_AIX_ACL)
    int		aclsize;
    struct acl *aclent;

    aclsize = sizeof(struct acl);
    aclent = malloc(aclsize);
    if (statacl((char *)fname, STX_NORMAL, aclent, aclsize) < 0)
    {
	if (errno == ENOSPC)
	{
	    aclsize = aclent->acl_len;
	    aclent = realloc(aclent, aclsize);
	    if (statacl((char *)fname, STX_NORMAL, aclent, aclsize) < 0)
	    {
		free(aclent);
		return NULL;
	    }
	}
	else
	{
	    free(aclent);
	    return NULL;
	}
    }
    ret = (vim_acl_T)aclent;
#endif /* HAVE_AIX_ACL */
#endif /* HAVE_SOLARIS_ACL */
#endif /* HAVE_POSIX_ACL */
    return ret;
}

/*
 * Set the ACL of file "fname" to "acl" (unless it's NULL).
 */
    void
mch_set_acl(fname, aclent)
    char_u	*fname;
    vim_acl_T	aclent;
{
    if (aclent == NULL)
	return;
#ifdef HAVE_POSIX_ACL
    acl_set_file((char *)fname, ACL_TYPE_ACCESS, (acl_t)aclent);
#else
#ifdef HAVE_SOLARIS_ACL
    acl((char *)fname, SETACL, ((vim_acl_solaris_T *)aclent)->acl_cnt,
	    ((vim_acl_solaris_T *)aclent)->acl_entry);
#else
#ifdef HAVE_AIX_ACL
    chacl((char *)fname, aclent, ((struct acl *)aclent)->acl_len);
#endif /* HAVE_AIX_ACL */
#endif /* HAVE_SOLARIS_ACL */
#endif /* HAVE_POSIX_ACL */
}

    void
mch_free_acl(aclent)
    vim_acl_T	aclent;
{
    if (aclent == NULL)
	return;
#ifdef HAVE_POSIX_ACL
    acl_free((acl_t)aclent);
#else
#ifdef HAVE_SOLARIS_ACL
    free(((vim_acl_solaris_T *)aclent)->acl_entry);
    free(aclent);
#else
#ifdef HAVE_AIX_ACL
    free(aclent);
#endif /* HAVE_AIX_ACL */
#endif /* HAVE_SOLARIS_ACL */
#endif /* HAVE_POSIX_ACL */
}
#endif

/*
 * Set hidden flag for "name".
 */
/* ARGSUSED */
    void
mch_hide(name)
    char_u	*name;
{
    /* can't hide a file */
}

/*
 * return TRUE if "name" is a directory
 * return FALSE if "name" is not a directory
 * return FALSE for error
 */
    int
mch_isdir(name)
    char_u *name;
{
    struct stat statb;

    if (*name == NUL)	    /* Some stat()s don't flag "" as an error. */
	return FALSE;
    if (stat((char *)name, &statb))
	return FALSE;
#ifdef _POSIX_SOURCE
    return (S_ISDIR(statb.st_mode) ? TRUE : FALSE);
#else
    return ((statb.st_mode & S_IFMT) == S_IFDIR ? TRUE : FALSE);
#endif
}

#if defined(FEAT_EVAL) || defined(PROTO)
/*
 * Return 1 if "name" can be executed, 0 if not.
 * Return -1 if unknown.
 */
    int
mch_can_exe(name)
    char_u	*name;
{
    char_u	*buf;
    char_u	*p;
    int		retval;

#ifdef VMS
    /* TODO */
    return -1;
#endif

    buf = alloc((unsigned)STRLEN(name) + 7);
    if (buf == NULL)
	return -1;
    sprintf((char *)buf, "which %s", name);
    p = get_cmd_output(buf, SHELL_SILENT);
    vim_free(buf);
    if (p == NULL)
	return -1;
    /* result can be: "name: Command not found" */
    retval = (*p != NUL && strstr((char *)p, "not found") == NULL);
    vim_free(p);
    return retval;
}
#endif

/*
 * Check what "name" is:
 * NODE_NORMAL: file or directory (or doesn't exist)
 * NODE_WRITABLE: writable device, socket, fifo, etc.
 * NODE_OTHER: non-writable things
 */
    int
mch_nodetype(name)
    char_u	*name;
{
    struct stat	st;

    if (stat((char *)name, &st))
	return NODE_NORMAL;
    if (S_ISREG(st.st_mode) || S_ISDIR(st.st_mode))
	return NODE_NORMAL;
#ifndef OS2
    if (S_ISBLK(st.st_mode))	/* block device isn't writable */
	return NODE_OTHER;
#endif
    /* Everything else is writable? */
    return NODE_WRITABLE;
}

    void
mch_early_init()
{
#ifdef HAVE_CHECK_STACK_GROWTH
    int			i;
#endif

#ifdef HAVE_CHECK_STACK_GROWTH
    check_stack_growth((char *)&i);

# ifdef HAVE_GETRLIMIT
    get_stack_limit();
# endif

#endif

    /*
     * Setup an alternative stack for signals.  Helps to catch signals when
     * running out of stack space.
     * Use of sigaltstack() is preferred, it's more portable.
     * Ignore any errors.
     */
#if defined(HAVE_SIGALTSTACK) || defined(HAVE_SIGSTACK)
    signal_stack = malloc(SIGSTKSZ);
    init_signal_stack();
#endif
}

    void
mch_exit(r)
    int r;
{
    exiting = TRUE;

#if defined(FEAT_X11) && defined(FEAT_CLIPBOARD)
    x11_export_final_selection();
#endif

#ifdef FEAT_GUI
    if (!gui.in_use)
#endif
    {
	settmode(TMODE_COOK);
#ifdef FEAT_TITLE
	mch_restore_title(3);	/* restore xterm title and icon name */
#endif
	/* Stop termcap first: May need to check for T_CRV response, which
	 * requires RAW mode. */
	stoptermcap();

	/*
	 * A newline is only required after a message in the alternate screen.
	 * This is set to TRUE by wait_return().
	 */
	if (!swapping_screen() || newline_on_exit)
	{
	    if (newline_on_exit || msg_didout)
		out_char('\n');
	    else
	    {
		restore_cterm_colors();	/* get original colors back */
		msg_clr_eos();		/* clear the rest of the display */
		windgoto((int)Rows - 1, 0);	/* may have moved the cursor */
	    }
	}

	/* Cursor may have been switched off without calling starttermcap()
	 * when doing "vim -u vimrc" and vimrc contains ":q". */
	if (full_screen)
	    cursor_on();
    }
    out_flush();
    ml_close_all(TRUE);		/* remove all memfiles */
    may_core_dump();
#ifdef FEAT_GUI
# ifndef FEAT_GUI_BEOS		/* BeOS always has GUI */
    if (gui.in_use)
# endif
	gui_exit(r);
#endif
#ifdef __QNX__
    /* A core dump won't be created if the signal handler
     * doesn't return, so we can't call exit() */
    if (deadly_signal != 0)
	return;
#endif

    exit(r);
}

    static void
may_core_dump()
{
    if (deadly_signal != 0)
    {
	signal(deadly_signal, SIG_DFL);
	kill(getpid(), deadly_signal);	/* Die using the signal we caught */
    }
}

#ifndef VMS

    void
mch_settmode(tmode)
    int		tmode;
{
    static int first = TRUE;

    /* Why is NeXT excluded here (and not in os_unixx.h)? */
#if defined(ECHOE) && defined(ICANON) && (defined(HAVE_TERMIO_H) || defined(HAVE_TERMIOS_H)) && !defined(__NeXT__)
    /*
     * for "new" tty systems
     */
# ifdef HAVE_TERMIOS_H
    static struct termios told;
	   struct termios tnew;
# else
    static struct termio told;
	   struct termio tnew;
# endif

    if (first)
    {
	first = FALSE;
# if defined(HAVE_TERMIOS_H)
	tcgetattr(read_cmd_fd, &told);
# else
	ioctl(read_cmd_fd, TCGETA, &told);
# endif
    }

    tnew = told;
    if (tmode == TMODE_RAW)
    {
	/*
	 * ~ICRNL enables typing ^V^M
	 */
	tnew.c_iflag &= ~ICRNL;
	tnew.c_lflag &= ~(ICANON | ECHO | ISIG | ECHOE
# if defined(IEXTEN) && !defined(__MINT__)
		    | IEXTEN	    /* IEXTEN enables typing ^V on SOLARIS */
				    /* but it breaks function keys on MINT */
# endif
				);
# ifdef ONLCR	    /* don't map NL -> CR NL, we do it ourselves */
	tnew.c_oflag &= ~ONLCR;
# endif
	tnew.c_cc[VMIN] = 1;		/* return after 1 char */
	tnew.c_cc[VTIME] = 0;		/* don't wait */
    }
    else if (tmode == TMODE_SLEEP)
	tnew.c_lflag &= ~(ECHO);

# if defined(HAVE_TERMIOS_H)
    tcsetattr(read_cmd_fd, TCSANOW, &tnew);
# else
    ioctl(read_cmd_fd, TCSETA, &tnew);
# endif

#else

    /*
     * for "old" tty systems
     */
# ifndef TIOCSETN
#  define TIOCSETN TIOCSETP	/* for hpux 9.0 */
# endif
    static struct sgttyb ttybold;
	   struct sgttyb ttybnew;

    if (first)
    {
	first = FALSE;
	ioctl(read_cmd_fd, TIOCGETP, &ttybold);
    }

    ttybnew = ttybold;
    if (tmode == TMODE_RAW)
    {
	ttybnew.sg_flags &= ~(CRMOD | ECHO);
	ttybnew.sg_flags |= RAW;
    }
    else if (tmode == TMODE_SLEEP)
	ttybnew.sg_flags &= ~(ECHO);
    ioctl(read_cmd_fd, TIOCSETN, &ttybnew);
#endif
    curr_tmode = tmode;
}

/*
 * Try to get the code for "t_kb" from the stty setting
 *
 * Even if termcap claims a backspace key, the user's setting *should*
 * prevail.  stty knows more about reality than termcap does, and if
 * somebody's usual erase key is DEL (which, for most BSD users, it will
 * be), they're going to get really annoyed if their erase key starts
 * doing forward deletes for no reason. (Eric Fischer)
 */
    void
get_stty()
{
    char_u  buf[2];
    char_u  *p;

    /* Why is NeXT excluded here (and not in os_unixx.h)? */
#if defined(ECHOE) && defined(ICANON) && (defined(HAVE_TERMIO_H) || defined(HAVE_TERMIOS_H)) && !defined(__NeXT__)
    /* for "new" tty systems */
# ifdef HAVE_TERMIOS_H
    struct termios keys;
# else
    struct termio keys;
# endif

# if defined(HAVE_TERMIOS_H)
    if (tcgetattr(read_cmd_fd, &keys) != -1)
# else
    if (ioctl(read_cmd_fd, TCGETA, &keys) != -1)
# endif
    {
	buf[0] = keys.c_cc[VERASE];
	intr_char = keys.c_cc[VINTR];
#else
    /* for "old" tty systems */
    struct sgttyb keys;

    if (ioctl(read_cmd_fd, TIOCGETP, &keys) != -1)
    {
	buf[0] = keys.sg_erase;
	intr_char = keys.sg_kill;
#endif
	buf[1] = NUL;
	add_termcode((char_u *)"kb", buf, FALSE);

	/*
	 * If <BS> and <DEL> are now the same, redefine <DEL>.
	 */
	p = find_termcode((char_u *)"kD");
	if (p != NULL && p[0] == buf[0] && p[1] == buf[1])
	    do_fixdel(NULL);
    }
#if 0
    }	    /* to keep cindent happy */
#endif
}

#endif /* VMS  */

#if defined(FEAT_MOUSE_TTY) || defined(PROTO)
/*
 * Set mouse clicks on or off.
 */
    void
mch_setmouse(on)
    int		on;
{
    static int	ison = FALSE;
    int		xterm_mouse_vers;

    if (on == ison)	/* return quickly if nothing to do */
	return;

    xterm_mouse_vers = use_xterm_mouse();
    if (xterm_mouse_vers > 0)
    {
	if (on)	/* enable mouse events, use mouse tracking if available */
	    out_str_nf((char_u *)
		       (xterm_mouse_vers > 1
			? IF_EB("\033[?1002h", ESC_STR "[?1002h")
			: IF_EB("\033[?1000h", ESC_STR "[?1000h")));
	else	/* disable mouse events, could probably always send the same */
	    out_str_nf((char_u *)
		       (xterm_mouse_vers > 1
			? IF_EB("\033[?1002l", "[?1002l")
			: IF_EB("\033[?1000l", "[?1000l")));
	ison = on;
    }

# ifdef FEAT_MOUSE_DEC
    else if (ttym_flags == TTYM_DEC)
    {
	if (on)	/* enable mouse events */
	    out_str_nf((char_u *)"\033[1;2'z\033[1;3'{");
	else	/* disable mouse events */
	    out_str_nf((char_u *)"\033['z");
	ison = on;
    }
# endif

# ifdef FEAT_MOUSE_GPM
    else
    {
	if (on)
	{
	    if (gpm_open())
		ison = TRUE;
	}
	else
	{
	    gpm_close();
	    ison = FALSE;
	}
    }
# endif

# ifdef FEAT_MOUSE_JSB
    else
    {
	if (on)
	{
	    /* D - Enable Mouse up/down messages
	     * L - Enable Left Button Reporting
	     * M - Enable Middle Button Reporting
	     * R - Enable Right Button Reporting
	     * K - Enable SHIFT and CTRL key Reporting
	     * + - Enable Advanced messaging of mouse moves and up/down messages
	     * Q - Quiet No Ack
	     * # - Numeric value of mouse pointer required
	     *	  0 = Multiview 2000 cursor, used as standard
	     *	  1 = Windows Arrow
	     *	  2 = Windows I Beam
	     *	  3 = Windows Hour Glass
	     *	  4 = Windows Cross Hair
	     *	  5 = Windows UP Arrow
	     */
#ifdef JSBTERM_MOUSE_NONADVANCED /* Disables full feedback of pointer movements */
	    out_str_nf((char_u *)IF_EB("\033[0~ZwLMRK1Q\033\\",
					 ESC_STR "[0~ZwLMRK1Q" ESC_STR "\\"));
#else
	    out_str_nf((char_u *)IF_EB("\033[0~ZwLMRK+1Q\033\\",
					ESC_STR "[0~ZwLMRK+1Q" ESC_STR "\\"));
#endif
	    ison = TRUE;
	}
	else
	{
	    out_str_nf((char_u *)IF_EB("\033[0~ZwQ\033\\",
					      ESC_STR "[0~ZwQ" ESC_STR "\\"));
	    ison = FALSE;
	}
    }
# endif
# ifdef FEAT_MOUSE_PTERM
    else
    {
	/* 1 = button press, 6 = release, 7 = drag, 1h...9l = right button */
	if (on)
	    out_str_nf("\033[>1h\033[>6h\033[>7h\033[>1h\033[>9l");
	else
	    out_str_nf("\033[>1l\033[>6l\033[>7l\033[>1l\033[>9h");
	ison = on;
    }
# endif
}

/*
 * Set the mouse termcode, depending on the 'term' and 'ttymouse' options.
 */
    void
check_mouse_termcode()
{
# ifdef FEAT_MOUSE_XTERM
    if (use_xterm_mouse()
#  ifdef FEAT_GUI
	    && !gui.in_use
#  endif
	    )
    {
	set_mouse_termcode(KS_MOUSE, (char_u *)(term_is_8bit(T_NAME)
		  ? IF_EB("\233M", CSI_STR "M") : IF_EB("\033[M", ESC_STR "[M")));
	if (*p_mouse != NUL)
	{
	    /* force mouse off and maybe on to send possibly new mouse
	     * activation sequence to the xterm, with(out) drag tracing. */
	    mch_setmouse(FALSE);
	    setmouse();
	}
    }
    else
	del_mouse_termcode(KS_MOUSE);
# endif

# ifdef FEAT_MOUSE_GPM
    if (!use_xterm_mouse()
#  ifdef FEAT_GUI
	    && !gui.in_use
#  endif
	    )
	set_mouse_termcode(KS_MOUSE, (char_u *)IF_EB("\033MG", ESC_STR "MG"));
# endif

# ifdef FEAT_MOUSE_JSB
    /* conflicts with xterm mouse: "\033[" and "\033[M" ??? */
    if (!use_xterm_mouse()
#  ifdef FEAT_GUI
	    && !gui.in_use
#  endif
	    )
	set_mouse_termcode(KS_JSBTERM_MOUSE,
			       (char_u *)IF_EB("\033[0~zw", ESC_STR "[0~zw"));
    else
	del_mouse_termcode(KS_JSBTERM_MOUSE);
# endif

# ifdef FEAT_MOUSE_NET
    /* There is no conflict, but one may type ESC } from Insert mode.  Don't
     * define it in the GUI or when using an xterm. */
    if (!use_xterm_mouse()
#  ifdef FEAT_GUI
	    && !gui.in_use
#  endif
	    )
	set_mouse_termcode(KS_NETTERM_MOUSE,
				       (char_u *)IF_EB("\033}", ESC_STR "}"));
    else
	del_mouse_termcode(KS_NETTERM_MOUSE);
# endif

# ifdef FEAT_MOUSE_DEC
    /* conflicts with xterm mouse: "\033[" and "\033[M" */
    if (!use_xterm_mouse()
#  ifdef FEAT_GUI
	    && !gui.in_use
#  endif
	    )
	set_mouse_termcode(KS_DEC_MOUSE,
				       (char_u *)IF_EB("\033[", ESC_STR "["));
    else
	del_mouse_termcode(KS_DEC_MOUSE);
# endif
# ifdef FEAT_MOUSE_PTERM
    /* same as the dec mouse */
    if (!use_xterm_mouse()
#  ifdef FEAT_GUI
	    && !gui.in_use
#  endif
	    )
	set_mouse_termcode(KS_PTERM_MOUSE,
				      (char_u *) IF_EB("\033[", ESC_STR "["));
    else
	del_mouse_termcode(KS_PTERM_MOUSE);
# endif
}
#endif

/*
 * set screen mode, always fails.
 */
/* ARGSUSED */
    int
mch_screenmode(arg)
    char_u   *arg;
{
    EMSG(_("E359: Screen mode setting not supported"));
    return FAIL;
}

#ifndef VMS

/*
 * Try to get the current window size:
 * 1. with an ioctl(), most accurate method
 * 2. from the environment variables LINES and COLUMNS
 * 3. from the termcap
 * 4. keep using the old values
 * Return OK when size could be determined, FAIL otherwise.
 */
    int
mch_get_shellsize()
{
    long	rows = 0;
    long	columns = 0;
    char_u	*p;

    /*
     * For OS/2 use _scrsize().
     */
# ifdef __EMX__
    {
	int s[2];

	_scrsize(s);
	columns = s[0];
	rows = s[1];
    }
# endif

    /*
     * 1. try using an ioctl. It is the most accurate method.
     *
     * Try using TIOCGWINSZ first, some systems that have it also define TIOCGSIZE
     * but don't have a struct ttysize.
     */
# ifdef TIOCGWINSZ
    {
	struct winsize	ws;

	if (ioctl(1, TIOCGWINSZ, &ws) == 0)
	{
	    columns = ws.ws_col;
	    rows = ws.ws_row;
	}
    }
# else /* TIOCGWINSZ */
#  ifdef TIOCGSIZE
    {
	struct ttysize	ts;

	if (ioctl(1, TIOCGSIZE, &ts) == 0)
	{
	    columns = ts.ts_cols;
	    rows = ts.ts_lines;
	}
    }
#  endif /* TIOCGSIZE */
# endif /* TIOCGWINSZ */

    /*
     * 2. get size from environment
     */
    if (columns == 0 || rows == 0)
    {
	if ((p = (char_u *)getenv("LINES")))
	    rows = atoi((char *)p);
	if ((p = (char_u *)getenv("COLUMNS")))
	    columns = atoi((char *)p);
    }

#ifdef HAVE_TGETENT
    /*
     * 3. try reading "co" and "li" entries from termcap
     */
    if (columns == 0 || rows == 0)
	getlinecol(&columns, &rows);
#endif

    /*
     * 4. If everything fails, use the old values
     */
    if (columns <= 0 || rows <= 0)
	return FAIL;

    Rows = rows;
    Columns = columns;
    return OK;
}

/*
 * Try to set the window size to Rows and Columns.
 */
    void
mch_set_shellsize()
{
    if (*T_CWS)
    {
	/*
	 * NOTE: if you get an error here that term_set_winsize() is
	 * undefined, check the output of configure.  It could probably not
	 * find a ncurses, termcap or termlib library.
	 */
	term_set_winsize((int)Rows, (int)Columns);
	out_flush();
	screen_start();			/* don't know where cursor is now */
    }
}

#endif /* VMS */

/*
 * Rows and/or Columns has changed.
 */
    void
mch_new_shellsize()
{
    /* Nothing to do. */
}

    int
mch_call_shell(cmd, options)
    char_u	*cmd;
    int		options;	/* SHELL_*, see vim.h */
{
#ifdef VMS
    char	*ifn = NULL;
    char	*ofn = NULL;
#endif
#ifdef USE_SYSTEM	/* use system() to start the shell: simple but slow */
    int	    x;
#ifndef __EMX__
    char_u  *newcmd;   /* only needed for unix */
#else
    /*
     * Set the preferred shell in the EMXSHELL environment variable (but
     * only if it is different from what is already in the environment).
     * Emx then takes care of whether to use "/c" or "-c" in an
     * intelligent way. Simply pass the whole thing to emx's system() call.
     * Emx also starts an interactive shell if system() is passed an empty
     * string.
     */
    char_u *p, *old;

    if (((old = (char_u *)getenv("EMXSHELL")) == NULL) || STRCMP(old, p_sh))
    {
	/* should check HAVE_SETENV, but I know we don't have it. */
	p = alloc(10 + strlen(p_sh));
	if (p)
	{
	    sprintf((char *)p, "EMXSHELL=%s", p_sh);
	    putenv((char *)p);	/* don't free the pointer! */
	}
    }
#endif

    out_flush();

    if (options & SHELL_COOKED)
	settmode(TMODE_COOK);	    /* set to normal mode */

#ifdef __EMX__
    if (cmd == NULL)
	x = system("");	/* this starts an interactive shell in emx */
    else
	x = system((char *)cmd);
    /* system() returns -1 when error occurs in starting shell */
    if (x == -1 && !emsg_silent)
    {
	MSG_PUTS(_("\nCannot execute shell "));
	msg_outtrans(p_sh);
	msg_putchar('\n');
    }
#else /* not __EMX__ */
    if (cmd == NULL)
	x = system((char *)p_sh);
    else
    {
# ifdef VMS
	if (ofn = strchr((char *)cmd, '>'))
	    *ofn++ = '\0';
	if (ifn = strchr((char *)cmd, '<'))
	{
	    char *p;

	    *ifn++ = '\0';
	    p = strchr(ifn,' '); /* chop off any trailing spaces */
	    if (p)
		*p = '\0';
	}
	if (ofn)
	    x = vms_sys((char *)cmd, ofn, ifn);
	else
	    x = system((char *)cmd);
# else
	newcmd = lalloc(STRLEN(p_sh)
		+ (extra_shell_arg == NULL ? 0 : STRLEN(extra_shell_arg))
		+ STRLEN(p_shcf) + STRLEN(cmd) + 4, TRUE);
	if (newcmd == NULL)
	    x = 0;
	else
	{
	    sprintf((char *)newcmd, "%s %s %s %s", p_sh,
		    extra_shell_arg == NULL ? "" : (char *)extra_shell_arg,
		    (char *)p_shcf,
		    (char *)cmd);
	    x = system((char *)newcmd);
	    vim_free(newcmd);
	}
# endif
    }
    if (emsg_silent)
	;
    else if (x == 127)
	MSG_PUTS(_("\nCannot execute shell sh\n"));
#endif	/* __EMX__ */
    else if (x && !(options & SHELL_SILENT))
    {
	MSG_PUTS(_("\nshell returned "));
	msg_outnum((long)x);
	msg_putchar('\n');
    }

    settmode(TMODE_RAW);		/* set to raw mode */
#ifdef FEAT_TITLE
    resettitle();
#endif
    return x;

#else /* USE_SYSTEM */	    /* don't use system(), use fork()/exec() */

#define EXEC_FAILED 122	    /* Exit code when shell didn't execute.  Don't use
			       127, some shell use that already */

    char_u	*newcmd = NULL;
    pid_t	pid;
    pid_t	wait_pid = 0;
#ifdef HAVE_UNION_WAIT
    union wait	status;
#else
    int		status = -1;
#endif
    int		retval = -1;
    char	**argv = NULL;
    int		argc;
    int		i;
    char_u	*p;
    int		inquote;
#ifdef FEAT_GUI
    int		pty_master_fd = -1;	    /* for pty's */
    int		pty_slave_fd = -1;
    char	*tty_name;
    int		fd_toshell[2];	    /* for pipes */
    int		fd_fromshell[2];
    int		pipe_error = FALSE;
# ifdef HAVE_SETENV
    char	envbuf[50];
# else
    static char	envbuf_Rows[20];
    static char	envbuf_Columns[20];
# endif
#endif
    int		did_settmode = FALSE; /* TRUE when settmode(TMODE_RAW) called */

    out_flush();
    if (options & SHELL_COOKED)
	settmode(TMODE_COOK);		/* set to normal mode */

    /*
     * 1: find number of arguments
     * 2: separate them and built argv[]
     */
    newcmd = vim_strsave(p_sh);
    if (newcmd == NULL)		/* out of memory */
	goto error;
    for (i = 0; i < 2; ++i)
    {
	p = newcmd;
	inquote = FALSE;
	argc = 0;
	for (;;)
	{
	    if (i == 1)
		argv[argc] = (char *)p;
	    ++argc;
	    while (*p && (inquote || (*p != ' ' && *p != TAB)))
	    {
		if (*p == '"')
		    inquote = !inquote;
		++p;
	    }
	    if (*p == NUL)
		break;
	    if (i == 1)
		*p++ = NUL;
	    p = skipwhite(p);
	}
	if (i == 0)
	{
	    argv = (char **)alloc((unsigned)((argc + 4) * sizeof(char *)));
	    if (argv == NULL)	    /* out of memory */
		goto error;
	}
    }
    if (cmd != NULL)
    {
	if (extra_shell_arg != NULL)
	    argv[argc++] = (char *)extra_shell_arg;
	argv[argc++] = (char *)p_shcf;
	argv[argc++] = (char *)cmd;
    }
    argv[argc] = NULL;

#ifdef FEAT_GUI
    /*
     * For the GUI: Try using a pseudo-tty to get the stdin/stdout of the
     * executed command into the Vim window.  Or use a pipe.
     */
    if (gui.in_use && show_shell_mess)
    {
	/*
	 * Try to open a master pty.
	 * If this works, open the slave pty.
	 * If the slave can't be opened, close the master pty.
	 */
	if (p_guipty)
	{
	    pty_master_fd = OpenPTY(&tty_name);	    /* open pty */
	    if (pty_master_fd >= 0 && ((pty_slave_fd =
				    open(tty_name, O_RDWR | O_EXTRA, 0)) < 0))
	    {
		close(pty_master_fd);
		pty_master_fd = -1;
	    }
	}
	/*
	 * If not opening a pty or it didn't work, try using pipes.
	 */
	if (pty_master_fd < 0)
	{
	    pipe_error = (pipe(fd_toshell) < 0);
	    if (!pipe_error)			    /* pipe create OK */
	    {
		pipe_error = (pipe(fd_fromshell) < 0);
		if (pipe_error)			    /* pipe create failed */
		{
		    close(fd_toshell[0]);
		    close(fd_toshell[1]);
		}
	    }
	    if (pipe_error)
	    {
		MSG_PUTS(_("\nCannot create pipes\n"));
		out_flush();
	    }
	}
    }

    if (!pipe_error)			/* pty or pipe opened or not used */
#endif

    {
#ifdef __BEOS__
	beos_cleanup_read_thread();
#endif
	if ((pid = fork()) == -1)	/* maybe we should use vfork() */
	{
	    MSG_PUTS(_("\nCannot fork\n"));
#ifdef FEAT_GUI
	    if (gui.in_use && show_shell_mess)
	    {
		if (pty_master_fd >= 0)		/* close the pseudo tty */
		{
		    close(pty_master_fd);
		    close(pty_slave_fd);
		}
		else				/* close the pipes */
		{
		    close(fd_toshell[0]);
		    close(fd_toshell[1]);
		    close(fd_fromshell[0]);
		    close(fd_fromshell[1]);
		}
	    }
#endif
	}
	else if (pid == 0)	/* child */
	{
	    reset_signals();		/* handle signals normally */

	    if (!show_shell_mess || (options & SHELL_EXPAND))
	    {
		int fd;

		/*
		 * Don't want to show any message from the shell.  Can't just
		 * close stdout and stderr though, because some systems will
		 * break if you try to write to them after that, so we must
		 * use dup() to replace them with something else -- webb
		 * Connect stdin to /dev/null too, so ":n `cat`" doesn't hang,
		 * waiting for input.
		 */
		fd = open("/dev/null", O_RDWR | O_EXTRA, 0);
		fclose(stdin);
		fclose(stdout);
		fclose(stderr);

		/*
		 * If any of these open()'s and dup()'s fail, we just continue
		 * anyway.  It's not fatal, and on most systems it will make
		 * no difference at all.  On a few it will cause the execvp()
		 * to exit with a non-zero status even when the completion
		 * could be done, which is nothing too serious.  If the open()
		 * or dup() failed we'd just do the same thing ourselves
		 * anyway -- webb
		 */
		if (fd >= 0)
		{
		    dup(fd); /* To replace stdin  (file descriptor 0) */
		    dup(fd); /* To replace stdout (file descriptor 1) */
		    dup(fd); /* To replace stderr (file descriptor 2) */

		    /* Don't need this now that we've duplicated it */
		    close(fd);
		}
	    }
#ifdef FEAT_GUI
	    else if (gui.in_use)
	    {

#ifdef HAVE_SETSID
		(void)setsid();
#endif
		/* push stream discipline modules */
		if (options & SHELL_COOKED)
		    SetupSlavePTY(pty_slave_fd);
#ifdef TIOCSCTTY
		/* try to become controlling tty (probably doesn't work,
		 * unless run by root) */
		ioctl(pty_slave_fd, TIOCSCTTY, (char *)NULL);
#endif
		/* Simulate to have a dumb terminal (for now) */
#ifdef HAVE_SETENV
		setenv("TERM", "dumb", 1);
		sprintf((char *)envbuf, "%ld", Rows);
		setenv("ROWS", (char *)envbuf, 1);
		sprintf((char *)envbuf, "%ld", Rows);
		setenv("LINES", (char *)envbuf, 1);
		sprintf((char *)envbuf, "%ld", Columns);
		setenv("COLUMNS", (char *)envbuf, 1);
#else
		/*
		 * Putenv does not copy the string, it has to remain valid.
		 * Use a static array to avoid loosing allocated memory.
		 */
		putenv("TERM=dumb");
		sprintf(envbuf_Rows, "ROWS=%ld", Rows);
		putenv(envbuf_Rows);
		sprintf(envbuf_Rows, "LINES=%ld", Rows);
		putenv(envbuf_Rows);
		sprintf(envbuf_Columns, "COLUMNS=%ld", Columns);
		putenv(envbuf_Columns);
#endif

		if (pty_master_fd >= 0)
		{
		    close(pty_master_fd);   /* close master side of pty */

		    /* set up stdin/stdout/stderr for the child */
		    close(0);
		    dup(pty_slave_fd);
		    close(1);
		    dup(pty_slave_fd);
		    close(2);
		    dup(pty_slave_fd);

		    close(pty_slave_fd);    /* has been dupped, close it now */
		}
		else
		{
		    /* set up stdin for the child */
		    close(fd_toshell[1]);
		    close(0);
		    dup(fd_toshell[0]);
		    close(fd_toshell[0]);

		    /* set up stdout for the child */
		    close(fd_fromshell[0]);
		    close(1);
		    dup(fd_fromshell[1]);
		    close(fd_fromshell[1]);

		    /* set up stderr for the child */
		    close(2);
		    dup(1);
		}
	    }
#endif
	    /*
	     * There is no type cast for the argv, because the type may be
	     * different on different machines. This may cause a warning
	     * message with strict compilers, don't worry about it.
	     * Call _exit() instead of exit() to avoid closing the connection
	     * to the X server (esp. with GTK, which uses atexit()).
	     */
	    execvp(argv[0], argv);
	    _exit(EXEC_FAILED);	    /* exec failed, return failure code */
	}
	else			/* parent */
	{
	    /*
	     * While child is running, ignore terminating signals.
	     */
	    catch_signals(SIG_IGN, SIG_ERR);

#ifdef FEAT_GUI

	    /*
	     * For the GUI we redirect stdin, stdout and stderr to our window.
	     */
	    if (gui.in_use && show_shell_mess)
	    {
#define BUFLEN 100		/* length for buffer, pseudo tty limit is 128 */
		char_u	    buffer[BUFLEN + 1];
		char_u	    ta_buf[BUFLEN + 1];	/* TypeAHead */
		int	    ta_len = 0;		/* valid chars in ta_buf[] */
		int	    len;
		int	    p_more_save;
		int	    old_State;
		int	    c;
		int	    toshell_fd;
		int	    fromshell_fd;

		if (pty_master_fd >= 0)
		{
		    close(pty_slave_fd);	/* close slave side of pty */
		    fromshell_fd = pty_master_fd;
		    toshell_fd = dup(pty_master_fd);
		}
		else
		{
		    close(fd_toshell[0]);
		    close(fd_fromshell[1]);
		    toshell_fd = fd_toshell[1];
		    fromshell_fd = fd_fromshell[0];
		}

		/*
		 * Write to the child if there are typed characters.
		 * Read from the child if there are characters available.
		 *   Repeat the reading a few times if more characters are
		 *   available. Need to check for typed keys now and then, but
		 *   not too often (delays when no chars are available).
		 * This loop is quit if no characters can be read from the pty
		 * (WaitForChar detected special condition), or there are no
		 * characters available and the child has exited.
		 * Only check if the child has exited when there is no more
		 * output. The child may exit before all the output has
		 * been printed.
		 *
		 * Currently this busy loops!
		 * This can probably dead-lock when the write blocks!
		 */
		p_more_save = p_more;
		p_more = FALSE;
		old_State = State;
		State = EXTERNCMD;	/* don't redraw at window resize */

		for (;;)
		{
		    /*
		     * Check if keys have been typed, write them to the child
		     * if there are any.  Don't do this if we are expanding
		     * wild cards (would eat typeahead).  Don't get extra
		     * characters when we already have one.
		     */
		    len = 0;
		    if (!(options & SHELL_EXPAND)
			    && (ta_len > 0
				|| (len = ui_inchar(ta_buf, BUFLEN, 10L)) > 0))
		    {
			/*
			 * For pipes:
			 * Check for CTRL-C: send interrupt signal to child.
			 * Check for CTRL-D: EOF, close pipe to child.
			 */
			if (len == 1 && (pty_master_fd < 0 || cmd != NULL))
			{
#ifdef SIGINT
			    /*
			     * Send SIGINT to the child's group or all
			     * processes in our group.
			     */
			    if (ta_buf[ta_len] == Ctrl_C
					       || ta_buf[ta_len] == intr_char)
# ifdef HAVE_SETSID
				kill(-pid, SIGINT);
# else
				kill(0, SIGINT);
# endif
#endif
			    if (pty_master_fd < 0 && toshell_fd >= 0
					       && ta_buf[ta_len] == Ctrl_D)
			    {
				close(toshell_fd);
				toshell_fd = -1;
			    }
			}

			/* replace K_BS by <BS> and K_DEL by <DEL> */
			for (i = ta_len; i < ta_len + len; ++i)
			{
			    if (ta_buf[i] == CSI && len - i > 2)
			    {
				c = TERMCAP2KEY(ta_buf[i + 1], ta_buf[i + 2]);
				if (c == K_DEL || c == K_KDEL || c == K_BS)
				{
				    mch_memmove(ta_buf + i + 1, ta_buf + i + 3,
						       (size_t)(len - i - 2));
				    if (c == K_DEL || c == K_KDEL)
					ta_buf[i] = DEL;
				    else
					ta_buf[i] = Ctrl_H;
				    len -= 2;
				}
			    }
			    else if (ta_buf[i] == '\r')
				ta_buf[i] = '\n';
			}

			/*
			 * For pipes: echo the typed characters.
			 * For a pty this does not seem to work.
			 */
			if (pty_master_fd < 0)
			{
			    for (i = ta_len; i < ta_len + len; ++i)
				if (ta_buf[i] == '\n' || ta_buf[i] == '\b')
				    msg_putchar(ta_buf[i]);
				else
				    msg_outtrans_len(ta_buf + i, 1);
			    windgoto(msg_row, msg_col);
			    out_flush();
			}

			ta_len += len;

			/*
			 * Write the characters to the child, unless EOF has
			 * been typed for pipes.  Write one character at a
			 * time, to avoid loosing too much typeahead.
			 */
			if (toshell_fd >= 0)
			{
			    len = write(toshell_fd, (char *)ta_buf, (size_t)1);
			    if (len > 0)
			    {
				ta_len -= len;
				mch_memmove(ta_buf, ta_buf + len, ta_len);
			    }
			}
		    }

		    /*
		     * Check if the child has any characters to be printed.
		     * Read them and write them to our window.	Repeat this as
		     * long as there is something to do, avoid the 10ms wait
		     * for mch_inchar(), or sending typeahead characters to
		     * the external process.
		     * TODO: This should handle escape sequences, compatible
		     * to some terminal (vt52?).
		     */
		    while (RealWaitForChar(fromshell_fd, 10L, NULL))
		    {
			len = read(fromshell_fd, (char *)buffer,
							      (size_t)BUFLEN);
			if (len <= 0)		    /* end of file or error */
			    goto finished;
			buffer[len] = NUL;
			msg_puts(buffer);
			windgoto(msg_row, msg_col);
			cursor_on();
			out_flush();
			if (got_int)
			    break;
		    }

		    /*
		     * Check if the child still exists, before checking for
		     * typed characters (otherwise we would loose typeahead).
		     */
#ifdef __NeXT__
		    wait_pid = wait4(pid, &status, WNOHANG, (struct rusage *) 0);
#else
		    wait_pid = waitpid(pid, &status, WNOHANG);
#endif
		    if ((wait_pid == (pid_t)-1 && errno == ECHILD)
			    || (wait_pid == pid && WIFEXITED(status)))
		    {
			wait_pid = pid;
			break;
		    }
		    wait_pid = 0;
		}
finished:
		p_more = p_more_save;

		/*
		 * Give all typeahead that wasn't used back to ui_inchar().
		 */
		if (ta_len)
		    ui_inchar_undo(ta_buf, ta_len);

		State = old_State;
		if (toshell_fd >= 0)
		    close(toshell_fd);
		close(fromshell_fd);
	    }
#endif /* FEAT_GUI */

	    /*
	     * Wait until our child has exited.
	     * Ignore wait() returning pids of other children and returning
	     * because of some signal like SIGWINCH.
	     * Don't wait if wait_pid was already set above, indicating the
	     * child already exited.
	     */
	    while (wait_pid != pid)
	    {
		wait_pid = wait(&status);
		if (wait_pid <= 0
#ifdef ECHILD
			&& errno == ECHILD
#endif
		   )
		    break;
	    }

	    /*
	     * Set to raw mode right now, otherwise a CTRL-C after
	     * catch_signals() will kill Vim.
	     */
	    settmode(TMODE_RAW);
	    did_settmode = TRUE;
	    set_signals();

	    if (WIFEXITED(status))
	    {
		retval = WEXITSTATUS(status);
		if (retval && !emsg_silent)
		{
		    if (retval == EXEC_FAILED)
		    {
			MSG_PUTS(_("\nCannot execute shell "));
			msg_outtrans(p_sh);
			msg_putchar('\n');
		    }
		    else if (!(options & SHELL_SILENT))
		    {
			MSG_PUTS(_("\nshell returned "));
			msg_outnum((long)retval);
			msg_putchar('\n');
		    }
		}
	    }
	    else
		MSG_PUTS(_("\nCommand terminated\n"));
	}
    }
    vim_free(argv);

error:
    if (!did_settmode)
	settmode(TMODE_RAW);		    /* set to raw mode */
#ifdef FEAT_TITLE
    resettitle();
#endif
    vim_free(newcmd);

    return retval;

#endif /* USE_SYSTEM */
}

/*
 * Check for CTRL-C typed by reading all available characters.
 * In cooked mode we should get SIGINT, no need to check.
 */
    void
mch_breakcheck()
{
    if (curr_tmode == TMODE_RAW && RealWaitForChar(read_cmd_fd, 0L, NULL))
	fill_input_buf(FALSE);
}

/*
 * Wait "msec" msec until a character is available from the keyboard or from
 * inbuf[]. msec == -1 will block forever.
 * When a GUI is being used, this will never get called -- webb
 */
    static int
WaitForChar(msec)
    long	msec;
{
#ifdef FEAT_MOUSE_GPM
    int		gpm_process_wanted;
#endif
#ifdef FEAT_XCLIPBOARD
    int		rest;
#endif
    int		avail;

    if (!vim_is_input_buf_empty())	    /* something in inbuf[] */
	return 1;

#if defined(FEAT_MOUSE_DEC)
    /* May need to query the mouse position. */
    if (WantQueryMouse)
    {
	WantQueryMouse = 0;
	mch_write((char_u *)IF_EB("\033[1'|", ESC_STR "[1'|"), 5);
    }
#endif

    /*
     * For FEAT_MOUSE_GPM and FEAT_XCLIPBOARD we loop here to process mouse
     * events.  This is a bit complicated, because they might both be defined.
     */
#if defined(FEAT_MOUSE_GPM) || defined(FEAT_XCLIPBOARD)
# ifdef FEAT_XCLIPBOARD
    rest = 0;
    if (do_xterm_trace())
	rest = msec;
# endif
    do
    {
# ifdef FEAT_XCLIPBOARD
	if (rest != 0)
	{
	    msec = XT_TRACE_DELAY;
	    if (rest >= 0 && rest < XT_TRACE_DELAY)
		msec = rest;
	    if (rest >= 0)
		rest -= msec;
	}
# endif
# ifdef FEAT_MOUSE_GPM
	gpm_process_wanted = 0;
	avail = RealWaitForChar(read_cmd_fd, msec, &gpm_process_wanted);
# else
	avail = RealWaitForChar(read_cmd_fd, msec, NULL);
# endif
	if (!avail)
	{
	    if (!vim_is_input_buf_empty())
		return 1;
# ifdef FEAT_XCLIPBOARD
	    if (rest == 0 || !do_xterm_trace())
# endif
		break;
	}
    }
    while (FALSE
# ifdef FEAT_MOUSE_GPM
	   || (gpm_process_wanted && mch_gpm_process() == 0)
# endif
# ifdef FEAT_XCLIPBOARD
	   || (!avail && rest != 0)
# endif
	  );

#else
    avail =  RealWaitForChar(read_cmd_fd, msec, NULL);
#endif
    return avail;
}

/*
 * Wait "msec" msec until a character is available from file descriptor "fd".
 * Time == -1 will block forever.
 * When a GUI is being used, this will not be used for input -- webb
 * Returns also, when a request from Sniff is waiting -- toni.
 * Or when a Linux GPM mouse event is waiting.
 */
/* ARGSUSED */
#if defined(__BEOS__) || defined(VMS)
    int
#else
    static  int
#endif
RealWaitForChar(fd, msec, check_for_gpm)
    int		fd;
    long	msec;
    int		*check_for_gpm;
{
    int		ret;
# if defined(FEAT_XCLIPBOARD) && defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
    struct timeval  start_tv;
# endif

    while (1)
    {
#ifndef HAVE_SELECT
	struct pollfd   fds[4];
	int		nfd;
# ifdef FEAT_XCLIPBOARD
	int		xterm_idx = -1;
# endif
# ifdef FEAT_MOUSE_GPM
	int		gpm_idx = -1;
# endif

	fds[0].fd = fd;
	fds[0].events = POLLIN;
	nfd = 1;

# ifdef FEAT_SNIFF
#  define SNIFF_IDX 1
	if (want_sniff_request)
	{
	    fds[SNIFF_IDX].fd = fd_from_sniff;
	    fds[SNIFF_IDX].events = POLLIN;
	    nfd++;
	}
# endif
# ifdef FEAT_XCLIPBOARD
	if (xterm_Shell != (Widget)0)
	{
	    xterm_idx = nfd;
	    fds[nfd].fd = ConnectionNumber(xterm_dpy);
	    fds[nfd].events = POLLIN;
	    nfd++;
#  if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
	    if (msec >= 0)
		gettimeofday(&start_tv, NULL);
#  endif
	}
# endif
# ifdef FEAT_MOUSE_GPM
	if (check_for_gpm != NULL && gpm_flag && gpm_fd >= 0)
	{
	    gpm_idx = nfd;
	    fds[nfd].fd = gpm_fd;
	    fds[nfd].events = POLLIN;
	    nfd++;
	}
# endif

	ret = poll(&fds, nfd, (int)msec);

# ifdef FEAT_SNIFF
	if (ret < 0)
	    sniff_disconnect(1);
	else if (want_sniff_request)
	{
	    if (fds[SNIFF_IDX].revents & POLLHUP)
		sniff_disconnect(1);
	    if (fds[SNIFF_IDX].revents & POLLIN)
		sniff_request_waiting = 1;
	}
# endif
# ifdef FEAT_XCLIPBOARD
	if (xterm_Shell != (Widget)0 && (fds[xterm_idx].revents & POLLIN))
	{
	    xterm_update();      /* Maybe we should hand out clipboard */
	    if (--ret == 0 && vim_is_input_buf_empty())
	    {
		if (msec > 0)
		{
#  if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
		    struct timeval  tv;

		    /* Compute remaining wait time. */
		    gettimeofday(&tv, NULL);
		    msec -= (tv.tv_sec - start_tv.tv_sec) * 1000L
				    + (tv.tv_usec - start_tv.tv_usec) / 1000L;
		    if (msec <= 0)
			break;	/* waited long enough */
#  else
		    /* Guess we got interrupted halfway. */
		    msec = msec / 2;
#  endif
		}
		continue;
	    }
	}
# endif
# ifdef FEAT_MOUSE_GPM
	if (gpm_idx >= 0 && (fds[gpm_idx].revents & POLLIN))
	{
	    *check_for_gpm = 1;
	    if (--ret == 0)
	    {
		if (msec > 0)
		    msec = msec / 2;
		continue;
	    }
	}
# endif
	break;

#else /* HAVE_SELECT */

	struct timeval  tv;
	fd_set		rfds, efds;
	int		maxfd;

# ifdef __EMX__
	/* don't check for incoming chars if not in raw mode, because select()
	 * always returns TRUE then (in some version of emx.dll) */
	if (curr_tmode != TMODE_RAW)
	    return 0;
# endif

	if (msec >= 0)
	{
	    tv.tv_sec = msec / 1000;
	    tv.tv_usec = (msec % 1000) * (1000000/1000);
	}

	/*
	 * Select on ready for reading and exceptional condition (end of file).
	 */
	FD_ZERO(&rfds); /* calls bzero() on a sun */
	FD_ZERO(&efds);
	FD_SET(fd, &rfds);
# if !defined(__QNX__) && !defined(__CYGWIN32__)
	/* For QNX select() always returns 1 if this is set.  Why? */
	FD_SET(fd, &efds);
# endif
	maxfd = fd;

# ifdef FEAT_SNIFF
	if (want_sniff_request)
	{
	    FD_SET(fd_from_sniff, &rfds);
	    FD_SET(fd_from_sniff, &efds);
	    if (maxfd < fd_from_sniff)
		maxfd = fd_from_sniff;
	}
# endif
# ifdef FEAT_XCLIPBOARD
	if (xterm_Shell != (Widget)0)
	{
	    FD_SET(ConnectionNumber(xterm_dpy), &rfds);
	    if (maxfd < ConnectionNumber(xterm_dpy))
		maxfd = ConnectionNumber(xterm_dpy);
#  if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
	    if (msec >= 0)
		gettimeofday(&start_tv, NULL);
#  endif
	}
# endif
# ifdef FEAT_MOUSE_GPM
	if (check_for_gpm != NULL && gpm_flag && gpm_fd >= 0)
	{
	    FD_SET(gpm_fd, &rfds);
	    FD_SET(gpm_fd, &efds);
	    if (maxfd < gpm_fd)
		maxfd = gpm_fd;
	}
# endif

	ret = select(maxfd + 1, &rfds, NULL, &efds, (msec >= 0) ? &tv : NULL);

# ifdef FEAT_SNIFF
	if (ret < 0 )
	    sniff_disconnect(1);
	else if (ret > 0 && want_sniff_request)
	{
	    if (FD_ISSET(fd_from_sniff, &efds))
		sniff_disconnect(1);
	    if (FD_ISSET(fd_from_sniff, &rfds))
		sniff_request_waiting = 1;
	}
# endif
# ifdef FEAT_XCLIPBOARD
	if (ret > 0 && xterm_Shell != (Widget)0
		&& FD_ISSET(ConnectionNumber(xterm_dpy), &rfds))
	{
	    xterm_update();	      /* Maybe we should hand out clipboard */
	    /* continue looping when we only got the X event and the input
	     * buffer is empty */
	    if (--ret == 0 && vim_is_input_buf_empty())
	    {
		if (msec > 0)
		{
#  if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
		    /* Compute remaining wait time. */
		    gettimeofday(&tv, NULL);
		    msec -= (tv.tv_sec - start_tv.tv_sec) * 1000L
				    + (tv.tv_usec - start_tv.tv_usec) / 1000L;
		    if (msec <= 0)
			break;	/* waited long enough */
#  else
		    /* Guess we got interrupted halfway. */
		    msec = msec / 2;
#  endif
		}
		continue;
	    }
	}
# endif
# ifdef FEAT_MOUSE_GPM
	if (ret > 0 && gpm_flag && check_for_gpm != NULL && gpm_fd >= 0)
	{
	    if (FD_ISSET(gpm_fd, &efds))
		gpm_close();
	    else if (FD_ISSET(gpm_fd, &rfds))
		*check_for_gpm = 1;
	}
# endif

	break;
#endif /* HAVE_SELECT */
    }
    return (ret > 0);
}

#ifndef VMS

#ifndef NO_EXPANDPATH
    static int
pstrcmp(a, b)
    const void *a, *b;
{
    return (pathcmp(*(char **)a, *(char **)b));
}

/*
 * Recursively expand one path component into all matching files and/or
 * directories.
 * "path" has backslashes before chars that are not to be expanded, starting
 * at "path + wildoff".
 * Return the number of matches found.
 */
    int
mch_expandpath(gap, path, flags)
    garray_T	*gap;
    char_u	*path;
    int		flags;		/* EW_* flags */
{
    return unix_expandpath(gap, path, 0, flags);
}

    static int
unix_expandpath(gap, path, wildoff, flags)
    garray_T	*gap;
    char_u	*path;
    int		wildoff;
    int		flags;		/* EW_* flags */
{
    char_u	*buf;
    char_u	*path_end;
    char_u	*p, *s, *e;
    int		start_len, c;
    char_u	*pat;
    DIR		*dirp;
    regmatch_T	regmatch;
    struct dirent *dp;
    int		starts_with_dot;
    int		matches;
    int		len;

    start_len = gap->ga_len;
    buf = alloc(STRLEN(path) + BASENAMELEN + 5);/* make room for file name */
    if (buf == NULL)
	return 0;

/*
 * Find the first part in the path name that contains a wildcard.
 * Copy it into buf, including the preceding characters.
 */
    p = buf;
    s = buf;
    e = NULL;
    path_end = path;
    while (*path_end)
    {
	/* May ignore a wildcard that has a backslash before it */
	if (path_end >= path + wildoff && rem_backslash(path_end))
	    *p++ = *path_end++;
	else if (*path_end == '/')
	{
	    if (e != NULL)
		break;
	    s = p + 1;
	}
	else if (vim_strchr((char_u *)"*?[{~$", *path_end) != NULL)
	    e = p;
#ifdef FEAT_MBYTE
	if (has_mbyte)
	{
	    len = (*mb_ptr2len_check)(path_end);
	    STRNCPY(p, path_end, len);
	    p += len;
	    path_end += len;
	}
	else
#endif
	    *p++ = *path_end++;
    }
    e = p;
    *e = NUL;

    /* now we have one wildcard component between s and e */
    /* Remove backslashes between "wildoff" and the start of the wildcard
     * component. */
    for (p = buf + wildoff; p < s; ++p)
	if (rem_backslash(p))
	{
	    STRCPY(p, p + 1);
	    --e;
	    --s;
	}

    /* convert the file pattern to a regexp pattern */
    starts_with_dot = (*s == '.');
    pat = file_pat_to_reg_pat(s, e, NULL, FALSE);
    if (pat == NULL)
    {
	vim_free(buf);
	return 0;
    }

    /* compile the regexp into a program */
#ifdef MACOS_X
    regmatch.rm_ic = TRUE;		/* Behave like Terminal.app */
#else
    regmatch.rm_ic = FALSE;		/* Don't ever ignore case */
#endif
    regmatch.regprog = vim_regcomp(pat, TRUE);
    vim_free(pat);

    if (regmatch.regprog == NULL)
    {
	vim_free(buf);
	return 0;
    }

    /* open the directory for scanning */
    c = *s;
    *s = NUL;
    dirp = opendir(*buf == NUL ? "." : (char *)buf);
    *s = c;

    /* Find all matching entries */
    if (dirp != NULL)
    {
	for (;;)
	{
	    dp = readdir(dirp);
	    if (dp == NULL)
		break;
	    if ((dp->d_name[0] != '.' || starts_with_dot)
		    && vim_regexec(&regmatch, (char_u *)dp->d_name, (colnr_T)0))
	    {
		STRCPY(s, dp->d_name);
		len = STRLEN(buf);
		STRCPY(buf + len, path_end);
		if (mch_has_exp_wildcard(path_end)) /* handle more wildcards */
		{
		    /* need to expand another component of the path */
		    /* remove backslashes for the remaining components only */
		    (void)unix_expandpath(gap, buf, len + 1, flags);
		}
		else
		{
		    /* no more wildcards, check if there is a match */
		    /* remove backslashes for the remaining components only */
		    if (*path_end)
			backslash_halve(buf + len + 1);
		    if (mch_getperm(buf) >= 0)	/* add existing file */
			addfile(gap, buf, flags);
		}
	    }
	}

	closedir(dirp);
    }

    vim_free(buf);
    vim_free(regmatch.regprog);

    matches = gap->ga_len - start_len;
    if (matches)
	qsort(((char_u **)gap->ga_data) + start_len, matches,
						   sizeof(char_u *), pstrcmp);
    return matches;
}
#endif

/*
 * mch_expand_wildcards() - this code does wild-card pattern matching using
 * the shell
 *
 * return OK for success, FAIL for error (you may lose some memory) and put
 * an error message in *file.
 *
 * num_pat is number of input patterns
 * pat is array of pointers to input patterns
 * num_file is pointer to number of matched file names
 * file is pointer to array of pointers to matched file names
 */

#ifndef SEEK_SET
# define SEEK_SET 0
#endif
#ifndef SEEK_END
# define SEEK_END 2
#endif

/* ARGSUSED */
    int
mch_expand_wildcards(num_pat, pat, num_file, file, flags)
    int		   num_pat;
    char_u	 **pat;
    int		  *num_file;
    char_u	***file;
    int		   flags;	/* EW_* flags */
{
    int		i;
    size_t	len;
    char_u	*p;
    int		dir;
#ifdef __EMX__
# define EXPL_ALLOC_INC	16
    char_u	**expl_files;
    size_t	files_alloced, files_free;
    char_u	*buf;
    int		has_wildcard;

    *num_file = 0;	/* default: no files found */
    files_alloced = EXPL_ALLOC_INC; /* how much space is allocated */
    files_free = EXPL_ALLOC_INC;    /* how much space is not used  */
    *file = (char_u **)alloc(sizeof(char_u **) * files_alloced);
    if (*file == NULL)
	return FAIL;

    for (; num_pat > 0; num_pat--, pat++)
    {
	expl_files = NULL;
	if (vim_strchr(*pat, '$') || vim_strchr(*pat, '~'))
	    /* expand environment var or home dir */
	    buf = expand_env_save(*pat);
	else
	    buf = vim_strsave(*pat);
	expl_files = NULL;
	has_wildcard = mch_has_exp_wildcard(buf);  /* (still) wildcards in there? */
	if (has_wildcard)   /* yes, so expand them */
	    expl_files = (char_u **)_fnexplode(buf);

	/*
	 * return value of buf if no wildcards left,
	 * OR if no match AND EW_NOTFOUND is set.
	 */
	if ((!has_wildcard && ((flags & EW_NOTFOUND) || mch_getperm(buf) >= 0))
		|| (expl_files == NULL && (flags & EW_NOTFOUND)))
	{   /* simply save the current contents of *buf */
	    expl_files = (char_u **)alloc(sizeof(char_u **) * 2);
	    if (expl_files != NULL)
	    {
		expl_files[0] = vim_strsave(buf);
		expl_files[1] = NULL;
	    }
	}
	vim_free(buf);

	/*
	 * Count number of names resulting from expansion,
	 * At the same time add a backslash to the end of names that happen to
	 * be directories, and replace slashes with backslashes.
	 */
	if (expl_files)
	{
	    for (i = 0; (p = expl_files[i]) != NULL; i++)
	    {
		dir = mch_isdir(p);
		/* If we don't want dirs and this is one, skip it */
		if ((dir && !(flags & EW_DIR)) || (!dir && !(flags & EW_FILE)))
		    continue;

		if (--files_free == 0)
		{
		    /* need more room in table of pointers */
		    files_alloced += EXPL_ALLOC_INC;
		    *file = (char_u **)vim_realloc(*file,
					   sizeof(char_u **) * files_alloced);
		    if (*file == NULL)
		    {
			EMSG(_(e_outofmem));
			*num_file = 0;
			return FAIL;
		    }
		    files_free = EXPL_ALLOC_INC;
		}
		slash_adjust(p);
		if (dir)
		{
		    /* For a directory we add a '/', unless it's already
		     * there. */
		    len = STRLEN(p);
		    if (((*file)[*num_file] = alloc(len + 2)) != NULL)
		    {
			STRCPY((*file)[*num_file], p);
			if (!vim_ispathsep((*file)[*num_file][len - 1]))
			{
			    (*file)[*num_file][len] = psepc;
			    (*file)[*num_file][len + 1] = 0;
			}
		    }
		}
		else
		{
		    (*file)[*num_file] = vim_strsave(p);
		}

		/*
		 * Error message already given by either alloc or vim_strsave.
		 * Should return FAIL, but returning OK works also.
		 */
		if ((*file)[*num_file] == NULL)
		    break;
		(*num_file)++;
	    }
	    _fnexplodefree((char **)expl_files);
	}
    }
    return OK;

#else /* __EMX__ */

    int		j;
    char_u	*tempname;
    char_u	*command;
    FILE	*fd;
    char_u	*buffer;
#define STYLE_ECHO  0	    /* use "echo" to expand */
#define STYLE_GLOB  1	    /* use "glob" to expand, for csh */
#define STYLE_PRINT 2	    /* use "print -N" to expand, for zsh */
#define STYLE_BT    3	    /* `cmd` expansion, execute the pattern directly */
    int		shell_style = STYLE_ECHO;
    int		check_spaces;
    static int	did_find_nul = FALSE;
    int		ampersent = FALSE;

    *num_file = 0;	/* default: no files found */
    *file = NULL;

    /*
     * If there are no wildcards, just copy the names to allocated memory.
     * Saves a lot of time, because we don't have to start a new shell.
     */
    if (!have_wildcard(num_pat, pat))
	return save_patterns(num_pat, pat, num_file, file);

    /*
     * Don't allow the use of backticks in secure and restricted mode.
     */
    if (secure || restricted)
	for (i = 0; i < num_pat; ++i)
	    if (vim_strchr(pat[i], '`') != NULL
		    && (check_restricted() || check_secure()))
		return FAIL;

    /*
     * get a name for the temp file
     */
    if ((tempname = vim_tempname('o')) == NULL)
    {
	EMSG(_(e_notmp));
	return FAIL;
    }

    /*
     * Let the shell expand the patterns and write the result into the temp
     * file.  if expanding `cmd` execute it directly.
     * If we use csh, glob will work better than echo.
     * If we use zsh, print -N will work better than glob.
     */
    if (num_pat == 1 && *pat[0] == '`'
	    && (len = STRLEN(pat[0])) > 2
	    && *(pat[0] + len - 1) == '`')
	shell_style = STYLE_BT;
    else if ((len = STRLEN(p_sh)) >= 3)
    {
	if (STRCMP(p_sh + len - 3, "csh") == 0)
	    shell_style = STYLE_GLOB;
	else if (STRCMP(p_sh + len - 3, "zsh") == 0)
	    shell_style = STYLE_PRINT;
    }

    /* "unset nonomatch; print -N >" plus two is 29 */
    len = STRLEN(tempname) + 29;
    for (i = 0; i < num_pat; ++i)	/* count the length of the patterns */
	len += STRLEN(pat[i]) + 3;	/* add space and two quotes */
    command = alloc(len);
    if (command == NULL)
    {
	/* out of memory */
	vim_free(tempname);
	return FAIL;
    }

    /*
     * Build the shell command:
     * - Set $nonomatch depending on EW_NOTFOUND (hopefully the shell
     *	 recognizes this).
     * - Add the shell command to print the expanded names.
     * - Add the temp file name.
     * - Add the file name patterns.
     */
    if (shell_style == STYLE_BT)
    {
	STRCPY(command, pat[0] + 1);		/* exclude first backtick */
	p = command + STRLEN(command) - 1;
	*p = ' ';				/* remove last backtick */
	while (p > command && vim_iswhite(*p))
	    --p;
	if (*p == '&')				/* remove trailing '&' */
	{
	    ampersent = TRUE;
	    *p = ' ';
	}
	STRCAT(command, ">");
    }
    else
    {
	if (flags & EW_NOTFOUND)
	    STRCPY(command, "set nonomatch; ");
	else
	    STRCPY(command, "unset nonomatch; ");
	if (shell_style == STYLE_GLOB)
	    STRCAT(command, "glob >");
	else if (shell_style == STYLE_PRINT)
	    STRCAT(command, "print -N >");
	else
	    STRCAT(command, "echo >");
    }
    STRCAT(command, tempname);
    if (shell_style != STYLE_BT)
	for (i = 0; i < num_pat; ++i)
	{
#ifdef USE_SYSTEM
	    STRCAT(command, " \"");	/* need extra quotes because we */
	    STRCAT(command, pat[i]);	/*	 start the shell twice */
	    STRCAT(command, "\"");
#else
	    STRCAT(command, " ");
	    STRCAT(command, pat[i]);
#endif
	}
    if (flags & EW_SILENT)
	show_shell_mess = FALSE;
    if (ampersent)
	STRCAT(command, "&");		/* put the '&' back after the
					   redirection */

    /*
     * Using zsh -G: If a pattern has no matches, it is just deleted from
     * the argument list, otherwise zsh gives an error message and doesn't
     * expand any other pattern.
     */
    if (shell_style == STYLE_PRINT)
	extra_shell_arg = (char_u *)"-G";   /* Use zsh NULL_GLOB option */

    /*
     * If we use -f then shell variables set in .cshrc won't get expanded.
     * vi can do it, so we will too, but it is only necessary if there is a "$"
     * in one of the patterns, otherwise we can still use the fast option.
     */
    else if (shell_style == STYLE_GLOB && !have_dollars(num_pat, pat))
	extra_shell_arg = (char_u *)"-f";	/* Use csh fast option */

    /*
     * execute the shell command
     */
    i = call_shell(command, SHELL_EXPAND | SHELL_SILENT);

    /* When running in the background, give it some time to create the temp
     * file, but don't wait for it to finish. */
    if (ampersent)
	mch_delay(10L, TRUE);

    extra_shell_arg = NULL;		/* cleanup */
    show_shell_mess = TRUE;
    vim_free(command);

    if (i)				/* mch_call_shell() failed */
    {
	mch_remove(tempname);
	vim_free(tempname);
	/*
	 * With interactive completion, the error message is not printed.
	 * However with USE_SYSTEM, I don't know how to turn off error messages
	 * from the shell, so screen may still get messed up -- webb.
	 */
#ifndef USE_SYSTEM
	if (!(flags & EW_SILENT))
#endif
	{
	    redraw_later_clear();	/* probably messed up screen */
	    msg_putchar('\n');		/* clear bottom line quickly */
	    cmdline_row = Rows - 1;	/* continue on last line */
#ifdef USE_SYSTEM
	    if (!(flags & EW_SILENT))
#endif
	    {
		MSG(_(e_wildexpand));
		msg_start();		/* don't overwrite this message */
	    }
	}
	/* If a `cmd` expansion failed, don't list `cmd` as a match, even when
	 * EW_NOTFOUND is given */
	if (shell_style == STYLE_BT)
	    return FAIL;
	goto notfound;
    }

    /*
     * read the names from the file into memory
     */
    fd = fopen((char *)tempname, "r");
    if (fd == NULL)
    {
	/* Something went wrong, perhaps a file name with a special char. */
	if (!(flags & EW_SILENT))
	{
	    MSG(_(e_wildexpand));
	    msg_start();		/* don't overwrite this message */
	}
	vim_free(tempname);
	goto notfound;
    }
    fseek(fd, 0L, SEEK_END);
    len = ftell(fd);			/* get size of temp file */
    fseek(fd, 0L, SEEK_SET);
    buffer = alloc(len + 1);
    if (buffer == NULL)
    {
	/* out of memory */
	mch_remove(tempname);
	vim_free(tempname);
	fclose(fd);
	return FAIL;
    }
    i = fread((char *)buffer, 1, len, fd);
    fclose(fd);
    mch_remove(tempname);
    if (i != len)
    {
	/* unexpected read error */
	EMSG2(_(e_notread), tempname);
	vim_free(tempname);
	vim_free(buffer);
	return FAIL;
    }
    vim_free(tempname);

    /* file names are separated with Space */
    if (shell_style == STYLE_ECHO)
    {
	buffer[len] = '\n';		/* make sure the buffer ends in NL */
	p = buffer;
	for (i = 0; *p != '\n'; ++i)	/* count number of entries */
	{
	    while (*p != ' ' && *p != '\n')
		++p;
	    p = skipwhite(p);		/* skip to next entry */
	}
    }
    /* file names are separated with NL */
    else if (shell_style == STYLE_BT)
    {
	buffer[len] = NUL;		/* make sure the buffer ends in NUL */
	p = buffer;
	for (i = 0; *p != NUL; ++i)	/* count number of entries */
	{
	    while (*p != '\n' && *p != NUL)
		++p;
	    if (*p != NUL)
		++p;
	    p = skipwhite(p);		/* skip leading white space */
	}
    }
    /* file names are separated with NUL */
    else
    {
	/*
	 * Some versions of zsh use spaces instead of NULs to separate
	 * results.  Only do this when there is no NUL before the end of the
	 * buffer, otherwise we would never be able to use file names with
	 * embedded spaces when zsh does use NULs.
	 * When we found a NUL once, we know zsh is OK, set did_find_nul and
	 * don't check for spaces again.
	 */
	check_spaces = FALSE;
	if (shell_style == STYLE_PRINT && !did_find_nul)
	{
	    /* If there is a NUL, set did_find_nul, else set check_spaces */
	    if (len && (int)STRLEN(buffer) < len - 1)
		did_find_nul = TRUE;
	    else
		check_spaces = TRUE;
	}

	/*
	 * Make sure the buffer ends with a NUL.  For STYLE_PRINT there
	 * already is one, for STYLE_GLOB it needs to be added.
	 */
	if (len && buffer[len - 1] == NUL)
	    --len;
	else
	    buffer[len] = NUL;
	i = 0;
	for (p = buffer; p < buffer + len; ++p)
	    if (*p == NUL || (*p == ' ' && check_spaces))   /* count entry */
	    {
		++i;
		*p = NUL;
	    }
	if (len)
	    ++i;			/* count last entry */
    }
    if (i == 0)
    {
	/*
	 * Can happen when using /bin/sh and typing ":e $NO_SUCH_VAR^I".
	 * /bin/sh will happily expand it to nothing rather than returning an
	 * error; and hey, it's good to check anyway -- webb.
	 */
	vim_free(buffer);
	goto notfound;
    }
    *num_file = i;
    *file = (char_u **)alloc(sizeof(char_u *) * i);
    if (*file == NULL)
    {
	/* out of memory */
	vim_free(buffer);
	return FAIL;
    }

    /*
     * Isolate the individual file names.
     */
    p = buffer;
    for (i = 0; i < *num_file; ++i)
    {
	(*file)[i] = p;
	/* Space or NL separates */
	if (shell_style == STYLE_ECHO || shell_style == STYLE_BT)
	{
	    while (!(shell_style == STYLE_ECHO && *p == ' ') && *p != '\n')
		++p;
	    if (p == buffer + len)		/* last entry */
		*p = NUL;
	    else
	    {
		*p++ = NUL;
		p = skipwhite(p);		/* skip to next entry */
	    }
	}
	else		/* NUL separates */
	{
	    while (*p && p < buffer + len)	/* skip entry */
		++p;
	    ++p;				/* skip NUL */
	}
    }

    /*
     * Move the file names to allocated memory.
     */
    for (j = 0, i = 0; i < *num_file; ++i)
    {
	/* Require the files to exist.	Helps when using /bin/sh */
	if (!(flags & EW_NOTFOUND) && mch_getperm((*file)[i]) < 0)
	    continue;

	/* check if this entry should be included */
	dir = (mch_isdir((*file)[i]));
	if ((dir && !(flags & EW_DIR)) || (!dir && !(flags & EW_FILE)))
	    continue;

	p = alloc((unsigned)(STRLEN((*file)[i]) + 1 + dir));
	if (p)
	{
	    STRCPY(p, (*file)[i]);
	    if (dir)
		STRCAT(p, "/");	    /* add '/' to a directory name */
	    (*file)[j++] = p;
	}
    }
    vim_free(buffer);
    *num_file = j;

    if (*num_file == 0)	    /* rejected all entries */
    {
	vim_free(*file);
	*file = NULL;
	goto notfound;
    }

    return OK;

notfound:
    if (flags & EW_NOTFOUND)
	return save_patterns(num_pat, pat, num_file, file);
    return FAIL;

#endif /* __EMX__ */
}

#endif /* VMS */

#ifndef __EMX__
    static int
save_patterns(num_pat, pat, num_file, file)
    int		num_pat;
    char_u	**pat;
    int		*num_file;
    char_u	***file;
{
    int		i;

    *file = (char_u **)alloc(num_pat * sizeof(char_u *));
    if (*file == NULL)
	return FAIL;
    for (i = 0; i < num_pat; i++)
	(*file)[i] = vim_strsave(pat[i]);
    *num_file = num_pat;
    return OK;
}
#endif


/*
 * Return TRUE if the string "p" contains a wildcard that mch_expandpath() can
 * expand.
 */
    int
mch_has_exp_wildcard(p)
    char_u  *p;
{
    for ( ; *p; ++p)
    {
#ifndef OS2
	if (*p == '\\' && p[1] != NUL)
	    ++p;
	else
#endif
	    if (vim_strchr((char_u *)
#ifdef VMS
				    "*?%"
#else
# ifdef OS2
				    "*?"
# else
				    "*?[{'"
# endif
#endif
						, *p) != NULL)
	    return TRUE;
    }
    return FALSE;
}

/*
 * Return TRUE if the string "p" contains a wildcard.
 * Don't recognize '~' at the end as a wildcard.
 */
    int
mch_has_wildcard(p)
    char_u  *p;
{
    for ( ; *p; ++p)
    {
#ifndef OS2
	if (*p == '\\' && p[1] != NUL)
	    ++p;
	else
#endif
	    if (vim_strchr((char_u *)
#ifdef VMS
				    "*?%$"
#else
# ifdef OS2
#  ifdef VIM_BACKTICK
				    "*?$`"
#  else
				    "*?$"
#  endif
# else
				    "*?[{`'$"
# endif
#endif
						, *p) != NULL
		|| (*p == '~' && p[1] != NUL))
	    return TRUE;
    }
    return FALSE;
}

#ifndef __EMX__
    static int
have_wildcard(num, file)
    int	    num;
    char_u  **file;
{
    int	    i;

    for (i = 0; i < num; i++)
	if (mch_has_wildcard(file[i]))
	    return 1;
    return 0;
}

    static int
have_dollars(num, file)
    int	    num;
    char_u  **file;
{
    int	    i;

    for (i = 0; i < num; i++)
	if (vim_strchr(file[i], '$') != NULL)
	    return TRUE;
    return FALSE;
}
#endif	/* ifndef __EMX__ */

#ifndef HAVE_RENAME
/*
 * Scaled-down version of rename(), which is missing in Xenix.
 * This version can only move regular files and will fail if the
 * destination exists.
 */
    int
mch_rename(src, dest)
    const char *src, *dest;
{
    struct stat	    st;

    if (stat(dest, &st) >= 0)	    /* fail if destination exists */
	return -1;
    if (link(src, dest) != 0)	    /* link file to new name */
	return -1;
    if (mch_remove(src) == 0)	    /* delete link to old name */
	return 0;
    return -1;
}
#endif /* !HAVE_RENAME */

#ifdef FEAT_MOUSE_GPM
/*
 * Initializes connection with gpm (if it isn't already opened)
 * Return 1 if succeeded (or connection already opened), 0 if failed
 */
    static int
gpm_open()
{
    static Gpm_Connect gpm_connect; /* Must it be kept till closing ? */

    if (!gpm_flag)
    {
	gpm_connect.eventMask = (GPM_UP | GPM_DRAG | GPM_DOWN);
	gpm_connect.defaultMask = ~GPM_HARD;
	/* Default handling for mouse move*/
	gpm_connect.minMod = 0; /* Handle any modifier keys */
	gpm_connect.maxMod = 0xffff;
	if (Gpm_Open(&gpm_connect, 0) > 0)
	{
	    /* gpm library tries to handling TSTP causes
	     * problems. Anyways, we close connection to Gpm whenever
	     * we are going to suspend or starting an external process
	     * so we should'nt  have problem with this
	     */
	    signal(SIGTSTP, restricted ? SIG_IGN : SIG_DFL);
	    return 1; /* succeed */
	}
	if (gpm_fd == -2)
	    Gpm_Close(); /* We don't want to talk to xterm via gpm */
	return 0;
    }
    return 1; /* already open */
}

/*
 * Closes connection to gpm
 * returns non-zero if connection succesfully closed
 */
    static void
gpm_close()
{
    if (gpm_flag && gpm_fd >= 0) /* if Open */
	Gpm_Close();
}

/* Reads gpm event and adds special keys to input buf. Returns length of
 * generated key sequence.
 * This function is made after gui_send_mouse_event
 */
    static int
mch_gpm_process()
{
    int			button;
    static Gpm_Event	gpm_event;
    char_u		string[6];
    int_u		vim_modifiers;
    int			row,col;
    unsigned char	buttons_mask;
    unsigned char	gpm_modifiers;
    static unsigned char old_buttons = 0;

    Gpm_GetEvent(&gpm_event);

#ifdef FEAT_GUI
    /* Don't put events in the input queue now. */
    if (hold_gui_events)
	return 0;
#endif

    row = gpm_event.y - 1;
    col = gpm_event.x - 1;

    string[0] = ESC; /* Our termcode */
    string[1] = 'M';
    string[2] = 'G';
    switch (GPM_BARE_EVENTS(gpm_event.type))
    {
	case GPM_DRAG:
	    string[3] = MOUSE_DRAG;
	    break;
	case GPM_DOWN:
	    buttons_mask = gpm_event.buttons & ~old_buttons;
	    old_buttons = gpm_event.buttons;
	    switch (buttons_mask)
	    {
		case GPM_B_LEFT:
		    button = MOUSE_LEFT;
		    break;
		case GPM_B_MIDDLE:
		    button = MOUSE_MIDDLE;
		    break;
		case GPM_B_RIGHT:
		    button = MOUSE_RIGHT;
		    break;
		default:
		    return 0;
		    /*Don't know what to do. Can more than one button be
		     * reported in one event? */
	    }
	    string[3] = (char_u)(button | 0x20);
	    SET_NUM_MOUSE_CLICKS(string[3], gpm_event.clicks + 1);
	    break;
	case GPM_UP:
	    string[3] = MOUSE_RELEASE;
	    old_buttons &= ~gpm_event.buttons;
	    break;
	default:
	    return 0;
    }
    /*This code is based on gui_x11_mouse_cb in gui_x11.c */
    gpm_modifiers = gpm_event.modifiers;
    vim_modifiers = 0x0;
    /* I ignore capslock stats. Aren't we all just hate capslock mixing with
     * Vim commands ? Besides, gpm_event.modifiers is unsigned char, and
     * K_CAPSSHIFT is defined 8, so it probably isn't even reported
     */
    if (gpm_modifiers & ((1 << KG_SHIFT) | (1 << KG_SHIFTR) | (1 << KG_SHIFTL)))
	vim_modifiers |= MOUSE_SHIFT;

    if (gpm_modifiers & ((1 << KG_CTRL) | (1 << KG_CTRLR) | (1 << KG_CTRLL)))
	vim_modifiers |= MOUSE_CTRL;
    if (gpm_modifiers & ((1 << KG_ALT) | (1 << KG_ALTGR)))
	vim_modifiers |= MOUSE_ALT;
    string[3] |= vim_modifiers;
    string[4] = (char_u)(col + ' ' + 1);
    string[5] = (char_u)(row + ' ' + 1);
    add_to_input_buf(string, 6);
    return 6;
}
#endif /* FEAT_MOUSE_GPM */

#if (defined(FEAT_EVAL) && (defined(USE_DLOPEN) || defined(HAVE_SHL_LOAD))) \
	|| defined(PROTO)
typedef char_u * (*STRPROCSTR)__ARGS((char_u *));
typedef char_u * (*INTPROCSTR)__ARGS((int));
typedef int (*STRPROCINT)__ARGS((char_u *));
typedef int (*INTPROCINT)__ARGS((int));

/*
 * Call a DLL routine which takes either a string or int param
 * and returns an allocated string.
 */
    int
mch_libcall(libname, funcname, argstring, argint, string_result, number_result)
    char_u	*libname;
    char_u	*funcname;
    char_u	*argstring;	/* NULL when using a argint */
    int		argint;
    char_u	**string_result;/* NULL when using number_result */
    int		*number_result;
{
# if defined(USE_DLOPEN)
    void	*hinstLib;
# else
    shl_t	hinstLib;
# endif
    STRPROCSTR	ProcAdd;
    INTPROCSTR	ProcAddI;
    char_u	*retval_str = NULL;
    int		retval_int = 0;
    int		success = FALSE;

    /* Get a handle to the DLL module. */
# if defined(USE_DLOPEN)
    hinstLib = dlopen((char *)libname, RTLD_LAZY
#  ifdef RTLD_LOCAL
	    | RTLD_LOCAL
#  endif
	    );
# else
    hinstLib = shl_load((const char*)libname, BIND_IMMEDIATE|BIND_VERBOSE, 0L);
# endif

    /* If the handle is valid, try to get the function address. */
    if (hinstLib != NULL)
    {
# ifdef HAVE_SETJMP_H
	/*
	 * Catch a crash when calling the library function.  For example when
	 * using a number where a string pointer is expected.
	 */
	mch_startjmp();
	if (SETJMP(lc_jump_env) != 0)
	    success = FALSE;
	else
# endif
	{
	    retval_str = NULL;
	    retval_int = 0;

	    if (argstring != NULL)
	    {
# if defined(USE_DLOPEN)
		ProcAdd = (STRPROCSTR)dlsym(hinstLib, (const char *)funcname);
# else
		if (shl_findsym(&hinstLib, (const char *)funcname,
					TYPE_PROCEDURE, (void *)&ProcAdd) < 0)
		    ProcAdd = NULL;
# endif
		if ((success = (ProcAdd != NULL)))
		{
		    if (string_result == NULL)
			retval_int = ((STRPROCINT)ProcAdd)(argstring);
		    else
			retval_str = (ProcAdd)(argstring);
		}
	    }
	    else
	    {
# if defined(USE_DLOPEN)
		ProcAddI = (INTPROCSTR)dlsym(hinstLib, (const char *)funcname);
# else
		if (shl_findsym(&hinstLib, (const char *)funcname,
				       TYPE_PROCEDURE, (void *)&ProcAddI) < 0)
		    ProcAddI = NULL;
# endif
		if ((success = (ProcAddI != NULL)))
		{
		    if (string_result == NULL)
			retval_int = ((INTPROCINT)ProcAddI)(argint);
		    else
			retval_str = (ProcAddI)(argint);
		}
	    }

	    /* Save the string before we free the library. */
	    /* Assume that a "1" or "-1" result is an illegal pointer. */
	    if (string_result == NULL)
		*number_result = retval_int;
	    else if (retval_str != NULL
		    && retval_str != (char_u *)1
		    && retval_str != (char_u *)-1)
		*string_result = vim_strsave(retval_str);
	}

# ifdef HAVE_SETJMP_H
	mch_endjmp();
#  ifdef SIGHASARG
	if (lc_signal != 0)
	{
	    int i;

	    /* try to find the name of this signal */
	    for (i = 0; signal_info[i].sig != -1; i++)
		if (lc_signal == signal_info[i].sig)
		    break;
	    EMSG2("E368: got SIG%s in libcall()", signal_info[i].name);
	}
#  endif
# endif

	/* Free the DLL module. */
# if defined(USE_DLOPEN)
	(void)dlclose(hinstLib);
# else
	(void)shl_unload(hinstLib);
# endif
    }

    if (!success)
    {
	EMSG2(_("E364: Library call failed for \"%s()\""), funcname);
	return FAIL;
    }

    return OK;
}
#endif

#if (defined(FEAT_X11) && defined(FEAT_XCLIPBOARD)) || defined(PROTO)
static int	xterm_trace = -1;	/* default: disabled */
static int	xterm_button;

/*
 * Setup a dummy window for X selections in a terminal.
 */
    void
setup_term_clip()
{
    int		z = 0;
    char	*strp = "";
    Widget	AppShell;

    if (!x_connect_to_server())
	return;

    open_app_context();
    if (app_context != NULL && xterm_Shell == (Widget)0)
    {
	int (*oldhandler)();
# if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
	struct timeval  start_tv;

	if (p_verbose > 0)
	    gettimeofday(&start_tv, NULL);
# endif

	/* Ignore X errors while opening the display */
	oldhandler = XSetErrorHandler(x_error_check);

	xterm_dpy = XtOpenDisplay(app_context, xterm_display,
		"vim_xterm", "Vim_xterm", NULL, 0, &z, &strp);

	/* Now handle X errors normally. */
	(void)XSetErrorHandler(oldhandler);

	if (xterm_dpy == NULL)
	{
	    if (p_verbose > 0)
		MSG(_("Opening the X display failed"));
	    return;
	}

# if defined(HAVE_GETTIMEOFDAY) && defined(HAVE_SYS_TIME_H)
	if (p_verbose > 0)
	    xopen_message(&start_tv);
# endif

	/* Create a Shell to make converters work. */
	AppShell = XtVaAppCreateShell("vim_xterm", "Vim_xterm",
		applicationShellWidgetClass, xterm_dpy,
		NULL);
	if (AppShell == (Widget)0)
	    return;
	xterm_Shell = XtVaCreatePopupShell("VIM",
		topLevelShellWidgetClass, AppShell,
		XtNmappedWhenManaged, 0,
		XtNwidth, 1,
		XtNheight, 1,
		NULL);
	if (xterm_Shell == (Widget)0)
	    return;

	x11_setup_atoms(xterm_dpy);
	if (x11_display == NULL)
	    x11_display = xterm_dpy;

	XtRealizeWidget(xterm_Shell);
	XSync(xterm_dpy, False);
	xterm_update();
    }
    if (xterm_Shell != (Widget)0)
    {
	clip_init(TRUE);
	if (x11_window == 0 && (strp = getenv("WINDOWID")) != NULL)
	    x11_window = (Window)atol(strp);
	/* Check if $WINDOWID is valid. */
	if (test_x11_window(xterm_dpy) == FAIL)
	    x11_window = 0;
	if (x11_window != 0)
	    xterm_trace = 0;
    }
}

    void
start_xterm_trace(button)
    int button;
{
    if (x11_window == 0 || xterm_trace < 0 || xterm_Shell == (Widget)0)
	return;
    xterm_trace = 1;
    xterm_button = button;
    do_xterm_trace();
}


    void
stop_xterm_trace()
{
    if (xterm_trace < 0)
	return;
    xterm_trace = 0;
}

/*
 * Query the xterm pointer and generate mouse termcodes if necessary
 * return TRUE if dragging is active, else FALSE
 */
    static int
do_xterm_trace()
{
    Window		root, child;
    int			root_x, root_y;
    int			win_x, win_y;
    int			row, col;
    int_u		mask_return;
    char_u		buf[50];
    char_u		*strp;
    long		got_hints;
    static char_u	*mouse_code;
    static char_u	mouse_name[2] = {KS_MOUSE, KE_FILLER};
    static int		prev_row = 0, prev_col = 0;
    static XSizeHints	xterm_hints;

    if (xterm_trace <= 0)
	return FALSE;

    if (xterm_trace == 1)
    {
	/* Get the hints just before tracking starts.  The font size might
	 * have changed recently */
	XGetWMNormalHints(xterm_dpy, x11_window, &xterm_hints, &got_hints);
	if (!(got_hints & PResizeInc)
		|| xterm_hints.width_inc <= 1
		|| xterm_hints.height_inc <= 1)
	{
	    xterm_trace = -1;  /* Not enough data -- disable tracing */
	    return FALSE;
	}

	/* Rely on the same mouse code for the duration of this */
	mouse_code = find_termcode(mouse_name);
	prev_row = mouse_row;
	prev_row = mouse_col;
	xterm_trace = 2;

	/* Find the offset of the chars, there might be a scrollbar on the
	 * left of the window and/or a menu on the top (eterm etc.) */
	XQueryPointer(xterm_dpy, x11_window, &root, &child, &root_x, &root_y,
		      &win_x, &win_y, &mask_return);
	xterm_hints.y = win_y - (xterm_hints.height_inc * mouse_row)
			      - (xterm_hints.height_inc / 2);
	if (xterm_hints.y <= xterm_hints.height_inc / 2)
	    xterm_hints.y = 2;
	xterm_hints.x = win_x - (xterm_hints.width_inc * mouse_col)
			      - (xterm_hints.width_inc / 2);
	if (xterm_hints.x <= xterm_hints.width_inc / 2)
	    xterm_hints.x = 2;
	return TRUE;
    }
    if (mouse_code == NULL)
    {
	xterm_trace = 0;
	return FALSE;
    }

    XQueryPointer(xterm_dpy, x11_window, &root, &child, &root_x, &root_y,
		  &win_x, &win_y, &mask_return);

    row = check_row((win_y - xterm_hints.y) / xterm_hints.height_inc);
    col = check_col((win_x - xterm_hints.x) / xterm_hints.width_inc);
    if (row == prev_row && col == prev_col)
	return TRUE;

    STRCPY(buf, mouse_code);
    strp = buf + STRLEN(buf);
    *strp++ = (xterm_button | MOUSE_DRAG) & ~0x20;
    *strp++ = (char_u)(col + ' ' + 1);
    *strp++ = (char_u)(row + ' ' + 1);
    *strp = 0;
    add_to_input_buf(buf, STRLEN(buf));

    prev_row = row;
    prev_col = col;
    return TRUE;
}

/*
 * Destroy the display, window and app_context.  Required for GTK.
 */
    void
clear_xterm_clip()
{
    if (xterm_Shell != (Widget)0)
    {
	XtDestroyWidget(xterm_Shell);
	xterm_Shell = (Widget)0;
    }
    if (xterm_dpy != NULL)
    {
#if 0
	/* Lesstif and Solaris crash here, lose some memory */
	XtCloseDisplay(xterm_dpy);
#endif
	if (x11_display == xterm_dpy)
	    x11_display = NULL;
	xterm_dpy = NULL;
    }
#if 0
    if (app_context != (XtAppContext)NULL)
    {
	/* Lesstif and Solaris crash here, lose some memory */
	XtDestroyApplicationContext(app_context);
	app_context = (XtAppContext)NULL;
    }
#endif
}

/*
 * Catch up with any queued X events.  This may put keyboard input into the
 * input buffer, call resize call-backs, trigger timers etc.  If there is
 * nothing in the X event queue (& no timers pending), then we return
 * immediately.
 */
    static void
xterm_update()
{
    XEvent event;

    while (XtAppPending(app_context) && !vim_is_input_buf_full())
    {
	XtAppNextEvent(app_context, &event);
#ifdef FEAT_CLIENTSERVER
	{
	    XPropertyEvent *e = (XPropertyEvent *)&event;

	    if (e->type == PropertyNotify && e->window == commWindow
		   && e->atom == commProperty && e->state == PropertyNewValue)
		serverEventProc(xterm_dpy, &event);
	}
#endif
	XtDispatchEvent(&event);
    }
}

    int
clip_xterm_own_selection(cbd)
    VimClipboard *cbd;
{
    if (xterm_Shell != (Widget)0)
	return clip_x11_own_selection(xterm_Shell, cbd);
    return FAIL;
}

    void
clip_xterm_lose_selection(cbd)
    VimClipboard *cbd;
{
    if (xterm_Shell != (Widget)0)
	clip_x11_lose_selection(xterm_Shell, cbd);
}

    void
clip_xterm_request_selection(cbd)
    VimClipboard *cbd;
{
    if (xterm_Shell != (Widget)0)
	clip_x11_request_selection(xterm_Shell, xterm_dpy, cbd);
}

    void
clip_xterm_set_selection(cbd)
    VimClipboard *cbd;
{
    clip_x11_set_selection(cbd);
}
#endif

#ifdef EBCDIC
/* Translate character to its CTRL- value */
char CtrlTable[] =
{
/* 00 - 5E */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* ^ */ 0x1E,
/* - */ 0x1F,
/* 61 - 6C */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* _ */ 0x1F,
/* 6E - 80 */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* a */ 0x01,
/* b */ 0x02,
/* c */ 0x03,
/* d */ 0x37,
/* e */ 0x2D,
/* f */ 0x2E,
/* g */ 0x2F,
/* h */ 0x16,
/* i */ 0x05,
/* 8A - 90 */
	0, 0, 0, 0, 0, 0, 0,
/* j */ 0x15,
/* k */ 0x0B,
/* l */ 0x0C,
/* m */ 0x0D,
/* n */ 0x0E,
/* o */ 0x0F,
/* p */ 0x10,
/* q */ 0x11,
/* r */ 0x12,
/* 9A - A1 */
	0, 0, 0, 0, 0, 0, 0, 0,
/* s */ 0x13,
/* t */ 0x3C,
/* u */ 0x3D,
/* v */ 0x32,
/* w */ 0x26,
/* x */ 0x18,
/* y */ 0x19,
/* z */ 0x3F,
/* AA - AC */
	0, 0, 0,
/* [ */ 0x27,
/* AE - BC */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
/* ] */ 0x1D,
/* BE - C0 */ 0, 0, 0,
/* A */ 0x01,
/* B */ 0x02,
/* C */ 0x03,
/* D */ 0x37,
/* E */ 0x2D,
/* F */ 0x2E,
/* G */ 0x2F,
/* H */ 0x16,
/* I */ 0x05,
/* CA - D0 */ 0, 0, 0, 0, 0, 0, 0,
/* J */ 0x15,
/* K */ 0x0B,
/* L */ 0x0C,
/* M */ 0x0D,
/* N */ 0x0E,
/* O */ 0x0F,
/* P */ 0x10,
/* Q */ 0x11,
/* R */ 0x12,
/* DA - DF */ 0, 0, 0, 0, 0, 0,
/* \ */ 0x1C,
/* E1 */ 0,
/* S */ 0x13,
/* T */ 0x3C,
/* U */ 0x3D,
/* V */ 0x32,
/* W */ 0x26,
/* X */ 0x18,
/* Y */ 0x19,
/* Z */ 0x3F,
/* EA - FF*/ 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

char MetaCharTable[]=
{/*   0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F */
      0,  0,  0,  0,'\\', 0,'F',  0,'W','M','N',  0,  0,  0,  0,  0,
      0,  0,  0,  0,']',  0,  0,'G',  0,  0,'R','O',  0,  0,  0,  0,
    '@','A','B','C','D','E',  0,  0,'H','I','J','K','L',  0,  0,  0,
    'P','Q',  0,'S','T','U','V',  0,'X','Y','Z','[',  0,  0,'^',  0
};


/* TODO: Use characters NOT numbers!!! */
char CtrlCharTable[]=
{/*   0   1   2   3   4   5   6   7   8   9   A   B   C   D   E   F */
    124,193,194,195,  0,201,  0,  0,  0,  0,  0,210,211,212,213,214,
    215,216,217,226,  0,209,200,  0,231,232,  0,  0,224,189, 95,109,
      0,  0,  0,  0,  0,  0,230,173,  0,  0,  0,  0,  0,197,198,199,
      0,  0,229,  0,  0,  0,  0,196,  0,  0,  0,  0,227,228,  0,233,
};


#endif
