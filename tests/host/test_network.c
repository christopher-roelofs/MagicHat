#define _GNU_SOURCE
/* Exercise the real libslirp backend against a loopback HTTP server. */
#include "host/network.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET sock;
#define SOCK_VALID(s) ((s) != INVALID_SOCKET)
static void nonblocking(sock s) { u_long on = 1; ioctlsocket(s, FIONBIO, &on); }
static void pause_ms(void) { Sleep(1); }
#define sock_close closesocket
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
typedef int sock;
#define SOCK_VALID(s) ((s) >= 0)
static void nonblocking(sock s) { fcntl(s, F_SETFL, O_NONBLOCK); }
static void pause_ms(void) { struct timespec pause={.tv_nsec=1000000}; nanosleep(&pause,NULL); }
#define sock_close close
#endif
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)
static const uint8_t mac[6] = {2,0,0,0x84,0,1};
static unsigned received;
static uint8_t last[2048];
static size_t last_len;
static struct { uint8_t bytes[1600]; size_t len; } queue[64];
static unsigned qhead, qtail;
static bool queue_tcp;
static void receive(void *opaque, const uint8_t *frame, size_t len)
{
    CHECK(len <= sizeof(last)); memcpy(last, frame, len); last_len = len; received++;
    if (queue_tcp && len >= 54 && frame[12] == 8 && frame[13] == 0 && frame[23] == 6) {
        CHECK(qtail - qhead < 64 && len <= sizeof(queue[0].bytes));
        memcpy(queue[qtail % 64].bytes, frame, len);
        queue[qtail++ % 64].len = len;
    }
}
static void be16(uint8_t *p, unsigned n) { p[0] = n >> 8; p[1] = n; }
static void be32(uint8_t *p, uint32_t n) { be16(p, n >> 16); be16(p + 2, n); }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | p[2]<<8 | p[3]; }
static unsigned sum(const uint8_t *p, size_t len)
{
    unsigned s = 0;
    for (size_t i = 0; i < len; i += 2) s += (p[i] << 8) | (i + 1 < len ? p[i+1] : 0);
    return s;
}
static unsigned checksum(unsigned s) { while (s >> 16) s = (s & 65535) + (s >> 16); return (~s) & 65535; }
static void tcp(mrc_network *n, unsigned port, uint32_t seq, uint32_t ack, unsigned flags, const char *body, unsigned mss, unsigned window)
{
    uint8_t f[1600] = {0}; size_t len = body ? strlen(body) : 0;
    unsigned thlen = (flags & 2) && mss ? 24 : 20;
    uint8_t gateway[6] = {0x52,0x55,10,0,2,2};
    memcpy(f, gateway, 6); memcpy(f+6, mac, 6); be16(f+12, 0x800);
    uint8_t *ip = f+14, *t = ip+20;
    ip[0]=0x45; be16(ip+2, 20+thlen+len); ip[8]=64; ip[9]=6;
    const uint8_t addresses[8] = {10,0,2,15,10,0,2,2}; memcpy(ip+12, addresses, 8);
    be16(ip+10, checksum(sum(ip, 20)));
    be16(t, 49152); be16(t+2, port); be32(t+4, seq); be32(t+8, ack);
    t[12]=(thlen/4)<<4; t[13]=flags; be16(t+14, window);
    if (thlen == 24) { t[20]=2; t[21]=4; be16(t+22,mss); }
    if (len) memcpy(t+thlen, body, len);
    be16(t+16, checksum(sum(ip+12, 8)+6+thlen+len+sum(t,thlen+len)));
    CHECK(mrc_network_send(n, f, 34+thlen+len));
}
/* An asynchronous host connect must retain the peer's SYN MSS. Exercise
 * a small receive window and a temporary zero window, like the guest's
 * large-download confirmation dialog. Check every byte, not just a marker. */
