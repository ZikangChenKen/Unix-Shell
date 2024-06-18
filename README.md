# Unix-Shell
A simple Unix shell program that supports job control.

• The prompt should be the string “tsh> ”.

• The search path should be the string contained by the PATH environmental variable. The search path is a string that contains a list of directories, where each directory is separated by the “:” character. If the search path contains an empty string, this should be interpreted as the current directory being located in the search path. The search path can contain an empty string if the entire path is an empty string, if there are two “:” characters in a row, or if the path starts or ends with the “:” character. The search path should be loaded when the program is initialized.

• The command line typed by the user should consist of a name and zero or more arguments, all separated by one or more spaces. If name is a built-in command, then tsh should handle it immediately and wait for the next command line. Otherwise, tsh should assume that name is the name of an executable ﬁle, which it loads and runs in the context of an initial child process. This initial child process is called a job.

If name does not start with a directory and the search path is not NULL, then tsh should assume that name is the name of an executable ﬁle contained in one of the directories in the search path. In this case, tsh should search the directories in the search path, in order, for an executable named name. The ﬁrst executable named name found in the search path that tsh can execute is the program that tsh loads and runs. If name starts with a directory or the search path is NULL, tsh should assume that name is the path name of an executable ﬁle.

• tsh must use the execve function. In other words, tsh may not use any other member of the exec family, such as execvp.

• tsh need not support pipes (|) or I/O redirection (< and >).

• Typing ctrl-c (ctrl-z) should cause a SIGINT (SIGTSTP) signal to be sent to the current foreground job, as well as any descendents of that job (e.g., any child processes that it forked). If there is no foreground job, then the signal should have no effect.

• If the command line ends with an ampersand (&), then tsh should run the job in the background.

Otherwise, it should run the job in the foreground.

• Each job can be identiﬁed by either a process ID (PID) or a job ID (JID), which is a positive integer assigned by tsh. JIDs should be denoted on the command line by the preﬁx ‘%’. For example, “%5” denotes JID 5, and “5” denotes PID 5. (We have provided you with all of the routines you need for manipulating the job list.)

• tsh should support the following built-in commands:

– The quit command terminates the shell.

– The jobs command lists the stopped jobs and running background jobs.

– The bg <job> command restarts <job> by sending it a SIGCONT signal, and then runs it in the background. The <job> argument can be either a PID or a JID.

– The fg <job> command restarts <job> by sending it a SIGCONT signal, and then runs it in the foreground. The <job> argument can be either a PID or a JID.

• tsh should reap all of its zombie children. If any job terminates because it receives a signal that it didn’t catch, then tsh should recognize this event and print a message with the job’s PID and a description of the offending signal of the form

Job [1] (11639) terminated by signal SIGINT

Where the job ID is 1 and the process id is 11639 and the job terminated because of a SIGINT signal. The provided signame array is useful for converting signal numbers into names. Speciﬁcally, use the signal number as the index for selecting an element of the array. The element will be the address of a string containing the symbol for that signal, without the “SIG” preﬁx.
