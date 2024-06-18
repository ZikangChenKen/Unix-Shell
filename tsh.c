/*
 * COMP 321 Project 4: Shell
 *
 * This program implements a tiny shell with job control.
 *
 * Yuqi Chen yc138 Zikang Chen zc45
 */

#include <sys/types.h>
#include <sys/wait.h>

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// You may assume that these constants are large enough.
#define MAXLINE      1024   // max line size
#define MAXARGS       128   // max args on a command line
#define MAXJOBS        16   // max jobs at any point in time
#define MAXJID   (1 << 16)  // max job ID

// The job states are:
#define UNDEF 0 // undefined
#define FG 1    // running in foreground
#define BG 2    // running in background
#define ST 3    // stopped

/*
 * The job state transitions and enabling actions are:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most one job can be in the FG state.
 */

struct Job {
	pid_t pid;              // job PID
	int jid;                // job ID [1, 2, ...]
	int state;              // UNDEF, FG, BG, or ST
	char cmdline[MAXLINE];  // command line
};
typedef volatile struct Job *JobP;

/*
 * Define the jobs list using the "volatile" qualifier because it is accessed
 * by a signal handler (as well as the main program).
 */
static volatile struct Job jobs[MAXJOBS];
static int nextjid = 1;            // next job ID to allocate

extern char **environ;             // defined by libc

static char prompt[] = "tsh> ";    // command line prompt (DO NOT CHANGE)
static bool verbose = false;       // If true, print additional output.
static char ** path_array;
static int path_count;

/*
 * The following array can be used to map a signal number to its name.
 * This mapping is valid for x86(-64)/Linux systems, such as CLEAR.
 * The mapping for other versions of Unix, such as FreeBSD, Mac OS X, or
 * Solaris, differ!
 */
static const char *const signame[NSIG] = {
	"Signal 0",
	"HUP",				/* SIGHUP */
	"INT",				/* SIGINT */
	"QUIT",				/* SIGQUIT */
	"ILL",				/* SIGILL */
	"TRAP",				/* SIGTRAP */
	"ABRT",				/* SIGABRT */
	"BUS",				/* SIGBUS */
	"FPE",				/* SIGFPE */
	"KILL",				/* SIGKILL */
	"USR1",				/* SIGUSR1 */
	"SEGV",				/* SIGSEGV */
	"USR2",				/* SIGUSR2 */
	"PIPE",				/* SIGPIPE */
	"ALRM",				/* SIGALRM */
	"TERM",				/* SIGTERM */
	"STKFLT",			/* SIGSTKFLT */
	"CHLD",				/* SIGCHLD */
	"CONT",				/* SIGCONT */
	"STOP",				/* SIGSTOP */
	"TSTP",				/* SIGTSTP */
	"TTIN",				/* SIGTTIN */
	"TTOU",				/* SIGTTOU */
	"URG",				/* SIGURG */
	"XCPU",				/* SIGXCPU */
	"XFSZ",				/* SIGXFSZ */
	"VTALRM",			/* SIGVTALRM */
	"PROF",				/* SIGPROF */
	"WINCH",			/* SIGWINCH */
	"IO",				/* SIGIO */
	"PWR",				/* SIGPWR */
	"Signal 31"
};

// You must implement the following functions:

static bool	builtin_cmd(char **argv);
static void	do_bgfg(char **argv);
static void	eval(const char *cmdline);
static void	initpath(const char *pathstr);
static void	waitfg(pid_t pid);

static void	sigchld_handler(int signum);
static void	sigint_handler(int signum);
static void	sigtstp_handler(int signum);

// We are providing the following functions to you:

static bool	parseline(const char *cmdline, char **argv);

static void	sigquit_handler(int signum);

