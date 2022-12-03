#include "main.h"

using namespace std;

int sockServer;
int Connect::serverSocket;

int create_server_socket(const Config *conf);
int read_conf_file(const char *path_conf);
void free_fcgi_list();
int set_uid();
int main_proc();

void create_proc(int);
int unixConnect(const char *path);

static string conf_path;
static string pidFile;

static int from_chld[2], unixFD[8];

static int numConn[8];
static pid_t pidArr[8];

static int start = 0, restart = 1;

static int received_sig = 0;
enum { SIG_RESTART_ = 1, SIG_CLOSE_,};

static unsigned int all_conn = 0;
//======================================================================
void server_restart()
{
    char ch[1];
    for (unsigned int i = 0; i < conf->NumProc; ++i)
    {
        int ret = send_fd(unixFD[i], -1, ch, 1);
        if (ret < 0)
        {
            fprintf(stderr, "<%s:%d> Error sendClientSock()\n", __func__, __LINE__);
            if (kill(pidArr[i], SIGKILL))
            {
                fprintf(stderr, "<%s:%d> Error: kill(%u, %u)\n", __func__, __LINE__, pidArr[i], SIGKILL);
                exit(1);
            }
        }
    }
}
//======================================================================
void server_close()
{
    char ch[1];
    for (unsigned int i = 0; i < conf->NumProc; ++i)
    {
        int ret = send_fd(unixFD[i], -1, ch, 1);
        if (ret < 0)
        {
            fprintf(stderr, "<%s:%d> Error sendClientSock()\n", __func__, __LINE__);
            if (kill(pidArr[i], SIGKILL))
            {
                fprintf(stderr, "<%s:%d> Error: kill(%u, %u)\n", __func__, __LINE__, pidArr[i], SIGKILL);
                exit(1);
            }
        }
    }

    restart = 0;
}
//======================================================================
static void signal_handler(int sig)
{
    if (sig == SIGINT)
    {
        fprintf(stderr, "<main> ####### SIGINT #######\n");
        received_sig = SIG_CLOSE_;
    }
    else if (sig == SIGTERM)
    {
        print_err("<main> ####### SIGTERM #######\n");
        signal(SIGCHLD, SIG_IGN);
        shutdown(sockServer, SHUT_RDWR);
        close(sockServer);
        
        for (unsigned int i = 0; i < conf->NumProc; ++i)
        {
            fprintf(stderr, "<%s> pid=%u\n", __func__, pidArr[i]);
            if (kill(pidArr[i], SIGTERM) < 0)
                fprintf(stderr, "<%s> Error kill(): %s\n", __func__, strerror(errno));
        }

        exit(0);
    }
    else if (sig == SIGSEGV)
    {
        fprintf(stderr, "<main> ####### SIGSEGV #######\n");
        exit(1);
    }
    else if (sig == SIGUSR1)
    {
        fprintf(stderr, "<main> ####### SIGUSR1 #######\n");
        received_sig = SIG_RESTART_;
    }
    else if (sig == SIGUSR2)
    {
        fprintf(stderr, "<main> ####### SIGUSR2 #######\n");
        received_sig = SIG_CLOSE_;
    }
    else
    {
        fprintf(stderr, "<%s:%d> ? sig=%d\n", __func__, __LINE__, sig);
    }
}
//======================================================================
pid_t create_child(int num_chld, int *from_chld);
//======================================================================
void create_proc(int NumProc)
{
    if (pipe(from_chld) < 0)
    {
        fprintf(stderr, "<%s:%d> Error pipe(): %s\n", __func__, __LINE__, strerror(errno));
        exit(1);
    }
    //------------------------------------------------------------------
    pid_t pid_child;
    int i = 0;
    while (i < NumProc)
    {
        pid_child = create_child(i, from_chld);
        if (pid_child < 0)
        {
            fprintf(stderr, "<%s:%d> Error create_child() %d\n", __func__, __LINE__, i);
            exit(1);
        }
        pidArr[i] = pid_child;
        ++i;
    }

    close(from_chld[1]);
    sleep(1);

    for (int i = 0; i < NumProc; ++i)
    {
        String s;
        s << "unix_sock_" << pidArr[i] << "_"  << i;

        if ((unixFD[i] = unixConnect(s.c_str(), SOCK_DGRAM)) < 0)
        {
            fprintf(stderr, "[%d]<%s:%d> Error create_fcgi_socket(%s)=%d: %s\n", i, __func__, __LINE__, 
                            s.c_str(), unixFD[i], strerror(errno));
            exit(1);
        }

        if (remove(s.c_str()) == -1)
        {
            fprintf(stderr, "[%d]<%s:%d> Error remove(%s): %s\n", i, __func__, __LINE__, s.c_str(), strerror(errno));
            exit(1);
        }
    }
}
//======================================================================
void print_help(const char *name)
{
    fprintf(stderr, "Usage: %s [-l] [-c configfile] [-s signal]\n"
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
        printf(" RLIMIT_NOFILE: cur=%ld, max=%ld\n", (long)lim.rlim_cur, (long)lim.rlim_max);
}
//======================================================================
void print_config()
{
    print_limits();
    
    cout << "   ServerSoftware       : " << conf->ServerSoftware.c_str()
         << "\n\n   ServerAddr           : " << conf->ServerAddr.c_str()
         << "\n   ServerPort           : " << conf->ServerPort.c_str()
         << "\n   ListenBacklog        : " << conf->ListenBacklog
         << "\n   tcp_cork             : " << conf->tcp_cork
         << "\n   TcpNoDelay           : " << conf->tcp_nodelay
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
    received_sig = 0;

    if (argc == 1)
        conf_path = "server.conf";
    else
    {
        int c, arg_print = 0;
        pid_t pid_ = 0;
        char *sig = NULL, *conf_dir_ = NULL;
        while ((c = getopt(argc, argv, "c:s:h:p")) != -1)
        {
            switch (c)
            {
                case 'c':
                    conf_dir_ = optarg;
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

        if (conf_dir_)
            conf_path = conf_dir_;
        else
            conf_path = "server.conf";

        if (arg_print)
        {
            if (read_conf_file(conf_path.c_str()))
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

            if (read_conf_file(conf_path.c_str()))
                return 1;
            pidFile = conf->PidFilePath + "/pid.txt";
            FILE *fpid = fopen(pidFile.c_str(), "r");
            if (!fpid)
            {
                fprintf(stderr, "<%s:%d> Error open PidFile(%s): %s\n", __func__, __LINE__, pidFile.c_str(), strerror(errno));
                return 1;
            }

            fscanf(fpid, "%u", &pid_);
            fclose(fpid);

            if (kill(pid_, sig_send))
            {
                fprintf(stderr, "<%d> Error kill(pid=%u, sig=%u): %s\n", __LINE__, pid_, sig_send, strerror(errno));
                return 1;
            }

            return 0;
        }
    }

    while (restart)
    {
        restart = 0;

        if (read_conf_file(conf_path.c_str()))
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
        if (start == 0)
        {
            start = 1;
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
                fprintf(stderr, "<%s:%d> Error signal(SIGINT): %s\n", __func__, __LINE__, strerror(errno));
                break;
            }

            if (signal(SIGTERM, signal_handler) == SIG_ERR)
            {
                fprintf(stderr, "<%s:%d> Error signal(SIGTERM): %s\n", __func__, __LINE__, strerror(errno));
                break;
            }

            if (signal(SIGSEGV, signal_handler) == SIG_ERR)
            {
                fprintf(stderr, "<%s:%d> Error signal(SIGSEGV): %s\n", __func__, __LINE__, strerror(errno));
                break;
            }

            if (signal(SIGUSR1, signal_handler) == SIG_ERR)
            {
                fprintf(stderr, "<%s:%d> Error signal(SIGUSR1): %s\n", __func__, __LINE__, strerror(errno));
                break;
            }

            if (signal(SIGUSR2, signal_handler) == SIG_ERR)
            {
                fprintf(stderr, "<%s:%d> Error signal(SIGUSR2): %s\n", __func__, __LINE__, strerror(errno));
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

    if (start == 1)
        remove(pidFile.c_str());
    return 0;
}
//======================================================================
int main_proc()
{
    if (start == 0)
    {
        start = 1;

        signal(SIGUSR2, SIG_IGN);
        if (signal(SIGINT, signal_handler) == SIG_ERR)
        {
            fprintf(stderr, "<%s:%d> Error signal(SIGINT): %s\n", __func__, __LINE__, strerror(errno));
            return 1;
        }

        if (signal(SIGSEGV, signal_handler) == SIG_ERR)
        {
            fprintf(stderr, "<%s:%d> Error signal(SIGSEGV): %s\n", __func__, __LINE__, strerror(errno));
            return 1;
        }

        if (signal(SIGUSR1, signal_handler) == SIG_ERR)
        {
            fprintf(stderr, "<%s:%d> Error signal(SIGUSR1): %s\n", __func__, __LINE__, strerror(errno));
            return 1;
        }
    
        if (signal(SIGTERM, signal_handler) == SIG_ERR)
        {
            fprintf(stderr, "<%s:%d> Error signal(SIGTERM): %s\n", __func__, __LINE__, strerror(errno));
            return 1;
        }
    }

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

    create_proc(conf->NumProc);
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

    unsigned int num_fdrd = 2, i_fd = 0;
    int err_send_sock = 0;

    while (1)
    {
        if (received_sig)
        {
            if (all_conn == 0)
            {
                if (received_sig == SIG_RESTART_)
                    restart = 1;

                received_sig = 0;
                break;
            }
            else
                num_fdrd = 1;
        }
        else
        {
            if (conf->NumCpuCores == 1)
                i_fd = 0;

            for (unsigned int i = i_fd; ; )
            {
                if (i_fd >= conf->NumProc)
                {
                    i_fd = 0;
                    if (conf->NumCpuCores == 1)
                    {
                        num_fdrd = 1;
                        break;
                    }
                }
                if (numConn[i_fd] < conf->MaxWorkConnections)
                {
                    i_fd++;
                    if (i_fd >= conf->NumProc)
                        i_fd = 0;
                    break;
                }

                i_fd++;

                if (i_fd == i)
                {
                    num_fdrd = 1;
                    break;
                }
            }

            if (err_send_sock > 0)
            {
                char data[1] = "";
                int ret = send_fd(unixFD[i_fd], err_send_sock, data, sizeof(data));
                if (ret == -ENOBUFS)
                {
                    num_fdrd = 1;
                    print_err("<%s:%d> Error send_fd: ENOBUFS\n", __func__, __LINE__);
                }
                else if (ret == -1)
                {
                    print_err("<%s:%d> Error sendClientSock()\n", __func__, __LINE__);
                    break;
                }
                else
                {
                    close(err_send_sock);
                    err_send_sock = 0;
                    numConn[i_fd]++;
                    all_conn++;
                    continue;
                }
            }
        }

        int ret_poll = poll(fdrd, num_fdrd, -1);
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

            for ( int i = 0; i < ret; i++)
            {
                numConn[s[i]]--;
                all_conn--;
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
            int ret = send_fd(unixFD[i_fd], clientSock, data, sizeof(data));
            if (ret == -ENOBUFS)
            {
                err_send_sock = clientSock;
                print_err("<%s:%d> Error send_fd: ENOBUFS\n", __func__, __LINE__);
                continue;
            }
            else if (ret == -1)
            {
                print_err("<%s:%d> Error send_fd()\n", __func__, __LINE__);
                break;
            }
            else
            {
                close(clientSock);
                numConn[i_fd]++;
                ret_poll--;
                all_conn++;
            }
        }

        if (ret_poll)
        {
            print_err("<%s:%d> fdrd[0].revents=0x%02x; fdrd[1].revents=0x%02x; ret=%d\n", __func__, __LINE__, 
                            fdrd[0].revents, fdrd[1].revents, ret_poll);
            break;
        }
    }

    for (unsigned int i = 0; i < conf->NumProc; ++i)
    {
        char ch = i;
        int ret = send_fd(unixFD[i], -1, &ch, 1);
        if (ret < 0)
        {
            fprintf(stderr, "<%s:%d> Error send_fd()\n", __func__, __LINE__);
            if (kill(pidArr[i], SIGKILL))
            {
                fprintf(stderr, "<%s:%d> Error: kill(%u, %u)\n", __func__, __LINE__, pidArr[i], SIGKILL);
            }
        }
        close(unixFD[i]);
    }

    close(sockServer);
    close(from_chld[0]);
    free_fcgi_list();

    while ((pid = wait(NULL)) != -1)
    {
        print_err("<> wait() pid: %d\n", pid);
    }
    
    shutdown(sockServer, SHUT_RDWR);
    
    if (restart == 0)
        fprintf(stderr, "<%s> ***** Close *****\n", __func__);
    else
        fprintf(stderr, "<%s> ***** Reload *****\n", __func__);

    print_err("<%s:%d> Exit server\n", __func__, __LINE__);
    fprintf(stderr, "<%s> ***** All connect: %u *****\n", __func__, all_conn);

    return 0;
}
//======================================================================
void manager(int sock, int, int);
//======================================================================
pid_t create_child(int num_chld, int *from_chld)
{
    pid_t pid;

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

        close(from_chld[0]);
        manager(sockServer, num_chld, from_chld[1]);
        close(from_chld[1]);

        close_logs();
        exit(0);
    }
    else if (pid < 0)
    {
        fprintf(stderr, "<> Error fork(): %s\n", strerror(errno));
    }

    return pid;
}
