#include "main.h"

using namespace std;

static mutex mtx_conn;
static condition_variable cond_close_conn;
static int num_conn = 0, all_req = 0;
//======================================================================
RequestManager::RequestManager(unsigned int n)
{
    list_start = list_end = NULL;
    size_list = stop_manager = all_thr = 0;
    count_thr = num_wait_thr = 0;
    NumProc = n;
}
//----------------------------------------------------------------------
RequestManager::~RequestManager() {}
//----------------------------------------------------------------------
int RequestManager::get_num_chld()
{
    return NumProc;
}
//----------------------------------------------------------------------
int RequestManager::get_num_thr()
{
lock_guard<std::mutex> lg(mtx_thr);
    return count_thr;
}
//----------------------------------------------------------------------
int RequestManager::get_all_thr()
{
    return all_thr;
}
//----------------------------------------------------------------------
int RequestManager::start_thr()
{
mtx_thr.lock();
    int ret = ++count_thr;
    ++all_thr;
mtx_thr.unlock();
    return ret;
}
//----------------------------------------------------------------------
void RequestManager::wait_exit_thr(unsigned int n)
{
    unique_lock<mutex> lk(mtx_thr);
    while (n == count_thr)
    {
        cond_exit_thr.wait(lk);
    }
}
//----------------------------------------------------------------------
void push_resp_list(Connect *req, RequestManager *ReqMan)
{
ReqMan->mtx_thr.lock();
    req->next = NULL;
    req->prev = ReqMan->list_end;
    if (ReqMan->list_start)
    {
        ReqMan->list_end->next = req;
        ReqMan->list_end = req;
    }
    else
        ReqMan->list_start = ReqMan->list_end = req;

    ++ReqMan->size_list;
    ++all_req;
ReqMan->mtx_thr.unlock();
    ReqMan->cond_list.notify_one();
}
//----------------------------------------------------------------------
Connect *RequestManager::pop_resp_list()
{
unique_lock<mutex> lk(mtx_thr);
    ++num_wait_thr;
    while (list_start == NULL)
    {
        cond_list.wait(lk);
        if (stop_manager)
            return NULL;
    }
    --num_wait_thr;
    Connect *req = list_start;
    if (list_start->next)
    {
        list_start->next->prev = NULL;
        list_start = list_start->next;
    }
    else
        list_start = list_end = NULL;

    --size_list;
    if (num_wait_thr <= 1)
        cond_new_thr.notify_one();

    return req;
}
//----------------------------------------------------------------------
int RequestManager::wait_create_thr(int *n)
{
unique_lock<mutex> lk(mtx_thr);
    while (((size_list <= num_wait_thr) || (count_thr >= conf->MaxThreads)) && !stop_manager)
    {
        cond_new_thr.wait(lk);
    }

    *n = count_thr;
    return stop_manager;
}
//----------------------------------------------------------------------
int RequestManager::end_thr(int ret)
{
mtx_thr.lock();
    if (((count_thr > conf->MinThreads) && (size_list < num_wait_thr)) || ret)
    {
        --count_thr;
        ret = EXIT_THR;
    }
mtx_thr.unlock();
    if (ret)
    {
        cond_exit_thr.notify_all();
    }
    return ret;
}
//----------------------------------------------------------------------
void RequestManager::close_manager()
{
    stop_manager = 1;
    cond_new_thr.notify_one();
    cond_exit_thr.notify_one();
    cond_list.notify_all();
}
//======================================================================
void start_conn()
{
mtx_conn.lock();
    ++num_conn;
mtx_conn.unlock();
}
//======================================================================
void check_num_conn()
{
unique_lock<mutex> lk(mtx_conn);
    while (num_conn >= (conf->MaxConnections - conf->OverMaxConnections))
    {
        cond_close_conn.wait(lk);
    }
}
//======================================================================
int is_maxconn()
{
mtx_conn.lock();
    int n = 0;
    if (num_conn >= conf->MaxConnections)
        n = 1;
mtx_conn.unlock();
    return n;
}
//======================================================================
void wait_close_all_conn()
{
unique_lock<mutex> lk(mtx_conn);
    while (num_conn > 0)
    {
        cond_close_conn.wait(lk);
    }
}
//======================================================================
void end_response(Connect *req)
{
    if (req->connKeepAlive == 0 || req->err < 0)
    { // ----- Close connect -----
        if (req->err > NO_PRINT_LOG)// 0 > err > NO_PRINT_LOG(-1000)
        {
            if (req->err < -1)// -1 > err > NO_PRINT_LOG(-1000)
            {
                req->respStatus = -req->err;
                send_message(req, NULL, NULL);
            }
            print_log(req);
        }

        shutdown(req->clientSocket, SHUT_RDWR);
        close(req->clientSocket);
        delete req;
        dec_work_conn();
    mtx_conn.lock();
        --num_conn;
    mtx_conn.unlock();
        cond_close_conn.notify_all();
    }
    else
    { // ----- KeepAlive -----
    #ifdef TCP_CORK_
        if (conf->tcp_cork == 'y')
        {
        #if defined(LINUX_)
            int optval = 0;
            setsockopt(req->clientSocket, SOL_TCP, TCP_CORK, &optval, sizeof(optval));
        #elif defined(FREEBSD_)
            int optval = 0;
            setsockopt(req->clientSocket, IPPROTO_TCP, TCP_NOPUSH, &optval, sizeof(optval));
        #endif
        }
    #endif
        print_log(req);
        req->timeout = conf->TimeoutKeepAlive;
        ++req->numReq;
        push_pollin_list(req);
    }
}
//======================================================================
void thr_create_manager(int numProc, RequestManager *ReqMan)
{
    int num_thr;
    thread thr;

    while (1)
    {
        if (ReqMan->wait_create_thr(&num_thr))
            break;
        try
        {
            thr = thread(response1, ReqMan);
        }
        catch (...)
        {
            print_err("[%d] <%s:%d> Error create thread: num_thr=%d, errno=%d\n", numProc, __func__, __LINE__, num_thr, errno);
            ReqMan->wait_exit_thr(num_thr);
            continue;
        }

        thr.detach();

        ReqMan->start_thr();
    }
    print_err("[%d] <%s:%d> *** Exit thread_req_manager() ***\n", numProc, __func__, __LINE__);
}
//======================================================================
static unsigned int nProc;
static RequestManager *RM;
static unsigned long allConn = 0;
//======================================================================
static void signal_handler_child(int sig)
{
    if (sig == SIGINT)
    {
        //print_err("[%d] <%s:%d> ### SIGINT ### all_req=%d\n", nProc, __func__, __LINE__, all_req);
    }
    else if (sig == SIGSEGV)
    {
        print_err("[%d] <%s:%d> ### SIGSEGV ###\n", nProc, __func__, __LINE__);
        exit(1);
    }
    else if (sig == SIGUSR1)
    {
        print_err("[%d] <%s:%d> ### SIGUSR1 ###\n", nProc, __func__, __LINE__);
    }
    else
        print_err("[%d] <%s:%d> sig=%d\n", nProc, __func__, __LINE__, sig);
}
//======================================================================
Connect *create_req();
int write_to_pipe(int fd, int *data, int size);
//======================================================================
void manager(int sockServer, unsigned int numProc, int fd_in)
{
    int pfd[2];
    int end_proc = 0;

    if ((numProc + 1) < conf->NumProc)
    {
        if (pipe(pfd) < 0)
        {
            fprintf(stderr, "<%s:%d> Error pipe(): %s\n", __func__, __LINE__, strerror(errno));
            exit(1);
        }

        if (create_child(sockServer, numProc + 1, pfd, fd_in) == 0)
        {
            fprintf(stderr, "<%s:%d> Error create_child()\n", __func__, __LINE__);
            exit(1);
        }  // max open fd = 8
    }
    else
        end_proc = 1; // max open fd = 7
    //------------------------------------------------------------------
    RequestManager *ReqMan = new(nothrow) RequestManager(numProc);
    if (!ReqMan)
    {
        print_err("<%s:%d> *********** Exit child %u ***********\n", __func__, __LINE__, numProc);
        close_logs();
        exit(1);
    }
    
    nProc = numProc;
    RM = ReqMan;
    //------------------------------------------------------------------
    printf("[%u] +++++ num threads=%u, pid=%u, uid=%u, gid=%u end_proc=%d +++++\n", numProc,
                                ReqMan->get_num_thr(), getpid(), getuid(), getgid(), end_proc);
    //------------------------------------------------------------------
    signal(SIGUSR2, SIG_IGN);
    
    if (signal(SIGINT, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d] <%s:%d> Error signal(SIGINT): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (signal(SIGSEGV, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d] <%s:%d> Error signal(SIGSEGV): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (signal(SIGUSR1, signal_handler_child) == SIG_ERR)
    {
        print_err("[%d] <%s:%d> Error signal(SIGUSR1): %s\n", numProc, __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }
    //------------------------------------------------------------------
    if (chdir(conf->DocumentRoot.c_str()))
    {
        print_err("[%d] <%s:%d> Error chdir(%s): %s\n", numProc, __func__, __LINE__, conf->DocumentRoot.c_str(), strerror(errno));
        exit(EXIT_FAILURE);
    }
    //------------------------------------------------------------------
    thread EventHandler;
    try
    {
        EventHandler = thread(event_handler, ReqMan);
    }
    catch (...)
    {
        print_err("[%d] <%s:%d> Error create thread(send_file_): errno=%d\n", numProc, __func__, __LINE__, errno);
        exit(errno);
    }
    //------------------------------------------------------------------
    unsigned int n = 0;
    while (n < conf->MinThreads)
    {
        thread thr;
        try
        {
            thr = thread(response1, ReqMan);
        }
        catch (...)
        {
            print_err("[%d] <%s:%d> Error create thread: errno=%d\n", numProc, __func__, __LINE__, errno);
            exit(errno);
        }

        ReqMan->start_thr();
        thr.detach();
        ++n;
    }
    //------------------------------------------------------------------
    thread thrReqMan;
    try
    {
        thrReqMan = thread(thr_create_manager, numProc, ReqMan);
    }
    catch (...)
    {
        print_err("<%s:%d> Error create thread %d: errno=%d\n", __func__, 
                __LINE__, ReqMan->get_all_thr(), errno);
        exit(errno);
    }
    //------------------------------------------------------------------
    static struct pollfd fdrd[2];
    fdrd[0].fd = fd_in;
    fdrd[0].events = POLLIN;

    fdrd[1].fd = sockServer;
    fdrd[1].events = POLLIN;

    int status = CONNECT_IGN, child_status = CONNECT_IGN, num_fd = 1;

    while (1)
    {
        struct sockaddr_storage clientAddr;
        socklen_t addrSize = sizeof(struct sockaddr_storage);

        if (status == CONNECT_IGN)
        {
            num_fd = 1;

            if ((child_status == CONNECT_WAIT) && (end_proc == 0))
            {
                print_err("[%d] <%s:%d> \"We are not here. It's not us.\"\n", numProc, __func__, __LINE__);
                child_status = PROC_CLOSE;
                write(pfd[1], &child_status, sizeof(child_status));
                break;
            }
        }
        else if (status == CONNECT_WAIT)
        {
            num_fd = 2;

            if (is_maxconn())
            {
                if (end_proc == 0)
                {
                    child_status = CONNECT_WAIT;
                    if (write_to_pipe(pfd[1], &child_status, sizeof(child_status)) < 0)
                        break;
                }

                check_num_conn();

                if (end_proc == 0)
                {
                    child_status = CONNECT_IGN;
                    if (write_to_pipe(pfd[1], &child_status, sizeof(child_status)) < 0)
                        break;
                }
            }
        }

        int ret_poll = poll(fdrd, num_fd, -1);
        if (ret_poll <= 0)
        {
            int err = errno;
            print_err("[%d] <%s:%d> Error poll()=-1: %s\n", numProc, __func__, __LINE__, strerror(err));
            if (err == EINTR)
                continue;
            else
                break;
        }

        if (fdrd[0].revents == POLLIN)
        {
            if (read(fd_in, &status, sizeof(status)) <= 0)
            {
                print_err("[%d] <%s:%d> Error read(): %s\n", numProc, __func__, __LINE__, strerror(errno));
                break;
            }

            ret_poll--;
            if (status == PROC_CLOSE)
            {
                print_err("[%d] <%s:%d> status=%d\n", numProc, __func__, __LINE__, status); 
                if (end_proc == 0)
                {
                    child_status = PROC_CLOSE;
                    write(pfd[1], &child_status, sizeof(child_status));
                }
                break;
            }
        }
        
        if (ret_poll && (fdrd[1].revents == POLLIN) && status)
        {
            int clientSocket = accept(sockServer, (struct sockaddr *)&clientAddr, &addrSize);
            if (clientSocket == -1)
            {
                int err_ = errno;
                print_err("[%d] <%s:%d>  Error accept(): %s\n", numProc, __func__, __LINE__, strerror(err_));
                if ((err_ == EAGAIN) || (err_ == EINTR) || (err_ == EMFILE))
                    continue;
                break;
            }

            Connect *req;
            req = create_req();
            if (!req)
            {
                shutdown(clientSocket, SHUT_RDWR);
                close(clientSocket);
                break;
            }

            int opt = 1;
            ioctl(clientSocket, FIONBIO, &opt);

            req->numProc = numProc;
            req->numConn = ++allConn;
            req->numReq = 1;
            req->clientSocket = clientSocket;
            req->timeout = conf->Timeout;
            n = getnameinfo((struct sockaddr *)&clientAddr,
                    addrSize,
                    req->remoteAddr,
                    sizeof(req->remoteAddr),
                    req->remotePort,
                    sizeof(req->remotePort),
                    NI_NUMERICHOST | NI_NUMERICSERV);
            if (n != 0)
                print_err(req, "<%s> Error getnameinfo()=%d: %s\n", __func__, n, gai_strerror(n));

            start_conn();
            push_conn(req);
        }
    }

    pid_t pid;
    while ((pid = wait(NULL)) != -1)
        fprintf(stderr, "<%d> wait() pid: %d\n", __LINE__, pid);
    if ((numProc + 1) < conf->NumProc)
        close(pfd[1]);

    wait_close_all_conn();

    shutdown(sockServer, SHUT_RDWR);
    close(sockServer);

    n = ReqMan->get_num_thr();
    print_err("[%d] <%s:%d>  numThr=%d; allNumThr=%u; all_req=%u; open_conn=%d\n", numProc, 
                    __func__, __LINE__, n, ReqMan->get_all_thr(), all_req, num_conn);
    ReqMan->close_manager();
    close_event_handler();

    thrReqMan.join();
    EventHandler.join();

    sleep(1);
    delete ReqMan;
}
//======================================================================
Connect *create_req(void)
{
    Connect *req = new(nothrow) Connect;
    if (!req)
        print_err("<%s:%d> Error malloc(): %s\n", __func__, __LINE__, str_err(errno));
    return req;
}
//======================================================================
int write_to_pipe(int fd, int *data, int size)
{
    int ret, err;
a1: 
    ret = write(fd, data, size);
    if (ret < 0)
    {
        err = errno;
        if (err == EINTR)
            goto a1;
        print_err("<%s:%d> Error write(): %s\n", __func__, __LINE__, strerror(err));
    }
    return ret;
}