static bool	addjob(JobP jobs, pid_t pid, int state, const char *cmdline);
static void	clearjob(JobP job);
static bool	deletejob(JobP jobs, pid_t pid);
static pid_t	fgpid(JobP jobs);
static JobP	getjobjid(JobP jobs, int jid); 
static JobP	getjobpid(JobP jobs, pid_t pid);
static void	initjobs(JobP jobs);
static void	listjobs(JobP jobs);
static int	maxjid(JobP jobs); 
static int	pid2jid(pid_t pid); 

static void	app_error(const char *msg);
static void	unix_error(const char *msg);
static void	usage(void);

static void	Sio_error(const char s[]);
static ssize_t	Sio_putl(long v);
static ssize_t	Sio_puts(const char s[]);
static void	sio_error(const char s[]);
static void	sio_ltoa(long v, char s[], int b);
static ssize_t	sio_putl(long v);
static ssize_t	sio_puts(const char s[]);
static void	sio_reverse(char s[]);
static size_t	sio_strlen(const char s[]);

/*
 *   A shell is an interactive command-line interpreter that runs
 *   programs on behalf of the user. A shell repeatedly prints a
 *   prompt, waits for a command line on stdin, and then carries
 *   out some action, as directed by the contents of the command
 *   line. 
 *
 *
 * Requires:
 *   The command line is a sequence of ASCII text words delimited by 
 *   whitespace. The first word in the command line is either the name
 *   of a built-in command or the name of an executable file.
 *
 * Effects:
 *   Run the tiny shell program.
 */
