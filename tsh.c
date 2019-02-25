
/* 
 * tsh - A tiny shell program with job control
 * <The line above is not a sufficient documentation.
 *  You will need to write your program documentation.>
 */

#include "tsh_helper.h"

/*
 * If DEBUG is defined, enable contracts and printing on dbg_printf.
 */
#ifdef DEBUG
/* When debugging is enabled, these form aliases to useful functions */
#define dbg_printf(...) printf(__VA_ARGS__)
#define dbg_requires(...) assert(__VA_ARGS__)
#define dbg_assert(...) assert(__VA_ARGS__)
#define dbg_ensures(...) assert(__VA_ARGS__)
#else
/* When debugging is disabled, no code gets generated for these */
#define dbg_printf(...)
#define dbg_requires(...)
#define dbg_assert(...)
#define dbg_ensures(...)
#endif

/* Function prototypes */
void eval(const char *cmdline);
void set_block();
void set_unblock();
/*
 * updatejob updates the job with the supplied process ID from the job list.
 * It returns true if successful and false if no job with this pid is found.
 */
bool updatejob(pid_t pid, int status, int options);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);
void sigquit_handler(int sig);



/*
 * <Write main's function header documentation. What does main do?>
 * "Each function should be prefaced with a comment describing the purpose
 *  of the function (in a sentence or two), the function's arguments and
 *  return value, any error cases that are relevant to the caller,
 *  any pertinent side effects, and any assumptions that the function makes."
 */
int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE_TSH];  // Cmdline for fgets
    bool emit_prompt = true;    // Emit prompt (default)

    // Redirect stderr to stdout (so that driver will get all output
    // on the pipe connected to stdout)
    Dup2(STDOUT_FILENO, STDERR_FILENO);

    // Parse the command line
    while ((c = getopt(argc, argv, "hvp")) != EOF)
    {
        switch (c)
        {
        case 'h':                   // Prints help message
            usage();
            break;
        case 'v':                   // Emits additional diagnostic info
            verbose = true;
            break;
        case 'p':                   // Disables prompt printing
            emit_prompt = false;  
            break;
        default:
            usage();
        }
    }

    // Install the signal handlers
    Signal(SIGINT,  sigint_handler);   // Handles ctrl-c
    Signal(SIGTSTP, sigtstp_handler);  // Handles ctrl-z
    Signal(SIGCHLD, sigchld_handler);  // Handles terminated or stopped child

    Signal(SIGTTIN, SIG_IGN);
    Signal(SIGTTOU, SIG_IGN);

    Signal(SIGQUIT, sigquit_handler); 

    // Initialize the job list
    initjobs(job_list);

    // Execute the shell's read/eval loop
    while (true)
    {
        if (emit_prompt)
        {
            printf("%s", prompt);
            fflush(stdout);
        }

        if ((fgets(cmdline, MAXLINE_TSH, stdin) == NULL) && ferror(stdin))
        {
            app_error("fgets error");
        }

        if (feof(stdin))
        { 
            // End of file (ctrl-d)
            printf ("\n");
            fflush(stdout);
            fflush(stderr);
            return 0;
        }
        
        // Remove the trailing newline
        cmdline[strlen(cmdline)-1] = '\0';
        
        // Evaluate the command line
        eval(cmdline);
        
        fflush(stdout);
    } 
    
    return -1; // control never reaches here
}


/* Handy guide for eval:
 *
 * If the user has requested a built-in command (quit, jobs, bg or fg),
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.
 * Note: each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.
 */

/* 
 * <What does eval do?>
 */
