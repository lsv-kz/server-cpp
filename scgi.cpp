#include "main.h"
#include "scgi.h"

using namespace std;
//======================================================================
int send_to_client(Connect *req, String *hdrs, int scgi_sock, char *tail_ptr, int tail_len)
{
    if (send_response_headers(req, hdrs))
    {
        return -1;
    }
    //-------------------------- send entity ---------------------------
    if (tail_len > 0)
    {
        int n = write_to_client(req, tail_ptr, tail_len, conf->Timeout);
        if (n < 0)
        {
            print_err(req, "<%s:%d> Error chunk_buf.add_arr(): %d\n", __func__, __LINE__, n);
            return -1;
        }
    }
    
    while (1)
    {
        char buf[4096];
        int ret = read_timeout(scgi_sock, buf, sizeof(buf), conf->TimeoutCGI);
        if (ret < 0)
        {
            print_err(req, "<%s:%d> Error read_timeout()\n", __func__, __LINE__);
            return -1;
        }
        else if (ret == 0)
            break;
        
        ret = write_to_client(req, buf, ret, conf->Timeout);
        if (ret < 0)
        {
            print_err(req, "<%s:%d> Error write_to_client()\n", __func__, __LINE__);
            return -1;
        }
    }
    
    return 0;
}
//======================================================================
int scgi_read_headers(Connect *req, int scgi_sock)
{
    req->respStatus = RS200;

    String hdrs(256);
    if (hdrs.error())
    {
        print_err(req, "<%s:%d> Error create String object\n", __func__, __LINE__);
        return -1;
    }

    const char *err_str = "Error: Blank line not found";
    int ReadFromScript = 0;
    const int size = 256;
    char buf[size];
    char *start_ptr = buf;
    int line = 0;
    while (line < 10)
    {
        int len;
        char *end_ptr, *str;

        end_ptr = (char*)memchr(start_ptr, '\n', ReadFromScript);
        if(end_ptr == NULL)
        {
            if (ReadFromScript > 0)
            {
                for (int n = 0; n < ReadFromScript; ++n)
                    buf[n] = *(start_ptr--);
            }
            else
                ReadFromScript = 0;

            start_ptr = buf;

            int rd = size - ReadFromScript;
            if (rd <= 0)
            {
                err_str = "Error: Buffer for read is small";
                break;
            }

            int ret = read_timeout(scgi_sock, buf + ReadFromScript, rd, conf->TimeoutCGI);
            if (ret <= 0)
            {
                print_err(req, "<%s:%d> read from scgi=%d, read_len=%d\n", __func__, __LINE__, ret, rd);
                if (ret)
                    err_str = "Error: Read from scgi";
                else
                    err_str = "Error: Blank line not found";
                break;
            }

            ReadFromScript += ret;
            continue;
        }

        str = start_ptr;
        len = end_ptr - start_ptr;
        start_ptr = end_ptr + 1;
        ReadFromScript -= (len + 1);

        if (len > 0)
        {
            if (*(end_ptr - 1) == '\r') --len;
        }

        *(str + len) = '\0';

        if (len == 0)
        {
            err_str = NULL;
            break;
        }

        ++line;

        if (!memchr(str, ':', len))
        {
            err_str = "Error: Line not header";
            break;
        }

        if (!strlcmp_case(str, "Status", 6))
        {
            req->respStatus = atoi(str + 7);//  req->respStatus = strtol(str + 7, NULL, 10);
            continue;
        }

        if (!strlcmp_case(str, "Date", 4) || \
            !strlcmp_case(str, "Server", 6) || \
            !strlcmp_case(str, "Accept-Ranges", 13) || \
            !strlcmp_case(str, "Content-Length", 14) || \
            !strlcmp_case(str, "Connection", 10))
        {
            continue;
        }

        hdrs << str << "\r\n";
        if (hdrs.error())
        {
            err_str = "Error: Create header";
            break;
        }
    }

    if (err_str)
    {
        print_err(req, "<%s:%d> %s\n Read From SCGI=%d\n", __func__, __LINE__, err_str, ReadFromScript);
        return -1;
    }

    int ret = send_to_client(req, &hdrs, scgi_sock, start_ptr, ReadFromScript);
    if (ret < 0)
        return -1;
    else
        return 0;
}
//======================================================================
int scgi_send_param(Connect *req, int scgi_sock)
{
    SCGI_client Scgi(scgi_sock, conf->TimeoutCGI);
    
    if (req->reqMethod == M_POST)
        Scgi.add("CONTENT_LENGTH", req->reqHdValue[req->req_hd.iReqContentLength]);
    else
    {
        Scgi.add("CONTENT_LENGTH", "0");
    }

    Scgi.add("REQUEST_METHOD", get_str_method(req->reqMethod));
    Scgi.add("REQUEST_URI", req->uri);

    if (req->reqMethod == M_POST)
    {
        Scgi.add("QUERY_STRING", NULL);
        Scgi.add("CONTENT_TYPE", req->reqHdValue[req->req_hd.iReqContentType]);
    }
    else if (req->reqMethod == M_GET)
    {
        Scgi.add("QUERY_STRING", req->sReqParam);
        Scgi.add("CONTENT_TYPE", NULL);
    }

    char *p = strchr(req->uri, '?');
    if (p)
        Scgi.add("DOCUMENT_URI", req->uri, p - req->uri);
    else
        Scgi.add("DOCUMENT_URI", req->uri);
    
    Scgi.add("DOCUMENT_ROOT", conf->DocumentRoot.c_str());
    Scgi.add("SCGI", "1");
    
    if (Scgi.error())
    {
        fprintf(stderr, "<%s:%d> Error send_param()\n", __func__, __LINE__);
        return -1;
    }
    
    Scgi.send_headers();
    
    if (req->reqMethod == M_POST)
    {
        if (req->tail)
        {
            int ret = Scgi.scgi_send(req->tail, req->lenTail);
            if (ret < 0)
            {
                print_err(req, "<%s:%d> Error scgi_send()\n", __func__, __LINE__);
                return -1;
            }
            req->req_hd.reqContentLength -= ret;
        }
        
        while (req->req_hd.reqContentLength > 0)
        {
            int rd;
            char buf[4096];
            if (req->req_hd.reqContentLength >= (long long)sizeof(buf))
                rd = sizeof(buf);
            else
                rd = (int)req->req_hd.reqContentLength;
            int ret = read_timeout(req->clientSocket, buf, rd, conf->Timeout);
            if (ret <= 0)
            {
                print_err(req, "<%s:%d> Error read_timeout()\n", __func__, __LINE__);
                return -1;
            }
            
            req->req_hd.reqContentLength -= ret;
            ret = Scgi.scgi_send(buf, ret);
            if (ret < 0)
            {
                print_err(req, "<%s:%d> Error scgi_send()\n", __func__, __LINE__);
                return -1;
            }
        }
    }
    
    int ret = scgi_read_headers(req, scgi_sock);
    return ret;
}
//======================================================================
int get_sock_fcgi(Connect *req, const char *script);
//======================================================================
int scgi(Connect *req)
{
    int  sock_scgi;
    
    if (req->reqMethod == M_POST)
    {
        if (req->req_hd.iReqContentType < 0)
        {
            print_err(req, "<%s:%d> Content-Type \?\n", __func__, __LINE__);
            return -RS400;
        }

        if (req->req_hd.reqContentLength < 0)
        {
            print_err(req, "<%s:%d> 411 Length Required\n", __func__, __LINE__);
            return -RS411;
        }

        if (req->req_hd.reqContentLength > conf->ClientMaxBodySize)
        {
            print_err(req, "<%s:%d> 413 Request entity too large: %lld\n", __func__, __LINE__, req->req_hd.reqContentLength);
            if (req->req_hd.reqContentLength < 50000000)
            {
                if (req->tail)
                    req->req_hd.reqContentLength -= req->lenTail;
                client_to_cosmos(req, &req->req_hd.reqContentLength);
                if (req->req_hd.reqContentLength == 0)
                    return -RS413;
            }
            return -1;
        }
    }

    if (timedwait_close_cgi())
    {
        return -1;
    }

    sock_scgi = get_sock_fcgi(req, req->scriptName);
    if (sock_scgi <= 0)
    {
        print_err(req, "<%s:%d> Error connect to scgi\n", __func__, __LINE__);
        cgi_dec();
        if (sock_scgi == 0)
            return -RS400;
        else
            return -RS502;
    }
    
    scgi_send_param(req, sock_scgi);
    close(sock_scgi);

    cgi_dec();

    req->connKeepAlive = 0;
    return 0;
}