int
main(int argc, char **argv) 
{
	struct sigaction action;
	int c;
	char cmdline[MAXLINE];
	char *path = NULL;
	bool emit_prompt = true;	// Emit a prompt by default.

	/*
	 * Redirect stderr to stdout (so that driver will get all output
	 * on the pipe connected to stdout).
	 */
	if (dup2(1, 2) < 0)
		unix_error("dup2 error");

	// Parse the command line.
	while ((c = getopt(argc, argv, "hvp")) != -1) {
		switch (c) {
		case 'h':             // Print a help message.
			usage();
			break;
		case 'v':             // Emit additional diagnostic info.
			verbose = true;
			break;
		case 'p':             // Don't print a prompt.
			// This is handy for automatic testing.
			emit_prompt = false;
			break;
		default:
			usage();
		}
	}

	/*
	 * Install sigint_handler() as the handler for SIGINT (ctrl-c).  SET
	 * action.sa_mask TO REFLECT THE SYNCHRONIZATION REQUIRED BY YOUR
	 * IMPLEMENTATION OF sigint_handler().
	 */
	action.sa_handler = sigint_handler;
	action.sa_flags = SA_RESTART;

	if (sigemptyset(&action.sa_mask) < 0)
		unix_error("sigemptyset error");


	if (sigaddset(&action.sa_mask, SIGCHLD) < 0){
		unix_error("sigaddset error");
	}
	if (sigaddset(&action.sa_mask, SIGTSTP) < 0){
		unix_error("sigaddset error");
	}
	if (sigaddset(&action.sa_mask, SIGINT) < 0){
		unix_error("sigaddset error");
	}



	if (sigaction(SIGINT, &action, NULL) < 0)
		unix_error("sigaction error");

	/*
	 * Install sigtstp_handler() as the handler for SIGTSTP (ctrl-z).  SET
	 * action.sa_mask TO REFLECT THE SYNCHRONIZATION REQUIRED BY YOUR
	 * IMPLEMENTATION OF sigtstp_handler().
	 */
	action.sa_handler = sigtstp_handler;
	action.sa_flags = SA_RESTART;

	if (sigaction(SIGTSTP, &action, NULL) < 0)
		unix_error("sigaction error");

	/*
	 * Install sigchld_handler() as the handler for SIGCHLD (terminated or
	 * stopped child).  SET action.sa_mask TO REFLECT THE SYNCHRONIZATION
	 * REQUIRED BY YOUR IMPLEMENTATION OF sigchld_handler().
	 */
	action.sa_handler = sigchld_handler;
	action.sa_flags = SA_RESTART;


	if (sigaction(SIGCHLD, &action, NULL) < 0)
		unix_error("sigaction error");

	/*
	 * Install sigquit_handler() as the handler for SIGQUIT.  This handler
	 * provides a clean way for the test harness to terminate the shell.
	 * Preemption of the processor by the other signal handlers during
	 * sigquit_handler() does no harm, so action.sa_mask is set to empty.
	 */
	action.sa_handler = sigquit_handler;
	action.sa_flags = SA_RESTART;
	if (sigemptyset(&action.sa_mask) < 0)
		unix_error("sigemptyset error");

	if (sigaction(SIGQUIT, &action, NULL) < 0)
		unix_error("sigaction error");

	// Initialize the search path.
	path = getenv("PATH");
	initpath(path);

	// Initialize the jobs list.
	initjobs(jobs);

	// Execute the shell's read/eval loop.
	while (true) {

		// Read the command line.
		if (emit_prompt) {
			printf("%s", prompt);
			fflush(stdout);
		}
		if (fgets(cmdline, MAXLINE, stdin) == NULL && ferror(stdin))
			app_error("fgets error");
		if (feof(stdin)) // End of file (ctrl-d)
			exit(0);

		// Evaluate the command line.
		eval(cmdline);
		fflush(stdout);
	}

	// Control never reaches here.
	assert(false);
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in.
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately.  Otherwise, fork a child process and
 * run the job in the context of the child.  If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
 *
 * Requires:
 *   "cmdline" is a NUL ('\0') terminated string with a trailing
 *   '\n' character.  "cmdline" must contain less than MAXARGS
 *   arguments.
 *
 * Effects:
 *   Main routine that parses and interprets the command line.
 */
static void
eval(const char *cmdline) 
{
	char *argv[MAXARGS], *dir_path, *full_path;	/* Argument list execve() */ 
	char buf[MAXLINE]; 		/* Holds modified command line */ 
	int bg; 				/* Should the job run in bg or fg? */ 
	pid_t pid; 				/* Process id */

	sigset_t mask;      /* Block all signals */
    sigset_t mask_one;      /* Block SIGCHLD */
    sigset_t prev_mask;      /* Previous blocked */

    strcpy(buf, cmdline);
    bg = parseline(buf, argv);
    if (argv[0] == NULL)
        return;             /* Ignore empty lines */

    if (!builtin_cmd(argv)) {
		if (sigfillset(&mask) < 0){
			unix_error("sigfillset error");
		}
		if (sigemptyset(&mask_one) < 0){
			unix_error("sigemptyset error");
		}
		if (sigaddset(&mask_one, SIGCHLD) < 0){
			unix_error("sigaddset error");
		}
        if (sigprocmask(SIG_BLOCK, &mask_one, &prev_mask) < 0){
			unix_error("sigprocmask error");
		}

		/* Runs in the context of a child process. */
        if ((pid = fork()) == 0) {   
			/* Restore mask for child. */
            if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
				unix_error("sigprocmask error");
			}  
            if (setpgid(0, 0) < 0){
				unix_error("setpgid error");
			}

			if (strchr(argv[0], '/') == NULL && path_array != NULL){
				for (int idx = 0; idx < path_count; idx++){
					/*Get the full direction of the path from path_array. */
					dir_path = path_array[idx];
					full_path =
						malloc(strlen(dir_path) + 1 + strlen(argv[0]) + 1);
					full_path[0] = '\0';
					strcat(strcat(strcat(full_path, dir_path), "/"), argv[0]);
					execve(full_path, argv, environ);
					free(full_path);
				}

			} else{
				execve(argv[0], argv, environ);
			}

			printf("%s: Command not found.\n", argv[0]);
			exit(0);
		} else if (pid < 0){
			unix_error("folk error");
		}


        if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0){
				unix_error("sigprocmask error");
		}

        /* Parent waits for foreground job to terminate. */
        if (!bg) {
            /* Add jobs. */

			addjob(jobs, pid, FG, cmdline);

			
            if (sigprocmask(SIG_SETMASK, &prev_mask, NULL)){
				unix_error("sigprocmask error");
			}
            /* Wait for job to finish. */
            waitfg(pid);
        } else {
            /* Add jobs. */


			addjob(jobs, pid, BG, cmdline);

            if (sigprocmask(SIG_SETMASK, &prev_mask, NULL)){
				unix_error("sigprocmask error");
			}
			//print summary
            
            printf("[%d] (%d) %s", pid2jid(pid),
					pid, (char *)(getjobpid(jobs, pid) -> cmdline));
        }
    }    
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 *
 * Requires:
 *   "cmdline" is a NUL ('\0') terminated string with a trailing
 *   '\n' character.  "cmdline" must contain less than MAXARGS
 *   arguments.
 *
 * Effects:
 *   Builds "argv" array from space delimited arguments on the command line.
 *   The final element of "argv" is set to NULL.  Characters enclosed in
 *   single quotes are treated as a single argument.  Returns true if
 *   the user has requested a BG job and false if the user has requested
 *   a FG job.
 *
 * Note:
 *   In the textbook, this function has the return type "int", but "bool"
 *   is more appropriate.
 */
