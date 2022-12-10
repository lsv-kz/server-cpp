#include "main.h"

using namespace std;

static mutex mtx_conn;
static condition_variable cond_close_conn;
static int count_conn = 0, all_req = 0;
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
int get_num_conn()
{
lock_guard<std::mutex> lg(mtx_conn);
    return count_conn;
}
//======================================================================
int wait_close_conn()
{
unique_lock<mutex> lk(mtx_conn);
    while (count_conn >= conf->MaxWorkConnections)
    {
        cond_close_conn.wait(lk);
    }
    return 0;
}
//======================================================================
int start_conn()
{
mtx_conn.lock();
    int ret = (++count_conn);
mtx_conn.unlock();
    return ret;
}
//======================================================================
static int fd_close_conn;
static int nProc;
//======================================================================
void end_response(Connect *req)
{
    if (req->connKeepAlive == 0 || req->err < 0)
    { // ----- Close connect -----
        if (req->err > NO_PRINT_LOG)// NO_PRINT_LOG(-1000) < err < 0
        {
            if (req->err <= -RS101) // NO_PRINT_LOG(-1000) < err <= -101
            {
                req->respStatus = -req->err;
                send_message(req, NULL, NULL);
            }
            print_log(req);
        }

        shutdown(req->clientSocket, SHUT_RDWR);
        close(req->clientSocket);
        delete req;
    mtx_conn.lock();
        --count_conn;
    mtx_conn.unlock();
        char ch = nProc;
        if (write(fd_close_conn, &ch, 1) <= 0)
        {
            print_err("<%s:%d> Error write(): %s\n", __func__, __LINE__, strerror(errno));
            exit(1);
        }
        cond_close_conn.notify_all();
    }
    else
    { // ----- KeepAlive -----
    #ifdef TCP_CORK_
        if (conf->TcpCork == 'y')
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
    //print_err("[%d] <%s:%d> *** Exit thread_req_manager() ***\n", numProc, __func__, __LINE__);
}
//======================================================================
int servSock, uxSock;
RequestManager *RM;
unsigned long allConn = 0;
//======================================================================
static void signal_handler_child(int sig)
{
    if (sig == SIGINT)
    {
        fprintf(stderr, "[%d]<%s:%d> ### SIGINT ### all req: %d\n", nProc, __func__, __LINE__, all_req);
    }
    else if (sig == SIGSEGV)
    {
        fprintf(stderr, "[%d]<%s:%d> ### SIGSEGV ###\n", nProc, __func__, __LINE__);
        shutdown(uxSock, SHUT_RDWR);
        close(uxSock);
        exit(1);
    }
    else
        fprintf(stderr, "[%d]<%s:%d> ### SIG=%d ###\n", nProc, __func__, __LINE__, sig);
}
//======================================================================
Connect *create_req();
void set_max_conn(int n);
int set_max_fd(int max_open_fd);
//======================================================================
void manager(int sockServer, int numProc, int unixSock, int to_parent)
{
    uxSock = unixSock;

    RequestManager *ReqMan = new(nothrow) RequestManager(numProc);
    if (!ReqMan)
    {
        print_err("<%s:%d> *********** Exit child %d ***********\n", __func__, __LINE__, numProc);
        close_logs();
        exit(1);
    }

    fd_close_conn = to_parent;
    nProc = numProc;
    servSock = sockServer;
    RM = ReqMan;
    //------------------------------------------------------------------
    if (signal(SIGINT, signal_handler_child) == SIG_ERR)
    {
        print_err("<%s:%d> Error signal(SIGINT): %s\n", __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (signal(SIGSEGV, signal_handler_child) == SIG_ERR)
    {
        print_err("<%s:%d> Error signal(SIGSEGV): %s\n", __func__, __LINE__, strerror(errno));
        exit(EXIT_FAILURE);
    }
    //------------------------------------------------------------------
    if (chdir(conf->DocumentRoot.c_str()))
    {
        print_err("[%d] <%s:%d> Error chdir(%s): %s\n", numProc, __func__, __LINE__, conf->DocumentRoot.c_str(), strerror(errno));
        exit(1);
    }
    //----------------------------------------
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
    printf("[%d] +++++ num threads=%d, pid=%d, uid=%d, gid=%d  +++++\n", numProc,
                            ReqMan->get_num_thr(), getpid(), getuid(), getgid());

    while (1)
    {
        struct sockaddr_storage clientAddr;
        socklen_t addrSize = sizeof(struct sockaddr_storage);
        char data[1] = "";
        int sz = sizeof(data);

        int clientSocket = recv_fd(unixSock, numProc, data, (int*)&sz);
        if (clientSocket < 0)
        {
            print_err("[%d]<%s:%d> Error recv_fd()\n", numProc, __func__, __LINE__);
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
        req->remoteAddr[0] = '\0';
        getpeername(clientSocket,(struct sockaddr *)&clientAddr, &addrSize);
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
        push_pollin_list(req);// --- First request ---
    }

    print_err("<%d> <%s:%d>  numThr=%d; allNumThr=%u; all_req=%u; open_conn=%d\n", numProc,
                    __func__, __LINE__, ReqMan->get_num_thr(), ReqMan->get_all_thr(), all_req, get_num_conn());

    ReqMan->close_manager();
    thrReqMan.join();

    close_event_handler();
    EventHandler.join();

    usleep(100000);
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