static void http_transfer(mrc_network *n, unsigned mss, unsigned window)
{
    sock server = socket(AF_INET,SOCK_STREAM,0); CHECK(SOCK_VALID(server));
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    CHECK(bind(server,(struct sockaddr *)&addr,sizeof(addr))==0);
    socklen_t alen=sizeof(addr); CHECK(getsockname(server,(struct sockaddr *)&addr,&alen)==0);
    CHECK(listen(server,1)==0); nonblocking(server);
    unsigned port=ntohs(addr.sin_port);
    queue_tcp=true; qhead=qtail=0;
    tcp(n,port,100,0,2,NULL,mss,window);
    const char request[]="GET / HTTP/1.0\r\n\r\n";
    char response[8192];
    int header=snprintf(response,sizeof(response),"HTTP/1.0 200 OK\r\nContent-Length: 8000\r\n\r\n");
    for (unsigned i=0;i<8000;i++) response[header+i]='A'+i%26;
    size_t length=header+8000, got=0;
    sock client=(sock)-1; uint32_t ack=0;
    bool requested=false, paused=false, reopened=false;
    size_t written=0;
    static uint64_t clock_ms;
    for(unsigned ms=1;ms<2000 && got<length;ms++) {
        mrc_network_poll(n,++clock_ms*1000000);
        if(!SOCK_VALID(client)) { client=accept(server,NULL,NULL); if(SOCK_VALID(client)) nonblocking(client); }
        while(qhead!=qtail) {
            uint8_t f[1600]; size_t flen=queue[qhead%64].len;
            memcpy(f,queue[qhead++%64].bytes,flen);
            unsigned ihl=(f[14]&15)*4; const uint8_t *t=f+14+ihl;
            if(!requested && (t[13]&0x12)==0x12) {
                ack=get32(t+4)+1;
                tcp(n,port,101,ack,0x10,NULL,mss,window);
                tcp(n,port,101,ack,0x18,request,mss,window); requested=true;
            }
            unsigned hlen=(t[12]>>4)*4, total=(f[16]<<8)|f[17];
            CHECK(total>=ihl+hlen && total+14<=flen);
            size_t bytes=total-ihl-hlen;
            if (bytes) {
                if (mss && bytes>mss) fprintf(stderr,"peer MSS %u ignored: received %zu-byte TCP payload\n",mss,bytes);
                CHECK(!mss || bytes<=mss);
                if (get32(t+4)==ack) {
                    CHECK(got+bytes<=length && !memcmp(response+got,t+hlen,bytes));
                    got+=bytes; ack+=bytes;
                }
                paused=true;
                tcp(n,port,101+sizeof(request)-1,ack,0x10,NULL,mss,reopened?window:0);
            }
        }
        if(paused && !reopened && ms>=100) {
            reopened=true;
            tcp(n,port,101+sizeof(request)-1,ack,0x10,NULL,mss,window);
        }
        if(SOCK_VALID(client) && !written) {
            char buf[512]; int len=recv(client,buf,sizeof(buf),0);
            if(len>0) {
                CHECK(len>=5 && !memcmp(buf,"GET /",5));
                int sent=send(client,response,length,0); CHECK(sent>0); written=sent;
            }
        } else if(SOCK_VALID(client) && written<length) {
            int sent=send(client,response+written,length-written,0);
            if(sent>0) written+=sent;
        }
        pause_ms();
    }
    CHECK(requested && reopened && written==length && got==length);
    tcp(n,port,101+sizeof(request)-1,ack,0x14,NULL,mss,window);
    queue_tcp=false;
    if(SOCK_VALID(client)) sock_close(client);
    sock_close(server);
}
int main(void)
{
#ifdef _WIN32
    WSADATA wsa; CHECK(WSAStartup(MAKEWORD(2,2),&wsa)==0);
#endif
    mrc_network *n = mrc_network_open(receive, NULL, NULL); CHECK(n);
    uint8_t arp[60] = {0}; memset(arp,255,6); memcpy(arp+6,mac,6); be16(arp+12,0x806);
    be16(arp+14,1); be16(arp+16,0x800); arp[18]=6; arp[19]=4; be16(arp+20,1);
    memcpy(arp+22,mac,6); const uint8_t guest[4]={10,0,2,15}, host[4]={10,0,2,2};
    memcpy(arp+28,guest,4); memcpy(arp+38,host,4);
    CHECK(mrc_network_send(n,arp,sizeof(arp)));
    CHECK(received && last[21]==2 && !memcmp(last+28,host,4));
    unsigned count = received;
    memcpy(arp+38,guest,4); CHECK(mrc_network_send(n,arp,sizeof(arp)));
    CHECK(received == count); /* never answer the guest's duplicate-address probe */
    const uint8_t remote[4] = {192,0,2,1}; memcpy(arp+38,remote,4);
    CHECK(mrc_network_send(n,arp,sizeof(arp)));
    CHECK(received == count+1 && last[21] == 2 && !memcmp(last+28,remote,4));
    arp[38] = 224; count = received;
    CHECK(mrc_network_send(n,arp,sizeof(arp))); CHECK(received == count);
    http_transfer(n,536,720);
    http_transfer(n,256,720);
    http_transfer(n,1200,2400);
    http_transfer(n,0,4096); /* no MSS option retains the normal default */
    mrc_network_close(n);
    puts("libslirp ARP, MSS negotiation and small-window HTTP transfers passed"); return 0;
}