static bool
parseline(const char *cmdline, char **argv) 
{
	int argc;                   // number of args
	static char array[MAXLINE]; // local copy of command line
	char *buf = array;          // ptr that traverses command line
	char *delim;                // points to first space delimiter
	bool bg;                    // background job?

	strcpy(buf, cmdline);

	// Replace trailing '\n' with space.
	buf[strlen(buf) - 1] = ' ';

	// Ignore leading spaces.
	while (*buf != '\0' && *buf == ' ')
		buf++;

	// Build the argv list.
	argc = 0;
	if (*buf == '\'') {
		buf++;
		delim = strchr(buf, '\'');
	} else
		delim = strchr(buf, ' ');
	while (delim != NULL) {
		argv[argc++] = buf;
		*delim = '\0';
		buf = delim + 1;
		while (*buf != '\0' && *buf == ' ')	// Ignore spaces.
			buf++;
		if (*buf == '\'') {
			buf++;
			delim = strchr(buf, '\'');
		} else
			delim = strchr(buf, ' ');
	}
	argv[argc] = NULL;

	// Ignore blank line.
	if (argc == 0)
		return (true);

	// Should the job run in the background?
	if ((bg = (*argv[argc - 1] == '&')) != 0)
		argv[--argc] = NULL;

	return (bg);
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *  it immediately.  
 *
 * Requires:
 *   "argv" is an argument list execve().
 *
 * Effects:
 *   Recognizes and interprets the built-in commands: quit, fg, bg, 
 * 	 and jobs.
 *
 * Note:
 *   In the textbook, this function has the return type "int", but "bool"
 *   is more appropriate.
 */
static bool
builtin_cmd(char **argv) 
{
	if (strcmp(argv[0], "quit") == 0) {
		// Terminate.
		exit(0);
	} else if (strcmp(argv[0], "fg") == 0 || strcmp(argv[0], "bg") == 0) {
		// Do the switch.
		do_bgfg(argv);
		return true;
	} else if (strcmp(argv[0], "jobs") == 0) {
		listjobs(jobs);
		return true;
	}

	// Not a built-in command.
	return false; 
}

/* 
 * do_bgfg - Execute the built-in bg and fg commands.
 *
 * Requires:
 *   "argv" is an argument list execve().
 *
 * Effects:
 *   Implements the bg and fg built-in commands.
 */
static void
do_bgfg(char **argv) 
{
	JobP job;
	sigset_t mask, prev_mask;
	
	char *id = argv[1];
	char cmdline[MAXLINE];

	if (sigfillset(&mask) < 0){
		unix_error("sigfillset error");
	}
	if (sigprocmask(SIG_BLOCK, &mask, &prev_mask) < 0){
		unix_error("sigprocmask error");
	}
	if (id == NULL){
		printf("PID error\n");
		if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
			unix_error("sigprocmask error");
		}
		return;
	}

	if (argv[1][0] == '%') {
        // Find job by jid.
        job = getjobjid(jobs, atoi(&(argv[1][1])));
        if (job == NULL) {
            printf("No such job\n");
            if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
				unix_error("sigprocmask error");
			}
            return;
        }
    } else if (isdigit(argv[1][0])) {
        // Find job by pid.
        job = getjobpid(jobs, atoi(argv[1]));
        if (job == NULL) {
			printf("No such process: %s\n", argv[1]);
            if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
				unix_error("sigprocmask error");
			}
            return;
        }
    } else {
        // Invalid input. 
        printf("fg: argument must be a PID or %jobid\n");
        if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
			unix_error("sigprocmask error");
		}
        return;
    }

    // Collect data.
    strcpy(cmdline, (char *)(job -> cmdline));
    
    // kill(-job -> pid, SIGCONT);
    if (strcmp(argv[0], "fg") == 0) {
        job -> state = FG;
        if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
			unix_error("sigprocmask error");
		}
		
		kill(-job -> pid, SIGCONT);
        waitfg(job -> pid);
    } else {
        job -> state = BG;
        if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
			unix_error("sigprocmask error");
		}
		printf("[%d] (%d)%s", job -> jid, job -> pid, job -> cmdline);
		kill(-job -> pid, SIGCONT);
    }
    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process.
 *
 * Requires:
 *   "pid" is a valid integer.
 *
 * Effects:
 *   Block until process pid is no longer the foreground process.
 */
