#include "main.h"

using namespace std;

int sockServer;
int Connect::serverSocket;

int read_conf_file(const char *path_conf);
int create_server_socket(const Config *conf);
int unix_socket_pair(int sock[2]);
int get_sock_buf(int domain, int optname, int type, int protocol);

void free_fcgi_list();
int set_uid();
int main_proc();
void create_proc(int);

static string confPath;
static string pidFile;

static pid_t pidChild[PROC_LIMIT];
static int unixFD[PROC_LIMIT][2];
static int numConn[PROC_LIMIT];
static int startServer = 0, restartServer = 1;
static int closeChldProc = 0;
static unsigned int allConn = 0;
static unsigned int numCreatedProc = 0;
//======================================================================
static void signal_handler(int sig)
{
    if (sig == SIGINT)
    {
        fprintf(stderr, "<%s> ###### SIGINT ######\n", __func__);
        shutdown(sockServer, SHUT_RDWR);
        close(sockServer);
        closeChldProc = 1;
    }
    else if (sig == SIGSEGV)
    {
        fprintf(stderr, "<%s> ###### SIGSEGV ######\n", __func__);
        shutdown(sockServer, SHUT_RDWR);
        close(sockServer);

        for (unsigned int i = 0; i < numCreatedProc; ++i)
        {
            if (pidChild[i] > 0)
                kill(pidChild[i], SIGKILL);
        }

        pid_t pid;
        while ((pid = wait(NULL)) != -1)
        {
            fprintf(stderr, "<%s> wait() pid: %d\n", __func__, pid);
        }
        exit(1);
    }
    else if (sig == SIGTERM)
    {
        print_err("<%s> ###### SIGTERM ######\n", __func__);
        shutdown(sockServer, SHUT_RDWR);
        close(sockServer);

        for (unsigned int i = 0; i < numCreatedProc; ++i)
        {
            if (pidChild[i] > 0)
                kill(pidChild[i], SIGKILL);
        }

        pid_t pid;
        while ((pid = wait(NULL)) != -1)
        {
            fprintf(stderr, "<%s> wait() pid: %d\n", __func__, pid);
        }
        exit(0);
    }
    else if (sig == SIGUSR1)
    {
        fprintf(stderr, "<%s> ###### SIGUSR1 ######\n", __func__);
        restartServer = 1;
        closeChldProc = 1;
    }
    else if (sig == SIGUSR2)
    {
        fprintf(stderr, "<%s> ###### SIGUSR2 ######\n", __func__);
        closeChldProc = 1;
    }
    else
    {
        fprintf(stderr, "<%s> ? sig=%d\n", __func__, sig);
    }
}
//======================================================================
pid_t create_child(int, int *, int);
//======================================================================
void create_proc(unsigned int NumProc, int from_chld[2], int *sndbuf)
{
    if (pipe(from_chld) < 0)
    {
        fprintf(stderr, "<%s:%d> Error pipe(): %s\n", __func__, __LINE__, strerror(errno));
        exit(1);
    }
    //------------------------------------------------------------------
    *sndbuf = get_sock_buf(AF_UNIX, SO_SNDBUF, SOCK_DGRAM, 0);
    if (*sndbuf < 0)
    {
        fprintf(stderr, " Error get_sock_buf(AF_UNIX, SOCK_DGRAM, 0): %s\n\n", strerror(-(*sndbuf)));
        *sndbuf = 0;
    }
    else
        fprintf(stderr, " AF_UNIX: SO_SNDBUF=%d\n\n", *sndbuf);

    if (*sndbuf >= 163840)
        *sndbuf = 0;
    else
        *sndbuf = 163840;

    numCreatedProc = 0;
    while (numCreatedProc < NumProc)
    {
        pidChild[numCreatedProc] = create_child(numCreatedProc, from_chld, *sndbuf);
        if (pidChild[numCreatedProc] < 0)
        {
            fprintf(stderr, "<%s:%d> Error create_child() %d\n", __func__, __LINE__, numCreatedProc);
            exit(1);
        }

        ++numCreatedProc;
    }
}
//======================================================================
void print_help(const char *name)
{
    fprintf(stderr, "Usage: %s [-h] [-p] [-c configfile] [-s signal]\n"
                    "Options:\n"
                    "   -h              : help\n"
                    "   -p              : print parameters\n"
                    "   -c configfile   : default: \"./server.conf\"\n"
                    "   -s signal       : restart, close, abort\n", name);
}
//======================================================================
void print_limits()
{
    struct rlimit lim;
    if (getrlimit(RLIMIT_NOFILE, &lim) == -1)
        fprintf(stdout, "<%s:%d> Error getrlimit(RLIMIT_NOFILE): %s\n", __func__, __LINE__, strerror(errno));
    else
        printf(" RLIMIT_NOFILE: cur=%ld, max=%ld\n\n", (long)lim.rlim_cur, (long)lim.rlim_max);
    printf(" hardware_concurrency(): %u\n\n", thread::hardware_concurrency());
    //------------------------------------------------------------------
    int sbuf = get_sock_buf(AF_INET, SO_SNDBUF, SOCK_STREAM, 0);
    if (sbuf < 0)
        fprintf(stderr, " Error get_sock_buf(AF_INET, SOCK_STREAM, 0): %s\n", strerror(-sbuf));
    else
        fprintf(stdout, " AF_INET: SO_SNDBUF=%d\n", sbuf);

    sbuf = get_sock_buf(AF_INET, SO_RCVBUF, SOCK_STREAM, 0);
    if (sbuf < 0)
        fprintf(stderr, " Error get_sock_buf(AF_INET, SOCK_STREAM, 0): %s\n", strerror(-sbuf));
    else
        fprintf(stdout, " AF_INET: SO_RCVBUF=%d\n\n", sbuf);
    //------------------------------------------------------------------
    sbuf = get_sock_buf(AF_UNIX, SO_SNDBUF, SOCK_DGRAM, 0);
    if (sbuf < 0)
        fprintf(stderr, " Error get_sock_buf(AF_UNIX, SOCK_DGRAM, 0): %s\n\n", strerror(-sbuf));
    else
        fprintf(stdout, " AF_UNIX: SO_SNDBUF=%d\n", sbuf);

    sbuf = get_sock_buf(AF_UNIX, SO_RCVBUF, SOCK_DGRAM, 0);
    if (sbuf < 0)
        fprintf(stderr, " Error get_sock_buf(AF_UNIX, SOCK_DGRAM, 0): %s\n\n", strerror(-sbuf));
    else
        fprintf(stdout, " AF_UNIX: SO_RCVBUF=%d\n\n", sbuf);
}
//======================================================================
void print_config()
{
    print_limits();

    cout << "   ServerSoftware       : " << conf->ServerSoftware.c_str()
         << "\n\n   ServerAddr           : " << conf->ServerAddr.c_str()
         << "\n   ServerPort           : " << conf->ServerPort.c_str()
         << "\n   ListenBacklog        : " << conf->ListenBacklog
         << "\n   TcpCork              : " << conf->TcpCork
         << "\n   TcpNoDelay           : " << conf->TcpNoDelay
         << "\n\n   SendFile             : " << conf->SendFile
         << "\n   SndBufSize           : " << conf->SndBufSize
         << "\n\n   NumCpuCores          : " << conf->NumCpuCores
         << "\n   MaxWorkConnections   : " << conf->MaxWorkConnections
         << "\n   MaxEventConnections  : " << conf->MaxEventConnections
         << "\n   TimeoutPoll          : " << conf->TimeoutPoll
         << "\n\n   NumProc              : " << conf->NumProc
         << "\n   MaxThreads           : " << conf->MaxThreads
         << "\n   MimThreads           : " << conf->MinThreads
         << "\n   MaxCgiProc           : " << conf->MaxCgiProc
         << "\n\n   MaxRequestsPerClient : " << conf->MaxRequestsPerClient
         << "\n   TimeoutKeepAlive     : " << conf->TimeoutKeepAlive
         << "\n   Timeout              : " << conf->Timeout
         << "\n   TimeoutCGI           : " << conf->TimeoutCGI
         << "\n   MaxRanges            : " << conf->MaxRanges
         << "\n\n   UsePHP               : " << conf->UsePHP.c_str()
         << "\n   PathPHP              : " << conf->PathPHP.c_str()
         << "\n   DocumentRoot         : " << conf->DocumentRoot.c_str()
         << "\n   ScriptPath           : " << conf->ScriptPath.c_str()
         << "\n   LogPath              : " << conf->LogPath.c_str()
         << "\n\n   ShowMediaFiles       : " << conf->ShowMediaFiles
         << "\n\n   ClientMaxBodySize    : " << conf->ClientMaxBodySize
         << "\n\n   AutoIndex            : " << conf->AutoIndex
         << "\n   index_html           : " << conf->index_html
         << "\n   index_php            : " << conf->index_php
         << "\n   index_pl             : " << conf->index_pl
         << "\n   index_fcgi           : " << conf->index_fcgi
         << "\n\n";
    cout << "   ------------- FastCGI -------------\n";
    fcgi_list_addr *i = conf->fcgi_list;
    for (; i; i = i->next)
    {
        cout << "   [" << i->script_name.c_str() << " : " << i->addr.c_str() << "]\n";
    }
}
//======================================================================
int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);

    if (argc == 1)
        confPath = "server.conf";
    else
    {
        int c, arg_print = 0;
        char *sig = NULL;
        while ((c = getopt(argc, argv, "c:s:h:p")) != -1)
        {
            switch (c)
            {
                case 'c':
                    confPath = optarg;
                    break;
                case 's':
                    sig = optarg;
                    break;
                case 'h':
                    print_help(argv[0]);
                    return 0;
                case 'p':
                    arg_print = 1;
                    break;
                default:
                    print_help(argv[0]);
                    return 0;
            }
        }

        if (!confPath.size())
            confPath = "server.conf";

        if (arg_print)
        {
            if (read_conf_file(confPath.c_str()))
                return 1;
            print_config();
            return 0;
        }

        if (sig)
        {
            int sig_send;
            if (!strcmp(sig, "restart"))
                sig_send = SIGUSR1;
            else if (!strcmp(sig, "close"))
                sig_send = SIGUSR2;
            else if (!strcmp(sig, "abort"))
                sig_send = SIGTERM;
            else
            {
                fprintf(stderr, "<%d> ? option -s: %s\n", __LINE__, sig);
                print_help(argv[0]);
                return 1;
            }

            if (read_conf_file(confPath.c_str()))
                return 1;
            pidFile = conf->PidFilePath + "/pid.txt";
            FILE *fpid = fopen(pidFile.c_str(), "r");
            if (!fpid)
            {
                fprintf(stderr, "<%s:%d> Error open PidFile(%s): %s\n", __func__, __LINE__, pidFile.c_str(), strerror(errno));
                return 1;
            }

            pid_t pid;
            fscanf(fpid, "%u", &pid);
            fclose(fpid);

            if (kill(pid, sig_send))
            {
                fprintf(stderr, "<%d> Error kill(pid=%u, sig=%u): %s\n", __LINE__, pid, sig_send, strerror(errno));
                return 1;
            }

            return 0;
        }
    }

    while (restartServer)
    {
        restartServer = 0;

        if (read_conf_file(confPath.c_str()))
            return 1;

        set_uid();
        //--------------------------------------------------------------
        sockServer = create_server_socket(conf);
        if (sockServer == -1)
        {
            fprintf(stderr, "<%s:%d> Error: create_server_socket(%s:%s)\n", __func__, __LINE__,
                        conf->ServerAddr.c_str(), conf->ServerPort.c_str());
            break;
        }

        Connect::serverSocket = sockServer;
        //--------------------------------------------------------------
        if (startServer == 0)
        {
            startServer = 1;
            pidFile = conf->PidFilePath + "/pid.txt";
            FILE *fpid = fopen(pidFile.c_str(), "w");
            if (!fpid)
            {
                fprintf(stderr, "<%s:%d> Error open PidFile(%s): %s\n", __func__, __LINE__, pidFile.c_str(), strerror(errno));
                return 1;
            }

            fprintf(fpid, "%u\n", getpid());
            fclose(fpid);
            //----------------------------------------------------------
            if (signal(SIGINT, signal_handler) == SIG_ERR)
            {
                fprintf(stderr, "<%s> Error signal(SIGINT): %s\n", __func__, strerror(errno));
                break;
            }

            if (signal(SIGTERM, signal_handler) == SIG_ERR)
            {
                fprintf(stderr, "<%s> Error signal(SIGTERM): %s\n", __func__, strerror(errno));
                break;
            }

            if (signal(SIGSEGV, signal_handler) == SIG_ERR)
            {
                fprintf(stderr, "<%s> Error signal(SIGSEGV): %s\n", __func__, strerror(errno));
                break;
            }

            if (signal(SIGUSR1, signal_handler) == SIG_ERR)
            {
                fprintf(stderr, "<%s> Error signal(SIGUSR1): %s\n", __func__, strerror(errno));
                break;
            }

            if (signal(SIGUSR2, signal_handler) == SIG_ERR)
            {
                fprintf(stderr, "<%s> Error signal(SIGUSR2): %s\n", __func__, strerror(errno));
                break;
            }
        }
        //--------------------------------------------------------------
        create_logfiles(conf->LogPath);
        //--------------------------------------------------------------
        int ret = main_proc();
        close_logs();
        if (ret)
            break;
    }

    if (startServer == 1)
        remove(pidFile.c_str());
    return 0;
}
//======================================================================
int main_proc()
{
    pid_t pid = getpid();
    //------------------------------------------------------------------
    cout << " [" << get_time().c_str() << "] - server \"" << conf->ServerSoftware.c_str()
         << "\" run, port: " << conf->ServerPort.c_str() << "\n";
    cerr << "  uid=" << getuid() << "; gid=" << getgid() << "\n\n";
    cout << "  uid=" << getuid() << "; gid=" << getgid() << "\n\n";
    cerr << "   MaxWorkConnections: " << conf->MaxWorkConnections << ", NumCpuCores: " << conf->NumCpuCores << "\n";
    cerr << "   SndBufSize: " << conf->SndBufSize << ", MaxEventConnections: " << conf->MaxEventConnections << "\n";
    //------------------------------------------------------------------
    for ( ; environ[0]; )
    {
        char *p, buf[512];
        if ((p = (char*)memccpy(buf, environ[0], '=', strlen(environ[0]))))
        {
            *(p - 1) = 0;
            unsetenv(buf);
        }
    }
    //------------------------------------------------------------------
    int from_chld[2], sndbuf = 0;
    create_proc(conf->NumProc, from_chld, &sndbuf);
    cout << "   pid = " << pid << "\n\n";
    //------------------------------------------------------------------
    for (unsigned int i = 0; i < conf->NumProc; ++i)
        numConn[i] = 0;
    //------------------------------------------------------------------
    static struct pollfd fdrd[2];

    fdrd[0].fd = from_chld[0];
    fdrd[0].events = POLLIN;

    fdrd[1].fd = sockServer;
    fdrd[1].events = POLLIN;

    closeChldProc = 0;

    unsigned int numFD = 2, indexProc = 0;

    while (1)
    {
        if (closeChldProc)
        {
            if (allConn == 0)
                break;
            numFD = 1;
        }
        else
        {
            if (conf->NumCpuCores == 1)
                indexProc = 0;
            else
            {
                indexProc++;
                if (indexProc >= numCreatedProc)
                    indexProc = 0;
            }

            for (unsigned int i = indexProc; ; )
            {
                if (numConn[indexProc] < conf->MaxWorkConnections)
                {
                    numFD = 2;
                    break;
                }

                indexProc++;
                if (indexProc >= numCreatedProc)
                    indexProc = 0;

                if (indexProc == i)
                {
                    if (numCreatedProc < conf->MaxNumProc)
                    {
                        pidChild[numCreatedProc] = create_child(numCreatedProc, from_chld, sndbuf);
                        if (pidChild[numCreatedProc] < 0)
                        {
                            fprintf(stderr, "<%s:%d> Error create_child() %d\n", __func__, __LINE__, numCreatedProc);
                            numFD = 1;
                        }
                        else
                        {
                            indexProc = numCreatedProc;
                            ++numCreatedProc;
                            numFD = 2;
                        }
                    }
                    else
                        numFD = 1;
                    break;
                }
            }

            if (allConn == 0)
                numFD = 2;
        }

        int ret_poll = poll(fdrd, numFD, -1);
        if (ret_poll <= 0)
        {
            print_err("<%s:%d> Error poll()=-1: %s\n", __func__, __LINE__, strerror(errno));
            continue;
        }

        if (fdrd[0].revents == POLLIN)
        {
            unsigned char s[8];
            int ret = read(from_chld[0], s, sizeof(s));
            if (ret <= 0)
            {
                print_err("<%s:%d> Error read()=%d: %s\n", __func__, __LINE__, ret, strerror(errno));
                break;
            }

            for (int i = 0; i < ret; i++)
            {
                numConn[s[i]]--;
                allConn--;
            }

            ret_poll--;
        }

        if (ret_poll && (fdrd[1].revents == POLLIN))
        {
            int clientSock = accept(sockServer, NULL, NULL);
            if (clientSock == -1)
            {
                print_err("<%s:%d> Error accept()=-1: %s\n", __func__, __LINE__, strerror(errno));
                break;
            }

            char data[1] = "";
            int ret = send_fd(unixFD[indexProc][1], clientSock, data, sizeof(data));
            if (ret < 0)
            {
                if (ret == -ENOBUFS)
                    print_err("<%s:%d> Error send_fd: ENOBUFS\n", __func__, __LINE__);
                else
                {
                    print_err("<%s:%d> Error send_fd()\n", __func__, __LINE__);
                    break;
                }
            }
            else
            {
                numConn[indexProc]++;
                allConn++;
            }
            close(clientSock);
            ret_poll--;
        }

        if (ret_poll)
        {
            print_err("<%s:%d> fdrd[0].revents=0x%02x; fdrd[1].revents=0x%02x\n", __func__, __LINE__,
                            fdrd[0].revents, fdrd[1].revents);
            break;
        }
    }

    for (unsigned int i = 0; i < numCreatedProc; ++i)
    {
        char ch = i;
        int ret = send_fd(unixFD[i][1], -1, &ch, 1);
        if (ret < 0)
        {
            fprintf(stderr, "<%s:%d> Error send_fd()\n", __func__, __LINE__);
            if (kill(pidChild[i], SIGKILL))
            {
                fprintf(stderr, "<%s:%d> Error: kill(%u, %u)\n", __func__, __LINE__, pidChild[i], SIGKILL);
            }
        }
        close(unixFD[i][1]);
    }

    close(sockServer);
    close(from_chld[0]);
    free_fcgi_list();

    while ((pid = wait(NULL)) != -1)
    {
        fprintf(stderr, "<%s> wait() pid: %d\n", __func__, pid);
    }

    if (restartServer == 0)
        fprintf(stderr, "<%s> ***** Close *****\n", __func__);
    else
        fprintf(stderr, "<%s> ***** Reload *****\n", __func__);

    fprintf(stderr, "<%s> ***** All connect: %u *****\n", __func__, allConn);

    return 0;
}
//======================================================================
void manager(int sock, int, int, int);
//======================================================================
pid_t create_child(int num_chld, int *from_chld, int sock_buf_size)
{
    pid_t pid;
    int ret = unix_socket_pair(unixFD[num_chld]);
    if (ret < 0)
    {
        fprintf(stderr, "<%s:%d> Error unix_socket_pair(): %s\n", __func__, __LINE__, strerror(errno));
        return -1;
    }

    if (sock_buf_size > 0)
    {
        socklen_t optlen = sizeof(sock_buf_size);

        if (setsockopt(unixFD[num_chld][1], SOL_SOCKET, SO_SNDBUF, &sock_buf_size, optlen) < 0)
        {
            fprintf(stderr, "<%s:%d> Error setsockopt(SO_SNDBUF): %s\n", __func__, __LINE__, strerror(errno));
            return -1;
        }

        if (setsockopt(unixFD[num_chld][0], SOL_SOCKET, SO_RCVBUF, &sock_buf_size, optlen) < 0)
        {
            fprintf(stderr, "<%s:%d> Error setsockopt(SO_RCVBUF): %s\n", __func__, __LINE__, strerror(errno));
            return -1;
        }
    }

    errno = 0;
    pid = fork();
    if (pid == 0)
    {
        uid_t uid = getuid();
        if (uid == 0)
        {
            if (setgid(conf->server_gid) == -1)
            {
                fprintf(stderr, "<%s> Error setgid(%d): %s\n", __func__, conf->server_gid, strerror(errno));
                exit(1);
            }

            if (setuid(conf->server_gid) == -1)
            {
                fprintf(stderr, "<%s> Error setuid(%d): %s\n", __func__, conf->server_uid, strerror(errno));
                exit(1);
            }
        }

        for (int i = 0; i <= num_chld; ++i)
        {
            close(unixFD[i][1]);
            //printf("[%d]<%s:%d> close[%d][0]=%d\n", num_chld, __func__, __LINE__, i, unixFD[i][0]);
        }

        close(from_chld[0]);
        manager(sockServer, num_chld, unixFD[num_chld][0], from_chld[1]);
        close(from_chld[1]);
        close(unixFD[num_chld][0]);

        close_logs();
        exit(0);
    }
    else if (pid < 0)
    {
        fprintf(stderr, "<> Error fork(): %s\n", strerror(errno));
        return -1;
    }

    close(unixFD[num_chld][0]);

    char ch;
    ret = read_from_pipe(from_chld[0], &ch, sizeof(ch), 3);
    if (ret <= 0)
    {
        fprintf(stderr, "<%s:%d> Error read()=%d\n", __func__, __LINE__, ret);
        pid = -1;
    }

    return pid;
}