void eval(const char *cmdline) 
{
    parseline_return parse_result;     
    struct cmdline_tokens token;
    //sigset_t ourmask;
    // TODO: remove the line below! It's only here to keep the compiler happy
    //Sigemptyset(&ourmask);

    // Parse command line
    parse_result = parseline(cmdline, &token);

    if (parse_result == PARSELINE_ERROR || parse_result == PARSELINE_EMPTY)
    {
        return;
    } else if (parse_result == PARSELINE_BG) // background job requested
    {
        if (token.builtin == BUILTIN_NONE) // not built-in cmd
        {
            set_block();
            pid_t pid = Fork();
            #ifdef DEBUG
            printf("PID post-fork: %d\n", pid);
            #endif
            if (pid < 0) // failed fork
            {
                printf("Error: fork failed.\n");
            } else if (!pid) // child process
            {
                set_unblock();
                Signal(SIGINT, SIG_DFL);
                Signal(SIGCHLD, SIG_DFL);
                Signal(SIGTSTP, SIG_DFL);
                Setpgid(0, 0);
                Execve(token.argv[0], token.argv, environ);
                exit(0);
            } else // parent process
            {
                addjob(&job_list[0], pid, BG, cmdline);
                struct job_t *job = getjobpid(&job_list[0], pid);
                printf("[%d] (%d) %s\n", job->jid, job->pid, job->cmdline);
                set_unblock();
            }
            // TODO: Reap zombies
            return;
        }
    } else if (parse_result == PARSELINE_FG) // foreground job requested
    {
        if (token.builtin == BUILTIN_NONE) // not built-in cmd
        {
            set_block();
            pid_t pid = Fork();
            #ifdef DEBUG
            printf("PID post-fork: %d\n", pid);
            #endif
            if (pid < 0) // failed fork
            {
                printf("Error: fork failed.\n");
            } else if (!pid) // child process
            {
                set_unblock();
                Signal(SIGINT, SIG_DFL);
                Signal(SIGCHLD, SIG_DFL);
                Signal(SIGTSTP, SIG_DFL);
                Setpgid(0, 0);
                Execve(token.argv[0], token.argv, environ);
                exit(0);
            } else // parent process
            {
                addjob(&job_list[0], pid, FG, cmdline);
                set_unblock();
                int status;
                waitpid(pid, &status, WUNTRACED);
                updatejob(pid, status, 0);
            }
        } else if (token.builtin == BUILTIN_JOBS) // built-in jobs cmd
        {
            set_block();
            listjobs(&job_list[0], 1);
            set_unblock();
        } else if (token.builtin == BUILTIN_QUIT) // built-in quit cmd
        {
            raise(SIGQUIT);
        } else if (token.builtin == BUILTIN_BG) // built-in bg cmd
        {
            set_block();
            char *argument = token.argv[1];
            if (argument[0] == '%') {
                memmove(argument, argument+1, strlen(argument));
                struct job_t *job = getjobjid(&job_list[0], atoi(argument));
                updatejob(job->pid, 0, 1);
                set_unblock();
            } else {
                int pid = atoi(argument);
                updatejob((pid_t) pid, 0, 1);
                set_unblock();
            }
        } else if (token.builtin == BUILTIN_FG) // built-in fg cmd
        {
            set_block();
            char *argument = token.argv[1];
            if (argument[0] == '%') {
                memmove(argument, argument+1, strlen(argument));
                struct job_t *job = getjobjid(&job_list[0], atoi(argument));
                updatejob(job->pid, 0, 2);
                set_unblock();
            } else {
                int pid = atoi(argument);
                updatejob((pid_t) pid, 0, 2);
                set_unblock();
            }
        }
        return;
    }
}

void set_block() 
{
    sigset_t sigset;
    Sigemptyset(&sigset);
    Sigaddset(&sigset, SIGCHLD);
    Sigaddset(&sigset, SIGINT);
    Sigaddset(&sigset, SIGTSTP);
    Sigprocmask(SIG_BLOCK, &sigset, NULL);
}

void set_unblock() 
{
    sigset_t sigset;
    Sigemptyset(&sigset);
    Sigaddset(&sigset, SIGCHLD);
    Sigaddset(&sigset, SIGINT);
    Sigaddset(&sigset, SIGTSTP);
    Sigprocmask(SIG_UNBLOCK, &sigset, NULL);
    // Sigsuspend(&sigset);
}

/*****************
 * Signal handlers
 *****************/

/* 
 * <What does sigchld_handler do?>
 */
void sigchld_handler(int sig) 
{
    // IMPLEMENTglobal bool variable that indicates whether job was bg or fg and only reap 
    /*pid_t pid;
    pid = wait(NULL);
    printf("PID %d exited.\n", pid);
    signal(SIGCHLD, sigchld_handler);*/
    return;
}

/* 
 * <What does sigint_handler do?>
 */
void sigint_handler(int sig) 
{
    set_block();
    int pid = fgpid(job_list); // get foreground pid
    int group_id = __getpgid(pid); // get group id
    if (group_id != getpid()) {
        kill(-group_id, SIGINT);
    }
    set_unblock();
    return;
}

/*
 * <What does sigtstp_handler do?>
 */
void sigtstp_handler(int sig) 
{
    set_block();
    int pid = fgpid(job_list); // get foreground pid
    int group_id = __getpgid(pid); // get group id
    if (group_id != getpid()) {
        kill(-group_id, SIGTSTP);
    }
    set_unblock();
    return;
}

// void reap() 
// {
//     int status;
//     pid_t pid = waitpid() after the job list is updated
// }

bool updatejob(pid_t pid, int status, int options)
{
    set_block();
    struct job_t *job = getjobpid(&job_list[0], pid);
    if (WIFSIGNALED(status) || WIFSTOPPED(status))
    {
        if (WIFSIGNALED(status) /*&& WTERMSIG(status) > 0*/)
        {
            printf("Job [%d] (%d) terminated by signal 2\n", pid2jid(&job_list[0], pid), pid);
            deletejob(&job_list[0], pid);
            set_unblock();
        } else if (WIFSTOPPED(status))
        {
            printf("Job [%d] (%d) stopped by signal 20\n", pid2jid(&job_list[0], pid), pid);
            job->state = ST;
            set_unblock();
        } 
    } else if (options == 1) // resume background job that was prev stopped
    {
        job->state = BG;
        printf("[%d] (%d) %s\n", job->jid, job->pid, job->cmdline);
        set_unblock();
    } else if (options == 2) // resume background job in foreground that was prev stopped
    {
        if (!fgpid(&job_list[0])) {
            job->state = FG;
            int group_id = __getpgid(job->pid);
            kill(-group_id, SIGCONT);
            wait(NULL);
        }
    } else 
    {
        deletejob(&job_list[0], pid);
        set_unblock();
    }
    return false;
}