static void
waitfg(pid_t pid)
{
	//sigset_t mask, prev;
	sigset_t mask;
	if (sigemptyset(&mask) < 0){
		unix_error("sigemptyset error");
	}
	while (pid == fgpid(jobs)){
		sigsuspend(&mask);
	}
	return;
}

/* 
 * initpath - Perform all necessary initialization of the search path,
 *  which may be simply saving the path.
 *
 * Requires:
 *   "pathstr" is a valid search path.
 *
 * Effects:
 *   Allocate the address of each search path contained in environment
 *   variables into a global array called path-array.
 */
static void
initpath(const char *pathstr) {
	char *delim;                // points to first space delimiter
	int argc = 0;
	path_count = 1;

	// Count the number of directories. 
	char *header = (char *)pathstr;
	while (*header != '\0'){
		if (*header == ':'){
			path_count++;
		}
		header++;
	}

	// Need to add 1 because of the Terminating NULL Pointer. 
	path_array = malloc((path_count + 1) * sizeof(char *));  

	// Check memory allocation.
	if (path_array == NULL){
		Sio_puts("Memory error.\n");
		_exit(1);
	}

	// Copy the pathstr.
	char *pathstr_copy = malloc((strlen(pathstr) + 1) * sizeof(char));
	if (pathstr_copy == NULL){
		Sio_puts("Memory error");
		_exit(1);
	}
	strcpy(pathstr_copy, pathstr);

	// Start copy.
	while ((delim = strchr(pathstr_copy, ':'))){
		*delim = '\0';
		
        if (pathstr_copy && (!pathstr_copy[0])) {
           path_array[argc++] = ".";
        } else {
           path_array[argc++] = pathstr_copy;
        }
        pathstr_copy = delim + 1;
    }

	// Adding the final path string.
	if (!pathstr_copy[0]) {
       	path_array[argc++] = ".";
   	} else {
       	path_array[argc++] = pathstr_copy;
   	}
   	path_array[argc] = NULL;
}

/*
 * The signal handlers follow.
 */

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *  a child job terminates (becomes a zombie), or stops because it
 *  received a SIGSTOP or SIGTSTP signal.  The handler reaps all
 *  available zombie children, but doesn't wait for any other
 *  currently running children to terminate.  
 *
 * Requires:
 *   "signum" is SIGCHILD, SIGTSTP, or SIGINT.
 *
 * Effects:
 *   Receive the terminal signal from input device or file. Decide the
 *   type of the termination: normally termination, stop, or interuption.
 */
static void
sigchld_handler(int signum)
{
	(void) signum;
	int olderrno = errno;
    sigset_t mask, prev_mask;
    sigfillset(&mask);
    
    pid_t pid; 
    int status;

	// Wait for the termination of child processes.
    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        if (WIFEXITED(status)) {             
            // Terminated normally.
            if (sigprocmask(SIG_BLOCK, &mask, &prev_mask) < 0) {
				unix_error("sigprocmask error");
			}
			getjobpid(jobs, pid) -> state = UNDEF;

            deletejob(jobs, pid);

            if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
				unix_error("sigprocmask error");
			}
        } else if (WIFSTOPPED(status)) {    
            // The child that caused the return is currently stopped.
            if (sigprocmask(SIG_BLOCK, &mask, &prev_mask) < 0) {
				unix_error("sigprocmask error");
			}
            JobP stop_job = getjobpid(jobs, pid);
            stop_job -> state = ST;
            Sio_puts("Job [");
            Sio_putl(pid2jid(pid));
            Sio_puts("] (");
            Sio_putl(pid);
            Sio_puts(") stopped by signal SIGTSTP\n");
            if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
				unix_error("sigprocmask error");
			}
        } else if (WIFSIGNALED(status)) {    
            // Terminated because of a signal that was not caught.
        	if (sigprocmask(SIG_BLOCK, &mask, &prev_mask) < 0) {
				unix_error("sigprocmask error");
			}

			Sio_puts("Job [");
            Sio_putl(pid2jid(pid));
            Sio_puts("] (");
            Sio_putl(pid);
            Sio_puts(") terminated by signal SIGINT\n");
			
            deletejob(jobs, pid);
            if (sigprocmask(SIG_SETMASK, &prev_mask, NULL) < 0){
				unix_error("sigprocmask error");
			}
            
        } else {                            
            assert(false);
        }
    }

    errno = olderrno;
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *  user types ctrl-c at the keyboard.  Catch it and send it along
 *  to the foreground job.  
 *
 * Requires:
 *   "signum" is SIGINT.
 *
 * Effects:
 *   Terminal the program.
 */
static void
sigint_handler(int signum)
{
    pid_t pid;

    if ((pid = fgpid(jobs)) > 0) {
        kill(-pid, signum);
    }

    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *  the user types ctrl-z at the keyboard.  Catch it and suspend the
 *  foreground job by sending it a SIGTSTP.  
 *
 * Requires:
 *   "signum" is SIGTSTP.
 *
 * Effects:
 *   Suspend the program.
 */
static void
sigtstp_handler(int signum)
{
    pid_t pid;

    if ((pid = fgpid(jobs)) > 0) {
        kill(-pid, signum);
    }
	
    return;
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *  child shell by sending it a SIGQUIT signal.
 *
 * Requires:
 *   "signum" is SIGQUIT.
 *
 * Effects:
 *   Terminates the program.
 */
static void
sigquit_handler(int signum)
{

	// Prevent an "unused parameter" warning.
	(void)signum;
	Sio_puts("Terminating after receipt of SIGQUIT signal\n");
	_exit(1);
}

/*
 * This comment marks the end of the signal handlers.
 */

/*
 * The following helper routines manipulate the jobs list.
 */

/*
 * Requires:
 *   "job" points to a job structure.
 *
 * Effects:
 *   Clears the fields in the referenced job structure.
 */
static void
clearjob(JobP job)
{

	job->pid = 0;
	job->jid = 0;
	job->state = UNDEF;
	job->cmdline[0] = '\0';
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Initializes the jobs list to an empty state.
 */
static void
initjobs(JobP jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		clearjob(&jobs[i]);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns the largest allocated job ID.
 */
static int
maxjid(JobP jobs) 
{
	int i, max = 0;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid > max)
			max = jobs[i].jid;
	return (max);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures, and "cmdline" is
 *   a properly terminated string.
 *
 * Effects: 
 *   Tries to add a job to the jobs list.  Returns true if the job was added
 *   and false otherwise.
 */
static bool
addjob(JobP jobs, pid_t pid, int state, const char *cmdline)
{
	int i;
    
	if (pid < 1)
		return (false);
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == 0) {
			jobs[i].pid = pid;
			jobs[i].state = state;
			jobs[i].jid = nextjid++;
			if (nextjid > MAXJOBS)
				nextjid = 1;
			// Remove the "volatile" qualifier using a cast.
			strcpy((char *)jobs[i].cmdline, cmdline);
			if (verbose) {
				printf("Added job [%d] %d %s\n", jobs[i].jid,
				    (int)jobs[i].pid, jobs[i].cmdline);
			}
			return (true);
		}
	}
	printf("Tried to create too many jobs\n");
	return (false);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Tries to delete the job from the jobs list whose PID equals "pid".
 *   Returns true if the job was deleted and false otherwise.
 */
static bool
deletejob(JobP jobs, pid_t pid) 
{
	int i;

	if (pid < 1)
		return (false);
	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid == pid) {
			clearjob(&jobs[i]);
			nextjid = maxjid(jobs) + 1;
			return (true);
		}
	}
	return (false);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns the PID of the current foreground job or 0 if no foreground
 *   job exists.
 */
static pid_t
fgpid(JobP jobs)
{
	int i;

	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].state == FG)
			return (jobs[i].pid);
	return (0);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns a pointer to the job structure with process ID "pid" or NULL if
 *   no such job exists.
 */
static JobP
getjobpid(JobP jobs, pid_t pid)
{
	int i;

	if (pid < 1)
		return (NULL);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return (&jobs[i]);
	return (NULL);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Returns a pointer to the job structure with job ID "jid" or NULL if no
 *   such job exists.
 */
static JobP
getjobjid(JobP jobs, int jid) 
{
	int i;

	if (jid < 1)
		return (NULL);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].jid == jid)
			return (&jobs[i]);
	return (NULL);
}

/*
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Returns the job ID for the job with process ID "pid" or 0 if no such
 *   job exists.
 */
static int
pid2jid(pid_t pid) 
{
	int i;

	if (pid < 1)
		return (0);
	for (i = 0; i < MAXJOBS; i++)
		if (jobs[i].pid == pid)
			return (jobs[i].jid);
	return (0);
}

/*
 * Requires:
 *   "jobs" points to an array of MAXJOBS job structures.
 *
 * Effects:
 *   Prints the jobs list.
 */
static void
listjobs(JobP jobs) 
{
	int i;

	for (i = 0; i < MAXJOBS; i++) {
		if (jobs[i].pid != 0) {
			printf("[%d] (%d) ", jobs[i].jid, (int)jobs[i].pid);
			switch (jobs[i].state) {
			case BG: 
				printf("Running ");
				break;
			case FG: 
				printf("Foreground ");
				break;
			case ST: 
				printf("Stopped ");
				break;
			default:
				printf("listjobs: Internal error: "
				    "job[%d].state=%d ", i, jobs[i].state);
			}
			printf("%s", jobs[i].cmdline);
		}
	}
}

/*
 * This comment marks the end of the jobs list helper routines.
 */

/*
 * Other helper routines follow.
 */

/*
 * Requires:
 *   Nothing.
 *
 * Effects:
 *   Prints a help message.
 */
static void
usage(void) 
{

	printf("Usage: shell [-hvp]\n");
	printf("   -h   print this message\n");
	printf("   -v   print additional diagnostic information\n");
	printf("   -p   do not emit a command prompt\n");
	exit(1);
}

/*
 * Requires:
 *   "msg" is a properly terminated string.
 *
 * Effects:
 *   Prints a Unix-style error message and terminates the program.
 */
static void
unix_error(const char *msg)
{

	fprintf(stdout, "%s: %s\n", msg, strerror(errno));
	exit(1);
}

/*
 * Requires:
 *   "msg" is a properly terminated string.
 *
 * Effects:
 *   Prints "msg" and terminates the program.
 */
static void
app_error(const char *msg)
{

	fprintf(stdout, "%s\n", msg);
	exit(1);
}

/*
 * Requires:
 *   The character array "s" is sufficiently large to store the ASCII
 *   representation of the long "v" in base "b".
 *
 * Effects:
 *   Converts a long "v" to a base "b" string, storing that string in the
 *   character array "s" (from K&R).  This function can be safely called by
 *   a signal handler.
 */
static void
sio_ltoa(long v, char s[], int b)
{
	int c, i = 0;

	do
		s[i++] = (c = v % b) < 10 ? c + '0' : c - 10 + 'a';
	while ((v /= b) > 0);
	s[i] = '\0';
	sio_reverse(s);
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Reverses a string (from K&R).  This function can be safely called by a
 *   signal handler.
 */
static void
sio_reverse(char s[])
{
	int c, i, j;

	for (i = 0, j = sio_strlen(s) - 1; i < j; i++, j--) {
		c = s[i];
		s[i] = s[j];
		s[j] = c;
	}
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Computes and returns the length of the string "s".  This function can be
 *   safely called by a signal handler.
 */
static size_t
sio_strlen(const char s[])
{
	size_t i = 0;

	while (s[i] != '\0')
		i++;
	return (i);
}

/*
 * Requires:
 *   None.
 *
 * Effects:
 *   Prints the long "v" to stdout using only functions that can be safely
 *   called by a signal handler, and returns either the number of characters
 *   printed or -1 if the long could not be printed.
 */
static ssize_t
sio_putl(long v)
{
	char s[128];
    
	sio_ltoa(v, s, 10);
	return (sio_puts(s));
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler, and returns either the number of characters
 *   printed or -1 if the string could not be printed.
 */
static ssize_t
sio_puts(const char s[])
{

	return (write(STDOUT_FILENO, s, sio_strlen(s)));
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler, and exits the program.
 */
static void
sio_error(const char s[])
{

	sio_puts(s);
	_exit(1);
}

/*
 * Requires:
 *   None.
 *
 * Effects:
 *   Prints the long "v" to stdout using only functions that can be safely
 *   called by a signal handler.  Either returns the number of characters
 *   printed or exits if the long could not be printed.
 */
static ssize_t
Sio_putl(long v)
{
	ssize_t n;
  
	if ((n = sio_putl(v)) < 0)
		sio_error("Sio_putl error");
	return (n);
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler.  Either returns the number of characters
 *   printed or exits if the string could not be printed.
 */
static ssize_t
Sio_puts(const char s[])
{
	ssize_t n;
  
	if ((n = sio_puts(s)) < 0)
		sio_error("Sio_puts error");
	return (n);
}

/*
 * Requires:
 *   "s" is a properly terminated string.
 *
 * Effects:
 *   Prints the string "s" to stdout using only functions that can be safely
 *   called by a signal handler, and exits the program.
 */
static void
Sio_error(const char s[])
{

	sio_error(s);
}

// Prevent "unused function" and "unused variable" warnings.
static const void *dummy_ref[] = { Sio_error, Sio_putl, addjob, builtin_cmd,
    deletejob, do_bgfg, dummy_ref, fgpid, getjobjid, getjobpid, listjobs,
    parseline, pid2jid, signame, waitfg };
